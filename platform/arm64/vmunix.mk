# zedBSD arm64/Raspberry Pi 4 bootstrap rules.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

# The project toolchain builds an AArch64 compiler, so the port uses
# it rather than depending on a system cross GCC being installed.
ARM64_CC ?= $(ZEDBSD_TARGET_LLVM_BIN)/clang --target=aarch64-unknown-zedbsd
ARM64_LD ?= $(ZEDBSD_TARGET_LLVM_BIN)/ld.lld
ARM64_OBJCOPY ?= $(ZEDBSD_TARGET_LLVM_BIN)/llvm-objcopy
ARM64_NM ?= $(ZEDBSD_TARGET_LLVM_BIN)/llvm-nm
AWK ?= awk
ARM64_PLATFORM := platform/arm64

# The kernel and the HAL read the compiler's freestanding headers and the
# tree's include/ and src/, never the C library (as on amd64).  The compiler
# knows the zedBSD target and defines __ZEDBSD__, which selects the zedBSD
# definitions (include/uapi/hosted.h, include/kern/kcrt.h).
ARM64_CPPFLAGS := -nostdlibinc -Iinclude -Isrc -I. \
	-Isrc/hal/arm64 -DHAL_ARCH_ARM64 -DHAL_BOARD_RPI4 \
	-DKERN_USER_ABI_AARCH64 -DKERN_USER_ABI_LP64
ARM64_CPPFLAGS += $(ZEDBSD_CONFIG_CPPFLAGS)
ARM64_CFLAGS := -march=armv8-a -mno-outline-atomics -mgeneral-regs-only -ffreestanding \
	-fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -fno-common -ffunction-sections -fdata-sections \
	-Os -Wall -Wextra -Werror

# Link-time optimization of vmunix (ws053): ZEDBSD_KERNEL_LTO_CFLAGS (from the
# top Makefile) goes to every C object of the kernel, its drivers and the
# HAL; the assembly stays native.
ARM64_KERNEL_LTO_CFLAGS := $(ZEDBSD_KERNEL_LTO_CFLAGS)

ARM64_BOOT_C := src/hal/cpu-up.c src/hal/idle-suspend-unsupported.c src/hal/arm64/asm.c src/hal/arm64/lib.c \
	src/hal/arm64/page.c src/hal/arm64/space.c \
	src/hal/arm64/int.c src/hal/arm64/irq.c \
	src/hal/arm64/task.c \
	src/hal/arm64/cmain.c src/hal/arm64/debug.c \
	src/hal/arm64/bsp-rpi4/uart.c \
	src/hal/arm64/bsp-rpi4/cons.c src/hal/arm64/bsp-rpi4/fdt.c \
	src/hal/arm64/bsp-rpi4/mailbox.c src/hal/arm64/bsp-rpi4/framebuffer.c \
	src/hal/arm64/bsp-rpi4/boot.c src/hal/arm64/bsp-rpi4/gic.c \
	src/hal/arm64/bsp-rpi4/clock.c src/hal/arm64/bsp-rpi4/font.c \
	src/hal/arm64/bsp-rpi4/led.c
ARM64_BOOT_S := src/hal/arm64/locore.S src/hal/arm64/trap.S \
	src/hal/arm64/dispatch.S
ARM64_BOOT_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(ARM64_BOOT_C)) \
	$(patsubst %.S,$(BUILD)/%.o,$(ARM64_BOOT_S))

