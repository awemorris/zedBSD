/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws177-p014: the host test of the sign-in code of a message
 * (userland/desktop/mailer/code.c): a code with letters right after a word
 * of a code in a message about signing in, not in a promotion, not in
 * small letters; the words as whole words (the "pin" of "shipping" is
 * not one); and the digits' rules as before.
 *
 * Prints "PASS name" or "FAIL name [code]" for each check; exits with 1
 * when one failed.
 */

#include "userland/desktop/mailer/mail.h"

#include <stdio.h>
#include <string.h>

/* The checks that failed. */
static int test_failures;

int main(void);
static void test_find(const char *name, const char *subject, const char *body, const char *expected);

/*
 * Runs the checks.
 */
int
main(void)
{
	/* Codes with letters. */
	test_find("lettered", "Your verification code", "Your code is X7K2PQ. It expires soon.", "X7K2PQ");
	test_find("lettered-colon", "Sign-in", "Code: AB12CD", "AB12CD");
	test_find("lettered-japanese", "\xe8\xaa\x8d\xe8\xa8\xbc", "\xe3\x82\xb3\xe3\x83\xbc\xe3\x83\x89\xef\xbc\x9a" "K9M3 \xe3\x81\xa7\xe3\x81\x99", "K9M3");
	test_find("lettered-before-digits", "Your one-time code", "Code: X7K2PQ\nOrder 123456 shipped.", "X7K2PQ");
	test_find("lettered-promotion", "Sale", "Use code SAVE20 for 20% off.", "");
	test_find("lettered-small", "Your verification", "Your code is below: 482913", "482913");
	test_find("lettered-word", "Verify", "Your code is READY.", "");
	test_find("lettered-prefix", "Verification", "G-482913 is your Google verification code.", "482913");

	/* Whole words. */
	test_find("word-shipping", "Shipping update", "Your parcel 123456 is on its way.", "");
	test_find("word-barcode", "Barcode", "Item 482913.", "");
	test_find("word-plural", "Your codes", "482913", "482913");
	test_find("word-pin", "PIN", "Your PIN: 7351", "7351");

	/* The digits as before. */
	test_find("digits-six", "Your sign-in code", "On 5 October 2026 you asked.\n\n482913\n", "482913");
	test_find("digits-year", "Your verification", "Order 2026 is ready. Your one-time code is 7351.", "7351");
	test_find("digits-in-word", "code", "Reference AB123456 only.", "");

	/* The outcome. */
	if (test_failures != 0) {
		printf("host-mail-code: %d FAILED\n", test_failures);
		return 1;
	}
	printf("host-mail-code: PASS\n");
	return 0;
}

/* Finds the code of a message and checks it against the one expected ("" for none). */
static void
test_find(
	const char *name,
	const char *subject,
	const char *body,
	const char *expected)
{
	char code[ML_CODE_MAX];
	int found;

	/* The code found. */
	found = ml_code_find(subject, body, code, sizeof(code));

	/* Found as expected, or nothing found when none is. */
	if ((expected[0] == '\0' && !found && code[0] == '\0') || (expected[0] != '\0' && found && strcmp(code, expected) == 0)) {
		printf("PASS %s\n", name);
		return;
	}

	/* Failed, with what was found. */
	printf("FAIL %s [%s]\n", name, code);
	test_failures++;
}
