/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The search of files (spec §7): the items under a folder whose
 * name, extension and kind match the words typed.
 *
 * Words are all required.  kind:image (text, code,
 * audio, video, archive, pdf, folder, program) a kind, .png, *.png or
 * ext:png an extension (any of those given), and any other word is part of
 * the name (without regard to ASCII case).  The folders are walked a few
 * entries at a time in the main loop, so results come as they are found
 * and the window keeps answering; a search stops at FM_SEARCH_RESULTS.
 */

#include "files.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* How many entries one call looks at before it checks the time. */
#define SEARCH_BATCH		64

/*
 * The kinds a kind: word names, and their categories.
 */
struct search_kind {
	const char *word;
	unsigned category;
};

/* The words of kind:. */
static const struct search_kind search_kinds[] = {
	{ "folder", FM_CATEGORY_FOLDER },
	{ "text", FM_CATEGORY_TEXT },
	{ "code", FM_CATEGORY_CODE },
	{ "image", FM_CATEGORY_IMAGE },
	{ "audio", FM_CATEGORY_AUDIO },
	{ "video", FM_CATEGORY_VIDEO },
	{ "archive", FM_CATEGORY_ARCHIVE },
	{ "pdf", FM_CATEGORY_PDF },
	{ "program", FM_CATEGORY_EXECUTABLE },
	{ "document", FM_CATEGORY_DOCUMENT }
};

static void search_parse(struct fm_search *search, const char *query);
static void search_word(struct fm_search *search, const char *word);
static void search_extension(struct fm_search *search, const char *extension);
static int search_passed(const struct fm_search *search, const char *name);
static int search_matches(struct fm_search *search, const struct fm_entry *entry);
static int search_contains(const char *text, const char *word);
static int search_push(struct fm_search *search, const char *path);
static void search_pop(struct fm_search *search);
static int search_skipped(const char *path);

/*
 * Starts a search for a query under a folder (the walk begins at the next
 * step); the results go into the listing, emptied first.
 */
void
fm_search_start(
	struct fm_search *search,
	const char *query,
	const char *base,
	int hidden)
{
	/* The old walk ends. */
	fm_search_stop(search);

	/* The query's words and the base folder. */
	search_parse(search, query);
	snprintf(search->base, sizeof(search->base), "%s", base);
	search->hidden = hidden;
	search->active = 1;
	search->visited = 0;

	/* The walk starts at the base. */
	(void)search_push(search, base);
}

/*
 * Walks on for about a budget of milliseconds, adding matches to the
 * listing; returns 1 while the search goes on, 0 once it is done.
 */
int
fm_search_step(
	struct fm_search *search,
	struct fm_listing *listing,
	uint64_t budget_ms)
{
	struct search_walk *walk;
	struct dirent *item;
	struct fm_entry *entry;
	uint64_t deadline;
	uint64_t now;
	char path[FM_PATH_MAX];
	int batch;
	int matched;
	int passed;
	int skipped;
	int walk_into;

	/* A finished search does nothing. */
	if (search->active == 0)
		return 0;

	/* The time this round ends. */
	now = fm_ops_clock();
	deadline = now + budget_ms;

	/* Entries, in batches, until the time is up. */
	for (;;) {
		for (batch = 0; batch < SEARCH_BATCH; batch++) {
			/* The walk is over when no folder is left, or enough was found. */
			if (search->walk_count == 0U || listing->count >= FM_SEARCH_RESULTS) {
				fm_search_stop(search);
				return 0;
			}

			/* The next entry of the folder on top; a folder read to its end is left. */
			walk = &search->walks[search->walk_count - 1U];
			item = readdir(walk->directory);
			if (item == NULL) {
				search_pop(search);
				continue;
			}

			/* The folder itself and its parent are passed over, and hidden entries unless they are shown. */
			passed = search_passed(search, item->d_name);
			if (passed != 0)
				continue;
			search->visited++;

			/* The entry, measured as a listing's entry is. */
			entry = fm_dir_add(listing, walk->path, item->d_name);
			if (entry == NULL) {
				fm_search_stop(search);
				return 0;
			}

			/* A folder (not a link to one) is walked too, unless it is one of the system's virtual ones. */
			walk_into = 0;
			if (entry->folder != 0 && entry->link == 0)
				walk_into = 1;
			snprintf(path, sizeof(path), "%s", entry->path);

			/* A match stays in the listing with its folder shown; any other goes again. */
			matched = search_matches(search, entry);
			if (matched != 0) {
				entry->detail = strdup(walk->path);
			} else {
				free(entry->name);
				free(entry->path);
				listing->count--;
			}

			/* The folder is walked after the ones above it. */
			if (walk_into == 0)
				continue;
			skipped = search_skipped(path);
			if (skipped == 0)
				(void)search_push(search, path);
		}

		/* The time is up: the rest in the next round. */
		now = fm_ops_clock();
		if (now >= deadline)
			return 1;
	}
}

