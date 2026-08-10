# Boots NEC PC-9800 architecture rules.
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
	-Ihal/include -Ihal/i386 -DHAL_ARCH_I386 -DHAL_BOARD_PC98
HAL_PC98_SOURCES := \
	hal/i386/lib.c hal/i386/irq.c hal/i386/page.c hal/i386/univ.c \
	hal/i386/int.c hal/i386/cmain.c hal/i386/task.c hal/i386/fb.c \
	hal/i386/bsp-pc98/cons.c hal/i386/bsp-pc98/pic.c \
	hal/i386/bsp-pc98/clock.c
HAL_PC98_ASM := hal/i386/locore.S hal/i386/trap.S hal/i386/dispatch.S
HAL_PC98_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(HAL_PC98_SOURCES)) \
	$(patsubst %.S,$(BUILD)/%.o,$(HAL_PC98_ASM))

BOOTS_KERN_CC := $(CC) -m32 -march=i386 -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -nostdinc -Os -Wall -Wextra -Werror \
	-I. -Ilibc/include -Ihal/include
KERN_OBJS := $(BUILD)/kern/kmain.o $(BUILD)/kern/sched-stub.o \
	$(BUILD)/kern/entry.o

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
	$(BUILD)/$(PC98)/stage2.o \
	$(BUILD)/$(PC98)/console.o \
	$(BUILD)/$(PC98)/display-pc98.o \
	$(BUILD)/$(PC98)/exit-trampoline.o \
	$(BUILD)/core/env.o \
	$(BUILD)/core/fs.o \
	$(BUILD)/core/namespace.o \
	$(PC98_BEUI_OBJS) \
	$(BUILD)/core/fat.o \
	$(BUILD)/core/fat16.o \
	$(BUILD)/core/blkdev.o \
	$(BUILD)/core/partition.o \
	$(BUILD)/drivers/ide-pc98.o \
	$(BUILD)/drivers/kbd-pc98.o \
	$(BUILD)/drivers/kbd-pc98-map.o \
	$(BUILD)/$(PC98)/partition-pc98.o \
	$(BUILD)/core/image.o \
	$(BUILD)/core/noct.o \
	$(BUILD)/core/noct-memory.o \
	$(BUILD)/core/noct-napi.o \
	$(BUILD)/$(PC98)/noct-platform.o \
	$(BUILD)/$(PC98)/noct-target.o \
	$(NOCT_OBJECTS) $(BOOTS_LIBC_OBJECTS) $(BOOTS_SOFTFLOAT_OBJECTS) \
	$(HAL_PC98_OBJS) $(KERN_OBJS)
M9_STAGE2_OBJS = $(filter-out $(BUILD)/$(PC98)/stage2.o,$(STAGE2_OBJS)) \
	$(BUILD)/$(PC98)/stage2-m9-test.o

all: $(BUILD)/boot2.bin $(BUILD)/ipl-lba0.bin $(BUILD)/ipl-lba2.bin \
	$(BUILD)/ipl-lba0.img $(BUILD)/ipl-lba2.img $(BUILD)/ipl-part.img \
	$(BUILD)/IO.SYS $(BUILD)/BOOT.SYS \
	$(BUILD)/partition-pbr.bin \
	$(BUILD)/chain-test.bin $(BUILD)/fdd-ipl.bin \
	$(BUILD)/BOOTAPP.BIN

# Convenience aliases for the primary artifacts.
BOOT.SYS: $(BUILD)/BOOT.SYS
BOOT-M9.SYS: $(BUILD)/BOOT-M9.SYS
BOOTAPP.BIN: $(BUILD)/BOOTAPP.BIN
.PHONY: BOOT.SYS BOOT-M9.SYS BOOTAPP.BIN

# ----------------------------------------------------------------------
# Per-object flag overrides.

$(NOCT_BUILD_DIR)/beui-pc98-cirrus.o: NOCT_CFLAGS := $(CIRRUS_NOCT_CFLAGS)

