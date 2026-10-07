/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What files reads of a file to show it without opening it: its
 * type as its first bytes tell it, its first lines when it is text (the
 * preview pane shows a few of them, Quick Look more), and its picture at
 * the size Quick Look shows it.
 *
 * One file is kept, the one shown last.  Showing another replaces it, and
 * so does the same file changed since it was read.  A listing learns a
 * file's type from its name only; the bytes are read here, for the one
 * file the user is looking at.
 */

#include "files.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static size_t peek_read_start(const char *path, unsigned char *bytes, size_t size);
static void peek_keep_lines(struct fm_peek *peek, const unsigned char *bytes, size_t length);
static int peek_textual(const struct fm_mime *mime);

/*
 * Reads what the preview shows of an item, unless it is the file already
 * read and unchanged.
 */
void
fm_peek_read(
	struct fm_peek *peek,
	const struct fm_entry *entry)
{
	unsigned char *bytes;
	size_t length;
	int textual;
	int match;
	int text;

	/* The same file, unchanged, is read already. */
	match = strcmp(peek->path, entry->path);
	if (match == 0 && peek->modified == entry->modified)
		return;

	/* Whatever was read of another file goes; this one is remembered. */
	fm_peek_release(peek);
	snprintf(peek->path, sizeof(peek->path), "%s", entry->path);
	peek->modified = entry->modified;
	peek->mime = entry->mime;

	/* A folder is not looked into. */
	if (entry->folder != 0) {
		fm_log("PEEK path=%s type=%s lines=0", peek->path, peek->mime->type);
		return;
	}

	/* The type its first bytes tell, when its name did not. */
	peek->mime = fm_mime_sniff(entry->path, entry->mime);

	/* Only text has lines to show. */
	textual = peek_textual(peek->mime);
	if (textual == 0) {
		fm_log("PEEK path=%s type=%s lines=0", peek->path, peek->mime->type);
		return;
	}

	/* A buffer for the file's start. */
	bytes = malloc(FM_PEEK_BYTES);
	if (bytes == NULL)
		return;

	/* The file's start, which must read as text to be shown as such. */
	length = peek_read_start(entry->path, bytes, FM_PEEK_BYTES);
	text = fm_mime_text(bytes, length);
	if (length != 0U && text != 0)
		peek_keep_lines(peek, bytes, length);

	/* The bytes read are not needed any more. */
	free(bytes);

	/* The log line the tests wait for. */
	fm_log("PEEK path=%s type=%s lines=%d", peek->path, peek->mime->type, peek->line_count);
}

/*
 * Asks for the picture of the file read, shrunk to fit a square of a
 * side, once; a file that is not a picture, or cannot be read as one, has
 * none.  The picture is made without waiting (ws177-p010): the window
 * draws on, and fm_peek_tick takes it when it is done.
 */
void
fm_peek_picture(
	struct fm_peek *peek,
	int side)
{
	int error;

	/* The picture is asked for once. */
	if (peek->picture_tried != 0)
		return;
	peek->picture_tried = 1;

	/* Only a picture has one. */
	if (peek->mime == NULL || peek->mime->category != FM_CATEGORY_IMAGE)
		return;

	/* Starts making it, no larger than the square. */
	error = fm_picture_begin(FM_PICTURE_PEEK, peek->path, side, side);
	if (error != 0) {
		fm_log("PEEK picture path=%s error=%d width=0 height=0", peek->path, error);
		return;
	}

	/* fm_peek_tick follows it from now on. */
	peek->picture_pending = 1;
}

/*
 * Takes the picture asked for when it is done.  Returns nonzero when it
 * is (made or not), so that the window is drawn again.
 */
int
fm_peek_tick(
	struct fm_peek *peek)
{
	int finished;
	int error;

	/* No picture is being made. */
	if (peek->picture_pending == 0)
		return 0;

	/* The picture, while it is made. */
	finished = fm_picture_follow(FM_PICTURE_PEEK, &peek->picture, &error);
	if (!finished)
		return 0;
	peek->picture_pending = 0;

	/* The log line the tests wait for. */
	fm_log("PEEK picture path=%s error=%d width=%d height=%d", peek->path, error, peek->picture.width, peek->picture.height);

	/* Succeeded: the picture is done. */
	return 1;
}

/*
 * Forgets the file read, freeing its lines and its picture.
 */
void
fm_peek_release(
	struct fm_peek *peek)
{
	/* Gives up a picture still being made. */
	if (peek->picture_pending != 0)
		fm_picture_cancel(FM_PICTURE_PEEK);

	/* The lines and the picture. */
	free(peek->text);
	kl_image_release(&peek->picture);

	/* Nothing is read now. */
	memset(peek, 0, sizeof(*peek));
}

/* Reads up to a size of bytes from a file's start; returns how many were read (none when it cannot be read). */
static size_t
peek_read_start(
	const char *path,
	unsigned char *bytes,
	size_t size)
{
	ssize_t count;
	size_t done;
	int descriptor;

	/* The file. */
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return 0;

	/* Its bytes, until the size, the end or an error. */
	done = 0;
	while (done < size) {
		count = read(descriptor, bytes + done, size - done);
		if (count <= 0)
			break;
		done += (size_t)count;
	}

	/* The file is not needed any more. */
	close(descriptor);

	/* Reports how many bytes were read. */
	return done;
}

/*
 * Keeps the first FM_PEEK_LINES lines of text: tabs become spaces and
 * carriage returns go, since the window draws each line as it is.
 */
static void
peek_keep_lines(
	struct fm_peek *peek,
	const unsigned char *bytes,
	size_t length)
{
	size_t index;
	size_t kept;
	int lines;

	/* The text up to the end of the last line kept (a last line without its end counts too). */
	lines = 0;
	kept = length;
	for (index = 0; index < length; index++) {
		if (bytes[index] != '\n')
			continue;
		lines++;
		if (lines == FM_PEEK_LINES) {
			kept = index + 1U;
			break;
		}
	}

	/* A last line that does not end in a newline is a line too. */
	if (kept > 0U && bytes[kept - 1U] != '\n')
		lines++;

	/* The copy, with room for its end. */
	peek->text = malloc(kept + 1U);
	if (peek->text == NULL)
		return;

	/* Each byte, tabs made spaces and carriage returns left out. */
	peek->text_length = 0;
	for (index = 0; index < kept; index++) {
		if (bytes[index] == '\r')
			continue;

		/* A tab is drawn as a space. */
		if (bytes[index] == '\t') {
			peek->text[peek->text_length] = ' ';
		} else {
			peek->text[peek->text_length] = (char)bytes[index];
		}

		/* The copy is one byte longer. */
		peek->text_length++;
	}

	/* The text ends there, and holds that many lines. */
	peek->text[peek->text_length] = '\0';
	peek->line_count = lines;
}

/* Tells whether a type is one whose lines the preview shows. */
static int
peek_textual(
	const struct fm_mime *mime)
{
	/* Text and source code. */
	if (mime->category == FM_CATEGORY_TEXT)
		return 1;
	if (mime->category == FM_CATEGORY_CODE)
		return 1;

	/* Anything else. */
	return 0;
}
