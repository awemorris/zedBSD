/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The programs the compositor starts (App Home's applications, the
 * desktop program, Settings, the input method): the event loop never
 * forks.  It prepares what the program gets -- its arguments, its
 * environment (the compositor's, with the socket's place and the
 * program's activation token), the descriptor it is lent -- and the file
 * I/O thread starts it with posix_spawn (vfork, nothing of the address
 * space copied).  The process ID comes back to the event loop with the
 * job (then), which keeps waitpid and kill as before.
 *
 * The program starts in a session of its own, with its signals unblocked
 * (the file I/O thread blocks all of its own), with none of the
 * compositor's descriptors but the lent one, and, for a command, with
 * /dev/null as its input.
 */

#include "kwl.h"
#include "activation.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The descriptor number a lent descriptor gets in the program, or the next one when the lent one is it already. */
#define SPAWN_LENT_AT		3

/* The most arguments a program is given, its name among them. */
#define SPAWN_ARGUMENTS		4U

/* The environment's variables the compositor sets or removes for a program, at most. */
#define SPAWN_CHANGES		4U

/*
 * One start for the file I/O thread: the program's path, its arguments
 * and environment (each a NULL-terminated array of the job's own strings),
 * the descriptor lent to it (-1 for none; the job closes it after the
 * start) and the number it gets there, whether its input is /dev/null,
 * what goes on with the result (then and context), and the result: the
 * process ID, or 0 with the start's errno.
 */
struct spawn_job {
	struct kwl_lane_job lane;
	char *path;
	char **arguments;
	char **environment;
	int lent;
	int lent_at;
	unsigned null_input;
	kwl_spawn_fn *then;
	void *context;
	pid_t pid;
	int error;
};

static int spawn_submit(struct kwl_server *server, const char *path, const char *const *arguments, const char *const *sets, const char *const *removes, int lent, unsigned null_input, kwl_spawn_fn *then, void *context);
static char **spawn_environment(const char *const *sets, const char *const *removes);
static char **spawn_strings(const char *const *strings);
static void spawn_strings_free(char **strings);
static int spawn_name_matches(const char *entry, const char *const *names);
static void spawn_run(struct kwl_lane_job *lane);
static void spawn_done(void *context, struct kwl_lane_job *lane);
static void spawn_drop(struct kwl_lane_job *lane);
static void spawn_free(struct spawn_job *job);

/* The file I/O thread's starts. */
static const struct kwl_lane_ops spawn_ops = {
	spawn_run,
	spawn_done,
	spawn_drop
};

/* The process's environment, which the event loop copies for each program. */
extern char **environ;

/*
 * Starts a command with /bin/sh -c on the file I/O thread, in a session of
 * its own, without the compositor's descriptors and with the compositor's
 * socket and its own activation token in its environment.
 * then(server, pid, 0, context) is called on the event loop's thread once
 * it has started (pid 0 and the errno when it did not).  Returns 0, or an
 * errno when the start could not be asked for (then is not called).
 */
int
kwl_spawn(
	struct kwl_server *server,
	const char *command,
	kwl_spawn_fn *then,
	void *context)
{
	const char *arguments[SPAWN_ARGUMENTS];
	const char *sets[SPAWN_CHANGES];
	const char *removes[SPAWN_CHANGES];
	char token[KWL_ACTIVATION_TOKEN_SIZE];
	char token_variable[KWL_ACTIVATION_TOKEN_SIZE + 32];
	char directory_variable[160];
	char display_variable[160];
	const char *slash;
	unsigned set_count;
	unsigned remove_count;
	int error;

	/*
	 * The program's activation token (activation.c): with it
	 * the program may bring a window to the front, its own or that of a
	 * running copy of itself it hands its request to.  A program starts
	 * without one when none can be made, never with the compositor's own.
	 */
	set_count = 0U;
	remove_count = 0U;
	error = kwl_activation_issue(server, command, "spawn", token, sizeof(token));
	if (error == 0) {
		(void)snprintf(token_variable, sizeof(token_variable), "XDG_ACTIVATION_TOKEN=%s", token);
		sets[set_count++] = token_variable;
	} else {
		removes[remove_count++] = "XDG_ACTIVATION_TOKEN";
	}

	/* The socket as XDG_RUNTIME_DIR and WAYLAND_DISPLAY, the way clients look for it. */
	slash = strrchr(server->socket_path, '/');
	if (slash != NULL) {
		(void)snprintf(directory_variable, sizeof(directory_variable), "XDG_RUNTIME_DIR=%.*s", (int)(slash - server->socket_path), server->socket_path);
		sets[set_count++] = directory_variable;
		(void)snprintf(display_variable, sizeof(display_variable), "WAYLAND_DISPLAY=%s", slash + 1);
	} else {
		(void)snprintf(display_variable, sizeof(display_variable), "WAYLAND_DISPLAY=%s", server->socket_path);
	}

	/* Adds WAYLAND_DISPLAY and ends both lists with NULL. */
	sets[set_count++] = display_variable;
	sets[set_count] = NULL;
	removes[remove_count] = NULL;

	/* The shell runs the command. */
	arguments[0] = "sh";
	arguments[1] = "-c";
	arguments[2] = command;
	arguments[3] = NULL;
	error = spawn_submit(server, "/bin/sh", arguments, sets, removes, -1, 1U, then, context);
	if (error != 0)
		return error;

	/* Succeeded: the file I/O thread starts it. */
	return 0;
}

/*
 * Starts a program with one argument on the file I/O thread, in a session
 * of its own, lent one descriptor (taken: the start closes it here
 * afterwards) whose number in the program is put in the environment
 * variable named (WAYLAND_SOCKET for the input method), without
 * WAYLAND_DISPLAY.  then is called as for kwl_spawn.  Returns 0, or an
 * errno when the start could not be asked for (the descriptor is closed,
 * then is not called).
 */
int
kwl_spawn_lent(
	struct kwl_server *server,
	const char *path,
	const char *argument,
	const char *variable,
	int descriptor,
	kwl_spawn_fn *then,
	void *context)
{
	const char *arguments[SPAWN_ARGUMENTS];
	const char *sets[SPAWN_CHANGES];
	const char *removes[SPAWN_CHANGES];
	char lent_variable[64];
	int lent_at;
	int error;

	/* The number the descriptor gets there (one past it when it already is that number). */
	lent_at = SPAWN_LENT_AT;
	if (descriptor == SPAWN_LENT_AT)
		lent_at = SPAWN_LENT_AT + 1;
	(void)snprintf(lent_variable, sizeof(lent_variable), "%s=%d", variable, lent_at);
	sets[0] = lent_variable;
	sets[1] = NULL;
	removes[0] = "WAYLAND_DISPLAY";
	removes[1] = NULL;

	/* The program and its argument. */
	arguments[0] = path;
	arguments[1] = argument;
	arguments[2] = NULL;
	error = spawn_submit(server, path, arguments, sets, removes, descriptor, 0U, then, context);
	if (error != 0)
		return error;

	/* Succeeded: the file I/O thread starts it. */
	return 0;
}

/*
 * Makes a start's job from the event loop's copies and gives it to the
 * file I/O thread (or runs it here without the thread, and its then).
 * Returns 0 or ENOMEM (a lent descriptor is closed then).
 */
static int
spawn_submit(
	struct kwl_server *server,
	const char *path,
	const char *const *arguments,
	const char *const *sets,
	const char *const *removes,
	int lent,
	unsigned null_input,
	kwl_spawn_fn *then,
	void *context)
{
	struct spawn_job *job;
	int error;

	/* The job and its copies. */
	job = calloc(1, sizeof(*job));
	if (job == NULL) {
		if (lent >= 0)
			close(lent);
		return ENOMEM;
	}

	/* Records which descriptor is lent to the program, and the number it appears as. */
	job->lent = lent;
	job->lent_at = SPAWN_LENT_AT;
	if (lent == SPAWN_LENT_AT)
		job->lent_at = SPAWN_LENT_AT + 1;
	job->null_input = null_input;
	job->then = then;
	job->context = context;
	job->path = strdup(path);
	job->arguments = spawn_strings(arguments);
	job->environment = spawn_environment(sets, removes);
	if (job->path == NULL || job->arguments == NULL || job->environment == NULL) {
		spawn_free(job);
		return ENOMEM;
	}

	/* To the thread, or here at once without it. */
	error = kwl_io_file_submit(server, &job->lane, &spawn_ops, 0U);
	if (error != 0) {
		spawn_run(&job->lane);
		spawn_done(server, &job->lane);
	}

	/* Succeeded: the start is asked for. */
	return 0;
}

/*
 * Copies the process's environment for a program: each variable but the
 * ones set or removed, then the ones set.  NULL without memory.
 */
static char **
spawn_environment(
	const char *const *sets,
	const char *const *removes)
{
	char **copy;
	size_t count;
	size_t index;
	size_t taken;
	unsigned set;
	int changed;

	/* How many there may be. */
	count = 0U;
	while (environ != NULL && environ[count] != NULL)
		count++;
	copy = calloc(count + SPAWN_CHANGES + 1U, sizeof(*copy));
	if (copy == NULL)
		return NULL;

	/* Each variable the program keeps. */
	taken = 0U;
	for (index = 0U; index < count; index++) {
		changed = spawn_name_matches(environ[index], sets);
		if (!changed)
			changed = spawn_name_matches(environ[index], removes);
		if (changed)
			continue;
		copy[taken] = strdup(environ[index]);
		if (copy[taken] == NULL) {
			spawn_strings_free(copy);
			return NULL;
		}

		/* The copy counts in the result. */
		taken++;
	}

	/* The ones the compositor sets. */
	for (set = 0U; sets[set] != NULL; set++) {
		copy[taken] = strdup(sets[set]);
		if (copy[taken] == NULL) {
			spawn_strings_free(copy);
			return NULL;
		}

		/* The copy counts in the result. */
		taken++;
	}

	/* Succeeded: the copy, ended by NULL. */
	return copy;
}

/* Copies a NULL-terminated array of strings; NULL without memory. */
static char **
spawn_strings(
	const char *const *strings)
{
	char **copy;
	size_t count;
	size_t index;

	/* The array. */
	count = 0U;
	while (strings[count] != NULL)
		count++;
	copy = calloc(count + 1U, sizeof(*copy));
	if (copy == NULL)
		return NULL;

	/* Each string. */
	for (index = 0U; index < count; index++) {
		copy[index] = strdup(strings[index]);
		if (copy[index] == NULL) {
			spawn_strings_free(copy);
			return NULL;
		}
	}

	/* Succeeded: the copy, ended by NULL. */
	return copy;
}

/* Frees a NULL-terminated array of strings and its strings (NULL does nothing). */
static void
spawn_strings_free(
	char **strings)
{
	size_t index;

	/* Nothing. */
	if (strings == NULL)
		return;

	/* Each string, then the array. */
	for (index = 0U; strings[index] != NULL; index++)
		free(strings[index]);
	free(strings);
}

/*
 * Tells whether an environment entry ("NAME=value") names one of the
 * names given (each "NAME" or "NAME=value").
 */
static int
spawn_name_matches(
	const char *entry,
	const char *const *names)
{
	const char *equals;
	size_t length;
	unsigned index;
	int same;

	/* The entry's name. */
	equals = strchr(entry, '=');
	if (equals == NULL)
		return 0;
	length = (size_t)(equals - entry);

	/* Each name given, with or without its value. */
	for (index = 0U; names[index] != NULL; index++) {
		same = strncmp(names[index], entry, length);
		if (same == 0 && (names[index][length] == '=' || names[index][length] == '\0'))
			return 1;
	}

	/* None. */
	return 0;
}

/*
 * Starts the program on the file I/O thread: its own session, its signals
 * unblocked and at their defaults where the compositor catches them, its
 * input /dev/null for a command, the lent descriptor at its number, and
 * every other descriptor from 3 on closed.  The lent descriptor is closed
 * here after.
 */
static void
spawn_run(
	struct kwl_lane_job *lane)
{
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	struct spawn_job *job;
	sigset_t signals;
	short flags;
	int first_closed;
	int error;

	/* What the program gets of the compositor's descriptors. */
	job = (struct spawn_job *)lane;
	first_closed = SPAWN_LENT_AT;
	error = posix_spawn_file_actions_init(&actions);
	if (error != 0) {
		job->error = error;
		return;
	}

	/* The program's input is /dev/null when asked. */
	if (job->null_input)
		error = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
	if (error == 0 && job->lent >= 0) {
		error = posix_spawn_file_actions_adddup2(&actions, job->lent, job->lent_at);
		first_closed = job->lent_at + 1;
	}

	/* Closes every other descriptor from the one after the lent one. */
	if (error == 0)
		error = posix_spawn_file_actions_addclosefrom_np(&actions, first_closed);

	/* Its own session, no signal blocked, the ones the compositor catches at their defaults. */
	if (error == 0)
		error = posix_spawnattr_init(&attributes);
	if (error != 0) {
		posix_spawn_file_actions_destroy(&actions);
		job->error = error;
		return;
	}

	/* Applies the session, the mask and the default signals. */
	flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_SETSID
	flags |= POSIX_SPAWN_SETSID;
#endif
	(void)posix_spawnattr_setflags(&attributes, flags);
	(void)sigemptyset(&signals);
	(void)posix_spawnattr_setsigmask(&attributes, &signals);
	(void)sigaddset(&signals, SIGINT);
	(void)sigaddset(&signals, SIGTERM);
	(void)sigaddset(&signals, SIGHUP);
	(void)sigaddset(&signals, SIGCHLD);
	(void)posix_spawnattr_setsigdefault(&attributes, &signals);

	/* The start. */
	job->error = posix_spawn(&job->pid, job->path, &actions, &attributes, job->arguments, job->environment);
	if (job->error != 0)
		job->pid = 0;
	posix_spawnattr_destroy(&attributes);
	posix_spawn_file_actions_destroy(&actions);

	/* Succeeded: the lent descriptor is the program's alone. */
	if (job->lent >= 0)
		close(job->lent);
	job->lent = -1;
}

/* Takes a start's result on the event loop's thread: its then hears the process ID or the failure. */
static void
spawn_done(
	void *context,
	struct kwl_lane_job *lane)
{
	struct spawn_job *job;

	/* The result, then the job goes. */
	job = (struct spawn_job *)lane;
	if (job->then != NULL)
		job->then(context, job->pid, job->error, job->context);
	spawn_free(job);
}

/* Frees a start not run (the thread stopped, at the end): nothing is started, nobody hears. */
static void
spawn_drop(
	struct kwl_lane_job *lane)
{
	/* Freed. */
	spawn_free((struct spawn_job *)lane);
}

/* Frees a start's job: its copies and a lent descriptor not given. */
static void
spawn_free(
	struct spawn_job *job)
{
	/* The descriptor, the copies, the job. */
	if (job->lent >= 0)
		close(job->lent);
	free(job->path);
	spawn_strings_free(job->arguments);
	spawn_strings_free(job->environment);
	free(job);
}