NOCT_GLUE_OBJS := $(BUILD)/core/noct.o $(BUILD)/core/noct-napi.o \
	$(BUILD)/$(PC98)/noct-target.o
$(NOCT_GLUE_OBJS): OBJ_CPPFLAGS = $(NOCT_CPPFLAGS)
$(NOCT_GLUE_OBJS): OBJ_CFLAGS = $(NOCT_CFLAGS)
$(BUILD)/$(PC98)/noct-platform.o: OBJ_CPPFLAGS = $(NOCT_CPPFLAGS) \
	$(BOOTS_LIBC_CPPFLAGS)
$(BUILD)/$(PC98)/noct-platform.o: OBJ_CFLAGS = $(BOOTS_LIBC_CFLAGS)

# stage2.c builds the BeUI HAL from the upstream PC-98 backends, so it
# needs the Noct include paths and the freestanding libc headers those
# reach for.  The code generation flags stay the ordinary Boots ones.
STAGE2_CPPFLAGS = $(NOCT_CPPFLAGS) -Ihal/include
$(BUILD)/$(PC98)/stage2.o: OBJ_CPPFLAGS = $(STAGE2_CPPFLAGS)
$(BUILD)/drivers/kbd-pc98-map.o: OBJ_CPPFLAGS = $(NOCT_CPPFLAGS)

$(BUILD)/$(PC98)/stage2.o $(BUILD)/$(PC98)/stage2-m9-test.o: \
	$(BUILD)/core/messages.h

$(BUILD)/$(PC98)/stage2-m9-test.o: $(PC98)/stage2.c
	@mkdir -p $(dir $@)
	$(CC) $(STAGE2_CPPFLAGS) $(BOOTS_CFLAGS) -DBOOTS_M9_WRITE_TEST \
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
# Stage 2 (BOOT.SYS) and the applet container.

$(BUILD)/stage2.elf: $(STAGE2_OBJS) $(PC98)/stage2.ld
	$(LD) -m elf_i386 --gc-sections -z max-page-size=512 \
		-T $(PC98)/stage2.ld -nostdlib \
		$(STAGE2_OBJS) -o $@

# BOOT.SYS is the two-segment ELF itself; patch-stage2.py enforces the
# subset contract Stage 1 relies on and patches the B98S v2 header.
$(BUILD)/BOOT.SYS: $(BUILD)/stage2.elf $(SCRIPTS_DIR)/patch-stage2.py
	cp $< $@
	$(PYTHON) $(SCRIPTS_DIR)/patch-stage2.py $@

$(BUILD)/stage2-m9-test.elf: $(M9_STAGE2_OBJS) $(PC98)/stage2.ld
	$(LD) -m elf_i386 --gc-sections -z max-page-size=512 \
		-T $(PC98)/stage2.ld -nostdlib \
		$(M9_STAGE2_OBJS) -o $@

$(BUILD)/BOOT-M9.SYS: $(BUILD)/stage2-m9-test.elf $(SCRIPTS_DIR)/patch-stage2.py
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
	$(NOCT_ROOT)/tests/beui-pc98-gdc-test.c \
	$(NOCT_ROOT)/src/api/beui-pc98-gdc.c $(BEUI_CORE_SOURCES)
	@mkdir -p $(dir $@)
	$(BEUI_TEST_CC) $(NOCT_ROOT)/src/api/beui-pc98-gdc.c \
		$(BEUI_CORE_SOURCES) $< -o $@

$(BUILD)/tests/beui-pc98-cirrus-host-test: \
	$(NOCT_ROOT)/tests/beui-pc98-cirrus-test.c \
	$(NOCT_ROOT)/src/api/beui-pc98-cirrus.c
	@mkdir -p $(dir $@)
	$(BEUI_TEST_CC) $(NOCT_ROOT)/src/api/beui-pc98-cirrus.c $< -o $@

