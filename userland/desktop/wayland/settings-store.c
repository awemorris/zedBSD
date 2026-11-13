/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compositor's store of the desktop's settings (settings-store.h; WS135,
 * plan/ws135/design.md sections 4.1 and 4.3).
 *
 * desktop.conf is the compositor's own file: key=value lines, read once
 * when the session starts and written once when it ends.  The write is a
 * merge: under an flock of a lock file beside it, the file is read again,
 * only the settings this session changed are put in (or taken out, for a
 * setting back at its default), every other line stays as it is (a hand
 * edit, a key the store does not know, a comment), and the new text is
 * flushed to the disk and renamed over the old file.  A reader never sees
 * half a file, and a hand edit made during the session is not lost.
 */

#include "settings-store.h"

#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* The largest file read or written, in bytes. */
#define STORE_FILE_MAX		65536U

/* The file's folder under the home, and its name there. */
#define STORE_FOLDER		".config/keiland"
#define STORE_NAME		"desktop.conf"

static void store_entry_set(struct kwl_settings_store *store, struct kwl_settings_entry *entry, const char *value, unsigned chosen);
static void store_parse(struct kwl_settings_store *store, const char *text, size_t length);
static void store_parse_line(struct kwl_settings_store *store, const char *line, size_t length);
static int store_clean_value(const struct kl_settings_key *key, const char *raw, char *value, size_t size);
static int store_entry_differs(const struct kwl_settings_entry *entry);
static void store_mark_saved(struct kwl_settings_store *store, const struct kwl_settings_change *changes, unsigned count);
static int store_merge(const char *folder, const char *path, const struct kwl_settings_change *changes, unsigned count);
static int store_load(const char *path, char *text, size_t capacity, size_t *length);
static int store_compose(const char *old_text, size_t old_length, const char *key, const char *value, char *text, size_t capacity, size_t *length);
static int store_append(char *text, size_t capacity, size_t *length, const char *part, size_t part_length);
static int store_append_line(char *text, size_t capacity, size_t *length, const char *key, const char *value);
static int store_line_is(const char *line, size_t length, const char *key);
static int store_replace(const char *path, const char *text, size_t length);
static int store_lock(const char *path);
static void store_mkdir(const char *folder);
static void *store_writer_run(void *argument);
static void store_copy(char *to, size_t size, const char *from);

/*
 * Makes the store's settings, each at its resolver's default, for the
 * file in a home (NULL or an empty home: nothing is read or written).
 *
 * The defaults the command line gives (kwl_settings_store_default) and the
 * file's values (kwl_settings_store_load) come after.  Returns 0, or
 * ENAMETOOLONG for a home whose file's path does not fit.
 */
int
kwl_settings_store_open(
	struct kwl_settings_store *store,
	const char *home)
{
	const struct kl_settings_key *key;
	struct kwl_settings_entry *entry;
	size_t count;
	size_t index;
	int written;
	int sound;

	/* Starts empty, with no file and no writer, and makes the writer's lock. */
	memset(store, 0, sizeof(*store));
	(void)pthread_mutex_init(&store->writer.lock, NULL);

	/* Makes one entry for each compositor setting, at the table's default, while there is room. */
	count = kl_settings_key_count();
	for (index = 0;
	     index < count && store->count < KWL_SETTINGS_ENTRIES;
	     index++) {
		/* Only a setting the compositor resolves is the store's. */
		key = kl_settings_key_at(index);
		if (key->resolver != KL_SETTINGS_RESOLVER_COMPOSITOR)
			continue;

		/* Ties the next entry to its key. */
		entry = &store->entries[store->count];
		entry->key = key;

		/* A number's default is the table's; a path's is none until the command line gives one. */
		if (key->type != KL_SETTINGS_TYPE_PATH)
			(void)snprintf(entry->fallback, sizeof(entry->fallback), "%d", key->fallback);

		/* Puts the default into effect. */
		store_copy(entry->value, sizeof(entry->value), entry->fallback);

		/*
		 * known says a value is in effect.  The sound's is audiod's, so it
		 * is not known until audiod reports (kwl_settings_store_report).
		 */
		entry->known = 1;
		sound = strncmp(key->name, "sound.", 6U);
		if (sound == 0)
			entry->known = 0;

		/* The entry is the store's from now on. */
		store->count++;
	}

	/* Without a home there is no file. */
	if (home == NULL || home[0] != '/')
		return 0;

	/* Names the folder under the home. */
	written = snprintf(store->folder, sizeof(store->folder), "%s/%s", home, STORE_FOLDER);
	if (written < 0 || (size_t)written >= sizeof(store->folder))
		return ENAMETOOLONG;

	/* Names the file in the folder. */
	written = snprintf(store->path, sizeof(store->path), "%s/%s", store->folder, STORE_NAME);
	if (written < 0 || (size_t)written >= sizeof(store->path))
		return ENAMETOOLONG;

	/* present tells the load and the saves that there is a file to read and write. */
	store->present = 1;

	/* Succeeded: the file can be read and written. */
	return 0;
}

