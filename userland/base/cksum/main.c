/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writes file checksums and sizes (POSIX XCU cksum).
 *
 *	cksum [file...]
 *
 * For each file (standard input for - or none) the CRC of POSIX XCU cksum
 * is written with the number of bytes and the name: the 32-bit CRC with
 * the polynomial 0x04C11DB7 of the bytes followed by their count in as
 * few bytes as it takes, least significant first, and complemented.
 * Standard input read without a file operand has no name.
 *
 * zedBSD also accepts -a sha256, which the installer uses: the SHA-256
 * digest in hexadecimal, two spaces and the name, with a backslash before
 * the digest and escapes in the name when the name holds a newline or a
 * backslash.  -a crc is the default.
 */

#include "userland/base/common/sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The size of the buffer the files are read through. */
#define CKSUM_BUFFER_SIZE 65536

/* The CRC polynomial of POSIX cksum. */
#define CKSUM_POLYNOMIAL 0x04c11db7UL

/*
 * The CRC of each byte value, for a byte at a time.
 *
 * It is filled once, before the first file, and only read after.
 */
static unsigned long cksum_table[256];

/*
 * Whether -a sha256 was given.
 *
 * It is set while the options are read and only read after.
 */
static int cksum_sha256;

static int read_options(int argc, char **argv);
static void fill_table(void);
static int sum_operand(const char *path);
static int sum_crc(int input, const char *name);
static int sum_sha256(int input, const char *name);
static int read_chunk(int input, unsigned char *buffer, size_t size, size_t *got);
static void usage(void);

/*
 * Runs cksum.
 */
int
main(
	int argc,
	char **argv)
{
	int first;
	int index;
	int failed;
	int status;

	/* Reads -a; the files follow. */
	first = read_options(argc, argv);
	fill_table();

	/* Standard input without files, with no name. */
	failed = 0;
	if (first >= argc) {
		if (cksum_sha256)
			status = sum_sha256(STDIN_FILENO, "-");
		else
			status = sum_crc(STDIN_FILENO, NULL);
		if (status != 0)
			failed = 1;
	}

	/* Each file; a failure is remembered and the rest go on. */
	for (index = first; index < argc; index++) {
		status = sum_operand(argv[index]);
		if (status != 0)
			failed = 1;
	}

	/* A failed write is an error too. */
	fflush(stdout);
	status = ferror(stdout);
	if (status != 0) {
		fprintf(stderr, "cksum: write error\n");
		return 1;
	}

	/* Reports whether any file could not be read. */
	if (failed)
		return 1;

	/* Succeeded: every file was summed. */
	return 0;
}

/* Reads -a crc|sha256 and -- and returns the index of the first file. */
static int
read_options(
	int argc,
	char **argv)
{
	int index;
	int compare;

	/* -a names the algorithm. */
	index = 1;
	cksum_sha256 = 0;
	if (index < argc) {
		compare = strcmp(argv[index], "-a");
		if (compare == 0) {
			if (index + 1 >= argc)
				usage();
			compare = strcmp(argv[index + 1], "sha256");
			if (compare == 0) {
				cksum_sha256 = 1;
			} else {
				compare = strcmp(argv[index + 1], "crc");
				if (compare != 0) {
					fprintf(stderr, "cksum: unsupported algorithm: '%s'\n", argv[index + 1]);
					exit(1);
				}
			}

			/* The algorithm is read. */
			index += 2;
		}
	}

	/* -- ends the options. */
	if (index < argc) {
		compare = strcmp(argv[index], "--");
		if (compare == 0)
			index++;
	}

	/* An option that is not known is an error. */
	if (index < argc && argv[index][0] == '-' && argv[index][1] != '\0') {
		fprintf(stderr, "cksum: unknown option '%s'\n", argv[index]);
		usage();
	}

	/* Reports where the files start. */
	return index;
}

/* Fills the table of the CRC of each byte value. */
static void
fill_table(void)
{
	unsigned long crc;
	int value;
	int bit;

	/* Divides each byte, shifted to the top, by the polynomial. */
	for (value = 0; value < 256; value++) {
		crc = (unsigned long)value << 24;
		for (bit = 0; bit < 8; bit++) {
			if (crc & 0x80000000UL)
				crc = (crc << 1) ^ CKSUM_POLYNOMIAL;
			else
				crc <<= 1;
		}

		/* Keeps the remainder. */
		cksum_table[value] = crc & 0xffffffffUL;
	}
}

