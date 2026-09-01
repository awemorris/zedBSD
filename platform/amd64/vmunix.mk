# zedBSD amd64/PC-AT bootstrap rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

AMD64_PLATFORM := platform/amd64
BIOS_LOADER := bootloader/pcat
UEFI_LOADER := bootloader/uefi
AMD64_ZEDBSD_CONFIG := $(AMD64_PLATFORM)/zedbsd.cfg
AMD64_NATIVE_ZEDBSD_CONFIG := $(AMD64_PLATFORM)/zedbsd-native.cfg
AMD64_UEFI_CONFIGURED_IMAGES := \
	$(BUILD)/bios-hdd-image.img \
	$(BUILD)/bios-hdd-image-fragmented.img \
	$(BUILD)/deferred-stub-qemu.img \
	$(BUILD)/phase19-qemu.img \
	$(BUILD)/phase20-qemu.img \
	$(BUILD)/posix-phase2-qemu.img \
	$(BUILD)/posix-phase3-qemu.img \
	$(BUILD)/posix-phase4-qemu.img \
	$(BUILD)/posix-phase5-qemu.img \
	$(BUILD)/posix-phase6-qemu.img \
	$(BUILD)/posix-phase7-qemu.img \
	$(BUILD)/posix-phase8-qemu.img \
	$(BUILD)/posix-phase85-qemu.img
$(AMD64_UEFI_CONFIGURED_IMAGES): $(AMD64_ZEDBSD_CONFIG) \
	$(ZEDBSD_IMAGE_HOST)
.DELETE_ON_ERROR: $(AMD64_UEFI_CONFIGURED_IMAGES)
.DELETE_ON_ERROR: $(BUILD)/ufs-root-hdd-image.img \
	$(BUILD)/hdd-image.img

# Variant is an image-composition input only.  This content-stable stamp
# invalidates a previously published hdd-image.img without leaking the
# selection into kernel, userland, or loader compilation.
AMD64_IMAGE_CONTRACT_STAMP := $(BUILD)/.disk-image-contract
AMD64_IMAGE_STAGE1 := $(if $(filter bios,$(ZEDBSD_VARIANT)),\
	$(BUILD)/bootloader/stage1-native.bin,$(BUILD)/bootloader/stage1.bin)
.PHONY: FORCE_AMD64_IMAGE_CONTRACT
FORCE_AMD64_IMAGE_CONTRACT:

$(AMD64_IMAGE_CONTRACT_STAMP): FORCE_AMD64_IMAGE_CONTRACT
	@mkdir -p $(dir $@)
	@value='layout=$(ZEDBSD_VARIANT)'; \
		if ! test -f $@ || ! grep -Fqx -- "$$value" $@; then \
			printf '%s\n' "$$value" > $@.tmp; \
			mv $@.tmp $@; \
		fi

define AMD64_VALIDATE_GPT_IMAGE
	$(NOCT) --path=tools/build platform/amd64/tools/check-amd64-gpt-image.noct \
		--layout $(ZEDBSD_VARIANT) \
		--machine pcat --stage1 $(AMD64_IMAGE_STAGE1) \
		--stage2 $(BUILD)/bootloader/stage2-chain.bin \
		--partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
		--bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE --kernel $(BUILD)/vmunix \
		--bootx64 $(BUILD)/uefi/BOOTX64.EFI \
		--zedbsd-config $(AMD64_ZEDBSD_CONFIG) \
		--arch-profile amd64 --arch-image $(AMD64_ARCH_UFS_IMAGE) \
		--arch-format ufs --data-image $(DATA_IMAGE) \
		--swapfile $(SWAP_IMAGE) $(1)
endef
EFI_CC ?= x86_64-w64-mingw32-gcc
EFI_LD ?= x86_64-w64-mingw32-ld
EFI_NM ?= x86_64-w64-mingw32-nm
EFI_CFLAGS := -std=c11 -ffreestanding -fshort-wchar -mno-red-zone \
	-fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -fno-ident -ffunction-sections -fdata-sections \
	-Os -Wall -Wextra -Werror -I.

AMD64_CPPFLAGS := -nostdinc -Iinclude -Iinclude/uapi -Isrc -I. \
	-Ilibc/include -DHAL_ARCH_AMD64 -DHAL_BOARD_PCAT -DHAL_PCAT_DEBUGCON \
	-DZEDBSD_USER_ABI_LP64 \
	-DPCAT_VGA_APERTURE_ADDRESS=0xffffffff800a0000ULL \
	-DPCAT_CIRRUS_APERTURE_ADDRESS=0xffffffffc0000000ULL
AMD64_CPPFLAGS += $(ZEDBSD_CONFIG_CPPFLAGS)
AMD64_CFLAGS := -m64 -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
	-ffreestanding -fno-pic -fno-pie -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror
AMD64_KERNEL_LIBC_CFLAGS := $(filter-out -mgeneral-regs-only,$(AMD64_CFLAGS))

AMD64_HAL_SOURCES := src/hal/x86/rtc.c src/hal/x86/boot-parameters.c \
	src/hal/amd64/asm.c src/hal/amd64/lib.c \
	src/hal/amd64/page.c src/hal/amd64/space.c \
	src/hal/amd64/acpi-window.c src/hal/amd64/cmain.c \
	src/hal/amd64/descriptor.c src/hal/amd64/int.c src/hal/amd64/irq.c \
	src/hal/amd64/msi-source.c \
	src/hal/amd64/task.c src/hal/amd64/percpu.c src/hal/amd64/smp.c \
	src/hal/amd64/bsp-pcat/boot.c \
	src/hal/amd64/bsp-pcat/handoff-validation.c \
	src/hal/amd64/bsp-pcat/cons.c \
	src/hal/amd64/bsp-pcat/pic.c src/hal/amd64/bsp-pcat/clock.c \
	src/hal/amd64/bsp-pcat/acpi.c src/hal/amd64/bsp-pcat/lapic.c \
	src/hal/amd64/bsp-pcat/early-init-policy.c \
	src/hal/amd64/bsp-pcat/mcfg.c \
	src/hal/amd64/bsp-pcat/ioapic.c
AMD64_HAL_ASM := src/hal/amd64/locore.S src/hal/amd64/trap.S \
	src/hal/amd64/dispatch.S src/hal/amd64/ap-trampoline.S