/*
 * Waits for a write still under way and lets the store go (the session's
 * end is kwl_settings_store_finish, before this).
 */
void
kwl_settings_store_close(
	struct kwl_settings_store *store)
{
	/* A writer that was started is joined. */
	if (store->writer.started) {
		(void)pthread_join(store->writer.thread, NULL);
		store->writer.started = 0;
	}

	/* The lock goes with the store. */
	(void)pthread_mutex_destroy(&store->writer.lock);
}

/*
 * Finds a compositor setting's entry; NULL for a name the store does not
 * hold (a key of an application, or none at all).
 */
struct kwl_settings_entry *
kwl_settings_store_find(
	struct kwl_settings_store *store,
	const char *name)
{
	unsigned index;
	int differs;

	/* Finds the entry whose key has the name. */
	for (index = 0; index < store->count; index++) {
		differs = strcmp(store->entries[index].key->name, name);
		if (differs == 0)
			return &store->entries[index];
	}

	/* The store has no such setting. */
	return NULL;
}

/*
 * Gives a setting the default its resolver uses when nothing was chosen
 * (the command line's wallpaper and opacity); a setting at its default
 * takes it at once.
 */
void
kwl_settings_store_default(
	struct kwl_settings_store *store,
	const char *name,
	const char *value)
{
	struct kwl_settings_entry *entry;

	/* Only a setting the store holds. */
	entry = kwl_settings_store_find(store, name);
	if (entry == NULL)
		return;

	/* Keeps the default. */
	store_copy(entry->fallback, sizeof(entry->fallback), value);

	/* A setting at its default takes it at once. */
	if (!entry->chosen)
		store_copy(entry->value, sizeof(entry->value), entry->fallback);
}

/*
 * Reads the file's values, once, at the session's start: a number outside
 * its range is moved into it, a value that is not of its setting's type is
 * passed over, and so are keys the store does not hold.  Each value read
 * is also the start the session's end compares with.
 *
 * Returns 0 (also for a missing file or no home), or the errno value of
 * reading, which read_error keeps.
 */
int
kwl_settings_store_load(
	struct kwl_settings_store *store)
{
	char *text;
	size_t length;
	int error;

	/* Without a home there is nothing to read. */
	if (!store->present)
		return 0;

	/* Allocates room for the file's text. */
	text = malloc(STORE_FILE_MAX);
	if (text == NULL) {
		store->read_error = ENOMEM;
		return ENOMEM;
	}

	/* Reads the text; a missing file is empty. */
	error = store_load(store->path, text, STORE_FILE_MAX, &length);
	if (error != 0) {
		free(text);
		store->read_error = error;
		return error;
	}

	/* Takes the settings from the text's lines, then lets the text go. */
	store_parse(store, text, length);
	free(text);

	/* Succeeded: the file's values are in effect. */
	return 0;
}

/*
 * Sets a setting to a value the user chose (a client's set, the system
 * bar).  Returns 0, ENOENT for a setting the store does not hold, EPERM
 * for one only the compositor reports, or EINVAL for a value it cannot
 * take.
 */
