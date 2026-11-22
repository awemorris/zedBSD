/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The folder mikuterm runs from: the directory mikuterm.exe is in, found
 * from the program's own path (never the current directory, the registry
 * or the user's profile), and the files mikuterm reads and writes in it.
 * Paths are UTF-8 for the rest of the program and UTF-16 for Windows.
 */

#include "mikuterm.h"

#include <windows.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The most UTF-16 units of the program's path Windows is asked for (its longest path). */
#define FOLDER_WIDE_MAX		32768U

/*
 * The program's path and its folder in UTF-8, with backslashes, found
 * once at the start (mikuterm_folder_init) and read only afterwards, from
 * any thread.  Empty until then.
 */
static char folder_program[MIKUTERM_PATH_MAX];
static char folder_path[MIKUTERM_PATH_MAX];

static int folder_wide(const char *path, wchar_t **wide);
static int folder_error(DWORD error);
static int folder_write(const wchar_t *path, DWORD access, DWORD disposition, const void *bytes, size_t size);
static int folder_absolute(const char *path);

/*
 * Finds the folder mikuterm.exe is in.  Returns 0, ENAMETOOLONG for a path
 * longer than mikuterm keeps, or EIO.
 */
int
mikuterm_folder_init(void)
{
	wchar_t *wide;
	DWORD length;
	int converted;
	char *slash;

	/* Room for the longest path Windows has. */
	wide = malloc(FOLDER_WIDE_MAX * sizeof(*wide));
	if (wide == NULL)
		return ENOMEM;

	/* The program's own path; a path cut short is refused. */
	length = GetModuleFileNameW(NULL, wide, FOLDER_WIDE_MAX);
	if (length == 0U || length >= FOLDER_WIDE_MAX) {
		free(wide);
		return EIO;
	}

	/* In UTF-8, with its NUL. */
	converted = WideCharToMultiByte(CP_UTF8, 0, wide, (int)length + 1, folder_program, (int)sizeof(folder_program), NULL, NULL);
	free(wide);
	if (converted <= 0)
		return ENAMETOOLONG;

	/* The folder is the path up to its last backslash. */
	memcpy(folder_path, folder_program, sizeof(folder_path));
	slash = strrchr(folder_path, '\\');
	if (slash == NULL)
		return EIO;
	*slash = '\0';

	/* Succeeded: the folder is known. */
	return 0;
}

/* Returns the folder mikuterm.exe is in (UTF-8, no backslash at its end). */
const char *
mikuterm_folder(void)
{
	/* Reports the folder found at the start. */
	return folder_path;
}

/* Returns the program's own path (UTF-8), for starting another window of it. */
const char *
mikuterm_program(void)
{
	/* Reports the path found at the start. */
	return folder_program;
}

/*
 * Writes the path of a file inside the folder ("config/mikuterm.conf";
 * slashes become backslashes).  Returns 0 or ENAMETOOLONG.
 */
int
mikuterm_path(
	char *path,
	size_t size,
	const char *inside)
{
	size_t folder_length;
	size_t inside_length;
	size_t index;

	/* The folder, a backslash and the name, with the NUL. */
	folder_length = strlen(folder_path);
	inside_length = strlen(inside);
	if (folder_length + 1U + inside_length + 1U > size)
		return ENAMETOOLONG;
	memcpy(path, folder_path, folder_length);
	path[folder_length] = '\\';
	memcpy(path + folder_length + 1U, inside, inside_length + 1U);

	/* Windows' separators in the part given. */
	for (index = folder_length + 1U; path[index] != '\0'; index++) {
		/* A slash is a backslash. */
		if (path[index] == '/')
			path[index] = '\\';
	}

	/* Succeeded: the path. */
	return 0;
}

/*
 * Writes the path of a file named in ssh\config (IdentityFile,
 * UserKnownHostsFile): ~/.ssh/X is the folder's ssh\X and ~/X the
 * folder's X, so a configuration written for OpenSSH keeps its meaning; a
 * name without a directory, or relative, is in ssh\; a Windows path
 * (C:\..., \\server\...) is taken as it is.  A path from the root of a
 * POSIX system (/etc/ssh/ssh_known_hosts) names nothing on Windows: ENOENT.
 * Returns 0, ENOENT or ENAMETOOLONG.
 */
