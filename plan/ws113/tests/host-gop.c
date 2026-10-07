/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws113-p002: the host test of the firmware's output (the GPU scanout rule; ws051-p004c: DP SST on a Type-C port):
 * drv_i915_gop_output_read() and drv_i915_gop_output_name(), taken out of
 * src/drivers/gpu/i915/display/output.c by host-gop.sh, on N0 reports made
 * here.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* What the two functions read of the driver's types (internal.h). */
enum i915_native_pipe_class {
	I915_N0_POWER_OFF = 0,
	I915_N0_READ_ERROR,
	I915_N0_READABLE_INACTIVE,
	I915_N0_READABLE_ACTIVE
};

struct i915_native_pipe {
	int readable;
	int cls;
	uint32_t transconf, trans_ddi_func, pipesrc, plane_ctl, plane_surf, plane_stride, plane_size;
};

struct i915_native_report {
	struct i915_native_pipe pipe[4];
};

#define I915_GOP_NONE		0U
#define I915_GOP_EDP		1U
#define I915_GOP_HDMI		2U
#define I915_GOP_OTHER		3U
#define I915_GOP_DP_TC		4U

struct i915_gop_output {
	unsigned kind;
	unsigned pipe;
	int port;
	unsigned mode;
	unsigned pipes;
};

#define kern_snprintf snprintf

/* The constants and the two functions of output.c (host-gop.sh writes them here). */
#include "gop-functions.inc"

static int failures;
static int checks;

/* A report with every pipe readable and off. */
static void
blank(
	struct i915_native_report *report)
{
	unsigned index;

	memset(report, 0, sizeof(*report));
	for (index = 0U; index < 4U; index++)
		report->pipe[index].cls = I915_N0_READABLE_INACTIVE;
}

/* Lights a pipe with a transcoder driving a port (enum port) in a mode. */
static void
light(
	struct i915_native_report *report,
	unsigned pipe,
	int port,
	unsigned mode)
{
	report->pipe[pipe].cls = I915_N0_READABLE_ACTIVE;
	report->pipe[pipe].transconf = 0x80000000U;
	report->pipe[pipe].trans_ddi_func = 0x80000000U | ((uint32_t)(port + 1) << 27) | (mode << 24);
}

/* Checks what was read and its name. */
static void
expect(
	const char *what,
	const struct i915_native_report *report,
	unsigned kind,
	unsigned pipes,
	const char *name)
{
	struct i915_gop_output gop;
	char text[48];

	drv_i915_gop_output_read(report, &gop);
	drv_i915_gop_output_name(&gop, text, sizeof(text));
	checks++;
	if (gop.kind == kind && gop.pipes == pipes && strcmp(text, name) == 0)
		return;
	failures++;
	printf("FAIL %s: kind %u pipes 0x%x '%s', not kind %u pipes 0x%x '%s'\n", what, gop.kind, gop.pipes, text, kind, pipes, name);
}

int
main(void)
{
	struct i915_native_report report;

	/* Nothing lit. */
	blank(&report);
	expect("none", &report, I915_GOP_NONE, 0U, "none");

	/* The GOP's panel on this machine: pipe A, DDI A, DP SST. */
	light(&report, 0U, 0, 2U);
	expect("eDP", &report, I915_GOP_EDP, 1U, "DP SST on DDI A, pipe A");

	/* The firmware cloning onto the panel and HDMI: the panel (the lowest pipe), both pipes recorded. */
	light(&report, 1U, 1, 0U);
	expect("clone", &report, I915_GOP_EDP, 3U, "DP SST on DDI A, pipe A");

	/* HDMI alone on DDI B (HDMI and DVI). */
	blank(&report);
	light(&report, 1U, 1, 0U);
	expect("HDMI", &report, I915_GOP_HDMI, 2U, "HDMI on DDI B, pipe B");
	blank(&report);
	light(&report, 1U, 1, 1U);
	expect("DVI", &report, I915_GOP_HDMI, 2U, "DVI on DDI B, pipe B");

	/* DP SST on a Type-C port (DP-alt): the external DP path (ws051-p004c), TC1 to TC4. */
	blank(&report);
	light(&report, 0U, 3, 2U);
	expect("DP TC1", &report, I915_GOP_DP_TC, 1U, "DP SST on DDI TC1, pipe A");
	blank(&report);
	light(&report, 1U, 4, 2U);
	expect("DP TC2", &report, I915_GOP_DP_TC, 2U, "DP SST on DDI TC2, pipe B");
	blank(&report);
	light(&report, 3U, 6, 2U);
	expect("DP TC4", &report, I915_GOP_DP_TC, 8U, "DP SST on DDI TC4, pipe D");

	/* The panel and a Type-C DP display cloned: the panel (the lowest pipe), both pipes recorded. */
	blank(&report);
	light(&report, 0U, 0, 2U);
	light(&report, 1U, 3, 2U);
	expect("clone TC1", &report, I915_GOP_EDP, 3U, "DP SST on DDI A, pipe A");

	/* An interface the driver cannot light: DP MST on a Type-C port, DP on DDI B, HDMI on a Type-C port, a port past TC4. */
	blank(&report);
	light(&report, 0U, 3, 3U);
	expect("DP MST TC1", &report, I915_GOP_OTHER, 1U, "DP MST on DDI TC1, pipe A");
	blank(&report);
	light(&report, 2U, 1, 2U);
	expect("DP B", &report, I915_GOP_OTHER, 4U, "DP SST on DDI B, pipe C");
	blank(&report);
	light(&report, 1U, 4, 0U);
	expect("HDMI TC2", &report, I915_GOP_OTHER, 2U, "HDMI on DDI TC2, pipe B");
	blank(&report);
	light(&report, 0U, 7, 2U);
	expect("DP past TC4", &report, I915_GOP_OTHER, 1U, "DP SST on DDI TC5, pipe A");

	/* A lit plane without an enabled transcoder drives no port; the next pipe that does is the output. */
	blank(&report);
	report.pipe[0].cls = I915_N0_READABLE_ACTIVE;
	report.pipe[0].plane_ctl = 0x80000000U;
	light(&report, 1U, 1, 0U);
	expect("plane only", &report, I915_GOP_HDMI, 3U, "HDMI on DDI B, pipe B");

	/* A pipe that could not be read is not lit. */
	blank(&report);
	report.pipe[0].cls = I915_N0_READ_ERROR;
	report.pipe[0].trans_ddi_func = 0x88000000U;
	expect("read error", &report, I915_GOP_NONE, 0U, "none");

	printf("host-gop: %d/%d passed\n", checks - failures, checks);
	return failures != 0;
}
