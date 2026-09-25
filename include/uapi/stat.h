/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The file status of the zedBSD system interface.
 *
 * The kernel fills a struct stat for stat(2) and its relatives, and the mode
 * bits name the file types and permissions in it and in every call that
 * takes a mode.  <sys/stat.h> of the C library includes this and adds the
 * functions.
 */

#ifndef KERN_UAPI_STAT_H
#define KERN_UAPI_STAT_H

#include <uapi/hosted.h>

#if !KERN_UAPI_HOST_LIBC
#include <uapi/time.h>
#include <uapi/types.h>

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
	struct timespec st_atim;
	struct timespec st_mtim;
	struct timespec st_ctim;
	blksize_t st_blksize;
	blkcnt_t st_blocks;
#ifndef KERN_USER_ABI_LP64
} __attribute__((packed, aligned(4)));
#else
};
#endif

#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec

#ifdef KERN_USER_ABI_LP64
_Static_assert(sizeof(struct timespec) == 16, "LP64 timespec ABI");
_Static_assert(sizeof(struct stat) == 112, "LP64 stat ABI");
_Static_assert(__builtin_offsetof(struct stat, st_ino) == 8, "LP64 stat ino");
_Static_assert(__builtin_offsetof(struct stat, st_size) == 40, "LP64 stat size");
_Static_assert(__builtin_offsetof(struct stat, st_atim) == 48, "LP64 stat time");
_Static_assert(__builtin_offsetof(struct stat, st_blksize) == 96,
	"LP64 stat blksize");
#else
_Static_assert(sizeof(struct timespec) == 12, "ILP32 time64 timespec ABI");
_Static_assert(sizeof(struct stat) == 80, "ILP32 time64 stat ABI");
#endif
#else

/*
 * A host fixture compiles kernel source against the host C library and takes
 * the file status from it.
 */
#include <sys/stat.h>
#ifdef LIBC_SYS_STAT_H
#error "the zedBSD C library headers are on the include path of a host build; define KERN_UAPI_NATIVE"
#endif
#endif

#endif
