/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Romaji to kana (plan/ws095/design.md section 7.1).
 *
 * The table is REmacs's (editor/skk.noct, skkInitRomaji) with the common
 * spellings it lacks added (ca, qa, she, thi, tsa, ltu, xwa, n' and
 * others).  The rules of skkComposeChar are kept: a whole match gives its
 * kana, n before a letter that cannot follow it gives ん, and a doubled
 * consonant gives っ.  Unlike skkComposeChar, a letter that no rule can
 * use is kept as a literal instead of being dropped.
 */

#include "ja.h"

#include <string.h>

/*
 * One romaji spelling and the kana it types, in UTF-8.
 */
struct romaji_rule {
	const char *letters;
	const char *kana;
};

/*
 * One ASCII mark and the Japanese mark it types, with whether the mark is
 * part of a word (ー) or stands between words.
 */
struct romaji_mark {
	char letter;
	uint32_t code;
	enum ja_unit_kind kind;
};

/*
 * Every spelling the composer knows.
 *
 * The first part is REmacs's table in its order; the rest are the
 * spellings Windows and Google's input methods also accept.
 */
static const struct romaji_rule romaji_rules[] = {
	{ "a", "あ" }, { "i", "い" }, { "u", "う" }, { "e", "え" }, { "o", "お" },
	{ "ka", "か" }, { "ki", "き" }, { "ku", "く" }, { "ke", "け" }, { "ko", "こ" },
	{ "sa", "さ" }, { "si", "し" }, { "shi", "し" }, { "su", "す" }, { "se", "せ" }, { "so", "そ" },
	{ "ta", "た" }, { "ti", "ち" }, { "chi", "ち" }, { "tu", "つ" }, { "tsu", "つ" }, { "te", "て" }, { "to", "と" },
	{ "na", "な" }, { "ni", "に" }, { "nu", "ぬ" }, { "ne", "ね" }, { "no", "の" },
	{ "ha", "は" }, { "hi", "ひ" }, { "hu", "ふ" }, { "fu", "ふ" }, { "he", "へ" }, { "ho", "ほ" },
	{ "ma", "ま" }, { "mi", "み" }, { "mu", "む" }, { "me", "め" }, { "mo", "も" },
	{ "ya", "や" }, { "yu", "ゆ" }, { "yo", "よ" },
	{ "ra", "ら" }, { "ri", "り" }, { "ru", "る" }, { "re", "れ" }, { "ro", "ろ" },
	{ "wa", "わ" }, { "wo", "を" }, { "nn", "ん" },
	{ "ga", "が" }, { "gi", "ぎ" }, { "gu", "ぐ" }, { "ge", "げ" }, { "go", "ご" },
	{ "za", "ざ" }, { "zi", "じ" }, { "ji", "じ" }, { "zu", "ず" }, { "ze", "ぜ" }, { "zo", "ぞ" },
	{ "da", "だ" }, { "di", "ぢ" }, { "du", "づ" }, { "de", "で" }, { "do", "ど" },
	{ "ba", "ば" }, { "bi", "び" }, { "bu", "ぶ" }, { "be", "べ" }, { "bo", "ぼ" },
	{ "pa", "ぱ" }, { "pi", "ぴ" }, { "pu", "ぷ" }, { "pe", "ぺ" }, { "po", "ぽ" },
	{ "kya", "きゃ" }, { "kyu", "きゅ" }, { "kyo", "きょ" },
	{ "sha", "しゃ" }, { "shu", "しゅ" }, { "sho", "しょ" },
	{ "sya", "しゃ" }, { "syu", "しゅ" }, { "syo", "しょ" },
	{ "cha", "ちゃ" }, { "chu", "ちゅ" }, { "cho", "ちょ" },
	{ "tya", "ちゃ" }, { "tyu", "ちゅ" }, { "tyo", "ちょ" },
	{ "nya", "にゃ" }, { "nyu", "にゅ" }, { "nyo", "にょ" },
	{ "hya", "ひゃ" }, { "hyu", "ひゅ" }, { "hyo", "ひょ" },
	{ "mya", "みゃ" }, { "myu", "みゅ" }, { "myo", "みょ" },
	{ "rya", "りゃ" }, { "ryu", "りゅ" }, { "ryo", "りょ" },
	{ "gya", "ぎゃ" }, { "gyu", "ぎゅ" }, { "gyo", "ぎょ" },
	{ "ja", "じゃ" }, { "ju", "じゅ" }, { "jo", "じょ" },
	{ "jya", "じゃ" }, { "jyu", "じゅ" }, { "jyo", "じょ" },
	{ "zya", "じゃ" }, { "zyu", "じゅ" }, { "zyo", "じょ" },
	{ "bya", "びゃ" }, { "byu", "びゅ" }, { "byo", "びょ" },
	{ "pya", "ぴゃ" }, { "pyu", "ぴゅ" }, { "pyo", "ぴょ" },
	{ "fa", "ふぁ" }, { "fi", "ふぃ" }, { "fe", "ふぇ" }, { "fo", "ふぉ" },
	{ "va", "ゔぁ" }, { "vi", "ゔぃ" }, { "vu", "ゔ" }, { "ve", "ゔぇ" }, { "vo", "ゔぉ" },
	{ "xa", "ぁ" }, { "xi", "ぃ" }, { "xu", "ぅ" }, { "xe", "ぇ" }, { "xo", "ぉ" },
	{ "xya", "ゃ" }, { "xyu", "ゅ" }, { "xyo", "ょ" }, { "xtu", "っ" },
	{ "la", "ぁ" }, { "li", "ぃ" }, { "lu", "ぅ" }, { "le", "ぇ" }, { "lo", "ぉ" },
	{ "dya", "ぢゃ" }, { "dyu", "ぢゅ" }, { "dyo", "ぢょ" },
	{ "we", "うぇ" }, { "wi", "うぃ" },
	{ "ca", "か" }, { "ci", "し" }, { "cu", "く" }, { "ce", "せ" }, { "co", "こ" },
	{ "qa", "くぁ" }, { "qi", "くぃ" }, { "qe", "くぇ" }, { "qo", "くぉ" },
	{ "she", "しぇ" }, { "je", "じぇ" }, { "che", "ちぇ" }, { "cya", "ちゃ" }, { "cyu", "ちゅ" }, { "cyo", "ちょ" },
	{ "thi", "てぃ" }, { "dhi", "でぃ" }, { "thu", "てゅ" }, { "dhu", "でゅ" }, { "twu", "とぅ" }, { "dwu", "どぅ" },
	{ "tsa", "つぁ" }, { "tsi", "つぃ" }, { "tse", "つぇ" }, { "tso", "つぉ" },
	{ "ltu", "っ" }, { "ltsu", "っ" }, { "xtsu", "っ" },
	{ "lya", "ゃ" }, { "lyu", "ゅ" }, { "lyo", "ょ" }, { "xwa", "ゎ" }, { "lwa", "ゎ" },
	{ "kwa", "くぁ" }, { "gwa", "ぐぁ" }, { "fyu", "ふゅ" }, { "ye", "いぇ" }, { "wu", "う" },
	{ "n'", "ん" }, { "xn", "ん" }
};

