/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What libmedia asks of Windows: the Vulkan loader and reading media files.
 * Paths are converted from UTF-8 to UTF-16 and only the W functions are called.
 */

#include "media-os.h"

#include <windows.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The most bytes one ReadFile asks for; a larger request is read in pieces of this size. */
#define MEDIA_OS_READ_PIECE ((size_t)1 << 30)

/* The most UTF-16 units a path may have (the limit of the extended-length path form). */
#define MEDIA_OS_PATH_UNITS 32767

/* The most UTF-8 bytes a path of that many units can take (three bytes for one unit). */
#define MEDIA_OS_PATH_BYTES (MEDIA_OS_PATH_UNITS * 3)

/* A media file open for reading: the handle CreateFileW returned, opened for synchronous I/O. */
struct media_os_file {
	HANDLE handle;
};

static int media_os_win32_widen(const char *path, wchar_t **wide);
static int media_os_win32_error(DWORD code);
static int media_os_win32_is_directory(const wchar_t *wide);
static int media_os_win32_measure(HANDLE handle, uint64_t *size);

/*
 * Loads the system's Vulkan loader from the system directory only.
 *
 * The module handle itself is the result, so no memory is held for it and
 * the loader stays loaded for the life of the process.
 */
int
media_os_vulkan_open(
	struct media_os_library **library)
{
	HMODULE module;
	DWORD code;

	/* A caller with no room for the result broke the contract. */
	if (library == NULL)
		return EINVAL;

	/* Reports no loader unless one loads below. */
	*library = NULL;

	/* Loads the loader the GPU driver installed, never a copy beside the program. */
	module = LoadLibraryExW(L"vulkan-1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (module == NULL) {
		code = GetLastError();

		/* Memory ran out while loading. */
		if (code == ERROR_NOT_ENOUGH_MEMORY)
			return ENOMEM;
		if (code == ERROR_OUTOFMEMORY)
			return ENOMEM;

		/* Any other failure means this system has no usable loader. */
		return ENODEV;
	}

	/* Hands the loader to the caller. */
	*library = (struct media_os_library *)module;

	/* Succeeded: the loader is loaded. */
	return 0;
}

/*
 * Finds a function in the loader.
 *
 * The address is moved into the function pointer byte by byte so that no
 * cast between unrelated function types is needed.
 */
media_os_function
media_os_library_symbol(
	struct media_os_library *library,
	const char *name)
{
	FARPROC address;
	media_os_function function;

	/* A missing loader or name has no function to find. */
	if (library == NULL)
		return NULL;
	if (name == NULL)
		return NULL;

	/* Looks the name up in the loader. */
	address = GetProcAddress((HMODULE)library, name);
	if (address == NULL)
		return NULL;

	/* Turns the address into the function pointer the caller expects. */
	memcpy(&function, &address, sizeof(function));

	/* Succeeded: the function the loader exports under the name. */
	return function;
}

/*
 * Opens a file for reading and measures it.
 *
 * The file may be open for writing elsewhere, and may be deleted or renamed
 * while it is read, as a file being downloaded can be.
 */
int
media_os_file_open(
	const char *path,
	struct media_os_file **file,
	uint64_t *size)
{
	struct media_os_file *opened;
	wchar_t *wide;
	HANDLE handle;
	uint64_t length;
	DWORD code;
	int directory;
	int error;

	/* The results need somewhere to go. */
	if (file == NULL)
		return EINVAL;

	/* Reports no file unless the open succeeds below, whatever else is wrong. */
	*file = NULL;
	if (size == NULL)
		return EINVAL;

	/* Reports no size unless the open succeeds below. */
	*size = 0;

	/* A missing or empty path names nothing. */
	if (path == NULL)
		return EINVAL;
	if (path[0] == '\0')
		return EINVAL;

	/* Converts the path to UTF-16 for the W functions. */
	error = media_os_win32_widen(path, &wide);
	if (error != 0)
		return error;

	/* Allocates the file object. */
	opened = malloc(sizeof(*opened));
	if (opened == NULL) {
		free(wide);
		return ENOMEM;
	}

	/* Opens the existing file for reading, shared with every other user and not inherited. */
	handle = CreateFileW(
		wide,
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		code = GetLastError();
		error = media_os_win32_error(code);

		/* Opening a directory fails with access denied; asks what the path names. */
		if (code == ERROR_ACCESS_DENIED) {
			directory = media_os_win32_is_directory(wide);
			if (directory != 0)
				error = EISDIR;
		}

		free(wide);
		free(opened);
		return error;
	}

	/* The path is no longer needed. */
	free(wide);

	/* Measures the file and refuses what is not a disk file. */
	error = media_os_win32_measure(handle, &length);
	if (error != 0) {
		CloseHandle(handle);
		free(opened);
		return error;
	}

	/* Hands the file and its size to the caller. */
	opened->handle = handle;
	*file = opened;
	*size = length;

	/* Succeeded: the file is open. */
	return 0;
}

/*
 * Reads exactly size bytes at offset without using the file position.
 *
 * Each ReadFile carries its own offset in an OVERLAPPED on the synchronous
 * handle, asks for at most 1 GiB, and a short read is continued.
 */
