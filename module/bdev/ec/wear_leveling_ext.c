/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Intel Corporation.
 *   All rights reserved.
 */

#include "bdev_ec.h"
#include "bdev_ec_internal.h"
#include "wear_leveling_ext.h"
#include "spdk/log.h"
#include "spdk/bdev.h"
#include "spdk/nvme.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/env.h"
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>

#define WEAR_LEVELING_EXT_NAME "wear_leveling"

/* 常量定义：避免重复计算 */
#define GB_TO_BYTES (1024ULL * 1024 * 1024)
#define TB_TO_GB (1024ULL)

/* 预计算权重表：磨损等级 0-100 对应的权重
 * weight = (100 - wear_level) * 10 + 1
 * 这样可以避免每次计算权重，直接查表即可
 */
static const uint32_t g_wear_weight_table[101] = {
	1001, 991, 981, 971, 961, 951, 941, 931, 921, 911,  /* 0-9 */
	901, 891, 881, 871, 861, 851, 841, 831, 821, 811,  /* 10-19 */
	801, 791, 781, 771, 761, 751, 741, 731, 721, 711,  /* 20-29 */
	701, 691, 681, 671, 661, 651, 641, 631, 621, 611,  /* 30-39 */
	601, 591, 581, 571, 561, 551, 541, 531, 521, 511,  /* 40-49 */
	501, 491, 481, 471, 461, 451, 441, 431, 421, 411,  /* 50-59 */
	401, 391, 381, 371, 361, 351, 341, 331, 321, 311,  /* 60-69 */
	301, 291, 281, 271, 261, 251, 241, 231, 221, 211,  /* 70-79 */
	201, 191, 181, 171, 161, 151, 141, 131, 121, 111,  /* 80-89 */
	101, 91, 81, 71, 61, 51, 41, 31, 21, 11, 1        /* 90-100 */
};

/* 磨损差异阈值：如果所有候选的磨损差异小于此值，使用快速路径（直接使用默认选择） */
#define WEAR_DIFF_THRESHOLD 5  /* 5% 的磨损差异阈值 */

/* Base bdev磨损信息（优化内存布局，提高缓存局部性）
 * 
 * 优化说明：
 * 1. 移除冗余的index字段（可通过数组索引计算）
 * 2. 使用位域压缩布尔标志，节省内存
 * 3. 将经常一起访问的字段放在一起（wear_level和predicted_wear_level）
 * 4. 对齐到8字节边界，提高访问效率
 */
struct base_bdev_wear_info {
	/* 磨损等级（热路径，放在前面） */
	uint8_t wear_level;         /* 当前磨损等级 0-100（实际读取的，用于制定策略的基准） */
	uint8_t predicted_wear_level; /* 预测的磨损等级 0-100（仅用于判断是否需要重新读取，不用于选择） */
	
	/* 写入统计（经常一起访问） */
	uint64_t total_writes;      /* 累计写入量（块数） */
	uint64_t writes_since_last_read; /* 上次读取后的写入量 */
	
	/* 时间戳 */
	uint64_t last_read_timestamp;    /* 上次读取实际磨损的时间戳 */
	
	/* 标志位（使用位域节省内存） */
	unsigned int is_operational : 1;  /* 是否可用 */
	unsigned int needs_reread : 1;    /* 是否需要重新读取 */
	unsigned int reserved : 6;        /* 保留位，用于未来扩展 */
} __attribute__((packed));

/* Base bdev配置信息（TBW和磨损率，经常一起访问） */
struct base_bdev_config {
	double tbw;          /* TBW（Total Bytes Written，单位：TB） */
	double wear_per_gb; /* 每GB磨损率 */
} __attribute__((packed));

/* 磨损均衡扩展模块（优化内存布局）
 * 
 * 优化说明：
 * 1. 移除未使用的wear_info_timestamp数组
 * 2. 将TBW和wear_per_gb合并为结构体，提高缓存局部性
 * 3. 将配置参数放在一起，统计信息放在一起
 * 4. 优化字段顺序，减少内存对齐浪费
 */
struct wear_leveling_ext {
	/* 扩展接口（必须放在最前面） */
	struct ec_bdev_extension_if ext_if;
	struct ec_bdev *ec_bdev;
	
	/* 磨损统计信息（热路径数据，放在前面） */
	struct base_bdev_wear_info wear_info[EC_MAX_K + EC_MAX_P];
	
	/* Base bdev配置信息（TBW和磨损率，经常一起访问） */
	struct base_bdev_config bdev_config[EC_MAX_K + EC_MAX_P];
	
	/* 磨损预测参数（配置参数，访问频率较低） */
	uint64_t wear_predict_threshold_blocks;  /* 写入多少块后重新读取（默认10GB） */
	uint8_t wear_predict_threshold_percent;  /* 预测磨损变化超过多少百分比时重新读取（默认5%） */
	uint64_t wear_read_interval_us;          /* 最小读取间隔（微秒，默认30秒） */
	
	/* 磨损均衡参数（已移除策略，只使用基于磨损的权重分配） */
	bool all_wear_unavailable;  /* 如果所有base bdev都无法读取磨损信息，回退到默认选择 */
	
	/* 性能优化：缓存block_size（避免重复查询） */
	uint32_t cached_block_size[EC_MAX_K + EC_MAX_P];
	
	/* 性能统计（访问频率低，放在最后） */
	uint64_t cache_hits;
	uint64_t cache_misses;
	uint64_t fast_path_hits;  /* 快速路径命中次数（磨损差异小，直接使用默认选择） */
};

/* 前向声明：使用弱符号访问 bdev_nvme_get_ctrlr（避免直接依赖 bdev_nvme 模块） */
extern struct spdk_nvme_ctrlr *bdev_nvme_get_ctrlr(struct spdk_bdev *bdev) __attribute__((weak));

/* 同步读取NVMe健康信息的上下文（优化版本：使用volatile标志，避免pthread同步原语） */
struct sync_health_read_ctx {
	struct spdk_nvme_health_information_page *health_page;
	struct spdk_nvme_cpl cpl;
	volatile bool completed;  /* 使用volatile确保可见性，避免pthread同步原语 */
	uint64_t timeout_tsc;     /* 超时时间戳（ticks） */
};

/* 健康信息读取完成回调（优化版本：无锁设计，增强鲁棒性） */
static void
sync_health_read_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct sync_health_read_ctx *ctx = (struct sync_health_read_ctx *)cb_arg;
	
	/* 参数检查（鲁棒性：防止NULL指针） */
	if (ctx == NULL) {
		SPDK_ERRLOG("sync_health_read_cb: ctx is NULL\n");
		return;
	}
	
	/* 原子性：在SPDK的单线程轮询模型中，回调在同一个线程中执行，无需锁 */
	if (cpl != NULL) {
		memcpy(&ctx->cpl, cpl, sizeof(*cpl));
	} else {
		/* cpl 为 NULL（不应该发生，但为了鲁棒性处理）
		 * 设置一个错误状态，表示命令失败
		 */
		memset(&ctx->cpl, 0, sizeof(ctx->cpl));
		ctx->cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		ctx->cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}
	
	ctx->completed = true;  /* volatile确保写入可见性 */
}

/* 验证健康信息页数据的有效性（全面验证，确保数据完整性）
 * 
 * 返回值：
 * 0 - 数据有效
 * -EINVAL - 数据无效
 * 
 * 验证内容：
 * 1. 指针有效性
 * 2. percentage_used 范围（0-100）
 * 3. available_spare 合理性（0-100，警告但不失败）
 * 4. 温度值合理性（可选）
 */
