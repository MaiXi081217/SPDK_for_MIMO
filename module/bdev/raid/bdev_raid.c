/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "bdev_raid.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/json.h"
#include "spdk/likely.h"
#include "spdk/trace.h"
#include "spdk_internal/trace_defs.h"

#define RAID_OFFSET_BLOCKS_INVALID	UINT64_MAX
#define RAID_BDEV_PROCESS_MAX_QD	16

#define RAID_BDEV_PROCESS_WINDOW_SIZE_KB_DEFAULT	1024
#define RAID_BDEV_PROCESS_MAX_BANDWIDTH_MB_SEC_DEFAULT	0

static bool g_shutdown_started = false;

/* List of all raid bdevs */
struct raid_all_tailq g_raid_bdev_list = TAILQ_HEAD_INITIALIZER(g_raid_bdev_list);

static TAILQ_HEAD(, raid_bdev_module) g_raid_modules = TAILQ_HEAD_INITIALIZER(g_raid_modules);

/*
 * raid_bdev_io_channel is the context of spdk_io_channel for raid bdev device. It
 * contains the relationship of raid bdev io channel with base bdev io channels.
 */
struct raid_bdev_io_channel {
	/* Array of IO channels of base bdevs */
	struct spdk_io_channel	**base_channel;

	/* Private raid module IO channel */
	struct spdk_io_channel	*module_channel;

	/* Background process data */
	struct {
		uint64_t offset;
		struct spdk_io_channel *target_ch;
		struct raid_bdev_io_channel *ch_processed;
	} process;
};

enum raid_bdev_process_state {
	RAID_PROCESS_STATE_INIT,
	RAID_PROCESS_STATE_RUNNING,
	RAID_PROCESS_STATE_STOPPING,
	RAID_PROCESS_STATE_STOPPED,
};

struct raid_process_qos {
	bool enable_qos;
	uint64_t last_tsc;
	double bytes_per_tsc;
	double bytes_available;
	double bytes_max;
	struct spdk_poller *process_continue_poller;
};

struct raid_bdev_process {
	struct raid_bdev		*raid_bdev;
	enum raid_process_type		type;
	enum raid_bdev_process_state	state;
	struct spdk_thread		*thread;
	struct raid_bdev_io_channel	*raid_ch;
	TAILQ_HEAD(, raid_bdev_process_request) requests;
	uint64_t			max_window_size;
	uint64_t			window_size;
	uint64_t			window_remaining;
	int				window_status;
	uint64_t			window_offset;
	bool				window_range_locked;
	struct raid_base_bdev_info	*target;
	int				status;
	TAILQ_HEAD(, raid_process_finish_action) finish_actions;
	struct raid_process_qos		qos;
	/* Fine-grained rebuild state tracking */
	enum raid_rebuild_state		rebuild_state;
	/* Callback for rebuild completion */
	void (*rebuild_done_cb)(void *ctx, int status);
	void *rebuild_done_ctx;
	/* Simplified rebuild: error flag (atomic) for unified error handling */
	volatile int			error_status;
};

static inline struct raid_bdev_process *
raid_bdev_get_active_process(struct raid_bdev *raid_bdev)
{
	struct raid_bdev_process *process;

	if (raid_bdev == NULL) {
		return NULL;
	}

	process = raid_bdev->process;
	if (process == NULL || process->state == RAID_PROCESS_STATE_STOPPED) {
		return NULL;
	}

	return process;
}

static inline bool
raid_bdev_process_is_running_type(const struct raid_bdev *raid_bdev, enum raid_process_type type)
{
	struct raid_bdev_process *process;

	if (raid_bdev == NULL) {
		return false;
	}

	process = raid_bdev->process;
	return process != NULL &&
	       process->type == type &&
	       process->state == RAID_PROCESS_STATE_RUNNING;
}

struct raid_process_finish_action {
	spdk_msg_fn cb;
	void *cb_ctx;
	TAILQ_ENTRY(raid_process_finish_action) link;
};

static struct spdk_raid_bdev_opts g_opts = {
	.process_window_size_kb = RAID_BDEV_PROCESS_WINDOW_SIZE_KB_DEFAULT,
	.process_max_bandwidth_mb_sec = RAID_BDEV_PROCESS_MAX_BANDWIDTH_MB_SEC_DEFAULT,
};

void
raid_bdev_get_opts(struct spdk_raid_bdev_opts *opts)
{
	*opts = g_opts;
}

int
raid_bdev_set_opts(const struct spdk_raid_bdev_opts *opts)
{
	if (opts->process_window_size_kb == 0) {
		return -EINVAL;
	}

	g_opts = *opts;

	return 0;
}

static struct raid_bdev_module *
raid_bdev_module_find(enum raid_level level)
{
	struct raid_bdev_module *raid_module;

	TAILQ_FOREACH(raid_module, &g_raid_modules, link) {
		if (raid_module->level == level) {
			return raid_module;
		}
	}

	return NULL;
}

void
raid_bdev_module_list_add(struct raid_bdev_module *raid_module)
{
	if (raid_bdev_module_find(raid_module->level) != NULL) {
		SPDK_ERRLOG("module for raid level '%s' already registered.\n",
			    raid_bdev_level_to_str(raid_module->level));
		assert(false);
	} else {
		TAILQ_INSERT_TAIL(&g_raid_modules, raid_module, link);
	}
}

struct spdk_io_channel *
raid_bdev_channel_get_base_channel(struct raid_bdev_io_channel *raid_ch, uint8_t idx)
{
	return raid_ch->base_channel[idx];
}

void *
raid_bdev_channel_get_module_ctx(struct raid_bdev_io_channel *raid_ch)
{
	assert(raid_ch->module_channel != NULL);

	return spdk_io_channel_get_ctx(raid_ch->module_channel);
}

struct raid_base_bdev_info *
raid_bdev_channel_get_base_info(struct raid_bdev_io_channel *raid_ch, struct spdk_bdev *base_bdev)
{
	struct spdk_io_channel *ch = spdk_io_channel_from_ctx(raid_ch);
	struct raid_bdev *raid_bdev = spdk_io_channel_get_io_device(ch);
	uint8_t i;

	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		struct raid_base_bdev_info *base_info = &raid_bdev->base_bdev_info[i];

		if (base_info->is_configured &&
		    spdk_bdev_desc_get_bdev(base_info->desc) == base_bdev) {
			return base_info;
		}
	}

	return NULL;
}

/* Function declarations */
static void	raid_bdev_examine(struct spdk_bdev *bdev);
static int	raid_bdev_init(void);
static void	raid_bdev_deconfigure(struct raid_bdev *raid_bdev,
				      raid_bdev_destruct_cb cb_fn, void *cb_arg);
/* Forward declarations for delete flow helpers */
static void	raid_bdev_delete_continue(struct raid_bdev *raid_bdev, raid_bdev_destruct_cb cb_fn,
			       void *cb_arg);
static void	raid_bdev_delete_on_quiesced(void *arg, int status);
static void	raid_bdev_delete_on_unquiesced(void *arg, int status);
static void	raid_bdev_delete_wipe_cb(int status, struct raid_bdev *raid_bdev, void *arg);
static void	raid_bdev_delete_restore_sb_cb(int restore_status, struct raid_bdev *raid_bdev,
			       void *arg);

struct raid_bdev_async_sb_ctx {
	uint32_t attempt;
};

#define RAID_BDEV_ASYNC_SB_MAX_RETRIES 3

static void
raid_bdev_write_superblock_async_cb(int status, struct raid_bdev *raid_bdev, void *ctx)
{
	struct raid_bdev_async_sb_ctx *async_ctx = ctx;

	if (status != 0) {
		async_ctx->attempt++;
		if (async_ctx->attempt < RAID_BDEV_ASYNC_SB_MAX_RETRIES) {
			SPDK_WARNLOG("Asynchronous superblock write failed on raid bdev %s (attempt %u/%u), retrying: %s\n",
				     raid_bdev->bdev.name, async_ctx->attempt, RAID_BDEV_ASYNC_SB_MAX_RETRIES,
				     spdk_strerror(-status));
			raid_bdev_write_superblock(raid_bdev, raid_bdev_write_superblock_async_cb, async_ctx);
			return;
		}

		SPDK_ERRLOG("Asynchronous superblock write failed on raid bdev %s after %u attempts: %s\n",
			    raid_bdev->bdev.name, RAID_BDEV_ASYNC_SB_MAX_RETRIES, spdk_strerror(-status));
	}

	free(async_ctx);
}

static void
raid_bdev_write_superblock_async(struct raid_bdev *raid_bdev)
{
	struct raid_bdev_async_sb_ctx *ctx;

	if (raid_bdev == NULL || raid_bdev->sb == NULL) {
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate async context for superblock write on raid bdev %s\n",
			    raid_bdev->bdev.name);
		return;
	}

	raid_bdev_write_superblock(raid_bdev, raid_bdev_write_superblock_async_cb, ctx);
}

static void
raid_bdev_ch_process_cleanup(struct raid_bdev_io_channel *raid_ch)
{
	raid_ch->process.offset = RAID_OFFSET_BLOCKS_INVALID;

	if (raid_ch->process.target_ch != NULL) {
		spdk_put_io_channel(raid_ch->process.target_ch);
		raid_ch->process.target_ch = NULL;
	}

	if (raid_ch->process.ch_processed != NULL) {
		free(raid_ch->process.ch_processed->base_channel);
		free(raid_ch->process.ch_processed);
		raid_ch->process.ch_processed = NULL;
	}
}

static int
raid_bdev_ch_process_setup(struct raid_bdev_io_channel *raid_ch, struct raid_bdev_process *process)
{
	struct raid_bdev *raid_bdev = process->raid_bdev;
	struct raid_bdev_io_channel *raid_ch_processed;
	struct raid_base_bdev_info *base_info;

	raid_ch->process.offset = process->window_offset;

	/* In the future we may have other types of processes which don't use a target bdev,
	 * like data scrubbing or strip size migration. Until then, expect that there always is
	 * a process target. */
	assert(process->target != NULL);

	/* Check if target disk is still available (may have been physically removed) */
	if (process->target->desc == NULL) {
		SPDK_ERRLOG("Target disk '%s' descriptor is NULL - disk may have been removed during rebuild\n",
			    process->target->name ? process->target->name : "unknown");
		goto err;
	}

	raid_ch->process.target_ch = spdk_bdev_get_io_channel(process->target->desc);
	if (raid_ch->process.target_ch == NULL) {
		SPDK_ERRLOG("Failed to get I/O channel for target disk '%s' - disk may have been removed\n",
			    process->target->name ? process->target->name : "unknown");
		goto err;
	}

	raid_ch_processed = calloc(1, sizeof(*raid_ch_processed));
	if (raid_ch_processed == NULL) {
		goto err;
	}
	raid_ch->process.ch_processed = raid_ch_processed;

	raid_ch_processed->base_channel = calloc(raid_bdev->num_base_bdevs,
					  sizeof(*raid_ch_processed->base_channel));
	if (raid_ch_processed->base_channel == NULL) {
		goto err;
	}

	/* For rebuild: check if we have enough base bdevs available */
	uint8_t available_base_bdevs = 0;
	bool *pair_has_available = NULL;
	uint8_t num_pairs = 0;
	
	/* For RAID10 rebuild: track which mirror pairs have at least one available disk */
	if (process->type == RAID_PROCESS_REBUILD && raid_bdev->level == RAID10) {
		/* For RAID10, num_mirror_pairs = num_base_bdevs / 2 */
		if (raid_bdev->num_base_bdevs % 2 == 0) {
			num_pairs = raid_bdev->num_base_bdevs / 2;
			pair_has_available = calloc(num_pairs, sizeof(bool));
			if (pair_has_available == NULL) {
				goto err;
			}
		}
	}
	
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		uint8_t slot = raid_bdev_base_bdev_slot(base_info);

		/* Safety check: ensure slot is within valid range */
		if (slot >= raid_bdev->num_base_bdevs) {
			SPDK_ERRLOG("Calculated slot %u exceeds num_base_bdevs %u\n",
				    slot, raid_bdev->num_base_bdevs);
			/* Slot calculation error is a serious issue - return error instead of continuing */
			free(pair_has_available);
			goto err;
		}

		if (base_info != process->target) {
			if (raid_ch->base_channel != NULL) {
				raid_ch_processed->base_channel[slot] = raid_ch->base_channel[slot];
				if (raid_ch_processed->base_channel[slot] != NULL) {
					available_base_bdevs++;
					/* For RAID10: mark the mirror pair as having an available disk */
					if (pair_has_available != NULL) {
						uint8_t pair_idx = slot / 2;
						if (pair_idx < num_pairs) {
							pair_has_available[pair_idx] = true;
						}
					}
				}
			}
		} else {
			raid_ch_processed->base_channel[slot] = raid_ch->process.target_ch;
			/* target_ch is already checked above, so it's available */
			available_base_bdevs++;
		}
	}

	/* For rebuild: ensure we have enough base bdevs to perform rebuild */
	if (process->type == RAID_PROCESS_REBUILD) {
		bool sufficient = false;
		
		if (raid_bdev->level == RAID10 && pair_has_available != NULL) {
			/* For RAID10: check that each mirror pair (excluding target's pair) has at least one available disk */
			uint8_t target_slot = raid_bdev_base_bdev_slot(process->target);
			uint8_t target_pair = target_slot / 2;
			uint8_t pairs_with_available = 0;
			
			for (uint8_t i = 0; i < num_pairs; i++) {
				if (i == target_pair) {
					/* Target's pair: we need the other disk in the pair to be available */
					uint8_t other_disk = (target_slot % 2 == 0) ? (target_slot + 1) : (target_slot - 1);
					if (other_disk < raid_bdev->num_base_bdevs &&
					    raid_ch_processed->base_channel[other_disk] != NULL) {
						pairs_with_available++;
					}
				} else if (pair_has_available[i]) {
					pairs_with_available++;
				}
			}
			
			/* We need all pairs except target's pair to have at least one available disk */
			sufficient = (pairs_with_available >= num_pairs - 1);
		} else {
			/* For other RAID levels: need at least min_base_bdevs_operational */
			sufficient = (available_base_bdevs >= raid_bdev->min_base_bdevs_operational);
		}
		
		if (!sufficient) {
			SPDK_ERRLOG("Insufficient base bdevs available for rebuild on RAID bdev '%s': "
				    "%u available, need at least %u\n",
				    raid_bdev->bdev.name, available_base_bdevs,
				    raid_bdev->min_base_bdevs_operational);
			if (pair_has_available != NULL) {
				free(pair_has_available);
			}
			goto err;
		}
	}
	
	if (pair_has_available != NULL) {
		free(pair_has_available);
	}

	raid_ch_processed->module_channel = raid_ch->module_channel;
	raid_ch_processed->process.offset = RAID_OFFSET_BLOCKS_INVALID;

	return 0;
err:
	raid_bdev_ch_process_cleanup(raid_ch);
	/* Return appropriate error code based on the failure reason */
	/* Most errors are logged above, but we return a generic error code */
	/* The actual error reason is logged, so callers can check logs for details */
	/* Note: We could track the specific error, but it would require additional state tracking */
	return -ENOMEM;
}

/*
 * brief:
 * raid_bdev_create_cb function is a cb function for raid bdev which creates the
 * hierarchy from raid bdev to base bdev io channels. It will be called per core
 * params:
 * io_device - pointer to raid bdev io device represented by raid_bdev
 * ctx_buf - pointer to context buffer for raid bdev io channel
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
raid_bdev_create_cb(void *io_device, void *ctx_buf)
{
	struct raid_bdev            *raid_bdev = io_device;
	struct raid_bdev_io_channel *raid_ch = ctx_buf;
	uint8_t i;
	int ret = -ENOMEM;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_create_cb, %p\n", raid_ch);

	assert(raid_bdev != NULL);
	assert(raid_bdev->state == RAID_BDEV_STATE_ONLINE);

	raid_ch->base_channel = calloc(raid_bdev->num_base_bdevs, sizeof(struct spdk_io_channel *));
	if (!raid_ch->base_channel) {
		SPDK_ERRLOG("Unable to allocate base bdevs io channel\n");
		return -ENOMEM;
	}

	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		/*
		 * Get the spdk_io_channel for all the base bdevs. This is used during
		 * split logic to send the respective child bdev ios to respective base
		 * bdev io channel.
		 * Skip missing base bdevs and the process target, which should also be treated as
		 * missing until the process completes.
		 */
		if (raid_bdev->base_bdev_info[i].is_configured == false ||
		    raid_bdev->base_bdev_info[i].is_process_target == true) {
			continue;
		}
		raid_ch->base_channel[i] = spdk_bdev_get_io_channel(
						   raid_bdev->base_bdev_info[i].desc);
		if (!raid_ch->base_channel[i]) {
			SPDK_ERRLOG("Unable to create io channel for base bdev\n");
			goto err;
		}
	}

	if (raid_bdev->module->get_io_channel) {
		raid_ch->module_channel = raid_bdev->module->get_io_channel(raid_bdev);
		if (!raid_ch->module_channel) {
			SPDK_ERRLOG("Unable to create io channel for raid module\n");
			goto err;
		}
	}

	struct raid_bdev_process *process = raid_bdev_get_active_process(raid_bdev);

	if (process != NULL) {
		ret = raid_bdev_ch_process_setup(raid_ch, process);
		if (ret != 0) {
			SPDK_ERRLOG("Failed to setup process io channel\n");
			goto err;
		}
	} else {
		raid_ch->process.offset = RAID_OFFSET_BLOCKS_INVALID;
	}

	return 0;
err:
	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		if (raid_ch->base_channel[i] != NULL) {
			spdk_put_io_channel(raid_ch->base_channel[i]);
		}
	}
	free(raid_ch->base_channel);

	raid_bdev_ch_process_cleanup(raid_ch);

	return ret;
}

/*
 * brief:
 * raid_bdev_destroy_cb function is a cb function for raid bdev which deletes the
 * hierarchy from raid bdev to base bdev io channels. It will be called per core
 * params:
 * io_device - pointer to raid bdev io device represented by raid_bdev
 * ctx_buf - pointer to context buffer for raid bdev io channel
 * returns:
 * none
 */
static void
raid_bdev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct raid_bdev *raid_bdev = io_device;
	struct raid_bdev_io_channel *raid_ch = ctx_buf;
	uint8_t i;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_destroy_cb\n");

	assert(raid_ch != NULL);
	assert(raid_ch->base_channel);

	if (raid_ch->module_channel) {
		spdk_put_io_channel(raid_ch->module_channel);
	}

	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		/* Free base bdev channels */
		if (raid_ch->base_channel[i] != NULL) {
			spdk_put_io_channel(raid_ch->base_channel[i]);
		}
	}
	free(raid_ch->base_channel);
	raid_ch->base_channel = NULL;

	raid_bdev_ch_process_cleanup(raid_ch);
}

/*
 * brief:
 * raid_bdev_cleanup is used to cleanup raid_bdev related data
 * structures.
 * params:
 * raid_bdev - pointer to raid_bdev
 * returns:
 * none
 */
static void
raid_bdev_cleanup(struct raid_bdev *raid_bdev)
{
	struct raid_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_cleanup, %p name %s, state %s\n",
		      raid_bdev, raid_bdev->bdev.name, raid_bdev_state_to_str(raid_bdev->state));
	assert(raid_bdev->state != RAID_BDEV_STATE_ONLINE);
	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		assert(base_info->desc == NULL);
		free(base_info->name);
	}

	TAILQ_REMOVE(&g_raid_bdev_list, raid_bdev, global_link);
}

static void
raid_bdev_free(struct raid_bdev *raid_bdev)
{
	raid_bdev_free_superblock(raid_bdev);
	free(raid_bdev->base_bdev_info);
	free(raid_bdev->bdev.name);
	free(raid_bdev);
}

static void
raid_bdev_cleanup_and_free(struct raid_bdev *raid_bdev)
{
	raid_bdev_cleanup(raid_bdev);
	raid_bdev_free(raid_bdev);
}

static void
raid_bdev_deconfigure_base_bdev(struct raid_base_bdev_info *base_info)
{
	struct raid_bdev *raid_bdev = base_info->raid_bdev;

	assert(base_info->is_configured);
	assert(raid_bdev->num_base_bdevs_discovered);
	raid_bdev->num_base_bdevs_discovered--;
	base_info->is_configured = false;
	base_info->is_process_target = false;
}

/*
 * brief:
 * free resource of base bdev for raid bdev
 * params:
 * base_info - raid base bdev info
 * returns:
 * none
 */
static void
raid_bdev_free_base_bdev_resource(struct raid_base_bdev_info *base_info)
{
	struct raid_bdev *raid_bdev;

	if (base_info == NULL) {
		return;
	}

	raid_bdev = base_info->raid_bdev;
	if (raid_bdev == NULL) {
		/* If raid_bdev is NULL, we can only clean up what we can */
		free(base_info->name);
		base_info->name = NULL;
		spdk_uuid_set_null(&base_info->uuid);
		base_info->is_failed = false;
		base_info->data_offset = 0;
		base_info->is_configured = false;
		base_info->is_process_target = false;
		base_info->remove_scheduled = false;
		return;
	}

	assert(spdk_get_thread() == spdk_thread_get_app_thread());
	assert(base_info->configure_cb == NULL);

	free(base_info->name);
	base_info->name = NULL;
	if (raid_bdev->state != RAID_BDEV_STATE_CONFIGURING) {
		spdk_uuid_set_null(&base_info->uuid);
	}
	base_info->is_failed = false;

	/* clear `data_offset` to allow it to be recalculated during configuration */
	base_info->data_offset = 0;

	/* Always deconfigure if configured, regardless of desc state */
	if (base_info->is_configured) {
		raid_bdev_deconfigure_base_bdev(base_info);
	}

	/* Only close desc and channel if they exist */
	if (base_info->desc != NULL) {
		spdk_bdev_module_release_bdev(spdk_bdev_desc_get_bdev(base_info->desc));
		spdk_bdev_close(base_info->desc);
		base_info->desc = NULL;
	}

	if (base_info->app_thread_ch != NULL) {
		spdk_put_io_channel(base_info->app_thread_ch);
		base_info->app_thread_ch = NULL;
	}

	/* Clear remove_scheduled flag to allow this slot to be reused */
	base_info->remove_scheduled = false;
}

static void
raid_bdev_io_device_unregister_cb(void *io_device)
{
	struct raid_bdev *raid_bdev = io_device;

	if (raid_bdev->num_base_bdevs_discovered == 0) {
		/* Free raid_bdev when there are no base bdevs left */
		SPDK_DEBUGLOG(bdev_raid, "raid bdev base bdevs is 0, going to free all in destruct\n");
		raid_bdev_cleanup(raid_bdev);
		spdk_bdev_destruct_done(&raid_bdev->bdev, 0);
		raid_bdev_free(raid_bdev);
	} else {
		spdk_bdev_destruct_done(&raid_bdev->bdev, 0);
	}
}

