/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/libc/syscall.h"

#include <zedbsd/syscall.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <sys/select.h>

extern void zedbsd_pthread_cancel_point(void) __attribute__((weak));
static void cancel_point(void)
{ if (zedbsd_pthread_cancel_point != NULL) zedbsd_pthread_cancel_point(); }

static intptr_t
call(uint32_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2,
	uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
	return zedbsd_syscall_result(zedbsd_syscall6(number, a0, a1, a2,
	    a3, a4, a5));
}

int
ppoll(struct pollfd *fds, nfds_t count, const struct timespec *timeout,
	const sigset_t *mask)
{
	int result;
	cancel_point();
	result = (int)call(ZEDBSD_SYS_ppoll, (uintptr_t)fds, count,
	    (uintptr_t)timeout, (uintptr_t)mask, 0, 0);
	cancel_point();
	return result;
}

int
poll(struct pollfd *fds, nfds_t count, int timeout_ms)
{
	struct timespec timeout;

	if (timeout_ms < -1) {
		errno = EINVAL;
		return -1;
	}
	if (timeout_ms == -1)
		return ppoll(fds, count, NULL, NULL);
	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
	return ppoll(fds, count, &timeout, NULL);
}

int
pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
	const struct timespec *timeout, const sigset_t *mask)
{
	int result;
	cancel_point();
	result = (int)call(ZEDBSD_SYS_pselect, (uintptr_t)nfds,
	    (uintptr_t)readfds, (uintptr_t)writefds, (uintptr_t)exceptfds,
	    (uintptr_t)timeout, (uintptr_t)mask);
	cancel_point();
	return result;
}

int
select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
	struct timeval *timeout)
{
	struct timespec timespec;
	const struct timespec *pointer = NULL;

	if (timeout != NULL) {
		if (timeout->tv_sec < 0 || timeout->tv_usec < 0 ||
		    timeout->tv_usec >= 1000000L) {
			errno = EINVAL;
			return -1;
		}
		timespec.tv_sec = timeout->tv_sec;
		timespec.tv_nsec = timeout->tv_usec * 1000L;
		pointer = &timespec;
	}
	return pselect(nfds, readfds, writefds, exceptfds, pointer, NULL);
}
