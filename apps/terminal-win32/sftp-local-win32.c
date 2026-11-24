/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Local filesystem layer for SFTP transfers on Windows.
 *
 * Paths are UTF-8 outside and converted to UTF-16 for Win32 APIs, with
 * the \\?\ prefix added automatically when an absolute drive path is
 * long so transfers work even when LongPathsEnabled is off.  Reparse
 * points (symlinks and junctions) are never followed during uploads or
 * written through during downloads.
 */

#include "apps/mikuterm-common/sftp-local.h"

#include <windows.h>
#include <shlobj.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Offset between Windows FILETIME (1601-01-01) and Unix epoch (1970-01-01) in 100-ns units. */
#define WIN32_UNIX_EPOCH_100NS	116444736000000000ULL
#define WIN32_100NS_PER_SEC	10000000ULL

/* Open local file handle backed by a Win32 HANDLE. */
struct sftp_local_file {
	HANDLE handle;
};

static int local_wide(const char *path, wchar_t **wide);
static int local_utf8(const wchar_t *wide, char *out, size_t out_cap);
static int local_errno(DWORD error);
static uint32_t filetime_to_unix(FILETIME ft);
static FILETIME unix_to_filetime(uint32_t sec);
static int check_no_reparse_chain(const char *dir_path);

/*
 * Finds the user's Downloads folder in UTF-8.
 */
int
sftp_local_downloads_dir(
	char *path,
	size_t size)
{
	PWSTR wide;
	HRESULT hr;
	int failed;
	int error;

	/* Queries the Known Folder path for Downloads. */
	hr = SHGetKnownFolderPath(&FOLDERID_Downloads, 0, NULL, &wide);
	failed = FAILED(hr);
	if (failed || wide == NULL)
		return EIO;

	/* Converts the UTF-16 folder path to UTF-8 and frees the shell buffer. */
	error = local_utf8(wide, path, size);
	CoTaskMemFree(wide);
	if (error != 0)
		return error;

	/* Succeeded: path holds the Downloads folder in UTF-8. */
	return 0;
}

/*
 * Queries metadata for path without following reparse points.
 */
int
sftp_local_stat(
	const char *path,
	struct sftp_local_entry *entry)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	wchar_t *wide;
	const char *base;
	const char *slash;
	BOOL ok;
	DWORD last_err;
	int error;

	/* Converts the path to UTF-16 and clears the output entry. */
	memset(entry, 0, sizeof(*entry));
	error = local_wide(path, &wide);
	if (error != 0)
		return error;

	/* Queries Win32 file attributes without traversing reparse targets. */
	ok = GetFileAttributesExW(wide, GetFileExInfoStandard, &data);
	if (!ok) {
		last_err = GetLastError();
		free(wide);
		error = local_errno(last_err);
		return error;
	}

	/* Frees the temporary wide path buffer. */
	free(wide);

	/* Extracts the final path component for entry->name. */
	base = path;
	slash = strrchr(path, '\\');
	if (slash != NULL)
		base = slash + 1;
	slash = strrchr(base, '/');
	if (slash != NULL)
		base = slash + 1;
	(void)snprintf(entry->name, sizeof(entry->name), "%s", base);

	/* Populates directory, reparse, size, and modification time fields. */
	entry->is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
	entry->is_reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
	entry->size = ((uint64_t)data.nFileSizeHigh << 32) | (uint64_t)data.nFileSizeLow;
	entry->mtime = filetime_to_unix(data.ftLastWriteTime);
	return 0;
}

/*
 * Lists entries of dir_path, skipping "." and "..".
 */
