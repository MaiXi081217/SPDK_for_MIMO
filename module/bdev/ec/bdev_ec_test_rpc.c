/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/bdev.h"
#include "bdev_ec.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/uuid.h"
#include "spdk/json.h"

/*
 * RPC 测试命令：bdev_ec_test
 * 用于测试 EC 模块的基本功能
 */
static void
rpc_bdev_ec_test(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct ec_bdev *ec_bdev = NULL;
	struct spdk_uuid uuid;
	int rc;
	int test_count = 0;
	int pass_count = 0;
	int fail_count = 0;

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "test_suite", "ec_bdev");
	spdk_json_write_named_array_begin(w, "results");

	/* 测试 1: 创建 EC bdev */
	test_count++;
	spdk_uuid_generate(&uuid);
	rc = ec_bdev_create("test_ec", 128, 2, 2, true, &uuid, &ec_bdev);
	if (rc == 0 && ec_bdev != NULL) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "test", "create_ec_bdev");
		spdk_json_write_named_bool(w, "passed", true);
		spdk_json_write_named_string(w, "message", "EC bdev created successfully");
		spdk_json_write_object_end(w);
		pass_count++;
	} else {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "test", "create_ec_bdev");
		spdk_json_write_named_bool(w, "passed", false);
		spdk_json_write_named_string_fmt(w, "message", "Failed to create EC bdev: %s", spdk_strerror(-rc));
		spdk_json_write_object_end(w);
		fail_count++;
	}

	/* 测试 2: 验证 EC bdev 属性 */
	if (ec_bdev != NULL) {
		test_count++;
		if (ec_bdev->k == 2 && ec_bdev->p == 2 && ec_bdev->strip_size_kb == 128) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "test", "verify_ec_properties");
			spdk_json_write_named_bool(w, "passed", true);
			spdk_json_write_named_string(w, "message", "EC properties verified");
			spdk_json_write_object_end(w);
			pass_count++;
		} else {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "test", "verify_ec_properties");
			spdk_json_write_named_bool(w, "passed", false);
			spdk_json_write_named_string(w, "message", "EC properties mismatch");
			spdk_json_write_object_end(w);
			fail_count++;
		}
	}

	/* 测试 3: 检查重建状态 API */
	if (ec_bdev != NULL) {
		test_count++;
		bool is_rebuilding = ec_bdev_is_rebuilding(ec_bdev);
		uint64_t current_stripe = 0, total_stripes = 0;
		rc = ec_bdev_get_rebuild_progress(ec_bdev, &current_stripe, &total_stripes);
		if (rc == -ENODEV && !is_rebuilding) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "test", "rebuild_progress_api");
			spdk_json_write_named_bool(w, "passed", true);
			spdk_json_write_named_string(w, "message", "Rebuild progress API works correctly");
			spdk_json_write_object_end(w);
			pass_count++;
		} else {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "test", "rebuild_progress_api");
			spdk_json_write_named_bool(w, "passed", false);
			spdk_json_write_named_string_fmt(w, "message", "Unexpected rebuild state: rc=%d, is_rebuilding=%d", rc, is_rebuilding);
			spdk_json_write_object_end(w);
			fail_count++;
		}
	}

	/* 清理 */
	if (ec_bdev != NULL) {
		ec_bdev_delete(ec_bdev, false, NULL, NULL);
	}

	/* 测试 4: 重复创建同名 EC 应该失败 */
	test_count++;
	spdk_uuid_generate(&uuid);
	rc = ec_bdev_create("test_ec", 128, 2, 2, true, &uuid, &ec_bdev);
	if (rc == 0 && ec_bdev != NULL) {
		ec_bdev_delete(ec_bdev, false, NULL, NULL);
		ec_bdev = NULL;
	}
	rc = ec_bdev_create("test_ec", 128, 2, 2, true, &uuid, &ec_bdev);
	if (rc != 0) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "test", "duplicate_name_rejection");
		spdk_json_write_named_bool(w, "passed", true);
		spdk_json_write_named_string(w, "message", "Duplicate name correctly rejected");
		spdk_json_write_object_end(w);
		pass_count++;
	} else {
		if (ec_bdev != NULL) {
			ec_bdev_delete(ec_bdev, false, NULL, NULL);
		}
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "test", "duplicate_name_rejection");
		spdk_json_write_named_bool(w, "passed", false);
		spdk_json_write_named_string(w, "message", "Duplicate name not rejected");
		spdk_json_write_object_end(w);
		fail_count++;
	}

	spdk_json_write_array_end(w);
	spdk_json_write_named_int(w, "total_tests", test_count);
	spdk_json_write_named_int(w, "passed", pass_count);
	spdk_json_write_named_int(w, "failed", fail_count);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("bdev_ec_test", rpc_bdev_ec_test, SPDK_RPC_RUNTIME)

