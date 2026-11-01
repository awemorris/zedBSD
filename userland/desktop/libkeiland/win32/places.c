/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The places of an application on Windows: its storage directory is
 * <the user's local application data>\<application>
 * (SHGetKnownFolderPath with FOLDERID_LocalAppData), and the bundled data
 * is the "share" directory beside the program (GetModuleFileNameW).  The
 * paths are UTF-8 with the system's backslashes; nothing is created.
 */

#include "userland/desktop/libkeiland/win32/window-win32.h"

#include <knownfolders.h>
#include <shlobj.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The longest path a module's name is asked for in (the system's long paths). */
#define PLACES_PATH_MAX	32768U

/* The bundled data's directory, beside the program. */
#define PLACES_DATA	"\\share"

static int places_utf8(const WCHAR *path, size_t count, char *buffer, size_t size);

/*
 * Writes the directory where the application keeps its data:
 * %LOCALAPPDATA%\<application>.
 */
int
kl_app_storage_directory(
	const struct kl_app *app,
	char *buffer,
	size_t size)
{
	PWSTR folder;
	HRESULT result;
	size_t length;
	size_t name;
	int error;

	/* Somewhere to write, and an application whose directory it is. */
	if (app == NULL || buffer == NULL || size == 0U)
		return EINVAL;
	if (app->application == NULL || app->application[0] == '\0')
		return EINVAL;
	buffer[0] = '\0';

	/* The user's local application data. */
	folder = NULL;
	result = SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0U, NULL, &folder);
	if (FAILED(result) || folder == NULL) {
		CoTaskMemFree(folder);
		return ENOENT;
	}
	error = places_utf8(folder, wcslen(folder), buffer, size);
	CoTaskMemFree(folder);
	if (error != 0)
		return error;

	/* The application's directory in it. */
	length = strlen(buffer);
	name = strlen(app->application);
	if (length + 1U + name + 1U > size) {
		buffer[0] = '\0';
		return ERANGE;
	}
	buffer[length] = '\\';
	memcpy(buffer + length + 1U, app->application, name + 1U);

	/* Succeeded. */
	return 0;
}

/*
 * Writes the directory of the bundled read-only data: the "share"
 * directory beside the program.
 */
int
kl_data_directory(
	char *buffer,
	size_t size)
{
	WCHAR *module;
	DWORD got;
	size_t cut;
	size_t index;
	size_t length;
	int error;

	/* Somewhere to write. */
	if (buffer == NULL || size == 0U)
		return EINVAL;
	buffer[0] = '\0';

	/* The program's own path. */
	module = calloc(PLACES_PATH_MAX, sizeof(*module));
	if (module == NULL)
		return ENOMEM;
	got = GetModuleFileNameW(NULL, module, PLACES_PATH_MAX);
	if (got == 0U || got >= PLACES_PATH_MAX) {
		free(module);
		return ENOENT;
	}

	/* Its directory (up to its last backslash). */
	cut = 0U;
	for (index = 0U; index < (size_t)got; index++) {
		if (module[index] == L'\\' || module[index] == L'/')
			cut = index;
	}
	error = places_utf8(module, cut, buffer, size);
	free(module);
	if (error != 0)
		return error;

	/* The data's directory in it. */
	length = strlen(buffer);
	if (length + sizeof(PLACES_DATA) > size) {
		buffer[0] = '\0';
		return ERANGE;
	}
	memcpy(buffer + length, PLACES_DATA, sizeof(PLACES_DATA));

	/* Succeeded. */
	return 0;
}

/* A path of the system's as UTF-8; 0, or ERANGE (buffer then empty). */
static int
places_utf8(
	const WCHAR *path,
	size_t count,
	char *buffer,
	size_t size)
{
	size_t consumed;
	int error;

	/* The conversion, whole. */
	error = keiui_win32_utf8_from_utf16((const uint16_t *)path, count, buffer, size, &consumed);
	if (error != 0 || consumed != count) {
		buffer[0] = '\0';
		return ERANGE;
	}

	/* Succeeded. */
	return 0;
}
