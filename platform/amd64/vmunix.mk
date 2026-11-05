#
# zedBSD amd64/PC-AT bootstrap rules.
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
#

ZEDBSD_CONFIG        := platform/amd64/zedbsd.cfg
NATIVE_ZEDBSD_CONFIG := platform/amd64/zedbsd-native.cfg
IMAGE_CONTRACT_STAMP := $(BUILD)/.disk-image-contract
IMAGE_STAGE1         := $(if $(filter bios,$(ZEDBSD_VARIANT)), $(BUILD)/bootloader/stage1-native.bin,$(BUILD)/bootloader/stage1.bin)
CPPFLAGS             := -nostdlibinc -Iinclude -Isrc -I. -DHAL_ARCH_AMD64 \
                        -DHAL_BOARD_PCAT -DHAL_PCAT_DEBUGCON \
                        -DKERN_USER_ABI_LP64 \
                        -DPCAT_VGA_APERTURE_ADDRESS=0xffffffffc1400000ULL \
                        -DPCAT_CIRRUS_APERTURE_ADDRESS=0xffffffffc0000000ULL \
                        $(ZEDBSD_CONFIG_CPPFLAGS)
CFLAGS              := -m64 -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
	               -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
                       -fno-asynchronous-unwind-tables -fno-unwind-tables \
                       -ffunction-sections -fdata-sections -Os -Wall -Wextra \
                       -Werror -Wframe-larger-than=8192 -fno-builtin
LIBC_CFLAGS         := $(filter-out -mgeneral-regs-only,$(CFLAGS))
KERNEL_LTO_CFLAGS   := $(ZEDBSD_KERNEL_LTO_CFLAGS)

$(BUILD)/vmunix: $(VMUNIX_OBJS) $(ZEDBSD_PLATFORM_DIR)/vmunix.ld
	$(LD) \
		-m elf_x86_64 \
		--gc-sections \
		-z max-page-size=4096 \
		-T $(ZEDBSD_PLATFORM_DIR)/vmunix.ld \
		-nostdlib \
		-Map $@.map \
		$(AMD64_VMUNIX_OBJS) \
		-o $@

define AMD64_VALIDATE_GPT_IMAGE
	$(NOCT) --path=tools/build platform/amd64/tools/check-amd64-gpt-image.noct \
		--layout $(ZEDBSD_VARIANT) \
		--machine pcat --stage1 $(IMAGE_STAGE1) \
		--stage2 $(BUILD)/bootloader/stage2-chain.bin \
		--partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
		--bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE --kernel $(BUILD)/vmunix \
		--bootx64 $(BUILD)/uefi/BOOTX64.EFI \
		--zedbsd-config $(ZEDBSD_CONFIG) \
		--arch-profile amd64 --arch-image $(AMD64_ARCH_UFS_IMAGE) \
		--arch-format ufs --data-image $(DATA_IMAGE) \
		--swapfile $(SWAP_IMAGE) $(1)
endef


AMD64_HAL_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(AMD64_HAL_SOURCES)) \
	$(patsubst %.S,$(BUILD)/%.o,$(AMD64_HAL_ASM))

AMD64_VMUNIX_OBJS := $(AMD64_HAL_OBJS) $(AMD64_KERNEL_OBJS)
ifneq ($(strip $(ZEDBSD_CONFIG)),)
$(AMD64_VMUNIX_OBJS): $(ZEDBSD_CONFIG)
$(AMD64_VMUNIX_OBJS): platform/amd64/vmunix.mk
endif
$(AMD64_VMUNIX_OBJS): $(ZEDBSD_PLATFORM_CONFIG_STAMP)
$(AMD64_VMUNIX_OBJS): $(ZEDBSD_KERNEL_LTO_STAMP)



# The i915 test build's scenario set (see I915_TEST_SET), rewritten only when it changes.
.PHONY: FORCE_AMD64_I915_TEST_SET
FORCE_AMD64_I915_TEST_SET:

$(BUILD)/.i915-test-set: FORCE_AMD64_I915_TEST_SET
	@mkdir -p $(dir $@)
	@value='I915_TEST_SET=$(I915_TEST_SET)'; \
 if ! test -f $@ || ! grep -Fqx -- "$$value" $@; then \
 printf '%s\n' "$$value" > $@.tmp; \
 mv $@.tmp $@; \
 fi


AMD64_USER_CPPFLAGS := -nostdinc \
	-isystem $(ZEDBSD_SYSROOT_AMD64)/usr/include \
	-Iinclude -Isrc -I. -DHAL_ARCH_AMD64 -DKERN_USER_ABI_LP64
AMD64_USER_CFLAGS := -m64 -march=x86-64 -mno-red-zone -ffreestanding \
	-fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -fno-builtin -fno-common -ffunction-sections \
	-fdata-sections -Os -Wall -Wextra -Werror

# The base programs are position-independent executables that load
# /lib/libc.so through /lib/ld.so; their objects are the -fPIC ones built
# under $(BUILD)/dynamic/obj.  (The POSIX-R and other test ELF files below
# stay static: they test the static runtime itself.)

AMD64_APP_OBJ := $(BUILD)/dynamic/obj

AMD64_APP_INPUTS := $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(BUILD)/dynamic/libc.so $(BUILD)/dynamic/ld.so \
	tools/build/check-dynamic-elf.py

AMD64_APP_LINK = $(CC) -m64 -nostdlib -pie -Wl,--no-relax -Wl,--gc-sections \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o

AMD64_APP_LIBS = -L$(BUILD)/dynamic -Wl,-rpath-link,$(BUILD)/dynamic -l:libc.so

AMD64_APP_CHECK = $(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 \
 --role application --needed libc.so

AMD64_APP_SH_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(AMD64_APP_OBJ),sh) \
	$(AMD64_APP_OBJ)/userland/base/libedit/readline.o

$(AMD64_APP_SH_OBJS): DYNAMIC_CPPFLAGS += -Iuserland/base/libedit

$(BUILD)/user64/%.o: %.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) \
 -fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/user64/src/libc/crt/crt0-amd64.o: src/libc/crt/crt0-amd64.S \
	include/hal/arch.h include/hal/arch/amd64.h
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) -c $< -o $@

$(AMD64_USER_READLINE_OBJ): AMD64_USER_CPPFLAGS += -Iuserland/base/libedit
$(AMD64_USER_SH_OBJS): AMD64_USER_CPPFLAGS += -Iuserland/base/libedit
$(AMD64_USER_READLINE_LIB): $(AMD64_USER_READLINE_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

# /lib/libcurses.a is what a program built on or for the target links, and
# those programs are position independent, so the archive holds the
# position-independent objects the shared libraries are built from.
AMD64_USER_CURSES_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,\
	$(BUILD)/dynamic/obj,curses)
