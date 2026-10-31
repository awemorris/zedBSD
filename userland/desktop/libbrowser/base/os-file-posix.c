/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The files of the operating-system layer on POSIX systems.
 */

#include "base/os-file.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* The mode a file made by wb_os_file_write is created with, before the umask applies. */
#define OS_FILE_MODE		0644

/* The mode of a replacement file and of a lock file: private to the user. */
#define OS_PRIVATE_FILE_MODE	0600

/* The mode of a directory made by wb_os_make_directories: private to the user. */
#define OS_DIRECTORY_MODE	0700

/* The smallest room a read of a file is offered, in bytes. */
#define OS_READ_MIN		4096U

/* The most bytes one read system call is asked for, so the count always fits the call's type. */
#define OS_READ_MAX		(1U << 30)

/* The most bytes one write system call is given, for the same reason. */
#define OS_WRITE_MAX		(1U << 30)

/* The room the suffix of a replacement file's name needs: ".tmp", two decimal numbers, a dot and the NUL. */
#define OS_TEMPORARY_SUFFIX_MAX	64U

/* How many names a replacement file tries before it gives up on finding an unused one. */
#define OS_TEMPORARY_ATTEMPTS	100

/* The name of the lock file inside a locked directory, with the separator before it. */
#define OS_LOCK_NAME		"/lock"

/*
 * A lock this process holds on a directory.
 *
 * It stays on the list of held locks from wb_os_lock_open to
 * wb_os_lock_close; the descriptor carries the system's flock, and the
 * directory's resolved name is what a second open in this process is
 * compared with.
 */
struct wb_os_lock {
	struct wb_os_lock *next;
	char *directory;
	int descriptor;
};

/*
 * The counter that makes the names of replacement files differ inside the
 * process.  It only ever increases and is advanced with an atomic add, so
 * threads replacing files in the same directory never choose the same name.
 */
static unsigned int os_temporary_counter;

/*
 * The locks this process holds, newest first.
 *
 * os_lock_mutex protects the list and the descriptors' lifetime: a lock is
 * taken and released with the mutex held, so two threads opening the same
 * directory cannot both find it free.  The list is empty when no lock is
 * held.
 */
static struct wb_os_lock *os_lock_held;

/* The mutex of os_lock_held. */
static pthread_mutex_t os_lock_mutex = PTHREAD_MUTEX_INITIALIZER;

static int os_utf8_is_valid(const unsigned char *bytes, size_t length);
static int os_path_check(const char *path, size_t *length);
static int os_error_from_errno(int code);
static int os_file_error_from_errno(int code);
static void os_buffer_restore(struct wb_buffer *buffer, size_t length);
static int os_open(const char *path, int flags, mode_t mode, int *descriptor);
static int os_write_all(int descriptor, const void *bytes, size_t length);
static int os_close(int descriptor);
static int os_sync(int descriptor);
static int os_read_all(int descriptor, size_t size_hint, struct wb_buffer *buffer);
static int os_ensure_directory(const char *path);
static int os_append_text(struct wb_buffer *buffer, const char *text, size_t length);
static int os_lock_acquire(struct wb_os_lock *entry, const char *lock_path);

/*
 * Appends the whole contents of a file to buffer.
 *
 * The buffer keeps its old bytes and gets a NUL after the new ones; on
 * failure its length is what it was.
 */
int
wb_os_file_read(
	const char *path,
	struct wb_buffer *buffer)
{
	struct stat status;
	size_t start;
	size_t hint;
	int descriptor;
	int error;
	int status_result;

	/* Refuses a call that names no buffer. */
	if (buffer == NULL)
		return EINVAL;

	/* Refuses a path that is NULL, empty, too long or not UTF-8. */
	error = os_path_check(path, NULL);
	if (error != 0)
		return error;

	/* Makes sure the buffer has storage, so a NUL follows even an empty file. */
	start = buffer->length;
	error = wb_buffer_reserve(buffer, 0);
	if (error != 0)
		return error;

	/* Opens the file for reading. */
	error = os_open(path, O_RDONLY | O_CLOEXEC, 0, &descriptor);
	if (error != 0)
		return error;

	/* Learns what the file is, so a directory is refused and the size is known. */
	status_result = fstat(descriptor, &status);
	if (status_result != 0) {
		error = os_file_error_from_errno(errno);
		close(descriptor);
		return error;
	}

