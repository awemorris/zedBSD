/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The faces the core fonts are drawn from.
 *
 * The caller hands the server TrueType files by their roles (x11server.h):
 * a monospaced face and a proportional one, each perhaps with its bold.  A
 * core font (font.c) is a role at a pixel size; this file measures its
 * lines and characters and draws its glyphs as coverage, which font.c
 * turns into one bit a pixel.  A role without a file of its own is drawn
 * with another's face: the bold roles with their regular face made bold,
 * the proportional ones with the monospaced face.  A face keeps the size
 * and weight it was last set to, so that a run of glyphs of one font sets
 * them once.
 */

#include "userland/desktop/xserver/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <truetype/truetype.h>

static int faces_select(struct x11_faces *faces, unsigned role, unsigned pixels, struct truetype_face **face);
static unsigned faces_resolve(const struct x11_faces *faces, unsigned role, int *bold);

/*
 * Opens the faces over the font files the caller keeps in memory, with the
 * file of the characters the monospaced faces lack (NULL: none), which the
 * font library reads by its path.  Returns 0; ENOMEM, with no face open;
 * or the error of a file that could not be read, with the others open.
 */
int
x11_faces_open(
	struct x11_faces *faces,
	const struct x11server_font_file *files,
	unsigned count,
	const char *fallback_path)
{
	unsigned index;
	unsigned role;
	int failed;
	int error;

	/* No face yet. */
	memset(faces, 0, sizeof(*faces));
	failed = 0;

	/* A face over each file of a role not yet taken (the first file of a role wins). */
	for (index = 0U; index < count; index++) {
		role = files[index].role;
		if (role >= X11_FACES)
			continue;
		if (faces->faces[role] != NULL)
			continue;
		error = truetype_open(files[index].bytes, files[index].size, 0U, &faces->faces[role]);
		if (error != 0) {
			faces->faces[role] = NULL;
			failed = error;
		}
	}

	/* The monospaced faces' companion, the face of the signs they lack. */
	if (fallback_path != NULL) {
		if (faces->faces[X11_FACE_MONO] != NULL)
			(void)truetype_open_companions(faces->faces[X11_FACE_MONO], NULL, fallback_path);
		if (faces->faces[X11_FACE_MONO_BOLD] != NULL)
			(void)truetype_open_companions(faces->faces[X11_FACE_MONO_BOLD], NULL, fallback_path);
	}

	/* The coverage a glyph is drawn into, once some face is open. */
	for (role = 0U; role < X11_FACES; role++) {
		if (faces->faces[role] != NULL)
			break;
	}
	if (role < X11_FACES) {
		faces->coverage = malloc((size_t)X11_GLYPH_DRAWN_MAX * X11_GLYPH_DRAWN_MAX);
		if (faces->coverage == NULL) {
			x11_faces_close(faces);
			return ENOMEM;
		}
	}

	/* Reports a file that could not be read (the others are open). */
	if (failed != 0)
		return failed;

	/* Succeeded: glyphs can be drawn from each face open. */
	return 0;
}

/*
 * Closes the faces (the files' bytes stay the caller's).
 */
void
x11_faces_close(
	struct x11_faces *faces)
{
	unsigned role;

	/* Each face. */
	for (role = 0U; role < X11_FACES; role++) {
		if (faces->faces[role] != NULL)
			truetype_close(faces->faces[role]);
	}

	/* The coverage, and no face. */
	free(faces->coverage);
	memset(faces, 0, sizeof(*faces));
}

/*
 * Tells whether a role is drawn from a face (its own or another's): 1, or
 * 0 when no face draws it.
 */
int
x11_face_present(
	const struct x11_faces *faces,
	unsigned role)
{
	unsigned resolved;
	int bold;

	/* The face the role is drawn with. */
	resolved = faces_resolve(faces, role, &bold);
	if (resolved >= X11_FACES)
		return 0;

	/* Succeeded: there is one. */
	return 1;
}

/*
 * Tells whether a role has its own font file (1) or relies on another
 * role's face (0).
 */
