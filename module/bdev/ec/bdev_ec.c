/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "bdev_ec.h"
#include "spdk/bdev_module.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/env.h"


#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/likely.h"
#include <isa-l/erasure_code.h>
#include <isa-l/gf_vect_mul.h>

#define EC_OFFSET_BLOCKS_INVALID	UINT64_MAX

static bool g_shutdown_started = false;

/* List of all EC bdevs */
struct ec_all_tailq g_ec_bdev_list = TAILQ_HEAD_INITIALIZER(g_ec_bdev_list);

/*
 * ec_bdev_io_channel is the context of spdk_io_channel for EC bdev device. It
 * contains the relationship of EC bdev io channel with base bdev io channels.
 */
struct ec_bdev_io_channel {
	/* Array of IO channels of base bdevs */
	struct spdk_io_channel	**base_channel;
};

/* Forward declarations for functions used in function table - these must NOT be static */
int ec_bdev_destruct(void *ctx);
void ec_bdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);
bool ec_bdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type);
struct spdk_io_channel *ec_bdev_get_io_channel(void *ctxt);
int ec_bdev_dump_info_json(void *ctx, struct spdk_json_write_ctx *w);
int ec_bdev_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size);
void ec_bdev_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success);
void _ec_bdev_destruct(void *ctxt);
void ec_bdev_module_stop_done(struct ec_bdev *ec_bdev);
void ec_bdev_io_device_unregister_cb(void *io_device);
void ec_base_bdev_io_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
void ec_base_bdev_reset_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
int ec_bdev_init_tables(struct ec_bdev *ec_bdev, uint8_t k, uint8_t p);
void ec_bdev_cleanup_tables(struct ec_bdev *ec_bdev);
void ec_bdev_free(struct ec_bdev *ec_bdev);
void ec_bdev_free_base_bdev_resource(struct ec_base_bdev_info *base_info);
void ec_submit_reset_request(struct ec_bdev_io *ec_io);

/* Type definition for examine load superblock callback */
typedef void (*ec_bdev_examine_load_sb_cb)(struct spdk_bdev *bdev,
		const struct ec_bdev_superblock *sb, int status, void *ctx);

int ec_bdev_examine_load_sb(const char *bdev_name, ec_bdev_examine_load_sb_cb cb, void *cb_ctx);

/* Forward declarations for static functions */
static void ec_bdev_examine(struct spdk_bdev *bdev);
static void ec_bdev_examine_sb(const struct ec_bdev_superblock *sb, struct spdk_bdev *bdev,
			       ec_base_bdev_cb cb_fn, void *cb_ctx);
static int ec_bdev_init(void);
static void ec_bdev_deconfigure(struct ec_bdev *ec_bdev, ec_bdev_destruct_cb cb_fn, void *cb_arg);
static void ec_bdev_configure_cont(struct ec_bdev *ec_bdev);
static int ec_bdev_configure(struct ec_bdev *ec_bdev, ec_bdev_configure_cb cb, void *cb_ctx);
static int ec_start(struct ec_bdev *ec_bdev);
static void ec_stop(struct ec_bdev *ec_bdev);
static void ec_submit_rw_request(struct ec_bdev_io *ec_io);
static void ec_submit_null_payload_request(struct ec_bdev_io *ec_io);
static struct ec_bdev *ec_bdev_find_by_uuid(const struct spdk_uuid *uuid);
static struct ec_base_bdev_info *ec_bdev_find_base_info_by_bdev(struct spdk_bdev *base_bdev);
static int ec_bdev_create_from_sb(const struct ec_bdev_superblock *sb, struct ec_bdev **ec_bdev_out);

/* Forward declaration for module structure - will be defined at end of file */
extern struct spdk_bdev_module g_ec_if;

/* EC bdev function table */
static const struct spdk_bdev_fn_table g_ec_bdev_fn_table = {
	.destruct		= ec_bdev_destruct,
	.submit_request		= ec_bdev_submit_request,
	.io_type_supported	= ec_bdev_io_type_supported,
	.get_io_channel		= ec_bdev_get_io_channel,
	.dump_info_json		= ec_bdev_dump_info_json,
	.write_config_json	= ec_bdev_write_config_json,
	.get_memory_domains	= ec_bdev_get_memory_domains,
};

/*
 * brief:
 * ec_bdev_create_cb function is a cb function for EC bdev which creates the
 * io channel for the EC bdev by getting the io channel for all the base bdevs.
 * params:
 * io_device - pointer to EC bdev io device
 * ctx_buf - pointer to EC bdev io channel context
 * returns:
 * none
 */
static int
ec_bdev_create_cb(void *io_device, void *ctx_buf)
{
	struct ec_bdev *ec_bdev = io_device;
	struct ec_bdev_io_channel *ec_ch = ctx_buf;
	struct ec_base_bdev_info *base_info;
	uint8_t i;

	SPDK_DEBUGLOG(bdev_ec, "ec_bdev_create_cb, %p\n", ec_ch);

	ec_ch->base_channel = calloc(ec_bdev->num_base_bdevs, sizeof(struct spdk_io_channel *));
	if (ec_ch->base_channel == NULL) {
		SPDK_ERRLOG("Unable to allocate base_channel array\n");
		return -1;
	}

	for (i = 0; i < ec_bdev->num_base_bdevs; i++) {
		ec_ch->base_channel[i] = NULL;
	}

	i = 0;
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->desc != NULL) {
			ec_ch->base_channel[i] = spdk_bdev_get_io_channel(base_info->desc);
			if (ec_ch->base_channel[i] == NULL) {
				SPDK_ERRLOG("Unable to get io channel for base bdev %s\n",
					    base_info->name);
			}
		}
		i++;
	}

	return 0;
}

/*
 * brief:
 * ec_bdev_destroy_cb function is a cb function for EC bdev which deletes the
 * io channel for the EC bdev by releasing the io channel for all the base bdevs.
 * params:
 * io_device - pointer to EC bdev io device
 * ctx_buf - pointer to EC bdev io channel context
 * returns:
 * none
 */
static void
ec_bdev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct ec_bdev *ec_bdev = io_device;
	struct ec_bdev_io_channel *ec_ch = ctx_buf;
	struct ec_base_bdev_info *base_info;
	uint8_t i;

	SPDK_DEBUGLOG(bdev_ec, "ec_bdev_destroy_cb\n");

	if (ec_ch->base_channel == NULL) {
		return;
	}

	i = 0;
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (ec_ch->base_channel[i] != NULL) {
			spdk_put_io_channel(ec_ch->base_channel[i]);
			ec_ch->base_channel[i] = NULL;
		}
		i++;
	}

	free(ec_ch->base_channel);
	ec_ch->base_channel = NULL;
}

/*
 * brief:
 * ec_bdev_get_io_channel is the get_io_channel function table pointer for
 * EC bdev. This is used to return the io channel for this EC bdev
 * params:
 * ctxt - pointer to ec_bdev
 * returns:
 * pointer to io channel for EC bdev
 */
struct spdk_io_channel *
ec_bdev_get_io_channel(void *ctxt)
{
	struct ec_bdev *ec_bdev = ctxt;

	return spdk_get_io_channel(ec_bdev);
}

/*
 * brief:
 * ec_bdev_io_type_supported is the io_supported function for bdev function
 * table which returns whether the particular io type is supported or not by
 * EC bdev module
 * params:
 * ctx - pointer to EC bdev context
 * type - io type
 * returns:
 * true - io_type is supported
 * false - io_type is not supported
 */
bool
ec_bdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct ec_bdev *ec_bdev = ctx;
	struct ec_base_bdev_info *base_info;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;

	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
			if (base_info->desc == NULL) {
				continue;
			}

			if (spdk_bdev_io_type_supported(spdk_bdev_desc_get_bdev(base_info->desc), io_type) == false) {
				return false;
			}
		}
		return true;

	default:
		return false;
	}
}

/*
 * brief:
 * ec_bdev_get_memory_domains returns memory domains supported by EC bdev
 * params:
 * bdev - pointer to spdk_bdev
 * domains - pointer to array of memory domains
 * num_domains - pointer to number of domains
 * returns:
 * 0 on success
 */
int
ec_bdev_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct ec_bdev *ec_bdev = ctx;
	struct ec_base_bdev_info *base_info;
	int domains_count = 0, rc = 0;

	/* First loop to get the number of memory domains */
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
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

	/* Second loop to get the actual domains */
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
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

/*
 * brief:
 * ec_bdev_write_info_json writes EC bdev information in JSON format
 * params:
 * ec_bdev - pointer to EC bdev
 * w - pointer to json context
 * returns:
 * none
 */
void
ec_bdev_write_info_json(struct ec_bdev *ec_bdev, struct spdk_json_write_ctx *w)
{
	struct ec_base_bdev_info *base_info;

	assert(ec_bdev != NULL);
	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	spdk_json_write_named_uuid(w, "uuid", &ec_bdev->bdev.uuid);
	spdk_json_write_named_uint32(w, "strip_size_kb", ec_bdev->strip_size_kb);
	spdk_json_write_named_string(w, "state", ec_bdev_state_to_str(ec_bdev->state));
	spdk_json_write_named_uint8(w, "k", ec_bdev->k);
	spdk_json_write_named_uint8(w, "p", ec_bdev->p);
	spdk_json_write_named_bool(w, "superblock", ec_bdev->superblock_enabled);
	spdk_json_write_named_uint32(w, "num_base_bdevs", ec_bdev->num_base_bdevs);
	spdk_json_write_named_uint32(w, "num_base_bdevs_discovered", ec_bdev->num_base_bdevs_discovered);
	spdk_json_write_named_uint32(w, "num_base_bdevs_operational",
				     ec_bdev->num_base_bdevs_operational);

	spdk_json_write_name(w, "base_bdevs_list");
	spdk_json_write_array_begin(w);
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		spdk_json_write_object_begin(w);
		spdk_json_write_name(w, "name");
		if (base_info->name) {
			spdk_json_write_string(w, base_info->name);
		} else {
			spdk_json_write_null(w);
		}
		spdk_json_write_named_uuid(w, "uuid", &base_info->uuid);
		spdk_json_write_named_bool(w, "is_configured", base_info->is_configured);
		spdk_json_write_named_bool(w, "is_data_block", base_info->is_data_block);
		spdk_json_write_named_bool(w, "is_failed", base_info->is_failed);
		spdk_json_write_named_uint64(w, "data_offset", base_info->data_offset);
		spdk_json_write_named_uint64(w, "data_size", base_info->data_size);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
}

/*
 * brief:
 * ec_bdev_dump_info_json dumps the EC bdev information in JSON format
 * params:
 * bdev - pointer to spdk_bdev
 * w - pointer to json context
 * returns:
 * none
 */
int
ec_bdev_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct ec_bdev *ec_bdev = ctx;

	spdk_json_write_named_object_begin(w, "ec");
	ec_bdev_write_info_json(ec_bdev, w);
	spdk_json_write_object_end(w);

	return 0;
}

/*
 * brief:
 * ec_bdev_write_config_json is the function table pointer for EC bdev
 * params:
 * bdev - pointer to spdk_bdev
 * w - pointer to json context
 * returns:
 * none
 */
void
ec_bdev_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct ec_bdev *ec_bdev = bdev->ctxt;
	struct ec_base_bdev_info *base_info;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_ec_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uuid(w, "uuid", &ec_bdev->bdev.uuid);
	if (ec_bdev->strip_size_kb != 0) {
		spdk_json_write_named_uint32(w, "strip_size_kb", ec_bdev->strip_size_kb);
	}
	if (ec_bdev->superblock_enabled) {
		spdk_json_write_named_bool(w, "superblock", ec_bdev->superblock_enabled);
	}

	spdk_json_write_named_array_begin(w, "base_bdevs");
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->name) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "name", base_info->name);
			spdk_json_write_named_bool(w, "is_data_block", base_info->is_data_block);
			spdk_json_write_object_end(w);
		} else {
			char str[32];

			snprintf(str, sizeof(str), "removed_base_bdev_%u", 
				 (uint8_t)(base_info - ec_bdev->base_bdev_info));
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "name", str);
			spdk_json_write_named_bool(w, "is_data_block", base_info->is_data_block);
			spdk_json_write_object_end(w);
		}
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

/*
 * brief:
 * ec_bdev_io_complete completes the EC bdev IO
 * params:
 * ec_io - pointer to ec_bdev_io
 * status - status of the IO
 * returns:
 * none
 */
void
ec_bdev_io_complete(struct ec_bdev_io *ec_io, enum spdk_bdev_io_status status)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(ec_io);

	if (spdk_unlikely(ec_io->completion_cb != NULL)) {
		ec_io->completion_cb(ec_io, status);
	} else {
		spdk_bdev_io_complete(bdev_io, status);
	}
}

/*
 * brief:
 * ec_bdev_io_complete_part - signal the completion of a part of the expected
 * base bdev IOs and complete the ec_io if this is the final expected IO.
 * params:
 * ec_io - pointer to ec_bdev_io
 * completed - the part of the ec_io that has been completed
 * status - status of the base IO
 * returns:
 * true - if the ec_io is completed
 * false - otherwise
 */
bool
ec_bdev_io_complete_part(struct ec_bdev_io *ec_io, uint64_t completed,
			enum spdk_bdev_io_status status)
{
	assert(ec_io->base_bdev_io_remaining >= completed);
	ec_io->base_bdev_io_remaining -= completed;

	if (status != ec_io->base_bdev_io_status_default) {
		ec_io->base_bdev_io_status = status;
	}

