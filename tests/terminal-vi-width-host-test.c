/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host unit test checking that terminal's grid and vi's line layout
 * count the same display widths for combining marks, CJK fullwidth
 * characters, and East Asian Ambiguous characters.
 *
 * Build and run from the top of the tree:
 *   mkdir -p build/tmp/vi-inc
 *   cp include/libc/terminfo.h build/tmp/vi-inc/terminfo.h
 *   cc -std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
 *      -fsanitize=address,undefined -fno-sanitize-recover=all \
 *      -Ibuild/tmp/vi-inc -Iuserland/desktop/terminal \
 *      -Iuserland/desktop/include -I. \
 *      tests/terminal-vi-width-host-test.c \
 *      userland/desktop/terminal/screen.c \
 *      userland/desktop/terminal/width.c \
 *      userland/desktop/terminal/search.c \
 *      userland/base/vi/screen.c \
 *      -no-pie -Wl,--unresolved-symbols=ignore-all \
 *      -o build/tmp/terminal-vi-width-host-test
 *   build/tmp/terminal-vi-width-host-test
 */

#include "userland/desktop/terminal/terminal.h"
#include "userland/base/vi/vi.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned test_checks;
static unsigned test_failures;

static void test_check(const char *what, int passed);
static unsigned terminal_column_after(const char *text, int ambiguous_wide);
static void test_combining_marks(void);
static void test_cjk_wide(void);
static void test_ambiguous_width(void);
static void test_last_column_combining(void);

/*
 * Runs every width-consistency check and reports the summary.
 */
int
main(
	void)
{
	/* Matches vi's UTF-8 locale setup for wcwidth. */
	setlocale(LC_CTYPE, "C.UTF-8");

	/* Checks combining marks, CJK wide characters, and Ambiguous width. */
	test_combining_marks();
	test_cjk_wide();
	test_ambiguous_width();
	test_last_column_combining();

	/* Summarizes the run. */
	printf("checks %u, failures %u\n", test_checks, test_failures);
	if (test_failures != 0U) {
		printf("terminal-vi-width-host-test: FAIL\n");
		return 1;
	}
	printf("terminal-vi-width-host-test: PASS\n");
	return 0;
}

/* Records one check's result and prints a failure line when it fails. */
static void
test_check(
	const char *what,
	int passed)
{
	test_checks++;
	if (!passed) {
		test_failures++;
		printf("FAIL: %s\n", what);
	}
}

/* Writes text to a fresh 80x24 terminal screen and returns the resulting cursor column. */
static unsigned
terminal_column_after(
	const char *text,
	int ambiguous_wide)
{
	static struct terminal_screen screen;

	/* Initializes a fresh grid with the requested Ambiguous-width mode. */
	terminal_screen_init(&screen, 80U, 24U);
	terminal_screen_set_ambiguous_wide(&screen, ambiguous_wide);

	/* Feeds the UTF-8 bytes as the shell would write them. */
	terminal_screen_write(&screen, (const unsigned char *)text, strlen(text));
	return screen.cursor_column;
}

/* Checks that combining marks take 0 columns in both terminal and vi. */
static void
test_combining_marks(
	void)
{
	const char *accented;
	const char *chained;
	const char *variation;
	unsigned term_col;
	size_t vi_col;

	/* Clears any inherited Ambiguous-width override. */
	unsetenv("ZEDBSD_AMBIGUOUS_WIDE");

	/* Base letter + U+0301 COMBINING ACUTE ACCENT. */
	accented = "e\xcc\x81x";
	term_col = terminal_column_after(accented, 0);
	vi_col = vi_text_column(accented, strlen(accented), strlen(accented));
	test_check("combining U+0301: terminal column is 2", term_col == 2U);
	test_check("combining U+0301: vi column matches terminal", vi_col == (size_t)term_col);
	test_check("combining U+0301: vi_text_offset_at_column(1) points to x",
	    vi_text_offset_at_column(accented, strlen(accented), 1U) == 3U);

	/* Base letter + U+0301 + U+20D0 COMBINING LEFT HARPOON ABOVE. */
	chained = "a\xcc\x81\xe2\x83\x90y";
	term_col = terminal_column_after(chained, 0);
	vi_col = vi_text_column(chained, strlen(chained), strlen(chained));
	test_check("chained combining marks: terminal column is 2", term_col == 2U);
	test_check("chained combining marks: vi column matches terminal", vi_col == (size_t)term_col);

	/* Base character + U+FE0F VARIATION SELECTOR-16. */
	variation = "x\xef\xb8\x8fz";
	term_col = terminal_column_after(variation, 0);
	vi_col = vi_text_column(variation, strlen(variation), strlen(variation));
	test_check("variation selector U+FE0F: terminal column is 2", term_col == 2U);
	test_check("variation selector U+FE0F: vi column matches terminal", vi_col == (size_t)term_col);

	/* Search bar column ignores combining marks too. */
	test_check("search bar column ignores combining marks",
	    terminal_search_bar_column(accented, strlen(accented), 0) == 8U);
}

