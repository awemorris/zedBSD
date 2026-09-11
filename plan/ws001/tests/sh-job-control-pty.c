/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t expired;
static pid_t child_process = -1;

static void
alarm_handler(int signal_number)
{
	(void)signal_number;
	expired = 1;
	if (child_process > 0) {
		(void)kill(-child_process, SIGKILL);
		(void)kill(child_process, SIGKILL);
	}
}

static int
write_all(int descriptor, const char *buffer, size_t length)
{
	while (length != 0) {
		ssize_t count = write(descriptor, buffer, length);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return 0;
		buffer += count;
		length -= (size_t)count;
	}
	return 1;
}

static pid_t
checkpoint_process(const char *path)
{
	char buffer[256];
	char *pid_text;
	FILE *file;
	long value;

	if (path == NULL)
		return -1;
	file = fopen(path, "r");
	if (file == NULL)
		return -1;
	if (fgets(buffer, sizeof(buffer), file) == NULL) {
		(void)fclose(file);
		return -1;
	}
	(void)fclose(file);
	pid_text = strstr(buffer, "pid=");
	if (pid_text == NULL)
		return -1;
	value = strtol(pid_text + 4, NULL, 10);
	return value > 0 ? (pid_t)value : -1;
}

static int
process_is_stopped(pid_t process)
{
	char path[64], status[512];
	char *right_parenthesis;
	FILE *file;

	if (process <= 0)
		return 0;
	(void)snprintf(path, sizeof(path), "/proc/%ld/stat", (long)process);
	file = fopen(path, "r");
	if (file == NULL)
		return 0;
	if (fgets(status, sizeof(status), file) == NULL) {
		(void)fclose(file);
		return 0;
	}
	(void)fclose(file);
	right_parenthesis = strrchr(status, ')');
	return right_parenthesis != NULL && right_parenthesis[1] == ' ' &&
	       (right_parenthesis[2] == 'T' || right_parenthesis[2] == 't');
}

int
main(int argc, char **argv)
{
	char buffer[1024];
	const char *after_stop_input = getenv("P014_AFTER_STOP_INPUT");
	const char *stop_checkpoint = getenv("P014_STOP_CHECKPOINT");
	int master, status;
	int sent_after_stop = 0;
	ssize_t count;
	struct sigaction action;

	if (argc < 3) {
		fprintf(stderr, "usage: %s INPUT COMMAND [ARG ...]\n", argv[0]);
		return 2;
	}
	memset(&action, 0, sizeof(action));
	action.sa_handler = alarm_handler;
	(void)sigemptyset(&action.sa_mask);
	if (sigaction(SIGALRM, &action, NULL) != 0)
		return 2;
	child_process = forkpty(&master, NULL, NULL, NULL);
	if (child_process < 0) {
		perror("forkpty");
		return 2;
	}
	if (child_process == 0) {
		execv(argv[2], &argv[2]);
		perror(argv[2]);
		_exit(127);
	}
	alarm(20);
	if (!write_all(master, argv[1], strlen(argv[1]))) {
		perror("pty input");
		(void)kill(-child_process, SIGKILL);
		(void)kill(child_process, SIGKILL);
	}
	if (fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK) < 0)
		return 2;
	for (;;) {
		struct pollfd descriptor;
		int polled;

		if (!sent_after_stop && stop_checkpoint != NULL) {
			pid_t stopped = checkpoint_process(stop_checkpoint);
			if (process_is_stopped(stopped)) {
				if (after_stop_input == NULL ||
				    !write_all(master, after_stop_input,
					       strlen(after_stop_input)))
					break;
				(void)printf(
				    "P014-CONTROLLER-STOPPED pid=%ld\n",
				    (long)stopped);
				(void)fflush(stdout);
				sent_after_stop = 1;
			}
		}
		descriptor.fd = master;
		descriptor.events = POLLIN | POLLHUP;
		descriptor.revents = 0;
		polled = poll(&descriptor, 1, 20);
		if (polled < 0 && errno == EINTR && !expired)
			continue;
		if (polled < 0 || expired)
			break;
		if (polled == 0)
			continue;
		count = read(master, buffer, sizeof(buffer));
		if (count > 0) {
			if (!write_all(STDOUT_FILENO, buffer, (size_t)count))
				break;
			continue;
		}
		if (count < 0 && errno == EINTR && !expired)
			continue;
		if (count < 0 && errno != EIO && !expired)
			perror("pty output");
		break;
	}
	(void)close(master);
	do
		count = waitpid(child_process, &status, 0);
	while (count < 0 && errno == EINTR);
	alarm(0);
	if (expired)
		return 124;
	if (stop_checkpoint != NULL && !sent_after_stop)
		return 125;
	if (count != child_process)
		return 2;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return 1;
}
