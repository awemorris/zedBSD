/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * q893 (ws177-p019's (b)): the host test of the on-screen keyboard's faces
 * for the kind of field (userland/desktop/wayland/keyboard-layout.c): the
 * kind from text-input-v3's purpose, the QWERTY face and the flick face
 * each kind opens on, the face key's round in each kind of field, and the
 * keys the email's, the web address's and the digits' faces have.
 *
 *     host-osk-field
 *
 * Prints "ok: ..." or "FAIL: ..." for each check; exits with 1 when one
 * failed.
 */

#include "keyboard.h"

#include <stdio.h>
#include <string.h>

/* How many checks failed. */
static int failures;

int main(void);
static void check(int condition, const char *text);
static int face_has(unsigned face, const char *label);

/*
 * Runs the checks.
 */
int
main(void)
{
	/* The kinds from the purposes: normal, alpha, digits, number, phone, url, email, name, password. */
	check(kwl_field_kind(0U) == KWL_FIELD_TEXT, "kind: normal is text");
	check(kwl_field_kind(1U) == KWL_FIELD_TEXT, "kind: alpha is text");
	check(kwl_field_kind(2U) == KWL_FIELD_NUMBER, "kind: digits is number");
	check(kwl_field_kind(3U) == KWL_FIELD_NUMBER, "kind: number is number");
	check(kwl_field_kind(4U) == KWL_FIELD_NUMBER, "kind: phone is number");
	check(kwl_field_kind(5U) == KWL_FIELD_URL, "kind: url is url");
	check(kwl_field_kind(6U) == KWL_FIELD_EMAIL, "kind: email is email");
	check(kwl_field_kind(7U) == KWL_FIELD_TEXT, "kind: name is text");
	check(kwl_field_kind(8U) == KWL_FIELD_TEXT, "kind: password is text");
	check(strcmp(kwl_field_kind_name(KWL_FIELD_NUMBER), "number") == 0, "kind: names");

	/* The QWERTY faces the kinds open on. */
	check(kwl_field_qwerty_face(KWL_FIELD_TEXT) == KWL_QWERTY_LETTERS, "qwerty: text opens the letters");
	check(kwl_field_qwerty_face(KWL_FIELD_NUMBER) == KWL_QWERTY_NUMBER, "qwerty: number opens the pad");
	check(kwl_field_qwerty_face(KWL_FIELD_EMAIL) == KWL_QWERTY_EMAIL, "qwerty: email opens the email's letters");
	check(kwl_field_qwerty_face(KWL_FIELD_URL) == KWL_QWERTY_URL, "qwerty: url opens the web address's letters");

	/* The flick faces: digits and addresses their own, a text field the user's. */
	check(kwl_field_flick_face(KWL_FIELD_NUMBER, KWL_FLICK_KANA) == KWL_FLICK_NUMBER, "flick: number opens the number face");
	check(kwl_field_flick_face(KWL_FIELD_EMAIL, KWL_FLICK_KANA) == KWL_FLICK_ALPHA, "flick: email opens the alpha face");
	check(kwl_field_flick_face(KWL_FIELD_URL, KWL_FLICK_NUMBER) == KWL_FLICK_ALPHA, "flick: url opens the alpha face");
	check(kwl_field_flick_face(KWL_FIELD_TEXT, KWL_FLICK_ALPHA) == KWL_FLICK_ALPHA, "flick: text keeps the user's face");
	check(kwl_field_flick_face(KWL_FIELD_TEXT, 99U) == KWL_FLICK_KANA, "flick: text without a face is kana");

	/* The face key's round: letters and symbols in a text field. */
	check(kwl_qwerty_face_next(KWL_QWERTY_LETTERS, KWL_QWERTY_LETTERS) == KWL_QWERTY_SYMBOLS, "round: text letters to symbols");
	check(kwl_qwerty_face_next(KWL_QWERTY_SYMBOLS, KWL_QWERTY_LETTERS) == KWL_QWERTY_LETTERS, "round: text symbols to letters");

	/* In an email field, back to the email's letters. */
	check(kwl_qwerty_face_next(KWL_QWERTY_EMAIL, KWL_QWERTY_EMAIL) == KWL_QWERTY_SYMBOLS, "round: email letters to symbols");
	check(kwl_qwerty_face_next(KWL_QWERTY_SYMBOLS, KWL_QWERTY_EMAIL) == KWL_QWERTY_EMAIL, "round: email symbols back to the email's");

	/* In a field of digits: the pad, the letters, the symbols, the pad. */
	check(kwl_qwerty_face_next(KWL_QWERTY_NUMBER, KWL_QWERTY_NUMBER) == KWL_QWERTY_LETTERS, "round: pad to letters");
	check(kwl_qwerty_face_next(KWL_QWERTY_LETTERS, KWL_QWERTY_NUMBER) == KWL_QWERTY_SYMBOLS, "round: letters to symbols");
	check(kwl_qwerty_face_next(KWL_QWERTY_SYMBOLS, KWL_QWERTY_NUMBER) == KWL_QWERTY_NUMBER, "round: symbols back to the pad");

	/* The faces' own keys. */
	check(face_has(KWL_QWERTY_EMAIL, "@") && face_has(KWL_QWERTY_EMAIL, "q") && !face_has(KWL_QWERTY_EMAIL, ","), "keys: the email's letters have @ for the comma");
	check(face_has(KWL_QWERTY_URL, "/") && face_has(KWL_QWERTY_URL, "q"), "keys: the web address's letters have /");
	check(face_has(KWL_QWERTY_NUMBER, "0") && face_has(KWL_QWERTY_NUMBER, "9") && face_has(KWL_QWERTY_NUMBER, "+") &&
	    face_has(KWL_QWERTY_NUMBER, "#") && face_has(KWL_QWERTY_NUMBER, "Del") && !face_has(KWL_QWERTY_NUMBER, "q"), "keys: the pad has digits and signs, no letters");
	check(strcmp(kwl_qwerty_face_name(KWL_QWERTY_NUMBER), "number") == 0 && strcmp(kwl_qwerty_face_name(KWL_QWERTY_EMAIL), "email") == 0, "keys: face names");

	/* The outcome. */
	if (failures != 0) {
		printf("host-osk-field: %d FAILED\n", failures);
		return 1;
	}
	printf("host-osk-field: PASS\n");
	return 0;
}

/* Prints a check's outcome. */
static void
check(
	int condition,
	const char *text)
{
	/* Passed. */
	if (condition) {
		printf("ok: %s\n", text);
		return;
	}

	/* Failed. */
	printf("FAIL: %s\n", text);
	failures++;
}

/* Tells whether a QWERTY face has a key of a label. */
static int
face_has(
	unsigned face,
	const char *label)
{
	const struct kwl_qwerty_key *keys;
	unsigned row;
	unsigned count;
	unsigned index;

	/* Each key of each row. */
	for (row = 0; row < KWL_QWERTY_ROWS; row++) {
		keys = kwl_qwerty_row(face, row, &count);
		for (index = 0; index < count; index++) {
			if (strcmp(keys[index].label, label) == 0)
				return 1;
		}
	}

	/* No key has it. */
	return 0;
}
