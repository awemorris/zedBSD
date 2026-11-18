/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The check recorder shared by the contract tests (see contract.h).
 */

#include "contract.h"

#include <stdio.h>

/*
 * How many checks the program has recorded.
 *
 * The program is single-threaded; the count only grows from
 * contract_begin() to contract_end().
 */
static int contract_checks;

/*
 * How many of the recorded checks failed.
 *
 * Zero at contract_end() is the only passing outcome.
 */
static int contract_failures;

/*
 * Announces a contract test and starts its counts at zero.
 */
void
contract_begin(
	const char *title)
{
	/* Starts with nothing recorded. */
	contract_checks = 0;
	contract_failures = 0;

	/* Names the test in the output. */
	printf("== %s ==\n", title);
}

/*
 * Names the contract group the following checks belong to.
 */
void
contract_section(
	const char *name)
{
	/* Prints the group name, so a failure is read against its contract. */
	printf("[%s]\n", name);
}

/*
 * Records one check and prints it when it failed.
 */
void
contract_check(
	int passed,
	const char *message)
{
	/* Counts the check whatever its outcome. */
	contract_checks++;

	/* A failed check is printed and counted against the test. */
	if (passed == 0) {
		printf("    FAIL: %s\n", message);
		contract_failures++;
	}
}

/*
 * Prints the totals and reports the program's exit status.
 */
int
contract_end(void)
{
	/* Prints how many checks ran and how many failed. */
	printf("== %d checks, %d failures ==\n", contract_checks, contract_failures);

	/* Any failed check fails the program. */
	if (contract_failures != 0)
		return 1;

	/* Succeeded: every check held. */
	return 0;
}
