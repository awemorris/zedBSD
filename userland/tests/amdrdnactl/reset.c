/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reset of the GPU (amdrdnactl reset [DEVICE]): asks the GPU device
 * for a reset (GPU_DEVICE_RESET, root only) and prints one line with the
 * result, the time the driver took, the resets so far and the other opens
 * of the device that were told it was lost.  The kernel waits for those
 * opens to close (their programs see the device lost) before the reset.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <uapi/gpu.h>

#include "amdrdnactl.h"

/*
 * Resets the GPU of the device at path; returns 0 when the GPU was set up
 * again, or 1 when the request was refused or the reset failed.
 */
int
control_reset(
	const char *path)
{
	struct gpu_device_reset ask;
	int descriptor;
	int saved;
	int result;

	/* The GPU device; this open holds nothing, as the reset asks. */
	descriptor = open(path, O_RDWR | O_CLOEXEC);
	if (descriptor < 0) {
		printf("RESET fail open %s %d\n", path, errno);
		return 1;
	}

	/* The request, with the kernel's own wait for the other opens; it answers when the reset has run. */
	memset(&ask, 0, sizeof(ask));
	ask.version = GPU_ABI_VERSION;
	ask.size = sizeof(ask);
	ask.wait_ms = 0U;
	result = ioctl(descriptor, GPU_DEVICE_RESET, &ask);
	saved = errno;
	(void)close(descriptor);
	if (result != 0) {
		printf("RESET refused %d (%s)\n", saved, strerror(saved));
		return 1;
	}

	/* The reset's own result. */
	if (ask.result != 0) {
		printf("RESET fail result=%d ms=%u resets=%u lost=%u\n", (int)ask.result, (unsigned)ask.milliseconds,
		    (unsigned)ask.resets, (unsigned)ask.lost);
		return 1;
	}

	/* Succeeded: the GPU runs again. */
	printf("RESET ok ms=%u resets=%u lost=%u\n", (unsigned)ask.milliseconds, (unsigned)ask.resets, (unsigned)ask.lost);
	return 0;
}
