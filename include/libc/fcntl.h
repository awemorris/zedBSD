/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_FCNTL_H
#define LIBC_FCNTL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <uapi/fcntl.h>

int faccessat(int, const char *, int, int);

#include <sys/types.h>
struct flock {
	short l_type;
	short l_whence;
	off_t l_start;
	off_t l_len;
	pid_t l_pid;
};
int open(const char *, int, ...);
int openat(int, const char *, int, ...);
int creat(const char *, mode_t);
int fcntl(int, int, ...);

/* Gives the bytes [offset, offset + length) of a file storage now. */
int posix_fallocate(int, off_t, off_t);

#ifdef __cplusplus
}
#endif

#endif