static int
validate_health_page(struct spdk_nvme_health_information_page *health_page, 
		     const char *bdev_name)
{
	const char *name = bdev_name ? bdev_name : "unknown";
	
	/* 检查健康信息页是否为空指针 */
	if (health_page == NULL) {
		SPDK_ERRLOG("Health page is NULL (bdev: %s)\n", name);
		return -EINVAL;
	}
	
	/* 验证 percentage_used 的有效性
	 * 根据NVMe规范：
	 * - 0-100: 正常磨损值（有效）
	 * - 101-253: 保留值，应视为无效
	 * - 254: 未报告（某些设备可能使用）
	 * - 255: 未报告（某些设备可能使用）
	 * 
	 * 为了与磨损分布功能完美契合，我们：
	 * - 0-100: 直接使用
	 * - >100: 视为无效，返回错误（调用者应设置 wear_level = 255）
	 */
	if (health_page->percentage_used > 100) {
		/* 值超出正常范围，可能是设备不支持或数据错误
		 * 记录详细的错误信息，包括实际值
		 */
		SPDK_WARNLOG("Invalid percentage_used value %u (bdev: %s, expected 0-100), treating as unavailable\n",
			     health_page->percentage_used, name);
		return -EINVAL;
	}
	
	/* 验证 available_spare 的合理性（用于检测数据损坏）
	 * available_spare 应该在 0-100 范围内
	 * 注意：这不是关键字段，如果超出范围，只记录警告，不返回错误
	 */
	if (health_page->available_spare > 100) {
		SPDK_WARNLOG("Suspicious available_spare value %u (bdev: %s, expected 0-100), but continuing\n",
			     health_page->available_spare, name);
		/* 不返回错误，因为这不是关键字段，但可能表示数据损坏 */
	}
	
	/* 验证温度值的合理性（可选，用于检测数据损坏）
	 * 根据NVMe规范，温度值 0xFFFF 表示无效
	 * 正常温度范围：-273°C 到 1000°C（0 到 1273 Kelvin）
	 */
	if (health_page->temperature != 0xFFFF) {
		if (health_page->temperature > 1273) {
			/* 温度值异常高，可能是数据损坏 */
			SPDK_WARNLOG("Suspicious temperature value %u Kelvin (bdev: %s), but continuing\n",
				     health_page->temperature, name);
			/* 不返回错误，因为这不是关键字段 */
		}
	}
	
	return 0;
}

/* 从NVMe读取磨损等级（全面优化版本：稳定性、性能、鲁棒性）
 * 
 * 优化点：
 * 1. 移除pthread同步原语，使用volatile标志（适配SPDK轮询模式）
 * 2. 使用SPDK时间函数，避免系统调用
 * 3. 优化轮询逻辑，减少不必要的延迟
 * 4. 使用ticks进行超时检查，更高效
 * 5. 控制器状态检查，确保控制器可用
 * 6. 数据有效性验证，处理各种异常情况
 * 7. 完善的错误处理和资源管理
 * 8. 与磨损分布功能完美契合（返回255表示无效值）
 * 
 * 返回值：
 * 0 - 成功，wear_level 包含有效值（0-100）
 * -EINVAL - 参数错误
 * -ENODEV - 设备不可用或无法获取控制器
 * -ETIMEDOUT - 超时
 * -EIO - 命令执行失败或数据无效
 * -ENOMEM - 内存分配失败
 * 
 * 注意：
 * - 如果返回错误，wear_level 不会被修改
 * - 如果数据无效（percentage_used > 100），返回 -EIO，wear_level 不会被修改
 * - 这个函数可能很慢（需要NVMe命令），应该被缓存
 * - 实现参考：xnvme_be_spdk_admin.c 和 lib/nvme/nvme.c
 */
static int
_get_nvme_ctrlr_for_health_check(struct spdk_bdev *bdev, struct spdk_nvme_ctrlr **ctrlr_out)
{
	const char *module_name;
	struct spdk_nvme_ctrlr *ctrlr;

	/* Check if it's an NVMe bdev */
	module_name = spdk_bdev_get_module_name(bdev);
	if (module_name == NULL || strcmp(module_name, "nvme") != 0) {
		SPDK_DEBUGLOG(bdev_ec, "Bdev %s is not an NVMe device\n", spdk_bdev_get_name(bdev));
		return -ENODEV;
	}

	/* Check if the bdev_nvme_get_ctrlr weak symbol is available */
	if (bdev_nvme_get_ctrlr == NULL) {
		SPDK_DEBUGLOG(bdev_ec, "bdev_nvme_get_ctrlr not available (bdev: %s)\n", spdk_bdev_get_name(bdev));
		return -ENODEV;
	}

	/* Get the NVMe controller */
	ctrlr = bdev_nvme_get_ctrlr(bdev);
	if (ctrlr == NULL) {
		SPDK_DEBUGLOG(bdev_ec, "Cannot get NVMe controller (bdev: %s)\n", spdk_bdev_get_name(bdev));
		return -ENODEV;
	}

	/* Check if the controller is in a failed state */
	if (spdk_nvme_ctrlr_is_failed(ctrlr)) {
		SPDK_WARNLOG("NVMe controller is in failed state (bdev: %s)\n", spdk_bdev_get_name(bdev));
		return -ENODEV;
	}

	*ctrlr_out = ctrlr;
	return 0;
}

/*
 * Helper to poll for health info command completion.
 * This encapsulates the busy-wait loop.
 */
static int
_poll_for_health_info_completion(struct spdk_nvme_ctrlr *ctrlr, struct sync_health_read_ctx *ctx,
				 uint64_t timeout_us, const char *bdev_name)
{
	uint64_t poll_count = 0;
	uint64_t ticks_per_usec = spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;
	uint64_t poll_interval_tsc = (ticks_per_usec > 0) ? (10 * ticks_per_usec) : 1;
	uint32_t max_poll_iterations = (timeout_us >= 10) ? (timeout_us / 10) * 2 : 100000;

	if (max_poll_iterations > 1000000) {
		max_poll_iterations = 1000000;
	}
	if (max_poll_iterations == 0) {
		max_poll_iterations = 100000;
	}

	while (!ctx->completed) {
		if (poll_count++ >= max_poll_iterations) {
			SPDK_ERRLOG("Max poll iterations reached (bdev: %s, count: %lu), aborting\n",
				    bdev_name, poll_count);
			return -ETIMEDOUT;
		}

		if (spdk_get_ticks() >= ctx->timeout_tsc) {
			SPDK_ERRLOG("Timeout waiting for health log page (bdev: %s, timeout: %lu us, poll_count: %lu)\n",
				    bdev_name, timeout_us, poll_count);
			return -ETIMEDOUT;
		}

		if (spdk_nvme_ctrlr_is_failed(ctrlr)) {
			SPDK_WARNLOG("Controller failed during health log page read (bdev: %s, poll_count: %lu)\n",
				     bdev_name, poll_count);
			return -ENODEV;
		}

		spdk_nvme_ctrlr_process_admin_completions(ctrlr);

		if (!ctx->completed) {
			uint64_t next_poll_tsc = spdk_get_ticks() + poll_interval_tsc;
			while (spdk_get_ticks() < next_poll_tsc && !ctx->completed) {
				spdk_pause();
			}
		}
	}

	return 0;
}

/*
 * Helper to process the result of the health info command.
 * This validates the result and extracts the wear level.
 */
static int
_process_health_info_result(struct sync_health_read_ctx *ctx, const char *bdev_name,
			    uint8_t *wear_level)
{
	int rc;

	if (spdk_nvme_cpl_is_error(&ctx->cpl)) {
		SPDK_ERRLOG("Failed to get health log page (bdev: %s): %s (sct: %u, sc: %u)\n",
			    bdev_name, spdk_nvme_cpl_get_status_string(&ctx->cpl.status),
			    ctx->cpl.status.sct, ctx->cpl.status.sc);
		return -EIO;
	}

	rc = validate_health_page(ctx->health_page, bdev_name);
	if (rc != 0) {
		SPDK_ERRLOG("Health page validation failed (bdev: %s)\n", bdev_name);
		return -EIO;
	}

	/* Double check for robustness */
	if (ctx->health_page->percentage_used > 100) {
		SPDK_ERRLOG("Unexpected invalid percentage_used %u after validation (bdev: %s)\n",
			    ctx->health_page->percentage_used, bdev_name);
		return -EIO;
	}

	*wear_level = ctx->health_page->percentage_used;
	return 0;
}

/* 从NVMe读取磨损等级（全面优化版本：稳定性、性能、鲁棒性）
 *
 * 优化点：
 * 1. 移除pthread同步原语，使用volatile标志（适配SPDK轮询模式）
 * 2. 使用SPDK时间函数，避免系统调用
 * 3. 优化轮询逻辑，减少不必要的延迟
 * 4. 使用ticks进行超时检查，更高效
 * 5. 控制器状态检查，确保控制器可用
 * 6. 数据有效性验证，处理各种异常情况
 * 7. 完善的错误处理和资源管理
 * 8. 与磨损分布功能完美契合（返回255表示无效值）
 * 
 * 返回值：
 * 0 - 成功，wear_level 包含有效值（0-100）
 * -EINVAL - 参数错误
 * -ENODEV - 设备不可用或无法获取控制器
 * -ETIMEDOUT - 超时
 * -EIO - 命令执行失败或数据无效
 * -ENOMEM - 内存分配失败
 * 
 * 注意：
 * - 如果返回错误，wear_level 不会被修改
 * - 如果数据无效（percentage_used > 100），返回 -EIO，wear_level 不会被修改
 * - 这个函数可能很慢（需要NVMe命令），应该被缓存
 * - 实现参考：xnvme_be_spdk_admin.c 和 lib/nvme/nvme.c
 */
