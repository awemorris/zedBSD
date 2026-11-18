/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The access modes, standard descriptors and seek origins of the zedBSD
 * system interface.
 *
 * access(2) and lseek(2) take these numbers from a program and the kernel
 * reads them, so both use the one definition here.  <unistd.h> and <stdio.h>
 * of the C library include this.
 */

#include <uapi/hosted.h>

#if !KERN_UAPI_HOST_LIBC
#ifndef KERN_UAPI_UNISTD_H
#define KERN_UAPI_UNISTD_H

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_DATA 3
#define SEEK_HOLE 4

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#endif

/*
 * A host fixture that asks for zedBSD's ABI (KERN_UAPI_NATIVE) and puts
 * include/uapi itself on the include path reaches this header under the
 * standard name <unistd.h>; the zedBSD C library's <unistd.h>, later on the
 * path, then still supplies the rest of the standard header.  The chain is
 * taken only when the zedBSD C library headers do come later (rtld-abi.h is
 * one of them and exists nowhere else), so it never reaches the host's
 * header.  A zedBSD build never chains: the kernel has no such header, and
 * the C library's includes this one.
 */
#if !defined(__ZEDBSD__) && !defined(LIBC_UNISTD_H) && defined(__has_include_next)
#if __has_include_next(<rtld-abi.h>) && __has_include_next(<unistd.h>)
#pragma GCC system_header
#include_next <unistd.h>
#endif
#endif
#else

/*
 * A host fixture compiles kernel source against the host C library, whose
 * lseek the fixture's own calls reach.  The host's <unistd.h> is the next one
 * on the include path; #include_next reaches it even when this directory is
 * itself on the path under the standard name.  The pragma keeps the extension
 * quiet in a -pedantic fixture.
 */
#pragma GCC system_header
#include_next <unistd.h>
#ifdef LIBC_UNISTD_H
#error "the zedBSD C library headers are on the include path of a host build; define KERN_UAPI_NATIVE"
#endif

#endif
