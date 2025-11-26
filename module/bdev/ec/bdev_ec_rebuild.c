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
#include "spdk/thread.h"
#include "spdk/vmd.h"
#include "spdk/crc32.h"
#include <string.h>

/* Forward declarations are now in bdev_ec.h and bdev_ec_internal.h */

/* Forward declarations */
static void ec_rebuild_stripe_read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void ec_rebuild_stripe_write_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void ec_rebuild_next_stripe(void *ctx);
static void ec_rebuild_cleanup(struct ec_rebuild_context *rebuild_ctx);

/*
 * Clean up rebuild context
 */
static void
ec_rebuild_cleanup(struct ec_rebuild_context *rebuild_ctx)
{
	if (rebuild_ctx == NULL) {
		return;
	}

	if (rebuild_ctx->stripe_buf != NULL && rebuild_ctx->rebuild_ch != NULL) {
		struct ec_bdev *ec_bdev = rebuild_ctx->target_base_info->ec_bdev;
		struct ec_bdev_io_channel *ec_ch = spdk_io_channel_get_ctx(rebuild_ctx->rebuild_ch);
		uint32_t strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
		ec_put_rmw_stripe_buf(ec_ch, rebuild_ctx->stripe_buf,
				      strip_size_bytes * ec_bdev->k);
	}

	if (rebuild_ctx->recover_buf != NULL) {
		spdk_dma_free(rebuild_ctx->recover_buf);
	}

	if (rebuild_ctx->rebuild_ch != NULL) {
		spdk_put_io_channel(rebuild_ctx->rebuild_ch);
	}

	free(rebuild_ctx);
}

/*
 * Rebuild stripe write complete callback
 */
static void
ec_rebuild_stripe_write_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ec_rebuild_context *rebuild_ctx = cb_arg;
	struct ec_bdev *ec_bdev;

	spdk_bdev_free_io(bdev_io);

	if (rebuild_ctx == NULL) {
		SPDK_ERRLOG("rebuild_ctx is NULL in write complete\n");
		return;
	}

	ec_bdev = rebuild_ctx->target_base_info->ec_bdev;
	if (ec_bdev == NULL) {
		SPDK_ERRLOG("ec_bdev is NULL in rebuild write complete\n");
		ec_rebuild_cleanup(rebuild_ctx);
		return;
	}

	if (!success) {
		SPDK_ERRLOG("Rebuild write failed for stripe %lu on EC bdev %s\n",
			    rebuild_ctx->current_stripe, ec_bdev->bdev.name);
		
		/* Update superblock to mark rebuild as FAILED (revert REBUILDING to FAILED) */
		if (ec_bdev_sb_update_base_bdev_state(ec_bdev, rebuild_ctx->target_slot,
						       EC_SB_BASE_BDEV_FAILED)) {
			ec_bdev_write_superblock(ec_bdev, NULL, NULL);
		}
		
		if (rebuild_ctx->rebuild_done_cb) {
			rebuild_ctx->rebuild_done_cb(rebuild_ctx->rebuild_done_ctx, -EIO);
		}
		ec_bdev->rebuild_ctx = NULL;
		ec_rebuild_cleanup(rebuild_ctx);
		return;
	}

	/* Write completed successfully - move to next stripe */
	rebuild_ctx->current_stripe++;
	rebuild_ctx->state = EC_REBUILD_STATE_IDLE;

	/* Check if rebuild is complete */
	if (rebuild_ctx->current_stripe >= rebuild_ctx->total_stripes) {
		SPDK_NOTICELOG("Rebuild completed for base bdev %s (slot %u) on EC bdev %s\n",
			       rebuild_ctx->target_base_info->name,
			       rebuild_ctx->target_slot, ec_bdev->bdev.name);

		/* Update superblock to mark base bdev as configured */
		if (ec_bdev_sb_update_base_bdev_state(ec_bdev, rebuild_ctx->target_slot,
						       EC_SB_BASE_BDEV_CONFIGURED)) {
			ec_bdev_write_superblock(ec_bdev, NULL, NULL);
		}

		/* Turn off LED for all healthy disks (rebuild completed, no need to blink anymore) */
		ec_bdev_set_healthy_disks_led(ec_bdev, SPDK_VMD_LED_STATE_OFF);

		/* Call completion callback */
		if (rebuild_ctx->rebuild_done_cb) {
			rebuild_ctx->rebuild_done_cb(rebuild_ctx->rebuild_done_ctx, 0);
		}

		ec_bdev->rebuild_ctx = NULL;
		ec_rebuild_cleanup(rebuild_ctx);
		return;
	}

	/* Continue with next stripe - use message to avoid stack overflow */
	rebuild_ctx->state = EC_REBUILD_STATE_IDLE;
	spdk_thread_send_msg(spdk_get_thread(), ec_rebuild_next_stripe, rebuild_ctx);
}