int
kwl_settings_store_choose(
	struct kwl_settings_store *store,
	const char *name,
	const char *value)
{
	struct kwl_settings_entry *entry;
	int error;

	/* Only a setting the store holds. */
	entry = kwl_settings_store_find(store, name);
	if (entry == NULL)
		return ENOENT;

	/* Only one that may be set. */
	if ((entry->key->flags & KL_SETTINGS_KEY_READ_ONLY) != 0U)
		return EPERM;

	/* A value of its type and within its range. */
	error = kl_settings_key_check(entry->key, value);
	if (error != 0)
		return error;

	/* Puts the chosen value into effect. */
	store_entry_set(store, entry, value, 1U);

	/* Succeeded: the value is in effect. */
	return 0;
}

/*
 * Puts a setting back at its resolver's default.  Returns 0, ENOENT or
 * EPERM.
 */
int
kwl_settings_store_reset(
	struct kwl_settings_store *store,
	const char *name)
{
	struct kwl_settings_entry *entry;

	/* Only a setting the store holds. */
	entry = kwl_settings_store_find(store, name);
	if (entry == NULL)
		return ENOENT;

	/* Only one that may be set. */
	if ((entry->key->flags & KL_SETTINGS_KEY_READ_ONLY) != 0U)
		return EPERM;

	/* Puts the default into effect, as not chosen. */
	store_entry_set(store, entry, entry->fallback, 0U);

	/* Succeeded: the default is in effect. */
	return 0;
}

/*
 * Takes a value the compositor measured (audiod's volume, whether there is
 * sound): it is in effect and known from now on.
 */
void
kwl_settings_store_report(
	struct kwl_settings_store *store,
	const char *name,
	const char *value)
{
	struct kwl_settings_entry *entry;

	/* Only a setting the store holds. */
	entry = kwl_settings_store_find(store, name);
	if (entry == NULL)
		return;

	/* The measured value is the one in effect. */
	store_entry_set(store, entry, value, 1U);
}

/*
 * Lists the settings the session's end would write: those kept in the
 * file whose value is known and differs from the start.  changes has room
 * for KWL_SETTINGS_ENTRIES.  Returns how many.
 */
unsigned
kwl_settings_store_changes(
	const struct kwl_settings_store *store,
	struct kwl_settings_change *changes)
{
	const struct kwl_settings_entry *entry;
	unsigned count;
	unsigned index;
	int differs;

	/* Lists each entry that differs from what the file held. */
	count = 0;
	for (index = 0; index < store->count; index++) {
		/* An entry the same as the file is not written. */
		entry = &store->entries[index];
		differs = store_entry_differs(entry);
		if (!differs)
			continue;

		/* Lists the key, its value, and whether it was chosen (unchosen: the key leaves the file). */
		store_copy(changes[count].key, sizeof(changes[count].key), entry->key->name);
		store_copy(changes[count].value, sizeof(changes[count].value), entry->value);
		changes[count].chosen = entry->chosen;
		count++;
	}

	/* Reports how many changes were listed. */
	return count;
}

/*
 * Merges the session's changes into the file now, waiting for the disk
 * (the end of the compositor).  Returns 0 (also with nothing to write or
 * no home), or the errno value of the merge; the file is then as it was.
 */
int
kwl_settings_store_save(
	struct kwl_settings_store *store)
{
	struct kwl_settings_change changes[KWL_SETTINGS_ENTRIES];
	unsigned count;
	int error;

	/* Without a home there is no file. */
	if (!store->present)
		return 0;

	/* Nothing differs: the file stays as it is. */
	count = kwl_settings_store_changes(store, changes);
	if (count == 0U)
		return 0;

	/* Merges the changes into the file. */
	error = store_merge(store->folder, store->path, changes, count);
	if (error != 0)
		return error;

	/*
	 * The written values are the file's start from now on, and
	 * saved_generation records that the file took every change up to the
	 * store's generation.
	 */
	store_mark_saved(store, changes, count);
	store->saved_generation = store->generation;

	/* Succeeded: the file holds every change. */
	return 0;
}

/*
 * Merges the session's changes into the file on a thread of its own (the
 * Log Out: the event loop goes on drawing until the session's manager ends
 * it).  The changes are copied now; a change made afterwards is written by
 * kwl_settings_store_finish.  Returns 0 (also with nothing to write), EBUSY
 * when a writer was started already, or the errno value of the thread.
 */
