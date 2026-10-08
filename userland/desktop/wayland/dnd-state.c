/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The mark of a drag and drop (ws189-p002, plan/ws189/phase001/phase.md
 * section 3.1): what the drop would do where the pointer is.
 *
 * A drag over the surface it started from that is not taken there, a drag
 * inside its own client, and a drag resting on the bar's applications say
 * nothing (the last waits for a window to come forward).  A target that
 * accepted a type shows the action chosen: a copy, a move or a choice to
 * be asked.  Anywhere else the drop would be given up, and the mark says
 * so.
 */

#include "dnd-state.h"

#include <stddef.h>

/*
 * Decides the mark of a drag from what is known of it.
 */
unsigned
kwl_dnd_mark(
	const struct kwl_dnd_facts *facts)
{
	/* The bar's applications wait to bring a window forward. */
	if (facts->on_bar)
		return KWL_DND_MARK_NEUTRAL;

	/* A drag inside its own client has no data to take or refuse. */
	if (!facts->has_source)
		return KWL_DND_MARK_NEUTRAL;

	/* No surface under the pointer takes a drop. */
	if (!facts->has_target)
		return KWL_DND_MARK_REFUSED;

	/* A target that took no type: the drag's own window says nothing, any other refuses. */
	if (!facts->accepted) {
		if (facts->over_origin)
			return KWL_DND_MARK_NEUTRAL;

		/* Any other window refuses it. */
		return KWL_DND_MARK_REFUSED;
	}

	/* The action chosen for a type taken. */
	switch (facts->action) {
	case KWL_DND_ACTION_COPY:
		return KWL_DND_MARK_COPY;
	case KWL_DND_ACTION_MOVE:
		return KWL_DND_MARK_MOVE;
	case KWL_DND_ACTION_ASK:
		return KWL_DND_MARK_ASK;
	default:
		break;
	}

	/* A type taken with no action the two sides share: the drag's own window says nothing. */
	if (facts->over_origin)
		return KWL_DND_MARK_NEUTRAL;

	/* Succeeded: elsewhere such a drop would be given up. */
	return KWL_DND_MARK_REFUSED;
}

/*
 * Names a mark for the log the tests read.
 */
const char *
kwl_dnd_mark_name(
	unsigned mark)
{
	/* Each mark's word. */
	switch (mark) {
	case KWL_DND_MARK_COPY:
		return "copy";
	case KWL_DND_MARK_MOVE:
		return "move";
	case KWL_DND_MARK_ASK:
		return "ask";
	case KWL_DND_MARK_REFUSED:
		return "refused";
	default:
		break;
	}

	/* Nothing to say. */
	return "neutral";
}
