/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The files of the operating-system layer on Windows.
 */

#include "base/os-file.h"
#include "base/os-win32.h"

#include <windows.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* The smallest room a read of a file is offered, in bytes. */
#define OS_READ_MIN		4096U

/* The most bytes one ReadFile call is asked for (its count is a DWORD). */
#define OS_READ_MAX		(1U << 24)

/* The most bytes one WriteFile call is given, for the same reason. */
#define OS_WRITE_MAX		(1U << 24)

/* The room the suffix of a replacement file's name needs: ".tmp", two decimal numbers, a dot and the NUL. */
#define OS_TEMPORARY_SUFFIX_MAX	64U

/* How many names a replacement file tries before it gives up on finding an unused one. */
#define OS_TEMPORARY_ATTEMPTS	100

/* How many times MoveFileExW is asked again when another program holds the destination for a moment. */
#define OS_MOVE_RETRIES		5

/* How long to wait before asking again, in milliseconds. */
#define OS_MOVE_WAIT		20U

/* The name of the lock file inside a locked directory. */
#define OS_LOCK_NAME		"lock"

/* How many times the full path or the current directory is asked for again when its size changed. */
#define OS_SIZE_ATTEMPTS	4

/*
 * A lock this process holds on a directory.
 *
 * It owns the handle of the lock file; the system's byte-range lock lives
 * as long as the handle does.
 */
struct wb_os_lock {
	HANDLE handle;
};

/*
 * The counter that makes the names of replacement files differ inside the
 * process.  It only ever increases and is advanced with an interlocked
 * increment, so threads replacing files in the same directory never choose
 * the same name.
 */
static volatile LONG os_temporary_counter;

static int os_error_here(void);
static int os_file_error_here(void);
static int os_file_error_from_code(unsigned long code);
static int os_is_directory(const wchar_t *path);
static int os_open_error(const wchar_t *path);
static void os_buffer_restore(struct wb_buffer *buffer, size_t length);
static int os_append_text(struct wb_buffer *buffer, const char *text, size_t length);
static int os_write_all(HANDLE handle, const void *bytes, size_t length);
static int os_read_all(HANDLE handle, uint64_t size_hint, struct wb_buffer *buffer);
static size_t os_format_number(wchar_t *out, unsigned long value);
static size_t os_root_length(const wchar_t *path, size_t length);
static size_t os_skip_names(const wchar_t *path, size_t length, size_t at, int count);
static int os_ensure_directory(const wchar_t *path);
static int os_is_separator(int c);
static int os_is_letter(int c);
static int os_has_drive(const char *path);
static int os_check_text(const char *path, size_t *length);
static int os_convert_slashes(struct wb_buffer *buffer, const char *text, int to_url);

/*
 * Appends the whole contents of a file to buffer.
 *
 * The file is read in binary and opened so that other readers, writers and
 * deleters are not kept out.
 */
int
wb_os_file_read(
	const char *path,
	struct wb_buffer *buffer)
{
	BY_HANDLE_FILE_INFORMATION information;
	wchar_t *wide;
	HANDLE handle;
	uint64_t hint;
	size_t start;
	int error;
	int fetched;

	/* Refuses a call that names no buffer. */
	if (buffer == NULL)
		return EINVAL;

	/* Converts the path for the W functions; NULL, empty and badly encoded paths are refused. */
	error = wb_os_wide_path(path, &wide);
	if (error != 0)
		return error;

	/* Makes sure the buffer has storage, so a NUL follows even an empty file. */
	start = buffer->length;
	error = wb_buffer_reserve(buffer, 0);
	if (error != 0) {
		free(wide);
		return error;
	}

	/* Opens the file for reading and shares it with every other user. */
	handle = CreateFileW(
		wide,
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
		NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		error = os_open_error(wide);
		free(wide);
		return error;
	}
	free(wide);

	/* Learns what the file is, so a directory is refused and the size is known. */
	fetched = GetFileInformationByHandle(handle, &information);
	if (fetched == 0) {
		error = os_file_error_here();
		CloseHandle(handle);
		return error;
	}

	/* Refuses a directory. */
	if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
		CloseHandle(handle);
		return EISDIR;
	}

	/* Takes the size the file claims as a hint for how much room to make. */
	hint = ((uint64_t)information.nFileSizeHigh << 32) | (uint64_t)information.nFileSizeLow;

	/* Reads the file to its end. */
	error = os_read_all(handle, hint, buffer);
	CloseHandle(handle);
	if (error != 0) {
		os_buffer_restore(buffer, start);
		return error;
	}

	/* Succeeded: the contents are appended and followed by a NUL. */
	return 0;
}

/*
 * Creates or empties a file and writes length bytes to it.
 *
 * The file is written in binary; other programs may read it meanwhile.
 */
