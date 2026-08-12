# zedBSD NEC PC-9800 architecture rules.
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
#
# Included from the top-level Makefile with ARCH=pc98; everything here
# builds into $(BUILD) = build/pc98.

PC98 := platform/pc98
BOOTSECT := bootsectors/pc98

CIRRUS_NOCT_CFLAGS = $(filter-out -Os,$(NOCT_CFLAGS)) -O2

# These object lists must be defined before the Stage 2 prerequisite list is
# expanded below.  The compiler rules themselves may remain with the related
# verification targets later in this file.
HAL_CC := $(CC) -m32 -march=i386 -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -nostdinc -Os -Wall -Wextra -Werror \
	-Iinclude -Iinclude/uapi -Isrc -Isrc/hal/i386 -Ilibc/include \
	-DHAL_ARCH_I386 -DHAL_BOARD_PC98
HAL_PC98_SOURCES := \
	src/hal/i386/lib.c src/hal/i386/irq.c src/hal/i386/page.c \
	src/hal/i386/space.c src/hal/i386/int.c src/hal/i386/cmain.c \
	src/hal/i386/task.c src/hal/i386/fb.c \
	src/hal/i386/bsp-pc98/cons.c src/hal/i386/bsp-pc98/pic.c \
	src/hal/i386/bsp-pc98/clock.c src/hal/i386/bsp-pc98/display.c \
	src/hal/i386/bsp-pc98/jisx0208.c
HAL_PC98_ASM := src/hal/i386/locore.S src/hal/i386/trap.S \
	src/hal/i386/dispatch.S
HAL_PC98_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(HAL_PC98_SOURCES)) \
	$(patsubst %.S,$(BUILD)/%.o,$(HAL_PC98_ASM))

ZEDBSD_KERN_CC := $(CC) -m32 -march=i386 -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -nostdinc -Os -Wall -Wextra -Werror \
	-Iinclude -Iinclude/uapi -Isrc -I. -Ilibc/include
KERN_OBJS := $(BUILD)/src/kern/entry.o $(BUILD)/src/kern/clock.o \
	$(BUILD)/src/kern/process.o $(BUILD)/src/kern/thread.o \
	$(BUILD)/src/kern/sched.o $(BUILD)/src/kern/vmspace.o \
	$(BUILD)/src/kern/filedesc.o $(BUILD)/src/kern/cwdinfo.o \
	$(BUILD)/src/kern/elf.o $(BUILD)/src/kern/exec.o \
	$(BUILD)/src/kern/user-probe.o $(BUILD)/src/kern/syscall.o \
	$(BUILD)/src/kern/uaccess.o $(BUILD)/src/kern/cdev.o \
	$(BUILD)/src/kern/devfs.o $(BUILD)/src/kern/console-device.o \
	$(BUILD)/src/kern/graphics-device.o $(BUILD)/src/kern/pc98/font.o \
	$(BUILD)/src/kern/system-device.o $(BUILD)/src/kern/boot-device.o \
	$(BUILD)/src/kern/init.o \
	$(BUILD)/src/kern/pc98/graphics.o

# Milestone verification nests QEMU tests.  Keep those chains ordered even
# when the caller requests a highly parallel compile.
.NOTPARALLEL: noct-m9-verify noct-m10-verify noct-m11-verify \
	noct-m14-verify noct-m15-verify noct-m17-verify beui-g2b-verify \
	beui-g2c-verify beui-g5-verify

# BeUI display backends this target selects from upstream Noct.  The
# Core-Graph blitter is the one hot loop in the graphical path, so it
# trades size for speed while the rest of the image stays at -Os.
PC98_BEUI_OBJS := \
	$(NOCT_BUILD_DIR)/beui-pc98-gdc.o \
	$(NOCT_BUILD_DIR)/beui-pc98-glyph.o \
	$(NOCT_BUILD_DIR)/beui-pc98-cirrus.o \
	$(NOCT_BUILD_DIR)/beui-pc98-auto.o

