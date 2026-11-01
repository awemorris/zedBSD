/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws074-p032: the host test of the form controls through the view
 * (<browser/browser.h>): the focus moving into fields with Tab and a click, typing,
 * Backspace, Delete, the arrows, Home and End, maxlength, a password, the
 * placeholder, a textarea's new lines, checkboxes and radio buttons with
 * Space and a click, and the submission by Enter in a field and by a
 * submit button (the entries, their order and encoding in UTF-8 and in
 * windows-1252), on plan/ws074/tests/pages/form.html, whose listeners write
 * submit, input and change to the console.  The submission's location is
 * what the view asks the link callback to follow.
 *
 *   host-form PAGES SANS MONO FALLBACK [-v]
 *
 * Prints one line per failed check and a summary.
 */

#include <browser/browser.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most console text kept since the last mark, in bytes. */
#define TEST_CONSOLE_MAX	65536U

/*
 * What the callbacks saw: the console since the last mark, and the last
 * location the view asked to follow (links are not followed, so the page
 * stays).
 */
struct test_state {
	char console[TEST_CONSOLE_MAX];
	size_t console_length;
	char link[4096];
	int links;
	int verbose;
};

static int failures;
static int checks;
static struct test_state state;

static void check(int condition, const char *what);
static void check_link(const char *expected, const char *what);
static void mark(void);
static int heard(const char *text);
static void press(struct browser_view *view, const char *key, const char *code, const char *text, uint32_t modifiers);
static void type(struct browser_view *view, const char *text);
static void tabs(struct browser_view *view, int count);
static int paint_has(struct browser_view *view, const char *text);
static void on_console(void *context, struct browser_view *view, int level, const char *text, size_t length);
static enum browser_policy on_link(void *context, struct browser_view *view, const char *href);

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
	int error;

	/* ws074-p080: localStorage goes under XDG_DATA_HOME; the test keeps it under build/, away from ~/.local/share. */
	setenv("XDG_DATA_HOME", "build/ws074-host-data", 1);

	if (argc < 5) {
		fprintf(stderr, "usage: host-form PAGES SANS MONO FALLBACK [-v]\n");
		return 2;
	}
	if (argc > 5 && strcmp(argv[5], "-v") == 0)
		state.verbose = 1;

	/* The view on form.html at 800 by 600. */
	memset(&paths, 0, sizeof(paths));
	paths.sans = argv[2];
	paths.mono = argv[3];
	paths.fallback = argv[4];
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.console = on_console;
	callbacks.link = on_link;
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
	snprintf(path, sizeof(path), "%s/form.html", argv[1]);
	error = browser_view_load(view, path);
	check(error == 0, "view: form.html loads");
	if (error != 0)
		return 1;
	error = browser_view_settle(view, 1000.0, BROWSER_SETTLE_LAYOUT);
	check(error == 0, "view: form.html settles");
	(void)browser_view_focus(view, 1);

	/* 1. The controls are drawn: the fields' values, the placeholder, the labels, the chosen option. */
	check(paint_has(view, "\"kei\""), "draw: the query's value");
	check(paint_has(view, "\"Type here\""), "draw: the placeholder");
	check(paint_has(view, "text 50.58 120.00 13px face 0 bold 0 #ff757575 \"Type here\""),
	    "draw: text-indent moves the placeholder");
	check(paint_has(view, "\"Search\""), "draw: a submit button's label");
	check(paint_has(view, "\"Books and more\""), "draw: the select's chosen option");
	check(paint_has(view, "\"first line\""), "draw: the textarea's first line");
	check(!paint_has(view, "\"abc\""), "draw: the password is not shown");

	/* 2. Tab into the first field, and type: input fires, the text and the caret show. */
	check(!paint_has(view, " 1.00 16.00 #ff000000"), "caret: none before the focus");
	tabs(view, 1);
	check(paint_has(view, " 1.00 16.00 #ff000000"), "caret: drawn in the focused field");
	type(view, "abc");
	check(paint_has(view, "\"abc\""), "type: the text shows");
	press(view, "Backspace", "Backspace", "", 0);
	press(view, "ArrowLeft", "ArrowLeft", "", 0);
	type(view, "X");
	press(view, "Home", "Home", "", 0);
	type(view, "<");
	press(view, "End", "End", "", 0);
	type(view, " >");
	press(view, "ArrowLeft", "ArrowLeft", "", 0);
	press(view, "ArrowLeft", "ArrowLeft", "", 0);
	press(view, "Delete", "Delete", "", 0);
	check(paint_has(view, "\"<aXb>\""), "edit: Backspace, the arrows, Home, End, Delete and Space");

	/* 3. Space in a field types; it does not scroll. */
	check(browser_view_scroll_y(view) == 0.0, "edit: Space did not scroll");

	/* 4. The query: typing fires input, and maxlength (12) stops it. */
	mark();
	tabs(view, 1);
	type(view, "0123456789");
	check(heard("input query"), "input: the query hears it");
	check(paint_has(view, "\"kei012345678\""), "maxlength: 12 characters");

	/* 5. A password shows bullets; the placeholder goes when the field has text. */
	tabs(view, 1);
	type(view, "d");
	check(paint_has(view, "\"\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\""), "password: four bullets");
	tabs(view, 1);
	type(view, "hi");
	check(!paint_has(view, "\"Type here\""), "placeholder: gone with text");

	/* 6. Space changes the checkboxes and the radio buttons. */
	mark();
	tabs(view, 2);
	press(view, " ", "Space", " ", 0);
	check(heard("change news"), "checkbox: Space changes it");
	check(browser_view_scroll_y(view) == 0.0, "checkbox: Space did not scroll");
	tabs(view, 2);
	press(view, " ", "Space", " ", 0);

	/* 7. Enter in a textarea is a new line. */
	tabs(view, 2);
	press(view, "End", "End", "", 0);
	press(view, "Enter", "Enter", "", 0);
	type(view, "third");
	check(paint_has(view, "\"third\""), "textarea: Enter starts a new line");

	/* 8. Enter in a field submits through the default button, UTF-8 and in tree order. */
	mark();
	state.links = 0;
	tabs(view, -6);
	type(view, "\xe6\xa4\x9c");
	press(view, "Enter", "Enter", "", 0);
	check(heard("submit search"), "submit: the form hears it");
	check(state.links == 1, "submit: Enter in a field submits");
	check_link("results.html?name=%3CaXb%3E&q=kei012345678&secret=abcd&hint=hi%E6%A4%9C&agree=on&news=yes&color=blue"
	    "&kind=books&notes=first+line%0D%0Asecond%0D%0Athird&source=hp&btn=Search", "submit: the entries");

	/* 9. A submit button submits with its own name, not the default button's. */
	state.links = 0;
	tabs(view, 8);
	press(view, "Enter", "Enter", "", 0);
	check(state.links == 1 && strstr(state.link, "&lucky=Lucky") != NULL && strstr(state.link, "btn=") == NULL,
	    "submit: the Lucky button is the submitter");

	/* 10. windows-1252: é as a byte, a character it lacks as a reference; the action's query and fragment go. */
	state.links = 0;
	tabs(view, 5);
	type(view, "\xc3\xa9\xe6\xa4\x9c");
	press(view, "Enter", "Enter", "", 0);
	check_link("latin.html?t=%E9%26%2326908%3B", "submit: windows-1252");

	/* 11. A POST form is not submitted in this pass. */
	state.links = 0;
	tabs(view, 2);
	type(view, "x");
	press(view, "Enter", "Enter", "", 0);
	check(state.links == 0, "submit: POST is not submitted yet");

	/* 12. Without the program's focus there is no caret. */
	(void)browser_view_focus(view, 0);
	check(!paint_has(view, " 1.00 16.00 #ff000000"), "caret: none without the focus");

	browser_view_destroy(view);
	printf("host-form: %d checks, %d failed\n", checks, failures);
	if (failures != 0)
		return 1;
	return 0;
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

