#include "bdev_ec.h"
#include "bdev_ec_internal.h"
#include "spdk/bdev_module.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/likely.h"

#define EC_OFFSET_BLOCKS_INVALID	UINT64_MAX

static bool g_shutdown_started = false;


/* List of all EC bdevs */
struct ec_all_tailq g_ec_bdev_list = TAILQ_HEAD_INITIALIZER(g_ec_bdev_list);


/* Forward declarations for functions used in function table - these must NOT be static */
int ec_bdev_destruct(void *ctx);
void ec_bdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);
bool ec_bdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type);
struct spdk_io_channel *ec_bdev_get_io_channel(void *ctxt);
int ec_bdev_dump_info_json(void *ctx, struct spdk_json_write_ctx *w);
int ec_bdev_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size);
void ec_bdev_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success);
void ec_bdev_module_stop_done(struct ec_bdev *ec_bdev);
void ec_bdev_free(struct ec_bdev *ec_bdev);
void ec_bdev_free_base_bdev_resource(struct ec_base_bdev_info *base_info);

/* Type definition for examine load superblock callback */
typedef void (*ec_bdev_examine_load_sb_cb)(struct spdk_bdev *bdev,
		const struct ec_bdev_superblock *sb, int status, void *ctx);

int ec_bdev_examine_load_sb(const char *bdev_name, ec_bdev_examine_load_sb_cb cb, void *cb_ctx);

/* Forward declarations for static functions */
static void ec_bdev_examine(struct spdk_bdev *bdev);
static int ec_bdev_init(void);
static void ec_bdev_deconfigure(struct ec_bdev *ec_bdev, ec_bdev_destruct_cb cb_fn, void *cb_arg);
static void ec_bdev_configure_cont(struct ec_bdev *ec_bdev);
static int ec_bdev_configure(struct ec_bdev *ec_bdev, ec_bdev_configure_cb cb, void *cb_ctx);
static int ec_start(struct ec_bdev *ec_bdev);
static void ec_stop(struct ec_bdev *ec_bdev);
static struct ec_bdev *ec_bdev_find_by_uuid(const struct spdk_uuid *uuid);
static struct ec_base_bdev_info *ec_bdev_find_base_info_by_bdev(struct spdk_bdev *base_bdev);
static int ec_bdev_create_from_sb(const struct ec_bdev_superblock *sb, struct ec_bdev **ec_bdev_out);
static int ec_bdev_configure_base_bdev(struct ec_base_bdev_info *base_info, bool existing,
				       ec_base_bdev_cb cb_fn, void *cb_ctx);
static void ec_bdev_event_base_bdev(enum spdk_bdev_event_type event, struct spdk_bdev *bdev, void *event_ctx);
static void ec_bdev_event_cb(enum spdk_bdev_event_type event, struct spdk_bdev *bdev, void *event_ctx);
static void ec_bdev_examine_cont(const struct ec_bdev_superblock *sb, struct spdk_bdev *bdev,
				 ec_base_bdev_cb cb_fn, void *cb_ctx);
static void ec_bdev_examine_others(void *_ctx, int status);

/* Structure for examine others context */
struct ec_bdev_examine_others_ctx {
	struct spdk_uuid ec_bdev_uuid;
	ec_base_bdev_cb cb_fn;
	void *cb_ctx;
};

/* Structure for delete superblock context */
struct ec_bdev_delete_sb_ctx {
	ec_bdev_destruct_cb cb_fn;
	void *cb_ctx;
};

