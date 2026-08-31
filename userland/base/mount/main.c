/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD mount userland command.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>

static const char *program_name(const char *path);
static int run_unmount(int argc, char **argv);
static int mount_all(void);
static int mount_fstab_entry(const char *source, const char *target, const char *type, char *options);

/*
 * Runs the mount command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	const char *option;
	struct mount_args arguments;
	const char *type, *source, *target;
	int flags, i;

	type = NULL;
	source = NULL;
	target = NULL;
	flags = 0;

	/* Handles the selected command-line operation. */
	if (strcmp(program_name(argv[0]), "umount") == 0) {
		/* Obtains the run unmount result. */
		function_result = run_unmount(argc, argv);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (argc == 2 && strcmp(argv[1], "-a") == 0) {
		/* Obtains the mount all result. */
		function_result = mount_all();

		/* Returns the computed result. */
		return function_result;
	}

	memset(&arguments, 0, sizeof(arguments));

	/* Process each remaining command-line operand. */
	arguments.size = sizeof(arguments);
	arguments.version = ZEDBSD_MOUNT_ARGS_VERSION;
	for (i = 1; i < argc; i++) {
		/* Handles the selected command-line operation. */
		if (strcmp(argv[i], "-r") == 0) {
			flags |= MNT_RDONLY;
		} else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
			type = argv[++i];
		} else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			option = argv[++i];

			/* Selects the matching value. */
			if (strcmp(option, "ro") == 0)
				flags |= MNT_RDONLY;
			else if (strcmp(option, "nosuid") == 0)
				flags |= MNT_NOSUID;
			else if (strncmp(option, "fspec=", 6) == 0) {
				source = option + 6;
			} else {
				fprintf(stderr,
					"mount: unsupported option: %s\n",
					option);

				/* Reports operation failure. */
				return 2;
			}
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "mount: unknown option: %s\n", argv[i]);

			/* Reports operation failure. */
			return 2;
		} else if (target == NULL) {
			target = argv[i];
		} else if (source == NULL) {
			source = target;
			target = argv[i];
		} else {
			target = NULL;
			break;
		}
	}

	/* Handles the type availability. */
	if (type == NULL || target == NULL) {
		fprintf(stderr,
			"usage: mount -t type [-r] [-o ro|nosuid|fspec=disk] "
			"[disk] directory\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Handles the source availability. */
	if (source != NULL) {
		/* Selects the matching prefix. */
		if (strncmp(source, "/dev/", 5) == 0)
			source += 5;

		/* Handles a failed strlen operation. */
		if (strlen(source) >= sizeof(arguments.fspec)) {
			fprintf(stderr, "mount: device name is too long\n");

			/* Reports operation failure. */
			return 2;
		}
		strcpy(arguments.fspec, source);
	}

	/* Handles a failed mount operation. */
	if (mount(type, target, flags, source != NULL ? &arguments : NULL) !=
	    0) {
		fprintf(stderr, "mount: %s on %s: %s\n",
			source != NULL ? source : type, target,
			strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the program name operation. */
static const char *
program_name(
	const char *path)
{
	const char *slash;

	slash = strrchr(path != NULL ? path : "", '/');

	/* Returns the computed result. */
	return slash != NULL ? slash + 1 : path;
}

/* Supports the run unmount operation. */
static int
run_unmount(
	int argc,
	char **argv)
{
	/* Validates the command-line arguments. */
	if (argc != 2) {
		fprintf(stderr, "usage: umount directory\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Validates the command-line arguments. */
	if (unmount(argv[1], 0) != 0) {
		fprintf(stderr, "umount: %s: %s\n", argv[1], strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the mount all operation. */
static int
mount_all(
	void)
{
	char *source, *target, *type, *options, *extra, *cursor;
	FILE *stream;
	char line[1024];
	unsigned line_number;
	int failed;

	stream = fopen("/etc/fstab", "r");
	line_number = 0;
	failed = 0;

	/* Handles the stream availability. */
	if (stream == NULL) {
		fprintf(stderr, "mount: /etc/fstab: %s\n", strerror(errno));

		/* Reports operation failure. */
		return 1;
	}
	while (fgets(line, sizeof(line), stream) != NULL) {
		cursor = line;
		line_number++;

		/* Continue while the operation condition remains true. */
		while (*cursor == ' ' || *cursor == '\t')
			cursor++;

		/* Checks the current cursor position. */
		if (*cursor == '#' || *cursor == '\n' || *cursor == '\0')
			continue;
		source = strtok(cursor, " \t\r\n");
		target = strtok(NULL, " \t\r\n");
		type = strtok(NULL, " \t\r\n");
		options = strtok(NULL, " \t\r\n");
		extra = strtok(NULL, " \t\r\n");

		/* Handles the source availability. */
		if (source == NULL || target == NULL || type == NULL ||
		    options == NULL || extra != NULL) {
			fprintf(stderr, "mount: /etc/fstab:%u: invalid entry\n",
				line_number);
			failed = 1;
			continue;
		}

		/* Handles a failed mount fstab entry operation. */
		if (mount_fstab_entry(source, target, type, options) != 0)
			failed = 1;
	}

	/* Handles an operation failure. */
	if (ferror(stream))
		failed = 1;

	/* Handles a failed fclose operation. */
	if (fclose(stream) != 0)
		failed = 1;

	/* Returns the computed result. */
	return failed;
}

/* Supports the mount fstab entry operation. */
static int
mount_fstab_entry(
	const char *source,
	const char *target,
	const char *type,
	char *options)
{
	struct mount_args arguments;
	char *option;
	int flags, nofail;

	flags = 0;
	nofail = 0;

	/* Selects the matching value. */
	if (strcmp(target, "/") == 0)
		return 0;
	memset(&arguments, 0, sizeof(arguments));
	arguments.size = sizeof(arguments);
	arguments.version = ZEDBSD_MOUNT_ARGS_VERSION;

	/* Selects the matching prefix. */
	if (strncmp(source, "/dev/", 5) == 0)
		source += 5;

	/* Handles a failed strlen operation. */
	if (strlen(source) >= sizeof(arguments.fspec)) {
		errno = ENAMETOOLONG;

		/* Reports operation failure. */
		return -1;
	}
	strcpy(arguments.fspec, source);

	/* Process each element required by the operation. */
	for (option = strtok(options, ","); option != NULL;
	     option = strtok(NULL, ",")) {
		/* Selects the matching value. */
		if (strcmp(option, "ro") == 0)
			flags |= MNT_RDONLY;
		else if (strcmp(option, "nosuid") == 0)
			flags |= MNT_NOSUID;
		else if (strcmp(option, "nofail") == 0)
			nofail = 1;
		else if (strcmp(option, "rw") != 0 &&
			 strcmp(option, "defaults") != 0) {
			fprintf(stderr, "mount: unsupported fstab option: %s\n",
				option);

			/* Reports operation failure. */
			return -1;
		}
	}

	/* Handles the reported system error. */
	if (mount(type, target, flags, &arguments) == 0 ||
	    (nofail && (errno == ENOENT || errno == ENODEV)))

		/* Reports successful completion. */
		return 0;
	fprintf(stderr, "mount: %s on %s: %s\n", source, target,
		strerror(errno));

	/* Reports operation failure. */
	return -1;
}
