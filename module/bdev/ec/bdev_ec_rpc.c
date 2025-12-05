/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/bdev.h"
#include "bdev_ec.h"
#include "bdev_ec_internal.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/env.h"

#include <string.h>
#include <errno.h>

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
	struct rpc_bdev_ec_base_bdev  base_bdevs[EC_RPC_MAX_BASE_BDEVS];
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

	/* Enable wear leveling for device selection */
	bool                                 wear_leveling_enabled;

	/* Enable debug logging for device selection */
	bool                                 debug_enabled;

	/* EC 扩展模式（NORMAL / SPARE / HYBRID / EXPAND），缺省为 NORMAL */
	enum ec_expansion_mode               expansion_mode;
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
				    EC_RPC_MAX_BASE_BDEVS, &base_bdevs->num_base_bdevs,
				    sizeof(struct rpc_bdev_ec_base_bdev));
	if (rc != 0) {
		return rc;
	}

	/* Validate array size */
	if (base_bdevs->num_base_bdevs == 0) {
		return -EINVAL; /* Empty array is invalid - need at least one base bdev */
	}

	if (base_bdevs->num_base_bdevs > EC_RPC_MAX_BASE_BDEVS) {
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
	{"wear_leveling_enabled", offsetof(struct rpc_bdev_ec_create, wear_leveling_enabled), spdk_json_decode_bool, true},
	{"debug_enabled", offsetof(struct rpc_bdev_ec_create, debug_enabled), spdk_json_decode_bool, true},
	/* 扩展模式：使用 int32 解码到 enum，保持与 RAID RPC 风格一致；可选参数，默认 NORMAL */
	{"expansion_mode", offsetof(struct rpc_bdev_ec_create, expansion_mode), spdk_json_decode_int32, true},
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

/* Forward declaration for context free helper used in error cleanup */
static void free_rpc_bdev_ec_create_ctx(struct rpc_bdev_ec_create_ctx *ctx);

/*
 * Helper to cleanup EC bdev (if allocated) and send a detailed error response
 * for bdev_ec_create failures. Used by both the synchronous error path in
 * rpc_bdev_ec_create and the asynchronous callback path.
 */
static void
rpc_bdev_ec_create_cleanup_and_send_error(struct rpc_bdev_ec_create_ctx *ctx)
{
	if (ctx->ec_bdev != NULL) {
		/* Cleanup EC bdev.
		 * If EC bdev is still in CONFIGURING state and no base bdevs were configured,
		 * we can delete it directly without going through the full delete process.
		 * Otherwise, use ec_bdev_delete for proper cleanup.
		 */
		if (ctx->ec_bdev->state == EC_BDEV_STATE_CONFIGURING &&
		    ctx->ec_bdev->num_base_bdevs_discovered == 0) {
			struct ec_bdev *iter;
			bool found = false;

			TAILQ_FOREACH(iter, &g_ec_bdev_list, global_link) {
				if (iter == ctx->ec_bdev) {
					found = true;
					break;
				}
			}
			if (found) {
				ec_bdev_delete(ctx->ec_bdev, false, NULL, NULL);
			} else {
				ec_bdev_free(ctx->ec_bdev);
			}
		} else {
			ec_bdev_delete(ctx->ec_bdev, false, NULL, NULL);
		}
	}

	/* Send error response with detailed error reason */
	if (ctx->status == -ENODEV) {
		if (ctx->missing_base_bdev_name != NULL) {
			spdk_jsonrpc_send_error_response_fmt(ctx->request, ctx->status,
							     "Failed to create EC bdev %s: base bdev '%s' not found (device may not exist or not be registered)",
							     ctx->req.name, ctx->missing_base_bdev_name);
		} else {
			spdk_jsonrpc_send_error_response_fmt(ctx->request, ctx->status,
							     "Failed to create EC bdev %s: base bdev not found (device may not exist or not be registered)",
							     ctx->req.name);
		}
	} else if (ctx->status == -EPERM) {
		if (ctx->missing_base_bdev_name != NULL) {
			spdk_jsonrpc_send_error_response_fmt(ctx->request, ctx->status,
							     "Failed to create EC bdev %s: base bdev '%s' is already claimed by another module (RAID, FTL, or another EC bdev). Please remove the existing bdev first",
							     ctx->req.name, ctx->missing_base_bdev_name);
		} else {
			spdk_jsonrpc_send_error_response_fmt(ctx->request, ctx->status,
							     "Failed to create EC bdev %s: one or more base bdevs are already claimed by another module (RAID, FTL, or another EC bdev). Please remove the existing bdevs first",
							     ctx->req.name);
		}
	} else if (ctx->status == -EINVAL) {
		spdk_jsonrpc_send_error_response_fmt(ctx->request, ctx->status,
						     "Failed to create EC bdev %s: invalid parameter (check base bdev configuration, UUID mismatch, or other validation errors)",
						     ctx->req.name);
	} else if (ctx->status == -ENOMEM) {
		spdk_jsonrpc_send_error_response_fmt(ctx->request, ctx->status,
						     "Failed to create EC bdev %s: out of memory",
						     ctx->req.name);
	} else {
		/* ctx->status is negative errno, normalize for spdk_strerror */
		int err = ctx->status;

		if (err > 0) {
			err = -err;
		}
		spdk_jsonrpc_send_error_response_fmt(ctx->request, ctx->status,
						     "Failed to create EC bdev %s: %s (error code: %d)",
						     ctx->req.name,
						     spdk_strerror(-err), ctx->status);
	}

	free_rpc_bdev_ec_create_ctx(ctx);
}

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
	struct ec_bdev *ec_bdev;

	if (status != 0) {
		ctx->status = status;
	}

	if (--ctx->remaining > 0) {
		/* Still waiting for more base bdevs to be added */
		return;
	}

	if (ctx->status != 0) {
		rpc_bdev_ec_create_cleanup_and_send_error(ctx);
	} else {
		ec_bdev = ctx->ec_bdev;

		/* 根据扩展模式设置 active / spare 标记（仅在创建完成时生效） */
		if (ctx->req.expansion_mode == EC_EXPANSION_MODE_NORMAL) {
			/* NORMAL：所有非失败 base bdev 作为 active，行为与旧版本一致 */
			struct ec_base_bdev_info *base_info;
			uint8_t active = 0;

			EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
				if (base_info->desc != NULL && !base_info->is_failed) {
					base_info->is_active = true;
					base_info->is_spare = false;
					active++;

					/* 如果启用了 superblock，同步写入 CONFIGURED 状态 */
					if (ec_bdev->sb != NULL) {
						uint8_t slot = (uint8_t)(base_info - ec_bdev->base_bdev_info);
						ec_bdev_sb_update_base_bdev_state(ec_bdev, slot,
										  EC_SB_BASE_BDEV_CONFIGURED);
					}
				}
			}
			ec_bdev->expansion_mode = EC_EXPANSION_MODE_NORMAL;
			ec_bdev->num_active_bdevs_before_expansion = active;
			ec_bdev->num_active_bdevs_after_expansion = active;
		} else if (ctx->req.expansion_mode == EC_EXPANSION_MODE_SPARE) {
			/* SPARE：前 k+p 个健康盘作为 active，其余盘标记为 spare */
			struct ec_base_bdev_info *base_info;
			uint8_t required = ctx->k + ctx->p;
			uint8_t active = 0;
			uint8_t spare = 0;

			EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
				if (base_info->desc == NULL || base_info->is_failed) {
					continue;
				}

				if (active < required) {
					base_info->is_active = true;
					base_info->is_spare = false;
					active++;

					/* Superblock 中标记为 CONFIGURED */
					if (ec_bdev->sb != NULL) {
						uint8_t slot = (uint8_t)(base_info - ec_bdev->base_bdev_info);
						ec_bdev_sb_update_base_bdev_state(ec_bdev, slot,
										  EC_SB_BASE_BDEV_CONFIGURED);
					}
				} else {
					base_info->is_active = false;
					base_info->is_spare = true;
					spare++;

					/* Superblock 中标记为 SPARE */
					if (ec_bdev->sb != NULL) {
						uint8_t slot = (uint8_t)(base_info - ec_bdev->base_bdev_info);
						ec_bdev_sb_update_base_bdev_state(ec_bdev, slot,
										  EC_SB_BASE_BDEV_SPARE);
					}
				}
			}

			ec_bdev->expansion_mode = EC_EXPANSION_MODE_SPARE;
			ec_bdev->num_active_bdevs_before_expansion = required;
			ec_bdev->num_active_bdevs_after_expansion = active;

			/* 验证active数量是否符合要求 */
			if (active < required) {
				SPDK_WARNLOG("EC[spare] EC bdev %s: WARNING - only %u active devices configured, "
					     "expected %u (k=%u, p=%u). Some base bdevs may have failed during creation.\n",
					     ec_bdev->bdev.name, active, required, ctx->k, ctx->p);
			}

			SPDK_NOTICELOG("EC[spare] EC bdev %s created in SPARE mode: active=%u, spare=%u (k=%u, p=%u)\n",
				       ec_bdev->bdev.name, active, spare, ctx->k, ctx->p);
		} else {
			/* 其他模式暂未实现，先当 NORMAL 处理（所有盘 active） */
			struct ec_base_bdev_info *base_info;
			uint8_t active = 0;

			EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
				if (base_info->desc != NULL && !base_info->is_failed) {
					base_info->is_active = true;
					base_info->is_spare = false;
					active++;
				}
			}
			ec_bdev->expansion_mode = EC_EXPANSION_MODE_NORMAL;
			ec_bdev->num_active_bdevs_before_expansion = active;
			ec_bdev->num_active_bdevs_after_expansion = active;
		}

		/* 如果 superblock 启用，统一写入一次，落盘当前 active/spare 状态 */
		if (ec_bdev->superblock_enabled && ec_bdev->sb != NULL) {
			ec_bdev_write_superblock(ec_bdev, NULL, NULL);
		}

		/* All base bdevs added successfully, configure wear leveling if requested */
		if (ctx->req.wear_leveling_enabled || ctx->req.debug_enabled) {
			struct ec_device_selection_config *config = &ctx->ec_bdev->selection_config;
			bool need_reinit = false;

			/* Update wear leveling settings if provided */
			if (ctx->req.wear_leveling_enabled != config->wear_leveling_enabled) {
				config->wear_leveling_enabled = ctx->req.wear_leveling_enabled;
				need_reinit = true;
			}

			/* Update debug flag if provided */
			if (ctx->req.debug_enabled) {
				config->debug_enabled = ctx->req.debug_enabled;
			}

			/* Re-initialize selection config if wear leveling was enabled */
			if (need_reinit && config->wear_leveling_enabled) {
				int rc = ec_bdev_init_selection_config(ctx->ec_bdev);
				if (rc != 0) {
					SPDK_WARNLOG("EC bdev %s: Failed to initialize wear leveling: %s\n",
						     ctx->req.name, spdk_strerror(-rc));
					/* Don't fail creation if wear leveling init fails */
				} else {
					SPDK_NOTICELOG("EC bdev %s: Wear leveling enabled during creation\n",
						       ctx->req.name);
				}
			}

			/* Save configuration to superblock if enabled */
			if (ctx->ec_bdev->superblock_enabled && ctx->ec_bdev->sb != NULL) {
				ec_bdev_sb_save_selection_metadata(ctx->ec_bdev);
				ec_bdev_write_superblock(ctx->ec_bdev, NULL, NULL);
			}
		}

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

	/* 规范 expansion_mode：未传或非法时统一视为 NORMAL，保证向后兼容 */
	if (req->expansion_mode < EC_EXPANSION_MODE_NORMAL ||
	    req->expansion_mode > EC_EXPANSION_MODE_EXPAND) {
		req->expansion_mode = EC_EXPANSION_MODE_NORMAL;
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

	/* Validate number of base bdevs
	 * - NORMAL：必须等于 k + p（保持旧行为完全不变）
	 * - SPARE：必须 >= k + p（额外盘作为 spare，后续在选择逻辑中通过 is_active 过滤）
	 * - 其他模式（HYBRID/EXPAND）：暂时按 NORMAL 处理，要求等于 k + p（后续接能力时再放开）
	 */
	if (req->expansion_mode == EC_EXPANSION_MODE_SPARE) {
		if (req->base_bdevs.num_base_bdevs < req->k + req->p) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
					     "Spare mode requires at least k + p base bdevs (%u + %u = %u), got %zu",
					     req->k, req->p, req->k + req->p, req->base_bdevs.num_base_bdevs);
			goto cleanup;
		}
	} else {
	if (req->base_bdevs.num_base_bdevs != req->k + req->p) {
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
					     "Number of base bdevs (%zu) must equal k + p (%u + %u = %u) in this mode",
					     req->base_bdevs.num_base_bdevs, req->k, req->p, req->k + req->p);
		goto cleanup;
		}
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
		if (rc == -EEXIST) {
			spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "EC bdev with name '%s' already exists. Please delete the existing bdev first or use a different name",
						     req->name);
		} else if (rc == -EINVAL) {
			spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "Failed to create EC bdev %s: invalid parameter (name too long, invalid strip_size, k/p values, or other validation errors)",
						     req->name);
		} else if (rc == -ENOMEM) {
			spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "Failed to create EC bdev %s: out of memory",
						     req->name);
		} else {
			spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "Failed to create EC bdev %s: %s (error code: %d)",
						     req->name, spdk_strerror(-rc), rc);
		}
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
			} else if (rc == -EPERM) {
				/* Base bdev is already claimed - fail immediately */
				ctx->status = -EPERM;
				/* Store the name of the claimed base bdev for error reporting */
				if (ctx->missing_base_bdev_name == NULL) {
					ctx->missing_base_bdev_name = strdup(base_bdev_name);
					if (ctx->missing_base_bdev_name == NULL) {
						SPDK_ERRLOG("Failed to allocate memory for claimed base bdev name\n");
					}
				}
			} else {
				/* Other errors */
				if (ctx->status == 0) {
					ctx->status = rc;
				}
				/* Store the name of the failed base bdev for error reporting if not already set */
				if (ctx->missing_base_bdev_name == NULL) {
					ctx->missing_base_bdev_name = strdup(base_bdev_name);
					if (ctx->missing_base_bdev_name == NULL) {
						SPDK_ERRLOG("Failed to allocate memory for failed base bdev name\n");
					}
				}
			}

			/* Synchronous error: cleanup and respond immediately */
			rpc_bdev_ec_create_cleanup_and_send_error(ctx);
			return;
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