void
raid_bdev_module_stop_done(struct raid_bdev *raid_bdev)
{
	if (raid_bdev->state != RAID_BDEV_STATE_CONFIGURING) {
		spdk_io_device_unregister(raid_bdev, raid_bdev_io_device_unregister_cb);
	}
}

static void
_raid_bdev_destruct(void *ctxt)
{
	struct raid_bdev *raid_bdev = ctxt;
	struct raid_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_destruct\n");

	assert(raid_bdev->process == NULL);

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		/*
		 * Close all base bdev descriptors for which call has come from below
		 * layers.  Also close the descriptors if we have started shutdown.
		 */
		if (g_shutdown_started || base_info->remove_scheduled == true) {
			raid_bdev_free_base_bdev_resource(base_info);
		}
	}

	if (g_shutdown_started) {
		raid_bdev->state = RAID_BDEV_STATE_OFFLINE;
	}

	if (raid_bdev->module->stop != NULL) {
		if (raid_bdev->module->stop(raid_bdev) == false) {
			return;
		}
	}

	raid_bdev_module_stop_done(raid_bdev);
}

static int
raid_bdev_destruct(void *ctx)
{
	spdk_thread_exec_msg(spdk_thread_get_app_thread(), _raid_bdev_destruct, ctx);

	return 1;
}

int
raid_bdev_remap_dix_reftag(void *md_buf, uint64_t num_blocks,
			   struct spdk_bdev *bdev, uint32_t remapped_offset)
{
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_error err_blk = {};
	int rc;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct iovec md_iov = {
		.iov_base	= md_buf,
		.iov_len	= num_blocks * bdev->md_len,
	};

	if (md_buf == NULL) {
		return 0;
	}

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = bdev->dif_pi_format;
	rc = spdk_dif_ctx_init(&dif_ctx,
			       bdev->blocklen, bdev->md_len, bdev->md_interleave,
			       bdev->dif_is_head_of_md, bdev->dif_type,
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	if (rc != 0) {
		SPDK_ERRLOG("Initialization of DIF context failed\n");
		return rc;
	}

	spdk_dif_ctx_set_remapped_init_ref_tag(&dif_ctx, remapped_offset);

	rc = spdk_dix_remap_ref_tag(&md_iov, num_blocks, &dif_ctx, &err_blk, false);
	if (rc != 0) {
		SPDK_ERRLOG("Remapping reference tag failed. type=%d, offset=%d"
			    PRIu32 "\n", err_blk.err_type, err_blk.err_offset);
	}

	return rc;
}

int
raid_bdev_verify_dix_reftag(struct iovec *iovs, int iovcnt, void *md_buf,
			    uint64_t num_blocks, struct spdk_bdev *bdev, uint32_t offset_blocks)
{
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_error err_blk = {};
	int rc;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct iovec md_iov = {
		.iov_base	= md_buf,
		.iov_len	= num_blocks * bdev->md_len,
	};

	if (md_buf == NULL) {
		return 0;
	}

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = bdev->dif_pi_format;
	rc = spdk_dif_ctx_init(&dif_ctx,
			       bdev->blocklen, bdev->md_len, bdev->md_interleave,
			       bdev->dif_is_head_of_md, bdev->dif_type,
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       offset_blocks, 0, 0, 0, 0, &dif_opts);
	if (rc != 0) {
		SPDK_ERRLOG("Initialization of DIF context failed\n");
		return rc;
	}

	rc = spdk_dix_verify(iovs, iovcnt, &md_iov, num_blocks, &dif_ctx, &err_blk);
	if (rc != 0) {
		SPDK_ERRLOG("Reference tag check failed. type=%d, offset=%d"
			    PRIu32 "\n", err_blk.err_type, err_blk.err_offset);
	}

	return rc;
}

void
raid_bdev_io_complete(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(raid_io);
	int rc;

	spdk_trace_record(TRACE_BDEV_RAID_IO_DONE, 0, 0, (uintptr_t)raid_io, (uintptr_t)bdev_io);

	if (raid_io->split.offset != RAID_OFFSET_BLOCKS_INVALID) {
		struct iovec *split_iov = raid_io->split.iov;
		const struct iovec *split_iov_orig = &raid_io->split.iov_copy;

		/*
		 * Non-zero offset here means that this is the completion of the first part of the
		 * split I/O (the higher LBAs). Then, we submit the second part and set offset to 0.
		 */
		if (raid_io->split.offset != 0) {
			raid_io->offset_blocks = bdev_io->u.bdev.offset_blocks;
			raid_io->md_buf = bdev_io->u.bdev.md_buf;

			if (status == SPDK_BDEV_IO_STATUS_SUCCESS) {
				raid_io->num_blocks = raid_io->split.offset;
				raid_io->iovcnt = raid_io->iovs - bdev_io->u.bdev.iovs;
				raid_io->iovs = bdev_io->u.bdev.iovs;
				if (split_iov != NULL) {
					raid_io->iovcnt++;
					split_iov->iov_len = split_iov->iov_base - split_iov_orig->iov_base;
					split_iov->iov_base = split_iov_orig->iov_base;
				}

				raid_io->split.offset = 0;
				raid_io->base_bdev_io_submitted = 0;
				raid_io->raid_ch = raid_io->raid_ch->process.ch_processed;

				switch (bdev_io->type) {
				case SPDK_BDEV_IO_TYPE_READ:
				case SPDK_BDEV_IO_TYPE_WRITE:
					raid_io->raid_bdev->module->submit_rw_request(raid_io);
					break;

				case SPDK_BDEV_IO_TYPE_FLUSH:
				case SPDK_BDEV_IO_TYPE_UNMAP:
					raid_io->raid_bdev->module->submit_null_payload_request(raid_io);
					break;
				default:
					SPDK_ERRLOG("io type %u should not happen split\n", bdev_io->type);
					raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
					break;
				}
				return;
			}
		}

		raid_io->num_blocks = bdev_io->u.bdev.num_blocks;
		raid_io->iovcnt = bdev_io->u.bdev.iovcnt;
		raid_io->iovs = bdev_io->u.bdev.iovs;
		if (split_iov != NULL) {
			*split_iov = *split_iov_orig;
		}
	}

	if (spdk_unlikely(raid_io->completion_cb != NULL)) {
		raid_io->completion_cb(raid_io, status);
	} else {
		if (spdk_unlikely(bdev_io->type == SPDK_BDEV_IO_TYPE_READ &&
				  spdk_bdev_get_dif_type(bdev_io->bdev) != SPDK_DIF_DISABLE &&
				  bdev_io->bdev->dif_check_flags & SPDK_DIF_FLAGS_REFTAG_CHECK &&
				  status == SPDK_BDEV_IO_STATUS_SUCCESS)) {

			rc = raid_bdev_remap_dix_reftag(bdev_io->u.bdev.md_buf,
							bdev_io->u.bdev.num_blocks, bdev_io->bdev,
							bdev_io->u.bdev.offset_blocks);
			if (rc != 0) {
				status = SPDK_BDEV_IO_STATUS_FAILED;
			}
		}
		spdk_bdev_io_complete(bdev_io, status);
	}
}

/*
 * brief:
 * raid_bdev_io_complete_part - signal the completion of a part of the expected
 * base bdev IOs and complete the raid_io if this is the final expected IO.
 * The caller should first set raid_io->base_bdev_io_remaining. This function
 * will decrement this counter by the value of the 'completed' parameter and
 * complete the raid_io if the counter reaches 0. The caller is free to
 * interpret the 'base_bdev_io_remaining' and 'completed' values as needed,
 * it can represent e.g. blocks or IOs.
 * params:
 * raid_io - pointer to raid_bdev_io
 * completed - the part of the raid_io that has been completed
 * status - status of the base IO
 * returns:
 * true - if the raid_io is completed
 * false - otherwise
 */
bool
raid_bdev_io_complete_part(struct raid_bdev_io *raid_io, uint64_t completed,
			   enum spdk_bdev_io_status status)
{
	assert(raid_io->base_bdev_io_remaining >= completed);
	raid_io->base_bdev_io_remaining -= completed;

	if (status != raid_io->base_bdev_io_status_default) {
		raid_io->base_bdev_io_status = status;
	}

	if (raid_io->base_bdev_io_remaining == 0) {
		raid_bdev_io_complete(raid_io, raid_io->base_bdev_io_status);
		return true;
	} else {
		return false;
	}
}

/*
 * brief:
 * raid_bdev_queue_io_wait function processes the IO which failed to submit.
 * It will try to queue the IOs after storing the context to bdev wait queue logic.
 * params:
 * raid_io - pointer to raid_bdev_io
 * bdev - the block device that the IO is submitted to
 * ch - io channel
 * cb_fn - callback when the spdk_bdev_io for bdev becomes available
 * returns:
 * none
 */
void
raid_bdev_queue_io_wait(struct raid_bdev_io *raid_io, struct spdk_bdev *bdev,
			struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn)
{
	raid_io->waitq_entry.bdev = bdev;
	raid_io->waitq_entry.cb_fn = cb_fn;
	raid_io->waitq_entry.cb_arg = raid_io;
	raid_io->waitq_entry.dep_unblock = true;

	spdk_bdev_queue_io_wait(bdev, ch, &raid_io->waitq_entry);
}

static void
raid_base_bdev_reset_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	raid_bdev_io_complete_part(raid_io, 1, success ?
				   SPDK_BDEV_IO_STATUS_SUCCESS :
				   SPDK_BDEV_IO_STATUS_FAILED);
}

static void raid_bdev_submit_reset_request(struct raid_bdev_io *raid_io);

static void
_raid_bdev_submit_reset_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	raid_bdev_submit_reset_request(raid_io);
}

/*
 * brief:
 * raid_bdev_submit_reset_request function submits reset requests
 * to member disks; it will submit as many as possible unless a reset fails with -ENOMEM, in
 * which case it will queue it for later submission
 * params:
 * raid_io
 * returns:
 * none
 */
static void
raid_bdev_submit_reset_request(struct raid_bdev_io *raid_io)
{
	struct raid_bdev		*raid_bdev;
	int				ret;
	uint8_t				i;
	struct raid_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;

	raid_bdev = raid_io->raid_bdev;

	if (raid_io->base_bdev_io_remaining == 0) {
		raid_io->base_bdev_io_remaining = raid_bdev->num_base_bdevs;
	}

	for (i = raid_io->base_bdev_io_submitted; i < raid_bdev->num_base_bdevs; i++) {
		base_info = &raid_bdev->base_bdev_info[i];
		base_ch = raid_io->raid_ch->base_channel[i];
		if (base_ch == NULL) {
			raid_io->base_bdev_io_submitted++;
			raid_bdev_io_complete_part(raid_io, 1, SPDK_BDEV_IO_STATUS_SUCCESS);
			continue;
		}
		ret = spdk_bdev_reset(base_info->desc, base_ch,
				      raid_base_bdev_reset_complete, raid_io);
		if (ret == 0) {
			raid_io->base_bdev_io_submitted++;
		} else if (ret == -ENOMEM) {
			raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
						base_ch, _raid_bdev_submit_reset_request);
			return;
		} else {
			SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
			assert(false);
			raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
	}
}

static void
raid_bdev_io_split(struct raid_bdev_io *raid_io, uint64_t split_offset)
{
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	size_t iov_offset = split_offset * raid_bdev->bdev.blocklen;
	int i;

	assert(split_offset != 0);
	assert(raid_io->split.offset == RAID_OFFSET_BLOCKS_INVALID);
	raid_io->split.offset = split_offset;

	raid_io->offset_blocks += split_offset;
	raid_io->num_blocks -= split_offset;
	if (raid_io->md_buf != NULL) {
		raid_io->md_buf += (split_offset * raid_bdev->bdev.md_len);
	}

	for (i = 0; i < raid_io->iovcnt; i++) {
		struct iovec *iov = &raid_io->iovs[i];

		if (iov_offset < iov->iov_len) {
			if (iov_offset == 0) {
				raid_io->split.iov = NULL;
			} else {
				raid_io->split.iov = iov;
				raid_io->split.iov_copy = *iov;
				iov->iov_base += iov_offset;
				iov->iov_len -= iov_offset;
			}
			raid_io->iovs += i;
			raid_io->iovcnt -= i;
			break;
		}

		iov_offset -= iov->iov_len;
	}
}

static inline void
_raid_bdev_request_split(struct raid_bdev_io *raid_io)
{
	struct raid_bdev_io_channel *raid_ch = raid_io->raid_ch;

	if (raid_ch->process.offset != RAID_OFFSET_BLOCKS_INVALID) {
		uint64_t offset_begin = raid_io->offset_blocks;
		uint64_t offset_end = offset_begin + raid_io->num_blocks;

		if (offset_end > raid_ch->process.offset) {
			if (offset_begin < raid_ch->process.offset) {
				/*
				 * If the I/O spans both the processed and unprocessed ranges,
				 * split it and first handle the unprocessed part. After it
				 * completes, the rest will be handled.
				 * This situation occurs when the process thread is not active
				 * or is waiting for the process window range to be locked
				 * (quiesced). When a window is being processed, such I/Os will be
				 * deferred by the bdev layer until the window is unlocked.
				 */
				SPDK_DEBUGLOG(bdev_raid, "split: process_offset: %lu offset_begin: %lu offset_end: %lu\n",
					      raid_ch->process.offset, offset_begin, offset_end);
				raid_bdev_io_split(raid_io, raid_ch->process.offset - offset_begin);
			}
		} else {
			/* Use the child channel, which corresponds to the already processed range */
			raid_io->raid_ch = raid_ch->process.ch_processed;
		}
	}
}

static void
raid_bdev_submit_rw_request(struct raid_bdev_io *raid_io)
{
	_raid_bdev_request_split(raid_io);
	raid_io->raid_bdev->module->submit_rw_request(raid_io);
}

static void
raid_bdev_submit_null_payload_request(struct raid_bdev_io *raid_io)
{
	_raid_bdev_request_split(raid_io);
	raid_io->raid_bdev->module->submit_null_payload_request(raid_io);
}

/*
 * brief:
 * Callback function to spdk_bdev_io_get_buf.
 * params:
 * ch - pointer to raid bdev io channel
 * bdev_io - pointer to parent bdev_io on raid bdev device
 * success - True if buffer is allocated or false otherwise.
 * returns:
 * none
 */
static void
raid_bdev_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		     bool success)
{
	struct raid_bdev_io *raid_io = (struct raid_bdev_io *)bdev_io->driver_ctx;

	if (!success) {
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	raid_io->iovs = bdev_io->u.bdev.iovs;
	raid_io->iovcnt = bdev_io->u.bdev.iovcnt;
	raid_io->md_buf = bdev_io->u.bdev.md_buf;

	raid_bdev_submit_rw_request(raid_io);
}

void
raid_bdev_io_init(struct raid_bdev_io *raid_io, struct raid_bdev_io_channel *raid_ch,
		  enum spdk_bdev_io_type type, uint64_t offset_blocks,
		  uint64_t num_blocks, struct iovec *iovs, int iovcnt, void *md_buf,
		  struct spdk_memory_domain *memory_domain, void *memory_domain_ctx)
{
	struct spdk_io_channel *ch = spdk_io_channel_from_ctx(raid_ch);
	struct raid_bdev *raid_bdev = spdk_io_channel_get_io_device(ch);

	raid_io->type = type;
	raid_io->offset_blocks = offset_blocks;
	raid_io->num_blocks = num_blocks;
	raid_io->iovs = iovs;
	raid_io->iovcnt = iovcnt;
	raid_io->memory_domain = memory_domain;
	raid_io->memory_domain_ctx = memory_domain_ctx;
	raid_io->md_buf = md_buf;

	raid_io->raid_bdev = raid_bdev;
	raid_io->raid_ch = raid_ch;
	raid_io->base_bdev_io_remaining = 0;
	raid_io->base_bdev_io_submitted = 0;
	raid_io->completion_cb = NULL;
	raid_io->split.offset = RAID_OFFSET_BLOCKS_INVALID;

	raid_bdev_io_set_default_status(raid_io, SPDK_BDEV_IO_STATUS_SUCCESS);
}

/*
 * brief:
 * raid_bdev_submit_request function is the submit_request function pointer of
 * raid bdev function table. This is used to submit the io on raid_bdev to below
 * layers.
 * params:
 * ch - pointer to raid bdev io channel
 * bdev_io - pointer to parent bdev_io on raid bdev device
 * returns:
 * none
 */
static void
raid_bdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct raid_bdev_io *raid_io = (struct raid_bdev_io *)bdev_io->driver_ctx;

	raid_bdev_io_init(raid_io, spdk_io_channel_get_ctx(ch), bdev_io->type,
			  bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
			  bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.md_buf,
			  bdev_io->u.bdev.memory_domain, bdev_io->u.bdev.memory_domain_ctx);

	spdk_trace_record(TRACE_BDEV_RAID_IO_START, 0, 0, (uintptr_t)raid_io, (uintptr_t)bdev_io);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, raid_bdev_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		raid_bdev_submit_rw_request(raid_io);
		break;

	case SPDK_BDEV_IO_TYPE_RESET:
		raid_bdev_submit_reset_request(raid_io);
		break;

	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		raid_bdev_submit_null_payload_request(raid_io);
		break;

	default:
		SPDK_ERRLOG("submit request, invalid io type %u\n", bdev_io->type);
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

/*
 * brief:
 * _raid_bdev_io_type_supported checks whether io_type is supported in
 * all base bdev modules of raid bdev module. If anyone among the base_bdevs
 * doesn't support, the raid device doesn't supports.
 *
 * params:
 * raid_bdev - pointer to raid bdev context
 * io_type - io type
 * returns:
 * true - io_type is supported
 * false - io_type is not supported
 */
inline static bool
_raid_bdev_io_type_supported(struct raid_bdev *raid_bdev, enum spdk_bdev_io_type io_type)
{
	struct raid_base_bdev_info *base_info;

	if (io_type == SPDK_BDEV_IO_TYPE_FLUSH ||
	    io_type == SPDK_BDEV_IO_TYPE_UNMAP) {
		if (raid_bdev->module->submit_null_payload_request == NULL) {
			return false;
		}
	}

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		if (base_info->desc == NULL) {
			continue;
		}

		if (spdk_bdev_io_type_supported(spdk_bdev_desc_get_bdev(base_info->desc), io_type) == false) {
			return false;
		}
	}

	return true;
}

/*
 * brief:
 * raid_bdev_io_type_supported is the io_supported function for bdev function
 * table which returns whether the particular io type is supported or not by
 * raid bdev module
 * params:
 * ctx - pointer to raid bdev context
 * type - io type
 * returns:
 * true - io_type is supported
 * false - io_type is not supported
 */
static bool
raid_bdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;

	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		return _raid_bdev_io_type_supported(ctx, io_type);

	default:
		return false;
	}

	return false;
}

/*
 * brief:
 * raid_bdev_get_io_channel is the get_io_channel function table pointer for
 * raid bdev. This is used to return the io channel for this raid bdev
 * params:
 * ctxt - pointer to raid_bdev
 * returns:
 * pointer to io channel for raid bdev
 */
static struct spdk_io_channel *
raid_bdev_get_io_channel(void *ctxt)
{
	struct raid_bdev *raid_bdev = ctxt;

	return spdk_get_io_channel(raid_bdev);
}

void
raid_bdev_write_info_json(struct raid_bdev *raid_bdev, struct spdk_json_write_ctx *w)
{
	struct raid_base_bdev_info *base_info;

	assert(raid_bdev != NULL);
	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	spdk_json_write_named_uuid(w, "uuid", &raid_bdev->bdev.uuid);
	spdk_json_write_named_uint32(w, "strip_size_kb", raid_bdev->strip_size_kb);
	spdk_json_write_named_string(w, "state", raid_bdev_state_to_str(raid_bdev->state));
	spdk_json_write_named_string(w, "raid_level", raid_bdev_level_to_str(raid_bdev->level));
	spdk_json_write_named_bool(w, "superblock", raid_bdev->superblock_enabled);
	spdk_json_write_named_uint32(w, "num_base_bdevs", raid_bdev->num_base_bdevs);
	spdk_json_write_named_uint32(w, "num_base_bdevs_discovered", raid_bdev->num_base_bdevs_discovered);
	spdk_json_write_named_uint32(w, "num_base_bdevs_operational",
				     raid_bdev->num_base_bdevs_operational);
	/* 输出总大小（块），单位与base_bdev的data_size一致
	 * 只有在online状态或blockcnt已设置时才输出
	 */
	if (raid_bdev->state == RAID_BDEV_STATE_ONLINE || raid_bdev->bdev.blockcnt > 0) {
		spdk_json_write_named_uint64(w, "total_size_blocks", raid_bdev->bdev.blockcnt);
		spdk_json_write_named_uint32(w, "blocklen", raid_bdev->bdev.blocklen);
	}
	if (raid_bdev->process) {
		struct raid_bdev_process *process = raid_bdev->process;
		uint64_t offset = process->window_offset;
		uint64_t total_size = raid_bdev->bdev.blockcnt;
		double percent = 0.0;

		if (total_size > 0) {
			percent = (double)offset * 100.0 / (double)total_size;
		}

		spdk_json_write_named_object_begin(w, "process");
		spdk_json_write_named_string(w, "type", raid_bdev_process_to_str(process->type));
		if (process->target != NULL) {
			spdk_json_write_named_string(w, "target", process->target->name);
			/* Find target slot */
			uint8_t target_slot = 0;
			RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
				if (base_info == process->target) {
					target_slot = (uint8_t)(base_info - raid_bdev->base_bdev_info);
					break;
				}
			}
			spdk_json_write_named_uint8(w, "target_slot", target_slot);
		}
		spdk_json_write_named_object_begin(w, "progress");
		spdk_json_write_named_uint64(w, "current_offset", offset);
		spdk_json_write_named_uint64(w, "total_size", total_size);
		spdk_json_write_named_double(w, "percent", percent);
		if (process->type == RAID_PROCESS_REBUILD) {
			spdk_json_write_named_string(w, "state", raid_rebuild_state_to_str(process->rebuild_state));
		}
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_name(w, "base_bdevs_list");
	spdk_json_write_array_begin(w);
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		spdk_json_write_object_begin(w);
		spdk_json_write_name(w, "name");
		if (base_info->name) {
			spdk_json_write_string(w, base_info->name);
		} else {
			spdk_json_write_null(w);
		}
		spdk_json_write_named_uuid(w, "uuid", &base_info->uuid);
		spdk_json_write_named_bool(w, "is_configured", base_info->is_configured);
		spdk_json_write_named_uint64(w, "data_offset", base_info->data_offset);
		spdk_json_write_named_uint64(w, "data_size", base_info->data_size);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
}

