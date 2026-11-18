/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * An application's own settings, which libkeiland resolves itself (WS135
 * D3 (a), plan/ws135/design.md section 2.3): the keys <app>.<name> are the
 * lines <name>=<value> of ~/.config/keiland/<app>.conf.
 *
 * The file is read when the settings open, and a change rewrites it with
 * that line replaced (or taken out, for a key back at its default) and
 * every other line kept, into a new file beside it that is flushed to the
 * disk and renamed over the old one: an application starting at the same
 * time never reads half a file.  Another process of the application reads
 * the file when it starts; it is not told of the change.
 */

#include "settings-private.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The largest file read or written, in bytes. */
#define APP_FILE_MAX		16384U

/* The files' folder under the home. */
#define APP_FOLDER		".config/keiland"

static void app_parse_line(const struct settings_app *app, struct settings_cache *cache, const char *line, size_t length);
static int app_read(const char *path, char *text, size_t capacity, size_t *length);
static int app_compose(const char *old_text, size_t old_length, const char *name, const char *value, char *text, size_t capacity, size_t *length);
static int app_append(char *text, size_t capacity, size_t *length, const char *part, size_t part_length);
static int app_append_line(char *text, size_t capacity, size_t *length, const char *name, size_t name_length, const char *value);
static int app_replace(const char *path, const char *text, size_t length);
static void app_mkdir(const char *folder);

/*
 * Names an application's file in a home.  Returns 0, EINVAL for a name
 * that is not well formed or too long, ENOENT without a home, or
 * ENAMETOOLONG.
 */
int
settings_app_open(
	struct settings_app *app,
	const char *name,
	const char *home)
{
	size_t length;
	int written;
	int valid;

	/* Starts unused, so that a failure leaves no file in use. */
	memset(app, 0, sizeof(*app));

	/* A well-formed name. */
	valid = kl_settings_name_valid(name);
	if (!valid)
		return EINVAL;

	/* One that fits. */
	length = strlen(name);
	if (length >= sizeof(app->name))
		return EINVAL;

	/* A home. */
	if (home == NULL || home[0] != '/')
		return ENOENT;

	/* Names the folder under the home. */
	written = snprintf(app->folder, sizeof(app->folder), "%s/%s", home, APP_FOLDER);
	if (written < 0 || (size_t)written >= sizeof(app->folder))
		return ENAMETOOLONG;

	/* Names the file in the folder. */
	written = snprintf(app->path, sizeof(app->path), "%s/%s.conf", app->folder, name);
	if (written < 0 || (size_t)written >= sizeof(app->path))
		return ENAMETOOLONG;

	/* Keeps the name; used tells the load and the writes that the file is the application's. */
	memcpy(app->name, name, length + 1U);
	app->used = 1;

	/* Succeeded: the file is the application's. */
	return 0;
}

/*
 * Puts the application's keys into the cache: each at its default, then
 * the file's values over them.
 */
void
settings_app_load(
	const struct settings_app *app,
	struct settings_cache *cache)
{
	const struct kl_settings_key *key;
	char fallback[16];
	char *text;
	size_t prefix_length;
	size_t length;
	size_t count;
	size_t index;
	size_t start;
	size_t end;
	int differs;
	int error;

	/* No application, no keys. */
	if (!app->used)
		return;

	/* Each key of the application starts at its default. */
	prefix_length = strlen(app->name);
	count = kl_settings_key_count();
	for (index = 0; index < count; index++) {
		/* Only an application's key. */
		key = kl_settings_key_at(index);
		if (key->resolver != KL_SETTINGS_RESOLVER_APP)
			continue;

		/* A prefix row's keys have no default of their own. */
		if ((key->flags & KL_SETTINGS_KEY_PREFIX) != 0U)
			continue;

		/* Only this application's: its name, then a '.'. */
		differs = strncmp(key->name, app->name, prefix_length);
		if (differs != 0 || key->name[prefix_length] != '.')
			continue;

		/* Puts the table's default into effect. */
		(void)snprintf(fallback, sizeof(fallback), "%d", key->fallback);
		settings_cache_set(cache, key->name, fallback, KL_SETTINGS_DEFAULT, 1U);
	}

