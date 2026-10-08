/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The collection (ws120-p008, ws177-p020): the songs of a folder (~/Music)
 * and of the files opened, grouped by album.
 *
 * A folder is looked through sixteen levels deep (a folder met again on
 * the way down, through a link, is passed over) for .m4a and .mp4 files
 * with sound; each file's tags (tags.c) give its song.  The tags are kept
 * in a cache file by the file's size and time, so that a file not changed
 * is not read again; the covers are not kept there, but read from the file
 * of the album's first song with one when the album is first drawn.  The
 * folders looked through are remembered with their times, so that a
 * change in them is found (mu_library_changed) and the folder looked
 * through again (mu_library_rescan).
 *
 * An album is its title with its album artist (the song's artist when the
 * song names none), the names compared without regard to case or to the
 * spaces around and between their words.  Songs that name no album artist,
 * of one album title in one folder, are one album even when their artists
 * differ (its artist is then "Various Artists").  Two albums of one title
 * and artist in different folders are told apart when both have a song of
 * the same number; a folder named "CD n", "Disc n" or "Disk n" counts as
 * the folder above it.  A file opened from outside the folder joins the
 * album of its title and artist.  The albums are in the order of their
 * titles, the songs in the order of their albums, then of their numbers,
 * then of their titles, so that the next song of a song is the next one of
 * the list.
 */

#include "music.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* How deep a folder is looked through. */
#define LIBRARY_DEPTH		16

/* The longest path looked at, with its NUL. */
#define LIBRARY_PATH_MAX	1024U

/* What a song without them is called, and the artist of an album of several. */
#define LIBRARY_NO_ARTIST	"Unknown Artist"
#define LIBRARY_NO_ALBUM	"Unknown Album"
#define LIBRARY_VARIOUS		"Various Artists"

/* The cache file's first line, and its place under the cache folder. */
#define LIBRARY_CACHE_HEAD	"music-tags 1"
#define LIBRARY_CACHE_NAME	"music/tags"

/* The fields of a line of the cache file, and the longest line. */
#define LIBRARY_CACHE_FIELDS	12U
#define LIBRARY_LINE_MAX	(LIBRARY_PATH_MAX + 4U * MU_TEXT_MAX + 128U)

/*
 * What the library knows of an album besides what the view shows: the
 * keys it is found by (its title and artist folded, its folder), whether
 * its artist is an album artist the tags named, and its songs' numbers
 * with their folders (to tell two albums of one name apart).  It follows
 * its album's index; library_sort moves both together.
 */
struct library_album {
	char *title_key;
	char *artist_key;
	char *folder;
	int artist_given;
	int *tracks;
	char **track_folders;
	size_t track_count;
	size_t track_room;
};

/* A folder looked through, and its time then. */
struct library_folder {
	char *path;
	struct timespec time;
};

/*
 * A file's tags as the cache keeps them: its path, size and time (in
 * nanoseconds, library_time), the tags without the cover's bytes, and
 * whether the last look through the folder met the file (only those are
 * written back).
 */
struct library_cached {
	char *path;
	long long size;
	long long time;
	struct mu_tags tags;
	int seen;
};

/*
 * The collection: the songs and the albums (with the library's own of
 * each album), the folder looked through (and whether it existed), the
 * folders met in it with their times, the files added from outside it,
 * and the cache of the files' tags (sorted by path) with whether it was
 * read and changed since.
 */
struct library {
	struct mu_song *songs;
	size_t song_count;
	size_t song_room;
	struct mu_album *albums;
	struct library_album *keys;
	size_t album_count;
	size_t album_room;
	char *root;
	int root_seen;
	struct library_folder *folders;
	size_t folder_count;
	size_t folder_room;
	char **extras;
	size_t extra_count;
	size_t extra_room;
	struct library_cached *cache;
	size_t cache_count;
	size_t cache_room;
	int cache_loaded;
	int cache_dirty;
};

/* The collection of the program; one thread uses it. */
static struct library library;

/*
 * Where the cache file is: set by mu_library_set_cache (a test's own, or
 * empty for none), otherwise found from $XDG_CACHE_HOME or $HOME at the
 * first scan.
 */
static char library_cache_path[LIBRARY_PATH_MAX];
static int library_cache_chosen;

static int library_walk(const char *folder, int depth, const dev_t *devices, const ino_t *nodes);
static int library_file(const char *path, const struct stat *status);
static int library_watch(const char *path, const struct stat *status);
static int library_wanted(const char *name);
static int library_insert(const char *path, struct mu_tags *tags, int outside);
static int library_album(const char *title, const char *artist, int artist_given, const char *folder, int track, int outside, size_t *album);
static int library_album_artist(size_t album, const char *artist);
static int library_album_matches(size_t index, const char *title_key, const char *artist_key, int artist_given, const char *folder, int outside);
static int library_album_conflicts(const struct library_album *key, int track, const char *folder);
static int library_album_track(size_t album, int track, const char *folder);
static int library_sort(void);
static int library_compare_albums(const void *left, const void *right);
static int library_compare_songs(const void *left, const void *right);
static int library_compare_text(const char *left, const char *right);
static int library_contains(const char *text, const char *search);
static void library_fold(const char *text, char *folded, size_t size);
static uint32_t library_fold_letter(uint32_t letter);
static size_t library_utf8_read(const unsigned char *text, uint32_t *letter);
static size_t library_utf8_write(uint32_t letter, char *out, size_t room);
static char *library_folder_of(const char *path);
static int library_disc_folder(const char *name);
static char *library_copy(const char *text);
static const char *library_base_name(const char *path);
static int library_has_suffix(const char *name, const char *suffix);
static void library_release_collection(void);
static void library_cache_choose(void);
static void library_cache_load(void);
static void library_cache_save(void);
static void library_cache_folder(void);
static struct library_cached *library_cache_find(const char *path);
static int library_cache_put(const char *path, const struct stat *status, const struct mu_tags *tags);
static int library_cache_line(char *line, struct library_cached *entry);
static void library_clean_text(const char *text, char *clean, size_t size);
static long long library_time(const struct stat *status);

/*
 * Sets the cache file of the tags (NULL or empty for none), before the
 * first scan.
 */
void
mu_library_set_cache(
	const char *path)
{
	/* Chosen: the path, or none. */
	library_cache_chosen = 1;
	library_cache_path[0] = '\0';
	if (path != NULL)
		(void)snprintf(library_cache_path, sizeof(library_cache_path), "%s", path);
}

/*
 * Looks through a folder for songs and adds them to the collection.
 * Returns 0 (a folder that does not exist has none), or an errno value.
 */
