/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The notifications' popup (ws156-p003, plan/ws156/phase001/phase.md
 * sections 3 and 4): the board of a headline ticker at the bottom middle
 * of the screen.
 *
 * The notifications waiting in the model (notify.c) are shown one at a
 * time on a board that enters from the right, stays and leaves to the left
 * (notify-flow.c); when one starts to leave it goes to the log and the next
 * enters.  The board is a fifth of the screen wide (320 to 640 pixels) and
 * 76 high, 48 above the bottom edge: the application's icon at the left,
 * the title on one line and the body on two, the close sign at the top
 * right.
 *
 *   x          dismissed: gone, not logged, the client told closed(DISMISSED)
 *   the body   an ACTION notification's: the client told activated, and it
 *              goes as dismissed; another's does nothing
 *   the pointer on the board  keeps it from leaving; an urgent one stays
 *              until the pointer touches it (at most 30 s)
 *
 * While the screen is locked, and while a fullscreen window is on top
 * (unless the notification is urgent), a notification is not shown but
 * goes straight to the log.  The log lines the tests read: KWL NOTIFY
 * show, hide, gone, skip, dismiss, activate.
 */

#include "kwl.h"
#include "glass.h"
#include "notify.h"
#include "notify-flow.h"
#include "notify-view.h"

#include <stdio.h>
#include <string.h>

/* The board's share of the screen's width (a fifth), its bounds, and its height, in pixels. */
#define POPUP_SHARE		5
#define POPUP_WIDTH_LEAST	320
#define POPUP_WIDTH_MOST	640
#define POPUP_HEIGHT		KWL_NOTIFY_BOARD_HEIGHT

/* How far above the bottom edge the board is, its corner radius and its padding. */
#define POPUP_BOTTOM		48
#define POPUP_RADIUS		12.0f
#define POPUP_PADDING		12

/* The application's icon, and the close sign's square at the top right. */
#define POPUP_ICON		36
#define POPUP_CLOSE		24

/* The parts of a board a press lands on. */
#define POPUP_PART_NONE		0U
#define POPUP_PART_BODY		1U
#define POPUP_PART_CLOSE	2U

/* The boards: the one entering or staying, and the one leaving. */
#define POPUP_BOARDS		2U

/* Where a board was drawn last, for the pointer: its notification (0: none) and its rectangle. */
struct popup_place {
	uint32_t id;
	int32_t left;
	int32_t top;
	int32_t width;
	int32_t height;
};

/*
 * Where a notification's body breaks into its two lines: the notification
 * and the byte its second line starts at (0: one line).  Measured once a
 * notification, when it is first drawn.
 */
struct popup_break {
	uint32_t id;
	size_t at;
};

/*
 * The popup's state for the compositor's life: the boards' movement, where
 * they were drawn, the body's line break, whether the pointer is on the
 * current board, and the press being followed (its notification and part).
 * Only the main loop touches it.
 */
static struct {
	int ready;
	struct kwl_notify_flow flow;
	struct popup_place places[POPUP_BOARDS];
	struct popup_break line_break;
	int hover;
	uint32_t pressed_id;
	unsigned pressed_part;
} popup;

static void popup_open(void);
static int popup_route(struct kwl_server *server, const struct kwl_notification *item);
static void popup_show_next(struct kwl_server *server, uint64_t now);
static unsigned popup_part(const struct kwl_server *server, uint32_t *id);
static void popup_draw_board(struct kwl_server *server, VkCommandBuffer command, const struct kwl_notify_board *board, uint64_t now, unsigned slot);
static int popup_board_part(const struct popup_place *place, int32_t x, int32_t y);
static void popup_draw_icon(struct kwl_server *server, VkCommandBuffer command, const struct kwl_notification *item, int32_t x, int32_t y, float opacity);
static size_t popup_line_break(struct kwl_server *server, const struct kwl_notification *item, int32_t width);

/*
 * Moves the popup on (from the glass look's tick): what the boards'
 * movement brings, and the next notification shown, sent to the log
 * unseen, or nothing.  Keeps the output being redrawn while a board is on
 * the screen.
 */
