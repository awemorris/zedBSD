/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GIC-400 of the Raspberry Pi 4 and 5: its distributor, which routes
 * the shared lines, and each CPU's banked private lines and CPU interface.
 */

#include <hal/hal.h>
#include "../bsp.h"
#include "../defs.h"
#include "../spin.h"
#include "gic.h"

/* The distributor's registers. */
#define GICD_CTLR 0x000
#define GICD_TYPER 0x004
#define GICD_IGROUPR 0x080
#define GICD_ISENABLER 0x100
#define GICD_ICENABLER 0x180
#define GICD_ICPENDR 0x280
#define GICD_IPRIORITYR 0x400
#define GICD_ITARGETSR 0x800
#define GICD_ICFGR 0xc00
#define GICD_SGIR 0xf00

/* The CPU interface's registers. */
#define GICC_CTLR 0x000
#define GICC_PMR 0x004
#define GICC_IAR 0x00c
#define GICC_EOIR 0x010

/* The one priority every line gets. */
#define GIC_PRIORITY 0xa0

/* The private interrupt of each CPU's physical timer. */
#define GIC_TIMER_PPI 30U

/*
 * The distributor and the CPU interface through the direct map, and the
 * number of lines the distributor has.  Set once by rpi_gic_init() on the
 * boot CPU before any line is used; NULL and zero before.
 */
static volatile uint32_t *dist;
static volatile uint32_t *cpuif;
static uint32_t irq_count;

/*
 * Guards the read-modify-write of the distributor's shared group and
 * configuration words, which CPUs may change at once.  Zero when free.
 */
static volatile unsigned distributor_lock;

static void write8(volatile uint32_t *base, unsigned offset, uint8_t value);

/*
 * Sets up the GIC-400's distributor and the boot CPU's part of it.
 *
 * Every shared line starts disabled, not pending, at one priority, level
 * triggered and routed to CPU interface 0, the boot CPU (ws208 D1: the
 * device interrupts stay on the boot CPU).
 */
void
rpi_gic_init(
	void)
{
	const struct rpi_fdt_info *info;
	uint32_t lines;
	uint32_t index;

	/* Finds the distributor and the CPU interface in the device tree. */
	info = rpi_boot_info();
	if (info->gic_dist_base == 0 || info->gic_cpu_base == 0)
		HAL_FATAL("GIC missing from FDT");
	dist = (volatile uint32_t *)(ARM64_DIRECT_BASE + (uintptr_t)info->gic_dist_base);
	cpuif = (volatile uint32_t *)(ARM64_DIRECT_BASE + (uintptr_t)info->gic_cpu_base);

	/* Stops the distributor and reads how many lines it has, up to 1020. */
	dist[GICD_CTLR / 4] = 0;
	lines = ((dist[GICD_TYPER / 4] & 0x1fU) + 1U) * 32U;
	if (lines > 1020U)
		lines = 1020U;
	irq_count = lines;

	/* Disables every line, drops what is pending, and puts it in Group 1. */
	for (index = 0; index < (lines + 31U) / 32U; index++) {
		dist[GICD_ICENABLER / 4 + index] = 0xffffffffU;
		dist[GICD_ICPENDR / 4 + index] = 0xffffffffU;
		dist[GICD_IGROUPR / 4 + index] = 0xffffffffU;
	}

	/* Gives every line the same priority. */
	for (index = 0; index < lines; index++)
		write8(dist, GICD_IPRIORITYR + index, GIC_PRIORITY);

	/* Routes every shared line to the boot CPU's interface. */
	for (index = 32U; index < lines; index++)
		write8(dist, GICD_ITARGETSR + index, 1);

	/* Makes every shared line level triggered. */
	for (index = 2U; index < (lines + 15U) / 16U; index++)
		dist[GICD_ICFGR / 4 + index] = 0;

	/* The boot CPU's private lines and CPU interface, as every other CPU's. */
	rpi_gic_init_cpu();

	/* Starts the distributor for both groups. */
	dist[GICD_CTLR / 4] = 3;
	hal_io_mb();
}

/*
 * Sets up the calling CPU's banked part of the GIC: its private lines
 * (SGI 0-15, PPI 16-31) and its CPU interface.
 *
 * The timer's PPI and the HAL's two SGIs are put in Group 0, which the
 * primary IAR serves when the CPU runs in the secure state (QEMU's direct
 * boot).  Under the firmware the CPU is non-secure, the group writes are
 * ignored and the primary IAR serves Group 1, where the firmware put them.
 */
void
rpi_gic_init_cpu(
	void)
{
	uint32_t groups;
	uint32_t line;

	/* Starts with every private line disabled and nothing pending. */
	dist[GICD_ICENABLER / 4] = 0xffffffffU;
	dist[GICD_ICPENDR / 4] = 0xffffffffU;

	/* Groups the private lines, the HAL's SGIs and the timer in Group 0. */
	groups = 0xffffffffU;
	groups &= ~(1U << GIC_TIMER_PPI);
	groups &= ~(1U << RPI_SGI_STOP);
	groups &= ~(1U << RPI_SGI_NOTIFY);
	dist[GICD_IGROUPR / 4] = groups;

	/* Gives the private lines the same priority as the shared ones. */
	for (line = 0; line < 32U; line++)
		write8(dist, GICD_IPRIORITYR + line, GIC_PRIORITY);

	/* Enables the HAL's SGIs; the GIC-400 keeps SGIs enabled in any case. */
	dist[GICD_ISENABLER / 4] = (1U << RPI_SGI_STOP) | (1U << RPI_SGI_NOTIFY);

	/* Lets every priority through and enables both groups at the interface. */
	cpuif[GICC_PMR / 4] = 0xff;
	cpuif[GICC_CTLR / 4] = 3;
	hal_io_mb();
}

