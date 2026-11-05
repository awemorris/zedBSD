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
override CONFIG_DRIVER_USB_RTL815X         := y
override CONFIG_DRIVER_RPI4_ETHERNET       := y
override CONFIG_DRIVER_USB_RTL8822BU       := y
override CONFIG_DRIVER_RPI4_WIFI           := y
override CONFIG_DRIVER_GRAPHICS            := y
override CONFIG_DRIVER_GRAPHICS_GENERIC    := y
override CONFIG_DRIVER_GPU                 := y
override CONFIG_DRIVER_VIDEOCORE_VI        := y
override CONFIG_DRIVER_AUDIO               := y
override CONFIG_DRIVER_RPI4_AUDIO          := y
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
override CONFIG_DRIVER_RPI4_BT             := y
override CONFIG_DRIVER_CPU_PERF            := y
override CONFIG_DRIVER_CAMERA              := y
override CONFIG_DRIVER_I2C                 := y
override CONFIG_DRIVER_GPIO                := y

# HAL C Sources
KERN_SOURCES += \
	src/hal/arm64/bsp-rpi4/uart.c \
	src/hal/arm64/bsp-rpi4/fdt.c \
	src/hal/arm64/bsp-rpi4/activity-led.c \
	src/hal/arm64/bsp-rpi4/board.c \
	src/hal/arm64/bsp-rpi4/watch-hvs.c \
	src/hal/arm64/bsp-rpi-common/mailbox.c \
	src/hal/arm64/bsp-rpi-common/framebuffer-mailbox.c

# HAL Assembly Sources
KERN_ASM := \
	src/hal/arm64/locore.S \
	src/hal/arm64/trap.S \
	src/hal/arm64/dispatch.S

# Kernel Sources
KERN_SOURCES += \
	src/kern/platform/rpi4.c \
	src/drivers/platform/rpi-common/rpi-sdhci.c \
	src/drivers/platform/rpi-common/rpi-console.c \
	src/drivers/platform/rpi4/rpi4-pcie.c \
	src/drivers/platform/rpi4/rpi4-board.c \
	src/drivers/platform/rpi4/rpi4-firmware.c \
	src/drivers/platform/rpi-common/rpi-gpio.c \
	src/drivers/wifi/bcm43455/bcm43455.c \
	src/drivers/wifi/bcm43455/bcm43455-bcdc.c \
	src/drivers/wifi/bcm43455/bcm43455-bus.c \
	src/drivers/wifi/bcm43455/bcm43455-chip.c \
	src/drivers/wifi/bcm43455/bcm43455-event.c \
	src/drivers/wifi/bcm43455/bcm43455-files.c \
	src/drivers/wifi/bcm43455/bcm43455-nvram.c \
	src/drivers/wifi/bcm43455/bcm43455-radio.c \
	src/drivers/wifi/bcm43455/bcm43455-sdpcm.c \
	src/drivers/wifi/bcm43455/bcm43455-station.c \
	src/drivers/pci/pci-brcmstb.c \
	src/drivers/mmc/sdhci.c \
	src/drivers/generic/fdt.c \
	src/drivers/generic/dma.c

ARM64_BOOT_FIRMWARE_DIR := $(RPI4_BOOT_FIRMWARE_DIR)
ARM64_BOOT_FIRMWARE_CACHE_FILES := $(RPI4_BOOT_FIRMWARE_CACHE_FILES)

include platform/rpi4/vmunix.mk
include platform/rpi4/bootloader.mk
include platform/rpi4/rootfs.mk
include platform/rpi4/disk-image.mk
include platform/rpi4/run.mk
include platform/rpi4/toolchain.mk
