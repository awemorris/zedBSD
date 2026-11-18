/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The movement of the notifications' popup (notify-flow.c, ws156-p003,
 * plan/ws156/phase001/phase.md sections 3.2 to 3.4 and 4.4): the board of
 * a headline ticker.
 *
 * A notification shown enters from beyond the screen's right edge to the
 * middle while it fades in (280 ms, ease-out), stays there (3 s; 1.5 s when
 * others wait; never while the pointer is on it; an urgent one until it is
 * touched, at most 30 s), and leaves to beyond the left edge while it fades
 * out (280 ms, ease-in).  When it starts to leave it is done with: the next
 * one may enter at once, so the boards follow one another.
 *
 * It knows nothing of the server or the model: the caller tells it the
 * time, whether others wait and whether the pointer is on the board, and
 * is told which notification started to leave, which has left, and when
 * the next may be shown.  So the host tests run it alone.
 */

#ifndef KWL_NOTIFY_FLOW_H
#define KWL_NOTIFY_FLOW_H

#include <stdint.h>

/* The stages of a board. */
#define KWL_NOTIFY_FLOW_NONE	0U
#define KWL_NOTIFY_FLOW_ENTER	1U
#define KWL_NOTIFY_FLOW_STAY	2U
#define KWL_NOTIFY_FLOW_LEAVE	3U

/* How long each stage lasts, in milliseconds. */
#define KWL_NOTIFY_FLOW_ENTER_MS	280U
#define KWL_NOTIFY_FLOW_STAY_MS		3000U
#define KWL_NOTIFY_FLOW_STAY_SHORT_MS	1500U
#define KWL_NOTIFY_FLOW_LEAVE_MS	280U
#define KWL_NOTIFY_FLOW_URGENT_MS	30000U

/*
 * One board: the notification on it (0: none), its stage and when the
 * stage began, the time it has stayed (the pointer's time on it not
 * counted), and whether it is urgent and not yet touched.
 */
struct kwl_notify_board {
	uint32_t id;
	unsigned stage;
	uint64_t since;
	uint64_t stayed;
	int urgent;
};

/*
 * The boards on the screen: the one entering or staying, the one leaving,
 * and the time of the last step (to count the stay).
 */
struct kwl_notify_flow {
	struct kwl_notify_board current;
	struct kwl_notify_board leaving;
	uint64_t last;
};

/*
 * What one step of the flow brought: the notification that started to
 * leave (done with: it goes to the log) and the one that has left the
 * screen (0: none), and whether the next waiting one may be shown.
 */
struct kwl_notify_flow_events {
	uint32_t started_leaving;
	uint32_t left;
	int show_next;
};

void kwl_notify_flow_init(struct kwl_notify_flow *flow);
void kwl_notify_flow_step(struct kwl_notify_flow *flow, uint64_t now, int waiting, int hover, struct kwl_notify_flow_events *events);
void kwl_notify_flow_show(struct kwl_notify_flow *flow, uint32_t id, int urgent, uint64_t now);
void kwl_notify_flow_remove(struct kwl_notify_flow *flow, uint32_t id);
void kwl_notify_flow_touch(struct kwl_notify_flow *flow);
int kwl_notify_flow_moving(const struct kwl_notify_flow *flow);
void kwl_notify_flow_place(const struct kwl_notify_board *board, uint64_t now, int32_t screen_width, int32_t board_width, int32_t *left, float *opacity);

#endif
