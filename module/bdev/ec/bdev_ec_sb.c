/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/bdev_module.h"
#include "spdk/crc32.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "bdev_ec.h"

struct ec_bdev_write_sb_ctx {
	struct ec_bdev *ec_bdev;
	int status;
	uint8_t submitted;
	uint8_t remaining;
	ec_bdev_write_sb_cb cb;
	void *cb_ctx;
	struct spdk_bdev_io_wait_entry wait_entry;
};

struct ec_bdev_read_sb_ctx {
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	ec_bdev_load_sb_cb cb;
	void *cb_ctx;
	void *buf;
	uint32_t buf_size;
};

/* Forward declarations */
static void ec_bdev_sb_update_crc(struct ec_bdev_superblock *sb);

int
ec_bdev_alloc_superblock(struct ec_bdev *ec_bdev, uint32_t block_size)
{
	struct ec_bdev_superblock *sb;
	uint64_t align = 0x1000; /* Default 4KB alignment */
	struct ec_base_bdev_info *base_info;

	assert(ec_bdev->sb == NULL);

	/* Try to get alignment from first configured base bdev if available */
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->is_configured && base_info->desc != NULL) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(base_info->desc);
			align = spdk_bdev_get_buf_align(bdev);
			break;
		}
	}

	/* If no base bdev is configured yet, use default alignment */
	/* Note: This is safe as 4KB is a common alignment requirement */
	sb = spdk_dma_zmalloc(SPDK_ALIGN_CEIL(EC_BDEV_SB_MAX_LENGTH, block_size), align, NULL);
	if (!sb) {
		SPDK_ERRLOG("Failed to allocate EC bdev sb buffer\n");
		return -ENOMEM;
	}

	ec_bdev->sb = sb;

	return 0;
}

void
ec_bdev_free_superblock(struct ec_bdev *ec_bdev)
{
	if (ec_bdev->sb_io_buf != NULL && ec_bdev->sb_io_buf != ec_bdev->sb) {
		assert(spdk_bdev_is_md_interleaved(&ec_bdev->bdev));
		spdk_dma_free(ec_bdev->sb_io_buf);
		ec_bdev->sb_io_buf = NULL;
	}
	if (ec_bdev->sb != NULL) {
		SPDK_DEBUGLOG(bdev_ec_sb, "Freed superblock buffers for %s\n",
			      ec_bdev->bdev.name);
		spdk_dma_free(ec_bdev->sb);
		ec_bdev->sb = NULL;
	}
}

void
ec_bdev_init_superblock(struct ec_bdev *ec_bdev)
{
	struct ec_bdev_superblock *sb = ec_bdev->sb;
	struct ec_base_bdev_info *base_info;
	struct ec_bdev_sb_base_bdev *sb_base_bdev;

	memcpy(&sb->signature, EC_BDEV_SB_SIG, sizeof(sb->signature));
	sb->version.major = EC_BDEV_SB_VERSION_MAJOR;
	sb->version.minor = EC_BDEV_SB_VERSION_MINOR;
	spdk_uuid_copy(&sb->uuid, &ec_bdev->bdev.uuid);
	snprintf(sb->name, EC_BDEV_SB_NAME_SIZE, "%s", ec_bdev->bdev.name);
	sb->ec_size = ec_bdev->bdev.blockcnt;
	sb->block_size = spdk_bdev_get_data_block_size(&ec_bdev->bdev);
	sb->strip_size = ec_bdev->strip_size;
	sb->state = ec_bdev->state;
	sb->num_base_bdevs = sb->base_bdevs_size = ec_bdev->num_base_bdevs;
	sb->k = ec_bdev->k;
	sb->p = ec_bdev->p;
	sb->length = sizeof(*sb) + sizeof(*sb_base_bdev) * sb->base_bdevs_size;

	sb_base_bdev = &sb->base_bdevs[0];
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		spdk_uuid_copy(&sb_base_bdev->uuid, &base_info->uuid);
		sb_base_bdev->data_offset = base_info->data_offset;
		sb_base_bdev->data_size = base_info->data_size;
		sb_base_bdev->state = EC_SB_BASE_BDEV_CONFIGURED;
		sb_base_bdev->slot = (uint8_t)(base_info - ec_bdev->base_bdev_info);
		sb_base_bdev->is_data_block = base_info->is_data_block;
		sb_base_bdev++;
	}

	/* Update CRC immediately after initialization for safety and debugging */
	ec_bdev_sb_update_crc(sb);
}

