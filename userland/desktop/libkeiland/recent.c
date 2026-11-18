/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop's list of recently used files (WS071, plan/ws071/design.md
 * section 6.2): one list for all applications, so that the file manager's
 * Recents and any application's "open recent" show the same files.
 *
 * The list is a text file, $XDG_DATA_HOME/keiland/recent (by default
 * ~/.local/share/keiland/recent), a line an entry, oldest first:
 *
 *     TIME<TAB>APPLICATION<TAB>PATH
 *
 * TIME is in seconds since the epoch.  A path is listed once (using it
 * again moves it to the end), and only the newest KL_RECENT_KEPT
 * entries are kept.  A change is made under an flock of a lock file beside
 * the list and written to a new file renamed over the old one, so readers
 * never see half a list and two applications do not lose each other's
 * entries.
 *
 * The user may stop the list (q824, WS148 p001 in the git history: Settings'
 * Storage, "Keep recent items"): a file recent.off beside the list says
 * so, the list is emptied, and kl_recent_add adds nothing until the file
 * is gone.  Files' Recents empties the list with kl_recent_clear.
 *
 * kl_recent_stamp (ws177-p008) lets a program that shows the list learn
 * that another changed it: each change writes a new file renamed over
 * the old one, so the list's inode, size and time, with whether it is
 * stopped, change with every change.
 */

#include <keiland/keiland.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The longest line read (a path, an application and a time). */
#define RECENT_LINE_MAX		(KL_RECENT_PATH_MAX + KL_RECENT_NAME_MAX + 32)

/*
 * One entry of the list as it is read and written: an allocated path, the
 * application's name and the time.
 */
struct recent_entry {
	char *path;
	char application[KL_RECENT_NAME_MAX];
	int64_t time;
};

static int recent_file(char *path, size_t size);
static int recent_off_file(char *path, size_t size);
static int recent_lock(const char *list);
static int recent_read(const char *list, struct recent_entry **entries, size_t *count);
static int recent_write(const char *list, const struct recent_entry *entries, size_t count);
static void recent_free(struct recent_entry *entries, size_t count);
static void recent_mkdir(const char *path);

/*
 * Adds a path to the recent list (or moves it to the newest place), with
 * the application that used it.
 *
 * Returns 0, or an errno value.
 */
int
kl_recent_add(
	const char *path,
	const char *application)
{
	struct recent_entry *entries;
	struct recent_entry *grown;
	char list[KL_RECENT_PATH_MAX];
	size_t length;
	size_t count;
	size_t index;
	size_t kept;
	int lock;
	int error;
	int match;
	int keep;

	/* Only an absolute path of a sane length. */
	if (path == NULL || path[0] != '/')
		return EINVAL;
	length = strlen(path);
	if (length >= KL_RECENT_PATH_MAX)
		return EINVAL;
	if (application == NULL)
		application = "";

	/* Nothing is added while the user keeps no list. */
	error = kl_recent_keep(&keep);
	if (error != 0)
		return error;
	if (!keep)
		return 0;

	/* The list's file and its lock. */
	error = recent_file(list, sizeof(list));
	if (error != 0)
		return error;
	lock = recent_lock(list);
	if (lock < 0)
		return errno;

	/* The entries as they are. */
	error = recent_read(list, &entries, &count);
	if (error != 0) {
		close(lock);
		return error;
	}

	/* The path's older entry goes. */
	kept = 0;
	for (index = 0; index < count; index++) {
		match = strcmp(entries[index].path, path);
		if (match == 0) {
			free(entries[index].path);
			continue;
		}

		/* Any other entry is kept. */
		entries[kept] = entries[index];
		kept++;
	}

	/* The kept entries are the list. */
	count = kept;

	/* The new entry at the end. */
	grown = realloc(entries, (count + 1U) * sizeof(entries[0]));
	if (grown == NULL) {
		recent_free(entries, count);
		close(lock);
		return ENOMEM;
	}

	/* The table has room for it. */
	entries = grown;
	entries[count].path = strdup(path);
	if (entries[count].path == NULL) {
		recent_free(entries, count);
		close(lock);
		return ENOMEM;
	}

	/* Its application and time. */
	snprintf(entries[count].application, sizeof(entries[count].application), "%s", application);
	entries[count].time = (int64_t)time(NULL);
	count++;

	/* The newest are written back (the oldest beyond the limit are left out). */
	index = 0;
	if (count > KL_RECENT_KEPT)
		index = count - KL_RECENT_KEPT;
	error = recent_write(list, entries + index, count - index);
	recent_free(entries, count);
	close(lock);

	/* Reports a list that could not be written. */
	if (error != 0)
		return error;

	/* Succeeded: the path is the newest entry. */
	return 0;
}

