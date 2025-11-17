/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Intel Corporation.
 *   All rights reserved.
 */

#ifndef WEAR_LEVELING_EXT_H
#define WEAR_LEVELING_EXT_H

#include "bdev_ec.h"

/* 创建并注册磨损均衡扩展
 * 
 * 算法原理：
 * - 根据磨损程度分配写入量：磨损低的写入多，磨损高的写入少
 * - 磨损相同时，写入量相同
 * - 使用确定性加权选择，确保同一stripe总是选择相同的base bdev
 * 
 * ec_bdev: EC bdev
 * 返回: 0成功，负数失败
 */
int wear_leveling_ext_register(struct ec_bdev *ec_bdev);

/* 注销磨损均衡扩展 */
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
			       uint8_t base_bdev_index,
			       double tbw);

/* 设置磨损预测阈值参数
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

#endif /* WEAR_LEVELING_EXT_H */

