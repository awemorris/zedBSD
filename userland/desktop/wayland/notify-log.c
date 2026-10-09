/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The notifications' log on a board (ws156-p004, plan/ws156/phase001/
 * phase.md section 5): Super+N shows the log where the popup shows, one
 * notification at a time, on a board that does not move (it fades in in
 * 150 ms).
 *
 * The board goes round a ring: the newest notification first, then with
 * Right (or the arrow at the board's right) the older ones, after the
 * oldest a board with "Clear all notifications", and after it the newest
 * again; Left goes the other way, so from the newest Left reaches the
 * clearing board.  A press on its button (or Enter on it) clears the log,
 * each client told closed(CLEARED).  A card's close sign takes that
 * notification out of the log (dismissed, not kept).  With an empty log
 * the board says "No notifications".
 *
 * Super+N again, Esc, a press outside the board, and 10 s without input
 * close it.  The log lines the tests read: KWL NOTIFY log open, at, close,
 * clear, dismiss.
 */

#include "kwl.h"
#include "glass.h"
#include "notify.h"
#include "notify-view.h"

#include <keiland/keiland.h>

#include <stdio.h>
#include <string.h>

/* The keys the board takes (Linux input event codes) and the Super modifier's bit. */
#define LOG_KEY_ESC		1U
#define LOG_KEY_ENTER		28U
#define LOG_KEY_N		49U
#define LOG_KEY_LEFT		105U
#define LOG_KEY_RIGHT		106U
#define LOG_MODIFIER_SUPER	0x40U

/* How long the board fades in, and how long it stays without input, in milliseconds. */
#define LOG_FADE_MS		150U
#define LOG_IDLE_MS		10000U

/* The arrows' squares beside the board, and the gap to it, in pixels. */
#define LOG_ARROW		28
#define LOG_ARROW_GAP		8

/* The clearing board's button, in pixels. */
#define LOG_BUTTON_WIDTH	220
#define LOG_BUTTON_HEIGHT	32

/* The parts of the board a press lands on. */
#define LOG_PART_OUTSIDE	0U
#define LOG_PART_CARD		1U
#define LOG_PART_CLOSE		2U
#define LOG_PART_LEFT		3U
#define LOG_PART_RIGHT		4U
#define LOG_PART_CLEAR		5U

/*
 * The log's board for the compositor's life: whether it is open, when it
 * opened and was last used, the place on the ring (0 the clearing board,
 * 1 the newest, up to the oldest), the press being followed, and where the
 * board was drawn.  Only the main loop touches it.
 */
static struct {
	int open;
	uint64_t opened_ms;
	uint64_t used_ms;
	size_t place;
	unsigned pressed;
	int32_t left;
	int32_t top;
	int32_t width;
} log_view;

static void log_open(struct kwl_server *server);
static void log_close(struct kwl_server *server, const char *why);
static void log_move(struct kwl_server *server, int step);
static void log_clear(struct kwl_server *server);
static size_t log_count(void);
static const struct kwl_notification *log_item(size_t place);
static unsigned log_part(const struct kwl_server *server);
static void log_draw_arrow(struct kwl_server *server, VkCommandBuffer command, int32_t x, int32_t y, unsigned icon, float opacity);

/*
 * Takes a key for the log: Super+N opens or closes the board; while it is
 * open, Left and Right go round the ring, Enter on the clearing board
 * clears, and Esc closes.  Other keys are not the board's.  Returns 1 when
 * the key was the log's.
 */
int
kwl_notify_log_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state)
{
	/* Super+N, pressed or released: the press opens or closes. */
	if (key == LOG_KEY_N && (server->modifiers & LOG_MODIFIER_SUPER) != 0U) {
		if (state != 0U && log_view.open)
			log_close(server, "key");
		else if (state != 0U)
			log_open(server);
		return 1;
	}

	/* A closed board takes no other key. */
	if (!log_view.open)
		return 0;

	/* The board's keys. */
	switch (key) {
	case LOG_KEY_ESC:
		/* Closes it. */
		if (state != 0U)
			log_close(server, "esc");
		return 1;
	case LOG_KEY_LEFT:
		/* Towards the newer, and from the newest to the clearing board. */
		if (state != 0U)
			log_move(server, -1);
		return 1;
	case LOG_KEY_RIGHT:
		/* Towards the older, and from the oldest to the clearing board. */
		if (state != 0U)
			log_move(server, 1);
		return 1;
	case LOG_KEY_ENTER:
		/* Clears the log on the clearing board; elsewhere nothing. */
		if (state != 0U && log_view.place == 0U)
			log_clear(server);
		return 1;
	default:
		/* Not the board's. */
		return 0;
	}
}

