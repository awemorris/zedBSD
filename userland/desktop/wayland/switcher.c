/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The application switcher's state (ws142-p005; switcher.h says what it
 * is).  The applications are taken when it opens and kept until it
 * closes, so that the order does not move under the user while the
 * selection does.
 */

#include "switcher.h"

#include <stdio.h>
#include <string.h>

/*
 * Opens the switcher on the applications in the bar's order, with the
 * current one selected (its index in that order; the leftmost when there
 * is none, -1, or it is not there).  Returns 0, or -1 when there is no
 * application (it stays off).
 */
int
kwl_switcher_open(
	struct kwl_switcher *switcher,
	const struct kwl_apps *apps,
	int current,
	unsigned via,
	unsigned placement)
{
	unsigned index;

	/* Nothing to switch to. */
	memset(switcher, 0, sizeof(*switcher));
	if (apps->count == 0U)
		return -1;

	/* The keys in the order of the bar's icons, from the left. */
	for (index = 0; index < apps->count; index++)
		(void)snprintf(switcher->keys[index], sizeof(switcher->keys[index]), "%s", apps->apps[index].key);
	switcher->count = apps->count;

	/* On, with the current application selected (the leftmost when no application is current). */
	switcher->on = 1;
	switcher->via = via;
	switcher->placement = placement;
	switcher->index = 0;
	if (current >= 0 && (unsigned)current < switcher->count)
		switcher->index = (unsigned)current;

	/* Succeeded: open. */
	return 0;
}

/*
 * Moves the selection by a number of steps, around at the ends.
 */
void
kwl_switcher_step(
	struct kwl_switcher *switcher,
	int delta)
{
	int count;
	int index;

	/* Only while on. */
	if (!switcher->on || switcher->count == 0U)
		return;

	/* A step taken, even one that stays (a single application): Alt let go after it brings the selection. */
	if (delta != 0)
		switcher->steps++;

	/* The new place, in the range. */
	count = (int)switcher->count;
	index = ((int)switcher->index + delta % count + count) % count;
	switcher->index = (unsigned)index;
}

/*
 * Gives the selected application's key, or NULL while off.
 */
const char *
kwl_switcher_selected(
	const struct kwl_switcher *switcher)
{
	/* Off. */
	if (!switcher->on || switcher->count == 0U)
		return NULL;

	/* Succeeded: the selection. */
	return switcher->keys[switcher->index];
}

/*
 * Tells what letting Alt go does to the keyboard's switcher (BUG-209, the
 * 2026-10-06 user instruction): a quick Alt+Tab (let go within
 * KWL_SWITCHER_QUICK_MS of the opening, before any step) leaves it open
 * and sticky, and a sticky switcher stays open at every later release;
 * otherwise the selection is brought.  Returns 1 when the selection is to
 * be brought, 0 when the switcher stays open.
 */
int
kwl_switcher_alt_released(
	struct kwl_switcher *switcher,
	uint64_t now_ms)
{
	uint64_t held_ms;

	/* Off: nothing to bring. */
	if (!switcher->on)
		return 0;

	/* Left open by a quick Alt+Tab: only Enter, a click or a tap brings now. */
	if (switcher->sticky)
		return 0;

	/* A quick Alt+Tab without a step leaves it open from now on. */
	held_ms = 0U;
	if (now_ms > switcher->opened_ms)
		held_ms = now_ms - switcher->opened_ms;
	if (switcher->steps == 0U && held_ms < KWL_SWITCHER_QUICK_MS) {
		switcher->sticky = 1U;
		return 0;
	}

	/* Succeeded: the selection is brought. */
	return 1;
}

/*
 * Closes the switcher.
 */
void
kwl_switcher_close(
	struct kwl_switcher *switcher)
{
	/* Off, with nothing kept. */
	memset(switcher, 0, sizeof(*switcher));
}
