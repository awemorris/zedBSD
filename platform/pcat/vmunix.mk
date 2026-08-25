# zedBSD IBM PC/AT i386 platform rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
PCAT := platform/pcat
BIOS_LOADER := bootloader/pcat

HAL_CC := $(CC) -m32 -march=i386 -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -nostdinc -Os -Wall -Wextra -Werror \
	-Iinclude -Iinclude/uapi -Isrc -Isrc/hal/i386 -Ilibc/include \
	-DHAL_ARCH_I386 -DHAL_BOARD_PCAT -DHAL_PCAT_DEBUGCON
HAL_PCAT_SOURCES := src/hal/i386/smp.c src/hal/i386/percpu.c src/hal/x86/rtc.c src/hal/i386/lib.c src/hal/i386/atomic.c src/hal/i386/irq.c \
	src/hal/i386/mps.c src/hal/i386/acpi.c src/hal/i386/lapic.c \
	src/hal/i386/ioapic.c src/hal/i386/interrupt-controller.c \
	src/hal/i386/page.c src/hal/i386/space.c src/hal/i386/int.c \
	src/hal/i386/cmain.c src/hal/i386/task.c \
	src/hal/i386/bsp-pcat/boot.c src/hal/i386/bsp-pcat/cons.c \
	src/hal/i386/bsp-pcat/pic.c src/hal/i386/bsp-pcat/pit.c
HAL_PCAT_ASM := src/hal/i386/locore.S src/hal/i386/ap-trampoline.S src/hal/i386/trap.S \
	src/hal/i386/dispatch.S
HAL_PCAT_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(HAL_PCAT_SOURCES)) \
	$(patsubst %.S,$(BUILD)/%.o,$(HAL_PCAT_ASM))

ZEDBSD_KERN_CC := $(CC) -m32 -march=i386 -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -nostdinc -Os -Wall -Wextra -Werror \
	-Iinclude -Iinclude/uapi -Isrc -I. -Ilibc/include
ZEDBSD_KERN_CC += $(ZEDBSD_CONFIG_CPPFLAGS)
KERN_OBJS := $(BUILD)/src/kern/entry.o $(BUILD)/src/kern/clock.o \
	$(BUILD)/src/kern/process-timer.o \
	$(BUILD)/src/kern/lock.o $(BUILD)/src/kern/klog.o $(BUILD)/src/kern/waitq.o \
	$(BUILD)/src/kern/buf.o $(BUILD)/src/kern/sysctl.o \
	$(BUILD)/src/kern/resource.o \
	$(BUILD)/src/kern/resource-limit.o \
	$(BUILD)/src/kern/poll.o \
	$(BUILD)/src/kern/usync.o \
	$(BUILD)/src/kern/process.o $(BUILD)/src/kern/thread.o \
	$(BUILD)/src/kern/sched.o $(BUILD)/src/kern/vm-lock.o \
	$(BUILD)/src/kern/vmspace.o \
	$(BUILD)/src/kern/vm-object.o $(BUILD)/src/kern/vm-commit.o \
	$(BUILD)/src/kern/filedesc.o \
	$(BUILD)/src/kern/record-lock.o \
	$(BUILD)/src/kern/pipe.o $(BUILD)/src/kern/cred.o \
	$(KERN_ACL_OBJS) \
	$(KERN_QUOTA_OBJS) \
	$(BUILD)/src/kern/signal.o \
	$(BUILD)/src/kern/cwdinfo.o $(BUILD)/src/kern/elf.o \
	$(BUILD)/src/kern/exec-prepare.o $(BUILD)/src/kern/exec.o \
	$(BUILD)/src/kern/user-probe.o \
	$(BUILD)/src/kern/syscall.o $(BUILD)/src/kern/uaccess.o \
	$(BUILD)/src/kern/cdev.o $(BUILD)/src/kern/devfs.o \
	$(BUILD)/src/kern/console-device.o $(BUILD)/src/kern/mouse-device.o \
	$(BUILD)/src/kern/input-queue.o $(BUILD)/src/kern/input-device.o \
	$(BUILD)/src/kern/input-keymap.o $(BUILD)/src/kern/locale-record.o \
	$(BUILD)/src/kern/tty.o \
	$(BUILD)/src/kern/graphics-device.o \
	$(BUILD)/src/kern/system-device.o \
	$(BUILD)/src/kern/pcat/font.o $(BUILD)/src/kern/pcat/vgafont.o \
	$(BUILD)/drivers/pcat-graphics.o \
	$(BUILD)/src/kern/init.o \
	$(KERN_NET_OBJS)

PCAT_USB_HCD_OBJS :=
ifeq ($(CONFIG_DRIVER_PCI_UHCI),y)
PCAT_USB_HCD_OBJS += $(BUILD)/drivers/pci-uhci.o
endif
ifeq ($(CONFIG_DRIVER_PCI_EHCI),y)
PCAT_USB_HCD_OBJS += $(BUILD)/drivers/pci-ehci.o
endif
ifeq ($(CONFIG_DRIVER_PCI_XHCI),y)
PCAT_USB_HCD_OBJS += $(BUILD)/drivers/pci-xhci.o
endif
PCAT_USB_CLASS_OBJS :=
ifeq ($(CONFIG_DRIVER_USB_STORAGE),y)
PCAT_USB_CLASS_OBJS += $(BUILD)/drivers/usb-storage.o
endif