int
mu_library_scan(
	const char *folder)
{
	struct stat status;
	dev_t devices[LIBRARY_DEPTH + 1];
	ino_t nodes[LIBRARY_DEPTH + 1];
	int directory;
	int found;
	int error;

	/* The cache, read once. */
	library_cache_choose();
	if (!library.cache_loaded) {
		library.cache_loaded = 1;
		library_cache_load();
	}

	/* The folder, remembered for a rescan. */
	if (library.root == NULL) {
		library.root = library_copy(folder);
		if (library.root == NULL)
			return ENOMEM;
	}

	/* The folder itself, which may not exist yet. */
	library.root_seen = 0;
	found = stat(folder, &status);
	if (found != 0)
		return 0;
	directory = S_ISDIR(status.st_mode);
	if (!directory)
		return 0;
	library.root_seen = 1;

	/* Its songs, from the top. */
	devices[0] = status.st_dev;
	nodes[0] = status.st_ino;
	error = library_watch(folder, &status);
	if (error == 0)
		error = library_walk(folder, 0, devices, nodes);
	if (error != 0)
		return error;

	/* The cache written back when it changed. */
	library_cache_save();

	/* In order. */
	error = library_sort();
	if (error != 0)
		return error;

	/* Succeeded: the folder's songs are in. */
	return 0;
}

/*
 * Tells whether the folder changed since it was looked through: a folder
 * met in it changed its time or went, or the folder came.  Returns 1 or 0.
 */
int
mu_library_changed(void)
{
	struct stat status;
	struct library_folder *folder;
	size_t index;
	int directory;
	int found;

	/* Never looked through. */
	if (library.root == NULL)
		return 0;

	/* A folder that did not exist and does now. */
	if (!library.root_seen) {
		found = stat(library.root, &status);
		if (found != 0)
			return 0;
		directory = S_ISDIR(status.st_mode);
		return directory;
	}

	/* Each folder met: gone, or of another time. */
	for (index = 0; index < library.folder_count; index++) {
		folder = &library.folders[index];
		found = stat(folder->path, &status);
		if (found != 0)
			return 1;
		if (status.st_mtim.tv_sec != folder->time.tv_sec || status.st_mtim.tv_nsec != folder->time.tv_nsec)
			return 1;
	}

	/* Succeeded: nothing changed. */
	return 0;
}

/*
 * Looks through the folder again (the cache spares the files not changed)
 * and adds the files from outside it again.  The albums' pictures are the
 * view's, forgotten before (mu_view_forget_pictures).  Returns 0 or an
 * errno value.
 */
int
mu_library_rescan(void)
{
	char **extras;
	size_t extra_count;
	size_t index;
	char *root;
	long song;
	int error;

	/* The folder and the files from outside are kept; the rest goes. */
	root = library.root;
	extras = library.extras;
	extra_count = library.extra_count;
	library.root = NULL;
	library.extras = NULL;
	library.extra_count = 0;
	library.extra_room = 0;
	library_release_collection();

	/* The folder again. */
	error = 0;
	if (root != NULL)
		error = mu_library_scan(root);
	free(root);

	/* The files from outside again (one gone is left out). */
	for (index = 0; index < extra_count; index++) {
		if (error == 0)
			(void)mu_library_add_file(extras[index], &song);
		free(extras[index]);
	}

	/* Their list. */
	free(extras);

	/* Reports the folder's outcome. */
	if (error != 0)
		return error;

	/* Succeeded: the collection is the folder's now. */
	return 0;
}

/*
 * Adds a file's song to the collection (it may be outside the folder),
 * or finds it there.  Returns 0 with its index, or an errno value: the
 * file's or its tags', ENOTSUP for one without sound.
 */
int
mu_library_add_file(
	const char *path,
	long *song)
{
	struct mu_tags tags;
	char **grown;
	size_t room;
	long found;
	int error;

	/* Already there. */
	found = mu_library_find(path);
	if (found >= 0) {
		*song = found;
		return 0;
	}

	/* Its tags, which need a track of sound. */
	error = mu_tags_read(path, &tags);
	if (error != 0)
		return error;
	if (!tags.has_sound) {
		mu_tags_release(&tags);
		return ENOTSUP;
	}

	/* Added, and the order made again. */
	error = library_insert(path, &tags, 1);
	mu_tags_release(&tags);
	if (error != 0)
		return error;
	error = library_sort();
	if (error != 0)
		return error;

	/* Remembered, to be added again after a rescan (without memory it is not). */
	if (library.extra_count == library.extra_room) {
		room = library.extra_room * 2U;
		if (room == 0U)
			room = 8U;
		grown = realloc(library.extras, room * sizeof(*grown));
		if (grown != NULL) {
			library.extras = grown;
			library.extra_room = room;
		}
	}

	/* The path, when there is room. */
	if (library.extra_count < library.extra_room) {
		library.extras[library.extra_count] = library_copy(path);
		if (library.extras[library.extra_count] != NULL)
			library.extra_count++;
	}

	/* Its place in the order. */
	found = mu_library_find(path);
	if (found < 0)
		return ENOENT;

	/* Succeeded: the song's index. */
	*song = found;
	return 0;
}

/*
 * Finds a song by its file; -1 when it is not in the collection.
 */
long
mu_library_find(
	const char *path)
{
	size_t index;
	int same;

	/* Each song. */
	for (index = 0; index < library.song_count; index++) {
		same = strcmp(library.songs[index].path, path);
		if (same == 0)
			return (long)index;
	}

	/* Not there. */
	return -1;
}

/*
 * Reads an album's cover from its file, when it has one and it is not read
 * yet.  Returns 0 (the album's cover holds the bytes, or the album has
 * none), ENOENT when the file has none any more, or an errno value of the
 * file's.
 */
int
mu_library_cover_load(
	size_t album)
{
	struct mu_tags tags;
	struct mu_album *found;
	int error;

	/* An album with a cover's file, its bytes not read yet. */
	if (album >= library.album_count)
		return EINVAL;
	found = &library.albums[album];
	if (found->cover != NULL || found->cover_path == NULL)
		return 0;

	/* The file's tags, its cover kept. */
	error = mu_tags_read(found->cover_path, &tags);
	if (error != 0)
		return error;
	found->cover = tags.cover;
	found->cover_size = tags.cover_size;
	tags.cover = NULL;
	mu_tags_release(&tags);

	/* None after all (the file changed since it was looked at). */
	if (found->cover == NULL)
		return ENOENT;

	/* Succeeded: the cover's bytes are the album's. */
	return 0;
}

/*
 * Reports the songs, in order.
 */
const struct mu_song *
mu_songs(
	size_t *count)
{
	/* The array. */
	*count = library.song_count;
	return library.songs;
}

/*
 * Reports the albums, in order.
 */
struct mu_album *
mu_albums(
	size_t *count)
{
	/* The array. */
	*count = library.album_count;
	return library.albums;
}

