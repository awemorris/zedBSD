/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * utime(): the access and modification times of a file, in whole
 * seconds.  utimensat() is the modern form; this one is kept because
 * portable software still calls it.
 */

#ifndef LIBC_UTIME_H
#define LIBC_UTIME_H

#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The two times utime() sets. */
struct utimbuf {
	time_t actime;
	time_t modtime;
};

/* Sets both times, or both to now when times is null. */
int utime(const char *path, const struct utimbuf *times);

#ifdef __cplusplus
}
#endif

#endif
