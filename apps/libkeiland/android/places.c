/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The places of the program on Android: the folder it keeps its own data in
 * (the app's private files), and the folder of the read-only data that comes
 * with it (fonts and licenses).
 *
 * The apk holds the read-only data as assets, which a program cannot open as
 * files; android_places_unpack writes them into the private files folder, and
 * again only when the apk's version of them changed.  The apk lists what it
 * holds in assets/share.list (one name a line) and says which version it is in
 * assets/share.version.
 */

#include "apps/libkeiland/android/android.h"

#include <android/asset_manager.h>
#include <android/native_activity.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The longest path the places make, with its NUL. */
#define PLACES_PATH_MAX		512U

/* The longest version text, with its NUL. */
#define PLACES_VERSION_MAX	128U

/* The size of the pieces an asset is copied in. */
#define PLACES_PIECE		65536U

/* The private files folder, kept from the first Activity (the folder does not change). */
static char places_base[PLACES_PATH_MAX];

static const char *places_files(void);
static int places_text(const char *path, char *buffer, size_t size);
static int places_make_directories(char *path);
static int places_copy(AAssetManager *assets, const char *name);
static int places_join(char *buffer, size_t size, const char *base, const char *middle, const char *name);

/*
 * Writes the directory where the application keeps its data:
 * <files>/<application>, made when it is not there.
 */
int
kl_app_storage_directory(
	const struct kl_app *app,
	char *buffer,
	size_t size)
{
	const char *files;
	int error;
	int status;

	/* The places for the answer. */
	if (app == NULL || buffer == NULL || size == 0U || app->application[0] == '\0')
		return EINVAL;
	buffer[0] = '\0';

	/* The private folder. */
	files = places_files();
	if (files == NULL)
		return ENOENT;
	error = places_join(buffer, size, files, NULL, app->application);
	if (error != 0)
		return error;

	/* Made when it is not there. */
	status = mkdir(buffer, 0700);
	if (status != 0 && errno != EEXIST) {
		error = errno;
		buffer[0] = '\0';
		return error;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Writes the directory of the read-only data: <files>/share.
 */
int
kl_data_directory(
	char *buffer,
	size_t size)
{
	const char *files;
	int error;

	/* The places for the answer. */
	if (buffer == NULL || size == 0U)
		return EINVAL;
	buffer[0] = '\0';

	/* The private folder. */
	files = places_files();
	if (files == NULL)
		return ENOENT;
	error = places_join(buffer, size, files, NULL, "share");
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Writes the assets of the apk (assets/share/...) into <files>/share when
 * the version in the apk is not the one written there.  Returns 0, ENOENT
 * (the apk holds none), or the error of a file.
 */
int
android_places_unpack(
	ANativeActivity *activity)
{
	char path[PLACES_PATH_MAX];
	char version[PLACES_VERSION_MAX];
	char written[PLACES_VERSION_MAX];
	char *list;
	char *line;
	char *next;
	size_t line_length;
	AAsset *asset;
	off_t length;
	FILE *file;
	const char *files;
	int got;
	int status;
	int same;
	int error;

	/* An Activity with its assets. */
	if (activity == NULL || activity->assetManager == NULL || activity->internalDataPath == NULL)
		return EINVAL;
	files = places_files();
	if (files == NULL)
		return ENOENT;

	/* The version the apk holds, and the one written. */
	asset = AAssetManager_open(activity->assetManager, "share.version", AASSET_MODE_BUFFER);
	if (asset == NULL)
		return ENOENT;
	length = AAsset_getLength(asset);
	if (length <= 0 || (size_t)length >= sizeof(version)) {
		AAsset_close(asset);
		return EINVAL;
	}

	got = AAsset_read(asset, version, (size_t)length);
	if (got != length) {
		AAsset_close(asset);
		return EIO;
	}

	version[length] = '\0';
	AAsset_close(asset);
	error = places_join(path, sizeof(path), places_base, "share", ".version");
	if (error != 0)
		return error;
	error = places_text(path, written, sizeof(written));
	if (error == 0) {
		same = strcmp(written, version);
		if (same == 0)
			return 0;
	}

	/* The list of what the apk holds. */
	asset = AAssetManager_open(activity->assetManager, "share.list", AASSET_MODE_BUFFER);
	if (asset == NULL)
		return ENOENT;
	length = AAsset_getLength(asset);
	list = malloc((size_t)length + 1U);
	if (list == NULL) {
		AAsset_close(asset);
		return ENOMEM;
	}

	got = AAsset_read(asset, list, (size_t)length);
	if (got != length) {
		free(list);
		AAsset_close(asset);
		return EIO;
	}

	list[length] = '\0';
	AAsset_close(asset);

	/* Each name, written. */
	error = 0;
	for (line = list; *line != '\0' && error == 0; line = next) {
		next = strchr(line, '\n');
		if (next != NULL)
			*next++ = '\0';
		else
			next = line + strlen(line);
		line_length = strlen(line);
		while (line_length > 0U && line[line_length - 1U] == '\r')
			line[--line_length] = '\0';
		if (line[0] == '\0')
			continue;
		error = places_copy(activity->assetManager, line);
	}

	free(list);
	if (error != 0)
		return error;

	/* The version last, so that a copy that stopped half way is made again. */
	file = fopen(path, "w");
	if (file == NULL)
		return errno;
	status = fputs(version, file);
	if (status == EOF) {
		(void)fclose(file);
		return EIO;
	}

	status = fclose(file);
	if (status != 0)
		return EIO;

	/* Succeeded: the data is as the apk's. */
	return 0;
}

/* Returns the private files folder (kept after the first look), or NULL when there is none. */
static const char *
places_files(void)
{
	ANativeActivity *activity;

	/* Kept already. */
	if (places_base[0] != '\0')
		return places_base;

	/* From the Activity. */
	activity = keiland_android_activity();
	if (activity == NULL || activity->internalDataPath == NULL)
		return NULL;
	if (strlen(activity->internalDataPath) >= sizeof(places_base))
		return NULL;
	(void)snprintf(places_base, sizeof(places_base), "%s", activity->internalDataPath);
	return places_base;
}

/* Reads a small text file into a buffer, ended by NUL.  Returns 0, or an error number. */
static int
places_text(
	const char *path,
	char *buffer,
	size_t size)
{
	int descriptor;
	ssize_t got;

	/* The file. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return errno;
	got = read(descriptor, buffer, size - 1U);
	(void)close(descriptor);
	if (got < 0)
		return EIO;
	buffer[got] = '\0';

	/* Succeeded. */
	return 0;
}

/* Makes the folders of a path up to its last slash (the path is changed while it is made, and put back). */
static int
places_make_directories(
	char *path)
{
	char *slash;
	int status;

	/* Each folder after the first character. */
	for (slash = strchr(path + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
		*slash = '\0';
		status = mkdir(path, 0700);
		if (status != 0 && errno != EEXIST) {
			*slash = '/';
			return errno;
		}

		*slash = '/';
	}

	/* Succeeded. */
	return 0;
}

/* Copies assets/share/<name> to <files>/share/<name>.  Returns 0, or an error number. */
static int
places_copy(
	AAssetManager *assets,
	const char *name)
{
	char from[PLACES_PATH_MAX];
	char to[PLACES_PATH_MAX];
	char *piece;
	AAsset *asset;
	FILE *file;
	int got;
	size_t wrote;
	int closed;
	int error;

	/* Where it is and where it goes. */
	error = places_join(from, sizeof(from), "share", NULL, name);
	if (error != 0)
		return error;
	error = places_join(to, sizeof(to), places_base, "share", name);
	if (error != 0)
		return error;
	error = places_make_directories(to);
	if (error != 0)
		return error;

	/* The asset and the file. */
	asset = AAssetManager_open(assets, from, AASSET_MODE_STREAMING);
	if (asset == NULL)
		return ENOENT;
	file = fopen(to, "wb");
	if (file == NULL) {
		error = errno;
		AAsset_close(asset);
		return error;
	}

	piece = malloc(PLACES_PIECE);
	if (piece == NULL) {
		(void)fclose(file);
		AAsset_close(asset);
		return ENOMEM;
	}

	/* Each piece. */
	error = 0;
	for (;;) {
		got = AAsset_read(asset, piece, PLACES_PIECE);
		if (got < 0) {
			error = EIO;
			break;
		}

		if (got == 0)
			break;
		wrote = fwrite(piece, 1U, (size_t)got, file);
		if (wrote != (size_t)got) {
			error = EIO;
			break;
		}
	}

	free(piece);
	AAsset_close(asset);
	closed = fclose(file);
	if (closed != 0 && error == 0)
		error = EIO;

	/* Reports any read or write failure. */
	if (error != 0)
		return error;

	/* Succeeded: the asset is copied. */
	return 0;
}

/* Joins base, middle (NULL for none) and name with slashes into a buffer.  Returns 0, or ERANGE. */
static int
places_join(
	char *buffer,
	size_t size,
	const char *base,
	const char *middle,
	const char *name)
{
	int written;

	/* The path. */
	if (middle != NULL)
		written = snprintf(buffer, size, "%s/%s/%s", base, middle, name);
	else
		written = snprintf(buffer, size, "%s/%s", base, name);
	if (written < 0 || (size_t)written >= size) {
		buffer[0] = '\0';
		return ERANGE;
	}

	/* Succeeded. */
	return 0;
}
