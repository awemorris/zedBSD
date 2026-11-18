/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The movement of the notifications' popup (ws156-p003): notify-flow.h
 * says what it is.
 */

#include "notify-flow.h"

#include <stdint.h>
#include <string.h>

static void flow_clear(struct kwl_notify_board *board);
static uint64_t flow_stay_limit(const struct kwl_notify_board *board, int waiting);

/*
 * Starts with no board on the screen.
 */
void
kwl_notify_flow_init(
	struct kwl_notify_flow *flow)
{
	/* Nothing shown, nothing leaving. */
	memset(flow, 0, sizeof(*flow));
}

/*
 * Moves the boards on to a time: an entering board stays once in the
 * middle; a staying board counts its stay (not while the pointer is on it)
 * and leaves when the stay is over; a leaving board goes when it is out.
 * With no board entering or staying the next waiting one may be shown.
 */
void
kwl_notify_flow_step(
	struct kwl_notify_flow *flow,
	uint64_t now,
	int waiting,
	int hover,
	struct kwl_notify_flow_events *events)
{
	struct kwl_notify_board *board;
	uint64_t elapsed;
	uint64_t passed;
	uint64_t limit;

	/* Nothing happened yet; the time since the last step. */
	memset(events, 0, sizeof(*events));
	passed = 0U;
	if (flow->last != 0U && now > flow->last)
		passed = now - flow->last;
	flow->last = now;

	/* The leaving board goes once it is beyond the left edge. */
	board = &flow->leaving;
	if (board->stage == KWL_NOTIFY_FLOW_LEAVE) {
		elapsed = now - board->since;
		if (elapsed >= KWL_NOTIFY_FLOW_LEAVE_MS) {
			events->left = board->id;
			flow_clear(board);
		}
	}

	/* The current board: entered, staying, or done with. */
	board = &flow->current;
	if (board->stage == KWL_NOTIFY_FLOW_ENTER) {
		elapsed = now - board->since;
		if (elapsed >= KWL_NOTIFY_FLOW_ENTER_MS) {
			board->stage = KWL_NOTIFY_FLOW_STAY;
			board->since = board->since + KWL_NOTIFY_FLOW_ENTER_MS;
			board->stayed = elapsed - KWL_NOTIFY_FLOW_ENTER_MS;
			passed = 0U;
		}
	} else if (board->stage == KWL_NOTIFY_FLOW_STAY && !hover) {
		/* The pointer away, the stay goes on. */
		board->stayed += passed;
	}

	/* A stay that is over: the board leaves (one still leaving goes at once), and it is done with. */
	if (board->stage == KWL_NOTIFY_FLOW_STAY) {
		limit = flow_stay_limit(board, waiting);
		if (board->stayed >= limit && !hover) {
			if (flow->leaving.stage != KWL_NOTIFY_FLOW_NONE)
				events->left = flow->leaving.id;
			flow->leaving = *board;
			flow->leaving.stage = KWL_NOTIFY_FLOW_LEAVE;
			flow->leaving.since = now;
			events->started_leaving = board->id;
			flow_clear(board);
		}
	}

	/* No board entering or staying: the next may come. */
	if (flow->current.stage == KWL_NOTIFY_FLOW_NONE && waiting)
		events->show_next = 1;
}

/*
 * Puts a notification on the current board: it enters from now.  An
 * urgent one stays until it is touched (kwl_notify_flow_touch).
 */
void
kwl_notify_flow_show(
	struct kwl_notify_flow *flow,
	uint32_t id,
	int urgent,
	uint64_t now)
{
	/* The board, entering. */
	flow->current.id = id;
	flow->current.stage = KWL_NOTIFY_FLOW_ENTER;
	flow->current.since = now;
	flow->current.stayed = 0U;
	flow->current.urgent = urgent;
}

/*
 * Takes a notification off the screen at once (dismissed, withdrawn, its
 * body clicked): its board is empty.
 */
