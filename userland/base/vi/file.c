/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reading a file into the buffer and writing the buffer back safely.
 */

#include "userland/base/vi/vi.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * The longest file name that the new file next to it repeats; a longer one
 * gets a short new name, so the new name stays within NAME_MAX.
 */
#define FILE_TEMPORARY_NAME_LIMIT 200U

/* The size of the blocks the buffer is written out in. */
#define FILE_WRITE_BLOCK 65536U

/*
 * The count of what a write put in the file, for the message after it.
 */
struct file_totals {
	size_t lines;
	unsigned long bytes;
};

static int read_whole(int descriptor, size_t size, char **contents, size_t *length);
static int split_lines(struct vi_buffer *buffer, const char *contents, size_t length);
static int write_lines(int descriptor, const struct vi_buffer *buffer, struct file_totals *totals);
static int write_all(int descriptor, const char *bytes, size_t length);
static int write_replacing(const char *target, const struct stat *existing, int exists, const struct vi_buffer *buffer, struct file_totals *totals);
static int write_temporary(int descriptor, const struct stat *existing, int exists, const struct vi_buffer *buffer, struct file_totals *totals);
static int write_in_place(const char *target, const struct vi_buffer *buffer, struct file_totals *totals);
static int resolve_target(const char *path, char **target);
static int keeps_identity(const struct stat *existing);
static int dangling_target(const char *path, char **target);
static void sync_directory(const char *path, size_t directory_length);

/*
 * Reads a file into the buffer, replacing what the buffer held.
 *
 * A file that does not exist gives an empty buffer to start a new file
 * in.  On failure the buffer is unchanged and the message says why.
 * Returns 0 or an errno value.
 */
int
vi_file_load(
	struct vi_editor *editor,
	const char *path)
{
	struct vi_buffer buffer;
	struct stat status;
	char *contents;
	size_t length;
	int descriptor;
	int incomplete;
	int is_regular;
	int error;
	int result;

	/* The new lines are read aside, so a failure leaves the buffer alone. */
	vi_buffer_init(&buffer);
	contents = NULL;
	length = 0;
	incomplete = 0;

	/* Opens the file; a missing one starts a new file. */
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0) {
		/* Any failure but a missing file is reported. */
		error = errno;
		if (error != ENOENT) {
			(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": %s", path, strerror(error));
			return error;
		}
	}

	/* Reads an existing file whole. */
	if (descriptor >= 0) {
		/* Learns what the name refers to and how large it is. */
		result = fstat(descriptor, &status);
		if (result != 0) {
			error = errno;
			(void)close(descriptor);
			(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": %s", path, strerror(error));
			return error;
		}

		/* Only a regular file is edited; a directory or a device is refused. */
		is_regular = S_ISREG(status.st_mode);
		if (!is_regular) {
			(void)close(descriptor);
			(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": not a regular file", path);
			return EINVAL;
		}

		/* A file over the limit is refused before it is read. */
		if ((unsigned long)status.st_size > VI_FILE_SIZE_LIMIT) {
			(void)close(descriptor);
			(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": file too large", path);
			return EFBIG;
		}

		/* Reads the bytes of the file. */
		error = read_whole(descriptor, (size_t)status.st_size, &contents, &length);
		(void)close(descriptor);
		if (error != 0) {
			/* Says that the file was too large, or why the read failed. */
			if (error == EFBIG) {
				(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": file too large", path);
			} else {
				(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": %s", path, strerror(error));
			}

			/* Reports why the file could not be read. */
			return error;
		}

		/* A last line without a newline is noted; writing adds the newline. */
		if (length != 0) {
			if (contents[length - 1] != '\n')
				incomplete = 1;
		}
	}

	/* Cuts the bytes into lines; an empty or new file is one empty line. */
	error = split_lines(&buffer, contents, length);
	free(contents);
	if (error != 0) {
		/* Gives back the lines made so far and says why the file could not be held. */
		vi_buffer_free(&buffer);
		if (error == EFBIG) {
			(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": file too large", path);
		} else {
			(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": %s", path, strerror(error));
		}

		/* Reports why the lines could not be held. */
		return error;
	}

	/* Replaces the buffer and forgets the history of the old one. */
	vi_buffer_free(&editor->buffer);
	editor->buffer = buffer;
	vi_undo_free(&editor->undo);
	editor->cursor.line = 0;
	editor->cursor.offset = 0;
	editor->want_column = 0;
	editor->top = 0;
	editor->top_skip = 0;
	vi_screen_invalidate(editor);

	/* Tells what was read. */
	if (descriptor < 0) {
		(void)snprintf(editor->message, sizeof(editor->message), "\"%s\" [New file]", path);
	} else if (incomplete) {
		(void)snprintf(
			editor->message,
			sizeof(editor->message),
			"\"%s\" %lu lines, %lu bytes [Incomplete last line]",
			path,
			(unsigned long)editor->buffer.count,
			(unsigned long)length);
	} else {
		(void)snprintf(
			editor->message,
			sizeof(editor->message),
			"\"%s\" %lu lines, %lu bytes",
			path,
			(unsigned long)editor->buffer.count,
			(unsigned long)length);
	}

	/* Succeeded: the buffer holds the file. */
	return 0;
}

