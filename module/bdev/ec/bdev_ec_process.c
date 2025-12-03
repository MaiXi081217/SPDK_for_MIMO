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
#include "spdk/json.h"

#define EC_OFFSET_BLOCKS_INVALID	UINT64_MAX
#define EC_BDEV_PROCESS_MAX_QD	16

#define EC_BDEV_PROCESS_WINDOW_SIZE_KB_DEFAULT	1024
#define EC_BDEV_PROCESS_MAX_BANDWIDTH_MB_SEC_DEFAULT	0

/* External reference to EC bdev module interface */
extern struct spdk_bdev_module g_ec_if;

/* ====================================================================
 * Forward declarations
 * ==================================================================== */
static void ec_bdev_process_thread_init(void *ctx);
static void ec_bdev_process_thread_run(struct ec_bdev_process *process);
static void ec_bdev_process_free(struct ec_bdev_process *process);
static int ec_bdev_ch_process_setup(struct ec_bdev_io_channel *ec_ch, struct ec_bdev_process *process);
static void ec_bdev_ch_process_cleanup(struct ec_bdev_io_channel *ec_ch);
static void ec_bdev_process_lock_window_range(struct ec_bdev_process *process);
static void ec_bdev_process_unlock_window_range(struct ec_bdev_process *process);
static void ec_bdev_process_window_range_locked(void *ctx, int status);
static void ec_bdev_process_window_range_unlocked(void *ctx, int status);
static int ec_bdev_submit_process_request(struct ec_bdev_process *process, uint64_t stripe_index);
static void ec_bdev_process_thread_run_wrapper(void *ctx);
static void ec_bdev_process_stripe_read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void ec_bdev_process_stripe_write_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void ec_bdev_process_update_superblock_on_error(struct ec_bdev_process *process);

/* ====================================================================
 * Structure definitions
 * ==================================================================== */

/* Per-stripe rebuild context (embedded in process request) */
struct ec_process_stripe_ctx {
	struct ec_bdev_process_request *process_req;
	uint64_t stripe_index;
	uint8_t data_indices[EC_MAX_K];
	uint8_t parity_indices[EC_MAX_P];
	uint8_t available_indices[EC_MAX_K];
	uint8_t num_available;
	uint8_t failed_frag_idx;
	uint8_t frag_map[EC_MAX_K + EC_MAX_P];
	uint8_t reads_completed;
	uint8_t reads_expected;
	uint8_t reads_submitted;  /* Track how many reads have been submitted (for retry logic) */
	bool reads_retry_needed;  /* Flag to indicate if retry is needed for remaining reads */
	unsigned char *stripe_buf;
	unsigned char *recover_buf;
	unsigned char *data_ptrs[EC_MAX_K];
};

/* ====================================================================
 * Helper functions
 * ==================================================================== */

/* Get active process helper */
static inline struct ec_bdev_process *
ec_bdev_get_active_process_internal(struct ec_bdev *ec_bdev)
{
	struct ec_bdev_process *process;

	if (ec_bdev == NULL) {
		return NULL;
	}

	process = ec_bdev->process;
	if (process == NULL || process->state >= EC_PROCESS_STATE_STOPPING) {
		return NULL;
	}

	return process;
}

/* ====================================================================
 * Channel management
 * ==================================================================== */

/* Cleanup process channel */
static void
ec_bdev_ch_process_cleanup(struct ec_bdev_io_channel *ec_ch)
{
	ec_ch->process.offset = EC_OFFSET_BLOCKS_INVALID;

	if (ec_ch->process.target_ch != NULL) {
		spdk_put_io_channel(ec_ch->process.target_ch);
		ec_ch->process.target_ch = NULL;
	}

	if (ec_ch->process.ch_processed != NULL) {
		free(ec_ch->process.ch_processed->base_channel);
		free(ec_ch->process.ch_processed);
		ec_ch->process.ch_processed = NULL;
	}
}

/* Setup process channel - simplified version for EC rebuild */
static int
ec_bdev_ch_process_setup(struct ec_bdev_io_channel *ec_ch, struct ec_bdev_process *process)
{
	struct ec_bdev *ec_bdev = process->ec_bdev;
	struct ec_bdev_io_channel *ec_ch_processed;
	struct ec_base_bdev_info *base_info;
	uint8_t available_base_bdevs = 0;

	ec_ch->process.offset = process->window_offset;

	/* Process must have a target */
	assert(process->target != NULL);

	/* Check if target disk is still available */
	if (process->target->desc == NULL) {
		SPDK_ERRLOG("Target disk '%s' descriptor is NULL - disk may have been removed\n",
			    process->target->name ? process->target->name : "unknown");
		goto err;
	}

	ec_ch->process.target_ch = spdk_bdev_get_io_channel(process->target->desc);
	if (ec_ch->process.target_ch == NULL) {
		SPDK_ERRLOG("Failed to get I/O channel for target disk '%s'\n",
			    process->target->name ? process->target->name : "unknown");
		goto err;
	}

	/* Allocate processed channel */
	ec_ch_processed = calloc(1, sizeof(*ec_ch_processed));
	if (ec_ch_processed == NULL) {
		goto err;
	}
	ec_ch->process.ch_processed = ec_ch_processed;

	ec_ch_processed->base_channel = calloc(ec_bdev->num_base_bdevs,
					       sizeof(*ec_ch_processed->base_channel));
	if (ec_ch_processed->base_channel == NULL) {
		goto err;
	}

	/* Setup base channels - for EC rebuild, we need at least k available base bdevs */
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		uint8_t slot = ec_bdev_base_bdev_slot(base_info);

		if (slot >= ec_bdev->num_base_bdevs) {
			SPDK_ERRLOG("Calculated slot %u exceeds num_base_bdevs %u\n",
				    slot, ec_bdev->num_base_bdevs);
			goto err;
		}

		if (base_info != process->target) {
			if (ec_ch->base_channel != NULL) {
				ec_ch_processed->base_channel[slot] = ec_ch->base_channel[slot];
				if (ec_ch_processed->base_channel[slot] != NULL) {
					available_base_bdevs++;
				}
			}
		} else {
			ec_ch_processed->base_channel[slot] = ec_ch->process.target_ch;
			available_base_bdevs++;
		}
	}

	/* For EC rebuild: need at least k available base bdevs (including target) */
	if (process->type == EC_PROCESS_REBUILD) {
		if (available_base_bdevs < ec_bdev->k) {
			SPDK_ERRLOG("Insufficient base bdevs available for rebuild on EC bdev '%s': "
				    "%u available, need at least %u\n",
				    ec_bdev->bdev.name, available_base_bdevs, ec_bdev->k);
			goto err;
		}
	}

	return 0;

