/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JH7110's AXI DMA controller as the board wires it.
 *
 * The controller's two clocks come from the STG generator, and it has two
 * reset lines there, which its node lists without names.  Once both run
 * and both are released, the generic driver takes the controller over.
 * The board has one such controller, kept for good once brought up.
 */

#include <drivers/dma/dw-axi-dmac.h>
#include <drivers/generic/fdt.h>
#include <drivers/platform/visionfive2/vf2-crg.h>
#include <drivers/platform/visionfive2/vf2-dmac.h>
#include <kern/klog.h>
#include <uapi/errno.h>

/* How many reset lines the controller's node lists. */
#define VF2_DMAC_RESETS		2U

/*
 * The controller once brought up.
 *
 * Set by the first successful drv_vf2_dmac_get() during the board's
 * single-threaded bring-up and never cleared.
 */
static struct drv_dw_axi_dmac *vf2_dmac;

/* The controller's node in the device tree; set with vf2_dmac. */
static uint32_t vf2_dmac_node;

static int start_clock(const struct drv_fdt *fdt, uint32_t node, const char *name);

/*
 * Brings up the DMA controller a node names and hands it out.
 *
 * Procedure referenced:
 * Linux v6.12 drivers/dma/dw-axi-dmac/dw-axi-dmac-platform.c (dw_probe:
 * for starfive,jh7110-axi-dma, release every listed reset line, and run
 * the core and configuration clocks before touching the registers).
 */
int
drv_vf2_dmac_get(
	const struct drv_fdt *fdt,
	uint32_t node,
	int board_default_coherent,
	struct drv_dw_axi_dmac **controller)
{
	struct drv_vf2_reset reset;
	unsigned position;
	int error;

	/* Refuses a missing tree or result. */
	if (fdt == NULL || controller == NULL)
		return EINVAL;

	/* A controller already up is handed out again; a second one is not supported. */
	if (vf2_dmac != NULL) {
		if (node != vf2_dmac_node)
			return ENOTSUP;
		*controller = vf2_dmac;
		return 0;
	}

	/* Runs the controller's core clock. */
	error = start_clock(fdt, node, "core-clk");
	if (error != 0)
		return error;

	/* Runs the controller's configuration clock. */
	error = start_clock(fdt, node, "cfgr-clk");
	if (error != 0)
		return error;

	/* Releases each of its reset lines. */
	for (position = 0; position < VF2_DMAC_RESETS; position++) {
		/* Finds the reset line at the position. */
		error = drv_vf2_reset_lookup_index(fdt, node, position, &reset);
		if (error != 0)
			return error;

		/* Releases it. */
		error = drv_vf2_reset_deassert(&reset);
		if (error != 0) {
			kern_logf("vf2-dmac: reset %u stays asserted (%d)\n", position, error);
			return error;
		}
	}

	/* Hands the running controller to the generic driver. */
	error = drv_dw_axi_dmac_attach(fdt, node, board_default_coherent, &vf2_dmac);
	if (error != 0)
		return error;

	/* Succeeded: the board's controller is up and kept for later callers. */
	vf2_dmac_node = node;
	*controller = vf2_dmac;
	return 0;
}

/* Runs one named clock of the controller. */
static int
start_clock(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name)
{
	struct drv_vf2_clock clock;
	int error;

	/* Finds the clock the name selects. */
	error = drv_vf2_clock_lookup(fdt, node, name, &clock);
	if (error != 0)
		return error;

	/* Opens its gate and those above it. */
	error = drv_vf2_clock_enable(&clock);
	if (error != 0) {
		kern_logf("vf2-dmac: clock %s stays off (%d)\n", name, error);
		return error;
	}

	/* Succeeded: the clock runs. */
	return 0;
}
