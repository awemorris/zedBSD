/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the rule of the question about a page that does not
 * respond (userland/desktop/browser/shell/unresponsive.c), on a clock of
 * the test's own:
 *   (a) between tasks nothing is asked, and the next look is a whole
 *       10 s away;
 *   (b) a task is asked about once, when it has run 10 s (the looks before
 *       are timed to that moment), and not again while the question shows
 *       (the looks then come every 250 ms);
 *   (c) after Wait the same task is asked about again when it ran 10 s
 *       more;
 *   (d) a task that ends takes its question away, and another task starts
 *       the count again (also when it comes while a question shows);
 *   (e) a task first seen past 10 s is asked about at once;
 *   (f) a window that looks only when the rule says (from idle, a 12 s
 *       task beginning 3.3 s in) asks once, 10 s into the task, and takes
 *       the question away when the task ends; a 35 s task with Wait at
 *       each question is asked about at 10, 20 and 30 s.
 *
 *   host-prompt-rule      (prints one line per check and a summary)
 */

#include "shell/unresponsive.h"

#include <stdarg.h>
#include <stdio.h>

/* How many checks ran and failed. */
static unsigned test_checks;
static unsigned test_failures;

static void test_check(int passed, const char *name, const char *format, ...);
static void test_steps(void);
static void test_timeline(void);
static void test_waits(void);

int
main(
	void)
{
	/* The checks. */
	test_steps();
	test_timeline();
	test_waits();

	/* The summary. */
	printf("host-prompt-rule: %u checks, %u failed\n", test_checks, test_failures);
	if (test_failures != 0U)
		return 1;
	return 0;
}

/* Writes one check's line and counts it. */
static void
test_check(
	int passed,
	const char *name,
	const char *format,
	...)
{
	va_list arguments;

	test_checks++;
	if (!passed)
		test_failures++;
	printf("%s %s: ", passed ? "PASS" : "FAIL", name);
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	printf("\n");
}

/* (a) to (e), one step at a time. */
static void
test_steps(void)
{
	struct shell_unresponsive rule;
	enum shell_unresponsive_change change;
	int timeout;

	shell_unresponsive_init(&rule);

	/* (a). */
	change = shell_unresponsive_step(&rule, 0, 0U, 0U, &timeout);
	test_check(change == SHELL_UNRESPONSIVE_NONE && timeout == 10000, "(a) between tasks nothing is asked", "change %d timeout %d", change,
	    timeout);

	/* (b). */
	change = shell_unresponsive_step(&rule, 1, 5U, 2500U, &timeout);
	test_check(change == SHELL_UNRESPONSIVE_NONE && timeout == 7500, "(b) a task 2.5 s in: the next look at its 10 s",
	    "change %d timeout %d", change, timeout);
	change = shell_unresponsive_step(&rule, 1, 5U, 9999U, &timeout);
	test_check(change == SHELL_UNRESPONSIVE_NONE && timeout == 1, "(b) 1 ms before its 10 s: not yet", "change %d timeout %d", change,
	    timeout);
	change = shell_unresponsive_step(&rule, 1, 5U, 10000U, &timeout);
	test_check(change == SHELL_UNRESPONSIVE_SHOW && timeout == 250, "(b) at its 10 s: asked", "change %d timeout %d", change, timeout);
	change = shell_unresponsive_step(&rule, 1, 5U, 10300U, &timeout);
	test_check(change == SHELL_UNRESPONSIVE_NONE && rule.shown && timeout == 250, "(b) not asked twice", "change %d timeout %d", change,
	    timeout);

	/* (c). */
	shell_unresponsive_answer(&rule, 12000U);
	change = shell_unresponsive_step(&rule, 1, 5U, 12100U, &timeout);
	test_check(change == SHELL_UNRESPONSIVE_NONE && !rule.shown && timeout == 9900, "(c) after Wait at 12 s: the next look at 22 s",
	    "change %d timeout %d", change, timeout);
	change = shell_unresponsive_step(&rule, 1, 5U, 22000U, &timeout);
	test_check(change == SHELL_UNRESPONSIVE_SHOW, "(c) asked again at 22 s", "change %d", change);

	/* (d). */
	change = shell_unresponsive_step(&rule, 0, 0U, 0U, &timeout);
	test_check(change == SHELL_UNRESPONSIVE_HIDE && !rule.shown && timeout == 10000, "(d) the task ended: the question goes",
	    "change %d timeout %d", change, timeout);
	change = shell_unresponsive_step(&rule, 1, 6U, 3000U, &timeout);
	test_check(change == SHELL_UNRESPONSIVE_NONE && timeout == 7000, "(d) another task: the count again", "change %d timeout %d", change,
	    timeout);
	change = shell_unresponsive_step(&rule, 1, 6U, 10000U, &timeout);
	change = shell_unresponsive_step(&rule, 1, 7U, 50U, &timeout);
	test_check(change == SHELL_UNRESPONSIVE_HIDE && !rule.shown && timeout == 9950, "(d) another task while asked: the question goes",
	    "change %d timeout %d", change, timeout);

	/* (e). */
	change = shell_unresponsive_step(&rule, 1, 8U, 14000U, &timeout);
	test_check(change == SHELL_UNRESPONSIVE_SHOW, "(e) a task first seen at 14 s: asked at once", "change %d", change);
}