$(BUILD)/lib/libcurses.a: $(AMD64_USER_CURSES_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/POSIX-R1.ELF: $(AMD64_USER_LIBC_OBJS) \
	$(BUILD)/user64/userland/tests/syscall-smoke.o platform/amd64/user.ld \
	$(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
 -z max-page-size=4096 -z stack-size=0x100000 \
 -T platform/amd64/user.ld \
 $(AMD64_USER_LIBC_OBJS) \
 $(BUILD)/user64/userland/tests/syscall-smoke.o -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(BUILD)/POSIX-R2.ELF: $(AMD64_USER_NET_LIBC_OBJS) \
	$(BUILD)/user64/userland/tests/posix-r2.o platform/amd64/user.ld \
	$(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
 -z max-page-size=4096 -z stack-size=0x100000 \
 -T platform/amd64/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
 $(BUILD)/user64/userland/tests/posix-r2.o -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(BUILD)/POSIX-R2-REMAINING.ELF: $(AMD64_USER_NET_LIBC_OBJS) \
	$(BUILD)/user64/userland/tests/posix-r2-remaining.o \
	platform/amd64/user.ld $(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
 -z max-page-size=4096 -z stack-size=0x100000 \
 -T platform/amd64/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
 $(BUILD)/user64/userland/tests/posix-r2-remaining.o -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(BUILD)/SUSV4-XSI.ELF: $(AMD64_USER_NET_LIBC_OBJS) \
	$(BUILD)/user64/userland/tests/susv4-xsi.o \
	platform/amd64/user.ld $(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
 -z max-page-size=4096 -z stack-size=0x100000 \
 -T platform/amd64/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
 $(BUILD)/user64/userland/tests/susv4-xsi.o -o $@
	@test -z "$$($(NM) -u $@)" || { $(NM) -u $@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

susv4-xsi-user-test: $(BUILD)/SUSV4-XSI.ELF

$(BUILD)/bin/sh: $(AMD64_APP_INPUTS) $(AMD64_APP_SH_OBJS)
	@mkdir -p $(dir $@)
	$(AMD64_APP_LINK) $(AMD64_APP_SH_OBJS) $(AMD64_APP_LIBS) -o $@
	$(AMD64_APP_CHECK) $@

$(BUILD)/SMP-STRESS.ELF: $(AMD64_USER_NET_LIBC_OBJS) \
	$(BUILD)/user64/userland/tests/smp-resource-stress.o \
	platform/amd64/user.ld $(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
 -z max-page-size=4096 -z stack-size=0x100000 \
 -T platform/amd64/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
 $(BUILD)/user64/userland/tests/smp-resource-stress.o -o $@
	@test -z "$$($(NM) -u $@)" || { $(NM) -u $@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

AMD64_USER_SYSCTL_OBJ := $(AMD64_APP_OBJ)/userland/base/sysctl/main.o
$(BUILD)/bin/sysctl: $(AMD64_APP_INPUTS) $(AMD64_USER_SYSCTL_OBJ)
	@mkdir -p $(dir $@)
	$(AMD64_APP_LINK) $(AMD64_USER_SYSCTL_OBJ) $(AMD64_APP_LIBS) -o $@
	$(AMD64_APP_CHECK) $@

AMD64_USER_MOUNT_OBJ := $(AMD64_APP_OBJ)/userland/base/mount/main.o
$(BUILD)/bin/mount: $(AMD64_APP_INPUTS) $(AMD64_USER_MOUNT_OBJ)
	@mkdir -p $(dir $@)
	$(AMD64_APP_LINK) $(AMD64_USER_MOUNT_OBJ) $(AMD64_APP_LIBS) -o $@
	$(AMD64_APP_CHECK) $@
$(BUILD)/bin/umount: $(BUILD)/bin/mount
	@mkdir -p $(dir $@)
	cp -f $< $@



USER_NET_COMMANDS := $(USERLAND_SELECTED_NETWORK_PROGRAMS)
USER_NET_COMMAND_TARGETS := $(addprefix $(BUILD)/bin/,$(USER_NET_COMMANDS))
AMD64_USER_NET_COMMON_OBJS := $(AMD64_APP_OBJ)/userland/base/net/netutil.o \
	$(AMD64_APP_OBJ)/userland/base/net/dhcp.o

define AMD64_USER_NET_COMMAND
$(BUILD)/bin/$(1): $(AMD64_APP_INPUTS) $(AMD64_USER_NET_COMMON_OBJS) \
	$(call ZEDBSD_USERLAND_OBJECTS,$(AMD64_APP_OBJ),$(1))
	@mkdir -p $$(dir $$@)
	$(AMD64_APP_LINK) $(AMD64_USER_NET_COMMON_OBJS) \
 $(call ZEDBSD_USERLAND_OBJECTS,$(AMD64_APP_OBJ),$(1)) $(AMD64_APP_LIBS) -o $$@
	$(AMD64_APP_CHECK) $$@
endef
$(foreach command,$(USER_NET_COMMANDS),\
	$(eval $(call AMD64_USER_NET_COMMAND,$(command))))
USER_BASIC_COMMANDS := $(filter $(ZEDBSD_USER_PROGRAMS),$(USERLAND_BASIC_PROGRAMS))
USER_BASIC_TARGETS := $(addprefix $(BUILD)/bin/,$(USER_BASIC_COMMANDS))
AMD64_USER_BASIC_COMMON_OBJ := $(AMD64_APP_OBJ)/userland/base/common/command.o \
	$(AMD64_APP_OBJ)/userland/base/common/pager.o

define AMD64_USER_BASIC_COMMAND
$(BUILD)/bin/$(1): $(AMD64_APP_INPUTS) $(AMD64_USER_BASIC_COMMON_OBJ) \
	$(call ZEDBSD_USERLAND_OBJECTS,$(AMD64_APP_OBJ),$(1))
	@mkdir -p $$(dir $$@)
	$(AMD64_APP_LINK) $(AMD64_USER_BASIC_COMMON_OBJ) \
 $(call ZEDBSD_USERLAND_OBJECTS,$(AMD64_APP_OBJ),$(1)) $(AMD64_APP_LIBS) -o $$@
	$(AMD64_APP_CHECK) $$@
endef
$(foreach command,$(filter-out camvk vkdemo vkcompute vkreset vkcoherency vkimage vkstage vkquad vkvideo-probe media-probe mediastorage display-events wltest wlshm mview wayland terminal files notes monitor pdfviewer settings imageview textedit videoplayer music photos phone calendar mailer kuidemo browser browser-probe xserver egltest glescompute glxtest zgears gpu-share-test gpu-fence-test acquire-fence-test gpu-forge-test menu-probe titlebar-probe popup-probe subsurface-probe seat-probe data-probe extras-probe tablet-probe keiland-ime ime-probe keiland-settings keiland-system keiland-notify printtest fidoctl passkey-fido2 ssh sshd ssh-keygen sftp-server,$(USER_BASIC_COMMANDS)),\
	$(eval $(call AMD64_USER_BASIC_COMMAND,$(command))))
# Static programs (the class static, ws168-p002): linked with the static C
# library alone and no runtime linker, as a child that sandbox_spawn starts
# needs (it opens no file, so it cannot load a shared library).
USER_STATIC_COMMANDS := $(foreach program,$(filter $(ZEDBSD_USER_PROGRAMS),$(USERLAND_PACKAGES)),\
	$(if $(filter static,$(USERLAND_$(program)_CLASS)),$(program)))

# A static program may link more objects than its package's (AMD64_USER_STATIC_EXTRA_<name>): keiland-preview
# (ws168-p003) takes the C library's mathematics and its parsing of floating point numbers, which the static C
# library does not have, compiled as the shared one's are (-mlong-double-64).
AMD64_USER_FLOAT_DIR := $(BUILD)/user64-float
AMD64_USER_FLOAT_OBJS := $(patsubst src/libc/%.c,$(AMD64_USER_FLOAT_DIR)/%.o,\
	$(ZEDBSD_LIBM_SOURCES) src/libc/softfloat.c src/libc/float-parse.c)
AMD64_USER_STATIC_EXTRA_keiland-preview := $(AMD64_USER_FLOAT_OBJS)

$(AMD64_USER_FLOAT_OBJS): $(AMD64_USER_FLOAT_DIR)/%.o: src/libc/%.c $(ZEDBSD_LIBM_HEADERS) src/libc/softfloat.h \
	$(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) -nostdinc -Iinclude/libc -Iinclude -I. -DHAL_ARCH_AMD64 -DKERN_USER_ABI_LP64 $(AMD64_USER_CFLAGS) \
 -mlong-double-64 -c $< -o $@

define AMD64_USER_STATIC_COMMAND
$(BUILD)/bin/$(1): $(AMD64_USER_LIBC_OBJS) $(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user64,$(1)) \
	$(AMD64_USER_STATIC_EXTRA_$(1)) platform/amd64/user.ld $(AMD64_USER_ELF_CHECK)
	@mkdir -p $$(dir $$@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
 -z max-page-size=4096 -z stack-size=0x100000 \
 -T platform/amd64/user.ld $(AMD64_USER_LIBC_OBJS) \
 $(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user64,$(1)) $(AMD64_USER_STATIC_EXTRA_$(1)) -o $$@
	@test -z "$$$$($(NM) -u $$@)" || { $(NM) -u $$@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $$@
endef
$(foreach command,$(USER_STATIC_COMMANDS),\
	$(eval $(call AMD64_USER_STATIC_COMMAND,$(command))))
# keiland-preview (ws168-p003) opens no file in its sandbox: its libpdf reads no substitute font file.
$(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user64,keiland-preview): AMD64_USER_CPPFLAGS += -DPDF_FONT_FILES=0
# ELF64 runtime linker and shared libc.
DYNAMIC_DIR := $(BUILD)/dynamic
DYNAMIC_CPPFLAGS := -nostdinc -I. -Iinclude \
	-isystem $(ZEDBSD_SYSROOT_AMD64)/usr/include \
	-DHAL_ARCH_AMD64 -DKERN_USER_ABI_LP64 -DKERN_DYNAMIC_LIBC
# Unwind tables are kept in the dynamic objects.  A C++ program raises an
# exception by walking the stack, and a frame with no unwind information stops
# that walk, so the shared C library has to describe its own frames for an
# exception to pass through a call it made.
DYNAMIC_CFLAGS := -m64 -march=x86-64 -mno-red-zone -Os -ffreestanding \
	-fPIC -fno-builtin -fno-stack-protector \
	-fasynchronous-unwind-tables \
	-ftls-model=global-dynamic -Wall -Wextra -Werror
DYNAMIC_LIBC_SOURCES := userland/base/libc/posix.c userland/base/libc/poll.c \
	userland/base/libc/termios.c userland/base/libc/pthread.c userland/base/libc/timer.c userland/base/libc/shm.c \
	userland/base/libc/semaphore.c userland/base/libc/mqueue.c userland/base/libc/dlfcn.c \
	userland/base/libc/socket.c userland/base/libc/resolver.c \
	userland/base/libc/resolver-dns.c userland/base/libc/signal.c \
	userland/base/libc/account.c userland/base/libc/crypt.c userland/base/libc/utmpx.c src/libc/heap.c \
	src/libc/string.c src/libc/ctype.c src/libc/locale.c src/libc/wide.c src/libc/int64.c \
	src/libc/strto.c src/libc/format.c \
	src/libc/stdio.c $(ZEDBSD_LIBC_USER_EXTRA_SOURCES)
DYNAMIC_LIBC_OBJS := $(patsubst %.c,$(DYNAMIC_DIR)/obj/%.o,\
	$(DYNAMIC_LIBC_SOURCES)) $(DYNAMIC_DIR)/obj/userland/base/libc/syscall.o
DYNAMIC_RTLD_OBJS := $(DYNAMIC_DIR)/obj/src/rtld/entry.o \
	$(DYNAMIC_DIR)/obj/src/rtld/tlsdesc.o \
	$(DYNAMIC_DIR)/obj/src/rtld/rtld.o \
	$(DYNAMIC_DIR)/obj/src/rtld/string.o
DYNAMIC_FLOAT_DIR := $(DYNAMIC_DIR)/float
DYNAMIC_LIBM_OBJS := $(patsubst src/libc/math/%.c,\
	$(DYNAMIC_FLOAT_DIR)/math/%.o,$(ZEDBSD_LIBM_SOURCES))
DYNAMIC_FLOAT_PARSE_OBJS := $(DYNAMIC_FLOAT_DIR)/softfloat.o \
	$(DYNAMIC_FLOAT_DIR)/float-parse.o
DYNAMIC_LIBC_OBJS += $(DYNAMIC_LIBM_OBJS) $(DYNAMIC_FLOAT_PARSE_OBJS)

$(DYNAMIC_DIR)/obj/%.o: %.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_CPPFLAGS) $(DYNAMIC_CFLAGS) -MMD -MP -c $< -o $@

$(DYNAMIC_DIR)/obj/userland/base/libc/syscall.o: \
	userland/base/libc/syscall-amd64.S include/hal/arch.h \
	include/hal/arch/amd64.h
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_CPPFLAGS) -m64 -c $< -o $@

$(DYNAMIC_DIR)/obj/src/rtld/entry.o: src/rtld/entry-amd64.S
	@mkdir -p $(dir $@)
	$(CC) -m64 -c $< -o $@

$(DYNAMIC_DIR)/obj/src/rtld/tlsdesc.o: src/rtld/tlsdesc-amd64.S
	@mkdir -p $(dir $@)
	$(CC) -m64 -c $< -o $@

$(DYNAMIC_DIR)/obj/userland/tests/tlstest.o: DYNAMIC_CFLAGS += -mtls-dialect=gnu2

$(DYNAMIC_LIBM_OBJS): $(DYNAMIC_FLOAT_DIR)/math/%.o: src/libc/math/%.c \
	$(ZEDBSD_LIBM_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -nostdinc -Iinclude/libc -Iinclude -I. $(DYNAMIC_CFLAGS) \
 -mlong-double-64 -c $< -o $@

$(DYNAMIC_FLOAT_DIR)/softfloat.o: src/libc/softfloat.c \
	src/libc/softfloat.h
	@mkdir -p $(dir $@)
	$(CC) -nostdinc -Iinclude/libc -Iinclude -I. $(DYNAMIC_CFLAGS) \
 -mlong-double-64 -c $< -o $@

$(DYNAMIC_FLOAT_DIR)/float-parse.o: src/libc/float-parse.c \
	src/libc/softfloat.h
	@mkdir -p $(dir $@)
	$(CC) -nostdinc -Iinclude/libc -Iinclude -I. $(DYNAMIC_CFLAGS) \
 -mlong-double-64 -c $< -o $@

$(DYNAMIC_DIR)/obj/src/libc/crt/crt1.o: src/libc/crt/crt1-amd64.S
	@mkdir -p $(dir $@)
	$(CC) -m64 -c $< -o $@

$(DYNAMIC_DIR)/ld.so: $(DYNAMIC_RTLD_OBJS)
	$(LD) -m elf_x86_64 -shared -Bsymbolic -e _rtld_start \
 --hash-style=sysv -z now -z relro -z separate-code $^ -o $@

# ws066-p002: -Bsymbolic-functions binds the library's calls to its own
# functions at link time, which leaves the loader 19 symbol relocations of
# libc.so instead of 515; the allocator and the functions that return its
# memory stay replaceable (userland/base/libc/interpose.list).
$(DYNAMIC_DIR)/libc.so: $(DYNAMIC_LIBC_OBJS) userland/base/libc/interpose.list
	$(LD) -m elf_x86_64 -shared -soname libc.so --hash-style=both \
 -Bsymbolic-functions --export-dynamic-symbol-list=userland/base/libc/interpose.list \
 -z now -z relro -z separate-code -z stack-size=0x100000 $(DYNAMIC_LIBC_OBJS) -o $@

# Portable libraries and applications share their source/link dependency graph.
DYNAMIC_SYSROOT := $(ZEDBSD_SYSROOT_AMD64)
DYNAMIC_LINK_CFLAGS := -m64
DYNAMIC_LINK_LDFLAGS := -m elf_x86_64
DYNAMIC_ELF_MACHINE := amd64
# libvulkan's thread-local variables through TLS descriptors, which ld.so resolves (src/rtld/tlsdesc-amd64.S).
DYNAMIC_VULKAN_TLS_CFLAGS := -mtls-dialect=gnu2
include platform/common/userland-dynamic.mk

$(DYNAMIC_DIR)/alt/rpathdep.so: \
	$(DYNAMIC_DIR)/obj/userland/tests/rpathdep.o $(DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 -shared -soname rpathdep.so --hash-style=gnu \
 -z now -z relro -z separate-code $< -o $@

$(DYNAMIC_DIR)/tlstest.so: \
	$(DYNAMIC_DIR)/obj/userland/tests/tlstest.o \
	$(DYNAMIC_DIR)/alt/rpathdep.so $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_x86_64 -shared -soname tlstest.so --hash-style=gnu \
 -z now -z relro -z separate-code --enable-new-dtags \
 -rpath '$$ORIGIN/alt' \
 $(DYNAMIC_DIR)/obj/userland/tests/tlstest.o \
 -L$(DYNAMIC_DIR)/alt -l:rpathdep.so -o $@

$(DYNAMIC_DIR)/rpathtest.so: \
	$(DYNAMIC_DIR)/obj/userland/tests/rpathtest.o \
	$(DYNAMIC_DIR)/alt/rpathdep.so $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_x86_64 -shared -soname rpthtest.so --hash-style=gnu \
 -z now -z relro -z separate-code --disable-new-dtags \
 -rpath '$$ORIGIN/alt' $< -L$(DYNAMIC_DIR)/alt \
 -l:rpathdep.so -o $@

$(DYNAMIC_DIR)/verstest.so: \
	$(DYNAMIC_DIR)/obj/userland/tests/versiontest.o \
	userland/tests/versiontest.map $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_x86_64 -shared -soname verstest.so --hash-style=gnu \
 -z now -z relro -z separate-code \
 --version-script=userland/tests/versiontest.map $< -o $@

$(DYNAMIC_DIR)/versuse.so: \
	$(DYNAMIC_DIR)/obj/userland/tests/versionuse.o \
	$(DYNAMIC_DIR)/verstest.so $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_x86_64 -shared -soname versuse.so --hash-style=gnu \
 -z now -z relro -z separate-code $< -L$(DYNAMIC_DIR) \
 -l:verstest.so -o $@

$(DYNAMIC_DIR)/dyntest: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_DIR)/obj/userland/tests/dyntest.o $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/ld.so $(DYNAMIC_DIR)/tlstest.so \
	$(DYNAMIC_DIR)/versuse.so
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=sysv,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
 $(DYNAMIC_DIR)/obj/userland/tests/dyntest.o \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libc.so -o $@

dynamic-userland-check: $(DYNAMIC_DIR)/ld.so $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/dyntest $(DYNAMIC_DIR)/tlstest.so \
	$(DYNAMIC_DIR)/rpathtest.so $(DYNAMIC_DIR)/verstest.so \
	$(DYNAMIC_DIR)/versuse.so tools/build/check-dynamic-elf.py
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role interpreter $(DYNAMIC_DIR)/ld.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role libc $(DYNAMIC_DIR)/libc.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role module $(DYNAMIC_DIR)/tlstest.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role rpath-module $(DYNAMIC_DIR)/rpathtest.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role version-definition $(DYNAMIC_DIR)/verstest.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role version-consumer $(DYNAMIC_DIR)/versuse.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role program $(DYNAMIC_DIR)/dyntest
	@echo "zedBSD amd64 dynamic userland artifacts: PASS"
.PHONY: dynamic-userland-check

AMD64_ARCH_IMAGE := $(ARCH_IMAGE_DIR)/amd64.img
include platform/common/userland-rootfs.mk
AMD64_ARCH_INPUTS := $(ZEDBSD_ROOTFS_INPUTS)
AMD64_ARCH_FILES := $(ZEDBSD_ROOTFS_FILES)
# E-127: a test image may replace rc.conf and add files (a oneshot service) from the command line:
#   make ... ZEDBSD_TEST_RC_CONF=plan/ws031/tests/vkprobe-rc.conf \
#            ZEDBSD_TEST_EXTRA_FILES='--file /etc/service.d/vkprobe=plan/ws031/tests/vkprobe-service' \
#            ZEDBSD_TEST_IMAGE_TAG=vkprobe   (required with the two above: its own cached rootfs image)
ifneq ($(ZEDBSD_TEST_RC_CONF),)
AMD64_ARCH_FILES := $(subst --file /etc/rc.conf=userland/base/etc/rc.conf,--file /etc/rc.conf=$(ZEDBSD_TEST_RC_CONF),$(AMD64_ARCH_FILES))
endif
AMD64_ARCH_FILES += $(ZEDBSD_TEST_EXTRA_FILES)
# The test rootfs is cached like any other image, so the files it carries have to be
# prerequisites of it.  $(wildcard) keeps only the words that name a file: an option name and
# a --mode permission are not paths and drop out here.
AMD64_ARCH_INPUTS += $(ZEDBSD_TEST_RC_CONF) \
	$(wildcard $(foreach a,$(ZEDBSD_TEST_EXTRA_FILES),$(lastword $(subst =, ,$(a)))))
$(eval $(call ZEDBSD_ARCH_IMAGE_RULE,$(AMD64_ARCH_IMAGE),amd64,$(AMD64_ARCH_INPUTS),$(AMD64_ARCH_FILES)))
$(eval $(call ZEDBSD_ROOTFS_TREE_RULE,amd64,$(AMD64_ARCH_INPUTS),$(AMD64_ARCH_FILES)))
AMD64_ARCH_UFS_IMAGE := $(ZEDBSD_ROOTFS_IMAGE_DIR)/amd64.ufs
# E-127: a test rootfs gets its own cached image, apart from the build's own
ifneq ($(ZEDBSD_TEST_IMAGE_TAG),)
AMD64_ARCH_UFS_IMAGE := $(ZEDBSD_ROOTFS_IMAGE_DIR)/amd64-$(ZEDBSD_TEST_IMAGE_TAG).ufs
endif
$(eval $(call ZEDBSD_ROOTFS_UFS_IMAGE_RULE,$(AMD64_ARCH_UFS_IMAGE),amd64))
rootfs: $(BUILD)/rootfs/.stamp


# ws035-p096: the boot logo on the boot FAT (/logo.ppm: the ZEDBSD FAT32 partition of the native layout (WS238), the payload FAT of the BIOS image), drawn by the
# UEFI and the BIOS loaders when zedbsd.cfg names it (logo=logo.ppm).  ws035-p107: it is the boot splash
# (userland/desktop/artwork/zedbsd-boot-splash.png, 1920x1080 without its spinner, "fit=contain" since ws035-p112: the loaders draw it in the
# middle over black bars, shrunk only when the screen is smaller, and the
# kernel's quiet console draws the spinner, src/drivers/platform/pcat/graphics/splash.c).
AMD64_BOOT_LOGO := $(BUILD)/boot-logo.ppm

$(AMD64_BOOT_LOGO): tools/build/make-boot-splash.py userland/desktop/artwork/zedbsd-boot-splash.png
	@mkdir -p $(dir $@)
	$(PYTHON) tools/build/make-boot-splash.py userland/desktop/artwork/zedbsd-boot-splash.png $@.tmp
	mv -f $@.tmp $@

# The BIOS image's zedbsd.cfg gets the graphical boot's lines too when ZEDBSD_GRAPHICAL_BOOT is y
# (ws035-p099; the BIOS loader draws the logo on its VBE framebuffer).
# WS193: the graphical login (login=graphical) is its own choice, ZEDBSD_GRAPHICAL_LOGIN, in the name as well.
AMD64_BIOS_ZEDBSD_CONFIG := $(BUILD)/zedbsd-bios-graphical-$(ZEDBSD_GRAPHICAL_BOOT)-login-$(ZEDBSD_GRAPHICAL_LOGIN)-kmsg-$(ZEDBSD_BOOT_KERNEL_MESSAGES).cfg

$(AMD64_BIOS_ZEDBSD_CONFIG): $(ZEDBSD_CONFIG)
	@mkdir -p $(dir $@)
	cp $< $@.tmp
	$(if $(filter y,$(ZEDBSD_GRAPHICAL_BOOT)),printf '%s\n' logo=logo.ppm >> $@.tmp)
	$(if $(filter y,$(ZEDBSD_GRAPHICAL_LOGIN)),printf '%s\n' login=graphical >> $@.tmp)
	$(if $(filter n,$(ZEDBSD_BOOT_KERNEL_MESSAGES)),printf '%s\n' kmsg=quiet >> $@.tmp)
	mv -f $@.tmp $@

$(BUILD)/bios-hdd-image.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2-chain.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix $(AMD64_ARCH_UFS_IMAGE) \
	$(DATA_IMAGE) $(SWAP_IMAGE) $(BUILD)/uefi/BOOTX64.EFI $(AMD64_BOOT_LOGO) $(AMD64_BIOS_ZEDBSD_CONFIG) \
	tools/build/make-bios-hdd-image.noct \
	platform/amd64/tools/check-amd64-gpt-image.noct
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct --backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force --machine pcat --gpt \
 --checker platform/amd64/tools/check-amd64-gpt-image.noct \
 --checker-runner $(NOCT) \
 --stage1 $(BUILD)/bootloader/stage1.bin \
 --stage2 $(BUILD)/bootloader/stage2-chain.bin \
 --partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
 --bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE --kernel $(BUILD)/vmunix \
 --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
 --zedbsd-config $(AMD64_BIOS_ZEDBSD_CONFIG) --logo $(AMD64_BOOT_LOGO) \
 --arch-profile amd64 --arch-image $(AMD64_ARCH_UFS_IMAGE) \
 --arch-format ufs --data-image $(DATA_IMAGE) \
 --swapfile $(SWAP_IMAGE) $@

$(BUILD)/ufs-root.img: $(AMD64_ARCH_UFS_IMAGE) \
	$(BUILD_TOOLS_DIR)/make-ufs-root-image.py tools/build/ufs_format.py
	$(PYTHON) $(BUILD_TOOLS_DIR)/make-ufs-root-image.py --force \
 --arch-profile amd64 --arch-image $(AMD64_ARCH_UFS_IMAGE) $@

$(BUILD)/ufs-root-hdd-image.img: $(BUILD)/bootloader/stage1-native.bin \
	$(BUILD)/bootloader/stage2-chain.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix $(BUILD)/ufs-root.img \
	$(NATIVE_ZEDBSD_CONFIG) \
	$(ZEDBSD_IMAGE_HOST) \
	$(BUILD_TOOLS_DIR)/make-bios-hdd-image.noct \
	$(BUILD_TOOLS_DIR)/check-bios-hdd-image.noct
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(BUILD_TOOLS_DIR)/make-bios-hdd-image.noct --backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force \
 --checker $(BUILD_TOOLS_DIR)/check-bios-hdd-image.noct \
 --checker-runner $(NOCT) \
 --machine pcat --stage1 $(BUILD)/bootloader/stage1-native.bin \
 --stage2 $(BUILD)/bootloader/stage2-chain.bin \
 --partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
 --bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE --kernel $(BUILD)/vmunix \
 --zedbsd-config $(NATIVE_ZEDBSD_CONFIG) \
 --ufs-root $(BUILD)/ufs-root.img --size-mib 193 $@

$(BUILD)/bios-hdd-image-fragmented.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix $(AMD64_ARCH_UFS_IMAGE) \
	$(DATA_IMAGE) $(SWAP_IMAGE) \
	$(BUILD)/uefi/BOOTX64.EFI tools/build/make-bios-hdd-image.noct \
	platform/amd64/tools/check-amd64-gpt-image.noct
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct --backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force --machine pcat --gpt \
 --checker platform/amd64/tools/check-amd64-gpt-image.noct \
 --checker-runner $(NOCT) \
 --stage1 $(BUILD)/bootloader/stage1.bin \
 --stage2 $(BUILD)/bootloader/stage2.bin \
 --partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
 --bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE --kernel $(BUILD)/vmunix \
 --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
 --zedbsd-config $(ZEDBSD_CONFIG) \
 --arch-profile amd64 --arch-image $(AMD64_ARCH_UFS_IMAGE) \
 --arch-format ufs --data-image $(DATA_IMAGE) \
 --swapfile $(SWAP_IMAGE) \
 --fragment-kernel $@

ifeq ($(ZEDBSD_VARIANT),native)
# WS062: the native layout.  WS238: the ESP carries only the UEFI loader; the
# kernel, zedbsd.cfg and the logo are on a FAT32 partition of the Basic Data
# type labelled ZEDBSD, which the loader finds by its zedbsd.cfg and makes
# boot0 (/boot/boot0), and which Windows and macOS mount like any FAT volume;
# the root is a UFS partition mounted read-write through rootpart=; swap is a
# partition of its own.  Nothing is layered: a write
# to the root goes to its partition, not through a loop device into a file
# on FAT.  The root image is built from the same staged tree as the overlay
# layout's rootfs.img, with room and inodes to be written to.  The root and
# swap are 1 GiB each, a 2 GiB image that CI publishes gzip-compressed
# (2026-09-26 user direction).
# ws035-p098: the lines of the graphical boot are added when ZEDBSD_GRAPHICAL_BOOT is y (the value is in
# the name, so switching it makes the image again).  ws035-p112: the graphical boot drops video= (640x480), so the UEFI
# loader asks GOP for 1920x1080, the splash's size, and draws black bars where the mode is another.
# ws075-p012: ZEDBSD_BOOT_EXTRA_LINES adds boot parameters of its own, one per word (for example
# display=hdmi display.mode=1920x1080@60); the words are in the name too, so changing them makes the image again.
AMD64_BOOT_EXTRA_EMPTY :=
AMD64_BOOT_EXTRA_SPACE := $(AMD64_BOOT_EXTRA_EMPTY) $(AMD64_BOOT_EXTRA_EMPTY)
AMD64_BOOT_EXTRA_TAG := $(if $(strip $(ZEDBSD_BOOT_EXTRA_LINES)),-$(subst $(AMD64_BOOT_EXTRA_SPACE),+,$(strip $(ZEDBSD_BOOT_EXTRA_LINES))))
AMD64_NATIVE_UEFI_ZEDBSD_CONFIG := $(BUILD)/zedbsd-native-uefi-graphical-$(ZEDBSD_GRAPHICAL_BOOT)-login-$(ZEDBSD_GRAPHICAL_LOGIN)-kmsg-$(ZEDBSD_BOOT_KERNEL_MESSAGES)$(AMD64_BOOT_EXTRA_TAG).cfg
AMD64_GRAPHICAL_BOOT_LINES := logo=logo.ppm
AMD64_NATIVE_ROOT_MIB ?= 1024
AMD64_NATIVE_ROOT_INODES ?= 65536
AMD64_NATIVE_SWAP_MIB ?= 1024
AMD64_NATIVE_BOOT_MIB ?= 256
# The sizes are in the names, so that changing one makes the image again.
AMD64_NATIVE_ROOT_IMAGE := $(ZEDBSD_ROOTFS_IMAGE_DIR)/amd64-native-root-$(AMD64_NATIVE_ROOT_MIB)m.ufs
ifneq ($(ZEDBSD_TEST_IMAGE_TAG),)
AMD64_NATIVE_ROOT_IMAGE := $(ZEDBSD_ROOTFS_IMAGE_DIR)/amd64-native-root-$(AMD64_NATIVE_ROOT_MIB)m-$(ZEDBSD_TEST_IMAGE_TAG).ufs
endif
AMD64_NATIVE_SWAP_IMAGE := $(BUILD)/native-swap-$(AMD64_NATIVE_SWAP_MIB)m.img

$(AMD64_NATIVE_ROOT_IMAGE): $(BUILD)/rootfs/.stamp $(ARCH_UFS_IMAGE_TOOLS)
	@mkdir -p $(dir $@)
	$(NOCT) --path=$(BUILD_TOOLS_DIR) \
 $(BUILD_TOOLS_DIR)/make-arch-overlay-ufs.noct \
 --backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force \
 --profile amd64 --output $@ --tree $(BUILD)/rootfs \
 --size-mib $(AMD64_NATIVE_ROOT_MIB) --min-inodes $(AMD64_NATIVE_ROOT_INODES)

$(AMD64_NATIVE_UEFI_ZEDBSD_CONFIG): platform/amd64/zedbsd-native-uefi.cfg
	@mkdir -p $(dir $@)
	cp $< $@.tmp
	$(if $(filter y,$(ZEDBSD_GRAPHICAL_BOOT)),grep -v '^video=' $< > $@.tmp)
	$(if $(filter y,$(ZEDBSD_GRAPHICAL_BOOT)),printf '%s\n' $(AMD64_GRAPHICAL_BOOT_LINES) >> $@.tmp)
	$(if $(filter y,$(ZEDBSD_GRAPHICAL_LOGIN)),printf '%s\n' login=graphical >> $@.tmp)
	$(if $(filter n,$(ZEDBSD_BOOT_KERNEL_MESSAGES)),printf '%s\n' kmsg=quiet >> $@.tmp)
	$(if $(strip $(ZEDBSD_BOOT_EXTRA_LINES)),printf '%s\n' $(ZEDBSD_BOOT_EXTRA_LINES) >> $@.tmp)
	mv -f $@.tmp $@

$(AMD64_NATIVE_SWAP_IMAGE): $(BUILD_TOOLS_DIR)/make-swapfile.noct
	@mkdir -p $(dir $@)
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(BUILD_TOOLS_DIR)/make-swapfile.noct \
 --size-mib $(AMD64_NATIVE_SWAP_MIB) --output $@

$(BUILD)/hdd-image.img: $(BUILD)/vmunix $(BUILD)/uefi/BOOTX64.EFI \
	$(AMD64_NATIVE_UEFI_ZEDBSD_CONFIG) $(AMD64_NATIVE_ROOT_IMAGE) \
	$(AMD64_NATIVE_SWAP_IMAGE) $(ZEDBSD_IMAGE_HOST) $(AMD64_BOOT_LOGO) \
	$(IMAGE_CONTRACT_STAMP) \
	platform/amd64/tools/check-amd64-native-image.py
	$(ZEDBSD_IMAGE_HOST) disk --machine pcat --layout native \
 --kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
 --zedbsd-config $(AMD64_NATIVE_UEFI_ZEDBSD_CONFIG) --logo $(AMD64_BOOT_LOGO) \
 --ufs-root $(AMD64_NATIVE_ROOT_IMAGE) --swapfile $(AMD64_NATIVE_SWAP_IMAGE) \
 --fat-size-mib $(AMD64_NATIVE_BOOT_MIB) $@.unchecked
	$(PYTHON) platform/amd64/tools/check-amd64-native-image.py \
 --kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
 --zedbsd-config $(AMD64_NATIVE_UEFI_ZEDBSD_CONFIG) --logo $(AMD64_BOOT_LOGO) \
 --ufs-root $(AMD64_NATIVE_ROOT_IMAGE) --swap $(AMD64_NATIVE_SWAP_IMAGE) \
 $@.unchecked
	mv -f $@.unchecked $@
else
$(BUILD)/hdd-image.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage1-native.bin \
	$(BUILD)/bootloader/stage2-chain.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix $(AMD64_ARCH_UFS_IMAGE) \
	$(DATA_IMAGE) $(SWAP_IMAGE) $(BUILD)/uefi/BOOTX64.EFI \
	$(ZEDBSD_CONFIG) $(ZEDBSD_IMAGE_HOST) \
	$(IMAGE_CONTRACT_STAMP) tools/build/make-bios-hdd-image.noct \
	tools/build/zedbuild.noct tools/build/overlay_journal_format.noct \
	platform/amd64/tools/check-amd64-gpt-image.noct
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct \
 --backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force --machine pcat \
 --layout $(ZEDBSD_VARIANT) \
 --checker platform/amd64/tools/check-amd64-gpt-image.noct \
 --checker-runner $(NOCT) \
 --stage1 $(IMAGE_STAGE1) \
 --stage2 $(BUILD)/bootloader/stage2-chain.bin \
 --partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
 --bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
 --kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
 --zedbsd-config $(ZEDBSD_CONFIG) \
 --arch-profile amd64 --arch-image $(AMD64_ARCH_UFS_IMAGE) \
 --arch-format ufs --data-image $(DATA_IMAGE) \
 --swapfile $(SWAP_IMAGE) $@
endif

AMD64_DEFERRED_TEST_UFS := $(ARCH_IMAGE_DIR)/amd64-deferred-test.ufs
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(AMD64_DEFERRED_TEST_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) tests/deferred-stub-zinit.rc,\
	$(AMD64_ARCH_FILES) --file /etc/zinit.rc=tests/deferred-stub-zinit.rc))

$(BUILD)/deferred-stub-qemu.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix \
	$(AMD64_DEFERRED_TEST_UFS) $(DATA_IMAGE) $(SWAP_IMAGE) \
	$(BUILD)/uefi/BOOTX64.EFI tools/build/make-bios-hdd-image.noct \
	platform/amd64/tools/check-amd64-gpt-image.noct
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct --backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force --machine pcat --gpt \
 --checker platform/amd64/tools/check-amd64-gpt-image.noct \
 --checker-runner $(NOCT) \
 --stage1 $(BUILD)/bootloader/stage1.bin \
 --stage2 $(BUILD)/bootloader/stage2.bin \
 --partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
 --bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
 --kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
 --zedbsd-config $(ZEDBSD_CONFIG) \
 --arch-profile amd64 --arch-image $(AMD64_DEFERRED_TEST_UFS) \
 --arch-format ufs --data-image $(DATA_IMAGE) \
 --swapfile $(SWAP_IMAGE) $@

deferred-stub-qemu-test: $(BUILD)/deferred-stub-qemu.img \
	tests/deferred-stub-qemu-test.py
	$(PYTHON) tests/deferred-stub-qemu-test.py \
 --qemu $(QEMU) --image $(BUILD)/deferred-stub-qemu.img

AMD64_POSIX_PHASE2_TEST_UFS := $(ARCH_IMAGE_DIR)/amd64-posix-phase2-test.ufs
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(AMD64_POSIX_PHASE2_TEST_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) tests/posix-phase2-zinit.rc \
	tests/posix-phase2-input.txt tests/posix-phase2-tsort.txt \
	tests/posix-phase2-uudecode.txt,\
	$(AMD64_ARCH_FILES) --file /etc/zinit.rc=tests/posix-phase2-zinit.rc \
	--file /etc/posix-phase2-input=tests/posix-phase2-input.txt \
	--file /etc/posix-phase2-tsort=tests/posix-phase2-tsort.txt \
	--file /etc/posix-phase2-uudecode=tests/posix-phase2-uudecode.txt))

$(BUILD)/posix-phase2-qemu.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix \
	$(AMD64_POSIX_PHASE2_TEST_UFS) $(DATA_IMAGE) $(SWAP_IMAGE) \
	$(BUILD)/uefi/BOOTX64.EFI tools/build/make-bios-hdd-image.noct \
	platform/amd64/tools/check-amd64-gpt-image.noct
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct --backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force --machine pcat --gpt \
 --checker platform/amd64/tools/check-amd64-gpt-image.noct \
 --checker-runner $(NOCT) \
 --stage1 $(BUILD)/bootloader/stage1.bin \
 --stage2 $(BUILD)/bootloader/stage2.bin \
 --partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
 --bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
 --kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
 --zedbsd-config $(ZEDBSD_CONFIG) \
 --arch-profile amd64 --arch-image $(AMD64_POSIX_PHASE2_TEST_UFS) \
 --arch-format ufs --data-image $(DATA_IMAGE) \
 --swapfile $(SWAP_IMAGE) $@

posix-phase2-qemu-test: $(BUILD)/posix-phase2-qemu.img \
	tests/posix-phase2-qemu-test.py
	$(PYTHON) tests/posix-phase2-qemu-test.py \
 --qemu $(QEMU) --image $(BUILD)/posix-phase2-qemu.img

AMD64_POSIX_PHASE3_TEST_UFS := $(ARCH_IMAGE_DIR)/amd64-posix-phase3-test.ufs
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(AMD64_POSIX_PHASE3_TEST_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) tests/posix-phase3-zinit.rc \
	tests/fixtures/phase3-messages.msg \
	tests/fixtures/zed-test-locale.src tests/fixtures/UTF-8.charmap,\
	$(AMD64_ARCH_FILES) --file /etc/zinit.rc=tests/posix-phase3-zinit.rc \
	--file /etc/phase3-messages.msg=tests/fixtures/phase3-messages.msg \
	--file /etc/zed-test-locale.src=tests/fixtures/zed-test-locale.src \
	--file /etc/UTF-8.charmap=tests/fixtures/UTF-8.charmap))

$(BUILD)/posix-phase3-qemu.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix \
	$(AMD64_POSIX_PHASE3_TEST_UFS) $(DATA_IMAGE) $(SWAP_IMAGE) \
	$(BUILD)/uefi/BOOTX64.EFI tools/build/make-bios-hdd-image.noct \
	platform/amd64/tools/check-amd64-gpt-image.noct
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct --backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force --machine pcat --gpt \
 --checker platform/amd64/tools/check-amd64-gpt-image.noct \
 --checker-runner $(NOCT) \
 --stage1 $(BUILD)/bootloader/stage1.bin \
 --stage2 $(BUILD)/bootloader/stage2.bin \
 --partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
 --bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
 --kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
 --zedbsd-config $(ZEDBSD_CONFIG) \
 --arch-profile amd64 --arch-image $(AMD64_POSIX_PHASE3_TEST_UFS) \
 --arch-format ufs --data-image $(DATA_IMAGE) \
 --swapfile $(SWAP_IMAGE) $@

posix-phase3-qemu-test: $(BUILD)/posix-phase3-qemu.img \
	tests/posix-phase3-qemu-test.py
	$(PYTHON) tests/posix-phase3-qemu-test.py \
 --qemu $(QEMU) --image $(BUILD)/posix-phase3-qemu.img

AMD64_POSIX_PHASE4_TEST_UFS := $(ARCH_IMAGE_DIR)/amd64-posix-phase4-test.ufs
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(AMD64_POSIX_PHASE4_TEST_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) tests/posix-phase4-zinit.rc \
	tests/fixtures/phase4.m4 tests/fixtures/phase4-m4.expected \
	tests/fixtures/phase4.ed,\
	$(AMD64_ARCH_FILES) --file /etc/zinit.rc=tests/posix-phase4-zinit.rc \
	--file /etc/phase4.m4=tests/fixtures/phase4.m4 \
	--file /etc/phase4-m4.expected=tests/fixtures/phase4-m4.expected \
	--file /etc/phase4.ed=tests/fixtures/phase4.ed))

$(BUILD)/posix-phase4-qemu.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix \
	$(AMD64_POSIX_PHASE4_TEST_UFS) $(DATA_IMAGE) $(SWAP_IMAGE) \
	$(BUILD)/uefi/BOOTX64.EFI tools/build/make-bios-hdd-image.noct \
	platform/amd64/tools/check-amd64-gpt-image.noct
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct --backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force --machine pcat --gpt \
 --checker platform/amd64/tools/check-amd64-gpt-image.noct \
 --checker-runner $(NOCT) \
 --stage1 $(BUILD)/bootloader/stage1.bin \
 --stage2 $(BUILD)/bootloader/stage2.bin \
 --partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
 --bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
 --kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
 --zedbsd-config $(ZEDBSD_CONFIG) \
 --arch-profile amd64 --arch-image $(AMD64_POSIX_PHASE4_TEST_UFS) \
 --arch-format ufs --data-image $(DATA_IMAGE) \
 --swapfile $(SWAP_IMAGE) $@

posix-phase4-qemu-test: $(BUILD)/posix-phase4-qemu.img \
	tests/posix-phase4-qemu-test.py
	$(PYTHON) tests/posix-phase4-qemu-test.py \
 --qemu $(QEMU) --image $(BUILD)/posix-phase4-qemu.img

$(BUILD)/bin/posix-phase5-helper: $(AMD64_USER_NET_LIBC_OBJS) \
	$(BUILD)/user64/userland/tests/posix-phase5-helper.o \
	platform/amd64/user.ld $(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
 -z max-page-size=4096 -z stack-size=0x100000 \
 -T platform/amd64/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
 $(BUILD)/user64/userland/tests/posix-phase5-helper.o -o $@
	@test -z "$$($(NM) -u $@)" || { $(NM) -u $@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

AMD64_POSIX_PHASE5_TEST_UFS := $(ARCH_IMAGE_DIR)/amd64-posix-phase5-test.ufs
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(AMD64_POSIX_PHASE5_TEST_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) $(BUILD)/bin/posix-phase5-helper \
	tests/posix-phase5-zinit.rc,\
	$(AMD64_ARCH_FILES) \
	--file /bin/posix-phase5-helper=$(BUILD)/bin/posix-phase5-helper \
	--file /etc/zinit.rc=tests/posix-phase5-zinit.rc))

$(BUILD)/posix-phase5-qemu.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix \
	$(AMD64_POSIX_PHASE5_TEST_UFS) $(DATA_IMAGE) $(SWAP_IMAGE) \
	$(BUILD)/uefi/BOOTX64.EFI tools/build/make-bios-hdd-image.noct \
	platform/amd64/tools/check-amd64-gpt-image.noct
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct --backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force --machine pcat --gpt \
 --checker platform/amd64/tools/check-amd64-gpt-image.noct \
 --checker-runner $(NOCT) \
 --stage1 $(BUILD)/bootloader/stage1.bin \
 --stage2 $(BUILD)/bootloader/stage2.bin \
 --partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
 --bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
 --kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
 --zedbsd-config $(ZEDBSD_CONFIG) \
 --arch-profile amd64 --arch-image $(AMD64_POSIX_PHASE5_TEST_UFS) \
 --arch-format ufs --data-image $(DATA_IMAGE) \
 --swapfile $(SWAP_IMAGE) $@

posix-phase5-qemu-test: $(BUILD)/posix-phase5-qemu.img \
	tests/posix-phase5-qemu-test.py
	$(PYTHON) tests/posix-phase5-qemu-test.py \
 --qemu $(QEMU) --image $(BUILD)/posix-phase5-qemu.img

AMD64_POSIX_PHASE6_TEST_UFS := $(ARCH_IMAGE_DIR)/amd64-posix-phase6-test.ufs
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(AMD64_POSIX_PHASE6_TEST_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) tests/posix-phase6-zinit.rc \
	tests/fixtures/phase6-cflow.c \
	$(BUILD)/user64/userland/base/cflow/main.o,\
	$(AMD64_ARCH_FILES) --file /etc/zinit.rc=tests/posix-phase6-zinit.rc \
	--file /etc/phase6-cflow.c=tests/fixtures/phase6-cflow.c \
	--file /etc/phase6-object.o=$(BUILD)/user64/userland/base/cflow/main.o))

