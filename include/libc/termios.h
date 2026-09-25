/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef KERN_TERMIOS_H
#define KERN_TERMIOS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <uapi/termios.h>
#include <sys/types.h>

#define L_ctermid 13
int tcgetattr(int, struct termios *);
int tcgetwinsize(int, struct winsize *);
int tcsetattr(int, int, const struct termios *);
int tcsetwinsize(int, const struct winsize *);
int tcdrain(int);
int tcflush(int, int);
int tcflow(int, int);
speed_t cfgetispeed(const struct termios *);
speed_t cfgetospeed(const struct termios *);
/*
 * Puts a terminal description into raw mode: no input translation, no output
 * processing, no line editing and no signal generation, delivering each byte
 * as it arrives.  A BSD extension rather than a POSIX one, but portable
 * software that drives a terminal expects it.
 */
void cfmakeraw(struct termios *);

int cfsetispeed(struct termios *, speed_t);
int cfsetospeed(struct termios *, speed_t);
pid_t tcgetpgrp(int);
pid_t tcgetsid(int);
int tcsetpgrp(int, pid_t);
int tcsendbreak(int, int);
char *ctermid(char *);

#ifdef __cplusplus
}
#endif

#endif
