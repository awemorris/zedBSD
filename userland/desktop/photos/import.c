/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The import (ws157-p004; photos.h, plan/ws157/phase001/phase.md D1, D3,
 * D5): a file, or a folder looked through four levels deep (leaving out
 * what is hidden), each JPEG, PNG and GIF in it: its SHA-256, and nothing
 * more when the library has that content already; else the day it was
 * taken (EXIF, else the import date in the local calendar), its place
 * Files/YYYY/MM/dd/<its name> (name-1.ext, name-2.ext... when another
 * content has the name), a copy there (the file given stays where it is)
 * and its line in the library, to be written by ph_db_save.
 */

#include "photos.h"
#include "userland/desktop/mediastorage/dimensions.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sha2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* How deep a folder is looked through, the piece read at once, and the most names tried for a place. */
#define IMPORT_DEPTH		4
#define IMPORT_CHUNK		(256U * 1024U)
#define IMPORT_TRIES		1000

static void import_walk(const char *root, const char *folder, int depth, struct ph_import_result *result);
static void import_file(const char *root, const char *path, struct ph_import_result *result);
static int import_descriptor(const char *root, int fd, const char *name, const struct stat *status);
static int import_one(const char *root, const char *path, const struct stat *status);
static int import_hash(int fd, char *hash, unsigned char *head, size_t head_size, size_t *head_length);
static int import_place(const char *root, const char *name, ph_time taken, char *relative, size_t size);
static int import_copy(int from, const char *to, uint64_t size);
static ph_time import_local(time_t when);

/*
 * Imports a file or a folder's photos into the library of a folder.
 * Returns 0 with the counts (a source that is not there: ENOENT).
 */
int
ph_import(
	const char *root,
	const char *source,
	struct ph_import_result *result)
{
	struct stat status;
	int error;
	int folder;

	/* What the source is. */
	memset(result, 0, sizeof(*result));
	error = stat(source, &status);
	if (error != 0)
		return errno;

	/* A folder's photos, or the file. */
	folder = S_ISDIR(status.st_mode);
	if (folder)
		import_walk(root, source, 0, result);
	else
		import_file(root, source, result);

	/* In order. */
	ph_library_sort();
	return 0;
}

/* Imports the photos of a folder and of its folders down to the depth. */
static void
import_walk(
	const char *root,
	const char *folder,
	int depth,
	struct ph_import_result *result)
{
	struct dirent *entry;
	struct stat status;
	char path[PH_PATH_MAX];
	int length;
	int error;
	int inside;
	int kind_folder;
	int kind_file;
	DIR *opened;

	/* The folder; one that cannot be opened has nothing. */
	opened = opendir(folder);
	if (opened == NULL)
		return;

	/* Each entry but the hidden ones. */
	for (;;) {
		entry = readdir(opened);
		if (entry == NULL)
			break;
		if (entry->d_name[0] == '.')
			continue;
		length = snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
		if (length < 0 || (size_t)length >= sizeof(path))
			continue;
		error = stat(path, &status);
		if (error != 0)
			continue;

		/* A folder down to the depth, never the library itself. */
		inside = strncmp(path, root, strlen(root));
		kind_folder = S_ISDIR(status.st_mode);
		kind_file = S_ISREG(status.st_mode);
		if (kind_folder) {
			if (depth + 1 < IMPORT_DEPTH && inside != 0)
				import_walk(root, path, depth + 1, result);
			continue;
		}

		/* A file. */
		if (kind_file)
			import_file(root, path, result);
	}

	/* Read through. */
	(void)closedir(opened);
}

/* Imports one file when it is a picture, counted. */
static void
import_file(
	const char *root,
	const char *path,
	struct ph_import_result *result)
{
	struct stat status;
	int error;
	int regular;

	/* A regular file. */
	error = stat(path, &status);
	if (error != 0)
		return;
	regular = S_ISREG(status.st_mode);
	if (!regular)
		return;

