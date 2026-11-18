/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Predictions (ws166-p002): while a reading is typed, the words whose
 * readings start with it, as smartphones and Windows's input method offer
 * them.
 *
 * The words come from the user's choices first (the user dictionary, the
 * most recent first) and the dictionaries after (shorter readings first,
 * the first candidate of each reading before its others).  A word whose
 * reading is exactly what is typed comes last, after the longer ones, since
 * the ordinary conversion offers it anyway.  For the on-screen keyboard
 * (ja_predict_keyboard, ws166-p002), which has no conversion of its own,
 * the words of the reading itself come first instead.  Each word is
 * offered once.  Only readings without okurigana are predicted.
 *
 * A dictionary's headwords are found by a binary search in an index of
 * them sorted by their bytes (UTF-8 keeps the order of the kana), made once
 * when the dictionary is read.
 */

#include "ja.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * A headword found for a prefix, with its place in the index, so that
 * sorting by the reading's length keeps the index's order among readings
 * of the same length.
 */
struct predict_match {
	const struct ja_dict_entry *entry;
	size_t position;
};

/* What the predictions are gathered into, and how many there may be. */
struct predict_list {
	struct ja_prediction *items;
	size_t count;
	size_t max;
};

static bool predict_has_okuri(const char *key, size_t length);
static int predict_compare_entries(const void *left, const void *right);
static int predict_compare_stamps(const void *left, const void *right);
static int predict_compare_matches(const void *left, const void *right);
static bool predict_starts_with(const char *key, size_t key_length, const char *prefix, size_t length);
static size_t predict_lower_bound(const struct ja_predict_index *index, const char *prefix, size_t length);
static void predict_user(const struct ja_user *user, const char *reading, size_t length, bool exact, struct predict_list *list);
static void predict_dicts(const struct ja_predict_index *const *indexes, size_t index_count, const char *reading, size_t length, bool exact, struct predict_list *list);
static void predict_entries(const struct predict_match *matches, size_t count, struct predict_list *list);
static void predict_add(struct predict_list *list, const char *text, size_t text_length, const char *reading, size_t reading_length);

/*
 * Makes a dictionary's index of headwords without okurigana, sorted by
 * their bytes.  Returns 0 or ENOMEM.
 */
int
ja_predict_index(
	const struct ja_dict *dict,
	struct ja_predict_index *index)
{
	size_t i;
	size_t count;
	bool okuri;

	/* Empty until made. */
	index->entries = NULL;
	index->count = 0;

	/* Room for every headword. */
	if (dict->entry_count == 0)
		return 0;

	/* The array of pointers. */
	index->entries = calloc(dict->entry_count, sizeof(index->entries[0]));
	if (index->entries == NULL)
		return ENOMEM;

	/* The headwords without okurigana. */
	count = 0;
	for (i = 0; i < dict->slot_count && count < dict->entry_count; i++) {
		/* An empty slot. */
		if (dict->slots[i].key == NULL)
			continue;

		/* Kept unless it ends in the letter of its okurigana. */
		okuri = predict_has_okuri(dict->slots[i].key, dict->slots[i].key_length);
		if (!okuri) {
			index->entries[count] = &dict->slots[i];
			count++;
		}
	}

	/* Sorted by their bytes. */
	index->count = count;
	qsort(index->entries, count, sizeof(index->entries[0]), predict_compare_entries);

	/* Succeeded: the index is made. */
	return 0;
}

/*
 * Lets an index go.
 */
void
ja_predict_index_free(
	struct ja_predict_index *index)
{
	/* The array; the entries are the dictionary's. */
	free(index->entries);
	index->entries = NULL;
	index->count = 0;
}

/*
 * Gives the predictions for a reading: up to max words whose readings
 * start with it, in the order the file's header describes.  Returns how
 * many there are.
 */