/* Closes the board after 10 s without input, and keeps its fade drawn. */
void
kwl_notify_log_tick(
	struct kwl_server *server)
{
	uint64_t now;

	/* Only an open board. */
	if (!log_view.open)
		return;

	/* Long without input: it closes. */
	now = kwl_milliseconds();
	if (now - log_view.used_ms >= LOG_IDLE_MS) {
		log_close(server, "idle");
		return;
	}

	/* Its fade needs frames. */
	if (now - log_view.opened_ms < LOG_FADE_MS)
		server->dirty = 1;
}

/*
 * Draws the open board: the notification at its place on the ring, the
 * clearing board, or "No notifications"; and the arrows beside it when the
 * ring has more than one place.
 */
void
kwl_notify_log_draw(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float soft[4] = { 0.34f, 0.38f, 0.46f, 1.0f };
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	const struct kwl_notification *item;
	float opacity;
	float ink[4];
	float fill[4];
	uint64_t elapsed;
	size_t count;
	int32_t middle;
	int32_t left;
	int32_t top;

	/* Only an open board. */
	if (!log_view.open)
		return;

	/* Its place, and its fade. */
	log_view.width = kwl_notify_board_width(server);
	log_view.left = ((int32_t)server->width - log_view.width) / 2;
	log_view.top = kwl_notify_board_top(server);
	elapsed = kwl_milliseconds() - log_view.opened_ms;
	opacity = 1.0f;
	if (elapsed < LOG_FADE_MS)
		opacity = (float)elapsed / (float)LOG_FADE_MS;
	count = log_count();
	middle = log_view.top + KWL_NOTIFY_BOARD_HEIGHT / 2;

	/* An empty log: the board says so. */
	if (count == 0U) {
		kwl_notify_draw_glass(server, command, log_view.left, log_view.top, log_view.width, opacity);
		memcpy(ink, soft, sizeof(ink));
		ink[3] = opacity;
		glass_draw_text_centred(server, command, SIZE_TITLE, log_view.left + log_view.width / 2, middle + 7, kl_tr("No notifications"), log_view.width, ink);
		return;
	}

	/* A notification's card, or the clearing board with its button. */
	item = log_item(log_view.place);
	if (item != NULL) {
		kwl_notify_draw_card(server, command, item, log_view.left, log_view.top, log_view.width, opacity);
	} else {
		kwl_notify_draw_glass(server, command, log_view.left, log_view.top, log_view.width, opacity);
		left = log_view.left + (log_view.width - LOG_BUTTON_WIDTH) / 2;
		top = middle - LOG_BUTTON_HEIGHT / 2;
		kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, opacity, fill);
		glass_draw_solid(server, command, (float)left, (float)top, (float)LOG_BUTTON_WIDTH, (float)LOG_BUTTON_HEIGHT, 8.0f, fill);
		memcpy(ink, white, sizeof(ink));
		ink[3] = opacity;
		glass_draw_text_centred(server, command, SIZE_BAR, left + LOG_BUTTON_WIDTH / 2, top + 21, kl_tr("Clear all notifications"), LOG_BUTTON_WIDTH, ink);
	}

	/* The arrows beside the board (the ring has the clearing board too). */
	log_draw_arrow(server, command, log_view.left - LOG_ARROW_GAP - LOG_ARROW, middle - LOG_ARROW / 2, GLASS_ICON_BACK, opacity);
	log_draw_arrow(server, command, log_view.left + log_view.width + LOG_ARROW_GAP, middle - LOG_ARROW / 2, GLASS_ICON_FORWARD, opacity);
}

