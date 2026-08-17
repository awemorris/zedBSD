/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_STATVFS_H
#define ZEDBSD_SYS_STATVFS_H

#include <stdint.h>

typedef uint64_t fsblkcnt_t;
typedef uint64_t fsfilcnt_t;

#define ST_RDONLY 0x00000001UL
#define ST_NOSUID 0x00000002UL

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
};

int statvfs(const char *, struct statvfs *);
int fstatvfs(int, struct statvfs *);

#endif
