# zedBSD riscv64 kernel.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

RISCV64_PLATFORM := platform/riscv64
AWK ?= awk

# The board the HAL is built for.
RISCV64_BOARD_DEFINE_virt := -DHAL_BOARD_VIRT
RISCV64_BSP_DIR_virt := bsp-virt
RISCV64_BOARD_DEFINE_visionfive2 := -DHAL_BOARD_VISIONFIVE2
RISCV64_BSP_DIR_visionfive2 := bsp-visionfive2

# The kernel and the HAL read the compiler's freestanding headers and the
# tree's include/ and src/, never the C library (as on the other ports).
RISCV64_CPPFLAGS := -nostdlibinc -Iinclude -Isrc -I. \
	-Isrc/hal/riscv64 -DHAL_ARCH_RISCV64 $(RISCV64_BOARD_DEFINE_$(ZEDBSD_BOARD)) \
	-DKERN_USER_ABI_RISCV64 -DKERN_USER_ABI_LP64
RISCV64_CPPFLAGS += $(ZEDBSD_CONFIG_CPPFLAGS)

# The kernel's C uses no floating point: the integer ABI, and code that may
# be linked anywhere in the address space (medany), without linker
# relaxation, which would need the global pointer.
RISCV64_CFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany -mno-relax \
	-ffreestanding -fno-pic -fno-pie -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -fno-common \
	-ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror

# Link-time optimization of vmunix: ZEDBSD_KERNEL_LTO_CFLAGS (from the top
# Makefile) goes to every C object of the kernel, its drivers and the HAL;
# the assembly stays native.
RISCV64_KERNEL_LTO_CFLAGS := $(ZEDBSD_KERNEL_LTO_CFLAGS)

RISCV64_BSP := src/hal/riscv64/$(RISCV64_BSP_DIR_$(ZEDBSD_BOARD))
RISCV64_BSP_C_virt := $(RISCV64_BSP)/board.c \
	$(RISCV64_BSP)/fwcfg.c $(RISCV64_BSP)/framebuffer.c
# The VisionFive 2's board: the shared L2 cache controller's maintenance
# and the screen the firmware left on.
RISCV64_BSP_C_visionfive2 := $(RISCV64_BSP)/board.c

RISCV64_BOOT_C := src/hal/riscv64/cmain.c src/hal/riscv64/bootinfo.c \
	src/hal/riscv64/fdt.c src/hal/riscv64/sbi.c src/hal/riscv64/percpu.c \
	src/hal/riscv64/smp.c src/hal/riscv64/int.c src/hal/riscv64/irq.c \
	src/hal/riscv64/plic.c src/hal/riscv64/clock.c src/hal/riscv64/page.c \
	src/hal/riscv64/space.c src/hal/riscv64/window.c src/hal/riscv64/task.c \
	src/hal/riscv64/debug.c \
	src/hal/riscv64/asm.c src/hal/riscv64/lib.c \
	src/hal/riscv64/loader.c src/hal/riscv64/cons.c \
	src/hal/riscv64/fbcons.c src/hal/riscv64/font.c \
	src/hal/idle-suspend-unsupported.c \
	$(RISCV64_BSP_C_$(ZEDBSD_BOARD))
RISCV64_BOOT_S := src/hal/riscv64/locore.S src/hal/riscv64/trap.S \
	src/hal/riscv64/dispatch.S
RISCV64_BOOT_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(RISCV64_BOOT_C)) \
	$(patsubst %.S,$(BUILD)/%.o,$(RISCV64_BOOT_S))

# The platform binding of the board: the teletype console on the UART, the
# initial ramdisk as the boot device, and the GPT reader the native disk
# image needs.
RISCV64_PLATFORM_SOURCES_virt := src/kern/platform/riscv-virt.c \
	src/drivers/serial/ns16550.c src/drivers/generic/serial-console.c \
	src/drivers/generic/ramdisk.c \
	src/drivers/disklabel/gpt.c src/drivers/disklabel/pcat.c
