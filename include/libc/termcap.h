/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The termcap interface, over the terminfo descriptions the system has.
 *
 * Programs written for termcap (vim, emacs, readline) ask for capabilities
 * by their two-letter termcap codes.  These calls translate the code to the
 * terminfo name and answer from the description setupterm() loads; there is
 * no termcap database.  The functions live in the curses library.
 */

#ifndef LIBC_TERMCAP_H
#define LIBC_TERMCAP_H

#ifdef __cplusplus
extern "C" {
#endif

/* The pad character, the cursor-up and backspace strings, and the line speed. */
extern char PC;
extern char *UP;
extern char *BC;
extern short ospeed;

int tgetent(char *, const char *);
int tgetflag(const char *);
int tgetnum(const char *);
char *tgetstr(const char *, char **);
char *tgoto(const char *, int, int);
int tputs(const char *, int, int (*)(int));

#ifdef __cplusplus
}
#endif

#endif