/* Structure for examine load superblock context */
struct ec_bdev_examine_load_sb_ctx {
	ec_bdev_examine_load_sb_cb cb;
	void *cb_ctx;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
};

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
		return -ENOMEM;
	}

	for (i = 0; i < ec_bdev->num_base_bdevs; i++) {
		ec_ch->base_channel[i] = NULL;
	}

	i = 0;
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		/* Skip base bdevs that are not configured or have no descriptor.
		 * For EC, we can tolerate some base bdevs being unavailable as long
		 * as we have at least k data blocks available. However, we should
		 * still try to get channels for all configured base bdevs.
		 */
		if (base_info->is_configured == false || base_info->desc == NULL) {
			i++;
			continue;
		}
		
		ec_ch->base_channel[i] = spdk_bdev_get_io_channel(base_info->desc);
		if (ec_ch->base_channel[i] == NULL) {
			/* For EC, we can tolerate some base bdev channels being unavailable.
			 * The I/O layer will handle missing channels appropriately.
			 * However, we log a warning for debugging purposes.
			 */
			SPDK_WARNLOG("Unable to get io channel for base bdev %s (slot %u). "
				     "EC may operate in degraded mode.\n",
				     base_info->name ? base_info->name : "(null)", i);
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
		if (base_info->is_configured == false || base_info->desc == NULL) {
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
		if (base_info->is_configured == false || base_info->desc == NULL) {
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

	if (ec_bdev->superblock_enabled) {
		/* EC bdev configuration is stored in the superblock */
		return;
	}

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_ec_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uuid(w, "uuid", &ec_bdev->bdev.uuid);
	spdk_json_write_named_uint8(w, "k", ec_bdev->k);
	spdk_json_write_named_uint8(w, "p", ec_bdev->p);
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
	/* Runtime check instead of assert (assert may be disabled in release builds) */
	if (ec_io->base_bdev_io_remaining < completed) {
		SPDK_ERRLOG("Invalid remaining count: %" PRIu64 " < %" PRIu64 "\n",
			    ec_io->base_bdev_io_remaining, completed);
		ec_io->base_bdev_io_remaining = 0;
		ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
		return true;
	}
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
/*
 * brief:
 * ec_bdev_destruct_impl is the actual destruct implementation
 * params:
 * ctx - pointer to EC bdev
 * returns:
 * none
 */
static void
ec_bdev_destruct_impl(void *ctx)
{
	struct ec_bdev *ec_bdev = ctx;
	struct ec_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_ec, "ec_bdev_destruct\n");

	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
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
	spdk_thread_exec_msg(spdk_thread_get_app_thread(), ec_bdev_destruct_impl, ctx);
	return 1;
}

/*
 * brief:
 * ec_bdev_io_device_unregister_cb callback for IO device unregister
 * params:
 * io_device - pointer to ec_bdev
 * returns:
 * none
 */
static void
ec_bdev_io_device_unregister_cb(void *io_device)
{
	struct ec_bdev *ec_bdev = io_device;

	if (ec_bdev->num_base_bdevs_discovered == 0) {
		/* Free ec_bdev when there are no base bdevs left */
		SPDK_DEBUGLOG(bdev_ec, "EC bdev base bdevs is 0, going to free all in destruct\n");
		spdk_bdev_destruct_done(&ec_bdev->bdev, 0);
		ec_bdev_free(ec_bdev);
	} else {
		spdk_bdev_destruct_done(&ec_bdev->bdev, 0);
	}
}

void
ec_bdev_module_stop_done(struct ec_bdev *ec_bdev)
{
	if (ec_bdev->state != EC_BDEV_STATE_CONFIGURING) {
		spdk_io_device_unregister(ec_bdev, ec_bdev_io_device_unregister_cb);
	} else {
		ec_bdev_free(ec_bdev);
	}
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

	/* Set optimal_io_boundary for reading (similar to RAID0/RAID5F)
	 * This allows bdev layer to automatically split I/O at strip boundaries
	 * Note: We do NOT set write_unit_size to allow flexible writes (FTL-friendly)
	 */
	ec_bdev->bdev.optimal_io_boundary = ec_bdev->strip_size;
	ec_bdev->bdev.split_on_optimal_io_boundary = true;

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

/* ec_select_base_bdevs_default is defined in bdev_ec_io.c */

/* Note: I/O functions have been moved to bdev_ec_io.c */

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

/*
 * brief:
 * ec_bdev_deconfigure_io_device_unregister_done callback for IO device unregister during deconfigure
 */
static void
ec_bdev_deconfigure_io_device_unregister_done(void *io_device)
{
	struct ec_bdev *ec_bdev = io_device;

	/* After IO device is unregistered, unregister the bdev */
	spdk_bdev_unregister(&ec_bdev->bdev, ec_bdev_deconfigure_unregister_done, ec_bdev);
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

	/* First unregister IO device to release all I/O channels, then unregister bdev */
	spdk_io_device_unregister(ec_bdev, ec_bdev_deconfigure_io_device_unregister_done);
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

	ec_bdev_deconfigure(ec_bdev, cb_fn, cb_arg);
}

/*
 * brief:
 * ec_bdev_free_base_bdev_resource frees resources for a base bdev
 * params:
 * base_info - pointer to base bdev info
 * returns:
 * none
 */
void
ec_bdev_free_base_bdev_resource(struct ec_base_bdev_info *base_info)
{
	if (base_info == NULL) {
		return;
	}

	if (base_info->app_thread_ch != NULL) {
		spdk_put_io_channel(base_info->app_thread_ch);
		base_info->app_thread_ch = NULL;
	}

	if (base_info->desc != NULL) {
		spdk_bdev_module_release_bdev(spdk_bdev_desc_get_bdev(base_info->desc));
		spdk_bdev_close(base_info->desc);
		base_info->desc = NULL;
	}

	if (base_info->name != NULL) {
		free(base_info->name);
		base_info->name = NULL;
	}

	spdk_uuid_set_null(&base_info->uuid);
	base_info->is_configured = false;
	base_info->remove_scheduled = false;
}

/*
 * brief:
 * ec_bdev_free frees an EC bdev structure
 * params:
 * ec_bdev - pointer to EC bdev
 * returns:
 * none
 */
void
ec_bdev_free(struct ec_bdev *ec_bdev)
{
	struct ec_base_bdev_info *base_info;

	if (ec_bdev == NULL) {
		return;
	}

	/* Close self_desc if it's still open (safety check) */
	if (ec_bdev->self_desc != NULL) {
		SPDK_WARNLOG("self_desc still open when freeing EC bdev %s, closing it\n",
			     ec_bdev->bdev.name);
		spdk_bdev_close(ec_bdev->self_desc);
		ec_bdev->self_desc = NULL;
	}

	/* Free extension interface if registered */
	if (ec_bdev->extension_if != NULL) {
		ec_bdev_unregister_extension(ec_bdev);
	}

	/* Free all base bdev resources */
	if (ec_bdev->base_bdev_info != NULL) {
		EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
			ec_bdev_free_base_bdev_resource(base_info);
		}
		free(ec_bdev->base_bdev_info);
		ec_bdev->base_bdev_info = NULL;
	}

	/* Free superblock */
	if (ec_bdev->sb != NULL) {
		ec_bdev_free_superblock(ec_bdev);
	}

	/* Free module private data */
	if (ec_bdev->module_private != NULL) {
		struct ec_bdev_module_private *mp = ec_bdev->module_private;
		if (mp->encode_matrix != NULL) {
			spdk_dma_free(mp->encode_matrix);
		}
		if (mp->g_tbls != NULL) {
			spdk_dma_free(mp->g_tbls);
		}
		if (mp->decode_matrix != NULL) {
			spdk_dma_free(mp->decode_matrix);
		}
		if (mp->temp_matrix != NULL) {
			spdk_dma_free(mp->temp_matrix);
		}
		if (mp->invert_matrix != NULL) {
			spdk_dma_free(mp->invert_matrix);
		}
		free(mp);
		ec_bdev->module_private = NULL;
	}

	/* Free bdev name */
	if (ec_bdev->bdev.name != NULL) {
		free(ec_bdev->bdev.name);
		ec_bdev->bdev.name = NULL;
	}

	/* Free EC bdev structure */
	free(ec_bdev);
}

/*
 * brief:
 * ec_bdev_create_sb creates and writes superblock for EC bdev
 * params:
 * ec_bdev - pointer to EC bdev
 * returns:
 * 0 on success, non-zero on failure
 */
static int
ec_bdev_create_sb(struct ec_bdev *ec_bdev)
{
	int rc;
	uint32_t block_size;

	if (ec_bdev == NULL) {
		return -EINVAL;
	}

	/* Get block size from first configured base bdev */
	block_size = 512; /* Default */
	if (ec_bdev->base_bdev_info != NULL) {
		struct ec_base_bdev_info *base_info;
		EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
			if (base_info->is_configured && base_info->desc != NULL) {
				struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(base_info->desc);
				block_size = spdk_bdev_get_data_block_size(bdev);
				break;
			}
		}
	}

	/* Allocate superblock */
	rc = ec_bdev_alloc_superblock(ec_bdev, block_size);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to allocate superblock for EC bdev %s\n", ec_bdev->bdev.name);
		return rc;
	}

	/* Initialize superblock */
	ec_bdev_init_superblock(ec_bdev);

	/* Write superblock synchronously */
	ec_bdev->superblock_enabled = true;

	return 0;
}

