/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The library in memory (ws157-p004; photos.h): the photos the database
 * and the imports gave, in the order of their dates (the newest first,
 * then of their paths), and the albums in the order of their names, each
 * with the sorted ids of its photos.  The lists the view shows (every
 * photo, the favourites, an album's) are made from them.
 */

#include "photos.h"
#include "userland/desktop/picture/media-kind.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/*
 * The library: the photos and the albums, with the room allocated for
 * each.
 */
struct library {
	struct ph_photo *photos;
	size_t photo_count;
	size_t photo_room;
	struct ph_album *albums;
	size_t album_count;
	size_t album_room;
};

/* The library of the program. */
static struct library library;

static int library_compare_photos(const void *left, const void *right);
static int library_compare_albums(const void *left, const void *right);
static int library_compare_ids(const void *left, const void *right);
static char *library_copy(const char *text);

/*
 * Adds a photo (its id, hash, dates, marks; its file under the library's
 * folder and the name it was imported with).  The order is made again by
 * ph_library_sort.  Returns 0, ENOSPC when the library is full, ENOMEM.
 */
int
ph_library_add(
	const struct ph_photo *photo,
	const char *root,
	const char *relative,
	const char *original)
{
	struct ph_photo *grown;
	struct ph_photo *added;
	const char *slash;
	size_t length;
	size_t room;

	/* The library is full. */
	if (library.photo_count == PH_PHOTOS_MAX)
		return ENOSPC;

	/* Room for it. */
	if (library.photo_count == library.photo_room) {
		room = library.photo_room * 2U;
		if (room == 0U)
			room = 64U;
		grown = realloc(library.photos, room * sizeof(grown[0]));
		if (grown == NULL)
			return ENOMEM;
		library.photos = grown;
		library.photo_room = room;
	}

	/* The photo, its path the root's and the relative part's. */
	added = &library.photos[library.photo_count];
	*added = *photo;
	length = strlen(root) + 1U + strlen(relative) + 1U;
	added->path = malloc(length);
	added->original = library_copy(original);
	if (added->path == NULL || added->original == NULL) {
		free(added->path);
		free(added->original);
		return ENOMEM;
	}

	/* Its parts point into its path. */
	(void)snprintf(added->path, length, "%s/%s", root, relative);
	added->relative = added->path + strlen(root) + 1U;
	slash = strrchr(added->path, '/');
	added->name = slash + 1;
	library.photo_count++;
	return 0;
}

/*
 * Puts the photos in the order of their dates (the newest first, then of
 * their paths) and the albums in the order of their names.
 */
void
ph_library_sort(void)
{
	/* Both lists. */
	if (library.photo_count > 1U)
		qsort(library.photos, library.photo_count, sizeof(library.photos[0]), library_compare_photos);
	if (library.album_count > 1U)
		qsort(library.albums, library.album_count, sizeof(library.albums[0]), library_compare_albums);
}

/*
 * Reports the photos, in order.
 */
struct ph_photo *
ph_photos(
	size_t *count)
{
	/* The array. */
	*count = library.photo_count;
	return library.photos;
}

/*
 * Reports the albums, in the order of their names.
 */
struct ph_album *
ph_albums(
	size_t *count)
{
	/* The array. */
	*count = library.album_count;
	return library.albums;
}

/*
 * Finds the photo of a content's hash: its index, or -1.
 */
long
ph_library_find_hash(
	const char *hash)
{
	size_t index;
	int same;

	/* Each photo. */
	for (index = 0; index < library.photo_count; index++) {
		same = strcmp(library.photos[index].hash, hash);
		if (same == 0)
			return (long)index;
	}

	/* None. */
	return -1;
}

/*
 * Finds the photo of an id: its index, or -1.
 */
long
ph_library_find_id(
	const char *id)
{
	size_t index;
	int same;

	/* Each photo. */
	for (index = 0; index < library.photo_count; index++) {
		same = strcmp(library.photos[index].id, id);
		if (same == 0)
			return (long)index;
	}

	/* None. */
	return -1;
}

/*
 * Lists the photos of a list (PH_LIST_*; an album's index for
 * PH_LIST_ALBUM), in order.  Returns how many indices are stored, at most
 * the capacity.
 */
size_t
ph_library_list(
	int list,
	size_t album,
	size_t *indices,
	size_t capacity)
{
	const struct ph_photo *photo;
	size_t count;
	size_t index;
	int member;

	/* An album that is not there lists nothing. */
	if (list == PH_LIST_ALBUM && album >= library.album_count)
		return 0;

	/* Each photo, in order. */
	count = 0;
	for (index = 0; index < library.photo_count && count < capacity; index++) {
		photo = &library.photos[index];
		if (list == PH_LIST_FAVORITES && !photo->favorite)
			continue;
		if (list == PH_LIST_ALBUM) {
			member = ph_album_has(&library.albums[album], photo->id);
			if (!member)
				continue;
		}

		/* Listed. */
		indices[count] = index;
		count++;
	}

	/* The photos listed. */
	return count;
}

/*
 * Adds an album of an id and a name (from its file), without photos.
 * Returns 0 with its index (the order is made again by ph_library_sort),
 * or ENOMEM.
 */
int
ph_album_new(
	const char *id,
	const char *name,
	size_t *album)
{
	struct ph_album *grown;
	size_t room;

	/* Room for it. */
	if (library.album_count == library.album_room) {
		room = library.album_room * 2U;
		if (room == 0U)
			room = 16U;
		grown = realloc(library.albums, room * sizeof(grown[0]));
		if (grown == NULL)
			return ENOMEM;
		library.albums = grown;
		library.album_room = room;
	}

	/* The album. */
	*album = library.album_count;
	memset(&library.albums[*album], 0, sizeof(library.albums[0]));
	(void)snprintf(library.albums[*album].id, sizeof(library.albums[0].id), "%s", id);
	(void)snprintf(library.albums[*album].name, sizeof(library.albums[0].name), "%s", name);
	library.album_count++;
	return 0;
}

/*
 * Makes a new album of a name, with a new random id, to be written.
 * Returns 0 with its index in the order of the names, EINVAL for an empty
 * name or one with a tab or a line's end, or ENOMEM.
 */
int
ph_album_create(
	const char *name,
	size_t *album)
{
	unsigned char random[16];
	char id[PH_ID_SIZE];
	const char *bad;
	size_t index;
	int error;
	int same;

	/* A name that can be a line. */
	bad = strpbrk(name, "\t\r\n");
	if (name[0] == '\0' || bad != NULL)
		return EINVAL;

	/* A new id. */
	error = getentropy(random, sizeof(random));
	if (error != 0)
		return EIO;
	for (index = 0; index < sizeof(random); index++)
		(void)snprintf(id + index * 2U, 3U, "%02x", random[index]);

	/* The album, to be written, and its place in the order. */
	error = ph_album_new(id, name, album);
	if (error != 0)
		return error;
	library.albums[*album].changed = 1;
	ph_library_sort();

	/* Its index after the sort. */
	for (index = 0; index < library.album_count; index++) {
		same = strcmp(library.albums[index].id, id);
		if (same == 0)
			*album = index;
	}

	/* Made. */
	return 0;
}

/*
 * Adds a photo's id to an album (nothing when it is there already).
 * Returns 0, EINVAL for an album that is not there, or ENOMEM.
 */
int
ph_album_add(
	size_t album,
	const char *id)
{
	struct ph_album *chosen;
	char (*grown)[PH_ID_SIZE];
	size_t room;
	int member;

	/* The album, and the id not in it yet. */
	if (album >= library.album_count)
		return EINVAL;
	chosen = &library.albums[album];
	member = ph_album_has(chosen, id);
	if (member)
		return 0;

	/* Room for one more. */
	if (chosen->count == chosen->room) {
		room = chosen->room * 2U;
		if (room == 0U)
			room = 16U;
		grown = realloc(chosen->members, room * sizeof(grown[0]));
		if (grown == NULL)
			return ENOMEM;
		chosen->members = grown;
		chosen->room = room;
	}

	/* Added and sorted, the album to be written. */
	(void)snprintf(chosen->members[chosen->count], PH_ID_SIZE, "%s", id);
	chosen->count++;
	qsort(chosen->members, chosen->count, sizeof(chosen->members[0]), library_compare_ids);
	chosen->changed = 1;
	return 0;
}

/*
 * Tells whether an album holds a photo's id.
 */
int
ph_album_has(
	const struct ph_album *album,
	const char *id)
{
	const void *found;

	/* The sorted ids. */
	if (album->count == 0U)
		return 0;
	found = bsearch(id, album->members, album->count, sizeof(album->members[0]), library_compare_ids);
	return found != NULL;
}

/*
 * Frees the library.
 */
void
ph_library_release(void)
{
	size_t index;

	/* Each photo's strings. */
	for (index = 0; index < library.photo_count; index++) {
		free(library.photos[index].path);
		free(library.photos[index].original);
	}

	/* Each album's ids. */
	for (index = 0; index < library.album_count; index++)
		free(library.albums[index].members);

	/* The arrays. */
	free(library.photos);
	free(library.albums);
	memset(&library, 0, sizeof(library));
}

/*
 * Tells what a file's first bytes say it is (PH_KIND_*).
 */
int
ph_picture_kind(
	const unsigned char *data,
	size_t size)
{
	int kind;

	/* Shares import classification with other media consumers. */
	kind = kl_media_kind(data, size);

	/* Succeeded: the classifier includes unsupported contents as PH_KIND_NONE. */
	return kind;
}

/* Orders two photos: the newest first, then by path. */
static int
library_compare_photos(
	const void *left,
	const void *right)
{
	const struct ph_photo *one;
	const struct ph_photo *other;

	/* The dates. */
	one = left;
	other = right;
	if (one->taken > other->taken)
		return -1;
	if (one->taken < other->taken)
		return 1;

	/* The paths. */
	return strcmp(one->path, other->path);
}

/* Orders two albums by their names, without regard to case, then by their ids. */
static int
library_compare_albums(
	const void *left,
	const void *right)
{
	const struct ph_album *one;
	const struct ph_album *other;
	int order;

	/* The names, then the ids. */
	one = left;
	other = right;
	order = strcasecmp(one->name, other->name);
	if (order != 0)
		return order;
	return strcmp(one->id, other->id);
}

/* Orders two ids. */
static int
library_compare_ids(
	const void *left,
	const void *right)
{
	/* As text. */
	return strcmp(left, right);
}

/* Copies a string (NULL when there is no room). */
static char *
library_copy(
	const char *text)
{
	size_t length;
	char *copy;

	/* The bytes and the NUL. */
	length = strlen(text) + 1U;
	copy = malloc(length);
	if (copy == NULL)
		return NULL;
	memcpy(copy, text, length);
	return copy;
}
