/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The listing of a folder: its items with what lstat and stat say of them,
 * sorted the way the window shows them.
 *
 * Names are sorted as people read them: without regard to case, and with
 * runs of digits compared as numbers ("file 9" before "file 10").  Folders
 * come before files whatever the sort.
 */

#include "files.h"

#include <keiland/keiland.h>

#include <dirent.h>
#include <grp.h>
#include <pwd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* How many entries a listing grows by. */
#define DIR_GROWTH		64U

/* The byte units of sizes, which are decimal as a file's size is shown on the desktop. */
#define DIR_KILO		1000ULL

/*
 * The sort the comparison applies and whether it is reversed.
 *
 * qsort passes no context, so fm_dir_sort sets these just before it sorts;
 * nothing else reads them.
 */
static unsigned dir_sort_key;
static int dir_sort_reverse;

static int dir_compare(const void *first, const void *second);
static int dir_compare_names(const char *first, const char *second);
static char *dir_join(const char *folder, const char *name);
static int dir_is_dot(const char *name);
static int dir_read_into(struct fm_listing *listing, const char *path, int hidden);
static void dir_trash_details(struct fm_listing *listing, size_t first, const char *trash);

/*
 * Reads a folder's items into a listing (emptied first).
 *
 * Items whose names start with a dot are left out unless hidden is set.
 * Returns 0, or the errno value of a folder that cannot be read (also kept
 * in the listing).
 */
int
fm_dir_read(
	struct fm_listing *listing,
	const char *path,
	int hidden)
{
	int error;

	/* The listing starts empty. */
	fm_dir_free(listing);

	/* The folder's items are added to it. */
	error = dir_read_into(listing, path, hidden);
	if (error != 0)
		return error;

	/* Succeeded: the listing holds the folder's items. */
	return 0;
}

/*
 * Reads the items of every trash of the user into a listing (emptied
 * first): the home trash's and those at the tops of the mounted volumes
 * (ws127-p003), each with where it was (its folder, as detail) and when
 * it was trashed (extra_time) from its record.
 *
 * Returns 0, or the errno value of a list of trashes that cannot be made.
 */
int
fm_dir_read_trash(
	struct fm_listing *listing)
{
	char files[FM_PATH_MAX + 8];
	char **trashes;
	size_t trash_count;
	size_t first;
	size_t index;
	int error;

	/* The listing starts empty. */
	fm_dir_free(listing);

	/* The user's trashes. */
	error = fm_trash_list(&trashes, &trash_count);
	if (error != 0) {
		listing->error = error;
		return error;
	}

	/* The items of each trash's files folder, hidden ones too (they were trashed like any other). */
	for (index = 0; index < trash_count; index++) {
		first = listing->count;
		snprintf(files, sizeof(files), "%s/files", trashes[index]);
		(void)dir_read_into(listing, files, 1);
		dir_trash_details(listing, first, trashes[index]);
	}

	/* A trash that could not be read leaves the others listed. */
	listing->error = 0;
	fm_paths_free(trashes, trash_count);

	/* Succeeded: the listing holds the trashes' items. */
	return 0;
}

/*
 * Sorts a listing: folders first, then by the key (names break ties), in
 * reverse when asked.
 */
void
fm_dir_sort(
	struct fm_listing *listing,
	unsigned sort,
	int reverse)
{
	/* The comparison reads the key and the direction from these. */
	dir_sort_key = sort;
	dir_sort_reverse = reverse;

	/* Sorts the entries. */
	if (listing->count > 1U)
		qsort(listing->entries, listing->count, sizeof(listing->entries[0]), dir_compare);
}

/*
 * Frees a listing's entries and leaves it empty.
 */
void
fm_dir_free(
	struct fm_listing *listing)
{
	size_t index;

	/* Each entry's strings. */
	for (index = 0; index < listing->count; index++) {
		free(listing->entries[index].name);
		free(listing->entries[index].path);
		free(listing->entries[index].detail);
	}

	/* The table itself. */
	free(listing->entries);
	memset(listing, 0, sizeof(*listing));
}

/*
 * Adds an item of a folder to a listing, with what lstat (and stat for a
 * link) says of it, and returns it; NULL when memory runs out.
 *
 * An item whose status cannot be read is still listed, as a file of no
 * size.
 */