/*
 * Writes the buffer to a file.
 *
 * With no path the buffer's own file is written.  Another file that
 * exists, or a file that is not writable, is refused unless force is set.
 * The file is replaced through a new file in the same directory, so a
 * failed write leaves the old contents whole.  Returns 0 or an errno value,
 * with the message saying what happened.
 */
int
vi_file_write(
	struct vi_editor *editor,
	const char *path,
	int force)
{
	struct file_totals totals;
	struct stat existing;
	char *target;
	char *name_copy;
	int is_own_file;
	int exists;
	int refused;
	int blocked;
	int in_place;
	int is_regular;
	int result;
	int error;

	/* Uses the buffer's own name when no other is given. */
	if (path == NULL)
		path = editor->file_name;

	/* A buffer with no name of its own needs one to be written. */
	if (path == NULL) {
		vi_message(editor, "No file name");
		return EINVAL;
	}

	/* Tells whether this is the file the buffer came from. */
	is_own_file = 0;
	if (editor->file_name != NULL) {
		result = strcmp(path, editor->file_name);
		if (result == 0)
			is_own_file = 1;
	}

	/* Follows a symbolic link to the file it names, which is the file written. */
	target = NULL;
	error = resolve_target(path, &target);
	if (error != 0) {
		(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": %s", path, strerror(error));
		return error;
	}

	/* Learns whether the file exists and what it is. */
	exists = 0;
	result = stat(target, &existing);
	if (result == 0) {
		exists = 1;
	} else if (errno != ENOENT) {
		error = errno;
		(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": %s", path, strerror(error));
		free(target);
		return error;
	}

	/* An existing file must be a regular file. */
	if (exists) {
		is_regular = S_ISREG(existing.st_mode);
		if (!is_regular) {
			(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": not a regular file", path);
			free(target);
			return EINVAL;
		}
	}

	/* Another existing file is overwritten only when asked with !. */
	refused = 0;
	if (exists && !is_own_file) {
		if (!force)
			refused = 1;
	}

	/* Reports the refusal to overwrite another file. */
	if (refused) {
		(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": file exists (add ! to override)", path);
		free(target);
		return EEXIST;
	}

	/* A file without write permission is tried only when asked with !. */
	if (exists && !force) {
		result = access(target, W_OK);
		if (result != 0) {
			(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": file is read-only (add ! to override)", path);
			free(target);
			return EACCES;
		}
	}

	/*
	 * A file is replaced whole through a new file, unless replacing would
	 * change it for others: a file with other hard links, or one whose
	 * owner or group vi could not give back, is written where it is.
	 */
	memset(&totals, 0, sizeof(totals));
	in_place = 0;
	if (exists) {
		in_place = keeps_identity(&existing);
	}

	/* Writes the file where it is, or through a new file that replaces it. */
	if (in_place) {
		error = write_in_place(target, &editor->buffer, &totals);
	} else {
		error = write_replacing(target, &existing, exists, &editor->buffer, &totals);

		/* A directory vi cannot add a file to, or rename in, still lets the file itself be written. */
		blocked = 0;
		if (error == EACCES)
			blocked = 1;
		else if (error == EROFS)
			blocked = 1;
		else if (error == EPERM)
			blocked = 1;
		else if (error == EXDEV)
			blocked = 1;
		if (exists && blocked) {
			memset(&totals, 0, sizeof(totals));
			error = write_in_place(target, &editor->buffer, &totals);
		}
	}

	/* The target name is no longer needed. */
	free(target);

	/* Reports a failed write; the message names the file as it was typed. */
	if (error != 0) {
		(void)snprintf(editor->message, sizeof(editor->message), "\"%s\": write failed: %s", path, strerror(error));
		return error;
	}

	/* A buffer without a name takes the name it was written under. */
	if (editor->file_name == NULL) {
		name_copy = strdup(path);
		if (name_copy != NULL) {
			editor->file_name = name_copy;
			is_own_file = 1;
		}
	}

	/* The buffer now matches its file. */
	if (is_own_file)
		vi_undo_mark_saved(editor);

	/* Tells what was written. */
	(void)snprintf(
		editor->message,
		sizeof(editor->message),
		"\"%s\" %lu lines, %lu bytes written",
		path,
		(unsigned long)totals.lines,
		totals.bytes);

	/* Succeeded. */
	return 0;
}

