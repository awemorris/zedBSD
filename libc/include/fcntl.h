/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_FCNTL_H
#define ZEDBSD_FCNTL_H

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_CREAT     0x0100
#define O_EXCL      0x0200
#define O_TRUNC     0x0400
#define O_APPEND    0x0800
#define O_DIRECTORY 0x1000
#define O_NONBLOCK  0x2000
#define O_CLOEXEC   0x4000

#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x0100
#define AT_REMOVEDIR        0x0200
#define AT_EACCESS          0x0400
#define AT_SYMLINK_FOLLOW   0x0800

int faccessat(int, const char *, int, int);

#define FD_CLOEXEC  0x0001

#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 5

#include <sys/types.h>
int open(const char *, int, ...);
int openat(int, const char *, int, ...);
int fcntl(int, int, ...);

#endif
