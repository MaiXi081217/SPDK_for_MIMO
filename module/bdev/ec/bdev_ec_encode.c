/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "bdev_ec.h"
#include "bdev_ec_internal.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include <isa-l/erasure_code.h>
#include <isa-l/gf_vect_mul.h>
#include <string.h>

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
	struct ec_bdev_module_private *mp;
	uint8_t k, p;
	uint8_t i;
	bool misaligned_warning = false;
	const size_t isal_optimal_align = 64;  /* ISA-L optimal alignment for SIMD */

	/* Validate input parameters */
	if (ec_bdev == NULL) {
		SPDK_ERRLOG("ec_bdev is NULL\n");
		return -EINVAL;
	}

	if (data_ptrs == NULL || parity_ptrs == NULL) {
		SPDK_ERRLOG("EC bdev %s: data_ptrs or parity_ptrs is NULL\n", ec_bdev->bdev.name);
		return -EINVAL;
	}

	mp = ec_bdev->module_private;
	if (mp == NULL || mp->g_tbls == NULL) {
		SPDK_ERRLOG("EC bdev %s: module_private or g_tbls is NULL\n", ec_bdev->bdev.name);
		return -EINVAL;
	}

	/* Optimized: Cache k and p to reduce memory access */
	k = ec_bdev->k;
	p = ec_bdev->p;

	/* Critical: Verify all data and parity pointers are non-NULL before encoding
	 * ISA-L's ec_encode_data cannot handle NULL pointers and will cause crashes
	 * Also check alignment during validation to reduce loop overhead
	 */
	for (i = 0; i < k; i++) {
		if (data_ptrs[i] == NULL) {
			SPDK_ERRLOG("EC bdev %s: data_ptrs[%u] is NULL\n", ec_bdev->bdev.name, i);
			return -EINVAL;
		}
		/* Check alignment while validating - combine operations */
		if (!misaligned_warning) {
			uintptr_t addr = (uintptr_t)data_ptrs[i];
			if ((addr % isal_optimal_align) != 0) {
				misaligned_warning = true;
			}
		}
	}
	for (i = 0; i < p; i++) {
		if (parity_ptrs[i] == NULL) {
			SPDK_ERRLOG("EC bdev %s: parity_ptrs[%u] is NULL\n", ec_bdev->bdev.name, i);
			return -EINVAL;
		}
		/* Check alignment while validating */
		if (!misaligned_warning) {
			uintptr_t addr = (uintptr_t)parity_ptrs[i];
			if ((addr % isal_optimal_align) != 0) {
				misaligned_warning = true;
			}
		}
	}

	/* Verify length is non-zero and check alignment */
	if (len == 0) {
		SPDK_ERRLOG("EC bdev %s: encoding length is zero\n", ec_bdev->bdev.name);
		return -EINVAL;
	}
	if (!misaligned_warning && (len % isal_optimal_align) != 0) {
		misaligned_warning = true;
	}

	/* Log warning if misaligned (only once per bdev to avoid log spam) */
	if (misaligned_warning && spdk_unlikely(ec_bdev->alignment_warned == false)) {
		SPDK_WARNLOG("EC bdev %s: Data buffers or length not optimally aligned for ISA-L. "
			     "For best performance, use 64-byte aligned buffers and lengths.\n",
			     ec_bdev->bdev.name);
		ec_bdev->alignment_warned = true;
	}

	/* Optimized: Aggressive prefetching strategy for ISA-L encoding
	 * Key optimizations:
	 * 1. Prefetch encoding tables (g_tbls) - frequently accessed during encoding
	 * 2. Staggered prefetching - start prefetching early to hide memory latency
	 * 3. Multiple cache lines for large blocks - prefetch ahead of current position
	 * 4. Interleaved prefetching - mix data and parity prefetches for better parallelism
	 */
	
	/* Prefetch encoding tables first - they're accessed frequently during encoding */
	EC_PREFETCH(mp->g_tbls, 0);
	if (mp->g_tbls != NULL && (32 * k * p) > 64) {
		/* Prefetch second cache line if tables are large */
		EC_PREFETCH((char *)mp->g_tbls + 64, 0);
	}

	/* Optimized: Aggressive prefetching for data blocks
	 * Prefetch strategy based on block size:
	 * - Small blocks (< 1KB): prefetch first cache line
	 * - Medium blocks (1KB - 8KB): prefetch first + middle
	 * - Large blocks (> 8KB): prefetch multiple cache lines ahead
	 */
	size_t prefetch_distance = 0;
	
	if (len >= 8192) {
		prefetch_distance = 1024;  /* Prefetch 1KB ahead for very large blocks */
	} else if (len >= 1024) {
		prefetch_distance = 256;   /* Prefetch 256B ahead for medium blocks */
	}

	for (i = 0; i < k; i++) {
		/* Prefetch first cache line immediately */
		EC_PREFETCH(data_ptrs[i], 0);
		
		/* Prefetch additional cache lines based on block size */
		if (len > 128) {
			EC_PREFETCH(data_ptrs[i] + 64, 0);
		}
		if (len >= 1024) {
			EC_PREFETCH(data_ptrs[i] + 256, 0);
		}
		if (prefetch_distance > 0 && len > prefetch_distance) {
			EC_PREFETCH(data_ptrs[i] + prefetch_distance, 0);
		}
	}

	/* Optimized: Prefetch parity blocks for write
	 * Use write hint (1) for parity blocks since they'll be written
	 */
	for (i = 0; i < p; i++) {
		/* Prefetch first cache line */
		EC_PREFETCH(parity_ptrs[i], 1);
		
		/* Prefetch additional cache lines for large blocks */
		if (len > 128) {
			EC_PREFETCH(parity_ptrs[i] + 64, 1);
		}
		if (len >= 1024) {
			EC_PREFETCH(parity_ptrs[i] + 256, 1);
		}
		if (prefetch_distance > 0 && len > prefetch_distance) {
			EC_PREFETCH(parity_ptrs[i] + prefetch_distance, 1);
		}
	}

	/* Memory barrier to ensure prefetched data is visible before encoding
	 * This is especially important for multi-threaded scenarios
	 */
