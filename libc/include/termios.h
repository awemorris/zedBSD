/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_TERMIOS_H
#define ZEDBSD_TERMIOS_H

#include <zedbsd/termios.h>
#include <sys/types.h>

#define L_ctermid 13
int tcgetattr(int, struct termios *);
int tcsetattr(int, int, const struct termios *);
int tcdrain(int);
int tcflush(int, int);
int tcflow(int, int);
speed_t cfgetispeed(const struct termios *);
speed_t cfgetospeed(const struct termios *);
int cfsetispeed(struct termios *, speed_t);
int cfsetospeed(struct termios *, speed_t);
pid_t tcgetpgrp(int);
int tcsetpgrp(int, pid_t);
char *ctermid(char *);

#endif
