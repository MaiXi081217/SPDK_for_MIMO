/*   SPDX-License-Identifier: BSD-3-Clause
 *   这是SPDK Bdev模块的核心实现文件
 *   我们将一步一步地构建这个模块
 */

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "bdev_demo.h"

/* ============================================
 * 第一步：定义数据结构
 * ============================================
 * 
 * 一个bdev模块需要定义几个关键的数据结构：
 * 1. bdev_io结构：每个I/O请求的上下文
 * 2. bdev结构：代表一个bdev设备
 * 3. io_channel结构：每个线程的I/O通道
 */

/* I/O请求的上下文结构
 * 这个结构会被放在spdk_bdev_io的driver_ctx字段中
 * 大小由get_ctx_size()函数返回
 */
struct demo_bdev_io {
	/* 用于将I/O放入队列的链表节点 */
	TAILQ_ENTRY(demo_bdev_io) link;
};

/* Demo Bdev设备结构
 * 每个demo bdev设备都有一个这样的结构
 */
struct demo_bdev {
	/* 这是SPDK框架要求的，必须放在第一个字段 */
	struct spdk_bdev bdev;
	
	/* 用于将bdev放入全局链表的节点 */
	TAILQ_ENTRY(demo_bdev) tailq;
};

/* I/O通道结构
 * 每个线程都会有一个这样的通道
 * 用于管理该线程的I/O请求
 */
struct demo_io_channel {
	/* 一个poller，用于异步处理I/O */
	struct spdk_poller *poller;
	
	/* I/O请求队列 */
	TAILQ_HEAD(, demo_bdev_io) io_queue;
};

/* ============================================
 * 第二步：定义全局变量和函数声明
 * ============================================
 */

/* 所有demo bdev设备的链表头 */
static TAILQ_HEAD(, demo_bdev) g_demo_bdevs = TAILQ_HEAD_INITIALIZER(g_demo_bdevs);

/* 函数前向声明 */
static int bdev_demo_poll_io(void *arg);
static int bdev_demo_create_channel(void *io_device, void *ctx_buf);
static void bdev_demo_destroy_channel(void *io_device, void *ctx_buf);

/* ============================================
 * 第三步：定义模块接口结构
 * ============================================
 * 
 * 这是SPDK框架识别模块的关键结构
 * 它定义了模块的生命周期函数
 */

/* 模块初始化函数（稍后实现） */
static int bdev_demo_initialize(void);

/* 模块清理函数（稍后实现） */
static void bdev_demo_finish(void);

/* 返回I/O上下文的大小 */
static int
bdev_demo_get_ctx_size(void)
{
	/* 返回我们定义的I/O上下文结构的大小 */
	return sizeof(struct demo_bdev_io);
}

/* 模块接口结构
 * 这是SPDK框架用来管理模块的核心结构
 */
static struct spdk_bdev_module demo_if = {
	.name = "demo",                    /* 模块名称 */
	.module_init = bdev_demo_initialize,  /* 初始化函数 */
	.module_fini = bdev_demo_finish,       /* 清理函数 */
	.async_fini = false,                  /* 是否异步清理 */
	.get_ctx_size = bdev_demo_get_ctx_size, /* 返回I/O上下文大小 */
};

/* 注册模块到SPDK框架
 * 这个宏会在程序启动时自动调用
 */
SPDK_BDEV_MODULE_REGISTER(demo, &demo_if)

/* ============================================
 * 第四步：实现模块初始化函数
 * ============================================
 */
static int
bdev_demo_initialize(void)
{
	/* 这里可以做一些模块级别的初始化工作
	 * 比如分配全局资源、初始化全局数据结构等
	 * 
	 * 对于我们的demo模块，暂时不需要做什么
	 */
	SPDK_NOTICELOG("Demo bdev module initialized\n");
	return 0;
}

/* ============================================
 * 第五步：实现模块清理函数
 * ============================================
 */
static void
bdev_demo_finish(void)
{
	/* 这里可以做一些模块级别的清理工作
	 * 比如释放全局资源等
	 * 
	 * 注意：此时所有的bdev应该已经被删除了
	 */
	SPDK_NOTICELOG("Demo bdev module finished\n");
}

/* ============================================
 * 第六步：实现bdev销毁函数
 * ============================================
 * 
 * 这个函数会在bdev被删除时调用
 * 它负责释放bdev相关的资源
 */
static int
bdev_demo_destruct(void *ctx)
{
	struct demo_bdev *demo_bdev = ctx;
	
	/* 从全局链表中移除 */
	TAILQ_REMOVE(&g_demo_bdevs, demo_bdev, tailq);
	
	/* 释放bdev名称的内存 */
	free(demo_bdev->bdev.name);
	
	/* 释放bdev结构本身 */
	free(demo_bdev);
	
	return 0;
}

