#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 7
SO_MINOR := 0

CFLAGS += -I$(SPDK_ROOT_DIR)/lib/bdev/
C_SRCS = bdev_ec.c bdev_ec_rpc.c bdev_ec_sb.c bdev_ec_io.c bdev_ec_encode.c bdev_ec_rebuild.c

# Add ISA-L include paths
CFLAGS += -I/usr/local/include/isa-l -I$(HOME)/isa-l/include
LDFLAGS += -lisal

LIBNAME = bdev_ec

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk

