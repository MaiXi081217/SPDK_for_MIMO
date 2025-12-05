# EC加盘框架实现文档

## 一、概述

### 1.1 目标

实现EC（Erasure Code）bdev的加盘功能，支持4种模式：
- **普通模式（NORMAL）**：保持原有行为，num_base_bdevs必须等于k+p（已实现）
- **混合模式（HYBRID）**：加盘后强制重平衡，数据按磨损分布在所有盘上
- **备用盘模式（SPARE）**：多余的盘作为spare，随时可以替换失败的盘
- **扩容模式（EXPAND）**：增加k或p，增加容量（未来功能）

### 1.2 设计原则

1. **向后兼容**：普通模式保持现有行为不变
2. **数据一致性**：重平衡期间保证数据一致性
3. **容错能力**：所有模式都保证容错能力
4. **磨损均衡**：充分利用磨损均衡功能
5. **可扩展性**：框架设计支持未来扩展
6. **稳定、简单、尽量复用**：优先复用SPDK现有的RAID/EC Process框架、rebuild逻辑、设备选择和superblock/RPC实现，只在这些框架上做最小必要扩展，避免从零重新造轮子

### 1.3 参考实现

- **RAID Process框架**：`module/bdev/raid/bdev_raid.c`
- **EC Process框架**：`module/bdev/ec/bdev_ec_process.c`
- **EC Rebuild实现**：`module/bdev/ec/bdev_ec_rebuild.c`
- **EC设备选择**：`module/bdev/ec/bdev_ec_selection.c`

---

## 二、整体架构设计

### 2.1 架构图

```
┌─────────────────────────────────────────────────────────┐
│                    EC Expansion Framework                │
├─────────────────────────────────────────────────────────┤
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │ Normal Mode  │  │ Hybrid Mode   │  │ Spare Mode   │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
│  ┌──────────────────────────────────────────────────┐  │
│  │         Device Selection Layer                    │  │
│  │  - Active/Spare设备管理                           │  │
│  │  - 磨损均衡设备选择                               │  │
│  └──────────────────────────────────────────────────┘  │
│  ┌──────────────────────────────────────────────────┐  │
│  │         Rebalance Framework                      │  │
│  │  - Process框架集成                               │  │
│  │  - 数据迁移                                       │  │
│  └──────────────────────────────────────────────────┘  │
│  ┌──────────────────────────────────────────────────┐  │
│  │         Rebuild Framework                        │  │
│  │  - 各模式下的重建策略                             │  │
│  │  - 重建和重平衡协调                               │  │
│  └──────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

### 2.2 核心组件

1. **模式管理器**：处理不同加盘模式的逻辑
2. **设备状态管理**：管理active/spare设备状态
3. **重平衡框架**：基于Process框架的数据迁移
4. **重建框架**：各模式下的重建策略和协调
5. **设备选择器**：修改设备选择逻辑支持active/spare
6. **Superblock扩展**：持久化模式和设备状态

---

## 三、数据结构设计

### 3.1 模式定义

**文件：`bdev_ec.h`**

```c
enum ec_expansion_mode {
    EC_EXPANSION_MODE_NORMAL = 0,  /* 普通模式：必须等于k+p */
    EC_EXPANSION_MODE_HYBRID = 1,  /* 混合模式：强制重平衡，磨损分布 */
    EC_EXPANSION_MODE_SPARE = 2,   /* 备用盘模式：spare自动替换 */
    EC_EXPANSION_MODE_EXPAND = 3,  /* 扩容模式：增加k/p（未来） */
};

enum ec_rebalance_state {
    EC_REBALANCE_IDLE = 0,
    EC_REBALANCE_RUNNING = 1,
    EC_REBALANCE_PAUSED = 2,
    EC_REBALANCE_COMPLETED = 3,
    EC_REBALANCE_FAILED = 4,
};
```

### 3.2 扩展 ec_base_bdev_info

```c
struct ec_base_bdev_info {
    // ... 现有字段 ...
    bool is_spare;      /* 是否为spare盘 */
    bool is_active;     /* 是否参与数据存储 */
    bool was_active_before_expansion;  /* 加盘前的状态（用于重平衡） */
};
```

### 3.3 扩展 ec_bdev

```c
struct ec_bdev {
    // ... 现有字段 ...
    enum ec_expansion_mode expansion_mode;
    enum ec_rebalance_state rebalance_state;
    struct ec_rebalance_context *rebalance_ctx;
    uint8_t num_active_bdevs_before_expansion;
    uint8_t num_active_bdevs_after_expansion;
    bool rebalance_required;
    uint8_t rebalance_progress;
};
```

### 3.4 重平衡上下文

```c
struct ec_rebalance_context {
    struct ec_bdev *ec_bdev;
    uint64_t current_stripe;
    uint64_t total_stripes;
    uint8_t old_active_indices[EC_MAX_BASE_BDEVS];
    uint8_t num_old_active;
    uint8_t new_active_indices[EC_MAX_BASE_BDEVS];
    uint8_t num_new_active;
    void (*done_cb)(void *ctx, int status);
    void *done_ctx;
    bool paused;
    int error_status;
};
```

### 3.5 Process类型扩展

```c
enum ec_process_type {
    EC_PROCESS_NONE,
    EC_PROCESS_REBUILD,
    EC_PROCESS_REBALANCE,  /* 新增 */
    EC_PROCESS_MAX
};
```

### 3.6 Superblock扩展

在superblock中添加：
- `expansion_mode`：加盘模式
- `rebalance_state`：重平衡状态
- `rebalance_progress`：重平衡进度

---

## 四、核心功能实现

### 4.1 加盘入口函数

**文件：`bdev_ec.c`**

```c
int ec_bdev_add_base_bdev_with_mode(struct ec_bdev *ec_bdev,
                                     const char *base_bdev_name,
                                     enum ec_expansion_mode mode,
                                     ec_base_bdev_cb cb_fn,
                                     void *cb_ctx)
{
    int rc = ec_bdev_add_base_bdev(ec_bdev, base_bdev_name, cb_fn, cb_ctx);
    if (rc != 0) {
        return rc;
    }
    
    if (num_base_bdevs + 1 > required_bdevs) {
        switch (mode) {
        case EC_EXPANSION_MODE_NORMAL:
            return -EINVAL;  /* 不允许超过k+p */
        case EC_EXPANSION_MODE_HYBRID:
            return ec_bdev_handle_hybrid_mode(ec_bdev);
        case EC_EXPANSION_MODE_SPARE:
            return ec_bdev_handle_spare_mode(ec_bdev);
        case EC_EXPANSION_MODE_EXPAND:
            return ec_bdev_handle_expand_mode(ec_bdev);
        }
    }
    
    return 0;
}
```

### 4.2 混合模式实现

**关键逻辑：**
1. 记录加盘前的active设备
2. 激活所有设备
3. 强制启动重平衡

```c
static int ec_bdev_handle_hybrid_mode(struct ec_bdev *ec_bdev)
{
    // 1. 记录加盘前的active设备
    // 2. 激活所有设备
    // 3. 启动重平衡
    return ec_bdev_start_rebalance(ec_bdev, NULL, NULL);
}
```

### 4.3 备用盘模式实现

**关键逻辑：**
1. 根据磨损选择k+p个设备作为active
2. 多余的标记为spare
3. 设备失败时自动用spare替换

```c
static int ec_bdev_handle_spare_mode(struct ec_bdev *ec_bdev)
{
    // 1. 根据磨损选择k+p个active设备
    // 2. 标记spare设备
    // 3. 更新superblock
    return 0;
}
```

### 4.4 设备选择逻辑修改

**文件：`bdev_ec_selection.c`**

修改设备选择逻辑，只从active设备中选择：

```c
EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
    if (base_info->desc != NULL && 
        !base_info->is_failed && 
        base_info->is_active) {  /* 只选择active设备 */
        candidates[num_candidates++] = device_idx;
    }
}
```

### 4.5 自动替换逻辑

**文件：`bdev_ec.c`**

```c
void ec_bdev_fail_base_bdev(struct ec_base_bdev_info *base_info)
{
    base_info->is_failed = true;
    
    if (base_info->is_active && 
        ec_bdev->expansion_mode == EC_EXPANSION_MODE_SPARE) {
        struct ec_base_bdev_info *spare = ec_bdev_find_spare_device(ec_bdev);
        if (spare != NULL) {
            spare->is_active = true;
            spare->is_spare = false;
            ec_bdev_start_rebuild(ec_bdev, base_info, spare, NULL, NULL);
        }
    }
}
```

---

## 五、重建框架设计

### 5.1 各模式下的重建策略

#### 普通模式（NORMAL）
- 设备失败后等待新设备，自动启动重建
- 重建期间不允许加盘/删盘操作

#### 备用盘模式（SPARE）
- Active设备失败时，自动用spare设备替换并启动重建
- 重建期间可以继续加盘（添加新的spare）

#### 混合模式（HYBRID）
- 所有设备都是active，重建目标可以是任意设备
- 重建期间可以继续加盘（但延迟重平衡）
- 重建和重平衡不能同时进行

### 5.2 重建过程中的操作处理

| 操作 | 普通模式 | 备用盘模式 | 混合模式 |
|------|---------|-----------|---------|
| **重建期间加盘** | 拒绝（-EBUSY） | 允许（添加spare） | 允许（延迟重平衡） |
| **重建期间删盘** | 检查设备数量 | 检查设备数量 | 检查设备数量 |
| **重建期间设备失败** | 检查可用设备 | 检查可用设备/spare | 检查可用设备 |
| **重建暂停/恢复** | 支持 | 支持 | 支持 |
| **重建取消** | 支持 | 支持 | 支持 |

### 5.3 重建和设备选择的交互

- **重建期间**：设备选择需要排除重建目标设备
- **重建完成后**：
  - 备用盘模式：spare设备变成active设备
  - 混合模式：可能需要启动重平衡（如果之前延迟了）

### 5.4 重建和重平衡的协调

**原则：** 重建优先级高于重平衡

**处理策略：**
- 重平衡期间设备失败 → 暂停重平衡，启动重建
- 重建期间加盘（混合模式）→ 延迟重平衡
- 重建完成后 → 自动启动重平衡（如果之前延迟了）

---

## 六、重平衡框架实现（完整设计方案）

### 6.1 核心设计原则

**设计目标：**
1. **精简稳定**：最小化数据结构，最大化复用现有机制
2. **完全兼容**：不破坏现有功能，完全复用profile和cache机制
3. **自动工作**：读取时自动选择正确设备，零维护成本
4. **数据一致性**：保证重平衡后读取的数据正确性

**核心机制：**
- **复用Profile机制**：利用现有的`group_profile_map`和`wear_profiles`
- **复用Cache机制**：利用现有的`assignment_cache`失效机制
- **Window机制**：复用Process框架的window锁定机制
- **统一重新编码**：数据迁移时统一重新编码，保证数据一致性

### 6.2 精简的数据结构

**文件：`bdev_ec_internal.h`**

```c
/* 精简的 rebalance_ctx - 只保留必要字段 */
struct ec_rebalance_context {
    /* 基础信息 */
    struct ec_bdev *ec_bdev;
    void (*done_cb)(void *ctx, int status);
    void *done_ctx;
    
