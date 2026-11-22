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

# Default configuration file.
CONFIGMK       := config.mk

# Host toolchain
HOST_CC        := cc
HOST_CXX       := c++
PYTHON         := python3

# Version String
VERSION        := 1.0.0

# LTO setting. (full, thin, none)
KERNEL_LTO     := full

# Indicates that the build is done by the toplevel Makefile
TOPLEVEL_BUILD := 1

# Import the current developer configuration.
include $(CONFIGMK)

# If PLATFORM is empty, show a prompt and let user input.
ifeq ($(PLATFORM),)
$(info PLATFORM is not set. Please specify the target platform, e.g., make PLATFORM=x86_64)
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
exit 1
endif

# Show the current configuration.
$(info PLATFORM:     $(PLATFORM))
$(info ARCHITECTURE: $(ARCHITECTURE))
$(info BOARD:        $(BOARD))

# Deterimine the build directory.
BUILD := build/$(PLATFORM)

EXPORT_LIST := \
	BUILD=$(abspath $(BUILD)) \
	ZEDBSD_CONFIGMK=$(abspath $(CONFIGMK)) \
	HOST_CC=$(HOST_CC) \
	HOST_CXX=$(HOST_CXX) \
	PYTHON=$(PYTHON) \
	VERSION=$(VERSION) \
	KERNEL_LTO=$(KERNEL_LTO) \
	TOPLEVEL_BUILD=$(TOPLEVEL_BUILD)

toolchain:
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile toolchain-platform $(EXPORT_LIST)

toolchain-cached:
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile toolchain-cached-platform $(EXPORT_LIST)

bootloader: toolchain
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile bootloader-platform $(EXPORT_LIST)

firmware: toolchain
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile firmware-platform $(EXPORT_LIST)

firmware-licenses:
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile firmware-licenses-platform $(EXPORT_LIST)

firmware-licenses-pending:
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile firmware-licenses-pending-platform $(EXPORT_LIST)

libc: toolchain
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile libc-platform $(EXPORT_LIST)

sysroot: libc
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile sysroot-platform $(EXPORT_LIST)

vmunix: sysroot
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile vmunix-platform $(EXPORT_LIST)

userland: sysroot
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile programs $(EXPORT_LIST)

rootfs: vmunix userland
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile rootfs-platform $(EXPORT_LIST)

disk: vmunix rootfs firmware bootloader
	$(MAKE) -f mk/platform-$(PLATFORM)/Makefile image-platform $(EXPORT_LIST)

# Foreign targets. (targets: keiland-linux, keiland-linux-install, keiland-freebsd, keiland-freebsd-install)
include mk/foreign.mk
