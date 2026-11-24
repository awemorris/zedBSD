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

struct sftp_local_file {
	HANDLE handle;
};

static int local_wide(const char *path, wchar_t **wide);
static int local_utf8(const wchar_t *wide, char *out, size_t out_cap);
static int local_errno(DWORD error);
static uint32_t filetime_to_unix(FILETIME ft);
static FILETIME unix_to_filetime(uint32_t sec);
static int check_no_reparse_chain(const char *dir_path);

/* Finds the user's Downloads folder in UTF-8. */
int
sftp_local_downloads_dir(
	char *path,
	size_t size)
{
	PWSTR wide;
	HRESULT hr;
	int error;

	hr = SHGetKnownFolderPath(&FOLDERID_Downloads, 0, NULL, &wide);
	if (FAILED(hr) || wide == NULL)
		return EIO;

	error = local_utf8(wide, path, size);
	CoTaskMemFree(wide);
	return error;
}

/* Queries metadata for path without following reparse points. */
int
sftp_local_stat(
	const char *path,
	struct sftp_local_entry *entry)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	wchar_t *wide;
	const char *base;
	const char *slash;
	int error;

	memset(entry, 0, sizeof(*entry));
	error = local_wide(path, &wide);
	if (error != 0)
		return error;

	if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &data)) {
		error = local_errno(GetLastError());
		free(wide);
		return error;
	}
	free(wide);

	base = path;
	slash = strrchr(path, '\\');
	if (slash != NULL)
		base = slash + 1;
	slash = strrchr(base, '/');
	if (slash != NULL)
		base = slash + 1;
	(void)snprintf(entry->name, sizeof(entry->name), "%s", base);

	entry->is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
	entry->is_reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
	entry->size = ((uint64_t)data.nFileSizeHigh << 32) | (uint64_t)data.nFileSizeLow;
	entry->mtime = filetime_to_unix(data.ftLastWriteTime);
	return 0;
}

/* Lists entries of dir_path, skipping "." and "..". */
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
	int error;

	*entries_out = NULL;
	*count_out = 0U;

	error = sftp_local_join_path(pattern, sizeof(pattern), dir_path, "*");
	if (error != 0)
		return error;

	error = local_wide(pattern, &wide);
	if (error != 0)
		return error;

	find = FindFirstFileExW(wide, FindExInfoBasic, &fdata, FindExSearchNameMatch, NULL, 0);
	free(wide);
	if (find == INVALID_HANDLE_VALUE) {
		error = local_errno(GetLastError());
		if (error == ENOENT)
			return 0;
		return error;
	}

	list = NULL;
	count = 0U;
	cap = 0U;
	for (;;) {
		if (wcscmp(fdata.cFileName, L".") != 0 && wcscmp(fdata.cFileName, L"..") != 0) {
			if (count == cap) {
				cap = cap == 0U ? 32U : cap * 2U;
				grown = realloc(list, cap * sizeof(*list));
				if (grown == NULL) {
					FindClose(find);
					free(list);
					return ENOMEM;
				}
				list = grown;
			}
			memset(&list[count], 0, sizeof(list[count]));
			if (local_utf8(fdata.cFileName, list[count].name, sizeof(list[count].name)) == 0) {
				list[count].is_dir = (fdata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
				list[count].is_reparse = (fdata.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
				list[count].size = ((uint64_t)fdata.nFileSizeHigh << 32) | (uint64_t)fdata.nFileSizeLow;
				list[count].mtime = filetime_to_unix(fdata.ftLastWriteTime);
				count++;
			}
		}
		if (!FindNextFileW(find, &fdata))
			break;
	}
	FindClose(find);

	*entries_out = list;
	*count_out = count;
	return 0;
}

/* Frees a directory listing returned by sftp_local_list. */
void
sftp_local_list_free(
	struct sftp_local_entry *entries,
	size_t count)
{
	(void)count;
	free(entries);
}

/* Opens a local file for reading, refusing reparse points. */
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
	int error;

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

	error = local_wide(path, &wide);
	if (error != 0)
		return error;

	handle = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	free(wide);
	if (handle == INVALID_HANDLE_VALUE)
		return local_errno(GetLastError());

	file = malloc(sizeof(*file));
	if (file == NULL) {
		CloseHandle(handle);
		return ENOMEM;
	}
	file->handle = handle;
	*file_out = file;
	*size = entry.size;
	*mtime = entry.mtime;
	return 0;
}

