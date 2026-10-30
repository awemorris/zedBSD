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
 * runs as root, through the kernel's /dev/thermal.  The answer is the
 * line thermal-rules.c makes: OK with what the kernel reported, FAIL
 * wheel, or ERROR and a word.  Every request is logged to the
 * authentication log.
 */

#include "sessiond.h"
#include "thermal-rules.h"
#include "../../base/common/account.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
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
 * Hands a decided request to the kernel and reports, as
 * SESSIOND_THERMAL_* bits, what it said beyond its errno.
 *
 * The kernel's /dev/thermal has not yet published the ioctls that choose
 * a strategy or a boost, so every request finds the device unavailable
 * (ENODEV, answered "ERROR unavailable") until they are.
 */
static int
thermal_carry(
	const struct sessiond_thermal_request *request,
	unsigned *flags)
{
	(void)request;

	/* No device to hand the request to yet. */
	*flags = 0;
	return ENODEV;
}
