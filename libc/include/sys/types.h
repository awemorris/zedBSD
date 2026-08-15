/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_SYS_TYPES_H
#define ZEDBSD_SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef __PTRDIFF_TYPE__ ssize_t;
#ifdef ZEDBSD_USER_ABI_LP64
typedef int64_t off_t;
typedef int64_t blkcnt_t;
typedef int64_t blksize_t;
#else
typedef int32_t off_t;
typedef int32_t blkcnt_t;
typedef int32_t blksize_t;
#endif
typedef uint32_t dev_t;
typedef uint64_t ino_t;
typedef uint32_t mode_t;
typedef uint32_t nlink_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef int32_t pid_t;
typedef int32_t tid_t;

#endif
