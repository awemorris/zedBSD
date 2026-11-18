/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Splitting a reading into segments, and each segment's candidates
 * (plan/ws095/design.md section 7.2).
 *
 * A segment is a word and the particles after it: a noun from a
 * dictionary, a verb or an adjective with its okurigana (ja-inflect.c),
 * する or 来る, a noun with する, a run of kana no dictionary knows, or a
 * run of literals.  The split is found by dynamic programming from left to
 * right, comparing the costs of the ways to reach each place in this
 * order: the fewest kana no dictionary knows and one-kana nouns counted
 * together (い 胃 and き 木 would otherwise swallow kana), then the fewest
 * segments, then the most kana in everyday words (the user's, the engine's
 * own and the supplement's) and particles together, then the most kana read
 * as particles, then the most kana found in a dictionary.
 */

#include "ja.h"

#include <stdlib.h>
#include <string.h>

/* The letters a verb's or an adjective's headword can end with. */
#define SEGMENT_CONSONANTS	"abdeghijkmnoprstuwyz"

/*
 * A word the dictionaries lack or get wrong that the engine always knows.
 */
struct segment_builtin {
	const char *reading;
	const char *candidates[3];
};

/*
 * The cost of the best way found so far to reach a place in the reading,
 * and the place its last segment started from.
 */
struct segment_cost {
	bool reached;
	unsigned int unknown;
	unsigned int single;
	unsigned int segments;
	unsigned int everyday;
	unsigned int particles;
	unsigned int dictionary;
	size_t previous;
};

/*
 * What one segment adds to a cost.
 */
struct segment_item {
	unsigned int unknown;
	unsigned int single;
	unsigned int everyday;
	unsigned int dictionary;
};

/*
 * The words the engine knows without a dictionary.
 */
static const struct segment_builtin segment_builtins[] = {
	{ "いい", { "いい", "良い", NULL } }
};

static bool *segment_particle_table(const struct ja_text *text, size_t start);
static bool segment_has_noun(const struct ja_lexicon *lexicon, const char *key, size_t length);
static bool segment_is_everyday_noun(const struct ja_lexicon *lexicon, const char *key, size_t length);
static bool segment_headword_ends(const struct ja_lexicon *lexicon, const struct ja_text *text, const char *key, size_t stem_length, size_t stem_end, bool *ends, bool *everyday_ends);
static void segment_entry_ends(const struct ja_lexicon *lexicon, const struct ja_dict_entry *entry, const struct ja_text *text, const char *key, size_t stem_length, size_t stem_end, bool *ends);
static bool segment_candidate_fits(const struct ja_lexicon *lexicon, const struct ja_text *text, const char *key, size_t stem_length, size_t stem_end, enum ja_conjugation conjugation, size_t core_end);
static bool segment_is_adjective(const struct ja_lexicon *lexicon, const char *stem, size_t length, char consonant);
static uint32_t segment_code_at(const struct ja_text *text, size_t unit);
static bool segment_is_loanword(const struct ja_text *text, size_t start, size_t end);
static bool segment_all_kana(const struct ja_text *text, size_t start, size_t end);
static void segment_item_better(struct segment_item *best, bool *valid, const struct segment_item *item);
static bool segment_cost_better(const struct segment_cost *left, const struct segment_cost *right);
static void segment_cores(const struct ja_lexicon *lexicon, const struct ja_text *text, size_t start, struct segment_item *cores, bool *valid);
static void segment_add_nouns(const struct ja_lexicon *lexicon, const char *key, size_t length, const char *suffix, size_t suffix_length, struct ja_segment *segment);
static void segment_add_dict_nouns(const struct ja_dict *dict, const char *key, size_t length, const char *suffix, size_t suffix_length, struct ja_segment *segment);
static void segment_add_dict_verbs(const struct ja_lexicon *lexicon, const struct ja_dict *dict, const struct ja_text *text, size_t start, size_t core_end, size_t end, struct ja_segment *segment);
static void segment_add_conjugated_entry(const struct ja_lexicon *lexicon, const struct ja_dict_entry *entry, const struct ja_text *text, const char *key, size_t stem_length, size_t stem_end, size_t core_end, size_t end, struct ja_segment *segment);
static void segment_add_dict_suru(const struct ja_dict *dict, const struct ja_text *text, size_t start, size_t core_end, size_t end, struct ja_segment *segment);
static void segment_add_entry(const struct ja_dict_entry *entry, const char *suffix, size_t suffix_length, struct ja_segment *segment);
static void segment_add_joined(struct ja_segment *segment, const char *word, size_t word_length, const char *suffix, size_t suffix_length);
static void segment_add_tier(const struct ja_lexicon *lexicon, const struct ja_text *text, size_t start, size_t core_end, size_t end, size_t tier, struct ja_segment *segment);
static void segment_add_katakana(const struct ja_text *text, size_t start, size_t core_end, size_t end, struct ja_segment *segment);
static void segment_add_full_width(const struct ja_text *text, size_t start, size_t end, struct ja_segment *segment);

/*
 * Builds the segmenter's view of a composition.
 */
void
ja_text_build(
	struct ja_text *text,
	const struct ja_unit *units,
	size_t unit_count)
{
	size_t i;
	size_t used;

	/* Writes each character and notes where it starts. */
	text->length = 0;
	for (i = 0; i < unit_count; i++) {
		text->offsets[i] = text->length;
		used = ja_utf8_encode(units[i].code, text->bytes + text->length);
		text->length += used;
		text->kana[i] = false;
		if (units[i].kind == JA_UNIT_KANA)
			text->kana[i] = true;
	}

	/* The end is a boundary as well. */
	text->offsets[unit_count] = text->length;
	text->bytes[text->length] = '\0';
	text->unit_count = unit_count;
}

/*
 * Splits a reading from a unit to its end into segments.
 *
 * Returns the number of spans written, in order.
 */
size_t
ja_segment_split(
	const struct ja_lexicon *lexicon,
	const struct ja_text *text,
	size_t start,
	struct ja_span *spans,
	size_t spans_max)
{
	struct segment_cost costs[JA_UNITS_MAX + 1U];
	struct segment_cost candidate;
	struct segment_item cores[JA_UNITS_MAX + 1U];
	bool valid[JA_UNITS_MAX + 1U];
	struct ja_span reversed[JA_UNITS_MAX];
	bool *particles;
	size_t count;
	size_t position;
	size_t core_end;
	size_t end;
	size_t row;
	size_t i;
	bool better;

	/* Nothing to split. */
	if (start >= text->unit_count)
		return 0;

	/* Where particles can end, from every place. */
	particles = segment_particle_table(text, start);
	if (particles == NULL) {
		/* Without memory the whole reading is one segment. */
		spans[0].start = start;
		spans[0].end = text->unit_count;
		return 1;
	}

	/* Only the start is reached before anything is split. */
	memset(costs, 0, sizeof(costs));
	costs[start].reached = true;

	/* From each place reached, tries every segment that can start there. */
	row = text->unit_count + 1U;
	for (position = start; position < text->unit_count; position++) {
		if (!costs[position].reached)
			continue;

		/* The words that can start here, by where each ends. */
		memset(valid, 0, sizeof(valid));
		segment_cores(lexicon, text, position, cores, valid);

		/* Each word with each run of particles after it is one segment. */
		for (core_end = position + 1U; core_end <= text->unit_count; core_end++) {
			if (!valid[core_end])
				continue;

			for (end = core_end; end <= text->unit_count; end++) {
				if (!particles[core_end * row + end])
					continue;

				/* The cost of reaching the end through this segment. */
				candidate = costs[position];
				candidate.unknown += cores[core_end].unknown;
				candidate.single += cores[core_end].single;
				candidate.segments++;
				candidate.everyday += cores[core_end].everyday;
				candidate.particles += (unsigned int)(end - core_end);
				candidate.dictionary += cores[core_end].dictionary;
				candidate.previous = position;
				candidate.reached = true;

				/* Keeps the cheaper way to the end. */
				better = segment_cost_better(&candidate, &costs[end]);
				if (better)
					costs[end] = candidate;
			}
		}

		/* A run of particles alone is a segment too (the は left after a shortened segment). */
		for (end = position + 1U; end <= text->unit_count; end++) {
			if (!particles[position * row + end])
				continue;

			/* The cost of reaching the end through the particles, which count as no word's particles. */
			candidate = costs[position];
			candidate.segments++;
			candidate.previous = position;
			candidate.reached = true;

			/* Keeps the cheaper way to the end. */
			better = segment_cost_better(&candidate, &costs[end]);
			if (better)
				costs[end] = candidate;
		}
	}

	free(particles);

	/* Walks back from the end along the cheapest way. */
	count = 0;
	end = text->unit_count;
	while (end > start && count < JA_UNITS_MAX) {
		reversed[count].start = costs[end].previous;
		reversed[count].end = end;
		count++;
		end = costs[end].previous;
	}

	/* Gives the spans in reading order, as many as fit. */
	if (count > spans_max)
		count = spans_max;

	for (i = 0; i < count; i++)
		spans[i] = reversed[count - 1U - i];

	/* The last span that fits reaches the end. */
	spans[count - 1U].end = text->unit_count;
	return count;
}

/*
 * Lists a segment's candidates: what the user chose before, the words of
 * the dictionaries with the segment's okurigana and particles (the longest
 * word first), the literal as typed, and the segment in hiragana and
 * katakana.
 */
void
ja_segment_candidates(
	const struct ja_lexicon *lexicon,
	const struct ja_text *text,
	struct ja_segment *segment)
{
	const struct ja_user_entry *learned;
	bool ends[JA_UNITS_MAX + 1U];
	size_t core_end;
	size_t i;
	const char *span_bytes;
	size_t span_length;
	size_t tier;
	bool particle;
	bool all_kana;
	bool loanword;

	segment->candidate_count = 0;
	segment->selected = 0;
	segment->presses = 0;
	span_bytes = text->bytes + text->offsets[segment->start];
	span_length = text->offsets[segment->end] - text->offsets[segment->start];

	/* What the user chose for this reading before comes first. */
	learned = NULL;
	if (lexicon->user != NULL)
		learned = ja_user_find(lexicon->user, span_bytes, span_length);
	if (learned != NULL) {
		for (i = 0; i < learned->candidate_count; i++)
			ja_segment_add_candidate(segment, learned->candidates[i], strlen(learned->candidates[i]));
	}

	/* A segment that is one particle of one kana is offered as typed first (the は left after shortening). */
	particle = false;
	if (segment->end == segment->start + 1U)
		particle = ja_is_particle(text, segment->start, segment->end);
	if (particle)
		ja_segment_add_candidate(segment, span_bytes, span_length);

	/*
	 * The words, each with the particles after it, source by source: the
	 * user's and the engine's own words and 来る, the supplement, the verbs
	 * written in kana, then the system dictionary; within each, the longest
	 * word first.
	 */
	for (tier = 0; tier < lexicon->dict_count + 2U; tier++) {
		for (core_end = segment->end; core_end > segment->start; core_end--) {
			memset(ends, 0, sizeof(ends));
			ja_particle_ends(text, core_end, ends);
			if (!ends[segment->end])
				continue;

			/* Only kana make words. */
			all_kana = segment_all_kana(text, segment->start, core_end);
			if (!all_kana)
				continue;

			segment_add_tier(lexicon, text, segment->start, core_end, segment->end, tier, segment);
		}
	}

	/* A literal is offered as typed and in full width. */
	if (!text->kana[segment->start]) {
		ja_segment_add_candidate(segment, span_bytes, span_length);
		segment_add_full_width(text, segment->start, segment->end, segment);
	}

	/* A word that looks taken from another language is offered in katakana before hiragana. */
	loanword = segment_is_loanword(text, segment->start, segment->end);
	if (!loanword)
		ja_segment_add_candidate(segment, span_bytes, span_length);

	/* The segment in katakana, with the particles left in hiragana: the shortest word first (コーヒーを before コーヒーヲ). */
	for (core_end = segment->start + 1U; core_end <= segment->end; core_end++) {
		memset(ends, 0, sizeof(ends));
		ja_particle_ends(text, core_end, ends);
		if (!ends[segment->end])
			continue;

		/* Only a kana word is written in katakana. */
		all_kana = segment_all_kana(text, segment->start, core_end);
		if (!all_kana)
			continue;

		segment_add_katakana(text, segment->start, core_end, segment->end, segment);
	}

	/* The segment in hiragana, after the katakana for a loanword. */
	ja_segment_add_candidate(segment, span_bytes, span_length);
}

/*
 * Adds a candidate to a segment unless it has it already, or it is too
 * long, or the list is full.
 *
 * Returns whether the segment has the candidate afterwards.
 */
bool
ja_segment_add_candidate(
	struct ja_segment *segment,
	const char *text,
	size_t length)
{
	size_t i;
	bool same;

	/* An empty candidate or one too long for the list. */
	if (length == 0U || length >= IME_CANDIDATE_MAX)
		return false;

	/* A candidate listed already. */
	for (i = 0; i < segment->candidate_count; i++) {
		same = ja_bytes_equal(segment->candidates[i], strlen(segment->candidates[i]), text, length);
		if (same)
			return true;
	}

	/* No room for another. */
	if (segment->candidate_count >= IME_CANDIDATES_MAX)
		return false;

	/* Succeeded: the candidate is last in the list. */
	memcpy(segment->candidates[segment->candidate_count], text, length);
	segment->candidates[segment->candidate_count][length] = '\0';
	segment->candidate_count++;
	return true;
}

/*
 * Makes the table of where particles can end, from each place from a
 * start: entry [from * (units + 1) + to].
 *
 * Returns NULL without memory.
 */
static bool *
segment_particle_table(
	const struct ja_text *text,
	size_t start)
{
	bool *table;
	size_t row;
	size_t from;

	/* One row per place, each with a flag per place. */
	row = text->unit_count + 1U;
	table = calloc(row * row, sizeof(table[0]));
	if (table == NULL)
		return NULL;

	/* Fills each row from its place. */
	for (from = start; from <= text->unit_count; from++)
		ja_particle_ends(text, from, table + from * row);

	/* Succeeded: the table. */
	return table;
}

/*
 * Tells whether any dictionary has a reading as a noun.
 */
static bool
segment_has_noun(
	const struct ja_lexicon *lexicon,
	const char *key,
	size_t length)
{
	const struct ja_dict_entry *entry;
	const struct ja_user_entry *learned;
	size_t i;
	bool same;

	/* A reading the user converted before. */
	if (lexicon->user != NULL) {
		learned = ja_user_find(lexicon->user, key, length);
		if (learned != NULL)
			return true;
	}

	/* A word the engine knows itself. */
	for (i = 0; i < sizeof(segment_builtins) / sizeof(segment_builtins[0]); i++) {
		same = ja_bytes_equal(segment_builtins[i].reading, strlen(segment_builtins[i].reading), key, length);
		if (same)
			return true;
	}

	/* A headword of one of the dictionaries. */
	for (i = 0; i < lexicon->dict_count; i++) {
		entry = ja_dict_find(lexicon->dicts[i], key, length);
		if (entry != NULL)
			return true;
	}

	/* No dictionary knows it. */
	return false;
}

/*
 * Tells whether a reading is an everyday noun: one the user converted
 * before, one the engine knows itself, or one of a supplement (a
 * dictionary before the system dictionary, the last one).
 */
static bool
segment_is_everyday_noun(
	const struct ja_lexicon *lexicon,
	const char *key,
	size_t length)
{
	const struct ja_dict_entry *entry;
	const struct ja_user_entry *learned;
	size_t i;
	bool same;

	/* A reading the user converted before. */
	if (lexicon->user != NULL) {
		learned = ja_user_find(lexicon->user, key, length);
		if (learned != NULL)
			return true;
	}

	/* A word the engine knows itself. */
	for (i = 0; i < sizeof(segment_builtins) / sizeof(segment_builtins[0]); i++) {
		same = ja_bytes_equal(segment_builtins[i].reading, strlen(segment_builtins[i].reading), key, length);
		if (same)
			return true;
	}

	/* A headword of a supplement; the system dictionary is the last one. */
	for (i = 0; i + 1U < lexicon->dict_count; i++) {
		entry = ja_dict_find(lexicon->dicts[i], key, length);
		if (entry != NULL)
			return true;
	}

	/* Only the system dictionary knows it, if any does. */
	return false;
}

/*
 * Marks where the okurigana of a verb's or an adjective's headword can end
 * after its stem, in every dictionary that has the headword, and apart
 * from them where it can end in a supplement (a dictionary before the
 * system dictionary, the last one).
 *
 * The key holds the stem's reading and, after it, the headword's letter.
 * Returns whether any dictionary has the headword.
 */
static bool
segment_headword_ends(
	const struct ja_lexicon *lexicon,
	const struct ja_text *text,
	const char *key,
	size_t stem_length,
	size_t stem_end,
	bool *ends,
	bool *everyday_ends)
{
	const struct ja_dict_entry *entry;
	size_t i;
	bool found;

	/* Each dictionary's entry adds the okurigana its candidates take. */
	found = false;
	for (i = 0; i < lexicon->dict_count; i++) {
		entry = ja_dict_find(lexicon->dicts[i], key, stem_length + 1U);
		if (entry == NULL)
			continue;

		found = true;
		segment_entry_ends(lexicon, entry, text, key, stem_length, stem_end, ends);

		/* A supplement's okurigana are everyday ones too; the system dictionary is the last. */
		if (i + 1U < lexicon->dict_count)
			segment_entry_ends(lexicon, entry, text, key, stem_length, stem_end, everyday_ends);
	}

	/* Reports whether any dictionary knows the headword. */
	return found;
}

/*
 * Marks where the okurigana of one entry's candidates can end: the rules
 * of each conjugation a candidate names, and every rule for a candidate
 * that names none.
 */
static void
segment_entry_ends(
	const struct ja_lexicon *lexicon,
	const struct ja_dict_entry *entry,
	const struct ja_text *text,
	const char *key,
	size_t stem_length,
	size_t stem_end,
	bool *ends)
{
	enum ja_conjugation conjugation;
	bool named[JA_CONJUGATION_ADJECTIVE + 1];
	const char *word;
	size_t word_length;
	size_t position;
	int kind;
	bool adjective;
	bool more;

	/* Notes which conjugations the candidates name. */
	memset(named, 0, sizeof(named));
	position = 0;
	for (;;) {
		more = ja_dict_next_conjugated(entry, &position, &word, &word_length, &conjugation);
		if (!more)
			break;

		named[conjugation] = true;
	}

	/* A candidate that names none takes every rule, or an adjective's alone under an adjective's headword. */
	if (named[JA_CONJUGATION_ANY]) {
		adjective = segment_is_adjective(lexicon, key, stem_length, key[stem_length]);
		ja_inflect_ends(text, stem_end, key[stem_length], adjective, ends);
	}

	/* Each conjugation named takes its own rules. */
	for (kind = JA_CONJUGATION_GODAN; kind <= JA_CONJUGATION_ADJECTIVE; kind++) {
		/* A conjugation no candidate names adds no okurigana. */
		if (named[kind])
			ja_inflect_conjugated_ends(text, stem_end, key[stem_length], (enum ja_conjugation)kind, ends);
	}
}

/*
 * Tells whether one candidate's okurigana, after its stem, can end a word
 * at a place.
 */
static bool
segment_candidate_fits(
	const struct ja_lexicon *lexicon,
	const struct ja_text *text,
	const char *key,
	size_t stem_length,
	size_t stem_end,
	enum ja_conjugation conjugation,
	size_t core_end)
{
	bool ends[JA_UNITS_MAX + 1U];
	bool adjective;

	/* The okurigana the candidate's conjugation takes, or every rule when it names none. */
	memset(ends, 0, sizeof(ends));
	if (conjugation == JA_CONJUGATION_ANY) {
		adjective = segment_is_adjective(lexicon, key, stem_length, key[stem_length]);
		ja_inflect_ends(text, stem_end, key[stem_length], adjective, ends);
	} else {
		ja_inflect_conjugated_ends(text, stem_end, key[stem_length], conjugation, ends);
	}

	/* An okurigana that does not end the word here. */
	if (!ends[core_end])
		return false;

	/* The candidate's okurigana ends the word. */
	return true;
}

/*
 * Tells whether a verb's headword is an adjective's: a pure-い adjective has
 * an i and a k headword with the same word (高い: たかi /高/ and たかk
 * /高/), where a verb and a noun that share a stem have different ones
 * (いk /行/ and いi /言/).
 */
static bool
segment_is_adjective(
	const struct ja_lexicon *lexicon,
	const char *stem,
	size_t length,
	char consonant)
{
	const struct ja_dict_entry *with_i;
	const struct ja_dict_entry *with_k;
	const char *word_i;
	const char *word_k;
	char key[JA_HEADWORD_MAX * 4U + 2U];
	size_t length_i;
	size_t length_k;
	size_t position;
	size_t d;
	bool more;
	bool same;

	/* Only the i and k headwords can be an adjective's. */
	if (consonant != 'i' && consonant != 'k')
		return false;

	/* Looks in each dictionary for both headwords with the same first word. */
	memcpy(key, stem, length);
	for (d = 0; d < lexicon->dict_count; d++) {
		key[length] = 'i';
		with_i = ja_dict_find(lexicon->dicts[d], key, length + 1U);
		key[length] = 'k';
		with_k = ja_dict_find(lexicon->dicts[d], key, length + 1U);
		if (with_i == NULL || with_k == NULL)
			continue;

		/* The first word of each. */
		position = 0;
		more = ja_dict_next_candidate(with_i, &position, &word_i, &length_i);
		if (!more)
			continue;

		position = 0;
		more = ja_dict_next_candidate(with_k, &position, &word_k, &length_k);
		if (!more)
			continue;

		/* The same word under both: an adjective. */
		same = ja_bytes_equal(word_i, length_i, word_k, length_k);
		if (same)
			return true;
	}

	/* No adjective has this stem. */
	return false;
}

/*
 * Gives the code point of a unit.
 */
static uint32_t
segment_code_at(
	const struct ja_text *text,
	size_t unit)
{
	uint32_t code;

	/* Decodes the unit's character. */
	code = 0;
	(void)ja_utf8_decode(text->bytes + text->offsets[unit], text->offsets[unit + 1U] - text->offsets[unit], &code);
	return code;
}

/*
 * Tells whether a span has a kana that marks a loanword.
 */
static bool
segment_is_loanword(
	const struct ja_text *text,
	size_t start,
	size_t end)
{
	uint32_t code;
	size_t i;
	bool mark;

	/* Looks at each character. */
	for (i = start; i < end; i++) {
		code = segment_code_at(text, i);
		mark = ja_is_loanword_mark(code);
		if (mark)
			return true;
	}

	/* No mark of a loanword. */
	return false;
}

/*
 * Tells whether every unit of a span is a kana.
 */
static bool
segment_all_kana(
	const struct ja_text *text,
	size_t start,
	size_t end)
{
	size_t i;

	/* Looks for a literal in the span. */
	for (i = start; i < end; i++) {
		if (!text->kana[i])
			return false;
	}

	/* Only kana. */
	return true;
}

/*
 * Keeps the cheaper of two ways to make one word.
 */
static void
segment_item_better(
	struct segment_item *best,
	bool *valid,
	const struct segment_item *item)
{
	/* The first way found. */
	if (!*valid) {
		*best = *item;
		*valid = true;
		return;
	}

	/* Fewer unknown kana and one-kana nouns win. */
	if (item->unknown + item->single != best->unknown + best->single) {
		if (item->unknown + item->single < best->unknown + best->single)
			*best = *item;
		return;
	}

	/* Then more kana of everyday words. */
	if (item->everyday != best->everyday) {
		if (item->everyday > best->everyday)
			*best = *item;
		return;
	}

	/* Then more dictionary kana. */
	if (item->dictionary > best->dictionary)
		*best = *item;
}

/*
 * Tells whether one way to reach a place is cheaper than another.
 */
static bool
segment_cost_better(
	const struct segment_cost *left,
	const struct segment_cost *right)
{
	unsigned int left_doubt;
	unsigned int right_doubt;
	unsigned int left_known;
	unsigned int right_known;

	/* Any way beats none. */
	if (!right->reached)
		return true;

	/*
	 * The fewest kana no dictionary knows and one-kana nouns together: a
	 * one-kana noun (子, 胃) that takes a kana out of an unknown word saves
	 * nothing this way.
	 */
	left_doubt = left->unknown + left->single;
	right_doubt = right->unknown + right->single;
	if (left_doubt != right_doubt) {
		if (left_doubt < right_doubt)
			return true;
		return false;
	}

	/* Then the fewest segments. */
	if (left->segments != right->segments) {
		if (left->segments < right->segments)
			return true;
		return false;
	}

	/*
	 * Then the most kana in everyday words and particles, that is the
	 * fewest read from the system dictionary alone: of two splits into as
	 * many segments, the one made of the supplement's words is the likelier
	 * (友達と｜話しました rather than 友達とは｜成しました), and a particle
	 * the supplement's verb would swallow stays one (月曜日に｜会いましょう
	 * rather than 月曜日｜似合いましょう).
	 */
	left_known = left->everyday + left->particles;
	right_known = right->everyday + right->particles;
	if (left_known != right_known) {
		if (left_known > right_known)
			return true;
		return false;
	}

	/*
	 * Then the most kana read as particles: a word boundary before は or
	 * が is likelier than a verb that swallows it (服は｜高い rather than
	 * 服｜叩かい).
	 */
	if (left->particles != right->particles) {
		if (left->particles > right->particles)
			return true;
		return false;
	}

	/* Then the most kana found in a dictionary. */
	if (left->dictionary > right->dictionary)
		return true;

	/* Not cheaper: the way found first stays. */
	return false;
}

/*
 * Finds every word that can start at a place, keeping for each end the
 * cheapest way to make it.
 */
static void
segment_cores(
	const struct ja_lexicon *lexicon,
	const struct ja_text *text,
	size_t start,
	struct segment_item *cores,
	bool *valid)
{
	struct segment_item item;
	bool ends[JA_UNITS_MAX + 1U];
	bool everyday_ends[JA_UNITS_MAX + 1U];
	char key[JA_HEADWORD_MAX * 4U + 2U];
	size_t run_end;
	size_t length;
	size_t key_length;
	size_t end;
	size_t i;
	bool found;
	bool everyday;
	bool no_word;
	uint32_t first;

	/* A run of literals is one word of its own. */
	if (!text->kana[start]) {
		end = start;
		while (end < text->unit_count && !text->kana[end])
			end++;

		memset(&item, 0, sizeof(item));
		segment_item_better(&cores[end], &valid[end], &item);
		return;
	}

	/* Words are made of the run of kana from here. */
	run_end = start;
	while (run_end < text->unit_count && text->kana[run_end])
		run_end++;

	/* Kana no dictionary knows, any number of them. */
	for (end = start + 1U; end <= run_end; end++) {
		item.unknown = (unsigned int)(end - start);
		item.single = 0;
		item.everyday = 0;
		item.dictionary = 0;
		segment_item_better(&cores[end], &valid[end], &item);
	}

	/* No word of a dictionary begins with ー, っ, ん or a small kana. */
	first = segment_code_at(text, start);
	no_word = ja_starts_no_word(first);
	if (no_word)
		return;

	/* する, 来る and the verbs written in kana, in their forms: the engine's own everyday words. */
	memset(ends, 0, sizeof(ends));
	ja_inflect_suru_ends(text, start, ends);
	ja_inflect_kuru_ends(text, start, ends);
	ja_inflect_kana_verb_ends(text, start, ends);
	for (end = start + 1U; end <= run_end; end++) {
		if (!ends[end])
			continue;

		item.unknown = 0;
		item.single = 0;
		item.everyday = (unsigned int)(end - start);
		item.dictionary = (unsigned int)(end - start);
		segment_item_better(&cores[end], &valid[end], &item);
	}

	/* Each reading from here that is a noun, alone or with する. */
	for (length = 1; length <= JA_HEADWORD_MAX && start + length <= run_end; length++) {
		key_length = text->offsets[start + length] - text->offsets[start];
		found = segment_has_noun(lexicon, text->bytes + text->offsets[start], key_length);
		if (!found)
			continue;

		/* Tells whether the noun is one of the everyday words. */
		everyday = segment_is_everyday_noun(lexicon, text->bytes + text->offsets[start], key_length);

		/* The noun alone; a noun of one kana costs more. */
		item.unknown = 0;
		item.single = 0;
		if (length == 1U)
			item.single = 1;
		item.everyday = 0;
		if (everyday)
			item.everyday = (unsigned int)length;
		item.dictionary = (unsigned int)length;
		segment_item_better(&cores[start + length], &valid[start + length], &item);

		/* The noun with a form of する (勉強します), everyday as the noun is. */
		memset(ends, 0, sizeof(ends));
		ja_inflect_suru_ends(text, start + length, ends);
		for (end = start + length + 1U; end <= run_end; end++) {
			if (!ends[end])
				continue;

			/* The form of する counts with its noun. */
			if (everyday)
				item.everyday = (unsigned int)(end - start);
			item.dictionary = (unsigned int)(end - start);
			segment_item_better(&cores[end], &valid[end], &item);
		}
	}

	/* Each reading from here that is a verb's or an adjective's stem. */
	for (length = 1; length <= JA_HEADWORD_MAX && start + length < run_end; length++) {
		key_length = text->offsets[start + length] - text->offsets[start];
		memcpy(key, text->bytes + text->offsets[start], key_length);
		for (i = 0; SEGMENT_CONSONANTS[i] != '\0'; i++) {
			/* Every okurigana the headword's candidates can take here, and those a supplement's take. */
			key[key_length] = SEGMENT_CONSONANTS[i];
			memset(ends, 0, sizeof(ends));
			memset(everyday_ends, 0, sizeof(everyday_ends));
			found = segment_headword_ends(lexicon, text, key, key_length, start + length, ends, everyday_ends);
			if (!found)
				continue;

			for (end = start + length + 1U; end <= run_end; end++) {
				if (!ends[end])
					continue;

				item.unknown = 0;
				item.single = 0;
				item.everyday = 0;
				if (everyday_ends[end])
					item.everyday = (unsigned int)(end - start);
				item.dictionary = (unsigned int)(end - start);
				segment_item_better(&cores[end], &valid[end], &item);
			}
		}
	}
}

/*
 * Adds the nouns of a reading from every dictionary, each followed by a
 * suffix.
 */
static void
segment_add_nouns(
	const struct ja_lexicon *lexicon,
	const char *key,
	size_t length,
	const char *suffix,
	size_t suffix_length,
	struct ja_segment *segment)
{
	const struct ja_user_entry *learned;
	size_t i;
	size_t j;
	bool same;

	/* What the user chose for the word alone. */
	if (lexicon->user != NULL) {
		learned = ja_user_find(lexicon->user, key, length);
		if (learned != NULL) {
			for (i = 0; i < learned->candidate_count; i++)
				segment_add_joined(segment, learned->candidates[i], strlen(learned->candidates[i]), suffix, suffix_length);
		}
	}

	/* The words the engine knows itself. */
	for (i = 0; i < sizeof(segment_builtins) / sizeof(segment_builtins[0]); i++) {
		same = ja_bytes_equal(segment_builtins[i].reading, strlen(segment_builtins[i].reading), key, length);
		if (!same)
			continue;

		for (j = 0; segment_builtins[i].candidates[j] != NULL; j++) {
			segment_add_joined(segment, segment_builtins[i].candidates[j], strlen(segment_builtins[i].candidates[j]),
					   suffix, suffix_length);
		}
	}

	/* Each dictionary in order. */
	for (i = 0; i < lexicon->dict_count; i++)
		segment_add_dict_nouns(lexicon->dicts[i], key, length, suffix, suffix_length, segment);
}

/*
 * Adds the nouns of a reading from one dictionary, each followed by a
 * suffix.
 */
static void
segment_add_dict_nouns(
	const struct ja_dict *dict,
	const char *key,
	size_t length,
	const char *suffix,
	size_t suffix_length,
	struct ja_segment *segment)
{
	const struct ja_dict_entry *entry;

	/* The headword's words, when the dictionary has it. */
	entry = ja_dict_find(dict, key, length);
	if (entry != NULL)
		segment_add_entry(entry, suffix, suffix_length, segment);
}

/*
 * Adds every candidate of a dictionary entry, each followed by a suffix.
 */
static void
segment_add_entry(
	const struct ja_dict_entry *entry,
	const char *suffix,
	size_t suffix_length,
	struct ja_segment *segment)
{
	const char *word;
	size_t word_length;
	size_t position;
	bool more;

	/* Walks the entry's candidates. */
	position = 0;
	for (;;) {
		more = ja_dict_next_candidate(entry, &position, &word, &word_length);
		if (!more)
			break;

		segment_add_joined(segment, word, word_length, suffix, suffix_length);
	}
}

/*
 * Adds a word followed by a suffix as one candidate.
 */
static void
segment_add_joined(
	struct ja_segment *segment,
	const char *word,
	size_t word_length,
	const char *suffix,
	size_t suffix_length)
{
	char joined[IME_CANDIDATE_MAX];

	/* A candidate that would be too long is left out. */
	if (word_length + suffix_length >= sizeof(joined))
		return;

	/* The word, then the suffix. */
	memcpy(joined, word, word_length);
	memcpy(joined + word_length, suffix, suffix_length);
	(void)ja_segment_add_candidate(segment, joined, word_length + suffix_length);
}

/*
 * Adds the candidates of one source whose word ends before a place,
 * followed by the rest of the segment (okurigana and particles) as typed.
 *
 * The sources, in order: 0 is the user's and the engine's own words and
 * 来る; then each dictionary, with the verbs written in kana (います,
 * ありません, します) just before the last one, the system dictionary, so that the
 * supplement's words come before them and the system dictionary's after.
 * A dictionary's words are a noun that is the whole word, a verb or an
 * adjective whose okurigana ends it (the longest stem first), and a noun
 * with a form of する.
 */
static void
segment_add_tier(
	const struct ja_lexicon *lexicon,
	const struct ja_text *text,
	size_t start,
	size_t core_end,
	size_t end,
	size_t tier,
	struct ja_segment *segment)
{
	struct ja_lexicon learned_only;
	bool ends[JA_UNITS_MAX + 1U];
	const char *start_bytes;
	const char *after_word;
	const struct ja_dict *dict;
	size_t word_length;
	size_t after_length;
	size_t kana_stem;
	size_t kana_tier;
	bool short_enough;

	start_bytes = text->bytes + text->offsets[start];
	word_length = text->offsets[core_end] - text->offsets[start];
	after_word = text->bytes + text->offsets[core_end];
	after_length = text->offsets[end] - text->offsets[core_end];

	/* Only a reading no longer than a headword can be a noun. */
	short_enough = false;
	if (core_end - start <= JA_HEADWORD_MAX)
		short_enough = true;

	/* The user's and the engine's own words, and 来る (来ました rather than 切ました). */
	if (tier == 0U) {
		learned_only = *lexicon;
		learned_only.dict_count = 0;
		if (short_enough)
			segment_add_nouns(&learned_only, start_bytes, word_length, after_word, after_length, segment);

		memset(ends, 0, sizeof(ends));
		ja_inflect_kuru_ends(text, start, ends);
		if (ends[core_end]) {
			segment_add_joined(segment, "来", strlen("来"), text->bytes + text->offsets[start + 1U],
					   text->offsets[end] - text->offsets[start + 1U]);
		}

		return;
	}

	/*
	 * The verbs written in kana come just before the system dictionary (the
	 * last one), or after the engine's own words when there is none.
	 */
	kana_tier = 1;
	if (lexicon->dict_count > 0U)
		kana_tier = lexicon->dict_count;

	if (tier == kana_tier) {
		kana_stem = ja_inflect_kana_verb_stem(text, start, core_end);
		if (kana_stem != start)
			(void)ja_segment_add_candidate(segment, start_bytes, text->offsets[end] - text->offsets[start]);

		/* A form of する alone is written in kana as well (します rather than 知ます, する rather than 刷る). */
		memset(ends, 0, sizeof(ends));
		ja_inflect_suru_ends(text, start, ends);
		if (ends[core_end])
			(void)ja_segment_add_candidate(segment, start_bytes, text->offsets[end] - text->offsets[start]);

		/* The verbs written in kana are this source's only words. */
		return;
	}

	/* The dictionaries before the last are the sources before the kana verbs; the last one comes after them. */
	if (tier < kana_tier) {
		dict = lexicon->dicts[tier - 1U];
	} else if (tier == kana_tier + 1U && lexicon->dict_count > 0U) {
		dict = lexicon->dicts[lexicon->dict_count - 1U];
	} else {
		return;
	}

	/* The dictionary's words. */
	if (short_enough)
		segment_add_dict_nouns(dict, start_bytes, word_length, after_word, after_length, segment);

	segment_add_dict_verbs(lexicon, dict, text, start, core_end, end, segment);
	segment_add_dict_suru(dict, text, start, core_end, end, segment);
}

/*
 * Adds one dictionary's verbs and adjectives whose okurigana ends a word,
 * the longest stem first; a candidate that names its conjugation is added
 * only where that conjugation's okurigana ends the word.
 */
static void
segment_add_dict_verbs(
	const struct ja_lexicon *lexicon,
	const struct ja_dict *dict,
	const struct ja_text *text,
	size_t start,
	size_t core_end,
	size_t end,
	struct ja_segment *segment)
{
	const struct ja_dict_entry *entry;
	char key[JA_HEADWORD_MAX * 4U + 2U];
	size_t stem_length;
	size_t key_length;
	size_t i;

	/* Each stem, the longest first, with each letter a headword can end with. */
	for (stem_length = core_end - start - 1U; stem_length >= 1U; stem_length--) {
		if (stem_length > JA_HEADWORD_MAX)
			continue;

		key_length = text->offsets[start + stem_length] - text->offsets[start];
		memcpy(key, text->bytes + text->offsets[start], key_length);
		for (i = 0; SEGMENT_CONSONANTS[i] != '\0'; i++) {
			key[key_length] = SEGMENT_CONSONANTS[i];
			entry = ja_dict_find(dict, key, key_length + 1U);
			if (entry == NULL)
				continue;

			/* The candidates whose okurigana ends the word, each with the rest of the segment. */
			segment_add_conjugated_entry(lexicon, entry, text, key, key_length, start + stem_length, core_end, end, segment);
		}
	}
}

/*
 * Adds the candidates of a verb's or an adjective's entry whose okurigana
 * ends a word, each followed by the rest of the segment as typed.
 */
static void
segment_add_conjugated_entry(
	const struct ja_lexicon *lexicon,
	const struct ja_dict_entry *entry,
	const struct ja_text *text,
	const char *key,
	size_t stem_length,
	size_t stem_end,
	size_t core_end,
	size_t end,
	struct ja_segment *segment)
{
	enum ja_conjugation conjugation;
	const char *word;
	const char *after_stem;
	size_t word_length;
	size_t after_length;
	size_t position;
	bool more;
	bool fits;

	/* The okurigana and particles after the stem, as typed. */
	after_stem = text->bytes + text->offsets[stem_end];
	after_length = text->offsets[end] - text->offsets[stem_end];

	/* Walks the entry's candidates. */
	position = 0;
	for (;;) {
		more = ja_dict_next_conjugated(entry, &position, &word, &word_length, &conjugation);
		if (!more)
			break;

		/* A candidate whose okurigana cannot end the word here is left out (借 一段 in 借った). */
		fits = segment_candidate_fits(lexicon, text, key, stem_length, stem_end, conjugation, core_end);
		if (!fits)
			continue;

		segment_add_joined(segment, word, word_length, after_stem, after_length);
	}
}

/*
 * Adds one dictionary's nouns that, with a form of する, make a word.
 */
static void
segment_add_dict_suru(
	const struct ja_dict *dict,
	const struct ja_text *text,
	size_t start,
	size_t core_end,
	size_t end,
	struct ja_segment *segment)
{
	bool ends[JA_UNITS_MAX + 1U];
	size_t noun_end;

	/* Each place the noun can end, with する from there to the end of the word. */
	for (noun_end = core_end - 1U; noun_end > start; noun_end--) {
		if (noun_end - start > JA_HEADWORD_MAX)
			continue;

		memset(ends, 0, sizeof(ends));
		ja_inflect_suru_ends(text, noun_end, ends);
		if (!ends[core_end])
			continue;

		segment_add_dict_nouns(dict, text->bytes + text->offsets[start], text->offsets[noun_end] - text->offsets[start],
				       text->bytes + text->offsets[noun_end], text->offsets[end] - text->offsets[noun_end], segment);
	}
}

/*
 * Adds a segment written in katakana up to a place and as typed after it.
 */
static void
segment_add_katakana(
	const struct ja_text *text,
	size_t start,
	size_t core_end,
	size_t end,
	struct ja_segment *segment)
{
	char written[IME_CANDIDATE_MAX];
	size_t length;
	size_t position;
	size_t used;
	size_t suffix_length;
	uint32_t code;
	uint32_t katakana;

	/* The word in katakana, character by character. */
	length = 0;
	position = text->offsets[start];
	while (position < text->offsets[core_end]) {
		used = ja_utf8_decode(text->bytes + position, text->offsets[core_end] - position, &code);
		position += used;

		/* A candidate that would be too long is left out. */
		if (length + 4U >= sizeof(written))
			return;

		katakana = ja_to_katakana(code);
		length += ja_utf8_encode(katakana, written + length);
	}

	/* The particles as typed. */
	suffix_length = text->offsets[end] - text->offsets[core_end];
	if (length + suffix_length >= sizeof(written))
		return;

	memcpy(written + length, text->bytes + text->offsets[core_end], suffix_length);
	(void)ja_segment_add_candidate(segment, written, length + suffix_length);
}

/*
 * Adds a span written in full-width characters.
 */
static void
segment_add_full_width(
	const struct ja_text *text,
	size_t start,
	size_t end,
	struct ja_segment *segment)
{
	char written[IME_CANDIDATE_MAX];
	size_t length;
	size_t position;
	size_t used;
	uint32_t code;
	uint32_t full;

	/* Each character in its full-width form. */
	length = 0;
	position = text->offsets[start];
	while (position < text->offsets[end]) {
		used = ja_utf8_decode(text->bytes + position, text->offsets[end] - position, &code);
		position += used;

		/* A candidate that would be too long is left out. */
		if (length + 4U >= sizeof(written))
			return;

		full = ja_to_full_ascii(code);
		length += ja_utf8_encode(full, written + length);
	}

	(void)ja_segment_add_candidate(segment, written, length);
}