/*
 * Keeps the buffer of a session that ended with unwritten changes in
 * $HOME/.vi-recover.NAME.PID, made new with only the user able to read it,
 * and says so on the standard error.  Without HOME nothing is kept.
 */
void
vi_file_recover(
	struct vi_editor *editor)
{
	struct file_totals totals;
	const char *home;
	const char *name;
	const char *slash;
	char path[PATH_MAX];
	int length;
	int descriptor;
	int result;
	int error;

	/* The file goes in the home directory, never in a directory others can write. */
	home = getenv("HOME");
	if (home == NULL || home[0] == '\0')
		return;

	/* Names it after the file being edited and this process. */
	name = "noname";
	if (editor->file_name != NULL) {
		name = editor->file_name;
		slash = strrchr(name, '/');
		if (slash != NULL)
			name = slash + 1;
	}

	/* Builds the path of the recovery file; a path that does not fit keeps nothing. */
	length = snprintf(path, sizeof(path), "%s/.vi-recover.%s.%ld", home, name, (long)getpid());
	if (length < 0 || (size_t)length >= sizeof(path))
		return;

	/* Creates the file new, readable by the user alone. */
	descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (descriptor < 0)
		return;

	/* Writes the lines and puts them on the disk. */
	memset(&totals, 0, sizeof(totals));
	error = write_lines(descriptor, &editor->buffer, &totals);
	if (error == 0) {
		result = fsync(descriptor);
		if (result != 0)
			error = errno;
	}

	/* The file is complete, or as complete as it will get. */
	(void)close(descriptor);

	/* Tells where the changes are, if the standard error still goes anywhere. */
	if (error == 0)
		(void)fprintf(stderr, "vi: unwritten changes saved in %s\n", path);
}

/* Reads a whole file of about the given size into one allocation. */
static int
read_whole(
	int descriptor,
	size_t size,
	char **contents,
	size_t *length)
{
	char *data;
	char *grown;
	size_t capacity;
	size_t used;
	ssize_t count;

	/* Starts with room for the size the file had, plus a little for growth. */
	capacity = size + 4096;
	data = malloc(capacity);
	if (data == NULL)
		return ENOMEM;

	/* Reads until the end of the file, growing if the file grows. */
	used = 0;
	for (;;) {
		/* Grows the storage when it is full, within the limit. */
		if (used == capacity) {
			if (capacity > VI_FILE_SIZE_LIMIT) {
				free(data);
				return EFBIG;
			}

			/* Doubles the storage and moves the bytes read so far into it. */
			capacity = capacity * 2;
			grown = realloc(data, capacity);
			if (grown == NULL) {
				free(data);
				return ENOMEM;
			}

			/* Keeps the larger storage. */
			data = grown;
		}

		/* Reads the next part of the file. */
		count = read(descriptor, data + used, capacity - used);
		if (count < 0) {
			/* A signal only interrupts the read. */
			if (errno == EINTR)
				continue;
			free(data);
			return errno;
		}

		/* The end of the file ends the reading. */
		if (count == 0)
			break;

		/* Counts the bytes taken in. */
		used += (size_t)count;
	}

	/* A file that grew past the limit while it was read is refused too. */
	if (used > VI_FILE_SIZE_LIMIT) {
		free(data);
		return EFBIG;
	}

	/* Hands the bytes to the caller. */
	*contents = data;
	*length = used;

	/* Succeeded: the caller owns the bytes. */
	return 0;
}