static int
get_nvme_wear_level_from_bdev(struct spdk_bdev *bdev, uint8_t *wear_level)
{
	struct spdk_nvme_ctrlr *ctrlr = NULL;
	struct spdk_nvme_health_information_page *health_page = NULL;
	struct sync_health_read_ctx ctx;
	int rc;
	uint64_t timeout_us = 1000000; /* 1秒超时 */
	const char *bdev_name;

	if (bdev == NULL || wear_level == NULL) {
		return -EINVAL;
	}

	bdev_name = spdk_bdev_get_name(bdev);

	rc = _get_nvme_ctrlr_for_health_check(bdev, &ctrlr);
	if (rc != 0) {
		return rc;
	}

	health_page = spdk_zmalloc(sizeof(*health_page), 0, NULL,
				   SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (health_page == NULL) {
		SPDK_ERRLOG("Failed to allocate health page buffer (bdev: %s)\n", bdev_name);
		return -ENOMEM;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.health_page = health_page;
	ctx.completed = false;

	uint64_t ticks_per_usec = spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;
	if (ticks_per_usec == 0) {
		ticks_per_usec = 1;
	}
	ctx.timeout_tsc = spdk_get_ticks() + timeout_us * ticks_per_usec;

	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctrlr,
					      SPDK_NVME_LOG_HEALTH_INFORMATION,
					      SPDK_NVME_GLOBAL_NS_TAG,
					      health_page,
					      sizeof(*health_page),
					      0,
					      sync_health_read_cb,
					      &ctx);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to send get log page command (bdev: %s): %s\n",
			    bdev_name, spdk_strerror(-rc));
		spdk_free(health_page);
		return rc;
	}

	rc = _poll_for_health_info_completion(ctrlr, &ctx, timeout_us, bdev_name);
	if (rc != 0) {
		spdk_free(health_page);
		return rc;
	}

	rc = _process_health_info_result(&ctx, bdev_name, wear_level);

	spdk_free(health_page);
	return rc;
}

/* 获取并缓存block size（避免重复查询）
 * 
 * 优化说明：
 * - block_size 通常是固定的（512B, 4KB等），很少变化
 * - 使用实例级缓存，避免多实例冲突
 * - 在初始化时一次性缓存所有base bdev的block size
 */
static uint32_t
get_cached_block_size(struct wear_leveling_ext *wl_ext, uint8_t idx)
{
	if (idx >= EC_MAX_K + EC_MAX_P || wl_ext == NULL) {
		return 512;
	}
	
	/* 如果已缓存，直接返回 */
	if (wl_ext->cached_block_size[idx] != 0) {
		return wl_ext->cached_block_size[idx];
	}
	
	/* 从bdev获取block size */
	if (wl_ext->ec_bdev != NULL && idx < wl_ext->ec_bdev->num_base_bdevs) {
		struct ec_base_bdev_info *base_info = &wl_ext->ec_bdev->base_bdev_info[idx];
		if (base_info->desc != NULL) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(base_info->desc);
			if (bdev != NULL) {
				uint32_t block_size = spdk_bdev_get_block_size(bdev);
				if (block_size != 0) {
					wl_ext->cached_block_size[idx] = block_size;
					return block_size;
				}
			}
		}
	}
	
	/* 默认512字节 */
	wl_ext->cached_block_size[idx] = 512;
	return 512;
}

/* 预测磨损等级变化（仅用于判断是否需要重新读取，不用于选择策略）
 * 基于写入量预测磨损增加
 * 
 * 注意：这个函数只更新 predicted_wear_level，用于判断是否需要重新读取实际磨损
 * 选择 base bdev 时使用的是 wear_level（基准磨损），而不是 predicted_wear_level
 * 
 * 磨损计算公式：
 * 磨损增加(%) = (写入GB数 / (TBW * 1024)) * 100
 * 
 * 例如：
 * - TBW = 180TB = 180 * 1024 GB = 184,320 GB
 * - 写入100GB，磨损增加 = (100 / 184320) * 100 = 0.0543%
 * - 写入1GB，磨损增加 = (1 / 184320) * 100 = 0.000543%
 */
static void
predict_wear_level_for_reread_check(struct wear_leveling_ext *wl_ext, uint8_t idx)
{
	if (idx >= EC_MAX_K + EC_MAX_P) {
		return;
	}
	
	struct base_bdev_wear_info *info = &wl_ext->wear_info[idx];
	
	/* 如果基准磨损是无效值（255），不进行预测计算 */
	if (info->wear_level == 255) {
		/* 保持 predicted_wear_level 为 255（无效值） */
		return;
	}
	
	uint64_t blocks_since_read = info->writes_since_last_read;
	uint32_t block_size;
	uint64_t gb_written;
	double wear_increase;
	
	/* 获取block size（使用缓存） */
	block_size = get_cached_block_size(wl_ext, idx);
	
	/* 计算自上次读取后写入的GB数
	 * 注意：检查乘法溢出，避免 blocks_since_read * block_size 溢出
	 */
	if (blocks_since_read > 0 && block_size > UINT64_MAX / blocks_since_read) {
		/* 溢出风险：使用饱和值 */
		gb_written = UINT64_MAX / GB_TO_BYTES;
	} else {
		gb_written = (blocks_since_read * block_size) / GB_TO_BYTES;
	}
	
	/* 预测磨损增加：根据该bdev的TBW计算
	 * wear_per_gb = 100 / (TBW * 1024)  // 100%磨损 / (TBW转换为GB)
	 * 检查 wear_per_gb 是否有效（防止异常值）
	 */
	if (wl_ext->bdev_config[idx].wear_per_gb > 0.0 && 
	    wl_ext->bdev_config[idx].wear_per_gb <= 100.0) {
		wear_increase = gb_written * wl_ext->bdev_config[idx].wear_per_gb;
		/* 检查结果是否合理（防止异常值） */
		if (wear_increase < 0.0 || wear_increase > 100.0) {
			/* 异常值，使用默认值 */
			double default_tbw = 180.0;
			double default_tbw_gb = default_tbw * TB_TO_GB;
			if (default_tbw_gb > 0.0) {
				wear_increase = (gb_written / default_tbw_gb) * 100.0;
			} else {
				wear_increase = 0.0;
			}
		}
	} else {
		/* 如果未设置TBW或值无效，使用默认值（假设180TB的TLC SSD） */
		double default_tbw = 180.0;  /* 默认180TB */
		double default_tbw_gb = default_tbw * TB_TO_GB;
		/* 防止除零（虽然 default_tbw > 0，但为了鲁棒性） */
		if (default_tbw_gb > 0.0) {
			wear_increase = (gb_written / default_tbw_gb) * 100.0;
		} else {
			/* 不应该发生，但为了鲁棒性 */
			wear_increase = 0.0;
		}
	}
	
	/* 更新预测磨损等级（仅用于判断是否需要重新读取）
	 * 注意：选择 base bdev 时使用的是 wear_level（基准磨损），不是 predicted_wear_level
	 */
	double predicted = (double)info->wear_level + wear_increase;
	if (predicted > 100.0) {
		info->predicted_wear_level = 100;
	} else if (predicted < 0.0) {
		info->predicted_wear_level = 0;
	} else {
		/* 四舍五入，避免截断损失 */
		info->predicted_wear_level = (uint8_t)(predicted + 0.5);
	}
}

