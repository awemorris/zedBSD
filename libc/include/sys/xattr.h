/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_XATTR_H
#define ZEDBSD_SYS_XATTR_H

#include <stddef.h>
#include <sys/types.h>

#define XATTR_CREATE  0x0001
#define XATTR_REPLACE 0x0002

ssize_t getxattr(const char *, const char *, void *, size_t);
ssize_t lgetxattr(const char *, const char *, void *, size_t);
ssize_t fgetxattr(int, const char *, void *, size_t);
int setxattr(const char *, const char *, const void *, size_t, int);
int lsetxattr(const char *, const char *, const void *, size_t, int);
int fsetxattr(int, const char *, const void *, size_t, int);
ssize_t listxattr(const char *, char *, size_t);
ssize_t llistxattr(const char *, char *, size_t);
ssize_t flistxattr(int, char *, size_t);
int removexattr(const char *, const char *);
int lremovexattr(const char *, const char *);
int fremovexattr(int, const char *);

#endif
