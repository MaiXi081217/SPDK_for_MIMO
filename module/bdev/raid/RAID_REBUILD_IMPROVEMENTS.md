
# RAID 模块重建功能改进文档

## 一、EC 详细进度报告机制说明

### 1. 详细进度报告（JSON 输出）

**不是实时推送，而是按需查询**：

- **触发时机**：每次调用 RPC `bdev_ec_get_bdevs` 时返回当前进度
- **数据来源**：从内存中的 `rebuild_ctx` 结构读取
- **输出内容**：
  ```json
  {
    "rebuild": {
      "target": "Nvme0n1",
      "target_slot": 2,
      "progress": {
        "current_stripe": 1000,
        "total_stripes": 10000,
        "percent": 10.0,
        "state": "reading"
      }
    }
  }
  ```

**特点**：
- ✅ 每次查询返回最新进度（从内存读取）
- ✅ 包含详细信息：目标磁盘、slot、进度、百分比、状态
- ✅ 非阻塞：不影响重建过程

### 2. 进度查询 API（C 函数）

**面向程序调用**：

```c
int ec_bdev_get_rebuild_progress(struct ec_bdev *ec_bdev, 
                                 uint64_t *current_stripe,
                                 uint64_t *total_stripes);
```

**特点**：
- ✅ 返回当前条带数和总条带数
- ✅ 可以被其他模块调用
- ✅ 可以被 RPC 封装

### 3. 两者的关系

```
┌─────────────────────────────────────────┐
│  rebuild_ctx (内存中的重建上下文)        │
│  - current_stripe                       │
│  - total_stripes                        │
│  - state                                │
│  - target_base_info                     │
└─────────────────────────────────────────┘
         │                    │
         │                    │
    ┌────▼────┐         ┌─────▼─────┐
    │ JSON    │         │ C API     │
    │ 输出    │         │ 查询      │
    └─────────┘         └───────────┘
         │                    │
         │                    │
    ┌────▼────┐         ┌─────▼─────┐
    │ RPC调用 │         │ 其他模块  │
    │ 返回    │         │ 调用      │
    └─────────┘         └───────────┘
```

**关键点**：
- **数据源相同**：都读取 `rebuild_ctx`
- **用途不同**：JSON 面向用户/监控，C API 面向程序集成
- **更新方式**：重建过程中更新 `rebuild_ctx`，查询时读取最新值
- **不是实时推送**：需要主动查询才能获取进度

## 二、RAID 模块实现的功能

### 1. REBUILDING 状态持久化 ✅

**实现内容**：
- 添加 `RAID_SB_BASE_BDEV_REBUILDING = 4` 状态
- 重建开始时更新 superblock 状态为 `REBUILDING`
- 重建完成时更新为 `CONFIGURED` 或 `FAILED`
- 系统重启后检测 `REBUILDING` 状态并自动恢复重建

**代码位置**：
- `bdev_raid.h`: 枚举定义
- `bdev_raid_sb.c`: `raid_bdev_sb_update_base_bdev_state()` 函数
- `bdev_raid.c`: `raid_bdev_start_rebuild_with_cb()` 函数

### 2. 重建进度持久化 ✅

**实现内容**：
- 在 superblock 中添加 `rebuild_offset` 和 `rebuild_total_size` 字段
- 每个 window 完成后更新进度到 superblock
- 系统重启后从 superblock 恢复重建进度

**代码位置**：
- `bdev_raid.h`: `struct raid_bdev_sb_base_bdev` 结构
- `bdev_raid_sb.c`: `raid_bdev_sb_update_rebuild_progress()` 函数
- `bdev_raid.c`: `raid_bdev_process_window_range_unlocked()` 函数

### 3. 详细进度报告 ✅

**实现内容**：
- 增强 `raid_bdev_write_info_json()` 函数
- 输出详细信息：
  - `target`: 目标磁盘名称
  - `target_slot`: 目标磁盘 slot
  - `current_offset`: 当前重建偏移（块）
  - `total_size`: 总大小（块）
  - `percent`: 百分比
  - `state`: 重建状态（IDLE/READING/WRITING/CALCULATING）

