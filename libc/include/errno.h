/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef BOOTS_ERRNO_H
#define BOOTS_ERRNO_H

extern int boots_errno;
#define errno boots_errno

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

#endif