ARM64_KERNEL_SOURCES := \
	src/kern/main.c \
	$(KERN_FAT_SOURCES) \
	src/kern/inode.c src/kern/file.c src/kern/namecache.c src/kern/namei.c \
	src/kern/mount.c src/kern/tmpfs.c src/kern/tmpfs-pages.c src/drivers/fs/overlayfs.c \
	src/kern/vfs.c src/kern/swap.c src/kern/backing-claim.c \
 src/kern/buf.c src/kern/cache.c src/kern/readahead.c src/kern/writeback.c src/kern/io.c \
	src/kern/sysctl.c src/kern/resource.c src/kern/poll.c src/kern/usync.c src/kern/disk.c \
	src/kern/device-io.c src/kern/irq.c src/kern/pmem.c \
	src/kern/test-checkpoint.c src/kern/text-display.c \
	src/drivers/generic/loop.c \
	src/kern/partition.c src/drivers/disklabel/mbr.c \
	src/kern/platform/rpi4.c \
	src/drivers/platform/rpi4/rpi4-sdhci.c \
	src/drivers/platform/rpi4/rpi4-console.c \
	src/drivers/platform/rpi4/rpi4-pcie.c \
	src/drivers/platform/rpi4/rpi4-firmware.c src/kern/dcache.c \
	src/kern/uncached.c \
	src/drivers/generic/fdt.c src/drivers/generic/dma.c \
	src/drivers/pci/pci.c src/drivers/pci/pci-brcmstb.c \
	src/kern/panic.c src/kern/entry.c src/kern/clock.c \
	src/kern/timer.c src/kern/klog.c \
	src/kern/lock.c src/kern/waitq.c \
	src/kern/process.c src/kern/ptrace.c src/kern/thread.c src/kern/sched.c src/kern/sleep.c src/kern/freeze.c src/kern/vmspace.c src/kern/vm-device.c \
	src/kern/vm.c src/kern/filedesc.c src/kern/handle.c src/kern/fd-object.c \
	src/kern/record-lock.c \
	src/kern/pipe.c src/kern/cred.c \
	src/kern/signal.c \
	src/kern/cwdinfo.c \
	src/kern/elf.c src/kern/exec.c src/kern/sandbox.c src/kern/user-probe.c src/kern/syscall.c \
	src/kern/uaccess.c src/kern/cdev.c src/kern/devfs.c \
	src/drivers/generic/console.c src/drivers/generic/input.c \
	$(KERN_GPU_SOURCES) \
	$(KERN_AUDIO_SOURCES) \
	src/kern/kcrt.c src/kern/heap.c \
	src/kern/random.c src/kern/random-crypto.c \
	src/kern/tty.c \
 src/drivers/generic/system-device.c src/kern/system-event.c src/drivers/generic/memory-device.c src/kern/shutdown.c \
	src/kern/init.c
# Onboard Ethernet uses the SCB bus, without PCIe or USB.
ifeq ($(CONFIG_DRIVER_BCM2711_GENET),y)
ARM64_KERNEL_SOURCES += src/drivers/ethernet/bcm54213pe.c \
	src/drivers/platform/rpi4/rpi4-ethernet.c
endif

# USB behind the Pi 4's PCIe (ws048): the core comes with any USB driver.
ARM64_USB_SOURCES :=
ifneq ($(filter y,$(CONFIG_DRIVER_PCI_XHCI) $(CONFIG_DRIVER_USB_HID) $(CONFIG_DRIVER_USB_HUB) $(CONFIG_DRIVER_USB_STORAGE) $(CONFIG_DRIVER_USB_CDC_NCM) $(CONFIG_DRIVER_USB_CDC_ECM) $(CONFIG_DRIVER_USB_CCID) $(CONFIG_DRIVER_USB_BT) $(CONFIG_DRIVER_USB_RTL8822BU)),)
ARM64_USB_SOURCES += src/drivers/usb/usb.c
endif
ifeq ($(CONFIG_DRIVER_PCI_XHCI),y)
ARM64_USB_SOURCES += src/drivers/pci/pci-xhci.c
endif
ifeq ($(CONFIG_DRIVER_USB_HID),y)
ARM64_USB_SOURCES += src/drivers/usb/usb-hid.c src/drivers/generic/hidraw.c src/drivers/generic/hidraw-describe.c src/drivers/generic/hid-report.c src/drivers/generic/hid-digitizer.c src/drivers/generic/hid-touch.c \
	src/drivers/generic/hid-input.c
