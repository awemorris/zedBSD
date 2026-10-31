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

# The board the kernel is built for: the Raspberry Pi 4 (BCM2711), the
# Raspberry Pi 5 (BCM2712) or the ODROID-C5 (Amlogic S905X5M).  The HAL's
# board support is the board's own directory (src/hal/arm64/bsp-rpi4/,
# bsp-rpi5/, bsp-odroidc5/), which is on the include path for its board.h
# and locore-board.h, then the support its SoC family shares
# (bsp-rpi-common/ for the Raspberry Pi boards, bsp-s905x5m-common/ for
# the S905X5M boards), whose directory is on the include path after the
# board's.  The GIC, the generic timer, the early console and the boot
# handoff every board shares are in src/hal/arm64/ itself.  The HAL tells
# the boards apart by HAL_BOARD_RPI4, HAL_BOARD_RPI5 and
# HAL_BOARD_ODROID_C5; the drivers do not.
ARM64_BSP_DIR_rpi4 := bsp-rpi4
ARM64_BSP_DIR_rpi5 := bsp-rpi5
ARM64_BSP_DIR_odroid-c5 := bsp-odroidc5
ARM64_SOC_DIR_rpi4 := bsp-rpi-common
ARM64_SOC_DIR_rpi5 := bsp-rpi-common
ARM64_SOC_DIR_odroid-c5 := bsp-s905x5m-common
ARM64_BOARD_DEFINE_odroid-c5 := -DHAL_BOARD_ODROID_C5 \
	-DCONFIG_DRIVER_MALI_GPU=$(if $(filter y,$(CONFIG_DRIVER_MALI_GPU)),1,0)
ARM64_BOARD_DEFINE_rpi4 := -DHAL_BOARD_RPI4
ARM64_BOARD_DEFINE_rpi5 := -DHAL_BOARD_RPI5 \
	-DCONFIG_DRIVER_RPI5_BT=$(if $(filter y,$(CONFIG_DRIVER_RPI5_BT)),1,0) \
	-DCONFIG_DRIVER_RPI5_FAN=$(if $(filter y,$(CONFIG_DRIVER_RPI5_FAN)),1,0) \
	-DCONFIG_DRIVER_BCM2712_GPU=$(if $(filter y,$(CONFIG_DRIVER_BCM2712_GPU)),1,0)

# The board's own HAL code and its SoC family's.  The Raspberry Pi boards
# take their console's framebuffer from the firmware through the mailbox;
# the S905X5M boards take the one the bootloader left scanning out.
ARM64_BOARD_BOOT_C_rpi4 := \
	src/hal/arm64/bsp-rpi4/uart.c src/hal/arm64/bsp-rpi4/fdt.c \
	src/hal/arm64/bsp-rpi4/activity-led.c src/hal/arm64/bsp-rpi4/board.c \
	src/hal/arm64/bsp-rpi4/watch-hvs.c \
	src/hal/arm64/bsp-rpi-common/mailbox.c \
	src/hal/arm64/bsp-rpi-common/framebuffer-mailbox.c
ARM64_BOARD_BOOT_C_rpi5 := \
	src/hal/arm64/bsp-rpi5/uart.c src/hal/arm64/bsp-rpi5/fdt.c \
	src/hal/arm64/bsp-rpi5/activity-led.c src/hal/arm64/bsp-rpi5/board.c \
	src/hal/arm64/bsp-rpi-common/mailbox.c \
	src/hal/arm64/bsp-rpi-common/framebuffer-mailbox.c
ARM64_BOARD_BOOT_C_odroid-c5 := \
	src/hal/arm64/bsp-odroidc5/activity-led.c src/hal/arm64/bsp-odroidc5/board.c \
	src/hal/arm64/bsp-s905x5m-common/uart.c src/hal/arm64/bsp-s905x5m-common/fdt.c \
	src/hal/arm64/bsp-s905x5m-common/soc.c \
	src/hal/arm64/bsp-s905x5m-common/framebuffer-boot.c

# The BCM43455 driver, on the Pi 4's and the Pi 5's SDIO hosts.
ARM64_BCM43455_SOURCES := src/drivers/wifi/bcm43455/bcm43455.c \
	src/drivers/wifi/bcm43455/bcm43455-bcdc.c \
	src/drivers/wifi/bcm43455/bcm43455-bus.c \
	src/drivers/wifi/bcm43455/bcm43455-chip.c \
	src/drivers/wifi/bcm43455/bcm43455-event.c \
	src/drivers/wifi/bcm43455/bcm43455-files.c \
	src/drivers/wifi/bcm43455/bcm43455-nvram.c \
	src/drivers/wifi/bcm43455/bcm43455-radio.c \
	src/drivers/wifi/bcm43455/bcm43455-sdpcm.c \
	src/drivers/wifi/bcm43455/bcm43455-station.c

