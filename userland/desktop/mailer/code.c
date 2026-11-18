/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sign-in code of a message (WS169 p003, plan/ws169/phase001/phase.md
 * section 4): when the subject or the words speak of a code (code,
 * passcode, verification, one-time, OTP, PIN, コード, 認証, 確認), the
 * first run of 6 to 8 digits in the words (or else the subject) that no
 * other digit or letter touches, or else the first run of 4 or 5 that is
 * not a year (1900 to 2099).
 *
 * ws177-p014: a code with letters (X7K2PQ) is taken when it comes right
 * after a word of a code ("Your code is X7K2PQ", "コード：X7K2PQ") in a
 * message that speaks of signing in (verification, one-time, 認証 ...):
 * 4 to 8 capitals and digits with at least one of each, so that a word or
 * a promotion's code (SAVE20 in a message about nothing else) is not
 * taken.  It is looked for before the digits.  An ASCII word counts only
 * as a whole word (its plural too), so the "pin" of "shipping" is not one.
 */

#include "mail.h"

#include <string.h>

/* The fewest and the most digits of a code, and the fewest of a long one (looked for first). */
#define CODE_DIGITS_MIN		4U
#define CODE_DIGITS_MAX		8U
#define CODE_DIGITS_LONG	6U

/* The fewest and the most characters of a code with letters. */
#define CODE_LETTERED_MIN	4U
#define CODE_LETTERED_MAX	8U

/* The words that tell a message carries a code, ASCII ones matched in any case. */
static const char *const code_words[] = {
	"code",
	"passcode",
	"verification",
	"verify",
	"one-time",
	"otp",
	"pin",
	"\xe3\x82\xb3\xe3\x83\xbc\xe3\x83\x89",	/* コード */
	"\xe8\xaa\x8d\xe8\xa8\xbc",		/* 認証 */
	"\xe7\xa2\xba\xe8\xaa\x8d"		/* 確認 */
};

/* The words that tell the code is one of signing in, without which a code with letters is not taken. */
static const char *const code_sign_in_words[] = {
	"passcode",
	"verification",
	"verify",
	"one-time",
	"otp",
	"sign-in",
	"sign in",
	"login",
	"log in",
	"\xe8\xaa\x8d\xe8\xa8\xbc",			/* 認証 */
	"\xe7\xa2\xba\xe8\xaa\x8d",			/* 確認 */
	"\xe3\x83\xaf\xe3\x83\xb3\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\xa0",	/* ワンタイム */
	"\xe3\x83\xad\xe3\x82\xb0\xe3\x82\xa4\xe3\x83\xb3"		/* ログイン */
};

/* The words a code with letters comes right after. */
static const char *const code_lead_words[] = {
	"code",
	"passcode",
	"otp",
	"pin",
	"\xe3\x82\xb3\xe3\x83\xbc\xe3\x83\x89"	/* コード */
};

/* The full-width colon and the particle は, which may stand between a word and its code. */
#define CODE_WIDE_COLON		"\xef\xbc\x9a"
#define CODE_PARTICLE_WA	"\xe3\x81\xaf"

static int code_speaks_of(const char *text, const char *const *words, size_t count);
static int code_contains(const char *text, const char *word);
static int code_word_at(const char *text, size_t at, const char *word);
static int code_take_lettered(const char *text, char *code, size_t size);
static size_t code_skip_between(const char *text, size_t at);
static int code_lettered_at(const char *text, size_t at, char *code, size_t size);
static int code_take(const char *text, size_t fewest, size_t most, char *code, size_t size);
static int code_is_year(const char *digits, size_t length);
static int code_is_digit(int c);
static int code_is_letter(int c);

/*
 * Finds the sign-in code of a message.  Returns 1 with it in code, 0 when
 * the message has none (code is then empty).
 */
