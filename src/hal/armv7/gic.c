/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GIC of the armv7 boards: its distributor, which routes the shared
 * lines, and each processor's banked private lines and CPU interface.
 *
 * Every line is put in Group 0, and the CPU interface signals both groups
 * as IRQ (FIQEn stays clear) and acknowledges a Group 1 interrupt too
 * (AckCtl), so a line the GIC kept in Group 1 is still served.  On the
 * ZYBO the processor runs in the secure state under JTAG; on the
 * emulator's virt board it is non-secure and the GIC has no security
 * extensions, so every register written here is the processor's to write.
 * Where a non-secure processor meets a GIC with security extensions the
 * group registers ignore the writes, which is harmless.
 *
 * Values referenced: ARM IHI 0048B (GIC architecture version 1 and 2), the
 * distributor's GICD_CTLR, GICD_TYPER, GICD_IGROUPR, GICD_ISENABLER,
 * GICD_ICENABLER, GICD_ICPENDR, GICD_IPRIORITYR, GICD_ITARGETSR, GICD_ICFGR
 * and GICD_SGIR, and the CPU interface's GICC_CTLR (EnableGrp0, EnableGrp1,
 * AckCtl, FIQEn), GICC_PMR, GICC_IAR and GICC_EOIR.
 */

#include <hal/hal.h>
#include "bootinfo.h"
#include "bsp.h"
#include "defs.h"
#include "gic.h"
#include "pte.h"
#include "space.h"
#include "spin.h"

/* The distributor's registers. */
#define GICD_CTLR		0x000U
#define GICD_TYPER		0x004U
#define GICD_IGROUPR		0x080U
#define GICD_ISENABLER		0x100U
#define GICD_ICENABLER		0x180U
#define GICD_ICPENDR		0x280U
#define GICD_IPRIORITYR		0x400U
#define GICD_ITARGETSR		0x800U
#define GICD_ICFGR		0xc00U
#define GICD_SGIR		0xf00U

/* The CPU interface's registers. */
#define GICC_CTLR		0x000U
#define GICC_PMR		0x004U
#define GICC_IAR		0x00cU
#define GICC_EOIR		0x010U

/*
 * The enable bits of both groups, in GICD_CTLR and GICC_CTLR, and the
 * CPU interface's acknowledgement of Group 1.
 */
#define GIC_ENABLE_GROUP0	0x1U
#define GIC_ENABLE_GROUP1	0x2U
#define GIC_ACK_CONTROL		0x4U

/* The bytes of the two blocks the HAL maps. */
#define GIC_DISTRIBUTOR_BYTES	0x1000U
#define GIC_CPU_INTERFACE_BYTES	0x1000U

/* The one priority every line gets, and the mask that lets every priority through. */
#define GIC_PRIORITY		0xa0U
#define GIC_PRIORITY_MASK_ALL	0xf0U

/* The largest number of lines a GIC has. */
#define GIC_LINES_MAX		1020U

/*
 * The distributor and the CPU interface through the device window, the
 * number of lines the distributor has, and whether the board says the
 * processor is secure (for the boot log).  Set once by armv7_gic_init()
 * on the boot processor before any line is used; NULL and zero before.
 */
static volatile uint8_t *distributor;
static volatile uint8_t *cpu_interface;
static uint32_t line_count;
static int gic_secure;

/*
 * Guards the read-modify-write of the distributor's shared configuration
 * words, which processors may change at once.  Zero when free.
 */
static volatile unsigned distributor_lock;

static uint32_t read_distributor(uint32_t offset);
static void write_distributor(uint32_t offset, uint32_t value);
static void write_distributor_byte(uint32_t offset, uint8_t value);
static void write_cpu_interface(uint32_t offset, uint32_t value);

/*
 * Finds the GIC, maps it and sets up its distributor and the boot
 * processor's part of it.
 *
 * Every shared line starts disabled, not pending, at one priority, level
 * triggered and routed to the boot processor's interface.
 */
