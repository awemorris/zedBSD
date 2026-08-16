/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_SELECT_H
#define ZEDBSD_SYS_SELECT_H

#include <zedbsd/select.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>

#define FD_SETSIZE ZEDBSD_FD_SETSIZE
typedef zedbsd_fd_set_t fd_set;

#define FD_ZERO(set) ((set)->bits[0] = 0U)
#define FD_SET(fd, set) ((set)->bits[0] |= (uint32_t)1U << (unsigned)(fd))
#define FD_CLR(fd, set) ((set)->bits[0] &= ~((uint32_t)1U << (unsigned)(fd)))
#define FD_ISSET(fd, set) (((set)->bits[0] & ((uint32_t)1U << (unsigned)(fd))) != 0U)

int select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
int pselect(int, fd_set *, fd_set *, fd_set *, const struct timespec *,
	const sigset_t *);

#endif
