/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * WS333 p019: the host test of sessiond's THERMAL rules
 * (userland/desktop/sessiond/thermal-rules.c, compiled unchanged).  The
 * cases:
 *   1. the words: each named strategy, manual with its axes (0, 100, the
 *      middle), boost of every fan and of one, at the edges of the ranges;
 *   2. malformed words: empty, unknown, firmware, a named strategy with
 *      axes, manual with too few or too many axes, an axis past 100, signs,
 *      leading zeros, four digits, a space at an end or two together, a
 *      tab, a line too long, a boost without a level or past 255;
 *   3. the right: root, a member of wheel, another user (EPERM), and a
 *      malformed request from another user (EINVAL, words first), with
 *      the request cleared on every refusal;
 *   4. the answers: OK with each bit and all three, FAIL wheel, each ERROR
 *      word, a buffer too small.
 * Prints "host-thermal-rules: PASS N checks" or the failed checks and FAIL.
 *
 *   sh plan/ws333/tests/sessiond-thermal/run.sh
 */

#include "userland/desktop/sessiond/thermal-rules.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* How many checks ran and how many failed. */
static unsigned checks;
static unsigned failures;

/*
 * One request of the table: the words, the user, and what the rules are
 * to make of it (the errno and, on success, the request).
 */
struct decide_case {
	const char *arguments;
	unsigned uid;
	int in_wheel;
	int error;
	struct sessiond_thermal_request request;
};

static void check(int passed, const char *what);
static void case_decide(void);
static void case_answer(void);
static void check_answer(int error, unsigned flags, const char *expected);