AMD64_HAL_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(AMD64_HAL_SOURCES)) \
	$(patsubst %.S,$(BUILD)/%.o,$(AMD64_HAL_ASM))

AMD64_USB_HCD_SOURCES :=
ifeq ($(CONFIG_DRIVER_PCI_UHCI),y)
AMD64_USB_HCD_SOURCES += src/drivers/pci-uhci.c
endif
ifeq ($(CONFIG_DRIVER_PCI_EHCI),y)
AMD64_USB_HCD_SOURCES += src/drivers/pci-ehci.c
endif
ifeq ($(CONFIG_DRIVER_PCI_XHCI),y)
AMD64_USB_HCD_SOURCES += src/drivers/pci-xhci.c
endif
AMD64_USB_CLASS_SOURCES :=
ifeq ($(CONFIG_DRIVER_USB_STORAGE),y)
AMD64_USB_CLASS_SOURCES += src/drivers/usb-storage.c
endif
AMD64_NVME_SOURCES :=
ifeq ($(CONFIG_DRIVER_PCI_NVME),y)
AMD64_NVME_SOURCES += src/drivers/pci-nvme.c
endif
ifeq ($(CONFIG_DRIVER_USB_CDC_NCM),y)
AMD64_USB_CLASS_SOURCES += src/drivers/usb-cdc-ncm.c \
	src/drivers/usb-cdc-ncm-net.c
endif
ifeq ($(CONFIG_DRIVER_USB_CDC_ECM),y)
AMD64_USB_CLASS_SOURCES += src/drivers/usb-cdc-ecm.c
endif
ifeq ($(CONFIG_DRIVER_USB_HID),y)
AMD64_USB_CLASS_SOURCES += src/drivers/usb-hid.c
endif
ifeq ($(CONFIG_DRIVER_USB_RTL8822BU),y)
AMD64_USB_CLASS_SOURCES += src/drivers/rtl8822b.c \
	src/drivers/rtl8822b-security.c \
	src/drivers/usb-rtl8822bu.c
endif
ifeq ($(CONFIG_KERNEL_USB_HID_CHECKPOINT),y)
AMD64_USB_CLASS_SOURCES += src/drivers/usb-hid-checkpoint.c
endif

AMD64_KERNEL_SOURCES := \
	src/kern/main.c \
	$(KERN_FAT_SOURCES) src/kern/inode.c src/kern/file.c \
	src/kern/namecache.c src/kern/namei.c src/kern/mount.c \
	src/kern/rootfs.c src/kern/tmpfs.c src/kern/overlayfs.c src/kern/vfs.c \
	src/kern/swap.c src/kern/backing-claim.c src/kern/swap-source.c \
	src/kern/swap-control.c src/kern/swap-boot.c \
	src/kern/swap-fat.c \
	src/kern/vm-reclaim.c src/kern/buf.c src/kern/sysctl.c \
	src/kern/resource.c src/kern/poll.c src/kern/usync.c \
	src/kern/resource-limit.c \
	src/kern/disk.c src/kern/partition.c \
	src/drivers/loop.c src/drivers/dma.c src/drivers/pci.c \
	src/drivers/pci-pcat.c src/drivers/usb.c $(AMD64_USB_HCD_SOURCES) \
	$(AMD64_USB_CLASS_SOURCES) \
	$(AMD64_NVME_SOURCES) \
	src/drivers/pcat-ide.c src/drivers/dp8390.c \
	src/drivers/pcat-ne2000.c src/drivers/hid/ps2-mouse.c \
	src/drivers/disklabel/mbr.c src/drivers/disklabel/gpt.c \
	src/drivers/disklabel/pcat-auto.c src/kern/platform/pcat.c \
	src/kern/panic.c src/kern/entry.c src/kern/clock.c \
	src/kern/process-timer.c src/kern/klog.c \
	src/kern/test-checkpoint.c \
	src/kern/lock.c src/kern/waitq.c \
	src/kern/process.c src/kern/thread.c src/kern/sched.c \
	src/kern/vm-lock.c src/kern/vmspace.c src/kern/vm-object.c src/kern/vm-commit.c \
	src/kern/filedesc.c \
	src/kern/record-lock.c \
	src/kern/pipe.c src/kern/cred.c src/kern/signal.c \
	src/kern/cwdinfo.c src/kern/elf.c src/kern/exec.c \
	src/kern/user-probe.c src/kern/syscall.c src/kern/uaccess.c \
	src/kern/cdev.c src/kern/devfs.c src/drivers/fs/console.c \
	src/drivers/input-queue.c src/drivers/input-capability.c \
	src/drivers/input-device.c src/drivers/input-subscriber.c \
	src/drivers/input-keymap.c src/drivers/hid/hid-report.c \
	src/kern/locale-record.c \
	src/kern/tty.c \
	src/kern/system-swap-device.c \
	src/kern/system-device.c src/kern/shutdown.c \
	src/drivers/graphics/pcat/vgafont.c src/kern/init.c
ifeq ($(CONFIG_DRIVER_GRAPHICS_DEVICE),y)
AMD64_KERNEL_SOURCES += \
	src/drivers/graphics/pcat/device.c \
	src/drivers/graphics/pcat/backend.c \
	src/drivers/graphics/pcat/font.c
endif
AMD64_KERNEL_SOURCES += $(KERN_NET_SOURCES) $(KERN_BLOCK_IDENTITY_SOURCES) \
	$(KERN_UFS1_SOURCES) $(KERN_UFS2_SOURCES)
AMD64_KERNEL_SOURCES += $(KERN_BOOT_SOURCES)
ifeq ($(CONFIG_KERNEL_TEST_CHECKPOINTS),y)
AMD64_KERNEL_SOURCES += plan/ws004-hardware/tests/pci-msi-qemu.c
endif
AMD64_KERNEL_SOURCES += $(KERN_ACL_SOURCES)
AMD64_KERNEL_SOURCES += $(KERN_QUOTA_SOURCES)
AMD64_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kern64/%.o,\
	$(AMD64_KERNEL_SOURCES))
AMD64_KERNEL_LIBC_OBJS := $(patsubst %.c,$(BUILD)/kern64/%.o,\
	$(ZEDBSD_LIBC_SOURCES))
