/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

#endif
