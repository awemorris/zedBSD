/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of Photos' library, database and import (ws157-p004): the
 * folder make-photos.py writes is imported into a library's folder, and
 * the places (Files/YYYY/MM/DD, the names kept, name-1 for another content
 * of a name), the duplicates, the database's files (a month a file, an
 * album a file, only what changed written), reading it back, the marks
 * and the albums are checked.  Run with TZ=UTC.
 *   host-photos-db SOURCE OTHER LIBRARY
 * (SOURCE the folder of photos, OTHER a folder with another beach.jpg,
 * LIBRARY a folder not there yet.)
 */

#include "photos.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* The checks failed. */
static int failures;

static void test_check(const char *name, int passed, const char *detail);
static long test_find(const char *relative);
static int test_lines(const char *path, const char *text);
static ino_t test_inode(const char *path);
static void test_touch(const char *path);

/* The log the program would write (not used here). */
void
ph_log(
	const char *format,
	...)
{
	(void)format;
}

/* Imports, saves, reads back and checks. */
int
main(
	int argc,
	char **argv)
{
	struct ph_import_result result;
	struct ph_photo *photos;
	struct ph_album *albums;
	struct stat status;
	size_t indices[32];
	size_t count;
	size_t album_count;
	size_t album;
	char path[1024];
	char text[64];
	const char *root;
	ph_time when;
	ino_t month_2021;
	ino_t month_2024;
	long beach;
	long drawing;
	long temple;
	int error;
	char drawing_path[128];
	char animation_path[128];
	char current_month[32];
	struct tm calendar;
	time_t now;

	/* The folders. */
	if (argc != 4) {
		fprintf(stderr, "usage: host-photos-db SOURCE OTHER LIBRARY\n");
		return 2;
	}

	/* The library. */
	root = argv[3];

	/* The times as the database writes them. */
	when = ph_time_make(2024, 8, 15, 10, 30, 5);
	ph_time_text(when, text, sizeof(text));
	test_check("time-text", strcmp(text, "2024-08-15T10:30:05") == 0, text);
	error = ph_time_parse(text, &when);
	test_check("time-parse", error == 0 && when == ph_time_make(2024, 8, 15, 10, 30, 5), "back");
	error = ph_time_parse("2024-13-01T00:00:00", &when);
	test_check("time-bad", error == EINVAL, "month 13");

	/* The new no-EXIF policy places files under the import date rather than source mtime. */
	now = time(NULL);
	gmtime_r(&now, &calendar);
	snprintf(drawing_path, sizeof(drawing_path), "Files/%04d/%02d/%02d/drawing.png", calendar.tm_year + 1900, calendar.tm_mon + 1, calendar.tm_mday);
	snprintf(animation_path, sizeof(animation_path), "Files/%04d/%02d/%02d/anim.gif", calendar.tm_year + 1900, calendar.tm_mon + 1, calendar.tm_mday);
	snprintf(current_month, sizeof(current_month), "%04d-%02d", calendar.tm_year + 1900, calendar.tm_mon + 1);

	/* The import: 8 pictures (not the text, the fake JPEG, the hidden ones, past four levels). */
	error = ph_import(root, argv[1], &result);
	photos = ph_photos(&count);
	test_check("import", error == 0 && result.imported == 8U && result.duplicates == 0U && result.failed == 0U && count == 8U, "8 imported");

	/* Their places: the day taken, the names kept. */
	beach = test_find("Files/2024/08/15/beach.jpg");
	temple = test_find("Files/2025/04/02/temple.jpg");
	drawing = test_find(drawing_path);
	test_check("place-exif", beach >= 0 && temple >= 0, "the EXIF's day");
	test_check("place-import-date", drawing >= 0 && test_find(animation_path) >= 0, "the import day");
	test_check("place-deep", test_find("Files/2019/05/05/four.jpg") >= 0, "four levels deep");
	(void)snprintf(path, sizeof(path), "%s/Files/2024/08/15/beach.jpg", root);
	error = stat(path, &status);
	test_check("copied", error == 0 && beach >= 0 && (uint64_t)status.st_size == photos[beach].size, "the copy's size");
	(void)snprintf(path, sizeof(path), "%s/beach.jpg", argv[1]);
	error = stat(path, &status);
	test_check("kept", error == 0, "the file given stays");
	test_check("id", beach >= 0 && strlen(photos[beach].id) == 32U && strncmp(photos[beach].id, photos[beach].hash, 32U) == 0, "the hash's start");
	test_check("order", photos[0].taken >= photos[count - 1U].taken, "the newest first");

	/* Again: every one is there already. */
	error = ph_import(root, argv[1], &result);
	(void)ph_photos(&count);
	test_check("duplicates", error == 0 && result.imported == 0U && result.duplicates == 8U && count == 8U, "not imported twice");

	/* Another content of a name taken on the same day: name-1. */
	error = ph_import(root, argv[2], &result);
	photos = ph_photos(&count);
	beach = test_find("Files/2024/08/15/beach-1.jpg");
	test_check("collision", error == 0 && result.imported == 1U && count == 9U && beach >= 0, "beach-1.jpg");
	test_check("original", beach >= 0 && strcmp(photos[beach].original, "beach.jpg") == 0, "the name it was imported with");

	/* Marks and an album, then the save. */
	drawing = test_find(drawing_path);
	temple = test_find("Files/2025/04/02/temple.jpg");
	photos[drawing].favorite = 1;
	photos[drawing].turns = 3;
	photos[drawing].changed = 1;
	error = ph_album_create("Trips", &album);
	test_check("album-create", error == 0, "made");
	error = ph_album_add(album, photos[temple].id);
	if (error == 0)
		error = ph_album_add(album, photos[beach].id);
	if (error == 0)
		error = ph_album_add(album, photos[beach].id);
	albums = ph_albums(&album_count);
	test_check("album-add", error == 0 && albums[album].count == 2U, "two photos, the same one once");
	error = ph_album_create("bad\tname", &album);
	test_check("album-bad-name", error == EINVAL, "a tab in the name");
	error = ph_db_save(root);
	test_check("save", error == 0, "written");

	/* The files: a month a file, an album a file. */
	(void)snprintf(path, sizeof(path), "%s/db/photos/2024-08.tsv", root);
	test_check("month-file", test_lines(path, "\tFiles/2024/08/15/beach.jpg\t") == 1 && test_lines(path, "\tFiles/2024/08/15/beach-1.jpg\t") == 1 &&
	    test_lines(path, "# keiland-photos 1") == 1, "2024-08.tsv");
	month_2024 = test_inode(path);
	(void)snprintf(path, sizeof(path), "%s/db/photos/%s.tsv", root, current_month);
	test_check("marks-line", test_lines(path, "\t1\t3\tdrawing.png") == 1, "favourite and turns");
	month_2021 = test_inode(path);
	albums = ph_albums(&album_count);
	(void)snprintf(path, sizeof(path), "%s/db/albums/%s.album", root, albums[0].id);
	test_check("album-file", test_lines(path, "name\tTrips") == 1 && test_lines(path, "photo\t") == 2, "the album's file");

	/* Read back into an empty library. */
	ph_library_release();
	error = ph_db_load(root);
	photos = ph_photos(&count);
	albums = ph_albums(&album_count);
	drawing = test_find(drawing_path);
	test_check("load", error == 0 && count == 9U && album_count == 1U, "9 photos and an album");
	test_check("load-marks", drawing >= 0 && photos[drawing].favorite == 1 && photos[drawing].turns == 3 && !photos[drawing].changed, "the marks");
	test_check("load-order", photos[0].taken >= photos[count - 1U].taken, "in order");
	count = ph_library_list(PH_LIST_ALBUM, 0, indices, 32);
	test_check("load-album", album_count == 1U && strcmp(albums[0].name, "Trips") == 0 && !albums[0].changed && count == 2U, "Trips' two photos");
	count = ph_library_list(PH_LIST_FAVORITES, 0, indices, 32);
	test_check("load-favorites", count == 1U && (long)indices[0] == drawing, "the favourite");

	/* A change in one month writes that month's file only. */
	photos[drawing].favorite = 0;
	photos[drawing].changed = 1;
	error = ph_db_save(root);
	(void)snprintf(path, sizeof(path), "%s/db/photos/%s.tsv", root, current_month);
	test_check("save-changed", error == 0 && test_inode(path) != month_2021 && test_lines(path, "\t0\t3\tdrawing.png") == 1, "import month written");
	(void)snprintf(path, sizeof(path), "%s/db/photos/2024-08.tsv", root);
	test_check("save-unchanged", test_inode(path) == month_2024, "2024-08 not written");

	/* A file put in Files by hand is not in the library (no line). */
	ph_library_release();
	(void)snprintf(path, sizeof(path), "%s/Files/2024/08/15/hand.jpg", root);
	test_touch(path);
	error = ph_db_load(root);
	(void)ph_photos(&count);
	test_check("by-hand", error == 0 && count == 9U && test_find("Files/2024/08/15/hand.jpg") < 0, "not listed");

	/* The result. */
	ph_library_release();
	if (failures != 0) {
		fprintf(stderr, "FAIL %d\n", failures);
		return 1;
	}

	/* Passed. */
	printf("PASS\n");
	return 0;
}

