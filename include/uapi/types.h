/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD public ABI scalar types
 */

#ifndef KERN_UAPI_TYPES_H
#define KERN_UAPI_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <uapi/hosted.h>

/* The user ABI, from the compilation itself when the build did not say; see
 * sys/types.h for why. */
#ifndef KERN_USER_ABI_LP64
#ifdef __LP64__
#define KERN_USER_ABI_LP64 1
#endif
#endif

/*
 * The pointed-to address belongs to the calling user ABI, not the kernel.
 */
#ifdef KERN_USER_ABI_LP64
typedef uint64_t uapi_ptr_t;
#else
typedef uint32_t uapi_ptr_t;
#endif

#if !KERN_UAPI_HOST_LIBC

/*
 * The scalar types of the system interface: sizes, offsets, identities and
 * times that cross between a program and the kernel.  <sys/types.h> of the C
 * library includes this.
 */
typedef __PTRDIFF_TYPE__ ssize_t;

#ifdef KERN_USER_ABI_LP64
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
typedef uint32_t id_t;
typedef int32_t tid_t;
typedef uint32_t useconds_t;

/*
 * The microseconds of a time of day.  It is signed because a difference
 * between two of them is one of these, and may run backwards.
 */
typedef long suseconds_t;
typedef uint32_t reclen_t;

/*
 * A time in whole seconds since the epoch, and the names of a clock and of a
 * timer.  <uapi/time.h> builds its structures from these; they live here so
 * that <sys/types.h> declares them without the structures and the clock
 * numbers that come with <time.h>.
 */
typedef int64_t time_t;
typedef int clockid_t;
typedef int32_t timer_t;

/*
 * A count of file system blocks and a count of file serial numbers, as
 * struct statvfs reports them.  They live here for the same reason as the
 * time types: <sys/types.h> declares them without the structure.
 */
typedef uint64_t fsblkcnt_t;
typedef uint64_t fsfilcnt_t;
#else

/*
 * A host fixture compiles kernel source against the host C library and takes
 * these names from it.  tid_t and reclen_t, which the host does not have, are
 * left to the fixture as before.
 */
#include <sys/types.h>
#ifdef LIBC_SYS_TYPES_H
#error "the zedBSD C library headers are on the include path of a host build; define KERN_UAPI_NATIVE"
#endif
#endif

#endif
