/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The user dictionary: the candidates the user chose, most recent first
 * (plan/ws095/design.md section 7.4).
 *
 * It is an SKK file of readings and candidates, like the system
 * dictionary, and is looked in before it.  Each save rewrites the file
 * whole: a temporary file is written, synced and renamed over
 * the old one, so that a crash leaves either the old or the new file and
 * never an empty one.  The file is the user's alone (mode 0600), and a
 * damaged or oversized file is read as far as it is well formed.  The
 * directory it lives in is made by the input method before the engine
 * starts.
 *
 * The choices learned while typing are not written at each commit
 * (BUG-143): the engine saves once no key has come for a while, and when
 * it closes.  That save goes by a thread of its own (ja_user_save_later):
 * the file's text is made at once, and the write, its sync and the rename
 * happen away from the keys, which on a slow disk took long enough that
 * the compositor passed the input method by.  Only the newest text waits to be
 * written; freeing the dictionary waits for the last write.
 */

#include "ja.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The table's size: a power of two well above the most entries kept. */
#define USER_SLOTS		16384U

/* The FNV-1a hash's starting value and its multiplier. */
#define USER_HASH_BASIS		2166136261U
#define USER_HASH_PRIME		16777619U

/* The first line of a saved file. */
#define USER_HEADER		";; Kei input method: the user's conversions, most recent first.\n"

/*
 * The thread that writes the user dictionary's file away from the keys.
 *
 * user_writer_lock guards it: pending is the newest text not yet written
 * (NULL for none) for the file at path, started says the thread runs and
 * must be joined, and stopping asks it to end once nothing waits.  There
 * is one, for the one dictionary the input method keeps.
 */
struct user_writer {
	pthread_t thread;
	int started;
	int stopping;
	char *pending;
	size_t pending_length;
	char path[1024];
};

static int user_load(struct ja_user *user);
static int user_parse_line(struct ja_user *user, const char *line, size_t length, uint64_t stamp);
static struct ja_user_entry *user_slot(struct ja_user *user, const char *reading, size_t length);
static int user_insert(struct ja_user *user, const char *reading, size_t length, uint64_t stamp, struct ja_user_entry **entry);
static int user_add_candidate(struct ja_user_entry *entry, const char *candidate, size_t length, bool to_front);
static void user_evict_oldest(struct ja_user *user);
static void user_free_entry(struct ja_user_entry *entry);
static bool user_is_storable(const char *text, size_t length);
static uint32_t user_hash(const char *text, size_t length);
static int user_compare_stamps(const void *left, const void *right);
static int user_write_all(int descriptor, const char *bytes, size_t length);
static int user_serialize(const struct ja_user *user, char **text, size_t *length);
static int user_write_file(const char *path, const char *text, size_t length);
static void *user_writer_run(void *argument);
static void user_writer_flush(void);

/* The one writer thread (see struct user_writer); zero until the first save that waits. */
static struct user_writer user_writer;

/* Guards user_writer; user_writer_wake tells the thread a text waits or the end is asked. */
static pthread_mutex_t user_writer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t user_writer_wake = PTHREAD_COND_INITIALIZER;

/*
 * Held around each write of the file, so that a save made at once
 * (ja_user_save) and the thread's never write the temporary file together.
 */
static pthread_mutex_t user_writer_file_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Opens the user dictionary at a path, reading the file if there is one.
 *
 * Returns 0 (also when the file does not exist yet), or ENOMEM.
 */
int
ja_user_open(
	struct ja_user *user,
	const char *path)
{
	size_t path_length;
	int error;

	memset(user, 0, sizeof(*user));

	/* Keeps where the file is written back to. */
	path_length = strlen(path);
	if (path_length >= sizeof(user->path))
		return ENAMETOOLONG;

	strcpy(user->path, path);

	/* Makes the empty table. */
	user->slot_count = USER_SLOTS;
	user->slots = calloc(user->slot_count, sizeof(user->slots[0]));
	if (user->slots == NULL)
		return ENOMEM;

	/* Reads what was learned before; a missing or damaged file is no failure. */
	error = user_load(user);
	if (error == ENOMEM) {
		ja_user_free(user);
		return error;
	}

	/* Succeeded: the dictionary can be looked in and learned into. */
	return 0;
}