VMUNIX_OBJS := $(BUILD)/src/kern/main.o $(BUILD)/src/kern/env.o \
	$(BUILD)/src/kern/fs.o $(BUILD)/src/kern/namespace.o \
	$(BUILD)/src/kern/fat.o $(BUILD)/src/kern/fat-lfn.o \
	$(BUILD)/src/kern/fat16.o $(BUILD)/src/kern/fat-vfs.o \
	$(BUILD)/src/kern/inode.o $(BUILD)/src/kern/file.o \
	$(BUILD)/src/kern/namecache.o $(BUILD)/src/kern/namei.o \
	$(BUILD)/src/kern/mount.o $(BUILD)/src/kern/rootfs.o \
	$(BUILD)/src/kern/tmpfs.o \
	$(BUILD)/src/kern/overlayfs.o \
	$(BUILD)/src/kern/vfs.o $(BUILD)/src/kern/swap.o \
	$(BUILD)/src/kern/swap-fat.o $(BUILD)/src/kern/vm-reclaim.o \
	$(BUILD)/src/kern/disk.o $(BUILD)/src/kern/partition.o \
	$(BUILD)/drivers/loop.o $(BUILD)/drivers/dma.o \
	$(BUILD)/drivers/pci.o $(BUILD)/drivers/pci-pcat.o \
	$(BUILD)/drivers/usb.o $(PCAT_USB_HCD_OBJS) \
	$(PCAT_USB_CLASS_OBJS) \
	$(BUILD)/drivers/pcat-ide.o $(BUILD)/drivers/dp8390.o \
	$(BUILD)/drivers/pcat-ne2000.o \
	$(BUILD)/drivers/pcat-ps2-mouse.o \
	$(BUILD)/src/kern/mbr-partition.o \
	$(BUILD)/src/kern/pcat/platform.o $(BUILD)/src/kern/image.o \
	$(BUILD)/src/kern/panic.o $(ZEDBSD_LIBC_OBJECTS) \
	$(HAL_PCAT_OBJS) $(KERN_OBJS) $(KERN_UFS1_OBJS) $(KERN_UFS2_OBJS) \
	$(KERN_UFS_CONSISTENCY_OBJS) $(ZEDBSD_COMPILER_RT_OBJECTS)

vmunix: $(BUILD)/vmunix

# Native four-stage MBR/FAT16 loader.
$(BUILD)/bootloader/stage1.o: $(BIOS_LOADER)/stage1.S \
	bootloader/include/disk-layout.inc bootloader/include/stage2-header.inc
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

$(BUILD)/bootloader/stage1.elf: $(BUILD)/bootloader/stage1.o \
	$(BIOS_LOADER)/stage1.ld
	$(LD) -m elf_i386 -T $(BIOS_LOADER)/stage1.ld $< -o $@

$(BUILD)/bootloader/stage1.bin: $(BUILD)/bootloader/stage1.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 512
	@test "$$(od -An -tx1 -j510 -N2 $@ | tr -d ' \n')" = 55aa

$(BUILD)/bootloader/stage2.o: $(BIOS_LOADER)/stage2-chain.S \
	bootloader/include/stage2-header.inc
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

$(BUILD)/bootloader/stage2.elf: $(BUILD)/bootloader/stage2.o \
	$(BIOS_LOADER)/stage2.ld
	$(LD) -m elf_i386 -T $(BIOS_LOADER)/stage2.ld $< -o $@

$(BUILD)/bootloader/stage2.raw: $(BUILD)/bootloader/stage2.elf
	$(OBJCOPY) -O binary -j .text $< $@

$(BUILD)/bootloader/stage2.bin: $(BUILD)/bootloader/stage2.raw \
	$(BUILD_TOOLS_DIR)/finalize-bios-stage2.noct
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(BUILD_TOOLS_DIR)/finalize-bios-stage2.noct --machine pcat $< $@

$(BUILD)/bootloader/partition-pbr.o: $(BIOS_LOADER)/partition-pbr.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@
$(BUILD)/bootloader/partition-pbr.elf: $(BUILD)/bootloader/partition-pbr.o
	$(LD) -m elf_i386 -Ttext=0 -e _start $< -o $@
$(BUILD)/bootloader/partition-pbr.bin: $(BUILD)/bootloader/partition-pbr.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 2048

$(BUILD)/bootloader/bootzbsd.o: $(BIOS_LOADER)/bootzbsd.S \
	$(BIOS_LOADER)/vbe.inc \
	bootloader/include/disk-layout.inc bootloader/include/stage2-header.inc \
	bootloader/include/mbr.inc bootloader/include/fat16.inc \
	bootloader/include/elf.inc bootloader/include/amd64-handoff.h
	@mkdir -p $(dir $@)
	$(CC) -m64 -I. -x assembler-with-cpp -c $< -o $@
$(BUILD)/bootloader/bootzbsd.elf: $(BUILD)/bootloader/bootzbsd.o $(BIOS_LOADER)/stage2.ld
	$(LD) -m elf_x86_64 -T $(BIOS_LOADER)/stage2.ld $< -o $@
$(BUILD)/bootloader/bootzbsd.raw: $(BUILD)/bootloader/bootzbsd.elf
	$(OBJCOPY) -O binary -j .text $< $@
$(BUILD)/bootloader/bootzbsd.bin: $(BUILD)/bootloader/bootzbsd.raw \
	$(BUILD_TOOLS_DIR)/finalize-bios-stage2.noct
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(BUILD_TOOLS_DIR)/finalize-bios-stage2.noct --machine pcat $< $@
$(BUILD)/bootloader/BOOTZBSD.EXE: $(BUILD)/bootloader/bootzbsd.bin \
	$(BUILD_TOOLS_DIR)/make-mz-exe.noct
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(BUILD_TOOLS_DIR)/make-mz-exe.noct --entry 0x20 $< $@