/* Reads up to size bytes at offset from an open local file. */
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

	*got = 0U;
	memset(&ov, 0, sizeof(ov));
	ov.Offset = (DWORD)(offset & 0xffffffffU);
	ov.OffsetHigh = (DWORD)(offset >> 32);

	if (!ReadFile(file->handle, buf, (DWORD)size, &nread, &ov)) {
		err = GetLastError();
		if (err == ERROR_HANDLE_EOF) {
			*got = 0U;
			return 0;
		}
		return local_errno(err);
	}
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
	int written;
	int error;

	*file_out = NULL;
	error = check_no_reparse_chain(dir_path);
	if (error != 0)
		return error;

	written = snprintf(temp_name, sizeof(temp_name), ".%s.mikuterm-part", final_name);
	if (written < 0 || (size_t)written >= sizeof(temp_name))
		return ENAMETOOLONG;

	error = sftp_local_join_path(temp_path, temp_cap, dir_path, temp_name);
	if (error != 0)
		return error;

	error = local_wide(temp_path, &wide);
	if (error != 0)
		return error;

	handle = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	    FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NORMAL, NULL);
	free(wide);
	if (handle == INVALID_HANDLE_VALUE)
		return local_errno(GetLastError());

	file = malloc(sizeof(*file));
	if (file == NULL) {
		CloseHandle(handle);
		return ENOMEM;
	}
	file->handle = handle;
	*file_out = file;
	return 0;
}

/* Writes size bytes at offset to an open local file. */
int
sftp_local_write_at(
	struct sftp_local_file *file,
	uint64_t offset,
	const void *buf,
	size_t size)
{
	OVERLAPPED ov;
	DWORD wrote;
	const uint8_t *ptr;
	size_t left;

	ptr = buf;
	left = size;
	while (left != 0U) {
		memset(&ov, 0, sizeof(ov));
		ov.Offset = (DWORD)(offset & 0xffffffffU);
		ov.OffsetHigh = (DWORD)(offset >> 32);
		if (!WriteFile(file->handle, ptr, (DWORD)left, &wrote, &ov))
			return local_errno(GetLastError());
		if (wrote == 0U)
			return EIO;
		ptr += wrote;
		offset += wrote;
		left -= wrote;
	}
	return 0;
}

/* Sets the modification time on an open local file. */
int
sftp_local_set_mtime(
	struct sftp_local_file *file,
	uint32_t mtime)
{
	FILETIME ft;

	ft = unix_to_filetime(mtime);
	if (!SetFileTime(file->handle, NULL, NULL, &ft))
		return local_errno(GetLastError());
	return 0;
}

/* Closes a local file handle. */
void
sftp_local_close(
	struct sftp_local_file *file)
{
	if (file == NULL)
		return;
	CloseHandle(file->handle);
	free(file);
}

/* Clears the hidden attribute on temp_path and atomically replaces final_path. */
int
sftp_local_commit_temp(
	const char *temp_path,
	const char *final_path)
{
	wchar_t *temp_w;
	wchar_t *final_w;
	DWORD attrs;
	int error;

	error = local_wide(temp_path, &temp_w);
	if (error != 0)
		return error;
	error = local_wide(final_path, &final_w);
	if (error != 0) {
		free(temp_w);
		return error;
	}

	attrs = GetFileAttributesW(temp_w);
	if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_HIDDEN) != 0U)
		(void)SetFileAttributesW(temp_w, attrs & ~FILE_ATTRIBUTE_HIDDEN);

	if (!MoveFileExW(temp_w, final_w, MOVEFILE_REPLACE_EXISTING)) {
		error = local_errno(GetLastError());
		free(temp_w);
		free(final_w);
		return error;
	}

	free(temp_w);
	free(final_w);
	return 0;
}