int
sftp_local_list(
	const char *dir_path,
	struct sftp_local_entry **entries_out,
	size_t *count_out)
{
	char pattern[MIKUTERM_PATH_MAX];
	wchar_t *wide;
	WIN32_FIND_DATAW fdata;
	HANDLE find;
	struct sftp_local_entry *list;
	size_t count;
	size_t cap;
	struct sftp_local_entry *grown;
	int cmp_dot;
	int cmp_dotdot;
	BOOL more;
	DWORD last_err;
	int error;

	/* Builds the wildcard search pattern and converts it to UTF-16. */
	*entries_out = NULL;
	*count_out = 0U;
	error = sftp_local_join_path(pattern, sizeof(pattern), dir_path, "*");
	if (error != 0)
		return error;

	/* Converts the search pattern to wide characters. */
	error = local_wide(pattern, &wide);
	if (error != 0)
		return error;

	/* Opens the directory search handle. */
	find = FindFirstFileExW(wide, FindExInfoBasic, &fdata, FindExSearchNameMatch, NULL, 0);
	free(wide);
	if (find == INVALID_HANDLE_VALUE) {
		last_err = GetLastError();
		error = local_errno(last_err);
		if (error == ENOENT)
			return 0;
		return error;
	}

	/* Collects every non-dot entry into a dynamically grown array. */
	list = NULL;
	count = 0U;
	cap = 0U;
	for (;;) {
		cmp_dot = wcscmp(fdata.cFileName, L".");
		cmp_dotdot = wcscmp(fdata.cFileName, L"..");
		if (cmp_dot != 0 && cmp_dotdot != 0) {
			if (count == cap) {
				if (cap == 0U)
					cap = 32U;
				else
					cap = cap * 2U;
				grown = realloc(list, cap * sizeof(*list));
				if (grown == NULL) {
					FindClose(find);
					free(list);
					return ENOMEM;
				}

				/* Commits the grown entry array. */
				list = grown;
			}

			/* Converts the entry name and copies its metadata. */
			memset(&list[count], 0, sizeof(list[count]));
			error = local_utf8(fdata.cFileName, list[count].name, sizeof(list[count].name));
			if (error == 0) {
				list[count].is_dir = (fdata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
				list[count].is_reparse = (fdata.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
				list[count].size = ((uint64_t)fdata.nFileSizeHigh << 32) | (uint64_t)fdata.nFileSizeLow;
				list[count].mtime = filetime_to_unix(fdata.ftLastWriteTime);
				count++;
			}
		}

		/* Advances to the next directory entry. */
		more = FindNextFileW(find, &fdata);
		if (!more)
			break;
	}

	/* Closes the search handle and returns the collected entries. */
	FindClose(find);
	*entries_out = list;
	*count_out = count;
	return 0;
}

/*
 * Frees a directory listing returned by sftp_local_list.
 */
void
sftp_local_list_free(
	struct sftp_local_entry *entries,
	size_t count)
{
	/* Frees the entry array. */
	(void)count;
	free(entries);
}

/*
 * Opens a local file for reading, refusing reparse points.
 */
int
sftp_local_open_read(
	const char *path,
	struct sftp_local_file **file_out,
	uint64_t *size,
	uint32_t *mtime)
{
	struct sftp_local_entry entry;
	struct sftp_local_file *file;
	wchar_t *wide;
	HANDLE handle;
	DWORD last_err;
	int error;

	/* Clears outputs and inspects the path without following reparse points. */
	*file_out = NULL;
	*size = 0U;
	*mtime = 0U;
	error = sftp_local_stat(path, &entry);
	if (error != 0)
		return error;
	if (entry.is_reparse)
		return ELOOP;
	if (entry.is_dir)
		return EISDIR;

	/* Converts the path to UTF-16 and opens the file for reading. */
	error = local_wide(path, &wide);
	if (error != 0)
		return error;

	/* Opens the existing Win32 file handle. */
	handle = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	free(wide);
	if (handle == INVALID_HANDLE_VALUE) {
		last_err = GetLastError();
		error = local_errno(last_err);
		return error;
	}

	/* Allocates the file wrapper and returns handle, size, and mtime. */
	file = malloc(sizeof(*file));
	if (file == NULL) {
		CloseHandle(handle);
		return ENOMEM;
	}

	/* Succeeded: publishes the open file handle and metadata. */
	file->handle = handle;
	*file_out = file;
	*size = entry.size;
	*mtime = entry.mtime;
	return 0;
}

/*
 * Reads up to size bytes at offset from an open local file.
 */
int
sftp_local_read_at(
	struct sftp_local_file *file,
	uint64_t offset,
	void *buf,
	size_t size,
	size_t *got)
{
	OVERLAPPED ov;
	DWORD nread;
	DWORD err;
	BOOL ok;
	int mapped;

	/* Sets up the overlapped offset and reads from the file handle. */
	*got = 0U;
	memset(&ov, 0, sizeof(ov));
	ov.Offset = (DWORD)(offset & 0xffffffffU);
	ov.OffsetHigh = (DWORD)(offset >> 32);
	ok = ReadFile(file->handle, buf, (DWORD)size, &nread, &ov);
	if (!ok) {
		err = GetLastError();
		if (err == ERROR_HANDLE_EOF) {
			*got = 0U;
			return 0;
		}

		/* Maps Win32 read error to errno. */
		mapped = local_errno(err);
		return mapped;
	}

	/* Succeeded: reports the number of bytes read. */
	*got = (size_t)nread;
	return 0;
}

/*
 * Verifies that no directory component of dir_path is a reparse point,
 * then opens a hidden temporary file in dir_path for writing.
 */
int
sftp_local_open_temp_write(
	const char *dir_path,
	const char *final_name,
	char *temp_path,
	size_t temp_cap,
	struct sftp_local_file **file_out)
{
	char temp_name[MIKUTERM_PATH_MAX];
	struct sftp_local_file *file;
	wchar_t *wide;
	HANDLE handle;
	DWORD last_err;
	int written;
	int error;

	/* Verifies that no ancestor directory is a reparse point. */
	*file_out = NULL;
	error = check_no_reparse_chain(dir_path);
	if (error != 0)
		return error;

	/* Builds the hidden temporary filename and joins it with dir_path. */
	written = snprintf(temp_name, sizeof(temp_name), ".%s.mikuterm-part", final_name);
	if (written < 0 || (size_t)written >= sizeof(temp_name))
		return ENAMETOOLONG;

	/* Joins dir_path and the temporary filename. */
	error = sftp_local_join_path(temp_path, temp_cap, dir_path, temp_name);
	if (error != 0)
		return error;

	/* Converts the temporary path to UTF-16 and creates the hidden file. */
	error = local_wide(temp_path, &wide);
	if (error != 0)
		return error;

	/* Creates the hidden temporary file exclusively for writing. */
	handle = CreateFileW(
		wide,
		GENERIC_WRITE,
		0,
		NULL,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NORMAL,
		NULL);
	free(wide);
	if (handle == INVALID_HANDLE_VALUE) {
		last_err = GetLastError();
		error = local_errno(last_err);
		return error;
	}

	/* Allocates the file wrapper. */
	file = malloc(sizeof(*file));
	if (file == NULL) {
		CloseHandle(handle);
		return ENOMEM;
	}

	/* Succeeded: publishes the open temporary file handle. */
	file->handle = handle;
	*file_out = file;
	return 0;
}

/*
 * Writes size bytes at offset to an open local file.
 */
int
sftp_local_write_at(
	struct sftp_local_file *file,
	uint64_t offset,
	const void *buf,
	size_t size)
{
	OVERLAPPED ov;
	DWORD wrote;
	DWORD last_err;
	const uint8_t *ptr;
	size_t left;
	BOOL ok;
	int error;

	/* Loops until all bytes are written at their respective offsets. */
	ptr = buf;
	left = size;
	while (left != 0U) {
		memset(&ov, 0, sizeof(ov));
		ov.Offset = (DWORD)(offset & 0xffffffffU);
		ov.OffsetHigh = (DWORD)(offset >> 32);
		ok = WriteFile(file->handle, ptr, (DWORD)left, &wrote, &ov);
		if (!ok) {
			last_err = GetLastError();
			error = local_errno(last_err);
			return error;
		}

		/* Fails if WriteFile reported zero bytes written without an error. */
		if (wrote == 0U)
			return EIO;
		ptr += wrote;
		offset += wrote;
		left -= wrote;
	}

	/* Succeeded: all bytes were written. */
	return 0;
}

/*
 * Sets the modification time on an open local file.
 */
int
sftp_local_set_mtime(
	struct sftp_local_file *file,
	uint32_t mtime)
{
	FILETIME ft;
	BOOL ok;
	DWORD last_err;
	int error;

	/* Converts the Unix timestamp to FILETIME and updates the file's write time. */
	ft = unix_to_filetime(mtime);
	ok = SetFileTime(file->handle, NULL, NULL, &ft);
	if (!ok) {
		last_err = GetLastError();
		error = local_errno(last_err);
		return error;
	}

	/* Succeeded: modification time updated. */
	return 0;
}

/*
 * Closes a local file handle.
 */
void
sftp_local_close(
	struct sftp_local_file *file)
{
	/* Ignores a NULL file handle. */
	if (file == NULL)
		return;

	/* Closes the Win32 handle and frees the wrapper. */
	CloseHandle(file->handle);
	free(file);
}

/*
 * Clears the hidden attribute on temp_path and atomically replaces
 * final_path.
 */
int
sftp_local_commit_temp(
	const char *temp_path,
	const char *final_path)
{
	wchar_t *temp_w;
	wchar_t *final_w;
	DWORD attrs;
	DWORD last_err;
	BOOL ok;
	int error;

	/* Converts both temporary and final paths to UTF-16. */
	error = local_wide(temp_path, &temp_w);
	if (error != 0)
		return error;
	error = local_wide(final_path, &final_w);
	if (error != 0) {
		free(temp_w);
		return error;
	}

	/* Clears the hidden attribute before moving into place. */
	attrs = GetFileAttributesW(temp_w);
	if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_HIDDEN) != 0U)
		(void)SetFileAttributesW(temp_w, attrs & ~FILE_ATTRIBUTE_HIDDEN);

	/* Atomically replaces final_path with temp_path. */
	ok = MoveFileExW(temp_w, final_w, MOVEFILE_REPLACE_EXISTING);
	if (!ok) {
		last_err = GetLastError();
		error = local_errno(last_err);
		free(temp_w);
		free(final_w);
		return error;
	}

	/* Succeeded: frees path buffers. */
	free(temp_w);
	free(final_w);
	return 0;
}

