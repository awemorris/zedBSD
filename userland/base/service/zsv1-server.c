/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static int
deadline_valid(const struct timespec *deadline)
{
	return deadline != NULL && deadline->tv_sec >= 0 &&
	       deadline->tv_nsec >= 0 && deadline->tv_nsec < 1000000000L;
}

static int
remaining_milliseconds(const struct timespec *deadline)
{
	struct timespec now;
	int64_t seconds, nanoseconds, milliseconds;

	if (!deadline_valid(deadline)) {
		errno = EINVAL;
		return -1;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return -1;
	seconds = (int64_t)deadline->tv_sec - (int64_t)now.tv_sec;
	nanoseconds = (int64_t)deadline->tv_nsec - (int64_t)now.tv_nsec;
	if (nanoseconds < 0) {
		seconds--;
		nanoseconds += 1000000000LL;
	}
	if (seconds < 0 || (seconds == 0 && nanoseconds == 0)) {
		errno = ETIMEDOUT;
		return -1;
	}
	if (seconds > INT_MAX / 1000)
		return INT_MAX;
	milliseconds = seconds * 1000 + (nanoseconds + 999999) / 1000000;
	if (milliseconds > INT_MAX)
		return INT_MAX;
	return (int)milliseconds;
}

static int
wait_readable(int descriptor, const struct timespec *deadline)
{
	struct pollfd poll_descriptor;

	poll_descriptor.fd = descriptor;
	poll_descriptor.events = POLLIN;
	for (;;) {
		int timeout = remaining_milliseconds(deadline);
		int result;

		if (timeout < 0)
			return -1;
		poll_descriptor.revents = 0;
		result = poll(&poll_descriptor, 1, timeout);
		if (result > 0) {
			if (poll_descriptor.revents & POLLNVAL) {
				errno = EBADF;
				return -1;
			}
			return 0;
		}
		if (result == 0) {
			errno = ETIMEDOUT;
			return -1;
		}
		if (errno != EINTR)
			return -1;
	}
}

static int
set_nonblocking(int descriptor, int *saved_flags)
{
	int flags = fcntl(descriptor, F_GETFL);

	if (flags < 0)
		return -1;
	if (fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0)
		return -1;
	*saved_flags = flags;
	return 0;
}

static void
restore_flags(int descriptor, int saved_flags, int *saved_errno)
{
	if (fcntl(descriptor, F_SETFL, saved_flags) != 0 && *saved_errno == 0)
		*saved_errno = errno;
}

int
zsv1_server_receive_fd(int descriptor, struct zsv1_request *request,
		       const struct timespec *deadline)
{
	unsigned char wire[ZSV1_REQUEST_MAX], extra;
	size_t used = 0;
	int saved_flags, saved_errno = 0, result = -1;

	if (descriptor < 0 || request == NULL || !deadline_valid(deadline)) {
		errno = EINVAL;
		return -1;
	}
	if (set_nonblocking(descriptor, &saved_flags) != 0)
		return -1;
	for (;;) {
		void *buffer = used < sizeof(wire) ? wire + used : &extra;
		size_t capacity =
		    used < sizeof(wire) ? sizeof(wire) - used : 1U;
		ssize_t count;

		if (wait_readable(descriptor, deadline) != 0) {
			saved_errno = errno;
			break;
		}
		count = recv(descriptor, buffer, capacity, 0);
		if (count > 0) {
			if (used == sizeof(wire)) {
				saved_errno = EMSGSIZE;
				break;
			}
			used += (size_t)count;
			continue;
		}
		if (count == 0) {
			if (zsv1_request_parse(wire, used, request) == 0)
				result = 0;
			else
				saved_errno = errno;
			break;
		}
		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
			continue;
		saved_errno = errno;
		break;
	}
	restore_flags(descriptor, saved_flags, &saved_errno);
	if (saved_errno != 0) {
		errno = saved_errno;
		return -1;
	}
	return result;
}

static int
send_all(int descriptor, const void *data, size_t length)
{
	const unsigned char *bytes = data;
	size_t sent = 0;

	while (sent < length) {
		ssize_t count =
		    send(descriptor, bytes + sent, length - sent, MSG_NOSIGNAL);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (count == 0)
				errno = EPIPE;
			return -1;
		}
		sent += (size_t)count;
	}
	return 0;
}

