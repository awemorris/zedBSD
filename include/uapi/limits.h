/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The limits of the zedBSD system interface.
 *
 * The kernel refuses a path, a name, an argument list or an entropy request
 * longer than these, so a program reads the same numbers to size its
 * buffers.  <limits.h> of the C library includes this and adds the limits of
 * the C language and of the utilities.
 */

#include <uapi/hosted.h>

#if !KERN_UAPI_HOST_LIBC
#ifndef KERN_UAPI_LIMITS_H
#define KERN_UAPI_LIMITS_H

#define NAME_MAX 255
#define PATH_MAX 256
/* Longest host name gethostname may report, not counting the terminator. */
#define HOST_NAME_MAX 255
#define ARG_MAX 16384
/*
 * The least a conforming system may offer.  A program that will not assume
 * this system's own limit reads this one instead: every system promises at
 * least this much.
 */
#define _POSIX_ARG_MAX 4096
#define RTSIG_MAX 33
#define SIGQUEUE_MAX 32
#define GETENTROPY_MAX 256

#define _POSIX_HOST_NAME_MAX 255
#define _POSIX_RTSIG_MAX 8
#define _POSIX_SIGQUEUE_MAX 32

/*
 * The largest byte count a read or write reports.  It is spelled with the
 * compiler's own __LONG_MAX__ so that this header needs no <limits.h>; the
 * value and the type are those of LONG_MAX.
 */
#define SSIZE_MAX __LONG_MAX__

#endif

/*
 * A host fixture that asks for zedBSD's ABI (KERN_UAPI_NATIVE) and puts
 * include/uapi itself on the include path reaches this header under the
 * standard name <limits.h>; the zedBSD C library's <limits.h>, later on the
 * path, then still supplies the rest of the standard header.  The chain is
 * taken only when the zedBSD C library headers do come later (rtld-abi.h is
 * one of them and exists nowhere else), so it never reaches the host's
 * header.  A zedBSD build never chains: the kernel has no such header, and
 * the C library's includes this one.
 */
#if !defined(__ZEDBSD__) && !defined(LIBC_LIMITS_H) && defined(__has_include_next)
#if __has_include_next(<rtld-abi.h>) && __has_include_next(<limits.h>)
#pragma GCC system_header
#include_next <limits.h>
#endif
#endif
#else

/*
 * A host fixture compiles kernel source against the host C library and takes
 * the limits from it.  The host's <limits.h> is the next one on the include
 * path; #include_next reaches it even when this directory is itself on the
 * path under the standard name.  The pragma keeps the extension quiet in a
 * -pedantic fixture.
 */
#pragma GCC system_header
#include_next <limits.h>
#ifdef LIBC_LIMITS_H
#error "the zedBSD C library headers are on the include path of a host build; define KERN_UAPI_NATIVE"
#endif

#endif
