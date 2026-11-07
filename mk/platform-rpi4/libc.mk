#
# zedBSD rpi4 (ARM64) libc Makefile
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#

PLATFORM_RPI4_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT         ?= $(abspath $(PLATFORM_RPI4_DIR)/../..)

include $(REPO_ROOT)/mk/libc.mk
include $(REPO_ROOT)/build-jobs.mk

LIBC_OUT_DIR := $(BUILD)/libc
LIBC_OBJ_DIR := $(BUILD)/libc/obj

# Static flags
ARM64_LIBC_STATIC_CFLAGS := \
	--target=aarch64-unknown-zedbsd \
	-march=armv8-a \
	-mno-outline-atomics \
	-DHAL_ARCH_ARM64 \
	-DKERN_USER_ABI_AARCH64 \
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
ARM64_LIBC_DYNAMIC_CFLAGS := \
	--target=aarch64-unknown-zedbsd \
	-march=armv8-a \
	-mno-outline-atomics \
	-DHAL_ARCH_ARM64 \
	-DKERN_USER_ABI_AARCH64 \
	-DKERN_USER_ABI_LP64 \
	-DKERN_DYNAMIC_LIBC \
	-nostdinc \
	-Iinclude/libc \
	-Iinclude \
	-Isrc -I. \
	-ffreestanding \
	-fPIC \
	-fno-builtin \
	-fno-stack-protector \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-ftls-model=global-dynamic \
	-ffunction-sections \
	-fdata-sections \
	-Os \
	-Wall \
	-Wextra \
	-Werror

COMPILER_RT_SOURCES := \
	src/libc/softfloat.c \
	src/libc/compiler-runtime.c \
	src/libc/softfloat128.c \
	src/libc/compiler-runtime128.c \
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
	udivsi3.c udivti3.c umoddi3.c umodsi3.c umodti3.c \
	clear_cache.c

LLVM_BUILTIN_DIR := $(REPO_ROOT)/build/llvm-source/compiler-rt/lib/builtins

CRT0_SRC := src/libc/crt/crt0-aarch64.S
CRT1_SRC := src/libc/crt/crt1-aarch64.S

#
# Static libc objects & library
#
LIBC_STATIC_C_OBJS := $(patsubst %.c,$(LIBC_OBJ_DIR)/static/%.o,$(LIBC_SOURCES))
$(LIBC_STATIC_C_OBJS): $(LIBC_OBJ_DIR)/static/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ARM64_LIBC_STATIC_CFLAGS) -c $< -o $@

$(LIBC_OBJ_DIR)/static/src/libc/setjmp-aarch64.o: src/libc/setjmp-aarch64.S
	@mkdir -p $(dir $@)
	$(CC) $(ARM64_LIBC_STATIC_CFLAGS) -D_ASM_SRC_ -c $< -o $@

LIBC_STATIC_OBJS := $(LIBC_STATIC_C_OBJS) $(LIBC_OBJ_DIR)/static/src/libc/setjmp-aarch64.o

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
LIBC_DYNAMIC_C_OBJS := $(patsubst %.c,$(LIBC_OBJ_DIR)/dynamic/%.o,$(LIBC_SOURCES))
$(LIBC_DYNAMIC_C_OBJS): $(LIBC_OBJ_DIR)/dynamic/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ARM64_LIBC_DYNAMIC_CFLAGS) -c $< -o $@

$(LIBC_OBJ_DIR)/dynamic/src/libc/syscall.o: src/libc/syscall-aarch64.S
	@mkdir -p $(dir $@)
	$(CC) $(ARM64_LIBC_DYNAMIC_CFLAGS) -c $< -o $@

$(LIBC_OBJ_DIR)/dynamic/src/libc/setjmp-aarch64.o: src/libc/setjmp-aarch64.S
	@mkdir -p $(dir $@)
	$(CC) $(ARM64_LIBC_DYNAMIC_CFLAGS) -D_ASM_SRC_ -c $< -o $@

