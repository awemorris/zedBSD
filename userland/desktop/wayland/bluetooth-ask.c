/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pairing's window (ws143-p006, plan/ws143/phase006/phase.md section 5
 * and review I1, I2): a pairing's question, which bluetoothd asks the
 * desktop that answers for the seat (bluetooth-shell.c hands it here), is
 * asked of the person at the screen in a card over the darkened desktop,
 * as the power dialog's (power-dialog.c):
 *
 *   CONFIRM  the device's name, "Pair if the device shows the same number",
 *            the number, Pair and Cancel
 *   CONSENT  the device's name, "Pair with this device?", Pair and Cancel
 *   PASSKEY  the device's name, "Type this number on the device, then
 *            press Enter", the number, Cancel (which gives up this desktop's
 *            own pairing; another's runs to its end)
 *
 * A pairing another user started says who ("Asked by NAME"): the person at
 * the screen decides.  A question is answered by its id; one not answered
 * in ASK_MS is answered no, and the window goes when the question ends
 * (ASK-END), when the service goes, and with the lock (a question while
 * the screen is locked or the login screen shows is answered no at once).
 * Esc cancels; Tab and the arrows move the keys' choice; Enter and Space
 * take it.  While it shows it takes every key, button and motion.
 * Log: "KWL BT ask kind=K id=N number=N own=0|1",
 * "KWL BT ask card x= y= width= height=", "KWL BT ask button=pair|cancel
 * x= y= width= height=", "KWL BT answer yes|no via=V error=E" and
 * "KWL BT ask closed via=V".
 */

#include "kwl.h"
#include "glass.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* How long a question waits for its answer (before bluetoothd's own 25 seconds), and the longest a number shows. */
#define ASK_MS			20000U
#define ASK_PASSKEY_MS		60000U

/* The darkening's time to open and to close (milliseconds), and how dark it gets. */
#define ASK_OPEN_MS		200U
#define ASK_CLOSE_MS		150U
#define ASK_DARK		0.55f

/* The card's size and corners, the buttons' size, gap and corners. */
#define ASK_CARD_WIDTH		440
#define ASK_CARD_HEIGHT		250
#define ASK_CARD_RADIUS		24.0f
#define ASK_BUTTON_WIDTH	150
#define ASK_BUTTON_HEIGHT	40
#define ASK_BUTTON_GAP		16
#define ASK_BUTTON_RADIUS	14.0f

/* The choices: Pair and Cancel, and a place on neither. */
#define ASK_PAIR		0
#define ASK_CANCEL		1
#define ASK_NONE		(-1)
#define ASK_OUTSIDE		(-2)

/*
 * The window: whether it shows and closes, since when (the darkening's
 * clock), when it was asked (the question's clock), the question, whether
 * it has Pair (a number to type has Cancel only), the keys' choice and the
 * button pressed, and whether its place was logged.
 */
static struct {
	unsigned open;
	unsigned closing;
	uint64_t start_ms;
	uint64_t asked_ms;
	struct kl_backend_bluetooth_question question;
	unsigned pair;
	int focus;
	int pressed;
	unsigned logged;
} ask_view;

static void ask_answer(struct kwl_server *server, int choice, const char *via);
static void ask_close(struct kwl_server *server, const char *via);
static float ask_progress(void);
static void ask_card(struct kwl_server *server, int32_t *card);
static void ask_button_rect(const int32_t *card, int choice, int32_t *rect);
static int ask_hit(struct kwl_server *server, int32_t x, int32_t y);
static const char *ask_choice_name(int choice, const char *pair, const char *cancel);
static void ask_draw_button(struct kwl_server *server, VkCommandBuffer command, const int32_t *rect, int choice, int lit, float progress);
static void ask_draw_middle(struct kwl_server *server, VkCommandBuffer command, enum glass_size size, int32_t middle, int32_t baseline, const char *text, const float *ink);
static const char *ask_kind_name(unsigned kind);

/*
 * Takes a pairing's question from the service: an end closes the window it
 * names; a question while the screen is locked or the login screen shows
 * is answered no; any other opens the window (in place of an older one).
 */
