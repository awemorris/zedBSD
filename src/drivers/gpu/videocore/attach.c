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
 * discovery, output readout and the initial scanout for the display, and
 * discovery plus native PM/clock preparation for the V3D engine.  Either part
 * may be missing (QEMU's raspi4b emulates neither) without stopping the other
 * or the boot.
 */

#include <stdbool.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <kern/klog.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/videocore/bcm2711-gpu.h"
#include "drivers/gpu/videocore/bcm2711-private.h"
#include "drivers/gpu/videocore/render-device.h"
#include "drivers/platform/rpi4/rpi4-firmware.h"

/*
 * How much of memory at the device tree's address may be read.
 *
 * The HAL checks the tree against the same bound before the kernel starts,
 * and the tree's header states its real size inside it.
 */
#define ATTACH_FDT_READ_LIMIT	(2U * 1024U * 1024U)

/*
 * The display path, as discovery found it.
 *
 * Filled once during attachment, while only the boot processor runs, and
 * read by the later display steps.  present is false until discovery
 * succeeds.
 */
static struct bcm2711_display attach_display;

/*
 * The V3D engine, as discovery found it.
 *
 * Filled once during attachment, like the display path, and read by the
 * later V3D steps.  present is false until discovery succeeds.
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
	bool tfu_off;
	int display_error;
	int v3d_error;
	int error;

	/* Stays out entirely when the boot asked for it. */
	off = bcm2711_stage_driver_off();
	if (off) {
		kern_logf("rpi4gpu: off by boot parameter\n");
		return ENODEV;
	}

	/* Finds the device tree in the direct map. */
	blob = kern_pmem_to_kernel((kern_physaddr_t)fdt_phys);
	if (blob == NULL) {
		kern_logf("rpi4gpu: no device tree\n");
		return ENODEV;
	}

	/* Refuses a tree whose header does not check out. */
	error = drv_fdt_open(&fdt, blob, ATTACH_FDT_READ_LIMIT);
	if (error != 0) {
		kern_logf("rpi4gpu: unreadable device tree (%d)\n", error);
		return ENODEV;
	}

	/*
	 * Prepares the firmware mailbox the clock questions go through.  A
	 * second preparation does nothing, so the PCIe code that may have
	 * prepared it already is not disturbed.  Without it the clock questions
	 * fail and the attachment goes on.
	 */
	error = drv_rpi4_firmware_init(&fdt);
	if (error != 0)
		kern_logf("rpi4gpu: firmware mailbox unavailable (%d)\n", error);

	/* Runs the display path's discovery. */
	display_error = bcm2711_display_discover(&fdt, &attach_display);

	/* Reads the boot output before the driver relinquishes firmware ownership. */
	if (display_error == 0) {
		display_error = bcm2711_display_readout(&attach_display, screen);
		if (display_error == 0) {
			/* Starts only from a complete, unique readout of the boot output. */
			display_error = bcm2711_display_start(&fdt, &attach_display);
			if (display_error != 0)
				kern_logf("rpi4gpu: display not started (%d)\n", display_error);
		}
	}

	/* Publishes display operations only after the real boot scanout was confirmed. */
	if (display_error == 0) {
		display_error = bcm2711_display_register(&attach_display);
		if (display_error != 0)
			kern_logf("rpi4gpu: display registration failed (%d)\n", display_error);
	}

	/* Runs the V3D engine's discovery, whatever became of the display. */
	v3d_error = bcm2711_v3d_discover(&fdt, &attach_v3d);

	/* Native PM and clocks must be established before any engine identification read. */
	if (v3d_error == 0) {
		v3d_error = bcm2711_v3d_power_prepare(&fdt, &attach_v3d);
		if (v3d_error == 0)
			v3d_error = bcm2711_v3d_power_start(&attach_v3d);
		if (v3d_error != 0)
			kern_logf("v3d: power not ready (%d)\n", v3d_error);
	}

	/* Initializes translations and serviced IRQs only after native power is ready. */
	if (v3d_error == 0) {
		v3d_error = bcm2711_v3d_hardware_start(&attach_v3d);
		if (v3d_error != 0)
			kern_logf("v3d: engine not ready (%d)\n", v3d_error);
	}

	/* The texture formatting unit converts textures unless the boot kept the CPU's conversion. */
	if (v3d_error == 0) {
		tfu_off = bcm2711_stage_tfu_off();
		if (!tfu_off)
			attach_v3d.hardware.tfu_uif_ready = true;
	}

	/* Publishes independent resource/share ownership only once the engine is ready. */
	if (v3d_error == 0) {
		v3d_error = bcm2711_render_register(&attach_v3d);
		if (v3d_error != 0)
			kern_logf("v3d: render registration failed (%d)\n", v3d_error);
	}

	/* Reports a board where neither part could be prepared. */
	if (display_error != 0 && v3d_error != 0)
		return ENODEV;

	/* Succeeded: at least one part is prepared. */
	return 0;
}
