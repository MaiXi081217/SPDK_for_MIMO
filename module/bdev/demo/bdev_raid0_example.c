/*   SPDX-License-Identifier: BSD-3-Clause
 *   
 *   RAID 0实现示例 - 基于Demo模块
 *   
 *   这个文件展示如何在demo的基础上实现RAID 0功能
 *   每个步骤都对应demo模块的相应步骤，展示实际实现
 */

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/util.h"

/* ========================================================================
 * 【步骤1-3：定义数据结构 - RAID 0版本】
 * ========================================================================
 * 
 * 🎯 我要实现什么？
 * 定义RAID 0的数据结构，需要管理多个底层磁盘
 * 
 * ❓ 为什么需要这个？
 * - RAID 0需要将数据分散到多个磁盘
 * - 需要知道每个磁盘的信息（描述符、通道等）
 * - 需要计算数据应该写入哪个磁盘
 * 
 * ✅ 如何实现？
 */

/* RAID 0 I/O上下文 */
struct raid0_bdev_io {
	/* 原始的RAID I/O请求 */
	struct spdk_bdev_io *raid_io;
	
	/* RAID通道 */
	struct raid0_io_channel *raid_ch;
	
	/* RAID bdev */
	struct raid0_bdev *raid_bdev;
	
	/* 在RAID中的偏移和长度（块） */
	uint64_t offset_blocks;
	uint64_t num_blocks;
	
	/* 数据缓冲区 */
	struct iovec *iovs;
	int iovcnt;
	
	/* 子I/O请求（提交到底层bdev的I/O） */
	struct spdk_bdev_io *base_io;
	
	/* 队列节点 */
	TAILQ_ENTRY(raid0_bdev_io) link;
};

/* RAID 0 Bdev结构 */
struct raid0_bdev {
	/* SPDK框架的bdev结构（必须第一个字段） */
	struct spdk_bdev bdev;
	
	/* 条带大小（块） */
	uint32_t strip_size;
	uint32_t strip_size_shift;  /* 用于快速除法：strip_size = 1 << shift */
	
	/* 底层bdev信息数组 */
	struct raid0_base_bdev_info *base_bdev_info;
	uint8_t num_base_bdevs;
	
	/* 全局链表节点 */
	TAILQ_ENTRY(raid0_bdev) tailq;
};

/* 底层bdev信息 */
struct raid0_base_bdev_info {
	/* 指向父RAID bdev */
	struct raid0_bdev *raid_bdev;
	
	/* 底层bdev名称 */
	char *name;
	
	/* 底层bdev描述符 */
	struct spdk_bdev_desc *desc;
	
	/* 数据大小（块） */
	uint64_t data_size;
	
	/* 是否失败 */
	bool is_failed;
};

/* RAID 0 I/O通道（每个线程一个） */
struct raid0_io_channel {
	/* Poller用于处理I/O */
	struct spdk_poller *poller;
	
	/* I/O请求队列 */
	TAILQ_HEAD(, raid0_bdev_io) io_queue;
	
	/* 底层bdev的通道数组（每个底层bdev一个通道） */
	struct spdk_io_channel **base_channel;
	
	/* RAID bdev指针 */
	struct raid0_bdev *raid_bdev;
};

/* ========================================================================
 * 【全局变量】
 * ========================================================================
 */

static TAILQ_HEAD(, raid0_bdev) g_raid0_bdevs = TAILQ_HEAD_INITIALIZER(g_raid0_bdevs);

/* 函数前向声明 */
static int raid0_poll_io(void *arg);
static int raid0_create_channel(void *io_device, void *ctx_buf);
static void raid0_destroy_channel(void *io_device, void *ctx_buf);
static void raid0_submit_rw_request(struct raid0_bdev_io *raid_io);
static void raid0_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

/* ========================================================================
 * 【步骤9：实现I/O处理poller函数 - RAID 0版本】
 * ========================================================================
 * 
 * 🎯 我要实现什么？
 * 处理RAID 0的I/O请求，将数据条带化到多个磁盘
 * 
 * ❓ 为什么需要这个？
 * - RAID 0需要将数据分散到多个磁盘以提高性能
 * - 需要计算数据应该写入哪个磁盘的哪个位置
 * 
 * ✅ 如何实现？
 */