	if (ec_io->base_bdev_io_remaining == 0) {
		ec_bdev_io_complete(ec_io, ec_io->base_bdev_io_status);
		return true;
	} else {
		return false;
	}
}

/*
 * brief:
 * ec_bdev_io_set_default_status sets the default status for EC bdev IO
 * params:
 * ec_io - pointer to ec_bdev_io
 * status - default status
 * returns:
 * none
 */
void
ec_bdev_io_set_default_status(struct ec_bdev_io *ec_io, enum spdk_bdev_io_status status)
{
	ec_io->base_bdev_io_status_default = status;
	ec_io->base_bdev_io_status = status;
}

/*
 * brief:
 * ec_bdev_queue_io_wait function processes the IO which failed to submit.
 * It will try to queue the IOs after storing the context to bdev wait queue logic.
 * params:
 * ec_io - pointer to ec_bdev_io
 * bdev - the block device that the IO is submitted to
 * ch - io channel
 * cb_fn - callback when the spdk_bdev_io for bdev becomes available
 * returns:
 * none
 */
void
ec_bdev_queue_io_wait(struct ec_bdev_io *ec_io, struct spdk_bdev *bdev,
		     struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn)
{
	ec_io->waitq_entry.bdev = bdev;
	ec_io->waitq_entry.cb_fn = cb_fn;
	ec_io->waitq_entry.cb_arg = ec_io;
	ec_io->waitq_entry.dep_unblock = true;

	spdk_bdev_queue_io_wait(bdev, ch, &ec_io->waitq_entry);
}

/*
 * brief:
 * ec_bdev_io_init initializes the EC bdev IO context
 * params:
 * ec_io - pointer to ec_bdev_io
 * ec_ch - pointer to ec_bdev_io_channel
 * type - IO type
 * offset_blocks - offset in blocks
 * num_blocks - number of blocks
 * iovs - pointer to iovec array
 * iovcnt - number of iovecs
 * md_buf - metadata buffer
 * memory_domain - memory domain
 * memory_domain_ctx - memory domain context
 * returns:
 * none
 */
void
ec_bdev_io_init(struct ec_bdev_io *ec_io, struct ec_bdev_io_channel *ec_ch,
	       enum spdk_bdev_io_type type, uint64_t offset_blocks,
	       uint64_t num_blocks, struct iovec *iovs, int iovcnt, void *md_buf,
	       struct spdk_memory_domain *memory_domain, void *memory_domain_ctx)
{
	struct spdk_io_channel *ch = spdk_io_channel_from_ctx(ec_ch);
	struct ec_bdev *ec_bdev = spdk_io_channel_get_io_device(ch);

	ec_io->type = type;
	ec_io->offset_blocks = offset_blocks;
	ec_io->num_blocks = num_blocks;
	ec_io->iovs = iovs;
	ec_io->iovcnt = iovcnt;
	ec_io->memory_domain = memory_domain;
	ec_io->memory_domain_ctx = memory_domain_ctx;
	ec_io->md_buf = md_buf;

	ec_io->ec_bdev = ec_bdev;
	ec_io->ec_ch = ec_ch;
	ec_io->base_bdev_io_remaining = 0;
	ec_io->base_bdev_io_submitted = 0;
	ec_io->completion_cb = NULL;

	ec_bdev_io_set_default_status(ec_io, SPDK_BDEV_IO_STATUS_SUCCESS);
}

/*
 * brief:
 * ec_bdev_submit_request function is the submit_request function pointer of
 * EC bdev function table. This is used to submit the io on ec_bdev to below
 * layers.
 * params:
 * ch - pointer to EC bdev io channel
 * bdev_io - pointer to parent bdev_io on EC bdev device
 * returns:
 * none
 */
void
ec_bdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct ec_bdev_io *ec_io = (struct ec_bdev_io *)bdev_io->driver_ctx;

	ec_bdev_io_init(ec_io, spdk_io_channel_get_ctx(ch), bdev_io->type,
			bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
			bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.md_buf,
			bdev_io->u.bdev.memory_domain, bdev_io->u.bdev.memory_domain_ctx);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, ec_bdev_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		ec_submit_rw_request(ec_io);
		break;

	case SPDK_BDEV_IO_TYPE_RESET:
		ec_submit_reset_request(ec_io);
		break;

	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		ec_submit_null_payload_request(ec_io);
		break;

	default:
		SPDK_ERRLOG("submit request, invalid io type %u\n", bdev_io->type);
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

/*
 * brief:
 * Callback function to spdk_bdev_io_get_buf.
 * params:
 * ch - pointer to EC bdev io channel
 * bdev_io - pointer to parent bdev_io on EC bdev device
 * success - True if buffer is allocated or false otherwise.
 * returns:
 * none
 */
void
ec_bdev_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		   bool success)
{
	struct ec_bdev_io *ec_io = (struct ec_bdev_io *)bdev_io->driver_ctx;

	if (!success) {
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	ec_io->iovs = bdev_io->u.bdev.iovs;
	ec_io->iovcnt = bdev_io->u.bdev.iovcnt;
	ec_io->md_buf = bdev_io->u.bdev.md_buf;

	ec_submit_rw_request(ec_io);
}

/*
 * brief:
 * ec_bdev_destruct is the destruct function table pointer for EC bdev
 * params:
 * ctx - pointer to ec_bdev
 * returns:
 * 1 - asynchronous destruction
 * 0 - synchronous destruction
 */
int
ec_bdev_destruct(void *ctx)
{
	spdk_thread_exec_msg(spdk_thread_get_app_thread(), _ec_bdev_destruct, ctx);

	return 1;
}

void
_ec_bdev_destruct(void *ctxt)
{
	struct ec_bdev *ec_bdev = ctxt;
	struct ec_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_ec, "ec_bdev_destruct\n");

	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		/*
		 * Close all base bdev descriptors for which call has come from below
		 * layers.  Also close the descriptors if we have started shutdown.
		 */
		if (g_shutdown_started || base_info->remove_scheduled == true) {
			ec_bdev_free_base_bdev_resource(base_info);
		}
	}

	if (g_shutdown_started) {
		ec_bdev->state = EC_BDEV_STATE_OFFLINE;
	}

	ec_stop(ec_bdev);

	ec_bdev_module_stop_done(ec_bdev);
}

void
ec_bdev_module_stop_done(struct ec_bdev *ec_bdev)
{
	if (ec_bdev->state != EC_BDEV_STATE_CONFIGURING) {
		spdk_io_device_unregister(ec_bdev, ec_bdev_io_device_unregister_cb);
	}
}

void
ec_bdev_io_device_unregister_cb(void *io_device)
{
	/* Nothing to do */
}

/*
 * brief:
 * ec_start is called when EC bdev is configured. It initializes the EC bdev
 * and sets up the block count and other properties.
 * params:
 * ec_bdev - pointer to EC bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
ec_start(struct ec_bdev *ec_bdev)
{
	struct ec_base_bdev_info *base_info;
	uint64_t min_data_size = UINT64_MAX;
	uint64_t data_size;
	uint32_t data_block_size;
	uint8_t k = ec_bdev->k;
	uint8_t p = ec_bdev->p;
	int rc;

	/* Find minimum data size from all base bdevs */
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->desc == NULL) {
			continue;
		}

		/* Calculate available data size considering data_offset */
		if (base_info->data_size == 0) {
			/* data_size should have been set during configure_base_bdev */
			data_size = base_info->blockcnt - base_info->data_offset;
		} else {
			data_size = base_info->data_size;
		}

		if (data_size < min_data_size) {
			min_data_size = data_size;
		}
	}

	if (min_data_size == UINT64_MAX) {
		SPDK_ERRLOG("No valid base bdevs found for EC bdev %s\n", ec_bdev->bdev.name);
		return -ENODEV;
	}

	/* EC bdev size is the minimum data size available from all base bdevs */
	/* TODO: Implement proper strip-based calculation for EC */
	ec_bdev->bdev.blockcnt = min_data_size;
	data_block_size = spdk_bdev_get_data_block_size(&ec_bdev->bdev);

	/* Convert strip_size_kb to blocks */
	ec_bdev->strip_size = (ec_bdev->strip_size_kb * 1024) / data_block_size;
	if (ec_bdev->strip_size == 0) {
		SPDK_ERRLOG("Strip size cannot be smaller than the device block size\n");
		return -EINVAL;
	}
	ec_bdev->strip_size_shift = spdk_u32log2(ec_bdev->strip_size);

	/* Initialize ISA-L tables */
	if (ec_bdev->module_private == NULL) {
		ec_bdev->module_private = calloc(1, sizeof(struct ec_bdev_module_private));
		if (ec_bdev->module_private == NULL) {
			SPDK_ERRLOG("Failed to allocate module private data\n");
			return -ENOMEM;
		}
	}

	rc = ec_bdev_init_tables(ec_bdev, k, p);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to initialize EC tables\n");
		return rc;
	}

	return 0;
}

/*
 * brief:
 * ec_stop is called when EC bdev is being stopped. It cleans up the EC bdev
 * resources.
 * params:
 * ec_bdev - pointer to EC bdev
 * returns:
 * none
 */
static void
ec_stop(struct ec_bdev *ec_bdev)
{
	if (ec_bdev->module_private != NULL) {
		ec_bdev_cleanup_tables(ec_bdev);
		free(ec_bdev->module_private);
		ec_bdev->module_private = NULL;
	}
}

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
static int __attribute__((unused))
ec_encode_stripe(struct ec_bdev *ec_bdev, unsigned char **data_ptrs,
		 unsigned char **parity_ptrs, int len)
{
	struct ec_bdev_module_private *mp = ec_bdev->module_private;
	uint8_t k = ec_bdev->k;
	uint8_t p = ec_bdev->p;

	if (mp == NULL || mp->g_tbls == NULL) {
		return -EINVAL;
	}

	/* Use ISA-L to encode data blocks into parity blocks */
	ec_encode_data(len, k, p, mp->g_tbls, data_ptrs, parity_ptrs);

	return 0;
}

/*
 * brief:
 * ec_decode_stripe decodes data stripe using ISA-L when some blocks are missing
 * params:
 * ec_bdev - pointer to EC bdev
 * frag_ptrs - array of pointers to all fragments (k+p pointers, NULL for missing)
 * frag_err_list - list of failed fragment indices
 * nerrs - number of erasures
 * recover_outp - array of pointers to output buffers for recovered fragments
 * len - length of each block in bytes
 * returns:
 * 0 on success, non-zero on failure
 */
static int __attribute__((unused))
ec_decode_stripe(struct ec_bdev *ec_bdev, unsigned char **frag_ptrs,
		 uint8_t *frag_err_list, int nerrs, unsigned char **recover_outp, int len)
{
	struct ec_bdev_module_private *mp = ec_bdev->module_private;
	unsigned char *decode_matrix = mp->decode_matrix;
	unsigned char *decode_index = mp->decode_index;
	unsigned char *g_tbls_decode;
	unsigned char *recover_srcs[EC_MAX_K];
	uint8_t k = ec_bdev->k;
	int i, rc;
	int g_tbls_decode_size = 32 * k * nerrs;

	if (mp == NULL || nerrs == 0 || nerrs > ec_bdev->p) {
		return -EINVAL;
	}

	/* Generate decode matrix */
	rc = ec_bdev_gen_decode_matrix(ec_bdev, frag_err_list, nerrs);
	if (rc != 0) {
		return rc;
	}

	/* Allocate temporary decode tables */
	g_tbls_decode = malloc(g_tbls_decode_size);
	if (g_tbls_decode == NULL) {
		return -ENOMEM;
	}

	/* Pack recovery array pointers as list of valid fragments */
	for (i = 0; i < k; i++) {
		recover_srcs[i] = frag_ptrs[decode_index[i]];
	}

	/* Initialize decode tables from decode matrix */
	ec_init_tables(k, nerrs, decode_matrix, g_tbls_decode);

	/* Recover data using ISA-L */
	ec_encode_data(len, k, nerrs, g_tbls_decode, recover_srcs, recover_outp);

	free(g_tbls_decode);

	return 0;
}

/*
 * brief:
 * ec_bdev_register_extension registers an extension interface for EC bdev
 * This allows external modules (e.g., FTL) to control I/O distribution
 * params:
 * ec_bdev - pointer to EC bdev
 * ext_if - extension interface to register
 * returns:
 * 0 on success, non-zero on failure
 */
int
ec_bdev_register_extension(struct ec_bdev *ec_bdev, struct ec_bdev_extension_if *ext_if)
{
	if (ec_bdev == NULL || ext_if == NULL) {
		return -EINVAL;
	}

	if (ec_bdev->extension_if != NULL) {
		SPDK_ERRLOG("Extension interface already registered for EC bdev %s\n",
			    ec_bdev->bdev.name);
		return -EEXIST;
	}

	if (ext_if->select_base_bdevs == NULL) {
		SPDK_ERRLOG("Extension interface must provide select_base_bdevs callback\n");
		return -EINVAL;
	}

	/* Initialize extension if init callback is provided */
	if (ext_if->init != NULL) {
		int rc = ext_if->init(ext_if, ec_bdev);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to initialize extension interface: %s\n",
				    spdk_strerror(-rc));
			return rc;
		}
	}

	ec_bdev->extension_if = ext_if;
	SPDK_NOTICELOG("Extension interface '%s' registered for EC bdev %s\n",
		       ext_if->name, ec_bdev->bdev.name);

	return 0;
}

/*
 * brief:
 * ec_bdev_unregister_extension unregisters an extension interface
 * params:
 * ec_bdev - pointer to EC bdev
 * returns:
 * none
 */