/*
 * Reads the recent list, newest first, into up to capacity items.
 *
 * Returns 0 (an absent list is empty), or an errno value.
 */
int
kl_recent_list(
	struct kl_recent_item *items,
	size_t capacity,
	size_t *count)
{
	struct recent_entry *entries;
	char list[KL_RECENT_PATH_MAX];
	size_t total;
	size_t index;
	int error;

	/* Nothing yet. */
	*count = 0;

	/* The list's file and its entries. */
	error = recent_file(list, sizeof(list));
	if (error != 0)
		return error;
	error = recent_read(list, &entries, &total);
	if (error != 0)
		return error;

	/* The newest first, as many as fit. */
	for (index = total; index > 0 && *count < capacity; index--) {
		snprintf(items[*count].path, sizeof(items[*count].path), "%s", entries[index - 1U].path);
		snprintf(items[*count].application, sizeof(items[*count].application), "%s", entries[index - 1U].application);
		items[*count].time = entries[index - 1U].time;
		(*count)++;
	}

	/* The entries read are not needed any more. */
	recent_free(entries, total);

	/* Succeeded: the items hold the newest entries. */
	return 0;
}

/*
 * Removes a path from the recent list (a file deleted, or one the user
 * does not want listed).
 *
 * Returns 0 (also when it was not listed), or an errno value.
 */
int
kl_recent_remove(
	const char *path)
{
	struct recent_entry *entries;
	char list[KL_RECENT_PATH_MAX];
	size_t count;
	size_t index;
	size_t kept;
	int lock;
	int error;
	int match;

	/* The list's file and its lock. */
	error = recent_file(list, sizeof(list));
	if (error != 0)
		return error;
	lock = recent_lock(list);
	if (lock < 0)
		return errno;

	/* The entries but the path's. */
	error = recent_read(list, &entries, &count);
	if (error != 0) {
		close(lock);
		return error;
	}

	/* The entries but the path. */
	kept = 0;
	for (index = 0; index < count; index++) {
		match = strcmp(entries[index].path, path);
		if (match == 0) {
			free(entries[index].path);
			continue;
		}

		/* Any other entry is kept. */
		entries[kept] = entries[index];
		kept++;
	}

	/* Written back. */
	error = recent_write(list, entries, kept);
	recent_free(entries, kept);
	close(lock);
	if (error != 0)
		return error;

	/* Succeeded: the path is not listed. */
	return 0;
}

/*
 * Empties the recent list.
 *
 * Returns 0, or an errno value.
 */
int
kl_recent_clear(void)
{
	char list[KL_RECENT_PATH_MAX];
	int lock;
	int error;

	/* The list's file and its lock. */
	error = recent_file(list, sizeof(list));
	if (error != 0)
		return error;
	lock = recent_lock(list);
	if (lock < 0)
		return errno;

	/* No entry written back. */
	error = recent_write(list, NULL, 0U);
	close(lock);
	if (error != 0)
		return error;

	/* Succeeded: the list is empty. */
	return 0;
}

/*
 * Chooses whether the recent list is kept: 0 empties it and stops it, 1
 * starts it again.
 *
 * Returns 0, or an errno value.
 */