/*
 * brief:
 * raid_bdev_dump_info_json is the function table pointer for raid bdev
 * params:
 * ctx - pointer to raid_bdev
 * w - pointer to json context
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
raid_bdev_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct raid_bdev *raid_bdev = ctx;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_dump_config_json\n");

	/* Dump the raid bdev configuration related information */
	spdk_json_write_named_object_begin(w, "raid");
	raid_bdev_write_info_json(raid_bdev, w);
	spdk_json_write_object_end(w);

	return 0;
}

/*
 * brief:
 * raid_bdev_write_config_json is the function table pointer for raid bdev
 * params:
 * bdev - pointer to spdk_bdev
 * w - pointer to json context
 * returns:
 * none
 */
static void
raid_bdev_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct raid_bdev *raid_bdev = bdev->ctxt;
	struct raid_base_bdev_info *base_info;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	if (raid_bdev->superblock_enabled) {
		/* raid bdev configuration is stored in the superblock */
		return;
	}

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_raid_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uuid(w, "uuid", &raid_bdev->bdev.uuid);
	if (raid_bdev->strip_size_kb != 0) {
		spdk_json_write_named_uint32(w, "strip_size_kb", raid_bdev->strip_size_kb);
	}
	spdk_json_write_named_string(w, "raid_level", raid_bdev_level_to_str(raid_bdev->level));

	spdk_json_write_named_array_begin(w, "base_bdevs");
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		if (base_info->name) {
			spdk_json_write_string(w, base_info->name);
		} else {
			char str[32];

			snprintf(str, sizeof(str), "removed_base_bdev_%u", raid_bdev_base_bdev_slot(base_info));
			spdk_json_write_string(w, str);
		}
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static int
raid_bdev_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct raid_bdev *raid_bdev = ctx;
	struct raid_base_bdev_info *base_info;
	int domains_count = 0, rc = 0;

	if (raid_bdev->module->memory_domains_supported == false) {
		return 0;
	}

	/* First loop to get the number of memory domains */
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		if (base_info->is_configured == false) {
			continue;
		}
		rc = spdk_bdev_get_memory_domains(spdk_bdev_desc_get_bdev(base_info->desc), NULL, 0);
		if (rc < 0) {
			return rc;
		}
		domains_count += rc;
	}

	if (!domains || array_size < domains_count) {
		return domains_count;
	}

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		if (base_info->is_configured == false) {
			continue;
		}
		rc = spdk_bdev_get_memory_domains(spdk_bdev_desc_get_bdev(base_info->desc), domains, array_size);
		if (rc < 0) {
			return rc;
		}
		domains += rc;
		array_size -= rc;
	}

	return domains_count;
}

/* g_raid_bdev_fn_table is the function table for raid bdev */
static const struct spdk_bdev_fn_table g_raid_bdev_fn_table = {
	.destruct		= raid_bdev_destruct,
	.submit_request		= raid_bdev_submit_request,
	.io_type_supported	= raid_bdev_io_type_supported,
	.get_io_channel		= raid_bdev_get_io_channel,
	.dump_info_json		= raid_bdev_dump_info_json,
	.write_config_json	= raid_bdev_write_config_json,
	.get_memory_domains	= raid_bdev_get_memory_domains,
};

struct raid_bdev *
raid_bdev_find_by_name(const char *name)
{
	struct raid_bdev *raid_bdev;

	TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
		if (strcmp(raid_bdev->bdev.name, name) == 0) {
			return raid_bdev;
		}
	}

	return NULL;
}

static struct raid_bdev *
raid_bdev_find_by_uuid(const struct spdk_uuid *uuid)
{
	struct raid_bdev *raid_bdev;

	TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
		if (spdk_uuid_compare(&raid_bdev->bdev.uuid, uuid) == 0) {
			return raid_bdev;
		}
	}

	return NULL;
}

static struct {
	const char *name;
	enum raid_level value;
} g_raid_level_names[] = {
	{ "raid0", RAID0 },
	{ "0", RAID0 },
	{ "raid1", RAID1 },
	{ "1", RAID1 },
	{ "raid5f", RAID5F },
	{ "5f", RAID5F },
	{ "concat", CONCAT },
	{ "raid10", RAID10 },
	{ "10", RAID10 },
};

const char *g_raid_state_names[] = {
	[RAID_BDEV_STATE_ONLINE]	= "online",
	[RAID_BDEV_STATE_CONFIGURING]	= "configuring",
	[RAID_BDEV_STATE_OFFLINE]	= "offline",
	[RAID_BDEV_STATE_MAX]		= NULL
};

static const char *g_raid_process_type_names[] = {
	[RAID_PROCESS_NONE]	= "none",
	[RAID_PROCESS_REBUILD]	= "rebuild",
	[RAID_PROCESS_MAX]	= NULL
};

/* We have to use the typedef in the function declaration to appease astyle. */
typedef enum raid_level raid_level_t;
typedef enum raid_bdev_state raid_bdev_state_t;

raid_level_t
raid_bdev_str_to_level(const char *str)
{
	unsigned int i;

	assert(str != NULL);

	for (i = 0; g_raid_level_names[i].name != NULL; i++) {
		if (strcasecmp(g_raid_level_names[i].name, str) == 0) {
			return g_raid_level_names[i].value;
		}
	}

	return INVALID_RAID_LEVEL;
}

const char *
raid_bdev_level_to_str(enum raid_level level)
{
	unsigned int i;

	for (i = 0; g_raid_level_names[i].name != NULL; i++) {
		if (g_raid_level_names[i].value == level) {
			return g_raid_level_names[i].name;
		}
	}

	return "";
}

raid_bdev_state_t
raid_bdev_str_to_state(const char *str)
{
	unsigned int i;

	assert(str != NULL);

	for (i = 0; i < RAID_BDEV_STATE_MAX; i++) {
		if (strcasecmp(g_raid_state_names[i], str) == 0) {
			break;
		}
	}

	return i;
}

const char *
raid_bdev_state_to_str(enum raid_bdev_state state)
{
	if (state >= RAID_BDEV_STATE_MAX) {
		return "";
	}

	return g_raid_state_names[state];
}

const char *
raid_bdev_process_to_str(enum raid_process_type value)
{
	if (value >= RAID_PROCESS_MAX) {
		return "";
	}

	return g_raid_process_type_names[value];
}

static const char *g_raid_rebuild_state_names[] = {
	[RAID_REBUILD_STATE_IDLE]		= "idle",
	[RAID_REBUILD_STATE_READING]		= "reading",
	[RAID_REBUILD_STATE_WRITING]		= "writing",
	[RAID_REBUILD_STATE_CALCULATING]	= "calculating",
};

const char *
raid_rebuild_state_to_str(enum raid_rebuild_state state)
{
	if (state > RAID_REBUILD_STATE_CALCULATING) {
		return "";
	}

	return g_raid_rebuild_state_names[state];
}

/*
 * brief:
 * raid_bdev_fini_start is called when bdev layer is starting the
 * shutdown process
 * params:
 * none
 * returns:
 * none
 */
static void
raid_bdev_fini_start(void)
{
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_fini_start\n");

	TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
		if (raid_bdev->state != RAID_BDEV_STATE_ONLINE) {
			RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
				raid_bdev_free_base_bdev_resource(base_info);
			}
		}
	}

	g_shutdown_started = true;
}

/*
 * brief:
 * raid_bdev_exit is called on raid bdev module exit time by bdev layer
 * params:
 * none
 * returns:
 * none
 */
static void
raid_bdev_exit(void)
{
	struct raid_bdev *raid_bdev, *tmp;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_exit\n");

	TAILQ_FOREACH_SAFE(raid_bdev, &g_raid_bdev_list, global_link, tmp) {
		raid_bdev_cleanup_and_free(raid_bdev);
	}
}

static void
raid_bdev_opts_config_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_raid_set_options");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_uint32(w, "process_window_size_kb", g_opts.process_window_size_kb);
	spdk_json_write_named_uint32(w, "process_max_bandwidth_mb_sec",
				     g_opts.process_max_bandwidth_mb_sec);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static int
raid_bdev_config_json(struct spdk_json_write_ctx *w)
{
	raid_bdev_opts_config_json(w);

	return 0;
}

/*
 * brief:
 * raid_bdev_get_ctx_size is used to return the context size of bdev_io for raid
 * module
 * params:
 * none
 * returns:
 * size of spdk_bdev_io context for raid
 */
static int
raid_bdev_get_ctx_size(void)
{
	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_get_ctx_size\n");
	return sizeof(struct raid_bdev_io);
}

static struct spdk_bdev_module g_raid_if = {
	.name = "raid",
	.module_init = raid_bdev_init,
	.fini_start = raid_bdev_fini_start,
	.module_fini = raid_bdev_exit,
	.config_json = raid_bdev_config_json,
	.get_ctx_size = raid_bdev_get_ctx_size,
	.examine_disk = raid_bdev_examine,
	.async_init = false,
	.async_fini = false,
};
SPDK_BDEV_MODULE_REGISTER(raid, &g_raid_if)

/*
 * brief:
 * raid_bdev_init is the initialization function for raid bdev module
 * params:
 * none
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
raid_bdev_init(void)
{
	return 0;
}

static int
_raid_bdev_create(const char *name, uint32_t strip_size, uint8_t num_base_bdevs,
		  enum raid_level level, bool superblock_enabled, const struct spdk_uuid *uuid,
		  struct raid_bdev **raid_bdev_out)
{
	struct raid_bdev *raid_bdev;
	struct spdk_bdev *raid_bdev_gen;
	struct raid_bdev_module *module;
	struct raid_base_bdev_info *base_info;
	uint8_t min_operational;

	if (strnlen(name, RAID_BDEV_SB_NAME_SIZE) == RAID_BDEV_SB_NAME_SIZE) {
		SPDK_ERRLOG("Raid bdev name '%s' exceeds %d characters\n", name, RAID_BDEV_SB_NAME_SIZE - 1);
		return -EINVAL;
	}

	if (raid_bdev_find_by_name(name) != NULL) {
		SPDK_ERRLOG("Duplicate raid bdev name found: %s\n", name);
		return -EEXIST;
	}

	if (level == RAID1) {
		if (strip_size != 0) {
			SPDK_ERRLOG("Strip size is not supported by raid1\n");
			return -EINVAL;
		}
	} else if (spdk_u32_is_pow2(strip_size) == false) {
		SPDK_ERRLOG("Invalid strip size %" PRIu32 "\n", strip_size);
		return -EINVAL;
	}

	module = raid_bdev_module_find(level);
	if (module == NULL) {
		SPDK_ERRLOG("Unsupported raid level '%d'\n", level);
		return -EINVAL;
	}

	assert(module->base_bdevs_min != 0);
	if (num_base_bdevs < module->base_bdevs_min) {
		SPDK_ERRLOG("At least %u base devices required for %s\n",
			    module->base_bdevs_min,
			    raid_bdev_level_to_str(level));
		return -EINVAL;
	}

	switch (module->base_bdevs_constraint.type) {
	case CONSTRAINT_MAX_BASE_BDEVS_REMOVED:
		min_operational = num_base_bdevs - module->base_bdevs_constraint.value;
		break;
	case CONSTRAINT_MIN_BASE_BDEVS_OPERATIONAL:
		min_operational = module->base_bdevs_constraint.value;
		break;
	case CONSTRAINT_UNSET:
		if (module->base_bdevs_constraint.value != 0) {
			SPDK_ERRLOG("Unexpected constraint value '%u' provided for raid bdev '%s'.\n",
				    (uint8_t)module->base_bdevs_constraint.value, name);
			return -EINVAL;
		}
		min_operational = num_base_bdevs;
		break;
	default:
		SPDK_ERRLOG("Unrecognised constraint type '%u' in module for raid level '%s'.\n",
			    (uint8_t)module->base_bdevs_constraint.type,
			    raid_bdev_level_to_str(module->level));
		return -EINVAL;
	};

	if (min_operational == 0 || min_operational > num_base_bdevs) {
		SPDK_ERRLOG("Wrong constraint value for raid level '%s'.\n",
			    raid_bdev_level_to_str(module->level));
		return -EINVAL;
	}

	raid_bdev = calloc(1, sizeof(*raid_bdev));
	if (!raid_bdev) {
		SPDK_ERRLOG("Unable to allocate memory for raid bdev\n");
		return -ENOMEM;
	}

	raid_bdev->module = module;
	raid_bdev->num_base_bdevs = num_base_bdevs;
	raid_bdev->base_bdev_info = calloc(raid_bdev->num_base_bdevs,
					   sizeof(struct raid_base_bdev_info));
	if (!raid_bdev->base_bdev_info) {
		SPDK_ERRLOG("Unable able to allocate base bdev info\n");
		raid_bdev_free(raid_bdev);
		return -ENOMEM;
	}

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->raid_bdev = raid_bdev;
	}

	/* strip_size_kb is from the rpc param.  strip_size is in blocks and used
	 * internally and set later.
	 */
	raid_bdev->strip_size = 0;
	raid_bdev->strip_size_kb = strip_size;
	raid_bdev->state = RAID_BDEV_STATE_CONFIGURING;
	raid_bdev->level = level;
	raid_bdev->min_base_bdevs_operational = min_operational;
	raid_bdev->superblock_enabled = superblock_enabled;

	raid_bdev_gen = &raid_bdev->bdev;

	raid_bdev_gen->name = strdup(name);
	if (!raid_bdev_gen->name) {
		SPDK_ERRLOG("Unable to allocate name for raid\n");
		raid_bdev_free(raid_bdev);
		return -ENOMEM;
	}

	raid_bdev_gen->product_name = "Raid Volume";
	raid_bdev_gen->ctxt = raid_bdev;
	raid_bdev_gen->fn_table = &g_raid_bdev_fn_table;
	raid_bdev_gen->module = &g_raid_if;
	raid_bdev_gen->write_cache = 0;
	spdk_uuid_copy(&raid_bdev_gen->uuid, uuid);

	TAILQ_INSERT_TAIL(&g_raid_bdev_list, raid_bdev, global_link);

	*raid_bdev_out = raid_bdev;

	return 0;
}

/*
 * brief:
 * raid_bdev_create allocates raid bdev based on passed configuration
 * params:
 * name - name for raid bdev
 * strip_size - strip size in KB
 * num_base_bdevs - number of base bdevs
 * level - raid level
 * superblock_enabled - true if raid should have superblock
 * uuid - uuid to set for the bdev
 * raid_bdev_out - the created raid bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
int
raid_bdev_create(const char *name, uint32_t strip_size, uint8_t num_base_bdevs,
		 enum raid_level level, bool superblock_enabled, const struct spdk_uuid *uuid,
		 struct raid_bdev **raid_bdev_out)
{
	struct raid_bdev *raid_bdev;
	int rc;

	assert(uuid != NULL);

	rc = _raid_bdev_create(name, strip_size, num_base_bdevs, level, superblock_enabled, uuid,
			       &raid_bdev);
	if (rc != 0) {
		return rc;
	}

	if (superblock_enabled && spdk_uuid_is_null(uuid)) {
		/* we need to have the uuid to store in the superblock before the bdev is registered */
		spdk_uuid_generate(&raid_bdev->bdev.uuid);
	}

	raid_bdev->num_base_bdevs_operational = num_base_bdevs;

	*raid_bdev_out = raid_bdev;

	return 0;
}

static void
_raid_bdev_unregistering_cont(void *ctx)
{
	struct raid_bdev *raid_bdev = ctx;

	spdk_bdev_close(raid_bdev->self_desc);
	raid_bdev->self_desc = NULL;
}

static void
raid_bdev_unregistering_cont(void *ctx)
{
	spdk_thread_exec_msg(spdk_thread_get_app_thread(), _raid_bdev_unregistering_cont, ctx);
}

static int
raid_bdev_process_add_finish_action(struct raid_bdev_process *process, spdk_msg_fn cb, void *cb_ctx)
{
	struct raid_process_finish_action *finish_action;

	assert(spdk_get_thread() == process->thread);
	assert(process->state < RAID_PROCESS_STATE_STOPPED);

	finish_action = calloc(1, sizeof(*finish_action));
	if (finish_action == NULL) {
		return -ENOMEM;
	}

	finish_action->cb = cb;
	finish_action->cb_ctx = cb_ctx;

	TAILQ_INSERT_TAIL(&process->finish_actions, finish_action, link);

	return 0;
}

static void
raid_bdev_unregistering_stop_process(void *ctx)
{
	struct raid_bdev_process *process = ctx;
	struct raid_bdev *raid_bdev = process->raid_bdev;
	int rc;

	process->state = RAID_PROCESS_STATE_STOPPING;
	if (process->status == 0) {
		process->status = -ECANCELED;
	}

	rc = raid_bdev_process_add_finish_action(process, raid_bdev_unregistering_cont, raid_bdev);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to add raid bdev '%s' process finish action: %s\n",
			    raid_bdev->bdev.name, spdk_strerror(-rc));
	}
}

static void
raid_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	struct raid_bdev *raid_bdev = event_ctx;

	if (type == SPDK_BDEV_EVENT_REMOVE) {
		struct raid_bdev_process *process = raid_bdev_get_active_process(raid_bdev);

		if (process != NULL) {
			spdk_thread_send_msg(process->thread, raid_bdev_unregistering_stop_process, process);
			return;
		}

		raid_bdev_unregistering_cont(raid_bdev);
	}
}

static void
raid_bdev_configure_cont(struct raid_bdev *raid_bdev)
{
	struct spdk_bdev *raid_bdev_gen = &raid_bdev->bdev;
	int rc;

	raid_bdev->state = RAID_BDEV_STATE_ONLINE;
	SPDK_DEBUGLOG(bdev_raid, "io device register %p\n", raid_bdev);
	SPDK_DEBUGLOG(bdev_raid, "blockcnt %" PRIu64 ", blocklen %u\n",
		      raid_bdev_gen->blockcnt, raid_bdev_gen->blocklen);
	spdk_io_device_register(raid_bdev, raid_bdev_create_cb, raid_bdev_destroy_cb,
				sizeof(struct raid_bdev_io_channel),
				raid_bdev_gen->name);
	rc = spdk_bdev_register(raid_bdev_gen);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to register raid bdev '%s': %s\n",
			    raid_bdev_gen->name, spdk_strerror(-rc));
		goto out;
	}

	/*
	 * Open the bdev internally to delay unregistering if we need to stop a background process
	 * first. The process may still need to unquiesce a range but it will fail because the
	 * bdev's internal.spinlock is destroyed by the time the destruct callback is reached.
	 * During application shutdown, bdevs automatically get unregistered by the bdev layer
	 * so this is the only way currently to do this correctly.
	 * TODO: try to handle this correctly in bdev layer instead.
	 */
	rc = spdk_bdev_open_ext(raid_bdev_gen->name, false, raid_bdev_event_cb, raid_bdev,
				&raid_bdev->self_desc);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to open raid bdev '%s': %s\n",
			    raid_bdev_gen->name, spdk_strerror(-rc));
		spdk_bdev_unregister(raid_bdev_gen, NULL, NULL);
		goto out;
	}

	SPDK_DEBUGLOG(bdev_raid, "raid bdev generic %p\n", raid_bdev_gen);
	SPDK_DEBUGLOG(bdev_raid, "raid bdev is created with name %s, raid_bdev %p\n",
		      raid_bdev_gen->name, raid_bdev);
out:
	if (rc != 0) {
		if (raid_bdev->module->stop != NULL) {
			raid_bdev->module->stop(raid_bdev);
		}
		spdk_io_device_unregister(raid_bdev, NULL);
		raid_bdev->state = RAID_BDEV_STATE_CONFIGURING;
	}

	if (raid_bdev->configure_cb != NULL) {
		raid_bdev->configure_cb(raid_bdev->configure_cb_ctx, rc);
		raid_bdev->configure_cb = NULL;
	}
}

static void
raid_bdev_configure_write_sb_cb(int status, struct raid_bdev *raid_bdev, void *ctx)
{
	if (status == 0) {
		raid_bdev_configure_cont(raid_bdev);
	} else {
		SPDK_ERRLOG("Failed to write raid bdev '%s' superblock: %s\n",
			    raid_bdev->bdev.name, spdk_strerror(-status));
		if (raid_bdev->module->stop != NULL) {
			raid_bdev->module->stop(raid_bdev);
		}
		if (raid_bdev->configure_cb != NULL) {
			raid_bdev->configure_cb(raid_bdev->configure_cb_ctx, status);
			raid_bdev->configure_cb = NULL;
		}
	}
}

/*
 * brief:
 * If raid bdev config is complete, then only register the raid bdev to
 * bdev layer and remove this raid bdev from configuring list and
 * insert the raid bdev to configured list
 * params:
 * raid_bdev - pointer to raid bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
raid_bdev_configure(struct raid_bdev *raid_bdev, raid_bdev_configure_cb cb, void *cb_ctx)
{
	uint32_t data_block_size = spdk_bdev_get_data_block_size(&raid_bdev->bdev);
	int rc;

	assert(raid_bdev->state == RAID_BDEV_STATE_CONFIGURING);
	assert(raid_bdev->num_base_bdevs_discovered == raid_bdev->num_base_bdevs_operational);
	assert(raid_bdev->bdev.blocklen > 0);

	/* The strip_size_kb is read in from user in KB. Convert to blocks here for
	 * internal use.
	 */
	raid_bdev->strip_size = (raid_bdev->strip_size_kb * 1024) / data_block_size;
	if (raid_bdev->strip_size == 0 && raid_bdev->level != RAID1) {
		SPDK_ERRLOG("Strip size cannot be smaller than the device block size\n");
		return -EINVAL;
	}
	raid_bdev->strip_size_shift = spdk_u32log2(raid_bdev->strip_size);

	rc = raid_bdev->module->start(raid_bdev);
	if (rc != 0) {
		SPDK_ERRLOG("raid module startup callback failed\n");
		return rc;
	}

	assert(raid_bdev->configure_cb == NULL);
	raid_bdev->configure_cb = cb;
	raid_bdev->configure_cb_ctx = cb_ctx;

	if (raid_bdev->superblock_enabled) {
		if (raid_bdev->sb == NULL) {
			rc = raid_bdev_alloc_superblock(raid_bdev, data_block_size);
			if (rc == 0) {
				raid_bdev_init_superblock(raid_bdev);
			}
		} else {
			assert(spdk_uuid_compare(&raid_bdev->sb->uuid, &raid_bdev->bdev.uuid) == 0);
			if (raid_bdev->sb->block_size != data_block_size) {
				SPDK_ERRLOG("blocklen does not match value in superblock\n");
				rc = -EINVAL;
			}
			if (raid_bdev->sb->raid_size != raid_bdev->bdev.blockcnt) {
				SPDK_ERRLOG("blockcnt does not match value in superblock\n");
				rc = -EINVAL;
			}
		}

		if (rc != 0) {
			raid_bdev->configure_cb = NULL;
			if (raid_bdev->module->stop != NULL) {
				raid_bdev->module->stop(raid_bdev);
			}
			return rc;
		}

		raid_bdev_write_superblock(raid_bdev, raid_bdev_configure_write_sb_cb, NULL);
	} else {
		raid_bdev_configure_cont(raid_bdev);
	}

	return 0;
}