/*
 * brief:
 * ec_bdev_delete_sb_cb callback wrapper for superblock wipe
 */
static void
ec_bdev_delete_sb_cb(int status, struct ec_bdev *ec_bdev, void *ctx)
{
	struct ec_bdev_delete_sb_ctx *delete_ctx = ctx;
	ec_bdev_destruct_cb cb_fn = delete_ctx->cb_fn;
	void *cb_ctx = delete_ctx->cb_ctx;

	free(delete_ctx);
	ec_bdev_delete_continue(ec_bdev, cb_fn, cb_ctx);
}

/*
 * brief:
 * ec_bdev_delete_sb deletes superblock for EC bdev
 * params:
 * ec_bdev - pointer to EC bdev
 * cb_fn - callback function
 * cb_ctx - callback context
 * returns:
 * none
 */
static void
ec_bdev_delete_sb(struct ec_bdev *ec_bdev, ec_bdev_destruct_cb cb_fn, void *cb_ctx)
{
	struct ec_bdev_delete_sb_ctx *delete_ctx;

	if (ec_bdev == NULL || ec_bdev->sb == NULL) {
		ec_bdev_delete_continue(ec_bdev, cb_fn, cb_ctx);
		return;
	}

	delete_ctx = calloc(1, sizeof(*delete_ctx));
	if (delete_ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate delete context\n");
		ec_bdev_delete_continue(ec_bdev, cb_fn, cb_ctx);
		return;
	}

	delete_ctx->cb_fn = cb_fn;
	delete_ctx->cb_ctx = cb_ctx;

	/* Wipe superblock */
	ec_bdev_wipe_superblock(ec_bdev, ec_bdev_delete_sb_cb, delete_ctx);
}

/*
 * brief:
 * ec_bdev_create_from_sb creates EC bdev from superblock
 * params:
 * sb - pointer to superblock
 * ec_bdev_out - pointer to output EC bdev
 * returns:
 * 0 on success, non-zero on failure
 */
static int
ec_bdev_create_from_sb(const struct ec_bdev_superblock *sb, struct ec_bdev **ec_bdev_out)
{
	struct ec_bdev *ec_bdev;
	struct ec_base_bdev_info *base_info;
	uint8_t i;
	int rc;

	if (sb == NULL || ec_bdev_out == NULL) {
		return -EINVAL;
	}

	/* Check if EC bdev with this UUID already exists */
	ec_bdev = ec_bdev_find_by_uuid(&sb->uuid);
	if (ec_bdev != NULL) {
		*ec_bdev_out = ec_bdev;
		return 0;
	}

	/* Convert strip_size from blocks to KB */
	uint32_t strip_size_kb = (sb->strip_size * sb->block_size) / 1024;

	/* Create EC bdev structure */
	rc = ec_bdev_create(sb->name, strip_size_kb, sb->k, sb->p, true, &sb->uuid, &ec_bdev);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to create EC bdev from superblock: %s\n", spdk_strerror(-rc));
		return rc;
	}

	/* Allocate and initialize superblock */
	rc = ec_bdev_alloc_superblock(ec_bdev, sb->block_size);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to allocate superblock: %s\n", spdk_strerror(-rc));
		ec_bdev_free(ec_bdev);
		return rc;
	}

	/* Copy superblock data */
	memcpy(ec_bdev->sb, sb, sb->length);
	ec_bdev->superblock_enabled = true;

	/* Initialize base bdev info from superblock */
	for (i = 0; i < sb->base_bdevs_size; i++) {
		const struct ec_bdev_sb_base_bdev *sb_base = &sb->base_bdevs[i];
		if (sb_base->slot >= ec_bdev->num_base_bdevs) {
			SPDK_ERRLOG("Invalid slot number %u in superblock\n", sb_base->slot);
			ec_bdev_free(ec_bdev);
			return -EINVAL;
		}

		base_info = &ec_bdev->base_bdev_info[sb_base->slot];
		spdk_uuid_copy(&base_info->uuid, &sb_base->uuid);
		base_info->data_offset = sb_base->data_offset;
		base_info->data_size = sb_base->data_size;
		base_info->is_data_block = sb_base->is_data_block;
	}

	*ec_bdev_out = ec_bdev;
	return 0;
}