int
wb_os_file_write(
	const char *path,
	const void *bytes,
	size_t length)
{
	wchar_t *wide;
	HANDLE handle;
	int error;
	int close_error;
	int closed;

	/* Refuses bytes that are missing when some are to be written. */
	if (bytes == NULL && length != 0)
		return EINVAL;

	/* Converts the path for the W functions; NULL, empty and badly encoded paths are refused. */
	error = wb_os_wide_path(path, &wide);
	if (error != 0)
		return error;

	/* Creates the file or empties the one that is there. */
	handle = CreateFileW(
		wide,
		GENERIC_WRITE,
		FILE_SHARE_READ,
		NULL,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		error = os_open_error(wide);
		free(wide);
		return error;
	}
	free(wide);

	/* Writes every byte. */
	error = os_write_all(handle, bytes, length);

	/* Closes the file; a late failure of the disk shows up here. */
	close_error = 0;
	closed = CloseHandle(handle);
	if (closed == 0)
		close_error = os_file_error_here();
	if (error != 0)
		return error;
	if (close_error != 0)
		return close_error;

	/* Succeeded: the file holds the bytes. */
	return 0;
}

/*
 * Replaces a file's contents as one step.
 *
 * The bytes go to a new file beside the old one, are flushed to the disk,
 * and the new file is moved over the old, so a reader sees one or the other
 * whole.
 */
int
wb_os_file_replace(
	const char *path,
	const void *bytes,
	size_t length)
{
	wchar_t *destination;
	wchar_t *temporary;
	size_t destination_length;
	size_t at;
	unsigned long number;
	int attempt;
	int error;
	int close_error;
	int flushed;
	int closed;
	int moved;
	unsigned long code;
	HANDLE handle;

	/* Refuses bytes that are missing when some are to be written. */
	if (bytes == NULL && length != 0)
		return EINVAL;

	/* Converts the path for the W functions; NULL, empty and badly encoded paths are refused. */
	error = wb_os_wide_path(path, &destination);
	if (error != 0)
		return error;

	/* Allocates the temporary file's name. */
	destination_length = wcslen(destination);
	temporary = (wchar_t *)malloc((destination_length + OS_TEMPORARY_SUFFIX_MAX) * sizeof(wchar_t));
	if (temporary == NULL) {
		free(destination);
		return ENOMEM;
	}

	/* Creates a new file under a name no one else is using. */
	handle = INVALID_HANDLE_VALUE;
	error = EIO;
	for (attempt = 0;
	     attempt < OS_TEMPORARY_ATTEMPTS;
	     attempt++) {
		/* Names the file after the path, the next number of this process and the process. */
		number = (unsigned long)InterlockedIncrement(&os_temporary_counter);
		wmemcpy(temporary, destination, destination_length);
		at = destination_length;
		wmemcpy(temporary + at, L".tmp", 4U);
		at += 4U;
		at += os_format_number(temporary + at, number);
		temporary[at] = L'.';
		at++;
		at += os_format_number(temporary + at, (unsigned long)GetCurrentProcessId());
		temporary[at] = L'\0';

		/* Creates the file only if the name is unused. */
		handle = CreateFileW(
			temporary,
			GENERIC_WRITE,
			FILE_SHARE_READ,
			NULL,
			CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL,
			NULL);
		if (handle != INVALID_HANDLE_VALUE) {
			error = 0;
			break;
		}

		/* Tries the next name when this one is taken, and gives up on any other failure. */
		code = GetLastError();
		if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) {
			error = os_file_error_from_code(code);
			break;
		}
	}
	if (error != 0) {
		free(temporary);
		free(destination);
		return error;
	}

	/* Writes every byte of the new contents. */
	error = os_write_all(handle, bytes, length);

	/* Flushes the new file to the disk before it can be seen under the old name. */
	if (error == 0) {
		flushed = FlushFileBuffers(handle);
		if (flushed == 0)
			error = os_file_error_here();
	}

	/* Closes the new file; a late failure of the disk shows up here. */
	close_error = 0;
	closed = CloseHandle(handle);
	if (closed == 0)
		close_error = os_file_error_here();
	if (error == 0)
		error = close_error;
	if (error != 0) {
		DeleteFileW(temporary);
		free(temporary);
		free(destination);
		return error;
	}

	/*
	 * Moves the new file over the old one.  Another program, such as a
	 * virus scanner, may hold the old file for a moment, so a refusal or a
	 * sharing violation is tried again after a short wait.
	 */
	error = EIO;
	for (attempt = 0;
	     attempt <= OS_MOVE_RETRIES;
	     attempt++) {
		/* Waits before every try but the first. */
		if (attempt > 0)
			Sleep(OS_MOVE_WAIT);

		/* Moves the file; the move is on the disk when the call returns. */
		moved = MoveFileExW(temporary, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
		if (moved != 0) {
			error = 0;
			break;
		}
		code = GetLastError();
		error = os_file_error_from_code(code);

		/* A directory in the way will not go away; reports it as such. */
		if (code == ERROR_ACCESS_DENIED && os_is_directory(destination) != 0) {
			error = EISDIR;
			break;
		}

		/* Tries again only for the failures another program's hold causes. */
		if (error != EACCES && error != EBUSY)
			break;
	}
	if (error != 0) {
		DeleteFileW(temporary);
		free(temporary);
		free(destination);
		return error;
	}
	free(temporary);
	free(destination);

	/* Succeeded: the path now holds the new contents in full. */
	return 0;
}

