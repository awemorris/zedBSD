/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Welcome of Settings (ws164-p002, plan/ws164/phase001/phase.md
 * section 3): the steps a person takes at the first login of an account --
 * Welcome, Network, Look, Keys, Done -- in Settings' window without its
 * list of pages.  The Network and the Look steps are Settings' own pages
 * (Wi-Fi when the machine has the radio, else Ethernet; Wallpaper and
 * Appearance), whose controls work as they do there; Welcome, Keys and
 * Done are drawn here.  A bar at the foot of the page's pane holds a dot a
 * step, Back, and Next (on the last step "Start using Kei"); Skip stands at
 * the top right of every step.
 *
 * Start using Kei and Skip set the desktop's setting welcome.done to 1 (the
 * compositor starts the Welcome at a login while it is 0) and close the
 * window; Start using Kei also opens Files, which shows Today.  Closing the
 * window sets nothing, so the Welcome comes again at the next login.  The
 * About page shows it again ("Show Welcome again").
 */

#include "settings.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The steps (Languages between Look and Keys, ws164 H3). */
#define WELCOME_STEP_WELCOME	0
#define WELCOME_STEP_NETWORK	1
#define WELCOME_STEP_LOOK	2
#define WELCOME_STEP_LANGUAGES	3
#define WELCOME_STEP_KEYS	4
#define WELCOME_STEP_DONE	5
#define WELCOME_STEPS		6

/* The bar at the foot of the pane: its height, the dots' size and gap, the buttons' inset from its edges. */
#define WELCOME_BAR		64
#define WELCOME_DOT		8
#define WELCOME_DOT_GAP		10
#define WELCOME_INSET		24

/* The Welcome step's mark and the room around it, the Keys card's row height, and the space between two cards. */
#define WELCOME_MARK		96U
#define WELCOME_ROW		40
#define WELCOME_CARD_GAP	18
#define WELCOME_TEXT		14U
#define WELCOME_TEXT_BIG	28U

/* The steps' words for the log and the tests. */
static const char *const welcome_words[WELCOME_STEPS] = { "welcome", "network", "look", "languages", "keys", "done" };

/* The Keys step: Kei's main keys and what they do. */
static const char *const welcome_keys[][2] = {
	{ "Super", "Open App Home (tap the key)" },
	{ "Super+Tab", "Wiseview: every window at a glance" },
	{ "Alt+Tab", "Switch between windows" },
	{ "Super+L", "Lock the screen" },
	{ "Alt+Shift+Left / Right", "Move between desktops" },
	{ "Alt+Space", "Switch the input language" }
};

