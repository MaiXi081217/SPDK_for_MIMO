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

## 六、重平衡框架实现

### 6.1 重平衡启动

**文件：`bdev_ec_rebalance.c`（新文件）**

```c
int ec_bdev_start_rebalance(struct ec_bdev *ec_bdev,
                            void (*done_cb)(void *ctx, int status),
                            void *done_ctx)
{
    // 1. 分配重平衡上下文
    // 2. 收集旧/新active设备列表
    // 3. 计算总stripe数量
    // 4. 使用Process框架启动重平衡
    return ec_bdev_start_process(ec_bdev, EC_PROCESS_REBALANCE, NULL, NULL);
}
```

### 6.2 Process框架集成

**文件：`bdev_ec_process.c`**

添加对`EC_PROCESS_REBALANCE`类型的支持：
- 重平衡stripe处理
- 数据迁移逻辑
- 进度跟踪

### 6.3 重平衡期间的操作处理

| 操作 | 处理策略 |
|------|---------|
| **重平衡期间加盘** | 拒绝（-EBUSY） |
| **重平衡期间删盘** | 检查设备数量（>= k+p） |
| **重平衡期间设备失败** | 暂停重平衡，启动重建 |
| **重平衡暂停/恢复** | 支持 |
| **重平衡取消** | 支持 |

### 6.4 数据迁移实现

**关键步骤：**
1. 读取旧数据（从旧设备选择）
2. 重新编码（如果需要）
3. 写入新设备（按新设备选择）
4. 更新进度

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
   - 真正实现 `EC_PROCESS_REBALANCE` 的 process 流程
   - 在 Hybrid 模式下激活所有盘，并通过重平衡迁移已有条带
   - 将 `ec_bdev_start_rebalance()` 与 RPC / 模式切换联通

3. **Superblock 中持久化 expansion_mode / rebalance_state 等扩展字段**
   - 当前仅在设计和接口层面规划，具体实现待后续补齐

4. **统一的 EC 状态 RPC（例如 `bdev_ec_get_status`）**
   - 返回 `expansion_mode`、active/spare 列表、rebuild/rebalance 状态、last_failure 等

整体来说：**目前框架已经完成"骨架 + Spare 模式创建时的 active/spare 标记 + 设备选择与 active 的联通 + Spare 模式自动替换逻辑 + JSON输出增强 + 完善的错误处理和验证"，现有 NORMAL 用法保持不变，新的 SPARE 创建方式在读写/选择路径上已具备基础语义，自动替换功能已实现并经过完善，但重平衡框架尚未实现。**

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