void
ec_bdev_unregister_extension(struct ec_bdev *ec_bdev)
{
	if (ec_bdev == NULL || ec_bdev->extension_if == NULL) {
		return;
	}

	/* Cleanup extension if fini callback is provided */
	if (ec_bdev->extension_if->fini != NULL) {
		ec_bdev->extension_if->fini(ec_bdev->extension_if, ec_bdev);
	}

	SPDK_NOTICELOG("Extension interface '%s' unregistered from EC bdev %s\n",
		       ec_bdev->extension_if->name, ec_bdev->bdev.name);
	ec_bdev->extension_if = NULL;
}

/*
 * brief:
 * ec_bdev_get_extension gets the registered extension interface
 * params:
 * ec_bdev - pointer to EC bdev
 * returns:
 * pointer to extension interface, or NULL if not registered
 */
struct ec_bdev_extension_if *
ec_bdev_get_extension(struct ec_bdev *ec_bdev)
{
	if (ec_bdev == NULL) {
		return NULL;
	}

	return ec_bdev->extension_if;
}

/*
 * brief:
 * ec_select_base_bdevs_default is the default base bdev selection function
 * This selects base bdevs sequentially based on their type (data/parity)
 * params:
 * ec_bdev - pointer to EC bdev
 * data_indices - output array of base bdev indices for data blocks (size k)
 * parity_indices - output array of base bdev indices for parity blocks (size p)
 * returns:
 * 0 on success, non-zero on failure
 */
static int
ec_select_base_bdevs_default(struct ec_bdev *ec_bdev, uint8_t *data_indices,
			     uint8_t *parity_indices)
{
	struct ec_base_bdev_info *base_info;
    uint8_t i = 0;
	uint8_t data_idx = 0;
	uint8_t parity_idx = 0;

	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->desc == NULL || base_info->is_failed) {
			i++;
			continue;
		}

		if (base_info->is_data_block && data_idx < ec_bdev->k) {
			data_indices[data_idx++] = i;
		} else if (!base_info->is_data_block && parity_idx < ec_bdev->p) {
			parity_indices[parity_idx++] = i;
		}
		i++;
	}

	if (data_idx < ec_bdev->k || parity_idx < ec_bdev->p) {
		return -ENODEV;
	}

	return 0;
}

/*
 * brief:
 * ec_submit_rw_request submits read/write requests to base bdevs
 * This implementation uses ISA-L for encoding/decoding
 * params:
 * ec_io - pointer to ec_bdev_io
 * returns:
 * none
 */
static void
ec_submit_rw_request(struct ec_bdev_io *ec_io)
{
	struct ec_bdev *ec_bdev = ec_io->ec_bdev;
	struct ec_base_bdev_info *base_info;
	uint8_t i;
	uint8_t num_operational = 0;
	uint8_t num_failed = 0;
	/* uint8_t failed_indices[EC_MAX_P]; - TODO: will be used when implementing decode path */
	/* uint32_t block_size = ec_bdev->bdev.blocklen; - TODO: will be used when implementing stripe-based encoding */
	/* uint64_t stripe_len_bytes; - TODO: will be used when implementing stripe-based encoding */
	/* uint64_t offset_bytes; - TODO: will be used when implementing stripe-based encoding */
	/* uint64_t total_bytes; - TODO: will be used when implementing stripe-based encoding */
	int rc;

	/* Count operational and failed base bdevs */
	i = 0;
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->desc == NULL || base_info->is_failed ||
		    ec_io->ec_ch->base_channel[i] == NULL) {
			/* TODO: Track failed_indices for decode path */
			/* if (num_failed < EC_MAX_P) { */
			/* 	failed_indices[num_failed] = i; */
			/* } */
			num_failed++;
		} else {
			num_operational++;
		}
		i++;
	}

	/* Check if we have enough operational blocks (need at least k) */
	if (num_operational < ec_bdev->k) {
		SPDK_ERRLOG("Not enough operational blocks (%u < %u) for EC bdev %s\n",
			    num_operational, ec_bdev->k, ec_bdev->bdev.name);
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	/* Calculate stripe size in bytes */
	/* For simplicity, we assume the request is aligned to stripe boundaries */
	/* TODO: Implement proper stripe alignment and partial stripe handling */
	/* stripe_len_bytes = ec_bdev->strip_size * block_size; */
	/* total_bytes = ec_io->num_blocks * block_size; */
	/* offset_bytes = ec_io->offset_blocks * block_size; */

	if (ec_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		/* Write operation: encode data and write to all blocks */
		/* unsigned char *data_ptrs[EC_MAX_K]; - TODO: will be used when implementing stripe-based encoding */
		/* unsigned char *parity_ptrs[EC_MAX_P]; - TODO: will be used when implementing stripe-based encoding */
		uint8_t data_indices[EC_MAX_K];
		uint8_t parity_indices[EC_MAX_P];
		uint8_t data_idx = 0;
		/* uint8_t parity_idx = 0; - TODO: will be used when implementing parity write */
		/* uint64_t stripe_offset; - TODO: will be used when implementing stripe-based encoding */
		/* uint32_t stripe_blocks; - TODO: will be used when implementing stripe-based encoding */
		/* uint32_t bytes_in_stripe; - TODO: will be used when implementing stripe-based encoding */
		struct ec_bdev_extension_if *ext_if = ec_bdev->extension_if;

		/* Select base bdevs using extension interface if available, otherwise use default */
		if (ext_if != NULL && ext_if->select_base_bdevs != NULL) {
			rc = ext_if->select_base_bdevs(ext_if, ec_bdev,
						      ec_io->offset_blocks,
						      ec_io->num_blocks,
						      data_indices, parity_indices,
						      ext_if->ctx);
			if (rc != 0) {
				SPDK_ERRLOG("Extension interface failed to select base bdevs: %s\n",
					    spdk_strerror(-rc));
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		} else {
			/* Use default selection */
			rc = ec_select_base_bdevs_default(ec_bdev, data_indices, parity_indices);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to select base bdevs: %s\n",
					    spdk_strerror(-rc));
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		}

		/* For write, we need to write to all k+p blocks */
		ec_io->base_bdev_io_remaining = ec_bdev->k + ec_bdev->p;
		ec_io->base_bdev_io_submitted = 0;

		/* TODO: Implement full stripe-based encoding */
		/* For now, implement a simplified version that writes data directly */
		/* and generates parity - this requires reading old data for partial writes */
		/* Full implementation will require managing stripe buffers and alignment */

		/* Write to selected data blocks */
		for (data_idx = 0; data_idx < ec_bdev->k; data_idx++) {
			i = data_indices[data_idx];
			base_info = &ec_bdev->base_bdev_info[i];
			if (base_info->desc == NULL || base_info->is_failed ||
			    ec_io->ec_ch->base_channel[i] == NULL) {
				SPDK_ERRLOG("Selected data base bdev %u is not available\n", i);
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}

			rc = spdk_bdev_write_blocks(base_info->desc,
						    ec_io->ec_ch->base_channel[i],
						    ec_io->iovs[0].iov_base,
						    ec_io->offset_blocks + base_info->data_offset,
						    ec_io->num_blocks,
						    ec_base_bdev_io_complete, ec_io);
			if (rc == 0) {
				ec_io->base_bdev_io_submitted++;
			} else if (rc == -ENOMEM) {
				ec_bdev_queue_io_wait(ec_io,
						      spdk_bdev_desc_get_bdev(base_info->desc),
						      ec_io->ec_ch->base_channel[i],
			(spdk_bdev_io_wait_cb)ec_submit_rw_request);
				return;
			} else {
				SPDK_ERRLOG("Failed to write to base bdev %s: %s\n",
					    base_info->name, spdk_strerror(-rc));
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		}

		/* TODO: Generate and write parity blocks using ec_encode_stripe */
		/* This requires reading old data for partial stripe updates */
		/* For now, we'll write data blocks and skip parity generation */
		/* Full implementation will handle stripe alignment and parity updates */
		/* When implementing, use parity_indices array to select parity base bdevs */

	} else {
		/* Read operation: read from data blocks, decode if needed */
		uint8_t data_indices[EC_MAX_K];
		uint8_t read_idx = 0;
		struct ec_bdev_extension_if *ext_if = ec_bdev->extension_if;

		/* If we have failures, we need to decode */
		if (num_failed > 0 && num_failed <= ec_bdev->p) {
			/* TODO: Implement decode path */
			/* This requires reading from k operational blocks and decoding */
			SPDK_WARNLOG("Decode path not yet fully implemented for EC bdev %s\n",
				     ec_bdev->bdev.name);
		}

		/* Select base bdevs using extension interface if available, otherwise use default */
		if (ext_if != NULL && ext_if->select_base_bdevs != NULL) {
			uint8_t parity_indices[EC_MAX_P];
			rc = ext_if->select_base_bdevs(ext_if, ec_bdev,
						      ec_io->offset_blocks,
						      ec_io->num_blocks,
						      data_indices, parity_indices,
						      ext_if->ctx);
			if (rc != 0) {
				SPDK_ERRLOG("Extension interface failed to select base bdevs: %s\n",
					    spdk_strerror(-rc));
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		} else {
			/* Use default selection */
			uint8_t parity_indices[EC_MAX_P];
			rc = ec_select_base_bdevs_default(ec_bdev, data_indices, parity_indices);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to select base bdevs: %s\n",
					    spdk_strerror(-rc));
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		}

		/* Read from selected data blocks */
		ec_io->base_bdev_io_remaining = ec_bdev->k;
		ec_io->base_bdev_io_submitted = 0;

		for (read_idx = 0; read_idx < ec_bdev->k; read_idx++) {
			i = data_indices[read_idx];
			base_info = &ec_bdev->base_bdev_info[i];
			if (base_info->desc == NULL || base_info->is_failed ||
			    ec_io->ec_ch->base_channel[i] == NULL) {
				SPDK_ERRLOG("Selected data base bdev %u is not available\n", i);
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}

			rc = spdk_bdev_read_blocks(base_info->desc,
						   ec_io->ec_ch->base_channel[i],
						   ec_io->iovs[0].iov_base,
						   ec_io->offset_blocks + base_info->data_offset,
						   ec_io->num_blocks,
						   ec_base_bdev_io_complete, ec_io);
			if (rc == 0) {
				ec_io->base_bdev_io_submitted++;
			} else if (rc == -ENOMEM) {
				ec_bdev_queue_io_wait(ec_io,
						      spdk_bdev_desc_get_bdev(base_info->desc),
						      ec_io->ec_ch->base_channel[i],
			(spdk_bdev_io_wait_cb)ec_submit_rw_request);
				return;
			} else {
				SPDK_ERRLOG("Failed to read from base bdev %s: %s\n",
					    base_info->name, spdk_strerror(-rc));
				ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		}
	}
}

/*
 * brief:
 * ec_submit_null_payload_request submits flush/unmap requests to base bdevs
 * params:
 * ec_io - pointer to ec_bdev_io
 * returns:
 * none
 */
static void
ec_submit_null_payload_request(struct ec_bdev_io *ec_io)
{
	struct ec_bdev *ec_bdev = ec_io->ec_bdev;
	struct ec_base_bdev_info *base_info;
	int rc;
	uint8_t i;

	ec_io->base_bdev_io_remaining = ec_bdev->num_base_bdevs_operational;
	ec_io->base_bdev_io_submitted = 0;

	i = 0;
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->desc == NULL || ec_io->ec_ch->base_channel[i] == NULL) {
			i++;
			ec_bdev_io_complete_part(ec_io, 1, SPDK_BDEV_IO_STATUS_SUCCESS);
			continue;
		}

		rc = spdk_bdev_flush_blocks(base_info->desc, ec_io->ec_ch->base_channel[i],
					    ec_io->offset_blocks, ec_io->num_blocks,
					    ec_base_bdev_io_complete, ec_io);
		if (rc == 0) {
			ec_io->base_bdev_io_submitted++;
		} else if (rc == -ENOMEM) {
			ec_bdev_queue_io_wait(ec_io, spdk_bdev_desc_get_bdev(base_info->desc),
					      ec_io->ec_ch->base_channel[i],
			(spdk_bdev_io_wait_cb)ec_submit_null_payload_request);
			return;
		} else {
			SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
			assert(false);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
		i++;
	}
}

/*
 * brief:
 * ec_submit_reset_request submits reset requests to all base bdevs
 * params:
 * ec_io - pointer to ec_bdev_io
 * returns:
 * none
 */
void
ec_submit_reset_request(struct ec_bdev_io *ec_io)
{
	struct ec_bdev *ec_bdev = ec_io->ec_bdev;
	struct ec_base_bdev_info *base_info;
	int rc;
	uint8_t i;

	if (ec_io->base_bdev_io_remaining == 0) {
		ec_io->base_bdev_io_remaining = ec_bdev->num_base_bdevs_operational;
	}

	i = 0;
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (ec_io->ec_ch->base_channel[i] == NULL) {
			ec_io->base_bdev_io_submitted++;
			ec_bdev_io_complete_part(ec_io, 1, SPDK_BDEV_IO_STATUS_SUCCESS);
			i++;
			continue;
		}

		rc = spdk_bdev_reset(base_info->desc, ec_io->ec_ch->base_channel[i],
				     ec_base_bdev_reset_complete, ec_io);
		if (rc == 0) {
			ec_io->base_bdev_io_submitted++;
		} else if (rc == -ENOMEM) {
			ec_bdev_queue_io_wait(ec_io, spdk_bdev_desc_get_bdev(base_info->desc),
					      ec_io->ec_ch->base_channel[i],
			(spdk_bdev_io_wait_cb)ec_submit_reset_request);
			return;
		} else {
			SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
			assert(false);
			ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
		i++;
	}
}

/*
 * brief:
 * ec_base_bdev_io_complete completes the base bdev IO
 * params:
 * bdev_io - pointer to spdk_bdev_io
 * success - success status
 * cb_arg - callback argument (ec_bdev_io)
 * returns:
 * none
 */
void
ec_base_bdev_io_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ec_bdev_io *ec_io = cb_arg;
	struct ec_bdev *ec_bdev = ec_io->ec_bdev;
	struct ec_base_bdev_info *base_info = NULL;
	struct ec_bdev_extension_if *ext_if;
	/* uint8_t i; - TODO: will be used when implementing full decode path */

	/* Find the base_info that handled this I/O */
	if (success && ec_bdev->extension_if != NULL &&
	    ec_bdev->extension_if->notify_io_complete != NULL) {
		EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
			if (base_info->desc != NULL &&
			    spdk_bdev_desc_get_bdev(base_info->desc) == bdev_io->bdev) {
				ext_if = ec_bdev->extension_if;
				ext_if->notify_io_complete(ext_if, ec_bdev, base_info,
							   ec_io->offset_blocks,
							   ec_io->num_blocks,
							   ec_io->type == SPDK_BDEV_IO_TYPE_WRITE,
							   ext_if->ctx);
				break;
			}
		}
	}

	spdk_bdev_free_io(bdev_io);

	ec_bdev_io_complete_part(ec_io, 1, success ?
				 SPDK_BDEV_IO_STATUS_SUCCESS :
				 SPDK_BDEV_IO_STATUS_FAILED);
}

