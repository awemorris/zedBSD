/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Owns ~/Pictures/Media/metadata.db: JSON with extensions preserved across updates. */
#include "userland/desktop/photos/photos.h"
#include "json.h"
#include "notify.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct mj_value *database;
static int database_container(const char *key, struct mj_value **array);
static const char *database_text(struct mj_value *object, const char *key);
static int database_number(struct mj_value *object, const char *key, int64_t *number);
static int database_photo(struct mj_value *record, const char *root);
static int database_album(struct mj_value *record);
static struct mj_value *database_record(struct mj_value *array, const char *id);
static int database_sync(void);
static int database_sync_photo(struct mj_value *record, const struct ph_photo *photo);

/*
 * Provides the current user's media root; originals remain ordinary files below it.
 */
int
ph_library_root(
	char *root,
	size_t size)
{
	const char *home;
	int length;

	/* A missing session home cannot be replaced by another user's database. */
	home = getenv("HOME");
	if (home == NULL || home[0] != '/')
		return ENOENT;
	length = snprintf(root, size, "%s/Pictures/Media", home);
	if (length < 0 || (size_t)length >= size)
		return ENAMETOOLONG;

	/* Succeeded: default commands operate on this user's Media folder. */
	return 0;
}

/*
 * Loads JSON into the shared in-memory media model, preserving the original extension tree.
 */
int
ph_db_load(
	const char *root)
{
	struct mj_value *array;
	struct mj_value *record;
	FILE *input;
	char path[PH_PATH_MAX];
	int64_t version;
	int length;
	int error;

	/* Never silently replace malformed or unsupported metadata with an empty database. */
	mj_free(database);
	database = NULL;
	length = snprintf(path, sizeof(path), "%s/metadata.db", root);
	if (length < 0 || (size_t)length >= sizeof(path))
		return ENAMETOOLONG;
	input = fopen(path, "r");
	if (input == NULL) {
		if (errno != ENOENT)
			return errno;
		database = mj_new(MJ_OBJECT);
		if (database == NULL)
			return ENOMEM;
		error = mj_number(database, "version", 1);
	} else {
		/* Complete JSON parsing precedes model mutation. */
		error = mj_read(input, &database);
		fclose(input);
	}

	/* Future format versions require explicit migration rather than accidental rewriting. */
	if (error != 0)
		return error;
	error = database_number(database, "version", &version);
	if (error != 0 || version != 1)
		return ENOTSUP;
	error = database_container("media", &array);
	if (error != 0)
		return error;
	for (record = array->child; record != NULL; record = record->next) {
		error = database_photo(record, root);
		if (error != 0)
			return error;
	}

	/* Album records retain unknown fields too, while memberships use stable media ids. */
	error = database_container("albums", &array);
	if (error != 0)
		return error;
	for (record = array->child; record != NULL; record = record->next) {
		error = database_album(record);
		if (error != 0)
			return error;
	}

	/* Succeeded: clients receive a consistently sorted snapshot. */
	ph_library_sort();
	return 0;
}

/*
 * Atomically replaces one JSON metadata file after merging the changed in-memory fields.
 */
int
ph_db_save(
	const char *root)
{
	struct ph_photo *photos;
	struct ph_album *albums;
	FILE *output;
	char path[PH_PATH_MAX];
	char temporary[PH_PATH_MAX];
	size_t count;
	size_t index;
	int changed;
	int descriptor;
	int length;
	int error;
	int result;

	/* Read-only queries and duplicate imports leave the metadata file untouched. */
	changed = 0;
	photos = ph_photos(&count);
	for (index = 0U; index < count; index++) {
		if (photos[index].changed)
			changed = 1;
	}

	/* Album-only edits still require a database replacement. */
	albums = ph_albums(&count);
	for (index = 0U; index < count; index++) {
		if (albums[index].changed)
			changed = 1;
	}

	/* An untouched new library becomes a documented empty JSON database on first query. */
	length = snprintf(path, sizeof(path), "%s/metadata.db", root);
	if (length < 0 || (size_t)length >= sizeof(path))
		return ENAMETOOLONG;
	result = access(path, F_OK);
	if (!changed && result == 0)
		return 0;
	error = database_sync();
	if (error != 0)
		return error;
	length = snprintf(temporary, sizeof(temporary), "%s/.metadata-XXXXXX", root);
	if (length < 0 || (size_t)length >= sizeof(temporary))
		return ENAMETOOLONG;
	descriptor = mkstemp(temporary);
	if (descriptor < 0)
		return errno;
	output = fdopen(descriptor, "w");
	if (output == NULL) {
		error = errno;
		close(descriptor);
		unlink(temporary);
		return error;
	}

	/* Flush and sync the new document before replacing the old directory entry. */
	error = mj_write(output, database);
	if (error == 0) {
		result = fflush(output);
		if (result != 0)
			error = errno;
	}

	/* An IO failure preserves the previous metadata file. */
	if (error == 0) {
		result = fsync(descriptor);
		if (result != 0)
			error = errno;
	}

	/* Close failures must not publish an incompletely written replacement. */
	result = fclose(output);
	if (result != 0 && error == 0)
		error = errno;
	if (error == 0) {
		result = rename(temporary, path);
		if (result != 0)
			error = errno;
	}

	/* Only our own failed temporary file is removed. */
	if (error != 0) {
		unlink(temporary);
		return error;
	}

	/* Succeeded: metadata and unknown extension fields are recoverable as ordinary JSON. */
	return 0;
}

