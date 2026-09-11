/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_POLL_H
#define LIBC_POLL_H

#include <uapi/poll.h>
#include <signal.h>
#include <time.h>

int poll(struct pollfd *, nfds_t, int);
int ppoll(struct pollfd *, nfds_t, const struct timespec *,
	const sigset_t *);

#endif
