/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_EC_INTERNAL_H
#define SPDK_BDEV_EC_INTERNAL_H

#include "spdk/stdinc.h"

/* Global configuration: Enable/disable EC encoding dedicated worker threads
 * This can be set by the application (e.g., via command line argument)
 * Default: true (enabled)
 */
extern bool g_ec_encode_workers_enabled;

#include "bdev_ec.h"
#include "spdk/thread.h"
#include "spdk/vmd.h"

/* Prefetch support - use compiler builtin if available, otherwise use inline asm */
#ifdef __SSE__
#include <emmintrin.h>
#define EC_PREFETCH(addr, hint) _mm_prefetch((const char *)(addr), (hint))
#elif defined(__GNUC__) || defined(__clang__)
#define EC_PREFETCH(addr, hint) __builtin_prefetch((addr), 0, (hint))
#else
/* No prefetch support - define as no-op */
#define EC_PREFETCH(addr, hint) do { (void)(addr); (void)(hint); } while (0)
#endif

/* ISA-L optimal alignment for SIMD operations */
#define EC_ISAL_OPTIMAL_ALIGN	64

/* RAID5F-style object pool size - pre-allocate stripe_private objects
 * Increased from 32 to 64 for better performance with sufficient memory
 * Each object is ~1-2KB, so 64 objects = ~64-128KB per io_channel
 */
#define EC_MAX_STRIPES 64

/* RAID5F-style: Always use async encoding (no threshold)
 * 
 * RAID5F uses accel framework which always uses async operations regardless of size.
 * We follow the same approach for consistency and maximum throughput.
 * 
 * Benefits:
 *   - Main thread never blocks - better concurrency
 *   - Consistent behavior - no size-based decisions
 *   - Handles high concurrency naturally
 * 
 * Note: Thread switch overhead is acceptable compared to I/O latency and
 *       the throughput benefits from non-blocking main thread.
 */

/* Type identifier for module_private structures */
enum ec_private_type {
	EC_PRIVATE_TYPE_FULL_STRIPE,
	EC_PRIVATE_TYPE_RMW,
	EC_PRIVATE_TYPE_DECODE
};

/* Chunk structure for iovec management (borrowed from RAID5F design)
 * Each data/parity block has its own chunk with mapped iovecs
 * This enables zero-copy writes using writev_blocks_ext
 */
struct ec_chunk {
	/* Corresponds to base_bdev index */
	uint8_t index;
	
	/* Array of iovecs mapped from original iovs */
	struct iovec *iovs;
	
	/* Number of used iovecs */
	int iovcnt;
	
	/* Total number of available iovecs in the array */
	int iovcnt_max;
	
	/* Pointer to buffer with I/O metadata (if any) */
	void *md_buf;
	
	/* For encoding: pointer to contiguous buffer (if data spans multiple iovs) */
	unsigned char *encode_buf;  /* NULL if data is in single iov */
};

/* Private structure for stripe write operations */
struct ec_stripe_private {
	enum ec_private_type type;
	unsigned char *parity_bufs[EC_MAX_P];
	uint8_t num_parity;
	/* Temporary buffers for cross-iov data (only used when need_temp_bufs is true) */
	unsigned char *temp_data_bufs[EC_MAX_K];
	uint8_t num_temp_bufs;  /* Number of temp buffers allocated (0 if not needed) */
	/* Base bdev indices (for snapshot storage) */
	uint64_t stripe_index;
	uint8_t data_indices[EC_MAX_K];
	uint8_t parity_indices[EC_MAX_P];
	
	/* RAID5F-style chunk management for zero-copy writes */
	struct ec_chunk data_chunks[EC_MAX_K];  /* Chunks for data blocks */
	struct ec_chunk parity_chunks[EC_MAX_P];  /* Chunks for parity blocks */
	
	/* RAID5F-style object pool link */
	TAILQ_ENTRY(ec_stripe_private) link;
	
	/* Flag to track if this object came from pool (chunk iovs already allocated) */
	bool from_pool;
	