int
x11_face_has_own(
	const struct x11_faces *faces,
	unsigned role)
{
	/* Only a role within range with an open face has its own. */
	if (role >= X11_FACES)
		return 0;
	if (faces->faces[role] == NULL)
		return 0;

	/* Succeeded: its own face is present. */
	return 1;
}

/*
 * Measures a character's advance width at a pixel size without loading
 * its outline: zero when the face lacks the character.  Returns 0, or
 * ENOENT without a face.
 */
int
x11_face_char_advance(
	struct x11_faces *faces,
	unsigned role,
	unsigned pixels,
	uint32_t codepoint,
	int *advance)
{
	struct truetype_design_metrics design;
	struct truetype_face *face;
	unsigned glyph;
	int units;
	int error;

	/* The face at the size. */
	*advance = 0;
	error = faces_select(faces, role, pixels, &face);
	if (error != 0)
		return error;

	/* A character the face lacks has no advance. */
	glyph = truetype_glyph_index(face, codepoint);
	if (glyph == 0U)
		return 0;

	/* Scale the design advance to whole pixels. */
	error = truetype_design_metrics(face, &design);
	if (error != 0 || design.units_per_em == 0U)
		return 0;
	error = truetype_glyph_design_advance(face, glyph, &units);
	if (error != 0)
		return 0;
	*advance = (units * (int)pixels + (int)design.units_per_em / 2) / (int)design.units_per_em;
	return 0;
}

/*
 * Measures a role's line at a pixel size: the ascent and descent (both
 * counted away from the baseline) and the advance of an M.  Returns 0, or
 * ENOENT without a face.
 */
int
x11_face_line(
	struct x11_faces *faces,
	unsigned role,
	unsigned pixels,
	int *ascent,
	int *descent,
	int *advance)
{
	struct truetype_metrics line;
	struct truetype_glyph metrics;
	struct truetype_face *face;
	unsigned glyph;
	int error;

	/* The face at the size. */
	error = faces_select(faces, role, pixels, &face);
	if (error != 0)
		return error;

	/* The line's ascent and descent (the library gives the descent below the baseline as a negative). */
	error = truetype_metrics(face, &line);
	if (error != 0)
		return error;
	*ascent = line.ascent;
	*descent = -line.descent;

	/* The advance of an M. */
	glyph = truetype_glyph_index(face, 'M');
	error = truetype_glyph_metrics(face, glyph, &metrics);
	if (error != 0)
		return error;
	*advance = metrics.advance;

	/* Succeeded: measured. */
	return 0;
}

/*
 * Measures a character of a role at a pixel size as QueryFont gives it:
 * all zero for a character the face does not have.  Returns 0, or ENOENT
 * without a face.
 */
int
x11_face_char(
	struct x11_faces *faces,
	unsigned role,
	unsigned pixels,
	uint32_t codepoint,
	struct x11_char_metrics *metrics)
{
	struct truetype_glyph glyph_metrics;
	struct truetype_face *face;
	unsigned glyph;
	int error;

	/* The face at the size. */
	memset(metrics, 0, sizeof(*metrics));
	error = faces_select(faces, role, pixels, &face);
	if (error != 0)
		return error;

	/* A character the face lacks (glyph 0, the face's sign for what it cannot draw) does not exist. */
	glyph = truetype_glyph_index(face, codepoint);
	if (glyph == 0U)
		return 0;

	/* Its advance (a glyph that cannot be measured is left out). */
	error = truetype_glyph_metrics(face, glyph, &glyph_metrics);
	if (error != 0)
		return 0;
	metrics->width = (int16_t)glyph_metrics.advance;

	/* A glyph without ink (a space) is all advance. */
	if (glyph_metrics.width == 0U || glyph_metrics.height == 0U)
		return 0;

	/* Succeeded: the ink's edges and its rows above and below the baseline. */
	metrics->left = (int16_t)glyph_metrics.left;
	metrics->right = (int16_t)(glyph_metrics.left + (int)glyph_metrics.width);
	metrics->ascent = (int16_t)glyph_metrics.top;
	metrics->descent = (int16_t)((int)glyph_metrics.height - glyph_metrics.top);
	return 0;
}