/* 检查是否需要重新读取实际磨损信息 */
static bool
should_reread_wear_level(struct wear_leveling_ext *wl_ext, uint8_t idx)
{
	if (idx >= EC_MAX_K + EC_MAX_P) {
		return true;
	}
	
	struct base_bdev_wear_info *info = &wl_ext->wear_info[idx];
	
	if (info->last_read_timestamp == 0) {
		/* 从未读取过，需要读取 */
		return true;
	}
	
	/* 检查最小读取间隔（优化：避免除零和溢出） */
	if (wl_ext->wear_read_interval_us == 0) {
		/* 如果间隔为0，总是需要读取（用于调试） */
		return true;
	}
	
	uint64_t now = spdk_get_ticks();
	uint64_t ticks_hz = spdk_get_ticks_hz();
	
	/* 避免除零和溢出：先检查时间差是否足够大 */
	if (now <= info->last_read_timestamp) {
		/* 时间戳异常，需要重新读取 */
		return true;
	}
	
	uint64_t ticks_diff = now - info->last_read_timestamp;
	
	/* 优化：使用更精确的时间计算，避免溢出
	 * 参考SPDK的env_ticks_to_usecs实现：
	 * time_us = ticks_diff / (ticks_hz / 1000000)
	 * 这样可以避免乘法溢出
	 */
	uint64_t ticks_per_usec = ticks_hz / SPDK_SEC_TO_USEC;
	if (ticks_per_usec == 0) {
		/* ticks_hz太小，使用原始方法但检查溢出 */
		if (ticks_diff > UINT64_MAX / SPDK_SEC_TO_USEC) {
			/* 溢出风险，直接返回true */
			return true;
		}
		uint64_t time_since_read_us = (ticks_diff * SPDK_SEC_TO_USEC) / ticks_hz;
		if (time_since_read_us < wl_ext->wear_read_interval_us) {
			return false;
		}
	} else {
		uint64_t time_since_read_us = ticks_diff / ticks_per_usec;
		if (time_since_read_us < wl_ext->wear_read_interval_us) {
			return false;
		}
	}
	
	/* 检查写入量阈值 */
	if (info->writes_since_last_read >= wl_ext->wear_predict_threshold_blocks) {
		return true;
	}
	
	/* 检查预测磨损变化阈值
	 * 注意：predicted_wear_level 和 wear_level 都是 uint8_t (0-100)
	 * 如果 predicted_wear_level > wear_level，差值不会溢出
	 */
	if (info->predicted_wear_level > info->wear_level) {
		uint8_t wear_delta = info->predicted_wear_level - info->wear_level;
		/* 检查是否超过阈值 */
		if (wear_delta >= wl_ext->wear_predict_threshold_percent) {
			return true;
		}
	}
	
	return false;
}

/* 获取磨损等级（使用预测机制） */
static int
get_wear_level_with_prediction(struct wear_leveling_ext *wl_ext,
			       uint8_t idx,
			       struct ec_base_bdev_info *base_info,
			       uint8_t *wear_level)
{
	if (idx >= EC_MAX_K + EC_MAX_P) {
		return -EINVAL;
	}
	
	if (base_info == NULL || base_info->desc == NULL) {
		return -ENODEV;
	}
	
	struct base_bdev_wear_info *info = &wl_ext->wear_info[idx];
	
	/* 检查是否需要重新读取实际磨损
	 * 注意：如果设置了 needs_reread 标志，优先重新读取
	 */
	if (info->needs_reread || should_reread_wear_level(wl_ext, idx)) {
		/* 需要重新读取 */
		struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(base_info->desc);
		if (bdev == NULL) {
			return -ENODEV;
		}
		
		uint8_t actual_wear;
		int rc = get_nvme_wear_level_from_bdev(bdev, &actual_wear);
		if (rc == 0) {
			/* 更新基准磨损等级（用于制定写入策略）
			 * 这是关键：更新 wear_level 会改变后续的写入策略
			 * 
			 * 注意：如果实际磨损小于基准磨损（理论上不应该发生，但可能由于设备重置等），
			 * 仍然使用实际值，因为这是设备的真实状态
			 */
			info->wear_level = actual_wear;
			info->predicted_wear_level = actual_wear;  /* 同步更新预测值 */
			info->writes_since_last_read = 0;
			info->last_read_timestamp = spdk_get_ticks();
			info->needs_reread = false; /* 清除重新读取标志 */
			wl_ext->cache_misses++;
		} else {
			/* 读取失败，处理各种错误场景（鲁棒性）
			 * 
			 * 错误场景包括：
			 * - ENODEV: 设备不可用或控制器失败
			 * - ETIMEDOUT: 超时（可能是临时故障）
			 * - EIO: 命令失败或数据无效
			 * - ENOMEM: 内存分配失败（罕见，但需要处理）
			 * 
			 * 策略：
			 * - 如果当前 wear_level 是有效值（0-100），保持使用（避免频繁失败导致策略不稳定）
			 * - 如果当前 wear_level 是无效值（255），保持255（表示无法获取磨损信息）
			 * - 保持 needs_reread 标志，以便后续重试
			 */
			if (info->wear_level == 255) {
				/* 当前已经是无效值，保持255 */
				SPDK_DEBUGLOG(bdev_ec, "Failed to read wear level (rc: %d), keeping invalid value 255\n", rc);
			} else {
				/* 当前是有效值，保持使用（避免频繁失败导致策略不稳定）
				 * 但记录警告，以便监控
				 */
				SPDK_WARNLOG("Failed to re-read wear level (rc: %d), using cached value %u\n", 
					     rc, info->wear_level);
			}
			wl_ext->cache_misses++;
			/* 保持 needs_reread 标志，以便后续重试 */
		}
	} else {
		/* 不需要重新读取，使用当前的基准磨损（wear_level）
		 * 注意：不更新 predicted_wear_level，因为选择策略使用的是 wear_level
		 * predicted_wear_level 只在 should_reread_wear_level 中用于判断是否需要重新读取
		 */
		wl_ext->cache_hits++;
		
		/* 更新预测值（仅用于判断是否需要重新读取，不影响选择策略） */
		predict_wear_level_for_reread_check(wl_ext, idx);
	}
	
	/* 返回基准磨损等级（用于制定写入策略）
	 * 关键：使用 wear_level 而不是 predicted_wear_level
	 * 这样在写入过程中，策略保持不变（基于基准磨损）
	 */
	*wear_level = info->wear_level;
	return 0;
}

/* 获取base bdev的磨损等级（扩展接口回调）
 * 
 * 注意：这个函数是扩展接口的一部分，用于外部查询磨损等级
 * 如果读取失败，返回错误，wear_level 不会被修改
 * 调用者应该检查返回值，如果失败，应该将 wear_level 视为无效值（255）
 */
static int
wear_leveling_get_wear_level(struct ec_bdev_extension_if *ext_if,
			      struct ec_bdev *ec_bdev,
			      struct ec_base_bdev_info *base_info,
			      uint8_t *wear_level,
			      void *ctx)
{
	struct spdk_bdev *bdev;
	int rc;
	
	/* 参数检查 */
	if (base_info == NULL || wear_level == NULL) {
		return -EINVAL;
	}
	
	if (base_info->desc == NULL) {
		return -ENODEV;
	}
	
	bdev = spdk_bdev_desc_get_bdev(base_info->desc);
	if (bdev == NULL) {
		return -ENODEV;
	}
	
	/* 从NVMe读取磨损信息
	 * 注意：如果读取失败，wear_level 不会被修改
	 * 调用者应该检查返回值，如果失败，应该将 wear_level 视为无效值（255）
	 */
	rc = get_nvme_wear_level_from_bdev(bdev, wear_level);
	if (rc != 0) {
		/* 读取失败，返回错误
		 * 注意：wear_level 不会被修改，调用者应该将其视为无效值（255）
		 */
		return rc;
	}
	
	/* 双重验证：确保返回的值是有效的（鲁棒性） */
	if (*wear_level > 100) {
		/* 值无效（不应该发生，因为函数内部已验证）
		 * 但为了鲁棒性，仍然处理这种情况
		 */
		SPDK_ERRLOG("Invalid wear level %u returned (expected 0-100), treating as unavailable\n",
			    *wear_level);
		return -EIO;
	}
	
	return 0;
}

/* 验证索引是否在有效范围内 */
static inline bool
is_valid_bdev_idx(struct ec_bdev *ec_bdev, uint8_t idx)
{
	return idx < ec_bdev->num_base_bdevs && idx < EC_MAX_K + EC_MAX_P;
}

/* 复制默认选择到输出数组（带边界检查） */
static int
copy_default_selection(struct ec_bdev *ec_bdev,
		       const uint8_t *default_data, uint8_t k,
		       const uint8_t *default_parity, uint8_t p,
		       uint8_t *data_indices, uint8_t *parity_indices,
		       const char *context)
{
	uint8_t i;
	
	for (i = 0; i < k; i++) {
		uint8_t idx = default_data[i];
		if (!is_valid_bdev_idx(ec_bdev, idx)) {
			SPDK_ERRLOG("Invalid data index %u (%s)\n", idx, context);
			return -ENODEV;
		}
		data_indices[i] = idx;
	}
	
	for (i = 0; i < p; i++) {
		uint8_t idx = default_parity[i];
		if (!is_valid_bdev_idx(ec_bdev, idx)) {
			SPDK_ERRLOG("Invalid parity index %u (%s)\n", idx, context);
			return -ENODEV;
		}
		parity_indices[i] = idx;
	}
	
	return 0;
}

