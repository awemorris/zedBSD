/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The part useradd, usermod and userdel share (ws196-p003): root's
 * request to account-admin, its run, and its answer.
 *
 * The request is root's form (docs/architecture/security.md, "Request
 * format"): the operation's line, then one "KEY VALUE" line a key.  A
 * value is passed as it was given; account-admin alone judges it by its
 * rules, so the terminal and Settings never disagree.  This side refuses
 * only what would break the form: a value holding a line break, which
 * would read as another key, and a request longer than account-admin
 * takes.
 *
 * account-admin runs as a child with the request on its standard input.
 * Its one answer line on standard output, "ok" or "error REASON", is
 * turned into a message on standard error and an exit status
 * (docs/reference/user-administration.md, "Exit status").  The environment
 * passes through, so that account-admin can log the user who ran sudo.
 */

#include "front.h"

#include "userland/base/common/account.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* The room of account-admin's answer: one short line. */
#define FRONT_ANSWER_MAX	128U

/* The status of a child that could not run account-admin. */
#define FRONT_EXEC_FAILED	127

/*
 * One reason account-admin may answer: its word, the exit status it
 * becomes, and what the message says of the user.
 */
struct front_reason {
	const char *word;
	int status;
	const char *message;
};

/*
 * account-admin's reasons for a request of root's form.  Settings' own
 * reasons (not-administrator, bad-password, weak-password, self) never
 * come back to root and take the general refusal.
 */
static const struct front_reason front_reasons[] = {
	{ "bad-request", USER_FRONT_USAGE, "account-admin did not take the request" },
	{ "bad-value", USER_FRONT_BAD_VALUE, "a value is outside its rules" },
	{ "bad-name", USER_FRONT_BAD_VALUE, "the name is not a valid user name" },
	{ "id-taken", USER_FRONT_ID_TAKEN, "the user ID is used by a user or a group" },
	{ "no-such-user", USER_FRONT_NO_SUCH_USER, "there is no such user" },
	{ "busy", USER_FRONT_BUSY, "the user has a running process" },
	{ "name-taken", USER_FRONT_NAME_TAKEN, "the name is used by a user or a group" },
	{ "home-exists", USER_FRONT_HOME, "the home is already there" },
	{ "home-failed", USER_FRONT_HOME, "the user is added, but the home could not be made" },
	{ "root", USER_FRONT_FAILED, "root and the system accounts are not changed" },
	{ "last-administrator", USER_FRONT_FAILED, "no administrator who can log in would be left" },
	{ "no-password", USER_FRONT_FAILED, "no password is behind the lock; set one with passwd" },
	{ "failed", USER_FRONT_FAILED, "the account files could not be changed; the system log says why" }
};

static void front_standard_descriptors(void);
static int front_run(const struct user_front_request *request, char *answer, size_t size);
static void front_child(const int *input, const int *output);
static void front_write(int descriptor, const char *text, size_t length);
static void front_read(int descriptor, char *answer, size_t size);
static void front_wait(pid_t child);
static int front_answer(const char *program, const char *name, const char *answer);
static int front_refusal(const char *program, const char *name, const char *answer);
static void front_close_pair(const int *pair);

/*
 * Checks that root runs the command, with descriptors 0 to 2 open.
 *
 * Returns USER_FRONT_OK, or USER_FRONT_FAILED after the message when the
 * real user ID is not 0: account-admin takes root's form only from root,
 * and the command is not set-user-ID.
 */
int
user_front_start(
	const char *program)
{
	uid_t real;

	/* Nothing the command opens later lands on a closed standard descriptor. */
	front_standard_descriptors();

	/* Root, directly or through sudo. */
	real = getuid();
	if (real != 0) {
		fprintf(stderr, "%s: Permission denied.\n", program);
		return USER_FRONT_FAILED;
	}

	/* Succeeded: root runs the command. */
	return USER_FRONT_OK;
}

/*
 * Begins a request of root's form with its operation's line.
 */
void
user_front_begin(
	struct user_front_request *request,
	const char *program,
	const char *operation)
{
	int length;

	/* The command, for the messages. */
	request->program = program;

	/* The operation's line, which always fits. */
	length = snprintf(request->text, sizeof(request->text), "%s\n", operation);
	request->length = (size_t)length;
}

/*
 * Adds one "KEY VALUE" line to a request; a value not given (NULL) adds
 * nothing.
 *
 * Returns USER_FRONT_OK, or USER_FRONT_BAD_VALUE after the message when
 * the value holds a line break or the request would grow longer than
 * account-admin takes.
 */
