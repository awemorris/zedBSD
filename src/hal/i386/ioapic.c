/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shared i386 I/O-APIC implementation.
 */

#include <hal/hal.h>

#include "defs.h"
#include "ioapic.h"

struct controller {
	volatile uint32_t *base;
	uint8_t id;
	uint32_t gsi_base;
	uint32_t entries;
};

struct route {
	struct controller *io;
	uint8_t pin;
	uint8_t low;
	uint8_t level;
	uint8_t destination;
	uint8_t present;
};

static struct controller controllers[I386_IOAPIC_MAX];
static struct route routes[16];
static unsigned count;
static volatile unsigned lock;

static uint32_t ioapic_read(struct controller *controller, uint8_t reg);
static void ioapic_write(struct controller *controller, uint8_t reg, uint32_t value);
static struct controller *controller_by_id(uint8_t id);
static struct controller *controller_by_gsi(uint32_t gsi, unsigned *pin);
static void write_route(int irq, int masked);

/*
 * Initializes the discovered I/O-APIC controllers and legacy IRQ routes.
 */
int
i386_ioapic_init(
	const struct i386_apic_topology *topology,
	uint8_t destination)
{
	const struct i386_apic_route *source;
	struct controller *controller;
	struct route *route;
	unsigned i;
	unsigned pin;

	/* Requires at least one discovered I/O-APIC controller. */
	if (topology == NULL || topology->ioapic_count == 0U)
		return HAL_ERR_INVALID;

	/* Clears old routes and publishes the discovered controller count. */
	hal_memset(routes, 0, sizeof(routes));
	count = topology->ioapic_count;

	/* Maps each controller and reads its redirection-table size. */
	for (i = 0; i < count; i++) {
		controllers[i].base = (volatile uint32_t *)(uintptr_t)
		    topology->ioapics[i].address;
		controllers[i].id = topology->ioapics[i].apic_id;
		controllers[i].gsi_base = topology->ioapics[i].gsi_base;
		controllers[i].entries =
		    ((ioapic_read(&controllers[i], 1) >> 16) & 0xffU) + 1U;
	}

	/* Resolves and installs every firmware legacy-IRQ route. */
	for (i = 0; i < topology->route_count; i++) {
		source = &topology->routes[i];
		pin = source->ioapic_pin;

		/* Ignores firmware routes outside the legacy IRQ range. */
		if (source->source_irq >= 16U)
			continue;

		/* Resolves a global-system interrupt or an explicit APIC ID. */
		if (source->ioapic_id == 0xffU) {
			controller = controller_by_gsi(pin, &pin);
		} else {
			controller = controller_by_id(source->ioapic_id);
		}

		/* Rejects routes outside the resolved controller. */
		if (controller == NULL || pin >= controller->entries)
			return HAL_ERR_INVALID;

		/* Publishes and initially masks the resolved legacy route. */
		route = &routes[source->source_irq];
		route->io = controller;
		route->pin = (uint8_t)pin;
		route->low = source->polarity_low;
		route->level = source->level_triggered;
		route->destination = destination;
		route->present = 1;
		write_route((int)source->source_irq, 1);
	}

	/* Reports a configured I/O-APIC route set. */
	return HAL_OK;
}

/*
 * Masks one valid legacy I/O-APIC route.
 */
void
i386_ioapic_mask(
	int irq)
{
	/* Ignores IRQ numbers outside the legacy route table. */
	if (irq >= 0 && irq < 16)
		write_route(irq, 1);
}

/*
 * Unmasks one valid legacy I/O-APIC route.
 */
void
i386_ioapic_unmask(
	int irq)
{
	/* Ignores IRQ numbers outside the legacy route table. */
	if (irq >= 0 && irq < 16)
		write_route(irq, 0);
}

/*
 * Changes one I/O-APIC route destination while leaving it masked.
 */
int
i386_ioapic_route(
	int irq,
	uint8_t cpu)
{
	/* Requires a populated legacy route. */
	if (irq < 0 || irq >= 16 || !routes[irq].present)
		return HAL_ERR_INVALID;

	/* Publishes the new destination through a masked redirection entry. */
	routes[irq].destination = cpu;
	write_route(irq, 1);

	/* Reports a programmed route. */
	return HAL_OK;
}

/* Reads one indirect I/O-APIC register. */
static uint32_t
ioapic_read(
	struct controller *controller,
	uint8_t reg)
{
	uint32_t value;

	/* Selects the register before ordering and reading its data window. */
	controller->base[0] = reg;
	hal_io_mb();
	value = controller->base[4];

	/* Returns the selected register value. */
	return value;
}

/* Writes one indirect I/O-APIC register. */
static void
ioapic_write(
	struct controller *controller,
	uint8_t reg,
	uint32_t value)
{
	/* Selects the register before publishing its data value. */
	controller->base[0] = reg;
	hal_io_mb();
	controller->base[4] = value;
	hal_io_mb();
}

/* Finds a discovered controller by its firmware APIC identifier. */
static struct controller *
controller_by_id(
	uint8_t id)
{
	unsigned i;

	/* Searches every discovered controller in firmware order. */
	for (i = 0; i < count; i++) {
		/* Returns the first matching firmware APIC identifier. */
		if (controllers[i].id == id)
			return &controllers[i];
	}

	/* Reports that no controller owns the identifier. */
	return NULL;
}

/* Finds the controller and pin which own one global-system interrupt. */
static struct controller *
controller_by_gsi(
	uint32_t gsi,
	unsigned *pin)
{
	unsigned i;

	/* Searches each controller's contiguous global-interrupt range. */
	for (i = 0; i < count; i++) {
		/* Returns the controller and relative pin for a contained GSI. */
		if (gsi >= controllers[i].gsi_base &&
		    gsi - controllers[i].gsi_base < controllers[i].entries) {
			*pin = gsi - controllers[i].gsi_base;
			return &controllers[i];
		}
	}

	/* Reports that no controller owns the interrupt. */
	return NULL;
}

/* Programs one legacy I/O-APIC redirection entry. */
static void
write_route(
	int irq,
	int masked)
{
	struct route *route;
	uint32_t low;
	bool enabled;

	/* Selects the software route and ignores absent entries. */
	route = &routes[irq];
	if (!route->present)
		return;

	/* Encodes vector, polarity, trigger mode, and mask state. */
	low = (uint32_t)(0xe0 + irq);

	/* Applies active-low electrical polarity. */
	if (route->low)
		low |= 1U << 13;

	/* Applies level-triggered delivery. */
	if (route->level)
		low |= 1U << 15;

	/* Applies the requested mask state. */
	if (masked)
		low |= 1U << 16;

	/* Serializes the shared indirect register pair with IRQs disabled. */
	enabled = hal_irq_disable();
	while (__atomic_exchange_n(&lock, 1U, __ATOMIC_ACQUIRE) != 0U)
		__asm__ volatile("pause");

	/* Writes the destination before the low redirection word. */
	ioapic_write(
		route->io,
		(uint8_t)(0x11U + route->pin * 2U),
		(uint32_t)route->destination << 24);
	ioapic_write(
		route->io,
		(uint8_t)(0x10U + route->pin * 2U),
		low);

	/* Releases the serializer before restoring interrupt state. */
	__atomic_store_n(&lock, 0U, __ATOMIC_RELEASE);

	/* Restores interrupts only when they were previously enabled. */
	if (enabled)
		hal_irq_enable();
}