err:
	ec_bdev_ch_process_cleanup(ec_ch);
	return -ENOMEM;
}

/* ====================================================================
 * Process lifecycle management
 * ==================================================================== */

/* Free process structure */
static void
ec_bdev_process_free(struct ec_bdev_process *process)
{
	if (process == NULL) {
		return;
	}

	/* Free request queue */
	while (!TAILQ_EMPTY(&process->requests)) {
		struct ec_bdev_process_request *req = TAILQ_FIRST(&process->requests);
		TAILQ_REMOVE(&process->requests, req, link);
		free(req);
	}

	/* Free finish actions */
	while (!TAILQ_EMPTY(&process->finish_actions)) {
		struct ec_process_finish_action *action = TAILQ_FIRST(&process->finish_actions);
		TAILQ_REMOVE(&process->finish_actions, action, link);
		free(action);
	}

	free(process);
}

/* Process thread init function */
static void
ec_bdev_process_thread_init(void *ctx)
{
	struct ec_bdev_process *process = ctx;
	
	/* Set thread in process */
	process->thread = spdk_get_thread();
	
	/* Get I/O channel for this thread */
	process->io_ch = spdk_get_io_channel(process->ec_bdev);
	if (process->io_ch == NULL) {
		SPDK_ERRLOG("Failed to get I/O channel for EC process on bdev %s\n",
			    process->ec_bdev->bdev.name);
		process->status = -ENOMEM;
		ec_bdev_process_thread_run(process);
		return;
	}
	process->ec_ch = spdk_io_channel_get_ctx(process->io_ch);
	
	/* Setup process channel */
	if (ec_bdev_ch_process_setup(process->ec_ch, process) != 0) {
		SPDK_ERRLOG("Failed to setup process channel for EC bdev %s\n",
			    process->ec_bdev->bdev.name);
		spdk_put_io_channel(process->io_ch);
		process->io_ch = NULL;
		process->status = -ENOMEM;
		ec_bdev_process_thread_run(process);
		return;
	}
	
	process->state = EC_PROCESS_STATE_RUNNING;
	
	/* Show rebuild notification when rebuild starts (aligned with RAID framework) */
	if (process->type == EC_PROCESS_REBUILD && process->target != NULL) {
		uint8_t target_slot = ec_bdev_base_bdev_slot(process->target);
		uint64_t total_size = process->ec_bdev->bdev.blockcnt;
		uint64_t current_offset = process->window_offset;
		double percent = 0.0;
		
		if (total_size > 0) {
			percent = (double)current_offset * 100.0 / (double)total_size;
		}
		
		SPDK_NOTICELOG("\n");
		SPDK_NOTICELOG("===========================================================\n");
		SPDK_NOTICELOG("REBUILD IN PROGRESS: EC bdev '%s'\n", process->ec_bdev->bdev.name);
		SPDK_NOTICELOG("Target disk: %s (slot %u)\n", 
			       process->target->name ? process->target->name : "unknown", target_slot);
		SPDK_NOTICELOG("Rebuild started at offset: %lu blocks (%.2f%%)\n",
			       current_offset, percent);
		SPDK_NOTICELOG("Total size: %lu blocks\n", total_size);
		SPDK_NOTICELOG("State: %s\n", ec_rebuild_state_to_str(process->rebuild_state));
		SPDK_NOTICELOG("Rebuild is running in background. This may take a while...\n");
		SPDK_NOTICELOG("Check progress: bdev_ec_get_bdevs\n");
		SPDK_NOTICELOG("===========================================================\n");
		SPDK_NOTICELOG("\n");
	}
	
	ec_bdev_process_thread_run(process);
}

/* ====================================================================
 * Window mechanism
 * ==================================================================== */

/* Window range locked callback */
static void
ec_bdev_process_window_range_locked(void *ctx, int status)
{
	struct ec_bdev_process *process = ctx;

	if (status != 0) {
		SPDK_ERRLOG("Failed to lock LBA range for EC bdev %s: %s\n",
			    process->ec_bdev->bdev.name, spdk_strerror(-status));
		/* Will be handled in finish */
		process->status = status;
		process->error_status = status;
		return;
	}

	process->window_range_locked = true;
	process->rebuild_state = EC_REBUILD_STATE_READING;

	/* Initialize window stripe tracking - aligned with RAID framework
	 * window_remaining starts at 0 and increases as requests are submitted
	 * This ensures accurate counting even if some submissions fail
	 */
	process->window_stripes_submitted = 0;
	process->window_remaining = 0;
	if (process->ec_bdev->strip_size > 0 && process->ec_bdev->k > 0) {
		uint64_t window_strips = process->window_size / process->ec_bdev->strip_size;
		process->window_stripes_total = window_strips / process->ec_bdev->k;
	} else {
		SPDK_ERRLOG("Invalid strip_size (%u) or k (%u) for EC bdev %s\n",
			    process->ec_bdev->strip_size, process->ec_bdev->k,
			    process->ec_bdev->bdev.name);
		process->status = -EINVAL;
		process->error_status = -EINVAL;
		return;
	}

	/* Start processing this window */
	ec_bdev_process_thread_run(process);
}

