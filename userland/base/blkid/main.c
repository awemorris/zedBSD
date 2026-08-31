/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * blkid - print block-device attributes.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <zedbsd/blkid.h>

static int identify(const char *path);

/*
 * Runs the blkid command.
 */
int
main(
	int argc,
	char **argv)
{
	int status, index;

	status = 0;

	/* Validates the command-line arguments. */
	if (argc == 1) {
		fprintf(stderr, "usage: blkid device ...\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Process each remaining command-line operand. */
	for (index = 1; index < argc; index++)
		status |= identify(argv[index]);

	/* Returns the computed result. */
	return status;
}

/* Supports the identify operation. */
static int
identify(
	const char *path)
{
	int error;
	struct block_identity id;
	int fd;

	fd = open(path, O_RDONLY);

	/* Checks the file descriptor. */
	if (fd < 0) {
		fprintf(stderr, "blkid: %s: %s\n", path, strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed ioctl operation. */
	if (ioctl(fd, BLKGETIDENTITY, &id) != 0) {
		error = errno;
		close(fd);

		/* Handles an operation failure. */
		if (error == ENOENT || error == ENOTTY || error == EOPNOTSUPP ||
		    error == ENXIO)

			/* Reports successful completion. */
			return 0;
		fprintf(stderr, "blkid: %s: %s\n", path, strerror(error));

		/* Reports operation failure. */
		return 1;
	}
	close(fd);
	printf("%s:", path);

	/* Handles the id condition. */
	if (id.flags & ZEDBSD_BLKID_LABEL)
		printf(" LABEL=\"%s\"", id.label);

	/* Handles the id condition. */
	if (id.flags & ZEDBSD_BLKID_UUID)
		printf(" UUID=\"%s\"", id.uuid);

	/* Handles the id condition. */
	if (id.flags & ZEDBSD_BLKID_TYPE)
		printf(" TYPE=\"%s\"", id.type);

	/* Handles the id condition. */
	if (id.flags & ZEDBSD_BLKID_PARTLABEL)
		printf(" PARTLABEL=\"%s\"", id.partlabel);

	/* Handles the id condition. */
	if (id.flags & ZEDBSD_BLKID_PARTUUID)
		printf(" PARTUUID=\"%s\"", id.partuuid);
	putchar('\n');

	/* Reports successful completion. */
	return 0;
}
