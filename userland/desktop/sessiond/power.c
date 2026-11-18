/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Ending the machine through sessiond (ws131-p027): the login screen's
 * "POWER poweroff|reboot" (greeter.c), and since ws131-p027 a session's
 * (the console session's Power Off dialog, ws099-p037), decided by
 * power-rules.c.  A session's answer is OK, "FAIL wheel" (the session's
 * user is neither root nor in wheel) or ERROR (another word, or the
 * program could not be started).  Every request is
 * logged to the authentication log.
 */

#include "sessiond.h"
#include "power-rules.h"
#include "../../base/common/account.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

/*
 * Whether a program that ends the machine was started (a sleep is then
 * refused, sleep.c).  sessiond's one thread sets and reads it.
 */
int sessiond_power_started;

/*
 * Runs the program that ends the machine (it asks init, which stops
 * sessiond among the rest), and logs who asked.  Returns 0, or the errno of
 * the fork.
 */
int
sessiond_power_run(
	const char *program,
	const char *what,
	const char *from)
{
	pid_t child;

	/* Logged first: the machine may be gone soon after. */
	sessiond_log("SESSIOND POWER %s from=%s", what, from);
	syslog(LOG_NOTICE, "%s from %s", what, from);

	/* The program in a child of its own. */
	child = fork();
	if (child < 0)
		return errno;
	if (child == 0) {
		(void)execl(program, program, (char *)NULL);
		_exit(127);
	}

	/* Succeeded: the machine is ending (no sleep from now on). */
	sessiond_power_started = 1;
	return 0;
}

/*
 * Answers a session's "POWER poweroff|reboot": decided by power-rules.c (root
 * or wheel), then carried out; "POWER suspend" and "POWER cancel" go to the
 * sleep (sleep.c), for any session user.
 */
void
sessiond_power_session(
	const struct sessiond_account *account,
	int control,
	const char *what)
{
	const char *program;
	const char *name;
	int in_wheel;
	int decided;
	int differs;
	int error;

	/* The sleep is any session user's (ws052-p011, the 2026-10-07 user decision N2); its cancel is never answered. */
	name = account->passwd.pw_name;
	differs = strcmp(what, "suspend");
	if (differs == 0) {
		sessiond_sleep_request(control, name);
		return;
	}
	differs = strcmp(what, "cancel");
	if (differs == 0) {
		sessiond_sleep_cancel();
		return;
	}

	/* Decided: the word and the user's right. */
	in_wheel = account_in_wheel(name, account->passwd.pw_gid);
	decided = sessiond_power_decide(what, account->passwd.pw_uid, in_wheel, &program);
	if (decided == EPERM) {
		sessiond_log("SESSIOND POWER %.16s by %s refused (not in wheel)", what, name);
		syslog(LOG_WARNING, "%.16s by %s refused (not in wheel)", what, name);
		(void)write(control, "FAIL wheel\n", 11U);
		return;
	}

	/* Another word is not one a session may ask. */
	if (decided != 0) {
		sessiond_log("SESSIOND POWER \"%.16s\" by %s refused", what, name);
		(void)write(control, "ERROR\n", 6U);
		return;
	}

	/* Carried out; the session hears that the machine is ending. */
	error = sessiond_power_run(program, what, name);
	if (error != 0) {
		(void)write(control, "ERROR\n", 6U);
		return;
	}

	/* Succeeded: OK. */
	(void)write(control, "OK\n", 3U);
}
