/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The platform-level interrupt controller (PLIC).
 *
 * Each source has a priority; each context (a hart in one privilege mode)
 * has an enable bit per source and a threshold, and takes a source only
 * when it is enabled there and its priority is above the threshold.  The
 * HAL routes every source to the boot hart's supervisor context, and masks
 * a source by its priority rather than its enable bit: the controller
 * drops the completion of a source that is not enabled, so a source masked
 * by its enable bit while its handler runs would never come again.
 */

#include <hal/hal.h>
#include "bootinfo.h"
#include "defs.h"
#include "percpu.h"
#include "plic.h"
#include "spin.h"

/*
 * The register layout: priorities from offset 0, the enable bits of
 * context C from 0x2000 + C * 0x80, and the threshold and claim register of
 * context C from 0x200000 + C * 0x1000.
 *
 * Values referenced: riscv/riscv-plic-spec (the memory map), QEMU v10.0.0
 * include/hw/riscv/virt.h (VIRT_PLIC_PRIORITY_BASE, VIRT_PLIC_ENABLE_BASE,
 * VIRT_PLIC_ENABLE_STRIDE, VIRT_PLIC_CONTEXT_BASE, VIRT_PLIC_CONTEXT_STRIDE).
 */
#define PLIC_PRIORITY_BASE	0x0U
#define PLIC_ENABLE_BASE	0x2000U
#define PLIC_ENABLE_STRIDE	0x80U
#define PLIC_CONTEXT_BASE	0x200000U
#define PLIC_CONTEXT_STRIDE	0x1000U
#define PLIC_THRESHOLD		0x0U
#define PLIC_CLAIM		0x4U

/* The highest source number the controller's layout can have. */
#define PLIC_SOURCE_LIMIT	1023U

/* The threshold that lets every source with a priority through, and the one that lets none. */
#define PLIC_THRESHOLD_OPEN	0U
#define PLIC_THRESHOLD_CLOSED	7U

/*
 * The controller's registers through the device window, its number of
 * sources and the boot hart's supervisor context, which every source is
 * routed to.  Set by riscv32_plic_init() on the boot hart before any
 * other hart starts, and constant afterwards.
 */
static volatile uint8_t *plic_registers;
static uint32_t plic_sources;
static unsigned plic_boot_context;

/*
 * Guards the enable words, which several sources share; zero when free.
 * The priorities are single words and need no lock.
 */
static volatile unsigned plic_enable_lock;

static void open_context(unsigned context, uint32_t threshold);
static int find_context(uint64_t hart_id, unsigned *context);

/*
 * Sets the controller up on the boot hart: every source off, and the boot
 * hart's context open and empty.
 */
void
riscv32_plic_init(
	void)
{
	const struct riscv32_fdt_info *info;
	struct riscv32_percpu *cpu;
	uint32_t source;
	int error;

	/* The controller the device tree names; the HAL cannot run without one. */
	info = riscv32_boot_fdt();
	if (info->plic_size == 0)
		HAL_FATAL("riscv32 device tree names no PLIC");
	plic_registers = (volatile uint8_t *)(RISCV32_DEVICE_BASE + info->plic_base);
	plic_sources = info->plic_sources;
	if (plic_sources > PLIC_SOURCE_LIMIT)
		plic_sources = PLIC_SOURCE_LIMIT;

	/* Finds the boot hart's supervisor context. */
	cpu = riscv32_percpu_current();
	error = find_context(cpu->hart_id, &plic_boot_context);
	if (error != 0)
		HAL_FATAL("riscv32 PLIC has no supervisor context for the boot hart");
	cpu->plic_context = plic_boot_context;
	cpu->has_plic_context = 1;

	/* Turns every source off: priority zero is never taken. */
	for (source = 1; source <= plic_sources; source++)
		hal_mmio_write32(plic_registers + PLIC_PRIORITY_BASE + source * 4U, 0);

	/* Opens the boot hart's context with nothing enabled. */
	open_context(plic_boot_context, PLIC_THRESHOLD_OPEN);
	hal_printf("RISCV32 PLIC sources=%u context=%u\n", plic_sources, plic_boot_context);
}

/*
 * Sets a secondary hart's context up: nothing enabled and the threshold
 * closed, since every source goes to the boot hart.
 */