/*
 * brief:
 * ec_base_bdev_reset_complete completes the base bdev reset IO
 * params:
 * bdev_io - pointer to spdk_bdev_io
 * success - success status
 * cb_arg - callback argument (ec_bdev_io)
 * returns:
 * none
 */
void
ec_base_bdev_reset_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ec_bdev_io *ec_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	ec_bdev_io_complete_part(ec_io, 1, success ?
				 SPDK_BDEV_IO_STATUS_SUCCESS :
				 SPDK_BDEV_IO_STATUS_FAILED);
}

/*
 * brief:
 * ec_bdev_find_by_name finds EC bdev by name
 * params:
 * name - EC bdev name
 * returns:
 * pointer to EC bdev if found, NULL otherwise
 */
struct ec_bdev *
ec_bdev_find_by_name(const char *name)
{
	struct ec_bdev *ec_bdev;

	TAILQ_FOREACH(ec_bdev, &g_ec_bdev_list, global_link) {
		if (strcmp(ec_bdev->bdev.name, name) == 0) {
			return ec_bdev;
		}
	}

	return NULL;
}

/*
 * brief:
 * ec_bdev_find_by_uuid finds EC bdev by UUID
 * params:
 * uuid - EC bdev UUID
 * returns:
 * pointer to EC bdev if found, NULL otherwise
 */
static struct ec_bdev *
ec_bdev_find_by_uuid(const struct spdk_uuid *uuid)
{
	struct ec_bdev *ec_bdev;

	TAILQ_FOREACH(ec_bdev, &g_ec_bdev_list, global_link) {
		if (spdk_uuid_compare(&ec_bdev->bdev.uuid, uuid) == 0) {
			return ec_bdev;
		}
	}

	return NULL;
}

/*
 * brief:
 * ec_bdev_find_base_info_by_bdev finds base bdev info by bdev
 * params:
 * base_bdev - pointer to base bdev
 * returns:
 * base bdev info if found, otherwise NULL
 */
static struct ec_base_bdev_info *
ec_bdev_find_base_info_by_bdev(struct spdk_bdev *base_bdev)
{
	struct ec_bdev *ec_bdev;
	struct ec_base_bdev_info *base_info;

	TAILQ_FOREACH(ec_bdev, &g_ec_bdev_list, global_link) {
		EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
			if (base_info->desc != NULL &&
			    spdk_bdev_desc_get_bdev(base_info->desc) == base_bdev) {
				return base_info;
			}
		}
	}

	return NULL;
}

/*
 * State names for EC bdev
 */
const char *g_ec_state_names[] = {
	[EC_BDEV_STATE_ONLINE]	= "online",
	[EC_BDEV_STATE_CONFIGURING]	= "configuring",
	[EC_BDEV_STATE_OFFLINE]	= "offline",
	[EC_BDEV_STATE_MAX]		= NULL
};

enum ec_bdev_state
ec_bdev_str_to_state(const char *str)
{
	unsigned int i;

	assert(str != NULL);

	for (i = 0; i < EC_BDEV_STATE_MAX; i++) {
		if (strcasecmp(g_ec_state_names[i], str) == 0) {
			break;
		}
	}

	return i;
}

const char *
ec_bdev_state_to_str(enum ec_bdev_state state)
{
	if (state >= EC_BDEV_STATE_MAX) {
		return "";
	}

	return g_ec_state_names[state];
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

/*
 * brief:
 * ec_bdev_deconfigure deconfigures an EC bdev
 * If EC bdev is online and registered, change the bdev state to
 * configuring and unregister this EC device
 * params:
 * ec_bdev - pointer to EC bdev
 * cb_fn - callback function
 * cb_arg - argument to callback function
 * returns:
 * none
 */
static void
ec_bdev_deconfigure_unregister_done(void *cb_arg, int rc)
{
	struct ec_bdev *ec_bdev = cb_arg;
	ec_bdev_destruct_cb user_cb_fn = ec_bdev->deconfigure_cb_fn;
	void *user_cb_arg = ec_bdev->deconfigure_cb_arg;

	/* Clear the stored callbacks */
	ec_bdev->deconfigure_cb_fn = NULL;
	ec_bdev->deconfigure_cb_arg = NULL;

	/* Close self_desc before calling user callback */
	if (ec_bdev->self_desc != NULL) {
		spdk_bdev_close(ec_bdev->self_desc);
		ec_bdev->self_desc = NULL;
	}

	/* Call user callback */
	if (user_cb_fn) {
		user_cb_fn(user_cb_arg, rc);
	}
}

static void
ec_bdev_deconfigure(struct ec_bdev *ec_bdev, ec_bdev_destruct_cb cb_fn, void *cb_arg)
{
	if (ec_bdev->state != EC_BDEV_STATE_ONLINE) {
		if (cb_fn) {
			cb_fn(cb_arg, 0);
		}
		return;
	}

	ec_bdev->state = EC_BDEV_STATE_OFFLINE;
	SPDK_DEBUGLOG(bdev_ec, "EC bdev state changing from online to offline\n");

	/* Store user callbacks to call them after unregister completes */
	ec_bdev->deconfigure_cb_fn = cb_fn;
	ec_bdev->deconfigure_cb_arg = cb_arg;

	/* Use wrapper callback that will close self_desc and then call user callback */
	spdk_bdev_unregister(&ec_bdev->bdev, ec_bdev_deconfigure_unregister_done, ec_bdev);
}

/*
 * brief:
 * ec_bdev_delete_continue continues the deletion process after superblock wiping
 * params:
 * ec_bdev - pointer to EC bdev
 * cb_fn - callback function
 * cb_arg - argument to callback function
 * returns:
 * none
 */
static void
ec_bdev_delete_continue(struct ec_bdev *ec_bdev, ec_bdev_destruct_cb cb_fn, void *cb_arg)
{
	struct ec_base_bdev_info *base_info;

	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		base_info->remove_scheduled = true;

		if (ec_bdev->state != EC_BDEV_STATE_ONLINE) {
			/*
			 * As EC bdev is not registered yet or already unregistered,
			 * so cleanup should be done here itself.
			 */
			ec_bdev_free_base_bdev_resource(base_info);
		}
	}

	if (ec_bdev->num_base_bdevs_discovered == 0) {
		/* There is no base bdev for this EC, so free the EC device. */
		TAILQ_REMOVE(&g_ec_bdev_list, ec_bdev, global_link);
		ec_bdev_free(ec_bdev);
		if (cb_fn) {
			cb_fn(cb_arg, 0);
		}
	} else {
		ec_bdev_deconfigure(ec_bdev, cb_fn, cb_arg);
	}
}

/*
 * brief:
 * Context structure for EC bdev deletion
 */
struct ec_bdev_delete_ctx {
	struct ec_bdev *ec_bdev;
	ec_bdev_destruct_cb cb_fn;
	void *cb_arg;
	bool abort_delete;
};

/*
 * brief:
 * ec_bdev_delete_on_unquiesced is called after unquiescing during deletion
 * params:
 * arg - deletion context
 * status - status of unquiesce operation
 * returns:
 * none
 */
static void
ec_bdev_delete_on_unquiesced(void *arg, int status)
{
	struct ec_bdev_delete_ctx *ctx = arg;
	struct ec_bdev *ec_bdev = ctx->ec_bdev;

	if (status != 0) {
		SPDK_ERRLOG("Failed to unquiesce EC bdev %s: %s\n",
			    ec_bdev->bdev.name, spdk_strerror(-status));
	}

	if (ctx->abort_delete) {
		/* Restore state and stop with error */
		ec_bdev->destroy_started = false;
		if (ctx->cb_fn) {
			ctx->cb_fn(ctx->cb_arg, status != 0 ? status : -EIO);
		}
		free(ctx);
		return;
	}

	ec_bdev_delete_continue(ec_bdev, ctx->cb_fn, ctx->cb_arg);
	free(ctx);
}

/*
 * brief:
 * ec_bdev_delete_restore_sb_cb is called when attempting to restore superblock after failed wipe
 * params:
 * restore_status - status of restore operation
 * ec_bdev - pointer to EC bdev
 * arg - deletion context
 * returns:
 * none
 */
static void
ec_bdev_delete_restore_sb_cb(int restore_status, struct ec_bdev *ec_bdev, void *arg)
{
	struct ec_bdev_delete_ctx *ctx = arg;
	(void)restore_status; /* We will abort regardless, but we tried to restore. */
	ctx->abort_delete = true;
	spdk_bdev_unquiesce(&ec_bdev->bdev, &g_ec_if, ec_bdev_delete_on_unquiesced, ctx);
}

/*
 * brief:
 * ec_bdev_delete_wipe_cb is called after wiping superblock during deletion
 * params:
 * status - status of wipe operation
 * ec_bdev - pointer to EC bdev
 * arg - deletion context
 * returns:
 * none
 */
static void
ec_bdev_delete_wipe_cb(int status, struct ec_bdev *ec_bdev, void *arg)
{
	struct ec_bdev_delete_ctx *ctx = arg;

	if (status != 0) {
		SPDK_ERRLOG("Failed to wipe EC bdev '%s' superblock: %s\n",
			    ec_bdev->bdev.name, spdk_strerror(-status));
		/* Attempt to restore original SB before aborting */
		ec_bdev_write_superblock(ec_bdev, ec_bdev_delete_restore_sb_cb, ctx);
		return;
	}

	spdk_bdev_unquiesce(&ec_bdev->bdev, &g_ec_if, ec_bdev_delete_on_unquiesced, ctx);
}

/*
 * brief:
 * ec_bdev_delete_on_quiesced is called after quiescing during deletion
 * params:
 * arg - deletion context
 * status - status of quiesce operation
 * returns:
 * none
 */
static void
ec_bdev_delete_on_quiesced(void *arg, int status)
{
	struct ec_bdev_delete_ctx *ctx = arg;
	struct ec_bdev *ec_bdev = ctx->ec_bdev;

	if (status != 0) {
		SPDK_ERRLOG("Failed to quiesce EC bdev %s: %s\n",
			    ec_bdev->bdev.name, spdk_strerror(-status));
		/* Abort: restore state and return error */
		ec_bdev->destroy_started = false;
		if (ctx->cb_fn) {
			ctx->cb_fn(ctx->cb_arg, status);
		}
		free(ctx);
		return;
	}

	/* Now wipe the superblock only on this EC's base devices */
	ec_bdev_wipe_superblock(ec_bdev, ec_bdev_delete_wipe_cb, ctx);
}

/*
 * brief:
 * ec_bdev_delete deletes an EC bdev
 * params:
 * ec_bdev - pointer to EC bdev
 * cb_fn - callback function
 * cb_ctx - argument to callback function
 * returns:
 * none
 */
void
ec_bdev_delete(struct ec_bdev *ec_bdev, ec_bdev_destruct_cb cb_fn, void *cb_ctx)
{
	SPDK_DEBUGLOG(bdev_ec, "delete EC bdev: %s\n", ec_bdev->bdev.name);

	if (ec_bdev->destroy_started) {
		SPDK_DEBUGLOG(bdev_ec, "destroying EC bdev %s is already started\n",
			      ec_bdev->bdev.name);
		if (cb_fn) {
			cb_fn(cb_ctx, -EALREADY);
		}
		return;
	}

	ec_bdev->destroy_started = true;

	/* If superblock is enabled and the EC is online with discovered bases, quiesce and wipe SB first */
	if (ec_bdev->superblock_enabled && ec_bdev->state == EC_BDEV_STATE_ONLINE &&
	    ec_bdev->num_base_bdevs_discovered > 0) {
		struct ec_bdev_delete_ctx *ctx = calloc(1, sizeof(*ctx));

		if (ctx != NULL) {
			int rc;
			ctx->ec_bdev = ec_bdev;
			ctx->cb_fn = cb_fn;
			ctx->cb_arg = cb_ctx;
			ctx->abort_delete = false;

			rc = spdk_bdev_quiesce(&ec_bdev->bdev, &g_ec_if, ec_bdev_delete_on_quiesced, ctx);
			if (rc == 0) {
				return;
			}

			/* If quiesce fails, abort deletion and restore state */
			free(ctx);
			ec_bdev->destroy_started = false;
			if (cb_fn) {
				cb_fn(cb_ctx, rc);
			}
			return;
		}
	}

	ec_bdev_delete_continue(ec_bdev, cb_fn, cb_ctx);
}

