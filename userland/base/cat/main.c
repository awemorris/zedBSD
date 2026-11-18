/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Concatenates and prints files (POSIX XCU cat).
 *
 *	cat [-u] [file...]
 *
 * Each file (standard input for - or none) is copied to standard output
 * in order.  cat never buffers its output: every read is written at once,
 * so -u, which asks for that, changes nothing.  A file that cannot be read
 * is reported and the others are still copied; a failed write ends cat.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The size of the buffer the files are copied through. */
#define CAT_BUFFER_SIZE 65536

static int copy_operand(const char *path);
static int copy_descriptor(int input, const char *name);
static int write_all(const char *data, size_t length);
static void usage(void);

/*
 * Runs cat.
 */
int
main(
	int argc,
	char **argv)
{
	int option;
	int index;
	int failed;
	int status;

	/* Reads -u, which changes nothing. */
	for (;;) {
		option = getopt(argc, argv, "u");
		if (option == -1)
			break;

		/* -u is the only option. */
		if (option != 'u')
			usage();
	}

	/* Standard input without files. */
	if (optind >= argc) {
		status = copy_descriptor(STDIN_FILENO, "standard input");
		if (status < 0)
			return 1;
		return 0;
	}

	/* Each file; a read failure is remembered and the rest go on. */
	failed = 0;
	for (index = optind; index < argc; index++) {
		status = copy_operand(argv[index]);

		/* A failed write stops cat. */
		if (status == -2)
			return 1;
		if (status != 0)
			failed = 1;
	}

	/* Reports whether any file could not be read. */
	if (failed)
		return 1;

	/* Succeeded: every file was copied. */
	return 0;
}

/*
 * Copies one operand, - being standard input.  Returns 0, -1 after a
 * read failure, or -2 after a write failure.
 */
static int
copy_operand(
	const char *path)
{
	int descriptor;
	int status;
	int compare;

	/* - is standard input. */
	compare = strcmp(path, "-");
	if (compare == 0) {
		status = copy_descriptor(STDIN_FILENO, "standard input");
		return status;
	}

	/* Opens the file. */
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0) {
		fprintf(stderr, "cat: %s: %s\n", path, strerror(errno));
		return -1;
	}

	/* Copies it and closes it. */
	status = copy_descriptor(descriptor, path);
	close(descriptor);

	/* Reports what happened. */
	if (status != 0)
		return status;

	/* Succeeded: the file was copied. */
	return 0;
}

/*
 * Copies a descriptor to standard output.  Returns 0, -1 after a read
 * failure, or -2 after a write failure.
 */
static int
copy_descriptor(
	int input,
	const char *name)
{
	static char buffer[CAT_BUFFER_SIZE];
	ssize_t got;
	int status;

	/* Copies each chunk as it arrives. */
	for (;;) {
		got = read(input, buffer, sizeof(buffer));
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0) {
			fprintf(stderr, "cat: %s: %s\n", name, strerror(errno));
			return -1;
		}

		/* The end of the input ends the copy. */
		if (got == 0)
			break;

		/* Writes the whole chunk. */
		status = write_all(buffer, (size_t)got);
		if (status != 0) {
			fprintf(stderr, "cat: write error: %s\n", strerror(errno));
			return -2;
		}
	}

	/* Succeeded: the input is used up. */
	return 0;
}

/* Writes a whole buffer to standard output. */
static int
write_all(
	const char *data,
	size_t length)
{
	ssize_t written;
	size_t offset;

	/* Writes until every byte is out. */
	offset = 0;
	while (offset < length) {
		written = write(STDOUT_FILENO, data + offset, length - offset);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		/* Goes on after what was written. */
		offset += (size_t)written;
	}

	/* Succeeded: the whole buffer was written. */
	return 0;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr, "usage: cat [-u] [file...]\n");
	exit(1);
}
