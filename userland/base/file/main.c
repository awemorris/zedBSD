/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD file userland command.
 */

#include "userland/base/common/command.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ELFCLASS32 1
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2
#define EM_386 3
#define EM_68K 4
#define EM_SPARCV9 43
#define EM_X86_64 62
#define EM_AARCH64 183

static int classify(const char *path, int follow);
static const char *machine_name(unsigned value);
static unsigned word(const unsigned char *p, int little);
static int text_data(const unsigned char *p, size_t n);

/*
 * Runs the file command.
 */
int
main(
	int argc,
	char **argv)
{
	int follow, index, failed;

	follow = 0;
	index = 1;
	failed = 0;

	/* Handles the selected command-line operation. */
	if (index < argc && !strcmp(argv[index], "-L")) {
		follow = 1;
		index++;
	}

	/* Handles the selected command-line operation. */
	if (index < argc && !strcmp(argv[index], "--"))
		index++;

	/* Validates the command-line arguments. */
	if (index == argc) {
		fprintf(stderr, "usage: file [-L] file...\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Process each remaining command-line operand. */
	for (; index < argc; index++)

		/* Validates the command-line arguments. */
		if (!classify(argv[index], follow))
			failed = 1;

	/* Returns the computed result. */
	return failed;
}

/* Supports the classify operation. */
static int
classify(
	const char *path,
	int follow)
{
	int little;
	char target[512];
	size_t i;
	struct stat st;
	unsigned char data[512];
	ssize_t n;
	int fd;

	/* Handles a failed stat operation. */
	if ((follow ? stat(path, &st) : lstat(path, &st)) != 0) {
		command_error("file", path);

		/* Reports successful completion. */
		return 0;
	}
	printf("%s: ", path);

	/* Handles the st condition. */
	if (S_ISDIR(st.st_mode)) {
		puts("directory");

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the st condition. */
	if (S_ISLNK(st.st_mode)) {

		n = readlink(path, (char *)target, sizeof(target) - 1U);

		/* Checks the current item count. */
		if (n < 0) {
			command_error("file", path);

			/* Reports successful completion. */
			return 0;
		}
		target[n] = '\0';
		printf("symbolic link to %s\n", target);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the st condition. */
	if (S_ISCHR(st.st_mode)) {
		puts("character special");

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the st condition. */
	if (S_ISBLK(st.st_mode)) {
		puts("block special");

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the st condition. */
	if (S_ISFIFO(st.st_mode)) {
		puts("fifo");

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the st condition. */
	if (S_ISSOCK(st.st_mode)) {
		puts("socket");

		/* Reports operation failure. */
		return 1;
	}
	fd = open(path, O_RDONLY);

	/* Checks the file descriptor. */
	if (fd < 0) {
		command_error("file", path);

		/* Reports successful completion. */
		return 0;
	}
	do

	/* Continue while the operation condition remains true. */
		n = read(fd, data, sizeof(data));
	while (n < 0 && errno == EINTR);
	close(fd);

	/* Checks the current item count. */
	if (n < 0) {
		command_error("file", path);

		/* Reports successful completion. */
		return 0;
	}

	/* Checks the current item count. */
	if (n == 0) {
		puts("empty");

		/* Reports operation failure. */
		return 1;
	}

	/* Checks the current item count. */
	if (n >= 20 && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' &&
	    data[3] == 'F' &&
	    (data[4] == ELFCLASS32 || data[4] == ELFCLASS64) &&
	    (data[5] == ELFDATA2LSB || data[5] == ELFDATA2MSB)) {
				little = data[5] == ELFDATA2LSB;
		printf("ELF %s-bit %s-endian, %s\n",
		       data[4] == ELFCLASS32 ? "32" : "64",
		       little ? "little" : "big",
		       machine_name(word(data + 18, little)));

		/* Reports operation failure. */
		return 1;
	}

	/* Checks the current item count. */
	if (n >= 2 && data[0] == '#' && data[1] == '!') {
		/* Process each remaining element. */
				i = 2;
		while (i < (size_t)n && (data[i] == ' ' || data[i] == '\t'))
			i++;
		printf("script text executable for ");

		/* Process each remaining element. */
		while (i < (size_t)n && data[i] != '\n' && data[i] != '\r')
			putchar(data[i++]);
		putchar('\n');

		/* Reports operation failure. */
		return 1;
	}

	/* Checks the current item count. */
	if (n >= 2 && data[0] == 'B' && data[1] == 'M') {
		puts("BMP image data");

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the text data condition. */
	if (text_data(data, (size_t)n)) {
		puts("text");

		/* Reports operation failure. */
		return 1;
	}
	puts("data");

	/* Reports operation failure. */
	return 1;
}

/* Supports the machine name operation. */
static const char *
machine_name(
	unsigned value)
{
	/* Dispatch the selected operation case. */
	switch (value) {
	case EM_386:
		/* Returns the computed result. */
		return "Intel 80386";
	case EM_X86_64:
		/* Returns the computed result. */
		return "AMD x86-64";
	case EM_AARCH64:
		/* Returns the computed result. */
		return "AArch64";
	case EM_SPARCV9:
		/* Returns the computed result. */
		return "SPARC V9";
	case EM_68K:
		/* Returns the computed result. */
		return "Motorola 68000";
	default:
		/* Returns the computed result. */
		return "unknown machine";
	}
}

/* Supports the word operation. */
static unsigned
word(
	const unsigned char *p,
	int little)
{
	/* Returns the computed result. */
	return little ? (unsigned)p[0] | (unsigned)p[1] << 8
		      : (unsigned)p[0] << 8 | (unsigned)p[1];
}

/* Supports the text data operation. */
static int
text_data(
	const unsigned char *p,
	size_t n)
{
	size_t i;

	/* Process each element required by the operation. */
	for (i = 0; i < n; i++)

		/* Handles a failed isprint operation. */
		if (p[i] != 0 && p[i] != '\n' && p[i] != '\r' && p[i] != '\t' &&
		    !isprint(p[i]))

			/* Reports successful completion. */
			return 0;

	/* Reports operation failure. */
	return 1;
}