/* 快速获取基准磨损等级（内联优化版本） */
static inline uint8_t
get_wear_level_fast(struct wear_leveling_ext *wl_ext, uint8_t idx)
{
	if (idx >= EC_MAX_K + EC_MAX_P) {
		return 255;
	}
	return wl_ext->wear_info[idx].wear_level;
}

/* Helper struct for collecting wear info from candidates */
struct wear_collection_result {
	uint32_t weights[EC_MAX_K];
	uint8_t cached_wear[EC_MAX_K];
	uint32_t total_weight;
	uint8_t min_wear;
	uint8_t max_wear;
	bool any_needs_reread;
	uint8_t valid_wear_count;
};

/*
 * Collect and validate wear information for candidate data bdevs.
 * Returns 0 on success, or a negative errno on failure.
 *
 * On failure, caller is expected to fall back to default selection.
 */
static int
_collect_and_validate_wear_info(struct wear_leveling_ext *wl_ext,
				struct ec_bdev *ec_bdev,
				const uint8_t *candidates, uint8_t k,
				struct wear_collection_result *result)
{
	uint8_t i;

	result->min_wear = 255;
	result->max_wear = 0;
	result->any_needs_reread = false;
	result->valid_wear_count = 0;
	result->total_weight = 0;

	for (i = 0; i < k; i++) {
		uint8_t candidate_idx = candidates[i];
		uint8_t wear;

		if (!is_valid_bdev_idx(ec_bdev, candidate_idx)) {
			/* Invalid index, caller will fall back to default selection. */
			SPDK_WARNLOG("Invalid candidate index %u detected\n", candidate_idx);
			return -ENODEV;
		}

		struct ec_base_bdev_info *base_info = &ec_bdev->base_bdev_info[candidate_idx];
		bool is_operational = (base_info->desc != NULL && !base_info->is_failed);

		wl_ext->wear_info[candidate_idx].is_operational = is_operational;

		if (!is_operational) {
			/* Non-operational bdev, caller will fall back to default selection. */
			SPDK_WARNLOG("Non-operational base bdev %u detected\n", candidate_idx);
			return -ENODEV;
		}

		/* Get wear level: if needs_reread is set, try to refresh first. */
		if (wl_ext->wear_info[candidate_idx].needs_reread) {
			uint8_t wear_level;
			int rc = get_wear_level_with_prediction(wl_ext, candidate_idx, base_info, &wear_level);
			if (rc == 0) {
				wear = wear_level;
			} else {
				/* Fall back to cached wear if refresh failed. */
				wear = get_wear_level_fast(wl_ext, candidate_idx);
			}
			result->any_needs_reread = true;
		} else {
			/* Use cached wear level. */
			wear = get_wear_level_fast(wl_ext, candidate_idx);
		}

		result->cached_wear[i] = wear;

		if (wear == 255 || wear > 100) {
			/* Wear info unavailable or invalid for this bdev. */
			SPDK_WARNLOG("Cannot get valid wear level from base bdev %u (wear=%u)\n",
				     candidate_idx, wear);
			return -EIO;
		}

		result->valid_wear_count++;
		if (wear < result->min_wear) {
			result->min_wear = wear;
		}
		if (wear > result->max_wear) {
			result->max_wear = wear;
		}

		result->weights[i] = g_wear_weight_table[wear];
		if (result->total_weight <= UINT32_MAX - result->weights[i]) {
			result->total_weight += result->weights[i];
		} else {
			result->total_weight = UINT32_MAX;
			SPDK_WARNLOG("total_weight overflow detected\n");
		}
	}

	if (result->valid_wear_count == 0) {
		SPDK_WARNLOG("Cannot get wear level from any candidate, no valid wear info\n");
		return -EIO;
	}

	if (result->total_weight == 0) {
		SPDK_ERRLOG("All candidate base bdevs have zero weight\n");
		return -ENODEV;
	}

	return 0;
}

/*
 * Perform deterministic weighted random selection of k data bdevs.
 * The selection is based on the provided weights and cached wear levels.
 */
static int
_perform_deterministic_weighted_selection(struct ec_bdev *ec_bdev,
					  const uint8_t *candidates, uint8_t k,
					  const struct wear_collection_result *wear_info,
					  uint64_t stripe_index, uint8_t *data_indices)
{
	bool selected[EC_MAX_K] = {false};
	uint8_t selected_count = 0;
	uint32_t total_weight = wear_info->total_weight;
	uint64_t seed64;
	uint32_t seed;
	uint8_t i, j;

	if (k == 0 || k > EC_MAX_K) {
		SPDK_ERRLOG("Invalid k value: %u (expected 1-%u)\n", k, EC_MAX_K);
		return -EINVAL;
	}

	if (total_weight == 0) {
		SPDK_ERRLOG("Total weight is zero, cannot perform weighted selection\n");
		return -ENODEV;
	}

	/* Use stripe_index as deterministic seed. */
	seed64 = stripe_index * 0x9e3779b97f4a7c15ULL;
	seed64 = (seed64 >> 32) ^ seed64;
	seed = (uint32_t)seed64;

	for (i = 0; i < k; i++) {
		/* Linear congruential generator (LCG) for deterministic pseudo-randomness. */
		seed = seed * 1664525UL + 1013904223UL;

		uint32_t random = (total_weight > 0) ?
				  (((total_weight & (total_weight - 1)) == 0) ?
				   (seed & (total_weight - 1)) : (seed % total_weight)) : 0;
		uint32_t cumulative = 0;
		bool found = false;

		for (j = 0; j < k; j++) {
			if (selected[j] || wear_info->weights[j] == 0) {
				continue;
			}

			uint32_t new_cumulative = cumulative + wear_info->weights[j];
			if (new_cumulative < cumulative) {
				/* Overflow detected, break for safety. */
				break;
			}
			cumulative = new_cumulative;

			if (random < cumulative) {
				if (selected_count >= k) {
					SPDK_ERRLOG("selected_count overflow: %u >= k=%u\n",
						    selected_count, k);
					return -ENODEV;
				}

				uint8_t candidate_idx = candidates[j];
				if (!is_valid_bdev_idx(ec_bdev, candidate_idx)) {
					SPDK_ERRLOG("Invalid candidate_idx %u\n", candidate_idx);
					return -ENODEV;
				}

				data_indices[selected_count++] = candidate_idx;
				selected[j] = true;
				total_weight = (total_weight >= wear_info->weights[j]) ?
					       total_weight - wear_info->weights[j] : 0;
				found = true;
				break;
			}
		}

		if (!found) {
			/* Fallback: select the unselected candidate with minimum wear (excluding 255). */
			uint8_t min_wear_idx = 0;
			uint8_t min_wear_val = 255;
			bool fallback_found = false;

			for (j = 0; j < k; j++) {
				if (selected[j] || wear_info->weights[j] == 0) {
					continue;
				}
				uint8_t wear = wear_info->cached_wear[j];
				if (wear < min_wear_val && wear != 255) {
					min_wear_val = wear;
					min_wear_idx = j;
					fallback_found = true;
				}
			}

			if (!fallback_found) {
				SPDK_ERRLOG("Failed to select base bdev: no available candidates in fallback\n");
				return -ENODEV;
			}

			if (selected_count >= k) {
				SPDK_ERRLOG("selected_count overflow in fallback: %u >= k=%u\n",
					    selected_count, k);
				return -ENODEV;
			}

			uint8_t candidate_idx = candidates[min_wear_idx];
			if (!is_valid_bdev_idx(ec_bdev, candidate_idx)) {
				SPDK_ERRLOG("Invalid candidate_idx %u in fallback\n", candidate_idx);
				return -ENODEV;
			}

			data_indices[selected_count++] = candidate_idx;
			selected[min_wear_idx] = true;
			total_weight = (total_weight >= wear_info->weights[min_wear_idx]) ?
				       total_weight - wear_info->weights[min_wear_idx] : 0;
		}
	}

	if (selected_count != k) {
		SPDK_ERRLOG("Failed to select %u data bdevs, only selected %u\n",
			    k, selected_count);
		return -ENODEV;
	}

	return 0;
}

