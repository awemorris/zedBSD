/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD cksum userland command.
 */

#include "userland/base/common/command.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int checksum(int fd, const char *name);
static unsigned long crc_byte(unsigned long crc, unsigned char byte);

/*
 * Runs the cksum command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	int fd;
	int i, failed;

	i = 1;
	failed = 0;

	/* Validates the command-line arguments. */
	if (i == argc) {
		/* Obtains the checksum result. */
		function_result = checksum(0, NULL);

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining command-line operand. */
	for (; i < argc; i++) {
		fd = !strcmp(argv[i], "-") ? 0 : open(argv[i], O_RDONLY);

		/* Checks the file descriptor. */
		if (fd < 0) {
			command_error("cksum", argv[i]);
			failed = 1;
			continue;
		}
		failed |= checksum(fd, argv[i]);

		/* Checks the file descriptor. */
		if (fd)
			close(fd);
	}

	/* Returns the computed result. */
	return failed;
}

/* Supports the checksum operation. */
static int
checksum(
	int fd,
	const char *name)
{
	ssize_t n;
	ssize_t i_index_for;
	unsigned char buffer[4096];
	unsigned long crc;
	unsigned long long length, value;

	/* Continue until the operation reaches a terminal state. */
	crc = 0;
	length = 0;
	for (;;) {
		n = read(fd, buffer, sizeof(buffer));

		/* Checks the current item count. */
		if (n < 0) {
			command_error("cksum", name);

			/* Reports operation failure. */
			return 1;
		}

		/* Checks the current item count. */
		if (!n)
			break;
		length += (unsigned long long)n;

		/* Process each remaining element. */
		for (i_index_for = 0; i_index_for < n; i_index_for++)
			crc = crc_byte(crc, buffer[i_index_for]);
	}

	/* Continue while the operation condition remains true. */
	value = length;
	while (value) {
		crc = crc_byte(crc, (unsigned char)value);
		value >>= 8;
	}
	crc = (~crc) & 0xffffffffUL;
	printf("%lu %llu%s%s\n", crc, length, name ? " " : "",
	       name ? name : "");

	/* Reports successful completion. */
	return 0;
}

/* Supports the crc byte operation. */
static unsigned long
crc_byte(
	unsigned long crc,
	unsigned char byte)
{
	int bit;

	crc ^= (unsigned long)byte << 24;

	/* Process each element required by the operation. */
	for (bit = 0; bit < 8; bit++) {
		crc =
		    (crc & 0x80000000UL) ? (crc << 1) ^ 0x04c11db7UL : crc << 1;
	}

	/* Returns the computed result. */
	return crc & 0xffffffffUL;
}
