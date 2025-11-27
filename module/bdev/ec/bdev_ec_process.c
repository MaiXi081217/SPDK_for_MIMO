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

/* Forward declarations */
static void ec_bdev_process_thread_init(void *ctx);
static void ec_bdev_process_thread_run(struct ec_bdev_process *process);
static void ec_bdev_process_free(struct ec_bdev_process *process);
static int ec_bdev_ch_process_setup(struct ec_bdev_io_channel *ec_ch, struct ec_bdev_process *process);
static void ec_bdev_ch_process_cleanup(struct ec_bdev_io_channel *ec_ch);
static void ec_bdev_process_lock_window_range(struct ec_bdev_process *process);
static void ec_bdev_process_unlock_window_range(struct ec_bdev_process *process);
static void ec_bdev_process_window_range_locked(void *ctx, int status);
static void ec_bdev_process_window_range_unlocked(void *ctx, int status);
static int ec_bdev_submit_process_request(struct ec_bdev_process *process, uint64_t offset_blocks,
					  uint32_t num_blocks);
static void ec_bdev_process_stripe_read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void ec_bdev_process_stripe_write_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

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
	unsigned char *stripe_buf;
	unsigned char *recover_buf;
	unsigned char *data_ptrs[EC_MAX_K];
};

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
	struct spdk_io_channel *ch = spdk_get_io_channel(process->ec_bdev);
	if (ch == NULL) {
		SPDK_ERRLOG("Failed to get I/O channel for EC process on bdev %s\n",
			    process->ec_bdev->bdev.name);
		process->status = -ENOMEM;
		ec_bdev_process_thread_run(process);
		return;
	}
	process->ec_ch = spdk_io_channel_get_ctx(ch);
	
	/* Setup process channel */
	if (ec_bdev_ch_process_setup(process->ec_ch, process) != 0) {
		SPDK_ERRLOG("Failed to setup process channel for EC bdev %s\n",
			    process->ec_bdev->bdev.name);
		spdk_put_io_channel(ch);
		process->status = -ENOMEM;
		ec_bdev_process_thread_run(process);
		return;
	}
	
	process->state = EC_PROCESS_STATE_RUNNING;
	ec_bdev_process_thread_run(process);
}

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

	rc = spdk_bdev_unquiesce_range(&process->ec_bdev->bdev, &g_ec_if,
					process->window_offset, process->max_window_size,
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

/* Process thread run function - simplified version */
static void
ec_bdev_process_thread_run(struct ec_bdev_process *process)
{
	/* Check if we need to finish */
	if (process->status != 0 || process->state >= EC_PROCESS_STATE_STOPPING) {
		/* Will be handled in finish */
		return;
	}

	/* Lock first window */
	if (!process->window_range_locked) {
		ec_bdev_process_lock_window_range(process);
	}
}

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

