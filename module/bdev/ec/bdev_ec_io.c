/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "bdev_ec.h"
#include "bdev_ec_internal.h"
#include "wear_leveling_ext.h"
#include "spdk/bdev_module.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/queue.h"
#include <string.h>

/* Forward declaration for RMW read complete callback */
static void ec_rmw_read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

/* Forward declaration for decode read complete callback */
static void ec_decode_read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

/* Forward declaration for write functions */
static int ec_submit_write_stripe(struct ec_bdev_io *ec_io, uint64_t stripe_index,
				  uint8_t *data_indices, uint8_t *parity_indices);
static int ec_submit_write_partial_stripe(struct ec_bdev_io *ec_io, uint64_t stripe_index,
					  uint64_t start_strip, uint32_t offset_in_strip,
					  uint8_t *data_indices, uint8_t *parity_indices);

/* Forward declaration for decode read function */
static int ec_submit_decode_read(struct ec_bdev_io *ec_io, uint64_t stripe_index,
				 uint32_t strip_idx_in_stripe, uint32_t offset_in_strip);

/*
 * Generic buffer pool get function - extracts common logic
 * Returns buffer from pool or allocates new one
 */
static unsigned char *
ec_get_buf_from_pool(struct ec_parity_buf_entry **pool_head,
		     uint32_t *pool_size, uint32_t *pool_count,
		     uint32_t expected_size, uint32_t requested_size,
		     struct ec_bdev *ec_bdev, struct ec_bdev_io_channel *ec_ch,
		     const char *pool_name)
{
	struct ec_parity_buf_entry *entry;
	unsigned char *buf;
	size_t align;

	if (pool_head == NULL) {
		return NULL;
	}

	/* Optimized: Use cached alignment from io_channel to avoid repeated lookups
	 * This reduces memory access overhead in hot path
	 */
	if (spdk_likely(ec_ch != NULL && ec_ch->cached_alignment > 0)) {
		align = ec_ch->cached_alignment;
	} else if (ec_bdev != NULL && ec_bdev->buf_alignment > 0) {
		align = ec_bdev->buf_alignment;
		/* Cache the alignment value for future use */
		if (ec_ch != NULL) {
			ec_ch->cached_alignment = align;
		}
	} else {
		align = EC_BDEV_DEFAULT_BUF_ALIGNMENT;
		/* Cache the default alignment value */
		if (ec_ch != NULL) {
			ec_ch->cached_alignment = align;
		}
	}

	/* Fast path - check pool first */
	entry = SLIST_FIRST((SLIST_HEAD(, ec_parity_buf_entry) *)pool_head);
	if (spdk_likely(entry != NULL && *pool_size == requested_size)) {
		SLIST_REMOVE_HEAD((SLIST_HEAD(, ec_parity_buf_entry) *)pool_head, link);
		if (spdk_likely(*pool_count > 0)) {
			(*pool_count)--;
		} else {
			SPDK_WARNLOG("%s pool count is 0 but pool is not empty\n", pool_name);
		}
		buf = entry->buf;
		free(entry);
		return buf;
	} else if (entry != NULL) {
		/* Size mismatch - clean up */
		SPDK_WARNLOG("Buffer size mismatch in %s pool: expected %u, requested %u\n",
			     pool_name, *pool_size, requested_size);
		SLIST_REMOVE_HEAD((SLIST_HEAD(, ec_parity_buf_entry) *)pool_head, link);
		if (*pool_count > 0) {
			(*pool_count)--;
		} else {
			SPDK_WARNLOG("%s pool count is 0 but pool is not empty (size mismatch)\n", pool_name);
		}
		spdk_dma_free(entry->buf);
		free(entry);
	}

	/* Allocate new buffer */
	buf = spdk_dma_malloc(requested_size, align, NULL);
	return buf;
}

/*
 * Generic buffer pool put function - extracts common logic
 * Returns buffer to pool or frees it
 */
static void
ec_put_buf_to_pool(struct ec_parity_buf_entry **pool_head,
		   uint32_t *pool_size, uint32_t *pool_count,
		   uint32_t buf_size, uint32_t max_pool_size,
		   unsigned char *buf, const char *pool_name)
{
	struct ec_parity_buf_entry *entry;

	if (buf == NULL || pool_head == NULL) {
		return;
	}

	/* Only pool buffers of the correct size, limit pool size */
	if (*pool_size == buf_size && *pool_count < max_pool_size) {
		entry = malloc(sizeof(*entry));
		if (entry != NULL) {
			entry->buf = buf;
			SLIST_INSERT_HEAD((SLIST_HEAD(, ec_parity_buf_entry) *)pool_head, entry, link);
			(*pool_count)++;
			return;
		}
	}

	/* Pool full or allocation failed - free the buffer */
	spdk_dma_free(buf);
}

/*
 * Get parity buffer from pool or allocate new one
 */
static unsigned char *
ec_get_parity_buf(struct ec_bdev_io_channel *ec_ch, struct ec_bdev *ec_bdev, uint32_t buf_size)
{
	if (ec_ch == NULL) {
		return NULL;
	}

	return ec_get_buf_from_pool((struct ec_parity_buf_entry **)&ec_ch->parity_buf_pool,
				    &ec_ch->parity_buf_size,
				    &ec_ch->parity_buf_count,
				    ec_ch->parity_buf_size,
				    buf_size,
				    ec_bdev,
				    ec_ch,
				    "parity");
}

/*
 * Return parity buffer to pool or free it
 */
static void
ec_put_parity_buf(struct ec_bdev_io_channel *ec_ch, unsigned char *buf, uint32_t buf_size)
{
	if (ec_ch == NULL) {
		return;
	}

	ec_put_buf_to_pool((struct ec_parity_buf_entry **)&ec_ch->parity_buf_pool,
			   &ec_ch->parity_buf_size,
			   &ec_ch->parity_buf_count,
			   buf_size,
			   EC_BDEV_PARITY_BUF_POOL_MAX,
			   buf,
			   "parity");
}

/*
 * Get RMW stripe buffer from pool or allocate new one
 */
unsigned char *
ec_get_rmw_stripe_buf(struct ec_bdev_io_channel *ec_ch, struct ec_bdev *ec_bdev, uint32_t buf_size)
{
	if (ec_ch == NULL) {
		return NULL;
	}

	return ec_get_buf_from_pool((struct ec_parity_buf_entry **)&ec_ch->rmw_stripe_buf_pool,
				    &ec_ch->rmw_buf_size,
				    &ec_ch->rmw_buf_count,
				    ec_ch->rmw_buf_size,
				    buf_size,
				    ec_bdev,
				    ec_ch,
				    "RMW stripe");
}

/*
 * Return RMW stripe buffer to pool or free it
 */
void
ec_put_rmw_stripe_buf(struct ec_bdev_io_channel *ec_ch, unsigned char *buf, uint32_t buf_size)
{
	if (ec_ch == NULL) {
		return;
	}

	ec_put_buf_to_pool((struct ec_parity_buf_entry **)&ec_ch->rmw_stripe_buf_pool,
			   &ec_ch->rmw_buf_size,
			   &ec_ch->rmw_buf_count,
			   buf_size,
			   EC_BDEV_RMW_BUF_POOL_MAX,
			   buf,
			   "RMW stripe");
}

/*
 * Get temporary data buffer from pool or allocate new one
 * Optimized: Reuse buffers for cross-iov data to reduce allocation overhead
 */
static unsigned char *
ec_get_temp_data_buf(struct ec_bdev_io_channel *ec_ch, struct ec_bdev *ec_bdev, uint32_t buf_size)
{
	if (ec_ch == NULL) {
		return NULL;
	}

	return ec_get_buf_from_pool((struct ec_parity_buf_entry **)&ec_ch->temp_data_buf_pool,
				    &ec_ch->temp_data_buf_size,
				    &ec_ch->temp_data_buf_count,
				    ec_ch->temp_data_buf_size,
				    buf_size,
				    ec_bdev,
				    ec_ch,
				    "temp_data");
}

/*
 * Return temporary data buffer to pool or free it
 */
static void
ec_put_temp_data_buf(struct ec_bdev_io_channel *ec_ch, unsigned char *buf, uint32_t buf_size)
{
	if (ec_ch == NULL) {
		return;
	}

	ec_put_buf_to_pool((struct ec_parity_buf_entry **)&ec_ch->temp_data_buf_pool,
			   &ec_ch->temp_data_buf_size,
			   &ec_ch->temp_data_buf_count,
			   buf_size,
			   EC_BDEV_TEMP_DATA_BUF_POOL_MAX,
			   buf,
			   "temp_data");
}

/*
 * Clean up RMW buffers using buffer pool
 */
static void
ec_cleanup_rmw_bufs(struct ec_bdev_io *ec_io, struct ec_rmw_private *rmw)
{
	struct ec_bdev *ec_bdev;
	uint32_t strip_size_bytes;
	uint8_t i;

	if (rmw == NULL || ec_io == NULL || ec_io->ec_ch == NULL) {
		return;
	}

	ec_bdev = ec_io->ec_bdev;
	if (ec_bdev == NULL) {
		return;
	}

	strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;

	if (rmw->stripe_buf != NULL) {
		ec_put_rmw_stripe_buf(ec_io->ec_ch, rmw->stripe_buf, 
				      strip_size_bytes * ec_bdev->k);
	}

	for (i = 0; i < ec_bdev->p; i++) {
		if (rmw->parity_bufs[i] != NULL) {
			ec_put_parity_buf(ec_io->ec_ch, rmw->parity_bufs[i], strip_size_bytes);
		}
	}
}

/*
 * Unified RMW error cleanup and completion
 * Optimized: Reduces code duplication across error paths
 */
