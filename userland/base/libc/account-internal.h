/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sizes the C library's account lookups are built around, shared by the
 * lookups themselves (account.c) and by sysconf(), which reports them to a
 * caller that sizes its own buffer.
 */

#ifndef LIBC_ACCOUNT_INTERNAL_H
#define LIBC_ACCOUNT_INTERNAL_H

/*
 * The buffer one password, group or shadow entry is decoded into.  getpwnam(),
 * getgrnam() and getspnam() keep one of these each, so an entry they can
 * return always fits a buffer of this size handed to the _r variants.
 */
#define ACCOUNT_RESULT_MAX 2048

#endif
