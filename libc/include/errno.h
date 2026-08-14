/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_ERRNO_H
#define ZEDBSD_ERRNO_H

extern int zedbsd_errno;
#define errno zedbsd_errno

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

#endif