$(BUILD)/bootloader/payload32.o: bootloader/tests/payload32-pcat.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@

$(BUILD)/bootloader/payload32.elf: $(BUILD)/bootloader/payload32.o \
	bootloader/tests/payload32-pcat.ld
	$(LD) -m elf_i386 -T bootloader/tests/payload32-pcat.ld $< -o $@

$(BUILD)/bootloader/payload64.o: bootloader/tests/payload64-pcat.S \
	bootloader/include/amd64-handoff.h
	@mkdir -p $(dir $@)
	$(CC) -m64 -I. -c $< -o $@

$(BUILD)/bootloader/payload64.elf: $(BUILD)/bootloader/payload64.o \
	bootloader/tests/payload64-pcat.ld
	$(LD) -m elf_x86_64 -T bootloader/tests/payload64-pcat.ld $< -o $@

USER_BASIC_COMMANDS := $(filter $(ZEDBSD_USER_PROGRAMS),$(USERLAND_BASIC_PROGRAMS))
USER_BASIC_TARGETS := $(addprefix $(BUILD)/bin/,$(USER_BASIC_COMMANDS))

I386_ARCH_IMAGE := $(ARCH_IMAGE_DIR)/i386.img
I386_ARCH_INPUTS := $(BUILD)/bin/sh \
	$(BUILD)/bin/nettest \
	$(BUILD)/bin/sysctl $(BUILD)/bin/mount $(BUILD)/bin/umount \
	$(BUILD)/dynamic/ld.so $(BUILD)/dynamic/libc.so \
	$(BUILD)/dynamic/dyntest $(BUILD)/dynamic/tlstest.so \
	$(BUILD)/dynamic/alt/rpathdep.so $(BUILD)/dynamic/rpathtest.so \
	$(BUILD)/dynamic/verstest.so $(BUILD)/dynamic/versuse.so
I386_ARCH_FILES := --file /bin/sh=$(BUILD)/bin/sh \
	--file /bin/nettest=$(BUILD)/bin/nettest \
	--file /sbin/sysctl=$(BUILD)/bin/sysctl \
	--file /sbin/mount=$(BUILD)/bin/mount \
	--file /sbin/umount=$(BUILD)/bin/umount \
	--file /lib/ld.so=$(BUILD)/dynamic/ld.so \
	--file /lib/libc.so=$(BUILD)/dynamic/libc.so \
	--file /lib/tlstest.so=$(BUILD)/dynamic/tlstest.so \
	--file /lib/alt/rpathdep.so=$(BUILD)/dynamic/alt/rpathdep.so \
	--file /lib/rpthtest.so=$(BUILD)/dynamic/rpathtest.so \
	--file /lib/verstest.so=$(BUILD)/dynamic/verstest.so \
	--file /lib/versuse.so=$(BUILD)/dynamic/versuse.so \
	--file /bin/dyntest=$(BUILD)/dynamic/dyntest
I386_ARCH_INPUTS += $(addprefix $(BUILD)/bin/,$(USERLAND_SELECTED_NETWORK_PROGRAMS))
I386_ARCH_FILES += $(foreach command,$(USERLAND_SELECTED_NETWORK_PROGRAMS),--file $(call zedbsd_userland_destination,$(command))=$(BUILD)/bin/$(command))
I386_ARCH_INPUTS += $(USER_BASIC_TARGETS)
I386_ARCH_FILES += $(foreach command,$(USER_BASIC_COMMANDS),--file $(call zedbsd_userland_destination,$(command))=$(BUILD)/bin/$(command))
I386_ARCH_FILES += $(ZEDBSD_USERLAND_FILE_MODES)
I386_ARCH_INPUTS += $(ZEDBSD_ACCOUNT_INPUTS)
I386_ARCH_FILES += $(ZEDBSD_ACCOUNT_FILES)
I386_ARCH_INPUTS += $(ZEDBSD_BASE_DATA_INPUTS)
I386_ARCH_FILES += $(ZEDBSD_BASE_DATA_FILES)
$(eval $(call ZEDBSD_ARCH_IMAGE_RULE,$(I386_ARCH_IMAGE),i386,$(I386_ARCH_INPUTS),$(I386_ARCH_FILES)))
$(eval $(call ZEDBSD_ROOTFS_TAR_RULE,$(BUILD)/rootfs.tar.gz,$(I386_ARCH_INPUTS),$(I386_ARCH_FILES)))
I386_ARCH_UFS_IMAGE := $(ARCH_IMAGE_DIR)/i386.ufs
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(I386_ARCH_UFS_IMAGE),i386,$(I386_ARCH_INPUTS),$(I386_ARCH_FILES)))
rootfs: $(BUILD)/rootfs/.stamp

