/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * How verbs and adjectives end, and the particles after a word
 * (plan/ws095/design.md sections 7.2 and 7.2.1).
 *
 * The SKK dictionary's headwords are built from the dictionary form alone
 * (書く is かk, 食べる たb, 見る みr, 高い たかi and たかk), so the other
 * forms are made here by rule.  After a headword's reading, the letter
 * that ends it is tried as:
 *
 *   - a godan verb conjugating through that letter's row, with the sound
 *     changes of the past (書いた, 泳いだ, 話した, 読んだ, 取った);
 *   - a first kana typed with that letter followed by an ichidan ending
 *     (食べる), a godan ending of the ら row (上がる) or an adjective
 *     ending (難しい);
 *   - for r, an ichidan ending straight after the reading (見る);
 *   - for i and k, an adjective ending straight after it (高い).
 *
 * The endings are a small automaton: each state lists the kana that may
 * follow and the state they lead to, so that ています or ませんでした are
 * made of their parts.  する and 来る have their own states, whose stems
 * し, き and こ take only the endings each really takes.  The rules
 * accept more than Japanese does; the segmenter's costs and the
 * dictionary decide among what they accept.
 */

#include "ja.h"

#include <string.h>

/*
 * A state of the endings' automaton.
 */
enum inflect_tail {
	TAIL_NONE,
	TAIL_END,
	TAIL_NEGATIVE,
	TAIL_ICHIDAN,
	TAIL_CONTINUATIVE,
	TAIL_TE,
	TAIL_E_ROW,
	TAIL_O_ROW,
	TAIL_ADJECTIVE,
	TAIL_PAST,
	TAIL_PAST_VOICED,
	TAIL_SURU,
	TAIL_SURU_STEM,
	TAIL_KURU,
	TAIL_KURU_KI,
	TAIL_KURU_KO
};

/*
 * One ending a state accepts, in UTF-8, and the state after it.
 */
struct inflect_suffix {
	const char *text;
	enum inflect_tail next;
};

/*
 * One state: whether the ending may stop there, what may follow, and a
 * state whose endings may follow as well.
 */
struct inflect_state {
	enum inflect_tail tail;
	bool accepts_empty;
	const struct inflect_suffix *suffixes;
	enum inflect_tail also;
};

/* The endings of the negative (書かない, 食べない). */
static const struct inflect_suffix inflect_negative[] = {
	{ "ない", TAIL_END }, { "なかった", TAIL_END }, { "なかったら", TAIL_END }, { "なくて", TAIL_END },
	{ "なければ", TAIL_END }, { "なく", TAIL_END }, { "なきゃ", TAIL_END }, { "ず", TAIL_END }, { "ずに", TAIL_END },
	{ "ないで", TAIL_END }, { "ないでください", TAIL_END },
	{ "れ", TAIL_ICHIDAN }, { "せ", TAIL_ICHIDAN },
	{ NULL, TAIL_NONE }
};

/* The endings after an ichidan verb's stem (食べ, 見), which a passive, causative or potential also takes. */
static const struct inflect_suffix inflect_ichidan[] = {
	{ "る", TAIL_END }, { "れば", TAIL_END }, { "よう", TAIL_END }, { "ろ", TAIL_END }, { "ながら", TAIL_END },
	{ "ない", TAIL_END }, { "なかった", TAIL_END }, { "なかったら", TAIL_END }, { "なくて", TAIL_END },
	{ "なければ", TAIL_END }, { "なく", TAIL_END }, { "ず", TAIL_END }, { "ずに", TAIL_END },
	{ "ないで", TAIL_END }, { "ないでください", TAIL_END },
	{ "ます", TAIL_END }, { "ました", TAIL_END }, { "ません", TAIL_END }, { "ませんでした", TAIL_END },
	{ "ましょう", TAIL_END }, { "まして", TAIL_END },
	{ "たい", TAIL_END }, { "たかった", TAIL_END }, { "たくない", TAIL_END }, { "たくて", TAIL_END },
	{ "たければ", TAIL_END }, { "たく", TAIL_END },
	{ "た", TAIL_END }, { "たら", TAIL_END }, { "たり", TAIL_END }, { "て", TAIL_TE },
	{ "られ", TAIL_ICHIDAN }, { "させ", TAIL_ICHIDAN },
	{ NULL, TAIL_NONE }
};