    /* 进度跟踪（最小化） */
    uint64_t current_stripe;
    uint64_t total_stripes;
    
    /* Profile管理（核心机制） */
    uint16_t old_active_profile_id;  /* 重平衡前的active profile */
    uint16_t new_active_profile_id;  /* 重平衡后的active profile（新创建） */
    
    /* 错误处理（简化） */
    int error_status;
};
```

**设计说明：**
- 不需要`old_active_indices`/`new_active_indices`（可以从ec_bdev获取）
- 不需要`migrated_stripe_bitmap`（利用assignment_cache失效）
- 不需要`rebalance_version`（profile即版本）
- 不需要`paused`（使用rebalance_state）

### 6.3 重平衡启动流程

**文件：`bdev_ec.c`**

```c
int ec_bdev_start_rebalance(struct ec_bdev *ec_bdev,
                            void (*done_cb)(void *ctx, int status),
                            void *done_ctx)
{
    struct ec_device_selection_config *config = &ec_bdev->selection_config;
    struct ec_rebalance_context *rebalance_ctx;
    uint64_t total_stripes;
    
    // 1. 检查状态
    if (rebalance_state == RUNNING || rebuild in progress) {
        return -EBUSY;
    }
    
    // 2. 分配rebalance_ctx（最小结构）
    rebalance_ctx = calloc(1, sizeof(*rebalance_ctx));
    
    // 3. 【关键】创建新profile（使用当前磨损级别，包含新设备）
    uint16_t new_profile_id = config->next_profile_id++;
    struct ec_wear_profile_slot *new_profile = 
        ec_selection_ensure_profile_slot(config, new_profile_id);
    
    // 收集当前磨损级别（包含新设备）
    uint32_t current_wear_levels[EC_MAX_BASE_BDEVS];
    ec_selection_collect_device_wear(ec_bdev, current_wear_levels);
    
    // 更新新profile的磨损级别和权重
    memcpy(new_profile->wear_levels, current_wear_levels, sizeof(current_wear_levels));
    ec_selection_compute_weights_from_levels(ec_bdev->num_base_bdevs,
                                             current_wear_levels,
                                             new_profile->wear_weights);
    
    // 4. 保存旧的active profile，设置新的active profile
    rebalance_ctx->old_active_profile_id = config->active_profile_id;
    rebalance_ctx->new_active_profile_id = new_profile_id;
    config->active_profile_id = new_profile_id;
    
    // 5. 计算总stripe数
    total_stripes = ec_bdev->bdev.blockcnt / (ec_bdev->strip_size * ec_bdev->k);
    rebalance_ctx->total_stripes = total_stripes;
    rebalance_ctx->current_stripe = 0;
    
    // 6. 【关键】清除所有assignment_cache（强制重新计算）
    ec_assignment_cache_reset(&config->assignment_cache);
    ec_assignment_cache_init(&config->assignment_cache, 
                            EC_ASSIGNMENT_CACHE_DEFAULT_CAPACITY);
    
    // 7. 【关键】绑定所有未绑定的group到旧profile（确保读取正确）
    // 遍历所有group，如果group_profile_map[group_id] = 0，绑定到old_active_profile_id
    if (config->group_profile_map != NULL && config->num_stripe_groups > 0) {
        uint32_t group_id;
        for (group_id = 0; group_id < config->num_stripe_groups; group_id++) {
            if (config->group_profile_map[group_id] == 0) {
                // 未绑定的group，绑定到旧profile（数据在旧设备）
                config->group_profile_map[group_id] = rebalance_ctx->old_active_profile_id;
                ec_selection_mark_group_dirty(ec_bdev);
            }
        }
    }
    
    // 8. 更新ec_bdev状态
    ec_bdev->rebalance_ctx = rebalance_ctx;
    ec_bdev->rebalance_state = EC_REBALANCE_RUNNING;
    ec_bdev->rebalance_progress = 0;
    
    // 9. 启动process（target为NULL）
    return ec_bdev_start_process(ec_bdev, EC_PROCESS_REBALANCE, NULL,
                                ec_bdev_rebalance_done_cb, rebalance_ctx);
}
```

**关键点：**
- 创建新profile，包含新设备的磨损级别
- 设置新的active_profile_id（新写入使用新profile）
- 清除所有cache（强制重新计算设备选择）

### 6.4 Stripe迁移流程（核心实现）

**文件：`bdev_ec_process.c`**

```c
static int ec_bdev_rebalance_migrate_stripe(struct ec_bdev_process *process,
                                            uint64_t stripe_index)
{
    struct ec_bdev *ec_bdev = process->ec_bdev;
    struct ec_rebalance_context *rebalance_ctx = ec_bdev->rebalance_ctx;
    struct ec_device_selection_config *config = &ec_bdev->selection_config;
    uint8_t old_data_indices[EC_MAX_K], old_parity_indices[EC_MAX_P];
    uint8_t new_data_indices[EC_MAX_K], new_parity_indices[EC_MAX_P];
    unsigned char *stripe_buf;  /* k个数据块 */
    unsigned char *parity_bufs[EC_MAX_P];  /* p个校验块 */
    uint32_t strip_size_bytes = ec_bdev->strip_size * ec_bdev->bdev.blocklen;
    uint32_t group_id;
    int rc;
    
    // 计算group_id
    group_id = ec_selection_calculate_group_id(config, stripe_index);
    
    // ========== 步骤1: 从旧设备读取数据 ==========
    // 【关键】获取旧profile（通过group_profile_map，如果未绑定则使用old_active_profile_id）
    uint16_t old_profile_id;
    if (config->group_profile_map != NULL && 
        group_id < config->group_profile_capacity &&
        config->group_profile_map[group_id] != 0) {
        // Group已绑定，使用绑定的profile
        old_profile_id = config->group_profile_map[group_id];
    } else {
        // Group未绑定，使用重平衡前的active profile
        old_profile_id = rebalance_ctx->old_active_profile_id;
    }
    
    // 临时修改active_profile_id以获取旧profile slot
    uint16_t saved_active = config->active_profile_id;
    config->active_profile_id = old_profile_id;
    
    // 获取旧profile slot
    struct ec_wear_profile_slot *old_profile = 
        ec_selection_get_profile_slot_for_stripe(ec_bdev, stripe_index);
    
    // 使用旧profile选择设备
    ec_select_base_bdevs_wear_leveling(ec_bdev, stripe_index,
                                      old_data_indices, old_parity_indices);
    
    // 恢复active_profile
    config->active_profile_id = saved_active;
    
    // 分配缓冲区
    stripe_buf = malloc(strip_size_bytes * ec_bdev->k);
    for (i = 0; i < ec_bdev->p; i++) {
        parity_bufs[i] = malloc(strip_size_bytes);
    }
    
    // 并行读取k个数据块
    for (i = 0; i < ec_bdev->k; i++) {
        idx = old_data_indices[i];
        pd_lba = (stripe_index << ec_bdev->strip_size_shift) +
                 ec_bdev->base_bdev_info[idx].data_offset;
        spdk_bdev_read_blocks(ec_bdev->base_bdev_info[idx].desc,
                             base_channel[idx],
                             stripe_buf + i * strip_size_bytes,
                             pd_lba, ec_bdev->strip_size,
                             ec_bdev_rebalance_read_complete, process_req);
    }
    // 等待所有读取完成（在回调中处理）
    
    // ========== 步骤2: 确定新设备选择 ==========
    // 使用新profile（已设置）选择设备
    ec_select_base_bdevs_wear_leveling(ec_bdev, stripe_index,
                                      new_data_indices, new_parity_indices);
    
    // ========== 步骤3: 重新编码生成新校验块 ==========
    // 准备数据指针
    unsigned char *data_ptrs[EC_MAX_K];
    for (i = 0; i < ec_bdev->k; i++) {
        data_ptrs[i] = stripe_buf + i * strip_size_bytes;
    }
    
    // 【关键】重新编码（必须，不能直接复制）
    rc = ec_encode_stripe(ec_bdev, data_ptrs, parity_bufs, strip_size_bytes);
    if (rc != 0) {
        // 错误处理
        return rc;
    }
    
    // ========== 步骤4: 写入新设备 ==========
    // 并行写入k个数据块
    for (i = 0; i < ec_bdev->k; i++) {
        idx = new_data_indices[i];
        pd_lba = (stripe_index << ec_bdev->strip_size_shift) +
                 ec_bdev->base_bdev_info[idx].data_offset;
        spdk_bdev_write_blocks(ec_bdev->base_bdev_info[idx].desc,
                              base_channel[idx],
                              data_ptrs[i],
                              pd_lba, ec_bdev->strip_size,
                              ec_bdev_rebalance_write_complete, process_req);
    }
    // 并行写入p个校验块
    for (i = 0; i < ec_bdev->p; i++) {
        idx = new_parity_indices[i];
        pd_lba = (stripe_index << ec_bdev->strip_size_shift) +
                 ec_bdev->base_bdev_info[idx].data_offset;
        spdk_bdev_write_blocks(ec_bdev->base_bdev_info[idx].desc,
                              base_channel[idx],
                              parity_bufs[i],
                              pd_lba, ec_bdev->strip_size,
                              ec_bdev_rebalance_write_complete, process_req);
    }
    // 等待所有写入完成（在回调中处理）
    
    // ========== 步骤5: 强制验证 ==========
    // 从新设备读取刚写入的数据
    // 使用EC解码验证数据完整性
    // 如果验证失败，标记错误并重试
    
    // ========== 步骤6: 更新group_profile_map ==========
    // 【关键】绑定group到新profile
    if (config->group_profile_map != NULL && 
        group_id < config->group_profile_capacity) {
        config->group_profile_map[group_id] = rebalance_ctx->new_active_profile_id;
        ec_selection_mark_group_dirty(ec_bdev);  // 标记需要持久化
    }
    
    // ========== 步骤7: 清除该stripe的cache ==========
    // 【关键】清除cache，强制下次读取时重新计算
    ec_assignment_cache_invalidate_stripe(config, stripe_index);
    
    return 0;
}
```

**关键点：**
1. **必须重新编码**：设备选择变化时，校验块必须重新计算，不能直接复制
2. **强制验证**：写入后立即读取验证，保证数据完整性
3. **更新profile绑定**：迁移后更新`group_profile_map`，绑定到新profile
4. **清除cache**：清除该stripe的cache，强制重新计算设备选择

### 6.5 读取时的自动选择（完全自动）

**文件：`bdev_ec_selection.c`（无需修改，现有机制自动工作）**

```
读取stripe时的流程（完全自动）：

