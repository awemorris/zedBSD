# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

# Project Toplevel Makefile

MAKEFLAGS += -r
.SUFFIXES:

all: disk

world: disk

help:
	@echo "zedBSD Build System"
	@echo ""
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  world      - Everything."
	@echo "  toolchain  - Setup the build environment."
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
MAKEFILES_BEFORE_CONFIG := $(MAKEFILE_LIST)
include $(CONFIGMK)

# The configuration file and every file it includes, which the packages'
# stamps follow (mk/userland.mk).
CONFIGMK_FILES := $(filter-out $(MAKEFILES_BEFORE_CONFIG),$(MAKEFILE_LIST))

# The same configuration file as an absolute path, for the nested makes
# (they run in other directories).
ZEDBSD_CONFIGMK := $(abspath $(CONFIGMK))

# If PLATFORM is empty, show a prompt and let user input.
ifeq ($(PLATFORM),)
$(info PLATFORM is not set. Please specify the target platform.)
$(info )
$(info Platforms:)
$(info   x86_64 (architecture: x86_64,  board: pcat))
$(info   intelmid64 (architecture: x86_64, board: pcat; Intel MID on edk2-intel-mid))
$(info   rpi5   (architecture: arm64,   board: rpi5))
$(info   rpi4   (architecture: arm64,   board: rpi4))
$(info   vf2    (architecture: riscv64, board: vf2))
$(info   i386   (architecture: i386,    board: pcat))
$(info   pc9821 (architecture: i386,    board: pc98))
$(info   pc9801ns (architecture: i386,    board: pc98))
override PLATFORM := $(shell printf 'Enter platform: ' >/dev/tty; IFS= read -r p </dev/tty && printf '%s\n' "$$p")
endif

# Import the platform default configuration.
-include mk/platform-$(PLATFORM)/defaults.mk

$(info PLATFORM:     $(PLATFORM))
$(info ARCHITECTURE: $(ARCHITECTURE))
$(info BOARD:        $(BOARD))

# Deterimine the build directory.
BUILD := build/$(PLATFORM)

export

toolchain:
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile toolchain-platform 

bootloader: toolchain
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile bootloader-platform

firmware:   toolchain
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile firmware-platform

libc: toolchain
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile libc-platform

sysroot: libc
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile sysroot-platform

vmunix: sysroot
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile vmunix-platform

userland: sysroot
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile programs

rootfs: vmunix userland
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile rootfs-platform

disk: vmunix rootfs firmware bootloader
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile image-platform

# Foreign targets. (targets: keiland-linux, keiland-linux-install, keiland-freebsd, keiland-freebsd-install)
include mk/foreign.mk
