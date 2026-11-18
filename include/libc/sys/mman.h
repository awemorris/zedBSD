/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SYS_MMAN_H
#define LIBC_SYS_MMAN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/types.h>
#include <uapi/mman.h>

void *mmap(void *, size_t, int, int, int, off_t);
int munmap(void *, size_t);
int mprotect(void *, size_t, int);

int madvise(void *, size_t, int);
int posix_madvise(void *, size_t, int);

/* posix_madvise names the same advice with a prefix of its own. */
#define POSIX_MADV_NORMAL     MADV_NORMAL
#define POSIX_MADV_RANDOM     MADV_RANDOM
#define POSIX_MADV_SEQUENTIAL MADV_SEQUENTIAL
#define POSIX_MADV_WILLNEED   MADV_WILLNEED
#define POSIX_MADV_DONTNEED   MADV_DONTNEED
int msync(void *, size_t, int);
int shm_open(const char *, int, mode_t);
int shm_unlink(const char *);

#ifdef __cplusplus
}
#endif

#endif