/* 基于磨损程度的权重分配算法（确定性版本）
 * 
 * 算法原理：
 * 1. 先使用默认算法确定候选base bdev（确定性的，基于stripe_index）
 * 2. 根据磨损程度计算权重：磨损越低，权重越高
 *    - 磨损为 0 时，权重最大
 *    - 磨损为 100 时，权重最小（但不为0，确保仍有机会被选中）
 *    - 磨损相同时，权重相同
 * 3. 使用确定性加权选择（基于stripe_index），确保同一stripe总是得到相同结果
 * 4. 这样磨损低的bdev会被分配更多写入，磨损高的bdev写入较少
 *    直到磨损程度趋于一致
 * 
 * 权重计算公式：
 * weight = (100 - wear_level) * 10 + 1
 * - wear_level = 0 时，weight = 1001
 * - wear_level = 50 时，weight = 501
 * - wear_level = 100 时，weight = 1
 */
static int
select_wear_based_strategy(struct wear_leveling_ext *wl_ext,
			   struct ec_bdev *ec_bdev,
			   uint64_t stripe_index,
			   uint8_t *data_indices,
			   uint8_t *parity_indices)
{
	uint8_t default_data_indices[EC_MAX_K];
	uint8_t default_parity_indices[EC_MAX_P];
	struct wear_collection_result wear_info;
	int rc;
	uint8_t k = ec_bdev->k;
	uint8_t p = ec_bdev->p;

	/* Basic sanity checks on k and p. */
	if (k == 0 || k > EC_MAX_K) {
		SPDK_ERRLOG("Invalid k value: %u (expected 1-%u)\n", k, EC_MAX_K);
		return -EINVAL;
	}
	if (p == 0 || p > EC_MAX_P) {
		SPDK_ERRLOG("Invalid p value: %u (expected 1-%u)\n", p, EC_MAX_P);
		return -EINVAL;
	}

	/* 1. Get default selection as a candidate set. */
	rc = ec_select_base_bdevs_default(ec_bdev, stripe_index,
					   default_data_indices, default_parity_indices);
	if (rc != 0) {
		return rc;
	}

	/* 2. Collect wear info and validate candidates. */
	rc = _collect_and_validate_wear_info(wl_ext, ec_bdev, default_data_indices, k, &wear_info);
	if (rc != 0) {
		/* Fallback to default if any candidate is invalid or wear is unavailable. */
		wl_ext->all_wear_unavailable = true;
		return copy_default_selection(ec_bdev, default_data_indices, k,
					      default_parity_indices, p,
					      data_indices, parity_indices,
					      "wear info collection failed");
	}

	/* 3. Fast path: If wear difference is small, use default selection. */
	if ((wear_info.max_wear - wear_info.min_wear) < WEAR_DIFF_THRESHOLD &&
	    !wear_info.any_needs_reread) {
		wl_ext->fast_path_hits++;
		return copy_default_selection(ec_bdev, default_data_indices, k,
					      default_parity_indices, p,
					      data_indices, parity_indices,
					      "fast path");
	}

	/* 4. Perform deterministic weighted selection for data indices. */
	rc = _perform_deterministic_weighted_selection(ec_bdev, default_data_indices, k, &wear_info,
						       stripe_index, data_indices);
	if (rc != 0) {
		return rc;
	}

	/* 5. Parity indices always use the default selection. */
	for (uint8_t i = 0; i < p; i++) {
		uint8_t idx = default_parity_indices[i];
		if (!is_valid_bdev_idx(ec_bdev, idx)) {
			SPDK_ERRLOG("Invalid parity index %u\n", idx);
			return -ENODEV;
		}
		parity_indices[i] = idx;
	}

	return 0;
}

/* 选择base bdev - 根据磨损情况（确定性算法）
 * 
 * 关键：使用stripe_index确保写入和读取时选择一致
 * 这样读取时也能用同样的算法计算出数据位置
 */
static int
wear_leveling_select_base_bdevs(struct ec_bdev_extension_if *ext_if,
				 struct ec_bdev *ec_bdev,
				 uint64_t offset_blocks,
				 uint32_t num_blocks,
				 uint8_t *data_indices,
				 uint8_t *parity_indices,
				 void *ctx)
{
	struct wear_leveling_ext *wl_ext = 
		SPDK_CONTAINEROF(ext_if, struct wear_leveling_ext, ext_if);
	
	uint32_t strip_size_shift = ec_bdev->strip_size_shift;
	uint8_t k = ec_bdev->k;
	
	if (k == 0 || k > EC_MAX_K) {
		SPDK_ERRLOG("Invalid k value: %u (expected 1-%u)\n", k, EC_MAX_K);
		return -EINVAL;
	}
	
	if (strip_size_shift > 63) {
		SPDK_ERRLOG("Invalid strip_size_shift: %u (too large)\n", strip_size_shift);
		return -EINVAL;
	}
	
	uint64_t start_strip = offset_blocks >> strip_size_shift;
	uint64_t stripe_index = start_strip / k;
	
	if (wl_ext->all_wear_unavailable) {
		return ec_select_base_bdevs_default(ec_bdev, stripe_index, data_indices, parity_indices);
	}
	
	return select_wear_based_strategy(wl_ext, ec_bdev, stripe_index, data_indices, parity_indices);
}

/* I/O完成通知 - 更新磨损统计 */
static void
wear_leveling_notify_io_complete(struct ec_bdev_extension_if *ext_if,
				  struct ec_bdev *ec_bdev,
				  struct ec_base_bdev_info *base_info,
				  uint64_t offset_blocks,
				  uint32_t num_blocks,
				  bool is_write,
				  void *ctx)
{
	struct wear_leveling_ext *wl_ext = 
		SPDK_CONTAINEROF(ext_if, struct wear_leveling_ext, ext_if);
	
	if (is_write && base_info != NULL && ec_bdev != NULL && ec_bdev->base_bdev_info != NULL) {
		ptrdiff_t idx_diff = base_info - ec_bdev->base_bdev_info;
		
		if (idx_diff < 0 || idx_diff >= ec_bdev->num_base_bdevs || 
		    idx_diff >= EC_MAX_K + EC_MAX_P || idx_diff > UINT8_MAX) {
			return;
		}
		
		uint8_t idx = (uint8_t)idx_diff;
		
		if (is_valid_bdev_idx(ec_bdev, idx)) {
			struct base_bdev_wear_info *info = &wl_ext->wear_info[idx];
			
			/* 更新写入统计 */
			if (info->total_writes <= UINT64_MAX - num_blocks) {
				info->total_writes += num_blocks;
			} else {
				info->total_writes = UINT64_MAX;
			}
			
			if (info->writes_since_last_read <= UINT64_MAX - num_blocks) {
				info->writes_since_last_read += num_blocks;
			} else {
				info->writes_since_last_read = UINT64_MAX;
				info->needs_reread = true;
			}
			
			predict_wear_level_for_reread_check(wl_ext, idx);
			if (should_reread_wear_level(wl_ext, idx)) {
				info->needs_reread = true;
			}
			
			/* 更新 is_operational 状态 */
			struct ec_base_bdev_info *base_info_check = &ec_bdev->base_bdev_info[idx];
			info->is_operational = (base_info_check->desc != NULL && !base_info_check->is_failed);
		}
	}
}

/* 扩展模块初始化回调 */
static int
wear_leveling_ext_init_cb(struct ec_bdev_extension_if *ext_if, struct ec_bdev *ec_bdev)
{
	struct wear_leveling_ext *wl_ext = 
		SPDK_CONTAINEROF(ext_if, struct wear_leveling_ext, ext_if);
	
	wl_ext->ec_bdev = ec_bdev;
	
	SPDK_NOTICELOG("Wear leveling extension initialized for EC bdev %s\n",
		       ec_bdev->bdev.name);
	
	return 0;
}

/* 扩展模块清理回调 */
static void
wear_leveling_ext_fini_cb(struct ec_bdev_extension_if *ext_if, struct ec_bdev *ec_bdev)
{
	SPDK_NOTICELOG("Wear leveling extension finalized for EC bdev %s\n",
		       ec_bdev->bdev.name);
}

/* 创建并注册磨损均衡扩展
 * 
 * 算法原理：
 * - 根据磨损程度分配写入量：磨损低的写入多，磨损高的写入少
 * - 磨损相同时，写入量相同
 * - 使用确定性加权选择，确保同一stripe总是选择相同的base bdev
 */
