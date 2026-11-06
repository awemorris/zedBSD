# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Project Toplevel Makefile
#

# Host toolchain
HOST_CC        := cc
HOST_CXX       := c++

# Kernel toolchain which we build
CC             :=
AS             :=
LD             :=
RANLIB         :=
NM             :=
OBJCOPY        :=
OBJDUMP        :=
READELF        :=
STRIP          :=

# Python3
PYTHON         := python3

# Configuration File
CONFIGMK       := config.mk

# Base Configuration
PLATFORM       := x86_64
ARCHITECTURE   := x86_64
BOARD          := pcat
VARIANT        := native

# Version String
VERSION        := 1.0.0

# LTO setting. (full, thin, none)
KERNEL_LTO     := full

# "zedimage-host" is a host-side tool to create UFS and disk images.
IMAGE_HOST     := build/zedimage-host

# Indicates that the build is done by the toplevel Makefile
TOPLEVEL_BUILD := 1

# The default goal for the toplevel Makefile is "disk"
all: disk

help:
	@echo "zedBSD Build System"
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  setup      - Setup the build environment."
	@echo "  vmunix     - Build the kernel image."
	@echo "  userland   - Build the userland."
	@echo "  disk       - Build the boot disk image."
	@echo "  clean      - Clean build artifacts."

#
# Toolchain.
#
# Defines:
#  - toolchain: Build the toolchain.
#  - toolchain-cached: Download and use a prebuilt toolchain if available.
#

include mk/toolchain.mk

#
# Configuration
#
# No targets are defined here.
#

# Import the config variable definitions.
include mk/config-list.mk

# Override the config variables by the config.mk file.
include $(CONFIGMK)

#
# Source Files
#

# Import the kernel source file list.
include mk/sources-kern.mk

# Import the libc source file list.
include mk/sources-libc.mk

# Deterimen the build directory.
BUILD := build/$(PLATFORM)

# rootfs file list.
include mk/rootfs-list.mk

# Determine the platform-specific Makefile
PLATFORM_MAKEFILE := mk/platform-$(PLATFORM)/Makefile

# Include the platform-specific Makefile. Exports:
#  - vmunix-$(PLATFORM)
#  - libc-$(PLATFORM)
#  - sysroot-$(PLATFORM)
#  - rootfs-$(PLATFORM)
#  - image-$(PLATFORM)
include $(PLATFORM_MAKEFILE)

setup:      toolchain libc sysroot bootloader firmware
libc:       toolchain libc-$(PLATFORM)
sysroot:    toolchain libc-$(PLATFORM) sysroot-$(PLATFORM)
bootloader: bootloader-$(PLATFORM)
firmware:   firmware-$(PLATFORM)
vmunix:     toolchain libc-$(PLATFORM) sysroot-$(PLATFORM) vmunix-$(PLATFORM)
userland:   toolchain libc-$(PLATFORM) sysroot-$(PLATFORM) programs
rootfs:     toolchain libc-$(PLATFORM) sysroot-$(PLATFORM) programs rootfs-$(PLATFORM)
disk:       toolchain libc-$(PLATFORM) sysroot-$(PLATFORM) programs rootfs-$(PLATFORM) image-$(PLATFORM)

#
# Userland Program Makefile
#

ifeq ($(strip $(CONFIG_USERLAND_PROGRAMS)),)
CONFIG_USERLAND_PROGRAMS := $(USERLAND_PROGRAMS)
endif
CONFIG_USERLAND_PROGRAMS := $(sort $(CONFIG_USERLAND_PROGRAMS) sh init cron getty syslogd networkd mount fsck sysctl net)

ALL_PROGRAM_MAKEFILES := $(sort \
	$(wildcard userland/base/*/Makefile))

SELECTED_PROGRAM_MAKEFILES := $(foreach mf, $(ALL_PROGRAM_MAKEFILES), \
	$(if $(filter $(notdir $(patsubst %/,%,$(dir $(mf)))), $(CONFIG_USERLAND_PROGRAMS)), \
		$(mf)))

PROGRAM_DIRS := $(dir $(SELECTED_PROGRAM_MAKEFILES))
ALL_PROGRAM_DIRS := $(dir $(ALL_PROGRAM_MAKEFILES))

PROGRAM_STAMPS := $(foreach d,$(PROGRAM_DIRS),$(BUILD)/packages/$(notdir $(patsubst %/,%,$(d)))/.stamp)

define PROGRAM_RULE
$$(BUILD)/packages/$(notdir $(patsubst %/,%,$(1)))/.stamp: $$(wildcard $(1)* $(1)*/* $(1)*/*/*) userland/base/package.mk
	@case "$(1)" in \
		userland/packages/*) \
			$$(MAKE) -C $(1) ;; \
		*) \
			$$(MAKE) -C $(1) CC="$$(CC)" CPPFLAGS="$$(USER_CPPFLAGS)" CFLAGS="$$(USER_CFLAGS)" AS="$$(AS)" LD="$$(LD)" RANLIB="$$(RANLIB)" NM="$$(NM)" OBJCOPY="$$(OBJCOPY)" OBJDUMP="$$(OBJDUMP)" READELF="$$(READELF)" STRIP="$$(STRIP)" ;; \
	esac
endef

$(foreach d,$(ALL_PROGRAM_DIRS),$(eval $(call PROGRAM_RULE,$(d))))

$(ALL_PROGRAM_DIRS) $(patsubst %/,%,$(ALL_PROGRAM_DIRS)): %: $(BUILD)/packages/$$(notdir $$(patsubst %/,%,%))/.stamp

.PHONY: packages

programs: libc $(PROGRAM_STAMPS)