AMD64_VMUNIX_OBJS := $(AMD64_HAL_OBJS) $(AMD64_KERNEL_OBJS) \
	$(AMD64_KERNEL_LIBC_OBJS)
ifneq ($(strip $(ZEDBSD_CONFIG)),)
$(AMD64_VMUNIX_OBJS): $(ZEDBSD_CONFIG)
endif
$(BUILD)/kern64/src/kern/vfs.o \
	$(BUILD)/kern64/src/kern/platform/pcat.o: \
	$(ZEDBSD_GRAPHICS_CONFIG_STAMP)
$(BUILD)/kern64/src/kern/platform/pcat.o: \
	$(ZEDBSD_PLATFORM_CONFIG_STAMP)

vmunix: $(BUILD)/vmunix

$(BUILD)/src/hal/amd64/%.o: src/hal/amd64/%.S
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -D_ASM_SRC_ -c $< -o $@

$(BUILD)/src/hal/amd64/%.o: src/hal/amd64/%.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -MMD -MP -c $< -o $@

# Shared x86 HAL sources must use the amd64 flags as well.  Without this
# rule the generic i386 pattern can leave a 32-bit object in build/amd64.
$(BUILD)/src/hal/x86/%.o: src/hal/x86/%.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kern64/src/kern/%.o: src/kern/%.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kern64/src/drivers/%.o: src/drivers/%.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kern64/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_KERNEL_LIBC_CFLAGS) -fno-builtin \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/vmunix: $(AMD64_VMUNIX_OBJS) $(ZEDBSD_GRAPHICS_CONFIG_STAMP) \
	$(AMD64_PLATFORM)/vmunix.ld \
	platform/amd64/tools/check-amd64-vmunix.noct
	$(LD) -m elf_x86_64 --gc-sections -z max-page-size=4096 \
		-T $(AMD64_PLATFORM)/vmunix.ld -nostdlib $(AMD64_VMUNIX_OBJS) -o $@
	$(NOCT) --path=tools/build platform/amd64/tools/check-amd64-vmunix.noct $@

$(BUILD)/bootloader/stage1.o: $(BIOS_LOADER)/stage1.S \
	bootloader/include/disk-layout.inc bootloader/include/stage2-header.inc
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -DZBL_STAGE2_LBA_OVERRIDE=34 \
		-x assembler-with-cpp -c $< -o $@

$(BUILD)/bootloader/stage1.elf: $(BUILD)/bootloader/stage1.o \
	$(BIOS_LOADER)/stage1.ld
	$(LD) -m elf_i386 -T $(BIOS_LOADER)/stage1.ld $< -o $@

$(BUILD)/bootloader/stage1.bin: $(BUILD)/bootloader/stage1.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 512

# The GPT hybrid reserves LBA 34 for its chain sector.  The separate native
# BIOS image retains the legacy LBA-1 chain sector and therefore needs a
# stage-1 artifact built without the GPT override.
$(BUILD)/bootloader/stage1-native.o: $(BIOS_LOADER)/stage1.S \
	bootloader/include/disk-layout.inc bootloader/include/stage2-header.inc
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

$(BUILD)/bootloader/stage1-native.elf: $(BUILD)/bootloader/stage1-native.o \
	$(BIOS_LOADER)/stage1.ld
	$(LD) -m elf_i386 -T $(BIOS_LOADER)/stage1.ld $< -o $@

$(BUILD)/bootloader/stage1-native.bin: $(BUILD)/bootloader/stage1-native.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 512

$(BUILD)/bootloader/stage2-chain.o: $(BIOS_LOADER)/stage2-chain.S \
	bootloader/include/stage2-header.inc
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

$(BUILD)/bootloader/stage2-chain.elf: $(BUILD)/bootloader/stage2-chain.o \
	$(BIOS_LOADER)/stage2.ld
	$(LD) -m elf_i386 -T $(BIOS_LOADER)/stage2.ld $< -o $@

$(BUILD)/bootloader/stage2-chain.raw: $(BUILD)/bootloader/stage2-chain.elf
	$(OBJCOPY) -O binary -j .text $< $@

$(BUILD)/bootloader/stage2-chain.bin: $(BUILD)/bootloader/stage2-chain.raw \
	tools/build/finalize-bios-stage2.noct
	$(NOCT) --path=tools/build tools/build/finalize-bios-stage2.noct --machine pcat $< $@

# Compatibility alias for focused fixtures that predate the chain-loader
# name. Its new chain-specific prerequisite forces an incremental tree with
# the retired direct-kernel stage2.o/bin to regenerate before use.
$(BUILD)/bootloader/stage2.bin: $(BUILD)/bootloader/stage2-chain.bin
	cp -f $< $@.tmp
	mv -f $@.tmp $@

$(BUILD)/bootloader/partition-pbr.o: $(BIOS_LOADER)/partition-pbr.S \
	bootloader/include/stage2-header.inc
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

$(BUILD)/bootloader/partition-pbr.elf: $(BUILD)/bootloader/partition-pbr.o
	$(LD) -m elf_i386 -Ttext=0 -e _start $< -o $@

$(BUILD)/bootloader/partition-pbr.bin: $(BUILD)/bootloader/partition-pbr.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 2048

$(BUILD)/bootloader/bootzbsd.o: $(BIOS_LOADER)/bootzbsd.S \
	$(BIOS_LOADER)/vbe.inc \
	bootloader/bios/fat-directory.h \
	bootloader/include/disk-layout.inc bootloader/include/stage2-header.inc \
	bootloader/include/mbr.inc bootloader/include/fat16.inc \
	bootloader/include/elf.inc bootloader/include/amd64-handoff.h \
	bootloader/include/boot-parameter-handoff.h \
	bootloader/include/boot-parameter-record.inc \
	bootloader/uefi/zedbsd-config.h \
	include/boot/parameter-handoff.h include/boot/parameters.h
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

$(BUILD)/bootloader/bios-zedbsd-config.i386.o: \
	bootloader/uefi/zedbsd-config.c bootloader/uefi/zedbsd-config.h \
	bootloader/include/boot-parameter-handoff.h include/boot/parameters.h
	@mkdir -p $(dir $@)
	$(CC) -m16 -march=i386 -mtune=i386 -Os -ffreestanding -fno-pic -fno-pie \
		-fno-stack-protector -fno-asynchronous-unwind-tables \
		-fno-unwind-tables -fno-builtin -Wall -Wextra -Werror -I. \
		-c $< -o $@


