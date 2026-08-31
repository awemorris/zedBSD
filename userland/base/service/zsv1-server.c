/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements shared userland service zsv1 server support.
 */

#define _POSIX_C_SOURCE 200809L
#include "userland/base/service/zsv1-server.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

static int deadline_valid(const struct timespec *deadline);
static int set_nonblocking(int descriptor, int *saved_flags);
static int wait_readable(int descriptor, const struct timespec *deadline);
static int remaining_milliseconds(const struct timespec *deadline);
static void restore_flags(int descriptor, int saved_flags, int *saved_errno);
static int send_all(int descriptor, const void *data, size_t length);
static int dependency_list_validate(const char *list, size_t *count_result);

/*
 * Implements the zsv1 server receive fd operation.
 */
int
zsv1_server_receive_fd(
	int descriptor,
	struct zsv1_request *request,
	const struct timespec *deadline)
{
	void *buffer;
	size_t capacity;
	ssize_t count;
	unsigned char wire[ZSV1_REQUEST_MAX], extra;
	size_t used;
	int saved_flags, saved_errno, result;

	used = 0;
	saved_errno = 0;
	result = -1;

	/* Handles a failed deadline valid operation. */
	if (descriptor < 0 || request == NULL || !deadline_valid(deadline)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed set nonblocking operation. */
	if (set_nonblocking(descriptor, &saved_flags) != 0)
		return -1;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {

		buffer = used < sizeof(wire) ? wire + used : &extra;
		capacity = used < sizeof(wire) ? sizeof(wire) - used : 1U;

		/* Handles a failed wait readable operation. */
		if (wait_readable(descriptor, deadline) != 0) {
			saved_errno = errno;
			break;
		}
		count = recv(descriptor, buffer, capacity, 0);

		/* Checks the remaining item count. */
		if (count > 0) {
			/* Checks the current capacity usage. */
			if (used == sizeof(wire)) {
				saved_errno = EMSGSIZE;
				break;
			}
			used += (size_t)count;
			continue;
		}

		/* Checks the remaining item count. */
		if (count == 0) {
			/* Handles a failed zsv1 request parse operation. */
			if (zsv1_request_parse(wire, used, request) == 0)
				result = 0;
			else
				saved_errno = errno;
			break;
		}

		/* Handles the reported system error. */
		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
			continue;
		saved_errno = errno;
		break;
	}
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
 * Implements the zsv1 server send record fd operation.
 */
int
zsv1_server_send_record_fd(
	int descriptor,
	const struct zsv1_record *record)
{
	int function_result;
	char line[ZSV1_RESPONSE_LINE_MAX + 1U];
	size_t length;

	/* Handles the record availability. */
	if (descriptor < 0 || record == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed zsv1 record format operation. */
	if (zsv1_record_format(record, line, sizeof(line), &length) != 0)
		return -1;

	/* Obtains the send all result. */
	function_result = send_all(descriptor, line, length);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the zsv1 server send end fd operation.
 */
int
zsv1_server_send_end_fd(
	int descriptor)
{
	int function_result;
	struct zsv1_record record;

	memset(&record, 0, sizeof(record));
	record.type = ZSV1_RECORD_END;

	/* Obtains the zsv1 server send record fd result. */
	function_result = zsv1_server_send_record_fd(descriptor, &record);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the zsv1 server send ok end fd operation.
 */
int
zsv1_server_send_ok_end_fd(
	int descriptor,
	const char *token)
{
	int function_result;
	struct zsv1_record record;

	/* Handles a failed strlen operation. */
	if (token == NULL || strlen(token) >= sizeof(record.token)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	memset(&record, 0, sizeof(record));
	record.type = ZSV1_RECORD_OK;
	strcpy(record.token, token);

	/* Handles a failed zsv1 server send record fd operation. */
	if (zsv1_server_send_record_fd(descriptor, &record) != 0)
		return -1;

	/* Obtains the zsv1 server send end fd result. */
	function_result = zsv1_server_send_end_fd(descriptor);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the zsv1 server send error end fd operation.
 */
int
zsv1_server_send_error_end_fd(
	int descriptor,
	int error,
	const char *reason)
{
	int function_result;
	struct zsv1_record record;

	/* Handles an operation failure. */
	if (error <= 0 || reason == NULL ||
	    strlen(reason) >= sizeof(record.token)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	memset(&record, 0, sizeof(record));
	record.type = ZSV1_RECORD_ERROR;
	record.error_number = error;
	strcpy(record.token, reason);

	/* Handles a failed zsv1 server send record fd operation. */
	if (zsv1_server_send_record_fd(descriptor, &record) != 0)
		return -1;

	/* Obtains the zsv1 server send end fd result. */
	function_result = zsv1_server_send_end_fd(descriptor);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the zsv1 server dependency lists validate operation.
 */
int
zsv1_server_dependency_lists_validate(
	const char *after,
	const char *requires,
	size_t *after_count,
	size_t *requires_count)
{
	size_t after_local, requires_local;

	/* Handles the after count availability. */
	if (after_count == NULL || requires_count == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed dependency list validate operation. */
	if (dependency_list_validate(after, &after_local) != 0 ||
	    dependency_list_validate(requires, &requires_local) != 0)

		/* Reports operation failure. */
		return -1;

	/* Handles the after local condition. */
	if (after_local > ZSV1_DEPENDENCY_MAX - requires_local) {
		errno = EMSGSIZE;

		/* Reports operation failure. */
		return -1;
	}
	*after_count = after_local;
	*requires_count = requires_local;
	/* Reports successful completion. */
	return 0;
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

/* Supports the wait readable operation. */
static int
wait_readable(
	int descriptor,
	const struct timespec *deadline)
{
	int timeout;
	int result;
	struct pollfd poll_descriptor;

	/* Continue until the operation reaches a terminal state. */
	poll_descriptor.fd = descriptor;
	poll_descriptor.events = POLLIN;
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
	if (seconds > INT_MAX / 1000)
		return INT_MAX;
	milliseconds = seconds * 1000 + (nanoseconds + 999999) / 1000000;

	/* Handles the milliseconds condition. */
	if (milliseconds > INT_MAX)
		return INT_MAX;

	/* Returns the computed result. */
	return (int)milliseconds;
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

/* Supports the send all operation. */
static int
send_all(
	int descriptor,
	const void *data,
	size_t length)
{
	ssize_t count;
	const unsigned char *bytes;
	size_t sent;

	bytes = data;
	sent = 0;

	/* Process each remaining element. */
	while (sent < length) {

		count = send(descriptor, bytes + sent, length - sent, MSG_NOSIGNAL);

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count <= 0) {
			/* Checks the remaining item count. */
			if (count == 0)
				errno = EPIPE;

			/* Reports operation failure. */
			return -1;
		}
		sent += (size_t)count;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the dependency list validate operation. */
static int
dependency_list_validate(
	const char *list,
	size_t *count_result)
{
	const char *comma;
	size_t index;
	char tokens[ZSV1_DEPENDENCY_MAX][ZSV1_NAME_CAPACITY];
	const char *cursor;
	size_t count;

	count = 0;

	/* Handles the list availability. */
	if (list == NULL || count_result == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the list condition. */
	if (*list == '\0') {
		*count_result = 0;
		/* Reports successful completion. */
		return 0;
	}

	/* Continue until the operation reaches a terminal state. */
	cursor = list;
	for (;;) {
				comma = strchr(cursor, ',');
		size_t length =
		    comma != NULL ? (size_t)(comma - cursor) : strlen(cursor);

		/* Checks the current data length. */
		if (length == 0 || length >= ZSV1_NAME_CAPACITY) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}

		/* Checks the remaining item count. */
		if (count == ZSV1_DEPENDENCY_MAX) {
			errno = EMSGSIZE;

			/* Reports operation failure. */
			return -1;
		}
		memcpy(tokens[count], cursor, length);
		tokens[count][length] = '\0';

		/* Handles a failed zsv1 name valid operation. */
		if (!zsv1_name_valid(tokens[count])) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}

		/* Process each remaining element. */
		for (index = 0; index < count; index++) {
			/* Selects the matching value. */
			if (strcmp(tokens[index], tokens[count]) == 0) {
				errno = EINVAL;

				/* Reports operation failure. */
				return -1;
			}
		}
		count++;

		/* Handles the comma availability. */
		if (comma == NULL)
			break;
		cursor = comma + 1;
	}
	*count_result = count;
	/* Reports successful completion. */
	return 0;
}