/*
 * Frees the user dictionary's memory.
 */
void
ja_user_free(
	struct ja_user *user)
{
	size_t i;

	/* A write still waiting or under way finishes first: the choices outlive the input method. */
	user_writer_flush();

	/* Frees every entry, then the table. */
	if (user->slots != NULL) {
		for (i = 0; i < user->slot_count; i++)
			user_free_entry(&user->slots[i]);
	}

	free(user->slots);
	memset(user, 0, sizeof(*user));
}

/*
 * Finds the entry of a reading; NULL when the user has not converted it.
 */
const struct ja_user_entry *
ja_user_find(
	const struct ja_user *user,
	const char *reading,
	size_t length)
{
	struct ja_user_entry *slot;

	/* A dictionary that could not be opened has no table. */
	if (user->slots == NULL)
		return NULL;

	/* Finds the reading's slot, which is empty when it has not been learned. */
	slot = user_slot((struct ja_user *)user, reading, length);
	if (slot->reading == NULL)
		return NULL;

	/* The learned entry. */
	return slot;
}

/*
 * Learns that a candidate was chosen for a reading: it is put first.
 *
 * Returns 0, EINVAL for a reading or candidate that cannot be written in
 * the file's form, or ENOMEM.
 */
int
ja_user_learn(
	struct ja_user *user,
	const char *reading,
	size_t length,
	const char *candidate)
{
	struct ja_user_entry *entry;
	size_t candidate_length;
	bool storable;
	int error;

	/* A dictionary that could not be opened learns nothing. */
	if (user->slots == NULL)
		return ENOMEM;

	/* Refuses what the file's lines cannot hold. */
	candidate_length = strlen(candidate);
	storable = user_is_storable(reading, length);
	if (!storable)
		return EINVAL;

	storable = user_is_storable(candidate, candidate_length);
	if (!storable)
		return EINVAL;

	/* Finds or makes the reading's entry, stamped as the newest. */
	user->stamp++;
	error = user_insert(user, reading, length, user->stamp, &entry);
	if (error != 0)
		return error;

	entry->stamp = user->stamp;

	/* Puts the candidate first. */
	error = user_add_candidate(entry, candidate, candidate_length, true);
	if (error != 0)
		return error;

	/* Succeeded: the choice is remembered. */
	return 0;
}

/*
 * Writes the user dictionary back to its file now.
 *
 * Returns 0 or the errno of the step that failed; the old file is then
 * left as it was.
 */
int
ja_user_save(
	const struct ja_user *user)
{
	char *text;
	size_t length;
	int error;

	/* The file's text. */
	error = user_serialize(user, &text, &length);
	if (error != 0)
		return error;

	/* Written in place of the old file, never together with the writer thread. */
	(void)pthread_mutex_lock(&user_writer_file_lock);

	error = user_write_file(user->path, text, length);

	(void)pthread_mutex_unlock(&user_writer_file_lock);

	free(text);

	/* Reports a write that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the file holds every choice. */
	return 0;
}

/*
 * Writes the user dictionary back to its file by the writer thread
 * (BUG-143): the text is made now, the slow write, sync and rename happen
 * away from the keys.  A newer save replaces a text that still waits.
 *
 * Returns 0, or ENOMEM or the errno of a thread that could not start; the
 * file is then written now instead.
 */
int
ja_user_save_later(
	const struct ja_user *user)
{
	char *text;
	size_t length;
	int error;

	/* The file's text. */
	error = user_serialize(user, &text, &length);
	if (error != 0)
		return error;

	/* The text waits for the thread, in place of an older one; the thread starts the first time. */
	(void)pthread_mutex_lock(&user_writer_lock);

	free(user_writer.pending);
	user_writer.pending = text;
	user_writer.pending_length = length;
	(void)snprintf(user_writer.path, sizeof(user_writer.path), "%s", user->path);
	error = 0;
	if (!user_writer.started) {
		user_writer.stopping = 0;
		error = pthread_create(&user_writer.thread, NULL, user_writer_run, NULL);
		if (error == 0)
			user_writer.started = 1;
	}
	if (error == 0)
		(void)pthread_cond_signal(&user_writer_wake);
	if (error != 0) {
		user_writer.pending = NULL;
		user_writer.pending_length = 0;
	}

	(void)pthread_mutex_unlock(&user_writer_lock);

	/* Without a thread the file is written now, as before. */
	if (error != 0) {
		(void)pthread_mutex_lock(&user_writer_file_lock);

		error = user_write_file(user->path, text, length);

		(void)pthread_mutex_unlock(&user_writer_file_lock);

		free(text);
		if (error != 0)
			return error;
	}

	/* Succeeded: the text is written, or will be. */
	return 0;
}

