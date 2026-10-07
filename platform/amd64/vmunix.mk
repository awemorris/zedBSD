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

# Variant is an image-composition input only. This content-stable stamp
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
EFI_CC := $(ZEDBSD_TARGET_LLVM_BIN)/clang \
	--target=x86_64-unknown-windows
EFI_LD := $(ZEDBSD_TARGET_LLVM_BIN)/lld-link
EFI_NM := $(ZEDBSD_TARGET_LLVM_BIN)/llvm-nm
EFI_CFLAGS := -std=c11 -ffreestanding -fshort-wchar -mno-red-zone \
	-fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -fno-ident -ffunction-sections -fdata-sections \
	-Os -Wall -Wextra -Werror -I.

# The kernel and the HAL read the compiler's freestanding headers (stdint.h,
# stddef.h, stdbool.h, stdarg.h, limits.h) and the tree's own include/ and
# src/, never the C library: -nostdlibinc keeps only the compiler's resource
# directory from the system search list, so the sysroot is not read.
AMD64_CPPFLAGS := -nostdlibinc \
	-Iinclude -Isrc -I. \
	-DHAL_ARCH_AMD64 -DHAL_BOARD_PCAT -DHAL_PCAT_DEBUGCON \
	-DKERN_USER_ABI_LP64 \
	-DPCAT_VGA_APERTURE_ADDRESS=0xffffffffc1400000ULL \
	-DPCAT_CIRRUS_APERTURE_ADDRESS=0xffffffffc0000000ULL
AMD64_CPPFLAGS += $(ZEDBSD_CONFIG_CPPFLAGS)
AMD64_CFLAGS := -m64 -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
	-ffreestanding -fno-pic -fno-pie -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror \
	-Wframe-larger-than=8192 -fno-builtin
AMD64_KERNEL_LIBC_CFLAGS := $(filter-out -mgeneral-regs-only,$(AMD64_CFLAGS))
# Link-time optimization of vmunix (ws053): ZEDBSD_KERNEL_LTO_CFLAGS (from the
# top Makefile) goes to every C object of the kernel, its drivers and the
# HAL; the assembly stays native.
AMD64_KERNEL_LTO_CFLAGS := $(ZEDBSD_KERNEL_LTO_CFLAGS)

AMD64_HAL_SOURCES := src/hal/x86/rtc.c src/hal/x86/boot-parameters.c \
	src/hal/x86/io.c \
	src/hal/amd64/asm.c src/hal/amd64/lib.c \
	src/hal/amd64/page.c src/hal/amd64/pmem-range.c \
	src/hal/amd64/ram-map.c src/hal/amd64/framebuffer-map.c \
	src/hal/amd64/space.c src/hal/amd64/image.c \
	src/hal/amd64/acpi-window.c src/hal/amd64/cmain.c \
	src/hal/amd64/descriptor.c src/hal/amd64/int.c src/hal/amd64/irq.c \
	src/hal/amd64/msi-source.c \
	src/hal/amd64/task.c src/hal/amd64/percpu.c src/hal/amd64/smp.c \
	src/hal/amd64/bsp-pcat/boot.c \
	src/hal/amd64/bsp-pcat/handoff-validation.c \
	src/hal/amd64/bsp-pcat/cons.c \
	src/hal/amd64/bsp-pcat/pic.c src/hal/amd64/bsp-pcat/clock.c \
	src/hal/amd64/bsp-pcat/acpi.c src/hal/amd64/bsp-pcat/lapic.c \
	src/hal/amd64/bsp-pcat/idle-suspend.c \
	src/hal/amd64/bsp-pcat/early-init-policy.c \
	src/hal/amd64/bsp-pcat/timecounter-policy.c \
	src/hal/amd64/bsp-pcat/timecounter.c \
	src/hal/amd64/bsp-pcat/mcfg.c \
	src/hal/amd64/bsp-pcat/ioapic.c
AMD64_HAL_ASM := src/hal/amd64/locore.S src/hal/amd64/trap.S \
	src/hal/amd64/dispatch.S src/hal/amd64/ap-trampoline.S
AMD64_HAL_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(AMD64_HAL_SOURCES)) \
	$(patsubst %.S,$(BUILD)/%.o,$(AMD64_HAL_ASM))

AMD64_USB_HCD_SOURCES :=
ifeq ($(CONFIG_DRIVER_PCI_UHCI),y)
AMD64_USB_HCD_SOURCES += src/drivers/pci/pci-uhci.c
endif
ifeq ($(CONFIG_DRIVER_PCI_EHCI),y)
AMD64_USB_HCD_SOURCES += src/drivers/pci/pci-ehci.c
endif
ifeq ($(CONFIG_DRIVER_PCI_XHCI),y)
AMD64_USB_HCD_SOURCES += src/drivers/pci/pci-xhci.c
endif
AMD64_USB_CLASS_SOURCES :=
ifeq ($(CONFIG_DRIVER_USB_STORAGE),y)
AMD64_USB_CLASS_SOURCES += src/drivers/usb/usb-storage.c
AMD64_USB_CLASS_SOURCES += src/drivers/usb/usb-uas.c src/drivers/usb/usb-uas-transport.c src/drivers/usb/usb-uas-disk.c
endif
AMD64_NVME_SOURCES :=
ifeq ($(CONFIG_DRIVER_PCI_NVME),y)
AMD64_NVME_SOURCES += src/drivers/pci/pci-nvme.c
endif
AMD64_HDA_SOURCES :=
ifeq ($(CONFIG_DRIVER_PCI_HDA),y)
AMD64_HDA_SOURCES += src/drivers/pci/pci-hda.c
endif
AMD64_VENUS_SOURCES :=
ifeq ($(CONFIG_DRIVER_PCI_VENUS),y)
AMD64_VENUS_SOURCES += src/drivers/gpu/venus/transport.c \
	src/drivers/gpu/venus/venus.c src/drivers/gpu/venus/display.c src/drivers/gpu/venus/share.c
