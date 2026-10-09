/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The attachment of the BCM2711 graphics driver.
 *
 * The platform calls the driver once while it discovers the board's devices.
 * The driver opens the firmware's device tree, honors rpi4gpu.off=1, and runs
 * P0 discovery, N0 output readout and R0 initial scanout for the display,
 * and V0 discovery for the V3D engine. Either part may be missing (QEMU's raspi4b emulates
 * neither) without stopping the other or the boot.
 */

#include <stdbool.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-gpu.h"
#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/platform/rpi4/rpi4-firmware.h"

/*
 * How much of memory at the device tree's address may be read.
 *
 * The HAL checks the tree against the same bound before the kernel starts,
 * and the tree's header states its real size inside it.
 */
#define ATTACH_FDT_READ_LIMIT	(2U * 1024U * 1024U)

/*
 * The display path, as stage P0 found it.
 *
 * Filled once during attachment, while only the boot processor runs, and
 * read by the later display stages.  present is false until P0 succeeds.
 */
static struct bcm2711_display attach_display;

/*
 * The V3D engine, as stage V0 found it.
 *
 * Filled once during attachment, like the display path, and read by the
 * later V3D stages.  present is false until V0 succeeds.
 */
static struct bcm2711_v3d attach_v3d;

/*
 * Finds the display path and the V3D engine and prepares them.
 */
int
drv_bcm2711_gpu_attach(
	uint64_t fdt_phys,
	const struct drv_bcm2711_boot_screen *screen)
{
	struct drv_fdt fdt;
	const void *blob;
	bool off;
	int display_error;
	int v3d_error;
	int error;

	/* Stays out entirely when the boot asked for it. */
	off = bcm2711_stage_driver_off();
	if (off) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "off by boot parameter");
		return ENODEV;
	}

	/* Finds the device tree in the direct map. */
	blob = kern_pmem_to_kernel((hal_physaddr_t)fdt_phys);
	if (blob == NULL) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "no device tree");
		return ENODEV;
	}

	/* Refuses a tree whose header does not check out. */
	error = drv_fdt_open(&fdt, blob, ATTACH_FDT_READ_LIMIT);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "unreadable device tree (%d)", error);
		return ENODEV;
	}

	/*
	 * Prepares the firmware mailbox the clock questions go through.  A
	 * second preparation does nothing, so the PCIe code that may have
	 * prepared it already is not disturbed.  Without it the clock lines
	 * show the error and the stages go on.
	 */
	error = drv_rpi4_firmware_init(&fdt);
	if (error != 0)
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "firmware mailbox unavailable (%d)", error);

	/* Runs the display path's discovery. */
	display_error = bcm2711_display_discover(&fdt, &attach_display);

	/* Reads the boot output before the driver relinquishes firmware ownership. */
	if (display_error == 0) {
		display_error = bcm2711_display_readout(&attach_display, screen);
		if (display_error == 0) {
			/* Starts only from a complete, unique readout of the boot output. */
			display_error = bcm2711_display_start(&fdt, &attach_display);
			if (display_error != 0)
				bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "R0 not started (%d)", display_error);
		}
	}

	/* Publishes display operations only after the real boot scanout was confirmed. */
	if (display_error == 0) {
		display_error = bcm2711_display_register(&attach_display);
		if (display_error != 0)
			bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P5 registration failed (%d)", display_error);
	}

	/* Runs the V3D engine's discovery, whatever became of the display. */
	v3d_error = bcm2711_v3d_discover(&fdt, &attach_v3d);

	/* Reports a board where neither part could be prepared. */
	if (display_error != 0 && v3d_error != 0)
		return ENODEV;

	/* Succeeded: at least one part is prepared. */
	return 0;
}
