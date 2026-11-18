/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SKK dictionaries (plan/ws095/design.md sections 7.3 and 9).
 *
 * A dictionary file is read whole into memory and indexed by headword in
 * an open-addressed hash table; the entries point into the file's bytes.
 * A line is "headword /candidate/candidate/"; lines starting with ";" are
 * comments.  A line of any other shape is counted and skipped, so that a
 * damaged file still gives what it can.  A candidate's annotation (after
 * ";") is not shown, and a Lisp candidate ("(concat ...)") is skipped.
 * An annotation that is exactly 五段, 一段 or 形容詞 says how a verb's or an
 * adjective's candidate conjugates; any other annotation says nothing to
 * the engine.
 *
 * The system's own dictionary is one file of two parts (SKK-JISYO.ja):
 * the supplement, then the system dictionary from the comment line
 * DICT_PART_SYSTEM on.  ja_dict_load_parts reads such a file once and
 * indexes each part as a dictionary of its own.
 */

#include "ja.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The comment line that begins the system dictionary's part of a dictionary of two parts (ws095-p017). */
#define DICT_PART_SYSTEM	";; ==== part: system ===="

/* The FNV-1a hash's starting value and its multiplier. */
#define DICT_HASH_BASIS		2166136261U
#define DICT_HASH_PRIME		16777619U

static int dict_read_file(const char *path, size_t size_max, char **data, size_t *size);
static int dict_index(struct ja_dict *dict);
static size_t dict_part_start(const char *data, size_t size);
static bool dict_parse_line(const char *line, size_t length, struct ja_dict_entry *entry);
static void dict_insert(struct ja_dict *dict, const struct ja_dict_entry *entry);
static uint32_t dict_hash(const char *key, size_t length);
static enum ja_conjugation dict_conjugation(const char *annotation, size_t length);

/*
 * Reads an SKK dictionary.
 *
 * Returns 0; ENOENT or another errno when the file cannot be read; EFBIG
 * when it is larger than the limit; ENOMEM.
 */
int
ja_dict_load(
	struct ja_dict *dict,
	const char *path,
	size_t size_max)
{
	int error;

	memset(dict, 0, sizeof(*dict));

	/* Reads the whole file. */
	error = dict_read_file(path, size_max, &dict->data, &dict->size);
	if (error != 0)
		return error;

	/* Indexes its lines by headword. */
	error = dict_index(dict);
	if (error != 0) {
		ja_dict_free(dict);
		return error;
	}

	/* Succeeded: the dictionary can be looked in. */
	return 0;
}

/*
 * Reads a dictionary file of two parts (ws095-p017): the part before the
 * line DICT_PART_SYSTEM into first (the supplement) and the part from it
 * on into second (the system dictionary).  A file without the line is all
 * second's, and first is left empty; *split says which it was.
 *
 * Returns 0; ENOENT or another errno when the file cannot be read; EFBIG
 * when it is larger than the limit; ENOMEM.
 */
int
ja_dict_load_parts(
	struct ja_dict *first,
	struct ja_dict *second,
	const char *path,
	size_t size_max,
	bool *split)
{
	char *data;
	size_t size;
	size_t start;
	int error;

	/* Nothing read yet, and no part found. */
	memset(first, 0, sizeof(*first));
	memset(second, 0, sizeof(*second));
	*split = false;

	/* Reads the whole file once. */
	error = dict_read_file(path, size_max, &data, &size);
	if (error != 0)
		return error;

	/* Without the line, the file is one dictionary: second's. */
	start = dict_part_start(data, size);
	if (start == size) {
		second->data = data;
		second->size = size;
		error = dict_index(second);
		if (error != 0) {
			ja_dict_free(second);
			return error;
		}
		return 0;
	}

	/* The system dictionary's part gets bytes of its own, terminated. */
	second->data = malloc(size - start + 1U);
	if (second->data == NULL) {
		free(data);
		return ENOMEM;
	}
	memcpy(second->data, data + start, size - start);
	second->data[size - start] = '\0';
	second->size = size - start;

	/* The supplement's part keeps the file's bytes, ended where the other part begins. */
	data[start] = '\0';
	first->data = data;
	first->size = start;

	/* Each part is indexed by its headwords. */
	error = dict_index(first);
	if (error == 0)
		error = dict_index(second);
	if (error != 0) {
		ja_dict_free(first);
		ja_dict_free(second);
		return error;
	}

	/* Succeeded: both parts can be looked in. */
	*split = true;
	return 0;
}

/*
 * Frees a dictionary's memory.
 */
void
ja_dict_free(
	struct ja_dict *dict)
{
	/* The file's bytes and the index into them. */
	free(dict->data);
	free(dict->slots);
	memset(dict, 0, sizeof(*dict));
}

/*
 * Finds a headword's entry; NULL when the dictionary does not have it.
 */
