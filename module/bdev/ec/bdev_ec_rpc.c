/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/bdev.h"
#include "bdev_ec.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/env.h"

#include <string.h>
#include <errno.h>

#define RPC_MAX_BASE_BDEVS 255

/*
 * Input structure for bdev_ec_get_bdevs RPC
 */
struct rpc_bdev_ec_get_bdevs {
	/* category - all or online or configuring or offline */
	char *category;
};

/*
 * brief:
 * free_rpc_bdev_ec_get_bdevs function frees RPC bdev_ec_get_bdevs related parameters
 */
static void
free_rpc_bdev_ec_get_bdevs(struct rpc_bdev_ec_get_bdevs *req)
{
	free(req->category);
}

/*
 * Decoder object for RPC get_ecs
 */
static const struct spdk_json_object_decoder rpc_bdev_ec_get_bdevs_decoders[] = {
	{"category", offsetof(struct rpc_bdev_ec_get_bdevs, category), spdk_json_decode_string, true},
};

/*
 * brief:
 * rpc_bdev_ec_get_bdevs function is the RPC for getting EC bdevs list.
 */
static void
rpc_bdev_ec_get_bdevs(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_bdev_ec_get_bdevs   req = {};
	struct spdk_json_write_ctx  *w;
	struct ec_bdev            *ec_bdev;
	enum ec_bdev_state        state;

	if (spdk_json_decode_object(params, rpc_bdev_ec_get_bdevs_decoders,
				    SPDK_COUNTOF(rpc_bdev_ec_get_bdevs_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (req.category == NULL) {
		req.category = strdup("all");
		if (req.category == NULL) {
			spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
			goto cleanup;
		}
	}

	state = ec_bdev_str_to_state(req.category);
	if (state == EC_BDEV_STATE_MAX && strcmp(req.category, "all") != 0) {
		spdk_jsonrpc_send_error_response(request, -EINVAL, spdk_strerror(EINVAL));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	/* Get EC bdev list based on the category requested */
	/* Note: RPC handlers run on the app thread, so no explicit locking needed
	 * for g_ec_bdev_list access. All modifications to the list also happen
	 * on the app thread (via RPC or initialization). */
	TAILQ_FOREACH(ec_bdev, &g_ec_bdev_list, global_link) {
		if (ec_bdev->state == state || state == EC_BDEV_STATE_MAX) {
			char uuid_str[SPDK_UUID_STRING_LEN];

			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "name", ec_bdev->bdev.name);
			spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &ec_bdev->bdev.uuid);
			spdk_json_write_named_string(w, "uuid", uuid_str);
			ec_bdev_write_info_json(ec_bdev, w);
			spdk_json_write_object_end(w);
		}
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_ec_get_bdevs(&req);
}
SPDK_RPC_REGISTER("bdev_ec_get_bdevs", rpc_bdev_ec_get_bdevs, SPDK_RPC_RUNTIME)

/*
 * Base bdev information in RPC bdev_ec_create
 */
struct rpc_bdev_ec_base_bdev {
	char *name;
};

/*
 * Base bdevs in RPC bdev_ec_create
 */
struct rpc_bdev_ec_create_base_bdevs {
	/* Number of base bdevs */
	size_t                        num_base_bdevs;

	/* List of base bdevs with their roles */
	struct rpc_bdev_ec_base_bdev  base_bdevs[RPC_MAX_BASE_BDEVS];
};

/*
 * Input structure for RPC rpc_bdev_ec_create
 */
struct rpc_bdev_ec_create {
	/* EC bdev name */
	char                                 *name;

	/* Number of data blocks (k) */
	uint8_t                              k;

	/* Number of parity blocks (p) */
	uint8_t                              p;

	/* EC strip size in KB */
	uint32_t                             strip_size_kb;

	/* Base bdevs list - all disks are equal, parity distributed round-robin */
	struct rpc_bdev_ec_create_base_bdevs base_bdevs;

	/* UUID for this EC bdev */
	struct spdk_uuid		     uuid;

	/* If set, information about EC bdev will be stored in superblock on each base bdev */
	bool                                 superblock_enabled;
};

/*
 * Decoder function for RPC bdev_ec_create to decode base bdev entry
 */
static int
decode_ec_base_bdev_entry(const struct spdk_json_val *val, void *out)
{
	struct rpc_bdev_ec_base_bdev *base_bdev = out;
	const struct spdk_json_object_decoder decoders[] = {
		{"name", offsetof(struct rpc_bdev_ec_base_bdev, name), spdk_json_decode_string},
	};
	int rc;

	/* Initialize to default values to avoid UB from uninitialized memory */
	memset(base_bdev, 0, sizeof(*base_bdev));

	/* Try to decode as object format first */
	rc = spdk_json_decode_object(val, decoders, SPDK_COUNTOF(decoders), base_bdev);
	if (rc == 0) {
		/* Object format decoded successfully */
		return 0;
	}

	/* Object decode failed, try as string format (old format) */
	base_bdev->name = NULL;
	rc = spdk_json_decode_string(val, &base_bdev->name);
	if (rc == 0) {
		/* String format decoded successfully */
		return 0;
	}

	/* Both formats failed - return error */
	return -1;
}

/*
 * Decoder function for RPC bdev_ec_create to decode base bdevs list
 */
static int
decode_ec_base_bdevs(const struct spdk_json_val *val, void *out)
{
	struct rpc_bdev_ec_create_base_bdevs *base_bdevs = out;
	int rc;

	rc = spdk_json_decode_array(val, decode_ec_base_bdev_entry, base_bdevs->base_bdevs,
				    RPC_MAX_BASE_BDEVS, &base_bdevs->num_base_bdevs,
				    sizeof(struct rpc_bdev_ec_base_bdev));
	if (rc != 0) {
		return rc;
	}

	/* Validate array size */
	if (base_bdevs->num_base_bdevs == 0) {
		return -EINVAL; /* Empty array is invalid - need at least one base bdev */
	}

	if (base_bdevs->num_base_bdevs > RPC_MAX_BASE_BDEVS) {
		/* This should not happen as spdk_json_decode_array enforces the limit,
		 * but check for safety */
		return -EINVAL;
	}

	return 0;
}

/*
 * Decoder object for RPC bdev_ec_create
 */
static const struct spdk_json_object_decoder rpc_bdev_ec_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_ec_create, name), spdk_json_decode_string},
	{"k", offsetof(struct rpc_bdev_ec_create, k), spdk_json_decode_uint8},
	{"p", offsetof(struct rpc_bdev_ec_create, p), spdk_json_decode_uint8},
	{"strip_size_kb", offsetof(struct rpc_bdev_ec_create, strip_size_kb), spdk_json_decode_uint32, true},
	{"base_bdevs", offsetof(struct rpc_bdev_ec_create, base_bdevs), decode_ec_base_bdevs},
	{"uuid", offsetof(struct rpc_bdev_ec_create, uuid), spdk_json_decode_uuid, true},
	{"superblock", offsetof(struct rpc_bdev_ec_create, superblock_enabled), spdk_json_decode_bool, true},
};

