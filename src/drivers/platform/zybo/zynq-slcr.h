/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Zynq-7000's system level control registers as the kernel's drivers
 * reach them: the clock gates and dividers of the peripherals.
 */

#ifndef KERN_DRIVERS_PLATFORM_ZYBO_ZYNQ_SLCR_H
#define KERN_DRIVERS_PLATFORM_ZYBO_ZYNQ_SLCR_H

#include <stdint.h>

/*
 * The registers the drivers read and write, as offsets from the SLCR's
 * base: the three PLLs, the processor clock, the peripherals' bus clock
 * gates (APER_CLK_CTRL), the first Ethernet controller's receive and
 * transmit clocks, the SD controllers' reference clock, and the processor
 * clock ratio that decides the bus clock cpu_1x.
 *
 * Values referenced: Linux v6.12 drivers/clk/zynq/clkc.c (ARM_PLL_CTRL
 * 0x100, DDR_PLL_CTRL 0x104, IO_PLL_CTRL 0x108, ARM_CLK_CTRL 0x120,
 * APER_CLK_CTRL 0x12c, GEM0_CLK_CTRL 0x140, SDIO_CLK_CTRL 0x150,
 * 621_TRUE 0x1c4); U-Boot v2025.07
 * board/xilinx/zynq/zynq-zybo/ps7_init_gpl.c (GEM0_RCLK_CTRL 0x138).
 */
#define DRV_ZYNQ_SLCR_ARM_PLL_CTRL	0x100U
#define DRV_ZYNQ_SLCR_DDR_PLL_CTRL	0x104U
#define DRV_ZYNQ_SLCR_IO_PLL_CTRL	0x108U
#define DRV_ZYNQ_SLCR_ARM_CLK_CTRL	0x120U
#define DRV_ZYNQ_SLCR_APER_CLK_CTRL	0x12cU
#define DRV_ZYNQ_SLCR_GEM0_RCLK_CTRL	0x138U
#define DRV_ZYNQ_SLCR_GEM0_CLK_CTRL	0x140U
#define DRV_ZYNQ_SLCR_SDIO_CLK_CTRL	0x150U
#define DRV_ZYNQ_SLCR_CLK_621_TRUE	0x1c4U

int drv_zynq_slcr_init(void);
int drv_zynq_slcr_read(uint32_t offset, uint32_t *value);
int drv_zynq_slcr_modify(uint32_t offset, uint32_t mask, uint32_t value);
uint64_t drv_zynq_slcr_pll_hz(uint32_t control_offset);
uint64_t drv_zynq_slcr_cpu_1x_hz(void);

#endif