$(BUILD)/posix-phase6-qemu.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix \
	$(AMD64_POSIX_PHASE6_TEST_UFS) $(DATA_IMAGE) $(SWAP_IMAGE) \
	$(BUILD)/uefi/BOOTX64.EFI tools/build/make-bios-hdd-image.noct \
	platform/amd64/tools/check-amd64-gpt-image.noct
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct --backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force --machine pcat --gpt \
 --checker platform/amd64/tools/check-amd64-gpt-image.noct \
 --checker-runner $(NOCT) \
 --stage1 $(BUILD)/bootloader/stage1.bin \
 --stage2 $(BUILD)/bootloader/stage2.bin \
 --partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
 --bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
 --kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
 --zedbsd-config $(ZEDBSD_CONFIG) \
 --arch-profile amd64 --arch-image $(AMD64_POSIX_PHASE6_TEST_UFS) \
 --arch-format ufs --data-image $(DATA_IMAGE) \
 --swapfile $(SWAP_IMAGE) $@

posix-phase6-qemu-test: $(BUILD)/posix-phase6-qemu.img \
	tests/posix-phase6-qemu-test.py
	$(PYTHON) tests/posix-phase6-qemu-test.py \
 --qemu $(QEMU) --image $(BUILD)/posix-phase6-qemu.img

