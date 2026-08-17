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
ifneq ($(filter $(ARCH),pc98 pcat),)
ZEDBSD_CPPFLAGS += -DHAL_ARCH_I386
else ifeq ($(ARCH),amd64)
ZEDBSD_CPPFLAGS += -DHAL_ARCH_AMD64
else ifeq ($(ARCH),arm64)
ZEDBSD_CPPFLAGS += -DHAL_ARCH_ARM64
else ifeq ($(ARCH),sparcv9)
ZEDBSD_CPPFLAGS += -DHAL_ARCH_SPARCV9
else ifeq ($(ARCH),x68k)
ZEDBSD_CPPFLAGS += -DHAL_ARCH_M68K
endif
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
	src/kern/net/unix-socket.c \
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

KERN_UFS1_SOURCES := \
	src/kern/ufs1/ufs1-endian.c \
	src/kern/ufs1/ufs1-super.c \
	src/kern/ufs1/ufs1-vfs.c
KERN_UFS1_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(KERN_UFS1_SOURCES))
KERN_UFS2_SOURCES := \
	src/kern/ufs2/ufs2-super.c \
	src/kern/ufs2/ufs2-vfs.c
KERN_UFS2_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(KERN_UFS2_SOURCES))
KERN_UFS_CONSISTENCY_SOURCES := \
	src/kern/ufs/ufs-journal.c \
	src/kern/ufs/ufs-softdep.c \
	src/kern/ufs/ufs-snapshot.c
KERN_UFS_CONSISTENCY_OBJS := $(patsubst %.c,$(BUILD)/%.o,\
	$(KERN_UFS_CONSISTENCY_SOURCES))
KERN_ACL_SOURCES := src/kern/posix-acl.c
KERN_ACL_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(KERN_ACL_SOURCES))
KERN_QUOTA_SOURCES := src/kern/quota.c
KERN_QUOTA_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(KERN_QUOTA_SOURCES))

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