endif
ifeq ($(CONFIG_DRIVER_USB_HUB),y)
ARM64_USB_SOURCES += src/drivers/usb/usb-hub.c
endif
ifeq ($(CONFIG_DRIVER_USB_STORAGE),y)
ARM64_USB_SOURCES += src/drivers/usb/usb-storage.c src/drivers/usb/usb-uas.c \
	src/drivers/usb/usb-uas-transport.c src/drivers/usb/usb-uas-disk.c
endif
ifeq ($(CONFIG_DRIVER_USB_CDC_NCM),y)
ARM64_USB_SOURCES += src/drivers/usb/usb-cdc-ncm.c src/drivers/usb/usb-cdc-ncm-net.c
endif
ifeq ($(CONFIG_DRIVER_USB_CDC_ECM),y)
ARM64_USB_SOURCES += src/drivers/usb/usb-cdc-ecm.c
endif
ifneq ($(filter y,$(CONFIG_DRIVER_USB_CDC_NCM) $(CONFIG_DRIVER_USB_CDC_ECM)),)
ARM64_USB_SOURCES += src/drivers/usb/usb-cdc-notification.c
endif
ifeq ($(CONFIG_DRIVER_USB_CCID),y)
ARM64_USB_SOURCES += src/drivers/usb/usb-ccid.c src/drivers/usb/usb-ccid-proto.c \
	src/drivers/generic/smartcard.c
endif
ifeq ($(CONFIG_DRIVER_USB_BT),y)
ARM64_USB_SOURCES += src/drivers/usb/usb-bt.c src/drivers/generic/bt-hci.c \
	src/drivers/generic/bt-hci-proto.c
endif
ifeq ($(CONFIG_DRIVER_USB_RTL8822BU),y)
ARM64_USB_SOURCES += src/drivers/usb/usb-rtl8822bu.c \
	src/drivers/wifi/rtl8822b/rtl8822b.c src/drivers/wifi/rtl8822b/rtl8822b-security.c