/*
 * The ASCII marks that type Japanese marks (the defaults of the Windows
 * and Google input methods).
 */
static const struct romaji_mark romaji_marks[] = {
	{ '-', 0x30fcU, JA_UNIT_KANA },
	{ ',', 0x3001U, JA_UNIT_LITERAL },
	{ '.', 0x3002U, JA_UNIT_LITERAL },
	{ '[', 0x300cU, JA_UNIT_LITERAL },
	{ ']', 0x300dU, JA_UNIT_LITERAL },
	{ '/', 0x30fbU, JA_UNIT_LITERAL }
};

static const struct romaji_rule *romaji_find(const char *letters, size_t length);
static bool romaji_is_prefix(const char *letters, size_t length);
static bool romaji_is_letter(char letter);
static bool romaji_is_vowel(char letter);
static void romaji_emit_kana(struct ja_romaji_result *result, const char *kana, const char *raw, size_t raw_length);
static void romaji_emit(struct ja_romaji_result *result, uint32_t code, enum ja_unit_kind kind, const char *raw, size_t raw_length);
static void romaji_drop(struct ja_romaji *romaji, size_t count);
static void romaji_settle(struct ja_romaji *romaji, struct ja_romaji_result *result);

/*
 * Empties the letters waiting to become a kana.
 */
