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
#include <sys/stat.h>

static const char *kind(mode_t m);

/*
 * Runs the stat command.
 */
int
main(
	int argc,
	char **argv)
{
	struct stat s;
	int i, failed;

	failed = 0;

	/* Validates the command-line arguments. */
	if (argc < 2) {
		fprintf(stderr, "usage: stat file...\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Process each remaining command-line operand. */
	for (i = 1; i < argc; i++) {
		/* Validates the command-line arguments. */
		if (lstat(argv[i], &s)) {
			command_error("stat", argv[i]);
			failed = 1;
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
	return failed;
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
