/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Device tree, mapping and interrupt helpers of the BCM2711 graphics driver.
 *
 * The firmware's device tree describes the display path and V3D but marks
 * them disabled, because the firmware drives the display itself.  The driver
 * uses those nodes anyway (the firmware-written tree is the only description
 * of the board it has), so these helpers do not look at a node's status.
 */

#include <stdbool.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"

/* The cells of one GIC interrupt specifier: kind, number and trigger. */
#define FDT_GIC_CELLS			3U

/* The specifier kind of a shared peripheral interrupt. */
#define FDT_GIC_KIND_SPI		0U

/* The GIC interrupt ID of the first shared peripheral interrupt. */
#define GIC_FIRST_SPI_ID		32U

/* The deepest node the search for an interrupt parent climbs from. */
#define FDT_PARENT_DEPTH_LIMIT		32U

static void irq_handler(int irq, kern_irq_ack_t acknowledge, void *argument);
static int interrupt_parent(const struct drv_fdt *fdt, uint32_t node, uint32_t *controller);

/*
 * Finds the first node of a compatible string, enabled or not.
 */
int
bcm2711_fdt_find(
	const struct drv_fdt *fdt,
	const char *compatible,
	uint32_t *node)
{
	int error;

	/* Walks the tree from the root for the compatible string. */
	error = drv_fdt_find_compatible(fdt, compatible, DRV_FDT_NO_NODE, node);
	if (error != 0)
		return ENODEV;

	/* Succeeded: node names the device. */
	return 0;
}

/*
 * Reads one register window of a node, in the CPU's physical address space.
 */
int
bcm2711_fdt_window(
	const struct drv_fdt *fdt,
	uint32_t node,
	unsigned index,
	struct bcm2711_window *window)
{
	uint64_t physical;
	uint64_t size;
	int error;

	/* Reads and translates the index-th address and size pair. */
	error = drv_fdt_reg(fdt, node, index, &physical, &size);
	if (error != 0)
		return error;

	/* Refuses an empty window, which nothing can be mapped for. */
	if (size == 0)
		return EINVAL;

	/* Records the window; it is mapped separately. */
	window->physical = physical;
	window->size = size;
	window->mapped = NULL;

	/* Succeeded: the window describes the device's registers. */
	return 0;
}

/*
 * Resolves a bounded string-list member without assuming binding order.
 */
int
bcm2711_fdt_string_index(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *property,
	const char *name,
	uint32_t *index)
{
	const uint8_t *names;
	uint32_t length;
	uint32_t first;
	uint32_t end;
	uint32_t ordinal;
	int comparison;
	int error;

	/* Requires a complete binding string list before walking any member. */
	error = drv_fdt_property(fdt, node, property, &names, &length);
	if (error != 0)
		return error;

	/* Checks termination inside the property before comparing each string. */
	first = 0;
	ordinal = 0;
	while (first < length) {
		/* A truncated member cannot identify a provider or register role. */
		end = first;
		while (end < length && names[end] != 0)
			end++;
		if (end == length)
			return EINVAL;
		comparison = kern_strcmp((const char *)names + first, name);
		if (comparison == 0) {
			/* The ordinal is independent of the device's physical window ordering. */
			*index = ordinal;
			return 0;
		}

		/* Advances past one complete NUL-terminated member only. */
		first = end + 1U;
		ordinal++;
	}

	/* The requested role is absent from this binding property. */
	return ENOENT;
}

/*
 * Resolves one register role through reg-names and the native FDT translation.
 */
int
bcm2711_fdt_named_window(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	struct bcm2711_window *window)
{
	uint32_t index;
	int error;

	/* Finds the role without relying on either firmware's reg ordering. */
	error = bcm2711_fdt_string_index(fdt, node, "reg-names", name, &index);
	if (error != 0)
		return error;

	/* Translates only the corresponding bounded register entry. */
	error = bcm2711_fdt_window(fdt, node, index, window);
	if (error != 0)
		return error;

	/* Succeeded: the CPU window belongs to the requested binding role. */
	return 0;
}

/*
 * Counts the address and size pairs of a node's reg property.
 */
uint32_t
bcm2711_fdt_reg_count(
	const struct drv_fdt *fdt,
	uint32_t node)
{
	uint64_t physical;
	uint64_t size;
	uint32_t count;
	int error;

	/* Reads pairs until the property runs out. */
	count = 0;
	for (;;) {
		/* Stops at the first pair that is not there. */
		error = drv_fdt_reg(fdt, node, count, &physical, &size);
		if (error != 0)
			break;

		/* Includes only a complete address and size pair. */
		count++;
	}

	/* Reports the number of pairs that could be read. */
	return count;
}

/*
 * Reads the index-th interrupt of a node as a GIC interrupt ID.
 *
 * Only a shared peripheral interrupt of a three-cell GIC specifier is
 * accepted; a node whose interrupts go through another controller (the HDMI
 * encoders go through their own interrupt block) is refused with ENOTSUP.
 */
