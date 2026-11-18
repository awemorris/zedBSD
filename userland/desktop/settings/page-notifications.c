/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Notifications page (ws177-p026): which applications may notify the
 * user, and whether Browser may read the sign-in codes of new mail.
 *
 *   Applications   Mail, Calendar, Phone and Browser, a switch each
 *                  (notify.allow.<application>, on unless turned off): the
 *                  compositor refuses the notifications of one turned off.
 *   Sign-in Codes  Browser may hear new mail's sign-in codes
 *                  (mail.codes.browser, off unless allowed; the same switch
 *                  as Mail's own, ws169-p002).
 *
 * The switches are the user's preferences (look.c's settings), read as
 * the page is drawn and written at a click; the compositor follows them
 * at once.
 */

#include "settings.h"

#include <stdio.h>
#include <string.h>

/* The switches (hit indices): an application's, then the sign-in codes'. */
#define NOTIFICATIONS_APP_FIRST		2
#define NOTIFICATIONS_CODES		20

/* The card's margin, its title and subtitle's room, a row, the switch, the gap after a card, and the text sizes. */
#define NOTIFICATIONS_PAD		20
#define NOTIFICATIONS_TITLED		64
#define NOTIFICATIONS_ROW		56
#define NOTIFICATIONS_GAP		16
#define NOTIFICATIONS_SWITCH		44
#define NOTIFICATIONS_TEXT_ROW		14U
#define NOTIFICATIONS_TEXT_SUB		12U

/* The applications that notify, their setting and their name. */
#define NOTIFICATIONS_APPS		4

/*
 * One application that notifies: the key of its switch (the compositor's
 * notify.allow.<its application ID>) and the name the page shows.
 */
struct notifications_app {
	const char *key;
	const char *name;
};

/* The applications, in the order the page shows them. */
static const struct notifications_app notifications_apps[NOTIFICATIONS_APPS] = {
	{ "notify.allow.mailer", "Mail" },
	{ "notify.allow.calendar", "Calendar" },
	{ "notify.allow.phone", "Phone" },
	{ "notify.allow.browser", "Browser" }
};

static int notifications_setting(struct se_app *app, const char *key, int fallback);
static int notifications_row(struct se_app *app, struct kl_canvas *canvas, int x, int y, int width, const char *name, const char *line, int on, int index);

/*
 * Draws the Notifications page's cards from a top edge; returns the edge
 * below them.
 */
int
se_notifications_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	int height;
	int index;
	int on;
	int y;

	/* The applications: a switch each. */
	height = NOTIFICATIONS_TITLED + NOTIFICATIONS_APPS * NOTIFICATIONS_ROW + 8;
	y = se_card_begin(app, canvas, x, top, width, height, kl_tr("Applications"), kl_tr("Turn off an application to hide its notifications."));
	for (index = 0; index < NOTIFICATIONS_APPS; index++) {
		on = notifications_setting(app, notifications_apps[index].key, 1);
		y = notifications_row(app, canvas, x, y, width, kl_tr(notifications_apps[index].name), kl_tr("Show its notifications"), on,
		    NOTIFICATIONS_APP_FIRST + index);
	}

	/* The sign-in codes: Browser's switch. */
	top += height + NOTIFICATIONS_GAP;
	height = NOTIFICATIONS_TITLED + NOTIFICATIONS_ROW + 8;
	y = se_card_begin(app, canvas, x, top, width, height, kl_tr("Sign-in Codes"), kl_tr("A code that comes by mail can be filled in for you."));
	on = notifications_setting(app, "mail.codes.browser", 0);
	(void)notifications_row(app, canvas, x, y, width, kl_tr("Browser"), kl_tr("May read the sign-in codes in new mail"), on, NOTIFICATIONS_CODES);

	/* The edge below the cards. */
	return top + height + NOTIFICATIONS_GAP;
}

/*
 * Carries out a click on a switch of the page: the setting turned over.
 */
void
se_notifications_press(
	struct se_app *app,
	int index)
{
	const char *key;
	int fallback;
	int on;

	/* The switch's setting. */
	key = NULL;
	fallback = 0;
	if (index >= NOTIFICATIONS_APP_FIRST && index < NOTIFICATIONS_APP_FIRST + NOTIFICATIONS_APPS) {
		key = notifications_apps[index - NOTIFICATIONS_APP_FIRST].key;
		fallback = 1;
	} else if (index == NOTIFICATIONS_CODES) {
		key = "mail.codes.browser";
	}

	/* Not a switch of the page. */
	if (key == NULL)
		return;

	/* Turned over and written; the page is drawn again. */
	on = notifications_setting(app, key, fallback);
	se_look_set_number(app, key, !on, fallback);
	se_log("NOTIFICATIONS set key=%s value=%d", key, !on);
	app->dirty = 1;
}

/* Reads a switch's setting (its default when the preferences are not open). */
static int
notifications_setting(
	struct se_app *app,
	const char *key,
	int fallback)
{
	int value;

	/* The preferences, or the default. */
	if (app->look.settings == NULL)
		return fallback;
	value = kl_settings_get_int(app->look.settings, key, fallback);

	/* Succeeded: on or off. */
	if (value != 0)
		return 1;
	return 0;
}

/* Draws a row: a name, a line under it, and a switch at the right; returns the row's bottom. */
static int
notifications_row(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	int width,
	const char *name,
	const char *line,
	int on,
	int index)
{
	int switch_x;

	/* The words. */
	switch_x = x + width - NOTIFICATIONS_PAD - NOTIFICATIONS_SWITCH;
	(void)kl_text_draw_fit(app->text, canvas, x + NOTIFICATIONS_PAD, y + 22, name, NOTIFICATIONS_TEXT_ROW, 1, switch_x - x - 2 * NOTIFICATIONS_PAD,
	    SE_COLOR_TEXT);
	(void)kl_text_draw_fit(app->text, canvas, x + NOTIFICATIONS_PAD, y + 42, line, NOTIFICATIONS_TEXT_SUB, 0, switch_x - x - 2 * NOTIFICATIONS_PAD,
	    SE_COLOR_TEXT_SECONDARY);

	/* The switch, which the user's preferences can take when they are open. */
	se_toggle_draw(app, canvas, switch_x, y + 16, on, app->look.writable, index);

	/* Succeeded: the row's bottom. */
	return y + NOTIFICATIONS_ROW;
}
