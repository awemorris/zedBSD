/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SYS_QUOTA_H
#define LIBC_SYS_QUOTA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <uapi/quota.h>

int quotactl(const char *, struct quota_control *);

#ifdef __cplusplus
}
#endif

#endif