int
kl_recent_set_keep(
	int keep)
{
	char off[KL_RECENT_PATH_MAX];
	int descriptor;
	int status;
	int error;

	/* The file that says the list is stopped. */
	error = recent_off_file(off, sizeof(off));
	if (error != 0)
		return error;

	/* Kept again: the file goes (none is the same). */
	if (keep) {
		status = unlink(off);
		if (status != 0 && errno != ENOENT)
			return errno;
		return 0;
	}

	/* Stopped: the file first, so that no add comes between, then the list emptied. */
	descriptor = open(off, O_WRONLY | O_CREAT, 0600);
	if (descriptor < 0)
		return errno;
	close(descriptor);
	error = kl_recent_clear();
	if (error != 0)
		return error;

	/* Succeeded: the list is stopped and empty. */
	return 0;
}

/*
 * Tells whether the recent list is kept: *keep is 1, or 0 while the user
 * stopped it.
 *
 * Returns 0, or an errno value.
 */
int
kl_recent_keep(
	int *keep)
{
	struct stat status;
	char off[KL_RECENT_PATH_MAX];
	int result;
	int error;

	/* Kept unless the stopping file is there. */
	*keep = 1;
	error = recent_off_file(off, sizeof(off));
	if (error != 0)
		return error;
	result = stat(off, &status);
	if (result == 0) {
		*keep = 0;
		return 0;
	}

	/* No file says kept; another failure is told. */
	if (errno != ENOENT)
		return errno;

	/* Succeeded: the list is kept. */
	return 0;
}

/*
 * Gives a stamp of the recent list that changes with every change of it:
 * its file's inode, size and time, and whether the list is stopped.  A
 * list not written yet stamps as empty.
 *
 * Returns 0, or an errno value.
 */
int
kl_recent_stamp(
	uint64_t *stamp)
{
	char list[KL_RECENT_PATH_MAX];
	char off[KL_RECENT_PATH_MAX];
	struct stat status;
	uint64_t value;
	int got;
	int error;

	/* The list's file and the one that stops it. */
	error = recent_file(list, sizeof(list));
	if (error != 0)
		return error;
	error = recent_off_file(off, sizeof(off));
	if (error != 0)
		return error;

	/* The list's file: a new one at each change (renamed over the old); none yet stamps as empty. */
	value = 0U;
	got = stat(list, &status);
	if (got == 0) {
		value = (uint64_t)status.st_ino;
		value = value * 1000003U + (uint64_t)status.st_size;
		value = value * 1000003U + (uint64_t)status.st_mtime;
	} else if (errno != ENOENT) {
		return errno;
	}

	/* Whether it is stopped, in the lowest bit. */
	value <<= 1;
	got = stat(off, &status);
	if (got == 0)
		value |= 1U;

	/* Succeeded: the stamp. */
	*stamp = value;
	return 0;
}

/* Writes the path of the file that stops the list (beside it); returns 0 or an errno value. */
static int
recent_off_file(
	char *path,
	size_t size)
{
	char list[KL_RECENT_PATH_MAX];
	int written;
	int error;

	/* The list's path, and .off after its name. */
	error = recent_file(list, sizeof(list));
	if (error != 0)
		return error;
	written = snprintf(path, size, "%s.off", list);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the file's path. */
	return 0;
}

/* Writes the list's path, making its folder; returns 0 or an errno value. */
static int
recent_file(
	char *path,
	size_t size)
{
	char folder[KL_RECENT_PATH_MAX];
	const char *data;
	const char *home;
	int written;

	/* $XDG_DATA_HOME/keiland, or ~/.local/share/keiland. */
	data = getenv("XDG_DATA_HOME");
	home = getenv("HOME");
	if (data != NULL && data[0] == '/') {
		written = snprintf(folder, sizeof(folder), "%s/keiland", data);
	} else if (home != NULL && home[0] == '/') {
		written = snprintf(folder, sizeof(folder), "%s/.local/share/keiland", home);
	} else {
		return ENOENT;
	}

	/* The folder path must fit. */
	if (written < 0 || (size_t)written >= sizeof(folder))
		return ENAMETOOLONG;

	/* The folder, and the list in it. */
	recent_mkdir(folder);
	written = snprintf(path, size, "%s/recent", folder);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the list's path. */
	return 0;
}