/*
 * Removes a file, not a directory.
 */
int
wb_os_file_remove(
	const char *path)
{
	wchar_t *wide;
	unsigned long code;
	int error;
	int removed;
	int directory;

	/* Converts the path for the W functions; NULL, empty and badly encoded paths are refused. */
	error = wb_os_wide_path(path, &wide);
	if (error != 0)
		return error;

	/* Deletes the file. */
	removed = DeleteFileW(wide);
	if (removed != 0) {
		free(wide);
		return 0;
	}
	code = GetLastError();

	/* The system refuses a directory with an access error; the directory is what to report. */
	directory = 0;
	if (code == ERROR_ACCESS_DENIED)
		directory = os_is_directory(wide);
	free(wide);
	if (directory != 0)
		return EISDIR;

	/* Reports why the file could not be removed. */
	error = wb_os_error_from_system(code);
	if (error == ENOTDIR)
		error = ENOENT;
	return error;
}

/*
 * Creates a directory and every missing directory above it.
 */
int
wb_os_make_directories(
	const char *path)
{
	wchar_t *wide;
	wchar_t saved;
	size_t length;
	size_t index;
	size_t start;
	size_t made;
	int error;

	/* Converts the path for the W functions; NULL, empty and badly encoded paths are refused. */
	error = wb_os_wide_path(path, &wide);
	if (error != 0)
		return error;

	/*
	 * Makes each part of the path in turn, from the top, after the root
	 * (the drive or the server and share), which is not made: a part ends
	 * at a separator or at the end of the text, and a part with no name (a
	 * doubled or a trailing separator) is left alone.
	 */
	length = wcslen(wide);
	start = os_root_length(wide, length);
	made = 0;
	error = 0;
	for (index = start;
	     index <= length && error == 0;
	     index++) {
		/* Skips a position that does not end a part. */
		if (wide[index] != L'\\' && wide[index] != L'\0')
			continue;

		/* Skips a part that has no name. */
		if (index == start) {
			start = index + 1U;
			continue;
		}

		/* Makes the directory that ends here. */
		saved = wide[index];
		wide[index] = L'\0';
		error = os_ensure_directory(wide);
		wide[index] = saved;
		start = index + 1U;
		made++;
	}

	/* A path that is only a root has nothing to make; checks that it is there. */
	if (error == 0 && made == 0)
		error = os_ensure_directory(wide);
	free(wide);

	/* A part that vanished while the walk was under way is a failure of the system, not a missing file. */
	if (error == ENOENT)
		error = EIO;

	/* Reports a part that could not be made. */
	if (error != 0)
		return error;

	/* Succeeded: the directory exists. */
	return 0;
}

/*
 * Calls each with every name in a directory.
 *
 * A name that is not valid UTF-16 is skipped.
 */
int
wb_os_list(
	const char *path,
	wb_os_list_each each,
	void *context)
{
	WIN32_FIND_DATAW found;
	wchar_t *wide;
	wchar_t *pattern;
	char *name;
	size_t wide_length;
	unsigned long code;
	HANDLE search;
	int error;
	int stop;
	int more;
	int converted;

	/* Refuses a call that names no function to call. */
	if (each == NULL)
		return EINVAL;

	/* Converts the path for the W functions; NULL, empty and badly encoded paths are refused. */
	error = wb_os_wide_path(path, &wide);
	if (error != 0)
		return error;

	/* Builds the search pattern: every name in the directory. */
	wide_length = wcslen(wide);
	pattern = (wchar_t *)malloc((wide_length + 3U) * sizeof(wchar_t));
	if (pattern == NULL) {
		free(wide);
		return ENOMEM;
	}
	wmemcpy(pattern, wide, wide_length);
	if (wide_length > 0 && wide[wide_length - 1U] != L'\\') {
		pattern[wide_length] = L'\\';
		wide_length++;
	}
	pattern[wide_length] = L'*';
	pattern[wide_length + 1U] = L'\0';

	/* Starts the search. */
	search = FindFirstFileW(pattern, &found);
	free(pattern);
	if (search == INVALID_HANDLE_VALUE) {
		code = GetLastError();
		error = wb_os_error_from_system(code);

		/* Tells a file and a missing directory apart, and an empty root from a missing one. */
		if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND || code == ERROR_DIRECTORY) {
			if (os_is_directory(wide) != 0) {
				free(wide);
				return 0;
			}
			if (GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES)
				error = ENOTDIR;
		}
		free(wide);
		return error;
	}
	free(wide);

	/* Visits the entries until the last one, an error, or the caller's stop. */
	stop = 0;
	more = 1;
	while (more != 0) {
		/* Skips the two entries every directory has. */
		if (wcscmp(found.cFileName, L".") != 0 && wcscmp(found.cFileName, L"..") != 0) {
			/* Converts the name; a name that is not text is skipped. */
			name = NULL;
			converted = wb_os_utf8_from_wide(found.cFileName, (size_t)-1, &name, NULL);
			if (converted == ENOMEM) {
				stop = ENOMEM;
				break;
			}
			if (converted == 0) {
				/* Offers the name to the caller. */
				stop = each(context, name);
				free(name);
				if (stop != 0)
					break;
			}
		}

		/* Moves to the next entry. */
		more = FindNextFileW(search, &found);
	}

	/* Tells the end of the directory from a failure of the walk. */
	if (stop == 0) {
		code = GetLastError();
		if (code != ERROR_NO_MORE_FILES)
			stop = wb_os_error_from_system(code);
	}
	FindClose(search);

	/* Reports the caller's stop or the failure of the walk. */
	if (stop != 0)
		return stop;

	/* Succeeded: every name was offered. */
	return 0;
}

