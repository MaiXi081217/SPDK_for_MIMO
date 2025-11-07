/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "bdev_ec.h"
#include "bdev_ec_internal.h"
#include "spdk/bdev_module.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/queue.h"
#include <string.h>

/* Forward declaration for RMW read complete callback */
static void ec_rmw_read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

/* Forward declaration for write functions */
static int ec_submit_write_stripe(struct ec_bdev_io *ec_io, uint64_t stripe_index,
				  uint8_t *data_indices, uint8_t *parity_indices);
static int ec_submit_write_partial_stripe(struct ec_bdev_io *ec_io, uint64_t stripe_index,
					  uint64_t start_strip, uint32_t offset_in_strip,
					  uint8_t *data_indices, uint8_t *parity_indices);

/*
 * Get parity buffer from pool or allocate new one
 */
static unsigned char *
ec_get_parity_buf(struct ec_bdev_io_channel *ec_ch, struct ec_bdev *ec_bdev, uint32_t buf_size)
{
	struct ec_parity_buf_entry *entry;
	unsigned char *buf;
	size_t align;

	if (ec_ch == NULL) {
		return NULL;
	}

	/* Use ec_bdev alignment if available, otherwise default to 4KB */
	align = (ec_bdev != NULL && ec_bdev->buf_alignment > 0) ? 
		ec_bdev->buf_alignment : 0x1000;

	/* Try to get from pool first */
	/* Note: We only pool buffers of the correct size, so if the first entry
	 * doesn't match, none will match. However, we check anyway for robustness. */
	entry = SLIST_FIRST(&ec_ch->parity_buf_pool);
	if (entry != NULL) {
		if (ec_ch->parity_buf_size == buf_size) {
			SLIST_REMOVE_HEAD(&ec_ch->parity_buf_pool, link);
			/* Prevent counter underflow */
			if (ec_ch->parity_buf_count > 0) {
				ec_ch->parity_buf_count--;
			} else {
				SPDK_WARNLOG("parity_buf_count is 0 but pool is not empty\n");
			}
			buf = entry->buf;
			/* Free the entry structure - we only need the buffer */
			free(entry);
			return buf;
		} else {
			/* Size mismatch - this shouldn't happen if pool is used correctly,
			 * but clean it up to avoid memory leak */
			SPDK_WARNLOG("Buffer size mismatch in parity pool: expected %u, requested %u\n",
				     ec_ch->parity_buf_size, buf_size);
			/* Remove mismatched entry and free it */
			SLIST_REMOVE_HEAD(&ec_ch->parity_buf_pool, link);
			/* Prevent counter underflow */
			if (ec_ch->parity_buf_count > 0) {
				ec_ch->parity_buf_count--;
			} else {
				SPDK_WARNLOG("parity_buf_count is 0 but pool is not empty (size mismatch)\n");
			}
			spdk_dma_free(entry->buf);
			free(entry);
			/* Continue to allocate new buffer */
		}
	}

	/* Allocate new buffer - use malloc instead of zmalloc for better performance */
	/* Parity buffers don't need to be zero-initialized as they will be overwritten by encoding */
	buf = spdk_dma_malloc(buf_size, align, NULL);
	if (buf == NULL) {
		return NULL;
	}

	return buf;
}

/*
 * Return parity buffer to pool or free it
 */
static void
ec_put_parity_buf(struct ec_bdev_io_channel *ec_ch, unsigned char *buf, uint32_t buf_size)
{
	struct ec_parity_buf_entry *entry;

	if (buf == NULL || ec_ch == NULL) {
		return;
	}

	/* Only pool buffers of the correct size, limit pool size to avoid memory waste */
	/* Optimized: Increased pool size to 128 for better concurrency and stability */
	if (ec_ch->parity_buf_size == buf_size && ec_ch->parity_buf_count < 128) {
		/* Optimized: Use malloc instead of calloc - entry only needs buf pointer */
		entry = malloc(sizeof(*entry));
		if (entry != NULL) {
			entry->buf = buf;
			SLIST_INSERT_HEAD(&ec_ch->parity_buf_pool, entry, link);
			ec_ch->parity_buf_count++;
			return;
		}
		/* Entry allocation failed - fall through to free buffer */
	}

	/* Pool full or allocation failed - free the buffer */
	spdk_dma_free(buf);
}

