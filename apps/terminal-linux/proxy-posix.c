/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The POSIX systems' connection through ssh_config's ProxyCommand
 * (proxy.h).
 *
 * The command runs without a shell: its line is split into words as a
 * shell splits a simple command (proxy-command.c), a first word "exec"
 * (which OpenSSH's ssh puts before the line itself) is passed over, and
 * the program the next word names (a path from the root, or a name looked
 * for in PATH, program-posix.c) is started by posix_spawn with the rest
 * as its arguments, in a process group of its own.
 *
 * The caller gets one end of a pair of connected loopback sockets; the
 * other end is the program's standard input and output, as OpenSSH hands
 * a ProxyCommand one end of a socket pair.  Nothing carries its bytes but
 * the program itself.  What it writes on its errors comes through a pipe,
 * whose last part a thread keeps to tell why a connection failed.  The
 * pair is checked to be the two ends this side made (the connecting
 * socket's own address is the one accepted).
 */

#include "mikuterm.h"
#include "proxy.h"
#include "proxy-command.h"
#include "program.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* The most words a command has, the program's among them. */
#define PROXY_WORDS_MAX		256U

/* The word OpenSSH's ssh puts before the command, passed over when the command has it. */
#define PROXY_EXEC		"exec"

/* The bytes read from the errors at once, and how often the errors' thread looks whether it is to stop (ms). */
#define PROXY_CHUNK		4096U
#define PROXY_ROUND_MS		100

/* How long the program has to end after it is asked (ms), and how often its end is looked for meanwhile. */
#define PROXY_END_MS		1000U
#define PROXY_END_ROUND_MS	10U

extern char **environ;

/*
 * One command's run: the program's process (its group's number too), the
 * read end of the pipe of its errors and the thread that reads it, and
 * the last of what it wrote there (tail, tail_size bytes of it, under
 * lock: the thread writes it while the connection's thread may read it).
 * stopping, under lock, tells the thread to end.
 */
struct proxy {
	pid_t pid;
	int errors;
	struct kl_thread *errors_thread;
	struct kl_mutex lock;
	int stopping;
	char tail[PROXY_ERRORS_MAX];
	size_t tail_size;
};

static int proxy_find(char **words, unsigned count, char *program, size_t program_size, unsigned *first, char *error, size_t error_size);
static int proxy_spawn(struct proxy *proxy, const char *program, char **arguments, int connection, int errors);
static int proxy_pair(struct kl_socket **ours, struct kl_socket **theirs);
static void proxy_from_errors(void *argument);
static void proxy_end(struct proxy *proxy);
static void proxy_free(struct proxy *proxy);

/*
 * Runs a command (its %-tokens already put in) and gives the connection
 * to it: *socket is the caller's end (it does not block; the caller
 * closes it before proxy_stop), and *proxy the run to stop.  Returns 0,
 * or ENOENT (no such program), EINVAL, E2BIG, ENOMEM or EIO with a line
 * about it in error (error_size bytes).
 */
int
proxy_start(
	const char *command,
	struct proxy **proxy,
	struct kl_socket **socket,
	char *error,
	size_t error_size)
{
	struct proxy *run;
	struct kl_socket *theirs;
	struct kl_thread_options options;
	char program[MIKUTERM_PATH_MAX];
	char *words[PROXY_WORDS_MAX + 1U];
	char *buffer;
	size_t size;
	unsigned count;
	unsigned first;
	int pipe_ends[2];
	int status;

	/* Nothing started yet. */
	*proxy = NULL;
	*socket = NULL;
	error[0] = '\0';

	/* The command's words. */
	size = strlen(command) + 1U;
	buffer = malloc(size);
	if (buffer == NULL)
		return ENOMEM;
	status = proxy_command_words(command, buffer, size, words, PROXY_WORDS_MAX, &count, error, error_size);
	if (status != 0) {
		free(buffer);
		if (error[0] == '\0')
			(void)snprintf(error, error_size, "ProxyCommand: the command cannot be read");
		return status;
	}
	words[count] = NULL;

	/* The program the first word (after exec) names. */
	status = proxy_find(words, count, program, sizeof(program), &first, error, error_size);
	if (status != 0) {
		free(buffer);
		return status;
	}

	/* The run's state. */
	run = calloc(1U, sizeof(*run));
	if (run == NULL) {
		free(buffer);
		return ENOMEM;
	}
	run->errors = -1;
	status = kl_mutex_init(&run->lock);
	if (status != 0) {
		free(run);
		free(buffer);
		return status;
	}

	/* The socket pair: the caller's end and the program's. */
	status = proxy_pair(socket, &theirs);
	if (status != 0) {
		proxy_free(run);
		free(buffer);
		(void)snprintf(error, error_size, "ProxyCommand: no connection to the program could be made");
		return status;
	}

	/* The pipe of its errors, closed on exec but in the program (as its standard error). */
	status = pipe2(pipe_ends, O_CLOEXEC);
	if (status != 0) {
		status = errno;
		kl_socket_close(theirs);
		kl_socket_close(*socket);
		*socket = NULL;
		proxy_free(run);
		free(buffer);
		(void)snprintf(error, error_size, "ProxyCommand: the program could not be started");
		return status;
	}
	run->errors = pipe_ends[0];

	/* The program, with its words after the program's word as its arguments; its ends are its own after. */
	words[first] = program;
	status = proxy_spawn(run, program, words + first, (int)kl_socket_native(theirs), pipe_ends[1]);
	kl_socket_close(theirs);
	(void)close(pipe_ends[1]);
	free(buffer);
	if (status != 0) {
		kl_socket_close(*socket);
		*socket = NULL;
		proxy_free(run);
		if (status == ENOENT || status == EACCES)
			(void)snprintf(error, error_size, "ProxyCommand: %s: %s", program, strerror(status));
		else
			(void)snprintf(error, error_size, "ProxyCommand: the program could not be started");
		return status;
	}

	/* The thread that keeps the last of its errors. */
	memset(&options, 0, sizeof(options));
	options.name = "proxy-errors";
	options.run = proxy_from_errors;
	options.argument = run;
	options.priority = KL_THREAD_PRIORITY_NORMAL;
	status = kl_thread_start(&options, &run->errors_thread);
	if (status != 0) {
		kl_socket_close(*socket);
		*socket = NULL;
		proxy_free(run);
		(void)snprintf(error, error_size, "ProxyCommand: the program's errors could not be read");
		return status;
	}

	/* Succeeded: the program runs and the socket is its connection. */
	*proxy = run;
	return 0;
}

/*
 * Writes the last of what the command wrote on its errors into text (size
 * bytes with its NUL), as it wrote it (its line breaks kept, a NUL left
 * out); empty when it wrote nothing.
 */
void
proxy_errors(
	struct proxy *proxy,
	char *text,
	size_t size)
{
	size_t length;
	size_t index;
	size_t used;

	/* Nothing yet. */
	if (size == 0U)
		return;
	text[0] = '\0';

	/* The tail, under the lock its thread writes it under. */
	kl_mutex_lock(&proxy->lock);

	length = proxy->tail_size;
	if (length >= size)
		length = size - 1U;
	used = 0U;
	for (index = proxy->tail_size - length; index < proxy->tail_size; index++) {
		/* A NUL would end the text early: it is left out. */
		if (proxy->tail[index] != '\0') {
			text[used] = proxy->tail[index];
			used++;
		}
	}
	text[used] = '\0';

	kl_mutex_unlock(&proxy->lock);
}

/*
 * Stops a command's run: the program and every program of its group end,
 * the errors' thread ends, and the run's state goes.  The caller has
 * closed its end of the socket pair, which the program reads as the end.
 */
void
proxy_stop(
	struct proxy *proxy)
{
	/* No run. */
	if (proxy == NULL)
		return;

	/* The run's state goes, the program ended first. */
	proxy_free(proxy);
}

/*
 * Finds the program the words name: the first word, or the second after
 * an "exec": a path from the root as it is, or a name without a folder
 * looked for in PATH.  Returns 0 with its path and *first its word's
 * index, or an error with a line about it.
 */
static int
proxy_find(
	char **words,
	unsigned count,
	char *program,
	size_t program_size,
	unsigned *first,
	char *error,
	size_t error_size)
{
	int status;

	/* The word, after an exec a shell would have run it with. */
	*first = 0U;
	if (count > 1U && strcmp(words[0], PROXY_EXEC) == 0)
		*first = 1U;

	/* The program's path. */
	status = mikuterm_program_find(words[*first], program, program_size);
	if (status == ENOENT)
		(void)snprintf(error, error_size, "ProxyCommand: %s: no such program", words[*first]);
	else if (status != 0)
		(void)snprintf(error, error_size, "ProxyCommand: %s: not a program's name or path (a path from the root, or a name in PATH)", words[*first]);
	return status;
}

/*
 * Starts the program in a process group of its own, with connection as
 * its standard input and output and errors as its standard error, the
 * signals as a new program has them, and every other descriptor closed.
 * Returns 0 or posix_spawn's error.
 */
static int
proxy_spawn(
	struct proxy *proxy,
	const char *program,
	char **arguments,
	int connection,
	int errors)
{
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	sigset_t defaults;
	sigset_t none;
	pid_t pid;
	int error;

	/* Its three standard descriptors, and none other. */
	error = posix_spawn_file_actions_init(&actions);
	if (error != 0)
		return error;
	error = posix_spawn_file_actions_adddup2(&actions, connection, STDIN_FILENO);
	if (error == 0)
		error = posix_spawn_file_actions_adddup2(&actions, connection, STDOUT_FILENO);
	if (error == 0)
		error = posix_spawn_file_actions_adddup2(&actions, errors, STDERR_FILENO);
	if (error == 0)
		error = posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);
	if (error != 0) {
		(void)posix_spawn_file_actions_destroy(&actions);
		return error;
	}

	/* Its own process group, and the signals of a program started anew (mikuterm ignores SIGPIPE). */
	error = posix_spawnattr_init(&attributes);
	if (error != 0) {
		(void)posix_spawn_file_actions_destroy(&actions);
		return error;
	}
	(void)sigfillset(&defaults);
	(void)sigdelset(&defaults, SIGKILL);
	(void)sigdelset(&defaults, SIGSTOP);
	(void)sigemptyset(&none);
	error = posix_spawnattr_setsigdefault(&attributes, &defaults);
	if (error == 0)
		error = posix_spawnattr_setsigmask(&attributes, &none);
	if (error == 0)
		error = posix_spawnattr_setpgroup(&attributes, 0);
	if (error == 0)
		error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP);

	/* The program. */
	if (error == 0)
		error = posix_spawn(&pid, program, &actions, &attributes, arguments, environ);
	(void)posix_spawnattr_destroy(&attributes);
	(void)posix_spawn_file_actions_destroy(&actions);
	if (error != 0)
		return error;

	/* Succeeded: it runs. */
	proxy->pid = pid;
	return 0;
}