static int
raid0_poll_io(void *arg)
{
	struct raid0_io_channel *ch = arg;
	struct raid0_bdev_io *raid_io;
	
	/* 从队列取出I/O */
	raid_io = TAILQ_FIRST(&ch->io_queue);
	if (raid_io == NULL) {
		return 0;  /* 队列为空 */
	}
	
	/* 从队列中移除 */
	TAILQ_REMOVE(&ch->io_queue, raid_io, link);
	
	/* 调用RAID 0的提交函数 */
	raid0_submit_rw_request(raid_io);
	
	return 1;  /* 需要立即再次调用 */
}

/* ========================================================================
 * 【核心函数：RAID 0提交读写请求】
 * ========================================================================
 * 
 * 🎯 我要实现什么？
 * 将RAID I/O请求转换为底层bdev的I/O请求
 * 
 * ❓ 为什么需要这个？
 * - RAID 0需要计算数据应该写入哪个磁盘
 * - 需要将RAID地址转换为物理磁盘地址
 * 
 * ❓ 实现这个需要什么？
 * 
 * RAID 0的地址转换公式：
 * 1. 计算条带：strip = offset / strip_size
 * 2. 计算磁盘索引：disk_idx = strip % num_disks（轮询分配）
 * 3. 计算磁盘内条带：disk_strip = strip / num_disks
 * 4. 计算磁盘内偏移：disk_offset = disk_strip * strip_size + offset_in_strip
 * 
 * 示例：
 * - RAID配置：3个磁盘，条带大小=4块
 * - RAID地址：offset=10块
 * - 计算：
 *   - strip = 10 / 4 = 2
 *   - disk_idx = 2 % 3 = 2（第3个磁盘）
 *   - disk_strip = 2 / 3 = 0
 *   - offset_in_strip = 10 % 4 = 2
 *   - disk_offset = 0 * 4 + 2 = 2块
 * 
 * ✅ 如何实现？
 */