$(BUILD)/bios-hdd-image.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix $(I386_ARCH_UFS_IMAGE) $(DATA_IMAGE) $(SWAP_IMAGE) \
	$(HOLORIS_NOCT) \
	$(BUILD_TOOLS_DIR)/make-bios-hdd-image.noct \
	$(BUILD_TOOLS_DIR)/check-bios-hdd-image.noct
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(BUILD_TOOLS_DIR)/make-bios-hdd-image.noct --backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force \
		--machine pcat --stage1 $(BUILD)/bootloader/stage1.bin \
		--stage2 $(BUILD)/bootloader/stage2.bin --partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
		--bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE --kernel $(BUILD)/vmunix \
		--arch-profile i386 --arch-image $(I386_ARCH_UFS_IMAGE) \
		--arch-format ufs --data-image $(DATA_IMAGE) \
		--swapfile $(SWAP_IMAGE) $@
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(BUILD_TOOLS_DIR)/check-bios-hdd-image.noct \
		--machine pcat --bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
		--kernel $(BUILD)/vmunix --arch-image $(I386_ARCH_UFS_IMAGE) \
		--data-image $(DATA_IMAGE) --swapfile $(SWAP_IMAGE) $@

$(BUILD)/ufs-root.img: $(I386_ARCH_UFS_IMAGE) \
	$(BUILD_TOOLS_DIR)/make-ufs1-root-image.py tools/build/ufs1_format.py
	$(PYTHON) $(BUILD_TOOLS_DIR)/make-ufs1-root-image.py --force \
		--arch-profile i386 --arch-image $(I386_ARCH_UFS_IMAGE) $@

$(BUILD)/ufs-root-hdd-image.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix $(BUILD)/ufs-root.img \
	$(BUILD_TOOLS_DIR)/make-bios-hdd-image.py
	$(PYTHON) $(BUILD_TOOLS_DIR)/make-bios-hdd-image.py --force \
		--machine pcat --stage1 $(BUILD)/bootloader/stage1.bin \
		--stage2 $(BUILD)/bootloader/stage2.bin --partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
		--bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE --kernel $(BUILD)/vmunix \
		--ufs-root $(BUILD)/ufs-root.img --size-mib 193 $@

$(BUILD)/src/hal/%.o: src/hal/%.c
	@mkdir -p $(dir $@)
	$(HAL_CC) -MMD -MP -c $< -o $@
$(BUILD)/src/hal/%.o: src/hal/%.S
	@mkdir -p $(dir $@)
	$(HAL_CC) -D_ASM_SRC_ -MMD -MP -c $< -o $@
$(BUILD)/src/kern/entry.o: src/kern/entry.c
	@mkdir -p $(dir $@)
	$(ZEDBSD_KERN_CC) -MMD -MP -c $< -o $@

$(BUILD)/vmunix: $(VMUNIX_OBJS) $(PCAT)/vmunix.ld \
	platform/pcat/tools/check-pcat-vmunix.noct
	$(LD) -m elf_i386 --gc-sections -z max-page-size=4096 \
		-T $(PCAT)/vmunix.ld -nostdlib $(VMUNIX_OBJS) -o $@
	$(NOCT) --path=$(BUILD_TOOLS_DIR) platform/pcat/tools/check-pcat-vmunix.noct $@
	grub-file --is-x86-multiboot $@

USER_LIBC_OBJS := $(BUILD)/src/crt/crt0.o \
	$(BUILD)/userland/base/libc/posix.o $(BUILD)/userland/base/libc/poll.o \
	$(BUILD)/userland/base/libc/termios.o \
	$(BUILD)/userland/base/libc/pthread.o \
	$(BUILD)/userland/base/libc/timer.o \
	$(BUILD)/userland/base/libc/shm.o \
	$(BUILD)/userland/base/libc/semaphore.o \
	$(BUILD)/userland/base/libc/mqueue.o \
	$(BUILD)/userland/base/libc/dlfcn.o \
	$(BUILD)/userland/base/libc/static-tls.o \
	$(BUILD)/userland/base/libc/socket.o \
	$(BUILD)/userland/base/libc/resolver.o $(BUILD)/userland/base/libc/resolver-dns.o \
	$(BUILD)/userland/base/libc/signal.o \
	$(BUILD)/userland/base/libc/account.o $(BUILD)/userland/base/libc/crypt.o \
	$(BUILD)/userland/base/libc/utmpx.o \
	$(BUILD)/libc/heap.o \
	$(BUILD)/libc/string.o $(BUILD)/libc/ctype.o $(BUILD)/libc/locale.o \
	$(BUILD)/libc/wide.o $(BUILD)/libc/int64.o \
	$(BUILD)/libc/strto.o $(BUILD)/libc/format.o $(BUILD)/libc/stdio.o \
	$(patsubst %.c,$(BUILD)/%.o,$(ZEDBSD_LIBC_USER_EXTRA_SOURCES))
USER_CFLAGS := $(ZEDBSD_CFLAGS) -fno-builtin -ffunction-sections \
	-fdata-sections -msoft-float -mno-80387 -mno-fp-ret-in-387 \
	-mno-mmx -mno-sse -mno-sse2
USER_STACK_LDFLAGS := -z stack-size=0x100000
USER_ELF_CHECK := $(BUILD_TOOLS_DIR)/check-user-elf.noct
$(BUILD)/src/crt/crt0.o: src/crt/crt0.S include/hal/arch.h \
	include/hal/arch/i386.h
	@mkdir -p $(dir $@)
	$(CC) $(ZEDBSD_CPPFLAGS) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/userland/base/tests/syscall-smoke.o: \
	OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(BUILD)/userland/base/tests/syscall-smoke.o: OBJ_CFLAGS = $(USER_CFLAGS)
$(BUILD)/userland/base/tests/posix-r2.o: OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(BUILD)/userland/base/tests/posix-r2.o: OBJ_CFLAGS = $(USER_CFLAGS)
$(BUILD)/userland/base/tests/posix-r2-remaining.o: OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(BUILD)/userland/base/tests/posix-r2-remaining.o: OBJ_CFLAGS = $(USER_CFLAGS)

