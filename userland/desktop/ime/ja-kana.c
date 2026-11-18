/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kana the Japanese engine knows: UTF-8, hiragana and katakana, the
 * half-width forms, full-width letters, and which consonant and vowel each
 * kana is typed with (plan/ws095/design.md section 7).
 *
 * The consonants follow the SKK dictionary's headwords as REmacs builds
 * them (tools/skkdict/expand.py): a verb's headword ends with the letter
 * its first okurigana kana is typed with, う being typed as w.
 */

#include "ja.h"

#include <string.h>

/* The first and the last hiragana, and how far katakana lie from them. */
#define KANA_HIRAGANA_FIRST	0x3041U
#define KANA_HIRAGANA_LAST	0x3096U
#define KANA_KATAKANA_OFFSET	0x60U

/* The printable ASCII range, the full-width letters and the ideographic space. */
#define KANA_ASCII_FIRST	0x21U
#define KANA_ASCII_LAST		0x7eU
#define KANA_FULL_FIRST		0xff01U
#define KANA_ASCII_SPACE	0x20U
#define KANA_FULL_SPACE		0x3000U

/* The code a malformed UTF-8 byte is read as. */
#define KANA_REPLACEMENT	0xfffdU

/*
 * One kana with the consonant it is typed with and its vowel (0 to 4 for
 * a, i, u, e, o; -1 for ん).
 */
struct kana_sound {
	uint32_t code;
	char consonant;
	int vowel;
};

/*
 * One row of the kana a godan verb conjugates through, by vowel.
 */
struct kana_row {
	char consonant;
	uint32_t kana[5];
};

/*
 * The consonant and the vowel of each kana a headword's okurigana can
 * begin with, in code order.
 */