/* The endings after a godan verb's continuative (書き). */
static const struct inflect_suffix inflect_continuative[] = {
	{ "ます", TAIL_END }, { "ました", TAIL_END }, { "ません", TAIL_END }, { "ませんでした", TAIL_END },
	{ "ましょう", TAIL_END }, { "まして", TAIL_END },
	{ "たい", TAIL_END }, { "たかった", TAIL_END }, { "たくない", TAIL_END }, { "たくて", TAIL_END },
	{ "たければ", TAIL_END }, { "たく", TAIL_END }, { "ながら", TAIL_END },
	{ NULL, TAIL_NONE }
};

/* The endings after the te form (書いて, 食べて). */
static const struct inflect_suffix inflect_te[] = {
	{ "い", TAIL_ICHIDAN }, { "る", TAIL_END }, { "た", TAIL_END }, { "ます", TAIL_END }, { "ない", TAIL_END },
	{ "ました", TAIL_END }, { "ください", TAIL_END }, { "しまう", TAIL_END }, { "しまった", TAIL_END },
	{ "おく", TAIL_END }, { "みる", TAIL_END }, { "も", TAIL_END }, { "は", TAIL_END },
	{ "きます", TAIL_END }, { "きました", TAIL_END }, { "きた", TAIL_END }, { "くる", TAIL_END },
	{ "いく", TAIL_END }, { "いきます", TAIL_END }, { "いった", TAIL_END },
	{ "あります", TAIL_END }, { "ある", TAIL_END }, { "おきます", TAIL_END }, { "みます", TAIL_END },
	{ NULL, TAIL_NONE }
};

/* The endings after a godan verb's e row: the conditional (書けば); the potential goes on as ichidan. */
static const struct inflect_suffix inflect_e_row[] = {
	{ "ば", TAIL_END },
	{ NULL, TAIL_NONE }
};

/* The ending after a godan verb's o row: the volitional (書こう). */
static const struct inflect_suffix inflect_o_row[] = {
	{ "う", TAIL_END },
	{ NULL, TAIL_NONE }
};

/* The endings of an adjective after its stem (高, 難し). */
static const struct inflect_suffix inflect_adjective[] = {
	{ "い", TAIL_END }, { "く", TAIL_END }, { "くて", TAIL_END }, { "くない", TAIL_END }, { "くなかった", TAIL_END },
	{ "くなる", TAIL_END }, { "くなった", TAIL_END }, { "かった", TAIL_END }, { "かったら", TAIL_END },
	{ "ければ", TAIL_END }, { "さ", TAIL_END }, { "そう", TAIL_END }, { "そうな", TAIL_END }, { "そうに", TAIL_END },
	{ "すぎる", TAIL_END }, { "すぎた", TAIL_END },
	{ NULL, TAIL_NONE }
};

/* The endings after the sound change of the past (書い, 取っ, 話し). */
static const struct inflect_suffix inflect_past[] = {
	{ "た", TAIL_END }, { "たら", TAIL_END }, { "たり", TAIL_END }, { "て", TAIL_TE },
	{ NULL, TAIL_NONE }
};

/* The endings after the voiced sound change of the past (読ん, 泳い). */
static const struct inflect_suffix inflect_past_voiced[] = {
	{ "だ", TAIL_END }, { "だら", TAIL_END }, { "だり", TAIL_END }, { "で", TAIL_TE },
	{ NULL, TAIL_NONE }
};

/* The forms of する. */
static const struct inflect_suffix inflect_suru[] = {
	{ "し", TAIL_SURU_STEM }, { "する", TAIL_END }, { "すれば", TAIL_END }, { "すべき", TAIL_END },
	{ "され", TAIL_ICHIDAN }, { "させ", TAIL_ICHIDAN }, { "せず", TAIL_END },
	{ NULL, TAIL_NONE }
};