size_t
ja_predict(
	const struct ja_user *user,
	const struct ja_predict_index *const *indexes,
	size_t index_count,
	const char *reading,
	size_t length,
	struct ja_prediction *out,
	size_t max)
{
	struct predict_list list;

	/* Nothing to predict from nothing. */
	if (length == 0 || max == 0)
		return 0;

	/* The list, empty. */
	list.items = out;
	list.count = 0;
	list.max = max;

	/* The longer readings: the user's words, then the dictionaries'. */
	predict_user(user, reading, length, false, &list);
	predict_dicts(indexes, index_count, reading, length, false, &list);

	/* The reading itself last, the user's first again. */
	predict_user(user, reading, length, true, &list);
	predict_dicts(indexes, index_count, reading, length, true, &list);

	/* The count. */
	return list.count;
}

/*
 * Gives the on-screen keyboard's words for a reading (ws166-p002): the
 * words of the reading itself first (the user's, then the dictionaries'),
 * then up to max in all whose readings are longer, in ja_predict's order.
 * Returns how many there are.
 */
size_t
ja_predict_keyboard(
	const struct ja_user *user,
	const struct ja_predict_index *const *indexes,
	size_t index_count,
	const char *reading,
	size_t length,
	struct ja_prediction *out,
	size_t max)
{
	struct predict_list list;

	/* Nothing to predict from nothing. */
	if (length == 0 || max == 0)
		return 0;

	/* The list, empty. */
	list.items = out;
	list.count = 0;
	list.max = max;

	/* The reading itself: the user's words, then the dictionaries'. */
	predict_user(user, reading, length, true, &list);
	predict_dicts(indexes, index_count, reading, length, true, &list);

	/* Then the longer readings, the user's first again. */
	predict_user(user, reading, length, false, &list);
	predict_dicts(indexes, index_count, reading, length, false, &list);

	/* The count. */
	return list.count;
}

/*
 * Says whether a headword has okurigana: it ends in the ASCII letter of
 * their first consonant after a kana (かk).
 */
static bool
predict_has_okuri(
	const char *key,
	size_t length)
{
	unsigned char last;
	unsigned char before;

	/* Too short to be a kana and a letter. */
	if (length < 2U)
		return false;

	/* A lower-case letter after a byte of a multi-byte character. */
	last = (unsigned char)key[length - 1U];
	before = (unsigned char)key[length - 2U];
	if (last >= 'a' && last <= 'z' && before >= 0x80U)
		return true;

	/* No okurigana. */
	return false;
}

/*
 * Orders two headwords by their bytes, a shorter one before the longer one
 * it begins.
 */
static int
predict_compare_entries(
	const void *left,
	const void *right)
{
	const struct ja_dict_entry *a;
	const struct ja_dict_entry *b;
	size_t shorter;
	int order;

	/* The bytes they share. */
	a = *(const struct ja_dict_entry *const *)left;
	b = *(const struct ja_dict_entry *const *)right;
	shorter = a->key_length;
	if (b->key_length < shorter)
		shorter = b->key_length;

	/* The first byte that differs decides. */
	order = memcmp(a->key, b->key, shorter);
	if (order != 0)
		return order;

	/* The shorter first. */
	if (a->key_length < b->key_length)
		return -1;

	/* The longer after. */
	if (a->key_length > b->key_length)
		return 1;

	/* The same. */
	return 0;
}

/*
 * Orders two user entries by their stamps, the most recent first.
 */
static int
predict_compare_stamps(
	const void *left,
	const void *right)
{
	const struct ja_user_entry *a;
	const struct ja_user_entry *b;

	/* The two entries. */
	a = *(const struct ja_user_entry *const *)left;
	b = *(const struct ja_user_entry *const *)right;

	/* The more recent first. */
	if (a->stamp > b->stamp)
		return -1;

	/* The older after. */
	if (a->stamp < b->stamp)
		return 1;

	/* The same. */
	return 0;
}

/*
 * Orders two headwords found by the length of their readings, and by their
 * place in the index among the same length.
 */