int
user_front_key(
	struct user_front_request *request,
	const char *key,
	const char *value)
{
	const char *line_break;
	size_t room;
	int length;

	/* A key not given. */
	if (value == NULL)
		return USER_FRONT_OK;

	/* A line break would end the value and start another key. */
	line_break = strchr(value, '\n');
	if (line_break != NULL) {
		fprintf(stderr, "%s: the %s may not hold a line break\n", request->program, key);
		return USER_FRONT_BAD_VALUE;
	}

	/* The line, after the text so far. */
	room = sizeof(request->text) - request->length;
	length = snprintf(request->text + request->length, room, "%s %s\n", key, value);
	if (length < 0 || (size_t)length >= room) {
		request->text[request->length] = '\0';
		fprintf(stderr, "%s: the %s is too long\n", request->program, key);
		return USER_FRONT_BAD_VALUE;
	}

	/* The request now ends after the line. */
	request->length += (size_t)length;

	/* Succeeded: the key is in the request. */
	return USER_FRONT_OK;
}

/*
 * Runs account-admin with a request and reports its answer about the
 * user named.
 *
 * Returns USER_FRONT_OK when account-admin made the change, or the exit
 * status of its refusal after the message (USER_FRONT_FAILED when it gave
 * no answer at all).
 */
int
user_front_send(
	const struct user_front_request *request,
	const char *name)
{
	char answer[FRONT_ANSWER_MAX];
	const char *message;
	int status;
	int error;

	/* account-admin, run with the request; it may stop reading early, which its answer explains. */
	(void)signal(SIGPIPE, SIG_IGN);
	error = front_run(request, answer, sizeof(answer));
	if (error != 0) {
		message = strerror(error);
		fprintf(stderr, "%s: %s could not be run: %s\n", request->program, ACCOUNT_ADMIN_PATH, message);
		return USER_FRONT_FAILED;
	}

	/* What the answer means to root. */
	status = front_answer(request->program, name, answer);
	if (status != USER_FRONT_OK)
		return status;

	/* Succeeded: account-admin made the change. */
	return USER_FRONT_OK;
}

/*
 * Opens /dev/null on any of descriptors 0 to 2 that is closed, so that the
 * pipes to account-admin are never made there.
 */
static void
front_standard_descriptors(void)
{
	int descriptor;
	int flags;
	int index;

	/* Each of the three. */
	for (index = 0; index < 3; index++) {
		/* Open already. */
		flags = fcntl(index, F_GETFD);
		if (flags != -1)
			continue;

		/* /dev/null in its place, or nothing can be done safely. */
		descriptor = open("/dev/null", O_RDWR);
		if (descriptor < 0)
			_exit(USER_FRONT_FAILED);
	}
}

/*
 * Runs account-admin as a child, writes the request to it, and reads its
 * answer line (without its end; empty when there was none).  Returns 0, or
 * the error that kept the child from being started.
 */
static int
front_run(
	const struct user_front_request *request,
	char *answer,
	size_t size)
{
	int input[2];
	int output[2];
	pid_t child;
	int error;

	/* The pipe the request goes through. */
	error = pipe(input);
	if (error != 0)
		return errno;

	/* The pipe the answer comes back through. */
	error = pipe(output);
	if (error != 0) {
		error = errno;
		front_close_pair(input);
		return error;
	}

	/* The child that becomes account-admin. */
	child = fork();
	if (child < 0) {
		error = errno;
		front_close_pair(input);
		front_close_pair(output);
		return error;
	}

	/* In the child, account-admin itself; it never comes back. */
	if (child == 0)
		front_child(input, output);

	/* The child's ends are the child's alone, so that each pipe ends when one side is done. */
	(void)close(input[0]);
	(void)close(output[1]);

	/* The request, then its end. */
	front_write(input[1], request->text, request->length);
	(void)close(input[1]);

	/* The answer, to the end of account-admin's output. */
	front_read(output[0], answer, size);
	(void)close(output[0]);

	/* account-admin's end, so that no child is left behind. */
	front_wait(child);

	/* Succeeded: the answer, if any, is read. */
	return 0;
}

/*
 * In the child: puts the request's pipe on standard input and the
 * answer's on standard output, and becomes account-admin.  Never returns.
 */
static void
front_child(
	const int *input,
	const int *output)
{
	char program_name[] = "account-admin";
	char *arguments[2];
	int error;

	/* The request as standard input. */
	error = dup2(input[0], 0);
	if (error < 0)
		_exit(FRONT_EXEC_FAILED);

	/* The answer as standard output. */
	error = dup2(output[1], 1);
	if (error < 0)
		_exit(FRONT_EXEC_FAILED);

	/* The pipes' own descriptors, now copied (they are above 2, as the standard ones are open). */
	front_close_pair(input);
	front_close_pair(output);

	/* account-admin decides for itself what a broken pipe does. */
	(void)signal(SIGPIPE, SIG_DFL);

	/* account-admin, with the environment as it is (its SUDO_USER is logged). */
	arguments[0] = program_name;
	arguments[1] = NULL;
	(void)execv(ACCOUNT_ADMIN_PATH, arguments);

	/* It could not be run: the parent reads no answer. */
	_exit(FRONT_EXEC_FAILED);
}

