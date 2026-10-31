/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The files backend of the storage: one file per record under a directory.
 *
 * Every record lives in its own file, so a write replaces exactly one file
 * through wb_os_file_replace and a reader finds the old record or the new
 * one, never a mixture.  The backend keeps no record in memory: each call
 * goes to the disk, builds the path from the space and the key, and uses
 * only the calls of base/os-file.h.
 */

#include "storage/files.h"
#include "storage/records.h"
#include "base/os-file.h"
#include "base/base.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* The length of the suffix ".ls" that the files of localStorage records end in. */
#define FILES_LOCAL_SUFFIX_LENGTH	3U

/*
 * One open files backend.
 *
 * directory is the absolute form of the directory given at open, without a
 * trailing '/', so a program that changes its current directory afterwards
 * still reaches the same files; lock is this process's hold on it.  Both
 * live from the open to the close call.
 */
struct files_context {
	char *directory;
	struct wb_os_lock *lock;
};

/*
 * What a walk over a space's directory carries: the function the walk
 * hands each valid key to, whether that function stopped the walk, and the
 * value it stopped with (kept apart from the walk's own errors, which
 * include ENOENT).
 */
struct files_walk {
	int is_local;
	int (*each)(void *each_context, const char *key);
	void *each_context;
	int stopped;
	int stop_value;
};

static int files_read(void *context, unsigned space, const char *key, struct browser_storage_sink *sink, int *found);
static int files_write(void *context, unsigned space, const char *key, const void *bytes, size_t length);
static int files_remove(void *context, unsigned space, const char *key);
static int files_list(void *context, unsigned space, int (*each)(void *each_context, const char *key), void *each_context);
static int files_clear(void *context, unsigned space);
static void files_close(void *context);

/*
 * The table the backend hands out.  It is constant and shared by every open
 * backend; the state of each lives in its context.
 */
static const struct browser_storage_calls files_calls = {
	BROWSER_STORAGE_VERSION,
	files_read,
	files_write,
	files_remove,
	files_list,
	files_clear,
	files_close
};

static int files_build_directory(const char *directory, char **absolute);
static int files_space_path(const struct files_context *files, unsigned space, struct wb_buffer *path);
static int files_record_path(const struct files_context *files, unsigned space, const char *key, struct wb_buffer *path);
static int files_walk(const struct files_context *files, unsigned space, int (*each)(void *each_context, const char *key), void *each_context);
static int files_walk_entry(void *context, const char *name);
static int files_remove_record(const struct files_context *files, unsigned space, const char *key);
static int files_collect_key(void *each_context, const char *key);

/*
 * Opens the files backend over a directory.
 *
 * The directory is made when missing and locked for this process, so a
 * second open of it, here or in another process, is refused with EBUSY.
 */
int
browser_storage_files_open(
	const char *directory,
	const struct browser_storage_calls **calls,
	void **context)
{
	struct files_context *files;
	int error;

	/* Leaves the results empty first, so every failure below reports them as NULL. */
	if (calls != NULL)
		*calls = NULL;
	if (context != NULL)
		*context = NULL;

	/* Refuses arguments that are missing. */
	if (directory == NULL)
		return EINVAL;
	if (directory[0] == '\0')
		return EINVAL;
	if (calls == NULL)
		return EINVAL;
	if (context == NULL)
		return EINVAL;

	/* Allocates the context the table's calls work on. */
	files = (struct files_context *)calloc(1, sizeof(*files));
	if (files == NULL)
		return ENOMEM;

	/* Makes the directory and every missing directory above it. */
	error = wb_os_make_directories(directory);
	if (error != 0) {
		free(files);
		return error;
	}

	/* Takes the process's lock on the directory; another holder makes it EBUSY. */
	error = wb_os_lock_open(directory, &files->lock);
	if (error != 0) {
		free(files);
		return error;
	}

	/* Fixes the directory's absolute form for the paths of every later call. */
	error = files_build_directory(directory, &files->directory);
	if (error != 0) {
		wb_os_lock_close(files->lock);
		free(files);
		return error;
	}

	/* Hands the table and the context to the caller. */
	*calls = &files_calls;
	*context = files;

	/* Succeeded: the directory is locked and ready. */
	return 0;
}

/*
 * Makes the absolute, separator-free form of a directory that exists.
 */