# The board's platform binding and the drivers it starts first: on the Pi 4
# the SD card, the console, PCIe and the firmware mailbox; on the Pi 5 the SD
# card, the console, the RP1 south bridge behind PCIe with its USB
# controllers shown to the PCI core and its Ethernet controller, and the
# external PCIe port of the M.2 HATs.
ARM64_BOARD_KERNEL_SOURCES_rpi4 := \
	src/kern/platform/rpi4.c \
	src/drivers/platform/rpi-common/rpi-sdhci.c src/drivers/mmc/sdhci.c \
	src/drivers/platform/rpi-common/rpi-console.c \
	src/drivers/platform/rpi4/rpi4-pcie.c \
	src/drivers/platform/rpi4/rpi4-board.c \
	src/drivers/platform/rpi4/rpi4-firmware.c src/kern/dcache.c \
	src/drivers/platform/rpi-common/rpi-gpio.c \
	src/kern/uncached.c \
	src/drivers/generic/fdt.c src/drivers/generic/dma.c \
	src/drivers/pci/pci.c src/drivers/pci/pci-brcmstb.c
ARM64_BOARD_KERNEL_SOURCES_rpi5 := \
	src/kern/platform/rpi5.c \
	src/drivers/platform/rpi-common/rpi-sdhci.c src/drivers/mmc/sdhci.c \
	src/drivers/platform/rpi-common/rpi-console.c \
	src/drivers/platform/rpi5/rp1.c \
	src/drivers/platform/rpi5/rp1-gpio.c \
	src/drivers/platform/rpi5/rp1-irq.c \
	src/drivers/platform/rpi5/rp1-usb.c \
	src/drivers/platform/rpi5/rp1-ethernet.c \
	src/drivers/ethernet/cadence-gem.c \
	src/drivers/ethernet/mii-phy.c \
	src/drivers/ethernet/bcm54213pe.c \
	src/drivers/platform/rpi5/rpi5-gio.c \
	src/drivers/platform/rpi5/rpi5-pinctrl.c \
	src/drivers/platform/rpi5/rpi5-wifi.c \
	src/drivers/platform/rpi5/rpi5-serror.c \
	src/drivers/platform/rpi5/rpi5-pcie.c \
	src/drivers/platform/rpi5/rpi5-mip.c \
	src/drivers/platform/rpi-common/rpi-mmc-irq.c \
	src/drivers/platform/rpi-common/rpi-sdio-host.c \
	src/drivers/platform/rpi-common/rpi-sdio.c \
	src/drivers/platform/rpi-common/rpi-gpio.c \
	$(ARM64_BCM43455_SOURCES) \
	src/kern/dcache.c \
	src/kern/uncached.c \
	src/drivers/generic/fdt.c src/drivers/generic/dma.c \
	src/drivers/pci/pci.c src/drivers/pci/pci-brcmstb.c
# The ODROID-C5's platform binding and its SoC's drivers: the debug UART's
# console, the SD card, the USB host in front of the xHCI driver, and the
# Ethernet port on the DesignWare EQoS driver.  The SoC's own sources are
# listed by platform/arm64/s905x5m.mk.
include platform/arm64/s905x5m.mk
ARM64_BOARD_KERNEL_SOURCES_odroid-c5 := \
	src/kern/platform/odroid-c5.c \
	$(ARM64_S905X5M_SOURCES) \
	src/kern/dcache.c \
	src/kern/uncached.c \
	src/drivers/generic/fdt.c src/drivers/generic/dma.c
# The console's text over the boot framebuffer, which /dev/graphics also
# draws through when the build has it.
ARM64_BOOTFB_SOURCES := src/drivers/generic/boot-fb.c \
	src/drivers/generic/fb-text.c \
	src/drivers/platform/pcat/graphics/vgafont.c
ifeq ($(CONFIG_DRIVER_GRAPHICS_DEVICE),y)
ARM64_BOOTFB_SOURCES += src/drivers/generic/graphics-device.c
endif
ARM64_BOARD_KERNEL_SOURCES_rpi4 += $(ARM64_BOOTFB_SOURCES)
ARM64_BOARD_KERNEL_SOURCES_rpi5 += $(ARM64_BOOTFB_SOURCES)
ARM64_BOARD_KERNEL_SOURCES_odroid-c5 += $(ARM64_BOOTFB_SOURCES)
# The Mali GPU and its display (platform/arm64/mali.mk), when the build asks for it.
ifeq ($(ZEDBSD_BOARD)$(CONFIG_DRIVER_MALI_GPU),odroid-c5y)
include platform/arm64/mali.mk
ARM64_BOARD_KERNEL_SOURCES_odroid-c5 += \
	$(filter-out $(KERN_GPU_SOURCES),src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c) \
	$(ARM64_MALI_SOURCES)
endif

