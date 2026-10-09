/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display path of the BCM2711: discovery (stage P0).
 *
 * The firmware lights the screen before the kernel starts, and its
 * framebuffer carries the kernel's console.  P0 only learns where the display
 * hardware is: it reads the device tree, maps the compositor and the two
 * timing generators of the HDMI ports, and installs (masked) interrupt
 * handlers.  It reads and writes no register, so the firmware's picture is
 * untouched on a real board and nothing happens on an emulator that lacks the
 * hardware.
 */

#include <stdbool.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"

/* The device tree names of the display hardware (binding facts). */
#define DISPLAY_COMPAT_COMPOSITOR	"brcm,bcm2711-hvs"
#define DISPLAY_COMPAT_TIMING_HDMI0	"brcm,bcm2711-pixelvalve2"
#define DISPLAY_COMPAT_TIMING_HDMI1	"brcm,bcm2711-pixelvalve4"
#define DISPLAY_COMPAT_HDMI0		"brcm,bcm2711-hdmi0"
#define DISPLAY_COMPAT_HDMI1		"brcm,bcm2711-hdmi1"
#define DISPLAY_COMPAT_HDMI_GLUE	"brcm,brcm2711-dvp"
#define DISPLAY_COMPAT_HDMI_IRQ_BLOCK	"brcm,bcm2711-l2-intc"
#define DISPLAY_COMPAT_GROUP		"brcm,bcm2711-vc5"

/* The firmware's clock numbers of the display path (firmware wiki). */
#define DISPLAY_CLOCK_CORE		4U
#define DISPLAY_CLOCK_HDMI_MACHINE	13U
#define DISPLAY_CLOCK_HDMI_PIXEL	14U

static void discover_timing(const struct drv_fdt *fdt, struct bcm2711_display *display, unsigned port, const char *compatible);
static void discover_hdmi(const struct drv_fdt *fdt, struct bcm2711_display *display, unsigned port, const char *compatible);
static void report_node(const struct drv_fdt *fdt, const char *label, const char *compatible);

/*
 * Runs stage P0: finds and maps the display hardware.
 *
 * Returns 0 when the compositor was found and mapped, which is what the
 * later display stages need; ENODEV when the board has no usable display
 * path; ECANCELED when the boot parameters stop the display before P0.
 */
int
bcm2711_display_discover(
	const struct drv_fdt *fdt,
	struct bcm2711_display *display)
{
	uint32_t node;
	bool allowed;
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

	/* Honors rpi4gpu.stop=P0. */
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_DISPLAY, "P0");
	if (!allowed) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P0 stopped by boot parameter");
		return ECANCELED;
	}

	/* Marks the start of the stage on the screen. */
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P0 begin");

	/* Finds the compositor; without it there is no display path to drive. */
	error = bcm2711_fdt_find(fdt, DISPLAY_COMPAT_COMPOSITOR, &node);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P0 no hvs node: no display");
		return ENODEV;
	}

	/* Reads the compositor's register window. */
	error = bcm2711_fdt_window(fdt, node, 0, &display->compositor);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P0 hvs reg unreadable (%d)", error);
		return ENODEV;
	}

	/* Reads the compositor's interrupt; a missing one is shown, not fatal. */
	error = bcm2711_fdt_gic_irq(fdt, node, 0, &display->compositor_irq.irq);
	if (error != 0)
		display->compositor_irq.irq = BCM2711_NO_IRQ;

	/* Maps the compositor; no register is touched. */
	error = bcm2711_map_window(&display->compositor);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P0 hvs map failed (%d)", error);
		return ENODEV;
	}

	/* Installs the compositor's handler, leaving the line masked. */
	error = bcm2711_irq_install(&display->compositor_irq);
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P0 hvs %llx+%llx irq %d%s",
			   (unsigned long long)display->compositor.physical,
			   (unsigned long long)display->compositor.size,
			   display->compositor_irq.irq,
			   error == 0 ? " (masked)" : " (no handler)");

	/* Finds the two timing generators and the two HDMI encoders. */
	discover_timing(fdt, display, 0, DISPLAY_COMPAT_TIMING_HDMI0);
	discover_timing(fdt, display, 1, DISPLAY_COMPAT_TIMING_HDMI1);
	discover_hdmi(fdt, display, 0, DISPLAY_COMPAT_HDMI0);
	discover_hdmi(fdt, display, 1, DISPLAY_COMPAT_HDMI1);

	/* Shows whether the HDMI glue, the HDMI interrupt block and the group node exist. */
	report_node(fdt, "dvp", DISPLAY_COMPAT_HDMI_GLUE);
	report_node(fdt, "l2-intc", DISPLAY_COMPAT_HDMI_IRQ_BLOCK);
	report_node(fdt, "vc5", DISPLAY_COMPAT_GROUP);

	/* Shows the firmware's clocks of the display path; only asks. */
	bcm2711_clock_report(BCM2711_FAMILY_DISPLAY, "core", DISPLAY_CLOCK_CORE);
	bcm2711_clock_report(BCM2711_FAMILY_DISPLAY, "hdmi", DISPLAY_CLOCK_HDMI_MACHINE);
	bcm2711_clock_report(BCM2711_FAMILY_DISPLAY, "bvb", DISPLAY_CLOCK_HDMI_PIXEL);

	/* Publishes the display path to the later stages. */
	display->present = true;
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P0 ok");

	/* Succeeded: the compositor is mapped. */
	return 0;
}