static const struct kana_sound kana_sounds[] = {
	{ 0x3042U, 'a', 0 },	/* あ */
	{ 0x3044U, 'i', 1 },	/* い */
	{ 0x3046U, 'w', 2 },	/* う */
	{ 0x3048U, 'e', 3 },	/* え */
	{ 0x304aU, 'o', 4 },	/* お */
	{ 0x304bU, 'k', 0 },	/* か */
	{ 0x304cU, 'g', 0 },	/* が */
	{ 0x304dU, 'k', 1 },	/* き */
	{ 0x304eU, 'g', 1 },	/* ぎ */
	{ 0x304fU, 'k', 2 },	/* く */
	{ 0x3050U, 'g', 2 },	/* ぐ */
	{ 0x3051U, 'k', 3 },	/* け */
	{ 0x3052U, 'g', 3 },	/* げ */
	{ 0x3053U, 'k', 4 },	/* こ */
	{ 0x3054U, 'g', 4 },	/* ご */
	{ 0x3055U, 's', 0 },	/* さ */
	{ 0x3056U, 'z', 0 },	/* ざ */
	{ 0x3057U, 's', 1 },	/* し */
	{ 0x3058U, 'j', 1 },	/* じ */
	{ 0x3059U, 's', 2 },	/* す */
	{ 0x305aU, 'z', 2 },	/* ず */
	{ 0x305bU, 's', 3 },	/* せ */
	{ 0x305cU, 'z', 3 },	/* ぜ */
	{ 0x305dU, 's', 4 },	/* そ */
	{ 0x305eU, 'z', 4 },	/* ぞ */
	{ 0x305fU, 't', 0 },	/* た */
	{ 0x3060U, 'd', 0 },	/* だ */
	{ 0x3061U, 't', 1 },	/* ち */
	{ 0x3062U, 'j', 1 },	/* ぢ */
	{ 0x3064U, 't', 2 },	/* つ */
	{ 0x3065U, 'z', 2 },	/* づ */
	{ 0x3066U, 't', 3 },	/* て */
	{ 0x3067U, 'd', 3 },	/* で */
	{ 0x3068U, 't', 4 },	/* と */
	{ 0x3069U, 'd', 4 },	/* ど */
	{ 0x306aU, 'n', 0 },	/* な */
	{ 0x306bU, 'n', 1 },	/* に */
	{ 0x306cU, 'n', 2 },	/* ぬ */
	{ 0x306dU, 'n', 3 },	/* ね */
	{ 0x306eU, 'n', 4 },	/* の */
	{ 0x306fU, 'h', 0 },	/* は */
	{ 0x3070U, 'b', 0 },	/* ば */
	{ 0x3071U, 'p', 0 },	/* ぱ */
	{ 0x3072U, 'h', 1 },	/* ひ */
	{ 0x3073U, 'b', 1 },	/* び */
	{ 0x3074U, 'p', 1 },	/* ぴ */
	{ 0x3075U, 'h', 2 },	/* ふ */
	{ 0x3076U, 'b', 2 },	/* ぶ */
	{ 0x3077U, 'p', 2 },	/* ぷ */
	{ 0x3078U, 'h', 3 },	/* へ */
	{ 0x3079U, 'b', 3 },	/* べ */
	{ 0x307aU, 'p', 3 },	/* ぺ */
	{ 0x307bU, 'h', 4 },	/* ほ */
	{ 0x307cU, 'b', 4 },	/* ぼ */
	{ 0x307dU, 'p', 4 },	/* ぽ */
	{ 0x307eU, 'm', 0 },	/* ま */
	{ 0x307fU, 'm', 1 },	/* み */
	{ 0x3080U, 'm', 2 },	/* む */
	{ 0x3081U, 'm', 3 },	/* め */
	{ 0x3082U, 'm', 4 },	/* も */
	{ 0x3084U, 'y', 0 },	/* や */
	{ 0x3086U, 'y', 2 },	/* ゆ */
	{ 0x3088U, 'y', 4 },	/* よ */
	{ 0x3089U, 'r', 0 },	/* ら */
	{ 0x308aU, 'r', 1 },	/* り */
	{ 0x308bU, 'r', 2 },	/* る */
	{ 0x308cU, 'r', 3 },	/* れ */
	{ 0x308dU, 'r', 4 },	/* ろ */
	{ 0x308fU, 'w', 0 },	/* わ */
	{ 0x3092U, 'w', 4 },	/* を */
	{ 0x3093U, 'n', -1 },	/* ん */
};

/*
 * The rows a godan verb's last kana runs through, by the consonant of its
 * headword (わ行 is the row of 買う: わ, い, う, え, お).
 */
static const struct kana_row kana_rows[] = {
	{ 'k', { 0x304bU, 0x304dU, 0x304fU, 0x3051U, 0x3053U } },	/* かきくけこ */
	{ 'g', { 0x304cU, 0x304eU, 0x3050U, 0x3052U, 0x3054U } },	/* がぎぐげご */
	{ 's', { 0x3055U, 0x3057U, 0x3059U, 0x305bU, 0x305dU } },	/* さしすせそ */
	{ 'z', { 0x3056U, 0x3058U, 0x305aU, 0x305cU, 0x305eU } },	/* ざじずぜぞ */
	{ 't', { 0x305fU, 0x3061U, 0x3064U, 0x3066U, 0x3068U } },	/* たちつてと */
	{ 'd', { 0x3060U, 0x3062U, 0x3065U, 0x3067U, 0x3069U } },	/* だぢづでど */
	{ 'n', { 0x306aU, 0x306bU, 0x306cU, 0x306dU, 0x306eU } },	/* なにぬねの */
	{ 'h', { 0x306fU, 0x3072U, 0x3075U, 0x3078U, 0x307bU } },	/* はひふへほ */
	{ 'b', { 0x3070U, 0x3073U, 0x3076U, 0x3079U, 0x307cU } },	/* ばびぶべぼ */
	{ 'p', { 0x3071U, 0x3074U, 0x3077U, 0x307aU, 0x307dU } },	/* ぱぴぷぺぽ */
	{ 'm', { 0x307eU, 0x307fU, 0x3080U, 0x3081U, 0x3082U } },	/* まみむめも */
	{ 'r', { 0x3089U, 0x308aU, 0x308bU, 0x308cU, 0x308dU } },	/* らりるれろ */
	{ 'w', { 0x308fU, 0x3044U, 0x3046U, 0x3048U, 0x304aU } },	/* わいうえお */
};