# The VisionFive 2's binding: the console's text over the loader's screen
# and the UART, the UART's input typed into the console (serial-console.c's
# receiver), the initial ramdisk as the boot device, the SoC's system control
# blocks, clock and reset generators and pin controllers, and the SD card
# slot (the DesignWare controller and its JH7110 glue) with the GPT reader
# its UEFI disk image needs, and the headphone jack (the PWM audio converter
# fed by the AXI DMA controller).
RISCV64_PLATFORM_SOURCES_visionfive2 := src/kern/platform/visionfive2.c \
	src/drivers/serial/ns16550.c src/drivers/generic/serial-console.c \
	src/drivers/generic/boot-fb.c src/drivers/generic/fb-text.c \
	src/drivers/platform/pcat/graphics/vgafont.c \
	src/drivers/generic/ramdisk.c \
	src/drivers/platform/visionfive2/vf2-syscon.c \
	src/drivers/platform/visionfive2/vf2-crg.c \
	src/drivers/platform/visionfive2/vf2-pinctrl.c \
	src/drivers/platform/visionfive2/vf2-mmc.c \
	src/drivers/mmc/dw-mmc.c \
	src/drivers/disklabel/gpt.c src/drivers/disklabel/pcat.c \
	src/drivers/dma/dw-axi-dmac.c src/drivers/platform/visionfive2/vf2-dmac.c \
	src/drivers/platform/visionfive2/vf2-pwmdac.c
# /dev/graphics over the loader's screen, when the build has it.
ifeq ($(CONFIG_DRIVER_GRAPHICS_DEVICE),y)
RISCV64_PLATFORM_SOURCES_visionfive2 += src/drivers/generic/graphics-device.c
endif
# The test kernel's DMA coherency check (/dev/vf2-dmatest).
ifeq ($(CONFIG_VF2_DMA_TEST),y)
RISCV64_PLATFORM_SOURCES_visionfive2 += src/drivers/platform/visionfive2/vf2-dmatest.c \
	src/drivers/platform/visionfive2/vf2-dmatest-core.c
endif