/*
 * Stops a search, closing its folders.
 */
void
fm_search_stop(
	struct fm_search *search)
{
	/* Every open folder. */
	while (search->walk_count != 0U)
		search_pop(search);

	/* The walk's table, and the search is over. */
	free(search->walks);
	search->walks = NULL;
	search->walk_capacity = 0;
	search->active = 0;
}

/* Splits a query into its words: names, extensions and kinds. */
static void
search_parse(
	struct fm_search *search,
	const char *query)
{
	char copy[FM_SEARCH_QUERY];
	char *word;
	char *end;

	/* Nothing yet. */
	search->name_count = 0;
	search->extension_count = 0;
	search->categories = 0;

	/* Each word between spaces, sorted by what it asks for. */
	snprintf(copy, sizeof(copy), "%s", query);
	word = copy;
	for (;;) {
		/* The spaces before the word. */
		while (*word == ' ')
			word++;
		if (*word == '\0')
			break;

		/* The word ends at the next space or the end. */
		end = strchr(word, ' ');
		if (end != NULL)
			*end = '\0';
		search_word(search, word);
		if (end == NULL)
			break;
		word = end + 1;
	}
}

/* Takes one word of a query: a kind, an extension, or part of the name. */
static void
search_word(
	struct fm_search *search,
	const char *word)
{
	size_t index;
	int match;

	/* kind:WORD wants a kind. */
	match = strncasecmp(word, "kind:", 5);
	if (match == 0) {
		for (index = 0; index < sizeof(search_kinds) / sizeof(search_kinds[0]); index++) {
			match = strcasecmp(word + 5, search_kinds[index].word);
			if (match == 0)
				search->categories |= 1U << search_kinds[index].category;
		}

		/* The kind is taken. */
		return;
	}

	/* ext:png, *.png and .png want an extension. */
	match = strncasecmp(word, "ext:", 4);
	if (match == 0) {
		search_extension(search, word + 4);
		return;
	}

	/* A star and a dot before the extension. */
	if (word[0] == '*' && word[1] == '.') {
		search_extension(search, word + 2);
		return;
	}

	/* A dot before the extension. */
	if (word[0] == '.' && word[1] != '\0') {
		search_extension(search, word + 1);
		return;
	}

	/* Anything else is part of the name. */
	if (search->name_count < FM_SEARCH_WORDS) {
		snprintf(search->names[search->name_count], FM_SEARCH_WORD, "%.63s", word);
		search->name_count++;
	}
}

/* Adds an extension a search wants (any of them will do). */
static void
search_extension(
	struct fm_search *search,
	const char *extension)
{
	/* Only as many as the search keeps. */
	if (search->extension_count == FM_SEARCH_WORDS)
		return;

	/* The extension after the others. */
	snprintf(search->extensions[search->extension_count], FM_SEARCH_WORD, "%.63s", extension);
	search->extension_count++;
}