$(BUILD)/POSIX-R1.ELF: $(USER_LIBC_OBJS) \
	$(BUILD)/userland/base/tests/syscall-smoke.o $(PCAT)/user.ld \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) $(USER_ELF_CHECK)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) -T $(PCAT)/user.ld $(USER_LIBC_OBJS) \
		$(BUILD)/userland/base/tests/syscall-smoke.o \
		$(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(USER_ELF_CHECK) $@

$(BUILD)/POSIX-R2.ELF: $(USER_LIBC_OBJS) \
	$(BUILD)/userland/base/tests/posix-r2.o $(ZEDBSD_SOFTFLOAT_OBJECTS) \
	$(PCAT)/user.ld $(USER_ELF_CHECK)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) -T $(PCAT)/user.ld $(USER_LIBC_OBJS) \
		$(BUILD)/userland/base/tests/posix-r2.o \
		$(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(USER_ELF_CHECK) $@

$(BUILD)/POSIX-R2-REMAINING.ELF: $(USER_LIBC_OBJS) \
	$(BUILD)/userland/base/tests/posix-r2-remaining.o $(PCAT)/user.ld \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) $(USER_ELF_CHECK)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) -T $(PCAT)/user.ld $(USER_LIBC_OBJS) \
		$(BUILD)/userland/base/tests/posix-r2-remaining.o \
		$(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(USER_ELF_CHECK) $@

USER_SYSCTL_OBJ := $(BUILD)/userland/base/sysctl/main.o
$(USER_SYSCTL_OBJ): OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(USER_SYSCTL_OBJ): OBJ_CFLAGS = $(USER_CFLAGS)
$(BUILD)/bin/sysctl: $(USER_LIBC_OBJS) $(USER_SYSCTL_OBJ) \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) $(PCAT)/user.ld $(USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) -T $(PCAT)/user.ld $(USER_LIBC_OBJS) \
		$(USER_SYSCTL_OBJ) $(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$($(NOCT_NM) -u $@)" || { $(NOCT_NM) -u $@; exit 1; }
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(USER_ELF_CHECK) $@

USER_MOUNT_OBJ := $(BUILD)/userland/base/mount/main.o
$(USER_MOUNT_OBJ): OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(USER_MOUNT_OBJ): OBJ_CFLAGS = $(USER_CFLAGS)
$(BUILD)/bin/mount: $(USER_LIBC_OBJS) $(USER_MOUNT_OBJ) \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) $(PCAT)/user.ld $(USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) -T $(PCAT)/user.ld $(USER_LIBC_OBJS) \
		$(USER_MOUNT_OBJ) $(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$($(NOCT_NM) -u $@)" || { $(NOCT_NM) -u $@; exit 1; }
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(USER_ELF_CHECK) $@
$(BUILD)/bin/umount: $(BUILD)/bin/mount
	@mkdir -p $(dir $@)
	cp -f $< $@

USER_NOCT_GLUE_OBJS := $(BUILD)/userland/packages/lang/noct/runtime/main.o \
	$(BUILD)/userland/packages/lang/noct/runtime/memory.o \
	$(BUILD)/userland/packages/lang/noct/runtime/platform.o \
	$(BUILD)/userland/packages/lang/noct/runtime/env.o \
	$(BUILD)/userland/packages/lang/noct/runtime/napi.o \
	$(BUILD)/userland/packages/lang/noct/runtime/target.o
$(USER_NOCT_GLUE_OBJS): OBJ_CPPFLAGS = $(USER_NOCT_CPPFLAGS) -Iinclude -Isrc
$(USER_NOCT_GLUE_OBJS): OBJ_CFLAGS = $(USER_CFLAGS)
$(BUILD)/userland/packages/lang/noct/runtime/napi.o: userland/packages/lang/noct/runtime/napi.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_NOCT_CPPFLAGS) -Iinclude -Isrc $(USER_CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/userland/packages/lang/noct/runtime/target.o: userland/packages/lang/noct/runtime/target.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_NOCT_CPPFLAGS) -Iinclude -Isrc $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/NOCT.ELF: $(USER_LIBC_OBJS) $(USER_NOCT_GLUE_OBJS) \
	$(USER_NOCT_OBJECTS) $(ZEDBSD_SOFTFLOAT_OBJECTS) $(PCAT)/user.ld \
	$(USER_ELF_CHECK)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) -T $(PCAT)/user.ld $(USER_LIBC_OBJS) \
		$(USER_NOCT_GLUE_OBJS) $(USER_NOCT_OBJECTS) \
		$(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$($(NOCT_NM) -u $@)" || { $(NOCT_NM) -u $@; exit 1; }
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(USER_ELF_CHECK) $@

$(BUILD)/bin/noct: $(BUILD)/NOCT.ELF
	@mkdir -p $(dir $@)
	cp $< $@
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(USER_ELF_CHECK) $@

USER_SH_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(BUILD),sh)
USER_READLINE_OBJ := $(BUILD)/userland/base/libedit/readline.o
USER_READLINE_LIB := $(BUILD)/lib/libreadline.a
$(BUILD)/userland/base/libc/posix.o $(BUILD)/userland/base/libc/socket.o \
	$(USER_SH_OBJS) $(USER_READLINE_OBJ): OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS) \
	-Iuserland/base/libedit
$(BUILD)/userland/base/libc/posix.o $(BUILD)/userland/base/libc/socket.o \
	$(USER_SH_OBJS) $(USER_READLINE_OBJ): OBJ_CFLAGS = $(USER_CFLAGS)

