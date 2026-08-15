# zedBSD — a scriptable bootstrap environment.
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
#
# Layout:
#   include/          public HAL and kernel interfaces
#   src/hal/          HAL and board support implementation
#   src/kern/         platform-neutral kernel services
#   userland/        statically linked user programs and their libc glue
#   userland/noct/   zedBSD Noct runtime, integration, and upstream submodule
#   libc/             freestanding libc subset
#   softfloat/        soft-float support built from vendor GCC/musl sources
#   platform/<arch>/  per-architecture targets (IPLs, stages, console)
#   apps/             generic Noct programs shipped on the boot volume
#
# Architecture selection: `make ARCH=pc98` or `./build.sh all pc98`.  Each
# architecture provides platform/<arch>/platform.mk and its artifacts land
# in build/<arch>/.  Architecture-neutral host artifacts stay shared at the
# top of build/ (build/host-noct, build/releases).

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
export ZEDBSD_ARCH := $(ARCH)
export ZEDBSD_BUILD_DIR := $(CURDIR)/$(BUILD)

ASFLAGS := --32
ZEDBSD_CPPFLAGS := -nostdinc -Iinclude -Iinclude/uapi -Isrc -I. -I$(BUILD) -Ilibc/include
ZEDBSD_CFLAGS := -m32 -march=i386 -Os -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -Wall -Wextra -Werror

include libc/libc.mk
include softfloat/softfloat.mk
include noct.mk

KERN_NET_SOURCES := \
	src/kern/net/packet-buf.c \
	src/kern/net/net-device.c \
	src/kern/net/core.c \
	src/kern/net/socket.c \
	src/kern/net/socket-file.c \
	src/kern/net/packet-socket.c \
	src/kern/net/checksum.c \
	src/kern/net/ethernet.c \
	src/kern/net/route.c \
	src/kern/net/inet-socket.c \
	src/kern/net/arp.c \
	src/kern/net/ipv4.c \
	src/kern/net/icmp.c \
	src/kern/net/udp.c \
	src/kern/net/tcp.c
KERN_NET_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(KERN_NET_SOURCES))

# ----------------------------------------------------------------------
# Generic compile rules.  Per-object flag overrides use target-specific
# variables; header dependencies come from -MMD.

OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
OBJ_CFLAGS = $(ZEDBSD_CFLAGS)
OBJ_CC = $(CC)

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(OBJ_CC) $(OBJ_CPPFLAGS) $(OBJ_CFLAGS) -MMD -MP -c $< -o $@

$(ZEDBSD_LIBC_OBJECTS): OBJ_CC = $(ZEDBSD_LIBC_CC)
$(ZEDBSD_LIBC_OBJECTS): OBJ_CPPFLAGS = $(ZEDBSD_LIBC_CPPFLAGS)
$(ZEDBSD_LIBC_OBJECTS): OBJ_CFLAGS = $(ZEDBSD_LIBC_CFLAGS)

$(BUILD)/kern/messages.h: src/kern/messages.txt $(SCRIPTS_DIR)/generate-messages.py
	@mkdir -p $(dir $@)
	$(PYTHON) $(SCRIPTS_DIR)/generate-messages.py $< $@

messages: $(BUILD)/kern/messages.h

# ----------------------------------------------------------------------
# Architecture-neutral host tests.  Platform makefiles append their own
# binaries to HOST_TEST_BINARIES and phony run targets to CHECK_RUN_TARGETS.