	/* RAID5F-style async encoding state */
	struct {
		int status;  /* Encoding status: 0=success, negative=error, -ECANCELED=cancelled */
		void (*cb)(struct ec_stripe_private *stripe_priv, int status);  /* Callback when encoding completes */
		unsigned char **data_ptrs;  /* Data pointers for encoding */
		unsigned char **parity_ptrs;  /* Parity pointers for encoding */
		size_t len;  /* Encoding length */
		struct ec_bdev *ec_bdev;  /* EC bdev for encoding */
		struct ec_bdev_io *ec_io;  /* EC I/O for encoding */
		bool cancelled;  /* Flag to indicate encoding was cancelled */
		bool used_dedicated_worker;  /* True if encode work queued to dedicated worker */
		/* Performance statistics */
		uint64_t t_data_write_submit;  /* Timestamp when data writes submitted */
		uint64_t t_encode_submit;  /* Timestamp when encoding task submitted to background thread */
		uint64_t t_encode_start;  /* Timestamp when encoding actually started in background thread */
		uint64_t t_encode_end;  /* Timestamp when encoding completed */
		uint64_t t_parity_write_submit;  /* Timestamp when parity writes submitted */
	} encode;
};

/* RMW state enumeration */
enum ec_rmw_state {
	EC_RMW_STATE_READING,
	EC_RMW_STATE_ENCODING,
	EC_RMW_STATE_WRITING
};

/* Decode state enumeration */
enum ec_decode_state {
	EC_DECODE_STATE_READING,
	EC_DECODE_STATE_DECODING,
	EC_DECODE_STATE_COPYING
};

/* Private structure for RMW (Read-Modify-Write) operations */
struct ec_rmw_private {
	enum ec_private_type type;
	/* Stripe buffer for full stripe data (k data blocks) */
	unsigned char *stripe_buf;
	
	/* Parity buffers (p parity blocks) */
	unsigned char *parity_bufs[EC_MAX_P];
	
	/* Data pointers for encoding */
	unsigned char *data_ptrs[EC_MAX_K];
	
	/* Stripe information */
	uint64_t stripe_index;
	uint32_t strip_idx_in_stripe;  /* Which strip in stripe (0 to k-1) */
	uint32_t offset_in_strip;      /* Offset within strip in blocks */
	uint32_t num_blocks_to_write;  /* Number of blocks to write */
	
	/* Base bdev indices */
	uint8_t data_indices[EC_MAX_K];
	uint8_t parity_indices[EC_MAX_P];
	
	/* State tracking */
	uint8_t reads_completed;
	uint8_t reads_expected;
	enum ec_rmw_state state;
	
	/* Performance statistics: timestamps for latency measurement */
	uint64_t t_rmw_start;          /* RMW operation start time */
	uint64_t t_read_complete;      /* All reads completed time */
	uint64_t t_encode_complete;    /* Encoding completed time */
	uint64_t t_write_complete;     /* All writes completed time */
};

/* Private structure for decode (recovery) operations */
struct ec_decode_private {
	enum ec_private_type type;
	/* Stripe buffer for full stripe data (k data blocks + p parity blocks) */
	unsigned char *stripe_buf;
	
	/* Data pointers for decoding - points to available blocks */
	unsigned char *data_ptrs[EC_MAX_K + EC_MAX_P];
	
	/* Output buffer for recovered data */
	unsigned char *recover_buf;
	
	/* Stripe information */
	uint64_t stripe_index;
	uint32_t strip_idx_in_stripe;  /* Which strip in stripe (0 to k-1) */
	uint32_t offset_in_strip;      /* Offset within strip in blocks */
	uint32_t num_blocks_to_read;   /* Number of blocks to read */
	
	/* Base bdev indices for available blocks */
	uint8_t available_indices[EC_MAX_K + EC_MAX_P];
	uint8_t failed_indices[EC_MAX_P];  /* Failed fragment indices (logical) */
	uint8_t num_available;
	uint8_t num_failed;
	
	/* Fragment indices mapping: base_bdev_index -> logical fragment index */
	uint8_t frag_map[EC_MAX_K + EC_MAX_P];  /* frag_map[base_idx] = frag_idx */
	
	/* State tracking */
	uint8_t reads_completed;
	uint8_t reads_expected;
	enum ec_decode_state state;
};

/* Rebuild state enumeration */
enum ec_rebuild_state {
	EC_REBUILD_STATE_IDLE,
	EC_REBUILD_STATE_READING,
	EC_REBUILD_STATE_DECODING,
	EC_REBUILD_STATE_WRITING
};

/* Rebuild context for recovering data to new disk */
struct ec_rebuild_context {
	/* Target base bdev to rebuild */
	struct ec_base_bdev_info *target_base_info;
	uint8_t target_slot;
	
	/* Current stripe being rebuilt */
	uint64_t current_stripe;
	uint64_t total_stripes;
	
	/* Stripe buffer for reading k blocks */
	unsigned char *stripe_buf;
	