/*
 * Makes a pair of connected loopback sockets: *ours does not block,
 * *theirs is the program's.  Returns 0, EIO when the end accepted is not
 * this side's, or the sockets' error.
 */
static int
proxy_pair(
	struct kl_socket **ours,
	struct kl_socket **theirs)
{
	struct kl_socket *listening;
	struct kl_socket *connecting;
	struct kl_socket *accepted;
	struct kl_address address;
	struct kl_address peer;
	struct kl_address mine;
	int differs;
	int error;

	/* A socket listening on a port of the loopback the system chooses. */
	*ours = NULL;
	*theirs = NULL;
	error = kl_socket_open(KL_ADDRESS_IPV4, KL_SOCKET_STREAM, 0U, &listening);
	if (error != 0)
		return error;
	kl_address_loopback(KL_ADDRESS_IPV4, 0U, &address);
	error = kl_socket_bind(listening, &address);
	if (error == 0)
		error = kl_socket_listen(listening, 1U);
	if (error == 0)
		error = kl_socket_local_address(listening, &address);
	if (error != 0) {
		kl_socket_close(listening);
		return error;
	}

	/* The connecting end. */
	error = kl_socket_open(KL_ADDRESS_IPV4, KL_SOCKET_STREAM, 0U, &connecting);
	if (error != 0) {
		kl_socket_close(listening);
		return error;
	}
	error = kl_socket_connect(connecting, &address);
	if (error == 0)
		error = kl_socket_local_address(connecting, &mine);
	if (error != 0) {
		kl_socket_close(connecting);
		kl_socket_close(listening);
		return error;
	}