$(BUILD)/bootloader/bios-fat-directory.i386.o: \
	bootloader/bios/fat-directory.c bootloader/bios/fat-directory.h
	@mkdir -p $(dir $@)
	$(CC) -m16 -march=i386 -mtune=i386 -Os -ffreestanding -fno-pic -fno-pie \
		-fno-stack-protector -fno-asynchronous-unwind-tables \
		-fno-unwind-tables -fno-builtin -Wall -Wextra -Werror -I. \
		-c $< -o $@

AMD64_BOOTZBSD_HELPERS := $(BUILD)/bootloader/bios-zedbsd-config.i386.o \
	$(BUILD)/bootloader/bios-fat-directory.i386.o

$(BUILD)/bootloader/bootzbsd.elf: $(BUILD)/bootloader/bootzbsd.o \
	$(AMD64_BOOTZBSD_HELPERS) $(BIOS_LOADER)/bootzbsd.ld
	$(LD) -m elf_i386 -T $(BIOS_LOADER)/bootzbsd.ld \
		$(filter %.o,$^) -o $@

$(BUILD)/bootloader/bootzbsd.raw: $(BUILD)/bootloader/bootzbsd.elf
	$(OBJCOPY) -O binary -j .text $< $@

$(BUILD)/bootloader/bootzbsd.bin: $(BUILD)/bootloader/bootzbsd.raw \
	tools/build/finalize-bios-stage2.noct
	$(NOCT) --path=tools/build tools/build/finalize-bios-stage2.noct --machine pcat $< $@

$(BUILD)/bootloader/BOOTZBSD.EXE: $(BUILD)/bootloader/bootzbsd.bin \
	tools/build/make-mz-exe.noct
	$(NOCT) --path=tools/build tools/build/make-mz-exe.noct --entry 0x20 $< $@

$(BUILD)/uefi/bootx64.o: $(UEFI_LOADER)/bootx64.c \
	$(UEFI_LOADER)/include/uefi.h $(UEFI_LOADER)/elf64.h \
	$(UEFI_LOADER)/framebuffer.h $(UEFI_LOADER)/memory-map.h \
	$(UEFI_LOADER)/volume-discovery.h \
	$(UEFI_LOADER)/zedbsd-config.h \
	bootloader/include/amd64-handoff.h \
	bootloader/include/boot-parameter-handoff.h \
	include/boot/parameter-handoff.h include/boot/parameters.h
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/volume-discovery.o: $(UEFI_LOADER)/volume-discovery.c \
	$(UEFI_LOADER)/volume-discovery.h
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/framebuffer.o: $(UEFI_LOADER)/framebuffer.c \
	$(UEFI_LOADER)/framebuffer.h
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/zedbsd-config.o: $(UEFI_LOADER)/zedbsd-config.c \
	$(UEFI_LOADER)/zedbsd-config.h \
	bootloader/include/boot-parameter-handoff.h \
	include/boot/parameter-handoff.h include/boot/parameters.h
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/elf64.o: $(UEFI_LOADER)/elf64.c $(UEFI_LOADER)/elf64.h
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/memory-map.o: $(UEFI_LOADER)/memory-map.c \
	$(UEFI_LOADER)/memory-map.h $(UEFI_LOADER)/include/uefi.h \
	bootloader/include/amd64-handoff.h
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/src/hal/amd64/locore.o: bootloader/include/amd64-handoff.h \
	bootloader/include/boot-parameter-handoff.h \
	include/boot/parameter-handoff.h include/boot/parameters.h

$(BUILD)/uefi/transition.o: $(UEFI_LOADER)/transition.S
	@mkdir -p $(dir $@)
	$(EFI_CC) -m64 -mno-red-zone -c $< -o $@

$(BUILD)/uefi/BOOTX64.EFI: $(BUILD)/uefi/bootx64.o \
	$(BUILD)/uefi/elf64.o $(BUILD)/uefi/framebuffer.o \
	$(BUILD)/uefi/memory-map.o \
	$(BUILD)/uefi/volume-discovery.o $(BUILD)/uefi/zedbsd-config.o \
	$(BUILD)/uefi/transition.o \
	platform/amd64/tools/check-bootx64.noct
	$(EFI_LD) -mi386pep --subsystem 10 --entry efi_main --image-base 0 \
		--gc-sections --enable-reloc-section --no-insert-timestamp \
		$(filter %.o,$^) -o $@
	@test -z "$$($(EFI_NM) -u $@ | grep -Ev \
		' (__bss_start__|__bss_end__|__end__|___tls_start__|___tls_end__)$$')" \
		|| { $(EFI_NM) -u $@; exit 1; }
	$(NOCT) --path=tools/build platform/amd64/tools/check-bootx64.noct $@

AMD64_USER_CPPFLAGS := -nostdinc -Iinclude -Iinclude/uapi -Isrc -I. \
	-Ilibc/include -DHAL_ARCH_AMD64 -DZEDBSD_USER_ABI_LP64
AMD64_USER_CFLAGS := -m64 -march=x86-64 -mno-red-zone -ffreestanding \
	-fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -fno-builtin -fno-common -ffunction-sections \
	-fdata-sections -Os -Wall -Wextra -Werror
AMD64_USER_RUNTIME_SOURCES := userland/base/libc/posix.c userland/base/libc/dlfcn.c userland/base/libc/static-tls.c userland/base/libc/poll.c \
	userland/base/libc/termios.c \
	userland/base/libc/pthread.c \
	userland/base/libc/timer.c \
	userland/base/libc/shm.c \
	userland/base/libc/semaphore.c \
	userland/base/libc/mqueue.c \
	userland/base/libc/socket.c userland/base/libc/resolver.c \
	userland/base/libc/resolver-dns.c \
	userland/base/libc/signal.c userland/base/libc/account.c userland/base/libc/crypt.c \
	userland/base/libc/utmpx.c libc/heap.c libc/string.c libc/ctype.c \
	libc/locale.c libc/wide.c \
	libc/int64.c libc/strto.c libc/format.c libc/stdio.c \
	$(ZEDBSD_LIBC_USER_EXTRA_SOURCES)
