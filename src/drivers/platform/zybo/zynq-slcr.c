/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Zynq-7000's system level control registers (SLCR) as the kernel's
 * drivers reach them.
 *
 * The drivers of the board's peripherals read the clock tree here to learn
 * the rate their controller runs at, and switch a clock on that the first
 * stage loader left off.  The SLCR is written only between an unlock and a
 * lock, so every write here is that sequence under one lock and the SLCR
 * is locked again between two of them.  The hardware layer maps the same
 * registers for its own use (the timers' clock and the system reset) and
 * keeps to the same sequence; the board has one processor running.
 */

#include <drivers/platform/zybo/zynq-slcr.h>

#include <stddef.h>

#include <kern/device-io.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/*
 * Where the SLCR is, and how many bytes of it are mapped.
 *
 * Values referenced: Linux v6.12 arch/arm/boot/dts/xilinx/zynq-7000.dtsi
 * (slcr, reg 0xf8000000 0x1000).
 */
#define ZYNQ_SLCR_PHYS			0xf8000000ULL
#define ZYNQ_SLCR_BYTES			0x1000U

/*
 * The lock and unlock registers and the keys they take.
 *
 * Values referenced: Linux v6.12 drivers/clk/zynq/clkc.c and
 * arch/arm/mach-zynq/slcr.c (SLCR_LOCK 0x4 with 0x767b, SLCR_UNLOCK 0x8
 * with 0xdf0d).
 */
#define ZYNQ_SLCR_LOCK			0x004U
#define ZYNQ_SLCR_UNLOCK		0x008U
#define ZYNQ_SLCR_LOCK_KEY		0x767bU
#define ZYNQ_SLCR_UNLOCK_KEY		0xdf0dU

/*
 * The board's crystal, PS_CLK, which every PLL multiplies.
 *
 * Value referenced: Linux v6.12 arch/arm/boot/dts/xilinx/zynq-zybo.dts
 * (ps-clk-frequency).
 */
#define ZYNQ_PS_CLK_HZ			50000000ULL

/*
 * The fields of a PLL's control register: its feedback divider, and the
 * bypass that passes the crystal through.
 *
 * Values referenced: Linux v6.12 drivers/clk/zynq/pll.c (PLLCTRL_FBDIV in
 * bits 18:12) and drivers/clk/zynq/clkc.c (the PLL's bypass mux in bit 4
 * of its control register).
 */
#define ZYNQ_PLL_FDIV_SHIFT		12U
#define ZYNQ_PLL_FDIV_MASK		0x7fU
#define ZYNQ_PLL_BYPASS			0x10U

/*
 * The fields of ARM_CLK_CTRL and CLK_621_TRUE: the PLL the processor clock
 * comes from (the ARM PLL for 0 and 1, the DDR PLL for 2, the I/O PLL for
 * 3), its divider, and the ratio bit that makes the bus clock cpu_1x a
 * sixth of the divided clock (6:2:1) rather than a quarter (4:2:1).
 *
 * Values referenced: Linux v6.12 drivers/clk/zynq/clkc.c (cpu_mux in bits
 * 5:4 of ARM_CLK_CTRL over armpll, armpll, ddrpll and iopll; cpu_div in
 * bits 13:8, one-based, zero meaning undivided; cpu_1x_div dividing
 * cpu_div by 4 + 2 times bit 0 of 621_TRUE).
 */
#define ZYNQ_CPU_SRCSEL_SHIFT		4U
#define ZYNQ_CPU_SRCSEL_MASK		0x3U
#define ZYNQ_CPU_SRCSEL_DDR		2U
#define ZYNQ_CPU_SRCSEL_IO		3U
#define ZYNQ_CPU_DIVISOR_SHIFT		8U
#define ZYNQ_CPU_DIVISOR_MASK		0x3fU
#define ZYNQ_CLK_621_TRUE_BIT		0x1U
#define ZYNQ_CPU_1X_RATIO_421		4U
#define ZYNQ_CPU_1X_RATIO_621		6U

/* The largest offset of a register in the mapped window. */
#define ZYNQ_SLCR_LAST_REGISTER		(ZYNQ_SLCR_BYTES - 4U)

/*
 * The SLCR mapped in kernel space, NULL before drv_zynq_slcr_init().
 *
 * Filled once by the first initialization during the platform's device
 * discovery and kept for the life of the system.
 */
static volatile uint8_t *slcr_registers;

/*
 * Serializes the unlock, write and lock sequences, so one never ends with
 * the SLCR unlocked or locks it under another.  A leaf: nothing is taken
 * under it.
 */
static struct spinlock slcr_lock;

static int offset_valid(uint32_t offset);

/*
 * Maps the SLCR for the drivers; a second call finds it mapped.
 *
 * Returns 0, or the errno of the failed mapping.
 */
int
drv_zynq_slcr_init(
	void)
{
	void *mapped;
	int error;

	/* An SLCR mapped earlier is ready. */
	if (slcr_registers != NULL)
		return 0;

	/* Maps the registers as device memory. */
	error = kern_device_map(ZYNQ_SLCR_PHYS, ZYNQ_SLCR_BYTES, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Prepares the lock of the write sequences, then publishes the map. */
	spin_init(&slcr_lock, LOCK_RANK_DEVICE, "zynq-slcr");
	slcr_registers = mapped;

	/* Succeeded: the SLCR is reachable. */
	return 0;
}

/*
 * Reads one SLCR register.
 *
 * Returns 0 with the register's value, ENXIO before the SLCR is mapped,
 * or EINVAL for an offset outside it or not word aligned.
 */
int
drv_zynq_slcr_read(
	uint32_t offset,
	uint32_t *value)
{
	int valid;

	/* Refuses nowhere to put the value. */
	if (value == NULL)
		return EINVAL;

	/* Refuses a read before the map. */
	if (slcr_registers == NULL)
		return ENXIO;

	/* Refuses an offset that names no register. */
	valid = offset_valid(offset);
	if (!valid)
		return EINVAL;

	/* Succeeded: the register's value. */
	*value = kern_mmio_read32(slcr_registers + offset);
	return 0;
}

/*
 * Replaces the masked bits of one SLCR register, between an unlock and a
 * lock.
 *
 * Returns 0, ENXIO before the SLCR is mapped, or EINVAL for an offset
 * outside it or not word aligned.
 */
int
drv_zynq_slcr_modify(
	uint32_t offset,
	uint32_t mask,
	uint32_t value)
{
	unsigned long enabled;
	uint32_t word;
	int valid;

	/* Refuses a write before the map. */
	if (slcr_registers == NULL)
		return ENXIO;

	/* Refuses an offset that names no register. */
	valid = offset_valid(offset);
	if (!valid)
		return EINVAL;

	/* Unlocks the SLCR, replaces the bits, and locks it again. */
	enabled = spin_lock_irqsave(&slcr_lock);

	word = kern_mmio_read32(slcr_registers + offset);
	word &= ~mask;
	word |= value & mask;
	kern_mmio_write32(slcr_registers + ZYNQ_SLCR_UNLOCK, ZYNQ_SLCR_UNLOCK_KEY);
	kern_mmio_write32(slcr_registers + offset, word);
	kern_mmio_write32(slcr_registers + ZYNQ_SLCR_LOCK, ZYNQ_SLCR_LOCK_KEY);

	spin_unlock_irqrestore(&slcr_lock, enabled);

	/* Succeeded: the register holds the new bits. */
	return 0;
}

/*
 * Reports one PLL's output in Hz: the crystal times the PLL's feedback
 * divider, or the crystal itself when the PLL is bypassed.
 *
 * Returns 0 when the SLCR cannot be read.
 */
uint64_t
drv_zynq_slcr_pll_hz(
	uint32_t control_offset)
{
	uint32_t control;
	uint32_t multiplier;
	int error;

	/* Reads the PLL's control register; without it the rate is unknown. */
	error = drv_zynq_slcr_read(control_offset, &control);
	if (error != 0)
		return 0;

	/* A bypassed PLL passes the crystal through. */
	if ((control & ZYNQ_PLL_BYPASS) != 0)
		return ZYNQ_PS_CLK_HZ;

	/* Succeeded: the crystal multiplied by the feedback divider. */
	multiplier = (control >> ZYNQ_PLL_FDIV_SHIFT) & ZYNQ_PLL_FDIV_MASK;
	return ZYNQ_PS_CLK_HZ * multiplier;
}

/*
 * Reports the bus clock cpu_1x in Hz: the processor clock's PLL divided by
 * the processor clock's divider and by the 6:2:1 or 4:2:1 ratio.  The
 * peripherals' register interfaces run on it, and the Ethernet
 * controller's MDIO clock is divided from it.
 *
 * Returns 0 when the SLCR cannot be read or the PLL gives no rate.
 */
uint64_t
drv_zynq_slcr_cpu_1x_hz(
	void)
{
	uint64_t parent;
	uint32_t control;
	uint32_t ratio_bit;
	uint32_t source;
	uint32_t divisor;
	uint32_t ratio;
	int error;

	/* Reads the processor clock's control; without it the rate is unknown. */
	error = drv_zynq_slcr_read(DRV_ZYNQ_SLCR_ARM_CLK_CTRL, &control);
	if (error != 0)
		return 0;

	/* Reads the clock ratio; without it the rate is unknown. */
	error = drv_zynq_slcr_read(DRV_ZYNQ_SLCR_CLK_621_TRUE, &ratio_bit);
	if (error != 0)
		return 0;

	/* Finds the PLL the processor clock comes from. */
	source = (control >> ZYNQ_CPU_SRCSEL_SHIFT) & ZYNQ_CPU_SRCSEL_MASK;
	if (source == ZYNQ_CPU_SRCSEL_DDR) {
		parent = drv_zynq_slcr_pll_hz(DRV_ZYNQ_SLCR_DDR_PLL_CTRL);
	} else if (source == ZYNQ_CPU_SRCSEL_IO) {
		parent = drv_zynq_slcr_pll_hz(DRV_ZYNQ_SLCR_IO_PLL_CTRL);
	} else {
		parent = drv_zynq_slcr_pll_hz(DRV_ZYNQ_SLCR_ARM_PLL_CTRL);
	}

	/* The processor clock's divider; zero divides by one. */
	divisor = (control >> ZYNQ_CPU_DIVISOR_SHIFT) & ZYNQ_CPU_DIVISOR_MASK;
	if (divisor == 0)
		divisor = 1;

	/* cpu_1x is a sixth of the divided clock at 6:2:1, a quarter at 4:2:1. */
	ratio = ZYNQ_CPU_1X_RATIO_421;
	if ((ratio_bit & ZYNQ_CLK_621_TRUE_BIT) != 0)
		ratio = ZYNQ_CPU_1X_RATIO_621;

	/* Succeeded: the bus clock, zero when the PLL gave none. */
	return parent / divisor / ratio;
}

/* Reports whether an offset names a word-aligned register of the window. */
static int
offset_valid(
	uint32_t offset)
{
	/* A register is a word at a word-aligned offset. */
	if ((offset & 3U) != 0)
		return 0;

	/* The register lies inside the mapped window. */
	if (offset > ZYNQ_SLCR_LAST_REGISTER)
		return 0;

	/* The offset names a register. */
	return 1;
}
