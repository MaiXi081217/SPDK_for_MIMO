# SPDK RAID模块开发任务

## 任务

在SPDK RAID0模块中，添加一个RPC命令`bdev_raid_get_strip_info`，用于查询指定RAID bdev的条带信息。

**功能要求**：
- 输入：RAID bdev名称
- 输出：该RAID bdev的条带大小（以blocks为单位）

**实现要求**：
- 在`module/bdev/raid/bdev_raid_rpc.c`中添加RPC方法
- 在`module/bdev/raid/bdev_raid.c`的`raid_bdev_write_info_json`函数中添加相应的JSON输出字段
- 确保返回的值是准确的blocks数

## 评分标准

- **功能实现正确性**（60分）
- **代码质量和完整性**（40分）

## 提交要求

请提供：
1. 完整的实现代码
2. 编译和测试结果
3. 使用说明

---

**注意**：本题是一个实际的开发任务，需要你通过阅读源代码来理解相关数据结构和计算逻辑，确保实现正确。
