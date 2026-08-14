/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_STAT_H
#define ZEDBSD_SYS_STAT_H

#include <sys/types.h>

#define S_IFMT   0170000U
#define S_IFSOCK 0140000U
#define S_IFLNK  0120000U
#define S_IFREG  0100000U
#define S_IFBLK  0060000U
#define S_IFDIR  0040000U
#define S_IFCHR  0020000U
#define S_IFIFO  0010000U

#define S_ISUID 0004000U
#define S_ISGID 0002000U
#define S_ISVTX 0001000U
#define S_IRWXU 0000700U
#define S_IRUSR 0000400U
#define S_IWUSR 0000200U
#define S_IXUSR 0000100U
#define S_IRWXG 0000070U
#define S_IRGRP 0000040U
#define S_IWGRP 0000020U
#define S_IXGRP 0000010U
#define S_IRWXO 0000007U
#define S_IROTH 0000004U
#define S_IWOTH 0000002U
#define S_IXOTH 0000001U

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)

struct stat {
	dev_t st_dev;
	ino_t st_ino;
	mode_t st_mode;
	nlink_t st_nlink;
	uid_t st_uid;
	gid_t st_gid;
	dev_t st_rdev;
	off_t st_size;
} __attribute__((packed, aligned(4)));

_Static_assert(sizeof(struct stat) == 36,
	"zedBSD ELF32 stat ABI must remain 36 bytes");

int fstat(int, struct stat *);
int stat(const char *, struct stat *);
int mkdir(const char *, mode_t);

#endif