/*
 * Draws a character of a role at a pixel size as coverage, which stays
 * valid until the next glyph is drawn.  A character the face lacks draws
 * the face's sign for it.  Returns 0, ENOENT without a face, or EINVAL for
 * a glyph larger than X11_GLYPH_DRAWN_MAX each way.
 */
int
x11_face_draw(
	struct x11_faces *faces,
	unsigned role,
	unsigned pixels,
	uint32_t codepoint,
	struct x11_face_glyph *glyph)
{
	struct truetype_glyph metrics;
	struct truetype_face *face;
	unsigned index;
	int error;

	/* The face at the size. */
	memset(glyph, 0, sizeof(*glyph));
	error = faces_select(faces, role, pixels, &face);
	if (error != 0)
		return error;

	/* The glyph's coverage, in rows of the largest width. */
	index = truetype_glyph_index(face, codepoint);
	error = truetype_render_glyph(face, index, &metrics, faces->coverage, X11_GLYPH_DRAWN_MAX, (size_t)X11_GLYPH_DRAWN_MAX * X11_GLYPH_DRAWN_MAX);
	if (error != 0)
		return error;

	/* Succeeded: where it sits and how far the origin moves. */
	glyph->coverage = faces->coverage;
	glyph->stride = X11_GLYPH_DRAWN_MAX;
	glyph->left = metrics.left;
	glyph->top = metrics.top;
	glyph->width = (int)metrics.width;
	glyph->height = (int)metrics.height;
	glyph->advance = metrics.advance;
	return 0;
}

/* Sets the face a role is drawn with to a size and weight, unless it already is. */
static int
faces_select(
	struct x11_faces *faces,
	unsigned role,
	unsigned pixels,
	struct truetype_face **face)
{
	unsigned resolved;
	int bold;
	int error;

	/* The face the role is drawn with, and whether it is made bold. */
	resolved = faces_resolve(faces, role, &bold);
	if (resolved >= X11_FACES)
		return ENOENT;
	*face = faces->faces[resolved];

	/* The size, when it is another. */
	if (faces->sizes[resolved] != pixels) {
		error = truetype_set_pixel_size(*face, pixels);
		if (error != 0) {
			faces->sizes[resolved] = 0U;
			return error;
		}
		faces->sizes[resolved] = pixels;
	}

	/* The weight, when it is another. */
	if (faces->bolds[resolved] != bold) {
		error = truetype_set_bold(*face, bold);
		if (error != 0)
			return error;
		faces->bolds[resolved] = bold;
	}

	/* Succeeded: the face draws the role. */
	return 0;
}

/*
 * Finds the face a role is drawn with (X11_FACES: none) and whether it is
 * to be made bold: the role's own, else the regular face of its kind made
 * bold, else the monospaced faces for the proportional ones.
 */
static unsigned
faces_resolve(
	const struct x11_faces *faces,
	unsigned role,
	int *bold)
{
	unsigned regular;
	int bold_role;

	/* The role's own face. */
	*bold = 0;
	if (role >= X11_FACES)
		return X11_FACES;
	if (faces->faces[role] != NULL)
		return role;

	/* The regular role of a bold one. */
	bold_role = 0;
	regular = role;
	if (role == X11_FACE_MONO_BOLD) {
		bold_role = 1;
		regular = X11_FACE_MONO;
	} else if (role == X11_FACE_SANS_BOLD) {
		bold_role = 1;
		regular = X11_FACE_SANS;
	}

	/* A bold role without its file: its regular face, made bold. */
	if (faces->faces[regular] != NULL) {
		*bold = bold_role;
		return regular;
	}

	/* A proportional role without a proportional face: the monospaced faces. */
	if (regular == X11_FACE_SANS) {
		if (bold_role && faces->faces[X11_FACE_MONO_BOLD] != NULL)
			return X11_FACE_MONO_BOLD;
		if (faces->faces[X11_FACE_MONO] != NULL) {
			*bold = bold_role;
			return X11_FACE_MONO;
		}
	}

	/* A fixed role without its own face: the monospaced face. */
	if (role == X11_FACE_FIXED && faces->faces[X11_FACE_MONO] != NULL)
		return X11_FACE_MONO;

	/* No face draws the role. */
	return X11_FACES;
}