AMD64_USER_LIBC_OBJS := $(BUILD)/user64/src/crt/crt0-amd64.o \
	$(patsubst %.c,$(BUILD)/user64/%.o,$(AMD64_USER_RUNTIME_SOURCES))
AMD64_USER_NET_LIBC_OBJS := $(AMD64_USER_LIBC_OBJS)
AMD64_USER_NETTEST_OBJS := $(BUILD)/user64/userland/base/nettest/main.o
AMD64_USER_SH_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user64,sh)
AMD64_USER_READLINE_OBJ := $(BUILD)/user64/userland/base/libedit/readline.o
AMD64_USER_READLINE_LIB := $(BUILD)/lib/libreadline.a
AMD64_USER_ELF_CHECK := tools/build/check-user-elf.noct

$(BUILD)/user64/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) \
		-fno-strict-aliasing -MMD -MP -c $< -o $@

$(BUILD)/user64/src/crt/crt0-amd64.o: src/crt/crt0-amd64.S \
	include/hal/arch.h include/hal/arch/amd64.h
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_USER_CPPFLAGS) $(AMD64_USER_CFLAGS) -c $< -o $@

$(AMD64_USER_READLINE_OBJ): AMD64_USER_CPPFLAGS += -Iuserland/base/libedit
$(AMD64_USER_SH_OBJS): AMD64_USER_CPPFLAGS += -Iuserland/base/libedit
$(AMD64_USER_READLINE_LIB): $(AMD64_USER_READLINE_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

AMD64_USER_CURSES_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,\
	$(BUILD)/user64,curses)