/*
 * Makes the file's text: the header, then one line per reading, most
 * recent first.  Returns 0 with a text the caller frees, or ENOMEM.
 */
static int
user_serialize(
	const struct ja_user *user,
	char **text,
	size_t *length)
{
	const struct ja_user_entry **order;
	char *line;
	char *grown;
	char *buffer;
	size_t line_size;
	size_t line_length;
	size_t capacity;
	size_t used;
	size_t count;
	size_t i;
	size_t j;

	/* A dictionary that could not be opened has nothing to save. */
	if (user->slots == NULL)
		return ENOMEM;

	/* Lists the entries, most recent first. */
	order = malloc((user->entry_count + 1U) * sizeof(order[0]));
	if (order == NULL)
		return ENOMEM;

	count = 0;
	for (i = 0; i < user->slot_count; i++) {
		if (user->slots[i].reading != NULL) {
			order[count] = &user->slots[i];
			count++;
		}
	}

	qsort(order, count, sizeof(order[0]), user_compare_stamps);

	/* Room for the longest line. */
	line_size = JA_HEADWORD_MAX * 4U * 4U + JA_USER_CANDIDATES_MAX * (IME_CANDIDATE_MAX + 1U) + 8U;
	line = malloc(line_size);
	if (line == NULL) {
		free(order);
		return ENOMEM;
	}

	/* The text starts with the header. */
	capacity = strlen(USER_HEADER) + 4096U;
	buffer = malloc(capacity);
	if (buffer == NULL) {
		free(line);
		free(order);
		return ENOMEM;
	}
	used = strlen(USER_HEADER);
	memcpy(buffer, USER_HEADER, used);

	/* One line per reading, the buffer grown when a line does not fit. */
	for (i = 0; i < count; i++) {
		line_length = 0;
		line_length += (size_t)snprintf(line + line_length, line_size - line_length, "%s /", order[i]->reading);
		for (j = 0; j < order[i]->candidate_count; j++)
			line_length += (size_t)snprintf(line + line_length, line_size - line_length, "%s/", order[i]->candidates[j]);
		line_length += (size_t)snprintf(line + line_length, line_size - line_length, "\n");

		/* A line that does not fit doubles the buffer. */
		if (used + line_length > capacity) {
			capacity = (capacity + line_length) * 2U;
			grown = realloc(buffer, capacity);
			if (grown == NULL) {
				free(buffer);
				free(line);
				free(order);
				return ENOMEM;
			}
			buffer = grown;
		}

		/* The line after the ones before it. */
		memcpy(buffer + used, line, line_length);
		used += line_length;
	}

	free(line);
	free(order);

	/* Succeeded: the caller owns the text. */
	*text = buffer;
	*length = used;
	return 0;
}

/*
 * Writes a text to a temporary file beside path, syncs it and renames it
 * over path.  Returns 0 or the errno of the step that failed; the old
 * file is then left as it was.  The caller holds user_writer_file_lock.
 */
static int
user_write_file(
	const char *path,
	const char *text,
	size_t length)
{
	char temporary[sizeof(user_writer.path) + 8U];
	int descriptor;
	int error;
	int status;

	/* Opens the temporary file beside the real one, the user's alone. */
	snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (descriptor < 0)
		return errno;

	/* The text. */
	error = user_write_all(descriptor, text, length);

	/* Makes the new file durable before it replaces the old one. */
	if (error == 0) {
		status = fsync(descriptor);
		if (status != 0)
			error = errno;
	}

	/* Closes it; a failed close loses what was written. */
	status = close(descriptor);
	if (status != 0 && error == 0)
		error = errno;

	/* A file not wholly written is not put in place. */
	if (error != 0) {
		unlink(temporary);
		return error;
	}

	/* Replaces the old file in one step. */
	status = rename(temporary, path);
	if (status != 0) {
		error = errno;
		unlink(temporary);
		return error;
	}

	/* Succeeded: the file holds the text. */
	return 0;
}