/*
 * brief:
 * ec_bdev_examine_load_sb_event_cb handles events for examine load superblock
 */
static void
ec_bdev_examine_load_sb_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	/* This is a temporary open for examination, so we don't need to handle events.
	 * If the bdev is removed, the examine process will fail naturally.
	 * We just log the event for debugging purposes.
	 */
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		SPDK_DEBUGLOG(bdev_ec, "Bdev %s removal event during examine\n", bdev->name);
		break;
	case SPDK_BDEV_EVENT_RESIZE:
		SPDK_DEBUGLOG(bdev_ec, "Bdev %s resize event during examine\n", bdev->name);
		break;
	case SPDK_BDEV_EVENT_MEDIA_MANAGEMENT:
		SPDK_DEBUGLOG(bdev_ec, "Bdev %s media management event during examine\n", bdev->name);
		break;
	default:
		SPDK_DEBUGLOG(bdev_ec, "Unknown event type %d for bdev %s during examine\n", type, bdev->name);
		break;
	}
}

/*
 * brief:
 * ec_bdev_examine_load_sb_cb_wrapper wraps the callback for examine load superblock
 */
static void
ec_bdev_examine_load_sb_cb_wrapper(const struct ec_bdev_superblock *sb, int status, void *ctx)
{
	struct ec_bdev_examine_load_sb_ctx *examine_ctx = ctx;
	struct spdk_bdev *bdev = NULL;

	if (examine_ctx->desc != NULL) {
		bdev = spdk_bdev_desc_get_bdev(examine_ctx->desc);
	}

	if (examine_ctx->ch != NULL) {
		spdk_put_io_channel(examine_ctx->ch);
	}
	if (examine_ctx->desc != NULL) {
		spdk_bdev_close(examine_ctx->desc);
	}

	examine_ctx->cb(bdev, sb, status, examine_ctx->cb_ctx);
	free(examine_ctx);
}

/*
 * brief:
 * ec_bdev_examine_load_sb loads superblock for examination
 * params:
 * bdev_name - name of base bdev
 * cb - callback function
 * cb_ctx - callback context
 * returns:
 * 0 on success, non-zero on failure
 */
int
ec_bdev_examine_load_sb(const char *bdev_name, ec_bdev_examine_load_sb_cb cb, void *cb_ctx)
{
	struct spdk_bdev *bdev;
	struct ec_bdev_examine_load_sb_ctx *examine_ctx;
	int rc;

	if (bdev_name == NULL || cb == NULL) {
		return -EINVAL;
	}

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (bdev == NULL) {
		/* Bdev not found, call callback with NULL superblock */
		cb(NULL, NULL, -ENODEV, cb_ctx);
		return -ENODEV;
	}

	examine_ctx = calloc(1, sizeof(*examine_ctx));
	if (examine_ctx == NULL) {
		cb(NULL, NULL, -ENOMEM, cb_ctx);
		return -ENOMEM;
	}

	examine_ctx->cb = cb;
	examine_ctx->cb_ctx = cb_ctx;

	rc = spdk_bdev_open_ext(bdev_name, false, ec_bdev_examine_load_sb_event_cb, NULL, &examine_ctx->desc);
	if (rc != 0) {
		SPDK_DEBUGLOG(bdev_ec, "Failed to open bdev %s: %s\n", bdev_name, spdk_strerror(-rc));
		free(examine_ctx);
		cb(NULL, NULL, rc, cb_ctx);
		return rc;
	}

	examine_ctx->ch = spdk_bdev_get_io_channel(examine_ctx->desc);
	if (examine_ctx->ch == NULL) {
		SPDK_ERRLOG("Failed to get I/O channel for bdev %s\n", bdev_name);
		spdk_bdev_close(examine_ctx->desc);
		free(examine_ctx);
		cb(NULL, NULL, -ENOMEM, cb_ctx);
		return -ENOMEM;
	}

	rc = ec_bdev_load_base_bdev_superblock(examine_ctx->desc, examine_ctx->ch,
					       ec_bdev_examine_load_sb_cb_wrapper, examine_ctx);
	if (rc != 0) {
		spdk_put_io_channel(examine_ctx->ch);
		spdk_bdev_close(examine_ctx->desc);
		free(examine_ctx);
		cb(NULL, NULL, rc, cb_ctx);
		return rc;
	}

	return 0;
}

/*
 * brief:
 * ec_bdev_get_ctx_size gets context size for EC bdev
 * params:
 * none
 * returns:
 * size of ec_bdev_io context
 */
static int
ec_bdev_get_ctx_size(void)
{
	return sizeof(struct ec_bdev_io);
}

/*
 * brief:
 * ec_bdev_config_json writes EC bdev config to JSON
 * params:
 * w - pointer to JSON write context
 * returns:
 * 0 on success
 */
static int
ec_bdev_config_json(struct spdk_json_write_ctx *w)
{
	struct ec_bdev *ec_bdev;

	TAILQ_FOREACH(ec_bdev, &g_ec_bdev_list, global_link) {
		ec_bdev_write_config_json(&ec_bdev->bdev, w);
	}

	return 0;
}

/*
 * brief:
 * ec_bdev_fini_start starts module finalization
 * params:
 * none
 * returns:
 * none
 */