/*
 * brief:
 * If raid bdev is online and registered, change the bdev state to
 * configuring and unregister this raid device. Queue this raid device
 * in configuring list
 * params:
 * raid_bdev - pointer to raid bdev
 * cb_fn - callback function
 * cb_arg - argument to callback function
 * returns:
 * none
 */
static void
raid_bdev_deconfigure_unregister_done(void *cb_arg, int rc)
{
	struct raid_bdev *raid_bdev = cb_arg;
	raid_bdev_destruct_cb user_cb_fn = raid_bdev->deconfigure_cb_fn;
	void *user_cb_arg = raid_bdev->deconfigure_cb_arg;

	/* Clear the stored callbacks */
	raid_bdev->deconfigure_cb_fn = NULL;
	raid_bdev->deconfigure_cb_arg = NULL;

	/* Close self_desc before calling user callback */
	if (raid_bdev->self_desc != NULL) {
		spdk_bdev_close(raid_bdev->self_desc);
		raid_bdev->self_desc = NULL;
	}

	/* Call user callback */
	if (user_cb_fn) {
		user_cb_fn(user_cb_arg, rc);
	}
}

static void
raid_bdev_deconfigure(struct raid_bdev *raid_bdev, raid_bdev_destruct_cb cb_fn,
		      void *cb_arg)
{
	if (raid_bdev->state != RAID_BDEV_STATE_ONLINE) {
		if (cb_fn) {
			cb_fn(cb_arg, 0);
		}
		return;
	}

	raid_bdev->state = RAID_BDEV_STATE_OFFLINE;
	SPDK_DEBUGLOG(bdev_raid, "raid bdev state changing from online to offline\n");

	/* Store user callbacks to call them after unregister completes */
	raid_bdev->deconfigure_cb_fn = cb_fn;
	raid_bdev->deconfigure_cb_arg = cb_arg;

	/* Use wrapper callback that will close self_desc and then call user callback */
	spdk_bdev_unregister(&raid_bdev->bdev, raid_bdev_deconfigure_unregister_done, raid_bdev);
}

/*
 * brief:
 * raid_bdev_find_base_info_by_bdev function finds the base bdev info by bdev.
 * params:
 * base_bdev - pointer to base bdev
 * returns:
 * base bdev info if found, otherwise NULL.
 */
static struct raid_base_bdev_info *
raid_bdev_find_base_info_by_bdev(struct spdk_bdev *base_bdev)
{
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;

	TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
		RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
			if (base_info->desc != NULL &&
			    spdk_bdev_desc_get_bdev(base_info->desc) == base_bdev) {
				return base_info;
			}
		}
	}

	return NULL;
}

static void
raid_bdev_remove_base_bdev_done(struct raid_base_bdev_info *base_info, int status)
{
	struct raid_bdev *raid_bdev;

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in raid_bdev_remove_base_bdev_done\n");
		return;
	}

	raid_bdev = base_info->raid_bdev;
	if (raid_bdev == NULL) {
		SPDK_ERRLOG("base_info->raid_bdev is NULL in raid_bdev_remove_base_bdev_done\n");
		base_info->remove_scheduled = false;
		if (base_info->remove_cb != NULL) {
			base_info->remove_cb(base_info->remove_cb_ctx, -EINVAL);
		}
		return;
	}

	assert(base_info->remove_scheduled);
	base_info->remove_scheduled = false;

	if (status == 0) {
		raid_bdev->num_base_bdevs_operational--;
		if (raid_bdev->num_base_bdevs_operational < raid_bdev->min_base_bdevs_operational) {
			/* There is not enough base bdevs to keep the raid bdev operational. */
			raid_bdev_deconfigure(raid_bdev, base_info->remove_cb, base_info->remove_cb_ctx);
			return;
		}
	}

	if (base_info->remove_cb != NULL) {
		base_info->remove_cb(base_info->remove_cb_ctx, status);
	}
}

static void
raid_bdev_remove_base_bdev_on_unquiesced(void *ctx, int status)
{
	struct raid_base_bdev_info *base_info = ctx;
	struct raid_bdev *raid_bdev;

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in raid_bdev_remove_base_bdev_on_unquiesced\n");
		return;
	}

	raid_bdev = base_info->raid_bdev;
	if (raid_bdev == NULL) {
		SPDK_ERRLOG("base_info->raid_bdev is NULL in raid_bdev_remove_base_bdev_on_unquiesced\n");
		raid_bdev_remove_base_bdev_done(base_info, -EINVAL);
		return;
	}

	if (status != 0) {
		SPDK_ERRLOG("Failed to unquiesce raid bdev %s: %s\n",
			    raid_bdev->bdev.name, spdk_strerror(-status));
	}

	raid_bdev_remove_base_bdev_done(base_info, status);
}

static void
raid_bdev_channel_remove_base_bdev(struct spdk_io_channel_iter *i)
{
	struct raid_base_bdev_info *base_info = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct raid_bdev_io_channel *raid_ch = spdk_io_channel_get_ctx(ch);
	struct raid_bdev *raid_bdev;
	uint8_t idx;

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in raid_bdev_channel_remove_base_bdev\n");
		spdk_for_each_channel_continue(i, -EINVAL);
		return;
	}

	raid_bdev = base_info->raid_bdev;
	if (raid_bdev == NULL || raid_bdev->base_bdev_info == NULL) {
		SPDK_ERRLOG("raid_bdev or base_bdev_info is NULL in raid_bdev_channel_remove_base_bdev\n");
		spdk_for_each_channel_continue(i, -EINVAL);
		return;
	}

	idx = raid_bdev_base_bdev_slot(base_info);

	/* Safety check: ensure idx is within valid range */
	if (idx >= raid_bdev->num_base_bdevs) {
		SPDK_ERRLOG("Calculated slot %u exceeds num_base_bdevs %u for RAID bdev '%s'\n",
			    idx, raid_bdev->num_base_bdevs, raid_bdev->bdev.name);
		spdk_for_each_channel_continue(i, -EINVAL);
		return;
	}

	SPDK_DEBUGLOG(bdev_raid, "slot: %u raid_ch: %p\n", idx, raid_ch);

	/* Safety check: ensure base_channel array is allocated */
	if (raid_ch->base_channel != NULL) {
		if (raid_ch->base_channel[idx] != NULL) {
			spdk_put_io_channel(raid_ch->base_channel[idx]);
			raid_ch->base_channel[idx] = NULL;
		}
	}

	if (raid_ch->process.ch_processed != NULL && raid_ch->process.ch_processed->base_channel != NULL) {
		if (idx < raid_bdev->num_base_bdevs) {
			raid_ch->process.ch_processed->base_channel[idx] = NULL;
		}
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
raid_bdev_channels_remove_base_bdev_done(struct spdk_io_channel_iter *i, int status)
{
	struct raid_base_bdev_info *base_info = spdk_io_channel_iter_get_ctx(i);
	struct raid_bdev *raid_bdev;

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in raid_bdev_channels_remove_base_bdev_done\n");
		return;
	}

	raid_bdev = base_info->raid_bdev;
	if (raid_bdev == NULL) {
		SPDK_ERRLOG("base_info->raid_bdev is NULL in raid_bdev_channels_remove_base_bdev_done\n");
		raid_bdev_free_base_bdev_resource(base_info);
		return;
	}

	raid_bdev_free_base_bdev_resource(base_info);

	spdk_bdev_unquiesce(&raid_bdev->bdev, &g_raid_if, raid_bdev_remove_base_bdev_on_unquiesced,
			    base_info);
}

static void
raid_bdev_remove_base_bdev_do_remove(struct raid_base_bdev_info *base_info)
{
	struct raid_bdev *raid_bdev;

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in raid_bdev_remove_base_bdev_do_remove\n");
		return;
	}

	raid_bdev = base_info->raid_bdev;
	if (raid_bdev == NULL) {
		SPDK_ERRLOG("base_info->raid_bdev is NULL in raid_bdev_remove_base_bdev_do_remove\n");
		return;
	}

	raid_bdev_deconfigure_base_bdev(base_info);

	spdk_for_each_channel(raid_bdev, raid_bdev_channel_remove_base_bdev, base_info,
			      raid_bdev_channels_remove_base_bdev_done);
}

static void
raid_bdev_remove_base_bdev_reset_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_base_bdev_info *base_info = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in raid_bdev_remove_base_bdev_reset_done\n");
		return;
	}

	raid_bdev_remove_base_bdev_do_remove(base_info);
}

static void
raid_bdev_remove_base_bdev_cont(struct raid_base_bdev_info *base_info)
{
	int rc;

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in raid_bdev_remove_base_bdev_cont\n");
		return;
	}

	if (base_info->desc == NULL || base_info->app_thread_ch == NULL) {
		SPDK_WARNLOG("base_info->desc or app_thread_ch is NULL, proceeding with removal\n");
		raid_bdev_remove_base_bdev_do_remove(base_info);
		return;
	}

	rc = spdk_bdev_reset(base_info->desc, base_info->app_thread_ch,
			     raid_bdev_remove_base_bdev_reset_done, base_info);
	if (rc != 0) {
		SPDK_WARNLOG("Reset base bdev '%s' before removal failed: %s\n",
			     base_info->name ? base_info->name : "unknown", spdk_strerror(-rc));
		/* Proceed with removal even if reset submission failed */
		raid_bdev_remove_base_bdev_do_remove(base_info);
	}
}

static void
raid_bdev_remove_base_bdev_write_sb_cb(int status, struct raid_bdev *raid_bdev, void *ctx)
{
	struct raid_base_bdev_info *base_info = ctx;

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in raid_bdev_remove_base_bdev_write_sb_cb\n");
		return;
	}

	if (status != 0) {
		SPDK_ERRLOG("Failed to write raid bdev '%s' superblock: %s\n",
			    raid_bdev->bdev.name, spdk_strerror(-status));
		raid_bdev_remove_base_bdev_done(base_info, status);
		return;
	}

	raid_bdev_remove_base_bdev_cont(base_info);
}

static void
raid_bdev_remove_base_bdev_on_quiesced(void *ctx, int status)
{
	struct raid_base_bdev_info *base_info = ctx;
	struct raid_bdev *raid_bdev;

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in raid_bdev_remove_base_bdev_on_quiesced\n");
		return;
	}

	raid_bdev = base_info->raid_bdev;
	if (raid_bdev == NULL) {
		SPDK_ERRLOG("base_info->raid_bdev is NULL in raid_bdev_remove_base_bdev_on_quiesced\n");
		return;
	}

	if (status != 0) {
		SPDK_ERRLOG("Failed to quiesce raid bdev %s: %s\n",
			    raid_bdev->bdev.name, spdk_strerror(-status));
		raid_bdev_remove_base_bdev_done(base_info, status);
		return;
	}

	if (raid_bdev->sb) {
		struct raid_bdev_superblock *sb = raid_bdev->sb;
		uint8_t slot;

		/* Safety check: ensure base_bdev_info array is valid before calculating slot */
		if (raid_bdev->base_bdev_info == NULL) {
			SPDK_ERRLOG("raid_bdev->base_bdev_info is NULL for RAID bdev '%s'\n", raid_bdev->bdev.name);
			raid_bdev_remove_base_bdev_done(base_info, -EINVAL);
			return;
		}

		slot = raid_bdev_base_bdev_slot(base_info);
		uint8_t i;

		for (i = 0; i < sb->base_bdevs_size; i++) {
			struct raid_bdev_sb_base_bdev *sb_base_bdev = &sb->base_bdevs[i];

			if (sb_base_bdev->state == RAID_SB_BASE_BDEV_CONFIGURED &&
			    sb_base_bdev->slot == slot) {
				if (base_info->is_failed) {
					sb_base_bdev->state = RAID_SB_BASE_BDEV_FAILED;
				} else {
					sb_base_bdev->state = RAID_SB_BASE_BDEV_MISSING;
				}

				raid_bdev_write_superblock(raid_bdev, raid_bdev_remove_base_bdev_write_sb_cb, base_info);
				return;
			}
		}
	}

	raid_bdev_remove_base_bdev_cont(base_info);
}

static int
raid_bdev_remove_base_bdev_quiesce(struct raid_base_bdev_info *base_info)
{
	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	return spdk_bdev_quiesce(&base_info->raid_bdev->bdev, &g_raid_if,
				 raid_bdev_remove_base_bdev_on_quiesced, base_info);
}

struct raid_bdev_process_base_bdev_remove_ctx {
	struct raid_bdev_process *process;
	struct raid_base_bdev_info *base_info;
	uint8_t num_base_bdevs_operational;
};

static void
_raid_bdev_process_base_bdev_remove_cont(void *ctx)
{
	struct raid_base_bdev_info *base_info = ctx;
	int ret;

	ret = raid_bdev_remove_base_bdev_quiesce(base_info);
	if (ret != 0) {
		raid_bdev_remove_base_bdev_done(base_info, ret);
	}
}

static void
raid_bdev_process_base_bdev_remove_cont(void *_ctx)
{
	struct raid_bdev_process_base_bdev_remove_ctx *ctx = _ctx;
	struct raid_base_bdev_info *base_info = ctx->base_info;

	free(ctx);

	spdk_thread_send_msg(spdk_thread_get_app_thread(), _raid_bdev_process_base_bdev_remove_cont,
			     base_info);
}

static void
_raid_bdev_process_base_bdev_remove(void *_ctx)
{
	struct raid_bdev_process_base_bdev_remove_ctx *ctx = _ctx;
	struct raid_bdev_process *process = ctx->process;
	int ret;

	if (ctx->base_info != process->target &&
	    ctx->num_base_bdevs_operational > process->raid_bdev->min_base_bdevs_operational) {
		/* process doesn't need to be stopped */
		raid_bdev_process_base_bdev_remove_cont(ctx);
		return;
	}

	assert(process->state > RAID_PROCESS_STATE_INIT &&
	       process->state < RAID_PROCESS_STATE_STOPPED);

	ret = raid_bdev_process_add_finish_action(process, raid_bdev_process_base_bdev_remove_cont, ctx);
	if (ret != 0) {
		raid_bdev_remove_base_bdev_done(ctx->base_info, ret);
		free(ctx);
		return;
	}

	process->state = RAID_PROCESS_STATE_STOPPING;

	if (process->status == 0) {
		process->status = -ENODEV;
	}
}

static int
raid_bdev_process_base_bdev_remove(struct raid_bdev_process *process,
				   struct raid_base_bdev_info *base_info)
{
	struct raid_bdev_process_base_bdev_remove_ctx *ctx;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	/*
	 * We have to send the process and num_base_bdevs_operational in the message ctx
	 * because the process thread should not access raid_bdev's properties. Particularly,
	 * raid_bdev->process may be cleared by the time the message is handled, but ctx->process
	 * will still be valid until the process is fully stopped.
	 */
	ctx->base_info = base_info;
	ctx->process = process;
	/*
	 * raid_bdev->num_base_bdevs_operational can't be used here because it is decremented
	 * after the removal and more than one base bdev may be removed at the same time
	 */
	RAID_FOR_EACH_BASE_BDEV(process->raid_bdev, base_info) {
		if (base_info->is_configured && !base_info->remove_scheduled) {
			ctx->num_base_bdevs_operational++;
		}
	}

	spdk_thread_send_msg(process->thread, _raid_bdev_process_base_bdev_remove, ctx);

	return 0;
}

/* Forward declaration */
static int _raid_bdev_remove_base_bdev(struct raid_base_bdev_info *base_info,
				       raid_base_bdev_cb cb_fn, void *cb_ctx);

/*
 * Callback after wiping superblock - continue with removal
 */
static void
raid_bdev_remove_base_bdev_after_wipe(void *ctx, int status)
{
	struct raid_base_bdev_info *base_info = ctx;
	struct raid_bdev *raid_bdev;
	raid_base_bdev_cb cb_fn;
	void *cb_ctx;
	int ret;

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in raid_bdev_remove_base_bdev_after_wipe\n");
		return;
	}

	raid_bdev = base_info->raid_bdev;
	if (raid_bdev == NULL) {
		SPDK_ERRLOG("base_info->raid_bdev is NULL in raid_bdev_remove_base_bdev_after_wipe\n");
		return;
	}

	cb_fn = base_info->remove_cb;
	cb_ctx = base_info->remove_cb_ctx;

	if (status != 0) {
		/* Wipe failed, but continue with removal anyway */
		SPDK_WARNLOG("Failed to wipe superblock on base bdev '%s', continuing with removal\n",
			     base_info->name ? base_info->name : "unknown");
	}

	if (raid_bdev->state != RAID_BDEV_STATE_ONLINE) {
		raid_bdev_free_base_bdev_resource(base_info);
		base_info->remove_scheduled = false;
		if (raid_bdev->num_base_bdevs_discovered == 0 &&
		    raid_bdev->state == RAID_BDEV_STATE_OFFLINE) {
			raid_bdev_cleanup_and_free(raid_bdev);
		}
		if (cb_fn != NULL) {
			cb_fn(cb_ctx, 0);
		}
		return;
	}

	if (raid_bdev->min_base_bdevs_operational == raid_bdev->num_base_bdevs) {
		raid_bdev->num_base_bdevs_operational--;
		raid_bdev_deconfigure(raid_bdev, cb_fn, cb_ctx);
		return;
	}
	struct raid_bdev_process *process = raid_bdev_get_active_process(raid_bdev);

	if (process != NULL) {
		ret = raid_bdev_process_base_bdev_remove(process, base_info);
	} else {
		ret = raid_bdev_remove_base_bdev_quiesce(base_info);
	}

	if (ret != 0) {
		base_info->remove_scheduled = false;
		if (cb_fn != NULL) {
			cb_fn(cb_ctx, ret);
		}
	}
}

static int
_raid_bdev_remove_base_bdev(struct raid_base_bdev_info *base_info,
			    raid_base_bdev_cb cb_fn, void *cb_ctx)
{
	struct raid_bdev *raid_bdev;
	struct raid_bdev_process *process;
	int ret = 0;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in _raid_bdev_remove_base_bdev\n");
		return -EINVAL;
	}

	raid_bdev = base_info->raid_bdev;
	if (raid_bdev == NULL) {
		SPDK_ERRLOG("base_info->raid_bdev is NULL in _raid_bdev_remove_base_bdev\n");
		return -EINVAL;
	}
	process = raid_bdev_get_active_process(raid_bdev);

	SPDK_DEBUGLOG(bdev_raid, "%s\n", base_info->name ? base_info->name : "unknown");

	if (base_info->remove_scheduled || !base_info->is_configured) {
		return -ENODEV;
	}

	if (base_info->desc == NULL) {
		SPDK_ERRLOG("base_info->desc is NULL, cannot remove base bdev\n");
		return -ENODEV;
	}

	if (process != NULL &&
	    process->type == RAID_PROCESS_REBUILD &&
	    process->target == base_info) {
		SPDK_WARNLOG("Cannot remove target base bdev '%s' while rebuild is running\n",
			     base_info->name ? base_info->name : raid_bdev->bdev.name);
		return -EBUSY;
	}

	base_info->remove_scheduled = true;
	base_info->remove_cb = cb_fn;
	base_info->remove_cb_ctx = cb_ctx;

	/* Try to wipe superblock first (if disk is failed, this might fail, which is OK) */
	if (base_info->desc != NULL && base_info->app_thread_ch != NULL) {
		int rc = raid_bdev_wipe_single_base_bdev_superblock(base_info,
								     raid_bdev_remove_base_bdev_after_wipe,
								     base_info);
		if (rc == 0) {
			return 0;
		}
		/* Wipe failed - callback already called. Don't clear remove_scheduled/remove_cb/remove_cb_ctx. */
		SPDK_WARNLOG("Failed to start superblock wipe for base bdev '%s', removal handled by wipe callback\n",
			     base_info->name ? base_info->name : "unknown");
		return rc;
	}
	SPDK_DEBUGLOG(bdev_raid, "Cannot wipe superblock - base bdev desc or channel is NULL\n");

	if (raid_bdev->state != RAID_BDEV_STATE_ONLINE) {
		raid_bdev_free_base_bdev_resource(base_info);
		base_info->remove_scheduled = false;
		if (raid_bdev->num_base_bdevs_discovered == 0 &&
		    raid_bdev->state == RAID_BDEV_STATE_OFFLINE) {
			raid_bdev_cleanup_and_free(raid_bdev);
		}
		if (cb_fn != NULL) {
			cb_fn(cb_ctx, 0);
		}
	} else if (raid_bdev->min_base_bdevs_operational == raid_bdev->num_base_bdevs) {
		raid_bdev->num_base_bdevs_operational--;
		raid_bdev_deconfigure(raid_bdev, cb_fn, cb_ctx);
	} else {
		if (process != NULL) {
			ret = raid_bdev_process_base_bdev_remove(process, base_info);
		} else {
			ret = raid_bdev_remove_base_bdev_quiesce(base_info);
		}
		if (ret != 0) {
			base_info->remove_scheduled = false;
		}
	}

	return ret;
}

/*
 * brief:
 * raid_bdev_remove_base_bdev function is called by below layers when base_bdev
 * is removed. This function checks if this base bdev is part of any raid bdev
 * or not. If yes, it takes necessary action on that particular raid bdev.
 * params:
 * base_bdev - pointer to base bdev which got removed
 * cb_fn - callback function
 * cb_arg - argument to callback function
 * returns:
 * 0 - success
 * non zero - failure
 */
int
raid_bdev_remove_base_bdev(struct spdk_bdev *base_bdev, raid_base_bdev_cb cb_fn, void *cb_ctx)
{
	struct raid_base_bdev_info *base_info;

	/* Find the raid_bdev which has claimed this base_bdev */
	base_info = raid_bdev_find_base_info_by_bdev(base_bdev);
	if (!base_info) {
		SPDK_ERRLOG("bdev to remove '%s' not found\n", base_bdev->name);
		return -ENODEV;
	}

	return _raid_bdev_remove_base_bdev(base_info, cb_fn, cb_ctx);
}

static void
raid_bdev_fail_base_remove_cb(void *ctx, int status)
{
	struct raid_base_bdev_info *base_info = ctx;

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in raid_bdev_fail_base_remove_cb\n");
		return;
	}

	if (status != 0) {
		SPDK_WARNLOG("Failed to remove base bdev %s\n", base_info->name ? base_info->name : "unknown");
		base_info->is_failed = false;
	}
}

