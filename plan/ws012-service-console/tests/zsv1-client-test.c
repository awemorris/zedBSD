/* WS012 SVC-T003 production ZSV1 client fixture. SPDX-License-Identifier: Zlib
 */
#define _POSIX_C_SOURCE 200809L
#include "userland/base/service/zsv1-client.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t alarm_seen;

static void
alarm_handler(int signal_number)
{
	(void)signal_number;
	alarm_seen = 1;
}

static void
fail(const char *message)
{
	fprintf(stderr, "zsv1-client-test: %s (errno=%d)\n", message, errno);
	exit(1);
}

static struct timespec
deadline_after(long milliseconds)
{
	struct timespec deadline;

	if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
		fail("clock_gettime");
	deadline.tv_sec += milliseconds / 1000;
	deadline.tv_nsec += (milliseconds % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	return deadline;
}

static void
sleep_milliseconds(long milliseconds)
{
	struct timespec delay;

	delay.tv_sec = milliseconds / 1000;
	delay.tv_nsec = (milliseconds % 1000) * 1000000L;
	while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
		;
}

static int
write_all(int descriptor, const char *data, size_t length, int fragmented)
{
	size_t offset = 0;

	while (offset < length) {
		size_t amount = fragmented ? 1U : length - offset;
		ssize_t result = write(descriptor, data + offset, amount);

		if (result > 0) {
			offset += (size_t)result;
			continue;
		}
		if (result < 0 && errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}

static void
server_process(int descriptor, const char *response, long delay, int fragmented)
{
	char request[ZSV1_REQUEST_MAX + 1U];
	size_t length = 0;

	for (;;) {
		ssize_t result = read(descriptor, request + length,
				      sizeof(request) - length);

		if (result > 0) {
			length += (size_t)result;
			if (length == sizeof(request))
				_exit(20);
			continue;
		}
		if (result == 0)
			break;
		if (errno != EINTR)
			_exit(21);
	}
	if (length != strlen("ZSV1 HALT\n") ||
	    memcmp(request, "ZSV1 HALT\n", length) != 0)
		_exit(22);
	sleep_milliseconds(delay);
	if (write_all(descriptor, response, strlen(response), fragmented) != 0)
		_exit(23);
	close(descriptor);
	_exit(0);
}

static int
exchange_with_server(const char *wire, long delay, int fragmented,
		     long deadline_ms, struct zsv1_response *response,
		     int interrupt_poll)
{
	struct zsv1_request request;
	struct timespec deadline;
	struct itimerval timer;
	int pair[2], status, result, saved_errno;
	pid_t child;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
		fail("socketpair");
	child = fork();
	if (child < 0)
		fail("fork");
	if (child == 0) {
		close(pair[0]);
		server_process(pair[1], wire, delay, fragmented);
	}
	close(pair[1]);
	memset(&request, 0, sizeof(request));
	request.command = ZSV1_COMMAND_HALT;
	deadline = deadline_after(deadline_ms);
	memset(&timer, 0, sizeof(timer));
	if (interrupt_poll) {
		timer.it_value.tv_usec = 20000;
		if (setitimer(ITIMER_REAL, &timer, NULL) != 0)
			fail("setitimer");
	}
	result =
	    zsv1_client_exchange_fd(pair[0], &request, response, &deadline);
	saved_errno = errno;
	memset(&timer, 0, sizeof(timer));
	(void)setitimer(ITIMER_REAL, &timer, NULL);
	close(pair[0]);
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0)
		fail("server child");
	errno = saved_errno;
	return result;
}

static void
test_success_and_protocol_failures(void)
{
	struct zsv1_response response;

	alarm_seen = 0;
	if (exchange_with_server("ZSV1 OK scheduled\nZSV1 END\n", 80, 1, 2000,
				 &response, 1) != 0 ||
	    !alarm_seen || !response.ok_present ||
	    strcmp(response.ok_token, "scheduled") != 0)
		fail("fragmented/EINTR exchange");
	if (exchange_with_server("ZSV1 ERROR 5 failed\nZSV1 END\n", 0, 0, 2000,
				 &response, 0) != 0 ||
	    !response.error_present || response.error_number != 5)
		fail("typed server error");
	if (exchange_with_server("ZSV1 OK partial\n", 0, 0, 2000, &response,
				 0) == 0)
		fail("EOF before END accepted");
	if (exchange_with_server("ZSV1 END\nZSV1 OK trailing\n", 0, 0, 2000,
				 &response, 0) == 0)
		fail("data after END accepted");
}

static void
test_deadline_not_reset(void)
{
	static const char partial[] = "ZSV1 OK scheduled\n";
	static const char terminal[] = "ZSV1 END\n";
	struct zsv1_request request;
	struct zsv1_response response;
	struct timespec deadline;
	char received[ZSV1_REQUEST_MAX + 1U];
	int pair[2], status, result, saved_errno;
	pid_t child;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
		fail("deadline socketpair");
	child = fork();
	if (child < 0)
		fail("deadline fork");
	if (child == 0) {
		size_t used = 0;

		close(pair[0]);
		(void)signal(SIGPIPE, SIG_IGN);
		for (;;) {
			ssize_t count = read(pair[1], received + used,
					     sizeof(received) - used);

			if (count > 0) {
				used += (size_t)count;
				if (used == sizeof(received))
					_exit(30);
				continue;
			}
			if (count == 0)
				break;
			if (errno != EINTR)
				_exit(31);
		}
		if (used != strlen("ZSV1 HALT\n") ||
		    memcmp(received, "ZSV1 HALT\n", used) != 0)
			_exit(32);
		if (write_all(pair[1], partial, sizeof(partial) - 1, 0) != 0)
			_exit(33);
		sleep_milliseconds(200);
		(void)write_all(pair[1], terminal, sizeof(terminal) - 1, 0);
		close(pair[1]);
		_exit(0);
	}
	close(pair[1]);
	memset(&request, 0, sizeof(request));
	request.command = ZSV1_COMMAND_HALT;
	deadline = deadline_after(80);
	result =
	    zsv1_client_exchange_fd(pair[0], &request, &response, &deadline);
	saved_errno = errno;
	close(pair[0]);
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0)
		fail("deadline child");
	if (result == 0 || saved_errno != ETIMEDOUT)
		fail("whole-operation deadline reset after partial response");
}

static void
test_deadlines_and_sigpipe(void)
{
	struct zsv1_request request;
	struct zsv1_response response;
	struct timespec deadline;
	int pair[2], result, saved_errno;
	pid_t child;
	int status;

	memset(&request, 0, sizeof(request));
	request.command = ZSV1_COMMAND_HALT;
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
		fail("timeout socketpair");
	deadline = deadline_after(-1);
	result =
	    zsv1_client_exchange_fd(pair[0], &request, &response, &deadline);
	saved_errno = errno;
	close(pair[0]);
	close(pair[1]);
	if (result == 0 || saved_errno != ETIMEDOUT)
		fail("expired whole-operation deadline");

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
		fail("SIGPIPE socketpair");
	child = fork();
	if (child < 0)
		fail("SIGPIPE fork");
	if (child == 0) {
		close(pair[0]);
		close(pair[1]);
		_exit(0);
	}
	close(pair[1]);
	if (waitpid(child, &status, 0) != child)
		fail("SIGPIPE wait");
	deadline = deadline_after(1000);
	if (zsv1_client_exchange_fd(pair[0], &request, &response, &deadline) ==
	    0)
		fail("closed peer accepted");
	close(pair[0]);
}

int
main(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = alarm_handler;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGALRM, &action, NULL) != 0)
		fail("sigaction");
	test_success_and_protocol_failures();
	test_deadline_not_reset();
	test_deadlines_and_sigpipe();
	puts("zsv1 client test: PASS");
	return 0;
}