	/* Imported, there already, failed, or not a picture (not counted). */
	error = import_one(root, path, &status);
	if (error == 0)
		result->imported++;
	else if (error == EEXIST)
		result->duplicates++;
	else if (error != ENOTSUP)
		result->failed++;
	ph_log("IMPORT file=%s error=%d", path, error);
}

/* Imports a named file through the same descriptor path used by received media. */
static int
import_one(
	const char *root,
	const char *path,
	const struct stat *status)
{
	const char *name;
	const char *slash;
	int descriptor;
	int error;

	/* Opens the stable source before hashing and copying it. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return errno;
	slash = strrchr(path, '/');
	name = path;
	if (slash != NULL)
		name = slash + 1;
	error = import_descriptor(root, descriptor, name, status);
	close(descriptor);
	if (error != 0)
		return error;

	/* Succeeded: the original remains intact and the library owns its copy. */
	return 0;
}

static int
import_descriptor(
	const char *root,
	int fd,
	const char *name,
	const struct stat *status)
{
	unsigned char head[16];
	struct ph_photo photo;
	char relative[PH_PATH_MAX];
	char destination[PH_PATH_MAX];
	const char *bad;
	size_t head_length;
	long found;
	int kind;
	int error;
	int length;
	time_t now;

	/* A name that can be a line. */
	bad = strpbrk(name, "/\t\r\n");
	if (bad != NULL || name[0] == '\0')
		return EINVAL;

	/* Its hash and its first bytes. */
	error = import_hash(fd, photo.hash, head, sizeof(head), &head_length);
	if (error != 0) {
		return error;
	}

	/* A picture, not in the library yet. */
	kind = ph_picture_kind(head, head_length);
	if (kind == PH_KIND_NONE) {
		return ENOTSUP;
	}

	/* Not in the library yet. */
	found = ph_library_find_hash(photo.hash);
	if (found >= 0) {
		return EEXIST;
	}

	/* Its line: the id, the dates, no marks. */
	now = time(NULL);
	memset(&photo.id, 0, sizeof(photo.id));
	memcpy(photo.id, photo.hash, PH_ID_SIZE - 1U);
	photo.size = (uint64_t)status->st_size;
	photo.imported = import_local(now);
	photo.taken = photo.imported;
	if (kind == PH_KIND_JPEG)
		(void)ph_exif_descriptor_date(fd, &photo.taken);

	/* Dimensions are read from headers without decoding the retained original. */
	media_dimensions(fd, kind, &photo.width, &photo.height);
	photo.favorite = 0;
	photo.turns = 0;
	photo.changed = 1;
	photo.path = NULL;
	photo.relative = NULL;
	photo.name = NULL;
	photo.original = NULL;

	/* Its place, and the copy there. */
	error = import_place(root, name, photo.taken, relative, sizeof(relative));
	if (error == 0) {
		length = snprintf(destination, sizeof(destination), "%s/%s", root, relative);
		error = ENAMETOOLONG;
		if (length >= 0 && (size_t)length < sizeof(destination))
			error = import_copy(fd, destination, photo.size);
	}

	/* Reports a failed copy without publishing a database row. */
	if (error != 0)
		return error;

	/* In the library. */
	error = ph_library_add(&photo, root, relative, name);
	if (error != 0) {
		(void)unlink(destination);
		return error;
	}

	/* Succeeded: the metadata owns a recoverable file copy. */
	return 0;
}

/* Hashes a whole file and keeps its first bytes; 0 or an errno value. */
static int
import_hash(
	int fd,
	char *hash,
	unsigned char *head,
	size_t head_size,
	size_t *head_length)
{
	SHA2_CTX context;
	unsigned char *chunk;
	ssize_t got;
	size_t take;

	/* Piece by piece. */
	chunk = malloc(IMPORT_CHUNK);
	if (chunk == NULL)
		return ENOMEM;
	SHA256Init(&context);
	*head_length = 0;
	for (;;) {
		got = read(fd, chunk, IMPORT_CHUNK);
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0) {
			free(chunk);
			return EIO;
		}

		/* The end. */
		if (got == 0)
			break;

		/* The first bytes kept, the rest hashed. */
		take = head_size - *head_length;
		if ((size_t)got < take)
			take = (size_t)got;
		memcpy(head + *head_length, chunk, take);
		*head_length += take;
		SHA256Update(&context, chunk, (size_t)got);
	}

	/* The hash in hexadecimal. */
	free(chunk);
	(void)SHA256End(&context, hash);
	return 0;
}