HOST_TEST_CC := $(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -I. -Iinclude/uapi

# Compile the public headers in both supported data models.  These are object
# fixtures rather than host executables so the ILP32 check does not depend on
# the host having 32-bit startup objects or libraries installed.
UAPI_ABI_TEST_FLAGS := -std=c11 -Wall -Wextra -Werror -nostdinc \
	-Ilibc/include -Iinclude/uapi -Iinclude

$(BUILD)/tests/uapi-abi-ilp32.o: tests/uapi-abi-layout.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -m32 $(UAPI_ABI_TEST_FLAGS) -c $< -o $@

$(BUILD)/tests/uapi-abi-lp64.o: tests/uapi-abi-layout.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -m64 $(UAPI_ABI_TEST_FLAGS) -DZEDBSD_USER_ABI_LP64 \
		-c $< -o $@

uapi-abi-layout-check: $(BUILD)/tests/uapi-abi-ilp32.o \
	$(BUILD)/tests/uapi-abi-lp64.o
	@echo "zedBSD ILP32/LP64 UAPI layout check: PASS"

$(BUILD)/tests/posix-header-ilp32.o: tests/posix-header-compile.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -m32 $(UAPI_ABI_TEST_FLAGS) -c $< -o $@

$(BUILD)/tests/posix-header-lp64.o: tests/posix-header-compile.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -m64 $(UAPI_ABI_TEST_FLAGS) -DZEDBSD_USER_ABI_LP64 \
		-c $< -o $@

posix-header-check: $(BUILD)/tests/posix-header-ilp32.o \
	$(BUILD)/tests/posix-header-lp64.o
	@echo "zedBSD POSIX ILP32/LP64 public-header check: PASS"

posix-api-matrix-check: tests/posix-r2-api.csv \
	scripts/check-posix-api-matrix.py
	$(PYTHON) scripts/check-posix-api-matrix.py

toolchain-info:
	bash scripts/collect-toolchain-info.sh

regression-matrix-build:
	bash scripts/run-regression-matrix.sh build

regression-matrix-runtime:
	bash scripts/run-regression-matrix.sh runtime

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
	$(HOST_TEST_CC) -DHAL_ARCH_I386 -Iinclude -Isrc src/kern/elf.c $< -o $@

$(BUILD)/tests/elf-m68k-host-test: tests/elf-m68k-host-test.c src/kern/elf.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -DHAL_ARCH_M68K -DZEDBSD_USER_ABI_M68K -Iinclude -Isrc \
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

$(BUILD)/tests/x68k-keyboard-host-test: \
	tests/x68k-keyboard-host-test.c \
	src/hal/m68k/bsp-x68k/keyboard-map.c \
	src/hal/m68k/bsp-x68k/keyboard-map.h
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc \
		src/hal/m68k/bsp-x68k/keyboard-map.c $< -o $@

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
	drivers/x68k-mb89352.h tests/disk-host-stubs.c src/kern/buf.c \
	src/kern/sysctl.c src/kern/disk.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -pthread -Iinclude -Isrc -Ilibc/include \
		drivers/x68k-mb89352.c drivers/x68k-spc-disk.c \
		src/kern/buf.c src/kern/sysctl.c src/kern/disk.c \
		tests/disk-host-stubs.c $< -o $@

$(BUILD)/tests/sched-host-test: tests/sched-host-test.c src/kern/sched.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Dtid_t=int -DZEDBSD_SCHED_TEST -Iinclude -Isrc \
		src/kern/sched.c $< -o $@

$(BUILD)/tests/concurrency-host-test: tests/concurrency-host-test.c \
	src/kern/lock.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -pthread -Dtid_t=int -DZEDBSD_LOCKDEP -Iinclude -Isrc \
	src/kern/lock.c $< -o $@

$(BUILD)/tests/smp-contract-host-test: tests/smp-contract-host-test.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Dtid_t=int -Iinclude -Isrc $< -o $@

$(BUILD)/tests/ufs1-format-host-test: tests/ufs1-format-host-test.c \
	src/kern/ufs1/ufs1-endian.c src/kern/ufs1/ufs1-super.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/ufs1/ufs1-endian.c \
		src/kern/ufs1/ufs1-super.c $< -o $@

ufs1-format-host-test: $(BUILD)/tests/ufs1-format-host-test
	@$(BUILD)/tests/ufs1-format-host-test

$(BUILD)/tests/ufs2-format-host-test: tests/ufs2-format-host-test.c \
	src/kern/ufs1/ufs1-endian.c src/kern/ufs2/ufs2-super.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/ufs1/ufs1-endian.c \
		src/kern/ufs2/ufs2-super.c $< -o $@

ufs2-format-host-test: $(BUILD)/tests/ufs2-format-host-test
	@$(BUILD)/tests/ufs2-format-host-test

$(BUILD)/tests/ufs-consistency-host-test: tests/ufs-consistency-host-test.c \
	$(KERN_UFS_CONSISTENCY_SOURCES)
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc $(KERN_UFS_CONSISTENCY_SOURCES) $< -o $@

$(BUILD)/tests/ufs1-test.img: scripts/make-ufs1-image.py scripts/ufs1_format.py
	@mkdir -p $(dir $@)
	PYTHONPATH=scripts $(PYTHON) scripts/make-ufs1-image.py --size-mib 4 $@

$(BUILD)/tests/ufs1-multicg-test.img: scripts/make-ufs1-image.py scripts/ufs1_format.py
	@mkdir -p $(dir $@)
	PYTHONPATH=scripts $(PYTHON) scripts/make-ufs1-image.py --size-mib 16 \
		--cylinder-groups 4 $@

$(BUILD)/tests/ufs2-multicg-test.img: scripts/make-ufs2-image.py \
	scripts/ufs2_format.py scripts/ufs1_format.py
	@mkdir -p $(dir $@)
	PYTHONPATH=scripts $(PYTHON) scripts/make-ufs2-image.py --size-mib 16 \
		--cylinder-groups 4 --journal-mib 1 --snapshot-mib 2 $@

$(BUILD)/tests/ufs1-vfs-host-test: tests/ufs1-vfs-host-test.c \
	$(BUILD)/tests/ufs1-test.img $(VFS_CORE_SOURCES) $(KERN_UFS1_SOURCES)
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Iinclude/uapi -Isrc \
		-DUFS1_TEST_IMAGE='"$(BUILD)/tests/ufs1-test.img"' \
		$(VFS_CORE_SOURCES) $(KERN_UFS1_SOURCES) tests/vfs-host-stubs.c $< -o $@

$(BUILD)/tests/ufs1-multicg-vfs-host-test: tests/ufs1-vfs-host-test.c \
	$(BUILD)/tests/ufs1-multicg-test.img $(VFS_CORE_SOURCES) $(KERN_UFS1_SOURCES)
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Iinclude/uapi -Isrc \
		-DUFS1_TEST_IMAGE='"$(BUILD)/tests/ufs1-multicg-test.img"' \
		$(VFS_CORE_SOURCES) $(KERN_UFS1_SOURCES) tests/vfs-host-stubs.c $< -o $@

$(BUILD)/tests/ufs2-multicg-vfs-host-test: tests/ufs1-vfs-host-test.c \
	$(BUILD)/tests/ufs2-multicg-test.img $(VFS_CORE_SOURCES) \
	$(KERN_UFS1_SOURCES) $(KERN_UFS2_SOURCES) \
	$(KERN_UFS_CONSISTENCY_SOURCES) $(KERN_QUOTA_SOURCES)
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Iinclude/uapi -Isrc \
		-DUFS2_TEST_IMAGE='"$(BUILD)/tests/ufs2-multicg-test.img"' \
		$(VFS_CORE_SOURCES) src/kern/ufs1/ufs1-endian.c \
		src/kern/ufs2/ufs2-super.c src/kern/ufs2/ufs2-vfs.c \
		$(KERN_UFS_CONSISTENCY_SOURCES) $(KERN_QUOTA_SOURCES) \
		tests/vfs-host-stubs.c $< -o $@

$(BUILD)/tests/vmspace-host-test: tests/vmspace-host-test.c \
	src/kern/vmspace.c src/kern/vm-object.c tests/vm-sync-host-stubs.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/vmspace.c \
		src/kern/vm-object.c tests/vm-sync-host-stubs.c $< -pthread -o $@

$(BUILD)/tests/vm-commit-host-test: tests/vm-commit-host-test.c \
	src/kern/vm-commit.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/vm-commit.c $< -pthread -o $@

$(BUILD)/tests/swap-host-test: tests/swap-host-test.c src/kern/swap.c \
	tests/spin-host-stubs.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/swap.c \
		tests/spin-host-stubs.c $< -pthread -o $@

$(BUILD)/tests/vm-reclaim-host-test: tests/vm-reclaim-host-test.c \
	src/kern/vm-reclaim.c src/kern/swap.c tests/spin-host-stubs.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/kern/vm-reclaim.c \
		src/kern/swap.c tests/spin-host-stubs.c $< -pthread -o $@

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
	tests/net-sync-host-stubs.c \
	src/kern/net/packet-buf.c src/kern/net/net-device.c \
	src/kern/net/socket.c src/kern/net/packet-socket.c \
	src/kern/net/ethernet.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Iinclude/uapi -Ilibc/include \
		tests/net-sync-host-stubs.c \
		src/kern/net/packet-buf.c src/kern/net/net-device.c \
		src/kern/net/socket.c src/kern/net/packet-socket.c \
		src/kern/net/ethernet.c $< -o $@

INET_HOST_SOURCES := src/kern/net/packet-buf.c src/kern/net/net-device.c \
	src/kern/net/socket.c src/kern/net/checksum.c src/kern/net/ethernet.c \
	src/kern/net/route.c src/kern/net/inet-socket.c src/kern/net/arp.c \
	src/kern/net/ipv4.c src/kern/net/icmp.c src/kern/net/udp.c \
	src/kern/net/tcp.c

$(BUILD)/tests/inet-stack-host-test: tests/inet-stack-host-test.c \
	tests/net-sync-host-stubs.c $(INET_HOST_SOURCES)
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Iinclude/uapi -Ilibc/include \
		tests/net-sync-host-stubs.c $(INET_HOST_SOURCES) $< -o $@

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
	src/kern/buf.c src/kern/disk.c src/kern/inode.c src/kern/file.c src/kern/namecache.c \
	src/kern/namei.c src/kern/mount.c src/kern/rootfs.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(ZEDBSD_HOST_TEST_CFLAGS) -Iinclude -Iinclude/uapi -Isrc src/kern/fs.c \
		src/kern/namespace.c src/kern/env.c src/kern/buf.c src/kern/disk.c \
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
	tests/disk-host-stubs.c \
	src/kern/buf.c src/kern/disk.c src/kern/partition.c src/kern/pc98/partition.c \
	src/kern/mbr-partition.c src/kern/pc98/partition-auto.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Iinclude/uapi -Isrc src/kern/buf.c src/kern/disk.c src/kern/partition.c \
		src/kern/pc98/partition.c src/kern/mbr-partition.c \
		src/kern/pc98/partition-auto.c tests/disk-host-stubs.c $< -o $@

$(BUILD)/tests/bufcache-host-test: tests/bufcache-host-test.c \
	tests/disk-host-stubs.c src/kern/buf.c src/kern/sysctl.c src/kern/disk.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -pthread -Iinclude -Iinclude/uapi -Isrc src/kern/buf.c \
		src/kern/sysctl.c src/kern/disk.c tests/disk-host-stubs.c $< -o $@

$(BUILD)/tests/checkpoint-host-test: tests/checkpoint-host-test.c \
	tests/disk-host-stubs.c src/kern/test-checkpoint.c src/kern/buf.c \
	src/kern/disk.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -pthread -DZEDBSD_TEST_CHECKPOINTS \
		-Iinclude -Iinclude/uapi -Isrc src/kern/test-checkpoint.c \
		src/kern/buf.c src/kern/disk.c tests/disk-host-stubs.c $< -o $@

$(BUILD)/tests/fault-injection-host-test: tests/fault-injection-host-test.c \
	src/kern/test-fault.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -pthread -DZEDBSD_TEST_FAULTS -Iinclude -Isrc \
		src/kern/test-fault.c $< -o $@

VFS_CORE_SOURCES := src/kern/buf.c src/kern/disk.c src/kern/inode.c src/kern/file.c \
	src/kern/namecache.c src/kern/namei.c src/kern/mount.c src/kern/rootfs.c

$(BUILD)/tests/vfs-host-test: tests/vfs-host-test.c $(VFS_CORE_SOURCES)
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Iinclude/uapi -Isrc $(VFS_CORE_SOURCES) \
		tests/vfs-host-stubs.c $< -o $@

$(BUILD)/tests/cred-host-test: tests/cred-host-test.c src/kern/cred.c \
	$(KERN_ACL_SOURCES)
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Dtid_t=int -Iinclude -Iinclude/uapi -Isrc src/kern/cred.c \
		$(KERN_ACL_SOURCES) $< -o $@

$(BUILD)/tests/quota-host-test: tests/quota-host-test.c $(KERN_QUOTA_SOURCES)
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Iinclude/uapi -Isrc $(KERN_QUOTA_SOURCES) $< -o $@

$(BUILD)/tests/ufs-snapshot-host-test: tests/ufs-snapshot-host-test.c \
	src/kern/ufs/ufs-snapshot.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Iinclude/uapi -Isrc \
		src/kern/ufs/ufs-snapshot.c $< -o $@

$(BUILD)/tests/clock-rtc-host-test: tests/clock-rtc-host-test.c \
	src/hal/x86/rtc.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Isrc src/hal/x86/rtc.c $< -o $@

$(BUILD)/tests/sh-lexer-host-test: tests/sh-lexer-host-test.c \
	userland/sh/lexer.c userland/sh/lexer.h
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) userland/sh/lexer.c $< -o $@

