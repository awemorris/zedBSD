/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Windows file loading, atomic replacement, CR/LF handling, and recovery for vi.
 */

#include "userland/base/vi/vi.h"

#include <windows.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_TEMPORARY_NAME_LIMIT 200U
#define FILE_WRITE_BLOCK 65536U
#define FILE_WIDE_MAX 32768U

/*
 * Count of lines and bytes written to disk.
 */
struct file_totals {
	size_t lines;
	unsigned long bytes;
};

static int utf8_to_wide(const char *utf8, wchar_t **wide);
static int win32_errno(DWORD error);
static int detect_crlf(const char *contents, size_t length);
static int split_lines(struct vi_buffer *buffer, const char *contents, size_t length, int crlf);
static int read_whole(HANDLE handle, size_t size, char **contents, size_t *length);
static int write_all(HANDLE handle, const char *bytes, size_t length);
static int write_lines(HANDLE handle, const struct vi_buffer *buffer, int crlf, struct file_totals *totals);
static int write_replacing(const wchar_t *target, int exists, const struct vi_buffer *buffer, int crlf, struct file_totals *totals);
static int write_in_place(const wchar_t *target, const struct vi_buffer *buffer, int crlf, struct file_totals *totals);

/*
 * Loads a UTF-8 named file into editor->buffer, auto-detecting CR+LF line endings.
 */
int
vi_file_load(
	struct vi_editor *editor,
	const char *path)
{
	struct vi_buffer buffer;
	wchar_t *wide;
	HANDLE handle;
	DWORD file_type;
	DWORD last;
	DWORD attrs;
	LARGE_INTEGER file_size;
	char *contents;
	size_t length;
	int exists;
	int incomplete;
	int crlf;
	int error;
	BOOL ok;

	/* Initializes a temporary buffer so a failed load leaves the editor intact. */
	vi_buffer_init(&buffer);
	contents = NULL;
	length = 0U;
	incomplete = 0;
	crlf = 0;
	exists = 0;

	/* Converts the UTF-8 path to UTF-16. */
	error = utf8_to_wide(path, &wide);
	if (error != 0) {
		(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": %s", path, strerror(error));
		return error;
	}

	/* Opens the existing file, or treats a missing file as a new empty buffer. */
	handle = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
	    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		last = GetLastError();
		free(wide);
		if (last != ERROR_FILE_NOT_FOUND && last != ERROR_PATH_NOT_FOUND) {
			error = win32_errno(last);
			(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": %s", path, strerror(error));
			return error;
		}
	} else {
		/* Verifies the target is a regular disk file within the size limit. */
		attrs = GetFileAttributesW(wide);
		free(wide);
		exists = 1;
		if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
			CloseHandle(handle);
			(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": not a regular file", path);
			return EINVAL;
		}

		/* Rejects non-disk handles such as pipes or devices. */
		file_type = GetFileType(handle);
		if (file_type != FILE_TYPE_DISK) {
			CloseHandle(handle);
			(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": not a regular file", path);
			return EINVAL;
		}

		/* Queries and validates the file size against the editor limit. */
		ok = GetFileSizeEx(handle, &file_size);
		if (!ok) {
			error = win32_errno(GetLastError());
			CloseHandle(handle);
			(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": %s", path, strerror(error));
			return error;
		}

		/* Enforces the upper bound before reading into memory. */
		if (file_size.QuadPart < 0 || (unsigned long long)file_size.QuadPart > (unsigned long long)VI_FILE_SIZE_LIMIT) {
			CloseHandle(handle);
			(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": file too large", path);
			return EFBIG;
		}

		/* Reads the file bytes and detects CR+LF line endings. */
		error = read_whole(handle, (size_t)file_size.QuadPart, &contents, &length);
		CloseHandle(handle);
		if (error != 0) {
			if (error == EFBIG)
				(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": file too large", path);
			else
				(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": %s", path, strerror(error));
			return error;
		}

		/* Records incomplete trailing lines and detects CR+LF endings. */
		if (length != 0U && contents[length - 1U] != '\n')
			incomplete = 1;
		crlf = detect_crlf(contents, length);
	}

	/* Splits the loaded bytes into lines in the temporary buffer. */
	error = split_lines(&buffer, contents, length, crlf);
	free(contents);
	if (error != 0) {
		vi_buffer_free(&buffer);
		if (error == EFBIG)
			(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": file too large", path);
		else
			(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": %s", path, strerror(error));
		return error;
	}

	/* Commits the new buffer and resets undo and cursor state. */
	vi_buffer_free(&editor->buffer);
	editor->buffer = buffer;
	vi_undo_free(&editor->undo);
	vi_undo_init(&editor->undo);
	editor->crlf = crlf;
	editor->cursor.line = 0U;
	editor->cursor.offset = 0U;
	editor->want_column = 0U;
	editor->top = 0U;
	editor->top_skip = 0U;
	vi_screen_invalidate(editor);

	/* Reports the loaded file summary in the status line. */
	if (!exists) {
		(void)snprintf(editor->message, sizeof(editor->message), "\"%s\" [New file]", path);
	} else if (incomplete) {
		(void)snprintf(editor->message, sizeof(editor->message),
		    "\"%s\" %lu lines, %lu bytes [Incomplete last line]",
		    path, (unsigned long)editor->buffer.count, (unsigned long)length);
	} else {
		(void)snprintf(editor->message, sizeof(editor->message),
		    "\"%s\" %lu lines, %lu bytes",
		    path, (unsigned long)editor->buffer.count, (unsigned long)length);
	}

	/* Succeeded. */
	return 0;
}

