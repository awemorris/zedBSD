/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws074-p056: the host test of the view's input (<browser/browser.h>): the keys
 * with the DOM's names and their default actions (scrolling, Tab and the
 * focus ring, Enter on a link, the history's keys, reloading), the wheel,
 * the pointer's buttons and clicks, and the program's focus, on
 * plan/ws074/tests/pages/keys.html with its listeners, which write what
 * they get to the console.
 *
 *   host-view PAGES SANS MONO FALLBACK
 *
 * (PAGES: plan/ws074/tests/pages; the fonts: build/ws035-fonts/Mahora-Regular.ttf,
 * JetBrainsMono-Regular.ttf, DroidSansFallbackFull.ttf).  Prints one line
 * per failed check and a summary; -v prints the console too.
 */

#include <browser/browser.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The public header has no helper for unused parameters. */
#define UNUSED_PARAMETER(name)	((void)(name))

/* The most console text kept since the last mark, in bytes. */
#define TEST_CONSOLE_MAX	65536U

/* The focus ring's color in the paint dump. */
#define TEST_RING_COLOR		"#ff1a73e8"

/*
 * What the callbacks saw: the console since the last mark, the last link
 * asked for and whether links are followed, and how many pages were shown.
 */
struct test_state {
	char console[TEST_CONSOLE_MAX];
	size_t console_length;
	char link[1024];
	int links;
	int follow;
	int committed;
	int verbose;
};

static int failures;
static int checks;
static struct test_state state;

static void check(int condition, const char *what);
static void mark(void);
static int heard(const char *text);
static int ring_drawn(struct browser_view *view);
static void press(struct browser_view *view, const char *key, const char *code, const char *text, uint32_t modifiers);
static void click(struct browser_view *view, float x, float y, int button);
static void on_console(void *context, struct browser_view *view, int level, const char *text, size_t length);
static enum browser_policy on_link(void *context, struct browser_view *view, const char *href);
static void on_committed(void *context, struct browser_view *view);
static void on_load(void *context, struct browser_view *view, enum browser_load_state load, const char *url, int error, const char *reason);