/*
 * The endings after する's stem し (します, しない, しよう, しろ): an
 * ichidan verb's, without る and れば, which する spells する and すれば, and
 * without られ and させ, which it spells され and させ (知る, not しる).
 */
static const struct inflect_suffix inflect_suru_stem[] = {
	{ "よう", TAIL_END }, { "ろ", TAIL_END }, { "ながら", TAIL_END },
	{ "ない", TAIL_END }, { "なかった", TAIL_END }, { "なかったら", TAIL_END }, { "なくて", TAIL_END },
	{ "なければ", TAIL_END }, { "なく", TAIL_END }, { "ないで", TAIL_END }, { "ないでください", TAIL_END },
	{ "ます", TAIL_END }, { "ました", TAIL_END }, { "ません", TAIL_END }, { "ませんでした", TAIL_END },
	{ "ましょう", TAIL_END }, { "まして", TAIL_END },
	{ "たい", TAIL_END }, { "たかった", TAIL_END }, { "たくない", TAIL_END }, { "たくて", TAIL_END },
	{ "たければ", TAIL_END }, { "たく", TAIL_END },
	{ "た", TAIL_END }, { "たら", TAIL_END }, { "たり", TAIL_END }, { "て", TAIL_TE },
	{ NULL, TAIL_NONE }
};

/* The forms of 来る. */
static const struct inflect_suffix inflect_kuru[] = {
	{ "き", TAIL_KURU_KI }, { "こ", TAIL_KURU_KO }, { "くる", TAIL_END }, { "くれば", TAIL_END },
	{ NULL, TAIL_NONE }
};

/*
 * The endings after 来る's continuative き (来ます, 来た, 来て): the polite
 * forms, the wish, the past and the te form; き takes no る (切る, not 来る).
 */
static const struct inflect_suffix inflect_kuru_ki[] = {
	{ "ます", TAIL_END }, { "ました", TAIL_END }, { "ません", TAIL_END }, { "ませんでした", TAIL_END },
	{ "ましょう", TAIL_END }, { "まして", TAIL_END },
	{ "たい", TAIL_END }, { "たかった", TAIL_END }, { "たくない", TAIL_END }, { "たくて", TAIL_END },
	{ "たければ", TAIL_END }, { "たく", TAIL_END }, { "ながら", TAIL_END },
	{ "た", TAIL_END }, { "たら", TAIL_END }, { "たり", TAIL_END }, { "て", TAIL_TE },
	{ NULL, TAIL_NONE }
};

/*
 * The endings after 来る's irrealis こ (来ない, 来よう, 来られる, 来い); こ
 * takes no polite or past ending (頃, not 来ろ).
 */
static const struct inflect_suffix inflect_kuru_ko[] = {
	{ "ない", TAIL_END }, { "なかった", TAIL_END }, { "なかったら", TAIL_END }, { "なくて", TAIL_END },
	{ "なければ", TAIL_END }, { "なく", TAIL_END }, { "ず", TAIL_END }, { "ずに", TAIL_END },
	{ "ないで", TAIL_END }, { "ないでください", TAIL_END },
	{ "よう", TAIL_END }, { "い", TAIL_END },
	{ "られ", TAIL_ICHIDAN }, { "させ", TAIL_ICHIDAN },
	{ NULL, TAIL_NONE }
};

/*
 * Every state of the automaton.
 */
static const struct inflect_state inflect_states[] = {
	{ TAIL_NEGATIVE, false, inflect_negative, TAIL_NONE },
	{ TAIL_ICHIDAN, true, inflect_ichidan, TAIL_NONE },
	{ TAIL_CONTINUATIVE, true, inflect_continuative, TAIL_NONE },
	{ TAIL_TE, true, inflect_te, TAIL_NONE },
	{ TAIL_E_ROW, true, inflect_e_row, TAIL_ICHIDAN },
	{ TAIL_O_ROW, false, inflect_o_row, TAIL_NONE },
	{ TAIL_ADJECTIVE, false, inflect_adjective, TAIL_NONE },
	{ TAIL_PAST, false, inflect_past, TAIL_NONE },
	{ TAIL_PAST_VOICED, false, inflect_past_voiced, TAIL_NONE },
	{ TAIL_SURU, false, inflect_suru, TAIL_NONE },
	{ TAIL_SURU_STEM, false, inflect_suru_stem, TAIL_NONE },
	{ TAIL_KURU, false, inflect_kuru, TAIL_NONE },
	{ TAIL_KURU_KI, false, inflect_kuru_ki, TAIL_NONE },
	{ TAIL_KURU_KO, false, inflect_kuru_ko, TAIL_NONE }
};