$(USER_READLINE_LIB): $(USER_READLINE_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

USER_CURSES_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(BUILD),curses)
$(BUILD)/lib/libcurses.a: $(USER_CURSES_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/bin/sh: $(USER_LIBC_OBJS) $(USER_SH_OBJS) $(USER_READLINE_LIB) \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) $(PCAT)/user.ld $(USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) -T $(PCAT)/user.ld $(USER_LIBC_OBJS) \
		$(USER_SH_OBJS) $(USER_READLINE_LIB) $(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$($(NOCT_NM) -u $@)" || { $(NOCT_NM) -u $@; exit 1; }
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(USER_ELF_CHECK) $@

USER_NETTEST_OBJS := $(BUILD)/userland/base/nettest/main.o
$(USER_NETTEST_OBJS): OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(USER_NETTEST_OBJS): OBJ_CFLAGS = $(USER_CFLAGS)

$(BUILD)/bin/nettest: $(USER_LIBC_OBJS) $(USER_NETTEST_OBJS) \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) $(PCAT)/user.ld $(USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) -T $(PCAT)/user.ld $(USER_LIBC_OBJS) \
		$(USER_NETTEST_OBJS) $(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$($(NOCT_NM) -u $@)" || { $(NOCT_NM) -u $@; exit 1; }
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(USER_ELF_CHECK) $@

USER_NET_COMMANDS := $(USERLAND_SELECTED_NETWORK_PROGRAMS)
USER_NET_COMMAND_TARGETS := $(addprefix $(BUILD)/bin/,$(USER_NET_COMMANDS))
USER_NET_COMMON_OBJS := $(BUILD)/userland/base/net/netutil.o \
	$(BUILD)/userland/base/net/dhcp.o
USER_NET_COMMAND_OBJS := $(addsuffix /main.o, \
	$(addprefix $(BUILD)/userland/,$(USER_NET_COMMANDS)))
$(BUILD)/userland/base/libc/resolver.o $(BUILD)/userland/base/libc/resolver-dns.o \
	$(USER_NET_COMMON_OBJS) $(USER_NET_COMMAND_OBJS): \
	OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(BUILD)/userland/base/libc/resolver.o $(BUILD)/userland/base/libc/resolver-dns.o \
	$(USER_NET_COMMON_OBJS) $(USER_NET_COMMAND_OBJS): \
	OBJ_CFLAGS = $(USER_CFLAGS)

define PCAT_USER_NET_COMMAND
$(BUILD)/bin/$(1): $(USER_LIBC_OBJS) $(USER_NET_COMMON_OBJS) \
	$(call ZEDBSD_USERLAND_OBJECTS,$(BUILD),$(1)) $(ZEDBSD_SOFTFLOAT_OBJECTS) \
	$(PCAT)/user.ld $(USER_ELF_CHECK)
	@mkdir -p $$(dir $$@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) -T $(PCAT)/user.ld $(USER_LIBC_OBJS) \
		$(USER_NET_COMMON_OBJS) $(call ZEDBSD_USERLAND_OBJECTS,$(BUILD),$(1)) \
		$(ZEDBSD_SOFTFLOAT_OBJECTS) -o $$@
	@test -z "$$$$($(NOCT_NM) -u $$@)" || { $(NOCT_NM) -u $$@; exit 1; }
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(USER_ELF_CHECK) $$@
endef
$(foreach command,$(USER_NET_COMMANDS),\
	$(eval $(call PCAT_USER_NET_COMMAND,$(command))))
USER_BASIC_COMMON_OBJ := $(BUILD)/userland/base/common/command.o $(BUILD)/userland/base/common/pager.o
USER_BASIC_COMMAND_OBJS := $(addsuffix /main.o, \
	$(addprefix $(BUILD)/userland/,$(USER_BASIC_COMMANDS)))
$(USER_BASIC_COMMON_OBJ) $(USER_BASIC_COMMAND_OBJS): OBJ_CPPFLAGS = $(ZEDBSD_CPPFLAGS)
$(USER_BASIC_COMMON_OBJ) $(USER_BASIC_COMMAND_OBJS): OBJ_CFLAGS = $(USER_CFLAGS)

define PCAT_USER_BASIC_COMMAND
$(BUILD)/bin/$(1): $(USER_LIBC_OBJS) $(USER_BASIC_COMMON_OBJ) \
	$(call ZEDBSD_USERLAND_OBJECTS,$(BUILD),$(1)) $(ZEDBSD_SOFTFLOAT_OBJECTS) \
	$(PCAT)/user.ld $(USER_ELF_CHECK)
	@mkdir -p $$(dir $$@)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static -z max-page-size=4096 \
		$(USER_STACK_LDFLAGS) -T $(PCAT)/user.ld $(USER_LIBC_OBJS) \
		$(USER_BASIC_COMMON_OBJ) $(call ZEDBSD_USERLAND_OBJECTS,$(BUILD),$(1)) \
		$(ZEDBSD_SOFTFLOAT_OBJECTS) -o $$@
	@test -z "$$$$($(NOCT_NM) -u $$@)" || { $(NOCT_NM) -u $$@; exit 1; }
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(USER_ELF_CHECK) $$@
endef
$(foreach command,$(USER_BASIC_COMMANDS),\
	$(eval $(call PCAT_USER_BASIC_COMMAND,$(command))))
# ----------------------------------------------------------------------
# First-generation ELF dynamic linker and shared libc.  Production programs
# remain on the static rules above until the dynamic smoke test is a mandatory
# image gate.

DYNAMIC_DIR := $(BUILD)/dynamic
DYNAMIC_CPPFLAGS := -nostdinc -I. -Iinclude -Iinclude/uapi -Ilibc/include \
	-DHAL_ARCH_I386 -DZEDBSD_DYNAMIC_LIBC
DYNAMIC_CFLAGS := -m32 -march=i386 -Os -ffreestanding -fPIC -fno-builtin \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-ftls-model=global-dynamic -Wall -Wextra -Werror -msoft-float \
	-mno-80387 -mno-fp-ret-in-387 \
	-mno-mmx -mno-sse -mno-sse2
DYNAMIC_LIBC_SOURCES := userland/base/libc/posix.c userland/base/libc/poll.c \
	userland/base/libc/termios.c userland/base/libc/pthread.c userland/base/libc/timer.c userland/base/libc/shm.c \
	userland/base/libc/semaphore.c userland/base/libc/mqueue.c userland/base/libc/dlfcn.c \
	userland/base/libc/socket.c \
	userland/base/libc/resolver.c userland/base/libc/resolver-dns.c \
	userland/base/libc/signal.c userland/base/libc/account.c userland/base/libc/crypt.c \
	userland/base/libc/utmpx.c libc/heap.c libc/string.c libc/ctype.c \
	libc/locale.c libc/wide.c \
	libc/int64.c libc/strto.c libc/format.c libc/stdio.c \
	$(ZEDBSD_LIBC_USER_EXTRA_SOURCES)
DYNAMIC_LIBC_OBJS := $(patsubst %.c,$(DYNAMIC_DIR)/obj/%.o,\
	$(DYNAMIC_LIBC_SOURCES)) $(DYNAMIC_DIR)/obj/userland/base/libc/syscall.o
DYNAMIC_RTLD_OBJS := $(DYNAMIC_DIR)/obj/userland/base/rtld/entry.o \
	$(DYNAMIC_DIR)/obj/userland/base/rtld/rtld.o \
	$(DYNAMIC_DIR)/obj/userland/base/rtld/string.o
DYNAMIC_SOFTFLOAT_DIR := $(DYNAMIC_DIR)/softfloat
DYNAMIC_COMPILER_RT_OBJS := $(addprefix $(DYNAMIC_SOFTFLOAT_DIR)/,\
	zed-softfloat.o compiler-runtime.o)
DYNAMIC_LIBM_OBJ := $(DYNAMIC_SOFTFLOAT_DIR)/math.o
DYNAMIC_FLOAT_PARSE_OBJ := $(DYNAMIC_SOFTFLOAT_DIR)/float-parse.o
DYNAMIC_SOFTFLOAT_OBJS := $(DYNAMIC_COMPILER_RT_OBJS) \
	$(DYNAMIC_LIBM_OBJ) $(DYNAMIC_FLOAT_PARSE_OBJ)
DYNAMIC_LIBC_OBJS += $(DYNAMIC_SOFTFLOAT_OBJS)

$(DYNAMIC_DIR)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_CPPFLAGS) $(DYNAMIC_CFLAGS) -MMD -MP -c $< -o $@

