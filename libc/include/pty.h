/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_PTY_H
#define ZEDBSD_PTY_H
#include <sys/types.h>
struct termios;
struct winsize;
int openpty(int *, int *, char *, const struct termios *,
	const struct winsize *);
pid_t forkpty(int *, char *, const struct termios *,
	const struct winsize *);
int login_tty(int);
#endif