/* Runs every case and prints the verdict. */
int
main(void)
{
	/* The cases. */
	case_decide();
	case_answer();

	/* Some check failed. */
	if (failures != 0U) {
		printf("host-thermal-rules: FAIL %u of %u checks\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check passed. */
	printf("host-thermal-rules: PASS %u checks\n", checks);
	return 0;
}

/* Counts one check and reports it when it failed. */
static void
check(
	int passed,
	const char *what)
{
	/* Counted; a failure is named. */
	checks++;
	if (!passed) {
		failures++;
		printf("FAIL: %s\n", what);
	}
}

/* Runs the table of requests through the rules. */
static void
case_decide(void)
{
	static const struct decide_case table[] = {
		/* 1. well-formed words, by root */
		{ "strategy performance", 0, 0, 0, { SESSIOND_THERMAL_STRATEGY, SESSIOND_THERMAL_PERFORMANCE, 0, 0, 0, 0, 0 } },
		{ "strategy balanced", 0, 0, 0, { SESSIOND_THERMAL_STRATEGY, SESSIOND_THERMAL_BALANCED, 0, 0, 0, 0, 0 } },
		{ "strategy powersave", 0, 0, 0, { SESSIOND_THERMAL_STRATEGY, SESSIOND_THERMAL_POWERSAVE, 0, 0, 0, 0, 0 } },
		{ "strategy manual 0 0 0", 0, 0, 0, { SESSIOND_THERMAL_STRATEGY, SESSIOND_THERMAL_MANUAL, 0, 0, 0, 0, 0 } },
		{ "strategy manual 100 100 100", 0, 0, 0, { SESSIOND_THERMAL_STRATEGY, SESSIOND_THERMAL_MANUAL, 100, 100, 100, 0, 0 } },
		{ "strategy manual 70 20 5", 0, 0, 0, { SESSIOND_THERMAL_STRATEGY, SESSIOND_THERMAL_MANUAL, 70, 20, 5, 0, 0 } },
		{ "boost all 0", 0, 0, 0, { SESSIOND_THERMAL_BOOST, 0, 0, 0, 0, SESSIOND_THERMAL_FAN_ALL, 0 } },
		{ "boost all 3", 0, 0, 0, { SESSIOND_THERMAL_BOOST, 0, 0, 0, 0, SESSIOND_THERMAL_FAN_ALL, 3 } },
		{ "boost 0 1", 0, 0, 0, { SESSIOND_THERMAL_BOOST, 0, 0, 0, 0, 0, 1 } },
		{ "boost 255 255", 0, 0, 0, { SESSIOND_THERMAL_BOOST, 0, 0, 0, 0, 255, 255 } },

		/* 2. malformed words */
		{ "", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy firmware", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy fast", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy Balanced", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy balanced 1 2 3", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy balanced extra", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy manual", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy manual 1 2", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy manual 1 2 3 4", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy manual 101 0 0", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy manual 0 0 101", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy manual -1 0 0", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy manual +1 0 0", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy manual 01 0 0", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy manual 0100 0 0", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy manual 5a 0 0", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ " strategy balanced", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy balanced ", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy  balanced", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy\tbalanced", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy balanced\r", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy manual 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "boost", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "boost all", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "boost all 1 2", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "boost all 256", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "boost 256 1", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "boost every 1", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "boost -1 1", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "fan all 1", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "status", 0, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },

		/* 3. the right: wheel may, another user may not, malformed words first */
		{ "strategy powersave", 1001, 1, 0, { SESSIOND_THERMAL_STRATEGY, SESSIOND_THERMAL_POWERSAVE, 0, 0, 0, 0, 0 } },
		{ "boost 1 2", 1001, 1, 0, { SESSIOND_THERMAL_BOOST, 0, 0, 0, 0, 1, 2 } },
		{ "strategy powersave", 1002, 0, EPERM, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy manual 10 20 30", 1002, 0, EPERM, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "boost all 1", 1002, 0, EPERM, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "strategy firmware", 1002, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } },
		{ "boost all 999", 1002, 0, EINVAL, { 0, 0, 0, 0, 0, 0, 0 } }
	};
	struct sessiond_thermal_request request;
	char what[160];
	size_t index;
	int error;
	int same;

	/* Each row: the errno, then the whole request (cleared on a refusal). */
	for (index = 0; index < sizeof(table) / sizeof(table[0]); index++) {
		memset(&request, 0x5a, sizeof(request));
		error = sessiond_thermal_decide(table[index].arguments, (uid_t)table[index].uid, table[index].in_wheel, &request);
		(void)snprintf(what, sizeof(what), "decide \"%s\" uid %u: errno %d, expected %d", table[index].arguments, table[index].uid, error, table[index].error);
		check(error == table[index].error, what);

		/* The request as decided, field by field. */
		same = 1;
		if (request.kind != table[index].request.kind ||
		    request.strategy != table[index].request.strategy ||
		    request.speed != table[index].request.speed ||
		    request.noise != table[index].request.noise ||
		    request.gating != table[index].request.gating ||
		    request.fan != table[index].request.fan ||
		    request.level != table[index].request.level)
			same = 0;
		(void)snprintf(what, sizeof(what), "decide \"%s\" uid %u: request %u %u %u %u %u %#x %u", table[index].arguments, table[index].uid, request.kind, request.strategy, request.speed, request.noise, request.gating, request.fan, request.level);
		check(same, what);
	}
}

/* Checks one answer line. */
static void
check_answer(
	int error,
	unsigned flags,
	const char *expected)
{
	char answer[SESSIOND_THERMAL_ANSWER_MAX];
	char what[160];
	int length;
	int differs;

	/* The line and its length. */
	memset(answer, 0, sizeof(answer));
	length = sessiond_thermal_answer(error, flags, answer, sizeof(answer));
	differs = strcmp(answer, expected);
	(void)snprintf(what, sizeof(what), "answer errno %d flags %#x: \"%s\" (%d), expected \"%s\"", error, flags, answer, length, expected);
	check(differs == 0 && length == (int)strlen(expected), what);
}

/* Checks the answer lines, and a buffer too small. */
static void
case_answer(void)
{
	char small[4];
	int length;

	/* 4. success with each bit, and all three */
	check_answer(0, 0, "OK\n");
	check_answer(0, SESSIOND_THERMAL_PARTIAL, "OK partial\n");
	check_answer(0, SESSIOND_THERMAL_PENDING, "OK pending\n");
	check_answer(0, SESSIOND_THERMAL_SUPERSEDED, "OK superseded\n");
	check_answer(0, SESSIOND_THERMAL_PARTIAL | SESSIOND_THERMAL_PENDING | SESSIOND_THERMAL_SUPERSEDED, "OK partial pending superseded\n");
	check_answer(0, 0x80U, "OK\n");

	/* The refusals and failures; the bits do not show on a failure. */
	check_answer(EPERM, 0, "FAIL wheel\n");
	check_answer(EINVAL, 0, "ERROR invalid\n");
	check_answer(EOPNOTSUPP, 0, "ERROR unsupported\n");
	check_answer(ENOENT, 0, "ERROR nofan\n");
	check_answer(ENODEV, 0, "ERROR unavailable\n");
	check_answer(EIO, 0, "ERROR failed\n");
	check_answer(EBUSY, SESSIOND_THERMAL_PENDING, "ERROR failed\n");

	/* A buffer too small for the line is 0, not a cut line. */
	length = sessiond_thermal_answer(0, SESSIOND_THERMAL_PARTIAL, small, sizeof(small));
	check(length == 0, "answer into 4 bytes is 0");
	length = sessiond_thermal_answer(EINVAL, 0, small, sizeof(small));
	check(length == 0, "error answer into 4 bytes is 0");
}
