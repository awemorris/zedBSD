/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The backlight probe (ws113-p013): reads and sets a backlight device the
 * way FreeBSD's backlight(8) does, for the machine test of the panel's
 * light.
 *
 *   backlight-probe [-f DEVICE]            prints the provider, the type and the brightness
 *   backlight-probe [-f DEVICE] PERCENT    sets the brightness, then prints it
 *
 * Each line is "BACKLIGHT ..." on standard output; the exit status is 0, or
 * 1 with "BACKLIGHT error=<errno> step=<what> reason=<strerror>" (zedBSD's errno
 * numbers are its own: ENOENT is 6).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <uapi/backlight.h>

/* The default device: the built-in panel's. */
#define PROBE_DEVICE	"/dev/backlight/backlight0"

static int probe_fail(const char *step, int error);

/*
 * Reads, and sets when asked, a backlight's brightness.
 */
int
main(
	int argc,
	char **argv)
{
	struct backlight_props props;
	struct backlight_info info;
	const char *path;
	char *end;
	long percent;
	int descriptor;
	int status;
	int first;
	int named;
	int set;

	/* The device: -f names another one. */
	path = PROBE_DEVICE;
	first = 1;
	named = 0;
	if (argc >= 3)
		named = strcmp(argv[1], "-f") == 0;
	if (named) {
		path = argv[2];
		first = 3;
	}

	/* The brightness to set, when one is given. */
	set = 0;
	percent = 0;
	if (first < argc) {
		percent = strtol(argv[first], &end, 10);
		if (*end != '\0' || percent < 0 || percent > 100) {
			fprintf(stderr, "usage: backlight-probe [-f DEVICE] [PERCENT]\n");
			return 2;
		}

		/* The brightness is set before it is read. */
		set = 1;
	}

	/* The device, for writing when it is set. */
	if (set) {
		descriptor = open(path, O_RDWR);
	} else {
		descriptor = open(path, O_RDONLY);
	}

	/* A device that is not there, or not the caller's to set. */
	if (descriptor < 0)
		return probe_fail("open", errno);

	/* What it is. */
	memset(&info, 0, sizeof(info));
	status = ioctl(descriptor, BACKLIGHTGETINFO, &info);
	if (status != 0)
		return probe_fail("info", errno);
	printf("BACKLIGHT device=%s name=%s type=%d\n", path, info.name, (int)info.type);

	/* The new brightness. */
	if (set) {
		memset(&props, 0, sizeof(props));
		props.brightness = (uint32_t)percent;
		status = ioctl(descriptor, BACKLIGHTUPDATESTATUS, &props);
		if (status != 0)
			return probe_fail("update", errno);
		printf("BACKLIGHT set=%ld\n", percent);
	}

	/* The brightness it has now. */
	memset(&props, 0, sizeof(props));
	status = ioctl(descriptor, BACKLIGHTGETSTATUS, &props);
	if (status != 0)
		return probe_fail("status", errno);
	printf("BACKLIGHT brightness=%u levels=%u\n", props.brightness, props.nlevels);

	/* Succeeded. */
	(void)close(descriptor);
	return 0;
}

/* Reports a failed step and gives the failing exit status. */
static int
probe_fail(
	const char *step,
	int error)
{
	/* The line the test reads. */
	printf("BACKLIGHT error=%d step=%s reason=%s\n", error, step, strerror(error));
	return 1;
}