static void
ec_bdev_fini_start(void)
{
	struct ec_bdev *ec_bdev, *tmp;

	g_shutdown_started = true;

	TAILQ_FOREACH_SAFE(ec_bdev, &g_ec_bdev_list, global_link, tmp) {
		ec_bdev_delete(ec_bdev, NULL, NULL);
	}
}

/*
 * brief:
 * ec_bdev_exit exits EC bdev module
 * params:
 * none
 * returns:
 * none
 */
static void
ec_bdev_exit(void)
{
	/* All EC bdevs should have been deleted by now */
	assert(TAILQ_EMPTY(&g_ec_bdev_list));
}

/*
 * brief:
 * ec_bdev_init initializes EC bdev module
 * params:
 * none
 * returns:
 * 0 on success, non-zero on failure
 */
static int
ec_bdev_init(void)
{
	return 0;
}

/*
 * brief:
 * ec_bdev_configure_cont continues EC bdev configuration
 * params:
 * ec_bdev - pointer to EC bdev
 * returns:
 * none
 */
static void
ec_bdev_configure_cont(struct ec_bdev *ec_bdev)
{
	struct ec_base_bdev_info *base_info;
	uint8_t num_configured = 0;
	uint8_t num_base_bdevs = ec_bdev->k + ec_bdev->p;
	int rc = 0;

	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->is_configured) {
			num_configured++;
		}
	}

	if (num_configured == num_base_bdevs) {
		/* All base bdevs are configured, start the EC bdev */
		rc = ec_start(ec_bdev);
		
		if (rc == 0) {
			ec_bdev->state = EC_BDEV_STATE_ONLINE;
			
			/* Register IO device before registering bdev (similar to raid module) */
			SPDK_DEBUGLOG(bdev_ec, "io device register %p\n", ec_bdev);
			SPDK_DEBUGLOG(bdev_ec, "blockcnt %" PRIu64 ", blocklen %u\n",
				      ec_bdev->bdev.blockcnt, ec_bdev->bdev.blocklen);
			spdk_io_device_register(ec_bdev, ec_bdev_create_cb, ec_bdev_destroy_cb,
						sizeof(struct ec_bdev_io_channel),
						ec_bdev->bdev.name);
			
			rc = spdk_bdev_register(&ec_bdev->bdev);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to register EC bdev '%s': %s\n",
					    ec_bdev->bdev.name, spdk_strerror(-rc));
				/* Cleanup IO device registration on failure */
				spdk_io_device_unregister(ec_bdev, NULL);
				ec_stop(ec_bdev);
				ec_bdev->state = EC_BDEV_STATE_CONFIGURING;
				goto out;
			}
			
			/*
			 * Open the bdev internally to delay unregistering if needed.
			 * During application shutdown, bdevs automatically get unregistered
			 * by the bdev layer so this is needed to handle cleanup correctly.
			 */
			rc = spdk_bdev_open_ext(ec_bdev->bdev.name, false, ec_bdev_event_cb,
						ec_bdev, &ec_bdev->self_desc);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to open EC bdev '%s': %s\n",
					    ec_bdev->bdev.name, spdk_strerror(-rc));
				spdk_bdev_unregister(&ec_bdev->bdev, NULL, NULL);
				spdk_io_device_unregister(ec_bdev, NULL);
				ec_stop(ec_bdev);
				ec_bdev->state = EC_BDEV_STATE_CONFIGURING;
				goto out;
			}
			
			SPDK_DEBUGLOG(bdev_ec, "EC bdev generic %p\n", &ec_bdev->bdev);
			SPDK_DEBUGLOG(bdev_ec, "EC bdev is created with name %s, ec_bdev %p\n",
				      ec_bdev->bdev.name, ec_bdev);
		} else {
			SPDK_ERRLOG("Failed to start EC bdev %s\n", ec_bdev->bdev.name);
			ec_bdev->state = EC_BDEV_STATE_OFFLINE;
			rc = -1; /* Set rc to error for callback */
		}
	} else {
		rc = 0; /* Still configuring, not an error */
	}
out:
	if (ec_bdev->configure_cb != NULL) {
		ec_bdev->configure_cb(ec_bdev->configure_cb_ctx, rc);
		ec_bdev->configure_cb = NULL;
		ec_bdev->configure_cb_ctx = NULL;
	}
}

/*
 * brief:
 * ec_bdev_configure configures EC bdev
 * params:
 * ec_bdev - pointer to EC bdev
 * cb - callback function
 * cb_ctx - callback context
 * returns:
 * 0 on success, non-zero on failure
 */
__attribute__((unused))
static int
ec_bdev_configure(struct ec_bdev *ec_bdev, ec_bdev_configure_cb cb, void *cb_ctx)
{
	ec_bdev->configure_cb = cb;
	ec_bdev->configure_cb_ctx = cb_ctx;

	ec_bdev_configure_cont(ec_bdev);

	return 0;
}


/*
 * brief:
 * ec_bdev_create creates a new EC bdev
 * params:
 * name - EC bdev name
 * strip_size - strip size in KB
 * k - number of data blocks
 * p - number of parity blocks
 * superblock - whether to use superblock
 * uuid - UUID for EC bdev (can be NULL)
 * ec_bdev_out - pointer to output EC bdev
 * returns:
 * 0 on success, non-zero on failure
 */