/*
 * Rebuild stripe read complete callback
 */
static void
ec_rebuild_stripe_read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ec_rebuild_context *rebuild_ctx = cb_arg;
	struct ec_bdev *ec_bdev;
	unsigned char *recover_ptrs[EC_MAX_P];
	uint8_t failed_frag_list[EC_MAX_P];
	int rc;

	spdk_bdev_free_io(bdev_io);

	if (rebuild_ctx == NULL) {
		SPDK_ERRLOG("rebuild_ctx is NULL in read complete\n");
		return;
	}

	ec_bdev = rebuild_ctx->target_base_info->ec_bdev;
	if (ec_bdev == NULL) {
		SPDK_ERRLOG("ec_bdev is NULL in rebuild read complete\n");
		if (rebuild_ctx->rebuild_done_cb) {
			rebuild_ctx->rebuild_done_cb(rebuild_ctx->rebuild_done_ctx, -EINVAL);
		}
		ec_rebuild_cleanup(rebuild_ctx);
		return;
	}

	if (!success) {
		SPDK_ERRLOG("Rebuild read failed for stripe %lu on EC bdev %s\n",
			    rebuild_ctx->current_stripe, ec_bdev->bdev.name);
		
		/* Update superblock to mark rebuild as FAILED (revert REBUILDING to FAILED) */
		if (ec_bdev_sb_update_base_bdev_state(ec_bdev, rebuild_ctx->target_slot,
						       EC_SB_BASE_BDEV_FAILED)) {
			ec_bdev_write_superblock(ec_bdev, NULL, NULL);
		}
		
		if (rebuild_ctx->rebuild_done_cb) {
			rebuild_ctx->rebuild_done_cb(rebuild_ctx->rebuild_done_ctx, -EIO);
		}
		ec_bdev->rebuild_ctx = NULL;
		ec_rebuild_cleanup(rebuild_ctx);
		return;
	}

	rebuild_ctx->reads_completed++;

	/* Check if all reads completed */
	if (rebuild_ctx->reads_completed >= rebuild_ctx->reads_expected) {
		/* All reads completed - proceed with decoding */
		rebuild_ctx->state = EC_REBUILD_STATE_DECODING;

		uint32_t strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
		failed_frag_list[0] = rebuild_ctx->failed_frag_idx;
		recover_ptrs[0] = rebuild_ctx->recover_buf;

		/* Decode the failed fragment */
		rc = ec_decode_stripe(ec_bdev, rebuild_ctx->data_ptrs, recover_ptrs,
				      failed_frag_list, 1, strip_size_bytes);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to decode stripe %lu during rebuild: %s\n",
				    rebuild_ctx->current_stripe, spdk_strerror(-rc));
			
			/* Update superblock to mark rebuild as FAILED (revert REBUILDING to FAILED) */
			if (ec_bdev_sb_update_base_bdev_state(ec_bdev, rebuild_ctx->target_slot,
							       EC_SB_BASE_BDEV_FAILED)) {
				ec_bdev_write_superblock(ec_bdev, NULL, NULL);
			}
			
			if (rebuild_ctx->rebuild_done_cb) {
				rebuild_ctx->rebuild_done_cb(rebuild_ctx->rebuild_done_ctx, rc);
			}
			ec_bdev->rebuild_ctx = NULL;
			ec_rebuild_cleanup(rebuild_ctx);
			return;
		}

		/* Write recovered data to target base bdev */
		rebuild_ctx->state = EC_REBUILD_STATE_WRITING;
		uint64_t pd_lba = (rebuild_ctx->current_stripe << ec_bdev->strip_size_shift) +
				  rebuild_ctx->target_base_info->data_offset;

		struct ec_bdev_io_channel *ec_ch = spdk_io_channel_get_ctx(rebuild_ctx->rebuild_ch);
		rc = spdk_bdev_write_blocks(rebuild_ctx->target_base_info->desc,
					    ec_ch->base_channel[rebuild_ctx->target_slot],
					    rebuild_ctx->recover_buf, pd_lba, ec_bdev->strip_size,
					    ec_rebuild_stripe_write_complete, rebuild_ctx);
		if (rc != 0) {
			if (rc == -ENOMEM) {
				/* Queue wait and retry */
				rebuild_ctx->wait_entry.bdev = spdk_bdev_desc_get_bdev(rebuild_ctx->target_base_info->desc);
				rebuild_ctx->wait_entry.cb_fn = ec_rebuild_next_stripe;
				rebuild_ctx->wait_entry.cb_arg = rebuild_ctx;
				spdk_bdev_queue_io_wait(rebuild_ctx->wait_entry.bdev,
							ec_ch->base_channel[rebuild_ctx->target_slot],
							&rebuild_ctx->wait_entry);
				return;
			} else {
				SPDK_ERRLOG("Failed to write stripe %lu during rebuild: %s\n",
					    rebuild_ctx->current_stripe, spdk_strerror(-rc));
				
				/* Update superblock to mark rebuild as FAILED (revert REBUILDING to FAILED) */
				if (ec_bdev_sb_update_base_bdev_state(ec_bdev, rebuild_ctx->target_slot,
								       EC_SB_BASE_BDEV_FAILED)) {
					ec_bdev_write_superblock(ec_bdev, NULL, NULL);
				}
				
				if (rebuild_ctx->rebuild_done_cb) {
					rebuild_ctx->rebuild_done_cb(rebuild_ctx->rebuild_done_ctx, rc);
				}
				ec_bdev->rebuild_ctx = NULL;
				ec_rebuild_cleanup(rebuild_ctx);
				return;
			}
		}
	} else {
		/* Still waiting for more reads to complete */
		return;
	}
}