/* Tells whether an entry's name is passed over: the folder itself, its parent, and hidden names unless they are shown. */
static int
search_passed(
	const struct fm_search *search,
	const char *name)
{
	/* A name not starting with a dot is looked at. */
	if (name[0] != '.')
		return 0;

	/* The folder itself and its parent. */
	if (name[1] == '\0')
		return 1;
	if (name[1] == '.' && name[2] == '\0')
		return 1;

	/* A hidden name, when hidden items are not shown. */
	if (search->hidden == 0)
		return 1;

	/* A hidden name that is shown. */
	return 0;
}

/* Tells whether an entry matches every word of the search. */
static int
search_matches(
	struct fm_search *search,
	const struct fm_entry *entry)
{
	const char *dot;
	int index;
	int found;
	int match;

	/* An empty query matches nothing. */
	if (search->name_count == 0 &&
	    search->extension_count == 0 &&
	    search->categories == 0U)
		return 0;

	/* Every name word is in the name. */
	for (index = 0; index < search->name_count; index++) {
		found = search_contains(entry->name, search->names[index]);
		if (found == 0)
			return 0;
	}

	/* One of the extensions is the name's. */
	if (search->extension_count != 0) {
		dot = strrchr(entry->name, '.');
		if (dot == NULL || dot == entry->name)
			return 0;

		/* Any of them will do. */
		found = 0;
		for (index = 0; index < search->extension_count; index++) {
			match = strcasecmp(dot + 1, search->extensions[index]);
			if (match == 0)
				found = 1;
		}

		/* None of them is the name's. */
		if (found == 0)
			return 0;
	}

	/* One of the kinds is the entry's. */
	if (search->categories != 0U && (search->categories & (1U << entry->mime->category)) == 0U)
		return 0;

	/* Every word matched. */
	return 1;
}

/* Tells whether a word is in a text, without regard to ASCII case. */
static int
search_contains(
	const char *text,
	const char *word)
{
	size_t length;
	int match;

	/* At each place of the text. */
	length = strlen(word);
	for (; *text != '\0'; text++) {
		match = strncasecmp(text, word, length);
		if (match == 0)
			return 1;
	}

	/* Not found. */
	return 0;
}

/* Opens a folder and puts it on the walk; nonzero when it cannot be read. */
static int
search_push(
	struct fm_search *search,
	const char *path)
{
	struct search_walk *grown;
	struct search_walk *walk;
	DIR *directory;
	size_t capacity;

	/* The folder. */
	directory = opendir(path);
	if (directory == NULL)
		return errno;

	/* Room on the walk. */
	if (search->walk_count == search->walk_capacity) {
		capacity = search->walk_capacity + 16U;
		grown = realloc(search->walks, capacity * sizeof(search->walks[0]));
		if (grown == NULL) {
			closedir(directory);
			return ENOMEM;
		}

		/* The table has room. */
		search->walks = grown;
		search->walk_capacity = capacity;
	}

	/* The folder on top. */
	walk = &search->walks[search->walk_count];
	walk->directory = directory;
	snprintf(walk->path, sizeof(walk->path), "%s", path);
	search->walk_count++;

	/* Succeeded: it is walked next. */
	return 0;
}

/* Closes the folder on top of the walk. */
static void
search_pop(
	struct fm_search *search)
{
	/* The folder is closed and leaves the walk. */
	search->walk_count--;
	closedir(search->walks[search->walk_count].directory);
}

/* Tells whether a folder is one of the system's virtual ones, which a search of the computer leaves out. */
static int
search_skipped(
	const char *path)
{
	static const char *const virtual_folders[] = { "/dev", "/proc", "/sys", "/run" };
	size_t index;
	int match;

	/* Each of them (only a search of the whole computer comes near them). */
	for (index = 0; index < sizeof(virtual_folders) / sizeof(virtual_folders[0]); index++) {
		match = strcmp(path, virtual_folders[index]);
		if (match == 0)
			return 1;
	}

	/* An ordinary folder. */
	return 0;
}