1. 计算group_id = stripe_index / group_size

2. 查找group_profile_map[group_id]：
   - 如果 = new_profile_id → 使用新profile → 新设备 ✓
   - 如果 = old_profile_id → 使用旧profile → 旧设备 ✓
   - 如果 = 0 → 使用active_profile（新profile）→ 新设备 ✓

3. 使用profile的wear_weights选择设备

4. 从选择的设备读取数据

关键：
- 已迁移的group：group_profile_map已更新 → 新profile → 新设备 ✓
- 未迁移的group：group_profile_map未更新 → 旧profile → 旧设备 ✓
- 完全自动，无需额外检查
```

### 6.6 Window机制和I/O并发

**Window机制：**
```
┌─────────────────────────────────────────────────┐
│  EC Bdev 总容量                                  │
├─────────────────────────────────────────────────┤
│  Window 1  │  Window 2  │  Window 3  │  ...   │
│  [锁定]     │  [未锁定]   │  [未锁定]   │        │
└─────────────────────────────────────────────────┘
```

**I/O行为：**
- **锁定范围**：正在迁移的window，该范围的I/O等待迁移完成
- **未锁定范围**：其他window，正常I/O可以继续
- **窗口大小**：默认1024KB（可配置），平衡并发与等待时间

**实现：**
```c
// 锁定window
spdk_bdev_quiesce_range(&ec_bdev->bdev, &g_ec_if,
                        window_offset, window_size,
                        ec_bdev_process_window_range_locked, process);

// 迁移完成后解锁
spdk_bdev_unquiesce_range(&ec_bdev->bdev, &g_ec_if,
                          window_offset, window_size,
                          ec_bdev_process_window_range_unlocked, process);
```

### 6.7 数据完整性保证

**三层保证机制：**

1. **EC校验块验证（自动）**
   - 每个stripe包含k个数据块和p个校验块
   - 写入后，校验块自动验证数据块的正确性
   - 如果数据损坏，读取时会通过EC解码检测到

2. **写入后强制验证（必须）**
   - 每个stripe写入后立即读取验证
   - 使用EC解码验证数据完整性
   - 如果验证失败，标记错误并重试

3. **Profile机制保证（自动）**
   - 已迁移的stripe：绑定到新profile → 自动从新设备读取 ✓
   - 未迁移的stripe：绑定到旧profile → 自动从旧设备读取 ✓
   - 完全自动，不会读错设备

### 6.8 重平衡期间的操作处理

| 操作 | 处理策略 |
|------|---------|
| **重平衡期间加盘** | 拒绝（-EBUSY） |
| **重平衡期间删盘** | 检查设备数量（>= k+p） |
| **重平衡期间设备失败** | 暂停重平衡，启动重建 |
| **重平衡期间正常I/O** | 允许（Window机制保证一致性） |
| **重平衡暂停/恢复** | 支持 |
| **重平衡取消** | 支持 |

### 6.9 完整的数据流转

```
重平衡前：
  Group 0 → Profile 1（旧磨损级别，旧设备）
  Group 1 → Profile 1

重平衡时：
  1. 创建 Profile 2（新磨损级别，包含新设备）
  2. 设置 active_profile_id = 2
  3. 迁移 Group 0：
     - 从旧设备读取（使用Profile 1）
     - 重新编码生成新校验块
     - 写入新设备（使用Profile 2）
     - 验证数据完整性
     - 更新 group_profile_map[0] = 2
     - 清除 assignment_cache[stripe_index]
  4. Group 1 还没迁移：group_profile_map[1] = 1（保持不变）

读取时：
  - Group 0：查map → Profile 2 → 新设备 ✓
  - Group 1：查map → Profile 1 → 旧设备 ✓
  - 完全自动，不会读错设备 ✓