/*
 * brief:
 * ec_bdev_free frees the EC bdev structure
 * params:
 * ec_bdev - pointer to EC bdev
 * returns:
 * none
 */
void
ec_bdev_free(struct ec_bdev *ec_bdev)
{
	struct ec_base_bdev_info *base_info;

	/* Unregister extension interface if registered */
	if (ec_bdev->extension_if != NULL) {
		ec_bdev_unregister_extension(ec_bdev);
	}

	/* Free all base_info names before freeing the array */
	if (ec_bdev->base_bdev_info != NULL) {
		EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
			if (base_info->name != NULL) {
				free(base_info->name);
				base_info->name = NULL;
			}
		}
	}

	ec_bdev_free_superblock(ec_bdev);
	free(ec_bdev->base_bdev_info);
	free(ec_bdev->bdev.name);
	free(ec_bdev);
}

/*
 * brief:
 * ec_bdev_create allocates EC bdev based on passed configuration
 * params:
 * name - name for EC bdev
 * strip_size - strip size in KB
 * k - number of data blocks
 * p - number of parity blocks
 * superblock_enabled - true if EC should have superblock
 * uuid - uuid to set for the bdev
 * ec_bdev_out - the created EC bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
int
ec_bdev_create(const char *name, uint32_t strip_size, uint8_t k, uint8_t p,
	       bool superblock_enabled, const struct spdk_uuid *uuid,
	       struct ec_bdev **ec_bdev_out)
{
	struct ec_bdev *ec_bdev;
	struct spdk_bdev *ec_bdev_gen;
	struct ec_base_bdev_info *base_info;
	uint8_t num_base_bdevs = k + p;
	uint8_t min_operational;

	assert(uuid != NULL);

	/* Validate name length */
	if (strnlen(name, EC_BDEV_SB_NAME_SIZE) == EC_BDEV_SB_NAME_SIZE) {
		SPDK_ERRLOG("EC bdev name '%s' exceeds %d characters\n", name, EC_BDEV_SB_NAME_SIZE - 1);
		return -EINVAL;
	}

	/* Check for duplicate name */
	if (ec_bdev_find_by_name(name) != NULL) {
		SPDK_ERRLOG("Duplicate EC bdev name found: %s\n", name);
		return -EEXIST;
	}

	/* Validate strip size - must be power of 2 */
	if (strip_size != 0 && spdk_u32_is_pow2(strip_size) == false) {
		SPDK_ERRLOG("Invalid strip size %" PRIu32 "\n", strip_size);
		return -EINVAL;
	}

	/* Validate k and p */
	if (k == 0 || p == 0) {
		SPDK_ERRLOG("Must have at least one data block (k) and one parity block (p)\n");
		return -EINVAL;
	}

	/* Note: k and p are uint8_t, so they cannot exceed 255 (EC_MAX_K/EC_MAX_P) */
	/* Additional validation is done by checking k+p <= num_base_bdevs */

	/* For EC, min_operational is k (need at least k blocks to decode) */
	min_operational = k;

	if (min_operational == 0 || min_operational > num_base_bdevs) {
		SPDK_ERRLOG("Wrong min_operational value for EC bdev\n");
		return -EINVAL;
	}

	/* Allocate EC bdev structure */
	ec_bdev = calloc(1, sizeof(*ec_bdev));
	if (!ec_bdev) {
		SPDK_ERRLOG("Unable to allocate memory for EC bdev\n");
		return -ENOMEM;
	}

	/* Allocate base bdev info array */
	ec_bdev->base_bdev_info = calloc(num_base_bdevs, sizeof(struct ec_base_bdev_info));
	if (!ec_bdev->base_bdev_info) {
		SPDK_ERRLOG("Unable to allocate base bdev info\n");
		ec_bdev_free(ec_bdev);
		return -ENOMEM;
	}

	/* Initialize EC bdev fields (set num_base_bdevs BEFORE iterating slots) */
	ec_bdev->strip_size = 0;
	ec_bdev->strip_size_kb = strip_size;
	ec_bdev->state = EC_BDEV_STATE_CONFIGURING;
	ec_bdev->num_base_bdevs = num_base_bdevs;

	/* Initialize base bdev info */
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		base_info->ec_bdev = ec_bdev;
	}
	ec_bdev->k = k;
	ec_bdev->p = p;
	ec_bdev->min_base_bdevs_operational = min_operational;
	ec_bdev->superblock_enabled = superblock_enabled;

	ec_bdev_gen = &ec_bdev->bdev;

	/* Set bdev name */
	ec_bdev_gen->name = strdup(name);
	if (!ec_bdev_gen->name) {
		SPDK_ERRLOG("Unable to allocate name for EC bdev\n");
		ec_bdev_free(ec_bdev);
		return -ENOMEM;
	}

	ec_bdev_gen->product_name = "EC Volume";
	ec_bdev_gen->ctxt = ec_bdev;
	ec_bdev_gen->fn_table = &g_ec_bdev_fn_table;
	ec_bdev_gen->module = &g_ec_if;
	ec_bdev_gen->write_cache = 0;
	spdk_uuid_copy(&ec_bdev_gen->uuid, uuid);

	/* If superblock is enabled and UUID is null, generate one */
	if (superblock_enabled && spdk_uuid_is_null(uuid)) {
		spdk_uuid_generate(&ec_bdev_gen->uuid);
	}

	/* Insert into global list */
	TAILQ_INSERT_TAIL(&g_ec_bdev_list, ec_bdev, global_link);

	ec_bdev->num_base_bdevs_operational = num_base_bdevs;

	*ec_bdev_out = ec_bdev;

	return 0;
}

/*
 * brief:
 * ec_bdev_deconfigure_base_bdev deconfigures a base bdev
 * params:
 * base_info - EC base bdev info
 * returns:
 * none
 */
static void
ec_bdev_deconfigure_base_bdev(struct ec_base_bdev_info *base_info)
{
	struct ec_bdev *ec_bdev = base_info->ec_bdev;

	assert(base_info->is_configured);
	assert(ec_bdev->num_base_bdevs_discovered);
	ec_bdev->num_base_bdevs_discovered--;
	base_info->is_configured = false;
}

/*
 * brief:
 * ec_bdev_free_base_bdev_resource frees resource of base bdev for EC bdev
 * params:
 * base_info - EC base bdev info
 * returns:
 * none
 */
void
ec_bdev_free_base_bdev_resource(struct ec_base_bdev_info *base_info)
{
	struct ec_bdev *ec_bdev = base_info->ec_bdev;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());
	assert(base_info->configure_cb == NULL);

	free(base_info->name);
	base_info->name = NULL;
	if (ec_bdev->state != EC_BDEV_STATE_CONFIGURING) {
		spdk_uuid_set_null(&base_info->uuid);
	}
	base_info->is_failed = false;

	/* clear `data_offset` to allow it to be recalculated during configuration */
	base_info->data_offset = 0;

	if (base_info->desc == NULL) {
		return;
	}

	spdk_bdev_module_release_bdev(spdk_bdev_desc_get_bdev(base_info->desc));
	spdk_bdev_close(base_info->desc);
	base_info->desc = NULL;
	spdk_put_io_channel(base_info->app_thread_ch);
	base_info->app_thread_ch = NULL;

	if (base_info->is_configured) {
		ec_bdev_deconfigure_base_bdev(base_info);
	}
}

/*
 * brief:
 * ec_bdev_remove_base_bdev_done is called when base bdev removal is complete
 * params:
 * base_info - EC base bdev info
 * status - status of the removal operation
 * returns:
 * none
 */
static void
ec_bdev_remove_base_bdev_done(struct ec_base_bdev_info *base_info, int status)
{
	struct ec_bdev *ec_bdev = base_info->ec_bdev;

	assert(base_info->remove_scheduled);
	base_info->remove_scheduled = false;

	if (status == 0) {
		ec_bdev->num_base_bdevs_operational--;
		if (ec_bdev->num_base_bdevs_operational < ec_bdev->min_base_bdevs_operational) {
			/* There is not enough base bdevs to keep the EC bdev operational. */
			ec_bdev_deconfigure(ec_bdev, base_info->remove_cb, base_info->remove_cb_ctx);
			return;
		}
	}

	if (base_info->remove_cb != NULL) {
		base_info->remove_cb(base_info->remove_cb_ctx, status);
	}
}

/*
 * brief:
 * ec_bdev_channel_remove_base_bdev removes base bdev from a channel
 * params:
 * i - io channel iterator
 * returns:
 * none
 */
static void
ec_bdev_channel_remove_base_bdev(struct spdk_io_channel_iter *i)
{
	struct ec_base_bdev_info *base_info = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct ec_bdev_io_channel *ec_ch = spdk_io_channel_get_ctx(ch);
	uint8_t idx = (uint8_t)(base_info - base_info->ec_bdev->base_bdev_info);

	SPDK_DEBUGLOG(bdev_ec, "slot: %u ec_ch: %p\n", idx, ec_ch);

	if (ec_ch->base_channel != NULL && ec_ch->base_channel[idx] != NULL) {
		spdk_put_io_channel(ec_ch->base_channel[idx]);
		ec_ch->base_channel[idx] = NULL;
	}

	spdk_for_each_channel_continue(i, 0);
}

/*
 * brief:
 * ec_bdev_channels_remove_base_bdev_done is called after removing base bdev from all channels
 * params:
 * i - io channel iterator
 * status - status of the operation
 * returns:
 * none
 */
static void
ec_bdev_channels_remove_base_bdev_done(struct spdk_io_channel_iter *i, int status)
{
	struct ec_base_bdev_info *base_info = spdk_io_channel_iter_get_ctx(i);
	struct ec_bdev *ec_bdev = base_info->ec_bdev;

	ec_bdev_free_base_bdev_resource(base_info);

	/* Update superblock if enabled */
	if (ec_bdev->superblock_enabled && ec_bdev->sb != NULL) {
		ec_bdev_init_superblock(ec_bdev);
		ec_bdev_write_superblock(ec_bdev, NULL, NULL);
	}

	ec_bdev_remove_base_bdev_done(base_info, status);
}

/*
 * brief:
 * ec_bdev_remove_base_bdev_do_remove performs the actual removal of base bdev
 * params:
 * base_info - EC base bdev info
 * returns:
 * none
 */
static void
ec_bdev_remove_base_bdev_do_remove(struct ec_base_bdev_info *base_info)
{
	ec_bdev_deconfigure_base_bdev(base_info);

	spdk_for_each_channel(base_info->ec_bdev, ec_bdev_channel_remove_base_bdev, base_info,
			      ec_bdev_channels_remove_base_bdev_done);
}

/*
 * brief:
 * ec_bdev_remove_base_bdev_reset_done is called after resetting base bdev before removal
 * params:
 * bdev_io - bdev io that was reset
 * success - whether reset was successful
 * cb_arg - callback argument (base_info)
 * returns:
 * none
 */
static void
ec_bdev_remove_base_bdev_reset_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ec_base_bdev_info *base_info = cb_arg;

	spdk_bdev_free_io(bdev_io);

	ec_bdev_remove_base_bdev_do_remove(base_info);
}

/*
 * brief:
 * ec_bdev_remove_base_bdev_cont continues base bdev removal after quiesce
 * params:
 * base_info - EC base bdev info
 * returns:
 * none
 */
static void
ec_bdev_remove_base_bdev_cont(struct ec_base_bdev_info *base_info)
{
	int rc;

	rc = spdk_bdev_reset(base_info->desc, base_info->app_thread_ch,
			     ec_bdev_remove_base_bdev_reset_done, base_info);
	if (rc != 0) {
		SPDK_WARNLOG("Reset base bdev '%s' before removal failed: %s\n",
			     base_info->name, spdk_strerror(-rc));
		/* Proceed with removal even if reset submission failed */
		ec_bdev_remove_base_bdev_do_remove(base_info);
	}
}

/*
 * brief:
 * ec_bdev_remove_base_bdev_on_unquiesced is called after unquiescing EC bdev
 * params:
 * ctx - callback context (base_info)
 * status - status of unquiesce operation
 * returns:
 * none
 */
static void __attribute__((unused))
ec_bdev_remove_base_bdev_on_unquiesced(void *ctx, int status)
{
	struct ec_base_bdev_info *base_info = ctx;
	struct ec_bdev *ec_bdev = base_info->ec_bdev;

	if (status != 0) {
		SPDK_ERRLOG("Failed to unquiesce EC bdev %s: %s\n",
			    ec_bdev->bdev.name, spdk_strerror(-status));
	}

	ec_bdev_remove_base_bdev_done(base_info, status);
}

/*
 * brief:
 * ec_bdev_remove_base_bdev_on_quiesced is called after quiescing EC bdev
 * params:
 * ctx - callback context (base_info)
 * status - status of quiesce operation
 * returns:
 * none
 */
static void
ec_bdev_remove_base_bdev_on_quiesced(void *ctx, int status)
{
	struct ec_base_bdev_info *base_info = ctx;
	struct ec_bdev *ec_bdev = base_info->ec_bdev;

	if (status != 0) {
		SPDK_ERRLOG("Failed to quiesce EC bdev %s: %s\n",
			    ec_bdev->bdev.name, spdk_strerror(-status));
		ec_bdev_remove_base_bdev_done(base_info, status);
		return;
	}

	ec_bdev_remove_base_bdev_cont(base_info);
}