int
ec_bdev_create(const char *name, uint32_t strip_size, uint8_t k, uint8_t p,
	       bool superblock, const struct spdk_uuid *uuid,
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

	/* Initialize EC bdev fields */
	ec_bdev->strip_size = 0;
	ec_bdev->strip_size_kb = strip_size;
	ec_bdev->state = EC_BDEV_STATE_CONFIGURING;
	ec_bdev->num_base_bdevs = num_base_bdevs;

	/* Initialize base bdev info */
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		base_info->ec_bdev = ec_bdev;
	}

	/* Initialize bdev structure */
	ec_bdev_gen = &ec_bdev->bdev;
	ec_bdev_gen->name = strdup(name);
	if (!ec_bdev_gen->name) {
		SPDK_ERRLOG("Unable to allocate memory for EC bdev name\n");
		ec_bdev_free(ec_bdev);
		return -ENOMEM;
	}

	ec_bdev_gen->product_name = "EC bdev";
	ec_bdev_gen->ctxt = ec_bdev;
	ec_bdev_gen->fn_table = &g_ec_bdev_fn_table;
	ec_bdev_gen->module = &g_ec_if;

	/* Set UUID */
	if (uuid != NULL) {
		spdk_uuid_copy(&ec_bdev_gen->uuid, uuid);
	} else {
		spdk_uuid_generate(&ec_bdev_gen->uuid);
	}

	/* Set block size and block count */
	ec_bdev_gen->blocklen = 512; /* Default block size */
	ec_bdev_gen->write_cache = 0;
	ec_bdev_gen->optimal_io_boundary = 0;
	ec_bdev_gen->split_on_optimal_io_boundary = false;

	/* Set k and p */
	ec_bdev->k = k;
	ec_bdev->p = p;

	/* Add to global list */
	TAILQ_INSERT_TAIL(&g_ec_bdev_list, ec_bdev, global_link);

	/* Create superblock if requested */
	if (superblock) {
		int rc = ec_bdev_create_sb(ec_bdev);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to create superblock: %s\n", spdk_strerror(-rc));
			ec_bdev_free(ec_bdev);
			return rc;
		}
	}

	*ec_bdev_out = ec_bdev;

	return 0;
}

/*
 * brief:
 * ec_bdev_delete deletes an EC bdev
 * params:
 * ec_bdev - pointer to EC bdev
 * cb_fn - callback function
 * cb_ctx - callback context
 * returns:
 * none
 */
void
ec_bdev_delete(struct ec_bdev *ec_bdev, ec_bdev_destruct_cb cb_fn, void *cb_ctx)
{
	if (ec_bdev == NULL) {
		if (cb_fn) {
			cb_fn(cb_ctx, 0);
		}
		return;
	}

	/* Check if deletion is already in progress (similar to RAID module) */
	if (ec_bdev->destroy_started) {
		SPDK_DEBUGLOG(bdev_ec, "Deletion of EC bdev %s is already started\n",
			      ec_bdev->bdev.name);
		if (cb_fn) {
			cb_fn(cb_ctx, -EALREADY);
		}
		return;
	}

	ec_bdev->destroy_started = true;

	TAILQ_REMOVE(&g_ec_bdev_list, ec_bdev, global_link);

	if (ec_bdev->sb != NULL) {
		ec_bdev_delete_sb(ec_bdev, cb_fn, cb_ctx);
	} else {
		ec_bdev_delete_continue(ec_bdev, cb_fn, cb_ctx);
	}
}

/*
 * brief:
 * ec_bdev_add_base_bdev adds a base bdev to EC bdev
 * params:
 * ec_bdev - pointer to EC bdev
 * name - base bdev name
 * is_data_block - whether this is a data block
 * cb_fn - callback function
 * cb_ctx - callback context
 * returns:
 * 0 on success, non-zero on failure
 */
int
ec_bdev_add_base_bdev(struct ec_bdev *ec_bdev, const char *name, bool is_data_block,
		     ec_base_bdev_cb cb_fn, void *cb_ctx)
{
	struct ec_base_bdev_info *base_info;
	uint8_t slot = UINT8_MAX;
	uint8_t data_slots = 0;
	uint8_t parity_slots = 0;

	/* Find an available slot */
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->name == NULL) {
			slot = data_slots + parity_slots;
			break;
		}
		if (base_info->is_data_block) {
			data_slots++;
		} else {
			parity_slots++;
		}
	}

	/* Check if we have space */
	if (is_data_block && data_slots >= ec_bdev->k) {
		SPDK_ERRLOG("No available data slots for EC bdev %s\n", ec_bdev->bdev.name);
		return -ENOSPC;
	}
	if (!is_data_block && parity_slots >= ec_bdev->p) {
		SPDK_ERRLOG("No available parity slots for EC bdev %s\n", ec_bdev->bdev.name);
		return -ENOSPC;
	}

	/* Check if we found an available slot */
	if (slot == UINT8_MAX) {
		SPDK_ERRLOG("No available slots for EC bdev %s\n", ec_bdev->bdev.name);
		return -ENOSPC;
	}

	base_info = &ec_bdev->base_bdev_info[slot];
	base_info->name = strdup(name);
	if (base_info->name == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for base bdev name\n");
		return -ENOMEM;
	}

	base_info->is_data_block = is_data_block;
	base_info->configure_cb = cb_fn;
	base_info->configure_cb_ctx = cb_ctx;

	/* Configure the base bdev */
	return ec_bdev_configure_base_bdev(base_info, false, cb_fn, cb_ctx);
}

/*
 * brief:
 * ec_bdev_remove_base_bdev removes a base bdev from EC bdev
 * params:
 * base_bdev - pointer to base bdev
 * cb_fn - callback function
 * cb_ctx - callback context
 * returns:
 * 0 on success, non-zero on failure
 */
