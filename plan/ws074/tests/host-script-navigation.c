/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of what a search page's buttons need, through the view
 * (<browser/browser.h>), on plan/ws074/tests/pages/script-navigation*.html:
 *
 * - pointer-events: none (inherited) and visibility: hidden boxes over a
 *   submit button do not take its click, which submits the form; a child
 *   with pointer-events: auto inside a pointer-events: none box is hit;
 * - a top-level page's script navigation (form.submit() with a textarea
 *   and a select, and an assignment to location.href) goes to the link
 *   callback;
 * - the DOM members the page's scripts call: compareDocumentPosition,
 *   getElementsByName, getAttributeNS and hasAttributeNS, the name, type
 *   and value of textarea, select and option.
 *
 *   host-script-navigation
 *
 * Run from the repository's root (the pages and the fonts are the
 * checked-in ones).  The link callback denies every link, so each page
 * stays.  Prints one line per failed check and a summary.
 */

#include <browser/browser.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The most console text kept, in bytes, and how long a page may take to ask for a link. */
#define NAV_CONSOLE_MAX		16384U
#define NAV_WAIT_MS		3000.0

/* The pages' directory and the checked-in fonts. */
#define NAV_PAGES		"plan/ws074/tests/pages/"

/* What the callbacks saw: the console, and the last link asked for. */
struct nav_state {
	char console[NAV_CONSOLE_MAX];
	size_t console_length;
	char link[4096];
	int links;
};

static unsigned checks;
static unsigned failures;
static struct nav_state state;

static void nav_check(int condition, const char *what);
static int nav_heard(const char *text);
static int nav_link_has(const char *text);
static void nav_reset(void);
static double nav_now(void);
static void nav_wait_link(struct browser_view *view);
static void nav_click(struct browser_view *view, float x, float y);
static struct browser_view *nav_open(const char *page);
static void nav_console(void *context, struct browser_view *view, int level, const char *text, size_t length);
static enum browser_policy nav_link(void *context, struct browser_view *view, const char *href);

/* The view's fonts: the checked-in ones. */
static const struct browser_fonts nav_fonts = {
	"userland/desktop/fonts/Mahora-Regular.ttf", "userland/desktop/fonts/JetBrainsMono-Regular.ttf",
	"userland/desktop/fonts/DroidSansFallbackFull.ttf", "userland/desktop/fonts/Mahora-Bold.ttf",
	"userland/desktop/fonts/Mahora-Mono.ttf"
};

/* The callbacks, the same for every view. */
static struct browser_callbacks nav_callbacks;

int
main(
	void)
{
	struct browser_view *view;

	/* Storage stays under build/, away from the home directory. */
	setenv("XDG_DATA_HOME", "build/ws074-host-data", 1);
	nav_callbacks.console = nav_console;
	nav_callbacks.link = nav_link;
	nav_callbacks.context = &state;

	/* 1. The page's members, as its script logged them at load. */
	view = nav_open("script-navigation.html");
	if (view == NULL)
		return 1;
	nav_check(nav_heard("pointer-events none none auto auto"), "pointer-events: computed, inherited, overridden");
	nav_check(nav_heard("position 4 2 20 10 0 33 16"), "compareDocumentPosition: following, preceding, contained, contains, same, disconnected");
	nav_check(nav_heard("names 1 textarea 0"), "getElementsByName");
	nav_check(nav_heard("namespaced none.png true null"), "getAttributeNS and hasAttributeNS");
	nav_check(nav_heard("controls q textarea s select-one 2 one"), "textarea and select: name, type, value; option: value");

	/* 2. A click on the button under pointer-events: none and visibility: hidden boxes submits the form. */
	nav_reset();
	nav_click(view, 50.0f, 120.0f);
	nav_check(nav_heard("click b"), "click through: the button is the click's target");
	nav_check(state.links == 1 && nav_link_has("script-navigation-target.html?q=abc&s=2&b=go"), "click through: the form is submitted");

	/* 3. A pointer-events: auto child inside a pointer-events: none box is hit. */
	nav_reset();
	nav_click(view, 250.0f, 320.0f);
	nav_check(nav_heard("click live"), "click: an auto child of a none box is the target");
	nav_check(state.links == 0, "click: no link from the child");
	browser_view_destroy(view);

	/* 4. form.submit() from a timer: the entries of the textarea, the select and the field. */
	nav_reset();
	view = nav_open("script-navigation-submit.html");
	if (view == NULL)
		return 1;
	nav_wait_link(view);
	nav_check(nav_heard("submitting"), "submit(): the timer ran");
	nav_check(state.links == 1 && nav_link_has("script-navigation-target.html?q=a+b&s=one&t=v"), "submit(): the view is asked to follow the submission");
	browser_view_destroy(view);

	/* 5. location.href from a timer. */
	nav_reset();
	view = nav_open("script-navigation-location.html");
	if (view == NULL)
		return 1;
	nav_wait_link(view);
	nav_check(nav_heard("assigning"), "location: the timer ran");
	nav_check(state.links == 1 && nav_link_has("script-navigation-target.html?from=location"), "location: the view is asked to follow it");
	browser_view_destroy(view);

	/* The summary. */
	printf("host-script-navigation: %u checks, %u failed\n", checks, failures);
	if (failures != 0)
		return 1;
	return 0;
}