/* Finds, maps and shows one timing generator; a missing one is only shown. */
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
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P0 pv of hdmi%u: no node", port);
		return;
	}

	/* Reads the generator's register window. */
	error = bcm2711_fdt_window(fdt, node, 0, window);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P0 pv of hdmi%u: reg unreadable (%d)", port, error);
		return;
	}

	/* Reads the generator's interrupt, which also carries its vertical blank. */
	error = bcm2711_fdt_gic_irq(fdt, node, 0, &line->irq);
	if (error != 0)
		line->irq = BCM2711_NO_IRQ;

	/* Maps the generator; a failure leaves it unmapped. */
	error = bcm2711_map_window(window);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P0 pv of hdmi%u: map failed (%d)", port, error);
		return;
	}

	/* Installs the generator's handler, leaving the line masked. */
	error = bcm2711_irq_install(line);
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P0 pv of hdmi%u %llx+%llx irq %d%s",
			   port,
			   (unsigned long long)window->physical,
			   (unsigned long long)window->size,
			   line->irq,
			   error == 0 ? " (masked)" : " (no handler)");
}

/* Finds and shows one HDMI encoder; P0 records its first window only. */
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
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P0 hdmi%u: no node", port);
		return;
	}

	/* Reads the encoder's first window and counts all of its windows. */
	error = bcm2711_fdt_window(fdt, node, 0, &first);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P0 hdmi%u: reg unreadable (%d)", port, error);
		return;
	}

	/* Records where the encoder is and how many windows it has. */
	display->hdmi_physical[port] = first.physical;
	display->hdmi_window_count[port] = bcm2711_fdt_reg_count(fdt, node);

	/* Shows the encoder; it is mapped by the stage that first reads it. */
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P0 hdmi%u %llx windows %u",
			   port,
			   (unsigned long long)display->hdmi_physical[port],
			   (unsigned)display->hdmi_window_count[port]);
}

/* Shows whether a node of the display path exists and whether it is enabled. */
static void
report_node(
	const struct drv_fdt *fdt,
	const char *label,
	const char *compatible)
{
	uint32_t node;
	bool enabled;
	int error;

	/* Finds the node. */
	error = bcm2711_fdt_find(fdt, compatible, &node);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P0 %s: no node", label);
		return;
	}

	/* Shows the node's status as the firmware left it. */
	enabled = drv_fdt_node_enabled(fdt, node);
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "P0 %s: %s", label, enabled ? "okay" : "disabled");
}
