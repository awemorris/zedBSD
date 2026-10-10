# zedBSD; Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Executes the production GENET paths against a bounded host hardware model.
REPO := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
OUT ?= $(REPO)/build/ws203-host
CC ?= cc
TESTS := $(REPO)/plan/ws203/tests
DTB ?= $(REPO)/vendor/raspberrypi-firmware/boot/bcm2711-rpi-4-b.dtb
CPPFLAGS := -D_POSIX_C_SOURCE=200809L -I$(TESTS)/include -I$(REPO)/include -I$(REPO)/src -I$(REPO)
CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS := -fsanitize=address,undefined
SOURCES := $(TESTS)/genet-host-test.c $(REPO)/src/drivers/ethernet/bcm54213pe.c \
	$(REPO)/src/drivers/platform/rpi4/rpi4-ethernet.c $(REPO)/src/drivers/generic/fdt.c

.PHONY: run
run: $(OUT)/genet-host-test
	$(OUT)/genet-host-test $(DTB)

$(OUT)/genet-host-test: $(SOURCES) $(REPO)/include/drivers/ethernet/bcm54213pe.h $(REPO)/src/drivers/platform/rpi4/rpi4-ethernet.h $(TESTS)/include/kern/thread.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) -o $@
