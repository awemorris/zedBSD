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
override CONFIG_DRIVER_VIDEOCORE_VII       := y
override CONFIG_DRIVER_AUDIO               := y
override CONFIG_DRIVER_RPI5_AUDIO          := y
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
	src/hal/arm64/bsp-rpi5/uart.c \
	src/hal/arm64/bsp-rpi5/fdt.c \
	src/hal/arm64/bsp-rpi5/activity-led.c
	src/hal/arm64/bsp-rpi5/board.c \
	src/hal/arm64/bsp-rpi-common/mailbox.c \
	src/hal/arm64/bsp-rpi-common/framebuffer-mailbox.c

# HAL Assembly Sources
KERN_ASM := \
	src/hal/arm64/locore.S \
	src/hal/arm64/trap.S \
	src/hal/arm64/dispatch.S

# Kernel Sources
KERN_SOURCES += \
	src/kern/platform/rpi5.c \
	src/drivers/platform/rpi-common/rpi-sdhci.c \
	src/drivers/platform/rpi-common/rpi-console.c \
	src/drivers/platform/rpi-common/rpi-mmc-irq.c \
	src/drivers/platform/rpi-common/rpi-sdio-host.c \
	src/drivers/platform/rpi-common/rpi-sdio.c \
	src/drivers/platform/rpi-common/rpi-gpio.c \
	src/drivers/platform/rpi5/rp1.c \
	src/drivers/platform/rpi5/rp1-gpio.c \
	src/drivers/platform/rpi5/rp1-irq.c \
	src/drivers/platform/rpi5/rp1-usb.c \
	src/drivers/platform/rpi5/rp1-ethernet.c \
	src/drivers/platform/rpi5/rpi5-gio.c \
	src/drivers/platform/rpi5/rpi5-pinctrl.c \
	src/drivers/platform/rpi5/rpi5-wifi.c \
	src/drivers/platform/rpi5/rpi5-serror.c \
	src/drivers/platform/rpi5/rpi5-pcie.c \
	src/drivers/platform/rpi5/rpi5-mip.c \
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
	src/drivers/mmc/sdhci.c \
	src/drivers/generic/fdt.c \
	src/drivers/generic/dma.c \
	src/drivers/pci/pci-brcmstb.c

include platform/arm64/rpi5-firmware.mk

ARM64_BOOT_FIRMWARE_DIR := $(RPI5_BOOT_FIRMWARE_DIR)
ARM64_BOOT_FIRMWARE_CACHE_FILES := $(RPI5_BOOT_FIRMWARE_CACHE_FILES)

include platform/rpi5/vmunix.mk
include platform/rpi5/bootloader.mk
include platform/rpi5/rootfs.mk
include platform/rpi5/disk-image.mk
include platform/rpi5/run.mk
include platform/rpi5/toolchain.mk