/* Window range unlocked callback */
static void
ec_bdev_process_window_range_unlocked(void *ctx, int status)
{
	struct ec_bdev_process *process = ctx;

	if (status != 0) {
		SPDK_ERRLOG("Failed to unlock LBA range for EC bdev %s: %s\n",
			    process->ec_bdev->bdev.name, spdk_strerror(-status));
		/* Will be handled in finish */
		process->status = status;
		process->error_status = status;
		return;
	}

	/* Verify window is truly complete before updating offset
	 * Simplified check: rely on window_remaining == 0 (set in request_complete)
	 * The queue check is redundant since window_remaining tracks all submitted requests
	 */
	if (process->window_remaining != 0) {
		SPDK_ERRLOG("Window unlock attempted but not complete: remaining=%lu\n",
			    process->window_remaining);
		/* This should not happen - window completion check should prevent this */
		/* Continue processing to complete remaining work */
		ec_bdev_process_thread_run(process);
		return;
	}

	process->window_range_locked = false;
	process->window_offset += process->window_size;

	/* Update superblock periodically (every window) for rebuild */
	if (process->type == EC_PROCESS_REBUILD &&
	    process->ec_bdev->superblock_enabled &&
	    process->ec_bdev->sb != NULL &&
	    process->target != NULL) {
		/* Keep state as REBUILDING during rebuild, will be updated to CONFIGURED on completion */
		/* Write superblock asynchronously (non-blocking) to persist progress */
		ec_bdev_write_superblock(process->ec_bdev, NULL, NULL);
	}

	/* Progress report (every window) */
	if (process->type == EC_PROCESS_REBUILD) {
		uint64_t total_blocks = process->ec_bdev->bdev.blockcnt;
		uint64_t completed_blocks = process->window_offset;
		uint32_t percent = (uint32_t)((completed_blocks * 100) / total_blocks);
		SPDK_NOTICELOG("EC rebuild progress for bdev %s: %lu/%lu blocks (%u%%)\n",
			       process->ec_bdev->bdev.name, completed_blocks, total_blocks, percent);
	}

	/* Check if rebuild is complete */
	if (process->window_offset >= process->ec_bdev->bdev.blockcnt) {
		/* Rebuild complete - update superblock to CONFIGURED */
		if (process->type == EC_PROCESS_REBUILD &&
		    process->ec_bdev->superblock_enabled &&
		    process->ec_bdev->sb != NULL &&
		    process->target != NULL) {
			uint8_t target_slot = ec_bdev_base_bdev_slot(process->target);
			if (ec_bdev_sb_update_base_bdev_state(process->ec_bdev, target_slot,
							      EC_SB_BASE_BDEV_CONFIGURED)) {
				ec_bdev_write_superblock(process->ec_bdev, NULL, NULL);
			}
		}
		/* Process complete */
		process->status = 0;
		process->error_status = 0;
		/* Will call finish */
		return;
	}

	/* Continue to next window */
	ec_bdev_process_lock_window_range(process);
}

/* Unlock window range */
static void
ec_bdev_process_unlock_window_range(struct ec_bdev_process *process)
{
	int rc;

	assert(process->window_range_locked == true);

	/* Use window_size (actual locked size) not max_window_size */
	rc = spdk_bdev_unquiesce_range(&process->ec_bdev->bdev, &g_ec_if,
					process->window_offset, process->window_size,
					ec_bdev_process_window_range_unlocked, process);
	if (rc != 0) {
		ec_bdev_process_window_range_unlocked(process, rc);
	}
}

/* Lock window range */
static void
ec_bdev_process_lock_window_range(struct ec_bdev_process *process)
{
	int rc;
	uint64_t window_size;

	/* Calculate window size */
	window_size = process->max_window_size;
	if (process->window_offset + window_size > process->ec_bdev->bdev.blockcnt) {
		window_size = process->ec_bdev->bdev.blockcnt - process->window_offset;
	}

	process->window_size = window_size;

	rc = spdk_bdev_quiesce_range(&process->ec_bdev->bdev, &g_ec_if,
				     process->window_offset, window_size,
				     ec_bdev_process_window_range_locked, process);
	if (rc != 0) {
		ec_bdev_process_window_range_locked(process, rc);
	}
}

/* ====================================================================
 * Error handling helpers
 * ==================================================================== */

/* Update superblock on rebuild error - centralized error handling */
static void
ec_bdev_process_update_superblock_on_error(struct ec_bdev_process *process)
{
	if (process->type == EC_PROCESS_REBUILD &&
	    process->ec_bdev->superblock_enabled &&
	    process->ec_bdev->sb != NULL &&
	    process->target != NULL) {
		uint8_t target_slot = ec_bdev_base_bdev_slot(process->target);
		if (ec_bdev_sb_update_base_bdev_state(process->ec_bdev, target_slot,
						      EC_SB_BASE_BDEV_FAILED)) {
			ec_bdev_write_superblock(process->ec_bdev, NULL, NULL);
		}
	}
}

/* ====================================================================
 * Request processing
 * ==================================================================== */

/* Process request complete callback - unified with RAID framework logic */
void
ec_bdev_process_request_complete(struct ec_bdev_process_request *process_req, int status)
{
	struct ec_bdev_process *process;
	struct ec_process_stripe_ctx *stripe_ctx;

	if (process_req == NULL) {
		return;
	}

	process = process_req->process;
	if (process == NULL) {
		free(process_req);
		return;
	}

	assert(spdk_get_thread() == process->thread);

	/* Unified error handling: set error status atomically (aligned with RAID) */
	if (status != 0) {
		process->window_status = status;
		process->error_status = status;
	}

	/* Get stripe context from process request */
	stripe_ctx = (struct ec_process_stripe_ctx *)(process_req + 1);

	/* Free buffers */
	if (stripe_ctx->stripe_buf != NULL) {
		struct ec_bdev *ec_bdev = process->ec_bdev;
		uint32_t strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
		ec_put_rmw_stripe_buf(process->ec_ch, stripe_ctx->stripe_buf,
				     strip_size_bytes * ec_bdev->k);
	}

	if (stripe_ctx->recover_buf != NULL) {
		spdk_dma_free(stripe_ctx->recover_buf);
	}

	/* Remove from queue - verify it's in queue first to avoid crashes */
	bool was_in_queue = false;
	struct ec_bdev_process_request *req_check;
	TAILQ_FOREACH(req_check, &process->requests, link) {
		if (req_check == process_req) {
			was_in_queue = true;
			break;
		}
	}
	if (was_in_queue) {
		TAILQ_REMOVE(&process->requests, process_req, link);
	} else {
		/* Request not in queue - this should not happen in normal flow
		 * but can occur in edge cases (e.g., retry logic). Log and continue.
		 */
		SPDK_WARNLOG("Request not in queue during completion (stripe %lu) - may be retry case\n",
			     stripe_ctx->stripe_index);
	}

	/* Update window progress - decrement window_remaining for completed stripe
	 * Only decrement if request was in queue (was counted in window_remaining)
	 */
	if (was_in_queue) {
		assert(process->window_remaining > 0);
		process->window_remaining--;
	}

	/* Free request */
	free(process_req);

	/* Check if window is complete (aligned with RAID framework) */
	if (process->window_remaining == 0) {
		/* Unified error handling: error_status and window_status should be in sync
		 * For rebuild, prefer error_status; for other processes, use window_status
		 */
		int error_to_report = 0;
		if (process->type == EC_PROCESS_REBUILD && process->error_status != 0) {
			error_to_report = process->error_status;
		} else if (process->window_status != 0) {
			error_to_report = process->window_status;
		}

		if (error_to_report != 0) {
			/* Error occurred - will be handled in thread_run */
			process->status = error_to_report;
			ec_bdev_process_thread_run(process);
			return;
		}

		/* Window complete - update channel offsets and unlock window
		 * Note: EC uses single-threaded process, so we only need to update
		 * the process channel's offset, not all channels like RAID does
		 */
		if (process->ec_ch != NULL) {
			process->ec_ch->process.offset = process->window_offset + process->window_size;
		}
		
		/* Unlock and move to next window */
		ec_bdev_process_unlock_window_range(process);
		return;
	}

	/* Continue processing next stripe */
	ec_bdev_process_thread_run(process);
}