struct fm_entry *
fm_dir_add(
	struct fm_listing *listing,
	const char *folder,
	const char *name)
{
	struct fm_entry *grown;
	struct fm_entry *entry;
	struct stat status;
	struct stat target;
	size_t capacity;
	int error;
	int link;

	/* Room for one more entry. */
	if (listing->count == listing->capacity) {
		capacity = listing->capacity + DIR_GROWTH;
		grown = realloc(listing->entries, capacity * sizeof(listing->entries[0]));
		if (grown == NULL)
			return NULL;
		listing->entries = grown;
		listing->capacity = capacity;
	}

	/* The new entry, empty, with its name. */
	entry = &listing->entries[listing->count];
	memset(entry, 0, sizeof(*entry));
	entry->child_count = -1;
	entry->name = strdup(name);
	if (entry->name == NULL)
		return NULL;

	/* Its full path. */
	entry->path = dir_join(folder, name);
	if (entry->path == NULL) {
		free(entry->name);
		return NULL;
	}

	/* What the item itself is (a link is not followed). */
	error = lstat(entry->path, &status);
	if (error == 0) {
		entry->size = (uint64_t)status.st_size;
		entry->mode = status.st_mode;
		entry->uid = status.st_uid;
		entry->gid = status.st_gid;
		entry->modified = status.st_mtime;
		entry->changed = status.st_ctime;
		entry->accessed = status.st_atime;
		entry->folder = S_ISDIR(status.st_mode);
	}

	/* A link shows as what it leads to, marked as a link. */
	link = 0;
	if (error == 0)
		link = S_ISLNK(status.st_mode);
	if (link != 0) {
		entry->link = 1;
		error = stat(entry->path, &target);
		if (error == 0) {
			entry->folder = S_ISDIR(target.st_mode);
			entry->size = (uint64_t)target.st_size;
			entry->mode = target.st_mode;
		}
	}

	/* The item's type from its name and mode. */
	entry->mime = fm_mime_guess(name, entry->mode);

	/* Succeeded: the entry is part of the listing. */
	listing->count++;
	return entry;
}

/*
 * Counts a folder's items (the hidden ones only when asked); -1 when the
 * folder cannot be read.
 */
int
fm_dir_count(
	const char *path,
	int hidden)
{
	struct dirent *item;
	DIR *folder;
	int count;
	int dot;

	/* The folder. */
	folder = opendir(path);
	if (folder == NULL)
		return -1;

	/* Every item but . and .. and, unless asked, the hidden ones. */
	count = 0;
	for (;;) {
		item = readdir(folder);
		if (item == NULL)
			break;

		/* The folder itself and its parent do not count. */
		dot = dir_is_dot(item->d_name);
		if (dot != 0)
			continue;

		/* A hidden item counts only when hidden ones are shown. */
		if (item->d_name[0] == '.' && hidden == 0)
			continue;
		count++;
	}

	/* The folder is closed. */
	closedir(folder);

	/* Reports the count. */
	return count;
}

/*
 * Writes a size the way the desktop shows it: "Zero bytes", "812 bytes",
 * "2.4 KB", "42.8 MB", "1.2 GB" (decimal units).
 */
void
fm_dir_size_text(
	uint64_t size,
	char *text,
	size_t length)
{
	static const char *const units[] = { "KB", "MB", "GB", "TB" };
	uint64_t scale;
	uint64_t tenths;
	int unit;

	/* No bytes at all. */
	if (size == 0U) {
		snprintf(text, length, "Zero bytes");
		return;
	}

	/* A single byte is named alone. */
	if (size == 1U) {
		snprintf(text, length, "1 byte");
		return;
	}

	/* Less than a kilobyte is counted in bytes. */
	if (size < DIR_KILO) {
		snprintf(text, length, "%llu bytes", (unsigned long long)size);
		return;
	}

	/* The largest unit the size reaches. */
	unit = 0;
	scale = DIR_KILO;
	while (unit < 3 && size >= scale * DIR_KILO) {
		scale *= DIR_KILO;
		unit++;
	}

	/* The size in that unit to a tenth. */
	tenths = (size * 10U + scale / 2U) / scale;
	snprintf(text, length, "%llu.%llu %s", (unsigned long long)(tenths / 10U), (unsigned long long)(tenths % 10U), units[unit]);
}

/*
 * Writes a count of items: "1 item", "12 items"; empty for a count that
 * is not known (below zero).
 */