/*
 * Appends the absolute form of path to buffer.
 *
 * The path is resolved against the current directory and drive and its "."
 * and ".." parts are removed; whether it exists is not checked.
 */
int
wb_os_full_path(
	const char *path,
	struct wb_buffer *buffer)
{
	wchar_t *wide;
	wchar_t *full;
	char *text;
	size_t text_length;
	size_t capacity;
	unsigned long needed;
	int attempt;
	int error;

	/* Refuses a call that names no buffer. */
	if (buffer == NULL)
		return EINVAL;

	/* Refuses an empty path. */
	if (path != NULL && path[0] == '\0')
		return EINVAL;

	/* Converts the path to UTF-16. */
	error = wb_os_wide_from_utf8(path, &wide, NULL);
	if (error != 0)
		return error;

	/* Asks for the full path with a buffer sized by the system's own answer, again if the answer grows. */
	full = NULL;
	capacity = 0;
	for (attempt = 0;
	     attempt < OS_SIZE_ATTEMPTS;
	     attempt++) {
		/* Learns how many units the answer needs, with its NUL. */
		needed = GetFullPathNameW(wide, (DWORD)capacity, full, NULL);
		if (needed == 0) {
			error = os_error_here();
			free(full);
			free(wide);
			return error;
		}

		/* Stops when the answer fit. */
		if (full != NULL && needed < capacity)
			break;

		/* Makes a buffer of the size the system asked for. */
		free(full);
		capacity = (size_t)needed + 1U;
		full = (wchar_t *)malloc(capacity * sizeof(wchar_t));
		if (full == NULL) {
			free(wide);
			return ENOMEM;
		}
	}
	free(wide);
	if (attempt >= OS_SIZE_ATTEMPTS) {
		free(full);
		return EIO;
	}

	/* Converts the answer to UTF-8. */
	error = wb_os_utf8_from_wide(full, (size_t)-1, &text, &text_length);
	free(full);
	if (error != 0)
		return error;

	/* Appends the answer. */
	error = os_append_text(buffer, text, text_length);
	free(text);
	if (error != 0)
		return error;

	/* Succeeded: the absolute path is appended. */
	return 0;
}

/*
 * Appends the current working directory to buffer.
 */
int
wb_os_current_directory(
	struct wb_buffer *buffer)
{
	wchar_t *wide;
	char *text;
	size_t text_length;
	size_t capacity;
	unsigned long needed;
	int attempt;
	int error;

	/* Refuses a call that names no buffer. */
	if (buffer == NULL)
		return EINVAL;

	/* Asks for the directory with a buffer sized by the system's own answer, again if the answer grows. */
	wide = NULL;
	capacity = 0;
	for (attempt = 0;
	     attempt < OS_SIZE_ATTEMPTS;
	     attempt++) {
		/* Learns how many units the answer needs, with its NUL. */
		needed = GetCurrentDirectoryW((DWORD)capacity, wide);
		if (needed == 0) {
			error = os_error_here();
			free(wide);
			return error;
		}

		/* Stops when the answer fit; a fitting answer is counted without its NUL. */
		if (wide != NULL && needed < capacity)
			break;

		/* Makes a buffer of the size the system asked for. */
		free(wide);
		capacity = (size_t)needed + 1U;
		wide = (wchar_t *)malloc(capacity * sizeof(wchar_t));
		if (wide == NULL)
			return ENOMEM;
	}
	if (attempt >= OS_SIZE_ATTEMPTS) {
		free(wide);
		return EIO;
	}

	/* Converts the answer to UTF-8. */
	error = wb_os_utf8_from_wide(wide, (size_t)-1, &text, &text_length);
	free(wide);
	if (error != 0)
		return error;

	/* Appends the answer. */
	error = os_append_text(buffer, text, text_length);
	free(text);
	if (error != 0)
		return error;

	/* Succeeded: the current directory is appended. */
	return 0;
}

/*
 * Reports whether a path is an absolute local path.
 *
 * A drive letter, a colon and a separator, or two separators and a name.
 */
int
wb_os_path_is_absolute(
	const char *path)
{
	int separator_first;
	int separator_second;
	int separator_third;

	/* NULL and the empty path name nothing. */
	if (path == NULL)
		return 0;
	if (path[0] == '\0')
		return 0;

	/* A drive letter, a colon and a separator. */
	if (os_has_drive(path) != 0) {
		separator_first = os_is_separator((unsigned char)path[2]);
		if (separator_first != 0)
			return 1;
		return 0;
	}

	/* Two separators followed by something that is not a separator: a UNC path. */
	separator_first = os_is_separator((unsigned char)path[0]);
	if (separator_first == 0)
		return 0;
	separator_second = os_is_separator((unsigned char)path[1]);
	if (separator_second == 0)
		return 0;
	separator_third = os_is_separator((unsigned char)path[2]);
	if (path[2] != '\0' && separator_third == 0)
		return 1;

	/* Anything else depends on the current drive or directory. */
	return 0;
}

