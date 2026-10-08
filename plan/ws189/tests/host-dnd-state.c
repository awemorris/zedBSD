/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of a drag's mark (ws189, userland/desktop/wayland/
 * dnd-state.c compiled unchanged): every combination of what is known of
 * a drag against the table of plan/ws189/phase001/phase.md section 3.1.
 */

#include "dnd-state.h"

#include <stdio.h>
#include <string.h>

/*
 * The name each mark is logged under.
 */
struct mark_name {
	unsigned mark;
	const char *name;
};

static unsigned expected(const struct kwl_dnd_facts *facts);
static int check_names(void);

/*
 * The names the log uses, one for each mark.
 */
static const struct mark_name mark_names[] = {
	{ KWL_DND_MARK_NEUTRAL, "neutral" },
	{ KWL_DND_MARK_COPY, "copy" },
	{ KWL_DND_MARK_MOVE, "move" },
	{ KWL_DND_MARK_ASK, "ask" },
	{ KWL_DND_MARK_REFUSED, "refused" }
};

/*
 * The action values tried, the four named ones and one that is none of them.
 */
static const uint32_t actions[] = {
	KWL_DND_ACTION_NONE,
	KWL_DND_ACTION_COPY,
	KWL_DND_ACTION_MOVE,
	KWL_DND_ACTION_ASK,
	3U
};

int
main(
	void)
{
	struct kwl_dnd_facts facts;
	unsigned bits;
	unsigned index;
	unsigned mark;
	unsigned want;
	int failures;
	int checks;
	int name_failures;

	/* No check run yet. */
	failures = 0;
	checks = 0;

	/* Every combination of the five facts with every action. */
	for (bits = 0; bits < 32U; bits++) {
		/* Each action for this combination. */
		for (index = 0; index < sizeof(actions) / sizeof(actions[0]); index++) {
			/* The facts of this combination. */
			memset(&facts, 0, sizeof(facts));
			facts.has_source = (bits >> 0) & 1U;
			facts.has_target = (bits >> 1) & 1U;
			facts.over_origin = (bits >> 2) & 1U;
			facts.accepted = (bits >> 3) & 1U;
			facts.on_bar = (bits >> 4) & 1U;
			facts.action = actions[index];

			/* The mark decided, against the mark the table gives. */
			mark = kwl_dnd_mark(&facts);
			want = expected(&facts);
			checks++;
			if (mark != want) {
				printf("FAIL source=%u target=%u origin=%u accepted=%u bar=%u action=%u: %s, wanted %s\n", facts.has_source, facts.has_target, facts.over_origin, facts.accepted, facts.on_bar, facts.action, kwl_dnd_mark_name(mark), kwl_dnd_mark_name(want));
				failures++;
			}
		}
	}

	/* The names the log uses. */
	checks += (int)(sizeof(mark_names) / sizeof(mark_names[0]));
	name_failures = check_names();
	failures += name_failures;

	/* The verdict. */
	if (failures != 0) {
		printf("host-dnd-state: %d of %d failed\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check passed. */
	printf("host-dnd-state: ok (%d checks)\n", checks);
	return 0;
}

/* Gives the mark the table of the design has for a combination of facts. */
static unsigned
expected(
	const struct kwl_dnd_facts *facts)
{
	/* The bar and a drag inside its client say nothing. */
	if (facts->on_bar)
		return KWL_DND_MARK_NEUTRAL;
	if (!facts->has_source)
		return KWL_DND_MARK_NEUTRAL;

	/* No target refuses. */
	if (!facts->has_target)
		return KWL_DND_MARK_REFUSED;

	/* A type taken shows its action. */
	if (facts->accepted && facts->action == KWL_DND_ACTION_COPY)
		return KWL_DND_MARK_COPY;
	if (facts->accepted && facts->action == KWL_DND_ACTION_MOVE)
		return KWL_DND_MARK_MOVE;
	if (facts->accepted && facts->action == KWL_DND_ACTION_ASK)
		return KWL_DND_MARK_ASK;

	/* Otherwise the drag's own window says nothing, any other refuses. */
	if (facts->over_origin)
		return KWL_DND_MARK_NEUTRAL;

	/* Succeeded: elsewhere the drop would be given up. */
	return KWL_DND_MARK_REFUSED;
}

/* Compares each mark's name with the one the log uses; returns how many differ. */
static int
check_names(void)
{
	const char *name;
	size_t index;
	int differs;
	int failed;

	/* Each mark against its name. */
	failed = 0;
	for (index = 0; index < sizeof(mark_names) / sizeof(mark_names[0]); index++) {
		name = kwl_dnd_mark_name(mark_names[index].mark);
		differs = strcmp(name, mark_names[index].name);
		if (differs != 0) {
			printf("FAIL name of mark %u: %s, wanted %s\n", mark_names[index].mark, name, mark_names[index].name);
			failed++;
		}
	}

	/* Reports how many names differ. */
	return failed;
}
