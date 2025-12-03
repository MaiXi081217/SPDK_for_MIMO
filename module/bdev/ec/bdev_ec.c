#include "bdev_ec.h"
#include "bdev_ec_internal.h"
#include "spdk/bdev_module.h"
#include "spdk/bdev.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/crc32.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/likely.h"
#include "spdk/queue.h"
#include "spdk/vmd.h"
#include "spdk/nvme.h"

/* Forward declarations for NVMe bdev functions - use weak linking if available */
extern struct spdk_nvme_ctrlr *bdev_nvme_get_ctrlr(struct spdk_bdev *bdev) __attribute__((weak));
extern struct spdk_pci_device *spdk_nvme_ctrlr_get_pci_device(struct spdk_nvme_ctrlr *ctrlr) __attribute__((weak));

static bool g_shutdown_started = false;

/* Global configuration: Enable/disable EC encoding dedicated worker threads
 * Can be controlled via command line argument -E in mimo_tgt
 * Default: true (enabled)
 */
bool g_ec_encode_workers_enabled = true;

/* Global zero buffer for reuse (similar to FTL's g_ftl_write_buf/g_ftl_read_buf)
 * This reduces allocation overhead for operations requiring zero data (e.g., superblock wipe)
 * Size: 1MB (0x100000) - same as FTL_ZERO_BUFFER_SIZE
 */
#define EC_ZERO_BUFFER_SIZE 0x100000
void *g_ec_zero_buf = NULL;

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
static void ec_bdev_examine_no_sb(struct spdk_bdev *bdev);
static int ec_bdev_init(void);
static void ec_bdev_deconfigure(struct ec_bdev *ec_bdev, ec_bdev_destruct_cb cb_fn, void *cb_arg);
static void ec_bdev_cleanup(struct ec_bdev *ec_bdev);
static void ec_bdev_configure_cont(struct ec_bdev *ec_bdev);
static void ec_bdev_configure_write_sb_cb(int status, struct ec_bdev *ec_bdev, void *ctx);
static void ec_bdev_configure_register_bdev(struct ec_bdev *ec_bdev);
static int ec_bdev_configure(struct ec_bdev *ec_bdev, ec_bdev_configure_cb cb, void *cb_ctx);
static int ec_start(struct ec_bdev *ec_bdev);
static void ec_stop(struct ec_bdev *ec_bdev);
static struct ec_bdev *ec_bdev_find_by_uuid(const struct spdk_uuid *uuid);
static struct ec_base_bdev_info *ec_bdev_find_base_info_by_bdev(struct spdk_bdev *base_bdev);
/* ec_rebuild_state_to_str is now public - declared in bdev_ec.h */
static int ec_bdev_create_from_sb(const struct ec_bdev_superblock *sb, struct ec_bdev **ec_bdev_out);
static int ec_bdev_configure_base_bdev(struct ec_base_bdev_info *base_info, bool existing,
				       ec_base_bdev_cb cb_fn, void *cb_ctx);
static void ec_bdev_event_base_bdev(enum spdk_bdev_event_type event, struct spdk_bdev *bdev, void *event_ctx);
static void ec_bdev_event_cb(enum spdk_bdev_event_type event, struct spdk_bdev *bdev, void *event_ctx);
static void ec_bdev_remove_base_bdev_on_unquiesced(void *ctx, int status);
static void ec_bdev_remove_base_bdev_on_quiesced(void *ctx, int status);
static void ec_bdev_remove_base_bdev_cont(struct ec_base_bdev_info *base_info);
static void ec_bdev_remove_base_bdev_reset_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void ec_bdev_remove_base_bdev_do_remove(struct ec_base_bdev_info *base_info);
static void ec_bdev_deconfigure_base_bdev(struct ec_base_bdev_info *base_info);
static void ec_bdev_channel_remove_base_bdev(struct spdk_io_channel_iter *i);
static void ec_bdev_channels_remove_base_bdev_done(struct spdk_io_channel_iter *i, int status);
static void ec_bdev_remove_base_bdev_write_sb_cb(int status, struct ec_bdev *ec_bdev, void *ctx);
static int ec_bdev_remove_base_bdev_quiesce(struct ec_base_bdev_info *base_info);
static int ec_bdev_process_add_finish_action(struct ec_bdev_process *process, spdk_msg_fn cb, void *cb_ctx);
static int ec_bdev_process_base_bdev_remove(struct ec_bdev_process *process,
					    struct ec_base_bdev_info *base_info);
static void ec_bdev_examine_cont(const struct ec_bdev_superblock *sb, struct spdk_bdev *bdev,
				 ec_base_bdev_cb cb_fn, void *cb_ctx);
static void ec_bdev_examine_others(void *_ctx, int status);

/* Forward declare module symbol for early use */
extern struct spdk_bdev_module g_ec_if;

/* Examine-done helper to match SPDK contract */
static void
ec_bdev_examine_done_cb(void *ctx, int status)
{
    struct spdk_bdev *bdev = ctx;

    if (status != 0 && bdev != NULL) {
        SPDK_ERRLOG("Failed to examine bdev %s: %s\n",
                    bdev->name, spdk_strerror(-status));
    }
    spdk_bdev_module_examine_done(&g_ec_if);
}

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

	/* Initialize buffer pools */
	SLIST_INIT(&ec_ch->parity_buf_pool);
	SLIST_INIT(&ec_ch->rmw_stripe_buf_pool);
	SLIST_INIT(&ec_ch->temp_data_buf_pool);
	ec_ch->parity_buf_count = 0;
	ec_ch->rmw_buf_count = 0;
	ec_ch->temp_data_buf_count = 0;
	
	/* RAID5F-style: Initialize stripe_private object pool */
	TAILQ_INIT(&ec_ch->free_stripe_privs);
	TAILQ_INIT(&ec_ch->encode_retry_queue);
	
	/* RAID5F-style: Initialize encoding retry queue */
	TAILQ_INIT(&ec_ch->encode_retry_queue);
	TAILQ_INIT(&ec_ch->encode_retry_queue);
	ec_ch->parity_buf_size = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
	ec_ch->rmw_buf_size = ec_ch->parity_buf_size * ec_bdev->k;
	ec_ch->temp_data_buf_size = ec_ch->parity_buf_size;  /* Same size as parity buffers */
	
	/* Optimized: Cache alignment value to avoid repeated lookups from ec_bdev
	 * This reduces memory access overhead in hot path (buffer allocation)
	 */
	ec_ch->cached_alignment = (ec_bdev->buf_alignment > 0) ?
		ec_bdev->buf_alignment : EC_BDEV_DEFAULT_BUF_ALIGNMENT;

	/* Pre-allocate buffers for better performance */
	size_t align = ec_ch->cached_alignment;
	struct ec_parity_buf_entry *entry;
	unsigned char *buf;
	uint8_t j;
	uint8_t parity_prealloc = ec_bdev->p * 8;
	uint8_t rmw_prealloc = 16;
	uint8_t temp_data_prealloc = ec_bdev->k * 2;
	
	if (parity_prealloc > 128) parity_prealloc = 128;
	if (rmw_prealloc > 64) rmw_prealloc = 64;
	if (temp_data_prealloc > 32) temp_data_prealloc = 32;

	for (j = 0; j < parity_prealloc && ec_ch->parity_buf_count < 128; j++) {
		buf = spdk_dma_malloc(ec_ch->parity_buf_size, align, NULL);
		if (buf != NULL) {
			entry = malloc(sizeof(*entry));
			if (entry != NULL) {
				entry->buf = buf;
				SLIST_INSERT_HEAD(&ec_ch->parity_buf_pool, entry, link);
				ec_ch->parity_buf_count++;
			} else {
				spdk_dma_free(buf);
			}
		}
	}

	for (j = 0; j < rmw_prealloc && ec_ch->rmw_buf_count < 64; j++) {
		buf = spdk_dma_malloc(ec_ch->rmw_buf_size, align, NULL);
		if (buf != NULL) {
			entry = malloc(sizeof(*entry));
			if (entry != NULL) {
				entry->buf = buf;
				SLIST_INSERT_HEAD(&ec_ch->rmw_stripe_buf_pool, entry, link);
				ec_ch->rmw_buf_count++;
			} else {
				spdk_dma_free(buf);
			}
		}
	}

	for (j = 0; j < temp_data_prealloc && ec_ch->temp_data_buf_count < 32; j++) {
		buf = spdk_dma_malloc(ec_ch->temp_data_buf_size, align, NULL);
		if (buf != NULL) {
			entry = malloc(sizeof(*entry));
			if (entry != NULL) {
				entry->buf = buf;
				SLIST_INSERT_HEAD(&ec_ch->temp_data_buf_pool, entry, link);
				ec_ch->temp_data_buf_count++;
			} else {
				spdk_dma_free(buf);
			}
		}
	}

	/* RAID5F-style: Pre-allocate stripe_private objects for object pool
	 * Pre-allocate EC_MAX_STRIPES objects to avoid malloc overhead in hot path
	 * With sufficient memory, larger pool size improves performance
	 */
	struct ec_stripe_private *stripe_priv;
	uint8_t k = ec_bdev->k;
	uint8_t p = ec_bdev->p;
	uint8_t stripe_idx;
	
	for (stripe_idx = 0; stripe_idx < EC_MAX_STRIPES; stripe_idx++) {
		stripe_priv = malloc(sizeof(*stripe_priv));
		if (stripe_priv == NULL) {
			SPDK_WARNLOG("Failed to pre-allocate stripe_private %u\n", stripe_idx);
			break;
		}
		memset(stripe_priv, 0, sizeof(*stripe_priv));
		stripe_priv->from_pool = true;  /* Pre-allocated objects are marked as from pool */
		
		if (ec_stripe_private_init_chunks(stripe_priv, k, p) != 0) {
			free(stripe_priv);
			SPDK_WARNLOG("Failed to init chunks for stripe_private %u\n", stripe_idx);
			break;
		}
		
		TAILQ_INSERT_HEAD(&ec_ch->free_stripe_privs, stripe_priv, link);
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

	/* Free buffer pools */
	struct ec_parity_buf_entry *buf_entry, *buf_entry_tmp;
	SLIST_FOREACH_SAFE(buf_entry, &ec_ch->parity_buf_pool, link, buf_entry_tmp) {
		SLIST_REMOVE(&ec_ch->parity_buf_pool, buf_entry, ec_parity_buf_entry, link);
		if (buf_entry->buf != NULL) {
			spdk_dma_free(buf_entry->buf);
		}
		free(buf_entry);
	}
	ec_ch->parity_buf_count = 0;
	
	SLIST_FOREACH_SAFE(buf_entry, &ec_ch->rmw_stripe_buf_pool, link, buf_entry_tmp) {
		SLIST_REMOVE(&ec_ch->rmw_stripe_buf_pool, buf_entry, ec_parity_buf_entry, link);
		if (buf_entry->buf != NULL) {
			spdk_dma_free(buf_entry->buf);
		}
		free(buf_entry);
	}
	ec_ch->rmw_buf_count = 0;
	
	/* Optimized: Free temporary data buffer pool */
	SLIST_FOREACH_SAFE(buf_entry, &ec_ch->temp_data_buf_pool, link, buf_entry_tmp) {
		SLIST_REMOVE(&ec_ch->temp_data_buf_pool, buf_entry, ec_parity_buf_entry, link);
		if (buf_entry->buf != NULL) {
			spdk_dma_free(buf_entry->buf);
		}
		free(buf_entry);
	}
	ec_ch->temp_data_buf_count = 0;
	
	/* RAID5F-style: Free stripe_private object pool */
	struct ec_stripe_private *stripe_priv;
	while ((stripe_priv = TAILQ_FIRST(&ec_ch->free_stripe_privs)) != NULL) {
		TAILQ_REMOVE(&ec_ch->free_stripe_privs, stripe_priv, link);
		
		/* Free chunk iov arrays */
		uint8_t k = ec_bdev->k;
		uint8_t p = ec_bdev->p;
		uint8_t j;
		
		for (j = 0; j < k; j++) {
			free(stripe_priv->data_chunks[j].iovs);
		}
		for (j = 0; j < p; j++) {
			free(stripe_priv->parity_chunks[j].iovs);
		}
		
		free(stripe_priv);
	}

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

	/* Output rebuild progress if rebuild is in progress */
	if (ec_bdev->rebuild_ctx != NULL && !ec_bdev->rebuild_ctx->paused) {
		struct ec_rebuild_context *rebuild_ctx = ec_bdev->rebuild_ctx;
		double percent = 0.0;

		if (rebuild_ctx->total_stripes > 0) {
			percent = (double)rebuild_ctx->current_stripe * 100.0 /
				  (double)rebuild_ctx->total_stripes;
		}

		spdk_json_write_named_object_begin(w, "rebuild");
		spdk_json_write_named_string(w, "target", rebuild_ctx->target_base_info->name);
		spdk_json_write_named_uint8(w, "target_slot", rebuild_ctx->target_slot);
		spdk_json_write_named_object_begin(w, "progress");
		spdk_json_write_named_uint64(w, "current_stripe", rebuild_ctx->current_stripe);
		spdk_json_write_named_uint64(w, "total_stripes", rebuild_ctx->total_stripes);
		spdk_json_write_named_double(w, "percent", percent);
		spdk_json_write_named_string(w, "state", ec_rebuild_state_to_str(rebuild_ctx->state));
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}

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
			spdk_json_write_object_end(w);
		} else {
			char str[32];

			snprintf(str, sizeof(str), "removed_base_bdev_%u", 
				 (uint8_t)(base_info - ec_bdev->base_bdev_info));
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "name", str);
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
	
	/* Optimized: Initialize base_info_map for O(1) lookup in completion callback */
	memset(ec_io->base_bdev_idx_map, 0xFF, sizeof(ec_io->base_bdev_idx_map));
	memset(ec_io->base_info_map, 0, sizeof(ec_io->base_info_map));

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
	struct ec_bdev *ec_bdev = spdk_io_channel_get_io_device(ch);

	/* Check if EC bdev is in ONLINE state before processing IO */
	if (ec_bdev->state != EC_BDEV_STATE_ONLINE) {
		SPDK_ERRLOG("EC bdev '%s' is not ONLINE (state: %s), rejecting IO request\n",
			    ec_bdev->bdev.name ? ec_bdev->bdev.name : "unknown",
			    ec_bdev_state_to_str(ec_bdev->state));
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

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

	SPDK_DEBUGLOG(bdev_ec, "ec_bdev_destruct for EC bdev %s, shutdown_started=%d\n",
		      ec_bdev->bdev.name, g_shutdown_started);

	/* During shutdown, don't free base bdev resources immediately if superblock
	 * wipe is in progress. The resources will be freed after superblock wipe
	 * completes or is aborted. */
	if (g_shutdown_started) {
		ec_bdev->state = EC_BDEV_STATE_OFFLINE;
		/* Mark base bdevs for removal, but don't free resources yet if
		 * superblock wipe might be in progress */
		EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
			base_info->remove_scheduled = true;
			/* Only free resources if base bdev is not configured or
			 * if we're sure no superblock operations are in progress */
			if (!base_info->is_configured) {
				ec_bdev_free_base_bdev_resource(base_info);
			}
		}
	} else {
		/* Normal deletion - free resources for scheduled removals */
		EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
			if (base_info->remove_scheduled == true) {
				ec_bdev_free_base_bdev_resource(base_info);
			}
		}
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
	size_t alignment = 0;
	struct spdk_bdev *base_bdev;
	int rc;

	/* Find minimum data size and maximum alignment from all base bdevs */
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

		/* Get maximum alignment requirement from base bdevs */
		base_bdev = spdk_bdev_desc_get_bdev(base_info->desc);
		alignment = spdk_max(alignment, spdk_bdev_get_buf_align(base_bdev));
	}

	/* Store alignment for memory allocations */
	ec_bdev->buf_alignment = alignment > 0 ? alignment : 0x1000; /* Default 4KB if no base bdevs */

	if (min_data_size == UINT64_MAX) {
		SPDK_ERRLOG("No valid base bdevs found for EC bdev %s\n", ec_bdev->bdev.name);
		return -ENODEV;
	}

	data_block_size = spdk_bdev_get_data_block_size(&ec_bdev->bdev);

	/* Convert strip_size_kb to blocks */
	ec_bdev->strip_size = (ec_bdev->strip_size_kb * 1024) / data_block_size;
	if (ec_bdev->strip_size == 0) {
		SPDK_ERRLOG("Strip size cannot be smaller than the device block size\n");
		return -EINVAL;
	}
	ec_bdev->strip_size_shift = spdk_u32log2(ec_bdev->strip_size);

	/* Align base bdev data size to strip_size boundary (similar to RAID) */
	uint64_t base_bdev_data_size = (min_data_size >> ec_bdev->strip_size_shift) << ec_bdev->strip_size_shift;

	/* Update all base bdev data_size to aligned value */
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->desc != NULL) {
			base_info->data_size = base_bdev_data_size;
		}
	}

	/* EC bdev size is k * aligned_data_size (k data blocks per stripe) */
	ec_bdev->bdev.blockcnt = base_bdev_data_size * k;

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

	rc = ec_bdev_init_encode_workers(ec_bdev);
	if (rc != 0) {
		ec_bdev_cleanup_tables(ec_bdev);
		return rc;
	}

	/* Set optimal_io_boundary to full stripe size for optimal write performance
	 * This allows bdev layer to automatically split I/O at full stripe boundaries,
	 * enabling efficient full stripe writes instead of splitting at strip boundaries.
	 * 
	 * For EC, the optimal I/O boundary should be a full stripe (strip_size * k),
	 * not a single strip, to maximize full stripe write performance.
	 * 
	 * Only set if we have multiple base bdevs (k + p > 1), similar to RAID0
	 */
	if (ec_bdev->num_base_bdevs > 1) {
		/* Set optimal_io_boundary to full stripe size (strip_size * k) */
		ec_bdev->bdev.optimal_io_boundary = ec_bdev->strip_size * k;
		ec_bdev->bdev.split_on_optimal_io_boundary = true;
		
		/* Set write_unit_size to full stripe size (strip_size * k) as a hint for optimal write performance
		 * Note: We do NOT set split_on_write_unit to avoid rejecting small writes (< write_unit_size).
		 * Instead, we rely on optimal_io_boundary splitting and EC module's RMW handling for partial stripes.
		 * This allows both small writes (via RMW) and large writes (via full stripe path) to work correctly.
		 */
		ec_bdev->bdev.write_unit_size = ec_bdev->strip_size * k;
		ec_bdev->bdev.split_on_write_unit = false;
	} else {
		/* Do not need to split reads/writes on single bdev EC modules. */
		ec_bdev->bdev.optimal_io_boundary = 0;
		ec_bdev->bdev.split_on_optimal_io_boundary = false;
		ec_bdev->bdev.write_unit_size = 0;
		ec_bdev->bdev.split_on_write_unit = false;
	}

	/* Initialize device selection configuration (fault tolerance + wear leveling) */
	rc = ec_bdev_init_selection_config(ec_bdev);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to initialize device selection configuration: %s\n",
			    spdk_strerror(-rc));
		ec_bdev_cleanup_encode_workers(ec_bdev);
		ec_bdev_cleanup_tables(ec_bdev);
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
		ec_bdev_cleanup_encode_workers(ec_bdev);
		ec_bdev_cleanup_selection_config(ec_bdev);
		ec_bdev_cleanup_tables(ec_bdev);
		free(ec_bdev->module_private);
		ec_bdev->module_private = NULL;
	}
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
 * Rebuild state names for EC bdev
 */