$(DYNAMIC_DIR)/obj/userland/base/libc/syscall.o: \
	userland/base/libc/syscall-i386.S include/hal/arch.h \
	include/hal/arch/i386.h
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_CPPFLAGS) -m32 -c $< -o $@

$(DYNAMIC_DIR)/obj/userland/base/rtld/entry.o: userland/base/rtld/entry-i386.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@

$(DYNAMIC_SOFTFLOAT_DIR)/%.o: src/softfloat/%.c \
	src/softfloat/zed-softfloat.h
	@mkdir -p $(dir $@)
	$(CC) -nostdinc -Ilibc/include -Iinclude/uapi -I. $(DYNAMIC_CFLAGS) \
		-mlong-double-64 -c $< -o $@

$(DYNAMIC_FLOAT_PARSE_OBJ): libc/float-parse.c \
	src/softfloat/zed-softfloat.h
	@mkdir -p $(dir $@)
	$(CC) -nostdinc -Ilibc/include -Iinclude/uapi -I. $(DYNAMIC_CFLAGS) \
		-mlong-double-64 -c $< -o $@

$(DYNAMIC_LIBM_OBJ): libc/math.c src/softfloat/zed-softfloat.h
	@mkdir -p $(dir $@)
	$(CC) -nostdinc -Ilibc/include -Iinclude/uapi -I. $(DYNAMIC_CFLAGS) \
		-mlong-double-64 -c $< -o $@

$(DYNAMIC_DIR)/ld.so: $(DYNAMIC_RTLD_OBJS)
	$(LD) -m elf_i386 -shared -Bsymbolic -e _rtld_start --hash-style=sysv \
		-z now -z relro -z separate-code $(DYNAMIC_RTLD_OBJS) -o $@

$(DYNAMIC_DIR)/libc.so: $(DYNAMIC_LIBC_OBJS)
	$(LD) -m elf_i386 -shared -soname libc.so --hash-style=both -z now \
		-z relro -z separate-code $(USER_STACK_LDFLAGS) \
		$(DYNAMIC_LIBC_OBJS) -o $@

$(DYNAMIC_DIR)/obj/src/crt/crt1.o: src/crt/crt1-i386.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@

