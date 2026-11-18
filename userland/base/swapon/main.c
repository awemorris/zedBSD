/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD swapon userland command.
 */

#include "userland/base/swap-control/swap-command.h"
#include "userland/base/common/fstab.h"

#include <uapi/system.h>

static int swapon_all(void);

/* Activates independent fstab swap entries after the root namespace exists. */
static int
swapon_all(void)
{
	struct command_fstab entry;
	struct system_swap_control control;
	FILE *stream;
	char *option;
	char *next;
	unsigned line;
	int descriptor;
	int result;
	int failed;
	int noauto;
	int nofail;
	int invalid;
	int error;

	stream = fopen(FSTAB_PATH, "r");
	if (stream == NULL) {
		swap_command_report_error("swapon", FSTAB_PATH, errno);
		return 1;
	}
	descriptor = -1;
	failed = 0;
	line = 0;
	while ((result = command_fstab_next(stream, &entry, &line)) != 0) {
		if (result < 0) {
			fprintf(stderr, "swapon: %s:%u: invalid entry or read failure\n",
			    FSTAB_PATH, line);
			failed = 1;
			if (ferror(stream))
				break;
			continue;
		}
		if (strcmp(entry.type, "swap") != 0)
			continue;

		/* Validates every option, including empty components. */
		noauto = 0;
		nofail = 0;
		invalid = 0;
		option = entry.options;
		while (option != NULL) {
			next = strchr(option, ',');
			if (next != NULL)
				*next++ = '\0';
			if (strcmp(option, "noauto") == 0)
				noauto = 1;
			else if (strcmp(option, "nofail") == 0)
				nofail = 1;
			else if (strcmp(option, "defaults") != 0 && strcmp(option, "sw") != 0) {
				fprintf(stderr, "swapon: %s:%u: unsupported option: %s\n",
				    FSTAB_PATH, line, option);
				invalid = 1;
			}
			option = next;
		}
		if (invalid) {
			failed = 1;
			continue;
		}
		if (noauto)
			continue;
		if (swap_command_make_request(&control, entry.source) != 0) {
			swap_command_report_error("swapon", entry.source, errno);
			failed = 1;
			continue;
		}

		/* An empty or fully skipped table never needs the control device. */
		if (descriptor < 0)
			descriptor = SWAP_COMMAND_OPEN("/dev/system", O_RDWR);
		if (descriptor < 0) {
			swap_command_report_error("swapon", "/dev/system", errno);
			failed = 1;
			break;
		}
		if (SWAP_COMMAND_IOCTL(descriptor, KERN_SYSTEM_SWAP_ADD, &control) != 0) {
			error = errno;
			if (error == EEXIST || (nofail && (error == ENOENT || error == ENODEV)))
				continue;
			swap_command_report_error("swapon", entry.source, error);
			failed = 1;
		}
	}
	if (descriptor >= 0 && SWAP_COMMAND_CLOSE(descriptor) != 0) {
		swap_command_report_error("swapon", "/dev/system", errno);
		failed = 1;
	}
	if (fclose(stream) != 0) {
		swap_command_report_error("swapon", FSTAB_PATH, errno);
		failed = 1;
	}
	return failed;
}

/*
 * Runs the swapon command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;

	if (argc == 2 && strcmp(argv[1], "-a") == 0)
		return swapon_all();

	/* Obtains the swap command main result. */
	function_result = swap_command_main("swapon", KERN_SYSTEM_SWAP_ADD, argc, argv);

	/* Returns the computed result. */
	return function_result;
}
