/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the PHONE lines (ws197-p003, plan/ws197/phase003/
 * phase.md sections 4.3 and 9.1), built with the host's compiler under
 * ASan and UBSan.
 *
 *   arguments  words, quoted strings with \" \\ and \xHH, spaces, the
 *              line's end; each malformed argument (no '=', an unended
 *              string, a bad escape, a NUL byte, too long)
 *   length     PHONE SEND's length: in range, missing, twice, 0, 8193,
 *              not digits, another request
 *   input      a client's bytes fed in pieces: lines become requests; a
 *              PHONE SEND line's text is read whole (from the same piece
 *              as its line, or later) and never read as lines, even when
 *              the text holds a newline and a request; the line right
 *              after the text (no newline ends a text) is a request again; a request that closes the client
 *              stops the reading; a malformed PHONE SEND line and a line
 *              too long are reported
 *
 *   plan/ws197/tests/bt-phone-host-test.sh
 */

#include "userland/base/bluetoothd/phoneio.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* How many requests and texts the test keeps. */
#define TEST_KEPT_MAX		8U

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/*
 * What the daemon's hooks saw: the request lines, the PHONE SEND lines
 * and their texts, and which request line closes the client (none: -1).
 */
struct seen {
	unsigned lines;
	char line[TEST_KEPT_MAX][BTD_PHONEIO_LINE_MAX];
	unsigned texts;
	char send_line[TEST_KEPT_MAX][BTD_PHONEIO_LINE_MAX];
	size_t text_length[TEST_KEPT_MAX];
	char text[TEST_KEPT_MAX][BTD_PHONE_SEND_MAX + 1U];
	int close_at;
};

/* What the hooks saw, static for its size. */
static struct seen seen;

static void check(int condition, const char *what);
static int argument(const char **cursor, const char *key, const char *value);
static int hook_line(void *context, char *line);
static void hook_text(void *context, const char *line, const uint8_t *text, size_t length);
static int feed(struct btd_phoneio_input *input, const char *bytes, size_t length);
static void test_arguments(void);
static void test_length(void);
static void test_input(void);

/*
 * Runs every part and reports the checks.
 */
int
main(void)
{
	/* Each part. */
	test_arguments();
	test_length();
	test_input();

	/* The count of what failed. */
	printf("bt-phoneio-host-test: %u checks, %u failed\n", checks, failures);
	if (failures != 0U)
		return 1;

	/* Succeeded: every check held. */
	return 0;
}

/* Counts a check and reports the one that failed. */
static void
check(
	int condition,
	const char *what)
{
	/* Counted. */
	checks++;
	if (condition)
		return;

	/* Reported. */
	failures++;
	printf("FAIL: %s\n", what);
}

/* Tells whether the next argument at *cursor is the key and value given. */
static int
argument(
	const char **cursor,
	const char *key,
	const char *value)
{
	char read_key[BTD_PHONEIO_KEY_MAX];
	char read_value[BTD_PHONEIO_VALUE_MAX];
	int same;
	int got;

	/* The argument. */
	got = btd_phoneio_next(cursor, read_key, sizeof(read_key), read_value, sizeof(read_value));
	if (got != BTD_PHONEIO_ARGUMENT)
		return 0;

	/* Its key. */
	same = strcmp(read_key, key);
	if (same != 0)
		return 0;

	/* Its value. */
	same = strcmp(read_value, value);
	if (same != 0)
		return 0;

	/* As given. */
	return 1;
}

/* The daemon's line hook: kept, and the client closes at close_at. */
static int
hook_line(
	void *context,
	char *line)
{
	(void)context;

	/* Kept. */
	if (seen.lines < TEST_KEPT_MAX)
		(void)snprintf(seen.line[seen.lines], sizeof(seen.line[0]), "%s", line);
	seen.lines++;

	/* The request that closes the client. */
	if (seen.close_at >= 0 && seen.lines == (unsigned)seen.close_at + 1U)
		return 1;

	/* Read on. */
	return 0;
}

/* The daemon's text hook: the line and the text kept. */
static void
hook_text(
	void *context,
	const char *line,
	const uint8_t *text,
	size_t length)
{
	(void)context;

	/* Kept. */
	if (seen.texts < TEST_KEPT_MAX) {
		(void)snprintf(seen.send_line[seen.texts], sizeof(seen.send_line[0]), "%s", line);
		memcpy(seen.text[seen.texts], text, length);
		seen.text[seen.texts][length] = '\0';
		seen.text_length[seen.texts] = length;
	}

	/* Counted. */
	seen.texts++;
}