/*
 * brief:
 * ec_bdev_remove_base_bdev_quiesce quiesces the EC bdev before removing base bdev
 * params:
 * base_info - EC base bdev info
 * returns:
 * 0 on success, non-zero on failure
 */
static int
ec_bdev_remove_base_bdev_quiesce(struct ec_base_bdev_info *base_info)
{
	struct ec_bdev *ec_bdev = base_info->ec_bdev;
	int ret;

	ret = spdk_bdev_quiesce(&ec_bdev->bdev, &g_ec_if,
				ec_bdev_remove_base_bdev_on_quiesced, base_info);
	if (ret != 0) {
		SPDK_ERRLOG("Failed to quiesce EC bdev %s: %s\n",
			    ec_bdev->bdev.name, spdk_strerror(-ret));
		return ret;
	}

	return 0;
}

/*
 * brief:
 * _ec_bdev_remove_base_bdev is the internal function to remove a base bdev
 * params:
 * base_info - EC base bdev info
 * cb_fn - callback function
 * cb_ctx - callback context
 * returns:
 * 0 on success, non-zero on failure
 */
static int
_ec_bdev_remove_base_bdev(struct ec_base_bdev_info *base_info,
			  ec_base_bdev_cb cb_fn, void *cb_ctx)
{
	struct ec_bdev *ec_bdev = base_info->ec_bdev;
	int ret = 0;

	SPDK_DEBUGLOG(bdev_ec, "%s\n", base_info->name);

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	if (base_info->remove_scheduled || !base_info->is_configured) {
		return -ENODEV;
	}

	assert(base_info->desc);
	base_info->remove_scheduled = true;

	if (ec_bdev->state != EC_BDEV_STATE_ONLINE) {
		/*
		 * As EC bdev is not registered yet or already unregistered,
		 * so cleanup should be done here itself.
		 *
		 * Removing a base bdev at this stage does not change the number of operational
		 * base bdevs, only the number of discovered base bdevs.
		 */
		ec_bdev_free_base_bdev_resource(base_info);
		base_info->remove_scheduled = false;
		if (ec_bdev->num_base_bdevs_discovered == 0 &&
		    ec_bdev->state == EC_BDEV_STATE_OFFLINE) {
			/* There is no base bdev for this EC, so free the EC device. */
			ec_bdev_free(ec_bdev);
		}
		if (cb_fn != NULL) {
			cb_fn(cb_ctx, 0);
		}
	} else if (ec_bdev->min_base_bdevs_operational == ec_bdev->num_base_bdevs) {
		/* This EC bdev does not tolerate removing a base bdev. */
		ec_bdev->num_base_bdevs_operational--;
		ec_bdev_deconfigure(ec_bdev, cb_fn, cb_ctx);
	} else {
		base_info->remove_cb = cb_fn;
		base_info->remove_cb_ctx = cb_ctx;

		ret = ec_bdev_remove_base_bdev_quiesce(base_info);

		if (ret != 0) {
			base_info->remove_scheduled = false;
		}
	}

	return ret;
}

/*
 * brief:
 * ec_bdev_remove_base_bdev function is called by below layers when base_bdev
 * is removed. This function checks if this base bdev is part of any EC bdev
 * or not. If yes, it takes necessary action on that particular EC bdev.
 * params:
 * base_bdev - pointer to base bdev which got removed
 * cb_fn - callback function
 * cb_ctx - argument to callback function
 * returns:
 * 0 - success
 * non zero - failure
 */
int
ec_bdev_remove_base_bdev(struct spdk_bdev *base_bdev, ec_base_bdev_cb cb_fn, void *cb_ctx)
{
	struct ec_base_bdev_info *base_info;

	/* Find the ec_bdev which has claimed this base_bdev */
	base_info = ec_bdev_find_base_info_by_bdev(base_bdev);
	if (!base_info) {
		SPDK_ERRLOG("bdev to remove '%s' not found\n", base_bdev->name);
		return -ENODEV;
	}

	return _ec_bdev_remove_base_bdev(base_info, cb_fn, cb_ctx);
}

/*
 * brief:
 * ec_bdev_event_base_bdev function is called by below layers when base_bdev
 * triggers asynchronous event.
 * params:
 * type - event details.
 * bdev - bdev that triggered event.
 * event_ctx - context for event.
 * returns:
 * none
 */
static void
ec_bdev_event_base_bdev(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		       void *event_ctx)
{
	struct ec_base_bdev_info *base_info = event_ctx;
	struct ec_bdev *ec_bdev = base_info->ec_bdev;
	int rc;

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		rc = ec_bdev_remove_base_bdev(bdev, NULL, NULL);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to remove base bdev %s: %s\n",
				    spdk_bdev_get_name(bdev), spdk_strerror(-rc));
		}
		break;
	case SPDK_BDEV_EVENT_RESIZE:
		/* TODO: Implement resize handling for EC bdev */
		SPDK_NOTICELOG("Resize event for base bdev %s in EC bdev %s\n",
			       bdev->name, ec_bdev->bdev.name);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

/*
 * brief:
 * ec_bdev_configure_base_bdev_cont continues the configuration of a base bdev
 * after sync across all channels
 * params:
 * base_info - EC base bdev info
 * returns:
 * none
 */
static void
ec_bdev_configure_base_bdev_cont(struct ec_base_bdev_info *base_info)
{
	struct ec_bdev *ec_bdev = base_info->ec_bdev;
	ec_base_bdev_cb configure_cb;
	int rc;

	base_info->is_configured = true;

	ec_bdev->num_base_bdevs_discovered++;
	assert(ec_bdev->num_base_bdevs_discovered <= ec_bdev->num_base_bdevs);
	assert(ec_bdev->num_base_bdevs_operational <= ec_bdev->num_base_bdevs);
	assert(ec_bdev->num_base_bdevs_operational >= ec_bdev->min_base_bdevs_operational);

	configure_cb = base_info->configure_cb;
	base_info->configure_cb = NULL;

	/*
	 * Configure the EC bdev when the number of discovered base bdevs reaches the number
	 * of base bdevs we know to be operational members of the array.
	 */
	if (ec_bdev->num_base_bdevs_discovered == ec_bdev->num_base_bdevs_operational) {
		rc = ec_bdev_configure(ec_bdev, configure_cb, base_info->configure_cb_ctx);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to configure EC bdev: %s\n", spdk_strerror(-rc));
		} else {
			configure_cb = NULL;
		}
	} else {
		rc = 0;
	}

	if (configure_cb != NULL) {
		configure_cb(base_info->configure_cb_ctx, rc);
	}
}

static void __attribute__((unused))
ec_bdev_ch_sync(struct spdk_io_channel_iter *i)
{
	spdk_for_each_channel_continue(i, 0);
}

static void __attribute__((unused))
_ec_bdev_configure_base_bdev_cont(struct spdk_io_channel_iter *i, int status)
{
	struct ec_base_bdev_info *base_info = spdk_io_channel_iter_get_ctx(i);

	ec_bdev_configure_base_bdev_cont(base_info);
}

/*
 * brief:
 * ec_bdev_configure_base_bdev_check_sb_cb checks superblock when configuring a new base bdev
 * params:
 * sb - superblock found (or NULL if not found)
 * status - status of superblock load
 * ctx - base_info context
 * returns:
 * none
 */
static void
ec_bdev_configure_base_bdev_check_sb_cb(const struct ec_bdev_superblock *sb, int status,
		void *ctx)
{
	struct ec_base_bdev_info *base_info = ctx;
	ec_base_bdev_cb configure_cb = base_info->configure_cb;

	switch (status) {
	case 0:
		/* valid superblock found */
		base_info->configure_cb = NULL;
		if (spdk_uuid_compare(&base_info->ec_bdev->bdev.uuid, &sb->uuid) == 0) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(base_info->desc);

			ec_bdev_free_base_bdev_resource(base_info);
			/* Use examine_sb to handle this case - it will configure the base bdev */
			ec_bdev_examine_sb(sb, bdev, configure_cb, base_info->configure_cb_ctx);
			return;
		}
		SPDK_ERRLOG("Superblock of a different EC bdev found on bdev %s\n", base_info->name);
		status = -EEXIST;
		ec_bdev_free_base_bdev_resource(base_info);
		break;
	case -EINVAL:
		/* no valid superblock */
		ec_bdev_configure_base_bdev_cont(base_info);
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

/*
 * brief:
 * ec_bdev_configure_base_bdev configures a base bdev for EC bdev
 * params:
 * base_info - EC base bdev info
 * existing - true if this is an existing bdev (from superblock)
 * cb_fn - callback function
 * cb_ctx - callback context
 * returns:
 * 0 on success, non-zero on failure
 */
static int
ec_bdev_configure_base_bdev(struct ec_base_bdev_info *base_info, bool existing,
			    ec_base_bdev_cb cb_fn, void *cb_ctx)
{
	struct ec_bdev *ec_bdev = base_info->ec_bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	const struct spdk_uuid *bdev_uuid;
	int rc;

	printf("[ec] ec_bdev_configure_base_bdev enter: base_info=%p name=%s existing=%d desc=%p\n",
	       (void *)base_info, base_info->name ? base_info->name : "(null)", (int)existing, (void *)base_info->desc);
	printf("[ec] base_info->ec_bdev=%p\n", (void *)ec_bdev);
	if (ec_bdev == NULL) {
		SPDK_ERRLOG("ec_bdev is NULL for base bdev '%s'\n", base_info->name ? base_info->name : "(null)");
		return -EINVAL;
	}

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

	rc = spdk_bdev_open_ext(base_info->name, true, ec_bdev_event_base_bdev, base_info, &desc);
	if (rc != 0) {
		if (rc != -ENODEV) {
			SPDK_ERRLOG("Unable to create desc on bdev '%s'\n", base_info->name);
		}
		printf("[ec] spdk_bdev_open_ext failed rc=%d name=%s\n", rc, base_info->name);
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

   rc = spdk_bdev_module_claim_bdev(bdev, NULL, &g_ec_if);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to claim this bdev as it is already claimed\n");
		spdk_bdev_close(desc);
		return rc;
	}

	SPDK_DEBUGLOG(bdev_ec, "bdev %s is claimed\n", bdev->name);

	base_info->app_thread_ch = spdk_bdev_get_io_channel(desc);
	if (base_info->app_thread_ch == NULL) {
		SPDK_ERRLOG("Failed to get io channel\n");
		spdk_bdev_module_release_bdev(bdev);
		spdk_bdev_close(desc);
		printf("[ec] get io channel failed\n");
		return -ENOMEM;
	}

	base_info->desc = desc;
	base_info->blockcnt = bdev->blockcnt;

	if (ec_bdev->superblock_enabled) {
		uint64_t data_offset;
		printf("[ec] superblock enabled, data_offset(current)=%lu\n", base_info->data_offset);

		if (base_info->data_offset == 0) {
			assert((EC_BDEV_MIN_DATA_OFFSET_SIZE % spdk_bdev_get_data_block_size(bdev)) == 0);
			data_offset = EC_BDEV_MIN_DATA_OFFSET_SIZE / spdk_bdev_get_data_block_size(bdev);
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

	/* EC bdev does not support DIF/DIX */
	if (spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) {
		SPDK_ERRLOG("Base bdev '%s' has DIF or DIX enabled - unsupported EC configuration\n",
			    bdev->name);
		rc = -EINVAL;
		goto out;
	}

	/*
	 * Set the EC bdev properties if this is the first base bdev configured,
	 * otherwise - verify. All base bdevs should have the same blocklen and metadata format.
	 */
	if (ec_bdev->bdev.blocklen == 0) {
		ec_bdev->bdev.blocklen = bdev->blocklen;
		ec_bdev->bdev.md_len = spdk_bdev_get_md_size(bdev);
		ec_bdev->bdev.md_interleave = spdk_bdev_is_md_interleaved(bdev);
		ec_bdev->bdev.dif_type = spdk_bdev_get_dif_type(bdev);
		ec_bdev->bdev.dif_check_flags = bdev->dif_check_flags;
		ec_bdev->bdev.dif_is_head_of_md = spdk_bdev_is_dif_head_of_md(bdev);
		ec_bdev->bdev.dif_pi_format = bdev->dif_pi_format;
	} else {
		if (ec_bdev->bdev.blocklen != bdev->blocklen) {
			SPDK_ERRLOG("EC bdev '%s' blocklen %u differs from base bdev '%s' blocklen %u\n",
				    ec_bdev->bdev.name, ec_bdev->bdev.blocklen, bdev->name, bdev->blocklen);
			rc = -EINVAL;
			goto out;
		}

		if (ec_bdev->bdev.md_len != spdk_bdev_get_md_size(bdev) ||
		    ec_bdev->bdev.md_interleave != spdk_bdev_is_md_interleaved(bdev) ||
		    ec_bdev->bdev.dif_type != spdk_bdev_get_dif_type(bdev) ||
		    ec_bdev->bdev.dif_check_flags != bdev->dif_check_flags ||
		    ec_bdev->bdev.dif_is_head_of_md != spdk_bdev_is_dif_head_of_md(bdev) ||
		    ec_bdev->bdev.dif_pi_format != bdev->dif_pi_format) {
			SPDK_ERRLOG("EC bdev '%s' has different metadata format than base bdev '%s'\n",
				    ec_bdev->bdev.name, bdev->name);
			rc = -EINVAL;
			goto out;
		}
	}

	assert(base_info->configure_cb == NULL);
	base_info->configure_cb = cb_fn;
	base_info->configure_cb_ctx = cb_ctx;

	if (existing) {
		ec_bdev_configure_base_bdev_cont(base_info);
	} else {
		/* check for existing superblock when using a new bdev */
		rc = ec_bdev_load_base_bdev_superblock(desc, base_info->app_thread_ch,
				ec_bdev_configure_base_bdev_check_sb_cb, base_info);
		if (rc) {
			SPDK_ERRLOG("Failed to read bdev %s superblock: %s\n",
				    bdev->name, spdk_strerror(-rc));
		}
	}
out:
	if (rc != 0) {
		base_info->configure_cb = NULL;
		ec_bdev_free_base_bdev_resource(base_info);
	}
	return rc;
}

/*
 * brief:
 * ec_bdev_add_base_bdev adds a base bdev to an EC bdev
 * params:
 * ec_bdev - pointer to EC bdev
 * name - name of the base bdev
 * is_data_block - true if this is a data block, false if parity block
 * cb_fn - callback function
 * cb_ctx - callback context
 * returns:
 * 0 on success, non-zero on failure
 */
int
ec_bdev_add_base_bdev(struct ec_bdev *ec_bdev, const char *name, bool is_data_block,
		     ec_base_bdev_cb cb_fn, void *cb_ctx)
{
	struct ec_base_bdev_info *base_info = NULL, *iter;
	struct spdk_bdev *bdev;
	int rc;



	assert(name != NULL);
	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	if (ec_bdev->state == EC_BDEV_STATE_CONFIGURING) {
		bdev = spdk_bdev_get_by_name(name);

		if (bdev != NULL) {
			EC_FOR_EACH_BASE_BDEV(ec_bdev, iter) {
				if (iter->name == NULL &&
				    spdk_uuid_compare(&bdev->uuid, &iter->uuid) == 0) {
					base_info = iter;
					break;
				}
			}
		}
	}

	if (base_info == NULL || ec_bdev->state == EC_BDEV_STATE_ONLINE) {
		EC_FOR_EACH_BASE_BDEV(ec_bdev, iter) {
			if (iter->name == NULL && spdk_uuid_is_null(&iter->uuid)) {
				base_info = iter;
				break;
			}
		}
	}

	if (base_info == NULL) {
		SPDK_ERRLOG("no empty slot found in EC bdev '%s' for new base bdev '%s' (is_data_block=%d)\n",
			    ec_bdev->bdev.name, name, is_data_block);
		return -EINVAL;
	}

	assert(base_info->is_configured == false);

	if (ec_bdev->state == EC_BDEV_STATE_ONLINE) {
		assert(base_info->data_size != 0);
		assert(base_info->desc == NULL);
	}

	/* Set is_data_block for this slot */
	base_info->is_data_block = is_data_block;

	base_info->name = strdup(name);
	if (base_info->name == NULL) {
		return -ENOMEM;
	}

	rc = ec_bdev_configure_base_bdev(base_info, false, cb_fn, cb_ctx);
	if (rc != 0 && (rc != -ENODEV || ec_bdev->state != EC_BDEV_STATE_CONFIGURING)) {
		SPDK_ERRLOG("base bdev '%s' configure failed: %s\n", name, spdk_strerror(-rc));
		free(base_info->name);
		base_info->name = NULL;
		/* Reset is_data_block to allow re-use of this slot */
		base_info->is_data_block = false;
	}

	return rc;
}

/*
 * brief:
 * ec_bdev_event_cb is called when EC bdev triggers asynchronous event
 * params:
 * type - event type
 * bdev - bdev that triggered event
 * event_ctx - context for event (ec_bdev)
 * returns:
 * none
 */
static void
ec_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	struct ec_bdev *ec_bdev = event_ctx;

	if (type == SPDK_BDEV_EVENT_REMOVE) {
		/* TODO: Handle EC bdev removal - similar to RAID */
		SPDK_NOTICELOG("EC bdev %s removal event\n", ec_bdev->bdev.name);
	}
}

/*
 * brief:
 * ec_bdev_configure_cont continues the configuration of EC bdev after
 * superblock is written (if enabled) or directly if superblock is disabled.
 * It registers the EC bdev to the bdev layer.
 * params:
 * ec_bdev - pointer to EC bdev
 * returns:
 * none
 */
static void
ec_bdev_configure_cont(struct ec_bdev *ec_bdev)
{
	struct spdk_bdev *ec_bdev_gen = &ec_bdev->bdev;
	int rc;

	ec_bdev->state = EC_BDEV_STATE_ONLINE;
	SPDK_DEBUGLOG(bdev_ec, "io device register %p\n", ec_bdev);
	SPDK_DEBUGLOG(bdev_ec, "blockcnt %" PRIu64 ", blocklen %u\n",
		      ec_bdev_gen->blockcnt, ec_bdev_gen->blocklen);
	spdk_io_device_register(ec_bdev, ec_bdev_create_cb,
				ec_bdev_destroy_cb,
				sizeof(struct ec_bdev_io_channel),
				ec_bdev_gen->name);
	rc = spdk_bdev_register(ec_bdev_gen);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to register EC bdev '%s': %s\n",
			    ec_bdev_gen->name, spdk_strerror(-rc));
		goto out;
	}

	/*
	 * Open the bdev internally to delay unregistering if needed.
	 * This is similar to RAID's approach.
	 */
	rc = spdk_bdev_open_ext(ec_bdev_gen->name, false, ec_bdev_event_cb, ec_bdev,
				&ec_bdev->self_desc);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to open EC bdev '%s': %s\n",
			    ec_bdev_gen->name, spdk_strerror(-rc));
		spdk_bdev_unregister(ec_bdev_gen, NULL, NULL);
		goto out;
	}

	SPDK_DEBUGLOG(bdev_ec, "EC bdev generic %p\n", ec_bdev_gen);
	SPDK_DEBUGLOG(bdev_ec, "EC bdev is created with name %s, ec_bdev %p\n",
		      ec_bdev_gen->name, ec_bdev);
