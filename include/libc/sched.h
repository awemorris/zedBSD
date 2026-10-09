/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SCHED_H
#define LIBC_SCHED_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/* struct timespec, for sched_rr_get_interval(); POSIX lets this header make <time.h>'s names visible. */
#include <time.h>

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2

/*
 * struct sched_param is defined in <sys/types.h>, included above, because
 * the thread attribute object there carries one.
 */

int sched_yield(void);

#ifdef __cplusplus
}
#endif

#endif
