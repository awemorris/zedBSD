/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The terminfo interface of the curses library, and its termcap interface.
 */

#ifndef LIBC_TERM_H
#define LIBC_TERM_H

#include <termcap.h>

#ifdef __cplusplus
extern "C" {
#endif

int setupterm(const char *, int, int *);
int tigetflag(const char *);
int tigetnum(const char *);
char *tigetstr(const char *);
char *tparm(const char *, ...);
char *tiparm(const char *, ...);
int putp(const char *);

#ifdef __cplusplus
}
#endif

#endif
