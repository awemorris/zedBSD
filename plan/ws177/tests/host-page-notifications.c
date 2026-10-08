/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws177-p026: the host test of Settings' Notifications page
 * (userland/desktop/settings/page-notifications.c): its two cards and
 * their switches drawn from the user's preferences (stand-ins record the
 * cards and the switches), and a click that turns a setting over.
 *
 *     host-page-notifications
 *
 * Prints "PASS name" or "FAIL name ..." and exits with 1 when one failed.
 */

#include "userland/desktop/settings/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What a frame drew: the cards' titles, the switches (index and state), and the settings written. */
#define DRAWN_MAX	32U

/* One switch drawn. */
struct toggle {
	int index;
	int on;
	int enabled;
};

/* The checks that failed, the frame, the preferences, and the last write. */
static int failures;
static char cards[DRAWN_MAX][64];
static size_t card_count;
static struct toggle toggles[DRAWN_MAX];
static size_t toggle_count;
static int allow_mailer = 1;
static int allow_calendar = 0;
static int codes = 0;
static char written_key[64];
static int written_value = -1;

/* The appearance's colours, all zero: the test does not look at colours. */
static const struct se_palette test_palette;
const struct se_palette *se_palette = &test_palette;

int main(void);
static void check(const char *name, int passed, const char *detail);
static void draw(struct se_app *app);
static const struct toggle *find(int index);

/* The widgets the page draws, recorded. */
int
se_card_begin(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width,
	int height,
	const char *title,
	const char *subtitle)
{
	/* A card's title. */
	(void)app;
	(void)canvas;
	(void)x;
	(void)width;
	(void)height;
	(void)subtitle;
	if (card_count < DRAWN_MAX) {
		(void)snprintf(cards[card_count], sizeof(cards[0]), "%s", title);
		card_count++;
	}
	return top + 64;
}

void
se_toggle_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	int on,
	int enabled,
	int index)
{
	/* A switch. */
	(void)app;
	(void)canvas;
	(void)x;
	(void)y;
	if (toggle_count < DRAWN_MAX) {
		toggles[toggle_count].index = index;
		toggles[toggle_count].on = on;
		toggles[toggle_count].enabled = enabled;
		toggle_count++;
	}
}

int
kl_text_draw_fit(
	struct kl_text *text,
	struct kl_canvas *canvas,
	int x,
	int baseline,
	const char *string,
	unsigned pixels,
	int bold,
	int width,
	kl_color color)
{
	/* Not looked at. */
	(void)text;
	(void)canvas;
	(void)x;
	(void)baseline;
	(void)string;
	(void)pixels;
	(void)bold;
	(void)width;
	(void)color;
	return 0;
}

const char *
kl_tr(
	const char *english)
{
	/* English. */
	return english;
}

void
se_log(
	const char *format,
	...)
{
	/* Not kept. */
	(void)format;
}

int
kl_settings_get_int(
	const struct kl_settings *settings,
	const char *key,
	int fallback)
{
	/* The test's preferences; the others their default. */
	(void)settings;
	if (strcmp(key, "notify.allow.mailer") == 0)
		return allow_mailer;
	if (strcmp(key, "notify.allow.calendar") == 0)
		return allow_calendar;
	if (strcmp(key, "mail.codes.browser") == 0)
		return codes;
	return fallback;
}

void
se_look_set_number(
	struct se_app *app,
	const char *key,
	int value,
	int fallback)
{
	/* Recorded. */
	(void)app;
	(void)fallback;
	(void)snprintf(written_key, sizeof(written_key), "%s", key);
	written_value = value;
}

/* Runs the checks. */
int
main(void)
{
	static struct se_app app;
	const struct toggle *found;

	/* Open preferences that can be written. */
	memset(&app, 0, sizeof(app));
	app.look.settings = (struct kl_settings *)&app;
	app.look.writable = 1;

	/* The cards and the switches: Mail on, Calendar off, Phone and Browser on (default), the codes off. */
	draw(&app);
	check("cards", card_count == 2U && strcmp(cards[0], "Applications") == 0 && strcmp(cards[1], "Sign-in Codes") == 0, "Applications, Sign-in Codes");
	check("switches", toggle_count == 5U && find(2)->on == 1 && find(3)->on == 0 && find(4)->on == 1 && find(5)->on == 1 && find(20)->on == 0,
	    "1 0 1 1 / 0");

	/* A click on Calendar turns it on; on the codes turns them on. */
	se_notifications_press(&app, 3);
	check("press-calendar", strcmp(written_key, "notify.allow.calendar") == 0 && written_value == 1, written_key);
	se_notifications_press(&app, 20);
	check("press-codes", strcmp(written_key, "mail.codes.browser") == 0 && written_value == 1, written_key);
	se_notifications_press(&app, 2);
	check("press-mail-off", strcmp(written_key, "notify.allow.mailer") == 0 && written_value == 0, written_key);

	/* Preferences that cannot be written: the switches are drawn disabled. */
	app.look.writable = 0;
	draw(&app);
	found = find(2);
	check("read-only", found != NULL && !found->enabled, "disabled");

	/* The outcome. */
	printf("host-page-notifications: %s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}

/* Prints a check's outcome. */
static void
check(
	const char *name,
	int passed,
	const char *detail)
{
	/* PASS, or FAIL with why. */
	if (passed) {
		printf("PASS %s\n", name);
	} else {
		printf("FAIL %s: %s\n", name, detail);
		failures++;
	}
}

/* Draws the page once, the frame recorded anew. */
static void
draw(
	struct se_app *app)
{
	/* A new frame. */
	card_count = 0;
	toggle_count = 0;
	(void)se_notifications_draw(app, NULL, 0, 0, 800);
}

/* Finds a switch by its index. */
static const struct toggle *
find(
	int index)
{
	static const struct toggle none = { -1, -1, -1 };
	size_t at;

	/* Each switch drawn. */
	for (at = 0; at < toggle_count; at++) {
		if (toggles[at].index == index)
			return &toggles[at];
	}
	return &none;
}
