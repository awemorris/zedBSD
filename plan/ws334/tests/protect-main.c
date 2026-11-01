/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A test's console program under Wine for the Windows wrapping of the
 * browser's storage (apps/browser-win32/protect.c, shell_storage_wrap):
 *   protect-test DIRECTORY
 * DIRECTORY is a new, empty directory for a files backend.  What it checks:
 *  1. a cookie's record written through the wrapping backend reads back
 *     the same bytes, and the record the files backend holds starts with
 *     the magic and does not hold the plain bytes;
 *  2. a cookie's record written before the wrapping (plain, straight to
 *     the files backend) reads back as it is;
 *  3. a cookie's record whose blob is damaged reads as missing (0, not
 *     found), and a missing one as missing;
 *  4. a localStorage record passes through as it is (the files backend
 *     holds the same bytes);
 *  5. remove, list and clear reach the files backend, and close closes it
 *     (the directory can be opened again).
 * Last line: test-protect: PASS (<n> checks), or the failures.
 */

#include "plan/ws334/tests/unit/check.h"

#include "userland/desktop/browser/shell/shell.h"

#include <browser/storage.h>

#include <stdio.h>
#include <string.h>

/* What a protected record starts with (protect.c). */
#define TEST_MAGIC		"OGDPAPI1"
#define TEST_MAGIC_LENGTH	8U

/* The plain bytes of the records. */
#define TEST_COOKIE		"cookie record: name=value; expires in a day"
#define TEST_LOCAL		"local storage record"

static int test_read(const struct browser_storage_calls *calls, void *context, unsigned space, const char *key, char *text, size_t size, size_t *length, int *found);
static int test_count(void *each_context, const char *key);

/*
 * Runs the checks over a files backend in the directory.
 */
