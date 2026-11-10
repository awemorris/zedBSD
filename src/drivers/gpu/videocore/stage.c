/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boot parameters of the BCM2711 and BCM2712 graphics drivers.
 *
 * The parameters are read from the raw boot command line, because the driver
 * attaches while the platform discovers its devices, before the kernel parses
 * the line:
 *
 *   rpi4gpu.off=1         the driver does not attach at all
 *   v3d.tfu=0             textures are converted by the CPU, never the
 *                         texture formatting unit; without it the unit
 *                         converts them
 *
 * The Raspberry Pi 5's rpi5gpu.start=hold is read by its attachment.
 */

#include <stdbool.h>
#include <stddef.h>

#include <kern/boot.h>
#include <kern/platform.h>

#include "drivers/gpu/videocore/bcm2711-private.h"

/* The handoff object that holds the raw boot command line. */
#define STAGE_COMMAND_LINE_HANDOFF	"boot.command-line"

/* The whole token that keeps the driver from attaching. */
#define STAGE_OFF_TOKEN			"rpi4gpu.off=1"

/* The whole token that keeps texture conversion off the texture formatting unit. */
#define STAGE_TFU_OFF_TOKEN		"v3d.tfu=0"

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
 * Reports whether the boot asked that textures never be converted by the texture formatting unit.
 */
bool
bcm2711_stage_tfu_off(
	void)
{
	const char *line;
	int present;

	/* Looks for v3d.tfu=0 among the boot parameters. */
	line = stage_command_line();
	present = kern_boot_parameters_token_present(line, STAGE_TFU_OFF_TOKEN);
	if (present)
		return true;

	/* The unit converts textures. */
	return false;
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