$(BUILD)/lib/libcurses.a: $(AMD64_USER_CURSES_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/POSIX-R1.ELF: $(AMD64_USER_LIBC_OBJS) \
	$(BUILD)/user64/userland/base/tests/syscall-smoke.o $(AMD64_PLATFORM)/user.ld \
	$(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld \
		$(AMD64_USER_LIBC_OBJS) \
		$(BUILD)/user64/userland/base/tests/syscall-smoke.o -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(BUILD)/POSIX-R2.ELF: $(AMD64_USER_NET_LIBC_OBJS) \
	$(BUILD)/user64/userland/base/tests/posix-r2.o $(AMD64_PLATFORM)/user.ld \
	$(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
		$(BUILD)/user64/userland/base/tests/posix-r2.o -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(BUILD)/POSIX-R2-REMAINING.ELF: $(AMD64_USER_NET_LIBC_OBJS) \
	$(BUILD)/user64/userland/base/tests/posix-r2-remaining.o \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
		$(BUILD)/user64/userland/base/tests/posix-r2-remaining.o -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

# WS008 NOCT-T020 test-only executable.  It is not part of the base-system
# program registry; the owning QEMU runner explicitly builds and injects it
# into a disposable image.
AMD64_NOCT_JIT_VM_PROBE_OBJ := \
	$(BUILD)/user64/plan/ws008-noct/tests/noct-jit-vm-probe.o
$(BUILD)/NOCT-JIT-VM-PROBE.ELF: $(AMD64_USER_LIBC_OBJS) \
	$(AMD64_NOCT_JIT_VM_PROBE_OBJ) $(AMD64_PLATFORM)/user.ld \
	$(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(AMD64_NOCT_JIT_VM_PROBE_OBJ) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(BUILD)/SUSV4-XSI.ELF: $(AMD64_USER_NET_LIBC_OBJS) \
	$(BUILD)/user64/userland/base/tests/susv4-xsi.o \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
		$(BUILD)/user64/userland/base/tests/susv4-xsi.o -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

susv4-xsi-user-test: $(BUILD)/SUSV4-XSI.ELF

$(BUILD)/bin/sh: $(AMD64_USER_LIBC_OBJS) $(AMD64_USER_SH_OBJS) \
	$(AMD64_USER_READLINE_LIB) \
	$(AMD64_PLATFORM)/user.ld \
	$(AMD64_USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(AMD64_USER_SH_OBJS) $(AMD64_USER_READLINE_LIB) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(BUILD)/SMP-STRESS.ELF: $(AMD64_USER_NET_LIBC_OBJS) \
	$(BUILD)/user64/userland/base/tests/smp-resource-stress.o \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
		$(BUILD)/user64/userland/base/tests/smp-resource-stress.o -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

AMD64_USER_SYSCTL_OBJ := $(BUILD)/user64/userland/base/sysctl/main.o
$(BUILD)/bin/sysctl: $(AMD64_USER_LIBC_OBJS) $(AMD64_USER_SYSCTL_OBJ) \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(AMD64_USER_SYSCTL_OBJ) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

AMD64_USER_MOUNT_OBJ := $(BUILD)/user64/userland/base/mount/main.o
$(BUILD)/bin/mount: $(AMD64_USER_LIBC_OBJS) $(AMD64_USER_MOUNT_OBJ) \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(AMD64_USER_MOUNT_OBJ) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@
$(BUILD)/bin/umount: $(BUILD)/bin/mount
	@mkdir -p $(dir $@)
	cp -f $< $@

$(BUILD)/bin/nettest: $(AMD64_USER_NET_LIBC_OBJS) \
	$(AMD64_USER_NETTEST_OBJS) $(AMD64_PLATFORM)/user.ld \
	$(AMD64_USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
		$(AMD64_USER_NETTEST_OBJS) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

USER_NET_COMMANDS := $(USERLAND_SELECTED_NETWORK_PROGRAMS)
USER_NET_COMMAND_TARGETS := $(addprefix $(BUILD)/bin/,$(USER_NET_COMMANDS))
AMD64_USER_NET_COMMON_OBJS := $(BUILD)/user64/userland/base/net/netutil.o \
	$(BUILD)/user64/userland/base/net/dhcp.o
AMD64_USER_NET_COMMAND_OBJS := $(addsuffix /main.o, \
	$(addprefix $(BUILD)/user64/userland/,$(USER_NET_COMMANDS)))

define AMD64_USER_NET_COMMAND
$(BUILD)/bin/$(1): $(AMD64_USER_NET_LIBC_OBJS) \
	$(AMD64_USER_NET_COMMON_OBJS) $(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user64,$(1)) \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	@mkdir -p $$(dir $$@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
		$(AMD64_USER_NET_COMMON_OBJS) \
		$(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user64,$(1)) -o $$@
	@test -z "$$$$(nm -u $$@)" || { nm -u $$@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $$@
endef
$(foreach command,$(USER_NET_COMMANDS),\
	$(eval $(call AMD64_USER_NET_COMMAND,$(command))))
USER_BASIC_COMMANDS := $(filter $(ZEDBSD_USER_PROGRAMS),$(USERLAND_BASIC_PROGRAMS))
USER_BASIC_TARGETS := $(addprefix $(BUILD)/bin/,$(USER_BASIC_COMMANDS))
AMD64_USER_BASIC_COMMON_OBJ := $(BUILD)/user64/userland/base/common/command.o $(BUILD)/user64/userland/base/common/pager.o

define AMD64_USER_BASIC_COMMAND
$(BUILD)/bin/$(1): $(AMD64_USER_LIBC_OBJS) \
	$(AMD64_USER_BASIC_COMMON_OBJ) $(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user64,$(1)) \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	@mkdir -p $$(dir $$@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(AMD64_USER_BASIC_COMMON_OBJ) \
		$(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user64,$(1)) -o $$@
	@test -z "$$$$(nm -u $$@)" || { nm -u $$@; exit 1; }
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $$@
endef
$(foreach command,$(USER_BASIC_COMMANDS),\
	$(eval $(call AMD64_USER_BASIC_COMMAND,$(command))))
# ELF64 runtime linker and shared libc.
DYNAMIC_DIR := $(BUILD)/dynamic
DYNAMIC_CPPFLAGS := -nostdinc -I. -Iinclude -Iinclude/uapi -Ilibc/include \
	-DHAL_ARCH_AMD64 -DZEDBSD_USER_ABI_LP64 -DZEDBSD_DYNAMIC_LIBC
DYNAMIC_CFLAGS := -m64 -march=x86-64 -mno-red-zone -Os -ffreestanding \
	-fPIC -fno-builtin -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-ftls-model=global-dynamic -Wall -Wextra -Werror
DYNAMIC_LIBC_SOURCES := userland/base/libc/posix.c userland/base/libc/poll.c \
	userland/base/libc/termios.c userland/base/libc/pthread.c userland/base/libc/timer.c userland/base/libc/shm.c \
	userland/base/libc/semaphore.c userland/base/libc/mqueue.c userland/base/libc/dlfcn.c \
	userland/base/libc/socket.c userland/base/libc/resolver.c \
	userland/base/libc/resolver-dns.c userland/base/libc/signal.c \
	userland/base/libc/account.c userland/base/libc/crypt.c userland/base/libc/utmpx.c libc/heap.c \
	libc/string.c libc/ctype.c libc/locale.c libc/wide.c libc/int64.c \
	libc/strto.c libc/format.c \
	libc/stdio.c $(ZEDBSD_LIBC_USER_EXTRA_SOURCES)
DYNAMIC_LIBC_OBJS := $(patsubst %.c,$(DYNAMIC_DIR)/obj/%.o,\
	$(DYNAMIC_LIBC_SOURCES)) $(DYNAMIC_DIR)/obj/userland/base/libc/syscall.o
DYNAMIC_RTLD_OBJS := $(DYNAMIC_DIR)/obj/userland/base/rtld/entry.o \
	$(DYNAMIC_DIR)/obj/userland/base/rtld/tlsdesc.o \
	$(DYNAMIC_DIR)/obj/userland/base/rtld/rtld.o \
	$(DYNAMIC_DIR)/obj/userland/base/rtld/string.o
DYNAMIC_FLOAT_DIR := $(DYNAMIC_DIR)/float
DYNAMIC_LIBM_OBJ := $(DYNAMIC_FLOAT_DIR)/math.o
DYNAMIC_FLOAT_PARSE_OBJS := $(DYNAMIC_FLOAT_DIR)/zed-softfloat.o \
	$(DYNAMIC_FLOAT_DIR)/float-parse.o
DYNAMIC_LIBC_OBJS += $(DYNAMIC_LIBM_OBJ) $(DYNAMIC_FLOAT_PARSE_OBJS)

$(DYNAMIC_DIR)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_CPPFLAGS) $(DYNAMIC_CFLAGS) -MMD -MP -c $< -o $@

$(DYNAMIC_DIR)/obj/userland/base/libc/syscall.o: \
	userland/base/libc/syscall-amd64.S include/hal/arch.h \
	include/hal/arch/amd64.h
	@mkdir -p $(dir $@)
	$(CC) $(DYNAMIC_CPPFLAGS) -m64 -c $< -o $@

$(DYNAMIC_DIR)/obj/userland/base/rtld/entry.o: userland/base/rtld/entry-amd64.S
	@mkdir -p $(dir $@)
	$(CC) -m64 -c $< -o $@

$(DYNAMIC_DIR)/obj/userland/base/rtld/tlsdesc.o: userland/base/rtld/tlsdesc-amd64.S
	@mkdir -p $(dir $@)
	$(CC) -m64 -c $< -o $@

$(DYNAMIC_DIR)/obj/userland/base/tests/tlstest.o: DYNAMIC_CFLAGS += -mtls-dialect=gnu2

$(DYNAMIC_LIBM_OBJ): libc/math.c src/softfloat/zed-softfloat.h
	@mkdir -p $(dir $@)
	$(CC) -nostdinc -Ilibc/include -Iinclude/uapi -I. $(DYNAMIC_CFLAGS) \
		-mlong-double-64 -c $< -o $@

$(DYNAMIC_FLOAT_DIR)/zed-softfloat.o: src/softfloat/zed-softfloat.c \
	src/softfloat/zed-softfloat.h
	@mkdir -p $(dir $@)
	$(CC) -nostdinc -Ilibc/include -Iinclude/uapi -I. $(DYNAMIC_CFLAGS) \
		-mlong-double-64 -c $< -o $@

$(DYNAMIC_FLOAT_DIR)/float-parse.o: libc/float-parse.c \
	src/softfloat/zed-softfloat.h
	@mkdir -p $(dir $@)
	$(CC) -nostdinc -Ilibc/include -Iinclude/uapi -I. $(DYNAMIC_CFLAGS) \
		-mlong-double-64 -c $< -o $@

$(DYNAMIC_DIR)/obj/src/crt/crt1.o: src/crt/crt1-amd64.S
	@mkdir -p $(dir $@)
	$(CC) -m64 -c $< -o $@

$(DYNAMIC_DIR)/ld.so: $(DYNAMIC_RTLD_OBJS)
	$(LD) -m elf_x86_64 -shared -Bsymbolic -e _rtld_start \
		--hash-style=sysv -z now -z relro -z separate-code $^ -o $@

$(DYNAMIC_DIR)/libc.so: $(DYNAMIC_LIBC_OBJS)
	$(LD) -m elf_x86_64 -shared -soname libc.so --hash-style=both \
		-z now -z relro -z separate-code -z stack-size=0x100000 $^ -o $@

$(DYNAMIC_DIR)/alt/rpathdep.so: \
	$(DYNAMIC_DIR)/obj/userland/base/tests/rpathdep.o $(DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 -shared -soname rpathdep.so --hash-style=gnu \
		-z now -z relro -z separate-code $< -o $@

$(DYNAMIC_DIR)/tlstest.so: \
	$(DYNAMIC_DIR)/obj/userland/base/tests/tlstest.o \
	$(DYNAMIC_DIR)/alt/rpathdep.so $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_x86_64 -shared -soname tlstest.so --hash-style=gnu \
		-z now -z relro -z separate-code --enable-new-dtags \
		-rpath '$$ORIGIN/alt' \
		$(DYNAMIC_DIR)/obj/userland/base/tests/tlstest.o \
		-L$(DYNAMIC_DIR)/alt -l:rpathdep.so -o $@

$(DYNAMIC_DIR)/rpathtest.so: \
	$(DYNAMIC_DIR)/obj/userland/base/tests/rpathtest.o \
	$(DYNAMIC_DIR)/alt/rpathdep.so $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_x86_64 -shared -soname rpthtest.so --hash-style=gnu \
		-z now -z relro -z separate-code --disable-new-dtags \
		-rpath '$$ORIGIN/alt' $< -L$(DYNAMIC_DIR)/alt \
		-l:rpathdep.so -o $@

$(DYNAMIC_DIR)/verstest.so: \
	$(DYNAMIC_DIR)/obj/userland/base/tests/versiontest.o \
	userland/base/tests/versiontest.map $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_x86_64 -shared -soname verstest.so --hash-style=gnu \
		-z now -z relro -z separate-code \
		--version-script=userland/base/tests/versiontest.map $< -o $@

$(DYNAMIC_DIR)/versuse.so: \
	$(DYNAMIC_DIR)/obj/userland/base/tests/versionuse.o \
	$(DYNAMIC_DIR)/verstest.so $(DYNAMIC_DIR)/ld.so
	$(LD) -m elf_x86_64 -shared -soname versuse.so --hash-style=gnu \
		-z now -z relro -z separate-code $< -L$(DYNAMIC_DIR) \
		-l:verstest.so -o $@

$(DYNAMIC_DIR)/dyntest: $(DYNAMIC_DIR)/obj/src/crt/crt1.o \
	$(DYNAMIC_DIR)/obj/userland/base/tests/dyntest.o $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/ld.so $(DYNAMIC_DIR)/tlstest.so \
	$(DYNAMIC_DIR)/versuse.so
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
		-Wl,--hash-style=sysv,-z,now,-z,relro,-z,separate-code \
		-Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
		-Wl,--dynamic-linker=/lib/ld.so \
		$(DYNAMIC_DIR)/obj/src/crt/crt1.o \
		$(DYNAMIC_DIR)/obj/userland/base/tests/dyntest.o \
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
AMD64_ARCH_INPUTS := $(BUILD)/bin/sh $(BUILD)/bin/nettest \
	$(BUILD)/bin/sysctl \
	$(BUILD)/bin/mount $(BUILD)/bin/umount \
	$(DYNAMIC_DIR)/ld.so $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/tlstest.so $(DYNAMIC_DIR)/dyntest \
	$(DYNAMIC_DIR)/alt/rpathdep.so $(DYNAMIC_DIR)/rpathtest.so \
	$(DYNAMIC_DIR)/verstest.so $(DYNAMIC_DIR)/versuse.so
AMD64_ARCH_FILES := --file /bin/sh=$(BUILD)/bin/sh \
	--file /bin/nettest=$(BUILD)/bin/nettest \
	--file /sbin/sysctl=$(BUILD)/bin/sysctl \
	--file /sbin/mount=$(BUILD)/bin/mount \
	--file /sbin/umount=$(BUILD)/bin/umount \
	--file /lib/ld.so=$(DYNAMIC_DIR)/ld.so \
	--file /lib/libc.so=$(DYNAMIC_DIR)/libc.so \
	--file /lib/tlstest.so=$(DYNAMIC_DIR)/tlstest.so \
	--file /lib/alt/rpathdep.so=$(DYNAMIC_DIR)/alt/rpathdep.so \
	--file /lib/rpthtest.so=$(DYNAMIC_DIR)/rpathtest.so \
	--file /lib/verstest.so=$(DYNAMIC_DIR)/verstest.so \
	--file /lib/versuse.so=$(DYNAMIC_DIR)/versuse.so \
	--file /bin/dyntest=$(DYNAMIC_DIR)/dyntest
AMD64_ARCH_INPUTS += $(addprefix $(BUILD)/bin/,$(USERLAND_SELECTED_NETWORK_PROGRAMS))
AMD64_ARCH_FILES += $(foreach command,$(USERLAND_SELECTED_NETWORK_PROGRAMS),--file $(call zedbsd_userland_destination,$(command))=$(BUILD)/bin/$(command))
AMD64_ARCH_INPUTS += $(USER_BASIC_TARGETS)
AMD64_ARCH_FILES += $(foreach command,$(USER_BASIC_COMMANDS),--file $(call zedbsd_userland_destination,$(command))=$(BUILD)/bin/$(command))
AMD64_ARCH_FILES += $(ZEDBSD_USERLAND_FILE_MODES)
AMD64_ARCH_INPUTS += $(ZEDBSD_ACCOUNT_INPUTS)
AMD64_ARCH_FILES += $(ZEDBSD_ACCOUNT_FILES)
AMD64_ARCH_INPUTS += $(ZEDBSD_BASE_DATA_INPUTS)
AMD64_ARCH_FILES += $(ZEDBSD_BASE_DATA_FILES)
$(eval $(call ZEDBSD_ARCH_IMAGE_RULE,$(AMD64_ARCH_IMAGE),amd64,$(AMD64_ARCH_INPUTS),$(AMD64_ARCH_FILES)))
$(eval $(call ZEDBSD_ROOTFS_TAR_RULE,$(BUILD)/rootfs.tar.gz,$(AMD64_ARCH_INPUTS),$(AMD64_ARCH_FILES)))
AMD64_ARCH_UFS_IMAGE := $(ARCH_IMAGE_DIR)/amd64.ufs
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(AMD64_ARCH_UFS_IMAGE),amd64,$(AMD64_ARCH_INPUTS),$(AMD64_ARCH_FILES)))
rootfs: $(BUILD)/rootfs/.stamp

$(BUILD)/bios-hdd-image.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage2-chain.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix $(AMD64_ARCH_UFS_IMAGE) \
	$(DATA_IMAGE) $(SWAP_IMAGE) $(BUILD)/uefi/BOOTX64.EFI \
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
		--zedbsd-config $(AMD64_ZEDBSD_CONFIG) \
		--arch-profile amd64 --arch-image $(AMD64_ARCH_UFS_IMAGE) \
		--arch-format ufs --data-image $(DATA_IMAGE) \
		--swapfile $(SWAP_IMAGE) $@

$(BUILD)/ufs-root.img: $(AMD64_ARCH_UFS_IMAGE) \
	$(BUILD_TOOLS_DIR)/make-ufs1-root-image.py tools/build/ufs1_format.py
	$(PYTHON) $(BUILD_TOOLS_DIR)/make-ufs1-root-image.py --force \
		--arch-profile amd64 --arch-image $(AMD64_ARCH_UFS_IMAGE) $@

$(BUILD)/ufs-root-hdd-image.img: $(BUILD)/bootloader/stage1-native.bin \
	$(BUILD)/bootloader/stage2-chain.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix $(BUILD)/ufs-root.img \
	$(AMD64_NATIVE_ZEDBSD_CONFIG) \
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
		--zedbsd-config $(AMD64_NATIVE_ZEDBSD_CONFIG) \
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
		--zedbsd-config $(AMD64_ZEDBSD_CONFIG) \
		--arch-profile amd64 --arch-image $(AMD64_ARCH_UFS_IMAGE) \
		--arch-format ufs --data-image $(DATA_IMAGE) \
		--swapfile $(SWAP_IMAGE) \
		--fragment-kernel $@

$(BUILD)/hdd-image.img: $(BUILD)/bootloader/stage1.bin \
	$(BUILD)/bootloader/stage1-native.bin \
	$(BUILD)/bootloader/stage2-chain.bin $(BUILD)/bootloader/partition-pbr.bin \
	$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix $(AMD64_ARCH_UFS_IMAGE) \
	$(DATA_IMAGE) $(SWAP_IMAGE) $(BUILD)/uefi/BOOTX64.EFI \
	$(AMD64_ZEDBSD_CONFIG) $(ZEDBSD_IMAGE_HOST) \
	$(AMD64_IMAGE_CONTRACT_STAMP) tools/build/make-bios-hdd-image.noct \
	tools/build/zedbuild.noct tools/build/overlay_journal_format.noct \
	platform/amd64/tools/check-amd64-gpt-image.noct
	$(NOCT) --path=tools/build tools/build/make-bios-hdd-image.noct \
		--backend $(abspath $(ZEDBSD_IMAGE_HOST)) --force --machine pcat \
		--layout $(ZEDBSD_VARIANT) \
		--checker platform/amd64/tools/check-amd64-gpt-image.noct \
		--checker-runner $(NOCT) \
		--stage1 $(AMD64_IMAGE_STAGE1) \
		--stage2 $(BUILD)/bootloader/stage2-chain.bin \
		--partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
		--bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
		--kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
		--zedbsd-config $(AMD64_ZEDBSD_CONFIG) \
		--arch-profile amd64 --arch-image $(AMD64_ARCH_UFS_IMAGE) \
		--arch-format ufs --data-image $(DATA_IMAGE) \
		--swapfile $(SWAP_IMAGE) $@

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
		--zedbsd-config $(AMD64_ZEDBSD_CONFIG) \
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
		--zedbsd-config $(AMD64_ZEDBSD_CONFIG) \
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
		--zedbsd-config $(AMD64_ZEDBSD_CONFIG) \
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
		--zedbsd-config $(AMD64_ZEDBSD_CONFIG) \
		--arch-profile amd64 --arch-image $(AMD64_POSIX_PHASE4_TEST_UFS) \
		--arch-format ufs --data-image $(DATA_IMAGE) \
		--swapfile $(SWAP_IMAGE) $@

posix-phase4-qemu-test: $(BUILD)/posix-phase4-qemu.img \
	tests/posix-phase4-qemu-test.py
	$(PYTHON) tests/posix-phase4-qemu-test.py \
		--qemu $(QEMU) --image $(BUILD)/posix-phase4-qemu.img

$(BUILD)/bin/posix-phase5-helper: $(AMD64_USER_NET_LIBC_OBJS) \
	$(BUILD)/user64/userland/base/tests/posix-phase5-helper.o \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
		$(BUILD)/user64/userland/base/tests/posix-phase5-helper.o -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
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
		--zedbsd-config $(AMD64_ZEDBSD_CONFIG) \
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
		--zedbsd-config $(AMD64_ZEDBSD_CONFIG) \
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
		--zedbsd-config $(AMD64_ZEDBSD_CONFIG) \
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
		--zedbsd-config $(AMD64_ZEDBSD_CONFIG) \
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
		--zedbsd-config $(AMD64_ZEDBSD_CONFIG) \
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
		--zedbsd-config $(AMD64_ZEDBSD_CONFIG) \
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
	$(AMD64_POSIX_PHASE85_CURSES_OBJS) $(AMD64_PLATFORM)/user.ld \
	$(AMD64_USER_ELF_CHECK)
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
		-z max-page-size=4096 -z stack-size=0x100000 \
		-T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
		$(AMD64_POSIX_PHASE85_CURSES_OBJS) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
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
		--zedbsd-config $(AMD64_ZEDBSD_CONFIG) \
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