void
riscv32_plic_init_hart(
	void)
{
	struct riscv32_percpu *cpu;
	unsigned context;
	int error;

	/* A hart without a supervisor context has nothing to set up. */
	cpu = riscv32_percpu_current();
	error = find_context(cpu->hart_id, &context);
	if (error != 0)
		return;
	cpu->plic_context = context;
	cpu->has_plic_context = 1;

	/* Closes the context. */
	open_context(context, PLIC_THRESHOLD_CLOSED);
}

/*
 * Reports how many sources the controller has; they are numbered from 1.
 */
uint32_t
riscv32_plic_sources(
	void)
{
	/* Succeeded: the number fixed at boot. */
	return plic_sources;
}

/*
 * Sets a source's priority; zero masks it.
 */
void
riscv32_plic_set_priority(
	uint32_t source,
	uint32_t priority)
{
	/* One word per source. */
	hal_mmio_write32(plic_registers + PLIC_PRIORITY_BASE + source * 4U, priority);
}

/*
 * Enables or disables a source at the boot hart's context.
 */
void
riscv32_plic_enable(
	uint32_t source,
	int enable)
{
	volatile uint8_t *word;
	uint32_t bits;
	bool enabled;

	/* The word and the bit of the source in the context's enable block. */
	word = plic_registers + PLIC_ENABLE_BASE + plic_boot_context * PLIC_ENABLE_STRIDE + (source / 32U) * 4U;

	/* Changes the one bit, the word being shared with 31 other sources. */
	enabled = riscv32_spin_lock(&plic_enable_lock);

	bits = hal_mmio_read32(word);
	if (enable) {
		bits |= 1U << (source % 32U);
	} else {
		bits &= ~(1U << (source % 32U));
	}

	/* Writes the context's enable word back. */
	hal_mmio_write32(word, bits);

	riscv32_spin_unlock(&plic_enable_lock, enabled);
}

/*
 * Claims the highest-priority pending source at the boot hart's context;
 * 0 when none is pending.
 */
uint32_t
riscv32_plic_claim(
	void)
{
	uint32_t source;

	/* Reading the claim register takes the source. */
	source = hal_mmio_read32(plic_registers + PLIC_CONTEXT_BASE + plic_boot_context * PLIC_CONTEXT_STRIDE + PLIC_CLAIM);

	/* Succeeded: the source taken, or 0. */
	return source;
}

/*
 * Ends a claimed source, which lets it raise the interrupt again.
 */
void
riscv32_plic_complete(
	uint32_t source)
{
	/* Writing the source to the claim register completes it. */
	hal_mmio_write32(plic_registers + PLIC_CONTEXT_BASE + plic_boot_context * PLIC_CONTEXT_STRIDE + PLIC_CLAIM, source);
}

/*
 * Empties a context: no source enabled, the threshold set, and anything
 * already claimed completed.
 */
static void
open_context(
	unsigned context,
	uint32_t threshold)
{
	volatile uint8_t *enables;
	volatile uint8_t *control;
	uint32_t word;
	uint32_t source;
	unsigned drained;

	/* Clears every enable word of the context. */
	enables = plic_registers + PLIC_ENABLE_BASE + context * PLIC_ENABLE_STRIDE;
	for (word = 0; word <= plic_sources / 32U; word++)
		hal_mmio_write32(enables + word * 4U, 0);

	/* Sets the threshold. */
	control = plic_registers + PLIC_CONTEXT_BASE + context * PLIC_CONTEXT_STRIDE;
	hal_mmio_write32(control + PLIC_THRESHOLD, threshold);

	/* Completes anything the firmware left claimed, so its source is not stuck. */
	for (drained = 0; drained <= plic_sources; drained++) {
		source = hal_mmio_read32(control + PLIC_CLAIM);
		if (source == 0)
			break;
		hal_mmio_write32(control + PLIC_CLAIM, source);
	}
}

/* Finds the supervisor context of a hart in the device tree. */
static int
find_context(
	uint64_t hart_id,
	unsigned *context)
{
	int error;

	/* The context whose entry names the hart's controller and its supervisor interrupt. */
	error = riscv32_fdt_s_context(riscv32_boot_fdt(), hart_id, context);
	if (error != 0)
		return -1;

	/* Succeeded: the context. */
	return 0;
}