AMD64_POSIX_PHASE7_TEST_UFS := $(ARCH_IMAGE_DIR)/amd64-posix-phase7-test.ufs
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(AMD64_POSIX_PHASE7_TEST_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) tests/posix-phase7-zinit.rc \
	tests/fixtures/phase6-cflow.c,\
	$(AMD64_ARCH_FILES) --file /etc/zinit.rc=tests/posix-phase7-zinit.rc \
	--file /etc/phase7-input=tests/fixtures/phase6-cflow.c))

$(BUILD)/posix-phase7-qemu.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix \
	$(AMD64_POSIX_PHASE7_TEST_UFS) $(DATA_IMAGE) $(SWAP_IMAGE) \
	$(BUILD)/uefi/BOOTX64.EFI tools/build/make-bios-hdd-image.noct \
	platform/amd64/tools/check-amd64-gpt-image.noct
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct --backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force --machine pcat --gpt \
 --checker platform/amd64/tools/check-amd64-gpt-image.noct \
 --checker-runner $(NOCT) \
 --stage1 $(BUILD)/bootloader/stage1.bin \
 --stage2 $(BUILD)/bootloader/stage2.bin \
 --partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
 --bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
 --kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
 --zedbsd-config $(ZEDBSD_CONFIG) \
 --arch-profile amd64 --arch-image $(AMD64_POSIX_PHASE7_TEST_UFS) \
 --arch-format ufs --data-image $(DATA_IMAGE) \
 --swapfile $(SWAP_IMAGE) $@