static const char *g_ec_rebuild_state_names[] = {
	[EC_REBUILD_STATE_IDLE]		= "idle",
	[EC_REBUILD_STATE_READING]	= "reading",
	[EC_REBUILD_STATE_DECODING]	= "decoding",
	[EC_REBUILD_STATE_WRITING]	= "writing"
};

const char *
ec_rebuild_state_to_str(enum ec_rebuild_state state)
{
	if (state > EC_REBUILD_STATE_WRITING) {
		return "unknown";
	}

	return g_ec_rebuild_state_names[state];
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
	struct ec_base_bdev_info *base_info;
	ec_bdev_destruct_cb user_cb_fn = ec_bdev->deconfigure_cb_fn;
	void *user_cb_arg = ec_bdev->deconfigure_cb_arg;

	SPDK_DEBUGLOG(bdev_ec, "ec_bdev_deconfigure_unregister_done for EC bdev %s, rc=%d\n",
		      ec_bdev->bdev.name, rc);

	/* Clear the stored callbacks */
	ec_bdev->deconfigure_cb_fn = NULL;
	ec_bdev->deconfigure_cb_arg = NULL;

	/* Close self_desc before cleaning up base bdevs */
	if (ec_bdev->self_desc != NULL) {
		spdk_bdev_close(ec_bdev->self_desc);
		ec_bdev->self_desc = NULL;
	}

	/* Now that bdev is unregistered, we can safely free base bdev resources.
	 * During shutdown, resources may have been marked for removal but not freed
	 * yet to allow superblock wipe to complete. */
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->remove_scheduled) {
			ec_bdev_free_base_bdev_resource(base_info);
		}
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

	/* Unregister bdev - this will automatically unregister IO device as well */
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

	SPDK_DEBUGLOG(bdev_ec, "ec_bdev_delete_continue for EC bdev %s, state=%d\n",
		      ec_bdev->bdev.name, ec_bdev->state);

	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		base_info->remove_scheduled = true;

		if (ec_bdev->state != EC_BDEV_STATE_ONLINE) {
			/*
			 * As EC bdev is not registered yet or already unregistered,
			 * so cleanup should be done here itself.
			 * However, during shutdown, don't free resources immediately
			 * if they might still be in use by superblock operations.
			 */
			if (!g_shutdown_started || !base_info->is_configured) {
				ec_bdev_free_base_bdev_resource(base_info);
			}
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
	struct ec_bdev *ec_bdev;

	if (base_info == NULL) {
		return;
	}

	ec_bdev = base_info->ec_bdev;

	/* Check if this base bdev was operational before closing resources */
	bool was_operational = (base_info->is_configured && 
				(base_info->desc != NULL || base_info->app_thread_ch != NULL));

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
	
	/* Update counters if this base bdev was configured */
	if (base_info->is_configured && ec_bdev != NULL) {
		assert(ec_bdev->num_base_bdevs_discovered > 0);
		ec_bdev->num_base_bdevs_discovered--;
		/* Decrement operational counter if it was operational */
		if (was_operational && ec_bdev->num_base_bdevs_operational > 0) {
			ec_bdev->num_base_bdevs_operational--;
		}
	}
	
	base_info->is_configured = false;
	
	/* NOTE: Do NOT clear remove_scheduled here - it must be cleared in ec_bdev_remove_base_bdev_done
	 * to ensure proper state tracking during the removal process. Clearing it here would cause
	 * assertion failures in ec_bdev_remove_base_bdev_done when called from unquiesce callback.
	 */
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

		ec_bdev_cleanup_encode_workers(ec_bdev);
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

	rc = ec_bdev_sb_reserve_buffer(ec_bdev, sb->length);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to reserve superblock buffer: %s\n", spdk_strerror(-rc));
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
		/* is_data_block is kept for superblock compatibility but not used */
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

	SPDK_DEBUGLOG(bdev_ec, "examine_load_sb_cb_wrapper called for bdev %s, status=%d, sb=%p\n",
		      bdev ? bdev->name : "unknown", status, sb);
	if (status != 0) {
		SPDK_DEBUGLOG(bdev_ec, "Superblock load failed for bdev %s: %s\n",
			      bdev ? bdev->name : "unknown", spdk_strerror(-status));
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
		SPDK_DEBUGLOG(bdev_ec, "Failed to initiate superblock load for bdev %s: %s\n",
			      bdev_name, spdk_strerror(-rc));
		spdk_put_io_channel(examine_ctx->ch);
		spdk_bdev_close(examine_ctx->desc);
		free(examine_ctx);
		cb(NULL, NULL, rc, cb_ctx);
		return rc;
	}

	SPDK_DEBUGLOG(bdev_ec, "Superblock load initiated for bdev %s, waiting for async completion\n",
		      bdev_name);
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
	struct ec_bdev *ec_bdev;
	struct ec_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_ec, "ec_bdev_fini_start\n");

	/* Similar to RAID module: during shutdown, don't actively delete EC bdevs.
	 * The bdev layer will automatically call ec_bdev_destruct for each EC bdev.
	 * We just need to:
	 * 1. Free resources for non-online EC bdevs
	 * 2. Set g_shutdown_started flag so destruct knows we're shutting down
	 */
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

	/* Free global zero buffer */
	if (g_ec_zero_buf != NULL) {
		spdk_free(g_ec_zero_buf);
		g_ec_zero_buf = NULL;
	}
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
	/* Allocate global zero buffer for reuse (similar to FTL)
	 * This reduces allocation overhead for operations requiring zero data
	 */
	g_ec_zero_buf = spdk_zmalloc(EC_ZERO_BUFFER_SIZE, EC_ZERO_BUFFER_SIZE, NULL,
				     SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!g_ec_zero_buf) {
		SPDK_ERRLOG("Failed to allocate global zero buffer\n");
		return -ENOMEM;
	}

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

	/* Check if EC bdev is already registered or in the process of being registered */
	if (ec_bdev->state == EC_BDEV_STATE_ONLINE) {
		struct spdk_bdev *registered_bdev = spdk_bdev_get_by_name(ec_bdev->bdev.name);
		if (registered_bdev != NULL && registered_bdev == &ec_bdev->bdev) {
			/* Already registered, nothing to do */
			SPDK_DEBUGLOG(bdev_ec, "EC bdev %s already registered, skipping configure_cont\n",
				      ec_bdev->bdev.name);
			return;
		}
	}

	/* Count configured base bdevs with early exit optimization */
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->is_configured) {
			num_configured++;
			/* Early exit: if we have all configured, stop counting */
			if (num_configured == num_base_bdevs) {
				break;
			}
		}
	}

	if (num_configured == num_base_bdevs) {
		/* All base bdevs are configured, start the EC bdev */
		rc = ec_start(ec_bdev);
		
		if (rc == 0) {
			/* If superblock is enabled, write it before registering bdev */
			if (ec_bdev->superblock_enabled && ec_bdev->sb != NULL) {
				/* Update superblock with latest information */
				ec_bdev_init_superblock(ec_bdev);
				/* Write superblock asynchronously - continue in callback */
				ec_bdev_write_superblock(ec_bdev, ec_bdev_configure_write_sb_cb, ec_bdev);
				return;  /* Don't call configure_cb here, it will be called in write_sb callback */
			}
			
			/* No superblock or superblock write not needed, continue with registration */
			ec_bdev_configure_register_bdev(ec_bdev);
		} else {
			SPDK_ERRLOG("Failed to start EC bdev %s\n", ec_bdev->bdev.name);
			ec_bdev->state = EC_BDEV_STATE_OFFLINE;
			rc = -1; /* Set rc to error for callback */
			if (ec_bdev->configure_cb != NULL) {
				ec_bdev->configure_cb(ec_bdev->configure_cb_ctx, rc);
				ec_bdev->configure_cb = NULL;
				ec_bdev->configure_cb_ctx = NULL;
			}
		}
	} else {
		rc = 0; /* Still configuring, not an error */
		if (ec_bdev->configure_cb != NULL) {
			ec_bdev->configure_cb(ec_bdev->configure_cb_ctx, rc);
			ec_bdev->configure_cb = NULL;
			ec_bdev->configure_cb_ctx = NULL;
		}
	}
}