/* Stripe write complete callback */
static void
ec_bdev_process_stripe_write_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ec_bdev_process_request *process_req = cb_arg;
	struct ec_bdev_process *process;
	struct ec_process_stripe_ctx *stripe_ctx;
	struct ec_bdev *ec_bdev;

	spdk_bdev_free_io(bdev_io);

	if (process_req == NULL) {
		return;
	}

	process = process_req->process;
	if (process == NULL) {
		ec_bdev_process_request_complete(process_req, -EINVAL);
		return;
	}

	ec_bdev = process->ec_bdev;
	stripe_ctx = (struct ec_process_stripe_ctx *)(process_req + 1);

	if (!success) {
		SPDK_ERRLOG("Rebuild write failed for stripe %lu on EC bdev %s\n",
			    stripe_ctx->stripe_index, ec_bdev->bdev.name);
		
		/* Update superblock and error status */
		ec_bdev_process_update_superblock_on_error(process);
		process->status = -EIO;
		process->error_status = -EIO;
		ec_bdev_process_request_complete(process_req, -EIO);
		return;
	}

	/* Write completed successfully - complete this request */
	process->rebuild_state = EC_REBUILD_STATE_IDLE;
	ec_bdev_process_request_complete(process_req, 0);
}

/* Stripe read complete callback */
static void
ec_bdev_process_stripe_read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ec_bdev_process_request *process_req = cb_arg;
	struct ec_bdev_process *process;
	struct ec_process_stripe_ctx *stripe_ctx;
	struct ec_bdev *ec_bdev;
	unsigned char *recover_ptrs[EC_MAX_P];
	uint8_t failed_frag_list[EC_MAX_P];
	int rc;

	spdk_bdev_free_io(bdev_io);

	if (process_req == NULL) {
		return;
	}

	process = process_req->process;
	if (process == NULL) {
		ec_bdev_process_request_complete(process_req, -EINVAL);
		return;
	}

	ec_bdev = process->ec_bdev;
	stripe_ctx = (struct ec_process_stripe_ctx *)(process_req + 1);

	if (!success) {
		SPDK_ERRLOG("Rebuild read failed for stripe %lu on EC bdev %s\n",
			    stripe_ctx->stripe_index, ec_bdev->bdev.name);
		
		/* Update superblock and error status */
		ec_bdev_process_update_superblock_on_error(process);
		process->status = -EIO;
		process->error_status = -EIO;
		ec_bdev_process_request_complete(process_req, -EIO);
		return;
	}

	stripe_ctx->reads_completed++;

	/* Check if all reads completed */
	if (stripe_ctx->reads_completed >= stripe_ctx->reads_expected) {
		/* If retry was needed, check if we need to submit remaining reads */
		if (stripe_ctx->reads_retry_needed && stripe_ctx->reads_expected < ec_bdev->k) {
			/* Partial reads completed - need to submit remaining reads */
			uint8_t remaining_reads = ec_bdev->k - stripe_ctx->reads_expected;
			uint8_t start_idx = stripe_ctx->reads_expected;
			int rc_retry;
			
			/* Submit remaining reads */
			for (uint8_t j = 0; j < remaining_reads; j++) {
				uint8_t idx_retry = stripe_ctx->available_indices[start_idx + j];
				struct ec_base_bdev_info *base_info_retry = &ec_bdev->base_bdev_info[idx_retry];
				uint64_t pd_lba_retry = (stripe_ctx->stripe_index << ec_bdev->strip_size_shift) +
							base_info_retry->data_offset;
				
				rc_retry = spdk_bdev_read_blocks(base_info_retry->desc,
								process->ec_ch->process.ch_processed->base_channel[idx_retry],
								stripe_ctx->data_ptrs[start_idx + j], pd_lba_retry, ec_bdev->strip_size,
								ec_bdev_process_stripe_read_complete, process_req);
				if (rc_retry != 0) {
					if (rc_retry == -ENOMEM) {
						/* Queue wait and retry again */
						process_req->ec_io.waitq_entry.bdev = spdk_bdev_desc_get_bdev(base_info_retry->desc);
						process_req->ec_io.waitq_entry.cb_fn = ec_bdev_process_thread_run_wrapper;
						process_req->ec_io.waitq_entry.cb_arg = process;
						process_req->ec_io.waitq_entry.dep_unblock = true;
						spdk_bdev_queue_io_wait(process_req->ec_io.waitq_entry.bdev,
									process->ec_ch->process.ch_processed->base_channel[idx_retry],
									&process_req->ec_io.waitq_entry);
						/* Update expected reads to include what we just tried to submit */
						stripe_ctx->reads_expected = start_idx + j;
						return; /* Will retry later */
					} else {
						SPDK_ERRLOG("Failed to read block %u for rebuild stripe %lu (retry): %s\n",
							    idx_retry, stripe_ctx->stripe_index, spdk_strerror(-rc_retry));
						ec_bdev_process_update_superblock_on_error(process);
						process->status = -EIO;
						process->error_status = -EIO;
						ec_bdev_process_request_complete(process_req, -EIO);
						return;
					}
				}
			}
			/* All remaining reads submitted - update expected count
			 * CRITICAL: Do NOT add to queue here - the request should already be in queue
			 * from the initial submission. If it's not in queue, it means the initial
			 * submission failed completely (all reads failed), which should have been
			 * handled in the error path.
			 * 
			 * Adding to queue here would cause double-counting if the request was
			 * already added during initial submission (even if some reads failed).
			 */
			stripe_ctx->reads_expected = ec_bdev->k;
			stripe_ctx->reads_retry_needed = false;
			
			/* Verify request is in queue (should always be true at this point) */
			bool in_queue = false;
			struct ec_bdev_process_request *req_check;
			TAILQ_FOREACH(req_check, &process->requests, link) {
				if (req_check == process_req) {
					in_queue = true;
					break;
				}
			}
			if (!in_queue) {
				/* This should not happen - if we reach here, it means the request
				 * was never added to queue, which indicates a logic error.
				 * Add it now to prevent the request from being lost, but log an error.
				 */
				SPDK_ERRLOG("Request not in queue during retry completion - adding now (logic error)\n");
				TAILQ_INSERT_TAIL(&process->requests, process_req, link);
				process->window_remaining++;
				process->window_stripes_submitted++;
			}
			return; /* Wait for all reads to complete */
		}
		
		/* All reads completed - proceed with decoding */
		process->rebuild_state = EC_REBUILD_STATE_DECODING;

		uint32_t strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
		failed_frag_list[0] = stripe_ctx->failed_frag_idx;
		recover_ptrs[0] = stripe_ctx->recover_buf;

		/* Decode the failed fragment */
		rc = ec_decode_stripe(ec_bdev, stripe_ctx->data_ptrs, recover_ptrs,
				      failed_frag_list, 1, strip_size_bytes);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to decode stripe %lu during rebuild: %s\n",
				    stripe_ctx->stripe_index, spdk_strerror(-rc));
			
			/* Update superblock and error status */
			ec_bdev_process_update_superblock_on_error(process);
			process->status = rc;
			process->error_status = rc;
			ec_bdev_process_request_complete(process_req, rc);
			return;
		}

		/* Write recovered data to target base bdev */
		process->rebuild_state = EC_REBUILD_STATE_WRITING;
		uint64_t pd_lba = (stripe_ctx->stripe_index << ec_bdev->strip_size_shift) +
				  process->target->data_offset;

		rc = spdk_bdev_write_blocks(process->target->desc,
					    process->ec_ch->process.target_ch,
					    stripe_ctx->recover_buf, pd_lba, ec_bdev->strip_size,
					    ec_bdev_process_stripe_write_complete, process_req);
		if (rc != 0) {
			if (rc == -ENOMEM) {
				/* Queue wait and retry - use waitq_entry from ec_io (each request has its own) */
				process_req->ec_io.waitq_entry.bdev = spdk_bdev_desc_get_bdev(process->target->desc);
				process_req->ec_io.waitq_entry.cb_fn = ec_bdev_process_thread_run_wrapper;
				process_req->ec_io.waitq_entry.cb_arg = process;
				process_req->ec_io.waitq_entry.dep_unblock = true;
				spdk_bdev_queue_io_wait(process_req->ec_io.waitq_entry.bdev,
							process->ec_ch->process.target_ch,
							&process_req->ec_io.waitq_entry);
				return;
			} else {
				SPDK_ERRLOG("Failed to write stripe %lu during rebuild: %s\n",
					    stripe_ctx->stripe_index, spdk_strerror(-rc));
				
				/* Update superblock and error status */
				ec_bdev_process_update_superblock_on_error(process);
				process->status = rc;
				process->error_status = rc;
				ec_bdev_process_request_complete(process_req, rc);
				return;
			}
		}
	}
}