int
ml_code_find(
	const char *subject,
	const char *body,
	char *code,
	size_t size)
{
	size_t count;
	int speaks;
	int sign_in;
	int taken;

	/* None yet. */
	code[0] = '\0';

	/* The subject or the words must speak of a code. */
	count = sizeof(code_words) / sizeof(code_words[0]);
	speaks = code_speaks_of(subject, code_words, count);
	if (!speaks)
		speaks = code_speaks_of(body, code_words, count);
	if (!speaks)
		return 0;

	/* Whether they speak of signing in, which a code with letters needs. */
	count = sizeof(code_sign_in_words) / sizeof(code_sign_in_words[0]);
	sign_in = code_speaks_of(subject, code_sign_in_words, count);
	if (!sign_in)
		sign_in = code_speaks_of(body, code_sign_in_words, count);

	/* A code with letters right after a word of a code, in the words or else the subject. */
	if (sign_in) {
		taken = code_take_lettered(body, code, size);
		if (taken)
			return 1;
		taken = code_take_lettered(subject, code, size);
		if (taken)
			return 1;
	}

	/* The first long code of the words. */
	taken = code_take(body, CODE_DIGITS_LONG, CODE_DIGITS_MAX, code, size);
	if (taken)
		return 1;

	/* Else of the subject. */
	taken = code_take(subject, CODE_DIGITS_LONG, CODE_DIGITS_MAX, code, size);
	if (taken)
		return 1;

	/* Else a short one of the words. */
	taken = code_take(body, CODE_DIGITS_MIN, CODE_DIGITS_LONG - 1U, code, size);
	if (taken)
		return 1;

	/* Else of the subject. */
	taken = code_take(subject, CODE_DIGITS_MIN, CODE_DIGITS_LONG - 1U, code, size);
	if (taken)
		return 1;

	/* No code. */
	return 0;
}

/* Tells whether a text has one of a list of words. */
static int
code_speaks_of(
	const char *text,
	const char *const *words,
	size_t count)
{
	size_t index;
	int found;

	/* Each word. */
	for (index = 0; index < count; index++) {
		found = code_contains(text, words[index]);
		if (found)
			return 1;
	}

	/* None. */
	return 0;
}

/* Tells whether a text holds a word, a whole one when it is ASCII. */
static int
code_contains(
	const char *text,
	const char *word)
{
	size_t at;
	int found;

	/* Each place the word could start. */
	for (at = 0; text[at] != '\0'; at++) {
		found = code_word_at(text, at, word);
		if (found)
			return 1;
	}

	/* Not held. */
	return 0;
}

/*
 * Tells whether a word starts at a place of a text: ASCII letters in any
 * case, and an ASCII word only as a whole word (no letter just before it,
 * none just after it but a plural's s).  Returns the word's length with
 * its plural's s, 0 when it is not there.
 */
static int
code_word_at(
	const char *text,
	size_t at,
	const char *word)
{
	size_t length;
	size_t index;
	int letter;
	int a;
	int b;

	/* The bytes from there, in lower case. */
	length = strlen(word);
	for (index = 0; index < length; index++) {
		a = (unsigned char)text[at + index];
		b = (unsigned char)word[index];
		if (a >= 'A' && a <= 'Z')
			a = a - 'A' + 'a';
		if (a != b)
			return 0;
	}

	/* A word of Japanese has no spaces around it. */
	if ((unsigned char)word[0] >= 0x80U)
		return (int)length;

	/* A letter just before makes it the end of another word ("shipping"). */
	if (at > 0U) {
		letter = code_is_letter((unsigned char)text[at - 1U]);
		if (letter)
			return 0;
	}

	/* A plural's s is a part of the word. */
	if (text[at + length] == 's' || text[at + length] == 'S') {
		letter = code_is_letter((unsigned char)text[at + length + 1U]);
		if (!letter)
			return (int)(length + 1U);
	}

	/* A letter just after makes it the start of another word ("pinch"). */
	letter = code_is_letter((unsigned char)text[at + length]);
	if (letter)
		return 0;

	/* Succeeded: the whole word. */
	return (int)length;
}

/* Takes the first code with letters that comes right after a word of a code. */
static int
code_take_lettered(
	const char *text,
	char *code,
	size_t size)
{
	size_t at;
	size_t index;
	size_t after;
	int length;
	int taken;

	/* Each place, each word a code follows. */
	for (at = 0; text[at] != '\0'; at++) {
		for (index = 0; index < sizeof(code_lead_words) / sizeof(code_lead_words[0]); index++) {
			length = code_word_at(text, at, code_lead_words[index]);
			if (length == 0)
				continue;

			/* What stands right after the word and its "is" or colon. */
			after = code_skip_between(text, at + (size_t)length);
			taken = code_lettered_at(text, after, code, size);
			if (taken)
				return 1;
		}
	}

	/* None. */
	return 0;
}

