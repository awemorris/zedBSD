/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_UAPI_POLL_H
#define ZEDBSD_UAPI_POLL_H

#include <stdint.h>

typedef uint32_t nfds_t;

struct pollfd {
	int fd;
	short events;
	short revents;
};

#define POLLIN	0x0001
#define POLLPRI	0x0002
#define POLLOUT	0x0004
#define POLLERR	0x0008
#define POLLHUP	0x0010
#define POLLNVAL	0x0020
#define POLLRDNORM	0x0040
#define POLLRDBAND	0x0080
#define POLLWRNORM	0x0100
#define POLLWRBAND	0x0200

#endif
