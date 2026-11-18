/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Starting keiland-preview on Linux (WS168 p004; client.h,
 * plan/ws168/preview-sandbox-design.md section 7): a new process with the input as
 * fd 0 and the output as fd 1 and every other descriptor closed, the
 * limits of memory, processor time and size written, and the program run
 * with an empty environment; the program confines itself with seccomp
 * before it reads a byte (linux/confine.c).
 */

/* close_range is a GNU extension of the C library. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../spawn.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

/* The program. */
#define SPAWN_PROGRAM		KEILAND_LIBEXECDIR "/keiland-preview"

/*
 * The system's record of a child: its process ID, which only this file
 * knows.  It lives from preview_spawn until the child is collected.
 */
struct preview_child {
	pid_t pid;
};

static void spawn_child(int input, int output, char **argv) __attribute__((noreturn));

/*
 * Starts the child.  Returns 0 with its record, or an errno value.
 */
int
preview_spawn(
	int input,
	int output,
	const struct preview_request *request,
	struct preview_child **child,
	int *status)
{
	struct preview_child *made;
	char width[32];
	char height[32];
	char stamp[PREVIEW_STAMP_MAX + 16U];
	char *argv[6];
	size_t count;
	pid_t pid;
	int error;

	/* The arguments. */
	*child = NULL;
	*status = PREVIEW_NO_SANDBOX;
	(void)snprintf(width, sizeof(width), "--width=%d", request->width);
	(void)snprintf(height, sizeof(height), "--height=%d", request->height);
	count = 0;
	argv[count++] = "keiland-preview";
	if (request->info) {
		/* What the file is (ws157-p013). */
		argv[count++] = "--info";
	} else {
		/* A picture of a size. */
		argv[count++] = width;
		argv[count++] = height;
		if (request->cover)
			argv[count++] = "--fit=cover";
		if (request->stamp[0] != '\0') {
			(void)snprintf(stamp, sizeof(stamp), "--stamp=%s", request->stamp);
			argv[count++] = stamp;
		}
	}

	/* The list's end. */
	argv[count] = NULL;

	/* The child's record, made before the child (nothing is allocated in it). */
	made = malloc(sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* The child. */
	pid = fork();
	if (pid < 0) {
		error = errno;
		free(made);
		return error;
	}

	/* The child goes on in spawn_child. */
	if (pid == 0)
		spawn_child(input, output, argv);

	/* Succeeded: the child runs. */
	made->pid = pid;
	*child = made;
	return 0;
}

/*
 * Looks at a child without waiting: 1 when it ended (its record freed),
 * 0 while it runs.
 */
int
preview_child_ended(
	struct preview_child *child,
	int *status)
{
	pid_t ended;
	int wait_status;
	int exited;
	int signalled;

	/* Still running, or the look was cut short (it is looked at again). */
	ended = waitpid(child->pid, &wait_status, WNOHANG);
	if (ended == 0)
		return 0;
	if (ended < 0 && errno == EINTR)
		return 0;

	/* How it ended: its exit status, the signal that ended it, or not known (collected already). */
	*status = PREVIEW_KILLED;
	if (ended == child->pid) {
		exited = WIFEXITED(wait_status);
		signalled = WIFSIGNALED(wait_status);
		if (exited)
			*status = WEXITSTATUS(wait_status);
		else if (signalled)
			*status = PREVIEW_KILLED + WTERMSIG(wait_status);
	}

	/* Succeeded: it ended, and its record goes. */
	free(child);
	return 1;
}

/*
 * Ends a child that runs and collects it (its record freed).
 */
int
preview_child_end(
	struct preview_child *child)
{
	pid_t ended;

	/* Ended, and collected. */
	(void)kill(child->pid, SIGKILL);
	do {
		ended = waitpid(child->pid, NULL, 0);
	} while (ended < 0 && errno == EINTR);
	free(child);

	/* Succeeded: it was ended by the signal. */
	return PREVIEW_KILLED + SIGKILL;
}

/* In the child: its two files, its limits, then the program (or the end, 127). */
static void
spawn_child(
	int input,
	int output,
	char **argv)
{
	static char *environment[1];
	struct rlimit limit;
	int first;
	int second;

	/* The input and the output, nothing else. */
	first = dup2(input, 0);
	second = dup2(output, 1);
	if (first < 0 || second < 0)
		_exit(127);
	(void)close_range(2, ~0U, 0);

	/* The limits. */
	limit.rlim_cur = PREVIEW_MEMORY_MAX;
	limit.rlim_max = PREVIEW_MEMORY_MAX;
	(void)setrlimit(RLIMIT_AS, &limit);
	limit.rlim_cur = PREVIEW_CPU_SECONDS;
	limit.rlim_max = PREVIEW_CPU_SECONDS;
	(void)setrlimit(RLIMIT_CPU, &limit);
	limit.rlim_cur = PREVIEW_WRITE_MAX;
	limit.rlim_max = PREVIEW_WRITE_MAX;
	(void)setrlimit(RLIMIT_FSIZE, &limit);

	/* The program, with no environment. */
	environment[0] = NULL;
	(void)execve(SPAWN_PROGRAM, argv, environment);
	_exit(127);
}
