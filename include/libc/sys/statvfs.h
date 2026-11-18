/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SYS_STATVFS_H
#define LIBC_SYS_STATVFS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <uapi/statvfs.h>

int statvfs(const char *, struct statvfs *);
int fstatvfs(int, struct statvfs *);

#ifdef __cplusplus
}
#endif

#endif
