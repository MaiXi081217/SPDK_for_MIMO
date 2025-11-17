/*   SPDX-License-Identifier: BSD-3-Clause
 *   RPC接口实现文件
 *   这个文件实现了通过JSON-RPC创建和删除demo bdev的功能
 */

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/bdev_module.h"
#include "spdk/log.h"

#include "bdev_demo.h"

/* ============================================
 * RPC：创建demo bdev
 * ============================================
 * 
 * RPC方法名：bdev_demo_create
 * 参数：
 *   - name: bdev名称（必需）
 *   - num_blocks: 块数量（必需）
 *   - block_size: 块大小，字节（可选，默认512）
 * 
 * 返回：创建的bdev名称
 */

/* RPC请求参数结构 */
struct rpc_create_demo {
	char *name;
	uint64_t num_blocks;
	uint32_t block_size;
};

/* 释放RPC请求参数的内存 */
static void
free_rpc_create_demo(struct rpc_create_demo *req)
{
	free(req->name);
}

/* JSON解码器数组
 * 这个数组告诉SPDK如何从JSON中解析参数
 */
static const struct spdk_json_object_decoder rpc_create_demo_decoders[] = {
	{"name", offsetof(struct rpc_create_demo, name), spdk_json_decode_string},
	{"num_blocks", offsetof(struct rpc_create_demo, num_blocks), spdk_json_decode_uint64},
	{"block_size", offsetof(struct rpc_create_demo, block_size), spdk_json_decode_uint32, true},
};

/* RPC处理函数 */
static void
rpc_bdev_demo_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_create_demo req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;
	int rc;

	/* 解析JSON参数 */
	if (spdk_json_decode_object(params, rpc_create_demo_decoders,
				    SPDK_COUNTOF(rpc_create_demo_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* 设置默认值 */
	if (req.block_size == 0) {
		req.block_size = 512;  /* 默认512字节 */
	}

	/* 调用创建函数 */
	rc = bdev_demo_create(&bdev, req.name, req.num_blocks, req.block_size);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	/* 返回成功结果 */
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, bdev->name);
	spdk_jsonrpc_end_result(request, w);
	
	free_rpc_create_demo(&req);
	return;

cleanup:
	free_rpc_create_demo(&req);
}

/* 注册RPC方法
 * SPDK_RPC_RUNTIME表示这个方法可以在运行时调用
 */
SPDK_RPC_REGISTER("bdev_demo_create", rpc_bdev_demo_create, SPDK_RPC_RUNTIME)

/* ============================================
 * RPC：删除demo bdev
 * ============================================
 * 
 * RPC方法名：bdev_demo_delete
 * 参数：
 *   - name: 要删除的bdev名称（必需）
 * 
 * 返回：true表示成功
 */

/* RPC请求参数结构 */
struct rpc_delete_demo {
	char *name;
};

/* 释放RPC请求参数的内存 */
static void
free_rpc_delete_demo(struct rpc_delete_demo *req)
{
	free(req->name);
}

/* JSON解码器数组 */
static const struct spdk_json_object_decoder rpc_delete_demo_decoders[] = {
	{"name", offsetof(struct rpc_delete_demo, name), spdk_json_decode_string},
};

/* 删除完成后的回调函数 */
static void
rpc_bdev_demo_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

/* RPC处理函数 */
static void
rpc_bdev_demo_delete(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_delete_demo req = {};

	/* 解析JSON参数 */
	if (spdk_json_decode_object(params, rpc_delete_demo_decoders,
				    SPDK_COUNTOF(rpc_delete_demo_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* 调用删除函数 */
	bdev_demo_delete(req.name, rpc_bdev_demo_delete_cb, request);
	
	free_rpc_delete_demo(&req);
	return;

cleanup:
	free_rpc_delete_demo(&req);
}

/* 注册RPC方法 */
SPDK_RPC_REGISTER("bdev_demo_delete", rpc_bdev_demo_delete, SPDK_RPC_RUNTIME)