out:
	if (rc != 0) {
		ec_stop(ec_bdev);
		spdk_io_device_unregister(ec_bdev, NULL);
		ec_bdev->state = EC_BDEV_STATE_CONFIGURING;
	}

	if (ec_bdev->configure_cb != NULL) {
		ec_bdev->configure_cb(ec_bdev->configure_cb_ctx, rc);
		ec_bdev->configure_cb = NULL;
	}
}

/*
 * brief:
 * ec_bdev_configure_write_sb_cb is called after superblock is written
 * params:
 * status - status of superblock write
 * ec_bdev - pointer to EC bdev
 * ctx - context (unused)
 * returns:
 * none
 */
static void
ec_bdev_configure_write_sb_cb(int status, struct ec_bdev *ec_bdev, void *ctx)
{
	if (status == 0) {
		ec_bdev_configure_cont(ec_bdev);
	} else {
		SPDK_ERRLOG("Failed to write EC bdev '%s' superblock: %s\n",
			    ec_bdev->bdev.name, spdk_strerror(-status));
		ec_stop(ec_bdev);
		if (ec_bdev->configure_cb != NULL) {
			ec_bdev->configure_cb(ec_bdev->configure_cb_ctx, status);
			ec_bdev->configure_cb = NULL;
		}
	}
}

/*
 * brief:
 * ec_bdev_configure configures the EC bdev after all base bdevs are discovered.
 * It calculates strip_size, initializes ISA-L tables, and handles superblock if enabled.
 * params:
 * ec_bdev - pointer to EC bdev
 * cb - callback function
 * cb_ctx - callback context
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
ec_bdev_configure(struct ec_bdev *ec_bdev, ec_bdev_configure_cb cb, void *cb_ctx)
{
	uint32_t data_block_size = spdk_bdev_get_data_block_size(&ec_bdev->bdev);
	int rc;

	assert(ec_bdev->state == EC_BDEV_STATE_CONFIGURING);
	assert(ec_bdev->num_base_bdevs_discovered == ec_bdev->num_base_bdevs_operational);
	assert(ec_bdev->bdev.blocklen > 0);

	/* Call ec_start to initialize EC bdev and calculate blockcnt */
	rc = ec_start(ec_bdev);
	if (rc != 0) {
		SPDK_ERRLOG("EC module startup callback failed\n");
		return rc;
	}

	assert(ec_bdev->configure_cb == NULL);
	ec_bdev->configure_cb = cb;
	ec_bdev->configure_cb_ctx = cb_ctx;

	if (ec_bdev->superblock_enabled) {
		if (ec_bdev->sb == NULL) {
			rc = ec_bdev_alloc_superblock(ec_bdev, data_block_size);
			if (rc == 0) {
				ec_bdev_init_superblock(ec_bdev);
			}
		} else {
			assert(spdk_uuid_compare(&ec_bdev->sb->uuid, &ec_bdev->bdev.uuid) == 0);
			if (ec_bdev->sb->block_size != data_block_size) {
				SPDK_ERRLOG("blocklen does not match value in superblock\n");
				rc = -EINVAL;
			}
			if (ec_bdev->sb->ec_size != ec_bdev->bdev.blockcnt) {
				SPDK_ERRLOG("blockcnt does not match value in superblock\n");
				rc = -EINVAL;
			}
		}

		if (rc != 0) {
			ec_bdev->configure_cb = NULL;
			ec_stop(ec_bdev);
			return rc;
		}

		ec_bdev_write_superblock(ec_bdev, ec_bdev_configure_write_sb_cb, NULL);
	} else {
		ec_bdev_configure_cont(ec_bdev);
	}

	return 0;
}

/*
 * brief:
 * ec_bdev_fini_start is called when bdev layer is starting the
 * shutdown process
 * params:
 * none
 * returns:
 * none
 */
static void
ec_bdev_fini_start(void)
{
	struct ec_bdev *ec_bdev;
	struct ec_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_ec, "ec_bdev_fini_start\n");

	TAILQ_FOREACH(ec_bdev, &g_ec_bdev_list, global_link) {
		if (ec_bdev->state != EC_BDEV_STATE_ONLINE) {
			EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
				ec_bdev_free_base_bdev_resource(base_info);
			}
		}
	}

	g_shutdown_started = true;
}

/*
 * brief:
 * ec_bdev_exit is called on EC bdev module exit time by bdev layer
 * params:
 * none
 * returns:
 * none
 */
static void
ec_bdev_exit(void)
{
	struct ec_bdev *ec_bdev, *tmp;

	SPDK_DEBUGLOG(bdev_ec, "ec_bdev_exit\n");

	TAILQ_FOREACH_SAFE(ec_bdev, &g_ec_bdev_list, global_link, tmp) {
		ec_bdev_free(ec_bdev);
	}
}

static int
ec_bdev_config_json(struct spdk_json_write_ctx *w)
{
	/* EC bdev module doesn't have global options */
	return 0;
}

/*
 * brief:
 * ec_bdev_get_ctx_size is used to return the context size of bdev_io for EC
 * module
 * params:
 * none
 * returns:
 * size of spdk_bdev_io context for EC
 */
static int
ec_bdev_get_ctx_size(void)
{
	SPDK_DEBUGLOG(bdev_ec, "ec_bdev_get_ctx_size\n");
	return sizeof(struct ec_bdev_io);
}

/*
 * brief:
 * ec_bdev_init is the initialization function for EC bdev module
 * params:
 * none
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
ec_bdev_init(void)
{
	return 0;
}

/*
 * brief:
 * ec_bdev_create_from_sb creates an EC bdev from superblock
 * This is used during examine to reconstruct EC bdev from on-disk metadata
 * params:
 * sb - superblock loaded from disk
 * ec_bdev_out - output EC bdev
 * returns:
 * 0 on success, non-zero on failure
 */
static int
ec_bdev_create_from_sb(const struct ec_bdev_superblock *sb, struct ec_bdev **ec_bdev_out)
{
	struct ec_bdev *ec_bdev;
	uint8_t i;
	int rc;

	/* Convert strip_size from blocks to KB */
	uint32_t strip_size_kb = (sb->strip_size * sb->block_size) / 1024;

	rc = ec_bdev_create((const char *)sb->name, strip_size_kb, sb->k, sb->p,
			   true, &sb->uuid, &ec_bdev);
	if (rc != 0) {
		return rc;
	}

	rc = ec_bdev_alloc_superblock(ec_bdev, sb->block_size);
	if (rc != 0) {
		ec_bdev_free(ec_bdev);
		return rc;
	}

	assert(sb->length <= EC_BDEV_SB_MAX_LENGTH);
	memcpy(ec_bdev->sb, sb, sb->length);

	/* Initialize base bdev info from superblock */
	for (i = 0; i < sb->base_bdevs_size; i++) {
		const struct ec_bdev_sb_base_bdev *sb_base_bdev = &sb->base_bdevs[i];
		struct ec_base_bdev_info *base_info = &ec_bdev->base_bdev_info[sb_base_bdev->slot];

		if (sb_base_bdev->state == EC_SB_BASE_BDEV_CONFIGURED) {
			spdk_uuid_copy(&base_info->uuid, &sb_base_bdev->uuid);
			base_info->is_data_block = sb_base_bdev->is_data_block;
			ec_bdev->num_base_bdevs_operational++;
		}

		base_info->data_offset = sb_base_bdev->data_offset;
		base_info->data_size = sb_base_bdev->data_size;
	}

	*ec_bdev_out = ec_bdev;
	return 0;
}

/*
 * brief:
 * ec_bdev_examine_no_sb handles examine when no superblock is found
 * This tries to match base bdevs to EC bdevs in CONFIGURING state
 * params:
 * bdev - base bdev to examine
 * returns:
 * none
 */
