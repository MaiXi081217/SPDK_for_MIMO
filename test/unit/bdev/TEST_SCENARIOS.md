# RAID/EC/磨损均衡模块单元测试场景文档

## 测试覆盖范围

本测试套件覆盖以下功能模块的所有主要场景：

### 一、RAID 模块测试

#### 1. 基本功能测试
- ✅ **test_raid_bdev_create**: 创建 RAID bdev（各种 RAID 级别）
- ✅ **test_raid_bdev_add_base_bdev**: 添加 base bdev
- ✅ **test_raid_write_info_json**: JSON 信息输出

#### 2. 重建功能测试
- ✅ **test_raid_rebuild_state_persistence**: REBUILDING 状态持久化
- ✅ **test_raid_rebuild_progress_api**: 重建进度查询 API
- ✅ **test_raid_rebuild_start_stop**: 启动和停止重建
- ✅ **test_rebuild_resume_after_restart**: 系统重启后恢复重建

#### 3. 热插拔测试
- ✅ **test_raid_hotplug_add**: 热插拔添加磁盘
- ✅ **test_raid_hotplug_remove**: 热插拔移除磁盘
- ✅ **test_hotplug_scenario_raid1_degraded**: RAID1 降级模式场景
- ✅ **test_hotplug_scenario_concurrent_operations**: 并发操作场景

### 二、EC 模块测试

#### 1. 基本功能测试
- ✅ **test_ec_bdev_create**: 创建 EC bdev（k+p 配置）
- ✅ **test_ec_bdev_add_base_bdev**: 添加 base bdev
- ✅ **test_ec_fail_base_bdev**: 标记 base bdev 为失败

#### 2. 重建功能测试
- ✅ **test_ec_rebuild_state_persistence**: REBUILDING 状态持久化
- ✅ **test_ec_rebuild_progress_api**: 重建进度查询 API
- ✅ **test_ec_rebuild_start_stop**: 启动和停止重建

#### 3. 热插拔测试
- ✅ **test_ec_hotplug_add**: 热插拔添加磁盘
- ✅ **test_ec_hotplug_remove**: 热插拔移除磁盘
- ✅ **test_hotplug_scenario_ec_failure_recovery**: EC 故障恢复场景

### 三、磨损均衡模块测试

#### 1. 基本功能测试
- ✅ **test_wear_leveling_register**: 注册磨损均衡扩展（三种模式）
- ✅ **test_wear_leveling_set_mode**: 动态切换模式
- ✅ **test_wear_leveling_set_tbw**: 设置 TBW 参数

#### 2. 自动降级测试
- ✅ **test_wear_leveling_auto_fallback**: 自动降级机制（FULL -> SIMPLE -> DISABLED）

## 详细重建场景测试（RAID 和 EC）

### 场景 1: 旧盘拔出，remove 是否清除 superblock
**描述**: 旧盘拔出后，RAID 按情况看是否能运行，拔出后需手动 remove 旧盘，remove 是否尝试清除 superblock

**测试步骤**:
1. 创建 RAID1/EC bdev（带 superblock）
2. 模拟一个磁盘物理拔出（标记为失败）
3. 验证 RAID/EC 是否能在降级模式运行（RAID1 应该可以，EC 取决于 k+p）
4. 手动调用 `remove_base_bdev`
5. 验证 remove 是否尝试清除 superblock（通过 `raid_bdev_wipe_single_base_bdev_superblock`）

**验证点**:
- ✅ remove 操作会调用 superblock 清除函数
- ✅ 即使清除失败，remove 操作也会继续
- ✅ superblock 状态更新为 MISSING 或 FAILED

### 场景 2: 新盘加入后自动重建
**描述**: 旧盘拔出，插入新盘，是否能手动加入 RAID，加入后能否识别是新盘并自动重建并更新超级块