void
kwl_bluetooth_ask_take(
	struct kwl_server *server,
	const struct kl_backend_bluetooth_question *question)
{
	int error;

	/* An end: the window of that question goes. */
	if (question->kind == KL_BACKEND_BT_ASK_END) {
		if (ask_view.open && !ask_view.closing && ask_view.question.id == question->id)
			ask_close(server, "end");
		return;
	}

	/* Nobody at the desktop: no, at once (a number only shows; this desktop's own pairing is given up). */
	if (server->greeter || server->locked) {
		error = 0;
		if (question->kind == KL_BACKEND_BT_ASK_PASSKEY && question->own)
			error = kwl_bluetooth_cancel();
		else if (question->kind != KL_BACKEND_BT_ASK_PASSKEY)
			error = kwl_bluetooth_answer(question->id, 0U);
		printf("KWL BT answer no via=locked error=%d\n", error);
		return;
	}

	/* The window, from now, the keys on Pair for this desktop's own pairing and on Cancel for another's. */
	memset(&ask_view, 0, sizeof(ask_view));
	ask_view.open = 1U;
	ask_view.start_ms = kwl_milliseconds();
	ask_view.asked_ms = ask_view.start_ms;
	ask_view.question = *question;
	if (question->kind != KL_BACKEND_BT_ASK_PASSKEY)
		ask_view.pair = 1U;
	ask_view.focus = ASK_CANCEL;
	if (question->own && question->kind != KL_BACKEND_BT_ASK_PASSKEY)
		ask_view.focus = ASK_PAIR;
	ask_view.pressed = ASK_NONE;
	server->dirty = 1;
	printf("KWL BT ask kind=%s id=%u number=%06u own=%u\n", ask_kind_name(question->kind), question->id, question->number, question->own);
}

/*
 * Follows the window: its darkening and its end, the question's time run
 * out (answered no), the service gone, the lock.
 */
void
kwl_bluetooth_ask_tick(
	struct kwl_server *server,
	unsigned reachable)
{
	uint64_t now;
	uint64_t limit;
	float progress;

	/* Shut: nothing. */
	if (!ask_view.open)
		return;

	/* Closing: drawn until it has lightened, then gone. */
	progress = ask_progress();
	if (ask_view.closing) {
		server->dirty = 1;
		if (progress <= 0.0f)
			memset(&ask_view, 0, sizeof(ask_view));
		return;
	}

	/* Opening, and the countdown: drawn every frame. */
	server->dirty = 1;

	/* The lock or the login screen: no. */
	if (server->greeter || server->locked) {
		ask_answer(server, ASK_CANCEL, "locked");
		return;
	}

	/* The service went: the question went with it. */
	if (!reachable) {
		ask_close(server, "service");
		return;
	}

	/* The time ran out: no (a number only goes). */
	now = kwl_milliseconds();
	limit = ASK_MS;
	if (ask_view.question.kind == KL_BACKEND_BT_ASK_PASSKEY)
		limit = ASK_PASSKEY_MS;
	if (now - ask_view.asked_ms >= limit) {
		if (ask_view.question.kind == KL_BACKEND_BT_ASK_PASSKEY)
			ask_close(server, "time");
		else
			ask_answer(server, ASK_CANCEL, "time");
	}
}

/* Tells whether the window shows (opening, open or closing). */
int
kwl_bluetooth_ask_showing(
	void)
{
	/* Open. */
	if (ask_view.open)
		return 1;
	return 0;
}

/*
 * Takes a button while the window shows: a press on a button holds it and
 * its release there takes it; a press outside the card does nothing (the
 * question is not answered by a stray click).  Returns 1 when the button
 * was the window's (every one while it shows).
 */
int
kwl_bluetooth_ask_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	int hit;

	/* Only while it shows, and not while it closes. */
	if (!ask_view.open)
		return 0;
	if (ask_view.closing)
		return 1;

	/* Only the left button (a finger's tap is one) acts. */
	if (button != KWL_BUTTON_LEFT)
		return 1;
	hit = ask_hit(server, server->pointer_x, server->pointer_y);

	/* A release on the button pressed takes it. */
	if (state == 0U) {
		if (ask_view.pressed >= 0 && hit == ask_view.pressed)
			ask_answer(server, hit, "press");
		ask_view.pressed = ASK_NONE;
		server->dirty = 1;
		return 1;
	}

	/* A press on a button holds it. */
	if (hit >= 0) {
		ask_view.pressed = hit;
		server->dirty = 1;
	}

	/* Succeeded: the button was the window's. */
	return 1;
}

/*
 * Takes a key while the window shows: Esc cancels, Tab and the arrows move
 * the keys' choice, Enter and Space take it.  Returns 1 when the key was
 * the window's (every one while it shows).
 */