void
kwl_notify_popup_tick(
	struct kwl_server *server)
{
	struct kwl_notify_flow_events events;
	struct kwl_notify_model *model;
	const struct kwl_notification *kept;
	uint32_t id;
	uint64_t now;
	unsigned part;
	size_t waiting;
	int moving;

	/* The model, and the time. */
	popup_open();
	model = kwl_notify_model();
	now = kwl_milliseconds();

	/* A board whose notification went (withdrawn by its client) goes at once. */
	if (popup.flow.current.stage != KWL_NOTIFY_FLOW_NONE) {
		kept = kwl_notify_find(model, popup.flow.current.id);
		if (kept == NULL)
			kwl_notify_flow_remove(&popup.flow, popup.flow.current.id);
	}

	/* The same for the leaving board. */
	if (popup.flow.leaving.stage != KWL_NOTIFY_FLOW_NONE) {
		kept = kwl_notify_find(model, popup.flow.leaving.id);
		if (kept == NULL)
			kwl_notify_flow_remove(&popup.flow, popup.flow.leaving.id);
	}

	/* The pointer on the current board holds it, and touches an urgent one. */
	part = popup_part(server, &id);
	popup.hover = 0;
	if (part != POPUP_PART_NONE && id == popup.flow.current.id && popup.flow.current.stage != KWL_NOTIFY_FLOW_NONE) {
		popup.hover = 1;
		kwl_notify_flow_touch(&popup.flow);
	}

	/* The boards move on. */
	waiting = kwl_notify_waiting(model);
	kwl_notify_flow_step(&popup.flow, now, waiting > 0U, popup.hover, &events);

	/* A board that started to leave is done with: its notification goes to the log. */
	if (events.started_leaving != 0U) {
		kwl_notify_hide_shown(server);
		printf("KWL NOTIFY hide id=%u\n", events.started_leaving);
	}

	/* One that has left the screen. */
	if (events.left != 0U)
		printf("KWL NOTIFY gone id=%u\n", events.left);

	/* The next one, when the board is free. */
	if (events.show_next)
		popup_show_next(server, now);

	/* A board on the screen moves every frame. */
	moving = kwl_notify_flow_moving(&popup.flow);
	if (moving)
		server->dirty = 1;

	/* The log's board closes after a while untouched (notify-log.c). */
	kwl_notify_log_tick(server);
}

/*
 * Draws the boards on the screen over the windows, the system bar and the
 * popups: the leaving one, then the current one.
 */
void
kwl_notify_popup_draw(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	uint64_t now;

	/* Nothing before the first notification. */
	if (!popup.ready)
		return;

	/* The places are this frame's. */
	now = kwl_milliseconds();
	memset(popup.places, 0, sizeof(popup.places));

	/* The board leaving, under the one coming. */
	if (popup.flow.leaving.stage != KWL_NOTIFY_FLOW_NONE)
		popup_draw_board(server, command, &popup.flow.leaving, now, 1U);
	if (popup.flow.current.stage != KWL_NOTIFY_FLOW_NONE)
		popup_draw_board(server, command, &popup.flow.current, now, 0U);

	/* The log's board over them, when open (notify-log.c). */
	kwl_notify_log_draw(server, command);
}

/*
 * Takes a pointer button on a board: a press on it is the popup's, and its
 * release on the same part acts (the close sign dismisses; the body of an
 * ACTION notification is activated).  Returns 1 when the button was the
 * popup's.
 */