static void
_raid_bdev_fail_base_bdev(void *ctx)
{
	struct raid_base_bdev_info *base_info = ctx;
	int rc;

	if (base_info->is_failed) {
		return;
	}
	base_info->is_failed = true;

	SPDK_NOTICELOG("Failing base bdev in slot %d ('%s') of raid bdev '%s'\n",
		       raid_bdev_base_bdev_slot(base_info), base_info->name, base_info->raid_bdev->bdev.name);

	rc = _raid_bdev_remove_base_bdev(base_info, raid_bdev_fail_base_remove_cb, base_info);
	if (rc != 0) {
		raid_bdev_fail_base_remove_cb(base_info, rc);
	}
}

void
raid_bdev_fail_base_bdev(struct raid_base_bdev_info *base_info)
{
	spdk_thread_exec_msg(spdk_thread_get_app_thread(), _raid_bdev_fail_base_bdev, base_info);
}

static void
raid_bdev_resize_write_sb_cb(int status, struct raid_bdev *raid_bdev, void *ctx)
{
	if (status != 0) {
		SPDK_ERRLOG("Failed to write raid bdev '%s' superblock after resizing the bdev: %s\n",
			    raid_bdev->bdev.name, spdk_strerror(-status));
	}
}

/*
 * brief:
 * raid_bdev_resize_base_bdev function is called by below layers when base_bdev
 * is resized. This function checks if the smallest size of the base_bdevs is changed.
 * If yes, call module handler to resize the raid_bdev if implemented.
 * params:
 * base_bdev - pointer to base bdev which got resized.
 * returns:
 * none
 */
static void
raid_bdev_resize_base_bdev(struct spdk_bdev *base_bdev)
{
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;
	uint64_t blockcnt_old;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_resize_base_bdev\n");

	base_info = raid_bdev_find_base_info_by_bdev(base_bdev);

	/* Find the raid_bdev which has claimed this base_bdev */
	if (!base_info) {
		SPDK_ERRLOG("raid_bdev whose base_bdev '%s' not found\n", base_bdev->name);
		return;
	}
	raid_bdev = base_info->raid_bdev;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	SPDK_NOTICELOG("base_bdev '%s' was resized: old size %" PRIu64 ", new size %" PRIu64 "\n",
		       base_bdev->name, base_info->blockcnt, base_bdev->blockcnt);

	base_info->blockcnt = base_bdev->blockcnt;

	if (!raid_bdev->module->resize) {
		return;
	}

	blockcnt_old = raid_bdev->bdev.blockcnt;
	if (raid_bdev->module->resize(raid_bdev) == false) {
		return;
	}

	SPDK_NOTICELOG("raid bdev '%s': block count was changed from %" PRIu64 " to %" PRIu64 "\n",
		       raid_bdev->bdev.name, blockcnt_old, raid_bdev->bdev.blockcnt);

	if (raid_bdev->superblock_enabled) {
		struct raid_bdev_superblock *sb = raid_bdev->sb;
		uint8_t i;

		for (i = 0; i < sb->base_bdevs_size; i++) {
			struct raid_bdev_sb_base_bdev *sb_base_bdev = &sb->base_bdevs[i];

			if (sb_base_bdev->slot < raid_bdev->num_base_bdevs) {
				base_info = &raid_bdev->base_bdev_info[sb_base_bdev->slot];
				sb_base_bdev->data_size = base_info->data_size;
			}
		}
		sb->raid_size = raid_bdev->bdev.blockcnt;
		raid_bdev_write_superblock(raid_bdev, raid_bdev_resize_write_sb_cb, NULL);
	}
}

/*
 * brief:
 * raid_bdev_event_base_bdev function is called by below layers when base_bdev
 * triggers asynchronous event.
 * params:
 * type - event details.
 * bdev - bdev that triggered event.
 * event_ctx - context for event.
 * returns:
 * none
 */
static void
raid_bdev_event_base_bdev(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			  void *event_ctx)
{
	int rc;

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		rc = raid_bdev_remove_base_bdev(bdev, NULL, NULL);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to remove base bdev %s: %s\n",
				    spdk_bdev_get_name(bdev), spdk_strerror(-rc));
		}
		break;
	case SPDK_BDEV_EVENT_RESIZE:
		raid_bdev_resize_base_bdev(bdev);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

/*
 * brief:
 * Deletes the specified raid bdev
 * params:
 * raid_bdev - pointer to raid bdev
 * cb_fn - callback function
 * cb_arg - argument to callback function
 */
struct raid_bdev_delete_ctx {
    struct raid_bdev *raid_bdev;
    raid_bdev_destruct_cb cb_fn;
    void *cb_arg;
    bool abort_delete;
};

void
raid_bdev_delete(struct raid_bdev *raid_bdev, raid_bdev_destruct_cb cb_fn, void *cb_arg)
{
    SPDK_DEBUGLOG(bdev_raid, "delete raid bdev: %s\n", raid_bdev->bdev.name);

	struct raid_bdev_process *process = raid_bdev_get_active_process(raid_bdev);

	if (process != NULL) {
		if (process->type == RAID_PROCESS_REBUILD && process->target != NULL) {
			uint8_t target_slot = raid_bdev_base_bdev_slot(process->target);
			uint64_t current_offset = 0, total_size = 0;
			double percent = 0.0;
			
			raid_bdev_get_rebuild_progress(raid_bdev, &current_offset, &total_size);
			if (total_size > 0) {
				percent = (double)current_offset * 100.0 / (double)total_size;
			}
			
			SPDK_ERRLOG("\n");
			SPDK_ERRLOG("===========================================================\n");
			SPDK_ERRLOG("CANNOT DELETE RAID: Rebuild is in progress!\n");
			SPDK_ERRLOG("RAID bdev: %s\n", raid_bdev->bdev.name);
			SPDK_ERRLOG("Target disk: %s (slot %u)\n", 
				    process->target->name, target_slot);
			SPDK_ERRLOG("Rebuild progress: %.2f%% (%lu / %lu blocks)\n",
				    percent, current_offset, total_size);
			SPDK_ERRLOG("State: %s\n", raid_rebuild_state_to_str(process->rebuild_state));
			SPDK_ERRLOG("Please wait for rebuild to complete before deleting.\n");
			SPDK_ERRLOG("===========================================================\n");
			SPDK_ERRLOG("\n");
		} else {
			SPDK_ERRLOG("Cannot delete raid bdev %s while %s is in progress\n",
				    raid_bdev->bdev.name,
				    raid_bdev_process_to_str(process->type));
		}
		if (cb_fn != NULL) {
			cb_fn(cb_arg, -EBUSY);
		}
		return;
	}

    if (raid_bdev->destroy_started) {
        SPDK_DEBUGLOG(bdev_raid, "destroying raid bdev %s is already started\n",
                      raid_bdev->bdev.name);
        if (cb_fn) {
            cb_fn(cb_arg, -EALREADY);
        }
        return;
    }

    raid_bdev->destroy_started = true;

    /* If superblock is enabled and the raid is online with discovered bases, quiesce and wipe SB first */
    if (raid_bdev->superblock_enabled && raid_bdev->state == RAID_BDEV_STATE_ONLINE &&
        raid_bdev->num_base_bdevs_discovered > 0) {
        struct raid_bdev_delete_ctx *ctx = calloc(1, sizeof(*ctx));

        if (ctx != NULL) {
            int rc;
            ctx->raid_bdev = raid_bdev;
            ctx->cb_fn = cb_fn;
            ctx->cb_arg = cb_arg;
            ctx->abort_delete = false;

            rc = spdk_bdev_quiesce(&raid_bdev->bdev, &g_raid_if, raid_bdev_delete_on_quiesced, ctx);
            if (rc == 0) {
                return;
            }

            /* If quiesce fails, abort deletion and restore state */
            free(ctx);
            raid_bdev->destroy_started = false;
            if (cb_fn) {
                cb_fn(cb_arg, rc);
            }
            return;
        }
    }

    raid_bdev_delete_continue(raid_bdev, cb_fn, cb_arg);
}

static void
raid_bdev_delete_continue(struct raid_bdev *raid_bdev, raid_bdev_destruct_cb cb_fn, void *cb_arg)
{
    struct raid_base_bdev_info *base_info;

    RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
        base_info->remove_scheduled = true;

        if (raid_bdev->state != RAID_BDEV_STATE_ONLINE) {
            /*
             * As raid bdev is not registered yet or already unregistered,
             * so cleanup should be done here itself.
             */
            raid_bdev_free_base_bdev_resource(base_info);
        }
    }

    if (raid_bdev->num_base_bdevs_discovered == 0) {
        /* There is no base bdev for this raid, so free the raid device. */
        raid_bdev_cleanup_and_free(raid_bdev);
        if (cb_fn) {
            cb_fn(cb_arg, 0);
        }
    } else {
        raid_bdev_deconfigure(raid_bdev, cb_fn, cb_arg);
    }
}

static void
raid_bdev_delete_on_unquiesced(void *arg, int status)
{
    struct raid_bdev_delete_ctx *ctx = arg;
    struct raid_bdev *raid_bdev = ctx->raid_bdev;

    if (status != 0) {
        SPDK_ERRLOG("Failed to unquiesce raid bdev %s: %s\n", raid_bdev->bdev.name, spdk_strerror(-status));
    }

    if (ctx->abort_delete) {
        /* Restore state and stop with error */
        raid_bdev->destroy_started = false;
        if (ctx->cb_fn) {
            ctx->cb_fn(ctx->cb_arg, status != 0 ? status : -EIO);
        }
        free(ctx);
        return;
    }

    raid_bdev_delete_continue(raid_bdev, ctx->cb_fn, ctx->cb_arg);
    free(ctx);
}

static void raid_bdev_delete_restore_sb_cb(int restore_status, struct raid_bdev *raid_bdev, void *arg)
{
    struct raid_bdev_delete_ctx *ctx = arg;
    (void)restore_status; /* We will abort regardless, but we tried to restore. */
    ctx->abort_delete = true;
    spdk_bdev_unquiesce(&raid_bdev->bdev, &g_raid_if, raid_bdev_delete_on_unquiesced, ctx);
}

static void
raid_bdev_delete_wipe_cb(int status, struct raid_bdev *raid_bdev, void *arg)
{
    struct raid_bdev_delete_ctx *ctx = arg;

    if (status != 0) {
        SPDK_ERRLOG("Failed to wipe raid bdev '%s' superblock: %s\n",
                    raid_bdev->bdev.name, spdk_strerror(-status));
        /* Attempt to restore original SB before aborting */
        raid_bdev_write_superblock(raid_bdev, raid_bdev_delete_restore_sb_cb, ctx);
        return;
    }

    spdk_bdev_unquiesce(&raid_bdev->bdev, &g_raid_if, raid_bdev_delete_on_unquiesced, ctx);
}

static void
raid_bdev_delete_on_quiesced(void *arg, int status)
{
    struct raid_bdev_delete_ctx *ctx = arg;
    struct raid_bdev *raid_bdev = ctx->raid_bdev;

    if (status != 0) {
        SPDK_ERRLOG("Failed to quiesce raid bdev %s: %s\n", raid_bdev->bdev.name, spdk_strerror(-status));
        /* Abort: restore state and return error */
        raid_bdev->destroy_started = false;
        if (ctx->cb_fn) {
            ctx->cb_fn(ctx->cb_arg, status);
        }
        free(ctx);
        return;
    }

    /* Now wipe the superblock only on this raid's base devices */
    raid_bdev_wipe_superblock(raid_bdev, raid_bdev_delete_wipe_cb, ctx);
}

static void raid_bdev_process_free(struct raid_bdev_process *process);

static void
_raid_bdev_process_finish_done(void *ctx)
{
	struct raid_bdev_process *process = ctx;
	struct raid_process_finish_action *finish_action;

	while ((finish_action = TAILQ_FIRST(&process->finish_actions)) != NULL) {
		TAILQ_REMOVE(&process->finish_actions, finish_action, link);
		finish_action->cb(finish_action->cb_ctx);
		free(finish_action);
	}

	/* Call rebuild done callback if rebuild was in progress */
	if (process->type == RAID_PROCESS_REBUILD && process->rebuild_done_cb != NULL) {
		process->rebuild_done_cb(process->rebuild_done_ctx, process->status);
	}

	spdk_poller_unregister(&process->qos.process_continue_poller);

	raid_bdev_process_free(process);

	spdk_thread_exit(spdk_get_thread());
}

static void
raid_bdev_process_finish_target_removed(void *ctx, int status)
{
	struct raid_bdev_process *process = ctx;

	if (status != 0) {
		SPDK_ERRLOG("Failed to remove target bdev: %s\n", spdk_strerror(-status));
	}

	spdk_thread_send_msg(process->thread, _raid_bdev_process_finish_done, process);
}

static void
__raid_bdev_process_finish(struct spdk_io_channel_iter *i, int status)
{
	struct raid_bdev_process *process = spdk_io_channel_iter_get_ctx(i);

	spdk_thread_send_msg(process->thread, _raid_bdev_process_finish_done, process);
}

static void
raid_bdev_channel_process_finish(struct spdk_io_channel_iter *i)
{
	struct raid_bdev_process *process = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct raid_bdev_io_channel *raid_ch = spdk_io_channel_get_ctx(ch);

	if (process->status == 0 && process->target != NULL) {
		uint8_t slot = raid_bdev_base_bdev_slot(process->target);
		struct raid_bdev *raid_bdev = process->raid_bdev;

		/* Safety check: ensure slot is within valid range */
		if (slot < raid_bdev->num_base_bdevs && raid_ch->base_channel != NULL) {
			raid_ch->base_channel[slot] = raid_ch->process.target_ch;
			raid_ch->process.target_ch = NULL;
		} else {
			SPDK_ERRLOG("Invalid slot %u or base_channel is NULL\n", slot);
		}
	}

	raid_bdev_ch_process_cleanup(raid_ch);

	spdk_for_each_channel_continue(i, 0);
}


static void
raid_bdev_process_finish_quiesced(void *ctx, int status)
{
	struct raid_bdev_process *process = ctx;
	struct raid_bdev *raid_bdev = process->raid_bdev;

	if (status != 0) {
		SPDK_ERRLOG("Failed to quiesce bdev: %s\n", spdk_strerror(-status));
		/* Quiesce failed - need to clean up process state */
		/* Process is in STOPPING state, need to finish cleanup */
		raid_bdev->process = NULL;
		if (process->target != NULL) {
			process->target->is_process_target = false;
		}
		/* Send message to process thread to finish cleanup */
		spdk_thread_send_msg(process->thread, _raid_bdev_process_finish_done, process);
		return;
	}

	/* Handle rebuild failure: remove target disk if rebuild failed */
	if (process->status != 0 && process->type == RAID_PROCESS_REBUILD) {
		if (process->target != NULL) {
			SPDK_ERRLOG("\n");
			SPDK_ERRLOG("===========================================================\n");
			SPDK_ERRLOG("REBUILD FAILED: Target disk '%s' may have been removed\n",
				    process->target->name ? process->target->name : "unknown");
			SPDK_ERRLOG("RAID bdev: %s\n", process->raid_bdev->bdev.name);
			SPDK_ERRLOG("Error: %s\n", spdk_strerror(-process->status));
			SPDK_ERRLOG("Removing target disk from RAID array...\n");
			SPDK_ERRLOG("===========================================================\n");
			SPDK_ERRLOG("\n");
			
			status = _raid_bdev_remove_base_bdev(process->target, raid_bdev_process_finish_target_removed,
							     process);
			/* _raid_bdev_remove_base_bdev may:
			 * - Return 0: wipe started, callback will be called when wipe completes
			 * - Return non-zero: either callback already called (wipe failed) or synchronous error
			 *   In case of synchronous error (e.g., -EBUSY, -ENODEV), callback is NOT called,
			 *   so we need to call it ourselves.
			 */
			if (status != 0) {
				/* Check if this is a synchronous error (callback not called) */
				/* Errors like -EBUSY, -ENODEV, -EINVAL are synchronous and don't call callback */
				if (status == -EBUSY || status == -ENODEV || status == -EINVAL) {
					/* Synchronous error - callback not called, call it now */
					SPDK_WARNLOG("Failed to remove target disk: %s\n", spdk_strerror(-status));
					raid_bdev_process_finish_target_removed(process, status);
				} else {
					/* Other errors (like from wipe) - callback may have been called already */
					/* Don't call callback again to avoid double callback */
					SPDK_WARNLOG("Failed to remove target disk: %s (callback may have been called)\n",
						     spdk_strerror(-status));
				}
			}
			/* If removal was started (status == 0), callback will be called when wipe completes */
			return;
		}
	}

	raid_bdev->process = NULL;
	if (process->target != NULL) {
		process->target->is_process_target = false;
	}

	/* Reuse original multi-channel cleanup */
	spdk_for_each_channel(process->raid_bdev, raid_bdev_channel_process_finish, process,
			      __raid_bdev_process_finish);
}

static void
_raid_bdev_process_finish(void *ctx)
{
	struct raid_bdev_process *process = ctx;
	int rc;

	rc = spdk_bdev_quiesce(&process->raid_bdev->bdev, &g_raid_if,
			       raid_bdev_process_finish_quiesced, process);
	if (rc != 0) {
		raid_bdev_process_finish_quiesced(ctx, rc);
	}
}

static void
raid_bdev_process_do_finish(struct raid_bdev_process *process)
{
	spdk_thread_send_msg(spdk_thread_get_app_thread(), _raid_bdev_process_finish, process);
}

static void raid_bdev_process_unlock_window_range(struct raid_bdev_process *process);
static void raid_bdev_process_thread_run(struct raid_bdev_process *process);

static void
raid_bdev_process_finish(struct raid_bdev_process *process, int status)
{
	assert(spdk_get_thread() == process->thread);

	if (process->status == 0) {
		process->status = status;
	}

	if (process->state >= RAID_PROCESS_STATE_STOPPING) {
		return;
	}

	assert(process->state == RAID_PROCESS_STATE_RUNNING);
	process->state = RAID_PROCESS_STATE_STOPPING;

	/* For simplified rebuild: set error_status if finishing with error */
	if (process->type == RAID_PROCESS_REBUILD && status != 0) {
		process->error_status = status;
	}

	if (process->window_range_locked) {
		raid_bdev_process_unlock_window_range(process);
	} else {
		raid_bdev_process_thread_run(process);
	}
}

static void
raid_bdev_process_window_range_unlocked(void *ctx, int status)
{
	struct raid_bdev_process *process = ctx;

	if (status != 0) {
		SPDK_ERRLOG("Failed to unlock LBA range: %s\n", spdk_strerror(-status));
		raid_bdev_process_finish(process, status);
		return;
	}

	process->window_range_locked = false;
	process->window_offset += process->window_size;

	/* Update rebuild progress in superblock periodically (every window) */
	if (process->type == RAID_PROCESS_REBUILD && 
	    process->raid_bdev->sb != NULL && 
	    process->target != NULL) {
		uint8_t target_slot = raid_bdev_base_bdev_slot(process->target);
		/* Update progress every window (can be optimized to update less frequently) */
		raid_bdev_sb_update_rebuild_progress(process->raid_bdev, target_slot,
						     process->window_offset,
						     process->raid_bdev->bdev.blockcnt);
		/* Write superblock asynchronously (non-blocking) */
		raid_bdev_write_superblock_async(process->raid_bdev);
	}

	/* Reuse original thread_run logic */
	raid_bdev_process_thread_run(process);
}

static void
raid_bdev_process_unlock_window_range(struct raid_bdev_process *process)
{
	int rc;

	assert(process->window_range_locked == true);

	rc = spdk_bdev_unquiesce_range(&process->raid_bdev->bdev, &g_raid_if,
				       process->window_offset, process->max_window_size,
				       raid_bdev_process_window_range_unlocked, process);
	if (rc != 0) {
		raid_bdev_process_window_range_unlocked(process, rc);
	}
}

static void
raid_bdev_process_channels_update_done(struct spdk_io_channel_iter *i, int status)
{
	struct raid_bdev_process *process = spdk_io_channel_iter_get_ctx(i);

	raid_bdev_process_unlock_window_range(process);
}

static void
raid_bdev_process_channel_update(struct spdk_io_channel_iter *i)
{
	struct raid_bdev_process *process = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct raid_bdev_io_channel *raid_ch = spdk_io_channel_get_ctx(ch);

	raid_ch->process.offset = process->window_offset + process->window_size;

	spdk_for_each_channel_continue(i, 0);
}

void
raid_bdev_process_request_complete(struct raid_bdev_process_request *process_req, int status)
{
	struct raid_bdev_process *process = process_req->process;

	assert(spdk_get_thread() == process->thread);

	/* Simplified error handling: set error status atomically */
	if (status != 0) {
		process->window_status = status;
		process->error_status = status;
	}

	/* Reuse original request handling logic */
	assert(process->window_remaining >= process_req->num_blocks);
	TAILQ_INSERT_TAIL(&process->requests, process_req, link);

	process->window_remaining -= process_req->num_blocks;
	if (process->window_remaining == 0) {
		/* Unified error handling: error_status and window_status should be in sync
		 * For rebuild, prefer error_status; for other processes, use window_status
		 */
		int error_to_report = 0;
		if (process->type == RAID_PROCESS_REBUILD && process->error_status != 0) {
			error_to_report = process->error_status;
		} else if (process->window_status != 0) {
			error_to_report = process->window_status;
		}
		
		if (error_to_report != 0) {
			raid_bdev_process_finish(process, error_to_report);
			return;
		}

		spdk_for_each_channel(process->raid_bdev, raid_bdev_process_channel_update, process,
				      raid_bdev_process_channels_update_done);
	}
}

static int
raid_bdev_submit_process_request(struct raid_bdev_process *process, uint64_t offset_blocks,
				 uint32_t num_blocks)
{
	struct raid_bdev *raid_bdev = process->raid_bdev;
	struct raid_bdev_process_request *process_req;
	int ret;

	process_req = TAILQ_FIRST(&process->requests);
	if (process_req == NULL) {
		assert(process->window_remaining > 0);
		return 0;
	}

	process_req->target = process->target;
	process_req->target_ch = process->raid_ch->process.target_ch;
	process_req->offset_blocks = offset_blocks;
	process_req->num_blocks = num_blocks;
	process_req->iov.iov_len = num_blocks * raid_bdev->bdev.blocklen;

	ret = raid_bdev->module->submit_process_request(process_req, process->raid_ch);
	if (ret <= 0) {
		if (ret < 0) {
			SPDK_ERRLOG("Failed to submit process request on %s: %s\n",
				    raid_bdev->bdev.name, spdk_strerror(-ret));
			process->window_status = ret;
			/* For simplified rebuild: also set error_status */
			if (process->type == RAID_PROCESS_REBUILD) {
				process->error_status = ret;
			}
		}
		/* ret == 0 means no request available (normal), ret < 0 means error */
		return ret;
	}

	process_req->num_blocks = ret;
	TAILQ_REMOVE(&process->requests, process_req, link);

	return ret;
}

