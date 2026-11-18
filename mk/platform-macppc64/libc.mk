# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# PowerPC Mac (64-bit) libc rules
#
# The C library has no 64-bit PowerPC system call layer, start files or
# loader, so only the compiler's helper routines, which the kernel links,
# are built here.
#

PLATFORM_MACPPC64_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT           ?= $(abspath $(PLATFORM_MACPPC64_DIR)/../..)

LIBC_OUT_DIR := $(BUILD)/libc
LIBC_OBJ_DIR := $(BUILD)/libc/obj

# Static flags
MACPPC64_LIBC_STATIC_CFLAGS := \
	--target=powerpc64-unknown-zedbsd \
	-mcpu=970 \
	-mno-altivec \
	-DHAL_ARCH_PPC64 \
	-DKERN_USER_ABI_PPC64 \
	-nostdinc \
	-Iinclude/libc \
	-Iinclude \
	-Isrc -I. \
	-ffreestanding \
	-fno-builtin \
	-fno-pic \
	-fno-pie \
	-fno-stack-protector \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-fno-common \
	-fno-strict-aliasing \
	-ffunction-sections \
	-fdata-sections \
	-Os \
	-Wall \
	-Wextra \
	-Werror

LLVM_BUILTIN_NAMES := \
	absvdi2.c absvsi2.c absvti2.c \
	addvdi3.c addvsi3.c addvti3.c \
	ashlti3.c ashrti3.c \
	clzti2.c cmpti2.c ctzti2.c \
	divmodti4.c divti3.c ffsti2.c int_util.c lshrti3.c \
	modti3.c muloti4.c multi3.c mulvti3.c \
	negti2.c negvti2.c parityti2.c popcountti2.c \
	subvti3.c ucmpti2.c udivmodti4.c udivti3.c umodti3.c

LLVM_BUILTIN_DIR := $(REPO_ROOT)/build/llvm-source/compiler-rt/lib/builtins

#
# LLVM builtins
#
BUILTIN_OBJS := $(patsubst %.c,$(LIBC_OBJ_DIR)/builtins/%.o,$(LLVM_BUILTIN_NAMES))
$(BUILTIN_OBJS): $(LIBC_OBJ_DIR)/builtins/%.o: $(LLVM_BUILTIN_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MACPPC64_LIBC_STATIC_CFLAGS) -Wno-unused-parameter -I$(LLVM_BUILTIN_DIR) -c $< -o $@

$(LIBC_OUT_DIR)/libclang_rt.builtins.a: $(BUILTIN_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcsD $@ $^

LIBC_ALL_TARGETS := \
	$(LIBC_OUT_DIR)/libclang_rt.builtins.a

.PHONY: libc-platform
libc-platform: $(LIBC_ALL_TARGETS)