$(BUILD)/tests/sh-expand-host-test: tests/sh-expand-host-test.c \
	userland/sh/expand.c userland/sh/expand.h userland/sh/lexer.c \
	userland/sh/lexer.h
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) userland/sh/lexer.c userland/sh/expand.c $< -o $@

HOST_TEST_BINARIES := $(BUILD)/tests/beui-host-test \
	$(BUILD)/tests/sh-lexer-host-test \
	$(BUILD)/tests/sh-expand-host-test \
	$(BUILD)/tests/blkdev-host-test \
	$(BUILD)/tests/bufcache-host-test \
	$(BUILD)/tests/checkpoint-host-test \
	$(BUILD)/tests/fault-injection-host-test \
	$(BUILD)/tests/vfs-host-test \
	$(BUILD)/tests/ufs1-vfs-host-test \
	$(BUILD)/tests/ufs1-multicg-vfs-host-test \
	$(BUILD)/tests/ufs2-multicg-vfs-host-test \
	$(BUILD)/tests/ufs-consistency-host-test \
	$(BUILD)/tests/cred-host-test \
	$(BUILD)/tests/quota-host-test \
	$(BUILD)/tests/ufs-snapshot-host-test \
	$(BUILD)/tests/clock-rtc-host-test \
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
	$(BUILD)/tests/x68k-keyboard-host-test \
	$(BUILD)/tests/m68k-exception-host-test \
	$(BUILD)/tests/m68k-fpu-frame-host-test \
	$(BUILD)/tests/x68k-partition-host-test \
	$(BUILD)/tests/x68k-scsi-host-test \
	$(BUILD)/tests/x68k-scsi-disk-host-test \
	$(BUILD)/tests/sched-host-test \
	$(BUILD)/tests/smp-contract-host-test \
	$(BUILD)/tests/concurrency-host-test \
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
CHECK_RUN_TARGETS := stdio-fs-host-test libc-host-test softfloat-host-test \
	uapi-abi-layout-check posix-header-check posix-api-matrix-check \
	ufs1-format-host-test ufs2-format-host-test