/* Submit process request for a stripe */
static int
ec_bdev_submit_process_request(struct ec_bdev_process *process, uint64_t stripe_index)
{
	struct ec_bdev *ec_bdev = process->ec_bdev;
	struct ec_bdev_process_request *process_req;
	struct ec_process_stripe_ctx *stripe_ctx;
	struct ec_base_bdev_info *base_info;
	uint8_t data_indices[EC_MAX_K];
	uint8_t parity_indices[EC_MAX_P];
	uint8_t i, idx;
	uint8_t num_available = 0;
	uint32_t strip_size_bytes;
	int rc;

	/* Allocate process request with embedded stripe context */
	process_req = calloc(1, sizeof(*process_req) + sizeof(*stripe_ctx));
	if (process_req == NULL) {
		return -ENOMEM;
	}

	process_req->process = process;
	process_req->target = process->target;
	process_req->target_ch = process->ec_ch->process.target_ch;

	stripe_ctx = (struct ec_process_stripe_ctx *)(process_req + 1);
	stripe_ctx->process_req = process_req;
	stripe_ctx->stripe_index = stripe_index;

	/* Get data and parity indices for this stripe */
	if (ec_bdev->selection_config.select_fn != NULL) {
		if (ec_bdev->selection_config.wear_leveling_enabled &&
		    ec_bdev->selection_config.select_fn == ec_select_base_bdevs_wear_leveling) {
			rc = ec_selection_bind_group_profile(ec_bdev, stripe_index);
			if (spdk_unlikely(rc != 0)) {
				SPDK_ERRLOG("Failed to bind stripe %lu to wear profile during rebuild on EC bdev %s: %s\n",
					    stripe_index, ec_bdev->bdev.name, spdk_strerror(-rc));
				goto handle_select_error;
			}
		}
		rc = ec_bdev->selection_config.select_fn(ec_bdev, stripe_index,
							data_indices, parity_indices);
	} else {
		rc = ec_select_base_bdevs_default(ec_bdev, stripe_index,
						  data_indices, parity_indices);
	}
	if (rc != 0) {
		SPDK_ERRLOG("Failed to select base bdevs for rebuild stripe %lu on EC bdev %s: %s\n",
			    stripe_index, ec_bdev->bdev.name, spdk_strerror(-rc));
		goto handle_select_error;
	}

	/* Copy indices to stripe context */
	memcpy(stripe_ctx->data_indices, data_indices, sizeof(data_indices));
	memcpy(stripe_ctx->parity_indices, parity_indices, sizeof(parity_indices));

	/* Find available base bdevs and determine failed fragment index */
	memset(stripe_ctx->frag_map, 0xFF, sizeof(stripe_ctx->frag_map));
	uint8_t k = ec_bdev->k;
	uint8_t p = ec_bdev->p;
	uint8_t target_slot = ec_bdev_base_bdev_slot(process->target);

	for (i = 0; i < k; i++) {
		idx = data_indices[i];
		stripe_ctx->frag_map[idx] = i;
		base_info = &ec_bdev->base_bdev_info[idx];
		if (idx == target_slot) {
			stripe_ctx->failed_frag_idx = i;
		} else if (base_info->desc != NULL && !base_info->is_failed) {
			if (process->ec_ch->process.ch_processed != NULL &&
			    process->ec_ch->process.ch_processed->base_channel[idx] != NULL) {
				stripe_ctx->available_indices[num_available++] = idx;
			}
		}
	}
	for (i = 0; i < p; i++) {
		idx = parity_indices[i];
		stripe_ctx->frag_map[idx] = k + i;
		base_info = &ec_bdev->base_bdev_info[idx];
		if (idx == target_slot) {
			stripe_ctx->failed_frag_idx = k + i;
		} else if (base_info->desc != NULL && !base_info->is_failed) {
			if (process->ec_ch->process.ch_processed != NULL &&
			    process->ec_ch->process.ch_processed->base_channel[idx] != NULL) {
				stripe_ctx->available_indices[num_available++] = idx;
			}
		}
	}

	stripe_ctx->num_available = num_available;

	if (num_available < k) {
		SPDK_ERRLOG("Not enough available blocks (%u < %u) for rebuild stripe %lu\n",
			    num_available, k, stripe_index);
		rc = -ENODEV;
		goto handle_select_error;
	}

	/* Allocate buffers */
	strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
	stripe_ctx->stripe_buf = ec_get_rmw_stripe_buf(process->ec_ch, ec_bdev,
						      strip_size_bytes * k);
	if (stripe_ctx->stripe_buf == NULL) {
		SPDK_ERRLOG("Failed to allocate stripe buffer for rebuild\n");
		rc = -ENOMEM;
		goto handle_select_error;
	}

	stripe_ctx->recover_buf = spdk_dma_malloc(strip_size_bytes, ec_bdev->buf_alignment, NULL);
	if (stripe_ctx->recover_buf == NULL) {
		SPDK_ERRLOG("Failed to allocate recovery buffer for rebuild\n");
		ec_put_rmw_stripe_buf(process->ec_ch, stripe_ctx->stripe_buf,
				     strip_size_bytes * k);
		rc = -ENOMEM;
		goto handle_select_error;
	}

	/* Prepare data pointers */
	for (i = 0; i < k; i++) {
		stripe_ctx->data_ptrs[i] = stripe_ctx->stripe_buf + i * strip_size_bytes;
	}

	/* Read k available blocks in parallel */
	process->rebuild_state = EC_REBUILD_STATE_READING;
	stripe_ctx->reads_completed = 0;
	stripe_ctx->reads_expected = k;
	stripe_ctx->reads_submitted = 0;
	stripe_ctx->reads_retry_needed = false;

	/* Track how many reads we've successfully submitted */
	uint8_t reads_submitted = 0;

	for (i = 0; i < k; i++) {
		idx = stripe_ctx->available_indices[i];
		base_info = &ec_bdev->base_bdev_info[idx];

		uint64_t pd_lba = (stripe_index << ec_bdev->strip_size_shift) +
				  base_info->data_offset;

		rc = spdk_bdev_read_blocks(base_info->desc,
					   process->ec_ch->process.ch_processed->base_channel[idx],
					   stripe_ctx->data_ptrs[i], pd_lba, ec_bdev->strip_size,
					   ec_bdev_process_stripe_read_complete, process_req);
		if (rc != 0) {
			if (rc == -ENOMEM) {
				if (reads_submitted > 0) {
					/* Some reads were already submitted - must preserve request for completion
					 * CRITICAL: Add to queue and update window_remaining to prevent:
					 * 1. Request loss (if thread_run retries, it would allocate a new request)
					 * 2. Window completion error (window_remaining would be incorrect)
					 * 
					 * The request will be retried in read_complete callback when partial reads finish,
					 * not in thread_run. This ensures the same request is reused.
					 */
					process_req->ec_io.waitq_entry.bdev = spdk_bdev_desc_get_bdev(base_info->desc);
					process_req->ec_io.waitq_entry.cb_fn = ec_bdev_process_thread_run_wrapper;
					process_req->ec_io.waitq_entry.cb_arg = process;
					process_req->ec_io.waitq_entry.dep_unblock = true;
					spdk_bdev_queue_io_wait(process_req->ec_io.waitq_entry.bdev,
								process->ec_ch->process.ch_processed->base_channel[idx],
								&process_req->ec_io.waitq_entry);
					/* Update expected reads to match what we've actually submitted */
					stripe_ctx->reads_expected = reads_submitted;
					stripe_ctx->reads_submitted = reads_submitted;
					stripe_ctx->reads_retry_needed = true;  /* Mark that retry is needed */
					
					/* Add to queue and update window_remaining */
					TAILQ_INSERT_TAIL(&process->requests, process_req, link);
					process->window_remaining++;
					process->window_stripes_submitted++;
					/* Will retry remaining reads via read_complete callback when partial reads finish */
					return 0;
				} else {
					/* No reads were submitted (all failed with ENOMEM):
					 * - Free buffers and request to avoid memory leak
					 * - Return -ENOMEM so thread_run can retry later
					 * - Since no I/O was submitted, it's safe to allocate a new request on retry
					 */
					if (stripe_ctx->stripe_buf != NULL) {
						strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
						ec_put_rmw_stripe_buf(process->ec_ch, stripe_ctx->stripe_buf,
								     strip_size_bytes * k);
					}
					if (stripe_ctx->recover_buf != NULL) {
						spdk_dma_free(stripe_ctx->recover_buf);
					}
					free(process_req);
					return -ENOMEM; /* Will retry via thread_run when resources become available */
				}
			} else {
				SPDK_ERRLOG("Failed to read block %u for rebuild stripe %lu: %s\n",
					    idx, stripe_index, spdk_strerror(-rc));
				/* Need to cancel any already-submitted reads - but we can't easily do that
				 * So we'll let them complete and fail the request in the completion callback
				 * 
				 * CRITICAL: If some reads were submitted, we must add to queue and update
				 * window_remaining so the completion callback can handle the error properly
				 */
				rc = -EIO;
				/* Update expected reads to match what we've submitted */
				stripe_ctx->reads_expected = reads_submitted;
				/* Add to queue so completion can handle the error */
				TAILQ_INSERT_TAIL(&process->requests, process_req, link);
				/* Update window_remaining - stripe was attempted (even if failed) */
				process->window_remaining++;
				process->window_stripes_submitted++;
				/* Set error status - completion callback will handle cleanup */
				process->status = rc;
				process->error_status = rc;
				return rc;
			}
		}
		reads_submitted++;
		stripe_ctx->reads_submitted++;
	}

	/* All reads submitted successfully - add to request queue and update counters
	 * Aligned with RAID framework: window_remaining increases when request is submitted
	 */
	TAILQ_INSERT_TAIL(&process->requests, process_req, link);
	process->window_remaining++;
	process->window_stripes_submitted++;
	return 0;

handle_select_error:
	/* Update superblock and error status */
	ec_bdev_process_update_superblock_on_error(process);

	/* Free allocated buffers */
	if (stripe_ctx->stripe_buf != NULL) {
		strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
		ec_put_rmw_stripe_buf(process->ec_ch, stripe_ctx->stripe_buf,
				     strip_size_bytes * k);
	}
	if (stripe_ctx->recover_buf != NULL) {
		spdk_dma_free(stripe_ctx->recover_buf);
	}

	free(process_req);
	process->status = rc;
	process->error_status = rc;
	return rc;
}

