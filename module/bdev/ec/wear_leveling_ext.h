/**
 * @file wear_leveling_ext.h
 * @brief 磨损均衡扩展模块 - EC Bdev 磨损感知调度
 * 
 * 本模块为 SPDK EC bdev 提供磨损均衡功能，通过监控各 base bdev 的磨损程度，
 * 智能分配写入负载，延长 SSD 寿命。
 * 
 * 主要特性：
 * - 三级模式：DISABLED/SIMPLE/FULL，可根据需求选择复杂度
 * - 确定性调度：同一 stripe 始终选择相同的 base bdev
 * - 自动降级：检测到故障时自动从复杂模式退回到简单模式
 * - 低开销：缓存机制减少 NVMe 命令调用
 * 
 * 设计原则：越简单越可靠
 */

#ifndef WEAR_LEVELING_EXT_H
#define WEAR_LEVELING_EXT_H

#include "bdev_ec.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * ============================================================================
 * 常量定义
 * ============================================================================
 */

/* 扩展名称 */
#define WEAR_LEVELING_EXT_NAME "wear_leveling"

/* 单位转换常量 */
#define WEAR_GB_TO_BYTES (1024ULL * 1024 * 1024)
#define WEAR_TB_TO_GB (1024ULL)

/* 磨损差异阈值：小于此值使用快速路径（默认策略） */
#define WEAR_DIFF_THRESHOLD 5  /* 5% */

/* 默认预测参数 */
#define WEAR_DEFAULT_THRESHOLD_BLOCKS (20971520ULL)  /* 10GB = 20M blocks * 512B */
#define WEAR_DEFAULT_THRESHOLD_PERCENT (5)           /* 5% */
#define WEAR_DEFAULT_READ_INTERVAL_US (30000000ULL)  /* 30秒 */

/* 快照存储配置 */
#define MAX_SNAPSHOT_STRIPES 1048576  /* 2^20 = 1M 个 stripe，覆盖常用范围 */
#define SNAPSHOT_BITMAP_SIZE (MAX_SNAPSHOT_STRIPES / 8)  /* 位图大小（字节） */

/* 自动降级阈值 */
#define WEAR_AUTO_FALLBACK_THRESHOLD (5)  /* 连续失败5次触发自动降级 */
#define SNAPSHOT_AUTO_FALLBACK_THRESHOLD (3)  /* 快照连续失败3次触发自动降级 */

/*
 * ============================================================================
 * 枚举和结构体定义
 * ============================================================================
 */

/* 磨损均衡模式：决定算法复杂度与可靠性取舍 */
enum wear_leveling_mode {
	WL_MODE_DISABLED = 0,	/* 始终使用默认EC调度，等同于未启用扩展 */
	WL_MODE_SIMPLE,		/* 仅基于缓存的磨损信息进行确定性排序，不触发预测/重读 */
	WL_MODE_FULL,		/* 完整特性：预测、NVMe健康信息读取、加权选择 */
};

/* Base bdev磨损信息（优化内存布局，提高缓存局部性）
 * 
 * 优化说明：
 * 1. 移除冗余的index字段（可通过数组索引计算）
 * 2. 使用位域压缩布尔标志，节省内存
 * 3. 将经常一起访问的字段放在一起
 * 4. 对齐到8字节边界，提高访问效率
 */
struct base_bdev_wear_info {
	/* 磨损等级（热路径，放在前面） */
	uint8_t wear_level;         /* 当前磨损等级 0-100 */
	uint8_t predicted_wear_level; /* 预测的磨损等级 0-100 */
	
	/* 写入统计 */
	uint64_t total_writes;      /* 累计写入量（块数） */
	uint64_t writes_since_last_read; /* 上次读取后的写入量 */
	
	/* 时间戳 */
	uint64_t last_read_timestamp;    /* 上次读取实际磨损的时间戳 */
	
	/* 标志位（使用位域节省内存） */
	unsigned int is_operational : 1;  /* 是否可用 */
	unsigned int needs_reread : 1;    /* 是否需要重新读取 */
	unsigned int reserved : 6;        /* 保留位 */
} __attribute__((packed));

/* Base bdev配置信息（TBW和磨损率） */
struct base_bdev_config {
	double tbw;          /* TBW（Total Bytes Written，单位：TB） */
	double wear_per_gb;  /* 每GB磨损率 */
} __attribute__((packed));

/* 数据采集模块 */
struct wear_data_provider {
	struct base_bdev_wear_info wear_info[EC_MAX_K + EC_MAX_P];
	struct base_bdev_config bdev_config[EC_MAX_K + EC_MAX_P];
	uint32_t cached_block_size[EC_MAX_K + EC_MAX_P];
	uint64_t cache_hits;
	uint64_t cache_misses;
};

/* 调度策略模块 */
struct wear_scheduler_state {
	enum wear_leveling_mode mode;
	bool all_wear_unavailable;
	uint64_t fast_path_hits;
	
	/* 健康检测和自动降级相关 */
	uint32_t consecutive_failures;     /* 连续失败次数 */
	uint32_t auto_fallback_threshold;  /* 自动降级阈值 */
	
	/* 快照失败计数（仅FULL模式） */
	uint32_t snapshot_failures;        /* 快照连续失败次数 */
	uint32_t snapshot_fallback_threshold;  /* 快照自动降级阈值 */
};