static int
ec_bdev_alloc_sb_io_buf(struct ec_bdev *ec_bdev)
{
	struct ec_bdev_superblock *sb = ec_bdev->sb;
	uint32_t data_block_size = spdk_bdev_get_data_block_size(&ec_bdev->bdev);
	uint64_t align = 0x1000; /* Default alignment */
	struct ec_base_bdev_info *base_info;

	/* Try to get alignment from first configured base bdev if available */
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->is_configured && base_info->desc != NULL) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(base_info->desc);
			align = spdk_bdev_get_buf_align(bdev);
			break;
		}
	}

	if (spdk_bdev_is_md_interleaved(&ec_bdev->bdev)) {
		/* Use actual data block size instead of sb->block_size */
		ec_bdev->sb_io_buf_size = spdk_divide_round_up(sb->length,
					    data_block_size) * ec_bdev->bdev.blocklen;
		ec_bdev->sb_io_buf = spdk_dma_zmalloc(ec_bdev->sb_io_buf_size, align, NULL);
		if (!ec_bdev->sb_io_buf) {
			SPDK_ERRLOG("Failed to allocate EC bdev sb io buffer\n");
			return -ENOMEM;
		}
	} else {
		ec_bdev->sb_io_buf_size = SPDK_ALIGN_CEIL(sb->length, ec_bdev->bdev.blocklen);
		ec_bdev->sb_io_buf = ec_bdev->sb;
	}

	return 0;
}

static void
ec_bdev_sb_update_crc(struct ec_bdev_superblock *sb)
{
	sb->crc = 0;
	sb->crc = spdk_crc32c_update(sb, sb->length, 0);
}

static bool
ec_bdev_sb_check_crc(struct ec_bdev_superblock *sb)
{
	uint32_t crc, prev = sb->crc;

	ec_bdev_sb_update_crc(sb);
	crc = sb->crc;
	sb->crc = prev;

	return crc == prev;
}