/*
 * The half-width katakana of each hiragana from ぁ to ゖ, in code order;
 * a voiced kana takes two half-width characters.
 */
static const char *const kana_half[KANA_HIRAGANA_LAST - KANA_HIRAGANA_FIRST + 1U] = {
	"\xef\xbd\xa7",	/* ぁ */
	"\xef\xbd\xb1",	/* あ */
	"\xef\xbd\xa8",	/* ぃ */
	"\xef\xbd\xb2",	/* い */
	"\xef\xbd\xa9",	/* ぅ */
	"\xef\xbd\xb3",	/* う */
	"\xef\xbd\xaa",	/* ぇ */
	"\xef\xbd\xb4",	/* え */
	"\xef\xbd\xab",	/* ぉ */
	"\xef\xbd\xb5",	/* お */
	"\xef\xbd\xb6",	/* か */
	"\xef\xbd\xb6\xef\xbe\x9e",	/* が */
	"\xef\xbd\xb7",	/* き */
	"\xef\xbd\xb7\xef\xbe\x9e",	/* ぎ */
	"\xef\xbd\xb8",	/* く */
	"\xef\xbd\xb8\xef\xbe\x9e",	/* ぐ */
	"\xef\xbd\xb9",	/* け */
	"\xef\xbd\xb9\xef\xbe\x9e",	/* げ */
	"\xef\xbd\xba",	/* こ */
	"\xef\xbd\xba\xef\xbe\x9e",	/* ご */
	"\xef\xbd\xbb",	/* さ */
	"\xef\xbd\xbb\xef\xbe\x9e",	/* ざ */
	"\xef\xbd\xbc",	/* し */
	"\xef\xbd\xbc\xef\xbe\x9e",	/* じ */
	"\xef\xbd\xbd",	/* す */
	"\xef\xbd\xbd\xef\xbe\x9e",	/* ず */
	"\xef\xbd\xbe",	/* せ */
	"\xef\xbd\xbe\xef\xbe\x9e",	/* ぜ */
	"\xef\xbd\xbf",	/* そ */
	"\xef\xbd\xbf\xef\xbe\x9e",	/* ぞ */
	"\xef\xbe\x80",	/* た */
	"\xef\xbe\x80\xef\xbe\x9e",	/* だ */
	"\xef\xbe\x81",	/* ち */
	"\xef\xbe\x81\xef\xbe\x9e",	/* ぢ */
	"\xef\xbd\xaf",	/* っ */
	"\xef\xbe\x82",	/* つ */
	"\xef\xbe\x82\xef\xbe\x9e",	/* づ */
	"\xef\xbe\x83",	/* て */
	"\xef\xbe\x83\xef\xbe\x9e",	/* で */
	"\xef\xbe\x84",	/* と */
	"\xef\xbe\x84\xef\xbe\x9e",	/* ど */
	"\xef\xbe\x85",	/* な */
	"\xef\xbe\x86",	/* に */
	"\xef\xbe\x87",	/* ぬ */
	"\xef\xbe\x88",	/* ね */
	"\xef\xbe\x89",	/* の */
	"\xef\xbe\x8a",	/* は */
	"\xef\xbe\x8a\xef\xbe\x9e",	/* ば */
	"\xef\xbe\x8a\xef\xbe\x9f",	/* ぱ */
	"\xef\xbe\x8b",	/* ひ */
	"\xef\xbe\x8b\xef\xbe\x9e",	/* び */
	"\xef\xbe\x8b\xef\xbe\x9f",	/* ぴ */
	"\xef\xbe\x8c",	/* ふ */
	"\xef\xbe\x8c\xef\xbe\x9e",	/* ぶ */
	"\xef\xbe\x8c\xef\xbe\x9f",	/* ぷ */
	"\xef\xbe\x8d",	/* へ */
	"\xef\xbe\x8d\xef\xbe\x9e",	/* べ */
	"\xef\xbe\x8d\xef\xbe\x9f",	/* ぺ */
	"\xef\xbe\x8e",	/* ほ */
	"\xef\xbe\x8e\xef\xbe\x9e",	/* ぼ */
	"\xef\xbe\x8e\xef\xbe\x9f",	/* ぽ */
	"\xef\xbe\x8f",	/* ま */
	"\xef\xbe\x90",	/* み */
	"\xef\xbe\x91",	/* む */
	"\xef\xbe\x92",	/* め */
	"\xef\xbe\x93",	/* も */
	"\xef\xbd\xac",	/* ゃ */
	"\xef\xbe\x94",	/* や */
	"\xef\xbd\xad",	/* ゅ */
	"\xef\xbe\x95",	/* ゆ */
	"\xef\xbd\xae",	/* ょ */
	"\xef\xbe\x96",	/* よ */
	"\xef\xbe\x97",	/* ら */
	"\xef\xbe\x98",	/* り */
	"\xef\xbe\x99",	/* る */
	"\xef\xbe\x9a",	/* れ */
	"\xef\xbe\x9b",	/* ろ */
	"\xef\xbe\x9c",	/* ゎ */
	"\xef\xbe\x9c",	/* わ */
	"\xef\xbd\xb2",	/* ゐ */
	"\xef\xbd\xb4",	/* ゑ */
	"\xef\xbd\xa6",	/* を */
	"\xef\xbe\x9d",	/* ん */
	"\xef\xbd\xb3\xef\xbe\x9e",	/* ゔ */
	"\xef\xbd\xb6",	/* ゕ */
	"\xef\xbd\xb9",	/* ゖ */
};

