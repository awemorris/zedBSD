/*
 * WS012 SVC-T003 ZSV1 production server receive tests
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#define _POSIX_C_SOURCE 200809L

#include "userland/base/service/zsv1-server.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void
require(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "SVC-T003 server: %s (errno=%d)\n", message,
			errno);
		exit(1);
	}
}

static struct timespec
deadline_after(long milliseconds)
{
	struct timespec deadline;

	require(clock_gettime(CLOCK_MONOTONIC, &deadline) == 0,
		"read monotonic clock");
	deadline.tv_sec += milliseconds / 1000;
	deadline.tv_nsec += (milliseconds % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	return deadline;
}

static long
elapsed_milliseconds(const struct timespec *start, const struct timespec *end)
{
	return (long)(end->tv_sec - start->tv_sec) * 1000L +
	       (end->tv_nsec - start->tv_nsec) / 1000000L;
}

static void
write_all(int descriptor, const void *data, size_t length)
{
	const unsigned char *bytes = data;
	size_t offset = 0;

	while (offset < length) {
		ssize_t count =
		    write(descriptor, bytes + offset, length - offset);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			_exit(2);
		offset += (size_t)count;
	}
}

static void
read_exact(int descriptor, void *data, size_t length)
{
	unsigned char *bytes = data;
	size_t offset = 0;

	while (offset < length) {
		ssize_t count =
		    read(descriptor, bytes + offset, length - offset);

		if (count < 0 && errno == EINTR)
			continue;
		require(count > 0, "read complete response");
		offset += (size_t)count;
	}
}

static void
sleep_milliseconds(long milliseconds)
{
	struct timespec interval;

	interval.tv_sec = milliseconds / 1000;
	interval.tv_nsec = (milliseconds % 1000) * 1000000L;
	while (nanosleep(&interval, &interval) != 0 && errno == EINTR)
		;
}

static void
test_fragmented_success(void)
{
	static const char *const fragments[] = {"ZSV1 ", "SHOW ", "sshd\n"};
	struct zsv1_request request;
	struct timespec deadline;
	int descriptors[2], status;
	pid_t child;
	size_t index;

	require(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0,
		"fragment socketpair");
	child = fork();
	require(child >= 0, "fragment writer fork");
	if (child == 0) {
		close(descriptors[0]);
		for (index = 0;
		     index < sizeof(fragments) / sizeof(fragments[0]);
		     index++) {
			write_all(descriptors[1], fragments[index],
				  strlen(fragments[index]));
			sleep_milliseconds(15);
		}
		if (shutdown(descriptors[1], SHUT_WR) != 0)
			_exit(3);
		close(descriptors[1]);
		_exit(0);
	}
	close(descriptors[1]);
	deadline = deadline_after(1000);
	require(zsv1_server_receive_fd(descriptors[0], &request, &deadline) ==
		    0,
		"fragmented request accepted");
	require(request.command == ZSV1_COMMAND_SHOW &&
		    strcmp(request.service, "sshd") == 0,
		"fragmented request contents");
	require(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		    WEXITSTATUS(status) == 0,
		"fragment writer status");
	close(descriptors[0]);
}

static void
test_newline_without_half_close_times_out(void)
{
	static const char request_text[] = "ZSV1 LIST\n";
	struct zsv1_request request;
	struct timespec deadline;
	int descriptors[2];

	require(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0,
		"no-half-close socketpair");
	write_all(descriptors[1], request_text, sizeof(request_text) - 1U);
	deadline = deadline_after(100);
	errno = 0;
	require(zsv1_server_receive_fd(descriptors[0], &request, &deadline) ==
			-1 &&
		    errno == ETIMEDOUT,
		"newline without half-close must time out");
	close(descriptors[0]);
	close(descriptors[1]);
}

static void
test_slow_drip_does_not_extend_deadline(void)
{
	static const char request_text[] = "ZSV1 LIST\n";
	struct zsv1_request request;
	struct timespec start, finish, deadline;
	int descriptors[2], status;
	pid_t child;
	size_t index;

	require(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0,
		"slow-drip socketpair");
	child = fork();
	require(child >= 0, "slow-drip writer fork");
	if (child == 0) {
		close(descriptors[0]);
		(void)signal(SIGPIPE, SIG_IGN);
		for (index = 0; index < sizeof(request_text) - 1U; index++) {
			if (write(descriptors[1], request_text + index, 1) != 1)
				break;
			sleep_milliseconds(60);
		}
		(void)shutdown(descriptors[1], SHUT_WR);
		close(descriptors[1]);
		_exit(0);
	}
	close(descriptors[1]);
	require(clock_gettime(CLOCK_MONOTONIC, &start) == 0,
		"slow-drip start clock");
	deadline = deadline_after(180);
	errno = 0;
	require(zsv1_server_receive_fd(descriptors[0], &request, &deadline) ==
			-1 &&
		    errno == ETIMEDOUT,
		"slow drip extended the absolute deadline");
	require(clock_gettime(CLOCK_MONOTONIC, &finish) == 0,
		"slow-drip finish clock");
	require(elapsed_milliseconds(&start, &finish) < 600,
		"slow-drip timeout was not bounded");
	close(descriptors[0]);
	require(waitpid(child, &status, 0) == child && WIFEXITED(status),
		"slow-drip writer status");
}

static void
test_rejected_complete_inputs(void)
{
	static const char trailing[] = "ZSV1 LIST\nZSV1 LIST\n";
	unsigned char overlong[ZSV1_REQUEST_MAX + 1U];
	struct zsv1_request request;
	struct timespec deadline;
	int descriptors[2];

	require(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0,
		"trailing socketpair");
	write_all(descriptors[1], trailing, sizeof(trailing) - 1U);
	require(shutdown(descriptors[1], SHUT_WR) == 0, "trailing half-close");
	deadline = deadline_after(1000);
	errno = 0;
	require(zsv1_server_receive_fd(descriptors[0], &request, &deadline) ==
			-1 &&
		    errno == EINVAL,
		"trailing request accepted");
	close(descriptors[0]);
	close(descriptors[1]);

	memset(overlong, 'A', sizeof(overlong));
	require(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0,
		"overlong socketpair");
	write_all(descriptors[1], overlong, sizeof(overlong));
	require(shutdown(descriptors[1], SHUT_WR) == 0, "overlong half-close");
	deadline = deadline_after(1000);
	errno = 0;
	require(zsv1_server_receive_fd(descriptors[0], &request, &deadline) ==
			-1 &&
		    errno == EMSGSIZE,
		"overlong request accepted");
	close(descriptors[0]);
	close(descriptors[1]);
}

static void
test_send_helpers(void)
{
	static const char expected[] = "ZSV1 OK scheduled\nZSV1 END\n";
	char response[sizeof(expected) - 1U];
	int descriptors[2];

	require(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0,
		"send socketpair");
	require(zsv1_server_send_ok_end_fd(descriptors[0], "scheduled") == 0,
		"send complete OK plus END");
	read_exact(descriptors[1], response, sizeof(response));
	require(memcmp(response, expected, sizeof(response)) == 0,
		"complete OK plus END contents");
	close(descriptors[0]);
	close(descriptors[1]);

	require(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0,
		"broken-peer socketpair");
	close(descriptors[1]);
	require(signal(SIGPIPE, SIG_DFL) != SIG_ERR,
		"restore default SIGPIPE disposition");
	errno = 0;
	require(zsv1_server_send_ok_end_fd(descriptors[0], "scheduled") == -1 &&
		    (errno == EPIPE || errno == ECONNRESET),
		"broken peer must fail without SIGPIPE termination");
	close(descriptors[0]);
}

static void
append_dependency(char *output, size_t capacity, size_t *used,
		  const char *prefix, size_t index)
{
	int length = snprintf(output + *used, capacity - *used, "%s%s%zu",
			      *used == 0 ? "" : ",", prefix, index);

	require(length > 0 && (size_t)length < capacity - *used,
		"construct dependency list");
	*used += (size_t)length;
}

static void
test_dependency_validation(void)
{
	static const char *const malformed[] = {
	    ",alpha", "alpha,", "alpha,,beta", "alpha beta", "alpha,alpha",
	};
	char after[512],
		requires[
		    512];
	size_t after_count, requires_count, after_used = 0, requires_used = 0;
	size_t index;

	require(zsv1_server_dependency_lists_validate("", "", &after_count,
						      &requires_count) == 0 &&
		    after_count == 0 && requires_count == 0,
		"empty dependency lists");
	for (index = 0; index < sizeof(malformed) / sizeof(malformed[0]);
	     index++) {
		errno = 0;
		require(zsv1_server_dependency_lists_validate(
			    malformed[index], "", &after_count,
			    &requires_count) == -1 &&
			    errno == EINVAL,
			"malformed or duplicate dependency accepted");
	}

	after[0] = '\0';
	requires[0] = '\0';
	for (index = 0; index < 16; index++) {
		append_dependency(after, sizeof(after), &after_used, "after",
				  index);
		append_dependency(requires, sizeof(requires), &requires_used,
				  "require", index);
	}
	require(zsv1_server_dependency_lists_validate(
		    after, requires, &after_count, &requires_count) == 0 &&
		    after_count == 16 && requires_count == 16,
		"combined 32 dependencies rejected");
	append_dependency(requires, sizeof(requires), &requires_used, "require",
			  16);
	errno = 0;
	require(zsv1_server_dependency_lists_validate(
		    after, requires, &after_count, &requires_count) == -1 &&
		    errno == EMSGSIZE,
		"combined 33 dependencies accepted");
}

int
main(void)
{
	test_fragmented_success();
	test_newline_without_half_close_times_out();
	test_slow_drip_does_not_extend_deadline();
	test_rejected_complete_inputs();
	test_send_helpers();
	test_dependency_validation();
	puts("SVC-T003 zsv1 server tests passed");
	return 0;
}