/* Feeds bytes as recv would, as far as each room takes them.  Returns the last result, or what stopped the reading. */
static int
feed(
	struct btd_phoneio_input *input,
	const char *bytes,
	size_t length)
{
	struct btd_phoneio_events events;
	uint8_t *room;
	size_t size;
	size_t taken;
	int error;

	/* The hooks. */
	events.context = NULL;
	events.line = hook_line;
	events.text = hook_text;

	/* Each piece the room takes. */
	error = 0;
	while (length != 0U) {
		btd_phoneio_input_room(input, &room, &size);
		if (size == 0U)
			return EMSGSIZE;
		taken = length;
		if (taken > size)
			taken = size;
		memcpy(room, bytes, taken);
		bytes += taken;
		length -= taken;
		error = btd_phoneio_input_got(input, taken, &events);
		if (error != 0)
			return error;
	}

	/* Succeeded. */
	return error;
}

/* The arguments of a request's line. */
static void
test_arguments(void)
{
	char key[BTD_PHONEIO_KEY_MAX];
	char value[BTD_PHONEIO_VALUE_MAX];
	const char *cursor;
	int got;

	/* Words and strings, with every escape. */
	cursor = "since=1700000000  to=\"+81 90\\\"x\\\\y\\x41\" count=32";
	check(argument(&cursor, "since", "1700000000"), "arguments: a word");
	check(argument(&cursor, "to", "+81 90\"x\\yA"), "arguments: a string with escapes");
	check(argument(&cursor, "count", "32"), "arguments: a word at the end");
	got = btd_phoneio_next(&cursor, key, sizeof(key), value, sizeof(value));
	check(got == BTD_PHONEIO_END, "arguments: the line's end");

	/* An empty string. */
	cursor = "cursor=\"\"";
	check(argument(&cursor, "cursor", ""), "arguments: an empty string");

	/* Each malformed argument. */
	cursor = "since";
	got = btd_phoneio_next(&cursor, key, sizeof(key), value, sizeof(value));
	check(got == BTD_PHONEIO_MALFORMED, "arguments: no '='");
	cursor = "to=\"abc";
	got = btd_phoneio_next(&cursor, key, sizeof(key), value, sizeof(value));
	check(got == BTD_PHONEIO_MALFORMED, "arguments: an unended string");
	cursor = "to=\"a\\qb\"";
	got = btd_phoneio_next(&cursor, key, sizeof(key), value, sizeof(value));
	check(got == BTD_PHONEIO_MALFORMED, "arguments: an unknown escape");
	cursor = "to=\"a\\x4\"";
	got = btd_phoneio_next(&cursor, key, sizeof(key), value, sizeof(value));
	check(got == BTD_PHONEIO_MALFORMED, "arguments: a short hex escape");
	cursor = "to=\"a\\x00\"";
	got = btd_phoneio_next(&cursor, key, sizeof(key), value, sizeof(value));
	check(got == BTD_PHONEIO_MALFORMED, "arguments: a NUL byte");
	cursor = "to=\"ab\"c";
	got = btd_phoneio_next(&cursor, key, sizeof(key), value, sizeof(value));
	check(got == BTD_PHONEIO_MALFORMED, "arguments: text after a closing quote");
	cursor = "averyveryverylongkey=1";
	got = btd_phoneio_next(&cursor, key, sizeof(key), value, sizeof(value));
	check(got == BTD_PHONEIO_MALFORMED, "arguments: a key too long");
	cursor = "to=abcdefgh";
	got = btd_phoneio_next(&cursor, key, sizeof(key), value, 8U);
	check(got == BTD_PHONEIO_MALFORMED, "arguments: a value too long");
}

/* PHONE SEND's length. */
static void
test_length(void)
{
	size_t length;
	int error;

	/* In range. */
	error = btd_phoneio_send_length("PHONE SEND to=\"+8190\" length=5", &length);
	check(error == 0 && length == 5U, "length: 5");
	error = btd_phoneio_send_length("PHONE SEND length=8192 to=1", &length);
	check(error == 0 && length == 8192U, "length: 8192, before to");

	/* Another request. */
	error = btd_phoneio_send_length("PHONE PAGE messages since=0 count=1", &length);
	check(error == ENOENT, "length: another request");
	error = btd_phoneio_send_length("SHOW", &length);
	check(error == ENOENT, "length: not a PHONE request");

	/* Malformed. */
	error = btd_phoneio_send_length("PHONE SEND to=1", &length);
	check(error == EINVAL, "length: missing");
	error = btd_phoneio_send_length("PHONE SEND length=0", &length);
	check(error == EINVAL, "length: 0");
	error = btd_phoneio_send_length("PHONE SEND length=8193", &length);
	check(error == EINVAL, "length: 8193");
	error = btd_phoneio_send_length("PHONE SEND length=12a", &length);
	check(error == EINVAL, "length: not digits");
	error = btd_phoneio_send_length("PHONE SEND length=5 length=5", &length);
	check(error == EINVAL, "length: twice");
	error = btd_phoneio_send_length("PHONE SEND to=\"x length=5", &length);
	check(error == EINVAL, "length: a malformed argument");
}