/*
 * A verb usually written in kana: its stem and whether it is ichidan (いる)
 * or a godan verb of the ら row (ある).  SKK files them under headwords
 * shared with other verbs (いr is 居る, 要る and 入れる), so they are
 * known here instead, written as typed.
 */
struct inflect_kana_verb {
	const char *stem;
	bool ichidan;
};

/*
 * The verbs written in kana: いる, ある, なる and できる.
 */
static const struct inflect_kana_verb inflect_kana_verbs[] = {
	{ "い", true },
	{ "あ", false },
	{ "な", false },
	{ "でき", true }
};

/*
 * The particles and copulas a word may take, the usual pairs (には, では,
 * への) among them as one entry, so that particles do not string together
 * into a segment of their own making (に, の and って would swallow
 * のって).
 */
static const char *const inflect_particles[] = {
	"は", "が", "を", "に", "へ", "と", "で", "も", "の", "や", "か", "ね", "よ", "な", "ば", "わ",
	"から", "まで", "より", "けど", "けれど", "ので", "のに", "だけ", "しか", "って",
	"でも", "など", "ほど", "くらい", "ぐらい", "ずつ", "こそ", "さえ", "とか", "かも", "ばかり",
	"には", "では", "へは", "への", "とは", "との", "での", "からは", "からの", "までに", "までの", "にも", "とも",
	"のは", "のが", "のを", "のも", "だけで", "だけが", "だけを", "しかない", "について", "として", "にとって",
	"です", "でした", "でしょう", "だ", "だった", "だろう", "じゃ", "じゃない", "ではない", "ではありません",
	"のです", "のでしょう",
	NULL
};

/*
 * The copulas and the particles after which a particle ending the
 * sentence may follow (ですね, だよ, のか); after a case particle it may
 * not (を and か would make をか).
 */
static const char *const inflect_final_hosts[] = {
	"の", "です", "でした", "でしょう", "だ", "だった", "だろう", "じゃない", "ではない", "ではありません", "けど",
	"のです", "のでしょう", "から", "まで", "だけ",
	NULL
};

/*
 * The words that may follow one of the hosts above within a sentence
 * (からです, のですが).
 */
static const char *const inflect_finals[] = {
	"けど", "が", "から", "ので", "です", "でした", "だ", "でしょう",
	NULL
};

/*
 * The particles that end a sentence, after a word, a particle or one of
 * the hosts above; only at the end of what is typed or before a mark
 * (行くね, ですか), so that the よ of 予定 is not taken as one (明日のよ).
 */
static const char *const inflect_sentence_ends[] = {
	"ね", "よ", "か", "な", "わ", "よね", "かな", "ですか", "ですね", "でしたか", "だよ",
	NULL
};

/* The godan state each vowel of a godan verb's last kana leads to. */
static const enum inflect_tail inflect_vowel_tails[5] = {
	TAIL_NEGATIVE,
	TAIL_CONTINUATIVE,
	TAIL_END,
	TAIL_E_ROW,
	TAIL_O_ROW
};

static void inflect_sentence_end(const struct ja_text *text, size_t start, bool *ends);
static void inflect_walk(const struct ja_text *text, size_t position, enum inflect_tail tail, bool *ends);
static const struct inflect_state *inflect_find_state(enum inflect_tail tail);
static void inflect_godan(const struct ja_text *text, size_t position, char consonant, bool *ends);
static void inflect_after_first_kana(const struct ja_text *text, size_t position, char consonant, bool *ends);
static uint32_t inflect_code_at(const struct ja_text *text, size_t unit);

