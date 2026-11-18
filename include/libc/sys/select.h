/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SYS_SELECT_H
#define LIBC_SYS_SELECT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <uapi/select.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>

#define FD_SETSIZE KERN_FD_SETSIZE
#define NFDBITS KERN_NFDBITS

typedef uint32_t fd_mask;

#define __FD_WORD(fd) ((unsigned)(fd) / NFDBITS)
#define __FD_BIT(fd) ((fd_mask)1U << ((unsigned)(fd) % NFDBITS))

#define FD_ZERO(set) do { \
	unsigned __fd_index; \
	for (__fd_index = 0; __fd_index < FD_SETSIZE / NFDBITS; __fd_index++) \
		(set)->fds_bits[__fd_index] = 0U; \
} while (0)
#define FD_SET(fd, set) ((set)->fds_bits[__FD_WORD(fd)] |= __FD_BIT(fd))
#define FD_CLR(fd, set) ((set)->fds_bits[__FD_WORD(fd)] &= ~__FD_BIT(fd))
#define FD_ISSET(fd, set) (((set)->fds_bits[__FD_WORD(fd)] & __FD_BIT(fd)) != 0U)

int select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
int pselect(int, fd_set *, fd_set *, fd_set *, const struct timespec *, const sigset_t *);

#ifdef __cplusplus
}
#endif

#endif
