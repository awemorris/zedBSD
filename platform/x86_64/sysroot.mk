# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# x86_64 libc and sysroot build rules
#

PLATFORM_X86_64_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT           ?= $(abspath $(PLATFORM_X86_64_DIR)/../..)

include $(REPO_ROOT)/src/libc/libc.mk
include $(REPO_ROOT)/build-jobs.mk

SYSROOT     := $(REPO_ROOT)/build/x86_64/sysroot
SYSROOT_INC := $(SYSROOT)/usr/include
SYSROOT_LIB := $(SYSROOT)/usr/lib
SYSROOT_OBJ := $(REPO_ROOT)/build/x86_64/packages/libc/obj

LIBC_CFLAGS ?= $(filter-out -mgeneral-regs-only,$(CFLAGS))

X86_64_LIBC_CFLAGS := \
	--target=x86_64-unknown-zedbsd \
	--sysroot=$(SYSROOT) \
	-m64 -march=x86-64 -mno-red-zone \
	-DHAL_ARCH_X86_64 -DKERN_USER_ABI_LP64 -DZEDBSD_STATIC_TLS \
	-nostdinc -Iinclude/libc -Iinclude -Isrc -I. \
	-ffreestanding -fno-builtin -fno-pic -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -fno-common -fno-strict-aliasing \
	-ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror

COMPILER_RT_SOURCES := \
	src/libc/softfloat.c \
	src/libc/compiler-runtime.c \
	$(ZEDBSD_LIBM_SOURCES) \
	src/libc/float-parse.c

LLVM_BUILTIN_NAMES := \
	absvdi2.c \
	absvsi2.c \
	absvti2.c \
	addvdi3.c \
	addvsi3.c \
	addvti3.c \
	ashldi3.c \
	ashlti3.c \
	ashrdi3.c \
	ashrti3.c \
	bswapdi2.c \
	bswapsi2.c \
	clzdi2.c \
	clzsi2.c \
	clzti2.c \
	cmpdi2.c \
	cmpti2.c \
	ctzdi2.c \
	ctzsi2.c \
	ctzti2.c \
	divdi3.c \
	divmoddi4.c \
	divmodsi4.c \
	divmodti4.c \
	divsi3.c \
	divti3.c \
	ffsdi2.c \
	ffssi2.c \
	ffsti2.c \
	int_util.c \
	lshrdi3.c \
	lshrti3.c \
	moddi3.c \
	modsi3.c \
	modti3.c \
	muldi3.c \
	mulodi4.c \
	mulosi4.c \
	muloti4.c \
	multi3.c \
	mulvdi3.c \
	mulvsi3.c \
	mulvti3.c \
	negdi2.c \
	negti2.c \
	negvdi2.c \
	negvsi2.c \
	negvti2.c \
	paritydi2.c \
	paritysi2.c \
	parityti2.c \
	popcountdi2.c \
	popcountsi2.c \
	popcountti2.c \
	subvdi3.c \
	subvsi3.c \
	subvti3.c \
	ucmpdi2.c \
	ucmpti2.c \
	udivdi3.c \
	udivmoddi4.c \
	udivmodsi4.c \
	udivmodti4.c \
	udivsi3.c \
	udivti3.c \
	umoddi3.c \
	umodsi3.c \
	umodti3.c

LLVM_BUILTIN_DIR := $(REPO_ROOT)/build/llvm-source/compiler-rt/lib/builtins
LLVM_BUILTIN_SOURCES := $(addprefix $(LLVM_BUILTIN_DIR)/,$(LLVM_BUILTIN_NAMES))

CRT0_SRC := src/libc/crt/crt0-x86_64.S
CRT1_SRC := src/libc/crt/crt1-x86_64.S

PUBLIC_HEADERS := $(shell find include/libc include/uapi userland/desktop/include -type f ! -name '*~' 2>/dev/null | LC_ALL=C sort)

SYSROOT_COMPLETE_STAMP := $(SYSROOT)/.zedbsd-sysroot-complete
HEADERS_INSTALLED_STAMP := $(SYSROOT)/.headers-installed

.PHONY: libc sysroot

sysroot: $(SYSROOT_COMPLETE_STAMP)

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