/*
 * The writer thread: writes the newest waiting text, again while more
 * come, and ends when asked once nothing waits.
 */
static void *
user_writer_run(
	void *argument)
{
	char path[sizeof(user_writer.path)];
	char *text;
	size_t length;

	(void)argument;

	/* Each text in turn. */
	for (;;) {
		/* Waits for a text, or for the end. */
		(void)pthread_mutex_lock(&user_writer_lock);

		while (user_writer.pending == NULL && !user_writer.stopping)
			(void)pthread_cond_wait(&user_writer_wake, &user_writer_lock);
		text = user_writer.pending;
		length = user_writer.pending_length;
		user_writer.pending = NULL;
		user_writer.pending_length = 0;
		memcpy(path, user_writer.path, sizeof(path));

		(void)pthread_mutex_unlock(&user_writer_lock);

		/* Nothing waits and the end was asked. */
		if (text == NULL)
			break;

		/* The file, written away from the keys; a failure leaves the old file. */
		(void)pthread_mutex_lock(&user_writer_file_lock);

		(void)user_write_file(path, text, length);

		(void)pthread_mutex_unlock(&user_writer_file_lock);

		free(text);
	}

	/* Succeeded: every text was written. */
	return NULL;
}

/* Waits for the writer thread to write what waits and end (nothing when it never started). */
static void
user_writer_flush(
	void)
{
	int started;

	/* Asks the thread to end once nothing waits. */
	(void)pthread_mutex_lock(&user_writer_lock);

	started = user_writer.started;
	if (started) {
		user_writer.stopping = 1;
		(void)pthread_cond_signal(&user_writer_wake);
	}

	(void)pthread_mutex_unlock(&user_writer_lock);

	/* A thread that never started has nothing to write. */
	if (!started)
		return;

	/* The last write, then the thread is gone; a later save starts another. */
	(void)pthread_join(user_writer.thread, NULL);

	(void)pthread_mutex_lock(&user_writer_lock);

	user_writer.started = 0;
	user_writer.stopping = 0;

	(void)pthread_mutex_unlock(&user_writer_lock);
}

/*
 * Reads the file's lines into the table; the first line is the newest.
 */
static int
user_load(
	struct ja_user *user)
{
	struct ja_dict file;
	size_t position;
	size_t end;
	size_t lines;
	uint64_t stamp;
	int error;

	/* Reads the file; a missing or unreadable file leaves the table empty. */
	error = ja_dict_load(&file, user->path, JA_USER_SIZE_MAX);
	if (error == ENOMEM)
		return error;

	if (error != 0)
		return 0;

	/* Counts the lines so that the first gets the newest stamp. */
	lines = 1;
	for (position = 0; position < file.size; position++) {
		if (file.data[position] == '\n')
			lines++;
	}

	/* Adds each line in turn. */
	stamp = lines;
	position = 0;
	while (position < file.size) {
		end = position;
		while (end < file.size && file.data[end] != '\n')
			end++;

		/* Comments and empty lines hold no entry. */
		if (end != position && file.data[position] != ';') {
			error = user_parse_line(user, file.data + position, end - position, stamp);
			if (error == ENOMEM) {
				ja_dict_free(&file);
				return error;
			}

			if (error != 0)
				user->malformed_count++;
		}

		stamp--;
		position = end + 1U;
	}

	user->stamp = lines;
	ja_dict_free(&file);

	/* Succeeded: what the file held is in the table. */
	return 0;
}

/*
 * Adds one line of the file to the table.
 */
