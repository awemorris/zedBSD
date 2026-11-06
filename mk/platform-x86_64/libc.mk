#
# zedBSD x86_64 libc Makefile
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#

PLATFORM_X86_64_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT           ?= $(abspath $(PLATFORM_X86_64_DIR)/../..)

include $(REPO_ROOT)/mk/sources-libc.mk
include $(REPO_ROOT)/build-jobs.mk

LIBC_OUT_DIR := $(BUILD)/libc
LIBC_OBJ_DIR := $(BUILD)/packages/libc/obj

# Static flags
X86_64_LIBC_STATIC_CFLAGS := \
	--target=x86_64-unknown-zedbsd \
	-m64 \
	-march=x86-64 \
	-mno-red-zone \
	-DHAL_ARCH_X86_64 \
	-DKERN_USER_ABI_LP64 \
	-DZEDBSD_STATIC_TLS \
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

# Dynamic flags (shared library)
X86_64_LIBC_DYNAMIC_CFLAGS := \
	--target=x86_64-unknown-zedbsd \
	-m64 -march=x86-64 -mno-red-zone \
	-DHAL_ARCH_X86_64 -DKERN_USER_ABI_LP64 -DKERN_DYNAMIC_LIBC \
	-nostdinc -Iinclude/libc -Iinclude -Isrc -I. \
	-ffreestanding -fPIC -fno-builtin \
	-fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -ftls-model=global-dynamic \
	-ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror

COMPILER_RT_SOURCES := \
	src/libc/softfloat.c \
	src/libc/compiler-runtime.c \
	$(LIBM_SOURCES) \
	src/libc/float-parse.c

LLVM_BUILTIN_NAMES := \
	absvdi2.c absvsi2.c absvti2.c \
	addvdi3.c addvsi3.c addvti3.c \
	ashldi3.c ashlti3.c ashrdi3.c ashrti3.c \
	bswapdi2.c bswapsi2.c clzdi2.c clzsi2.c clzti2.c \
	cmpdi2.c cmpti2.c ctzdi2.c ctzsi2.c ctzti2.c \
	divdi3.c divmoddi4.c divmodsi4.c divmodti4.c divsi3.c divti3.c \
	ffsdi2.c ffssi2.c ffsti2.c int_util.c lshrdi3.c lshrti3.c \
	moddi3.c modsi3.c modti3.c \
	muldi3.c mulodi4.c mulosi4.c muloti4.c multi3.c \
	mulvdi3.c mulvsi3.c mulvti3.c \
	negdi2.c negti2.c negvdi2.c negvsi2.c negvti2.c \
	paritydi2.c paritysi2.c parityti2.c \
	popcountdi2.c popcountsi2.c popcountti2.c \
	subvdi3.c subvsi3.c subvti3.c ucmpdi2.c ucmpti2.c \
	udivdi3.c udivmoddi4.c udivmodsi4.c udivmodti4.c \
	udivsi3.c udivti3.c umoddi3.c umodsi3.c umodti3.c

LLVM_BUILTIN_DIR := $(REPO_ROOT)/build/llvm-source/compiler-rt/lib/builtins

CRT0_SRC := src/libc/crt/crt0-x86_64.S
CRT1_SRC := src/libc/crt/crt1-x86_64.S

#
# Static libc objects & library
#
LIBC_STATIC_OBJS := $(patsubst %.c,$(LIBC_OBJ_DIR)/static/%.o,$(LIBC_SOURCES))
$(LIBC_STATIC_OBJS): $(LIBC_OBJ_DIR)/static/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(X86_64_LIBC_STATIC_CFLAGS) -c $< -o $@

