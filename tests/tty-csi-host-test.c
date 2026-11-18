/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Runs the virtual console's escape-sequence reader on the host (ws228-p004).
 *
 * It checks the cursor position with a row and a column, the graphic
 * rendition and the attribute byte it makes, that the one-parameter
 * commands read as before, and that sequences the console does not know
 * are read to their end without showing up as text.
 *
 * Build and run from the top of the tree:
 *   cc -std=c99 -O2 -Wall -Wextra -Werror -Isrc src/kern/tty-csi.c \
 *      tests/tty-csi-host-test.c -o build/tmp/tty-csi-host-test
 *   build/tmp/tty-csi-host-test
 */

#include <stdio.h>
#include <string.h>

#include "kern/tty-csi.h"

/* The console grid the position checks clamp against. */
#define TEST_ROWS 25U
#define TEST_COLUMNS 80U

/* The display's attribute byte for plain text: light grey on black. */
#define TEST_ATTRIBUTE_PLAIN 0x07U

/*
 * The number of checks that failed.
 *
 * Every check adds to it on failure; main reports it at the end and exits
 * with failure when it is not zero.
 */
static unsigned test_failures;

static void test_expect(int condition, const char *what);
static enum tty_csi_result test_feed(struct tty_csi_parser *parser, const char *text);
static void test_plain_and_literal(void);
static void test_position(void);
static void test_one_parameter(void);
static void test_discarded(void);
static void test_rendition(void);
static void test_parameter_bounds(void);

/*
 * Runs every check and reports the outcome.
 */
