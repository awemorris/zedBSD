/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of /sbin/passkey's sign-in methods (ws200-p001,
 * userland/base/passkey/record.c and request.c): the methods' field read
 * (each word once, no unknown or empty word) and written in its order, the
 * methods the screens take (those set up, the password too when nothing
 * else is a first sign-in), an options line's methods (every method when
 * the field does not read), and the set-methods request's fields.
 */

#include "userland/base/passkey/passkey.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The checks that failed. */
static unsigned failures;

static void expect(int condition, const char *what);

/*
 * Runs the checks; the exit status says whether they all held.
 */
int
main(void)
{
	struct passkey_request request;
	struct passkey_options options;
	char text[PASSKEY_METHODS_MAX];
	char buffer[256];
	char file[320];
	unsigned methods;
	int error;

	/* The field read: every word, some, in any order. */
	error = passkey_methods_parse("password,pin,fido2", &methods);
	expect(error == 0 && methods == PASSKEY_METHODS_ALL, "every method");
	error = passkey_methods_parse("fido2", &methods);
	expect(error == 0 && methods == PASSKEY_METHOD_FIDO2, "a key alone");
	error = passkey_methods_parse("pin,password", &methods);
	expect(error == 0 && methods == (PASSKEY_METHOD_PASSWORD | PASSKEY_METHOD_PIN), "two in another order");

	/* What is refused. */
	expect(passkey_methods_parse("", &methods) == EINVAL, "an empty field");
	expect(passkey_methods_parse("password,", &methods) == EINVAL, "an empty word at the end");
	expect(passkey_methods_parse("password,password", &methods) == EINVAL, "a word twice");
	expect(passkey_methods_parse("pass", &methods) == EINVAL, "a word cut short");
	expect(passkey_methods_parse("passwords", &methods) == EINVAL, "a word too long");
	expect(passkey_methods_parse("face", &methods) == EINVAL, "an unknown word");

	/* Written in their order. */
	passkey_methods_text(PASSKEY_METHODS_ALL, text, sizeof(text));
	expect(strcmp(text, PASSKEY_METHODS_DEFAULT) == 0, "every method written as the default");
	passkey_methods_text(PASSKEY_METHOD_FIDO2 | PASSKEY_METHOD_PIN, text, sizeof(text));
	expect(strcmp(text, "pin,fido2") == 0, "the PIN and a key written in order");
	passkey_methods_text(0U, text, sizeof(text));
	expect(text[0] == '\0', "none written as nothing");

	/* What the screens take: those set up and turned on. */
	expect(passkey_methods_effective(PASSKEY_METHODS_ALL, 1, 1) == PASSKEY_METHODS_ALL, "every method set up");
	expect(passkey_methods_effective(PASSKEY_METHODS_ALL, 0, 0) == PASSKEY_METHOD_PASSWORD, "nothing set up but the password");
	expect(passkey_methods_effective(PASSKEY_METHOD_FIDO2 | PASSKEY_METHOD_PIN, 1, 1) == (PASSKEY_METHOD_PIN | PASSKEY_METHOD_FIDO2),
	    "the password turned off with a key");

	/* The password taken in their stead when nothing else is a first sign-in. */
	expect(passkey_methods_effective(PASSKEY_METHOD_FIDO2, 1, 0) == PASSKEY_METHOD_PASSWORD, "a key turned on but none registered");
	expect(passkey_methods_effective(PASSKEY_METHOD_PIN, 1, 1) == (PASSKEY_METHOD_PASSWORD | PASSKEY_METHOD_PIN), "the PIN alone");
	expect(passkey_methods_effective(0U, 1, 1) == PASSKEY_METHOD_PASSWORD, "nothing turned on");

	/* An options line's methods: read, or every method. */
	passkey_options_default(&options);
	expect(passkey_options_methods(&options) == PASSKEY_METHODS_ALL, "the defaults have every method");
	(void)snprintf(options.methods, sizeof(options.methods), "%s", "password,fido2");
	expect(passkey_options_methods(&options) == (PASSKEY_METHOD_PASSWORD | PASSKEY_METHOD_FIDO2), "a line's methods");
	(void)snprintf(options.methods, sizeof(options.methods), "%s", "nonsense");
	expect(passkey_options_methods(&options) == PASSKEY_METHODS_ALL, "a field that does not read has every method");

	/* A line written with methods, read back, and its defaults not kept. */
	passkey_options_default(&options);
	passkey_methods_text(PASSKEY_METHOD_PASSWORD | PASSKEY_METHOD_FIDO2, options.methods, sizeof(options.methods));
	error = passkey_options_line("kei", 1000, &options, buffer, sizeof(buffer));
	expect(error == 0 && strcmp(buffer, "kei:1000:options:methods=password,fido2:key-pin=1:key-touch=1") == 0, "the line with the methods");
	expect(!passkey_options_is_default(&options), "methods turned off are no default");
	(void)snprintf(file, sizeof(file), "# zedBSD passkey 1\n%s\n", buffer);
	passkey_options_default(&options);
	(void)passkey_options_read(file, strlen(file), "kei", 1000, &options);
	expect(strcmp(options.methods, "password,fido2") == 0, "the line read back");

	/* set-methods: the name, the password and the methods. */
	(void)snprintf(buffer, sizeof(buffer), "set-methods\nkei\nsecret\npassword,fido2\n");
	error = passkey_request_parse(buffer, strlen(buffer), &request);
	expect(error == 0 && request.operation == PASSKEY_OP_SET_METHODS && strcmp(request.fields[3], "password,fido2") == 0, "set-methods is read");
	(void)snprintf(buffer, sizeof(buffer), "set-methods\nkei\nsecret\n");
	expect(passkey_request_parse(buffer, strlen(buffer), &request) == EINVAL, "set-methods without the methods is refused");

	/* The verdict. */
	if (failures != 0U) {
		printf("passkey-methods-host-test: %u FAILED\n", failures);
		return 1;
	}

	/* Every check held. */
	printf("passkey-methods-host-test: PASS\n");
	return 0;
}

/* Counts and reports a check that does not hold. */
static void
expect(
	int condition,
	const char *what)
{
	/* A check that holds says nothing. */
	if (condition)
		return;
	failures++;
	printf("FAIL: %s\n", what);
}