/*
 * Writes editor->buffer to path using ReplaceFileW / MoveFileExW to preserve ACLs.
 */
int
vi_file_write(
	struct vi_editor *editor,
	const char *path,
	int force)
{
	struct file_totals totals;
	wchar_t *wide;
	DWORD attrs;
	DWORD last;
	int is_own_file;
	int exists;
	int same_name;
	char *name_copy;
	int error;

	/* Uses the buffer's own name when no explicit path is given. */
	if (path == NULL)
		path = editor->file_name;
	if (path == NULL) {
		vi_message(editor, "No file name");
		return EINVAL;
	}

	/* Checks whether the target is the buffer's own file. */
	is_own_file = 0;
	if (editor->file_name != NULL) {
		same_name = strcmp(path, editor->file_name);
		if (same_name == 0)
			is_own_file = 1;
	}

	/* Converts the target path to UTF-16. */
	error = utf8_to_wide(path, &wide);
	if (error != 0) {
		(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": %s", path, strerror(error));
		return error;
	}

	/* Checks whether the target exists and is a writable regular file. */
	attrs = GetFileAttributesW(wide);
	exists = 0;
	if (attrs != INVALID_FILE_ATTRIBUTES) {
		exists = 1;
		if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
			free(wide);
			(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": not a regular file", path);
			return EINVAL;
		}
	} else {
		last = GetLastError();
		if (last != ERROR_FILE_NOT_FOUND && last != ERROR_PATH_NOT_FOUND) {
			error = win32_errno(last);
			free(wide);
			(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": %s", path, strerror(error));
			return error;
		}
	}

	/* Refuses to clobber another file or a read-only file without force. */
	if (exists && !is_own_file && !force) {
		free(wide);
		(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": file exists (add ! to override)", path);
		return EEXIST;
	}

	/* Blocks overwriting read-only files unless forced. */
	if (exists && !force && (attrs & FILE_ATTRIBUTE_READONLY) != 0U) {
		free(wide);
		(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": file is read-only (add ! to override)", path);
		return EACCES;
	}

	/* Writes atomically via a temporary file, falling back to in-place on denial. */
	memset(&totals, 0, sizeof(totals));
	error = write_replacing(wide, exists, &editor->buffer, editor->crlf, &totals);
	if (error != 0 && exists)
		error = write_in_place(wide, &editor->buffer, editor->crlf, &totals);
	free(wide);
	if (error != 0) {
		(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": %s", path, strerror(error));
		return error;
	}

	/* Adopts the name if the buffer was unnamed and marks it saved. */
	if (editor->file_name == NULL) {
		name_copy = strdup(path);
		if (name_copy != NULL) {
			editor->file_name = name_copy;
			is_own_file = 1;
		}
	}

	/* Clears dirty state when saving the buffer's own file. */
	if (is_own_file)
		vi_undo_mark_saved(editor);

	/* Reports the write summary. */
	(void)snprintf(editor->message, sizeof(editor->message),
	    "\"%s\" %lu lines, %lu bytes written",
	    path, (unsigned long)totals.lines, totals.bytes);
	return 0;
}