struct rpc_bdev_ec_create_ctx {
	struct rpc_bdev_ec_create req;
	struct ec_bdev *ec_bdev;
	struct spdk_jsonrpc_request *request;
	uint8_t remaining;
	int status;
	uint8_t k;
	uint8_t p;
	char *missing_base_bdev_name; /* Name of base bdev that was not found (for error reporting) */
};

static void
free_rpc_bdev_ec_create_ctx(struct rpc_bdev_ec_create_ctx *ctx)
{
	struct rpc_bdev_ec_create *req;
	size_t i;

	if (!ctx) {
		return;
	}

	req = &ctx->req;

	free(req->name);
	for (i = 0; i < req->base_bdevs.num_base_bdevs; i++) {
		free(req->base_bdevs.base_bdevs[i].name);
	}
	free(ctx->missing_base_bdev_name);

	free(ctx);
}

static void
rpc_bdev_ec_create_add_base_bdev_cb(void *_ctx, int status)
{
	struct rpc_bdev_ec_create_ctx *ctx = _ctx;

	if (status != 0) {
		ctx->status = status;
	}

	/* Runtime check instead of assert (assert may be disabled in release builds) */
	if (ctx->remaining == 0) {
		SPDK_ERRLOG("Invalid state: remaining counter is 0 in callback\n");
		free_rpc_bdev_ec_create_ctx(ctx);
		return;
	}

	if (--ctx->remaining > 0) {
		return;
	}

	if (ctx->status != 0) {
		/* Only cleanup if ec_bdev was successfully created */
		if (ctx->ec_bdev != NULL) {
			/* If EC bdev is still in CONFIGURING state and no base bdevs were configured,
			 * we can directly free it without going through the full delete process.
			 * This avoids potential issues with uninitialized resources.
			 *
			 * Note: This direct free path is safe because:
			 * 1. No base bdevs have been configured (num_base_bdevs_discovered == 0)
			 * 2. EC bdev is not registered with bdev layer yet (still CONFIGURING)
			 * 3. No I/O channels or other resources have been allocated
			 * 4. No other components hold references to this ec_bdev
			 *
			 * If any base bdevs were configured, we must use the full delete process
			 * to properly clean up resources (I/O channels, base bdev claims, etc.)
			 */
			if (ctx->ec_bdev->state == EC_BDEV_STATE_CONFIGURING &&
			    ctx->ec_bdev->num_base_bdevs_discovered == 0) {
				/* Direct cleanup for EC bdev that hasn't been fully configured.
				 * Since ec_bdev_delete checks destroy_started and removes from list,
				 * we need to check if it's already in the list before removing.
				 * However, since we're in the create callback and deletion hasn't
				 * started yet, we can safely remove it here. But to be consistent
				 * with ec_bdev_delete, let's use it instead of direct free.
				 */
				/* Check if ec_bdev is still in the list */
				struct ec_bdev *iter;
				bool found = false;
				TAILQ_FOREACH(iter, &g_ec_bdev_list, global_link) {
					if (iter == ctx->ec_bdev) {
						found = true;
						break;
					}
				}
				if (found) {
					/* Use ec_bdev_delete for consistent cleanup - don't wipe superblock on error */
					ec_bdev_delete(ctx->ec_bdev, false, NULL, NULL);
				} else {
					/* Already removed, just free */
					ec_bdev_free(ctx->ec_bdev);
				}
			} else {
				/* Use full delete process for configured EC bdevs - don't wipe superblock on error */
				ec_bdev_delete(ctx->ec_bdev, false, NULL, NULL);
			}
		}
		
		if (ctx->status == -ENODEV) {
			if (ctx->missing_base_bdev_name != NULL) {
				spdk_jsonrpc_send_error_response_fmt(ctx->request, ctx->status,
							     "Failed to create EC bdev %s: base bdev '%s' not found",
							     ctx->req.name, ctx->missing_base_bdev_name);
			} else {
				spdk_jsonrpc_send_error_response_fmt(ctx->request, ctx->status,
							     "Failed to create EC bdev %s: base bdev not found",
							     ctx->req.name);
			}
		} else {
			/* ctx->status is negative errno, normalize for spdk_strerror */
			int err = ctx->status;
			if (err > 0) {
				err = -err;
			}
			spdk_jsonrpc_send_error_response_fmt(ctx->request, ctx->status,
						     "Failed to create EC bdev %s: %s",
						     ctx->req.name,
						     spdk_strerror(-err));
		}
    } else {
        /* Return the created EC bdev name */
		struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(ctx->request);
		spdk_json_write_string(w, ctx->req.name);
		spdk_jsonrpc_end_result(ctx->request, w);
	}

	free_rpc_bdev_ec_create_ctx(ctx);
}