static int
ec_bdev_parse_superblock(struct ec_bdev_read_sb_ctx *ctx)
{
	struct ec_bdev_superblock *sb = ctx->buf;
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(ctx->desc);
	struct ec_bdev_sb_base_bdev *sb_base_bdev;
	uint8_t i;
	char uuid_str[SPDK_UUID_STRING_LEN];

	if (memcmp(sb->signature, EC_BDEV_SB_SIG, sizeof(sb->signature))) {
		SPDK_DEBUGLOG(bdev_ec_sb, "Invalid superblock signature on bdev %s\n",
			      spdk_bdev_get_name(bdev));
		return -EINVAL;
	}

	/* Check length before processing */
	if (sb->length > EC_BDEV_SB_MAX_LENGTH) {
		SPDK_ERRLOG("Superblock length %u exceeds maximum %zu on bdev %s\n",
			    sb->length, (size_t)EC_BDEV_SB_MAX_LENGTH, spdk_bdev_get_name(bdev));
		return -EINVAL;
	}

	if (spdk_divide_round_up(sb->length, spdk_bdev_get_data_block_size(bdev)) >
	    spdk_divide_round_up(ctx->buf_size, bdev->blocklen)) {
		return -EAGAIN;
	}

	if (!ec_bdev_sb_check_crc(sb)) {
		uint32_t expected_crc = sb->crc;
		uint32_t calculated_crc;
		/* Calculate CRC for logging (ec_bdev_sb_check_crc already did this but we don't have access) */
		sb->crc = 0;
		calculated_crc = spdk_crc32c_update(sb, sb->length, 0);
		sb->crc = expected_crc; /* Restore original value */
		SPDK_WARNLOG("Incorrect superblock CRC on bdev %s (expected: 0x%08x, calculated: 0x%08x)\n",
			     spdk_bdev_get_name(bdev), expected_crc, calculated_crc);
		return -EINVAL;
	}

	if (sb->version.major != EC_BDEV_SB_VERSION_MAJOR) {
		SPDK_ERRLOG("Unsupported superblock major version %d (expected %d) on bdev %s\n",
			    sb->version.major, EC_BDEV_SB_VERSION_MAJOR, spdk_bdev_get_name(bdev));
		return -EINVAL;
	}

	if (sb->version.minor > EC_BDEV_SB_VERSION_MINOR) {
		SPDK_WARNLOG("Superblock minor version %d on bdev %s is higher than the currently supported: %d\n",
			     sb->version.minor, spdk_bdev_get_name(bdev), EC_BDEV_SB_VERSION_MINOR);
	}

	for (i = 0; i < sb->base_bdevs_size; i++) {
		sb_base_bdev = &sb->base_bdevs[i];
		if (sb_base_bdev->slot >= sb->num_base_bdevs) {
			SPDK_ERRLOG("Invalid superblock base bdev slot number %u (max: %u) on bdev %s\n",
				    sb_base_bdev->slot, sb->num_base_bdevs, spdk_bdev_get_name(bdev));
			return -EINVAL;
		}
	}

	/* Log successful parsing */
	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &sb->uuid);
	SPDK_DEBUGLOG(bdev_ec_sb, "Superblock parsed successfully on %s (uuid=%s, name=%s, k=%u, p=%u)\n",
		      spdk_bdev_get_name(bdev), uuid_str, sb->name, sb->k, sb->p);

	return 0;
}

static void
ec_bdev_read_sb_ctx_free(struct ec_bdev_read_sb_ctx *ctx)
{
	spdk_dma_free(ctx->buf);

	free(ctx);
}

static void ec_bdev_read_sb_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

static int
ec_bdev_read_sb_remainder(struct ec_bdev_read_sb_ctx *ctx)
{
	struct ec_bdev_superblock *sb = ctx->buf;
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(ctx->desc);
	uint32_t buf_size_prev;
	void *buf;
	int rc;

	buf_size_prev = ctx->buf_size;
	ctx->buf_size = spdk_divide_round_up(spdk_min(sb->length, EC_BDEV_SB_MAX_LENGTH),
					     spdk_bdev_get_data_block_size(bdev)) * bdev->blocklen;
	buf = spdk_dma_realloc(ctx->buf, ctx->buf_size, spdk_bdev_get_buf_align(bdev), NULL);
	if (buf == NULL) {
		SPDK_ERRLOG("Failed to reallocate buffer\n");
		return -ENOMEM;
	}
	ctx->buf = buf;

	rc = spdk_bdev_read(ctx->desc, ctx->ch, ctx->buf + buf_size_prev, buf_size_prev,
			    ctx->buf_size - buf_size_prev, ec_bdev_read_sb_cb, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to read bdev %s superblock remainder: %s\n",
			    spdk_bdev_get_name(bdev), spdk_strerror(-rc));
		return rc;
	}

	return 0;
}