/* ============================================
 * 第七步：实现I/O提交函数
 * ============================================
 * 
 * 这是bdev模块最核心的函数
 * 当有I/O请求时，SPDK框架会调用这个函数
 * 
 * 参数说明：
 * - _ch: I/O通道（每个线程一个）
 * - bdev_io: I/O请求结构
 */
static void
bdev_demo_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct demo_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct demo_bdev_io *demo_io = (struct demo_bdev_io *)bdev_io->driver_ctx;
	
	/* 根据I/O类型进行处理 */
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		/* 对于READ请求，我们需要：
		 * 1. 将I/O放入队列
		 * 2. Poller会异步处理并完成I/O
		 */
		TAILQ_INSERT_TAIL(&ch->io_queue, demo_io, link);
		break;
		
	case SPDK_BDEV_IO_TYPE_WRITE:
		/* 对于WRITE请求，同样放入队列 */
		TAILQ_INSERT_TAIL(&ch->io_queue, demo_io, link);
		break;
		
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		/* WRITE_ZEROES：写入零 */
		TAILQ_INSERT_TAIL(&ch->io_queue, demo_io, link);
		break;
		
	case SPDK_BDEV_IO_TYPE_RESET:
		/* RESET：重置设备 */
		TAILQ_INSERT_TAIL(&ch->io_queue, demo_io, link);
		break;
		
	case SPDK_BDEV_IO_TYPE_ABORT:
		/* ABORT：取消一个正在进行的I/O */
		/* 对于demo，我们直接返回失败 */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
		
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	default:
		/* 不支持的操作 */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

/* ============================================
 * 第八步：实现I/O类型支持检查函数
 * ============================================
 * 
 * 这个函数告诉SPDK框架，我们的bdev支持哪些I/O类型
 */
static bool
bdev_demo_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
		return true;
	case SPDK_BDEV_IO_TYPE_ABORT:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	default:
		return false;
	}
}

/* ============================================
 * 第九步：实现I/O通道创建函数
 * ============================================
 * 
 * 当线程第一次访问bdev时，会调用这个函数创建I/O通道
 */
static struct spdk_io_channel *
bdev_demo_get_io_channel(void *ctx)
{
	/* 获取或创建I/O通道
	 * 参数ctx就是demo_bdev结构（在bdev_demo_create中注册的）
	 * SPDK框架会自动调用create_channel回调来创建通道
	 */
	return spdk_get_io_channel(ctx);
}

/* ============================================
 * 第十步：实现I/O通道创建回调
 * ============================================
 * 
 * 当创建I/O通道时，SPDK框架会调用这个函数
 */
static int
bdev_demo_create_channel(void *io_device, void *ctx_buf)
{
	struct demo_io_channel *ch = ctx_buf;
	
	/* 初始化I/O队列 */
	TAILQ_INIT(&ch->io_queue);
	
	/* 创建poller来处理I/O
	 * poller函数会在每个事件循环中被调用
	 */
	ch->poller = spdk_poller_register(bdev_demo_poll_io, ch, 0);
	
	return 0;
}

/* ============================================
 * 第十一步：实现I/O通道销毁回调
 * ============================================
 */
static void
bdev_demo_destroy_channel(void *io_device, void *ctx_buf)
{
	struct demo_io_channel *ch = ctx_buf;
	
	/* 取消注册poller */
	spdk_poller_unregister(&ch->poller);
}

/* ============================================
 * 第十二步：实现I/O处理poller函数
 * ============================================
 * 
 * 这个函数会在每个事件循环中被调用
 * 它负责处理队列中的I/O请求
 */
static int
bdev_demo_poll_io(void *arg)
{
	struct demo_io_channel *ch = arg;
	struct demo_bdev_io *demo_io;
	struct spdk_bdev_io *bdev_io;
	
	/* 从队列中取出一个I/O请求 */
	demo_io = TAILQ_FIRST(&ch->io_queue);
	if (demo_io == NULL) {
		/* 队列为空，返回0表示不需要立即再次调用 */
		return 0;
	}
	
	/* 从队列中移除 */
	TAILQ_REMOVE(&ch->io_queue, demo_io, link);
	
	/* 获取spdk_bdev_io结构 */
	bdev_io = spdk_bdev_io_from_ctx(demo_io);
	
	/* 根据I/O类型处理 */
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		/* 对于READ，demo bdev返回零数据
		 * 实际应用中，这里应该从存储设备读取数据
		 */
		/* 注意：如果用户没有提供缓冲区，我们需要分配一个 */
		if (bdev_io->u.bdev.iovs[0].iov_base == NULL) {
			/* 用户没有提供缓冲区，我们需要分配
			 * 但为了简化，我们假设用户总是提供缓冲区
			 */
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			break;
		}
			/* 将缓冲区清零（模拟读取零数据）
		 * 注意：我们需要处理多个iov的情况
		 */
		{
			uint64_t bytes_to_read = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
			uint64_t bytes_read = 0;
			int i;
			
			for (i = 0; i < bdev_io->u.bdev.iovcnt && bytes_read < bytes_to_read; i++) {
				uint64_t to_zero = spdk_min(bytes_to_read - bytes_read,
							     bdev_io->u.bdev.iovs[i].iov_len);
				memset(bdev_io->u.bdev.iovs[i].iov_base, 0, to_zero);
				bytes_read += to_zero;
			}
		}
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		break;
		
	case SPDK_BDEV_IO_TYPE_WRITE:
		/* 对于WRITE，demo bdev直接丢弃数据
		 * 实际应用中，这里应该写入存储设备
		 */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		break;
		
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		/* WRITE_ZEROES：直接成功 */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		break;
		
	case SPDK_BDEV_IO_TYPE_RESET:
		/* RESET：清空所有待处理的I/O */
		/* 对于demo，我们简单地完成这个I/O */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		break;
		
	default:
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
	
	/* 返回1表示需要立即再次调用（如果还有I/O） */
	return 1;
}

