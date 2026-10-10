/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Serializes a library model for compositor transport without accessing its database. */
#include "photos.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The longest transport row, including the absolute library root. */
#define SNAPSHOT_LINE_MAX (PH_PATH_MAX * 3U + 512U)

static int snapshot_fields(char *line, char **fields, size_t count);
static int snapshot_photo(char **fields, const char *root);
static int snapshot_album(char **fields);
static int snapshot_member(char **fields);
static int snapshot_number(const char *text, int64_t *number);

/*
 * Writes the complete library as a versioned text snapshot, including albums.
 */
int
ph_snapshot_write(
	FILE *output,
	const char *root)
{
	struct ph_photo *photos;
	struct ph_album *albums;
	size_t count;
	size_t index;
	size_t member;
	int written;

	/* Keeps a root and relative paths separate so copied libraries remain recoverable. */
	written = fprintf(output, "# keiland-media 1\t%s\n", root);
	if (written < 0)
		return EIO;
	photos = ph_photos(&count);

	/* Carries each photo/video's stable id, original name and mutable metadata. */
	for (index = 0U; index < count; index++) {
		written = fprintf(output, "P\t%s\t%s\t%s\t%llu\t%lld\t%lld\t%d\t%d\t%d\t%d\t%s\n", photos[index].id, photos[index].path, photos[index].hash, (unsigned long long)photos[index].size, (long long)photos[index].taken, (long long)photos[index].imported, photos[index].width, photos[index].height, photos[index].favorite, photos[index].turns, photos[index].original);
		if (written < 0)
			return EIO;
	}

	/* Defines albums before their memberships, preserving empty albums too. */
	albums = ph_albums(&count);
	for (index = 0U; index < count; index++) {
		written = fprintf(output, "A\t%s\t%s\n", albums[index].id, albums[index].name);
		if (written < 0)
			return EIO;
		for (member = 0U; member < albums[index].count; member++) {
			written = fprintf(output, "M\t%s\t%s\n", albums[index].id, albums[index].members[member]);
			if (written < 0)
				return EIO;
		}
	}

	/* Succeeded: the caller can publish this snapshot independently of the database. */
	return 0;
}

/*
 * Reads a compositor snapshot into an empty library model.
 */
int
ph_snapshot_read(
	FILE *input,
	char *root,
	size_t size)
{
	char line[SNAPSHOT_LINE_MAX];
	char *fields[12];
	char *got;
	size_t length;
	size_t album;
	struct ph_album *albums;
	size_t album_count;
	size_t index;
	int count;
	int error;
	int same;

	/* Requires the versioned root header before any relative path is accepted. */
	got = fgets(line, sizeof(line), input);
	if (got == NULL)
		return EIO;
	same = strncmp(line, "# keiland-media 1\t", 18U);
	if (same != 0)
		return EPROTO;
	length = strcspn(line + 18U, "\r\n");
	if (length == 0U || length >= size || line[18] != '/')
		return EINVAL;
	memcpy(root, line + 18U, length);
	root[length] = '\0';

	/* Parses complete rows, refusing truncated snapshots rather than presenting partial results. */
	for (;;) {
		got = fgets(line, sizeof(line), input);
		if (got == NULL)
			break;
		length = strlen(line);
		if (length == 0U || line[length - 1U] != '\n')
			return EPROTO;
		line[length - 1U] = '\0';
		count = snapshot_fields(line, fields, 12U);
		error = EPROTO;
		if (count == 12 && fields[0][0] == 'P' && fields[0][1] == '\0') {
			error = snapshot_photo(fields, root);
		} else if (count == 3 && fields[0][0] == 'A' && fields[0][1] == '\0') {
			error = ph_album_new(fields[1], fields[2], &album);
		} else if (count == 3 && fields[0][0] == 'M' && fields[0][1] == '\0') {
			error = snapshot_member(fields);
		}

		/* Leaves the caller responsible for releasing an incomplete model on failure. */
		if (error != 0)
			return error;
	}

	/* A stream failure is distinct from the complete final row. */
	error = ferror(input);
	if (error != 0)
		return EIO;
	ph_library_sort();
	albums = ph_albums(&album_count);
	for (index = 0U; index < album_count; index++)
		albums[index].changed = 0;

	/* Succeeded: the model describes the CLI's entire current library. */
	return 0;
}