/*
 * Lists the songs shown: those of an album (-1 for every one) whose
 * title, artist or album has a search (any for an empty one or NULL), in
 * order.  Returns how many indices are stored, at most the capacity.
 */
size_t
mu_library_list(
	long album,
	const char *search,
	size_t *indices,
	size_t capacity)
{
	const struct mu_song *song;
	size_t count;
	size_t index;
	int matches;

	/* Each song, in order. */
	count = 0;
	for (index = 0; index < library.song_count && count < capacity; index++) {
		/* Of the album. */
		song = &library.songs[index];
		if (album >= 0 && song->album != (size_t)album)
			continue;

		/* With the search in its title, its artist or its album. */
		if (search != NULL && search[0] != '\0') {
			matches = library_contains(song->title, search);
			if (!matches)
				matches = library_contains(song->artist, search);
			if (!matches)
				matches = library_contains(library.albums[song->album].title, search);
			if (!matches)
				continue;
		}

		/* Shown. */
		indices[count] = index;
		count++;
	}

	/* The count. */
	return count;
}

/*
 * Finds the song after (a step of 1) or before (-1) a song in the order,
 * which goes on from an album's last song to the next album's first.
 * Returns -1 past either end.
 */
long
mu_library_next(
	long song,
	int step)
{
	long next;

	/* The neighbour. */
	next = song + step;
	if (song < 0 || next < 0 || next >= (long)library.song_count)
		return -1;
	return next;
}

/*
 * Frees the collection, the cache and what is remembered (the albums'
 * pictures are the view's, released before).
 */
void
mu_library_release(void)
{
	size_t index;

	/* The songs and the albums. */
	library_release_collection();

	/* The folder and the files from outside. */
	free(library.root);
	for (index = 0; index < library.extra_count; index++)
		free(library.extras[index]);
	free(library.extras);

	/* The cache. */
	for (index = 0; index < library.cache_count; index++)
		free(library.cache[index].path);
	free(library.cache);

	/* Empty. */
	memset(&library, 0, sizeof(library));
}

/*
 * Looks through a folder, and the folders in it to the depth, adding the
 * songs: devices and nodes hold the folders above (down to this one), so
 * that a folder met again through a link is passed over.  Returns 0 or an
 * errno value.
 */
static int
library_walk(
	const char *folder,
	int depth,
	const dev_t *devices,
	const ino_t *nodes)
{
	dev_t below_devices[LIBRARY_DEPTH + 1];
	ino_t below_nodes[LIBRARY_DEPTH + 1];
	struct dirent *entry;
	struct stat status;
	char path[LIBRARY_PATH_MAX];
	DIR *directory;
	int written;
	int found;
	int folder_entry;
	int above;
	int level;
	int error;

	/* The folder, which may have gone meanwhile. */
	directory = opendir(folder);
	if (directory == NULL) {
		if (errno == ENOENT || errno == ENOTDIR || errno == EACCES)
			return 0;
		return errno;
	}

	/* Each entry, but the hidden ones. */
	error = 0;
	for (;;) {
		entry = readdir(directory);
		if (entry == NULL)
			break;
		if (entry->d_name[0] == '.')
			continue;

		/* Its path. */
		written = snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
		if (written < 0 || (size_t)written >= sizeof(path))
			continue;
		found = stat(path, &status);
		if (found != 0)
			continue;

		/* A file: a song when its tags say so. */
		folder_entry = S_ISDIR(status.st_mode);
		if (!folder_entry) {
			error = library_file(path, &status);
			if (error != 0)
				break;
			continue;
		}

		/* A folder too deep is passed over. */
		if (depth + 1 >= LIBRARY_DEPTH)
			continue;

		/* So is one met on the way down (a link back up). */
		above = 0;
		for (level = 0; level <= depth; level++) {
			if (devices[level] == status.st_dev && nodes[level] == status.st_ino)
				above = 1;
		}

		/* Passed over then. */
		if (above)
			continue;

		/* Looked through, after it is remembered. */
		memcpy(below_devices, devices, (size_t)(depth + 1) * sizeof(*devices));
		memcpy(below_nodes, nodes, (size_t)(depth + 1) * sizeof(*nodes));
		below_devices[depth + 1] = status.st_dev;
		below_nodes[depth + 1] = status.st_ino;
		error = library_watch(path, &status);
		if (error == 0)
			error = library_walk(path, depth + 1, below_devices, below_nodes);
		if (error != 0)
			break;
	}

	/* The folder is closed. */
	(void)closedir(directory);

	/* Reports a failure. */
	if (error != 0)
		return error;

	/* Succeeded: the folder's songs are in. */
	return 0;
}

/* Adds a file of the folder when it is a song: its tags from the cache when it did not change. Returns 0 or ENOMEM. */
static int
library_file(
	const char *path,
	const struct stat *status)
{
	struct library_cached *cached;
	struct mu_tags tags;
	int wanted;
	int regular;
	int found;
	long long time;
	int error;

	/* A regular file of a song's name. */
	wanted = library_wanted(library_base_name(path));
	regular = S_ISREG(status->st_mode);
	if (!wanted || !regular)
		return 0;

	/* The cache's tags for a file of the same size and time; otherwise the file's, kept in the cache without the cover. */
	cached = library_cache_find(path);
	time = library_time(status);
	if (cached != NULL && cached->size == (long long)status->st_size && cached->time == time) {
		tags = cached->tags;
		cached->seen = 1;
	} else {
		found = mu_tags_read(path, &tags);
		if (found != 0)
			return 0;
		mu_tags_release(&tags);
		error = library_cache_put(path, status, &tags);
		if (error != 0)
			return error;
	}

	/* A song has sound (an .mp4 with pictures too is the song's sound alone). */
	if (!tags.has_sound)
		return 0;
	error = library_insert(path, &tags, 0);
	if (error != 0)
		return error;

	/* Succeeded: the song is in. */
	return 0;
}

