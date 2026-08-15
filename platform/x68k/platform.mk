# zedBSD MC68030/X68000 build rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

M68K_CC ?= m68k-linux-gnu-gcc
M68K_LD ?= m68k-linux-gnu-ld
M68K_NM ?= m68k-linux-gnu-nm
M68K_OBJDUMP ?= m68k-linux-gnu-objdump
M68K_OBJCOPY ?= m68k-linux-gnu-objcopy
X68K_PLATFORM := platform/x68k

M68K_CPPFLAGS := -nostdinc -Iinclude -Iinclude/uapi -Isrc -I. \
	-Ilibc/include -Isrc/hal/m68k -DHAL_ARCH_M68K -DHAL_BOARD_X68K \
	-DZEDBSD_USER_ABI_M68K -DZEDBSD_PAGE_SIZE=4096 \
	-DZEDBSD_USER_PAGE_SIZE=4096 -DZEDBSD_NO_PRINTF_FLOAT \
	-DZEDBSD_INIT_PATH='"/x68k/bin/sh"'
M68K_KERNEL_CFLAGS := -m68030 -msoft-float -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-common -ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror
M68K_USER_CFLAGS := -m68030 -msoft-float -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-builtin -fno-common -ffunction-sections -fdata-sections \
	-Os -Wall -Wextra -Werror
M68K_USER_CPPFLAGS := -nostdinc -Iinclude -Iinclude/uapi -Isrc -I. \
	-Ilibc/include -DZEDBSD_USER_ABI_M68K -DZEDBSD_USER_PAGE_SIZE=4096 \
	-DZEDBSD_NO_PRINTF_FLOAT

# These overrides make an explicit `libc-objects` request use the m68k kernel
# contract.  X68k user libc is defined separately when the user image lands.
ZEDBSD_LIBC_CC := $(M68K_CC)
ZEDBSD_LIBC_NM := $(M68K_NM)
ZEDBSD_LIBC_OBJDUMP := $(M68K_OBJDUMP)
ZEDBSD_LIBC_CPPFLAGS := $(M68K_CPPFLAGS)
ZEDBSD_LIBC_CFLAGS := $(M68K_KERNEL_CFLAGS) -fno-builtin \
	-fno-isolate-erroneous-paths-dereference -fno-strict-aliasing

# The generic softfloat host executable links target libc/softfloat objects.
# That is runnable for the x86 targets but not for big-endian m68k.  Its pure
# host coverage remains in the PC targets; X68k runs the architecture-neutral
# host tests and is fixed to the soft-float ABI.
CHECK_RUN_TARGETS := $(filter-out softfloat-host-test,$(CHECK_RUN_TARGETS))
CHECK_RUN_TARGETS += x68k-user-abi-check x68k-target-audit \
	x68k-emulator-rom-host-test

x68k-emulator-rom-host-test:
	$(PYTHON) tests/x68k-emulator-rom-host-test.py

X68K_CONTRACT_OBJ := $(BUILD)/src/hal/m68k/bsp-x68k/contract.o
X68K_USER_CONTRACT_OBJ := $(BUILD)/user/userland/x68k-contract.o
X68K_CRT0_OBJ := $(BUILD)/user/userland/crt0-m68k.o
X68K_USER_RUNTIME_SOURCES := \
	userland/libc/posix.c userland/libc/socket.c userland/libc/signal.c \
	libc/heap.c libc/string.c libc/ctype.c libc/int64.c libc/strto.c \
	libc/format.c libc/stdio.c
X68K_USER_SH_SOURCES := userland/sh/main.c userland/sh/applet.c \
	userland/sh/builtins.c
X68K_USER_RUNTIME_OBJS := \
	$(patsubst %.c,$(BUILD)/user/%.o,$(X68K_USER_RUNTIME_SOURCES))
X68K_USER_SH_OBJS := \
	$(patsubst %.c,$(BUILD)/user/%.o,$(X68K_USER_SH_SOURCES))
X68K_USER_OBJS := $(X68K_CRT0_OBJ) $(X68K_USER_RUNTIME_OBJS) \
	$(X68K_USER_SH_OBJS)