/*
 * Removes a local file.
 */
int
sftp_local_remove(
	const char *path)
{
	wchar_t *wide;
	BOOL ok;
	DWORD last_err;
	int error;

	/* Converts the path to UTF-16 and deletes the file. */
	error = local_wide(path, &wide);
	if (error != 0)
		return error;

	/* Deletes the Win32 file. */
	ok = DeleteFileW(wide);
	if (!ok) {
		last_err = GetLastError();
		error = local_errno(last_err);
		free(wide);
		return error;
	}

	/* Succeeded: file deleted. */
	free(wide);
	return 0;
}

/*
 * Creates a local directory, succeeding if it already exists as a
 * non-reparse directory.
 */
int
sftp_local_mkdir(
	const char *path)
{
	wchar_t *wide;
	struct sftp_local_entry entry;
	BOOL created;
	DWORD last_err;
	int error;

	/* Converts the path to UTF-16 and attempts directory creation. */
	error = local_wide(path, &wide);
	if (error != 0)
		return error;

	/* Creates the directory on disk. */
	created = CreateDirectoryW(wide, NULL);
	if (created) {
		free(wide);
		return 0;
	}

	/* Maps the failure and accepts an existing non-reparse directory. */
	last_err = GetLastError();
	error = local_errno(last_err);
	free(wide);
	if (error == EEXIST) {
		error = sftp_local_stat(path, &entry);
		if (error == 0 && entry.is_dir && !entry.is_reparse)
			return 0;
		return EEXIST;
	}

	/* Reports the directory creation failure. */
	return error;
}

