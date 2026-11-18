/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Starting keiland-preview on zedBSD (WS168 p004; client.h,
 * plan/ws168/preview-sandbox-design.md sections 3 and 5): sandbox_spawn with the
 * program's image (a static one), the input as fd 0 and the output as fd
 * 1 and no other, the arguments, and the limits of memory, processor time
 * and size written.  The child is in its sandbox from its first
 * instruction.
 */

#include "../spawn.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <fcntl.h>
#include <sandbox.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
	struct sandbox_spawn spawn;
	struct sandbox_fd fds[2];
	char width[32];
	char height[32];
	char stamp[PREVIEW_STAMP_MAX + 16U];
	char *argv[6];
	size_t count;
	pid_t pid;
	int image;
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

	/* The child's record. */
	made = malloc(sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* The program's image. */
	image = open(SPAWN_PROGRAM, O_RDONLY | O_CLOEXEC);
	if (image < 0) {
		error = errno;
		free(made);
		return error;
	}

	/* The request: two files, the limits, nothing else allowed. */
	fds[0].from = input;
	fds[0].to = 0;
	fds[1].from = output;
	fds[1].to = 1;
	memset(&spawn, 0, sizeof(spawn));
	spawn.size = sizeof(spawn);
	spawn.image = image;
	spawn.fd_count = 2;
	spawn.fds = (uint64_t)(uintptr_t)fds;
	spawn.argv = (uint64_t)(uintptr_t)argv;
	spawn.memory_max = PREVIEW_MEMORY_MAX;
	spawn.cpu_seconds = PREVIEW_CPU_SECONDS;
	spawn.write_max = PREVIEW_WRITE_MAX;
	pid = sandbox_spawn(&spawn);
	error = errno;
	(void)close(image);
	if (pid < 0) {
		free(made);
		return error;
	}

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