int
kwl_settings_store_save_later(
	struct kwl_settings_store *store)
{
	sigset_t signals_all;
	sigset_t signals_before;
	struct kwl_settings_writer *writer;
	int error;

	/* The changes as they are now, for the thread; none leaves the file as it is. */
	error = kwl_settings_store_save_give(store);
	if (error != 0)
		return error;

	/* Nothing to write. */
	writer = &store->writer;
	if (!writer->given)
		return 0;

	/* Starts the thread that merges the changes. */
	/* The thread blocks every signal: the stops and a child's end go to the event loop's thread. */
	(void)sigfillset(&signals_all);
	(void)pthread_sigmask(SIG_SETMASK, &signals_all, &signals_before);
	error = pthread_create(&writer->thread, NULL, store_writer_run, writer);
	(void)pthread_sigmask(SIG_SETMASK, &signals_before, NULL);
	if (error != 0) {
		writer->given = 0;
		return error;
	}

	/*
	 * started tells kwl_settings_store_finish and close that a thread runs
	 * or waits to be joined, and refuses a second writer.
	 */
	writer->given = 0;
	writer->started = 1;

	/* Succeeded: the file is being written. */
	return 0;
}

/*
 * Gives the session's changes as they are now to the writer, for a thread
 * of the caller's to merge with kwl_settings_store_save_run (the Log Out,
 * on the compositor's file I/O thread); writer.given says there was
 * something to write.  A change made afterwards is written by
 * kwl_settings_store_finish, which the caller calls once that thread ran
 * it.  Returns 0 (also with nothing to write or no home), or EBUSY when a
 * writer was given or started already.
 */
int
kwl_settings_store_save_give(
	struct kwl_settings_store *store)
{
	struct kwl_settings_writer *writer;

	/* Without a home there is no file. */
	if (!store->present)
		return 0;

	/* One writer a session. */
	writer = &store->writer;
	if (writer->started || writer->given)
		return EBUSY;

	/* The changes as they are now; none leaves the file as it is. */
	writer->count = kwl_settings_store_changes(store, writer->changes);
	if (writer->count == 0U)
		return 0;

	/* The file's path and folder, the generation it writes, and an empty result. */
	store_copy(writer->path, sizeof(writer->path), store->path);
	store_copy(writer->folder, sizeof(writer->folder), store->folder);
	writer->generation = store->generation;
	writer->done = 0;
	writer->error = 0;

	/* Succeeded: the writer holds the changes, for a thread to merge. */
	writer->given = 1;
	return 0;
}

/*
 * Merges the changes the writer was given into the file, on the thread
 * that runs it, and records how it went for kwl_settings_store_finish.
 */
void
kwl_settings_store_save_run(
	struct kwl_settings_writer *writer)
{
	int error;

	/* Merges the changes, away from the event loop. */
	error = store_merge(writer->folder, writer->path, writer->changes, writer->count);

	/* Records how it went; done says the merge is over. */
	(void)pthread_mutex_lock(&writer->lock);

	writer->error = error;
	writer->done = 1;

	(void)pthread_mutex_unlock(&writer->lock);
}

/*
 * Ends the session's writing, once, as the compositor ends: waits for the
 * writer, takes what it wrote as written, and merges now whatever changed
 * since (or everything, when the writer failed or never ran).  Returns 0
 * or the errno value of the last merge.
 */
int
kwl_settings_store_finish(
	struct kwl_settings_store *store)
{
	struct kwl_settings_writer *writer;
	int error;

	/* Waits for the writer, when one was started. */
	writer = &store->writer;
	if (writer->started) {
		/* Joins it; clearing started records that no thread runs now. */
		(void)pthread_join(writer->thread, NULL);
		writer->started = 0;

		/* What it wrote is in the file: those settings start from it now. */
		if (writer->error == 0)
			store_mark_saved(store, writer->changes, writer->count);
	}

	/* What a writer given to the caller's thread wrote there is in the file (done 0: it never ran). */
	if (writer->given && writer->done && writer->error == 0)
		store_mark_saved(store, writer->changes, writer->count);