const struct ja_dict_entry *
ja_dict_find(
	const struct ja_dict *dict,
	const char *key,
	size_t key_length)
{
	const struct ja_dict_entry *slot;
	uint32_t hash;
	size_t index;
	bool same;

	/* An empty dictionary has no table. */
	if (dict->slot_count == 0U)
		return NULL;

	/* Walks the slots from the headword's own until an empty one. */
	hash = dict_hash(key, key_length);
	index = hash & (dict->slot_count - 1U);
	for (;;) {
		slot = &dict->slots[index];
		if (slot->key == NULL)
			return NULL;

		/* The same headword. */
		same = ja_bytes_equal(slot->key, slot->key_length, key, key_length);
		if (same)
			return slot;

		index = (index + 1U) & (dict->slot_count - 1U);
	}
}

/*
 * Gives the next candidate of an entry, from a position that starts at 0.
 *
 * Returns false when there are no more.  The candidate is not
 * terminated; its annotation is left out.
 */
bool
ja_dict_next_candidate(
	const struct ja_dict_entry *entry,
	size_t *position,
	const char **candidate,
	size_t *length)
{
	enum ja_conjugation conjugation;
	bool more;

	/* Reads the candidate; how it conjugates is not wanted here. */
	more = ja_dict_next_conjugated(entry, position, candidate, length, &conjugation);
	if (!more)
		return false;

	/* Succeeded: one candidate found. */
	return true;
}

/*
 * Gives the next candidate of an entry and how it conjugates, from a
 * position that starts at 0.
 *
 * Returns false when there are no more.  The candidate is not
 * terminated; its annotation is left out, and read only for the
 * conjugation it names.
 */
bool
ja_dict_next_conjugated(
	const struct ja_dict_entry *entry,
	size_t *position,
	const char **candidate,
	size_t *length,
	enum ja_conjugation *conjugation)
{
	const char *text;
	size_t start;
	size_t end;
	size_t annotation;

	text = entry->candidates;

	/* Goes past each empty or Lisp candidate. */
	for (;;) {
		/* The candidates begin after the first slash. */
		start = *position;
		if (start == 0U)
			start = 1;

		/* Nothing is left after the last slash. */
		if (start >= entry->candidates_length)
			return false;

		/* The candidate runs to the next slash. */
		end = start;
		while (end < entry->candidates_length && text[end] != '/')
			end++;

		*position = end + 1U;

		/* The annotation after a semicolon is not shown. */
		annotation = start;
		while (annotation < end && text[annotation] != ';')
			annotation++;

		/* An empty candidate is skipped. */
		if (annotation == start)
			continue;

		/* A Lisp form is not text to insert. */
		if (text[start] == '(')
			continue;

		break;
	}

	/* Reads the conjugation the annotation names, if it names one. */
	*conjugation = JA_CONJUGATION_ANY;
	if (annotation < end)
		*conjugation = dict_conjugation(text + annotation + 1U, end - annotation - 1U);

	/* Succeeded: one candidate found. */
	*candidate = text + start;
	*length = annotation - start;
	return true;
}

/*
 * Reads a whole file into a buffer with a terminating NUL.
 */
static int
dict_read_file(
	const char *path,
	size_t size_max,
	char **data,
	size_t *size)
{
	struct stat status;
	char *buffer;
	size_t done;
	ssize_t count;
	int descriptor;
	int error;
	bool regular;

	/* Opens the file. */
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return errno;

	/* Refuses what is not a regular file or is too large. */
	error = fstat(descriptor, &status);
	if (error != 0) {
		error = errno;
		close(descriptor);
		return error;
	}

	regular = S_ISREG(status.st_mode);
	if (!regular) {
		close(descriptor);
		return EINVAL;
	}

	if ((unsigned long long)status.st_size > (unsigned long long)size_max) {
		close(descriptor);
		return EFBIG;
	}

	/* Allocates room for the bytes and a NUL. */
	buffer = malloc((size_t)status.st_size + 1U);
	if (buffer == NULL) {
		close(descriptor);
		return ENOMEM;
	}

	/* Reads until the end of the file. */
	done = 0;
	while (done < (size_t)status.st_size) {
		count = read(descriptor, buffer + done, (size_t)status.st_size - done);
		if (count < 0) {
			error = errno;
			free(buffer);
			close(descriptor);
			return error;
		}

		/* The file became shorter while it was read. */
		if (count == 0)
			break;

		done += (size_t)count;
	}

	close(descriptor);

	/* Succeeded: the bytes read, terminated. */
	buffer[done] = '\0';
	*data = buffer;
	*size = done;
	return 0;
}

/*
 * Builds the table of headwords from the dictionary's lines.
 */
static int
dict_index(
	struct ja_dict *dict)
{
	struct ja_dict_entry entry;
	size_t lines;
	size_t position;
	size_t end;
	bool parsed;

	/* Counts the lines, an upper bound on the headwords. */
	lines = 1;
	for (position = 0; position < dict->size; position++) {
		if (dict->data[position] == '\n')
			lines++;
	}

	/* Makes the table at least twice as large, a power of two. */
	dict->slot_count = 16;
	while (dict->slot_count < lines * 2U)
		dict->slot_count *= 2U;