/*
 * Takes a pointer button while the board is open: a press on an arrow
 * goes round the ring, on a card's close sign takes that notification out,
 * on the clearing button clears; a press outside closes the board (and is
 * left to what is under it).  Returns 1 when the button was the board's.
 */
int
kwl_notify_log_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	const struct kwl_notification *item;
	unsigned part;
	uint32_t id;
	size_t count;
	int error;

	/* Only the left button of an open board, or the release of a press it took. */
	if (button != KWL_BUTTON_LEFT)
		return 0;
	if (!log_view.open) {
		if (state == 0U && log_view.pressed != LOG_PART_OUTSIDE) {
			log_view.pressed = LOG_PART_OUTSIDE;
			return 1;
		}

		/* Not the board's. */
		return 0;
	}

	/* A press outside closes the board and is not the board's. */
	part = log_part(server);
	log_view.used_ms = kwl_milliseconds();
	if (state != 0U && part == LOG_PART_OUTSIDE) {
		log_close(server, "outside");
		return 0;
	}

	/* A press is kept; its release acts where it was. */
	if (state != 0U) {
		log_view.pressed = part;
		return 1;
	}

	/* A release elsewhere than its press does nothing. */
	if (part != log_view.pressed) {
		log_view.pressed = LOG_PART_OUTSIDE;
		return 1;
	}

	/* The press is answered. */
	log_view.pressed = LOG_PART_OUTSIDE;

	/* What the release on the part does. */
	switch (part) {
	case LOG_PART_LEFT:
		log_move(server, -1);
		break;
	case LOG_PART_RIGHT:
		log_move(server, 1);
		break;
	case LOG_PART_CLEAR:
		log_clear(server);
		break;
	case LOG_PART_CLOSE:
		/* The notification out of the log, the place kept in the ring. */
		item = log_item(log_view.place);
		if (item == NULL)
			break;
		id = item->id;
		error = kwl_notify_dismiss_id(server, id);
		if (error == 0)
			printf("KWL NOTIFY log dismiss id=%u\n", id);
		count = log_count();
		if (log_view.place > count)
			log_view.place = count;
		server->dirty = 1;
		break;
	default:
		/* A card's body does nothing. */
		break;
	}

	/* Succeeded: the button was the board's. */
	return 1;
}

/* Reports whether the log's board is open. */
int
kwl_notify_log_showing(void)
{
	/* Open or not. */
	return log_view.open;
}

/* Opens the board on the newest notification. */
static void
log_open(
	struct kwl_server *server)
{
	size_t count;

	/* Open from now, at the newest (or nothing). */
	log_view.open = 1;
	log_view.opened_ms = kwl_milliseconds();
	log_view.used_ms = log_view.opened_ms;
	log_view.pressed = LOG_PART_OUTSIDE;
	count = log_count();
	log_view.place = 0U;
	if (count > 0U)
		log_view.place = 1U;
	server->dirty = 1;
	printf("KWL NOTIFY log open count=%lu\n", (unsigned long)count);
}

/* Closes the board, saying why. */
static void
log_close(
	struct kwl_server *server,
	const char *why)
{
	/* Closed. */
	log_view.open = 0;
	server->dirty = 1;
	printf("KWL NOTIFY log close reason=%s\n", why);
}

/* Goes one place round the ring: +1 towards the older, -1 towards the newer, through the clearing board. */
static void
log_move(
	struct kwl_server *server,
	int step)
{
	const struct kwl_notification *item;
	size_t places;

	/* The ring's places: the clearing board and the notifications. */
	places = log_count() + 1U;
	if (step > 0)
		log_view.place = (log_view.place + 1U) % places;
	else
		log_view.place = (log_view.place + places - 1U) % places;
	log_view.used_ms = kwl_milliseconds();
	server->dirty = 1;

	/* Where it is now. */
	item = log_item(log_view.place);
	if (item == NULL)
		printf("KWL NOTIFY log at place=%lu clear\n", (unsigned long)log_view.place);
	else
		printf("KWL NOTIFY log at place=%lu id=%u\n", (unsigned long)log_view.place, item->id);
}

/* Clears the log; the board then says there is nothing. */
static void
log_clear(
	struct kwl_server *server)
{
	size_t count;