	/* Recovery buffer for decoded data */
	unsigned char *recover_buf;
	
	/* Data pointers for decoding */
	unsigned char *data_ptrs[EC_MAX_K];
	
	/* Available base bdev indices for reading */
	uint8_t available_indices[EC_MAX_K];
	uint8_t num_available;
	
	/* Failed fragment index (logical fragment index 0 to m-1) */
	uint8_t failed_frag_idx;
	
	/* Fragment mapping */
	uint8_t frag_map[EC_MAX_K + EC_MAX_P];
	
	/* State tracking */
	enum ec_rebuild_state state;
	uint8_t reads_completed;
	uint8_t reads_expected;
	
	/* I/O channel for rebuild operations */
	struct spdk_io_channel *rebuild_ch;
	
	/* Wait entry for I/O queue wait */
	struct spdk_bdev_io_wait_entry wait_entry;
	
	/* Callback for rebuild completion */
	void (*rebuild_done_cb)(void *ctx, int status);
	void *rebuild_done_ctx;
	
	/* Flag to indicate if rebuild is paused */
	bool paused;
};

static inline struct ec_rebuild_context *
ec_bdev_get_active_rebuild(struct ec_bdev *ec_bdev)
{
	struct ec_rebuild_context *ctx;

	if (ec_bdev == NULL) {
		return NULL;
	}

	ctx = ec_bdev->rebuild_ctx;
	if (ctx == NULL || ctx->paused) {
		return NULL;
	}

	return ctx;
}

static inline bool
ec_bdev_is_rebuild_target(struct ec_bdev *ec_bdev, struct ec_base_bdev_info *base_info)
{
	struct ec_rebuild_context *ctx = ec_bdev_get_active_rebuild(ec_bdev);

	return ctx != NULL && ctx->target_base_info == base_info;
}

/* I/O completion callbacks */
void ec_base_bdev_io_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
void ec_base_bdev_reset_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

/* I/O path functions */
void ec_submit_rw_request(struct ec_bdev_io *ec_io);
void ec_submit_null_payload_request(struct ec_bdev_io *ec_io);
void ec_submit_reset_request(struct ec_bdev_io *ec_io);

/* ISA-L encoding/decoding functions */
int ec_encode_stripe(struct ec_bdev *ec_bdev, unsigned char **data_ptrs,
		     unsigned char **parity_ptrs, size_t len);
int ec_encode_stripe_update(struct ec_bdev *ec_bdev, uint8_t vec_i,
			    unsigned char *old_data, unsigned char *new_data,
			    unsigned char **parity_ptrs, size_t len);
int ec_bdev_init_tables(struct ec_bdev *ec_bdev, uint8_t k, uint8_t p);
void ec_bdev_cleanup_tables(struct ec_bdev *ec_bdev);
int ec_bdev_gen_decode_matrix(struct ec_bdev *ec_bdev, uint8_t *frag_err_list, int nerrs);
int ec_decode_stripe(struct ec_bdev *ec_bdev, unsigned char **data_ptrs,
		     unsigned char **recover_ptrs, uint8_t *frag_err_list, int nerrs, size_t len);

int ec_bdev_init_encode_workers(struct ec_bdev *ec_bdev);
void ec_bdev_cleanup_encode_workers(struct ec_bdev *ec_bdev);
struct spdk_thread *ec_bdev_get_encode_worker_thread(struct ec_bdev *ec_bdev, bool *is_dedicated);

static inline void
ec_bdev_encode_worker_task_start(struct ec_bdev *ec_bdev)
{
	struct ec_bdev_module_private *mp;

	if (ec_bdev == NULL) {
		return;
	}

	mp = ec_bdev->module_private;
	if (mp == NULL) {
		return;
	}

	__atomic_fetch_add(&mp->encode_workers.active_tasks, 1, __ATOMIC_RELAXED);
}

static inline void
ec_bdev_encode_worker_task_done(struct ec_bdev *ec_bdev)
{
	struct ec_bdev_module_private *mp;

	if (ec_bdev == NULL) {
		return;
	}

	mp = ec_bdev->module_private;
	if (mp == NULL) {
		return;
	}

	__atomic_fetch_sub(&mp->encode_workers.active_tasks, 1, __ATOMIC_RELAXED);
}

/* Device selection strategy configuration is defined in bdev_ec.h */

/* Base bdev selection */
int ec_select_base_bdevs_default(struct ec_bdev *ec_bdev, uint64_t stripe_index,
				 uint8_t *data_indices, uint8_t *parity_indices);