int
main(
	int argc,
	char **argv)
{
	struct browser_fonts paths;
	struct browser_callbacks callbacks;
	struct browser_view_options options;
	struct browser_view *view;
	char path[1024];
	double bottom;
	double before;
	const char *url;
	int error;

	/* ws074-p080: localStorage goes under XDG_DATA_HOME; the test keeps it under build/, away from ~/.local/share. */
	setenv("XDG_DATA_HOME", "build/ws074-host-data", 1);

	if (argc < 5) {
		fprintf(stderr, "usage: host-view PAGES SANS MONO FALLBACK [-v]\n");
		return 2;
	}
	if (argc > 5 && strcmp(argv[5], "-v") == 0)
		state.verbose = 1;

	/* The view on keys.html at 800 by 600, every page read at once. */
	memset(&paths, 0, sizeof(paths));
	paths.sans = argv[2];
	paths.mono = argv[3];
	paths.fallback = argv[4];
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.console = on_console;
	callbacks.link = on_link;
	callbacks.committed = on_committed;
	callbacks.load = on_load;
	callbacks.context = &state;
	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION;
	options.fonts = &paths;
	options.callbacks = &callbacks;
	options.stack_base = __builtin_frame_address(0);
	options.width = 800;
	options.height = 600;
	options.fetch = BROWSER_FETCH_AT_ONCE;
	error = browser_view_create(&options, &view);
	check(error == 0, "view: made");
	if (error != 0)
		return 1;
	snprintf(path, sizeof(path), "%s/keys.html", argv[1]);
	error = browser_view_load(view, path);
	check(error == 0, "view: keys.html loads");
	if (error != 0)
		return 1;
	error = browser_view_settle(view, 1000.0, BROWSER_SETTLE_LAYOUT);
	check(error == 0, "view: keys.html settles");

	/* 1. A character key: keydown, keypress and keyup with the DOM's names and the legacy number. */
	mark();
	press(view, "x", "KeyX", "x", 0);
	check(heard("keydown key=x code=KeyX keyCode=88 shift=false ctrl=false alt=false repeat=false"), "key: keydown x");
	check(heard("keypress key=x"), "key: keypress x");
	check(heard("keyup key=x code=KeyX"), "key: keyup x");
	mark();
	press(view, "X", "KeyX", "X", BROWSER_MOD_SHIFT);
	check(heard("keydown key=X code=KeyX keyCode=88 shift=true"), "key: Shift+x is X");
	mark();
	press(view, "c", "KeyC", "", BROWSER_MOD_CTRL);
	check(heard("keydown key=c code=KeyC keyCode=67 shift=false ctrl=true"), "key: Ctrl+c");
	check(!heard("keypress"), "key: Ctrl+c types nothing");
	mark();
	(void)browser_view_key(view, "a", "KeyA", "a", 1, 1, 0);
	check(heard("repeat=true"), "key: a held repeats");
	(void)browser_view_key(view, "a", "KeyA", "a", 0, 0, 0);

	/* 2. Tab: tabindex 1, then 2, then the rest in the document's order, with the ring. */
	check(!ring_drawn(view), "focus: no ring before Tab");
	mark();
	press(view, "Tab", "Tab", "", 0);
	check(heard("focusin three"), "focus: Tab goes to tabindex 1");
	check(ring_drawn(view), "focus: the ring is drawn");
	mark();
	press(view, "Tab", "Tab", "", 0);
	check(heard("focusout three") && heard("focusin two"), "focus: then tabindex 2");
	mark();
	press(view, "Tab", "Tab", "", 0);
	check(heard("focusin one"), "focus: then the first link");
	mark();
	press(view, "Tab", "Tab", "", 0);
	check(heard("focusin four"), "focus: then the button");
	check(browser_view_scroll_y(view) == 0.0, "focus: no scroll for an element in view");
	mark();
	press(view, "Tab", "Tab", "", 0);
	check(heard("focusin last"), "focus: then the last link");
	check(browser_view_scroll_y(view) > 0.0, "focus: the last link is scrolled into view");
	mark();
	press(view, "Tab", "Tab", "", 0);
	check(heard("focusout last") && !heard("focusin"), "focus: past the end nothing is focused");
	check(!ring_drawn(view), "focus: no ring when nothing is focused");
	mark();
	press(view, "Tab", "Tab", "", BROWSER_MOD_SHIFT);
	check(heard("focusin last"), "focus: Shift+Tab from nothing goes to the last");
	mark();
	press(view, "Tab", "Tab", "", BROWSER_MOD_SHIFT);
	check(heard("focusin four"), "focus: Shift+Tab goes back");
	press(view, "Tab", "Tab", "", 0);

	/* 3. Enter on the focused link: its click, then the link (denied here). */
	mark();
	state.links = 0;
	press(view, "Enter", "Enter", "", 0);
	check(heard("click target=last"), "enter: the link gets a click");
	check(state.links == 1 && strcmp(state.link, "blocks.html") == 0, "enter: the link is followed");

	/* 4. The keys that scroll, and a keydown canceled by the page. */
	press(view, "Home", "Home", "", 0);
	check(browser_view_scroll_y(view) == 0.0, "scroll: Home");
	press(view, "End", "End", "", 0);
	bottom = browser_view_scroll_y(view);
	check(bottom > 600.0, "scroll: End");
	press(view, "ArrowUp", "ArrowUp", "", 0);
	check(browser_view_scroll_y(view) == bottom - 40.0, "scroll: ArrowUp is 40 pixels");
	before = browser_view_scroll_y(view);
	press(view, "ArrowDown", "ArrowDown", "", BROWSER_MOD_SHIFT);
	check(browser_view_scroll_y(view) == before, "scroll: a canceled keydown does not scroll");
	press(view, "ArrowDown", "ArrowDown", "", 0);
	check(browser_view_scroll_y(view) == before + 40.0, "scroll: ArrowDown");
	press(view, "Home", "Home", "", 0);
	press(view, "PageDown", "PageDown", "", 0);
	check(browser_view_scroll_y(view) == 560.0, "scroll: PageDown is the view less 40 pixels");
	press(view, " ", "Space", " ", BROWSER_MOD_SHIFT);
	check(browser_view_scroll_y(view) == 0.0, "scroll: Shift+Space goes up a page");
	press(view, " ", "Space", " ", 0);
	check(browser_view_scroll_y(view) == 560.0, "scroll: Space goes down a page");
	press(view, "PageUp", "PageUp", "", 0);
	check(browser_view_scroll_y(view) == 0.0, "scroll: PageUp");

	/* 5. The wheel, and a wheel event canceled by the page. */
	mark();
	error = browser_view_wheel(view, 100.0f, 100.0f, 0.0f, 120.0f, 0);
	check(error == 0 && browser_view_scroll_y(view) == 120.0, "wheel: scrolls by its distance");
	check(heard("wheel deltaY=120 ctrl=false"), "wheel: the page gets it");
	error = browser_view_wheel(view, 100.0f, 100.0f, 0.0f, 120.0f, BROWSER_MOD_CTRL);
	check(error == 0 && browser_view_scroll_y(view) == 120.0, "wheel: a canceled wheel does not scroll");
	press(view, "Home", "Home", "", 0);

	/* 6. A click on the first link: mousedown, mouseup, click, the focus without a ring, and the link. */
	mark();
	state.links = 0;
	click(view, 55.0f, 104.0f, BROWSER_BUTTON_PRIMARY);
	check(heard("mousedown button=0 target=one"), "click: mousedown");
	check(heard("mouseup button=0"), "click: mouseup");
	check(heard("click target=one"), "click: click");
	check(heard("focusin one"), "click: the press focuses the link");
	check(!ring_drawn(view), "click: no ring for a focus of the pointer");
	check(state.links == 1 && strcmp(state.link, "first.html") == 0, "click: the link is followed");
	mark();
	state.links = 0;
	(void)browser_view_pointer_button(view, 55.0f, 104.0f, BROWSER_BUTTON_PRIMARY, 1, 0);
	(void)browser_view_pointer_button(view, 90.0f, 104.0f, BROWSER_BUTTON_PRIMARY, 0, 0);
	check(!heard("click target") && state.links == 0, "click: a drag is not a click");
	mark();
	click(view, 400.0f, 500.0f, BROWSER_BUTTON_PRIMARY);
	check(heard("focusout one"), "click: a press on nothing takes the focus away");

	/* 7. The program's focus: the ring only while the view has it. */
	press(view, "Tab", "Tab", "", 0);
	check(ring_drawn(view), "window: the ring after Tab");
	mark();
	(void)browser_view_focus(view, 0);
	check(heard("focusout three"), "window: the focused element hears the focus go");
	check(!ring_drawn(view), "window: no ring without the focus");
	mark();
	(void)browser_view_focus(view, 1);
	check(heard("focusin three"), "window: and come back");
	check(ring_drawn(view), "window: the ring again");

	/* 8. The history: a link followed, then Alt+Left, Alt+Right, the back key, the forward button, F5. */
	state.follow = 1;
	state.committed = 0;
	press(view, "Home", "Home", "", 0);
	click(view, 55.0f, 104.0f, BROWSER_BUTTON_PRIMARY);
	url = browser_view_url(view);
	check(state.committed == 1 && strstr(url, "first.html") != NULL, "history: the link is followed");
	press(view, "ArrowLeft", "ArrowLeft", "", BROWSER_MOD_ALT);
	url = browser_view_url(view);
	check(state.committed == 2 && strstr(url, "keys.html") != NULL, "history: Alt+Left goes back");
	press(view, "ArrowRight", "ArrowRight", "", BROWSER_MOD_ALT);
	url = browser_view_url(view);
	check(state.committed == 3 && strstr(url, "first.html") != NULL, "history: Alt+Right goes forward");
	press(view, "BrowserBack", "BrowserBack", "", 0);
	url = browser_view_url(view);
	check(state.committed == 4 && strstr(url, "keys.html") != NULL, "history: the back key");
	click(view, 10.0f, 10.0f, BROWSER_BUTTON_FORWARD);
	url = browser_view_url(view);
	check(state.committed == 5 && strstr(url, "first.html") != NULL, "history: the forward button");
	press(view, "F5", "F5", "", 0);
	check(state.committed == 6 && strstr(browser_view_url(view), "first.html") != NULL, "history: F5 reloads");
	press(view, "r", "KeyR", "", BROWSER_MOD_CTRL);
	check(state.committed == 7, "history: Ctrl+R reloads");
	press(view, "ArrowLeft", "ArrowLeft", "", 0);
	check(state.committed == 7, "history: Left alone does not go back");
	press(view, "Escape", "Escape", "", 0);
	check(state.committed == 7, "history: Escape without a load does nothing");

	browser_view_destroy(view);
	printf("host-view: %d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}

/* Counts a check and reports it when it failed. */
static void
check(
	int condition,
	const char *what)
{
	checks++;
	if (condition)
		return;
	failures++;
	printf("FAIL %s\n", what);
}

/* Forgets the console heard so far. */
static void
mark(void)
{
	state.console_length = 0;
	state.console[0] = '\0';
}

/* Tells whether the console since the last mark has a text. */
static int
heard(
	const char *text)
{
	return strstr(state.console, text) != NULL;
}

/* Tells whether the display list has the focus ring. */
static int
ring_drawn(
	struct browser_view *view)
{
	char *text;
	size_t length;
	int found;
	int error;

	error = browser_view_dump(view, BROWSER_DUMP_PAINT, &text, &length);
	if (error != 0)
		return 0;
	found = strstr(text, TEST_RING_COLOR) != NULL;
	browser_free(text);
	return found;
}

/* Presses a key and lets it go. */
static void
press(
	struct browser_view *view,
	const char *key,
	const char *code,
	const char *text,
	uint32_t modifiers)
{
	int error;

	error = browser_view_key(view, key, code, text, 1, 0, modifiers);
	if (error != 0)
		printf("key %s: error %d\n", key, error);
	error = browser_view_key(view, key, code, text, 0, 0, modifiers);
	if (error != 0)
		printf("key %s up: error %d\n", key, error);
}

/* Presses a button and lets it go at the same place. */
static void
click(
	struct browser_view *view,
	float x,
	float y,
	int button)
{
	int error;

	(void)browser_view_pointer_move(view, x, y, 0);
	error = browser_view_pointer_button(view, x, y, button, 1, 0);
	if (error != 0)
		printf("button %d: error %d\n", button, error);
	error = browser_view_pointer_button(view, x, y, button, 0, 0);
	if (error != 0)
		printf("button %d up: error %d\n", button, error);
}

/* Keeps a console line. */
static void
on_console(
	void *context,
	struct browser_view *view,
	int level,
	const char *text,
	size_t length)
{
	struct test_state *test;

	UNUSED_PARAMETER(view);
	UNUSED_PARAMETER(level);
	test = context;
	if (test->verbose)
		printf("console: %.*s\n", (int)length, text);
	if (test->console_length + length + 2U > sizeof(test->console))
		return;
	memcpy(test->console + test->console_length, text, length);
	test->console_length += length;
	test->console[test->console_length++] = '\n';
	test->console[test->console_length] = '\0';
}

/* Keeps the link asked for, and follows it when the test says so. */
static enum browser_policy
on_link(
	void *context,
	struct browser_view *view,
	const char *href)
{
	struct test_state *test;

	UNUSED_PARAMETER(view);
	test = context;
	snprintf(test->link, sizeof(test->link), "%s", href);
	test->links++;
	if (test->follow)
		return BROWSER_POLICY_ALLOW;
	return BROWSER_POLICY_DENY;
}

/* Counts the pages shown. */
static void
on_committed(
	void *context,
	struct browser_view *view)
{
	struct test_state *test;

	UNUSED_PARAMETER(view);
	test = context;
	test->committed++;
}

/* Reports a load that failed. */
static void
on_load(
	void *context,
	struct browser_view *view,
	enum browser_load_state load,
	const char *url,
	int error,
	const char *reason)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(view);
	if (load == BROWSER_LOAD_FAILED)
		printf("load failed: %s error %d %s\n", url, error, reason);
}