static const struct kana_sound *kana_find_sound(uint32_t code);

/*
 * Writes a code point as UTF-8.
 *
 * Returns the number of bytes written (at most 4).
 */
size_t
ja_utf8_encode(
	uint32_t code,
	char *out)
{
	/* One byte for ASCII. */
	if (code < 0x80U) {
		out[0] = (char)code;
		return 1;
	}

	/* Two bytes up to U+07FF. */
	if (code < 0x800U) {
		out[0] = (char)(0xc0U | (code >> 6));
		out[1] = (char)(0x80U | (code & 0x3fU));
		return 2;
	}

	/* Three bytes up to U+FFFF. */
	if (code < 0x10000U) {
		out[0] = (char)(0xe0U | (code >> 12));
		out[1] = (char)(0x80U | ((code >> 6) & 0x3fU));
		out[2] = (char)(0x80U | (code & 0x3fU));
		return 3;
	}

	/* Four bytes for the rest. */
	out[0] = (char)(0xf0U | (code >> 18));
	out[1] = (char)(0x80U | ((code >> 12) & 0x3fU));
	out[2] = (char)(0x80U | ((code >> 6) & 0x3fU));
	out[3] = (char)(0x80U | (code & 0x3fU));
	return 4;
}

/*
 * Reads one code point from UTF-8.
 *
 * Returns the number of bytes it took, 0 at the end of the text.  A byte
 * that does not begin a well-formed character is read alone as U+FFFD.
 */