int
kwl_notify_popup_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	const struct kwl_notification *item;
	uint32_t id;
	unsigned part;
	int taken;
	int error;

	/* The log's board, when open, first (notify-log.c). */
	taken = kwl_notify_log_button(server, button, state);
	if (taken)
		return 1;

	/* Only the left button, and only with a board on the screen or a press followed. */
	if (button != KWL_BUTTON_LEFT || !popup.ready)
		return 0;

	/* A press: the popup's when on a board. */
	part = popup_part(server, &id);
	if (state != 0U) {
		if (part == POPUP_PART_NONE)
			return 0;
		popup.pressed_id = id;
		popup.pressed_part = part;
		return 1;
	}

	/* A release of a press the popup did not take is not its. */
	if (popup.pressed_part == POPUP_PART_NONE)
		return 0;

	/* The release acts only where the press was. */
	if (part != popup.pressed_part || id != popup.pressed_id) {
		popup.pressed_part = POPUP_PART_NONE;
		return 1;
	}

	/* The press is answered. */
	popup.pressed_part = POPUP_PART_NONE;

	/* The close sign: dismissed, not logged. */
	if (part == POPUP_PART_CLOSE) {
		error = kwl_notify_dismiss_id(server, id);
		if (error == 0)
			printf("KWL NOTIFY dismiss id=%u\n", id);
		kwl_notify_flow_remove(&popup.flow, id);
		server->dirty = 1;
		return 1;
	}

	/* The body: an ACTION notification is activated and goes; another's does nothing. */
	item = kwl_notify_find(kwl_notify_model(), id);
	if (item == NULL || (item->flags & KWL_NOTIFY_ACTION) == 0U)
		return 1;
	error = kwl_notify_activate(server, id);
	if (error == 0)
		printf("KWL NOTIFY activate id=%u\n", id);
	kwl_notify_flow_remove(&popup.flow, id);
	server->dirty = 1;
	return 1;
}

/* Reports whether a board, or the log's, is on the screen (the glass look is then not still, and draws over a fullscreen window). */
int
kwl_notify_popup_showing(void)
{
	int moving;
	int open;

	/* The log's board. */
	open = kwl_notify_log_showing();
	if (open)
		return 1;

	/* Nothing before the first notification. */
	if (!popup.ready)
		return 0;

	/* Succeeded: whether a board moves. */
	moving = kwl_notify_flow_moving(&popup.flow);
	return moving;
}

/* Reports where a notification's board's top is: 48 pixels above the bottom edge. */
int32_t
kwl_notify_board_top(
	const struct kwl_server *server)
{
	/* Its height and the gap above the bottom edge. */
	return (int32_t)server->height - POPUP_BOTTOM - POPUP_HEIGHT;
}

/* Starts the popup's state the first time it is needed. */
static void
popup_open(void)
{
	/* Once. */
	if (popup.ready)
		return;
	memset(&popup, 0, sizeof(popup));
	kwl_notify_flow_init(&popup.flow);
	popup.ready = 1;
}

/* Reports the width of a notification's board (the popup's and the log's): a fifth of the screen's, within 320 and 640 pixels. */
int32_t
kwl_notify_board_width(
	const struct kwl_server *server)
{
	int32_t width;

	/* A fifth, within the bounds. */
	width = (int32_t)server->width / POPUP_SHARE;
	if (width < POPUP_WIDTH_LEAST)
		width = POPUP_WIDTH_LEAST;
	if (width > POPUP_WIDTH_MOST)
		width = POPUP_WIDTH_MOST;

	/* Succeeded: the width. */
	return width;
}

/*
 * Tells whether a notification is not to be shown but to go straight to
 * the log: while the screen is locked (whatever it is), and while a
 * fullscreen window is on top (unless it is urgent).  Returns the reason's
 * code: 0 to show, 1 locked, 2 fullscreen.
 */
static int
popup_route(
	struct kwl_server *server,
	const struct kwl_notification *item)
{
	struct kwl_object *top;

	/* The lock screen shows no notification. */
	if (server->locked)
		return 1;

	/* A fullscreen window on top is not covered, but by an urgent one. */
	top = kwl_top_window(server);
	if (top != NULL && top->fullscreen && (item->flags & KWL_NOTIFY_URGENT) == 0U)
		return 2;

	/* Succeeded: shown. */
	return 0;
}

/*
 * Shows the next waiting notification on the board; those that may not be
 * shown now (locked, under a fullscreen window) go straight to the log,
 * and the one after them is tried.
 */