static void
raid0_submit_rw_request(struct raid0_bdev_io *raid_io)
{
	struct raid0_bdev *raid_bdev = raid_io->raid_bdev;
	struct raid0_io_channel *raid_ch = raid_io->raid_ch;
	
	uint64_t start_strip, end_strip;
	uint64_t pd_strip;      /* Physical disk strip */
	uint32_t offset_in_strip;
	uint64_t pd_lba;        /* Physical disk LBA */
	uint8_t pd_idx;         /* Physical disk index */
	struct raid0_base_bdev_info *base_info;
	struct spdk_io_channel *base_ch;
	int rc;
	
	/* ============================================
	 * 步骤1：计算条带位置
	 * ============================================
	 */
	start_strip = raid_io->offset_blocks >> raid_bdev->strip_size_shift;
	end_strip = (raid_io->offset_blocks + raid_io->num_blocks - 1) >>
		    raid_bdev->strip_size_shift;
	
	/* 简化实现：要求I/O不能跨条带边界 */
	if (start_strip != end_strip && raid_bdev->num_base_bdevs > 1) {
		SPDK_ERRLOG("I/O spans strip boundary! Not supported in this example.\n");
		spdk_bdev_io_complete(raid_io->raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	
	/* ============================================
	 * 步骤2：计算物理磁盘索引和偏移
	 * ============================================
	 */
	/* 计算物理磁盘的条带号 */
	pd_strip = start_strip / raid_bdev->num_base_bdevs;
	
	/* 计算物理磁盘索引（轮询分配） */
	pd_idx = start_strip % raid_bdev->num_base_bdevs;
	
	/* 计算在条带内的偏移 */
	offset_in_strip = raid_io->offset_blocks & (raid_bdev->strip_size - 1);
	
	/* 计算物理磁盘的LBA */
	pd_lba = (pd_strip << raid_bdev->strip_size_shift) + offset_in_strip;
	
	/* ============================================
	 * 步骤3：获取底层bdev信息
	 * ============================================
	 */
	base_info = &raid_bdev->base_bdev_info[pd_idx];
	if (base_info->desc == NULL || base_info->is_failed) {
		SPDK_ERRLOG("Base bdev %u is not available\n", pd_idx);
		spdk_bdev_io_complete(raid_io->raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	
	/* 获取底层bdev的通道 */
	base_ch = raid_ch->base_channel[pd_idx];
	if (base_ch == NULL) {
		SPDK_ERRLOG("Base channel %u is NULL\n", pd_idx);
		spdk_bdev_io_complete(raid_io->raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	
	/* ============================================
	 * 步骤4：提交I/O到底层bdev
	 * ============================================
	 */
	enum spdk_bdev_io_type io_type = raid_io->raid_io->type;
	if (io_type == SPDK_BDEV_IO_TYPE_READ) {
		/* 读取：从对应的物理磁盘读取 */
		rc = spdk_bdev_readv_blocks(base_info->desc, base_ch,
					    raid_io->iovs, raid_io->iovcnt,
					    pd_lba, raid_io->num_blocks,
					    raid0_io_completion, raid_io);
	} else if (io_type == SPDK_BDEV_IO_TYPE_WRITE) {
		/* 写入：写入对应的物理磁盘 */
		rc = spdk_bdev_writev_blocks(base_info->desc, base_ch,
					     raid_io->iovs, raid_io->iovcnt,
					     pd_lba, raid_io->num_blocks,
					     raid0_io_completion, raid_io);
	} else {
		SPDK_ERRLOG("Unsupported I/O type: %u\n", io_type);
		spdk_bdev_io_complete(raid_io->raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	
	if (rc != 0) {
		SPDK_ERRLOG("Failed to submit I/O to base bdev %u: %s\n",
			   pd_idx, spdk_strerror(-rc));
		spdk_bdev_io_complete(raid_io->raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/* ========================================================================
 * 【I/O完成回调】
 * ========================================================================
 * 
 * 🎯 我要实现什么？
 * 处理底层bdev的I/O完成回调
 * 
 * ❓ 为什么需要这个？
 * - 底层bdev的I/O是异步的
 * - 完成后需要通知RAID I/O完成
 * 
 * ✅ 如何实现？
 */

static void
raid0_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid0_bdev_io *raid_io = cb_arg;
	
	/* 释放底层I/O */
	spdk_bdev_free_io(bdev_io);
	
	/* 完成RAID I/O */
	spdk_bdev_io_complete(raid_io->raid_io,
			     success ? SPDK_BDEV_IO_STATUS_SUCCESS :
				       SPDK_BDEV_IO_STATUS_FAILED);
	
	/* 释放RAID I/O上下文（如果需要） */
	free(raid_io);
}

/* ========================================================================
 * 【步骤13：实现submit_request函数 - RAID 0版本】
 * ========================================================================
 * 
 * 🎯 我要实现什么？
 * 接收RAID I/O请求并放入队列
 * 
 * ❓ 为什么需要这个？
 * - SPDK框架通过这个函数提交I/O
 * - 我们需要将I/O放入队列，由poller处理
 * 
 * ✅ 如何实现？
 */

static void
raid0_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct raid0_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct raid0_bdev_io *raid_io;
	
	/* 分配RAID I/O上下文 */
	raid_io = calloc(1, sizeof(struct raid0_bdev_io));
	if (raid_io == NULL) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		return;
	}
	
	/* 填充RAID I/O信息 */
	raid_io->raid_io = bdev_io;
	raid_io->raid_ch = ch;
	raid_io->raid_bdev = ch->raid_bdev;
	raid_io->offset_blocks = bdev_io->u.bdev.offset_blocks;
	raid_io->num_blocks = bdev_io->u.bdev.num_blocks;
	raid_io->iovs = bdev_io->u.bdev.iovs;
	raid_io->iovcnt = bdev_io->u.bdev.iovcnt;
	
	/* 根据I/O类型处理 */
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		/* 放入队列，由poller处理 */
		TAILQ_INSERT_TAIL(&ch->io_queue, raid_io, link);
		break;
		
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
		/* 简化实现：直接放入队列 */
		TAILQ_INSERT_TAIL(&ch->io_queue, raid_io, link);
		break;
		
	default:
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		free(raid_io);
		break;
	}
}

/* ========================================================================
 * 【步骤10：实现I/O通道创建回调 - RAID 0版本】
 * ========================================================================
 * 
 * 🎯 我要实现什么？
 * 初始化RAID 0的I/O通道，需要为每个底层bdev创建通道
 * 
 * ❓ 为什么需要这个？
 * - RAID 0需要访问多个底层bdev
 * - 每个底层bdev都需要一个通道
 * 
 * ✅ 如何实现？
 */

static int
raid0_create_channel(void *io_device, void *ctx_buf)
{
	struct raid0_bdev *raid_bdev = io_device;
	struct raid0_io_channel *ch = ctx_buf;
	uint8_t i;
	
	/* 初始化I/O队列 */
	TAILQ_INIT(&ch->io_queue);
	
	/* 保存RAID bdev指针 */
	ch->raid_bdev = raid_bdev;
	
	/* 分配底层bdev通道数组 */
	ch->base_channel = calloc(raid_bdev->num_base_bdevs,
				  sizeof(struct spdk_io_channel *));
	if (ch->base_channel == NULL) {
		return -ENOMEM;
	}
	
	/* 为每个底层bdev创建通道 */
	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		struct raid0_base_bdev_info *base_info = &raid_bdev->base_bdev_info[i];
		
		if (base_info->desc == NULL || base_info->is_failed) {
			continue;
		}
		
		/* 获取底层bdev */
		struct spdk_bdev *base_bdev = spdk_bdev_desc_get_bdev(base_info->desc);
		
		/* 创建底层bdev的通道 */
		ch->base_channel[i] = spdk_bdev_get_io_channel(base_info->desc);
		if (ch->base_channel[i] == NULL) {
			SPDK_ERRLOG("Failed to get channel for base bdev %s\n",
				   base_info->name);
			/* 清理已创建的通道 */
			for (uint8_t j = 0; j < i; j++) {
				if (ch->base_channel[j]) {
					spdk_put_io_channel(ch->base_channel[j]);
				}
			}
			free(ch->base_channel);
			return -ENOMEM;
		}
	}
	
	/* 创建poller */
	ch->poller = spdk_poller_register(raid0_poll_io, ch, 0);
	
	return 0;
}

/* ========================================================================
 * 【步骤11：实现I/O通道销毁回调 - RAID 0版本】
 * ========================================================================
 */

static void
raid0_destroy_channel(void *io_device, void *ctx_buf)
{
	struct raid0_io_channel *ch = ctx_buf;
	uint8_t i;
	
	/* 取消poller */
	spdk_poller_unregister(&ch->poller);
	
	/* 释放所有底层bdev的通道 */
	if (ch->base_channel) {
		for (i = 0; i < ch->raid_bdev->num_base_bdevs; i++) {
			if (ch->base_channel[i]) {
				spdk_put_io_channel(ch->base_channel[i]);
			}
		}
		free(ch->base_channel);
	}
}

/* ========================================================================
 * 【其他必要的函数（简化实现）】
 * ========================================================================
 */

static int
raid0_destruct(void *ctx)
{
	struct raid0_bdev *raid_bdev = ctx;
	uint8_t i;
	
	TAILQ_REMOVE(&g_raid0_bdevs, raid_bdev, tailq);
	
	/* 关闭所有底层bdev */
	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		if (raid_bdev->base_bdev_info[i].desc) {
			spdk_bdev_close(raid_bdev->base_bdev_info[i].desc);
		}
		free(raid_bdev->base_bdev_info[i].name);
	}
	
	free(raid_bdev->base_bdev_info);
	free(raid_bdev->bdev.name);
	free(raid_bdev);
	
	return 0;
}

static bool
raid0_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
		return true;
	default:
		return false;
	}
}

static struct spdk_io_channel *
raid0_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(ctx);
}

static const struct spdk_bdev_fn_table raid0_fn_table = {
	.destruct = raid0_destruct,
	.submit_request = raid0_submit_request,
	.io_type_supported = raid0_io_type_supported,
	.get_io_channel = raid0_get_io_channel,
};

/* ========================================================================
 * 【步骤17：实现bdev创建函数 - RAID 0版本】
 * ========================================================================
 * 
 * 🎯 我要实现什么？
 * 创建RAID 0 bdev，需要打开多个底层bdev
 * 
 * ❓ 为什么需要这个？
 * - RAID 0需要管理多个底层bdev
 * - 需要打开每个底层bdev的描述符
 * - 需要计算RAID的总容量
 * 
 * ✅ 如何实现？
 */

int
raid0_bdev_create(struct spdk_bdev **bdev, const char *name,
		  uint32_t strip_size, const char **base_bdev_names,
		  uint8_t num_base_bdevs)
{
	struct raid0_bdev *raid_bdev;
	uint8_t i;
	int rc;
	
	/* 检查参数 */
	if (name == NULL || strlen(name) == 0) {
		return -EINVAL;
	}
	if (strip_size == 0 || (strip_size & (strip_size - 1)) != 0) {
		/* 条带大小必须是2的幂 */
		return -EINVAL;
	}
	if (num_base_bdevs == 0 || num_base_bdevs > 255) {
		return -EINVAL;
	}
	
	/* 分配RAID bdev结构 */
	raid_bdev = calloc(1, sizeof(struct raid0_bdev));
	if (raid_bdev == NULL) {
		return -ENOMEM;
	}
	
	/* 分配底层bdev信息数组 */
	raid_bdev->base_bdev_info = calloc(num_base_bdevs,
					   sizeof(struct raid0_base_bdev_info));
	if (raid_bdev->base_bdev_info == NULL) {
		free(raid_bdev);
		return -ENOMEM;
	}
	
	/* 设置基本属性 */
	raid_bdev->bdev.name = strdup(name);
	raid_bdev->strip_size = strip_size;
	raid_bdev->strip_size_shift = spdk_u32log2(strip_size);
	raid_bdev->num_base_bdevs = num_base_bdevs;
	
	/* 打开所有底层bdev */
	uint64_t min_size = UINT64_MAX;
	for (i = 0; i < num_base_bdevs; i++) {
		struct raid0_base_bdev_info *base_info = &raid_bdev->base_bdev_info[i];
		
		base_info->raid_bdev = raid_bdev;
		base_info->name = strdup(base_bdev_names[i]);
		
		/* 打开底层bdev */
		rc = spdk_bdev_open_ext(base_bdev_names[i], true, NULL, NULL,
					&base_info->desc);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to open base bdev %s: %s\n",
				   base_bdev_names[i], spdk_strerror(-rc));
			goto cleanup;
		}
		
		/* 获取底层bdev */
		struct spdk_bdev *base_bdev = spdk_bdev_desc_get_bdev(base_info->desc);
		
		/* 记录最小容量（RAID 0容量 = 最小容量 * 磁盘数） */
		uint64_t base_size = spdk_bdev_get_num_blocks(base_bdev);
		if (base_size < min_size) {
			min_size = base_size;
		}
		
		base_info->data_size = base_size;
	}
	
	/* 计算RAID 0总容量：所有磁盘容量之和 */
	uint64_t raid_size = min_size * num_base_bdevs;
	
	/* 设置bdev属性 */
	raid_bdev->bdev.product_name = "RAID0 Disk";
	raid_bdev->bdev.blocklen = 512;  /* 假设所有底层bdev都是512字节 */
	raid_bdev->bdev.blockcnt = raid_size;
	raid_bdev->bdev.ctxt = raid_bdev;
	raid_bdev->bdev.fn_table = &raid0_fn_table;
	
	/* 注册I/O设备 */
	spdk_io_device_register(raid_bdev, raid0_create_channel,
				raid0_destroy_channel,
				sizeof(struct raid0_io_channel),
				"raid0_bdev");
	
	/* 注册bdev */
	rc = spdk_bdev_register(&raid_bdev->bdev);
	if (rc != 0) {
		spdk_io_device_unregister(raid_bdev, NULL);
		goto cleanup;
	}
	
	TAILQ_INSERT_TAIL(&g_raid0_bdevs, raid_bdev, tailq);
	*bdev = &raid_bdev->bdev;
	
	SPDK_NOTICELOG("Created RAID0 bdev: %s (strip_size: %u, num_disks: %u, size: %lu)\n",
		       name, strip_size, num_base_bdevs, raid_size);
	
	return 0;

cleanup:
	/* 清理资源 */
	for (uint8_t j = 0; j < i; j++) {
		if (raid_bdev->base_bdev_info[j].desc) {
			spdk_bdev_close(raid_bdev->base_bdev_info[j].desc);
		}
		free(raid_bdev->base_bdev_info[j].name);
	}
	free(raid_bdev->base_bdev_info);
	free(raid_bdev->bdev.name);
	free(raid_bdev);
	return rc;
}