/* Writes a check's result. */
static void
test_check(
	const char *name,
	int passed,
	const char *detail)
{
	/* A line each. */
	if (passed) {
		printf("ok %s: %s\n", name, detail);
		return;
	}

	/* Failed. */
	printf("NOT OK %s: %s\n", name, detail);
	failures++;
}

/* Finds a photo by its path under the library: its index, or -1. */
static long
test_find(
	const char *relative)
{
	struct ph_photo *photos;
	size_t count;
	size_t index;
	int same;

	/* Each photo. */
	photos = ph_photos(&count);
	for (index = 0; index < count; index++) {
		same = strcmp(photos[index].relative, relative);
		if (same == 0)
			return (long)index;
	}

	/* None. */
	return -1;
}

/* Counts a file's lines that hold a text (-1 when it cannot be read). */
static int
test_lines(
	const char *path,
	const char *text)
{
	char line[4096];
	char *got;
	const char *found;
	int count;
	FILE *file;

	/* Each line. */
	file = fopen(path, "r");
	if (file == NULL)
		return -1;
	count = 0;
	for (;;) {
		got = fgets(line, sizeof(line), file);
		if (got == NULL)
			break;
		found = strstr(line, text);
		if (found != NULL)
			count++;
	}

	/* Read through. */
	(void)fclose(file);
	return count;
}

/* A file's inode (0 when it is not there): a file renamed over another has a new one. */
static ino_t
test_inode(
	const char *path)
{
	struct stat status;
	int error;

	/* Its status. */
	error = stat(path, &status);
	if (error != 0)
		return 0;
	return status.st_ino;
}

/* Makes an empty file and its folders. */
static void
test_touch(
	const char *path)
{
	FILE *file;
	int error;

	/* The folders, then the file. */
	error = ph_db_folders(path);
	if (error != 0)
		return;
	file = fopen(path, "wb");
	if (file != NULL)
		(void)fclose(file);
}