/*
 * brief:
 * rpc_bdev_ec_create function is the RPC for creating EC bdevs. It takes
 * input as EC bdev name, strip size in KB and list of base bdev names with
 * their roles (data or parity).
 */
static void
rpc_bdev_ec_create(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_bdev_ec_create	*req;
	struct ec_bdev			*ec_bdev;
	int				rc;
	size_t				i;
	struct rpc_bdev_ec_create_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		goto cleanup;
	}
	req = &ctx->req;
	/* ctx is zero-initialized by calloc, but explicitly initialize req for clarity */
	memset(req, 0, sizeof(*req));

	if (spdk_json_decode_object(params, rpc_bdev_ec_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_ec_create_decoders),
				    req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}



	/* Validate k and p */
	if (req->k == 0 || req->p == 0) {
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
					     "Must have at least one data block (k) and one parity block (p)");
		goto cleanup;
	}

	/* Note: req->k and req->p are uint8_t, so they are automatically limited to 0-255.
	 * EC_MAX_K and EC_MAX_P are 255, so no explicit range check is needed here.
	 * The decoder will reject values outside uint8_t range automatically. */

	/* Validate that number of base bdevs matches k + p */
	if (req->base_bdevs.num_base_bdevs != req->k + req->p) {
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
					     "Number of base bdevs (%zu) must equal k + p (%u + %u = %u)",
					     req->base_bdevs.num_base_bdevs, req->k, req->p, req->k + req->p);
		goto cleanup;
	}

	/* Validate base bdev names */
	for (i = 0; i < req->base_bdevs.num_base_bdevs; i++) {
		if (req->base_bdevs.base_bdevs[i].name == NULL ||
		    strlen(req->base_bdevs.base_bdevs[i].name) == 0) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
						     "The base bdev name cannot be empty");
			goto cleanup;
		}
	}

	ctx->k = req->k;
	ctx->p = req->p;


	/* Create EC bdev */
	rc = ec_bdev_create(req->name, req->strip_size_kb, req->k, req->p,
		   req->superblock_enabled, &req->uuid, &ec_bdev);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc,
					     "Failed to create EC bdev %s: %s",
					     req->name, spdk_strerror(-rc));
		goto cleanup;
	}

	ctx->ec_bdev = ec_bdev;
	ctx->request = request;
	ctx->remaining = req->base_bdevs.num_base_bdevs;

	assert(ctx->remaining > 0);

	/* Add base bdevs - all disks are equal, parity distributed round-robin */
	for (i = 0; i < req->base_bdevs.num_base_bdevs; i++) {
		const char *base_bdev_name = req->base_bdevs.base_bdevs[i].name;

		rc = ec_bdev_add_base_bdev(ec_bdev, base_bdev_name,
				   rpc_bdev_ec_create_add_base_bdev_cb, ctx);
		if (rc != 0) {
			/* ec_bdev_add_base_bdev failed - it doesn't call the callback on error,
			 * so we need to handle it here to avoid hanging.
			 */
			SPDK_DEBUGLOG(bdev_ec, "Failed to add base bdev %s: %s\n",
				      base_bdev_name, spdk_strerror(-rc));
			
			if (rc == -ENODEV) {
				/* Base bdev doesn't exist - fail immediately */
				ctx->status = -ENODEV;
				/* Store the name of the missing base bdev for error reporting */
				ctx->missing_base_bdev_name = strdup(base_bdev_name);
				if (ctx->missing_base_bdev_name == NULL) {
					SPDK_ERRLOG("Failed to allocate memory for missing base bdev name\n");
				}
			} else {
				/* Other errors */
				if (ctx->status == 0) {
					ctx->status = rc;
				}
			}
			
			/* Decrement remaining for all base bdevs that haven't been added yet.
			 * Note: We need to account for:
			 * - The current base bdev that failed (i)
			 * - All remaining base bdevs that won't be added (num_base_bdevs - i - 1)
			 * Total to cancel: (num_base_bdevs - i) base bdevs
			 * Since ec_bdev_add_base_bdev doesn't call the callback on error,
			 * we need to cancel all remaining base bdevs including the current one.
			 */
			size_t remaining_to_cancel = req->base_bdevs.num_base_bdevs - i;
			if (remaining_to_cancel > 0 && remaining_to_cancel <= ctx->remaining) {
				ctx->remaining -= (uint8_t)remaining_to_cancel;
			}
			
			/* Call callback to delete EC bdev and send error response.
			 * This will handle cleanup and response. */
			rpc_bdev_ec_create_add_base_bdev_cb(ctx, rc);
			break;
		}
		/* else: rc == 0, callback will be called asynchronously by ec_bdev_configure_base_bdev */
	}
	return;
