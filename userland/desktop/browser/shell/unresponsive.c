/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * When to ask about a page that does not respond: the rule of
 * unresponsive.h, which the prompt (prompt.c) steps on the window's
 * thread with what the view tells of its running task.
 */

#include "shell/unresponsive.h"

#include <string.h>

/* Starts with no task watched. */
void
shell_unresponsive_init(
	struct shell_unresponsive *rule)
{
	/* Nothing runs, nothing is asked. */
	memset(rule, 0, sizeof(*rule));
	rule->due_ms = SHELL_UNRESPONSIVE_MS;
}

/*
 * Takes what the view tells now: whether a task runs (busy), its number
 * and how long it has run.  Reports what the question does, and gives in
 * *timeout when to look again, in milliseconds: when the task watched is
 * due, soon while a question shows (its task may end), and otherwise a
 * whole SHELL_UNRESPONSIVE_MS (a task that begins after this look is due
 * no sooner).
 */
enum shell_unresponsive_change
shell_unresponsive_step(
	struct shell_unresponsive *rule,
	int busy,
	uint32_t task,
	uint32_t milliseconds,
	int *timeout)
{
	enum shell_unresponsive_change change;

	/* Between tasks nothing runs for any time. */
	if (!busy) {
		task = 0U;
		milliseconds = 0U;
	}

	/* Another task (or none): the question of the old one goes, and the count starts again. */
	change = SHELL_UNRESPONSIVE_NONE;
	if (task != rule->task) {
		if (rule->shown)
			change = SHELL_UNRESPONSIVE_HIDE;
		rule->task = task;
		rule->due_ms = SHELL_UNRESPONSIVE_MS;
		rule->shown = 0;
	}

	/* The task watched has run long enough: its question. */
	if (task != 0U && !rule->shown && milliseconds >= rule->due_ms) {
		rule->shown = 1;
		change = SHELL_UNRESPONSIVE_SHOW;
	}

	/* When to look again. */
	if (task == 0U) {
		*timeout = (int)SHELL_UNRESPONSIVE_MS;
	} else if (rule->shown) {
		*timeout = (int)SHELL_UNRESPONSIVE_POLL_MS;
	} else {
		*timeout = (int)(rule->due_ms - milliseconds);
	}

	/* Succeeded: the change, if any. */
	return change;
}

/*
 * The user answered the question (wait, or stop: a task that goes on all
 * the same is asked about again), when the task had run so long: the
 * question goes, and comes back when the task ran SHELL_UNRESPONSIVE_MS
 * more.
 */
void
shell_unresponsive_answer(
	struct shell_unresponsive *rule,
	uint32_t milliseconds)
{
	/* No question, and the next one's time. */
	rule->shown = 0;
	rule->due_ms = milliseconds + SHELL_UNRESPONSIVE_MS;
	if (rule->due_ms < milliseconds)
		rule->due_ms = UINT32_MAX;
}
