/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display path of the BCM2711: discovery.
 *
 * The firmware lights the screen before the kernel starts, and its
 * framebuffer carries the kernel's console.  Discovery only learns where the display
 * hardware is: it reads the device tree, maps the compositor and the two
 * timing generators of the HDMI ports, and installs (masked) interrupt
 * handlers.  It reads and writes no register, so the firmware's picture is
 * untouched on a real board and nothing happens on an emulator that lacks the
 * hardware.
 */

#include <stdbool.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <kern/klog.h>
#include <uapi/errno.h>

#include "drivers/gpu/videocore/bcm2711-private.h"

/* The device tree names of the display hardware (binding facts). */
#define DISPLAY_COMPAT_COMPOSITOR	"brcm,bcm2711-hvs"
#define DISPLAY_COMPAT_TIMING_HDMI0	"brcm,bcm2711-pixelvalve2"
#define DISPLAY_COMPAT_TIMING_HDMI1	"brcm,bcm2711-pixelvalve4"
#define DISPLAY_COMPAT_HDMI0		"brcm,bcm2711-hdmi0"
#define DISPLAY_COMPAT_HDMI1		"brcm,bcm2711-hdmi1"

static void discover_timing(const struct drv_fdt *fdt, struct bcm2711_display *display, unsigned port, const char *compatible);
static void discover_hdmi(const struct drv_fdt *fdt, struct bcm2711_display *display, unsigned port, const char *compatible);

/*
 * Finds and maps the display hardware.
 *
 * Returns 0 when the compositor was found and mapped, which is what the
 * later display steps need; ENODEV when the board has no usable display
 * path.
 */
int
bcm2711_display_discover(
	const struct drv_fdt *fdt,
	struct bcm2711_display *display)
{
	uint32_t node;
	unsigned port;
	int error;

	/* Starts from an empty description with no interrupt lines. */
	display->present = false;
	display->compositor.mapped = NULL;
	display->compositor_irq.irq = BCM2711_NO_IRQ;
	display->compositor_irq.registered = false;
	display->compositor_irq.count = 0;
	display->compositor_irq.service = NULL;
	display->compositor_irq.owner = NULL;

	/* Clears the ports' timing generators and encoders. */
	for (port = 0; port < BCM2711_TIMING_COUNT; port++) {
		display->timing[port].mapped = NULL;
		display->timing_irq[port].irq = BCM2711_NO_IRQ;
		display->timing_irq[port].registered = false;
		display->timing_irq[port].count = 0;
		display->timing_irq[port].service = NULL;
		display->timing_irq[port].owner = NULL;
		display->hdmi_physical[port] = 0;
		display->hdmi_window_count[port] = 0;
	}

	/* Finds the compositor; without it there is no display path to drive. */
	error = bcm2711_fdt_find(fdt, DISPLAY_COMPAT_COMPOSITOR, &node);
	if (error != 0) {
		kern_logf("rpi4gpu: no hvs node: no display\n");
		return ENODEV;
	}

	/* Reads the compositor's register window. */
	error = bcm2711_fdt_window(fdt, node, 0, &display->compositor);
	if (error != 0) {
		kern_logf("rpi4gpu: compositor registers unreadable (%d)\n", error);
		return ENODEV;
	}

	/* Reads the compositor's interrupt; a missing one is not fatal. */
	error = bcm2711_fdt_gic_irq(fdt, node, 0, &display->compositor_irq.irq);
	if (error != 0)
		display->compositor_irq.irq = BCM2711_NO_IRQ;

	/* Maps the compositor; no register is touched. */
	error = bcm2711_map_window(&display->compositor);
	if (error != 0) {
		kern_logf("rpi4gpu: compositor map failed (%d)\n", error);
		return ENODEV;
	}

	/* Installs the compositor's handler, leaving the line masked; a line without a handler stays absent. */
	(void)bcm2711_irq_install(&display->compositor_irq);

	/* Finds the two timing generators and the two HDMI encoders. */
	discover_timing(fdt, display, 0, DISPLAY_COMPAT_TIMING_HDMI0);
	discover_timing(fdt, display, 1, DISPLAY_COMPAT_TIMING_HDMI1);
	discover_hdmi(fdt, display, 0, DISPLAY_COMPAT_HDMI0);
	discover_hdmi(fdt, display, 1, DISPLAY_COMPAT_HDMI1);

	/* Publishes the display path to the later steps. */
	display->present = true;

	/* Succeeded: the compositor is mapped. */
	return 0;
}

/* Finds and maps one timing generator; a missing one is only logged. */
static void
discover_timing(
	const struct drv_fdt *fdt,
	struct bcm2711_display *display,
	unsigned port,
	const char *compatible)
{
	struct bcm2711_window *window;
	struct bcm2711_irq_line *line;
	uint32_t node;
	int error;

	/* Selects the port's window and interrupt line. */
	window = &display->timing[port];
	line = &display->timing_irq[port];

	/* Finds the generator's node. */
	error = bcm2711_fdt_find(fdt, compatible, &node);
	if (error != 0) {
		kern_logf("rpi4gpu: no timing generator for hdmi%u\n", port);
		return;
	}

	/* Reads the generator's register window. */
	error = bcm2711_fdt_window(fdt, node, 0, window);
	if (error != 0) {
		kern_logf("rpi4gpu: timing generator of hdmi%u registers unreadable (%d)\n", port, error);
		return;
	}

	/* Reads the generator's interrupt, which also carries its vertical blank. */
	error = bcm2711_fdt_gic_irq(fdt, node, 0, &line->irq);
	if (error != 0)
		line->irq = BCM2711_NO_IRQ;

	/* Maps the generator; a failure leaves it unmapped. */
	error = bcm2711_map_window(window);
	if (error != 0) {
		kern_logf("rpi4gpu: timing generator of hdmi%u map failed (%d)\n", port, error);
		return;
	}

	/* Installs the generator's handler, leaving the line masked; a line without a handler stays absent. */
	(void)bcm2711_irq_install(line);

	/* Succeeded: the timing generator is mapped. */
	return;
}

/* Finds one HDMI encoder; discovery records its first window only. */
static void
discover_hdmi(
	const struct drv_fdt *fdt,
	struct bcm2711_display *display,
	unsigned port,
	const char *compatible)
{
	struct bcm2711_window first;
	uint32_t node;
	int error;

	/* Finds the encoder's node. */
	error = bcm2711_fdt_find(fdt, compatible, &node);
	if (error != 0) {
		kern_logf("rpi4gpu: no hdmi%u encoder\n", port);
		return;
	}

	/* Reads the encoder's first window and counts all of its windows. */
	error = bcm2711_fdt_window(fdt, node, 0, &first);
	if (error != 0) {
		kern_logf("rpi4gpu: hdmi%u registers unreadable (%d)\n", port, error);
		return;
	}

	/* Records where the encoder is and how many windows it has. */
	display->hdmi_physical[port] = first.physical;
	display->hdmi_window_count[port] = bcm2711_fdt_reg_count(fdt, node);

	/* Succeeded: the encoder is recorded; the step that first reads it maps it. */
	return;
}