/*
 * Marks where the okurigana after a headword's reading can end.
 *
 * The reading ends before unit stem_end, and the headword's last letter
 * is the consonant.  An adjective's headword (高い has たかi and たかk)
 * takes the adjective's endings alone, so that 辛 (からk) does not take a
 * verb's (から + きました).  Each unit an okurigana can end before is
 * marked in ends, which has room for every unit and the end, and which the
 * caller has cleared.
 */
void
ja_inflect_ends(
	const struct ja_text *text,
	size_t stem_end,
	char consonant,
	bool adjective,
	bool *ends)
{
	uint32_t code;
	uint32_t next_code;
	char first_letter;
	char next_letter;

	/* No okurigana fits after the last unit. */
	if (stem_end >= text->unit_count)
		return;

	/* An adjective ends only as one. */
	if (adjective) {
		inflect_walk(text, stem_end, TAIL_ADJECTIVE, ends);
		ends[stem_end] = false;
		return;
	}

	/* The headword's letter as the row of a godan verb (書く, 読む, 買う). */
	inflect_godan(text, stem_end, consonant, ends);

	/*
	 * The letter as the first kana of a longer okurigana (食べる, 上がる,
	 * 難しい); a doubled consonant is typed with the next kana's letter
	 * (引っかかる).
	 */
	code = inflect_code_at(text, stem_end);
	next_code = inflect_code_at(text, stem_end + 1U);
	first_letter = ja_kana_consonant(code);
	next_letter = ja_kana_consonant(next_code);
	if (code == 0x3063U) {
		if (next_letter == consonant)
			inflect_after_first_kana(text, stem_end + 2U, consonant, ends);
	} else if (first_letter == consonant) {
		inflect_after_first_kana(text, stem_end + 1U, consonant, ends);
	}

	/* An ichidan verb whose okurigana is る alone (見る, 出る). */
	if (consonant == 'r')
		inflect_walk(text, stem_end, TAIL_ICHIDAN, ends);

	/* An adjective whose okurigana is its ending alone (高い, 高く). */
	if (consonant == 'i' || consonant == 'k')
		inflect_walk(text, stem_end, TAIL_ADJECTIVE, ends);

	/* An okurigana is never empty. */
	ends[stem_end] = false;
}

/*
 * Marks where the okurigana after a headword's reading can end for a
 * candidate whose conjugation the dictionary names.
 *
 * The reading holds every kana of the stem that does not change: a godan
 * verb conjugates through the row of the headword's letter straight after
 * it (かえr 帰: 帰ります), an ichidan verb's and an adjective's endings
 * follow it straight (かえr 変え: 変えます, たかi 高: 高かった).  A
 * candidate whose conjugation is not named takes every okurigana
 * ja_inflect_ends() knows.  The ends are marked as ja_inflect_ends()
 * marks them.
 */
void
ja_inflect_conjugated_ends(
	const struct ja_text *text,
	size_t stem_end,
	char consonant,
	enum ja_conjugation conjugation,
	bool *ends)
{
	/* No okurigana fits after the last unit. */
	if (stem_end >= text->unit_count)
		return;

	/* Follows the endings of the conjugation named. */
	switch (conjugation) {
	case JA_CONJUGATION_GODAN:
		/* The row of the headword's letter, with its sound changes. */
		inflect_godan(text, stem_end, consonant, ends);
		break;
	case JA_CONJUGATION_ICHIDAN:
		/* An ichidan verb's dictionary form ends in る, its headword in r. */
		if (consonant == 'r')
			inflect_walk(text, stem_end, TAIL_ICHIDAN, ends);
		break;
	case JA_CONJUGATION_ADJECTIVE:
		/* An adjective's dictionary form ends in い, its headword in i or k. */
		if (consonant == 'i' || consonant == 'k')
			inflect_walk(text, stem_end, TAIL_ADJECTIVE, ends);
		break;
	case JA_CONJUGATION_ANY:
	default:
		/* Every rule, as for a headword of SKK-JISYO.X. */
		ja_inflect_ends(text, stem_end, consonant, false, ends);
		break;
	}

	/* An okurigana is never empty. */
	ends[stem_end] = false;
}

/*
 * Marks where a form of する beginning at a unit can end.
 */