/*
 * Get RMW stripe buffer from pool or allocate new one
 */
static unsigned char *
ec_get_rmw_stripe_buf(struct ec_bdev_io_channel *ec_ch, struct ec_bdev *ec_bdev, uint32_t buf_size)
{
	struct ec_parity_buf_entry *entry;
	unsigned char *buf;
	size_t align;

	if (ec_ch == NULL) {
		return NULL;
	}

	/* Use ec_bdev alignment if available, otherwise default to 4KB */
	align = (ec_bdev != NULL && ec_bdev->buf_alignment > 0) ? 
		ec_bdev->buf_alignment : 0x1000;

	/* Try to get from pool first */
	/* Note: We only pool buffers of the correct size, so if the first entry
	 * doesn't match, none will match. However, we check anyway for robustness. */
	entry = SLIST_FIRST(&ec_ch->rmw_stripe_buf_pool);
	if (entry != NULL) {
		if (ec_ch->rmw_buf_size == buf_size) {
			SLIST_REMOVE_HEAD(&ec_ch->rmw_stripe_buf_pool, link);
			/* Prevent counter underflow */
			if (ec_ch->rmw_buf_count > 0) {
				ec_ch->rmw_buf_count--;
			} else {
				SPDK_WARNLOG("rmw_buf_count is 0 but pool is not empty\n");
			}
			buf = entry->buf;
			/* Free the entry structure - we only need the buffer */
			free(entry);
			return buf;
		} else {
			/* Size mismatch - this shouldn't happen if pool is used correctly,
			 * but clean it up to avoid memory leak */
			SPDK_WARNLOG("Buffer size mismatch in RMW stripe pool: expected %u, requested %u\n",
				     ec_ch->rmw_buf_size, buf_size);
			/* Remove mismatched entry and free it */
			SLIST_REMOVE_HEAD(&ec_ch->rmw_stripe_buf_pool, link);
			/* Prevent counter underflow */
			if (ec_ch->rmw_buf_count > 0) {
				ec_ch->rmw_buf_count--;
			} else {
				SPDK_WARNLOG("rmw_buf_count is 0 but pool is not empty (size mismatch)\n");
			}
			spdk_dma_free(entry->buf);
			free(entry);
			/* Continue to allocate new buffer */
		}
	}

	/* Allocate new buffer - use malloc instead of zmalloc */
	buf = spdk_dma_malloc(buf_size, align, NULL);
	if (buf == NULL) {
		return NULL;
	}

	return buf;
}

/*
 * Return RMW stripe buffer to pool or free it
 */