static int
user_parse_line(
	struct ja_user *user,
	const char *line,
	size_t length,
	uint64_t stamp)
{
	struct ja_user_entry *entry;
	size_t space;
	size_t start;
	size_t end;
	bool storable;
	int error;

	/* A carriage return at the end is not part of the line. */
	if (length != 0U && line[length - 1U] == '\r')
		length--;

	/* The reading runs to the first space; the candidates follow in slashes. */
	space = 0;
	while (space < length && line[space] != ' ')
		space++;

	if (space == 0U || space + 2U >= length)
		return EINVAL;

	if (line[space + 1U] != '/' || line[length - 1U] != '/')
		return EINVAL;

	storable = user_is_storable(line, space);
	if (!storable)
		return EINVAL;

	/* Finds or makes the reading's entry. */
	error = user_insert(user, line, space, stamp, &entry);
	if (error != 0)
		return error;

	/* Adds each candidate in the file's order. */
	start = space + 2U;
	while (start < length) {
		end = start;
		while (end < length && line[end] != '/')
			end++;

		/* An empty candidate adds nothing. */
		if (end > start) {
			error = user_add_candidate(entry, line + start, end - start, false);
			if (error == ENOMEM)
				return error;
		}

		start = end + 1U;
	}

	/* Succeeded: the line is in the table. */
	return 0;
}

/*
 * Finds the slot a reading lives in, or the empty slot it would take.
 */
static struct ja_user_entry *
user_slot(
	struct ja_user *user,
	const char *reading,
	size_t length)
{
	struct ja_user_entry *slot;
	uint32_t hash;
	size_t index;
	bool same;

	/* Walks the slots from the reading's own until it or an empty one. */
	hash = user_hash(reading, length);
	index = hash & (user->slot_count - 1U);
	for (;;) {
		slot = &user->slots[index];
		if (slot->reading == NULL)
			return slot;

		/* The same reading. */
		same = ja_bytes_equal(slot->reading, strlen(slot->reading), reading, length);
		if (same)
			return slot;

		index = (index + 1U) & (user->slot_count - 1U);
	}
}

/*
 * Finds a reading's entry, making it if it is new; the oldest entry makes
 * room when the table is full.
 */
static int
user_insert(
	struct ja_user *user,
	const char *reading,
	size_t length,
	uint64_t stamp,
	struct ja_user_entry **entry)
{
	struct ja_user_entry *slot;

	/* An existing entry. */
	slot = user_slot(user, reading, length);
	if (slot->reading != NULL) {
		*entry = slot;
		return 0;
	}

	/* A full table forgets its oldest reading first. */
	if (user->entry_count >= JA_USER_ENTRIES_MAX) {
		user_evict_oldest(user);
		slot = user_slot(user, reading, length);
	}

	/* Copies the reading into the new entry. */
	slot->reading = malloc(length + 1U);
	if (slot->reading == NULL)
		return ENOMEM;

	memcpy(slot->reading, reading, length);
	slot->reading[length] = '\0';
	slot->candidate_count = 0;
	slot->stamp = stamp;
	user->entry_count++;

	/* Succeeded: the new entry. */
	*entry = slot;
	return 0;
}

/*
 * Adds a candidate to an entry, first or last; a candidate it already has
 * is moved rather than repeated, and the last one falls off a full entry.
 */
static int
user_add_candidate(
	struct ja_user_entry *entry,
	const char *candidate,
	size_t length,
	bool to_front)
{
	char *copy;
	size_t i;
	size_t found;
	bool same;

	/* Looks for the same candidate. */
	found = entry->candidate_count;
	for (i = 0; i < entry->candidate_count; i++) {
		same = ja_bytes_equal(entry->candidates[i], strlen(entry->candidates[i]), candidate, length);
		if (same) {
			found = i;
			break;
		}
	}

	/* A candidate read from the file again adds nothing. */
	if (found < entry->candidate_count && !to_front)
		return 0;

	/* A known candidate is taken out, to be put first. */
	if (found < entry->candidate_count) {
		copy = entry->candidates[found];
		memmove(&entry->candidates[found], &entry->candidates[found + 1U],
			(entry->candidate_count - found - 1U) * sizeof(entry->candidates[0]));
		entry->candidate_count--;
	} else {
		copy = malloc(length + 1U);
		if (copy == NULL)
			return ENOMEM;

		memcpy(copy, candidate, length);
		copy[length] = '\0';
	}

	/* A full entry drops its oldest candidate. */
	if (entry->candidate_count >= JA_USER_CANDIDATES_MAX) {
		if (!to_front) {
			free(copy);
			return 0;
		}

		free(entry->candidates[entry->candidate_count - 1U]);
		entry->candidate_count--;
	}

	/* Puts the candidate first or last. */
	if (to_front) {
		memmove(&entry->candidates[1], &entry->candidates[0], entry->candidate_count * sizeof(entry->candidates[0]));
		entry->candidates[0] = copy;
	} else {
		entry->candidates[entry->candidate_count] = copy;
	}

	/* Succeeded: the entry holds the candidate. */
	entry->candidate_count++;
	return 0;
}