	/* The accepted end; the listening one goes. */
	error = kl_socket_accept(listening, 0U, &accepted, &peer);
	kl_socket_close(listening);
	if (error != 0) {
		kl_socket_close(connecting);
		return error;
	}

	/* The accepted connection must be this side's own: its family, its port and its address. */
	differs = memcmp(peer.bytes, mine.bytes, sizeof(peer.bytes));
	if (peer.family != mine.family ||
	    peer.port != mine.port ||
	    differs != 0) {
		kl_socket_close(accepted);
		kl_socket_close(connecting);
		return EIO;
	}

	/* The caller's end does not block; small packets go at once both ways. */
	error = kl_socket_set_nonblocking(connecting, 1);
	if (error != 0) {
		kl_socket_close(accepted);
		kl_socket_close(connecting);
		return error;
	}
	(void)kl_socket_set_option(connecting, KL_SOCKET_NO_DELAY, 1);
	(void)kl_socket_set_option(accepted, KL_SOCKET_NO_DELAY, 1);

	/* Succeeded: the pair. */
	*ours = connecting;
	*theirs = accepted;
	return 0;
}

/*
 * The errors' thread: reads what the program writes on its errors until
 * its end or the run's stop, keeping the last PROXY_ERRORS_MAX bytes.
 */