/*
 * Writes the whole of a text to a descriptor, stopping at the first
 * failure: account-admin that stops reading still answers why.
 */
static void
front_write(
	int descriptor,
	const char *text,
	size_t length)
{
	size_t done;
	ssize_t count;

	/* Each piece the pipe takes. */
	done = 0;
	while (done < length) {
		/* As much as the pipe takes now. */
		count = write(descriptor, text + done, length - done);
		if (count < 0) {
			/* A signal came first: the same piece again. */
			if (errno == EINTR)
				continue;

			/* The reader is gone. */
			return;
		}

		/* The part written. */
		done += (size_t)count;
	}
}

/*
 * Reads a descriptor to its end into an answer, keeping the first line
 * without its end (cut to the answer's room); anything after it is read
 * and dropped, so that the writer is never held.
 */
static void
front_read(
	int descriptor,
	char *answer,
	size_t size)
{
	char scrap[FRONT_ANSWER_MAX];
	char *line_end;
	size_t filled;
	ssize_t count;

	/* The answer's room, filled to its end or to the output's. */
	filled = 0;
	for (;;) {
		/* Into the answer while it has room, else into the scrap. */
		if (filled + 1U < size) {
			count = read(descriptor, answer + filled, size - 1U - filled);
		} else {
			count = read(descriptor, scrap, sizeof(scrap));
		}

		/* A signal came first: read again. */
		if (count < 0 && errno == EINTR)
			continue;

		/* The output's end, or a failure that ends it as well. */
		if (count <= 0)
			break;

		/* What came into the answer. */
		if (filled + 1U < size)
			filled += (size_t)count;
	}

	/* The first line alone. */
	answer[filled] = '\0';
	line_end = strchr(answer, '\n');
	if (line_end != NULL)
		*line_end = '\0';
}

/*
 * Waits for account-admin's end.
 */
static void
front_wait(
	pid_t child)
{
	pid_t waited;
	int status;

	/* Until it has ended, through any signal that comes first. */
	for (;;) {
		/* The child's end. */
		waited = waitpid(child, &status, 0);
		if (waited == child)
			return;

		/* A signal came first: wait again. */
		if (errno == EINTR)
			continue;

		/* No such child any more. */
		return;
	}
}

/*
 * Turns account-admin's answer about a user into the exit status and, for
 * a refusal, its message.
 */
static int
front_answer(
	const char *program,
	const char *name,
	const char *answer)
{
	int status;
	int same;

	/* Anything but "ok" is a refusal, said and reported. */
	same = strcmp(answer, "ok");
	if (same != 0) {
		status = front_refusal(program, name, answer);
		return status;
	}

	/* Succeeded: the change is made. */
	return USER_FRONT_OK;
}

/*
 * Says why account-admin refused a request about a user, and reports the
 * refusal's exit status (never USER_FRONT_OK).
 */
static int
front_refusal(
	const char *program,
	const char *name,
	const char *answer)
{
	const char *word;
	size_t count;
	size_t i;
	int same;

	/* Not account-admin's answer at all: it could not be run, or it was stopped. */
	same = strncmp(answer, "error ", 6U);
	if (same != 0) {
		fprintf(stderr, "%s: %s gave no answer\n", program, ACCOUNT_ADMIN_PATH);
		return USER_FRONT_FAILED;
	}

	/* The reason's word, among the known ones. */
	word = answer + 6;
	count = sizeof(front_reasons) / sizeof(front_reasons[0]);
	for (i = 0; i < count; i++) {
		/* This reason, with its message and its status. */
		same = strcmp(word, front_reasons[i].word);
		if (same == 0) {
			fprintf(stderr, "%s: %s: %s\n", program, name, front_reasons[i].message);
			return front_reasons[i].status;
		}
	}

	/* A reason this command does not know: a refusal all the same. */
	fprintf(stderr, "%s: %s: account-admin refused: %s\n", program, name, word);

	/* Reports the general refusal. */
	return USER_FRONT_FAILED;
}

/*
 * Closes both descriptors of a pipe.
 */
static void
front_close_pair(
	const int *pair)
{
	/* The read end, then the write end. */
	(void)close(pair[0]);
	(void)close(pair[1]);
}