/*
 * Rebuild next stripe
 */
static void
ec_rebuild_next_stripe(void *ctx)
{
	struct ec_rebuild_context *rebuild_ctx = ctx;
	struct ec_bdev *ec_bdev;
	struct ec_base_bdev_info *base_info;
	uint8_t data_indices[EC_MAX_K];
	uint8_t parity_indices[EC_MAX_P];
	uint8_t i, idx;
	uint8_t num_available = 0;
	int rc;

	if (rebuild_ctx == NULL) {
		return;
	}

	ec_bdev = rebuild_ctx->target_base_info->ec_bdev;
	if (ec_bdev == NULL) {
		SPDK_ERRLOG("ec_bdev is NULL in rebuild next stripe\n");
		ec_rebuild_cleanup(rebuild_ctx);
		return;
	}

	/* Check if rebuild is paused */
	if (rebuild_ctx->paused) {
		return;
	}

	/* Check if rebuild is complete */
	if (rebuild_ctx->current_stripe >= rebuild_ctx->total_stripes) {
		return;
	}

	/* Get data and parity indices for this stripe */
	if (ec_bdev->selection_config.select_fn != NULL) {
		if (ec_bdev->selection_config.wear_leveling_enabled &&
		    ec_bdev->selection_config.select_fn == ec_select_base_bdevs_wear_leveling) {
			rc = ec_selection_bind_group_profile(ec_bdev, rebuild_ctx->current_stripe);
			if (spdk_unlikely(rc != 0)) {
				SPDK_ERRLOG("Failed to bind stripe %lu to wear profile during rebuild on EC bdev %s: %s\n",
					    rebuild_ctx->current_stripe, ec_bdev->bdev.name, spdk_strerror(-rc));
				goto handle_select_error;
			}
			SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: Rebuild stripe %lu bound to profile before selection\n",
				      ec_bdev->bdev.name, rebuild_ctx->current_stripe);
		}
		rc = ec_bdev->selection_config.select_fn(ec_bdev, rebuild_ctx->current_stripe,
							data_indices, parity_indices);
		SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: Rebuild stripe %lu selected via wear-leveling path (rc=%d)\n",
			      ec_bdev->bdev.name, rebuild_ctx->current_stripe, rc);
	} else {
		rc = ec_select_base_bdevs_default(ec_bdev, rebuild_ctx->current_stripe,
						  data_indices, parity_indices);
		SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: Rebuild stripe %lu selected via default path (rc=%d)\n",
			      ec_bdev->bdev.name, rebuild_ctx->current_stripe, rc);
	}
	if (rc != 0) {
		SPDK_ERRLOG("Failed to select base bdevs for rebuild stripe %lu on EC bdev %s: %s\n",
			    rebuild_ctx->current_stripe, ec_bdev->bdev.name, spdk_strerror(-rc));
		goto handle_select_error;
	}

	/* Find available base bdevs and determine failed fragment index */
	memset(rebuild_ctx->frag_map, 0xFF, sizeof(rebuild_ctx->frag_map));
	uint8_t k = ec_bdev->k;
	uint8_t p = ec_bdev->p;

	for (i = 0; i < k; i++) {
		idx = data_indices[i];
		rebuild_ctx->frag_map[idx] = i;
		base_info = &ec_bdev->base_bdev_info[idx];
		if (idx == rebuild_ctx->target_slot) {
			rebuild_ctx->failed_frag_idx = i;
		} else if (base_info->desc != NULL && !base_info->is_failed) {
			struct ec_bdev_io_channel *ec_ch = spdk_io_channel_get_ctx(rebuild_ctx->rebuild_ch);
			if (ec_ch->base_channel[idx] != NULL) {
				rebuild_ctx->available_indices[num_available++] = idx;
			}
		}
	}
	for (i = 0; i < p; i++) {
		idx = parity_indices[i];
		rebuild_ctx->frag_map[idx] = k + i;
		base_info = &ec_bdev->base_bdev_info[idx];
		if (idx == rebuild_ctx->target_slot) {
			rebuild_ctx->failed_frag_idx = k + i;
		} else if (base_info->desc != NULL && !base_info->is_failed) {
			struct ec_bdev_io_channel *ec_ch = spdk_io_channel_get_ctx(rebuild_ctx->rebuild_ch);
			if (ec_ch->base_channel[idx] != NULL) {
				rebuild_ctx->available_indices[num_available++] = idx;
			}
		}
	}

	if (num_available < k) {
		SPDK_ERRLOG("Not enough available blocks (%u < %u) for rebuild stripe %lu\n",
			    num_available, k, rebuild_ctx->current_stripe);
		
		/* Update superblock to mark rebuild as FAILED (revert REBUILDING to FAILED) */
		if (ec_bdev_sb_update_base_bdev_state(ec_bdev, rebuild_ctx->target_slot,
						       EC_SB_BASE_BDEV_FAILED)) {
			ec_bdev_write_superblock(ec_bdev, NULL, NULL);
		}
		
		if (rebuild_ctx->rebuild_done_cb) {
			rebuild_ctx->rebuild_done_cb(rebuild_ctx->rebuild_done_ctx, -ENODEV);
		}
		ec_bdev->rebuild_ctx = NULL;
		ec_rebuild_cleanup(rebuild_ctx);
		return;
	}

	/* Prepare data pointers - use first k available blocks */
	uint32_t strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
	for (i = 0; i < k; i++) {
		rebuild_ctx->data_ptrs[i] = rebuild_ctx->stripe_buf + i * strip_size_bytes;
	}

	/* Read k available blocks in parallel */
	rebuild_ctx->state = EC_REBUILD_STATE_READING;
	rebuild_ctx->reads_completed = 0;
	rebuild_ctx->reads_expected = k;

	for (i = 0; i < k; i++) {
		idx = rebuild_ctx->available_indices[i];
		base_info = &ec_bdev->base_bdev_info[idx];

		uint64_t pd_lba = (rebuild_ctx->current_stripe << ec_bdev->strip_size_shift) +
				  base_info->data_offset;

		struct ec_bdev_io_channel *ec_ch = spdk_io_channel_get_ctx(rebuild_ctx->rebuild_ch);
		rc = spdk_bdev_read_blocks(base_info->desc,
					   ec_ch->base_channel[idx],
					   rebuild_ctx->data_ptrs[i], pd_lba, ec_bdev->strip_size,
					   ec_rebuild_stripe_read_complete, rebuild_ctx);
		if (rc != 0) {
			if (rc == -ENOMEM) {
				/* Queue wait and retry */
				rebuild_ctx->wait_entry.bdev = spdk_bdev_desc_get_bdev(base_info->desc);
				rebuild_ctx->wait_entry.cb_fn = ec_rebuild_next_stripe;
				rebuild_ctx->wait_entry.cb_arg = rebuild_ctx;
				spdk_bdev_queue_io_wait(rebuild_ctx->wait_entry.bdev,
							ec_ch->base_channel[idx],
							&rebuild_ctx->wait_entry);
				return;
			} else {
				SPDK_ERRLOG("Failed to read block %u for rebuild stripe %lu: %s\n",
					    idx, rebuild_ctx->current_stripe, spdk_strerror(-rc));
				
				/* Update superblock to mark rebuild as FAILED (revert REBUILDING to FAILED) */
				if (ec_bdev_sb_update_base_bdev_state(ec_bdev, rebuild_ctx->target_slot,
								       EC_SB_BASE_BDEV_FAILED)) {
					ec_bdev_write_superblock(ec_bdev, NULL, NULL);
				}
				
				if (rebuild_ctx->rebuild_done_cb) {
					rebuild_ctx->rebuild_done_cb(rebuild_ctx->rebuild_done_ctx, rc);
				}
				ec_bdev->rebuild_ctx = NULL;
				ec_rebuild_cleanup(rebuild_ctx);
				return;
			}
		}
	}
	return;