LIBC_DYNAMIC_OBJS := $(LIBC_DYNAMIC_C_OBJS) \
	$(LIBC_OBJ_DIR)/dynamic/src/libc/syscall.o \
	$(LIBC_OBJ_DIR)/dynamic/src/libc/setjmp-aarch64.o

$(LIBC_OUT_DIR)/libc.so: $(LIBC_DYNAMIC_OBJS) src/libc/interpose.list
	@mkdir -p $(dir $@)
	$(LD) -m aarch64elf -shared -soname libc.so --hash-style=both \
		-Bsymbolic-functions --export-dynamic-symbol-list=src/libc/interpose.list \
		-z now -z relro -z separate-code -z max-page-size=4096 -z stack-size=0x100000 \
		$(LIBC_DYNAMIC_OBJS) -o $@

#
# Static & dynamic libm
#
LIBM_STATIC_OBJS := $(patsubst %.c,$(LIBC_OBJ_DIR)/static/%.o,$(LIBM_SOURCES))
$(LIBM_STATIC_OBJS): $(LIBC_OBJ_DIR)/static/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ARM64_LIBC_STATIC_CFLAGS) -c $< -o $@

$(LIBC_OUT_DIR)/libm.a: $(LIBM_STATIC_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcsD $@ $^

LIBM_DYNAMIC_OBJS := $(patsubst %.c,$(LIBC_OBJ_DIR)/dynamic/%.o,$(LIBM_SOURCES))
$(LIBM_DYNAMIC_OBJS): $(LIBC_OBJ_DIR)/dynamic/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ARM64_LIBC_DYNAMIC_CFLAGS) -c $< -o $@

$(LIBC_OUT_DIR)/libm.so: $(LIBM_DYNAMIC_OBJS)
	@mkdir -p $(dir $@)
	$(LD) -m aarch64elf -shared -soname libm.so --hash-style=both \
		-z now -z relro -z separate-code -z max-page-size=4096 \
		$(LIBM_DYNAMIC_OBJS) -o $@

#
# Static & dynamic libutil
#
LIBUTIL_STATIC_OBJS := $(LIBC_OBJ_DIR)/static/src/libc/libutil.o
$(LIBUTIL_STATIC_OBJS): src/libc/libutil.c
	@mkdir -p $(dir $@)
	$(CC) $(ARM64_LIBC_STATIC_CFLAGS) -c $< -o $@

$(LIBC_OUT_DIR)/libutil.a: $(LIBUTIL_STATIC_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcsD $@ $^

LIBUTIL_DYNAMIC_OBJS := $(LIBC_OBJ_DIR)/dynamic/src/libc/libutil.o
$(LIBUTIL_DYNAMIC_OBJS): src/libc/libutil.c
	@mkdir -p $(dir $@)
	$(CC) $(ARM64_LIBC_DYNAMIC_CFLAGS) -c $< -o $@

$(LIBC_OUT_DIR)/libutil.so: $(LIBUTIL_DYNAMIC_OBJS)
	@mkdir -p $(dir $@)
	$(LD) -m aarch64elf -shared -soname libutil.so --hash-style=both \
		-z now -z relro -z separate-code -z max-page-size=4096 \
		$(LIBUTIL_DYNAMIC_OBJS) -o $@

#
# Compiler runtime & LLVM builtins
#
COMPILER_RT_OBJS := $(patsubst %.c,$(LIBC_OBJ_DIR)/compiler-rt/%.o,$(COMPILER_RT_SOURCES))
$(COMPILER_RT_OBJS): $(LIBC_OBJ_DIR)/compiler-rt/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ARM64_LIBC_STATIC_CFLAGS) -c $< -o $@

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
	$(CC) $(ARM64_LIBC_STATIC_CFLAGS) -Wno-unused-parameter -I$(LLVM_BUILTIN_DIR) -c $< -o $@

