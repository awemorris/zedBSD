/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws110-p001: the host test of the compositor's role from its options
 * (userland/desktop/wayland/role.c).  Each case is a set of options and
 * the role, deadline or refusal they must give.  Prints one line a case
 * and the last line host-role: PASS or host-role: FAIL.
 */

#include "userland/desktop/wayland/role.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The options a case gives, by name. */
#define OPTION_TESTING		0x01U
#define OPTION_SESSION		0x02U
#define OPTION_GREETER		0x04U
#define OPTION_CONTROL_FD	0x08U
#define OPTION_LOCK_IDLE	0x10U
#define OPTION_TIMEOUT		0x20U
#define OPTION_MAX_FRAMES	0x40U
#define OPTION_LOCKED		0x80U

/* What a case expects: an error (0 or EINVAL), and when 0 the role and deadline. */
struct role_case {
	const char *name;
	unsigned options;
	int error;
	unsigned role;
	uint64_t timeout_ms;
};

int main(void);
static int run_case(const struct role_case *item);
static unsigned option_given(unsigned options, unsigned option);

/* The cases; a --timeout in them is 900 s. */
static const struct role_case cases[] = {
	{ "no options: a desktop without a deadline", 0U, 0, KWL_ROLE_NORMAL, UINT64_MAX },
	{ "--session: the same", OPTION_SESSION, 0, KWL_ROLE_NORMAL, UINT64_MAX },
	{ "--session --control-fd --lock-idle: sessiond's", OPTION_SESSION | OPTION_CONTROL_FD | OPTION_LOCK_IDLE, 0, KWL_ROLE_NORMAL, UINT64_MAX },
	{ "--testing: 150 s", OPTION_TESTING, 0, KWL_ROLE_TESTING, KWL_ROLE_TESTING_TIMEOUT_MS },
	{ "--testing --timeout: its own deadline", OPTION_TESTING | OPTION_TIMEOUT, 0, KWL_ROLE_TESTING, 900000U },
	{ "--testing --max-frames: 150 s and frames", OPTION_TESTING | OPTION_MAX_FRAMES, 0, KWL_ROLE_TESTING, KWL_ROLE_TESTING_TIMEOUT_MS },
	{ "--greeter: the login screen", OPTION_GREETER, 0, KWL_ROLE_GREETER, UINT64_MAX },
	{ "--timeout alone: refused", OPTION_TIMEOUT, EINVAL, 0U, 0U },
	{ "--max-frames alone: refused", OPTION_MAX_FRAMES, EINVAL, 0U, 0U },
	{ "--session --timeout: refused", OPTION_SESSION | OPTION_TIMEOUT, EINVAL, 0U, 0U },
	{ "--testing --session: refused", OPTION_TESTING | OPTION_SESSION, EINVAL, 0U, 0U },
	{ "--testing --greeter: refused", OPTION_TESTING | OPTION_GREETER, EINVAL, 0U, 0U },
	{ "--testing --control-fd: refused", OPTION_TESTING | OPTION_CONTROL_FD, EINVAL, 0U, 0U },
	{ "--testing --lock-idle: refused", OPTION_TESTING | OPTION_LOCK_IDLE, EINVAL, 0U, 0U },
	{ "--greeter --session: refused", OPTION_GREETER | OPTION_SESSION, EINVAL, 0U, 0U },
	{ "--greeter --timeout: refused", OPTION_GREETER | OPTION_TIMEOUT, EINVAL, 0U, 0U },
	{ "--session --control-fd --locked: sessiond's after a crash", OPTION_SESSION | OPTION_CONTROL_FD | OPTION_LOCKED, 0, KWL_ROLE_NORMAL, UINT64_MAX },
	{ "--locked without --control-fd: refused", OPTION_SESSION | OPTION_LOCKED, EINVAL, 0U, 0U },
	{ "--greeter --control-fd --locked: refused", OPTION_GREETER | OPTION_CONTROL_FD | OPTION_LOCKED, EINVAL, 0U, 0U },
};

/*
 * Runs every case.
 */
int
main(
	void)
{
	size_t i;
	int failed;
	int passed;

	/* Each case, counting the failures. */
	failed = 0;
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		/* One case. */
		passed = run_case(&cases[i]);
		if (!passed)
			failed++;
	}

	/* The verdict. */
	if (failed != 0) {
		printf("host-role: FAIL (%d)\n", failed);
		return 1;
	}

	/* Succeeded: every case gave what it expects. */
	printf("host-role: PASS\n");
	return 0;
}

/*
 * Resolves one case and compares what it gave with what it expects.
 * Returns 1 when they agree.
 */
static int
run_case(
	const struct role_case *item)
{
	struct kwl_role_request request;
	struct kwl_role role;
	int error;

	/* The request the options make. */
	memset(&request, 0, sizeof(request));
	request.testing = option_given(item->options, OPTION_TESTING);
	request.session = option_given(item->options, OPTION_SESSION);
	request.greeter = option_given(item->options, OPTION_GREETER);
	request.control_fd = option_given(item->options, OPTION_CONTROL_FD);
	request.lock_idle = option_given(item->options, OPTION_LOCK_IDLE);
	request.locked = option_given(item->options, OPTION_LOCKED);
	request.timeout = option_given(item->options, OPTION_TIMEOUT);
	request.max_frames = option_given(item->options, OPTION_MAX_FRAMES);
	request.timeout_ms = 900000U;

	/* The decision. */
	error = kwl_role_resolve(&request, &role);
	if (error != item->error) {
		printf("%s: FAIL (error %d)\n", item->name, error);
		return 0;
	}

	/* A refusal must say why. */
	if (error != 0 && role.refusal == NULL) {
		printf("%s: FAIL (no reason)\n", item->name);
		return 0;
	}

	/* An accepted case must give its role and deadline. */
	if (error == 0 && (role.role != item->role || role.timeout_ms != item->timeout_ms)) {
		printf("%s: FAIL (role %s, %llu ms)\n",
		       item->name,
		       kwl_role_name(role.role),
		       (unsigned long long)role.timeout_ms);
		return 0;
	}

	/* Prints the reason of a refusal, or the role. */
	if (error != 0)
		printf("%s: ok (%s)\n", item->name, role.refusal);
	else
		printf("%s: ok (%s)\n", item->name, kwl_role_name(role.role));

	/* Succeeded: as expected. */
	return 1;
}

/*
 * Says whether a case gives an option: 1 or 0, as the request holds it.
 */
static unsigned
option_given(
	unsigned options,
	unsigned option)
{
	/* The option is among the case's. */
	if ((options & option) != 0U)
		return 1U;

	/* It is not. */
	return 0U;
}