static int
predict_compare_matches(
	const void *left,
	const void *right)
{
	const struct predict_match *a;
	const struct predict_match *b;

	/* The two. */
	a = left;
	b = right;

	/* The shorter reading first. */
	if (a->entry->key_length != b->entry->key_length) {
		if (a->entry->key_length < b->entry->key_length)
			return -1;

		/* The longer after. */
		return 1;
	}

	/* The earlier in the index first. */
	if (a->position < b->position)
		return -1;

	/* The later after. */
	if (a->position > b->position)
		return 1;

	/* The same. */
	return 0;
}

/*
 * Says whether a reading starts with a prefix.
 */
static bool
predict_starts_with(
	const char *key,
	size_t key_length,
	const char *prefix,
	size_t length)
{
	int order;

	/* Too short to start with it. */
	if (key_length < length)
		return false;

	/* The prefix's bytes. */
	order = memcmp(key, prefix, length);
	if (order != 0)
		return false;

	/* It starts with it. */
	return true;
}

/*
 * Gives the place of the first headword of an index not before a prefix in
 * the index's order.
 */
static size_t
predict_lower_bound(
	const struct ja_predict_index *index,
	const char *prefix,
	size_t length)
{
	const struct ja_dict_entry *entry;
	size_t low;
	size_t high;
	size_t middle;
	size_t shorter;
	int order;

	/* Halves the range until it is one place. */
	low = 0;
	high = index->count;
	while (low < high) {
		/* The middle headword against the prefix. */
		middle = low + (high - low) / 2U;
		entry = index->entries[middle];
		shorter = entry->key_length;
		if (length < shorter)
			shorter = length;

		/* The bytes they share, and a shorter headword that begins the prefix comes before it. */
		order = memcmp(entry->key, prefix, shorter);
		if (order == 0 && entry->key_length < length)
			order = -1;

		/* Before the prefix: the place is after it. */
		if (order < 0)
			low = middle + 1U;
		else
			high = middle;
	}

	/* The place. */
	return low;
}

/*
 * Adds the user's words whose readings start with the reading (exact:
 * whose readings are the reading), the most recently chosen first.
 */
static void
predict_user(
	const struct ja_user *user,
	const char *reading,
	size_t length,
	bool exact,
	struct predict_list *list)
{
	const struct ja_user_entry **found;
	const struct ja_user_entry *entry;
	size_t entry_length;
	size_t count;
	size_t i;
	size_t j;
	bool starts;
	bool okuri;

	/* No user dictionary, or nothing it could add. */
	if (user == NULL || user->slots == NULL || user->entry_count == 0 || list->count == list->max)
		return;

	/* Room for every entry. */
	found = calloc(user->entry_count, sizeof(found[0]));
	if (found == NULL)
		return;

	/* The entries that match. */
	count = 0;
	for (i = 0; i < user->slot_count && count < user->entry_count; i++) {
		/* An entry with a reading. */
		entry = &user->slots[i];
		if (entry->reading == NULL)
			continue;

		/* Its reading starts with the reading, without okurigana. */
		entry_length = strlen(entry->reading);
		starts = predict_starts_with(entry->reading, entry_length, reading, length);
		okuri = predict_has_okuri(entry->reading, entry_length);
		if (!starts || okuri)
			continue;

		/* The longer readings, or the reading itself. */
		if ((entry_length == length) == exact) {
			found[count] = entry;
			count++;
		}
	}

	/* The most recent first, each entry's candidates in its order (the latest choice first). */
	qsort(found, count, sizeof(found[0]), predict_compare_stamps);
	for (i = 0; i < count; i++) {
		/* Each candidate of the entry. */
		for (j = 0; j < found[i]->candidate_count; j++)
			predict_add(list, found[i]->candidates[j], strlen(found[i]->candidates[j]), found[i]->reading, strlen(found[i]->reading));
	}

	/* The list of entries goes. */
	free(found);
}

/*
 * Adds the dictionaries' words whose readings start with the reading
 * (exact: whose readings are the reading), dictionary by dictionary.
 */