	/* Allocates room for the file's text; without it the defaults stay. */
	text = malloc(APP_FILE_MAX);
	if (text == NULL)
		return;

	/* Reads the text; a missing or unreadable file leaves the defaults. */
	error = app_read(app->path, text, APP_FILE_MAX, &length);
	if (error != 0) {
		free(text);
		return;
	}

	/* Each line, the last one with or without its newline. */
	start = 0;
	while (start < length) {
		/* Finds the line's end. */
		end = start;
		while (end < length && text[end] != '\n')
			end++;

		/* Takes the line's key, if it is a line of one. */
		app_parse_line(app, cache, text + start, end - start);

		/* Moves past the newline to the next line. */
		start = end + 1U;
	}

	/* Lets the text go. */
	free(text);
}

/*
 * Writes one of the application's keys into its file: value, or NULL to
 * take the key out (its default).  Returns 0, EINVAL, E2BIG or an errno
 * value of the file.
 */
int
settings_app_write(
	const struct settings_app *app,
	const char *key,
	const char *value)
{
	const char *name;
	char *before;
	char *after;
	size_t prefix_length;
	size_t before_length;
	size_t after_length;
	int differs;
	int error;

	/* Only with the application's file. */
	if (!app->used)
		return ENOENT;

	/* Only the application's own key: its name, then a '.'. */
	prefix_length = strlen(app->name);
	differs = strncmp(key, app->name, prefix_length);
	if (differs != 0 || key[prefix_length] != '.')
		return EINVAL;

	/* The line's name is the key after the application's name and the '.'. */
	name = key + prefix_length + 1U;

	/* Allocates room for the file as it is. */
	before = malloc(APP_FILE_MAX);
	if (before == NULL)
		return ENOMEM;

	/* Allocates room for the file as it is to be. */
	after = malloc(APP_FILE_MAX);
	if (after == NULL) {
		free(before);
		return ENOMEM;
	}

	/* Makes the folder; one that exists is left alone. */
	app_mkdir(app->folder);

	/* Reads the file as it is. */
	error = app_read(app->path, before, APP_FILE_MAX, &before_length);
	if (error != 0)
		goto cleanup;

	/* Writes the text with the line changed. */
	error = app_compose(before, before_length, name, value, after, APP_FILE_MAX, &after_length);
	if (error != 0)
		goto cleanup;

	/* Puts the new text in place of the file. */
	error = app_replace(app->path, after, after_length);

cleanup:
	/* Lets the room go. */
	free(after);
	free(before);

	/* Reports why the file could not be changed. */
	if (error != 0)
		return error;

	/* Succeeded: the file holds the change. */
	return 0;
}

/* Takes a name=value line's key into the cache when it is one of the application's, of its type and range. */
static void
app_parse_line(
	const struct settings_app *app,
	struct settings_cache *cache,
	const char *line,
	size_t length)
{
	const struct kl_settings_key *key;
	char full[KL_SETTINGS_KEY_MAX];
	char raw[KL_SETTINGS_VALUE_MAX];
	char clean[KL_SETTINGS_VALUE_MAX];
	const char *equals;
	size_t name_length;
	size_t value_length;
	int number;
	int written;
	int error;

	/* An empty line and a comment hold no key. */
	if (length == 0U || line[0] == '#')
		return;

	/* Finds the first '='; a line without one holds no key. */
	equals = memchr(line, '=', length);
	if (equals == NULL)
		return;

	/* A name before it, and a value after it that fits. */
	name_length = (size_t)(equals - line);
	value_length = length - name_length - 1U;
	if (name_length == 0U || value_length >= sizeof(raw))
		return;

	/* Makes the key: the application's name, a '.', and the line's name, fitting. */
	written = snprintf(full, sizeof(full), "%s.%.*s", app->name, (int)name_length, line);
	if (written < 0 || (size_t)written >= sizeof(full))
		return;

	/* Copies the value out as a string. */
	memcpy(raw, equals + 1, value_length);
	raw[value_length] = '\0';

	/* A key of the table's, the application's. */
	key = kl_settings_key_find(full);
	if (key == NULL || key->resolver != KL_SETTINGS_RESOLVER_APP)
		return;

	/* An opener as it is, when it is one; a number moved into its range. */
	if (key->type == KL_SETTINGS_TYPE_OPENER) {
		/* An opener that is not well formed is passed over. */
		error = kl_settings_key_check(key, raw);
		if (error != 0)
			return;

		/* Takes the opener as it is. */
		(void)snprintf(clean, sizeof(clean), "%s", raw);
	} else {
		/* A value that is not a number is passed over. */
		error = kl_settings_key_number(key, raw, &number);
		if (error != 0)
			return;

		/* Takes the number, moved into its range. */
		(void)snprintf(clean, sizeof(clean), "%d", number);
	}

	/* Puts the value into effect. */
	settings_cache_set(cache, full, clean, 0U, 1U);
}