	/* No writer is given from now on. */
	writer->given = 0;

	/* Merges now whatever the writer did not write. */
	error = kwl_settings_store_save(store);
	if (error != 0)
		return error;

	/* Succeeded: the file holds the session's settings. */
	return 0;
}

/* Sets an entry's value and whether it was chosen, and counts a change of a kept setting. */
static void
store_entry_set(
	struct kwl_settings_store *store,
	struct kwl_settings_entry *entry,
	const char *value,
	unsigned chosen)
{
	int differs;

	/*
	 * generation tells kwl_settings_store_finish that something changed
	 * after the writer took its copy.  Only a change of the value or of
	 * whether it was chosen counts.
	 */
	differs = strcmp(entry->value, value);
	if (differs != 0 ||
	    entry->chosen != chosen ||
	    !entry->known)
		store->generation++;

	/* Puts the value into effect; it is known from now on. */
	store_copy(entry->value, sizeof(entry->value), value);
	entry->chosen = chosen;
	entry->known = 1;
}

/* Reads each line of the file's text. */
static void
store_parse(
	struct kwl_settings_store *store,
	const char *text,
	size_t length)
{
	size_t start;
	size_t end;

	/* Each line, the last one with or without its newline. */
	start = 0;
	while (start < length) {
		/* Finds the line's end. */
		end = start;
		while (end < length && text[end] != '\n')
			end++;

		/* Takes the line's setting, if it is a line of one. */
		store_parse_line(store, text + start, end - start);

		/* Moves past the newline to the next line. */
		start = end + 1U;
	}
}

/* Takes a key=value line's setting; a comment, an empty line or a line not well formed is passed over. */
static void
store_parse_line(
	struct kwl_settings_store *store,
	const char *line,
	size_t length)
{
	struct kwl_settings_entry *entry;
	char key[KL_SETTINGS_KEY_MAX];
	char raw[KL_SETTINGS_VALUE_MAX];
	char clean[KL_SETTINGS_VALUE_MAX];
	const char *equals;
	size_t key_length;
	size_t value_length;
	int error;

	/* An empty line and a comment hold no setting. */
	if (length == 0U || line[0] == '#')
		return;

	/* Finds the first '='; a line without one holds no setting. */
	equals = memchr(line, '=', length);
	if (equals == NULL)
		return;

	/* The key before it must be there and fit. */
	key_length = (size_t)(equals - line);
	value_length = length - key_length - 1U;
	if (key_length == 0U || key_length >= sizeof(key))
		return;

	/* The value after it must fit. */
	if (value_length >= sizeof(raw))
		return;

	/* Copies the key and the value out as strings. */
	memcpy(key, line, key_length);
	key[key_length] = '\0';
	memcpy(raw, equals + 1, value_length);
	raw[value_length] = '\0';

	/* Only a setting the store holds. */
	entry = kwl_settings_store_find(store, key);
	if (entry == NULL)
		return;

	/* Not one the store only reports. */
	if ((entry->key->flags & KL_SETTINGS_KEY_KEPT) == 0U)
		return;

	/* A key given twice keeps its first value (the merge keeps only one line of a key). */
	if (entry->start_chosen)
		return;

	/* A value the setting cannot take is passed over. */
	error = store_clean_value(entry->key, raw, clean, sizeof(clean));
	if (error != 0)
		return;

	/* Keeps the value as the start the session's end compares with. */
	entry->start_chosen = 1;
	store_copy(entry->start, sizeof(entry->start), clean);

	/* It is in effect too, except the sound's, which audiod holds (volume.c gives it to audiod). */
	if (entry->known) {
		store_copy(entry->value, sizeof(entry->value), clean);
		entry->chosen = 1;
	}
}