static void
ec_bdev_examine_no_sb(struct spdk_bdev *bdev)
{
	struct ec_bdev *ec_bdev;
	struct ec_base_bdev_info *base_info;

	TAILQ_FOREACH(ec_bdev, &g_ec_bdev_list, global_link) {
		if (ec_bdev->state != EC_BDEV_STATE_CONFIGURING || ec_bdev->sb != NULL) {
			continue;
		}
		EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
			if (base_info->desc == NULL &&
			    ((base_info->name != NULL && strcmp(bdev->name, base_info->name) == 0) ||
			     spdk_uuid_compare(&base_info->uuid, &bdev->uuid) == 0)) {
				ec_bdev_configure_base_bdev(base_info, true, NULL, NULL);
				break;
			}
		}
	}
}

/*
 * Context structure for examining other base bdevs
 */
struct ec_bdev_examine_others_ctx {
	struct spdk_uuid ec_bdev_uuid;
	uint8_t current_base_bdev_idx;
	ec_base_bdev_cb cb_fn;
	void *cb_ctx;
};

static void ec_bdev_examine_others(void *_ctx, int status);

static void
ec_bdev_examine_others_load_cb(struct spdk_bdev *bdev, const struct ec_bdev_superblock *sb,
				int status, void *_ctx)
{
	struct ec_bdev_examine_others_ctx *ctx = _ctx;

	if (status != 0) {
		if (ctx->cb_fn != NULL) {
			ctx->cb_fn(ctx->cb_ctx, status);
		}
		free(ctx);
		return;
	}

	/* Use ec_bdev_examine_sb to handle this base bdev */
	ec_bdev_examine_sb(sb, bdev, ec_bdev_examine_others, ctx);
}

static void
ec_bdev_examine_others(void *_ctx, int status)
{
	struct ec_bdev_examine_others_ctx *ctx = _ctx;
	struct ec_bdev *ec_bdev;
	struct ec_base_bdev_info *base_info;
	char uuid_str[SPDK_UUID_STRING_LEN];

	if (status != 0 && status != -EEXIST) {
		goto out;
	}

	ec_bdev = ec_bdev_find_by_uuid(&ctx->ec_bdev_uuid);
	if (ec_bdev == NULL) {
		status = -ENODEV;
		goto out;
	}

	for (base_info = &ec_bdev->base_bdev_info[ctx->current_base_bdev_idx];
	     base_info < &ec_bdev->base_bdev_info[ec_bdev->num_base_bdevs];
	     base_info++) {
		if (base_info->is_configured || spdk_uuid_is_null(&base_info->uuid)) {
			continue;
		}

		spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &base_info->uuid);

		if (spdk_bdev_get_by_name(uuid_str) == NULL) {
			continue;
		}

		ctx->current_base_bdev_idx = (uint8_t)(base_info - ec_bdev->base_bdev_info);

		status = ec_bdev_examine_load_sb(uuid_str, ec_bdev_examine_others_load_cb, ctx);
		if (status != 0) {
			continue;
		}
		return;
	}
out:
	if (ctx->cb_fn != NULL) {
		ctx->cb_fn(ctx->cb_ctx, status);
	}
	free(ctx);
}

/*
 * brief:
 * ec_bdev_examine_sb processes superblock found during examine
 * This function handles the logic of reconstructing or adding base bdevs
 * params:
 * sb - superblock found
 * bdev - base bdev that contains the superblock
 * cb_fn - callback function
 * cb_ctx - callback context
 * returns:
 * none
 */
static void
ec_bdev_examine_sb(const struct ec_bdev_superblock *sb, struct spdk_bdev *bdev,
		   ec_base_bdev_cb cb_fn, void *cb_ctx)
{
	const struct ec_bdev_sb_base_bdev *sb_base_bdev = NULL;
	struct ec_bdev *ec_bdev;
	struct ec_base_bdev_info *iter, *base_info;
	uint8_t i;
	int rc;

	if (sb->block_size != spdk_bdev_get_data_block_size(bdev)) {
		SPDK_WARNLOG("Bdev %s block size (%u) does not match the value in superblock (%u)\n",
			     bdev->name, sb->block_size, spdk_bdev_get_data_block_size(bdev));
		rc = -EINVAL;
		goto out;
	}

	if (spdk_uuid_is_null(&sb->uuid)) {
		SPDK_WARNLOG("NULL EC bdev UUID in superblock on bdev %s\n", bdev->name);
		rc = -EINVAL;
		goto out;
	}

	ec_bdev = ec_bdev_find_by_uuid(&sb->uuid);

	if (ec_bdev) {
		if (ec_bdev->sb == NULL) {
			SPDK_WARNLOG("EC superblock is null\n");
			rc = -EINVAL;
			goto out;
		}

		if (sb->seq_number > ec_bdev->sb->seq_number) {
			SPDK_DEBUGLOG(bdev_ec,
				      "EC superblock seq_number on bdev %s (%lu) greater than existing EC bdev %s (%lu)\n",
				      bdev->name, sb->seq_number, ec_bdev->bdev.name, ec_bdev->sb->seq_number);

			if (ec_bdev->state != EC_BDEV_STATE_CONFIGURING) {
				SPDK_WARNLOG("Newer version of EC bdev %s superblock found on bdev %s but EC bdev is not in configuring state.\n",
					     ec_bdev->bdev.name, bdev->name);
				rc = -EBUSY;
				goto out;
			}

			/* remove and then recreate the EC bdev using the newer superblock */
			ec_bdev_delete(ec_bdev, NULL, NULL);
			ec_bdev = NULL;
		} else if (sb->seq_number < ec_bdev->sb->seq_number) {
			SPDK_DEBUGLOG(bdev_ec,
				      "EC superblock seq_number on bdev %s (%lu) smaller than existing EC bdev %s (%lu)\n",
				      bdev->name, sb->seq_number, ec_bdev->bdev.name, ec_bdev->sb->seq_number);
			/* use the current EC bdev superblock */
			sb = ec_bdev->sb;
		}
	}

	/* Find the base bdev in superblock */
	for (i = 0; i < sb->base_bdevs_size; i++) {
		sb_base_bdev = &sb->base_bdevs[i];

		assert(spdk_uuid_is_null(&sb_base_bdev->uuid) == false);

		if (spdk_uuid_compare(&sb_base_bdev->uuid, spdk_bdev_get_uuid(bdev)) == 0) {
			break;
		}
	}

	if (i == sb->base_bdevs_size) {
		SPDK_DEBUGLOG(bdev_ec, "EC superblock does not contain this bdev's uuid\n");
		rc = -EINVAL;
		goto out;
	}

	if (!ec_bdev) {
		struct ec_bdev_examine_others_ctx *ctx;

		ctx = calloc(1, sizeof(*ctx));
		if (ctx == NULL) {
			rc = -ENOMEM;
			goto out;
		}

		rc = ec_bdev_create_from_sb(sb, &ec_bdev);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to create EC bdev %s: %s\n",
				    sb->name, spdk_strerror(-rc));
			free(ctx);
			goto out;
		}

		/* after this base bdev is configured, examine other base bdevs that may be present */
		spdk_uuid_copy(&ctx->ec_bdev_uuid, &sb->uuid);
		ctx->cb_fn = cb_fn;
		ctx->cb_ctx = cb_ctx;

		cb_fn = ec_bdev_examine_others;
		cb_ctx = ctx;
	}

	if (ec_bdev->state == EC_BDEV_STATE_ONLINE) {
		assert(sb_base_bdev->slot < ec_bdev->num_base_bdevs);
		base_info = &ec_bdev->base_bdev_info[sb_base_bdev->slot];
		assert(base_info->is_configured == false);
		assert(sb_base_bdev->state == EC_SB_BASE_BDEV_MISSING ||
		       sb_base_bdev->state == EC_SB_BASE_BDEV_FAILED);
		assert(spdk_uuid_is_null(&base_info->uuid));
		spdk_uuid_copy(&base_info->uuid, &sb_base_bdev->uuid);
		base_info->is_data_block = sb_base_bdev->is_data_block;
		SPDK_NOTICELOG("Re-adding bdev %s to EC bdev %s.\n", bdev->name, ec_bdev->bdev.name);
		rc = ec_bdev_configure_base_bdev(base_info, true, cb_fn, cb_ctx);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to configure bdev %s as base bdev of EC %s: %s\n",
				    bdev->name, ec_bdev->bdev.name, spdk_strerror(-rc));
		}
		goto out;
	}

	if (sb_base_bdev->state != EC_SB_BASE_BDEV_CONFIGURED) {
		SPDK_NOTICELOG("Bdev %s is not an active member of EC bdev %s. Ignoring.\n",
			       bdev->name, ec_bdev->bdev.name);
		rc = -EINVAL;
		goto out;
	}

	base_info = NULL;
	EC_FOR_EACH_BASE_BDEV(ec_bdev, iter) {
		if (spdk_uuid_compare(&iter->uuid, spdk_bdev_get_uuid(bdev)) == 0) {
			base_info = iter;
			break;
		}
	}

	if (base_info == NULL) {
		SPDK_ERRLOG("Bdev %s is not a member of EC bdev %s\n",
			    bdev->name, ec_bdev->bdev.name);
		rc = -EINVAL;
		goto out;
	}

	if (base_info->is_configured) {
		rc = -EEXIST;
		goto out;
	}

	rc = ec_bdev_configure_base_bdev(base_info, true, cb_fn, cb_ctx);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to configure bdev %s as base bdev of EC %s: %s\n",
			    bdev->name, ec_bdev->bdev.name, spdk_strerror(-rc));
	}
out:
	if (rc != 0 && cb_fn != NULL) {
		cb_fn(cb_ctx, rc);
	}
}

/*
 * Context structure for examine operations
 */
struct ec_bdev_examine_ctx {
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	ec_bdev_examine_load_sb_cb cb;
	void *cb_ctx;
};

static void
ec_bdev_examine_ctx_free(struct ec_bdev_examine_ctx *ctx)
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
ec_bdev_examine_load_sb_done(const struct ec_bdev_superblock *sb, int status, void *_ctx)
{
	struct ec_bdev_examine_ctx *ctx = _ctx;
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(ctx->desc);

	if (ctx->cb != NULL) {
		ctx->cb(bdev, sb, status, ctx->cb_ctx);
	} else {
		ec_bdev_examine_ctx_free(ctx);
	}
}

static void
ec_bdev_examine_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	/* No action needed for examine */
}

int
ec_bdev_examine_load_sb(const char *bdev_name, ec_bdev_examine_load_sb_cb cb, void *cb_ctx)
{
	struct ec_bdev_examine_ctx *ctx;
	int rc;

	assert(cb != NULL);

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	rc = spdk_bdev_open_ext(bdev_name, false, ec_bdev_examine_event_cb, NULL, &ctx->desc);
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

	rc = ec_bdev_load_base_bdev_superblock(ctx->desc, ctx->ch, ec_bdev_examine_load_sb_done, ctx);
	if (rc) {
		SPDK_ERRLOG("Failed to read bdev %s superblock: %s\n",
			    bdev_name, spdk_strerror(-rc));
		goto err;
	}

	return 0;
err:
	ec_bdev_examine_ctx_free(ctx);
	return rc;
}

static void
ec_bdev_examine_done(void *ctx, int status)
{
	struct spdk_bdev *bdev = ctx;

	(void)bdev; /* Currently unused, but may be needed for future error reporting */
	if (status != 0) {
		SPDK_DEBUGLOG(bdev_ec, "Failed to examine bdev %s: %s\n",
			      bdev->name, spdk_strerror(-status));
	}
	spdk_bdev_module_examine_done(&g_ec_if);
}

static void
ec_bdev_examine_cont(struct spdk_bdev *bdev, const struct ec_bdev_superblock *sb, int status,
		     void *ctx)
{
	switch (status) {
	case 0:
		/* valid superblock found */
		SPDK_DEBUGLOG(bdev_ec, "EC superblock found on bdev %s\n", bdev->name);
		ec_bdev_examine_sb(sb, bdev, ec_bdev_examine_done, bdev);
		return;
	case -EINVAL:
		/* no valid superblock, check if it can be claimed anyway */
		ec_bdev_examine_no_sb(bdev);
		status = 0;
		break;
	}

	ec_bdev_examine_done(bdev, status);
}

/*
 * brief:
 * ec_bdev_examine function is the examine function called by the below layers
 * like bdev_nvme layer. This function will check if this base bdev can be
 * claimed by this EC bdev or not.
 * params:
 * bdev - pointer to base bdev
 * returns:
 * none
 */
static void
ec_bdev_examine(struct spdk_bdev *bdev)
{
	int rc = 0;

	if (ec_bdev_find_base_info_by_bdev(bdev) != NULL) {
		goto done;
	}

	if (spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) {
		ec_bdev_examine_no_sb(bdev);
		goto done;
	}

	rc = ec_bdev_examine_load_sb(bdev->name, ec_bdev_examine_cont, NULL);
	if (rc != 0) {
		goto done;
	}

	return;
done:
	ec_bdev_examine_done(bdev, rc);
}

struct spdk_bdev_module g_ec_if = {
	.name = "ec",
	.module_init = ec_bdev_init,
	.fini_start = ec_bdev_fini_start,
	.module_fini = ec_bdev_exit,
	.config_json = ec_bdev_config_json,
	.get_ctx_size = ec_bdev_get_ctx_size,
	.examine_disk = ec_bdev_examine,
	.async_init = false,
	.async_fini = false,
};
SPDK_BDEV_MODULE_REGISTER(ec, &g_ec_if)

/* Log component for bdev EC bdev module */
SPDK_LOG_REGISTER_COMPONENT(bdev_ec)