# The VisionFive 2's PowerVR GPU core: the power management unit, the
# core's glue, the driver's early stage, its display node and the display
# controller's takeover decision, the later stage that loads and starts
# the firmware, the render node over it, the shading cluster and data
# sequencer instruction encoders, the driver's own compute programs built
# with them, and the Vulkan executor of the render node (with the shared
# wire codec of the i915 executor).
ifeq ($(CONFIG_DRIVER_POWERVR),y)
RISCV64_PLATFORM_SOURCES_visionfive2 += src/drivers/platform/visionfive2/vf2-pmu.c \
	src/drivers/platform/visionfive2/vf2-gpu.c \
	src/drivers/gpu/powervr/powervr.c \
	src/drivers/gpu/powervr/display/display.c \
	src/drivers/gpu/powervr/display/dc8200.c \
	src/drivers/gpu/powervr/display/dc8200-adopt.c \
	src/drivers/gpu/powervr/display/dc8200-flip.c \
	src/drivers/gpu/powervr/display/dc8200-hdmi.c \
	src/drivers/gpu/powervr/display/dc8200-mode.c \
	src/drivers/gpu/powervr/firmware-image.c \
	src/drivers/gpu/powervr/mips-table.c \
	src/drivers/gpu/powervr/retry.c \
	src/drivers/gpu/powervr/stage.c \
	src/drivers/gpu/powervr/stage-log.c \
	src/drivers/gpu/powervr/firmware-memory.c \
	src/drivers/gpu/powervr/firmware-file.c \
	src/drivers/gpu/powervr/firmware-boot.c \
	src/drivers/gpu/powervr/commands.c \
	src/drivers/gpu/powervr/coherency.c \
	src/drivers/gpu/powervr/gpu-mmu.c \
	src/drivers/gpu/powervr/render.c \
	src/drivers/gpu/powervr/render-queue.c \
	src/drivers/gpu/powervr/render-kick-layout.c \
	src/drivers/gpu/powervr/render-sr-program.c \
	src/drivers/gpu/powervr/render-context.c \
	src/drivers/gpu/powervr/render-command.c \
	src/drivers/gpu/powervr/render-job.c \
	src/drivers/gpu/powervr/transfer-context.c \
	src/drivers/gpu/powervr/transfer-job.c \
	src/drivers/gpu/powervr/render-target-layout.c \
	src/drivers/gpu/powervr/render-target.c \
	src/drivers/gpu/powervr/compiler/usc-encode.c \
	src/drivers/gpu/powervr/compiler/pds-encode.c \
	src/drivers/gpu/powervr/compute-program.c \
	src/drivers/gpu/powervr/fixed-area.c \
	src/drivers/gpu/powervr/render-runtime.c \
	src/drivers/gpu/powervr/compiler/shader-spirv.c \
	src/drivers/gpu/powervr/compiler/shader-spirv-interface.c \
	src/drivers/gpu/powervr/compiler/shader-spirv-control.c \
	src/drivers/gpu/powervr/compiler/shader-spirv-instruction.c \
	src/drivers/gpu/powervr/compiler/shader-backend.c \
	src/drivers/gpu/powervr/vk/vk-object.c \
	src/drivers/gpu/powervr/vk/vk-stream.c \
	src/drivers/gpu/powervr/vk/vk-device.c \
	src/drivers/gpu/powervr/vk/vk-memory.c \
	src/drivers/gpu/powervr/vk/vk-image.c \
	src/drivers/gpu/powervr/image-state.c \
	src/drivers/gpu/powervr/descriptor-set.c \
	src/drivers/gpu/powervr/data-program.c \
	src/drivers/gpu/powervr/render-program.c \
	src/drivers/gpu/powervr/transfer-program.c \
	src/drivers/gpu/powervr/render-load-program.c \
	src/drivers/gpu/powervr/vk/vk-layout.c \
	src/drivers/gpu/powervr/vk/vk-command.c \
	src/drivers/gpu/powervr/vk/vk-queue.c \
	src/drivers/gpu/powervr/vk/vk-pass.c \
	src/drivers/gpu/powervr/vk/vk-transfer.c \
	src/drivers/gpu/powervr/render-pass.c \
	src/drivers/gpu/powervr/render-draw.c \
	src/drivers/gpu/powervr/render-depth.c \
	src/drivers/gpu/powervr/vk/vk-draw.c \
	src/drivers/gpu/i915/vk/vk-codec.c
# The test kernel's check of the core's compute path (ws269-p008).
ifeq ($(CONFIG_POWERVR_COMPUTE_CHECK),y)
RISCV64_PLATFORM_SOURCES_visionfive2 += src/drivers/gpu/powervr/compute-check.c
endif
# The dump of what the firmware knows of a context reset (q1532).
ifeq ($(CONFIG_POWERVR_HWR_DUMP),y)
RISCV64_PLATFORM_SOURCES_visionfive2 += src/drivers/gpu/powervr/firmware-hwr.c
endif
# The test kernel's check of the core's render path.
ifeq ($(CONFIG_POWERVR_RENDER_CHECK),y)
RISCV64_PLATFORM_SOURCES_visionfive2 += src/drivers/gpu/powervr/render-check.c
endif
# The display node needs the common GPU core even without a PCI GPU.
RISCV64_PLATFORM_SOURCES_visionfive2 += $(filter-out $(KERN_GPU_SOURCES),src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c)
endif