/* A client's bytes in pieces. */
static void
test_input(void)
{
	static const char stream[] =
		"SHOW\nPHONE SEND to=\"+8190\" length=16\nhi\nSHOW\nFORGET xSTATUS\n";
	static struct btd_phoneio_input input;
	static char long_line[600];
	unsigned piece;
	size_t at;
	int error;

	/* All at once: SHOW, the text (holding a newline and a request), STATUS. */
	memset(&seen, 0, sizeof(seen));
	seen.close_at = -1;
	btd_phoneio_input_init(&input);
	error = feed(&input, stream, sizeof(stream) - 1U);
	check(error == 0, "input: read");
	check(seen.lines == 2U && strcmp(seen.line[0], "SHOW") == 0 && strcmp(seen.line[1], "STATUS") == 0, "input: the requests around the text");
	check(seen.texts == 1U && seen.text_length[0] == 16U && memcmp(seen.text[0], "hi\nSHOW\nFORGET x", 16U) == 0, "input: the text whole, never read as lines");
	check(strcmp(seen.send_line[0], "PHONE SEND to=\"+8190\" length=16") == 0, "input: the text's line");
	btd_phoneio_input_clear(&input);

	/* One byte at a time: the same. */
	memset(&seen, 0, sizeof(seen));
	seen.close_at = -1;
	btd_phoneio_input_init(&input);
	error = 0;
	for (at = 0U; at < sizeof(stream) - 1U && error == 0; at++)
		error = feed(&input, stream + at, 1U);
	check(error == 0 && seen.lines == 2U && seen.texts == 1U && memcmp(seen.text[0], "hi\nSHOW\nFORGET x", 16U) == 0, "input: one byte at a time");
	btd_phoneio_input_clear(&input);

	/* In pieces of 7. */
	memset(&seen, 0, sizeof(seen));
	seen.close_at = -1;
	btd_phoneio_input_init(&input);
	error = 0;
	for (piece = 0U; piece * 7U < sizeof(stream) - 1U && error == 0; piece++) {
		at = sizeof(stream) - 1U - piece * 7U;
		if (at > 7U)
			at = 7U;
		error = feed(&input, stream + piece * 7U, at);
	}

	/* The same. */
	check(error == 0 && seen.lines == 2U && seen.texts == 1U, "input: pieces of 7");
	btd_phoneio_input_clear(&input);

	/* A request that closes the client: nothing after it is read. */
	memset(&seen, 0, sizeof(seen));
	seen.close_at = 0;
	btd_phoneio_input_init(&input);
	error = feed(&input, "SHOW\nSTATUS\n", 12U);
	check(error == ECANCELED && seen.lines == 1U, "input: a request that closes the client stops the reading");
	btd_phoneio_input_clear(&input);

	/* A malformed PHONE SEND line. */
	memset(&seen, 0, sizeof(seen));
	seen.close_at = -1;
	btd_phoneio_input_init(&input);
	error = feed(&input, "PHONE SEND to=1\nSHOW\n", 21U);
	check(error == EINVAL && seen.lines == 0U, "input: a malformed PHONE SEND line");
	btd_phoneio_input_clear(&input);

	/* A line too long. */
	memset(&seen, 0, sizeof(seen));
	seen.close_at = -1;
	btd_phoneio_input_init(&input);
	memset(long_line, 'x', sizeof(long_line));
	error = feed(&input, long_line, sizeof(long_line));
	check(error == EMSGSIZE, "input: a line too long");
	btd_phoneio_input_clear(&input);

	/* A text cleared while it is read (the client went). */
	memset(&seen, 0, sizeof(seen));
	seen.close_at = -1;
	btd_phoneio_input_init(&input);
	error = feed(&input, "PHONE SEND to=1 length=100\nabc", 30U);
	check(error == 0 && input.text != NULL && input.text_used == 3U, "input: a text being read");
	btd_phoneio_input_clear(&input);
	check(input.text == NULL && seen.texts == 0U, "input: cleared, no request");
}