$(DYNAMIC_DIR)/alt/rpathdep.so: \
	$(DYNAMIC_DIR)/obj/userland/base/tests/rpathdep.o $(DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -shared -soname rpathdep.so --hash-style=gnu \
		-z now -z relro -z separate-code $< -o $@

$(DYNAMIC_DIR)/tlstest.so: \
	$(DYNAMIC_DIR)/obj/userland/base/tests/tlstest.o \
	$(DYNAMIC_DIR)/alt/rpathdep.so $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_i386 -shared -soname tlstest.so --hash-style=gnu \
		-z now -z relro -z separate-code --enable-new-dtags \
		-rpath '$$ORIGIN/alt' \
		$(DYNAMIC_DIR)/obj/userland/base/tests/tlstest.o \
		-L$(DYNAMIC_DIR)/alt -l:rpathdep.so -o $@

$(DYNAMIC_DIR)/rpathtest.so: \
	$(DYNAMIC_DIR)/obj/userland/base/tests/rpathtest.o \
	$(DYNAMIC_DIR)/alt/rpathdep.so $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_i386 -shared -soname rpthtest.so --hash-style=gnu \
		-z now -z relro -z separate-code --disable-new-dtags \
		-rpath '$$ORIGIN/alt' $< -L$(DYNAMIC_DIR)/alt \
		-l:rpathdep.so -o $@

$(DYNAMIC_DIR)/verstest.so: \
	$(DYNAMIC_DIR)/obj/userland/base/tests/versiontest.o \
	userland/base/tests/versiontest.map $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_i386 -shared -soname verstest.so --hash-style=gnu \
		-z now -z relro -z separate-code \
		--version-script=userland/base/tests/versiontest.map $< -o $@

$(DYNAMIC_DIR)/versuse.so: \
	$(DYNAMIC_DIR)/obj/userland/base/tests/versionuse.o \
	$(DYNAMIC_DIR)/verstest.so $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_i386 -shared -soname versuse.so --hash-style=gnu \
		-z now -z relro -z separate-code $< -L$(DYNAMIC_DIR) \
		-l:verstest.so -o $@

$(DYNAMIC_DIR)/dyntest: $(DYNAMIC_DIR)/obj/src/crt/crt1.o \
	$(DYNAMIC_DIR)/obj/userland/base/tests/dyntest.o $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/ld.so $(DYNAMIC_DIR)/tlstest.so \
	$(DYNAMIC_DIR)/versuse.so
	$(CC) -m32 -nostdlib -pie -Wl,--no-relax,--hash-style=sysv,-z,now,-z,relro \
		-Wl,-z,separate-code,-z,stack-size=0x100000,--allow-shlib-undefined \
		-Wl,--dynamic-linker=/lib/ld.so \
		$(DYNAMIC_DIR)/obj/src/crt/crt1.o \
		$(DYNAMIC_DIR)/obj/userland/base/tests/dyntest.o \
		-L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) -l:libc.so -o $@

dynamic-userland-check: $(DYNAMIC_DIR)/ld.so $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/dyntest $(DYNAMIC_DIR)/tlstest.so \
	$(DYNAMIC_DIR)/rpathtest.so $(DYNAMIC_DIR)/verstest.so \
	$(DYNAMIC_DIR)/versuse.so tools/build/check-dynamic-elf.py
	$(PYTHON) tools/build/check-dynamic-elf.py --machine i386 --role interpreter $(DYNAMIC_DIR)/ld.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine i386 --role libc $(DYNAMIC_DIR)/libc.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine i386 --role module $(DYNAMIC_DIR)/tlstest.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine i386 --role rpath-module $(DYNAMIC_DIR)/rpathtest.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine i386 --role version-definition $(DYNAMIC_DIR)/verstest.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine i386 --role version-consumer $(DYNAMIC_DIR)/versuse.so
	$(PYTHON) tools/build/check-dynamic-elf.py --machine i386 --role program $(DYNAMIC_DIR)/dyntest
	@echo "zedBSD i386 dynamic userland artifacts: PASS"
.PHONY: dynamic-userland-check

$(BUILD)/hdd-image.img: $(BUILD)/bios-hdd-image.img
	cp -f $< $@

$(BUILD)/zedbsd-grub.iso: $(BUILD)/vmunix \
	platform/pcat/tools/make-pcat-grub-iso.sh
	platform/pcat/tools/make-pcat-grub-iso.sh $@ $(BUILD)/vmunix

$(BUILD)/tests/pcat-mbr-host-test: tests/pcat-mbr-host-test.c \
	tests/disk-host-stubs.c \
	src/kern/buf.c src/kern/disk.c src/kern/partition.c src/kern/mbr-partition.c
	@mkdir -p $(dir $@)
	$(HOST_TEST_CC) -Iinclude -Iinclude/uapi -Isrc src/kern/buf.c src/kern/disk.c src/kern/partition.c \
		src/kern/mbr-partition.c tests/disk-host-stubs.c $< -o $@

pcat-mbr-host-test: $(BUILD)/tests/pcat-mbr-host-test
	$(BUILD)/tests/pcat-mbr-host-test

HOST_TEST_BINARIES += $(BUILD)/tests/pcat-mbr-host-test
.PHONY: pcat-mbr-host-test

hal-pcat-compile: $(HAL_PCAT_OBJS)
	@echo "HAL i386/PCAT compile check: PASS"
kern-compile: $(KERN_OBJS)
	@echo "zedBSD kernel glue compile check: PASS"
CHECK_RUN_TARGETS += hal-pcat-compile kern-compile
.PHONY: hal-pcat-compile kern-compile
