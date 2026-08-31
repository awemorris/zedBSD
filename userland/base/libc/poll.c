/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library poll support.
 */

#include "userland/base/libc/syscall.h"

#include <zedbsd/syscall.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <sys/select.h>

extern void __pthread_cancel_point(void) __attribute__((weak));

static void cancel_point(void);
static intptr_t call(uint32_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5);

/*
 * Implements the ppoll operation.
 */
int
ppoll(
	struct pollfd *fds,
	nfds_t count,
	const struct timespec *timeout,
	const sigset_t *mask)
{
	int result;

	cancel_point();
	result = (int)call(ZEDBSD_SYS_ppoll, (uintptr_t)fds, count,
			   (uintptr_t)timeout, (uintptr_t)mask, 0, 0);
	cancel_point();

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the poll operation.
 */
int
poll(
	struct pollfd *fds,
	nfds_t count,
	int timeout_ms)
{
	int function_result;
	struct timespec timeout;

	/* Handles the timeout ms condition. */
	if (timeout_ms < -1) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the timeout ms condition. */
	if (timeout_ms == -1) {
		/* Obtains the ppoll result. */
		function_result = ppoll(fds, count, NULL, NULL);

		/* Returns the computed result. */
		return function_result;
	}
	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;

	/* Obtains the ppoll result. */
	function_result = ppoll(fds, count, &timeout, NULL);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the pselect operation.
 */
int
pselect(
	int nfds,
	fd_set *readfds,
	fd_set *writefds,
	fd_set *exceptfds,
	const struct timespec *timeout,
	const sigset_t *mask)
{
	int result;

	cancel_point();
	result =
	    (int)call(ZEDBSD_SYS_pselect, (uintptr_t)nfds, (uintptr_t)readfds,
		      (uintptr_t)writefds, (uintptr_t)exceptfds,
		      (uintptr_t)timeout, (uintptr_t)mask);
	cancel_point();

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the select operation.
 */
int
select(
	int nfds,
	fd_set *readfds,
	fd_set *writefds,
	fd_set *exceptfds,
	struct timeval *timeout)
{
	int function_result;
	struct timespec timespec;
	const struct timespec *pointer;

	pointer = NULL;

	/* Handles the timeout availability. */
	if (timeout != NULL) {
		/* Handles the timeout condition. */
		if (timeout->tv_sec < 0 || timeout->tv_usec < 0 ||
		    timeout->tv_usec >= 1000000L) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}
		timespec.tv_sec = timeout->tv_sec;
		timespec.tv_nsec = timeout->tv_usec * 1000L;
		pointer = &timespec;
	}

	/* Obtains the pselect result. */
	function_result = pselect(nfds, readfds, writefds, exceptfds, pointer, NULL);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the cancel point operation. */
static void
cancel_point(
	void)
{
	/* Handles the pthread cancel point availability. */
	if (__pthread_cancel_point != NULL)
		__pthread_cancel_point();
}

/* Supports the call operation. */
static intptr_t
call(
	uint32_t number,
	uintptr_t a0,
	uintptr_t a1,
	uintptr_t a2,
	uintptr_t a3,
	uintptr_t a4,
	uintptr_t a5)
{
	intptr_t function_result;

	/* Obtains the syscall result result. */
	function_result = syscall_result(__syscall6(number, a0, a1, a2, a3, a4, a5));

	/* Returns the computed result. */
	return function_result;
}
