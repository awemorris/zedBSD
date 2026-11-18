/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of the Digilent ZYBO (Zynq-7010) the board's files share: the
 * addresses of the PS's blocks, the system level control registers, the
 * Cortex-A9 MPCore's private blocks and the UART.
 *
 * The same code runs on the board and in the emulator's xilinx-zynq-a9
 * machine; the SLCR's PS IDCODE tells them apart (the emulator leaves it
 * zero), and the board's files ask zybo_emulated() where they differ.
 */

#ifndef KERN_HAL_ARMV7_BSP_ZYBO_ZYBO_H
#define KERN_HAL_ARMV7_BSP_ZYBO_ZYBO_H

#include <hal/types.h>

/*
 * The physical addresses and sizes of the blocks the HAL uses: the SLCR,
 * the MPCore's private region (SCU at +0x000, GIC CPU interface at +0x100,
 * global timer at +0x200, private timer at +0x600, GIC distributor at
 * +0x1000, PL310 at +0x2000) and UART1, the board's console.
 *
 * Values referenced: Linux v6.12 arch/arm/boot/dts/xilinx/zynq-7000.dtsi,
 * the reg of slcr, scu, intc, global_timer, scutimer, L2 and uart1.
 */
#define ZYBO_SLCR_PHYS			0xf8000000U
#define ZYBO_SLCR_BYTES			0x1000U
#define ZYBO_MPCORE_PHYS		0xf8f00000U
#define ZYBO_MPCORE_BYTES		0x3000U
#define ZYBO_MPCORE_SCU			0x0000U
#define ZYBO_MPCORE_GIC_CPU		0x0100U
#define ZYBO_MPCORE_GLOBAL_TIMER	0x0200U
#define ZYBO_MPCORE_PRIVATE_TIMER	0x0600U
#define ZYBO_MPCORE_GIC_DISTRIBUTOR	0x1000U
#define ZYBO_MPCORE_L2			0x2000U
#define ZYBO_UART1_PHYS			0xe0001000U
#define ZYBO_UART_BYTES			0x1000U

/*
 * The SLCR's registers the HAL reads and writes.
 *
 * Values referenced: Linux v6.12 arch/arm/mach-zynq/slcr.c (unlock 0x8,
 * PS_RST_CTRL 0x200, REBOOT_STATUS 0x258, PSS_IDCODE 0x530, the unlock
 * key 0xDF0D) and drivers/clk/zynq/clkc.c (ARM_PLL_CTRL 0x100,
 * DDR_PLL_CTRL 0x104, IO_PLL_CTRL 0x108, ARM_CLK_CTRL 0x120); the lock
 * register 0x4 and its key 0x767B, from U-Boot v2025.07
 * arch/arm/mach-zynq/slcr.c.
 */
#define ZYBO_SLCR_LOCK			0x004U
#define ZYBO_SLCR_UNLOCK		0x008U
#define ZYBO_SLCR_ARM_PLL_CTRL		0x100U
#define ZYBO_SLCR_DDR_PLL_CTRL		0x104U
#define ZYBO_SLCR_IO_PLL_CTRL		0x108U
#define ZYBO_SLCR_ARM_CLK_CTRL		0x120U
#define ZYBO_SLCR_PSS_RST_CTRL		0x200U
#define ZYBO_SLCR_REBOOT_STATUS		0x258U
#define ZYBO_SLCR_PSS_IDCODE		0x530U
#define ZYBO_SLCR_LOCK_KEY		0x767bU
#define ZYBO_SLCR_UNLOCK_KEY		0xdf0dU

int zybo_slcr_init(void);
uint32_t zybo_slcr_read(uint32_t offset);
void zybo_slcr_write(uint32_t offset, uint32_t value);
uint64_t zybo_slcr_periphclk(void);
volatile uint8_t *zybo_mpcore(void);
int zybo_emulated(void);
int zybo_uart_init(void);
void zybo_uart_putc(int character);
int zybo_uart_getc(void);

#endif
