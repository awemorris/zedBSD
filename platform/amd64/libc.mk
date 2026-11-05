
AMD64_USER_LIBC_OBJS := $(BUILD)/user64/src/libc/crt/crt0-amd64.o \
	$(patsubst %.c,$(BUILD)/user64/%.o,$(AMD64_USER_RUNTIME_SOURCES))
# User programs consume the canonical target sysroot. The relocatable libc
# bundle preserves the established whole-runtime static link semantics while
# eliminating per-command recompilation of the same sources.
AMD64_USER_LIBC_OBJS := \
	$(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt0.o \
	$(ZEDBSD_SYSROOT_AMD64)/usr/lib/libc.o
AMD64_USER_NET_LIBC_OBJS := $(AMD64_USER_LIBC_OBJS)
AMD64_USER_SH_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user64,sh)
AMD64_USER_READLINE_OBJ := $(BUILD)/user64/userland/base/libedit/readline.o
AMD64_USER_READLINE_LIB := $(BUILD)/lib/libreadline.a
AMD64_USER_ELF_CHECK := tools/build/check-user-elf.noct