**JSON 输出示例**：
```json
{
  "process": {
    "type": "rebuild",
    "target": "Nvme0n1",
    "target_slot": 2,
    "progress": {
      "current_offset": 1048576,
      "total_size": 104857600,
      "percent": 1.0,
      "state": "reading"
    }
  }
}
```

### 4. 细粒度状态跟踪 ✅

**实现内容**：
- 添加 `enum raid_rebuild_state` 枚举：
  - `RAID_REBUILD_STATE_IDLE`
  - `RAID_REBUILD_STATE_READING`
  - `RAID_REBUILD_STATE_WRITING`
  - `RAID_REBUILD_STATE_CALCULATING` (用于 RAID5F)
- 在 `raid_bdev_process` 结构中添加 `rebuild_state` 字段
- 提供 `raid_rebuild_state_to_str()` 函数

### 5. 进度查询 API ✅

**实现内容**：
- `raid_bdev_get_rebuild_progress()`: 获取重建进度
- `raid_bdev_is_rebuilding()`: 检查是否正在重建

**API 签名**：
```c
int raid_bdev_get_rebuild_progress(struct raid_bdev *raid_bdev, 
                                   uint64_t *current_offset,
                                   uint64_t *total_size);

bool raid_bdev_is_rebuilding(struct raid_bdev *raid_bdev);
```

### 6. 重建完成回调 ✅

**实现内容**：
- 在 `raid_bdev_process` 结构中添加回调字段：
  - `rebuild_done_cb`: 完成回调函数
  - `rebuild_done_ctx`: 回调上下文
- 重建完成时自动调用回调
- 提供 `raid_bdev_start_rebuild_with_cb()` 函数支持回调

**使用示例**：
```c
void my_rebuild_done_cb(void *ctx, int status) {
    if (status == 0) {
        printf("Rebuild completed successfully\n");
    } else {
        printf("Rebuild failed: %s\n", spdk_strerror(-status));
    }
}

raid_bdev_start_rebuild_with_cb(target, my_rebuild_done_cb, my_ctx);
```

## 三、关键实现细节

### 1. Superblock 结构变化

**之前**：
```c
struct raid_bdev_sb_base_bdev {
    // ... 现有字段 ...
    uint8_t reserved[23];
};  // 64 字节
```

**现在**：
```c
struct raid_bdev_sb_base_bdev {
    // ... 现有字段 ...
    uint64_t rebuild_offset;      // 新增：当前重建偏移
    uint64_t rebuild_total_size;  // 新增：总重建大小
    uint8_t reserved[7];          // 减少：从23到7
};  // 80 字节
```

### 2. 进度更新策略

**更新频率**：
- 每个 window 完成后更新一次
- 可以优化为每 N 个 window 更新一次（减少 superblock 写入）

**更新位置**：
- `raid_bdev_process_window_range_unlocked()`: window 完成时

### 3. 状态转换流程

```
启动重建:
  CONFIGURED/FAILED/MISSING → REBUILDING (写入superblock)
  
重建进行中:
  REBUILDING (定期更新进度)
  
重建完成:
  REBUILDING → CONFIGURED (成功) 或 FAILED (失败)
  
系统重启:
  检测到 REBUILDING → 自动恢复重建（从保存的进度继续）
```

### 4. 自动恢复机制

**检测时机**：
- 系统启动时 `raid_bdev_examine()` 检测 superblock
- 发现 `REBUILDING` 状态时自动触发重建恢复

**恢复逻辑**：
- 从 superblock 读取 `rebuild_offset`
- 设置 `process->window_offset = rebuild_offset`
- 从该位置继续重建

## 四、使用示例

### 1. 查询重建进度（RPC）