/*
 * Appends the local path a file: URL names.
 *
 * Only a path that starts with a drive letter is accepted, and only on
 * this machine.
 */
int
wb_os_path_from_url(
	const char *host,
	const char *path,
	struct wb_buffer *buffer)
{
	size_t start;
	int error;

	/* Refuses a call that names no path or no buffer. */
	if (path == NULL || buffer == NULL)
		return EINVAL;

	/* Refuses a URL that names another machine: opening it would send the user's credentials there. */
	if (host != NULL && host[0] != '\0')
		return ENOENT;

	/* Refuses a path that does not start with "/X:" and then a separator or the end ('|' stands for ':'). */
	if (path[0] != '/')
		return ENOENT;
	if (os_is_letter((unsigned char)path[1]) == 0)
		return ENOENT;
	if (path[2] != ':' && path[2] != '|')
		return ENOENT;
	if (path[3] != '/' && path[3] != '\0')
		return ENOENT;

	/* Writes the drive and its root. */
	start = buffer->length;
	error = wb_buffer_append_byte(buffer, (unsigned char)path[1]);
	if (error == 0)
		error = wb_buffer_append_byte(buffer, ':');
	if (error == 0)
		error = wb_buffer_append_byte(buffer, '\\');
	if (error != 0) {
		os_buffer_restore(buffer, start);
		return error;
	}

	/* Writes the rest of the path with the Windows separator. */
	if (path[3] == '/') {
		error = os_convert_slashes(buffer, path + 4, 0);
		if (error != 0) {
			os_buffer_restore(buffer, start);
			return error;
		}
	}

	/* Succeeded: the local path is appended. */
	return 0;
}

/*
 * Splits an absolute local path into the host and the path of its file: URL.
 *
 * A drive path has no host; a UNC path's server is the host.
 */
int
wb_os_url_from_path(
	const char *local,
	struct wb_buffer *host,
	struct wb_buffer *path)
{
	size_t host_start;
	size_t path_start;
	size_t server_end;
	int error;

	/* Refuses a call that names no path or no place for an answer. */
	if (local == NULL || host == NULL || path == NULL)
		return EINVAL;
	host_start = host->length;
	path_start = path->length;

	/* A drive path: "/X:" and the rest with the URL separator. */
	if (os_has_drive(local) != 0) {
		/* Refuses a drive-relative path. */
		if (os_is_separator((unsigned char)local[2]) == 0)
			return EINVAL;

		error = wb_buffer_append_byte(path, '/');
		if (error == 0)
			error = wb_buffer_append_byte(path, (unsigned char)local[0]);
		if (error == 0)
			error = wb_buffer_append_byte(path, ':');
		if (error == 0)
			error = os_convert_slashes(path, local + 2, 1);
		if (error != 0) {
			os_buffer_restore(path, path_start);
			return error;
		}

		/* Succeeded: the URL's path is appended and the host stays empty. */
		return 0;
	}

	/* Anything but a UNC path is relative, and a path that begins "\\?\" or "\\.\" names a device. */
	if (os_is_separator((unsigned char)local[0]) == 0)
		return EINVAL;
	if (os_is_separator((unsigned char)local[1]) == 0)
		return EINVAL;
	if (local[2] == '\0' || os_is_separator((unsigned char)local[2]) != 0)
		return EINVAL;
	if (local[2] == '?')
		return EINVAL;
	if (local[2] == '.' && (local[3] == '\0' || os_is_separator((unsigned char)local[3]) != 0))
		return EINVAL;

	/* Finds the end of the server's name. */
	server_end = 2;
	while (local[server_end] != '\0' && os_is_separator((unsigned char)local[server_end]) == 0)
		server_end++;

	/* Appends the server as the host. */
	error = wb_buffer_append(host, local + 2, server_end - 2U);
	if (error != 0) {
		os_buffer_restore(host, host_start);
		return error;
	}

	/* Appends the share and the rest as the path; a server alone gives the root. */
	if (local[server_end] == '\0')
		error = wb_buffer_append_byte(path, '/');
	else
		error = os_convert_slashes(path, local + server_end, 1);
	if (error != 0) {
		os_buffer_restore(host, host_start);
		os_buffer_restore(path, path_start);
		return error;
	}

	/* Succeeded: the host and the URL's path are appended. */
	return 0;
}

/*
 * Takes this process's lock on a directory, creating "<directory>/lock".
 *
 * The lock is a byte-range lock on the first byte of the lock file, held by
 * a handle that is not inherited.
 */
int
wb_os_lock_open(
	const char *directory,
	struct wb_os_lock **lock)
{
	OVERLAPPED region;
	struct wb_os_lock *entry;
	wchar_t *wide;
	char *lock_path;
	size_t length;
	size_t extra;
	unsigned long code;
	HANDLE handle;
	int error;
	int locked;

