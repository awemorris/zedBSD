#
# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
#

#
# Raspberry Pi 4 bootstrap rules.
#

#
# Toolchain
#

ARM64_CC ?=       $(ZEDBSD_TARGET_LLVM_BIN)/clang --target=aarch64-unknown-zedbsd
ARM64_LD ?=       $(ZEDBSD_TARGET_LLVM_BIN)/ld.lld
ARM64_OBJCOPY ?=  $(ZEDBSD_TARGET_LLVM_BIN)/llvm-objcopy
ARM64_NM ?=       $(ZEDBSD_TARGET_LLVM_BIN)/llvm-nm
AWK ?=            awk

#
# Basic Settings
#

ARM64_PLATFORM :=      platform/arm64
ARM64_BSP_DIR :=       bsp-rpi4
ARM64_SOC_DIR :=       bsp-rpi-common
ARM64_BOARD_DEFINE :=  -DHAL_BOARD_RPI4

#
# Flags
#

ARM64_CPPFLAGS := \
	-nostdlibinc \
	-Iinclude \
	-Isrc -I. \
	-Isrc/hal/arm64 \
	-Isrc/hal/arm64/$(ARM64_BSP_DIR_$(ZEDBSD_BOARD)) \
	-Isrc/hal/arm64/$(ARM64_SOC_DIR_$(ZEDBSD_BOARD)) \
	-DHAL_ARCH_ARM64 $(ARM64_BOARD_DEFINE_$(ZEDBSD_BOARD)) \
	-DKERN_USER_ABI_AARCH64 -DKERN_USER_ABI_LP64 \
	$(ZEDBSD_CONFIG_CPPFLAGS)

ARM64_CFLAGS := \
	-march=armv8-a \
	-mno-outline-atomics \
	-mgeneral-regs-only \
	-ffreestanding \
	-fno-pic -fno-pie \
	-fno-stack-protector \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-fno-common \
	-ffunction-sections \
	-fdata-sections \
	-Os \
	-Wall \
	-Wextra \
	-Werror \
	$(ZEDBSD_KERNEL_LTO_CFLAGS)

#
# HAL
#

ARM64_HAL_C := \
	src/hal/arm64/smp.c \
	src/hal/arm64/percpu.c \
	src/hal/arm64/watch.c \
	src/hal/idle-suspend-unsupported.c \
	src/hal/arm64/asm.c \
	src/hal/arm64/lib.c \
	src/hal/arm64/page.c \
	src/hal/arm64/space.c \
	src/hal/arm64/int.c \
	src/hal/arm64/irq.c \
	src/hal/arm64/task.c \
	src/hal/arm64/cmain.c \
	src/hal/arm64/debug.c \
	src/hal/arm64/cons.c \
	src/hal/arm64/framebuffer.c \
	src/hal/arm64/boot.c \
	src/hal/arm64/gic.c \
	src/hal/arm64/clock.c \
	src/hal/arm64/font.c \
	src/hal/arm64/led.c \
	src/hal/arm64/bsp-rpi4/uart.c src/hal/arm64/bsp-rpi4/fdt.c \
	src/hal/arm64/bsp-rpi4/activity-led.c src/hal/arm64/bsp-rpi4/board.c \
	src/hal/arm64/bsp-rpi4/watch-hvs.c \
	src/hal/arm64/bsp-rpi-common/mailbox.c \
	src/hal/arm64/bsp-rpi-common/framebuffer-mailbox.c

ARM64_HAL_S := \
	src/hal/arm64/locore.S \
	src/hal/arm64/trap.S \
	src/hal/arm64/dispatch.S

ARM64_HAL_OBJS := \
	$(patsubst %.c,$(BUILD)/%.o,$(ARM64_HAL_C)) \
	$(patsubst %.S,$(BUILD)/%.o,$(ARM64_HAL_S))

#
# Kernel Drivers
#

ARM64_KERNEL_SOURCES := \
	$(KERN_SOURCES) \
	src/kern/platform/rpi4.c \
	src/drivers/disklabel/mbr.c \
	src/drivers/platform/rpi-common/rpi-sdhci.c\
	src/drivers/platform/rpi-common/rpi-console.c \
	src/drivers/platform/rpi-common/rpi-gpio.c \
	src/drivers/platform/rpi-common/rpi-mmc-irq.c \
	src/drivers/platform/rpi-common/rpi-sdio-host.c \
	src/drivers/platform/rpi-common/rpi-sdio.c \
	src/drivers/platform/rpi-common/rpi-gpio.c \
	src/drivers/platform/rpi4/rpi4-pcie.c \
	src/drivers/platform/rpi4/rpi4-board.c \
	src/drivers/platform/rpi4/rpi4-firmware.c \
	src/drivers/platform/rpi4/rpi4-vchiq.c \
	src/drivers/platform/rpi4/rpi4-vchiq-wire.c
	src/drivers/ethernet/cadence-gem.c \
	src/drivers/ethernet/mii-phy.c \
	src/drivers/pci/pci.c \
	src/drivers/pci/pci-brcmstb.c \
	src/drivers/mmc/sdhci.c \
	src/drivers/generic/loop.c \
	src/drivers/generic/fdt.c \
	src/drivers/generic/dma.c \
	src/drivers/generic/boot-fb.c \
	src/drivers/generic/fb-text.c \
	src/drivers/generic/console.c \
	src/drivers/generic/input.c \
	src/drivers/generic/system-device.c \
	src/drivers/generic/memory-device.c \

