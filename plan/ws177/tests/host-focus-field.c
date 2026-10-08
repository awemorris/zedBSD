/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws177-p017: the host test of browser_view_focus_field (libbrowser's
 * public <browser.h> alone, linked with the host's libbrowser.so): on a
 * page with an email field, a hidden and a disabled one-time-code field,
 * one whose autocomplete is "section-x ONE-TIME-CODE" and another after
 * it, the focus goes to the fourth (its focus event fires) and typed
 * digits reach it; a page with none answers ENOENT and keeps its focus.
 *
 *     host-focus-field
 *
 * Prints "PASS name" or "FAIL name [detail]" for each check; exits with 1
 * when one failed.
 */

#include <browser/browser.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* What the console said last. */
static char test_console[256];

/* The checks that failed. */
static int test_failures;

int main(void);
static void test_check(const char *name, int passed, const char *detail);
static void test_console_line(void *context, struct browser_view *view, int level, const char *text, size_t length);

/*
 * Runs the checks.
 */
int
main(void)
{
	struct browser_view_options options;
	struct browser_callbacks callbacks;
	struct browser_fonts fonts;
	struct browser_view *view;
	int error;

	/* The view, with the tree's fonts. */
	fonts.sans = "userland/desktop/fonts/Mahora-Regular.ttf";
	fonts.mono = "userland/desktop/fonts/JetBrainsMono-Regular.ttf";
	fonts.fallback = "userland/desktop/fonts/DroidSansFallbackFull.ttf";
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.console = test_console_line;
	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION;
	options.fonts = &fonts;
	options.callbacks = &callbacks;
	options.stack_base = __builtin_frame_address(0);
	options.fetch = BROWSER_FETCH_AT_ONCE;
	options.width = 400;
	options.height = 120;
	error = browser_view_create(&options, &view);
	if (error != 0) {
		printf("FAIL create [%d]\n", error);
		return 1;
	}

	/* The page with the fields, laid out. */
	error = browser_view_load(view, "plan/ws177/tests/pages/focus-field.html");
	if (error == 0)
		error = browser_view_settle(view, 0.0, BROWSER_SETTLE_LAYOUT);
	test_check("load", error == 0, "");
	(void)browser_view_focus(view, 1);

	/* The field: the drawn, enabled one with the token in any case, the first in order. */
	test_console[0] = '\0';
	error = browser_view_focus_field(view, "one-time-code");
	test_check("focus-field", error == 0 && strcmp(test_console, "focus=b") == 0, test_console);

	/* Digits typed reach it. */
	error = browser_view_key(view, "4", "Digit4", "4", 1, 0, 0U);
	if (error == 0)
		error = browser_view_key(view, "4", "Digit4", "4", 0, 0, 0U);
	if (error == 0)
		error = browser_view_key(view, "2", "Digit2", "2", 1, 0, 0U);
	if (error == 0)
		error = browser_view_key(view, "2", "Digit2", "2", 0, 0, 0U);
	test_check("typed", error == 0 && strcmp(test_console, "value=42") == 0, test_console);

	/* A page without one: ENOENT, and nothing is focused. */
	error = browser_view_load(view, "plan/ws177/tests/pages/no-field.html");
	if (error == 0)
		error = browser_view_settle(view, 0.0, BROWSER_SETTLE_LAYOUT);
	test_console[0] = '\0';
	if (error == 0)
		error = browser_view_focus_field(view, "one-time-code");
	test_check("no-field", error == ENOENT && test_console[0] == '\0', test_console);

	/* The outcome. */
	browser_view_destroy(view);
	if (test_failures != 0) {
		printf("host-focus-field: %d FAILED\n", test_failures);
		return 1;
	}
	printf("host-focus-field: PASS\n");
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
	printf("FAIL %s [%s]\n", name, detail);
	test_failures++;
}

/* Keeps what the page's console said last. */
static void
test_console_line(
	void *context,
	struct browser_view *view,
	int level,
	const char *text,
	size_t length)
{
	(void)context;
	(void)view;
	(void)level;

	/* The line, cut to the room. */
	if (length >= sizeof(test_console))
		length = sizeof(test_console) - 1U;
	memcpy(test_console, text, length);
	test_console[length] = '\0';
}