/*
 * Releases the private metadata tree at command exit.
 */
void
media_database_release(
	void)
{
	/* Model strings have separate ownership from the JSON tree. */
	mj_free(database);
	database = NULL;
}

/* Finds or creates a top-level array without dropping unrelated root fields. */
static int
database_container(
	const char *key,
	struct mj_value **array)
{
	int error;

	/* Missing optional containers are initialized for version one. */
	*array = mj_get(database, key);
	if (*array == NULL) {
		*array = mj_new(MJ_ARRAY);
		if (*array == NULL)
			return ENOMEM;
		error = mj_attach(database, key, *array);
		if (error != 0) {
			mj_free(*array);
			return error;
		}
	}

	/* Wrong types indicate malformed metadata rather than an empty library. */
	if ((*array)->type != MJ_ARRAY)
		return EINVAL;

	/* Succeeded: the database owns the returned array. */
	return 0;
}

/* Obtains one required string field without coercing an extension's type. */
static const char *
database_text(
	struct mj_value *object,
	const char *key)
{
	struct mj_value *value;

	/* Known fields have a stable, documented JSON type. */
	value = mj_get(object, key);
	if (value == NULL || value->type != MJ_STRING)
		return NULL;

	/* Succeeded: the tree retains ownership of this string. */
	return value->text;
}

/* Reads an integer while rejecting decimal or overflowing known fields. */
static int
database_number(
	struct mj_value *object,
	const char *key,
	int64_t *number)
{
	struct mj_value *value;
	char *end;
	long long parsed;

	/* Future unknown numbers are preserved lexically; known fields must be integral. */
	value = mj_get(object, key);
	if (value == NULL || value->type != MJ_NUMBER)
		return EINVAL;
	errno = 0;
	parsed = strtoll(value->text, &end, 10);
	if (errno != 0 || *end != 0)
		return EINVAL;
	*number = (int64_t)parsed;

	/* Succeeded: the model can represent this field without truncation. */
	return 0;
}

/* Restores one media record with a relative file path and explicit date/size fields. */
static int
database_photo(
	struct mj_value *record,
	const char *root)
{
	struct ph_photo photo;
	const char *id;
	const char *hash;
	const char *path;
	const char *name;
	const char *bad;
	const char *keys[7] = {"size", "taken", "imported", "width", "height", "favorite", "turns"};
	int64_t numbers[7];
	size_t index;
	size_t id_length;
	size_t hash_length;
	int error;
	int prefix;

	/* Known text fields remain compatible with the path-only snapshot protocol. */
	id = database_text(record, "id");
	hash = database_text(record, "sha256");
	path = database_text(record, "path");
	name = database_text(record, "original_name");
	if (id == NULL || hash == NULL || path == NULL || name == NULL)
		return EINVAL;
	id_length = strlen(id);
	hash_length = strlen(hash);
	if (id_length != PH_ID_SIZE - 1U || hash_length != PH_HASH_SIZE - 1U)
		return EINVAL;
	prefix = strncmp(path, "Files/", 6U);
	bad = strpbrk(path, "\t\r\n");
	if (prefix != 0 || bad != NULL)
		return EINVAL;
	bad = strpbrk(name, "\t\r\n");
	if (bad != NULL)
		return EINVAL;
	for (index = 0U; index < 7U; index++) {
		error = database_number(record, keys[index], &numbers[index]);
		if (error != 0)
			return error;
	}

	/* Validate model ranges before publishing any record. */
	if (numbers[0] < 0 || numbers[3] < 0 || numbers[3] > INT_MAX || numbers[4] < 0 || numbers[4] > INT_MAX || numbers[5] < 0 || numbers[5] > 1 || numbers[6] < 0 || numbers[6] > 3)
		return EINVAL;
	memset(&photo, 0, sizeof(photo));
	memcpy(photo.id, id, PH_ID_SIZE);
	memcpy(photo.hash, hash, PH_HASH_SIZE);
	photo.size = (uint64_t)numbers[0];
	photo.taken = numbers[1];
	photo.imported = numbers[2];
	photo.width = (int)numbers[3];
	photo.height = (int)numbers[4];
	photo.favorite = (int)numbers[5];
	photo.turns = (int)numbers[6];
	error = ph_library_add(&photo, root, path, name);

	/* Succeeded: the model owns separate path/name storage. */
	return error;
}

