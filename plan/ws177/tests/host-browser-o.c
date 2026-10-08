/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws177-p019: the host test of the browser's text input and forms
 * (libbrowser's public <browser/browser.h> alone): what an input method reads
 * around the caret (the value, the caret, the purpose by type and by
 * inputmode, a textarea's multiline hint, a long value cut around the
 * caret), the chosen part of a composed text under a thicker line, a
 * click that puts the composed text into the value (input fires), a
 * click in another field that puts it into the first field's value before
 * its blur (q893), a script's value that ends the composing, a textarea's
 * value from script, and the text input's session, which changes with
 * the focus and with each composing the page ends itself.
 *
 *     host-browser-o PAGE SANS MONO FALLBACK
 *
 * Prints "PASS name" or "FAIL name [detail]" for each check; exits with 1
 * when one failed.
 */

#include <browser/browser.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most console text kept. */
#define TEST_CONSOLE_MAX	8192U

/* What the console said since the last mark. */
static char test_console[TEST_CONSOLE_MAX];
static size_t test_console_length;

/* The checks that failed. */
static int test_failures;

int main(int argc, char **argv);
static void test_check(const char *name, int passed, const char *detail);
static void test_console_line(void *context, struct browser_view *view, int level, const char *text, size_t length);
static int test_underlines(struct browser_view *view, double height_wanted);

/*
 * Runs the checks.
 */
int
main(
	int argc,
	char **argv)
{
	struct browser_view_options options;
	struct browser_callbacks callbacks;
	struct browser_fonts fonts;
	struct browser_view *view;
	static char text[32768];
	static char longer[6000];
	float caret[4];
	float mail[4];
	uint64_t session;
	uint64_t composed;
	const char *input_at;
	const char *blur_at;
	size_t cursor;
	unsigned hints;
	int purpose;
	int lines;
	int known;
	int error;

	/* The page and the fonts. */
	if (argc != 5) {
		fprintf(stderr, "usage: host-browser-o PAGE SANS MONO FALLBACK\n");
		return 2;
	}
	fonts.sans = argv[2];
	fonts.mono = argv[3];
	fonts.fallback = argv[4];
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.console = test_console_line;
	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION;
	options.fonts = &fonts;
	options.callbacks = &callbacks;
	options.stack_base = __builtin_frame_address(0);
	options.fetch = BROWSER_FETCH_AT_ONCE;
	options.width = 600;
	options.height = 400;
	error = browser_view_create(&options, &view);
	if (error != 0)
		return 2;

	/* The page, laid out; its script read and set the textarea's value. */
	error = browser_view_load(view, argv[1]);
	if (error == 0)
		error = browser_view_settle(view, 0.0, BROWSER_SETTLE_LAYOUT);
	(void)browser_view_focus(view, 1);
	test_check("load", error == 0, "");
	test_check("textarea-value", strstr(test_console, "notes=hello\n") != NULL && strstr(test_console, "notes-set=3 true\n") != NULL, test_console);

	/* An email field: its purpose, and where its caret is (for a click in it later). */
	session = browser_view_text_session(view);
	(void)browser_view_focus_field(view, "email");
	known = browser_view_text_context(view, text, sizeof(text), &cursor, &purpose, &hints);
	test_check("purpose-email", known == 1 && purpose == BROWSER_TEXT_PURPOSE_EMAIL && hints == 0U && text[0] == '\0', text);
	test_check("session-focus", browser_view_text_session(view) != session, "");
	(void)browser_view_text_target(view, mail);

	/* A numeric inputmode wins over the type. */
	(void)browser_view_focus_field(view, "one-time-code");
	known = browser_view_text_context(view, text, sizeof(text), &cursor, &purpose, &hints);
	test_check("purpose-inputmode", known == 1 && purpose == BROWSER_TEXT_PURPOSE_DIGITS, "");

	/* A textarea: its text (set by the script), the caret at its end, several lines. */
	(void)browser_view_focus_field(view, "street-address");
	known = browser_view_text_context(view, text, sizeof(text), &cursor, &purpose, &hints);
	test_check("textarea-context", known == 1 && strcmp(text, "x\ny") == 0 && cursor == 3U && (hints & BROWSER_TEXT_HINT_MULTILINE) != 0U, text);

	/* A plain field: what was committed, and the caret after it. */
	(void)browser_view_focus_field(view, "name");
	(void)browser_view_commit_text(view, "ab\xe6\x97\xa5", 0U, 0U);
	known = browser_view_text_context(view, text, sizeof(text), &cursor, &purpose, &hints);
	test_check("context-value", known == 1 && strcmp(text, "ab\xe6\x97\xa5") == 0 && cursor == 5U && purpose == BROWSER_TEXT_PURPOSE_NORMAL, text);

	/* A value longer than the room: the part around the caret, at characters' starts. */
	memset(longer, 'a', sizeof(longer) - 1U);
	longer[sizeof(longer) - 1U] = '\0';
	(void)browser_view_commit_text(view, longer, 0U, 0U);
	known = browser_view_text_context(view, text, 1000U, &cursor, &purpose, &hints);
	test_check("context-cut", known == 1 && strlen(text) == 999U && cursor == 999U, "");
	(void)browser_view_commit_text(view, "", 5999U, 0U);
	lines = test_underlines(view, 1.0);

	/* A composed text with a chosen part (bytes 3 to 6 of あいう): a thin line under all, a thick one under the part. */
	session = browser_view_text_session(view);
	(void)browser_view_compose(view, "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86", 3, 6);
	test_check("clause-thin", test_underlines(view, 1.0) == lines + 1, "");
	test_check("clause-thick", test_underlines(view, 3.0) == 1, "");

	/* Without a chosen part, no thick line. */
	(void)browser_view_compose(view, "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86", 9, 9);
	test_check("clause-none", test_underlines(view, 3.0) == 0, "");

	/* Composing alone keeps the session. */
	composed = browser_view_text_session(view);
	test_check("session-compose", composed == session, "");

	/* A click in the field puts the composed text into the value, which input says, and the session changes. */
	test_console_length = 0;
	test_console[0] = '\0';
	(void)browser_view_text_target(view, caret);
	error = browser_view_pointer_button(view, caret[0], caret[1] + caret[3] / 2.0f, BROWSER_BUTTON_PRIMARY, 1, 0U);
	if (error == 0)
		error = browser_view_pointer_button(view, caret[0], caret[1] + caret[3] / 2.0f, BROWSER_BUTTON_PRIMARY, 0, 0U);
	test_check("click-commits", error == 0 && strstr(test_console, "input name ab\xe6\x97\xa5\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86\n") != NULL &&
	    test_underlines(view, 1.0) == lines, test_console);
	test_check("session-click", browser_view_text_session(view) != composed, "");

	/*
	 * A click in another field while composing (q893): the composed text
	 * goes into the first field's value (input fires) before its blur, the
	 * other field has the focus with nothing composed, and the session
	 * changes.
	 */
	(void)browser_view_compose(view, "\xe3\x81\x8b\xe3\x81\x8d", -1, -1);
	composed = browser_view_text_session(view);
	test_console_length = 0;
	test_console[0] = '\0';
	error = browser_view_pointer_button(view, mail[0] + 4.0f, mail[1] + mail[3] / 2.0f, BROWSER_BUTTON_PRIMARY, 1, 0U);
	if (error == 0)
		error = browser_view_pointer_button(view, mail[0] + 4.0f, mail[1] + mail[3] / 2.0f, BROWSER_BUTTON_PRIMARY, 0, 0U);
	input_at = strstr(test_console, "input name ab\xe6\x97\xa5\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86\xe3\x81\x8b\xe3\x81\x8d\n");
	blur_at = strstr(test_console, "blur name ab\xe6\x97\xa5\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86\xe3\x81\x8b\xe3\x81\x8d\n");
	test_check("other-field-commits", error == 0 && input_at != NULL && blur_at != NULL && input_at < blur_at, test_console);
	known = browser_view_text_context(view, text, sizeof(text), &cursor, &purpose, &hints);
	test_check("other-field-focus", known == 1 && purpose == BROWSER_TEXT_PURPOSE_EMAIL && text[0] == '\0' && test_underlines(view, 1.0) == lines, text);
	test_check("session-other-field", browser_view_text_session(view) != composed, "");

	/* A script's value ends what was composed, and the session changes. */
	(void)browser_view_focus_field(view, "nickname");
	(void)browser_view_compose(view, "x", -1, -1);
	composed = browser_view_text_session(view);
	error = browser_view_settle(view, 300.0, BROWSER_SETTLE_LAYOUT);
	known = browser_view_text_context(view, text, sizeof(text), &cursor, &purpose, &hints);
	test_check("script-ends-compose", error == 0 && known == 1 && strcmp(text, "set") == 0 && test_underlines(view, 1.0) == lines, text);
	test_check("session-script", browser_view_text_session(view) != composed, "");

	/* The outcome. */
	browser_view_destroy(view);
	if (test_failures != 0) {
		printf("host-browser-o: %d FAILED\n", test_failures);
		return 1;
	}
	printf("host-browser-o: PASS\n");
	return 0;
}

/* Prints a check's outcome. */
static void
test_check(
	const char *name,
	int passed,
	const char *detail)
{
	/* Passed. */
	if (passed) {
		printf("PASS %s\n", name);
		return;
	}

	/* Failed, with what was seen. */
	printf("FAIL %s [%.300s]\n", name, detail);
	test_failures++;
}

/* Keeps a console line. */
static void
test_console_line(
	void *context,
	struct browser_view *view,
	int level,
	const char *text,
	size_t length)
{
	size_t room;

	(void)context;
	(void)view;
	(void)level;

	/* The line and its end, cut to the room left. */
	room = TEST_CONSOLE_MAX - test_console_length - 2U;
	if (length > room)
		length = room;
	memcpy(test_console + test_console_length, text, length);
	test_console_length += length;
	test_console[test_console_length] = '\n';
	test_console_length++;
	test_console[test_console_length] = '\0';
}

/* Counts the display list's rectangles of a height and wider than five pixels (a composed text's lines). */
static int
test_underlines(
	struct browser_view *view,
	double height_wanted)
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
		if (read == 4 && height == height_wanted && width > 5.0)
			count++;
	}
	free(dump);

	/* The lines. */
	return count;
}