X68K_STAGE1_OBJ := $(BUILD)/boot/x68k/stage1.o
X68K_STAGE2_OBJS := $(BUILD)/boot/x68k/stage2-start.o \
	$(BUILD)/boot/x68k/iocs.o $(BUILD)/boot/x68k/stage2.o \
	$(BUILD)/boot/x68k/mb89352.o
X68K_EARLY_C_SOURCES := \
	src/hal/m68k/runtime.c \
	src/hal/m68k/cache.c \
	src/hal/m68k/exception.c \
	src/hal/m68k/io.c \
	src/hal/m68k/space.c \
	src/hal/m68k/task.c \
	src/hal/m68k/trap.c \
	src/hal/m68k/bsp-x68k/boot.c \
	src/hal/m68k/bsp-x68k/cmain.c \
	src/hal/m68k/bsp-x68k/console.c \
	src/hal/m68k/bsp-x68k/keyboard.c \
	src/hal/m68k/bsp-x68k/keyboard-map.c \
	src/hal/m68k/bsp-x68k/machine.c \
	src/hal/m68k/bsp-x68k/handoff.c \
	src/hal/m68k/bsp-x68k/irq.c \
	src/hal/m68k/bsp-x68k/memory-map.c \
	src/hal/m68k/bsp-x68k/pmem.c \
	src/hal/m68k/bsp-x68k/scsi.c \
	src/hal/m68k/bsp-x68k/timer.c
X68K_EARLY_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(X68K_EARLY_C_SOURCES)) \
	$(BUILD)/src/hal/m68k/bsp-x68k/contract.o \
	$(BUILD)/src/hal/m68k/cache030.o \
	$(BUILD)/src/hal/m68k/irq030.o \
	$(BUILD)/src/hal/m68k/mmu030-asm.o \
	$(BUILD)/src/hal/m68k/bsp-x68k/reboot.o \
	$(BUILD)/src/hal/m68k/task-asm.o \
	$(BUILD)/src/hal/m68k/vectors.o

X68K_KERNEL_SOURCES := \
	src/kern/main.c src/kern/env.c src/kern/fs.c src/kern/namespace.c \
	src/kern/fat.c src/kern/fat-lfn.c src/kern/fat16.c src/kern/fat-vfs.c \
	src/kern/inode.c src/kern/file.c src/kern/namecache.c src/kern/namei.c \
	src/kern/mount.c src/kern/rootfs.c src/kern/vfs.c src/kern/swap.c \
	src/kern/swap-fat.c src/kern/vm-reclaim.c src/kern/disk.c \
	src/kern/partition.c src/kern/x68k/partition.c src/kern/x68k/platform.c \
	drivers/x68k-mb89352.c drivers/x68k-spc-disk.c \
	src/kern/image.c src/kern/panic.c src/kern/entry.c src/kern/clock.c \
	src/kern/process.c src/kern/thread.c src/kern/sched.c src/kern/vmspace.c \
	src/kern/vm-object.c src/kern/vm-commit.c src/kern/filedesc.c \
	src/kern/pipe.c src/kern/cred.c src/kern/signal.c src/kern/cwdinfo.c \
	src/kern/elf.c src/kern/exec.c src/kern/user-probe.c \
	src/kern/syscall.c src/kern/uaccess.c src/kern/cdev.c src/kern/devfs.c \
	src/kern/console-device.c src/kern/graphics-device.c \
	src/kern/system-device.c src/kern/x68k/unsupported-devices.c \
	src/kern/init.c
X68K_KERNEL_SOURCES += $(KERN_NET_SOURCES)
X68K_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kernel/%.o,$(X68K_KERNEL_SOURCES))
X68K_KERNEL_LIBC_OBJS := $(patsubst %.c,$(BUILD)/kernel/%.o,$(ZEDBSD_LIBC_SOURCES))
X68K_VMUNIX_OBJS := $(X68K_EARLY_OBJS) $(X68K_KERNEL_OBJS) \
	$(X68K_KERNEL_LIBC_OBJS)
