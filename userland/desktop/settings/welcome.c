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
 *
 * ws177-p007: a welcome.done that cannot be set, and a Files that cannot
 * be started, are said over the bar (the window stays until the next press);
 * the Network step says when joining failed or no network was found, and
 * what to do next; the headers and the greeting are broken into lines when
 * a narrow window or a long translation needs it, and the bar leaves out
 * its dots where they would run into the buttons; Enter goes Next, Esc
 * closes the window (the Welcome comes again at the next login), Alt+Left
 * and Alt+Right go Back and Next.
 */

#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* A header's title and summary sizes (libkeiland's kl_header's), the lines a summary takes at most, and the room under a line. */
#define WELCOME_TEXT_TITLE	30U
#define WELCOME_TEXT_SUMMARY	15U
#define WELCOME_SUMMARY_LINES	4
#define WELCOME_LINE_GAP	4

/* The bar's message: its size and the lines it takes at most. */
#define WELCOME_TEXT_MESSAGE	13U
#define WELCOME_MESSAGE_LINES	2

/* The room kept between the bar's dots or message and its buttons. */
#define WELCOME_BAR_GAP		12

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
static void welcome_name(const struct se_app *app, char *name, size_t size);
static int welcome_lines(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width, const char *text, unsigned pixels, int bold, kl_color color, int most);
static int welcome_network_note(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static int welcome_no_network(const struct se_app *app);

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

		/* The header, what went wrong and what to do next (ws177-p007), then the page. */
		y = welcome_header(app, canvas, x, top, width - skip_width - 16, title, summary);
		y = welcome_network_note(app, canvas, x, y, width);
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
	int dots_width;
	int next_width;
	int back_width;
	int left;
	int right;
	int step;
	int y;

	/* A thin line over the bar. */
	y = panel->y + panel->height - WELCOME_BAR;
	line.x = panel->x + WELCOME_INSET;
	line.y = y;
	line.width = panel->width - 2 * WELCOME_INSET;
	line.height = 1;
	kl_canvas_fill(canvas, &line, SE_COLOR_PANEL_EDGE);

	/* Back, at the left, but on the first step; the room left of the middle starts after it. */
	button_y = y + (WELCOME_BAR - 32) / 2;
	left = panel->x + WELCOME_INSET;
	if (app->welcome_step > WELCOME_STEP_WELCOME) {
		back_width = se_button_width(app, kl_tr("Back"));
		(void)se_button_draw(app, canvas, left, button_y, kl_tr("Back"), 0, 1, SE_WELCOME_BACK);
		left += back_width + WELCOME_BAR_GAP;
	}

	/* Next, or Start using Kei (Close once Files could not be started), at the right; the middle ends before it. */
	label = kl_tr("Next");
	if (app->welcome_step == WELCOME_STEP_DONE)
		label = kl_tr("Start using Kei");
	if (app->welcome_step == WELCOME_STEP_DONE && app->welcome_files_failed)
		label = kl_tr("Close");
	next_width = se_button_width(app, label);
	(void)se_button_draw(app, canvas, panel->x + panel->width - WELCOME_INSET - next_width, button_y, label, 1, 1, SE_WELCOME_NEXT);
	right = panel->x + panel->width - WELCOME_INSET - next_width - WELCOME_BAR_GAP;

	/* What went wrong, in the middle in place of the dots (ws177-p007). */
	if (app->welcome_message[0] != '\0') {
		(void)welcome_lines(app, canvas, left, y + 10, right - left, app->welcome_message, WELCOME_TEXT_MESSAGE, 0, SE_COLOR_BAD, WELCOME_MESSAGE_LINES);
		return;
	}

	/* The dots, in the middle, when they fit between the buttons. */
	dots_width = WELCOME_STEPS * WELCOME_DOT + (WELCOME_STEPS - 1) * WELCOME_DOT_GAP;
	dots_x = panel->x + (panel->width - dots_width) / 2;
	if (dots_x < left || dots_x + dots_width > right)
		return;
	for (step = 0; step < WELCOME_STEPS; step++) {
		color = SE_COLOR_TEXT_SECONDARY;
		if (step == app->welcome_step)
			color = SE_COLOR_ACCENT;
		kl_canvas_circle(canvas, (float)(dots_x + step * (WELCOME_DOT + WELCOME_DOT_GAP) + WELCOME_DOT / 2), (float)(y + WELCOME_BAR / 2),
				 (float)WELCOME_DOT / 2.0f, color);
	}
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

/*
 * Takes a key the step's page did not (ws177-p007): Enter goes Next (or
 * starts Kei on the last step), Esc closes the window without the Welcome
 * done (it comes again at the next login), Alt+Left and Alt+Right go Back
 * and Next.  Returns 1 when the key was the Welcome's.
 */
int
se_welcome_key(
	struct se_app *app,
	const struct se_event *event)
{
	/* Alt with the arrows walks the steps. */
	if ((event->modifiers & SE_MOD_ALT) != 0U) {
		if (event->key == SE_KEY_LEFT && app->welcome_step > WELCOME_STEP_WELCOME)
			return se_welcome_press(app, SE_WELCOME_BACK);
		if (event->key == SE_KEY_RIGHT)
			return se_welcome_press(app, SE_WELCOME_NEXT);
		return 0;
	}

	/* Enter: Next. */
	if (event->key == SE_KEY_ENTER)
		return se_welcome_press(app, SE_WELCOME_NEXT);

	/* Esc: the window closes, nothing set. */
	if (event->key == SE_KEY_ESC) {
		se_log("WELCOME closed step=%d", app->welcome_step);
		app->welcome = 0;
		app->request = SE_REQUEST_CLOSE;
		return 1;
	}

	/* Succeeded: another key is not the Welcome's. */
	return 0;
}

/*
 * Brings the Welcome back at its last step when Files could not be
 * started as it ended (ws177-p007): the window stays, says so, and its
 * button only closes it.
 */
void
se_welcome_files_failed(
	struct se_app *app,
	int error)
{
	/* The last step again, without the list. */
	app->welcome = 1;
	app->welcome_step = WELCOME_STEP_DONE;
	app->show_sidebar = 0;
	app->page = (unsigned)welcome_page(app, WELCOME_STEP_DONE);
	app->page_scroll = 0;
	app->logged_count = -1;
	app->dirty = 1;

	/* What went wrong and what to do; Start becomes Close. */
	app->welcome_files_failed = 1;
	(void)snprintf(app->welcome_message, sizeof(app->welcome_message), "%s (%s)", kl_tr("Files could not be opened. Open it from App Home."), strerror(error));
	se_log("WELCOME files-failed error=%d", error);
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
	char name[128];
	char words[192];
	int baseline;
	int bottom;

	/* The mark. */
	se_mark_draw(canvas, x, top, WELCOME_MARK, 1.0f);

	/* The greeting under it, with the account's name. */
	welcome_name(app, name, sizeof(name));
	(void)kl_tr_format(words, sizeof(words), kl_tr("Welcome to Kei, {1}"), name, (const char *)NULL);
	kl_text_metrics(app->text, WELCOME_TEXT_BIG, &title);
	baseline = top + (int)WELCOME_MARK + 28 + title.ascent;
	(void)kl_text_draw_fit(app->text, canvas, x, baseline, words, WELCOME_TEXT_BIG, 1, width, SE_COLOR_TITLE);

	/* A line on what follows, broken into lines when it is longer than the column (ws177-p007). */
	kl_text_metrics(app->text, WELCOME_TEXT, &line);
	bottom = welcome_lines(app, canvas, x, baseline + title.descent + 14, width, kl_tr("Let's set up a few things. You can change each of them later in Settings."),
			       WELCOME_TEXT, 0, SE_COLOR_TEXT_SECONDARY, WELCOME_SUMMARY_LINES);

	/* The edge below. */
	return bottom;
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
	struct kl_text_line line;
	int bottom;

	/* The title, bold, in the size of a page's header. */
	kl_text_metrics(app->text, WELCOME_TEXT_TITLE, &line);
	(void)kl_text_draw_fit(app->text, canvas, x, top + line.ascent, kl_tr(title), WELCOME_TEXT_TITLE, 1, width, SE_COLOR_TEXT);

	/* The summary under it, broken into lines when it is longer than the column (ws177-p007). */
	bottom = welcome_lines(app, canvas, x, top + line.height + 2, width, kl_tr(summary), WELCOME_TEXT_SUMMARY, 0, SE_COLOR_TEXT_SECONDARY, WELCOME_SUMMARY_LINES);

	/* Succeeded: the edge below the summary. */
	return bottom;
}

/*
 * Draws a text from a top edge in a column, broken into lines at its
 * width (after a space where it can be), at most a number of lines (the
 * last cut short).  Returns the edge below the last line.
 */
static int
welcome_lines(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width,
	const char *text,
	unsigned pixels,
	int bold,
	kl_color color,
	int most)
{
	struct kl_text_line line;
	size_t done;
	size_t length;
	int rows;
	int baseline;

	/* Each line, as much as fits. */
	kl_text_metrics(app->text, pixels, &line);
	done = 0U;
	rows = 0;
	baseline = top + line.ascent;
	while (text[done] != '\0' && rows < most) {
		/* The last line allowed takes the rest, cut short. */
		if (rows + 1 == most) {
			(void)kl_text_draw_fit(app->text, canvas, x, baseline, text + done, pixels, bold, width, color);
			rows++;
			break;
		}

		/* One line, broken where it fits. */
		length = kl_text_break(app->text, text + done, pixels, bold, width);
		if (length == 0U)
			break;
		(void)kl_text_draw(app->text, canvas, x, baseline, text + done, length, pixels, bold, color);
		rows++;
		baseline += line.height + WELCOME_LINE_GAP;

		/* The next line starts after the spaces the break left. */
		done += length;
		while (text[done] == ' ')
			done++;
	}

	/* No line: the edge is the top. */
	if (rows == 0)
		return top;

	/* Succeeded: the edge below the last line. */
	return top + rows * (line.height + WELCOME_LINE_GAP) - WELCOME_LINE_GAP;
}

/*
 * Draws, under the Network step's header, what went wrong and what to do
 * next (ws177-p007): a join that failed (the Wi-Fi page's message), or a
 * machine with no Wi-Fi and no wired connection.  Returns the edge below
 * (the top when there is nothing to say).
 */
static int
welcome_network_note(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	char words[SE_MESSAGE + 96];
	int none;
	int bottom;

	/* A join that failed: its reason, and the next step. */
	if (app->network.message_bad && app->network.message[0] != '\0') {
		(void)snprintf(words, sizeof(words), "%s %s", app->network.message, kl_tr("Try the key again, or press Next and connect later in Settings."));
		bottom = welcome_lines(app, canvas, x, top + WELCOME_CARD_GAP, width, words, WELCOME_TEXT, 0, SE_COLOR_BAD, WELCOME_SUMMARY_LINES);
		return bottom;
	}

	/* No network at all: Next goes on without one. */
	none = welcome_no_network(app);
	if (none) {
		bottom = welcome_lines(app, canvas, x, top + WELCOME_CARD_GAP, width,
				       kl_tr("No Wi-Fi and no wired connection were found. Press Next to go on; Settings > Ethernet or Wi-Fi connects later."),
				       WELCOME_TEXT, 0, SE_COLOR_TEXT_SECONDARY, WELCOME_SUMMARY_LINES);
		return bottom;
	}

	/* Succeeded: nothing to say. */
	return top;
}

/* Tells whether the machine has no network to join now: no Wi-Fi radio and no wired interface with its cable in. */
static int
welcome_no_network(
	const struct se_app *app)
{
	const struct kl_network_link *link;
	size_t index;

	/* A Wi-Fi radio is a network to join. */
	if (app->network.state.wifi != KL_WIFI_ABSENT)
		return 0;

	/* A running interface but the loopback is a wired connection. */
	for (index = 0U; index < app->network.link_count; index++) {
		link = &app->network.links[index];
		if (link->loopback)
			continue;
		if (link->running)
			return 0;
	}

	/* Succeeded: none. */
	return 1;
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

	/* The setting; a failure is logged. */
	error = se_look_set(app, "welcome.done", "1");
	how = "done";
	if (skipped)
		how = "skip";
	se_log("WELCOME %s error=%d", how, error);

	/*
	 * A setting that could not be kept is said first, and the window stays
	 * (ws177-p007): the Welcome will come again at the next login.  The
	 * next press goes on as before.
	 */
	if (error != 0 && !app->welcome_unsaved) {
		app->welcome_unsaved = 1;
		(void)snprintf(app->welcome_message, sizeof(app->welcome_message), "%s (%s)",
			       kl_tr("The Welcome could not be marked as done; it will show again at the next login. Press again to go on."), strerror(error));
		app->dirty = 1;
		return;
	}

	/* Files at Today (not when it could not be started before), then the window goes. */
	if (!skipped && !app->welcome_files_failed)
		app->request_files = 1;
	app->welcome = 0;
	app->request = SE_REQUEST_CLOSE;
}

/*
 * Writes the account's name for the greeting: its full name, else its
 * login, else "there" (before the desktop told the account, ws188-p002:
 * the frame that follows its answer shows the name).
 */
static void
welcome_name(
	const struct se_app *app,
	char *name,
	size_t size)
{
	const struct se_users *users;

	/* The account as the desktop told it. */
	users = &app->users;
	if (users->full_name[0] != '\0') {
		(void)snprintf(name, size, "%s", users->full_name);
		return;
	}

	/* Without a full name, the login. */
	if (users->name[0] != '\0') {
		(void)snprintf(name, size, "%s", users->name);
		return;
	}

	/* Not told yet. */
	(void)snprintf(name, size, "%s", "there");
}