```

### 6.10 关键优势总结

1. **完全复用现有机制**
   - ✅ 使用现有的`group_profile_map`
   - ✅ 使用现有的`wear_profiles`
   - ✅ 使用现有的`assignment_cache`
   - ✅ 不需要额外的数据结构

2. **不破坏任何功能**
   - ✅ 未迁移的group：继续使用旧profile → 旧设备 ✓
   - ✅ 已迁移的group：使用新profile → 新设备 ✓
   - ✅ 新写入：使用新profile → 新设备 ✓
   - ✅ 完全兼容现有机制

3. **自动工作，零维护**
   - ✅ 读取时自动查找profile
   - ✅ 自动选择正确设备
   - ✅ 不需要额外检查
   - ✅ 零维护成本

4. **精简稳定**
   - ✅ 最小化数据结构（只有6个字段）
   - ✅ 最大化复用现有机制
   - ✅ 减少状态管理
   - ✅ 简化错误处理

---

## 七、Superblock扩展

### 7.1 保存模式信息

```c
void ec_bdev_sb_save_expansion_metadata(struct ec_bdev *ec_bdev)
{
    sb->expansion_mode = (uint32_t)ec_bdev->expansion_mode;
    sb->rebalance_state = (uint32_t)ec_bdev->rebalance_state;
    sb->rebalance_progress = ec_bdev->rebalance_progress;
    ec_bdev_sb_update_crc(sb);
}
```

### 7.2 加载模式信息

```c
void ec_bdev_sb_load_expansion_metadata(struct ec_bdev *ec_bdev,
                                        const struct ec_bdev_superblock *sb)
{
    ec_bdev->expansion_mode = (enum ec_expansion_mode)sb->expansion_mode;
    ec_bdev->rebalance_state = (enum ec_rebalance_state)sb->rebalance_state;
    ec_bdev->rebalance_progress = sb->rebalance_progress;
    
    // 根据模式恢复设备状态
    if (ec_bdev->expansion_mode == EC_EXPANSION_MODE_SPARE) {
        ec_bdev_restore_spare_devices(ec_bdev);
    }
}
```

---

## 八、RPC接口设计

### 8.1 创建EC（支持加盘模式）

```c
struct rpc_bdev_ec_create {
    // ... 现有字段 ...
    enum ec_expansion_mode expansion_mode;  // 新增
};
```

### 8.2 添加base bdev（支持加盘模式）

```c
struct rpc_bdev_ec_add_base_bdev {
    char *name;
    char *base_bdev_name;
    enum ec_expansion_mode expansion_mode;  // 新增
};
```

### 8.3 查询重平衡状态

```c
rpc_bdev_ec_get_rebalance_status()
// 返回：rebalance_state, rebalance_progress, current_stripe, total_stripes
```

---

## 九、边界情况和错误处理

### 9.1 多设备同时失败

**处理策略：**
- 重建期间：检查可用设备数量，如果 >= k 继续重建，否则失败
- 重平衡期间：暂停重平衡，检查设备数量，启动重建或失败

### 9.2 重建和重平衡同时进行

**原则：** 不能同时进行，重建优先

**实现：**
- 重平衡期间设备失败 → 暂停重平衡，启动重建
- 重建期间启动重平衡 → 延迟重平衡（`rebalance_required = true`）

### 9.3 系统重启后的状态恢复

**处理策略：**
1. 从superblock读取状态
2. 检查设备状态
3. 决定是否继续或重新开始

---

## 十、实现步骤

### 阶段1：基础框架（1周）
- 数据结构扩展
- 基础函数实现
- 设备选择修改

### 阶段2：备用盘模式（1周）
- Spare模式实现
- 自动替换逻辑
- Superblock集成

### 阶段3：混合模式（2-3周）
- 混合模式实现
- 重平衡框架
- Process框架集成
- 进度跟踪

### 阶段4：测试和优化（1-2周）
- 单元测试
- 集成测试
- 性能测试

### 阶段5：扩容模式（未来，6-8周）
- k/p变更逻辑
- 容量重新计算
- 数据重新分布

---

## 十一、测试计划

### 11.1 单元测试
- 备用盘模式：spare设备选择、自动替换、rebuild流程
- 混合模式：设备激活、重平衡启动、数据迁移
- 设备选择：active设备选择、磨损均衡选择、容错保证

### 11.2 集成测试
- 端到端测试：创建EC → 加盘 → 重平衡 → 验证数据
- 数据一致性测试：重平衡期间数据读写、数据验证
- 性能测试：重平衡速度、对正常I/O的影响

### 11.3 重建场景测试

#### 普通模式
- 基本重建流程
- 重建期间操作（加盘/删盘/设备失败）
- 重建中断恢复

#### 备用盘模式
- 自动替换测试
- 重建期间操作
- 多设备失败

#### 混合模式
- 重建和重平衡协调
- 重建期间操作
- 边界情况

### 11.4 压力测试
- 并发测试：多个EC同时重平衡/重建
- 错误注入测试：各种失败场景
- 长时间运行测试

---

## 十二、注意事项

### 12.1 数据一致性
- 重平衡期间：使用window机制锁定LBA范围
- 设备选择：使用确定性算法保证读写一致性

### 12.2 性能考虑
- 重平衡性能：使用Process框架的window机制，支持QoS限制
- 设备选择性能：使用缓存加速查询

### 12.3 错误处理
- 重平衡错误：记录错误状态，支持暂停/恢复/取消
- 重建错误：记录错误状态，支持暂停/恢复/取消
- 设备失败：自动替换（spare模式）或降级运行
- 并发错误：重建和重平衡不能同时进行

### 12.4 向后兼容
- Superblock版本：保持向后兼容，支持版本升级
- RPC接口：默认使用普通模式，可选参数支持新模式

---

## 十三、调试与可观测性设计

### 13.1 日志输出规范

1. **等级与用途**
   - **`SPDK_NOTICELOG`**：模式切换、重建/重平衡开始与结束、自动替换成功等关键“正常事件”
   - **`SPDK_WARNLOG`**：功能仍可继续，但存在风险或退化（例如：无可用spare、降级运行）
   - **`SPDK_ERRLOG`**：导致操作失败、状态回滚或需要人工介入的问题

2. **前缀约定（便于 grep）**
   - `EC[expansion]`：加盘/模式切换相关
   - `EC[spare]`：备用盘模式与自动替换相关
   - `EC[hybrid]`：混合模式与强制重平衡相关
   - `EC[rebalance]`：重平衡流程相关
   - `EC[rebuild]`：重建流程相关

3. **典型日志点**
   - **Spare 模式**
     - 加盘：`NOTICE EC[expansion] add base bdev '%s' to %s, mode=%s`
     - 角色分配：`NOTICE EC[spare] active=%u, spare=%u (k=%u, p=%u)`
     - 设备失败：`ERR EC[spare] device '%s' failed (slot=%u)`
     - 自动替换成功：`NOTICE EC[spare] replace failed '%s'(slot=%u) with spare '%s'(slot=%u)`
     - 无可用 spare：`WARN EC[spare] no spare available for failed '%s'(slot=%u)`
   - **Hybrid / 重平衡**
     - 启用混合模式：`NOTICE EC[hybrid] enable hybrid mode on %s: active_before=%u, active_after=%u`
     - 重平衡开始：`NOTICE EC[rebalance] start on %s: total_stripes=%lu`
     - 重平衡迁移失败：`ERR EC[rebalance] stripe=%lu migrate failed: %s`
     - 重平衡完成：`NOTICE EC[rebalance] completed on %s, moved_stripes=%lu`
   - **冲突与边界**
     - 重建中拒绝加盘（NORMAL）：  
       `ERR EC[expansion] cannot add '%s' to %s: rebuild in progress (mode=NORMAL, progress=%u%%)`
     - 重平衡中拒绝加盘（HYBRID）：  
       `ERR EC[hybrid] cannot add '%s' to %s: rebalance in progress (%u%%)`
     - 重平衡中设备失败：  
       `WARN EC[rebalance] pause on %s: device '%s' failed, starting rebuild`
   - **重建**
     - 启动：`NOTICE EC[rebuild] start on %s, target='%s'(slot=%u)`
     - 完成：`NOTICE EC[rebuild] completed on %s, target='%s' status=0`
     - 失败：`ERR EC[rebuild] failed on %s, target='%s' status=%d`

通过统一前缀和信息字段，开发和运维可以直接用 `grep "EC["` 快速定位到 EC 相关事件，并从单条日志里看出：哪个 bdev、哪个模式、哪个 base bdev、当前大致进度/状态。

### 13.2 JSON / RPC 状态观测

为避免“反复写测试碰状态”，需要在运行时通过 RPC 直接看到当前 EC 的关键状态。实现方式：

1. **扩展现有 EC info / stats RPC，或新增 `bdev_ec_get_status`（推荐）**
   - 返回字段示例：
   ```json
   {
     "name": "EC0",
     "k": 6,
     "p": 2,
     "expansion_mode": "HYBRID",
     "base_bdevs": [
       { "name": "Nvme0n1", "slot": 0, "role": "active", "state": "OK" },
       { "name": "Nvme0n2", "slot": 1, "role": "spare",  "state": "OK" }
     ],
     "rebuild": {
       "state": "RUNNING",
       "progress": 37,
       "target": "Nvme0n3",
       "last_error": ""
     },
     "rebalance": {
       "state": "PAUSED",
       "progress": 52,
       "current_stripe": 1234,
       "total_stripes": 4096,
       "last_error": "device failed"
     },
     "last_failure": {
       "reason": "no spare available",
       "base_bdev": "Nvme0n4"
     }
   }
   ```

2. **关键 JSON 字段规划**
   - **基础信息**
     - `expansion_mode`：`NORMAL` / `SPARE` / `HYBRID` / `EXPAND`
     - `base_bdevs[i].role`：`active` / `spare` / `failed`
   - **重建状态**
     - `rebuild.state`：`IDLE` / `RUNNING` / `PAUSED` / `COMPLETED` / `FAILED`
     - `rebuild.progress`：0–100
     - `rebuild.target`：当前重建目标 base bdev 名字
     - `rebuild.last_error`：最近一次失败原因（字符串）
   - **重平衡状态**
     - `rebalance.state`：`IDLE` / `RUNNING` / `PAUSED` / `COMPLETED` / `FAILED`
     - `rebalance.progress`：0–100
     - `rebalance.current_stripe` / `rebalance.total_stripes`
     - `rebalance.last_error`
   - **最近一次失败**
     - `last_failure.reason`
     - `last_failure.base_bdev`
     - `last_failure.timestamp`（可选）

3. **使用方式**
   - **日常开发调试**：  
     - 看 log：定位是哪一类事件/错误（前缀 + 详细字段）  
     - 调一次状态 RPC：看到全局状态（模式、active/spare 分布、重建/重平衡进度与最后错误），避免写大量“探测用”测试。
   - **运维排障**：  
     - 出问题时导出当前 EC 状态 JSON + 日志片段即可分析，不需要重现复杂场景。

这一节作为实现约束，编码时所有新功能都应满足：**关键路径上至少有一条清晰的日志，且能通过 RPC 看到当前状态和最近一次错误原因。**

---

## 十四、总结

### 14.1 实现工作量
- **普通模式**：~10行（仅参数）
- **备用盘模式**：~230行
- **混合模式**：~910行
- **扩容模式**：~1500行（未来）

**总计待实现：** ~1150行代码（不包括扩容模式）

### 14.2 实现优先级
1. 阶段1：基础框架（1周）
2. 阶段2：备用盘模式（1周）
3. 阶段3：混合模式（2-3周）
4. 阶段4：测试和优化（1-2周）
5. 阶段5：扩容模式（未来，6-8周）

### 14.3 关键风险
1. 重平衡数据一致性：需要仔细设计window机制
2. 重平衡性能：需要优化I/O路径
3. 错误恢复：需要完善的错误处理机制
4. 重建和重平衡协调：需要避免资源竞争和状态混乱
5. 多设备失败处理：需要完善的降级和恢复机制
6. 系统重启恢复：需要可靠的状态持久化和恢复

### 14.4 成功标准
1. 功能完整性：所有模式正常工作
2. 数据一致性：重平衡期间数据一致
3. 性能影响：重平衡对正常I/O影响小
4. 稳定性：长时间运行稳定

---

## 十五、当前实现进度（代码状态对照）

### 15.1 已完成（并保持原有功能兼容）

1. **数据结构骨架**
   - `enum ec_expansion_mode` / `enum ec_rebalance_state` 已在 `bdev_ec.h` 定义
   - `struct ec_base_bdev_info` 增加 `is_spare` / `is_active` / `was_active_before_expansion`
   - `struct ec_bdev` 增加：
     - `expansion_mode`
     - `rebalance_state`
     - `rebalance_ctx`
     - `num_active_bdevs_before_expansion` / `num_active_bdevs_after_expansion`
     - `rebalance_required` / `rebalance_progress`
   - `enum ec_process_type` 增加 `EC_PROCESS_REBALANCE`（仅作为骨架，尚未接逻辑）

2. **默认设备选择与磨损均衡选择与 active 标记打通**
   - `ec_select_base_bdevs_default()`（`bdev_ec_io.c`）：
     - 在有盘被标记为 `is_active == true` 时，仅从 active 盘中选择
     - 若没有任何盘被标记为 active，则维持旧行为：所有非失败盘参与选择
   - `ec_select_base_bdevs_wear_leveling()`（`bdev_ec_selection.c`）：
     - 生成候选列表时，同样在有 active 标记时仅选 active 盘
     - 无 active 标记时行为与旧版本一致

3. **RPC 创建接口扩展（支持 SPARE 模式）**
   - `rpc_bdev_ec_create`（`bdev_ec_rpc.c`）：
     - 新增参数 `expansion_mode`（可选，int32 → `enum ec_expansion_mode`，默认 `NORMAL`）
     - 参数校验：
       - `NORMAL` 及其他模式（HYBRID/EXPAND，当前未实现）：`num_base_bdevs == k + p`
       - `SPARE`：`num_base_bdevs >= k + p`
   - 创建完成后，在 `rpc_bdev_ec_create_add_base_bdev_cb()` 中根据 `expansion_mode` 设置并同步 superblock：
     - **NORMAL / 其他模式（当前当 NORMAL 处理）**：
       - 所有健康 base bdev：`is_active = true`, `is_spare = false`
       - 如果启用 superblock，则对应 slot 的 state 写入 `EC_SB_BASE_BDEV_CONFIGURED`
       - `expansion_mode = EC_EXPANSION_MODE_NORMAL`
       - `num_active_bdevs_before_expansion == num_active_bdevs_after_expansion == k + p`
     - **SPARE 模式**：
       - 按 base_bdev_info 顺序，前 `k+p` 个健康盘：
         - `is_active = true`, `is_spare = false`
         - superblock 中写 `EC_SB_BASE_BDEV_CONFIGURED`
       - 其余健康盘：
         - `is_active = false`, `is_spare = true`
         - superblock 中写 `EC_SB_BASE_BDEV_SPARE`
       - 完成后统一 `ec_bdev_write_superblock()` 落盘当前 active/spare 状态
       - 更新：
         - `expansion_mode = EC_EXPANSION_MODE_SPARE`
         - `num_active_bdevs_before_expansion = k + p`
         - `num_active_bdevs_after_expansion = 实际 active 数（通常 = k + p）`
       - 输出日志：
         - `EC[spare] EC bdev %s created in SPARE mode: active=%u, spare=%zu (k=%u, p=%u)`

4. **扩展 API 骨架（不改变现有调用路径）**
   - `ec_bdev_add_base_bdev_with_mode()`（`bdev_ec.c`）：
     - 当前仅记录一条 `SPDK_DEBUGLOG`，然后直接调用原有 `ec_bdev_add_base_bdev()`，行为完全一致
   - `ec_bdev_start_rebalance()`（`bdev_ec.c`）：
     - 当前为占位实现，输出一条 `EC[rebalance] ... skeleton only` 的 `NOTICE`，返回 `-ENOTSUP`
     - 不在任何现有流程中自动调用

5. **JSON输出增强（已完成 ✅）**
   - `ec_bdev_write_info_json()`（`bdev_ec.c`）：
     - **Rebuild状态输出**：同时支持legacy rebuild_ctx和process框架的rebuild
       - 自动检测使用哪种框架（优先process框架）
       - 输出target、target_slot、current_stripe、total_stripes、percent、state
     - **Rebalance状态输出**：输出rebalance对象，包含state和progress字段
       - state: IDLE / RUNNING / PAUSED / COMPLETED / FAILED
       - progress: 0–100
       - 待实现：current_stripe、total_stripes、last_error（需要rebalance_ctx结构）
   - **Spare模式汇总**：输出spare_mode_summary对象
     - expansion_mode、active/spare/failed设备数量统计

### 15.2 尚未实现 / 后续计划

1. **Spare 模式后续增强**
   - 根据磨损（wear level）选择 active 盘，而不是简单“前 k+p 个”

2. **Hybrid 模式与重平衡框架**
   - ✅ **完整设计方案已完成**：基于Profile机制的精简稳定架构（详见第16章）
   - ✅ **基础框架已实现**：
     - `ec_bdev_start_rebalance()`：基础框架（创建新profile、清除cache、启动process）
     - `EC_PROCESS_REBALANCE`：Process框架支持（channel设置、进度更新、完成处理）
     - `rebalance_ctx`：精简数据结构（只有6个字段）
   - ✅ **核心迁移逻辑已实现**：Stripe迁移的具体实现
     - ✅ 从旧设备读取数据（并行读取k个数据块，支持ENOMEM重试）
     - ✅ 重新编码生成新校验块（使用ec_encode_stripe，必须重新编码）
     - ✅ 写入新设备（并行写入k个数据块+p个校验块，支持ENOMEM重试）
     - ✅ 更新group_profile_map（绑定到新profile）
     - ✅ 清除assignment_cache（强制重新计算）
     - ✅ 进度更新和资源清理
     - ⏳ **待实现**：强制验证（写入后立即读取验证）
   - ⏳ **待实现**：在 Hybrid 模式下激活所有盘，并通过重平衡迁移已有条带
   - ⏳ **待实现**：将 `ec_bdev_start_rebalance()` 与 RPC / 模式切换联通
   - ✅ **已实现**：`ec_assignment_cache_invalidate_stripe()` 函数
   - ✅ **核心迁移逻辑已实现**：Stripe迁移逻辑
     - ✅ 从旧设备选择（使用旧profile，通过group_profile_map查找）
     - ✅ 从旧设备读取数据（并行读取k个数据块，支持ENOMEM重试）
     - ✅ 重新编码生成新校验块（使用ec_encode_stripe）
     - ✅ 写入新设备（并行写入k个数据块+p个校验块，支持ENOMEM重试）
     - ✅ 更新group_profile_map（绑定到新profile）
     - ✅ 清除assignment_cache（强制重新计算）
     - ✅ 进度更新（current_stripe和rebalance_progress）
     - ✅ 资源清理（stripe_buf和recover_buf在request_complete中释放）
     - ⏳ **待实现**：写入后强制验证（读取并验证写入的数据完整性）

3. **Superblock 中持久化 expansion_mode / rebalance_state 等扩展字段**
   - 当前仅在设计和接口层面规划，具体实现待后续补齐

4. **统一的 EC 状态 RPC（例如 `bdev_ec_get_status`）**
   - 返回 `expansion_mode`、active/spare 列表、rebuild/rebalance 状态、last_failure 等

整体来说：**目前框架已经完成"骨架 + Spare 模式创建时的 active/spare 标记 + 设备选择与 active 的联通 + Spare 模式自动替换逻辑 + JSON输出增强 + 完善的错误处理和验证 + 重平衡基础框架和完整设计方案"，现有 NORMAL 用法保持不变，新的 SPARE 创建方式在读写/选择路径上已具备基础语义，自动替换功能已实现并经过完善，重平衡的完整设计方案已完成（基于Profile机制的精简稳定架构），待实现Stripe迁移的具体逻辑。**

**最新进展（2024-12-04）：**
- ✅ 完善了rebuild状态的JSON输出，同时支持legacy和process框架
- ✅ 完善了rebalance状态的JSON输出（基本状态和进度）
- ✅ Spare模式的自动替换逻辑已实现（设备失败时自动用spare替换并启动rebuild）
- ✅ **改进和完善（2024-12-04）：**
  - 添加了active/spare数量统计和验证逻辑
  - 添加了active设备数量不足的警告（至少需要k个）
  - 更新了num_active_bdevs_after_expansion计数（spare提升后）
  - 改进了日志输出：显示详细的active/spare状态和提升后的计数
  - 添加了slot有效性验证和superblock更新错误处理
  - 改进了SPARE模式创建时的日志：准确统计spare数量并添加验证警告
  - 添加了边界检查：desc有效性、slot范围验证、计数溢出保护

**最新进展（2024-12-XX）：**
- ✅ **重平衡完整设计方案已完成**：
  - 基于Profile机制的精简稳定架构
  - 完全复用现有的group_profile_map和wear_profiles机制
  - 不破坏任何现有功能，自动工作
  - 三层数据完整性保证机制（EC校验块、强制验证、Profile机制）
- ✅ **重平衡基础框架已实现**：
  - `ec_bdev_start_rebalance()`：基础框架（创建新profile、清除cache、启动process）
  - `EC_PROCESS_REBALANCE`：Process框架支持
  - `rebalance_ctx`：精简数据结构
  - 进度更新和完成处理逻辑
- ✅ **核心迁移逻辑已实现**：Stripe迁移逻辑
  - ✅ `ec_assignment_cache_invalidate_stripe()`函数
  - ✅ 从旧设备选择（使用旧profile，通过group_profile_map查找）
  - ✅ 从旧设备读取数据（并行读取k个数据块，支持ENOMEM重试）
  - ✅ 重新编码生成新校验块（使用ec_encode_stripe）
  - ✅ 写入新设备（并行写入k个数据块+p个校验块，支持ENOMEM重试）
  - ✅ 更新group_profile_map（绑定到新profile）
  - ✅ 清除assignment_cache（强制重新计算）
  - ✅ 进度更新和资源清理
  - ⏳ **待实现**：写入后强制验证（读取并验证写入的数据完整性）

---

## 附录：场景总结表

### 各模式下的重建策略总结

| 场景 | 普通模式 | 备用盘模式 | 混合模式 | 扩容模式 |
|------|---------|-----------|---------|---------|
| **设备失败** | 等待新设备，自动重建 | 自动用spare替换，自动重建 | 等待新设备，自动重建 | 等待新设备，自动重建 |
| **重建期间加盘** | 拒绝（-EBUSY） | 允许（添加spare） | 允许（延迟重平衡） | 拒绝（-EBUSY） |
| **重建期间删盘** | 检查设备数量 | 检查设备数量 | 检查设备数量 | 检查设备数量 |
| **重建期间设备失败** | 检查可用设备 | 检查可用设备/spare | 检查可用设备 | 检查可用设备 |
| **重建暂停/恢复** | 支持 | 支持 | 支持 | 支持 |
| **重建取消** | 支持 | 支持 | 支持 | 支持 |

### 重平衡场景总结

| 场景 | 混合模式 | 其他模式 |
|------|---------|---------|
| **重平衡期间加盘** | 拒绝（-EBUSY） | 不适用 |
| **重平衡期间删盘** | 检查设备数量 | 不适用 |
| **重平衡期间设备失败** | 暂停重平衡，启动重建 | 不适用 |
| **重平衡暂停/恢复** | 支持 | 不适用 |
| **重平衡取消** | 支持 | 不适用 |

### 重建和重平衡协调

| 场景 | 处理策略 |
|------|---------|
| **重建优先** | 重平衡期间设备失败 → 暂停重平衡，启动重建 |
| **重平衡延迟** | 重建期间加盘（混合模式）→ 延迟重平衡 |
| **不能同时进行** | 重建和重平衡不能同时进行 |
| **状态恢复** | 系统重启后恢复重建/重平衡状态 |

---

## 附录：参考代码位置

- **RAID Process框架**：`module/bdev/raid/bdev_raid.c`
- **EC Process框架**：`module/bdev/ec/bdev_ec_process.c`
- **EC Rebuild**：`module/bdev/ec/bdev_ec_rebuild.c`
- **EC设备选择**：`module/bdev/ec/bdev_ec_selection.c`
- **EC Superblock**：`module/bdev/ec/bdev_ec_sb.c`
- **EC RPC**：`module/bdev/ec/bdev_ec_rpc.c`

## 附录：关键函数清单

**重建相关**：`ec_bdev_start_rebuild()`, `ec_bdev_stop_rebuild()`, `ec_bdev_pause_rebuild()`, `ec_bdev_resume_rebuild()`, `ec_bdev_cancel_rebuild()`, `ec_bdev_is_rebuilding()`, `ec_bdev_is_rebuild_target()`

**重平衡相关**：`ec_bdev_start_rebalance()`, `ec_bdev_stop_rebalance()`, `ec_bdev_pause_rebalance()`, `ec_bdev_resume_rebalance()`, `ec_bdev_cancel_rebalance()`, `ec_bdev_rebalance_progress()`

**设备管理**：`ec_bdev_add_base_bdev_with_mode()`, `ec_bdev_fail_base_bdev()`, `ec_bdev_find_spare_device()`, `ec_bdev_select_active_devices_by_wear()`, `ec_bdev_activate_all_devices()`

**模式处理**：`ec_bdev_handle_normal_mode()`, `ec_bdev_handle_hybrid_mode()`, `ec_bdev_handle_spare_mode()`, `ec_bdev_handle_expand_mode()`

---

## 十六、重平衡完整解决方案总结（2024-12-XX）

### 16.1 设计目标

**核心目标：**
1. **精简稳定**：最小化数据结构，最大化复用现有机制
2. **完全兼容**：不破坏现有功能，完全复用profile和cache机制
3. **自动工作**：读取时自动选择正确设备，零维护成本
4. **数据一致性**：保证重平衡后读取的数据正确性

### 16.2 核心机制：复用Profile机制

**设计思路：**
- 不修改`selection_seed`（避免破坏未迁移stripe的读取）
- 不引入版本号（profile即版本）
- 不引入bitmap（利用assignment_cache失效）
- 完全复用现有的`group_profile_map`和`wear_profiles`机制

**工作原理：**
```
重平衡前：
  Group 0 → Profile 1（旧磨损级别，旧设备 [A,B,C,D]）
  Group 1 → Profile 1

