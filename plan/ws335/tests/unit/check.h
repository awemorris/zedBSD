/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The checks of the unit tests of libcrypto-compat and libssl-compat.
 *
 * A test calls CHECK and CHECK_BYTES as often as it likes, then returns
 * check_finish("test-<name>") from main: it prints the last line run-
 * unit.sh reads ("test-<name>: PASS (<n> checks)" or "... FAIL (<m> of
 * <n> failed)") and returns the exit status.  A failed check prints its
 * file, line and expression (and for bytes, both values in hex, cut to
 * 64 bytes) to stdout.
 *
 * hex_decode turns a hex string of a test vector into bytes.
 *
 * The helpers are marked unused so that a test which does not call one
 * builds with -Werror.
 */

#ifndef WS335_TESTS_CHECK_H
#define WS335_TESTS_CHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The counts of one test program (static: each test is one translation unit). */
static unsigned long check_count;
static unsigned long check_failed;

/* Records one check of a condition. */
#define CHECK(condition) check_one((condition) != 0, __FILE__, __LINE__, #condition)

/* Records one check that size bytes at got equal those at expected. */
#define CHECK_BYTES(got, expected, size) check_bytes((const unsigned char *)(got), (const unsigned char *)(expected), (size), __FILE__, __LINE__, #got)

/* Counts a check and reports it when it failed; returns the condition. */
static int __attribute__((unused))
check_one(
	int good,
	const char *file,
	int line,
	const char *text)
{
	/* Counted either way. */
	check_count++;
	if (good)
		return 1;

	/* Reported. */
	check_failed++;
	printf("FAIL %s:%d: %s\n", file, line, text);
	return 0;
}

/* Prints up to 64 bytes in hex after a label. */
static void __attribute__((unused))
check_print_hex(
	const char *label,
	const unsigned char *bytes,
	size_t size)
{
	size_t index;

	/* At most 64 bytes, then a mark when cut. */
	printf("  %s ", label);
	for (index = 0; index < size && index < 64U; index++)
		printf("%02x", bytes[index]);
	if (size > 64U)
		printf("...");
	printf("\n");
}

/* Counts a comparison of two byte strings and reports both when they differ. */
static int __attribute__((unused))
check_bytes(
	const unsigned char *got,
	const unsigned char *expected,
	size_t size,
	const char *file,
	int line,
	const char *text)
{
	int same;

	/* The comparison. */
	same = size == 0U || memcmp(got, expected, size) == 0;
	if (!check_one(same, file, line, text)) {
		check_print_hex("got     ", got, size);
		check_print_hex("expected", expected, size);
	}
	return same;
}

/*
 * Decodes hex text into out (room bytes); returns the bytes written, or
 * (size_t)-1 for an odd length, a non-hex character or too little room.
 */
static size_t __attribute__((unused))
hex_decode(
	const char *text,
	unsigned char *out,
	size_t room)
{
	size_t length;
	size_t index;
	unsigned value;
	int digit;
	int half;

	/* Two characters per byte. */
	length = strlen(text);
	if (length % 2U != 0U || length / 2U > room)
		return (size_t)-1;

	/* Each byte. */
	for (index = 0; index < length / 2U; index++) {
		value = 0;
		for (half = 0; half < 2; half++) {
			digit = text[index * 2U + (size_t)half];
			if (digit >= '0' && digit <= '9')
				value = value * 16U + (unsigned)(digit - '0');
			else if (digit >= 'a' && digit <= 'f')
				value = value * 16U + (unsigned)(digit - 'a' + 10);
			else if (digit >= 'A' && digit <= 'F')
				value = value * 16U + (unsigned)(digit - 'A' + 10);
			else
				return (size_t)-1;
		}
		out[index] = (unsigned char)value;
	}
	return length / 2U;
}

/* Prints the last line and returns the exit status of the test. */
static int __attribute__((unused))
check_finish(
	const char *name)
{
	/* No checks at all is a failure too: a test that tested nothing. */
	if (check_failed == 0 && check_count > 0) {
		printf("%s: PASS (%lu checks)\n", name, check_count);
		return EXIT_SUCCESS;
	}
	printf("%s: FAIL (%lu of %lu failed)\n", name, check_failed, check_count);
	return EXIT_FAILURE;
}

#endif