void
kwl_notify_flow_remove(
	struct kwl_notify_flow *flow,
	uint32_t id)
{
	/* Whichever board has it. */
	if (flow->current.stage != KWL_NOTIFY_FLOW_NONE && flow->current.id == id)
		flow_clear(&flow->current);
	if (flow->leaving.stage != KWL_NOTIFY_FLOW_NONE && flow->leaving.id == id)
		flow_clear(&flow->leaving);
}

/*
 * Tells the flow the urgent board was touched (the pointer or a key): it
 * stays its ordinary time from now on.
 */
void
kwl_notify_flow_touch(
	struct kwl_notify_flow *flow)
{
	/* An urgent board no longer waits for a touch. */
	if (flow->current.urgent) {
		flow->current.urgent = 0;
		flow->current.stayed = 0U;
	}
}

/* Reports whether a board is on the screen (the output keeps being redrawn). */
int
kwl_notify_flow_moving(
	const struct kwl_notify_flow *flow)
{
	/* Either board. */
	if (flow->current.stage != KWL_NOTIFY_FLOW_NONE)
		return 1;
	if (flow->leaving.stage != KWL_NOTIFY_FLOW_NONE)
		return 1;

	/* Succeeded: nothing on the screen. */
	return 0;
}

/*
 * Reports where a board is at a time and how opaque: its left edge in a
 * screen of a width, for a board of a width.  Entering, from its left edge
 * at the screen's right edge to the middle, eased out (1 - (1 - t)^3), its
 * opacity rising; staying, in the middle; leaving, from the middle to its
 * right edge at the screen's left edge, eased in (t^3), its opacity falling.
 */
void
kwl_notify_flow_place(
	const struct kwl_notify_board *board,
	uint64_t now,
	int32_t screen_width,
	int32_t board_width,
	int32_t *left,
	float *opacity)
{
	float middle;
	float t;
	float eased;
	float from;
	float to;
	uint64_t elapsed;

	/* The middle, where it stays. */
	middle = (float)(screen_width - board_width) / 2.0f;
	*left = (int32_t)middle;
	*opacity = 1.0f;

	/* How far into its stage. */
	elapsed = 0U;
	if (now > board->since)
		elapsed = now - board->since;

	/* Entering: from the right edge, eased out, fading in. */
	if (board->stage == KWL_NOTIFY_FLOW_ENTER) {
		t = (float)elapsed / (float)KWL_NOTIFY_FLOW_ENTER_MS;
		if (t > 1.0f)
			t = 1.0f;
		eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
		from = (float)screen_width;
		to = middle;
		*left = (int32_t)(from + (to - from) * eased);
		*opacity = eased;
		return;
	}

	/* Leaving: to beyond the left edge, eased in, fading out. */
	if (board->stage == KWL_NOTIFY_FLOW_LEAVE) {
		t = (float)elapsed / (float)KWL_NOTIFY_FLOW_LEAVE_MS;
		if (t > 1.0f)
			t = 1.0f;
		eased = t * t * t;
		from = middle;
		to = -(float)board_width;
		*left = (int32_t)(from + (to - from) * eased);
		*opacity = 1.0f - eased;
	}
}

/* Empties a board. */
static void
flow_clear(
	struct kwl_notify_board *board)
{
	/* No notification, no stage. */
	memset(board, 0, sizeof(*board));
}

/* Reports how long a board stays: until touched (an urgent one, at most 30 s), shorter when others wait. */
static uint64_t
flow_stay_limit(
	const struct kwl_notify_board *board,
	int waiting)
{
	/* An urgent board not yet touched. */
	if (board->urgent)
		return KWL_NOTIFY_FLOW_URGENT_MS;

	/* Others waiting speed it on. */
	if (waiting)
		return KWL_NOTIFY_FLOW_STAY_SHORT_MS;

	/* Succeeded: the ordinary stay. */
	return KWL_NOTIFY_FLOW_STAY_MS;
}
