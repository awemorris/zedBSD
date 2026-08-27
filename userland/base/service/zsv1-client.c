/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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
	if (seconds > INT_MAX / 1000) {
		return INT_MAX;
	}
	milliseconds = seconds * 1000 + (nanoseconds + 999999) / 1000000;
	if (milliseconds > INT_MAX)
		return INT_MAX;
	return (int)milliseconds;
}

static int
wait_ready(int descriptor, short events, const struct timespec *deadline)
{
	struct pollfd poll_descriptor;

	poll_descriptor.fd = descriptor;
	poll_descriptor.events = events;
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
	int flags;

	flags = fcntl(descriptor, F_GETFL);
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

static int
send_request(int descriptor, const char *request, size_t length,
	     const struct timespec *deadline)
{
	size_t offset = 0;

	while (offset < length) {
		ssize_t result;

		if (wait_ready(descriptor, POLLOUT, deadline) != 0)
			return -1;
		result = send(descriptor, request + offset, length - offset,
			      MSG_NOSIGNAL);
		if (result > 0) {
			offset += (size_t)result;
			continue;
		}
		if (result < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (result == 0)
			errno = EPIPE;
		return -1;
	}
	while (shutdown(descriptor, SHUT_WR) != 0) {
		if (errno != EINTR)
			return -1;
	}
	return 0;
}

static int
receive_response(int descriptor, struct zsv1_response *response,
		 const struct timespec *deadline)
{
	struct zsv1_decoder decoder;
	char buffer[512];

	zsv1_decoder_init(&decoder);
	for (;;) {
		ssize_t length;

		if (wait_ready(descriptor, POLLIN, deadline) != 0)
			return -1;
		length = recv(descriptor, buffer, sizeof(buffer), 0);
		if (length > 0) {
			if (zsv1_decoder_feed(&decoder, buffer,
					      (size_t)length) != 0)
				return -1;
			continue;
		}
		if (length == 0)
			break;
		if (errno != EINTR && errno != EAGAIN)
			return -1;
	}
	if (zsv1_decoder_finish(&decoder) != 0)
		return -1;
	*response = decoder.response;
	return 0;
}

int
zsv1_client_exchange_fd(int descriptor, const struct zsv1_request *request,
			struct zsv1_response *response,
			const struct timespec *deadline)
{
	char wire[ZSV1_REQUEST_MAX + 1U];
	size_t wire_length;
	int saved_flags, saved_errno = 0, result = -1;

	if (descriptor < 0 || request == NULL || response == NULL ||
	    !deadline_valid(deadline)) {
		errno = EINVAL;
		return -1;
	}
	if (zsv1_request_format(request, wire, sizeof(wire), &wire_length) != 0)
		return -1;
	if (set_nonblocking(descriptor, &saved_flags) != 0)
		return -1;
	if (send_request(descriptor, wire, wire_length, deadline) == 0 &&
	    receive_response(descriptor, response, deadline) == 0)
		result = 0;
	else
		saved_errno = errno;
	restore_flags(descriptor, saved_flags, &saved_errno);
	if (saved_errno != 0) {
		errno = saved_errno;
		return -1;
	}
	return result;
}

static int
make_deadline(struct timespec *deadline)
{
	if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0)
		return -1;
	if ((uint64_t)deadline->tv_sec >
	    (uint64_t)INT64_MAX - ZSV1_CLIENT_TIMEOUT_SECONDS) {
		errno = EOVERFLOW;
		return -1;
	}
	deadline->tv_sec += ZSV1_CLIENT_TIMEOUT_SECONDS;
	return 0;
}

static int
connect_until(int descriptor, const struct sockaddr_un *address,
	      const struct timespec *deadline)
{
	int result;

	result = connect(descriptor, (const struct sockaddr *)address,
			 sizeof(*address));
	if (result == 0)
		return 0;
	if (errno != EINPROGRESS && errno != EAGAIN)
		return -1;
	if (wait_ready(descriptor, POLLOUT, deadline) != 0)
		return -1;
	{
		int socket_error = 0;
		socklen_t length = sizeof(socket_error);

		if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socket_error,
			       &length) != 0)
			return -1;
		if (socket_error != 0) {
			errno = socket_error;
			return -1;
		}
	}
	return 0;
}

int
zsv1_client_call(const char *path, const struct zsv1_request *request,
		 struct zsv1_response *response)
{
	struct sockaddr_un address;
	struct timespec deadline;
	int descriptor, result, saved_errno;

	if (path == NULL || request == NULL || response == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (strlen(path) >= sizeof(address.sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (make_deadline(&deadline) != 0)
		return -1;
	descriptor =
	    socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (descriptor < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, path);
	if (connect_until(descriptor, &address, &deadline) != 0) {
		saved_errno = errno;
		close(descriptor);
		errno = saved_errno;
		return -1;
	}
	result =
	    zsv1_client_exchange_fd(descriptor, request, response, &deadline);
	saved_errno = errno;
	if (close(descriptor) != 0 && result == 0) {
		result = -1;
		saved_errno = errno;
	}
	if (result != 0)
		errno = saved_errno;
	return result;
}
