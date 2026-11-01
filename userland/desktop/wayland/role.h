/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compositor's role, decided once from the options (role.c, WS110):
 * a user's desktop (the default, and --session, which says the same), a
 * finite test run (--testing, with --timeout and --max-frames), or the
 * login screen (--greeter).  It knows nothing of the server: main.c hands
 * it what the options asked for and applies what it decides, so the host
 * tests run it alone.
 */

#ifndef KWL_ROLE_H
#define KWL_ROLE_H

#include <stdint.h>

/* The roles. */
#define KWL_ROLE_NORMAL		0U
#define KWL_ROLE_TESTING	1U
#define KWL_ROLE_GREETER	2U

/* How long a test run lasts when --timeout does not say. */
#define KWL_ROLE_TESTING_TIMEOUT_MS	150000U

/*
 * What the options asked for: a flag for each option that bears on the
 * role (given or not), and the --timeout value in milliseconds.
 */
struct kwl_role_request {
	unsigned testing;
	unsigned session;
	unsigned greeter;
	unsigned control_fd;
	unsigned locked;
	unsigned lock_idle;
	unsigned timeout;
	unsigned max_frames;
	uint64_t timeout_ms;
};

/*
 * What was decided: the role, the deadline (UINT64_MAX for none), and for
 * a refusal the sentence that says why.
 */
struct kwl_role {
	unsigned role;
	uint64_t timeout_ms;
	const char *refusal;
};

int kwl_role_resolve(const struct kwl_role_request *request, struct kwl_role *role);
const char *kwl_role_name(unsigned role);

#endif