posix-phase7-qemu-test: $(BUILD)/posix-phase7-qemu.img \
	tests/posix-phase7-qemu-test.py
	$(PYTHON) tests/posix-phase7-qemu-test.py \
 --qemu $(QEMU) --image $(BUILD)/posix-phase7-qemu.img

AMD64_POSIX_PHASE8_TEST_UFS := $(ARCH_IMAGE_DIR)/amd64-posix-phase8-test.ufs
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(AMD64_POSIX_PHASE8_TEST_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) tests/posix-phase8-zinit.rc \
	tests/fixtures/phase8-initial.txt tests/fixtures/phase8-second.txt,\
	$(AMD64_ARCH_FILES) --file /etc/zinit.rc=tests/posix-phase8-zinit.rc \
	--file /etc/phase8-initial=tests/fixtures/phase8-initial.txt \
	--file /etc/phase8-second=tests/fixtures/phase8-second.txt))

$(BUILD)/posix-phase8-qemu.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix \
	$(AMD64_POSIX_PHASE8_TEST_UFS) $(DATA_IMAGE) $(SWAP_IMAGE) \
	$(BUILD)/uefi/BOOTX64.EFI tools/build/make-bios-hdd-image.noct \
	platform/amd64/tools/check-amd64-gpt-image.noct
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct --backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force --machine pcat --gpt \
 --checker platform/amd64/tools/check-amd64-gpt-image.noct \
 --checker-runner $(NOCT) \
 --stage1 $(BUILD)/bootloader/stage1.bin \
 --stage2 $(BUILD)/bootloader/stage2.bin \
 --partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
 --bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
 --kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
 --zedbsd-config $(ZEDBSD_CONFIG) \
 --arch-profile amd64 --arch-image $(AMD64_POSIX_PHASE8_TEST_UFS) \
 --arch-format ufs --data-image $(DATA_IMAGE) \
 --swapfile $(SWAP_IMAGE) $@

