/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What Image Viewer shares with Files (ws128-p005): moving the image shown
 * to the trash, and the applications that open it.
 *
 * Both are Files' own code, built into the viewer from Files' sources, so
 * that the two follow one set of rules: the trash is the freedesktop.org
 * Trash with a volume's own trash for an image on another file system
 * (files/trash.c), and the applications are the ones Files offers for the
 * image's type, its chosen default first (files/apps.c).  Nothing here
 * draws or knows about the window.
 */

#include "imageview.h"
#include "userland/desktop/files/files.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* How many bytes one step of a copy into a trash on another file system moves. */
#define SHARE_COPY_CHUNK	65536U

static int share_ways(const char *path, const char *format, struct fm_opener *openers);
static int share_trash_place(const char *path, const struct stat *status, char *trashed, size_t size);
static int share_copy(const char *source, const char *target, mode_t mode);
static int share_copy_bytes(int in, int out);
static const char *share_type(const char *format);
static const char *share_base(const char *path);

/*
 * Attaches the application and the window the applications that open an
 * image are started from.
 */
void
iv_share_attach(
	struct kl_app *app,
	struct kl_window *window)
{
	/* Files' starts, from the viewer's application and window. */
	fm_apps_attach(app, window);
}

/*
 * Moves a file to its trash (the home trash, or its volume's), with the
 * record of where it was, and writes where it went in the trash.
 *
 * Returns 0, or an errno value with the file where it was.
 */
int
iv_share_trash(
	const char *path,
	char *trashed,
	size_t size)
{
	struct stat status;
	int error;

	/* Only a file that is there. */
	error = lstat(path, &status);
	if (error != 0)
		return errno;

	/* The file into its trash. */
	error = share_trash_place(path, &status, trashed, size);
	if (error != 0)
		return error;

	/* Succeeded: the file is in the trash. */
	return 0;
}

/*
 * Lists the names of the applications that open an image of a format
 * ("PNG", "JPEG" or "GIF"), as Files offers them, the default first.
 *
 * Returns how many there are (at most capacity).
 */
int
iv_share_openers(
	const char *path,
	const char *format,
	char names[][IV_OPENER_NAME],
	int capacity)
{
	struct fm_opener openers[FM_OPENERS];
	size_t length;
	int count;
	int index;

	/* The ways Files offers for the image's type. */
	count = share_ways(path, format, openers);
	if (count > capacity)
		count = capacity;

	/* Their names, for the menu (cut to the menu's length). */
	for (index = 0; index < count; index++) {
		length = strlen(openers[index].name);
		if (length >= IV_OPENER_NAME)
			length = IV_OPENER_NAME - 1;
		memcpy(names[index], openers[index].name, length);
		names[index][length] = '\0';
	}

	/* Reports how many there are. */
	return count;
}

/*
 * Opens an image in the application at a place of iv_share_openers' list.
 *
 * Returns 0, ENOENT when there is no such application, or the errno value
 * of a start that failed.
 */
int
iv_share_open_with(
	const char *path,
	const char *format,
	int index)
{
	struct fm_opener openers[FM_OPENERS];
	int count;
	int error;

	/* The ways for the image's type again (the lists may have changed). */
	count = share_ways(path, format, openers);
	if (index < 0 || index >= count)
		return ENOENT;

	/* The application, started on the image apart from the viewer. */
	error = fm_apps_launch(&openers[index], path);
	if (error != 0)
		return error;

	/* Succeeded: the application is on its way. */
	return 0;
}

/*
 * Writes a line of Files' log for the code shared with it, as the
 * viewer's own (Files' launch says what it started).
 */
void
fm_log(
	const char *format,
	...)
{
	va_list arguments;

	/* The viewer's prefix, the message and the end of the line, at once. */
	fputs("IMAGEVIEW ", stderr);
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
	fflush(stderr);
}

/*
 * Fills the ways Files offers for an image of a format, less Quick Look
 * (Files' own window, which the viewer has no use for); reports how many.
 */
static int
share_ways(
	const char *path,
	const char *format,
	struct fm_opener *openers)
{
	struct fm_mime mime;
	int count;
	int kept;
	int index;
	int quicklook;

	/* The image's type, as Files knows it. */
	memset(&mime, 0, sizeof(mime));
	mime.type = share_type(format);
	mime.kind = "Image";

	/* The ways Files offers for that type. */
	count = fm_apps_for(path, &mime, S_IFREG | 0644, openers, FM_OPENERS);

	/* Each way but Quick Look is kept, in its order. */
	kept = 0;
	for (index = 0; index < count; index++) {
		quicklook = fm_apps_is_quicklook(&openers[index]);
		if (quicklook != 0)
			continue;
		if (kept != index)
			openers[kept] = openers[index];
		kept++;
	}

	/* Reports how many ways are kept. */
	return kept;
}