static void
ec_bdev_read_sb_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct ec_bdev_read_sb_ctx *ctx = cb_arg;
	struct ec_bdev_superblock *sb = NULL;
	int status;

	if (spdk_bdev_is_md_interleaved(bdev_io->bdev) && ctx->buf_size > bdev->blocklen) {
		const uint32_t data_block_size = spdk_bdev_get_data_block_size(bdev);
		uint32_t num_blocks = ctx->buf_size / bdev->blocklen;
		uint32_t i;

		/* Use reverse iteration with memcpy to avoid overlapping issues
		 * when bdev->blocklen > data_block_size (i.e., metadata padding exists) */
		for (i = num_blocks; i > 0; i--) {
			memcpy(ctx->buf + (i - 1) * data_block_size,
			       ctx->buf + (i - 1) * bdev->blocklen,
			       data_block_size);
		}
	}

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		status = -EIO;
		goto out;
	}

	status = ec_bdev_parse_superblock(ctx);
	if (status == -EAGAIN) {
		status = ec_bdev_read_sb_remainder(ctx);
		if (status == 0) {
			return;
		}
	} else if (status != 0) {
		SPDK_DEBUGLOG(bdev_ec_sb, "failed to parse bdev %s superblock\n",
			      spdk_bdev_get_name(spdk_bdev_desc_get_bdev(ctx->desc)));
	} else {
		sb = ctx->buf;
	}
out:
	ctx->cb(sb, status, ctx->cb_ctx);

	ec_bdev_read_sb_ctx_free(ctx);
}

int
ec_bdev_load_base_bdev_superblock(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				  ec_bdev_load_sb_cb cb, void *cb_ctx)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct ec_bdev_read_sb_ctx *ctx;
	int rc;

	assert(cb != NULL);

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	ctx->desc = desc;
	ctx->ch = ch;
	ctx->cb = cb;
	ctx->cb_ctx = cb_ctx;
	ctx->buf_size = spdk_divide_round_up(sizeof(struct ec_bdev_superblock),
					     spdk_bdev_get_data_block_size(bdev)) * bdev->blocklen;
	ctx->buf = spdk_dma_malloc(ctx->buf_size, spdk_bdev_get_buf_align(bdev), NULL);
	if (!ctx->buf) {
		rc = -ENOMEM;
		goto err;
	}

	rc = spdk_bdev_read(desc, ch, ctx->buf, 0, ctx->buf_size, ec_bdev_read_sb_cb, ctx);
	if (rc) {
		goto err;
	}

	return 0;
err:
	ec_bdev_read_sb_ctx_free(ctx);

	return rc;
}

static void
ec_bdev_write_sb_base_bdev_done(int status, struct ec_bdev_write_sb_ctx *ctx)
{
	if (status != 0) {
		ctx->status = status;
	}

	if (--ctx->remaining == 0) {
		ctx->cb(ctx->status, ctx->ec_bdev, ctx->cb_ctx);
		free(ctx);
	}
}

static void
ec_bdev_write_superblock_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ec_bdev_write_sb_ctx *ctx = cb_arg;
	int status = 0;

	if (!success) {
		SPDK_ERRLOG("Failed to save superblock on bdev %s\n", bdev_io->bdev->name);
		status = -EIO;
	}

	spdk_bdev_free_io(bdev_io);

	ec_bdev_write_sb_base_bdev_done(status, ctx);
}

