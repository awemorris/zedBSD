/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_MMAN_H
#define ZEDBSD_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE  0x00
#define PROT_READ  0x01
#define PROT_WRITE 0x02
#define PROT_EXEC  0x04

#define MAP_PRIVATE   0x0001
#define MAP_SHARED    0x0002
#define MAP_FIXED     0x0010
#define MAP_ANONYMOUS 0x0020
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FAILED ((void *)-1)

#define MS_ASYNC      0x0001
#define MS_INVALIDATE 0x0002
#define MS_SYNC       0x0004

void *mmap(void *, size_t, int, int, int, off_t);
int munmap(void *, size_t);
int mprotect(void *, size_t, int);
int msync(void *, size_t, int);

#endif
