/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_ERRNO_H
#define LIBC_ERRNO_H

#include <uapi/errno.h>

#ifdef __cplusplus
extern "C" {
#endif

int *__libc_errno_location(void);
#define errno (*__libc_errno_location())

#ifdef __cplusplus
}
#endif

#endif
