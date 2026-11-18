/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Zynq-7000's glue for the SD card slot's controller, SDIO0, an Arasan
 * SDHCI of specification version 2.00.
 *
 * The first stage loader (U-Boot SPL's ps7_init) has routed the slot's
 * pins and started the controller's clocks; the glue switches on a clock
 * it finds off, takes the controller's base clock from the SLCR (the
 * controller's capabilities name none), and hands the mapped controller to
 * the SDHCI driver, which talks to the card.  The driver polls; the
 * controller's interrupt is not used.
 *
 * The slot's card detect is read from the controller's present state for
 * the boot log only: the card is initialized whatever it says.
 */

#include <drivers/platform/zybo/zynq-sdhci.h>

#include <stddef.h>
#include <stdint.h>

#include <drivers/mmc/sdhci.h>
#include <drivers/platform/zybo/zynq-slcr.h>
#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/*
 * Where SDIO0's registers are, and how many bytes of them are mapped.
 *
 * Values referenced: Linux v6.12 arch/arm/boot/dts/xilinx/zynq-7000.dtsi
 * (sdhci0, reg 0xe0100000 0x1000).
 */
#define ZYNQ_SDIO0_PHYS			0xe0100000ULL
#define ZYNQ_SDIO0_BYTES		0x1000U

/* The unit number of the slot's card, which names its disk mmcblk0. */
#define ZYNQ_SDIO0_UNIT			0U

/*
 * SDIO0's bus clock gate in APER_CLK_CTRL.
 *
 * Value referenced: Linux v6.12 drivers/clk/zynq/clkc.c (sdio0_aper, bit 10
 * of APER_CLK_CTRL).
 */
#define ZYNQ_APER_SDIO0			0x00000400U

/*
 * The fields of SDIO_CLK_CTRL: SDIO0's reference clock gate, the PLL the
 * reference clock comes from, and the divider applied to it.  The source
 * is the I/O PLL for 0 and 1, the ARM PLL for 2 and the DDR PLL for 3.
 *
 * Values referenced: Linux v6.12 drivers/clk/zynq/clkc.c (the periph
 * clock's gate in bit 0, its mux over io, io, arm and ddr PLLs in bits
 * 5:4, its divider in bits 13:8); QEMU v10.0.11 hw/misc/zynq_slcr.c (the
 * same fields in its clock computation).
 */
#define ZYNQ_SDIO_CLKACT0		0x00000001U
#define ZYNQ_SDIO_SRCSEL_SHIFT		4U
#define ZYNQ_SDIO_SRCSEL_MASK		0x3U
#define ZYNQ_SDIO_SRCSEL_ARM		2U
#define ZYNQ_SDIO_SRCSEL_DDR		3U
#define ZYNQ_SDIO_DIVISOR_SHIFT		8U
#define ZYNQ_SDIO_DIVISOR_MASK		0x3fU

/*
 * The base clock taken when the SLCR gives none: the ZYBO's, the I/O PLL's
 * 1000 MHz divided by 20.
 *
 * Value referenced: U-Boot v2025.07
 * board/xilinx/zynq/zynq-zybo/ps7_init_gpl.c (0xf8000150, SDIO_CLK_CTRL:
 * DIVISOR 0x14, SRCSEL 0, the I/O PLL).
 */
#define ZYNQ_SDIO_FALLBACK_HZ		50000000ULL

/*
 * The present state register and its card inserted bit.
 *
 * Values referenced: SD Host Controller Simplified Specification 4.20,
 * the Present State register (offset 0x24, Card Inserted in bit 16).
 */
#define ZYNQ_SDHCI_PRESENT_STATE	0x24U
#define ZYNQ_SDHCI_CARD_INSERTED	0x00010000U

static int switch_clocks_on(void);
static uint64_t base_clock_rate(void);
static int card_present(void *context, int *present);

/*
 * Brings up the card in the ZYBO's SD slot and publishes it as mmcblk0.
 *
 * Returns 0 with the disk, or the errno of the step that failed: the SLCR
 * or the controller could not be mapped, or the card did not come up (an
 * empty slot among the reasons).
 */
int
drv_zynq_sdhci_init(
	struct disk **disk)
{
	struct drv_sdhci_config config;
	void *mapped;
	uint64_t base_clock;
	int error;

	/* Refuses nowhere to put the disk. */
	if (disk == NULL)
		return EINVAL;

	/* Starts without a disk, which every failure below leaves. */
	*disk = NULL;

	/* Maps the SLCR the controller's clocks are read and set in. */
	error = drv_zynq_slcr_init();
	if (error != 0) {
		kern_logf("zynq-sdhci: SLCR unavailable (%d)\n", error);
		return error;
	}

	/* Switches on a clock of the controller the first stage loader left off. */
	error = switch_clocks_on();
	if (error != 0) {
		kern_logf("zynq-sdhci: clocks unavailable (%d)\n", error);
		return error;
	}

	/* Takes the controller's base clock from the clock tree. */
	base_clock = base_clock_rate();

	/* Maps the controller's registers as device memory. */
	error = kern_device_map(ZYNQ_SDIO0_PHYS, ZYNQ_SDIO0_BYTES, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0) {
		kern_logf("zynq-sdhci: registers unavailable (%d)\n", error);
		return error;
	}

	/* Describes the controller to the SDHCI driver. */
	config.registers = mapped;
	config.physical_base = ZYNQ_SDIO0_PHYS;
	config.base_clock_hz = base_clock;
	config.max_clock_hz = 0;
	config.quirks = 0;
	config.card_present = card_present;
	config.context = mapped;
	config.unit = ZYNQ_SDIO0_UNIT;

	/* Brings the card up and publishes it. */
	error = drv_sdhci_attach(&config, disk);
	if (error != 0)
		return error;

	/* Succeeded: the card is the disk mmcblk0. */
	return 0;
}

/*
 * Switches SDIO0's bus clock and reference clock on when either is off.
 *
 * On the board the first stage loader started both, and the emulator's
 * reset values have both on; a write is made only for a clock found off.
 */
static int
switch_clocks_on(
	void)
{
	uint32_t gates;
	uint32_t control;
	int error;

	/* Reads the bus clock gates. */
	error = drv_zynq_slcr_read(DRV_ZYNQ_SLCR_APER_CLK_CTRL, &gates);
	if (error != 0)
		return error;

	/* Opens SDIO0's bus clock gate when it is closed. */
	if ((gates & ZYNQ_APER_SDIO0) == 0) {
		error = drv_zynq_slcr_modify(DRV_ZYNQ_SLCR_APER_CLK_CTRL, ZYNQ_APER_SDIO0, ZYNQ_APER_SDIO0);
		if (error != 0)
			return error;

		/* Writes the change to the boot log. */
		kern_logf("zynq-sdhci: switched the bus clock on\n");
	}

	/* Reads the reference clock's control. */
	error = drv_zynq_slcr_read(DRV_ZYNQ_SLCR_SDIO_CLK_CTRL, &control);
	if (error != 0)
		return error;

	/* Starts SDIO0's reference clock when it is stopped. */
	if ((control & ZYNQ_SDIO_CLKACT0) == 0) {
		error = drv_zynq_slcr_modify(DRV_ZYNQ_SLCR_SDIO_CLK_CTRL, ZYNQ_SDIO_CLKACT0, ZYNQ_SDIO_CLKACT0);
		if (error != 0)
			return error;

		/* Writes the change to the boot log. */
		kern_logf("zynq-sdhci: switched the reference clock on\n");
	}

	/* Succeeded: both clocks run. */
	return 0;
}

/*
 * Computes the controller's base clock: the PLL SDIO_CLK_CTRL selects,
 * divided by its divider.
 *
 * A clock tree that gives no rate (a stopped PLL, a zero divider) leaves
 * the ZYBO's 50 MHz, with a warning in the boot log.
 */
static uint64_t
base_clock_rate(
	void)
{
	uint64_t parent;
	uint64_t rate;
	uint32_t control;
	uint32_t source;
	uint32_t divisor;
	int error;

	/* Reads the reference clock's control; without it the rate is the ZYBO's. */
	error = drv_zynq_slcr_read(DRV_ZYNQ_SLCR_SDIO_CLK_CTRL, &control);
	if (error != 0) {
		kern_logf("zynq-sdhci: base clock unknown, taking %llu Hz\n",
			  (unsigned long long)ZYNQ_SDIO_FALLBACK_HZ);
		return ZYNQ_SDIO_FALLBACK_HZ;
	}

	/* Finds the PLL the reference clock comes from. */
	source = (control >> ZYNQ_SDIO_SRCSEL_SHIFT) & ZYNQ_SDIO_SRCSEL_MASK;
	if (source == ZYNQ_SDIO_SRCSEL_ARM) {
		parent = drv_zynq_slcr_pll_hz(DRV_ZYNQ_SLCR_ARM_PLL_CTRL);
	} else if (source == ZYNQ_SDIO_SRCSEL_DDR) {
		parent = drv_zynq_slcr_pll_hz(DRV_ZYNQ_SLCR_DDR_PLL_CTRL);
	} else {
		parent = drv_zynq_slcr_pll_hz(DRV_ZYNQ_SLCR_IO_PLL_CTRL);
	}

	/* Refuses a rate the clock tree does not give. */
	divisor = (control >> ZYNQ_SDIO_DIVISOR_SHIFT) & ZYNQ_SDIO_DIVISOR_MASK;
	if (parent == 0 || divisor == 0) {
		kern_logf("zynq-sdhci: base clock unknown (control %x), taking %llu Hz\n",
			  control,
			  (unsigned long long)ZYNQ_SDIO_FALLBACK_HZ);
		return ZYNQ_SDIO_FALLBACK_HZ;
	}

	/* Writes the rate the card clock will be divided from to the boot log. */
	rate = parent / divisor;
	kern_logf("zynq-sdhci: base clock %llu Hz (pll %llu Hz / %u)\n",
		  (unsigned long long)rate,
		  (unsigned long long)parent,
		  (unsigned)divisor);

	/* Succeeded: the base clock's rate. */
	return rate;
}

/*
 * Reports whether the controller sees a card in the slot.
 *
 * The context is the controller's mapped registers.
 */
static int
card_present(
	void *context,
	int *present)
{
	volatile uint8_t *registers;
	uint32_t state;

	/* Reads the present state. */
	registers = context;
	state = kern_mmio_read32(registers + ZYNQ_SDHCI_PRESENT_STATE);

	/* A card is in when the card inserted bit is set. */
	*present = 0;
	if ((state & ZYNQ_SDHCI_CARD_INSERTED) != 0)
		*present = 1;

	/* Succeeded: the answer is in present. */
	return 0;
}