/* Remembers a folder looked through with its time. Returns 0 or ENOMEM. */
static int
library_watch(
	const char *path,
	const struct stat *status)
{
	struct library_folder *grown;
	size_t room;

	/* Room for one more. */
	if (library.folder_count == library.folder_room) {
		room = library.folder_room * 2U;
		if (room == 0U)
			room = 32U;
		grown = realloc(library.folders, room * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		library.folders = grown;
		library.folder_room = room;
	}

	/* The folder and its time. */
	library.folders[library.folder_count].path = library_copy(path);
	if (library.folders[library.folder_count].path == NULL)
		return ENOMEM;
	library.folders[library.folder_count].time = status->st_mtim;
	library.folder_count++;

	/* Succeeded: remembered. */
	return 0;
}

/* Tells whether a file's name is one of a song: .m4a or .mp4. */
static int
library_wanted(
	const char *name)
{
	int song;

	/* By its suffix. */
	song = library_has_suffix(name, ".m4a");
	if (!song)
		song = library_has_suffix(name, ".mp4");
	return song;
}

/*
 * Adds a song from its tags (its file names the album's cover when the
 * album has none yet); outside says the file is not of the folder.
 * Returns 0 or ENOMEM.
 */
static int
library_insert(
	const char *path,
	struct mu_tags *tags,
	int outside)
{
	struct mu_song *grown;
	struct mu_song *song;
	struct mu_album *found;
	const char *artist;
	const char *album_artist;
	const char *album_title;
	const char *title;
	char name[MU_TEXT_MAX];
	char *folder;
	char *dot;
	size_t album;
	size_t room;
	int artist_given;
	int error;

	/* What a song without its tags is called: its file's name without the suffix. */
	title = tags->title;
	if (title[0] == '\0') {
		(void)snprintf(name, sizeof(name), "%s", library_base_name(path));
		dot = strrchr(name, '.');
		if (dot != NULL && dot != name)
			*dot = '\0';
		title = name;
	}

	/* An artist and an album for a song without them. */
	artist = tags->artist;
	if (artist[0] == '\0')
		artist = LIBRARY_NO_ARTIST;
	artist_given = 1;
	album_artist = tags->album_artist;
	if (album_artist[0] == '\0') {
		album_artist = artist;
		artist_given = 0;
	}

	/* The album's title, or the unknown album. */
	album_title = tags->album;
	if (album_title[0] == '\0')
		album_title = LIBRARY_NO_ALBUM;

	/* Its album, by its title, artist and folder, with its number noted there. */
	folder = library_folder_of(path);
	if (folder == NULL)
		return ENOMEM;
	error = library_album(album_title, album_artist, artist_given, folder, tags->track, outside, &album);
	if (error == 0)
		error = library_album_track(album, tags->track, folder);
	free(folder);
	if (error != 0)
		return error;

	/* Its file names the album's cover, when the album has none yet. */
	found = &library.albums[album];
	if (found->cover_path == NULL && (tags->has_cover || tags->cover != NULL)) {
		found->cover_path = library_copy(path);
		if (found->cover_path == NULL)
			return ENOMEM;
	}

	/* Room for the song. */
	if (library.song_count == library.song_room) {
		room = library.song_room * 2U;
		if (room == 0U)
			room = 64U;
		grown = realloc(library.songs, room * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		library.songs = grown;
		library.song_room = room;
	}

	/* The song. */
	song = &library.songs[library.song_count];
	memset(song, 0, sizeof(*song));
	song->path = library_copy(path);
	song->title = library_copy(title);
	song->artist = library_copy(artist);
	song->track = tags->track;
	song->duration_ms = tags->duration_ms;
	song->album = album;
	if (song->path == NULL || song->title == NULL || song->artist == NULL) {
		free(song->path);
		free(song->title);
		free(song->artist);
		return ENOMEM;
	}

	/* Succeeded: one more. */
	library.song_count++;
	return 0;
}

/*
 * Finds a song's album, or makes it: one of the title and the artist (its
 * album artist's, or, without one, any of the same folder, whose artist is
 * then "Various Artists" when the artists differ) whose songs have no song
 * of the same number from another folder.  A file from outside the folder
 * joins one of the title and artist from any folder.  Returns 0 with the
 * album's index, or ENOMEM.
 */
static int
library_album(
	const char *title,
	const char *artist,
	int artist_given,
	const char *folder,
	int track,
	int outside,
	size_t *album)
{
	struct library_album *key;
	struct mu_album *made;
	struct mu_album *grown_albums;
	struct library_album *grown_keys;
	char title_key[MU_TEXT_MAX];
	char artist_key[MU_TEXT_MAX];
	size_t index;
	size_t room;
	int matches;
	int conflict;
	int differs;
	int error;

	/* The keys: the names folded. */
	library_fold(title, title_key, sizeof(title_key));
	library_fold(artist, artist_key, sizeof(artist_key));

	/* One already made of the same names (or folder), without a song of the same number from another folder. */
	for (index = 0; index < library.album_count; index++) {
		matches = library_album_matches(index, title_key, artist_key, artist_given, folder, outside);
		if (!matches)
			continue;
		conflict = 0;
		if (!outside)
			conflict = library_album_conflicts(&library.keys[index], track, folder);
		if (conflict)
			continue;

		/* Several artists in one album without an album artist: Various Artists. */
		differs = strcmp(library.keys[index].artist_key, artist_key);
		if (differs != 0 && !artist_given) {
			error = library_album_artist(index, LIBRARY_VARIOUS);
			if (error != 0)
				return error;
		}

		/* An album artist named by a later song is the album's. */
		if (artist_given && !library.keys[index].artist_given) {
			library.keys[index].artist_given = 1;
			error = library_album_artist(index, artist);
			if (error != 0)
				return error;
		}

		/* Found. */
		*album = index;
		return 0;
	}

	/* Room for one more. */
	if (library.album_count == library.album_room) {
		room = library.album_room * 2U;
		if (room == 0U)
			room = 16U;
		grown_albums = realloc(library.albums, room * sizeof(*grown_albums));
		if (grown_albums == NULL)
			return ENOMEM;
		library.albums = grown_albums;
		grown_keys = realloc(library.keys, room * sizeof(*grown_keys));
		if (grown_keys == NULL)
			return ENOMEM;
		library.keys = grown_keys;
		library.album_room = room;
	}

	/* Made, without a cover yet. */
	made = &library.albums[library.album_count];
	key = &library.keys[library.album_count];
	memset(made, 0, sizeof(*made));
	memset(key, 0, sizeof(*key));
	made->title = library_copy(title);
	made->artist = library_copy(artist);
	key->title_key = library_copy(title_key);
	key->artist_key = library_copy(artist_key);
	key->folder = library_copy(folder);
	key->artist_given = artist_given;
	if (made->title == NULL ||
	    made->artist == NULL ||
	    key->title_key == NULL ||
	    key->artist_key == NULL ||
	    key->folder == NULL) {
		free(made->title);
		free(made->artist);
		free(key->title_key);
		free(key->artist_key);
		free(key->folder);
		return ENOMEM;
	}

	/* Succeeded: the new album. */
	*album = library.album_count;
	library.album_count++;
	return 0;
}

/* Names an album's artist (a copy); ENOMEM keeps the old name. */
static int
library_album_artist(
	size_t album,
	const char *artist)
{
	char *copy;

	/* The copy, then the old name freed. */
	copy = library_copy(artist);
	if (copy == NULL)
		return ENOMEM;
	free(library.albums[album].artist);
	library.albums[album].artist = copy;

	/* Succeeded. */
	return 0;
}

/*
 * Tells whether an album is a song's by its names: the same title, and the
 * same artist named the same way; or, without an album artist on either
 * side, the same folder (a file from outside the folder does not join by
 * the folder).
 */
static int
library_album_matches(
	size_t index,
	const char *title_key,
	const char *artist_key,
	int artist_given,
	const char *folder,
	int outside)
{
	const struct library_album *key;
	int differs;

	/* The title. */
	key = &library.keys[index];
	differs = strcmp(key->title_key, title_key);
	if (differs != 0)
		return 0;

	/* The artist, named the same way (as the album's artist or a song's). */
	differs = strcmp(key->artist_key, artist_key);
	if (differs == 0)
		return 1;

	/* Without an album artist on either side: the folder. */
	if (artist_given || key->artist_given || outside)
		return 0;
	differs = strcmp(key->folder, folder);
	if (differs != 0)
		return 0;

	/* Succeeded: an album of several artists in one folder. */
	return 1;
}

/* Tells whether an album has a song of a number (above 0) from another folder. */
static int
library_album_conflicts(
	const struct library_album *key,
	int track,
	const char *folder)
{
	size_t index;
	int differs;

	/* A song without a number never conflicts. */
	if (track <= 0)
		return 0;

	/* Each of the album's numbers. */
	for (index = 0; index < key->track_count; index++) {
		if (key->tracks[index] != track)
			continue;
		differs = strcmp(key->track_folders[index], folder);
		if (differs != 0)
			return 1;
	}

	/* Succeeded: no conflict. */
	return 0;
}

/* Notes a song's number and folder on its album. Returns 0 or ENOMEM. */
static int
library_album_track(
	size_t album,
	int track,
	const char *folder)
{
	struct library_album *key;
	char **grown_folders;
	int *grown_tracks;
	size_t room;

	/* A song without a number is not noted. */
	if (track <= 0)
		return 0;

	/* Room for one more. */
	key = &library.keys[album];
	if (key->track_count == key->track_room) {
		room = key->track_room * 2U;
		if (room == 0U)
			room = 16U;
		grown_tracks = realloc(key->tracks, room * sizeof(*grown_tracks));
		if (grown_tracks == NULL)
			return ENOMEM;
		key->tracks = grown_tracks;
		grown_folders = realloc(key->track_folders, room * sizeof(*grown_folders));
		if (grown_folders == NULL)
			return ENOMEM;
		key->track_folders = grown_folders;
		key->track_room = room;
	}

	/* The number and its folder. */
	key->track_folders[key->track_count] = library_copy(folder);
	if (key->track_folders[key->track_count] == NULL)
		return ENOMEM;
	key->tracks[key->track_count] = track;
	key->track_count++;

	/* Succeeded: noted. */
	return 0;
}

/*
 * Puts the albums and the songs in order: the albums by title, the songs
 * by album, number and title.  Returns 0 or ENOMEM.
 */
static int
library_sort(void)
{
	struct mu_album *sorted;
	struct library_album *sorted_keys;
	size_t *order;
	size_t *place;
	size_t index;

	/* Nothing to order. */
	if (library.album_count == 0U)
		return 0;

	/* The albums' order, as indices sorted by the albums they name. */
	order = malloc(library.album_count * sizeof(*order));
	place = malloc(library.album_count * sizeof(*place));
	sorted = malloc(library.album_count * sizeof(*sorted));
	sorted_keys = malloc(library.album_count * sizeof(*sorted_keys));
	if (order == NULL || place == NULL || sorted == NULL || sorted_keys == NULL) {
		free(order);
		free(place);
		free(sorted);
		free(sorted_keys);
		return ENOMEM;
	}

	/* Sorted. */
	for (index = 0; index < library.album_count; index++)
		order[index] = index;
	qsort(order, library.album_count, sizeof(*order), library_compare_albums);

	/* The albums and their keys moved into that order, and where each went. */
	for (index = 0; index < library.album_count; index++) {
		sorted[index] = library.albums[order[index]];
		sorted_keys[index] = library.keys[order[index]];
		place[order[index]] = index;
	}

	/* Copied back. */
	memcpy(library.albums, sorted, library.album_count * sizeof(*sorted));
	memcpy(library.keys, sorted_keys, library.album_count * sizeof(*sorted_keys));

	/* The songs follow their albums, then sort. */
	for (index = 0; index < library.song_count; index++)
		library.songs[index].album = place[library.songs[index].album];
	qsort(library.songs, library.song_count, sizeof(*library.songs), library_compare_songs);

	/* Succeeded: in order. */
	free(order);
	free(place);
	free(sorted);
	free(sorted_keys);
	return 0;
}

/* Compares two albums by their indices: by title, then by artist, then by folder. */
static int
library_compare_albums(
	const void *left,
	const void *right)
{
	const struct library_album *first;
	const struct library_album *second;
	int order;

	/* The keys of the albums the indices name. */
	first = &library.keys[*(const size_t *)left];
	second = &library.keys[*(const size_t *)right];

	/* The titles, then the artists, then the folders. */
	order = strcmp(first->title_key, second->title_key);
	if (order == 0)
		order = strcmp(first->artist_key, second->artist_key);
	if (order == 0)
		order = strcmp(first->folder, second->folder);
	return order;
}

/* Compares two songs: by album, then by number (none last), then by title, then by path. */
static int
library_compare_songs(
	const void *left,
	const void *right)
{
	const struct mu_song *first;
	const struct mu_song *second;
	long first_track;
	long second_track;
	int order;

	/* The albums' order. */
	first = left;
	second = right;
	if (first->album < second->album)
		return -1;
	if (first->album > second->album)
		return 1;

	/* The numbers, a song without one after those with. */
	first_track = first->track;
	if (first_track <= 0)
		first_track = 0x7fffffffL;
	second_track = second->track;
	if (second_track <= 0)
		second_track = 0x7fffffffL;
	if (first_track < second_track)
		return -1;
	if (first_track > second_track)
		return 1;

	/* The titles, then the paths. */
	order = library_compare_text(first->title, second->title);
	if (order == 0)
		order = strcmp(first->path, second->path);
	return order;
}

/* Compares two texts without regard to case or to the spaces around and between their words. */
static int
library_compare_text(
	const char *left,
	const char *right)
{
	char first[MU_TEXT_MAX];
	char second[MU_TEXT_MAX];
	int order;

	/* Folded, then byte by byte. */
	library_fold(left, first, sizeof(first));
	library_fold(right, second, sizeof(second));
	order = strcmp(first, second);
	return order;
}

/* Tells whether a text has a search in it, without regard to case or spaces. */
static int
library_contains(
	const char *text,
	const char *search)
{
	char folded_text[MU_TEXT_MAX];
	char folded_search[MU_TEXT_MAX];
	const char *found;

	/* Both folded. */
	library_fold(text, folded_text, sizeof(folded_text));
	library_fold(search, folded_search, sizeof(folded_search));

	/* The search in the text. */
	found = strstr(folded_text, folded_search);
	if (found == NULL)
		return 0;

	/* Succeeded: in it. */
	return 1;
}

/*
 * Folds a text for comparing: each letter of the scripts it knows as its
 * small letter (ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic, the
 * full-width Latin letters), the spaces at its ends left out and the
 * spaces between its words made one.  A byte that is not UTF-8 is kept.
 */
static void
library_fold(
	const char *text,
	char *folded,
	size_t size)
{
	const unsigned char *at;
	uint32_t letter;
	size_t length;
	size_t used;
	size_t written;
	int space;

	/* Each letter, a run of spaces as one (none at the start or the end). */
	at = (const unsigned char *)text;
	used = 0;
	space = 0;
	while (*at != '\0' && used + 5U < size) {
		length = library_utf8_read(at, &letter);
		at += length;

		/* A space waits for the next letter. */
		if (letter == ' ' || letter == '\t' || letter == '\n' || letter == 0x3000U) {
			if (used > 0U)
				space = 1;
			continue;
		}

		/* One space before it, then the letter folded. */
		if (space) {
			folded[used] = ' ';
			used++;
			space = 0;
		}

		/* The letter folded. */
		letter = library_fold_letter(letter);
		written = library_utf8_write(letter, folded + used, size - used - 1U);
		used += written;
	}

	/* Ended. */
	folded[used] = '\0';
}

/* Gives a letter's small letter, for the scripts the collection folds. */
static uint32_t
library_fold_letter(
	uint32_t letter)
{
	/* ASCII and Latin-1's capitals (not the multiplication sign). */
	if (letter >= 'A' && letter <= 'Z')
		return letter + 32U;
	if (letter >= 0xc0U && letter <= 0xdeU && letter != 0xd7U)
		return letter + 32U;

	/* Latin Extended-A: Y with diaeresis, and the capitals paired with the small letters after them. */
	if (letter == 0x178U)
		return 0xffU;
	if ((letter >= 0x100U && letter <= 0x137U) || (letter >= 0x14aU && letter <= 0x177U)) {
		if ((letter & 1U) == 0U)
			return letter + 1U;
		return letter;
	}

	/* The same, with the capitals at the odd letters. */
	if ((letter >= 0x139U && letter <= 0x148U) || (letter >= 0x179U && letter <= 0x17eU)) {
		if ((letter & 1U) != 0U)
			return letter + 1U;
		return letter;
	}

	/* Greek and Cyrillic capitals. */
	if (letter >= 0x391U && letter <= 0x3a9U && letter != 0x3a2U)
		return letter + 32U;
	if (letter >= 0x410U && letter <= 0x42fU)
		return letter + 32U;
	if (letter >= 0x400U && letter <= 0x40fU)
		return letter + 80U;

	/* The full-width Latin capitals. */
	if (letter >= 0xff21U && letter <= 0xff3aU)
		return letter + 32U;

	/* Succeeded: not a capital the collection folds. */
	return letter;
}

/* Reads one letter of UTF-8 (a byte that does not start one is that byte); gives the bytes read. */
static size_t
library_utf8_read(
	const unsigned char *text,
	uint32_t *letter)
{
	size_t length;
	size_t index;
	uint32_t value;

	/* The length the first byte says. */
	value = text[0];
	length = 1U;
	if (value >= 0xc2U && value <= 0xdfU) {
		length = 2U;
		value &= 0x1fU;
	} else if (value >= 0xe0U && value <= 0xefU) {
		length = 3U;
		value &= 0x0fU;
	} else if (value >= 0xf0U && value <= 0xf4U) {
		length = 4U;
		value &= 0x07U;
	}

	/* The bytes after it; a broken sequence is its first byte alone. */
	for (index = 1U; index < length; index++) {
		if ((text[index] & 0xc0U) != 0x80U) {
			*letter = text[0];
			return 1U;
		}

		/* Six more bits. */
		value = (value << 6) | (text[index] & 0x3fU);
	}

	/* Succeeded: the letter. */
	*letter = value;
	return length;
}

/* Writes a letter as UTF-8 (a byte that was not UTF-8, below 0x100, as itself); gives the bytes written (0 without room). */
static size_t
library_utf8_write(
	uint32_t letter,
	char *out,
	size_t room)
{
	/* One byte. */
	if (letter < 0x80U) {
		if (room < 1U)
			return 0U;
		out[0] = (char)letter;
		return 1U;
	}

	/* Two bytes. */
	if (letter < 0x800U) {
		if (room < 2U)
			return 0U;
		out[0] = (char)(0xc0U | (letter >> 6));
		out[1] = (char)(0x80U | (letter & 0x3fU));
		return 2U;
	}

	/* Three bytes. */
	if (letter < 0x10000U) {
		if (room < 3U)
			return 0U;
		out[0] = (char)(0xe0U | (letter >> 12));
		out[1] = (char)(0x80U | ((letter >> 6) & 0x3fU));
		out[2] = (char)(0x80U | (letter & 0x3fU));
		return 3U;
	}

	/* Four bytes. */
	if (room < 4U)
		return 0U;
	out[0] = (char)(0xf0U | (letter >> 18));
	out[1] = (char)(0x80U | ((letter >> 12) & 0x3fU));
	out[2] = (char)(0x80U | ((letter >> 6) & 0x3fU));
	out[3] = (char)(0x80U | (letter & 0x3fU));
	return 4U;
}

/*
 * Gives the folder an album's song counts as in (allocated; NULL without
 * memory): the file's folder, or the folder above it for a folder named
 * "CD n", "Disc n" or "Disk n".
 */
static char *
library_folder_of(
	const char *path)
{
	char *folder;
	char *slash;
	int disc;

	/* The file's folder ("." for a bare name). */
	folder = library_copy(path);
	if (folder == NULL)
		return NULL;
	slash = strrchr(folder, '/');
	if (slash == NULL) {
		free(folder);
		folder = library_copy(".");
		return folder;
	}

	/* The path cut at its last slash. */
	*slash = '\0';

	/* A disc's folder counts as the one above. */
	disc = library_disc_folder(library_base_name(folder));
	slash = strrchr(folder, '/');
	if (disc && slash != NULL && slash != folder)
		*slash = '\0';

	/* Succeeded: the folder. */
	return folder;
}

/* Tells whether a folder's name is a disc's: CD, Disc or Disk, maybe a space, then digits. */
static int
library_disc_folder(
	const char *name)
{
	char folded[MU_TEXT_MAX];
	size_t at;
	int differs;

	/* The name folded, then its word. */
	library_fold(name, folded, sizeof(folded));
	at = 0;
	differs = strncmp(folded, "cd", 2U);
	if (differs == 0)
		at = 2U;
	differs = strncmp(folded, "disc", 4U);
	if (differs == 0)
		at = 4U;
	differs = strncmp(folded, "disk", 4U);
	if (differs == 0)
		at = 4U;
	if (at == 0U)
		return 0;

	/* A space at most, then digits to the end. */
	if (folded[at] == ' ')
		at++;
	if (folded[at] < '0' || folded[at] > '9')
		return 0;
	while (folded[at] >= '0' && folded[at] <= '9')
		at++;
	if (folded[at] != '\0')
		return 0;

	/* Succeeded: a disc's folder. */
	return 1;
}

/* Copies a text into a new allocation (NULL when there is no memory). */
static char *
library_copy(
	const char *text)
{
	char *copy;
	size_t size;

	/* The bytes with the NUL. */
	size = strlen(text) + 1U;
	copy = malloc(size);
	if (copy == NULL)
		return NULL;
	memcpy(copy, text, size);
	return copy;
}

/* The last part of a path. */
static const char *
library_base_name(
	const char *path)
{
	const char *slash;

	/* After the last slash. */
	slash = strrchr(path, '/');
	if (slash == NULL)
		return path;
	return slash + 1;
}

/* Tells whether a name ends with a suffix, without regard to the case of ASCII letters. */
static int
library_has_suffix(
	const char *name,
	const char *suffix)
{
	size_t name_length;
	size_t suffix_length;
	int order;

	/* The end of the name against the suffix. */
	name_length = strlen(name);
	suffix_length = strlen(suffix);
	if (name_length < suffix_length)
		return 0;
	order = strcasecmp(name + name_length - suffix_length, suffix);
	return order == 0;
}

/* Frees the songs, the albums and the folders met (not the cache, the folder or the files from outside). */
static void
library_release_collection(void)
{
	struct library_album *key;
	size_t index;
	size_t track;

	/* The songs' strings, then their array. */
	for (index = 0; index < library.song_count; index++) {
		free(library.songs[index].path);
		free(library.songs[index].title);
		free(library.songs[index].artist);
	}

	/* Their array. */
	free(library.songs);
	library.songs = NULL;
	library.song_count = 0;
	library.song_room = 0;

	/* The albums' strings, covers and keys, then their arrays. */
	for (index = 0; index < library.album_count; index++) {
		free(library.albums[index].title);
		free(library.albums[index].artist);
		free(library.albums[index].cover_path);
		free(library.albums[index].cover);
		key = &library.keys[index];
		free(key->title_key);
		free(key->artist_key);
		free(key->folder);
		for (track = 0; track < key->track_count; track++)
			free(key->track_folders[track]);
		free(key->tracks);
		free(key->track_folders);
	}

	/* Their arrays. */
	free(library.albums);
	free(library.keys);
	library.albums = NULL;
	library.keys = NULL;
	library.album_count = 0;
	library.album_room = 0;

	/* The folders met. */
	for (index = 0; index < library.folder_count; index++)
		free(library.folders[index].path);
	free(library.folders);
	library.folders = NULL;
	library.folder_count = 0;
	library.folder_room = 0;
}

/* Finds the cache file's place, once: the one set, or the cache folder's. */
static void
library_cache_choose(void)
{
	const char *base;
	const char *home;

	/* Set already. */
	if (library_cache_chosen)
		return;
	library_cache_chosen = 1;

	/* $XDG_CACHE_HOME, else ~/.cache; none without a home. */
	library_cache_path[0] = '\0';
	base = getenv("XDG_CACHE_HOME");
	if (base != NULL && base[0] == '/') {
		(void)snprintf(library_cache_path, sizeof(library_cache_path), "%s/%s", base, LIBRARY_CACHE_NAME);
		return;
	}

	/* The home's. */
	home = getenv("HOME");
	if (home != NULL && home[0] == '/')
		(void)snprintf(library_cache_path, sizeof(library_cache_path), "%s/.cache/%s", home, LIBRARY_CACHE_NAME);
}

/* Reads the cache file into the cache (one that is missing or of another version is empty). */
static void
library_cache_load(void)
{
	struct library_cached entry;
	struct library_cached *grown;
	char line[LIBRARY_LINE_MAX];
	char *read_line;
	FILE *file;
	size_t room;
	int other;
	int status;

	/* The file, of this version. */
	if (library_cache_path[0] == '\0')
		return;
	file = fopen(library_cache_path, "r");
	if (file == NULL)
		return;
	read_line = fgets(line, sizeof(line), file);
	other = 1;
	if (read_line != NULL)
		other = strncmp(line, LIBRARY_CACHE_HEAD, strlen(LIBRARY_CACHE_HEAD));
	if (other != 0) {
		(void)fclose(file);
		return;
	}

	/* Each line an entry, kept in the file's order (it was written sorted). */
	for (;;) {
		read_line = fgets(line, sizeof(line), file);
		if (read_line == NULL)
			break;
		status = library_cache_line(line, &entry);
		if (status != 0)
			continue;

		/* Room for it. */
		if (library.cache_count == library.cache_room) {
			room = library.cache_room * 2U;
			if (room == 0U)
				room = 256U;
			grown = realloc(library.cache, room * sizeof(*grown));
			if (grown == NULL) {
				free(entry.path);
				break;
			}

			/* The larger array. */
			library.cache = grown;
			library.cache_room = room;
		}

		/* Kept. */
		library.cache[library.cache_count] = entry;
		library.cache_count++;
	}

	/* The file is closed. */
	(void)fclose(file);
}

/* Writes the files met in the last look through the folder to the cache file, when the cache changed. */
static void
library_cache_save(void)
{
	struct library_cached *entry;
	char temporary[LIBRARY_PATH_MAX + 8U];
	char clean[4][MU_TEXT_MAX];
	FILE *file;
	size_t index;
	int dropped;
	int status;

	/* Some entry new, or one not met (a file gone). */
	dropped = 0;
	for (index = 0; index < library.cache_count; index++) {
		if (!library.cache[index].seen)
			dropped = 1;
	}

	/* Nothing to write: no cache file, or nothing changed. */
	if (library_cache_path[0] == '\0')
		return;
	if (!library.cache_dirty && !dropped)
		return;

	/* Written beside, in its folder made when missing, then put in place whole. */
	library_cache_folder();
	(void)snprintf(temporary, sizeof(temporary), "%s.new", library_cache_path);
	file = fopen(temporary, "w");
	if (file == NULL)
		return;
	(void)fprintf(file, "%s\n", LIBRARY_CACHE_HEAD);

	/* Each entry met. */
	for (index = 0; index < library.cache_count; index++) {
		entry = &library.cache[index];
		if (!entry->seen)
			continue;
		library_clean_text(entry->tags.title, clean[0], sizeof(clean[0]));
		library_clean_text(entry->tags.artist, clean[1], sizeof(clean[1]));
		library_clean_text(entry->tags.album_artist, clean[2], sizeof(clean[2]));
		library_clean_text(entry->tags.album, clean[3], sizeof(clean[3]));
		(void)fprintf(file, "%s\t%lld\t%lld\t%s\t%s\t%s\t%s\t%d\t%lld\t%d\t%d\t%d\n",
		    entry->path,
		    entry->size,
		    entry->time,
		    clean[0],
		    clean[1],
		    clean[2],
		    clean[3],
		    entry->tags.track,
		    (long long)entry->tags.duration_ms,
		    entry->tags.has_sound,
		    entry->tags.has_video,
		    entry->tags.has_cover);
	}

	/* In place; the cache matches the file now. */
	status = fclose(file);
	if (status != 0)
		return;
	(void)rename(temporary, library_cache_path);
	library.cache_dirty = 0;
}

/* Makes the cache file's folder and the one above it, when they are missing. */
static void
library_cache_folder(void)
{
	char folder[LIBRARY_PATH_MAX];
	char *slash;

	/* The folder of the file. */
	(void)snprintf(folder, sizeof(folder), "%s", library_cache_path);
	slash = strrchr(folder, '/');
	if (slash == NULL)
		return;
	*slash = '\0';

	/* The one above it first (~/.cache), then it. */
	slash = strrchr(folder, '/');
	if (slash != NULL && slash != folder) {
		*slash = '\0';
		(void)mkdir(folder, 0700);
		*slash = '/';
	}

	/* The folder itself. */
	(void)mkdir(folder, 0700);
}

/* Finds a file's entry in the cache (sorted by path), or NULL. */
static struct library_cached *
library_cache_find(
	const char *path)
{
	size_t low;
	size_t high;
	size_t middle;
	int order;

	/* A binary search. */
	low = 0;
	high = library.cache_count;
	while (low < high) {
		middle = low + (high - low) / 2U;
		order = strcmp(library.cache[middle].path, path);
		if (order == 0)
			return &library.cache[middle];
		if (order < 0)
			low = middle + 1U;
		else
			high = middle;
	}

	/* Not there. */
	return NULL;
}

/* Puts a file's tags (without the cover) in the cache, in the order of the paths. Returns 0 or ENOMEM. */
static int
library_cache_put(
	const char *path,
	const struct stat *status,
	const struct mu_tags *tags)
{
	struct library_cached *entry;
	struct library_cached *grown;
	size_t at;
	size_t room;
	int order;

	/* An entry of the path already: its tags replaced. */
	library.cache_dirty = 1;
	entry = library_cache_find(path);
	if (entry != NULL) {
		entry->size = (long long)status->st_size;
		entry->time = library_time(status);
		entry->tags = *tags;
		entry->tags.cover = NULL;
		entry->tags.cover_size = 0;
		entry->seen = 1;
		return 0;
	}

	/* Room for one more. */
	if (library.cache_count == library.cache_room) {
		room = library.cache_room * 2U;
		if (room == 0U)
			room = 256U;
		grown = realloc(library.cache, room * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		library.cache = grown;
		library.cache_room = room;
	}

	/* Its place in the order. */
	at = 0;
	while (at < library.cache_count) {
		order = strcmp(library.cache[at].path, path);
		if (order > 0)
			break;
		at++;
	}

	/* Room made there, then its path. */
	memmove(&library.cache[at + 1U], &library.cache[at], (library.cache_count - at) * sizeof(*library.cache));
	entry = &library.cache[at];
	memset(entry, 0, sizeof(*entry));
	entry->path = library_copy(path);
	if (entry->path == NULL) {
		memmove(&library.cache[at], &library.cache[at + 1U], (library.cache_count - at) * sizeof(*library.cache));
		return ENOMEM;
	}

	/* The entry. */
	entry->size = (long long)status->st_size;
	entry->time = library_time(status);
	entry->tags = *tags;
	entry->tags.cover = NULL;
	entry->tags.cover_size = 0;
	entry->seen = 1;
	library.cache_count++;

	/* Succeeded: in the cache. */
	return 0;
}

/* Reads one line of the cache file into an entry (its path allocated). Returns 0, EINVAL for a line that is not one, or ENOMEM. */
static int
library_cache_line(
	char *line,
	struct library_cached *entry)
{
	char *fields[LIBRARY_CACHE_FIELDS];
	char *at;
	char *tab;
	size_t count;

	/* The line's end, then its fields between the tabs. */
	at = strchr(line, '\n');
	if (at != NULL)
		*at = '\0';
	count = 0;
	at = line;
	while (count < LIBRARY_CACHE_FIELDS) {
		fields[count] = at;
		count++;
		tab = strchr(at, '\t');
		if (tab == NULL)
			break;
		*tab = '\0';
		at = tab + 1;
	}

	/* A line of all the fields only. */
	if (count != LIBRARY_CACHE_FIELDS)
		return EINVAL;

	/* The entry. */
	memset(entry, 0, sizeof(*entry));
	entry->size = strtoll(fields[1], NULL, 10);
	entry->time = strtoll(fields[2], NULL, 10);
	(void)snprintf(entry->tags.title, sizeof(entry->tags.title), "%s", fields[3]);
	(void)snprintf(entry->tags.artist, sizeof(entry->tags.artist), "%s", fields[4]);
	(void)snprintf(entry->tags.album_artist, sizeof(entry->tags.album_artist), "%s", fields[5]);
	(void)snprintf(entry->tags.album, sizeof(entry->tags.album), "%s", fields[6]);
	entry->tags.track = atoi(fields[7]);
	entry->tags.duration_ms = strtoll(fields[8], NULL, 10);
	entry->tags.has_sound = atoi(fields[9]);
	entry->tags.has_video = atoi(fields[10]);
	entry->tags.has_cover = atoi(fields[11]);
	entry->path = library_copy(fields[0]);
	if (entry->path == NULL)
		return ENOMEM;

	/* Succeeded: an entry. */
	return 0;
}

/* Copies a text for the cache file: its tabs and line ends as spaces. */
static void
library_clean_text(
	const char *text,
	char *clean,
	size_t size)
{
	size_t index;

	/* Each byte. */
	for (index = 0; text[index] != '\0' && index + 1U < size; index++) {
		clean[index] = text[index];
		if (text[index] == '\t' || text[index] == '\n' || text[index] == '\r')
			clean[index] = ' ';
	}

	/* Ended. */
	clean[index] = '\0';
}

/* Gives a file's time of change in nanoseconds (the cache's time). */
static long long
library_time(
	const struct stat *status)
{
	/* The seconds and the nanoseconds. */
	return (long long)status->st_mtim.tv_sec * 1000000000LL + (long long)status->st_mtim.tv_nsec;
}