int
ec_bdev_remove_base_bdev(struct spdk_bdev *base_bdev, ec_base_bdev_cb cb_fn, void *cb_ctx)
{
	struct ec_base_bdev_info *base_info = ec_bdev_find_base_info_by_bdev(base_bdev);

	if (base_info == NULL) {
		SPDK_ERRLOG("Base bdev not found\n");
		return -ENODEV;
	}

	base_info->remove_scheduled = true;
		base_info->remove_cb = cb_fn;
		base_info->remove_cb_ctx = cb_ctx;

	ec_bdev_free_base_bdev_resource(base_info);

	if (cb_fn) {
		cb_fn(cb_ctx, 0);
	}

	return 0;
}


/*
 * brief:
 * ec_bdev_configure_base_bdev configures a base bdev
 * params:
 * base_info - pointer to base bdev info
 * existing - whether this is an existing bdev
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

	base_info->app_thread_ch = spdk_bdev_get_io_channel(desc);
	if (base_info->app_thread_ch == NULL) {
		SPDK_ERRLOG("Unable to get I/O channel for base bdev '%s'\n", base_info->name);
		/* Closing the desc will automatically release the claim */
		spdk_bdev_close(desc);
		return -ENOMEM;
	}

	base_info->desc = desc;
	base_info->blockcnt = spdk_bdev_get_num_blocks(bdev);
	base_info->data_offset = EC_BDEV_MIN_DATA_OFFSET_SIZE / spdk_bdev_get_block_size(bdev);
	base_info->data_size = base_info->blockcnt - base_info->data_offset;

	/* Update EC bdev block count */
	if (ec_bdev->bdev.blockcnt == 0) {
		ec_bdev->bdev.blockcnt = base_info->data_size * ec_bdev->k;
		ec_bdev->bdev.blocklen = spdk_bdev_get_block_size(bdev);
		} else {
		uint64_t min_data_size = base_info->data_size;
		struct ec_base_bdev_info *iter;

			EC_FOR_EACH_BASE_BDEV(ec_bdev, iter) {
			if (iter->desc != NULL && iter->data_size < min_data_size) {
				min_data_size = iter->data_size;
			}
		}

		ec_bdev->bdev.blockcnt = min_data_size * ec_bdev->k;
	}

	base_info->is_configured = true;
	ec_bdev->num_base_bdevs_discovered++;
	ec_bdev->num_base_bdevs_operational++;
	
	assert(ec_bdev->num_base_bdevs_discovered <= ec_bdev->num_base_bdevs);
	assert(ec_bdev->num_base_bdevs_operational <= ec_bdev->num_base_bdevs);
	assert(ec_bdev->num_base_bdevs_operational >= ec_bdev->min_base_bdevs_operational);

	/* Continue configuration */
	ec_bdev_configure_cont(ec_bdev);

	if (cb_fn) {
		cb_fn(cb_ctx, 0);
	}

	return 0;
}

/*
 * brief:
 * ec_bdev_event_cb handles EC bdev events (for self_desc)
 * params:
 * event - event type
 * bdev - pointer to EC bdev
 * event_ctx - event context (ec_bdev pointer)
 * returns:
 * none
 */
static void
ec_bdev_event_cb(enum spdk_bdev_event_type event, struct spdk_bdev *bdev, void *event_ctx)
{
	struct ec_bdev *ec_bdev = event_ctx;

	switch (event) {
	case SPDK_BDEV_EVENT_REMOVE:
		/* Handle EC bdev removal - similar to raid module */
		SPDK_DEBUGLOG(bdev_ec, "EC bdev %s removal event\n", ec_bdev->bdev.name);
		
		/* Check if ec_bdev is still in the list. If not, deletion is already in progress. */
		struct ec_bdev *iter;
		bool found = false;
		TAILQ_FOREACH(iter, &g_ec_bdev_list, global_link) {
			if (iter == ec_bdev) {
				found = true;
				break;
			}
		}
		
		if (!found) {
			/* EC bdev already removed from list, deletion is in progress.
			 * This event was likely triggered by spdk_bdev_unregister called
			 * from ec_bdev_deconfigure. The deletion will complete through
			 * the normal flow (ec_bdev_deconfigure_unregister_done callback).
			 */
			SPDK_DEBUGLOG(bdev_ec, "EC bdev %s already removed from list, deletion in progress, ignoring event\n",
				      ec_bdev->bdev.name);
			return;
		}
		
		/* Check if deletion is already in progress via deconfigure callback.
		 * If deconfigure_cb_fn is set, it means ec_bdev_deconfigure was called
		 * and deletion is already in progress. In this case, we should not
		 * call ec_bdev_delete again, as it would overwrite the callback.
		 */
		if (ec_bdev->deconfigure_cb_fn != NULL) {
			SPDK_DEBUGLOG(bdev_ec, "EC bdev %s deletion already in progress (deconfigure_cb_fn set), ignoring event\n",
				      ec_bdev->bdev.name);
			return;
		}
		
		/* This is an external removal (not via RPC delete).
		 * The bdev layer will handle unregistering, we just need to clean up.
		 */
		ec_bdev_delete(ec_bdev, NULL, NULL);
		break;
	case SPDK_BDEV_EVENT_RESIZE:
		/* EC bdev resize is not supported */
		SPDK_NOTICELOG("EC bdev %s resize event is not supported\n", ec_bdev->bdev.name);
		break;
	case SPDK_BDEV_EVENT_MEDIA_MANAGEMENT:
		/* EC bdev media management event is not supported */
		SPDK_NOTICELOG("EC bdev %s media management event is not supported\n", ec_bdev->bdev.name);
		break;
	default:
		SPDK_WARNLOG("Unsupported bdev event type %d for EC bdev %s\n", event, ec_bdev->bdev.name);
		break;
	}
}

/*
 * brief:
 * ec_bdev_event_base_bdev handles base bdev events
 * params:
 * event - event type
 * bdev - pointer to base bdev
 * event_ctx - event context
 * returns:
 * none
 */