# The VisionFive 2's first Ethernet port: the DesignWare Ethernet QoS
# controller, its JH7110 glue, and the PHY drivers for its MDIO bus.
RISCV64_PLATFORM_SOURCES_visionfive2 += \
	src/drivers/platform/visionfive2/vf2-ethernet.c \
	src/drivers/ethernet/dw-eqos.c src/drivers/ethernet/mii-phy.c \
	src/drivers/ethernet/yt8531.c

RISCV64_KERNEL_SOURCES := \
	src/kern/main.c \
	$(KERN_FAT_SOURCES) \
	src/kern/inode.c src/kern/file.c src/kern/namecache.c src/kern/namei.c \
	src/kern/mount.c src/kern/tmpfs.c src/kern/tmpfs-pages.c src/drivers/fs/overlayfs.c \
	src/kern/vfs.c src/kern/backing-claim.c \
	src/kern/buf.c src/kern/cache.c src/kern/readahead.c src/kern/writeback.c src/kern/io.c \
	src/kern/sysctl.c src/kern/thermal.c src/kern/resource.c src/kern/poll.c src/kern/usync.c src/kern/disk.c \
	src/kern/device-io.c src/kern/irq.c src/kern/pmem.c \
	src/kern/test-checkpoint.c src/kern/text-display.c \
	src/drivers/generic/loop.c \
	src/kern/partition.c src/drivers/disklabel/mbr.c \
	$(RISCV64_PLATFORM_SOURCES_$(ZEDBSD_BOARD)) \
	src/kern/dcache.c src/kern/uncached.c \
	src/drivers/generic/fdt.c src/drivers/generic/dma.c \
	src/kern/panic.c src/kern/entry.c src/kern/clock.c \
	src/kern/timer.c src/kern/klog.c \
	src/kern/lock.c src/kern/waitq.c \
	src/kern/process.c src/kern/thread.c src/kern/sched.c src/kern/sleep.c src/kern/freeze.c \
	$(KERN_VM_SOURCES) \
	src/kern/filedesc.c src/kern/handle.c src/kern/fd-object.c \
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
	src/kern/tty.c src/kern/tty-csi.c \
	src/drivers/generic/system-device.c src/kern/system-event.c src/drivers/generic/memory-device.c \
	src/kern/shutdown.c \
	src/kern/init.c
RISCV64_KERNEL_SOURCES += $(KERN_NET_SOURCES) $(KERN_BLOCK_IDENTITY_SOURCES) \
	$(KERN_UFS_SOURCES)
RISCV64_KERNEL_SOURCES += $(KERN_BOOT_SOURCES)
RISCV64_KERNEL_SOURCES += $(KERN_ACL_SOURCES)
RISCV64_KERNEL_SOURCES += $(KERN_QUOTA_SOURCES)
ifeq ($(CONFIG_INPUT_TEST_INJECT),y)
RISCV64_KERNEL_SOURCES += src/drivers/generic/input-inject.c
endif
# The camera framework, /dev/camN; the camera source drivers build on it.
ifneq ($(filter y,$(CONFIG_DRIVER_CAMERA) $(CONFIG_DRIVER_USB_UVC)),)
RISCV64_KERNEL_SOURCES += src/drivers/camera/cam.c
endif

# The drivers behind a board's PCI hosts that the configuration selects:
# xHCI with USB keyboards, and NVMe.
RISCV64_PCI_DEVICE_SOURCES :=
ifneq ($(filter y,$(CONFIG_DRIVER_PCI_XHCI) $(CONFIG_DRIVER_USB_HID) $(CONFIG_DRIVER_USB_HUB) $(CONFIG_DRIVER_USB_UVC)),)
RISCV64_PCI_DEVICE_SOURCES += src/drivers/usb/usb.c
endif
ifeq ($(CONFIG_DRIVER_PCI_XHCI),y)
RISCV64_PCI_DEVICE_SOURCES += src/drivers/pci/pci-xhci.c src/drivers/pci/pci-xhci-isoch.c
endif
ifeq ($(CONFIG_DRIVER_USB_HID),y)
RISCV64_PCI_DEVICE_SOURCES += src/drivers/usb/usb-hid.c src/drivers/generic/hidraw.c \
	src/drivers/generic/hidraw-describe.c src/drivers/generic/hid-report.c \
	src/drivers/generic/hid-digitizer.c src/drivers/generic/hid-touch.c \
	src/drivers/generic/hid-input.c
