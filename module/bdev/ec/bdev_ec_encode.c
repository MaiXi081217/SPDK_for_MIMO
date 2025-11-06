/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "bdev_ec.h"
#include "bdev_ec_internal.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include <isa-l/erasure_code.h>
#include <isa-l/gf_vect_mul.h>
#include <string.h>

/*
 * brief:
 * ec_encode_stripe encodes data stripe using ISA-L
 * params:
 * ec_bdev - pointer to EC bdev
 * data_ptrs - array of pointers to data blocks (k pointers)
 * parity_ptrs - array of pointers to parity blocks (p pointers)
 * len - length of each block in bytes
 * returns:
 * 0 on success, non-zero on failure
 */
int
ec_encode_stripe(struct ec_bdev *ec_bdev, unsigned char **data_ptrs,
		 unsigned char **parity_ptrs, size_t len)
{
	struct ec_bdev_module_private *mp = ec_bdev->module_private;

	if (mp == NULL || mp->g_tbls == NULL) {
		return -EINVAL;
	}

	/* Use ISA-L to encode k data blocks into p parity blocks */
	ec_encode_data(len, ec_bdev->k, ec_bdev->p, mp->g_tbls, data_ptrs, parity_ptrs);

	return 0;
}

/*
 * brief:
 * ec_bdev_init_tables initializes ISA-L tables for EC encoding/decoding
 * params:
 * ec_bdev - pointer to EC bdev
 * k - number of data blocks
 * p - number of parity blocks
 * returns:
 * 0 on success, non-zero on failure
 */
int
ec_bdev_init_tables(struct ec_bdev *ec_bdev, uint8_t k, uint8_t p)
{
	struct ec_bdev_module_private *mp = ec_bdev->module_private;
	uint8_t m = k + p;
	int matrix_size = k * m;
	int g_tbls_size = 32 * k * p;

	if (mp == NULL) {
		return -EINVAL;
	}

	/* Allocate encode matrix */
	mp->encode_matrix = calloc(1, matrix_size);
	if (mp->encode_matrix == NULL) {
		return -ENOMEM;
	}

	/* Generate Cauchy matrix for encoding */
	gf_gen_cauchy1_matrix(mp->encode_matrix, m, k);

	/* Allocate decode tables */
	mp->g_tbls = calloc(1, g_tbls_size);
	if (mp->g_tbls == NULL) {
		free(mp->encode_matrix);
		mp->encode_matrix = NULL;
		return -ENOMEM;
	}

	/* Initialize decode tables from encode matrix */
	ec_init_tables(k, p, &mp->encode_matrix[k * k], mp->g_tbls);

	/* Allocate decode matrix and temp matrices */
	mp->decode_matrix = calloc(1, matrix_size);
	mp->temp_matrix = calloc(1, matrix_size);
	mp->invert_matrix = calloc(1, k * k);
	if (mp->decode_matrix == NULL || mp->temp_matrix == NULL || mp->invert_matrix == NULL) {
		ec_bdev_cleanup_tables(ec_bdev);
		return -ENOMEM;
	}

	return 0;
}

/*
 * brief:
 * ec_bdev_gen_decode_matrix generates decode matrix from encode matrix and erasure list
 * This is based on ISA-L's gf_gen_decode_matrix_simple function
 * params:
 * ec_bdev - pointer to EC bdev
 * frag_err_list - list of failed fragment indices
 * nerrs - number of erasures
 * returns:
 * 0 on success, non-zero on failure
 */
int
ec_bdev_gen_decode_matrix(struct ec_bdev *ec_bdev, uint8_t *frag_err_list, int nerrs)
{
	struct ec_bdev_module_private *mp = ec_bdev->module_private;
	unsigned char *encode_matrix = mp->encode_matrix;
	unsigned char *decode_matrix = mp->decode_matrix;
	unsigned char *invert_matrix = mp->invert_matrix;
	unsigned char *temp_matrix = mp->temp_matrix;
	unsigned char *decode_index = mp->decode_index;
	uint8_t k = ec_bdev->k;
	uint8_t m = ec_bdev->k + ec_bdev->p;
	uint8_t frag_in_err[EC_MAX_K + EC_MAX_P];
	int i, j, p, r;
	unsigned char s;
	unsigned char *b = temp_matrix;

	if (mp == NULL || encode_matrix == NULL || decode_matrix == NULL ||
	    invert_matrix == NULL || temp_matrix == NULL) {
		return -EINVAL;
	}

	if (nerrs > ec_bdev->p || nerrs < 0 || nerrs > m) {
		return -EINVAL;
	}

	memset(frag_in_err, 0, sizeof(frag_in_err));

	/* Mark the input fragments with error for later processing */
	for (i = 0; i < nerrs; i++) {
		if (frag_err_list[i] >= m) {
			return -EINVAL;
		}
		frag_in_err[frag_err_list[i]] = 1;
	}

	/* Construct b (matrix that encoded remaining frags) by removing erased rows */
	for (i = 0, r = 0; i < k; i++, r++) {
		while (r < m && frag_in_err[r]) {
			r++;
		}
		if (r >= m) {
			/* Not enough valid fragments to decode */
			return -EINVAL;
		}
		for (j = 0; j < k; j++) {
			b[k * i + j] = encode_matrix[k * r + j];
		}
		decode_index[i] = r;
	}

	/* Invert matrix to get recovery matrix */
	if (gf_invert_matrix(b, invert_matrix, k) < 0) {
		return -EINVAL;
	}

	/* Get decode matrix with only wanted recovery rows */
	for (i = 0; i < nerrs; i++) {
		if (frag_err_list[i] < k) {
			/* A source (data) error */
			for (j = 0; j < k; j++) {
				decode_matrix[k * i + j] = invert_matrix[k * frag_err_list[i] + j];
			}
		}
	}

	/* For non-src (parity) erasures need to multiply encode matrix * invert */
	for (p = 0; p < nerrs; p++) {
		if (frag_err_list[p] >= k) {
			/* A parity error */
			for (i = 0; i < k; i++) {
				s = 0;
				for (j = 0; j < k; j++) {
					s ^= gf_mul(invert_matrix[j * k + i],
						    encode_matrix[k * frag_err_list[p] + j]);
				}
				decode_matrix[k * p + i] = s;
			}
		}
	}

	return 0;
}

/*
 * brief:
 * ec_bdev_cleanup_tables cleans up ISA-L tables
 * params:
 * ec_bdev - pointer to EC bdev
 * returns:
 * none
 */
void
ec_bdev_cleanup_tables(struct ec_bdev *ec_bdev)
{
	struct ec_bdev_module_private *mp = ec_bdev->module_private;

	if (mp == NULL) {
		return;
	}

	free(mp->encode_matrix);
	mp->encode_matrix = NULL;

	free(mp->g_tbls);
	mp->g_tbls = NULL;

	free(mp->decode_matrix);
	mp->decode_matrix = NULL;

	free(mp->temp_matrix);
	mp->temp_matrix = NULL;

	free(mp->invert_matrix);
	mp->invert_matrix = NULL;
}

