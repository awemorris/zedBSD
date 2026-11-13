/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The power dialog on the desktop (ws099-p037, BUG-235, the 2026-10-06
 * user requests: Log Out ended the session without asking, and a tablet
 * wants Power Off).  App Home's Power Off and the power button (WS182,
 * backend-host.c) open it: the desktop darkens in POWER_OPEN_MS, and a
 * card in the middle offers Power Off, Restart, Log Out and Cancel
 * (power-layout.c).  Power Off and Restart are asked of the session's
 * backend when it offers them (kl_backend_power_get_state's actions, as
 * the power was last read, backend-host.c; a
 * zedBSD session offers them to root and wheel only, and shows them faint
 * to other users), Log Out
 * ends the session as App Home's Log Out did, Cancel closes it.  Esc, a
 * press outside the card and a swipe of two fingers down on the touch pad
 * cancel; Tab and the arrows move the keys' choice, Enter and Space take
 * it.  While it shows it takes every key, button and motion.
 * Log: "KWL POWER dialog open source=S poweroff=0|1 restart=0|1" and
 * "KWL POWER choice=poweroff|restart|logout|cancel via=V error=E".
 */

#include "kwl.h"
#include "glass.h"
#include "power-layout.h"

#include <keiland/keiland.h>

#include <stdio.h>
#include <string.h>

/* The darkening's time to open and to close (milliseconds), and how dark it gets. */
#define POWER_OPEN_MS		200U
#define POWER_CLOSE_MS		150U
#define POWER_DARK		0.55f

/* The modifiers' bit of Shift, which turns Tab back. */
#define POWER_SHIFT		1U

/* A button's corners, and the card's. */
#define POWER_BUTTON_RADIUS	14.0f
#define POWER_CARD_RADIUS	24.0f

static float power_progress(struct kwl_server *server);
static void power_close(struct kwl_server *server);
static void power_choose(struct kwl_server *server, int choice, const char *via);
static void power_logout(struct kwl_server *server);
static void power_draw_button(struct kwl_server *server, struct kwl_list *command, const int32_t *rect, int choice, int lit, float progress);
static const char *power_words(int choice);
static int power_enabled(struct kwl_server *server, int choice);

/*
 * Opens the power dialog (App Home's Power Off, "home"; the power button,
 * "button"): the choices the session may take now, the keys on
 * Cancel, the darkening from now.
 */
void
kwl_power_dialog_open(
	struct kwl_server *server,
	const char *source)
{
	struct kwl_power_dialog *dialog;
	unsigned poweroff_bit;
	unsigned reboot_bit;
	unsigned poweroff;
	unsigned restart;

	/* Shown already: nothing more. */
	dialog = &server->power_dialog;
	if (dialog->open && !dialog->closing)
		return;

	/*
	 * The choices: Log Out and Cancel always, Power Off and Restart when the
	 * session's backend offers them as the power was last read (the start,
	 * each change; the event loop does not ask the backend here).
	 */
	memset(dialog, 0, sizeof(*dialog));
	dialog->enabled = KWL_POWER_BIT(KWL_POWER_LOGOUT) | KWL_POWER_BIT(KWL_POWER_CANCEL);
	poweroff_bit = KL_BACKEND_POWER_ACTION_BIT(KL_BACKEND_POWER_POWEROFF);
	reboot_bit = KL_BACKEND_POWER_ACTION_BIT(KL_BACKEND_POWER_REBOOT);
	poweroff = 0U;
	restart = 0U;
	if ((server->power.actions & poweroff_bit) != 0U)
		poweroff = 1U;
	if ((server->power.actions & reboot_bit) != 0U)
		restart = 1U;
	if (poweroff)
		dialog->enabled |= KWL_POWER_BIT(KWL_POWER_POWEROFF);
	if (restart)
		dialog->enabled |= KWL_POWER_BIT(KWL_POWER_RESTART);

	/* Open from now, the keys on Cancel, no press yet. */
	dialog->open = 1U;
	dialog->start_ms = kwl_milliseconds();
	dialog->focus = KWL_POWER_CANCEL;
	dialog->pressed = -1;
	(void)snprintf(dialog->source, sizeof(dialog->source), "%s", source);
	server->dirty = 1;
	printf("KWL POWER dialog open source=%s poweroff=%u restart=%u\n", dialog->source, poweroff, restart);
}

/*
 * Tells whether the power dialog shows (opening, open or closing).
 */