STAGE2_OBJS = \
	$(BUILD)/$(PC98)/boot-header.o \
	$(BUILD)/src/kern/main.o \
	$(BUILD)/src/kern/pc98/linux-entry.o \
	$(BUILD)/src/kern/env.o \
	$(BUILD)/src/kern/fs.o \
	$(BUILD)/src/kern/namespace.o \
	$(PC98_BEUI_OBJS) \
	$(BUILD)/src/kern/fat.o \
	$(BUILD)/src/kern/fat-lfn.o \
	$(BUILD)/src/kern/fat16.o \
	$(BUILD)/src/kern/fat-vfs.o \
	$(BUILD)/src/kern/inode.o \
	$(BUILD)/src/kern/file.o \
	$(BUILD)/src/kern/namecache.o \
	$(BUILD)/src/kern/namei.o \
	$(BUILD)/src/kern/mount.o \
	$(BUILD)/src/kern/rootfs.o \
	$(BUILD)/src/kern/vfs.o \
	$(BUILD)/src/kern/swap.o \
	$(BUILD)/src/kern/swap-fat.o \
	$(BUILD)/src/kern/vm-reclaim.o \
	$(BUILD)/src/kern/disk.o \
	$(BUILD)/src/kern/partition.o \
	$(BUILD)/drivers/pc98-ide.o \
	$(BUILD)/src/kern/pc98/partition.o \
	$(BUILD)/src/kern/pc98/platform.o \
	$(BUILD)/src/kern/pc98/linux-boot.o \
	$(BUILD)/src/kern/image.o \
	$(BUILD)/src/kern/panic.o \
	$(ZEDBSD_LIBC_OBJECTS) \
	$(HAL_PC98_OBJS) $(KERN_OBJS)
M9_STAGE2_OBJS = $(filter-out $(BUILD)/src/kern/main.o \
	$(BUILD)/src/kern/shell.o $(BUILD)/src/kern/device.o,$(STAGE2_OBJS)) \
	$(BUILD)/$(PC98)/stage2-m9-test.o \
	$(BUILD)/$(PC98)/shell-m9-test.o \
	$(BUILD)/$(PC98)/device-m9-test.o

all: $(BUILD)/boot2.bin $(BUILD)/ipl-lba0.bin $(BUILD)/ipl-lba2.bin \
	$(BUILD)/ipl-lba0.img $(BUILD)/ipl-lba2.img $(BUILD)/ipl-part.img \
	$(BUILD)/IO.SYS $(BUILD)/vmunix \
	$(BUILD)/INIT.ELF $(BUILD)/bin/noct $(BUILD)/bin/sh \
	$(BUILD)/bin/linux \
	$(BUILD)/partition-pbr.bin \
	$(BUILD)/chain-test.bin $(BUILD)/fdd-ipl.bin \
	$(BUILD)/BOOTAPP.BIN

# Convenience aliases for the primary artifacts.
vmunix: $(BUILD)/vmunix
vmunix-m9: $(BUILD)/vmunix-m9
BOOTAPP.BIN: $(BUILD)/BOOTAPP.BIN
INIT.ELF: $(BUILD)/INIT.ELF
NOCT.ELF: $(BUILD)/NOCT.ELF
SH: $(BUILD)/bin/sh
LINUX: $(BUILD)/bin/linux
USER-FAULT.ELF: $(BUILD)/USER-FAULT.ELF
USER-SWAP.ELF: $(BUILD)/USER-SWAP.ELF
.PHONY: vmunix vmunix-m9 BOOTAPP.BIN INIT.ELF NOCT.ELF SH LINUX USER-FAULT.ELF USER-SWAP.ELF

# ----------------------------------------------------------------------
# Per-object flag overrides.

$(NOCT_BUILD_DIR)/beui-pc98-cirrus.o: NOCT_CFLAGS := $(CIRRUS_NOCT_CFLAGS)

NOCT_GLUE_OBJS := $(BUILD)/src/noct/noct.o $(BUILD)/src/noct/napi.o \
	$(BUILD)/src/noct/target.o
$(NOCT_GLUE_OBJS): OBJ_CPPFLAGS = $(NOCT_CPPFLAGS) -Iinclude -Isrc
$(NOCT_GLUE_OBJS): OBJ_CFLAGS = $(NOCT_CFLAGS)
$(BUILD)/src/kern/pc98/graphics.o: OBJ_CPPFLAGS = $(NOCT_CPPFLAGS) -Iinclude -Isrc
$(BUILD)/src/kern/pc98/graphics.o: OBJ_CFLAGS = $(ZEDBSD_CFLAGS)
$(BUILD)/src/noct/platform.o: OBJ_CPPFLAGS = $(NOCT_CPPFLAGS) \
	$(ZEDBSD_LIBC_CPPFLAGS)