static void
_raid_bdev_process_thread_run(struct raid_bdev_process *process)
{
	struct raid_bdev *raid_bdev = process->raid_bdev;
	uint64_t offset = process->window_offset;
	const uint64_t offset_end = spdk_min(offset + process->max_window_size, raid_bdev->bdev.blockcnt);
	int ret;

	/* Initialize window_status for this window */
	process->window_status = 0;
	process->window_remaining = 0;

	while (offset < offset_end) {
		ret = raid_bdev_submit_process_request(process, offset, offset_end - offset);
		if (ret < 0) {
			/* Error occurred, already set in window_status */
			break;
		} else if (ret == 0) {
			/* No request available (normal case, e.g., waiting for I/O completion) */
			/* Continue to next iteration or wait */
			break;
		}

		process->window_remaining += ret;
		offset += ret;
	}

	if (process->window_remaining > 0) {
		process->window_size = process->window_remaining;
	} else if (process->window_status != 0) {
		/* Error occurred */
		raid_bdev_process_finish(process, process->window_status);
	} else {
		/* window_remaining == 0 and window_status == 0
		 * This means no I/O was submitted in this iteration.
		 * If ret == 0, it means no request available (normal when waiting for I/O completion).
		 * If ret > 0 was never returned, it means we couldn't submit any I/O.
		 * In either case, if we've reached the end of the window, we should continue to next window.
		 * But if we're in the middle and can't submit, this might indicate a problem.
		 * 
		 * If offset < offset_end and no I/O was submitted, we need to continue processing
		 * in the next iteration (via process_continue_poller or next window lock).
		 * The process will be resumed when requests become available.
		 */
		if (offset >= offset_end) {
			/* Reached end of window, continue to next window */
			SPDK_DEBUGLOG(bdev_raid, "Reached end of window without submitting I/O\n");
			/* Continue to next window - this will be handled by the window completion callback */
		} else {
			/* In middle of window but couldn't submit - wait for requests to become available */
			SPDK_DEBUGLOG(bdev_raid, "No I/O submitted in this window, waiting for requests\n");
			/* The process will be resumed when requests become available via process_continue_poller */
			/* No need to do anything here - the poller will retry */
		}
	}
}

static void
raid_bdev_process_window_range_locked(void *ctx, int status)
{
	struct raid_bdev_process *process = ctx;

	if (status != 0) {
		SPDK_ERRLOG("Failed to lock LBA range: %s\n", spdk_strerror(-status));
		raid_bdev_process_finish(process, status);
		return;
	}

	process->window_range_locked = true;

	if (process->state == RAID_PROCESS_STATE_STOPPING) {
		raid_bdev_process_unlock_window_range(process);
		return;
	}

	_raid_bdev_process_thread_run(process);
}

static bool
raid_bdev_process_consume_token(struct raid_bdev_process *process)
{
	struct raid_bdev *raid_bdev = process->raid_bdev;
	uint64_t now = spdk_get_ticks();

	process->qos.bytes_available = spdk_min(process->qos.bytes_max,
						process->qos.bytes_available +
						(now - process->qos.last_tsc) * process->qos.bytes_per_tsc);
	process->qos.last_tsc = now;
	if (process->qos.bytes_available > 0.0) {
		process->qos.bytes_available -= process->window_size * raid_bdev->bdev.blocklen;
		return true;
	}
	return false;
}

static bool
raid_bdev_process_lock_window_range(struct raid_bdev_process *process)
{
	struct raid_bdev *raid_bdev = process->raid_bdev;
	int rc;

	assert(process->window_range_locked == false);

	if (process->qos.enable_qos) {
		if (raid_bdev_process_consume_token(process)) {
			spdk_poller_pause(process->qos.process_continue_poller);
		} else {
			spdk_poller_resume(process->qos.process_continue_poller);
			return false;
		}
	}

	rc = spdk_bdev_quiesce_range(&raid_bdev->bdev, &g_raid_if,
				     process->window_offset, process->max_window_size,
				     raid_bdev_process_window_range_locked, process);
	if (rc != 0) {
		raid_bdev_process_window_range_locked(process, rc);
	}
	return true;
}

static int
raid_bdev_process_continue_poll(void *arg)
{
	struct raid_bdev_process *process = arg;

	if (raid_bdev_process_lock_window_range(process)) {
		return SPDK_POLLER_BUSY;
	}
	return SPDK_POLLER_IDLE;
}

static void
raid_bdev_process_thread_run(struct raid_bdev_process *process)
{
	struct raid_bdev *raid_bdev = process->raid_bdev;

	assert(spdk_get_thread() == process->thread);

	if (process->state == RAID_PROCESS_STATE_STOPPING) {
		raid_bdev_process_do_finish(process);
		return;
	}

	if (process->window_offset >= raid_bdev->bdev.blockcnt) {
		SPDK_DEBUGLOG(bdev_raid, "process completed on %s\n", raid_bdev->bdev.name);
		raid_bdev_process_finish(process, 0);
		return;
	}

	/* Reuse original LBA locking mechanism for all process types */
	assert(process->window_remaining == 0);
	assert(process->window_range_locked == false);
	process->max_window_size = spdk_min(raid_bdev->bdev.blockcnt - process->window_offset,
					    process->max_window_size);
	raid_bdev_process_lock_window_range(process);
}


static void
raid_bdev_process_thread_init(void *ctx)
{
	struct raid_bdev_process *process = ctx;
	struct raid_bdev *raid_bdev = process->raid_bdev;
	struct spdk_io_channel *ch;

	process->thread = spdk_get_thread();

	ch = spdk_get_io_channel(raid_bdev);
	if (ch == NULL) {
		process->status = -ENOMEM;
		process->error_status = -ENOMEM;
		raid_bdev_process_do_finish(process);
		return;
	}

	process->raid_ch = spdk_io_channel_get_ctx(ch);

	process->state = RAID_PROCESS_STATE_RUNNING;

	if (process->qos.enable_qos) {
		process->qos.process_continue_poller = SPDK_POLLER_REGISTER(raid_bdev_process_continue_poll,
						       process, 0);
		spdk_poller_pause(process->qos.process_continue_poller);
	}

	if (process->type == RAID_PROCESS_REBUILD && process->target != NULL) {
		uint8_t target_slot = raid_bdev_base_bdev_slot(process->target);
		double percent = 0.0;
		uint64_t total_size = raid_bdev->bdev.blockcnt;
		uint64_t current_offset = process->window_offset;
		
		if (total_size > 0) {
			percent = (double)current_offset * 100.0 / (double)total_size;
		}
		
		SPDK_NOTICELOG("\n");
		SPDK_NOTICELOG("===========================================================\n");
		SPDK_NOTICELOG("REBUILD IN PROGRESS: RAID bdev '%s'\n", raid_bdev->bdev.name);
		SPDK_NOTICELOG("Target disk: %s (slot %u)\n", 
			       process->target->name, target_slot);
		SPDK_NOTICELOG("Rebuild started at offset: %lu blocks (%.2f%%)\n",
			       current_offset, percent);
		SPDK_NOTICELOG("Total size: %lu blocks\n", total_size);
		SPDK_NOTICELOG("State: %s\n", raid_rebuild_state_to_str(process->rebuild_state));
		SPDK_NOTICELOG("Rebuild is running in background. This may take a while...\n");
		SPDK_NOTICELOG("Check progress: bdev_raid_get_bdevs\n");
		SPDK_NOTICELOG("===========================================================\n");
		SPDK_NOTICELOG("\n");
	} else {
		SPDK_NOTICELOG("Started %s on raid bdev %s\n",
			       raid_bdev_process_to_str(process->type), raid_bdev->bdev.name);
	}

	raid_bdev_process_thread_run(process);
}

static void
raid_bdev_channels_abort_start_process_done(struct spdk_io_channel_iter *i, int status)
{
	struct raid_bdev_process *process = spdk_io_channel_iter_get_ctx(i);

	if (process->target != NULL) {
		_raid_bdev_remove_base_bdev(process->target, NULL, NULL);
	}
	raid_bdev_process_free(process);

	/* TODO: update sb */
}

static void
raid_bdev_channel_abort_start_process(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct raid_bdev_io_channel *raid_ch = spdk_io_channel_get_ctx(ch);

	raid_bdev_ch_process_cleanup(raid_ch);

	spdk_for_each_channel_continue(i, 0);
}

static void
raid_bdev_channels_start_process_done(struct spdk_io_channel_iter *i, int status)
{
	struct raid_bdev_process *process = spdk_io_channel_iter_get_ctx(i);
	struct raid_bdev *raid_bdev = process->raid_bdev;
	struct spdk_thread *thread;
	char thread_name[RAID_BDEV_SB_NAME_SIZE + 16];

	if (status == 0 && process->target != NULL &&
	    (process->target->remove_scheduled || !process->target->is_configured ||
	     raid_bdev->num_base_bdevs_operational <= raid_bdev->min_base_bdevs_operational)) {
		/* a base bdev was removed before we got here */
		status = -ENODEV;
	}

	if (status != 0) {
		SPDK_ERRLOG("Failed to start %s on %s: %s\n",
			    raid_bdev_process_to_str(process->type), raid_bdev->bdev.name,
			    spdk_strerror(-status));
		goto err;
	}

	snprintf(thread_name, sizeof(thread_name), "%s_%s",
		 raid_bdev->bdev.name, raid_bdev_process_to_str(process->type));

	thread = spdk_thread_create(thread_name, NULL);
	if (thread == NULL) {
		SPDK_ERRLOG("Failed to create %s thread for %s\n",
			    raid_bdev_process_to_str(process->type), raid_bdev->bdev.name);
		goto err;
	}

	raid_bdev->process = process;

	spdk_thread_send_msg(thread, raid_bdev_process_thread_init, process);

	return;
err:
	spdk_for_each_channel(process->raid_bdev, raid_bdev_channel_abort_start_process, process,
			      raid_bdev_channels_abort_start_process_done);
}

static void
raid_bdev_channel_start_process(struct spdk_io_channel_iter *i)
{
	struct raid_bdev_process *process = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct raid_bdev_io_channel *raid_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	rc = raid_bdev_ch_process_setup(raid_ch, process);

	spdk_for_each_channel_continue(i, rc);
}

static void
raid_bdev_process_start(struct raid_bdev_process *process)
{
	struct raid_bdev *raid_bdev = process->raid_bdev;

	assert(raid_bdev->module->submit_process_request != NULL);

	/* Initialize simplified rebuild error status */
	if (process->type == RAID_PROCESS_REBUILD) {
		process->error_status = 0;
	}

	/* Reuse original multi-channel setup (but simplified processing) */
	spdk_for_each_channel(raid_bdev, raid_bdev_channel_start_process, process,
			      raid_bdev_channels_start_process_done);
}

static void
raid_bdev_process_request_free(struct raid_bdev_process_request *process_req)
{
	spdk_dma_free(process_req->iov.iov_base);
	spdk_dma_free(process_req->md_buf);
	free(process_req);
}

static struct raid_bdev_process_request *
raid_bdev_process_alloc_request(struct raid_bdev_process *process)
{
	struct raid_bdev *raid_bdev = process->raid_bdev;
	struct raid_bdev_process_request *process_req;

	process_req = calloc(1, sizeof(*process_req));
	if (process_req == NULL) {
		return NULL;
	}

	process_req->process = process;
	process_req->iov.iov_len = process->max_window_size * raid_bdev->bdev.blocklen;
	process_req->iov.iov_base = spdk_dma_malloc(process_req->iov.iov_len, 4096, 0);
	if (process_req->iov.iov_base == NULL) {
		free(process_req);
		return NULL;
	}
	if (spdk_bdev_is_md_separate(&raid_bdev->bdev)) {
		process_req->md_buf = spdk_dma_malloc(process->max_window_size * raid_bdev->bdev.md_len, 4096, 0);
		if (process_req->md_buf == NULL) {
			raid_bdev_process_request_free(process_req);
			return NULL;
		}
	}

	return process_req;
}

static void
raid_bdev_process_free(struct raid_bdev_process *process)
{
	struct raid_bdev_process_request *process_req;

	while ((process_req = TAILQ_FIRST(&process->requests)) != NULL) {
		TAILQ_REMOVE(&process->requests, process_req, link);
		raid_bdev_process_request_free(process_req);
	}

	free(process);
}

static struct raid_bdev_process *
raid_bdev_process_alloc(struct raid_bdev *raid_bdev, enum raid_process_type type,
			struct raid_base_bdev_info *target)
{
	struct raid_bdev_process *process;
	struct raid_bdev_process_request *process_req;
	int i;

	process = calloc(1, sizeof(*process));
	if (process == NULL) {
		return NULL;
	}

	process->raid_bdev = raid_bdev;
	process->type = type;
	process->target = target;
	process->max_window_size = spdk_max(spdk_divide_round_up(g_opts.process_window_size_kb * 1024UL,
					    spdk_bdev_get_data_block_size(&raid_bdev->bdev)),
					    raid_bdev->bdev.write_unit_size);
	if (raid_bdev->level == RAID10) {
		process->max_window_size = spdk_min(process->max_window_size, raid_bdev->strip_size);
	}
	TAILQ_INIT(&process->requests);
	TAILQ_INIT(&process->finish_actions);
	/* Initialize rebuild state and callback */
	process->rebuild_state = RAID_REBUILD_STATE_IDLE;
	process->rebuild_done_cb = NULL;
	process->rebuild_done_ctx = NULL;
	/* Initialize simplified rebuild error status */
	process->error_status = 0;

	if (g_opts.process_max_bandwidth_mb_sec != 0) {
		process->qos.enable_qos = true;
		process->qos.last_tsc = spdk_get_ticks();
		process->qos.bytes_per_tsc = g_opts.process_max_bandwidth_mb_sec * 1024 * 1024.0 /
					     spdk_get_ticks_hz();
		process->qos.bytes_max = g_opts.process_max_bandwidth_mb_sec * 1024 * 1024.0 / SPDK_SEC_TO_MSEC;
		process->qos.bytes_available = 0.0;
	}

	for (i = 0; i < RAID_BDEV_PROCESS_MAX_QD; i++) {
		process_req = raid_bdev_process_alloc_request(process);
		if (process_req == NULL) {
			raid_bdev_process_free(process);
			return NULL;
		}

		TAILQ_INSERT_TAIL(&process->requests, process_req, link);
	}

	return process;
}

/* Forward declarations */
static const struct raid_bdev_sb_base_bdev *
raid_bdev_sb_find_base_bdev_by_slot(const struct raid_bdev *raid_bdev, uint8_t slot);
static int
raid_bdev_start_rebuild_with_cb(struct raid_base_bdev_info *target,
				void (*done_cb)(void *ctx, int status), void *done_ctx);

static int
raid_bdev_start_rebuild(struct raid_base_bdev_info *target)
{
	return raid_bdev_start_rebuild_with_cb(target, NULL, NULL);
}

static int
raid_bdev_start_rebuild_with_cb(struct raid_base_bdev_info *target,
				void (*done_cb)(void *ctx, int status), void *done_ctx)
{
	struct raid_bdev_process *process;
	struct raid_bdev *raid_bdev = target->raid_bdev;
	uint8_t target_slot;
	uint64_t rebuild_offset = 0;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	process = raid_bdev_process_alloc(raid_bdev, RAID_PROCESS_REBUILD, target);
	if (process == NULL) {
		return -ENOMEM;
	}

	/* Set rebuild callback */
	process->rebuild_done_cb = done_cb;
	process->rebuild_done_ctx = done_ctx;

	/* Find target slot */
	target_slot = raid_bdev_base_bdev_slot(target);

	/* Safety check: ensure target_slot is within valid range */
	if (target_slot >= raid_bdev->num_base_bdevs) {
		SPDK_ERRLOG("Calculated target_slot %u exceeds num_base_bdevs %u for RAID bdev '%s'\n",
			    target_slot, raid_bdev->num_base_bdevs, raid_bdev->bdev.name);
		return -EINVAL;
	}

	/* Check if we're resuming a previous rebuild (REBUILDING state) */
	if (raid_bdev->sb != NULL) {
		const struct raid_bdev_sb_base_bdev *sb_base = raid_bdev_sb_find_base_bdev_by_slot(raid_bdev, target_slot);
		if (sb_base != NULL && sb_base->state == RAID_SB_BASE_BDEV_REBUILDING) {
			/* Resume from previous progress */
			rebuild_offset = sb_base->rebuild_offset;
			/* Validate rebuild_offset: ensure it's within valid range */
			if (rebuild_offset > raid_bdev->bdev.blockcnt) {
				SPDK_WARNLOG("Invalid rebuild_offset %lu (exceeds blockcnt %lu), resetting to 0\n",
					     rebuild_offset, raid_bdev->bdev.blockcnt);
				rebuild_offset = 0;
			}
			process->window_offset = rebuild_offset;
			SPDK_NOTICELOG("Resuming rebuild for base bdev %s (slot %u) from offset %lu blocks\n",
				       target->name, target_slot, rebuild_offset);
		}
	}

	/* Update superblock to mark base bdev as REBUILDING before starting rebuild */
	if (raid_bdev->sb != NULL) {
		if (raid_bdev_sb_update_base_bdev_state(raid_bdev, target_slot,
							 RAID_SB_BASE_BDEV_REBUILDING)) {
			/* Initialize or update rebuild progress in superblock */
			raid_bdev_sb_update_rebuild_progress(raid_bdev, target_slot, rebuild_offset,
							     raid_bdev->bdev.blockcnt);
			raid_bdev_write_superblock_async(raid_bdev);
		}
	}

	raid_bdev_process_start(process);

	return 0;
}

static void raid_bdev_configure_base_bdev_cont(struct raid_base_bdev_info *base_info);

static void
_raid_bdev_configure_base_bdev_cont(struct spdk_io_channel_iter *i, int status)
{
	struct raid_base_bdev_info *base_info = spdk_io_channel_iter_get_ctx(i);

	raid_bdev_configure_base_bdev_cont(base_info);
}

static void
raid_bdev_ch_sync(struct spdk_io_channel_iter *i)
{
	spdk_for_each_channel_continue(i, 0);
}

/* Forward declarations for helper functions */