#
# Link
#

ARM64_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kernel/%.o,$(ARM64_KERNEL_SOURCES))

ARM64_VMUNIX_OBJS := $(ARM64_HAL_OBJS) $(ARM64_KERNEL_OBJS)

$(ARM64_VMUNIX_OBJS): $(ZEDBSD_PLATFORM_CONFIG_STAMP)
$(ARM64_VMUNIX_OBJS): $(ZEDBSD_KERNEL_LTO_STAMP)

#
# User programs read the C library headers from the target sysroot
# (toolchain/llvm/sysroot.mk).
#

ARM64_USER_CPPFLAGS := \
	-nostdinc \
	-isystem $(ZEDBSD_SYSROOT_ARM64)/usr/include \
	-Iinclude \
	-Isrc \
	-I. \
	-DHAL_ARCH_ARM64 \
	-DKERN_USER_ABI_AARCH64 \
	-DKERN_USER_ABI_LP64

ARM64_USER_CFLAGS := \
	-march=armv8-a \
	-mno-outline-atomics \
	-ffreestanding \
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

ARM64_USER_RUNTIME_OBJS := \
	$(ZEDBSD_SYSROOT_ARM64)/usr/lib/crt0.o \
	$(ZEDBSD_SYSROOT_ARM64)/usr/lib/libc.o

ARM64_USER_SH_SOURCES := $(USERLAND_sh_SOURCES)
ARM64_USER_SH_OBJS := $(patsubst %.c,$(BUILD)/user/%.o,$(ARM64_USER_SH_SOURCES))

ARM64_USER_READLINE_OBJ := $(BUILD)/user/userland/base/libedit/readline.o
ARM64_USER_READLINE_LIB := $(BUILD)/lib/libreadline.a
ARM64_USER_OBJS := $(ARM64_USER_SH_OBJS)

vmunix: $(BUILD)/vmunix

$(BUILD)/tests/rpi4-fdt-host-test: tests/rpi4-fdt-host-test.c src/hal/arm64/bsp-rpi4/fdt.c \
				   src/hal/arm64/fdt.h src/drivers/generic/fdt.c \
				   src/drivers/platform/rpi4/rpi4-board.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -Iinclude -Isrc \
		src/hal/arm64/bsp-rpi4/fdt.c src/drivers/generic/fdt.c \
		src/drivers/platform/rpi4/rpi4-board.c $< -o $@

rpi4-fdt-host-test: $(BUILD)/tests/rpi4-fdt-host-test $(RPI4_BOOT_FIRMWARE_DTB)
	$< $(RPI4_BOOT_FIRMWARE_DTB)

#
# Tests
#

CHECK_RUN_TARGETS += rpi4-fdt-host-test

$(BUILD)/tests/sdhci-under-test.c: src/drivers/mmc/sdhci.c
	@mkdir -p $(dir $@)
	sed -e '/^#include/d' $< > $@

$(BUILD)/tests/sdhci-host-test: tests/sdhci-host-test.c $(BUILD)/tests/sdhci-under-test.c include/drivers/mmc/sdhci.h
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -Iinclude -I$(BUILD)/tests $< -o $@

sdhci-host-test: $(BUILD)/tests/sdhci-host-test
	$<

CHECK_RUN_TARGETS += sdhci-host-test

CADENCE_GEM_HOST_TEST_DRIVERS := src/drivers/ethernet/cadence-gem.c \
	src/drivers/platform/rpi5/rp1-ethernet.c \
	src/drivers/ethernet/bcm54213pe.c \
	src/drivers/ethernet/mii-phy.c

$(BUILD)/tests/cadence-gem-host-test: tests/cadence-gem-host-test.c \
				      tests/cadence-gem-host-test.h \
				      $(CADENCE_GEM_HOST_TEST_DRIVERS) \
	include/drivers/ethernet/cadence-gem.h include/drivers/ethernet/mii-phy.h \
	include/drivers/ethernet/bcm54213pe.h src/drivers/platform/rpi5/rp1-ethernet.h
	@mkdir -p $(BUILD)/tests/cadence-gem
	for driver in $(CADENCE_GEM_HOST_TEST_DRIVERS); do \
		sed -e '/^#include/d' $$driver > $(BUILD)/tests/cadence-gem/$$(basename $$driver) || exit 1; \
	done
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -Iinclude -Isrc -Itests \
		-include tests/cadence-gem-host-test.h tests/cadence-gem-host-test.c \
		$(addprefix $(BUILD)/tests/cadence-gem/,$(notdir $(CADENCE_GEM_HOST_TEST_DRIVERS))) -o $@

cadence-gem-host-test: $(BUILD)/tests/cadence-gem-host-test
	$< -t > $(BUILD)/tests/cadence-gem-rp1.trace
	cmp tests/cadence-gem-rp1.trace $(BUILD)/tests/cadence-gem-rp1.trace
	$<

