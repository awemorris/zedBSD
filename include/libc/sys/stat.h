/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SYS_STAT_H
#define LIBC_SYS_STAT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <uapi/stat.h>

int fstat(int, struct stat *);
int stat(const char *, struct stat *);
int lstat(const char *, struct stat *);
int fstatat(int, const char *, struct stat *, int);
int mkdir(const char *, mode_t);
int mkdirat(int, const char *, mode_t);
int mkfifo(const char *, mode_t);
int mkfifoat(int, const char *, mode_t);
int mknod(const char *, mode_t, dev_t);
int mknodat(int, const char *, mode_t, dev_t);
int chmod(const char *, mode_t);
int fchmod(int, mode_t);
int fchmodat(int, const char *, mode_t, int);
int futimens(int, const struct timespec [2]);
int utimensat(int, const char *, const struct timespec [2], int);
int utimes(const char *, const struct timeval [2]);
mode_t umask(mode_t);

#ifdef __cplusplus
}
#endif

#endif
