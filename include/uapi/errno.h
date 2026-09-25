/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The error numbers of the zedBSD system interface.
 *
 * The kernel returns them and the C library stores them in errno, so both
 * read the one definition here.  <errno.h> of the C library includes this and
 * adds errno itself.
 */

#include <uapi/hosted.h>

#if !KERN_UAPI_HOST_LIBC
#ifndef KERN_UAPI_ERRNO_H
#define KERN_UAPI_ERRNO_H

#define EDOM 1
#define ERANGE 2
#define EINVAL 3
#define ENOMEM 4
#define EIO 5
#define ENOENT 6
#define EINTR 7
#define ENOSPC 8
#define EROFS 9
#define EOVERFLOW 10
#define ENAMETOOLONG 11
#define ENXIO 12
#define ENODEV 13
#define ENOTDIR 14
#define EISDIR 15
#define EEXIST 16
#define EBUSY 17
#define ENOTEMPTY 18
#define EBADF 19
#define ENOSYS 20
#define EOPNOTSUPP 21
#define ENOEXEC 22
#define EFAULT 23
#define EAGAIN 24
#define EACCES 25
#define ESRCH 26
#define ECHILD 27
#define E2BIG 28
#define ENFILE 29
#define EMSGSIZE 30
#define ENOBUFS 31
#define ENETDOWN 32
#define ENETUNREACH 33
#define EPROTONOSUPPORT 34
#define EAFNOSUPPORT 35

/*
 * A protocol family that this system does not carry.  It is the same
 * condition as an unsupported address family here, and has the same value,
 * as it does on the systems this name comes from.
 */
#define EPFNOSUPPORT 68
#define ETXTBSY 69
#define EBADMSG 70
#define EMULTIHOP 71
#define ENETRESET 72
#define ENOLCK 73
#define ENOLINK 74
#define ENOSR 75
#define ENOSTR 76
#define ENOTRECOVERABLE 77
#define EOWNERDEAD 78
#define EPROTOTYPE 79
#define ESOCKTNOSUPPORT 80
#define ETIME 81
#define EADDRINUSE 36
#define EADDRNOTAVAIL 37
#define EISCONN 38
#define ENOTCONN 39
#define ECONNREFUSED 40
#define ECONNRESET 41
#define ETIMEDOUT 42
#define EHOSTUNREACH 43
#define EPIPE 44
#define EDESTADDRREQ 45
#define EMFILE 46
#define EPERM 47
#define EXDEV 48
#define ESPIPE 49
#define ELOOP 50
#define EFBIG 51
#define ENOTTY 52
#define EINPROGRESS 53
#define EALREADY 54
#define ECONNABORTED 55
#define ENOPROTOOPT 56
#define EMLINK 57
#define EDEADLK 58
#define ECANCELED 59
#define ENOTSOCK 60
#define EILSEQ 61
#define ENODATA 62
#define EDQUOT 63
#define ENOTSUP EOPNOTSUPP
#define ENOMSG 64
#define EIDRM 65
#define ESTALE 66
#define EPROTO 67

#define EWOULDBLOCK EAGAIN

#endif

/*
 * A host fixture that asks for zedBSD's ABI (KERN_UAPI_NATIVE) and puts
 * include/uapi itself on the include path reaches this header under the
 * standard name <errno.h>; the zedBSD C library's <errno.h>, later on the
 * path, then still supplies the rest of the standard header.  The chain is
 * taken only when the zedBSD C library headers do come later (rtld-abi.h is
 * one of them and exists nowhere else), so it never reaches the host's
 * header.  A zedBSD build never chains: the kernel has no such header, and
 * the C library's includes this one.
 */
#if !defined(__ZEDBSD__) && !defined(LIBC_ERRNO_H) && defined(__has_include_next)
#if __has_include_next(<rtld-abi.h>) && __has_include_next(<errno.h>)
#pragma GCC system_header
#include_next <errno.h>
#endif
#endif
#else

/*
 * A host fixture compiles kernel source against the host C library, whose
 * error numbers the fixture's own calls return.  The host's <errno.h> is the
 * next one on the include path; #include_next reaches it even when this
 * directory is itself on the path under the standard name.  The pragma keeps
 * the extension quiet in a -pedantic fixture.
 */
#pragma GCC system_header
#include_next <errno.h>
#ifdef LIBC_ERRNO_H
#error "the zedBSD C library headers are on the include path of a host build; define KERN_UAPI_NATIVE"
#endif

#endif