/*
 * Moves a file into its trash: the record first, then a rename, or a copy
 * and the file's removal across file systems.  Returns 0, or an errno value
 * with nothing of it left in the trash.
 */
static int
share_trash_place(
	const char *path,
	const struct stat *status,
	char *trashed,
	size_t size)
{
	char trash[FM_OPS_PATH_MAX];
	char name[FM_OPS_PATH_MAX];
	char record[2 * FM_OPS_PATH_MAX + 32];
	int written;
	int error;

	/* The file's trash: its volume's, or the home trash. */
	error = fm_trash_for(path, trash, sizeof(trash));
	if (error != 0)
		return error;

	/* A name in the trash nothing has. */
	error = fm_trash_name(trash, share_base(path), name, sizeof(name));
	if (error != 0)
		return error;

	/* Where the file goes among the trash's files. */
	written = snprintf(trashed, size, "%s/files/%s", trash, name);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* The record of where it was and when. */
	snprintf(record, sizeof(record), "%s/info/%s.trashinfo", trash, name);
	error = fm_trash_info_write(record, path, time(NULL));
	if (error != 0)
		return error;

	/* Within one file system a rename. */
	error = rename(path, trashed);
	if (error == 0)
		return 0;
	error = errno;

	/* Another failure than a crossing of file systems takes the record back. */
	if (error != EXDEV) {
		(void)unlink(record);
		return error;
	}

	/* Across file systems a copy, then the file's removal. */
	error = share_copy(path, trashed, status->st_mode);
	if (error == 0) {
		error = unlink(path);
		if (error != 0)
			error = errno;
	}

	/* A copy or a removal that failed leaves the file where it was, and nothing in the trash. */
	if (error != 0) {
		(void)unlink(trashed);
		(void)unlink(record);
		return error;
	}

	/* Succeeded: the file is in the trash. */
	return 0;
}

/* Copies a regular file to a new path with its permissions; returns 0 or an errno value. */
static int
share_copy(
	const char *source,
	const char *target,
	mode_t mode)
{
	int in;
	int out;
	int regular;
	int closed;
	int error;

	/* Only a regular file is copied. */
	regular = S_ISREG(mode);
	if (regular == 0)
		return EXDEV;

	/* The file to read. */
	in = open(source, O_RDONLY);
	if (in < 0)
		return errno;

	/* The new file, which must not be there yet. */
	out = open(target, O_WRONLY | O_CREAT | O_EXCL, mode & 0777);
	if (out < 0) {
		error = errno;
		close(in);
		return error;
	}

	/* The bytes, then both files closed (a failed close of the copy loses its last bytes). */
	error = share_copy_bytes(in, out);
	close(in);
	closed = close(out);
	if (closed != 0 && error == 0)
		error = errno;

	/* Reports a copy that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the copy is whole. */
	return 0;
}

/* Copies every byte of one descriptor to another; returns 0 or an errno value. */
static int
share_copy_bytes(
	int in,
	int out)
{
	unsigned char *buffer;
	ssize_t got;
	ssize_t put;
	ssize_t done;
	int error;

	/* The buffer a step moves. */
	buffer = malloc(SHARE_COPY_CHUNK);
	if (buffer == NULL)
		return ENOMEM;

	/* Each chunk, written in full, until the end of the file. */
	error = 0;
	for (;;) {
		got = read(in, buffer, SHARE_COPY_CHUNK);
		if (got == 0)
			break;
		if (got < 0) {
			error = errno;
			break;
		}

		/* The chunk, perhaps in several writes. */
		done = 0;
		while (done < got) {
			put = write(out, buffer + done, (size_t)(got - done));
			if (put < 0) {
				error = errno;
				break;
			}

			/* The part written moves the chunk on. */
			done += put;
		}

		/* A write that failed ends the copy. */
		if (error != 0)
			break;
	}

	/* The buffer goes. */
	free(buffer);

	/* Reports a copy that failed. */
	if (error != 0)
		return error;

	/* Succeeded: every byte is copied. */
	return 0;
}

/* Reports the MIME type of a format the viewer reads. */
static const char *
share_type(
	const char *format)
{
	int match;

	/* An image not read at all is taken for a PNG picture. */
	if (format == NULL)
		return "image/png";

	/* A JPEG picture. */
	match = strcmp(format, "JPEG");
	if (match == 0)
		return "image/jpeg";

	/* A GIF picture. */
	match = strcmp(format, "GIF");
	if (match == 0)
		return "image/gif";

	/* A PNG picture, and anything else the viewer read as one. */
	return "image/png";
}

/* Reports the last part of a path. */
static const char *
share_base(
	const char *path)
{
	const char *slash;

	/* The part after the last slash, or the whole path. */
	slash = strrchr(path, '/');
	if (slash == NULL)
		return path;

	/* The name after the slash. */
	return slash + 1;
}