static void
ec_rmw_error_cleanup(struct ec_bdev_io *ec_io, struct ec_rmw_private *rmw,
		     unsigned char *old_data_snapshot)
{
	if (old_data_snapshot != NULL) {
		spdk_dma_free(old_data_snapshot);
	}
	if (rmw != NULL) {
		ec_cleanup_rmw_bufs(ec_io, rmw);
		free(rmw);
	}
	if (ec_io != NULL) {
		ec_io->module_private = NULL;
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/*
 * Clean up stripe private structure and parity buffers
 * Optimized helper to reduce code duplication
 */
static void
ec_cleanup_stripe_private(struct ec_bdev_io *ec_io, uint32_t strip_size_bytes)
{
	if (ec_io->module_private == NULL) {
		return;
	}

	struct ec_stripe_private *sp = ec_io->module_private;
	if (sp != NULL && ec_io->ec_ch != NULL) {
		/* Optimized: Return temporary data buffers to pool if allocated */
		if (sp->num_temp_bufs > 0) {
			for (uint8_t j = 0; j < sp->num_temp_bufs; j++) {
				if (sp->temp_data_bufs[j] != NULL) {
					ec_put_temp_data_buf(ec_io->ec_ch, sp->temp_data_bufs[j], strip_size_bytes);
				}
			}
		}
		/* Return parity buffers to pool */
		for (uint8_t j = 0; j < sp->num_parity; j++) {
			ec_put_parity_buf(ec_io->ec_ch, sp->parity_bufs[j], strip_size_bytes);
		}
		free(sp);
		ec_io->module_private = NULL;
	}
}

/*
 * Base bdev selection - default implementation
 * This function is declared in bdev_ec_internal.h
 * 
 * For EC with k data blocks and p parity blocks across n = k+p disks,
 * parity blocks are distributed in a round-robin fashion across all disks.
 * For stripe_index i, the parity blocks start at position:
 *   parity_start = (n - (i % n)) % n
 * This ensures even distribution of parity across all disks.
 */
int
ec_select_base_bdevs_default(struct ec_bdev *ec_bdev, uint64_t stripe_index,
			     uint8_t *data_indices, uint8_t *parity_indices)
{
	struct ec_base_bdev_info *base_info;
	uint8_t n = ec_bdev->num_base_bdevs;  /* Total number of disks: k + p */
	uint8_t parity_start;  /* Starting position for parity blocks in this stripe */
	uint8_t i;
	uint8_t data_idx = 0;
	uint8_t parity_idx = 0;
	uint8_t operational_count = 0;
	bool test_mode = wear_leveling_ext_is_test_mode();

	/* Calculate parity start position using round-robin (similar to RAID5)
	 * For stripe_index i, parity blocks start at position (n - (i % n)) % n
	 * and occupy the next p consecutive positions (wrapping around if needed)
	 * Optimized: calculate once before loop
	 */
	parity_start = (n - (stripe_index % n)) % n;

	/* Select data and parity indices based on round-robin distribution
	 * Optimize: use bitmap for parity position lookup (O(1) instead of O(p))
	 * For p <= EC_BDEV_BITMAP_SIZE_LIMIT, use uint64_t bitmap; otherwise fall back to array lookup
	 */
	uint64_t parity_bitmap = 0;
	uint8_t parity_positions[EC_MAX_P];
	
	/* Pre-calculate parity positions and build bitmap if p <= limit */
	for (uint8_t p_idx = 0; p_idx < ec_bdev->p; p_idx++) {
		uint8_t pos = (parity_start + p_idx) % n;
		parity_positions[p_idx] = pos;
		if (ec_bdev->p <= EC_BDEV_BITMAP_SIZE_LIMIT && pos < EC_BDEV_BITMAP_SIZE_LIMIT) {
			parity_bitmap |= (1ULL << pos);
		}
	}

	/* Optimized: Combine counting and selection in a single pass */
	i = 0;
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (!test_mode && (base_info->desc == NULL || base_info->is_failed)) {
			i++;
			continue;
		}

		/* Count operational bdevs while selecting */
		operational_count++;

		/* Check if this position is a parity position - optimized lookup */
		bool is_parity_pos = false;
		if (ec_bdev->p <= EC_BDEV_BITMAP_SIZE_LIMIT && i < EC_BDEV_BITMAP_SIZE_LIMIT) {
			/* Fast bitmap lookup for small p */
			is_parity_pos = (parity_bitmap & (1ULL << i)) != 0;
		} else {
			/* Fall back to array lookup for large p */
			for (uint8_t p_idx = 0; p_idx < ec_bdev->p; p_idx++) {
				if (i == parity_positions[p_idx]) {
					is_parity_pos = true;
					break;
				}
			}
		}

		if (is_parity_pos && parity_idx < ec_bdev->p) {
			parity_indices[parity_idx++] = i;
		} else if (!is_parity_pos && data_idx < ec_bdev->k) {
			data_indices[data_idx++] = i;
		}
		
		/* Early exit optimization: if we have enough indices, stop iterating */
		if (parity_idx == ec_bdev->p && data_idx == ec_bdev->k) {
			break;
		}
		i++;
	}

	/* Check if we have enough operational bdevs and indices */
	if (operational_count < ec_bdev->k || data_idx < ec_bdev->k || parity_idx < ec_bdev->p) {
		return -ENODEV;
	}

	return 0;
}

/*
 * Map base bdev indices to logical fragment indices for a given stripe
 * 
 * For round-robin parity distribution, we need to map base_bdev indices
 * to logical fragment indices (0 to m-1) where:
 * - 0 to k-1 are data fragments
 * - k to m-1 are parity fragments
 * 
 * This mapping is based on the data_indices and parity_indices arrays
 * returned by ec_select_base_bdevs_default.
 * 
 * params:
 * ec_bdev - pointer to EC bdev
 * stripe_index - stripe index
 * base_bdev_indices - array of failed base bdev indices
 * frag_indices - output array of logical fragment indices (0 to m-1)
 * num_failed - number of failed base bdevs
 * returns:
 * 0 on success, non-zero on failure
 */
/*
 * Full stripe write - optimal path
 */
static int
ec_submit_write_stripe(struct ec_bdev_io *ec_io, uint64_t stripe_index,
		       uint8_t *data_indices, uint8_t *parity_indices)
{
	struct ec_bdev *ec_bdev = ec_io->ec_bdev;
	struct ec_base_bdev_info *base_info;
	unsigned char *data_ptrs[EC_MAX_K];
	unsigned char *parity_ptrs[EC_MAX_P];
	uint32_t strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
	/* Optimized: Cache k and p in local variables to reduce memory access */
	uint8_t k = ec_bdev->k;
	uint8_t p = ec_bdev->p;
	uint8_t i, idx;
	int rc;

	/* Get parity buffers from pool or allocate new ones */
	for (i = 0; i < p; i++) {
		parity_ptrs[i] = ec_get_parity_buf(ec_io->ec_ch, ec_bdev, strip_size_bytes);
		if (parity_ptrs[i] == NULL) {
			SPDK_ERRLOG("Failed to allocate parity buffer %u\n", i);
			for (idx = 0; idx < i; idx++) {
				ec_put_parity_buf(ec_io->ec_ch, parity_ptrs[idx], strip_size_bytes);
			}
			return -ENOMEM;
		}
	}

	/* Prepare data pointers from iovs - optimized for performance
	 * Key optimizations:
	 * 1. Single pass through iovs (no repeated traversal)
	 * 2. Support for cross-iov data by copying to temporary buffers
	 * 3. Early validation to avoid unnecessary work
	 */
	size_t raid_io_offset = 0;
	size_t raid_io_iov_offset = 0;
	int raid_io_iov_idx = 0;
	unsigned char *temp_data_bufs[EC_MAX_K] = {NULL};  /* Temporary buffers for cross-iov data */
	bool need_temp_bufs = false;

	/* Optimized: First pass - quick check if any data blocks span multiple iovs
	 * Early exit optimization: if we find one that spans, we can stop checking
	 * This avoids unnecessary traversal for the common case where data is contiguous
	 */
	for (i = 0; i < k && !need_temp_bufs; i++) {
		size_t target_offset = i * strip_size_bytes;
		size_t current_offset = raid_io_offset;
		int current_iov_idx = raid_io_iov_idx;
		size_t current_iov_offset = raid_io_iov_offset;

		/* Advance to target offset - optimized loop */
		while (current_offset < target_offset && current_iov_idx < ec_io->iovcnt) {
			size_t bytes_in_iov = ec_io->iovs[current_iov_idx].iov_len - current_iov_offset;
			if (spdk_unlikely(bytes_in_iov == 0)) {
				current_iov_idx++;
				current_iov_offset = 0;
				continue;
			}
			size_t bytes_to_skip = target_offset - current_offset;
			if (bytes_to_skip < bytes_in_iov) {
				current_iov_offset += bytes_to_skip;
				current_offset = target_offset;
				break;
			} else {
				current_offset += bytes_in_iov;
				current_iov_idx++;
				current_iov_offset = 0;
			}
		}

		if (spdk_unlikely(current_iov_idx >= ec_io->iovcnt)) {
			SPDK_ERRLOG("Not enough data in iovs for stripe write\n");
			for (idx = 0; idx < p; idx++) {
				ec_put_parity_buf(ec_io->ec_ch, parity_ptrs[idx], strip_size_bytes);
			}
			return -EINVAL;
		}

		/* Check if data spans multiple iovs - early exit if found */
		size_t remaining = ec_io->iovs[current_iov_idx].iov_len - current_iov_offset;
		if (remaining < strip_size_bytes) {
			need_temp_bufs = true;
			/* Found one that spans - no need to check others */
			break;
		}
	}

	/* Second pass: Prepare data pointers or copy to temp buffers */
	raid_io_offset = 0;
	raid_io_iov_offset = 0;
	raid_io_iov_idx = 0;

	if (need_temp_bufs) {
		/* Optimized: Get temporary buffers from pool or allocate new ones */
		for (i = 0; i < k; i++) {
			temp_data_bufs[i] = ec_get_temp_data_buf(ec_io->ec_ch, ec_bdev, strip_size_bytes);
			if (temp_data_bufs[i] == NULL) {
				SPDK_ERRLOG("Failed to allocate temp buffer for data block %u\n", i);
				/* Clean up already allocated buffers */
				for (idx = 0; idx < i; idx++) {
					if (temp_data_bufs[idx] != NULL) {
						ec_put_temp_data_buf(ec_io->ec_ch, temp_data_bufs[idx], strip_size_bytes);
					}
				}
				for (idx = 0; idx < p; idx++) {
					ec_put_parity_buf(ec_io->ec_ch, parity_ptrs[idx], strip_size_bytes);
				}
				return -ENOMEM;
			}
		}
	}

	for (i = 0; i < k; i++) {
		size_t target_offset = i * strip_size_bytes;

		/* Advance to target offset */
		while (raid_io_offset < target_offset && raid_io_iov_idx < ec_io->iovcnt) {
			size_t bytes_in_this_iov = ec_io->iovs[raid_io_iov_idx].iov_len - raid_io_iov_offset;
			
			if (bytes_in_this_iov == 0) {
				raid_io_iov_idx++;
				raid_io_iov_offset = 0;
				continue;
			}

			size_t bytes_to_skip = target_offset - raid_io_offset;
			if (bytes_to_skip < bytes_in_this_iov) {
				raid_io_iov_offset += bytes_to_skip;
				raid_io_offset = target_offset;
				break;
			} else {
				raid_io_offset += bytes_in_this_iov;
				raid_io_iov_idx++;
				raid_io_iov_offset = 0;
			}
		}

		if (raid_io_iov_idx >= ec_io->iovcnt) {
			SPDK_ERRLOG("Not enough data in iovs for stripe write\n");
			for (idx = 0; idx < k; idx++) {
				if (temp_data_bufs[idx] != NULL) {
					spdk_dma_free(temp_data_bufs[idx]);
				}
			}
			for (idx = 0; idx < p; idx++) {
				ec_put_parity_buf(ec_io->ec_ch, parity_ptrs[idx], strip_size_bytes);
			}
			return -EINVAL;
		}

		if (ec_io->iovs[raid_io_iov_idx].iov_base == NULL) {
			SPDK_ERRLOG("iov_base is NULL for iov index %d\n", raid_io_iov_idx);
			for (idx = 0; idx < k; idx++) {
				if (temp_data_bufs[idx] != NULL) {
					spdk_dma_free(temp_data_bufs[idx]);
				}
			}
			for (idx = 0; idx < p; idx++) {
				ec_put_parity_buf(ec_io->ec_ch, parity_ptrs[idx], strip_size_bytes);
			}
			return -EINVAL;
		}

		size_t remaining_in_iov = ec_io->iovs[raid_io_iov_idx].iov_len - raid_io_iov_offset;
		
		if (remaining_in_iov >= strip_size_bytes) {
			/* Data is contiguous in single iov - use directly */
			data_ptrs[i] = (unsigned char *)ec_io->iovs[raid_io_iov_idx].iov_base + raid_io_iov_offset;
			raid_io_iov_offset += strip_size_bytes;
			raid_io_offset += strip_size_bytes;
			if (raid_io_iov_offset >= ec_io->iovs[raid_io_iov_idx].iov_len) {
				raid_io_iov_idx++;
				raid_io_iov_offset = 0;
			}
		} else {
			/* Data spans multiple iovs - copy to temp buffer */
			data_ptrs[i] = temp_data_bufs[i];
			size_t bytes_copied = 0;
			int copy_iov_idx = raid_io_iov_idx;
			size_t copy_iov_offset = raid_io_iov_offset;

			while (bytes_copied < strip_size_bytes && copy_iov_idx < ec_io->iovcnt) {
				size_t available = ec_io->iovs[copy_iov_idx].iov_len - copy_iov_offset;
				size_t needed = strip_size_bytes - bytes_copied;
				size_t to_copy = spdk_min(available, needed);

				/* Optimized: Use efficient copy for aligned data */
				if (to_copy >= 64 &&
				    ((uintptr_t)(temp_data_bufs[i] + bytes_copied) % 8 == 0) &&
				    ((uintptr_t)((unsigned char *)ec_io->iovs[copy_iov_idx].iov_base + copy_iov_offset) % 8 == 0)) {
					/* Use 64-bit copy for aligned large blocks */
					uint64_t *dst64 = (uint64_t *)(temp_data_bufs[i] + bytes_copied);
					uint64_t *src64 = (uint64_t *)((unsigned char *)ec_io->iovs[copy_iov_idx].iov_base + copy_iov_offset);
					size_t len64 = to_copy / 8;
					size_t j;
					for (j = 0; j < len64; j++) {
						dst64[j] = src64[j];
					}
					/* Handle remaining bytes */
					if (to_copy % 8 != 0) {
						memcpy(temp_data_bufs[i] + bytes_copied + len64 * 8,
						       (unsigned char *)ec_io->iovs[copy_iov_idx].iov_base + copy_iov_offset + len64 * 8,
						       to_copy % 8);
					}
				} else {
					/* Fall back to standard memcpy */
					memcpy(temp_data_bufs[i] + bytes_copied,
					       (unsigned char *)ec_io->iovs[copy_iov_idx].iov_base + copy_iov_offset,
					       to_copy);
				}
				bytes_copied += to_copy;
				copy_iov_offset += to_copy;

				if (copy_iov_offset >= ec_io->iovs[copy_iov_idx].iov_len) {
					copy_iov_idx++;
					copy_iov_offset = 0;
				}
			}

			if (bytes_copied < strip_size_bytes) {
				SPDK_ERRLOG("Not enough data to copy for data block %u\n", i);
				for (idx = 0; idx < k; idx++) {
					if (temp_data_bufs[idx] != NULL) {
						spdk_dma_free(temp_data_bufs[idx]);
					}
				}
				for (idx = 0; idx < p; idx++) {
					ec_put_parity_buf(ec_io->ec_ch, parity_ptrs[idx], strip_size_bytes);
				}
				return -EINVAL;
			}

			/* Update position for next iteration */
			raid_io_iov_idx = copy_iov_idx;
			raid_io_iov_offset = copy_iov_offset;
			raid_io_offset += strip_size_bytes;
		}
	}

	/* Encode stripe using ISA-L */
	rc = ec_encode_stripe(ec_bdev, data_ptrs, parity_ptrs, strip_size_bytes);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to encode stripe: %s\n", spdk_strerror(-rc));
		/* Clean up temp buffers if allocated */
		if (need_temp_bufs) {
			for (idx = 0; idx < k; idx++) {
				if (temp_data_bufs[idx] != NULL) {
					spdk_dma_free(temp_data_bufs[idx]);
				}
			}
		}
		for (i = 0; i < p; i++) {
			ec_put_parity_buf(ec_io->ec_ch, parity_ptrs[i], strip_size_bytes);
		}
		return rc;
	}

	/* Store buffers in module_private for cleanup */
	/* Optimized: Use malloc instead of calloc - we initialize all fields explicitly */
	struct ec_stripe_private *stripe_priv = malloc(sizeof(*stripe_priv));
	if (stripe_priv == NULL) {
		/* Clean up temp buffers if allocated */
		if (need_temp_bufs) {
			for (idx = 0; idx < k; idx++) {
				if (temp_data_bufs[idx] != NULL) {
					spdk_dma_free(temp_data_bufs[idx]);
				}
			}
		}
		for (i = 0; i < p; i++) {
			ec_put_parity_buf(ec_io->ec_ch, parity_ptrs[i], strip_size_bytes);
		}
		return -ENOMEM;
	}
	stripe_priv->type = EC_PRIVATE_TYPE_FULL_STRIPE;
	stripe_priv->num_parity = p;
	stripe_priv->num_temp_bufs = need_temp_bufs ? k : 0;
	/* Store stripe_index and indices for snapshot storage */
	stripe_priv->stripe_index = stripe_index;
	memcpy(stripe_priv->data_indices, data_indices, k * sizeof(uint8_t));
	memcpy(stripe_priv->parity_indices, parity_indices, p * sizeof(uint8_t));
	/* Store parity buffers */
	for (i = 0; i < p; i++) {
		stripe_priv->parity_bufs[i] = parity_ptrs[i];
	}
	/* Store temp data buffers if allocated */
	if (need_temp_bufs) {
		for (i = 0; i < k; i++) {
			stripe_priv->temp_data_bufs[i] = temp_data_bufs[i];
		}
	} else {
		/* Initialize to NULL for safety */
		for (i = 0; i < k; i++) {
			stripe_priv->temp_data_bufs[i] = NULL;
		}
	}
	ec_io->module_private = stripe_priv;

	/* Write data and parity blocks in parallel
	 * Optimized: Single loop with early validation to reduce overhead
	 */
	ec_io->base_bdev_io_remaining = k + p;
	ec_io->base_bdev_io_submitted = 0;

	/* Optimized: Pre-calculate pd_strip_base and strip_size once */
	uint64_t pd_strip_base = stripe_index << ec_bdev->strip_size_shift;
	uint32_t strip_size = ec_bdev->strip_size;

	/* Optimized: Validate all base bdevs before submitting any I/O
	 * This avoids partial failures and allows early exit
	 * Combined validation loop for better cache locality
	 */
	for (i = 0; i < k + p; i++) {
		if (i < k) {
			idx = data_indices[i];
		} else {
			idx = parity_indices[i - k];
		}
		/* Defensive check: ensure idx is within bounds */
		if (spdk_unlikely(idx >= ec_bdev->num_base_bdevs)) {
			SPDK_ERRLOG("Invalid %s base bdev index %u (max %u)\n",
				    i < k ? "data" : "parity", idx, ec_bdev->num_base_bdevs);
			ec_cleanup_stripe_private(ec_io, strip_size_bytes);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return -EINVAL;
		}
		base_info = &ec_bdev->base_bdev_info[idx];
		if (spdk_unlikely(base_info->desc == NULL || base_info->is_failed ||
				  ec_io->ec_ch->base_channel[idx] == NULL)) {
			SPDK_ERRLOG("%s base bdev %u is not available\n",
				    i < k ? "Data" : "Parity", idx);
			ec_cleanup_stripe_private(ec_io, strip_size_bytes);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return -ENODEV;
		}
	}

	/* Optimized: Submit writes in separate loops to reduce branch prediction overhead
	 * Separating data and parity writes eliminates conditional checks in hot path
	 * Performance benefit: reduces branch mispredictions, improves instruction cache locality
	 * Note: For small k/p values (typically <=8), the slight reduction in interleaving
	 * has negligible impact on parallelism, while branch reduction provides measurable benefit
	 */
	
	/* Submit all data block writes first */
	for (i = 0; i < k; i++) {
		idx = data_indices[i];
		/* Defensive check: ensure idx is within bounds */
		if (spdk_unlikely(idx >= ec_bdev->num_base_bdevs)) {
			SPDK_ERRLOG("Invalid data base bdev index %u (max %u)\n",
				    idx, ec_bdev->num_base_bdevs);
			ec_cleanup_stripe_private(ec_io, strip_size_bytes);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return -EINVAL;
		}
		base_info = &ec_bdev->base_bdev_info[idx];
		uint64_t pd_lba = pd_strip_base + base_info->data_offset;
		
		rc = spdk_bdev_write_blocks(base_info->desc,
					    ec_io->ec_ch->base_channel[idx],
					    data_ptrs[i], pd_lba, strip_size,
					    ec_base_bdev_io_complete, ec_io);
		if (rc == 0) {
			/* Optimized: Store base_bdev_idx for O(1) lookup in completion callback */
			if (spdk_likely(ec_io->base_bdev_io_submitted < (EC_MAX_K + EC_MAX_P))) {
				ec_io->base_bdev_idx_map[ec_io->base_bdev_io_submitted] = idx;
			}
			ec_io->base_bdev_io_submitted++;
		} else if (rc == -ENOMEM) {
			/* Queue wait to retry when resources become available */
			ec_bdev_queue_io_wait(ec_io,
					      spdk_bdev_desc_get_bdev(base_info->desc),
					      ec_io->ec_ch->base_channel[idx],
					      (spdk_bdev_io_wait_cb)ec_submit_rw_request);
			return 0;
		} else {
			SPDK_ERRLOG("Failed to write data block %u: %s\n",
				    idx, spdk_strerror(-rc));
			ec_cleanup_stripe_private(ec_io, strip_size_bytes);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return rc;
		}
	}
	
	/* Submit all parity block writes */
	for (i = 0; i < p; i++) {
		idx = parity_indices[i];
		/* Defensive check: ensure idx is within bounds */
		if (spdk_unlikely(idx >= ec_bdev->num_base_bdevs)) {
			SPDK_ERRLOG("Invalid parity base bdev index %u (max %u)\n",
				    idx, ec_bdev->num_base_bdevs);
			ec_cleanup_stripe_private(ec_io, strip_size_bytes);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return -EINVAL;
		}
		base_info = &ec_bdev->base_bdev_info[idx];
		uint64_t pd_lba = pd_strip_base + base_info->data_offset;
		
		rc = spdk_bdev_write_blocks(base_info->desc,
					    ec_io->ec_ch->base_channel[idx],
					    parity_ptrs[i], pd_lba, strip_size,
					    ec_base_bdev_io_complete, ec_io);
		if (rc == 0) {
			/* Optimized: Store base_bdev_idx for O(1) lookup in completion callback */
			if (spdk_likely(ec_io->base_bdev_io_submitted < (EC_MAX_K + EC_MAX_P))) {
				ec_io->base_bdev_idx_map[ec_io->base_bdev_io_submitted] = idx;
			}
			ec_io->base_bdev_io_submitted++;
		} else if (rc == -ENOMEM) {
			/* Queue wait to retry when resources become available */
			ec_bdev_queue_io_wait(ec_io,
					      spdk_bdev_desc_get_bdev(base_info->desc),
					      ec_io->ec_ch->base_channel[idx],
					      (spdk_bdev_io_wait_cb)ec_submit_rw_request);
			return 0;
		} else {
			SPDK_ERRLOG("Failed to write parity block %u: %s\n",
				    idx, spdk_strerror(-rc));
			ec_cleanup_stripe_private(ec_io, strip_size_bytes);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return rc;
		}
	}

	return 0;
}