# The NVMe driver behind the Pi 5's external PCIe port, when the build asks
# for it.
ifeq ($(CONFIG_DRIVER_PCI_NVME),y)
ARM64_BOARD_KERNEL_SOURCES_rpi5 += src/drivers/pci/pci-nvme.c
endif
# The Pi 5's onboard Bluetooth on uarta, when the build asks for it.
ifeq ($(CONFIG_DRIVER_RPI5_BT),y)
ARM64_BOARD_KERNEL_SOURCES_rpi5 += \
	src/drivers/platform/rpi5/rpi5-bt.c \
	src/drivers/platform/rpi-common/rpi-bt-link.c
endif

# The Pi 5's processor temperature and fan connector, when the build asks
# for them.
ifeq ($(CONFIG_DRIVER_RPI5_FAN),y)
ARM64_BOARD_KERNEL_SOURCES_rpi5 += \
	src/drivers/platform/rpi5/rpi5-fan.c
endif

# The kernel and the HAL read the compiler's freestanding headers and the
# tree's include/ and src/, never the C library (as on amd64).  The compiler
# knows the zedBSD target and defines __ZEDBSD__, which selects the zedBSD
# definitions (include/uapi/hosted.h, include/kern/kcrt.h).
ARM64_CPPFLAGS := -nostdlibinc -Iinclude -Isrc -I. \
	-Isrc/hal/arm64 -Isrc/hal/arm64/$(ARM64_BSP_DIR_$(ZEDBSD_BOARD)) \
	-Isrc/hal/arm64/$(ARM64_SOC_DIR_$(ZEDBSD_BOARD)) \
	-DHAL_ARCH_ARM64 $(ARM64_BOARD_DEFINE_$(ZEDBSD_BOARD)) \
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

ARM64_BOOT_C := src/hal/arm64/smp.c src/hal/arm64/percpu.c src/hal/arm64/watch.c src/hal/idle-suspend-unsupported.c src/hal/arm64/asm.c src/hal/arm64/lib.c \
	src/hal/arm64/page.c src/hal/arm64/space.c \
	src/hal/arm64/int.c src/hal/arm64/irq.c \
	src/hal/arm64/task.c \
	src/hal/arm64/cmain.c src/hal/arm64/debug.c \
	src/hal/arm64/cons.c \
	src/hal/arm64/framebuffer.c \
	src/hal/arm64/boot.c src/hal/arm64/gic.c \
	src/hal/arm64/clock.c src/hal/arm64/font.c \
	src/hal/arm64/led.c \
	$(ARM64_BOARD_BOOT_C_$(ZEDBSD_BOARD))
ARM64_BOOT_S := src/hal/arm64/locore.S src/hal/arm64/trap.S \
	src/hal/arm64/dispatch.S
ARM64_BOOT_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(ARM64_BOOT_C)) \
	$(patsubst %.S,$(BUILD)/%.o,$(ARM64_BOOT_S))

ARM64_KERNEL_SOURCES := \
	src/kern/main.c \
	$(KERN_FAT_SOURCES) \
	src/kern/inode.c src/kern/file.c src/kern/namecache.c src/kern/namei.c \
	src/kern/mount.c src/kern/tmpfs.c src/kern/tmpfs-pages.c src/drivers/fs/overlayfs.c \
	src/kern/vfs.c src/kern/backing-claim.c \
 src/kern/buf.c src/kern/cache.c src/kern/readahead.c src/kern/writeback.c src/kern/io.c \
	src/kern/sysctl.c src/kern/thermal.c src/kern/thermal-guard.c src/kern/resource.c src/kern/poll.c src/kern/usync.c src/kern/disk.c \
	src/kern/device-io.c src/kern/irq.c src/kern/pmem.c \
	src/kern/test-checkpoint.c src/kern/text-display.c \
	src/drivers/generic/loop.c \
	src/kern/partition.c src/drivers/disklabel/mbr.c \
	$(ARM64_BOARD_KERNEL_SOURCES_$(ZEDBSD_BOARD)) \
	src/kern/panic.c src/kern/entry.c src/kern/clock.c \
	src/kern/timer.c src/kern/klog.c \
	src/kern/lock.c src/kern/waitq.c \
	src/kern/process.c src/kern/ptrace.c src/kern/thread.c src/kern/sched.c src/kern/sleep.c src/kern/cpuperf.c src/kern/freeze.c $(KERN_VM_SOURCES) \
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
 src/drivers/generic/system-device.c src/kern/system-event.c src/drivers/generic/memory-device.c src/kern/shutdown.c \
	src/kern/init.c
# Onboard Ethernet uses the SCB bus, without PCIe or USB.
ifeq ($(CONFIG_DRIVER_BCM2711_GENET),y)
ARM64_KERNEL_SOURCES += src/drivers/ethernet/bcm54213pe.c \
	src/drivers/platform/rpi4/rpi4-ethernet.c
endif