int
mikuterm_ssh_path(
	char *path,
	size_t size,
	const char *written)
{
	char inside[MIKUTERM_PATH_MAX];
	const char *directory;
	const char *rest;
	size_t length;
	int absolute;
	int home_ssh;
	int home;
	int error;

	/* A Windows path as it is. */
	absolute = folder_absolute(written);
	if (absolute) {
		length = strlen(written);
		if (length + 1U > size)
			return ENAMETOOLONG;
		memcpy(path, written, length + 1U);
		return 0;
	}

	/* A POSIX path from the root names nothing here. */
	if (written[0] == '/')
		return ENOENT;

	/* The home's .ssh is the folder's ssh, the home is the folder, and anything else is in ssh. */
	home_ssh = strncmp(written, "~/.ssh/", 7);
	home = strncmp(written, "~/", 2);
	directory = "ssh/";
	rest = written;
	if (home_ssh == 0) {
		rest = written + 7;
	} else if (home == 0) {
		directory = "";
		rest = written + 2;
	}

	/* The name inside the folder. */
	length = strlen(directory);
	if (length + strlen(rest) + 1U > sizeof(inside))
		return ENAMETOOLONG;
	memcpy(inside, directory, length);
	memcpy(inside + length, rest, strlen(rest) + 1U);

	/* Its path. */
	error = mikuterm_path(path, size, inside);
	if (error != 0)
		return error;

	/* Succeeded: the path in the folder. */
	return 0;
}

/*
 * Reads a whole file, at most limit bytes, into memory the caller frees;
 * a NUL follows its bytes.  Returns 0, ENOENT, EACCES, EFBIG, ENOMEM or
 * EIO.
 */
int
mikuterm_file_read(
	const char *path,
	size_t limit,
	char **text,
	size_t *size)
{
	wchar_t *wide;
	HANDLE file;
	LARGE_INTEGER length;
	DWORD got;
	BOOL done;
	char *bytes;
	size_t have;
	int error;

	/* Nothing is read yet. */
	*text = NULL;
	*size = 0U;

	/* The path for Windows. */
	error = folder_wide(path, &wide);
	if (error != 0)
		return error;

	/* The file, which others may read and write meanwhile. */
	file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	free(wide);
	if (file == INVALID_HANDLE_VALUE)
		return folder_error(GetLastError());

	/* Its length, within the limit. */
	done = GetFileSizeEx(file, &length);
	if (!done) {
		CloseHandle(file);
		return EIO;
	}
	if (length.QuadPart < 0 || (unsigned long long)length.QuadPart > (unsigned long long)limit) {
		CloseHandle(file);
		return EFBIG;
	}

	/* Room for it and a NUL. */
	bytes = malloc((size_t)length.QuadPart + 1U);
	if (bytes == NULL) {
		CloseHandle(file);
		return ENOMEM;
	}

	/* Its bytes, until the end or the length. */
	have = 0U;
	while (have < (size_t)length.QuadPart) {
		done = ReadFile(file, bytes + have, (DWORD)((size_t)length.QuadPart - have), &got, NULL);
		if (!done) {
			CloseHandle(file);
			free(bytes);
			return EIO;
		}
		if (got == 0U)
			break;
		have += got;
	}
	CloseHandle(file);

	/* Succeeded: the text, NUL-terminated. */
	bytes[have] = '\0';
	*text = bytes;
	*size = have;
	return 0;
}

/*
 * Appends bytes to a file in one write, making the file when it is not
 * there: another program appending to it at the same time adds its own
 * lines whole, before or after.  Returns 0, EACCES, ENOENT, ENOMEM or EIO.
 */
int
mikuterm_file_append(
	const char *path,
	const void *bytes,
	size_t size)
{
	wchar_t *wide;
	int error;

	/* The path for Windows. */
	error = folder_wide(path, &wide);
	if (error != 0)
		return error;

	/* The bytes at the end. */
	error = folder_write(wide, FILE_APPEND_DATA, OPEN_ALWAYS, bytes, size);
	free(wide);
	if (error != 0)
		return error;

	/* Succeeded: the bytes are in the file. */
	return 0;
}