/*
 * RMW read complete callback
 */
static void
ec_rmw_read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ec_bdev_io *ec_io = cb_arg;
	struct ec_rmw_private *rmw;
	struct ec_bdev *ec_bdev;
	struct spdk_bdev *failed_bdev = NULL;
	uint8_t i;
	int rc;

	/* Save bdev pointer before freeing I/O */
	if (bdev_io != NULL) {
		failed_bdev = bdev_io->bdev;
	}

	spdk_bdev_free_io(bdev_io);

	if (ec_io == NULL || ec_io->module_private == NULL) {
		SPDK_ERRLOG("Invalid ec_io or module_private in RMW read complete\n");
		return;
	}

	rmw = ec_io->module_private;
	ec_bdev = ec_io->ec_bdev;

	if (rmw->type != EC_PRIVATE_TYPE_RMW) {
		SPDK_ERRLOG("Invalid RMW context type\n");
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (!success) {
		SPDK_ERRLOG("RMW read failed for EC bdev %s\n", ec_bdev->bdev.name);
		/* Find the base_info for this failed read and mark it as failed */
		if (failed_bdev != NULL) {
			struct ec_base_bdev_info *failed_base_info = NULL;
			EC_FOR_EACH_BASE_BDEV(ec_bdev, failed_base_info) {
				if (failed_base_info->desc != NULL &&
				    spdk_bdev_desc_get_bdev(failed_base_info->desc) == failed_bdev) {
					if (!failed_base_info->is_failed) {
						SPDK_WARNLOG("RMW read failed on base bdev '%s' (slot %u) of EC bdev '%s', marking as failed\n",
							     failed_base_info->name ? failed_base_info->name : "unknown",
							     (uint8_t)(failed_base_info - ec_bdev->base_bdev_info),
							     ec_bdev->bdev.name);
						ec_bdev_fail_base_bdev(failed_base_info);
					}
					break;
				}
			}
		}
		ec_rmw_error_cleanup(ec_io, rmw, NULL);
		return;
	}

	rmw->reads_completed++;
	
	/* Check if we're in a failed state.
	 * When a read fails with -ENOMEM or other error after some reads have been
	 * submitted, we set reads_expected = reads_completed to mark failure.
	 * When all pending reads complete, reads_completed will equal reads_expected,
	 * but reads_expected will be less than k (the expected number of reads).
	 */
	if (rmw->reads_completed >= rmw->reads_expected) {
		/* Optimized: Check if we have enough data blocks (k) for encoding
		 * Note: We need at least k data blocks, but we also read p parity blocks
		 */
		if (rmw->reads_expected < ec_bdev->k + ec_bdev->p) {
			/* We're in a failed state - clean up and complete with error */
			ec_cleanup_rmw_bufs(ec_io, rmw);
			free(rmw);
			ec_io->module_private = NULL;
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
		/* All reads completed successfully - proceed with incremental encoding */
	} else {
		/* Still waiting for more reads to complete */
		return;
	}

	/* All reads completed - merge new data and encode */
	rmw->state = EC_RMW_STATE_ENCODING;

	uint32_t strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
	uint32_t block_size = ec_bdev->bdev.blocklen;
	uint32_t offset_bytes = rmw->offset_in_strip * block_size;
	uint32_t num_bytes_to_write = rmw->num_blocks_to_write * block_size;
	
	/* Verify strip_idx_in_stripe is valid */
	if (rmw->strip_idx_in_stripe >= ec_bdev->k) {
		SPDK_ERRLOG("Invalid strip_idx_in_stripe %u (k=%u)\n",
			    rmw->strip_idx_in_stripe, ec_bdev->k);
		ec_rmw_error_cleanup(ec_io, rmw, NULL);
		return;
	}
	
	/* Verify offset is within strip bounds */
	if (offset_bytes + num_bytes_to_write > strip_size_bytes) {
		SPDK_ERRLOG("Write exceeds strip boundary: offset %u + size %u > strip_size %u\n",
			    offset_bytes, num_bytes_to_write, strip_size_bytes);
		ec_rmw_error_cleanup(ec_io, rmw, NULL);
		return;
	}
	
	unsigned char *target_stripe_data = rmw->data_ptrs[rmw->strip_idx_in_stripe] + offset_bytes;
	
	/* Optimized: Save old data before merge for incremental parity update
	 * This enables using ec_encode_stripe_update instead of full re-encoding
	 * Performance: O(k*p*len) -> O(p*len) for encoding
	 * 
	 * Note: ec_encode_stripe_update requires the entire strip (not just the modified portion)
	 * because parity calculation depends on the entire strip
	 */
	unsigned char *old_data_snapshot = NULL;
	unsigned char *full_strip_old_data = rmw->data_ptrs[rmw->strip_idx_in_stripe];
	
	/* Allocate temporary buffer for old data snapshot if incremental update is possible
	 * We always need the full strip for incremental update
	 */
	if (num_bytes_to_write >= strip_size_bytes / 4) {
		/* Significant write (>25%): worth incremental update
		 * Save entire strip's old data before merge
		 */
		old_data_snapshot = spdk_dma_malloc(strip_size_bytes, EC_ISAL_OPTIMAL_ALIGN, NULL);
		if (old_data_snapshot != NULL) {
			/* Save entire strip's old data before merge */
			memcpy(old_data_snapshot, full_strip_old_data, strip_size_bytes);
		}
	}
	/* For very small writes (<25% of strip), full re-encode may be faster due to overhead */

	/* Copy new data from iovs to stripe buffer */
	size_t bytes_copied = 0;
	int iov_idx = 0;
	size_t iov_offset = 0;

	for (iov_idx = 0; iov_idx < ec_io->iovcnt && bytes_copied < num_bytes_to_write; iov_idx++) {
		/* Safety check: ensure iov_base is not NULL */
		if (ec_io->iovs[iov_idx].iov_base == NULL) {
			SPDK_ERRLOG("iov_base is NULL for iov index %d in RMW\n", iov_idx);
			ec_rmw_error_cleanup(ec_io, rmw, old_data_snapshot);
			return;
		}
		
		size_t remaining_in_iov = ec_io->iovs[iov_idx].iov_len - iov_offset;
		size_t remaining_to_copy = num_bytes_to_write - bytes_copied;
		size_t to_copy = spdk_min(remaining_in_iov, remaining_to_copy);

		/* Safety check: verify iov_offset is within bounds */
		if (iov_offset >= ec_io->iovs[iov_idx].iov_len) {
			SPDK_ERRLOG("iov_offset %zu exceeds iov_len %zu for iov index %d\n",
				    iov_offset, ec_io->iovs[iov_idx].iov_len, iov_idx);
			ec_rmw_error_cleanup(ec_io, rmw, old_data_snapshot);
			return;
		}

		/* Optimized: Use efficient memcpy for aligned data
		 * For large copies, memcpy will use SIMD instructions automatically
		 * For small copies, inline copy may be faster
		 */
		if (to_copy >= 64 && 
		    ((uintptr_t)(target_stripe_data + bytes_copied) % 8 == 0) &&
		    ((uintptr_t)((unsigned char *)ec_io->iovs[iov_idx].iov_base + iov_offset) % 8 == 0)) {
			/* Use 64-bit copy for aligned large blocks */
			uint64_t *dst64 = (uint64_t *)(target_stripe_data + bytes_copied);
			uint64_t *src64 = (uint64_t *)((unsigned char *)ec_io->iovs[iov_idx].iov_base + iov_offset);
			size_t len64 = to_copy / 8;
			size_t j;
			for (j = 0; j < len64; j++) {
				dst64[j] = src64[j];
			}
			/* Handle remaining bytes */
			if (to_copy % 8 != 0) {
				memcpy(target_stripe_data + bytes_copied + len64 * 8,
				       (unsigned char *)ec_io->iovs[iov_idx].iov_base + iov_offset + len64 * 8,
				       to_copy % 8);
			}
		} else {
			/* Fall back to standard memcpy for unaligned or small data */
			memcpy(target_stripe_data + bytes_copied,
			       (unsigned char *)ec_io->iovs[iov_idx].iov_base + iov_offset,
			       to_copy);
		}
		bytes_copied += to_copy;
		iov_offset += to_copy;

		if (iov_offset >= ec_io->iovs[iov_idx].iov_len) {
			iov_offset = 0;
		}
	}

	if (bytes_copied < num_bytes_to_write) {
		SPDK_ERRLOG("Not enough data in iovs for RMW write\n");
		ec_rmw_error_cleanup(ec_io, rmw, old_data_snapshot);
		return;
	}

	/* Optimized: Use incremental parity update instead of full re-encoding
	 * This is a fundamental optimization that reduces encoding complexity from O(k*p*len) to O(p*len)
	 * Key benefits:
	 * 1. Only process the changed data block, not all k blocks
	 * 2. Update parity blocks incrementally using XOR delta
	 * 3. Significant performance improvement for large k values (e.g., k=8: ~8x faster encoding)
	 * 
	 * Note: We need old_data (before merge) and new_data (after merge) to compute delta
	 * old_data_snapshot contains the entire strip's old data
	 * full_strip_old_data (now modified) contains the entire strip's new data
	 */
	unsigned char *full_strip_new_data = rmw->data_ptrs[rmw->strip_idx_in_stripe];
	
	/* Try incremental update first if old_data_snapshot is available */
	if (old_data_snapshot != NULL) {
		rc = ec_encode_stripe_update(ec_bdev, rmw->strip_idx_in_stripe,
					     old_data_snapshot, full_strip_new_data,
					     rmw->parity_bufs, strip_size_bytes);
		spdk_dma_free(old_data_snapshot);
		old_data_snapshot = NULL; /* Mark as freed to avoid double-free */
		
		if (rc != 0) {
			SPDK_WARNLOG("Failed to incrementally update parity in RMW: %s, falling back to full re-encode\n",
				     spdk_strerror(-rc));
			/* Fall through to full re-encode */
		} else {
			/* Incremental update successful - significant performance improvement! */
			goto encode_success;
		}
	}
	
	/* Fallback: Full re-encode (for small writes or when incremental update failed) */
	rc = ec_encode_stripe(ec_bdev, rmw->data_ptrs, rmw->parity_bufs, strip_size_bytes);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to encode stripe in RMW: %s\n", spdk_strerror(-rc));
		ec_rmw_error_cleanup(ec_io, rmw, old_data_snapshot);
		return;
	}

encode_success:

	/* Write blocks - standard path: write all k data blocks + p parity blocks */
	rmw->state = EC_RMW_STATE_WRITING;
	/* Cache k and p for RMW write path */
	uint8_t k = ec_bdev->k;
	uint8_t p = ec_bdev->p;
	
	ec_io->base_bdev_io_remaining = k + p;
	ec_io->base_bdev_io_submitted = 0;

	struct ec_base_bdev_info *base_info;
	uint8_t idx;

	/* Pre-calculate pd_strip_base to avoid repeated calculation */
	uint64_t pd_strip_base = rmw->stripe_index << ec_bdev->strip_size_shift;

	/* Optimized: Validate all base bdevs before submitting writes
	 * Separate validation loops for data and parity to reduce branch overhead
	 */
	for (i = 0; i < k; i++) {
		idx = rmw->data_indices[i];
		base_info = &ec_bdev->base_bdev_info[idx];
		if (spdk_unlikely(base_info->desc == NULL || base_info->is_failed ||
				  ec_io->ec_ch->base_channel[idx] == NULL)) {
			SPDK_ERRLOG("Data base bdev %u is not available for RMW write\n", idx);
			ec_rmw_error_cleanup(ec_io, rmw, NULL);
			return;
		}
	}
	for (i = 0; i < p; i++) {
		idx = rmw->parity_indices[i];
		base_info = &ec_bdev->base_bdev_info[idx];
		if (spdk_unlikely(base_info->desc == NULL || base_info->is_failed ||
				  ec_io->ec_ch->base_channel[idx] == NULL)) {
			SPDK_ERRLOG("Parity base bdev %u is not available for RMW write\n", idx);
			ec_rmw_error_cleanup(ec_io, rmw, NULL);
			return;
		}
	}

	/* Optimized: Submit writes in separate loops to reduce branch prediction overhead
	 * Separating data and parity writes eliminates conditional checks in hot path
	 * Performance benefit: reduces branch mispredictions, improves instruction cache locality
	 */
	uint32_t strip_size = ec_bdev->strip_size;
	
	/* Submit all data block writes first */
	for (i = 0; i < k; i++) {
		idx = rmw->data_indices[i];
		/* Defensive check: ensure idx is within bounds */
		if (spdk_unlikely(idx >= ec_bdev->num_base_bdevs)) {
			SPDK_ERRLOG("Invalid data base bdev index %u (max %u) in RMW write\n",
				    idx, ec_bdev->num_base_bdevs);
			ec_rmw_error_cleanup(ec_io, rmw, NULL);
			return;
		}
		base_info = &ec_bdev->base_bdev_info[idx];
		uint64_t pd_lba = pd_strip_base + base_info->data_offset;

		rc = spdk_bdev_write_blocks(base_info->desc,
					    ec_io->ec_ch->base_channel[idx],
					    rmw->data_ptrs[i], pd_lba, strip_size,
					    ec_base_bdev_io_complete, ec_io);
		if (rc == 0) {
			/* Optimized: Store base_bdev_idx for O(1) lookup in completion callback */
			if (spdk_likely(ec_io->base_bdev_io_submitted < (EC_MAX_K + EC_MAX_P))) {
				ec_io->base_bdev_idx_map[ec_io->base_bdev_io_submitted] = idx;
			}
			ec_io->base_bdev_io_submitted++;
		} else if (rc == -ENOMEM) {
			ec_bdev_queue_io_wait(ec_io,
					      spdk_bdev_desc_get_bdev(base_info->desc),
					      ec_io->ec_ch->base_channel[idx],
					      (spdk_bdev_io_wait_cb)ec_submit_rw_request);
			return;
		} else {
			SPDK_ERRLOG("Failed to write data block %u in RMW: %s\n",
				    idx, spdk_strerror(-rc));
			ec_rmw_error_cleanup(ec_io, rmw, NULL);
			return;
		}
	}

	/* Submit all parity block writes */
	for (i = 0; i < p; i++) {
		idx = rmw->parity_indices[i];
		/* Defensive check: ensure idx is within bounds */
		if (spdk_unlikely(idx >= ec_bdev->num_base_bdevs)) {
			SPDK_ERRLOG("Invalid parity base bdev index %u (max %u) in RMW write\n",
				    idx, ec_bdev->num_base_bdevs);
			ec_rmw_error_cleanup(ec_io, rmw, NULL);
			return;
		}
		base_info = &ec_bdev->base_bdev_info[idx];
		uint64_t pd_lba = pd_strip_base + base_info->data_offset;

		rc = spdk_bdev_write_blocks(base_info->desc,
					    ec_io->ec_ch->base_channel[idx],
					    rmw->parity_bufs[i], pd_lba, strip_size,
					    ec_base_bdev_io_complete, ec_io);
		if (rc == 0) {
			/* Optimized: Store base_bdev_idx for O(1) lookup in completion callback */
			if (spdk_likely(ec_io->base_bdev_io_submitted < (EC_MAX_K + EC_MAX_P))) {
				ec_io->base_bdev_idx_map[ec_io->base_bdev_io_submitted] = idx;
			}
			ec_io->base_bdev_io_submitted++;
		} else if (rc == -ENOMEM) {
			ec_bdev_queue_io_wait(ec_io,
					      spdk_bdev_desc_get_bdev(base_info->desc),
					      ec_io->ec_ch->base_channel[idx],
					      (spdk_bdev_io_wait_cb)ec_submit_rw_request);
			return;
		} else {
			SPDK_ERRLOG("Failed to write parity block %u in RMW: %s\n",
				    idx, spdk_strerror(-rc));
			ec_rmw_error_cleanup(ec_io, rmw, NULL);
			return;
		}
	}
}