int
main(
	void)
{
	/* Runs each group of checks. */
	test_plain_and_literal();
	test_position();
	test_one_parameter();
	test_discarded();
	test_rendition();
	test_parameter_bounds();

	/* Reports the failed checks. */
	if (test_failures != 0U) {
		printf("tty-csi-host-test: FAIL (%u)\n", test_failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("tty-csi-host-test: PASS\n");
	return 0;
}

/* Counts and names a check that did not hold. */
static void
test_expect(
	int condition,
	const char *what)
{
	/* A check that holds needs no report. */
	if (condition)
		return;

	/* Names the failed check. */
	printf("FAIL: %s\n", what);
	test_failures++;
}

/* Feeds a string to the reader and reports what its last byte meant. */
static enum tty_csi_result
test_feed(
	struct tty_csi_parser *parser,
	const char *text)
{
	enum tty_csi_result meaning;
	size_t index;
	size_t length;

	/* Feeds every byte in order, keeping the meaning of the last. */
	meaning = TTY_CSI_PLAIN;
	length = strlen(text);
	for (index = 0; index < length; index++)
		meaning = tty_csi_feed(parser, (unsigned char)text[index]);

	/* Reports the meaning of the last byte. */
	return meaning;
}

/* Checks text, an escape that opens a sequence, and an escape that does not. */
static void
test_plain_and_literal(
	void)
{
	struct tty_csi_parser parser;
	enum tty_csi_result meaning;

	/* Text is shown and an escape waits for what follows. */
	tty_csi_reset(&parser);
	meaning = tty_csi_feed(&parser, 'a');
	test_expect(meaning == TTY_CSI_PLAIN, "a text byte is plain");
	meaning = tty_csi_feed(&parser, 0x1bU);
	test_expect(meaning == TTY_CSI_PENDING, "an escape is pending");

	/* An escape followed by another letter is shown as it always was. */
	meaning = tty_csi_feed(&parser, 'c');
	test_expect(meaning == TTY_CSI_LITERAL, "ESC c is shown literally");
	test_expect(parser.state == TTY_CSI_GROUND, "ESC c returns to text");

	/* A reset in the middle of a sequence returns to text. */
	(void)test_feed(&parser, "\033[12");
	tty_csi_reset(&parser);
	meaning = tty_csi_feed(&parser, 'x');
	test_expect(meaning == TTY_CSI_PLAIN, "reset ends an open sequence");
}

/* Checks the cursor position with a row and a column. */
static void
test_position(
	void)
{
	struct tty_csi_parser parser;
	enum tty_csi_result meaning;
	unsigned row;
	unsigned column;

	/* Row 5, column 10 is the zero-based cell 4, 9. */
	tty_csi_reset(&parser);
	meaning = test_feed(&parser, "\033[5;10H");
	test_expect(meaning == TTY_CSI_COMMAND, "ESC[5;10H is a command");
	test_expect(parser.command == 'H', "ESC[5;10H ends with H");
	test_expect(tty_csi_parameter_count(&parser) == 2U, "ESC[5;10H has two parameters");
	tty_csi_position(&parser, TEST_ROWS, TEST_COLUMNS, &row, &column);
	test_expect(row == 4U && column == 9U, "ESC[5;10H goes to 4,9");

	/* f is the same command as H. */
	meaning = test_feed(&parser, "\033[2;3f");
	test_expect(meaning == TTY_CSI_COMMAND && parser.command == 'f', "ESC[2;3f is a command");
	tty_csi_position(&parser, TEST_ROWS, TEST_COLUMNS, &row, &column);
	test_expect(row == 1U && column == 2U, "ESC[2;3f goes to 1,2");

	/* No parameter is the home cell. */
	(void)test_feed(&parser, "\033[H");
	tty_csi_position(&parser, TEST_ROWS, TEST_COLUMNS, &row, &column);
	test_expect(row == 0U && column == 0U, "ESC[H goes home");

	/* An empty row is the first row. */
	(void)test_feed(&parser, "\033[;5H");
	tty_csi_position(&parser, TEST_ROWS, TEST_COLUMNS, &row, &column);
	test_expect(row == 0U && column == 4U, "ESC[;5H goes to 0,4");

	/* A row alone keeps the first column. */
	(void)test_feed(&parser, "\033[7H");
	tty_csi_position(&parser, TEST_ROWS, TEST_COLUMNS, &row, &column);
	test_expect(row == 6U && column == 0U, "ESC[7H goes to 6,0");

	/* Zero means the first. */
	(void)test_feed(&parser, "\033[0;0H");
	tty_csi_position(&parser, TEST_ROWS, TEST_COLUMNS, &row, &column);
	test_expect(row == 0U && column == 0U, "ESC[0;0H goes home");

	/* A position past the grid stops at its last cell. */
	(void)test_feed(&parser, "\033[99;999H");
	tty_csi_position(&parser, TEST_ROWS, TEST_COLUMNS, &row, &column);
	test_expect(row == 24U && column == 79U, "ESC[99;999H stops at 24,79");

	/* The terminfo cup string for row 0, column 0 after %i. */
	(void)test_feed(&parser, "\033[1;1H");
	tty_csi_position(&parser, TEST_ROWS, TEST_COLUMNS, &row, &column);
	test_expect(row == 0U && column == 0U, "ESC[1;1H goes home");

	/* A sequence split between two writes is read as one. */
	(void)test_feed(&parser, "\033[1");
	meaning = test_feed(&parser, "2;3");
	test_expect(meaning == TTY_CSI_PENDING, "a split sequence waits");
	meaning = test_feed(&parser, "4H");
	test_expect(meaning == TTY_CSI_COMMAND, "a split sequence finishes");
	tty_csi_position(&parser, TEST_ROWS, TEST_COLUMNS, &row, &column);
	test_expect(row == 11U && column == 33U, "ESC[12;34H split goes to 11,33");
}

/* Checks that the one-parameter commands read as they did before. */
static void
test_one_parameter(
	void)
{
	struct tty_csi_parser parser;
	enum tty_csi_result meaning;

	/* A count is read and marked written. */
	tty_csi_reset(&parser);
	meaning = test_feed(&parser, "\033[3A");
	test_expect(meaning == TTY_CSI_COMMAND && parser.command == 'A', "ESC[3A is a command");
	test_expect(tty_csi_parameter(&parser, 0U, 1U) == 3U, "ESC[3A counts 3");
	test_expect(tty_csi_parameter_present(&parser, 0U), "ESC[3A has its parameter");

	/* A missing count takes the default and is not marked written. */
	(void)test_feed(&parser, "\033[C");
	test_expect(tty_csi_parameter(&parser, 0U, 1U) == 1U, "ESC[C counts 1");
	test_expect(!tty_csi_parameter_present(&parser, 0U), "ESC[C has no parameter");

	/* A written zero is kept as zero, as the erase commands need. */
	(void)test_feed(&parser, "\033[0K");
	test_expect(parser.command == 'K', "ESC[0K ends with K");
	test_expect(tty_csi_parameter(&parser, 0U, 1U) == 0U, "ESC[0K selects 0");
	test_expect(tty_csi_parameter_present(&parser, 0U), "ESC[0K has its parameter");

	/* The whole-screen erase selector. */
	(void)test_feed(&parser, "\033[2J");
	test_expect(parser.command == 'J', "ESC[2J ends with J");
	test_expect(tty_csi_parameter(&parser, 0U, 1U) == 2U, "ESC[2J selects 2");

	/* A control byte ends a sequence as it always did, and is a command nobody knows. */
	meaning = test_feed(&parser, "\033[\n");
	test_expect(meaning == TTY_CSI_COMMAND && parser.command == '\n', "ESC[ newline ends the sequence");
}

/* Checks that sequences the console does not know are read to their end and dropped. */
static void
test_discarded(
	void)
{
	struct tty_csi_parser parser;
	enum tty_csi_result meaning;

	/* A private marker: show and hide the cursor. */
	tty_csi_reset(&parser);
	meaning = test_feed(&parser, "\033[?25l");
	test_expect(meaning == TTY_CSI_DISCARDED, "ESC[?25l is dropped");
	meaning = tty_csi_feed(&parser, 'z');
	test_expect(meaning == TTY_CSI_PLAIN, "text follows a dropped sequence");

	/* A sub-parameter separator. */
	meaning = test_feed(&parser, "\033[4:3m");
	test_expect(meaning == TTY_CSI_DISCARDED, "ESC[4:3m is dropped");

	/* An intermediate byte. */
	meaning = test_feed(&parser, "\033[2 q");
	test_expect(meaning == TTY_CSI_DISCARDED, "ESC[2 SP q is dropped");

	/* A fresh sequence after a dropped one is read normally. */
	meaning = test_feed(&parser, "\033[1;1H");
	test_expect(meaning == TTY_CSI_COMMAND, "a sequence after a dropped one is a command");
}

/* Checks the graphic rendition and the attribute byte it makes. */
static void
test_rendition(
	void)
{
	struct tty_csi_parser parser;
	struct tty_csi_attribute attribute;
	uint8_t display;

	/* Plain text is light grey on black. */
	tty_csi_reset(&parser);
	tty_csi_attribute_reset(&attribute);
	display = tty_csi_attribute_byte(&attribute);
	test_expect(display == TEST_ATTRIBUTE_PLAIN, "plain is 0x07");

	/* Reverse swaps the colours. */
	(void)test_feed(&parser, "\033[7m");
	tty_csi_attribute_apply(&attribute, &parser);
	display = tty_csi_attribute_byte(&attribute);
	test_expect(display == 0x70U, "ESC[7m is 0x70");

	/* An empty rendition is a reset. */
	(void)test_feed(&parser, "\033[m");
	tty_csi_attribute_apply(&attribute, &parser);
	display = tty_csi_attribute_byte(&attribute);
	test_expect(display == TEST_ATTRIBUTE_PLAIN, "ESC[m resets");

	/* Bold brightens the text colour. */
	(void)test_feed(&parser, "\033[1m");
	tty_csi_attribute_apply(&attribute, &parser);
	display = tty_csi_attribute_byte(&attribute);
	test_expect(display == 0x0fU, "ESC[1m is 0x0f");

	/* Bold and reverse together brighten the swapped text colour only. */
	(void)test_feed(&parser, "\033[7m");
	tty_csi_attribute_apply(&attribute, &parser);
	display = tty_csi_attribute_byte(&attribute);
	test_expect(display == 0x78U, "ESC[1m ESC[7m is 0x78");

	/* 22 and 27 end bold and reverse. */
	(void)test_feed(&parser, "\033[22;27m");
	tty_csi_attribute_apply(&attribute, &parser);
	display = tty_csi_attribute_byte(&attribute);
	test_expect(display == TEST_ATTRIBUTE_PLAIN, "ESC[22;27m is plain");

	/* ANSI red text is the display's colour 4. */
	(void)test_feed(&parser, "\033[31m");
	tty_csi_attribute_apply(&attribute, &parser);
	display = tty_csi_attribute_byte(&attribute);
	test_expect(display == 0x04U, "ESC[31m is 0x04");

	/* ANSI blue behind text is the display's colour 1. */
	(void)test_feed(&parser, "\033[44m");
	tty_csi_attribute_apply(&attribute, &parser);
	display = tty_csi_attribute_byte(&attribute);
	test_expect(display == 0x14U, "ESC[31m ESC[44m is 0x14");

	/* 39 and 49 return the colours to their defaults. */
	(void)test_feed(&parser, "\033[39;49m");
	tty_csi_attribute_apply(&attribute, &parser);
	display = tty_csi_attribute_byte(&attribute);
	test_expect(display == TEST_ATTRIBUTE_PLAIN, "ESC[39;49m is plain");

	/* The terminfo setaf and setab strings for yellow on cyan. */
	(void)test_feed(&parser, "\033[33m");
	tty_csi_attribute_apply(&attribute, &parser);
	(void)test_feed(&parser, "\033[46m");
	tty_csi_attribute_apply(&attribute, &parser);
	display = tty_csi_attribute_byte(&attribute);
	test_expect(display == 0x36U, "yellow on cyan is 0x36");

	/* An empty first parameter resets before the second takes effect. */
	(void)test_feed(&parser, "\033[;7m");
	tty_csi_attribute_apply(&attribute, &parser);
	display = tty_csi_attribute_byte(&attribute);
	test_expect(display == 0x70U, "ESC[;7m is a reset then reverse");

	/* A direct colour's numbers are not read as renditions. */
	(void)test_feed(&parser, "\033[0;38;2;1;7;0m");
	tty_csi_attribute_apply(&attribute, &parser);
	display = tty_csi_attribute_byte(&attribute);
	test_expect(display == TEST_ATTRIBUTE_PLAIN, "ESC[38;2;1;7;0m changes nothing");

	/* An indexed colour's number is not read as a rendition, and what follows still is. */
	(void)test_feed(&parser, "\033[48;5;1;7m");
	tty_csi_attribute_apply(&attribute, &parser);
	display = tty_csi_attribute_byte(&attribute);
	test_expect(display == 0x70U, "ESC[48;5;1;7m is reverse only");

	/* A rendition the console cannot show is passed over. */
	(void)test_feed(&parser, "\033[0;4;5;1m");
	tty_csi_attribute_apply(&attribute, &parser);
	display = tty_csi_attribute_byte(&attribute);
	test_expect(display == 0x0fU, "ESC[0;4;5;1m is bold only");
}

/* Checks the bounds on the number and size of parameters. */
static void
test_parameter_bounds(
	void)
{
	struct tty_csi_parser parser;
	struct tty_csi_attribute attribute;
	enum tty_csi_result meaning;
	uint8_t display;
	unsigned value;

	/* A long number stops growing instead of wrapping around. */
	tty_csi_reset(&parser);
	meaning = test_feed(&parser, "\033[99999999999A");
	test_expect(meaning == TTY_CSI_COMMAND, "a long count is a command");
	value = tty_csi_parameter(&parser, 0U, 1U);
	test_expect(value >= 1000U && value < 10000U, "a long count stops below 10000");

	/* Parameters past the kept ones are read and dropped. */
	tty_csi_attribute_reset(&attribute);
	meaning = test_feed(&parser, "\033[1;0;0;0;0;0;0;0;0;0;0;0;0;0;0;1;7;7;7m");
	test_expect(meaning == TTY_CSI_COMMAND, "a long rendition is a command");
	test_expect(tty_csi_parameter_count(&parser) == TTY_CSI_PARAMETER_MAX, "the count stops at the kept parameters");
	tty_csi_attribute_apply(&attribute, &parser);
	display = tty_csi_attribute_byte(&attribute);
	test_expect(display == 0x0fU, "parameters past sixteen are dropped");
}