重平衡时：
  1. 创建 Profile 2（新磨损级别，包含新设备 [E,F,G,H]）
  2. 设置 active_profile_id = 2（新写入用新profile）
  3. 迁移 Group 0：
     - 从旧设备读取（使用Profile 1）
     - 重新编码生成新校验块
     - 写入新设备（使用Profile 2）
     - 验证数据完整性
     - 更新 group_profile_map[0] = 2
     - 清除 assignment_cache[stripe_index]

重平衡后：
  Group 0 → Profile 2（新设备 [E,F,G,H]）✓
  Group 1 → Profile 1（旧设备 [A,B,C,D]）✓

读取时（完全自动）：
  - Group 0：查group_profile_map[0] = 2 → Profile 2 → 新设备 ✓
  - Group 1：查group_profile_map[1] = 1 → Profile 1 → 旧设备 ✓
```

### 16.3 精简的数据结构

```c
/* 精简的 rebalance_ctx - 只有6个字段 */
struct ec_rebalance_context {
    struct ec_bdev *ec_bdev;
    void (*done_cb)(void *ctx, int status);
    void *done_ctx;
    uint64_t current_stripe;
    uint64_t total_stripes;
    uint16_t old_active_profile_id;  /* 重平衡前的active profile */
    uint16_t new_active_profile_id;  /* 重平衡后的active profile */
    int error_status;
};
```

**不需要的字段（已删除）：**
- `old_active_indices`/`new_active_indices`（可以从ec_bdev获取）
- `migrated_stripe_bitmap`（利用assignment_cache失效）
- `rebalance_version`（profile即版本）
- `paused`（使用rebalance_state）
- `last_error`（简化错误处理）

### 16.4 完整的数据迁移流程

```
┌─────────────────────────────────────────────────────────┐
│  重平衡 Stripe 迁移流程（完整版）                          │
├─────────────────────────────────────────────────────────┤
│  1. 锁定 Window LBA 范围                                 │
│     └─ 该范围的I/O等待，其他范围正常I/O继续                │
│                                                          │
│  2. 从旧设备读取数据                                      │
│     a) 临时使用旧profile选择设备                          │
│     b) 并行读取k个数据块                                  │
│     c) 记录旧设备列表                                     │
│                                                          │
│  3. 确定新设备选择                                        │
│     a) 使用新profile（已设置）选择设备                     │
│     b) 记录新设备列表                                     │
│                                                          │
│  4. 【必须】重新编码生成新校验块                           │
│     a) 准备数据指针（k个数据块）                           │
│     b) 调用 ec_encode_stripe() 重新编码                    │
│     c) 生成p个新校验块                                    │
│     注意：不能直接复制校验块，必须重新编码                  │
│                                                          │
│  5. 写入新设备                                            │
│     a) 并行写入k个数据块到新设备                            │
│     b) 并行写入p个校验块到新设备                            │
│                                                          │
│  6. 【强制验证】读取并验证写入的数据                        │
│     a) 从新设备读取刚写入的数据                            │
│     b) 使用EC解码验证数据完整性                             │
│     c) 如果验证失败，标记错误并重试                        │
│                                                          │
│  7. 【关键】更新group_profile_map                         │
│     a) group_profile_map[group_id] = new_profile_id      │
│     b) 标记group_map为dirty（需要持久化）                  │
│                                                          │
│  8. 【关键】清除该stripe的assignment_cache                 │
│     a) ec_assignment_cache_invalidate_stripe(stripe_index)│
│     b) 强制下次读取时重新计算设备选择                       │
│                                                          │
│  9. 解锁 Window LBA 范围                                  │
│     └─ 该范围的I/O可以继续                                 │
│                                                          │
│  10. 旧数据自然失效                                        │
│      └─ 设备选择算法通过profile自动排除旧设备                │
└─────────────────────────────────────────────────────────┘
```

### 16.5 数据完整性保证机制

**三层保证：**

1. **EC校验块验证（自动）**
   - 每个stripe包含k个数据块和p个校验块
   - 写入后，校验块自动验证数据块的正确性
   - 如果数据损坏，读取时会通过EC解码检测到

2. **写入后强制验证（必须）**
   - 每个stripe写入后立即读取验证
   - 使用EC解码验证数据完整性
   - 如果验证失败，标记错误并重试

3. **Profile机制保证（自动）**
   - 已迁移的stripe：绑定到新profile → 自动从新设备读取 ✓
   - 未迁移的stripe：绑定到旧profile → 自动从旧设备读取 ✓
   - 完全自动，不会读错设备

### 16.6 为什么必须重新编码？

**核心原因：**
- EC校验块 = 编码矩阵 × 数据块
- 校验块依赖于数据块的内容和位置
- 设备选择变化 → 数据块位置变化 → 校验块必须重新计算

**不能直接复制的原因：**
```
场景：从设备 [A,B,C,D,E] 迁移到 [F,G,H,I,J]

