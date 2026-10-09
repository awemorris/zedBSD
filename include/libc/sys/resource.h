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

/*
 * The number of resource limits, under the name FreeBSD and Linux give it.
 * It is not POSIX; software that validates a resource number before calling
 * getrlimit() (Python's resource module among them) is written against it.
 */
#define RLIM_NLIMITS RLIMIT_NLIMITS

/*
 * The values a soft or hard limit that cannot be represented reads back as:
 * every limit fits rlim_t here, so they are RLIM_INFINITY, as the standard
 * allows.
 */
#define RLIM_SAVED_CUR RLIM_INFINITY
#define RLIM_SAVED_MAX RLIM_INFINITY

int getrlimit(int, struct rlimit *);
int setrlimit(int, const struct rlimit *);
int getpriority(int, id_t);
int setpriority(int, id_t, int);
int getrusage(int, struct rusage *);

#ifdef __cplusplus
}
#endif

#endif
