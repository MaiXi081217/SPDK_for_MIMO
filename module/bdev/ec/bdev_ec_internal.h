/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_EC_INTERNAL_H
#define SPDK_BDEV_EC_INTERNAL_H

#include "bdev_ec.h"

/* Type identifier for module_private structures */
enum ec_private_type {
	EC_PRIVATE_TYPE_FULL_STRIPE,
	EC_PRIVATE_TYPE_RMW
};

/* Private structure for stripe write operations */
struct ec_stripe_private {
	enum ec_private_type type;
	unsigned char *parity_bufs[EC_MAX_P];
	uint8_t num_parity;
};

/* RMW state enumeration */
enum ec_rmw_state {
	EC_RMW_STATE_READING,
	EC_RMW_STATE_ENCODING,
	EC_RMW_STATE_WRITING
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
int ec_bdev_init_tables(struct ec_bdev *ec_bdev, uint8_t k, uint8_t p);
void ec_bdev_cleanup_tables(struct ec_bdev *ec_bdev);
int ec_bdev_gen_decode_matrix(struct ec_bdev *ec_bdev, uint8_t *frag_err_list, int nerrs);

/* Base bdev selection */
int ec_select_base_bdevs_default(struct ec_bdev *ec_bdev, uint8_t *data_indices,
				 uint8_t *parity_indices);

#endif /* SPDK_BDEV_EC_INTERNAL_H */