handle_select_error:
	/* Update superblock to mark rebuild as FAILED (revert REBUILDING to FAILED) */
	if (ec_bdev->sb != NULL) {
		uint8_t sb_idx;
		for (sb_idx = 0; sb_idx < ec_bdev->sb->base_bdevs_size; sb_idx++) {
			if (ec_bdev->sb->base_bdevs[sb_idx].slot == rebuild_ctx->target_slot) {
				ec_bdev->sb->base_bdevs[sb_idx].state = EC_SB_BASE_BDEV_FAILED;
				ec_bdev->sb->seq_number++;
				ec_bdev->sb->crc = 0;
				ec_bdev->sb->crc = spdk_crc32c_update(ec_bdev->sb, ec_bdev->sb->length, 0);
				ec_bdev_write_superblock(ec_bdev, NULL, NULL);
				break;
			}
		}
	}

	if (rebuild_ctx->rebuild_done_cb) {
		rebuild_ctx->rebuild_done_cb(rebuild_ctx->rebuild_done_ctx, rc);
	}
	ec_bdev->rebuild_ctx = NULL;
	ec_rebuild_cleanup(rebuild_ctx);
}

/*
 * Start rebuild for a target base bdev
 */
int
ec_bdev_start_rebuild(struct ec_bdev *ec_bdev, struct ec_base_bdev_info *target_base_info,
		      void (*done_cb)(void *ctx, int status), void *done_ctx)
{
	struct ec_rebuild_context *rebuild_ctx;
	uint32_t strip_size_bytes;
	uint8_t i;

	if (ec_bdev == NULL || target_base_info == NULL) {
		return -EINVAL;
	}

	if (ec_bdev->rebuild_ctx != NULL) {
		SPDK_WARNLOG("Rebuild already in progress for EC bdev %s\n", ec_bdev->bdev.name);
		return -EBUSY;
	}

	if (!target_base_info->is_configured) {
		SPDK_ERRLOG("Target base bdev %s is not configured\n", target_base_info->name);
		return -ENODEV;
	}

	/* Allocate rebuild context */
	rebuild_ctx = calloc(1, sizeof(*rebuild_ctx));
	if (rebuild_ctx == NULL) {
		return -ENOMEM;
	}

	rebuild_ctx->target_base_info = target_base_info;
	rebuild_ctx->target_slot = 0;
	/* Find target slot */
	for (i = 0; i < ec_bdev->num_base_bdevs; i++) {
		if (&ec_bdev->base_bdev_info[i] == target_base_info) {
			rebuild_ctx->target_slot = i;
			break;
		}
	}

	/* Update superblock to mark base bdev as REBUILDING before starting rebuild */
	if (ec_bdev_sb_update_base_bdev_state(ec_bdev, rebuild_ctx->target_slot,
					       EC_SB_BASE_BDEV_REBUILDING)) {
		ec_bdev_write_superblock(ec_bdev, NULL, NULL);
	}

	/* Calculate total stripes */
	uint64_t total_strips = ec_bdev->bdev.blockcnt / ec_bdev->strip_size;
	rebuild_ctx->total_stripes = total_strips / ec_bdev->k;
	rebuild_ctx->current_stripe = 0;
	rebuild_ctx->state = EC_REBUILD_STATE_IDLE;
	rebuild_ctx->rebuild_done_cb = done_cb;
	rebuild_ctx->rebuild_done_ctx = done_ctx;
	rebuild_ctx->paused = false;

	/* Get I/O channel for rebuild */
	rebuild_ctx->rebuild_ch = spdk_get_io_channel(&ec_bdev->bdev);
	if (rebuild_ctx->rebuild_ch == NULL) {
		SPDK_ERRLOG("Failed to get I/O channel for rebuild\n");
		/* Revert superblock state from REBUILDING to FAILED */
		if (ec_bdev_sb_update_base_bdev_state(ec_bdev, rebuild_ctx->target_slot,
						       EC_SB_BASE_BDEV_FAILED)) {
			ec_bdev_write_superblock(ec_bdev, NULL, NULL);
		}
		free(rebuild_ctx);
		return -ENOMEM;
	}

	/* Allocate buffers */
	strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
	struct ec_bdev_io_channel *ec_ch = spdk_io_channel_get_ctx(rebuild_ctx->rebuild_ch);
	rebuild_ctx->stripe_buf = ec_get_rmw_stripe_buf(ec_ch, ec_bdev,
						       strip_size_bytes * ec_bdev->k);
	if (rebuild_ctx->stripe_buf == NULL) {
		SPDK_ERRLOG("Failed to allocate stripe buffer for rebuild\n");
		/* Revert superblock state from REBUILDING to FAILED */
		if (ec_bdev_sb_update_base_bdev_state(ec_bdev, rebuild_ctx->target_slot,
						       EC_SB_BASE_BDEV_FAILED)) {
			ec_bdev_write_superblock(ec_bdev, NULL, NULL);
		}
		spdk_put_io_channel(rebuild_ctx->rebuild_ch);
		free(rebuild_ctx);
		return -ENOMEM;
	}

	rebuild_ctx->recover_buf = spdk_dma_malloc(strip_size_bytes, ec_bdev->buf_alignment, NULL);
	if (rebuild_ctx->recover_buf == NULL) {
		SPDK_ERRLOG("Failed to allocate recovery buffer for rebuild\n");
		/* Revert superblock state from REBUILDING to FAILED */
		if (ec_bdev_sb_update_base_bdev_state(ec_bdev, rebuild_ctx->target_slot,
						       EC_SB_BASE_BDEV_FAILED)) {
			ec_bdev_write_superblock(ec_bdev, NULL, NULL);
		}
		ec_put_rmw_stripe_buf(ec_ch, rebuild_ctx->stripe_buf,
				      strip_size_bytes * ec_bdev->k);
		spdk_put_io_channel(rebuild_ctx->rebuild_ch);
		free(rebuild_ctx);
		return -ENOMEM;
	}

	/* Initialize wait entry */
	rebuild_ctx->wait_entry.bdev = NULL;
	rebuild_ctx->wait_entry.cb_fn = ec_rebuild_next_stripe;
	rebuild_ctx->wait_entry.cb_arg = rebuild_ctx;
	rebuild_ctx->wait_entry.dep_unblock = true;

	ec_bdev->rebuild_ctx = rebuild_ctx;

	SPDK_NOTICELOG("Starting rebuild for base bdev %s (slot %u) on EC bdev %s\n",
		       target_base_info->name, rebuild_ctx->target_slot, ec_bdev->bdev.name);

	/* Start rebuilding first stripe */
	spdk_thread_send_msg(spdk_get_thread(), ec_rebuild_next_stripe, rebuild_ctx);

	return 0;
}