void
ja_romaji_reset(
	struct ja_romaji *romaji)
{
	/* Nothing is pending between compositions. */
	memset(romaji->pending, 0, sizeof(romaji->pending));
	romaji->pending_length = 0;
}

/*
 * Adds one typed character and gives back the characters it completed.
 *
 * A letter (or the apostrophe of n') joins the pending letters; a mark
 * first settles what is pending and then types its Japanese mark; any
 * other character settles what is pending and is kept as a literal.
 */
void
ja_romaji_feed(
	struct ja_romaji *romaji,
	char letter,
	struct ja_romaji_result *result)
{
	size_t i;
	bool is_letter;

	result->count = 0;

	/* A letter joins the pending ones and completes what it can. */
	is_letter = romaji_is_letter(letter);
	if (is_letter) {
		romaji->pending[romaji->pending_length] = letter;
		romaji->pending_length++;
		romaji->pending[romaji->pending_length] = '\0';
		romaji_settle(romaji, result);
		return;
	}

	/* The apostrophe completes a pending n as ん. */
	if (letter == '\'' && romaji->pending_length == 1U && romaji->pending[0] == 'n') {
		romaji_emit_kana(result, "ん", "n'", 2);
		ja_romaji_reset(romaji);
		return;
	}

	/* Anything else first ends what is pending. */
	ja_romaji_flush(romaji, result);

	/* A mark with a Japanese form types that form. */
	for (i = 0; i < sizeof(romaji_marks) / sizeof(romaji_marks[0]); i++) {
		if (romaji_marks[i].letter == letter) {
			romaji_emit(result, romaji_marks[i].code, romaji_marks[i].kind, &letter, 1);
			return;
		}
	}

	/* Any other character is kept as it was typed. */
	romaji_emit(result, (unsigned char)letter, JA_UNIT_LITERAL, &letter, 1);
}

/*
 * Ends the pending letters: a lone n becomes ん and any other letter is
 * kept as a literal.
 */
void
ja_romaji_flush(
	struct ja_romaji *romaji,
	struct ja_romaji_result *result)
{
	size_t i;

	/* A lone n at the end is the ん it was meant to be. */
	if (romaji->pending_length == 1U && romaji->pending[0] == 'n') {
		romaji_emit_kana(result, "ん", "n", 1);
		ja_romaji_reset(romaji);
		return;
	}

	/* Every other pending letter stays as the letter typed. */
	for (i = 0; i < romaji->pending_length; i++)
		romaji_emit(result, (unsigned char)romaji->pending[i], JA_UNIT_LITERAL, &romaji->pending[i], 1);

	ja_romaji_reset(romaji);
}

/*
 * Takes back the last pending letter.
 *
 * Returns whether there was one to take back.
 */
bool
ja_romaji_backspace(
	struct ja_romaji *romaji)
{
	/* Nothing pending to take back. */
	if (romaji->pending_length == 0U)
		return false;

	/* Succeeded: the last letter is gone. */
	romaji->pending_length--;
	romaji->pending[romaji->pending_length] = '\0';
	return true;
}

/*
 * Finds the rule spelled exactly by some letters.
 */
static const struct romaji_rule *
romaji_find(
	const char *letters,
	size_t length)
{
	size_t i;
	size_t rule_length;
	bool same;

	/* Looks through every rule for the same spelling. */
	for (i = 0; i < sizeof(romaji_rules) / sizeof(romaji_rules[0]); i++) {
		rule_length = strlen(romaji_rules[i].letters);
		if (rule_length != length)
			continue;

		/* The same letters in the same order. */
		same = ja_bytes_equal(romaji_rules[i].letters, rule_length, letters, length);
		if (same)
			return &romaji_rules[i];
	}

	/* No rule is spelled so. */
	return NULL;
}

/*
 * Tells whether some letters begin a longer rule, so that more letters
 * are to be waited for.
 */
static bool
romaji_is_prefix(
	const char *letters,
	size_t length)
{
	size_t i;
	size_t rule_length;
	bool same;

	/* Looks through every rule for one that starts with the letters. */
	for (i = 0; i < sizeof(romaji_rules) / sizeof(romaji_rules[0]); i++) {
		rule_length = strlen(romaji_rules[i].letters);
		if (rule_length <= length)
			continue;

		/* The rule goes on after the same letters. */
		same = ja_bytes_equal(romaji_rules[i].letters, length, letters, length);
		if (same)
			return true;
	}

	/* No rule can still be completed. */
	return false;
}