/*
 * brief:
 * ec_bdev_configure_write_sb_cb callback for superblock write during configuration
 * params:
 * status - write status
 * ec_bdev - pointer to EC bdev
 * ctx - context (ec_bdev pointer)
 * returns:
 * none
 */
static void
ec_bdev_configure_write_sb_cb(int status, struct ec_bdev *ec_bdev, void *ctx)
{
	int rc = 0;
	
	if (status == 0) {
		/* Superblock written successfully, continue with registration */
		ec_bdev_configure_register_bdev(ec_bdev);
	} else {
		SPDK_ERRLOG("Failed to write EC bdev '%s' superblock: %s\n",
			    ec_bdev->bdev.name, spdk_strerror(-status));
		ec_stop(ec_bdev);
		ec_bdev->state = EC_BDEV_STATE_CONFIGURING;
		rc = status;
	}
	
	if (ec_bdev->configure_cb != NULL) {
		ec_bdev->configure_cb(ec_bdev->configure_cb_ctx, rc);
		ec_bdev->configure_cb = NULL;
		ec_bdev->configure_cb_ctx = NULL;
	}
}

/*
 * brief:
 * ec_bdev_configure_register_bdev registers EC bdev after configuration
 * params:
 * ec_bdev - pointer to EC bdev
 * returns:
 * none
 */
static void
ec_bdev_configure_register_bdev(struct ec_bdev *ec_bdev)
{
	int rc;
	
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
		if (ec_bdev->configure_cb != NULL) {
			ec_bdev->configure_cb(ec_bdev->configure_cb_ctx, rc);
			ec_bdev->configure_cb = NULL;
			ec_bdev->configure_cb_ctx = NULL;
		}
		return;
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
		if (ec_bdev->configure_cb != NULL) {
			ec_bdev->configure_cb(ec_bdev->configure_cb_ctx, rc);
			ec_bdev->configure_cb = NULL;
			ec_bdev->configure_cb_ctx = NULL;
		}
		return;
	}
	
	SPDK_DEBUGLOG(bdev_ec, "EC bdev generic %p\n", &ec_bdev->bdev);
	SPDK_DEBUGLOG(bdev_ec, "EC bdev is created with name %s, ec_bdev %p\n",
		      ec_bdev->bdev.name, ec_bdev);
	
	if (ec_bdev->configure_cb != NULL) {
		ec_bdev->configure_cb(ec_bdev->configure_cb_ctx, 0);
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
	ec_bdev->min_base_bdevs_operational = min_operational;
	ec_bdev->alignment_warned = false;

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
	if (uuid != NULL && !spdk_uuid_is_null(uuid)) {
		/* User specified UUID - use it */
		spdk_uuid_copy(&ec_bdev_gen->uuid, uuid);
	} else {
		/* No UUID specified - generate random UUID */
		spdk_uuid_generate(&ec_bdev_gen->uuid);
	}

	/* Set block size and block count */
	ec_bdev_gen->blocklen = 512; /* Default block size */
	ec_bdev_gen->blockcnt = 0; /* Will be updated during configure */
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
ec_bdev_delete(struct ec_bdev *ec_bdev, bool wipe_sb, ec_bdev_destruct_cb cb_fn, void *cb_ctx)
{
	struct ec_rebuild_context *rebuild_ctx;

	if (ec_bdev == NULL) {
		if (cb_fn) {
			cb_fn(cb_ctx, 0);
		}
		return;
	}

	rebuild_ctx = ec_bdev_get_active_rebuild(ec_bdev);
	if (rebuild_ctx != NULL) {
		SPDK_ERRLOG("Cannot delete EC bdev %s while rebuild is in progress\n",
			    ec_bdev->bdev.name);
		if (cb_fn) {
			cb_fn(cb_ctx, -EBUSY);
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

	/* Only wipe superblock if explicitly requested (e.g., from RPC delete command) */
	if (wipe_sb && ec_bdev->sb != NULL) {
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
 * cb_fn - callback function
 * cb_ctx - callback context
 * returns:
 * 0 on success, non-zero on failure
 */
int
ec_bdev_add_base_bdev(struct ec_bdev *ec_bdev, const char *name,
		     ec_base_bdev_cb cb_fn, void *cb_ctx)
{
	struct ec_base_bdev_info *base_info;
	struct ec_base_bdev_info *iter;
	uint8_t slot = UINT8_MAX;
	int rc;
	struct spdk_bdev *bdev;

	/* Check if rebuild is in progress (legacy or process framework) */
	if (ec_bdev_is_rebuilding(ec_bdev)) {
		struct ec_bdev_process *process = ec_bdev->process;
		if (process != NULL && process->state < EC_PROCESS_STATE_STOPPING && 
		    process->type == EC_PROCESS_REBUILD) {
			/* Process framework rebuild - show progress */
			uint64_t total_blocks = ec_bdev->bdev.blockcnt;
			uint64_t completed_blocks = process->window_offset;
			uint32_t percent = total_blocks > 0 ? 
				(uint32_t)((completed_blocks * 100) / total_blocks) : 0;
			SPDK_ERRLOG("\n");
			SPDK_ERRLOG("===========================================================\n");
			SPDK_ERRLOG("Cannot add base bdev '%s' to EC bdev %s: Rebuild is in progress!\n",
				    name, ec_bdev->bdev.name);
			SPDK_ERRLOG("Target disk: %s (slot %u)\n",
				    process->target->name ? process->target->name : "unknown",
				    ec_bdev_base_bdev_slot(process->target));
			SPDK_ERRLOG("Rebuild progress: %u%% (%lu / %lu blocks)\n",
				    percent, completed_blocks, total_blocks);
			SPDK_ERRLOG("Please wait for rebuild to complete before adding base bdev.\n");
			SPDK_ERRLOG("===========================================================\n");
			SPDK_ERRLOG("\n");
		} else {
			/* Legacy rebuild */
			SPDK_ERRLOG("Cannot add base bdev '%s' to EC bdev %s while rebuild is running\n",
				    name, ec_bdev->bdev.name);
		}
		return -EBUSY;
	}

	/* Check if base bdev exists immediately (like RAID does) to avoid waiting */
	bdev = spdk_bdev_get_by_name(name);
	if (bdev == NULL) {
		/* Base bdev doesn't exist - return immediately instead of waiting */
		return -ENODEV;
	}

	/* Find an available slot */
	base_info = NULL;
	
	/* For ONLINE state: try to match UUID to original slot first */
	if (ec_bdev->state == EC_BDEV_STATE_ONLINE) {
		if (bdev != NULL && ec_bdev->superblock_enabled && ec_bdev->sb != NULL) {
			/* Try to find the original slot from superblock by UUID */
			const struct ec_bdev_sb_base_bdev *sb_base = 
				ec_bdev_sb_find_base_bdev_by_uuid(ec_bdev->sb, spdk_bdev_get_uuid(bdev));
			
			if (sb_base != NULL && sb_base->slot < ec_bdev->num_base_bdevs) {
				/* Found matching UUID in superblock - check if the slot is available */
				iter = &ec_bdev->base_bdev_info[sb_base->slot];
				if (iter->name == NULL) {
					/* Slot is available - use it to restore original position */
					base_info = iter;
					slot = sb_base->slot;
					/* Copy UUID from superblock to slot if not already set */
					if (spdk_uuid_is_null(&base_info->uuid)) {
						spdk_uuid_copy(&base_info->uuid, &sb_base->uuid);
					}
				}
			}
		}
	}

	/* If UUID matching failed or slot is occupied, fall back to first empty slot */
	if (base_info == NULL) {
		EC_FOR_EACH_BASE_BDEV(ec_bdev, iter) {
			if (iter->name == NULL && spdk_uuid_is_null(&iter->uuid)) {
				base_info = iter;
				slot = (uint8_t)(iter - ec_bdev->base_bdev_info);
				break;
			}
		}
	}

	/* Check if we found an available slot */
	if (base_info == NULL || slot == UINT8_MAX) {
		SPDK_ERRLOG("No available slots for EC bdev %s\n", ec_bdev->bdev.name);
		return -ENOSPC;
	}
	base_info->name = strdup(name);
	if (base_info->name == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for base bdev name\n");
		/* No cleanup needed - base_info->name is NULL, slot will be reused */
		return -ENOMEM;
	}

	base_info->configure_cb = cb_fn;
	base_info->configure_cb_ctx = cb_ctx;

	/* Configure the base bdev */
	rc = ec_bdev_configure_base_bdev(base_info, false, cb_fn, cb_ctx);
	if (rc != 0) {
		/* If configuration failed, clean up the name we allocated */
		free(base_info->name);
		base_info->name = NULL;
		base_info->configure_cb = NULL;
		base_info->configure_cb_ctx = NULL;
	}
	return rc;
}

/*
 * Callback after wiping superblock - continue with removal
 */
static void
ec_bdev_remove_base_bdev_done(struct ec_base_bdev_info *base_info, int status)
{
	struct ec_bdev *ec_bdev;

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in ec_bdev_remove_base_bdev_done\n");
		return;
	}

	ec_bdev = base_info->ec_bdev;
	if (ec_bdev == NULL) {
		SPDK_ERRLOG("base_info->ec_bdev is NULL in ec_bdev_remove_base_bdev_done\n");
		if (base_info->remove_scheduled) {
			base_info->remove_scheduled = false;
		}
		if (base_info->remove_cb != NULL) {
			base_info->remove_cb(base_info->remove_cb_ctx, -EINVAL);
		}
		return;
	}

	/* Verify remove_scheduled before clearing it (aligned with RAID framework)
	 * Use defensive check instead of assert to avoid crashes in edge cases
	 */
	if (!base_info->remove_scheduled) {
		SPDK_ERRLOG("remove_scheduled is false in ec_bdev_remove_base_bdev_done - this should not happen\n");
		/* Continue anyway to avoid resource leak - callback may still need to be called */
	}
	base_info->remove_scheduled = false;

	if (status == 0) {
		/* Check if there are enough operational base bdevs to keep EC bdev operational */
		if (ec_bdev->num_base_bdevs_operational < ec_bdev->min_base_bdevs_operational) {
			/* There is not enough base bdevs to keep the EC bdev operational. */
			ec_bdev_deconfigure(ec_bdev, base_info->remove_cb, base_info->remove_cb_ctx);
			return;
		}
	}

	/* Call callback if set */
	if (base_info->remove_cb != NULL) {
		ec_base_bdev_cb cb_fn = base_info->remove_cb;
		void *cb_ctx = base_info->remove_cb_ctx;
		base_info->remove_cb = NULL;
		base_info->remove_cb_ctx = NULL;
		cb_fn(cb_ctx, status);
	}
}

struct ec_bdev_deconfigure_wrapper_ctx {
	struct ec_base_bdev_info *base_info;
	ec_base_bdev_cb user_cb;
	void *user_ctx;
};

/*
 * Wrapper callback for ec_bdev_deconfigure to clear remove_scheduled flag
 */
static void
ec_bdev_deconfigure_wrapper_cb(void *ctx, int status)
{
	struct ec_bdev_deconfigure_wrapper_ctx *wrapper_ctx = ctx;
	
	if (wrapper_ctx->base_info != NULL) {
		wrapper_ctx->base_info->remove_scheduled = false;
	}
	if (wrapper_ctx->user_cb != NULL) {
		wrapper_ctx->user_cb(wrapper_ctx->user_ctx, status);
	}
	free(wrapper_ctx);
}

static void
ec_bdev_remove_base_bdev_after_wipe(void *ctx, int status)
{
	struct ec_base_bdev_info *base_info = ctx;

	if (status != 0) {
		/* Wipe failed, but continue with removal anyway */
		SPDK_WARNLOG("Failed to wipe superblock on base bdev '%s', continuing with removal\n",
			     base_info->name ? base_info->name : "unknown");
	}

	/* Now free the base bdev resources */
	ec_bdev_free_base_bdev_resource(base_info);

	/* Call done callback (which will assert remove_scheduled and call user callback) */
	ec_bdev_remove_base_bdev_done(base_info, 0);
}

/*
 * brief:
 * ec_bdev_cleanup is used to cleanup ec_bdev related data
 * structures.
 * params:
 * ec_bdev - pointer to ec_bdev
 * returns:
 * none
 */
static void
ec_bdev_cleanup(struct ec_bdev *ec_bdev)
{
	struct ec_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_ec, "ec_bdev_cleanup, %p name %s, state %s\n",
		      ec_bdev, ec_bdev->bdev.name, ec_bdev_state_to_str(ec_bdev->state));
	assert(ec_bdev->state != EC_BDEV_STATE_ONLINE);
	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		assert(base_info->desc == NULL);
		free(base_info->name);
	}

	TAILQ_REMOVE(&g_ec_bdev_list, ec_bdev, global_link);
}

static void
ec_bdev_cleanup_and_free(struct ec_bdev *ec_bdev)
{
	ec_bdev_cleanup(ec_bdev);
	ec_bdev_free(ec_bdev);
}

/*
 * brief:
 * ec_bdev_remove_base_bdev removes a base bdev from EC bdev
 * This function will attempt to wipe the superblock on the base bdev before removal.
 * If wipe fails (e.g., disk is already failed), removal will continue anyway.
 * params:
 * base_bdev - pointer to base bdev
 * cb_fn - callback function
 * cb_ctx - callback context
 * returns:
 * 0 on success, non-zero on failure
 */
static int
_ec_bdev_remove_base_bdev(struct ec_base_bdev_info *base_info,
			  ec_base_bdev_cb cb_fn, void *cb_ctx)
{
	struct ec_bdev *ec_bdev;
	struct ec_bdev_process *process;
	int ret = 0;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in _ec_bdev_remove_base_bdev\n");
		return -EINVAL;
	}

	ec_bdev = base_info->ec_bdev;
	if (ec_bdev == NULL) {
		SPDK_ERRLOG("base_info->ec_bdev is NULL in _ec_bdev_remove_base_bdev\n");
		return -EINVAL;
	}
	process = ec_bdev->process;

	SPDK_DEBUGLOG(bdev_ec, "%s\n", base_info->name ? base_info->name : "unknown");

	if (base_info->remove_scheduled || !base_info->is_configured) {
		return -ENODEV;
	}

	if (base_info->desc == NULL) {
		SPDK_ERRLOG("base_info->desc is NULL, cannot remove base bdev\n");
		return -ENODEV;
	}

	/* Check if this base bdev is the rebuild target */
	if (ec_bdev_is_rebuild_target(ec_bdev, base_info)) {
		struct ec_bdev_process *process = ec_bdev->process;
		if (process != NULL && process->state < EC_PROCESS_STATE_STOPPING && 
		    process->type == EC_PROCESS_REBUILD) {
			/* Process framework rebuild - show progress */
			uint64_t total_blocks = ec_bdev->bdev.blockcnt;
			uint64_t completed_blocks = process->window_offset;
			uint32_t percent = total_blocks > 0 ? 
				(uint32_t)((completed_blocks * 100) / total_blocks) : 0;
			SPDK_ERRLOG("\n");
			SPDK_ERRLOG("===========================================================\n");
			SPDK_ERRLOG("Cannot remove base bdev '%s': Rebuild is in progress!\n",
				    base_info->name ? base_info->name : "unknown");
			SPDK_ERRLOG("EC bdev: %s\n", ec_bdev->bdev.name);
			SPDK_ERRLOG("Target disk: %s (slot %u)\n",
				    base_info->name ? base_info->name : "unknown",
				    ec_bdev_base_bdev_slot(base_info));
			SPDK_ERRLOG("Rebuild progress: %u%% (%lu / %lu blocks)\n",
				    percent, completed_blocks, total_blocks);
			SPDK_ERRLOG("Please wait for rebuild to complete before removing base bdev.\n");
			SPDK_ERRLOG("===========================================================\n");
			SPDK_ERRLOG("\n");
		} else {
			SPDK_WARNLOG("Cannot remove base bdev '%s' while rebuild is running\n",
				     base_info->name ? base_info->name : "unknown");
		}
		return -EBUSY;
	}

	base_info->remove_scheduled = true;
	base_info->remove_cb = cb_fn;
	base_info->remove_cb_ctx = cb_ctx;

	/* Try to wipe superblock first (if disk is failed, this might fail, which is OK) */
	if (base_info->desc != NULL && base_info->app_thread_ch != NULL) {
		int rc = ec_bdev_wipe_single_base_bdev_superblock(base_info,
								   ec_bdev_remove_base_bdev_after_wipe,
								   base_info);
		if (rc == 0) {
			/* Wipe started successfully - callback will be called when wipe completes */
			return 0;
		}
		/* Wipe failed to start - check if callback was called
		 * If callback was called, it will handle removal. Otherwise, we need to handle it here.
		 * For synchronous errors (like -EINVAL), callback is NOT called, so we handle removal here.
		 */
		if (rc == -EINVAL || rc == -ENODEV) {
			/* Synchronous error - callback was called immediately, removal handled by callback */
			SPDK_WARNLOG("Failed to start superblock wipe for base bdev '%s': %s (removal handled by wipe callback)\n",
				     base_info->name ? base_info->name : "unknown", spdk_strerror(-rc));
			return rc;
		}
		/* Other errors - callback may have been called (e.g., -ENOMEM with queue wait)
		 * Don't clear remove_scheduled/remove_cb/remove_cb_ctx - callback will handle it
		 */
		SPDK_WARNLOG("Failed to start superblock wipe for base bdev '%s': %s (callback may be called later)\n",
			     base_info->name ? base_info->name : "unknown", spdk_strerror(-rc));
		return rc;
	}
	SPDK_DEBUGLOG(bdev_ec, "Cannot wipe superblock - base bdev desc or channel is NULL\n");

	if (ec_bdev->state != EC_BDEV_STATE_ONLINE) {
		ec_bdev_free_base_bdev_resource(base_info);
		base_info->remove_scheduled = false;
		if (ec_bdev->num_base_bdevs_discovered == 0 &&
		    ec_bdev->state == EC_BDEV_STATE_OFFLINE) {
			ec_bdev_cleanup_and_free(ec_bdev);
		}
		if (cb_fn != NULL) {
			cb_fn(cb_ctx, 0);
		}
	} else if (ec_bdev->min_base_bdevs_operational == ec_bdev->num_base_bdevs) {
		ec_bdev->num_base_bdevs_operational--;
		/* When deconfiguring the entire EC bdev, we need to clear remove_scheduled
		 * since we're bypassing the normal quiesce/unquiesce flow that would call
		 * ec_bdev_remove_base_bdev_done. Create a wrapper callback to clear the flag.
		 */
		struct ec_bdev_deconfigure_wrapper_ctx *wrapper_ctx = calloc(1, sizeof(*wrapper_ctx));
		if (wrapper_ctx == NULL) {
			SPDK_ERRLOG("Failed to allocate wrapper context for deconfigure\n");
			base_info->remove_scheduled = false;
			if (cb_fn != NULL) {
				cb_fn(cb_ctx, -ENOMEM);
			}
			return -ENOMEM;
		}
		wrapper_ctx->base_info = base_info;
		wrapper_ctx->user_cb = cb_fn;
		wrapper_ctx->user_ctx = cb_ctx;
		
		ec_bdev_deconfigure(ec_bdev, ec_bdev_deconfigure_wrapper_cb, wrapper_ctx);
		return 0;
	} else {
		if (process != NULL) {
			ret = ec_bdev_process_base_bdev_remove(process, base_info);
		} else {
			ret = ec_bdev_remove_base_bdev_quiesce(base_info);
		}
		if (ret != 0) {
			base_info->remove_scheduled = false;
		}
	}

	return ret;
}

static void
ec_bdev_remove_base_bdev_on_unquiesced(void *ctx, int status)
{
	struct ec_base_bdev_info *base_info = ctx;
	struct ec_bdev *ec_bdev;

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in ec_bdev_remove_base_bdev_on_unquiesced\n");
		return;
	}

	ec_bdev = base_info->ec_bdev;
	if (ec_bdev == NULL) {
		SPDK_ERRLOG("base_info->ec_bdev is NULL in ec_bdev_remove_base_bdev_on_unquiesced\n");
		ec_bdev_remove_base_bdev_done(base_info, -EINVAL);
		return;
	}

	if (status != 0) {
		SPDK_ERRLOG("Failed to unquiesce EC bdev %s: %s\n",
			    ec_bdev->bdev.name, spdk_strerror(-status));
	}

	ec_bdev_remove_base_bdev_done(base_info, status);
}