$(LIBC_OUT_DIR)/libc.a: $(LIBC_STATIC_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcsD $@ $^

$(LIBC_OUT_DIR)/libc.o: $(LIBC_OUT_DIR)/libc.a
	@mkdir -p $(dir $@)
	$(LD) -r --whole-archive $< --no-whole-archive -o $@

#
# Dynamic libc objects & shared library
#
LIBC_DYNAMIC_OBJS := $(patsubst %.c,$(LIBC_OBJ_DIR)/dynamic/%.o,$(LIBC_SOURCES)) $(LIBC_OBJ_DIR)/dynamic/syscall.o

$(LIBC_OBJ_DIR)/dynamic/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(X86_64_LIBC_DYNAMIC_CFLAGS) -c $< -o $@

$(LIBC_OBJ_DIR)/dynamic/syscall.o: src/libc/syscall-amd64.S
	@mkdir -p $(dir $@)
	$(CC) $(X86_64_LIBC_DYNAMIC_CFLAGS) -c $< -o $@

$(LIBC_OUT_DIR)/libc.so: $(LIBC_DYNAMIC_OBJS) src/libc/interpose.list
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 -shared -soname libc.so --hash-style=both \
		-Bsymbolic-functions --export-dynamic-symbol-list=src/libc/interpose.list \
		-z now -z relro -z separate-code -z stack-size=0x100000 \
		$(LIBC_DYNAMIC_OBJS) -o $@

#
# Static & dynamic libm
#
LIBM_STATIC_OBJS := $(patsubst %.c,$(LIBC_OBJ_DIR)/static/%.o,$(LIBM_SOURCES))
$(LIBM_STATIC_OBJS): $(LIBC_OBJ_DIR)/static/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(X86_64_LIBC_STATIC_CFLAGS) -c $< -o $@

$(LIBC_OUT_DIR)/libm.a: $(LIBM_STATIC_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcsD $@ $^

LIBM_DYNAMIC_OBJS := $(patsubst %.c,$(LIBC_OBJ_DIR)/dynamic/%.o,$(LIBM_SOURCES))
$(LIBM_DYNAMIC_OBJS): $(LIBC_OBJ_DIR)/dynamic/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(X86_64_LIBC_DYNAMIC_CFLAGS) -c $< -o $@

$(LIBC_OUT_DIR)/libm.so: $(LIBM_DYNAMIC_OBJS)
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 -shared -soname libm.so --hash-style=both \
		-z now -z relro -z separate-code \
		$(LIBM_DYNAMIC_OBJS) -o $@

#
# Static & dynamic libutil
#
LIBUTIL_STATIC_OBJS := $(LIBC_OBJ_DIR)/static/src/libc/libutil.o
$(LIBUTIL_STATIC_OBJS): src/libc/libutil.c
	@mkdir -p $(dir $@)
	$(CC) $(X86_64_LIBC_STATIC_CFLAGS) -c $< -o $@

$(LIBC_OUT_DIR)/libutil.a: $(LIBUTIL_STATIC_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcsD $@ $^

LIBUTIL_DYNAMIC_OBJS := $(LIBC_OBJ_DIR)/dynamic/src/libc/libutil.o
$(LIBUTIL_DYNAMIC_OBJS): src/libc/libutil.c
	@mkdir -p $(dir $@)
	$(CC) $(X86_64_LIBC_DYNAMIC_CFLAGS) -c $< -o $@

$(LIBC_OUT_DIR)/libutil.so: $(LIBUTIL_DYNAMIC_OBJS)
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 -shared -soname libutil.so --hash-style=both \
		-z now -z relro -z separate-code \
		$(LIBUTIL_DYNAMIC_OBJS) -o $@

#
# Compiler runtime & LLVM builtins
#
COMPILER_RT_OBJS := $(patsubst %.c,$(LIBC_OBJ_DIR)/compiler-rt/%.o,$(COMPILER_RT_SOURCES))
$(COMPILER_RT_OBJS): $(LIBC_OBJ_DIR)/compiler-rt/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(X86_64_LIBC_STATIC_CFLAGS) -mlong-double-64 -c $< -o $@

$(LIBC_OUT_DIR)/libzedbsd-compiler-rt.a: $(COMPILER_RT_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcsD $@ $^

$(LIBC_OUT_DIR)/libzedbsd-compiler-rt.o: $(LIBC_OUT_DIR)/libzedbsd-compiler-rt.a
	@mkdir -p $(dir $@)
	$(LD) -r --whole-archive $< --no-whole-archive -o $@

BUILTIN_OBJS := $(patsubst %.c,$(LIBC_OBJ_DIR)/builtins/%.o,$(LLVM_BUILTIN_NAMES))
$(BUILTIN_OBJS): $(LIBC_OBJ_DIR)/builtins/%.o: $(LLVM_BUILTIN_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(X86_64_LIBC_STATIC_CFLAGS) -Wno-unused-parameter -I$(LLVM_BUILTIN_DIR) -c $< -o $@

$(LIBC_OUT_DIR)/libclang_rt.builtins.a: $(BUILTIN_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcsD $@ $^

#
# CRT objects
#
$(LIBC_OUT_DIR)/crt0.o: $(CRT0_SRC)
	@mkdir -p $(dir $@)
	$(CC) --target=x86_64-unknown-zedbsd -m64 -nostdinc -Iinclude -DHAL_ARCH_X86_64 -DKERN_USER_ABI_LP64 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -c $< -o $@

$(LIBC_OUT_DIR)/crt1.o: $(CRT1_SRC)
	@mkdir -p $(dir $@)
	$(CC) --target=x86_64-unknown-zedbsd -m64 -nostdinc -Iinclude -DHAL_ARCH_X86_64 -DKERN_USER_ABI_LP64 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -c $< -o $@

LIBC_ALL_TARGETS := \
	$(LIBC_OUT_DIR)/libc.a \
	$(LIBC_OUT_DIR)/libc.so \
	$(LIBC_OUT_DIR)/libm.a \
	$(LIBC_OUT_DIR)/libm.so \
	$(LIBC_OUT_DIR)/libutil.a \
	$(LIBC_OUT_DIR)/libutil.so \
	$(LIBC_OUT_DIR)/libc.o \
	$(LIBC_OUT_DIR)/crt0.o \
	$(LIBC_OUT_DIR)/crt1.o \
	$(LIBC_OUT_DIR)/libzedbsd-compiler-rt.a \
	$(LIBC_OUT_DIR)/libzedbsd-compiler-rt.o \
	$(LIBC_OUT_DIR)/libclang_rt.builtins.a

.PHONY: libc-x86_64
libc-x86_64: $(LIBC_ALL_TARGETS)
