/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws090-p025: the host test of an input method's text in a page's form
 * controls through the view (<browser/browser.h> only): which focused
 * element takes it (a text field and a textarea, not a password field)
 * with its caret's rectangle, the composed text shown at the caret
 * underlined without changing the value or firing input, the committed
 * text going in with input, the bytes deleted around the caret, and the
 * composing ending when the focus moves, its text going into the value
 * with input (q893, as a click would), on plan/ws090/tests/pages/ime.html,
 * whose listeners write each input with the control's value to the
 * console.
 *
 *   host-browser-ime PAGES SANS MONO FALLBACK [-v]
 *
 * Prints one line per failed check and a summary; exits 1 on a failure.
 */

#include <browser/browser.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most console text kept since the last mark, in bytes. */
#define TEST_CONSOLE_MAX	65536U

/* What the console said since the last mark, and whether each check is printed. */
struct test_state {
	char console[TEST_CONSOLE_MAX];
	size_t console_length;
	int verbose;
};

/* The checks made and failed, and the console; the test's one thread alone uses them. */
static int test_failures;
static int test_checks;
static struct test_state test_state;

static void check(int condition, const char *what);
static void mark(void);
static int heard(const char *text);
static void press(struct browser_view *view, const char *key, const char *code, const char *text, uint32_t modifiers);
static void tabs(struct browser_view *view, int count);
static int paint_has(struct browser_view *view, const char *text);
static int paint_underlines(struct browser_view *view);
static int target(struct browser_view *view, float *caret);
static void on_console(void *context, struct browser_view *view, int level, const char *text, size_t length);