	/* Every logged notification goes, its client told. */
	count = kwl_notify_clear_log(server);
	log_view.place = 0U;
	log_view.used_ms = kwl_milliseconds();
	server->dirty = 1;
	printf("KWL NOTIFY log clear count=%lu\n", (unsigned long)count);
}

/* Reports how many notifications the log holds. */
static size_t
log_count(void)
{
	const struct kwl_notification *log[KWL_NOTIFY_LOG];
	size_t count;

	/* The log, newest first. */
	count = kwl_notify_log(kwl_notify_model(), log, KWL_NOTIFY_LOG);
	return count;
}

/* Finds the notification at a place on the ring: NULL for the clearing board (0) or past the log. */
static const struct kwl_notification *
log_item(
	size_t place)
{
	const struct kwl_notification *log[KWL_NOTIFY_LOG];
	size_t count;

	/* The clearing board. */
	if (place == 0U)
		return NULL;

	/* The log, newest first. */
	count = kwl_notify_log(kwl_notify_model(), log, KWL_NOTIFY_LOG);
	if (place > count)
		return NULL;

	/* Succeeded: the notification. */
	return log[place - 1U];
}

/* Finds the part of the open board under the pointer. */
static unsigned
log_part(
	const struct kwl_server *server)
{
	int32_t x;
	int32_t y;
	int32_t middle;
	int32_t left;
	int32_t top;
	int close;
	size_t count;

	/* The pointer, on the anchor's output. */
	if (server->pointer_output != 0U)
		return LOG_PART_OUTSIDE;
	x = server->pointer_x;
	y = server->pointer_y;
	middle = log_view.top + KWL_NOTIFY_BOARD_HEIGHT / 2;
	count = log_count();

	/* The arrows beside the board (a log with notifications). */
	if (count > 0U && y >= middle - LOG_ARROW / 2 && y < middle + LOG_ARROW / 2) {
		if (x >= log_view.left - LOG_ARROW_GAP - LOG_ARROW && x < log_view.left - LOG_ARROW_GAP)
			return LOG_PART_LEFT;
		if (x >= log_view.left + log_view.width + LOG_ARROW_GAP && x < log_view.left + log_view.width + LOG_ARROW_GAP + LOG_ARROW)
			return LOG_PART_RIGHT;
	}

	/* Outside the board. */
	if (x < log_view.left || x >= log_view.left + log_view.width || y < log_view.top || y >= log_view.top + KWL_NOTIFY_BOARD_HEIGHT)
		return LOG_PART_OUTSIDE;

	/* The clearing board's button. */
	if (count > 0U && log_view.place == 0U) {
		left = log_view.left + (log_view.width - LOG_BUTTON_WIDTH) / 2;
		top = middle - LOG_BUTTON_HEIGHT / 2;
		if (x >= left && x < left + LOG_BUTTON_WIDTH && y >= top && y < top + LOG_BUTTON_HEIGHT)
			return LOG_PART_CLEAR;
		return LOG_PART_CARD;
	}

	/* A card's close sign, or the card. */
	close = kwl_notify_card_close_at(log_view.left, log_view.top, log_view.width, x, y);
	if (count > 0U && close)
		return LOG_PART_CLOSE;

	/* Succeeded: the card (or the empty board). */
	return LOG_PART_CARD;
}

/* Draws an arrow beside the board: a small round glass with the icon. */
static void
log_draw_arrow(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t x,
	int32_t y,
	unsigned icon,
	float opacity)
{
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float glass[4] = { 1.0f, 1.0f, 1.0f, 0.86f };
	float ink[4];
	float fill[4];

	/* The round glass. */
	memcpy(fill, glass, sizeof(fill));
	fill[3] *= opacity;
	glass_draw_solid(server, command, (float)x, (float)y, (float)LOG_ARROW, (float)LOG_ARROW, (float)LOG_ARROW / 2.0f, fill);

	/* The icon in it. */
	memcpy(ink, dark, sizeof(ink));
	ink[3] = opacity;
	glass_draw_icon(server, command, icon, x + 4, y + 4, (unsigned)(LOG_ARROW - 8), ink);
}