/* ====================================================================
 * Thread run logic
 * ==================================================================== */

/* Wrapper function for io_wait callback (converts void* to ec_bdev_process*) */
static void
ec_bdev_process_thread_run_wrapper(void *ctx)
{
	struct ec_bdev_process *process = ctx;
	ec_bdev_process_thread_run(process);
}

/* Process thread run function - process window stripes */
static void
ec_bdev_process_thread_run(struct ec_bdev_process *process)
{
	struct ec_bdev *ec_bdev = process->ec_bdev;
	uint64_t current_stripe;
	uint64_t window_end_stripe;
	uint64_t total_stripes;
	uint32_t pending_requests;

	/* Check if we need to finish - use error_status for rebuild, status for others */
	int error_to_check = (process->type == EC_PROCESS_REBUILD && process->error_status != 0) ?
			     process->error_status : process->status;
	if (error_to_check != 0 || process->state >= EC_PROCESS_STATE_STOPPING) {
		/* Process finished or error occurred */
		if (error_to_check == 0 && process->window_offset >= ec_bdev->bdev.blockcnt) {
			/* Rebuild complete */
			SPDK_NOTICELOG("Rebuild completed for base bdev %s (slot %u) on EC bdev %s\n",
				       process->target->name,
				       ec_bdev_base_bdev_slot(process->target),
				       ec_bdev->bdev.name);

			/* Turn off LED for all healthy disks */
			ec_bdev_set_healthy_disks_led(ec_bdev, SPDK_VMD_LED_STATE_OFF);

			/* Call completion callback */
			if (process->rebuild_done_cb != NULL) {
				process->rebuild_done_cb(process->rebuild_done_ctx, 0);
			}

			/* Cleanup */
			if (process->ec_ch != NULL) {
				ec_bdev_ch_process_cleanup(process->ec_ch);
			}
			if (process->io_ch != NULL) {
				spdk_put_io_channel(process->io_ch);
				process->io_ch = NULL;
			}
			process->state = EC_PROCESS_STATE_STOPPED;
			ec_bdev->process = NULL;
			ec_bdev_process_free(process);
		} else if (process->status != 0) {
			/* Error occurred */
			SPDK_ERRLOG("EC process failed for bdev %s: %s\n",
				    ec_bdev->bdev.name, spdk_strerror(-process->status));

			/* Call completion callback */
			if (process->rebuild_done_cb != NULL) {
				process->rebuild_done_cb(process->rebuild_done_ctx, process->status);
			}

			/* Cleanup */
			if (process->ec_ch != NULL) {
				ec_bdev_ch_process_cleanup(process->ec_ch);
			}
			if (process->io_ch != NULL) {
				spdk_put_io_channel(process->io_ch);
				process->io_ch = NULL;
			}
			process->state = EC_PROCESS_STATE_STOPPED;
			ec_bdev->process = NULL;
			ec_bdev_process_free(process);
		}
		return;
	}

	/* Check if window is locked */
	if (!process->window_range_locked) {
		/* Lock first window */
		ec_bdev_process_lock_window_range(process);
		return;
	}

	/* Count pending requests */
	pending_requests = 0;
	struct ec_bdev_process_request *req;
	TAILQ_FOREACH(req, &process->requests, link) {
		pending_requests++;
	}

	/* Check queue depth limit */
	if (pending_requests >= EC_BDEV_PROCESS_MAX_QD) {
		/* Queue is full, wait for completion */
		return;
	}

	/* Calculate stripe range for current window
	 * Defensive check: ensure strip_size and k are valid to prevent division by zero
	 */
	if (ec_bdev->strip_size == 0 || ec_bdev->k == 0) {
		SPDK_ERRLOG("Invalid strip_size (%u) or k (%u) for EC bdev %s\n",
			    ec_bdev->strip_size, ec_bdev->k, ec_bdev->bdev.name);
		process->status = -EINVAL;
		process->error_status = -EINVAL;
		return;
	}
	
	total_stripes = (ec_bdev->bdev.blockcnt / ec_bdev->strip_size) / ec_bdev->k;
	current_stripe = (process->window_offset / ec_bdev->strip_size) / ec_bdev->k;
	window_end_stripe = ((process->window_offset + process->window_size) / ec_bdev->strip_size) / ec_bdev->k;
	if (window_end_stripe > total_stripes) {
		window_end_stripe = total_stripes;
	}

	/* Process all stripes in current window */
	while (current_stripe < window_end_stripe && pending_requests < EC_BDEV_PROCESS_MAX_QD) {
		int rc = ec_bdev_submit_process_request(process, current_stripe);
		if (rc != 0) {
			if (rc == -ENOMEM) {
				/* ENOMEM handling:
				 * - If some reads were submitted: request added to queue in submit function,
				 *   window_remaining updated. Retry will happen in read_complete callback.
				 * - If no reads were submitted: request not in queue, can retry here later.
				 * 
				 * In both cases, don't increment current_stripe - the stripe will be completed
				 * via the existing request (if in queue) or retried later (if not in queue).
				 */
				return;
			} else {
				/* Fatal error - request was added to queue and window_remaining updated
				 * So we should increment current_stripe to avoid retrying the same stripe
				 */
				current_stripe++;
				process->status = rc;
				process->error_status = rc;
				return;
			}
		}
		/* Success: request added to queue, window_remaining and window_stripes_submitted updated */
		current_stripe++;
		pending_requests++;
	}

	/* Window completion is now checked in ec_bdev_process_request_complete
	 * when window_remaining reaches 0 (aligned with RAID framework)
	 * This ensures completion check happens atomically with request completion
	 */
}

