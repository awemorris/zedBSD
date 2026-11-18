/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SYS_MOUNT_H
#define LIBC_SYS_MOUNT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <uapi/unmount.h>
#include <uapi/mount.h>

int mount(const char *, const char *, int, void *);
int unmount(const char *, int);

#ifdef __cplusplus
}
#endif

#endif
