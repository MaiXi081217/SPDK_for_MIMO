# MIMO - 高性能存储框架

## 项目概述
MIMO 是基于 SPDK 深度定制的高性能存储框架，核心目标包括：精简默认组件、强化 RAID/EC 功能、提供磨损均衡与 SMART 感知、以及扩展 Go 侧的外部通知能力。本项目将用户可见的所有输出统一为 MIMO 品牌标识，并提供灵活的 JSON 配置支持。

## 关键特性

### 核心功能
- **RAID / EC 增强**：从零实现 EC bdev 模块、补充重建逻辑、引入 RAID10、提供 superblock 清理流程，并交付详细的 RPC/CLI 与测试工具链
- **磨损均衡**：实现 NVMe SMART 采集与 wear-leveling 扩展，支持多策略选择、快速路径、健康信息缓存及降级逻辑
- **Go 通知桥接**：新增 `go/notifybridge` 模块，支持通过 JSON 配置文件灵活配置，在 NVMe 设备移除等事件时触发外部通知，方便与管控系统联动

### 品牌化与配置
- **MIMO 品牌标识**：所有用户可见输出统一为 MIMO 品牌，包括日志、错误消息、帮助信息等
- **JSON 配置支持**：Go 通知桥接支持通过配置文件或环境变量灵活配置所有参数，无需修改代码
- **精简构建**：清理上游文档/测试、裁剪 GitHub Actions、重写 Version 体系，确保仓库可在离线环境快速拉起

### 测试与文档
- **专用测试**：补充聚焦 RAID/EC 的专用单测、使用指南及 demo，保证新特性可验证、可学习
- **完整文档**：提供详细的使用文档、配置说明和 API 参考

