/*   SPDX-License-Identifier: BSD-3-Clause
 *   这是一个SPDK Bdev模块的演示文件
 *   我们将逐步构建一个完整的bdev模块
 */

#ifndef SPDK_BDEV_DEMO_H
#define SPDK_BDEV_DEMO_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"

/**
 * 创建demo bdev的函数声明
 * 这个函数会被RPC调用
 * 
 * @param bdev 输出参数，返回创建的bdev指针
 * @param name bdev的名称
 * @param num_blocks bdev的块数量
 * @param block_size 每个块的大小（字节）
 * @return 0表示成功，负数表示失败
 */
int bdev_demo_create(struct spdk_bdev **bdev, const char *name,
		     uint64_t num_blocks, uint32_t block_size);

/**
 * 删除demo bdev的函数声明
 * 
 * @param name 要删除的bdev名称
 * @param cb_fn 删除完成后的回调函数
 * @param cb_arg 回调函数的参数
 */
void bdev_demo_delete(const char *name, spdk_bdev_unregister_cb cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_DEMO_H */