# The onboard Wi-Fi: the legacy EMMC controller as an SDIO host, the SD
# interrupt line's dispatcher and the chip's bring-up (ws206-p002b), and
# the BCM43455 driver on top of them (ws206-p004).
ifeq ($(CONFIG_DRIVER_BCM2711_WIFI),y)
ARM64_KERNEL_SOURCES += src/drivers/platform/rpi-common/rpi-mmc-irq.c \
	src/drivers/platform/rpi-common/rpi-sdio-host.c \
	src/drivers/platform/rpi-common/rpi-sdio.c \
	src/drivers/platform/rpi4/rpi4-wifi.c \
	$(ARM64_BCM43455_SOURCES)
endif

# The onboard Bluetooth: the PL011 as the controller's HCI transport (ws215-p003).
ifeq ($(CONFIG_DRIVER_BCM2711_BT),y)
ARM64_KERNEL_SOURCES += src/drivers/platform/rpi4/rpi4-bt.c \
	src/drivers/platform/rpi-common/rpi-bt-link.c
endif

# The headphone jack: PWM1 and a legacy DMA channel (ws207-p002).
ifeq ($(CONFIG_DRIVER_BCM2711_AUDIO),y)
ARM64_KERNEL_SOURCES += src/drivers/platform/rpi4/rpi4-audio.c \
	src/drivers/platform/rpi4/rpi4-audio-ring.c
endif

# The VCHIQ link to the VideoCore firmware (ws236-p002).
ifeq ($(CONFIG_DRIVER_BCM2711_VCHIQ),y)
ARM64_KERNEL_SOURCES += src/drivers/platform/rpi4/rpi4-vchiq.c \
	src/drivers/platform/rpi4/rpi4-vchiq-wire.c
endif

# The VideoCore's H.264 decoder over MMAL (ws236-p003), for the video decode
# and the test kernels' probe of it.
ifneq ($(filter y,$(CONFIG_DRIVER_BCM2711_VIDEO) $(CONFIG_DRIVER_BCM2711_VIDEO_PROBE)),)
ARM64_KERNEL_SOURCES += src/drivers/platform/rpi4/rpi4-mmal.c \
	src/drivers/platform/rpi4/rpi4-mmal-wire.c \
	src/drivers/platform/rpi4/rpi4-h264.c
endif
ifeq ($(CONFIG_DRIVER_BCM2711_VIDEO_PROBE),y)
ARM64_KERNEL_SOURCES += src/drivers/platform/rpi4/rpi4-mmal-probe.c \
	src/drivers/platform/rpi4/rpi4-mmal-dump.c
endif

# USB behind the Pi 4's PCIe (ws048): the core comes with any USB driver.
ARM64_USB_SOURCES :=
ifneq ($(filter y,$(CONFIG_DRIVER_PCI_XHCI) $(CONFIG_DRIVER_USB_HID) $(CONFIG_DRIVER_USB_HUB) $(CONFIG_DRIVER_USB_STORAGE) $(CONFIG_DRIVER_USB_CDC_NCM) $(CONFIG_DRIVER_USB_CDC_ECM) $(CONFIG_DRIVER_USB_CCID) $(CONFIG_DRIVER_USB_BT) $(CONFIG_DRIVER_USB_UVC) $(CONFIG_DRIVER_USB_RTL8822BU)),)
ARM64_USB_SOURCES += src/drivers/usb/usb.c
endif
ifeq ($(CONFIG_DRIVER_PCI_XHCI),y)
ARM64_USB_SOURCES += src/drivers/pci/pci-xhci.c src/drivers/pci/pci-xhci-isoch.c
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
ARM64_USB_SOURCES += src/drivers/usb/usb-bt.c
endif
# The USB Video Class cameras, on the camera framework.
ifeq ($(CONFIG_DRIVER_USB_UVC),y)
ARM64_USB_SOURCES += src/drivers/usb/usb-uvc.c src/drivers/usb/usb-uvc-desc.c src/drivers/usb/usb-uvc-stream.c src/drivers/usb/usb-uvc-payload.c
endif
# The Bluetooth HCI class, for a USB controller or the Pi 4's or the Pi 5's onboard one.
ifneq ($(filter y,$(CONFIG_DRIVER_USB_BT) $(CONFIG_DRIVER_BCM2711_BT) $(if $(filter rpi5,$(ZEDBSD_BOARD)),$(CONFIG_DRIVER_RPI5_BT))),)
ARM64_KERNEL_SOURCES += src/drivers/generic/bt-hci.c src/drivers/generic/bt-hci-proto.c
endif
ifeq ($(CONFIG_DRIVER_USB_RTL8822BU),y)
ARM64_USB_SOURCES += src/drivers/usb/usb-rtl8822bu.c \
	src/drivers/wifi/rtl8822b/rtl8822b.c src/drivers/wifi/rtl8822b/rtl8822b-security.c