void
armv7_gic_init(
	void)
{
	uint64_t distributor_physical;
	uint64_t cpu_interface_physical;
	void *mapped;
	uint32_t index;
	int error;

	/* Asks the board where the two blocks are and whether the processor is secure. */
	error = bsp_gic_bases(armv7_boot_fdt(), &distributor_physical, &cpu_interface_physical, &gic_secure);
	if (error != HAL_OK)
		HAL_FATAL("armv7 GIC not found");

	/* Maps both blocks into the device window. */
	error = armv7_space_map_device_window(distributor_physical, GIC_DISTRIBUTOR_BYTES, ARMV7_PTE_ATTR_DEVICE, &mapped);
	if (error != HAL_OK)
		HAL_FATAL("armv7 GIC distributor cannot be mapped");
	distributor = mapped;
	error = armv7_space_map_device_window(cpu_interface_physical, GIC_CPU_INTERFACE_BYTES, ARMV7_PTE_ATTR_DEVICE, &mapped);
	if (error != HAL_OK)
		HAL_FATAL("armv7 GIC CPU interface cannot be mapped");
	cpu_interface = mapped;

	/* Stops the distributor and reads how many lines it has. */
	write_distributor(GICD_CTLR, 0);
	line_count = ((read_distributor(GICD_TYPER) & 0x1fU) + 1U) * 32U;
	if (line_count > GIC_LINES_MAX)
		line_count = GIC_LINES_MAX;

	/* Disables every shared line, drops what is pending, and puts it in Group 0. */
	for (index = 1; index < (line_count + 31U) / 32U; index++) {
		write_distributor(GICD_ICENABLER + index * 4U, 0xffffffffU);
		write_distributor(GICD_ICPENDR + index * 4U, 0xffffffffU);
		write_distributor(GICD_IGROUPR + index * 4U, 0);
	}

	/* Gives every shared line the same priority and routes it to the boot processor. */
	for (index = 32U; index < line_count; index++) {
		write_distributor_byte(GICD_IPRIORITYR + index, GIC_PRIORITY);
		write_distributor_byte(GICD_ITARGETSR + index, 1);
	}

	/* Makes every shared line level triggered. */
	for (index = 2U; index < (line_count + 15U) / 16U; index++)
		write_distributor(GICD_ICFGR + index * 4U, 0);

	/* The boot processor's private lines and CPU interface, as every other processor's. */
	armv7_gic_init_cpu();

	/* Starts the distributor for both groups. */
	write_distributor(GICD_CTLR, GIC_ENABLE_GROUP0 | GIC_ENABLE_GROUP1);
	hal_io_mb();
	hal_printf("ARMV7 GIC distributor=%llx cpu=%llx secure=%u\n",
		   distributor_physical,
		   cpu_interface_physical,
		   (uint32_t)gic_secure);
}

/*
 * Sets up the calling processor's banked part of the GIC: its private
 * lines (SGI 0-15, PPI 16-31) and its CPU interface.
 */
void
armv7_gic_init_cpu(
	void)
{
	uint32_t line;

	/* Starts with every private line disabled, nothing pending, all in Group 0. */
	write_distributor(GICD_ICENABLER, 0xffffffffU);
	write_distributor(GICD_ICPENDR, 0xffffffffU);
	write_distributor(GICD_IGROUPR, 0);

	/* Gives the private lines the same priority as the shared ones. */
	for (line = 0; line < 32U; line++)
		write_distributor_byte(GICD_IPRIORITYR + line, GIC_PRIORITY);

	/* Enables the HAL's SGIs; a GIC that keeps SGIs enabled ignores it. */
	write_distributor(GICD_ISENABLER, (1U << ARMV7_SGI_STOP) | (1U << ARMV7_SGI_NOTIFY));

	/* Lets every priority through and enables the interface, both groups as IRQ. */
	write_cpu_interface(GICC_PMR, GIC_PRIORITY_MASK_ALL);
	write_cpu_interface(GICC_CTLR, GIC_ENABLE_GROUP0 | GIC_ENABLE_GROUP1 | GIC_ACK_CONTROL);
	hal_io_mb();
}

/*
 * Takes the highest pending interrupt at the calling processor's
 * interface.
 */
uint32_t
armv7_gic_ack(
	void)
{
	uint32_t acknowledge;

	/* Reading the acknowledge register makes the interrupt active. */
	acknowledge = hal_mmio_read32(cpu_interface + GICC_IAR);

	/* Succeeded: the interrupt's number and, for an SGI, its sender. */
	return acknowledge;
}

/*
 * Ends an interrupt the calling processor took.
 */
void
armv7_gic_eoi(
	uint32_t value)
{
	/* Writes back what the acknowledge read gave. */
	write_cpu_interface(GICC_EOIR, value);
}

/*
 * Reports how many lines the distributor has.
 */
uint32_t
armv7_gic_line_count(
	void)
{
	/* Succeeded: the count read at boot, or zero before. */
	return line_count;
}

/*
 * Disables one interrupt line.
 */