static void
raid_bdev_configure_base_bdev_cont(struct raid_base_bdev_info *base_info)
{
	struct raid_bdev *raid_bdev = base_info->raid_bdev;
	raid_base_bdev_cb configure_cb = NULL;
	int rc;

	/* Safety check: ensure raid_bdev is valid */
	if (raid_bdev == NULL) {
		SPDK_ERRLOG("base_info->raid_bdev is NULL in raid_bdev_configure_base_bdev_cont\n");
		if (base_info->configure_cb) {
			configure_cb = base_info->configure_cb;
			base_info->configure_cb = NULL;
			configure_cb(base_info->configure_cb_ctx, -EINVAL);
		}
		return;
	}

	if (raid_bdev->num_base_bdevs_discovered == raid_bdev->num_base_bdevs_operational &&
	    base_info->is_process_target == false) {
		/* TODO: defer if rebuild in progress on another base bdev */
		assert(raid_bdev->process == NULL);
		assert(raid_bdev->state == RAID_BDEV_STATE_ONLINE);
		base_info->is_process_target = true;
		/* To assure is_process_target is set before is_configured when checked in raid_bdev_create_cb() */
		spdk_for_each_channel(raid_bdev, raid_bdev_ch_sync, base_info, _raid_bdev_configure_base_bdev_cont);
		return;
	}

	/* For RAID10: Check if this slot is already occupied (scenario 4: old disk re-inserted after rebuild) */
	if (raid_bdev->level == RAID10 && raid_bdev->state == RAID_BDEV_STATE_ONLINE && raid_bdev->sb != NULL) {
		/* Safety check: ensure base_bdev_info array is valid before calculating slot */
		if (raid_bdev->base_bdev_info == NULL) {
			SPDK_ERRLOG("raid_bdev->base_bdev_info is NULL for RAID bdev '%s'\n", raid_bdev->bdev.name);
			if (base_info->configure_cb) {
				configure_cb = base_info->configure_cb;
				base_info->configure_cb = NULL;
				configure_cb(base_info->configure_cb_ctx, -EINVAL);
			}
			return;
		}
		uint8_t slot_idx = raid_bdev_base_bdev_slot(base_info);

		/* Check if slot is already occupied by another disk
		 * Note: We only check is_configured, not name, because name is set
		 * during the add process before configure_cont is called.
		 * If is_configured is true, it means this slot was already configured
		 * and should not be reused for a new disk.
		 */
		if (base_info->is_configured) {
			/* Slot is already occupied - this disk cannot be added */
			const struct raid_bdev_sb_base_bdev *sb_base = raid_bdev_sb_find_base_bdev_by_slot(raid_bdev, slot_idx);
			const char *uuid_match_info = "";

			if (sb_base != NULL && sb_base->state == RAID_SB_BASE_BDEV_CONFIGURED) {
				/* Check if UUID matches - if it matches, this is the old disk re-inserted */
				if (!spdk_uuid_is_null(&sb_base->uuid) &&
				    !spdk_uuid_is_null(&base_info->uuid) &&
				    spdk_uuid_compare(&base_info->uuid, &sb_base->uuid) == 0) {
					uuid_match_info = " (UUID matches - this is the old disk that was replaced)";
				} else {
					uuid_match_info = " (UUID mismatch - different disk)";
				}
			}

			SPDK_NOTICELOG("===========================================================\n");
			SPDK_NOTICELOG("Base bdev '%s' (slot %u) cannot be added to RAID bdev '%s' because the slot is already occupied.%s\n",
				       base_info->name, slot_idx, raid_bdev->bdev.name, uuid_match_info);
			SPDK_NOTICELOG("The RAID bdev is already fully configured with another disk in this slot.\n");
			SPDK_NOTICELOG("If you don't need this disk for the RAID bdev, please clear its superblock\n");
			SPDK_NOTICELOG("using RPC 'bdev_wipe_superblock' to make it available for other use.\n");
			SPDK_NOTICELOG("Example: bdev_wipe_superblock -b %s\n", base_info->name);
			SPDK_NOTICELOG("===========================================================\n");

			/* Clear configure_cb before freeing resources, as raid_bdev_free_base_bdev_resource
			 * asserts that configure_cb is NULL
			 */
			if (base_info->configure_cb) {
				configure_cb = base_info->configure_cb;
				base_info->configure_cb = NULL;
			}
			raid_bdev_free_base_bdev_resource(base_info);
			if (configure_cb) {
				configure_cb(base_info->configure_cb_ctx, -EEXIST);
			}
			return;
		}
	}

	base_info->is_configured = true;

	raid_bdev->num_base_bdevs_discovered++;
	assert(raid_bdev->num_base_bdevs_discovered <= raid_bdev->num_base_bdevs);
	assert(raid_bdev->num_base_bdevs_operational <= raid_bdev->num_base_bdevs);
	assert(raid_bdev->num_base_bdevs_operational >= raid_bdev->min_base_bdevs_operational);

	configure_cb = base_info->configure_cb;
	base_info->configure_cb = NULL;
	/*
	 * Configure the raid bdev when the number of discovered base bdevs reaches the number
	 * of base bdevs we know to be operational members of the array. Usually this is equal
	 * to the total number of base bdevs (num_base_bdevs) but can be less - when the array is
	 * degraded.
	 */
	if (raid_bdev->num_base_bdevs_discovered == raid_bdev->num_base_bdevs_operational) {
		rc = raid_bdev_configure(raid_bdev, configure_cb, base_info->configure_cb_ctx);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to configure raid bdev: %s\n", spdk_strerror(-rc));
			/* raid_bdev_configure failed synchronously - callback was not set, need to call it here */
			/* But wait - if rc != 0, raid_bdev_configure may have already called the callback or not set it */
			/* Actually, looking at raid_bdev_configure code, if it fails synchronously (rc != 0),
			 * it sets configure_cb to NULL (line 2150) and returns the error.
			 * So configure_cb should still be valid here and we need to call it.
			 */
		} else {
			/* raid_bdev_configure is asynchronous - callback will be called later in raid_bdev_configure_cont */
			/* Set configure_cb to NULL to prevent duplicate callback */
			configure_cb = NULL;
		}
	} else if (base_info->is_process_target) {
		raid_bdev->num_base_bdevs_operational++;
		rc = raid_bdev_start_rebuild(base_info);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to start rebuild: %s\n", spdk_strerror(-rc));
			/* Rebuild failed - rollback state */
			base_info->is_configured = false;
			base_info->is_process_target = false;
			raid_bdev->num_base_bdevs_operational--;
			_raid_bdev_remove_base_bdev(base_info, NULL, NULL);
		}
	} else {
		rc = 0;
	}

	/* For RAID10: Check if this base bdev needs rebuild after configuration */
	if (raid_bdev->level == RAID10 && raid_bdev->state == RAID_BDEV_STATE_ONLINE && raid_bdev->sb != NULL) {
		/* Safety check: ensure base_bdev_info array is valid before calculating slot */
		if (raid_bdev->base_bdev_info == NULL) {
			SPDK_ERRLOG("raid_bdev->base_bdev_info is NULL for RAID bdev '%s'\n", raid_bdev->bdev.name);
			if (configure_cb != NULL) {
				configure_cb(base_info->configure_cb_ctx, -EINVAL);
				configure_cb = NULL;
			}
			return;
		}
		uint8_t slot_idx = raid_bdev_base_bdev_slot(base_info);

		/* Safety check: ensure slot_idx is within valid range */
		if (slot_idx >= raid_bdev->num_base_bdevs) {
			SPDK_ERRLOG("Calculated slot %u exceeds num_base_bdevs %u for RAID bdev '%s'\n",
				    slot_idx, raid_bdev->num_base_bdevs, raid_bdev->bdev.name);
			if (configure_cb != NULL) {
				configure_cb(base_info->configure_cb_ctx, -EINVAL);
				configure_cb = NULL;
			}
			return;
		}

		bool needs_rebuild = false;
		const char *rebuild_reason = NULL;

		/* Find this slot in superblock */
		const struct raid_bdev_sb_base_bdev *sb_base = raid_bdev_sb_find_base_bdev_by_slot(raid_bdev, slot_idx);

		if (sb_base != NULL) {
			/* Check if slot is already CONFIGURED in RAID bdev's superblock.
			 * If it's CONFIGURED, it means rebuild has already completed, and this
			 * might be an old disk with stale superblock that was re-inserted.
			 * In this case, we should NOT trigger rebuild again.
			 */
			if (sb_base->state == RAID_SB_BASE_BDEV_CONFIGURED) {
				/* Slot is already CONFIGURED - rebuild has completed.
				 * Check if this is the same disk (UUID matches) or a different disk.
				 */
				if (!spdk_uuid_is_null(&sb_base->uuid) &&
				    spdk_uuid_compare(&base_info->uuid, &sb_base->uuid) == 0) {
					/* Same disk - this is the disk that was rebuilt.
					 * No need to rebuild again, just update its superblock if needed.
					 */
					SPDK_NOTICELOG("Base bdev %s (slot %u) is already CONFIGURED in RAID bdev %s superblock. "
						       "This appears to be the same disk that was rebuilt. Skipping rebuild.\n",
						       base_info->name, slot_idx, raid_bdev->bdev.name);
					needs_rebuild = false;
				} else {
					/* Different disk - this is a replacement disk, but slot is already CONFIGURED.
					 * This shouldn't happen in normal flow, but if it does, we should rebuild
					 * to ensure data consistency.
					 */
					SPDK_WARNLOG("Base bdev %s (slot %u) UUID doesn't match expected UUID in RAID bdev %s, "
						     "but slot is CONFIGURED. This might be an old disk re-inserted. "
						     "Will rebuild to ensure data consistency.\n",
						     base_info->name, slot_idx, raid_bdev->bdev.name);
					needs_rebuild = true;
					rebuild_reason = "UUID mismatch with CONFIGURED slot (old disk re-inserted?)";
				}
			} else {
				/* Slot is MISSING or FAILED - check if this is a replacement disk */
				if (sb_base->state == RAID_SB_BASE_BDEV_MISSING ||
				    sb_base->state == RAID_SB_BASE_BDEV_FAILED) {
					/* Check if UUID matches - if it matches, this is the old failed disk re-inserted */
					if (!spdk_uuid_is_null(&sb_base->uuid) &&
					    spdk_uuid_compare(&base_info->uuid, &sb_base->uuid) == 0) {
						/* Same UUID - this is the old failed disk that was repaired and re-inserted.
						 * We should ALWAYS rebuild in this case because:
						 * 1. The disk may have been repaired, but its data may be stale
						 * 2. Even if the system is in degraded mode, we need to rebuild to restore redundancy
						 * 3. Even if another disk was used to rebuild, the original disk's data is outdated
						 */
						needs_rebuild = true;
						rebuild_reason = "old failed disk re-inserted after repair (UUID matches)";
					} else {
						/* Different UUID - this is a replacement disk */
						needs_rebuild = true;
						rebuild_reason = "slot was previously MISSING/FAILED (replacement disk)";
					}
				}

				/* Check if UUID doesn't match expected UUID (wrong disk or new disk) */
				if (!needs_rebuild && !spdk_uuid_is_null(&sb_base->uuid) &&
				    spdk_uuid_compare(&base_info->uuid, &sb_base->uuid) != 0) {
					needs_rebuild = true;
					rebuild_reason = "UUID mismatch (new disk or wrong disk)";
				}

				/* If UUID is null in base_info, this is a new disk without superblock */
				if (!needs_rebuild && spdk_uuid_is_null(&base_info->uuid)) {
					needs_rebuild = true;
					rebuild_reason = "new disk without superblock";
				}
			}
		} else {
			/* Slot not found in superblock - this shouldn't happen, but treat as needs rebuild */
			needs_rebuild = true;
			rebuild_reason = "slot not found in superblock";
		}

		/* Additional check: if base_info->uuid was set from examine but doesn't match
		 * the expected UUID in superblock, we need rebuild */
		if (!needs_rebuild && sb_base != NULL && !spdk_uuid_is_null(&sb_base->uuid)) {
			if (spdk_uuid_compare(&base_info->uuid, &sb_base->uuid) != 0) {
				needs_rebuild = true;
				rebuild_reason = "UUID mismatch with superblock";
			}
		}

		/* If this is a new disk (no superblock), we need to write superblock and rebuild */
		if (!needs_rebuild && spdk_uuid_is_null(&base_info->uuid)) {
			needs_rebuild = true;
			rebuild_reason = "new disk - needs superblock write and rebuild";
		}

		/* Only start rebuild if not already started in the earlier branch (4104-4110) */
		if (needs_rebuild && raid_bdev->process == NULL && !base_info->is_process_target) {
			/* Start rebuild for this base bdev */
			SPDK_NOTICELOG("\n");
			SPDK_NOTICELOG("===========================================================\n");
			SPDK_NOTICELOG("REBUILD STARTING: Base bdev %s (slot %u) on RAID bdev %s\n",
				       base_info->name, slot_idx, raid_bdev->bdev.name);
			SPDK_NOTICELOG("Reason: %s\n",
				       rebuild_reason ? rebuild_reason : "unknown reason");
			SPDK_NOTICELOG("Rebuild will start automatically. Please wait...\n");
			SPDK_NOTICELOG("You can check rebuild progress using: bdev_raid_get_bdevs\n");
			SPDK_NOTICELOG("===========================================================\n");
			SPDK_NOTICELOG("\n");
			
			base_info->is_process_target = true;
			raid_bdev->num_base_bdevs_operational++;
			rc = raid_bdev_start_rebuild(base_info);
			if (rc != 0) {
				SPDK_WARNLOG("Failed to start rebuild for base bdev %s: %s\n",
					     base_info->name, spdk_strerror(-rc));
				/* Don't fail configuration if rebuild start fails, but rollback state */
				base_info->is_process_target = false;
				raid_bdev->num_base_bdevs_operational--;
				/* Note: is_configured remains true as the disk is still configured in the array */
			} else {
				/* Rebuild started successfully - output will be shown in thread_init */
			}
		}
	}

	if (configure_cb != NULL) {
		configure_cb(base_info->configure_cb_ctx, rc);
	}
}

static void raid_bdev_examine_sb(const struct raid_bdev_superblock *sb, struct spdk_bdev *bdev,
				 raid_base_bdev_cb cb_fn, void *cb_ctx);

/* Helper function to find base bdev in superblock by UUID */
static const struct raid_bdev_sb_base_bdev *
raid_bdev_sb_find_base_bdev_by_uuid(const struct raid_bdev_superblock *sb,
				     const struct spdk_uuid *uuid)
{
	uint8_t i;

	for (i = 0; i < sb->base_bdevs_size; i++) {
		const struct raid_bdev_sb_base_bdev *sb_base = &sb->base_bdevs[i];
		if (spdk_uuid_compare(&sb_base->uuid, uuid) == 0) {
			return sb_base;
		}
	}
	return NULL;
}

/* Helper function to find base bdev in superblock by slot */
static const struct raid_bdev_sb_base_bdev *
raid_bdev_sb_find_base_bdev_by_slot(const struct raid_bdev *raid_bdev, uint8_t slot)
{
	const struct raid_bdev_superblock *sb = raid_bdev->sb;
	uint8_t i;

	if (sb == NULL) {
		return NULL;
	}

	for (i = 0; i < sb->base_bdevs_size; i++) {
		const struct raid_bdev_sb_base_bdev *sb_base = &sb->base_bdevs[i];
		if (sb_base->slot == slot) {
			return sb_base;
		}
	}
	return NULL;
}

static void
raid_bdev_configure_base_bdev_check_sb_cb(const struct raid_bdev_superblock *sb, int status,
		void *ctx)
{
	struct raid_base_bdev_info *base_info = ctx;
	struct raid_bdev *raid_bdev = base_info->raid_bdev;
	raid_base_bdev_cb configure_cb = base_info->configure_cb;
	struct spdk_bdev *bdev;

	switch (status) {
	case 0:
		/* valid superblock found */
		base_info->configure_cb = NULL;
		if (spdk_uuid_compare(&raid_bdev->bdev.uuid, &sb->uuid) == 0) {
			/* Superblock UUID matches - for RAID10, continue with configuration directly
			 * Don't call examine_sb because this is manual add, not auto-examine.
			 * Manual add should use configure_cont which has strict validation
			 * (slot occupancy check, rebuild decision, etc.)
			 */
			if (raid_bdev->level == RAID10) {
				bdev = spdk_bdev_desc_get_bdev(base_info->desc);
				/* Set UUID from superblock if not already set */
				if (spdk_uuid_is_null(&base_info->uuid)) {
					const struct raid_bdev_sb_base_bdev *sb_base = raid_bdev_sb_find_base_bdev_by_uuid(sb, spdk_bdev_get_uuid(bdev));
					if (sb_base != NULL) {
						spdk_uuid_copy(&base_info->uuid, &sb_base->uuid);
					}
				}
				/* Continue with configuration using configure_cont for strict validation
				 * Note: Do NOT call raid_bdev_free_base_bdev_resource here because
				 * raid_bdev_configure_base_bdev_cont needs base_info->name, base_info->desc,
				 * and other resources. The resource cleanup will be handled by configure_cont
				 * or its error paths if needed.
				 */
				raid_bdev_configure_base_bdev_cont(base_info);
				return;
			} else {
				/* For other RAID levels, use existing logic */
				bdev = spdk_bdev_desc_get_bdev(base_info->desc);
				raid_bdev_free_base_bdev_resource(base_info);
				raid_bdev_examine_sb(sb, bdev, configure_cb, base_info->configure_cb_ctx);
				return;
			}
		}
		/* Superblock UUID mismatch - this disk belongs to a different RAID group */
		if (raid_bdev->level == RAID10) {
			/* Enhanced error message for RAID10 (scenario 5) */
			char target_uuid_str[SPDK_UUID_STRING_LEN];
			char found_uuid_str[SPDK_UUID_STRING_LEN];

			spdk_uuid_fmt_lower(target_uuid_str, sizeof(target_uuid_str),
					    &raid_bdev->bdev.uuid);
			spdk_uuid_fmt_lower(found_uuid_str, sizeof(found_uuid_str), &sb->uuid);

			SPDK_ERRLOG("===========================================================\n");
			SPDK_ERRLOG("ERROR: Base bdev '%s' has a superblock from a different RAID group.\n",
				    base_info->name);
			SPDK_ERRLOG("Target RAID bdev UUID: %s\n", target_uuid_str);
			SPDK_ERRLOG("Found superblock UUID: %s (RAID bdev: %s)\n",
				    found_uuid_str, sb->name);
			SPDK_ERRLOG("This disk cannot be added to RAID bdev '%s'.\n",
				    raid_bdev->bdev.name);
			SPDK_ERRLOG("ACTION REQUIRED: Please clear the superblock on this disk\n");
			SPDK_ERRLOG("using RPC 'bdev_wipe_superblock' before adding it to the RAID group.\n");
			SPDK_ERRLOG("Example: bdev_wipe_superblock -b %s\n", base_info->name);
			SPDK_ERRLOG("===========================================================\n");
		} else {
			SPDK_ERRLOG("Superblock of a different raid bdev found on bdev %s\n", base_info->name);
		}
		/* Use -EINVAL instead of -EEXIST for UUID mismatch, as this is an invalid argument */
		status = -EINVAL;
		raid_bdev_free_base_bdev_resource(base_info);
		break;
	case -EINVAL:
		/* no valid superblock */
		raid_bdev_configure_base_bdev_cont(base_info);
		return;
	default:
		SPDK_ERRLOG("Failed to examine bdev %s: %s\n",
			    base_info->name, spdk_strerror(-status));
		break;
	}

	if (configure_cb != NULL) {
		base_info->configure_cb = NULL;
		configure_cb(base_info->configure_cb_ctx, status);
	}
}

static int
raid_bdev_configure_base_bdev(struct raid_base_bdev_info *base_info, bool existing,
			      raid_base_bdev_cb cb_fn, void *cb_ctx)
{
	struct raid_bdev *raid_bdev = base_info->raid_bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	const struct spdk_uuid *bdev_uuid;
	int rc;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());
	assert(base_info->desc == NULL);

	/*
	 * Base bdev can be added by name or uuid. Here we assure both properties are set and valid
	 * before claiming the bdev.
	 */

	if (!spdk_uuid_is_null(&base_info->uuid)) {
		char uuid_str[SPDK_UUID_STRING_LEN];
		const char *bdev_name;

		spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &base_info->uuid);

		/* UUID of a bdev is registered as its alias */
		bdev = spdk_bdev_get_by_name(uuid_str);
		if (bdev == NULL) {
			return -ENODEV;
		}

		bdev_name = spdk_bdev_get_name(bdev);

		if (base_info->name == NULL) {
			assert(existing == true);
			base_info->name = strdup(bdev_name);
			if (base_info->name == NULL) {
				return -ENOMEM;
			}
		} else if (strcmp(base_info->name, bdev_name) != 0) {
			SPDK_ERRLOG("Name mismatch for base bdev '%s' - expected '%s'\n",
				    bdev_name, base_info->name);
			return -EINVAL;
		}
	}

	assert(base_info->name != NULL);

	rc = spdk_bdev_open_ext(base_info->name, true, raid_bdev_event_base_bdev, NULL, &desc);
	if (rc != 0) {
		if (rc != -ENODEV) {
			SPDK_ERRLOG("Unable to create desc on bdev '%s'\n", base_info->name);
		}
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);
	bdev_uuid = spdk_bdev_get_uuid(bdev);

	if (spdk_uuid_is_null(&base_info->uuid)) {
		spdk_uuid_copy(&base_info->uuid, bdev_uuid);
	} else if (spdk_uuid_compare(&base_info->uuid, bdev_uuid) != 0) {
		SPDK_ERRLOG("UUID mismatch for base bdev '%s'\n", base_info->name);
		spdk_bdev_close(desc);
		return -EINVAL;
	}

	rc = spdk_bdev_module_claim_bdev(bdev, NULL, &g_raid_if);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to claim this bdev as it is already claimed\n");
		spdk_bdev_close(desc);
		return rc;
	}

	SPDK_DEBUGLOG(bdev_raid, "bdev %s is claimed\n", bdev->name);

	base_info->app_thread_ch = spdk_bdev_get_io_channel(desc);
	if (base_info->app_thread_ch == NULL) {
		SPDK_ERRLOG("Failed to get io channel\n");
		spdk_bdev_module_release_bdev(bdev);
		spdk_bdev_close(desc);
		return -ENOMEM;
	}

	base_info->desc = desc;
	base_info->blockcnt = bdev->blockcnt;

	if (raid_bdev->superblock_enabled) {
		uint64_t data_offset;

		if (base_info->data_offset == 0) {
			assert((RAID_BDEV_MIN_DATA_OFFSET_SIZE % spdk_bdev_get_data_block_size(bdev)) == 0);
			data_offset = RAID_BDEV_MIN_DATA_OFFSET_SIZE / spdk_bdev_get_data_block_size(bdev);
		} else {
			data_offset = base_info->data_offset;
		}

		if (bdev->optimal_io_boundary != 0) {
			data_offset = spdk_round_up(data_offset, bdev->optimal_io_boundary);
			if (base_info->data_offset != 0 && base_info->data_offset != data_offset) {
				SPDK_WARNLOG("Data offset %lu on bdev '%s' is different than optimal value %lu\n",
					     base_info->data_offset, base_info->name, data_offset);
				data_offset = base_info->data_offset;
			}
		}

		base_info->data_offset = data_offset;
	}

	if (base_info->data_offset >= bdev->blockcnt) {
		SPDK_ERRLOG("Data offset %lu exceeds base bdev capacity %lu on bdev '%s'\n",
			    base_info->data_offset, bdev->blockcnt, base_info->name);
		rc = -EINVAL;
		goto out;
	}

	if (base_info->data_size == 0) {
		base_info->data_size = bdev->blockcnt - base_info->data_offset;
	} else if (base_info->data_offset + base_info->data_size > bdev->blockcnt) {
		SPDK_ERRLOG("Data offset and size exceeds base bdev capacity %lu on bdev '%s'\n",
			    bdev->blockcnt, base_info->name);
		rc = -EINVAL;
		goto out;
	}

	if (!raid_bdev->module->dif_supported && spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) {
		SPDK_ERRLOG("Base bdev '%s' has DIF or DIX enabled - unsupported RAID configuration\n",
			    bdev->name);
		rc = -EINVAL;
		goto out;
	}

	/*
	 * Set the raid bdev properties if this is the first base bdev configured,
	 * otherwise - verify. Assumption is that all the base bdevs for any raid bdev should
	 * have the same blocklen and metadata format.
	 */
	if (raid_bdev->bdev.blocklen == 0) {
		raid_bdev->bdev.blocklen = bdev->blocklen;
		raid_bdev->bdev.md_len = spdk_bdev_get_md_size(bdev);
		raid_bdev->bdev.md_interleave = spdk_bdev_is_md_interleaved(bdev);
		raid_bdev->bdev.dif_type = spdk_bdev_get_dif_type(bdev);
		raid_bdev->bdev.dif_check_flags = bdev->dif_check_flags;
		raid_bdev->bdev.dif_is_head_of_md = spdk_bdev_is_dif_head_of_md(bdev);
		raid_bdev->bdev.dif_pi_format = bdev->dif_pi_format;
	} else {
		if (raid_bdev->bdev.blocklen != bdev->blocklen) {
			SPDK_ERRLOG("Raid bdev '%s' blocklen %u differs from base bdev '%s' blocklen %u\n",
				    raid_bdev->bdev.name, raid_bdev->bdev.blocklen, bdev->name, bdev->blocklen);
			rc = -EINVAL;
			goto out;
		}

		if (raid_bdev->bdev.md_len != spdk_bdev_get_md_size(bdev) ||
		    raid_bdev->bdev.md_interleave != spdk_bdev_is_md_interleaved(bdev) ||
		    raid_bdev->bdev.dif_type != spdk_bdev_get_dif_type(bdev) ||
		    raid_bdev->bdev.dif_check_flags != bdev->dif_check_flags ||
		    raid_bdev->bdev.dif_is_head_of_md != spdk_bdev_is_dif_head_of_md(bdev) ||
		    raid_bdev->bdev.dif_pi_format != bdev->dif_pi_format) {
			SPDK_ERRLOG("Raid bdev '%s' has different metadata format than base bdev '%s'\n",
				    raid_bdev->bdev.name, bdev->name);
			rc = -EINVAL;
			goto out;
		}
	}

	assert(base_info->configure_cb == NULL);
	base_info->configure_cb = cb_fn;
	base_info->configure_cb_ctx = cb_ctx;

	if (existing) {
		raid_bdev_configure_base_bdev_cont(base_info);
	} else {
		/* check for existing superblock when using a new bdev */
		rc = raid_bdev_load_base_bdev_superblock(desc, base_info->app_thread_ch,
				raid_bdev_configure_base_bdev_check_sb_cb, base_info);
		if (rc) {
			SPDK_ERRLOG("Failed to read bdev %s superblock: %s\n",
				    bdev->name, spdk_strerror(-rc));
		}
	}
out:
	if (rc != 0) {
		base_info->configure_cb = NULL;
		raid_bdev_free_base_bdev_resource(base_info);
	}
	return rc;
}

