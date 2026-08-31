/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements shared userland service zsv1 client support.
 */

#define _POSIX_C_SOURCE 200809L
#include "userland/base/service/zsv1-client.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int deadline_valid(const struct timespec *deadline);
static int set_nonblocking(int descriptor, int *saved_flags);
static int send_request(int descriptor, const char *request, size_t length, const struct timespec *deadline);
static int wait_ready(int descriptor, short events, const struct timespec *deadline);
static int remaining_milliseconds(const struct timespec *deadline);
static int receive_response(int descriptor, struct zsv1_response *response, const struct timespec *deadline);
static void restore_flags(int descriptor, int saved_flags, int *saved_errno);
static int make_deadline(struct timespec *deadline);
static int connect_until(int descriptor, const struct sockaddr_un *address, const struct timespec *deadline);

/*
 * Implements the zsv1 client exchange fd operation.
 */
int
zsv1_client_exchange_fd(
	int descriptor,
	const struct zsv1_request *request,
	struct zsv1_response *response,
	const struct timespec *deadline)
{
	char wire[ZSV1_REQUEST_MAX + 1U];
	size_t wire_length;
	int saved_flags, saved_errno, result;

	saved_errno = 0;
	result = -1;

	/* Handles a failed deadline valid operation. */
	if (descriptor < 0 || request == NULL || response == NULL ||
	    !deadline_valid(deadline)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed zsv1 request format operation. */
	if (zsv1_request_format(request, wire, sizeof(wire), &wire_length) != 0)
		return -1;

	/* Handles a failed set nonblocking operation. */
	if (set_nonblocking(descriptor, &saved_flags) != 0)
		return -1;

	/* Handles a failed send request operation. */
	if (send_request(descriptor, wire, wire_length, deadline) == 0 &&
	    receive_response(descriptor, response, deadline) == 0)
		result = 0;
	else
		saved_errno = errno;
	restore_flags(descriptor, saved_flags, &saved_errno);

	/* Handles the saved errno condition. */
	if (saved_errno != 0) {
		errno = saved_errno;

		/* Reports operation failure. */
		return -1;
	}

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the zsv1 client call operation.
 */
int
zsv1_client_call(
	const char *path,
	const struct zsv1_request *request,
	struct zsv1_response *response)
{
	struct sockaddr_un address;
	struct timespec deadline;
	int descriptor, result, saved_errno;

	/* Handles the path availability. */
	if (path == NULL || request == NULL || response == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed strlen operation. */
	if (strlen(path) >= sizeof(address.sun_path)) {
		errno = ENAMETOOLONG;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed make deadline operation. */
	if (make_deadline(&deadline) != 0)
		return -1;
	descriptor =
	    socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, path);

	/* Handles a failed connect until operation. */
	if (connect_until(descriptor, &address, &deadline) != 0) {
		saved_errno = errno;
		close(descriptor);
		errno = saved_errno;

		/* Reports operation failure. */
		return -1;
	}
	result =
	    zsv1_client_exchange_fd(descriptor, request, response, &deadline);
	saved_errno = errno;

	/* Handles a failed close operation. */
	if (close(descriptor) != 0 && result == 0) {
		result = -1;
		saved_errno = errno;
	}

	/* Checks the operation result. */
	if (result != 0)
		errno = saved_errno;

	/* Returns the computed result. */
	return result;
}

/* Supports the deadline valid operation. */
static int
deadline_valid(
	const struct timespec *deadline)
{
	/* Returns the computed result. */
	return deadline != NULL && deadline->tv_sec >= 0 &&
	       deadline->tv_nsec >= 0 && deadline->tv_nsec < 1000000000L;
}

/* Supports the set nonblocking operation. */
static int
set_nonblocking(
	int descriptor,
	int *saved_flags)
{
	int flags;

	flags = fcntl(descriptor, F_GETFL);

	/* Checks the active flags. */
	if (flags < 0)
		return -1;

	/* Handles a failed fcntl operation. */
	if (fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0)
		return -1;
	*saved_flags = flags;
	/* Reports successful completion. */
	return 0;
}

/* Supports the send request operation. */
static int
send_request(
	int descriptor,
	const char *request,
	size_t length,
	const struct timespec *deadline)
{
	ssize_t result;
	size_t offset;

	offset = 0;

	/* Process each remaining element. */
	while (offset < length) {
		/* Handles a failed wait ready operation. */
		if (wait_ready(descriptor, POLLOUT, deadline) != 0)
			return -1;
		result = send(descriptor, request + offset, length - offset,
			      MSG_NOSIGNAL);

		/* Checks the operation result. */
		if (result > 0) {
			offset += (size_t)result;
			continue;
		}

		/* Handles the reported system error. */
		if (result < 0 && (errno == EINTR || errno == EAGAIN))
			continue;

		/* Checks the operation result. */
		if (result == 0)
			errno = EPIPE;

		/* Reports operation failure. */
		return -1;
	}
	while (shutdown(descriptor, SHUT_WR) != 0) {
		/* Handles the reported system error. */
		if (errno != EINTR)
			return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the wait ready operation. */
static int
wait_ready(
	int descriptor,
	short events,
	const struct timespec *deadline)
{
	int timeout;
	int result;
	struct pollfd poll_descriptor;

	/* Continue until the operation reaches a terminal state. */
	poll_descriptor.fd = descriptor;
	poll_descriptor.events = events;
	for (;;) {
		timeout = remaining_milliseconds(deadline);

		/* Handles the timeout condition. */
		if (timeout < 0)
			return -1;
		poll_descriptor.revents = 0;
		result = poll(&poll_descriptor, 1, timeout);

		/* Checks the operation result. */
		if (result > 0) {
			/* Handles the poll descriptor condition. */
			if (poll_descriptor.revents & POLLNVAL) {
				errno = EBADF;

				/* Reports operation failure. */
				return -1;
			}

			/* Reports successful completion. */
			return 0;
		}

		/* Checks the operation result. */
		if (result == 0) {
			errno = ETIMEDOUT;

			/* Reports operation failure. */
			return -1;
		}

		/* Handles the reported system error. */
		if (errno != EINTR)
			return -1;
	}
}

/* Supports the remaining milliseconds operation. */
static int
remaining_milliseconds(
	const struct timespec *deadline)
{
	struct timespec now;
	int64_t seconds, nanoseconds, milliseconds;

	/* Handles a failed deadline valid operation. */
	if (!deadline_valid(deadline)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed clock gettime operation. */
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return -1;
	seconds = (int64_t)deadline->tv_sec - (int64_t)now.tv_sec;
	nanoseconds = (int64_t)deadline->tv_nsec - (int64_t)now.tv_nsec;

	/* Handles the nanoseconds condition. */
	if (nanoseconds < 0) {
		seconds--;
		nanoseconds += 1000000000LL;
	}

	/* Handles the seconds condition. */
	if (seconds < 0 || (seconds == 0 && nanoseconds == 0)) {
		errno = ETIMEDOUT;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the seconds condition. */
	if (seconds > INT_MAX / 1000) {
		return INT_MAX;
	}
	milliseconds = seconds * 1000 + (nanoseconds + 999999) / 1000000;

	/* Handles the milliseconds condition. */
	if (milliseconds > INT_MAX)
		return INT_MAX;

	/* Returns the computed result. */
	return (int)milliseconds;
}

/* Supports the receive response operation. */
static int
receive_response(
	int descriptor,
	struct zsv1_response *response,
	const struct timespec *deadline)
{
	ssize_t length;
	struct zsv1_decoder decoder;
	char buffer[512];

	zsv1_decoder_init(&decoder);

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles a failed wait ready operation. */
		if (wait_ready(descriptor, POLLIN, deadline) != 0)
			return -1;
		length = recv(descriptor, buffer, sizeof(buffer), 0);

		/* Checks the current data length. */
		if (length > 0) {
			/* Handles a failed zsv1 decoder feed operation. */
			if (zsv1_decoder_feed(&decoder, buffer,
					      (size_t)length) != 0)

				/* Reports operation failure. */
				return -1;
			continue;
		}

		/* Checks the current data length. */
		if (length == 0)
			break;

		/* Handles the reported system error. */
		if (errno != EINTR && errno != EAGAIN)
			return -1;
	}

	/* Handles a failed zsv1 decoder finish operation. */
	if (zsv1_decoder_finish(&decoder) != 0)
		return -1;
	*response = decoder.response;
	/* Reports successful completion. */
	return 0;
}

/* Supports the restore flags operation. */
static void
restore_flags(
	int descriptor,
	int saved_flags,
	int *saved_errno)
{
	/* Handles a failed fcntl operation. */
	if (fcntl(descriptor, F_SETFL, saved_flags) != 0 && *saved_errno == 0)
		*saved_errno = errno;
}

/* Supports the make deadline operation. */
static int
make_deadline(
	struct timespec *deadline)
{
	/* Handles a failed clock gettime operation. */
	if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0)
		return -1;

	/* Handles the uint64 t condition. */
	if ((uint64_t)deadline->tv_sec >
	    (uint64_t)INT64_MAX - ZSV1_CLIENT_TIMEOUT_SECONDS) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	deadline->tv_sec += ZSV1_CLIENT_TIMEOUT_SECONDS;

	/* Reports successful completion. */
	return 0;
}

/* Supports the connect until operation. */
static int
connect_until(
	int descriptor,
	const struct sockaddr_un *address,
	const struct timespec *deadline)
{
	int socket_error;
	socklen_t length;
	int result;

	result = connect(descriptor, (const struct sockaddr *)address,
			 sizeof(*address));

	/* Checks the operation result. */
	if (result == 0)
		return 0;

	/* Handles the reported system error. */
	if (errno != EINPROGRESS && errno != EAGAIN)
		return -1;

	/* Handles a failed wait ready operation. */
	if (wait_ready(descriptor, POLLOUT, deadline) != 0)
		return -1;

	socket_error = 0;
	length = sizeof(socket_error);

	/* Handles an operation failure. */
	if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socket_error,
		       &length) != 0)

		/* Reports operation failure. */
		return -1;

	/* Handles an operation failure. */
	if (socket_error != 0) {
		errno = socket_error;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}