/*
 * RPC structure for bdev_ec_set_selection_strategy
 */
struct rpc_bdev_ec_set_selection_strategy {
	char *name;
	uint8_t stripe_group_size;  /* For fault tolerance in wear leveling (built-in) */
	bool wear_leveling_enabled;
	bool debug_enabled;
	bool refresh_wear_levels;  /* Refresh wear levels from devices */
};

static void
free_rpc_bdev_ec_set_selection_strategy(struct rpc_bdev_ec_set_selection_strategy *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_ec_set_selection_strategy_decoders[] = {
	{"name", offsetof(struct rpc_bdev_ec_set_selection_strategy, name), spdk_json_decode_string},
	{"stripe_group_size", offsetof(struct rpc_bdev_ec_set_selection_strategy, stripe_group_size), spdk_json_decode_uint8, true},
	{"wear_leveling_enabled", offsetof(struct rpc_bdev_ec_set_selection_strategy, wear_leveling_enabled), spdk_json_decode_bool, true},
	{"debug_enabled", offsetof(struct rpc_bdev_ec_set_selection_strategy, debug_enabled), spdk_json_decode_bool, true},
	{"refresh_wear_levels", offsetof(struct rpc_bdev_ec_set_selection_strategy, refresh_wear_levels), spdk_json_decode_bool, true},
};

/*
 * RPC handler for bdev_ec_set_selection_strategy
 * Configures device selection strategy (fault tolerance + wear leveling)
 */
static void
rpc_bdev_ec_set_selection_strategy(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct rpc_bdev_ec_set_selection_strategy req = {};
	struct ec_bdev *ec_bdev;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_ec_set_selection_strategy_decoders,
				    SPDK_COUNTOF(rpc_bdev_ec_set_selection_strategy_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	ec_bdev = ec_bdev_find_by_name(req.name);
	if (ec_bdev == NULL) {
		spdk_jsonrpc_send_error_response_fmt(request, -ENODEV,
						     "EC bdev %s not found", req.name);
		goto cleanup;
	}

	SPDK_NOTICELOG("EC bdev %s: Configuring device selection strategy\n", req.name);

	struct ec_device_selection_config *config = &ec_bdev->selection_config;

	/* Update stripe_group_size (for fault tolerance in wear leveling, built-in) */
	if (req.stripe_group_size > 0 && req.stripe_group_size <= ec_bdev->num_base_bdevs) {
		config->stripe_group_size = req.stripe_group_size;
	} else {
		/* Default to number of base bdevs if invalid or not provided */
		config->stripe_group_size = ec_bdev->num_base_bdevs;
		if (req.stripe_group_size > 0) {
			SPDK_WARNLOG("EC bdev %s: Invalid stripe_group_size %u, using default %u\n",
				     req.name, req.stripe_group_size, config->stripe_group_size);
		}
	}
	SPDK_NOTICELOG("EC bdev %s: Stripe group size set to %u (for fault tolerance in wear leveling)\n",
		       req.name, config->stripe_group_size);

	/* Update wear leveling settings (if provided) */
	if (req.wear_leveling_enabled != config->wear_leveling_enabled) {
		config->wear_leveling_enabled = req.wear_leveling_enabled;
		if (config->wear_leveling_enabled) {
			SPDK_NOTICELOG("EC bdev %s: Wear leveling enabled, reading wear levels...\n",
				       req.name);
			/* Re-read wear levels */
			rc = ec_bdev_init_selection_config(ec_bdev);
			if (rc != 0) {
				spdk_jsonrpc_send_error_response_fmt(request, rc,
								     "Failed to initialize wear leveling: %s",
								     spdk_strerror(-rc));
				goto cleanup;
			}
		} else {
			SPDK_NOTICELOG("EC bdev %s: Wear leveling disabled\n", req.name);
		}
	}

	/* Update debug flag (if provided) */
	if (req.debug_enabled) {
		config->debug_enabled = req.debug_enabled;
		if (config->debug_enabled) {
			SPDK_NOTICELOG("EC bdev %s: Debug logging enabled for device selection\n",
				       req.name);
		}
	}

	/* Refresh wear levels if requested */
	if (req.refresh_wear_levels) {
		if (!config->wear_leveling_enabled) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
							     "Wear leveling is not enabled for EC bdev %s", req.name);
			goto cleanup;
		}

		SPDK_NOTICELOG("EC bdev %s: Refreshing wear levels for all devices...\n", req.name);
		rc = ec_selection_create_profile_from_devices(ec_bdev, true, true);
		if (rc != 0) {
			spdk_jsonrpc_send_error_response_fmt(request, rc,
							     "Failed to refresh wear levels: %s",
							     spdk_strerror(-rc));
			goto cleanup;
		}
		SPDK_NOTICELOG("EC bdev %s: Wear levels refreshed successfully\n", req.name);
	}

	/* Update selection function based on current configuration
	 * 逻辑说明：
	 * 1. 如果启用磨损均衡，使用磨损均衡算法（内置容错保证）
	 * 2. 如果未启用磨损均衡，使用默认 round-robin（已经保证不重叠）
	 * 注意：容错保证是磨损均衡算法的内置功能，不是独立配置项
	 */
	if (config->wear_leveling_enabled) {
		/* 磨损均衡算法内置容错保证，确保同组条带不共享设备 */
		config->select_fn = ec_select_base_bdevs_wear_leveling;
		SPDK_NOTICELOG("EC bdev %s: Using wear-leveling device selection (with built-in fault tolerance)\n", req.name);
	} else {
		/* 使用默认 round-robin（已经保证不重叠） */
		config->select_fn = ec_select_base_bdevs_default;
		SPDK_NOTICELOG("EC bdev %s: Using default round-robin device selection\n", req.name);
	}

	SPDK_NOTICELOG("EC bdev %s: Device selection strategy configured successfully\n",
		       req.name);

	/* Save configuration to superblock if enabled */
	if (ec_bdev->superblock_enabled && ec_bdev->sb != NULL) {
		/* Update superblock with current configuration (includes wear leveling config) */
		ec_bdev_sb_save_selection_metadata(ec_bdev);
		ec_bdev_write_superblock(ec_bdev, NULL, NULL);
		SPDK_NOTICELOG("EC bdev %s: Wear leveling configuration saved to superblock\n",
			       req.name);
	}

	spdk_jsonrpc_send_bool_response(request, true);
	free_rpc_bdev_ec_set_selection_strategy(&req);
	return;

cleanup:
	free_rpc_bdev_ec_set_selection_strategy(&req);
}
SPDK_RPC_REGISTER("bdev_ec_set_selection_strategy", rpc_bdev_ec_set_selection_strategy, SPDK_RPC_RUNTIME)