$(BUILD)/tests/noct-host-test: tests/noct-host-test.c \
	apps/LS.NCT apps/CP.NCT core/noct-m6-script.h \
	$(NOCT_GLUE_OBJS) $(BUILD)/core/env.o $(BUILD)/core/fs.o $(BUILD)/core/namespace.o \
	$(NOCT_OBJECTS) $(BOOTS_LIBC_OBJECTS) $(BOOTS_SOFTFLOAT_OBJECTS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -m32 -no-pie -fno-builtin -fno-stack-protector -Wall -Wextra \
		-Werror -I. -Ilibc/include -I$(NOCT_ROOT)/include \
		-DBOOTS_NOCT_JIT_CODE_MAX=$(NOCT_JIT_CODE_MAX) \
		tests/noct-host-test.c $(NOCT_GLUE_OBJS) \
		$(BUILD)/core/env.o $(BUILD)/core/fs.o \
		$(BUILD)/core/namespace.o $(NOCT_OBJECTS) \
		$(BOOTS_LIBC_OBJECTS) $(BOOTS_SOFTFLOAT_OBJECTS) -o $@

NOCT_M6_JIT_CODE := $(BUILD)/logs/m6-jit-code.bin
NOCT_TEST_JIT_CODE_SIZE := 98304

noct-host-test: $(BUILD)/tests/noct-host-test
	@mkdir -p $(dir $(NOCT_M6_JIT_CODE))
	$(BUILD)/tests/noct-host-test $(NOCT_M6_JIT_CODE) apps/LS.NCT apps/CP.NCT
	@test $$(stat -c%s $(NOCT_M6_JIT_CODE)) -eq $(NOCT_TEST_JIT_CODE_SIZE)
	@echo "Boots Noct interpreter/JIT lifecycle host tests: PASS"

# Compile-check the i386 HAL and PC-98 BSP under the same freestanding
# target flags used by the BOOT.SYS link.

$(BUILD)/hal/%.o: hal/%.c
	@mkdir -p $(dir $@)
	$(HAL_CC) -MMD -MP -c $< -o $@

$(BUILD)/hal/%.o: hal/%.S
	@mkdir -p $(dir $@)
	$(HAL_CC) -D_ASM_SRC_ -MMD -MP -c $< -o $@

hal-pc98-compile: $(HAL_PC98_OBJS)
	@echo "HAL i386/PC-98 compile check: PASS"

# The kernel-side HAL glue compiles in Boots' own type world.

$(BUILD)/kern/kmain.o: kern/kmain.c
	@mkdir -p $(dir $@)
	$(BOOTS_KERN_CC) -MMD -MP -c $< -o $@

$(BUILD)/kern/sched-stub.o: kern/sched-stub.c
	@mkdir -p $(dir $@)
	$(HAL_CC) -MMD -MP -c $< -o $@

$(BUILD)/kern/entry.o: kern/entry.S
	@mkdir -p $(dir $@)
	$(HAL_CC) -D_ASM_SRC_ -MMD -MP -c $< -o $@

kern-compile: $(KERN_OBJS)
	@echo "Boots kernel glue compile check: PASS"
.PHONY: hal-pc98-compile kern-compile

HOST_TEST_BINARIES += $(BUILD)/tests/beui-pc98-gdc-host-test \
	$(BUILD)/tests/beui-pc98-cirrus-host-test
CHECK_RUN_TARGETS += noct-host-test hal-pc98-compile kern-compile

# ----------------------------------------------------------------------
# Milestone and QEMU verification chains.

noct-m4-opcode-check: $(BUILD)/core/noct.o $(BUILD)/$(PC98)/noct-platform.o
	@if $(NOCT_OBJDUMP) -d --no-show-raw-insn $^ | \
		grep -E '(^[[:space:]]*[0-9a-f]+:[[:space:]]+f[a-z0-9]+[[:space:]])|\b(bswap|cmpxchg|xadd|cmov[a-z]*|rdtsc|ud2|cpuid|fx[a-z]+|movaps|movups|xmm[0-9]|ymm[0-9]|zmm[0-9])\b'; then \
		echo "ERROR: M4 glue contains a post-i386 opcode" >&2; \
		exit 1; \
	fi
	@echo "Boots Noct M4 glue i386 opcode check: PASS"

noct-m4-verify: noct-m3-verify noct-host-test noct-m4-opcode-check \
	$(BUILD)/BOOT.SYS
	@echo "Boots M4 historical lifecycle checks: PASS"

NOCT_M5_DISASSEMBLY := $(BUILD)/logs/m5.disassembly
NOCT_M5_REJECTED := $(BUILD)/logs/m5-rejected.txt

noct-m5-final-opcode-check: $(BUILD)/stage2.elf softfloat-opcode-check
	@mkdir -p $(dir $(NOCT_M5_DISASSEMBLY))
	@$(NOCT_OBJDUMP) -d --no-show-raw-insn $(BUILD)/stage2.elf > \
		$(NOCT_M5_DISASSEMBLY)
	@grep -E '(^[[:space:]]*[0-9a-f]+:[[:space:]]+f[a-z0-9]+[[:space:]])|\b(bswap|cmpxchg|xadd|cmov[a-z]*|rdtsc|ud2|cpuid|fx[a-z]+|movaps|movups|xmm[0-9]|ymm[0-9]|zmm[0-9])\b' \
		$(NOCT_M5_DISASSEMBLY) > $(NOCT_M5_REJECTED) || true
	@if test -s $(NOCT_M5_REJECTED); then \
		echo "ERROR: final BOOT.SYS ELF contains a forbidden opcode" >&2; \
		cat $(NOCT_M5_REJECTED) >&2; \
		exit 1; \
	fi
	@echo "Boots M5 final i386 opcode check: PASS"

noct-m5-verify: noct-m4-verify softfloat-host-test noct-m5-final-opcode-check
	@echo "Boots M5 historical soft-float checks: PASS"

noct-m6-verify: noct-m5-verify noct-host-test $(BUILD)/BOOT.SYS
	@echo "Boots M6 verification: PASS (forced i386 JIT)"

noct-m7-verify: noct-m6-verify noct-host-test $(BUILD)/BOOT.SYS
	@echo "Boots M7 host/build verification: PASS (arguments and main signature)"

noct-m8-verify: noct-m7-verify noct-host-test $(BUILD)/BOOT.SYS \
	noct-m5-final-opcode-check
	@echo "Boots M8 host/build verification: PASS (safe native APIs)"

bios-write-qemu-test: $(BUILD)/BOOT-M9.SYS
	$(SCRIPTS_DIR)/test-bios-write.sh all

noct-m9-verify: noct-m8-verify check $(BUILD)/BOOT.SYS $(BUILD)/BOOT-M9.SYS \
	bios-write-qemu-test
	@echo "Boots M9 verification: PASS (IDE/SCSI BIOS write/read/restore)"

noct-file-qemu-test: $(BUILD)/BOOT.SYS
	$(SCRIPTS_DIR)/test-noct-file.sh

ide-multidrive-qemu-test: $(BUILD)/BOOT.SYS
	$(SCRIPTS_DIR)/test-ide-multidrive.sh

noct-m10-verify: noct-m9-verify check $(BUILD)/BOOT.SYS \
	noct-m5-final-opcode-check noct-file-qemu-test
	@echo "Boots M10 verification: PASS (FAT16 writer and Noct File API)"

noct-utilities-qemu-test: $(BUILD)/BOOT.SYS apps/LS.NCT apps/CP.NCT
	$(SCRIPTS_DIR)/test-noct-utilities.sh

noct-m11-verify: noct-m10-verify check $(BUILD)/BOOT.SYS \
	noct-m5-final-opcode-check noct-utilities-qemu-test
	@echo "Boots M11 safe utilities verification: PASS (LS.NCT and CP.NCT)"

noct-env-qemu-test: $(BUILD)/BOOT.SYS
	$(SCRIPTS_DIR)/test-noct-env.sh

noct-m14-verify: noct-m11-verify check $(BUILD)/BOOT.SYS \
	noct-m5-final-opcode-check noct-env-qemu-test
	@echo "Boots M14 verification: PASS (environment and intrinsic APIs)"

noct-repl-qemu-test: $(BUILD)/BOOT.SYS
	$(SCRIPTS_DIR)/test-noct-repl.sh

term-japanese-qemu-test: $(BUILD)/BOOT.SYS
	$(SCRIPTS_DIR)/test-term-japanese.sh

noct-m15-verify: noct-m14-verify check $(BUILD)/BOOT.SYS \
	noct-m5-final-opcode-check noct-repl-qemu-test
	@echo "Boots M15 REPL verification: PASS (keyboard/error/Ctrl-C on i386)"

noct-m17-verify: noct-m15-verify check $(BUILD)/BOOT.SYS \
	noct-m5-final-opcode-check
	@for memory in 5 8 16 32 64 96; do \
		echo "Testing Boots Noct RAM profile: $${memory} MiB"; \
		BOOTS_TEST_MEMORY_MIB=$$memory \
			$(SCRIPTS_DIR)/test-noct-repl.sh || exit $$?; \
	done
	@echo "Boots M17 verification: PASS (5/8/16/32/64/>64 MiB profiles)"

beui-g1-verify: check $(BUILD)/BOOT.SYS noct-m5-final-opcode-check
	@echo "Boots BeUI G1 verification: PASS (lifecycle and HAL boundary)"

beui-gdc-qemu-test: $(BUILD)/BOOT.SYS
	$(SCRIPTS_DIR)/test-beui-gdc.sh

beui-g2a-verify: check $(BUILD)/BOOT.SYS noct-m5-final-opcode-check \
	beui-gdc-qemu-test
	@echo "Boots BeUI G2a verification: PASS (GDC and BMP image path)"

beui-cirrus-qemu-test: $(BUILD)/BOOT.SYS
	$(SCRIPTS_DIR)/test-beui-cirrus.sh

beui-g2b-verify: beui-g2a-verify
	$(SCRIPTS_DIR)/test-beui-cirrus.sh
	@echo "Boots BeUI G2b verification: PASS (Core-Graph/Cirrus and GDC fallback)"

beui-menu-cirrus-qemu-test: $(BUILD)/BOOT.SYS
	$(SCRIPTS_DIR)/test-beui-menu.sh

beui-menu-gdc-qemu-test: $(BUILD)/BOOT.SYS
	BOOTS_BEUI_MACHINE=pc9801 BOOTS_BEUI_TEST_TAG=menu-gdc \
		$(SCRIPTS_DIR)/test-beui-menu.sh

beui-g2c-verify: beui-g2b-verify \
	beui-menu-cirrus-qemu-test beui-menu-gdc-qemu-test
	@echo "Boots BeUI G2c verification: PASS (CGROM text and keyboard menu)"

autoexec-remacs-qemu-test: $(BUILD)/BOOT.SYS
	$(SCRIPTS_DIR)/test-autoexec-remacs.sh

beui-g4-verify: beui-g2c-verify term-japanese-qemu-test \
	autoexec-remacs-qemu-test
	@echo "Boots BeUI G4 verification: PASS (AUTOEXEC.NCT to Remacs)"

beui-input-qemu-test: $(BUILD)/BOOT.SYS
	$(SCRIPTS_DIR)/test-beui-input.sh

beui-holoris-cirrus-qemu-test: $(BUILD)/BOOT.SYS
	$(SCRIPTS_DIR)/test-beui-holoris.sh

beui-holoris-gdc-qemu-test: $(BUILD)/BOOT.SYS
	BOOTS_HOLORIS_MACHINE=pc9801 BOOTS_HOLORIS_TEST_TAG=holoris-gdc \
		$(SCRIPTS_DIR)/test-beui-holoris.sh

beui-g5-verify: beui-g4-verify beui-input-qemu-test \
	beui-holoris-cirrus-qemu-test beui-holoris-gdc-qemu-test
	@echo "Boots BeUI G5 verification: PASS (BeUI-only input and Holoris)"

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
	beui-holoris-gdc-qemu-test beui-g5-verify