/* Device selection strategy functions */
int ec_bdev_init_selection_config(struct ec_bdev *ec_bdev);
void ec_bdev_cleanup_selection_config(struct ec_bdev *ec_bdev);
int ec_select_base_bdevs_wear_leveling(struct ec_bdev *ec_bdev,
		uint64_t stripe_index,
		uint8_t *data_indices,
		uint8_t *parity_indices);

/* Rebuild functions */
int ec_bdev_start_rebuild(struct ec_bdev *ec_bdev, struct ec_base_bdev_info *target_base_info,
			  void (*done_cb)(void *ctx, int status), void *done_ctx);
void ec_bdev_stop_rebuild(struct ec_bdev *ec_bdev);
bool ec_bdev_is_rebuilding(struct ec_bdev *ec_bdev);
int ec_bdev_get_rebuild_progress(struct ec_bdev *ec_bdev, uint64_t *current_stripe,
				 uint64_t *total_stripes);

/* Base bdev failure handling */
void ec_bdev_fail_base_bdev(struct ec_base_bdev_info *base_info);

/* LED control */
void ec_bdev_set_healthy_disks_led(struct ec_bdev *ec_bdev, enum spdk_vmd_led_state led_state);
int ec_selection_bind_group_profile(struct ec_bdev *ec_bdev, uint64_t stripe_index);
int ec_selection_create_profile_from_devices(struct ec_bdev *ec_bdev, bool make_active,
					     bool persist_now);

/* RMW stripe buffer management */
unsigned char *ec_get_rmw_stripe_buf(struct ec_bdev_io_channel *ec_ch, struct ec_bdev *ec_bdev, uint32_t buf_size);
void ec_put_rmw_stripe_buf(struct ec_bdev_io_channel *ec_ch, unsigned char *buf, uint32_t buf_size);

/* Helper function to initialize chunk iov arrays for stripe_private */
int ec_stripe_private_init_chunks(struct ec_stripe_private *stripe_priv, uint8_t k, uint8_t p);

/* Validation helper functions */
static inline int
ec_validate_encode_params(struct ec_bdev *ec_bdev, unsigned char **data_ptrs,
			  unsigned char **parity_ptrs, size_t len)
{
	if (ec_bdev == NULL) {
		SPDK_ERRLOG("ec_bdev is NULL\n");
		return -EINVAL;
	}

	if (data_ptrs == NULL || parity_ptrs == NULL) {
		SPDK_ERRLOG("EC bdev %s: data_ptrs or parity_ptrs is NULL\n", ec_bdev->bdev.name);
		return -EINVAL;
	}

	if (ec_bdev->module_private == NULL || ec_bdev->module_private->g_tbls == NULL) {
		SPDK_ERRLOG("EC bdev %s: module_private or g_tbls is NULL\n", ec_bdev->bdev.name);
		return -EINVAL;
	}

	if (len == 0) {
		SPDK_ERRLOG("EC bdev %s: encoding length is zero\n", ec_bdev->bdev.name);
		return -EINVAL;
	}

	return 0;
}

static inline int
ec_validate_decode_params(struct ec_bdev *ec_bdev, unsigned char **data_ptrs,
			  unsigned char **recover_ptrs, uint8_t *frag_err_list,
			  int nerrs, size_t len)
{
	if (ec_bdev == NULL) {
		SPDK_ERRLOG("ec_bdev is NULL\n");
		return -EINVAL;
	}

	if (data_ptrs == NULL || recover_ptrs == NULL || frag_err_list == NULL) {
		SPDK_ERRLOG("EC bdev %s: data_ptrs, recover_ptrs or frag_err_list is NULL\n",
			    ec_bdev->bdev.name);
		return -EINVAL;
	}

	if (ec_bdev->module_private == NULL || ec_bdev->module_private->encode_matrix == NULL) {
		SPDK_ERRLOG("EC bdev %s: module_private or encode_matrix is NULL\n",
			    ec_bdev->bdev.name);
		return -EINVAL;
	}

	if (nerrs == 0 || nerrs > ec_bdev->p) {
		SPDK_ERRLOG("EC bdev %s: invalid nerrs %d (must be 1-%u)\n",
			    ec_bdev->bdev.name, nerrs, ec_bdev->p);
		return -EINVAL;
	}

	if (len == 0) {
		SPDK_ERRLOG("EC bdev %s: decoding length is zero\n", ec_bdev->bdev.name);
		return -EINVAL;
	}

	return 0;
}

#endif /* SPDK_BDEV_EC_INTERNAL_H */