/* ============================================
 * 第十三步：定义bdev函数表
 * ============================================
 * 
 * 这个结构定义了bdev的所有操作函数
 */
static const struct spdk_bdev_fn_table demo_fn_table = {
	.destruct = bdev_demo_destruct,
	.submit_request = bdev_demo_submit_request,
	.io_type_supported = bdev_demo_io_type_supported,
	.get_io_channel = bdev_demo_get_io_channel,
};

/* ============================================
 * 第十四步：实现bdev创建函数
 * ============================================
 * 
 * 这个函数创建一个新的demo bdev设备
 */
int
bdev_demo_create(struct spdk_bdev **bdev, const char *name,
		 uint64_t num_blocks, uint32_t block_size)
{
	struct demo_bdev *demo_bdev;
	int rc;
	
	/* 检查参数 */
	if (name == NULL || strlen(name) == 0) {
		SPDK_ERRLOG("bdev name cannot be empty\n");
		return -EINVAL;
	}
	
	if (num_blocks == 0) {
		SPDK_ERRLOG("num_blocks cannot be zero\n");
		return -EINVAL;
	}
	
	if (block_size == 0 || block_size % 512 != 0) {
		SPDK_ERRLOG("block_size must be a multiple of 512\n");
		return -EINVAL;
	}
	
	/* 分配bdev结构 */
	demo_bdev = calloc(1, sizeof(struct demo_bdev));
	if (demo_bdev == NULL) {
		SPDK_ERRLOG("Failed to allocate demo_bdev\n");
		return -ENOMEM;
	}
	
	/* 设置bdev的基本信息 */
	demo_bdev->bdev.name = strdup(name);
	if (demo_bdev->bdev.name == NULL) {
		free(demo_bdev);
		return -ENOMEM;
	}
	
	demo_bdev->bdev.product_name = "Demo Disk";
	demo_bdev->bdev.write_cache = 0;
	demo_bdev->bdev.blocklen = block_size;
	demo_bdev->bdev.blockcnt = num_blocks;
	demo_bdev->bdev.ctxt = demo_bdev;
	demo_bdev->bdev.module = &demo_if;
	demo_bdev->bdev.fn_table = &demo_fn_table;
	
	/* 注册I/O设备
	 * 这样SPDK框架就知道如何创建I/O通道了
	 */
	spdk_io_device_register(demo_bdev, bdev_demo_create_channel,
				 bdev_demo_destroy_channel,
				 sizeof(struct demo_io_channel),
				 "demo_bdev");
	
	/* 注册bdev到SPDK框架 */
	rc = spdk_bdev_register(&demo_bdev->bdev);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to register bdev: %s\n", spdk_strerror(-rc));
		spdk_io_device_unregister(demo_bdev, NULL);
		free(demo_bdev->bdev.name);
		free(demo_bdev);
		return rc;
	}
	
	/* 添加到全局链表 */
	TAILQ_INSERT_TAIL(&g_demo_bdevs, demo_bdev, tailq);
	
	*bdev = &demo_bdev->bdev;
	
	SPDK_NOTICELOG("Created demo bdev: %s (blocks: %lu, block_size: %u)\n",
		       name, num_blocks, block_size);
	
	return 0;
}

/* ============================================
 * 第十五步：实现bdev删除函数
 * ============================================
 */
void
bdev_demo_delete(const char *name, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct demo_bdev *demo_bdev, *tmp;
	
	/* 在全局链表中查找 */
	TAILQ_FOREACH_SAFE(demo_bdev, &g_demo_bdevs, tailq, tmp) {
		if (strcmp(demo_bdev->bdev.name, name) == 0) {
			/* 找到了，开始删除 */
			spdk_bdev_unregister(&demo_bdev->bdev, cb_fn, cb_arg);
			return;
		}
	}
	
	/* 没找到 */
	if (cb_fn) {
		cb_fn(cb_arg, -ENOENT);
	}
}