endif
ARM64_KERNEL_SOURCES += $(ARM64_USB_SOURCES)
# The camera framework, /dev/camN; the camera source drivers build on it.
ifneq ($(filter y,$(CONFIG_DRIVER_CAMERA) $(CONFIG_DRIVER_USB_UVC)),)
ARM64_KERNEL_SOURCES += src/drivers/camera/cam.c
endif
# The test-only input injection (aat-input), as on amd64.
ifeq ($(CONFIG_INPUT_TEST_INJECT),y)
ARM64_KERNEL_SOURCES += src/drivers/generic/input-inject.c
endif
# /dev/input/bridge (ws143-p005, ws215-p002): the Bluetooth daemon's HID devices, on the HID input glue; it refuses a FIDO
# descriptor with hidraw-describe.c.  Both come with usb-hid, or here without it.
ifeq ($(CONFIG_INPUT_BRIDGE),y)
ARM64_KERNEL_SOURCES += src/drivers/generic/input-bridge.c src/drivers/generic/input-bridge-setup.c
ifneq ($(CONFIG_DRIVER_USB_HID),y)
ARM64_KERNEL_SOURCES += src/drivers/generic/hidraw-describe.c src/drivers/generic/hid-report.c \
	src/drivers/generic/hid-digitizer.c src/drivers/generic/hid-touch.c src/drivers/generic/hid-input.c
endif
endif
# The BCM2711 graphics driver (ws141): the display path and V3D.
ifeq ($(CONFIG_DRIVER_BCM2711_GPU),y)
# Native BCM2711 nodes need the common GPU core even without a PCI GPU.
ARM64_KERNEL_SOURCES += $(filter-out $(KERN_GPU_SOURCES),src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c)
# The shared Zlib scalar parser has no device or Gen12 code-generation dependencies.
ARM64_KERNEL_SOURCES += $(filter-out $(KERN_GPU_SOURCES),src/drivers/gpu/compiler/spirv.c src/drivers/gpu/compiler/optimize.c src/drivers/gpu/i915/vk/vk-codec.c)
ARM64_KERNEL_SOURCES += src/drivers/gpu/bcm2711/attach.c \
	src/drivers/gpu/bcm2711/stage.c src/drivers/gpu/bcm2711/fdt-util.c \
	src/drivers/gpu/bcm2711/firmware.c src/drivers/gpu/bcm2711/display/display.c \
	src/drivers/gpu/bcm2711/display/readout.c src/drivers/gpu/bcm2711/display/list.c \
	src/drivers/gpu/bcm2711/v3d.c src/drivers/gpu/bcm2711/v3d-power.c \
	src/drivers/gpu/bcm2711/v3d-hardware.c src/drivers/gpu/bcm2711/mmu.c \
	src/drivers/gpu/bcm2711/v3d-job.c src/drivers/gpu/bcm2711/v3d-diagnostic.c src/drivers/gpu/bcm2711/v3d-tfu-probe.c \
	src/drivers/gpu/bcm2711/cl.c src/drivers/gpu/bcm2711/compiler/qpu.c src/drivers/gpu/bcm2711/buffer.c \
	src/drivers/gpu/bcm2711/compiler/shader.c src/drivers/gpu/bcm2711/compiler/shader-analyze.c \
	src/drivers/gpu/bcm2711/compiler/shader-lower.c src/drivers/gpu/bcm2711/compiler/shader-output.c src/drivers/gpu/bcm2711/compiler/shader-schedule.c \
	src/drivers/gpu/bcm2711/native-bin.c src/drivers/gpu/bcm2711/native-pass.c src/drivers/gpu/bcm2711/native-colour.c src/drivers/gpu/bcm2711/native-shader.c src/drivers/gpu/bcm2711/native-texture.c src/drivers/gpu/bcm2711/native-miptree.c src/drivers/gpu/bcm2711/native-storage.c src/drivers/gpu/bcm2711/native-viewport.c \
	src/drivers/gpu/bcm2711/vk/vulkan-object.c src/drivers/gpu/bcm2711/vk/vulkan-session.c \
	src/drivers/gpu/bcm2711/vk/vulkan-stream.c src/drivers/gpu/bcm2711/vk/vulkan-dispatch.c src/drivers/gpu/bcm2711/vk/vulkan-device.c src/drivers/gpu/bcm2711/vk/vulkan-sync.c src/drivers/gpu/bcm2711/vk/vulkan-queue.c src/drivers/gpu/bcm2711/vk/vulkan-query.c src/drivers/gpu/bcm2711/vk/vulkan-memory.c src/drivers/gpu/bcm2711/vk/vulkan-resource.c src/drivers/gpu/bcm2711/vk/vulkan-input.c src/drivers/gpu/bcm2711/vk/vulkan-layout.c src/drivers/gpu/bcm2711/vk/vulkan-layout-compat.c src/drivers/gpu/bcm2711/vk/vulkan-descriptor-pool.c src/drivers/gpu/bcm2711/vk/vulkan-descriptor-sets.c src/drivers/gpu/bcm2711/vk/vulkan-descriptor-update.c src/drivers/gpu/bcm2711/vk/vulkan-target.c \
	src/drivers/gpu/bcm2711/vk/vulkan-pipeline-build.c src/drivers/gpu/bcm2711/vk/vulkan-pipeline-state.c src/drivers/gpu/bcm2711/vk/vulkan-pipeline.c src/drivers/gpu/bcm2711/vk/vulkan-pipeline-decode.c \
	src/drivers/gpu/bcm2711/vk/vulkan-command-pool.c src/drivers/gpu/bcm2711/vk/vulkan-command-batch.c src/drivers/gpu/bcm2711/vk/vulkan-command-buffer.c \
	src/drivers/gpu/bcm2711/vk/vulkan-barrier.c src/drivers/gpu/bcm2711/vk/vulkan-barrier-decode.c src/drivers/gpu/bcm2711/vk/vulkan-barrier-validate.c src/drivers/gpu/bcm2711/vk/vulkan-record.c src/drivers/gpu/bcm2711/vk/vulkan-record-decode.c src/drivers/gpu/bcm2711/vk/vulkan-record-validate.c src/drivers/gpu/bcm2711/vk/vulkan-draw.c src/drivers/gpu/bcm2711/vk/vulkan-draw-validate.c src/drivers/gpu/bcm2711/vk/vulkan-prepared.c src/drivers/gpu/bcm2711/vk/vulkan-uniform.c src/drivers/gpu/bcm2711/vk/vulkan-native-draw.c src/drivers/gpu/bcm2711/vk/vulkan-native-bin.c src/drivers/gpu/bcm2711/vk/vulkan-native-pass.c src/drivers/gpu/bcm2711/vk/vulkan-native-execute.c src/drivers/gpu/bcm2711/vk/vulkan-native-job.c src/drivers/gpu/bcm2711/vk/vulkan-buffer-copy.c src/drivers/gpu/bcm2711/vk/vulkan-transfer.c src/drivers/gpu/bcm2711/vk/vulkan-native-image.c \
	src/drivers/gpu/bcm2711/share.c src/drivers/gpu/bcm2711/v3d-memory.c \
	src/drivers/gpu/bcm2711/render-device.c src/drivers/gpu/bcm2711/render-runtime.c src/drivers/gpu/bcm2711/render-worker.c \
	src/drivers/gpu/bcm2711/display/display-program.c \
	src/drivers/gpu/bcm2711/display/display-start.c \
	src/drivers/gpu/bcm2711/display/display-execute.c \
	src/drivers/gpu/bcm2711/display/display-flip.c \
	src/drivers/gpu/bcm2711/display/display-device.c \
	src/drivers/gpu/bcm2711/display/display-irq.c