static void
proxy_from_errors(
	void *argument)
{
	struct proxy *proxy;
	struct pollfd wait;
	unsigned char chunk[PROXY_CHUNK];
	ssize_t got;
	size_t keep;
	size_t drop;
	int ready;
	int stopping;

	/* Round by round, until the end of the pipe or the stop. */
	proxy = argument;
	for (;;) {
		/* The stop. */
		kl_mutex_lock(&proxy->lock);

		stopping = proxy->stopping;

		kl_mutex_unlock(&proxy->lock);

		if (stopping)
			break;

		/* Something to read, or a round's wait. */
		wait.fd = proxy->errors;
		wait.events = POLLIN;
		wait.revents = 0;
		ready = poll(&wait, 1U, PROXY_ROUND_MS);
		if (ready < 0 && errno != EINTR)
			break;
		if (ready <= 0)
			continue;

		/* What came; the end of the pipe ends the thread. */
		got = read(proxy->errors, chunk, sizeof(chunk));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;

		/* The tail: the oldest bytes go to make room. */
		kl_mutex_lock(&proxy->lock);

		keep = (size_t)got;
		if (keep > PROXY_ERRORS_MAX)
			keep = PROXY_ERRORS_MAX;
		drop = 0U;
		if (proxy->tail_size + keep > PROXY_ERRORS_MAX)
			drop = proxy->tail_size + keep - PROXY_ERRORS_MAX;
		memmove(proxy->tail, proxy->tail + drop, proxy->tail_size - drop);
		proxy->tail_size -= drop;
		memcpy(proxy->tail + proxy->tail_size, chunk + (size_t)got - keep, keep);
		proxy->tail_size += keep;

		kl_mutex_unlock(&proxy->lock);
	}
}

/*
 * Ends the program's process group: asked to end, then, after
 * PROXY_END_MS without its end, killed; the program is reaped.
 */
static void
proxy_end(
	struct proxy *proxy)
{
	unsigned waited;
	pid_t done;
	int status;

	/* Nothing runs. */
	if (proxy->pid <= 0)
		return;

	/* The group is asked to end. */
	(void)kill(-proxy->pid, SIGTERM);

	/* Its end, for a moment. */
	for (waited = 0U; waited < PROXY_END_MS; waited += PROXY_END_ROUND_MS) {
		done = waitpid(proxy->pid, &status, WNOHANG);
		if (done == proxy->pid || (done < 0 && errno != EINTR)) {
			(void)kill(-proxy->pid, SIGKILL);
			proxy->pid = 0;
			return;
		}
		kl_thread_sleep(PROXY_END_ROUND_MS);
	}

	/* Then the end of the whole group. */
	(void)kill(-proxy->pid, SIGKILL);
	do {
		done = waitpid(proxy->pid, &status, 0);
	} while (done < 0 && errno == EINTR);
	proxy->pid = 0;
}

/* Frees a run: the program ended, the errors' thread stopped, the pipe and the lock gone. */
static void
proxy_free(
	struct proxy *proxy)
{
	/* The program and its group. */
	proxy_end(proxy);

	/* The errors' thread, told to stop. */
	if (proxy->errors_thread != NULL) {
		kl_mutex_lock(&proxy->lock);

		proxy->stopping = 1;

		kl_mutex_unlock(&proxy->lock);

		(void)kl_thread_join(proxy->errors_thread);
	}

	/* The pipe, the lock and the state. */
	if (proxy->errors >= 0)
		(void)close(proxy->errors);
	kl_mutex_destroy(&proxy->lock);
	free(proxy);
}
