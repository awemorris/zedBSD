/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The database (ws157-p004; photos.h, plan/ws157/phase001/phase.md D2):
 * text files under ~/Pictures/Library/db, split so that a cloud copies
 * little when something changes.
 *
 *   db/photos/YYYY-MM.tsv     the photos taken in a month, a line each:
 *                             id, path under the library, SHA-256, size,
 *                             taken, imported (YYYY-MM-DDTHH:MM:SS), width,
 *                             height, favourite (0 or 1), turns (0 to 3),
 *                             the name it was imported with; tab separated
 *   db/albums/<id>.album      an album: "name<TAB>NAME", then a line
 *                             "photo<TAB>ID" for each of its photos
 *
 * Each file starts with a heading line (# keiland-photos 1, or
 * # keiland-photos-album 1).  A save writes only the months with a photo
 * that changed and the albums that changed, each as a new file renamed
 * over the old one.  A line that cannot be read is passed over.
 */

#include "photos.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The headings, the fields of a photo's line, and the longest line read. */
#define DB_PHOTOS_HEADING	"# keiland-photos 1"
#define DB_ALBUM_HEADING	"# keiland-photos-album 1"
#define DB_FIELDS		11U
#define DB_LINE_MAX		(PH_PATH_MAX * 2U + 256U)

/* The most months a save writes at once. */
#define DB_MONTHS_MAX		1200U

static int db_load_photos(const char *root, const char *path);
static int db_load_album(const char *path, const char *id);
static int db_split(char *line, char **fields, size_t capacity);
static int db_number(const char *text, uint64_t *number);
static int db_write_month(const char *root, int month);
static int db_write_album(const char *root, const struct ph_album *album);
static int db_replace(const char *path, const char *temporary);
static int db_month(ph_time when);

/*
 * Writes the library's folder: ~/Pictures/Library.  Returns 0, or ENOENT
 * when $HOME is not set, ENAMETOOLONG.
 */
int
ph_library_root(
	char *root,
	size_t size)
{
	const char *home;
	int length;

	/* The home's. */
	home = getenv("HOME");
	if (home == NULL || home[0] != '/')
		return ENOENT;
	length = snprintf(root, size, "%s/%s", home, PH_LIBRARY);
	if (length < 0 || (size_t)length >= size)
		return ENAMETOOLONG;
	return 0;
}

/*
 * Reads the database of a library's folder into the library (a folder
 * without one has none) and puts it in order.  Returns 0, or ENOMEM.
 */
int
ph_db_load(
	const char *root)
{
	struct dirent *entry;
	char folder[PH_PATH_MAX];
	char path[PH_PATH_MAX];
	char id[PH_ID_SIZE];
	size_t length;
	int suffix;
	int error;
	DIR *opened;

	/* Each month's file of photos. */
	error = 0;
	(void)snprintf(folder, sizeof(folder), "%s/%s", root, PH_LIBRARY_PHOTOS);
	opened = opendir(folder);
	while (opened != NULL && error == 0) {
		entry = readdir(opened);
		if (entry == NULL)
			break;
		length = strlen(entry->d_name);
		if (entry->d_name[0] == '.' || length <= 4U)
			continue;
		suffix = strcmp(entry->d_name + length - 4U, ".tsv");
		if (suffix != 0)
			continue;
		length = (size_t)snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
		if (length >= sizeof(path))
			continue;
		error = db_load_photos(root, path);
	}

	/* Read through. */
	if (opened != NULL)
		(void)closedir(opened);

	/* Each album's file. */
	(void)snprintf(folder, sizeof(folder), "%s/%s", root, PH_LIBRARY_ALBUMS);
	opened = opendir(folder);
	while (opened != NULL && error == 0) {
		entry = readdir(opened);
		if (entry == NULL)
			break;
		length = strlen(entry->d_name);
		if (length != PH_ID_SIZE - 1U + 6U)
			continue;
		suffix = strcmp(entry->d_name + PH_ID_SIZE - 1U, ".album");
		if (suffix != 0)
			continue;
		(void)snprintf(id, sizeof(id), "%.*s", (int)(PH_ID_SIZE - 1U), entry->d_name);
		length = (size_t)snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
		if (length >= sizeof(path))
			continue;
		error = db_load_album(path, id);
	}

	/* Read through. */
	if (opened != NULL)
		(void)closedir(opened);

	/* In order. */
	ph_library_sort();
	return error;
}

/*
 * Writes the months with a photo that changed and the albums that
 * changed.  Returns 0 or an errno value (what was written stays written).
 */
int
ph_db_save(
	const char *root)
{
	static int months[DB_MONTHS_MAX];
	struct ph_photo *photos;
	struct ph_album *albums;
	size_t month_count;
	size_t count;
	size_t index;
	size_t seen;
	int month;
	int known;
	int error;
	int first;

	/* The months that changed. */
	photos = ph_photos(&count);
	month_count = 0;
	for (index = 0; index < count; index++) {
		if (!photos[index].changed)
			continue;
		month = db_month(photos[index].taken);
		known = 0;
		for (seen = 0; seen < month_count; seen++) {
			if (months[seen] == month)
				known = 1;
		}

		/* A month not seen yet. */
		if (!known && month_count < DB_MONTHS_MAX) {
			months[month_count] = month;
			month_count++;
		}
	}

	/* Each written, its photos not changed any more. */
	first = 0;
	for (seen = 0; seen < month_count; seen++) {
		error = db_write_month(root, months[seen]);
		if (error != 0 && first == 0)
			first = error;
		if (error != 0)
			continue;
		for (index = 0; index < count; index++) {
			month = db_month(photos[index].taken);
			if (month == months[seen])
				photos[index].changed = 0;
		}
	}

	/* Each album that changed. */
	albums = ph_albums(&count);
	for (index = 0; index < count; index++) {
		if (!albums[index].changed)
			continue;
		error = db_write_album(root, &albums[index]);
		if (error != 0 && first == 0)
			first = error;
		if (error == 0)
			albums[index].changed = 0;
	}

	/* The first failure, or 0. */
	return first;
}

/*
 * Makes the folders of a file's path that are not there.  Returns 0 or an
 * errno value.
 */
int
ph_db_folders(
	const char *path)
{
	char folder[PH_PATH_MAX];
	char *slash;
	int length;
	int status;

	/* Each folder from the top. */
	length = snprintf(folder, sizeof(folder), "%s", path);
	if (length < 0 || (size_t)length >= sizeof(folder))
		return ENAMETOOLONG;
	for (slash = strchr(folder + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
		*slash = '\0';
		status = mkdir(folder, 0755);
		if (status != 0 && errno != EEXIST)
			return errno;
		*slash = '/';
	}

	/* The folders are there. */
	return 0;
}

/*
 * Writes a time as YYYY-MM-DDTHH:MM:SS.
 */
void
ph_time_text(
	ph_time when,
	char *text,
	size_t size)
{
	long seconds;
	int year;
	int month;
	int day;

	/* The day and the time of day. */
	ph_time_split(when, &year, &month, &day);
	seconds = (long)(when % 86400);
	if (seconds < 0)
		seconds += 86400;
	(void)snprintf(text, size, "%04d-%02d-%02dT%02ld:%02ld:%02ld", year, month, day, seconds / 3600, seconds / 60 % 60, seconds % 60);
}

/*
 * Reads a time written as YYYY-MM-DDTHH:MM:SS.  Returns 0, or EINVAL.
 */
int
ph_time_parse(
	const char *text,
	ph_time *when)
{
	int fields[6];
	int read;
	char end;

	/* The six numbers and nothing after. */
	read = sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d%c", &fields[0], &fields[1], &fields[2], &fields[3], &fields[4], &fields[5], &end);
	if (read != 6 || fields[1] < 1 || fields[1] > 12 || fields[2] < 1 || fields[2] > 31)
		return EINVAL;
	*when = ph_time_make(fields[0], fields[1], fields[2], fields[3], fields[4], fields[5]);
	return 0;
}

/* Reads a month's file of photos into the library; 0, or ENOMEM (a line that cannot be read is passed over). */
static int
db_load_photos(
	const char *root,
	const char *path)
{
	static char line[DB_LINE_MAX];
	struct ph_photo photo;
	char *fields[DB_FIELDS];
	char *got;
	uint64_t numbers[5];
	size_t length;
	int count;
	int error;
	int bad;
	FILE *file;

	/* The file. */
	file = fopen(path, "r");
	if (file == NULL)
		return 0;

	/* Each line but the heading and comments. */
	error = 0;
	for (;;) {
		got = fgets(line, sizeof(line), file);
		if (got == NULL)
			break;
		length = strlen(line);
		if (length == 0U || line[length - 1U] != '\n' || line[0] == '#')
			continue;
		line[length - 1U] = '\0';

		/* The fields. */
		count = db_split(line, fields, DB_FIELDS);
		if (count != (int)DB_FIELDS)
			continue;
		memset(&photo, 0, sizeof(photo));
		bad = strlen(fields[0]) != PH_ID_SIZE - 1U || strlen(fields[2]) != PH_HASH_SIZE - 1U || fields[1][0] == '\0' || fields[1][0] == '/';
		bad = bad || strstr(fields[1], "..") != NULL;
		bad = bad || db_number(fields[3], &numbers[0]) != 0 || ph_time_parse(fields[4], &photo.taken) != 0;
		bad = bad || ph_time_parse(fields[5], &photo.imported) != 0 || db_number(fields[6], &numbers[1]) != 0;
		bad = bad || db_number(fields[7], &numbers[2]) != 0 || db_number(fields[8], &numbers[3]) != 0 || db_number(fields[9], &numbers[4]) != 0;
		if (bad || numbers[3] > 1U || numbers[4] > 3U)
			continue;

		/* The photo. */
		(void)snprintf(photo.id, sizeof(photo.id), "%s", fields[0]);
		(void)snprintf(photo.hash, sizeof(photo.hash), "%s", fields[2]);
		photo.size = numbers[0];
		photo.width = (int)numbers[1];
		photo.height = (int)numbers[2];
		photo.favorite = (int)numbers[3];
		photo.turns = (int)numbers[4];
		error = ph_library_add(&photo, root, fields[1], fields[10]);
		if (error != 0)
			break;
	}

	/* Read through. */
	(void)fclose(file);
	if (error == ENOSPC)
		error = 0;
	return error;
}

/* Reads an album's file into the library; 0, or ENOMEM (a file without its name is passed over). */
static int
db_load_album(
	const char *path,
	const char *id)
{
	static char line[DB_LINE_MAX];
	struct ph_album *albums;
	char *fields[2];
	char *got;
	size_t length;
	size_t album;
	size_t total;
	int count;
	int error;
	int same;
	int named;
	FILE *file;

	/* The file. */
	file = fopen(path, "r");
	if (file == NULL)
		return 0;

	/* Each line: the name first, then the photos. */
	error = 0;
	named = 0;
	album = 0;
	for (;;) {
		got = fgets(line, sizeof(line), file);
		if (got == NULL)
			break;
		length = strlen(line);
		if (length == 0U || line[length - 1U] != '\n' || line[0] == '#')
			continue;
		line[length - 1U] = '\0';
		count = db_split(line, fields, 2);
		if (count != 2)
			continue;

		/* The name makes the album. */
		same = strcmp(fields[0], "name");
		if (same == 0 && !named) {
			error = ph_album_new(id, fields[1], &album);
			if (error != 0)
				break;
			named = 1;
			continue;
		}

		/* A photo of it. */
		same = strcmp(fields[0], "photo");
		length = strlen(fields[1]);
		if (same != 0 || !named || length != PH_ID_SIZE - 1U)
			continue;
		error = ph_album_add(album, fields[1]);
		if (error != 0)
			break;
	}

	/* Read through; as read, not changed. */
	(void)fclose(file);
	albums = ph_albums(&total);
	if (named)
		albums[album].changed = 0;
	return error;
}

/* Cuts a line at its tabs into at most capacity fields; returns how many there are (capacity + 1 for more). */
static int
db_split(
	char *line,
	char **fields,
	size_t capacity)
{
	char *tab;
	size_t count;

	/* Each field. */
	count = 0;
	for (;;) {
		if (count == capacity)
			return (int)capacity + 1;
		fields[count] = line;
		count++;
		tab = strchr(line, '\t');
		if (tab == NULL)
			break;
		*tab = '\0';
		line = tab + 1;
	}

	/* The fields. */
	return (int)count;
}

/* Reads a decimal number; 0, or EINVAL. */
static int
db_number(
	const char *text,
	uint64_t *number)
{
	char *end;

	/* Digits only. */
	if (text[0] < '0' || text[0] > '9')
		return EINVAL;
	*number = (uint64_t)strtoull(text, &end, 10);
	if (*end != '\0')
		return EINVAL;
	return 0;
}

/* Writes a month's file: every photo taken in it (none: an empty file with its heading). */
static int
db_write_month(
	const char *root,
	int month)
{
	struct ph_photo *photos;
	char path[PH_PATH_MAX];
	char temporary[PH_PATH_MAX + 32U];
	char taken[32];
	char imported[32];
	size_t count;
	size_t index;
	int taken_month;
	int error;
	int status;
	int closed;
	FILE *file;

	/* The file's name, its folders, and the new file beside it. */
	(void)snprintf(path, sizeof(path), "%s/%s/%04d-%02d.tsv", root, PH_LIBRARY_PHOTOS, month / 12, month % 12 + 1);
	error = ph_db_folders(path);
	if (error != 0)
		return error;
	(void)snprintf(temporary, sizeof(temporary), "%s.new-%ld", path, (long)getpid());
	file = fopen(temporary, "w");
	if (file == NULL)
		return errno;

	/* The heading, then each photo of the month. */
	status = fprintf(file, "%s\n", DB_PHOTOS_HEADING);
	photos = ph_photos(&count);
	for (index = 0; index < count && status >= 0; index++) {
		taken_month = db_month(photos[index].taken);
		if (taken_month != month)
			continue;
		ph_time_text(photos[index].taken, taken, sizeof(taken));
		ph_time_text(photos[index].imported, imported, sizeof(imported));
		status = fprintf(file, "%s\t%s\t%s\t%llu\t%s\t%s\t%d\t%d\t%d\t%d\t%s\n", photos[index].id, photos[index].relative, photos[index].hash,
		    (unsigned long long)photos[index].size, taken, imported, photos[index].width, photos[index].height, photos[index].favorite,
		    photos[index].turns, photos[index].original);
	}

	/* Closed and renamed over the old one. */
	closed = fclose(file);
	if (closed != 0 || status < 0) {
		(void)unlink(temporary);
		return EIO;
	}

	/* In place. */
	return db_replace(path, temporary);
}

/* Writes an album's file. */
static int
db_write_album(
	const char *root,
	const struct ph_album *album)
{
	char path[PH_PATH_MAX];
	char temporary[PH_PATH_MAX + 32U];
	size_t index;
	int error;
	int status;
	int closed;
	FILE *file;

	/* The file's name, its folders, and the new file beside it. */
	(void)snprintf(path, sizeof(path), "%s/%s/%s.album", root, PH_LIBRARY_ALBUMS, album->id);
	error = ph_db_folders(path);
	if (error != 0)
		return error;
	(void)snprintf(temporary, sizeof(temporary), "%s.new-%ld", path, (long)getpid());
	file = fopen(temporary, "w");
	if (file == NULL)
		return errno;

	/* The heading, the name, the photos. */
	status = fprintf(file, "%s\nname\t%s\n", DB_ALBUM_HEADING, album->name);
	for (index = 0; index < album->count && status >= 0; index++)
		status = fprintf(file, "photo\t%s\n", album->members[index]);

	/* Closed and renamed over the old one. */
	closed = fclose(file);
	if (closed != 0 || status < 0) {
		(void)unlink(temporary);
		return EIO;
	}

	/* In place. */
	return db_replace(path, temporary);
}

/* Renames a new file over the old one; 0, or an errno value (the new file goes). */
static int
db_replace(
	const char *path,
	const char *temporary)
{
	int status;
	int error;

	/* In its place. */
	status = rename(temporary, path);
	if (status == 0)
		return 0;

	/* It could not be. */
	error = errno;
	(void)unlink(temporary);
	return error;
}

/* The month of a time as one number (the year's months counted). */
static int
db_month(
	ph_time when)
{
	int year;
	int month;
	int day;

	/* Split. */
	ph_time_split(when, &year, &month, &day);
	return year * 12 + month - 1;
}