int
zsv1_server_send_record_fd(int descriptor, const struct zsv1_record *record)
{
	char line[ZSV1_RESPONSE_LINE_MAX + 1U];
	size_t length;

	if (descriptor < 0 || record == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (zsv1_record_format(record, line, sizeof(line), &length) != 0)
		return -1;
	return send_all(descriptor, line, length);
}

int
zsv1_server_send_end_fd(int descriptor)
{
	struct zsv1_record record;

	memset(&record, 0, sizeof(record));
	record.type = ZSV1_RECORD_END;
	return zsv1_server_send_record_fd(descriptor, &record);
}

int
zsv1_server_send_ok_end_fd(int descriptor, const char *token)
{
	struct zsv1_record record;

	if (token == NULL || strlen(token) >= sizeof(record.token)) {
		errno = EINVAL;
		return -1;
	}
	memset(&record, 0, sizeof(record));
	record.type = ZSV1_RECORD_OK;
	strcpy(record.token, token);
	if (zsv1_server_send_record_fd(descriptor, &record) != 0)
		return -1;
	return zsv1_server_send_end_fd(descriptor);
}

int
zsv1_server_send_error_end_fd(int descriptor, int error, const char *reason)
{
	struct zsv1_record record;

	if (error <= 0 || reason == NULL ||
	    strlen(reason) >= sizeof(record.token)) {
		errno = EINVAL;
		return -1;
	}
	memset(&record, 0, sizeof(record));
	record.type = ZSV1_RECORD_ERROR;
	record.error_number = error;
	strcpy(record.token, reason);
	if (zsv1_server_send_record_fd(descriptor, &record) != 0)
		return -1;
	return zsv1_server_send_end_fd(descriptor);
}

static int
dependency_list_validate(const char *list, size_t *count_result)
{
	char tokens[ZSV1_DEPENDENCY_MAX][ZSV1_NAME_CAPACITY];
	const char *cursor;
	size_t count = 0;

	if (list == NULL || count_result == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (*list == '\0') {
		*count_result = 0;
		return 0;
	}
	cursor = list;
	for (;;) {
		const char *comma = strchr(cursor, ',');
		size_t length =
		    comma != NULL ? (size_t)(comma - cursor) : strlen(cursor);
		size_t index;

		if (length == 0 || length >= ZSV1_NAME_CAPACITY) {
			errno = EINVAL;
			return -1;
		}
		if (count == ZSV1_DEPENDENCY_MAX) {
			errno = EMSGSIZE;
			return -1;
		}
		memcpy(tokens[count], cursor, length);
		tokens[count][length] = '\0';
		if (!zsv1_name_valid(tokens[count])) {
			errno = EINVAL;
			return -1;
		}
		for (index = 0; index < count; index++) {
			if (strcmp(tokens[index], tokens[count]) == 0) {
				errno = EINVAL;
				return -1;
			}
		}
		count++;
		if (comma == NULL)
			break;
		cursor = comma + 1;
	}
	*count_result = count;
	return 0;
}

int
zsv1_server_dependency_lists_validate(const char *after, const char *requires,
				      size_t *after_count,
				      size_t *requires_count)
{
	size_t after_local, requires_local;

	if (after_count == NULL || requires_count == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (dependency_list_validate(after, &after_local) != 0 ||
	    dependency_list_validate(requires, &requires_local) != 0)
		return -1;
	if (after_local > ZSV1_DEPENDENCY_MAX - requires_local) {
		errno = EMSGSIZE;
		return -1;
	}
	*after_count = after_local;
	*requires_count = requires_local;
	return 0;
}
