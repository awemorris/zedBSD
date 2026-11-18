/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The account on zedBSD (ws160-p002): the user's password changed by
 * passwd's batch mode (userland/base/passwd, ws160-p001); and the
 * administration of the people's accounts (ws089-p026) by account-admin
 * (userland/base/account-admin), which reads the caller's password and the
 * operation's lines on its standard input and answers one line ("ok" or
 * "error WORD") on its standard output, run the same way.
 *
 * passwd -s runs as a child with its standard input a pipe and its output
 * and errors thrown away; the current and the new password go down the
 * pipe one a line, and its exit status says how it went (account.h).
 * passwd is set-user-ID root and changes the account of its real user ID,
 * the compositor's own user: the compositor holds no privilege.  The child
 * runs only async-signal-safe calls before its exec, as the compositor has
 * threads.  The lines are wiped after they are written.  SIGPIPE, when
 * passwd ends before it read them, is held for this thread and taken back
 * before the thread goes on.
 *
 * The people's accounts as Settings lists them (ws188-p002) are read by
 * the shared machine/users.c with zedBSD's group of administrators.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include "userland/base/common/account.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The lines' room: two passwords, their ends and a NUL. */
#define ACCOUNT_LINES		(2U * (ACCOUNT_PASSWORD_MAX + 1U) + 1U)

/* The longest operation given account-admin (ACCOUNT_ADMIN_PATH, the base's account.h), and the longest answer read. */
#define ACCOUNT_OPERATION_MAX	1024U
#define ACCOUNT_ANSWER_MAX	64U

static int account_write(int descriptor, const char *text, size_t length);
static size_t account_read_answer(int descriptor, char *answer, size_t size);
static void account_wipe(char *text, size_t size);

/*
 * Changes the password of the compositor's user from current to fresh.
 */
int
kl_backend_account_set_password(
	const char *current,
	const char *fresh)
{
	char lines[ACCOUNT_LINES];
	char *argv[3];
	struct timespec none;
	sigset_t pipe_signal;
	sigset_t previous;
	size_t current_length;
	size_t fresh_length;
	size_t current_clean;
	size_t fresh_clean;
	size_t length;
	pid_t child;
	pid_t waited;
	int descriptors[2];
	int null;
	int status;
	int exited;
	int error;

	/* Two passwords that are not empty, fit and hold no line end. */
	current_length = strlen(current);
	fresh_length = strlen(fresh);
	if (current_length == 0U || fresh_length == 0U)
		return EINVAL;
	if (current_length > ACCOUNT_PASSWORD_MAX || fresh_length > ACCOUNT_PASSWORD_MAX)
		return EINVAL;
	current_clean = strcspn(current, "\n");
	fresh_clean = strcspn(fresh, "\n");
	if (current_clean != current_length || fresh_clean != fresh_length)
		return EINVAL;

	/* The pipe passwd reads, closed on exec on this side. */
	error = pipe(descriptors);
	if (error != 0)
		return EIO;
	(void)fcntl(descriptors[1], F_SETFD, FD_CLOEXEC);

	/* SIGPIPE held for this thread while the lines go. */
	sigemptyset(&pipe_signal);
	sigaddset(&pipe_signal, SIGPIPE);
	(void)pthread_sigmask(SIG_BLOCK, &pipe_signal, &previous);

	/* passwd -s, its input the pipe, its output and errors thrown away. */
	argv[0] = "passwd";
	argv[1] = "-s";
	argv[2] = NULL;
	null = open("/dev/null", O_WRONLY | O_CLOEXEC);
	child = fork();
	if (child == 0) {
		/* Async-signal-safe calls only, then the exec. */
		(void)dup2(descriptors[0], STDIN_FILENO);
		if (null >= 0) {
			(void)dup2(null, STDOUT_FILENO);
			(void)dup2(null, STDERR_FILENO);
		}

		/* passwd. */
		(void)execv(ACCOUNT_PASSWD_PATH, argv);

		/* passwd could not run. */
		_exit(127);
	}

	/* The child's ends are its own now. */
	(void)close(descriptors[0]);
	if (null >= 0)
		(void)close(null);
	if (child < 0) {
		(void)close(descriptors[1]);
		(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
		return EIO;
	}

	/* The two lines, wiped once written. */
	length = (size_t)snprintf(lines, sizeof(lines), "%s\n%s\n", current, fresh);
	error = account_write(descriptors[1], lines, length);
	account_wipe(lines, sizeof(lines));
	(void)close(descriptors[1]);

	/* passwd's end. */
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);

	/* A SIGPIPE it raised is taken, and the mask is as it was. */
	none.tv_sec = 0;
	none.tv_nsec = 0;
	(void)sigtimedwait(&pipe_signal, NULL, &none);
	(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);

	/* passwd did not end by itself, or could not run. */
	if (waited < 0)
		return EIO;
	exited = WIFEXITED(status);
	if (!exited)
		return EIO;
	(void)error;

	/* Its exit status. */
	switch (WEXITSTATUS(status)) {
	case ACCOUNT_PASSWD_OK:
		return 0;
	case ACCOUNT_PASSWD_WRONG:
		return EACCES;
	case ACCOUNT_PASSWD_REFUSED:
	case ACCOUNT_PASSWD_MISMATCH:
		return EINVAL;
	default:
		return EIO;
	}
}

