#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk

TEST_FILE = bdev_raid_ec_wear_test.c

# Enable linking with env library for unit tests
UNIT_TEST_LINK_ENV = 1

SPDK_LIB_LIST += bdev_raid bdev_ec bdev json jsonrpc rpc thread trace util log nvme

CFLAGS += -I$(SPDK_ROOT_DIR)/module/bdev/raid
CFLAGS += -I$(SPDK_ROOT_DIR)/module/bdev/ec
CFLAGS += -I$(SPDK_ROOT_DIR)/module/bdev