## 提交时间线（严格按时间顺序）
1. **2025-10-16 · 4d80d5f – Initial commit for my customized SPDK**：引入自定义基线，包含完整 SPDK 树，作为后续所有差异的起点。
2. **2025-10-16 · 212cd76 – remove github actions workflow**：移除三条 GitHub Actions 工作流，避免离线构建被 CI 依赖阻断。
3. **2025-10-16 · 9cfa811 – 将重建逻辑默认为分配唯一uuid重建**：微调 NVMe bdev 重建逻辑，确保每次 rebuild 生成独立 UUID。
4. **2025-10-16 · e89c7f2 – 修改spdk输出为mimo品牌标识**：在 `mimo_tgt`、env DPDK 初始化及 reactor 中替换品牌信息，统一日志输出形象。
5. **2025-10-17 · 4389279 – Add submodules initialization**：为 dpdk、intel-ipsec-mb、isa-l-crypto、libvfio-user、ocf、xnvme 等配置 `.gitmodules` 并补齐初始记录。
6. **2025-10-20 · 92b0bcf – Add isa-l and update submodule info**：单独加入 isa-l 子模块，并更新 `.gitmodules` 指向。
7. **2025-10-20 · e1cc8c8 – remove submodules to init**：暂时移除多个子模块（dpdk、isa-l 系列、libvfio-user、xnvme），为后续重新初始化做准备。
8. **2025-10-20 · 01c85e1 – remove submodules to init**：继续移除 ocf 子模块，保持仓库干净状态。
9. **2025-10-20 · 0c93263 – Re-add submodules properly**：重新正确添加全部子模块引用，修复上一阶段的初始化问题。
10. **2025-10-21 · f88f709 – update dpdk submodule to spdk-25.03**：把 dpdk 子模块切换到 `spdk-25.03`，锁定可靠版本。
11. **2025-10-24 · 1b27af4 – 添加VERSION**：新增 `VERSION` 文件，便于手动标记发行版本。
12. **2025-10-24 · 6c96340 – 更新VERSION为json格式**：引入 `VERSION.json` 以 JSON 结构描述版本信息。
13. **2025-10-24 · cc1c966 – 删除原先VERSION**：移除旧格式 `VERSION`，准备切换新的生成方式。
14. **2025-10-24 · 05ba4e1 – VERSION need in make**：确保新版本文件被 Make 流程引用，避免构建缺失。
15. **2025-10-30 · c3d9394 – 删除test的编译，并修改sock名称**：大幅删除上游文档、测试和示例（重达 20 万行），同时改动顶层 `Makefile` 与若干脚本，使仓库更轻量并调整 sock 命名。
16. **2025-10-30 · ad98c00 – 修改配置文件默认不编译test**：在 `configure` 中默认关闭 test 相关构建，持续压缩依赖。
17. **2025-10-30 · 813b291 – 修改Makefile的编译程序命名**：微调 `app/spdk_tgt/Makefile`，统一编译生成的程序名为 `mimo_tgt`。
18. **2025-10-31 · ddbadf5 – 修改无法获取sock问题**：在 `include/spdk/init.h` 修正 socket 获取逻辑，防止初始化失败。
19. **2025-11-04 · ec06148 – 修改bdev_delete_raid命令**：让 `bdev_delete_raid` 在删除内存配置后同步清理 superblock，避免残留影响其他阵列。
20. **2025-11-04 · b66d063 – 修改raid创建时输出**：`bdev_raid_rpc.c` 及 CLI 在创建成功后直接打印 RAID 名称，提升可观测性。
21. **2025-11-05 · e87bce6 – 增加了ec模块，可以进行基本操作**：一次性引入完整 EC bdev 模块（3k+ 行）及 RPC/CLI 接口，并在 Makefile 中注册。
22. **2025-11-06 · 8b3eee6 – ec模块测试版本**：对 EC 模块进行大规模重构，新增 encode/io/internal 细分文件与 CLI 参数，支撑更多场景测试。
23. **2025-11-06 · 614e178 – 修改setup卡住问题**：修复 EC 初始化阶段的阻塞问题，保证 setup 流程可顺利退出。
24. **2025-11-07 · e37ba9e – ec**：补充 10 余个文件，包括 600 行的结构文档及大量代码优化，细化 EC 架构、RPC 行为与 Python CLI。
25. **2025-11-10 · fdeeab5 – 优化EC，增加bdev_get_bdevs输出**：引入块大小/块数量输出，继续优化 encode/io 路径并在 RAID 中同步逻辑。
26. **2025-11-11 · fb88e7d – 取消demo编译**：关闭 `module/bdev/Makefile` 中 demo 目标的编译，进一步缩短构建时间。
27. **2025-11-11 · ca601ad – 输出SMART磨损信息**：在 `bdev_nvme.c` 中为控制器添加时打印所有命名空间的 SMART/TBW，暴露磨损程度。
28. **2025-11-12 · 34f589b – 添加ec重建功能**：实现 EC 重建流水线、`bdev_ec_rebuild.c`、superblock 清理 RPC 及 CLI 参数，覆盖多场景恢复策略。
29. **2025-11-12 · c260966 – 优化代码逻辑**：针对上一提交的重建路径做进一步整理，增强状态同步与 superblock 维护。
30. **2025-11-13 · 9745120 – 增加raid10功能，优化EC重建**：新增 `raid10.c` 与 superblock 支撑，丰富 CLI/schema，同时继续精炼 EC 重建接口。
31. **2025-11-17 · 3871724 – Add wear leveling extension for EC bdev**：实现独立的 `wear_leveling_ext` 模块、注册流程及 Python 标志位，实现基于 SMART 的磨损均衡策略。
32. **2025-11-17 · 8f404e8 – 重构优化**：拆分 EC 超长函数、压缩 wear-leveling 代码，提升可维护性与可读性。
33. **2025-11-17 · d09142b – 修复编译警告**：在 wear-leveling 模块补齐头文件和显式类型转换，消除告警。
34. **2025-11-17 · 82e8f8c – 保存所有当前更改**：一次性提交大量文档（包括 FTL 计划、EC 详细指南、demo 教程）及 demo 程序，形成完整学习资料。
35. **2025-11-17 · 3330a62 – Simplify wear leveling mode handling**：调优 wear-leveling 模式切换逻辑与头文件接口，同时在 Python CLI 中同步行为。
36. **2025-11-18 · aaf2a49 – RAID重建功能改进（1）**：记录重建状态到 superblock、提供 JSON 进度 API、增加状态机/回调、引入恢复文档并修正 wear-leveling 依赖。
37. **2025-11-18 · abae2d9 – RAID重建功能改进（2）**：与上一提交相同的变更再次合入，确保不同分支保持一致。
38. **2025-11-18 · 5409926 – 添加RAID/EC/磨损均衡单测**：编写 35 个场景的 `bdev_raid_ec_wear_test.c` 及配套 Makefile/文档，覆盖重建、热插拔与磨损模式。
39. **2025-11-18 · f227e0e – 修复测试文件编译错误**：为新单测补足参数、修正静态函数调用，确保可编译运行。
40. **2025-11-18 · 621ddc4 – 修复 raid_bdev_sb_find_base_bdev_by_slot 调用**：统一测试内对静态函数的访问方式，避免链接失败。
41. **2025-11-18 · 6640bad – 清理不存在的 wear_leveling_ext.c 引用**：更新 EC Makefile，防止多余源文件导致构建报错。
42. **2025-11-18 · fc95a68 – 再次修复测试编译**：为测试 Makefile 添加环境库、移除不存在的依赖，并继续修正函数调用。
43. **2025-11-18 · 7e9e98a – 添加磨损均衡mock与 vmd 依赖**：为单测提供 wear-leveling mock，并补充 vmd 库链接，彻底解流程依赖。
44. **2025-11-18 · 76027c8 – 简化测试，使用 spdk_ut_run_tests**：改用统一测试入口、删去多余 mock，使单测结构更轻。
45. **2025-11-18 · 7be4156 – 修复编译警告**：把 mock 设为 static、移除未使用变量并临时注释未实现的重建进度 API。
46. **2025-11-18 · c817450 – 修复 unittest.mk 中 ENV_LIBS 未定义**：在 `mk/spdk.unittest.mk` 定义 ENV_LIBS，保证链接环境库。
47. **2025-11-18 · 077974c – 修复 unittest 链接命令**：将 ENV_LIBS 显式加入链接命令，防止缺库。
48. **2025-11-18 · 51b762f – 调整链接顺序**：把 ENV_LIBS 移到 LIBS 之前，解决链接顺序问题。
49. **2025-11-18 · 8f9700f – 添加 RPC 测试命令**：编写 `bdev_raid_test_rpc.c` / `bdev_ec_test_rpc.c`，提供创建、验证、重建进度等 RPC 场景以支撑测试。
50. **2025-11-18 · 99a293c – 将 bdev_raid_test_rpc.c 加入 Makefile**：确保新增 RPC 被 RAID 模块编译进来。
51. **2025-11-19 · 9e115f8 – update**：把所有 RPC 测试集中到 `lib/bdev/bdev_test_rpc.c`，移除旧的单测、脚本与文档，并适配 Makefile / CLI。
52. **2025-11-19 · 528a236 – 优化EC磨损分布代码**：抽取统一的 base bdev 选择辅助函数，重构 encode/io/sb 多处代码，减少重复并提升可读性。
53. **2025-11-19 · d002c51 – Merge branch 'raid-rebuild-improvements' into feature**：合并 RAID 重建分支，整合新的 bdev 测试 RPC、EC helper、python CLI 以及 unittest 构建修复。
54. **2025-11-19 · 2965f97 – 全面更新**：继续扩展 `bdev_test_rpc.c`、调整 EC encode/io/internal/ RPC，强化测试命令覆盖与入参校验。
55. **2025-11-19 · c3f7f2d – 实现了 Go 调用仓库**：新增 `go/notifybridge`（含 README、示例配置、模块依赖）及 C 头文件/入口，把 Go 进程桥接到 `mimo_tgt`。
56. **2025-11-19 · ee99cbd – Add Go notify hook for NVMe removal**：把 Go 钩子整合进通用 `mk/spdk.app.mk`，并在 `bdev_nvme.c` 中触发 NVMe 删除事件通知，同时精简 `mimo_tgt` Makefile。
57. **2025-11-20 · 831446d – 将用户可见输出中的 spdk 改为 mimo 并更新通知端口为 9988**：全面更新所有用户可见输出（日志、错误消息、帮助信息、文件路径等）为 MIMO 品牌标识，并实现 Go 通知桥接的 JSON 配置文件支持，支持通过配置文件或环境变量灵活配置所有参数。