/*
 * Partial stripe write - RMW path
 */
static int
ec_submit_write_partial_stripe(struct ec_bdev_io *ec_io, uint64_t stripe_index,
			       uint64_t start_strip, uint32_t offset_in_strip,
			       uint8_t *data_indices, uint8_t *parity_indices)
{
	struct ec_bdev *ec_bdev = ec_io->ec_bdev;
	/* Optimized: Cache frequently accessed values */
	uint32_t strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
	uint8_t k = ec_bdev->k;
	uint8_t p = ec_bdev->p;
	struct ec_rmw_private *rmw;
	struct ec_base_bdev_info *base_info;
	uint32_t strip_idx_in_stripe = start_strip % k;
	uint8_t i;
	uint8_t idx;
	int rc;

	/* Allocate RMW context */
	/* Optimized: Use malloc instead of calloc - we initialize all fields explicitly */
	rmw = malloc(sizeof(*rmw));
	if (rmw == NULL) {
		SPDK_ERRLOG("Failed to allocate RMW context\n");
		return -ENOMEM;
	}
	rmw->type = EC_PRIVATE_TYPE_RMW;
	/* Optimized: Only initialize fields that will be used before assignment */
	rmw->stripe_buf = NULL;
	/* Note: parity_bufs and data_ptrs will be set below, no need to zero-initialize */

	/* Get full stripe buffer from pool or allocate new one */
	rmw->stripe_buf = ec_get_rmw_stripe_buf(ec_io->ec_ch, ec_bdev, strip_size_bytes * k);
	if (rmw->stripe_buf == NULL) {
		SPDK_ERRLOG("Failed to allocate stripe buffer for RMW\n");
		free(rmw);
		return -ENOMEM;
	}

	/* Get parity buffers from pool or allocate new ones */
	for (i = 0; i < p; i++) {
		rmw->parity_bufs[i] = ec_get_parity_buf(ec_io->ec_ch, ec_bdev, strip_size_bytes);
		if (rmw->parity_bufs[i] == NULL) {
			SPDK_ERRLOG("Failed to allocate parity buffer %u for RMW\n", i);
			for (uint8_t j = 0; j < i; j++) {
				ec_put_parity_buf(ec_io->ec_ch, rmw->parity_bufs[j], strip_size_bytes);
			}
			ec_put_rmw_stripe_buf(ec_io->ec_ch, rmw->stripe_buf, strip_size_bytes * k);
			free(rmw);
			return -ENOMEM;
		}
	}

	/* Prepare data pointers */
	for (i = 0; i < k; i++) {
		rmw->data_ptrs[i] = rmw->stripe_buf + i * strip_size_bytes;
	}

	/* Store stripe information */
	rmw->stripe_index = stripe_index;
	rmw->strip_idx_in_stripe = strip_idx_in_stripe;
	rmw->offset_in_strip = offset_in_strip;
	rmw->num_blocks_to_write = ec_io->num_blocks;
	/* Optimized: For small arrays, direct assignment may be faster than memcpy */
	/* Compiler can optimize small loops better than memcpy for tiny arrays */
	for (i = 0; i < k; i++) {
		rmw->data_indices[i] = data_indices[i];
	}
	for (i = 0; i < p; i++) {
		rmw->parity_indices[i] = parity_indices[i];
	}

	rmw->state = EC_RMW_STATE_READING;
	rmw->reads_completed = 0;
	/* Optimized: Read both data blocks (k) and parity blocks (p) for incremental update
	 * This allows us to use ec_encode_stripe_update instead of full re-encoding
	 * Performance improvement: O(k*p*len) -> O(p*len) for encoding
	 */
	rmw->reads_expected = k + p;
	
	ec_io->module_private = rmw;

	/* Optimized: Read all k data blocks and p parity blocks in parallel
	 * This enables incremental parity update instead of full re-encoding
	 */
	for (i = 0; i < k; i++) {
		idx = data_indices[i];
		base_info = &ec_bdev->base_bdev_info[idx];
		if (spdk_unlikely(idx >= ec_bdev->num_base_bdevs)) {
			SPDK_ERRLOG("Invalid data base bdev index %u (max %u) in RMW read\n",
				    idx, ec_bdev->num_base_bdevs);
			ec_rmw_error_cleanup(ec_io, rmw, NULL);
			return -EINVAL;
		}
		base_info = &ec_bdev->base_bdev_info[idx];
		if (base_info->desc == NULL || base_info->is_failed ||
		    ec_io->ec_ch->base_channel[idx] == NULL) {
			SPDK_ERRLOG("Data base bdev %u is not available for RMW read\n", idx);
			ec_rmw_error_cleanup(ec_io, rmw, NULL);
			return -ENODEV;
		}

		/* Optimized: Pre-calculate strip_size_shift once */
		uint64_t pd_lba = (stripe_index << ec_bdev->strip_size_shift) + base_info->data_offset;

		rc = spdk_bdev_read_blocks(base_info->desc,
					    ec_io->ec_ch->base_channel[idx],
					    rmw->data_ptrs[i], pd_lba, ec_bdev->strip_size,
					    ec_rmw_read_complete, ec_io);
		if (rc == 0) {
			/* Read submitted successfully */
		} else if (rc == -ENOMEM) {
			/* If we've already submitted some reads (i > 0), we can't safely
			 * free resources or queue wait because:
			 * 1. Those reads will complete later and need the rmw context
			 * 2. If we queue wait and retry, we'll reallocate resources,
			 *    causing a leak of the old resources
			 * 
			 * The safest approach is to mark the operation as failed and
			 * let the pending reads complete and clean up.
			 */
			if (i > 0) {
				/* Some reads already submitted - mark as failed so
				 * completed reads will clean up resources
				 */
				rmw->reads_expected = rmw->reads_completed;
				/* Don't queue wait - let pending reads complete and fail */
				return 0;
			} else {
				/* No reads submitted yet - safe to free and queue wait */
				ec_cleanup_rmw_bufs(ec_io, rmw);
				free(rmw);
				ec_io->module_private = NULL;
				
				ec_bdev_queue_io_wait(ec_io,
						      spdk_bdev_desc_get_bdev(base_info->desc),
						      ec_io->ec_ch->base_channel[idx],
						      (spdk_bdev_io_wait_cb)ec_submit_rw_request);
				return 0;
			}
		} else {
			SPDK_ERRLOG("Failed to read data block %u for RMW: %s\n",
				    idx, spdk_strerror(-rc));
			/* On error, mark as failed so pending reads will clean up */
			if (i > 0) {
				rmw->reads_expected = rmw->reads_completed;
			} else {
				/* No reads submitted - safe to free immediately */
				ec_rmw_error_cleanup(ec_io, rmw, NULL);
			}
			if (i == 0) {
				return rc;
			}
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return rc;
		}
	}
	
	/* CRITICAL FIX: Also read parity blocks for incremental update
	 * Without this, reads_expected = k + p but we only read k blocks,
	 * causing the callback to wait forever for p missing reads!
	 */
	for (i = 0; i < p; i++) {
		idx = parity_indices[i];
		if (spdk_unlikely(idx >= ec_bdev->num_base_bdevs)) {
			SPDK_ERRLOG("Invalid parity base bdev index %u (max %u) in RMW read\n",
				    idx, ec_bdev->num_base_bdevs);
			ec_rmw_error_cleanup(ec_io, rmw, NULL);
			return -EINVAL;
		}
		base_info = &ec_bdev->base_bdev_info[idx];
		if (base_info->desc == NULL || base_info->is_failed ||
		    ec_io->ec_ch->base_channel[idx] == NULL) {
			SPDK_ERRLOG("Parity base bdev %u is not available for RMW read\n", idx);
			ec_rmw_error_cleanup(ec_io, rmw, NULL);
			return -ENODEV;
		}

		/* Optimized: Pre-calculate strip_size_shift once */
		uint64_t pd_lba = (stripe_index << ec_bdev->strip_size_shift) + base_info->data_offset;

		rc = spdk_bdev_read_blocks(base_info->desc,
					    ec_io->ec_ch->base_channel[idx],
					    rmw->parity_bufs[i], pd_lba, ec_bdev->strip_size,
					    ec_rmw_read_complete, ec_io);
		if (rc == 0) {
			/* Read submitted successfully */
		} else if (rc == -ENOMEM) {
			/* If we've already submitted some reads (k data + some parity), we can't safely
			 * free resources or queue wait because:
			 * 1. Those reads will complete later and need the rmw context
			 * 2. If we queue wait and retry, we'll reallocate resources,
			 *    causing a leak of the old resources
			 * 
			 * The safest approach is to mark the operation as failed and
			 * let the pending reads complete and clean up.
			 */
			if (i > 0 || k > 0) {
				/* Some reads already submitted - mark as failed so
				 * completed reads will clean up resources
				 */
				rmw->reads_expected = rmw->reads_completed;
				/* Don't queue wait - let pending reads complete and fail */
				return 0;
			} else {
				/* No reads submitted yet - safe to free and queue wait */
				ec_cleanup_rmw_bufs(ec_io, rmw);
				free(rmw);
				ec_io->module_private = NULL;
				
				ec_bdev_queue_io_wait(ec_io,
						      spdk_bdev_desc_get_bdev(base_info->desc),
						      ec_io->ec_ch->base_channel[idx],
						      (spdk_bdev_io_wait_cb)ec_submit_rw_request);
				return 0;
			}
		} else {
			SPDK_ERRLOG("Failed to read parity block %u for RMW: %s\n",
				    idx, spdk_strerror(-rc));
			/* On error, mark as failed so pending reads will clean up */
			if (i > 0 || k > 0) {
				rmw->reads_expected = rmw->reads_completed;
			} else {
				/* No reads submitted - safe to free immediately */
				ec_rmw_error_cleanup(ec_io, rmw, NULL);
			}
			if (i == 0 && k == 0) {
				return rc;
			}
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return rc;
		}
	}

	return 0;
}

