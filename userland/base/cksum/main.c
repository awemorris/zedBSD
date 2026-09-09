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
#include <errno.h>
#include "userland/base/common/sha256.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int checksum(int fd, const char *name);
static int sha256_checksum(int fd, const char *name);
static int use_sha256;
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
	use_sha256 = 0;
	if (i < argc && strcmp(argv[i], "-a") == 0) {
		if (i + 1 >= argc) {
			fprintf(stderr, "usage: cksum [-a crc|sha256] [--] [file ...]\n");
			return 2;
		}
		if (strcmp(argv[i + 1], "sha256") == 0)
			use_sha256 = 1;
		else if (strcmp(argv[i + 1], "crc") != 0) {
			fprintf(stderr, "cksum: unsupported algorithm\n");
			return 2;
		}
		i += 2;
	}
	if (i < argc && strcmp(argv[i], "--") == 0)
		i++;

	/* Validates the command-line arguments. */
	if (i == argc) {
		/* Obtains the checksum result. */
		function_result = (use_sha256 ? sha256_checksum(0, NULL) : checksum(0, NULL));

		/* Returns the computed result. */
		return function_result || fflush(stdout) != 0;
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
		failed |= use_sha256 ? sha256_checksum(fd, argv[i]) : checksum(fd, argv[i]);

		/* Checks the file descriptor. */
		if (strcmp(argv[i], "-") != 0 && close(fd) != 0) {
			command_error("cksum", argv[i]);
			failed = 1;
		}
	}

	/* Returns the computed result. */
	return failed || fflush(stdout) != 0;
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

/* Hash a stream with bounded memory, including stdin and interrupted reads. */
static int
sha256_checksum(int fd, const char *name)
{
	struct command_sha256_context context;
	uint8_t buffer[16384];
	uint8_t digest[32];
	const char *cursor;
	ssize_t count;
	unsigned i;
	int error;
	int escape;

	command_sha256_init(&context);
	for (;;) {
		count = read(fd, buffer, sizeof(buffer));
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			command_error("cksum", name);
			return 1;
		}
		if (count == 0)
			break;
		error = command_sha256_update(&context, buffer, (size_t)count);
		if (error != 0) {
			errno = error;
			command_error("cksum", name);
			return 1;
		}
	}
	command_sha256_final(&context, digest);
	if (name == NULL)
		name = "-";
	escape = strchr(name, '\n') != NULL || strchr(name, '\\') != NULL;
	if (escape)
		putchar('\\');
	for (i = 0; i < sizeof(digest); i++)
		printf("%02x", (unsigned)digest[i]);
	fputs("  ", stdout);
	for (cursor = name; *cursor != '\0'; cursor++) {
		if (*cursor == '\n')
			fputs("\\n", stdout);
		else if (*cursor == '\\')
			fputs("\\\\", stdout);
		else
			putchar(*cursor);
	}
	putchar('\n');
	return ferror(stdout) ? 1 : 0;
}