/* Reads a file's whole text (a missing file is empty); returns 0, E2BIG or an errno value. */
static int
app_read(
	const char *path,
	char *text,
	size_t capacity,
	size_t *length)
{
	ssize_t count;
	int descriptor;
	int error;

	/* Starts with an empty text. */
	*length = 0;

	/* Opens the file, when there is one. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0) {
		/* A missing file is an empty text. */
		if (errno == ENOENT)
			return 0;

		/* Reports why the file could not be opened. */
		return errno;
	}

	/* Reads every byte, up to the room there is. */
	error = 0;
	for (;;) {
		/* A file that fills the room is too large. */
		if (*length == capacity) {
			error = E2BIG;
			break;
		}

		/* Reads the next bytes; an interruption is tried again. */
		count = read(descriptor, text + *length, capacity - *length);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			error = errno;
			break;
		}

		/* Nothing more is the end. */
		if (count == 0)
			break;

		/* The bytes join the text. */
		*length += (size_t)count;
	}

	/* Closes the file, which is not needed any more. */
	(void)close(descriptor);

	/* Reports why the text could not be read. */
	if (error != 0)
		return error;

	/* Succeeded: the text is read. */
	return 0;
}

/* Writes a text with one name set (value) or taken out (NULL), every other line kept; returns 0 or E2BIG. */
static int
app_compose(
	const char *old_text,
	size_t old_length,
	const char *name,
	const char *value,
	char *text,
	size_t capacity,
	size_t *length)
{
	size_t name_length;
	size_t start;
	size_t end;
	int written;
	int matched;
	int differs;
	int error;

	/* Nothing written yet, and the name not yet met. */
	*length = 0;
	written = 0;
	name_length = strlen(name);

	/* Copies each line of the old text, changing the name's. */
	start = 0;
	while (start < old_length) {
		/* Finds the line's end. */
		end = start;
		while (end < old_length && old_text[end] != '\n')
			end++;

		/* Tells whether the line is the name's: the name, then a '='. */
		matched = 0;
		if (end - start > name_length) {
			differs = memcmp(old_text + start, name, name_length);
			if (differs == 0 && old_text[start + name_length] == '=')
				matched = 1;
		}

		/* The name's line: its first becomes the new value (or goes), the others go. */
		if (matched) {
			/* Writes the new value in place of the name's first line, unless the name leaves the file. */
			if (!written && value != NULL) {
				error = app_append_line(text, capacity, length, name, name_length, value);
				if (error != 0)
					return error;
			}

			/* written records that the name was met, so its later lines go. */
			written = 1;
		} else {
			/* Any other line stays as it was. */
			error = app_append(text, capacity, length, old_text + start, end - start);
			if (error != 0)
				return error;

			/* With its newline. */
			error = app_append(text, capacity, length, "\n", 1U);
			if (error != 0)
				return error;
		}

		/* Moves past the newline to the next line. */
		start = end + 1U;
	}

	/* A name the file did not have goes at its end, unless it leaves the file. */
	if (!written && value != NULL) {
		error = app_append_line(text, capacity, length, name, name_length, value);
		if (error != 0)
			return error;
	}

	/* Succeeded: the new text. */
	return 0;
}

