/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The file system status of the zedBSD system interface.
 *
 * The kernel fills a struct statvfs for statvfs(2) and fstatvfs(2).
 * <sys/statvfs.h> of the C library includes this and adds the functions.
 */

#ifndef KERN_UAPI_STATVFS_H
#define KERN_UAPI_STATVFS_H

#include <uapi/hosted.h>

#if !KERN_UAPI_HOST_LIBC
#include <stdint.h>

typedef uint64_t fsblkcnt_t;
typedef uint64_t fsfilcnt_t;

#define ST_RDONLY 0x00000001UL
#define ST_NOSUID 0x00000002UL
/*
 * The filesystem is held by this machine rather than reached over a
 * network.  This is not one of the two flags the standard defines; it is
 * what this system reports for the question portable software asks about a
 * path before deciding whether reading it twice is cheap.  MNT_LOCAL in
 * <sys/mount.h> says the same thing about a mount, and the two are kept
 * apart because f_flag carries ST_ values and a mount carries MNT_ ones.
 */
#define ST_LOCAL  0x00000004UL

struct statvfs {
	uint64_t f_bsize;
	uint64_t f_frsize;
	fsblkcnt_t f_blocks;
	fsblkcnt_t f_bfree;
	fsblkcnt_t f_bavail;
	fsfilcnt_t f_files;
	fsfilcnt_t f_ffree;
	fsfilcnt_t f_favail;
	uint64_t f_fsid;
	uint64_t f_flag;
	uint64_t f_namemax;
	char f_basetype[16];
};
#else

/*
 * A host fixture compiles kernel source against the host C library and takes
 * the file system status from it.
 */
#include <sys/statvfs.h>
#ifdef LIBC_SYS_STATVFS_H
#error "the zedBSD C library headers are on the include path of a host build; define KERN_UAPI_NATIVE"
#endif
#endif

#endif