X68K_AUDIT_C_SOURCES := \
	src/hal/m68k/cache.c \
	src/hal/m68k/exception.c \
	src/hal/m68k/io.c \
	src/hal/m68k/space.c \
	src/hal/m68k/task.c \
	src/hal/m68k/trap.c \
	src/hal/m68k/bsp-x68k/boot.c \
	src/hal/m68k/bsp-x68k/cmain.c \
	src/hal/m68k/bsp-x68k/console.c \
	src/hal/m68k/bsp-x68k/keyboard.c \
	src/hal/m68k/bsp-x68k/keyboard-map.c \
	src/hal/m68k/bsp-x68k/machine.c \
	src/hal/m68k/bsp-x68k/handoff.c \
	src/hal/m68k/bsp-x68k/irq.c \
	src/hal/m68k/bsp-x68k/memory-map.c \
	src/hal/m68k/bsp-x68k/pmem.c \
	src/hal/m68k/bsp-x68k/scsi.c \
	src/hal/m68k/bsp-x68k/timer.c \
	src/kern/x68k/platform.c \
	src/kern/x68k/partition.c \
	drivers/x68k-mb89352.c \
	drivers/x68k-spc-disk.c
X68K_AUDIT_S_SOURCES := \
	src/hal/m68k/cache030.S \
	src/hal/m68k/irq030.S \
	src/hal/m68k/mmu030-asm.S \
	src/hal/m68k/bsp-x68k/reboot.S \
	src/hal/m68k/task.S \
	src/hal/m68k/vectors.S
X68K_AUDIT_C_OBJS := $(patsubst %.c,$(BUILD)/target-audit/%.o,\
	$(X68K_AUDIT_C_SOURCES))
X68K_AUDIT_S_OBJS := $(patsubst %.S,$(BUILD)/target-audit/%.o,\
	$(X68K_AUDIT_S_SOURCES))

all: $(BUILD)/vmunix $(BUILD)/bin/sh $(BUILD)/contract-user.elf \
	x68k-boot-image
vmunix: $(BUILD)/vmunix
SH: $(BUILD)/bin/sh
x68k-contract: $(BUILD)/vmunix $(BUILD)/contract-user.elf