void
armv7_gic_mask(
	uint32_t intid)
{
	/* A line the distributor does not have is ignored. */
	if (intid >= line_count)
		return;

	/* The clear-enable register needs no lock. */
	write_distributor(GICD_ICENABLER + (intid / 32U) * 4U, 1U << (intid % 32U));
}

/*
 * Enables one interrupt line.
 */
void
armv7_gic_unmask(
	uint32_t intid)
{
	/* A line the distributor does not have is ignored. */
	if (intid >= line_count)
		return;

	/* The set-enable register needs no lock. */
	write_distributor(GICD_ISENABLER + (intid / 32U) * 4U, 1U << (intid % 32U));
}

/*
 * Configures the trigger mode of one disabled interrupt.
 *
 * A private line's mode is fixed by the hardware and is accepted only as
 * it already is.
 */
int
armv7_gic_set_trigger(
	uint32_t intid,
	int trigger)
{
	uint32_t offset;
	uint32_t mask;
	uint32_t configuration;
	uint32_t requested;
	uint32_t observed;
	uint32_t line_enabled;
	bool enabled;

	/* Requires an implemented interrupt and one of the HAL's two modes. */
	if (distributor == NULL || intid >= line_count)
		return HAL_ERR_INVALID;
	if (trigger != HAL_IRQ_TRIGGER_EDGE && trigger != HAL_IRQ_TRIGGER_LEVEL)
		return HAL_ERR_INVALID;

	/* Names the line's edge bit in its configuration word. */
	offset = GICD_ICFGR + (intid / 16U) * 4U;
	mask = 1U << ((intid % 16U) * 2U + 1U);
	requested = 0;
	if (trigger == HAL_IRQ_TRIGGER_EDGE)
		requested = mask;

	/* Changes the word with the line disabled, against other processors' changes. */
	enabled = armv7_spin_lock(&distributor_lock);

	configuration = read_distributor(offset);

	/* Refuses a line that is enabled: its mode may change only while it is off. */
	line_enabled = read_distributor(GICD_ISENABLER + (intid / 32U) * 4U) & (1U << (intid % 32U));
	if (line_enabled != 0) {
		armv7_spin_unlock(&distributor_lock, enabled);
		return HAL_ERR_BUSY;
	}

	/* A private line is not reprogrammed. */
	if (intid < 32U) {
		armv7_spin_unlock(&distributor_lock, enabled);

		/* Refuses a private line whose mode differs. */
		if ((configuration & mask) != requested)
			return HAL_ERR_UNSUPPORTED;

		/* Succeeded: the private line already has the mode. */
		return HAL_OK;
	}

	/* Writes the shared line's mode and reads it back. */
	write_distributor(offset, (configuration & ~mask) | requested);
	observed = read_distributor(offset) & mask;

	armv7_spin_unlock(&distributor_lock, enabled);

	/* Refuses a controller that did not take the mode. */
	if (observed != requested)
		return HAL_ERR_UNSUPPORTED;

	/* Succeeded: the next unmask uses the mode. */
	return HAL_OK;
}

/*
 * Sends a software-generated interrupt to the CPU interfaces of a mask.
 *
 * The barrier first makes the sender's writes visible to the targets
 * before they take the interrupt.
 */
void
armv7_gic_send_sgi(
	uint32_t sgi,
	uint32_t target_interfaces)
{
	uint32_t request;

	/* Publishes the sender's memory writes to the targets. */
	__asm__ volatile("dsb ish" : : : "memory");

	/* Names the targets by CPU interface (bits 16-23) and the SGI (bits 0-3). */
	request = ((target_interfaces & 0xffU) << 16) | (sgi & 0xfU);
	write_distributor(GICD_SGIR, request);
}

/* Reads one word of the distributor. */
static uint32_t
read_distributor(
	uint32_t offset)
{
	uint32_t value;

	/* The register at its offset in the window. */
	value = hal_mmio_read32(distributor + offset);

	/* Succeeded: the register's value. */
	return value;
}

/* Writes one word of the distributor. */
static void
write_distributor(
	uint32_t offset,
	uint32_t value)
{
	/* The register at its offset in the window. */
	hal_mmio_write32(distributor + offset, value);
}

/* Writes one byte of a byte-wide distributor register array. */
static void
write_distributor_byte(
	uint32_t offset,
	uint8_t value)
{
	/* The array is addressed by byte, one byte per interrupt line. */
	hal_mmio_write8(distributor + offset, value);
}

/* Writes one word of the CPU interface. */
static void
write_cpu_interface(
	uint32_t offset,
	uint32_t value)
{
	/* The register at its offset in the window. */
	hal_mmio_write32(cpu_interface + offset, value);
}