	/* Refuses a directory. */
	if (S_ISDIR(status.st_mode)) {
		close(descriptor);
		return EISDIR;
	}

	/* Takes the size of a regular file as a hint for how much room to make. */
	hint = 0;
	if (S_ISREG(status.st_mode) && status.st_size > 0)
		hint = (size_t)status.st_size;

	/* Reads the file to its end. */
	error = os_read_all(descriptor, hint, buffer);
	close(descriptor);
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
 * The file is created with mode 0644 under the umask.
 */
int
wb_os_file_write(
	const char *path,
	const void *bytes,
	size_t length)
{
	int descriptor;
	int error;
	int close_error;

	/* Refuses bytes that are missing when some are to be written. */
	if (bytes == NULL && length != 0)
		return EINVAL;

	/* Refuses a path that is NULL, empty, too long or not UTF-8. */
	error = os_path_check(path, NULL);
	if (error != 0)
		return error;

	/* Opens the file, creating or emptying it. */
	error = os_open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, OS_FILE_MODE, &descriptor);
	if (error != 0)
		return error;

	/* Writes every byte. */
	error = os_write_all(descriptor, bytes, length);

	/* Closes the file; a late failure of the disk shows up here. */
	close_error = os_close(descriptor);
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
 * and the new file is renamed over the old, so a reader sees one or the
 * other whole.
 */
int
wb_os_file_replace(
	const char *path,
	const void *bytes,
	size_t length)
{
	char *temporary;
	size_t path_length;
	size_t capacity;
	unsigned int number;
	int attempt;
	int descriptor;
	int error;
	int close_error;
	int renamed;

	/* Refuses bytes that are missing when some are to be written. */
	if (bytes == NULL && length != 0)
		return EINVAL;

	/* Refuses a path that is NULL, empty, too long or not UTF-8. */
	error = os_path_check(path, &path_length);
	if (error != 0)
		return error;

	/* Refuses a path with no room left for the temporary file's suffix. */
	if (path_length > WB_OS_PATH_MAX - OS_TEMPORARY_SUFFIX_MAX)
		return ENAMETOOLONG;

	/* Allocates the temporary file's name. */
	capacity = path_length + OS_TEMPORARY_SUFFIX_MAX;
	temporary = (char *)malloc(capacity);
	if (temporary == NULL)
		return ENOMEM;

	/* Creates a new private file under a name no one else is using. */
	descriptor = -1;
	error = EIO;
	for (attempt = 0;
	     attempt < OS_TEMPORARY_ATTEMPTS;
	     attempt++) {
		/* Takes the next number of this process and names the file after the path, the number and the process. */
		number = __atomic_add_fetch(&os_temporary_counter, 1U, __ATOMIC_RELAXED);
		snprintf(temporary, capacity, "%s.tmp%u.%ld", path, number, (long)getpid());

		/* Creates the file only if the name is unused. */
		error = os_open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, OS_PRIVATE_FILE_MODE, &descriptor);
		if (error != EEXIST)
			break;
	}
	if (error != 0) {
		free(temporary);
		if (error == EEXIST)
			return EIO;
		return error;
	}

	/* Writes every byte of the new contents. */
	error = os_write_all(descriptor, bytes, length);

	/* Flushes the new file to the disk before it can be seen under the old name. */
	if (error == 0)
		error = os_sync(descriptor);

	/* Closes the new file; a late failure of the disk shows up here. */
	close_error = os_close(descriptor);
	if (error == 0)
		error = close_error;
	if (error != 0) {
		unlink(temporary);
		free(temporary);
		return error;
	}

	/* Renames the new file over the old one. */
	renamed = rename(temporary, path);
	if (renamed != 0) {
		error = os_file_error_from_errno(errno);
		unlink(temporary);
		free(temporary);
		return error;
	}

	/* Succeeded: the path now holds the new contents in full. */
	free(temporary);
	return 0;
}

/*
 * Removes a file, not a directory.
 */