/*
 * Runs the checks on ime.html.
 */
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
	float caret[4];
	int lines;
	int taken;
	int error;

	/* The page's storage stays under build/, and the arguments. */
	setenv("XDG_DATA_HOME", "build/ws090-host-data", 1);
	if (argc < 5) {
		fprintf(stderr, "usage: host-browser-ime PAGES SANS MONO FALLBACK [-v]\n");
		return 2;
	}
	if (argc > 5 && strcmp(argv[5], "-v") == 0)
		test_state.verbose = 1;

	/* The view on ime.html at 800 by 600, with the program's focus. */
	paths.sans = argv[2];
	paths.mono = argv[3];
	paths.fallback = argv[4];
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.console = on_console;
	callbacks.context = &test_state;
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
	snprintf(path, sizeof(path), "%s/ime.html", argv[1]);
	error = browser_view_load(view, path);
	check(error == 0, "view: ime.html loads");
	if (error != 0)
		return 1;
	error = browser_view_settle(view, 1000.0, BROWSER_SETTLE_LAYOUT);
	check(error == 0, "view: ime.html settles");
	(void)browser_view_focus(view, 1);

	/* 1. Nothing focused takes no text, and what comes is dropped; the lines a pixel tall the page has without an underline. */
	lines = paint_underlines(view);
	taken = target(view, caret);
	check(taken == 0, "target: none before the focus");
	error = browser_view_commit_text(view, "x", 0U, 0U);
	check(error == 0 && !paint_has(view, "\"x\""), "target: a commit without one is dropped");

	/* 2. The text field takes it, with its caret's rectangle where the field is. */
	tabs(view, 1);
	taken = target(view, caret);
	check(taken == 1, "target: the text field");
	check(caret[0] > 30.0f && caret[1] > 0.0f && caret[2] == 1.0f && caret[3] > 10.0f, "target: the caret's rectangle");
	press(view, "a", "KeyA", "a", 0U);
	press(view, "b", "KeyB", "b", 0U);

	/* 3. Composing shows the text at the caret, underlined, without input or a new value. */
	mark();
	error = browser_view_compose(view, "\xe3\x81\xab\xe3\x81\xbb", -1, -1);
	check(error == 0, "compose: taken");
	check(paint_has(view, "\"ab\xe3\x81\xab\xe3\x81\xbb\""), "compose: shown at the caret");
	check(paint_underlines(view) == lines + 1, "compose: underlined");
	check(!heard("input name"), "compose: no input");
	taken = target(view, caret);
	check(taken == 1 && caret[0] > 40.0f, "compose: the caret after the composed text");

	/* 4. Committing replaces the composed text with the committed one, with input. */
	mark();
	error = browser_view_compose(view, "", -1, -1);
	check(error == 0, "commit: the composing ends");
	error = browser_view_commit_text(view, "\xe6\x97\xa5\xe6\x9c\xac", 0U, 0U);
	check(error == 0, "commit: taken");
	check(heard("input name ab\xe6\x97\xa5\xe6\x9c\xac"), "commit: input with the value");
	check(paint_has(view, "\"ab\xe6\x97\xa5\xe6\x9c\xac\""), "commit: shown");
	check(paint_underlines(view) == lines, "commit: no underline");

	/* 5. A commit while composing replaces it; the deletion takes whole characters before the caret. */
	error = browser_view_compose(view, "z", -1, -1);
	check(error == 0 && paint_has(view, "\"ab\xe6\x97\xa5\xe6\x9c\xacz\""), "compose: again");
	mark();
	error = browser_view_commit_text(view, "", 3U, 0U);
	check(error == 0, "delete: taken");
	check(heard("input name ab\xe6\x97\xa5\n"), "delete: one character of three bytes before the caret");
	check(paint_underlines(view) == lines, "delete: the composed text went");
	press(view, "Home", "Home", "", 0U);
	mark();
	error = browser_view_commit_text(view, "", 0U, 2U);
	check(error == 0 && heard("input name \xe6\x97\xa5\n"), "delete: two bytes after the caret");

	/* 6. A password field takes keys only. */
	tabs(view, 1);
	taken = target(view, caret);
	check(taken == 0, "password: not a target");
	error = browser_view_compose(view, "q", -1, -1);
	check(error == 0 && paint_underlines(view) == lines, "password: nothing composed");

	/* 7. A textarea takes it at its caret, and the composing ends when the focus moves, its text put into the value (q893). */
	tabs(view, 1);
	taken = target(view, caret);
	check(taken == 1, "textarea: a target");
	error = browser_view_compose(view, "\xe3\x81\xa6", 0, 3);
	check(error == 0 && paint_has(view, "\"\xe3\x81\xa6\"") && paint_underlines(view) == lines + 1, "textarea: composed");
	mark();
	tabs(view, -1);
	check(paint_underlines(view) == lines && heard("input notes \xe3\x81\xa6\n") && paint_has(view, "\"\xe3\x81\xa6\""), "focus: the composing ends in the value");
	tabs(view, 1);
	mark();
	error = browser_view_commit_text(view, "\xe6\x89\x8b", 0U, 0U);
	check(error == 0 && heard("input notes") && paint_has(view, "\"\xe3\x81\xa6\xe6\x89\x8b\""), "textarea: committed, with input");

	/* The view goes, and the summary. */
	browser_view_destroy(view);
	printf("host-browser-ime: %d checks, %d failed\n", test_checks, test_failures);
	if (test_failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Counts a check, and prints it when it failed (or every one with -v). */
static void
check(
	int condition,
	const char *what)
{
	/* The count, and the line. */
	test_checks++;
	if (!condition) {
		test_failures++;
		printf("FAIL %s\n", what);
	} else if (test_state.verbose) {
		printf("ok %s\n", what);
	}
}

/* Forgets the console so far. */
static void
mark(void)
{
	/* Empty from here. */
	test_state.console_length = 0;
	test_state.console[0] = '\0';
}

/* Tells whether the console since the last mark has a text. */
static int
heard(
	const char *text)
{
	const char *found;

	/* The text anywhere in it. */
	found = strstr(test_state.console, text);
	if (found == NULL)
		return 0;

	/* Heard. */
	return 1;
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

	/* Down, then up. */
	error = browser_view_key(view, key, code, text, 1, 0, modifiers);
	if (error != 0)
		printf("key %s: error %d\n", key, error);
	error = browser_view_key(view, key, code, text, 0, 0, modifiers);
	if (error != 0)
		printf("key %s up: error %d\n", key, error);
}

/* Presses Tab count times (Shift+Tab when count is negative). */
static void
tabs(
	struct browser_view *view,
	int count)
{
	uint32_t modifiers;

	/* Backward with Shift. */
	modifiers = 0U;
	if (count < 0) {
		modifiers = BROWSER_MOD_SHIFT;
		count = -count;
	}

	/* Each press. */
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

	/* The dump. */
	error = browser_view_dump(view, BROWSER_DUMP_PAINT, &dump, &length);
	if (error != 0)
		return 0;

	/* The text in it. */
	found = 0;
	if (strstr(dump, text) != NULL)
		found = 1;
	if (test_state.verbose && !found)
		printf("-- paint has no %s\n", text);
	free(dump);

	/* Whether it was found. */
	return found;
}

/* Counts the display list's rectangles a pixel tall and wider than five pixels (a composed text's underline is one). */
static int
paint_underlines(
	struct browser_view *view)
{
	const char *line;
	char *dump;
	size_t length;
	double x;
	double y;
	double width;
	double height;
	int count;
	int read;
	int error;

	/* The dump. */
	error = browser_view_dump(view, BROWSER_DUMP_PAINT, &dump, &length);
	if (error != 0)
		return -1;

	/* Each rectangle's line. */
	count = 0;
	for (line = dump; line != NULL && *line != '\0'; line = strchr(line, '\n')) {
		if (*line == '\n')
			line++;
		read = sscanf(line, "rect %lf %lf %lf %lf", &x, &y, &width, &height);
		if (read == 4 && height == 1.0 && width > 5.0)
			count++;
	}
	free(dump);

	/* The underlines. */
	return count;
}

/* Asks the view whether its focus takes an input method's text, after a paint (which places the caret). */
static int
target(
	struct browser_view *view,
	float *caret)
{
	int taken;

	/* The paint, then the answer. */
	(void)paint_has(view, "");
	taken = browser_view_text_target(view, caret);

	/* Whether it does. */
	return taken;
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
	struct test_state *state;
	size_t room;

	(void)view;
	(void)level;

	/* The line, cut to the room left, and its end. */
	state = context;
	if (state->verbose)
		printf("console: %.*s\n", (int)length, text);
	room = TEST_CONSOLE_MAX - state->console_length - 2U;
	if (length > room)
		length = room;
	memcpy(state->console + state->console_length, text, length);
	state->console_length += length;
	state->console[state->console_length] = '\n';
	state->console_length++;
	state->console[state->console_length] = '\0';
}