static int
files_build_directory(
	const char *directory,
	char **absolute)
{
	struct wb_buffer buffer;
	char *copy;
	size_t length;
	int error;

	wb_buffer_init(&buffer);

	/* Resolves the directory against the current directory. */
	error = wb_os_full_path(directory, &buffer);
	if (error != 0) {
		wb_buffer_release(&buffer);
		return error;
	}

	/* Drops trailing '/' so a path is the directory, '/', and a name. */
	length = buffer.length;
	while (length > 0 && buffer.data[length - 1] == '/')
		length--;

	/* Copies the result to a block the context owns. */
	copy = (char *)malloc(length + 1);
	if (copy == NULL) {
		wb_buffer_release(&buffer);
		return ENOMEM;
	}
	memcpy(copy, buffer.data, length);
	copy[length] = '\0';
	wb_buffer_release(&buffer);

	/* Succeeded: the caller owns the copy. */
	*absolute = copy;
	return 0;
}

/*
 * Appends the path of a space's directory to a buffer.
 */
static int
files_space_path(
	const struct files_context *files,
	unsigned space,
	struct wb_buffer *path)
{
	int error;

	/* Starts with the backend's directory. */
	error = wb_buffer_append_string(path, files->directory);
	if (error != 0)
		return error;
	error = wb_buffer_append_byte(path, '/');
	if (error != 0)
		return error;

	/* Adds the name of the space's directory: fixed names for the three spaces, a number for others. */
	if (space == BROWSER_STORAGE_COOKIES) {
		error = wb_buffer_append_string(path, WB_STORAGE_FILES_COOKIES);
	} else if (space == BROWSER_STORAGE_CACHE) {
		error = wb_buffer_append_string(path, WB_STORAGE_FILES_CACHE);
	} else if (space == BROWSER_STORAGE_LOCAL) {
		error = wb_buffer_append_string(path, WB_STORAGE_FILES_LOCAL);
	} else {
		error = wb_buffer_printf(path, "%s%u", WB_STORAGE_FILES_OTHER, space);
	}
	if (error != 0)
		return error;

	/* Succeeded: the path names the space's directory. */
	return 0;
}

/*
 * Appends the path of a record's file to a buffer.
 */
static int
files_record_path(
	const struct files_context *files,
	unsigned space,
	const char *key,
	struct wb_buffer *path)
{
	int error;

	/* Starts with the space's directory. */
	error = files_space_path(files, space, path);
	if (error != 0)
		return error;

	/* Adds the key as the file name. */
	error = wb_buffer_append_byte(path, '/');
	if (error != 0)
		return error;
	error = wb_buffer_append_string(path, key);
	if (error != 0)
		return error;

	/* A localStorage record's name carries the suffix of the files of earlier engines. */
	if (space == BROWSER_STORAGE_LOCAL) {
		error = wb_buffer_append_string(path, WB_STORAGE_FILES_LOCAL_SUFFIX);
		if (error != 0)
			return error;
	}

	/* Succeeded: the path names the record's file. */
	return 0;
}

/*
 * Reads a record whole and appends it to the sink in pieces.
 */
static int
files_read(
	void *context,
	unsigned space,
	const char *key,
	struct browser_storage_sink *sink,
	int *found)
{
	const struct files_context *files;
	struct wb_buffer path;
	struct wb_buffer bytes;
	size_t offset;
	size_t piece;
	int valid;
	int error;

	/* Refuses a place to report to that is missing. */
	if (found == NULL)
		return EINVAL;

	/* Starts from "not found", which is also what every early exit reports. */
	*found = 0;

	/* Refuses arguments that are missing. */
	if (context == NULL)
		return EINVAL;
	if (key == NULL)
		return EINVAL;
	if (sink == NULL)
		return EINVAL;

	/* Refuses a key that could name something outside the space's directory. */
	valid = wb_storage_key_valid(key);
	if (!valid)
		return EINVAL;

	files = (const struct files_context *)context;
	wb_buffer_init(&path);
	wb_buffer_init(&bytes);

	/* Builds the record's path. */
	error = files_record_path(files, space, key, &path);
	if (error != 0) {
		wb_buffer_release(&path);
		return error;
	}

	/* Reads the file whole. */
	error = wb_os_file_read(wb_buffer_string(&path), &bytes);
	wb_buffer_release(&path);

	/* A missing file, or a missing space directory, is a record that is not there. */
	if (error == ENOENT || error == ENOTDIR) {
		wb_buffer_release(&bytes);
		return 0;
	}
	if (error != 0) {
		wb_buffer_release(&bytes);
		return error;
	}

	/* Hands the bytes to the sink in pieces of at most WB_STORAGE_FILES_PIECE. */
	offset = 0;
	while (offset < bytes.length) {
		piece = bytes.length - offset;
		if (piece > WB_STORAGE_FILES_PIECE)
			piece = WB_STORAGE_FILES_PIECE;

		error = browser_storage_sink_append(sink, bytes.data + offset, piece);
		if (error != 0) {
			wb_buffer_release(&bytes);
			return error;
		}

		offset += piece;
	}
	wb_buffer_release(&bytes);

	/* Succeeded: the record exists and the sink holds all of it. */
	*found = 1;
	return 0;
}

