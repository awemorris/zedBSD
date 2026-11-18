/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reading and saving files for Text Editor (plan/ws092/design.md
 * section 6).
 *
 * A file is read whole.  One with a NUL byte is not plain text and is
 * refused.  A UTF-8 byte order mark is taken off and put back when the
 * file is saved; so are CR LF line ends, when the file has more of them
 * than bare LFs (the text is edited with LF alone).  Bytes that are not
 * UTF-8 are kept as they are.
 *
 * A save writes the whole text to a new file next to the old one, flushes
 * it to the disk, gives it the old file's permissions and renames it over
 * the old one, so that a failure never leaves the file half written.
 */

#include "textedit.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The UTF-8 byte order mark. */
#define FILE_BOM		"\xef\xbb\xbf"
#define FILE_BOM_LENGTH		3U

/* How many bytes a save writes at a time. */
#define FILE_CHUNK		(64U * 1024U)

static int file_read_all(int descriptor, char *text, size_t length);
static void file_scan(char *text, size_t *length, struct te_file_info *info);
static int file_valid_utf8(const char *text, size_t length);
static int file_write_text(int descriptor, const struct te_buffer *buffer, const struct te_file_info *info);
static int file_write_run(int descriptor, char *chunk, size_t *used, const char *bytes, size_t length, int crlf);
static int file_flush(int descriptor, const char *bytes, size_t length);
static int file_temporary(const char *path, char *temporary, size_t size);

/*
 * Reads a whole file into allocated memory (the caller frees it) and says
 * how it was written.
 *
 * Returns 0; ENOENT when there is no such file (info->exists is then 0);
 * EFBIG when it is larger than TE_FILE_MAX; EILSEQ when it holds a NUL
 * byte; EISDIR for a folder; or another errno value.
 */
int
te_file_read(
	const char *path,
	char **text,
	size_t *length,
	struct te_file_info *info)
{
	struct stat status;
	const char *nul;
	char *bytes;
	size_t size;
	int descriptor;
	int error;
	int folder;

	/* Nothing known yet. */
	memset(info, 0, sizeof(*info));
	*text = NULL;
	*length = 0;

	/* The file. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return errno;

	/* What it is and how large. */
	error = fstat(descriptor, &status);
	if (error != 0) {
		error = errno;
		close(descriptor);
		return error;
	}

	/* A folder is not a text. */
	folder = S_ISDIR(status.st_mode);
	if (folder) {
		close(descriptor);
		return EISDIR;
	}

	/* A file too large to edit. */
	if (status.st_size < 0 || (unsigned long)status.st_size > TE_FILE_MAX) {
		close(descriptor);
		return EFBIG;
	}

	/* Room for all of it (at least a byte, so that NULL means failure). */
	size = (size_t)status.st_size;
	bytes = malloc(size + 1U);
	if (bytes == NULL) {
		close(descriptor);
		return ENOMEM;
	}

	/* All of it. */
	error = file_read_all(descriptor, bytes, size);
	close(descriptor);
	if (error != 0) {
		free(bytes);
		return error;
	}

	/* A NUL byte says it is not plain text. */
	nul = memchr(bytes, '\0', size);
	if (nul != NULL) {
		free(bytes);
		return EILSEQ;
	}

	/* The byte order mark and CR LF come off; what was found is noted. */
	file_scan(bytes, &size, info);
	info->exists = 1;
	info->mode = status.st_mode & 07777;
	info->mtime = status.st_mtime;
	info->size = status.st_size;

	/* Succeeded: the text. */
	*text = bytes;
	*length = size;
	return 0;
}

/*
 * Saves a document's text to a path, the way the file was (info), and
 * notes the file's new time and size in info.
 *
 * Returns 0, or an errno value (the old file is then as it was).
 */
int
te_file_write(
	const char *path,
	const struct te_buffer *buffer,
	struct te_file_info *info)
{
	char temporary[TE_PATH_MAX + 64];
	char resolved[PATH_MAX];
	const char *target;
	struct stat status;
	char *real;
	int descriptor;
	int error;
	int closed;