## 快速开始

### 构建项目

```bash
cd /home/max/SPDK_for_MIMO
./configure --with-rdma --with-golang --with-raid5f --disable-tests
make -j$(nproc)
```

### 运行 MIMO Target

```bash
./build/bin/mimo_tgt
```

### 配置 Go 通知桥接

创建配置文件 `/etc/mimo/notify_config.json`：

```json
{
  "endpoint": "http://127.0.0.1:9988/mimo/events",
  "method": "POST",
  "timeout": "2s",
  "source": "mimo",
  "retry": 2,
  "retry_backoff_ms": 100
}
```

或通过环境变量指定：

```bash
export MIMO_NOTIFY_CONFIG=/path/to/notify_config.json
./build/bin/mimo_tgt
```

详细配置说明请参考 `go/notifybridge/README.md`。

## 主要模块

- **RAID/EC 模块**：支持 RAID0/1/5/10 和 EC（Erasure Coding），提供完整的重建、superblock 管理功能
- **磨损均衡**：基于 NVMe SMART 数据的智能磨损均衡策略
- **Go 通知桥接**：灵活的事件通知系统，支持 JSON 配置
- **MIMO Target**：高性能存储目标应用，统一 MIMO 品牌标识

## 文档

- [Go 通知桥接文档](go/notifybridge/README.md)
- [EC 模块文档](module/bdev/ec/README.md)
- [RAID 模块文档](module/bdev/raid/README.md)

> 如需查询某次提交的详细 diff，可执行 `git show <hash>`。

