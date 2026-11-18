/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compositor's role from its options (WS110; 2026-10-05 user: a
 * compositor started without a role is a user's desktop with no deadline,
 * and a test run says --testing).  The options are read first, in any
 * order, and the role is decided here once, so --testing --timeout=N and
 * --timeout=N --testing mean the same.
 */

#include "role.h"
#include <errno.h>
#include <stddef.h>

static const char *role_refusal(const struct kwl_role_request *request);

/*
 * Decides the role and the deadline from what the options asked for.
 * Returns 0, or EINVAL with role->refusal saying why when the options
 * contradict each other.
 */
int
kwl_role_resolve(
	const struct kwl_role_request *request,
	struct kwl_role *role)
{
	const char *refusal;

	/* Refuses options that ask for two roles, or a test's limits without the test. */
	role->refusal = NULL;
	refusal = role_refusal(request);
	if (refusal != NULL) {
		role->refusal = refusal;
		return EINVAL;
	}

	/* A test run ends at its deadline: the one given, else the default. */
	if (request->testing) {
		role->role = KWL_ROLE_TESTING;
		role->timeout_ms = KWL_ROLE_TESTING_TIMEOUT_MS;

		/* The deadline --timeout gave replaces the default. */
		if (request->timeout)
			role->timeout_ms = request->timeout_ms;

		/* Succeeded: a finite test run. */
		return 0;
	}

	/* The login screen and a user's desktop have no deadline. */
	role->timeout_ms = UINT64_MAX;
	role->role = KWL_ROLE_NORMAL;

	/* --greeter asks for the login screen; anything else is a desktop. */
	if (request->greeter)
		role->role = KWL_ROLE_GREETER;

	/* Succeeded: the role is decided. */
	return 0;
}

/*
 * Names a role as the compositor's READY line prints it.
 */
const char *
kwl_role_name(
	unsigned role)
{
	/* A test run. */
	if (role == KWL_ROLE_TESTING)
		return "testing";

	/* The login screen. */
	if (role == KWL_ROLE_GREETER)
		return "greeter";

	/* A user's desktop. */
	return "normal";
}

/*
 * Says why the options cannot be used together, or NULL when they can.
 */
static const char *
role_refusal(
	const struct kwl_role_request *request)
{
	/* A test run is not a login session, which sessiond starts. */
	if (request->testing && request->session)
		return "--testing and --session cannot be given together";

	/* Nor the login screen. */
	if (request->testing && request->greeter)
		return "--testing and --greeter cannot be given together";

	/* A session's descriptor to sessiond belongs to a login session. */
	if (request->testing && request->control_fd)
		return "--control-fd is for a login session, not --testing";

	/* So does the lock after idle time. */
	if (request->testing && request->lock_idle)
		return "--lock-idle is for a login session, not --testing";

	/* A session that starts locked is one sessiond started, which unlocks it. */
	if (request->locked && !request->control_fd)
		return "--locked needs --control-fd";
	if (request->locked && (request->testing || request->greeter))
		return "--locked is for a login session";

	/* The login screen is not a session. */
	if (request->greeter && request->session)
		return "--greeter and --session cannot be given together";

	/* A deadline is a test's: a desktop runs until it is logged out of. */
	if (!request->testing && request->timeout)
		return "--timeout needs --testing";

	/* So is a frame limit. */
	if (!request->testing && request->max_frames)
		return "--max-frames needs --testing";

	/* The options agree. */
	return NULL;
}