/*
 * Stop rebuild
 */
void
ec_bdev_stop_rebuild(struct ec_bdev *ec_bdev)
{
	if (ec_bdev == NULL || ec_bdev->rebuild_ctx == NULL) {
		return;
	}

	SPDK_NOTICELOG("Stopping rebuild for EC bdev %s\n", ec_bdev->bdev.name);
	
	/* Update superblock to mark rebuild as FAILED (revert REBUILDING to FAILED) */
	if (ec_bdev->rebuild_ctx != NULL) {
		if (ec_bdev_sb_update_base_bdev_state(ec_bdev, ec_bdev->rebuild_ctx->target_slot,
						      EC_SB_BASE_BDEV_FAILED)) {
			ec_bdev_write_superblock(ec_bdev, NULL, NULL);
		}
	}
	
	ec_bdev->rebuild_ctx->paused = true;

	if (ec_bdev->rebuild_ctx->rebuild_done_cb) {
		ec_bdev->rebuild_ctx->rebuild_done_cb(ec_bdev->rebuild_ctx->rebuild_done_ctx, -ECANCELED);
	}

	ec_rebuild_cleanup(ec_bdev->rebuild_ctx);
	ec_bdev->rebuild_ctx = NULL;
}

/*
 * Check if rebuild is in progress
 */
bool
ec_bdev_is_rebuilding(struct ec_bdev *ec_bdev)
{
	return ec_bdev_get_active_rebuild(ec_bdev) != NULL;
}

/*
 * Get rebuild progress
 */
int
ec_bdev_get_rebuild_progress(struct ec_bdev *ec_bdev, uint64_t *current_stripe,
			     uint64_t *total_stripes)
{
	struct ec_rebuild_context *ctx;

	if (ec_bdev == NULL || current_stripe == NULL || total_stripes == NULL) {
		return -EINVAL;
	}

	ctx = ec_bdev_get_active_rebuild(ec_bdev);
	if (ctx == NULL) {
		return -ENODEV;
	}

	*current_stripe = ctx->current_stripe;
	*total_stripes = ctx->total_stripes;

	return 0;
}