static void
ec_bdev_remove_base_bdev_on_quiesced(void *ctx, int status)
{
	struct ec_base_bdev_info *base_info = ctx;
	struct ec_bdev *ec_bdev;

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in ec_bdev_remove_base_bdev_on_quiesced\n");
		return;
	}

	ec_bdev = base_info->ec_bdev;
	if (ec_bdev == NULL) {
		SPDK_ERRLOG("base_info->ec_bdev is NULL in ec_bdev_remove_base_bdev_on_quiesced\n");
		return;
	}

	if (status != 0) {
		SPDK_ERRLOG("Failed to quiesce EC bdev %s: %s\n",
			    ec_bdev->bdev.name, spdk_strerror(-status));
		ec_bdev_remove_base_bdev_done(base_info, status);
		return;
	}

	if (ec_bdev->sb) {
		struct ec_bdev_superblock *sb = ec_bdev->sb;
		uint8_t slot = ec_bdev_base_bdev_slot(base_info);
		uint8_t i;

		/* Safety check: ensure base_bdev_info array is valid before calculating slot */
		if (ec_bdev->base_bdev_info == NULL) {
			SPDK_ERRLOG("ec_bdev->base_bdev_info is NULL for EC bdev '%s'\n", ec_bdev->bdev.name);
			ec_bdev_remove_base_bdev_done(base_info, -EINVAL);
			return;
		}

		for (i = 0; i < sb->base_bdevs_size; i++) {
			struct ec_bdev_sb_base_bdev *sb_base_bdev = &sb->base_bdevs[i];

			if (sb_base_bdev->state == EC_SB_BASE_BDEV_CONFIGURED &&
			    sb_base_bdev->slot == slot) {
				if (base_info->is_failed) {
					sb_base_bdev->state = EC_SB_BASE_BDEV_FAILED;
				} else {
					sb_base_bdev->state = EC_SB_BASE_BDEV_MISSING;
				}

				ec_bdev_write_superblock(ec_bdev, ec_bdev_remove_base_bdev_write_sb_cb, base_info);
				return;
			}
		}
	}

	ec_bdev_remove_base_bdev_cont(base_info);
}

static void
ec_bdev_remove_base_bdev_cont(struct ec_base_bdev_info *base_info)
{
	int rc;

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in ec_bdev_remove_base_bdev_cont\n");
		return;
	}

	if (base_info->desc == NULL || base_info->app_thread_ch == NULL) {
		SPDK_WARNLOG("base_info->desc or app_thread_ch is NULL, proceeding with removal\n");
		ec_bdev_remove_base_bdev_do_remove(base_info);
		return;
	}

	/* Reset the base bdev to ensure all I/O is complete */
	rc = spdk_bdev_reset(base_info->desc, base_info->app_thread_ch,
			     ec_bdev_remove_base_bdev_reset_done, base_info);
	if (rc != 0) {
		if (rc == -ENOMEM) {
			SPDK_ERRLOG("No memory to reset base bdev '%s'\n",
				    base_info->name ? base_info->name : "unknown");
		} else {
			SPDK_ERRLOG("Failed to reset base bdev '%s': %s\n",
				    base_info->name ? base_info->name : "unknown", spdk_strerror(-rc));
		}
		ec_bdev_remove_base_bdev_do_remove(base_info);
	}
}

static void
ec_bdev_remove_base_bdev_reset_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ec_base_bdev_info *base_info = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in ec_bdev_remove_base_bdev_reset_done\n");
		return;
	}

	ec_bdev_remove_base_bdev_do_remove(base_info);
}

static void
ec_bdev_remove_base_bdev_do_remove(struct ec_base_bdev_info *base_info)
{
	struct ec_bdev *ec_bdev;

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in ec_bdev_remove_base_bdev_do_remove\n");
		return;
	}

	ec_bdev = base_info->ec_bdev;
	if (ec_bdev == NULL) {
		SPDK_ERRLOG("base_info->ec_bdev is NULL in ec_bdev_remove_base_bdev_do_remove\n");
		return;
	}

	ec_bdev_deconfigure_base_bdev(base_info);

	spdk_for_each_channel(ec_bdev, ec_bdev_channel_remove_base_bdev, base_info,
			      ec_bdev_channels_remove_base_bdev_done);
}