static void
_ec_bdev_write_superblock(void *_ctx)
{
	struct ec_bdev_write_sb_ctx *ctx = _ctx;
	struct ec_bdev *ec_bdev = ctx->ec_bdev;
	struct ec_base_bdev_info *base_info;
	uint8_t i;
	int rc;

	for (i = ctx->submitted; i < ec_bdev->num_base_bdevs; i++) {
		base_info = &ec_bdev->base_bdev_info[i];

		if (!base_info->is_configured || base_info->remove_scheduled) {
			/* Skip unconfigured or scheduled-for-removal base bdevs -
			 * decrement remaining directly */
			ctx->submitted++;
			ctx->remaining--;
			continue;
		}

		rc = spdk_bdev_write(base_info->desc, base_info->app_thread_ch,
				     ec_bdev->sb_io_buf, 0, ec_bdev->sb_io_buf_size,
				     ec_bdev_write_superblock_cb, ctx);
		if (rc != 0) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(base_info->desc);

			if (rc == -ENOMEM) {
				ctx->wait_entry.bdev = bdev;
				ctx->wait_entry.cb_fn = _ec_bdev_write_superblock;
				ctx->wait_entry.cb_arg = ctx;
				spdk_bdev_queue_io_wait(bdev, base_info->app_thread_ch, &ctx->wait_entry);
				return;
			}

			/* Write failed immediately - call done callback to decrement remaining */
			/* Note: remaining >= 1 is expected here (at least this write operation) */
			if (ctx->remaining == 0) {
				SPDK_ERRLOG("Invalid state: remaining is 0 when handling write failure\n");
				/* Still call done to ensure callback is triggered */
			}
			ec_bdev_write_sb_base_bdev_done(rc, ctx);
			/* Don't increment submitted here since the write failed immediately */
			continue;
		}

		ctx->submitted++;
	}

	/* All write operations have been submitted (or skipped for unconfigured bdevs).
	 * If all base bdevs were unconfigured, remaining may have reached 0 during the loop.
	 * Check if we need to call the final callback now. */
	if (ctx->remaining == 0) {
		ctx->cb(ctx->status, ctx->ec_bdev, ctx->cb_ctx);
		free(ctx);
	}
}

void
ec_bdev_write_superblock(struct ec_bdev *ec_bdev, ec_bdev_write_sb_cb cb, void *cb_ctx)
{
	struct ec_bdev_write_sb_ctx *ctx;
	struct ec_bdev_superblock *sb = ec_bdev->sb;
	int rc;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());
	assert(sb != NULL);
	assert(cb != NULL);

	if (ec_bdev->sb_io_buf == NULL) {
		rc = ec_bdev_alloc_sb_io_buf(ec_bdev);
		if (rc != 0) {
			goto err;
		}
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		rc = -ENOMEM;
		goto err;
	}

	ctx->ec_bdev = ec_bdev;
	ctx->remaining = ec_bdev->num_base_bdevs;
	ctx->cb = cb;
	ctx->cb_ctx = cb_ctx;

	sb->seq_number++;
	ec_bdev_sb_update_crc(sb);

	if (spdk_bdev_is_md_interleaved(&ec_bdev->bdev)) {
		void *sb_buf = sb;
		uint32_t i;

		for (i = 0; i < ec_bdev->sb_io_buf_size / ec_bdev->bdev.blocklen; i++) {
			memcpy(ec_bdev->sb_io_buf + (i * ec_bdev->bdev.blocklen),
			       sb_buf + (i * sb->block_size), sb->block_size);
		}
	}

	_ec_bdev_write_superblock(ctx);
	return;
err:
	cb(rc, ec_bdev, cb_ctx);
}

static void
ec_bdev_wipe_superblock_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ec_bdev_write_sb_ctx *ctx = cb_arg;
	int status = 0;

	/* During shutdown, IO operations may be aborted, which is expected.
	 * Don't treat aborted operations as errors during shutdown.
	 * Check if EC bdev is in OFFLINE state, which indicates shutdown/cleanup.
	 */
	if (!success) {
		if (ctx->ec_bdev != NULL && ctx->ec_bdev->state == EC_BDEV_STATE_OFFLINE) {
			/* During shutdown, aborted operations are expected */
			SPDK_DEBUGLOG(bdev_ec_sb, "Superblock wipe failed/aborted on bdev %s during shutdown\n",
				      bdev_io->bdev->name);
			status = 0;  /* Treat as success during shutdown */
		} else {
			SPDK_ERRLOG("Failed to wipe superblock on bdev %s\n", bdev_io->bdev->name);
			status = -EIO;
		}
	}

	/* Free the IO before calling the done callback, as the bdev_io might
	 * reference resources that are being cleaned up during shutdown */
	spdk_bdev_free_io(bdev_io);

	/* Check if ctx is still valid (might have been freed during shutdown) */
	if (ctx != NULL) {
		ec_bdev_write_sb_base_bdev_done(status, ctx);
	}
}