/*
 * Tells whether a character is a lowercase letter, the only characters a
 * rule is spelled with.
 */
static bool
romaji_is_letter(
	char letter)
{
	/* Below a. */
	if (letter < 'a')
		return false;

	/* Above z. */
	if (letter > 'z')
		return false;

	/* A lowercase letter. */
	return true;
}

/*
 * Tells whether a letter is a vowel, which is never doubled into っ.
 */
static bool
romaji_is_vowel(
	char letter)
{
	/* The five vowels. */
	switch (letter) {
	case 'a':
	case 'i':
	case 'u':
	case 'e':
	case 'o':
		return true;
	default:
		break;
	}

	/* Anything else. */
	return false;
}

/*
 * Adds the characters of a kana spelling to a result; the raw letters go
 * with the first of them.
 */
static void
romaji_emit_kana(
	struct ja_romaji_result *result,
	const char *kana,
	const char *raw,
	size_t raw_length)
{
	size_t position;
	size_t used;
	size_t length;
	uint32_t code;
	bool first;

	/* Reads the kana one character at a time. */
	length = strlen(kana);
	position = 0;
	first = true;
	while (position < length) {
		used = ja_utf8_decode(kana + position, length - position, &code);
		position += used;

		/* Only the first character carries the letters typed. */
		if (first) {
			romaji_emit(result, code, JA_UNIT_KANA, raw, raw_length);
		} else {
			romaji_emit(result, code, JA_UNIT_KANA, "", 0);
		}

		first = false;
	}
}

/*
 * Adds one character to a result.
 */
static void
romaji_emit(
	struct ja_romaji_result *result,
	uint32_t code,
	enum ja_unit_kind kind,
	const char *raw,
	size_t raw_length)
{
	struct ja_unit *unit;

	/* A result never holds more than one step's characters. */
	if (result->count >= JA_RAW_MAX)
		return;

	/* Fills the next character. */
	unit = &result->units[result->count];
	memset(unit, 0, sizeof(*unit));
	unit->code = code;
	unit->kind = kind;

	/* Keeps as many of the letters typed as fit, terminated. */
	if (raw_length >= JA_RAW_MAX)
		raw_length = JA_RAW_MAX - 1U;

	memcpy(unit->raw, raw, raw_length);
	result->count++;
}

/*
 * Drops the first pending letters.
 */
static void
romaji_drop(
	struct ja_romaji *romaji,
	size_t count)
{
	/* Moves the rest, with its terminator, to the front. */
	memmove(romaji->pending, romaji->pending + count, romaji->pending_length - count + 1U);
	romaji->pending_length -= count;
}

/*
 * Turns as many pending letters into kana as the rules allow.
 */
static void
romaji_settle(
	struct ja_romaji *romaji,
	struct ja_romaji_result *result)
{
	const struct romaji_rule *rule;
	bool prefix;
	bool vowel;
	char first;
	char second;

	/* Settles the front of the pending letters until more must be waited for. */
	while (romaji->pending_length != 0U) {
		/* A whole spelling types its kana. */
		rule = romaji_find(romaji->pending, romaji->pending_length);
		if (rule != NULL) {
			romaji_emit_kana(result, rule->kana, romaji->pending, romaji->pending_length);
			ja_romaji_reset(romaji);
			return;
		}

		/* The start of a longer spelling waits for its next letter. */
		prefix = romaji_is_prefix(romaji->pending, romaji->pending_length);
		if (prefix)
			return;

		first = romaji->pending[0];
		second = romaji->pending[1];
		vowel = romaji_is_vowel(first);

		/* An n that no spelling can go on from is ん. */
		if (first == 'n') {
			romaji_emit_kana(result, "ん", "n", 1);
			romaji_drop(romaji, 1);
			continue;
		}

		/* A doubled consonant, or the t of tch, is っ. */
		if (romaji->pending_length >= 2U && !vowel) {
			if (first == second || (first == 't' && second == 'c')) {
				romaji_emit_kana(result, "っ", &first, 1);
				romaji_drop(romaji, 1);
				continue;
			}
		}

		/* A letter that no spelling can use is kept as it was typed. */
		romaji_emit(result, (unsigned char)first, JA_UNIT_LITERAL, &first, 1);
		romaji_drop(romaji, 1);
	}
}