/* Takes the list's lock (a file beside it); returns its descriptor, or -1 with errno set. */
static int
recent_lock(
	const char *list)
{
	char path[KL_RECENT_PATH_MAX + 8];
	int descriptor;
	int status;

	/* The lock file. */
	snprintf(path, sizeof(path), "%s.lock", list);
	descriptor = open(path, O_RDWR | O_CREAT, 0600);
	if (descriptor < 0)
		return -1;

	/* Held until the descriptor is closed. */
	status = flock(descriptor, LOCK_EX);
	if (status != 0) {
		close(descriptor);
		return -1;
	}

	/* Succeeded: the lock is held. */
	return descriptor;
}

/* Reads the list's entries (an absent list has none); returns 0 or an errno value. */
static int
recent_read(
	const char *list,
	struct recent_entry **entries,
	size_t *count)
{
	struct recent_entry *grown;
	char line[RECENT_LINE_MAX];
	char *first_tab;
	char *second_tab;
	char *newline;
	char *read;
	FILE *file;
	long long seconds;

	/* Nothing yet. */
	*entries = NULL;
	*count = 0;

	/* The file, if there is one. */
	file = fopen(list, "r");
	if (file == NULL) {
		if (errno == ENOENT)
			return 0;
		return errno;
	}

	/* Each well-formed line. */
	for (;;) {
		read = fgets(line, sizeof(line), file);
		if (read == NULL)
			break;

		/* The three fields between the tabs. */
		newline = strchr(line, '\n');
		if (newline != NULL)
			*newline = '\0';
		first_tab = strchr(line, '\t');
		if (first_tab == NULL)
			continue;
		second_tab = strchr(first_tab + 1, '\t');
		if (second_tab == NULL || second_tab[1] != '/')
			continue;
		*first_tab = '\0';
		*second_tab = '\0';

		/* One more entry. */
		grown = realloc(*entries, (*count + 1U) * sizeof(grown[0]));
		if (grown == NULL)
			break;
		*entries = grown;
		seconds = strtoll(line, NULL, 10);
		grown[*count].time = (int64_t)seconds;
		snprintf(grown[*count].application, sizeof(grown[*count].application), "%s", first_tab + 1);
		grown[*count].path = strdup(second_tab + 1);
		if (grown[*count].path == NULL)
			break;
		(*count)++;
	}

	/* The file is not needed any more. */
	fclose(file);

	/* Succeeded: the entries are read. */
	return 0;
}

/* Writes the entries as the new list (a new file renamed over the old); returns 0 or an errno value. */
static int
recent_write(
	const char *list,
	const struct recent_entry *entries,
	size_t count)
{
	char temporary[KL_RECENT_PATH_MAX + 8];
	FILE *file;
	size_t index;
	int status;

	/* A new file beside the list. */
	snprintf(temporary, sizeof(temporary), "%s.new", list);
	file = fopen(temporary, "w");
	if (file == NULL)
		return errno;

	/* A line an entry. */
	for (index = 0; index < count; index++)
		fprintf(file, "%lld\t%s\t%s\n", (long long)entries[index].time, entries[index].application, entries[index].path);
	status = fclose(file);
	if (status != 0) {
		(void)unlink(temporary);
		return EIO;
	}

	/* It replaces the list at once. */
	status = rename(temporary, list);
	if (status != 0) {
		(void)unlink(temporary);
		return errno;
	}

	/* Succeeded: the list is written. */
	return 0;
}

/* Frees entries read from the list. */
static void
recent_free(
	struct recent_entry *entries,
	size_t count)
{
	size_t index;

	/* Each path, then the table. */
	for (index = 0; index < count; index++)
		free(entries[index].path);
	free(entries);
}

/* Makes a folder and the folders above it that are missing (as mkdir -p). */
static void
recent_mkdir(
	const char *path)
{
	char partial[KL_RECENT_PATH_MAX];
	size_t index;

	/* Each prefix that ends at a slash, then the whole path. */
	snprintf(partial, sizeof(partial), "%s", path);
	for (index = 1; partial[index] != '\0'; index++) {
		if (partial[index] != '/')
			continue;
		partial[index] = '\0';
		(void)mkdir(partial, 0700);
		partial[index] = '/';
	}

	/* The folder itself. */
	(void)mkdir(partial, 0700);
}
