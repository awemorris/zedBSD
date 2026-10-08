/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of Music's collection in its quasi-normal cases
 * (ws177-p020): names folded (case and spaces, Latin-1, Greek, Cyrillic,
 * full-width), Various Artists, an album artist named by a later song,
 * disc folders, the same album's names with the same numbers in two
 * folders, ten folders down and a link back up, an .mp4 with pictures, the
 * covers read when first drawn and cropped to their middle square, the
 * cache of the tags, and the folder looked through again when it changed.
 *   host-music-m FOLDER     (the files host-music-m.py wrote)
 */

#include "music.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The checks failed. */
static int failures;

static void test_check(const char *name, int passed, const char *detail);
static long test_album(const char *title);
static int test_retitle(const char *path, const char *from, const char *to, long seconds);
static int test_touch(const char *path, long seconds);
static int test_copy(const char *from, const char *to);

/* The log the program would write (not used by the collection). */
void
mu_log(
	const char *format,
	...)
{
	(void)format;
}

/* Reads the collection and checks it. */
int
main(
	int argc,
	char **argv)
{
	struct mu_album *albums;
	struct kl_image picture;
	size_t indices[64];
	size_t song_count;
	size_t album_count;
	char music[512];
	char cache[1024];
	char path[1024];
	char other[1024];
	char line[64];
	uint32_t pixel;
	FILE *file;
	long album;
	long song;
	int error;

	/* The folder. */
	if (argc != 2 || strlen(argv[1]) > 400U) {
		fprintf(stderr, "usage: host-music-m FOLDER   (a path of at most 400 bytes)\n");
		return 2;
	}
	(void)snprintf(music, sizeof(music), "%s/Music", argv[1]);
	(void)snprintf(cache, sizeof(cache), "%s/cache/music/tags", argv[1]);

	/* The collection, with a cache of its own. */
	mu_library_set_cache(cache);
	error = mu_library_scan(music);
	(void)mu_songs(&song_count);
	albums = mu_albums(&album_count);
	test_check("scan", error == 0 && song_count == 19U && album_count == 12U, "19 songs in 12 albums, the link back up passed over");
	if (song_count != 19U || album_count != 12U) {
		printf("  %lu songs, %lu albums\n", (unsigned long)song_count, (unsigned long)album_count);
		return 1;
	}

	/* Names folded: one album each. */
	test_check("fold-ascii", test_album("Fold One") == test_album("Fold Two"), "Blue  Sky and  blue sky ");
	test_check("fold-greek", test_album("Alpha") == test_album("Beta"), "Greek capitals and small letters");
	test_check("fold-cyrillic", test_album("Pervaya") == test_album("Vtoraya"), "Cyrillic");
	test_check("fold-wide", test_album("Wide One") == test_album("Wide Two"), "full-width Latin");
	test_check("search-greek", mu_library_list(-1, "\xce\xb1\xce\xbb\xcf\x86\xce\xb1", indices, 64) == 2U, "the Greek album searched in small letters");
	test_check("search-cyrillic", mu_library_list(-1, "\xd0\x94\xd0\x9e\xd0\x9c", indices, 64) == 2U, "the Cyrillic album searched in capitals");

	/* Several artists without an album artist: Various Artists; an album artist named later. */
	album = test_album("Party One");
	test_check("various", album >= 0 && album == test_album("Party Two") && strcmp(albums[album].artist, "Various Artists") == 0, "Party");
	album = test_album("Later One");
	test_check("artist-later", album >= 0 && album == test_album("Later Two") && strcmp(albums[album].artist, "Zed") == 0, "Later by Zed");

	/* Disc folders: one album; the same names and numbers in two folders: two. */
	test_check("discs", test_album("Act One") == test_album("Act Two"), "Opera on CD 1 and CD 2");
	test_check("conflict", test_album("Hit A") != test_album("Hit B"), "Greatest Hits twice");

	/* Ten folders down, and the .mp4 with pictures and sound. */
	(void)snprintf(path, sizeof(path), "%s/d1/d2/d3/d4/d5/d6/d7/d8/d9/d10/deep.m4a", music);
	test_check("deep", mu_library_find(path) >= 0, path);
	(void)snprintf(path, sizeof(path), "%s/film.mp4", music);
	test_check("video-sound", mu_library_find(path) >= 0, path);

	/* The cover: read when first drawn, its middle square (green) made into the picture. */
	album = test_album("Fold One");
	test_check("cover-later", album >= 0 && albums[album].cover == NULL && albums[album].cover_path != NULL, "not read at the scan");
	error = mu_library_cover_load((size_t)album);
	test_check("cover-load", error == 0 && albums[album].cover != NULL, "read from its song's file");
	memset(&picture, 0, sizeof(picture));
	error = -1;
	if (albums[album].cover != NULL)
		error = mu_cover_picture(albums[album].cover, albums[album].cover_size, 30, &picture);
	pixel = 0U;
	if (error == 0)
		pixel = picture.pixels[15U * picture.stride + 1U];
	test_check("cover-crop", error == 0 && ((pixel >> 8) & 0xffU) > 200U && ((pixel >> 16) & 0xffU) < 60U && (pixel & 0xffU) < 60U,
	    "the left edge is the middle's green, not the red of the left");
	kl_image_release(&picture);

	/* The cache written. */
	line[0] = '\0';
	file = fopen(cache, "r");
	if (file != NULL) {
		if (fgets(line, sizeof(line), file) == NULL)
			line[0] = '\0';
		fclose(file);
	}
	test_check("cache-written", strcmp(line, "music-tags 1\n") == 0, cache);

	/* Nothing changed yet. */
	test_check("unchanged", mu_library_changed() == 0, "right after the scan");

	/* A file from outside, kept over the rescans. */
	(void)snprintf(path, sizeof(path), "%s/outside.m4a", argv[1]);
	error = mu_library_add_file(path, &song);
	test_check("outside", error == 0 && song >= 0, path);

	/* The cache used: a file of the same size and time is not read again. */
	(void)snprintf(path, sizeof(path), "%s/cache/1.m4a", music);
	error = test_retitle(path, "Cache Song", "Fresh Song", 0);
	mu_library_release();
	mu_library_set_cache(cache);
	error |= mu_library_scan(music);
	test_check("cache-used", error == 0 && test_album("Cache Song") >= 0 && test_album("Fresh Song") < 0, "the title from the cache");

	/* A new folder with a song: changed, looked through again, the song in; the one from outside still there. */
	(void)snprintf(path, sizeof(path), "%s/outside.m4a", argv[1]);
	error = mu_library_add_file(path, &song);
	(void)snprintf(path, sizeof(path), "%s/new", music);
	error |= mkdir(path, 0755);
	(void)snprintf(path, sizeof(path), "%s/spare.m4a", argv[1]);
	(void)snprintf(other, sizeof(other), "%s/new/spare.m4a", music);
	error |= test_copy(path, other);
	error |= test_touch(music, 20);
	test_check("changed", error == 0 && mu_library_changed() == 1, "a folder added");
	error = mu_library_rescan();
	(void)mu_songs(&song_count);
	(void)snprintf(path, sizeof(path), "%s/outside.m4a", argv[1]);
	test_check("rescan", error == 0 && song_count == 21U && mu_library_find(other) >= 0 && mu_library_find(path) >= 0,
	    "the new song and the one from outside");
	test_check("rescan-quiet", mu_library_changed() == 0, "nothing changed since");

	/* A file changed in its time: read again after its folder changed. */
	(void)snprintf(path, sizeof(path), "%s/cache/1.m4a", music);
	error = test_touch(path, 30);
	(void)snprintf(path, sizeof(path), "%s/cache", music);
	error |= test_touch(path, 30);
	test_check("changed-file", error == 0 && mu_library_changed() == 1, "its folder's time");
	error = mu_library_rescan();
	test_check("cache-stale", error == 0 && test_album("Fresh Song") >= 0 && test_album("Cache Song") < 0, "the file read again");

	/* A song moved out of the folder: gone after the rescan (the folder's time moved on past the clock's tick). */
	(void)snprintf(path, sizeof(path), "%s/gone.m4a", argv[1]);
	error = rename(other, path);
	(void)snprintf(path, sizeof(path), "%s/new", music);
	error |= test_touch(path, 40);
	test_check("moved-out", error == 0 && mu_library_changed() == 1, "the folder's time changed");
	error = mu_library_rescan();
	(void)mu_songs(&song_count);
	test_check("gone", error == 0 && song_count == 20U && mu_library_find(other) < 0, "one song fewer");

	/* The end. */
	mu_library_release();
	if (failures != 0) {
		printf("host-music-m: FAIL %d\n", failures);
		return 1;
	}
	printf("host-music-m: PASS\n");
	return 0;
}