int
kwl_bluetooth_ask_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state)
{
	/* Only while it shows; a release, or a key while it closes, does nothing. */
	if (!ask_view.open)
		return 0;
	if (state == 0U || ask_view.closing)
		return 1;

	/* Esc cancels. */
	if (key == KEY_ESC) {
		ask_answer(server, ASK_CANCEL, "escape");
		return 1;
	}

	/* Enter and Space take the keys' choice. */
	if (key == KEY_ENTER || key == KEY_SPACE) {
		ask_answer(server, ask_view.focus, "key");
		return 1;
	}

	/* Tab and the arrows move between the two (a number has Cancel only). */
	if (ask_view.pair && (key == KEY_TAB || key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN)) {
		ask_view.focus = ASK_CANCEL - ask_view.focus;
		server->dirty = 1;
	}

	/* Succeeded: the key was the window's. */
	return 1;
}

/* Takes the pointer's motion while the window shows (the button under it is drawn lit).  Returns 1 when it was the window's. */
int
kwl_bluetooth_ask_motion(
	struct kwl_server *server)
{
	/* Only while it shows. */
	if (!ask_view.open)
		return 0;
	server->dirty = 1;
	return 1;
}

/*
 * Draws the window over everything but the cursor: the darkened desktop and
 * the card with the device, the words, the number and the buttons.
 */
void
kwl_bluetooth_ask_draw(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float dark_ink[4] = { 0.10f, 0.14f, 0.22f, 1.0f };
	static const float soft_ink[4] = { 0.30f, 0.36f, 0.46f, 1.0f };
	const struct kl_backend_bluetooth_question *question;
	struct glass_shape shape;
	char number[16];
	char line[160];
	const char *words;
	int32_t card[4];
	int32_t rect[4];
	int32_t middle;
	uint64_t elapsed;
	unsigned left;
	float dark[4];
	float ink[4];
	float soft[4];
	float progress;
	int hover;
	int choice;
	int lit;

	/* Only while it shows. */
	if (!ask_view.open)
		return;
	question = &ask_view.question;

	/* The desktop darkened, as far as the window has opened. */
	progress = ask_progress();
	server->layer_on = 0;
	dark[0] = 0.0f;
	dark[1] = 0.0f;
	dark[2] = 0.0f;
	dark[3] = ASK_DARK * progress;
	glass_draw_solid(server, command, 0.0f, 0.0f, (float)server->width, (float)server->height, 0.0f, dark);

	/* The card's frosted glass in the middle. */
	ask_card(server, card);
	glass_shape_init(&shape, (float)card[0], (float)card[1], (float)card[2], (float)card[3]);
	shape.mode = MODE_GLASS;
	shape.radius = ASK_CARD_RADIUS;
	shape.soft = 1.0f;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.70f;
	shape.edge = 0.70f;
	shape.opacity = progress;
	glass_shape_draw(server, command, &shape);

	/* The inks, fading with the card. */
	memcpy(ink, dark_ink, sizeof(ink));
	memcpy(soft, soft_ink, sizeof(soft));
	ink[3] = progress;
	soft[3] = progress;
	middle = card[0] + card[2] / 2;

	/* The device's name. */
	ask_draw_middle(server, command, SIZE_SIGN, middle, card[1] + 44, question->name, ink);

	/* What to do. */
	words = kl_tr("Pair with this device?");
	if (question->kind == KL_BACKEND_BT_ASK_CONFIRM)
		words = kl_tr("Pair if the device shows the same number.");
	if (question->kind == KL_BACKEND_BT_ASK_PASSKEY)
		words = kl_tr("Type this number on the device, then press Enter.");
	ask_draw_middle(server, command, SIZE_BAR, middle, card[1] + 72, words, soft);

	/* The number, large. */
	if (question->kind != KL_BACKEND_BT_ASK_CONSENT) {
		(void)snprintf(number, sizeof(number), "%06u", question->number);
		ask_draw_middle(server, command, SIZE_ICON, middle, card[1] + 128, number, ink);
	}

	/* Who asked (another user's pairing), and the time left to answer. */
	line[0] = '\0';
	if (!question->own)
		(void)kl_tr_format(line, sizeof(line), kl_tr("Asked by {1}"), question->user, (const char *)NULL);
	if (question->own && question->kind != KL_BACKEND_BT_ASK_PASSKEY) {
		elapsed = kwl_milliseconds() - ask_view.asked_ms;
		left = 0U;
		if (elapsed < ASK_MS)
			left = (unsigned)((ASK_MS - elapsed + 999U) / 1000U);
		(void)snprintf(number, sizeof(number), "%u", left);
		(void)kl_tr_format(line, sizeof(line), kl_tr("Closes in {1} s"), number, (const char *)NULL);
	}

	/* That line under the number. */
	ask_draw_middle(server, command, SIZE_BAR, middle, card[1] + 160, line, soft);

	/* The buttons, the one under the pointer (or pressed, or the keys' choice) lit. */
	hover = ask_hit(server, server->pointer_x, server->pointer_y);
	for (choice = ASK_PAIR; choice <= ASK_CANCEL; choice++) {
		if (choice == ASK_PAIR && !ask_view.pair)
			continue;
		lit = 0;
		if (choice == hover || choice == ask_view.pressed || choice == ask_view.focus)
			lit = 1;
		ask_button_rect(card, choice, rect);
		ask_draw_button(server, command, rect, choice, lit, progress);
	}

	/* The places, once, for the tests that click them. */
	if (!ask_view.logged) {
		ask_view.logged = 1U;
		printf("KWL BT ask card x=%d y=%d width=%d height=%d\n", card[0], card[1], card[2], card[3]);
		for (choice = ASK_PAIR; choice <= ASK_CANCEL; choice++) {
			if (choice == ASK_PAIR && !ask_view.pair)
				continue;
			ask_button_rect(card, choice, rect);
			printf("KWL BT ask button=%s x=%d y=%d width=%d height=%d\n", ask_choice_name(choice, "pair", "cancel"), rect[0], rect[1], rect[2], rect[3]);
		}
	}
}