/*
 * Forgets the reading used longest ago, and rebuilds the table so that
 * no other reading is cut off from its slot.
 */
static void
user_evict_oldest(
	struct ja_user *user)
{
	struct ja_user_entry *old_slots;
	struct ja_user_entry *slot;
	size_t oldest;
	size_t i;

	/* Finds the oldest entry. */
	oldest = user->slot_count;
	for (i = 0; i < user->slot_count; i++) {
		if (user->slots[i].reading == NULL)
			continue;

		/* An older stamp than the one found so far. */
		if (oldest == user->slot_count || user->slots[i].stamp < user->slots[oldest].stamp)
			oldest = i;
	}

	/* Nothing to forget. */
	if (oldest == user->slot_count)
		return;

	user_free_entry(&user->slots[oldest]);
	user->entry_count--;

	/* Moves the other entries into a fresh table of the same size. */
	old_slots = user->slots;
	user->slots = calloc(user->slot_count, sizeof(user->slots[0]));
	if (user->slots == NULL) {
		user->slots = old_slots;
		return;
	}

	for (i = 0; i < user->slot_count; i++) {
		if (old_slots[i].reading == NULL)
			continue;

		/* Each entry takes the slot its reading now hashes to. */
		slot = user_slot(user, old_slots[i].reading, strlen(old_slots[i].reading));
		*slot = old_slots[i];
	}

	free(old_slots);
}

/*
 * Frees an entry's strings and empties its slot.
 */
static void
user_free_entry(
	struct ja_user_entry *entry)
{
	size_t i;

	/* The candidates, then the reading. */
	for (i = 0; i < entry->candidate_count; i++)
		free(entry->candidates[i]);

	free(entry->reading);
	memset(entry, 0, sizeof(*entry));
}

/*
 * Tells whether a text can be a reading or a candidate in the file: not
 * empty, not too long, and without the characters that separate them.
 */
static bool
user_is_storable(
	const char *text,
	size_t length)
{
	size_t i;

	/* An empty text, or one longer than a candidate. */
	if (length == 0U || length >= IME_CANDIDATE_MAX)
		return false;

	/* The separators of the file's lines. */
	for (i = 0; i < length; i++) {
		if (text[i] == '/' || text[i] == ';' || text[i] == '\n' || text[i] == '\r' || text[i] == ' ' || text[i] == '\0')
			return false;
	}

	/* The text can be written as it is. */
	return true;
}

/*
 * Hashes a reading (FNV-1a).
 */
static uint32_t
user_hash(
	const char *text,
	size_t length)
{
	uint32_t hash;
	size_t i;

	/* Folds in each byte. */
	hash = USER_HASH_BASIS;
	for (i = 0; i < length; i++) {
		hash ^= (unsigned char)text[i];
		hash *= USER_HASH_PRIME;
	}

	/* The reading's hash. */
	return hash;
}

/*
 * Orders entries newest first, for qsort.
 */
static int
user_compare_stamps(
	const void *left,
	const void *right)
{
	const struct ja_user_entry *const *left_entry;
	const struct ja_user_entry *const *right_entry;

	left_entry = left;
	right_entry = right;

	/* The newer entry goes first. */
	if ((*left_entry)->stamp > (*right_entry)->stamp)
		return -1;

	if ((*left_entry)->stamp < (*right_entry)->stamp)
		return 1;

	/* Two entries of one stamp keep either order. */
	return 0;
}

/*
 * Writes all of some bytes, going on after a short write.
 */
static int
user_write_all(
	int descriptor,
	const char *bytes,
	size_t length)
{
	ssize_t count;
	size_t done;

	/* Writes until every byte is out. */
	done = 0;
	while (done < length) {
		count = write(descriptor, bytes + done, length - done);
		if (count < 0) {
			if (errno == EINTR)
				continue;

			return errno;
		}

		done += (size_t)count;
	}

	/* Succeeded: every byte written. */
	return 0;
}