/* Counts a check and reports it when it failed. */
static void
nav_check(
	int condition,
	const char *what)
{
	checks++;
	if (!condition) {
		failures++;
		printf("FAIL %s (links %d: %s)\n", what, state.links, state.link);
	}
}

/* Tells whether a line of the console since the last reset has the text. */
static int
nav_heard(
	const char *text)
{
	return strstr(state.console, text) != NULL;
}

/* Tells whether the last link asked for has the text. */
static int
nav_link_has(
	const char *text)
{
	return strstr(state.link, text) != NULL;
}

/* Forgets the console and the links seen. */
static void
nav_reset(void)
{
	memset(&state, 0, sizeof(state));
}

/* The monotonic clock, in milliseconds. */
static double
nav_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1e6;
}

/* Runs the view as a program's loop does until a link is asked for, or the wait ends. */
static void
nav_wait_link(
	struct browser_view *view)
{
	struct timespec pause;
	double end;

	end = nav_now() + NAV_WAIT_MS;
	pause.tv_sec = 0;
	pause.tv_nsec = 5000000L;
	while (state.links == 0 && nav_now() < end) {
		browser_view_process(view);
		nanosleep(&pause, NULL);
	}
}

/* Clicks the main button at a point, then lets the view do what the click left to do. */
static void
nav_click(
	struct browser_view *view,
	float x,
	float y)
{
	(void)browser_view_pointer_move(view, x, y, 0);
	(void)browser_view_pointer_button(view, x, y, BROWSER_BUTTON_PRIMARY, 1, 0);
	(void)browser_view_pointer_button(view, x, y, BROWSER_BUTTON_PRIMARY, 0, 0);
	browser_view_process(view);
}

/* Makes a view of 800 by 600 on a page of the directory, laid out; NULL (counted as a failure) when it cannot. */
static struct browser_view *
nav_open(
	const char *page)
{
	struct browser_view_options options;
	struct browser_view *view;
	char path[512];
	int error;

	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION;
	options.fonts = &nav_fonts;
	options.callbacks = &nav_callbacks;
	options.stack_base = __builtin_frame_address(0);
	options.width = 800;
	options.height = 600;
	options.fetch = BROWSER_FETCH_AT_ONCE;
	error = browser_view_create(&options, &view);
	nav_check(error == 0, "view: made");
	if (error != 0)
		return NULL;
	snprintf(path, sizeof(path), NAV_PAGES "%s", page);
	error = browser_view_load(view, path);
	nav_check(error == 0, path);
	if (error != 0) {
		browser_view_destroy(view);
		return NULL;
	}
	(void)browser_view_settle(view, 1000.0, BROWSER_SETTLE_LAYOUT);
	return view;
}

/* Keeps what the page writes to the console, a line each. */
static void
nav_console(
	void *context,
	struct browser_view *view,
	int level,
	const char *text,
	size_t length)
{
	struct nav_state *seen;
	size_t room;

	(void)view;
	(void)level;
	seen = context;
	room = sizeof(seen->console) - seen->console_length - 1U;
	if (length + 1U > room)
		return;
	memcpy(seen->console + seen->console_length, text, length);
	seen->console_length += length;
	seen->console[seen->console_length++] = '\n';
	seen->console[seen->console_length] = '\0';
}

/* Keeps the link the view asks to follow, and denies it so that the page stays. */
static enum browser_policy
nav_link(
	void *context,
	struct browser_view *view,
	const char *href)
{
	struct nav_state *seen;

	(void)view;
	seen = context;
	snprintf(seen->link, sizeof(seen->link), "%s", href);
	seen->links++;
	return BROWSER_POLICY_DENY;
}