static int welcome_page(const struct se_app *app, int step);
static int welcome_intro(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static int welcome_keys_card(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static int welcome_done(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static int welcome_header(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width, const char *title, const char *summary);
static void welcome_finish(struct se_app *app, int skipped);
static void welcome_name(char *name, size_t size);

/*
 * Starts the Welcome at its first step: the list of pages goes, the
 * window shows the step (logged).
 */
void
se_welcome_start(
	struct se_app *app)
{
	/* The first step, without the list. */
	app->welcome = 1;
	app->welcome_step = WELCOME_STEP_WELCOME;
	app->show_sidebar = 0;
	app->page = (unsigned)welcome_page(app, WELCOME_STEP_WELCOME);
	app->page_scroll = 0;
	app->logged_count = -1;
	app->dirty = 1;
	se_log("WELCOME step=0 name=%s", welcome_words[WELCOME_STEP_WELCOME]);
}

/*
 * Draws the step shown in the page's pane, from a top edge in a column (x,
 * width): its header and content.  Returns the edge below them.
 */
int
se_welcome_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	const struct se_page *page;
	const char *title;
	const char *summary;
	int skip_width;
	int y;

	/* Skip, at the top right. */
	skip_width = se_button_width(app, kl_tr("Skip"));
	(void)se_button_draw(app, canvas, x + width - skip_width, top, kl_tr("Skip"), 0, 1, SE_WELCOME_SKIP);

	/* The step's own content. */
	switch (app->welcome_step) {
	case WELCOME_STEP_WELCOME:
		return welcome_intro(app, canvas, x, top, width - skip_width - 16);
	case WELCOME_STEP_NETWORK:
		/* Settings' Wi-Fi page, or the Ethernet page without the radio. */
		page = &se_pages[app->page];
		title = "Connect to a network";
		summary = "Join Wi-Fi now, or later in Settings.";
		if (app->page == SE_PAGE_ETHERNET) {
			title = "Your network";
			summary = "The wired connection and its address.";
		}

		/* The header, then the page. */
		y = welcome_header(app, canvas, x, top, width - skip_width - 16, title, summary);
		return page->draw(app, canvas, x, y + WELCOME_CARD_GAP, width);
	case WELCOME_STEP_LOOK:
		/* The wallpaper, then the windows' look. */
		y = welcome_header(app, canvas, x, top, width - skip_width - 16, "Choose your look", "A picture for the desktop, and how clear the windows are.");
		y = se_wallpaper_draw(app, canvas, x, y + WELCOME_CARD_GAP, width);
		return se_appearance_draw(app, canvas, x, y + WELCOME_CARD_GAP, width);
	case WELCOME_STEP_LANGUAGES:
		/* Settings' Languages page: the input method and the display language. */
		y = welcome_header(app, canvas, x, top, width - skip_width - 16, "Languages and input", "How you type, and the language of the desktop's words.");
		return se_languages_draw(app, canvas, x, y + WELCOME_CARD_GAP, width);
	case WELCOME_STEP_KEYS:
		y = welcome_header(app, canvas, x, top, width - skip_width - 16, "Keys to know", "Kei is quick to use from the keyboard.");
		return welcome_keys_card(app, canvas, x, y + WELCOME_CARD_GAP, width);
	default:
		break;
	}

	/* Done. */
	return welcome_done(app, canvas, x, top, width - skip_width - 16);
}

/*
 * Draws the bar at the foot of the page's pane: a dot a step (the one
 * shown in the accent), Back (not on the first step) and Next, or on the
 * last step "Start using Kei".
 */
void
se_welcome_bar(
	struct se_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *panel)
{
	struct kl_rect line;
	const char *label;
	kl_color color;
	int button_y;
	int dots_x;
	int next_width;
	int step;
	int y;

	/* A thin line over the bar. */
	y = panel->y + panel->height - WELCOME_BAR;
	line.x = panel->x + WELCOME_INSET;
	line.y = y;
	line.width = panel->width - 2 * WELCOME_INSET;
	line.height = 1;
	kl_canvas_fill(canvas, &line, SE_COLOR_PANEL_EDGE);

	/* The dots, in the middle. */
	dots_x = panel->x + (panel->width - (WELCOME_STEPS * WELCOME_DOT + (WELCOME_STEPS - 1) * WELCOME_DOT_GAP)) / 2;
	for (step = 0; step < WELCOME_STEPS; step++) {
		color = SE_COLOR_TEXT_SECONDARY;
		if (step == app->welcome_step)
			color = SE_COLOR_ACCENT;
		kl_canvas_circle(canvas, (float)(dots_x + step * (WELCOME_DOT + WELCOME_DOT_GAP) + WELCOME_DOT / 2), (float)(y + WELCOME_BAR / 2),
				 (float)WELCOME_DOT / 2.0f, color);
	}

	/* Back, at the left, but on the first step. */
	button_y = y + (WELCOME_BAR - 32) / 2;
	if (app->welcome_step > WELCOME_STEP_WELCOME)
		(void)se_button_draw(app, canvas, panel->x + WELCOME_INSET, button_y, kl_tr("Back"), 0, 1, SE_WELCOME_BACK);

	/* Next, or Start using Kei, at the right. */
	label = kl_tr("Next");
	if (app->welcome_step == WELCOME_STEP_DONE)
		label = kl_tr("Start using Kei");
	next_width = se_button_width(app, label);
	(void)se_button_draw(app, canvas, panel->x + panel->width - WELCOME_INSET - next_width, button_y, label, 1, 1, SE_WELCOME_NEXT);
}

/* Reports the height of the bar at the foot of the pane, which the step's content stays above. */
int
se_welcome_bar_height(void)
{
	/* The bar's. */
	return WELCOME_BAR;
}

/*
 * Carries out a press of the Welcome's controls (Back, Next, Skip); any
 * other control is the step's page's.  Returns 1 when it was the Welcome's.
 */
int
se_welcome_press(
	struct se_app *app,
	int index)
{
	int step;

	/* Only the Welcome's own. */
	if (index != SE_WELCOME_BACK && index != SE_WELCOME_NEXT && index != SE_WELCOME_SKIP)
		return 0;

	/* Skip: the Welcome done, the window closed. */
	if (index == SE_WELCOME_SKIP) {
		welcome_finish(app, 1);
		return 1;
	}

	/* Next on the last step: done, Files opened. */
	if (index == SE_WELCOME_NEXT && app->welcome_step == WELCOME_STEP_DONE) {
		welcome_finish(app, 0);
		return 1;
	}

	/* The step before or after, its page's controls the step's (logged). */
	step = app->welcome_step + 1;
	if (index == SE_WELCOME_BACK)
		step = app->welcome_step - 1;
	if (step < 0)
		step = 0;
	app->welcome_step = step;
	app->page = (unsigned)welcome_page(app, step);
	app->page_scroll = 0;
	app->logged_count = -1;
	app->dirty = 1;
	se_log("WELCOME step=%d name=%s", step, welcome_words[step]);
	return 1;
}

/* Gives the page whose controls a step's are: Wi-Fi or Ethernet, Appearance (with Wallpaper), Languages, else Home (none). */
static int
welcome_page(
	const struct se_app *app,
	int step)
{
	/* The network: Wi-Fi when the machine has the radio. */
	if (step == WELCOME_STEP_NETWORK) {
		if (app->network.state.wifi != KL_WIFI_ABSENT)
			return SE_PAGE_WIFI;
		return SE_PAGE_ETHERNET;
	}

	/* The look: Appearance's and Wallpaper's controls are look.c's alike. */
	if (step == WELCOME_STEP_LOOK)
		return SE_PAGE_APPEARANCE;

	/* Languages: the Languages page's controls. */
	if (step == WELCOME_STEP_LANGUAGES)
		return SE_PAGE_LANGUAGES;

	/* No page's controls. */
	return SE_PAGE_HOME;
}

/* Draws the Welcome step: the mark, "Welcome to Kei, NAME" and a line; returns the edge below. */
static int
welcome_intro(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct kl_text_line title;
	struct kl_text_line line;
	char name[64];
	char words[128];
	int baseline;

	/* The mark. */
	se_mark_draw(canvas, x, top, WELCOME_MARK, 1.0f);

	/* The greeting under it, with the account's name. */
	welcome_name(name, sizeof(name));
	(void)kl_tr_format(words, sizeof(words), kl_tr("Welcome to Kei, {1}"), name, (const char *)NULL);
	kl_text_metrics(app->text, WELCOME_TEXT_BIG, &title);
	baseline = top + (int)WELCOME_MARK + 28 + title.ascent;
	(void)kl_text_draw_fit(app->text, canvas, x, baseline, words, WELCOME_TEXT_BIG, 1, width, SE_COLOR_TITLE);

	/* A line on what follows. */
	kl_text_metrics(app->text, WELCOME_TEXT, &line);
	baseline += title.descent + 14 + line.ascent;
	(void)kl_text_draw_fit(app->text, canvas, x, baseline, kl_tr("Let's set up a few things. You can change each of them later in Settings."),
			       WELCOME_TEXT, 0, width, SE_COLOR_TEXT_SECONDARY);

	/* The edge below. */
	return baseline + line.descent;
}

/* Draws the Keys step's card: a row a key; returns the edge below it. */
static int
welcome_keys_card(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	size_t count;
	size_t at;
	int y;

	/* The card, then a row each key. */
	count = sizeof(welcome_keys) / sizeof(welcome_keys[0]);
	y = se_card_begin(app, canvas, x, top, width, se_card_height((int)count, 0), NULL, NULL);
	for (at = 0; at < count; at++)
		y = se_row_value(app, canvas, x, y, width, welcome_keys[at][0], kl_tr(welcome_keys[at][1]), at + 1U == count);

	/* The edge below the card. */
	return top + se_card_height((int)count, 0);
}

/* Draws the Done step; returns the edge below it. */
static int
welcome_done(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	/* A header alone. */
	return welcome_header(app, canvas, x, top, width, "You are all set",
			      "Start using Kei opens Files at Today. The Welcome is in Settings > About whenever you want it again.");
}

/* Draws a step's header: its title large and a line under it; returns the edge below. */
static int
welcome_header(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width,
	const char *title,
	const char *summary)
{
	struct se_page page;

	/* A page's header, of the step's words. */
	memset(&page, 0, sizeof(page));
	page.name = title;
	page.summary = summary;
	return se_page_header(app, canvas, &page, x, top, width);
}

/*
 * Ends the Welcome: welcome.done set (the compositor starts it no more),
 * the window closed, and (finished, not skipped) Files opened at Today.
 */
static void
welcome_finish(
	struct se_app *app,
	int skipped)
{
	const char *how;
	int error;

	/* The setting (a failure is logged; the window closes all the same). */
	error = se_look_set(app, "welcome.done", "1");
	how = "done";
	if (skipped)
		how = "skip";
	se_log("WELCOME %s error=%d", how, error);

	/* Files at Today, then the window goes. */
	if (!skipped)
		app->request_files = 1;
	app->welcome = 0;
	app->request = SE_REQUEST_CLOSE;
}

/* Writes the account's name for the greeting: its full name (GECOS), else its login, else "there". */
static void
welcome_name(
	char *name,
	size_t size)
{
	const struct passwd *account;
	const char *comma;
	size_t length;

	/* The account of the process. */
	(void)snprintf(name, size, "%s", "there");
	account = getpwuid(getuid());
	if (account == NULL)
		return;
	(void)snprintf(name, size, "%s", account->pw_name);

	/* Its full name, to the first comma, when it has one. */
	if (account->pw_gecos == NULL || account->pw_gecos[0] == '\0')
		return;
	comma = strchr(account->pw_gecos, ',');
	length = strlen(account->pw_gecos);
	if (comma != NULL)
		length = (size_t)(comma - account->pw_gecos);
	if (length == 0)
		return;
	if (length >= size)
		length = size - 1U;
	memcpy(name, account->pw_gecos, length);
	name[length] = '\0';
}