static void
_ec_bdev_wipe_superblock(void *_ctx)
{
	struct ec_bdev_write_sb_ctx *ctx = _ctx;
	struct ec_bdev *ec_bdev = ctx->ec_bdev;
	struct ec_base_bdev_info *base_info;
	uint8_t i;
	int rc;

	for (i = ctx->submitted; i < ec_bdev->num_base_bdevs; i++) {
		base_info = &ec_bdev->base_bdev_info[i];

		if (!base_info->is_configured) {
			/* Skip unconfigured base bdevs - decrement remaining directly */
			ctx->submitted++;
			ctx->remaining--;
			continue;
		}

		/* Check if desc or channel is NULL (might have been closed during shutdown) */
		if (base_info->desc == NULL || base_info->app_thread_ch == NULL) {
			SPDK_DEBUGLOG(bdev_ec_sb, "Base bdev %s desc or channel is NULL, skipping wipe\n",
				      base_info->name ? base_info->name : "unknown");
			ctx->submitted++;
			ctx->remaining--;
			continue;
		}

		rc = spdk_bdev_write(base_info->desc, base_info->app_thread_ch,
				   ec_bdev->sb_io_buf, 0, ec_bdev->sb_io_buf_size,
				   ec_bdev_wipe_superblock_cb, ctx);
		if (rc != 0) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(base_info->desc);

			if (rc == -ENOMEM) {
				ctx->wait_entry.bdev = bdev;
				ctx->wait_entry.cb_fn = _ec_bdev_wipe_superblock;
				ctx->wait_entry.cb_arg = ctx;
				spdk_bdev_queue_io_wait(bdev, base_info->app_thread_ch, &ctx->wait_entry);
				return;
			}

			/* Write failed immediately - call done callback to decrement remaining */
			/* Note: remaining >= 1 is expected here (at least this write operation) */
			if (ctx->remaining == 0) {
				SPDK_ERRLOG("Invalid state: remaining is 0 when handling wipe failure\n");
				/* Still call done to ensure callback is triggered */
			}
			ec_bdev_write_sb_base_bdev_done(rc, ctx);
			/* Don't increment submitted here since the write failed immediately */
			continue;
		}

		ctx->submitted++;
	}

	/* All write operations have been submitted (or skipped for unconfigured bdevs).
	 * If all base bdevs were unconfigured, remaining may have reached 0 during the loop.
	 * Check if we need to call the final callback now. */
	if (ctx->remaining == 0) {
		ctx->cb(ctx->status, ctx->ec_bdev, ctx->cb_ctx);
		free(ctx);
	}
}

void
ec_bdev_wipe_superblock(struct ec_bdev *ec_bdev, ec_bdev_write_sb_cb cb, void *cb_ctx)
{
	struct ec_bdev_write_sb_ctx *ctx;
	int rc;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());
	assert(cb != NULL);

	if (ec_bdev->sb_io_buf == NULL) {
		rc = ec_bdev_alloc_sb_io_buf(ec_bdev);
		if (rc != 0) {
			goto err;
		}
	}

	/* Overwrite the superblock area with zeros */
	memset(ec_bdev->sb_io_buf, 0, ec_bdev->sb_io_buf_size);

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		rc = -ENOMEM;
		goto err;
	}

	ctx->ec_bdev = ec_bdev;
	ctx->remaining = ec_bdev->num_base_bdevs;
	ctx->cb = cb;
	ctx->cb_ctx = cb_ctx;

	_ec_bdev_wipe_superblock(ctx);
	return;
err:
	cb(rc, ec_bdev, cb_ctx);
}

/*
 * Context for wiping single base bdev superblock
 */