/*
 * Applies only dirty metadata from a client's model to the CLI's freshly read library.
 */
int
ph_snapshot_apply(
	FILE *input)
{
	char line[SNAPSHOT_LINE_MAX];
	char *fields[4];
	char *got;
	struct ph_photo *photos;
	size_t count;
	long index;
	int64_t favorite;
	int64_t turns;
	int error;
	int split;

	/* Processes id-based changes without replacing concurrent additions. */
	for (;;) {
		got = fgets(line, sizeof(line), input);
		if (got == NULL)
			break;
		split = snapshot_fields(line, fields, 4U);
		if (split < 0)
			return EINVAL;
		error = EINVAL;
		if (split == 4 && fields[0][0] == 'P') {
			error = snapshot_number(fields[2], &favorite);
			if (error != 0)
				return error;
			error = snapshot_number(fields[3], &turns);
			if (error != 0 || favorite < 0 || favorite > 1 || turns < 0 || turns > 3)
				return EINVAL;
			index = ph_library_find_id(fields[1]);
			if (index < 0)
				return ENOENT;
			photos = ph_photos(&count);
			photos[index].favorite = (int)favorite;
			photos[index].turns = (int)turns;
			photos[index].changed = 1;
			error = 0;
		} else if (split == 3 && fields[0][0] == 'A') {
			error = snapshot_album(fields);
		} else if (split == 3 && fields[0][0] == 'M') {
			error = snapshot_member(fields);
		}

		/* Refuses malformed updates before writing the persistent database. */
		if (error != 0)
			return error;
	}

	/* Stream errors cannot be reported as a successful metadata update. */
	error = ferror(input);
	if (error != 0)
		return EIO;

	/* Succeeded: dirty model records can be saved atomically by month or album. */
	return 0;
}

/*
 * Writes dirty marks and album definitions as id-based CLI update commands.
 */
int
ph_snapshot_changes(
	FILE *output)
{
	struct ph_photo *photos;
	struct ph_album *albums;
	size_t count;
	size_t index;
	size_t member;
	int written;

	/* Emits only edited media marks so unrelated concurrent changes survive. */
	photos = ph_photos(&count);
	for (index = 0U; index < count; index++) {
		if (!photos[index].changed)
			continue;
		written = fprintf(output, "P\t%s\t%d\t%d\n", photos[index].id, photos[index].favorite, photos[index].turns);
		if (written < 0)
			return EIO;
	}

	/* Album memberships are additive, preserving imports made by other clients. */
	albums = ph_albums(&count);
	for (index = 0U; index < count; index++) {
		if (!albums[index].changed)
			continue;
		written = fprintf(output, "A\t%s\t%s\n", albums[index].id, albums[index].name);
		if (written < 0)
			return EIO;
		for (member = 0U; member < albums[index].count; member++) {
			written = fprintf(output, "M\t%s\t%s\n", albums[index].id, albums[index].members[member]);
			if (written < 0)
				return EIO;
		}
	}

	/* Succeeded: the CLI can merge these edits into its freshly loaded database. */
	return 0;
}

/* Splits one tab-separated row without losing empty final fields. */
static int
snapshot_fields(
	char *line,
	char **fields,
	size_t capacity)
{
	char *at;
	size_t count;
	size_t length;

	/* Removes one transport newline before splitting. */
	length = strlen(line);
	if (length > 0U && line[length - 1U] == '\n')
		line[--length] = '\0';
	count = 0U;
	at = line;
	for (;;) {
		if (count == capacity)
			return -1;
		fields[count++] = at;
		at = strchr(at, '\t');
		if (at == NULL)
			break;
		*at++ = '\0';
	}

	/* Succeeded: every field has a separate terminator. */
	return (int)count;
}

