/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The POSIX systems' part of mikuterm's X server process (x11-spawn.h):
 * the program started again with posix_spawn (safe in a program with
 * threads, where a fork would copy one thread of many), its end of the
 * link as its standard input, the rest of its descriptors closed on exec
 * as every one mikuterm opens is.  It ends with its link: when mikuterm
 * goes, its end of the link closes and the child reads the end.  Nothing
 * here is Linux's own: the program's path is mikuterm_program's.
 */

#include "mikuterm.h"
#include "x11-host.h"
#include "x11-spawn.h"

#include <keiland/keiland_socket.h>

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* How often the end of the child is looked for while it has its moment (ms). */
#define SPAWN_POLL_MS		10U

extern char **environ;

static int spawn_reaped(struct x11_host *host, int wait);
static void spawn_nap(unsigned milliseconds);

/*
 * Keeps the pair's listener from being shared: nothing to do, as no
 * other program can bind a POSIX system's port in use without asking to
 * share it (x11-host.c still checks the end it accepts).
 */
void
x11_spawn_guard(
	struct kl_socket *listener)
{
	(void)listener;
}

/*
 * Starts the program as mikuterm --x11-server with the link's end as its
 * standard input; mikuterm's own descriptor of that end is closed once
 * the child has its copy.  Returns 0, or an errno value.
 */
int
x11_spawn_start(
	struct x11_host *host,
	struct kl_socket *child_end)
{
	posix_spawn_file_actions_t actions;
	char *arguments[3];
	const char *program;
	pid_t pid;
	int descriptor;
	int error;

	/* The program, and the switch. */
	program = mikuterm_program();
	if (program == NULL || program[0] == '\0')
		return ENOENT;
	arguments[0] = (char *)program;
	arguments[1] = (char *)X11_SPAWN_SWITCH;
	arguments[2] = NULL;

	/* The link's end as standard input (the copy dup2 makes is kept across the exec). */
	descriptor = (int)kl_socket_native(child_end);
	error = posix_spawn_file_actions_init(&actions);
	if (error != 0)
		return error;
	error = posix_spawn_file_actions_adddup2(&actions, descriptor, STDIN_FILENO);
	if (error == 0)
		error = posix_spawn(&pid, program, &actions, NULL, arguments, environ);
	(void)posix_spawn_file_actions_destroy(&actions);
	if (error != 0)
		return error;

	/* Succeeded: the child runs with its copy, and mikuterm needs its own no longer. */
	host->pid = (unsigned long)pid;
	kl_socket_close(child_end);
	host->child_end = NULL;
	return 0;
}

/*
 * Ends the child after its link closed: it has X11_SPAWN_END_MS to end
 * by itself, then it is killed; either way it is reaped.
 */
void
x11_spawn_stop(
	struct x11_host *host)
{
	unsigned waited;

	/* Nothing runs. */
	if (host->pid == 0)
		return;

	/* The moment. */
	for (waited = 0U; waited < X11_SPAWN_END_MS; waited += SPAWN_POLL_MS) {
		if (spawn_reaped(host, 0))
			return;
		spawn_nap(SPAWN_POLL_MS);
	}

	/* Then the end. */
	(void)kill((pid_t)host->pid, SIGKILL);
	(void)spawn_reaped(host, 1);
}

/*
 * Lets the child bring its windows to the front: a POSIX system has no
 * such right to give (the desktop decides).
 */
void
x11_host_foreground(
	const struct x11_host *host)
{
	(void)host;
}

/*
 * Reaps the child if it has ended (waiting for it when asked).  Returns
 * 1 when it is gone (host->pid then 0), or 0.
 */
static int
spawn_reaped(
	struct x11_host *host,
	int wait)
{
	pid_t done;
	int status;
	int flags;

	/* Its end, or not yet. */
	flags = WNOHANG;
	if (wait)
		flags = 0;
	do {
		done = waitpid((pid_t)host->pid, &status, flags);
	} while (done < 0 && errno == EINTR);
	if (done == 0)
		return 0;

	/* Gone (reaped now, or by someone else before). */
	host->pid = 0;
	return 1;
}

/* Sleeps a few milliseconds. */
static void
spawn_nap(
	unsigned milliseconds)
{
	struct timespec nap;

	/* The nap, whole unless a signal cuts it. */
	nap.tv_sec = (time_t)(milliseconds / 1000U);
	nap.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	(void)nanosleep(&nap, NULL);
}
