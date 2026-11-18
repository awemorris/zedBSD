/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_UAPI_SELECT_H
#define KERN_UAPI_SELECT_H

#include <stdint.h>

/*
 * A descriptor set holds FD_SETSIZE bits, the same count as other POSIX
 * systems.  select() reads and writes only the words that hold descriptors
 * below its nfds argument.
 */
#define KERN_FD_SETSIZE	1024
#define KERN_NFDBITS	32

typedef struct fd_set {
	uint32_t fds_bits[KERN_FD_SETSIZE / KERN_NFDBITS];
} fd_set;

#endif