posix-phase8-qemu-test: $(BUILD)/posix-phase8-qemu.img \
	tests/posix-phase8-qemu-test.py
	$(PYTHON) tests/posix-phase8-qemu-test.py \
 --qemu $(QEMU) --image $(BUILD)/posix-phase8-qemu.img

AMD64_PHASE19_TEST_UFS := $(ARCH_IMAGE_DIR)/amd64-phase19-test.ufs
AMD64_PHASE19_TEST_FILES := $(subst \
	--file /etc/rc.conf=userland/base/etc/rc.conf,\
	--file /etc/rc.conf=tests/phase19-rc.conf,$(AMD64_ARCH_FILES))
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(AMD64_PHASE19_TEST_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) tests/phase19-rc.conf tests/phase19-service \
	tests/phase19-smoke.sh,\
	$(AMD64_PHASE19_TEST_FILES) \
	--file /etc/service.d/phase19_smoke=tests/phase19-service \
	--file /etc/phase19-smoke.sh=tests/phase19-smoke.sh))

$(BUILD)/phase19-qemu.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix \
	$(AMD64_PHASE19_TEST_UFS) $(DATA_IMAGE) $(SWAP_IMAGE) \
	$(BUILD)/uefi/BOOTX64.EFI tools/build/make-bios-hdd-image.noct \
	platform/amd64/tools/check-amd64-gpt-image.noct
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct --backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force --machine pcat --gpt \
 --checker platform/amd64/tools/check-amd64-gpt-image.noct \
 --checker-runner $(NOCT) \
 --stage1 $(BUILD)/bootloader/stage1.bin \
 --stage2 $(BUILD)/bootloader/stage2.bin \
 --partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
 --bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
 --kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
 --zedbsd-config $(ZEDBSD_CONFIG) \
 --arch-profile amd64 --arch-image $(AMD64_PHASE19_TEST_UFS) \
 --arch-format ufs --data-image $(DATA_IMAGE) \
 --swapfile $(SWAP_IMAGE) $@