/* Gives a value read from the file as its setting takes it: a number moved into its range, a path that is absolute; EINVAL for one it cannot take. */
static int
store_clean_value(
	const struct kl_settings_key *key,
	const char *raw,
	char *value,
	size_t size)
{
	int number;
	int valid;
	int error;

	/* Any value must be well formed. */
	valid = kl_settings_value_valid(raw);
	if (!valid)
		return EINVAL;

	/* A path is absolute, and kept as it is. */
	if (key->type == KL_SETTINGS_TYPE_PATH) {
		/* A relative path is refused. */
		if (raw[0] != '/')
			return EINVAL;

		/* Gives the path as it is. */
		(void)snprintf(value, size, "%s", raw);

		/* Succeeded: the path as the setting takes it. */
		return 0;
	}

	/* A number within its range. */
	error = kl_settings_key_number(key, raw, &number);
	if (error != 0)
		return EINVAL;

	/* Gives the number, moved into its range. */
	(void)snprintf(value, size, "%d", number);

	/* Succeeded: the value as the setting takes it. */
	return 0;
}

/* Tells whether an entry is kept in the file, known, and differs from what the file held at the start. */
static int
store_entry_differs(
	const struct kwl_settings_entry *entry)
{
	int differs;

	/* A setting never kept is not written. */
	if ((entry->key->flags & KL_SETTINGS_KEY_KEPT) == 0U)
		return 0;

	/* Nor one not known yet. */
	if (!entry->known)
		return 0;

	/* Chosen now and not at the start, or the other way round. */
	if (entry->chosen != entry->start_chosen)
		return 1;

	/* Both at the default. */
	if (!entry->chosen)
		return 0;

	/* Both chosen: a different value. */
	differs = strcmp(entry->value, entry->start);
	if (differs != 0)
		return 1;

	/* The same as the file. */
	return 0;
}

/* Makes the written changes the start of their settings: the file holds them now. */
static void
store_mark_saved(
	struct kwl_settings_store *store,
	const struct kwl_settings_change *changes,
	unsigned count)
{
	struct kwl_settings_entry *entry;
	unsigned index;

	/* Updates the start of each setting written. */
	for (index = 0; index < count; index++) {
		/* A change the store no longer holds has no start to update. */
		entry = kwl_settings_store_find(store, changes[index].key);
		if (entry == NULL)
			continue;

		/* The file's value is the change's. */
		entry->start_chosen = changes[index].chosen;
		store_copy(entry->start, sizeof(entry->start), changes[index].value);
	}
}

/* Merges changes into the file under its lock: read again, each change's key put in or taken out, the rest kept; returns 0 or an errno value. */
static int
store_merge(
	const char *folder,
	const char *path,
	const struct kwl_settings_change *changes,
	unsigned count)
{
	const char *value;
	char *before;
	char *after;
	char *swap;
	size_t before_length;
	size_t after_length;
	unsigned index;
	int lock;
	int error;

	/* Allocates room for the file as it is. */
	before = malloc(STORE_FILE_MAX);
	if (before == NULL)
		return ENOMEM;

	/* Allocates room for the file as it is to be. */
	after = malloc(STORE_FILE_MAX);
	if (after == NULL) {
		free(before);
		return ENOMEM;
	}

	/* Makes the folder, then takes the lock beside the file (held until closed). */
	store_mkdir(folder);
	lock = store_lock(path);
	if (lock < 0) {
		error = errno;
		free(after);
		free(before);
		return error;
	}

	/* Reads the file as it is now, under the lock. */
	error = store_load(path, before, STORE_FILE_MAX, &before_length);
	if (error != 0)
		goto cleanup;

	/* Applies each change in turn, the text of one change the start of the next. */
	for (index = 0; index < count; index++) {
		/* A setting back at its default leaves the file. */
		value = NULL;
		if (changes[index].chosen)
			value = changes[index].value;

		/* Writes the text with this key changed. */
		error = store_compose(before, before_length, changes[index].key, value, after, STORE_FILE_MAX, &after_length);
		if (error != 0)
			goto cleanup;

		/* The new text is the one the next change works on. */
		swap = before;
		before = after;
		after = swap;
		before_length = after_length;
	}

	/* Puts the new text in place of the old file. */
	error = store_replace(path, before, before_length);

cleanup:
	/* Lets the lock and the room go. */
	(void)close(lock);
	free(after);
	free(before);

	/* Reports why the file could not be changed. */
	if (error != 0)
		return error;

	/* Succeeded: the file holds the changes. */
	return 0;
}