# Libc objects
LIBC_OBJS := $(patsubst %.c,$(SYSROOT_OBJ)/libc/%.o,$(LIBC_SOURCES))
$(LIBC_OBJS): $(SYSROOT_OBJ)/libc/%.o: %.c $(HEADERS_INSTALLED_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(X86_64_LIBC_CFLAGS) -c $< -o $@

$(SYSROOT_LIB)/libc.a: $(LIBC_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcsD $@ $^

$(SYSROOT_LIB)/libc.o: $(SYSROOT_LIB)/libc.a
	$(LD) -r --whole-archive $< --no-whole-archive -o $@

# Compiler runtime objects
COMPILER_RT_OBJS := $(patsubst %.c,$(SYSROOT_OBJ)/compiler-rt/%.o,$(COMPILER_RT_SOURCES))
$(COMPILER_RT_OBJS): $(SYSROOT_OBJ)/compiler-rt/%.o: %.c $(HEADERS_INSTALLED_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(X86_64_LIBC_CFLAGS) -mlong-double-64 -c $< -o $@

$(SYSROOT_LIB)/libzedbsd-compiler-rt.a: $(COMPILER_RT_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcsD $@ $^

$(SYSROOT_LIB)/libzedbsd-compiler-rt.o: $(SYSROOT_LIB)/libzedbsd-compiler-rt.a
	$(LD) -r --whole-archive $< --no-whole-archive -o $@

# LLVM builtins
BUILTIN_OBJS := $(patsubst %.c,$(SYSROOT_OBJ)/builtins/%.o,$(LLVM_BUILTIN_NAMES))
$(BUILTIN_OBJS): $(SYSROOT_OBJ)/builtins/%.o: $(LLVM_BUILTIN_DIR)/%.c $(HEADERS_INSTALLED_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(X86_64_LIBC_CFLAGS) -Wno-unused-parameter -I$(LLVM_BUILTIN_DIR) -c $< -o $@

$(SYSROOT_LIB)/libclang_rt.builtins.a: $(BUILTIN_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcsD $@ $^

# CRT objects
$(SYSROOT_LIB)/crt0.o: $(CRT0_SRC)
	@mkdir -p $(dir $@)
	$(CC) --target=x86_64-unknown-zedbsd -m64 -nostdinc -Iinclude -DHAL_ARCH_X86_64 -DKERN_USER_ABI_LP64 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -c $< -o $@

$(SYSROOT_LIB)/crt1.o: $(CRT1_SRC)
	@mkdir -p $(dir $@)
	$(CC) --target=x86_64-unknown-zedbsd -m64 -nostdinc -Iinclude -DHAL_ARCH_X86_64 -DKERN_USER_ABI_LP64 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -c $< -o $@

# Dummy libm.a
$(SYSROOT_LIB)/libm.a:
	@mkdir -p $(dir $@)
	$(AR) rcsD $@

# Complete sysroot with linker scripts and smoke link verification
$(SYSROOT_COMPLETE_STAMP): \
		$(HEADERS_INSTALLED_STAMP) \
		$(SYSROOT_LIB)/libc.a \
		$(SYSROOT_LIB)/libc.o \
		$(SYSROOT_LIB)/libzedbsd-compiler-rt.a \
		$(SYSROOT_LIB)/libzedbsd-compiler-rt.o \
		$(SYSROOT_LIB)/libclang_rt.builtins.a \
		$(SYSROOT_LIB)/crt0.o \
		$(SYSROOT_LIB)/crt1.o \
		$(SYSROOT_LIB)/libm.a
	@mkdir -p $(SYSROOT_LIB)/zedbsd/x86_64
	cp -f platform/x86_64/user.ld platform/x86_64/vmunix.ld $(SYSROOT_LIB)/zedbsd/x86_64/
	@printf '%s\n' '#include <stdint.h>' 'int main(void) { return (int)sizeof(uintptr_t); }' > $(SYSROOT)/smoke.c
	$(CC) --target=x86_64-unknown-zedbsd --sysroot=$(SYSROOT) \
		-m64 -march=x86-64 -mno-red-zone -DHAL_ARCH_X86_64 -DKERN_USER_ABI_LP64 \
		-nostdinc -isystem $(SYSROOT_INC) \
		-ffreestanding -fno-pic -fno-pie -fno-stack-protector \
		-c $(SYSROOT)/smoke.c -o $(SYSROOT)/smoke.o
	$(CC) --target=x86_64-unknown-zedbsd --sysroot=$(SYSROOT) \
		-m64 -nostdlib -static -Wl,--build-id=none \
		-Wl,-T,$(SYSROOT_LIB)/zedbsd/x86_64/user.ld \
		$(SYSROOT_LIB)/crt0.o $(SYSROOT)/smoke.o \
		-Wl,--start-group $(SYSROOT_LIB)/libc.a \
		$(SYSROOT_LIB)/libzedbsd-compiler-rt.a \
		$(SYSROOT_LIB)/libclang_rt.builtins.a -Wl,--end-group \
		-o $(SYSROOT)/smoke.elf
	@$(READELF) -h $(SYSROOT)/smoke.elf | grep -F 'Advanced Micro Devices X86-64' >/dev/null
	@if $(NM) -u $(SYSROOT)/smoke.elf | grep .; then \
		echo "sysroot: smoke link retained undefined symbols" >&2; exit 1; \
	fi
	@touch $@
