/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The mark a drag and drop shows of what its drop would do (dnd-state.c,
 * ws189-p002, plan/ws189/phase001/phase.md section 3.1): a copy, a move, a
 * choice to be asked, no drop, or nothing to say yet.
 *
 * It knows nothing of the server: data.c hands it what it knows of the
 * drag, and shell.c draws what it says.  So the host tests run it alone.
 */

#ifndef KWL_DND_STATE_H
#define KWL_DND_STATE_H

#include <stdint.h>

/* The marks (the same values as kwl.h's KWL_DND_STATE_*). */
#define KWL_DND_MARK_NEUTRAL	0U
#define KWL_DND_MARK_COPY	1U
#define KWL_DND_MARK_MOVE	2U
#define KWL_DND_MARK_ASK	3U
#define KWL_DND_MARK_REFUSED	4U

/* The drag and drop actions (wl_data_device_manager's). */
#define KWL_DND_ACTION_NONE	0U
#define KWL_DND_ACTION_COPY	1U
#define KWL_DND_ACTION_MOVE	2U
#define KWL_DND_ACTION_ASK	4U

/*
 * What is known of a drag at a moment: whether it has a source (a drag
 * inside its own client has none), whether a surface under the pointer
 * heard it and whether that is the surface it started from, whether that
 * target accepted a type and the action chosen (the action a target before
 * version 3 cannot tell is a copy), and whether the pointer rests on the
 * bar's applications (spring-loaded, apps-bar.c).
 */
struct kwl_dnd_facts {
	unsigned has_source;
	unsigned has_target;
	unsigned over_origin;
	unsigned accepted;
	uint32_t action;
	unsigned on_bar;
};

unsigned kwl_dnd_mark(const struct kwl_dnd_facts *facts);
const char *kwl_dnd_mark_name(unsigned mark);

#endif