$(LIBC_OUT_DIR)/libclang_rt.builtins.a: $(BUILTIN_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcsD $@ $^

#
# CRT objects
#
$(LIBC_OUT_DIR)/crt0.o: $(CRT0_SRC)
	@mkdir -p $(dir $@)
	$(CC) --target=aarch64-unknown-zedbsd -march=armv8-a -mno-outline-atomics -nostdinc -Iinclude -DHAL_ARCH_ARM64 -DKERN_USER_ABI_AARCH64 -DKERN_USER_ABI_LP64 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -c $< -o $@

$(LIBC_OUT_DIR)/crt1.o: $(CRT1_SRC)
	@mkdir -p $(dir $@)
	$(CC) --target=aarch64-unknown-zedbsd -march=armv8-a -mno-outline-atomics -nostdinc -Iinclude -DHAL_ARCH_ARM64 -DKERN_USER_ABI_AARCH64 -DKERN_USER_ABI_LP64 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -c $< -o $@

#
# Dynamic linker (ld.so)
#
$(LIBC_OBJ_DIR)/dynamic/src/rtld/entry.o: src/rtld/entry-aarch64.S
	@mkdir -p $(dir $@)
	$(CC) --target=aarch64-unknown-zedbsd -march=armv8-a -mno-outline-atomics -c $< -o $@

$(LIBC_OBJ_DIR)/dynamic/src/rtld/tlsdesc.o: src/rtld/tlsdesc-arm64.S
	@mkdir -p $(dir $@)
	$(CC) --target=aarch64-unknown-zedbsd -march=armv8-a -mno-outline-atomics -c $< -o $@

$(LIBC_OBJ_DIR)/dynamic/src/rtld/rtld.o: src/rtld/rtld.c
	@mkdir -p $(dir $@)
	$(CC) $(ARM64_LIBC_DYNAMIC_CFLAGS) -DKERN_DYNAMIC_LIBC -c $< -o $@

$(LIBC_OBJ_DIR)/dynamic/src/rtld/string.o: src/rtld/string.c
	@mkdir -p $(dir $@)
	$(CC) $(ARM64_LIBC_DYNAMIC_CFLAGS) -DKERN_DYNAMIC_LIBC -c $< -o $@

$(LIBC_OUT_DIR)/ld.so: $(LIBC_OBJ_DIR)/dynamic/src/rtld/entry.o \
	$(LIBC_OBJ_DIR)/dynamic/src/rtld/tlsdesc.o \
	$(LIBC_OBJ_DIR)/dynamic/src/rtld/rtld.o \
	$(LIBC_OBJ_DIR)/dynamic/src/rtld/string.o
	@mkdir -p $(dir $@)
	$(LD) -m aarch64elf -shared -Bsymbolic -e _rtld_start \
		--hash-style=sysv -z now -z relro -z separate-code -z max-page-size=4096 $^ -o $@

LIBC_ALL_TARGETS := \
	$(LIBC_OUT_DIR)/libc.a \
	$(LIBC_OUT_DIR)/libc.so \
	$(LIBC_OUT_DIR)/libm.a \
	$(LIBC_OUT_DIR)/libm.so \
	$(LIBC_OUT_DIR)/libutil.a \
	$(LIBC_OUT_DIR)/libutil.so \
	$(LIBC_OUT_DIR)/ld.so \
	$(LIBC_OUT_DIR)/libc.o \
	$(LIBC_OUT_DIR)/crt0.o \
	$(LIBC_OUT_DIR)/crt1.o \
	$(LIBC_OUT_DIR)/libzedbsd-compiler-rt.a \
	$(LIBC_OUT_DIR)/libzedbsd-compiler-rt.o \
	$(LIBC_OUT_DIR)/libclang_rt.builtins.a

.PHONY: libc-platform
libc-platform: $(LIBC_ALL_TARGETS)