HOST_TEST_CC := $(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -I.

$(BUILD)/tests/fat-host-test: tests/fat-host-test.c \
	src/kern/fs.c src/kern/fat.c src/kern/fat-lfn.c src/kern/fat16.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/fs.c src/kern/fat.c \
		src/kern/fat-lfn.c src/kern/fat16.c $< -o $@

$(BUILD)/tests/fat-write-host-test: tests/fat-write-host-test.c \
	src/kern/fs.c src/kern/fat.c src/kern/fat-lfn.c src/kern/fat16.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/fs.c src/kern/fat.c \
		src/kern/fat-lfn.c src/kern/fat16.c $< -o $@

$(BUILD)/tests/fat32-host-test: tests/fat32-host-test.c \
	src/kern/fs.c src/kern/fat.c src/kern/fat-lfn.c src/kern/fat16.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/fs.c src/kern/fat.c \
		src/kern/fat-lfn.c src/kern/fat16.c $< -o $@

$(BUILD)/tests/env-host-test: tests/env-host-test.c src/kern/env.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -Wall -Wextra -Werror -I. -Iinclude -Isrc src/kern/env.c $< -o $@

$(BUILD)/tests/noct-memory-host-test: tests/noct-memory-host-test.c \
	userland/noct/integration/memory.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -Wall -Wextra -Werror -I. -Iinclude -Isrc userland/noct/integration/memory.c $< -o $@

$(BUILD)/tests/user-noct-memory-host-test: \
	tests/user-noct-memory-host-test.c userland/noct/runtime/memory.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Iinclude/uapi -Isrc \
		userland/noct/runtime/memory.c $< -o $@

$(BUILD)/tests/heap-context-host-test: tests/heap-context-host-test.c \
	libc/heap.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) libc/heap.c $< -o $@

$(BUILD)/tests/elf-host-test: tests/elf-host-test.c src/kern/elf.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/elf.c $< -o $@

$(BUILD)/tests/elf-m68k-host-test: tests/elf-m68k-host-test.c src/kern/elf.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -DZEDBSD_USER_ABI_M68K -Iinclude -Isrc \
		src/kern/elf.c $< -o $@

$(BUILD)/tests/m68k-mmu-host-test: tests/m68k-mmu-host-test.c \
	src/hal/m68k/mmu030.h
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc $< -o $@

$(BUILD)/tests/m68k-space-host-test: tests/m68k-space-host-test.c \
	src/hal/m68k/space.c src/hal/m68k/space.h src/hal/m68k/mmu030.h
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/hal/m68k/space.c $< -o $@

$(BUILD)/tests/x68k-memory-map-host-test: \
	tests/x68k-memory-map-host-test.c \
	src/hal/m68k/bsp-x68k/memory-map.c \
	src/hal/m68k/bsp-x68k/memory-map.h
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc \
		src/hal/m68k/bsp-x68k/memory-map.c $< -o $@

$(BUILD)/tests/x68k-handoff-host-test: tests/x68k-handoff-host-test.c \
	src/hal/m68k/bsp-x68k/handoff.c src/hal/m68k/bsp-x68k/bsp.h
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc \
		src/hal/m68k/bsp-x68k/handoff.c $< -o $@

$(BUILD)/tests/x68k-mmio-host-test: tests/x68k-mmio-host-test.c \
	src/hal/m68k/bsp-x68k/mmio.h
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc $< -o $@

$(BUILD)/tests/m68k-exception-host-test: \
	tests/m68k-exception-host-test.c src/hal/m68k/exception.c \
	src/hal/m68k/exception.h
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/hal/m68k/exception.c $< -o $@

$(BUILD)/tests/m68k-fpu-frame-host-test: \
	tests/m68k-fpu-frame-host-test.c src/hal/m68k/fpu.c src/hal/m68k/fpu.h
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/hal/m68k/fpu.c $< -o $@

$(BUILD)/tests/x68k-partition-host-test: \
	tests/x68k-partition-host-test.c src/kern/x68k/partition.c \
	include/kern/x68k-partition.h
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/x68k/partition.c $< -o $@

$(BUILD)/tests/x68k-scsi-host-test: tests/x68k-scsi-host-test.c \
	drivers/x68k-mb89352.c drivers/x68k-mb89352.h
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc drivers/x68k-mb89352.c $< -o $@

$(BUILD)/tests/x68k-scsi-disk-host-test: \
	tests/x68k-scsi-disk-host-test.c drivers/x68k-spc-disk.c \
	drivers/x68k-spc-disk.h drivers/x68k-mb89352.c \
	drivers/x68k-mb89352.h src/kern/disk.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc -Ilibc/include \
		drivers/x68k-mb89352.c drivers/x68k-spc-disk.c \
		src/kern/disk.c $< -o $@

$(BUILD)/tests/sched-host-test: tests/sched-host-test.c src/kern/sched.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Dtid_t=int -Iinclude -Isrc src/kern/sched.c $< -o $@

