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
 * Base bdev selection - default implementation
 * This function is declared in bdev_ec_internal.h
 */
int
ec_select_base_bdevs_default(struct ec_bdev *ec_bdev, uint8_t *data_indices,
			     uint8_t *parity_indices)
{
	struct ec_base_bdev_info *base_info;
	uint8_t i = 0;
	uint8_t data_idx = 0;
	uint8_t parity_idx = 0;

	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->desc == NULL || base_info->is_failed) {
			i++;
			continue;
		}

		if (base_info->is_data_block && data_idx < ec_bdev->k) {
			data_indices[data_idx++] = i;
		} else if (!base_info->is_data_block && parity_idx < ec_bdev->p) {
			parity_indices[parity_idx++] = i;
		}
		i++;
	}

	if (data_idx < ec_bdev->k || parity_idx < ec_bdev->p) {
		return -ENODEV;
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
	uint8_t i, idx;
	int rc;

	/* Allocate parity buffers */
	for (i = 0; i < ec_bdev->p; i++) {
		parity_ptrs[i] = spdk_dma_zmalloc(strip_size_bytes, 0, NULL);
		if (parity_ptrs[i] == NULL) {
			SPDK_ERRLOG("Failed to allocate parity buffer %u\n", i);
			for (idx = 0; idx < i; idx++) {
				spdk_dma_free(parity_ptrs[idx]);
			}
			return -ENOMEM;
		}
	}

	/* Prepare data pointers from iovs */
	size_t raid_io_offset = 0;
	size_t raid_io_iov_offset = 0;
	int raid_io_iov_idx = 0;

	for (i = 0; i < ec_bdev->k; i++) {
		size_t target_offset = i * strip_size_bytes;

		/* Find the position in iovs where this data block starts */
		while (raid_io_offset < target_offset && raid_io_iov_idx < ec_io->iovcnt) {
			size_t bytes_in_this_iov = ec_io->iovs[raid_io_iov_idx].iov_len - raid_io_iov_offset;
			size_t bytes_to_skip = target_offset - raid_io_offset;

			/* Safety check: if iov_len is 0 or already consumed, skip to next iov */
			if (bytes_in_this_iov == 0) {
				raid_io_iov_idx++;
				raid_io_iov_offset = 0;
				continue;
			}

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
			for (idx = 0; idx < ec_bdev->p; idx++) {
				spdk_dma_free(parity_ptrs[idx]);
			}
			return -EINVAL;
		}

		/* Safety check: ensure iov_base is not NULL */
		if (ec_io->iovs[raid_io_iov_idx].iov_base == NULL) {
			SPDK_ERRLOG("iov_base is NULL for iov index %d\n", raid_io_iov_idx);
			for (idx = 0; idx < ec_bdev->p; idx++) {
				spdk_dma_free(parity_ptrs[idx]);
			}
			return -EINVAL;
		}

		data_ptrs[i] = (unsigned char *)ec_io->iovs[raid_io_iov_idx].iov_base + raid_io_iov_offset;
	}

	/* Encode stripe using ISA-L */
	rc = ec_encode_stripe(ec_bdev, data_ptrs, parity_ptrs, strip_size_bytes);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to encode stripe: %s\n", spdk_strerror(-rc));
		for (i = 0; i < ec_bdev->p; i++) {
			spdk_dma_free(parity_ptrs[i]);
		}
		return rc;
	}

	/* Store parity buffers in module_private for cleanup */
	struct ec_stripe_private *stripe_priv = calloc(1, sizeof(*stripe_priv));
	if (stripe_priv == NULL) {
		for (i = 0; i < ec_bdev->p; i++) {
			spdk_dma_free(parity_ptrs[i]);
		}
		return -ENOMEM;
	}
	stripe_priv->type = EC_PRIVATE_TYPE_FULL_STRIPE;
	for (i = 0; i < ec_bdev->p; i++) {
		stripe_priv->parity_bufs[i] = parity_ptrs[i];
	}
	stripe_priv->num_parity = ec_bdev->p;
	ec_io->module_private = stripe_priv;

	/* Write data blocks */
	ec_io->base_bdev_io_remaining = ec_bdev->k + ec_bdev->p;
	ec_io->base_bdev_io_submitted = 0;

	for (i = 0; i < ec_bdev->k; i++) {
		idx = data_indices[i];
		base_info = &ec_bdev->base_bdev_info[idx];
		if (base_info->desc == NULL || base_info->is_failed ||
		    ec_io->ec_ch->base_channel[idx] == NULL) {
			SPDK_ERRLOG("Data base bdev %u is not available\n", idx);
			if (ec_io->module_private != NULL) {
				struct ec_stripe_private *sp = ec_io->module_private;
				for (uint8_t j = 0; j < sp->num_parity; j++) {
					spdk_dma_free(sp->parity_bufs[j]);
				}
				free(sp);
				ec_io->module_private = NULL;
			}
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return -ENODEV;
		}

		uint64_t pd_strip = stripe_index;
		uint64_t pd_lba = (pd_strip << ec_bdev->strip_size_shift) + base_info->data_offset;
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
			if (ec_io->module_private != NULL) {
				struct ec_stripe_private *sp = ec_io->module_private;
				for (uint8_t j = 0; j < sp->num_parity; j++) {
					spdk_dma_free(sp->parity_bufs[j]);
				}
				free(sp);
				ec_io->module_private = NULL;
			}
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return rc;
		}
	}

	/* Write parity blocks */
	for (i = 0; i < ec_bdev->p; i++) {
		idx = parity_indices[i];
		base_info = &ec_bdev->base_bdev_info[idx];
		if (base_info->desc == NULL || base_info->is_failed ||
		    ec_io->ec_ch->base_channel[idx] == NULL) {
			SPDK_ERRLOG("Parity base bdev %u is not available\n", idx);
			if (ec_io->module_private != NULL) {
				struct ec_stripe_private *sp = ec_io->module_private;
				for (uint8_t j = 0; j < sp->num_parity; j++) {
					spdk_dma_free(sp->parity_bufs[j]);
				}
				free(sp);
				ec_io->module_private = NULL;
			}
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return -ENODEV;
		}

		uint64_t pd_strip = stripe_index;
		uint64_t pd_lba = (pd_strip << ec_bdev->strip_size_shift) + base_info->data_offset;
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
			if (ec_io->module_private != NULL) {
				struct ec_stripe_private *sp = ec_io->module_private;
				for (uint8_t j = 0; j < sp->num_parity; j++) {
					spdk_dma_free(sp->parity_bufs[j]);
				}
				free(sp);
				ec_io->module_private = NULL;
			}
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
		if (rmw->stripe_buf != NULL) {
			spdk_dma_free(rmw->stripe_buf);
		}
		for (i = 0; i < ec_bdev->p; i++) {
			if (rmw->parity_bufs[i] != NULL) {
				spdk_dma_free(rmw->parity_bufs[i]);
			}
		}
		free(rmw);
		ec_io->module_private = NULL;
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	rmw->reads_completed++;
	if (rmw->reads_completed < rmw->reads_expected) {
		return;
	}

	/* All reads completed - merge new data and encode */
	rmw->state = EC_RMW_STATE_ENCODING;

	uint32_t strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
	uint32_t block_size = ec_bdev->bdev.blocklen;
	uint32_t offset_bytes = rmw->offset_in_strip * block_size;
	uint32_t num_bytes_to_write = rmw->num_blocks_to_write * block_size;
	unsigned char *target_stripe_data = rmw->data_ptrs[rmw->strip_idx_in_stripe] + offset_bytes;

	/* Copy new data from iovs to stripe buffer */
	size_t bytes_copied = 0;
	int iov_idx = 0;
	size_t iov_offset = 0;

	for (iov_idx = 0; iov_idx < ec_io->iovcnt && bytes_copied < num_bytes_to_write; iov_idx++) {
		size_t remaining_in_iov = ec_io->iovs[iov_idx].iov_len - iov_offset;
		size_t remaining_to_copy = num_bytes_to_write - bytes_copied;
		size_t to_copy = spdk_min(remaining_in_iov, remaining_to_copy);

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
		spdk_dma_free(rmw->stripe_buf);
		for (i = 0; i < ec_bdev->p; i++) {
			spdk_dma_free(rmw->parity_bufs[i]);
		}
		free(rmw);
		ec_io->module_private = NULL;
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	/* Encode stripe using ISA-L */
	rc = ec_encode_stripe(ec_bdev, rmw->data_ptrs, rmw->parity_bufs, strip_size_bytes);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to encode stripe in RMW: %s\n", spdk_strerror(-rc));
		spdk_dma_free(rmw->stripe_buf);
		for (i = 0; i < ec_bdev->p; i++) {
			spdk_dma_free(rmw->parity_bufs[i]);
		}
		free(rmw);
		ec_io->module_private = NULL;
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	/* Write all blocks (k data + p parity) */
	rmw->state = EC_RMW_STATE_WRITING;
	ec_io->base_bdev_io_remaining = ec_bdev->k + ec_bdev->p;
	ec_io->base_bdev_io_submitted = 0;

	struct ec_base_bdev_info *base_info;
	uint8_t idx;

	/* Write data blocks */
	for (i = 0; i < ec_bdev->k; i++) {
		idx = rmw->data_indices[i];
		base_info = &ec_bdev->base_bdev_info[idx];
		if (base_info->desc == NULL || base_info->is_failed ||
		    ec_io->ec_ch->base_channel[idx] == NULL) {
			SPDK_ERRLOG("Data base bdev %u is not available for RMW write\n", idx);
			spdk_dma_free(rmw->stripe_buf);
			for (uint8_t j = 0; j < ec_bdev->p; j++) {
				spdk_dma_free(rmw->parity_bufs[j]);
			}
			free(rmw);
			ec_io->module_private = NULL;
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		uint64_t pd_strip = rmw->stripe_index;
		uint64_t pd_lba = (pd_strip << ec_bdev->strip_size_shift) + base_info->data_offset;

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
			spdk_dma_free(rmw->stripe_buf);
			for (uint8_t j = 0; j < ec_bdev->p; j++) {
				spdk_dma_free(rmw->parity_bufs[j]);
			}
			free(rmw);
			ec_io->module_private = NULL;
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
	}

	/* Write parity blocks */
	for (i = 0; i < ec_bdev->p; i++) {
		idx = rmw->parity_indices[i];
		base_info = &ec_bdev->base_bdev_info[idx];
		if (base_info->desc == NULL || base_info->is_failed ||
		    ec_io->ec_ch->base_channel[idx] == NULL) {
			SPDK_ERRLOG("Parity base bdev %u is not available for RMW write\n", idx);
			spdk_dma_free(rmw->stripe_buf);
			for (uint8_t j = 0; j < ec_bdev->p; j++) {
				spdk_dma_free(rmw->parity_bufs[j]);
			}
			free(rmw);
			ec_io->module_private = NULL;
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		uint64_t pd_strip = rmw->stripe_index;
		uint64_t pd_lba = (pd_strip << ec_bdev->strip_size_shift) + base_info->data_offset;

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
			spdk_dma_free(rmw->stripe_buf);
			for (uint8_t j = 0; j < ec_bdev->p; j++) {
				spdk_dma_free(rmw->parity_bufs[j]);
			}
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
	uint32_t strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
	struct ec_rmw_private *rmw;
	struct ec_base_bdev_info *base_info;
	uint32_t strip_idx_in_stripe = start_strip % ec_bdev->k;
	uint8_t i;
	uint8_t idx;
	int rc;

	/* Allocate RMW context */
	rmw = calloc(1, sizeof(*rmw));
	if (rmw == NULL) {
		SPDK_ERRLOG("Failed to allocate RMW context\n");
		return -ENOMEM;
	}
	rmw->type = EC_PRIVATE_TYPE_RMW;

	/* Allocate full stripe buffer */
	rmw->stripe_buf = spdk_dma_zmalloc(strip_size_bytes * ec_bdev->k, 0, NULL);
	if (rmw->stripe_buf == NULL) {
		SPDK_ERRLOG("Failed to allocate stripe buffer for RMW\n");
		free(rmw);
		return -ENOMEM;
	}

	/* Allocate parity buffers */
	for (i = 0; i < ec_bdev->p; i++) {
		rmw->parity_bufs[i] = spdk_dma_zmalloc(strip_size_bytes, 0, NULL);
		if (rmw->parity_bufs[i] == NULL) {
			SPDK_ERRLOG("Failed to allocate parity buffer %u for RMW\n", i);
			for (uint8_t j = 0; j < i; j++) {
				spdk_dma_free(rmw->parity_bufs[j]);
			}
			spdk_dma_free(rmw->stripe_buf);
			free(rmw);
			return -ENOMEM;
		}
	}

	/* Prepare data pointers */
	for (i = 0; i < ec_bdev->k; i++) {
		rmw->data_ptrs[i] = rmw->stripe_buf + i * strip_size_bytes;
	}

	/* Store stripe information */
	rmw->stripe_index = stripe_index;
	rmw->strip_idx_in_stripe = strip_idx_in_stripe;
	rmw->offset_in_strip = offset_in_strip;
	rmw->num_blocks_to_write = ec_io->num_blocks;
	memcpy(rmw->data_indices, data_indices, ec_bdev->k);
	memcpy(rmw->parity_indices, parity_indices, ec_bdev->p);

	rmw->state = EC_RMW_STATE_READING;
	rmw->reads_completed = 0;
	rmw->reads_expected = ec_bdev->k;
	ec_io->module_private = rmw;

	/* Read all k data blocks in parallel */
	for (i = 0; i < ec_bdev->k; i++) {
		idx = data_indices[i];
		base_info = &ec_bdev->base_bdev_info[idx];
		if (base_info->desc == NULL || base_info->is_failed ||
		    ec_io->ec_ch->base_channel[idx] == NULL) {
			SPDK_ERRLOG("Data base bdev %u is not available for RMW read\n", idx);
			spdk_dma_free(rmw->stripe_buf);
			for (uint8_t j = 0; j < ec_bdev->p; j++) {
				spdk_dma_free(rmw->parity_bufs[j]);
			}
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
			ec_bdev_queue_io_wait(ec_io,
					      spdk_bdev_desc_get_bdev(base_info->desc),
					      ec_io->ec_ch->base_channel[idx],
					      (spdk_bdev_io_wait_cb)ec_submit_rw_request);
			spdk_dma_free(rmw->stripe_buf);
			for (uint8_t j = 0; j < ec_bdev->p; j++) {
				spdk_dma_free(rmw->parity_bufs[j]);
			}
			free(rmw);
			ec_io->module_private = NULL;
			return 0;
		} else {
			SPDK_ERRLOG("Failed to read data block %u for RMW: %s\n",
				    idx, spdk_strerror(-rc));
			spdk_dma_free(rmw->stripe_buf);
			for (uint8_t j = 0; j < ec_bdev->p; j++) {
				spdk_dma_free(rmw->parity_bufs[j]);
			}
			free(rmw);
			ec_io->module_private = NULL;
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

	/* Count operational and failed base bdevs */
	i = 0;
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->desc == NULL || base_info->is_failed ||
		    ec_io->ec_ch->base_channel[i] == NULL) {
			num_failed++;
		} else {
			num_operational++;
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
		struct ec_bdev_extension_if *ext_if = ec_bdev->extension_if;

		start_strip = ec_io->offset_blocks >> ec_bdev->strip_size_shift;
		end_strip = (ec_io->offset_blocks + ec_io->num_blocks - 1) >>
			    ec_bdev->strip_size_shift;

		if (start_strip != end_strip && ec_bdev->num_base_bdevs > 1) {
			SPDK_ERRLOG("I/O spans strip boundary for EC bdev %s\n",
				    ec_bdev->bdev.name);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		stripe_index = start_strip / ec_bdev->k;
		offset_in_strip = ec_io->offset_blocks & (ec_bdev->strip_size - 1);

		if (ext_if != NULL && ext_if->select_base_bdevs != NULL) {
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
			rc = ec_select_base_bdevs_default(ec_bdev, data_indices, parity_indices);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to select base bdevs\n");
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		}

		uint64_t full_stripe_size = ec_bdev->strip_size * ec_bdev->k;
		if (offset_in_strip == 0 && ec_io->num_blocks == full_stripe_size &&
		    (start_strip % ec_bdev->k) == 0) {
			rc = ec_submit_write_stripe(ec_io, stripe_index, data_indices, parity_indices);
			if (rc != 0) {
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			}
		} else {
			rc = ec_submit_write_partial_stripe(ec_io, stripe_index, start_strip,
							    offset_in_strip, data_indices, parity_indices);
			if (rc != 0) {
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
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
		if (ext_if != NULL && ext_if->select_base_bdevs != NULL) {
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
			rc = ec_select_base_bdevs_default(ec_bdev, data_indices, parity_indices);
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

		rc = spdk_bdev_read_blocks(base_info->desc,
					   ec_io->ec_ch->base_channel[target_data_idx],
					   ec_io->iovs[0].iov_base,
					   pd_lba,
					   ec_io->num_blocks,
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
			SPDK_ERRLOG("bdev io submit error not due to ENOMEM\n");
			assert(false);
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
			SPDK_ERRLOG("bdev io submit error not due to ENOMEM\n");
			assert(false);
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
	struct ec_bdev *ec_bdev = ec_io->ec_bdev;
	struct ec_base_bdev_info *base_info = NULL;
	struct ec_bdev_extension_if *ext_if;
	bool all_complete = false;

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