	/* Leaves the output in its failure state until the lock is taken. */
	if (lock != NULL)
		*lock = NULL;

	/* Refuses a call that names no place for the lock. */
	if (lock == NULL)
		return EINVAL;

	/* Refuses a directory that is NULL, empty or too long. */
	error = os_check_text(directory, &length);
	if (error != 0)
		return error;
	if (length > WB_OS_PATH_MAX - sizeof(OS_LOCK_NAME) - 1U)
		return ENAMETOOLONG;

	/* Names the lock file; a directory that ends in a separator needs none added. */
	lock_path = (char *)malloc(length + 1U + sizeof(OS_LOCK_NAME));
	if (lock_path == NULL)
		return ENOMEM;
	memcpy(lock_path, directory, length);
	extra = 0;
	if (os_is_separator((unsigned char)directory[length - 1U]) == 0) {
		lock_path[length] = '\\';
		extra = 1;
	}
	memcpy(lock_path + length + extra, OS_LOCK_NAME, sizeof(OS_LOCK_NAME));

	/* Converts the lock file's path for the W functions. */
	error = wb_os_wide_path(lock_path, &wide);
	free(lock_path);
	if (error != 0)
		return error;

	/* Allocates the lock. */
	entry = (struct wb_os_lock *)malloc(sizeof(*entry));
	if (entry == NULL) {
		free(wide);
		return ENOMEM;
	}

	/* Opens the lock file, creating it if it is missing; the handle is not inherited. */
	handle = CreateFileW(
		wide,
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
	free(wide);
	if (handle == INVALID_HANDLE_VALUE) {
		error = os_file_error_here();
		free(entry);
		return error;
	}

	/* Takes the exclusive lock on the first byte without waiting for another holder to let go. */
	memset(&region, 0, sizeof(region));
	locked = LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &region);
	if (locked == 0) {
		code = GetLastError();
		error = wb_os_error_from_system(code);
		CloseHandle(handle);
		free(entry);
		return error;
	}

	/* Hands the lock to the caller. */
	entry->handle = handle;
	*lock = entry;

	/* Succeeded: this process holds the lock. */
	return 0;
}

/*
 * Releases a lock and frees it.
 *
 * The lock file stays.
 */
void
wb_os_lock_close(
	struct wb_os_lock *lock)
{
	OVERLAPPED region;

	/* NULL holds nothing. */
	if (lock == NULL)
		return;

	/* Releases the byte range and closes the handle. */
	memset(&region, 0, sizeof(region));
	UnlockFileEx(lock->handle, 0, 1, 0, &region);
	CloseHandle(lock->handle);

	/* Frees the lock. */
	free(lock);
}

/*
 * Maps the calling thread's last Windows error to the layer's value.
 */
static int
os_error_here(void)
{
	unsigned long code;
	int error;

	/* Reads the error before anything else can change it. */
	code = GetLastError();
	error = wb_os_error_from_system(code);

	/* Reports the layer's value. */
	return error;
}

/*
 * Maps the last Windows error for the functions that work on files.
 */
static int
os_file_error_here(void)
{
	unsigned long code;
	int error;

	/* Reads the error before anything else can change it. */
	code = GetLastError();
	error = os_file_error_from_code(code);

	/* Reports the layer's value. */
	return error;
}

/*
 * Maps a Windows error code for the functions that work on files.
 *
 * Their contracts have no ENOTDIR: a path through something that is not a
 * directory names no file.
 */
static int
os_file_error_from_code(
	unsigned long code)
{
	int error;

	/* Maps the code. */
	error = wb_os_error_from_system(code);

	/* A file inside a thing that is not a directory does not exist. */
	if (error == ENOTDIR)
		return ENOENT;

	/* Reports the layer's value. */
	return error;
}

/*
 * Reports whether a path exists and is a directory.
 *
 * It does not change the last error the caller saw, only reads attributes.
 */
static int
os_is_directory(
	const wchar_t *path)
{
	unsigned long attributes;

	/* Reads the attributes. */
	attributes = GetFileAttributesW(path);
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return 0;

	/* A directory carries the directory attribute. */
	if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
		return 1;

	/* It exists and is something else. */
	return 0;
}

/*
 * Maps the failure of an open call that has just happened.
 *
 * The system reports a directory opened as a file as an access error; the
 * layer reports it as EISDIR.
 */
static int
os_open_error(
	const wchar_t *path)
{
	unsigned long code;
	int error;

	/* Reads the error before the attribute check can change it. */
	code = GetLastError();
	error = wb_os_error_from_system(code);

	/* A directory in the way is not a plain refusal. */
	if (code == ERROR_ACCESS_DENIED && os_is_directory(path) != 0)
		return EISDIR;

	/* A path through something that is not a directory names no file. */
	if (error == ENOTDIR)
		return ENOENT;

	/* Reports the layer's value. */
	return error;
}

/*
 * Puts a buffer back to a length it had, keeping the NUL after the bytes.
 */