**测试步骤**:
1. 创建 RAID1/EC bdev（带 superblock）
2. 模拟一个磁盘物理拔出（标记为 MISSING）
3. 更新 superblock 状态为 MISSING
4. 模拟插入新盘（UUID 不同或为 NULL）
5. 手动调用 `add_base_bdev`
6. 验证系统是否识别为新盘（UUID 不匹配或为 NULL）
7. 验证是否自动启动重建
8. 验证 superblock 是否更新（状态变为 REBUILDING）

**验证点**:
- ✅ UUID 不匹配或为 NULL 时识别为新盘
- ✅ 自动启动重建（`raid_bdev_start_rebuild`）
- ✅ superblock 状态更新为 REBUILDING
- ✅ 重建进度初始化为 0

### 场景 3: 重建中断后恢复
**描述**: 旧盘拔出，插入新盘，手动加入 RAID 后重建过程中，如果将新盘拔出，是否能保留重建中状态，下次添加是否能识别并自动重建

**测试步骤**:
1. 创建 RAID1/EC bdev（带 superblock）
2. 一个磁盘失败，插入新盘，启动重建
3. 重建进行中（更新进度到 superblock）
4. 模拟新盘被拔出（`remove_base_bdev`）
5. 验证 superblock 状态是否保留为 REBUILDING
6. 验证重建进度是否保留
7. 模拟重新插入新盘（或同一新盘）
8. 验证系统是否识别 REBUILDING 状态
9. 验证是否从保存的进度继续重建

**验证点**:
- ✅ REBUILDING 状态保留在 superblock
- ✅ 重建进度（offset, total_size）保留
- ✅ 重新添加时识别 REBUILDING 状态
- ✅ 从保存的进度继续重建（`rebuild_offset`）

### 场景 4: 重建完成后旧盘重新插入
**描述**: 旧盘拔出，插入新盘，手动加入 RAID，重建完成，插入旧盘，如果旧盘的 superblock 没有清除，是否识别到 RAID 已经重建完成并不尝试加入 RAID 组，是否会提示清除 superblock

**测试步骤**:
1. 创建 RAID1/EC bdev（带 superblock）
2. 一个磁盘失败，插入新盘，重建完成
3. 模拟旧盘重新插入（superblock 未清除，UUID 匹配）
4. 验证系统是否识别到 RAID 已经重建完成
5. 验证是否不尝试加入 RAID 组（slot 已被占用）
6. 验证是否会提示清除 superblock

**验证点**:
- ✅ 识别到 slot 已被占用（CONFIGURED 状态）
- ✅ 拒绝加入 RAID 组
- ✅ 提示用户清除 superblock
- ✅ UUID 匹配但 slot 被占用时正确处理

### 场景 5: 不同 RAID 组的 superblock 冲突
**描述**: 插入的新盘手动加入 RAID 后有不是同一组的 RAID 的 superblock，是否会拒绝加入并提示清除超级快后加入

**测试步骤**:
1. 创建 RAID1/EC bdev（raid_bdev1）
2. 创建另一个 RAID1/EC bdev（raid_bdev2）
3. 模拟一个磁盘属于 raid_bdev2（有 raid_bdev2 的 superblock）
4. 尝试将这个磁盘添加到 raid_bdev1
5. 验证系统是否检测到 UUID 不匹配（不同 RAID 组）
6. 验证是否拒绝加入
7. 验证是否会提示清除 superblock

**验证点**:
- ✅ 检测到 UUID 不匹配（不同 RAID 组）
- ✅ 拒绝加入目标 RAID 组
- ✅ 提示清除 superblock 后重新加入
- ✅ 防止数据损坏

### 场景 6: 修复后的磁盘重新加入
**描述**: 坏盘拔出，修复好之后能否加入

**测试步骤**:
1. 创建 RAID1/EC bdev（带 superblock）
2. 一个磁盘失败（标记为 FAILED）
3. 插入新盘，重建完成
4. 修复旧盘，重新插入（UUID 匹配）
5. 验证系统是否识别到这是修复后的旧盘
6. 验证是否可以重新加入（作为 spare 或替换）
7. 验证是否需要重建（因为数据可能已过时）

