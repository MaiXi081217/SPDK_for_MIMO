# EC Process框架移植状态总结

## 日期：2024年当前日期

## 已完成的工作

### 1. 结构定义（已完成 ✅）
- **文件**: `bdev_ec_internal.h`
- **内容**:
  - 定义了 `enum ec_process_type` (EC_PROCESS_NONE, EC_PROCESS_REBUILD)
  - 定义了 `enum ec_bdev_process_state` (INIT, RUNNING, STOPPING, STOPPED)
  - 定义了 `struct ec_bdev_process` - 核心process结构，包含：
    - window机制相关字段（window_offset, window_size, window_remaining, window_status）
    - error_status（volatile int，用于统一错误处理）
    - rebuild_state（细粒度状态跟踪）
    - request queue和finish_actions队列
  - 定义了 `struct ec_bdev_process_request` - process请求结构
  - 定义了 `struct ec_process_qos` - QoS控制结构
  - 定义了 `struct ec_process_finish_action` - 完成动作结构
  - 添加了辅助函数：`ec_bdev_get_active_process()`, `ec_bdev_base_bdev_slot()`

- **文件**: `bdev_ec.h`
- **内容**:
  - 在 `struct ec_bdev` 中添加了 `process` 字段（指向 `ec_bdev_process`）
  - 在 `struct ec_bdev_io_channel` 中添加了 `process` 字段（包含offset, target_ch, ch_processed）

### 2. Process框架基础实现（已完成 ✅）
- **文件**: `bdev_ec_process.c` (新创建，约480行)
- **内容**:
  - `ec_bdev_start_process()` - 启动process，创建独立线程
  - `ec_bdev_stop_process()` - 停止process
  - `ec_bdev_is_process_in_progress()` - 检查process是否进行中
  - `ec_bdev_process_thread_init()` - process线程初始化
  - `ec_bdev_ch_process_setup()` - 设置process channel（简化版，适配EC的k+p结构）
  - `ec_bdev_ch_process_cleanup()` - 清理process channel
  - `ec_bdev_process_free()` - 释放process结构

### 3. Window机制实现（已完成 ✅）
- **文件**: `bdev_ec_process.c`
- **内容**:
  - `ec_bdev_process_lock_window_range()` - 锁定LBA范围（使用spdk_bdev_quiesce_range）
  - `ec_bdev_process_unlock_window_range()` - 解锁LBA范围（使用spdk_bdev_unquiesce_range）
  - `ec_bdev_process_window_range_locked()` - 锁定完成回调
  - `ec_bdev_process_window_range_unlocked()` - 解锁完成回调
  - Window机制已完整实现，支持逐window处理

### 4. Superblock集成（已完成 ✅）
- **文件**: `bdev_ec_process.c`
- **内容**:
  - Process启动时：更新superblock状态为 `EC_SB_BASE_BDEV_REBUILDING`
  - 每个window完成时：写入superblock以持久化进度（状态保持REBUILDING）
  - Process完成时：更新superblock状态为 `EC_SB_BASE_BDEV_CONFIGURED`
  - Process停止/失败时：更新superblock状态为 `EC_SB_BASE_BDEV_FAILED`
  - 完全融合了EC的superblock机制

### 5. Makefile更新（已完成 ✅）
- **文件**: `Makefile`
- **内容**: 添加了 `bdev_ec_process.c` 到编译列表

### 6. 函数声明（已完成 ✅）
- **文件**: `bdev_ec_internal.h`
- **内容**: 添加了process框架相关函数的声明

## 当前代码状态

### 编译状态
- ✅ 编译通过，无错误
- ⚠️ 有1个警告：`ec_bdev_process_unlock_window_range` 定义但未使用（这是正常的，会在后续实现中使用）

### 代码位置
- 主要文件：`/home/max/SPDK_for_MIMO/module/bdev/ec/bdev_ec_process.c` (约480行)
- 头文件修改：`bdev_ec_internal.h`, `bdev_ec.h`