size_t
ja_utf8_decode(
	const char *text,
	size_t length,
	uint32_t *code)
{
	const unsigned char *bytes;
	size_t count;
	size_t i;
	uint32_t value;

	bytes = (const unsigned char *)text;

	/* Nothing is left to read. */
	if (length == 0U)
		return 0;

	/* ASCII is one byte. */
	if (bytes[0] < 0x80U) {
		*code = bytes[0];
		return 1;
	}

	/* The lead byte says how many bytes follow. */
	if ((bytes[0] & 0xe0U) == 0xc0U) {
		count = 2;
		value = bytes[0] & 0x1fU;
	} else if ((bytes[0] & 0xf0U) == 0xe0U) {
		count = 3;
		value = bytes[0] & 0x0fU;
	} else if ((bytes[0] & 0xf8U) == 0xf0U) {
		count = 4;
		value = bytes[0] & 0x07U;
	} else {
		*code = KANA_REPLACEMENT;
		return 1;
	}

	/* A character cut short by the end of the text is malformed. */
	if (count > length) {
		*code = KANA_REPLACEMENT;
		return 1;
	}

	/* Gathers the continuation bytes, each of which must be one. */
	for (i = 1; i < count; i++) {
		if ((bytes[i] & 0xc0U) != 0x80U) {
			*code = KANA_REPLACEMENT;
			return 1;
		}

		value = (value << 6) | (bytes[i] & 0x3fU);
	}

	/* Succeeded: one character read. */
	*code = value;
	return count;
}

/*
 * Tells whether two runs of bytes are the same.
 */
bool
ja_bytes_equal(
	const char *left,
	size_t left_length,
	const char *right,
	size_t right_length)
{
	int order;

	/* Runs of different lengths differ. */
	if (left_length != right_length)
		return false;

	/* Compares the bytes. */
	order = memcmp(left, right, left_length);
	if (order != 0)
		return false;

	/* The same bytes. */
	return true;
}

/*
 * Tells whether a code point is a hiragana (ぁ to ゖ).
 */
bool
ja_is_hiragana(
	uint32_t code)
{
	/* Below the block. */
	if (code < KANA_HIRAGANA_FIRST)
		return false;

	/* Above the block. */
	if (code > KANA_HIRAGANA_LAST)
		return false;

	/* Within it. */
	return true;
}

/*
 * Tells whether no word can begin with a kana: the long-vowel mark, the
 * small kana, っ and ん only follow another kana.
 */
bool
ja_starts_no_word(
	uint32_t code)
{
	/* The kana that only follow another. */
	switch (code) {
	case 0x30fcU:
		/* ー */
	case 0x3063U:
		/* っ */
	case 0x3093U:
		/* ん */
	case 0x3041U:
	case 0x3043U:
	case 0x3045U:
	case 0x3047U:
	case 0x3049U:
		/* ぁ ぃ ぅ ぇ ぉ */
	case 0x3083U:
	case 0x3085U:
	case 0x3087U:
	case 0x308eU:
		/* ゃ ゅ ょ ゎ */
		return true;
	default:
		break;
	}

	/* Any other kana can begin a word. */
	return false;
}

/*
 * Tells whether a kana marks a word taken from another language: the
 * long-vowel mark and the small vowels (コーヒー, ファイル), which native
 * words seldom have.
 */
bool
ja_is_loanword_mark(
	uint32_t code)
{
	/* ー and the small vowels. */
	switch (code) {
	case 0x30fcU:
	case 0x3041U:
	case 0x3043U:
	case 0x3045U:
	case 0x3047U:
	case 0x3049U:
		return true;
	default:
		break;
	}

	/* Any other kana. */
	return false;
}

/*
 * Gives the katakana of a hiragana; any other code point is given back
 * unchanged.
 */
uint32_t
ja_to_katakana(
	uint32_t code)
{
	bool hiragana;

	/* Only hiragana have a katakana a fixed distance away. */
	hiragana = ja_is_hiragana(code);
	if (!hiragana)
		return code;

	/* The katakana of the same sound. */
	return code + KANA_KATAKANA_OFFSET;
}

/*
 * Gives the half-width katakana of a hiragana or of a Japanese mark, as
 * UTF-8; NULL for a code point that has none.
 */
