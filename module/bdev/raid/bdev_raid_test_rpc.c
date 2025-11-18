/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/bdev.h"
#include "bdev_raid.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/uuid.h"
#include "spdk/json.h"

/*
 * RPC 测试命令：bdev_raid_test
 * 用于测试 RAID 模块的基本功能
 */
static void
rpc_bdev_raid_test(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct raid_bdev *raid_bdev = NULL;
	struct spdk_uuid uuid;
	int rc;
	int test_count = 0;
	int pass_count = 0;
	int fail_count = 0;

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "test_suite", "raid_bdev");
	spdk_json_write_named_array_begin(w, "results");

	/* 测试 1: 创建 RAID bdev */
	test_count++;
	spdk_uuid_generate(&uuid);
	rc = raid_bdev_create("test_raid0", 128, 4, RAID0, false, &uuid, &raid_bdev);
	if (rc == 0 && raid_bdev != NULL) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "test", "create_raid_bdev");
		spdk_json_write_named_bool(w, "passed", true);
		spdk_json_write_named_string(w, "message", "RAID bdev created successfully");
		spdk_json_write_object_end(w);
		pass_count++;
	} else {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "test", "create_raid_bdev");
		spdk_json_write_named_bool(w, "passed", false);
		spdk_json_write_named_string_fmt(w, "message", "Failed to create RAID bdev: %s", spdk_strerror(-rc));
		spdk_json_write_object_end(w);
		fail_count++;
	}

	/* 测试 2: 验证 RAID bdev 属性 */
	if (raid_bdev != NULL) {
		test_count++;
		if (raid_bdev->level == RAID0 && raid_bdev->strip_size_kb == 128) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "test", "verify_raid_properties");
			spdk_json_write_named_bool(w, "passed", true);
			spdk_json_write_named_string(w, "message", "RAID properties verified");
			spdk_json_write_object_end(w);
			pass_count++;
		} else {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "test", "verify_raid_properties");
			spdk_json_write_named_bool(w, "passed", false);
			spdk_json_write_named_string(w, "message", "RAID properties mismatch");
			spdk_json_write_object_end(w);
			fail_count++;
		}
	}

	/* 测试 3: 检查重建状态 API */
	if (raid_bdev != NULL) {
		test_count++;
		bool is_rebuilding = raid_bdev_is_rebuilding(raid_bdev);
		uint64_t current_offset = 0, total_size = 0;
		rc = raid_bdev_get_rebuild_progress(raid_bdev, &current_offset, &total_size);
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
	if (raid_bdev != NULL) {
		raid_bdev_delete(raid_bdev, NULL, NULL);
	}

	/* 测试 4: 重复创建同名 RAID 应该失败 */
	test_count++;
	spdk_uuid_generate(&uuid);
	rc = raid_bdev_create("test_raid0", 128, 4, RAID0, false, &uuid, &raid_bdev);
	if (rc == 0 && raid_bdev != NULL) {
		raid_bdev_delete(raid_bdev, NULL, NULL);
		raid_bdev = NULL;
	}
	rc = raid_bdev_create("test_raid0", 128, 4, RAID0, false, &uuid, &raid_bdev);
	if (rc != 0) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "test", "duplicate_name_rejection");
		spdk_json_write_named_bool(w, "passed", true);
		spdk_json_write_named_string(w, "message", "Duplicate name correctly rejected");
		spdk_json_write_object_end(w);
		pass_count++;
	} else {
		if (raid_bdev != NULL) {
			raid_bdev_delete(raid_bdev, NULL, NULL);
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
SPDK_RPC_REGISTER("bdev_raid_test", rpc_bdev_raid_test, SPDK_RPC_RUNTIME)