static void
ec_bdev_event_base_bdev(enum spdk_bdev_event_type event, struct spdk_bdev *bdev, void *event_ctx)
{
	struct ec_base_bdev_info *base_info = event_ctx;

	switch (event) {
	case SPDK_BDEV_EVENT_REMOVE:
		base_info->remove_scheduled = true;
		if (base_info->remove_cb) {
			base_info->remove_cb(base_info->remove_cb_ctx, 0);
		}
		break;
	default:
		SPDK_WARNLOG("Unsupported bdev event: %d\n", event);
				break;
	}
}

/*
 * brief:
 * ec_bdev_examine_cont_wrapper wraps ec_bdev_examine_cont to match ec_bdev_examine_load_sb_cb signature
 */
static void
ec_bdev_examine_cont_wrapper(struct spdk_bdev *bdev, const struct ec_bdev_superblock *sb,
			      int status, void *cb_ctx)
{
	/* cb_ctx is actually a pointer to a structure containing cb_fn and cb_ctx */
	/* For now, we'll just call examine_cont with NULL callback since examine_cont handles its own callbacks */
	ec_bdev_examine_cont(sb, bdev, NULL, NULL);
}

/*
 * brief:
 * ec_bdev_examine_cont continues examine process
 * params:
 * sb - pointer to superblock
 * bdev - pointer to base bdev
 * cb_fn - callback function
 * cb_ctx - callback context
 * returns:
 * none
 */
static void
ec_bdev_examine_cont(const struct ec_bdev_superblock *sb, struct spdk_bdev *bdev,
		   ec_base_bdev_cb cb_fn, void *cb_ctx)
{
	const struct ec_bdev_sb_base_bdev *sb_base_bdev = NULL;
	struct ec_bdev *ec_bdev;
	struct ec_base_bdev_info *base_info, *iter;
	uint8_t i;
	int rc = 0;

	if (sb == NULL) {
		SPDK_DEBUGLOG(bdev_ec, "No superblock found on bdev %s\n", bdev->name);
		if (cb_fn) {
			cb_fn(cb_ctx, 0);
		}
		return;
	}

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
		SPDK_ERRLOG("Base bdev %s not found in EC bdev %s\n", bdev->name, ec_bdev->bdev.name);
		rc = -ENODEV;
		goto out;
	}

	if (base_info->is_configured) {
		SPDK_DEBUGLOG(bdev_ec, "Base bdev %s already configured for EC bdev %s\n",
			      bdev->name, ec_bdev->bdev.name);
		rc = 0;
		goto out;
	}

	rc = ec_bdev_configure_base_bdev(base_info, true, cb_fn, cb_ctx);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to configure bdev %s as base bdev of EC %s: %s\n",
			    bdev->name, ec_bdev->bdev.name, spdk_strerror(-rc));
	}

out:
	if (rc != 0 && cb_fn) {
		cb_fn(cb_ctx, rc);
	}
}

/* ec_bdev_examine_sb and ec_bdev_examine_no_sb merged into callers */

/*
 * brief:
 * ec_bdev_examine_others examines other base bdevs
 * params:
 * ctx - pointer to examine context
 * status - status
 * returns:
 * none
 */
static void
ec_bdev_examine_others(void *_ctx, int status)
{
	struct ec_bdev_examine_others_ctx *ctx = _ctx;
	struct ec_bdev *ec_bdev;
	struct ec_bdev_sb_base_bdev *sb_base_bdev;
	struct ec_base_bdev_info *base_info;
	uint8_t i;

	ec_bdev = ec_bdev_find_by_uuid(&ctx->ec_bdev_uuid);
	if (ec_bdev == NULL) {
		SPDK_ERRLOG("EC bdev not found\n");
		if (ctx->cb_fn) {
			ctx->cb_fn(ctx->cb_ctx, -ENODEV);
		}
	free(ctx);
		return;
	}

	/* Examine other base bdevs */
	for (i = 0; i < ec_bdev->sb->base_bdevs_size; i++) {
		sb_base_bdev = &ec_bdev->sb->base_bdevs[i];

		if (sb_base_bdev->state != EC_SB_BASE_BDEV_CONFIGURED) {
			continue;
		}

		base_info = NULL;
		EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
			if (spdk_uuid_compare(&base_info->uuid, &sb_base_bdev->uuid) == 0) {
				break;
			}
		}

		if (base_info == NULL || base_info->is_configured) {
			continue;
		}

		/* Examine this base bdev by UUID */
		{
			char uuid_str[SPDK_UUID_STRING_LEN];
			struct spdk_bdev *target_bdev;

			spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &sb_base_bdev->uuid);
			target_bdev = spdk_bdev_get_by_name(uuid_str);
			if (target_bdev == NULL) {
				/* Bdev not found yet, skip it */
				continue;
			}

			/* Examine this base bdev */
			ec_bdev_examine_load_sb(spdk_bdev_get_name(target_bdev), ec_bdev_examine_cont_wrapper, NULL);
		}
	}

	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_ctx, status);
	}
	free(ctx);
}

/* ec_bdev_examine_done merged into ec_bdev_examine */

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
		SPDK_DEBUGLOG(bdev_ec, "No superblock found on bdev %s\n", bdev->name);
		rc = 0;
		goto done;
	}

	rc = ec_bdev_examine_load_sb(bdev->name, ec_bdev_examine_cont_wrapper, NULL);
	if (rc != 0) {
		goto done;
	}

	return;
done:
	SPDK_DEBUGLOG(bdev_ec, "Examine done for bdev %s with status %d\n", bdev->name, rc);
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