endif
ARM64_KERNEL_SOURCES += $(ARM64_USB_SOURCES)
# The BCM2711 graphics driver (ws141): the display path and V3D.
ifeq ($(CONFIG_DRIVER_BCM2711_GPU),y)
# Native BCM2711 nodes need the common GPU core even without a PCI GPU.
ARM64_KERNEL_SOURCES += $(filter-out $(KERN_GPU_SOURCES),src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c)
# The shared Zlib scalar parser has no device or Gen12 code-generation dependencies.
ARM64_KERNEL_SOURCES += $(filter-out $(KERN_GPU_SOURCES),src/drivers/gpu/compiler/spirv.c src/drivers/gpu/i915/render/codec.c)
ARM64_KERNEL_SOURCES += src/drivers/gpu/bcm2711/attach.c \
	src/drivers/gpu/bcm2711/stage.c src/drivers/gpu/bcm2711/fdt-util.c \
	src/drivers/gpu/bcm2711/firmware.c src/drivers/gpu/bcm2711/display.c \
	src/drivers/gpu/bcm2711/readout.c src/drivers/gpu/bcm2711/list.c \
	src/drivers/gpu/bcm2711/v3d.c src/drivers/gpu/bcm2711/v3d-power.c \
	src/drivers/gpu/bcm2711/v3d-hardware.c src/drivers/gpu/bcm2711/mmu.c \
	src/drivers/gpu/bcm2711/v3d-job.c src/drivers/gpu/bcm2711/v3d-diagnostic.c \
	src/drivers/gpu/bcm2711/cl.c src/drivers/gpu/bcm2711/qpu.c src/drivers/gpu/bcm2711/buffer.c \
	src/drivers/gpu/bcm2711/shader.c src/drivers/gpu/bcm2711/shader-analyze.c \
	src/drivers/gpu/bcm2711/shader-lower.c src/drivers/gpu/bcm2711/shader-output.c \
	src/drivers/gpu/bcm2711/native-bin.c src/drivers/gpu/bcm2711/native-pass.c src/drivers/gpu/bcm2711/native-colour.c src/drivers/gpu/bcm2711/native-shader.c src/drivers/gpu/bcm2711/native-texture.c src/drivers/gpu/bcm2711/native-storage.c src/drivers/gpu/bcm2711/native-viewport.c \
	src/drivers/gpu/bcm2711/vulkan-object.c src/drivers/gpu/bcm2711/vulkan-session.c \
	src/drivers/gpu/bcm2711/vulkan-stream.c src/drivers/gpu/bcm2711/vulkan-dispatch.c src/drivers/gpu/bcm2711/vulkan-device.c src/drivers/gpu/bcm2711/vulkan-sync.c src/drivers/gpu/bcm2711/vulkan-queue.c src/drivers/gpu/bcm2711/vulkan-query.c src/drivers/gpu/bcm2711/vulkan-memory.c src/drivers/gpu/bcm2711/vulkan-resource.c src/drivers/gpu/bcm2711/vulkan-input.c src/drivers/gpu/bcm2711/vulkan-layout.c src/drivers/gpu/bcm2711/vulkan-layout-compat.c src/drivers/gpu/bcm2711/vulkan-descriptor-pool.c src/drivers/gpu/bcm2711/vulkan-descriptor-sets.c src/drivers/gpu/bcm2711/vulkan-descriptor-update.c src/drivers/gpu/bcm2711/vulkan-target.c \
	src/drivers/gpu/bcm2711/vulkan-pipeline-build.c src/drivers/gpu/bcm2711/vulkan-pipeline-state.c src/drivers/gpu/bcm2711/vulkan-pipeline.c src/drivers/gpu/bcm2711/vulkan-pipeline-decode.c \
	src/drivers/gpu/bcm2711/vulkan-command-pool.c src/drivers/gpu/bcm2711/vulkan-command-batch.c src/drivers/gpu/bcm2711/vulkan-command-buffer.c \
	src/drivers/gpu/bcm2711/vulkan-barrier.c src/drivers/gpu/bcm2711/vulkan-barrier-decode.c src/drivers/gpu/bcm2711/vulkan-barrier-validate.c src/drivers/gpu/bcm2711/vulkan-record.c src/drivers/gpu/bcm2711/vulkan-record-decode.c src/drivers/gpu/bcm2711/vulkan-record-validate.c src/drivers/gpu/bcm2711/vulkan-draw.c src/drivers/gpu/bcm2711/vulkan-draw-validate.c src/drivers/gpu/bcm2711/vulkan-prepared.c src/drivers/gpu/bcm2711/vulkan-uniform.c src/drivers/gpu/bcm2711/vulkan-native-draw.c src/drivers/gpu/bcm2711/vulkan-native-bin.c src/drivers/gpu/bcm2711/vulkan-native-pass.c src/drivers/gpu/bcm2711/vulkan-native-execute.c src/drivers/gpu/bcm2711/vulkan-native-job.c src/drivers/gpu/bcm2711/vulkan-buffer-copy.c src/drivers/gpu/bcm2711/vulkan-transfer.c src/drivers/gpu/bcm2711/vulkan-native-image.c \
	src/drivers/gpu/bcm2711/share.c src/drivers/gpu/bcm2711/v3d-memory.c \
	src/drivers/gpu/bcm2711/render-device.c src/drivers/gpu/bcm2711/render-runtime.c src/drivers/gpu/bcm2711/render-worker.c \
	src/drivers/gpu/bcm2711/display-program.c \
	src/drivers/gpu/bcm2711/display-start.c \
	src/drivers/gpu/bcm2711/display-execute.c \
	src/drivers/gpu/bcm2711/display-flip.c \
	src/drivers/gpu/bcm2711/display-device.c \
	src/drivers/gpu/bcm2711/display-irq.c
