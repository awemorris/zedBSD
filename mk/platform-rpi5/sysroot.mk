# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# rpi5 (ARM64) sysroot build rules
#

PLATFORM_RPI5_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT         ?= $(abspath $(PLATFORM_RPI5_DIR)/../..)

# The sysroot is in the build directory, where the packages look for it.
SYSROOT     := $(abspath $(BUILD))/sysroot
SYSROOT_INC := $(SYSROOT)/usr/include
SYSROOT_LIB := $(SYSROOT)/usr/lib

PUBLIC_HEADERS := $(shell find include/libc include/uapi userland/desktop/include -type f ! -name '*~' 2>/dev/null | LC_ALL=C sort)

SYSROOT_COMPLETE_STAMP  := $(SYSROOT)/.zedbsd-sysroot-complete
HEADERS_INSTALLED_STAMP := $(SYSROOT)/.headers-installed

.PHONY: sysroot-platform
sysroot-platform: $(SYSROOT_COMPLETE_STAMP)

# Install public headers into sysroot
$(HEADERS_INSTALLED_STAMP): $(PUBLIC_HEADERS)
	@mkdir -p $(SYSROOT_INC)
	@for header in $(PUBLIC_HEADERS); do \
		case "$$header" in \
		include/libc/*) relative="$${header#include/libc/}" ;; \
		include/uapi/*) relative="$${header#include/}" ;; \
		userland/desktop/include/*) relative="$${header#userland/desktop/include/}" ;; \
		*) continue ;; \
		esac; \
		mkdir -p "$$(dirname "$(SYSROOT_INC)/$$relative")"; \
		cp -f "$$header" "$(SYSROOT_INC)/$$relative"; \
	done
	@touch $@

# Complete sysroot by copying libraries from libc build and verifying with smoke link
$(SYSROOT_COMPLETE_STAMP): $(HEADERS_INSTALLED_STAMP) $(LIBC_ALL_TARGETS)
	@mkdir -p $(SYSROOT_LIB)/zedbsd/arm64 $(SYSROOT_LIB)/zedbsd/rpi5 $(SYSROOT_LIB)/zedbsd/aarch64 $(BUILD)/dynamic
	cp -f $(LIBC_OUT_DIR)/libc.a $(LIBC_OUT_DIR)/libc.so $(SYSROOT_LIB)/
	cp -f $(LIBC_OUT_DIR)/libm.a $(LIBC_OUT_DIR)/libm.so $(SYSROOT_LIB)/
	cp -f $(LIBC_OUT_DIR)/libutil.a $(LIBC_OUT_DIR)/libutil.so $(SYSROOT_LIB)/
	cp -f $(LIBC_OUT_DIR)/libc.o $(SYSROOT_LIB)/
	cp -f $(LIBC_OUT_DIR)/crt0.o $(LIBC_OUT_DIR)/crt1.o $(SYSROOT_LIB)/
	cp -f $(LIBC_OUT_DIR)/libzedbsd-compiler-rt.a $(LIBC_OUT_DIR)/libzedbsd-compiler-rt.o $(SYSROOT_LIB)/
	cp -f $(LIBC_OUT_DIR)/libclang_rt.builtins.a $(SYSROOT_LIB)/
	cp -f $(LIBC_OUT_DIR)/ld.so $(SYSROOT_LIB)/
	cp -f $(LIBC_OUT_DIR)/libc.so $(LIBC_OUT_DIR)/libm.so $(LIBC_OUT_DIR)/libutil.so $(LIBC_OUT_DIR)/ld.so $(BUILD)/dynamic/
	cp -f mk/platform-rpi5/user.ld mk/platform-rpi5/vmunix.ld $(SYSROOT_LIB)/zedbsd/arm64/
	cp -f mk/platform-rpi5/user.ld mk/platform-rpi5/vmunix.ld $(SYSROOT_LIB)/zedbsd/rpi5/
	cp -f mk/platform-rpi5/user.ld mk/platform-rpi5/vmunix.ld $(SYSROOT_LIB)/zedbsd/aarch64/
	@printf '%s\n' '#include <stdint.h>' 'int main(void) { return (int)sizeof(uintptr_t); }' > $(SYSROOT)/smoke.c
	$(CC) --target=aarch64-unknown-zedbsd --sysroot=$(SYSROOT) \
		-march=armv8-a -mno-outline-atomics -DHAL_ARCH_ARM64 -DKERN_USER_ABI_AARCH64 -DKERN_USER_ABI_LP64 \
		-nostdinc -isystem $(SYSROOT_INC) \
		-ffreestanding -fno-pic -fno-pie -fno-stack-protector \
		-c $(SYSROOT)/smoke.c -o $(SYSROOT)/smoke.o
	$(CC) --target=aarch64-unknown-zedbsd --sysroot=$(SYSROOT) \
		-nostdlib -static -Wl,--build-id=none \
		-Wl,-T,$(SYSROOT_LIB)/zedbsd/arm64/user.ld \
		$(SYSROOT_LIB)/crt0.o $(SYSROOT)/smoke.o \
		-Wl,--start-group $(SYSROOT_LIB)/libc.a \
		$(SYSROOT_LIB)/libzedbsd-compiler-rt.a \
		$(SYSROOT_LIB)/libclang_rt.builtins.a -Wl,--end-group \
		-o $(SYSROOT)/smoke.elf
	@$(READELF) -h $(SYSROOT)/smoke.elf | grep -F 'AArch64' >/dev/null
	@if $(NM) -u $(SYSROOT)/smoke.elf | grep .; then \
		echo "sysroot: smoke link retained undefined symbols" >&2; exit 1; \
	fi
	@touch $@