/* ====================================================================
 * Process start/stop interface
 * ==================================================================== */

/* Start process - create thread and initialize */
int
ec_bdev_start_process(struct ec_bdev *ec_bdev, enum ec_process_type type,
		      struct ec_base_bdev_info *target,
		      void (*done_cb)(void *ctx, int status), void *done_ctx)
{
	struct ec_bdev_process *process;

	if (ec_bdev == NULL || target == NULL) {
		return -EINVAL;
	}

	if (ec_bdev->process != NULL) {
		SPDK_WARNLOG("Process already in progress for EC bdev %s\n", ec_bdev->bdev.name);
		return -EBUSY;
	}

	/* Allocate process structure */
	process = calloc(1, sizeof(*process));
	if (process == NULL) {
		return -ENOMEM;
	}

	process->ec_bdev = ec_bdev;
	process->type = type;
	process->state = EC_PROCESS_STATE_INIT;
	process->target = target;
	process->rebuild_done_cb = done_cb;
	process->rebuild_done_ctx = done_ctx;
	process->window_status = 0;
	process->error_status = 0;
	process->window_offset = 0;
	process->window_range_locked = false;
	process->max_window_size = EC_BDEV_PROCESS_WINDOW_SIZE_KB_DEFAULT * 1024 / 512; /* Convert KB to blocks */
	process->window_size = 0;
	process->window_remaining = 0;
	process->window_stripes_submitted = 0;
	process->window_stripes_total = 0;
	process->rebuild_state = EC_REBUILD_STATE_IDLE;

	TAILQ_INIT(&process->requests);
	TAILQ_INIT(&process->finish_actions);

	/* Create process thread */
	char thread_name[64];
	snprintf(thread_name, sizeof(thread_name), "ec_process_%s", ec_bdev->bdev.name);
	process->thread = spdk_thread_create(thread_name, NULL);
	if (process->thread == NULL) {
		SPDK_ERRLOG("Failed to create process thread for EC bdev %s\n", ec_bdev->bdev.name);
		ec_bdev_process_free(process);
		return -ENOMEM;
	}

	ec_bdev->process = process;

	/* For rebuild: update superblock state to REBUILDING before starting */
	if (type == EC_PROCESS_REBUILD && ec_bdev->superblock_enabled && ec_bdev->sb != NULL) {
		uint8_t target_slot = ec_bdev_base_bdev_slot(target);
		if (ec_bdev_sb_update_base_bdev_state(ec_bdev, target_slot,
						      EC_SB_BASE_BDEV_REBUILDING)) {
			ec_bdev_write_superblock(ec_bdev, NULL, NULL);
		}
	}

	SPDK_NOTICELOG("EC process started for bdev %s, type %d\n", ec_bdev->bdev.name, type);

	/* Start process thread */
	spdk_thread_send_msg(process->thread, ec_bdev_process_thread_init, process);

	return 0;
}

