/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ZYBO's processor setup before the MMU and the caches are on: the
 * Snoop Control Unit is turned on, and the Cortex-A9's auxiliary control
 * register gets SMP (the processor takes part in coherency) and FW (cache
 * and TLB maintenance is broadcast), as an SMP Linux kernel sets them even
 * on one processor.  The processor enters secure here, where the register
 * may be written.  Anything but a Cortex-A9 is left as it is.
 *
 * It runs from the physical copy of the image with the MMU off, so it
 * reaches the SCU at its physical address, and it uses r0 to r3 only
 * (bsp.h).
 *
 * Procedure referenced: Linux v6.12 arch/arm/mm/proc-v7.S,
 * __v7_ca9mp_setup and __v7_mp_setup (ACTLR bit 0 FW and bit 6 SMP, set
 * when not set; the Cortex-A9 by MIDR & 0xff0ffff0 == 0x410fc090), and
 * arch/arm/kernel/smp_scu.c, scu_enable (SCU_CTRL at 0x00, its enable
 * bit 0, set when not set).  Values referenced: Linux v6.12
 * arch/arm/boot/dts/xilinx/zynq-7000.dtsi, the SCU at 0xf8f00000.
 */

	.syntax unified
	.arm

	.text
	.balign 4
	.global bsp_cpu_early_setup
	.type bsp_cpu_early_setup, %function
bsp_cpu_early_setup:
	/* Only a Cortex-A9 has these bits and this SCU. */
	mrc p15, 0, r0, c0, c0, 0
	ldr r1, =0xff0ffff0
	and r0, r0, r1
	ldr r1, =0x410fc090
	cmp r0, r1
	bxne lr

	/* Turns the SCU on, keeping its other bits. */
	ldr r0, =0xf8f00000
	ldr r1, [r0]
	orr r1, r1, #0x1
	str r1, [r0]
	dsb

	/* Makes the processor take part in coherency and broadcast its maintenance. */
	mrc p15, 0, r1, c1, c0, 1
	orr r1, r1, #0x41
	mcr p15, 0, r1, c1, c0, 1
	isb
	bx lr
	.ltorg
	.size bsp_cpu_early_setup, . - bsp_cpu_early_setup