int
kwl_power_dialog_showing(
	struct kwl_server *server)
{
	/* Shut. */
	if (!server->power_dialog.open)
		return 0;

	/* Succeeded: it shows. */
	return 1;
}

/*
 * Takes a button while the dialog shows: a press on a button that may be
 * taken holds it and its release there takes it; a press outside the card
 * cancels.  Returns 1 when the button was the dialog's (every one while it
 * shows).
 */
int
kwl_power_dialog_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	struct kwl_power_dialog *dialog;
	struct kwl_power_layout layout;
	int enabled;
	int hit;

	/* Only while it shows, and not while it closes. */
	dialog = &server->power_dialog;
	if (!dialog->open)
		return 0;
	if (dialog->closing)
		return 1;

	/* Only the left button (a finger's tap is one) acts. */
	if (button != KWL_BUTTON_LEFT)
		return 1;

	/* What is under the pointer. */
	kwl_power_layout((int32_t)server->width, (int32_t)server->height, &layout);
	hit = kwl_power_hit(&layout, server->pointer_x, server->pointer_y);

	/* A release on the button pressed takes it; elsewhere nothing. */
	if (state == 0U) {
		if (dialog->pressed >= 0 && hit == dialog->pressed)
			power_choose(server, hit, "press");
		dialog->pressed = -1;
		server->dirty = 1;
		return 1;
	}

	/* A press outside the card cancels. */
	if (hit == KWL_POWER_OUTSIDE) {
		power_choose(server, KWL_POWER_CANCEL, "outside");
		return 1;
	}

	/* A press on a button that may be taken holds it. */
	enabled = power_enabled(server, hit);
	if (enabled) {
		dialog->pressed = hit;
		server->dirty = 1;
	}

	/* Succeeded: the button was the dialog's. */
	return 1;
}

/*
 * Takes a key while the dialog shows: Esc cancels, Tab and the arrows move
 * the keys' choice, Enter and Space take it.  Returns 1 when the key was
 * the dialog's (every one while it shows).
 */
int
kwl_power_dialog_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state)
{
	struct kwl_power_dialog *dialog;
	int step;

	/* Only while it shows; a release, or a key while it closes, does nothing. */
	dialog = &server->power_dialog;
	if (!dialog->open)
		return 0;
	if (state == 0U || dialog->closing)
		return 1;

	/* Esc cancels. */
	if (key == KEY_ESC) {
		power_choose(server, KWL_POWER_CANCEL, "escape");
		return 1;
	}

	/* Enter and Space take the keys' choice. */
	if (key == KEY_ENTER || key == KEY_SPACE) {
		power_choose(server, dialog->focus, "key");
		return 1;
	}

	/* Down, Right and Tab move on; Up, Left and Shift+Tab back. */
	step = 0;
	if (key == KEY_DOWN || key == KEY_RIGHT) {
		step = 1;
	} else if (key == KEY_UP || key == KEY_LEFT) {
		step = -1;
	} else if (key == KEY_TAB) {
		step = 1;
		if ((server->modifiers & POWER_SHIFT) != 0U)
			step = -1;
	}

	/* The new choice, drawn lit. */
	if (step != 0) {
		dialog->focus = kwl_power_focus_step(dialog->focus, step, dialog->enabled);
		server->dirty = 1;
	}

	/* Succeeded: the key was the dialog's. */
	return 1;
}

/*
 * Takes the pointer's motion while the dialog shows (the button under it is
 * drawn lit).  Returns 1 when the motion was the dialog's.
 */
int
kwl_power_dialog_motion(
	struct kwl_server *server)
{
	/* Only while it shows. */
	if (!server->power_dialog.open)
		return 0;

	/* Succeeded: drawn again with the button under the pointer lit. */
	server->dirty = 1;
	return 1;
}

/*
 * Takes a swipe of two fingers on the touch pad while the dialog shows: one
 * down cancels.  Returns 1 when the swipe was the dialog's.
 */
int
kwl_power_dialog_swipe(
	struct kwl_server *server,
	int down)
{
	/* Only while it shows, and not while it closes. */
	if (!server->power_dialog.open)
		return 0;
	if (server->power_dialog.closing)
		return 1;

	/* Down cancels. */
	if (down)
		power_choose(server, KWL_POWER_CANCEL, "swipe");

	/* Succeeded: the swipe was the dialog's. */
	return 1;
}