/* Adds the lines of a file's bytes to an empty buffer. */
static int
split_lines(
	struct vi_buffer *buffer,
	const char *contents,
	size_t length)
{
	size_t start;
	size_t index;
	int error;

	/* Adds each line that a newline ends. */
	start = 0;
	for (index = 0; index < length; index++) {
		if (contents[index] == '\n') {
			error = vi_buffer_append_line(buffer, contents + start, index - start);
			if (error != 0)
				return error;
			start = index + 1;
		}
	}

	/* Adds a last line that has no newline. */
	if (start < length) {
		error = vi_buffer_append_line(buffer, contents + start, length - start);
		if (error != 0)
			return error;
	}

	/* An empty file is one empty line. */
	if (buffer->count == 0) {
		error = vi_buffer_append_line(buffer, "", 0);
		if (error != 0)
			return error;
	}

	/* Succeeded. */
	return 0;
}

/* Writes every line with its newline to a descriptor, counting what was written. */
static int
write_lines(
	int descriptor,
	const struct vi_buffer *buffer,
	struct file_totals *totals)
{
	char *block;
	size_t used;
	size_t index;
	const struct vi_line *line;
	int error;

	/* Lines are gathered into large blocks so a big file takes few writes. */
	block = malloc(FILE_WRITE_BLOCK);
	if (block == NULL)
		return ENOMEM;

	/* Writes each line and the newline after it. */
	used = 0;
	for (index = 0; index < buffer->count; index++) {
		line = &buffer->lines[index];

		/* A line that does not fit flushes the block first; a long line goes straight out. */
		if (used + line->length + 1 > FILE_WRITE_BLOCK) {
			error = write_all(descriptor, block, used);
			if (error != 0) {
				free(block);
				return error;
			}

			/* The block is empty again. */
			used = 0;
		}

		/* A line too long for a block goes straight out; any other joins the block. */
		if (line->length + 1 > FILE_WRITE_BLOCK) {
			error = write_all(descriptor, line->text, line->length);
			if (error != 0) {
				free(block);
				return error;
			}
		} else {
			memcpy(block + used, line->text, line->length);
			used += line->length;
		}

		/* Every line ends with a newline, the last one included. */
		block[used] = '\n';
		used++;
		totals->lines++;
		totals->bytes += (unsigned long)line->length + 1;
	}