endif
AMD64_I915_SOURCES :=
ifeq ($(CONFIG_DRIVER_PCI_I915),y)
AMD64_I915_SOURCES += src/drivers/gpu/i915/command.c src/drivers/gpu/i915/compiler/compile.c src/drivers/gpu/i915/compiler/eu.c src/drivers/gpu/i915/compiler/spirv.c src/drivers/gpu/i915/context.c src/drivers/gpu/i915/defaults.c src/drivers/gpu/i915/device.c src/drivers/gpu/i915/device-info.c src/drivers/gpu/i915/display/aux.c src/drivers/gpu/i915/display/backlight.c src/drivers/gpu/i915/display/clock.c src/drivers/gpu/i915/display/color.c src/drivers/gpu/i915/display/control.c src/drivers/gpu/i915/display/dc9.c src/drivers/gpu/i915/display/ddi.c src/drivers/gpu/i915/display/diagnostics.c src/drivers/gpu/i915/display/display.c src/drivers/gpu/i915/display/dmc.c src/drivers/gpu/i915/display/dp.c src/drivers/gpu/i915/display/dp-sink.c src/drivers/gpu/i915/display/edid.c src/drivers/gpu/i915/display/edid-read.c src/drivers/gpu/i915/display/gmbus.c src/drivers/gpu/i915/display/hdmi.c src/drivers/gpu/i915/display/hdmi-mode.c src/drivers/gpu/i915/display/hotplug.c src/drivers/gpu/i915/display/interrupts.c src/drivers/gpu/i915/display/modeset.c src/drivers/gpu/i915/display/opregion.c src/drivers/gpu/i915/display/output.c src/drivers/gpu/i915/display/panel-backlight.c src/drivers/gpu/i915/display/panel.c src/drivers/gpu/i915/display/phy.c src/drivers/gpu/i915/display/pipe.c src/drivers/gpu/i915/display/plane.c src/drivers/gpu/i915/display/power.c src/drivers/gpu/i915/display/present.c src/drivers/gpu/i915/display/scanout.c src/drivers/gpu/i915/display/state.c src/drivers/gpu/i915/display/takeover.c src/drivers/gpu/i915/display/vblank.c src/drivers/gpu/i915/display/vbt.c src/drivers/gpu/i915/display/watermark.c src/drivers/gpu/i915/dma.c src/drivers/gpu/i915/engine.c src/drivers/gpu/i915/firmware.c src/drivers/gpu/i915/ggtt.c src/drivers/gpu/i915/gt-power.c src/drivers/gpu/i915/i915.c src/drivers/gpu/i915/irq.c src/drivers/gpu/i915/job.c src/drivers/gpu/i915/memory.c src/drivers/gpu/i915/migrate.c src/drivers/gpu/i915/mmio.c src/drivers/gpu/i915/park.c src/drivers/gpu/i915/pci.c src/drivers/gpu/i915/perf.c src/drivers/gpu/i915/power.c src/drivers/gpu/i915/ppgtt.c src/drivers/gpu/i915/pxp.c src/drivers/gpu/i915/render/batch.c src/drivers/gpu/i915/render/blit.c src/drivers/gpu/i915/render/codec.c src/drivers/gpu/i915/render/command.c src/drivers/gpu/i915/render/compute.c src/drivers/gpu/i915/render/descriptor.c src/drivers/gpu/i915/render/dispatch.c src/drivers/gpu/i915/render/draw.c src/drivers/gpu/i915/render/fence.c src/drivers/gpu/i915/render/forget.c src/drivers/gpu/i915/render/image.c src/drivers/gpu/i915/render/instance.c src/drivers/gpu/i915/render/math.c src/drivers/gpu/i915/render/memory.c src/drivers/gpu/i915/render/object.c src/drivers/gpu/i915/render/objects.c src/drivers/gpu/i915/render/pipeline.c src/drivers/gpu/i915/render/pipeline-prepare.c src/drivers/gpu/i915/render/render-pass.c src/drivers/gpu/i915/render/reply.c src/drivers/gpu/i915/render/state.c src/drivers/gpu/i915/render/sync.c src/drivers/gpu/i915/render/transport.c src/drivers/gpu/i915/render/video.c src/drivers/gpu/i915/render/video-h264-tables.c src/drivers/gpu/i915/render/video-mfx.c src/drivers/gpu/i915/render/vulkan.c src/drivers/gpu/i915/request.c src/drivers/gpu/i915/request-queue.c src/drivers/gpu/i915/reset.c src/drivers/gpu/i915/resource.c src/drivers/gpu/i915/runtime-pm.c src/drivers/gpu/i915/session.c src/drivers/gpu/i915/submit.c src/drivers/gpu/i915/sync.c src/drivers/gpu/i915/tlb.c src/drivers/gpu/i915/trace.c src/drivers/gpu/i915/verify-workarounds.c src/drivers/gpu/i915/workarounds.c src/drivers/gpu/i915/worker.c src/drivers/gpu/i915/workqueue.c
# The Type-C ports and the Dekel PHY access of the display (ws051-p002b).
AMD64_I915_SOURCES += src/drivers/gpu/i915/display/dkl-phy.c src/drivers/gpu/i915/display/tc.c src/drivers/gpu/i915/display/tc-kern.c
# The external DP ports on the Type-C ports: the sink probe and its binding (ws051-p004a).
AMD64_I915_SOURCES += src/drivers/gpu/i915/display/dp-ext.c src/drivers/gpu/i915/display/dp-ext-kern.c
AMD64_I915_SOURCES += src/drivers/gpu/i915/display/head.c src/drivers/gpu/i915/display/head-rules.c
endif
# The i915 test build: checkpoints the production code calls through weak symbols.
# The runner runs the scenario -DI915_TEST_SCENARIO=<name> (in ZEDBSD_TEST_CPPFLAGS) after the start;
# I915_TEST_ORACLE=y also links the draw readback the pixel oracle reads (slow: it dumps a frame).
# I915_TEST_SET chooses the scenarios the build links.  The test kernel with every scenario does not fit
# AMD64_KERNEL_MAX_BYTES (q762, 2026-10-05: the kernel without tests is within about 450 KiB of it, and the tests add
# 1.4 MiB), so each set links the runner and a part of them (the runner refers to every scenario weakly and says
# "not linked" for one the set leaves out).  plan/ws031/tests/vkloop-hw.sh builds each scenario's own image and
# picks its set when I915_TEST_SET is not given:
#   vkx, vkc, vke1, vke2  that render scenario alone (vke2 with the compiler's boundary steps, ws031-p024,
#                         -DI915_VKE2_BOUNDARY); "boundary" is vke2's name before q762, "render" vkx's
#   execution             ktest eu draw r1 tex t3 bl
#   display               lcdb lcdc lcdd lcdg lcdo lcdr hdmib dual dual_share n1 aux hdmi_edid hdmi_hpd
#   display_ktest         display_ktest's eDP, eDP-sync, modeset and scanout parts
#   display_ktest2        display_ktest's show, lcdg, OpRegion and hotplug parts (the scenario says which it lacks)
#   compute               vkcs (ws101-p006)
I915_TEST_SET ?= vke2
AMD64_I915_TEST_SET_STAMP :=
ifeq ($(I915_TESTS),y)
ifeq ($(I915_TEST_ORACLE),y)
AMD64_I915_SOURCES += src/drivers/gpu/i915/tests/render/readback.c
endif
AMD64_I915_SOURCES += src/drivers/gpu/i915/tests/execution/runner.c src/drivers/gpu/i915/tests/execution/firmware-override.c
ifeq ($(I915_TEST_SET),compute)
AMD64_I915_SOURCES += src/drivers/gpu/i915/tests/render/compute.c
else ifeq ($(I915_TEST_SET),execution)
AMD64_I915_SOURCES += src/drivers/gpu/i915/tests/execution/ktest.c src/drivers/gpu/i915/tests/execution/ktest-sync.c src/drivers/gpu/i915/tests/execution/ktest-display.c src/drivers/gpu/i915/tests/execution/ktest-display-probe.c src/drivers/gpu/i915/tests/execution/ktest-gt.c src/drivers/gpu/i915/tests/execution/eu-test.c src/drivers/gpu/i915/tests/execution/ppgtt-walk.c src/drivers/gpu/i915/tests/execution/draw-test.c src/drivers/gpu/i915/tests/execution/fhd-render.c src/drivers/gpu/i915/tests/fixtures/draw-fixture.c
else ifeq ($(I915_TEST_SET),display)
AMD64_I915_SOURCES += src/drivers/gpu/i915/tests/display/lcd-run.c src/drivers/gpu/i915/tests/display/hdmi-output.c src/drivers/gpu/i915/tests/display/lcd-flip.c src/drivers/gpu/i915/tests/display/lcd-opregion.c src/drivers/gpu/i915/tests/display/lcd-gpu.c src/drivers/gpu/i915/tests/display/hdmi-hotplug.c src/drivers/gpu/i915/tests/display/aux.c src/drivers/gpu/i915/tests/display/hpd-model.c src/drivers/gpu/i915/tests/execution/fhd-render.c src/drivers/gpu/i915/tests/execution/eu-test.c src/drivers/gpu/i915/tests/execution/draw-test.c src/drivers/gpu/i915/tests/fixtures/draw-fixture.c src/drivers/gpu/i915/tests/execution/ppgtt-walk.c
else ifeq ($(I915_TEST_SET),display_ktest)
AMD64_I915_SOURCES += src/drivers/gpu/i915/tests/display/display-ktest.c src/drivers/gpu/i915/tests/display/edp-ktest.c src/drivers/gpu/i915/tests/display/edp-sync-ktest.c src/drivers/gpu/i915/tests/display/lcd-modeset-ktest.c src/drivers/gpu/i915/tests/display/scanout-ktest.c src/drivers/gpu/i915/tests/display/dp-fake-hw.c src/drivers/gpu/i915/tests/display/lcd-fake-hw.c src/drivers/gpu/i915/tests/execution/ktest.c src/drivers/gpu/i915/tests/execution/ppgtt-walk.c
else ifeq ($(I915_TEST_SET),display_ktest2)
AMD64_I915_SOURCES += src/drivers/gpu/i915/tests/display/display-ktest.c src/drivers/gpu/i915/tests/display/lcd-show-ktest.c src/drivers/gpu/i915/tests/display/lcdg-ktest.c src/drivers/gpu/i915/tests/display/opregion-ktest.c src/drivers/gpu/i915/tests/display/hpd-ktest.c src/drivers/gpu/i915/tests/display/hpd-model.c src/drivers/gpu/i915/tests/display/lcd-fake-hw.c src/drivers/gpu/i915/tests/execution/ktest.c src/drivers/gpu/i915/tests/execution/ppgtt-walk.c
else ifneq ($(filter vkx render,$(I915_TEST_SET)),)
AMD64_I915_SOURCES += src/drivers/gpu/i915/tests/render/executor.c
else ifeq ($(I915_TEST_SET),vkc)
AMD64_I915_SOURCES += src/drivers/gpu/i915/tests/render/compiler.c
else ifeq ($(I915_TEST_SET),vke1)
AMD64_I915_SOURCES += src/drivers/gpu/i915/tests/render/features.c
else ifneq ($(filter vke2 boundary,$(I915_TEST_SET)),)
AMD64_I915_SOURCES += src/drivers/gpu/i915/tests/render/generality.c
AMD64_CPPFLAGS += -DI915_VKE2_BOUNDARY=1
else
$(error I915_TEST_SET=$(I915_TEST_SET) is not a test set: vkx, vkc, vke1, vke2, execution, display, display_ktest, display_ktest2 or compute)
endif
# The scenarios linked change with I915_TEST_SET: a content-stable stamp relinks an existing BUILD when it changes.
AMD64_I915_TEST_SET_STAMP := $(BUILD)/.i915-test-set
endif
# The i915 test VBT: I915_TEST_VBT=y builds the captured VBT of the QEMU passthrough test machine
# (vendor/intel-vbt/) into display/vbt.c, used when the guest has no OpRegion.
# XXX: a test crutch; delete it once the GPU tests run on bare metal.
ifeq ($(I915_TEST_VBT),y)
AMD64_CPPFLAGS += -DI915_TEST_VBT=1
endif
# The i915 capture display: I915_TEST_CAPTURE=y leaves the display hardware alone (no modeset, no panel)
# and captures every presentation into guest RAM that the QEMU host reads (display/capture.c).
# Test builds only: the production kernel neither compiles nor links capture.c.
ifeq ($(I915_TEST_CAPTURE),y)
AMD64_CPPFLAGS += -DI915_TEST_CAPTURE=1
AMD64_I915_SOURCES += src/drivers/gpu/i915/display/capture.c
endif
AMD64_ACPI_SOURCES :=
ifeq ($(CONFIG_DRIVER_ACPI),y)
AMD64_ACPI_SOURCES += $(sort $(wildcard src/drivers/acpi/*.c))
# The PC/AT host places the BARs the firmware left unassigned in the _CRS windows (BUG-210).
AMD64_ACPI_SOURCES += src/drivers/pci/pci-pcat-assign.c src/drivers/pci/pci-window.c
# The USB-C connectors' UCSI driver (WS050) runs over the ACPI device USBC000.
ifeq ($(CONFIG_DRIVER_TYPEC),y)
AMD64_ACPI_SOURCES += $(sort $(wildcard src/drivers/typec/*.c))
endif
endif
AMD64_I2C_SOURCES :=
ifeq ($(CONFIG_DRIVER_PCI_LPSS_I2C),y)
AMD64_I2C_SOURCES += src/drivers/i2c/i2c.c src/drivers/i2c/lpss-i2c.c
# HID over I2C finds its devices in the ACPI namespace (WS159).
ifeq ($(CONFIG_DRIVER_ACPI),y)
AMD64_I2C_SOURCES += src/drivers/i2c/i2c-hid.c src/drivers/gpio/intel-gpio.c
endif
endif
AMD64_INTEL_WLAN_SOURCES :=
ifeq ($(CONFIG_DRIVER_PCI_INTEL_AX211),y)
AMD64_INTEL_WLAN_SOURCES += src/drivers/wifi/intel-ax211/intel-ax211.c \
	src/drivers/wifi/intel-ax211/intel-ax211-assoc.c \
	src/drivers/wifi/intel-ax211/intel-ax211-boot.c \
	src/drivers/wifi/intel-ax211/intel-ax211-bss.c \
	src/drivers/wifi/intel-ax211/intel-ax211-command.c \
	src/drivers/wifi/intel-ax211/intel-ax211-dma.c \
	src/drivers/wifi/intel-ax211/intel-ax211-firmware.c \
	src/drivers/wifi/intel-ax211/intel-ax211-init.c \
	src/drivers/wifi/intel-ax211/intel-ax211-mmio.c \
	src/drivers/wifi/intel-ax211/intel-ax211-pci-mmio.c \
	src/drivers/wifi/intel-ax211/intel-ax211-protocol.c \
	src/drivers/wifi/intel-ax211/intel-ax211-runtime.c \
	src/drivers/wifi/intel-ax211/intel-ax211-runtime-start.c \
	src/drivers/wifi/intel-ax211/intel-ax211-scan.c \
	src/drivers/wifi/intel-ax211/intel-ax211-scan-session.c \
	src/drivers/wifi/intel-ax211/intel-ax211-rx.c \
	src/drivers/wifi/intel-ax211/intel-ax211-key.c \
	src/drivers/wifi/intel-ax211/intel-ax211-tx.c \
	src/drivers/wifi/intel-ax211/intel-ax211-tx-ring.c \
	src/drivers/wifi/intel-ax211/intel-ax211-transport-backend.c \
	src/drivers/wifi/intel-ax211/intel-ax211-transport.c \

endif
ifeq ($(CONFIG_DRIVER_USB_CDC_NCM),y)
AMD64_USB_CLASS_SOURCES += src/drivers/usb/usb-cdc-ncm.c \
	src/drivers/usb/usb-cdc-ncm-net.c
endif
ifeq ($(CONFIG_DRIVER_USB_CDC_ECM),y)
AMD64_USB_CLASS_SOURCES += src/drivers/usb/usb-cdc-ecm.c
endif
ifeq ($(CONFIG_DRIVER_USB_HID),y)
AMD64_USB_CLASS_SOURCES += src/drivers/usb/usb-hid.c src/drivers/generic/hidraw.c src/drivers/generic/hidraw-describe.c
endif
# The HID report parser and the pen and touch state machines serve every HID
# transport (USB, I2C-HID) and the test injector (ws159-p003).
AMD64_HID_SOURCES :=
ifneq ($(filter y,$(CONFIG_DRIVER_USB_HID) $(CONFIG_DRIVER_PCI_LPSS_I2C) $(CONFIG_INPUT_TEST_INJECT)),)
AMD64_HID_SOURCES += src/drivers/generic/hid-report.c src/drivers/generic/hid-digitizer.c src/drivers/generic/hid-touch.c
endif
# The USB CCID readers and the smart card slots' class (ws161-p003).
ifeq ($(CONFIG_DRIVER_USB_CCID),y)
AMD64_USB_CLASS_SOURCES += src/drivers/usb/usb-ccid.c src/drivers/usb/usb-ccid-proto.c src/drivers/generic/smartcard.c
endif
# The USB Bluetooth controllers and the HCI class, /dev/btN (ws143-p002).
ifeq ($(CONFIG_DRIVER_USB_BT),y)
AMD64_USB_CLASS_SOURCES += src/drivers/usb/usb-bt.c src/drivers/generic/bt-hci.c src/drivers/generic/bt-hci-proto.c
endif
ifeq ($(CONFIG_DRIVER_USB_HUB),y)
AMD64_USB_CLASS_SOURCES += src/drivers/usb/usb-hub.c
endif
ifeq ($(CONFIG_DRIVER_USB_RTL8822BU),y)
AMD64_USB_CLASS_SOURCES += src/drivers/wifi/rtl8822b/rtl8822b.c \
	src/drivers/wifi/rtl8822b/rtl8822b-security.c \
	src/drivers/usb/usb-rtl8822bu.c
endif
ifeq ($(CONFIG_KERNEL_USB_HID_CHECKPOINT),y)
AMD64_USB_CLASS_SOURCES += src/drivers/usb/usb-hid-checkpoint.c
endif

AMD64_KERNEL_SOURCES := \
	src/kern/main.c \
	$(KERN_FAT_SOURCES) src/kern/inode.c src/kern/file.c \
	src/kern/namecache.c src/kern/namei.c src/kern/mount.c \
	src/kern/tmpfs.c src/kern/tmpfs-pages.c src/drivers/fs/overlayfs.c src/kern/vfs.c \
	src/kern/swap.c src/kern/backing-claim.c \
 src/kern/buf.c src/kern/cache.c src/kern/readahead.c src/kern/writeback.c src/kern/io.c src/kern/sysctl.c \
	src/kern/resource.c src/kern/poll.c src/kern/usync.c \
	src/kern/disk.c src/kern/partition.c \
	src/drivers/generic/loop.c src/drivers/generic/dma.c src/drivers/pci/pci.c \
	src/drivers/pci/pci-pcat.c src/drivers/pci/pci-power.c src/drivers/usb/usb.c $(AMD64_USB_HCD_SOURCES) \
	$(AMD64_USB_CLASS_SOURCES) \
	$(AMD64_HID_SOURCES) \
	$(AMD64_NVME_SOURCES) \
	$(AMD64_VENUS_SOURCES) \
	$(AMD64_HDA_SOURCES) \
	$(AMD64_I915_SOURCES) \
	$(AMD64_INTEL_WLAN_SOURCES) \
	$(AMD64_ACPI_SOURCES) \
	$(AMD64_I2C_SOURCES) \
	src/drivers/platform/pcat/pcat-ide.c src/drivers/ethernet/dp8390.c \
	src/drivers/isa/ne2000.c src/drivers/platform/pcat/ps2-8042.c \
	src/drivers/disklabel/mbr.c src/drivers/disklabel/gpt.c \
	src/drivers/disklabel/pcat.c src/kern/platform/pcat.c \
	src/kern/panic.c src/kern/entry.c src/kern/heap.c src/kern/clock.c \
	src/kern/timer.c src/kern/klog.c src/kern/kcrt.c \
	src/kern/random.c src/kern/random-crypto.c \
	src/kern/device-io.c src/kern/irq.c src/kern/pmem.c \
	src/kern/test-checkpoint.c \
	src/kern/lock.c src/kern/waitq.c \
	src/kern/process.c src/kern/ptrace.c src/kern/thread.c src/kern/sched.c src/kern/sleep.c src/kern/freeze.c \
 src/kern/vmspace.c src/kern/vm-device.c src/kern/vm.c \
	src/kern/filedesc.c src/kern/handle.c src/kern/fd-object.c \
	src/kern/record-lock.c \
	src/kern/pipe.c src/kern/cred.c src/kern/signal.c \
	src/kern/cwdinfo.c src/kern/elf.c src/kern/exec.c src/kern/sandbox.c \
	src/kern/user-probe.c src/kern/syscall.c src/kern/uaccess.c \
	src/kern/cdev.c src/kern/devfs.c src/kern/text-display.c \
	src/drivers/generic/console.c \
	src/drivers/generic/input.c \
	src/drivers/generic/backlight.c \
	$(KERN_GPU_SOURCES) \
	$(KERN_AUDIO_SOURCES) \
	src/kern/tty.c \
	src/drivers/generic/system-device.c src/kern/system-event.c src/drivers/generic/memory-device.c src/kern/shutdown.c \
	src/drivers/platform/pcat/graphics/vgafont.c src/drivers/platform/pcat/graphics/splash.c src/kern/init.c
ifeq ($(CONFIG_INPUT_TEST_INJECT),y)
AMD64_KERNEL_SOURCES += src/drivers/generic/input-inject.c
endif
# The test kernel's loopback security key and card (ws161-p002, p003), on the raw HID and the smart card classes
# (built with usb-hid and usb-ccid, or here without them).
# The test kernel's loopback Bluetooth controller (ws143-p002), on the HCI class (built with usb-bt, or here without it).
ifeq ($(CONFIG_BT_TEST_LOOPBACK),y)
AMD64_KERNEL_SOURCES += src/drivers/generic/bt-hci-loopback.c
ifneq ($(CONFIG_DRIVER_USB_BT),y)
AMD64_KERNEL_SOURCES += src/drivers/generic/bt-hci.c src/drivers/generic/bt-hci-proto.c
endif
endif
ifeq ($(CONFIG_SECURITY_KEY_TEST_LOOPBACK),y)
AMD64_KERNEL_SOURCES += src/drivers/generic/hidraw-loopback.c src/drivers/generic/smartcard-loopback.c
ifneq ($(CONFIG_DRIVER_USB_HID),y)
AMD64_KERNEL_SOURCES += src/drivers/generic/hidraw.c src/drivers/generic/hidraw-describe.c
endif
ifneq ($(CONFIG_DRIVER_USB_CCID),y)
AMD64_KERNEL_SOURCES += src/drivers/generic/smartcard.c
endif
endif
ifeq ($(CONFIG_DRIVER_GRAPHICS_DEVICE),y)
AMD64_KERNEL_SOURCES += \
	src/drivers/platform/pcat/graphics/pcat-graphics.c \
	src/drivers/platform/pcat/graphics/backend.c \
	src/drivers/platform/pcat/graphics/font.c \
	src/drivers/platform/pcat/graphics/text.c \
	src/drivers/platform/pcat/serial-mirror.c
endif
AMD64_KERNEL_SOURCES += $(KERN_NET_SOURCES) $(KERN_BLOCK_IDENTITY_SOURCES) \
	$(KERN_UFS_SOURCES)
AMD64_KERNEL_SOURCES += $(KERN_BOOT_SOURCES)
ifeq ($(CONFIG_KERNEL_TEST_CHECKPOINTS),y)
AMD64_KERNEL_SOURCES += plan/ws004/tests/pci-msi-qemu.c
endif
AMD64_KERNEL_SOURCES += $(KERN_ACL_SOURCES)
AMD64_KERNEL_SOURCES += $(KERN_QUOTA_SOURCES)
AMD64_KERNEL_OBJS := $(patsubst %.c,$(BUILD)/kern64/%.o,\
	$(AMD64_KERNEL_SOURCES))
# The kernel links no C library object: kcrt (src/kern/kcrt.c) and the
# kernel heap (src/kern/heap.c) supply what it used from libc.
AMD64_VMUNIX_OBJS := $(AMD64_HAL_OBJS) $(AMD64_KERNEL_OBJS)
ifneq ($(strip $(ZEDBSD_CONFIG)),)
$(AMD64_VMUNIX_OBJS): $(ZEDBSD_CONFIG)
$(AMD64_VMUNIX_OBJS): platform/amd64/vmunix.mk
endif
$(AMD64_VMUNIX_OBJS): $(ZEDBSD_PLATFORM_CONFIG_STAMP)
$(AMD64_VMUNIX_OBJS): $(ZEDBSD_KERNEL_LTO_STAMP)
# The kernel reads no C library source or header (the sysroot is not a
# prerequisite): no libc source may appear in the link list, and the link
# recipe checks every object's -MMD dependencies (check-kernel-includes.noct).
$(if $(filter libc/% src/libc/%,$(AMD64_KERNEL_SOURCES)),\
	$(error a C library source is in the amd64 kernel link list: \
	$(filter libc/% src/libc/%,$(AMD64_KERNEL_SOURCES))))
$(BUILD)/kern64/src/kern/vfs.o \
	$(BUILD)/kern64/src/kern/platform/pcat.o: \
	$(ZEDBSD_GRAPHICS_CONFIG_STAMP)

vmunix: $(BUILD)/vmunix

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

$(BUILD)/src/hal/amd64/%.o: src/hal/amd64/%.S
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -D_ASM_SRC_ -c $< -o $@

$(BUILD)/src/hal/amd64/%.o: src/hal/amd64/%.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) $(AMD64_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

# Shared x86 HAL sources must use the amd64 flags as well. Without this
# rule the generic i386 pattern can leave a 32-bit object in build/amd64.
$(BUILD)/src/hal/x86/%.o: src/hal/x86/%.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) $(AMD64_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kern64/src/kern/%.o: src/kern/%.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) $(AMD64_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kern64/src/drivers/%.o: src/drivers/%.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) $(AMD64_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kern64/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_KERNEL_LIBC_CFLAGS) -fno-builtin \
 -fno-strict-aliasing $(AMD64_KERNEL_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/vmunix: $(AMD64_VMUNIX_OBJS) $(ZEDBSD_GRAPHICS_CONFIG_STAMP) $(AMD64_I915_TEST_SET_STAMP) \
	$(AMD64_PLATFORM)/vmunix.ld \
	platform/amd64/tools/check-amd64-vmunix.noct \
	platform/amd64/tools/check-kernel-includes.noct
	$(LD) -m elf_x86_64 --gc-sections -z max-page-size=4096 \
 -T $(AMD64_PLATFORM)/vmunix.ld -nostdlib -Map $@.map \
 $(AMD64_VMUNIX_OBJS) -o $@
	$(NOCT) --path=tools/build platform/amd64/tools/check-kernel-includes.noct \
 $(wildcard $(AMD64_VMUNIX_OBJS:.o=.d)) || { rm -f $@; exit 1; }
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

# The GPT hybrid reserves LBA 34 for its chain sector. The separate native
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
	$(LD) -m elf_i386 --image-base=0 -Ttext=0 -e _start $< -o $@

$(BUILD)/bootloader/partition-pbr.bin: $(BUILD)/bootloader/partition-pbr.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c%s $@) -eq 2048

$(BUILD)/bootloader/bootzbsd.o: $(BIOS_LOADER)/bootzbsd.S \
	$(BIOS_LOADER)/vbe.inc \
	bootloader/bios/fat-directory.h bootloader/bios/logo.h bootloader/common/logo-path.h \
	bootloader/include/disk-layout.inc bootloader/include/stage2-header.inc \
	bootloader/include/mbr.inc bootloader/include/fat16.inc \
	bootloader/include/elf.inc bootloader/include/amd64-handoff.h \
	bootloader/include/boot-parameter-handoff.h \
	bootloader/include/boot-parameter-record.inc \
	bootloader/uefi/zedbsd-config.h \
	include/kern/boot.h
	@mkdir -p $(dir $@)
	$(CC) -m32 -I. -x assembler-with-cpp -c $< -o $@

$(BUILD)/bootloader/bios-zedbsd-config.i386.o: \
	bootloader/uefi/zedbsd-config.c bootloader/uefi/zedbsd-config.h \
	bootloader/include/boot-parameter-handoff.h include/kern/boot.h
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

$(BUILD)/bootloader/bios-logo.i386.o: bootloader/bios/logo.c bootloader/bios/logo.h
	@mkdir -p $(dir $@)
	$(CC) -m16 -march=i386 -mtune=i386 -Os -ffreestanding -fno-pic -fno-pie \
 -fno-stack-protector -fno-asynchronous-unwind-tables \
 -fno-unwind-tables -fno-builtin -Wall -Wextra -Werror -I. \
 -c $< -o $@
$(BUILD)/bootloader/common-logo-path.i386.o: bootloader/common/logo-path.c bootloader/common/logo-path.h
	@mkdir -p $(dir $@)
	$(CC) -m16 -march=i386 -mtune=i386 -Os -ffreestanding -fno-pic -fno-pie \
 -fno-stack-protector -fno-asynchronous-unwind-tables \
 -fno-unwind-tables -fno-builtin -Wall -Wextra -Werror -I. \
 -c $< -o $@
AMD64_BOOTZBSD_HELPERS := $(BUILD)/bootloader/bios-zedbsd-config.i386.o \
	$(BUILD)/bootloader/bios-logo.i386.o $(BUILD)/bootloader/common-logo-path.i386.o \
	$(BUILD)/bootloader/bios-fat-directory.i386.o \
	$(BUILD)/bootloader/bios-memory-map.i386.o $(BUILD)/bootloader/common-memory-map.i386.o

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
	$(UEFI_LOADER)/framebuffer.h $(UEFI_LOADER)/video.h $(UEFI_LOADER)/logo.h $(UEFI_LOADER)/memory-map.h \
	$(UEFI_LOADER)/volume-discovery.h \
	$(UEFI_LOADER)/zedbsd-config.h \
	$(UEFI_LOADER)/boot-keys.h bootloader/common/boot-override.h \
	bootloader/include/amd64-handoff.h \
	bootloader/include/amd64-kernel-image.h \
	bootloader/include/boot-parameter-handoff.h \
	include/kern/boot.h
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

$(BUILD)/uefi/logo.o: $(UEFI_LOADER)/logo.c $(UEFI_LOADER)/logo.h \
	bootloader/common/logo-path.h bootloader/include/amd64-handoff.h
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/common-logo-path.o: bootloader/common/logo-path.c bootloader/common/logo-path.h
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -I. -c $< -o $@

$(BUILD)/uefi/common-boot-override.o: bootloader/common/boot-override.c bootloader/common/boot-override.h \
	bootloader/include/boot-parameter-handoff.h include/kern/boot.h
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -I. -c $< -o $@

$(BUILD)/uefi/boot-keys.o: $(UEFI_LOADER)/boot-keys.c $(UEFI_LOADER)/boot-keys.h \
	$(UEFI_LOADER)/include/uefi.h bootloader/common/boot-override.h
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/video.o: $(UEFI_LOADER)/video.c $(UEFI_LOADER)/video.h \
	$(UEFI_LOADER)/include/uefi.h bootloader/include/boot-parameter-handoff.h \
	include/kern/boot.h
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/zedbsd-config.o: $(UEFI_LOADER)/zedbsd-config.c \
	$(UEFI_LOADER)/zedbsd-config.h \
	bootloader/include/boot-parameter-handoff.h \
	include/kern/boot.h
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/elf64.o: $(UEFI_LOADER)/elf64.c $(UEFI_LOADER)/elf64.h \
	bootloader/include/amd64-kernel-image.h
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/memory-map.o: $(UEFI_LOADER)/memory-map.c \
	$(UEFI_LOADER)/memory-map.h $(UEFI_LOADER)/include/uefi.h \
	bootloader/include/amd64-handoff.h
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/src/hal/amd64/locore.o: bootloader/include/amd64-handoff.h \
	bootloader/include/boot-parameter-handoff.h \
	include/kern/boot.h

$(BUILD)/uefi/transition.o: $(UEFI_LOADER)/transition.S bootloader/include/amd64-handoff.h
	@mkdir -p $(dir $@)
	$(EFI_CC) -m64 -mno-red-zone -c $< -o $@

$(BUILD)/uefi/BOOTX64.EFI: $(BUILD)/uefi/bootx64.o \
	$(BUILD)/uefi/elf64.o $(BUILD)/uefi/framebuffer.o $(BUILD)/uefi/video.o $(BUILD)/uefi/logo.o $(BUILD)/uefi/common-logo-path.o \
	$(BUILD)/uefi/common-boot-override.o $(BUILD)/uefi/boot-keys.o \
	$(BUILD)/uefi/memory-map.o $(BUILD)/uefi/memory-map-v6.o $(BUILD)/uefi/common-memory-map.o \
	$(BUILD)/uefi/volume-discovery.o $(BUILD)/uefi/zedbsd-config.o \
	$(BUILD)/uefi/transition.o \
	platform/amd64/tools/check-bootx64.noct
	$(EFI_LD) /subsystem:efi_application /entry:efi_main /base:0 \
 /fixed:no /timestamp:0 /nodefaultlib \
 /out:$@ $(filter %.o,$^)
	@test -z "$$($(EFI_NM) -u $@ | grep -Ev \
 ' (__bss_start__|__bss_end__|__end__|___tls_start__|___tls_end__)$$')" \
 || { $(EFI_NM) -u $@; exit 1; }
	$(NOCT) --path=tools/build platform/amd64/tools/check-bootx64.noct $@

AMD64_USER_CPPFLAGS := -nostdinc \
	-isystem $(ZEDBSD_SYSROOT_AMD64)/usr/include \
	-Iinclude -Isrc -I. -DHAL_ARCH_AMD64 -DKERN_USER_ABI_LP64
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
	userland/base/libc/utmpx.c src/libc/heap.c src/libc/string.c src/libc/ctype.c \
	src/libc/locale.c src/libc/wide.c \
	src/libc/int64.c src/libc/strto.c src/libc/format.c src/libc/stdio.c \
	$(ZEDBSD_LIBC_USER_EXTRA_SOURCES)
AMD64_USER_LIBC_OBJS := $(BUILD)/user64/src/libc/crt/crt0-amd64.o \
	$(patsubst %.c,$(BUILD)/user64/%.o,$(AMD64_USER_RUNTIME_SOURCES))
# User programs consume the canonical target sysroot. The relocatable libc
# bundle preserves the established whole-runtime static link semantics while
# eliminating per-command recompilation of the same sources.
AMD64_USER_LIBC_OBJS := \
	$(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt0.o \
	$(ZEDBSD_SYSROOT_AMD64)/usr/lib/libc.o
AMD64_USER_NET_LIBC_OBJS := $(AMD64_USER_LIBC_OBJS)
AMD64_USER_SH_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(BUILD)/user64,sh)
AMD64_USER_READLINE_OBJ := $(BUILD)/user64/userland/base/libedit/readline.o
AMD64_USER_READLINE_LIB := $(BUILD)/lib/libreadline.a
AMD64_USER_ELF_CHECK := tools/build/check-user-elf.noct

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
	$(BUILD)/user64/userland/tests/syscall-smoke.o $(AMD64_PLATFORM)/user.ld \
	$(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
 -z max-page-size=4096 -z stack-size=0x100000 \
 -T $(AMD64_PLATFORM)/user.ld \
 $(AMD64_USER_LIBC_OBJS) \
 $(BUILD)/user64/userland/tests/syscall-smoke.o -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(BUILD)/POSIX-R2.ELF: $(AMD64_USER_NET_LIBC_OBJS) \
	$(BUILD)/user64/userland/tests/posix-r2.o $(AMD64_PLATFORM)/user.ld \
	$(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
 -z max-page-size=4096 -z stack-size=0x100000 \
 -T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
 $(BUILD)/user64/userland/tests/posix-r2.o -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(BUILD)/POSIX-R2-REMAINING.ELF: $(AMD64_USER_NET_LIBC_OBJS) \
	$(BUILD)/user64/userland/tests/posix-r2-remaining.o \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
 -z max-page-size=4096 -z stack-size=0x100000 \
 -T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
 $(BUILD)/user64/userland/tests/posix-r2-remaining.o -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@

$(BUILD)/SUSV4-XSI.ELF: $(AMD64_USER_NET_LIBC_OBJS) \
	$(BUILD)/user64/userland/tests/susv4-xsi.o \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
 -z max-page-size=4096 -z stack-size=0x100000 \
 -T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
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
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
 -z max-page-size=4096 -z stack-size=0x100000 \
 -T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
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
$(foreach command,$(filter-out vkdemo vkvideo-probe display-events wltest wlshm mview wayland terminal files notes monitor pdfviewer settings imageview textedit videoplayer music photos phone calendar mailer kuidemo browser browser-probe xserver egltest glescompute glxtest zgears gpu-share-test gpu-fence-test acquire-fence-test gpu-forge-test menu-probe titlebar-probe popup-probe subsurface-probe seat-probe data-probe extras-probe tablet-probe keiland-ime ime-probe keiland-settings keiland-system keiland-notify printtest fidoctl passkey-fido2,$(USER_BASIC_COMMANDS)),\
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
	$(AMD64_USER_STATIC_EXTRA_$(1)) $(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	@mkdir -p $$(dir $$@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
 -z max-page-size=4096 -z stack-size=0x100000 \
 -T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) \
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

# The utility library.  It holds what is not part of the C library and not
# wanted by every program, and is built from the same tree so that the two
# cannot drift apart.
DYNAMIC_LIBUTIL_OBJS := $(DYNAMIC_DIR)/obj/src/libc/libutil.o

$(DYNAMIC_DIR)/libutil.so: $(DYNAMIC_LIBUTIL_OBJS) $(DYNAMIC_DIR)/libc.so \
	tools/build/check-dynamic-elf.py
	$(LD) -m elf_x86_64 -shared -soname libutil.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 $(DYNAMIC_LIBUTIL_OBJS) -L$(DYNAMIC_DIR) -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 \
 --role shared-library --needed libc.so --soname libutil.so $@

# Wayland client transport is a normal shared dependency of the Vulkan WSI.
DYNAMIC_WAYLAND_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libwayland-client)

$(DYNAMIC_DIR)/libwayland-client.so: $(DYNAMIC_WAYLAND_OBJS) $(DYNAMIC_DIR)/libc.so \
	userland/desktop/libwayland/exports.map tools/build/check-dynamic-elf.py
	$(LD) -m elf_x86_64 -shared -soname libwayland-client.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libwayland/exports.map \
 $(DYNAMIC_WAYLAND_OBJS) -L$(DYNAMIC_DIR) -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role shared-library \
 --needed libc.so --soname libwayland-client.so $@

# The TrueType reader draws glyphs for whoever puts text on a display; it
# needs nothing but the C library and the mathematics in it.
DYNAMIC_TRUETYPE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libtruetype)

$(DYNAMIC_DIR)/libtruetype.so: $(DYNAMIC_TRUETYPE_OBJS) $(DYNAMIC_DIR)/libc.so \
	userland/desktop/libtruetype/exports.map tools/build/check-dynamic-elf.py
	$(LD) -m elf_x86_64 -shared -soname libtruetype.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libtruetype/exports.map \
 $(DYNAMIC_TRUETYPE_OBJS) -L$(DYNAMIC_DIR) -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role shared-library \
 --needed libc.so --soname libtruetype.so $@

# libz-compat (ws071-p010): the zlib interface of the base programs; it needs nothing but the C library.
DYNAMIC_Z_COMPAT_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libz-compat)

$(DYNAMIC_DIR)/libz-compat.so: $(DYNAMIC_Z_COMPAT_OBJS) $(DYNAMIC_DIR)/libc.so \
	userland/base/libz-compat/exports.map tools/build/check-dynamic-elf.py
	$(LD) -m elf_x86_64 -shared -soname libz-compat.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/base/libz-compat/exports.map \
 $(DYNAMIC_Z_COMPAT_OBJS) -L$(DYNAMIC_DIR) -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role shared-library \
 --needed libc.so --soname libz-compat.so $@

# libpng-compat (ws071-p010): libpng's simplified API of the base programs, over libz-compat.
DYNAMIC_PNG_COMPAT_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libpng-compat)

$(DYNAMIC_DIR)/libpng-compat.so: $(DYNAMIC_PNG_COMPAT_OBJS) $(DYNAMIC_DIR)/libz-compat.so $(DYNAMIC_DIR)/libc.so \
	userland/base/libpng-compat/exports.map tools/build/check-dynamic-elf.py
	$(LD) -m elf_x86_64 -shared -soname libpng-compat.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/base/libpng-compat/exports.map \
 $(DYNAMIC_PNG_COMPAT_OBJS) -L$(DYNAMIC_DIR) -l:libz-compat.so -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role shared-library \
 --needed libz-compat.so --needed libc.so --soname libpng-compat.so $@

# libjpeg-compat (ws074-p019): the libjpeg decompression interface of the base programs; it needs nothing
# but the C library.
DYNAMIC_JPEG_COMPAT_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libjpeg-compat)

$(DYNAMIC_DIR)/libjpeg-compat.so: $(DYNAMIC_JPEG_COMPAT_OBJS) $(DYNAMIC_DIR)/libc.so \
	userland/base/libjpeg-compat/exports.map tools/build/check-dynamic-elf.py
	$(LD) -m elf_x86_64 -shared -soname libjpeg-compat.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/base/libjpeg-compat/exports.map \
 $(DYNAMIC_JPEG_COMPAT_OBJS) -L$(DYNAMIC_DIR) -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role shared-library \
 --needed libc.so --soname libjpeg-compat.so $@

# libpdf (ws079-p004): the PDF library of the base programs; its reader decodes Flate streams through
# libz-compat and JPEG images through libjpeg-compat (ws079-p006), and draws the glyphs of text through
# libtruetype (ws079-p007).
DYNAMIC_PDF_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libpdf)

$(DYNAMIC_DIR)/libpdf.so: $(DYNAMIC_PDF_OBJS) $(DYNAMIC_DIR)/libz-compat.so $(DYNAMIC_DIR)/libjpeg-compat.so \
	$(DYNAMIC_DIR)/libtruetype.so $(DYNAMIC_DIR)/libc.so \
	userland/base/libpdf/exports.map tools/build/check-dynamic-elf.py
	$(LD) -m elf_x86_64 -shared -soname libpdf.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/base/libpdf/exports.map \
 $(DYNAMIC_PDF_OBJS) -L$(DYNAMIC_DIR) -l:libz-compat.so -l:libjpeg-compat.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role shared-library \
 --needed libz-compat.so --needed libjpeg-compat.so --needed libtruetype.so --needed libc.so --soname libpdf.so $@

# libgif-compat (ws074-p051): giflib's decoding interface of the base programs; it needs nothing but the
# C library.
DYNAMIC_GIF_COMPAT_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libgif-compat)

$(DYNAMIC_DIR)/libgif-compat.so: $(DYNAMIC_GIF_COMPAT_OBJS) $(DYNAMIC_DIR)/libc.so \
	userland/base/libgif-compat/exports.map tools/build/check-dynamic-elf.py
	$(LD) -m elf_x86_64 -shared -soname libgif-compat.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/base/libgif-compat/exports.map \
 $(DYNAMIC_GIF_COMPAT_OBJS) -L$(DYNAMIC_DIR) -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role shared-library \
 --needed libc.so --soname libgif-compat.so $@

# The Wayland EGL window (WS068 p002); it needs nothing but the C library.
DYNAMIC_WAYLAND_EGL_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libwayland-egl)

$(DYNAMIC_DIR)/libwayland-egl.so: $(DYNAMIC_WAYLAND_EGL_OBJS) $(DYNAMIC_DIR)/libc.so \
	userland/desktop/libwayland-egl/exports.map tools/build/check-dynamic-elf.py
	$(LD) -m elf_x86_64 -shared -soname libwayland-egl.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libwayland-egl/exports.map \
 $(DYNAMIC_WAYLAND_EGL_OBJS) -L$(DYNAMIC_DIR) -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role shared-library \
 --needed libc.so --soname libwayland-egl.so $@

# EGL over Vulkan (WS068 p002): Wayland and display-direct windows.
DYNAMIC_EGL_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libegl)

$(DYNAMIC_DIR)/libEGL.so: $(DYNAMIC_EGL_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so userland/desktop/libegl/exports.map tools/build/check-dynamic-elf.py
	$(LD) -m elf_x86_64 -shared -soname libEGL.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libegl/exports.map \
 $(DYNAMIC_EGL_OBJS) -L$(DYNAMIC_DIR) -l:libvulkan.so -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role shared-library \
 --needed libvulkan.so --needed libwayland-client.so --needed libc.so --soname libEGL.so $@

# OpenGL ES over EGL and Vulkan (WS068 p002, p008); it finds its context and frame through libEGL.
DYNAMIC_GLESV2_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libglesv2)

$(DYNAMIC_DIR)/libGLESv2.so: $(DYNAMIC_GLESV2_OBJS) $(DYNAMIC_DIR)/libEGL.so $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libc.so \
	userland/desktop/libglesv2/exports.map tools/build/check-dynamic-elf.py
	$(LD) -m elf_x86_64 -shared -soname libGLESv2.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libglesv2/exports.map \
 $(DYNAMIC_GLESV2_OBJS) -L$(DYNAMIC_DIR) -l:libEGL.so -l:libvulkan.so -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role shared-library \
 --needed libEGL.so --needed libvulkan.so --needed libc.so --soname libGLESv2.so $@

# The desktop's way into the system and zdesktop's Wayland extensions (the
# System Menu, WS070): the C library and the Wayland client; the file
# chooser (ws092-p003) draws its text with libtruetype.
DYNAMIC_ZDESKTOP_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libkeiland)

$(DYNAMIC_DIR)/libkeiland.so: $(DYNAMIC_ZDESKTOP_OBJS) $(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libpng-compat.so $(DYNAMIC_DIR)/libz-compat.so \
	$(DYNAMIC_DIR)/libc.so userland/desktop/libkeiland/exports.map tools/build/check-dynamic-elf.py
	$(PYTHON) userland/desktop/libkeiland/exports.py --check
	$(LD) -m elf_x86_64 -shared -soname libkeiland.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libkeiland/exports.map \
 $(DYNAMIC_ZDESKTOP_OBJS) -L$(DYNAMIC_DIR) -l:libwayland-client.so -l:libtruetype.so -l:libvulkan.so \
 -l:libpng-compat.so -l:libz-compat.so -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role shared-library \
 --needed libwayland-client.so --needed libtruetype.so --needed libvulkan.so --needed libpng-compat.so \
 --needed libz-compat.so --needed libc.so --soname libkeiland.so $@

# Vulkan is an ordinary shared dependency of the portable base application.
DYNAMIC_VULKAN_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libvulkan)
DYNAMIC_VKDEMO_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,vkdemo)
DYNAMIC_VULKAN_CHECK := tools/build/check-dynamic-elf.py

$(DYNAMIC_DIR)/libvulkan.so: $(DYNAMIC_VULKAN_OBJS) $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/libwayland-client.so \
	userland/desktop/libvulkan/exports.map userland/desktop/libvulkan/api-commands.tsv \
	$(DYNAMIC_VULKAN_CHECK)
	$(LD) -m elf_x86_64 -shared -soname libvulkan.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libvulkan/exports.map \
 $(DYNAMIC_VULKAN_OBJS) -L$(DYNAMIC_DIR) -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role shared-library \
 --needed libwayland-client.so --needed libc.so --soname libvulkan.so \
 --exports-tsv userland/desktop/libvulkan/api-commands.tsv $@

$(BUILD)/bin/vkdemo: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_VKDEMO_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_VKDEMO_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libc.so $@

# The Vulkan Video probe (ws083) is a plain Vulkan client like vkdemo.
DYNAMIC_VKVIDEO_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,vkvideo-probe)

$(BUILD)/bin/vkvideo-probe: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_VKVIDEO_PROBE_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_VKVIDEO_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libc.so $@

# The Vulkan display events probe (ws113-p003) is a plain Vulkan client like vkdemo.
DYNAMIC_DISPLAY_EVENTS_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,display-events)

$(BUILD)/bin/display-events: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_DISPLAY_EVENTS_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_DISPLAY_EVENTS_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libc.so $@

# The compositor draws window mode with standard Vulkan (WS035 p052), and
# reaches the system (the network, ws035-p013) through libkeiland.
DYNAMIC_ZDESKTOP_PROGRAM_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,wayland)

$(BUILD)/bin/wayland: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_ZDESKTOP_PROGRAM_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libpng-compat.so $(DYNAMIC_DIR)/libjpeg-compat.so $(DYNAMIC_DIR)/libz-compat.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_ZDESKTOP_PROGRAM_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libtruetype.so -l:libkeiland.so -l:libpng-compat.so -l:libjpeg-compat.so -l:libz-compat.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libtruetype.so --needed libkeiland.so --needed libpng-compat.so --needed libjpeg-compat.so \
 --needed libz-compat.so --needed libc.so $@

# The test application imports only standard Wayland and Vulkan entry points.
DYNAMIC_WLTEST_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,wltest)

$(BUILD)/bin/wltest: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_WLTEST_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_WLTEST_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libc.so $@

# The acquire-fence test is wltest's window and renderer with a fence of its own.
DYNAMIC_ACQUIRE_FENCE_TEST_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,acquire-fence-test)

$(BUILD)/bin/acquire-fence-test: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_ACQUIRE_FENCE_TEST_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_ACQUIRE_FENCE_TEST_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libc.so $@

# The GPU buffer forgery test (ws103-p004): Vulkan, Wayland and the C library.
DYNAMIC_GPU_FORGE_TEST_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,gpu-forge-test)

$(BUILD)/bin/gpu-forge-test: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_GPU_FORGE_TEST_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_GPU_FORGE_TEST_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libc.so $@

# The System Menu probe (WS070 p002): Wayland, libkeiland and the C library.
DYNAMIC_MENU_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,menu-probe)

$(BUILD)/bin/menu-probe: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_MENU_PROBE_OBJS) $(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libkeiland.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_MENU_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libkeiland.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libwayland-client.so --needed libkeiland.so --needed libc.so $@

# The settings probe (ws135-p003): Wayland, libkeiland and the C library.
DYNAMIC_KEILAND_SETTINGS_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,keiland-settings)

$(BUILD)/bin/keiland-settings: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_KEILAND_SETTINGS_OBJS) $(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libkeiland.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_KEILAND_SETTINGS_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libkeiland.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libwayland-client.so --needed libkeiland.so --needed libc.so $@

# The system probe (ws131-p010): Wayland, libkeiland and the C library.
DYNAMIC_KEILAND_SYSTEM_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,keiland-system)

$(BUILD)/bin/keiland-system: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_KEILAND_SYSTEM_OBJS) $(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libkeiland.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_KEILAND_SYSTEM_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libkeiland.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libwayland-client.so --needed libkeiland.so --needed libc.so $@

# The print test client (ws145-p003): Wayland, libkeiland and the C library.
DYNAMIC_PRINTTEST_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,printtest)

$(BUILD)/bin/printtest: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_PRINTTEST_OBJS) $(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libkeiland.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_PRINTTEST_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libkeiland.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libwayland-client.so --needed libkeiland.so --needed libc.so $@

# The notification client (ws156-p002): Wayland, libkeiland and the C library.
DYNAMIC_KEILAND_NOTIFY_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,keiland-notify)

$(BUILD)/bin/keiland-notify: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_KEILAND_NOTIFY_OBJS) $(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libkeiland.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_KEILAND_NOTIFY_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libkeiland.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libwayland-client.so --needed libkeiland.so --needed libc.so $@

# The security key tool (ws161-p004): libpasskey's sources, OpenSSL's libcrypto (the package's staged headers and
# library; the Guardrail's exception until the release, WS172 p005) and the C library.
DYNAMIC_FIDOCTL_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,fidoctl)
DYNAMIC_FIDOCTL_SSL := $(ZEDBSD_EXT_openssl_STAGEDIR)/usr

$(DYNAMIC_FIDOCTL_OBJS): DYNAMIC_CPPFLAGS += -I$(DYNAMIC_FIDOCTL_SSL)/include
$(DYNAMIC_FIDOCTL_OBJS): $(ZEDBSD_OPENSSL_STAGED)

$(BUILD)/bin/fidoctl: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_FIDOCTL_OBJS) $(ZEDBSD_OPENSSL_LIBCRYPTO) \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_FIDOCTL_OBJS) \
 -L$(DYNAMIC_DIR) -L$(DYNAMIC_FIDOCTL_SSL)/lib -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libcrypto.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libcrypto.so --needed libc.so $@

# passkey's security key style (ws172-p003): libpasskey's sources, OpenSSL's libcrypto (as fidoctl's) and the C
# library (crypt() for the password of a registration).
DYNAMIC_PASSKEY_FIDO2_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,passkey-fido2)

$(DYNAMIC_PASSKEY_FIDO2_OBJS): DYNAMIC_CPPFLAGS += -I$(DYNAMIC_FIDOCTL_SSL)/include
$(DYNAMIC_PASSKEY_FIDO2_OBJS): $(ZEDBSD_OPENSSL_STAGED)

$(BUILD)/bin/passkey-fido2: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_PASSKEY_FIDO2_OBJS) $(ZEDBSD_OPENSSL_LIBCRYPTO) \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_PASSKEY_FIDO2_OBJS) \
 -L$(DYNAMIC_DIR) -L$(DYNAMIC_FIDOCTL_SSL)/lib -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libcrypto.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libcrypto.so --needed libc.so $@

# The Titlebar Presentation probe (WS070 p008): Wayland, libkeiland and the C library.
DYNAMIC_TITLEBAR_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,titlebar-probe)

$(BUILD)/bin/titlebar-probe: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_TITLEBAR_PROBE_OBJS) $(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libkeiland.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_TITLEBAR_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libkeiland.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libwayland-client.so --needed libkeiland.so --needed libc.so $@

# The popup probe (WS035 p076): standard Wayland and the C library.
DYNAMIC_POPUP_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,popup-probe)

$(BUILD)/bin/popup-probe: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_POPUP_PROBE_OBJS) $(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libkeiland.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_POPUP_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libkeiland.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libwayland-client.so --needed libkeiland.so --needed libc.so $@

# The sub-surface probe (WS035 p077): standard Wayland and the C library.
DYNAMIC_SUBSURFACE_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,subsurface-probe)

$(BUILD)/bin/subsurface-probe: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_SUBSURFACE_PROBE_OBJS) $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_SUBSURFACE_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libwayland-client.so --needed libc.so $@

# The keyboard and output probe (WS035 p078): standard Wayland and the C library.
DYNAMIC_SEAT_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,seat-probe)

$(BUILD)/bin/seat-probe: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_SEAT_PROBE_OBJS) $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_SEAT_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libwayland-client.so --needed libc.so $@

# The pen probe (WS079 p003): standard Wayland (the tablet protocol) and the C library.
DYNAMIC_TABLET_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,tablet-probe)

$(BUILD)/bin/tablet-probe: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_TABLET_PROBE_OBJS) $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_TABLET_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libwayland-client.so --needed libc.so $@

# The clipboard probe (WS035 p079): standard Wayland and the C library.
DYNAMIC_DATA_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,data-probe)

$(BUILD)/bin/data-probe: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_DATA_PROBE_OBJS) $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_DATA_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libwayland-client.so --needed libc.so $@

# The decoration, cursor-shape and viewporter probe (WS035 p080): standard Wayland and the C library.
DYNAMIC_EXTRAS_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,extras-probe)

$(BUILD)/bin/extras-probe: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_EXTRAS_PROBE_OBJS) $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_EXTRAS_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libwayland-client.so --needed libc.so $@

# The wl_shm test client imports only standard Wayland and C library entry points.
DYNAMIC_WLSHM_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,wlshm)

$(BUILD)/bin/wlshm: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_WLSHM_OBJS) $(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libkeiland.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_WLSHM_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libkeiland.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libwayland-client.so --needed libkeiland.so --needed libc.so $@

# The input method (ws095-p004) imports standard Wayland and C library entry points; its candidate window
# (ws095-p005) draws with libkeiland's canvas and text (libkeiland/ui, and so with what libkeiland links).  Its test client imports
# only standard Wayland and C library entry points.
DYNAMIC_KEILAND_IME_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,keiland-ime)

$(BUILD)/bin/keiland-ime: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_KEILAND_IME_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libpng-compat.so $(DYNAMIC_DIR)/libz-compat.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_KEILAND_IME_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libpng-compat.so -l:libz-compat.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libpng-compat.so --needed libz-compat.so --needed libc.so $@

DYNAMIC_IME_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,ime-probe)

$(BUILD)/bin/ime-probe: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_IME_PROBE_OBJS) $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_IME_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libwayland-client.so --needed libc.so $@

# The model viewer imports only standard Wayland, Vulkan and C library entry points.
DYNAMIC_MVIEW_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,mview)

$(BUILD)/bin/mview: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_MVIEW_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_MVIEW_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libc.so $@

# The terminal imports standard Wayland, Vulkan, TrueType and C library entry
# points (WS035 p068), and zdesktop's System Menu through libkeiland (WS070).
DYNAMIC_ZDESKTOP_TERMINAL_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,terminal)

$(BUILD)/bin/terminal: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_ZDESKTOP_TERMINAL_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_ZDESKTOP_TERMINAL_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so --needed libc.so $@

# The file manager (WS071) imports standard Wayland, Vulkan, TrueType and C
# library entry points, and zdesktop's own extensions through libkeiland.
# ws168-p004: no decoder (keiland-preview makes its pictures in a sandbox).
DYNAMIC_ZDESKTOP_FILES_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,files)

$(BUILD)/bin/files: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_ZDESKTOP_FILES_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_ZDESKTOP_FILES_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so --needed libc.so $@

# Settings (WS089) imports standard Wayland, Vulkan, TrueType and C library entry points, and
# zdesktop's own extensions through libkeiland.  It compiles the file manager's canvas, text and
# icons (the same objects as files: the pattern rule builds them once).
DYNAMIC_ZDESKTOP_SETTINGS_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,settings)

$(BUILD)/bin/settings: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_ZDESKTOP_SETTINGS_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_ZDESKTOP_SETTINGS_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so --needed libc.so $@

# The System Monitor (ws134-p002) imports standard Wayland, Vulkan, TrueType and C library entry points,
# and the titlebar, and the window and text (libkeiland/ui), through libkeiland.
DYNAMIC_ZDESKTOP_MONITOR_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,monitor)

$(BUILD)/bin/monitor: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_ZDESKTOP_MONITOR_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_ZDESKTOP_MONITOR_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libc.so $@

# Notes (ws079-p005) imports standard Wayland, Vulkan, TrueType and C library entry points,
# zdesktop's System Menu and the recent files through libkeiland, and libpdf for its PDF; ws175-p007: libz-compat
# for the images it keeps compressed; ws175-p008: libpng-compat, libjpeg-compat and libgif-compat for the image files
# it puts on a page (picture.c).
DYNAMIC_ZDESKTOP_NOTES_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,notes)

$(BUILD)/bin/notes: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_ZDESKTOP_NOTES_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so $(DYNAMIC_DIR)/libpng-compat.so $(DYNAMIC_DIR)/libjpeg-compat.so \
	$(DYNAMIC_DIR)/libgif-compat.so $(DYNAMIC_DIR)/libz-compat.so $(DYNAMIC_DIR)/libpdf.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_ZDESKTOP_NOTES_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libpng-compat.so -l:libjpeg-compat.so \
 -l:libgif-compat.so -l:libz-compat.so -l:libpdf.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libpng-compat.so --needed libjpeg-compat.so --needed libgif-compat.so --needed libz-compat.so --needed libpdf.so \
 --needed libc.so $@

# PDF Viewer (ws079-p006) imports standard Wayland, Vulkan, TrueType and C library entry points,
# zdesktop's menus and titlebar through libkeiland, and libpdf (which brings libz-compat and
# libjpeg-compat).
DYNAMIC_PDFVIEWER_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,pdfviewer)

$(BUILD)/bin/pdfviewer: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_PDFVIEWER_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so $(DYNAMIC_DIR)/libpdf.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_PDFVIEWER_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libpdf.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libpdf.so --needed libc.so $@

# Image Viewer (ws091) imports standard Wayland, Vulkan, TrueType and C library entry points,
# zdesktop's menus, titlebar and glass through libkeiland, and libpng-compat (with libz-compat),
# libjpeg-compat and libgif-compat for its images.
DYNAMIC_IMAGEVIEW_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,imageview)

$(BUILD)/bin/imageview: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_IMAGEVIEW_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so $(DYNAMIC_DIR)/libpng-compat.so \
	$(DYNAMIC_DIR)/libz-compat.so $(DYNAMIC_DIR)/libjpeg-compat.so $(DYNAMIC_DIR)/libgif-compat.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_IMAGEVIEW_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libpng-compat.so \
 -l:libz-compat.so -l:libjpeg-compat.so -l:libgif-compat.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libpng-compat.so --needed libz-compat.so --needed libjpeg-compat.so --needed libgif-compat.so \
 --needed libc.so $@

# Video Player (WS122) imports standard Wayland, Vulkan, TrueType and C library entry points, and the window and
# the widgets through libkeiland.  It is not linked to FFmpeg: its decoding add-in (WS122 p004) opens libavcodec,
# libavutil and libswscale with dlopen when the system has them (the libavcodec package), without FFmpeg's headers.
DYNAMIC_VIDEOPLAYER_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,videoplayer)

$(BUILD)/bin/videoplayer: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_VIDEOPLAYER_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_VIDEOPLAYER_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libc.so $@

# Music (WS120) imports standard Wayland, Vulkan, TrueType and C library entry points, the window and the widgets
# through libkeiland, and the covers' decoding through libjpeg-compat and libpng-compat (picture.c's GIF part needs
# libgif-compat).  Like Video Player it is not linked to FFmpeg: the decoding add-in opens libavcodec with dlopen.
DYNAMIC_MUSIC_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,music)

$(BUILD)/bin/music: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_MUSIC_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so $(DYNAMIC_DIR)/libpng-compat.so $(DYNAMIC_DIR)/libz-compat.so \
	$(DYNAMIC_DIR)/libjpeg-compat.so $(DYNAMIC_DIR)/libgif-compat.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_MUSIC_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libpng-compat.so -l:libz-compat.so \
 -l:libjpeg-compat.so -l:libgif-compat.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libpng-compat.so --needed libz-compat.so --needed libjpeg-compat.so --needed libgif-compat.so --needed libc.so $@

# Photos (WS157) imports standard Wayland, Vulkan, TrueType and C library entry points, the window and the widgets
# through libkeiland, and the pictures' decoding through libjpeg-compat, libpng-compat and libgif-compat.
DYNAMIC_PHOTOS_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,photos)

$(BUILD)/bin/photos: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_PHOTOS_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so $(DYNAMIC_DIR)/libpng-compat.so $(DYNAMIC_DIR)/libz-compat.so \
	$(DYNAMIC_DIR)/libjpeg-compat.so $(DYNAMIC_DIR)/libgif-compat.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_PHOTOS_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libpng-compat.so -l:libz-compat.so \
 -l:libjpeg-compat.so -l:libgif-compat.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libpng-compat.so --needed libz-compat.so --needed libjpeg-compat.so --needed libgif-compat.so --needed libc.so $@

# Phone (WS170 p000, the mock of the messages and calls application) imports standard Wayland, Vulkan,
# TrueType and C library entry points, and the window, the widgets and the drawing through libkeiland.
DYNAMIC_PHONE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,phone)

$(BUILD)/bin/phone: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_PHONE_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_PHONE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libc.so $@

# Calendar (WS155 p000, the mock of the calendar) imports standard Wayland, Vulkan, TrueType, the C library's and
# its mathematics' entry points, and the window, the widgets and the drawing through libkeiland.
DYNAMIC_CALENDAR_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,calendar)

$(BUILD)/bin/calendar: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_CALENDAR_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_CALENDAR_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libc.so $@

# Mail (WS169 p000, the mock of the mail application) imports standard Wayland, Vulkan, TrueType and C library entry
# points, and the window, the widgets and the drawing through libkeiland.
DYNAMIC_MAILER_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,mailer)

$(BUILD)/bin/mailer: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_MAILER_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_MAILER_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libc.so $@

# Text Editor (WS092) imports standard Wayland, Vulkan, TrueType and C library entry points, and
# zdesktop's menus, titlebar, glass and recent files through libkeiland.
DYNAMIC_TEXTEDIT_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,textedit)

$(BUILD)/bin/textedit: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_TEXTEDIT_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libpng-compat.so $(DYNAMIC_DIR)/libz-compat.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_TEXTEDIT_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libpng-compat.so -l:libz-compat.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libpng-compat.so --needed libz-compat.so --needed libc.so $@

# The widgets' sampler (WS090 ws090-p005, the test image only) imports standard Wayland, Vulkan, TrueType
# and C library entry points, and the widgets, the window and zdesktop's glass through libkeiland.
DYNAMIC_KUIDEMO_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,kuidemo)

$(BUILD)/bin/kuidemo: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_KUIDEMO_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_KUIDEMO_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so \
 --needed libc.so $@

# libmedia (ws121-p002): the playing of media files (the container reader, the decoding add-in that opens libavcodec
# with dlopen, the audiod client and the engine); it needs nothing but the C library.
DYNAMIC_MEDIA_LIBRARY_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libmedia)

$(DYNAMIC_DIR)/libmedia.so: $(DYNAMIC_MEDIA_LIBRARY_OBJS) $(DYNAMIC_DIR)/libc.so \
	userland/desktop/libmedia/exports.map tools/build/check-dynamic-elf.py
	$(LD) -m elf_x86_64 -shared -soname libmedia.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libmedia/exports.map \
 $(DYNAMIC_MEDIA_LIBRARY_OBJS) -L$(DYNAMIC_DIR) -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role shared-library \
 --needed libc.so --soname libmedia.so $@

# The Web browser engine (WS074, libbrowser since ws074-p057) keeps its modules in subdirectories of
# userland/desktop/libbrowser and includes their private headers from that root; it imports standard Vulkan for its
# GPU renderer (ws074-p014), libtruetype for its text, libjpeg-compat, libpng-compat (with
# libz-compat) and libgif-compat for its images (ws074-p021), and libmedia for <video> and <audio> (ws121-p004).  Only the calls of <browser.h> leave it.
DYNAMIC_BROWSER_LIBRARY_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libbrowser)
$(DYNAMIC_BROWSER_LIBRARY_OBJS): DYNAMIC_CPPFLAGS += -Iuserland/desktop/libbrowser

$(DYNAMIC_DIR)/libbrowser.so: $(DYNAMIC_BROWSER_LIBRARY_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libtruetype.so \
	$(DYNAMIC_DIR)/libmedia.so \
	$(DYNAMIC_DIR)/libjpeg-compat.so $(DYNAMIC_DIR)/libpng-compat.so $(DYNAMIC_DIR)/libz-compat.so \
	$(DYNAMIC_DIR)/libgif-compat.so $(DYNAMIC_DIR)/libc.so \
	userland/desktop/libbrowser/exports.map tools/build/check-dynamic-elf.py
	$(LD) -m elf_x86_64 -shared -soname libbrowser.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libbrowser/exports.map \
 $(DYNAMIC_BROWSER_LIBRARY_OBJS) -L$(DYNAMIC_DIR) -l:libvulkan.so -l:libtruetype.so -l:libmedia.so \
 -l:libjpeg-compat.so -l:libpng-compat.so -l:libz-compat.so -l:libgif-compat.so -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role shared-library \
 --needed libvulkan.so --needed libtruetype.so --needed libmedia.so --needed libjpeg-compat.so --needed libpng-compat.so \
 --needed libz-compat.so --needed libgif-compat.so --needed libc.so --soname libbrowser.so $@

# /bin/browser is the shell over libbrowser (ws074-p057): its command line and headless modes (main.c)
# and its window (shell/), which import the engine through <browser.h>, standard Wayland and Vulkan
# for the window and its swapchain (ws074-p014), and zdesktop's titlebar through libkeiland (ws074-p045).
DYNAMIC_ZDESKTOP_BROWSER_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,browser)
$(DYNAMIC_ZDESKTOP_BROWSER_OBJS): DYNAMIC_CPPFLAGS += -Iuserland/desktop/browser

$(BUILD)/bin/browser: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_ZDESKTOP_BROWSER_OBJS) $(DYNAMIC_DIR)/libbrowser.so $(DYNAMIC_DIR)/libvulkan.so \
	$(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so \
	$(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_ZDESKTOP_BROWSER_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libbrowser.so -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libbrowser.so --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so \
 --needed libc.so $@

# The second program over libbrowser (ws074-p057): <browser.h> and the C library, no window.
DYNAMIC_BROWSER_PROBE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,browser-probe)

$(BUILD)/bin/browser-probe: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_BROWSER_PROBE_OBJS) $(DYNAMIC_DIR)/libbrowser.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so \
	$(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_BROWSER_PROBE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libbrowser.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libbrowser.so --needed libc.so $@

# OpenGL (WS069 p004; WS178): libGLESv2's translation with the fixed function and OpenGL 3.x, GL only.
DYNAMIC_GL_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,libgl)

$(DYNAMIC_DIR)/libGL.so: $(DYNAMIC_GL_OBJS) $(DYNAMIC_DIR)/libEGL.so $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libc.so \
	userland/desktop/libGL/exports.map tools/build/check-dynamic-elf.py
	$(LD) -m elf_x86_64 -shared -soname libGL.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/libGL/exports.map \
 $(DYNAMIC_GL_OBJS) -L$(DYNAMIC_DIR) -l:libEGL.so -l:libvulkan.so -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role shared-library \
 --needed libEGL.so --needed libvulkan.so --needed libc.so --soname libGL.so $@

# GLX (WS069 p004; WS178): xserver's library, contexts of libGL's GL for X windows, a private libX11 inside.
DYNAMIC_GLX_OBJS := $(patsubst %.c,$(DYNAMIC_DIR)/obj/%.o,$(KEILAND_LIBGLX_SOURCES))

$(DYNAMIC_DIR)/libGLX.so: $(DYNAMIC_GLX_OBJS) $(DYNAMIC_DIR)/libGL.so $(DYNAMIC_DIR)/libEGL.so $(DYNAMIC_DIR)/libc.so \
	userland/desktop/xserver/libGLX/exports.map tools/build/check-dynamic-elf.py
	$(LD) -m elf_x86_64 -shared -soname libGLX.so --hash-style=both -Bsymbolic-functions \
 -z defs -z now -z relro -z separate-code -z stack-size=0x100000 \
 --version-script=userland/desktop/xserver/libGLX/exports.map \
 $(DYNAMIC_GLX_OBJS) -L$(DYNAMIC_DIR) -l:libGL.so -l:libEGL.so -l:libc.so -o $@
	$(PYTHON) tools/build/check-dynamic-elf.py --machine amd64 --role shared-library \
 --needed libGL.so --needed libEGL.so --needed libc.so --soname libGLX.so $@

# The GLX test application (WS069 p004): Xlib built in, GL from libGL and GLX from libGLX.
DYNAMIC_GLXTEST_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,glxtest)

$(BUILD)/bin/glxtest: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_GLXTEST_OBJS) $(DYNAMIC_DIR)/libGL.so $(DYNAMIC_DIR)/libGLX.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_GLXTEST_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libGL.so -l:libGLX.so -l:libc.so -o $@

# Gears (WS069 p005): OpenGL 1.x's fixed function through GLX, Xlib built in; GL from libGL, GLX from libGLX.
DYNAMIC_ZGEARS_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,zgears)

$(BUILD)/bin/zgears: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_ZGEARS_OBJS) $(DYNAMIC_DIR)/libGL.so $(DYNAMIC_DIR)/libGLX.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_ZGEARS_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libGL.so -l:libGLX.so -l:libc.so -o $@

# The EGL test application (WS068 p002) imports standard Wayland, EGL and GLES entry points.
DYNAMIC_EGLTEST_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,egltest)

$(BUILD)/bin/egltest: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_EGLTEST_OBJS) $(DYNAMIC_DIR)/libEGL.so $(DYNAMIC_DIR)/libGLESv2.so $(DYNAMIC_DIR)/libwayland-egl.so \
	$(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_EGLTEST_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libEGL.so -l:libGLESv2.so -l:libwayland-egl.so -l:libwayland-client.so -l:libc.so -o $@

# The OpenGL ES 3.1 compute test (ws101-p009) imports standard EGL and GLES entry points (a pbuffer context, no window).
DYNAMIC_GLESCOMPUTE_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,glescompute)

$(BUILD)/bin/glescompute: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_GLESCOMPUTE_OBJS) $(DYNAMIC_DIR)/libEGL.so $(DYNAMIC_DIR)/libGLESv2.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_GLESCOMPUTE_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libEGL.so -l:libGLESv2.so -l:libc.so -o $@

# zdesktop's X11 server imports standard Wayland, Vulkan, TrueType and C library entry points (WS069 p008, p011).
DYNAMIC_ZDESKTOP_X11SERVER_OBJS := $(call ZEDBSD_USERLAND_OBJECTS,$(DYNAMIC_DIR)/obj,xserver)

$(BUILD)/bin/xserver: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_ZDESKTOP_X11SERVER_OBJS) $(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libwayland-client.so \
	$(DYNAMIC_DIR)/libkeiland.so $(DYNAMIC_DIR)/libtruetype.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so $(DYNAMIC_VULKAN_CHECK)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o $(DYNAMIC_ZDESKTOP_X11SERVER_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libwayland-client.so -l:libkeiland.so -l:libtruetype.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libwayland-client.so --needed libkeiland.so --needed libtruetype.so --needed libc.so $@

# The external-fence test uses only the installed standard Vulkan shared library.
$(BUILD)/bin/gpu-fence-test: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_DIR)/obj/userland/tests/gpu-fence/main.o \
	$(DYNAMIC_DIR)/libvulkan.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
 $(DYNAMIC_DIR)/obj/userland/tests/gpu-fence/main.o \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libvulkan.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libvulkan.so --needed libc.so $@

# This finite fixture links private Vulkan helpers without exporting them from the public DSO.
$(DYNAMIC_DIR)/obj/userland/tests/gpu-share/main.o: DYNAMIC_CPPFLAGS += -Iuserland/desktop/libvulkan

$(BUILD)/bin/gpu-share-test: $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
	$(DYNAMIC_DIR)/obj/userland/tests/gpu-share/main.o $(DYNAMIC_VULKAN_OBJS) \
	$(DYNAMIC_DIR)/libwayland-client.so $(DYNAMIC_DIR)/libc.so $(DYNAMIC_DIR)/ld.so
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -pie -Wl,--no-relax \
 -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
 -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
 -Wl,--dynamic-linker=/lib/ld.so \
 $(ZEDBSD_SYSROOT_AMD64)/usr/lib/crt1.o \
 $(DYNAMIC_DIR)/obj/userland/tests/gpu-share/main.o $(DYNAMIC_VULKAN_OBJS) \
 -L$(DYNAMIC_DIR) -Wl,-rpath-link,$(DYNAMIC_DIR) \
 -l:libwayland-client.so -l:libc.so -o $@
	$(PYTHON) $(DYNAMIC_VULKAN_CHECK) --machine amd64 --role application \
 --needed libwayland-client.so --needed libc.so $@

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
AMD64_ARCH_INPUTS := $(BUILD)/bin/sh \
	$(BUILD)/bin/sysctl \
	$(BUILD)/bin/mount $(BUILD)/bin/umount \
	$(DYNAMIC_DIR)/ld.so $(DYNAMIC_DIR)/libc.so \
	$(DYNAMIC_DIR)/libutil.so \
	$(DYNAMIC_DIR)/tlstest.so $(DYNAMIC_DIR)/dyntest \
	$(DYNAMIC_DIR)/alt/rpathdep.so $(DYNAMIC_DIR)/rpathtest.so \
	$(DYNAMIC_DIR)/verstest.so $(DYNAMIC_DIR)/versuse.so
AMD64_ARCH_FILES := --file /bin/sh=$(BUILD)/bin/sh \
	--file /sbin/sysctl=$(BUILD)/bin/sysctl \
	--file /sbin/mount=$(BUILD)/bin/mount \
	--file /sbin/umount=$(BUILD)/bin/umount \
	--file /lib/ld.so=$(DYNAMIC_DIR)/ld.so \
	--file /lib/libc.so=$(DYNAMIC_DIR)/libc.so \
	--file /lib/libutil.so=$(DYNAMIC_DIR)/libutil.so \
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
AMD64_ARCH_INPUTS += $(addprefix $(BUILD)/bin/,$(USER_STATIC_COMMANDS))
AMD64_ARCH_FILES += $(foreach command,$(USER_STATIC_COMMANDS),--file $(call zedbsd_userland_destination,$(command))=$(BUILD)/bin/$(command))
AMD64_ARCH_FILES += $(ZEDBSD_USERLAND_FILE_MODES)
AMD64_ARCH_INPUTS += $(ZEDBSD_ACCOUNT_INPUTS)
AMD64_ARCH_FILES += $(ZEDBSD_ACCOUNT_FILES)
AMD64_ARCH_INPUTS += $(ZEDBSD_BASE_DATA_INPUTS)
AMD64_ARCH_FILES += $(ZEDBSD_BASE_DATA_FILES)
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

# ws035-p096: the boot logo on the boot FAT (/logo.ppm: the ESP of the native layout, the payload FAT of the BIOS image), drawn by the
# UEFI and the BIOS loaders when zedbsd.cfg names it (logo=logo.ppm).  ws035-p107: it is the Kei boot splash
# (userland/desktop/artwork/kei-boot-splash.png, 1920x1080 without its spinner, "fit=contain" since ws035-p112: the loaders draw it in the
# middle over black bars, shrunk only when the screen is smaller, and the
# kernel's quiet console draws the spinner, src/drivers/platform/pcat/graphics/splash.c).
AMD64_BOOT_LOGO := $(BUILD)/boot-logo.ppm

$(AMD64_BOOT_LOGO): tools/build/make-boot-splash.py userland/desktop/artwork/kei-boot-splash.png
	@mkdir -p $(dir $@)
	$(PYTHON) tools/build/make-boot-splash.py userland/desktop/artwork/kei-boot-splash.png $@.tmp
	mv -f $@.tmp $@

# The BIOS image's zedbsd.cfg gets the graphical boot's lines too when ZEDBSD_GRAPHICAL_BOOT is y
# (ws035-p099; the BIOS loader draws the logo on its VBE framebuffer).
AMD64_BIOS_ZEDBSD_CONFIG := $(BUILD)/zedbsd-bios-graphical-$(ZEDBSD_GRAPHICAL_BOOT)-kmsg-$(ZEDBSD_BOOT_KERNEL_MESSAGES).cfg

$(AMD64_BIOS_ZEDBSD_CONFIG): $(AMD64_ZEDBSD_CONFIG)
	@mkdir -p $(dir $@)
	cp $< $@.tmp
	$(if $(filter y,$(ZEDBSD_GRAPHICAL_BOOT)),printf '%s\n' logo=logo.ppm login=graphical >> $@.tmp)
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

ifeq ($(ZEDBSD_VARIANT),native)
# WS062: the native layout.  The ESP carries the UEFI loader, the kernel and
# zedbsd.cfg; the root is a UFS partition mounted read-write through
# rootpart=; swap is a partition of its own.  Nothing is layered: a write
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
AMD64_NATIVE_UEFI_ZEDBSD_CONFIG := $(BUILD)/zedbsd-native-uefi-graphical-$(ZEDBSD_GRAPHICAL_BOOT)-kmsg-$(ZEDBSD_BOOT_KERNEL_MESSAGES)$(AMD64_BOOT_EXTRA_TAG).cfg
AMD64_GRAPHICAL_BOOT_LINES := logo=logo.ppm login=graphical
AMD64_NATIVE_ROOT_MIB ?= 1024
AMD64_NATIVE_ROOT_INODES ?= 65536
AMD64_NATIVE_SWAP_MIB ?= 1024
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

$(AMD64_NATIVE_UEFI_ZEDBSD_CONFIG): $(AMD64_PLATFORM)/zedbsd-native-uefi.cfg
	@mkdir -p $(dir $@)
	cp $< $@.tmp
	$(if $(filter y,$(ZEDBSD_GRAPHICAL_BOOT)),grep -v '^video=' $< > $@.tmp)
	$(if $(filter y,$(ZEDBSD_GRAPHICAL_BOOT)),printf '%s\n' $(AMD64_GRAPHICAL_BOOT_LINES) >> $@.tmp)
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
	$(AMD64_IMAGE_CONTRACT_STAMP) \
	platform/amd64/tools/check-amd64-native-image.py
	$(ZEDBSD_IMAGE_HOST) disk --machine pcat --layout native \
 --kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
 --zedbsd-config $(AMD64_NATIVE_UEFI_ZEDBSD_CONFIG) --logo $(AMD64_BOOT_LOGO) \
 --ufs-root $(AMD64_NATIVE_ROOT_IMAGE) --swapfile $(AMD64_NATIVE_SWAP_IMAGE) \
 $@.unchecked
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
	$(BUILD)/user64/userland/tests/posix-phase5-helper.o \
	$(AMD64_PLATFORM)/user.ld $(AMD64_USER_ELF_CHECK)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static \
 -z max-page-size=4096 -z stack-size=0x100000 \
 -T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_NET_LIBC_OBJS) \
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

$(BUILD)/uefi/memory-map-v6.o: $(UEFI_LOADER)/memory-map-v6.c $(UEFI_LOADER)/memory-map.h bootloader/common/memory-map.h bootloader/include/amd64-handoff.h
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