static void
predict_dicts(
	const struct ja_predict_index *const *indexes,
	size_t index_count,
	const char *reading,
	size_t length,
	bool exact,
	struct predict_list *list)
{
	const struct ja_predict_index *index;
	struct predict_match *matches;
	size_t first;
	size_t last;
	size_t count;
	size_t i;
	size_t k;
	bool starts;

	/* Each dictionary, while there is room. */
	for (i = 0; i < index_count && list->count < list->max; i++) {
		/* The range of headwords that start with the reading. */
		index = indexes[i];
		if (index == NULL || index->count == 0)
			continue;

		/* The first headword not before the reading, then the end of those that start with it. */
		first = predict_lower_bound(index, reading, length);
		last = first;
		for (;;) {
			/* Past the end, or a headword that no longer starts with it. */
			if (last >= index->count)
				break;

			/* A headword that still starts with the reading. */
			starts = predict_starts_with(index->entries[last]->key, index->entries[last]->key_length, reading, length);
			if (!starts)
				break;

			/* It is in the range. */
			last++;
		}

		/* The range's headwords that are longer than the reading, or the reading itself. */
		matches = calloc(last - first + 1U, sizeof(matches[0]));
		if (matches == NULL)
			return;

		/* The kind asked for, among the range. */
		count = 0;
		for (k = first; k < last; k++) {
			/* Kept when it is the kind asked for. */
			if ((index->entries[k]->key_length == length) == exact) {
				matches[count].entry = index->entries[k];
				matches[count].position = k;
				count++;
			}
		}

		/* The shorter readings first, then their candidates. */
		qsort(matches, count, sizeof(matches[0]), predict_compare_matches);
		predict_entries(matches, count, list);
		free(matches);
	}
}

/*
 * Adds the candidates of headwords: the first candidate of each, then the
 * others of each in turn.
 */
static void
predict_entries(
	const struct predict_match *matches,
	size_t count,
	struct predict_list *list)
{
	const char *candidate;
	size_t position;
	size_t candidate_length;
	size_t i;
	bool more;

	/* The first candidate of each headword. */
	for (i = 0; i < count && list->count < list->max; i++) {
		/* Its first. */
		position = 0;
		more = ja_dict_next_candidate(matches[i].entry, &position, &candidate, &candidate_length);
		if (more)
			predict_add(list, candidate, candidate_length, matches[i].entry->key, matches[i].entry->key_length);
	}

	/* The others, headword by headword. */
	for (i = 0; i < count && list->count < list->max; i++) {
		/* After the first. */
		position = 0;
		more = ja_dict_next_candidate(matches[i].entry, &position, &candidate, &candidate_length);
		while (more && list->count < list->max) {
			/* The next, added when it is new. */
			more = ja_dict_next_candidate(matches[i].entry, &position, &candidate, &candidate_length);
			if (more)
				predict_add(list, candidate, candidate_length, matches[i].entry->key, matches[i].entry->key_length);
		}
	}
}

/*
 * Adds a word unless it is there already, or it or its reading does not
 * fit, or the list is full.
 */
static void
predict_add(
	struct predict_list *list,
	const char *text,
	size_t text_length,
	const char *reading,
	size_t reading_length)
{
	struct ja_prediction *item;
	size_t i;
	bool same;

	/* Full, or too long. */
	if (list->count == list->max || text_length == 0 || text_length >= IME_CANDIDATE_MAX ||
	    reading_length >= JA_PREDICT_READING_MAX)
		return;

	/* Once each. */
	for (i = 0; i < list->count; i++) {
		/* The same word. */
		same = ja_bytes_equal(list->items[i].text, strlen(list->items[i].text), text, text_length);
		if (same)
			return;
	}

	/* The word and its reading. */
	item = &list->items[list->count];
	memcpy(item->text, text, text_length);
	item->text[text_length] = '\0';
	memcpy(item->reading, reading, reading_length);
	item->reading[reading_length] = '\0';
	list->count++;
}