如果直接复制：
- 数据块从A复制到F ✓（内容相同）
- 但校验块从D复制到I ✗（校验块必须重新计算）
- 结果：数据不一致 ❌

必须重新编码：
- 数据块从A读取，写入F ✓
- 校验块重新计算，写入I ✓
- 结果：数据一致 ✓
```

### 16.7 读取时的自动选择机制

**完全自动，但需要注意边界情况：**

```c
// 现有的 ec_selection_get_profile_slot_for_stripe() 逻辑：

1. 计算group_id = stripe_index / group_size

2. 查找group_profile_map[group_id]：
   - 如果 != 0 → 使用绑定的profile ✓
   - 如果 = 0 → 使用active_profile_id（当前active profile）⚠️

3. 使用profile的wear_weights选择设备

4. 从选择的设备读取数据

关键逻辑：
- 已迁移的group：group_profile_map[group_id] = new_profile_id → 新profile → 新设备 ✓
- 未迁移但已绑定的group：group_profile_map[group_id] = old_profile_id → 旧profile → 旧设备 ✓
- 未绑定的group（group_profile_map[group_id] = 0）：使用active_profile_id ⚠️
```

**⚠️ 潜在问题1：未绑定group的处理**

**问题描述：**
- 如果`group_profile_map[group_id] = 0`（未绑定），会使用`active_profile_id`
- 重平衡期间，`active_profile_id`已设置为新profile
- 但未迁移的group数据还在旧设备，使用新profile会选择新设备 → **读取错误** ❌

**解决方案：**
1. **方案A（推荐）**：重平衡前，确保所有已写入的group都已绑定到旧profile
   - 在`ec_bdev_start_rebalance()`中，遍历所有group，如果`group_profile_map[group_id] = 0`，绑定到`old_active_profile_id`
   - 这样未迁移的group会使用旧profile，读取正确 ✓

2. **方案B**：重平衡期间，临时修改读取逻辑
   - 如果`group_profile_map[group_id] = 0`且重平衡进行中，使用`old_active_profile_id`
   - 但这样需要修改读取路径，增加复杂度

**推荐实现（方案A）：**
```c
// 在 ec_bdev_start_rebalance() 中，绑定所有未绑定的group到旧profile
uint32_t group_id;
for (group_id = 0; group_id < config->num_stripe_groups; group_id++) {
    if (config->group_profile_map[group_id] == 0) {
        // 未绑定的group，绑定到旧profile（数据在旧设备）
        config->group_profile_map[group_id] = rebalance_ctx->old_active_profile_id;
        ec_selection_mark_group_dirty(ec_bdev);
    }
}
```

**⚠️ 潜在问题2：重平衡期间的并发读取**

**问题描述：**
- 重平衡期间，如果读取未迁移的stripe，`group_profile_map`可能还是旧值
- 如果`group_profile_map[group_id] = 0`，会使用新的`active_profile_id`（新profile）
- 但数据还在旧设备，会读错 ❌

**解决方案：**
- 使用Window机制：正在迁移的window被锁定，该范围的I/O等待
- 未迁移的window：如果group已绑定，使用绑定的profile；如果未绑定，使用旧profile（通过方案A解决）
- 已迁移的window：使用新profile，读取新设备 ✓

**⚠️ 潜在问题3：临时修改active_profile_id的线程安全性**

**问题描述：**
- 在迁移时临时修改`config->active_profile_id`来读取旧设备
- 但如果有其他线程同时读取，可能会用错profile

**解决方案：**
- SPDK是单线程模型（每个线程有独立的channel），不存在真正的并发问题
- 但需要确保在同一个线程内，临时修改不会影响其他操作
- **更好的方案**：不修改`active_profile_id`，直接使用`group_profile_map`中绑定的profile（如果未绑定，使用`old_active_profile_id`）

### 16.8 Window机制和I/O并发

**Window机制：**
- 正在迁移的window：锁定（quiesce），该范围的I/O等待
- 其他window：未锁定，正常I/O可以继续
- 窗口大小：默认1024KB（可配置）

**效果：**
- 重平衡期间，大部分I/O可以正常进行
- 只有正在迁移的window会短暂等待
- 对正常I/O的影响最小

### 16.9 关键函数实现清单

**需要实现的函数：**

1. **`ec_bdev_start_rebalance()`** - ✅ 已实现基础框架
   - ✅ 分配rebalance_ctx
   - ✅ 创建新profile（包含新设备的磨损级别）
   - ✅ 清除assignment_cache
   - ✅ **绑定所有未绑定的group到旧profile（关键修复，避免读取错误）**
   - ✅ 计算总stripe数
   - ✅ 设置新的active_profile_id

2. **`ec_bdev_submit_process_request()`中的rebalance逻辑** - ✅ 核心逻辑已实现
   - ✅ 从旧设备选择（使用旧profile，通过group_profile_map查找）
   - ✅ 从旧设备读取数据（并行读取k个数据块，支持ENOMEM重试）
   - ✅ 重新编码生成新校验块（使用ec_encode_stripe）
   - ✅ 写入新设备（并行写入k个数据块+p个校验块，支持ENOMEM重试）
   - ✅ 更新group_profile_map（绑定到新profile）
   - ✅ 清除assignment_cache（强制重新计算）
   - ✅ 进度更新（current_stripe和rebalance_progress）
   - ⏳ **待实现**：写入后强制验证（读取并验证写入的数据完整性）

3. **`ec_assignment_cache_invalidate_stripe()`** - ✅ 已实现
   - ✅ 清除单个stripe的cache

4. **`ec_bdev_rebalance_done_cb()`** - ✅ 已实现
   - ✅ 更新rebalance_state
   - ✅ 调用用户回调
   - ✅ 清理资源

### 16.10 实现优先级

**阶段1：核心迁移逻辑（高优先级）** - ✅ 已完成
1. ✅ 实现`ec_assignment_cache_invalidate_stripe()`
2. ✅ 完善`ec_bdev_start_rebalance()`（包括绑定未绑定group到旧profile）
3. ✅ 实现`ec_bdev_submit_process_request()`中的rebalance逻辑
   - ✅ 从旧设备选择（使用旧profile）
   - ✅ 从旧设备读取数据（并行读取，支持重试）
   - ✅ 重新编码（使用ec_encode_stripe）
   - ✅ 写入新设备（并行写入，支持重试）
   - ✅ 更新group_profile_map（绑定到新profile）
   - ✅ 清除assignment_cache（强制重新计算）
   - ✅ 进度更新和资源清理

**阶段2：验证和错误处理（中优先级）** - ✅ 已完成
1. ✅ 实现写入后强制验证（读取并验证写入的数据完整性）
2. ✅ 错误处理和重试逻辑（已实现ENOMEM重试机制）
3. ✅ 旧设备失败时的恢复逻辑（从parity设备恢复数据）
4. ✅ 重平衡失败时的回滚逻辑（回滚group_profile_map）
5. ✅ 重平衡完成时的持久化逻辑（确保group_profile_map被持久化）
6. ✅ 重平衡完成检查逻辑（正确调用rebalance_done_cb）
7. ✅ 统一进度计算（避免重复计算和不同步）

**阶段3：集成和测试（中优先级）**
1. 与Hybrid模式集成
2. 端到端测试
3. 性能测试

### 16.11 关键优势总结

1. **完全复用现有机制**
   - ✅ 使用现有的`group_profile_map`
   - ✅ 使用现有的`wear_profiles`
   - ✅ 使用现有的`assignment_cache`
   - ✅ 不需要额外的数据结构

2. **不破坏任何功能**
   - ✅ 未迁移的group：继续使用旧profile → 旧设备 ✓
   - ✅ 已迁移的group：使用新profile → 新设备 ✓
   - ✅ 新写入：使用新profile → 新设备 ✓
   - ✅ 完全兼容现有机制

3. **自动工作，零维护**
   - ✅ 读取时自动查找profile
   - ✅ 自动选择正确设备
   - ✅ 不需要额外检查
   - ✅ 零维护成本

4. **精简稳定**
   - ✅ 最小化数据结构（只有6个字段）
   - ✅ 最大化复用现有机制
   - ✅ 减少状态管理
   - ✅ 简化错误处理

5. **数据一致性保证**
   - ✅ EC校验块自动验证
   - ✅ 写入后强制验证
   - ✅ Profile机制自动保证
   - ✅ 三层保证机制

### 16.12 关键修复和注意事项

**修复1：未绑定group的处理**
- 在`ec_bdev_start_rebalance()`中，绑定所有未绑定的group到旧profile
- 确保未迁移的group使用旧profile，读取正确 ✓

**修复2：读取旧设备时的profile选择**
- 不依赖临时修改`active_profile_id`
- 直接使用`group_profile_map`中绑定的profile，如果未绑定则使用`old_active_profile_id`
- 更安全，避免并发问题 ✓

**注意事项：**
1. **Window机制必须正确实现**：正在迁移的window必须锁定，避免并发读取错误
2. **未写入的group**：如果group从未写入过，数据可能不存在，读取会失败（这是正常的）
3. **重平衡期间的写入**：新写入使用新profile，写入新设备 ✓

### 16.13 发现的问题和修复方案

**问题1：未绑定group的读取错误** ⚠️ **严重** ✅ **已修复**

**问题描述：**
- 如果`group_profile_map[group_id] = 0`（未绑定），`ec_selection_get_profile_slot_for_stripe()`会使用`active_profile_id`
- 重平衡期间，`active_profile_id`已设置为新profile
- 但未迁移的group数据还在旧设备，使用新profile会选择新设备 → **读取错误** ❌

**修复方案：**
- 在`ec_bdev_start_rebalance()`中，遍历所有group
- 如果`group_profile_map[group_id] = 0`，绑定到`old_active_profile_id`
- 确保未迁移的group使用旧profile，读取正确 ✓

**问题2：读取旧设备时的profile选择逻辑** ⚠️ **中等** ✅ **已修复**

**问题描述：**
- 原设计临时修改`active_profile_id`来读取旧设备
- 但`ec_selection_get_profile_slot_for_stripe()`会先检查`group_profile_map`
- 如果group已绑定，会使用绑定的profile（正确）
- 如果group未绑定，会使用临时的`active_profile_id`（需要确保是旧profile）

**修复方案：**
- 不依赖临时修改`active_profile_id`
- 直接使用`group_profile_map`中绑定的profile
- 如果未绑定，使用`old_active_profile_id`（通过问题1的修复，所有group都已绑定）

**问题3：重平衡期间的并发读取** ⚠️ **已解决**

**问题描述：**
- 重平衡期间，如果读取未迁移的stripe，可能读错设备

**解决方案：**
- Window机制：正在迁移的window被锁定，该范围的I/O等待

---

**问题4：重平衡完成检查逻辑不完整** ⚠️ **严重** ✅ **已修复**

**问题描述：**
- `ec_bdev_process_thread_run`中只检查了`EC_PROCESS_REBUILD`的完成
- 未检查`EC_PROCESS_REBALANCE`的完成，重平衡完成时不会调用`rebalance_done_cb`
- 重平衡完成时，`process->rebuild_done_cb`会被调用，但这是为rebuild设计的

**修复方案：**
- 在`ec_bdev_process_thread_run`中，添加`EC_PROCESS_REBALANCE`的完成检查
- 如果重平衡完成，调用`ec_bdev_rebalance_done_cb`而不是`rebuild_done_cb`
- 确保重平衡完成时正确调用回调，状态更新正确 ✓

**位置：** `bdev_ec_process.c:2042-2090`

---

**问题5：重平衡进度计算可能不准确** ⚠️ **中等** ✅ **已修复**

**问题描述：**
- 进度在两个地方更新：`window_range_unlocked`和`stripe_write_complete`
- 可能导致进度重复计算或不同步
- `current_stripe`在`stripe_write_complete`中递增，但在`window_range_unlocked`中基于`window_offset`计算

**修复方案：**
- 统一进度计算：只在`ec_bdev_process_stripe_write_complete`中更新进度
- 移除`ec_bdev_process_window_range_unlocked`中的重平衡进度计算
- 确保进度计算的单一来源，避免不同步 ✓

**位置：** `bdev_ec_process.c:424-432` 和 `bdev_ec_process.c:952-957`

---

**问题6：重平衡失败时未回滚group_profile_map** ⚠️ **严重** ✅ **已修复**

**问题描述：**
- 重平衡失败时，部分group可能已更新到新profile，但未回滚
- 可能导致部分数据使用新profile，部分使用旧profile，造成不一致

**修复方案：**
- 在`ec_bdev_process_request_complete`中，如果重平衡失败（`status != 0`）
- 检查当前stripe的group是否已更新到`new_active_profile_id`
- 如果已更新，回滚到`old_active_profile_id`
- 调用`ec_selection_mark_group_dirty`确保回滚被持久化 ✓

**位置：** `bdev_ec_process.c:698-702`

---

**问题7：重平衡过程中旧设备失败处理不完整** ⚠️ **中等** ✅ **已修复**

**问题描述：**
- 从旧设备读取失败时，直接返回`-ENODEV`，未尝试从其他设备恢复
- 对于EC，即使旧设备失败，仍可从其他k个设备恢复数据

**修复方案：**
- 在`ec_bdev_submit_process_request`中，检查旧数据设备的可用性
- 如果数据设备失败，尝试从parity设备读取以补充到k个可用设备
- 如果成功收集到k个可用设备，设置`failed_frag_idx`和`frag_map`用于解码
- 在`ec_bdev_process_stripe_read_complete`中，如果`num_failed_data > 0`，调用`ec_decode_stripe`恢复失败的数据块
- 确保即使旧设备失败，也能成功完成重平衡 ✓

**位置：** `bdev_ec_process.c:1733-1744` 和 `bdev_ec_process.c:1440-1485`

---

**问题8：重平衡完成时未持久化group_profile_map** ⚠️ **严重** ✅ **已修复**

**问题描述：**
- 重平衡完成时，`group_profile_map`已更新为新profile，但可能未持久化到superblock
- `ec_selection_mark_group_dirty`会触发异步刷新，但完成回调中未确保刷新完成
- 如果系统在刷新完成前崩溃，重启后可能丢失部分group的绑定关系

**修复方案：**
- 在`ec_bdev_rebalance_done_cb`中，如果`superblock_enabled`为true
- 调用`ec_bdev_sb_save_selection_metadata`保存`group_profile_map`
- 调用`ec_bdev_write_superblock`写入superblock
- 添加`ec_bdev_rebalance_sb_write_done`回调，确保superblock写入完成后再调用用户回调
- 确保`group_profile_map`在重平衡完成时被持久化 ✓

**位置：** `bdev_ec.c:253-289` 和 `bdev_ec.c:291-320`
- 未迁移的window：如果group已绑定（通过问题1修复），使用绑定的profile，读取正确 ✓
- 已迁移的window：使用新profile，读取新设备 ✓

**问题4：临时修改active_profile_id的线程安全性** ⚠️ **低风险**

**问题描述：**
- 临时修改`config->active_profile_id`可能影响其他操作

**解决方案：**
- SPDK是单线程模型（每个线程有独立的channel），不存在真正的并发问题
- 但通过问题2的修复，不再需要临时修改`active_profile_id`，更安全 ✓

### 16.14 一句话总结

```
重平衡时：创建新profile，绑定所有未绑定group到旧profile，迁移完更新group_profile_map绑定到新profile
读取时：通过group_profile_map自动查找profile，自动选择正确设备 ✓
```

**该方案完全复用现有机制，不破坏任何功能，自动工作，精简稳定。**