/* Adds one transport photo/video row to the client's memory model. */
static int
snapshot_photo(
	char **fields,
	const char *root)
{
	struct ph_photo photo;
	int64_t numbers[7];
	size_t index;
	size_t length;
	size_t root_length;
	const char *relative;
	int same;
	int error;

	/* Reads numeric metadata before allocating the photo's strings. */
	memset(&photo, 0, sizeof(photo));
	for (index = 0U; index < 7U; index++) {
		error = snapshot_number(fields[index + 4U], &numbers[index]);
		if (error != 0)
			return error;
	}

	/* Requires complete stable identities and a relative path. */
	length = strlen(fields[1]);
	if (length != PH_ID_SIZE - 1U)
		return EINVAL;
	length = strlen(fields[3]);
	if (length != PH_HASH_SIZE - 1U || numbers[0] < 0)
		return EINVAL;
	memcpy(photo.id, fields[1], PH_ID_SIZE);
	memcpy(photo.hash, fields[3], PH_HASH_SIZE);
	photo.size = (uint64_t)numbers[0];
	photo.taken = numbers[1];
	photo.imported = numbers[2];
	photo.width = (int)numbers[3];
	photo.height = (int)numbers[4];
	photo.favorite = (int)numbers[5];
	photo.turns = (int)numbers[6];
	/* Interprets each absolute file path under the root carried by this snapshot. */
	root_length = strlen(root);
	length = strlen(fields[2]);
	if (length <= root_length)
		return EINVAL;
	same = strncmp(fields[2], root, root_length);
	if (same != 0 || fields[2][root_length] != '/')
		return EINVAL;
	relative = fields[2] + root_length + 1U;
	error = ph_library_add(&photo, root, relative, fields[11]);
	if (error != 0)
		return error;

	/* Succeeded: the memory model owns the row's strings. */
	return 0;
}

/* Updates one album definition without dropping its concurrent memberships. */
static int
snapshot_album(
	char **fields)
{
	struct ph_album *albums;
	size_t count;
	size_t index;
	int same;
	int error;

	/* Preserves the existing album's ids when only its name changes. */
	albums = ph_albums(&count);
	for (index = 0U; index < count; index++) {
		same = strcmp(albums[index].id, fields[1]);
		if (same == 0)
			break;
	}

	/* Creates a new identity only when the latest model has no such album. */
	if (index == count) {
		error = ph_album_new(fields[1], fields[2], &index);
		if (error != 0)
			return error;
		albums = ph_albums(&count);
	}

	/* Marks a definition for the CLI's atomic album save. */
	(void)snprintf(albums[index].name, sizeof(albums[index].name), "%s", fields[2]);
	albums[index].changed = 1;

	/* Succeeded: all prior membership ids remain attached to this identity. */
	return 0;
}

/* Adds an id-based membership after its album definition. */
static int
snapshot_member(
	char **fields)
{
	struct ph_album *albums;
	size_t count;
	size_t index;
	int same;
	int error;

	/* Finds the album by identity rather than its sort position. */
	albums = ph_albums(&count);
	for (index = 0U; index < count; index++) {
		same = strcmp(albums[index].id, fields[1]);
		if (same != 0)
			continue;
		error = ph_album_add(index, fields[2]);
		if (error != 0)
			return error;

		/* Succeeded: this membership is present without duplicate ids. */
		return 0;
	}

	/* A membership cannot precede its album. */
	return ENOENT;
}

/* Reads one complete signed decimal transport field. */
static int
snapshot_number(
	const char *text,
	int64_t *number)
{
	char *end;
	long long value;

	/* Rejects overflow and trailing bytes, including empty fields. */
	errno = 0;
	value = strtoll(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0')
		return EINVAL;
	*number = (int64_t)value;

	/* Succeeded: the field fits the transport's signed range. */
	return 0;
}