/* (f), first part: the window looks only when the rule says. */
static void
test_timeline(void)
{
	struct shell_unresponsive rule;
	enum shell_unresponsive_change change;
	unsigned shows;
	unsigned hides;
	long shown_at;
	long hidden_at;
	long now;
	int timeout;
	int busy;

	/* A 12 s task from 3.3 s; looks for 30 s. */
	shell_unresponsive_init(&rule);
	shows = 0U;
	hides = 0U;
	shown_at = -1;
	hidden_at = -1;
	for (now = 0; now < 30000; now += timeout) {
		busy = now >= 3300 && now < 15300;
		change = shell_unresponsive_step(&rule, busy, busy ? 9U : 0U, busy ? (uint32_t)(now - 3300) : 0U, &timeout);
		if (change == SHELL_UNRESPONSIVE_SHOW) {
			shows++;
			shown_at = now;
		}
		if (change == SHELL_UNRESPONSIVE_HIDE) {
			hides++;
			hidden_at = now;
		}
	}
	test_check(shows == 1U && shown_at == 13300 && hides == 1U && hidden_at >= 15300 && hidden_at <= 15550,
	    "(f) a 12 s task: asked once at its 10 s, and the question goes at its end", "%u shows (at %ld ms), %u hides (at %ld ms)", shows,
	    shown_at, hides, hidden_at);
}

/* (f), second part: Wait at each question of a 35 s task. */
static void
test_waits(void)
{
	struct shell_unresponsive rule;
	enum shell_unresponsive_change change;
	unsigned shows;
	long at[4];
	long now;
	int timeout;
	int busy;

	/* A 35 s task from 0; Wait 1 s after each question. */
	shell_unresponsive_init(&rule);
	shows = 0U;
	for (now = 0; now < 40000; now += timeout) {
		busy = now < 35000;
		change = shell_unresponsive_step(&rule, busy, busy ? 11U : 0U, busy ? (uint32_t)now : 0U, &timeout);
		if (change == SHELL_UNRESPONSIVE_SHOW) {
			if (shows < 4U)
				at[shows] = now;
			shows++;
		}
		if (rule.shown && busy && now >= at[shows - 1U] + 1000) {
			shell_unresponsive_answer(&rule, (uint32_t)now);
			timeout = 1;
		}
	}
	test_check(shows == 3U && at[0] == 10000 && at[1] == 21000 && at[2] == 32000,
	    "(f) a 35 s task with Wait 1 s after each question: asked 10 s after the start and after each Wait",
	    "%u shows (%ld, %ld, %ld ms)", shows, shows > 0U ? at[0] : -1L, shows > 1U ? at[1] : -1L, shows > 2U ? at[2] : -1L);
}