static void
os_buffer_restore(
	struct wb_buffer *buffer,
	size_t length)
{
	/* Cuts the buffer back and terminates it again. */
	buffer->length = length;
	if (buffer->data != NULL)
		buffer->data[length] = '\0';
}

/*
 * Appends text to a buffer; on failure the buffer keeps its length.
 */
static int
os_append_text(
	struct wb_buffer *buffer,
	const char *text,
	size_t length)
{
	size_t start;
	int error;

	/* Appends, and cuts the buffer back if it could not take all of it. */
	start = buffer->length;
	error = wb_buffer_append(buffer, text, length);
	if (error != 0) {
		os_buffer_restore(buffer, start);
		return error;
	}

	/* Succeeded: the text is appended. */
	return 0;
}

/*
 * Writes every byte to a handle, in pieces, retrying short writes.
 */
static int
os_write_all(
	HANDLE handle,
	const void *bytes,
	size_t length)
{
	const unsigned char *cursor;
	size_t left;
	size_t piece;
	DWORD written;
	int done;

	/* Writes until nothing is left. */
	cursor = (const unsigned char *)bytes;
	left = length;
	while (left > 0) {
		piece = left;
		if (piece > OS_WRITE_MAX)
			piece = OS_WRITE_MAX;

		/* Writes one piece. */
		written = 0;
		done = WriteFile(handle, cursor, (DWORD)piece, &written, NULL);
		if (done == 0)
			return os_file_error_here();

		/* A write that takes nothing would loop for ever. */
		if (written == 0)
			return EIO;

		cursor += written;
		left -= (size_t)written;
	}

	/* Succeeded: every byte is written. */
	return 0;
}

/*
 * Reads a handle to its end onto the end of a buffer.
 *
 * size_hint is the size the file claims, used to make room at once.  The
 * buffer is left with the bytes read so far on a failure; the caller cuts
 * it back.
 */
static int
os_read_all(
	HANDLE handle,
	uint64_t size_hint,
	struct wb_buffer *buffer)
{
	size_t room;
	DWORD got;
	int error;
	int done;

	/* Makes room for the whole file at once when its size is known. */
	if (size_hint > 0) {
		if (size_hint > (uint64_t)((size_t)-1 - OS_READ_MIN))
			return ENOMEM;
		error = wb_buffer_reserve(buffer, (size_t)size_hint + OS_READ_MIN);
		if (error != 0)
			return error;
	}

	/* Reads until the end of the file. */
	for (;;) {
		/* Keeps at least a minimal room free for the next read. */
		room = buffer->capacity - buffer->length - 1U;
		if (room < OS_READ_MIN) {
			error = wb_buffer_reserve(buffer, OS_READ_MIN);
			if (error != 0)
				return error;
			room = buffer->capacity - buffer->length - 1U;
		}
		if (room > OS_READ_MAX)
			room = OS_READ_MAX;

		/* Reads into the free room. */
		got = 0;
		done = ReadFile(handle, buffer->data + buffer->length, (DWORD)room, &got, NULL);
		if (done == 0) {
			/* The end of the file is not an error. */
			if (GetLastError() == ERROR_HANDLE_EOF)
				break;
			return os_file_error_here();
		}

		/* The end of the file. */
		if (got == 0)
			break;

		buffer->length += (size_t)got;
	}

	/* Terminates the bytes. */
	buffer->data[buffer->length] = '\0';

	/* Succeeded: the file is read to its end. */
	return 0;
}

/*
 * Writes a number in decimal to out, without a NUL.
 *
 * Returns how many units were written (at most 20).
 */
static size_t
os_format_number(
	wchar_t *out,
	unsigned long value)
{
	wchar_t digits[24];
	size_t count;
	size_t index;

	/* Collects the digits, lowest first; zero is one digit. */
	count = 0;
	do {
		digits[count] = (wchar_t)(L'0' + (value % 10UL));
		count++;
		value /= 10UL;
	} while (value != 0);

	/* Writes them highest first. */
	for (index = 0;
	     index < count;
	     index++)
		out[index] = digits[count - 1U - index];

	/* Reports how many units were written. */
	return count;
}

/*
 * Finds where the root of a converted path ends: the part that is never made.
 *
 * Understood: "\\?\C:\", "\\?\UNC\server\share\", "\\.\...", "C:\", "C:",
 * "\\server\share\", "\" and a relative path (no root).
 */
static size_t
os_root_length(
	const wchar_t *path,
	size_t length)
{
	size_t at;

	/* An extended-length UNC path: the server and the share are the root. */
	if (length >= 8U && wmemcmp(path, L"\\\\?\\UNC\\", 8U) == 0) {
		at = os_skip_names(path, length, 8U, 2);
		return at;
	}

	/* An extended-length or device path: a drive is the root when there is one. */
	if (length >= 4U && path[0] == L'\\' && path[1] == L'\\' && (path[2] == L'?' || path[2] == L'.') && path[3] == L'\\') {
		at = 4U;
		if (length >= 6U && os_is_letter((int)path[4]) != 0 && path[5] == L':') {
			at = 6U;
			if (length > 6U && path[6] == L'\\')
				at = 7U;
		}
		return at;
	}

	/* A drive, with or without the separator after it. */
	if (length >= 2U && os_is_letter((int)path[0]) != 0 && path[1] == L':') {
		at = 2U;
		if (length > 2U && path[2] == L'\\')
			at = 3U;
		return at;
	}

	/* A UNC path: the server and the share are the root. */
	if (length >= 2U && path[0] == L'\\' && path[1] == L'\\') {
		at = os_skip_names(path, length, 2U, 2);
		return at;
	}

	/* A single leading separator is the root of the current drive. */
	if (length >= 1U && path[0] == L'\\')
		return 1U;

	/* A relative path has no root. */
	return 0;
}

