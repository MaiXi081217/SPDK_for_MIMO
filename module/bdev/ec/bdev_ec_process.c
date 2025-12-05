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

/* Constants are now defined in bdev_ec_internal.h */

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
	uint8_t failed_frag_idx;  /* For rebuild: single failed fragment index */
	uint8_t failed_data_indices[EC_MAX_K];  /* For rebalance: list of failed data fragment indices */
	uint8_t num_failed_data;  /* Number of failed data fragments (for rebalance) */
	uint8_t frag_map[EC_MAX_K + EC_MAX_P];
	uint8_t reads_completed;
	uint8_t reads_expected;
	uint8_t reads_submitted;  /* Track how many reads have been submitted (for retry logic) */
	bool reads_retry_needed;  /* Flag to indicate if retry is needed for remaining reads */
	unsigned char *stripe_buf;
	unsigned char *recover_buf;
	unsigned char *data_ptrs[EC_MAX_K];
	/* For rebalance verification */
	unsigned char *verify_buf;  /* Buffer for reading back data for verification */
	unsigned char *verify_parity_buf;  /* Buffer for reading back parity for verification */
	unsigned char *original_data_buf;  /* Buffer to save original written data for verification comparison */
	uint8_t verify_reads_completed;  /* Track verification reads */
	uint8_t verify_reads_expected;  /* Expected verification reads (k+p) */
	bool verification_in_progress;  /* Flag to indicate verification is in progress */
};