void
ja_inflect_suru_ends(
	const struct ja_text *text,
	size_t start,
	bool *ends)
{
	/* Walks the forms of する. */
	inflect_walk(text, start, TAIL_SURU, ends);

	/* Nothing is no form. */
	ends[start] = false;

	/* The stem し alone is no word of its own. */
	if (start + 1U <= text->unit_count)
		ends[start + 1U] = false;
}

/*
 * Marks where a form of 来る beginning at a unit can end.
 */
void
ja_inflect_kuru_ends(
	const struct ja_text *text,
	size_t start,
	bool *ends)
{
	/* Walks the forms of 来る. */
	inflect_walk(text, start, TAIL_KURU, ends);

	/* Nothing is no form. */
	ends[start] = false;

	/* The stems き and こ alone are no word of their own (木, 子). */
	if (start + 1U <= text->unit_count)
		ends[start + 1U] = false;
}

/*
 * Marks where a verb written in kana (いる, ある, なる, できる) beginning at
 * a unit can end.
 */
void
ja_inflect_kana_verb_ends(
	const struct ja_text *text,
	size_t start,
	bool *ends)
{
	size_t i;
	size_t stem_end;
	bool matched;

	/* Each verb whose stem is typed here, with its endings. */
	for (i = 0; i < sizeof(inflect_kana_verbs) / sizeof(inflect_kana_verbs[0]); i++) {
		matched = ja_text_match(text, start, inflect_kana_verbs[i].stem, &stem_end);
		if (!matched)
			continue;

		/* An ichidan verb takes its endings after the stem; a godan one through the ら row. */
		if (inflect_kana_verbs[i].ichidan) {
			inflect_walk(text, stem_end, TAIL_ICHIDAN, ends);
		} else if (stem_end < text->unit_count) {
			inflect_godan(text, stem_end, 'r', ends);
		}

		/* The stem alone is no form. */
		ends[stem_end] = false;
	}

	/* Nothing is no form. */
	ends[start] = false;
}

/*
 * Gives where the stem of a verb written in kana ends, when such a verb
 * beginning at a unit ends at another; the start itself when none does.
 */
size_t
ja_inflect_kana_verb_stem(
	const struct ja_text *text,
	size_t start,
	size_t end)
{
	bool ends[JA_UNITS_MAX + 1U];
	size_t i;
	size_t stem_end;
	bool matched;

	/* Tries each verb alone. */
	for (i = 0; i < sizeof(inflect_kana_verbs) / sizeof(inflect_kana_verbs[0]); i++) {
		matched = ja_text_match(text, start, inflect_kana_verbs[i].stem, &stem_end);
		if (!matched)
			continue;

		/* The verb's endings from its stem. */
		memset(ends, 0, sizeof(ends));
		if (inflect_kana_verbs[i].ichidan) {
			inflect_walk(text, stem_end, TAIL_ICHIDAN, ends);
		} else if (stem_end < text->unit_count) {
			inflect_godan(text, stem_end, 'r', ends);
		}

		/* This verb ends there. */
		if (stem_end < end && ends[end])
			return stem_end;
	}

	/* No verb written in kana ends there. */
	return start;
}

/*
 * Marks where the particles after a word beginning at a unit can end: none,
 * one particle (or usual pair), and one particle that ends a sentence
 * after it.  The start itself is marked, for a word that takes none.
 */
void
ja_particle_ends(
	const struct ja_text *text,
	size_t start,
	bool *ends)
{
	size_t i;
	size_t j;
	size_t end;
	size_t final_end;
	bool matched;
	bool host;
	bool same;

	/* No particle at all, or only one that ends the sentence. */
	ends[start] = true;
	inflect_sentence_end(text, start, ends);

	/* One particle, what may follow a copula or の, and one ending the sentence. */
	for (i = 0; inflect_particles[i] != NULL; i++) {
		matched = ja_text_match(text, start, inflect_particles[i], &end);
		if (!matched)
			continue;

		ends[end] = true;
		inflect_sentence_end(text, end, ends);

		/* What follows within the sentence comes after a copula or の only. */
		host = false;
		for (j = 0; inflect_final_hosts[j] != NULL; j++) {
			same = ja_bytes_equal(inflect_particles[i], strlen(inflect_particles[i]), inflect_final_hosts[j],
					      strlen(inflect_final_hosts[j]));
			if (same)
				host = true;
		}

		if (!host)
			continue;

		/* A word following the copula or の, and one ending the sentence after it. */
		for (j = 0; inflect_finals[j] != NULL; j++) {
			matched = ja_text_match(text, end, inflect_finals[j], &final_end);
			if (!matched)
				continue;

			ends[final_end] = true;
			inflect_sentence_end(text, final_end, ends);
		}
	}
}