static void
ec_bdev_channel_remove_base_bdev(struct spdk_io_channel_iter *i)
{
	struct ec_base_bdev_info *base_info = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct ec_bdev_io_channel *ec_ch = spdk_io_channel_get_ctx(ch);
	struct ec_bdev *ec_bdev;
	uint8_t idx;

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in ec_bdev_channel_remove_base_bdev\n");
		spdk_for_each_channel_continue(i, -EINVAL);
		return;
	}

	ec_bdev = base_info->ec_bdev;
	if (ec_bdev == NULL || ec_bdev->base_bdev_info == NULL) {
		SPDK_ERRLOG("ec_bdev or base_bdev_info is NULL in ec_bdev_channel_remove_base_bdev\n");
		spdk_for_each_channel_continue(i, -EINVAL);
		return;
	}

	idx = ec_bdev_base_bdev_slot(base_info);

	/* Safety check: ensure idx is within valid range */
	if (idx >= ec_bdev->num_base_bdevs) {
		SPDK_ERRLOG("Calculated slot %u exceeds num_base_bdevs %u for EC bdev '%s'\n",
			    idx, ec_bdev->num_base_bdevs, ec_bdev->bdev.name);
		spdk_for_each_channel_continue(i, -EINVAL);
		return;
	}

	SPDK_DEBUGLOG(bdev_ec, "slot: %u ec_ch: %p\n", idx, ec_ch);

	/* Safety check: ensure base_channel array is allocated */
	if (ec_ch->base_channel != NULL) {
		if (ec_ch->base_channel[idx] != NULL) {
			spdk_put_io_channel(ec_ch->base_channel[idx]);
			ec_ch->base_channel[idx] = NULL;
		}
	}

	if (ec_ch->process.ch_processed != NULL && ec_ch->process.ch_processed->base_channel != NULL) {
		if (idx < ec_bdev->num_base_bdevs) {
			ec_ch->process.ch_processed->base_channel[idx] = NULL;
		}
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
ec_bdev_channels_remove_base_bdev_done(struct spdk_io_channel_iter *i, int status)
{
	struct ec_base_bdev_info *base_info = spdk_io_channel_iter_get_ctx(i);
	struct ec_bdev *ec_bdev;

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in ec_bdev_channels_remove_base_bdev_done\n");
		return;
	}

	ec_bdev = base_info->ec_bdev;
	if (ec_bdev == NULL) {
		SPDK_ERRLOG("base_info->ec_bdev is NULL in ec_bdev_channels_remove_base_bdev_done\n");
		/* Clear remove_scheduled before freeing resources since we won't call remove_base_bdev_done */
		base_info->remove_scheduled = false;
		ec_bdev_free_base_bdev_resource(base_info);
		return;
	}

	/* Free base bdev resources, but keep remove_scheduled flag set - it will be cleared
	 * in ec_bdev_remove_base_bdev_done after unquiesce completes. This ensures proper
	 * state tracking throughout the removal process.
	 */
	ec_bdev_free_base_bdev_resource(base_info);

	spdk_bdev_unquiesce(&ec_bdev->bdev, &g_ec_if, ec_bdev_remove_base_bdev_on_unquiesced,
			    base_info);
}

static void
ec_bdev_remove_base_bdev_write_sb_cb(int status, struct ec_bdev *ec_bdev, void *ctx)
{
	struct ec_base_bdev_info *base_info = ctx;

	if (status != 0) {
		SPDK_ERRLOG("Failed to write EC bdev '%s' superblock: %s\n",
			    ec_bdev->bdev.name, spdk_strerror(-status));
	}

	ec_bdev_remove_base_bdev_cont(base_info);
}

static int
ec_bdev_remove_base_bdev_quiesce(struct ec_base_bdev_info *base_info)
{
	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	return spdk_bdev_quiesce(&base_info->ec_bdev->bdev, &g_ec_if,
				 ec_bdev_remove_base_bdev_on_quiesced, base_info);
}

struct ec_bdev_process_base_bdev_remove_ctx {
	struct ec_bdev_process *process;
	struct ec_base_bdev_info *base_info;
	uint8_t num_base_bdevs_operational;
};

static void
_ec_bdev_process_base_bdev_remove_cont(void *ctx)
{
	struct ec_base_bdev_info *base_info = ctx;
	int ret;

	ret = ec_bdev_remove_base_bdev_quiesce(base_info);
	if (ret != 0) {
		ec_bdev_remove_base_bdev_done(base_info, ret);
	}
}

static void
ec_bdev_process_base_bdev_remove_cont(void *_ctx)
{
	struct ec_bdev_process_base_bdev_remove_ctx *ctx = _ctx;
	struct ec_base_bdev_info *base_info = ctx->base_info;

	free(ctx);

	spdk_thread_send_msg(spdk_thread_get_app_thread(), _ec_bdev_process_base_bdev_remove_cont,
			     base_info);
}

static void
_ec_bdev_process_base_bdev_remove(void *_ctx)
{
	struct ec_bdev_process_base_bdev_remove_ctx *ctx = _ctx;
	struct ec_bdev_process *process = ctx->process;
	int ret;

	if (ctx->base_info != process->target &&
	    ctx->num_base_bdevs_operational > process->ec_bdev->min_base_bdevs_operational) {
		/* process doesn't need to be stopped */
		ec_bdev_process_base_bdev_remove_cont(ctx);
		return;
	}

	assert(process->state > EC_PROCESS_STATE_INIT &&
	       process->state < EC_PROCESS_STATE_STOPPED);

	ret = ec_bdev_process_add_finish_action(process, ec_bdev_process_base_bdev_remove_cont, ctx);
	if (ret != 0) {
		ec_bdev_remove_base_bdev_done(ctx->base_info, ret);
		free(ctx);
		return;
	}

	process->state = EC_PROCESS_STATE_STOPPING;

	if (process->status == 0) {
		process->status = -ENODEV;
	}
}

static int
ec_bdev_process_add_finish_action(struct ec_bdev_process *process, spdk_msg_fn cb, void *cb_ctx)
{
	struct ec_process_finish_action *finish_action;

	assert(spdk_get_thread() == process->thread);
	assert(process->state < EC_PROCESS_STATE_STOPPED);

	finish_action = calloc(1, sizeof(*finish_action));
	if (finish_action == NULL) {
		return -ENOMEM;
	}

	finish_action->cb = cb;
	finish_action->cb_ctx = cb_ctx;

	TAILQ_INSERT_TAIL(&process->finish_actions, finish_action, link);

	return 0;
}

static int
ec_bdev_process_base_bdev_remove(struct ec_bdev_process *process,
				 struct ec_base_bdev_info *base_info)
{
	struct ec_bdev_process_base_bdev_remove_ctx *ctx;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	/*
	 * We have to send the process and num_base_bdevs_operational in the message ctx
	 * because the process thread should not access ec_bdev's properties. Particularly,
	 * ec_bdev->process may be cleared by the time the message is handled, but ctx->process
	 * will still be valid until the process is fully stopped.
	 */
	ctx->base_info = base_info;
	ctx->process = process;
	/*
	 * ec_bdev->num_base_bdevs_operational can't be used here because it is decremented
	 * after the removal and more than one base bdev may be removed at the same time
	 */
	EC_FOR_EACH_BASE_BDEV(process->ec_bdev, base_info) {
		if (base_info->is_configured && !base_info->remove_scheduled) {
			ctx->num_base_bdevs_operational++;
		}
	}

	spdk_thread_send_msg(process->thread, _ec_bdev_process_base_bdev_remove, ctx);

	return 0;
}

static void
ec_bdev_deconfigure_base_bdev(struct ec_base_bdev_info *base_info)
{
	struct ec_bdev *ec_bdev = base_info->ec_bdev;

	assert(base_info->is_configured);
	assert(ec_bdev->num_base_bdevs_discovered);
	ec_bdev->num_base_bdevs_discovered--;
	base_info->is_configured = false;
}

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
 * Callback for when superblock write completes after marking base bdev as failed
 * Note: We don't remove the base bdev here - it will be removed manually by the user
 * via RPC call when they are ready to replace the disk.
 */
static void
ec_bdev_fail_base_bdev_write_sb_cb(int status, struct ec_bdev *ec_bdev, void *ctx)
{
	struct ec_base_bdev_info *base_info = ctx;

	if (status != 0) {
		SPDK_ERRLOG("Failed to write EC bdev '%s' superblock after marking base bdev as failed: %s\n",
			    ec_bdev->bdev.name, spdk_strerror(-status));
		/* Superblock write failed, but base bdev is already marked as failed */
	}

	/* Don't remove the base bdev automatically - wait for user to manually remove it
	 * via RPC call. The base bdev is marked as failed and will be excluded from I/O operations.
	 * User should:
	 * 1. Physically replace the failed disk
	 * 2. Use RPC 'bdev_ec_remove_base_bdev' to remove the failed disk (this will try to wipe superblock)
	 * 3. Use RPC 'bdev_ec_add_base_bdev' to add the new disk
	 * 4. Rebuild will automatically start when the new disk is added
	 */
	SPDK_NOTICELOG("Base bdev '%s' marked as FAILED in superblock. Use RPC to remove it when ready.\n",
		       base_info->name ? base_info->name : "unknown");
}

/*
 * Set LED state for a base bdev (if it's an NVMe device behind VMD)
 * This function attempts to set the LED to REBUILD state to indicate
 * that the disk is healthy and should be used for rebuild.
 */
static void
ec_bdev_set_base_bdev_led(struct ec_base_bdev_info *base_info, enum spdk_vmd_led_state led_state)
{
	struct spdk_bdev *bdev;
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_pci_device *pci_device;
	const char *module_name;
	int rc;

	if (base_info == NULL || base_info->desc == NULL) {
		return;
	}

	bdev = spdk_bdev_desc_get_bdev(base_info->desc);
	if (bdev == NULL) {
		return;
	}

	/* Check if this is an NVMe bdev by module name */
	module_name = spdk_bdev_get_module_name(bdev);
	if (module_name == NULL || strcmp(module_name, "nvme") != 0) {
		/* Not an NVMe bdev, skip LED control */
		return;
	}

	/* Check if bdev_nvme_get_ctrlr function is available (weak symbol) */
	if (bdev_nvme_get_ctrlr == NULL) {
		/* NVMe bdev module not linked, skip LED control */
		SPDK_DEBUGLOG(bdev_ec, "bdev_nvme_get_ctrlr not available, skipping LED control for '%s'\n",
			      base_info->name ? base_info->name : "unknown");
		return;
	}

	/* Get NVMe controller from bdev */
	ctrlr = bdev_nvme_get_ctrlr(bdev);
	if (ctrlr == NULL) {
		/* Not an NVMe bdev or controller not available, skip LED control */
		return;
	}

	/* Check if spdk_nvme_ctrlr_get_pci_device function is available */
	if (spdk_nvme_ctrlr_get_pci_device == NULL) {
		/* Function not available, skip LED control */
		SPDK_DEBUGLOG(bdev_ec, "spdk_nvme_ctrlr_get_pci_device not available, skipping LED control for '%s'\n",
			      base_info->name ? base_info->name : "unknown");
		return;
	}

	/* Get PCI device from NVMe controller */
	pci_device = spdk_nvme_ctrlr_get_pci_device(ctrlr);
	if (pci_device == NULL) {
		/* Not a PCIe-attached NVMe device, skip LED control */
		return;
	}

	/* Set LED state (only works for devices behind VMD) */
	rc = spdk_vmd_set_led_state(pci_device, led_state);
	if (rc != 0) {
		/* LED control failed - device might not be behind VMD, which is OK */
		SPDK_DEBUGLOG(bdev_ec, "Failed to set LED state for base bdev '%s': %s (device may not be behind VMD)\n",
			      base_info->name ? base_info->name : "unknown", spdk_strerror(-rc));
	} else {
		SPDK_NOTICELOG("Set LED state to %d for base bdev '%s'\n",
			       led_state, base_info->name ? base_info->name : "unknown");
	}
}

/*
 * Get PCIe BDF address string for a base bdev
 * Returns 0 on success, -1 on failure (not a PCIe device or function not available)
 */
static int
ec_bdev_get_pcie_bdf(struct ec_base_bdev_info *base_info, char *bdf_str, size_t bdf_str_size)
{
	struct spdk_bdev *bdev;
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_pci_device *pci_device;
	struct spdk_pci_addr pci_addr;
	const char *module_name;
	int rc;

	if (base_info == NULL || base_info->desc == NULL || bdf_str == NULL || bdf_str_size == 0) {
		return -1;
	}

	bdev = spdk_bdev_desc_get_bdev(base_info->desc);
	if (bdev == NULL) {
		return -1;
	}

	/* Check if this is an NVMe bdev by module name */
	module_name = spdk_bdev_get_module_name(bdev);
	if (module_name == NULL || strcmp(module_name, "nvme") != 0) {
		/* Not an NVMe bdev */
		return -1;
	}

	/* Check if bdev_nvme_get_ctrlr function is available (weak symbol) */
	if (bdev_nvme_get_ctrlr == NULL) {
		/* NVMe bdev module not linked */
		return -1;
	}

	/* Get NVMe controller from bdev */
	ctrlr = bdev_nvme_get_ctrlr(bdev);
	if (ctrlr == NULL) {
		/* Not an NVMe bdev or controller not available */
		return -1;
	}

	/* Check if spdk_nvme_ctrlr_get_pci_device function is available */
	if (spdk_nvme_ctrlr_get_pci_device == NULL) {
		/* Function not available */
		return -1;
	}

	/* Get PCI device from NVMe controller */
	pci_device = spdk_nvme_ctrlr_get_pci_device(ctrlr);
	if (pci_device == NULL) {
		/* Not a PCIe-attached NVMe device */
		return -1;
	}

	/* Get PCI address */
	pci_addr = spdk_pci_device_get_addr(pci_device);
	
	/* Format PCI address as BDF string */
	rc = spdk_pci_addr_fmt(bdf_str, bdf_str_size, &pci_addr);
	if (rc != 0) {
		return -1;
	}

	return 0;
}

/*
 * Set LED state for all healthy base bdevs in an EC bdev
 * This is called when a disk fails to make healthy disks blink
 * Also called when rebuild completes to turn off LEDs
 */
