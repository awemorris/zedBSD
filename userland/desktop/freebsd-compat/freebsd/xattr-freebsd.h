/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Declares the native compatibility subset used by file information, tags and metadata copies. */
#ifndef KEILAND_XATTR_FREEBSD_H
#define KEILAND_XATTR_FREEBSD_H

#include <sys/types.h>
#include <stddef.h>
#include <errno.h>

/* The shared tag caller's no-data spelling denotes FreeBSD's actual missing-attribute errno. */
#define ENODATA ENOATTR

/* Native namespaces appear as user.NAME/system.NAME; get/list size zero performs a size inquiry. */
ssize_t getxattr(const char *path, const char *name, void *value, size_t capacity);
ssize_t lgetxattr(const char *path, const char *name, void *value, size_t capacity);
ssize_t fgetxattr(int descriptor, const char *name, void *value, size_t capacity);
/* Existing callers use flags zero; every other flag is explicitly refused as unsupported. */
int setxattr(const char *path, const char *name, const void *value, size_t size, int flags);
int fsetxattr(int descriptor, const char *name, const void *value, size_t size, int flags);
int removexattr(const char *path, const char *name);
/* Enumeration translates native length-prefixed entries into complete NUL-separated names. */
ssize_t flistxattr(int descriptor, char *names, size_t capacity);
ssize_t llistxattr(const char *path, char *names, size_t capacity);

#endif