/*
 * Skips count names, each with the separator after it, from a position.
 *
 * Returns the position after them, or the length when the text ends first.
 */
static size_t
os_skip_names(
	const wchar_t *path,
	size_t length,
	size_t at,
	int count)
{
	int done;

	/* Moves over each name and its separator. */
	for (done = 0;
	     done < count;
	     done++) {
		while (at < length && path[at] != L'\\')
			at++;
		if (at < length)
			at++;
	}

	/* Reports where the names end. */
	return at;
}

/*
 * Makes one directory, accepting one that exists.
 *
 * Returns ENOTDIR when the name exists and is not a directory.
 */
static int
os_ensure_directory(
	const wchar_t *path)
{
	unsigned long attributes;
	unsigned long code;
	int created;

	/* Looks at what is there already. */
	attributes = GetFileAttributesW(path);
	if (attributes != INVALID_FILE_ATTRIBUTES) {
		if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			return 0;
		return ENOTDIR;
	}
	code = GetLastError();

	/* Any failure but "nothing there" is the system's refusal. */
	if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
		return wb_os_error_from_system(code);

	/* Makes the directory. */
	created = CreateDirectoryW(path, NULL);
	if (created != 0)
		return 0;
	code = GetLastError();

	/* Another program made the name in the meantime; it is fine if it is a directory. */
	if (code == ERROR_ALREADY_EXISTS) {
		if (os_is_directory(path) != 0)
			return 0;
		return ENOTDIR;
	}

	/* Reports why the directory could not be made. */
	return wb_os_error_from_system(code);
}

/*
 * Reports whether a character is a path separator of Windows.
 */
static int
os_is_separator(
	int c)
{
	/* Both the backslash and the slash separate parts. */
	if (c == '\\')
		return 1;
	if (c == '/')
		return 1;

	/* Anything else belongs to a name. */
	return 0;
}

/*
 * Reports whether a character is an ASCII letter.
 */
static int
os_is_letter(
	int c)
{
	/* Drive letters are ASCII only. */
	if (c >= 'a' && c <= 'z')
		return 1;
	if (c >= 'A' && c <= 'Z')
		return 1;

	/* Anything else is not a drive letter. */
	return 0;
}

/*
 * Reports whether a path starts with a drive letter and a colon.
 *
 * Reads at most the first two bytes.
 */
static int
os_has_drive(
	const char *path)
{
	/* A letter, then a colon. */
	if (os_is_letter((unsigned char)path[0]) == 0)
		return 0;
	if (path[1] != ':')
		return 0;

	/* It does. */
	return 1;
}

/*
 * Checks a path argument and measures it.
 *
 * Returns EINVAL for NULL and the empty path and ENAMETOOLONG for one longer
 * than the layer accepts.  The text is never read past WB_OS_PATH_MAX + 1
 * bytes.  Whether it is UTF-8 is checked when it is converted.
 */
static int
os_check_text(
	const char *path,
	size_t *length)
{
	size_t measured;

	/* Refuses NULL and the empty path. */
	if (path == NULL)
		return EINVAL;
	if (path[0] == '\0')
		return EINVAL;

	/* Measures the path, stopping one byte past the longest the layer accepts. */
	measured = 0;
	while (measured <= WB_OS_PATH_MAX && path[measured] != '\0')
		measured++;
	if (measured > WB_OS_PATH_MAX)
		return ENAMETOOLONG;

	/* Hands the length to the caller. */
	*length = measured;

	/* Succeeded: the path is usable. */
	return 0;
}

/*
 * Appends text with every separator turned to the other system's.
 *
 * With to_url set, '\\' becomes '/'; otherwise '/' becomes '\\'.  On failure
 * the buffer keeps its length.
 */
static int
os_convert_slashes(
	struct wb_buffer *buffer,
	const char *text,
	int to_url)
{
	size_t start;
	size_t index;
	char byte;
	int error;

	/* Appends the text byte by byte, turning the separators. */
	start = buffer->length;
	error = 0;
	for (index = 0;
	     text[index] != '\0' && error == 0;
	     index++) {
		byte = text[index];
		if (to_url != 0 && byte == '\\')
			byte = '/';
		else if (to_url == 0 && byte == '/')
			byte = '\\';
		error = wb_buffer_append_byte(buffer, (unsigned char)byte);
	}
	if (error != 0) {
		os_buffer_restore(buffer, start);
		return error;
	}

	/* Succeeded: the text is appended. */
	return 0;
}