/* Checks the last location asked for. */
static void
check_link(
	const char *expected,
	const char *what)
{
	int same;

	same = strcmp(state.link, expected) == 0;
	check(same, what);
	if (!same)
		printf("  got      %s\n  expected %s\n", state.link, expected);
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

/* Types UTF-8 text a character at a time, as the keys that type it. */
static void
type(
	struct browser_view *view,
	const char *text)
{
	char character[8];
	size_t length;

	while (*text != '\0') {
		length = 1;
		while (((unsigned char)text[length] & 0xc0U) == 0x80U)
			length++;
		memcpy(character, text, length);
		character[length] = '\0';
		press(view, character, "Unidentified", character, 0);
		text += length;
	}
}

/* Presses Tab (Shift+Tab for a negative count) a number of times. */
static void
tabs(
	struct browser_view *view,
	int count)
{
	uint32_t modifiers;

	modifiers = 0;
	if (count < 0) {
		modifiers = BROWSER_MOD_SHIFT;
		count = -count;
	}
	while (count > 0) {
		press(view, "Tab", "Tab", "", modifiers);
		count--;
	}
}

/* Tells whether the display list's dump has a text. */
static int
paint_has(
	struct browser_view *view,
	const char *text)
{
	char *dump;
	size_t length;
	int found;
	int error;

	error = browser_view_dump(view, BROWSER_DUMP_PAINT, &dump, &length);
	if (error != 0)
		return 0;
	found = strstr(dump, text) != NULL;
	if (state.verbose && !found)
		printf("-- paint has no %s\n", text);
	browser_free(dump);
	return found;
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
	size_t room;

	(void)view;
	(void)level;
	test = context;
	if (test->verbose)
		printf("console: %.*s\n", (int)length, text);
	room = TEST_CONSOLE_MAX - test->console_length - 2U;
	if (length > room)
		length = room;
	memcpy(test->console + test->console_length, text, length);
	test->console_length += length;
	test->console[test->console_length] = '\n';
	test->console_length++;
	test->console[test->console_length] = '\0';
}

/* Records a location the view asks to follow, and refuses it so the page stays. */
static enum browser_policy
on_link(
	void *context,
	struct browser_view *view,
	const char *href)
{
	struct test_state *test;

	(void)view;
	test = context;
	snprintf(test->link, sizeof(test->link), "%s", href);
	test->links++;
	return BROWSER_POLICY_DENY;
}