/*
 * Replaces a file with the bytes: they are written to a new file beside
 * it that then takes its name, so a reader finds the old file or the new
 * one whole.  Returns 0, EACCES, ENOENT, ENOMEM or EIO.
 */
int
mikuterm_file_replace(
	const char *path,
	const void *bytes,
	size_t size)
{
	char fresh[MIKUTERM_PATH_MAX];
	wchar_t *wide;
	wchar_t *fresh_wide;
	BOOL moved;
	int error;

	/* The new file's name: the path and ".new". */
	if (strlen(path) + 5U > sizeof(fresh))
		return ENAMETOOLONG;
	memcpy(fresh, path, strlen(path));
	memcpy(fresh + strlen(path), ".new", 5);

	/* Both paths for Windows. */
	error = folder_wide(path, &wide);
	if (error != 0)
		return error;
	error = folder_wide(fresh, &fresh_wide);
	if (error != 0) {
		free(wide);
		return error;
	}

	/* The bytes in the new file. */
	error = folder_write(fresh_wide, GENERIC_WRITE, CREATE_ALWAYS, bytes, size);
	if (error != 0) {
		free(fresh_wide);
		free(wide);
		return error;
	}

	/* The new file takes the old one's name. */
	moved = MoveFileExW(fresh_wide, wide, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	free(fresh_wide);
	free(wide);
	if (!moved)
		return folder_error(GetLastError());

	/* Succeeded: the file has the bytes. */
	return 0;
}

/* Tells whether a file or a directory is at a path (1) or not (0). */
int
mikuterm_file_exists(
	const char *path)
{
	wchar_t *wide;
	DWORD attributes;
	int error;

	/* The path for Windows; one that cannot be one names nothing. */
	error = folder_wide(path, &wide);
	if (error != 0)
		return 0;

	/* Its attributes, which only something that is there has. */
	attributes = GetFileAttributesW(wide);
	free(wide);
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return 0;

	/* Succeeded: something is there. */
	return 1;
}

/* Converts a UTF-8 path to UTF-16 in memory the caller frees; returns 0, EINVAL or ENOMEM. */
static int
folder_wide(
	const char *path,
	wchar_t **wide)
{
	int units;
	int converted;

	/* How many UTF-16 units it takes, with the NUL; a path that is not UTF-8 is refused. */
	*wide = NULL;
	units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
	if (units <= 0)
		return EINVAL;

	/* The room. */
	*wide = malloc((size_t)units * sizeof(**wide));
	if (*wide == NULL)
		return ENOMEM;

	/* The conversion. */
	converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, *wide, units);
	if (converted != units) {
		free(*wide);
		*wide = NULL;
		return EINVAL;
	}

	/* Succeeded: the path in UTF-16. */
	return 0;
}

/* Turns a Windows error of opening or moving a file into an errno value. */
static int
folder_error(
	DWORD error)
{
	/* What the error means for the caller. */
	switch (error) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
		return ENOENT;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_WRITE_PROTECT:
		return EACCES;
	default:
		return EIO;
	}
}

/* Opens a file with an access and a disposition and writes the bytes in one call; returns 0 or an errno value. */
static int
folder_write(
	const wchar_t *path,
	DWORD access,
	DWORD disposition,
	const void *bytes,
	size_t size)
{
	HANDLE file;
	DWORD written;
	BOOL done;

	/* The file; others may still read it. */
	file = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return folder_error(GetLastError());

	/* The bytes in one write, all of them, on the disk before the file closes. */
	done = WriteFile(file, bytes, (DWORD)size, &written, NULL);
	if (!done || written != (DWORD)size) {
		CloseHandle(file);
		return EIO;
	}
	done = FlushFileBuffers(file);
	CloseHandle(file);
	if (!done)
		return EIO;

	/* Succeeded: the bytes are written. */
	return 0;
}

/* Tells whether a path is a Windows path from a drive or a server (1) or not (0). */
static int
folder_absolute(
	const char *path)
{
	/* A drive's letter, its colon and a separator. */
	if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
	    path[1] == ':' &&
	    (path[2] == '\\' || path[2] == '/'))
		return 1;

	/* A server's share. */
	if (path[0] == '\\' && path[1] == '\\')
		return 1;

	/* Anything else is not. */
	return 0;
}
