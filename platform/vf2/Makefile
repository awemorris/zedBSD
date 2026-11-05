# Defaults
override CONFIG_DRIVER_PCI                 := y
override CONFIG_DRIVER_USB                 := y
override CONFIG_DRIVER_PCI_NVME            := y
override CONFIG_DRIVER_PCI_AHCI            := y
override CONFIG_DRIVER_LOOP                := y
override CONFIG_DRIVER_FS_UFS              := y
override CONFIG_DRIVER_FS_FAT              := y
override CONFIG_DRIVER_FS_OVERLAYFS        := y
override CONFIG_KERN_NET                   := y
override CONFIG_KERN_NET_IPV4              := y
override CONFIG_KERN_NET_IPV6              := y
override CONFIG_KERN_NET_ETHERNET          := y
override CONFIG_KERN_NET_WIFI              := y
override CONFIG_DRIVER_USB_CDC_NCM         := y
override CONFIG_DRIVER_USB_CDC_ECM         := y
override CONFIG_DRIVER_PCI_RTL8125         := y
override CONFIG_DRIVER_USB_RTL815X         := y
override CONFIG_DRIVER_VF2_ETHERNET        := y
override CONFIG_DRIVER_USB_RTL8822BU       := y
override CONFIG_DRIVER_GRAPHICS            := y
override CONFIG_DRIVER_GRAPHICS_GENERIC    := y
override CONFIG_DRIVER_GPU                 := y
override CONFIG_DRIVER_POWERVR             := y
override CONFIG_DRIVER_AUDIO               := y
override CONFIG_DRIVER_VF2_AUDIO           := y
override CONFIG_DRIVER_HID                 := y
override CONFIG_DRIVER_USB                 := y
override CONFIG_DRIVER_PCI_EHCI            := y
override CONFIG_DRIVER_PCI_XHCI            := y
override CONFIG_DRIVER_USB_HUB             := y
override CONFIG_DRIVER_USB_HID             := y
override CONFIG_DRIVER_USB_STORAGE         := y
override CONFIG_DRIVER_USB_UASP            := y
override CONFIG_DRIVER_USB_CCID            := y
override CONFIG_DRIVER_USB_BT              := y
override CONFIG_DRIVER_USB_UVC             := y
override CONFIG_DRIVER_BT                  := y
override CONFIG_DRIVER_INPUT_BRIDGE        := y
override CONFIG_DRIVER_RPI5_BT             := y
override CONFIG_DRIVER_CPU_PERF            := y
override CONFIG_DRIVER_CAMERA              := y
override CONFIG_DRIVER_I2C                 := y
override CONFIG_DRIVER_GPIO                := y

# HAL C Sources
KERN_SOURCES += \
	src/hal/riscv64/cmain.c \
	src/hal/riscv64/bootinfo.c \
	src/hal/riscv64/fdt.c \
	src/hal/riscv64/sbi.c \
	src/hal/riscv64/percpu.c \
	src/hal/riscv64/smp.c \
	src/hal/riscv64/int.c \
	src/hal/riscv64/irq.c \
	src/hal/riscv64/plic.c \
	src/hal/riscv64/clock.c \
	src/hal/riscv64/page.c \
	src/hal/riscv64/space.c \
	src/hal/riscv64/window.c \
	src/hal/riscv64/task.c \
	src/hal/riscv64/debug.c \
	src/hal/riscv64/asm.c \
	src/hal/riscv64/lib.c \
	src/hal/riscv64/loader.c \
	src/hal/riscv64/cons.c \
	src/hal/riscv64/fbcons.c \
	src/hal/riscv64/font.c \
	src/hal/idle-suspend-unsupported.c

# HAL Assembly Sources
KERN_ASM := \
	src/hal/riscv64/locore.S \
	src/hal/riscv64/trap.S \
	src/hal/riscv64/dispatch.S

# Kernel Sources
KERN_SOURCES += \
	src/kern/platform/visionfive2.c \
	src/drivers/serial/ns16550.c \
	src/drivers/generic/serial-console.c \
	src/drivers/generic/boot-fb.c \
	src/drivers/generic/fb-text.c \
	src/drivers/platform/pcat/graphics/vgafont.c \
	src/drivers/generic/ramdisk.c \
	src/drivers/platform/visionfive2/vf2-syscon.c \
	src/drivers/platform/visionfive2/vf2-crg.c \
	src/drivers/platform/visionfive2/vf2-pinctrl.c \
	src/drivers/platform/visionfive2/vf2-mmc.c \
	src/drivers/mmc/dw-mmc.c \
	src/drivers/disklabel/gpt.c \
	src/drivers/disklabel/pcat.c \
	src/drivers/dma/dw-axi-dmac.c \
	src/drivers/platform/visionfive2/vf2-dmac.c \
	src/drivers/platform/visionfive2/vf2-pwmdac.c

include platform/rpi5/vmunix.mk
include platform/rpi5/bootloader.mk
include platform/rpi5/rootfs.mk
include platform/rpi5/disk-image.mk
include platform/rpi5/run.mk
include platform/rpi5/toolchain.mk