/*
 * Marks where a particle ending the sentence at a unit ends, when it ends
 * the text or comes before a mark.
 */
static void
inflect_sentence_end(
	const struct ja_text *text,
	size_t start,
	bool *ends)
{
	size_t i;
	size_t end;
	bool matched;

	/* Each particle that ends a sentence. */
	for (i = 0; inflect_sentence_ends[i] != NULL; i++) {
		matched = ja_text_match(text, start, inflect_sentence_ends[i], &end);
		if (!matched)
			continue;

		/* Only at the end of the text or before a literal (a mark). */
		if (end == text->unit_count || !text->kana[end])
			ends[end] = true;
	}
}

/*
 * Tells whether a span is exactly one particle.
 */
bool
ja_is_particle(
	const struct ja_text *text,
	size_t start,
	size_t end)
{
	size_t i;
	size_t match_end;
	bool matched;

	/* Tries each particle over the whole span. */
	for (i = 0; inflect_particles[i] != NULL; i++) {
		matched = ja_text_match(text, start, inflect_particles[i], &match_end);
		if (!matched)
			continue;

		/* The particle covers the span exactly. */
		if (match_end == end)
			return true;
	}

	/* The span is no single particle. */
	return false;
}

/*
 * Tells whether some kana follow at a unit, and where they end.
 *
 * Only kana match: a literal never takes part in a word.
 */
bool
ja_text_match(
	const struct ja_text *text,
	size_t start,
	const char *suffix,
	size_t *end)
{
	size_t length;
	size_t byte_start;
	size_t byte_end;
	size_t unit;
	bool same;

	/* Nothing follows the last unit. */
	if (start >= text->unit_count)
		return false;

	/* The suffix must fit in what is left of the text. */
	length = strlen(suffix);
	byte_start = text->offsets[start];
	if (length > text->length - byte_start)
		return false;

	/* The same bytes. */
	same = ja_bytes_equal(text->bytes + byte_start, length, suffix, length);
	if (!same)
		return false;

	/* Finds the unit the match ends before, checking each one is a kana. */
	byte_end = byte_start + length;
	unit = start;
	while (unit < text->unit_count && text->offsets[unit] < byte_end) {
		if (!text->kana[unit])
			return false;

		unit++;
	}

	/* A match that ends inside a character is no match. */
	if (text->offsets[unit] != byte_end)
		return false;

	/* Succeeded: the suffix ends before this unit. */
	*end = unit;
	return true;
}

/*
 * Follows the automaton from a state at a unit, marking where it may stop.
 */
static void
inflect_walk(
	const struct ja_text *text,
	size_t position,
	enum inflect_tail tail,
	bool *ends)
{
	const struct inflect_state *state;
	size_t i;
	size_t end;
	bool matched;

	/* The end state stops here. */
	if (tail == TAIL_END) {
		ends[position] = true;
		return;
	}

	/* An unknown state accepts nothing. */
	state = inflect_find_state(tail);
	if (state == NULL)
		return;

	/* A state that may stop marks its own place. */
	if (state->accepts_empty)
		ends[position] = true;

	/* Follows each ending that is typed next. */
	for (i = 0; state->suffixes[i].text != NULL; i++) {
		matched = ja_text_match(text, position, state->suffixes[i].text, &end);
		if (!matched)
			continue;

		/* Goes on from after the ending. */
		inflect_walk(text, end, state->suffixes[i].next, ends);
	}

	/* A state whose endings are shared with another goes on into it. */
	if (state->also != TAIL_NONE)
		inflect_walk(text, position, state->also, ends);
}