int
raid_bdev_add_base_bdev(struct raid_bdev *raid_bdev, const char *name,
			raid_base_bdev_cb cb_fn, void *cb_ctx)
{
	struct raid_base_bdev_info *base_info = NULL, *iter;
	int rc;

	assert(name != NULL);
	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	if (raid_bdev_get_active_process(raid_bdev) != NULL) {
		SPDK_ERRLOG("raid bdev '%s' is in process\n",
			    raid_bdev->bdev.name);
		return -EPERM;
	}

	if (raid_bdev->state == RAID_BDEV_STATE_CONFIGURING) {
		struct spdk_bdev *bdev = spdk_bdev_get_by_name(name);

		if (bdev != NULL) {
			RAID_FOR_EACH_BASE_BDEV(raid_bdev, iter) {
				if (iter->name == NULL &&
				    spdk_uuid_compare(&bdev->uuid, &iter->uuid) == 0) {
					base_info = iter;
					break;
				}
			}
		}
	}

	if (base_info == NULL || raid_bdev->state == RAID_BDEV_STATE_ONLINE) {
		RAID_FOR_EACH_BASE_BDEV(raid_bdev, iter) {
			if (iter->name == NULL && spdk_uuid_is_null(&iter->uuid)) {
				base_info = iter;
				break;
			}
		}
	}

	if (base_info == NULL) {
		SPDK_ERRLOG("no empty slot found in raid bdev '%s' for new base bdev '%s'\n",
			    raid_bdev->bdev.name, name);
		return -EINVAL;
	}

	/* Safety check: ensure base_info has valid raid_bdev pointer */
	if (base_info->raid_bdev == NULL) {
		SPDK_ERRLOG("base_info->raid_bdev is NULL for slot in raid bdev '%s'\n",
			    raid_bdev->bdev.name);
		return -EINVAL;
	}

	/* Safety check: ensure base_info belongs to the correct raid_bdev */
	if (base_info->raid_bdev != raid_bdev) {
		SPDK_ERRLOG("base_info belongs to different raid_bdev '%s' (expected '%s')\n",
			    base_info->raid_bdev->bdev.name, raid_bdev->bdev.name);
		return -EINVAL;
	}

	assert(base_info->is_configured == false);

	if (raid_bdev->state == RAID_BDEV_STATE_ONLINE) {
		assert(base_info->data_size != 0);
		assert(base_info->desc == NULL);
	}

	base_info->name = strdup(name);
	if (base_info->name == NULL) {
		return -ENOMEM;
	}

	rc = raid_bdev_configure_base_bdev(base_info, false, cb_fn, cb_ctx);
	if (rc != 0 && (rc != -ENODEV || raid_bdev->state != RAID_BDEV_STATE_CONFIGURING)) {
		SPDK_ERRLOG("base bdev '%s' configure failed: %s\n", name, spdk_strerror(-rc));
		free(base_info->name);
		base_info->name = NULL;
	}

	return rc;
}

static int
raid_bdev_create_from_sb(const struct raid_bdev_superblock *sb, struct raid_bdev **raid_bdev_out)
{
	struct raid_bdev *raid_bdev;
	uint8_t i;
	int rc;

	rc = _raid_bdev_create(sb->name, (sb->strip_size * sb->block_size) / 1024, sb->num_base_bdevs,
			       sb->level, true, &sb->uuid, &raid_bdev);
	if (rc != 0) {
		return rc;
	}

	rc = raid_bdev_alloc_superblock(raid_bdev, sb->block_size);
	if (rc != 0) {
		raid_bdev_free(raid_bdev);
		return rc;
	}

	assert(sb->length <= RAID_BDEV_SB_MAX_LENGTH);
	memcpy(raid_bdev->sb, sb, sb->length);

	for (i = 0; i < sb->base_bdevs_size; i++) {
		const struct raid_bdev_sb_base_bdev *sb_base_bdev = &sb->base_bdevs[i];
		struct raid_base_bdev_info *base_info = &raid_bdev->base_bdev_info[sb_base_bdev->slot];

		if (sb_base_bdev->state == RAID_SB_BASE_BDEV_CONFIGURED) {
			spdk_uuid_copy(&base_info->uuid, &sb_base_bdev->uuid);
			raid_bdev->num_base_bdevs_operational++;
		}

		base_info->data_offset = sb_base_bdev->data_offset;
		base_info->data_size = sb_base_bdev->data_size;
	}

	*raid_bdev_out = raid_bdev;
	return 0;
}

static void
raid_bdev_examine_no_sb(struct spdk_bdev *bdev)
{
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;

	TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
		if (raid_bdev->state != RAID_BDEV_STATE_CONFIGURING || raid_bdev->sb != NULL) {
			continue;
		}
		RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
			if (base_info->desc == NULL &&
			    ((base_info->name != NULL && strcmp(bdev->name, base_info->name) == 0) ||
			     spdk_uuid_compare(&base_info->uuid, &bdev->uuid) == 0)) {
				raid_bdev_configure_base_bdev(base_info, true, NULL, NULL);
				break;
			}
		}
	}
}

struct raid_bdev_examine_others_ctx {
	struct spdk_uuid raid_bdev_uuid;
	uint8_t current_base_bdev_idx;
	raid_base_bdev_cb cb_fn;
	void *cb_ctx;
};

static void
raid_bdev_examine_others_done(void *_ctx, int status)
{
	struct raid_bdev_examine_others_ctx *ctx = _ctx;

	if (ctx->cb_fn != NULL) {
		ctx->cb_fn(ctx->cb_ctx, status);
	}
	free(ctx);
}

typedef void (*raid_bdev_examine_load_sb_cb)(struct spdk_bdev *bdev,
		const struct raid_bdev_superblock *sb, int status, void *ctx);
static int raid_bdev_examine_load_sb(const char *bdev_name, raid_bdev_examine_load_sb_cb cb,
				     void *cb_ctx);
static void raid_bdev_examine_sb(const struct raid_bdev_superblock *sb, struct spdk_bdev *bdev,
				 raid_base_bdev_cb cb_fn, void *cb_ctx);
static void raid_bdev_examine_others(void *_ctx, int status);

static void
raid_bdev_examine_others_load_cb(struct spdk_bdev *bdev, const struct raid_bdev_superblock *sb,
				 int status, void *_ctx)
{
	struct raid_bdev_examine_others_ctx *ctx = _ctx;

	if (status != 0) {
		raid_bdev_examine_others_done(ctx, status);
		return;
	}

	raid_bdev_examine_sb(sb, bdev, raid_bdev_examine_others, ctx);
}

static void
raid_bdev_examine_others(void *_ctx, int status)
{
	struct raid_bdev_examine_others_ctx *ctx = _ctx;
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;
	char uuid_str[SPDK_UUID_STRING_LEN];

	if (status != 0 && status != -EEXIST) {
		goto out;
	}

	raid_bdev = raid_bdev_find_by_uuid(&ctx->raid_bdev_uuid);
	if (raid_bdev == NULL) {
		status = -ENODEV;
		goto out;
	}

	for (base_info = &raid_bdev->base_bdev_info[ctx->current_base_bdev_idx];
	     base_info < &raid_bdev->base_bdev_info[raid_bdev->num_base_bdevs];
	     base_info++) {
		if (base_info->is_configured || spdk_uuid_is_null(&base_info->uuid)) {
			continue;
		}

		spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &base_info->uuid);

		if (spdk_bdev_get_by_name(uuid_str) == NULL) {
			continue;
		}

		ctx->current_base_bdev_idx = raid_bdev_base_bdev_slot(base_info);

		status = raid_bdev_examine_load_sb(uuid_str, raid_bdev_examine_others_load_cb, ctx);
		if (status != 0) {
			continue;
		}
		return;
	}
out:
	raid_bdev_examine_others_done(ctx, status);
}

static void
raid_bdev_examine_sb(const struct raid_bdev_superblock *sb, struct spdk_bdev *bdev,
		     raid_base_bdev_cb cb_fn, void *cb_ctx)
{
	const struct raid_bdev_sb_base_bdev *sb_base_bdev = NULL;
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *iter, *base_info;
	uint8_t i;
	int rc;

	if (sb->block_size != spdk_bdev_get_data_block_size(bdev)) {
		SPDK_WARNLOG("Bdev %s block size (%u) does not match the value in superblock (%u)\n",
			     bdev->name, sb->block_size, spdk_bdev_get_data_block_size(bdev));
		rc = -EINVAL;
		goto out;
	}

	if (spdk_uuid_is_null(&sb->uuid)) {
		SPDK_WARNLOG("NULL raid bdev UUID in superblock on bdev %s\n", bdev->name);
		rc = -EINVAL;
		goto out;
	}

	raid_bdev = raid_bdev_find_by_uuid(&sb->uuid);

	if (raid_bdev) {
		if (raid_bdev->sb == NULL) {
			SPDK_WARNLOG("raid superblock is null\n");
			rc = -EINVAL;
			goto out;
		}

		if (sb->seq_number > raid_bdev->sb->seq_number) {
			SPDK_DEBUGLOG(bdev_raid,
				      "raid superblock seq_number on bdev %s (%lu) greater than existing raid bdev %s (%lu)\n",
				      bdev->name, sb->seq_number, raid_bdev->bdev.name, raid_bdev->sb->seq_number);

			if (raid_bdev->state != RAID_BDEV_STATE_CONFIGURING) {
				SPDK_WARNLOG("Newer version of raid bdev %s superblock found on bdev %s but raid bdev is not in configuring state.\n",
					     raid_bdev->bdev.name, bdev->name);
				rc = -EBUSY;
				goto out;
			}

			/* remove and then recreate the raid bdev using the newer superblock */
			raid_bdev_delete(raid_bdev, NULL, NULL);
			raid_bdev = NULL;
		} else if (sb->seq_number < raid_bdev->sb->seq_number) {
			SPDK_DEBUGLOG(bdev_raid,
				      "raid superblock seq_number on bdev %s (%lu) smaller than existing raid bdev %s (%lu)\n",
				      bdev->name, sb->seq_number, raid_bdev->bdev.name, raid_bdev->sb->seq_number);
			/* use the current raid bdev superblock */
			sb = raid_bdev->sb;
		}
	}

	for (i = 0; i < sb->base_bdevs_size; i++) {
		sb_base_bdev = &sb->base_bdevs[i];

		assert(spdk_uuid_is_null(&sb_base_bdev->uuid) == false);

		if (spdk_uuid_compare(&sb_base_bdev->uuid, spdk_bdev_get_uuid(bdev)) == 0) {
			break;
		}
	}

	if (i == sb->base_bdevs_size) {
		SPDK_DEBUGLOG(bdev_raid, "raid superblock does not contain this bdev's uuid\n");
		rc = -EINVAL;
		goto out;
	}

	if (!raid_bdev) {
		struct raid_bdev_examine_others_ctx *ctx;

		ctx = calloc(1, sizeof(*ctx));
		if (ctx == NULL) {
			rc = -ENOMEM;
			goto out;
		}

		rc = raid_bdev_create_from_sb(sb, &raid_bdev);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to create raid bdev %s: %s\n",
				    sb->name, spdk_strerror(-rc));
			free(ctx);
			goto out;
		}

		/* after this base bdev is configured, examine other base bdevs that may be present */
		spdk_uuid_copy(&ctx->raid_bdev_uuid, &sb->uuid);
		ctx->cb_fn = cb_fn;
		ctx->cb_ctx = cb_ctx;

		cb_fn = raid_bdev_examine_others;
		cb_ctx = ctx;
	}

	if (raid_bdev->state == RAID_BDEV_STATE_ONLINE) {
		assert(sb_base_bdev->slot < raid_bdev->num_base_bdevs);
		base_info = &raid_bdev->base_bdev_info[sb_base_bdev->slot];

		/* For RAID10: Check if this slot is already occupied by another disk
		 * This is a safety check in examine path: if slot is already occupied,
		 * we don't auto-replace it to avoid accidental data loss.
		 * User should manually remove the existing disk first if replacement is intended.
		 * Note: We only check is_configured, not name, because name may be set
		 * during configuration but is_configured is the definitive flag.
		 */
		if (raid_bdev->level == RAID10) {
			if (base_info->is_configured) {
				/* RAID bdev is already fully configured, this disk cannot be added */
				/* This can happen if superblock state is CONFIGURED but slot is occupied */
				SPDK_NOTICELOG("===========================================================\n");
				SPDK_NOTICELOG("NVMe bdev '%s' belongs to RAID bdev '%s' (slot %u), but RAID bdev is already fully configured.\n",
					       bdev->name, raid_bdev->bdev.name, sb_base_bdev->slot);
				SPDK_NOTICELOG("This disk cannot be automatically added to the RAID bdev as the slot is already occupied.\n");
				SPDK_NOTICELOG("If you want to replace the existing disk, please:\n");
				SPDK_NOTICELOG("1. Remove the existing disk using RPC 'bdev_raid_remove_base_bdev'\n");
				SPDK_NOTICELOG("2. Then this disk will be automatically added on next examine\n");
				SPDK_NOTICELOG("If you don't need this disk for the RAID bdev, please clear its superblock\n");
				SPDK_NOTICELOG("using RPC 'bdev_wipe_superblock' to make it available for other use.\n");
				SPDK_NOTICELOG("Example: bdev_wipe_superblock -b %s\n", bdev->name);
				SPDK_NOTICELOG("===========================================================\n");
				rc = 0; /* Don't treat as error, just inform user */
				goto out;
			}
		}

		/* Slot is not occupied, check superblock state
		 * This is the auto-recovery path: if slot is FAILED/MISSING/REBUILDING,
		 * we automatically configure and rebuild the disk.
		 */
		if (sb_base_bdev->state == RAID_SB_BASE_BDEV_MISSING ||
		    sb_base_bdev->state == RAID_SB_BASE_BDEV_FAILED ||
		    sb_base_bdev->state == RAID_SB_BASE_BDEV_REBUILDING) {
			/* Auto-recovering a MISSING, FAILED, or REBUILDING disk */
			assert(base_info->is_configured == false);
			assert(spdk_uuid_is_null(&base_info->uuid));
			spdk_uuid_copy(&base_info->uuid, &sb_base_bdev->uuid);
			if (sb_base_bdev->state == RAID_SB_BASE_BDEV_REBUILDING) {
				SPDK_NOTICELOG("Auto-recovering bdev %s to RAID bdev %s (previous rebuild was not completed).\n",
					       bdev->name, raid_bdev->bdev.name);
			} else {
				SPDK_NOTICELOG("Auto-recovering bdev %s to RAID bdev %s (slot was %s).\n",
					       bdev->name, raid_bdev->bdev.name,
					       sb_base_bdev->state == RAID_SB_BASE_BDEV_FAILED ? "FAILED" : "MISSING");
			}
			/* Use existing=true to indicate this is from examine (auto-recovery) */
			rc = raid_bdev_configure_base_bdev(base_info, true, cb_fn, cb_ctx);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to configure bdev %s as base bdev of raid %s: %s\n",
					    bdev->name, raid_bdev->bdev.name, spdk_strerror(-rc));
				goto out;
			}
			/* If rc == 0, raid_bdev_configure_base_bdev will call the callback asynchronously */
			return;
		} else if (sb_base_bdev->state == RAID_SB_BASE_BDEV_CONFIGURED) {
			/* Superblock says CONFIGURED, but slot is not occupied - this is unexpected.
			 * Continue to normal processing path below to handle it.
			 */
		} else {
			/* Unknown state */
			SPDK_WARNLOG("Unknown superblock state %u for bdev %s in RAID bdev %s\n",
				     sb_base_bdev->state, bdev->name, raid_bdev->bdev.name);
			rc = -EINVAL;
			goto out;
		}
	}

	if (sb_base_bdev->state != RAID_SB_BASE_BDEV_CONFIGURED) {
		SPDK_NOTICELOG("Bdev %s is not an active member of raid bdev %s. Ignoring.\n",
			       bdev->name, raid_bdev->bdev.name);
		rc = -EINVAL;
		goto out;
	}

	base_info = NULL;
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, iter) {
		if (spdk_uuid_compare(&iter->uuid, spdk_bdev_get_uuid(bdev)) == 0) {
			base_info = iter;
			break;
		}
	}

	if (base_info == NULL) {
		SPDK_ERRLOG("Bdev %s is not a member of raid bdev %s\n",
			    bdev->name, raid_bdev->bdev.name);
		rc = -EINVAL;
		goto out;
	}

	if (base_info->is_configured) {
		SPDK_DEBUGLOG(bdev_raid, "Base bdev %s already configured for raid bdev %s\n",
			      bdev->name, raid_bdev->bdev.name);
		rc = 0;
		if (cb_fn != 0) {
			cb_fn(cb_ctx, rc);
		}
		return;
	}

	rc = raid_bdev_configure_base_bdev(base_info, true, cb_fn, cb_ctx);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to configure bdev %s as base bdev of raid %s: %s\n",
			    bdev->name, raid_bdev->bdev.name, spdk_strerror(-rc));
		goto out;
	}
	/* If rc == 0, raid_bdev_configure_base_bdev will call the callback asynchronously */
	return;
out:
	if (rc != 0 && cb_fn != 0) {
		cb_fn(cb_ctx, rc);
	}
}

struct raid_bdev_examine_ctx {
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	raid_bdev_examine_load_sb_cb cb;
	void *cb_ctx;
};

static void
raid_bdev_examine_ctx_free(struct raid_bdev_examine_ctx *ctx)
{
	if (!ctx) {
		return;
	}

	if (ctx->ch) {
		spdk_put_io_channel(ctx->ch);
	}

	if (ctx->desc) {
		spdk_bdev_close(ctx->desc);
	}

	free(ctx);
}

static void
raid_bdev_examine_load_sb_done(const struct raid_bdev_superblock *sb, int status, void *_ctx)
{
	struct raid_bdev_examine_ctx *ctx = _ctx;
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(ctx->desc);

	ctx->cb(bdev, sb, status, ctx->cb_ctx);

	raid_bdev_examine_ctx_free(ctx);
}

static void
raid_bdev_examine_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
}

static int
raid_bdev_examine_load_sb(const char *bdev_name, raid_bdev_examine_load_sb_cb cb, void *cb_ctx)
{
	struct raid_bdev_examine_ctx *ctx;
	int rc;

	assert(cb != NULL);

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	rc = spdk_bdev_open_ext(bdev_name, false, raid_bdev_examine_event_cb, NULL, &ctx->desc);
	if (rc) {
		SPDK_ERRLOG("Failed to open bdev %s: %s\n", bdev_name, spdk_strerror(-rc));
		goto err;
	}

	ctx->ch = spdk_bdev_get_io_channel(ctx->desc);
	if (!ctx->ch) {
		SPDK_ERRLOG("Failed to get io channel for bdev %s\n", bdev_name);
		rc = -ENOMEM;
		goto err;
	}

	ctx->cb = cb;
	ctx->cb_ctx = cb_ctx;

	rc = raid_bdev_load_base_bdev_superblock(ctx->desc, ctx->ch, raid_bdev_examine_load_sb_done, ctx);
	if (rc) {
		SPDK_ERRLOG("Failed to read bdev %s superblock: %s\n",
			    bdev_name, spdk_strerror(-rc));
		goto err;
	}

	return 0;
err:
	raid_bdev_examine_ctx_free(ctx);
	return rc;
}

static void
raid_bdev_examine_done(void *ctx, int status)
{
	struct spdk_bdev *bdev = ctx;

	if (status != 0) {
		SPDK_ERRLOG("Failed to examine bdev %s: %s\n",
			    bdev->name, spdk_strerror(-status));
	}
	spdk_bdev_module_examine_done(&g_raid_if);
}

static void
raid_bdev_examine_cont(struct spdk_bdev *bdev, const struct raid_bdev_superblock *sb, int status,
		       void *ctx)
{
	switch (status) {
	case 0:
		/* valid superblock found */
		SPDK_DEBUGLOG(bdev_raid, "raid superblock found on bdev %s\n", bdev->name);
		raid_bdev_examine_sb(sb, bdev, raid_bdev_examine_done, bdev);
		return;
	case -EINVAL:
		/* no valid superblock, check if it can be claimed anyway */
		raid_bdev_examine_no_sb(bdev);
		status = 0;
		break;
	}

	raid_bdev_examine_done(bdev, status);
}

/*
 * brief:
 * raid_bdev_examine function is the examine function call by the below layers
 * like bdev_nvme layer. This function will check if this base bdev can be
 * claimed by this raid bdev or not.
 * params:
 * bdev - pointer to base bdev
 * returns:
 * none
 */
static void
raid_bdev_examine(struct spdk_bdev *bdev)
{
	int rc = 0;

	if (raid_bdev_find_base_info_by_bdev(bdev) != NULL) {
		goto done;
	}

	if (spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) {
		raid_bdev_examine_no_sb(bdev);
		goto done;
	}

	rc = raid_bdev_examine_load_sb(bdev->name, raid_bdev_examine_cont, NULL);
	if (rc != 0) {
		goto done;
	}

	return;
done:
	raid_bdev_examine_done(bdev, rc);
}

/*
 * Check if rebuild is in progress
 */
bool
raid_bdev_is_rebuilding(struct raid_bdev *raid_bdev)
{
	return raid_bdev_process_is_running_type(raid_bdev, RAID_PROCESS_REBUILD);
}

/*
 * Get rebuild progress
 */
int
raid_bdev_get_rebuild_progress(struct raid_bdev *raid_bdev, uint64_t *current_offset,
			       uint64_t *total_size)
{
	struct raid_bdev_process *process;

	if (raid_bdev == NULL || current_offset == NULL || total_size == NULL) {
		return -EINVAL;
	}

	process = raid_bdev_get_active_process(raid_bdev);
	if (process == NULL || process->type != RAID_PROCESS_REBUILD) {
		return -ENODEV;
	}

	*current_offset = process->window_offset;
	*total_size = raid_bdev->bdev.blockcnt;

	return 0;
}

/* Log component for bdev raid bdev module */
SPDK_LOG_REGISTER_COMPONENT(bdev_raid)

static void
bdev_raid_trace(void)
{
	struct spdk_trace_tpoint_opts opts[] = {
		{
			"BDEV_RAID_IO_START", TRACE_BDEV_RAID_IO_START,
			OWNER_TYPE_NONE, OBJECT_BDEV_RAID_IO, 1,
			{{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 }}
		},
		{
			"BDEV_RAID_IO_DONE", TRACE_BDEV_RAID_IO_DONE,
			OWNER_TYPE_NONE, OBJECT_BDEV_RAID_IO, 0,
			{{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 }}
		}
	};


	spdk_trace_register_object(OBJECT_BDEV_RAID_IO, 'R');
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));
	spdk_trace_tpoint_register_relation(TRACE_BDEV_IO_START, OBJECT_BDEV_RAID_IO, 1);
	spdk_trace_tpoint_register_relation(TRACE_BDEV_IO_DONE, OBJECT_BDEV_RAID_IO, 0);
}
SPDK_TRACE_REGISTER_FN(bdev_raid_trace, "bdev_raid", TRACE_GROUP_BDEV_RAID)