/* Adds a part to a text; returns 0, or E2BIG when it does not fit. */
static int
app_append(
	char *text,
	size_t capacity,
	size_t *length,
	const char *part,
	size_t part_length)
{
	/* A part that does not fit is not added. */
	if (part_length > capacity - *length)
		return E2BIG;

	/* Adds the part at the end. */
	memcpy(text + *length, part, part_length);
	*length += part_length;

	/* Succeeded: the part is added. */
	return 0;
}

/* Adds a name=value line to a text; returns 0, or E2BIG when it does not fit. */
static int
app_append_line(
	char *text,
	size_t capacity,
	size_t *length,
	const char *name,
	size_t name_length,
	const char *value)
{
	int error;

	/* Adds the name. */
	error = app_append(text, capacity, length, name, name_length);
	if (error != 0)
		return error;

	/* Adds the '='. */
	error = app_append(text, capacity, length, "=", 1U);
	if (error != 0)
		return error;

	/* Adds the value. */
	error = app_append(text, capacity, length, value, strlen(value));
	if (error != 0)
		return error;

	/* Ends the line. */
	error = app_append(text, capacity, length, "\n", 1U);
	if (error != 0)
		return error;

	/* Succeeded: the line is added. */
	return 0;
}

/* Writes a new file beside the file, flushes it and renames it over the file; returns 0 or an errno value. */
static int
app_replace(
	const char *path,
	const char *text,
	size_t length)
{
	char temporary[SETTINGS_APP_PATH_MAX + 8U];
	ssize_t count;
	size_t done;
	int descriptor;
	int status;
	int error;

	/* A new file with a name of its own beside the file, the user's alone. */
	(void)snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
	descriptor = mkstemp(temporary);
	if (descriptor < 0)
		return errno;

	/* Writes every byte of the text. */
	error = 0;
	done = 0;
	while (done < length) {
		/* Writes what is left; an interruption is tried again. */
		count = write(descriptor, text + done, length - done);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			error = errno;
			break;
		}

		/* Counts the bytes written. */
		done += (size_t)count;
	}

	/* Flushes the text to the disk before it takes the file's name. */
	if (error == 0) {
		status = fsync(descriptor);
		if (status != 0)
			error = errno;
	}

	/* The file is closed either way; a closing that fails is a failed write. */
	status = close(descriptor);
	if (error == 0 && status != 0)
		error = EIO;

	/* A failed write leaves the file as it was. */
	if (error != 0) {
		(void)unlink(temporary);
		return error;
	}

	/* Renames the new file over the file, which replaces it at once. */
	status = rename(temporary, path);
	if (status != 0) {
		error = errno;
		(void)unlink(temporary);
		return error;
	}

	/* Succeeded: the file holds the new text. */
	return 0;
}

/* Makes the files' folder and the missing folders above it (as mkdir -p), for the user alone. */
static void
app_mkdir(
	const char *folder)
{
	char partial[SETTINGS_APP_PATH_MAX];
	size_t length;
	size_t index;

	/* A folder too long for the copy is not made. */
	length = strlen(folder);
	if (length >= sizeof(partial))
		return;

	/* Copies the folder, to cut it at each slash. */
	memcpy(partial, folder, length + 1U);

	/* Makes each prefix that ends at a slash; one that exists is left alone. */
	for (index = 1; index < length; index++) {
		/* Only a slash ends a prefix. */
		if (partial[index] != '/')
			continue;

		/* Makes the folder up to this slash. */
		partial[index] = '\0';
		(void)mkdir(partial, 0700);
		partial[index] = '/';
	}

	/* Makes the folder itself. */
	(void)mkdir(partial, 0700);
}
