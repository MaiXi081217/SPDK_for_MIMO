# SPDK 新功能技术文档

## 概述

本文档详细介绍了SPDK项目中新增的功能模块和改进，包括EC（纠删码）模块、RAID模块增强、磨损均衡扩展等核心功能。这些功能显著提升了存储系统的可靠性、容错能力和可维护性。

---

## 目录

1. [EC（纠删码）模块](#1-ec纠删码模块)
2. [RAID模块改进](#2-raid模块改进)
3. [RAID10功能](#3-raid10功能)
4. [磨损均衡扩展](#4-磨损均衡扩展)
5. [Superblock管理](#5-superblock管理)
6. [SMART信息增强](#6-smart信息增强)
7. [使用示例](#7-使用示例)

---

## 1. EC（纠删码）模块

### 1.1 功能概述

EC（Erasure Code）模块实现了基于纠删码的数据冗余存储。它使用k个数据块和p个校验块来提供容错能力，最多可以容忍p个块同时失败。

### 1.2 核心特性

- **轮询分布校验条带**：校验块以轮询方式分布在所有磁盘上，类似RAID5
- **动态条带选择**：每个条带的数据和校验位置根据条带索引动态计算
- **高性能优化**：位图查找、查找表、提前退出等多项性能优化
- **内存对齐支持**：自动从base bdev获取对齐要求，优化ISA-L性能
- **多iovec支持**：支持分散/聚集I/O，减少系统调用
- **Superblock支持**：可选的元数据存储，用于持久化配置和自动重建
- **UUID识别重建**：通过base bdevs的UUID自动识别和重建EC bdev
- **热插拔支持**：支持动态添加和删除base bdev

### 1.3 关键参数

- **k**: 数据块数量（每个条带的数据块数）
- **p**: 校验块数量（每个条带的校验块数）
- **n = k + p**: 总磁盘数
- **strip_size**: 条带大小（以块为单位）
- **stripe_index**: 条带索引（每个条带包含k个strips）

### 1.4 状态管理

EC Bdev有三种主要状态：

```
CONFIGURING → ONLINE → OFFLINE
     ↑           ↓         ↓
     └───────────┴─────────┘
```

- **CONFIGURING**: 初始状态，正在配置base bdevs
- **ONLINE**: 所有base bdevs已配置，可以处理I/O请求
- **OFFLINE**: 正在取消配置或已取消配置，不再接受新的I/O请求

### 1.5 重建功能

EC模块支持多种场景下的重建处理：

- **自动重建**：系统重启后通过superblock自动识别和重建
- **手动重建**：支持手动触发重建操作
- **进度跟踪**：实时跟踪重建进度，支持断点续传
- **状态持久化**：重建状态和进度保存到superblock，支持中断后恢复

### 1.6 使用方法

#### 创建EC Bdev

```bash
# 使用RPC命令创建EC bdev
rpc.py bdev_ec_create -n ec1 -k 4 -p 2 -z 64 -b "Nvme0n1 Nvme1n1 Nvme2n1 Nvme3n1 Nvme4n1 Nvme5n1"
```

参数说明：
- `-n, --name`: EC bdev名称
- `-k, --k`: 数据块数量（必需）
- `-p, --p`: 校验块数量（必需）
- `-z, --strip-size-kb`: 条带大小（KB，可选）
- `-b, --base-bdevs`: base bdev列表，空格分隔

#### 查询EC Bdev信息

```bash
# 查询所有EC bdev
rpc.py bdev_ec_get_bdevs -c all

# 查询在线EC bdev
rpc.py bdev_ec_get_bdevs -c online
```

#### 添加Base Bdev

```bash
# 向现有EC bdev添加base bdev
rpc.py bdev_ec_add_base_bdev -e ec1 -b Nvme6n1
```

#### 删除EC Bdev

```bash
# 删除EC bdev（会擦除superblock）
rpc.py bdev_ec_delete -n ec1
```

---

## 2. RAID模块改进

### 2.1 功能概述

对RAID模块进行了重大改进，实现了与EC模块相同级别的故障处理能力，包括重建进度持久化、状态跟踪、自动恢复等功能。

### 2.2 核心改进

#### 2.2.1 REBUILDING状态持久化

- 添加`RAID_SB_BASE_BDEV_REBUILDING = 4`状态
- 重建开始时更新superblock状态为`REBUILDING`
- 重建完成时更新为`CONFIGURED`或`FAILED`
- 系统重启后检测`REBUILDING`状态并自动恢复重建

#### 2.2.2 重建进度持久化

- 在superblock中添加`rebuild_offset`和`rebuild_total_size`字段
- 每个window完成后更新进度到superblock
- 系统重启后从superblock恢复重建进度，支持断点续传

#### 2.2.3 详细进度报告

增强的进度报告包含以下信息：
- `target`: 目标磁盘名称
- `target_slot`: 目标磁盘slot
- `current_offset`: 当前重建偏移（块）
- `total_size`: 总大小（块）
- `percent`: 百分比
- `state`: 重建状态（IDLE/READING/WRITING/CALCULATING）

#### 2.2.4 细粒度状态跟踪

添加了4个重建状态：
- `RAID_REBUILD_STATE_IDLE`: 空闲状态
- `RAID_REBUILD_STATE_READING`: 正在读取
- `RAID_REBUILD_STATE_WRITING`: 正在写入
- `RAID_REBUILD_STATE_CALCULATING`: 正在计算（用于RAID5F）

#### 2.2.5 进度查询API

提供了程序化的进度查询接口：

```c
int raid_bdev_get_rebuild_progress(struct raid_bdev *raid_bdev, 
                                   uint64_t *current_offset,
                                   uint64_t *total_size);

bool raid_bdev_is_rebuilding(struct raid_bdev *raid_bdev);
```

#### 2.2.6 重建完成回调

支持注册重建完成回调函数，实现异步通知：

```c
void raid_bdev_start_rebuild_with_cb(struct raid_bdev_base_bdev_info *target_base_info,
                                     raid_bdev_rebuild_done_cb cb_fn,
                                     void *cb_ctx);
```

### 2.3 使用方法

#### 查询重建进度

```bash
# 通过RPC查询RAID bdev信息（包含重建进度）
rpc.py bdev_raid_get_bdevs -c all
```

返回示例：
```json
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

#### 设置重建参数

```bash
# 设置重建窗口大小和最大带宽
rpc.py bdev_raid_set_options -w 1024 -b 1000
```

参数说明：
- `-w, --process-window-size-kb`: 后台处理（如重建）窗口大小（KiB）
- `-b, --process-max-bandwidth-mb-sec`: 后台处理最大带宽（MiB/Sec）

---

## 3. RAID10功能

### 3.1 功能概述

新增了RAID10（RAID 1+0）支持，提供了镜像和条带化的组合，既保证了数据冗余，又提升了I/O性能。

### 3.2 特性

- **镜像+条带化**：先镜像后条带化，提供高可靠性和高性能
- **容错能力**：每个镜像组可以容忍一个磁盘失败
- **性能优化**：条带化提供并行I/O能力
- **自动重建**：支持故障磁盘的自动重建

### 3.3 使用方法

#### 创建RAID10

```bash
# 创建RAID10 bdev
rpc.py bdev_raid_create -n raid10_1 -r raid10 -z 64 -b "Nvme0n1 Nvme1n1 Nvme2n1 Nvme3n1"
```

参数说明：
- `-n, --name`: RAID bdev名称
- `-r, --raid-level`: RAID级别，支持`raid0`、`raid1`、`raid10`（或`10`）、`concat`
- `-z, --strip-size-kb`: 条带大小（KB）
- `-b, --base-bdevs`: base bdev列表，空格分隔

**注意**：RAID10需要偶数个base bdev，它们会被分成镜像对。

#### 删除RAID Bdev

```bash
# 删除RAID bdev（会同时删除superblock）
rpc.py bdev_raid_delete -n raid10_1
```

---

## 4. 磨损均衡扩展

### 4.1 功能概述

为EC模块添加了磨损均衡（Wear Leveling）扩展接口，允许外部模块（如FTL）通过扩展接口控制I/O分布和磨损均衡策略，平衡各个存储设备的写入负载，延长SSD等存储设备的使用寿命。

### 4.2 特性

- **扩展接口**：提供标准化的扩展接口，允许外部模块集成
- **智能数据分布**：根据设备的磨损程度动态调整数据分布
- **负载均衡**：平衡各个base bdev的写入负载
- **磨损监控**：通过回调接口获取和更新磨损信息
- **灵活策略**：外部模块可以实现自定义的磨损均衡策略

### 4.3 扩展接口

EC模块提供了以下扩展接口回调：

#### 4.3.1 选择Base Bdev回调

```c
typedef int (*ec_ext_select_base_bdevs_fn)(
    struct ec_bdev_extension_if *ext_if,
    struct ec_bdev *ec_bdev,
    uint64_t offset_blocks,
    uint32_t num_blocks,
    uint8_t *data_indices,    // 输出：数据块索引数组（大小k）
    uint8_t *parity_indices,  // 输出：校验块索引数组（大小p）
    void *ctx
);
```

**功能**：根据磨损均衡策略选择数据块和校验块应该写入哪些base bdev。

#### 4.3.2 获取磨损程度回调

```c
typedef int (*ec_ext_get_wear_level_fn)(
    struct ec_bdev_extension_if *ext_if,
    struct ec_bdev *ec_bdev,
    struct ec_base_bdev_info *base_info,
    uint8_t *wear_level,  // 输出：磨损程度（0-100，0=无磨损，100=最大磨损）
    void *ctx
);
```

**功能**：获取指定base bdev的磨损程度信息。

#### 4.3.3 I/O完成通知回调

```c
typedef void (*ec_ext_notify_io_complete_fn)(
    struct ec_bdev_extension_if *ext_if,
    struct ec_bdev *ec_bdev,
    struct ec_base_bdev_info *base_info,
    uint64_t offset_blocks,
    uint32_t num_blocks,
    bool is_write,
    void *ctx
);
```

**功能**：通知外部模块I/O操作完成，用于更新磨损统计。

### 4.4 使用方法

磨损均衡功能通过扩展接口实现，需要外部模块（如FTL）注册扩展接口：

```c
// 定义扩展接口
struct ec_bdev_extension_if ext_if = {
    .name = "wear_leveling",
    .ctx = my_context,
    .select_base_bdevs = my_select_base_bdevs,
    .get_wear_level = my_get_wear_level,
    .notify_io_complete = my_notify_io_complete,
    .init = my_init,
    .fini = my_fini,
};

// 注册扩展接口到EC bdev
ec_bdev_register_extension(ec_bdev, &ext_if);
```

**工作流程**：

1. 外部模块注册扩展接口
2. EC模块在写入数据时调用`select_base_bdevs`回调，根据磨损程度选择base bdev
3. EC模块通过`get_wear_level`回调获取各base bdev的磨损程度
4. I/O完成后通过`notify_io_complete`回调更新磨损统计
5. 在重建时优先使用磨损程度较低的设备

---

## 5. Superblock管理

### 5.1 功能概述

Superblock是存储在base bdev开头的元数据，用于持久化EC和RAID配置信息，支持系统重启后的自动重建。

### 5.2 功能特性

- **配置持久化**：保存EC/RAID配置到磁盘
- **自动重建**：系统重启后自动识别和重建
- **UUID识别**：通过UUID匹配base bdev位置
- **状态跟踪**：保存设备状态和重建进度

### 5.3 清除Superblock

新增了清除superblock的RPC命令，用于清理磁盘上的元数据：

```bash
# 清除指定bdev的superblock
rpc.py bdev_wipe_superblock -n Nvme0n1 -s 4096
```

参数说明：
- `-n, --name`: bdev名称（必需）
- `-s, --size`: 要清除的大小（字节），默认4096字节

**使用场景**：
- 清理旧的EC/RAID配置
- 重置磁盘状态
- 故障恢复

### 5.4 Superblock生命周期

- **创建时**：如果启用superblock，会在所有base bdev上写入配置信息
- **运行时**：定期更新状态和进度信息
- **删除时**：RPC删除命令会擦除superblock，关闭时保留superblock
- **重启时**：从superblock读取配置并自动重建

---

## 6. SMART信息增强

### 6.1 功能概述

在添加NVMe控制器时，自动输出所有命名空间的SMART信息，包括磨损程度等关键指标。

### 6.2 输出信息

当添加NVMe控制器时，系统会自动输出：

- **命名空间信息**：所有命名空间的基本信息
- **SMART信息**：包括但不限于：
  - 磨损程度（Wear Level）
  - 温度信息
  - 健康状态
  - 写入量统计
  - 错误统计

### 6.3 使用方法

```bash
# 添加NVMe控制器（会自动输出SMART信息）
rpc.py bdev_nvme_attach_controller -b Nvme0 -t PCIe -a 0000:01:00.0
```

系统会自动在控制台输出所有命名空间的SMART信息，方便用户了解设备状态。

---

## 7. 使用示例

### 7.1 完整EC Bdev使用流程

```bash
# 1. 创建EC bdev（k=4, p=2，共6个磁盘）
rpc.py bdev_ec_create -n ec1 -k 4 -p 2 -z 64 \
  -b "Nvme0n1 Nvme1n1 Nvme2n1 Nvme3n1 Nvme4n1 Nvme5n1"

# 2. 查询EC bdev状态
rpc.py bdev_ec_get_bdevs -c all

# 3. 使用EC bdev（通过其他SPDK应用）
# EC bdev可以作为普通bdev使用

# 4. 如果某个磁盘故障，系统会自动检测并开始重建
# 查询重建进度
rpc.py bdev_ec_get_bdevs -c all
# 返回信息中包含重建进度

# 5. 替换故障磁盘后，可以添加新的base bdev
rpc.py bdev_ec_add_base_bdev -e ec1 -b Nvme6n1

# 6. 删除EC bdev（会擦除superblock）
rpc.py bdev_ec_delete -n ec1
```

### 7.2 RAID10使用流程

```bash
# 1. 创建RAID10（需要偶数个磁盘，至少4个）
rpc.py bdev_raid_create -n raid10_1 -r raid10 -z 64 \
  -b "Nvme0n1 Nvme1n1 Nvme2n1 Nvme3n1"

# 2. 查询RAID状态和重建进度
rpc.py bdev_raid_get_bdevs -c all

# 3. 设置重建参数（可选）
rpc.py bdev_raid_set_options -w 1024 -b 1000

# 4. 删除RAID bdev
rpc.py bdev_raid_delete -n raid10_1
```

### 7.3 故障恢复流程

```bash
# 场景：系统重启后，EC/RAID bdev自动重建

# 1. 启动SPDK应用
./build/bin/spdk_tgt

# 2. 添加base bdev（系统会自动检测superblock）
rpc.py bdev_nvme_attach_controller -b Nvme0 -t PCIe -a 0000:01:00.0
rpc.py bdev_nvme_attach_controller -b Nvme1 -t PCIe -a 0000:02:00.0
# ... 添加其他base bdev

# 3. 系统自动从superblock识别EC/RAID配置
# 4. 如果检测到REBUILDING状态，自动恢复重建
# 5. 查询重建进度
rpc.py bdev_ec_get_bdevs -c all
rpc.py bdev_raid_get_bdevs -c all
```

### 7.4 清理Superblock

```bash
# 如果需要完全清理磁盘上的元数据
rpc.py bdev_wipe_superblock -n Nvme0n1 -s 4096
rpc.py bdev_wipe_superblock -n Nvme1n1 -s 4096
# ... 清理其他磁盘
```

---

## 8. 技术细节

### 8.1 EC模块架构

EC模块采用模块化设计，主要组件包括：

- **bdev_ec.c**: 核心逻辑，包括创建、删除、I/O处理
- **bdev_ec_encode.c**: 编码/解码逻辑，使用ISA-L库
- **bdev_ec_io.c**: I/O处理，包括读写、RMW等
- **bdev_ec_rebuild.c**: 重建逻辑
- **bdev_ec_sb.c**: Superblock管理
- **bdev_ec_rpc.c**: RPC接口

### 8.2 性能优化

- **位图查找**：O(1)复杂度的校验位置查找
- **内存对齐**：自动获取最大对齐要求，优化硬件加速
- **多iovec支持**：减少系统调用
- **查找表优化**：O(1)复杂度的base bdev索引映射

### 8.3 可靠性保证

- **状态持久化**：所有关键状态保存到superblock
- **断点续传**：重建进度持久化，支持中断后恢复
- **错误处理**：完善的错误处理和资源清理机制
- **资源管理**：确保所有资源正确释放

---

## 9. 注意事项

### 9.1 Superblock版本兼容性

- 新字段（如`rebuild_offset`、`rebuild_total_size`）是新添加的
- 旧版本的superblock这些字段为0（不影响功能）
- 建议在升级时注意版本兼容性

### 9.2 性能考虑

- **Superblock写入频率**：每个window完成后写入一次
- **优化建议**：可以改为每N个window写入一次（减少I/O）
- **异步写入**：使用异步写入避免阻塞

### 9.3 状态一致性

- **内存状态**：实时进度存储在内存中
- **持久化状态**：superblock中的进度可能略有滞后
- **重启恢复**：从superblock恢复，可能丢失最后一个window的进度（可接受）

### 9.4 磁盘要求

- **RAID10**：需要偶数个base bdev
- **EC模块**：需要至少k+p个base bdev
- **Superblock**：每个base bdev需要预留空间（默认前4KB）

---

## 10. 总结

本次更新为SPDK添加了以下核心功能：

1. ✅ **完整的EC模块**：提供纠删码数据冗余，支持自动重建和进度跟踪
2. ✅ **RAID模块增强**：实现了与EC模块相同级别的故障处理能力
3. ✅ **RAID10支持**：新增RAID10功能，提供镜像+条带化
4. ✅ **磨损均衡扩展**：智能平衡存储设备写入负载
5. ✅ **Superblock管理**：完善的元数据管理和清除功能
6. ✅ **SMART信息增强**：自动输出设备健康信息

这些功能显著提升了SPDK存储系统的：
- **可靠性**：通过冗余和自动重建保证数据安全
- **可维护性**：通过状态持久化和进度跟踪简化运维
- **性能**：通过优化算法和负载均衡提升I/O性能
- **可用性**：通过自动恢复和热插拔支持提升系统可用性

---

## 附录

### A. 相关文档

- `module/bdev/ec/EC_BDEV_ARCHITECTURE.md`: EC模块详细架构文档
- `module/bdev/ec/WEAR_LEVELING_FLOW.md`: 磨损均衡流程文档
- `module/bdev/raid/RAID_REBUILD_IMPROVEMENTS.md`: RAID重建改进文档

### B. RPC命令参考

#### EC模块RPC命令

- `bdev_ec_create`: 创建EC bdev
- `bdev_ec_delete`: 删除EC bdev
- `bdev_ec_get_bdevs`: 查询EC bdev信息
- `bdev_ec_add_base_bdev`: 添加base bdev
- `bdev_ec_remove_base_bdev`: 移除base bdev

#### RAID模块RPC命令

- `bdev_raid_create`: 创建RAID bdev
- `bdev_raid_delete`: 删除RAID bdev
- `bdev_raid_get_bdevs`: 查询RAID bdev信息
- `bdev_raid_set_options`: 设置RAID选项
- `bdev_raid_add_base_bdev`: 添加base bdev
- `bdev_raid_remove_base_bdev`: 移除base bdev

#### 通用RPC命令

- `bdev_wipe_superblock`: 清除superblock

### C. 故障排查

#### EC/RAID bdev无法自动重建

1. 检查superblock是否存在：`bdev_wipe_superblock`会清除superblock
2. 检查base bdev是否正确添加
3. 检查UUID是否匹配
4. 查看日志了解详细错误信息

#### 重建进度不更新

1. 检查是否正在重建：`bdev_ec_get_bdevs`或`bdev_raid_get_bdevs`
2. 检查base bdev状态
3. 检查是否有I/O错误

#### 性能问题

1. 调整重建窗口大小：`bdev_raid_set_options -w <size>`
2. 限制重建带宽：`bdev_raid_set_options -b <bandwidth>`
3. 检查base bdev性能

---

**文档版本**: 1.0  
**最后更新**: 2024年  
**作者**: SPDK开发团队