**验证点**:
- ✅ 识别到 UUID 匹配且状态是 FAILED
- ✅ 可以重新加入（如果 slot 可用）
- ✅ 需要重建（因为数据可能已过时）
- ✅ 正确处理修复后的磁盘

## 其他测试场景详细说明

### 场景 7: RAID1 降级模式
**描述**: 一个磁盘失败后，系统继续运行在降级模式，替换磁盘后自动重建

**测试步骤**:
1. 创建 RAID1 bdev（2 个磁盘）
2. 模拟一个磁盘失败
3. 验证系统继续运行（降级模式）
4. 添加替换磁盘
5. 验证自动启动重建
6. 验证重建完成后状态恢复

### 场景 2: EC 故障恢复
**描述**: 一个数据块失败，使用冗余块恢复，替换失败块后重建

**测试步骤**:
1. 创建 EC bdev（k=2, p=2）
2. 模拟一个数据块失败
3. 验证可以使用冗余块恢复数据
4. 添加替换块
5. 验证自动启动重建
6. 验证重建完成后冗余恢复

### 场景 3: 并发操作
**描述**: 重建进行中时进行热插拔，多个磁盘同时故障

**测试步骤**:
1. 创建 RAID10 bdev
2. 启动重建
3. 在重建进行中热插拔另一个磁盘
4. 验证操作正确处理
5. 模拟多个磁盘同时故障
6. 验证系统正确处理

### 场景 4: 系统重启后恢复重建
**描述**: 系统重启后从 superblock 恢复重建状态和进度

**测试步骤**:
1. 创建带 superblock 的 RAID bdev
2. 启动重建并更新进度
3. 模拟系统重启（保存状态到 superblock）
4. 重新加载 superblock
5. 验证 REBUILDING 状态恢复
6. 验证重建进度恢复
7. 验证从恢复的进度继续重建

### 场景 5: 磨损均衡自动降级
**描述**: 连续失败后自动从 FULL 模式降级到 SIMPLE，再到 DISABLED

**测试步骤**:
1. 注册磨损均衡扩展（FULL 模式）
2. 模拟连续 5 次获取磨损信息失败
3. 验证自动降级到 SIMPLE 模式
4. 继续模拟失败
5. 验证自动降级到 DISABLED 模式
6. 模拟成功获取磨损信息
7. 验证失败计数器重置

## 测试运行方法

### 编译测试
```bash
cd /home/max/SPDK_for_MIMO
make -C test/unit/bdev
```

### 运行测试
```bash
# 方法 1: 使用脚本
./test/unit/bdev/run_tests.sh

# 方法 2: 直接运行
./build/test/unit/bdev/bdev_raid_ec_wear_test
```

### 运行特定测试套件
```bash
# 只运行 RAID 测试
./build/test/unit/bdev/bdev_raid_ec_wear_test -s raid_bdev

# 只运行 EC 测试
./build/test/unit/bdev/bdev_raid_ec_wear_test -s ec_bdev

# 只运行磨损均衡测试
./build/test/unit/bdev/bdev_raid_ec_wear_test -s wear_leveling
```

## 注意事项

1. **Mock 数据**: 某些测试需要真实的 bdev，当前使用 mock 数据，实际测试中需要真实的 NVMe bdev
2. **异步操作**: 某些操作是异步的，测试中可能需要等待回调完成
3. **Superblock**: 某些测试需要 superblock 支持，确保创建 bdev 时启用 superblock
4. **线程环境**: SPDK 需要线程环境，测试中需要初始化 SPDK 环境

## 扩展测试建议

### 性能测试
- 重建速度测试
- 热插拔响应时间测试
- 磨损均衡选择算法性能测试

### 压力测试
- 大量并发 I/O 操作
- 频繁热插拔操作
- 长时间运行稳定性测试

### 边界条件测试
- 最大/最小 k+p 值
- 超大/超小 strip size
- 极端磨损值（0%, 100%）