struct ec_bdev_wipe_single_sb_ctx {
	struct ec_base_bdev_info *base_info;
	void *zero_buf;
	ec_base_bdev_cb cb;
	void *cb_ctx;
};

/*
 * Wipe superblock on a single base bdev
 * This is used when removing a failed base bdev
 */
static void
ec_bdev_wipe_single_base_bdev_sb_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ec_bdev_wipe_single_sb_ctx *ctx = cb_arg;
	int status = 0;

	if (!success) {
		/* Don't treat wipe failure as critical - disk might already be bad */
		SPDK_WARNLOG("Failed to wipe superblock on bdev %s (this is OK if disk is already failed)\n",
			     bdev_io->bdev->name);
		status = -EIO;
		/* Continue anyway - wipe failure is not critical */
	}

	spdk_bdev_free_io(bdev_io);

	/* Free the zero buffer */
	if (ctx->zero_buf != NULL) {
		spdk_dma_free(ctx->zero_buf);
	}

	/* Call user callback */
	if (ctx->cb != NULL) {
		ctx->cb(ctx->cb_ctx, status);
	}

	free(ctx);
}

int
ec_bdev_wipe_single_base_bdev_superblock(struct ec_base_bdev_info *base_info,
					  ec_base_bdev_cb cb, void *cb_ctx)
{
	struct ec_bdev *ec_bdev;
	struct ec_bdev_wipe_single_sb_ctx *ctx;
	int rc;

	if (base_info == NULL) {
		return -EINVAL;
	}

	ec_bdev = base_info->ec_bdev;
	if (ec_bdev == NULL || ec_bdev->sb_io_buf_size == 0) {
		/* No superblock to wipe */
		if (cb != NULL) {
			cb(cb_ctx, 0);
		}
		return 0;
	}

	if (base_info->desc == NULL || base_info->app_thread_ch == NULL) {
		SPDK_WARNLOG("Cannot wipe superblock - base bdev desc or channel is NULL\n");
		if (cb != NULL) {
			cb(cb_ctx, -ENODEV);
		}
		return -ENODEV;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		if (cb != NULL) {
			cb(cb_ctx, -ENOMEM);
		}
		return -ENOMEM;
	}

	ctx->base_info = base_info;
	ctx->cb = cb;
	ctx->cb_ctx = cb_ctx;

	/* Allocate zero buffer for wiping */
	ctx->zero_buf = spdk_dma_malloc(ec_bdev->sb_io_buf_size, 0x1000, NULL);
	if (ctx->zero_buf == NULL) {
		SPDK_ERRLOG("Failed to allocate buffer for wiping superblock\n");
		free(ctx);
		if (cb != NULL) {
			cb(cb_ctx, -ENOMEM);
		}
		return -ENOMEM;
	}

	memset(ctx->zero_buf, 0, ec_bdev->sb_io_buf_size);

	/* Write zeros to superblock area */
	rc = spdk_bdev_write(base_info->desc, base_info->app_thread_ch,
			     ctx->zero_buf, 0, ec_bdev->sb_io_buf_size,
			     ec_bdev_wipe_single_base_bdev_sb_cb, ctx);
	if (rc != 0) {
		if (rc == -ENOMEM) {
			/* I/O queue full - cannot wipe superblock immediately */
			SPDK_WARNLOG("I/O queue full, cannot wipe superblock immediately\n");
			spdk_dma_free(ctx->zero_buf);
			free(ctx);
			if (cb != NULL) {
				cb(cb_ctx, -ENOMEM);
			}
			return -ENOMEM;
		}

		SPDK_WARNLOG("Failed to submit superblock wipe I/O: %s\n", spdk_strerror(-rc));
		spdk_dma_free(ctx->zero_buf);
		free(ctx);
		if (cb != NULL) {
			cb(cb_ctx, rc);
		}
		return rc;
	}

	/* Buffer will be freed in callback */
	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(bdev_ec_sb)