/* Opens one operand, - being standard input, and sums it. */
static int
sum_operand(
	const char *path)
{
	int descriptor;
	int status;
	int compare;

	/* - is standard input, named as given. */
	descriptor = STDIN_FILENO;
	compare = strcmp(path, "-");
	if (compare != 0) {
		descriptor = open(path, O_RDONLY);
		if (descriptor < 0) {
			fprintf(stderr, "cksum: %s: %s\n", path, strerror(errno));
			return -1;
		}
	}

	/* Sums it by the chosen algorithm. */
	if (cksum_sha256)
		status = sum_sha256(descriptor, path);
	else
		status = sum_crc(descriptor, path);

	/* Closes a file that was opened. */
	if (descriptor != STDIN_FILENO)
		close(descriptor);
	if (status != 0)
		return -1;

	/* Succeeded: the file was summed. */
	return 0;
}

/* Writes the POSIX CRC, the size and the name of one input. */
static int
sum_crc(
	int input,
	const char *name)
{
	static unsigned char buffer[CKSUM_BUFFER_SIZE];
	unsigned long long length;
	unsigned long long remaining;
	unsigned long crc;
	const char *shown;
	size_t got;
	size_t index;
	int status;

	/* The CRC of the bytes. */
	shown = "standard input";
	if (name != NULL)
		shown = name;
	crc = 0;
	length = 0;
	for (;;) {
		status = read_chunk(input, buffer, sizeof(buffer), &got);
		if (status != 0) {
			fprintf(stderr, "cksum: %s: %s\n", shown, strerror(errno));
			return -1;
		}

		/* The end of the input ends the sum. */
		if (got == 0)
			break;
		length += got;
		for (index = 0; index < got; index++)
			crc = ((crc << 8) ^ cksum_table[((crc >> 24) ^ buffer[index]) & 0xff]) & 0xffffffffUL;
	}

	/* Then the length, least significant byte first, as few as it takes. */
	for (remaining = length; remaining != 0; remaining >>= 8)
		crc = ((crc << 8) ^ cksum_table[((crc >> 24) ^ (remaining & 0xff)) & 0xff]) & 0xffffffffUL;
	crc = ~crc & 0xffffffffUL;

	/* The CRC, the size, and the name when there is one. */
	if (name != NULL)
		printf("%lu %llu %s\n", crc, length, name);
	else
		printf("%lu %llu\n", crc, length);
	return 0;
}

/* Writes the SHA-256 digest and the name of one input for -a sha256. */
static int
sum_sha256(
	int input,
	const char *name)
{
	static unsigned char buffer[CKSUM_BUFFER_SIZE];
	struct command_sha256_context context;
	uint8_t digest[32];
	const char *cursor;
	const char *newline;
	const char *backslash;
	size_t got;
	size_t index;
	int status;
	int escape;

	/* Hashes the bytes. */
	command_sha256_init(&context);
	for (;;) {
		status = read_chunk(input, buffer, sizeof(buffer), &got);
		if (status != 0) {
			fprintf(stderr, "cksum: %s: %s\n", name, strerror(errno));
			return -1;
		}

		/* The end of the input ends the hash. */
		if (got == 0)
			break;
		status = command_sha256_update(&context, buffer, got);
		if (status != 0) {
			fprintf(stderr, "cksum: %s: %s\n", name, strerror(status));
			return -1;
		}
	}

	/* Finishes the digest. */
	command_sha256_final(&context, digest);

	/* A name with a newline or a backslash is escaped, marked by a backslash. */
	escape = 0;
	newline = strchr(name, '\n');
	backslash = strchr(name, '\\');
	if (newline != NULL || backslash != NULL)
		escape = 1;
	if (escape)
		putchar('\\');

	/* The digest, two spaces and the name. */
	for (index = 0; index < sizeof(digest); index++)
		printf("%02x", (unsigned)digest[index]);
	fputs("  ", stdout);
	for (cursor = name; *cursor != '\0'; cursor++) {
		if (*cursor == '\n')
			fputs("\\n", stdout);
		else if (*cursor == '\\')
			fputs("\\\\", stdout);
		else
			putchar(*cursor);
	}

	/* Ends the line. */
	putchar('\n');

	/* Succeeded: the digest was written. */
	return 0;
}

/* Reads one chunk, retrying after an interruption; got 0 is the end. */
static int
read_chunk(
	int input,
	unsigned char *buffer,
	size_t size,
	size_t *got)
{
	ssize_t count;

	/* Reads, retrying an interrupted read. */
	for (;;) {
		count = read(input, buffer, size);
		if (count >= 0)
			break;
		if (errno != EINTR)
			return -1;
	}

	/* Succeeded: the chunk, or the end. */
	*got = (size_t)count;
	return 0;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX form and the extension. */
	fprintf(stderr, "usage: cksum [-a crc|sha256] [file...]\n");
	exit(1);
}
