#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = mimo_tgt

C_SRCS := spdk_tgt.c

SPDK_LIB_LIST = $(ALL_MODULES_LIST)

SPDK_LIB_LIST += event event_iscsi event_nvmf

ifeq ($(SPDK_ROOT_DIR)/lib/env_dpdk,$(CONFIG_ENV))
SPDK_LIB_LIST += env_dpdk_rpc
endif

ifeq ($(OS),Linux)
SPDK_LIB_LIST += event_nbd
ifeq ($(CONFIG_UBLK),y)
SPDK_LIB_LIST += event_ublk
endif
ifeq ($(CONFIG_VHOST),y)
SPDK_LIB_LIST += event_vhost_blk event_vhost_scsi
endif
ifeq ($(CONFIG_VFIO_USER),y)
SPDK_LIB_LIST += event_vfu_tgt
endif
endif

ifeq ($(CONFIG_FSDEV),y)
SPDK_LIB_LIST += event_fsdev
endif

# Add Go notification bridge shared library
GO_NOTIFY_LIB_DIR := $(SPDK_ROOT_DIR)/build/lib
GO_NOTIFY_LIB := $(GO_NOTIFY_LIB_DIR)/libspdk_go_notify.so
GO_NOTIFY_SRC_DIR := $(SPDK_ROOT_DIR)/go/notifybridge
SYS_LIBS += -L$(GO_NOTIFY_LIB_DIR) -lspdk_go_notify -Wl,-rpath=$(GO_NOTIFY_LIB_DIR)
CFLAGS += -I$(SPDK_ROOT_DIR)/include

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk

# Build Go notification bridge shared library before linking
$(GO_NOTIFY_LIB):
	@echo "  GO BUILD $(notdir $@)"
	$(Q)mkdir -p $(GO_NOTIFY_LIB_DIR)
	$(Q)cd $(GO_NOTIFY_SRC_DIR) && go build -buildmode=c-shared -o $(GO_NOTIFY_LIB) .

$(APP): $(GO_NOTIFY_LIB)

install: $(APP)
	$(INSTALL_APP)

uninstall:
	$(UNINSTALL_APP)