## 待完成的工作

### 1. Request Queue和Submit Process Request（待实现 ⏳）
- **位置**: `bdev_ec_process.c`
- **需要实现**:
  - `ec_bdev_submit_process_request()` - 提交process request（适配EC的k+p解码）
  - `ec_bdev_process_request_complete()` - request完成回调
  - `ec_bdev_process_stripe_read_complete()` - 条带读取完成回调（融合现有逻辑）
  - `ec_bdev_process_stripe_write_complete()` - 条带写入完成回调（融合现有逻辑）
  - `ec_bdev_process_thread_run()` - 完善process线程运行逻辑，处理window内的条带

### 2. 融合现有条带处理逻辑（待实现 ⏳）
- **需要融合的现有逻辑**（来自 `bdev_ec_rebuild.c`）:
  - 条带选择逻辑（`ec_rebuild_next_stripe`中的选择部分，支持wear leveling）
  - 解码逻辑（`ec_decode_stripe`调用）
  - 缓冲区管理（`stripe_buf`和`recover_buf`，使用`ec_get_rmw_stripe_buf`）
  - 错误处理和superblock更新
- **融合方式**:
  - 将逐条带处理改为按window处理
  - 在window内，对每个条带调用类似`ec_rebuild_next_stripe`的逻辑
  - 使用process request queue管理I/O
  - 复用现有的解码和缓冲区管理

### 3. Process Request结构扩展（待实现 ⏳）
- **需要添加**:
  - 在 `ec_bdev_process_request` 中添加条带相关的上下文（或使用 `ec_process_stripe_ctx`）
  - 存储条带索引、可用base bdevs、frag_map等信息

### 4. 错误处理和状态输出（待实现 ⏳）
- **需要添加**:
  - 统一的错误处理（error_status和window_status同步）
  - 用户友好的状态输出（类似RAID的SPDK_NOTICELOG输出）
  - 进度报告（基于window_offset而非stripe计数）

### 5. 替换现有rebuild实现（待实现 ⏳）
- **需要修改**:
  - `ec_bdev_start_rebuild()` - 改为调用 `ec_bdev_start_process()`
  - `ec_bdev_stop_rebuild()` - 改为调用 `ec_bdev_stop_process()`
  - `ec_bdev_is_rebuilding()` - 改为检查process
  - `ec_bdev_get_rebuild_progress()` - 改为基于window_offset计算

### 6. 测试和验证（待完成 ⏳）
- 编译测试
- 功能测试（创建EC，移除base bdev，添加新bdev，验证rebuild）

## 关键设计决策

### 1. 保持简单可靠稳定
- 复用RAID的process框架设计
- 最小化自定义实现
- 保持与现有EC代码的兼容性

### 2. 融合现有逻辑
- 不重写条带处理逻辑，而是适配到process框架
- 保留wear leveling支持
- 保留现有的缓冲区管理机制

### 3. Window vs Stripe
- **Window**: LBA范围，用于锁定和进度跟踪
- **Stripe**: EC的逻辑单位，一个window可能包含多个stripe
- 在window内按stripe处理，但使用window机制管理进度

## 下一步工作重点

1. **实现submit_process_request** - 这是核心，需要融合现有的条带处理逻辑
2. **实现request完成回调** - 处理读取、解码、写入的完整流程
3. **完善thread_run逻辑** - 在window内处理所有条带
4. **替换现有rebuild接口** - 保持API兼容性

## 注意事项

- EC的rebuild是按**条带（stripe）**处理的，不是按块（block）
- 每个条带需要读取k个块，解码，然后写入target
- 需要支持wear leveling的条带选择
- 需要复用现有的缓冲区池（`ec_get_rmw_stripe_buf`）
- Window机制用于LBA范围锁定和进度管理，但实际处理仍按条带进行