/*
 * Joins dir and name with a backslash separator.
 */
int
sftp_local_join_path(
	char *out,
	size_t out_cap,
	const char *dir,
	const char *name)
{
	size_t len;
	int written;

	/* Formats the joined path without duplicating a trailing separator. */
	len = strlen(dir);
	if (len != 0U && (dir[len - 1U] == '\\' || dir[len - 1U] == '/'))
		written = snprintf(out, out_cap, "%s%s", dir, name);
	else
		written = snprintf(out, out_cap, "%s\\%s", dir, name);

	/* Reports ENAMETOOLONG if the joined path was truncated. */
	if (written < 0 || (size_t)written >= out_cap)
		return ENAMETOOLONG;

	/* Succeeded: out holds the joined path. */
	return 0;
}

/* Converts UTF-8 path to UTF-16, adding \\?\ for long drive paths. */
static int
local_wide(
	const char *path,
	wchar_t **wide_out)
{
	int units;
	int converted;
	wchar_t *wide;
	size_t i;
	size_t wlen;

	/* Measures the required UTF-16 buffer length. */
	*wide_out = NULL;
	units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
	if (units <= 0)
		return EINVAL;

	/* Allocates extra room for an optional \\?\ long-path prefix. */
	wide = malloc(((size_t)units + 4U) * sizeof(*wide));
	if (wide == NULL)
		return ENOMEM;

	/* Converts the UTF-8 string to UTF-16. */
	converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, units);
	if (converted != units) {
		free(wide);
		return EINVAL;
	}

	/* Normalizes forward slashes to backslashes. */
	for (i = 0U; wide[i] != L'\0'; i++) {
		if (wide[i] == L'/')
			wide[i] = L'\\';
	}

	/* Prepends \\?\ for long absolute drive paths so LongPathsEnabled=0 still works. */
	wlen = wcslen(wide);
	if (wlen >= 248U &&
	    ((wide[0] >= L'A' && wide[0] <= L'Z') || (wide[0] >= L'a' && wide[0] <= L'z')) &&
	    wide[1] == L':' && wide[2] == L'\\') {
		memmove(wide + 4, wide, ((size_t)units) * sizeof(*wide));
		wide[0] = L'\\';
		wide[1] = L'\\';
		wide[2] = L'?';
		wide[3] = L'\\';
	}

	/* Succeeded: returns the wide string. */
	*wide_out = wide;
	return 0;
}