phase19-qemu-test: $(BUILD)/phase19-qemu.img tests/phase19-qemu-test.py
	$(PYTHON) tests/phase19-qemu-test.py \
 --qemu qemu-system-x86_64 --image $(BUILD)/phase19-qemu.img

AMD64_PHASE20_TEST_UFS := $(ARCH_IMAGE_DIR)/amd64-phase20-test.ufs
AMD64_PHASE20_TEST_FILES := $(subst \
	--file /etc/rc.conf=userland/base/etc/rc.conf,\
	--file /etc/rc.conf=tests/phase20-rc.conf,$(AMD64_ARCH_FILES))
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(AMD64_PHASE20_TEST_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) tests/phase20-rc.conf tests/phase20-service \
	tests/phase20-smoke.sh,\
	$(AMD64_PHASE20_TEST_FILES) \
	--file /etc/service.d/phase20_smoke=tests/phase20-service \
	--file /etc/phase20-smoke.sh=tests/phase20-smoke.sh))

$(BUILD)/phase20-qemu.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix \
	$(AMD64_PHASE20_TEST_UFS) $(DATA_IMAGE) $(SWAP_IMAGE) \
	$(BUILD)/uefi/BOOTX64.EFI tools/build/make-bios-hdd-image.noct \
	platform/amd64/tools/check-amd64-gpt-image.noct
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct --backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force --machine pcat --gpt \
 --checker platform/amd64/tools/check-amd64-gpt-image.noct \
 --checker-runner $(NOCT) \
 --stage1 $(BUILD)/bootloader/stage1.bin \
 --stage2 $(BUILD)/bootloader/stage2.bin \
 --partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
 --bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
 --kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
 --zedbsd-config $(ZEDBSD_CONFIG) \
 --arch-profile amd64 --arch-image $(AMD64_PHASE20_TEST_UFS) \
 --arch-format ufs --data-image $(DATA_IMAGE) \
 --swapfile $(SWAP_IMAGE) $@

