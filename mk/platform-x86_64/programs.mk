# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

USER_CPPFLAGS := \
	$(CPPFLAGS) \
	-Iinclude \
	-Isrc \
	-I. \
	-D__ZEDBSD__ \
	-DHAL_ARCH_X86_64 \
	-DKERN_USER_ABI_LP64

USER_CFLAGS := \
	$(CFLAGS)\
	-m64 \
	-march=x86-64 \
	-fno-pic \
	-fno-pie \
	-fno-stack-protector \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-fno-builtin \
	-fno-common \
	-ffunction-sections \
	-fdata-sections \
	-Os \
	-Wall \
	-Wextra \
	-Werror

# Base userland programs
$(BUILD)/bin/sh: userland/base/sh/*.c userland/base/libedit/readline.c $(SYSROOT_COMPLETE_STAMP)
	@mkdir -p $(dir $@)
	$(CC) --target=x86_64-unknown-zedbsd --sysroot=$(SYSROOT) \
		-m64 -march=x86-64 -DHAL_ARCH_X86_64 -DKERN_USER_ABI_LP64 \
		-nostdinc -isystem $(SYSROOT_INC) \
		-Iinclude -Isrc -I. -Iuserland/base/libedit \
		-ffreestanding -fno-stack-protector -nostdlib -static \
		userland/base/sh/*.c userland/base/libedit/readline.c \
		-Wl,-T,$(SYSROOT_LIB)/zedbsd/x86_64/user.ld \
		$(SYSROOT_LIB)/crt0.o \
		-Wl,--start-group $(SYSROOT_LIB)/libc.a \
		$(SYSROOT_LIB)/libzedbsd-compiler-rt.a \
		$(SYSROOT_LIB)/libclang_rt.builtins.a -Wl,--end-group \
		-o $@
	@test -z "$$($(NM) -u $@)" || { $(NM) -u $@; exit 1; }

programs: $(BUILD)/bin/sh