/* Reads a file's whole text (a missing file is empty); returns 0, E2BIG for a file too large, or an errno value. */
static int
store_load(
	const char *path,
	char *text,
	size_t capacity,
	size_t *length)
{
	ssize_t count;
	int descriptor;
	int error;

	/* Starts with an empty text. */
	*length = 0;

	/* Opens the file, when there is one. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0) {
		/* A missing file is an empty text. */
		if (errno == ENOENT)
			return 0;

		/* Reports why the file could not be opened. */
		return errno;
	}

	/* Reads every byte, up to the room there is. */
	error = 0;
	for (;;) {
		/* A file that fills the room is too large. */
		if (*length == capacity) {
			error = E2BIG;
			break;
		}

		/* Reads the next bytes; an interruption is tried again. */
		count = read(descriptor, text + *length, capacity - *length);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			error = errno;
			break;
		}

		/* Nothing more is the end. */
		if (count == 0)
			break;

		/* The bytes join the text. */
		*length += (size_t)count;
	}

	/* Closes the file, which is not needed any more. */
	(void)close(descriptor);

	/* Reports why the text could not be read. */
	if (error != 0)
		return error;

	/* Succeeded: the text is read. */
	return 0;
}

/* Writes a text with one key set (value) or taken out (NULL): the key's first line replaced, its other lines dropped, every other line kept; returns 0 or E2BIG. */
static int
store_compose(
	const char *old_text,
	size_t old_length,
	const char *key,
	const char *value,
	char *text,
	size_t capacity,
	size_t *length)
{
	size_t start;
	size_t end;
	int written;
	int matched;
	int error;

	/* Nothing written yet, and the key not yet met. */
	*length = 0;
	written = 0;

	/* Copies each line of the old text, changing the key's. */
	start = 0;
	while (start < old_length) {
		/* Finds the line's end. */
		end = start;
		while (end < old_length && old_text[end] != '\n')
			end++;

		/* The key's line: its first becomes the new value (or goes), the others go. */
		matched = store_line_is(old_text + start, end - start, key);
		if (matched) {
			/* Writes the new value in place of the key's first line, unless the key leaves the file. */
			if (!written && value != NULL) {
				error = store_append_line(text, capacity, length, key, value);
				if (error != 0)
					return error;
			}

			/* written records that the key was met, so its later lines go. */
			written = 1;
		} else {
			/* Any other line stays as it was. */
			error = store_append(text, capacity, length, old_text + start, end - start);
			if (error != 0)
				return error;

			/* With its newline. */
			error = store_append(text, capacity, length, "\n", 1U);
			if (error != 0)
				return error;
		}

		/* Moves past the newline to the next line. */
		start = end + 1U;
	}

	/* A key the file did not have goes at its end, unless it leaves the file. */
	if (!written && value != NULL) {
		error = store_append_line(text, capacity, length, key, value);
		if (error != 0)
			return error;
	}

	/* Succeeded: the new text is written. */
	return 0;
}

/* Adds a part to a text; returns 0, or E2BIG when it does not fit. */
static int
store_append(
	char *text,
	size_t capacity,
	size_t *length,
	const char *part,
	size_t part_length)
{
	/* A part that does not fit is not added. */
	if (part_length > capacity - *length)
		return E2BIG;

	/* Adds the part at the end. */
	memcpy(text + *length, part, part_length);
	*length += part_length;

	/* Succeeded: the part is added. */
	return 0;
}

/* Adds a key=value line to a text; returns 0 or E2BIG. */
static int
store_append_line(
	char *text,
	size_t capacity,
	size_t *length,
	const char *key,
	const char *value)
{
	int error;

	/* Adds the key. */
	error = store_append(text, capacity, length, key, strlen(key));
	if (error != 0)
		return error;

	/* Adds the '='. */
	error = store_append(text, capacity, length, "=", 1U);
	if (error != 0)
		return error;

	/* Adds the value. */
	error = store_append(text, capacity, length, value, strlen(value));
	if (error != 0)
		return error;

	/* Ends the line. */
	error = store_append(text, capacity, length, "\n", 1U);
	if (error != 0)
		return error;

	/* Succeeded: the line is added. */
	return 0;
}