/*
 * Clean up decode buffers
 */
static void
ec_cleanup_decode_bufs(struct ec_bdev_io *ec_io, struct ec_decode_private *decode)
{
	struct ec_bdev *ec_bdev;
	uint32_t strip_size_bytes;

	if (decode == NULL || ec_io == NULL || ec_io->ec_ch == NULL) {
		return;
	}

	ec_bdev = ec_io->ec_bdev;
	if (ec_bdev == NULL) {
		return;
	}

	strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;

	if (decode->stripe_buf != NULL) {
		ec_put_rmw_stripe_buf(ec_io->ec_ch, decode->stripe_buf,
				      strip_size_bytes * (ec_bdev->k + ec_bdev->p));
	}

	if (decode->recover_buf != NULL) {
		spdk_dma_free(decode->recover_buf);
	}
}

/*
 * Decode read complete callback
 */
static void
ec_decode_read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ec_bdev_io *ec_io = cb_arg;
	struct ec_decode_private *decode;
	struct ec_bdev *ec_bdev;
	struct spdk_bdev *failed_bdev = NULL;
	uint8_t i;
	int rc;

	/* Save bdev pointer before freeing I/O */
	if (bdev_io != NULL) {
		failed_bdev = bdev_io->bdev;
	}

	spdk_bdev_free_io(bdev_io);

	if (ec_io == NULL || ec_io->module_private == NULL) {
		SPDK_ERRLOG("Invalid ec_io or module_private in decode read complete\n");
		return;
	}

	decode = ec_io->module_private;
	ec_bdev = ec_io->ec_bdev;

	if (decode->type != EC_PRIVATE_TYPE_DECODE) {
		SPDK_ERRLOG("Invalid decode context type\n");
		ec_cleanup_decode_bufs(ec_io, decode);
		free(decode);
		ec_io->module_private = NULL;
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (!success) {
		SPDK_ERRLOG("Decode read failed for EC bdev %s\n", ec_bdev->bdev.name);
		/* Find the base_info for this failed read and mark it as failed */
		if (failed_bdev != NULL) {
			struct ec_base_bdev_info *failed_base_info = NULL;
			EC_FOR_EACH_BASE_BDEV(ec_bdev, failed_base_info) {
				if (failed_base_info->desc != NULL &&
				    spdk_bdev_desc_get_bdev(failed_base_info->desc) == failed_bdev) {
					if (!failed_base_info->is_failed) {
						SPDK_WARNLOG("Decode read failed on base bdev '%s' (slot %u) of EC bdev '%s', marking as failed\n",
							     failed_base_info->name ? failed_base_info->name : "unknown",
							     (uint8_t)(failed_base_info - ec_bdev->base_bdev_info),
							     ec_bdev->bdev.name);
						ec_bdev_fail_base_bdev(failed_base_info);
					}
					break;
				}
			}
		}
		ec_cleanup_decode_bufs(ec_io, decode);
		free(decode);
		ec_io->module_private = NULL;
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	decode->reads_completed++;

	/* Check if all reads completed */
	if (decode->reads_completed >= decode->reads_expected) {
		/* All reads completed - proceed with decoding */
		decode->state = EC_DECODE_STATE_DECODING;

		uint32_t strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
		unsigned char *recover_ptrs[EC_MAX_P];

		/* Find which failed fragment we need to recover */
		uint8_t target_frag_idx = decode->strip_idx_in_stripe;
		bool need_recover = false;
		for (i = 0; i < decode->num_failed; i++) {
			if (decode->failed_indices[i] == target_frag_idx) {
				need_recover = true;
				recover_ptrs[0] = decode->recover_buf;
				break;
			}
		}

		if (!need_recover) {
			/* Target fragment is available, just copy from stripe buffer
			 * Find which data pointer contains the target fragment */
			uint8_t target_ptr_idx = 0xFF;
			uint8_t k = ec_bdev->k;
			for (i = 0; i < k; i++) {
				uint8_t base_idx = decode->available_indices[i];
				if (decode->frag_map[base_idx] == target_frag_idx) {
					target_ptr_idx = i;
					break;
				}
			}
			if (target_ptr_idx == 0xFF) {
				SPDK_ERRLOG("Failed to find target fragment in available blocks\n");
				ec_cleanup_decode_bufs(ec_io, decode);
				free(decode);
				ec_io->module_private = NULL;
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}

			/* Copy data from stripe buffer to iovs */
			uint32_t block_size = ec_bdev->bdev.blocklen;
			uint32_t offset_bytes = decode->offset_in_strip * block_size;
			uint32_t num_bytes = decode->num_blocks_to_read * block_size;
			unsigned char *src = decode->data_ptrs[target_ptr_idx] + offset_bytes;

			size_t bytes_copied = 0;
			int iov_idx = 0;
			size_t iov_offset = 0;

			for (iov_idx = 0; iov_idx < ec_io->iovcnt && bytes_copied < num_bytes; iov_idx++) {
				if (ec_io->iovs[iov_idx].iov_base == NULL) {
					SPDK_ERRLOG("iov_base is NULL for iov index %d\n", iov_idx);
					ec_cleanup_decode_bufs(ec_io, decode);
					free(decode);
					ec_io->module_private = NULL;
					ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
					return;
				}

				size_t remaining_in_iov = ec_io->iovs[iov_idx].iov_len - iov_offset;
				size_t remaining_to_copy = num_bytes - bytes_copied;
				size_t to_copy = spdk_min(remaining_in_iov, remaining_to_copy);

				memcpy((unsigned char *)ec_io->iovs[iov_idx].iov_base + iov_offset,
				       src + bytes_copied, to_copy);
				bytes_copied += to_copy;
				iov_offset += to_copy;

				if (iov_offset >= ec_io->iovs[iov_idx].iov_len) {
					iov_offset = 0;
				}
			}

			if (bytes_copied < num_bytes) {
				SPDK_ERRLOG("Not enough space in iovs for decode copy\n");
				ec_cleanup_decode_bufs(ec_io, decode);
				free(decode);
				ec_io->module_private = NULL;
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}

			/* Success - clean up and complete */
			ec_cleanup_decode_bufs(ec_io, decode);
			free(decode);
			ec_io->module_private = NULL;
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_SUCCESS);
			return;
		}

		/* Need to decode - prepare recovery pointers */
		recover_ptrs[0] = decode->recover_buf;

		/* Decode the failed fragment */
		rc = ec_decode_stripe(ec_bdev, decode->data_ptrs, recover_ptrs,
				      decode->failed_indices, decode->num_failed, strip_size_bytes);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to decode stripe: %s\n", spdk_strerror(-rc));
			ec_cleanup_decode_bufs(ec_io, decode);
			free(decode);
			ec_io->module_private = NULL;
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		/* Copy recovered data to iovs */
		decode->state = EC_DECODE_STATE_COPYING;
		uint32_t block_size = ec_bdev->bdev.blocklen;
		uint32_t offset_bytes = decode->offset_in_strip * block_size;
		uint32_t num_bytes = decode->num_blocks_to_read * block_size;
		unsigned char *src = decode->recover_buf + offset_bytes;

		size_t bytes_copied = 0;
		int iov_idx = 0;
		size_t iov_offset = 0;

		for (iov_idx = 0; iov_idx < ec_io->iovcnt && bytes_copied < num_bytes; iov_idx++) {
			if (ec_io->iovs[iov_idx].iov_base == NULL) {
				SPDK_ERRLOG("iov_base is NULL for iov index %d\n", iov_idx);
				ec_cleanup_decode_bufs(ec_io, decode);
				free(decode);
				ec_io->module_private = NULL;
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}

			size_t remaining_in_iov = ec_io->iovs[iov_idx].iov_len - iov_offset;
			size_t remaining_to_copy = num_bytes - bytes_copied;
			size_t to_copy = spdk_min(remaining_in_iov, remaining_to_copy);

			memcpy((unsigned char *)ec_io->iovs[iov_idx].iov_base + iov_offset,
			       src + bytes_copied, to_copy);
			bytes_copied += to_copy;
			iov_offset += to_copy;

			if (iov_offset >= ec_io->iovs[iov_idx].iov_len) {
				iov_offset = 0;
			}
		}

		if (bytes_copied < num_bytes) {
			SPDK_ERRLOG("Not enough space in iovs for decode copy\n");
			ec_cleanup_decode_bufs(ec_io, decode);
			free(decode);
			ec_io->module_private = NULL;
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		/* Success - clean up and complete */
		ec_cleanup_decode_bufs(ec_io, decode);
		free(decode);
		ec_io->module_private = NULL;
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		/* Still waiting for more reads to complete */
		return;
	}
}

/*
 * Submit decode read - read from k available blocks and decode to recover failed data
 */
static int
ec_submit_decode_read(struct ec_bdev_io *ec_io, uint64_t stripe_index,
		     uint32_t strip_idx_in_stripe, uint32_t offset_in_strip)
{
	struct ec_bdev *ec_bdev = ec_io->ec_bdev;
	uint32_t strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
	uint8_t k = ec_bdev->k;
	uint8_t p = ec_bdev->p;
	uint8_t m = k + p;
	struct ec_decode_private *decode;
	struct ec_base_bdev_info *base_info;
	uint8_t data_indices[EC_MAX_K];
	uint8_t parity_indices[EC_MAX_P];
	uint8_t failed_base_indices[EC_MAX_P] __attribute__((unused));
	uint8_t failed_frag_indices[EC_MAX_P];
	uint8_t i, idx;
	uint8_t num_failed = 0;
	uint8_t num_available = 0;
	int rc;

	/* Try to load snapshot first, fall back to default if not available */
	bool snapshot_loaded = wear_leveling_ext_load_snapshot(ec_bdev, stripe_index,
							       data_indices, parity_indices);
	if (!snapshot_loaded) {
		/* No snapshot available, use default selection */
		rc = ec_select_base_bdevs_default(ec_bdev, stripe_index, data_indices, parity_indices);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to select base bdevs for decode\n");
			return rc;
		}
	} else {
		/* Snapshot loaded successfully */
		rc = 0;
	}

	/* Allocate decode context first */
	decode = malloc(sizeof(*decode));
	if (decode == NULL) {
		SPDK_ERRLOG("Failed to allocate decode context\n");
		return -ENOMEM;
	}

	/* Find failed base bdevs and build fragment mapping */
	uint8_t frag_map[EC_MAX_K + EC_MAX_P];
	uint8_t available_indices[EC_MAX_K + EC_MAX_P];
	memset(frag_map, 0xFF, sizeof(frag_map));
	for (i = 0; i < k; i++) {
		idx = data_indices[i];
		frag_map[idx] = i;  /* Data fragment: 0 to k-1 */
		base_info = &ec_bdev->base_bdev_info[idx];
		if (base_info->desc == NULL || base_info->is_failed ||
		    ec_io->ec_ch->base_channel[idx] == NULL) {
			failed_base_indices[num_failed] = idx;
			failed_frag_indices[num_failed] = i;
			num_failed++;
		} else {
			available_indices[num_available++] = idx;
		}
	}
	for (i = 0; i < p; i++) {
		idx = parity_indices[i];
		frag_map[idx] = k + i;  /* Parity fragment: k to m-1 */
		base_info = &ec_bdev->base_bdev_info[idx];
		if (base_info->desc == NULL || base_info->is_failed ||
		    ec_io->ec_ch->base_channel[idx] == NULL) {
			failed_base_indices[num_failed] = idx;
			failed_frag_indices[num_failed] = k + i;
			num_failed++;
		} else {
			available_indices[num_available++] = idx;
		}
	}

	if (num_available < k) {
		SPDK_ERRLOG("Not enough available blocks (%u < %u) for decode\n",
			    num_available, k);
		free(decode);
		return -ENODEV;
	}

	if (num_failed == 0) {
		SPDK_ERRLOG("No failed blocks to decode\n");
		free(decode);
		return -EINVAL;
	}

	if (num_failed > p) {
		SPDK_ERRLOG("Too many failed blocks (%u > %u) for decode\n", num_failed, p);
		free(decode);
		return -ENODEV;
	}
	decode->type = EC_PRIVATE_TYPE_DECODE;
	decode->stripe_index = stripe_index;
	decode->strip_idx_in_stripe = strip_idx_in_stripe;
	decode->offset_in_strip = offset_in_strip;
	decode->num_blocks_to_read = ec_io->num_blocks;
	decode->num_failed = num_failed;
	decode->num_available = num_available;
	memcpy(decode->failed_indices, failed_frag_indices, num_failed);
	memcpy(decode->frag_map, frag_map, sizeof(frag_map));
	memcpy(decode->available_indices, available_indices, num_available);

	/* Get stripe buffer */
	decode->stripe_buf = ec_get_rmw_stripe_buf(ec_io->ec_ch, ec_bdev,
						   strip_size_bytes * m);
	if (decode->stripe_buf == NULL) {
		SPDK_ERRLOG("Failed to allocate stripe buffer for decode\n");
		free(decode);
		return -ENOMEM;
	}

	/* Allocate recovery buffer
	 * Optimized: Use cached alignment from io_channel if available
	 */
	size_t align = (ec_io->ec_ch != NULL && ec_io->ec_ch->cached_alignment > 0) ?
		ec_io->ec_ch->cached_alignment :
		((ec_bdev->buf_alignment > 0) ? ec_bdev->buf_alignment : EC_BDEV_DEFAULT_BUF_ALIGNMENT);
	decode->recover_buf = spdk_dma_malloc(strip_size_bytes, align, NULL);
	if (decode->recover_buf == NULL) {
		SPDK_ERRLOG("Failed to allocate recovery buffer\n");
		ec_put_rmw_stripe_buf(ec_io->ec_ch, decode->stripe_buf, strip_size_bytes * m);
		free(decode);
		return -ENOMEM;
	}

	/* Prepare data pointers - use first k available blocks */
	for (i = 0; i < k; i++) {
		decode->data_ptrs[i] = decode->stripe_buf + i * strip_size_bytes;
	}

	decode->state = EC_DECODE_STATE_READING;
	decode->reads_completed = 0;
	decode->reads_expected = k;
	ec_io->module_private = decode;

	/* Read k available blocks in parallel */
	for (i = 0; i < k; i++) {
		idx = decode->available_indices[i];
		base_info = &ec_bdev->base_bdev_info[idx];

		uint64_t pd_lba = (stripe_index << ec_bdev->strip_size_shift) + base_info->data_offset;

		rc = spdk_bdev_read_blocks(base_info->desc,
					   ec_io->ec_ch->base_channel[idx],
					   decode->data_ptrs[i], pd_lba, ec_bdev->strip_size,
					   ec_decode_read_complete, ec_io);
		if (rc == 0) {
			/* Read submitted successfully */
		} else if (rc == -ENOMEM) {
			if (i > 0) {
				/* Some reads already submitted - mark as failed */
				decode->reads_expected = decode->reads_completed;
				return 0;
			} else {
				/* No reads submitted yet - safe to free and queue wait */
				ec_cleanup_decode_bufs(ec_io, decode);
				free(decode);
				ec_io->module_private = NULL;
				ec_bdev_queue_io_wait(ec_io,
						      spdk_bdev_desc_get_bdev(base_info->desc),
						      ec_io->ec_ch->base_channel[idx],
						      (spdk_bdev_io_wait_cb)ec_submit_rw_request);
				return 0;
			}
		} else {
			SPDK_ERRLOG("Failed to read data block %u for decode: %s\n",
				    idx, spdk_strerror(-rc));
			if (i > 0) {
				decode->reads_expected = decode->reads_completed;
			} else {
				ec_cleanup_decode_bufs(ec_io, decode);
				free(decode);
				ec_io->module_private = NULL;
			}
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return rc;
		}
	}

	return 0;
}