static void
ec_put_rmw_stripe_buf(struct ec_bdev_io_channel *ec_ch, unsigned char *buf, uint32_t buf_size)
{
	struct ec_parity_buf_entry *entry;

	if (buf == NULL || ec_ch == NULL) {
		return;
	}

	/* Only pool buffers of the correct size, limit pool size */
	/* Optimized: Increased pool size to 64 for better concurrency and stability */
	if (ec_ch->rmw_buf_size == buf_size && ec_ch->rmw_buf_count < 64) {
		/* Optimized: Use malloc instead of calloc - entry only needs buf pointer */
		entry = malloc(sizeof(*entry));
		if (entry != NULL) {
			entry->buf = buf;
			SLIST_INSERT_HEAD(&ec_ch->rmw_stripe_buf_pool, entry, link);
			ec_ch->rmw_buf_count++;
			return;
		}
		/* Entry allocation failed - fall through to free buffer */
	}

	/* Pool full or allocation failed - free the buffer */
	spdk_dma_free(buf);
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

	/* Calculate parity start position using round-robin (similar to RAID5)
	 * For stripe_index i, parity blocks start at position (n - (i % n)) % n
	 * and occupy the next p consecutive positions (wrapping around if needed)
	 * Optimized: calculate once before loop
	 */
	parity_start = (n - (stripe_index % n)) % n;

	/* Select data and parity indices based on round-robin distribution
	 * Optimize: use bitmap for parity position lookup (O(1) instead of O(p))
	 * For p <= 64, use uint64_t bitmap; otherwise fall back to array lookup
	 */
	uint64_t parity_bitmap = 0;
	uint8_t parity_positions[EC_MAX_P];
	
	/* Pre-calculate parity positions and build bitmap if p <= 64 */
	for (uint8_t p_idx = 0; p_idx < ec_bdev->p; p_idx++) {
		uint8_t pos = (parity_start + p_idx) % n;
		parity_positions[p_idx] = pos;
		if (ec_bdev->p <= 64 && pos < 64) {
			parity_bitmap |= (1ULL << pos);
		}
	}

	/* Optimized: Combine counting and selection in a single pass */
	i = 0;
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->desc == NULL || base_info->is_failed) {
			i++;
			continue;
		}

		/* Count operational bdevs while selecting */
		operational_count++;

		/* Check if this position is a parity position - optimized lookup */
		bool is_parity_pos = false;
		if (ec_bdev->p <= 64 && i < 64) {
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
/* Forward declaration - this function is used for decode path (not yet implemented) */
static int __attribute__((unused))
ec_map_base_bdev_to_frag_indices(struct ec_bdev *ec_bdev, uint64_t stripe_index,
				 uint8_t *base_bdev_indices, uint8_t *frag_indices,
				 uint8_t num_failed)
{
	uint8_t data_indices[EC_MAX_K];
	uint8_t parity_indices[EC_MAX_P];
	uint8_t i, j;
	int rc;

	/* Get data and parity indices for this stripe */
	rc = ec_select_base_bdevs_default(ec_bdev, stripe_index, data_indices, parity_indices);
	if (rc != 0) {
		return rc;
	}

	/* Map each failed base_bdev index to its logical fragment index
	 * Optimize: build lookup table for O(1) lookup instead of O(k+p) search
	 * Note: base_bdev indices are uint8_t (max 255), and array size is 510,
	 * so no explicit bounds check needed for uint8_t values
	 */
	uint8_t base_to_frag_map[EC_MAX_K + EC_MAX_P];
	memset(base_to_frag_map, 0xFF, sizeof(base_to_frag_map));  /* 0xFF = invalid */
	
	/* Build lookup table: map base_bdev index -> fragment index */
	for (j = 0; j < ec_bdev->k; j++) {
		/* data_indices[j] is uint8_t (max 255), always < array size (510) */
		base_to_frag_map[data_indices[j]] = j;  /* Data fragment: 0 to k-1 */
	}
	for (j = 0; j < ec_bdev->p; j++) {
		/* parity_indices[j] is uint8_t (max 255), always < array size (510) */
		base_to_frag_map[parity_indices[j]] = ec_bdev->k + j;  /* Parity fragment: k to m-1 */
	}

	/* Map failed base bdevs using lookup table */
	for (i = 0; i < num_failed; i++) {
		uint8_t base_idx = base_bdev_indices[i];
		/* base_idx is uint8_t (max 255), always < array size (510), so safe to index */
		uint8_t frag_idx = base_to_frag_map[base_idx];
		if (frag_idx == 0xFF) {
			/* Base bdev index not found in data or parity indices */
			return -EINVAL;
		}
		frag_indices[i] = frag_idx;
	}

	return 0;
}

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

	/* Prepare data pointers from iovs - optimized to reduce nested loop overhead */
	size_t raid_io_offset = 0;
	size_t raid_io_iov_offset = 0;
	int raid_io_iov_idx = 0;

	/* Optimized: Pre-calculate strip_size_bytes for faster loop */
	for (i = 0; i < k; i++) {
		/* Optimized: Calculate target_offset once per iteration */
		size_t target_offset = i * strip_size_bytes;

		/* Optimized: Advance through iovs more efficiently */
		/* Skip to the target offset by advancing through iovs */
		while (raid_io_offset < target_offset && raid_io_iov_idx < ec_io->iovcnt) {
			size_t bytes_in_this_iov = ec_io->iovs[raid_io_iov_idx].iov_len - raid_io_iov_offset;
			
			/* Safety check: if iov_len is 0, skip to next iov */
			if (bytes_in_this_iov == 0) {
				raid_io_iov_idx++;
				raid_io_iov_offset = 0;
				continue;
			}

			size_t bytes_to_skip = target_offset - raid_io_offset;
			if (bytes_to_skip < bytes_in_this_iov) {
				/* Target is within this iov */
				raid_io_iov_offset += bytes_to_skip;
				raid_io_offset = target_offset;
				break;
			} else {
				/* Target is beyond this iov, skip entire iov */
				raid_io_offset += bytes_in_this_iov;
				raid_io_iov_idx++;
				raid_io_iov_offset = 0;
			}
		}

		if (raid_io_iov_idx >= ec_io->iovcnt) {
			SPDK_ERRLOG("Not enough data in iovs for stripe write\n");
			for (idx = 0; idx < p; idx++) {
				ec_put_parity_buf(ec_io->ec_ch, parity_ptrs[idx], strip_size_bytes);
			}
			return -EINVAL;
		}

		/* Safety check: ensure iov_base is not NULL */
		if (ec_io->iovs[raid_io_iov_idx].iov_base == NULL) {
			SPDK_ERRLOG("iov_base is NULL for iov index %d\n", raid_io_iov_idx);
			for (idx = 0; idx < p; idx++) {
				ec_put_parity_buf(ec_io->ec_ch, parity_ptrs[idx], strip_size_bytes);
			}
			return -EINVAL;
		}

		data_ptrs[i] = (unsigned char *)ec_io->iovs[raid_io_iov_idx].iov_base + raid_io_iov_offset;
		/* Note: raid_io_iov_offset is guaranteed to be < iov_len by the while loop logic */
	}

	/* Encode stripe using ISA-L */
	rc = ec_encode_stripe(ec_bdev, data_ptrs, parity_ptrs, strip_size_bytes);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to encode stripe: %s\n", spdk_strerror(-rc));
		for (i = 0; i < p; i++) {
			ec_put_parity_buf(ec_io->ec_ch, parity_ptrs[i], strip_size_bytes);
		}
		return rc;
	}

	/* Store parity buffers in module_private for cleanup */
	/* Optimized: Use malloc instead of calloc - we initialize all fields explicitly */
	struct ec_stripe_private *stripe_priv = malloc(sizeof(*stripe_priv));
	if (stripe_priv == NULL) {
		for (i = 0; i < p; i++) {
			ec_put_parity_buf(ec_io->ec_ch, parity_ptrs[i], strip_size_bytes);
		}
		return -ENOMEM;
	}
	stripe_priv->type = EC_PRIVATE_TYPE_FULL_STRIPE;
	stripe_priv->num_parity = p;
	/* Optimized: Direct assignment loop instead of separate loop */
	for (i = 0; i < p; i++) {
		stripe_priv->parity_bufs[i] = parity_ptrs[i];
	}
	ec_io->module_private = stripe_priv;

	/* Write data blocks */
	ec_io->base_bdev_io_remaining = k + p;
	ec_io->base_bdev_io_submitted = 0;

	/* Optimized: Pre-calculate pd_strip_base to avoid repeated calculation */
	uint64_t pd_strip_base = stripe_index << ec_bdev->strip_size_shift;

	for (i = 0; i < k; i++) {
		idx = data_indices[i];
		base_info = &ec_bdev->base_bdev_info[idx];
		/* Optimized: Fast path check - combine all conditions */
		if (spdk_unlikely(base_info->desc == NULL || base_info->is_failed ||
				  ec_io->ec_ch->base_channel[idx] == NULL)) {
			SPDK_ERRLOG("Data base bdev %u is not available\n", idx);
			ec_cleanup_stripe_private(ec_io, strip_size_bytes);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return -ENODEV;
		}

		/* Optimized: Use pre-calculated pd_strip_base */
		uint64_t pd_lba = pd_strip_base + base_info->data_offset;
		rc = spdk_bdev_write_blocks(base_info->desc,
					    ec_io->ec_ch->base_channel[idx],
					    data_ptrs[i], pd_lba, ec_bdev->strip_size,
					    ec_base_bdev_io_complete, ec_io);
		if (rc == 0) {
			ec_io->base_bdev_io_submitted++;
		} else if (rc == -ENOMEM) {
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

	/* Write parity blocks */
	/* Optimized: Reuse pd_strip_base calculated above */
	for (i = 0; i < p; i++) {
		idx = parity_indices[i];
		base_info = &ec_bdev->base_bdev_info[idx];
		/* Optimized: Fast path check - combine all conditions */
		if (spdk_unlikely(base_info->desc == NULL || base_info->is_failed ||
				  ec_io->ec_ch->base_channel[idx] == NULL)) {
			SPDK_ERRLOG("Parity base bdev %u is not available\n", idx);
			ec_cleanup_stripe_private(ec_io, strip_size_bytes);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return -ENODEV;
		}

		/* Optimized: Use pre-calculated pd_strip_base */
		uint64_t pd_lba = pd_strip_base + base_info->data_offset;
		rc = spdk_bdev_write_blocks(base_info->desc,
					    ec_io->ec_ch->base_channel[idx],
					    parity_ptrs[i], pd_lba, ec_bdev->strip_size,
					    ec_base_bdev_io_complete, ec_io);
		if (rc == 0) {
			ec_io->base_bdev_io_submitted++;
		} else if (rc == -ENOMEM) {
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
	uint8_t i;
	int rc;

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
		ec_cleanup_rmw_bufs(ec_io, rmw);
		free(rmw);
		ec_io->module_private = NULL;
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
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
		if (rmw->reads_expected < ec_bdev->k) {
			/* We're in a failed state - clean up and complete with error */
			ec_cleanup_rmw_bufs(ec_io, rmw);
			free(rmw);
			ec_io->module_private = NULL;
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
		/* All reads completed successfully - proceed with encoding */
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
		ec_cleanup_rmw_bufs(ec_io, rmw);
		free(rmw);
		ec_io->module_private = NULL;
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	
	/* Verify offset is within strip bounds */
	if (offset_bytes + num_bytes_to_write > strip_size_bytes) {
		SPDK_ERRLOG("Write exceeds strip boundary: offset %u + size %u > strip_size %u\n",
			    offset_bytes, num_bytes_to_write, strip_size_bytes);
		ec_cleanup_rmw_bufs(ec_io, rmw);
		free(rmw);
		ec_io->module_private = NULL;
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	
	unsigned char *target_stripe_data = rmw->data_ptrs[rmw->strip_idx_in_stripe] + offset_bytes;

	/* Copy new data from iovs to stripe buffer */
	size_t bytes_copied = 0;
	int iov_idx = 0;
	size_t iov_offset = 0;

	for (iov_idx = 0; iov_idx < ec_io->iovcnt && bytes_copied < num_bytes_to_write; iov_idx++) {
		/* Safety check: ensure iov_base is not NULL */
		if (ec_io->iovs[iov_idx].iov_base == NULL) {
			SPDK_ERRLOG("iov_base is NULL for iov index %d in RMW\n", iov_idx);
			ec_cleanup_rmw_bufs(ec_io, rmw);
			free(rmw);
			ec_io->module_private = NULL;
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
		
		size_t remaining_in_iov = ec_io->iovs[iov_idx].iov_len - iov_offset;
		size_t remaining_to_copy = num_bytes_to_write - bytes_copied;
		size_t to_copy = spdk_min(remaining_in_iov, remaining_to_copy);

		/* Safety check: verify iov_offset is within bounds */
		if (iov_offset >= ec_io->iovs[iov_idx].iov_len) {
			SPDK_ERRLOG("iov_offset %zu exceeds iov_len %zu for iov index %d\n",
				    iov_offset, ec_io->iovs[iov_idx].iov_len, iov_idx);
			ec_cleanup_rmw_bufs(ec_io, rmw);
			free(rmw);
			ec_io->module_private = NULL;
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		memcpy(target_stripe_data + bytes_copied,
		       (unsigned char *)ec_io->iovs[iov_idx].iov_base + iov_offset,
		       to_copy);
		bytes_copied += to_copy;
		iov_offset += to_copy;

		if (iov_offset >= ec_io->iovs[iov_idx].iov_len) {
			iov_offset = 0;
		}
	}

	if (bytes_copied < num_bytes_to_write) {
		SPDK_ERRLOG("Not enough data in iovs for RMW write\n");
		ec_cleanup_rmw_bufs(ec_io, rmw);
		free(rmw);
		ec_io->module_private = NULL;
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	/* Re-encode entire stripe with all data blocks */
	rc = ec_encode_stripe(ec_bdev, rmw->data_ptrs, rmw->parity_bufs, strip_size_bytes);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to encode stripe in RMW: %s\n", spdk_strerror(-rc));
		ec_cleanup_rmw_bufs(ec_io, rmw);
		free(rmw);
		ec_io->module_private = NULL;
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

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

	/* Write all k data blocks */
	for (i = 0; i < k; i++) {
		idx = rmw->data_indices[i];
		base_info = &ec_bdev->base_bdev_info[idx];
		if (base_info->desc == NULL || base_info->is_failed ||
		    ec_io->ec_ch->base_channel[idx] == NULL) {
			SPDK_ERRLOG("Data base bdev %u is not available for RMW write\n", idx);
			ec_cleanup_rmw_bufs(ec_io, rmw);
			free(rmw);
			ec_io->module_private = NULL;
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		/* Use pre-calculated pd_strip_base */
		uint64_t pd_lba = pd_strip_base + base_info->data_offset;

		rc = spdk_bdev_write_blocks(base_info->desc,
					    ec_io->ec_ch->base_channel[idx],
					    rmw->data_ptrs[i], pd_lba, ec_bdev->strip_size,
					    ec_base_bdev_io_complete, ec_io);
		if (rc == 0) {
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
			ec_cleanup_rmw_bufs(ec_io, rmw);
			free(rmw);
			ec_io->module_private = NULL;
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
	}

	/* Write parity blocks */
	/* Optimized: Reuse pd_strip_base calculated above */
	for (i = 0; i < p; i++) {
		idx = rmw->parity_indices[i];
		base_info = &ec_bdev->base_bdev_info[idx];
		if (base_info->desc == NULL || base_info->is_failed ||
		    ec_io->ec_ch->base_channel[idx] == NULL) {
			SPDK_ERRLOG("Parity base bdev %u is not available for RMW write\n", idx);
			ec_cleanup_rmw_bufs(ec_io, rmw);
			free(rmw);
			ec_io->module_private = NULL;
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		/* Optimized: Use pre-calculated pd_strip_base */
		uint64_t pd_lba = pd_strip_base + base_info->data_offset;

		rc = spdk_bdev_write_blocks(base_info->desc,
					    ec_io->ec_ch->base_channel[idx],
					    rmw->parity_bufs[i], pd_lba, ec_bdev->strip_size,
					    ec_base_bdev_io_complete, ec_io);
		if (rc == 0) {
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
			ec_cleanup_rmw_bufs(ec_io, rmw);
			free(rmw);
			ec_io->module_private = NULL;
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
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
	rmw->reads_expected = k;
	
	ec_io->module_private = rmw;

	/* Standard path: Read all k data blocks in parallel */
	for (i = 0; i < k; i++) {
		idx = data_indices[i];
		base_info = &ec_bdev->base_bdev_info[idx];
		if (base_info->desc == NULL || base_info->is_failed ||
		    ec_io->ec_ch->base_channel[idx] == NULL) {
			SPDK_ERRLOG("Data base bdev %u is not available for RMW read\n", idx);
			ec_cleanup_rmw_bufs(ec_io, rmw);
			free(rmw);
			ec_io->module_private = NULL;
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return -ENODEV;
		}

		uint64_t pd_strip = stripe_index;
		uint64_t pd_lba = (pd_strip << ec_bdev->strip_size_shift) + base_info->data_offset;

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
				ec_cleanup_rmw_bufs(ec_io, rmw);
				free(rmw);
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

	/* Count operational and failed base bdevs - optimized with early exit */
	i = 0;
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->desc == NULL || base_info->is_failed ||
		    ec_io->ec_ch->base_channel[i] == NULL) {
			num_failed++;
		} else {
			num_operational++;
			/* Early exit optimization: if we have enough operational bdevs, stop counting */
			if (num_operational >= ec_bdev->k + ec_bdev->p) {
				/* We have enough operational bdevs, skip remaining checks */
				break;
			}
		}
		i++;
	}

	if (num_operational < ec_bdev->k) {
		SPDK_ERRLOG("Not enough operational blocks (%u < %u) for EC bdev %s\n",
			    num_operational, ec_bdev->k, ec_bdev->bdev.name);
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
		uint8_t k = ec_bdev->k;
		uint32_t strip_size = ec_bdev->strip_size;
		uint32_t strip_size_shift = ec_bdev->strip_size_shift;
		struct ec_bdev_extension_if *ext_if = ec_bdev->extension_if;

		start_strip = ec_io->offset_blocks >> strip_size_shift;
		end_strip = (ec_io->offset_blocks + ec_io->num_blocks - 1) >> strip_size_shift;

		if (start_strip != end_strip && ec_bdev->num_base_bdevs > 1) {
			SPDK_ERRLOG("I/O spans strip boundary for EC bdev %s\n",
				    ec_bdev->bdev.name);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		stripe_index = start_strip / k;
		offset_in_strip = ec_io->offset_blocks & (strip_size - 1);

		/* Optimized: Check ext_if once and cache the result */
		bool use_ext_if = (ext_if != NULL && ext_if->select_base_bdevs != NULL);
		if (use_ext_if) {
			rc = ext_if->select_base_bdevs(ext_if, ec_bdev,
						      ec_io->offset_blocks,
						      ec_io->num_blocks,
						      data_indices, parity_indices,
						      ext_if->ctx);
			if (rc != 0) {
				SPDK_ERRLOG("Extension interface failed to select base bdevs\n");
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		} else {
			rc = ec_select_base_bdevs_default(ec_bdev, stripe_index, data_indices, parity_indices);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to select base bdevs\n");
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
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
		struct ec_bdev_extension_if *ext_if = ec_bdev->extension_if;

		start_strip = ec_io->offset_blocks >> ec_bdev->strip_size_shift;
		end_strip = (ec_io->offset_blocks + ec_io->num_blocks - 1) >>
			    ec_bdev->strip_size_shift;

		if (start_strip != end_strip && ec_bdev->num_base_bdevs > 1) {
			SPDK_ERRLOG("Read I/O spans strip boundary for EC bdev %s\n",
				    ec_bdev->bdev.name);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		stripe_index = start_strip / ec_bdev->k;
		strip_idx_in_stripe = start_strip % ec_bdev->k;
		offset_in_strip = ec_io->offset_blocks & (ec_bdev->strip_size - 1);

		if (num_failed > 0 && num_failed <= ec_bdev->p) {
			SPDK_WARNLOG("Decode path not yet implemented for EC bdev %s\n",
				     ec_bdev->bdev.name);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		uint8_t data_indices[EC_MAX_K];
		uint8_t parity_indices[EC_MAX_P];
		/* Optimized: Check ext_if once and cache the result */
		bool use_ext_if = (ext_if != NULL && ext_if->select_base_bdevs != NULL);
		if (use_ext_if) {
			rc = ext_if->select_base_bdevs(ext_if, ec_bdev,
						      ec_io->offset_blocks,
						      ec_io->num_blocks,
						      data_indices, parity_indices,
						      ext_if->ctx);
			if (rc != 0) {
				SPDK_ERRLOG("Extension interface failed to select base bdevs\n");
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
			target_data_idx = data_indices[strip_idx_in_stripe];
		} else {
			rc = ec_select_base_bdevs_default(ec_bdev, stripe_index, data_indices, parity_indices);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to select base bdevs\n");
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
			target_data_idx = data_indices[strip_idx_in_stripe];
		}

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
	struct ec_bdev_extension_if *ext_if;
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

	if (success && ec_bdev->extension_if != NULL &&
	    ec_bdev->extension_if->notify_io_complete != NULL) {
		EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
			if (base_info->desc != NULL &&
			    spdk_bdev_desc_get_bdev(base_info->desc) == bdev_io->bdev) {
				ext_if = ec_bdev->extension_if;
				ext_if->notify_io_complete(ext_if, ec_bdev, base_info,
							   ec_io->offset_blocks,
							   ec_io->num_blocks,
							   ec_io->type == SPDK_BDEV_IO_TYPE_WRITE,
							   ext_if->ctx);
				break;
			}
		}
	}

	spdk_bdev_free_io(bdev_io);

	all_complete = ec_bdev_io_complete_part(ec_io, 1, success ?
						SPDK_BDEV_IO_STATUS_SUCCESS :
						SPDK_BDEV_IO_STATUS_FAILED);

	/* Clean up resources if this was a write and all I/O is complete */
	if (all_complete && ec_io->type == SPDK_BDEV_IO_TYPE_WRITE && ec_io->module_private != NULL) {
		struct ec_stripe_private *stripe_priv = ec_io->module_private;
		struct ec_rmw_private *rmw;
		uint8_t i;
		uint32_t strip_size_bytes;

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