/*
 * Sends a software-generated interrupt to the CPU interfaces of a mask.
 *
 * The barrier first makes the sender's writes visible to the targets
 * before they take the interrupt.
 */
void
rpi_gic_send_sgi(
	uint32_t sgi,
	uint32_t target_interfaces)
{
	uint32_t request;

	/* Publishes the sender's memory writes to the targets. */
	__asm__ volatile("dsb ish" : : : "memory");

	/* Names the targets by CPU interface (bits 16-23) and the SGI (bits 0-3). */
	request = ((target_interfaces & 0xffU) << 16) | (sgi & 0xfU);
	dist[GICD_SGIR / 4] = request;
	hal_io_mb();
}

/*
 * Takes the highest pending interrupt at the calling CPU's interface.
 */
uint32_t
rpi_gic_ack(
	void)
{
	uint32_t acknowledge;

	/* Reading the acknowledge register makes the interrupt active. */
	acknowledge = cpuif[GICC_IAR / 4];

	/* Succeeded: the interrupt's number and, for an SGI, its sender. */
	return acknowledge;
}

/*
 * Ends an interrupt the calling CPU took.
 */
void
rpi_gic_eoi(
	uint32_t value)
{
	/* Writes back what the acknowledge read gave. */
	cpuif[GICC_EOIR / 4] = value;
	hal_io_mb();
}

/*
 * Disables one interrupt line.
 */
void
rpi_gic_mask(
	uint32_t id)
{
	/* A line the distributor does not have is ignored. */
	if (id >= irq_count)
		return;

	/* The clear-enable register needs no lock. */
	dist[GICD_ICENABLER / 4 + id / 32] = 1U << (id & 31);
}

/*
 * Enables one interrupt line, in the group the primary IAR serves.
 *
 * Direct QEMU boot enters secure EL1, where the primary IAR serves Group 0.
 * Firmware normally enters non-secure EL1; there the group write is
 * ignored and the primary IAR aliases Group 1.
 */
void
rpi_gic_unmask(
	uint32_t id)
{
	bool enabled;

	/* A line the distributor does not have is ignored. */
	if (id >= irq_count)
		return;

	/* Moves the line to Group 0 without losing another CPU's change to the word. */
	enabled = arm64_spin_lock(&distributor_lock);

	dist[GICD_IGROUPR / 4 + id / 32] &= ~(1U << (id & 31));

	arm64_spin_unlock(&distributor_lock, enabled);

	/* Enables the line; the set-enable register needs no lock. */
	dist[GICD_ISENABLER / 4 + id / 32] = 1U << (id & 31);
}

/*
 * Configures the trigger mode of one disabled interrupt.
 */
int
rpi_gic_set_trigger(
	uint32_t id,
	int trigger)
{
	uint32_t offset;
	uint32_t mask;
	uint32_t configuration;
	uint32_t requested;
	uint32_t observed;
	uint32_t line_enabled;
	bool enabled;

	/* Requires an implemented interrupt. */
	if (dist == NULL || id >= irq_count)
		return HAL_ERR_INVALID;

	/* Refuses an encoding outside the HAL's two trigger modes. */
	if (trigger != HAL_IRQ_TRIGGER_EDGE && trigger != HAL_IRQ_TRIGGER_LEVEL)
		return HAL_ERR_INVALID;

	/* Changes only the trigger bit, preserving the neighboring interrupts. */
	offset = GICD_ICFGR / 4U + id / 16U;
	mask = 1U << ((id % 16U) * 2U + 1U);
	requested = 0;
	if (trigger == HAL_IRQ_TRIGGER_EDGE)
		requested = mask;

	/* Serializes distributor updates while the requested line stays disabled. */
	enabled = arm64_spin_lock(&distributor_lock);

	configuration = dist[offset];

	/* Refuses a line that is enabled: its trigger may change only while it is off. */
	line_enabled = dist[GICD_ISENABLER / 4U + id / 32U] & (1U << (id % 32U));
	if (line_enabled != 0) {
		arm64_spin_unlock(&distributor_lock, enabled);

		return HAL_ERR_BUSY;
	}

	/* Preserves fixed SGI/PPI configuration unless it already matches. */
	if (id < 32U) {
		arm64_spin_unlock(&distributor_lock, enabled);

		/* Refuses a private line that would need reprogramming. */
		if ((configuration & mask) != requested)
			return HAL_ERR_UNSUPPORTED;

		/* Succeeded: the private line already has the requested mode. */
		return HAL_OK;
	}

	/* Publishes the SPI trigger and detects a read-only distributor field. */
	dist[offset] = (configuration & ~mask) | requested;
	hal_io_mb();
	observed = dist[offset] & mask;

	arm64_spin_unlock(&distributor_lock, enabled);

	/* Refuses a controller that did not accept the requested trigger. */
	if (observed != requested)
		return HAL_ERR_UNSUPPORTED;

	/* Succeeded: the masked SPI is configured for the next unmask. */
	return HAL_OK;
}

/* Writes one byte of a byte-wide distributor register array. */
static void
write8(
	volatile uint32_t *base,
	unsigned offset,
	uint8_t value)
{
	volatile uint8_t *bytes;

	/* The array is addressed by byte, one byte per interrupt line. */
	bytes = (volatile uint8_t *)base;
	bytes[offset] = value;
}