/* Restores one album's name and memberships from its extensible object. */
static int
database_album(
	struct mj_value *record)
{
	struct mj_value *members;
	struct mj_value *member;
	const char *id;
	const char *name;
	size_t index;
	int error;

	/* The membership container is required but may be empty. */
	id = database_text(record, "id");
	name = database_text(record, "name");
	members = mj_get(record, "members");
	if (id == NULL || name == NULL || members == NULL || members->type != MJ_ARRAY)
		return EINVAL;
	error = ph_album_new(id, name, &index);
	if (error != 0)
		return error;
	for (member = members->child; member != NULL; member = member->next) {
		if (member->type != MJ_STRING)
			return EINVAL;
		error = ph_album_add(index, member->text);
		if (error != 0)
			return error;
	}

	/* Succeeded: no extension fields were interpreted or discarded. */
	return 0;
}

/* Finds an existing JSON record so unknown fields stay attached to the same stable id. */
static struct mj_value *
database_record(
	struct mj_value *array,
	const char *id)
{
	struct mj_value *record;
	const char *known;
	int same;

	/* Existing identities are reused rather than reconstructed from the visible snapshot. */
	for (record = array->child; record != NULL; record = record->next) {
		known = database_text(record, "id");
		if (known == NULL)
			continue;
		same = strcmp(known, id);
		if (same == 0)
			return record;
	}

	/* New imports own a fresh JSON object. */
	record = mj_new(MJ_OBJECT);
	if (record != NULL)
		mj_append(array, record);

	/* Succeeded: NULL reports allocation failure. */
	return record;
}

/* Merges model fields into the preserved document before its atomic replacement. */
static int
database_sync(
	void)
{
	struct mj_value *array;
	struct mj_value *record;
	struct mj_value *members;
	struct mj_value *member;
	struct ph_photo *photos;
	struct ph_album *albums;
	size_t count;
	size_t index;
	size_t entry;
	int error;

	/* Only changed media records need their known fields updated. */
	error = database_container("media", &array);
	if (error != 0)
		return error;
	photos = ph_photos(&count);
	for (index = 0U; index < count; index++) {
		if (!photos[index].changed)
			continue;
		record = database_record(array, photos[index].id);
		if (record == NULL)
			return ENOMEM;
		error = database_sync_photo(record, &photos[index]);
		if (error != 0)
			return error;
	}

	/* Album extension fields remain alongside the updated known fields. */
	error = database_container("albums", &array);
	if (error != 0)
		return error;
	albums = ph_albums(&count);
	for (index = 0U; index < count; index++) {
		if (!albums[index].changed)
			continue;
		record = database_record(array, albums[index].id);
		if (record == NULL)
			return ENOMEM;
		error = mj_set(record, "id", MJ_STRING, albums[index].id);
		if (error == 0)
			error = mj_set(record, "name", MJ_STRING, albums[index].name);
		if (error != 0)
			return error;
		members = mj_get(record, "members");
		if (members == NULL) {
			members = mj_new(MJ_ARRAY);
			if (members == NULL)
				return ENOMEM;
			error = mj_attach(record, "members", members);
			if (error != 0) {
				mj_free(members);
				return error;
			}
		}

		/* Replace only the known membership list. */
		mj_free(members->child);
		members->child = NULL;
		members->tail = NULL;
		for (entry = 0U; entry < albums[index].count; entry++) {
			member = mj_new(MJ_STRING);
			if (member == NULL)
				return ENOMEM;
			member->text = strdup(albums[index].members[entry]);
			if (member->text == NULL) {
				mj_free(member);
				return ENOMEM;
			}

			/* Transfer each completed member string to the array. */
			mj_append(members, member);
		}
	}

	/* Succeeded: the document includes both current known fields and future extensions. */
	return 0;
}

/* Updates the documented fields of a media object without removing future metadata. */
static int
database_sync_photo(
	struct mj_value *record,
	const struct ph_photo *photo)
{
	const char *keys[7] = {"size", "taken", "imported", "width", "height", "favorite", "turns"};
	int64_t numbers[7];
	size_t index;
	int error;

	/* Stable identity, file path, hash and original filename accompany every record. */
	error = mj_set(record, "id", MJ_STRING, photo->id);
	if (error == 0)
		error = mj_set(record, "path", MJ_STRING, photo->relative);
	if (error == 0)
		error = mj_set(record, "sha256", MJ_STRING, photo->hash);
	if (error == 0)
		error = mj_set(record, "original_name", MJ_STRING, photo->original);
	if (error != 0)
		return error;
	numbers[0] = (int64_t)photo->size;
	numbers[1] = photo->taken;
	numbers[2] = photo->imported;
	numbers[3] = photo->width;
	numbers[4] = photo->height;
	numbers[5] = photo->favorite;
	numbers[6] = photo->turns;
	for (index = 0U; index < 7U; index++) {
		error = mj_number(record, keys[index], numbers[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: geographic or other unknown properties remain in this object. */
	return 0;
}