endif
ARM64_KERNEL_SOURCES += $(KERN_NET_SOURCES) $(KERN_BLOCK_IDENTITY_SOURCES) \
	$(KERN_UFS_SOURCES)
ARM64_KERNEL_SOURCES += $(KERN_BOOT_SOURCES)
ARM64_KERNEL_SOURCES += $(KERN_ACL_SOURCES)
ARM64_KERNEL_SOURCES += $(KERN_QUOTA_SOURCES)
ARM64_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kernel/%.o,$(ARM64_KERNEL_SOURCES))
# The kernel links no C library object: kcrt and the kernel heap supply
# what it used from libc.
ARM64_VMUNIX_OBJS := $(ARM64_BOOT_OBJS) $(ARM64_KERNEL_OBJS)
$(ARM64_VMUNIX_OBJS): $(ZEDBSD_PLATFORM_CONFIG_STAMP)
$(ARM64_VMUNIX_OBJS): $(ZEDBSD_KERNEL_LTO_STAMP)

# User programs read the C library headers from the target sysroot
# (toolchain/llvm/sysroot.mk), as on amd64.
ARM64_USER_CPPFLAGS := -nostdinc \
	-isystem $(ZEDBSD_SYSROOT_ARM64)/usr/include \
	-Iinclude -Isrc -I. -DHAL_ARCH_ARM64 -DKERN_USER_ABI_AARCH64 \
	-DKERN_USER_ABI_LP64
ARM64_USER_CFLAGS := -march=armv8-a -mno-outline-atomics \
	-ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-builtin -fno-common -ffunction-sections -fdata-sections \
	-Os -Wall -Wextra -Werror
# The static programs link the sysroot's start file and its relocatable C
# library, as on amd64.
ARM64_USER_RUNTIME_OBJS := $(ZEDBSD_SYSROOT_ARM64)/usr/lib/crt0.o \
	$(ZEDBSD_SYSROOT_ARM64)/usr/lib/libc.o
ARM64_USER_SH_SOURCES := $(USERLAND_sh_SOURCES)
ARM64_USER_SH_OBJS := \
	$(patsubst %.c,$(BUILD)/user/%.o,$(ARM64_USER_SH_SOURCES))
ARM64_USER_READLINE_OBJ := $(BUILD)/user/userland/base/libedit/readline.o
ARM64_USER_READLINE_LIB := $(BUILD)/lib/libreadline.a
ARM64_USER_OBJS := $(ARM64_USER_SH_OBJS)

vmunix: $(BUILD)/vmunix
$(BUILD)/tests/rpi4-fdt-host-test: tests/rpi4-fdt-host-test.c \
	src/hal/arm64/bsp-rpi4/fdt.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -Iinclude -Isrc \
 src/hal/arm64/bsp-rpi4/fdt.c $< -o $@

rpi4-fdt-host-test: $(BUILD)/tests/rpi4-fdt-host-test
	$< vendor/raspberrypi-firmware/boot/bcm2711-rpi-4-b.dtb

CHECK_RUN_TARGETS += rpi4-fdt-host-test

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

# libc carries one assembly source on this target, because clang has no
# setjmp builtin for AArch64.
$(BUILD)/src/libc/%.o: src/libc/%.S
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(ARM64_CFLAGS) -D_ASM_SRC_ -c $< -o $@