const char *
ja_to_half_katakana(
	uint32_t code)
{
	bool hiragana;

	/* A hiragana has its entry in the table. */
	hiragana = ja_is_hiragana(code);
	if (hiragana)
		return kana_half[code - KANA_HIRAGANA_FIRST];

	/* The marks that have half-width forms. */
	switch (code) {
	case 0x30fcU:
		/* ー */
		return "\xef\xbd\xb0";
	case 0x3001U:
		/* 、 */
		return "\xef\xbd\xa4";
	case 0x3002U:
		/* 。 */
		return "\xef\xbd\xa1";
	case 0x300cU:
		/* 「 */
		return "\xef\xbd\xa2";
	case 0x300dU:
		/* 」 */
		return "\xef\xbd\xa3";
	case 0x30fbU:
		/* ・ */
		return "\xef\xbd\xa5";
	default:
		break;
	}

	/* Nothing else has a half-width katakana. */
	return NULL;
}

/*
 * Gives the full-width form of a printable ASCII character or of the
 * space; any other code point is given back unchanged.
 */
uint32_t
ja_to_full_ascii(
	uint32_t code)
{
	/* The space becomes the ideographic space. */
	if (code == KANA_ASCII_SPACE)
		return KANA_FULL_SPACE;

	/* Below the printable range. */
	if (code < KANA_ASCII_FIRST)
		return code;

	/* Above it. */
	if (code > KANA_ASCII_LAST)
		return code;

	/* The full-width letter the same distance into its block. */
	return KANA_FULL_FIRST + (code - KANA_ASCII_FIRST);
}

/*
 * Gives the letter a kana is typed with as the first letter of an
 * okurigana (the last letter of a verb's headword); 0 for a kana that
 * begins no okurigana.
 */
char
ja_kana_consonant(
	uint32_t code)
{
	const struct kana_sound *sound;

	/* Looks the kana up. */
	sound = kana_find_sound(code);
	if (sound == NULL)
		return 0;

	/* The letter it is typed with. */
	return sound->consonant;
}

/*
 * Gives the vowel of a kana, 0 to 4 for a, i, u, e, o; -1 for a kana with
 * no vowel of its own or not known.
 */
int
ja_kana_vowel(
	uint32_t code)
{
	const struct kana_sound *sound;

	/* Looks the kana up. */
	sound = kana_find_sound(code);
	if (sound == NULL)
		return -1;

	/* Its vowel. */
	return sound->vowel;
}

/*
 * Gives the kana a godan verb whose headword ends with a consonant takes
 * for a vowel; 0 when there is no such row.
 */
uint32_t
ja_godan_kana(
	char consonant,
	int vowel)
{
	size_t i;

	/* A vowel outside a, i, u, e, o has no kana. */
	if (vowel < 0 || vowel > 4)
		return 0;

	/* Finds the row of the consonant. */
	for (i = 0; i < sizeof(kana_rows) / sizeof(kana_rows[0]); i++) {
		if (kana_rows[i].consonant == consonant)
			return kana_rows[i].kana[vowel];
	}

	/* No godan verb conjugates through this consonant. */
	return 0;
}

/*
 * Finds a kana's entry in the table of sounds.
 */
static const struct kana_sound *
kana_find_sound(
	uint32_t code)
{
	size_t low;
	size_t high;
	size_t middle;

	/* Halves the table, which is in code order, until the kana is found. */
	low = 0;
	high = sizeof(kana_sounds) / sizeof(kana_sounds[0]);
	while (low < high) {
		middle = low + (high - low) / 2U;

		/* The kana is found. */
		if (kana_sounds[middle].code == code)
			return &kana_sounds[middle];

		/* Goes on in the half that can hold the kana. */
		if (kana_sounds[middle].code < code) {
			low = middle + 1U;
		} else {
			high = middle;
		}
	}

	/* The kana begins no okurigana. */
	return NULL;
}