	/* A link is saved through: the file it names is replaced, not the link. */
	target = path;
	real = realpath(path, resolved);
	if (real != NULL)
		target = resolved;

	/* The new file next to the target. */
	error = file_temporary(target, temporary, sizeof(temporary));
	if (error != 0)
		return error;
	descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
	if (descriptor < 0)
		return errno;

	/* The text, then the disk. */
	error = file_write_text(descriptor, buffer, info);
	if (error == 0) {
		error = fsync(descriptor);
		if (error != 0)
			error = errno;
	}

	/* The old file's permissions, when there was one. */
	if (error == 0 && info->exists) {
		error = fchmod(descriptor, info->mode);
		if (error != 0)
			error = errno;
	}

	/* The new file is complete. */
	closed = close(descriptor);
	if (closed != 0 && error == 0)
		error = errno;
	if (error != 0) {
		(void)unlink(temporary);
		return error;
	}

	/* It takes the old one's place. */
	error = rename(temporary, target);
	if (error != 0) {
		error = errno;
		(void)unlink(temporary);
		return error;
	}

	/* What the file is now, so that a change on the disk can be told later. */
	error = stat(target, &status);
	if (error == 0) {
		info->mtime = status.st_mtime;
		info->size = status.st_size;
		info->mode = status.st_mode & 07777;
	}

	/* Succeeded: the file holds the text. */
	info->exists = 1;
	return 0;
}

/*
 * Reports whether a file changed on the disk since it was read or saved
 * (its time or size differ, or it went away).
 */
int
te_file_changed(
	const char *path,
	const struct te_file_info *info)
{
	struct stat status;
	int error;

	/* A file that did not exist has nothing to compare. */
	if (!info->exists)
		return 0;

	/* A file that went away changed. */
	error = stat(path, &status);
	if (error != 0)
		return 1;

	/* Another time or size. */
	if (status.st_mtime != info->mtime)
		return 1;
	if (status.st_size != info->size)
		return 1;

	/* The same file. */
	return 0;
}

/* Reads exactly a length of bytes from a descriptor; returns 0 or an errno value. */
static int
file_read_all(
	int descriptor,
	char *text,
	size_t length)
{
	size_t done;
	ssize_t count;

	/* Until all of it has come. */
	done = 0;
	while (done < length) {
		count = read(descriptor, text + done, length - done);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			return errno;

		/* A file that shrank while it was read. */
		if (count == 0)
			return EIO;
		done += (size_t)count;
	}

	/* Succeeded: all of it. */
	return 0;
}

/*
 * Takes the byte order mark and the CR of CR LF line ends off a text read
 * (when CR LF is the file's line end), in place, and notes what it found.
 */
static void
file_scan(
	char *text,
	size_t *length,
	struct te_file_info *info)
{
	size_t crlf;
	size_t lf;
	size_t from;
	size_t to;
	int mark;

	/* The byte order mark. */
	mark = 1;
	if (*length >= FILE_BOM_LENGTH)
		mark = memcmp(text, FILE_BOM, FILE_BOM_LENGTH);
	if (mark == 0) {
		memmove(text, text + FILE_BOM_LENGTH, *length - FILE_BOM_LENGTH);
		*length -= FILE_BOM_LENGTH;
		info->bom = 1;
	}

	/* How the lines end: CR LF, or LF alone. */
	crlf = 0;
	lf = 0;
	for (from = 0; from < *length; from++) {
		if (text[from] != '\n')
			continue;
		if (from > 0U && text[from - 1U] == '\r') {
			crlf++;
		} else {
			lf++;
		}
	}

	/* A file of CR LF lines is edited with LF alone. */
	if (crlf > lf) {
		info->crlf = 1;
		to = 0;
		for (from = 0; from < *length; from++) {
			if (text[from] == '\r' && from + 1U < *length && text[from + 1U] == '\n')
				continue;
			text[to] = text[from];
			to++;
		}

		/* The text is shorter by the CRs. */
		*length = to;
	}