/*
 * Follows the dialog's darkening, and ends it once it has closed; the
 * login or lock screen closes it at once.
 */
void
kwl_power_dialog_tick(
	struct kwl_server *server)
{
	struct kwl_power_dialog *dialog;
	float progress;

	/* Shut: nothing. */
	dialog = &server->power_dialog;
	if (!dialog->open)
		return;

	/* The login and lock screens have no dialog over them. */
	if (server->greeter || server->locked) {
		memset(dialog, 0, sizeof(*dialog));
		server->dirty = 1;
		return;
	}

	/* Darkening or lightening: each frame asks for the next until it ends. */
	progress = power_progress(server);
	if ((!dialog->closing && progress < 1.0f) || (dialog->closing && progress > 0.0f))
		server->dirty = 1;

	/* Closed. */
	if (dialog->closing && progress <= 0.0f) {
		memset(dialog, 0, sizeof(*dialog));
		server->dirty = 1;
	}
}

/*
 * Draws the dialog over everything but the cursor: the darkened desktop and
 * the card with its four buttons, fading in as it opens and out as it
 * closes.
 */
void
kwl_power_dialog_draw(
	struct kwl_server *server,
	struct kwl_list *command)
{
	struct kwl_power_dialog *dialog;
	struct kwl_power_layout layout;
	struct glass_shape shape;
	float dark[4];
	float progress;
	int hover;
	int choice;
	int lit;

	/* Only while it shows. */
	dialog = &server->power_dialog;
	if (!dialog->open)
		return;

	/* The desktop darkened, as far as the dialog has opened. */
	progress = power_progress(server);
	server->layer_on = 0;
	dark[0] = 0.0f;
	dark[1] = 0.0f;
	dark[2] = 0.0f;
	dark[3] = POWER_DARK * progress;
	glass_draw_solid(server, command, 0.0f, 0.0f, (float)server->width, (float)server->height, 0.0f, dark);

	/* The card's frosted glass in the middle. */
	kwl_power_layout((int32_t)server->width, (int32_t)server->height, &layout);
	glass_shape_init(&shape, (float)layout.card[0], (float)layout.card[1], (float)layout.card[2], (float)layout.card[3]);
	shape.mode = MODE_GLASS;
	shape.radius = POWER_CARD_RADIUS;
	shape.soft = 1.0f;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.62f;
	shape.edge = 0.70f;
	shape.opacity = progress;
	glass_shape_draw(server, command, &shape);

	/* The buttons, the one under the pointer (or pressed, or the keys' choice) lit. */
	hover = kwl_power_hit(&layout, server->pointer_x, server->pointer_y);
	for (choice = 0; choice < KWL_POWER_CHOICES; choice++) {
		lit = 0;
		if (choice == hover || choice == dialog->pressed || choice == dialog->focus)
			lit = 1;
		power_draw_button(server, command, layout.buttons[choice], choice, lit, progress);
	}
}

/* Tells how far the dialog has opened (0 to 1), or how much of it is left while it closes. */
static float
power_progress(
	struct kwl_server *server)
{
	struct kwl_power_dialog *dialog;
	uint64_t elapsed;
	float t;

	/* The time since it began to open or to close. */
	dialog = &server->power_dialog;
	elapsed = kwl_milliseconds() - dialog->start_ms;

	/* Closing: from 1 down to 0. */
	if (dialog->closing) {
		t = 1.0f - (float)elapsed / (float)POWER_CLOSE_MS;
		if (t < 0.0f)
			t = 0.0f;
		return t;
	}

	/* Opening: from 0 up to 1. */
	t = (float)elapsed / (float)POWER_OPEN_MS;
	if (t > 1.0f)
		t = 1.0f;
	return t;
}

/* Starts closing the dialog: it lightens and goes (kwl_power_dialog_tick). */
static void
power_close(
	struct kwl_server *server)
{
	/* Closing from now; no press holds a button. */
	server->power_dialog.closing = 1U;
	server->power_dialog.start_ms = kwl_milliseconds();
	server->power_dialog.pressed = -1;
	server->dirty = 1;
}

/*
 * Takes a choice: Power Off and Restart are asked of the session's backend
 * (when it offers them), Log Out ends the session, Cancel only closes.  A
 * choice that may not be taken does nothing.
 */
