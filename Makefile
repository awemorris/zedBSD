# Boots — a scriptable bootstrap environment.
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
#
# Layout:
#   include/          public HAL and kernel interfaces
#   src/hal/          HAL and board support implementation
#   src/kern/         platform-neutral kernel services
#   src/noct/         temporary in-kernel Noct integration
#   libc/             freestanding libc subset
#   softfloat/        soft-float support built from vendor GCC/musl sources
#   platform/<arch>/  per-architecture targets (IPLs, stages, console)
#   apps/             Noct programs shipped on the boot volume
#   noct/             NoctLang submodule
#
# Architecture selection: `make ARCH=pc98` or `./build.sh pc98`.  Each
# architecture provides platform/<arch>/platform.mk and its artifacts land
# in build/<arch>/.  Architecture-neutral host artifacts stay shared at the
# top of build/ (build/host-noct, build/remacs, build/releases).

AS := as
LD := ld
OBJCOPY := objcopy
CC := gcc
HOSTCC ?= cc
PYTHON ?= python3
.DEFAULT_GOAL := all

ARCH ?= pc98
PLATFORM_MK := platform/$(ARCH)/platform.mk
ifeq ($(wildcard $(PLATFORM_MK)),)
$(error Unknown ARCH '$(ARCH)'; available: \
	$(patsubst platform/%/platform.mk,%,$(wildcard platform/*/platform.mk)))
endif

BUILD := build/$(ARCH)
SCRIPTS_DIR := scripts

# Scripts invoked from make must resolve the same architecture and build
# tree; direct invocations default to pc98 on their own.
export BOOTS_ARCH := $(ARCH)
export BOOTS_BUILD_DIR := $(CURDIR)/$(BUILD)

ASFLAGS := --32
BOOTS_CPPFLAGS := -nostdinc -Iinclude -Isrc -I. -I$(BUILD) -Ilibc/include
BOOTS_CFLAGS := -m32 -march=i386 -Os -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -Wall -Wextra -Werror

include libc/libc.mk
include softfloat/softfloat.mk
include noct.mk

# ----------------------------------------------------------------------
# Generic compile rules.  Per-object flag overrides use target-specific
# variables; header dependencies come from -MMD.

OBJ_CPPFLAGS = $(BOOTS_CPPFLAGS)
OBJ_CFLAGS = $(BOOTS_CFLAGS)

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CPPFLAGS) $(OBJ_CFLAGS) -MMD -MP -c $< -o $@

$(BOOTS_LIBC_OBJECTS): OBJ_CPPFLAGS = $(BOOTS_LIBC_CPPFLAGS)
$(BOOTS_LIBC_OBJECTS): OBJ_CFLAGS = $(BOOTS_LIBC_CFLAGS)

$(BUILD)/kern/messages.h: src/kern/messages.txt $(SCRIPTS_DIR)/generate-messages.py
	@mkdir -p $(dir $@)
	$(PYTHON) $(SCRIPTS_DIR)/generate-messages.py $< $@

# ----------------------------------------------------------------------
# Architecture-neutral host tests.  Platform makefiles append their own
# binaries to HOST_TEST_BINARIES and phony run targets to CHECK_RUN_TARGETS.

HOST_TEST_CC := $(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -I.

$(BUILD)/tests/fat-host-test: tests/fat-host-test.c \
	src/kern/fs.c src/kern/fat.c src/kern/fat16.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/fs.c src/kern/fat.c \
		src/kern/fat16.c $< -o $@

$(BUILD)/tests/fat-write-host-test: tests/fat-write-host-test.c \
	src/kern/fs.c src/kern/fat.c src/kern/fat16.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/fs.c src/kern/fat.c \
		src/kern/fat16.c $< -o $@

$(BUILD)/tests/env-host-test: tests/env-host-test.c src/kern/env.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -Wall -Wextra -Werror -I. -Iinclude -Isrc src/kern/env.c $< -o $@

$(BUILD)/tests/noct-memory-host-test: tests/noct-memory-host-test.c \
	src/noct/memory.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -Wall -Wextra -Werror -I. -Iinclude -Isrc src/noct/memory.c $< -o $@

$(BUILD)/tests/stdio-fs-host-test: tests/stdio-fs-host-test.c \
	src/kern/fs.c src/kern/namespace.c src/kern/env.c $(BOOTS_LIBC_SOURCES) \
	src/kern/disk.c src/kern/inode.c src/kern/file.c src/kern/namecache.c \
	src/kern/namei.c src/kern/mount.c src/kern/rootfs.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(BOOTS_HOST_TEST_CFLAGS) -Iinclude -Isrc src/kern/fs.c \
		src/kern/namespace.c src/kern/env.c src/kern/disk.c \
		src/kern/inode.c src/kern/file.c src/kern/namecache.c \
		src/kern/namei.c src/kern/mount.c src/kern/rootfs.c \
		$(BOOTS_LIBC_SOURCES) $< -o $@

stdio-fs-host-test: $(BUILD)/tests/stdio-fs-host-test
	$(BUILD)/tests/stdio-fs-host-test

# BeUI lives upstream in the Noct submodule, but Boots links it, so the
# upstream host tests run here against the very sources this tree builds.
BEUI_TEST_CC := $(HOST_TEST_CC) -I$(NOCT_ROOT)/include -I$(NOCT_ROOT)/src/api
BEUI_CORE_SOURCES := $(NOCT_ROOT)/src/api/beui-core.c \
	$(NOCT_ROOT)/src/api/beui-image.c

$(BUILD)/tests/beui-host-test: $(NOCT_ROOT)/tests/beui-test.c \
	$(BEUI_CORE_SOURCES)
	@mkdir -p $(dir $@)
	$(BEUI_TEST_CC) $(BEUI_CORE_SOURCES) $< -o $@

$(BUILD)/tests/blkdev-host-test: tests/blkdev-host-test.c \
	src/kern/disk.c src/kern/partition.c src/kern/pc98/partition.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/disk.c src/kern/partition.c \
		src/kern/pc98/partition.c $< -o $@

VFS_CORE_SOURCES := src/kern/disk.c src/kern/inode.c src/kern/file.c \
	src/kern/namecache.c src/kern/namei.c src/kern/mount.c src/kern/rootfs.c

$(BUILD)/tests/vfs-host-test: tests/vfs-host-test.c $(VFS_CORE_SOURCES)
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc $(VFS_CORE_SOURCES) $< -o $@

HOST_TEST_BINARIES := $(BUILD)/tests/beui-host-test \
	$(BUILD)/tests/blkdev-host-test \
	$(BUILD)/tests/vfs-host-test \
	$(BUILD)/tests/fat-host-test \
	$(BUILD)/tests/fat-write-host-test \
	$(BUILD)/tests/env-host-test \
	$(BUILD)/tests/noct-memory-host-test
CHECK_RUN_TARGETS := stdio-fs-host-test libc-host-test softfloat-host-test

# ----------------------------------------------------------------------
# Architecture-specific rules (artifacts, disk images, QEMU tests,
# milestone verification chains).

include $(PLATFORM_MK)

check: $(HOST_TEST_BINARIES) $(CHECK_RUN_TARGETS)
	@set -e; for test in $(HOST_TEST_BINARIES); do \
		echo "$$test"; $$test; done

clean:
	rm -rf $(BUILD)

distclean:
	rm -rf build

-include $(wildcard $(BUILD)/*.d $(BUILD)/*/*.d $(BUILD)/*/*/*.d \
	$(BUILD)/*/*/*/*.d)

.PHONY: all check clean distclean stdio-fs-host-test
