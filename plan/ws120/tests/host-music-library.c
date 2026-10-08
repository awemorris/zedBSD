/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of Music's tags and collection (ws120-p008): the files
 * make-m4a.py writes are read into the collection, and its order, names,
 * covers, search, next songs and the files added from outside are checked
 * (without the cache of the tags; ws177-p020 changed the depth, the .mp4
 * with pictures and the covers read when first drawn).
 *   host-music-library FOLDER     (FOLDER/Music and the files beside it)
 */

#include "music.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The checks failed. */
static int failures;

static void test_check(const char *name, int passed, const char *detail);
static int test_title(size_t index, const char *title);

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
	const struct mu_song *songs;
	struct mu_album *albums;
	struct mu_tags tags;
	size_t indices[16];
	size_t song_count;
	size_t album_count;
	char path[1024];
	long song;
	int error;

	/* The folder. */
	if (argc != 2) {
		fprintf(stderr, "usage: host-music-library FOLDER\n");
		return 2;
	}

	/* One file's tags. */
	(void)snprintf(path, sizeof(path), "%s/Music/Ann/Blue/02 second.m4a", argv[1]);
	error = mu_tags_read(path, &tags);
	test_check("tags-read", error == 0, path);
	test_check("tags-text", strcmp(tags.title, "Second Song") == 0 && strcmp(tags.artist, "Ann") == 0 &&
	    strcmp(tags.album, "Blue") == 0 && tags.album_artist[0] == '\0', tags.title);
	test_check("tags-track", tags.track == 2 && tags.duration_ms == 200500, "track 2, 200500 ms");
	test_check("tags-cover", tags.cover_size == 10U && tags.cover != NULL && memcmp(tags.cover, "\xff\xd8\xff\xe0JPEG-A", 10) == 0,
	    "the JPEG's bytes");
	test_check("tags-kind", tags.has_sound && !tags.has_video, "sound only");
	mu_tags_release(&tags);
	(void)snprintf(path, sizeof(path), "%s/Music/broken.m4a", argv[1]);
	error = mu_tags_read(path, &tags);
	test_check("tags-broken", error == EINVAL, "not an MP4");

	/* The folder's songs, in order (no cache). */
	mu_library_set_cache(NULL);
	(void)snprintf(path, sizeof(path), "%s/Music", argv[1]);
	error = mu_library_scan(path);
	test_check("scan", error == 0, path);
	songs = mu_songs(&song_count);
	albums = mu_albums(&album_count);
	test_check("scan-count", song_count == 8U && album_count == 4U, "8 songs in 4 albums");
	test_check("order", test_title(0, "First Song") && test_title(1, "Second Song") && test_title(2, "Voice Memo") &&
	    test_title(3, "Bob's Tune") && test_title(4, "Caf\xc3\xa9") && test_title(5, "A Clip") &&
	    test_title(6, "Too Deep") && test_title(7, "untagged"), "by album, number, title");
	if (song_count != 8U || album_count != 4U)
		return 1;

	/* The albums: titles, artists, covers. */
	test_check("albums", strcmp(albums[0].title, "Blue") == 0 && strcmp(albums[1].title, "Memos") == 0 &&
	    strcmp(albums[2].title, "Mix") == 0 && strcmp(albums[3].title, "Unknown Album") == 0, albums[0].title);
	test_check("album-artist", strcmp(albums[0].artist, "Ann") == 0 && strcmp(albums[2].artist, "Various") == 0 &&
	    strcmp(albums[3].artist, "Unknown Artist") == 0, albums[2].artist);
	test_check("album-cover-later", albums[0].cover == NULL && albums[0].cover_path != NULL && albums[2].cover_path == NULL,
	    "Blue's cover read later, Mix none");
	error = mu_library_cover_load(0);
	test_check("album-cover", error == 0 && albums[0].cover != NULL && albums[0].cover_size == 10U, "Blue's cover read");
	error = mu_library_cover_load(2);
	test_check("album-cover-none", error == 0 && albums[2].cover == NULL, "Mix has none");
	test_check("song-artist", strcmp(songs[3].artist, "Bob") == 0 && strcmp(songs[7].artist, "Unknown Artist") == 0,
	    songs[3].artist);
	test_check("song-length", songs[0].duration_ms == 61000 && songs[3].duration_ms == 3723000, "61 s, 1:02:03");
	test_check("song-album", songs[0].album == 0U && songs[1].album == 0U && songs[3].album == 2U && songs[4].album == 2U,
	    "the songs' albums");

	/* The lists: an album, a search. */
	test_check("list-album", mu_library_list(2, NULL, indices, 16) == 2U && indices[0] == 3U && indices[1] == 4U, "Mix");
	test_check("list-search", mu_library_list(-1, "ANN", indices, 16) == 2U && indices[0] == 0U && indices[1] == 1U,
	    "Ann");
	test_check("list-search-album", mu_library_list(-1, "mix", indices, 16) == 2U, "the album's title");
	test_check("list-none", mu_library_list(-1, "zzz", indices, 16) == 0U, "nothing");
	test_check("list-capacity", mu_library_list(-1, "", indices, 3) == 3U, "at most the capacity");

	/* The next songs. */
	test_check("next", mu_library_next(1, 1) == 2 && mu_library_next(7, 1) == -1 && mu_library_next(0, -1) == -1 &&
	    mu_library_next(3, -1) == 2, "through the albums, -1 past the ends");

	/* A file from outside, in its album's order; again, the same; a video, refused. */
	(void)snprintf(path, sizeof(path), "%s/outside.m4a", argv[1]);
	error = mu_library_add_file(path, &song);
	songs = mu_songs(&song_count);
	test_check("add", error == 0 && song == 2 && song_count == 9U && test_title(2, "Outside"), "third of Blue");
	error = mu_library_add_file(path, &song);
	mu_songs(&song_count);
	test_check("add-again", error == 0 && song == 2 && song_count == 9U, "found");
	(void)snprintf(path, sizeof(path), "%s/film.m4a", argv[1]);
	error = mu_library_add_file(path, &song);
	test_check("add-video", error == ENOTSUP, "no sound");

	/* The end. */
	mu_library_release();
	if (failures != 0) {
		printf("host-music-library: FAIL %d\n", failures);
		return 1;
	}
	printf("host-music-library: PASS\n");
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

/* Tells whether the song at an index has a title. */
static int
test_title(
	size_t index,
	const char *title)
{
	const struct mu_song *songs;
	size_t count;

	/* Within the songs. */
	songs = mu_songs(&count);
	if (index >= count)
		return 0;
	if (strcmp(songs[index].title, title) != 0) {
		printf("  song %zu is \"%s\"\n", index, songs[index].title);
		return 0;
	}
	return 1;
}
