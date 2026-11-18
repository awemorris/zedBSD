/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rules of POWER (ws131-p027; power-rules.h says what they are).
 *
 * The login screen may power off and restart (no one is logged in on the
 * console then).  A session's request comes only from the console's
 * session (its socket is the session's alone), and only root or a member
 * of wheel may end the machine (the 2026-10-06 user decision: wheel only,
 * whether or not other users are logged in).
 */

#include "power-rules.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/*
 * Gives the program a POWER word asks for ("poweroff" or "reboot"), or
 * NULL for any other word.
 */
const char *
sessiond_power_program(
	const char *what)
{
	int differs;

	/* Power off. */
	differs = strcmp(what, "poweroff");
	if (differs == 0)
		return SESSIOND_POWEROFF;

	/* Restart. */
	differs = strcmp(what, "reboot");
	if (differs == 0)
		return SESSIOND_REBOOT;

	/* Nothing else. */
	return NULL;
}

/*
 * Decides a session's POWER request: the word (EINVAL for another), and
 * the right (EPERM when the session's user is neither root nor a member of
 * wheel).  Returns 0 with the program to run.
 */
int
sessiond_power_decide(
	const char *what,
	uid_t uid,
	int in_wheel,
	const char **program)
{
	/* One of the two words. */
	*program = sessiond_power_program(what);
	if (*program == NULL)
		return EINVAL;

	/* Only root or a member of wheel. */
	if (uid != 0 && !in_wheel)
		return EPERM;

	/* Succeeded: the request may be carried out. */
	return 0;
}