$(BUILD)/src/noct/platform.o: OBJ_CFLAGS = $(ZEDBSD_LIBC_CFLAGS)

STAGE2_CPPFLAGS = $(ZEDBSD_CPPFLAGS)

$(BUILD)/src/kern/startup.o $(BUILD)/$(PC98)/stage2-m9-test.o: \
	$(BUILD)/kern/messages.h

$(BUILD)/$(PC98)/stage2-m9-test.o: src/kern/main.c
	@mkdir -p $(dir $@)
	$(CC) $(STAGE2_CPPFLAGS) $(ZEDBSD_CFLAGS) -DZEDBSD_M9_WRITE_TEST \
		-MMD -MP -c $< -o $@

$(BUILD)/$(PC98)/shell-m9-test.o: src/kern/shell.c
	@mkdir -p $(dir $@)
	$(CC) $(STAGE2_CPPFLAGS) $(ZEDBSD_CFLAGS) -DZEDBSD_M9_WRITE_TEST \
		-MMD -MP -c $< -o $@

$(BUILD)/$(PC98)/device-m9-test.o: src/kern/device.c
	@mkdir -p $(dir $@)
	$(CC) $(ZEDBSD_CPPFLAGS) $(ZEDBSD_CFLAGS) -DZEDBSD_M9_WRITE_TEST \
		-MMD -MP -c $< -o $@

# ----------------------------------------------------------------------
# Flat binaries from the 16-bit boot-sector world.
# link-flat: name, object, text address.

define link-flat
$(BUILD)/$(1).elf: $(BUILD)/$(BOOTSECT)/$(2).o
	$$(LD) -m elf_i386 -Ttext=$(3) -e _start $$< -o $$@
endef

$(eval $(call link-flat,boot2,boot2,0))
$(eval $(call link-flat,disk-ipl,disk-ipl,0))
$(eval $(call link-flat,lba2,lba2,0))
$(eval $(call link-flat,partition-pbr,partition-pbr,0))
$(eval $(call link-flat,stage1,stage1,0))
$(eval $(call link-flat,chain-test,chain-test,0))
$(eval $(call link-flat,fdd-ipl,fdd-ipl,0))

$(BUILD)/boot2.bin: $(BUILD)/boot2.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@sz=$$(stat -c%s $@); echo "boot2.bin: $$sz bytes"; \
		if [ $$sz -gt 1024 ]; then echo "ERROR: IPL > 1024"; exit 1; fi

$(BUILD)/ipl-lba0.bin: $(BUILD)/disk-ipl.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 512

$(BUILD)/ipl-lba2.bin: $(BUILD)/lba2.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 7168

$(BUILD)/partition-pbr.bin: $(BUILD)/partition-pbr.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 1024


$(BUILD)/IO.SYS: $(BUILD)/stage1.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@sz=$$(stat -c%s $@); echo "IO.SYS: $$sz bytes"; test $$sz -le 65024

$(BUILD)/chain-test.bin: $(BUILD)/chain-test.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 512

$(BUILD)/fdd-ipl.bin: $(BUILD)/fdd-ipl.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 512

# Staging copies used by disk-image tooling.
$(BUILD)/ipl-lba0.img: $(BUILD)/ipl-lba0.bin
	cp $< $@
$(BUILD)/ipl-lba2.img: $(BUILD)/ipl-lba2.bin
	cp $< $@
$(BUILD)/ipl-part.img: $(BUILD)/partition-pbr.bin
	cp $< $@

# ----------------------------------------------------------------------
# Stage 2 (vmunix) and the applet container.

USER_LIBC_OBJS := $(BUILD)/user/crt0.o $(BUILD)/user/libc/posix.o \
	$(BUILD)/libc/heap.o $(BUILD)/libc/string.o $(BUILD)/libc/ctype.o \
	$(BUILD)/libc/int64.o $(BUILD)/libc/strto.o $(BUILD)/libc/format.o \
	$(BUILD)/libc/stdio.o