/*
 * ============================================================================
 * 公共 API
 * ============================================================================
 */

/* 创建并注册磨损均衡扩展
 * 
 * 算法原理：
 * - 根据磨损程度分配写入量：磨损低的写入多，磨损高的写入少
 * - 磨损相同时，写入量相同
 * - 使用确定性加权选择，确保同一stripe总是选择相同的base bdev
 * 
 * ec_bdev: EC bdev
 * mode: 磨损均衡模式 (DISABLED/SIMPLE/FULL)
 * 返回: 0成功，负数失败
 */
int wear_leveling_ext_register(struct ec_bdev *ec_bdev, enum wear_leveling_mode mode);

/**
 * @brief 注销磨损均衡扩展
 * 
 * @param ec_bdev EC bdev 实例
 */
void wear_leveling_ext_unregister(struct ec_bdev *ec_bdev);

/* 设置指定base bdev的TBW（Total Bytes Written，总写入字节数，单位：TB）
 * ec_bdev: EC bdev
 * base_bdev_index: base bdev索引（0到num_base_bdevs-1）
 * tbw: TBW值（单位：TB）
 * 
 * 常见SSD的TBW参考值：
 * - TLC (3D NAND): 通常 0.3-0.6 TBW per 100GB容量
 *   例如：500GB TLC SSD，TBW通常为 150-300TB
 * - MLC: 通常 1-2 TBW per 100GB容量
 *   例如：500GB MLC SSD，TBW通常为 500-1000TB
 * - SLC: 通常 10+ TBW per 100GB容量
 *   例如：500GB SLC SSD，TBW通常为 5000TB+
 * 
 * 磨损率会自动计算：wear_per_gb = 100 / (TBW * 1024)
 * 例如：TBW=180TB，wear_per_gb = 100 / (180 * 1024) = 0.000543
 * 即每1GB写入增加约0.000543%磨损
 * 
 * 返回: 0成功，负数失败
 */
int wear_leveling_ext_set_tbw(struct ec_bdev *ec_bdev,
			       uint16_t base_bdev_index,
			       double tbw);

/* 设置磨损预测阈值参数（仅在FULL模式有效）
 * ec_bdev: EC bdev
 * threshold_blocks: 写入多少块后重新读取（默认10GB = 20971520 blocks）
 * threshold_percent: 预测磨损变化超过多少百分比时重新读取（默认5%）
 * read_interval_us: 最小读取间隔（微秒，默认30秒 = 30000000）
 * 返回: 0成功，负数失败
 */
int wear_leveling_ext_set_predict_params(struct ec_bdev *ec_bdev,
					  uint64_t threshold_blocks,
					  uint8_t threshold_percent,
					  uint64_t read_interval_us);

/**
 * @brief 设置磨损均衡模式（运行时动态切换）
 * 
 * @param ec_bdev EC bdev 实例
 * @param mode 新模式
 * @return 0 成功，负数失败（-EINVAL, -ENOENT）
 */
int wear_leveling_ext_set_mode(struct ec_bdev *ec_bdev,
			       enum wear_leveling_mode mode);

/**
 * @brief 获取当前磨损均衡模式
 * 
 * @param ec_bdev EC bdev 实例
 * @return 当前模式（0-2），失败返回负数（-EINVAL, -ENOENT）
 */
int wear_leveling_ext_get_mode(struct ec_bdev *ec_bdev);

/*
 * ============================================================================
 * 快照存储 API（内部使用，确保读取一致性）
 * ============================================================================
 */

/**
 * @brief 存储 stripe 的 bdev 选择快照（仅在写入成功后调用）
 * 
 * @param ec_bdev EC bdev 实例
 * @param stripe_index Stripe 索引
 * @param data_indices 数据块索引数组（k个）
 * @param parity_indices 校验块索引数组（p个）
 * @return 0 成功，负数失败（-EINVAL, -ENOENT, -ERANGE）
 */
int wear_leveling_ext_store_snapshot(struct ec_bdev *ec_bdev,
				     uint64_t stripe_index,
				     const uint8_t *data_indices,
				     const uint8_t *parity_indices);

/**
 * @brief 从快照读取 stripe 的 bdev 选择（读取时调用）
 * 
 * @param ec_bdev EC bdev 实例
 * @param stripe_index Stripe 索引
 * @param data_indices 输出：数据块索引数组（k个）
 * @param parity_indices 输出：校验块索引数组（p个）
 * @return true 快照存在且读取成功，false 快照不存在（应使用默认选择）
 */
bool wear_leveling_ext_load_snapshot(struct ec_bdev *ec_bdev,
				     uint64_t stripe_index,
				     uint8_t *data_indices,
				     uint8_t *parity_indices);

/*
 * ============================================================================
 * 测试辅助 API（仅用于单元测试 / RPC 场景）
 * ============================================================================
 */
void wear_leveling_ext_enable_test_mode(bool enable);
bool wear_leveling_ext_is_test_mode(void);

int wear_leveling_ext_test_override_wear(struct ec_bdev *ec_bdev,
					 uint16_t base_bdev_index,
					 uint8_t wear_level,
					 bool is_operational);

int wear_leveling_ext_test_get_fast_path_hits(struct ec_bdev *ec_bdev,
					      uint64_t *fast_path_hits);

#endif /* WEAR_LEVELING_EXT_H */