.PHONY: phase20-qemu-test phase20-qemu-test-inner \
	phase20-interactive-shell-qemu-test
phase20-qemu-test:
	$(MAKE) BUILD=build/amd64-phase20 CONFIG_DRIVER_NE2000=y \
 phase20-qemu-test-inner

phase20-qemu-test-inner: $(BUILD)/phase20-qemu.img \
	tests/phase20-qemu-test.py
	$(PYTHON) tests/phase20-qemu-test.py \
 --qemu qemu-system-x86_64 --image $(BUILD)/phase20-qemu.img

phase20-interactive-shell-qemu-test: $(BUILD)/hdd-image.img \
	tests/phase20-interactive-shell-qemu-test.py
	$(PYTHON) tests/phase20-interactive-shell-qemu-test.py \
 --qemu qemu-system-x86_64 --image $(BUILD)/hdd-image.img

AMD64_POSIX_PHASE85_CURSES_SOURCES := tests/posix-phase85-curses.c \
	userland/base/curses/curses.c userland/base/common/terminfo.c
AMD64_POSIX_PHASE85_CURSES_OBJS := $(patsubst %.c,\
	$(BUILD)/user64/%.o,$(AMD64_POSIX_PHASE85_CURSES_SOURCES))
$(BUILD)/bin/phase85-curses-test: $(AMD64_USER_LIBC_OBJS) \
	$(AMD64_POSIX_PHASE85_CURSES_OBJS) platform/amd64/user.ld \
	$(AMD64_USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
 -z max-page-size=4096 -z stack-size=0x100000 \
 -T platform/amd64/user.ld $(AMD64_USER_LIBC_OBJS) \
 $(AMD64_POSIX_PHASE85_CURSES_OBJS) -o $@
	@test -z "$$($(NM) -u $@)" || { $(NM) -u $@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

AMD64_POSIX_PHASE85_TEST_UFS := $(ARCH_IMAGE_DIR)/amd64-posix-phase85-test.ufs
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(AMD64_POSIX_PHASE85_TEST_UFS),amd64,\
	$(AMD64_ARCH_INPUTS) $(BUILD)/bin/phase85-curses-test \
	tests/posix-phase85-zinit.rc tests/fixtures/phase85-terminal.ti,\
	$(AMD64_ARCH_FILES) \
	--file /bin/phase85-curses-test=$(BUILD)/bin/phase85-curses-test \
	--file /etc/zinit.rc=tests/posix-phase85-zinit.rc \
	--file /etc/phase85-terminal.ti=tests/fixtures/phase85-terminal.ti))

$(BUILD)/posix-phase85-qemu.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix \
	$(AMD64_POSIX_PHASE85_TEST_UFS) $(DATA_IMAGE) $(SWAP_IMAGE) \
	$(BUILD)/uefi/BOOTX64.EFI tools/build/make-bios-hdd-image.noct \
	platform/amd64/tools/check-amd64-gpt-image.noct
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct --backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force --machine pcat --gpt \
 --checker platform/amd64/tools/check-amd64-gpt-image.noct \
 --checker-runner $(NOCT) \
 --stage1 $(BUILD)/bootloader/stage1.bin \
 --stage2 $(BUILD)/bootloader/stage2.bin \
 --partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
 --bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
 --kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
 --zedbsd-config $(ZEDBSD_CONFIG) \
 --arch-profile amd64 --arch-image $(AMD64_POSIX_PHASE85_TEST_UFS) \
 --arch-format ufs --data-image $(DATA_IMAGE) \
 --swapfile $(SWAP_IMAGE) $@

posix-phase85-qemu-test: $(BUILD)/posix-phase85-qemu.img \
	tests/posix-phase85-qemu-test.py
	$(PYTHON) tests/posix-phase85-qemu-test.py \
 --qemu $(QEMU) --image $(BUILD)/posix-phase85-qemu.img

posix-phase10-qemu-test: phase10-local-source-check posix-phase4-qemu-test
	@echo "zedBSD POSIX Phase 10 local replacements amd64 QEMU test: PASS"

amd64-hal-compile: $(AMD64_HAL_OBJS)
	@echo "HAL amd64/PCAT compile check: PASS"
CHECK_RUN_TARGETS += amd64-hal-compile

.PHONY: amd64-hal-compile deferred-stub-qemu-test posix-phase2-qemu-test \
	posix-phase3-qemu-test posix-phase4-qemu-test posix-phase5-qemu-test \
	posix-phase6-qemu-test posix-phase7-qemu-test posix-phase8-qemu-test \
	posix-phase85-qemu-test posix-phase10-qemu-test

$(BUILD)/uefi/memory-map-v6.o: bootloader/uefi/memory-map-v6.c bootloader/uefi/memory-map.h bootloader/common/memory-map.h bootloader/include/amd64-handoff.h
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -I. -c $< -o $@

$(BUILD)/uefi/common-memory-map.o: bootloader/common/memory-map.c bootloader/common/memory-map.h bootloader/include/amd64-handoff.h
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -I. -c $< -o $@

$(BUILD)/bootloader/bios-memory-map.i386.o: bootloader/bios/memory-map.c bootloader/common/memory-map.h bootloader/bios/memory-map.h bootloader/include/amd64-handoff.h
	@mkdir -p $(dir $@)
	$(CC) -m16 -march=i386 -mtune=i386 -Os -ffreestanding -fno-pic -fno-pie \
 -fno-stack-protector -fno-asynchronous-unwind-tables \
 -fno-unwind-tables -fno-builtin -Wall -Wextra -Werror -I. \
 -c $< -o $@

$(BUILD)/bootloader/common-memory-map.i386.o: bootloader/common/memory-map.c bootloader/common/memory-map.h bootloader/bios/memory-map.h bootloader/include/amd64-handoff.h
	@mkdir -p $(dir $@)
	$(CC) -m16 -march=i386 -mtune=i386 -Os -ffreestanding -fno-pic -fno-pie \
 -fno-stack-protector -fno-asynchronous-unwind-tables \
 -fno-unwind-tables -fno-builtin -Wall -Wextra -Werror -I. \
 -c $< -o $@

# The copied boot-source record is part of the UEFI producer ABI.
$(BUILD)/uefi/bootx64.o $(BUILD)/uefi/volume-discovery.o: include/kern/boot.h

# The installer's units and image (make installer-image), after the native image's variables it uses.
include platform/amd64/installer-image.mk
