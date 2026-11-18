/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A face's companions (ws090-p020): the files a desktop installs beside
 * its font, which the face draws with as if they were one font.
 *
 * The bold companion holds the face's own glyphs in a bold weight, in the
 * same order (the desktop's Mahora Regular and Mahora Bold): while the face
 * is bold, a glyph of its own is drawn from it as it is, not widened.  The
 * next companion draws the characters the face lacks (the desktop's
 * monospaced fallback, for the signs and the Latin letters Mahora does not
 * have yet): truetype_glyph_index finds them there and numbers them after
 * the face's own glyphs, and every call given such a number draws from the
 * companion at the face's size and weight (widened when bold).  The calls
 * in design units scale a companion's units to the face's em.
 *
 * The face's line (truetype_metrics, truetype_design_metrics) stays its
 * own, so that whether a companion is installed never moves a layout.  A
 * companion that cannot be read is left out: the face draws what it has.
 */

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* The largest companion file read, in bytes. */
#define COMPANION_FILE_MAX	(64U * 1024U * 1024U)

/*
 * The flags of a companion's open.  The Windows C runtime has no
 * close-on-exec and opens a file in text mode (turning CR LF into LF and
 * stopping at a ^Z) unless told O_BINARY; a system without one of the
 * flags gets 0 for it.
 */
#ifndef O_CLOEXEC
#define O_CLOEXEC	0
#endif
#ifndef O_BINARY
#define O_BINARY	0
#endif

static int companion_open(const char *path, struct truetype_face **face);
static int companion_same_widths(const struct truetype_face *face, const struct truetype_face *bold);
static int companion_read(const char *path, void **data, size_t *size);

/*
 * Opens a face's companions from their files: bold_path, the same glyphs in
 * a bold weight, and next_path, the face that draws the characters this one
 * lacks.  Either may be NULL.  A file that is missing or unreadable, and a
 * bold face whose glyphs are not the face's (another count, em or width:
 * Mahora Bold is no bold of Mahora Mono), is left out.  Returns 0, or EINVAL for a missing face or one that has
 * companions already.
 */
int
truetype_open_companions(
	struct truetype_face *face,
	const char *bold_path,
	const char *next_path)
{
	struct truetype_face *bold;
	struct truetype_face *next;
	int error;
	int same;

	/* A face without companions yet. */
	if (face == NULL)
		return EINVAL;
	if (face->bold_face != NULL || face->next != NULL)
		return EINVAL;

	/* The bold face, kept only when its glyphs are the face's own. */
	bold = NULL;
	if (bold_path != NULL) {
		error = companion_open(bold_path, &bold);
		if (error != 0)
			bold = NULL;
	}
	if (bold != NULL) {
		same = companion_same_widths(face, bold);
		if (!same) {
			truetype_close(bold);
			bold = NULL;
		}
	}

	/* The face for the characters this one lacks. */
	next = NULL;
	if (next_path != NULL) {
		error = companion_open(next_path, &next);
		if (error != 0)
			next = NULL;
	}

	/* Both take the face's size and weight from now on (the bold face draws without widening). */
	if (bold != NULL)
		bold->pixels = face->pixels;
	if (next != NULL) {
		next->pixels = face->pixels;
		next->bold = face->bold;
	}

	/* Succeeded: the face draws with them. */
	face->bold_face = bold;
	face->next = next;
	return 0;
}

/*
 * Tells whether a glyph number of a face is drawn from its bold companion
 * while the face is bold (1: drawn as it is, not widened), or not (0: a
 * glyph of the next companion's, or a face without a bold one).
 */
int
truetype_glyph_bold_face(
	const struct truetype_face *face,
	unsigned glyph)
{
	/* A face without a bold companion, or a glyph not of its own. */
	if (face == NULL || face->bold_face == NULL)
		return 0;
	if (glyph >= face->glyph_count)
		return 0;

	/* Succeeded: the bold companion has the glyph. */
	return 1;
}

/*
 * Finds the face that draws a glyph number of a face: a number past the
 * face's own glyphs is the next companion's (less the face's count), and
 * while the face is bold, one of its own is its bold companion's.  *glyph
 * becomes the number in the face found.
 */
struct truetype_face *
truetype_resolve(
	struct truetype_face *face,
	unsigned *glyph)
{
	/* Past each face's own glyphs, into the next one. */
	while (*glyph >= face->glyph_count && face->next != NULL) {
		*glyph -= face->glyph_count;
		face = face->next;
	}

	/* A glyph of the face's own, drawn bold: the bold face's. */
	if (face->bold != 0U && face->bold_face != NULL)
		face = face->bold_face;

	/* Succeeded: the face that draws it. */
	return face;
}

/*
 * The same as truetype_resolve, for the calls that only read a face.
 */
const struct truetype_face *
truetype_resolve_const(
	const struct truetype_face *face,
	unsigned *glyph)
{
	/* Past each face's own glyphs, into the next one. */
	while (*glyph >= face->glyph_count && face->next != NULL) {
		*glyph -= face->glyph_count;
		face = face->next;
	}

	/* A glyph of the face's own, drawn bold: the bold face's. */
	if (face->bold != 0U && face->bold_face != NULL)
		face = face->bold_face;

	/* Succeeded: the face that draws it. */
	return face;
}

/* Tells whether a bold face has the face's glyphs: as many, in the same em, each as wide (1), or not (0). */
static int
companion_same_widths(
	const struct truetype_face *face,
	const struct truetype_face *bold)
{
	unsigned glyph;
	int regular_width;
	int bold_width;
	int error;

	/* The same count and em. */
	if (bold->glyph_count != face->glyph_count)
		return 0;
	if (bold->units_per_em != face->units_per_em)
		return 0;

	/* Each glyph as wide in both. */
	for (glyph = 0; glyph < face->glyph_count; glyph++) {
		error = truetype_glyph_design_advance(face, glyph, &regular_width);
		if (error != 0)
			return 0;
		error = truetype_glyph_design_advance(bold, glyph, &bold_width);
		if (error != 0)
			return 0;
		if (regular_width != bold_width)
			return 0;
	}

	/* Succeeded: the bold face draws the same glyphs. */
	return 1;
}

/* Opens a companion's file as a face that owns the bytes it reads. */
static int
companion_open(
	const char *path,
	struct truetype_face **face)
{
	void *data;
	size_t size;
	int error;

	/* The whole file. */
	data = NULL;
	size = 0;
	error = companion_read(path, &data, &size);
	if (error != 0)
		return error;

	/* Its face, which frees the bytes when it closes. */
	error = truetype_open(data, size, 0U, face);
	if (error != 0) {
		free(data);
		return error;
	}
	(*face)->owned = data;

	/* Succeeded: the companion is open. */
	return 0;
}

/* Reads a whole file into memory of its own (at most COMPANION_FILE_MAX bytes). */
static int
companion_read(
	const char *path,
	void **data,
	size_t *size)
{
	struct stat status;
	unsigned char *bytes;
	size_t length;
	ssize_t got;
	int descriptor;
	int error;

	/* The file. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC | O_BINARY);
	if (descriptor < 0)
		return errno;

	/* Its size, which has to be a font's. */
	error = fstat(descriptor, &status);
	if (error != 0) {
		error = errno;
		close(descriptor);
		return error;
	}
	if (status.st_size <= 0 || (unsigned long long)status.st_size > COMPANION_FILE_MAX) {
		close(descriptor);
		return EINVAL;
	}
	length = (size_t)status.st_size;

	/* The memory for it. */
	bytes = malloc(length);
	if (bytes == NULL) {
		close(descriptor);
		return ENOMEM;
	}

	/* Every byte, as the reads return them. */
	*size = 0;
	while (*size < length) {
		got = read(descriptor, bytes + *size, length - *size);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		*size += (size_t)got;
	}
	close(descriptor);

	/* A file that ended early (it changed while read). */
	if (*size != length) {
		free(bytes);
		return EIO;
	}

	/* Succeeded: the caller owns the bytes. */
	*data = bytes;
	return 0;
}