void
ec_bdev_set_healthy_disks_led(struct ec_bdev *ec_bdev, enum spdk_vmd_led_state led_state)
{
	struct ec_base_bdev_info *base_info;

	if (ec_bdev == NULL) {
		return;
	}

	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		/* Only set LED for healthy (non-failed) base bdevs */
		if (base_info->is_configured && !base_info->is_failed &&
		    base_info->desc != NULL) {
			ec_bdev_set_base_bdev_led(base_info, led_state);
		}
	}
}

/*
 * Internal function to fail a base bdev (called from app thread)
 */
static void
_ec_bdev_fail_base_bdev(void *ctx)
{
	struct ec_base_bdev_info *base_info = ctx;
	struct ec_bdev *ec_bdev;
	uint8_t slot;
	char pcie_bdf[32] = "N/A";

	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in fail_base_bdev\n");
		return;
	}

	ec_bdev = base_info->ec_bdev;
	if (ec_bdev == NULL) {
		SPDK_ERRLOG("ec_bdev is NULL in fail_base_bdev\n");
		return;
	}

	if (base_info->is_failed) {
		/* Already marked as failed, skip */
		return;
	}

	base_info->is_failed = true;
	slot = (uint8_t)(base_info - ec_bdev->base_bdev_info);

	/* Try to get PCIe BDF address for the failed disk */
	if (ec_bdev_get_pcie_bdf(base_info, pcie_bdf, sizeof(pcie_bdf)) == 0) {
		SPDK_ERRLOG("===========================================================\n");
		SPDK_ERRLOG("CRITICAL: Base bdev FAILED in slot %u ('%s') of EC bdev '%s'\n",
			       slot, base_info->name ? base_info->name : "unknown", ec_bdev->bdev.name);
		SPDK_ERRLOG("FAILED DISK PCIe LOCATION: %s\n", pcie_bdf);
		SPDK_ERRLOG("ACTION REQUIRED: Please replace the failed disk at PCIe %s\n", pcie_bdf);
		SPDK_ERRLOG("After replacing, use RPC 'bdev_ec_add_base_bdev' to add the new disk\n");
		SPDK_ERRLOG("The EC bdev is now in degraded mode - data is still accessible\n");
		SPDK_ERRLOG("but redundancy is reduced. Replace the disk as soon as possible.\n");
		SPDK_ERRLOG("Healthy disks will have their LEDs blinking to indicate they are operational.\n");
		SPDK_ERRLOG("===========================================================\n");
	} else {
		/* PCIe BDF not available (not a PCIe device or NVMe module not linked) */
		SPDK_ERRLOG("===========================================================\n");
		SPDK_ERRLOG("CRITICAL: Base bdev FAILED in slot %u ('%s') of EC bdev '%s'\n",
			       slot, base_info->name ? base_info->name : "unknown", ec_bdev->bdev.name);
		SPDK_ERRLOG("ACTION REQUIRED: Please replace the failed disk\n");
		SPDK_ERRLOG("After replacing, use RPC 'bdev_ec_add_base_bdev' to add the new disk\n");
		SPDK_ERRLOG("The EC bdev is now in degraded mode - data is still accessible\n");
		SPDK_ERRLOG("but redundancy is reduced. Replace the disk as soon as possible.\n");
		SPDK_ERRLOG("Healthy disks will have their LEDs blinking to indicate they are operational.\n");
		SPDK_ERRLOG("===========================================================\n");
	}

	/* Set LED state for healthy disks to REBUILD (blinking) to indicate they are operational */
	ec_bdev_set_healthy_disks_led(ec_bdev, SPDK_VMD_LED_STATE_REBUILD);

	/* Update superblock if it exists */
	/* Find the slot in superblock and update its state to FAILED if it's CONFIGURED or REBUILDING */
	if (ec_bdev->sb != NULL) {
		const struct ec_bdev_sb_base_bdev *sb_base = ec_bdev_sb_find_base_bdev_by_slot(ec_bdev, slot);

		if (sb_base != NULL) {
			/* Update state to FAILED if it's CONFIGURED or REBUILDING */
			if (sb_base->state == EC_SB_BASE_BDEV_CONFIGURED ||
			    sb_base->state == EC_SB_BASE_BDEV_REBUILDING) {
				if (ec_bdev_sb_update_base_bdev_state(ec_bdev, slot,
								       EC_SB_BASE_BDEV_FAILED)) {
					/* Write superblock asynchronously - but don't remove the base bdev yet */
					ec_bdev_write_superblock(ec_bdev, ec_bdev_fail_base_bdev_write_sb_cb, base_info);
				}
				return;
			}
		}
	}

	/* No superblock or slot not found - mark as failed but don't remove */
	SPDK_WARNLOG("No superblock found for failed base bdev, marking as failed but keeping in EC bdev\n");
}

/*
 * brief:
 * ec_bdev_fail_base_bdev marks a base bdev as failed and removes it from EC bdev
 * This function should be called when I/O operations fail on a base bdev.
 * It will update the superblock to mark the base bdev as FAILED and remove it.
 * When a new disk is inserted, the examine mechanism will automatically detect
 * the FAILED state and trigger rebuild.
 * params:
 * base_info - pointer to base bdev info
 * returns:
 * none
 */
void
ec_bdev_fail_base_bdev(struct ec_base_bdev_info *base_info)
{
	if (base_info == NULL) {
		SPDK_ERRLOG("base_info is NULL in ec_bdev_fail_base_bdev\n");
		return;
	}

	/* Execute on app thread to ensure thread safety */
	spdk_thread_exec_msg(spdk_thread_get_app_thread(), _ec_bdev_fail_base_bdev, base_info);
}

/* Forward declaration */
static void ec_bdev_configure_base_bdev_cont(struct ec_base_bdev_info *base_info);

/*
 * Callback for checking superblock UUID when manually adding base bdev
 * This implements scenario 5: reject disk with different EC group superblock
 */
static void
ec_bdev_configure_base_bdev_check_sb_cb(const struct ec_bdev_superblock *sb, int status,
					void *ctx)
{
	struct ec_base_bdev_info *base_info = ctx;
	struct ec_bdev *ec_bdev = base_info->ec_bdev;
	ec_base_bdev_cb configure_cb = base_info->configure_cb;
	struct spdk_bdev *bdev;

	switch (status) {
	case 0:
		/* valid superblock found */
		base_info->configure_cb = NULL;
		if (spdk_uuid_compare(&ec_bdev->bdev.uuid, &sb->uuid) == 0) {
			/* Superblock UUID matches - continue with configuration directly
			 * Don't call examine_cont because this is manual add, not auto-examine.
			 * Manual add should use configure_cont which has strict validation
			 * (slot occupancy check, rebuild decision, etc.)
			 */
			bdev = spdk_bdev_desc_get_bdev(base_info->desc);
			/* Set UUID from superblock if not already set */
			if (spdk_uuid_is_null(&base_info->uuid)) {
				const struct ec_bdev_sb_base_bdev *sb_base = ec_bdev_sb_find_base_bdev_by_uuid(sb, spdk_bdev_get_uuid(bdev));
				if (sb_base != NULL) {
					spdk_uuid_copy(&base_info->uuid, &sb_base->uuid);
				}
			}
			/* Continue with configuration using configure_cont for strict validation
			 * Note: Do NOT call ec_bdev_free_base_bdev_resource here because
			 * ec_bdev_configure_base_bdev_cont needs base_info->name, base_info->desc,
			 * and other resources. The resource cleanup will be handled by configure_cont
			 * or its error paths if needed.
			 */
			ec_bdev_configure_base_bdev_cont(base_info);
			return;
		}
		/* Superblock UUID mismatch - this disk belongs to a different EC group */
		{
			char target_uuid_str[SPDK_UUID_STRING_LEN];
			char found_uuid_str[SPDK_UUID_STRING_LEN];
			
			spdk_uuid_fmt_lower(target_uuid_str, sizeof(target_uuid_str),
					    &ec_bdev->bdev.uuid);
			spdk_uuid_fmt_lower(found_uuid_str, sizeof(found_uuid_str), &sb->uuid);
			
			SPDK_ERRLOG("===========================================================\n");
			SPDK_ERRLOG("ERROR: Base bdev '%s' has a superblock from a different EC group.\n",
				    base_info->name);
			SPDK_ERRLOG("Target EC bdev UUID: %s\n", target_uuid_str);
			SPDK_ERRLOG("Found superblock UUID: %s (EC bdev: %s)\n",
				    found_uuid_str, sb->name);
			SPDK_ERRLOG("This disk cannot be added to EC bdev '%s'.\n",
				    ec_bdev->bdev.name);
			SPDK_ERRLOG("ACTION REQUIRED: Please clear the superblock on this disk\n");
			SPDK_ERRLOG("using RPC 'bdev_wipe_superblock' before adding it to the EC group.\n");
			SPDK_ERRLOG("Example: bdev_wipe_superblock -b %s\n", base_info->name);
			SPDK_ERRLOG("===========================================================\n");
		}
		status = -EEXIST;
		ec_bdev_free_base_bdev_resource(base_info);
		break;
	case -EINVAL:
		/* no valid superblock - this is a new disk, continue with configuration */
		ec_bdev_configure_base_bdev_cont(base_info);
		return;
	default:
		SPDK_ERRLOG("Failed to examine bdev %s: %s\n",
			    base_info->name, spdk_strerror(-status));
		ec_bdev_free_base_bdev_resource(base_info);
		break;
	}

	if (configure_cb != NULL) {
		base_info->configure_cb = NULL;
		configure_cb(base_info->configure_cb_ctx, status);
	}
}

/*
 * Continue base bdev configuration after superblock check
 * This function contains the rest of the configuration logic
 */
