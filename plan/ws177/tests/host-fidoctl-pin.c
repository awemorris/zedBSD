/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of fidoctl's PIN reading (ws177-p006): the program's own
 * source is taken in with its main renamed, and its reader is given lines
 * from a pipe (not a terminal, so no prompt and no echo change): a PIN of
 * 4 characters is taken, one of 3 refused, 4 characters of 3 bytes each
 * taken, 63 bytes taken, 64 refused, a line past the buffer refused with
 * its rest dropped (the next line read whole), and no line refused.
 */

#define main fidoctl_main
#include "userland/base/fidoctl/main.c"
#undef main

/* The number of failed checks. */
static int test_failures;

static void test_check(int condition, const char *what);
static int test_read(const char *input, char *pin, size_t size);

/*
 * Runs the steps.
 */
int
main(void)
{
	char pin[FIDOCTL_PIN_LINE];
	char input[400];
	int error;

	/* Four characters, three, four of three bytes each. */
	error = test_read("1234\n", pin, sizeof(pin));
	test_check(error == 0 && strcmp(pin, "1234") == 0, "a PIN of 4 characters is taken");
	error = test_read("123\n", pin, sizeof(pin));
	test_check(error == EINVAL, "a PIN of 3 characters is refused");
	error = test_read("\xe3\x81\x82\xe3\x81\x82\xe3\x81\x82\xe3\x81\x82\n", pin, sizeof(pin));
	test_check(error == 0 && strlen(pin) == 12U, "4 characters of 3 bytes each are taken");

	/* 63 bytes taken, 64 refused. */
	memset(input, 'a', 63U);
	input[63] = '\n';
	input[64] = '\0';
	error = test_read(input, pin, sizeof(pin));
	test_check(error == 0 && strlen(pin) == 63U, "63 bytes are taken");
	memset(input, 'a', 64U);
	input[64] = '\n';
	input[65] = '\0';
	error = test_read(input, pin, sizeof(pin));
	test_check(error == EINVAL, "64 bytes are refused");

	/* A line past the buffer: refused, and the next line read whole. */
	memset(input, 'b', 300U);
	input[300] = '\n';
	memcpy(input + 301, "5678\n", 6U);
	error = test_read(input, pin, sizeof(pin));
	test_check(error == EINVAL, "a line past the buffer is refused");
	error = fidoctl_read_pin("PIN: ", pin, sizeof(pin));
	test_check(error == 0 && strcmp(pin, "5678") == 0, "its rest is dropped, the next line read whole");

	/* No line. */
	error = test_read("", pin, sizeof(pin));
	test_check(error == EINVAL, "no line is refused");

	/* The verdict. */
	if (test_failures != 0) {
		printf("host-fidoctl-pin: %d FAILED\n", test_failures);
		return 1;
	}

	/* Succeeded: every step held. */
	printf("host-fidoctl-pin: PASS\n");
	return 0;
}

/* Counts and names a failed check. */
static void
test_check(
	int condition,
	const char *what)
{
	/* A failure is named. */
	if (!condition) {
		printf("FAIL: %s\n", what);
		test_failures++;
	}
}

/* Gives standard input the text from a pipe, then reads one PIN; returns the reader's answer. */
static int
test_read(
	const char *input,
	char *pin,
	size_t size)
{
	FILE *stream;
	int fds[2];
	ssize_t written;
	int error;

	/* A pipe holding the text, as standard input. */
	error = pipe(fds);
	if (error != 0)
		return -1;
	written = write(fds[1], input, strlen(input));
	(void)written;
	(void)close(fds[1]);
	(void)dup2(fds[0], STDIN_FILENO);
	(void)close(fds[0]);
	stream = freopen(NULL, "r", stdin);
	if (stream == NULL)
		clearerr(stdin);

	/* Read. */
	error = fidoctl_read_pin("PIN: ", pin, size);
	return error;
}