/* Stop process */
void
ec_bdev_stop_process(struct ec_bdev *ec_bdev)
{
	struct ec_bdev_process *process;

	if (ec_bdev == NULL) {
		return;
	}

	process = ec_bdev->process;
	if (process == NULL) {
		return;
	}

	SPDK_NOTICELOG("Stopping EC process for bdev %s\n", ec_bdev->bdev.name);

	/* Mark as stopping */
	process->state = EC_PROCESS_STATE_STOPPING;

	/* For rebuild: update superblock state to FAILED if stopping */
	if (process->type == EC_PROCESS_REBUILD &&
	    ec_bdev->superblock_enabled &&
	    ec_bdev->sb != NULL &&
	    process->target != NULL) {
		uint8_t target_slot = ec_bdev_base_bdev_slot(process->target);
		if (ec_bdev_sb_update_base_bdev_state(ec_bdev, target_slot,
						      EC_SB_BASE_BDEV_FAILED)) {
			ec_bdev_write_superblock(ec_bdev, NULL, NULL);
		}
	}

	/* Call done callback if set */
	if (process->rebuild_done_cb != NULL) {
		process->rebuild_done_cb(process->rebuild_done_ctx, -ECANCELED);
	}

	/* Cleanup will be done in process thread */
	ec_bdev->process = NULL;
	ec_bdev_process_free(process);
}

/* Check if process is in progress */
bool
ec_bdev_is_process_in_progress(struct ec_bdev *ec_bdev)
{
	return ec_bdev_get_active_process_internal(ec_bdev) != NULL;
}