/*
 * Creates or replaces a record's file in one step.
 */
static int
files_write(
	void *context,
	unsigned space,
	const char *key,
	const void *bytes,
	size_t length)
{
	const struct files_context *files;
	struct wb_buffer path;
	int valid;
	int error;

	/* Refuses arguments that are missing. */
	if (context == NULL)
		return EINVAL;
	if (key == NULL)
		return EINVAL;
	if (bytes == NULL && length != 0)
		return EINVAL;

	/* Refuses a key that could name something outside the space's directory. */
	valid = wb_storage_key_valid(key);
	if (!valid)
		return EINVAL;

	files = (const struct files_context *)context;
	wb_buffer_init(&path);

	/* Makes the space's directory on its first write. */
	error = files_space_path(files, space, &path);
	if (error != 0) {
		wb_buffer_release(&path);
		return error;
	}
	error = wb_os_make_directories(wb_buffer_string(&path));
	if (error != 0) {
		wb_buffer_release(&path);
		return error;
	}

	/* Builds the record's path. */
	wb_buffer_clear(&path);
	error = files_record_path(files, space, key, &path);
	if (error != 0) {
		wb_buffer_release(&path);
		return error;
	}

	/* Replaces the file, so a reader or a crash sees the old bytes or the new bytes in full. */
	error = wb_os_file_replace(wb_buffer_string(&path), bytes, length);
	wb_buffer_release(&path);
	if (error != 0)
		return error;

	/* Succeeded: the record holds exactly these bytes. */
	return 0;
}

/*
 * Removes a record; a record that is not there is not an error.
 */
static int
files_remove(
	void *context,
	unsigned space,
	const char *key)
{
	int valid;
	int error;

	/* Refuses arguments that are missing. */
	if (context == NULL)
		return EINVAL;
	if (key == NULL)
		return EINVAL;

	/* Refuses a key that could name something outside the space's directory. */
	valid = wb_storage_key_valid(key);
	if (!valid)
		return EINVAL;

	/* Removes the record's file. */
	error = files_remove_record((const struct files_context *)context, space, key);
	if (error != 0)
		return error;

	/* Succeeded: the record is gone, or never was. */
	return 0;
}

/*
 * Calls each with every key of a space.
 */
static int
files_list(
	void *context,
	unsigned space,
	int (*each)(void *each_context, const char *key),
	void *each_context)
{
	int error;

	/* Refuses arguments that are missing. */
	if (context == NULL)
		return EINVAL;
	if (each == NULL)
		return EINVAL;

	/* Walks the space's directory. */
	error = files_walk((const struct files_context *)context, space, each, each_context);
	if (error != 0)
		return error;

	/* Succeeded: each was called with every key and never stopped the walk. */
	return 0;
}

/*
 * Removes every record of a space.
 *
 * The keys are gathered first, since the directory should not change while
 * it is walked; then each record is removed.  A failure is remembered and
 * the rest is still tried.
 */
static int
files_clear(
	void *context,
	unsigned space)
{
	const struct files_context *files;
	struct wb_buffer keys;
	size_t offset;
	size_t length;
	int first_error;
	int error;

	/* Refuses a context that is missing. */
	if (context == NULL)
		return EINVAL;

	files = (const struct files_context *)context;
	wb_buffer_init(&keys);

	/* Gathers the keys of the space, each followed by a NUL. */
	error = files_walk(files, space, files_collect_key, &keys);
	if (error != 0) {
		wb_buffer_release(&keys);
		return error;
	}

	/* Removes each gathered record and keeps the first failure. */
	first_error = 0;
	offset = 0;
	while (offset < keys.length) {
		length = strlen((const char *)keys.data + offset);

		error = files_remove_record(files, space, (const char *)keys.data + offset);
		if (error != 0 && first_error == 0)
			first_error = error;

		offset += length + 1;
	}
	wb_buffer_release(&keys);

	/* Reports the first record that could not be removed. */
	if (first_error != 0)
		return first_error;

	/* Succeeded: every record of the space is gone. */
	return 0;
}