endif
# The Raspberry Pi 5's V3D 7.1 driver (ws279): the 7.1 engine files, which keep the names of the Pi 4's 4.2
# hardware and job files and take their place, with the shared device-tree, stage-mark, page-table and buffer
# files; and the shader compiler (ws279-p004): the version-independent compiler files and the 7.1 copies of the
# encoder, lowering, epilogues and scheduler, which take the place of the 4.2 ones.  Only the Pi 5 builds it.
ifeq ($(ZEDBSD_BOARD)$(CONFIG_DRIVER_BCM2712_GPU),rpi5y)
ARM64_KERNEL_SOURCES += $(filter-out $(KERN_GPU_SOURCES),src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c)
ARM64_KERNEL_SOURCES += src/drivers/gpu/bcm2711/attach71.c src/drivers/gpu/bcm2711/v3d71.c \
	src/drivers/gpu/bcm2711/v3d71-firmware.c src/drivers/gpu/bcm2711/v3d71-hardware.c \
	src/drivers/gpu/bcm2711/v3d71-job.c src/drivers/gpu/bcm2711/v3d71-cl.c \
	src/drivers/gpu/bcm2711/v3d71-diagnostic.c src/drivers/gpu/bcm2711/display/hvs6.c \
	src/drivers/gpu/bcm2711/display/hvs6-flip.c src/drivers/gpu/bcm2711/display/display-device.c \
	src/drivers/gpu/bcm2711/share.c \
	src/drivers/gpu/bcm2711/native71-bin.c src/drivers/gpu/bcm2711/native71-pass.c src/drivers/gpu/bcm2711/native-colour.c \
	src/drivers/gpu/bcm2711/native71-shader.c src/drivers/gpu/bcm2711/native71-texture.c src/drivers/gpu/bcm2711/native-miptree.c src/drivers/gpu/bcm2711/native-storage.c \
	src/drivers/gpu/bcm2711/native71-viewport.c \
	src/drivers/gpu/bcm2711/vk/vulkan-object.c src/drivers/gpu/bcm2711/vk/vulkan-session.c \
	src/drivers/gpu/bcm2711/vk/vulkan-stream.c src/drivers/gpu/bcm2711/vk/vulkan-dispatch.c src/drivers/gpu/bcm2711/vk/vulkan-device.c src/drivers/gpu/bcm2711/vk/vulkan-sync.c src/drivers/gpu/bcm2711/vk/vulkan-queue.c src/drivers/gpu/bcm2711/vk/vulkan-query.c src/drivers/gpu/bcm2711/vk/vulkan-memory.c src/drivers/gpu/bcm2711/vk/vulkan-resource.c src/drivers/gpu/bcm2711/vk/vulkan-input.c src/drivers/gpu/bcm2711/vk/vulkan-layout.c src/drivers/gpu/bcm2711/vk/vulkan-layout-compat.c src/drivers/gpu/bcm2711/vk/vulkan-descriptor-pool.c src/drivers/gpu/bcm2711/vk/vulkan-descriptor-sets.c src/drivers/gpu/bcm2711/vk/vulkan-descriptor-update.c src/drivers/gpu/bcm2711/vk/vulkan-target.c \
	src/drivers/gpu/bcm2711/vk/vulkan-pipeline-build.c src/drivers/gpu/bcm2711/vk/vulkan-pipeline-state.c src/drivers/gpu/bcm2711/vk/vulkan-pipeline.c src/drivers/gpu/bcm2711/vk/vulkan-pipeline-decode.c \
	src/drivers/gpu/bcm2711/vk/vulkan-command-pool.c src/drivers/gpu/bcm2711/vk/vulkan-command-batch.c src/drivers/gpu/bcm2711/vk/vulkan-command-buffer.c \
	src/drivers/gpu/bcm2711/vk/vulkan-barrier.c src/drivers/gpu/bcm2711/vk/vulkan-barrier-decode.c src/drivers/gpu/bcm2711/vk/vulkan-barrier-validate.c src/drivers/gpu/bcm2711/vk/vulkan-record.c src/drivers/gpu/bcm2711/vk/vulkan-record-decode.c src/drivers/gpu/bcm2711/vk/vulkan-record-validate.c src/drivers/gpu/bcm2711/vk/vulkan-draw.c src/drivers/gpu/bcm2711/vk/vulkan-draw-validate.c src/drivers/gpu/bcm2711/vk/vulkan-prepared.c src/drivers/gpu/bcm2711/vk/vulkan-uniform.c src/drivers/gpu/bcm2711/vk/vulkan-native-draw.c src/drivers/gpu/bcm2711/vk/vulkan-native-bin.c src/drivers/gpu/bcm2711/vk/vulkan-native-pass.c src/drivers/gpu/bcm2711/vk/vulkan-native-execute.c src/drivers/gpu/bcm2711/vk/vulkan-native-job.c src/drivers/gpu/bcm2711/vk/vulkan-buffer-copy.c src/drivers/gpu/bcm2711/vk/vulkan-transfer.c src/drivers/gpu/bcm2711/vk/vulkan-native-image.c \
	src/drivers/gpu/bcm2711/v3d-memory.c \
	src/drivers/gpu/bcm2711/render-device.c src/drivers/gpu/bcm2711/render-runtime.c src/drivers/gpu/bcm2711/render-worker.c \
	src/drivers/gpu/bcm2711/stage.c src/drivers/gpu/bcm2711/fdt-util.c \
	src/drivers/gpu/bcm2711/mmu.c src/drivers/gpu/bcm2711/buffer.c
