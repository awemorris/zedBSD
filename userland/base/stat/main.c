/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <stdio.h>
#include <sys/stat.h>
static const char *
kind(mode_t m)
{
	if (S_ISREG(m))
		return "regular file";
	if (S_ISDIR(m))
		return "directory";
	if (S_ISLNK(m))
		return "symbolic link";
	if (S_ISCHR(m))
		return "character device";
	if (S_ISBLK(m))
		return "block device";
	if (S_ISFIFO(m))
		return "fifo";
	if (S_ISSOCK(m))
		return "socket";
	return "unknown";
}
int
main(int argc, char **argv)
{
	int i, failed = 0;
	if (argc < 2) {
		fprintf(stderr, "usage: stat file...\n");
		return 1;
	}
	for (i = 1; i < argc; i++) {
		struct stat s;
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
	return failed;
}
