# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# i386 sysroot build rules
#

PLATFORM_PC9821_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT ?= $(abspath $(PLATFORM_PC9821_DIR)/../..)

include $(REPO_ROOT)/build-jobs.mk

SYSROOT     := $(BUILD)/sysroot
SYSROOT_INC := $(SYSROOT)/usr/include
SYSROOT_LIB := $(SYSROOT)/usr/lib

PUBLIC_HEADERS := $(shell find include/libc include/uapi userland/desktop/include -type f ! -name '*~' 2>/dev/null | LC_ALL=C sort)

SYSROOT_COMPLETE_STAMP := $(SYSROOT)/.zedbsd-sysroot-complete
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
	@mkdir -p $(SYSROOT_LIB)/zedbsd/i386 $(SYSROOT_LIB)/zedbsd/pc9821 $(BUILD)/dynamic
	cp -f $(LIBC_OUT_DIR)/libc.a $(LIBC_OUT_DIR)/libc.so $(SYSROOT_LIB)/
	cp -f $(LIBC_OUT_DIR)/libm.a $(LIBC_OUT_DIR)/libm.so $(SYSROOT_LIB)/
	cp -f $(LIBC_OUT_DIR)/libutil.a $(LIBC_OUT_DIR)/libutil.so $(SYSROOT_LIB)/
	cp -f $(LIBC_OUT_DIR)/libc.o $(SYSROOT_LIB)/
	cp -f $(LIBC_OUT_DIR)/crt0.o $(LIBC_OUT_DIR)/crt1.o $(SYSROOT_LIB)/
	cp -f $(LIBC_OUT_DIR)/libzedbsd-compiler-rt.a $(LIBC_OUT_DIR)/libzedbsd-compiler-rt.o $(SYSROOT_LIB)/
	cp -f $(LIBC_OUT_DIR)/libclang_rt.builtins.a $(SYSROOT_LIB)/
	cp -f $(LIBC_OUT_DIR)/ld.so $(SYSROOT_LIB)/
	cp -f $(LIBC_OUT_DIR)/libc.so $(LIBC_OUT_DIR)/libm.so $(LIBC_OUT_DIR)/libutil.so $(LIBC_OUT_DIR)/ld.so $(BUILD)/dynamic/
	cp -f mk/platform-pc9821/user.ld mk/platform-pc9821/vmunix.ld $(SYSROOT_LIB)/zedbsd/i386/
	cp -f mk/platform-pc9821/user.ld mk/platform-pc9821/vmunix.ld $(SYSROOT_LIB)/zedbsd/pc9821/
	@printf '%s\n' '#include <stdint.h>' 'int main(void) { return (int)sizeof(uintptr_t); }' > $(SYSROOT)/smoke.c
	$(CC) --target=i386-unknown-zedbsd --sysroot=$(SYSROOT) \
		-m32 -march=i386 -mno-red-zone -DHAL_ARCH_I386 -DKERN_USER_ABI_ILP32 \
		-nostdinc -isystem $(SYSROOT_INC) \
		-ffreestanding -fno-pic -fno-pie -fno-stack-protector \
		-c $(SYSROOT)/smoke.c -o $(SYSROOT)/smoke.o
	$(CC) --target=i386-unknown-zedbsd --sysroot=$(SYSROOT) \
		-m32 -nostdlib -static -Wl,--build-id=none \
		-Wl,-T,$(SYSROOT_LIB)/zedbsd/i386/user.ld \
		$(SYSROOT_LIB)/crt0.o $(SYSROOT)/smoke.o \
		-Wl,--start-group $(SYSROOT_LIB)/libc.a \
		$(SYSROOT_LIB)/libzedbsd-compiler-rt.a \
		$(SYSROOT_LIB)/libclang_rt.builtins.a -Wl,--end-group \
		-o $(SYSROOT)/smoke.elf
	@$(READELF) -h $(SYSROOT)/smoke.elf | grep -E 'Intel (80)?386' >/dev/null
	@if $(NM) -u $(SYSROOT)/smoke.elf | grep .; then \
		echo "sysroot: smoke link retained undefined symbols" >&2; exit 1; \
	fi
	@touch $@
