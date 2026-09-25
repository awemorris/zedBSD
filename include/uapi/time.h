/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The time types and clocks of the zedBSD system interface.
 *
 * A time crosses the system call boundary as one of these structures, and a
 * clock or a timer is named by one of these numbers.  <time.h> and
 * <sys/time.h> of the C library include this and add the calendar and the
 * functions.
 */

#include <uapi/hosted.h>

#if !KERN_UAPI_HOST_LIBC
#ifndef KERN_UAPI_TIME_H
#define KERN_UAPI_TIME_H

#include <stdint.h>
#include <uapi/types.h>

typedef int64_t time_t;
typedef int clockid_t;
typedef int32_t timer_t;
#define CLOCK_MONOTONIC 1
#define CLOCK_REALTIME  2
#define TIMER_ABSTIME   1
#define UTIME_NOW  1073741823L
#define UTIME_OMIT 1073741822L
struct timespec { time_t tv_sec; long tv_nsec; };
struct itimerspec { struct timespec it_interval; struct timespec it_value; };

struct timeval {
	time_t tv_sec;
	suseconds_t tv_usec;
};

struct itimerval { struct timeval it_interval, it_value; };

#define ITIMER_REAL 0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF 2

#endif

/*
 * A host fixture that asks for zedBSD's ABI (KERN_UAPI_NATIVE) and puts
 * include/uapi itself on the include path reaches this header under the
 * standard name <time.h>; the zedBSD C library's <time.h>, later on the
 * path, then still supplies the rest of the standard header.  The chain is
 * taken only when the zedBSD C library headers do come later (rtld-abi.h is
 * one of them and exists nowhere else), so it never reaches the host's
 * header.  A zedBSD build never chains: the kernel has no such header, and
 * the C library's includes this one.
 */
#if !defined(__ZEDBSD__) && !defined(KERN_TIME_H) && defined(__has_include_next)
#if __has_include_next(<rtld-abi.h>) && __has_include_next(<time.h>)
#pragma GCC system_header
#include_next <time.h>
#endif
#endif
#else

/*
 * A host fixture compiles kernel source against the host C library, whose
 * clocks the fixture's own calls read.  The host's <time.h> is the next one
 * on the include path; #include_next reaches it even when this directory is
 * itself on the path under the standard name.  The pragma keeps the extension
 * quiet in a -pedantic fixture.
 */
#pragma GCC system_header
#include_next <time.h>
#include <sys/time.h>
#if defined(KERN_TIME_H) || defined(LIBC_SYS_TIME_H)
#error "the zedBSD C library headers are on the include path of a host build; define KERN_UAPI_NATIVE"
#endif

#endif