$(BUILD)/src/hal/m68k/%.o: src/hal/m68k/%.c
	@mkdir -p $(dir $@)
	$(M68K_CC) $(M68K_CPPFLAGS) $(M68K_KERNEL_CFLAGS) -fno-builtin \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/src/hal/m68k/%.o: src/hal/m68k/%.S
	@mkdir -p $(dir $@)
	$(M68K_CC) $(M68K_CPPFLAGS) $(M68K_KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/src/hal/m68k/task-asm.o: src/hal/m68k/task.S
	@mkdir -p $(dir $@)
	$(M68K_CC) $(M68K_CPPFLAGS) $(M68K_KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/%.o: %.c
	@mkdir -p $(dir $@)
	$(M68K_CC) $(M68K_CPPFLAGS) $(M68K_KERNEL_CFLAGS) -fno-builtin \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/kernel/libc/%.o: libc/%.c
	@mkdir -p $(dir $@)
	$(M68K_CC) $(M68K_CPPFLAGS) $(M68K_KERNEL_CFLAGS) -fno-builtin \
		-fno-isolate-erroneous-paths-dereference -fno-strict-aliasing \
		-MMD -MP -c $< -o $@

$(BUILD)/target-audit/%.o: %.c
	@mkdir -p $(dir $@)
	$(M68K_CC) $(M68K_CPPFLAGS) $(M68K_KERNEL_CFLAGS) -fno-builtin \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/target-audit/%.o: %.S
	@mkdir -p $(dir $@)
	$(M68K_CC) $(M68K_CPPFLAGS) $(M68K_KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/user/%.o: %.c
	@mkdir -p $(dir $@)
	$(M68K_CC) $(M68K_USER_CPPFLAGS) $(M68K_USER_CFLAGS) \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

x68k-target-audit: $(X68K_AUDIT_C_OBJS) $(X68K_AUDIT_S_OBJS) \
	$(X68K_CRT0_OBJ) $(X68K_USER_CONTRACT_OBJ)
	@if $(M68K_OBJDUMP) -d --no-show-raw-insn $(X68K_AUDIT_C_OBJS) | \
		grep -E '^[[:space:]]*[0-9a-f]+:[[:space:]]+f[a-z]'; then \
		echo "ERROR: m68k soft-float C object contains FPU instruction" >&2; \
		exit 1; \
	fi
	@if $(M68K_OBJDUMP) -d --no-show-raw-insn \
		$(X68K_CRT0_OBJ) $(X68K_USER_CONTRACT_OBJ) | \
		grep -E '^[[:space:]]*[0-9a-f]+:[[:space:]]+f[a-z]'; then \
		echo "ERROR: m68k soft-float user object contains FPU instruction" >&2; \
		exit 1; \
	fi
	@if $(M68K_OBJDUMP) -d $(X68K_AUDIT_S_OBJS) | \
		grep -E '\b(cinv|cpush|pflusha)'; then \
		echo "ERROR: MC68040-only instruction in MC68030 objects" >&2; \
		exit 1; \
	fi

$(BUILD)/user/userland/x68k-contract.o: userland/x68k-contract.S
	@mkdir -p $(dir $@)
	$(M68K_CC) $(M68K_USER_CPPFLAGS) $(M68K_USER_CFLAGS) -c $< -o $@

$(X68K_CRT0_OBJ): userland/crt0-m68k.S
	@mkdir -p $(dir $@)
	$(M68K_CC) $(M68K_USER_CPPFLAGS) $(M68K_USER_CFLAGS) -c $< -o $@

x68k-user-abi-check: $(X68K_CRT0_OBJ)
	@$(M68K_OBJDUMP) -dr $< | grep -q 'trap #0' || { \
		echo "ERROR: m68k syscall veneer has no TRAP #0" >&2; exit 1; }
	@$(M68K_OBJDUMP) -dr $< | grep -q 'moveq #80,%d0' || { \
		echo "ERROR: m68k signal restorer syscall number mismatch" >&2; \
		exit 1; }

$(BUILD)/bin/sh: $(X68K_USER_OBJS) $(X68K_PLATFORM)/user.ld \
	scripts/check-user-elf.py
	@mkdir -p $(dir $@)
	$(M68K_LD) --gc-sections -nostdlib -static -z max-page-size=4096 \
		-z stack-size=0x100000 -T $(X68K_PLATFORM)/user.ld \
		$(X68K_USER_OBJS) -o $@
	@test -z "$$($(M68K_NM) -u $@)" || { $(M68K_NM) -u $@; exit 1; }
	$(PYTHON) scripts/check-user-elf.py --machine m68k $@
	@if $(M68K_OBJDUMP) -d --no-show-raw-insn $@ | \
		grep -E '^[[:space:]]*[0-9a-f]+:[[:space:]]+f[a-z]'; then \
		echo "ERROR: m68k soft-float shell contains FPU instruction" >&2; \
		exit 1; \
	fi

$(BUILD)/boot/x68k/%.o: boot/x68k/%.S boot/x68k/boot-layout.h
	@mkdir -p $(dir $@)
	$(M68K_CC) $(M68K_CPPFLAGS) -m68030 -msoft-float -ffreestanding \
		-fno-pic -fno-pie -c $< -o $@

$(BUILD)/boot/x68k/%.o: boot/x68k/%.c boot/x68k/boot-layout.h
	@mkdir -p $(dir $@)
	$(M68K_CC) $(M68K_CPPFLAGS) $(M68K_KERNEL_CFLAGS) -fno-builtin \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/boot/x68k/mb89352.o: drivers/x68k-mb89352.c \
	drivers/x68k-mb89352.h
	@mkdir -p $(dir $@)
	$(M68K_CC) $(M68K_CPPFLAGS) $(M68K_KERNEL_CFLAGS) -fno-builtin \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/vmunix: $(X68K_VMUNIX_OBJS) $(X68K_PLATFORM)/vmunix.ld \
	scripts/check-m68k-vmunix.py
	$(M68K_LD) --gc-sections -z max-page-size=4096 \
		-T $(X68K_PLATFORM)/vmunix.ld -nostdlib $(X68K_VMUNIX_OBJS) -o $@
	@test -z "$$($(M68K_NM) -u $@)" || { $(M68K_NM) -u $@; exit 1; }
	$(PYTHON) scripts/check-m68k-vmunix.py $@
	@if $(M68K_OBJDUMP) -d --no-show-raw-insn $(X68K_VMUNIX_OBJS) | \
		grep -E '^[[:space:]]*[0-9a-f]+:[[:space:]]+f[a-z]'; then \
		echo "ERROR: m68k kernel contains unexpected FPU instructions" >&2; \
		exit 1; \
	fi

$(BUILD)/contract-user.elf: $(X68K_USER_CONTRACT_OBJ) \
	$(X68K_PLATFORM)/user.ld scripts/check-user-elf.py
	$(M68K_LD) --gc-sections -nostdlib -static -z max-page-size=4096 \
		-z stack-size=0x100000 -T $(X68K_PLATFORM)/user.ld \
		$(X68K_USER_CONTRACT_OBJ) -o $@
	@test -z "$$($(M68K_NM) -u $@)" || { $(M68K_NM) -u $@; exit 1; }
	$(PYTHON) scripts/check-user-elf.py --machine m68k $@

$(BUILD)/stage1.elf: $(X68K_STAGE1_OBJ) boot/x68k/stage1.ld
	$(M68K_LD) -N -static -T boot/x68k/stage1.ld -nostdlib \
		$(X68K_STAGE1_OBJ) -o $@

$(BUILD)/stage1.bin: $(BUILD)/stage1.elf
	$(M68K_OBJCOPY) -O binary $< $@
	@test "$$(stat -c %s $@)" -le 1024 || { \
		echo "ERROR: X68k stage 1 exceeds 1024 bytes" >&2; exit 1; }

$(BUILD)/stage2.elf: $(X68K_STAGE2_OBJS) boot/x68k/stage2.ld
	$(M68K_LD) -N -static -T boot/x68k/stage2.ld -nostdlib \
		$(X68K_STAGE2_OBJS) -o $@
	@test -z "$$($(M68K_NM) -u $@)" || { $(M68K_NM) -u $@; exit 1; }

$(BUILD)/stage2.bin: $(BUILD)/stage2.elf
	$(M68K_OBJCOPY) -O binary $< $@

$(BUILD)/zedbsd-x68k.hd: $(BUILD)/stage1.bin $(BUILD)/stage2.bin \
	$(BUILD)/vmunix $(BUILD)/bin/sh scripts/make-x68k-image.py \
	scripts/check-x68k-image.py
	$(PYTHON) scripts/make-x68k-image.py --force \
		--stage1 $(BUILD)/stage1.bin --stage2 $(BUILD)/stage2.bin \
		--kernel $(BUILD)/vmunix --shell $(BUILD)/bin/sh \
		--manifest-json $(BUILD)/manifest.json \
		--manifest-bin $(BUILD)/mame-manifest.bin $@
	$(PYTHON) scripts/check-x68k-image.py \
		--stage1 $(BUILD)/stage1.bin --stage2 $(BUILD)/stage2.bin \
		--kernel $(BUILD)/vmunix --shell $(BUILD)/bin/sh $@

x68k-boot-image hdd-image: $(BUILD)/zedbsd-x68k.hd

x68k-image-check: $(BUILD)/zedbsd-x68k.hd
	$(PYTHON) scripts/check-x68k-image.py \
		--stage1 $(BUILD)/stage1.bin --stage2 $(BUILD)/stage2.bin \
		--kernel $(BUILD)/vmunix --shell $(BUILD)/bin/sh $<
	$(PYTHON) tests/x68k-image-host-test.py \
		--checker scripts/check-x68k-image.py \
		--stage1 $(BUILD)/stage1.bin --stage2 $(BUILD)/stage2.bin \
		--kernel $(BUILD)/vmunix --shell $(BUILD)/bin/sh $<

.PHONY: vmunix SH x68k-contract x68k-user-abi-check x68k-target-audit \
	x68k-emulator-rom-host-test \
	x68k-boot-image hdd-image x68k-image-check

-include $(X68K_EARLY_OBJS:.o=.d) $(X68K_KERNEL_OBJS:.o=.d) \
	$(X68K_KERNEL_LIBC_OBJS:.o=.d) $(X68K_STAGE2_OBJS:.o=.d)
-include $(X68K_AUDIT_C_OBJS:.o=.d)
-include $(X68K_USER_OBJS:.o=.d)
