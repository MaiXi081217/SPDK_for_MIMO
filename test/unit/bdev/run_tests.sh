#!/bin/bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 Intel Corporation.
#  All rights reserved.
#
# RAID/EC/磨损均衡模块单元测试运行脚本

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPDK_ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD_DIR="$SPDK_ROOT_DIR/build"
TEST_BINARY="$BUILD_DIR/test/unit/bdev/bdev_raid_ec_wear_test"

echo "=========================================="
echo "RAID/EC/磨损均衡模块单元测试"
echo "=========================================="
echo ""

# 检查测试二进制文件是否存在
if [ ! -f "$TEST_BINARY" ]; then
    echo "错误: 测试二进制文件不存在: $TEST_BINARY"
    echo "请先编译测试: cd $SPDK_ROOT_DIR && make -C test/unit/bdev"
    exit 1
fi

# 运行测试
echo "运行测试..."
echo ""

cd "$SPDK_ROOT_DIR"
"$TEST_BINARY" "$@"

TEST_RESULT=$?

echo ""
echo "=========================================="
if [ $TEST_RESULT -eq 0 ]; then
    echo "所有测试通过！"
else
    echo "测试失败，退出码: $TEST_RESULT"
fi
echo "=========================================="

exit $TEST_RESULT

