/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD stat userland command.
 */

#include "userland/base/common/command.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *kind(mode_t m);
static int valid_format(const char *format);
static void print_format(const char *format, const char *path, const struct stat *status);

/*
 * Runs the stat command.
 */
int
main(
	int argc,
	char **argv)
{
	struct stat s;
	int i, first, failed;
	const char *format;

	failed = 0;
	format = NULL;
	first = 1;
	if (first < argc && strcmp(argv[first], "-c") == 0) {
		if (first + 1 >= argc)
			return 2;
		format = argv[first + 1];
		first += 2;
		if (!valid_format(format)) {
			fprintf(stderr, "stat: unsupported format\n");
			return 2;
		}
	}
	if (first < argc && strcmp(argv[first], "--") == 0)
		first++;

	/* Validates the command-line arguments. */
	if (first >= argc) {
		fprintf(stderr, "usage: stat [-c FORMAT] [--] file...\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Process each remaining command-line operand. */
	for (i = first; i < argc; i++) {
		/* Validates the command-line arguments. */
		if (lstat(argv[i], &s)) {
			command_error("stat", argv[i]);
			failed = 1;
			continue;
		}
		if (format != NULL) {
			print_format(format, argv[i], &s);
			continue;
		}
		printf("  File: %s\n  Size: %lld\tBlocks: %lld\tIO Block: "
		       "%ld\t%s\nDevice: %llu\tInode: %llu\tLinks: "
		       "%llu\nAccess: (%04o)\tUid: %u\tGid: %u\nAccess: "
		       "%lld\nModify: %lld\nChange: %lld\n",
		       argv[i], (long long)s.st_size, (long long)s.st_blocks,
		       (long)s.st_blksize, kind(s.st_mode),
		       (unsigned long long)s.st_dev,
		       (unsigned long long)s.st_ino,
		       (unsigned long long)s.st_nlink,
		       (unsigned)(s.st_mode & 07777), (unsigned)s.st_uid,
		       (unsigned)s.st_gid, (long long)s.st_atime,
		       (long long)s.st_mtime, (long long)s.st_ctime);
	}

	/* Returns the computed result. */
	if (fflush(stdout) != 0)
		failed = 1;
	return failed || ferror(stdout);
}

/* Supports the kind operation. */
static const char *
kind(
	mode_t m)
{
	/* Handles the m condition. */
	if (S_ISREG(m))
		return "regular file";

	/* Handles the m condition. */
	if (S_ISDIR(m))
		return "directory";

	/* Handles the m condition. */
	if (S_ISLNK(m))
		return "symbolic link";

	/* Handles the m condition. */
	if (S_ISCHR(m))
		return "character device";

	/* Handles the m condition. */
	if (S_ISBLK(m))
		return "block device";

	/* Handles the m condition. */
	if (S_ISFIFO(m))
		return "fifo";

	/* Handles the m condition. */
	if (S_ISSOCK(m))
		return "socket";

	/* Returns the computed result. */
	return "unknown";
}

/* Validate before emitting any record, so an invalid field never looks complete. */
static int
valid_format(const char *format)
{
	while (*format != '\0') {
		if (*format++ != '%')
			continue;
		if (*format == '\0')
			return 0;
		if (strchr("difsuagn%", *format) == NULL)
			return 0;
		format++;
	}
	return 1;
}

/* Numeric fields describe lstat identity; symbolic links are never followed. */
static void
print_format(const char *format, const char *path, const struct stat *status)
{
	char field;

	while (*format != '\0') {
		field = *format++;
		if (field != '%') {
			putchar(field);
			continue;
		}
		switch (*format++) {
		case 'd': printf("%llu", (unsigned long long)status->st_dev); break;
		case 'i': printf("%llu", (unsigned long long)status->st_ino); break;
		case 'f': printf("%x", (unsigned)status->st_mode); break;
		case 's': printf("%lld", (long long)status->st_size); break;
		case 'u': printf("%u", (unsigned)status->st_uid); break;
		case 'g': printf("%u", (unsigned)status->st_gid); break;
		case 'a': printf("%o", (unsigned)status->st_mode & 07777U); break;
		case 'n': fputs(path, stdout); break;
		case '%': putchar('%'); break;
		}
	}
	putchar('\n');
}