static void
popup_show_next(
	struct kwl_server *server,
	uint64_t now)
{
	static const char *const reasons[3] = { "", "locked", "fullscreen" };
	struct kwl_notify_model *model;
	const struct kwl_notification *item;
	uint32_t id;
	int route;
	int error;

	/* Until one shows or none waits. */
	model = kwl_notify_model();
	for (;;) {
		error = kwl_notify_show_next(model, &id);
		if (error != 0)
			return;
		item = kwl_notify_find(model, id);
		if (item == NULL)
			return;

		/* Shown: it enters. */
		route = popup_route(server, item);
		if (route == 0) {
			kwl_notify_flow_show(&popup.flow, id, (item->flags & KWL_NOTIFY_URGENT) != 0U, now);
			printf("KWL NOTIFY show id=%u client=%llu urgent=%u title=\"%.40s\"\n",
			       id,
			       (unsigned long long)item->client,
			       (item->flags & KWL_NOTIFY_URGENT) != 0U,
			       item->title);
			server->dirty = 1;
			return;
		}

		/* Not now: into the log unseen. */
		printf("KWL NOTIFY skip id=%u reason=%s\n", id, reasons[route]);
		kwl_notify_hide_shown(server);
	}
}

/*
 * Finds the part of a board under the pointer, and its notification:
 * the close sign, the body, or none (the pointer elsewhere, or on another
 * output).
 */
static unsigned
popup_part(
	const struct kwl_server *server,
	uint32_t *id)
{
	const struct popup_place *place;
	int32_t x;
	int32_t y;
	unsigned slot;
	unsigned part;

	/* Nothing, unless the pointer is on the anchor's output over a board. */
	*id = 0U;
	if (server->pointer_output != 0U)
		return POPUP_PART_NONE;
	x = server->pointer_x;
	y = server->pointer_y;

	/* The current board first: it is over the leaving one. */
	for (slot = 0U; slot < POPUP_BOARDS; slot++) {
		place = &popup.places[slot];
		if (place->id == 0U)
			continue;
		part = (unsigned)popup_board_part(place, x, y);
		if (part == POPUP_PART_NONE)
			continue;
		*id = place->id;
		return part;
	}

	/* Succeeded: no board under the pointer. */
	return POPUP_PART_NONE;
}

/* Draws one board where its movement has it, as opaque as it makes it, and keeps its place for the pointer. */
static void
popup_draw_board(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct kwl_notify_board *board,
	uint64_t now,
	unsigned slot)
{
	const struct kwl_notification *item;
	struct popup_place *place;
	float opacity;
	int32_t width;
	int32_t left;
	int32_t top;

	/* The notification, if it is still kept. */
	item = kwl_notify_find(kwl_notify_model(), board->id);
	if (item == NULL)
		return;

	/* Where the board is and how opaque. */
	width = kwl_notify_board_width(server);
	kwl_notify_flow_place(board, now, (int32_t)server->width, width, &left, &opacity);
	top = kwl_notify_board_top(server);
	place = &popup.places[slot];
	place->id = board->id;
	place->left = left;
	place->top = top;
	place->width = width;
	place->height = POPUP_HEIGHT;

	/* The card. */
	kwl_notify_draw_card(server, command, item, left, top, width, opacity);
}

/*
 * Reports the part of a board under a point: the close sign's square at the
 * top right, the body, or none.
 */
static int
popup_board_part(
	const struct popup_place *place,
	int32_t x,
	int32_t y)
{
	/* Outside the board. */
	if (x < place->left || x >= place->left + place->width || y < place->top || y >= place->top + place->height)
		return POPUP_PART_NONE;

	/* The close sign's square, or the rest. */
	if (x >= place->left + place->width - POPUP_PADDING / 2 - POPUP_CLOSE && y < place->top + POPUP_PADDING / 2 + POPUP_CLOSE)
		return POPUP_PART_CLOSE;

	/* Succeeded: the body. */
	return POPUP_PART_BODY;
}

/*
 * Reports whether a point is on the close sign of a notification's card
 * drawn at (left, top) a width wide (the log's board asks).
 */
