/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD fuser userland command.
 */

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <zedbsd/system.h>

static void usage(void);
static int show_file(int descriptor, const char *path, int mount_query, int show_user);
static void print_flags(unsigned flags);

/*
 * Runs the fuser command.
 */
int
main(
	int argc,
	char **argv)
{
	int result;
	int descriptor, option, mount_query, show_user;
	int found, failed, index;

	mount_query = 0;
	show_user = 0;
	found = 0;
	failed = 0;

	/* Parse each command-line option. */
	while ((option = getopt(argc, argv, "cfu")) != -1) {
		/* Dispatch the selected command-line option. */
		switch (option) {
		case 'c':
			mount_query = 1;
			break;
		case 'f':
			break;
		case 'u':
			show_user = 1;
			break;
		default:
			usage();

			/* Reports operation failure. */
			return 2;
		}
	}

	/* Validates the command-line arguments. */
	if (optind == argc) {
		usage();

		/* Reports operation failure. */
		return 2;
	}
	descriptor = open("/dev/system", O_RDONLY);

	/* Checks the file descriptor. */
	if (descriptor < 0) {
		fprintf(stderr, "fuser: /dev/system: %s\n", strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Process each remaining command-line operand. */
	for (index = optind; index < argc; index++) {
				result = show_file(descriptor, argv[index], mount_query, show_user);

		/* Checks the operation result. */
		if (result > 0)
			found = 1;
		else if (result < 0)
			failed = 1;
	}
	(void)close(descriptor);

	/* Returns the computed result. */
	return failed || !found;
}

/* Supports the usage operation. */
static void
usage(
	void)
{
	fprintf(stderr, "usage: fuser [-cfu] file ...\n");
}

/* Supports the show file operation. */
static int
show_file(
	int descriptor,
	const char *path,
	int mount_query,
	int show_user)
{
	struct passwd *account;
	struct system_file_usage query;
	int found;

	found = 0;

	memset(&query, 0, sizeof(query));
	query.version = ZEDBSD_SYSTEM_FILE_USAGE_VERSION;
	query.struct_size = sizeof(query);
	query.cursor_pid = -1;
	query.query_flags =
	    mount_query ? ZEDBSD_SYSTEM_FILE_USAGE_QUERY_MOUNT : 0;

	/* Handles a failed strlen operation. */
	if (strlen(path) >= sizeof(query.path)) {
		fprintf(stderr, "fuser: %s: path is too long\n", path);

		/* Reports operation failure. */
		return -1;
	}
	strcpy(query.path, path);
	printf("%s:", path);

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles a failed ioctl operation. */
		if (ioctl(descriptor, ZEDBSD_SYSTEM_GET_FILE_USAGE, &query) !=
		    0) {
			/* Handles the reported system error. */
			if (errno == ENOENT)
				break;
			fprintf(stderr, "fuser: %s: %s\n", path,
				strerror(errno));
			putchar('\n');

			/* Reports operation failure. */
			return -1;
		}
		printf(" %d", query.pid);
		print_flags(query.usage_flags);

		/* Handles the show user condition. */
		if (show_user) {
						account = getpwuid(query.uid);

			/* Handles the account availability. */
			if (account != NULL)
				printf("(%s)", account->pw_name);
			else
				printf("(%u)", query.uid);
		}
		found = 1;
	}
	putchar('\n');

	/* Returns the computed result. */
	return found;
}

/* Supports the print flags operation. */
static void
print_flags(
	unsigned flags)
{
	/* Checks the active flags. */
	if ((flags & ZEDBSD_SYSTEM_FILE_USAGE_CWD) != 0)
		putchar('c');

	/* Checks the active flags. */
	if ((flags & ZEDBSD_SYSTEM_FILE_USAGE_ROOT) != 0)
		putchar('r');

	/* Checks the active flags. */
	if ((flags & ZEDBSD_SYSTEM_FILE_USAGE_EXECUTABLE) != 0)
		putchar('e');

	/* Checks the active flags. */
	if ((flags & ZEDBSD_SYSTEM_FILE_USAGE_OPEN) != 0)
		putchar('f');

	/* Checks the active flags. */
	if ((flags & ZEDBSD_SYSTEM_FILE_USAGE_MAPPED) != 0)
		putchar('m');

	/* Checks the active flags. */
	if ((flags & ZEDBSD_SYSTEM_FILE_USAGE_SOCKET) != 0)
		putchar('s');
}