/*
 * Answers the question by a choice (Pair: yes; Cancel: no, or for a number
 * shown this desktop's own pairing given up) and closes the window.
 */
static void
ask_answer(
	struct kwl_server *server,
	int choice,
	const char *via)
{
	const struct kl_backend_bluetooth_question *question;
	int error;

	/* A number has no Pair. */
	question = &ask_view.question;
	if (choice == ASK_PAIR && !ask_view.pair)
		return;

	/* The answer. */
	error = 0;
	if (question->kind != KL_BACKEND_BT_ASK_PASSKEY)
		error = kwl_bluetooth_answer(question->id, choice == ASK_PAIR);
	else if (question->own)
		error = kwl_bluetooth_cancel();
	printf("KWL BT answer %s via=%s error=%d\n", ask_choice_name(choice, "yes", "no"), via, error);

	/* The window goes. */
	ask_close(server, via);
}

/* Starts closing the window: it lightens and goes (kwl_bluetooth_ask_tick). */
static void
ask_close(
	struct kwl_server *server,
	const char *via)
{
	/* Closing from now; no press holds a button. */
	ask_view.closing = 1U;
	ask_view.start_ms = kwl_milliseconds();
	ask_view.pressed = ASK_NONE;
	server->dirty = 1;
	printf("KWL BT ask closed via=%s\n", via);
}

/* Tells how far the window has opened (0 to 1), or how much of it is left while it closes. */
static float
ask_progress(
	void)
{
	uint64_t elapsed;
	float t;

	/* The time since it began to open or to close. */
	elapsed = kwl_milliseconds() - ask_view.start_ms;

	/* Closing: from 1 down to 0. */
	if (ask_view.closing) {
		t = 1.0f - (float)elapsed / (float)ASK_CLOSE_MS;
		if (t < 0.0f)
			t = 0.0f;
		return t;
	}

	/* Opening: from 0 up to 1. */
	t = (float)elapsed / (float)ASK_OPEN_MS;
	if (t > 1.0f)
		t = 1.0f;
	return t;
}

/* Gives the card's rectangle (x, y, width, height), in the middle of the screen. */
static void
ask_card(
	struct kwl_server *server,
	int32_t *card)
{
	/* In the middle. */
	card[0] = ((int32_t)server->width - ASK_CARD_WIDTH) / 2;
	card[1] = ((int32_t)server->height - ASK_CARD_HEIGHT) / 2;
	card[2] = ASK_CARD_WIDTH;
	card[3] = ASK_CARD_HEIGHT;
}