ARM64_KERNEL_SOURCES += $(filter-out $(KERN_GPU_SOURCES),src/drivers/gpu/compiler/spirv.c src/drivers/gpu/compiler/optimize.c src/drivers/gpu/i915/vk/vk-codec.c)
ARM64_KERNEL_SOURCES += src/drivers/gpu/bcm2711/compiler/qpu71.c \
	src/drivers/gpu/bcm2711/compiler/shader.c src/drivers/gpu/bcm2711/compiler/shader-analyze.c \
	src/drivers/gpu/bcm2711/compiler/shader71-lower.c src/drivers/gpu/bcm2711/compiler/shader71-output.c \
	src/drivers/gpu/bcm2711/compiler/shader71-schedule.c
endif
# The H.264 decode as Vulkan Video on the V3D device (ws236-p004), with the VideoCore firmware's decoder.
ifeq ($(CONFIG_DRIVER_BCM2711_GPU)$(CONFIG_DRIVER_BCM2711_VIDEO),yy)
ARM64_KERNEL_SOURCES += src/drivers/gpu/bcm2711/vk/vulkan-video.c
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
	src/hal/arm64/bsp-rpi4/fdt.c src/hal/arm64/fdt.h src/drivers/generic/fdt.c \
	src/drivers/platform/rpi4/rpi4-board.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -Iinclude -Isrc \
 src/hal/arm64/bsp-rpi4/fdt.c src/drivers/generic/fdt.c \
 src/drivers/platform/rpi4/rpi4-board.c $< -o $@

rpi4-fdt-host-test: $(BUILD)/tests/rpi4-fdt-host-test $(RPI4_BOOT_FIRMWARE_DTB)
	$< $(RPI4_BOOT_FIRMWARE_DTB)

CHECK_RUN_TARGETS += rpi4-fdt-host-test