$(BUILD)/tests/vmspace-host-test: tests/vmspace-host-test.c \
	src/kern/vmspace.c src/kern/vm-object.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/vmspace.c \
		src/kern/vm-object.c $< -o $@

$(BUILD)/tests/vm-commit-host-test: tests/vm-commit-host-test.c \
	src/kern/vm-commit.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/vm-commit.c $< -o $@

$(BUILD)/tests/swap-host-test: tests/swap-host-test.c src/kern/swap.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/swap.c $< -o $@

$(BUILD)/tests/vm-reclaim-host-test: tests/vm-reclaim-host-test.c \
	src/kern/vm-reclaim.c src/kern/swap.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/vm-reclaim.c \
		src/kern/swap.c $< -o $@

$(BUILD)/tests/packet-buf-host-test: tests/packet-buf-host-test.c \
	src/kern/net/packet-buf.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Iinclude/uapi -Ilibc/include \
		src/kern/net/packet-buf.c $< -o $@

$(BUILD)/tests/net-device-host-test: tests/net-device-host-test.c \
	src/kern/net/packet-buf.c src/kern/net/net-device.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Iinclude/uapi -Ilibc/include \
		src/kern/net/packet-buf.c src/kern/net/net-device.c $< -o $@

$(BUILD)/tests/packet-socket-host-test: tests/packet-socket-host-test.c \
	src/kern/net/packet-buf.c src/kern/net/net-device.c \
	src/kern/net/socket.c src/kern/net/packet-socket.c \
	src/kern/net/ethernet.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Iinclude/uapi -Ilibc/include \
		src/kern/net/packet-buf.c src/kern/net/net-device.c \
		src/kern/net/socket.c src/kern/net/packet-socket.c \
		src/kern/net/ethernet.c $< -o $@

INET_HOST_SOURCES := src/kern/net/packet-buf.c src/kern/net/net-device.c \
	src/kern/net/socket.c src/kern/net/checksum.c src/kern/net/ethernet.c \
	src/kern/net/route.c src/kern/net/inet-socket.c src/kern/net/arp.c \
	src/kern/net/ipv4.c src/kern/net/icmp.c src/kern/net/udp.c \
	src/kern/net/tcp.c

$(BUILD)/tests/inet-stack-host-test: tests/inet-stack-host-test.c \
	$(INET_HOST_SOURCES)
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Iinclude/uapi -Ilibc/include \
		$(INET_HOST_SOURCES) $< -o $@

$(BUILD)/tests/dhcp-host-test: tests/dhcp-host-test.c userland/net/dhcp.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Iinclude/uapi -Ilibc/include \
		userland/net/dhcp.c $< -o $@

$(BUILD)/tests/dns-host-test: tests/dns-host-test.c \
	userland/libc/resolver-dns.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Iinclude/uapi -Ilibc/include \
		userland/libc/resolver-dns.c $< -o $@

$(BUILD)/tests/stdio-fs-host-test: tests/stdio-fs-host-test.c \
	tests/vfs-host-stubs.c \
	src/kern/fs.c src/kern/namespace.c src/kern/env.c $(ZEDBSD_LIBC_SOURCES) \
	src/kern/disk.c src/kern/inode.c src/kern/file.c src/kern/namecache.c \
	src/kern/namei.c src/kern/mount.c src/kern/rootfs.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(ZEDBSD_HOST_TEST_CFLAGS) -Iinclude -Isrc src/kern/fs.c \
		src/kern/namespace.c src/kern/env.c src/kern/disk.c \
		src/kern/inode.c src/kern/file.c src/kern/namecache.c \
		src/kern/namei.c src/kern/mount.c src/kern/rootfs.c \
		tests/vfs-host-stubs.c \
		$(ZEDBSD_LIBC_SOURCES) $< -o $@

stdio-fs-host-test: $(BUILD)/tests/stdio-fs-host-test
	$(BUILD)/tests/stdio-fs-host-test

