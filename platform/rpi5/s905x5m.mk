# zedBSD arm64: the Amlogic S905X5M's drivers the ODROID-C5 builds.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

# The SoC's peripheral drivers under src/drivers/platform/s905x5m/ and the
# shared cores they sit on: the console on the debug UART and the boot
# framebuffer; the SoC's clocks, resets, pins and device tree;
# the SD card on the Amlogic SD/eMMC core; the Ethernet port on the
# DesignWare EQoS core with its PHY drivers; and the USB host shown to the
# PCI core, whose xHCI driver comes with the USB sources.
ARM64_S905X5M_SOURCES := \
	src/drivers/platform/s905x5m/s905x5m-console.c \
	src/drivers/platform/s905x5m/s905x5m-soc.c \
	src/drivers/platform/s905x5m/s905x5m-mmc.c \
	src/drivers/mmc/meson-mmc.c \
	src/drivers/platform/s905x5m/s905x5m-ethernet.c \
	src/drivers/ethernet/dw-eqos.c \
	src/drivers/ethernet/mii-phy.c \
	src/drivers/ethernet/yt8531.c \
	src/drivers/platform/s905x5m/s905x5m-usb.c \
	src/drivers/pci/pci.c
