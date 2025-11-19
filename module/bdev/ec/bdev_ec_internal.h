/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_EC_INTERNAL_H
#define SPDK_BDEV_EC_INTERNAL_H

#include "bdev_ec.h"
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

/* Type identifier for module_private structures */
enum ec_private_type {
	EC_PRIVATE_TYPE_FULL_STRIPE,
	EC_PRIVATE_TYPE_RMW,
	EC_PRIVATE_TYPE_DECODE
};

/* Private structure for stripe write operations */
struct ec_stripe_private {
	enum ec_private_type type;
	unsigned char *parity_bufs[EC_MAX_P];
	uint8_t num_parity;
	/* Temporary buffers for cross-iov data (only used when need_temp_bufs is true) */
	unsigned char *temp_data_bufs[EC_MAX_K];
	uint8_t num_temp_bufs;  /* Number of temp buffers allocated (0 if not needed) */
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

/* Base bdev selection */
int ec_select_base_bdevs_default(struct ec_bdev *ec_bdev, uint64_t stripe_index,
				 uint8_t *data_indices, uint8_t *parity_indices);

/* Extension interface helper - unified base bdev selection */
static inline int
ec_select_base_bdevs_with_extension(struct ec_bdev *ec_bdev,
				    uint64_t offset_blocks,
				    uint32_t num_blocks,
				    uint64_t stripe_index,
				    uint8_t *data_indices,
				    uint8_t *parity_indices)
{
	struct ec_bdev_extension_if *ext_if = ec_bdev->extension_if;

	/* Use extension interface if available, otherwise fall back to default */
	if (ext_if != NULL && ext_if->select_base_bdevs != NULL) {
		int rc = ext_if->select_base_bdevs(ext_if, ec_bdev,
						    offset_blocks, num_blocks,
						    data_indices, parity_indices,
						    ext_if->ctx);
		if (spdk_unlikely(rc != 0)) {
			SPDK_ERRLOG("Extension interface failed to select base bdevs for EC bdev %s\n",
				    ec_bdev->bdev.name);
			return rc;
		}
		return 0;
	}

	/* Fall back to default selection */
	return ec_select_base_bdevs_default(ec_bdev, stripe_index, data_indices, parity_indices);
}

/* Extension interface helper - notify I/O completion for wear level tracking */
static inline void
ec_notify_extension_io_complete(struct ec_bdev *ec_bdev,
				struct ec_base_bdev_info *base_info,
				uint64_t offset_blocks,
				uint32_t num_blocks,
				bool is_write)
{
	struct ec_bdev_extension_if *ext_if = ec_bdev->extension_if;

	if (ext_if != NULL && ext_if->notify_io_complete != NULL && base_info != NULL) {
		ext_if->notify_io_complete(ext_if, ec_bdev, base_info,
					   offset_blocks, num_blocks,
					   is_write, ext_if->ctx);
	}
}

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

/* RMW stripe buffer management */
unsigned char *ec_get_rmw_stripe_buf(struct ec_bdev_io_channel *ec_ch, struct ec_bdev *ec_bdev, uint32_t buf_size);
void ec_put_rmw_stripe_buf(struct ec_bdev_io_channel *ec_ch, unsigned char *buf, uint32_t buf_size);

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

