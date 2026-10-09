/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws199-p002 (d), p003: the host test of /sbin/passkey's account options
 * (userland/base/passkey/record.c, ws199-p001 section 2): the defaults
 * without a line, one line read, the lines that read as the defaults (two,
 * a field missing, unknown or repeated, no touch with the PIN asked), the
 * line written back with WS200's methods kept, the defaults' line dropped,
 * other accounts' and kinds' lines kept; and the requests of the keys'
 * operations (set-options, auth-fido2, key-owner).
 */

#include "userland/base/passkey/passkey.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The checks that failed. */
static int failures;

static void check(int condition, const char *what);
static int read_options(const char *text, const char *name, unsigned uid, struct passkey_options *options);
static void test_read(void);
static void test_write(void);
static void test_requests(void);
static void terminate(char *text, size_t size, size_t length);

int
main(void)
{
	/* Each part. */
	test_read();
	test_write();
	test_requests();

	/* The result. */
	if (failures != 0) {
		printf("passkey-options-host-test: %d failures\n", failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("passkey-options-host-test: ok\n");
	return 0;
}

/* Notes a check that failed. */
static void
check(
	int condition,
	const char *what)
{
	/* Only a failure is said. */
	if (!condition) {
		printf("FAIL %s\n", what);
		failures++;
	}
}

/* Reads an account's options from a file's text; returns what passkey_options_read did (1 a line read, 0 the defaults). */
static int
read_options(
	const char *text,
	const char *name,
	unsigned uid,
	struct passkey_options *options)
{
	int found;

	/* The whole text. */
	found = passkey_options_read(text, strlen(text), name, (uid_t)uid, options);
	return found;
}

/* The lines that read, and those that read as the defaults. */
static void
test_read(void)
{
	struct passkey_options options;
	int found;
	int defaults;

	/* No line: every method, the PIN and the touch. */
	found = read_options("# zedBSD passkey 1\nkei:1000:pin:$6$x:2026-10-10\n", "kei", 1000U, &options);
	defaults = passkey_options_is_default(&options);
	check(found == 0 && defaults && strcmp(options.methods, PASSKEY_METHODS_DEFAULT) == 0, "no line: the defaults");

	/* One line: as it says. */
	found = read_options("# zedBSD passkey 1\nkei:1000:options:methods=password,fido2:key-pin=0:key-touch=0\n", "kei", 1000U, &options);
	check(found == 1 && strcmp(options.methods, "password,fido2") == 0 && options.key_pin == 0 && options.key_touch == 0, "one line read");
	defaults = passkey_options_is_default(&options);
	check(!defaults, "that line is not the defaults");

	/* The touch alone. */
	found = read_options("# zedBSD passkey 1\nkei:1000:options:methods=password,pin,fido2:key-pin=0:key-touch=1\n", "kei", 1000U, &options);
	check(found == 1 && options.key_pin == 0 && options.key_touch == 1, "the touch alone");

	/* Another account's line, or the same name with another user ID: the defaults. */
	found = read_options("# zedBSD passkey 1\nana:1001:options:methods=password:key-pin=0:key-touch=0\n", "kei", 1000U, &options);
	check(found == 0 && options.key_pin == 1 && options.key_touch == 1, "another account's line");
	found = read_options("# zedBSD passkey 1\nkei:1002:options:methods=password:key-pin=0:key-touch=0\n", "kei", 1000U, &options);
	check(found == 0 && options.key_pin == 1, "another user ID's line");

	/* Two lines: the defaults (the strongest). */
	found = read_options("# zedBSD passkey 1\nkei:1000:options:methods=password:key-pin=0:key-touch=0\n"
	    "kei:1000:options:methods=password:key-pin=0:key-touch=1\n", "kei", 1000U, &options);
	check(found == 0 && options.key_pin == 1 && options.key_touch == 1, "two lines: the defaults");

	/* No touch while the PIN is asked: the defaults. */
	found = read_options("# zedBSD passkey 1\nkei:1000:options:methods=password:key-pin=1:key-touch=0\n", "kei", 1000U, &options);
	check(found == 0 && options.key_pin == 1 && options.key_touch == 1, "no touch with the PIN: the defaults");

	/* A field missing, repeated, unknown, or a value that is not 0 or 1: the defaults. */
	found = read_options("# zedBSD passkey 1\nkei:1000:options:methods=password:key-pin=0\n", "kei", 1000U, &options);
	check(found == 0 && options.key_pin == 1, "a field missing");
	found = read_options("# zedBSD passkey 1\nkei:1000:options:methods=password:key-pin=0:key-pin=0\n", "kei", 1000U, &options);
	check(found == 0 && options.key_pin == 1, "a field repeated");
	found = read_options("# zedBSD passkey 1\nkei:1000:options:methods=password:key-pin=0:key-color=0\n", "kei", 1000U, &options);
	check(found == 0 && options.key_pin == 1, "an unknown field");
	found = read_options("# zedBSD passkey 1\nkei:1000:options:methods=password:key-pin=2:key-touch=0\n", "kei", 1000U, &options);
	check(found == 0 && options.key_pin == 1, "a value that is not 0 or 1");
	found = read_options("# zedBSD passkey 1\nkei:1000:options:methods=password:key-pin=0:key-touch=0:more=1\n", "kei", 1000U, &options);
	check(found == 0 && options.key_pin == 1, "a field too many");

	/* The fields in another order read the same. */
	found = read_options("# zedBSD passkey 1\nkei:1000:options:key-touch=1:key-pin=0:methods=password,fido2\n", "kei", 1000U, &options);
	check(found == 1 && options.key_pin == 0 && options.key_touch == 1 && strcmp(options.methods, "password,fido2") == 0, "another order");
}

/* The line written back, as set-options and the last key's removal write it. */
static void
test_write(void)
{
	static const char text[] = "# zedBSD passkey 1\n"
	    "kei:1000:fido2:AQI:pQEC:3:zedbsd.login:Desk:2026-10-10\n"
	    "kei:1000:options:methods=password,fido2:key-pin=1:key-touch=1\n"
	    "ana:1001:options:methods=password:key-pin=0:key-touch=0\n";
	struct passkey_options options;
	char line[PASSKEY_REQUEST_MAX];
	char output[1024];
	size_t written;
	int error;
	int found;
	int defaults;

	/* set-options 0 0: WS200's methods kept, the key's PIN and touch changed. */
	found = passkey_options_read(text, sizeof(text) - 1U, "kei", 1000, &options);
	check(found == 1, "the account's line read");
	options.key_pin = 0;
	options.key_touch = 0;
	error = passkey_options_line("kei", 1000, &options, line, sizeof(line));
	check(error == 0 && strcmp(line, "kei:1000:options:methods=password,fido2:key-pin=0:key-touch=0") == 0, "the line written with the methods kept");
	error = passkey_record_replace(text, sizeof(text) - 1U, "kei", "options", line, output, sizeof(output), &written);
	check(error == 0, "the file written again");
	terminate(output, sizeof(output), written);
	check(strstr(output, "kei:1000:fido2:AQI:pQEC:3:zedbsd.login:Desk:2026-10-10\n") != NULL, "the key's line kept");
	check(strstr(output, "ana:1001:options:methods=password:key-pin=0:key-touch=0\n") != NULL, "another account's options kept");
	check(strstr(output, "kei:1000:options:methods=password,fido2:key-pin=1") == NULL, "the old line gone");
	found = passkey_options_read(output, written, "kei", 1000, &options);
	check(found == 1 && options.key_pin == 0 && options.key_touch == 0 && strcmp(options.methods, "password,fido2") == 0, "read back");

	/* The defaults: the line goes. */
	passkey_options_default(&options);
	defaults = passkey_options_is_default(&options);
	check(defaults && options.key_pin == 1 && options.key_touch == 1, "the defaults are the defaults");
	error = passkey_record_replace(text, sizeof(text) - 1U, "kei", "options", NULL, output, sizeof(output), &written);
	terminate(output, sizeof(output), written);
	check(error == 0 && strstr(output, "kei:1000:options:") == NULL && strstr(output, "ana:1001:options:") != NULL, "the defaults' line dropped");
	check(strncmp(output, "# zedBSD passkey 1\n", 19U) == 0, "the header first");

	/* No room: refused. */
	error = passkey_record_replace(text, sizeof(text) - 1U, "kei", "options", line, output, 40U, &written);
	check(error == ENOSPC, "no room");

	/* A name too long for the line's room. */
	error = passkey_options_line("kei", 1000, &options, line, 10U);
	check(error == ENAMETOOLONG, "a line that does not fit");
}

/* The keys' operations' requests: their fields. */
static void
test_requests(void)
{
	struct passkey_request request;
	char buffer[PASSKEY_REQUEST_MAX + 1U];
	int error;

	/* set-options NAME PASSWORD PIN TOUCH. */
	(void)snprintf(buffer, sizeof(buffer), "set-options\nkei\npass word\n0\n1\n");
	error = passkey_request_parse(buffer, strlen(buffer), &request);
	check(error == 0 && request.operation == PASSKEY_OP_SET_OPTIONS && strcmp(request.fields[4], "1") == 0, "set-options");
	(void)snprintf(buffer, sizeof(buffer), "set-options\nkei\npass word\n0\n");
	error = passkey_request_parse(buffer, strlen(buffer), &request);
	check(error == EINVAL, "set-options without the touch");

	/* auth-fido2 NAME login|unlock PIN, the PIN may be empty. */
	(void)snprintf(buffer, sizeof(buffer), "auth-fido2\nkei\nunlock\n\n");
	error = passkey_request_parse(buffer, strlen(buffer), &request);
	check(error == 0 && request.operation == PASSKEY_OP_AUTH_FIDO2 && request.fields[3][0] == '\0', "auth-fido2 without the PIN");

	/* key-owner NAME|-. */
	(void)snprintf(buffer, sizeof(buffer), "key-owner\n-\n");
	error = passkey_request_parse(buffer, strlen(buffer), &request);
	check(error == 0 && request.operation == PASSKEY_OP_KEY_OWNER && strcmp(request.fields[1], "-") == 0, "key-owner of every account");
	(void)snprintf(buffer, sizeof(buffer), "key-owner\nkei\n");
	error = passkey_request_parse(buffer, strlen(buffer), &request);
	check(error == 0 && request.operation == PASSKEY_OP_KEY_OWNER, "key-owner of one account");
	(void)snprintf(buffer, sizeof(buffer), "key-owner\n\n");
	error = passkey_request_parse(buffer, strlen(buffer), &request);
	check(error == EINVAL, "key-owner without a name");
	(void)snprintf(buffer, sizeof(buffer), "key-owner\nkei\nextra\n");
	error = passkey_request_parse(buffer, strlen(buffer), &request);
	check(error == EINVAL, "key-owner with a field too many");
}

/* Ends a written text with a NUL, within its buffer. */
static void
terminate(
	char *text,
	size_t size,
	size_t length)
{
	/* At its end, or at the buffer's last byte. */
	if (length >= size)
		length = size - 1U;
	text[length] = '\0';
}