int
kwl_notify_card_close_at(
	int32_t left,
	int32_t top,
	int32_t width,
	int32_t x,
	int32_t y)
{
	struct popup_place place;
	int part;

	/* The card's place, then its part. */
	place.id = 1U;
	place.left = left;
	place.top = top;
	place.width = width;
	place.height = POPUP_HEIGHT;
	part = popup_board_part(&place, x, y);
	if (part == POPUP_PART_CLOSE)
		return 1;

	/* Succeeded: not the close sign. */
	return 0;
}

/*
 * Draws a notification's card at (left, top), a width wide, as opaque as
 * asked: the shadow, the white glass, the icon, the application's name and
 * the title, the body on two lines, and the close sign (the popup's boards
 * and the log's).
 */
void
kwl_notify_draw_card(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct kwl_notification *item,
	int32_t left,
	int32_t top,
	int32_t width,
	float opacity)
{
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float soft[4] = { 0.34f, 0.38f, 0.46f, 1.0f };
	const char *app;
	char first[KWL_NOTIFY_BODY_MAX + 1U];
	float ink[4];
	float faint[4];
	int32_t text_left;
	int32_t text_width;
	int32_t app_width;
	size_t at;

	/* The inks, as opaque as the card. */
	memcpy(ink, dark, sizeof(ink));
	memcpy(faint, soft, sizeof(faint));
	ink[3] = opacity;
	faint[3] = opacity;

	/* The shadow and the glass. */
	kwl_notify_draw_glass(server, command, left, top, width, opacity);

	/* The application's icon at the left, in the middle of the height. */
	popup_draw_icon(server, command, item, left + POPUP_PADDING, top + (POPUP_HEIGHT - POPUP_ICON) / 2, opacity);

	/* The text's column, between the icon and the close sign. */
	text_left = left + POPUP_PADDING + POPUP_ICON + POPUP_PADDING;
	text_width = width - (text_left - left) - POPUP_PADDING - POPUP_CLOSE;

	/* The application's name (its window's ID when it gave none), faint, then the title on the same line. */
	app = item->app;
	if (app[0] == '\0')
		app = kwl_notify_app_id(server, item->client);
	app_width = 0;
	if (app[0] != '\0') {
		glass_draw_text(server, command, SIZE_BAR, text_left, top + 22, app, text_width / 3, faint);
		app_width = glass_text_width(server, SIZE_BAR, app);
		if (app_width > text_width / 3)
			app_width = text_width / 3;
		app_width += POPUP_PADDING / 2;
	}

	/* The title after it. */
	glass_draw_text(server, command, SIZE_TITLE, text_left + app_width, top + 22, item->title, text_width - app_width, ink);

	/* The body on two lines, the second cut short with an ellipsis. */
	at = popup_line_break(server, item, text_width);
	if (at == 0U) {
		glass_draw_text(server, command, SIZE_BAR, text_left, top + 44, item->body, text_width, ink);
	} else {
		(void)snprintf(first, sizeof(first), "%.*s", (int)at, item->body);
		glass_draw_text(server, command, SIZE_BAR, text_left, top + 44, first, text_width, ink);
		glass_draw_text(server, command, SIZE_BAR, text_left, top + 64, item->body + at, text_width, ink);
	}

	/* The close sign at the top right. */
	glass_draw_glyph(server, command, SIZE_SIGN, GLASS_CLOSE_GLYPH, left + width - POPUP_PADDING / 2 - POPUP_CLOSE + 6, top + POPUP_PADDING / 2 + 18, faint);
}

/* Draws a board's shadow and white glass at (left, top), a width wide, as opaque as asked. */
void
kwl_notify_draw_glass(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t left,
	int32_t top,
	int32_t width,
	float opacity)
{
	struct glass_shape shape;

	/* The shadow. */
	glass_shape_init(&shape, (float)left, (float)top + 6.0f, (float)width, (float)POPUP_HEIGHT);
	shape.quad[0] -= 40.0f;
	shape.quad[1] -= 40.0f;
	shape.quad[2] += 80.0f;
	shape.quad[3] += 80.0f;
	shape.mode = MODE_SHADOW;
	shape.radius = POPUP_RADIUS;
	shape.soft = 18.0f;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.24f;
	shape.opacity = opacity;
	glass_shape_draw(server, command, &shape);

	/* The glass, as white as the volume's popup. */
	glass_shape_init(&shape, (float)left, (float)top, (float)width, (float)POPUP_HEIGHT);
	shape.mode = MODE_GLASS;
	shape.radius = POPUP_RADIUS;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.86f;
	shape.edge = 0.85f;
	shape.opacity = opacity;
	glass_shape_draw(server, command, &shape);
}