$(BUILD)/kernel/%.o: %.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(ARM64_CFLAGS) -fno-builtin \
 -fno-strict-aliasing $(ARM64_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kernel/libc/%.o: src/libc/%.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(filter-out -mgeneral-regs-only,$(ARM64_CFLAGS)) \
 -fno-builtin -fno-strict-aliasing $(ARM64_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/%.o: %.c $(ZEDBSD_SYSROOT_ARM64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_USER_CPPFLAGS) $(ARM64_USER_CFLAGS) \
 -fno-strict-aliasing -MMD -MP -c $< -o $@

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

# The base programs are position-independent executables that load
# /lib/libc.so through /lib/ld.so; their objects are the -fPIC ones built
# under $(BUILD)/dynamic/obj.  (The POSIX-R test ELF files stay static.)
# They are linked by the compiler driver, which knows zedBSD's link on this
# target as on amd64 (the loader, the hash style, 4 KiB pages).
ARM64_APP_OBJ := $(BUILD)/dynamic/obj
ARM64_APP_INPUTS := $(ZEDBSD_SYSROOT_ARM64)/usr/lib/crt1.o \
	$(BUILD)/dynamic/libc.so $(BUILD)/dynamic/ld.so \
	tools/build/check-dynamic-elf.py
ARM64_APP_LINK = $(CC) -nostdlib -pie -Wl,--no-relax -Wl,--gc-sections \
 -Wl,--hash-style=sysv,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so $(ZEDBSD_SYSROOT_ARM64)/usr/lib/crt1.o
ARM64_APP_LIBS = -L$(BUILD)/dynamic -Wl,-rpath-link,$(BUILD)/dynamic -l:libc.so
ARM64_APP_CHECK = $(PYTHON) tools/build/check-dynamic-elf.py --machine aarch64 \
 --role application --needed libc.so
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
ARM64_USER_NET_COMMON_OBJS := $(BUILD)/user/userland/base/net/netutil.o \
	$(BUILD)/user/userland/base/net/dhcp.o

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

$(BUILD)/POSIX-R2-REMAINING.ELF: \
	$(ARM64_USER_RUNTIME_OBJS) \
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
$(foreach command,$(USER_STATIC_COMMANDS),\
	$(eval $(call ARM64_USER_STATIC_COMMAND,$(command))))
# No substitute-font file can be opened by the sandboxed preview helper.
$(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user,keiland-preview): ARM64_USER_CPPFLAGS += -DPDF_FONT_FILES=0

# ELF64 runtime linker and shared libc for the aarch64 architecture overlay.
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
	$(ZEDBSD_LIBM_HEADERS)
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(DYNAMIC_CPPFLAGS) \
 $(DYNAMIC_CFLAGS) -c $< -o $@
$(DYNAMIC_FLOAT_DIR)/softfloat.o: src/libc/softfloat.c src/libc/softfloat.h
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(DYNAMIC_CPPFLAGS) \
 $(DYNAMIC_CFLAGS) -c $< -o $@
$(DYNAMIC_FLOAT_DIR)/compiler-runtime.o: src/libc/compiler-runtime.c src/libc/softfloat.h
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(DYNAMIC_CPPFLAGS) \
 $(DYNAMIC_CFLAGS) -c $< -o $@
$(DYNAMIC_FLOAT_DIR)/softfloat128.o: src/libc/softfloat128.c \
	src/libc/softfloat128.h src/libc/softfloat.h
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(DYNAMIC_CPPFLAGS) \
 $(DYNAMIC_CFLAGS) -c $< -o $@
$(DYNAMIC_FLOAT_DIR)/compiler-runtime128.o: src/libc/compiler-runtime128.c \
	src/libc/softfloat128.h src/libc/softfloat.h
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(DYNAMIC_CPPFLAGS) \
 $(DYNAMIC_CFLAGS) -c $< -o $@
$(DYNAMIC_FLOAT_DIR)/float-parse.o: src/libc/float-parse.c src/libc/softfloat.h
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

# Portable libraries and applications use the same graph as amd64.
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

AARCH64_ARCH_INPUTS := $(BUILD)/bin/sh $(BUILD)/bin/sysctl \
	$(BUILD)/bin/mount $(BUILD)/bin/umount \
	$(DYNAMIC_DIR)/ld.so $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/tlstest.so $(DYNAMIC_DIR)/dyntest \
	$(DYNAMIC_DIR)/alt/rpathdep.so $(DYNAMIC_DIR)/rpathtest.so \
	$(DYNAMIC_DIR)/verstest.so $(DYNAMIC_DIR)/versuse.so
AARCH64_ARCH_FILES := --file /bin/sh=$(BUILD)/bin/sh \
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
AARCH64_ARCH_INPUTS += $(addprefix $(BUILD)/bin/,$(USERLAND_SELECTED_NETWORK_PROGRAMS))
AARCH64_ARCH_FILES += $(foreach command,$(USERLAND_SELECTED_NETWORK_PROGRAMS),--file $(call zedbsd_userland_destination,$(command))=$(BUILD)/bin/$(command))
AARCH64_ARCH_INPUTS += $(USER_BASIC_TARGETS) $(USER_STATIC_TARGETS)
AARCH64_ARCH_FILES += $(foreach command,$(USER_BASIC_COMMANDS) $(USER_STATIC_COMMANDS),--file $(call zedbsd_userland_destination,$(command))=$(BUILD)/bin/$(command))
AARCH64_ARCH_FILES += $(ZEDBSD_USERLAND_FILE_MODES)
AARCH64_ARCH_INPUTS += $(ZEDBSD_ACCOUNT_INPUTS)
AARCH64_ARCH_FILES += $(ZEDBSD_ACCOUNT_FILES)
AARCH64_ARCH_INPUTS += $(ZEDBSD_BASE_DATA_INPUTS)
AARCH64_ARCH_FILES += $(ZEDBSD_BASE_DATA_FILES)
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
# Recheck the value on every invocation; retain the mtime when unchanged so
# toggling the option in either direction rebuilds only the necessary image.
.PHONY: rpi4-boot-command-line
rpi4-boot-command-line:

$(BUILD)/cmdline.txt: rpi4-boot-command-line
	@mkdir -p $(dir $@)
	@printf '%s\n' login=$(if $(filter y,$(ZEDBSD_GRAPHICAL_LOGIN)),graphical,console) > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

# The SD card: a FAT boot partition with the firmware and the kernel, and
# the UFS root that the kernel's legacy autoroot finds beside it.
$(BUILD)/hdd-image.img: $(BUILD)/vmunix $(BUILD)/ufs-root.img \
	$(DATA_IMAGE) $(SWAP_IMAGE) $(ARM64_PLATFORM)/config.txt $(BUILD)/cmdline.txt \
	platform/arm64/tools/make-rpi4-ufs-root-hdd-image.py \
	platform/arm64/tools/make-rpi4-hdd-image.py \
	platform/arm64/tools/check-rpi4-hdd-image.py tools/build/check-ufs-image.py
	$(PYTHON) platform/arm64/tools/make-rpi4-ufs-root-hdd-image.py --force \
 --kernel $(BUILD)/vmunix --ufs-root $(BUILD)/ufs-root.img \
 --data-image $(DATA_IMAGE) --swapfile $(SWAP_IMAGE) \
 --config $(ARM64_PLATFORM)/config.txt --cmdline $(BUILD)/cmdline.txt \
 --firmware-dir vendor/raspberrypi-firmware/boot $@

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
	$(PYTHON) platform/arm64/tools/check-arm64-vmunix.py \
 --elf $< --image $@ --fix-image
	$(PYTHON) platform/arm64/tools/check-arm64-vmunix.py --elf $< --image $@

.PHONY: rpi4-fdt-host-test

-include $(ARM64_BOOT_OBJS:.o=.d) $(ARM64_KERNEL_OBJS:.o=.d)

-include $(ARM64_USER_OBJS:.o=.d)
-include $(wildcard $(DYNAMIC_DIR)/obj/*/*/*.d $(DYNAMIC_DIR)/obj/*/*/*/*.d $(DYNAMIC_DIR)/obj/*/*/*/*/*.d)

$(BUILD)/src/hal/pmem-constraints.o: src/hal/pmem-constraints.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(ARM64_CFLAGS) -MMD -MP -c $< -o $@