cleanup:
	free_rpc_bdev_ec_create_ctx(ctx);
}
SPDK_RPC_REGISTER("bdev_ec_create", rpc_bdev_ec_create, SPDK_RPC_RUNTIME)

/*
 * Input structure for RPC deleting an EC bdev
 */
struct rpc_bdev_ec_delete {
	/* EC bdev name */
	char *name;
};

/*
 * brief:
 * free_rpc_bdev_ec_delete function is used to free RPC bdev_ec_delete related parameters
 */
static void
free_rpc_bdev_ec_delete(struct rpc_bdev_ec_delete *req)
{
	free(req->name);
}

/*
 * Decoder object for RPC ec_bdev_delete
 */
static const struct spdk_json_object_decoder rpc_bdev_ec_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_ec_delete, name), spdk_json_decode_string},
};

struct rpc_bdev_ec_delete_ctx {
	struct rpc_bdev_ec_delete req;
	struct spdk_jsonrpc_request *request;
};

/*
 * brief:
 * bdev_ec_delete_done callback function for deletion completion
 */
static void
bdev_ec_delete_done(void *cb_arg, int rc)
{
	struct rpc_bdev_ec_delete_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request = ctx->request;

	if (rc != 0) {
		SPDK_ERRLOG("Failed to delete EC bdev %s (%d): %s\n",
			    ctx->req.name, rc, spdk_strerror(-rc));
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-rc));
		goto exit;
	}

	spdk_jsonrpc_send_bool_response(request, true);
