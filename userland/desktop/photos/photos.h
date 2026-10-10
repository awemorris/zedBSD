/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Photos' shared in-memory media model. The mediastorage CLI owns JSON
 * metadata under ~/Pictures/Media/metadata.db and copied originals under
 * Files/YYYY/MM/dd. Desktop clients acquire snapshots via the compositor.
 * SHA-256 deduplication preserves stable identities and the original files.
 */

#ifndef PHOTOS_PHOTOS_H
#define PHOTOS_PHOTOS_H

#include <stdio.h>

#include <stddef.h>
#include <stdint.h>

/* The longest path kept, with its NUL. */
#define PH_PATH_MAX		1024U

/* The most photos the library keeps. */
#define PH_PHOTOS_MAX		50000U

/* The longest album name, with its NUL. */
#define PH_NAME_MAX		128U

/* A photo's or an album's id (32 hexadecimal digits) and a SHA-256 in hexadecimal, with their NULs. */
#define PH_ID_SIZE		33U
#define PH_HASH_SIZE		65U

/* The library's folder under the home, and its parts. */
#define PH_LIBRARY "Pictures/Media"
#define PH_LIBRARY_IMAGES "Files"
#define PH_LIBRARY_PHOTOS	"db/photos"
#define PH_LIBRARY_ALBUMS	"db/albums"

/*
 * A date and time as a count of seconds, the calendar's time read as if it
 * were UTC (the EXIF date has no zone; a file's time is turned into the
 * local calendar first), so that the year, month and day come back as
 * they were written.
 */
typedef int64_t ph_time;

/*
 * One photo: its id and content's hash, its file (the full path, and the
 * part under the library's folder), its name (the path's last part) and
 * the name it had when imported, its size in bytes, when it was taken and
 * imported, its picture's size (0 when not known), whether it is a
 * favourite, the quarter turns clockwise the user gave it (0 to 3), and
 * whether its line changed since its month's file was written.  The
 * strings are the library's.
 */
struct ph_photo {
	char id[PH_ID_SIZE];
	char hash[PH_HASH_SIZE];
	char *path;
	const char *relative;
	const char *name;
	char *original;
	uint64_t size;
	ph_time taken;
	ph_time imported;
	int width;
	int height;
	int favorite;
	int turns;
	int changed;
};

/*
 * One album: its id, its name, the ids of its photos (sorted, allocated:
 * count of them in room) and whether it changed since its file was
 * written.
 */
struct ph_album {
	char id[PH_ID_SIZE];
	char name[PH_NAME_MAX];
	char (*members)[PH_ID_SIZE];
	size_t count;
	size_t room;
	int changed;
};

/* What an import did: the photos imported, those in the library already, and those that failed. */
struct ph_import_result {
	unsigned imported;
	unsigned duplicates;
	unsigned failed;
};

/* What a file's first bytes say it is. */
#define PH_KIND_NONE		0
#define PH_KIND_JPEG		1
#define PH_KIND_PNG		2
#define PH_KIND_GIF		3
#define PH_KIND_VIDEO 4

/* The lists at the left: every photo by date, the favourites, an album. */
#define PH_LIST_TIMELINE	0
#define PH_LIST_FAVORITES	1
#define PH_LIST_ALBUM		2

/* The date of a JPEG's EXIF (exif.c). */
int ph_exif_date(const unsigned char *data, size_t size, ph_time *taken);
int ph_exif_descriptor_date(int descriptor, ph_time *taken);
int ph_exif_file_date(const char *path, ph_time *taken);
ph_time ph_time_make(int year, int month, int day, int hour, int minute, int second);
void ph_time_split(ph_time when, int *year, int *month, int *day);

/* The library in memory (library.c). */
int ph_library_add(const struct ph_photo *photo, const char *root, const char *relative, const char *original);
void ph_library_sort(void);
struct ph_photo *ph_photos(size_t *count);
struct ph_album *ph_albums(size_t *count);
long ph_library_find_hash(const char *hash);
long ph_library_find_id(const char *id);
size_t ph_library_list(int list, size_t album, size_t *indices, size_t capacity);
int ph_album_new(const char *id, const char *name, size_t *album);
int ph_album_create(const char *name, size_t *album);
int ph_album_add(size_t album, const char *id);
int ph_album_has(const struct ph_album *album, const char *id);
void ph_library_release(void);
int ph_picture_kind(const unsigned char *data, size_t size);

/* The database (db.c). */
int ph_library_root(char *root, size_t size);
int ph_db_load(const char *root);
int ph_db_save(const char *root);
int ph_db_folders(const char *path);
void ph_time_text(ph_time when, char *text, size_t size);
int ph_time_parse(const char *text, ph_time *when);

/* The import (import.c). */
int ph_import(const char *root, const char *source, struct ph_import_result *result);

/* The compositor transport snapshot, independent of persistent database files. */
int ph_snapshot_write(FILE *output, const char *root);
int ph_snapshot_read(FILE *input, char *root, size_t size);
int ph_snapshot_apply(FILE *input);
int ph_snapshot_changes(FILE *output);

/* The log for the tests (main.c, and the host tests' own). */
void ph_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif
