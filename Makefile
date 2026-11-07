# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

# Project Toplevel Makefile

MAKEFLAGS += -r
.SUFFIXES:

all:        disk
setup:      toolchain libc sysroot bootloader firmware
libc:       toolchain libc-platform
sysroot:    toolchain libc-platform sysroot-platform
bootloader: bootloader-platform
firmware:   firmware-platform
vmunix:     toolchain libc-platform sysroot-platform vmunix-platform
userland:   toolchain libc-platform sysroot-platform programs
rootfs:     toolchain firmware-platform bootloader-platform libc-platform sysroot-platform vmunix-platform programs rootfs-platform
disk:       toolchain firmware-platform bootloader-platform libc-platform sysroot-platform vmunix-platform programs rootfs-platform image-platform

help:
	@echo "The zedBSD Build System"
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  setup      - Setup the build environment."
	@echo "  vmunix     - Build the kernel image."
	@echo "  userland   - Build the userland."
	@echo "  disk       - Build the boot disk image."
	@echo "  clean      - Clean build artifacts."

# Default configuration file (overridable)
CONFIGMK       := config.mk

# Host toolchain
HOST_CC        := cc
HOST_CXX       := c++
PYTHON         := python3

# Kernel toolchain (which we build)
CC             :=
AS             :=
LD             :=
RANLIB         :=
NM             :=
OBJCOPY        :=
OBJDUMP        :=
READELF        :=
STRIP          :=

# Version String
VERSION        := 1.0.0

# LTO setting. (full, thin, none)
KERNEL_LTO     := full

# Indicates that the build is done by the toplevel Makefile
TOPLEVEL_BUILD := 1

# Import the base configuration variables.
include mk/config.mk

# Import the current developer configuration.
include $(CONFIGMK)

# Import the platform default configuration.
-include mk/platform-$(PLATFORM)/defaults.mk

# Deterimine the build directory.
BUILD := build/$(PLATFORM)

# Toolchain. (targets: toolchain, toolchain-cached)
include mk/toolchain.mk

# Kernel sources.
include mk/kernel.mk

# libc sources.
include mk/libc.mk

# Userland build rules. (target: programs)
include mk/userland.mk

# Rootfs build rules. (target: rootfs)
include mk/rootfs.mk

# Platform build rules (targets: libc-platform, sysroot-platform, firmware-platform, bootloader-platform, vmunix-platform, rootfs-platform, image-platform)
include mk/platform-$(PLATFORM)/Makefile

# Foreign targets. (targets: keiland-linux, keiland-linux-install, keiland-freebsd, keiland-freebsd-install)
include mk/foreign.mk