int
main(
	int argc,
	char **argv)
{
	const struct browser_storage_calls *calls;
	const struct browser_storage_calls *inner;
	void *context;
	void *inner_context;
	char text[4096];
	size_t length;
	unsigned count;
	int found;
	int error;
	int status;

	/* The directory. */
	if (argc != 2) {
		fprintf(stderr, "usage: protect-test DIRECTORY\n");
		return 2;
	}

	/* The files backend, then the wrapping one over it (the files backend's table kept to look beneath). */
	error = browser_storage_files_open(argv[1], &calls, &context);
	CHECK_EQ(error, 0);
	if (error != 0)
		return check_finish("protect");
	inner = calls;
	inner_context = context;
	error = shell_storage_wrap(&calls, &context);
	CHECK_EQ(error, 0);
	CHECK(calls != inner);

	/* 1. A cookie through the wrapping backend, and what the files backend holds. */
	error = calls->write(context, BROWSER_STORAGE_COOKIES, "0123abcd", TEST_COOKIE, strlen(TEST_COOKIE));
	CHECK_EQ(error, 0);
	error = test_read(calls, context, BROWSER_STORAGE_COOKIES, "0123abcd", text, sizeof(text), &length, &found);
	CHECK_EQ(error, 0);
	CHECK_EQ(found, 1);
	CHECK_EQ(length, strlen(TEST_COOKIE));
	CHECK(memcmp(text, TEST_COOKIE, strlen(TEST_COOKIE)) == 0);
	error = test_read(inner, inner_context, BROWSER_STORAGE_COOKIES, "0123abcd", text, sizeof(text) - 1U, &length, &found);
	CHECK_EQ(error, 0);
	CHECK_EQ(found, 1);
	CHECK(length > TEST_MAGIC_LENGTH);
	CHECK(memcmp(text, TEST_MAGIC, TEST_MAGIC_LENGTH) == 0);
	text[length] = '\0';
	CHECK(strstr(text + TEST_MAGIC_LENGTH, "name=value") == NULL);
	printf("protect-test: the cookie's record on disk is %zu bytes for %zu plain\n", length, strlen(TEST_COOKIE));

	/* 2. A plain record of before the wrapping. */
	error = inner->write(inner_context, BROWSER_STORAGE_COOKIES, "0123abce", TEST_COOKIE, strlen(TEST_COOKIE));
	CHECK_EQ(error, 0);
	error = test_read(calls, context, BROWSER_STORAGE_COOKIES, "0123abce", text, sizeof(text), &length, &found);
	CHECK_EQ(error, 0);
	CHECK_EQ(found, 1);
	CHECK_EQ(length, strlen(TEST_COOKIE));
	CHECK(memcmp(text, TEST_COOKIE, strlen(TEST_COOKIE)) == 0);

	/* 3. A damaged blob, and a missing record. */
	error = inner->write(inner_context, BROWSER_STORAGE_COOKIES, "0123abcf", TEST_MAGIC "not a blob", TEST_MAGIC_LENGTH + 10U);
	CHECK_EQ(error, 0);
	error = test_read(calls, context, BROWSER_STORAGE_COOKIES, "0123abcf", text, sizeof(text), &length, &found);
	CHECK_EQ(error, 0);
	CHECK_EQ(found, 0);
	CHECK_EQ(length, 0);
	error = test_read(calls, context, BROWSER_STORAGE_COOKIES, "0123abd0", text, sizeof(text), &length, &found);
	CHECK_EQ(error, 0);
	CHECK_EQ(found, 0);

	/* 4. localStorage as it is. */
	error = calls->write(context, BROWSER_STORAGE_LOCAL, "6669", TEST_LOCAL, strlen(TEST_LOCAL));
	CHECK_EQ(error, 0);
	error = test_read(inner, inner_context, BROWSER_STORAGE_LOCAL, "6669", text, sizeof(text), &length, &found);
	CHECK_EQ(error, 0);
	CHECK_EQ(found, 1);
	CHECK_EQ(length, strlen(TEST_LOCAL));
	CHECK(memcmp(text, TEST_LOCAL, strlen(TEST_LOCAL)) == 0);

	/* 5. list, remove and clear, then close and open again. */
	count = 0U;
	error = calls->list(context, BROWSER_STORAGE_COOKIES, test_count, &count);
	CHECK_EQ(error, 0);
	CHECK_EQ(count, 3U);
	error = calls->remove(context, BROWSER_STORAGE_COOKIES, "0123abcd");
	CHECK_EQ(error, 0);
	error = test_read(inner, inner_context, BROWSER_STORAGE_COOKIES, "0123abcd", text, sizeof(text), &length, &found);
	CHECK_EQ(error, 0);
	CHECK_EQ(found, 0);
	error = calls->clear(context, BROWSER_STORAGE_COOKIES);
	CHECK_EQ(error, 0);
	count = 0U;
	error = inner->list(inner_context, BROWSER_STORAGE_COOKIES, test_count, &count);
	CHECK_EQ(error, 0);
	CHECK_EQ(count, 0U);
	calls->close(context);
	error = browser_storage_files_open(argv[1], &calls, &context);
	CHECK_EQ(error, 0);
	if (error == 0)
		calls->close(context);

	/* Succeeded or not: the summary line. */
	status = check_finish("protect");
	return status;
}

/* Reads a record through a backend into text (up to size bytes): its length and whether it was found. */
static int
test_read(
	const struct browser_storage_calls *calls,
	void *context,
	unsigned space,
	const char *key,
	char *text,
	size_t size,
	size_t *length,
	int *found)
{
	struct browser_storage_sink *sink;
	const void *bytes;
	size_t had;
	int error;

	/* A sink of the test's, and the read into it. */
	*length = 0;
	*found = 0;
	error = browser_storage_sink_create(&sink);
	if (error != 0)
		return error;
	error = calls->read(context, space, key, sink, found);
	if (error == 0)
		error = browser_storage_sink_bytes(sink, &bytes, &had);
	if (error != 0) {
		browser_storage_sink_destroy(sink);
		return error;
	}

	/* The bytes, cut to the room. */
	if (had > size)
		had = size;
	if (had != 0)
		memcpy(text, bytes, had);
	*length = had;
	browser_storage_sink_destroy(sink);

	/* Succeeded: the record read. */
	return 0;
}

/* Counts a key of a list. */
static int
test_count(
	void *each_context,
	const char *key)
{
	unsigned *count;

	(void)key;

	/* One more. */
	count = each_context;
	(*count)++;
	return 0;
}