/* Skips the spaces, colons, dashes, "is" and は between a word and its code; returns where the code would start. */
static size_t
code_skip_between(
	const char *text,
	size_t at)
{
	int differs;
	int letter;

	/* Until something else. */
	for (;;) {
		/* A space, a colon, an equals sign or a dash. */
		if (text[at] == ' ' ||
		    text[at] == '\t' ||
		    text[at] == '\r' ||
		    text[at] == '\n' ||
		    text[at] == ':' ||
		    text[at] == '=' ||
		    text[at] == '-') {
			at++;
			continue;
		}

		/* A full-width colon. */
		differs = strncmp(text + at, CODE_WIDE_COLON, 3U);
		if (differs == 0) {
			at += 3U;
			continue;
		}

		/* The particle は. */
		differs = strncmp(text + at, CODE_PARTICLE_WA, 3U);
		if (differs == 0) {
			at += 3U;
			continue;
		}

		/* The word "is" (not the start of another word). */
		if ((text[at] == 'i' || text[at] == 'I') && (text[at + 1U] == 's' || text[at + 1U] == 'S')) {
			letter = code_is_letter((unsigned char)text[at + 2U]);
			if (!letter) {
				at += 2U;
				continue;
			}
		}

		/* Succeeded: the code would start here. */
		return at;
	}
}

/*
 * Takes a code with letters that starts at a place: a whole run of 4 to 8
 * ASCII letters and digits with at least one capital and one digit and no
 * small letter.
 */
static int
code_lettered_at(
	const char *text,
	size_t at,
	char *code,
	size_t size)
{
	size_t end;
	size_t digits;
	size_t capitals;
	int c;

	/* The run of letters and digits, counted. */
	digits = 0;
	capitals = 0;
	for (end = at; text[end] != '\0'; end++) {
		c = (unsigned char)text[end];
		if (c >= '0' && c <= '9') {
			digits++;
		} else if (c >= 'A' && c <= 'Z') {
			capitals++;
		} else if (c >= 'a' && c <= 'z') {
			/* A small letter makes it a word. */
			return 0;
		} else {
			break;
		}
	}

	/* Too short or too long a run, or one that does not fit. */
	if (end - at < CODE_LETTERED_MIN || end - at > CODE_LETTERED_MAX)
		return 0;
	if (end - at >= size)
		return 0;

	/* Digits alone are left to the digits' rules, and letters alone are a word. */
	if (digits == 0U || capitals == 0U)
		return 0;

	/* Succeeded: the code. */
	memcpy(code, text + at, end - at);
	code[end - at] = '\0';
	return 1;
}

/* Takes the first run of fewest to most digits that no digit or letter touches and is not a year. */
static int
code_take(
	const char *text,
	size_t fewest,
	size_t most,
	char *code,
	size_t size)
{
	size_t start;
	size_t end;
	int before;
	int after;
	int digit;
	int year;

	/* Each run of digits. */
	start = 0;
	while (text[start] != '\0') {
		/* Not a digit: on. */
		digit = code_is_digit((unsigned char)text[start]);
		if (!digit) {
			start++;
			continue;
		}

		/* The run's end. */
		end = start;
		for (;;) {
			digit = code_is_digit((unsigned char)text[end]);
			if (!digit)
				break;
			end++;
		}

		/* A letter just before or after makes it a part of a word (an order number, a street). */
		before = 0;
		if (start > 0U)
			before = code_is_letter((unsigned char)text[start - 1U]);
		after = code_is_letter((unsigned char)text[end]);
		year = code_is_year(text + start, end - start);

		/* A code: of the right length, alone, not a year, and fitting the room. */
		if (end - start >= fewest &&
		    end - start <= most &&
		    !before &&
		    !after &&
		    !year &&
		    end - start < size) {
			memcpy(code, text + start, end - start);
			code[end - start] = '\0';
			return 1;
		}

		/* The next run. */
		start = end;
	}

	/* None. */
	return 0;
}

/* Tells whether a run of digits is a year of the dates mail carries (1900 to 2099). */
static int
code_is_year(
	const char *digits,
	size_t length)
{
	/* Four digits only. */
	if (length != 4U)
		return 0;

	/* 19xx. */
	if (digits[0] == '1' && digits[1] == '9')
		return 1;

	/* 20xx. */
	if (digits[0] == '2' && digits[1] == '0')
		return 1;

	/* Not a year. */
	return 0;
}

/* Tells whether a byte is an ASCII digit. */
static int
code_is_digit(
	int c)
{
	/* 0 to 9. */
	if (c >= '0' && c <= '9')
		return 1;

	/* Anything else. */
	return 0;
}

/* Tells whether a byte is an ASCII letter. */
static int
code_is_letter(
	int c)
{
	/* A small letter. */
	if (c >= 'a' && c <= 'z')
		return 1;

	/* A capital. */
	if (c >= 'A' && c <= 'Z')
		return 1;

	/* Anything else. */
	return 0;
}