```bash
# 调用 RPC 获取 RAID bdev 信息
curl -X POST http://localhost:8080 -d '{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "bdev_raid_get_bdevs",
  "params": {
    "category": "all"
  }
}'

# 返回示例：
{
  "result": [
    {
      "name": "raid0",
      "state": "online",
      "process": {
        "type": "rebuild",
        "target": "Nvme0n1",
        "target_slot": 2,
        "progress": {
          "current_offset": 1048576,
          "total_size": 104857600,
          "percent": 1.0,
          "state": "reading"
        }
      }
    }
  ]
}
```

### 2. 程序化查询进度（C API）

```c
struct raid_bdev *raid_bdev = raid_bdev_find_by_name("raid0");
uint64_t current_offset, total_size;

if (raid_bdev_is_rebuilding(raid_bdev)) {
    int rc = raid_bdev_get_rebuild_progress(raid_bdev, &current_offset, &total_size);
    if (rc == 0) {
        double percent = (double)current_offset * 100.0 / (double)total_size;
        printf("Rebuild progress: %.2f%% (%lu / %lu blocks)\n", 
               percent, current_offset, total_size);
    }
}
```

### 3. 使用重建完成回调

```c
void rebuild_complete_cb(void *ctx, int status) {
    if (status == 0) {
        printf("Rebuild completed successfully!\n");
    } else {
        printf("Rebuild failed: %s\n", spdk_strerror(-status));
    }
}

// 启动重建并注册回调
raid_bdev_start_rebuild_with_cb(target_base_info, 
                                 rebuild_complete_cb, 
                                 my_context);
```

## 五、与 EC 模块的对比

| 特性 | EC 模块 | RAID 模块（实现后） | 状态 |
|------|---------|---------------------|------|
| **REBUILDING 状态** | ✅ | ✅ | 已实现 |
| **进度持久化** | ✅ | ✅ | 已实现 |
| **详细进度报告** | ✅ | ✅ | 已实现 |
| **细粒度状态** | ✅ (4个状态) | ✅ (4个状态) | 已实现 |
| **进度查询 API** | ✅ | ✅ | 已实现 |
| **重建完成回调** | ✅ | ✅ | 已实现 |
| **自动恢复** | ✅ | ✅ | 已实现 |

## 六、注意事项

### 1. Superblock 版本兼容性

- **新字段**：`rebuild_offset` 和 `rebuild_total_size` 是新添加的
- **旧版本**：旧版本的 superblock 这些字段为 0（不影响功能）
- **建议**：升级 superblock 版本号（如果需要）

### 2. 性能考虑

- **Superblock 写入频率**：每个 window 完成后写入一次
- **优化建议**：可以改为每 N 个 window 写入一次（减少 I/O）
- **异步写入**：使用 `raid_bdev_write_superblock(raid_bdev, NULL, NULL)` 异步写入

### 3. 状态一致性

- **内存状态**：`process->window_offset` 是实时进度
- **持久化状态**：superblock 中的进度可能略有滞后（每个 window 更新一次）
- **重启恢复**：从 superblock 恢复，可能丢失最后一个 window 的进度（可接受）

## 七、总结

已成功将 EC 模块的故障处理模式应用到 RAID 模块，实现了：

1. ✅ **REBUILDING 状态持久化**：支持中断后恢复
2. ✅ **重建进度持久化**：支持断点续传
3. ✅ **详细进度报告**：JSON 输出，包含完整信息
4. ✅ **细粒度状态跟踪**：4 个状态（IDLE/READING/WRITING/CALCULATING）
5. ✅ **进度查询 API**：程序化查询接口
6. ✅ **重建完成回调**：支持异步通知

**EC 的详细进度报告机制**：
- **不是实时推送**，而是**按需查询**
- 每次调用 RPC 时返回当前进度（从内存读取）
- 进度查询 API 和 JSON 输出共享同一个数据源（`rebuild_ctx`/`process`）
- 重建过程中实时更新内存状态，查询时读取最新值

这些改进使 RAID 模块在故障处理方面达到了与 EC 模块相同的可靠性水平。

