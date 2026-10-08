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

/* The expected mark of each combination, by the table. */
static unsigned
expected(
	const struct kwl_dnd_facts *facts)
{
	if (facts->on_bar || !facts->has_source)
		return KWL_DND_MARK_NEUTRAL;
	if (!facts->has_target)
		return KWL_DND_MARK_REFUSED;
	if (facts->accepted && facts->action == KWL_DND_ACTION_COPY)
		return KWL_DND_MARK_COPY;
	if (facts->accepted && facts->action == KWL_DND_ACTION_MOVE)
		return KWL_DND_MARK_MOVE;
	if (facts->accepted && facts->action == KWL_DND_ACTION_ASK)
		return KWL_DND_MARK_ASK;
	return facts->over_origin ? KWL_DND_MARK_NEUTRAL : KWL_DND_MARK_REFUSED;
}

int
main(
	void)
{
	static const uint32_t actions[] = { KWL_DND_ACTION_NONE, KWL_DND_ACTION_COPY, KWL_DND_ACTION_MOVE, KWL_DND_ACTION_ASK, 3U };
	struct kwl_dnd_facts facts;
	unsigned bits;
	unsigned index;
	unsigned mark;
	unsigned want;
	int failures;
	int checks;

	failures = 0;
	checks = 0;
	for (bits = 0; bits < 32U; bits++) {
		for (index = 0; index < sizeof(actions) / sizeof(actions[0]); index++) {
			memset(&facts, 0, sizeof(facts));
			facts.has_source = (bits >> 0) & 1U;
			facts.has_target = (bits >> 1) & 1U;
			facts.over_origin = (bits >> 2) & 1U;
			facts.accepted = (bits >> 3) & 1U;
			facts.on_bar = (bits >> 4) & 1U;
			facts.action = actions[index];
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
	checks += 5;
	if (strcmp(kwl_dnd_mark_name(KWL_DND_MARK_NEUTRAL), "neutral") != 0 ||
	    strcmp(kwl_dnd_mark_name(KWL_DND_MARK_COPY), "copy") != 0 ||
	    strcmp(kwl_dnd_mark_name(KWL_DND_MARK_MOVE), "move") != 0 ||
	    strcmp(kwl_dnd_mark_name(KWL_DND_MARK_ASK), "ask") != 0 ||
	    strcmp(kwl_dnd_mark_name(KWL_DND_MARK_REFUSED), "refused") != 0) {
		printf("FAIL names\n");
		failures++;
	}

	if (failures != 0) {
		printf("host-dnd-state: %d of %d failed\n", failures, checks);
		return 1;
	}
	printf("host-dnd-state: ok (%d checks)\n", checks);
	return 0;
}