#ifdef __x86_64__
	asm volatile("" ::: "memory");  /* Compiler memory barrier */
#endif

	/* Use ISA-L to encode k data blocks into p parity blocks */
	/* Note: ISA-L's ec_encode_data is highly optimized with SIMD instructions */
	/* It can handle unaligned data, but aligned data performs better */
	ec_encode_data(len, k, p, mp->g_tbls, data_ptrs, parity_ptrs);

	return 0;
}

/*
 * brief:
 * ec_encode_stripe_update performs incremental update of parity blocks when a single data block changes
 * This is much more efficient than re-encoding the entire stripe for partial writes
 * params:
 * ec_bdev - pointer to EC bdev
 * vec_i - index of the data block that changed (0 to k-1)
 * old_data - pointer to old data block (can be NULL if not available, will read from disk)
 * new_data - pointer to new data block
 * parity_ptrs - array of pointers to parity blocks (p pointers) - will be updated in-place
 * len - length of each block in bytes
 * returns:
 * 0 on success, non-zero on failure
 */
int
ec_encode_stripe_update(struct ec_bdev *ec_bdev, uint8_t vec_i,
		       unsigned char *old_data, unsigned char *new_data,
		       unsigned char **parity_ptrs, size_t len)
{
	struct ec_bdev_module_private *mp = ec_bdev->module_private;
	uint8_t k, p;
	uint8_t i;
	unsigned char *delta_data = NULL;
	const size_t isal_optimal_align = 64;

	if (mp == NULL || mp->g_tbls == NULL) {
		return -EINVAL;
	}

	k = ec_bdev->k;
	p = ec_bdev->p;

	if (vec_i >= k) {
		SPDK_ERRLOG("Invalid vec_i %u (k=%u)\n", vec_i, k);
		return -EINVAL;
	}

	/* If old_data is provided, compute delta (XOR difference) for incremental update */
	if (old_data != NULL && new_data != NULL) {
		/* Allocate temporary buffer for delta if needed */
		/* For optimal performance, delta should be aligned */
		delta_data = spdk_dma_malloc(len, isal_optimal_align, NULL);
		if (delta_data == NULL) {
			SPDK_ERRLOG("Failed to allocate delta buffer for incremental update\n");
			/* Fall back to full re-encode if delta allocation fails */
			return -ENOMEM;
		}

		/* Compute delta = old_data XOR new_data */
		/* This represents the change that needs to be applied to parity */
		/* Optimized: Use word-sized XOR for better performance on aligned data */
		if (len >= 8 && ((uintptr_t)old_data % 8 == 0) && ((uintptr_t)new_data % 8 == 0) &&
		    ((uintptr_t)delta_data % 8 == 0)) {
			/* Use 64-bit XOR for aligned data - much faster */
			uint64_t *old64 = (uint64_t *)old_data;
			uint64_t *new64 = (uint64_t *)new_data;
			uint64_t *delta64 = (uint64_t *)delta_data;
			size_t len64 = len / 8;
			size_t j;
			for (j = 0; j < len64; j++) {
				delta64[j] = old64[j] ^ new64[j];
			}
			/* Handle remaining bytes */
			for (j = len64 * 8; j < len; j++) {
				delta_data[j] = old_data[j] ^ new_data[j];
			}
		} else {
			/* Fall back to byte-wise XOR for unaligned data */
			size_t j;
			for (j = 0; j < len; j++) {
				delta_data[j] = old_data[j] ^ new_data[j];
			}
		}
	} else if (new_data != NULL) {
		/* If no old_data, use new_data directly (assumes parity was zero-initialized) */
		delta_data = new_data;
	} else {
		SPDK_ERRLOG("Both old_data and new_data cannot be NULL\n");
		return -EINVAL;
	}

	/* Optimized: Aggressive prefetching for incremental update
	 * Prefetch strategy optimized for incremental update pattern:
	 * - Delta data is read once, prefetch multiple cache lines
	 * - Parity blocks are read-modify-write, prefetch for both read and write
	 */
	
	/* Prefetch delta data for read - prefetch multiple cache lines for large blocks */
	if (delta_data != NULL) {
		EC_PREFETCH(delta_data, 0);
		if (len > 128) {
			EC_PREFETCH(delta_data + 64, 0);
		}
		if (len >= 1024) {
			EC_PREFETCH(delta_data + 256, 0);
			if (len >= 8192) {
				EC_PREFETCH(delta_data + 1024, 0);
			}
		}
	}

	/* Prefetch parity blocks for read-modify-write
	 * Parity blocks need to be read first, then modified
	 */
	for (i = 0; i < p; i++) {
		if (parity_ptrs[i] != NULL) {
			/* Prefetch for read first (hint 0) since we read before write */
			EC_PREFETCH(parity_ptrs[i], 0);
			if (len > 128) {
				EC_PREFETCH(parity_ptrs[i] + 64, 0);
			}
			if (len >= 1024) {
				EC_PREFETCH(parity_ptrs[i] + 256, 0);
				/* Also prefetch for write since we'll modify */
				EC_PREFETCH(parity_ptrs[i], 1);
			}
		}
	}

	/* Prefetch encoding tables - accessed during incremental update */
	EC_PREFETCH(mp->g_tbls, 0);

	/* Use ISA-L's incremental update function to update parity blocks */
	/* This is much faster than re-encoding the entire stripe */
	/* ec_encode_data_update computes: parity[i] = parity[i] XOR (delta * coefficient[i][vec_i]) */
	ec_encode_data_update(len, k, p, vec_i, mp->g_tbls, delta_data, parity_ptrs);

	/* Free temporary delta buffer if we allocated it */
	if (delta_data != NULL && delta_data != new_data && delta_data != old_data) {
		spdk_dma_free(delta_data);
	}

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
	/* Optimized: Use malloc instead of calloc - matrix will be fully initialized by gf_gen_cauchy1_matrix */
	mp->encode_matrix = malloc(matrix_size);
	if (mp->encode_matrix == NULL) {
		return -ENOMEM;
	}

	/* Generate Cauchy matrix for encoding */
	gf_gen_cauchy1_matrix(mp->encode_matrix, m, k);

	/* Allocate decode tables */
	/* Optimized: Use malloc instead of calloc - tables will be fully initialized by ec_init_tables */
	mp->g_tbls = malloc(g_tbls_size);
	if (mp->g_tbls == NULL) {
		free(mp->encode_matrix);
		mp->encode_matrix = NULL;
		return -ENOMEM;
	}

	/* Initialize decode tables from encode matrix */
	ec_init_tables(k, p, &mp->encode_matrix[k * k], mp->g_tbls);

	/* Allocate decode matrix and temp matrices */
	/* Optimized: Use malloc instead of calloc - matrices will be initialized when used */
	mp->decode_matrix = malloc(matrix_size);
	mp->temp_matrix = malloc(matrix_size);
	mp->invert_matrix = malloc(k * k);
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
	/* Optimized: Cache k, p, and m to reduce memory access */
	uint8_t k = ec_bdev->k;
	uint8_t p = ec_bdev->p;
	uint8_t m = k + p;
	uint8_t frag_in_err[EC_MAX_K + EC_MAX_P];
	int i, j, p_idx, r;
	unsigned char s;
	unsigned char *b = temp_matrix;

	if (mp == NULL || encode_matrix == NULL || decode_matrix == NULL ||
	    invert_matrix == NULL || temp_matrix == NULL) {
		return -EINVAL;
	}

	if (nerrs > p || nerrs < 0 || nerrs > m) {
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
	/* Optimized: Renamed loop variable from p to p_idx to avoid shadowing p variable */
	for (p_idx = 0; p_idx < nerrs; p_idx++) {
		if (frag_err_list[p_idx] >= k) {
			/* A parity error */
			for (i = 0; i < k; i++) {
				s = 0;
				for (j = 0; j < k; j++) {
					s ^= gf_mul(invert_matrix[j * k + i],
						    encode_matrix[k * frag_err_list[p_idx] + j]);
				}
				decode_matrix[k * p_idx + i] = s;
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

/*
 * brief:
 * ec_decode_stripe decodes/recover failed fragments using ISA-L
 * params:
 * ec_bdev - pointer to EC bdev
 * data_ptrs - array of pointers to available data blocks (k pointers)
 * recover_ptrs - array of pointers to recovery output buffers (nerrs pointers)
 * frag_err_list - list of failed fragment indices (logical fragment indices 0 to m-1)
 * nerrs - number of erasures
 * len - length of each block in bytes
 * returns:
 * 0 on success, non-zero on failure
 */
int
ec_decode_stripe(struct ec_bdev *ec_bdev, unsigned char **data_ptrs,
		 unsigned char **recover_ptrs, uint8_t *frag_err_list, int nerrs, size_t len)
{
	struct ec_bdev_module_private *mp;
	uint8_t k, p;
	uint8_t i;
	unsigned char *decode_tbls = NULL;
	int rc;

	/* Validate input parameters */
	if (ec_bdev == NULL) {
		SPDK_ERRLOG("ec_bdev is NULL\n");
		return -EINVAL;
	}

	if (data_ptrs == NULL || recover_ptrs == NULL || frag_err_list == NULL) {
		SPDK_ERRLOG("EC bdev %s: data_ptrs, recover_ptrs or frag_err_list is NULL\n",
			    ec_bdev->bdev.name);
		return -EINVAL;
	}

	mp = ec_bdev->module_private;
	if (mp == NULL || mp->encode_matrix == NULL) {
		SPDK_ERRLOG("EC bdev %s: module_private or encode_matrix is NULL\n",
			    ec_bdev->bdev.name);
		return -EINVAL;
	}

	k = ec_bdev->k;
	p = ec_bdev->p;

	if (nerrs == 0 || nerrs > p) {
		SPDK_ERRLOG("EC bdev %s: invalid nerrs %d (must be 1-%u)\n",
			    ec_bdev->bdev.name, nerrs, p);
		return -EINVAL;
	}

	if (len == 0) {
		SPDK_ERRLOG("EC bdev %s: decoding length is zero\n", ec_bdev->bdev.name);
		return -EINVAL;
	}

	/* Verify all data pointers are non-NULL */
	for (i = 0; i < k; i++) {
		if (data_ptrs[i] == NULL) {
			SPDK_ERRLOG("EC bdev %s: data_ptrs[%u] is NULL\n", ec_bdev->bdev.name, i);
			return -EINVAL;
		}
	}

	/* Verify all recovery pointers are non-NULL */
	for (i = 0; i < nerrs; i++) {
		if (recover_ptrs[i] == NULL) {
			SPDK_ERRLOG("EC bdev %s: recover_ptrs[%u] is NULL\n", ec_bdev->bdev.name, i);
			return -EINVAL;
		}
	}

	/* Generate decode matrix */
	rc = ec_bdev_gen_decode_matrix(ec_bdev, frag_err_list, nerrs);
	if (rc != 0) {
		SPDK_ERRLOG("EC bdev %s: failed to generate decode matrix: %s\n",
			    ec_bdev->bdev.name, spdk_strerror(-rc));
		return rc;
	}

	/* Allocate decode tables */
	decode_tbls = malloc(32 * k * nerrs);
	if (decode_tbls == NULL) {
		SPDK_ERRLOG("EC bdev %s: failed to allocate decode tables\n", ec_bdev->bdev.name);
		return -ENOMEM;
	}

	/* Initialize decode tables from decode matrix */
	ec_init_tables(k, nerrs, mp->decode_matrix, decode_tbls);

	/* Use ISA-L to decode/recover failed fragments
	 * ec_encode_data can be used for decoding by providing decode matrix
	 * data_ptrs: k available source blocks
	 * recover_ptrs: nerrs output buffers for recovered blocks
	 */
	ec_encode_data(len, k, nerrs, decode_tbls, data_ptrs, recover_ptrs);

	/* Free decode tables */
	free(decode_tbls);

	return 0;
}