	/* Whether some bytes are not UTF-8. */
	info->invalid = !file_valid_utf8(text, *length);
}

/* Reports whether a text is well-formed UTF-8. */
static int
file_valid_utf8(
	const char *text,
	size_t length)
{
	uint32_t codepoint;
	size_t index;
	size_t before;

	/* Each character; U+FFFD from bytes other than its own encoding is a malformed byte. */
	index = 0;
	while (index < length) {
		before = index;
		codepoint = te_utf8_next(text, length, &index);
		if (codepoint != 0xfffdU)
			continue;
		if (index - before != 3U)
			return 0;
	}

	/* Every byte is part of a character. */
	return 1;
}

/* Writes a document's text as the file was written: with its mark and its line ends. */
static int
file_write_text(
	int descriptor,
	const struct te_buffer *buffer,
	const struct te_file_info *info)
{
	const char *first;
	const char *second;
	size_t first_length;
	size_t second_length;
	size_t used;
	char *chunk;
	int error;

	/* The staging chunk. */
	chunk = malloc(FILE_CHUNK);
	if (chunk == NULL)
		return ENOMEM;
	used = 0;

	/* The byte order mark first, when the file had one. */
	error = 0;
	if (info->bom) {
		memcpy(chunk, FILE_BOM, FILE_BOM_LENGTH);
		used = FILE_BOM_LENGTH;
	}

	/* The text's two runs, each LF made CR LF when the file had those. */
	te_buffer_segments(buffer, &first, &first_length, &second, &second_length);
	error = file_write_run(descriptor, chunk, &used, first, first_length, info->crlf);
	if (error == 0)
		error = file_write_run(descriptor, chunk, &used, second, second_length, info->crlf);

	/* What is left in the chunk. */
	if (error == 0)
		error = file_flush(descriptor, chunk, used);
	free(chunk);

	/* Reports a failed write. */
	if (error != 0)
		return error;

	/* Succeeded: the text is written. */
	return 0;
}

/* Adds bytes to the staging chunk, writing it out whenever it fills. */
static int
file_write_run(
	int descriptor,
	char *chunk,
	size_t *used,
	const char *bytes,
	size_t length,
	int crlf)
{
	size_t index;
	int error;

	/* Each byte, with a CR before each LF for a CR LF file. */
	for (index = 0; index < length; index++) {
		/* A full chunk goes out first (room for two bytes). */
		if (*used + 2U > FILE_CHUNK) {
			error = file_flush(descriptor, chunk, *used);
			if (error != 0)
				return error;
			*used = 0;
		}

		/* The CR of a CR LF line end. */
		if (crlf && bytes[index] == '\n') {
			chunk[*used] = '\r';
			(*used)++;
		}

		/* The byte. */
		chunk[*used] = bytes[index];
		(*used)++;
	}

	/* Succeeded: the bytes are staged or written. */
	return 0;
}

/* Writes bytes to a descriptor in full; returns 0 or an errno value. */
static int
file_flush(
	int descriptor,
	const char *bytes,
	size_t length)
{
	size_t done;
	ssize_t count;

	/* Until all of them are written. */
	done = 0;
	while (done < length) {
		count = write(descriptor, bytes + done, length - done);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			return errno;
		if (count == 0)
			return EIO;

		/* The part written. */
		done += (size_t)count;
	}

	/* Succeeded: all written. */
	return 0;
}

/* Makes the name of the new file a save writes: ".NAME.textedit-PID" in the target's folder. */
static int
file_temporary(
	const char *path,
	char *temporary,
	size_t size)
{
	const char *slash;
	int written;

	/* The folder part and the name. */
	slash = strrchr(path, '/');
	if (slash == NULL) {
		written = snprintf(temporary, size, ".%s.textedit-%ld", path, (long)getpid());
	} else {
		written = snprintf(temporary, size, "%.*s.%s.textedit-%ld", (int)(slash - path + 1), path, slash + 1, (long)getpid());
	}

	/* A name that does not fit. */
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the name. */
	return 0;
}