/*
 * Tells whether the system can administer the accounts: account-admin is
 * there and may run.
 */
int
kl_backend_account_can_administer(void)
{
	int status;

	/* The tool, runnable. */
	status = access(ACCOUNT_ADMIN_PATH, X_OK);
	if (status != 0)
		return 0;

	/* There. */
	return 1;
}

/*
 * Carries out an administrator's change through account-admin: the
 * password and the operation's lines on its input, its answer read; the
 * word of a refusal copied into reason.
 */
int
kl_backend_account_administer(
	const char *password,
	const char *operation,
	char *reason,
	size_t size)
{
	char lines[ACCOUNT_PASSWORD_MAX + ACCOUNT_OPERATION_MAX + 3U];
	char answer[ACCOUNT_ANSWER_MAX];
	char *argv[2];
	struct timespec none;
	sigset_t pipe_signal;
	sigset_t previous;
	size_t password_length;
	size_t operation_length;
	size_t password_clean;
	size_t length;
	size_t got;
	pid_t child;
	pid_t waited;
	int input[2];
	int output[2];
	int null;
	int status;
	int exited;
	int code;
	int same;
	int error;

	/* No word yet; a password of one line and an operation that fit, the operation ended by a line end. */
	if (reason != NULL && size != 0U)
		reason[0] = '\0';
	password_length = strlen(password);
	operation_length = strlen(operation);
	password_clean = strcspn(password, "\n");
	if (password_length == 0U || password_length > ACCOUNT_PASSWORD_MAX || password_clean != password_length)
		return EINVAL;
	if (operation_length == 0U || operation_length > ACCOUNT_OPERATION_MAX || operation[operation_length - 1U] != '\n')
		return EINVAL;

	/* The pipe the tool reads, closed on exec on this side. */
	error = pipe(input);
	if (error != 0)
		return EIO;
	(void)fcntl(input[1], F_SETFD, FD_CLOEXEC);

	/* The pipe it answers on, closed on exec on this side. */
	error = pipe(output);
	if (error != 0) {
		(void)close(input[0]);
		(void)close(input[1]);
		return EIO;
	}

	/* Closed on exec on this side. */
	(void)fcntl(output[0], F_SETFD, FD_CLOEXEC);

	/* SIGPIPE held for this thread while the lines go. */
	sigemptyset(&pipe_signal);
	sigaddset(&pipe_signal, SIGPIPE);
	(void)pthread_sigmask(SIG_BLOCK, &pipe_signal, &previous);

	/* account-admin, its input and output the pipes, its errors thrown away. */
	argv[0] = "account-admin";
	argv[1] = NULL;
	null = open("/dev/null", O_WRONLY | O_CLOEXEC);
	child = fork();
	if (child == 0) {
		/* Async-signal-safe calls only, then the exec. */
		(void)dup2(input[0], STDIN_FILENO);
		(void)dup2(output[1], STDOUT_FILENO);
		if (null >= 0)
			(void)dup2(null, STDERR_FILENO);

		/* The tool. */
		(void)execv(ACCOUNT_ADMIN_PATH, argv);

		/* It could not run. */
		_exit(127);
	}

	/* The child's ends are its own now. */
	(void)close(input[0]);
	(void)close(output[1]);
	if (null >= 0)
		(void)close(null);
	if (child < 0) {
		(void)close(input[1]);
		(void)close(output[0]);
		(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
		return EIO;
	}

	/* The password and the operation, wiped once written; the input closed, which ends the request. */
	length = (size_t)snprintf(lines, sizeof(lines), "%s\n%s", password, operation);
	error = account_write(input[1], lines, length);
	account_wipe(lines, sizeof(lines));
	(void)close(input[1]);

	/* Its answer, then its end. */
	got = account_read_answer(output[0], answer, sizeof(answer));
	(void)close(output[0]);
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);

	/* A SIGPIPE it raised is taken, and the mask is as it was. */
	none.tv_sec = 0;
	none.tv_nsec = 0;
	(void)sigtimedwait(&pipe_signal, NULL, &none);
	(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);

	/* The tool did not end by itself, or could not run, or said nothing. */
	(void)error;
	if (waited < 0)
		return EIO;
	exited = WIFEXITED(status);
	if (!exited || got == 0U)
		return EIO;

	/* "ok" with a clean exit: done. */
	same = strcmp(answer, "ok");
	code = WEXITSTATUS(status);
	if (same == 0 && code == 0)
		return 0;

	/* Anything but "error WORD" is a failure. */
	same = strncmp(answer, "error ", 6U);
	if (same != 0)
		return EIO;

	/* The word kept for the caller. */
	if (reason != NULL && size != 0U)
		(void)snprintf(reason, size, "%s", answer + 6);

	/* Not an administrator or a wrong password: denied. */
	same = strcmp(answer + 6, "not-administrator");
	if (same == 0)
		return EACCES;
	same = strcmp(answer + 6, "bad-password");
	if (same == 0)
		return EACCES;

	/* A failure of the tool. */
	same = strcmp(answer + 6, "failed");
	if (same == 0)
		return EIO;

	/* Any other refusal. */
	return EINVAL;
}