/* Forward declarations for verification helper functions (after struct definition) */
static void ec_bdev_process_free_verification_buffers(struct ec_process_stripe_ctx *stripe_ctx);
static void ec_bdev_process_verification_error(struct ec_bdev_process_request *process_req,
					       struct ec_process_stripe_ctx *stripe_ctx,
					       int error_code, const char *error_msg);

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

	/* Rebuild requires a target, but rebalance does not */
	if (process->type == EC_PROCESS_REBUILD) {
		/* Process must have a target for rebuild */
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
	} else {
		/* For rebalance, target_ch is not needed */
		ec_ch->process.target_ch = NULL;
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

		/* Setup base channels */
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		uint8_t slot = ec_bdev_base_bdev_slot(base_info);

		if (slot >= ec_bdev->num_base_bdevs) {
			SPDK_ERRLOG("Calculated slot %u exceeds num_base_bdevs %u\n",
				    slot, ec_bdev->num_base_bdevs);
			goto err;
		}

		if (process->type == EC_PROCESS_REBUILD) {
			/* For rebuild: use target_ch for target, base_channel for others */
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
		} else if (process->type == EC_PROCESS_REBALANCE) {
			/* For rebalance: use all active base bdevs */
			if (base_info->desc != NULL && !base_info->is_failed && base_info->is_active) {
				if (ec_ch->base_channel != NULL) {
					ec_ch_processed->base_channel[slot] = ec_ch->base_channel[slot];
					if (ec_ch_processed->base_channel[slot] != NULL) {
						available_base_bdevs++;
					}
				}
			}
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
	} else if (process->type == EC_PROCESS_REBALANCE) {
		/* For rebalance: need at least k+p active base bdevs */
		if (available_base_bdevs < ec_bdev->k + ec_bdev->p) {
			SPDK_ERRLOG("Insufficient active base bdevs available for rebalance on EC bdev '%s': "
				    "%u available, need at least %u\n",
				    ec_bdev->bdev.name, available_base_bdevs, ec_bdev->k + ec_bdev->p);
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
	
	/* Show process notification when process starts */
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
	} else if (process->type == EC_PROCESS_REBALANCE) {
		struct ec_rebalance_context *rebalance_ctx = process->ec_bdev->rebalance_ctx;
		if (rebalance_ctx != NULL) {
			SPDK_NOTICELOG("\n");
			SPDK_NOTICELOG("===========================================================\n");
			SPDK_NOTICELOG("REBALANCE IN PROGRESS: EC bdev '%s'\n", process->ec_bdev->bdev.name);
			SPDK_NOTICELOG("Old active profile: %u\n", rebalance_ctx->old_active_profile_id);
			SPDK_NOTICELOG("New active profile: %u\n", rebalance_ctx->new_active_profile_id);
			SPDK_NOTICELOG("Total stripes: %lu\n", rebalance_ctx->total_stripes);
			SPDK_NOTICELOG("Rebalance is running in background. This may take a while...\n");
		SPDK_NOTICELOG("Check progress: bdev_ec_get_bdevs\n");
		SPDK_NOTICELOG("===========================================================\n");
		SPDK_NOTICELOG("\n");
		}
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
	} else if (process->type == EC_PROCESS_REBALANCE) {
		/* 【问题5修复】统一进度计算：使用 rebalance_ctx->current_stripe */
		/* Progress is updated in stripe_write_complete based on current_stripe */
		/* This ensures consistency and avoids duplicate progress updates */
		struct ec_rebalance_context *rebalance_ctx = process->ec_bdev->rebalance_ctx;
		if (rebalance_ctx != NULL && rebalance_ctx->total_stripes > 0) {
			uint32_t percent = (uint32_t)((rebalance_ctx->current_stripe * 100) / 
								      rebalance_ctx->total_stripes);
			SPDK_NOTICELOG("EC[rebalance] Rebalance progress for bdev %s: %lu/%lu stripes (%u%%)\n",
				       process->ec_bdev->bdev.name, 
				       rebalance_ctx->current_stripe, 
				       rebalance_ctx->total_stripes, 
				       percent);
		}
	}

	/* Check if process is complete */
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
		} else if (process->type == EC_PROCESS_REBALANCE) {
			/* Rebalance complete - update rebalance state */
			struct ec_rebalance_context *rebalance_ctx = process->ec_bdev->rebalance_ctx;
			if (rebalance_ctx != NULL) {
				rebalance_ctx->current_stripe = rebalance_ctx->total_stripes;
				process->ec_bdev->rebalance_progress = 100;
			}
			SPDK_NOTICELOG("EC[rebalance] Rebalance completed on EC bdev %s\n",
				       process->ec_bdev->bdev.name);
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

/* Free verification buffers - centralized cleanup function */
static void
ec_bdev_process_free_verification_buffers(struct ec_process_stripe_ctx *stripe_ctx)
{
	if (stripe_ctx == NULL) {
		return;
	}
	
	if (stripe_ctx->verify_buf != NULL) {
		spdk_dma_free(stripe_ctx->verify_buf);
		stripe_ctx->verify_buf = NULL;
	}
	if (stripe_ctx->verify_parity_buf != NULL) {
		spdk_dma_free(stripe_ctx->verify_parity_buf);
		stripe_ctx->verify_parity_buf = NULL;
	}
	if (stripe_ctx->original_data_buf != NULL) {
		spdk_dma_free(stripe_ctx->original_data_buf);
		stripe_ctx->original_data_buf = NULL;
	}
	stripe_ctx->verification_in_progress = false;
}

/* Handle verification error - centralized error handling */
static void
ec_bdev_process_verification_error(struct ec_bdev_process_request *process_req,
				    struct ec_process_stripe_ctx *stripe_ctx,
				    int error_code, const char *error_msg)
{
	struct ec_bdev_process *process = process_req->process;
	struct ec_bdev *ec_bdev = process->ec_bdev;
	
	SPDK_ERRLOG("%s", error_msg);
	
	/* 【问题4修复】验证失败时，如果 group_profile_map 已经更新，需要回滚 */
	if (process->type == EC_PROCESS_REBALANCE && ec_bdev->rebalance_ctx != NULL) {
		struct ec_rebalance_context *rebalance_ctx = ec_bdev->rebalance_ctx;
		struct ec_device_selection_config *config = &ec_bdev->selection_config;
		uint32_t group_id = ec_selection_calculate_group_id(config, stripe_ctx->stripe_index);
		
		/* 检查当前 stripe 的 group 是否已更新到新 profile */
		if (config->group_profile_map != NULL && 
		    group_id < config->group_profile_capacity &&
		    config->group_profile_map[group_id] == rebalance_ctx->new_active_profile_id) {
			/* 回滚当前 stripe 的 group_profile_map */
			SPDK_WARNLOG("EC[rebalance] Rolling back group %u for failed verification of stripe %lu\n",
				     group_id, stripe_ctx->stripe_index);
			config->group_profile_map[group_id] = rebalance_ctx->old_active_profile_id;
			ec_selection_mark_group_dirty(ec_bdev);
			
			/* 从跟踪列表中移除（如果存在） */
			if (rebalance_ctx->updated_groups != NULL) {
				for (uint32_t i = 0; i < rebalance_ctx->num_updated_groups; i++) {
					if (rebalance_ctx->updated_groups[i] == group_id) {
						/* 移除：将最后一个元素移到当前位置 */
						rebalance_ctx->updated_groups[i] = 
							rebalance_ctx->updated_groups[--rebalance_ctx->num_updated_groups];
						break;
					}
				}
			}
		}
	}
	
	ec_bdev_process_free_verification_buffers(stripe_ctx);
	process->status = error_code;
	process->error_status = error_code;
	if (ec_bdev->rebalance_ctx) {
		ec_bdev->rebalance_ctx->error_status = error_code;
	}
	ec_bdev_process_request_complete(process_req, error_code);
}

/* ====================================================================
 * Request processing
 * ==================================================================== */

/* Process request complete callback - unified with RAID framework logic */
void
ec_bdev_process_request_complete(struct ec_bdev_process_request *process_req, int status)
{
	struct ec_bdev_process *process;

	if (process_req == NULL) {
		return;
	}

	process = process_req->process;
	if (process == NULL) {
		free(process_req);
		return;
	}

	assert(spdk_get_thread() == process->thread);

	/* Set error status if any (aligned with RAID framework) */
	if (status != 0) {
		process->window_status = status;
	}

	/* Insert request back to queue for reuse (aligned with RAID framework) */
	TAILQ_INSERT_TAIL(&process->requests, process_req, link);

	/* Always decrement window_remaining (aligned with RAID framework) */
	assert(process->window_remaining > 0);
	process->window_remaining--;

	/* Check if window is complete (aligned with RAID framework) */
	if (process->window_remaining == 0) {
		if (process->window_status != 0) {
			/* Error occurred - finish process with error */
			process->status = process->window_status;
			ec_bdev_process_thread_run(process);
			return;
		}

		/* Window complete - update channel offsets and unlock window */
		if (process->ec_ch != NULL) {
			process->ec_ch->process.offset = process->window_offset + process->window_size;
		}
		
		/* Unlock window range */
		ec_bdev_process_unlock_window_range(process);
		
		/* Continue processing next window */
		ec_bdev_process_thread_run(process);
	}
}

/* Stripe write complete callback */
static void
ec_bdev_process_stripe_write_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ec_bdev_process_request *process_req = cb_arg;
	struct ec_bdev_process *process;
	struct ec_process_stripe_ctx *stripe_ctx;
	struct ec_bdev *ec_bdev;

	/* bdev_io may be NULL when called from read_complete for verification processing */
	if (bdev_io != NULL) {
		spdk_bdev_free_io(bdev_io);
	}

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
		if (process->type == EC_PROCESS_REBUILD) {
			SPDK_ERRLOG("Rebuild write failed for stripe %lu on EC bdev %s\n",
				    stripe_ctx->stripe_index, ec_bdev->bdev.name);
		} else if (process->type == EC_PROCESS_REBALANCE) {
			/* Check if this is a verification read failure */
			if (stripe_ctx->verification_in_progress) {
				SPDK_ERRLOG("Rebalance verification read failed for stripe %lu on EC bdev %s\n",
					    stripe_ctx->stripe_index, ec_bdev->bdev.name);
				/* Free verification buffers */
				ec_bdev_process_free_verification_buffers(stripe_ctx);
			} else {
				SPDK_ERRLOG("Rebalance write failed for stripe %lu on EC bdev %s\n",
					    stripe_ctx->stripe_index, ec_bdev->bdev.name);
			}
		}
		
		/* Update superblock and error status */
		ec_bdev_process_update_superblock_on_error(process);
		process->status = -EIO;
		process->error_status = -EIO;
		if (process->type == EC_PROCESS_REBALANCE && ec_bdev->rebalance_ctx) {
			ec_bdev->rebalance_ctx->error_status = -EIO;
		}
		ec_bdev_process_request_complete(process_req, -EIO);
		return;
	}

	if (process->type == EC_PROCESS_REBUILD) {
		/* Rebuild: single write to target, complete immediately */
		process->rebuild_state = EC_REBUILD_STATE_IDLE;
		ec_bdev_process_request_complete(process_req, 0);
	} else if (process->type == EC_PROCESS_REBALANCE) {
		/* Rebalance: track multiple writes (k data + p parity) */
		stripe_ctx->reads_completed++;  /* Reuse for write tracking */
		
		/* Check if all writes completed */
		if (stripe_ctx->reads_completed >= stripe_ctx->reads_expected) {
			/* All writes completed - start verification if not already in progress */
			if (!stripe_ctx->verification_in_progress) {
				/* 【步骤8】写入后强制验证：从新设备读取并验证数据完整性 */
				uint8_t k = ec_bdev->k;
				uint8_t p = ec_bdev->p;
				uint32_t strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
				uint8_t i, idx;
				int rc;
				
				/* Allocate verification buffers */
				stripe_ctx->verify_buf = spdk_dma_malloc(strip_size_bytes * k, ec_bdev->buf_alignment, NULL);
				if (stripe_ctx->verify_buf == NULL) {
					SPDK_ERRLOG("Failed to allocate verification buffer for rebalance stripe %lu\n",
						    stripe_ctx->stripe_index);
					process->status = -ENOMEM;
					process->error_status = -ENOMEM;
					ec_bdev_process_request_complete(process_req, -ENOMEM);
					return;
				}
				
				stripe_ctx->verify_parity_buf = spdk_dma_malloc(strip_size_bytes * p, ec_bdev->buf_alignment, NULL);
				if (stripe_ctx->verify_parity_buf == NULL) {
					SPDK_ERRLOG("Failed to allocate verification parity buffer for rebalance stripe %lu\n",
						    stripe_ctx->stripe_index);
					spdk_dma_free(stripe_ctx->verify_buf);
					stripe_ctx->verify_buf = NULL;
					process->status = -ENOMEM;
					process->error_status = -ENOMEM;
					ec_bdev_process_request_complete(process_req, -ENOMEM);
					return;
				}
				
				/* Start verification reads */
				stripe_ctx->verification_in_progress = true;
				stripe_ctx->verify_reads_completed = 0;
				stripe_ctx->verify_reads_expected = k + p;
				
				/* Prepare data pointers for verification */
				unsigned char *verify_data_ptrs[EC_MAX_K];
				for (i = 0; i < k; i++) {
					verify_data_ptrs[i] = stripe_ctx->verify_buf + i * strip_size_bytes;
				}
				
				/* Read k data blocks for verification */
				for (i = 0; i < k; i++) {
					idx = stripe_ctx->data_indices[i];
					struct ec_base_bdev_info *base_info = &ec_bdev->base_bdev_info[idx];
					
					if (base_info->desc == NULL || base_info->is_failed) {
						SPDK_ERRLOG("Verification: data device %u is not available for rebalance stripe %lu\n",
							    idx, stripe_ctx->stripe_index);
						spdk_dma_free(stripe_ctx->verify_buf);
						spdk_dma_free(stripe_ctx->verify_parity_buf);
						stripe_ctx->verify_buf = NULL;
						stripe_ctx->verify_parity_buf = NULL;
						process->status = -ENODEV;
						process->error_status = -ENODEV;
						ec_bdev_process_request_complete(process_req, -ENODEV);
						return;
					}
					
					uint64_t pd_lba = (stripe_ctx->stripe_index << ec_bdev->strip_size_shift) +
							  base_info->data_offset;
					
					/* Use read_complete callback for verification reads */
					rc = spdk_bdev_read_blocks(base_info->desc,
								process->ec_ch->process.ch_processed->base_channel[idx],
								verify_data_ptrs[i], pd_lba, ec_bdev->strip_size,
								ec_bdev_process_stripe_read_complete, process_req);
					if (rc != 0) {
						if (rc == -ENOMEM) {
							/* Queue wait and retry - track how many reads we've submitted */
							/* Keep verification_in_progress = true, but update expected count */
							stripe_ctx->verify_reads_expected = i;  /* Only count reads we've submitted so far */
							process_req->ec_io.waitq_entry.bdev = spdk_bdev_desc_get_bdev(base_info->desc);
							process_req->ec_io.waitq_entry.cb_fn = ec_bdev_process_thread_run_wrapper;
							process_req->ec_io.waitq_entry.cb_arg = process;
							process_req->ec_io.waitq_entry.dep_unblock = true;
							spdk_bdev_queue_io_wait(process_req->ec_io.waitq_entry.bdev,
										process->ec_ch->process.ch_processed->base_channel[idx],
										&process_req->ec_io.waitq_entry);
							return;  /* Will retry - verification reads will complete via callback */
						} else {
							char error_msg[256];
							snprintf(error_msg, sizeof(error_msg),
								 "Failed to read data for verification from device %u for rebalance stripe %lu: %s\n",
								 idx, stripe_ctx->stripe_index, spdk_strerror(-rc));
							ec_bdev_process_verification_error(process_req, stripe_ctx, rc, error_msg);
							return;
						}
					}
				}
				
				/* Read p parity blocks for verification */
				unsigned char *verify_parity_ptrs[EC_MAX_P];
				for (i = 0; i < p; i++) {
					verify_parity_ptrs[i] = stripe_ctx->verify_parity_buf + i * strip_size_bytes;
				}
				
				for (i = 0; i < p; i++) {
					idx = stripe_ctx->parity_indices[i];
					struct ec_base_bdev_info *base_info = &ec_bdev->base_bdev_info[idx];
					
					if (base_info->desc == NULL || base_info->is_failed) {
						SPDK_ERRLOG("Verification: parity device %u is not available for rebalance stripe %lu\n",
							    idx, stripe_ctx->stripe_index);
						spdk_dma_free(stripe_ctx->verify_buf);
						spdk_dma_free(stripe_ctx->verify_parity_buf);
						stripe_ctx->verify_buf = NULL;
						stripe_ctx->verify_parity_buf = NULL;
						stripe_ctx->verification_in_progress = false;  /* Reset verification state */
						process->status = -ENODEV;
						process->error_status = -ENODEV;
						ec_bdev_process_request_complete(process_req, -ENODEV);
						return;
					}
					
					uint64_t pd_lba = (stripe_ctx->stripe_index << ec_bdev->strip_size_shift) +
							  base_info->data_offset;
					
					/* Use read_complete callback for verification reads */
					rc = spdk_bdev_read_blocks(base_info->desc,
								process->ec_ch->process.ch_processed->base_channel[idx],
								verify_parity_ptrs[i], pd_lba, ec_bdev->strip_size,
								ec_bdev_process_stripe_read_complete, process_req);
					if (rc != 0) {
						if (rc == -ENOMEM) {
							/* Queue wait and retry - track how many reads we've submitted */
							/* Keep verification_in_progress = true, but update expected count */
							stripe_ctx->verify_reads_expected = k + i;  /* k data reads + i parity reads submitted so far */
							process_req->ec_io.waitq_entry.bdev = spdk_bdev_desc_get_bdev(base_info->desc);
							process_req->ec_io.waitq_entry.cb_fn = ec_bdev_process_thread_run_wrapper;
							process_req->ec_io.waitq_entry.cb_arg = process;
							process_req->ec_io.waitq_entry.dep_unblock = true;
							spdk_bdev_queue_io_wait(process_req->ec_io.waitq_entry.bdev,
										process->ec_ch->process.ch_processed->base_channel[idx],
										&process_req->ec_io.waitq_entry);
							return;  /* Will retry - verification reads will complete via callback */
						} else {
							char error_msg[256];
							snprintf(error_msg, sizeof(error_msg),
								 "Failed to read parity for verification from device %u for rebalance stripe %lu: %s\n",
								 idx, stripe_ctx->stripe_index, spdk_strerror(-rc));
							ec_bdev_process_verification_error(process_req, stripe_ctx, rc, error_msg);
							return;
						}
					}
				}
				
				return;  /* Verification reads submitted, will continue in callback */
			} else {
				/* Verification reads completed - verify data integrity */
				uint8_t k = ec_bdev->k;
				uint8_t p = ec_bdev->p;
				uint32_t strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
				unsigned char *verify_data_ptrs[EC_MAX_K];
				unsigned char *verify_parity_ptrs[EC_MAX_P];
				unsigned char *computed_parity_ptrs[EC_MAX_P];
				uint8_t i;
				int rc;
				
				/* Prepare pointers */
				for (i = 0; i < k; i++) {
					verify_data_ptrs[i] = stripe_ctx->verify_buf + i * strip_size_bytes;
				}
				for (i = 0; i < p; i++) {
					verify_parity_ptrs[i] = stripe_ctx->verify_parity_buf + i * strip_size_bytes;
					/* Use recover_buf for computed parity (already allocated) */
					computed_parity_ptrs[i] = stripe_ctx->recover_buf + i * strip_size_bytes;
				}
				
				/* Re-encode to compute expected parity */
				rc = ec_encode_stripe(ec_bdev, verify_data_ptrs, computed_parity_ptrs, strip_size_bytes);
				if (rc != 0) {
					char error_msg[256];
					snprintf(error_msg, sizeof(error_msg),
						 "Failed to re-encode for verification on stripe %lu: %s\n",
						 stripe_ctx->stripe_index, spdk_strerror(-rc));
					ec_bdev_process_verification_error(process_req, stripe_ctx, rc, error_msg);
					return;
				}
				
				/* Compare original written data with read data */
				/* Use original_data_buf if available (saved before writing), otherwise use data_ptrs */
				unsigned char *original_data_ptrs[EC_MAX_K];
				if (stripe_ctx->original_data_buf != NULL) {
					for (i = 0; i < k; i++) {
						original_data_ptrs[i] = stripe_ctx->original_data_buf + i * strip_size_bytes;
					}
				} else {
					/* Fallback: use data_ptrs (may contain decoded data if recovery occurred) */
					for (i = 0; i < k; i++) {
						original_data_ptrs[i] = stripe_ctx->data_ptrs[i];
					}
				}
				
				for (i = 0; i < k; i++) {
					if (memcmp(original_data_ptrs[i], verify_data_ptrs[i], strip_size_bytes) != 0) {
						char error_msg[256];
						snprintf(error_msg, sizeof(error_msg),
							 "Verification failed for stripe %lu: data block %u mismatch\n",
							 stripe_ctx->stripe_index, i);
						ec_bdev_process_verification_error(process_req, stripe_ctx, -EIO, error_msg);
						return;
					}
				}
				
				/* Compare computed parity with read parity */
				for (i = 0; i < p; i++) {
					if (memcmp(computed_parity_ptrs[i], verify_parity_ptrs[i], strip_size_bytes) != 0) {
						char error_msg[256];
						snprintf(error_msg, sizeof(error_msg),
							 "Verification failed for stripe %lu: parity block %u mismatch\n",
							 stripe_ctx->stripe_index, i);
						ec_bdev_process_verification_error(process_req, stripe_ctx, -EIO, error_msg);
						return;
					}
				}
				
				/* Verification passed - free verification buffers and reset flag */
				ec_bdev_process_free_verification_buffers(stripe_ctx);
				
				/* All writes and verification completed - update metadata and complete */
				struct ec_rebalance_context *rebalance_ctx = ec_bdev->rebalance_ctx;
				struct ec_device_selection_config *config = &ec_bdev->selection_config;
				uint32_t group_id;
				
				if (rebalance_ctx != NULL) {
					/* 【步骤6】更新group_profile_map（绑定到新profile） */
					group_id = ec_selection_calculate_group_id(config, stripe_ctx->stripe_index);
					if (config->group_profile_map != NULL && 
					    group_id < config->group_profile_capacity) {
						/* 【问题5修复】跟踪本次更新的 group */
						uint32_t old_profile = config->group_profile_map[group_id];
						config->group_profile_map[group_id] = rebalance_ctx->new_active_profile_id;
						
						/* 如果这个 group 之前不是新 profile，则添加到跟踪列表 */
						if (old_profile != rebalance_ctx->new_active_profile_id) {
							/* 检查是否需要扩展数组 */
							if (rebalance_ctx->num_updated_groups >= rebalance_ctx->updated_groups_capacity) {
								uint32_t new_capacity = rebalance_ctx->updated_groups_capacity == 0 ? 
								    16 : rebalance_ctx->updated_groups_capacity * 2;
								uint32_t *new_array = realloc(rebalance_ctx->updated_groups,
												new_capacity * sizeof(uint32_t));
								if (new_array != NULL) {
									rebalance_ctx->updated_groups = new_array;
									rebalance_ctx->updated_groups_capacity = new_capacity;
								}
							}
							if (rebalance_ctx->updated_groups != NULL &&
							    rebalance_ctx->num_updated_groups < rebalance_ctx->updated_groups_capacity) {
								rebalance_ctx->updated_groups[rebalance_ctx->num_updated_groups++] = group_id;
							}
						}
						
						ec_selection_mark_group_dirty(ec_bdev);
					}
					
					/* 【步骤7】清除该stripe的assignment_cache */
					ec_assignment_cache_invalidate_stripe(config, stripe_ctx->stripe_index);
					
					/* Update progress only after successful verification */
					rebalance_ctx->current_stripe++;
					if (rebalance_ctx->total_stripes > 0) {
						uint32_t percent = (uint32_t)((rebalance_ctx->current_stripe * 100) / 
								      rebalance_ctx->total_stripes);
						ec_bdev->rebalance_progress = percent;
					}
				}
				
				/* Complete this request */
				ec_bdev_process_request_complete(process_req, 0);
			}
		} else if (stripe_ctx->verification_in_progress) {
			/* Verification read completed */
			stripe_ctx->verify_reads_completed++;
			
			/* Check if all verification reads completed */
			/* Note: verify_reads_expected may be less than k+p if ENOMEM occurred during submission */
			if (stripe_ctx->verify_reads_completed >= stripe_ctx->verify_reads_expected) {
				/* If verify_reads_expected < k+p, we need to submit remaining verification reads */
				uint8_t total_k = ec_bdev->k;
				uint8_t total_p = ec_bdev->p;
				uint32_t strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
				uint8_t total_expected = total_k + total_p;
				
				if (stripe_ctx->verify_reads_expected < total_expected) {
					/* Partial verification reads completed - need to submit remaining reads */
					uint8_t remaining_verify_reads = total_expected - stripe_ctx->verify_reads_expected;
					uint8_t start_idx;
					uint8_t i, idx;
					int rc_retry;
					
					/* Determine where to continue: if verify_reads_expected < total_k, continue data reads */
					if (stripe_ctx->verify_reads_expected < total_k) {
						/* Continue data reads */
						start_idx = stripe_ctx->verify_reads_expected;
						unsigned char *verify_data_ptrs[EC_MAX_K];
						for (i = 0; i < total_k; i++) {
							verify_data_ptrs[i] = stripe_ctx->verify_buf + i * strip_size_bytes;
						}
						
						for (uint8_t j = 0; j < remaining_verify_reads && (start_idx + j) < total_k; j++) {
							idx = stripe_ctx->data_indices[start_idx + j];
							struct ec_base_bdev_info *base_info_retry = &ec_bdev->base_bdev_info[idx];
							
							if (base_info_retry->desc == NULL || base_info_retry->is_failed) {
								SPDK_ERRLOG("Verification retry: data device %u is not available for rebalance stripe %lu\n",
									    idx, stripe_ctx->stripe_index);
								spdk_dma_free(stripe_ctx->verify_buf);
								spdk_dma_free(stripe_ctx->verify_parity_buf);
								stripe_ctx->verify_buf = NULL;
								stripe_ctx->verify_parity_buf = NULL;
								stripe_ctx->verification_in_progress = false;
								process->status = -ENODEV;
								process->error_status = -ENODEV;
								ec_bdev_process_request_complete(process_req, -ENODEV);
								return;
							}
							
							uint64_t pd_lba_retry = (stripe_ctx->stripe_index << ec_bdev->strip_size_shift) +
										base_info_retry->data_offset;
							
							/* Use read_complete callback for verification reads */
							rc_retry = spdk_bdev_read_blocks(base_info_retry->desc,
										process->ec_ch->process.ch_processed->base_channel[idx],
										verify_data_ptrs[start_idx + j], pd_lba_retry, ec_bdev->strip_size,
										ec_bdev_process_stripe_read_complete, process_req);
							if (rc_retry != 0) {
								if (rc_retry == -ENOMEM) {
									/* Queue wait and retry again */
									process_req->ec_io.waitq_entry.bdev = spdk_bdev_desc_get_bdev(base_info_retry->desc);
									process_req->ec_io.waitq_entry.cb_fn = ec_bdev_process_thread_run_wrapper;
									process_req->ec_io.waitq_entry.cb_arg = process;
									process_req->ec_io.waitq_entry.dep_unblock = true;
									spdk_bdev_queue_io_wait(process_req->ec_io.waitq_entry.bdev,
												process->ec_ch->process.ch_processed->base_channel[idx],
												&process_req->ec_io.waitq_entry);
									/* Update expected reads to include what we just tried to submit */
									stripe_ctx->verify_reads_expected = start_idx + j;
									return; /* Will retry later */
								} else {
									SPDK_ERRLOG("Failed to read data for verification (retry) from device %u for rebalance stripe %lu: %s\n",
										    idx, stripe_ctx->stripe_index, spdk_strerror(-rc_retry));
									spdk_dma_free(stripe_ctx->verify_buf);
									spdk_dma_free(stripe_ctx->verify_parity_buf);
									stripe_ctx->verify_buf = NULL;
									stripe_ctx->verify_parity_buf = NULL;
									stripe_ctx->verification_in_progress = false;
									process->status = rc_retry;
									process->error_status = rc_retry;
									ec_bdev_process_request_complete(process_req, rc_retry);
									return;
								}
							}
						}
						
						/* If we've completed all data reads, continue with parity reads */
						if (stripe_ctx->verify_reads_expected >= total_k) {
							/* Continue with parity reads */
							unsigned char *verify_parity_ptrs[EC_MAX_P];
							for (i = 0; i < total_p; i++) {
								verify_parity_ptrs[i] = stripe_ctx->verify_parity_buf + i * strip_size_bytes;
							}
							
							uint8_t parity_start_idx = stripe_ctx->verify_reads_expected - total_k;
							uint8_t remaining_parity_reads = total_expected - stripe_ctx->verify_reads_expected;
							
							for (uint8_t j = 0; j < remaining_parity_reads && (parity_start_idx + j) < total_p; j++) {
								idx = stripe_ctx->parity_indices[parity_start_idx + j];
								struct ec_base_bdev_info *base_info_retry = &ec_bdev->base_bdev_info[idx];
								
								if (base_info_retry->desc == NULL || base_info_retry->is_failed) {
									SPDK_ERRLOG("Verification retry: parity device %u is not available for rebalance stripe %lu\n",
										    idx, stripe_ctx->stripe_index);
									spdk_dma_free(stripe_ctx->verify_buf);
									spdk_dma_free(stripe_ctx->verify_parity_buf);
									stripe_ctx->verify_buf = NULL;
									stripe_ctx->verify_parity_buf = NULL;
									stripe_ctx->verification_in_progress = false;
									process->status = -ENODEV;
									process->error_status = -ENODEV;
									ec_bdev_process_request_complete(process_req, -ENODEV);
									return;
								}
								
								uint64_t pd_lba_retry = (stripe_ctx->stripe_index << ec_bdev->strip_size_shift) +
											base_info_retry->data_offset;
								
								/* Use read_complete callback for verification reads */
								rc_retry = spdk_bdev_read_blocks(base_info_retry->desc,
											process->ec_ch->process.ch_processed->base_channel[idx],
											verify_parity_ptrs[parity_start_idx + j], pd_lba_retry, ec_bdev->strip_size,
											ec_bdev_process_stripe_read_complete, process_req);
								if (rc_retry != 0) {
									if (rc_retry == -ENOMEM) {
										/* Queue wait and retry again */
										process_req->ec_io.waitq_entry.bdev = spdk_bdev_desc_get_bdev(base_info_retry->desc);
										process_req->ec_io.waitq_entry.cb_fn = ec_bdev_process_thread_run_wrapper;
										process_req->ec_io.waitq_entry.cb_arg = process;
										process_req->ec_io.waitq_entry.dep_unblock = true;
										spdk_bdev_queue_io_wait(process_req->ec_io.waitq_entry.bdev,
													process->ec_ch->process.ch_processed->base_channel[idx],
													&process_req->ec_io.waitq_entry);
										/* Update expected reads to include what we just tried to submit */
										stripe_ctx->verify_reads_expected = total_k + parity_start_idx + j;
										return; /* Will retry later */
									} else {
										SPDK_ERRLOG("Failed to read parity for verification (retry) from device %u for rebalance stripe %lu: %s\n",
											    idx, stripe_ctx->stripe_index, spdk_strerror(-rc_retry));
										spdk_dma_free(stripe_ctx->verify_buf);
										spdk_dma_free(stripe_ctx->verify_parity_buf);
										stripe_ctx->verify_buf = NULL;
										stripe_ctx->verify_parity_buf = NULL;
										stripe_ctx->verification_in_progress = false;
										process->status = rc_retry;
										process->error_status = rc_retry;
										ec_bdev_process_request_complete(process_req, rc_retry);
										return;
									}
								}
							}
						}
						
						/* Update expected reads to total */
						stripe_ctx->verify_reads_expected = total_expected;
						return; /* Wait for all verification reads to complete */
					} else {
						/* Continue parity reads */
						start_idx = stripe_ctx->verify_reads_expected - total_k;
						unsigned char *verify_parity_ptrs[EC_MAX_P];
						for (i = 0; i < total_p; i++) {
							verify_parity_ptrs[i] = stripe_ctx->verify_parity_buf + i * strip_size_bytes;
						}
						
						for (uint8_t j = 0; j < remaining_verify_reads && (start_idx + j) < total_p; j++) {
							idx = stripe_ctx->parity_indices[start_idx + j];
							struct ec_base_bdev_info *base_info_retry = &ec_bdev->base_bdev_info[idx];
							
							if (base_info_retry->desc == NULL || base_info_retry->is_failed) {
								SPDK_ERRLOG("Verification retry: parity device %u is not available for rebalance stripe %lu\n",
									    idx, stripe_ctx->stripe_index);
								spdk_dma_free(stripe_ctx->verify_buf);
								spdk_dma_free(stripe_ctx->verify_parity_buf);
								stripe_ctx->verify_buf = NULL;
								stripe_ctx->verify_parity_buf = NULL;
								stripe_ctx->verification_in_progress = false;
								process->status = -ENODEV;
								process->error_status = -ENODEV;
								ec_bdev_process_request_complete(process_req, -ENODEV);
								return;
							}
							
							uint64_t pd_lba_retry = (stripe_ctx->stripe_index << ec_bdev->strip_size_shift) +
										base_info_retry->data_offset;
							
							rc_retry = spdk_bdev_read_blocks(base_info_retry->desc,
										process->ec_ch->process.ch_processed->base_channel[idx],
										verify_parity_ptrs[start_idx + j], pd_lba_retry, ec_bdev->strip_size,
										ec_bdev_process_stripe_write_complete, process_req);
							if (rc_retry != 0) {
								if (rc_retry == -ENOMEM) {
									/* Queue wait and retry again */
									process_req->ec_io.waitq_entry.bdev = spdk_bdev_desc_get_bdev(base_info_retry->desc);
									process_req->ec_io.waitq_entry.cb_fn = ec_bdev_process_thread_run_wrapper;
									process_req->ec_io.waitq_entry.cb_arg = process;
									process_req->ec_io.waitq_entry.dep_unblock = true;
									spdk_bdev_queue_io_wait(process_req->ec_io.waitq_entry.bdev,
												process->ec_ch->process.ch_processed->base_channel[idx],
												&process_req->ec_io.waitq_entry);
									/* Update expected reads to include what we just tried to submit */
									stripe_ctx->verify_reads_expected = total_k + start_idx + j;
									return; /* Will retry later */
								} else {
									SPDK_ERRLOG("Failed to read parity for verification (retry) from device %u for rebalance stripe %lu: %s\n",
										    idx, stripe_ctx->stripe_index, spdk_strerror(-rc_retry));
									spdk_dma_free(stripe_ctx->verify_buf);
									spdk_dma_free(stripe_ctx->verify_parity_buf);
									stripe_ctx->verify_buf = NULL;
									stripe_ctx->verify_parity_buf = NULL;
									stripe_ctx->verification_in_progress = false;
									process->status = rc_retry;
									process->error_status = rc_retry;
									ec_bdev_process_request_complete(process_req, rc_retry);
									return;
								}
							}
						}
						
						/* Update expected reads to total */
						stripe_ctx->verify_reads_expected = total_expected;
						return; /* Wait for all verification reads to complete */
					}
				}
				
				/* All verification reads completed - verify data integrity */
				/* Use variables from outer scope if available, otherwise define new ones */
				uint8_t verify_k = ec_bdev->k;
				uint8_t verify_p = ec_bdev->p;
				uint32_t verify_strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
				unsigned char *verify_data_ptrs[EC_MAX_K];
				unsigned char *verify_parity_ptrs[EC_MAX_P];
				unsigned char *computed_parity_ptrs[EC_MAX_P];
				uint8_t verify_i;
				int verify_rc;
				
				/* Prepare pointers */
				for (verify_i = 0; verify_i < verify_k; verify_i++) {
					verify_data_ptrs[verify_i] = stripe_ctx->verify_buf + verify_i * verify_strip_size_bytes;
				}
				for (verify_i = 0; verify_i < verify_p; verify_i++) {
					verify_parity_ptrs[verify_i] = stripe_ctx->verify_parity_buf + verify_i * verify_strip_size_bytes;
					/* Use recover_buf for computed parity (already allocated) */
					computed_parity_ptrs[verify_i] = stripe_ctx->recover_buf + verify_i * verify_strip_size_bytes;
				}
				
				/* Re-encode to compute expected parity */
				verify_rc = ec_encode_stripe(ec_bdev, verify_data_ptrs, computed_parity_ptrs, verify_strip_size_bytes);
				if (verify_rc != 0) {
					char error_msg[256];
					snprintf(error_msg, sizeof(error_msg),
						 "Failed to re-encode for verification on stripe %lu: %s\n",
						 stripe_ctx->stripe_index, spdk_strerror(-verify_rc));
					ec_bdev_process_verification_error(process_req, stripe_ctx, verify_rc, error_msg);
					return;
				}
				
				/* Compare original written data with read data */
				/* Use original_data_buf if available (saved before writing), otherwise use data_ptrs */
				unsigned char *original_data_ptrs_verify[EC_MAX_K];
				if (stripe_ctx->original_data_buf != NULL) {
					for (verify_i = 0; verify_i < verify_k; verify_i++) {
						original_data_ptrs_verify[verify_i] = stripe_ctx->original_data_buf + verify_i * verify_strip_size_bytes;
					}
				} else {
					/* Fallback: use data_ptrs (may contain decoded data if recovery occurred) */
					for (verify_i = 0; verify_i < verify_k; verify_i++) {
						original_data_ptrs_verify[verify_i] = stripe_ctx->data_ptrs[verify_i];
					}
				}
				
				for (verify_i = 0; verify_i < verify_k; verify_i++) {
					if (memcmp(original_data_ptrs_verify[verify_i], verify_data_ptrs[verify_i], verify_strip_size_bytes) != 0) {
						SPDK_ERRLOG("Verification failed for stripe %lu: data block %u mismatch\n",
							    stripe_ctx->stripe_index, verify_i);
						spdk_dma_free(stripe_ctx->verify_buf);
						spdk_dma_free(stripe_ctx->verify_parity_buf);
						stripe_ctx->verify_buf = NULL;
						stripe_ctx->verify_parity_buf = NULL;
						stripe_ctx->verification_in_progress = false;
						process->status = -EIO;
						process->error_status = -EIO;
						if (ec_bdev->rebalance_ctx) {
							ec_bdev->rebalance_ctx->error_status = -EIO;
						}
						ec_bdev_process_request_complete(process_req, -EIO);
						return;
					}
				}
				
				/* Compare computed parity with read parity */
				for (verify_i = 0; verify_i < verify_p; verify_i++) {
					if (memcmp(computed_parity_ptrs[verify_i], verify_parity_ptrs[verify_i], verify_strip_size_bytes) != 0) {
						char error_msg[256];
						snprintf(error_msg, sizeof(error_msg),
							 "Verification failed for stripe %lu: parity block %u mismatch\n",
							 stripe_ctx->stripe_index, verify_i);
						ec_bdev_process_verification_error(process_req, stripe_ctx, -EIO, error_msg);
						return;
					}
				}
				
				/* Verification passed - free verification buffers and reset flag */
				ec_bdev_process_free_verification_buffers(stripe_ctx);
				
				/* All writes and verification completed - update metadata and complete */
				struct ec_rebalance_context *rebalance_ctx = ec_bdev->rebalance_ctx;
				struct ec_device_selection_config *config = &ec_bdev->selection_config;
				uint32_t group_id;
				
				if (rebalance_ctx != NULL) {
					/* 【步骤6】更新group_profile_map（绑定到新profile） */
					group_id = ec_selection_calculate_group_id(config, stripe_ctx->stripe_index);
					if (config->group_profile_map != NULL && 
					    group_id < config->group_profile_capacity) {
						/* 【问题5修复】跟踪本次更新的 group */
						uint32_t old_profile = config->group_profile_map[group_id];
						config->group_profile_map[group_id] = rebalance_ctx->new_active_profile_id;
						
						/* 如果这个 group 之前不是新 profile，则添加到跟踪列表 */
						if (old_profile != rebalance_ctx->new_active_profile_id) {
							/* 检查是否需要扩展数组 */
							if (rebalance_ctx->num_updated_groups >= rebalance_ctx->updated_groups_capacity) {
								uint32_t new_capacity = rebalance_ctx->updated_groups_capacity == 0 ? 
								    16 : rebalance_ctx->updated_groups_capacity * 2;
								uint32_t *new_array = realloc(rebalance_ctx->updated_groups,
												new_capacity * sizeof(uint32_t));
								if (new_array != NULL) {
									rebalance_ctx->updated_groups = new_array;
									rebalance_ctx->updated_groups_capacity = new_capacity;
								}
							}
							if (rebalance_ctx->updated_groups != NULL &&
							    rebalance_ctx->num_updated_groups < rebalance_ctx->updated_groups_capacity) {
								rebalance_ctx->updated_groups[rebalance_ctx->num_updated_groups++] = group_id;
							}
						}
						
						ec_selection_mark_group_dirty(ec_bdev);
					}
					
					/* 【步骤7】清除该stripe的assignment_cache */
					ec_assignment_cache_invalidate_stripe(config, stripe_ctx->stripe_index);
					
					/* Update progress only after successful verification */
					rebalance_ctx->current_stripe++;
					if (rebalance_ctx->total_stripes > 0) {
						uint32_t percent = (uint32_t)((rebalance_ctx->current_stripe * 100) / 
								      rebalance_ctx->total_stripes);
						ec_bdev->rebalance_progress = percent;
					}
				}
				
				/* Complete this request */
				ec_bdev_process_request_complete(process_req, 0);
			}
			/* Otherwise, wait for more verification reads to complete */
		}
		/* Otherwise, wait for more writes to complete */
	}
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
		if (process->type == EC_PROCESS_REBUILD) {
		SPDK_ERRLOG("Rebuild read failed for stripe %lu on EC bdev %s\n",
			    stripe_ctx->stripe_index, ec_bdev->bdev.name);
		} else if (process->type == EC_PROCESS_REBALANCE) {
			SPDK_ERRLOG("Rebalance read failed for stripe %lu on EC bdev %s\n",
				    stripe_ctx->stripe_index, ec_bdev->bdev.name);
		}
		
		/* Update superblock and error status */
		ec_bdev_process_update_superblock_on_error(process);
		process->status = -EIO;
		process->error_status = -EIO;
		ec_bdev_process_request_complete(process_req, -EIO);
		return;
	}

	/* Check if this is a verification read */
	if (stripe_ctx->verification_in_progress) {
		/* Handle verification read completion */
		stripe_ctx->verify_reads_completed++;
		
		/* Check if all verification reads completed */
		if (stripe_ctx->verify_reads_completed >= stripe_ctx->verify_reads_expected) {
			/* All verification reads completed - trigger verification processing */
			/* Process verification in write_complete callback (it checks verification_in_progress) */
			/* Note: bdev_io is already freed by spdk_bdev_free_io above, so we pass NULL */
			ec_bdev_process_stripe_write_complete(NULL, true, cb_arg);
			return;
		}
		/* More verification reads pending */
		return;
	}
	
	/* Normal read completion */
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
		
		/* All reads completed - proceed with processing */
		if (process->type == EC_PROCESS_REBUILD) {
			/* Rebuild: decode and write to target */
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
		} else if (process->type == EC_PROCESS_REBALANCE) {
			/* Rebalance: re-encode and write to new devices */
			struct ec_rebalance_context *rebalance_ctx = ec_bdev->rebalance_ctx;
			uint32_t strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
			unsigned char *parity_ptrs[EC_MAX_P];
			uint8_t k = ec_bdev->k;
			uint8_t p = ec_bdev->p;
			uint8_t i, idx;
			/* 【修复】如果从parity设备读取了数据，需要先解码恢复失败的数据块 */
			/* Use saved failed data indices list instead of calculating */
			if (stripe_ctx->num_failed_data > 0 && stripe_ctx->reads_expected < k) {
				/* We read from parity devices - need to decode failed data fragments */
				/* Use the saved list of failed data fragment indices */
				uint8_t num_failed = stripe_ctx->num_failed_data;
				
				/* Prepare recovery pointers for failed data fragments */
				unsigned char *recover_ptrs[EC_MAX_K];
				for (i = 0; i < num_failed; i++) {
					uint8_t failed_idx = stripe_ctx->failed_data_indices[i];
					recover_ptrs[i] = stripe_ctx->data_ptrs[failed_idx];
				}
				
				/* Decode failed data fragments using saved indices */
				rc = ec_decode_stripe(ec_bdev, stripe_ctx->data_ptrs, recover_ptrs,
						      stripe_ctx->failed_data_indices, num_failed, strip_size_bytes);
				if (rc != 0) {
					SPDK_ERRLOG("Failed to decode %u failed data fragments for rebalance stripe %lu: %s\n",
						    num_failed, stripe_ctx->stripe_index, spdk_strerror(-rc));
					process->status = rc;
					process->error_status = rc;
					if (rebalance_ctx) {
						rebalance_ctx->error_status = rc;
					}
					ec_bdev_process_request_complete(process_req, rc);
					return;
				}
			}
			
			/* Prepare parity pointers */
			for (i = 0; i < p; i++) {
				parity_ptrs[i] = stripe_ctx->recover_buf + i * strip_size_bytes;
			}
			
			/* 【步骤4】重新编码生成新校验块 */
			rc = ec_encode_stripe(ec_bdev, stripe_ctx->data_ptrs, parity_ptrs, strip_size_bytes);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to re-encode stripe %lu during rebalance: %s\n",
					    stripe_ctx->stripe_index, spdk_strerror(-rc));
				process->status = rc;
				process->error_status = rc;
				if (rebalance_ctx) {
					rebalance_ctx->error_status = rc;
				}
				ec_bdev_process_request_complete(process_req, rc);
				return;
			}
			
			/* Save original data for verification comparison (before writing) */
			if (stripe_ctx->original_data_buf == NULL) {
				stripe_ctx->original_data_buf = spdk_dma_malloc(strip_size_bytes * k, ec_bdev->buf_alignment, NULL);
				if (stripe_ctx->original_data_buf == NULL) {
					SPDK_ERRLOG("Failed to allocate original data buffer for verification on stripe %lu\n",
						    stripe_ctx->stripe_index);
					process->status = -ENOMEM;
					process->error_status = -ENOMEM;
					if (rebalance_ctx) {
						rebalance_ctx->error_status = -ENOMEM;
					}
					ec_bdev_process_request_complete(process_req, -ENOMEM);
					return;
				}
				/* Copy original data before writing */
				for (i = 0; i < k; i++) {
					memcpy(stripe_ctx->original_data_buf + i * strip_size_bytes,
					       stripe_ctx->data_ptrs[i], strip_size_bytes);
				}
			}
			
			/* 【步骤5】写入新设备（k个数据块 + p个校验块） */
			/* Track writes */
			stripe_ctx->reads_completed = 0;  /* Reuse for write tracking */
			stripe_ctx->reads_expected = k + p;
			
			/* Write k data blocks */
			for (i = 0; i < k; i++) {
				idx = stripe_ctx->data_indices[i];
				struct ec_base_bdev_info *base_info = &ec_bdev->base_bdev_info[idx];
				
				if (base_info->desc == NULL || base_info->is_failed) {
					/* 【问题1修复】设备失败时，尝试重新选择设备而不是直接中止重平衡
					 * 局部重试机制：将该设备标记为不可用，重新触发设备选择
					 */
					SPDK_WARNLOG("New data device %u is not available for rebalance stripe %lu - attempting device reselection\n",
						    idx, stripe_ctx->stripe_index);
					
					/* 标记该设备在当前重平衡上下文中暂时不可用 */
					rebalance_ctx->device_failure_retry_count++;
					
					/* 如果重试次数过多，跳过该条带 */
					if (rebalance_ctx->device_failure_retry_count > 3) {
						SPDK_ERRLOG("Device reselection failed after %u retries for stripe %lu - skipping stripe\n",
							    rebalance_ctx->device_failure_retry_count, stripe_ctx->stripe_index);
						
						/* 记录失败的条带 */
						if (rebalance_ctx->num_failed_stripes >= rebalance_ctx->failed_stripes_capacity) {
							uint32_t new_capacity = rebalance_ctx->failed_stripes_capacity == 0 ? 
							    64 : rebalance_ctx->failed_stripes_capacity * 2;
							uint64_t *new_array = realloc(rebalance_ctx->failed_stripes,
												new_capacity * sizeof(uint64_t));
							if (new_array == NULL) {
								SPDK_ERRLOG("Failed to allocate memory for failed stripes list\n");
								/* 继续处理，但无法记录失败条带 */
							} else {
								rebalance_ctx->failed_stripes = new_array;
								rebalance_ctx->failed_stripes_capacity = new_capacity;
							}
						}
						if (rebalance_ctx->failed_stripes != NULL) {
							rebalance_ctx->failed_stripes[rebalance_ctx->num_failed_stripes++] = stripe_ctx->stripe_index;
						}
						
						/* 完成当前请求（标记为跳过） */
						ec_bdev_process_request_complete(process_req, 0);
						return;
					}
					
					/* 【问题2修复】重新选择设备：使用force_profile_id参数，避免全局状态切换竞态条件 */
					base_info->is_failed = true;
					
					/* 重新触发设备选择，使用force_profile_id参数而不是修改全局状态 */
					rc = ec_select_base_bdevs_wear_leveling_with_profile(ec_bdev, stripe_ctx->stripe_index,
										stripe_ctx->data_indices, stripe_ctx->parity_indices,
										rebalance_ctx->new_active_profile_id);
					
					/* 恢复设备状态 */
					base_info->is_failed = false;
					
					if (rc != 0) {
						SPDK_ERRLOG("Failed to reselect devices after failure for stripe %lu: %s\n",
							    stripe_ctx->stripe_index, spdk_strerror(-rc));
						/* 跳过该条带 */
						if (rebalance_ctx->num_failed_stripes >= rebalance_ctx->failed_stripes_capacity) {
							uint32_t new_capacity = rebalance_ctx->failed_stripes_capacity == 0 ? 
							    64 : rebalance_ctx->failed_stripes_capacity * 2;
							uint64_t *new_array = realloc(rebalance_ctx->failed_stripes,
												new_capacity * sizeof(uint64_t));
							if (new_array != NULL) {
								rebalance_ctx->failed_stripes = new_array;
								rebalance_ctx->failed_stripes_capacity = new_capacity;
							}
						}
						if (rebalance_ctx->failed_stripes != NULL) {
							rebalance_ctx->failed_stripes[rebalance_ctx->num_failed_stripes++] = stripe_ctx->stripe_index;
						}
						ec_bdev_process_request_complete(process_req, 0);
						return;
					}
					
					/* 更新条带上下文中的设备索引（函数会直接修改传入的数组） */
					/* 注意：ec_select_base_bdevs_wear_leveling_with_profile 会直接修改传入的数组，
					 * 所以 stripe_ctx->data_indices 和 stripe_ctx->parity_indices 已经被更新了 */
					
					/* 重置重试计数（成功重新选择） */
					rebalance_ctx->device_failure_retry_count = 0;
					
					/* 继续使用新选择的设备 */
					idx = stripe_ctx->data_indices[i];
					base_info = &ec_bdev->base_bdev_info[idx];
					
					/* 再次检查新选择的设备是否可用 */
					if (base_info->desc == NULL || base_info->is_failed) {
						SPDK_ERRLOG("Reselected data device %u is also not available for stripe %lu - skipping stripe\n",
							    idx, stripe_ctx->stripe_index);
						if (rebalance_ctx->num_failed_stripes >= rebalance_ctx->failed_stripes_capacity) {
							uint32_t new_capacity = rebalance_ctx->failed_stripes_capacity == 0 ? 
							    64 : rebalance_ctx->failed_stripes_capacity * 2;
							uint64_t *new_array = realloc(rebalance_ctx->failed_stripes,
												new_capacity * sizeof(uint64_t));
							if (new_array != NULL) {
								rebalance_ctx->failed_stripes = new_array;
								rebalance_ctx->failed_stripes_capacity = new_capacity;
							}
						}
						if (rebalance_ctx->failed_stripes != NULL) {
							rebalance_ctx->failed_stripes[rebalance_ctx->num_failed_stripes++] = stripe_ctx->stripe_index;
						}
						ec_bdev_process_request_complete(process_req, 0);
						return;
					}
				}
				
				uint64_t pd_lba = (stripe_ctx->stripe_index << ec_bdev->strip_size_shift) +
						  base_info->data_offset;
				
				rc = spdk_bdev_write_blocks(base_info->desc,
							    process->ec_ch->process.ch_processed->base_channel[idx],
							    stripe_ctx->data_ptrs[i], pd_lba, ec_bdev->strip_size,
							    ec_bdev_process_stripe_write_complete, process_req);
				if (rc != 0) {
					if (rc == -ENOMEM) {
						/* Queue wait and retry */
						process_req->ec_io.waitq_entry.bdev = spdk_bdev_desc_get_bdev(base_info->desc);
						process_req->ec_io.waitq_entry.cb_fn = ec_bdev_process_thread_run_wrapper;
						process_req->ec_io.waitq_entry.cb_arg = process;
						process_req->ec_io.waitq_entry.dep_unblock = true;
						spdk_bdev_queue_io_wait(process_req->ec_io.waitq_entry.bdev,
									process->ec_ch->process.ch_processed->base_channel[idx],
									&process_req->ec_io.waitq_entry);
						return;  /* Will retry */
					} else {
						SPDK_ERRLOG("Failed to write data to new device %u for rebalance stripe %lu: %s\n",
							    idx, stripe_ctx->stripe_index, spdk_strerror(-rc));
						process->status = rc;
						process->error_status = rc;
						if (rebalance_ctx) {
							rebalance_ctx->error_status = rc;
						}
						ec_bdev_process_request_complete(process_req, rc);
						return;
					}
				}
			}
			
			/* Write p parity blocks */
			for (i = 0; i < p; i++) {
				idx = stripe_ctx->parity_indices[i];
				struct ec_base_bdev_info *base_info = &ec_bdev->base_bdev_info[idx];
				
				if (base_info->desc == NULL || base_info->is_failed) {
					/* 【问题1修复】设备失败时，尝试重新选择设备而不是直接中止重平衡 */
					SPDK_WARNLOG("New parity device %u is not available for rebalance stripe %lu - attempting device reselection\n",
						    idx, stripe_ctx->stripe_index);
					
					/* 标记该设备在当前重平衡上下文中暂时不可用 */
					rebalance_ctx->device_failure_retry_count++;
					
					/* 如果重试次数过多，跳过该条带 */
					if (rebalance_ctx->device_failure_retry_count > 3) {
						SPDK_ERRLOG("Device reselection failed after %u retries for stripe %lu - skipping stripe\n",
							    rebalance_ctx->device_failure_retry_count, stripe_ctx->stripe_index);
						
						/* 记录失败的条带 */
						if (rebalance_ctx->num_failed_stripes >= rebalance_ctx->failed_stripes_capacity) {
							uint32_t new_capacity = rebalance_ctx->failed_stripes_capacity == 0 ? 
							    64 : rebalance_ctx->failed_stripes_capacity * 2;
							uint64_t *new_array = realloc(rebalance_ctx->failed_stripes,
												new_capacity * sizeof(uint64_t));
							if (new_array == NULL) {
								SPDK_ERRLOG("Failed to allocate memory for failed stripes list\n");
							} else {
								rebalance_ctx->failed_stripes = new_array;
								rebalance_ctx->failed_stripes_capacity = new_capacity;
							}
						}
						if (rebalance_ctx->failed_stripes != NULL) {
							rebalance_ctx->failed_stripes[rebalance_ctx->num_failed_stripes++] = stripe_ctx->stripe_index;
						}
						
						/* 完成当前请求（标记为跳过） */
						ec_bdev_process_request_complete(process_req, 0);
						return;
					}
					
					/* 【问题2修复】重新选择设备：使用force_profile_id参数，避免全局状态切换竞态条件 */
					base_info->is_failed = true;
					
					/* 重新触发设备选择，使用force_profile_id参数而不是修改全局状态 */
					rc = ec_select_base_bdevs_wear_leveling_with_profile(ec_bdev, stripe_ctx->stripe_index,
										stripe_ctx->data_indices, stripe_ctx->parity_indices,
										rebalance_ctx->new_active_profile_id);
					
					/* 恢复设备状态 */
					base_info->is_failed = false;
					
					if (rc != 0) {
						SPDK_ERRLOG("Failed to reselect devices after failure for stripe %lu: %s\n",
							    stripe_ctx->stripe_index, spdk_strerror(-rc));
						/* 跳过该条带 */
						if (rebalance_ctx->num_failed_stripes >= rebalance_ctx->failed_stripes_capacity) {
							uint32_t new_capacity = rebalance_ctx->failed_stripes_capacity == 0 ? 
							    64 : rebalance_ctx->failed_stripes_capacity * 2;
							uint64_t *new_array = realloc(rebalance_ctx->failed_stripes,
												new_capacity * sizeof(uint64_t));
							if (new_array != NULL) {
								rebalance_ctx->failed_stripes = new_array;
								rebalance_ctx->failed_stripes_capacity = new_capacity;
							}
						}
						if (rebalance_ctx->failed_stripes != NULL) {
							rebalance_ctx->failed_stripes[rebalance_ctx->num_failed_stripes++] = stripe_ctx->stripe_index;
						}
						ec_bdev_process_request_complete(process_req, 0);
						return;
					}
					
					/* 更新条带上下文中的设备索引（函数会直接修改传入的数组） */
					/* 注意：ec_select_base_bdevs_wear_leveling_with_profile 会直接修改传入的数组，
					 * 所以 stripe_ctx->data_indices 和 stripe_ctx->parity_indices 已经被更新了 */
					
					/* 重置重试计数（成功重新选择） */
					rebalance_ctx->device_failure_retry_count = 0;
					
					/* 继续使用新选择的设备 */
					idx = stripe_ctx->parity_indices[i];
					base_info = &ec_bdev->base_bdev_info[idx];
					
					/* 再次检查新选择的设备是否可用 */
					if (base_info->desc == NULL || base_info->is_failed) {
						SPDK_ERRLOG("Reselected parity device %u is also not available for stripe %lu - skipping stripe\n",
							    idx, stripe_ctx->stripe_index);
						if (rebalance_ctx->num_failed_stripes >= rebalance_ctx->failed_stripes_capacity) {
							uint32_t new_capacity = rebalance_ctx->failed_stripes_capacity == 0 ? 
							    64 : rebalance_ctx->failed_stripes_capacity * 2;
							uint64_t *new_array = realloc(rebalance_ctx->failed_stripes,
												new_capacity * sizeof(uint64_t));
							if (new_array != NULL) {
								rebalance_ctx->failed_stripes = new_array;
								rebalance_ctx->failed_stripes_capacity = new_capacity;
							}
						}
						if (rebalance_ctx->failed_stripes != NULL) {
							rebalance_ctx->failed_stripes[rebalance_ctx->num_failed_stripes++] = stripe_ctx->stripe_index;
						}
						ec_bdev_process_request_complete(process_req, 0);
						return;
					}
				}
				
				uint64_t pd_lba = (stripe_ctx->stripe_index << ec_bdev->strip_size_shift) +
						  base_info->data_offset;
				
				rc = spdk_bdev_write_blocks(base_info->desc,
							    process->ec_ch->process.ch_processed->base_channel[idx],
							    parity_ptrs[i], pd_lba, ec_bdev->strip_size,
							    ec_bdev_process_stripe_write_complete, process_req);
				if (rc != 0) {
					if (rc == -ENOMEM) {
						/* Queue wait and retry */
						process_req->ec_io.waitq_entry.bdev = spdk_bdev_desc_get_bdev(base_info->desc);
						process_req->ec_io.waitq_entry.cb_fn = ec_bdev_process_thread_run_wrapper;
						process_req->ec_io.waitq_entry.cb_arg = process;
						process_req->ec_io.waitq_entry.dep_unblock = true;
						spdk_bdev_queue_io_wait(process_req->ec_io.waitq_entry.bdev,
									process->ec_ch->process.ch_processed->base_channel[idx],
									&process_req->ec_io.waitq_entry);
						return;  /* Will retry */
					} else {
						SPDK_ERRLOG("Failed to write parity to new device %u for rebalance stripe %lu: %s\n",
							    idx, stripe_ctx->stripe_index, spdk_strerror(-rc));
						process->status = rc;
						process->error_status = rc;
						if (rebalance_ctx) {
							rebalance_ctx->error_status = rc;
						}
						ec_bdev_process_request_complete(process_req, rc);
						return;
					}
				}
			}
			
			return;  /* Writes submitted, will continue in write_complete callback */
		}
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
	uint8_t old_data_indices[EC_MAX_K];
	uint8_t old_parity_indices[EC_MAX_P];
	uint8_t i, idx;
	uint8_t num_available = 0;
	uint32_t strip_size_bytes;
	int rc;

	/* Try to reuse request from queue first (aligned with RAID framework) */
	process_req = TAILQ_FIRST(&process->requests);
	if (process_req != NULL) {
		/* Remove from queue - will be re-inserted on completion */
		TAILQ_REMOVE(&process->requests, process_req, link);
		/* Get stripe context from reused request */
		stripe_ctx = (struct ec_process_stripe_ctx *)(process_req + 1);
		/* Clear stripe context for reuse */
		memset(stripe_ctx, 0, sizeof(*stripe_ctx));
	} else {
		/* No request available in queue - allocate new one */
		process_req = NULL;
		stripe_ctx = NULL;
	}
	
	/* Handle rebalance differently from rebuild */
	if (process->type == EC_PROCESS_REBALANCE) {
		struct ec_rebalance_context *rebalance_ctx = ec_bdev->rebalance_ctx;
		struct ec_device_selection_config *config = &ec_bdev->selection_config;
		uint32_t group_id;
		uint16_t old_profile_id;
		uint8_t k = ec_bdev->k;
		uint8_t p = ec_bdev->p;
		
		if (rebalance_ctx == NULL) {
			SPDK_ERRLOG("Rebalance context is NULL for stripe %lu\n", stripe_index);
			if (process_req == NULL) {
				/* Only free if we allocated it */
			} else {
				/* Put back to queue on error */
				TAILQ_INSERT_TAIL(&process->requests, process_req, link);
			}
			return -EINVAL;
		}
		
		/* Allocate process request with embedded stripe context if not reused */
		if (process_req == NULL) {
			process_req = calloc(1, sizeof(*process_req) + sizeof(*stripe_ctx));
			if (process_req == NULL) {
				return -ENOMEM;
			}
			stripe_ctx = (struct ec_process_stripe_ctx *)(process_req + 1);
		}
		
		process_req->process = process;
		process_req->target = NULL;  /* Rebalance doesn't have a target */
		process_req->target_ch = NULL;
		stripe_ctx->process_req = process_req;
		stripe_ctx->stripe_index = stripe_index;
		
		/* Calculate group_id */
		group_id = ec_selection_calculate_group_id(config, stripe_index);
		
		/* 【步骤1】获取旧profile（通过group_profile_map，如果未绑定则使用old_active_profile_id） */
		if (config->group_profile_map != NULL && 
		    group_id < config->group_profile_capacity &&
		    config->group_profile_map[group_id] != 0) {
			/* Group已绑定，使用绑定的profile */
			old_profile_id = config->group_profile_map[group_id];
		} else {
			/* Group未绑定，使用重平衡前的active profile */
			old_profile_id = rebalance_ctx->old_active_profile_id;
		}
		
		/* 【问题2修复】使用force_profile_id参数选择旧设备，避免全局状态切换竞态条件 */
		rc = ec_select_base_bdevs_wear_leveling_with_profile(ec_bdev, stripe_index,
							old_data_indices, old_parity_indices,
							old_profile_id);
		
		if (rc != 0) {
			SPDK_ERRLOG("Failed to select old devices for rebalance stripe %lu: %s\n",
				    stripe_index, spdk_strerror(-rc));
			/* Put request back to queue on error (will be reused later) */
			TAILQ_INSERT_TAIL(&process->requests, process_req, link);
			return rc;
		}
		
		/* 【步骤2】使用新profile选择新设备
		 * 【问题2修复】使用force_profile_id参数，避免全局状态切换竞态条件
		 * 因为此时group_profile_map尚未更新（在验证通过后才更新），会回退到active_profile_id
		 */
		rc = ec_select_base_bdevs_wear_leveling_with_profile(ec_bdev, stripe_index,
							data_indices, parity_indices,
							rebalance_ctx->new_active_profile_id);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to select new devices for rebalance stripe %lu: %s\n",
				    stripe_index, spdk_strerror(-rc));
			/* Put request back to queue on error (will be reused later) */
			TAILQ_INSERT_TAIL(&process->requests, process_req, link);
			return rc;
		}
		
		/* Copy new indices to stripe context (for writing) */
		memcpy(stripe_ctx->data_indices, data_indices, sizeof(data_indices));
		memcpy(stripe_ctx->parity_indices, parity_indices, sizeof(parity_indices));
		
		/* Store old indices (data + parity) in available_indices for reading */
		/* 【问题7修复】包含parity设备，以便在数据设备失败时可以从parity恢复 */
		memcpy(stripe_ctx->available_indices, old_data_indices, k * sizeof(uint8_t));
		memcpy(stripe_ctx->available_indices + k, old_parity_indices, p * sizeof(uint8_t));
		stripe_ctx->num_available = k + p;  /* Include parity devices for recovery */
		
		/* Allocate buffers */
		strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
		stripe_ctx->stripe_buf = ec_get_rmw_stripe_buf(process->ec_ch, ec_bdev,
							      strip_size_bytes * k);
		if (stripe_ctx->stripe_buf == NULL) {
			SPDK_ERRLOG("Failed to allocate stripe buffer for rebalance\n");
			/* Put request back to queue on error (will be reused later) */
			TAILQ_INSERT_TAIL(&process->requests, process_req, link);
			return -ENOMEM;
		}
		
		/* Allocate parity buffer for re-encoding */
		stripe_ctx->recover_buf = spdk_dma_malloc(strip_size_bytes * p, ec_bdev->buf_alignment, NULL);
		if (stripe_ctx->recover_buf == NULL) {
			SPDK_ERRLOG("Failed to allocate parity buffer for rebalance\n");
			ec_put_rmw_stripe_buf(process->ec_ch, stripe_ctx->stripe_buf,
					     strip_size_bytes * k);
			/* Put request back to queue on error (will be reused later) */
			TAILQ_INSERT_TAIL(&process->requests, process_req, link);
			return -ENOMEM;
		}
		
		/* Prepare data pointers */
		for (i = 0; i < k; i++) {
			stripe_ctx->data_ptrs[i] = stripe_ctx->stripe_buf + i * strip_size_bytes;
		}
		
		/* 【步骤3】从旧设备读取k个数据块（并行） */
		stripe_ctx->reads_completed = 0;
		stripe_ctx->reads_expected = k;
		stripe_ctx->reads_submitted = 0;
		stripe_ctx->reads_retry_needed = false;
		
		/* Update counters - request will be inserted to queue on completion
		 * Aligned with RAID framework: window_remaining increases when request is submitted
		 */
		process->window_remaining++;
		process->window_stripes_submitted++;
		
		/* Submit reads from old devices */
		/* 【问题7修复】尝试从旧设备读取，如果失败则尝试从其他可用设备恢复 */
		uint8_t failed_data_indices[EC_MAX_K];
		uint8_t num_failed_data = 0;
		uint8_t num_available_read = 0;
		
		for (i = 0; i < k; i++) {
			idx = old_data_indices[i];
			base_info = &ec_bdev->base_bdev_info[idx];
			
			if (base_info->desc == NULL || base_info->is_failed) {
				/* Mark this data device as failed */
				failed_data_indices[num_failed_data++] = i;
				SPDK_WARNLOG("Old data device %u is not available for rebalance stripe %lu, will try recovery\n",
					     idx, stripe_ctx->stripe_index);
			} else {
				/* Device is available - submit read */
				uint64_t pd_lba = (stripe_ctx->stripe_index << ec_bdev->strip_size_shift) +
						  base_info->data_offset;
				
				rc = spdk_bdev_read_blocks(base_info->desc,
							   process->ec_ch->process.ch_processed->base_channel[idx],
							   stripe_ctx->data_ptrs[i], pd_lba, ec_bdev->strip_size,
							   ec_bdev_process_stripe_read_complete, process_req);
				if (rc != 0) {
					if (rc == -ENOMEM) {
						/* Queue wait and retry */
						process_req->ec_io.waitq_entry.bdev = spdk_bdev_desc_get_bdev(base_info->desc);
						process_req->ec_io.waitq_entry.cb_fn = ec_bdev_process_thread_run_wrapper;
						process_req->ec_io.waitq_entry.cb_arg = process;
						process_req->ec_io.waitq_entry.dep_unblock = true;
						spdk_bdev_queue_io_wait(process_req->ec_io.waitq_entry.bdev,
									process->ec_ch->process.ch_processed->base_channel[idx],
									&process_req->ec_io.waitq_entry);
						stripe_ctx->reads_expected = num_available_read;
						stripe_ctx->reads_submitted = num_available_read;
						stripe_ctx->reads_retry_needed = true;
						return 0;  /* Request is in queue, will retry */
					} else {
						/* Read failed - mark as failed and try recovery */
						failed_data_indices[num_failed_data++] = i;
						SPDK_WARNLOG("Failed to read from old data device %u for rebalance stripe %lu: %s, will try recovery\n",
							     idx, stripe_ctx->stripe_index, spdk_strerror(-rc));
					}
				} else {
					/* Read submitted successfully */
					num_available_read++;
					stripe_ctx->reads_submitted++;
				}
			}
		}
		
		/* If we have failed data devices, try to recover from parity devices */
		if (num_failed_data > 0) {
			/* Check if we have enough available devices (k total needed) */
			uint8_t total_available = num_available_read;
			uint8_t j;
			
			/* Count available parity devices */
			for (j = 0; j < p && total_available < k; j++) {
				idx = old_parity_indices[j];
				base_info = &ec_bdev->base_bdev_info[idx];
				if (base_info->desc != NULL && !base_info->is_failed) {
					total_available++;
				}
			}
			
			if (total_available < k) {
				SPDK_ERRLOG("Not enough available devices for rebalance stripe %lu: "
					    "need %u, have %u (failed data: %u)\n",
					    stripe_ctx->stripe_index, k, total_available, num_failed_data);
				/* Free buffers and put request back to queue */
				ec_put_rmw_stripe_buf(process->ec_ch, stripe_ctx->stripe_buf,
						     strip_size_bytes * k);
				spdk_dma_free(stripe_ctx->recover_buf);
				/* Put request back to queue on error (will be reused later) */
				TAILQ_INSERT_TAIL(&process->requests, process_req, link);
				return -ENODEV;
			}
			
			/* We have enough devices - read from parity devices to fill gaps */
			uint8_t parity_read_idx = 0;
			for (j = 0; j < num_failed_data && parity_read_idx < p; j++) {
				uint8_t failed_data_idx = failed_data_indices[j];
				
				/* Find next available parity device */
				while (parity_read_idx < p) {
					idx = old_parity_indices[parity_read_idx];
					base_info = &ec_bdev->base_bdev_info[idx];
					if (base_info->desc != NULL && !base_info->is_failed) {
						/* Read from parity device into data buffer (will decode later) */
						uint64_t pd_lba = (stripe_ctx->stripe_index << ec_bdev->strip_size_shift) +
								  base_info->data_offset;
						
						/* Use a temporary buffer for parity data - we'll decode later */
						/* For now, read into data_ptrs[failed_data_idx] - we'll reorganize in read_complete */
						rc = spdk_bdev_read_blocks(base_info->desc,
									   process->ec_ch->process.ch_processed->base_channel[idx],
									   stripe_ctx->data_ptrs[failed_data_idx], pd_lba, ec_bdev->strip_size,
									   ec_bdev_process_stripe_read_complete, process_req);
						if (rc != 0) {
							if (rc == -ENOMEM) {
								/* Queue wait and retry */
								process_req->ec_io.waitq_entry.bdev = spdk_bdev_desc_get_bdev(base_info->desc);
								process_req->ec_io.waitq_entry.cb_fn = ec_bdev_process_thread_run_wrapper;
								process_req->ec_io.waitq_entry.cb_arg = process;
								process_req->ec_io.waitq_entry.dep_unblock = true;
								spdk_bdev_queue_io_wait(process_req->ec_io.waitq_entry.bdev,
												process->ec_ch->process.ch_processed->base_channel[idx],
												&process_req->ec_io.waitq_entry);
								stripe_ctx->reads_expected = num_available_read + j;
								stripe_ctx->reads_submitted = num_available_read + j;
								stripe_ctx->reads_retry_needed = true;
								return 0;  /* Request is in queue, will retry */
							} else {
								SPDK_ERRLOG("Failed to read from old parity device %u for rebalance stripe %lu: %s\n",
									     idx, stripe_ctx->stripe_index, spdk_strerror(-rc));
								/* Free buffers and put request back to queue */
								ec_put_rmw_stripe_buf(process->ec_ch, stripe_ctx->stripe_buf,
										     strip_size_bytes * k);
								spdk_dma_free(stripe_ctx->recover_buf);
								/* Put request back to queue on error (will be reused later) */
								TAILQ_INSERT_TAIL(&process->requests, process_req, link);
								return rc;
							}
						}
						/* Read submitted successfully */
						num_available_read++;
						stripe_ctx->reads_submitted++;
						parity_read_idx++;
						break;  /* Move to next failed data device */
					}
					parity_read_idx++;
				}
			}
			
			/* Update reads_expected to include parity reads */
			stripe_ctx->reads_expected = num_available_read;
			/* Save complete list of failed data fragment indices for decoding */
			stripe_ctx->num_failed_data = num_failed_data;
			memcpy(stripe_ctx->failed_data_indices, failed_data_indices, num_failed_data * sizeof(uint8_t));
			/* Also store first failed fragment index for compatibility */
			stripe_ctx->failed_frag_idx = (num_failed_data > 0) ? failed_data_indices[0] : UINT8_MAX;
			
			/* Set up frag_map correctly for decoding:
			 * - data_indices[i] maps to logical fragment i (0 to k-1)
			 * - parity_indices[i] maps to logical fragment k+i (k to k+p-1)
			 * When we read from parity devices, we need to track which parity device
			 * was used to recover which failed data fragment
			 */
			memset(stripe_ctx->frag_map, 0xFF, sizeof(stripe_ctx->frag_map));
			for (j = 0; j < k; j++) {
				stripe_ctx->frag_map[old_data_indices[j]] = j;
			}
			for (j = 0; j < p; j++) {
				stripe_ctx->frag_map[old_parity_indices[j]] = k + j;
			}
		} else {
			/* No failed devices - normal path */
			stripe_ctx->reads_expected = k;
			stripe_ctx->num_failed_data = 0;
			stripe_ctx->failed_frag_idx = UINT8_MAX;
		}
		
		/* reads_submitted has been updated during the read submission loops above */
		/* Ensure it matches the expected number */
		if (stripe_ctx->reads_submitted != stripe_ctx->reads_expected) {
			SPDK_ERRLOG("Rebalance stripe %lu: reads_submitted (%u) != reads_expected (%u)\n",
				    stripe_ctx->stripe_index, stripe_ctx->reads_submitted, stripe_ctx->reads_expected);
		}
		
		return 0;  /* Reads submitted, will continue in read_complete callback */
	}
	
	/* Rebuild path continues here... */
	if (process->type == EC_PROCESS_REBUILD) {
		/* Allocate process request with embedded stripe context for rebuild */
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
					/* Put request back to queue on error (will be reused later) */
					if (process_req != NULL) {
						TAILQ_INSERT_TAIL(&process->requests, process_req, link);
					}
					return rc;
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
			/* Put request back to queue on error (will be reused later) */
			if (process_req != NULL) {
				TAILQ_INSERT_TAIL(&process->requests, process_req, link);
			}
			return rc;
		}
		
		/* Copy indices to stripe context */
		memcpy(stripe_ctx->data_indices, data_indices, sizeof(data_indices));
		memcpy(stripe_ctx->parity_indices, parity_indices, sizeof(parity_indices));
		
		/* Find available devices and target slot */
		uint8_t target_slot = ec_bdev_base_bdev_slot(process->target);
		num_available = 0;
		uint8_t rebuild_k = ec_bdev->k;
		uint8_t rebuild_p = ec_bdev->p;
		
		for (i = 0; i < rebuild_k; i++) {
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
		for (i = 0; i < rebuild_p; i++) {
			idx = parity_indices[i];
			stripe_ctx->frag_map[idx] = rebuild_k + i;
			base_info = &ec_bdev->base_bdev_info[idx];
			if (idx == target_slot) {
				stripe_ctx->failed_frag_idx = rebuild_k + i;
			} else if (base_info->desc != NULL && !base_info->is_failed) {
				if (process->ec_ch->process.ch_processed != NULL &&
				    process->ec_ch->process.ch_processed->base_channel[idx] != NULL) {
					stripe_ctx->available_indices[num_available++] = idx;
				}
			}
		}
		
		stripe_ctx->num_available = num_available;
		
		if (num_available < rebuild_k) {
			SPDK_ERRLOG("Not enough available blocks (%u < %u) for rebuild stripe %lu\n",
				    num_available, rebuild_k, stripe_index);
			/* Put request back to queue on error (will be reused later) */
			if (process_req != NULL) {
				TAILQ_INSERT_TAIL(&process->requests, process_req, link);
			}
			return -ENODEV;
		}
		
		/* Allocate buffers */
		uint32_t rebuild_strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
		stripe_ctx->stripe_buf = ec_get_rmw_stripe_buf(process->ec_ch, ec_bdev,
							      rebuild_strip_size_bytes * rebuild_k);
		if (stripe_ctx->stripe_buf == NULL) {
			SPDK_ERRLOG("Failed to allocate stripe buffer for rebuild\n");
			/* Put request back to queue on error (will be reused later) */
			if (process_req != NULL) {
				TAILQ_INSERT_TAIL(&process->requests, process_req, link);
			}
			return -ENOMEM;
		}
		
		stripe_ctx->recover_buf = spdk_dma_malloc(rebuild_strip_size_bytes, ec_bdev->buf_alignment, NULL);
		if (stripe_ctx->recover_buf == NULL) {
			SPDK_ERRLOG("Failed to allocate recovery buffer for rebuild\n");
			ec_put_rmw_stripe_buf(process->ec_ch, stripe_ctx->stripe_buf,
					     rebuild_strip_size_bytes * rebuild_k);
			/* Put request back to queue on error (will be reused later) */
			if (process_req != NULL) {
				TAILQ_INSERT_TAIL(&process->requests, process_req, link);
			}
			return -ENOMEM;
		}
		
		/* Prepare data pointers */
		for (i = 0; i < rebuild_k; i++) {
			stripe_ctx->data_ptrs[i] = stripe_ctx->stripe_buf + i * rebuild_strip_size_bytes;
		}
		
		/* Read k available blocks in parallel */
		process->rebuild_state = EC_REBUILD_STATE_READING;
		stripe_ctx->reads_completed = 0;
		stripe_ctx->reads_expected = rebuild_k;
		stripe_ctx->reads_submitted = 0;
		stripe_ctx->reads_retry_needed = false;
		
		/* Update counters - request will be inserted to queue on completion
		 * Aligned with RAID framework: window_remaining increases when request is submitted
		 */
		process->window_remaining++;
		process->window_stripes_submitted++;
		
		/* Submit reads from available devices */
		for (i = 0; i < rebuild_k; i++) {
			idx = stripe_ctx->available_indices[i];
			base_info = &ec_bdev->base_bdev_info[idx];
			
			if (base_info->desc == NULL || base_info->is_failed) {
				SPDK_ERRLOG("Device %u is not available for rebuild stripe %lu\n",
					    idx, stripe_ctx->stripe_index);
				/* Free buffers and put request back to queue */
				ec_put_rmw_stripe_buf(process->ec_ch, stripe_ctx->stripe_buf,
						     rebuild_strip_size_bytes * rebuild_k);
				spdk_dma_free(stripe_ctx->recover_buf);
				/* Put request back to queue on error (will be reused later) */
				TAILQ_INSERT_TAIL(&process->requests, process_req, link);
				return -ENODEV;
			}
			
			uint64_t pd_lba = (stripe_ctx->stripe_index << ec_bdev->strip_size_shift) +
					  base_info->data_offset;
			
			rc = spdk_bdev_read_blocks(base_info->desc,
						   process->ec_ch->process.ch_processed->base_channel[idx],
						   stripe_ctx->data_ptrs[i], pd_lba, ec_bdev->strip_size,
						   ec_bdev_process_stripe_read_complete, process_req);
			if (rc != 0) {
				if (rc == -ENOMEM) {
					/* Queue wait and retry */
					process_req->ec_io.waitq_entry.bdev = spdk_bdev_desc_get_bdev(base_info->desc);
					process_req->ec_io.waitq_entry.cb_fn = ec_bdev_process_thread_run_wrapper;
					process_req->ec_io.waitq_entry.cb_arg = process;
					process_req->ec_io.waitq_entry.dep_unblock = true;
					spdk_bdev_queue_io_wait(process_req->ec_io.waitq_entry.bdev,
								process->ec_ch->process.ch_processed->base_channel[idx],
								&process_req->ec_io.waitq_entry);
					stripe_ctx->reads_expected = i;
					stripe_ctx->reads_submitted = i;
					stripe_ctx->reads_retry_needed = true;
					return 0;  /* Request is in queue, will retry */
				} else {
					SPDK_ERRLOG("Failed to read from device %u for rebuild stripe %lu: %s\n",
						    idx, stripe_ctx->stripe_index, spdk_strerror(-rc));
					/* Free buffers and put request back to queue */
					ec_put_rmw_stripe_buf(process->ec_ch, stripe_ctx->stripe_buf,
							     rebuild_strip_size_bytes * rebuild_k);
					spdk_dma_free(stripe_ctx->recover_buf);
					/* Put request back to queue on error (will be reused later) */
					TAILQ_INSERT_TAIL(&process->requests, process_req, link);
					return rc;
				}
			}
			stripe_ctx->reads_submitted++;
		}
		
		return 0;  /* Reads submitted, will continue in read_complete callback */
	}

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
					
					/* Update window_remaining - request will be inserted to queue on completion */
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
				/* Update window_remaining - stripe was attempted (even if failed)
				 * Request will be inserted to queue on completion
				 */
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

	/* All reads submitted successfully - update counters
	 * Note: Request is NOT inserted to queue here - it will be inserted on completion
	 * Aligned with RAID framework: window_remaining increases when request is submitted
	 */
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
			/* Process complete - handle rebuild and rebalance differently */
			if (process->type == EC_PROCESS_REBUILD) {
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
			} else if (process->type == EC_PROCESS_REBALANCE) {
				/* Rebalance complete */
				SPDK_NOTICELOG("Rebalance completed on EC bdev %s\n",
					       ec_bdev->bdev.name);

				/* Call rebalance completion callback */
				if (process->rebalance_done_cb != NULL) {
					process->rebalance_done_cb(process->rebalance_done_ctx, 0);
				}
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

			/* Handle rebalance failure - rollback group_profile_map */
			if (process->type == EC_PROCESS_REBALANCE) {
				struct ec_rebalance_context *rebalance_ctx = ec_bdev->rebalance_ctx;
				if (rebalance_ctx != NULL) {
					struct ec_device_selection_config *config = &ec_bdev->selection_config;
					uint32_t group_id;
					
					SPDK_WARNLOG("EC[rebalance] Rolling back group_profile_map for failed rebalance on EC bdev %s\n",
						     ec_bdev->bdev.name);
					
					/* 【问题5修复】Rollback: 只回滚本次重平衡中更新的 group
					 * 而不是所有匹配 new_active_profile_id 的 group
					 * 这样可以避免回滚之前重平衡留下的 group
					 */
					if (config->group_profile_map != NULL && 
					    rebalance_ctx->updated_groups != NULL &&
					    rebalance_ctx->num_updated_groups > 0) {
						SPDK_WARNLOG("EC[rebalance] Rolling back %u groups updated during this rebalance\n",
							     rebalance_ctx->num_updated_groups);
						for (uint32_t i = 0; i < rebalance_ctx->num_updated_groups; i++) {
							group_id = rebalance_ctx->updated_groups[i];
							if (group_id < config->group_profile_capacity &&
							    config->group_profile_map[group_id] == rebalance_ctx->new_active_profile_id) {
								config->group_profile_map[group_id] = rebalance_ctx->old_active_profile_id;
							}
						}
						/* Mark as dirty to persist rollback */
						ec_selection_mark_group_dirty(ec_bdev);
					} else {
						/* 【问题2修复】如果没有跟踪信息，说明跟踪失败或未初始化
						 * 这种情况下，我们无法安全地回滚，只能记录错误
						 * 不应该使用fallback逻辑，因为可能回滚错误的group
						 */
						SPDK_ERRLOG("EC[rebalance] Cannot rollback: updated_groups tracking unavailable for EC bdev %s\n",
							     ec_bdev->bdev.name);
						SPDK_ERRLOG("EC[rebalance] Manual intervention may be required to fix group_profile_map\n");
						/* 不执行回滚，避免回滚错误的group */
					}
					
					/* Revert active_profile_id to old profile */
					config->active_profile_id = rebalance_ctx->old_active_profile_id;
				}
			}

			/* Call completion callback based on process type */
			if (process->type == EC_PROCESS_REBUILD) {
				if (process->rebuild_done_cb != NULL) {
					process->rebuild_done_cb(process->rebuild_done_ctx, process->status);
				}
			} else if (process->type == EC_PROCESS_REBALANCE) {
				if (process->rebalance_done_cb != NULL) {
					process->rebalance_done_cb(process->rebalance_done_ctx, process->status);
				}
			} else {
				/* For other types, use rebuild callback as default */
				if (process->rebuild_done_cb != NULL) {
					process->rebuild_done_cb(process->rebuild_done_ctx, process->status);
				}
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

	if (ec_bdev == NULL) {
		return -EINVAL;
	}

	/* Rebuild requires a target, but rebalance does not */
	if (type == EC_PROCESS_REBUILD && target == NULL) {
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
	/* Set callbacks based on process type */
	if (type == EC_PROCESS_REBUILD) {
		process->rebuild_done_cb = done_cb;
		process->rebuild_done_ctx = done_ctx;
		process->rebalance_done_cb = NULL;
		process->rebalance_done_ctx = NULL;
	} else if (type == EC_PROCESS_REBALANCE) {
		process->rebalance_done_cb = done_cb;
		process->rebalance_done_ctx = done_ctx;
		process->rebuild_done_cb = NULL;
		process->rebuild_done_ctx = NULL;
	} else {
		/* For other types, use rebuild callback as default */
		process->rebuild_done_cb = done_cb;
		process->rebuild_done_ctx = done_ctx;
		process->rebalance_done_cb = NULL;
		process->rebalance_done_ctx = NULL;
	}
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
	/* Call completion callback based on process type */
	if (process->type == EC_PROCESS_REBUILD) {
		if (process->rebuild_done_cb != NULL) {
			process->rebuild_done_cb(process->rebuild_done_ctx, -ECANCELED);
		}
	} else if (process->type == EC_PROCESS_REBALANCE) {
		if (process->rebalance_done_cb != NULL) {
			process->rebalance_done_cb(process->rebalance_done_ctx, -ECANCELED);
		}
	} else {
		/* For other types, use rebuild callback as default */
		if (process->rebuild_done_cb != NULL) {
			process->rebuild_done_cb(process->rebuild_done_ctx, -ECANCELED);
		}
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