/* Checks that CJK fullwidth characters take 2 columns in both terminal and vi. */
static void
test_cjk_wide(
	void)
{
	const char *japanese;
	unsigned term_col;
	size_t vi_col;

	unsetenv("ZEDBSD_AMBIGUOUS_WIDE");

	/* "あいう" (3 fullwidth characters = 6 columns) + "a" = 7 columns. */
	japanese = "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86" "a";
	term_col = terminal_column_after(japanese, 0);
	vi_col = vi_text_column(japanese, strlen(japanese), strlen(japanese));
	test_check("CJK fullwidth: terminal column is 7", term_col == 7U);
	test_check("CJK fullwidth: vi column matches terminal", vi_col == (size_t)term_col);
	test_check("CJK fullwidth: vi_text_offset_at_column(2) points to second char",
	    vi_text_offset_at_column(japanese, strlen(japanese), 2U) == 3U);
}

/* Checks that East Asian Ambiguous characters match between terminal and vi in both modes. */
static void
test_ambiguous_width(
	void)
{
	const char *ambiguous;
	unsigned term_col;
	size_t vi_col;

	/* U+25CB WHITE CIRCLE, U+2500 BOX DRAWINGS LIGHT HORIZONTAL, U+03B1 GREEK SMALL LETTER ALPHA. */
	ambiguous = "\xe2\x97\x8b\xe2\x94\x80\xce\xb1" "z";

	/* Narrow mode (default). */
	unsetenv("ZEDBSD_AMBIGUOUS_WIDE");
	term_col = terminal_column_after(ambiguous, 0);
	vi_col = vi_text_column(ambiguous, strlen(ambiguous), strlen(ambiguous));
	test_check("ambiguous narrow: terminal column is 4", term_col == 4U);
	test_check("ambiguous narrow: vi column matches terminal", vi_col == (size_t)term_col);

	/* Wide mode (ambiguous_wide = 1 / ZEDBSD_AMBIGUOUS_WIDE=1). */
	setenv("ZEDBSD_AMBIGUOUS_WIDE", "1", 1);
	term_col = terminal_column_after(ambiguous, 1);
	vi_col = vi_text_column(ambiguous, strlen(ambiguous), strlen(ambiguous));
	test_check("ambiguous wide: terminal column is 7", term_col == 7U);
	test_check("ambiguous wide: vi column matches terminal", vi_col == (size_t)term_col);
	test_check("ambiguous wide: vi_text_offset_at_column(2) points to second char",
	    vi_text_offset_at_column(ambiguous, strlen(ambiguous), 2U) == 3U);

	unsetenv("ZEDBSD_AMBIGUOUS_WIDE");
}

/* Checks that a combining mark after a character in the last column does not wrap. */
static void
test_last_column_combining(
	void)
{
	static struct terminal_screen screen;
	unsigned i;

	/* Fills columns 0..9 of a 10-column grid, leaving wrap_pending set. */
	terminal_screen_init(&screen, 10U, 4U);
	for (i = 0U; i < 10U; i++)
		terminal_screen_write(&screen, (const unsigned char *)"a", 1U);
	test_check("last column sets wrap_pending at column 9, row 0",
	    screen.cursor_column == 9U && screen.cursor_row == 0U && screen.wrap_pending == 1);

	/* A combining mark attaches without wrapping to row 1. */
	terminal_screen_write(&screen, (const unsigned char *)"\xcc\x81", 2U);
	test_check("combining mark at last column does not wrap",
	    screen.cursor_column == 9U && screen.cursor_row == 0U && screen.wrap_pending == 1);
}