/* Reports one check. */
static void
test_check(
	const char *name,
	int passed,
	const char *detail)
{
	/* PASS or FAIL with what was expected. */
	if (passed) {
		printf("PASS %s\n", name);
		return;
	}
	printf("FAIL %s: %s\n", name, detail);
	failures++;
}

/* Gives the album of the song of a title (-1 for none). */
static long
test_album(
	const char *title)
{
	const struct mu_song *songs;
	size_t count;
	size_t index;
	int same;

	/* Each song. */
	songs = mu_songs(&count);
	for (index = 0; index < count; index++) {
		same = strcmp(songs[index].title, title);
		if (same == 0)
			return (long)songs[index].album;
	}
	return -1;
}

/* Changes a title of the same length in a file, and gives it back its time (moved by a number of seconds). */
static int
test_retitle(
	const char *path,
	const char *from,
	const char *to,
	long seconds)
{
	struct timespec times[2];
	struct stat status;
	unsigned char *bytes;
	size_t length;
	size_t index;
	FILE *file;
	int found;

	/* The file and its time. */
	found = stat(path, &status);
	if (found != 0 || strlen(from) != strlen(to))
		return -1;
	bytes = malloc((size_t)status.st_size);
	file = fopen(path, "r+b");
	if (bytes == NULL || file == NULL || fread(bytes, 1, (size_t)status.st_size, file) != (size_t)status.st_size) {
		free(bytes);
		if (file != NULL)
			fclose(file);
		return -1;
	}

	/* The title replaced where it is. */
	length = strlen(from);
	for (index = 0; index + length <= (size_t)status.st_size; index++) {
		if (memcmp(bytes + index, from, length) == 0)
			memcpy(bytes + index, to, length);
	}
	rewind(file);
	(void)fwrite(bytes, 1, (size_t)status.st_size, file);
	fclose(file);
	free(bytes);

	/* The time as it was, or moved. */
	times[0] = status.st_atim;
	times[1] = status.st_mtim;
	times[1].tv_sec += seconds;
	return utimensat(AT_FDCWD, path, times, 0);
}

/* Moves a file's or folder's time on by a number of seconds. */
static int
test_touch(
	const char *path,
	long seconds)
{
	struct timespec times[2];
	struct stat status;
	int found;

	/* Its time, moved. */
	found = stat(path, &status);
	if (found != 0)
		return -1;
	times[0] = status.st_atim;
	times[1] = status.st_mtim;
	times[1].tv_sec += seconds;
	return utimensat(AT_FDCWD, path, times, 0);
}

/* Copies a file. */
static int
test_copy(
	const char *from,
	const char *to)
{
	char buffer[4096];
	size_t count;
	FILE *in;
	FILE *out;

	/* Both files. */
	in = fopen(from, "rb");
	out = fopen(to, "wb");
	if (in == NULL || out == NULL) {
		if (in != NULL)
			fclose(in);
		if (out != NULL)
			fclose(out);
		return -1;
	}

	/* The bytes. */
	for (;;) {
		count = fread(buffer, 1, sizeof(buffer), in);
		if (count == 0U)
			break;
		(void)fwrite(buffer, 1, count, out);
	}
	fclose(in);
	return fclose(out) == 0 ? 0 : -1;
}