/*
 * Reads the people's accounts (ws188-p002): zedBSD's administrators are
 * wheel's members (account-admin's rule, docs/architecture/security.md).
 */
size_t
kl_backend_users_read(
	struct kl_backend_user *list,
	size_t capacity,
	unsigned *skipped)
{
	static const char *const admin_groups[] = { "wheel", NULL };
	size_t count;

	/* The shared reading with zedBSD's group. */
	count = kl_backend_users_posix(list, capacity, skipped, admin_groups);

	/* Succeeded: the accounts read. */
	return count;
}

/*
 * Reads the tool's answer line (its end and anything after it dropped)
 * into a buffer; returns its length (0 for none).
 */
static size_t
account_read_answer(
	int descriptor,
	char *answer,
	size_t size)
{
	ssize_t got;
	size_t used;
	char *end;

	/* As much as fits, to the end of the output. */
	used = 0;
	while (used + 1U < size) {
		got = read(descriptor, answer + used, size - 1U - used);

		/* Interrupted: again. */
		if (got < 0 && errno == EINTR)
			continue;

		/* An error or the end. */
		if (got <= 0)
			break;
		used += (size_t)got;
	}

	/* The first line alone. */
	answer[used] = '\0';
	end = strchr(answer, '\n');
	if (end != NULL)
		*end = '\0';

	/* Its length. */
	return strlen(answer);
}

/* Writes the whole text to the pipe; returns 0, or an errno value (EPIPE when passwd ended). */
static int
account_write(
	int descriptor,
	const char *text,
	size_t length)
{
	ssize_t written;
	size_t done;

	/* All of it. */
	done = 0;
	while (done < length) {
		written = write(descriptor, text + done, length - done);
		if (written < 0 && errno == EINTR)
			continue;
		if (written < 0)
			return errno;
		done += (size_t)written;
	}

	/* Succeeded: written. */
	return 0;
}

/* Overwrites text that held a password. */
static void
account_wipe(
	char *text,
	size_t size)
{
	volatile char *byte;
	size_t index;

	/* Byte by byte, so that the compiler keeps the writes. */
	byte = text;
	for (index = 0; index < size; index++)
		byte[index] = '\0';
}
