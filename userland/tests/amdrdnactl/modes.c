/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display's modes as the GPU device offers them (amdrdnactl modes):
 * the display query (size, preferred mode, name) and each mode the
 * enumeration lists, each checked again by the validation, and a mode no
 * sink lists, which the validation must refuse.  One line each; nothing is
 * set and no lease is taken.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <uapi/gpu.h>
#include <uapi/gpu-display.h>

#include "amdrdnactl.h"

/* The most modes the tool lists, and a size no sink lists. */
#define MODES_MAX			64U
#define MODES_ODD_WIDTH			1234U
#define MODES_ODD_HEIGHT		567U

static int modes_check(int descriptor, const struct gpu_display_info *output, uint32_t width, uint32_t height, uint32_t refresh_millihz);

/*
 * Lists the modes of the first display of the GPU device at path; returns
 * 0, or 1 when a step failed.
 */
int
control_modes(
	const char *path)
{
	struct gpu_display_info output;
	struct gpu_display_mode mode;
	uint32_t count;
	uint32_t index;
	int descriptor;
	int error;
	int result;

	/* The device. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0) {
		printf("MODES fail open %s %d\n", path, errno);
		return 1;
	}

	/* The first display. */
	memset(&output, 0, sizeof(output));
	output.version = GPU_ABI_VERSION;
	output.size = sizeof(output);
	output.index = 0U;
	error = ioctl(descriptor, GPU_DISPLAY_QUERY, &output);
	if (error != 0) {
		printf("MODES fail query %d\n", errno);
		(void)close(descriptor);
		return 1;
	}
	printf("MODES display id=%u name=\"%s\" current=%ux%u@%u preferred=%ux%u physical=%ux%umm\n", output.display_id, output.name,
	    output.current_width, output.current_height, output.refresh_millihz, output.preferred_width, output.preferred_height,
	    output.physical_width_mm, output.physical_height_mm);

	/* The count. */
	memset(&mode, 0, sizeof(mode));
	mode.version = GPU_ABI_VERSION;
	mode.size = sizeof(mode);
	mode.display_id = output.display_id;
	mode.generation = output.generation;
	mode.operation = GPU_DISPLAY_MODE_ENUMERATE;
	mode.index = GPU_DISPLAY_COUNT_ONLY;
	error = ioctl(descriptor, GPU_DISPLAY_MODE, &mode);
	if (error != 0 || mode.count == 0U || mode.count > MODES_MAX) {
		printf("MODES fail count %d %u\n", error != 0 ? errno : 0, mode.count);
		(void)close(descriptor);
		return 1;
	}
	count = mode.count;

	/* Each mode, then its validation. */
	result = 0;
	for (index = 0U; index < count; index++) {
		memset(&mode, 0, sizeof(mode));
		mode.version = GPU_ABI_VERSION;
		mode.size = sizeof(mode);
		mode.display_id = output.display_id;
		mode.generation = output.generation;
		mode.operation = GPU_DISPLAY_MODE_ENUMERATE;
		mode.index = index;
		error = ioctl(descriptor, GPU_DISPLAY_MODE, &mode);
		if (error != 0) {
			printf("MODES fail mode %u %d\n", index, errno);
			result = 1;
			continue;
		}
		error = modes_check(descriptor, &output, mode.width, mode.height, mode.refresh_millihz);
		printf("MODES mode %u %ux%u@%u.%03u validate=%d\n", index, mode.width, mode.height, mode.refresh_millihz / 1000U,
		    mode.refresh_millihz % 1000U, error);
		if (error != 0)
			result = 1;
	}

	/* A mode no sink lists is refused. */
	error = modes_check(descriptor, &output, MODES_ODD_WIDTH, MODES_ODD_HEIGHT, 0U);
	printf("MODES odd %ux%u validate=%d\n", MODES_ODD_WIDTH, MODES_ODD_HEIGHT, error);
	if (error == 0)
		result = 1;

	/* The device given back. */
	(void)close(descriptor);
	printf("MODES %s count=%u\n", result == 0 ? "ok" : "fail", count);
	return result;
}

/* Validates a mode; returns 0 or the errno. */
static int
modes_check(
	int descriptor,
	const struct gpu_display_info *output,
	uint32_t width,
	uint32_t height,
	uint32_t refresh_millihz)
{
	struct gpu_display_mode mode;
	int error;

	/* The validation. */
	memset(&mode, 0, sizeof(mode));
	mode.version = GPU_ABI_VERSION;
	mode.size = sizeof(mode);
	mode.display_id = output->display_id;
	mode.generation = output->generation;
	mode.operation = GPU_DISPLAY_MODE_VALIDATE;
	mode.width = width;
	mode.height = height;
	mode.refresh_millihz = refresh_millihz;
	error = ioctl(descriptor, GPU_DISPLAY_MODE, &mode);
	if (error != 0)
		return errno;

	/* Succeeded: the mode can be lit. */
	return 0;
}