int
bcm2711_fdt_gic_irq(
	const struct drv_fdt *fdt,
	uint32_t node,
	unsigned index,
	int *irq)
{
	const uint8_t *value;
	const uint8_t *marker;
	uint32_t controller;
	uint32_t length;
	uint32_t marker_length;
	uint32_t cells;
	uint32_t kind;
	uint32_t number;
	uint32_t flags;
	int error;

	/* Finds the controller the node's interrupts are delivered to. */
	error = interrupt_parent(fdt, node, &controller);
	if (error != 0)
		return error;

	/* Refuses a parent that is not an interrupt controller. */
	error = drv_fdt_property(fdt, controller, "interrupt-controller", &marker, &marker_length);
	if (error != 0)
		return ENOTSUP;

	/* Refuses a controller whose specifiers are not the GIC's three cells. */
	cells = drv_fdt_node_cells(fdt, controller, "#interrupt-cells", 0U);
	if (cells != FDT_GIC_CELLS)
		return ENOTSUP;

	/* Finds the requested specifier inside the interrupts property. */
	error = drv_fdt_property(fdt, node, "interrupts", &value, &length);
	if (error != 0)
		return ENOENT;

	/* Reports an index past the last specifier. */
	if ((uint64_t)(index + 1U) * FDT_GIC_CELLS * 4U > length)
		return ENOENT;

	/* Decodes the kind and the number of the specifier. */
	kind = (uint32_t)drv_fdt_cells_value(value, index * FDT_GIC_CELLS, 1U);
	number = (uint32_t)drv_fdt_cells_value(value, index * FDT_GIC_CELLS + 1U, 1U);

	/* Refuses anything but a shared peripheral interrupt. */
	if (kind != FDT_GIC_KIND_SPI)
		return ENOTSUP;

	/* Requires the level-high SPI mode initialized by the RPi4 GIC implementation. */
	flags = (uint32_t)drv_fdt_cells_value(value, index * FDT_GIC_CELLS + 2U, 1U);
	if ((flags & 0x0fU) != 4U)
		return ENOTSUP;

	/* Succeeded: the kernel numbers GIC interrupts by their interrupt ID. */
	*irq = (int)(number + GIC_FIRST_SPI_ID);
	return 0;
}

/*
 * Maps a register window without caching.
 *
 * The mapping is kept for the life of the system.  Mapping reads nothing
 * from the device.
 */
int
bcm2711_map_window(
	struct bcm2711_window *window)
{
	void *mapped;
	int error;

	/* Maps the window as uncached device memory. */
	error = kern_device_map(window->physical, (size_t)window->size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Publishes the mapping to the stages that use the window. */
	window->mapped = mapped;

	/* Succeeded: the window is reachable. */
	return 0;
}

/*
 * Installs the driver's handler on one interrupt line, leaving it masked.
 *
 * The device's own interrupts may already be enabled by the firmware, and
 * the handler cannot yet quiet them, so the line is unmasked only by the
 * stage that takes the device's interrupts over.
 */
int
bcm2711_irq_install(
	struct bcm2711_irq_line *line)
{
	int error;

	/* A line the device tree did not name has nothing to install. */
	if (line->irq == BCM2711_NO_IRQ)
		return ENOENT;

	/* Keeps the line masked while the handler goes in. */
	kern_irq_mask(line->irq);

	/* Installs the handler with the line as its argument. */
	error = kern_irq_register(line->irq, irq_handler, line);
	if (error != 0)
		return error;

	/* Marks the line as owned by the driver, so a later stage may unmask it. */
	line->registered = true;
	line->count = 0;

	/* Succeeded: the handler is installed and the line stays masked. */
	return 0;
}

/*
 * Services an owned device source before retiring its GIC acknowledgement.
 * Discovery leaves lines masked. Display initialization supplies persistent
 * source callbacks before opening HVS/PV delivery; V3D remains masked.
 */
static void
irq_handler(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct bcm2711_irq_line *line;
	bool handled;

	/* The persistent argument identifies the source independently of its ID. */
	(void)irq;

	/* Services the device source before sending EOI on a level-triggered line. */
	line = argument;
	handled = true;
	if (line->service != NULL)
		handled = line->service(line->owner);

	/* Counts owned events, including the generic masked V3D discovery handler. */
	if (handled)
		line->count++;

	/* Retires the acknowledgement before returning, as every handler must. */
	kern_irq_send_eoi(acknowledge);

	/* Succeeded: this interrupt acknowledgement no longer belongs to the private handler. */
	return;
}

/* Finds the interrupt controller that serves a node, climbing its ancestors. */
static int
interrupt_parent(
	const struct drv_fdt *fdt,
	uint32_t node,
	uint32_t *controller)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t current;
	uint32_t parent;
	uint32_t phandle;
	unsigned depth;
	int error;

	/* Climbs from the node until one level names its interrupt parent. */
	current = node;
	for (depth = 0; depth < FDT_PARENT_DEPTH_LIMIT; depth++) {
		/* Stops at the first interrupt-parent property. */
		error = drv_fdt_property(fdt, current, "interrupt-parent", &value, &length);
		if (error == 0 && length == 4U)
			break;

		/* Moves to the parent; the root without the property ends the search. */
		error = drv_fdt_parent(fdt, current, &parent);
		if (error != 0)
			return ENOENT;
		current = parent;
	}

	/* Refuses a tree deeper than the search allows. */
	if (depth == FDT_PARENT_DEPTH_LIMIT)
		return ELOOP;

	/* Resolves the phandle to the controller's node. */
	phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
	error = drv_fdt_find_phandle(fdt, phandle, controller);
	if (error != 0)
		return ENOENT;

	/* Succeeded: controller names the node's interrupt controller. */
	return 0;
}