/* Tells whether a line is the key's (starts with the key and '='). */
static int
store_line_is(
	const char *line,
	size_t length,
	const char *key)
{
	size_t key_length;
	int differs;

	/* The line must hold the key and the '='. */
	key_length = strlen(key);
	if (length <= key_length)
		return 0;

	/* The line must start with the key's characters. */
	differs = memcmp(line, key, key_length);
	if (differs != 0)
		return 0;

	/* Then the '='. */
	if (line[key_length] != '=')
		return 0;

	/* It is the key's line. */
	return 1;
}

/* Writes a new file beside the file, flushes it and renames it over the file; returns 0 or an errno value. */
static int
store_replace(
	const char *path,
	const char *text,
	size_t length)
{
	char temporary[KWL_SETTINGS_PATH_MAX + 8U];
	ssize_t count;
	size_t done;
	int descriptor;
	int status;
	int error;

	/* A new file with a name of its own beside the file, the user's alone. */
	(void)snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
	descriptor = mkstemp(temporary);
	if (descriptor < 0)
		return errno;

	/* Writes every byte of the text. */
	error = 0;
	done = 0;
	while (done < length) {
		/* Writes what is left; an interruption is tried again. */
		count = write(descriptor, text + done, length - done);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			error = errno;
			break;
		}

		/* The bytes written. */
		done += (size_t)count;
	}

	/* Flushes the text to the disk before it takes the file's name. */
	if (error == 0) {
		status = fsync(descriptor);
		if (status != 0)
			error = errno;
	}

	/* The file is closed either way; a closing that fails is a failed write. */
	status = close(descriptor);
	if (error == 0 && status != 0)
		error = EIO;

	/* A failed write leaves the file as it was. */
	if (error != 0) {
		(void)unlink(temporary);
		return error;
	}

	/* Renames the new file over the file, which replaces it at once. */
	status = rename(temporary, path);
	if (status != 0) {
		error = errno;
		(void)unlink(temporary);
		return error;
	}

	/* Succeeded: the file holds the new text. */
	return 0;
}

/* Takes the lock beside the file; returns its descriptor, or -1 with errno set. */
static int
store_lock(
	const char *path)
{
	char lock_path[KWL_SETTINGS_PATH_MAX + 8U];
	int descriptor;
	int status;
	int error;

	/* Opens the lock file, making it when it is missing. */
	(void)snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
	descriptor = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (descriptor < 0)
		return -1;

	/* Takes the lock, held until the descriptor is closed. */
	status = flock(descriptor, LOCK_EX);
	if (status != 0) {
		error = errno;
		(void)close(descriptor);
		errno = error;
		return -1;
	}

	/* Succeeded: the lock is held. */
	return descriptor;
}

/* Makes the file's folder and the missing folders above it (as mkdir -p), for the user alone. */
static void
store_mkdir(
	const char *folder)
{
	char partial[KWL_SETTINGS_PATH_MAX];
	size_t index;

	/* Makes each prefix that ends at a slash. */
	store_copy(partial, sizeof(partial), folder);
	for (index = 1; partial[index] != '\0'; index++) {
		/* Only a slash ends a prefix. */
		if (partial[index] != '/')
			continue;

		/* Makes the folder up to this slash; one that exists is left alone. */
		partial[index] = '\0';
		(void)mkdir(partial, 0700);
		partial[index] = '/';
	}

	/* Makes the folder itself. */
	(void)mkdir(partial, 0700);
}

/* The writer's own thread: merges the changes it was given and records how it went. */
static void *
store_writer_run(
	void *argument)
{
	struct kwl_settings_writer *writer;

	/* The writer that started the thread. */
	writer = argument;

	/* Merges them, recording how it went for kwl_settings_store_finish after the join. */
	kwl_settings_store_save_run(writer);

	/* Succeeded: the thread ends. */
	return NULL;
}

/* Copies a string into room of its own, cut to fit; the source may lie in the same object. */
static void
store_copy(
	char *to,
	size_t size,
	const char *from)
{
	size_t length;

	/* Copies the bytes that fit, then the NUL. */
	length = strlen(from);
	if (length >= size)
		length = size - 1U;
	memmove(to, from, length);
	to[length] = '\0';
}