/*
 * RPC structure for bdev_ec_refresh_wear_levels
 */
struct rpc_bdev_ec_refresh_wear_levels {
	char *name;
};

static void
free_rpc_bdev_ec_refresh_wear_levels(struct rpc_bdev_ec_refresh_wear_levels *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_ec_refresh_wear_levels_decoders[] = {
	{"name", offsetof(struct rpc_bdev_ec_refresh_wear_levels, name), spdk_json_decode_string},
};

/*
 * RPC handler for bdev_ec_refresh_wear_levels
 * Refreshes wear levels for all devices and recalculates weights
 */
static void
rpc_bdev_ec_refresh_wear_levels(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_bdev_ec_refresh_wear_levels req = {};
	struct ec_bdev *ec_bdev;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_ec_refresh_wear_levels_decoders,
				    SPDK_COUNTOF(rpc_bdev_ec_refresh_wear_levels_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	ec_bdev = ec_bdev_find_by_name(req.name);
	if (ec_bdev == NULL) {
		spdk_jsonrpc_send_error_response_fmt(request, -ENODEV,
						     "EC bdev %s not found", req.name);
		goto cleanup;
	}

	struct ec_device_selection_config *config = &ec_bdev->selection_config;

	/* Check if wear leveling is enabled */
	if (!config->wear_leveling_enabled) {
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
						     "Wear leveling is not enabled for EC bdev %s", req.name);
		goto cleanup;
	}

	SPDK_NOTICELOG("EC bdev %s: Refreshing wear levels for all devices...\n", req.name);

	/* Create new wear profile and make it active */
	rc = ec_selection_create_profile_from_devices(ec_bdev, true, true);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "Failed to refresh wear levels: %s",
						     spdk_strerror(-rc));
		goto cleanup;
	}

	SPDK_NOTICELOG("EC bdev %s: Wear levels refreshed successfully\n", req.name);

	spdk_jsonrpc_send_bool_response(request, true);
	free_rpc_bdev_ec_refresh_wear_levels(&req);
	return;

cleanup:
	free_rpc_bdev_ec_refresh_wear_levels(&req);
}
SPDK_RPC_REGISTER("bdev_ec_refresh_wear_levels", rpc_bdev_ec_refresh_wear_levels, SPDK_RPC_RUNTIME)