static void
ec_bdev_configure_base_bdev_cont(struct ec_base_bdev_info *base_info)
{
	struct ec_bdev *ec_bdev = base_info->ec_bdev;
	struct spdk_bdev *bdev;
	ec_base_bdev_cb configure_cb = NULL;
	int rc;

	if (base_info->desc == NULL) {
		SPDK_ERRLOG("Base bdev desc is NULL in configure_cont\n");
		if (base_info->configure_cb) {
			configure_cb = base_info->configure_cb;
			base_info->configure_cb = NULL;
			configure_cb(base_info->configure_cb_ctx, -EINVAL);
		}
		return;
	}

	bdev = spdk_bdev_desc_get_bdev(base_info->desc);

	/*
	 * Set the EC bdev properties if this is the first base bdev configured,
	 * otherwise - verify. Assumption is that all the base bdevs for any EC bdev should
	 * have the same blocklen and metadata format.
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
			/* Clear configure_cb before freeing resources */
			if (base_info->configure_cb) {
				configure_cb = base_info->configure_cb;
				base_info->configure_cb = NULL;
			}
			ec_bdev_free_base_bdev_resource(base_info);
			if (configure_cb) {
				configure_cb(base_info->configure_cb_ctx, -EINVAL);
			}
			return;
		}

		if (ec_bdev->bdev.md_len != spdk_bdev_get_md_size(bdev) ||
		    ec_bdev->bdev.md_interleave != spdk_bdev_is_md_interleaved(bdev) ||
		    ec_bdev->bdev.dif_type != spdk_bdev_get_dif_type(bdev) ||
		    ec_bdev->bdev.dif_check_flags != bdev->dif_check_flags ||
		    ec_bdev->bdev.dif_is_head_of_md != spdk_bdev_is_dif_head_of_md(bdev) ||
		    ec_bdev->bdev.dif_pi_format != bdev->dif_pi_format) {
			SPDK_ERRLOG("EC bdev '%s' has different metadata format than base bdev '%s'\n",
				    ec_bdev->bdev.name, bdev->name);
			/* Clear configure_cb before freeing resources */
			if (base_info->configure_cb) {
				configure_cb = base_info->configure_cb;
				base_info->configure_cb = NULL;
			}
			ec_bdev_free_base_bdev_resource(base_info);
			if (configure_cb) {
				configure_cb(base_info->configure_cb_ctx, -EINVAL);
			}
			return;
		}
	}

	/* Check if this slot is already occupied (for scenario 4: old disk re-inserted after rebuild) */
	if (ec_bdev->state == EC_BDEV_STATE_ONLINE && ec_bdev->sb != NULL) {
		uint8_t slot_idx = (uint8_t)(base_info - ec_bdev->base_bdev_info);
		
		/* Check if slot is already occupied by another disk
		 * Note: We only check is_configured, not name, because name is set
		 * during the add process before configure_cont is called.
		 * If is_configured is true, it means this slot was already configured
		 * and should not be reused for a new disk.
		 */
		if (base_info->is_configured) {
			/* Slot is already occupied - this disk cannot be added */
			const struct ec_bdev_sb_base_bdev *sb_base = ec_bdev_sb_find_base_bdev_by_slot(ec_bdev, slot_idx);
			const char *uuid_match_info = "";
			
			if (sb_base != NULL && sb_base->state == EC_SB_BASE_BDEV_CONFIGURED) {
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
			SPDK_NOTICELOG("Base bdev '%s' (slot %u) cannot be added to EC bdev '%s' because the slot is already occupied.%s\n",
				       base_info->name, slot_idx, ec_bdev->bdev.name, uuid_match_info);
			SPDK_NOTICELOG("The EC bdev is already fully configured with another disk in this slot.\n");
			SPDK_NOTICELOG("If you don't need this disk for the EC bdev, please clear its superblock\n");
			SPDK_NOTICELOG("using RPC 'bdev_wipe_superblock' to make it available for other use.\n");
			SPDK_NOTICELOG("Example: bdev_wipe_superblock -b %s\n", base_info->name);
			SPDK_NOTICELOG("===========================================================\n");
			
			/* Clear configure_cb before freeing resources to ensure proper cleanup order */
			if (base_info->configure_cb) {
				configure_cb = base_info->configure_cb;
				base_info->configure_cb = NULL;
			}
			ec_bdev_free_base_bdev_resource(base_info);
			if (configure_cb) {
				configure_cb(base_info->configure_cb_ctx, -EEXIST);
			}
			return;
		}
	}

	/* Update EC bdev block count */
	if (ec_bdev->bdev.blockcnt == 0) {
		ec_bdev->bdev.blockcnt = base_info->data_size * ec_bdev->k;
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
	
	/* Relax assertion for CONFIGURING state - during reconfiguration, we may temporarily
	 * have fewer operational base bdevs than minimum required, but we're rebuilding.
	 */
	if (ec_bdev->state != EC_BDEV_STATE_CONFIGURING) {
		assert(ec_bdev->num_base_bdevs_operational >= ec_bdev->min_base_bdevs_operational);
	}
	
	/* If EC bdev is OFFLINE (deconfigured), we need to reconfigure it first.
	 * Transition to CONFIGURING state to allow configuration to proceed.
	 */
	if (ec_bdev->state == EC_BDEV_STATE_OFFLINE) {
		ec_bdev->state = EC_BDEV_STATE_CONFIGURING;
		SPDK_DEBUGLOG(bdev_ec, "EC bdev state changing from offline to configuring for reconfigure\n");
	}
	
	/* During reconfiguration in CONFIGURING state, update num_base_bdevs_operational
	 * to match num_base_bdevs_discovered if needed.
	 */
	if (ec_bdev->state == EC_BDEV_STATE_CONFIGURING &&
	    ec_bdev->num_base_bdevs_operational < ec_bdev->num_base_bdevs_discovered) {
		ec_bdev->num_base_bdevs_operational = ec_bdev->num_base_bdevs_discovered;
		SPDK_DEBUGLOG(bdev_ec, "Updating num_base_bdevs_operational to %u for reconfigure\n",
			      ec_bdev->num_base_bdevs_operational);
	}

	/* Continue configuration */
	ec_bdev_configure_cont(ec_bdev);

	/* Check if this base bdev needs rebuild. Rebuild is needed when:
	 * 1. EC bdev is ONLINE (fully operational)
	 * 2. Superblock is enabled
	 * 3. One of the following conditions is true:
	 *    a) Slot was previously MISSING or FAILED (replacement disk)
	 *    b) New disk's UUID doesn't match expected UUID (wrong disk)
	 *    c) New disk has no superblock or superblock belongs to other EC (new/foreign disk)
	 */
	if (ec_bdev->state == EC_BDEV_STATE_ONLINE && ec_bdev->sb != NULL) {
		uint8_t slot_idx = (uint8_t)(base_info - ec_bdev->base_bdev_info);
		bool needs_rebuild = false;
		const char *rebuild_reason = NULL;
		
		/* Find this slot in superblock */
		const struct ec_bdev_sb_base_bdev *sb_base = ec_bdev_sb_find_base_bdev_by_slot(ec_bdev, slot_idx);
		
		if (sb_base != NULL) {
			/* IMPORTANT: Check if slot is already CONFIGURED in EC bdev's superblock.
			 * If it's CONFIGURED, it means rebuild has already completed, and this
			 * might be an old disk with stale superblock that was re-inserted.
			 * In this case, we should NOT trigger rebuild again.
			 */
			if (sb_base->state == EC_SB_BASE_BDEV_CONFIGURED) {
				/* Slot is already CONFIGURED - rebuild has completed.
				 * Check if this is the same disk (UUID matches) or a different disk.
				 */
				if (!spdk_uuid_is_null(&sb_base->uuid) &&
				    spdk_uuid_compare(&base_info->uuid, &sb_base->uuid) == 0) {
					/* Same disk - this is the disk that was rebuilt.
					 * No need to rebuild again, just update its superblock if needed.
					 */
					SPDK_NOTICELOG("Base bdev %s (slot %u) is already CONFIGURED in EC bdev %s superblock. "
						       "This appears to be the same disk that was rebuilt. Skipping rebuild.\n",
						       base_info->name, slot_idx, ec_bdev->bdev.name);
					needs_rebuild = false;
				} else {
					/* Different disk - this is a replacement disk, but slot is already CONFIGURED.
					 * This shouldn't happen in normal flow, but if it does, we should rebuild
					 * to ensure data consistency.
					 */
					SPDK_WARNLOG("Base bdev %s (slot %u) UUID doesn't match expected UUID in EC bdev %s, "
						     "but slot is CONFIGURED. This might be an old disk re-inserted. "
						     "Will rebuild to ensure data consistency.\n",
						     base_info->name, slot_idx, ec_bdev->bdev.name);
					needs_rebuild = true;
					rebuild_reason = "UUID mismatch with CONFIGURED slot (old disk re-inserted?)";
				}
			} else {
				/* Slot is MISSING, FAILED, or REBUILDING - check if this is a replacement disk */
				if (sb_base->state == EC_SB_BASE_BDEV_MISSING ||
				    sb_base->state == EC_SB_BASE_BDEV_FAILED ||
				    sb_base->state == EC_SB_BASE_BDEV_REBUILDING) {
					/* REBUILDING state means rebuild was in progress but not completed.
					 * We should always rebuild in this case, regardless of UUID.
					 */
					if (sb_base->state == EC_SB_BASE_BDEV_REBUILDING) {
						needs_rebuild = true;
						rebuild_reason = "previous rebuild was not completed (REBUILDING state)";
					} else {
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
		
		if (needs_rebuild && !ec_bdev_is_rebuilding(ec_bdev)) {
			/* Start rebuild for this base bdev */
			SPDK_NOTICELOG("Starting automatic rebuild for base bdev %s (slot %u) on EC bdev %s: %s\n",
				       base_info->name, slot_idx, ec_bdev->bdev.name,
				       rebuild_reason ? rebuild_reason : "unknown reason");
			rc = ec_bdev_start_rebuild(ec_bdev, base_info, NULL, NULL);
			if (rc != 0) {
				SPDK_WARNLOG("Failed to start rebuild for base bdev %s: %s\n",
					     base_info->name, spdk_strerror(-rc));
				/* Don't fail configuration if rebuild start fails */
			}
		}
	}

	/* Auto-update wear leveling weights if enabled (after adding new device)
	 * This ensures new devices (which might be old user disks) are included
	 * in wear level calculations
	 */
	if (ec_bdev->state == EC_BDEV_STATE_ONLINE &&
	    ec_bdev->selection_config.wear_leveling_enabled) {
		SPDK_NOTICELOG("EC bdev %s: New device added, auto-updating wear leveling weights\n",
			       ec_bdev->bdev.name);
		int rc = ec_selection_create_profile_from_devices(ec_bdev, true, true);
		if (rc != 0) {
			SPDK_WARNLOG("EC bdev %s: Failed to update wear leveling weights after adding device: %s\n",
				     ec_bdev->bdev.name, spdk_strerror(-rc));
			/* Don't fail device addition if weight update fails */
		} else {
			SPDK_NOTICELOG("EC bdev %s: Wear leveling weights updated successfully\n",
				       ec_bdev->bdev.name);
		}
	}

	configure_cb = base_info->configure_cb;
	base_info->configure_cb = NULL;
	if (configure_cb != NULL) {
		configure_cb(base_info->configure_cb_ctx, 0);
	}
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
	
	/* Calculate data_offset - align to optimal_io_boundary if base bdev has one */
	uint64_t data_offset = EC_BDEV_MIN_DATA_OFFSET_SIZE / spdk_bdev_get_block_size(bdev);
	if (bdev->optimal_io_boundary != 0) {
		data_offset = spdk_round_up(data_offset, bdev->optimal_io_boundary);
	}
	base_info->data_offset = data_offset;
	base_info->data_size = base_info->blockcnt - base_info->data_offset;

	/* Check DIF support - EC bdev does not support DIF/DIX */
	if (spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) {
		SPDK_ERRLOG("Base bdev '%s' has DIF or DIX enabled - unsupported EC configuration\n",
			    bdev->name);
		spdk_bdev_module_release_bdev(bdev);
		spdk_bdev_close(desc);
		return -EINVAL;
	}

	/* For new base bdevs (not existing), check superblock UUID before configuring */
	if (!existing && ec_bdev->sb != NULL) {
		base_info->configure_cb = cb_fn;
		base_info->configure_cb_ctx = cb_ctx;
		rc = ec_bdev_load_base_bdev_superblock(desc, base_info->app_thread_ch,
						       ec_bdev_configure_base_bdev_check_sb_cb, base_info);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to read bdev %s superblock: %s\n",
				    bdev->name, spdk_strerror(-rc));
			base_info->configure_cb = NULL;
			spdk_bdev_module_release_bdev(bdev);
			spdk_bdev_close(desc);
			return rc;
		}
		/* If rc == 0, callback will continue with configuration */
		return 0;
	}

	/* For existing base bdevs or when superblock is not enabled, continue with configuration */
	base_info->configure_cb = cb_fn;
	base_info->configure_cb_ctx = cb_ctx;
	ec_bdev_configure_base_bdev_cont(base_info);

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
			 * from ec_bdev_deconfigure. We need to close self_desc to allow
			 * spdk_bdev_unregister to complete.
			 */
			SPDK_DEBUGLOG(bdev_ec, "EC bdev %s already removed from list, deletion in progress, closing self_desc\n",
				      ec_bdev->bdev.name);
			if (ec_bdev->self_desc != NULL) {
				spdk_bdev_close(ec_bdev->self_desc);
				ec_bdev->self_desc = NULL;
			}
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
		/* Base bdev removal event - don't wipe superblock */
		ec_bdev_delete(ec_bdev, false, NULL, NULL);
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
	SPDK_DEBUGLOG(bdev_ec, "examine_cont_wrapper called for bdev %s, status=%d, sb=%p\n",
		      bdev ? bdev->name : "unknown", status, sb);
    /* Pass a completion callback so we can signal examine_done reliably */
    ec_bdev_examine_cont(sb, bdev, ec_bdev_examine_done_cb, bdev);
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
	int rc = 0;

	SPDK_DEBUGLOG(bdev_ec, "examine_cont called for bdev %s, sb=%p\n", bdev->name, sb);
	if (sb == NULL) {
		SPDK_DEBUGLOG(bdev_ec, "No superblock found on bdev %s - this is a new disk\n", bdev->name);
		/* No superblock - this is a new disk. User must manually add it to EC bdev
		 * using RPC 'bdev_ec_add_base_bdev'. We don't auto-match to FAILED/MISSING slots.
		 * This is intentional: examine only auto-recovers known disks (with superblock),
		 * new disks require explicit user action to avoid accidental addition.
		 */
		/* Try examine_no_sb for EC bdevs without superblock */
		ec_bdev_examine_no_sb(bdev);
		if (cb_fn) {
			cb_fn(cb_ctx, 0);
		} else {
			/* Ensure examine_done is always called */
			ec_bdev_examine_done_cb(bdev, 0);
		}
		return;
	}

	{
		char uuid_str[SPDK_UUID_STRING_LEN];
		spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &sb->uuid);
		SPDK_DEBUGLOG(bdev_ec, "Superblock found on bdev %s, EC name=%s, UUID=%s\n",
			      bdev->name, sb->name, uuid_str);
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

			/* remove and then recreate the EC bdev using the newer superblock - don't wipe superblock */
			ec_bdev_delete(ec_bdev, false, NULL, NULL);
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
	sb_base_bdev = ec_bdev_sb_find_base_bdev_by_uuid(sb, spdk_bdev_get_uuid(bdev));

	if (sb_base_bdev == NULL) {
		/* This bdev's UUID is not in the superblock - it's a new/foreign disk
		 * User must manually add it to EC bdev using RPC 'bdev_ec_add_base_bdev'
		 * This is intentional: examine only auto-recovers known disks that are
		 * already in the superblock, new disks require explicit user action. */
		SPDK_DEBUGLOG(bdev_ec, "EC superblock does not contain this bdev's uuid - "
			      "this is a new disk or foreign disk, user must manually add it\n");
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

		/* For newly created EC bdev from superblock, we need to configure the current base bdev.
		 * Find the base_info for this bdev and set its name before configuring. */
		assert(sb_base_bdev->slot < ec_bdev->num_base_bdevs);
		base_info = &ec_bdev->base_bdev_info[sb_base_bdev->slot];
		
		/* Set base_info->name if not set (from superblock creation, name is not set) */
		if (base_info->name == NULL) {
			base_info->name = strdup(spdk_bdev_get_name(bdev));
			if (base_info->name == NULL) {
				SPDK_ERRLOG("Failed to allocate name for base bdev %s\n",
					    spdk_bdev_get_name(bdev));
				free(ctx);
				rc = -ENOMEM;
				goto out;
			}
		}
		
		/* Configure this base bdev */
		if (base_info->is_configured) {
			SPDK_DEBUGLOG(bdev_ec, "Base bdev %s already configured for EC bdev %s\n",
				      bdev->name, ec_bdev->bdev.name);
			/* If EC bdev is not registered yet, trigger configure_cont to check if all
			 * base bdevs are configured and register the EC bdev.
			 * Only trigger if state is CONFIGURING to avoid duplicate calls. */
			if (ec_bdev->state == EC_BDEV_STATE_CONFIGURING) {
				struct spdk_bdev *registered_bdev = spdk_bdev_get_by_name(ec_bdev->bdev.name);
				if (registered_bdev == NULL || registered_bdev != &ec_bdev->bdev) {
					SPDK_DEBUGLOG(bdev_ec, "EC bdev %s not registered yet, triggering configure_cont\n",
						      ec_bdev->bdev.name);
					ec_bdev_configure_cont(ec_bdev);
				}
			}
			rc = 0;
		} else {
			SPDK_DEBUGLOG(bdev_ec, "Configuring base bdev %s for EC bdev %s from superblock\n",
				      bdev->name, ec_bdev->bdev.name);
			rc = ec_bdev_configure_base_bdev(base_info, true, cb_fn, cb_ctx);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to configure bdev %s as base bdev of EC %s: %s\n",
					    bdev->name, ec_bdev->bdev.name, spdk_strerror(-rc));
			}
		}
		goto out;
	}

	if (ec_bdev->state == EC_BDEV_STATE_ONLINE) {
		assert(sb_base_bdev->slot < ec_bdev->num_base_bdevs);
		base_info = &ec_bdev->base_bdev_info[sb_base_bdev->slot];
		
		/* Check if this slot is already occupied by another disk
		 * This is a safety check in examine path: if slot is already occupied,
		 * we don't auto-replace it to avoid accidental data loss.
		 * User should manually remove the existing disk first if replacement is intended.
		 * Note: We only check is_configured, not name, because name may be set
		 * during configuration but is_configured is the definitive flag.
		 */
		if (base_info->is_configured) {
			/* EC bdev is already fully configured, this disk cannot be added */
			/* This can happen if superblock state is CONFIGURED but slot is occupied */
			SPDK_NOTICELOG("===========================================================\n");
			SPDK_NOTICELOG("NVMe bdev '%s' belongs to EC bdev '%s' (slot %u), but EC bdev is already fully configured.\n",
				       bdev->name, ec_bdev->bdev.name, sb_base_bdev->slot);
			SPDK_NOTICELOG("This disk cannot be automatically added to the EC bdev as the slot is already occupied.\n");
			SPDK_NOTICELOG("If you want to replace the existing disk, please:\n");
			SPDK_NOTICELOG("1. Remove the existing disk using RPC 'bdev_ec_remove_base_bdev'\n");
			SPDK_NOTICELOG("2. Then this disk will be automatically added on next examine\n");
			SPDK_NOTICELOG("If you don't need this disk for the EC bdev, please clear its superblock\n");
			SPDK_NOTICELOG("using RPC 'bdev_wipe_superblock' to make it available for other use.\n");
			SPDK_NOTICELOG("Example: bdev_wipe_superblock -b %s\n", bdev->name);
			SPDK_NOTICELOG("===========================================================\n");
			rc = 0; /* Don't treat as error, just inform user */
			goto out;
		}
		
		/* Slot is not occupied, check superblock state
		 * This is the auto-recovery path: if slot is FAILED/MISSING/REBUILDING,
		 * we automatically configure and rebuild the disk.
		 */
		if (sb_base_bdev->state == EC_SB_BASE_BDEV_MISSING ||
		    sb_base_bdev->state == EC_SB_BASE_BDEV_FAILED ||
		    sb_base_bdev->state == EC_SB_BASE_BDEV_REBUILDING) {
			/* Auto-recovering a MISSING, FAILED, or REBUILDING disk */
			assert(base_info->is_configured == false);
			assert(spdk_uuid_is_null(&base_info->uuid));
			spdk_uuid_copy(&base_info->uuid, &sb_base_bdev->uuid);
			/* is_data_block is kept for superblock compatibility but not used */
			base_info->is_data_block = sb_base_bdev->is_data_block;
			if (sb_base_bdev->state == EC_SB_BASE_BDEV_REBUILDING) {
				SPDK_NOTICELOG("Auto-recovering bdev %s to EC bdev %s (previous rebuild was not completed).\n",
					       bdev->name, ec_bdev->bdev.name);
			} else {
				SPDK_NOTICELOG("Auto-recovering bdev %s to EC bdev %s (slot was %s).\n",
					       bdev->name, ec_bdev->bdev.name,
					       sb_base_bdev->state == EC_SB_BASE_BDEV_FAILED ? "FAILED" : "MISSING");
			}
			/* Use existing=true to indicate this is from examine (auto-recovery) */
			rc = ec_bdev_configure_base_bdev(base_info, true, cb_fn, cb_ctx);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to configure bdev %s as base bdev of EC %s: %s\n",
					    bdev->name, ec_bdev->bdev.name, spdk_strerror(-rc));
			}
			goto out;
		} else if (sb_base_bdev->state == EC_SB_BASE_BDEV_CONFIGURED) {
			/* Superblock says CONFIGURED, but slot is not occupied - this is unexpected.
			 * Continue to normal processing path below to handle it. */
		} else {
			/* Unknown state */
			SPDK_WARNLOG("Unknown superblock state %u for bdev %s in EC bdev %s\n",
				     sb_base_bdev->state, bdev->name, ec_bdev->bdev.name);
			rc = -EINVAL;
			goto out;
		}
	}

	if (sb_base_bdev->state != EC_SB_BASE_BDEV_CONFIGURED) {
		SPDK_NOTICELOG("Bdev %s is not an active member of EC bdev %s. Ignoring.\n",
			       bdev->name, ec_bdev->bdev.name);
		rc = -EINVAL;
		goto out;
	}

	/* At this point, ec_bdev->state != EC_BDEV_STATE_ONLINE or
	 * ec_bdev->state == EC_BDEV_STATE_ONLINE but slot is not occupied and state is CONFIGURED.
	 * Check if EC bdev is ONLINE and the slot is already occupied by another disk */
	if (ec_bdev->state == EC_BDEV_STATE_ONLINE) {
		assert(sb_base_bdev->slot < ec_bdev->num_base_bdevs);
		base_info = &ec_bdev->base_bdev_info[sb_base_bdev->slot];
		
		if (base_info->is_configured || base_info->name != NULL) {
			/* EC bdev is already fully configured, this disk cannot be added */
			SPDK_NOTICELOG("===========================================================\n");
			SPDK_NOTICELOG("NVMe bdev '%s' belongs to EC bdev '%s' (slot %u), but EC bdev is already fully configured.\n",
				       bdev->name, ec_bdev->bdev.name, sb_base_bdev->slot);
			SPDK_NOTICELOG("This disk cannot be added to the EC bdev as all slots are occupied.\n");
			SPDK_NOTICELOG("If you don't need this disk for the EC bdev, please clear its superblock\n");
			SPDK_NOTICELOG("using RPC 'bdev_wipe_superblock' to make it available for other use.\n");
			SPDK_NOTICELOG("Example: bdev_wipe_superblock -b %s\n", bdev->name);
			SPDK_NOTICELOG("===========================================================\n");
			rc = 0; /* Don't treat as error, just inform user */
			goto out;
		}
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
		/* If EC bdev is not registered yet, trigger configure_cont to check if all
		 * base bdevs are configured and register the EC bdev.
		 * Only trigger if state is CONFIGURING to avoid duplicate calls. */
		if (ec_bdev->state == EC_BDEV_STATE_CONFIGURING) {
			struct spdk_bdev *registered_bdev = spdk_bdev_get_by_name(ec_bdev->bdev.name);
			if (registered_bdev == NULL || registered_bdev != &ec_bdev->bdev) {
				SPDK_DEBUGLOG(bdev_ec, "EC bdev %s not registered yet, triggering configure_cont\n",
					      ec_bdev->bdev.name);
				ec_bdev_configure_cont(ec_bdev);
			}
		}
		rc = 0;
		goto out;
	}

	rc = ec_bdev_configure_base_bdev(base_info, true, cb_fn, cb_ctx);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to configure bdev %s as base bdev of EC %s: %s\n",
			    bdev->name, ec_bdev->bdev.name, spdk_strerror(-rc));
	}

