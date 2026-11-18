/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5 (BCM2712): its name in the HAL's messages and handoff
 * objects, and the peripherals the HAL reaches at fixed addresses.
 */

#ifndef KERN_HAL_ARM64_BSP_RPI5_BOARD_H
#define KERN_HAL_ARM64_BSP_RPI5_BOARD_H

/*
 * The board's name in the HAL's boot messages, and the prefix of the
 * handoff objects the kernel reads ("rpi5.fdt").
 */
#define ARM64_BOARD_TAG "RPI5"
#define ARM64_BOARD_HANDOFF "rpi5"

/*
 * The peripherals the HAL reaches before it reads the device tree: the
 * debug UART (the three-pin connector between the HDMI ports) and the
 * always-on GPIO block that drives the activity LED.  Both sit in the
 * gigabyte from 0x10_4000_0000 that locore.S maps as device memory.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * arch/arm64/boot/dts/broadcom/bcm2712.dtsi (the soc node's ranges, uart10
 * and gio_aon).
 */
#define RPI5_DEBUG_UART_BASE 0x107d001000ULL
#define RPI5_AON_GPIO_BASE 0x107d517c00ULL

/*
 * Where the VideoCore sees the ARM's memory through the mailbox: at the
 * ARM's physical addresses, since the firmware node's dma-ranges is empty,
 * an identity.
 *
 * Value referenced: Raspberry Pi Linux rpi-6.12.y
 * arch/arm64/boot/dts/broadcom/bcm2712-rpi.dtsi (the firmware node).
 */
#define RPI_BOARD_MAILBOX_BUS_ALIAS 0x00000000U

/*
 * The base clock of the SD card's controller, which the HAL hands the
 * kernel as "rpi5.sdhci.clock": a fixed 200 MHz, whatever the controller's
 * capabilities say.
 *
 * Value referenced: Raspberry Pi Linux rpi-6.12.y
 * arch/arm64/boot/dts/broadcom/bcm2712.dtsi (clk_emmc2, sdio1's clock).
 */
#define ARM64_BOARD_SDHCI_CLOCK_HZ 200000000ULL

#endif