/*
 * Finds a state's description.
 */
static const struct inflect_state *
inflect_find_state(
	enum inflect_tail tail)
{
	size_t i;

	/* Looks through the states. */
	for (i = 0; i < sizeof(inflect_states) / sizeof(inflect_states[0]); i++) {
		if (inflect_states[i].tail == tail)
			return &inflect_states[i];
	}

	/* No such state. */
	return NULL;
}

/*
 * Marks where a godan verb conjugating through a consonant's row can end,
 * its last kana being at a unit.
 */
static void
inflect_godan(
	const struct ja_text *text,
	size_t position,
	char consonant,
	bool *ends)
{
	uint32_t code;
	uint32_t previous;
	uint32_t row_kana;
	int vowel;

	/* The verb's last kana, and the kana before it (い of 行く). */
	code = inflect_code_at(text, position);
	previous = 0;
	if (position >= 1U)
		previous = inflect_code_at(text, position - 1U);

	/* The five kana of the row, each with its endings. */
	for (vowel = 0; vowel < 5; vowel++) {
		row_kana = ja_godan_kana(consonant, vowel);
		if (row_kana == 0U || row_kana != code)
			continue;

		/* The ending after this kana. */
		inflect_walk(text, position + 1U, inflect_vowel_tails[vowel], ends);
	}

	/* The sound changes of the past, by the row. */
	switch (consonant) {
	case 'k':
		/* 書いた; 行く alone takes っ (行った). */
		if (code == 0x3044U)
			inflect_walk(text, position + 1U, TAIL_PAST, ends);

		/* The っ of 行った, after the い of 行. */
		if (code == 0x3063U && previous == 0x3044U)
			inflect_walk(text, position + 1U, TAIL_PAST, ends);
		break;
	case 'g':
		/* 泳いだ. */
		if (code == 0x3044U)
			inflect_walk(text, position + 1U, TAIL_PAST_VOICED, ends);
		break;
	case 's':
		/* 話した. */
		if (code == 0x3057U)
			inflect_walk(text, position + 1U, TAIL_PAST, ends);
		break;
	case 't':
	case 'r':
	case 'w':
		/* 持った, 取った, 買った. */
		if (code == 0x3063U)
			inflect_walk(text, position + 1U, TAIL_PAST, ends);
		break;
	case 'n':
	case 'b':
	case 'm':
		/* 死んだ, 飛んだ, 読んだ. */
		if (code == 0x3093U)
			inflect_walk(text, position + 1U, TAIL_PAST_VOICED, ends);
		break;
	default:
		break;
	}
}

/*
 * Marks where an okurigana can end whose first kana, typed with the
 * headword's consonant, ends before a unit.
 */
static void
inflect_after_first_kana(
	const struct ja_text *text,
	size_t position,
	char consonant,
	bool *ends)
{
	int vowel;

	UNUSED_PARAMETER(consonant);

	/* An ichidan verb, whose stem ends in an i or e kana (食べる, 用いる). */
	vowel = ja_kana_vowel(inflect_code_at(text, position - 1U));
	if (vowel == 1 || vowel == 3)
		inflect_walk(text, position, TAIL_ICHIDAN, ends);

	/* A godan verb of the ら row after it (上がる, 変わる). */
	if (position < text->unit_count)
		inflect_godan(text, position, 'r', ends);

	/* An adjective ending after it (難しい, 大きい). */
	inflect_walk(text, position, TAIL_ADJECTIVE, ends);
}

/*
 * Gives the code point of a unit, or 0 past the end or for a literal.
 */
static uint32_t
inflect_code_at(
	const struct ja_text *text,
	size_t unit)
{
	uint32_t code;

	/* Nothing past the end. */
	if (unit >= text->unit_count)
		return 0;

	/* A literal is no kana. */
	if (!text->kana[unit])
		return 0;

	/* Decodes the unit's character. */
	(void)ja_utf8_decode(text->bytes + text->offsets[unit], text->offsets[unit + 1U] - text->offsets[unit], &code);
	return code;
}
