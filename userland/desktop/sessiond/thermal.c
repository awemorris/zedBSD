/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A session's THERMAL request: "THERMAL strategy ..." or "THERMAL boost
 * ..." on the session's socket, decided by thermal-rules.c (the words,
 * root or wheel), then carried out on the user's behalf by sessiond, which
 * runs as root, through /sbin/thermal and the kernel's /dev/thermal.  The
 * answer is the line thermal-rules.c makes: OK with what the kernel
 * reported, FAIL wheel, or ERROR and a word.  Every request is logged to
 * the authentication log.
 *
 * sessiond carries a request out by running /sbin/thermal, so that a
 * strategy a session chooses is kept for the next boot by the one program
 * that keeps it.
 */

#include "sessiond.h"
#include "thermal-rules.h"
#include "../../base/common/account.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

static int thermal_carry(const struct sessiond_thermal_request *request, unsigned *flags);

/*
 * Answers a session's THERMAL request (arguments: the line after
 * "THERMAL ") on its socket.
 */
void
sessiond_thermal(
	const struct sessiond_account *account,
	int control,
	const char *arguments)
{
	struct sessiond_thermal_request request;
	char answer[SESSIOND_THERMAL_ANSWER_MAX];
	const char *name;
	unsigned flags;
	int in_wheel;
	int decided;
	int error;
	int length;

	/* Decided: the words and the user's right. */
	name = account->passwd.pw_name;
	in_wheel = account_in_wheel(name, account->passwd.pw_gid);
	decided = sessiond_thermal_decide(arguments, account->passwd.pw_uid, in_wheel, &request);

	/* A refusal is logged, and the request goes no further. */
	flags = 0;
	error = decided;
	if (decided == EPERM) {
		sessiond_log("SESSIOND THERMAL \"%.40s\" by %s refused (not in wheel)", arguments, name);
		syslog(LOG_WARNING, "thermal request \"%.40s\" by %s refused (not in wheel)", arguments, name);
	} else if (decided != 0) {
		sessiond_log("SESSIOND THERMAL \"%.40s\" by %s refused (not a request)", arguments, name);
	} else {
		/* Carried out on the user's behalf; the outcome is logged with who asked. */
		error = thermal_carry(&request, &flags);
		sessiond_log("SESSIOND THERMAL %.40s by %s: errno=%d flags=%#x", arguments, name, error, flags);
		syslog(LOG_NOTICE, "thermal %.40s by %s: errno=%d", arguments, name, error);
	}

	/* The answer line. */
	length = sessiond_thermal_answer(error, flags, answer, sizeof(answer));
	if (length > 0)
		(void)write(control, answer, (size_t)length);
}

/*
 * Hands a decided request to /sbin/thermal, which asks the kernel and
 * keeps a chosen strategy for the next boot, and reports its errno and,
 * as SESSIOND_THERMAL_* bits, what the kernel said beyond it.  sessiond
 * waits for the command, which waits for the drivers up to five seconds.
 */
static int
thermal_carry(
	const struct sessiond_thermal_request *request,
	unsigned *flags)
{
	struct sessiond_thermal_command command;
	char output[SESSIOND_THERMAL_OUTPUT_MAX];
	size_t filled;
	ssize_t got;
	pid_t child;
	pid_t waited;
	int channel[2];
	int exit_code;
	int status;
	int error;

	/* The command line, and a pipe for what it prints. */
	*flags = 0;
	sessiond_thermal_command(request, &command);
	error = pipe(channel);
	if (error != 0)
		return EIO;

	/* Starts the child that runs the command. */
	child = fork();
	if (child < 0) {
		(void)close(channel[0]);
		(void)close(channel[1]);
		return EIO;
	}

	/* The child runs the command with both its outputs on the pipe. */
	if (child == 0) {
		(void)dup2(channel[1], STDOUT_FILENO);
		(void)dup2(channel[1], STDERR_FILENO);
		(void)close(channel[0]);
		(void)close(channel[1]);
		(void)execv(SESSIOND_THERMAL_COMMAND, command.argv);
		_exit(SESSIOND_THERMAL_NOT_RUN);
	}

	/* Only the child writes the pipe, so its end closes when the command ends. */
	(void)close(channel[1]);

	/* Reads what it prints, as much as fits, until it closes the pipe. */
	filled = 0;
	for (;;) {
		got = read(channel[0], output + filled, sizeof(output) - 1U - filled);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		filled += (size_t)got;
		if (filled == sizeof(output) - 1U)
			break;
	}

	/* The output read so far, as text. */
	output[filled] = '\0';
	(void)close(channel[0]);

	/* Waits for it to end; a command ended by a signal said nothing of how it went. */
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	exit_code = -1;
	if (waited == child && WIFEXITED(status))
		exit_code = WEXITSTATUS(status);

	/* Reads the outcome from how it ended and what it printed. */
	error = sessiond_thermal_outcome(exit_code, output, flags);
	if (error != 0)
		return error;

	/* Succeeded: the kernel took the request. */
	return 0;
}
