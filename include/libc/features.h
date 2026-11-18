/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * XXX: Move this file under include/libc/
 */

#ifndef LIBC_FEATURES_H
#define LIBC_FEATURES_H

/*
 * XXX: Rename __KERN_* to __LIBC_*
 */
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 202405L
#define __KERN_POSIX_2024_VISIBLE	1
#elif defined(_XOPEN_SOURCE) && _XOPEN_SOURCE >= 800
#define __KERN_POSIX_2024_VISIBLE	1
#else
#define __KERN_POSIX_2024_VISIBLE	0
#endif

/*
 * XXX: Rename __KERN_* to __LIBC_*
 */
#if !defined(_POSIX_C_SOURCE) && !defined(_XOPEN_SOURCE)
#define __KERN_LEGACY_VISIBLE	1
#elif defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE < 202405L
#define __KERN_LEGACY_VISIBLE	1
#elif defined(_XOPEN_SOURCE) && _XOPEN_SOURCE < 800
#define __KERN_LEGACY_VISIBLE	1
#else
#define __KERN_LEGACY_VISIBLE	0
#endif

/*
 * POSIX.1-2024 system interfaces,
 */
#define _POSIX_VERSION			202405L
#define _POSIX2_VERSION			200809L
#define _XOPEN_VERSION			700
#define _XOPEN_UNIX			1
#define _POSIX_JOB_CONTROL		1
#define _POSIX_THREADS			200809L
#define _POSIX_THREAD_ATTR_STACKSIZE	200809L
#define _POSIX_THREAD_PROCESS_SHARED	200809L
#define _POSIX_REALTIME_SIGNALS		200809L
#define _POSIX_SHARED_MEMORY_OBJECTS	200809L
#define _POSIX_SEMAPHORES		200809L
#define _POSIX_MESSAGE_PASSING		200809L
#define _POSIX_DEVICE_CONTROL		202405L
#define _POSIX_THREAD_SAFE_FUNCTIONS	(-1)
#define _POSIX_THREAD_PRIO_INHERIT	(-1)
#define _POSIX_THREAD_PRIO_PROTECT	(-1)
#define _POSIX_ASYNCHRONOUS_IO		(-1)
#define _POSIX_PRIORITIZED_IO		(-1)
#define _POSIX_TIMERS			200809L
#define _POSIX_MONOTONIC_CLOCK		200809L
#define _POSIX_TYPED_MEMORY_OBJECTS	(-1)

/*
 * The rest of POSIX.1-2024's options and option groups (ws001-p045).  A
 * value of 202405L is a feature the C library and the kernel provide; -1 is
 * one they do not, which a program finds out here rather than at run time.
 * Those the standard makes mandatory but zedBSD lacks are -1 too, so that
 * nothing claims what is not there (the scheduling ones are ws001-p047, the
 * memory locking ones ws001-p051).
 */
#define _POSIX_ADVISORY_INFO		202405L
#define _POSIX_BARRIERS			202405L
#define _POSIX_CHOWN_RESTRICTED		1
#define _POSIX_CLOCK_SELECTION		202405L
#define _POSIX_CPUTIME			(-1)
#define _POSIX_FSYNC			202405L
#define _POSIX_IPV6			202405L
#define _POSIX_MAPPED_FILES		202405L
#define _POSIX_MEMLOCK			(-1)
#define _POSIX_MEMLOCK_RANGE		(-1)
#define _POSIX_MEMORY_PROTECTION	202405L
#define _POSIX_NO_TRUNC			1
#define _POSIX_PRIORITY_SCHEDULING	(-1)
#define _POSIX_RAW_SOCKETS		(-1)
#define _POSIX_READER_WRITER_LOCKS	202405L
#define _POSIX_REGEXP			1
#define _POSIX_SAVED_IDS		1
#define _POSIX_SHELL			1
#define _POSIX_SPAWN			202405L
#define _POSIX_SPIN_LOCKS		202405L
#define _POSIX_SPORADIC_SERVER		(-1)
#define _POSIX_SYNCHRONIZED_IO		202405L
#define _POSIX_THREAD_ATTR_STACKADDR	202405L
#define _POSIX_THREAD_CPUTIME		(-1)
#define _POSIX_THREAD_PRIORITY_SCHEDULING	(-1)
#define _POSIX_THREAD_ROBUST_PRIO_INHERIT	(-1)
#define _POSIX_THREAD_ROBUST_PRIO_PROTECT	(-1)
#define _POSIX_THREAD_SPORADIC_SERVER	(-1)
#define _POSIX_TIMEOUTS			202405L

/*
 * The C-language compilation environments: an int of 32 bits with long,
 * off_t and pointers of 64 on the LP64 targets, and everything of 32 bits
 * on i386 (whose off_t is 32 bits).
 */
#ifdef __LP64__
#define _POSIX_V7_ILP32_OFF32		(-1)
#define _POSIX_V7_ILP32_OFFBIG		(-1)
#define _POSIX_V7_LP64_OFF64		1
#define _POSIX_V7_LPBIG_OFFBIG		1
#define _POSIX_V8_ILP32_OFF32		(-1)
#define _POSIX_V8_ILP32_OFFBIG		(-1)
#define _POSIX_V8_LP64_OFF64		1
#define _POSIX_V8_LPBIG_OFFBIG		1
#else
#define _POSIX_V7_ILP32_OFF32		1
#define _POSIX_V7_ILP32_OFFBIG		(-1)
#define _POSIX_V7_LP64_OFF64		(-1)
#define _POSIX_V7_LPBIG_OFFBIG		(-1)
#define _POSIX_V8_ILP32_OFF32		1
#define _POSIX_V8_ILP32_OFFBIG		(-1)
#define _POSIX_V8_LP64_OFF64		(-1)
#define _POSIX_V8_LPBIG_OFFBIG		(-1)
#endif

/*
 * The utilities' options: the C binding and terminals are there, localedef
 * is a base utility; the C development utilities (c17), FORTRAN, the
 * software development utilities and the user portability utilities are
 * not claimed.
 */
#define _POSIX2_C_BIND			202405L
#define _POSIX2_C_DEV			(-1)
#define _POSIX2_CHAR_TERM		1
#define _POSIX2_FORT_RUN		(-1)
#define _POSIX2_LOCALEDEF		202405L
#define _POSIX2_SW_DEV			(-1)
#define _POSIX2_UPE			(-1)

/*
 * The X/Open option groups: crypt(), the enhanced internationalization and
 * the XSI shared memory are there; the realtime groups (which need the
 * scheduling options) and UUCP are not.
 */
#define _XOPEN_CRYPT			1
#define _XOPEN_ENH_I18N			1
#define _XOPEN_REALTIME			(-1)
#define _XOPEN_REALTIME_THREADS		(-1)
#define _XOPEN_SHM			1
#define _XOPEN_UUCP			(-1)

#endif