USER_CFLAGS := $(ZEDBSD_CFLAGS) -fno-builtin -ffunction-sections \
	-fdata-sections -msoft-float -mno-80387 -mno-fp-ret-in-387 \
	-mno-mmx -mno-sse -mno-sse2
$(BUILD)/user/libc/posix.o $(BUILD)/user/tests/syscall-smoke.o: \
	OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(BUILD)/user/libc/posix.o $(BUILD)/user/tests/syscall-smoke.o: \
	OBJ_CFLAGS = $(USER_CFLAGS)

$(BUILD)/INIT.ELF: $(USER_LIBC_OBJS) $(BUILD)/user/tests/syscall-smoke.o \
	$(PC98)/noct-user.ld
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		-T $(PC98)/noct-user.ld $(USER_LIBC_OBJS) \
		$(BUILD)/user/tests/syscall-smoke.o -o $@

USER_NOCT_GLUE_OBJS := $(BUILD)/user/noct/main.o \
	$(BUILD)/user/noct/platform.o $(BUILD)/user/noct/napi.o \
	$(BUILD)/user/noct/target.o $(BUILD)/user/noct/env.o
$(USER_NOCT_GLUE_OBJS): OBJ_CPPFLAGS = $(USER_NOCT_CPPFLAGS) -Iinclude -Isrc
$(USER_NOCT_GLUE_OBJS): OBJ_CFLAGS = $(USER_CFLAGS)
$(BUILD)/user/noct/napi.o: src/noct/napi.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_NOCT_CPPFLAGS) -Iinclude -Isrc $(USER_CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/user/noct/target.o: src/noct/target.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_NOCT_CPPFLAGS) -Iinclude -Isrc $(USER_CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/user/noct/env.o: src/kern/env.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_NOCT_CPPFLAGS) -Iinclude -Isrc $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/NOCT.ELF: $(USER_LIBC_OBJS) $(USER_NOCT_GLUE_OBJS) \
	$(USER_NOCT_OBJECTS) $(ZEDBSD_SOFTFLOAT_OBJECTS) $(PC98)/noct-user.ld
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		-T $(PC98)/noct-user.ld $(USER_LIBC_OBJS) $(USER_NOCT_GLUE_OBJS) \
		$(USER_NOCT_OBJECTS) $(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$($(NOCT_NM) -u $@)" || { $(NOCT_NM) -u $@; exit 1; }

$(BUILD)/bin/noct: $(BUILD)/NOCT.ELF
	@mkdir -p $(dir $@)
	cp $< $@

USER_SH_OBJS := $(BUILD)/user/sh/main.o $(BUILD)/user/sh/applet.o
$(USER_SH_OBJS): OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(USER_SH_OBJS): OBJ_CFLAGS = $(USER_CFLAGS)

$(BUILD)/bin/sh: $(USER_LIBC_OBJS) $(USER_SH_OBJS) \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) $(PC98)/noct-user.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		-T $(PC98)/noct-user.ld $(USER_LIBC_OBJS) $(USER_SH_OBJS) \
		$(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$($(NOCT_NM) -u $@)" || { $(NOCT_NM) -u $@; exit 1; }

USER_BOOTLINUX_OBJS := $(BUILD)/user/bootlinux/main.o
$(USER_BOOTLINUX_OBJS): OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(USER_BOOTLINUX_OBJS): OBJ_CFLAGS = $(USER_CFLAGS)

$(BUILD)/bin/linux: $(USER_LIBC_OBJS) $(USER_BOOTLINUX_OBJS) \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) $(PC98)/noct-user.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		-T $(PC98)/noct-user.ld $(USER_LIBC_OBJS) \
		$(USER_BOOTLINUX_OBJS) $(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$($(NOCT_NM) -u $@)" || { $(NOCT_NM) -u $@; exit 1; }

$(BUILD)/tests/user-fault.o: tests/user-fault.S
	@mkdir -p $(dir $@)
	$(AS) --32 $< -o $@

$(BUILD)/USER-FAULT.ELF: $(BUILD)/tests/user-fault.o $(PC98)/user-init.ld
	$(LD) -m elf_i386 -nostdlib -static -z max-page-size=4096 \
		-T $(PC98)/user-init.ld $< -o $@

$(BUILD)/USER-SWAP.ELF: $(BUILD)/tests/user-swap.o $(PC98)/user-init.ld
	$(LD) -m elf_i386 -nostdlib -static -z max-page-size=4096 \
		-T $(PC98)/user-init.ld $< -o $@

$(BUILD)/stage2.elf: $(STAGE2_OBJS) $(PC98)/stage2.ld
	$(LD) -m elf_i386 --gc-sections -z max-page-size=512 \
		-T $(PC98)/stage2.ld -nostdlib \
		$(STAGE2_OBJS) -o $@

# vmunix is the two-segment ELF itself; patch-stage2.py enforces the
# subset contract Stage 1 relies on and patches the B98S v2 header.
$(BUILD)/vmunix: $(BUILD)/stage2.elf $(SCRIPTS_DIR)/patch-stage2.py
	cp $< $@
	$(PYTHON) $(SCRIPTS_DIR)/patch-stage2.py $@

$(BUILD)/stage2-m9-test.elf: $(M9_STAGE2_OBJS) $(PC98)/stage2.ld
	$(LD) -m elf_i386 --gc-sections -z max-page-size=512 \
		-T $(PC98)/stage2.ld -nostdlib \
		$(M9_STAGE2_OBJS) -o $@

$(BUILD)/vmunix-m9: $(BUILD)/stage2-m9-test.elf $(SCRIPTS_DIR)/patch-stage2.py
	cp $< $@
	$(PYTHON) $(SCRIPTS_DIR)/patch-stage2.py $@

$(BUILD)/applet-test.elf: $(BUILD)/$(BOOTSECT)/applet-test.o $(PC98)/applet.ld
	$(LD) -m elf_i386 -T $(PC98)/applet.ld -nostdlib $< -o $@

$(BUILD)/BOOTAPP.BIN: $(BUILD)/applet-test.elf $(SCRIPTS_DIR)/patch-applet.py
	$(OBJCOPY) -O binary $< $@
	$(PYTHON) $(SCRIPTS_DIR)/patch-applet.py $@

# ----------------------------------------------------------------------
# Test disk images.

$(BUILD)/hdd-test.img: all $(SCRIPTS_DIR)/make-hdd-image.sh \
	$(SCRIPTS_DIR)/install-image.sh
	rm -f $@
	$(SCRIPTS_DIR)/make-hdd-image.sh $@

hdd-image: $(BUILD)/hdd-test.img

hdd-boot-qemu-test:
	$(SCRIPTS_DIR)/test-hdd-boot.sh

.PHONY: hdd-image hdd-boot-qemu-test

# ----------------------------------------------------------------------
# PC-98 host tests.

# Register-level backend tests, upstream with the drivers they cover.
$(BUILD)/tests/beui-pc98-gdc-host-test: \
	$(NOCT_ROOT)/tests/testcases/beui-pc98-gdc-test.c \
	$(NOCT_ROOT)/src/api/beui-pc98-gdc.c $(BEUI_CORE_SOURCES)
	@mkdir -p $(dir $@)
	$(BEUI_TEST_CC) $(NOCT_ROOT)/src/api/beui-pc98-gdc.c \
		$(BEUI_CORE_SOURCES) $< -o $@

$(BUILD)/tests/beui-pc98-cirrus-host-test: \
	$(NOCT_ROOT)/tests/testcases/beui-pc98-cirrus-test.c \
	$(NOCT_ROOT)/src/api/beui-pc98-cirrus.c
	@mkdir -p $(dir $@)
	$(BEUI_TEST_CC) $(NOCT_ROOT)/src/api/beui-pc98-cirrus.c $< -o $@

$(BUILD)/tests/noct-host-test: tests/noct-host-test.c \
	apps/ls.nct apps/cp.nct src/noct/noct-m6-script.h \
	$(NOCT_GLUE_OBJS) $(BUILD)/src/kern/env.o $(BUILD)/src/kern/fs.o \
	$(BUILD)/src/kern/namespace.o \
	$(BUILD)/src/kern/disk.o $(BUILD)/src/kern/inode.o \
	$(BUILD)/src/kern/file.o $(BUILD)/src/kern/namecache.o \
	$(BUILD)/src/kern/namei.o $(BUILD)/src/kern/mount.o \
	$(BUILD)/src/kern/rootfs.o \
	$(NOCT_OBJECTS) $(ZEDBSD_LIBC_OBJECTS) $(ZEDBSD_SOFTFLOAT_OBJECTS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -m32 -no-pie -fno-builtin -fno-stack-protector -Wall -Wextra \
		-Werror -I. -Iinclude -Isrc -Ilibc/include -I$(NOCT_ROOT)/include \
		-DZEDBSD_NOCT_JIT_CODE_MAX=$(NOCT_JIT_CODE_MAX) \
		tests/noct-host-test.c $(NOCT_GLUE_OBJS) \
		$(BUILD)/src/kern/env.o $(BUILD)/src/kern/fs.o \
		$(BUILD)/src/kern/namespace.o $(NOCT_OBJECTS) \
		$(BUILD)/src/kern/disk.o $(BUILD)/src/kern/inode.o \
		$(BUILD)/src/kern/file.o $(BUILD)/src/kern/namecache.o \
		$(BUILD)/src/kern/namei.o $(BUILD)/src/kern/mount.o \
		$(BUILD)/src/kern/rootfs.o \
		$(ZEDBSD_LIBC_OBJECTS) $(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@

NOCT_M6_JIT_CODE := $(BUILD)/logs/m6-jit-code.bin
NOCT_TEST_JIT_CODE_SIZE := 98304

noct-host-test: $(BUILD)/tests/noct-host-test
	@mkdir -p $(dir $(NOCT_M6_JIT_CODE))
	$(BUILD)/tests/noct-host-test $(NOCT_M6_JIT_CODE) apps/ls.nct apps/cp.nct
	@test $$(stat -c%s $(NOCT_M6_JIT_CODE)) -eq $(NOCT_TEST_JIT_CODE_SIZE)
	@echo "zedBSD Noct interpreter/JIT lifecycle host tests: PASS"

# Compile-check the i386 HAL and PC-98 BSP under the same freestanding
# target flags used by the vmunix link.

$(BUILD)/src/hal/%.o: src/hal/%.c
	@mkdir -p $(dir $@)
	$(HAL_CC) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/%.o: src/hal/%.S
	@mkdir -p $(dir $@)
	$(HAL_CC) -D_ASM_SRC_ -MMD -MP -c $< -o $@

hal-pc98-compile: $(HAL_PC98_OBJS)
	@echo "HAL i386/PC-98 compile check: PASS"

# The kernel-side HAL glue compiles in zedBSD' own type world.

$(BUILD)/src/kern/entry.o: src/kern/entry.c
	@mkdir -p $(dir $@)
	$(ZEDBSD_KERN_CC) -MMD -MP -c $< -o $@

kern-compile: $(KERN_OBJS)
	@echo "zedBSD kernel glue compile check: PASS"
.PHONY: hal-pc98-compile kern-compile

$(BUILD)/tests/hal-pc98-keyboard-host-test: \
	tests/hal-pc98-keyboard-host-test.c \
	src/hal/i386/bsp-pc98/cons.c \
	src/hal/i386/bsp-pc98/jisx0208.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -std=gnu11 -Iinclude -Isrc $< -o $@

HOST_TEST_BINARIES += $(BUILD)/tests/beui-pc98-gdc-host-test \
	$(BUILD)/tests/beui-pc98-cirrus-host-test \
	$(BUILD)/tests/hal-pc98-keyboard-host-test
CHECK_RUN_TARGETS += noct-host-test hal-pc98-compile kern-compile

# ----------------------------------------------------------------------
# Milestone and QEMU verification chains.

noct-m4-opcode-check: $(BUILD)/src/noct/noct.o $(BUILD)/src/noct/platform.o
	@if $(NOCT_OBJDUMP) -d --no-show-raw-insn $^ | \
		grep -E '(^[[:space:]]*[0-9a-f]+:[[:space:]]+f[a-z0-9]+[[:space:]])|\b(bswap|cmpxchg|xadd|cmov[a-z]*|rdtsc|ud2|cpuid|fx[a-z]+|movaps|movups|xmm[0-9]|ymm[0-9]|zmm[0-9])\b'; then \
		echo "ERROR: M4 glue contains a post-i386 opcode" >&2; \
		exit 1; \
	fi
	@echo "zedBSD Noct M4 glue i386 opcode check: PASS"

noct-m4-verify: noct-m3-verify noct-host-test noct-m4-opcode-check \
	$(BUILD)/vmunix
	@echo "zedBSD M4 historical lifecycle checks: PASS"

NOCT_M5_DISASSEMBLY := $(BUILD)/logs/m5.disassembly
NOCT_M5_REJECTED := $(BUILD)/logs/m5-rejected.txt

noct-m5-final-opcode-check: $(BUILD)/stage2.elf softfloat-opcode-check
	@mkdir -p $(dir $(NOCT_M5_DISASSEMBLY))
	@$(NOCT_OBJDUMP) -d --no-show-raw-insn $(BUILD)/stage2.elf > \
		$(NOCT_M5_DISASSEMBLY)
	@grep -E '(^[[:space:]]*[0-9a-f]+:[[:space:]]+f[a-z0-9]+[[:space:]])|\b(bswap|cmpxchg|xadd|cmov[a-z]*|rdtsc|ud2|cpuid|fx[a-z]+|movaps|movups|xmm[0-9]|ymm[0-9]|zmm[0-9])\b' \
		$(NOCT_M5_DISASSEMBLY) > $(NOCT_M5_REJECTED) || true
	@if test -s $(NOCT_M5_REJECTED); then \
		echo "ERROR: final vmunix ELF contains a forbidden opcode" >&2; \
		cat $(NOCT_M5_REJECTED) >&2; \
		exit 1; \
	fi
	@echo "zedBSD M5 final i386 opcode check: PASS"

noct-m5-verify: noct-m4-verify softfloat-host-test noct-m5-final-opcode-check
	@echo "zedBSD M5 historical soft-float checks: PASS"

noct-m6-verify: noct-m5-verify noct-host-test $(BUILD)/vmunix
	@echo "zedBSD M6 verification: PASS (forced i386 JIT)"

noct-m7-verify: noct-m6-verify noct-host-test $(BUILD)/vmunix
	@echo "zedBSD M7 host/build verification: PASS (arguments and main signature)"

noct-m8-verify: noct-m7-verify noct-host-test $(BUILD)/vmunix \
	noct-m5-final-opcode-check
	@echo "zedBSD M8 host/build verification: PASS (safe native APIs)"

bios-write-qemu-test: $(BUILD)/vmunix-m9
	$(SCRIPTS_DIR)/test-bios-write.sh all

noct-m9-verify: noct-m8-verify check $(BUILD)/vmunix $(BUILD)/vmunix-m9 \
	bios-write-qemu-test
	@echo "zedBSD M9 verification: PASS (IDE/SCSI BIOS write/read/restore)"

noct-file-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-noct-file.sh

ide-multidrive-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-ide-multidrive.sh

noct-m10-verify: noct-m9-verify check $(BUILD)/vmunix \
	noct-m5-final-opcode-check noct-file-qemu-test
	@echo "zedBSD M10 verification: PASS (FAT16 writer and Noct File API)"

noct-utilities-qemu-test: $(BUILD)/vmunix apps/ls.nct apps/cp.nct
	$(SCRIPTS_DIR)/test-noct-utilities.sh

noct-m11-verify: noct-m10-verify check $(BUILD)/vmunix \
	noct-m5-final-opcode-check noct-utilities-qemu-test
	@echo "zedBSD M11 safe utilities verification: PASS (ls.nct and cp.nct)"

noct-env-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-noct-env.sh

noct-m14-verify: noct-m11-verify check $(BUILD)/vmunix \
	noct-m5-final-opcode-check noct-env-qemu-test
	@echo "zedBSD M14 verification: PASS (environment and intrinsic APIs)"

noct-repl-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-noct-repl.sh

term-japanese-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-term-japanese.sh

noct-m15-verify: noct-m14-verify check $(BUILD)/vmunix \
	noct-m5-final-opcode-check noct-repl-qemu-test
	@echo "zedBSD M15 REPL verification: PASS (keyboard/error/Ctrl-C on i386)"

noct-m17-verify: noct-m15-verify check $(BUILD)/vmunix \
	noct-m5-final-opcode-check
	@for memory in 5 8 16 32 64 96; do \
		echo "Testing zedBSD Noct RAM profile: $${memory} MiB"; \
		ZEDBSD_TEST_MEMORY_MIB=$$memory \
			$(SCRIPTS_DIR)/test-noct-repl.sh || exit $$?; \
	done
	@echo "zedBSD M17 verification: PASS (5/8/16/32/64/>64 MiB profiles)"

beui-g1-verify: check $(BUILD)/vmunix noct-m5-final-opcode-check
	@echo "zedBSD BeUI G1 verification: PASS (lifecycle and HAL boundary)"

beui-gdc-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-beui-gdc.sh

beui-g2a-verify: check $(BUILD)/vmunix noct-m5-final-opcode-check \
	beui-gdc-qemu-test
	@echo "zedBSD BeUI G2a verification: PASS (GDC and BMP image path)"

beui-cirrus-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-beui-cirrus.sh

beui-g2b-verify: beui-g2a-verify
	$(SCRIPTS_DIR)/test-beui-cirrus.sh
	@echo "zedBSD BeUI G2b verification: PASS (Core-Graph/Cirrus and GDC fallback)"

beui-menu-cirrus-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-beui-menu.sh

beui-menu-gdc-qemu-test: $(BUILD)/vmunix
	ZEDBSD_BEUI_MACHINE=pc9801 ZEDBSD_BEUI_TEST_TAG=menu-gdc \
		$(SCRIPTS_DIR)/test-beui-menu.sh

beui-g2c-verify: beui-g2b-verify \
	beui-menu-cirrus-qemu-test beui-menu-gdc-qemu-test
	@echo "zedBSD BeUI G2c verification: PASS (CGROM text and keyboard menu)"

autoexec-remacs-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-autoexec-remacs.sh

beui-g4-verify: beui-g2c-verify term-japanese-qemu-test \
	autoexec-remacs-qemu-test
	@echo "zedBSD BeUI G4 verification: PASS (menu.nct to Remacs)"

beui-input-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-beui-input.sh

beui-holoris-cirrus-qemu-test: $(BUILD)/vmunix
	$(SCRIPTS_DIR)/test-beui-holoris.sh

beui-holoris-gdc-qemu-test: $(BUILD)/vmunix
	ZEDBSD_HOLORIS_MACHINE=pc9801 ZEDBSD_HOLORIS_TEST_TAG=holoris-gdc \
		$(SCRIPTS_DIR)/test-beui-holoris.sh

swap-lowmem-qemu-test: $(BUILD)/vmunix $(BUILD)/USER-SWAP.ELF
	$(SCRIPTS_DIR)/test-swap-lowmem.sh

beui-g5-verify: beui-g4-verify beui-input-qemu-test \
	beui-holoris-cirrus-qemu-test beui-holoris-gdc-qemu-test
	@echo "zedBSD BeUI G5 verification: PASS (BeUI-only input and Holoris)"

.PHONY: noct-host-test noct-m4-opcode-check noct-m4-verify \
	ide-multidrive-qemu-test \
	noct-m5-final-opcode-check noct-m5-verify noct-m6-verify \
	noct-m7-verify noct-m8-verify bios-write-qemu-test noct-m9-verify \
	noct-file-qemu-test noct-m10-verify noct-utilities-qemu-test \
	noct-m11-verify noct-env-qemu-test noct-m14-verify \
	noct-repl-qemu-test term-japanese-qemu-test noct-m15-verify \
	noct-m17-verify beui-g1-verify beui-gdc-qemu-test beui-g2a-verify \
	beui-cirrus-qemu-test beui-g2b-verify beui-menu-cirrus-qemu-test \
	beui-menu-gdc-qemu-test beui-g2c-verify autoexec-remacs-qemu-test \
	beui-g4-verify beui-input-qemu-test beui-holoris-cirrus-qemu-test \
	beui-holoris-gdc-qemu-test beui-g5-verify swap-lowmem-qemu-test