out:
    if (rc != 0) {
        if (cb_fn) {
            cb_fn(cb_ctx, rc);
        } else {
            ec_bdev_examine_done_cb(bdev, rc);
        }
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
				/* Bdev not found yet - try to examine it by UUID alias
				 * This will trigger examine if the bdev exists but hasn't been examined yet */
				int examine_rc = spdk_bdev_examine(uuid_str);
				if (examine_rc != 0) {
					/* Bdev doesn't exist yet or examine failed, skip it for now */
					SPDK_DEBUGLOG(bdev_ec, "Base bdev with UUID %s not found or examine failed, will retry later\n",
						      uuid_str);
					continue;
				}
				/* Re-check after examine */
				target_bdev = spdk_bdev_get_by_name(uuid_str);
				if (target_bdev == NULL) {
					/* Still not found, skip it */
					continue;
				}
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
/*
 * brief:
 * ec_bdev_examine_no_sb examines base bdev when no superblock is found
 * This allows EC bdevs created without superblock to be rebuilt by matching
 * base bdev names or UUIDs with CONFIGURING EC bdevs
 * params:
 * bdev - pointer to base bdev
 * returns:
 * none
 */
static void
ec_bdev_examine_no_sb(struct spdk_bdev *bdev)
{
	struct ec_bdev *ec_bdev;
	struct ec_base_bdev_info *base_info;

	TAILQ_FOREACH(ec_bdev, &g_ec_bdev_list, global_link) {
		/* Only examine EC bdevs that are in CONFIGURING state and have no superblock */
		if (ec_bdev->state != EC_BDEV_STATE_CONFIGURING || ec_bdev->sb != NULL) {
			continue;
		}
		
		EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
			/* Check if this base bdev slot is not yet configured */
			if (base_info->desc == NULL &&
			    ((base_info->name != NULL && strcmp(bdev->name, base_info->name) == 0) ||
			     (!spdk_uuid_is_null(&base_info->uuid) && 
			      spdk_uuid_compare(&base_info->uuid, spdk_bdev_get_uuid(bdev)) == 0))) {
				/* Found a match - configure this base bdev */
				SPDK_DEBUGLOG(bdev_ec, "Matching base bdev %s to EC bdev %s (no superblock)\n",
					      bdev->name, ec_bdev->bdev.name);
				ec_bdev_configure_base_bdev(base_info, true, NULL, NULL);
				return;
			}
		}
	}
}

static void
ec_bdev_examine(struct spdk_bdev *bdev)
{
	int rc = 0;
	struct ec_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_ec, "Examining bdev %s\n", bdev->name);
	base_info = ec_bdev_find_base_info_by_bdev(bdev);
	if (base_info != NULL) {
		SPDK_DEBUGLOG(bdev_ec, "Base bdev %s already found in EC bdev list\n", bdev->name);
		/* Base bdev is already configured. Check if EC bdev is registered.
		 * If not, we need to continue examine to complete the configuration. */
		struct ec_bdev *ec_bdev = base_info->ec_bdev;
		if (ec_bdev != NULL) {
			/* Check if EC bdev is registered by trying to get it by name */
			struct spdk_bdev *registered_bdev = spdk_bdev_get_by_name(ec_bdev->bdev.name);
			if (registered_bdev != NULL && registered_bdev == &ec_bdev->bdev) {
				/* EC bdev is registered, base bdev is configured - nothing to do */
				SPDK_DEBUGLOG(bdev_ec, "EC bdev %s is already registered, skipping examine\n",
					      ec_bdev->bdev.name);
				goto done;
			}
			/* EC bdev exists but not registered - continue examine to complete configuration */
			SPDK_DEBUGLOG(bdev_ec, "Base bdev %s is configured but EC bdev %s is not registered, continuing examine\n",
				      bdev->name, ec_bdev->bdev.name);
			/* Continue to load superblock and examine_cont to complete configuration */
		} else {
			/* Base bdev is configured but EC bdev is NULL - this shouldn't happen,
			 * but continue examine to be safe */
			SPDK_WARNLOG("Base bdev %s is configured but EC bdev is NULL, continuing examine\n",
				     bdev->name);
		}
	}

	if (spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) {
		SPDK_DEBUGLOG(bdev_ec, "Base bdev %s has DIF enabled, skipping examine\n", bdev->name);
		/* Try examine_no_sb for EC bdevs without superblock */
		ec_bdev_examine_no_sb(bdev);
		rc = 0;
		goto done;
	}

	SPDK_DEBUGLOG(bdev_ec, "Loading superblock for bdev %s\n", bdev->name);
	rc = ec_bdev_examine_load_sb(bdev->name, ec_bdev_examine_cont_wrapper, NULL);
	if (rc != 0) {
		SPDK_DEBUGLOG(bdev_ec, "Superblock load failed for bdev %s (rc=%d), trying examine_no_sb\n",
			      bdev->name, rc);
		/* Superblock load failed - try examine_no_sb for EC bdevs without superblock */
		ec_bdev_examine_no_sb(bdev);
		rc = 0;  /* Don't treat as error, examine_no_sb will handle it */
		goto done;
	}
	SPDK_DEBUGLOG(bdev_ec, "Superblock load initiated for bdev %s, waiting for callback\n", bdev->name);

	return;
done:
    SPDK_DEBUGLOG(bdev_ec, "Examine done for bdev %s with status %d\n", bdev->name, rc);
    ec_bdev_examine_done_cb(bdev, rc);
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