/*
 * Writes unwritten changes to a recovery file on abnormal exit.
 */
void
vi_file_recover(
	struct vi_editor *editor)
{
	wchar_t dir[MAX_PATH];
	wchar_t recover[MAX_PATH + 128];
	DWORD dir_len;
	HANDLE handle;
	struct file_totals totals;

	/* Resolves the Windows temporary directory. */
	dir_len = GetTempPathW((DWORD)(sizeof(dir) / sizeof(dir[0])), dir);
	if (dir_len == 0U || dir_len >= (DWORD)(sizeof(dir) / sizeof(dir[0])))
		return;

	/* Creates the recovery file and flushes the buffer contents. */
	(void)swprintf(recover, sizeof(recover) / sizeof(recover[0]),
	    L"%svi-recover.%lu.txt", dir, (unsigned long)GetCurrentProcessId());
	handle = CreateFileW(recover, GENERIC_WRITE, 0U, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE)
		return;

	/* Writes all lines and closes the recovery file. */
	memset(&totals, 0, sizeof(totals));
	(void)write_lines(handle, &editor->buffer, editor->crlf, &totals);
	(void)FlushFileBuffers(handle);
	CloseHandle(handle);
}

/*
 * Converts a UTF-8 path to a newly allocated UTF-16 string.
 */
static int
utf8_to_wide(
	const char *utf8,
	wchar_t **wide)
{
	int units;
	int converted;
	wchar_t *buffer;

	/* Rejects empty paths. */
	if (utf8 == NULL || utf8[0] == '\0')
		return EINVAL;

	/* Measures the UTF-16 length. */
	units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
	if (units <= 0 || (unsigned)units > FILE_WIDE_MAX)
		return EINVAL;

	/* Allocates and converts the UTF-16 path. */
	buffer = malloc((size_t)units * sizeof(wchar_t));
	if (buffer == NULL)
		return ENOMEM;
	converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, buffer, units);
	if (converted <= 0) {
		free(buffer);
		return EINVAL;
	}

	/* Succeeded. */
	*wide = buffer;
	return 0;
}

/*
 * Maps common Win32 error codes to errno values.
 */
static int
win32_errno(
	DWORD error)
{
	/* Translates filesystem error codes. */
	switch (error) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
		return ENOENT;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
		return EACCES;
	case ERROR_FILE_EXISTS:
	case ERROR_ALREADY_EXISTS:
		return EEXIST;
	case ERROR_DISK_FULL:
		return ENOSPC;
	case ERROR_FILENAME_EXCED_RANGE:
		return ENAMETOOLONG;
	default:
		return EIO;
	}
}

/*
 * Returns 1 when every newline in contents is preceded by CR (and at least one CRLF exists).
 */
static int
detect_crlf(
	const char *contents,
	size_t length)
{
	size_t index;
	int saw_lf;

	/* Empty files default to LF. */
	if (contents == NULL || length == 0U)
		return 0;

	/* Checks that every LF is immediately preceded by CR. */
	saw_lf = 0;
	for (index = 0U; index < length; index++) {
		if (contents[index] == '\n') {
			if (index == 0U || contents[index - 1U] != '\r')
				return 0;
			saw_lf = 1;
		}
	}

	/* True only when at least one CRLF line ending was seen. */
	return saw_lf;
}

/*
 * Splits loaded file bytes into lines, stripping trailing CR when crlf is set.
 */