/* Gives a button's rectangle: Pair at the left and Cancel at the right, or Cancel alone in the middle. */
static void
ask_button_rect(
	const int32_t *card,
	int choice,
	int32_t *rect)
{
	int32_t middle;

	/* Under the words. */
	middle = card[0] + card[2] / 2;
	rect[1] = card[1] + card[3] - 24 - ASK_BUTTON_HEIGHT;
	rect[2] = ASK_BUTTON_WIDTH;
	rect[3] = ASK_BUTTON_HEIGHT;

	/* Cancel alone. */
	if (!ask_view.pair) {
		rect[0] = middle - ASK_BUTTON_WIDTH / 2;
		return;
	}

	/* The two side by side. */
	rect[0] = middle - ASK_BUTTON_GAP / 2 - ASK_BUTTON_WIDTH;
	if (choice == ASK_CANCEL)
		rect[0] = middle + ASK_BUTTON_GAP / 2;
}

/* Tells which button a point is on (ASK_PAIR, ASK_CANCEL), or ASK_NONE on the card, or ASK_OUTSIDE. */
static int
ask_hit(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	int32_t card[4];
	int32_t rect[4];
	int choice;

	/* Each button. */
	ask_card(server, card);
	for (choice = ASK_PAIR; choice <= ASK_CANCEL; choice++) {
		if (choice == ASK_PAIR && !ask_view.pair)
			continue;
		ask_button_rect(card, choice, rect);
		if (x >= rect[0] && x < rect[0] + rect[2] && y >= rect[1] && y < rect[1] + rect[3])
			return choice;
	}

	/* The card, or outside it. */
	if (x >= card[0] && x < card[0] + card[2] && y >= card[1] && y < card[1] + card[3])
		return ASK_NONE;
	return ASK_OUTSIDE;
}

/* Gives the word for a choice, Pair's or Cancel's, for the log. */
static const char *
ask_choice_name(
	int choice,
	const char *pair,
	const char *cancel)
{
	/* The one chosen. */
	if (choice == ASK_PAIR)
		return pair;
	return cancel;
}

/* Draws a button: Pair in the accent with its ink, Cancel white glass with dark words; lit a little brighter. */
static void
ask_draw_button(
	struct kwl_server *server,
	VkCommandBuffer command,
	const int32_t *rect,
	int choice,
	int lit,
	float progress)
{
	float fill[4];
	float ink[4];
	const char *text;
	int32_t width;
	unsigned kept;

	/* Pair: the accent the user chose, as it is; Cancel: white with dark words. */
	kept = server->keep_colours;
	if (choice == ASK_PAIR) {
		kwl_accent_colour(server, 0, KWL_ACCENT_FILL, 0.92f * progress, fill);
		kwl_accent_colour(server, 0, KWL_ACCENT_INK, progress, ink);
		kept = kwl_accent_as_is(server);
		text = kl_tr("Pair");
	} else {
		fill[0] = 1.0f;
		fill[1] = 1.0f;
		fill[2] = 1.0f;
		fill[3] = 0.55f * progress;
		ink[0] = 0.10f;
		ink[1] = 0.14f;
		ink[2] = 0.22f;
		ink[3] = progress;
		text = kl_tr("Cancel");
	}

	/* Lit: more opaque. */
	if (lit)
		fill[3] = fill[3] + (progress - fill[3]) * 0.45f;

	/* The button and its words in its middle. */
	glass_draw_solid(server, command, (float)rect[0], (float)rect[1], (float)rect[2], (float)rect[3], ASK_BUTTON_RADIUS, fill);
	width = glass_text_width(server, SIZE_SEARCH, text);
	glass_draw_text(server, command, SIZE_SEARCH, rect[0] + (rect[2] - width) / 2, rect[1] + rect[3] / 2 + 7, text, rect[2] - 24, ink);
	kwl_accent_done(server, kept);
}

/* Draws a line of text centred on middle, cut to the card's width. */
static void
ask_draw_middle(
	struct kwl_server *server,
	VkCommandBuffer command,
	enum glass_size size,
	int32_t middle,
	int32_t baseline,
	const char *text,
	const float *ink)
{
	int32_t width;
	int32_t limit;

	/* Its width, no wider than the card's inside. */
	limit = ASK_CARD_WIDTH - 48;
	width = glass_text_width(server, size, text);
	if (width > limit)
		width = limit;
	glass_draw_text(server, command, size, middle - width / 2, baseline, text, limit, ink);
}

/* Names a question's kind for the log. */
static const char *
ask_kind_name(
	unsigned kind)
{
	/* Each kind. */
	if (kind == KL_BACKEND_BT_ASK_CONFIRM)
		return "confirm";
	if (kind == KL_BACKEND_BT_ASK_CONSENT)
		return "consent";
	if (kind == KL_BACKEND_BT_ASK_PASSKEY)
		return "passkey";
	return "end";
}