/* Removes a local file. */
int
sftp_local_remove(
	const char *path)
{
	wchar_t *wide;
	int error;

	error = local_wide(path, &wide);
	if (error != 0)
		return error;

	if (!DeleteFileW(wide)) {
		error = local_errno(GetLastError());
		free(wide);
		return error;
	}
	free(wide);
	return 0;
}

/* Creates a local directory, succeeding if it already exists as a non-reparse directory. */
int
sftp_local_mkdir(
	const char *path)
{
	wchar_t *wide;
	struct sftp_local_entry entry;
	int error;

	error = local_wide(path, &wide);
	if (error != 0)
		return error;

	if (CreateDirectoryW(wide, NULL)) {
		free(wide);
		return 0;
	}
	error = local_errno(GetLastError());
	free(wide);
	if (error == EEXIST) {
		if (sftp_local_stat(path, &entry) == 0 && entry.is_dir && !entry.is_reparse)
			return 0;
	}
	return error;
}

/* Joins dir and name with a backslash separator. */
int
sftp_local_join_path(
	char *out,
	size_t out_cap,
	const char *dir,
	const char *name)
{
	size_t len;
	int written;

	len = strlen(dir);
	if (len != 0U && (dir[len - 1U] == '\\' || dir[len - 1U] == '/'))
		written = snprintf(out, out_cap, "%s%s", dir, name);
	else
		written = snprintf(out, out_cap, "%s\\%s", dir, name);

	if (written < 0 || (size_t)written >= out_cap)
		return ENAMETOOLONG;
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

	*wide_out = NULL;
	units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
	if (units <= 0)
		return EINVAL;

	wide = malloc(((size_t)units + 4U) * sizeof(*wide));
	if (wide == NULL)
		return ENOMEM;

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
	if (wcslen(wide) >= 248U &&
	    ((wide[0] >= L'A' && wide[0] <= L'Z') || (wide[0] >= L'a' && wide[0] <= L'z')) &&
	    wide[1] == L':' && wide[2] == L'\\') {
		memmove(wide + 4, wide, ((size_t)units) * sizeof(*wide));
		wide[0] = L'\\';
		wide[1] = L'\\';
		wide[2] = L'?';
		wide[3] = L'\\';
	}

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

	n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)out_cap, NULL, NULL);
	if (n <= 0)
		return ENAMETOOLONG;
	return 0;
}

/* Maps common Win32 errors to errno values. */
static int
local_errno(
	DWORD error)
{
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

	v = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
	if (v < WIN32_UNIX_EPOCH_100NS)
		return 0U;
	sec = (v - WIN32_UNIX_EPOCH_100NS) / WIN32_100NS_PER_SEC;
	if (sec > 0xffffffffULL)
		return 0U;
	return (uint32_t)sec;
}

/* Converts 32-bit Unix seconds to a Windows FILETIME. */
static FILETIME
unix_to_filetime(
	uint32_t sec)
{
	FILETIME ft;
	uint64_t v;

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
	size_t len;
	size_t i;
	wchar_t *wide;
	DWORD attrs;
	int error;

	if (snprintf(copy, sizeof(copy), "%s", dir_path) >= (int)sizeof(copy))
		return ENAMETOOLONG;

	len = strlen(copy);
	for (i = 0U; i <= len; i++) {
		char saved;

		if (i < len && copy[i] != '\\' && copy[i] != '/')
			continue;
		if (i == 0U || (i == 2U && copy[1] == ':'))
			continue;
		saved = copy[i];
		copy[i] = '\0';
		error = local_wide(copy, &wide);
		if (error != 0)
			return error;
		attrs = GetFileAttributesW(wide);
		free(wide);
		if (attrs == INVALID_FILE_ATTRIBUTES)
			return local_errno(GetLastError());
		if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
			return ELOOP;
		copy[i] = saved;
	}
	return 0;
}