/*
 * Draws a notification's icon in a square at (x, y): the Kei mark for the
 * compositor's own, the application's picture when it has one, else its
 * name's first letter on a tile of the accent colour.
 */
static void
popup_draw_icon(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct kwl_notification *item,
	int32_t x,
	int32_t y,
	float opacity)
{
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	const char *name;
	char letter[2];
	float fill[4];
	float ink[4];
	int picture;

	/* The compositor's own: the Kei mark. */
	if (item->client == 0U) {
		glass_draw_mark(server, command, x, y, POPUP_ICON, GLASS_MARK_BAR, opacity);
		return;
	}

	/* The application's picture, by its window's ID or by the name it gave. */
	name = kwl_notify_app_id(server, item->client);
	picture = -1;
	if (name[0] != '\0')
		picture = kwl_icon_for_app_id(name);
	if (picture < 0 && item->app[0] != '\0')
		picture = kwl_icon_for_app_id(item->app);
	if (picture >= 0) {
		glass_draw_app_tile(server, command, (unsigned)picture, (float)x, (float)y, (float)POPUP_ICON, opacity, 0.0f, GLASS_HOLE_SCENE);
		return;
	}

	/* Else its name's first letter on the accent's tile. */
	if (item->app[0] != '\0')
		name = item->app;
	letter[0] = '?';
	if (name[0] != '\0')
		letter[0] = name[0];
	letter[1] = '\0';
	kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, opacity, fill);
	glass_draw_solid(server, command, (float)x, (float)y, (float)POPUP_ICON, (float)POPUP_ICON, 9.0f, fill);
	memcpy(ink, white, sizeof(ink));
	ink[3] = opacity;
	glass_draw_text_centred(server, command, SIZE_TITLE, x + POPUP_ICON / 2, y + POPUP_ICON / 2 + 7, letter, POPUP_ICON, ink);
}

/*
 * Finds where a notification's body breaks into two lines in a width: the
 * byte after the last space of the longest beginning that fits, or the
 * last whole character that fits when no space does; 0 for a body that
 * fits on one line.  Measured once a notification.
 */
static size_t
popup_line_break(
	struct kwl_server *server,
	const struct kwl_notification *item,
	int32_t width)
{
	char prefix[KWL_NOTIFY_BODY_MAX + 1U];
	size_t length;
	size_t at;
	size_t fits;
	size_t space;
	int32_t measured;

	/* Measured before. */
	if (popup.line_break.id == item->id)
		return popup.line_break.at;

	/* A body on one line breaks nowhere. */
	popup.line_break.id = item->id;
	popup.line_break.at = 0U;
	measured = glass_text_width(server, SIZE_BAR, item->body);
	if (measured <= width)
		return 0U;

	/* The longest beginning that fits, character by character, and its last space. */
	length = strlen(item->body);
	fits = 0U;
	space = 0U;
	for (at = 1U; at <= length; at++) {
		/* Only at a character's end (not inside a UTF-8 sequence). */
		if (at < length && (item->body[at] & 0xc0) == 0x80)
			continue;
		memcpy(prefix, item->body, at);
		prefix[at] = '\0';
		measured = glass_text_width(server, SIZE_BAR, prefix);
		if (measured > width)
			break;
		fits = at;
		if (item->body[at - 1U] == ' ')
			space = at;
	}

	/* After the last space when there is one, else where the characters stop fitting. */
	popup.line_break.at = fits;
	if (space > 0U)
		popup.line_break.at = space;
	return popup.line_break.at;
}