# The Pi 5's trees (C1 and D0) through the HAL's parser built for the Pi 5.
ifeq ($(ZEDBSD_BOARD),rpi5)
$(BUILD)/tests/rpi5-fdt-host-test: tests/rpi5-fdt-host-test.c \
	src/hal/arm64/bsp-rpi5/fdt.c src/hal/arm64/fdt.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -Iinclude -Isrc \
 src/hal/arm64/bsp-rpi5/fdt.c $< -o $@

.PHONY: rpi5-fdt-host-test
rpi5-fdt-host-test: $(BUILD)/tests/rpi5-fdt-host-test $(RPI5_BOOT_FIRMWARE_DTB) $(RPI5_BOOT_FIRMWARE_D0_DTB)
	$< $(RPI5_BOOT_FIRMWARE_DTB)
	$< $(RPI5_BOOT_FIRMWARE_D0_DTB)

CHECK_RUN_TARGETS += rpi5-fdt-host-test

# RP1's module (rp1.c, rp1-gpio.c, rp1-irq.c) on the Pi 5's trees, against
# a root complex kept in host memory.
$(BUILD)/tests/rpi5-rp1-host-test: tests/rpi5-rp1-host-test.c \
	src/drivers/platform/rpi5/rp1.c src/drivers/platform/rpi5/rp1-gpio.c \
	src/drivers/platform/rpi5/rp1-irq.c src/drivers/platform/rpi5/rp1-irq.h \
	src/drivers/platform/rpi5/rp1.h src/drivers/platform/rpi5/rp1-gpio.h \
	src/drivers/generic/fdt.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -Iinclude -Isrc \
 src/drivers/platform/rpi5/rp1.c src/drivers/platform/rpi5/rp1-gpio.c \
 src/drivers/platform/rpi5/rp1-irq.c src/drivers/generic/fdt.c $< -o $@

.PHONY: rpi5-rp1-host-test
rpi5-rp1-host-test: $(BUILD)/tests/rpi5-rp1-host-test $(RPI5_BOOT_FIRMWARE_DTB) $(RPI5_BOOT_FIRMWARE_D0_DTB)
	$< $(RPI5_BOOT_FIRMWARE_DTB)
	$< $(RPI5_BOOT_FIRMWARE_D0_DTB)

CHECK_RUN_TARGETS += rpi5-rp1-host-test
endif

# The SDHCI driver on the host against a fake controller and card: the
# driver is copied without its include lines, and the test supplies the
# kernel types and functions it uses, the fake registers and the fake clock.
$(BUILD)/tests/sdhci-under-test.c: src/drivers/mmc/sdhci.c
	@mkdir -p $(dir $@)
	sed -e '/^#include/d' $< > $@

$(BUILD)/tests/sdhci-host-test: tests/sdhci-host-test.c \
	$(BUILD)/tests/sdhci-under-test.c include/drivers/mmc/sdhci.h
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -Iinclude -I$(BUILD)/tests $< -o $@

sdhci-host-test: $(BUILD)/tests/sdhci-host-test
	$<

CHECK_RUN_TARGETS += sdhci-host-test

# The Cadence GEM driver on the host with the Pi 5's glue and the PHY
# drivers, against a fake controller, fake PHYs and fake DMA memory: each
# driver is copied without its include lines and compiled with the test's
# view of the kernel in front of it.  The Pi 5's trace must match the one
# recorded from the driver before its generic part was split from the glue
# (the error numbers in it are the Linux host's).
CADENCE_GEM_HOST_TEST_DRIVERS := src/drivers/ethernet/cadence-gem.c \
	src/drivers/platform/rpi5/rp1-ethernet.c \
	src/drivers/ethernet/bcm54213pe.c \
	src/drivers/ethernet/mii-phy.c

$(BUILD)/tests/cadence-gem-host-test: tests/cadence-gem-host-test.c \
	tests/cadence-gem-host-test.h $(CADENCE_GEM_HOST_TEST_DRIVERS) \
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
# Recheck the value on every invocation; retain the mtime when unchanged so
# toggling the option in either direction rebuilds only the necessary image.
.PHONY: rpi4-boot-command-line
rpi4-boot-command-line:

$(BUILD)/cmdline.txt: rpi4-boot-command-line
	@mkdir -p $(dir $@)
	@printf '%s\n' login=$(if $(filter y,$(ZEDBSD_GRAPHICAL_LOGIN)),graphical,console) > $@.tmp
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

-include $(ARM64_BOOT_OBJS:.o=.d) $(ARM64_KERNEL_OBJS:.o=.d)

-include $(ARM64_USER_OBJS:.o=.d)
-include $(wildcard $(DYNAMIC_DIR)/obj/*/*/*.d $(DYNAMIC_DIR)/obj/*/*/*/*.d $(DYNAMIC_DIR)/obj/*/*/*/*/*.d)

$(BUILD)/src/hal/pmem-constraints.o: src/hal/pmem-constraints.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CPPFLAGS) $(ARM64_CFLAGS) -MMD -MP -c $< -o $@