static int
split_lines(
	struct vi_buffer *buffer,
	const char *contents,
	size_t length,
	int crlf)
{
	size_t start;
	size_t index;
	size_t line_length;
	int error;

	/* An empty file starts with one empty line. */
	if (length == 0U)
		return vi_buffer_append_line(buffer, "", 0U);

	/* Appends each newline-terminated line. */
	start = 0U;
	for (index = 0U; index < length; index++) {
		if (contents[index] == '\n') {
			line_length = index - start;
			if (crlf && line_length > 0U && contents[start + line_length - 1U] == '\r')
				line_length--;
			error = vi_buffer_append_line(buffer, contents + start, line_length);
			if (error != 0)
				return error;
			start = index + 1U;
		}
	}

	/* Appends a trailing incomplete line if present. */
	if (start < length) {
		line_length = length - start;
		if (crlf && line_length > 0U && contents[start + line_length - 1U] == '\r')
			line_length--;
		error = vi_buffer_append_line(buffer, contents + start, line_length);
		if (error != 0)
			return error;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Reads the entire file into a newly allocated buffer.
 */
static int
read_whole(
	HANDLE handle,
	size_t size,
	char **contents,
	size_t *length)
{
	char *buffer;
	size_t total;
	size_t remaining;
	DWORD want;
	DWORD got;
	BOOL ok;
	int error;

	/* Zero-byte files need no allocation. */
	if (size == 0U) {
		*contents = NULL;
		*length = 0U;
		return 0;
	}

	/* Allocates the file buffer. */
	buffer = malloc(size);
	if (buffer == NULL)
		return ENOMEM;

	/* Reads in blocks until size bytes or EOF. */
	total = 0U;
	while (total < size) {
		remaining = size - total;
		want = (DWORD)remaining;
		if (remaining > FILE_WRITE_BLOCK)
			want = FILE_WRITE_BLOCK;
		got = 0U;
		ok = ReadFile(handle, buffer + total, want, &got, NULL);
		if (!ok) {
			error = win32_errno(GetLastError());
			free(buffer);
			return error;
		}

		/* Stops reading at EOF. */
		if (got == 0U)
			break;
		total += (size_t)got;
	}

	/* Hands ownership of buffer to the caller. */
	*contents = buffer;
	*length = total;
	return 0;
}

/*
 * Writes length bytes to handle.
 */
static int
write_all(
	HANDLE handle,
	const char *bytes,
	size_t length)
{
	size_t written;
	size_t remaining;
	DWORD want;
	DWORD got;
	BOOL ok;

	/* Loops until all bytes are written or WriteFile fails. */
	written = 0U;
	while (written < length) {
		remaining = length - written;
		want = (DWORD)remaining;
		if (remaining > FILE_WRITE_BLOCK)
			want = FILE_WRITE_BLOCK;
		got = 0U;
		ok = WriteFile(handle, bytes + written, want, &got, NULL);
		if (!ok || got == 0U)
			return win32_errno(GetLastError());
		written += (size_t)got;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Streams all lines of buffer to handle, using CRLF or LF endings.
 */
static int
write_lines(
	HANDLE handle,
	const struct vi_buffer *buffer,
	int crlf,
	struct file_totals *totals)
{
	char *block;
	size_t used;
	size_t index;
	const struct vi_line *line;
	const char *ending;
	size_t ending_len;
	int error;

	/* Allocates a staging block to batch small lines. */
	block = malloc(FILE_WRITE_BLOCK);
	if (block == NULL)
		return ENOMEM;

	/* Selects the line ending sequence. */
	ending = "\n";
	ending_len = 1U;
	if (crlf) {
		ending = "\r\n";
		ending_len = 2U;
	}

	/* Resets staging counters before writing lines. */
	used = 0U;
	totals->lines = 0U;
	totals->bytes = 0UL;

	/* Writes each line followed by its ending. */
	for (index = 0U; index < buffer->count; index++) {
		line = &buffer->lines[index];
		if (line->length + ending_len > FILE_WRITE_BLOCK) {
			if (used > 0U) {
				error = write_all(handle, block, used);
				if (error != 0) {
					free(block);
					return error;
				}

				/* Resets the staging buffer after flushing. */
				used = 0U;
			}

			/* Writes the oversized line and its ending directly. */
			error = write_all(handle, line->text, line->length);
			if (error == 0)
				error = write_all(handle, ending, ending_len);
			if (error != 0) {
				free(block);
				return error;
			}
		} else {
			if (used + line->length + ending_len > FILE_WRITE_BLOCK) {
				error = write_all(handle, block, used);
				if (error != 0) {
					free(block);
					return error;
				}

				/* Resets the staging buffer after flushing. */
				used = 0U;
			}

			/* Copies the line text into the staging buffer when non-empty. */
			if (line->length > 0U) {
				memcpy(block + used, line->text, line->length);
				used += line->length;
			}

			/* Appends the line ending to the staging buffer. */
			memcpy(block + used, ending, ending_len);
			used += ending_len;
		}

		/* Updates the written line and byte counters. */
		totals->lines++;
		totals->bytes += (unsigned long)(line->length + ending_len);
	}

	/* Flushes any remaining staged bytes. */
	if (used > 0U) {
		error = write_all(handle, block, used);
		if (error != 0) {
			free(block);
			return error;
		}
	}

	/* Succeeded. */
	free(block);
	return 0;
}

/*
 * Writes to a sibling temporary file and atomically replaces target.
 */
static int
write_replacing(
	const wchar_t *target,
	int exists,
	const struct vi_buffer *buffer,
	int crlf,
	struct file_totals *totals)
{
	size_t target_len;
	wchar_t *temp_path;
	HANDLE handle;
	BOOL ok;
	int error;

	/* Builds a sibling temporary filename. */
	target_len = wcslen(target);
	temp_path = malloc((target_len + 32U) * sizeof(wchar_t));
	if (temp_path == NULL)
		return ENOMEM;
	(void)swprintf(temp_path, target_len + 32U, L"%ls.vi%lu.tmp",
	    target, (unsigned long)GetCurrentProcessId());

	/* Creates the temporary file and writes all lines. */
	handle = CreateFileW(temp_path, GENERIC_WRITE, 0U, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		error = win32_errno(GetLastError());
		free(temp_path);
		return error;
	}

	/* Writes all lines and flushes buffers before closing the temp file. */
	error = write_lines(handle, buffer, crlf, totals);
	if (error == 0) {
		ok = FlushFileBuffers(handle);
		if (!ok)
			error = win32_errno(GetLastError());
	}

	/* Closes the temp file handle before replacing or deleting. */
	CloseHandle(handle);
	if (error != 0) {
		(void)DeleteFileW(temp_path);
		free(temp_path);
		return error;
	}

	/* Atomically replaces target, preserving ACLs when target already exists. */
	if (exists) {
		ok = ReplaceFileW(target, temp_path, NULL, REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL);
		if (!ok) {
			ok = MoveFileExW(temp_path, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
			if (!ok) {
				error = win32_errno(GetLastError());
				(void)DeleteFileW(temp_path);
				free(temp_path);
				return error;
			}
		}
	} else {
		ok = MoveFileExW(temp_path, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
		if (!ok) {
			error = win32_errno(GetLastError());
			(void)DeleteFileW(temp_path);
			free(temp_path);
			return error;
		}
	}

	/* Succeeded. */
	free(temp_path);
	return 0;
}

/*
 * Truncates and rewrites target in place when temporary creation fails.
 */
static int
write_in_place(
	const wchar_t *target,
	const struct vi_buffer *buffer,
	int crlf,
	struct file_totals *totals)
{
	HANDLE handle;
	BOOL ok;
	int error;

	/* Opens the existing file and truncates it to zero bytes. */
	handle = CreateFileW(target, GENERIC_WRITE, 0U, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE)
		return win32_errno(GetLastError());
	ok = SetEndOfFile(handle);
	if (!ok) {
		error = win32_errno(GetLastError());
		CloseHandle(handle);
		return error;
	}

	/* Writes the buffer and flushes to disk. */
	error = write_lines(handle, buffer, crlf, totals);
	if (error == 0) {
		ok = FlushFileBuffers(handle);
		if (!ok)
			error = win32_errno(GetLastError());
	}

	/* Closes the target handle and returns the write result. */
	CloseHandle(handle);
	return error;
}