# BeUI lives upstream in the Noct submodule, but zedBSD links it, so the
# upstream host tests run here against the very sources this tree builds.
BEUI_TEST_CC := $(HOST_TEST_CC) -I$(NOCT_ROOT)/include -I$(NOCT_ROOT)/src/api
BEUI_CORE_SOURCES := $(NOCT_ROOT)/src/api/beui-core.c \
	$(NOCT_ROOT)/src/api/beui-image.c

$(BUILD)/tests/beui-host-test: $(NOCT_ROOT)/tests/testcases/beui-test.c \
	$(BEUI_CORE_SOURCES)
	@mkdir -p $(dir $@)
	$(BEUI_TEST_CC) $(BEUI_CORE_SOURCES) $< -o $@

$(BUILD)/tests/blkdev-host-test: tests/blkdev-host-test.c \
	src/kern/disk.c src/kern/partition.c src/kern/pc98/partition.c \
	src/kern/mbr-partition.c src/kern/pc98/partition-auto.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/disk.c src/kern/partition.c \
		src/kern/pc98/partition.c src/kern/mbr-partition.c \
		src/kern/pc98/partition-auto.c $< -o $@

VFS_CORE_SOURCES := src/kern/disk.c src/kern/inode.c src/kern/file.c \
	src/kern/namecache.c src/kern/namei.c src/kern/mount.c src/kern/rootfs.c

$(BUILD)/tests/vfs-host-test: tests/vfs-host-test.c $(VFS_CORE_SOURCES)
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc $(VFS_CORE_SOURCES) \
		tests/vfs-host-stubs.c $< -o $@

HOST_TEST_BINARIES := $(BUILD)/tests/beui-host-test \
	$(BUILD)/tests/blkdev-host-test \
	$(BUILD)/tests/vfs-host-test \
	$(BUILD)/tests/fat-host-test \
	$(BUILD)/tests/fat-write-host-test \
	$(BUILD)/tests/fat32-host-test \
	$(BUILD)/tests/env-host-test \
	$(BUILD)/tests/noct-memory-host-test \
	$(BUILD)/tests/user-noct-memory-host-test \
	$(BUILD)/tests/heap-context-host-test \
	$(BUILD)/tests/elf-host-test \
	$(BUILD)/tests/elf-m68k-host-test \
	$(BUILD)/tests/m68k-mmu-host-test \
	$(BUILD)/tests/m68k-space-host-test \
	$(BUILD)/tests/x68k-memory-map-host-test \
	$(BUILD)/tests/x68k-handoff-host-test \
	$(BUILD)/tests/x68k-mmio-host-test \
	$(BUILD)/tests/m68k-exception-host-test \
	$(BUILD)/tests/m68k-fpu-frame-host-test \
	$(BUILD)/tests/x68k-partition-host-test \
	$(BUILD)/tests/x68k-scsi-host-test \
	$(BUILD)/tests/x68k-scsi-disk-host-test \
	$(BUILD)/tests/sched-host-test \
	$(BUILD)/tests/vmspace-host-test \
	$(BUILD)/tests/vm-commit-host-test \
	$(BUILD)/tests/swap-host-test \
	$(BUILD)/tests/vm-reclaim-host-test \
	$(BUILD)/tests/packet-buf-host-test \
	$(BUILD)/tests/net-device-host-test \
	$(BUILD)/tests/packet-socket-host-test \
	$(BUILD)/tests/inet-stack-host-test \
	$(BUILD)/tests/dhcp-host-test \
	$(BUILD)/tests/dns-host-test
CHECK_RUN_TARGETS := stdio-fs-host-test libc-host-test softfloat-host-test

# ----------------------------------------------------------------------
# Architecture-specific rules (artifacts, disk images, QEMU tests,
# milestone verification chains).

include $(PLATFORM_MK)
include bootloader/unified/unified.mk

check: $(HOST_TEST_BINARIES) $(CHECK_RUN_TARGETS)
	@set -e; for test in $(HOST_TEST_BINARIES); do \
		echo "$$test"; $$test; done

clean:
	rm -rf $(BUILD)

distclean:
	rm -rf build

-include $(wildcard $(BUILD)/*.d $(BUILD)/*/*.d $(BUILD)/*/*/*.d \
	$(BUILD)/*/*/*/*.d)

.PHONY: all check clean distclean messages stdio-fs-host-test