int
wear_leveling_ext_register(struct ec_bdev *ec_bdev)
{
	struct wear_leveling_ext *wl_ext;
	
	if (ec_bdev == NULL) {
		return -EINVAL;
	}
	
	/* 检查是否已经注册了扩展 */
	if (ec_bdev->extension_if != NULL) {
		SPDK_ERRLOG("EC bdev %s already has an extension registered\n",
			    ec_bdev->bdev.name);
		return -EEXIST;
	}
	
	wl_ext = calloc(1, sizeof(*wl_ext));
	if (wl_ext == NULL) {
		return -ENOMEM;
	}
	
	/* 初始化扩展接口 */
	wl_ext->ext_if.name = WEAR_LEVELING_EXT_NAME;
	wl_ext->ext_if.ctx = wl_ext;
	wl_ext->ext_if.select_base_bdevs = wear_leveling_select_base_bdevs;
	wl_ext->ext_if.get_wear_level = wear_leveling_get_wear_level;
	wl_ext->ext_if.notify_io_complete = wear_leveling_notify_io_complete;
	wl_ext->ext_if.init = wear_leveling_ext_init_cb;
	wl_ext->ext_if.fini = wear_leveling_ext_fini_cb;
	
	/* 设置磨损预测参数 */
	wl_ext->wear_predict_threshold_blocks = 20971520; /* 默认10GB (20M blocks * 512B) */
	wl_ext->wear_predict_threshold_percent = 5;       /* 默认5% */
	wl_ext->wear_read_interval_us = 30000000;         /* 默认30秒最小读取间隔 */
	
	/* 参数验证：确保阈值合理 */
	if (wl_ext->wear_predict_threshold_percent == 0) {
		wl_ext->wear_predict_threshold_percent = 1; /* 最小1% */
	}
	if (wl_ext->wear_predict_threshold_percent > 100) {
		wl_ext->wear_predict_threshold_percent = 100; /* 最大100% */
	}
	
	/* 初始化磨损信息 */
	memset(wl_ext->wear_info, 0, sizeof(wl_ext->wear_info));
	memset(wl_ext->bdev_config, 0, sizeof(wl_ext->bdev_config));
	memset(wl_ext->cached_block_size, 0, sizeof(wl_ext->cached_block_size));
	wl_ext->cache_hits = 0;
	wl_ext->cache_misses = 0;
	wl_ext->fast_path_hits = 0;
	wl_ext->all_wear_unavailable = false;  /* 初始化为false，后续检查 */
	
	/* 初始化每个bdev的TBW和磨损率，并读取实际磨损信息
	 * 根据实际NVMe盘的特性设置默认值：
	 * - TLC (3D NAND): 通常 0.3-0.6 TBW per 100GB容量
	 *   例如：500GB TLC SSD，TBW通常为 150-300TB
	 * - MLC: 通常 1-2 TBW per 100GB容量
	 * - SLC: 通常 10+ TBW per 100GB容量
	 * 
	 * 默认使用TLC的平均值：0.4 TBW per 100GB
	 * 即500GB SSD的TBW = 500 * 0.4 = 200TB
	 */
	struct ec_base_bdev_info *base_info;
	uint8_t i = 0;
	uint8_t operational_count = 0;  /* 可用的base bdev数量 */
	uint8_t wear_read_success_count = 0;  /* 成功读取磨损信息的数量 */
	
	/* 注意：num_base_bdevs 是 uint8_t，最大值是 255
	 * 数组大小是 EC_MAX_K + EC_MAX_P = 510，所以不会超出数组范围
	 * 因此不需要检查 num_base_bdevs 是否超过数组大小
	 */
	
	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (i >= EC_MAX_K + EC_MAX_P) {
			SPDK_ERRLOG("EC bdev %s: base bdev index %u exceeds maximum\n",
				    ec_bdev->bdev.name, i);
			break;
		}
		
		if (base_info->desc != NULL) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(base_info->desc);
			if (bdev == NULL) {
				/* bdev 为 NULL，跳过这个 base_info */
				wl_ext->bdev_config[i].tbw = 0;
				wl_ext->bdev_config[i].wear_per_gb = 0;
				i++;
				continue;
			}
			
			/* 计算容量（GB）
			 * 注意：检查乘法溢出，避免 num_blocks * block_size 溢出
			 */
			uint64_t num_blocks = spdk_bdev_get_num_blocks(bdev);
			uint32_t block_size = spdk_bdev_get_block_size(bdev);
			uint64_t capacity_gb;
			
			if (num_blocks > 0 && block_size > UINT64_MAX / num_blocks) {
				/* 溢出风险：使用饱和值 */
				capacity_gb = UINT64_MAX / GB_TO_BYTES;
			} else {
				capacity_gb = (num_blocks * block_size) / GB_TO_BYTES;
			}
			
			/* 默认TBW：TLC SSD，0.4 TBW per 100GB容量
			 * 注意：capacity_gb 是 uint64_t，0.004 是 double
			 * 显式转换为 double 进行计算，避免精度损失
			 */
			double capacity_gb_double = (double)capacity_gb;
			wl_ext->bdev_config[i].tbw = capacity_gb_double * 0.004;  /* 0.4 / 100 = 0.004 */
			
			/* 检查结果是否合理（防止异常值） */
			if (wl_ext->bdev_config[i].tbw < 0.0 || wl_ext->bdev_config[i].tbw > 1000000.0) {
				SPDK_WARNLOG("Calculated TBW out of range: %.2f TB (capacity_gb=%lu), using default\n",
					     wl_ext->bdev_config[i].tbw, capacity_gb);
				wl_ext->bdev_config[i].tbw = 180.0;  /* 默认180TB */
			}
			
			/* 计算每GB磨损率：wear_per_gb = 100 / (TBW * 1024)
			 * 注意：tbw 可能为 0（如果 capacity_gb 为 0），需要检查
			 * 防止除零和异常值
			 */
			double tbw_gb = wl_ext->bdev_config[i].tbw * TB_TO_GB;
			if (wl_ext->bdev_config[i].tbw > 0.0 && tbw_gb > 0.0) {
				wl_ext->bdev_config[i].wear_per_gb = 100.0 / tbw_gb;
				/* 检查结果是否合理（防止异常值） */
				if (wl_ext->bdev_config[i].wear_per_gb <= 0.0 || 
				    wl_ext->bdev_config[i].wear_per_gb > 100.0) {
					/* 异常值，使用默认值 */
					wl_ext->bdev_config[i].wear_per_gb = 0.000543;  /* 默认180TB的磨损率 */
				}
			} else {
				/* TBW 为 0 或无效，使用默认值 */
				wl_ext->bdev_config[i].wear_per_gb = 0.000543;  /* 默认180TB的磨损率 */
			}
			
			/* 性能优化：预缓存block_size，避免首次访问时的查询开销 */
			if (block_size != 0) {
				wl_ext->cached_block_size[i] = block_size;
			} else {
				wl_ext->cached_block_size[i] = 512;  /* 默认值 */
			}
			
			/* 初始化时读取实际磨损信息
			 * 优化：异步读取可能更好，但这里保持同步以确保初始化完成
			 * 如果读取失败，使用默认值，后续会通过预测机制更新
			 */
			if (!base_info->is_failed) {
				operational_count++;
				uint8_t wear_level = 255;  /* 初始化为无效值 */
				int read_rc = get_nvme_wear_level_from_bdev(bdev, &wear_level);
				if (read_rc == 0) {
					/* 成功读取，验证值的有效性（双重检查，鲁棒性） */
					if (wear_level <= 100) {
						wl_ext->wear_info[i].wear_level = wear_level;
						wl_ext->wear_info[i].predicted_wear_level = wear_level;
						wl_ext->wear_info[i].last_read_timestamp = spdk_get_ticks();
						wl_ext->wear_info[i].is_operational = 1;
						wl_ext->wear_info[i].needs_reread = 0;
						wear_read_success_count++;  /* 成功读取磨损信息 */
					} else {
						/* 读取成功但值无效（不应该发生，因为函数内部已验证）
						 * 但为了鲁棒性，仍然处理这种情况
						 */
						SPDK_ERRLOG("EC bdev %s: base bdev %u returned invalid wear level %u (expected 0-100)\n",
							    ec_bdev->bdev.name, i, wear_level);
						wl_ext->wear_info[i].wear_level = 255;  /* 无效值 */
						wl_ext->wear_info[i].predicted_wear_level = 255;
						wl_ext->wear_info[i].last_read_timestamp = 0;
						wl_ext->wear_info[i].needs_reread = 1;
						wl_ext->wear_info[i].is_operational = 1;  /* 设备可用，只是无法读取磨损 */
					}
				} else {
					/* 读取失败，处理各种错误场景（鲁棒性）
					 * 
					 * 错误场景包括：
					 * - ENODEV: 设备不可用、控制器失败、不是NVMe设备
					 * - ETIMEDOUT: 超时（可能是临时故障）
					 * - EIO: 命令失败或数据无效
					 * - ENOMEM: 内存分配失败（罕见）
					 * 
					 * 策略：标记为无效值（255），表示无法获取磨损信息
					 * 这样磨损分布功能会回退到默认策略
					 */
					SPDK_WARNLOG("EC bdev %s: Failed to read wear level from base bdev %u (rc: %d, bdev: %s)\n",
						     ec_bdev->bdev.name, i, read_rc, spdk_bdev_get_name(bdev));
					wl_ext->wear_info[i].wear_level = 255;  /* 无效值 */
					wl_ext->wear_info[i].predicted_wear_level = 255;
					wl_ext->wear_info[i].last_read_timestamp = 0; /* 标记需要读取 */
					wl_ext->wear_info[i].needs_reread = 1;
					wl_ext->wear_info[i].is_operational = 1;  /* 设备可用，只是无法读取磨损 */
				}
			} else {
				/* base bdev 已失败，标记为不可用 */
				wl_ext->wear_info[i].is_operational = 0;
			}
		} else {
			/* base_info->desc 为 NULL，标记为不可用 */
			wl_ext->bdev_config[i].tbw = 0;
			wl_ext->bdev_config[i].wear_per_gb = 0;
			wl_ext->wear_info[i].is_operational = 0;
		}
		
		i++;
	}
	
	/* 检查是否有任意一个可用的base bdev无法读取磨损信息
	 * 如果有任意一个无法读取，就立即回退到默认写入方式
	 * 这是严格的要求：只要有一个磁盘无法读取磨损信息，就禁用整个磨损均衡模块
	 */
	if (operational_count > 0) {
		if (wear_read_success_count < operational_count) {
			/* 有任意一个可用的base bdev无法读取磨损信息，立即回退到默认写入方式 */
			SPDK_WARNLOG("EC bdev %s: Cannot read wear level from %u/%u operational base bdevs, "
				     "immediately falling back to default write strategy\n", 
				     ec_bdev->bdev.name, 
				     operational_count - wear_read_success_count, operational_count);
			wl_ext->all_wear_unavailable = true;
		} else {
			/* 所有可用的base bdev都能读取磨损信息，可以使用磨损均衡 */
			wl_ext->all_wear_unavailable = false;
			SPDK_NOTICELOG("EC bdev %s: Wear leveling enabled, all %u base bdevs have wear info\n",
				       ec_bdev->bdev.name, operational_count);
		}
	} else {
		/* 没有可用的base bdev，这种情况不应该发生 */
		SPDK_ERRLOG("EC bdev %s: No operational base bdevs found\n", ec_bdev->bdev.name);
		wl_ext->all_wear_unavailable = true;
	}
	
	/* 注册到EC bdev
	 * 注意：如果注册失败（例如 init 回调失败），需要释放 wl_ext
	 */
	int rc = ec_bdev_register_extension(ec_bdev, &wl_ext->ext_if);
	if (rc != 0) {
		/* 注册失败，释放已分配的内存 */
		SPDK_ERRLOG("Failed to register wear leveling extension for EC bdev %s: %s\n",
			    ec_bdev->bdev.name, spdk_strerror(-rc));
		free(wl_ext);
		return rc;
	}
	
	return 0;
}