exit:
	free_rpc_bdev_ec_delete(&ctx->req);
	free(ctx);
}

/*
 * brief:
 * rpc_bdev_ec_delete function is the RPC for deleting an EC bdev.
 */
static void
rpc_bdev_ec_delete(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_bdev_ec_delete_ctx *ctx;
	struct ec_bdev *ec_bdev;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_ec_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_ec_delete_decoders),
				    &ctx->req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	ec_bdev = ec_bdev_find_by_name(ctx->req.name);
	if (ec_bdev == NULL) {
		spdk_jsonrpc_send_error_response_fmt(request, -ENODEV,
						     "EC bdev %s not found",
						     ctx->req.name);
		goto cleanup;
	}

	ctx->request = request;

	/* RPC delete command - wipe superblock */
	ec_bdev_delete(ec_bdev, true, bdev_ec_delete_done, ctx);

	return;

cleanup:
	free_rpc_bdev_ec_delete(&ctx->req);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_ec_delete", rpc_bdev_ec_delete, SPDK_RPC_RUNTIME)

/*
 * Input structure for RPC adding base bdev to EC bdev
 */
struct rpc_bdev_ec_add_base_bdev {
	/* EC bdev name */
	char *ec_bdev;
	/* Base bdev name */
	char *base_bdev;
};

/*
 * brief:
 * free_rpc_bdev_ec_add_base_bdev function frees RPC bdev_ec_add_base_bdev related parameters
 */
static void
free_rpc_bdev_ec_add_base_bdev(struct rpc_bdev_ec_add_base_bdev *req)
{
	free(req->ec_bdev);
	free(req->base_bdev);
}

/*
 * Decoder object for RPC bdev_ec_add_base_bdev
 */
