#  SPDX-License-Identifier: BSD-3-Clause
#  Makefile for demo bdev module

# 设置SPDK根目录（相对于当前目录）
SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)

# 包含SPDK通用makefile规则
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

# 共享库版本号
SO_VER := 7
SO_MINOR := 0

# 源文件列表
C_SRCS = bdev_demo.c bdev_demo_rpc.c

# 库名称（会生成libbdev_demo.so）
LIBNAME = bdev_demo

# 使用空映射文件（不需要导出符号）
SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

# 包含SPDK库构建规则
include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk

