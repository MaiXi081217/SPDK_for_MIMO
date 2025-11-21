# SPDK RAID模块开发任务

## 任务

在SPDK RAID模块的性能调优工作中，需要了解RAID bdev的IO对齐信息，以便优化应用程序的IO模式。

**任务**：添加一个RPC命令`bdev_raid_get_io_alignment`，用于查询指定RAID bdev的IO对齐相关信息。

**功能要求**：
1. 输入：RAID bdev名称
2. 输出：JSON格式，包含以下信息：
   - `strip_size_kb`：用户创建时指定的条带大小（KB）
   - `strip_size_blocks`：实际使用的条带大小（blocks）
   - `optimal_io_boundary_blocks`：系统用于IO对齐的边界大小（blocks）
   - `io_split_threshold_blocks`：超过此大小的IO请求会被系统自动分割（blocks）
   - `split_enabled`：系统是否启用了IO自动分割（boolean）

3. 所有blocks单位的字段必须准确反映系统实际使用的值

**实现要求**：
- 在`module/bdev/raid/bdev_raid_rpc.c`中添加RPC方法
- 可以复用现有的数据结构，不需要从零实现新功能
- 确保返回的值准确，特别是`optimal_io_boundary_blocks`和`io_split_threshold_blocks`必须反映系统实际行为

## 评分标准

- **功能实现正确性**（50分）
- **数据准确性**（40分）：特别是optimal_io_boundary_blocks和io_split_threshold_blocks的准确性
- **代码质量**（10分）

## 提交要求

请提供：
1. 完整的实现代码
2. 数据获取方式的说明，包括：
   - 每个字段的值从哪里获取
   - 为什么选择这个数据源
   - 如何验证返回值的准确性
3. 测试结果和验证方法
4. 在实现过程中遇到的问题和解决方案

---

**注意**：本题是一个实际的开发任务，需要你通过阅读源代码来理解RAID模块的数据结构、IO处理机制等，确保返回的数据准确反映系统实际行为。要准确获取`optimal_io_boundary_blocks`和`io_split_threshold_blocks`，你需要深入理解系统如何决定IO分割的边界。