	dict->slots = calloc(dict->slot_count, sizeof(dict->slots[0]));
	if (dict->slots == NULL)
		return ENOMEM;

	/* Parses each line and puts its headword in the table. */
	position = 0;
	while (position < dict->size) {
		end = position;
		while (end < dict->size && dict->data[end] != '\n')
			end++;

		/* A comment or an empty line holds no headword. */
		if (end == position || dict->data[position] == ';') {
			position = end + 1U;
			continue;
		}

		/* A line of another shape is counted and skipped. */
		parsed = dict_parse_line(dict->data + position, end - position, &entry);
		if (!parsed) {
			dict->malformed_count++;
			position = end + 1U;
			continue;
		}

		dict_insert(dict, &entry);
		position = end + 1U;
	}

	/* Succeeded: every well-formed line is indexed. */
	return 0;
}

/*
 * Finds where the system dictionary's part of a dictionary of two parts
 * begins: the start of the first line that is exactly DICT_PART_SYSTEM
 * (a carriage return before its newline allowed), or size when no line
 * is.
 */
static size_t
dict_part_start(
	const char *data,
	size_t size)
{
	size_t mark_length;
	size_t position;
	size_t end;
	size_t length;
	bool same;

	/* Each line, from the start. */
	mark_length = strlen(DICT_PART_SYSTEM);
	position = 0;
	while (position < size) {
		end = position;
		while (end < size && data[end] != '\n')
			end++;

		/* The line without a carriage return at its end. */
		length = end - position;
		if (length != 0U && data[end - 1U] == '\r')
			length--;

		/* The line that begins the system's part. */
		same = ja_bytes_equal(data + position, length, DICT_PART_SYSTEM, mark_length);
		if (same)
			return position;
		position = end + 1U;
	}

	/* No such line. */
	return size;
}

/*
 * Splits a line into its headword and its candidates.
 *
 * Returns false for a line that is not "headword /.../".
 */
static bool
dict_parse_line(
	const char *line,
	size_t length,
	struct ja_dict_entry *entry)
{
	size_t space;

	/* A carriage return at the end is not part of the line. */
	if (length != 0U && line[length - 1U] == '\r')
		length--;

	/* The headword runs to the first space. */
	space = 0;
	while (space < length && line[space] != ' ')
		space++;

	/* No headword, or nothing after it. */
	if (space == 0U || space + 2U >= length)
		return false;

	/* The candidates start with a slash after the space. */
	if (line[space + 1U] != '/')
		return false;

	/* And end with one. */
	if (line[length - 1U] != '/')
		return false;

	/* Succeeded: the line's two parts. */
	entry->key = line;
	entry->key_length = space;
	entry->candidates = line + space + 1U;
	entry->candidates_length = length - space - 1U;
	return true;
}

/*
 * Puts an entry in the table; a headword seen before keeps its first line.
 */
static void
dict_insert(
	struct ja_dict *dict,
	const struct ja_dict_entry *entry)
{
	struct ja_dict_entry *slot;
	uint32_t hash;
	size_t index;
	bool same;

	/* Walks the slots from the headword's own until an empty one. */
	hash = dict_hash(entry->key, entry->key_length);
	index = hash & (dict->slot_count - 1U);
	for (;;) {
		slot = &dict->slots[index];
		if (slot->key == NULL)
			break;

		/* The same headword again: the first line wins. */
		same = ja_bytes_equal(slot->key, slot->key_length, entry->key, entry->key_length);
		if (same)
			return;

		index = (index + 1U) & (dict->slot_count - 1U);
	}

	/* Succeeded: the headword has its slot. */
	*slot = *entry;
	dict->entry_count++;
}

/*
 * Hashes a headword (FNV-1a).
 */
static uint32_t
dict_hash(
	const char *key,
	size_t length)
{
	uint32_t hash;
	size_t i;

	/* Folds in each byte. */
	hash = DICT_HASH_BASIS;
	for (i = 0; i < length; i++) {
		hash ^= (unsigned char)key[i];
		hash *= DICT_HASH_PRIME;
	}

	/* The headword's hash. */
	return hash;
}

/*
 * Tells which conjugation an annotation names; any other text names none.
 */
static enum ja_conjugation
dict_conjugation(
	const char *annotation,
	size_t length)
{
	bool same;

	/* A godan verb, conjugating through its headword letter's row. */
	same = ja_bytes_equal(annotation, length, "五段", strlen("五段"));
	if (same)
		return JA_CONJUGATION_GODAN;

	/* An ichidan verb, whose endings follow its stem. */
	same = ja_bytes_equal(annotation, length, "一段", strlen("一段"));
	if (same)
		return JA_CONJUGATION_ICHIDAN;

	/* An adjective, whose endings follow its stem. */
	same = ja_bytes_equal(annotation, length, "形容詞", strlen("形容詞"));
	if (same)
		return JA_CONJUGATION_ADJECTIVE;

	/* An annotation for the reader only. */
	return JA_CONJUGATION_ANY;
}
