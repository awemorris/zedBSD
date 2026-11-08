# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

# Project Toplevel Makefile

MAKEFLAGS += -r
.SUFFIXES:

all:        disk
setup:      toolchain .WAIT libc sysroot bootloader firmware
bootloader: toolchain .WAIT bootloader-platform
firmware:   toolchain .WAIT firmware-platform
libc:       toolchain .WAIT libc-platform
sysroot:    libc .WAIT sysroot-platform
vmunix:     sysroot .WAIT vmunix-platform
userland:   sysroot .WAIT programs
rootfs:     vmunix userland .WAIT rootfs-platform
disk:       vmunix rootfs firmware bootloader .WAIT image-platform

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

# If PLATFORM is empty, show a prompt and let user input.
ifeq ($(PLATFORM),)
$(info PLATFORM is not set. Please specify the target platform.)
$(info )
$(info Platforms:)
$(info   x86_64 (architecture: x86_64,  board: pcat))
$(info   rpi5   (architecture: arm64,   board: rpi5))
$(info   rpi4   (architecture: arm64,   board: rpi4))
$(info   vf2    (architecture: riscv64, board: vf2))
$(info   i386   (architecture: i386,    board: pcat))
$(info   pc9821 (architecture: i386,    board: pc98))
$(info   pc9801 (architecture: i386,    board: pc98))
override PLATFORM := $(shell printf 'Enter platform: ' >/dev/tty; IFS= read -r p </dev/tty && printf '%s\n' "$$p")
endif

# Import the platform default configuration.
-include mk/platform-$(PLATFORM)/defaults.mk

$(info PLATFORM:     $(PLATFORM))
$(info ARCHITECTURE: $(ARCHITECTURE))
$(info BOARD:        $(BOARD))

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