static void
power_choose(
	struct kwl_server *server,
	int choice,
	const char *via)
{
	int enabled;
	int error;

	/* A choice the session may not take stays where it is. */
	enabled = power_enabled(server, choice);
	if (!enabled)
		return;

	/* The dialog closes whatever was chosen. */
	power_close(server);

	/* Each choice's work. */
	error = 0;
	switch (choice) {
	case KWL_POWER_POWEROFF:
		error = kl_backend_power_action(server->backend, KL_BACKEND_POWER_POWEROFF);
		break;
	case KWL_POWER_RESTART:
		error = kl_backend_power_action(server->backend, KL_BACKEND_POWER_REBOOT);
		break;
	case KWL_POWER_LOGOUT:
		power_logout(server);
		break;
	default:
		break;
	}

	/* The log says what was chosen and how it went. */
	printf("KWL POWER choice=%s via=%s error=%d\n", kwl_power_choice_name(choice), via, error);
}

/*
 * Ends the session (App Home's Log Out of before, ws035-p095): the
 * compositor ends, and sessiond shows the login screen again (handoff.c);
 * without a session manager the compositor only stops.
 */
static void
power_logout(
	struct kwl_server *server)
{
	int handed;

	/* Through sessiond, or the compositor stops. */
	printf("KWL SESSION logout\n");
	handed = kwl_handoff_logout(server);
	if (!handed)
		kwl_request_stop();
}

/*
 * Draws one of the card's buttons: Power Off red with white words, the
 * others white glass with dark words; lit a little brighter, a choice that
 * may not be taken faint.
 */
static void
power_draw_button(
	struct kwl_server *server,
	struct kwl_list *command,
	const int32_t *rect,
	int choice,
	int lit,
	float progress)
{
	float fill[4];
	float ink[4];
	float alpha;
	const char *text;
	int32_t width;
	int enabled;

	/* A choice that may not be taken is faint. */
	alpha = progress;
	enabled = power_enabled(server, choice);
	if (!enabled)
		alpha = progress * 0.35f;

	/* Power Off red with white words; the others white with dark words. */
	if (choice == KWL_POWER_POWEROFF) {
		fill[0] = 0.86f;
		fill[1] = 0.25f;
		fill[2] = 0.24f;
		fill[3] = 0.92f * alpha;
		ink[0] = 1.0f;
		ink[1] = 1.0f;
		ink[2] = 1.0f;
		ink[3] = alpha;
	} else {
		fill[0] = 1.0f;
		fill[1] = 1.0f;
		fill[2] = 1.0f;
		fill[3] = 0.55f * alpha;
		ink[0] = 0.10f;
		ink[1] = 0.14f;
		ink[2] = 0.22f;
		ink[3] = alpha;
	}

	/* Lit: a little more opaque (Power Off) or whiter. */
	if (lit && alpha >= progress) {
		fill[3] = fill[3] + (1.0f - fill[3]) * 0.45f;
		if (choice == KWL_POWER_POWEROFF) {
			fill[0] = 0.92f;
			fill[1] = 0.30f;
		}
	}

	/* The button. */
	glass_draw_solid(server, command, (float)rect[0], (float)rect[1], (float)rect[2], (float)rect[3], POWER_BUTTON_RADIUS, fill);

	/* Its words, in the session's language, in its middle. */
	text = power_words(choice);
	width = glass_text_width(server, SIZE_SEARCH, text);
	glass_draw_text(server, command, SIZE_SEARCH, rect[0] + (rect[2] - width) / 2, rect[1] + rect[3] / 2 + 7, text, rect[2] - 24, ink);
}

/* Gives a choice's words in the session's language. */
static const char *
power_words(
	int choice)
{
	/* Each one (the catalog's texts, WS158). */
	switch (choice) {
	case KWL_POWER_POWEROFF:
		return kl_tr("Power Off");
	case KWL_POWER_RESTART:
		return kl_tr("Restart");
	case KWL_POWER_LOGOUT:
		return kl_tr("Log Out");
	default:
		return kl_tr("Cancel");
	}
}

/* Tells whether a choice may be taken now (a choice out of range may not). */
static int
power_enabled(
	struct kwl_server *server,
	int choice)
{
	unsigned bit;

	/* No such choice. */
	if (choice < 0 || choice >= KWL_POWER_CHOICES)
		return 0;

	/* One the session does not offer. */
	bit = KWL_POWER_BIT(choice);
	if ((server->power_dialog.enabled & bit) == 0U)
		return 0;

	/* Succeeded: it may be taken. */
	return 1;
}