static const struct spdk_json_object_decoder rpc_bdev_ec_add_base_bdev_decoders[] = {
	{"ec_bdev", offsetof(struct rpc_bdev_ec_add_base_bdev, ec_bdev), spdk_json_decode_string},
	{"base_bdev", offsetof(struct rpc_bdev_ec_add_base_bdev, base_bdev), spdk_json_decode_string},
};

static void
rpc_bdev_ec_add_base_bdev_done(void *ctx, int status)
{
	struct spdk_jsonrpc_request *request = ctx;

	if (status != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, status, "Failed to add base bdev to EC bdev: %s",
						     spdk_strerror(-status));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}

/*
 * brief:
 * bdev_ec_add_base_bdev function is the RPC for adding base bdev to an EC bdev.
 * It takes base bdev and EC bdev names as input.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
rpc_bdev_ec_add_base_bdev(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_bdev_ec_add_base_bdev req = {};
	struct ec_bdev *ec_bdev;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_ec_add_base_bdev_decoders,
				    SPDK_COUNTOF(rpc_bdev_ec_add_base_bdev_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	ec_bdev = ec_bdev_find_by_name(req.ec_bdev);
	if (ec_bdev == NULL) {
		spdk_jsonrpc_send_error_response_fmt(request, -ENODEV, "EC bdev %s is not found in config",
						     req.ec_bdev);
		goto cleanup;
	}

	rc = ec_bdev_add_base_bdev(ec_bdev, req.base_bdev,
				   rpc_bdev_ec_add_base_bdev_done, request);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "Failed to add base bdev %s to EC bdev %s: %s",
						     req.base_bdev, req.ec_bdev,
						     spdk_strerror(-rc));
		goto cleanup;
	}

	return;

cleanup:
	free_rpc_bdev_ec_add_base_bdev(&req);
}
SPDK_RPC_REGISTER("bdev_ec_add_base_bdev", rpc_bdev_ec_add_base_bdev, SPDK_RPC_RUNTIME)

/*
 * Decoder object for RPC bdev_ec_remove_base_bdev
 */
static const struct spdk_json_object_decoder rpc_bdev_ec_remove_base_bdev_decoders[] = {
	{"name", 0, spdk_json_decode_string},
};

static void
rpc_bdev_ec_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
}

static void
rpc_bdev_ec_remove_base_bdev_done(void *ctx, int status)
{
	struct spdk_jsonrpc_request *request = ctx;

	if (status != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, status, "Failed to remove base bdev from EC bdev");
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}

/*
 * brief:
 * bdev_ec_remove_base_bdev function is the RPC for removing base bdev from an EC bdev.
 * It takes base bdev name as input.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
rpc_bdev_ec_remove_base_bdev(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct spdk_bdev_desc *desc;
	char *name = NULL;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_ec_remove_base_bdev_decoders,
				    SPDK_COUNTOF(rpc_bdev_ec_remove_base_bdev_decoders),
				    &name)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	rc = spdk_bdev_open_ext(name, false, rpc_bdev_ec_event_cb, NULL, &desc);
	free(name);
	if (rc != 0) {
		goto err;
	}

	rc = ec_bdev_remove_base_bdev(spdk_bdev_desc_get_bdev(desc), rpc_bdev_ec_remove_base_bdev_done,
				      request);
	spdk_bdev_close(desc);
	if (rc != 0) {
		/* If ec_bdev_remove_base_bdev returns error, callback is not called,
		 * so we need to send error response here. */
		rpc_bdev_ec_remove_base_bdev_done(request, rc);
		return;
	}

	/* If successful, callback will be called synchronously by ec_bdev_remove_base_bdev */
	return;
err:
	/* If spdk_bdev_open_ext failed, desc is NULL, so no need to close */
	rpc_bdev_ec_remove_base_bdev_done(request, rc);
}
SPDK_RPC_REGISTER("bdev_ec_remove_base_bdev", rpc_bdev_ec_remove_base_bdev, SPDK_RPC_RUNTIME)

