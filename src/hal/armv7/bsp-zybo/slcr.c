/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ZYBO's system level control registers and the MPCore's private
 * region, as the HAL reaches them: mapped once into the device window at
 * boot, read freely, and written only between an unlock and a lock.
 *
 * The clock the private and global timers count is the MPCore's
 * peripheral clock, half the processor's.  On the board it follows from
 * the PLL and the divider the first-stage loader (U-Boot SPL's ps7_init)
 * programmed; the emulator's timers ignore the SLCR and count 10 ns
 * steps, so there it is 100 MHz.
 */

#include <hal/hal.h>
#include "../defs.h"
#include "../pte.h"
#include "../space.h"
#include "zybo.h"

/*
 * The board's crystal, PS_CLK, which every PLL multiplies.
 *
 * Values referenced: Linux v6.12 arch/arm/boot/dts/xilinx/zynq-zybo.dts,
 * ps-clk-frequency.
 */
#define SLCR_PS_CLK_HZ		50000000ULL

/*
 * The emulator's timer clock: one count per 10 ns with no prescaler.
 *
 * Values referenced: QEMU v10.0.11 hw/timer/arm_mptimer.c
 * (timerblock_scale) and hw/timer/a9gtimer.c (a9_gtimer_get_scale).
 */
#define SLCR_EMULATED_TIMER_HZ	100000000ULL

/*
 * The fields of a PLL's control register and of ARM_CLK_CTRL: the PLL's
 * feedback divider and its bypass, the processor clock's source and its
 * divider.
 *
 * Values referenced: Linux v6.12 drivers/clk/zynq/pll.c (PLLCTRL_FBDIV in
 * bits 18:12) and drivers/clk/zynq/clkc.c (the PLL's bypass mux in bit 4
 * of its control register; cpu_mux in bits 5:4 of ARM_CLK_CTRL, armpll
 * for 0 and 1, ddrpll for 2, iopll for 3; cpu_div in bits 13:8, one-based,
 * zero meaning undivided; cpu_3or2x, the timers' clock, half of
 * cpu_6or4x).
 */
#define SLCR_PLL_FDIV_SHIFT	12U
#define SLCR_PLL_FDIV_MASK	0x7fU
#define SLCR_PLL_BYPASS		0x10U
#define SLCR_CPU_SRCSEL_SHIFT	4U
#define SLCR_CPU_SRCSEL_MASK	0x3U
#define SLCR_CPU_SRCSEL_DDR	2U
#define SLCR_CPU_SRCSEL_IO	3U
#define SLCR_CPU_DIVISOR_SHIFT	8U
#define SLCR_CPU_DIVISOR_MASK	0x3fU

/*
 * The SLCR's and the MPCore region's registers in the device window, NULL
 * before zybo_slcr_init().  Set once on the boot processor.
 */
static volatile uint8_t *slcr_registers;
static volatile uint8_t *mpcore_registers;

/*
 * Whether the machine is the emulator's: the PS IDCODE read zero.  Set
 * once by zybo_slcr_init(), before anything asks.
 */
static int slcr_emulated;

static uint64_t pll_output(uint32_t control_offset);

/*
 * Maps the SLCR and the MPCore's private region, and tells the board from
 * the emulator by the PS IDCODE.
 */
int
zybo_slcr_init(
	void)
{
	void *mapped;
	uint32_t idcode;
	int error;

	/* Maps the SLCR. */
	error = armv7_space_map_device_window(ZYBO_SLCR_PHYS, ZYBO_SLCR_BYTES, ARMV7_PTE_ATTR_DEVICE, &mapped);
	if (error != HAL_OK)
		return error;
	slcr_registers = mapped;

	/* Maps the MPCore's private region: SCU, GIC, timers and the L2 controller. */
	error = armv7_space_map_device_window(ZYBO_MPCORE_PHYS, ZYBO_MPCORE_BYTES, ARMV7_PTE_ATTR_DEVICE, &mapped);
	if (error != HAL_OK)
		return error;
	mpcore_registers = mapped;

	/*
	 * The emulator leaves the PS IDCODE zero (Values referenced: QEMU
	 * v10.0.11 hw/misc/zynq_slcr.c, PSS_IDCODE read-only with no reset
	 * value); the board's is its part number.
	 */
	idcode = zybo_slcr_read(ZYBO_SLCR_PSS_IDCODE);
	slcr_emulated = 0;
	if (idcode == 0)
		slcr_emulated = 1;

	/* Succeeded: the registers are reachable. */
	return HAL_OK;
}

/*
 * Reads one SLCR register.
 */
uint32_t
zybo_slcr_read(
	uint32_t offset)
{
	uint32_t value;

	/* Before the map there is nothing to read. */
	if (slcr_registers == NULL)
		return 0;

	/* Succeeded: the register's value. */
	value = hal_mmio_read32(slcr_registers + offset);
	return value;
}

/*
 * Writes one SLCR register between an unlock and a lock, which every
 * write to it needs.
 */
void
zybo_slcr_write(
	uint32_t offset,
	uint32_t value)
{
	/* Before the map there is nothing to write. */
	if (slcr_registers == NULL)
		return;

	/* Unlocks, writes and locks again. */
	hal_mmio_write32(slcr_registers + ZYBO_SLCR_UNLOCK, ZYBO_SLCR_UNLOCK_KEY);
	hal_mmio_write32(slcr_registers + offset, value);
	hal_mmio_write32(slcr_registers + ZYBO_SLCR_LOCK, ZYBO_SLCR_LOCK_KEY);
}

/*
 * Reports the MPCore's peripheral clock, which the private and global
 * timers count, in Hz; zero when the clock tree is not one this reads.
 */
uint64_t
zybo_slcr_periphclk(
	void)
{
	uint32_t control;
	uint32_t source;
	uint32_t divisor;
	uint64_t parent;

	/* The emulator's timers count 10 ns steps whatever the SLCR says. */
	if (slcr_emulated)
		return SLCR_EMULATED_TIMER_HZ;

	/* The processor clock's source: one of the three PLLs. */
	control = zybo_slcr_read(ZYBO_SLCR_ARM_CLK_CTRL);
	source = (control >> SLCR_CPU_SRCSEL_SHIFT) & SLCR_CPU_SRCSEL_MASK;
	if (source == SLCR_CPU_SRCSEL_DDR) {
		parent = pll_output(ZYBO_SLCR_DDR_PLL_CTRL);
	} else if (source == SLCR_CPU_SRCSEL_IO) {
		parent = pll_output(ZYBO_SLCR_IO_PLL_CTRL);
	} else {
		parent = pll_output(ZYBO_SLCR_ARM_PLL_CTRL);
	}

	/* The processor clock is the source divided; a zero divider divides by one. */
	divisor = (control >> SLCR_CPU_DIVISOR_SHIFT) & SLCR_CPU_DIVISOR_MASK;
	if (divisor == 0)
		divisor = 1;

	/* Succeeded: the peripheral clock, half the processor's. */
	return parent / divisor / 2U;
}

/*
 * Reports the MPCore's private region in the device window, or NULL
 * before zybo_slcr_init().
 */
volatile uint8_t *
zybo_mpcore(
	void)
{
	/* Succeeded: the address mapped at boot. */
	return mpcore_registers;
}

/*
 * Reports whether the machine is the emulator's rather than the board.
 */
int
zybo_emulated(
	void)
{
	/* Succeeded: what the PS IDCODE said at boot. */
	return slcr_emulated;
}

/* Reports one PLL's output in Hz: PS_CLK times its divider, or PS_CLK bypassed. */
static uint64_t
pll_output(
	uint32_t control_offset)
{
	uint32_t control;
	uint32_t multiplier;

	/* A bypassed PLL passes the crystal through. */
	control = zybo_slcr_read(control_offset);
	if ((control & SLCR_PLL_BYPASS) != 0)
		return SLCR_PS_CLK_HZ;

	/* Succeeded: the crystal multiplied by the feedback divider. */
	multiplier = (control >> SLCR_PLL_FDIV_SHIFT) & SLCR_PLL_FDIV_MASK;
	return SLCR_PS_CLK_HZ * multiplier;
}
