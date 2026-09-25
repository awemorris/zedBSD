/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SYS_RESOURCE_H
#define LIBC_SYS_RESOURCE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <uapi/resource.h>
#include <sys/time.h>

int getrlimit(int, struct rlimit *);
int setrlimit(int, const struct rlimit *);
int getpriority(int, id_t);
int setpriority(int, id_t, int);
int getrusage(int, struct rusage *);

#ifdef __cplusplus
}
#endif

#endif