void
fm_dir_items_text(
	long count,
	char *text,
	size_t length)
{
	char number[32];

	/* An unknown count says nothing. */
	if (count < 0) {
		text[0] = '\0';
		return;
	}

	/* The count in the language's form for it (one item is singular in English). */
	snprintf(number, sizeof(number), "%ld", count);
	(void)kl_tr_format(text, length, kl_trn("{1} item", "{1} items", (unsigned long)count), number, (const char *)NULL);
}

/*
 * Writes an owner as "user:group", by name where the accounts know them
 * and by number otherwise.
 */
void
fm_owner_text(
	uid_t uid,
	gid_t gid,
	char *text,
	size_t length)
{
	struct passwd *account;
	struct group *group;
	char user_name[64];
	char group_name[64];

	/* The user's name, or number. */
	snprintf(user_name, sizeof(user_name), "%lu", (unsigned long)uid);
	account = getpwuid(uid);
	if (account != NULL && account->pw_name != NULL)
		snprintf(user_name, sizeof(user_name), "%s", account->pw_name);

	/* The group's name, or number. */
	snprintf(group_name, sizeof(group_name), "%lu", (unsigned long)gid);
	group = getgrgid(gid);
	if (group != NULL && group->gr_name != NULL)
		snprintf(group_name, sizeof(group_name), "%s", group->gr_name);

	/* Both, joined. */
	snprintf(text, length, "%s:%s", user_name, group_name);
}

/* Orders two entries: folders first, then the sort's key, then the names. */
static int
dir_compare(
	const void *first,
	const void *second)
{
	const struct fm_entry *left;
	const struct fm_entry *right;
	int order;

	/* A folder comes before a file whatever the direction. */
	left = first;
	right = second;
	if (left->folder != right->folder) {
		if (left->folder != 0)
			return -1;
		return 1;
	}

	/* The key's order. */
	order = 0;
	switch (dir_sort_key) {
	case FM_SORT_KIND:
		order = strcmp(left->mime->kind, right->mime->kind);
		break;
	case FM_SORT_SIZE:
		if (left->size < right->size)
			order = -1;
		else if (left->size > right->size)
			order = 1;
		break;
	case FM_SORT_MODIFIED:
		if (left->modified < right->modified)
			order = 1;
		else if (left->modified > right->modified)
			order = -1;
		break;
	default:
		break;
	}

	/* Names break ties, and are the whole order of a name sort. */
	if (order == 0)
		order = dir_compare_names(left->name, right->name);

	/* A reversed sort turns the order round. */
	if (dir_sort_reverse != 0)
		order = -order;

	/* Reports the order. */
	return order;
}

/* Orders two names without regard to case, runs of digits as numbers. */
static int
dir_compare_names(
	const char *first,
	const char *second)
{
	const unsigned char *left;
	const unsigned char *right;
	size_t left_digits;
	size_t right_digits;
	unsigned left_character;
	unsigned right_character;

	/* Each position of the two names. */
	left = (const unsigned char *)first;
	right = (const unsigned char *)second;
	while (*left != '\0' && *right != '\0') {
		/* Two runs of digits compare as numbers: first by length without leading zeros, then digit by digit. */
		if (*left >= '0' &&
		    *left <= '9' &&
		    *right >= '0' &&
		    *right <= '9') {
			while (*left == '0' && left[1] >= '0' && left[1] <= '9')
				left++;
			while (*right == '0' && right[1] >= '0' && right[1] <= '9')
				right++;
			left_digits = 0;
			while (left[left_digits] >= '0' && left[left_digits] <= '9')
				left_digits++;
			right_digits = 0;
			while (right[right_digits] >= '0' && right[right_digits] <= '9')
				right_digits++;

			/* A longer number is larger. */
			if (left_digits != right_digits) {
				if (left_digits < right_digits)
					return -1;
				return 1;
			}

			/* Numbers of one length compare digit by digit. */
			while (left_digits > 0) {
				if (*left != *right) {
					if (*left < *right)
						return -1;
					return 1;
				}

				/* The same digit: on to the next. */
				left++;
				right++;
				left_digits--;
			}

			/* Equal numbers: the comparison goes on after them. */
			continue;
		}

		/* Other characters compare without regard to ASCII case. */
		left_character = *left;
		right_character = *right;
		if (left_character >= 'A' && left_character <= 'Z')
			left_character += 'a' - 'A';
		if (right_character >= 'A' && right_character <= 'Z')
			right_character += 'a' - 'A';
		if (left_character != right_character) {
			if (left_character < right_character)
				return -1;
			return 1;
		}

		/* The same character: on to the next. */
		left++;
		right++;
	}

	/* The shorter name comes first. */
	if (*left == '\0' && *right == '\0')
		return 0;
	if (*left == '\0')
		return -1;

	/* The first name is the longer one. */
	return 1;
}