overlay-journal-format-host-test: tests/overlay-journal-format-host-test.py \
	scripts/overlay_journal_format.py
	PYTHONPATH=scripts $(PYTHON) tests/overlay-journal-format-host-test.py

CHECK_RUN_TARGETS += overlay-journal-format-host-test

ufs1-format-python-test: tests/ufs1-format-host-test.py \
	scripts/ufs1_format.py scripts/check-ufs1-image.py
	PYTHONPATH=scripts $(PYTHON) tests/ufs1-format-host-test.py

CHECK_RUN_TARGETS += ufs1-format-python-test

ufs2-format-python-test: tests/ufs2-format-host-test.py \
	scripts/ufs1_format.py scripts/ufs2_format.py scripts/check-ufs1-image.py \
	scripts/check-ufs2-image.py
	PYTHONPATH=scripts $(PYTHON) tests/ufs2-format-host-test.py

CHECK_RUN_TARGETS += ufs2-format-python-test

# ----------------------------------------------------------------------
# Architecture-specific rules (artifacts, disk images, QEMU tests,
# milestone verification chains).

include mk/arch-images.mk
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
	$(BUILD)/*/*/*/*.d $(BUILD)/*/*/*/*/*.d \
	$(BUILD)/*/*/*/*/*/*.d $(BUILD)/*/*/*/*/*/*/*.d)

.PHONY: all check clean distclean messages stdio-fs-host-test \
	overlay-journal-format-host-test uapi-abi-layout-check \
	posix-header-check posix-api-matrix-check ufs1-format-host-test \
	ufs2-format-host-test ufs1-format-python-test ufs2-format-python-test \
	toolchain-info regression-matrix-build \
	regression-matrix-runtime