int
wb_os_file_remove(
	const char *path)
{
	struct stat status;
	int removed;
	int found;
	int error;
	int code;

	/* Refuses a path that is NULL, empty, too long or not UTF-8. */
	error = os_path_check(path, NULL);
	if (error != 0)
		return error;

	/* Removes the name. */
	removed = unlink(path);
	if (removed == 0)
		return 0;
	code = errno;

	/* Some systems refuse a directory with a permission error; the directory is what to report. */
	if (code == EPERM || code == EACCES) {
		found = lstat(path, &status);
		if (found == 0 && S_ISDIR(status.st_mode))
			return EISDIR;
	}

	/* Reports why the file could not be removed. */
	error = os_file_error_from_errno(code);
	return error;
}

/*
 * Creates a directory and every missing directory above it.
 */
int
wb_os_make_directories(
	const char *path)
{
	char *copy;
	size_t length;
	size_t index;
	size_t start;
	int error;

	/* Refuses a path that is NULL, empty, too long or not UTF-8. */
	error = os_path_check(path, &length);
	if (error != 0)
		return error;

	/* Copies the path, which the walk below cuts into its leading parts one by one. */
	copy = (char *)malloc(length + 1U);
	if (copy == NULL)
		return ENOMEM;
	memcpy(copy, path, length + 1U);

	/* Cuts off the separators at the end. */
	while (length > 1U && copy[length - 1U] == '/')
		length--;
	copy[length] = '\0';

	/*
	 * Makes each part of the path in turn, from the top: the part ends at
	 * a separator or at the end of the text, and a part with no name (the
	 * root, a doubled separator) is left alone.
	 */
	error = 0;
	start = 0;
	for (index = 0;
	     index <= length && error == 0;
	     index++) {
		/* Skips a position that does not end a part. */
		if (copy[index] != '/' && copy[index] != '\0')
			continue;

		/* Skips a part that has no name. */
		if (index == start) {
			start = index + 1U;
			continue;
		}

		/* Makes the directory that ends here. */
		copy[index] = '\0';
		error = os_ensure_directory(copy);
		if (index < length)
			copy[index] = '/';
		start = index + 1U;
	}
	free(copy);

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
 * A name that is not valid UTF-8 is skipped.
 */
int
wb_os_list(
	const char *path,
	wb_os_list_each each,
	void *context)
{
	DIR *directory;
	struct dirent *entry;
	size_t name_length;
	int error;
	int valid;
	int stop;

	/* Refuses a call that names no function to call. */
	if (each == NULL)
		return EINVAL;

	/* Refuses a path that is NULL, empty, too long or not UTF-8. */
	error = os_path_check(path, NULL);
	if (error != 0)
		return error;

	/* Opens the directory. */
	directory = opendir(path);
	if (directory == NULL)
		return os_error_from_errno(errno);

	/* Visits the entries until the last one, an error, or the caller's stop. */
	stop = 0;
	for (;;) {
		/* Reads the next entry; errno stays 0 at the end of the directory. */
		errno = 0;
		entry = readdir(directory);
		if (entry == NULL) {
			if (errno != 0)
				stop = EIO;
			break;
		}

		/* Skips the two entries every directory has. */
		if (strcmp(entry->d_name, ".") == 0)
			continue;
		if (strcmp(entry->d_name, "..") == 0)
			continue;

		/* Skips a name that is not text. */
		name_length = strlen(entry->d_name);
		valid = os_utf8_is_valid((const unsigned char *)entry->d_name, name_length);
		if (valid == 0)
			continue;

		/* Offers the name to the caller. */
		stop = each(context, entry->d_name);
		if (stop != 0)
			break;
	}
	closedir(directory);

	/* Reports the caller's stop or the failure of the walk. */
	if (stop != 0)
		return stop;

	/* Succeeded: every name was offered. */
	return 0;
}

/*
 * Appends the absolute form of path to buffer, with symbolic links resolved.
 *
 * The file must exist.
 */
int
wb_os_full_path(
	const char *path,
	struct wb_buffer *buffer)
{
	char *resolved;
	size_t length;
	int error;
	int valid;

	/* Refuses a call that names no buffer. */
	if (buffer == NULL)
		return EINVAL;

	/* Refuses a path that is NULL, empty, too long or not UTF-8. */
	error = os_path_check(path, NULL);
	if (error != 0)
		return error;

	/* Resolves the path; the system allocates the answer. */
	resolved = realpath(path, NULL);
	if (resolved == NULL)
		return os_file_error_from_errno(errno);

	/* Refuses an answer that is not text or is longer than the layer accepts. */
	length = strlen(resolved);
	valid = os_utf8_is_valid((const unsigned char *)resolved, length);
	if (valid == 0) {
		free(resolved);
		return EILSEQ;
	}
	if (length > WB_OS_PATH_MAX) {
		free(resolved);
		return ENAMETOOLONG;
	}

	/* Appends the answer. */
	error = os_append_text(buffer, resolved, length);
	free(resolved);
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
	char *text;
	char *answer;
	size_t capacity;
	size_t length;
	int error;
	int valid;

	/* Refuses a call that names no buffer. */
	if (buffer == NULL)
		return EINVAL;

	/* Asks for the directory with a buffer that doubles until the answer fits. */
	capacity = 512U;
	text = NULL;
	for (;;) {
		/* Allocates room for the answer. */
		text = (char *)malloc(capacity);
		if (text == NULL)
			return ENOMEM;

		/* Asks the system for the directory. */
		answer = getcwd(text, capacity);
		if (answer != NULL)
			break;

		/* Gives up on any failure but a buffer that was too small. */
		error = errno;
		free(text);
		if (error != ERANGE)
			return os_file_error_from_errno(error);

		/* Refuses a directory longer than the layer accepts. */
		if (capacity > WB_OS_PATH_MAX)
			return ENAMETOOLONG;
		capacity *= 2U;
	}

	/* Refuses an answer that is not text or is longer than the layer accepts. */
	length = strlen(text);
	valid = os_utf8_is_valid((const unsigned char *)text, length);
	if (valid == 0) {
		free(text);
		return EILSEQ;
	}
	if (length > WB_OS_PATH_MAX) {
		free(text);
		return ENAMETOOLONG;
	}

	/* Appends the answer. */
	error = os_append_text(buffer, text, length);
	free(text);
	if (error != 0)
		return error;

	/* Succeeded: the current directory is appended. */
	return 0;
}

/*
 * Reports whether a path is an absolute local path.
 *
 * Only the first byte is looked at.
 */
int
wb_os_path_is_absolute(
	const char *path)
{
	/* NULL and the empty path name nothing. */
	if (path == NULL)
		return 0;
	if (path[0] == '\0')
		return 0;

	/* A path is absolute when it starts at the root. */
	if (path[0] == '/')
		return 1;

	/* Anything else is relative. */
	return 0;
}

/*
 * Appends the local path a file: URL names.
 *
 * Only this machine's files can be named; the path is used as it is.
 */
int
wb_os_path_from_url(
	const char *host,
	const char *path,
	struct wb_buffer *buffer)
{
	int error;

	/* Refuses a call that names no path or no buffer. */
	if (path == NULL || buffer == NULL)
		return EINVAL;

	/* Refuses a URL that names another machine. */
	if (host != NULL && host[0] != '\0')
		return ENOENT;

	/* Appends the path unchanged. */
	error = os_append_text(buffer, path, strlen(path));
	if (error != 0)
		return error;

	/* Succeeded: the local path is appended. */
	return 0;
}

/*
 * Splits an absolute local path into the host and the path of its file: URL.
 *
 * On POSIX the host is always this machine, so only the path is written.
 */
int
wb_os_url_from_path(
	const char *local,
	struct wb_buffer *host,
	struct wb_buffer *path)
{
	int error;

	/* Refuses a call that names no path or no place for an answer. */
	if (local == NULL || host == NULL || path == NULL)
		return EINVAL;

	/* Refuses a path that does not start at the root. */
	if (local[0] != '/')
		return EINVAL;

	/* Appends the path unchanged; the host stays empty. */
	error = os_append_text(path, local, strlen(local));
	if (error != 0)
		return error;

	/* Succeeded: the URL's path is appended. */
	return 0;
}

/*
 * Takes this process's lock on a directory, creating "<directory>/lock".
 *
 * A second open of the same directory in this process is refused, because
 * the system's lock alone does not keep a process out of its own files on
 * every system.
 */
int
wb_os_lock_open(
	const char *directory,
	struct wb_os_lock **lock)
{
	struct wb_os_lock *entry;
	char *lock_path;
	char *resolved;
	size_t length;
	size_t path_size;
	int error;
	int valid;

	/* Leaves the output in its failure state until the lock is taken. */
	if (lock != NULL)
		*lock = NULL;

	/* Refuses a call that names no place for the lock. */
	if (lock == NULL)
		return EINVAL;

	/* Refuses a directory that is NULL, empty, too long or not UTF-8. */
	error = os_path_check(directory, NULL);
	if (error != 0)
		return error;

	/* Resolves the directory, so two spellings of it are one name. */
	resolved = realpath(directory, NULL);
	if (resolved == NULL)
		return os_file_error_from_errno(errno);

	/* Refuses a resolved name that is not text or leaves no room for the lock file's name. */
	length = strlen(resolved);
	valid = os_utf8_is_valid((const unsigned char *)resolved, length);
	if (valid == 0) {
		free(resolved);
		return EILSEQ;
	}
	if (length > WB_OS_PATH_MAX - sizeof(OS_LOCK_NAME)) {
		free(resolved);
		return ENAMETOOLONG;
	}

	/* Names the lock file; the root directory already ends in a separator. */
	path_size = length + sizeof(OS_LOCK_NAME);
	lock_path = (char *)malloc(path_size);
	if (lock_path == NULL) {
		free(resolved);
		return ENOMEM;
	}
	if (length == 1U && resolved[0] == '/')
		snprintf(lock_path, path_size, "%s", OS_LOCK_NAME);
	else
		snprintf(lock_path, path_size, "%s%s", resolved, OS_LOCK_NAME);

	/* Allocates the lock. */
	entry = (struct wb_os_lock *)malloc(sizeof(*entry));
	if (entry == NULL) {
		free(lock_path);
		free(resolved);
		return ENOMEM;
	}
	entry->next = NULL;
	entry->directory = resolved;
	entry->descriptor = -1;

	/* Takes the lock with the list of held locks held still. */
	pthread_mutex_lock(&os_lock_mutex);

	error = os_lock_acquire(entry, lock_path);

	pthread_mutex_unlock(&os_lock_mutex);

	/* Frees what the failed attempt allocated. */
	free(lock_path);
	if (error != 0) {
		free(entry->directory);
		free(entry);
		return error;
	}

	/* Hands the lock to the caller. */
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
	struct wb_os_lock **link;

	/* NULL holds nothing. */
	if (lock == NULL)
		return;

	/* Takes the lock off the list and closes the descriptor, which releases the system's lock, while the list is held still. */
	pthread_mutex_lock(&os_lock_mutex);

	for (link = &os_lock_held;
	     *link != NULL;
	     link = &(*link)->next) {
		if (*link == lock) {
			*link = lock->next;
			break;
		}
	}
	close(lock->descriptor);

	pthread_mutex_unlock(&os_lock_mutex);

	/* Frees the lock. */
	free(lock->directory);
	free(lock);
}

/*
 * Reports whether the bytes are well-formed UTF-8.
 *
 * The forms are those of the Unicode Standard: no overlong form, no
 * surrogate, nothing above U+10FFFF, no truncated sequence.
 */
static int
os_utf8_is_valid(
	const unsigned char *bytes,
	size_t length)
{
	size_t index;
	size_t count;
	size_t follow;
	unsigned int lead;
	unsigned int low;
	unsigned int high;

	/* Walks the sequences one lead byte at a time. */
	index = 0;
	while (index < length) {
		lead = bytes[index];

		/* An ASCII byte is a sequence of its own. */
		if (lead < 0x80U) {
			index++;
			continue;
		}

		/* Finds how many continuation bytes follow and what the first of them may be. */
		low = 0x80U;
		high = 0xbfU;
		if (lead >= 0xc2U && lead <= 0xdfU) {
			count = 1;
		} else if (lead >= 0xe0U && lead <= 0xefU) {
			count = 2;
			if (lead == 0xe0U)
				low = 0xa0U;
			if (lead == 0xedU)
				high = 0x9fU;
		} else if (lead >= 0xf0U && lead <= 0xf4U) {
			count = 3;
			if (lead == 0xf0U)
				low = 0x90U;
			if (lead == 0xf4U)
				high = 0x8fU;
		} else {
			/* A continuation byte, C0, C1 and F5 to FF never start a sequence. */
			return 0;
		}

		/* Refuses a sequence that the text ends in the middle of. */
		if (length - index - 1U < count)
			return 0;

		/* Checks the first continuation byte against its range. */
		if (bytes[index + 1U] < low || bytes[index + 1U] > high)
			return 0;

		/* Checks the other continuation bytes. */
		for (follow = 2;
		     follow <= count;
		     follow++) {
			if (bytes[index + follow] < 0x80U || bytes[index + follow] > 0xbfU)
				return 0;
		}
		index += count + 1U;
	}

	/* Every sequence was well-formed. */
	return 1;
}

/*
 * Checks a path argument and measures it.
 *
 * Returns EINVAL for NULL and the empty path, ENAMETOOLONG for one longer
 * than the layer accepts and EILSEQ for one that is not UTF-8.  The text is
 * never read past WB_OS_PATH_MAX + 1 bytes.
 */
static int
os_path_check(
	const char *path,
	size_t *length)
{
	size_t measured;
	int valid;

	/* Refuses NULL and the empty path. */
	if (path == NULL)
		return EINVAL;
	if (path[0] == '\0')
		return EINVAL;

	/* Measures the path, stopping one byte past the longest the layer accepts. */
	measured = strnlen(path, (size_t)WB_OS_PATH_MAX + 1U);
	if (measured > WB_OS_PATH_MAX)
		return ENAMETOOLONG;

	/* Refuses a path that is not text. */
	valid = os_utf8_is_valid((const unsigned char *)path, measured);
	if (valid == 0)
		return EILSEQ;

	/* Hands the length to the caller that asked for it. */
	if (length != NULL)
		*length = measured;

	/* Succeeded: the path is usable. */
	return 0;
}

/*
 * Maps an errno value of a system call to the one the layer returns.
 *
 * Values the layer does not name become EIO.
 */
static int
os_error_from_errno(
	int code)
{
	/* Sorts the system's reasons into the layer's. */
	switch (code) {
	case ENOENT:
	case ELOOP:
		/* The file is not there, or a chain of links never reaches one. */
		return ENOENT;
	case EACCES:
	case EPERM:
	case EROFS:
		/* The system refused access. */
		return EACCES;
	case EISDIR:
		/* The path names a directory where a file is needed. */
		return EISDIR;
	case ENOTDIR:
		/* A part of the path that must be a directory is not one. */
		return ENOTDIR;
	case EBUSY:
	case ETXTBSY:
		/* Another program holds the file. */
		return EBUSY;
	case ENOSPC:
#ifdef EDQUOT
	case EDQUOT:
#endif
		/* The disk or the user's quota is full. */
		return ENOSPC;
	case ENAMETOOLONG:
		/* A name is longer than the system accepts. */
		return ENAMETOOLONG;
	case ENOMEM:
		/* Memory ran out. */
		return ENOMEM;
	case EMFILE:
	case ENFILE:
		/* No descriptor is left. */
		return EMFILE;
	case EEXIST:
		/* The thing to create exists. */
		return EEXIST;
	default:
		break;
	}

	/* Any other failure of the system. */
	return EIO;
}

/*
 * Maps an errno value for the functions that work on files.
 *
 * Their contracts have no ENOTDIR: a path through something that is not a
 * directory names no file.
 */
static int
os_file_error_from_errno(
	int code)
{
	int error;

	/* A file inside a thing that is not a directory does not exist. */
	if (code == ENOTDIR)
		return ENOENT;

	/* Maps the rest as it is. */
	error = os_error_from_errno(code);
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
 * Opens a file, retrying when a signal interrupts the call.
 *
 * Returns 0 and the descriptor, or the layer's error value for the file
 * functions.
 */
static int
os_open(
	const char *path,
	int flags,
	mode_t mode,
	int *descriptor)
{
	int opened;

	/* Opens, again while signals cut the call short. */
	do {
		opened = open(path, flags, mode);
	} while (opened < 0 && errno == EINTR);
	if (opened < 0)
		return os_file_error_from_errno(errno);

	/* Hands the descriptor to the caller. */
	*descriptor = opened;

	/* Succeeded: the file is open. */
	return 0;
}

/*
 * Writes every byte to a descriptor, in pieces, retrying short writes.
 */
static int
os_write_all(
	int descriptor,
	const void *bytes,
	size_t length)
{
	const unsigned char *cursor;
	size_t left;
	size_t piece;
	ssize_t written;

	/* Writes until nothing is left. */
	cursor = (const unsigned char *)bytes;
	left = length;
	while (left > 0) {
		piece = left;
		if (piece > OS_WRITE_MAX)
			piece = OS_WRITE_MAX;

		/* Writes one piece. */
		written = write(descriptor, cursor, piece);
		if (written < 0) {
			/* A signal cut the call short; asks again. */
			if (errno == EINTR)
				continue;
			return os_file_error_from_errno(errno);
		}

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
 * Closes a descriptor and reports a failure of the late write-back.
 */
static int
os_close(
	int descriptor)
{
	int closed;

	/* Closes the descriptor; an interrupted close has still released it. */
	closed = close(descriptor);
	if (closed != 0 && errno != EINTR)
		return os_file_error_from_errno(errno);

	/* Succeeded: the descriptor is closed. */
	return 0;
}

/*
 * Flushes a file's data to the disk, retrying when a signal interrupts the call.
 */
static int
os_sync(
	int descriptor)
{
	int synced;

	/* Flushes, again while signals cut the call short. */
	do {
		synced = fsync(descriptor);
	} while (synced != 0 && errno == EINTR);
	if (synced != 0)
		return os_file_error_from_errno(errno);

	/* Succeeded: the data is on the disk. */
	return 0;
}

/*
 * Reads a descriptor to its end onto the end of a buffer.
 *
 * size_hint is the size the file claims (0 when unknown), used to make room
 * at once.  The buffer is left with the bytes read so far on a failure; the
 * caller cuts it back.
 */
static int
os_read_all(
	int descriptor,
	size_t size_hint,
	struct wb_buffer *buffer)
{
	size_t room;
	ssize_t got;
	int error;

	/* Makes room for the whole file at once when its size is known. */
	if (size_hint > 0) {
		if (size_hint > (size_t)-1 - OS_READ_MIN)
			return ENOMEM;
		error = wb_buffer_reserve(buffer, size_hint + OS_READ_MIN);
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
		got = read(descriptor, buffer->data + buffer->length, room);
		if (got < 0) {
			/* A signal cut the call short; asks again. */
			if (errno == EINTR)
				continue;
			return os_file_error_from_errno(errno);
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
 * Makes one directory, accepting one that exists.
 *
 * Returns ENOTDIR when the name exists and is not a directory.
 */
static int
os_ensure_directory(
	const char *path)
{
	struct stat status;
	int made;
	int found;
	int code;

	/* Makes the directory. */
	made = mkdir(path, OS_DIRECTORY_MODE);
	if (made == 0)
		return 0;
	code = errno;

	/* A directory that is there already is what was wanted, whatever the refusal. */
	found = stat(path, &status);
	if (found == 0 && S_ISDIR(status.st_mode))
		return 0;

	/* A name that exists and is no directory is in the way. */
	if (code == EEXIST)
		return ENOTDIR;

	/* Reports why the directory could not be made. */
	return os_error_from_errno(code);
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
 * Opens and locks the lock file, and puts the lock on the list.
 *
 * The caller holds os_lock_mutex.
 */
static int
os_lock_acquire(
	struct wb_os_lock *entry,
	const char *lock_path)
{
	struct wb_os_lock *other;
	int descriptor;
	int locked;
	int error;

	/* Refuses a directory this process holds already. */
	for (other = os_lock_held;
	     other != NULL;
	     other = other->next) {
		if (strcmp(other->directory, entry->directory) == 0)
			return EBUSY;
	}

	/* Opens the lock file, creating it private to the user. */
	error = os_open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, OS_PRIVATE_FILE_MODE, &descriptor);
	if (error != 0)
		return error;

	/* Takes the exclusive lock without waiting for another process to let go. */
	do {
		locked = flock(descriptor, LOCK_EX | LOCK_NB);
	} while (locked != 0 && errno == EINTR);
	if (locked != 0) {
		error = EIO;
		if (errno == EWOULDBLOCK)
			error = EBUSY;
		close(descriptor);
		return error;
	}

	/* Puts the lock on the list of those held. */
	entry->descriptor = descriptor;
	entry->next = os_lock_held;
	os_lock_held = entry;

	/* Succeeded: the lock is held. */
	return 0;
}