int
media_os_file_read_at(
	struct media_os_file *file,
	uint64_t offset,
	void *data,
	size_t size)
{
	unsigned char *bytes;
	OVERLAPPED overlapped;
	uint64_t last;
	uint64_t position;
	BOOL succeeded;
	DWORD count;
	size_t done;
	size_t piece;

	/* A missing file cannot be read. */
	if (file == NULL)
		return EINVAL;

	/* Reading nothing succeeds wherever it is asked, even past the end. */
	if (size == 0)
		return 0;

	/* A read with no room for the bytes broke the contract. */
	if (data == NULL)
		return EINVAL;

	/* A range whose end does not fit in 64 bits broke the contract. */
	if ((uint64_t)size > UINT64_MAX - offset)
		return EINVAL;

	/* A file offset is a signed 64-bit number, so a range ending beyond it ends beyond any file. */
	last = offset + (uint64_t)size - 1;
	if (last > (uint64_t)INT64_MAX)
		return EIO;

	/* Reads the range piece by piece until every byte has arrived. */
	bytes = data;
	done = 0;
	while (done < size) {
		piece = size - done;
		if (piece > MEDIA_OS_READ_PIECE)
			piece = MEDIA_OS_READ_PIECE;

		/* Names where this piece starts in the file. */
		position = offset + (uint64_t)done;
		memset(&overlapped, 0, sizeof(overlapped));
		overlapped.Offset = (DWORD)(position & 0xffffffffU);
		overlapped.OffsetHigh = (DWORD)(position >> 32);

		count = 0;
		succeeded = ReadFile(file->handle, bytes + done, (DWORD)piece, &count, &overlapped);

		/* The read failed (reading at or past the end is one of the ways). */
		if (!succeeded)
			return EIO;

		/* The file ended before the range did: it shrank. */
		if (count == 0)
			return EIO;

		done += (size_t)count;
	}

	/* Succeeded: every byte of the range is in the buffer. */
	return 0;
}

/* Closes a file and frees it. */
void
media_os_file_close(
	struct media_os_file *file)
{
	/* Closing nothing does nothing. */
	if (file == NULL)
		return;

	/* Gives the handle back to the system and frees the object. */
	CloseHandle(file->handle);
	free(file);
}

/* Does nothing: Windows never makes a GPU picture that carries a descriptor. */
void
media_os_close_descriptor(
	int descriptor)
{
	/* There is nothing to close. */
	(void)descriptor;
}

/*
 * Converts a UTF-8 path to a UTF-16 string allocated with malloc.
 *
 * Returns EILSEQ for bytes that are not UTF-8, ENAMETOOLONG for a path
 * longer than Windows names, ENOMEM and EIO.
 */
static int
media_os_win32_widen(
	const char *path,
	wchar_t **wide)
{
	wchar_t *converted;
	size_t length;
	DWORD code;
	int units;
	int written;

	/* Reports nothing unless the conversion succeeds below. */
	*wide = NULL;

	/* Refuses a path no Windows name can need, before sizing a buffer for it. */
	length = strlen(path);
	if (length > MEDIA_OS_PATH_BYTES)
		return ENAMETOOLONG;

	/* Counts the UTF-16 units of the path with its end, refusing bytes that are not UTF-8. */
	units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, (int)length + 1, NULL, 0);
	if (units == 0) {
		code = GetLastError();
		if (code == ERROR_NO_UNICODE_TRANSLATION)
			return EILSEQ;

		return EIO;
	}

	/* Refuses a path with more units than Windows names (the end is not counted). */
	if (units - 1 > MEDIA_OS_PATH_UNITS)
		return ENAMETOOLONG;

	/* Allocates the UTF-16 string. */
	converted = malloc((size_t)units * sizeof(*converted));
	if (converted == NULL)
		return ENOMEM;

	/* Converts the path. */
	written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, (int)length + 1, converted, units);
	if (written != units) {
		free(converted);
		return EIO;
	}

	/* Hands the string to the caller. */
	*wide = converted;

	/* Succeeded: the caller frees the string. */
	return 0;
}

/*
 * Turns the Windows error of a failed open into the error this interface reports.
 */
static int
media_os_win32_error(
	DWORD code)
{
	/* Groups Windows' reasons into the interface's few. */
	switch (code) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_NAME:
	case ERROR_BAD_PATHNAME:
	case ERROR_BAD_NETPATH:
	case ERROR_DIRECTORY:
		return ENOENT;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
		return EACCES;
	case ERROR_FILENAME_EXCED_RANGE:
		return ENAMETOOLONG;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:
	case ERROR_TOO_MANY_OPEN_FILES:
		return ENOMEM;
	default:
		break;
	}

	/* Anything else is a failure of the file system or the device. */
	return EIO;
}

/*
 * Tells whether a path names a directory.
 */
static int
media_os_win32_is_directory(
	const wchar_t *wide)
{
	DWORD attributes;

	/* Asks what the path names. */
	attributes = GetFileAttributesW(wide);
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return 0;

	/* A directory carries the directory attribute. */
	if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
		return 1;

	/* Anything else is a file. */
	return 0;
}

/*
 * Reads the size of an open handle and refuses anything that is not a disk
 * file (a device, a pipe).
 */
static int
media_os_win32_measure(
	HANDLE handle,
	uint64_t *size)
{
	LARGE_INTEGER length;
	DWORD type;
	BOOL succeeded;

	/* Only a disk file can be read at any offset. */
	type = GetFileType(handle);
	if (type != FILE_TYPE_DISK)
		return EIO;

	/* Asks the system for the size in bytes. */
	succeeded = GetFileSizeEx(handle, &length);
	if (!succeeded)
		return EIO;

	/* A negative size is no size. */
	if (length.QuadPart < 0)
		return EIO;

	/* Reports the size. */
	*size = (uint64_t)length.QuadPart;

	/* Succeeded: the size is known. */
	return 0;
}