/* Converts UTF-16 string to UTF-8. */
static int
local_utf8(
	const wchar_t *wide,
	char *out,
	size_t out_cap)
{
	int n;

	/* Encodes the UTF-16 string as UTF-8. */
	n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)out_cap, NULL, NULL);
	if (n <= 0)
		return ENAMETOOLONG;

	/* Succeeded: out holds the UTF-8 string. */
	return 0;
}

/* Maps common Win32 errors to errno values. */
static int
local_errno(
	DWORD error)
{
	/* Translates Win32 GetLastError codes used by SFTP local I/O. */
	switch (error) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
		return ENOENT;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
		return EACCES;
	case ERROR_ALREADY_EXISTS:
	case ERROR_FILE_EXISTS:
		return EEXIST;
	case ERROR_DISK_FULL:
		return ENOSPC;
	case ERROR_FILENAME_EXCED_RANGE:
		return ENAMETOOLONG;
	default:
		return EIO;
	}
}

/* Converts a Windows FILETIME to 32-bit Unix seconds, clamping out-of-range values to 0. */
static uint32_t
filetime_to_unix(
	FILETIME ft)
{
	uint64_t v;
	uint64_t sec;

	/* Converts 100-ns intervals since 1601 to Unix seconds. */
	v = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
	if (v < WIN32_UNIX_EPOCH_100NS)
		return 0U;
	sec = (v - WIN32_UNIX_EPOCH_100NS) / WIN32_100NS_PER_SEC;
	if (sec > 0xffffffffULL)
		return 0U;

	/* Returns the clamped 32-bit timestamp. */
	return (uint32_t)sec;
}

/* Converts 32-bit Unix seconds to a Windows FILETIME. */
static FILETIME
unix_to_filetime(
	uint32_t sec)
{
	FILETIME ft;
	uint64_t v;

	/* Shifts Unix seconds to the Windows FILETIME epoch. */
	v = (uint64_t)sec * WIN32_100NS_PER_SEC + WIN32_UNIX_EPOCH_100NS;
	ft.dwLowDateTime = (DWORD)(v & 0xffffffffU);
	ft.dwHighDateTime = (DWORD)(v >> 32);
	return ft;
}

/* Checks that dir_path and every existing parent segment is not a reparse point. */
static int
check_no_reparse_chain(
	const char *dir_path)
{
	char copy[MIKUTERM_PATH_MAX];
	char saved;
	size_t len;
	size_t i;
	wchar_t *wide;
	DWORD attrs;
	DWORD last_err;
	int written;
	int error;

	/* Copies dir_path so each prefix can be NUL-terminated in place. */
	written = snprintf(copy, sizeof(copy), "%s", dir_path);
	if (written < 0 || written >= (int)sizeof(copy))
		return ENAMETOOLONG;

	/* Inspects every directory prefix for FILE_ATTRIBUTE_REPARSE_POINT. */
	len = strlen(copy);
	for (i = 0U; i <= len; i++) {
		if (i < len && copy[i] != '\\' && copy[i] != '/')
			continue;
		if (i == 0U || (i == 2U && copy[1] == ':'))
			continue;
		saved = copy[i];
		copy[i] = '\0';
		error = local_wide(copy, &wide);
		if (error != 0)
			return error;

		/* Queries the attributes of this directory prefix. */
		attrs = GetFileAttributesW(wide);
		free(wide);
		if (attrs == INVALID_FILE_ATTRIBUTES) {
			last_err = GetLastError();
			error = local_errno(last_err);
			return error;
		}

		/* Refuses any reparse point (junction or symlink) along the chain. */
		if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
			return ELOOP;
		copy[i] = saved;
	}

	/* Succeeded: no directory segment is a reparse point. */
	return 0;
}