/*
 * Finds the place of a file taken on a day: Files/YYYY/MM/dd/<name>, or
 * <stem>-N<extension> when the name is taken.  Returns 0 or EEXIST.
 */
static int
import_place(
	const char *root,
	const char *name,
	ph_time taken,
	char *relative,
	size_t size)
{
	struct stat status;
	char path[PH_PATH_MAX];
	const char *dot;
	int year;
	int month;
	int day;
	int stem;
	int tries;
	int error;
	int length;

	/* The day's folder, and the name's stem and extension. */
	ph_time_split(taken, &year, &month, &day);
	dot = strrchr(name, '.');
	stem = (int)strlen(name);
	if (dot != NULL && dot != name)
		stem = (int)(dot - name);
	if (dot == NULL || dot == name)
		dot = "";

	/* The name, then name-1, name-2...: the first not there. */
	for (tries = 0; tries < IMPORT_TRIES; tries++) {
		if (tries == 0)
			(void)snprintf(relative, size, "%s/%04d/%02d/%02d/%s", PH_LIBRARY_IMAGES, year, month, day, name);
		else
			(void)snprintf(relative, size, "%s/%04d/%02d/%02d/%.*s-%d%s", PH_LIBRARY_IMAGES, year, month, day, stem, name, tries, dot);
		length = snprintf(path, sizeof(path), "%s/%s", root, relative);
		if (length < 0 || (size_t)length >= sizeof(path))
			return ENAMETOOLONG;
		error = lstat(path, &status);
		if (error != 0 && errno == ENOENT)
			return 0;
	}

	/* Every name was taken. */
	return EEXIST;
}

/* Copies a file from its start to a new file (its folders made); 0, or an errno value (nothing is left). */
static int
import_copy(
	int from,
	const char *to,
	uint64_t size)
{
	unsigned char *chunk;
	uint64_t done;
	ssize_t got;
	ssize_t wrote;
	int error;
	int closed;
	int out;
	size_t copied;

	/* The folders and the new file. */
	error = ph_db_folders(to);
	if (error != 0)
		return error;
	out = open(to, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
	if (out < 0)
		return errno;
	chunk = malloc(IMPORT_CHUNK);
	if (chunk == NULL) {
		(void)close(out);
		(void)unlink(to);
		return ENOMEM;
	}

	/* Piece by piece from the start. */
	done = 0;
	error = 0;
	while (error == 0) {
		got = pread(from, chunk, IMPORT_CHUNK, (off_t)done);
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0)
			error = EIO;
		if (got <= 0)
			break;
		/* Writes each complete chunk even when the filesystem accepts only part of it. */
		copied = 0U;
		while (copied < (size_t)got) {
			wrote = write(out, chunk + copied, (size_t)got - copied);
			if (wrote < 0 && errno == EINTR)
				continue;
			if (wrote <= 0) {
				error = EIO;
				break;
			}

			/* Continues at the first byte not accepted by the output file. */
			copied += (size_t)wrote;
		}

		/* Only a complete chunk advances the original's offset. */
		if (error == 0)
			done += (uint64_t)got;
	}

	/* Closed; a short or failed copy leaves nothing. */
	free(chunk);
	closed = close(out);
	if (closed != 0 && error == 0)
		error = EIO;
	if (error == 0 && done != size)
		error = EIO;
	if (error != 0) {
		(void)unlink(to);
		return error;
	}

	/* Succeeded: the complete original was copied. */
	return 0;
}

/* A time as the local calendar has it (ph_time). */
static ph_time
import_local(
	time_t when)
{
	struct tm local;

	/* The local calendar's fields. */
	memset(&local, 0, sizeof(local));
	(void)localtime_r(&when, &local);
	return ph_time_make(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
}