endif
ifeq ($(CONFIG_DRIVER_USB_HUB),y)
RISCV64_PCI_DEVICE_SOURCES += src/drivers/usb/usb-hub.c
endif
# The USB Video Class cameras, on the camera framework.
ifeq ($(CONFIG_DRIVER_USB_UVC),y)
RISCV64_PCI_DEVICE_SOURCES += src/drivers/usb/usb-uvc.c src/drivers/usb/usb-uvc-desc.c src/drivers/usb/usb-uvc-stream.c src/drivers/usb/usb-uvc-payload.c
endif
ifeq ($(CONFIG_DRIVER_PCI_NVME),y)
RISCV64_PCI_DEVICE_SOURCES += src/drivers/pci/pci-nvme.c
endif
# The generic GHCI GPU driver (a PCIe GPU with the GPU Host Controller Interface 1.0, the KC705 board in the
# VisionFive 2's M.2 slot): the device, the ring, the fences, the interrupt, the recovery, the sessions and
# their GPU addresses, the resources, the display (DISP2), the shader compiler and the Vulkan executor (with
# the shared wire codec of the i915 executor, unless the board's own drivers already build it).
ifeq ($(CONFIG_DRIVER_PCI_GHCI),y)
RISCV64_PCI_DEVICE_SOURCES += src/drivers/gpu/ghci/ghci.c src/drivers/gpu/ghci/pci-attach.c src/drivers/gpu/ghci/cp.c \
	src/drivers/gpu/ghci/recover.c src/drivers/gpu/ghci/service.c src/drivers/gpu/ghci/memory.c \
	src/drivers/gpu/ghci/check.c src/drivers/gpu/ghci/caps.c src/drivers/gpu/ghci/cmd.c \
	src/drivers/gpu/ghci/vm.c src/drivers/gpu/ghci/range.c src/drivers/gpu/ghci/share.c \
	src/drivers/gpu/ghci/isa.c src/drivers/gpu/ghci/compiler/gsi1-compile.c \
	src/drivers/gpu/ghci/compiler/gsi1-encode.c src/drivers/gpu/ghci/compiler/gsi1-io.c \
	src/drivers/gpu/ghci/compiler/gsi1-memory.c src/drivers/gpu/ghci/compiler/gsi1-texture.c \
	src/drivers/gpu/ghci/compiler/gsi1-image.c src/drivers/gpu/ghci/compiler/gsi1-schedule.c \
	src/drivers/gpu/ghci/compiler/gsi1-spill.c src/drivers/gpu/compiler/spirv.c src/drivers/gpu/compiler/optimize.c \
	src/drivers/gpu/ghci/jobs.c src/drivers/gpu/ghci/capset.c \
	src/drivers/gpu/ghci/display.c src/drivers/gpu/ghci/display-scan.c src/drivers/gpu/ghci/adv7511.c \
	src/drivers/gpu/ghci/vk/vk-node.c src/drivers/gpu/ghci/vk/vk-stream.c src/drivers/gpu/ghci/vk/vk-dispatch.c \
	src/drivers/gpu/ghci/vk/vk-object.c src/drivers/gpu/ghci/vk/vk-reply.c src/drivers/gpu/ghci/vk/vk-caps.c \
	src/drivers/gpu/ghci/vk/vk-format.c src/drivers/gpu/ghci/vk/vk-device.c src/drivers/gpu/ghci/vk/vk-memory.c \
	src/drivers/gpu/ghci/vk/vk-command.c src/drivers/gpu/ghci/vk/vk-queue.c \
	src/drivers/gpu/ghci/vk/vk-layout.c src/drivers/gpu/ghci/vk/vk-pipeline.c src/drivers/gpu/ghci/vk/vk-pass.c src/drivers/gpu/ghci/vk/vk-execute.c \
	src/drivers/gpu/ghci/vk/vk-meta.c \
	$(filter-out $(RISCV64_PLATFORM_SOURCES_$(ZEDBSD_BOARD)),src/drivers/gpu/i915/vk/vk-codec.c)