CHECK_RUN_TARGETS += cadence-gem-host-test

#
# Rules
#

$(BUILD)/src/hal/cpu-up.o: src/hal/cpu-up.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(ARM64_CFLAGS) $(ARM64_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/idle-suspend-unsupported.o: src/hal/idle-suspend-unsupported.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(ARM64_CFLAGS) $(ARM64_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/arm64/%.o: src/hal/arm64/%.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(ARM64_CFLAGS) $(ARM64_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/arm64/%.o: src/hal/arm64/%.S
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(ARM64_CFLAGS) -D_ASM_SRC_ -c $< -o $@

# libc carries one assembly source on this target, because clang has
# no setjmp builtin for AArch64.
$(BUILD)/src/libc/%.o: src/libc/%.S
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(ARM64_CFLAGS) -D_ASM_SRC_ -c $< -o $@

$(BUILD)/kernel/%.o: %.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(ARM64_CFLAGS) -fno-builtin -fno-strict-aliasing $(ARM64_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kernel/libc/%.o: src/libc/%.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(filter-out -mgeneral-regs-only,$(ARM64_CFLAGS)) -fno-builtin -fno-strict-aliasing $(ARM64_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/%.o: %.c $(ZEDBSD_SYSROOT_ARM64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_USER_CPPFLAGS) $(ARM64_USER_CFLAGS) -fno-strict-aliasing -MMD -MP -c $< -o $@

$(ARM64_USER_SH_OBJS) $(ARM64_USER_READLINE_OBJ): \
	ARM64_USER_CPPFLAGS += -Iuserland/base/libedit

$(ARM64_USER_READLINE_LIB): $(ARM64_USER_READLINE_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

ARM64_USER_CURSES_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,\
	$(BUILD)/user,curses)

$(BUILD)/lib/libcurses.a: $(ARM64_USER_CURSES_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

#
# The base programs are position-independent executables that load
# /lib/libc.so through /lib/ld.so. Their objects are the -fPIC ones
# built under $(BUILD)/dynamic/obj.  (The POSIX-R test ELF files stay
# static.)  They are linked by the compiler driver, which knows
# zedBSD's link on this target as on amd64 (the loader, the hash
# style, 4 KiB pages).
#

ARM64_APP_OBJ :=     $(BUILD)/dynamic/obj

ARM64_APP_INPUTS :=  $(ZEDBSD_SYSROOT_ARM64)/usr/lib/crt1.o \
		     $(BUILD)/dynamic/libc.so \
		     $(BUILD)/dynamic/ld.so \
		     tools/build/check-dynamic-elf.py

ARM64_APP_LINK =     $(CC) -nostdlib -pie -Wl,--no-relax -Wl,--gc-sections \
		     -Wl,--hash-style=sysv,-z,now,-z,relro,-z,separate-code \
		     -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
		     -Wl,--dynamic-linker=/lib/ld.so \
		     $(ZEDBSD_SYSROOT_ARM64)/usr/lib/crt1.o

ARM64_APP_LIBS =     -L$(BUILD)/dynamic -Wl,-rpath-link,$(BUILD)/dynamic -l:libc.so

ARM64_APP_CHECK =    $(PYTHON) tools/build/check-dynamic-elf.py --machine aarch64 --role application --needed libc.so

ARM64_APP_SH_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(ARM64_APP_OBJ),sh) \
		     $(ARM64_APP_OBJ)/userland/base/libedit/readline.o

$(ARM64_APP_SH_OBJS): DYNAMIC_CPPFLAGS += -Iuserland/base/libedit

$(BUILD)/bin/sh: $(ARM64_APP_INPUTS) $(ARM64_APP_SH_OBJS)
	@mkdir -p $(dir $@)
	$(ARM64_APP_LINK) $(ARM64_APP_SH_OBJS) $(ARM64_APP_LIBS) -o $@
	$(ARM64_APP_CHECK) $@

$(BUILD)/bin/sysctl: $(ARM64_APP_INPUTS) $(ARM64_APP_OBJ)/userland/base/sysctl/main.o
	@mkdir -p $(dir $@)
	$(ARM64_APP_LINK) $(ARM64_APP_OBJ)/userland/base/sysctl/main.o $(ARM64_APP_LIBS) -o $@
	$(ARM64_APP_CHECK) $@

$(BUILD)/bin/mount: $(ARM64_APP_INPUTS) $(ARM64_APP_OBJ)/userland/base/mount/main.o
	@mkdir -p $(dir $@)
	$(ARM64_APP_LINK) $(ARM64_APP_OBJ)/userland/base/mount/main.o $(ARM64_APP_LIBS) -o $@
	$(ARM64_APP_CHECK) $@

$(BUILD)/bin/umount: $(BUILD)/bin/mount
	@mkdir -p $(dir $@)
	cp -f $< $@

USER_BASIC_COMMANDS := $(filter $(ZEDBSD_USER_PROGRAMS),$(USERLAND_BASIC_PROGRAMS))
USER_BASIC_TARGETS := $(addprefix $(BUILD)/bin/,$(USER_BASIC_COMMANDS))
ARM64_USER_BASIC_COMMON_OBJ := $(BUILD)/user/userland/base/common/command.o $(BUILD)/user/userland/base/common/pager.o

define ARM64_USER_BASIC_COMMAND
$(BUILD)/bin/$(1): $(ARM64_APP_INPUTS) $(addprefix $(ARM64_APP_OBJ)/userland/base/common/,command.o pager.o) \
	$(call ZEDBSD_USERLAND_OBJECTS,$(ARM64_APP_OBJ),$(1))
	@mkdir -p $$(dir $$@)
	$(ARM64_APP_LINK) $(addprefix $(ARM64_APP_OBJ)/userland/base/common/,command.o pager.o) \
		$(call ZEDBSD_USERLAND_OBJECTS,$(ARM64_APP_OBJ),$(1)) $(ARM64_APP_LIBS) -o $$@
	$(ARM64_APP_CHECK) $$@
endef

ARM64_USER_NET_COMMANDS := $(USERLAND_SELECTED_NETWORK_PROGRAMS)
ARM64_USER_NET_COMMON_OBJS := $(BUILD)/user/userland/base/net/netutil.o $(BUILD)/user/userland/base/net/dhcp.o

define ARM64_USER_NET_COMMAND
$(BUILD)/bin/$(1): $(ARM64_APP_INPUTS) $(addprefix $(ARM64_APP_OBJ)/userland/base/net/,netutil.o dhcp.o) \
	$(call ZEDBSD_USERLAND_OBJECTS,$(ARM64_APP_OBJ),$(1))
	@mkdir -p $$(dir $$@)
	$(ARM64_APP_LINK) $(addprefix $(ARM64_APP_OBJ)/userland/base/net/,netutil.o dhcp.o) \
 $(call ZEDBSD_USERLAND_OBJECTS,$(ARM64_APP_OBJ),$(1)) $(ARM64_APP_LIBS) -o $$@
	$(ARM64_APP_CHECK) $$@
endef

$(foreach command,$(ARM64_USER_NET_COMMANDS),\
	$(eval $(call ARM64_USER_NET_COMMAND,$(command))))

$(BUILD)/POSIX-R1.ELF: $(ARM64_USER_RUNTIME_OBJS) \
		       $(BUILD)/user/userland/tests/syscall-smoke.o \
		       $(ARM64_PLATFORM)/user.ld tools/build/check-user-elf.py
	$(ARM64_LD) --gc-sections -nostdlib -static -z max-page-size=4096 \
		-z stack-size=0x100000 -T $(ARM64_PLATFORM)/user.ld \
		 $(ARM64_USER_RUNTIME_OBJS) \
		 $(BUILD)/user/userland/tests/syscall-smoke.o -o $@
	@# A weak undefined symbol is an optional hook, not a link error.
	@# GNU nm drops those from -u; llvm-nm reports them, so the strong
	@# references are selected explicitly.
	@test -z "$$($(ARM64_NM) -u $@ | $(AWK) '$$1 == "U"')" || \
		{ $(ARM64_NM) -u $@; exit 1; }
	$(PYTHON) tools/build/check-user-elf.py --machine aarch64 $@

$(BUILD)/POSIX-R2.ELF: $(ARM64_USER_RUNTIME_OBJS) \
		       $(BUILD)/user/userland/tests/posix-r2.o \
		       $(ARM64_PLATFORM)/user.ld tools/build/check-user-elf.py
	$(ARM64_LD) --gc-sections -nostdlib -static -z max-page-size=4096 \
		-z stack-size=0x100000 -T $(ARM64_PLATFORM)/user.ld \
		$(ARM64_USER_RUNTIME_OBJS) \
		$(BUILD)/user/userland/tests/posix-r2.o -o $@
	@# A weak undefined symbol is an optional hook, not a link error.
	@# GNU nm drops those from -u; llvm-nm reports them, so the strong
	@# references are selected explicitly.
	@test -z "$$($(ARM64_NM) -u $@ | $(AWK) '$$1 == "U"')" || \
		{ $(ARM64_NM) -u $@; exit 1; }
	$(PYTHON) tools/build/check-user-elf.py --machine aarch64 $@

$(BUILD)/POSIX-R2-REMAINING.ELF: $(ARM64_USER_RUNTIME_OBJS) \
				 $(BUILD)/user/userland/tests/posix-r2-remaining.o \
				 $(ARM64_PLATFORM)/user.ld tools/build/check-user-elf.py
	$(ARM64_LD) --gc-sections -nostdlib -static -z max-page-size=4096 \
		-z stack-size=0x100000 -T $(ARM64_PLATFORM)/user.ld \
		$(ARM64_USER_RUNTIME_OBJS) \
		$(BUILD)/user/userland/tests/posix-r2-remaining.o -o $@
	@# A weak undefined symbol is an optional hook, not a link error.
	@# GNU nm drops those from -u; llvm-nm reports them, so the strong
	@# references are selected explicitly.
	@test -z "$$($(ARM64_NM) -u $@ | $(AWK) '$$1 == "U"')" || \
		{ $(ARM64_NM) -u $@; exit 1; }
	$(PYTHON) tools/build/check-user-elf.py --machine aarch64 $@

# Sandbox helpers keep their registered static class on arm64 as on amd64.
USER_STATIC_COMMANDS := $(foreach program,$(filter $(ZEDBSD_USER_PROGRAMS),$(USERLAND_PACKAGES)),\
			$(if $(filter static,$(USERLAND_$(program)_CLASS)),$(program)))

USER_STATIC_TARGETS := $(addprefix $(BUILD)/bin/,$(USER_STATIC_COMMANDS))

# The sandboxed preview includes math and number parsing without loading a DSO.
ARM64_USER_FLOAT_OBJS := $(patsubst src/libc/%.c,$(BUILD)/user-float/%.o,\
			 $(ZEDBSD_LIBM_SOURCES) src/libc/softfloat.c src/libc/compiler-runtime.c \
			 src/libc/softfloat128.c src/libc/compiler-runtime128.c src/libc/float-parse.c)

ARM64_USER_STATIC_EXTRA_keiland-preview := $(ARM64_USER_FLOAT_OBJS)

$(ARM64_USER_FLOAT_OBJS): $(BUILD)/user-float/%.o: src/libc/%.c \
			  $(ZEDBSD_LIBM_HEADERS) src/libc/softfloat.h src/libc/softfloat128.h \
			  $(ZEDBSD_SYSROOT_ARM64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_USER_CPPFLAGS) $(ARM64_USER_CFLAGS) -c $< -o $@

define ARM64_USER_STATIC_COMMAND
$(BUILD)/bin/$(1): $(ARM64_USER_RUNTIME_OBJS) \
		   $(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user,$(1)) \
		   $(ARM64_USER_STATIC_EXTRA_$(1)) \
		   $(ZEDBSD_SYSROOT_ARM64)/usr/lib/libclang_rt.builtins.a \
		   $(ARM64_PLATFORM)/user.ld tools/build/check-user-elf.py
	@mkdir -p $$(dir $$@)
	$(ARM64_LD) --gc-sections -nostdlib -static -z max-page-size=4096 \
		-z stack-size=0x100000 -T $(ARM64_PLATFORM)/user.ld \
		$(ARM64_USER_RUNTIME_OBJS) \
		$(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user,$(1)) \
		$(ARM64_USER_STATIC_EXTRA_$(1)) \
		$(ZEDBSD_SYSROOT_ARM64)/usr/lib/libclang_rt.builtins.a -o $$@
	$(PYTHON) tools/build/check-user-elf.py --machine aarch64 $$@
endef

$(foreach command,$(USER_STATIC_COMMANDS), $(eval $(call ARM64_USER_STATIC_COMMAND,$(command))))
# No substitute-font file can be opened by the sandboxed preview helper.
$(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user,keiland-preview): ARM64_USER_CPPFLAGS += -DPDF_FONT_FILES=0

#
# ELF64 runtime linker and shared libc for the aarch64 architecture overlay.
#

DYNAMIC_DIR := $(BUILD)/dynamic

DYNAMIC_CPPFLAGS := -nostdinc -I. -Iinclude \
	-isystem $(ZEDBSD_SYSROOT_ARM64)/usr/include \
	-DHAL_ARCH_ARM64 -DKERN_USER_ABI_AARCH64 -DKERN_USER_ABI_LP64 \
	-DKERN_DYNAMIC_LIBC

DYNAMIC_CFLAGS := -march=armv8-a -mno-outline-atomics -Os -ffreestanding \
	-fPIC -fno-builtin -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
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
	$(DYNAMIC_LIBC_SOURCES)) $(DYNAMIC_DIR)/obj/userland/base/libc/syscall.o \
	$(DYNAMIC_DIR)/obj/src/libc/setjmp-aarch64.o

DYNAMIC_RTLD_OBJS := $(DYNAMIC_DIR)/obj/src/rtld/entry.o \
	$(DYNAMIC_DIR)/obj/src/rtld/tlsdesc.o \
	$(DYNAMIC_DIR)/obj/src/rtld/rtld.o \
	$(DYNAMIC_DIR)/obj/src/rtld/string.o

DYNAMIC_FLOAT_DIR := $(DYNAMIC_DIR)/float

DYNAMIC_LIBM_OBJS := $(patsubst src/libc/math/%.c,\
	$(DYNAMIC_FLOAT_DIR)/math/%.o,$(ZEDBSD_LIBM_SOURCES))

DYNAMIC_FLOAT_PARSE_OBJS := $(DYNAMIC_FLOAT_DIR)/softfloat.o \
	$(DYNAMIC_FLOAT_DIR)/compiler-runtime.o \
	$(DYNAMIC_FLOAT_DIR)/softfloat128.o \
	$(DYNAMIC_FLOAT_DIR)/compiler-runtime128.o \
	$(DYNAMIC_FLOAT_DIR)/float-parse.o

DYNAMIC_LIBC_OBJS += $(DYNAMIC_LIBM_OBJS) $(DYNAMIC_FLOAT_PARSE_OBJS)

$(DYNAMIC_DIR)/obj/%.o: %.c $(ZEDBSD_SYSROOT_ARM64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(DYNAMIC_CPPFLAGS) $(DYNAMIC_CFLAGS) -MMD -MP -c $< -o $@

$(DYNAMIC_DIR)/obj/userland/base/libc/syscall.o: \
	userland/base/libc/syscall-aarch64.S include/hal/arch.h \
	include/hal/arch/aarch64.h
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(DYNAMIC_CPPFLAGS) -c $< -o $@

$(DYNAMIC_DIR)/obj/src/libc/setjmp-aarch64.o: src/libc/setjmp-aarch64.S
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(DYNAMIC_CPPFLAGS) -D_ASM_SRC_ -c $< -o $@

$(DYNAMIC_DIR)/obj/src/rtld/entry.o: src/rtld/entry-aarch64.S
	@mkdir -p $(dir $@)
	$(ARM64_CC) -c $< -o $@

$(DYNAMIC_DIR)/obj/src/rtld/tlsdesc.o: src/rtld/tlsdesc-arm64.S
	@mkdir -p $(dir $@)
	$(ARM64_CC) -c $< -o $@

$(DYNAMIC_LIBM_OBJS): $(DYNAMIC_FLOAT_DIR)/math/%.o: src/libc/math/%.c \
	$(ZEDBSD_LIBM_HEADERS) $(ZEDBSD_SYSROOT_ARM64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(DYNAMIC_CPPFLAGS) \
 $(DYNAMIC_CFLAGS) -c $< -o $@

$(DYNAMIC_FLOAT_DIR)/softfloat.o: src/libc/softfloat.c src/libc/softfloat.h \
	$(ZEDBSD_SYSROOT_ARM64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(DYNAMIC_CPPFLAGS) \
 $(DYNAMIC_CFLAGS) -c $< -o $@

$(DYNAMIC_FLOAT_DIR)/compiler-runtime.o: src/libc/compiler-runtime.c src/libc/softfloat.h \
	$(ZEDBSD_SYSROOT_ARM64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(DYNAMIC_CPPFLAGS) \
 $(DYNAMIC_CFLAGS) -c $< -o $@

$(DYNAMIC_FLOAT_DIR)/softfloat128.o: src/libc/softfloat128.c \
	src/libc/softfloat128.h src/libc/softfloat.h \
	$(ZEDBSD_SYSROOT_ARM64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(DYNAMIC_CPPFLAGS) \
 $(DYNAMIC_CFLAGS) -c $< -o $@

$(DYNAMIC_FLOAT_DIR)/compiler-runtime128.o: src/libc/compiler-runtime128.c \
	src/libc/softfloat128.h src/libc/softfloat.h \
	$(ZEDBSD_SYSROOT_ARM64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(DYNAMIC_CPPFLAGS) \
 $(DYNAMIC_CFLAGS) -c $< -o $@

$(DYNAMIC_FLOAT_DIR)/float-parse.o: src/libc/float-parse.c src/libc/softfloat.h \
	$(ZEDBSD_SYSROOT_ARM64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(DYNAMIC_CPPFLAGS) \
 $(DYNAMIC_CFLAGS) -c $< -o $@

$(DYNAMIC_DIR)/ld.so: $(DYNAMIC_RTLD_OBJS)
	$(ARM64_LD) -shared -Bsymbolic -e _rtld_start --hash-style=sysv \
 -z now -z relro -z separate-code -z max-page-size=4096 $^ -o $@

$(DYNAMIC_DIR)/libc.so: $(DYNAMIC_LIBC_OBJS)
	$(ARM64_LD) -shared -soname libc.so --hash-style=both -z now -z relro \
 -z separate-code -z max-page-size=4096 -z stack-size=0x100000 \
 --allow-shlib-undefined $^ -o $@

#
# Portable libraries and applications use the same graph as amd64.
#

DYNAMIC_SYSROOT := $(ZEDBSD_SYSROOT_ARM64)
DYNAMIC_LINK_CFLAGS := -march=armv8-a -mno-outline-atomics
DYNAMIC_LINK_LDFLAGS := -m aarch64elf -z max-page-size=4096
DYNAMIC_ELF_MACHINE := aarch64
include platform/common/userland-dynamic.mk

$(foreach command,$(filter-out $(DYNAMIC_PORTABLE_PROGRAMS),$(USER_BASIC_COMMANDS)),\
	$(eval $(call ARM64_USER_BASIC_COMMAND,$(command))))

$(DYNAMIC_DIR)/alt/rpathdep.so: \
	$(DYNAMIC_DIR)/obj/userland/tests/rpathdep.o $(DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(ARM64_LD) -shared -soname rpathdep.so --hash-style=gnu \
 -z now -z relro -z separate-code -z max-page-size=4096 $< -o $@

$(DYNAMIC_DIR)/tlstest.so: $(DYNAMIC_DIR)/obj/userland/tests/tlstest.o \
	$(DYNAMIC_DIR)/alt/rpathdep.so $(DYNAMIC_DIR)/ld.so
	$(ARM64_LD) -shared -soname tlstest.so --hash-style=gnu \
 -z now -z relro -z separate-code -z max-page-size=4096 \
 --enable-new-dtags -rpath '$$ORIGIN/alt' \
 $(DYNAMIC_DIR)/obj/userland/tests/tlstest.o \
 -L$(DYNAMIC_DIR)/alt -l:rpathdep.so -o $@

$(DYNAMIC_DIR)/rpathtest.so: \
	$(DYNAMIC_DIR)/obj/userland/tests/rpathtest.o \
	$(DYNAMIC_DIR)/alt/rpathdep.so $(DYNAMIC_DIR)/ld.so
	$(ARM64_LD) -shared -soname rpthtest.so --hash-style=gnu \
 -z now -z relro -z separate-code -z max-page-size=4096 \
 --disable-new-dtags -rpath '$$ORIGIN/alt' $< \
 -L$(DYNAMIC_DIR)/alt -l:rpathdep.so -o $@

$(DYNAMIC_DIR)/verstest.so: \
	$(DYNAMIC_DIR)/obj/userland/tests/versiontest.o \
	userland/tests/versiontest.map $(DYNAMIC_DIR)/ld.so
	$(ARM64_LD) -shared -soname verstest.so --hash-style=gnu \
 -z now -z relro -z separate-code -z max-page-size=4096 \
 --version-script=userland/tests/versiontest.map $< -o $@

$(DYNAMIC_DIR)/versuse.so: \
	$(DYNAMIC_DIR)/obj/userland/tests/versionuse.o \
	$(DYNAMIC_DIR)/verstest.so $(DYNAMIC_DIR)/ld.so
	$(ARM64_LD) -shared -soname versuse.so --hash-style=gnu \
 -z now -z relro -z separate-code -z max-page-size=4096 \
 $< -L$(DYNAMIC_DIR) -l:verstest.so -o $@

$(DYNAMIC_DIR)/dyntest: $(ZEDBSD_SYSROOT_ARM64)/usr/lib/crt1.o \
	$(DYNAMIC_DIR)/obj/userland/tests/dyntest.o $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/ld.so $(DYNAMIC_DIR)/tlstest.so \
	$(DYNAMIC_DIR)/versuse.so
	$(CC) -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=sysv,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_ARM64)/usr/lib/crt1.o \
 $(DYNAMIC_DIR)/obj/userland/tests/dyntest.o \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libc.so -o $@

dynamic-userland-check: $(DYNAMIC_DIR)/ld.so $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/dyntest $(DYNAMIC_DIR)/tlstest.so \
	$(DYNAMIC_DIR)/rpathtest.so $(DYNAMIC_DIR)/verstest.so \
	$(DYNAMIC_DIR)/versuse.so tools/build/check-dynamic-elf.py
	$(PYTHON) tools/build/check-dynamic-elf.py --machine aarch64 --role interpreter $(DYNAMIC_DIR)/ld.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine aarch64 --role libc $(DYNAMIC_DIR)/libc.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine aarch64 --role module $(DYNAMIC_DIR)/tlstest.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine aarch64 --role rpath-module $(DYNAMIC_DIR)/rpathtest.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine aarch64 --role version-definition $(DYNAMIC_DIR)/verstest.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine aarch64 --role version-consumer $(DYNAMIC_DIR)/versuse.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine aarch64 --role program $(DYNAMIC_DIR)/dyntest
	@echo "zedBSD aarch64 dynamic userland artifacts: PASS"

.PHONY: dynamic-userland-check

include platform/common/userland-rootfs.mk

AARCH64_ARCH_INPUTS := $(ZEDBSD_ROOTFS_INPUTS)
AARCH64_ARCH_FILES := $(ZEDBSD_ROOTFS_FILES)

$(eval $(call ZEDBSD_ROOTFS_TREE_RULE,aarch64,$(AARCH64_ARCH_INPUTS),$(AARCH64_ARCH_FILES)))
AARCH64_ARCH_UFS_IMAGE := $(ZEDBSD_ROOTFS_IMAGE_DIR)/aarch64.ufs
$(eval $(call ZEDBSD_ROOTFS_UFS_IMAGE_RULE,$(AARCH64_ARCH_UFS_IMAGE),aarch64))

rootfs: $(BUILD)/rootfs/.stamp

$(BUILD)/ufs-root.img: $(AARCH64_ARCH_UFS_IMAGE) \
	tools/build/make-ufs-root-image.py tools/build/ufs_format.py
	$(PYTHON) tools/build/make-ufs-root-image.py --force \
 --arch-profile aarch64 --arch-image $(AARCH64_ARCH_UFS_IMAGE) $@

# Firmware forwards this one-line file through /chosen/bootargs.  sessiond
# reads login= through kern.boot.login, independently of splash/quiet boot.
# Without the kernel's messages on the screen (ZEDBSD_BOOT_KERNEL_MESSAGES n)
# the line carries kmsg=quiet: the records stay in dmesg only.
# The Pi 5's line also starts its GPU at boot (rpi5gpu.start=boot), so the
# graphical login and a desktop started from the text console find the
# display and render nodes; without it the GPU waits for root's
# sysctl hw.gpu.start=1.
# Recheck the value on every invocation; retain the mtime when unchanged so
# toggling the option in either direction rebuilds only the necessary image.
.PHONY: rpi4-boot-command-line
rpi4-boot-command-line:

RPI_BOOT_COMMAND_LINE := login=$(if $(filter y,$(ZEDBSD_GRAPHICAL_LOGIN)),graphical,console)$(if $(filter n,$(ZEDBSD_BOOT_KERNEL_MESSAGES)), kmsg=quiet)$(if $(filter rpi5,$(ZEDBSD_BOARD)), rpi5gpu.start=boot)

$(BUILD)/cmdline.txt: rpi4-boot-command-line
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RPI_BOOT_COMMAND_LINE)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

# The firmware's config.txt (ws215-p003): the source as it is, or, with the onboard
# Bluetooth driver and without the serial console, without the lines that give the
# PL011 to the console.  Rechecked on every invocation like cmdline.txt, so a change
# of either option rebuilds the image.
.PHONY: rpi4-config-file
rpi4-config-file:

ifeq ($(ZEDBSD_BOARD),rpi5)
# The Pi 5's config.txt is its source as it is: the debug UART has pins of its
# own, so no line moves between the console and the Bluetooth controller.
$(BUILD)/config.txt: rpi4-config-file $(ARM64_PLATFORM)/config-rpi5.txt
	@mkdir -p $(dir $@)
	@cp $(ARM64_PLATFORM)/config-rpi5.txt $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi
else
$(BUILD)/config.txt: rpi4-config-file $(ARM64_PLATFORM)/config.txt platform/arm64/tools/make-rpi4-config.py
	@mkdir -p $(dir $@)
	@$(PYTHON) platform/arm64/tools/make-rpi4-config.py --source $(ARM64_PLATFORM)/config.txt \
 --bluetooth $(CONFIG_DRIVER_BCM2711_BT) --serial $(CONFIG_RPI4_SERIAL_CONSOLE) --output $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi
endif

# The SD card: a FAT boot partition with the firmware and the kernel, and
# the UFS root that the kernel's legacy autoroot finds beside it.  The
# ODROID-C5's card is laid out by platform/arm64/odroid-c5.mk.
ifneq ($(ZEDBSD_BOARD),odroid-c5)
$(BUILD)/hdd-image.img: $(BUILD)/vmunix $(BUILD)/ufs-root.img \
	$(DATA_IMAGE) $(SWAP_IMAGE) $(BUILD)/config.txt $(BUILD)/cmdline.txt \
	$(ARM64_BOOT_FIRMWARE_CACHE_FILES) \
	platform/arm64/tools/make-rpi4-ufs-root-hdd-image.py \
	platform/arm64/tools/make-rpi4-hdd-image.py \
	platform/arm64/tools/check-rpi4-hdd-image.py tools/build/check-ufs-image.py
	$(PYTHON) platform/arm64/tools/make-rpi4-ufs-root-hdd-image.py --force \
 --kernel $(BUILD)/vmunix --ufs-root $(BUILD)/ufs-root.img \
 --data-image $(DATA_IMAGE) --swapfile $(SWAP_IMAGE) \
 --config $(BUILD)/config.txt --cmdline $(BUILD)/cmdline.txt \
 --board $(ZEDBSD_BOARD) --firmware-dir $(ARM64_BOOT_FIRMWARE_DIR) $@
endif

$(BUILD)/kernel.elf: $(ARM64_VMUNIX_OBJS) $(ARM64_PLATFORM)/vmunix.ld \
	platform/arm64/tools/check-arm64-vmunix.py
	$(ARM64_LD) --gc-sections -z max-page-size=4096 \
 -T $(ARM64_PLATFORM)/vmunix.ld -nostdlib $(ARM64_VMUNIX_OBJS) -o $@
	@# A weak undefined symbol is an optional hook, not a link error.
	@# GNU nm drops those from -u; llvm-nm reports them, so the strong
	@# references are selected explicitly.
	@test -z "$$($(ARM64_NM) -u $@ | $(AWK) '$$1 == "U"')" || \
		{ $(ARM64_NM) -u $@; exit 1; }
	$(PYTHON) platform/arm64/tools/check-arm64-vmunix.py --elf $@

$(BUILD)/vmunix: $(BUILD)/kernel.elf \
	platform/arm64/tools/check-arm64-vmunix.py
	$(ARM64_OBJCOPY) -O binary $< $@
	$(PYTHON) platform/arm64/tools/check-arm64-vmunix.py --elf $< --image $@

.PHONY: rpi4-fdt-host-test sdhci-host-test cadence-gem-host-test

-include $(ARM64_HAL_OBJS:.o=.d) $(ARM64_KERNEL_OBJS:.o=.d)

-include $(ARM64_USER_OBJS:.o=.d)
-include $(wildcard $(DYNAMIC_DIR)/obj/*/*/*.d $(DYNAMIC_DIR)/obj/*/*/*/*.d $(DYNAMIC_DIR)/obj/*/*/*/*/*.d)

$(BUILD)/src/hal/pmem-constraints.o: src/hal/pmem-constraints.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(ARM64_CFLAGS) -MMD -MP -c $< -o $@
