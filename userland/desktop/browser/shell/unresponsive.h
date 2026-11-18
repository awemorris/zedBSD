/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * When to ask about a page that does not respond (unresponsive.c): the
 * rule alone, over what the view tells of its running task
 * (browser_view_script_busy), with nothing of the window, so that a host
 * test runs it on a clock of its own.
 *
 * A task that runs SHELL_UNRESPONSIVE_MS is asked about once; after the
 * user waits (or stops it and it goes on), the same task is asked about
 * again when it ran SHELL_UNRESPONSIVE_MS more; a task that ends takes its
 * question away; another task starts the count again.
 */

#ifndef KEILAND_BROWSER_SHELL_UNRESPONSIVE_H
#define KEILAND_BROWSER_SHELL_UNRESPONSIVE_H

#include <stdint.h>

/* How long a task runs before the question, in milliseconds. */
#define SHELL_UNRESPONSIVE_MS		10000U

/* How often a question shown looks whether its task ended, in milliseconds. */
#define SHELL_UNRESPONSIVE_POLL_MS	250U

/* What a step changed: nothing, the question to show (again, for another task), or to take away. */
enum shell_unresponsive_change {
	SHELL_UNRESPONSIVE_NONE,
	SHELL_UNRESPONSIVE_SHOW,
	SHELL_UNRESPONSIVE_HIDE
};

/*
 * The rule's state: the task watched (0 for none), how long it runs
 * before it is asked about, and whether its question shows.
 */
struct shell_unresponsive {
	uint32_t task;
	uint32_t due_ms;
	int shown;
};

void shell_unresponsive_init(struct shell_unresponsive *rule);
enum shell_unresponsive_change shell_unresponsive_step(struct shell_unresponsive *rule, int busy, uint32_t task,
    uint32_t milliseconds, int *timeout);
void shell_unresponsive_answer(struct shell_unresponsive *rule, uint32_t milliseconds);

#endif