endif

# The virt board's ECAM host and the VisionFive 2's two PLDA
# root complexes.
RISCV64_PCI_SOURCES_virt := src/drivers/platform/riscv-virt/virt-pci.c \
	src/drivers/pci/pci.c src/drivers/pci/pci-ecam-fdt.c \
	$(RISCV64_PCI_DEVICE_SOURCES)
RISCV64_PCI_SOURCES_visionfive2 := src/drivers/platform/visionfive2/vf2-pcie.c \
	src/drivers/pci/pci.c src/drivers/pci/pci-plda.c \
	$(RISCV64_PCI_DEVICE_SOURCES)
RISCV64_KERNEL_SOURCES += $(RISCV64_PCI_SOURCES_$(ZEDBSD_BOARD))
RISCV64_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kernel/%.o,$(RISCV64_KERNEL_SOURCES))

# The kernel links no C library object: kcrt and the kernel heap supply
# what it used from libc.
RISCV64_VMUNIX_OBJS := $(RISCV64_BOOT_OBJS) $(RISCV64_KERNEL_OBJS)
$(RISCV64_VMUNIX_OBJS): $(ZEDBSD_PLATFORM_CONFIG_STAMP)
$(RISCV64_VMUNIX_OBJS): $(ZEDBSD_KERNEL_LTO_STAMP)

vmunix: $(BUILD)/vmunix

# The kernel is an ELF the firmware's loader places by its physical
# addresses; the emulator takes it as it is.
$(BUILD)/vmunix: $(RISCV64_VMUNIX_OBJS) $(RISCV64_PLATFORM)/vmunix.ld
	$(RISCV64_LD) -m elf64lriscv --gc-sections --no-relax -z max-page-size=4096 \
 -T $(RISCV64_PLATFORM)/vmunix.ld -nostdlib $(RISCV64_VMUNIX_OBJS) -o $@
	@# A weak undefined symbol is an optional hook, not a link error.
	@test -z "$$($(RISCV64_NM) -u $@ | $(AWK) '$$1 == "U"')" || \
		{ $(RISCV64_NM) -u $@; exit 1; }

$(BUILD)/src/hal/idle-suspend-unsupported.o: src/hal/idle-suspend-unsupported.c
	@mkdir -p $(dir $@)
	$(RISCV64_CC) $(RISCV64_CPPFLAGS) $(RISCV64_CFLAGS) $(RISCV64_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/riscv64/%.o: src/hal/riscv64/%.c
	@mkdir -p $(dir $@)
	$(RISCV64_CC) $(RISCV64_CPPFLAGS) $(RISCV64_CFLAGS) $(RISCV64_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/hal/riscv64/%.o: src/hal/riscv64/%.S
	@mkdir -p $(dir $@)
	$(RISCV64_CC) $(RISCV64_CPPFLAGS) $(RISCV64_CFLAGS) -D_ASM_SRC_ -c $< -o $@

$(BUILD)/kernel/%.o: %.c
	@mkdir -p $(dir $@)
	$(RISCV64_CC) $(RISCV64_CPPFLAGS) $(RISCV64_CFLAGS) -fno-builtin \
 -fno-strict-aliasing $(RISCV64_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

-include $(RISCV64_BOOT_OBJS:.o=.d) $(RISCV64_KERNEL_OBJS:.o=.d)