	/* Writes what is left in the block. */
	error = write_all(descriptor, block, used);
	free(block);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Writes all of the bytes, resuming after a signal or a short write. */
static int
write_all(
	int descriptor,
	const char *bytes,
	size_t length)
{
	ssize_t count;

	/* Writes until every byte is out. */
	while (length != 0) {
		count = write(descriptor, bytes, length);
		if (count < 0) {
			/* A signal only interrupts the write. */
			if (errno == EINTR)
				continue;
			return errno;
		}

		/* Moves past the bytes the write took. */
		bytes += count;
		length -= (size_t)count;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Writes the buffer to a new file next to the target and renames it over
 * the target, keeping the target's permissions and owner.  The target is
 * untouched unless the whole write succeeded.
 */
static int
write_replacing(
	const char *target,
	const struct stat *existing,
	int exists,
	const struct vi_buffer *buffer,
	struct file_totals *totals)
{
	char *temporary;
	const char *slash;
	size_t directory_length;
	size_t name_length;
	size_t size;
	int descriptor;
	int result;
	int error;

	/* The new file is .NAME.vi-XXXXXX in the directory of the target, or .vi-XXXXXX for a long name. */
	slash = strrchr(target, '/');
	directory_length = 0;
	if (slash != NULL)
		directory_length = (size_t)(slash - target) + 1;
	size = strlen(target) + 16;
	temporary = malloc(size);
	if (temporary == NULL)
		return ENOMEM;

	/* Names the new file: the target's directory, then a hidden name made from the target's. */
	memcpy(temporary, target, directory_length);
	name_length = strlen(target + directory_length);
	if (name_length > FILE_TEMPORARY_NAME_LIMIT) {
		(void)snprintf(temporary + directory_length, size - directory_length, ".vi-XXXXXX");
	} else {
		(void)snprintf(temporary + directory_length, size - directory_length, ".%s.vi-XXXXXX", target + directory_length);
	}

	/* Creates the new file. */
	descriptor = mkstemp(temporary);
	if (descriptor < 0) {
		error = errno;
		free(temporary);
		return error;
	}

	/* Fills the new file and puts it on the disk; it is closed whatever the outcome. */
	error = write_temporary(descriptor, existing, exists, buffer, totals);
	if (error != 0) {
		/* Throws the new file away, leaving the target as it was. */
		(void)unlink(temporary);
		free(temporary);
		return error;
	}

	/* Replaces the target with the new file in one step. */
	result = rename(temporary, target);
	if (result != 0) {
		/* Throws the new file away, leaving the target as it was. */
		error = errno;
		(void)unlink(temporary);
		free(temporary);
		return error;
	}

	/* Puts the new name on the disk too. */
	sync_directory(temporary, directory_length);
	free(temporary);

	/* Succeeded: the target holds the buffer. */
	return 0;
}

/*
 * Fills a new file with the buffer, gives it the target's owner and
 * permissions, puts it on the disk and closes it.
 *
 * The descriptor is closed whether this succeeds or not.
 */
static int
write_temporary(
	int descriptor,
	const struct stat *existing,
	int exists,
	const struct vi_buffer *buffer,
	struct file_totals *totals)
{
	mode_t mode;
	mode_t mask;
	int result;
	int error;

	/* Writes the lines. */
	error = write_lines(descriptor, buffer, totals);
	if (error != 0) {
		(void)close(descriptor);
		return error;
	}

	/*
	 * An existing file keeps its owner and permissions.  The owner comes
	 * first, because changing the owner clears the set-ID bits.
	 */
	if (exists) {
		result = fchown(descriptor, existing->st_uid, existing->st_gid);
		if (result != 0) {
			error = errno;
			(void)close(descriptor);
			return error;
		}

		/* Then the permissions, set-ID bits included. */
		mode = existing->st_mode & 07777;
		result = fchmod(descriptor, mode);
		if (result != 0) {
			error = errno;
			(void)close(descriptor);
			return error;
		}
	} else {
		/* A new file gets the permissions any new file would, after the umask. */
		mask = umask(0);
		(void)umask(mask);
		result = fchmod(descriptor, 0666 & ~mask);
		if (result != 0) {
			error = errno;
			(void)close(descriptor);
			return error;
		}
	}

	/* Puts the contents on the disk before the name points at them. */
	result = fsync(descriptor);
	if (result != 0) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* Closes the new file; a failed close can mean a failed write. */
	result = close(descriptor);
	if (result != 0)
		return errno;

	/* Succeeded: the new file holds the buffer on the disk. */
	return 0;
}

/*
 * Writes the buffer over the target itself, keeping its inode, its links,
 * its owner and its permissions.
 *
 * The storage for a longer file is claimed before anything is written,
 * so a full disk is found while the old contents are still whole.
 */
static int
write_in_place(
	const char *target,
	const struct vi_buffer *buffer,
	struct file_totals *totals)
{
	struct stat status;
	off_t old_size;
	off_t new_size;
	int descriptor;
	int result;
	int error;

	/* Opens the file without emptying it. */
	descriptor = open(target, O_WRONLY);
	if (descriptor < 0)
		return errno;

	/* Learns how large the file is now. */
	result = fstat(descriptor, &status);
	if (result != 0) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* Compares the size the file has with the size the buffer needs. */
	old_size = status.st_size;
	new_size = (off_t)buffer->bytes;

	/* A longer file claims its storage first; a failure puts the old size back. */
	if (new_size > old_size) {
		error = posix_fallocate(descriptor, old_size, new_size - old_size);
		if (error != 0) {
			(void)ftruncate(descriptor, old_size);
			(void)close(descriptor);
			return error;
		}
	}

	/* Writes the lines from the start. */
	error = write_lines(descriptor, buffer, totals);
	if (error != 0) {
		(void)close(descriptor);
		return error;
	}

	/* Cuts off what is left of a longer old file. */
	result = ftruncate(descriptor, new_size);
	if (result != 0) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* Puts the contents on the disk. */
	result = fsync(descriptor);
	if (result != 0) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* Closes the file; a failed close can mean a failed write. */
	result = close(descriptor);
	if (result != 0)
		return errno;

	/* Succeeded. */
	return 0;
}

/*
 * Tells whether replacing a file with a new one would change who owns it,
 * which only the superuser can put back.
 */
static int
keeps_identity(
	const struct stat *existing)
{
	uid_t user;
	gid_t group;

	/* Other names for the same file would keep the old contents. */
	if (existing->st_nlink > 1)
		return 1;

	/* The superuser can give the new file any owner. */
	user = geteuid();
	if (user == 0)
		return 0;

	/* Anyone else makes a new file that is theirs, in their own group. */
	group = getegid();
	if (existing->st_uid != user)
		return 1;
	if (existing->st_gid != group)
		return 1;

	/* The new file can have the same owner and group. */
	return 0;
}

/* Puts a directory's entries on the disk, after a rename in it. */
static void
sync_directory(
	const char *path,
	size_t directory_length)
{
	char *directory;
	int descriptor;

	/* The directory is the part of the path before the name, or the current one. */
	directory = malloc(directory_length + 2);
	if (directory == NULL)
		return;
	if (directory_length == 0) {
		directory[0] = '.';
		directory[1] = '\0';
	} else {
		memcpy(directory, path, directory_length);
		directory[directory_length] = '\0';
	}

	/* Opens and syncs the directory; a failure here only weakens the guarantee. */
	descriptor = open(directory, O_RDONLY);
	free(directory);
	if (descriptor < 0)
		return;
	(void)fsync(descriptor);
	(void)close(descriptor);
}

/*
 * Finds the file a name stands for: the name itself, or the file a
 * symbolic link points to, so that writing replaces the file rather than
 * the link.
 */
static int
resolve_target(
	const char *path,
	char **target)
{
	struct stat link_status;
	char *resolved;
	int is_link;
	int result;
	int error;

	/* A name that is not a symbolic link is its own target. */
	is_link = 0;
	result = lstat(path, &link_status);
	if (result == 0)
		is_link = S_ISLNK(link_status.st_mode);
	if (!is_link) {
		*target = strdup(path);
		if (*target == NULL)
			return ENOMEM;

		/* Succeeded: the name itself is the target. */
		return 0;
	}

	/* Follows every link in the name to the real file. */
	resolved = realpath(path, NULL);
	if (resolved == NULL) {
		/* A link to a file that does not exist yet names the file to create. */
		error = errno;
		if (error != ENOENT)
			return error;
		error = dangling_target(path, target);
		if (error != 0)
			return error;

		/* Succeeded: the target is the file the link will create. */
		return 0;
	}

	/* Hands the real file's name to the caller. */
	*target = resolved;

	/* Succeeded: the caller owns the target name. */
	return 0;
}

/*
 * Finds the file a symbolic link names when that file does not exist:
 * the link's text, taken from the link's directory when it is relative.
 */
static int
dangling_target(
	const char *path,
	char **target)
{
	char link[PATH_MAX];
	const char *slash;
	size_t directory_length;
	size_t link_length;
	ssize_t count;
	char *joined;

	/* Reads what the link says. */
	count = readlink(path, link, sizeof(link) - 1);
	if (count < 0)
		return errno;
	link[count] = '\0';
	link_length = (size_t)count;

	/* A relative link is relative to the directory the link is in. */
	directory_length = 0;
	if (link[0] != '/') {
		slash = strrchr(path, '/');
		if (slash != NULL)
			directory_length = (size_t)(slash - path) + 1;
	}

	/* Joins the directory and the link's text. */
	joined = malloc(directory_length + link_length + 1);
	if (joined == NULL)
		return ENOMEM;
	memcpy(joined, path, directory_length);
	memcpy(joined + directory_length, link, link_length + 1);
	*target = joined;

	/* Succeeded: the caller owns the name. */
	return 0;
}