/*
 * Main I/O submission function
 */
void
ec_submit_rw_request(struct ec_bdev_io *ec_io)
{
	struct ec_bdev *ec_bdev = ec_io->ec_bdev;
	struct ec_base_bdev_info *base_info;
	uint8_t i;
	uint8_t num_operational = 0;
	uint8_t num_failed = 0;
	int rc;

	/* Optimized: Count operational and failed base bdevs with early exit
	 * Cache k and p to avoid repeated memory access
	 */
	uint8_t k = ec_bdev->k;
	uint8_t p = ec_bdev->p;
	uint8_t required_operational = k + p;
	
	i = 0;
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->desc == NULL || base_info->is_failed ||
		    ec_io->ec_ch->base_channel[i] == NULL) {
			num_failed++;
		} else {
			num_operational++;
			/* Early exit: if we have enough operational bdevs, stop counting */
			if (num_operational >= required_operational) {
				break;
			}
		}
		i++;
	}

	if (num_operational < k) {
		SPDK_ERRLOG("Not enough operational blocks (%u < %u) for EC bdev %s\n",
			    num_operational, k, ec_bdev->bdev.name);
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	/* Defensive check: k should never be 0, but check to prevent division by zero */
	if (spdk_unlikely(k == 0)) {
		SPDK_ERRLOG("Invalid k value (0) for EC bdev %s\n", ec_bdev->bdev.name);
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (ec_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		uint8_t data_indices[EC_MAX_K];
		uint8_t parity_indices[EC_MAX_P];
		uint64_t start_strip;
		uint64_t end_strip;
		uint64_t stripe_index;
		uint32_t offset_in_strip;
		/* Optimized: Cache frequently accessed values */
		uint32_t strip_size = ec_bdev->strip_size;
		uint32_t strip_size_shift = ec_bdev->strip_size_shift;

		start_strip = ec_io->offset_blocks >> strip_size_shift;
		end_strip = (ec_io->offset_blocks + ec_io->num_blocks - 1) >> strip_size_shift;

		/* Optimized: Early check for strip boundary violation */
		if (spdk_unlikely(start_strip != end_strip && ec_bdev->num_base_bdevs > 1)) {
			SPDK_ERRLOG("I/O spans strip boundary for EC bdev %s\n",
				    ec_bdev->bdev.name);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		stripe_index = start_strip / k;
		offset_in_strip = ec_io->offset_blocks & (strip_size - 1);

		/* Select base bdevs using extension interface or default */
		rc = ec_select_base_bdevs_with_extension(ec_bdev,
							  ec_io->offset_blocks,
							  ec_io->num_blocks,
							  stripe_index,
							  data_indices, parity_indices);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to select base bdevs for EC bdev %s\n",
				    ec_bdev->bdev.name);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		/* Optimized: Check conditions in order of likelihood to fail early */
		/* Most restrictive check first: start_strip alignment */
		if ((start_strip % k) == 0 && offset_in_strip == 0) {
			uint64_t full_stripe_size = strip_size * k;
			if (ec_io->num_blocks == full_stripe_size) {
				rc = ec_submit_write_stripe(ec_io, stripe_index, data_indices, parity_indices);
				if (rc == 0) {
					return;
				} else if (rc == -ENOMEM) {
					/* Buffer allocation failed - queue I/O wait on first available base bdev
					 * to retry when buffers become available. This ensures we retry the
					 * full stripe write path instead of falling back to RMW. */
					struct ec_base_bdev_info *wait_base_info = NULL;
					struct spdk_io_channel *wait_channel = NULL;
					uint8_t wait_idx;
					
					/* Find first available base bdev to wait on */
					wait_idx = 0;
					EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
						if (base_info->desc != NULL && !base_info->is_failed &&
						    ec_io->ec_ch->base_channel[wait_idx] != NULL) {
							wait_base_info = base_info;
							wait_channel = ec_io->ec_ch->base_channel[wait_idx];
							break;
						}
						wait_idx++;
					}
					
					if (wait_base_info != NULL && wait_channel != NULL) {
						ec_bdev_queue_io_wait(ec_io,
								      spdk_bdev_desc_get_bdev(wait_base_info->desc),
								      wait_channel,
								      (spdk_bdev_io_wait_cb)ec_submit_rw_request);
						return;
					} else {
						/* No available base bdev - fail the I/O */
						SPDK_ERRLOG("No available base bdev for I/O wait\n");
						ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
						return;
					}
				} else {
					/* Other error - fail the I/O */
					ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
					return;
				}
			}
		}
		/* Partial stripe write (RMW path) */
		{
			rc = ec_submit_write_partial_stripe(ec_io, stripe_index, start_strip,
							    offset_in_strip, data_indices, parity_indices);
			if (rc == 0) {
				/* Success */
			} else if (rc == -ENOMEM) {
				/* Buffer allocation failed - queue I/O wait on first available base bdev
				 * to retry when buffers become available. */
				struct ec_base_bdev_info *wait_base_info = NULL;
				struct spdk_io_channel *wait_channel = NULL;
				uint8_t wait_idx;
				
				/* Find first available base bdev to wait on */
				wait_idx = 0;
				EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
					if (base_info->desc != NULL && !base_info->is_failed &&
					    ec_io->ec_ch->base_channel[wait_idx] != NULL) {
						wait_base_info = base_info;
						wait_channel = ec_io->ec_ch->base_channel[wait_idx];
						break;
					}
					wait_idx++;
				}
				
				if (wait_base_info != NULL && wait_channel != NULL) {
					ec_bdev_queue_io_wait(ec_io,
							      spdk_bdev_desc_get_bdev(wait_base_info->desc),
							      wait_channel,
							      (spdk_bdev_io_wait_cb)ec_submit_rw_request);
					return;
				} else {
					/* No available base bdev - fail the I/O */
					SPDK_ERRLOG("No available base bdev for I/O wait in RMW path\n");
					ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
					return;
				}
			} else {
				/* Other error - fail the I/O */
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		}

	} else {
		/* Read operation */
		uint64_t start_strip;
		uint64_t end_strip;
		uint64_t stripe_index;
		uint32_t offset_in_strip;
		uint8_t strip_idx_in_stripe;
		uint8_t target_data_idx;

		start_strip = ec_io->offset_blocks >> ec_bdev->strip_size_shift;
		end_strip = (ec_io->offset_blocks + ec_io->num_blocks - 1) >>
			    ec_bdev->strip_size_shift;

		if (start_strip != end_strip && ec_bdev->num_base_bdevs > 1) {
			SPDK_ERRLOG("Read I/O spans strip boundary for EC bdev %s\n",
				    ec_bdev->bdev.name);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		/* Defensive check: k should never be 0, but check to prevent division by zero */
		if (spdk_unlikely(ec_bdev->k == 0)) {
			SPDK_ERRLOG("Invalid k value (0) for EC bdev %s\n", ec_bdev->bdev.name);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		stripe_index = start_strip / ec_bdev->k;
		strip_idx_in_stripe = start_strip % ec_bdev->k;
		offset_in_strip = ec_io->offset_blocks & (ec_bdev->strip_size - 1);

		if (num_failed > 0 && num_failed <= ec_bdev->p) {
			/* Use decode path to recover from failed blocks */
			rc = ec_submit_decode_read(ec_io, stripe_index, strip_idx_in_stripe, offset_in_strip);
			if (rc == 0) {
				return;
			} else if (rc == -ENOMEM) {
				/* Buffer allocation failed - queue I/O wait */
				struct ec_base_bdev_info *wait_base_info = NULL;
				struct spdk_io_channel *wait_channel = NULL;
				uint8_t wait_idx;
				
				wait_idx = 0;
				EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
					if (base_info->desc != NULL && !base_info->is_failed &&
					    ec_io->ec_ch->base_channel[wait_idx] != NULL) {
						wait_base_info = base_info;
						wait_channel = ec_io->ec_ch->base_channel[wait_idx];
						break;
					}
					wait_idx++;
				}
				
				if (wait_base_info != NULL && wait_channel != NULL) {
					ec_bdev_queue_io_wait(ec_io,
							      spdk_bdev_desc_get_bdev(wait_base_info->desc),
							      wait_channel,
							      (spdk_bdev_io_wait_cb)ec_submit_rw_request);
					return;
				} else {
					SPDK_ERRLOG("No available base bdev for I/O wait in decode path\n");
					ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
					return;
				}
			} else {
				SPDK_ERRLOG("Failed to submit decode read: %s\n", spdk_strerror(-rc));
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		}

		uint8_t data_indices[EC_MAX_K];
		uint8_t parity_indices[EC_MAX_P];
		/* Try to load snapshot first, fall back to extension/default if not available */
		bool snapshot_loaded = wear_leveling_ext_load_snapshot(ec_bdev, stripe_index,
								       data_indices, parity_indices);
		if (!snapshot_loaded) {
			/* No snapshot available, use extension interface or default */
			rc = ec_select_base_bdevs_with_extension(ec_bdev,
								  ec_io->offset_blocks,
								  ec_io->num_blocks,
								  stripe_index,
								  data_indices, parity_indices);
		} else {
			/* Snapshot loaded successfully */
			rc = 0;
		}
		if (rc != 0) {
			SPDK_ERRLOG("Failed to select base bdevs for EC bdev %s\n",
				    ec_bdev->bdev.name);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
		target_data_idx = data_indices[strip_idx_in_stripe];

		base_info = &ec_bdev->base_bdev_info[target_data_idx];
		if (base_info->desc == NULL || base_info->is_failed ||
		    ec_io->ec_ch->base_channel[target_data_idx] == NULL) {
			SPDK_ERRLOG("Target data base bdev %u is not available\n", target_data_idx);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		uint64_t pd_strip = stripe_index;
		uint64_t pd_lba = (pd_strip << ec_bdev->strip_size_shift) + offset_in_strip + base_info->data_offset;

		ec_io->base_bdev_io_remaining = 1;
		ec_io->base_bdev_io_submitted = 0;

		/* Use readv_blocks to support multiple iovecs */
		rc = spdk_bdev_readv_blocks(base_info->desc,
					    ec_io->ec_ch->base_channel[target_data_idx],
					    ec_io->iovs, ec_io->iovcnt,
					    pd_lba, ec_io->num_blocks,
					    ec_base_bdev_io_complete, ec_io);
		if (rc == 0) {
			/* Optimized: Store base_bdev_idx for O(1) lookup in completion callback */
			if (spdk_likely(ec_io->base_bdev_io_submitted < (EC_MAX_K + EC_MAX_P))) {
				ec_io->base_bdev_idx_map[ec_io->base_bdev_io_submitted] = target_data_idx;
			}
			ec_io->base_bdev_io_submitted++;
		} else if (rc == -ENOMEM) {
			ec_bdev_queue_io_wait(ec_io,
					      spdk_bdev_desc_get_bdev(base_info->desc),
					      ec_io->ec_ch->base_channel[target_data_idx],
					      (spdk_bdev_io_wait_cb)ec_submit_rw_request);
			return;
		} else {
			SPDK_ERRLOG("Failed to read from base bdev %s: %s\n",
				    base_info->name, spdk_strerror(-rc));
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
	}
}

/*
 * Null payload request (flush/unmap)
 */
void
ec_submit_null_payload_request(struct ec_bdev_io *ec_io)
{
	struct ec_bdev *ec_bdev = ec_io->ec_bdev;
	struct ec_base_bdev_info *base_info;
	int rc;
	uint8_t i;

	ec_io->base_bdev_io_remaining = ec_bdev->num_base_bdevs_operational;
	ec_io->base_bdev_io_submitted = 0;

	i = 0;
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->desc == NULL || ec_io->ec_ch->base_channel[i] == NULL) {
			i++;
			ec_bdev_io_complete_part(ec_io, 1, SPDK_BDEV_IO_STATUS_SUCCESS);
			continue;
		}

		rc = spdk_bdev_flush_blocks(base_info->desc, ec_io->ec_ch->base_channel[i],
					    ec_io->offset_blocks, ec_io->num_blocks,
					    ec_base_bdev_io_complete, ec_io);
		if (rc == 0) {
			/* Optimized: Store base_bdev_idx for O(1) lookup in completion callback */
			if (spdk_likely(ec_io->base_bdev_io_submitted < (EC_MAX_K + EC_MAX_P))) {
				ec_io->base_bdev_idx_map[ec_io->base_bdev_io_submitted] = i;
			}
			ec_io->base_bdev_io_submitted++;
		} else if (rc == -ENOMEM) {
			ec_bdev_queue_io_wait(ec_io,
					      spdk_bdev_desc_get_bdev(base_info->desc),
					      ec_io->ec_ch->base_channel[i],
					      (spdk_bdev_io_wait_cb)ec_submit_null_payload_request);
			return;
		} else {
			SPDK_ERRLOG("bdev io submit error not due to ENOMEM: %s\n",
				    spdk_strerror(-rc));
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
		i++;
	}
}

/*
 * Reset request
 */
void
ec_submit_reset_request(struct ec_bdev_io *ec_io)
{
	struct ec_bdev *ec_bdev = ec_io->ec_bdev;
	struct ec_base_bdev_info *base_info;
	int rc;
	uint8_t i;

	if (ec_io->base_bdev_io_remaining == 0) {
		ec_io->base_bdev_io_remaining = ec_bdev->num_base_bdevs_operational;
	}
	if (ec_io->base_bdev_io_remaining > ec_bdev->num_base_bdevs) {
		SPDK_ERRLOG("Invalid remaining count %" PRIu64 " exceeds num_base_bdevs %u\n",
			    ec_io->base_bdev_io_remaining, ec_bdev->num_base_bdevs);
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	i = 0;
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (ec_io->ec_ch->base_channel[i] == NULL) {
			ec_io->base_bdev_io_submitted++;
			ec_bdev_io_complete_part(ec_io, 1, SPDK_BDEV_IO_STATUS_SUCCESS);
			i++;
			continue;
		}

		rc = spdk_bdev_reset(base_info->desc, ec_io->ec_ch->base_channel[i],
				     ec_base_bdev_reset_complete, ec_io);
		if (rc == 0) {
			ec_io->base_bdev_io_submitted++;
		} else if (rc == -ENOMEM) {
			ec_bdev_queue_io_wait(ec_io,
					      spdk_bdev_desc_get_bdev(base_info->desc),
					      ec_io->ec_ch->base_channel[i],
					      (spdk_bdev_io_wait_cb)ec_submit_reset_request);
			return;
		} else {
			SPDK_ERRLOG("bdev io submit error not due to ENOMEM: %s\n",
				    spdk_strerror(-rc));
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
		i++;
	}
}

/*
 * Base bdev I/O completion callback
 */
void
ec_base_bdev_io_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ec_bdev_io *ec_io = cb_arg;
	struct ec_bdev *ec_bdev;
	struct ec_base_bdev_info *base_info = NULL;
	bool all_complete = false;

	if (ec_io == NULL) {
		SPDK_ERRLOG("ec_io is NULL in completion callback\n");
		spdk_bdev_free_io(bdev_io);
		return;
	}

	ec_bdev = ec_io->ec_bdev;
	if (ec_bdev == NULL) {
		SPDK_ERRLOG("ec_bdev is NULL in completion callback\n");
		spdk_bdev_free_io(bdev_io);
		return;
	}

	/* Optimized: Find the base_info for this I/O using direct index lookup
	 * We iterate through submitted I/Os to find matching bdev
	 * This is still O(n) but with early exit optimization
	 */
	uint8_t base_bdev_idx = UINT8_MAX;
	uint8_t i;
	
	/* Optimized: Early exit - check submitted I/Os first (most likely to match) */
	for (i = 0; i < ec_io->base_bdev_io_submitted && i < ec_bdev->num_base_bdevs; i++) {
		uint8_t idx = ec_io->base_bdev_idx_map[i];
		if (spdk_likely(idx < ec_bdev->num_base_bdevs)) {
			struct ec_base_bdev_info *candidate = &ec_bdev->base_bdev_info[idx];
			if (candidate->desc != NULL &&
			    spdk_bdev_desc_get_bdev(candidate->desc) == bdev_io->bdev) {
				base_bdev_idx = idx;
				base_info = candidate;
				break;
			}
		}
	}
	
	/* Fallback: If not found in submitted I/Os, search all base bdevs */
	if (base_info == NULL) {
		EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
			if (base_info->desc != NULL &&
			    spdk_bdev_desc_get_bdev(base_info->desc) == bdev_io->bdev) {
				break;
			}
		}
	}

	/* If I/O failed, mark the base bdev as failed */
	if (!success && base_info != NULL && !base_info->is_failed) {
		SPDK_WARNLOG("I/O failed on base bdev '%s' (slot %u) of EC bdev '%s', marking as failed\n",
			     base_info->name ? base_info->name : "unknown",
			     (uint8_t)(base_info - ec_bdev->base_bdev_info),
			     ec_bdev->bdev.name);
		ec_bdev_fail_base_bdev(base_info);
	}

	/* Notify extension interface of I/O completion for wear level tracking */
	if (success) {
		ec_notify_extension_io_complete(ec_bdev, base_info,
						ec_io->offset_blocks,
						ec_io->num_blocks,
						ec_io->type == SPDK_BDEV_IO_TYPE_WRITE);
	}

	spdk_bdev_free_io(bdev_io);

	all_complete = ec_bdev_io_complete_part(ec_io, 1, success ?
						SPDK_BDEV_IO_STATUS_SUCCESS :
						SPDK_BDEV_IO_STATUS_FAILED);

	/* Clean up resources if this was a write and all I/O is complete */
	if (all_complete && ec_io->type == SPDK_BDEV_IO_TYPE_WRITE && ec_io->module_private != NULL) {
		struct ec_stripe_private *stripe_priv = ec_io->module_private;
		struct ec_rmw_private *rmw = NULL;
		uint8_t i;
		uint32_t strip_size_bytes;
		
		/* Store snapshot if write succeeded and wear leveling is in FULL mode
		 * (SIMPLE mode typically uses fast path/default selection, no snapshot needed)
		 */
		bool write_succeeded = (ec_io->base_bdev_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);
		if (write_succeeded) {
			int mode = wear_leveling_ext_get_mode(ec_bdev);
			/* Only store snapshot in FULL mode (SIMPLE mode uses default selection most of the time) */
			/* 检查返回值：如果返回错误（负数），说明扩展未启用或已降级，跳过快照存储 */
			if (mode == 2) {  /* WL_MODE_FULL = 2 */
				int store_rc;
				if (stripe_priv->type == EC_PRIVATE_TYPE_RMW) {
					rmw = (struct ec_rmw_private *)stripe_priv;
					/* Store snapshot for RMW write */
					store_rc = wear_leveling_ext_store_snapshot(ec_bdev, rmw->stripe_index,
										      rmw->data_indices, rmw->parity_indices);
				} else if (stripe_priv->type == EC_PRIVATE_TYPE_FULL_STRIPE) {
					/* Store snapshot for full stripe write */
					store_rc = wear_leveling_ext_store_snapshot(ec_bdev, stripe_priv->stripe_index,
										      stripe_priv->data_indices, stripe_priv->parity_indices);
				} else {
					store_rc = 0;  /* Unknown type, skip snapshot */
				}
				/* Log error if snapshot storage failed (non-critical, don't fail the write) */
				/* Note: Auto-fallback logic is handled inside wear_leveling_ext_store_snapshot */
				if (store_rc != 0 && store_rc != -ENOENT && store_rc != -ERANGE) {
					uint64_t failed_stripe_idx = (stripe_priv->type == EC_PRIVATE_TYPE_RMW && rmw != NULL) ?
						rmw->stripe_index : stripe_priv->stripe_index;
					SPDK_WARNLOG("Failed to store snapshot for stripe %lu (rc=%d), "
						     "read consistency may be affected\n",
						     failed_stripe_idx, store_rc);
				}
			}
		}

		if (ec_io->ec_ch == NULL) {
			SPDK_ERRLOG("ec_ch is NULL when cleaning up buffers\n");
			/* Still free the private structure to avoid leak */
			if (stripe_priv->type == EC_PRIVATE_TYPE_RMW) {
				rmw = (struct ec_rmw_private *)stripe_priv;
				if (rmw->stripe_buf != NULL) {
					spdk_dma_free(rmw->stripe_buf);
				}
				for (i = 0; i < ec_bdev->p; i++) {
					if (rmw->parity_bufs[i] != NULL) {
						spdk_dma_free(rmw->parity_bufs[i]);
					}
				}
				free(rmw);
			} else {
				/* Free temp data buffers if allocated */
				if (stripe_priv->num_temp_bufs > 0) {
					for (i = 0; i < stripe_priv->num_temp_bufs; i++) {
						if (stripe_priv->temp_data_bufs[i] != NULL) {
							spdk_dma_free(stripe_priv->temp_data_bufs[i]);
						}
					}
				}
				for (i = 0; i < stripe_priv->num_parity; i++) {
					if (stripe_priv->parity_bufs[i] != NULL) {
						spdk_dma_free(stripe_priv->parity_bufs[i]);
					}
				}
				free(stripe_priv);
			}
			ec_io->module_private = NULL;
			return;
		}

		strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;

		if (stripe_priv->type == EC_PRIVATE_TYPE_RMW) {
			rmw = (struct ec_rmw_private *)stripe_priv;
			if (rmw->stripe_buf != NULL) {
				ec_put_rmw_stripe_buf(ec_io->ec_ch, rmw->stripe_buf, 
						      strip_size_bytes * ec_bdev->k);
			}
			for (i = 0; i < ec_bdev->p; i++) {
				if (rmw->parity_bufs[i] != NULL) {
					ec_put_parity_buf(ec_io->ec_ch, rmw->parity_bufs[i], strip_size_bytes);
				}
			}
			free(rmw);
		} else {
			/* Free temp data buffers if allocated */
			if (stripe_priv->num_temp_bufs > 0) {
				for (i = 0; i < stripe_priv->num_temp_bufs; i++) {
					if (stripe_priv->temp_data_bufs[i] != NULL) {
						spdk_dma_free(stripe_priv->temp_data_bufs[i]);
					}
				}
			}
			for (i = 0; i < stripe_priv->num_parity; i++) {
				if (stripe_priv->parity_bufs[i] != NULL) {
					ec_put_parity_buf(ec_io->ec_ch, stripe_priv->parity_bufs[i], strip_size_bytes);
				}
			}
			free(stripe_priv);
		}
		ec_io->module_private = NULL;
	}
}

/*
 * Base bdev reset completion callback
 */
void
ec_base_bdev_reset_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ec_bdev_io *ec_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	ec_bdev_io_complete_part(ec_io, 1, success ?
				 SPDK_BDEV_IO_STATUS_SUCCESS :
				 SPDK_BDEV_IO_STATUS_FAILED);
}


