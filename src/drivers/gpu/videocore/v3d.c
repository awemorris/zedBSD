/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The V3D 4.2 render engine of the BCM2711: discovery.
 *
 * The engine sits in its own power domain, which is off until the driver
 * turns it on, and touching its registers while the domain is off can stop
 * the processor.  Discovery therefore only reads the device tree, maps the hub and
 * core windows and installs a (masked) interrupt handler.  It reads no
 * register of the engine.
 */

#include <stdbool.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <kern/klog.h>
#include <uapi/errno.h>

#include "drivers/gpu/videocore/bcm2711-private.h"

/* The device tree name of the engine (binding fact). */
#define ENGINE_COMPATIBLE		"brcm,2711-v3d"

/* The register windows of the engine, in the order the device tree lists them. */
#define ENGINE_WINDOW_HUB			0U
#define ENGINE_WINDOW_CORE			1U

/* The cells of one clocks entry: the provider's phandle and its clock number. */
#define ENGINE_CLOCK_CELLS			2U

static uint32_t read_clock_id(const struct drv_fdt *fdt, uint32_t node);
static bool has_property(const struct drv_fdt *fdt, uint32_t node, const char *name);

/*
 * Finds and maps the V3D engine.
 *
 * Returns 0 when both windows were found and mapped; ENODEV when the board
 * has no usable engine.
 */
int
bcm2711_v3d_discover(
	const struct drv_fdt *fdt,
	struct bcm2711_v3d *v3d)
{
	uint32_t node;
	int error;

	/* Starts from an empty description with no interrupt line. */
	v3d->present = false;
	v3d->hub.mapped = NULL;
	v3d->core.mapped = NULL;
	v3d->irq.irq = BCM2711_NO_IRQ;
	v3d->irq.registered = false;
	v3d->irq.count = 0;
	v3d->irq.service = NULL;
	v3d->irq.owner = NULL;
	v3d->has_power_domain = false;
	v3d->has_reset = false;
	v3d->clock_id = 0;

	/* Finds the engine; without it there is nothing to render with. */
	error = bcm2711_fdt_find(fdt, ENGINE_COMPATIBLE, &node);
	if (error != 0) {
		kern_logf("v3d: no v3d node: no engine\n");
		return ENODEV;
	}

	/* Reads the hub window, which holds the parts shared by every core. */
	error = bcm2711_fdt_window(fdt, node, ENGINE_WINDOW_HUB, &v3d->hub);
	if (error != 0) {
		kern_logf("v3d: hub registers unreadable (%d)\n", error);
		return ENODEV;
	}

	/* Reads the window of the engine's single core. */
	error = bcm2711_fdt_window(fdt, node, ENGINE_WINDOW_CORE, &v3d->core);
	if (error != 0) {
		kern_logf("v3d: core registers unreadable (%d)\n", error);
		return ENODEV;
	}

	/* Reads the one interrupt the hub and the core share. */
	error = bcm2711_fdt_gic_irq(fdt, node, 0, &v3d->irq.irq);
	if (error != 0)
		v3d->irq.irq = BCM2711_NO_IRQ;

	/* Records which power, reset and clock providers the node names. */
	v3d->has_power_domain = has_property(fdt, node, "power-domains");
	v3d->has_reset = has_property(fdt, node, "resets");
	v3d->clock_id = read_clock_id(fdt, node);

	/* Maps the hub; no register is read while the power domain may be off. */
	error = bcm2711_map_window(&v3d->hub);
	if (error != 0) {
		kern_logf("v3d: hub map failed (%d)\n", error);
		return ENODEV;
	}

	/* Maps the core, under the same rule. */
	error = bcm2711_map_window(&v3d->core);
	if (error != 0) {
		kern_logf("v3d: core map failed (%d)\n", error);
		return ENODEV;
	}

	/* Installs the engine's handler, leaving the line masked; a line without a handler stays absent. */
	(void)bcm2711_irq_install(&v3d->irq);

	/* Publishes the engine to the later steps. */
	v3d->present = true;

	/* Succeeded: both windows are mapped. */
	return 0;
}

/*
 * Reads the firmware clock number from the node's first clocks entry.
 *
 * The clock is read by position, not by name, because the firmware's tree
 * misspells the property that names it.  Zero means no clock was named.
 */
static uint32_t
read_clock_id(
	const struct drv_fdt *fdt,
	uint32_t node)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t clock_id;
	int error;

	/* Finds the clocks property. */
	error = drv_fdt_property(fdt, node, "clocks", &value, &length);
	if (error != 0)
		return 0;

	/* Refuses an entry too short to hold a provider and a number. */
	if (length < ENGINE_CLOCK_CELLS * 4U)
		return 0;

	/* Reads the number that follows the provider's phandle. */
	clock_id = (uint32_t)drv_fdt_cells_value(value, 1U, 1U);

	/* Reports the clock number. */
	return clock_id;
}

/* Reports whether a node has a property, whatever its value. */
static bool
has_property(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name)
{
	const uint8_t *value;
	uint32_t length;
	int error;

	/* Looks the property up. */
	error = drv_fdt_property(fdt, node, name, &value, &length);
	if (error != 0)
		return false;

	/* The property is there. */
	return true;
}