/* 注销磨损均衡扩展 */
void
wear_leveling_ext_unregister(struct ec_bdev *ec_bdev)
{
	struct ec_bdev_extension_if *ext_if;
	struct wear_leveling_ext *wl_ext;
	
	if (ec_bdev == NULL) {
		return;
	}
	
	ext_if = ec_bdev_get_extension(ec_bdev);
	if (ext_if == NULL || strcmp(ext_if->name, WEAR_LEVELING_EXT_NAME) != 0) {
		return;
	}
	
	wl_ext = SPDK_CONTAINEROF(ext_if, struct wear_leveling_ext, ext_if);
	
	ec_bdev_unregister_extension(ec_bdev);
	free(wl_ext);
}

/* 设置指定base bdev的TBW */
int
wear_leveling_ext_set_tbw(struct ec_bdev *ec_bdev,
			   uint8_t base_bdev_index,
			   double tbw)
{
	struct ec_bdev_extension_if *ext_if;
	struct wear_leveling_ext *wl_ext;
	
	if (ec_bdev == NULL) {
		return -EINVAL;
	}
	
	if (base_bdev_index >= ec_bdev->num_base_bdevs || 
	    base_bdev_index >= EC_MAX_K + EC_MAX_P) {
		return -EINVAL;
	}
	
	if (tbw <= 0 || tbw > 100000) {
		/* TBW范围：0.1TB到100000TB（100PB） */
		return -EINVAL;
	}
	
	ext_if = ec_bdev_get_extension(ec_bdev);
	if (ext_if == NULL || strcmp(ext_if->name, WEAR_LEVELING_EXT_NAME) != 0) {
		return -ENOENT;
	}
	
	wl_ext = SPDK_CONTAINEROF(ext_if, struct wear_leveling_ext, ext_if);
	
	/* 设置TBW */
	wl_ext->bdev_config[base_bdev_index].tbw = tbw;
	
	/* 重新计算磨损率：wear_per_gb = 100 / (TBW * 1024)
	 * 注意：tbw 已经检查过 > 0，但为了鲁棒性，仍然检查除零和异常值
	 */
	double tbw_gb = tbw * TB_TO_GB;
	if (tbw_gb > 0.0) {
		wl_ext->bdev_config[base_bdev_index].wear_per_gb = 100.0 / tbw_gb;
		/* 检查结果是否合理（防止异常值） */
		if (wl_ext->bdev_config[base_bdev_index].wear_per_gb <= 0.0 ||
		    wl_ext->bdev_config[base_bdev_index].wear_per_gb > 100.0) {
			SPDK_ERRLOG("Invalid wear_per_gb calculated: %f (tbw=%.2f)\n",
				    wl_ext->bdev_config[base_bdev_index].wear_per_gb, tbw);
			return -EINVAL;
		}
	} else {
		SPDK_ERRLOG("Invalid TBW value: %.2f (cannot calculate wear_per_gb)\n", tbw);
		return -EINVAL;
	}
	
	SPDK_NOTICELOG("Set TBW for base bdev %u to %.2f TB, wear_per_gb = %.6f\n",
		       base_bdev_index, tbw, wl_ext->bdev_config[base_bdev_index].wear_per_gb);
	
	return 0;
}

/* 设置磨损预测阈值参数 */
int
wear_leveling_ext_set_predict_params(struct ec_bdev *ec_bdev,
				      uint64_t threshold_blocks,
				      uint8_t threshold_percent,
				      uint64_t read_interval_us)
{
	struct ec_bdev_extension_if *ext_if;
	struct wear_leveling_ext *wl_ext;
	
	if (ec_bdev == NULL) {
		return -EINVAL;
	}
	
	/* 参数验证：确保阈值合理 */
	if (threshold_percent == 0 || threshold_percent > 100) {
		return -EINVAL;
	}
	
	/* 参数验证：read_interval_us 为 0 时表示总是读取（用于调试），允许 */
	
	ext_if = ec_bdev_get_extension(ec_bdev);
	if (ext_if == NULL || strcmp(ext_if->name, WEAR_LEVELING_EXT_NAME) != 0) {
		return -ENOENT;
	}
	
	wl_ext = SPDK_CONTAINEROF(ext_if, struct wear_leveling_ext, ext_if);
	
	wl_ext->wear_predict_threshold_blocks = threshold_blocks;
	wl_ext->wear_predict_threshold_percent = threshold_percent;
	wl_ext->wear_read_interval_us = read_interval_us;
	
	SPDK_NOTICELOG("Set wear predict params: threshold_blocks=%lu, threshold_percent=%u, read_interval_us=%lu\n",
		       threshold_blocks, threshold_percent, read_interval_us);
	
	return 0;
}