/*
 * Releases the directory lock and frees the context.
 */
static void
files_close(
	void *context)
{
	struct files_context *files;

	/* A context that is not there needs nothing. */
	if (context == NULL)
		return;

	files = (struct files_context *)context;

	/* Lets another process or open use the directory. */
	wb_os_lock_close(files->lock);

	/* Frees the directory's path and the context. */
	free(files->directory);
	free(files);
}

/*
 * Calls each with every valid key found in a space's directory.
 *
 * A missing directory, or a file where the directory should be, lists
 * nothing.  The value each stops the walk with is returned as it is.
 */
static int
files_walk(
	const struct files_context *files,
	unsigned space,
	int (*each)(void *each_context, const char *key),
	void *each_context)
{
	struct files_walk walk;
	struct wb_buffer path;
	int error;

	wb_buffer_init(&path);

	/* Builds the space directory's path. */
	error = files_space_path(files, space, &path);
	if (error != 0) {
		wb_buffer_release(&path);
		return error;
	}

	/* Prepares what the walk hands each entry. */
	memset(&walk, 0, sizeof(walk));
	walk.is_local = (space == BROWSER_STORAGE_LOCAL);
	walk.each = each;
	walk.each_context = each_context;

	/* Visits every name in the directory. */
	error = wb_os_list(wb_buffer_string(&path), files_walk_entry, &walk);
	wb_buffer_release(&path);

	/* The caller's own stop value wins, whatever number it is. */
	if (walk.stopped)
		return walk.stop_value;

	/* A space that was never written has no directory. */
	if (error == ENOENT || error == ENOTDIR)
		return 0;
	if (error != 0)
		return error;

	/* Succeeded: every key was passed on. */
	return 0;
}

/*
 * Passes one directory entry on when its name is a valid key.
 */
static int
files_walk_entry(
	void *context,
	const char *name)
{
	struct files_walk *walk;
	char key[BROWSER_STORAGE_KEY_MAX + 1];
	size_t length;
	int valid;
	int error;

	walk = (struct files_walk *)context;
	length = strlen(name);

	/* A localStorage name is the key and the suffix; anything without the suffix is not a record. */
	if (walk->is_local) {
		if (length <= FILES_LOCAL_SUFFIX_LENGTH)
			return 0;
		if (strcmp(name + length - FILES_LOCAL_SUFFIX_LENGTH, WB_STORAGE_FILES_LOCAL_SUFFIX) != 0)
			return 0;
		length -= FILES_LOCAL_SUFFIX_LENGTH;
	}

	/* A name too long or too short for a key, such as a replace's temporary file, is skipped. */
	if (length == 0)
		return 0;
	if (length > BROWSER_STORAGE_KEY_MAX)
		return 0;

	/* Copies the key's text out of the name. */
	memcpy(key, name, length);
	key[length] = '\0';

	/* A name with characters outside the key alphabet is some other file. */
	valid = wb_storage_key_valid(key);
	if (!valid)
		return 0;

	/* Hands the key on; a non-zero answer stops the walk and is remembered. */
	error = walk->each(walk->each_context, key);
	if (error != 0) {
		walk->stopped = 1;
		walk->stop_value = error;
		return error;
	}

	/* Succeeded: the walk goes on to the next name. */
	return 0;
}

/*
 * Removes one record's file; a file that is not there counts as removed.
 */
static int
files_remove_record(
	const struct files_context *files,
	unsigned space,
	const char *key)
{
	struct wb_buffer path;
	int error;

	wb_buffer_init(&path);

	/* Builds the record's path. */
	error = files_record_path(files, space, key, &path);
	if (error != 0) {
		wb_buffer_release(&path);
		return error;
	}

	/* Removes the file. */
	error = wb_os_file_remove(wb_buffer_string(&path));
	wb_buffer_release(&path);

	/* A record that was not there, or whose space directory was not, is already removed. */
	if (error == ENOENT || error == ENOTDIR)
		return 0;
	if (error != 0)
		return error;

	/* Succeeded: the record is not there. */
	return 0;
}

/*
 * Appends a key and a NUL to the buffer a clear gathers them in.
 */
static int
files_collect_key(
	void *each_context,
	const char *key)
{
	struct wb_buffer *keys;
	int error;

	keys = (struct wb_buffer *)each_context;

	/* Adds the key with its terminating NUL, which separates it from the next. */
	error = wb_buffer_append(keys, key, strlen(key) + 1);
	if (error != 0)
		return error;

	/* Succeeded: the key is gathered. */
	return 0;
}
