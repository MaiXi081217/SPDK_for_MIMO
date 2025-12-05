# EC模块RPC命令参考

## 一、基本操作

### bdev_ec_get_bdevs - 查询EC设备列表

**参数**：
- `category` (可选): `all`(默认) / `online` / `configuring` / `offline`

**示例**：
```bash
./scripts/rpc.py bdev_ec_get_bdevs
./scripts/rpc.py bdev_ec_get_bdevs -c online
```

**返回**：设备列表，包含状态、rebuild进度、base_bdevs_list、spare_mode_summary等

---

### bdev_ec_create - 创建EC设备

**必需参数**：
- `name`: EC设备名称
- `k`: 数据块数量
- `p`: 校验块数量
- `base_bdevs`: base bdev名称列表

**可选参数**：
- `strip_size_kb` / `-z`: 条带大小（KB，2的幂次方）
- `uuid` / `--uuid`: EC设备UUID
- `superblock` / `-s`: 是否启用superblock（默认false，可用`-s`短选项）
- `wear_leveling_enabled` / `-w`: 是否启用磨损均衡（默认false，可用`-w`短选项）
- `debug_enabled` / `--debug-enabled`: 是否启用调试日志（默认false）
- `expansion_mode` / `-e`: 扩展模式（默认0，可用`-e`短选项）
  - `0`: NORMAL - base_bdevs数量必须等于k+p
  - `2`: SPARE - base_bdevs数量可以大于k+p，多余的作为spare

**参数说明**：
- 所有参数都支持短选项（如`-s`对应`--superblock`，`-e`对应`--expansion-mode`）
- 也可以使用全称（如`--superblock`，`--expansion-mode`）

**示例**：
```bash
# 普通模式（6+2）
./scripts/rpc.py bdev_ec_create \
  -n EC0 -k 6 -p 2 -z 64 \
  -b "Nvme0n1 Nvme0n2 Nvme0n3 Nvme0n4 Nvme0n5 Nvme0n6 Nvme0n7 Nvme0n8" \
  -s

# SPARE模式（6+2，10个盘，2个spare）- 使用短选项
./scripts/rpc.py bdev_ec_create \
  -n EC1 -k 6 -p 2 -z 64 \
  -b "Nvme0n1 Nvme0n2 Nvme0n3 Nvme0n4 Nvme0n5 Nvme0n6 Nvme0n7 Nvme0n8 Nvme0n9 Nvme0n10" \
  -e 2 -s

# 或使用全称
./scripts/rpc.py bdev_ec_create \
  -n EC1 -k 6 -p 2 --strip-size-kb 64 \
  --base-bdevs "Nvme0n1 Nvme0n2 Nvme0n3 Nvme0n4 Nvme0n5 Nvme0n6 Nvme0n7 Nvme0n8 Nvme0n9 Nvme0n10" \
  --expansion-mode 2 --superblock
```

---

### bdev_ec_delete - 删除EC设备

**参数**：
- `name`: EC设备名称

**示例**：
```bash
./scripts/rpc.py bdev_ec_delete -n EC0
```

---

## 二、设备管理

### bdev_ec_add_base_bdev - 添加base设备

**参数**：
- `ec_bdev`: EC设备名称
- `base_bdev`: base bdev名称

**示例**：
```bash
./scripts/rpc.py bdev_ec_add_base_bdev -e EC0 -b Nvme0n9
```

**注意**：rebuild进行中时会被拒绝；SPARE模式下新设备标记为spare

---

### bdev_ec_remove_base_bdev - 移除base设备

**参数**：
- `name`: base bdev名称

**示例**：
```bash
./scripts/rpc.py bdev_ec_remove_base_bdev -n Nvme0n1
```

---

## 三、磨损均衡

### bdev_ec_set_selection_strategy - 设置设备选择策略

**参数**：
- `name`: EC设备名称
- `stripe_group_size` (可选): 条带组大小
- `wear_leveling_enabled` (可选): 是否启用磨损均衡
- `debug_enabled` (可选): 是否启用调试日志
- `refresh_wear_levels` (可选): 是否立即刷新磨损级别

**示例**：
```bash
./scripts/rpc.py bdev_ec_set_selection_strategy \
  -n EC0 --wear-leveling-enabled true --refresh-wear-levels true
```

---

### bdev_ec_refresh_wear_levels - 刷新磨损级别

**参数**：
- `name`: EC设备名称

**示例**：
```bash
./scripts/rpc.py bdev_ec_refresh_wear_levels -n EC0
```

---

## 四、常用操作

### 创建并监控SPARE模式EC

```bash
# 创建
./scripts/rpc.py bdev_ec_create -n EC0 -k 6 -p 2 \
  --base-bdevs "Nvme0n1 Nvme0n2 Nvme0n3 Nvme0n4 Nvme0n5 Nvme0n6 Nvme0n7 Nvme0n8 Nvme0n9 Nvme0n10" \
  --expansion-mode 2 --superblock true

# 查看状态
./scripts/rpc.py bdev_ec_get_bdevs | jq '.[] | select(.name=="EC0")'

# 查看spare汇总
./scripts/rpc.py bdev_ec_get_bdevs | jq '.[] | select(.name=="EC0") | .spare_mode_summary'

# 查看rebuild进度
./scripts/rpc.py bdev_ec_get_bdevs | jq '.[] | select(.name=="EC0") | .rebuild'
```

---

## 五、JSON输出字段

**主要字段**：`name`, `uuid`, `k`, `p`, `strip_size_kb`, `state`, `expansion_mode`, `rebalance_state`

**rebuild对象**（如果正在进行）：`target`, `target_slot`, `progress` (current_stripe, total_stripes, percent, state)

**base_bdevs_list**：每个base bdev的 `name`, `uuid`, `role` (active/spare/failed), `is_configured`, `is_failed`

**spare_mode_summary**（SPARE模式）：`expansion_mode`, `active`, `spare`, `failed` 数量