/* Joins a folder and a name into an allocated path (the root has no second slash). */
static char *
dir_join(
	const char *folder,
	const char *name)
{
	size_t folder_length;
	size_t name_length;
	char *path;
	size_t at;

	/* The parts' lengths, the folder without a trailing slash. */
	folder_length = strlen(folder);
	while (folder_length > 0 && folder[folder_length - 1U] == '/')
		folder_length--;
	name_length = strlen(name);

	/* The path: folder, slash, name. */
	path = malloc(folder_length + name_length + 2U);
	if (path == NULL)
		return NULL;
	memcpy(path, folder, folder_length);
	at = folder_length;
	path[at] = '/';
	at++;
	memcpy(path + at, name, name_length + 1U);

	/* Reports the joined path. */
	return path;
}

/* Tells whether a name is the folder itself (.) or its parent (..). */
static int
dir_is_dot(
	const char *name)
{
	/* A single dot. */
	if (name[0] == '.' && name[1] == '\0')
		return 1;

	/* Two dots. */
	if (name[0] == '.' && name[1] == '.' && name[2] == '\0')
		return 1;

	/* Any other name. */
	return 0;
}

/*
 * Adds a folder's items to a listing (what it has stays), as fm_dir_read
 * does; returns 0, or the errno value of a folder that cannot be read
 * (also kept in the listing).
 */
static int
dir_read_into(
	struct fm_listing *listing,
	const char *path,
	int hidden)
{
	struct dirent *item;
	struct stat status;
	struct fm_entry *entry;
	DIR *folder;
	int error;
	int dot;

	/* The folder's own modification time, which tells later whether it changed. */
	error = stat(path, &status);
	if (error == 0)
		listing->modified = status.st_mtime;

	/* The folder. */
	folder = opendir(path);
	if (folder == NULL) {
		listing->error = errno;
		return listing->error;
	}

	/* Each item but . and .., and the hidden ones unless they are asked for. */
	for (;;) {
		errno = 0;
		item = readdir(folder);
		if (item == NULL)
			break;

		/* The folder itself and its parent are not items. */
		dot = dir_is_dot(item->d_name);
		if (dot != 0)
			continue;

		/* A hidden item, unless hidden ones are shown. */
		if (item->d_name[0] == '.' && hidden == 0)
			continue;

		/* The item, with what its status says. */
		entry = fm_dir_add(listing, path, item->d_name);
		if (entry == NULL) {
			listing->error = ENOMEM;
			break;
		}
	}

	/* The folder is closed whatever happened. */
	closedir(folder);

	/* Reports the error the listing ended with, if any. */
	if (listing->error != 0)
		return listing->error;

	/* Succeeded: the listing holds the folder's items. */
	return 0;
}

/*
 * Gives the items of one trash, from the entry first on, what their
 * records say: the name each had (BUG-140), the folder it was in and when
 * it was trashed.
 */
static void
dir_trash_details(
	struct fm_listing *listing,
	size_t first,
	const char *trash)
{
	struct fm_entry *entry;
	char original[FM_PATH_MAX];
	char *slash;
	char *shown;
	size_t index;
	time_t deleted;
	int error;

	/* Each item's record: the folder it was in and the time it was trashed. */
	for (index = first; index < listing->count; index++) {
		entry = &listing->entries[index];
		error = fm_trash_info_read(trash, entry->name, original, sizeof(original), &deleted);
		if (error != 0)
			continue;

		/*
		 * The item shows under the name it had (BUG-140): a second item
		 * of the same name is kept in the trash's files as "NAME.2", but
		 * it is still NAME, of NAME's kind.  The path stays the one in
		 * the trash, which is what Put Back and Delete Immediately use.
		 */
		slash = strrchr(original, '/');
		if (slash != NULL && slash[1] != '\0') {
			shown = strdup(slash + 1);
			if (shown != NULL) {
				free(entry->name);
				entry->name = shown;
				entry->mime = fm_mime_guess(entry->name, entry->mode);
			}
		}

		/* The folder is the original path without its last part. */
		if (slash != NULL && slash != original)
			*slash = '\0';
		entry->detail = strdup(original);
		entry->extra_time = deleted;
	}
}
