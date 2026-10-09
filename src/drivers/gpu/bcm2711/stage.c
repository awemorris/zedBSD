/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stage marks of the BCM2711 graphics driver, and its boot parameters.
 *
 * The driver is brought up on the board one stage at a time.  Each stage
 * writes a line when it begins and when it ends, so a photograph of the
 * firmware's framebuffer console shows how far the driver came even when a
 * later stage blanks the screen.  Lines are cut to the console's width.
 *
 * The parameters are read from the raw boot command line, because the driver
 * attaches while the platform discovers its devices, before the kernel parses
 * the line:
 *
 *   rpi4gpu.off=1         the driver does not attach at all
 *   rpi4gpu.stop=<stage>  the display path stops before that stage
 *   v3d.stop=<stage>      the V3D engine stops before that stage
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#include <kern/boot.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/platform.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"

/* The handoff object that holds the raw boot command line. */
#define STAGE_COMMAND_LINE_HANDOFF	"boot.command-line"

/* The whole token that keeps the driver from attaching. */
#define STAGE_OFF_TOKEN			"rpi4gpu.off=1"

/* The longest stop token: a family, ".stop=" and a stage name. */
#define STAGE_TOKEN_CAPACITY		48U

/* The step of the wait before a dangerous write, in microseconds. */
#define STAGE_PAUSE_STEP_US		1000U

static const char *stage_command_line(void);

/*
 * Reports whether the boot asked the driver to stay out entirely.
 */
bool
bcm2711_stage_driver_off(
	void)
{
	const char *line;
	int present;

	/* Looks for rpi4gpu.off=1 among the boot parameters. */
	line = stage_command_line();
	present = kern_boot_parameters_token_present(line, STAGE_OFF_TOKEN);
	if (present)
		return true;

	/* The driver attaches by default. */
	return false;
}

/*
 * Reports whether a stage may run.
 *
 * A stage may not run when the boot parameters hold <family>.stop=<stage>;
 * the part then stops before that stage and every later one.
 */
bool
bcm2711_stage_allowed(
	const char *family,
	const char *stage)
{
	char token[STAGE_TOKEN_CAPACITY];
	const char *line;
	int length;
	int present;

	/* Spells the stop token of this family and stage. */
	length = kern_snprintf(token, sizeof(token), "%s.stop=%s", family, stage);
	if (length < 0 || (size_t)length >= sizeof(token))
		return false;

	/* Looks for the token among the boot parameters. */
	line = stage_command_line();
	present = kern_boot_parameters_token_present(line, token);
	if (present)
		return false;

	/* Succeeded: nothing stops the stage. */
	return true;
}

/*
 * Writes one stage-mark line, cut to the console's width.
 *
 * The line reads "<family>: <text>".  Whatever does not fit in
 * BCM2711_STAGE_LINE_COLUMNS columns is dropped, so one mark never wraps and
 * pushes the earlier marks off the 25-line console.
 */
void
bcm2711_stage_mark(
	const char *family,
	const char *format,
	...)
{
	char line[BCM2711_STAGE_LINE_COLUMNS + 1U];
	va_list arguments;
	int prefix;

	/* Writes the family's prefix. */
	prefix = kern_snprintf(line, sizeof(line), "%s: ", family);
	if (prefix < 0)
		return;

	/* Leaves the prefix alone when it alone fills the line. */
	if ((size_t)prefix >= sizeof(line) - 1U)
		prefix = (int)(sizeof(line) - 1U);

	/* Writes the text after the prefix; the formatter cuts what does not fit. */
	va_start(arguments, format);
	(void)kern_vsnprintf(line + prefix, sizeof(line) - (size_t)prefix, format, arguments);
	va_end(arguments);

	/* Publishes the line to the console and the kernel log. */
	kern_logf("%s\n", line);

	/* Succeeded: the bounded stage diagnostic has reached the kernel log. */
	return;
}

/*
 * Waits before a write that may blank the screen.
 *
 * The begin line of the stage is already on the screen; the wait lets a
 * photograph catch it before the screen may go dark.  The wait polls the
 * monotonic counter, because the driver may run before interrupts are on.
 */
void
bcm2711_stage_pause(
	const char *family,
	const char *stage)
{
	unsigned waited_ms;

	/* Says on the screen that the next step is a dangerous one. */
	bcm2711_stage_mark(family, "%s waits %u ms before writing", stage, BCM2711_STAGE_PAUSE_MS);

	/* Waits out the pause one millisecond at a time. */
	for (waited_ms = 0; waited_ms < BCM2711_STAGE_PAUSE_MS; waited_ms++)
		kern_usleep_range(STAGE_PAUSE_STEP_US, STAGE_PAUSE_STEP_US);

	/* Succeeded: the announced pre-write observation interval has ended. */
	return;
}

/* Finds the raw boot command line, or NULL when the boot gave none. */
static const char *
stage_command_line(
	void)
{
	const char *line;

	/* Asks the platform for the line the loader handed over. */
	line = kern_boot_handoff(STAGE_COMMAND_LINE_HANDOFF);

	/* Reports the line; the platform keeps owning it. */
	return line;
}
