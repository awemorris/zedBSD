/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What libmedia asks of a POSIX system: the Vulkan loader and reading media files.
 */

#include "media-os.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* The most bytes one pread asks for; a larger request is read in pieces of this size. */
#define MEDIA_OS_READ_PIECE ((size_t)1 << 30)

/* A media file open for reading: the descriptor that open returned. */
struct media_os_file {
	int descriptor;
};

/* The names under which the Vulkan loader may be installed, in the order they are tried. */
static const char *const media_os_vulkan_names[] = {
	"libvulkan.so.1",
	"libvulkan.so"
};

static int media_os_posix_error(int code);
static int media_os_posix_measure(int descriptor, uint64_t *size);

/*
 * Loads the system's Vulkan loader.
 *
 * The loader handle itself is the result, so no memory is held for it and
 * the loader stays loaded for the life of the process.
 */
int
media_os_vulkan_open(
	struct media_os_library **library)
{
	void *handle;
	size_t index;
	size_t count;

	/* A caller with no room for the result broke the contract. */
	if (library == NULL)
		return EINVAL;

	/* Reports no loader unless one opens below. */
	*library = NULL;

	/* Tries each name of the loader until one of them loads. */
	handle = NULL;
	count = sizeof(media_os_vulkan_names) / sizeof(media_os_vulkan_names[0]);
	for (index = 0;
	     index < count;
	     index++) {
		handle = dlopen(media_os_vulkan_names[index], RTLD_NOW | RTLD_LOCAL);
		if (handle != NULL)
			break;
	}

	/* No name loaded: this system has no Vulkan loader. */
	if (handle == NULL)
		return ENODEV;

	/* Hands the loader to the caller. */
	*library = (struct media_os_library *)handle;

	/* Succeeded: the loader is loaded. */
	return 0;
}

/*
 * Finds a function in the loader.
 *
 * The address is moved into a function pointer byte by byte because a
 * data pointer cannot be converted to a function pointer in ISO C.
 */
media_os_function
media_os_library_symbol(
	struct media_os_library *library,
	const char *name)
{
	void *address;
	media_os_function function;

	/* A missing loader or name has no function to find. */
	if (library == NULL)
		return NULL;
	if (name == NULL)
		return NULL;

	/* Looks the name up in the loader. */
	address = dlsym((void *)library, name);
	if (address == NULL)
		return NULL;

	/* Turns the address into a function pointer. */
	memcpy(&function, &address, sizeof(function));

	/* Succeeded: the function the loader exports under the name. */
	return function;
}

/*
 * Opens a file for reading and measures it.
 *
 * A path is a string of bytes on this system, so it is passed on as it is
 * and never tested for UTF-8.
 */
int
media_os_file_open(
	const char *path,
	struct media_os_file **file,
	uint64_t *size)
{
	struct media_os_file *opened;
	uint64_t length;
	int descriptor;
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

	/* Allocates the file object. */
	opened = malloc(sizeof(*opened));
	if (opened == NULL)
		return ENOMEM;

	/* Opens the file, trying again when a signal interrupted the call. */
	do {
		descriptor = open(path, O_RDONLY | O_CLOEXEC);
	} while (descriptor < 0 && errno == EINTR);
	if (descriptor < 0) {
		error = media_os_posix_error(errno);
		free(opened);
		return error;
	}

	/* Measures the file and refuses what is not a regular file. */
	error = media_os_posix_measure(descriptor, &length);
	if (error != 0) {
		close(descriptor);
		free(opened);
		return error;
	}

	/* Hands the file and its size to the caller. */
	opened->descriptor = descriptor;
	*file = opened;
	*size = length;

	/* Succeeded: the file is open. */
	return 0;
}

/*
 * Reads exactly size bytes at offset without using the file position.
 *
 * A read the kernel cuts short is continued, and one a signal interrupted
 * is repeated.
 */
int
media_os_file_read_at(
	struct media_os_file *file,
	uint64_t offset,
	void *data,
	size_t size)
{
	unsigned char *bytes;
	uint64_t last;
	off_t position;
	ssize_t count;
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

	/*
	 * The last byte of the range must be an offset the system can name;
	 * a file never reaches one that it cannot, so such a range ends
	 * beyond the file.
	 */
	last = offset + (uint64_t)size - 1;
	position = (off_t)last;
	if (position < 0)
		return EIO;
	if ((uint64_t)position != last)
		return EIO;

	/* Reads the range piece by piece until every byte has arrived. */
	bytes = data;
	done = 0;
	while (done < size) {
		piece = size - done;
		if (piece > MEDIA_OS_READ_PIECE)
			piece = MEDIA_OS_READ_PIECE;

		count = pread(file->descriptor, bytes + done, piece, (off_t)(offset + (uint64_t)done));

		/* A signal interrupted the call before it read: repeats it. */
		if (count < 0 && errno == EINTR)
			continue;

		/* The read failed. */
		if (count < 0)
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

	/* Gives the descriptor back to the system and frees the object. */
	close(file->descriptor);
	free(file);
}

/* Closes a descriptor that came with a GPU picture. */
void
media_os_close_descriptor(
	int descriptor)
{
	/* A negative value is no descriptor. */
	if (descriptor < 0)
		return;

	/* Gives the descriptor back to the system. */
	close(descriptor);
}

/*
 * Turns the errno of a failed open into the error this interface reports.
 */
static int
media_os_posix_error(
	int code)
{
	/* Groups the system's reasons into the interface's few. */
	switch (code) {
	case ENOENT:
	case ENOTDIR:
		return ENOENT;
	case EACCES:
	case EPERM:
		return EACCES;
	case EISDIR:
		return EISDIR;
	case ENAMETOOLONG:
		return ENAMETOOLONG;
	case ENOMEM:
	case EMFILE:
	case ENFILE:
		return ENOMEM;
	default:
		break;
	}

	/* Anything else is a failure of the file system or the device. */
	return EIO;
}

/*
 * Reads the size of an open descriptor and refuses a directory or any
 * file that has no size to read at offsets (a device, a pipe).
 */
static int
media_os_posix_measure(
	int descriptor,
	uint64_t *size)
{
	struct stat status;
	int failed;

	/* Asks the system about the open file. */
	failed = fstat(descriptor, &status);
	if (failed != 0)
		return EIO;

	/* A directory can be opened for reading but holds no bytes to read. */
	if (S_ISDIR(status.st_mode))
		return EISDIR;

	/* Only a regular file can be read at any offset. */
	if (!S_ISREG(status.st_mode))
		return EIO;

	/* A negative size is no size. */
	if (status.st_size < 0)
		return EIO;

	/* Reports the size. */
	*size = (uint64_t)status.st_size;

	/* Succeeded: the size is known. */
	return 0;
}
