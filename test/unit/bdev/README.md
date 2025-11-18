# RAID/EC/磨损均衡模块单元测试

## 概述

本测试套件提供了对 SPDK RAID、EC 和磨损均衡模块的全面单元测试，覆盖了所有主要功能和场景。

## 测试内容

### RAID 模块测试（10 个测试用例）
1. **基本功能**
   - 创建 RAID bdev（各种 RAID 级别）
   - 添加 base bdev
   - JSON 信息输出

2. **重建功能**
   - REBUILDING 状态持久化
   - 重建进度查询 API
   - 启动和停止重建
   - 系统重启后恢复重建

3. **热插拔**
   - 热插拔添加/移除磁盘
   - RAID1 降级模式场景
   - 并发操作场景

### EC 模块测试（9 个测试用例）
1. **基本功能**
   - 创建 EC bdev（k+p 配置）
   - 添加 base bdev
   - 标记 base bdev 为失败

2. **重建功能**
   - REBUILDING 状态持久化
   - 重建进度查询 API
   - 启动和停止重建

3. **热插拔**
   - 热插拔添加/移除磁盘
   - EC 故障恢复场景

### 磨损均衡模块测试（4 个测试用例）
1. **基本功能**
   - 注册磨损均衡扩展（三种模式：DISABLED/SIMPLE/FULL）
   - 动态切换模式
   - 设置 TBW 参数

2. **自动降级**
   - 自动降级机制（FULL -> SIMPLE -> DISABLED）

## 编译和运行

### 前置要求
- CUnit 开发库（`libcunit-dev` 或 `CUnit-devel`）
- SPDK 已编译
- 所有依赖库已构建

### 编译测试
```bash
cd /home/max/SPDK_for_MIMO
make -C test/unit/bdev
```

### 运行测试
```bash
# 方法 1: 使用脚本（推荐）
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

### 运行特定测试用例
```bash
# 运行单个测试
./build/test/unit/bdev/bdev_raid_ec_wear_test -s raid_bdev -t test_raid_bdev_create
```

## 测试场景说明

详细测试场景请参考 [TEST_SCENARIOS.md](TEST_SCENARIOS.md)

### 主要测试场景
1. **RAID1 降级模式**: 磁盘失败后继续运行，替换后自动重建
2. **EC 故障恢复**: 使用冗余块恢复数据，替换后重建
3. **并发操作**: 重建进行中时热插拔，多磁盘同时故障
4. **系统重启恢复**: 从 superblock 恢复重建状态和进度
5. **磨损均衡自动降级**: 连续失败后自动降级模式

## 测试输出

测试运行后会显示：
- 每个测试用例的执行结果（PASS/FAIL）
- 测试套件统计信息
- 失败测试的详细信息

示例输出：
```
==========================================
RAID/EC/磨损均衡模块单元测试
==========================================

Running Suite: raid_bdev
  Test: test_raid_bdev_create ... passed
  Test: test_raid_bdev_add_base_bdev ... passed
  ...

Running Suite: ec_bdev
  Test: test_ec_bdev_create ... passed
  ...

Running Suite: wear_leveling
  Test: test_wear_leveling_register ... passed
  ...

==========================================
所有测试通过！
==========================================
```

## 注意事项

1. **Mock 数据**: 某些测试使用 mock 数据，实际测试中需要真实的 NVMe bdev
2. **异步操作**: 某些操作是异步的，测试中可能需要等待回调完成
3. **Superblock**: 某些测试需要 superblock 支持，确保创建 bdev 时启用
4. **线程环境**: SPDK 需要线程环境，测试中需要初始化 SPDK 环境

## 扩展测试

### 建议添加的测试
- **性能测试**: 重建速度、热插拔响应时间
- **压力测试**: 大量并发 I/O、频繁热插拔
- **边界条件**: 最大/最小 k+p、极端磨损值

## 故障排查

### 编译错误
- 检查 CUnit 库是否安装
- 检查 SPDK 库是否已编译
- 检查依赖库路径是否正确

### 运行时错误
- 检查 SPDK 环境是否初始化
- 检查是否有足够的权限访问设备
- 查看日志输出获取详细信息

## 贡献

添加新测试时：
1. 在相应的测试套件中添加测试函数
2. 使用 `CU_ADD_TEST` 注册测试
3. 更新本文档和 TEST_SCENARIOS.md

