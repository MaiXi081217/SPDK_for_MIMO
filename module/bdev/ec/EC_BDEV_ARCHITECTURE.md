# EC Bdev 架构与实现文档

## 目录
1. [概述](#概述)
2. [核心数据结构](#核心数据结构)
3. [状态机](#状态机)
4. [创建流程](#创建流程)
5. [删除流程](#删除流程)
6. [I/O流程](#io流程)
7. [重建与恢复流程](#重建与恢复流程)
8. [性能优化](#性能优化)
9. [常见问题与解决方案](#常见问题与解决方案)

---

## 概述

EC (Erasure Code) Bdev 是 SPDK 中的一个块设备模块，实现了基于纠删码的数据冗余。它使用 k 个数据块和 p 个校验块来提供容错能力，最多可以容忍 p 个块同时失败。

### 核心特性

- **轮询分布校验条带**：校验块以轮询方式分布在所有磁盘上，类似 RAID5
- **动态条带选择**：每个条带的数据和校验位置根据条带索引动态计算
- **高性能优化**：位图查找、查找表、提前退出等多项性能优化
- **内存对齐支持**：自动从 base bdev 获取对齐要求，优化 ISA-L 性能
- **多 iovec 支持**：支持分散/聚集 I/O，减少系统调用
- **Superblock 支持**：可选的元数据存储，用于持久化配置和自动重建
- **UUID 识别重建**：通过 base bdevs 的 UUID 自动识别和重建 EC bdev
- **热插拔支持**：支持动态添加和删除 base bdev

### 关键参数

- **k**: 数据块数量（每个条带的数据块数）
- **p**: 校验块数量（每个条带的校验块数）
- **n = k + p**: 总磁盘数
- **strip_size**: 条带大小（以块为单位）
- **stripe_index**: 条带索引（每个条带包含 k 个 strips）

---

## 核心数据结构

### `struct ec_bdev` - EC Bdev 主结构

```c
struct ec_bdev {
    struct spdk_bdev          bdev;                    // SPDK bdev 结构
    struct spdk_bdev_desc     *self_desc;              // 内部描述符
    TAILQ_ENTRY(ec_bdev)      global_link;             // 全局链表节点
    
    struct ec_base_bdev_info  *base_bdev_info;         // Base bdev 信息数组
    uint32_t                  strip_size;              // 条带大小（块）
    enum ec_bdev_state        state;                     // 状态
    uint8_t                   num_base_bdevs;          // Base bdev 总数
    uint8_t                   num_base_bdevs_discovered;  // 已发现的 base bdev 数
    uint8_t                   num_base_bdevs_operational;  // 可操作的 base bdev 数
    
    uint8_t                   k;                       // 数据块数
    uint8_t                   p;                       // 校验块数
    
    bool                      destroy_started;          // 删除已开始标志
    bool                      superblock_enabled;       // Superblock 启用标志
    struct ec_bdev_superblock *sb;                      // Superblock 指针
    
    size_t                    buf_alignment;            // 缓冲区对齐要求
};
```

### `struct ec_base_bdev_info` - Base Bdev 信息

```c
struct ec_base_bdev_info {
    struct ec_bdev            *ec_bdev;                 // 所属 EC bdev
    char                      *name;                    // Base bdev 名称
    struct spdk_uuid          uuid;                     // UUID
    struct spdk_bdev_desc     *desc;                    // Bdev 描述符
    uint64_t                  data_offset;              // 数据区域偏移（块）
    uint64_t                  data_size;                // 数据区域大小（块）
    
    bool                      remove_scheduled;          // 移除已调度
    bool                      is_configured;            // 已配置标志
    bool                      is_failed;                // 失败标志
    
    struct spdk_io_channel    *app_thread_ch;          // App 线程 IO channel
};
```

---

## 状态机

EC Bdev 有三种主要状态：

```
CONFIGURING → ONLINE → OFFLINE
     ↑           ↓         ↓
     └───────────┴─────────┘
```

### 状态说明

1. **EC_BDEV_STATE_CONFIGURING**
   - 初始状态，正在配置 base bdevs
   - 尚未注册到 bdev 层
   - 不能处理 I/O 请求

2. **EC_BDEV_STATE_ONLINE**
   - 所有 base bdevs 已配置
   - 已注册到 bdev 层
   - 可以处理 I/O 请求

3. **EC_BDEV_STATE_OFFLINE**
   - 正在取消配置或已取消配置
   - 不再接受新的 I/O 请求
   - 等待现有 I/O 完成

---

## 创建流程

```
RPC: bdev_ec_create
  ↓
ec_bdev_create()
  ├─ 分配 ec_bdev 结构
  ├─ 初始化基本参数 (k, p, strip_size_kb)
  ├─ 分配 base_bdev_info 数组
  ├─ 设置状态为 CONFIGURING
  └─ 添加到全局链表
      ↓
  循环添加 base bdevs
      ↓
  ec_bdev_add_base_bdev()
      └─ ec_bdev_configure_base_bdev()
          ├─ spdk_bdev_open_ext()
          ├─ spdk_bdev_module_claim_bdev()
          ├─ 获取 IO channel
          ├─ 计算 data_offset 和 data_size
          └─ ec_bdev_configure_cont()
              ↓
          检查是否所有 base bdevs 已配置
              ↓
          是 → ec_start()
              ├─ 计算 EC bdev 大小
              ├─ 初始化 ISA-L 编码表
              └─ 注册 bdev
                  ↓
              状态 → ONLINE
```

---

## 删除流程

### RPC 删除（擦除 Superblock）

```
RPC: bdev_ec_delete
  ↓
ec_bdev_delete(ec_bdev, wipe_sb=true, ...)
  ├─ 检查是否已开始删除
  ├─ 设置 destroy_started = true
  ├─ 从全局链表移除
  └─ 检查是否有 superblock
      ↓
  有 superblock → ec_bdev_delete_sb()
      ├─ 擦除所有 base bdevs 的 superblock
      └─ ec_bdev_delete_continue()
          ↓
  无 superblock → ec_bdev_delete_continue()
      ├─ 标记所有 base bdevs 为 remove_scheduled
      └─ ec_bdev_deconfigure()
          ├─ 设置状态为 OFFLINE
          └─ spdk_bdev_unregister()
              ↓
          ec_bdev_deconfigure_unregister_done()
              ├─ 关闭 self_desc
              ├─ 释放所有 base bdev 资源
              └─ 调用用户回调
                  ↓
          ec_bdev_free()
```

### 关闭时（保留 Superblock）

```
应用程序关闭 (Ctrl+C)
  ↓
ec_bdev_fini_start()
  ├─ 释放非在线状态的资源
  └─ 设置 g_shutdown_started = true
      ↓
  SPDK bdev 层自动调用 ec_bdev_destruct()
      ├─ 检测到 g_shutdown_started = true
      ├─ 设置状态为 OFFLINE
      ├─ 标记 base bdevs 为 remove_scheduled
      └─ ec_bdev_module_stop_done()
          └─ spdk_io_device_unregister()
              ↓
          ec_bdev_free()
```

**关键点**：
- RPC 删除时：`wipe_sb=true`，会擦除 superblock
- 关闭时：`wipe_sb=false`，保留 superblock 以便下次启动时自动重建

---

## I/O流程

### 读取流程

```
ec_submit_rw_request() (READ)
  ├─ 计算 start_strip, end_strip
  ├─ 计算 stripe_index = start_strip / k
  ├─ ec_select_base_bdevs_default() 选择数据盘
  └─ spdk_bdev_readv_blocks()
      ↓
  ec_base_bdev_io_complete()
      └─ 完成 I/O
```

### 写入流程

#### 全条带写入

```
ec_submit_rw_request() (WRITE - full stripe)
  ├─ 计算 stripe_index
  ├─ ec_select_base_bdevs_default()
  └─ ec_submit_write_stripe()
      ├─ 分配校验缓冲区
      ├─ ec_encode_stripe() (ISA-L)
      ├─ 并行写入 k 个数据块
      └─ 并行写入 p 个校验块
```

#### 部分条带写入（RMW）

```
ec_submit_rw_request() (WRITE - partial stripe)
  └─ ec_submit_write_partial_stripe()
      ├─ 分配 RMW 上下文
      ├─ 读取所有 k 个数据块
      ├─ 合并新数据到 stripe buffer
      ├─ ec_encode_stripe() (重新编码)
      ├─ 写入修改的数据块
      └─ 写入所有校验块
```

### 条带选择逻辑

```c
// 轮询分布算法
parity_start = (n - (stripe_index % n)) % n;

// 对于每个 base bdev (i = 0 到 n-1):
//   如果 i 在 [parity_start, parity_start + p - 1] 范围内（模 n）:
//      它是校验盘
//   否则:
//      它是数据盘
```

---

## 重建与恢复流程

### Superblock 重建流程

当 tgt 重启后，EC bdev 可以通过 superblock 自动重建：

```
tgt 重启
  ↓
添加 base bdev (例如: NVMe0n1)
  ↓
ec_bdev_examine(NVMe0n1)
  ↓
ec_bdev_examine_load_sb()：读取 superblock
  ↓
ec_bdev_examine_cont()
  ├─ 从 superblock 提取 EC bdev UUID
  ├─ 通过 UUID 查找或创建 EC bdev
  ├─ 通过 base bdev UUID 匹配位置
  └─ ec_bdev_configure_base_bdev()
      ↓
  继续添加其他 base bdevs...
      ↓
  所有 base bdevs 配置完成 → EC bdev 上线
```

**关键函数**：
- `ec_bdev_examine()`：入口函数，当 base bdev 被添加时自动调用
- `ec_bdev_examine_load_sb()`：从 base bdev 读取 superblock
- `ec_bdev_examine_cont()`：通过 UUID 识别和重建
- `ec_bdev_create_from_sb()`：从 superblock 创建 EC bdev

---

## 性能优化

### 1. 条带选择算法优化

使用位图（bitmap）进行 O(1) 查找，并预计算所有校验位置：

```c
/* 预计算所有校验位置并构建位图 */
uint64_t parity_bitmap = 0;
for (uint8_t p_idx = 0; p_idx < ec_bdev->p; p_idx++) {
    uint8_t pos = (parity_start + p_idx) % n;
    if (ec_bdev->p <= 64 && pos < 64) {
        parity_bitmap |= (1ULL << pos);
    }
}

/* 快速查找：位图查找（O(1)） */
bool is_parity_pos = (parity_bitmap & (1ULL << i)) != 0;
```

**性能提升**：从 O(n × p) 降低到 O(n + p)，查找从 O(p) 降低到 O(1)

### 2. 内存对齐优化

从所有 base bdev 获取最大对齐要求，确保 ISA-L 和硬件加速正常工作：

```c
size_t alignment = 0;
EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
    alignment = spdk_max(alignment, spdk_bdev_get_buf_align(base_bdev));
}
ec_bdev->buf_alignment = alignment > 0 ? alignment : 0x1000;
```

### 3. 多 Iovec 支持

使用 `spdk_bdev_readv_blocks()` 支持分散/聚集 I/O，减少系统调用。

### 4. 查找表优化

构建查找表，将 base bdev 索引映射到逻辑片段索引，查找复杂度从 O(k+p) 降低到 O(1)。

---

## 常见问题与解决方案

### 1. 关闭时资源清理顺序问题

#### 问题描述

关闭时出现 `unrecoverable spinlock error 8: Lock has been destroyed` 错误，导致程序崩溃。

#### 原因分析

在 `ec_bdev_destruct_impl` 中，如果 `g_shutdown_started` 为 true，会立即调用 `ec_bdev_free_base_bdev_resource` 释放 base bdev 资源（关闭 desc 和 channel）。但是，如果此时 superblock 擦除操作还在进行（使用这些 desc 和 channel），就会导致资源被提前释放，引发 spinlock 错误。

#### 解决方案

在关闭时，不立即释放已配置的 base bdev 资源，而是标记为 `remove_scheduled`，等待 superblock 操作完成后再释放：

```c
static void
ec_bdev_destruct_impl(void *ctx)
{
    if (g_shutdown_started) {
        ec_bdev->state = EC_BDEV_STATE_OFFLINE;
        /* 标记 base bdevs 为 remove_scheduled，但不立即释放资源 */
        EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
            base_info->remove_scheduled = true;
            /* 只释放未配置的 base bdev 资源 */
            if (!base_info->is_configured) {
                ec_bdev_free_base_bdev_resource(base_info);
            }
        }
    }
    // ...
}
```

同时在 superblock 擦除操作中检查资源是否已被释放：

```c
/* 在 _ec_bdev_wipe_superblock 中 */
if (base_info->desc == NULL || base_info->app_thread_ch == NULL) {
    /* 资源已被释放，跳过擦除 */
    ctx->submitted++;
    ctx->remaining--;
    continue;
}
```

**关键点**：
- 关闭时只释放未配置的 base bdev 资源
- 已配置的 base bdev 资源在 superblock 操作完成后由 `ec_bdev_deconfigure_unregister_done` 释放
- Superblock 擦除操作需要检查资源是否已被释放

---

### 2. 关闭时重复 Unregister 问题

#### 问题描述

关闭时出现警告：`Unable to unregister bdev 'ec1' during spdk_bdev_finish()`。

#### 原因分析

在 `ec_bdev_fini_start` 中主动调用 `ec_bdev_delete`，这会触发 `spdk_bdev_unregister`。但是 SPDK 的 bdev 层在关闭时也会自动尝试 unregister 所有 bdev，导致冲突。

#### 解决方案

参考 RAID 模块的做法，在 `ec_bdev_fini_start` 中不主动删除 EC bdev，只设置标志和释放非在线状态的资源，让 bdev 层自动处理：

```c
static void
ec_bdev_fini_start(void)
{
    /* 不主动删除 EC bdev，让 bdev 层自动处理 */
    TAILQ_FOREACH(ec_bdev, &g_ec_bdev_list, global_link) {
        if (ec_bdev->state != EC_BDEV_STATE_ONLINE) {
            EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
                ec_bdev_free_base_bdev_resource(base_info);
            }
        }
    }
    g_shutdown_started = true;
}
```

**关键点**：
- 关闭时不主动调用 `ec_bdev_delete`
- 只释放非在线状态的资源
- 设置 `g_shutdown_started = true` 标志
- 让 bdev 层自动调用 `ec_bdev_destruct` 来处理关闭

---

### 3. Superblock 擦除时机问题

#### 问题描述

关闭时 superblock 被擦除，导致下次启动时无法自动重建 EC bdev。

#### 原因分析

之前的实现中，关闭时也会擦除 superblock，导致配置信息丢失。

#### 解决方案

添加 `wipe_sb` 参数控制是否擦除 superblock：

```c
void ec_bdev_delete(struct ec_bdev *ec_bdev, bool wipe_sb, 
                    ec_bdev_destruct_cb cb_fn, void *cb_ctx)
{
    // ...
    /* 只有 RPC 删除时才擦除 superblock */
    if (wipe_sb && ec_bdev->sb != NULL) {
        ec_bdev_delete_sb(ec_bdev, cb_fn, cb_ctx);
    } else {
        ec_bdev_delete_continue(ec_bdev, cb_fn, cb_ctx);
    }
}
```

- **RPC 删除时**：`wipe_sb=true`，擦除 superblock
- **关闭时**：`wipe_sb=false`，保留 superblock

**关键点**：
- RPC 删除命令传递 `wipe_sb=true`
- 关闭时传递 `wipe_sb=false`
- 其他非 RPC 调用（如错误处理、base bdev 移除事件）也传递 `wipe_sb=false`

---

### 4. RMW 路径资源管理问题

#### 问题描述

在部分条带写入（RMW）路径中，当 `-ENOMEM` 错误发生在初始读取阶段时，如果部分读取已经提交，资源会被错误地释放，导致 use-after-free 或 double-free。

#### 原因分析

在 `ec_submit_write_partial_stripe` 中，如果 `-ENOMEM` 发生在读取提交过程中，且部分读取已经提交，立即释放 `rmw` 资源会导致后续的 `ec_rmw_read_complete` 回调访问已释放的内存。

#### 解决方案

如果部分读取已经提交，不立即释放资源，而是标记为失败状态，让后续的读取完成回调来清理：

```c
if (rc == -ENOMEM) {
    if (i > 0) {
        /* 部分读取已提交 - 标记为失败，让完成回调清理 */
        rmw->reads_expected = rmw->reads_completed;
        return 0;
    } else {
        /* 没有读取已提交 - 安全释放资源 */
        // ... 释放资源 ...
    }
}
```

在 `ec_rmw_read_complete` 中检查失败状态：

```c
if (rmw->reads_completed >= rmw->reads_expected) {
    if (rmw->reads_expected < ec_bdev->k) {
        /* 失败状态 - 清理资源并完成 I/O */
        // ... 清理资源 ...
        ec_bdev_io_complete(ec_io, SPDK_BDEV_IO_STATUS_FAILED);
        return;
    }
    /* 正常完成 - 继续编码 */
}
```

**关键点**：
- 如果部分操作已提交，不立即释放资源
- 标记为失败状态，让完成回调清理
- 如果没有任何操作提交，可以安全释放资源

---

### 5. Superblock 写入/擦除回调未调用

#### 问题描述

当所有 base bdevs 都未配置时，superblock 写入或擦除操作可能不调用最终回调。

#### 原因分析

如果所有 base bdevs 都未配置，循环中所有操作都被跳过，`remaining` 可能不会减为 0，导致回调永远不会被调用。

#### 解决方案

在循环后检查 `remaining == 0` 的情况：

```c
for (i = 0; i < ec_bdev->num_base_bdevs; i++) {
    if (!base_info->is_configured) {
        ctx->remaining--;
        continue;
    }
    // 提交操作...
}

/* 检查是否所有操作都被跳过 */
if (ctx->remaining == 0) {
    ctx->cb(ctx->status, ctx->ec_bdev, ctx->cb_ctx);
    free(ctx);
}
```

---

### 6. 异步回调处理原则

#### 通用原则

1. **所有异步操作都必须有回调**
2. **错误路径必须调用回调**（即使返回错误）
3. **成功路径在函数内部调用回调**（如果函数负责调用）
4. **检查所有可能的退出路径**

#### 示例

```c
// 正确：所有路径都调用回调
static void
some_async_operation(..., callback_fn, callback_ctx)
{
    int rc = 0;
    
    // 操作...
    if (rc != 0) {
        // 错误路径：调用回调
        if (callback_fn) {
            callback_fn(callback_ctx, rc);
        }
        return;
    }
    
    // 成功路径：在完成时调用回调
}
```

---

## 总结

EC Bdev 模块的关键设计原则：

1. **异步操作**：所有操作都是异步的，使用回调函数
2. **资源管理**：确保所有资源都正确释放，特别是在错误路径
3. **状态管理**：正确维护状态机，确保状态转换的正确性
4. **错误处理**：所有错误路径都要调用回调
5. **关闭处理**：关闭时不主动删除，让 bdev 层自动处理
6. **Superblock 管理**：RPC 删除时擦除，关闭时保留

遵循这些原则可以避免大部分常见问题。

---
