/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PDF of a Notes document (plan/ws079/design-pdf.md sections 1 to 3).
 *
 * A notebook Notes made is written whole with libpdf's writer: each stroke's
 * outline, the polygon pdf_outline_stroke() makes and the screen fills too,
 * as one filled path in the stroke's colour and opacity, and the whole
 * document as the edit data (encode.c), attached.
 *
 * A notebook written on another program's PDF -- or on a notebook of which
 * another program changed pages -- keeps that PDF as its base.  Each save
 * writes the base's bytes unchanged and adds one revision (libpdf's
 * update): the strokes drawn over each page of the base that has them, the
 * pages Notes added, the edit data.  Since every save starts again from the
 * base, the revision of the last save is replaced rather than piled up, and
 * the base -- the other program's file -- stays as it was.
 *
 * Either file is written under a temporary name, put on the disk and
 * renamed over the old one, so a crash leaves the old file or the new one.
 *
 * Opening reads the edit data back and rebuilds the document from it; the
 * PDF's paths are not read.  A file whose newest revision is the one Notes
 * added to its base gets its base back and its strokes stay editable.  Any
 * other PDF becomes the base as it stands: its pages are drawn under the
 * strokes, except the pages of a notebook Notes saved that no other program
 * changed, whose strokes stay editable and are written anew in place.
 *
 * ws175-p007: a page of the base with edits of its objects is written with
 * libpdf's editor (its content changed, the strokes over it), and the
 * images inserted on a page of Notes' own are drawn under its strokes
 * (plan/ws175/phase001/design.md section 6.4 and [N2]).  Opening checks
 * that every edit's object is on the base's page and reads every image
 * back from the file; when one is not, the file is opened as it is shown
 * (NOTES_OPENED_REBASED, design.md [H3], [N1]).
 *
 * ws216-p003: each stroke is written as its shapes (shape.c), each one fill:
 * the watercolour's multiplied over what is under it, the pencil's as a
 * plain fill of the grain's least opacity under a fill of the grain (a
 * tiling pattern of the page, plan/ws216/phase001/phase.md section 5.2).
 * A notebook with the drawing tools (edit data 3.0) has its pages'
 * content compressed with Flate; one without them is written as before.
 */

#include "notes.h"

#include <errno.h>
#include <fcntl.h>
#include <sha2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The longest path saved. */
#define SAVE_PATH_MAX		4096U

/* The largest PDF Notes opens, as libpdf's reader. */
#define SAVE_FILE_MAX		((size_t)512 * 1024 * 1024)

/* The size of each read while a file is loaded. */
#define SAVE_READ_CHUNK		65536U

/*
 * What the strokes of one save share in the writer (ws216-p003): the
 * pencil's grain, added to the document once, when the first pencil
 * stroke is written.
 */
struct save_paint {
	int has_grain;
	size_t grain;
};

static int save_whole(struct notes_document *document, const char *temporary);
static int save_update(struct notes_document *document, const char *temporary);
static int save_page(struct pdf_writer *writer, struct notes_document *document, size_t index, struct save_paint *paint);
static int save_strokes(struct pdf_writer *writer, struct notes_page *page, struct save_paint *paint);
static int save_shape(struct pdf_writer *writer, const struct notes_stroke *stroke, const struct notes_shape *shape, struct save_paint *paint);
static int save_shape_path(struct pdf_writer *writer, const struct notes_shape *shape);
static int save_grain(struct pdf_writer *writer, struct save_paint *paint);
static int save_compress(struct pdf_writer *writer, const struct notes_document *document);
static int save_edit_data(struct pdf_writer *writer, struct notes_document *document);
static int save_sync(const char *path);
static int save_sync_folder(const char *path);
static int open_own(struct notes_document *saved, struct pdf_document *file);
static int open_base(struct notes_document *saved, const unsigned char *data, size_t size, struct pdf_document *file);
static int open_as_base(struct notes_document *document, struct notes_document *saved, int decoded, int rebase, const unsigned char *data, size_t size, struct pdf_document *file, size_t *changed);
static int open_edits(struct notes_document *saved, struct pdf_document *file);
static int open_keys(struct notes_document *saved, size_t index);
static int open_images(struct notes_page *page, struct pdf_document *file, size_t index);
static int page_unchanged(const struct notes_page *page, struct pdf_document *file, size_t index);
static int read_file(const char *path, size_t limit, unsigned char **data, size_t *size);
static void hash_bytes(const unsigned char *data, size_t size, unsigned char digest[32]);

/*
 * Saves a document as a PDF at a path, and tells how many bytes it has.
 *
 * On success the document is no longer dirty and keeps the PDF's
 * identifier for the next save.  Returns 0, or an errno value (the old
 * file is then unchanged).
 */
int
notes_save_pdf(
	struct notes_document *document,
	const char *path,
	size_t *bytes)
{
	char temporary[SAVE_PATH_MAX + 8U];
	struct stat status;
	int written;
	int result;
	int error;

	/* The temporary name beside the file. */
	written = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	if (written < 0 || (size_t)written >= sizeof(temporary))
		return ENAMETOOLONG;

	/* Writes the temporary file: the whole notebook, or its base and a revision. */
	if (document->base != NULL) {
		error = save_update(document, temporary);
	} else {
		error = save_whole(document, temporary);
	}

	/* A file that could not be written goes. */
	if (error != 0) {
		(void)unlink(temporary);
		return error;
	}

	/* Puts the bytes on the disk before the name. */
	error = save_sync(temporary);
	if (error != 0) {
		(void)unlink(temporary);
		return error;
	}

	/* Replaces the old file in one step. */
	result = rename(temporary, path);
	if (result != 0) {
		error = errno;
		(void)unlink(temporary);
		return error;
	}

	/* Puts the new name on the disk. */
	(void)save_sync_folder(path);

	/* The file's size. */
	*bytes = 0;
	result = stat(path, &status);
	if (result == 0)
		*bytes = (size_t)status.st_size;

	/* Succeeded: the file is the document as it stands. */
	document->dirty = 0;
	return 0;
}

/*
 * Writes a clean copy of a saved notebook (File > Save Clean Copy,
 * ws175-p009, plan/ws175/phase001/design.md D1): the PDF at from, as it
 * reads, written whole at path without its earlier revisions, without the
 * resources its pages no longer use (a deleted image's bytes go) and
 * without Notes' edit data, so the copy opens as another program's PDF
 * with the edits part of its pages.  The copy is written under a temporary
 * name, put on the disk and renamed, as a save is; from stays as it is.
 * Tells the copy's size, its objects and the resources it left out.
 * Returns 0, or an errno value (an old file at path is then unchanged).
 */
int
notes_save_clean_copy(
	const char *from,
	const char *path,
	size_t *bytes,
	size_t *objects,
	size_t *dropped)
{
	char temporary[SAVE_PATH_MAX + 8U];
	struct pdf_clean_counts counts;
	struct pdf_document *file;
	struct stat status;
	int written;
	int result;
	int error;

	/* The temporary name beside the copy. */
	written = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	if (written < 0 || (size_t)written >= sizeof(temporary))
		return ENAMETOOLONG;

	/* The saved notebook, read. */
	error = pdf_document_open(from, &file);
	if (error != 0)
		return error;

	/* The copy, without the edit data. */
	memset(&counts, 0, sizeof(counts));
	error = pdf_document_save_clean(file, temporary, NOTES_ATTACHMENT_NAME, &counts);
	pdf_document_close(file);
	if (error == 0)
		error = save_sync(temporary);
	if (error != 0) {
		(void)unlink(temporary);
		return error;
	}

	/* Replaces an old file of the name in one step. */
	result = rename(temporary, path);
	if (result != 0) {
		error = errno;
		(void)unlink(temporary);
		return error;
	}

	/* Puts the new name on the disk. */
	(void)save_sync_folder(path);

	/* The copy's size and counts. */
	*bytes = 0;
	result = stat(path, &status);
	if (result == 0)
		*bytes = (size_t)status.st_size;
	*objects = counts.objects;
	*dropped = counts.dropped;
	return 0;
}

/*
 * Opens a PDF into an empty document, and tells what it found (one of
 * NOTES_OPENED_*).
 *
 * A notebook Notes saved is rebuilt from its edit data.  Another
 * program's PDF becomes the document's base (design-pdf.md section 3):
 * each of its pages is drawn under the strokes, and the saves add to it.
 * Returns 0, EACCES for an encrypted PDF and EPERM for a signed one (Notes
 * does not write on them), the reader's errno value for a file it cannot
 * read (PDF_EFORMAT for a damaged one, ENOTSUP for one it does not read
 * yet), or ENOMEM.
 */
int
notes_open_pdf(
	const char *path,
	struct notes_document *document,
	unsigned *opened)
{
	struct notes_document saved;
	struct pdf_document *file;
	unsigned char *data;
	const void *edit;
	size_t edit_size;
	size_t changed;
	size_t size;
	int is_signed;
	int encrypted;
	int decoded;
	int rebase;
	int error;

	/* The file's bytes. */
	error = read_file(path, SAVE_FILE_MAX, &data, &size);
	if (error != 0)
		return error;

	/* The PDF; one the reader refuses because it is encrypted is told apart. */
	error = pdf_document_open_memory(data, size, &file);
	if (error == ENOTSUP) {
		free(data);
		encrypted = 0;
		(void)pdf_document_encrypted(path, &encrypted);
		if (encrypted)
			return EACCES;
		return ENOTSUP;
	}

	/* Any other file the reader cannot read (one that needs a password is encrypted too). */
	if (error != 0) {
		free(data);
		return error;
	}

	/* Refuses an encrypted PDF, which the reader opens with its empty user password but Notes does not write on. */
	encrypted = 0;
	error = pdf_document_encrypted(path, &encrypted);
	if (error != 0)
		encrypted = 0;
	if (encrypted) {
		pdf_document_close(file);
		free(data);
		return EACCES;
	}

	/* Refuses a signed PDF, whose signatures a new revision could invalidate. */
	error = pdf_document_signed(file, &is_signed);
	if (error == 0 && is_signed)
		error = EPERM;
	if (error != 0) {
		pdf_document_close(file);
		free(data);
		return error;
	}

	/* The edit data, when Notes wrote on the file before. */
	decoded = 0;
	memset(&saved, 0, sizeof(saved));
	error = pdf_document_find_attachment_type(file, NOTES_ATTACHMENT_NAME, NOTES_ATTACHMENT_TYPE, &edit, &edit_size);
	if (error == 0) {
		error = notes_decode_document(edit, edit_size, &saved);
		if (error == 0)
			decoded = 1;
	}

	/* A notebook written on another program's PDF, whose newest revision is Notes' own: its base again, its edits checked. */
	rebase = 0;
	if (decoded && saved.base_size != 0U) {
		error = open_base(&saved, data, size, file);
		if (error == 0)
			error = open_edits(&saved, file);
		if (error == 0) {
			pdf_document_close(file);
			free(data);
			*document = saved;
			*opened = NOTES_OPENED_ANNOTATED;
			return 0;
		}

		/* Edits that do not match the file: it is opened as it is shown. */
		if (error == ESTALE && saved.base != NULL)
			rebase = 1;
	}

	/* A notebook Notes saved, unchanged since, its edits checked. */
	if (decoded && saved.base_size == 0U) {
		error = open_own(&saved, file);
		if (error == 0) {
			error = open_edits(&saved, file);
			if (error == ESTALE)
				rebase = 1;
		}

		/* Its edits match: the notebook as it was saved. */
		if (error == 0) {
			pdf_document_close(file);
			free(data);
			*document = saved;
			*opened = NOTES_OPENED_NOTES;
			return 0;
		}
	}

	/* Memory gone is a failure, not another program's change. */
	if (decoded && error == ENOMEM) {
		notes_document_free(&saved);
		pdf_document_close(file);
		free(data);
		return ENOMEM;
	}

	/* Any other PDF is the base as it stands. */
	changed = 0;
	error = open_as_base(document, &saved, decoded, rebase, data, size, file, &changed);
	if (decoded)
		notes_document_free(&saved);
	free(data);
	if (error != 0) {
		pdf_document_close(file);
		return error;
	}

	/* Succeeded: another program's PDF, a notebook another program changed, or one whose edits did not match. */
	*opened = NOTES_OPENED_FOREIGN;
	if (decoded)
		*opened = NOTES_OPENED_CHANGED;
	if (rebase)
		*opened = NOTES_OPENED_REBASED;
	return 0;
}

/*
 * Gives a document recovered from its journal the base its edit data
 * names: the first base_size bytes of the file, which must still hash as
 * they did.
 *
 * Returns 0 (also for a notebook without a base), ESTALE when the file no
 * longer starts with the base, or the errno value of reading it.
 */
int
notes_attach_base(
	const char *path,
	struct notes_document *document)
{
	struct pdf_document *base;
	unsigned char digest[32];
	unsigned char *data;
	size_t size;
	int differs;
	int error;

	/* A notebook written whole has no base. */
	if (document->base_size == 0U)
		return 0;

	/* The file, which must be at least as long as the base. */
	error = read_file(path, SAVE_FILE_MAX, &data, &size);
	if (error != 0)
		return error;
	if (size < document->base_size) {
		free(data);
		return ESTALE;
	}

	/* The base's bytes must be the ones the edit data recorded. */
	hash_bytes(data, (size_t)document->base_size, digest);
	differs = memcmp(digest, document->base_hash, sizeof(digest));
	if (differs != 0) {
		free(data);
		return ESTALE;
	}

	/* The base, read from those bytes. */
	error = pdf_document_open_memory(data, (size_t)document->base_size, &base);
	free(data);
	if (error != 0)
		return error;

	/* Succeeded: the document writes on its base again. */
	document->base = base;
	return 0;
}

/* Writes a notebook Notes made, whole, to a file. */
static int
save_whole(
	struct notes_document *document,
	const char *temporary)
{
	struct pdf_writer *writer;
	struct save_paint paint;
	size_t index;
	int result;
	int error;

	/* The writer. */
	memset(&paint, 0, sizeof(paint));
	error = pdf_writer_create(&writer);
	if (error != 0)
		return error;

	/* The pages' content compressed, for a notebook with the drawing tools. */
	error = save_compress(writer, document);
	if (error != 0) {
		pdf_writer_destroy(writer);
		return error;
	}

	/* The identifier a saved document keeps, and its creation date. */
	if (document->has_pdf_id) {
		error = pdf_writer_set_document_id(writer, document->pdf_id);
		if (error != 0) {
			pdf_writer_destroy(writer);
			return error;
		}
	}

	/* The creation date is the document's; the modification date is the save's. */
	error = pdf_writer_set_dates(writer, (time_t)(document->time_base / 1000U), 0);
	if (error != 0) {
		pdf_writer_destroy(writer);
		return error;
	}

	/* Each page with its strokes. */
	for (index = 0; index < document->page_count; index++) {
		error = save_page(writer, document, index, &paint);
		if (error != 0) {
			pdf_writer_destroy(writer);
			return error;
		}
	}

	/* The edit data, with each page's content hash. */
	error = save_edit_data(writer, document);
	if (error != 0) {
		pdf_writer_destroy(writer);
		return error;
	}

	/* Writes the file. */
	error = pdf_writer_save(writer, temporary);
	if (error != 0) {
		pdf_writer_destroy(writer);
		return error;
	}

	/* Keeps the identifier the first save made. */
	result = pdf_writer_get_document_id(writer, document->pdf_id);
	if (result == 0)
		document->has_pdf_id = 1;
	pdf_writer_destroy(writer);

	/* Succeeded: the file holds the notebook. */
	return 0;
}

/*
 * Writes a notebook written on a base PDF: the base's bytes and a revision
 * with the strokes, to a file.
 *
 * Each page of the base is listed in order: kept when nothing is drawn on
 * it, drawn over, or -- a page whose content was Notes' own strokes --
 * drawn anew in place of its content.  The pages Notes added go between
 * them where they stand in the notebook.
 */
static int
save_update(
	struct notes_document *document,
	const char *temporary)
{
	struct pdf_writer *writer;
	struct pdf_page_editor *editor;
	struct notes_page *page;
	struct save_paint paint;
	size_t index;
	int error;

	/* The writer that adds to the base; a signed base is refused. */
	memset(&paint, 0, sizeof(paint));
	error = pdf_writer_create_update(document->base, &writer);
	if (error != 0)
		return error;

	/* The revision's content compressed, for a notebook with the drawing tools. */
	error = save_compress(writer, document);
	if (error != 0) {
		pdf_writer_destroy(writer);
		return error;
	}

	/* Each page, in the notebook's order. */
	for (index = 0; index < document->page_count; index++) {
		page = document->pages[index];

		/* A page of the base without strokes or edits stays as the base has it. */
		if (page->origin == NOTES_ORIGIN_OVER && page->stroke_count == 0U && page->edit_count == 0U) {
			error = pdf_writer_keep_page(writer, page->source);
			if (error != 0) {
				pdf_writer_destroy(writer);
				return error;
			}

			/* Nothing is drawn on it. */
			continue;
		}

		/*
		 * A page of the base: edited (its content changed, ws175-p007), or
		 * drawn over; replaced when its content was Notes' own.
		 */
		if (page->origin == NOTES_ORIGIN_OVER && page->edit_count > 0U) {
			error = notes_page_editor(document, index, &editor);
			if (error == 0)
				error = pdf_writer_begin_page_edited(writer, editor);
		} else if (page->origin == NOTES_ORIGIN_OVER) {
			error = pdf_writer_begin_page_over(writer, page->source, PDF_PAGE_OVERLAY);
		} else if (page->origin == NOTES_ORIGIN_REPLACE) {
			error = pdf_writer_begin_page_over(writer, page->source, PDF_PAGE_REPLACE);
		} else {
			error = pdf_writer_begin_page(writer, page->width, page->height);
		}

		/* The images inserted on a page of Notes' own, under its strokes (design.md [N2]). */
		if (error == 0 && page->origin != NOTES_ORIGIN_OVER && page->edit_count > 0U) {
			error = notes_page_editor(document, index, &editor);
			if (error == 0)
				error = pdf_writer_draw_page_editor(writer, editor);
		}

		/* A page that could not be begun ends the save. */
		if (error != 0) {
			pdf_writer_destroy(writer);
			return error;
		}

		/* The page's strokes. */
		error = save_strokes(writer, page, &paint);
		if (error != 0) {
			pdf_writer_destroy(writer);
			return error;
		}

		/* The page is finished. */
		error = pdf_writer_end_page(writer);
		if (error != 0) {
			pdf_writer_destroy(writer);
			return error;
		}
	}

	/* The edit data, with the content hash of each page whose hash is known. */
	error = save_edit_data(writer, document);
	if (error != 0) {
		pdf_writer_destroy(writer);
		return error;
	}

	/* Writes the file: the base, then the revision. */
	error = pdf_writer_save(writer, temporary);
	pdf_writer_destroy(writer);
	if (error != 0)
		return error;

	/* Succeeded: the file holds the base and the notebook's revision. */
	return 0;
}

/*
 * Writes one page of a notebook written whole: the images inserted on it
 * (ws175-p007), then each stroke's outline filled in its colour.
 */
static int
save_page(
	struct pdf_writer *writer,
	struct notes_document *document,
	size_t index,
	struct save_paint *paint)
{
	struct pdf_page_editor *editor;
	struct notes_page *page;
	int error;

	/* The page. */
	page = document->pages[index];
	error = pdf_writer_begin_page(writer, page->width, page->height);
	if (error != 0)
		return error;

	/* Its inserted images. */
	if (page->edit_count > 0U) {
		error = notes_page_editor(document, index, &editor);
		if (error == 0)
			error = pdf_writer_draw_page_editor(writer, editor);
		if (error != 0)
			return error;
	}

	/* Its strokes. */
	error = save_strokes(writer, page, paint);
	if (error != 0)
		return error;

	/* The page is finished. */
	error = pdf_writer_end_page(writer);
	if (error != 0)
		return error;

	/* Succeeded: the page is written. */
	return 0;
}

/* Writes a page's strokes, bottom first: each one's shapes, each filled once. */
static int
save_strokes(
	struct pdf_writer *writer,
	struct notes_page *page,
	struct save_paint *paint)
{
	struct notes_stroke *stroke;
	size_t index;
	size_t shape;
	int error;

	/* Each stroke, bottom first. */
	for (index = 0; index < page->stroke_count; index++) {
		/* The shapes, the same polygons the screen draws. */
		stroke = page->strokes[index];
		error = notes_stroke_outline(stroke);
		if (error != 0)
			return error;

		/* Each shape in its order. */
		for (shape = 0; shape < stroke->shape_count; shape++) {
			error = save_shape(writer, stroke, &stroke->shapes[shape], paint);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: every stroke is written. */
	return 0;
}

/*
 * Writes one shape of a stroke: in the stroke's colour at its opacity times
 * the shape's, laid over by its blend mode; a shape of the grain as a plain
 * fill of the grain's least opacity under a fill of the grain, which add up
 * to 1 - (1 - a g) (1 - a h) at a texel of the grain h, as the screen draws.
 */
static int
save_shape(
	struct pdf_writer *writer,
	const struct notes_stroke *stroke,
	const struct notes_shape *shape,
	struct save_paint *paint)
{
	enum pdf_blend_mode blend;
	double red;
	double green;
	double blue;
	double alpha;
	int error;

	/* The colour and its opacity, times the shape's. */
	red = (double)((stroke->color >> 24) & 0xffU) / 255.0;
	green = (double)((stroke->color >> 16) & 0xffU) / 255.0;
	blue = (double)((stroke->color >> 8) & 0xffU) / 255.0;
	alpha = (double)(stroke->color & 0xffU) / 255.0;
	alpha *= (double)shape->alpha;

	/* The blend mode the next colour selects. */
	blend = PDF_BLEND_NORMAL;
	if (shape->blend == NOTES_BLEND_MULTIPLY)
		blend = PDF_BLEND_MULTIPLY;
	error = pdf_writer_set_blend_mode(writer, blend);
	if (error != 0)
		return error;

	/* A plain shape, filled once. */
	if (shape->paint != NOTES_PAINT_GRAIN) {
		error = pdf_writer_set_fill_color(writer, red, green, blue, alpha);
		if (error != 0)
			return error;
		error = save_shape_path(writer, shape);
		if (error != 0)
			return error;

		/* Succeeded: the shape is painted. */
		return 0;
	}

	/* The grain's least opacity, a plain fill under the grain. */
	error = pdf_writer_set_fill_color(writer, red, green, blue, alpha * (double)NOTES_GRAIN_FLOOR);
	if (error != 0)
		return error;
	error = save_shape_path(writer, shape);
	if (error != 0)
		return error;

	/* The grain, added to the document the first time. */
	error = save_grain(writer, paint);
	if (error != 0)
		return error;

	/* The grain in the colour at the shape's opacity, in squares of the grain's cell from the page's top left. */
	error = pdf_writer_set_fill_grain(writer, paint->grain, red, green, blue, alpha, NOTES_GRAIN_CELL);
	if (error != 0)
		return error;
	error = save_shape_path(writer, shape);
	if (error != 0)
		return error;

	/* Succeeded: the shape is painted through the grain. */
	return 0;
}

/* Fills a shape's polygon with the nonzero rule, each contour a subpath. */
static int
save_shape_path(
	struct pdf_writer *writer,
	const struct notes_shape *shape)
{
	size_t contour;
	size_t first;
	size_t corner;
	int error;

	/* A polygon of one contour, as the pen's outline has always been written. */
	if (shape->contours == NULL) {
		error = pdf_writer_fill_outline(writer, shape->polygon, shape->corner_count);
		if (error != 0)
			return error;

		/* Succeeded: the polygon is painted. */
		return 0;
	}

	/* Each contour: a subpath from its first corner, closed. */
	first = 0;
	for (contour = 0; contour < shape->contour_count; contour++) {
		error = pdf_writer_move_to(writer, shape->polygon[first].x, shape->polygon[first].y);
		if (error != 0)
			return error;

		/* An edge to each of its other corners. */
		for (corner = first + 1U; corner < first + shape->contours[contour]; corner++) {
			error = pdf_writer_line_to(writer, shape->polygon[corner].x, shape->polygon[corner].y);
			if (error != 0)
				return error;
		}

		/* Back to its first corner. */
		error = pdf_writer_close_path(writer);
		if (error != 0)
			return error;
		first += shape->contours[contour];
	}

	/* Paints every contour at once, so that a hole stays empty. */
	error = pdf_writer_fill(writer, PDF_FILL_NONZERO);
	if (error != 0)
		return error;

	/* Succeeded: the contours are painted. */
	return 0;
}

/* Adds the pencil's grain to the writer's document, once. */
static int
save_grain(
	struct pdf_writer *writer,
	struct save_paint *paint)
{
	unsigned char *tile;
	int error;

	/* A grain added already. */
	if (paint->has_grain)
		return 0;

	/* Allocates the tile. */
	tile = malloc((size_t)NOTES_GRAIN_SIDE * NOTES_GRAIN_SIDE);
	if (tile == NULL)
		return ENOMEM;

	/* Makes it and gives it to the writer, which keeps a copy. */
	notes_grain_tile(tile);
	error = pdf_writer_add_grain(writer, tile, NOTES_GRAIN_SIDE, NOTES_GRAIN_SIDE, &paint->grain);
	free(tile);
	if (error != 0)
		return error;

	/* Succeeded: the strokes share the grain. */
	paint->has_grain = 1;
	return 0;
}

/*
 * Has the writer compress the pages' content with Flate when the notebook
 * has the drawing tools (edit data 3.0, whose readers decode it); other
 * notebooks are written as before, which older readers compare unchanged.
 */
static int
save_compress(
	struct pdf_writer *writer,
	const struct notes_document *document)
{
	int drawn;
	int error;

	/* A notebook without the drawing tools is not compressed. */
	drawn = notes_document_new_tools(document);
	if (!drawn)
		return 0;

	/* Compresses the content at the save. */
	error = pdf_writer_set_content_flate(writer, 1);
	if (error != 0)
		return error;

	/* Succeeded: the content is compressed. */
	return 0;
}

/*
 * Records each page's content hash in the document and attaches the edit
 * data to the writer.
 *
 * A page whose hash the writer does not know (a page of the base kept or
 * drawn over, whose content includes the base's own) records none.
 */
static int
save_edit_data(
	struct pdf_writer *writer,
	struct notes_document *document)
{
	struct notes_buffer edit;
	size_t index;
	int error;

	/* Each page's content hash, which the edit data records for the next opening to compare. */
	for (index = 0; index < document->page_count; index++) {
		error = pdf_writer_get_page_content_hash(writer, index, document->pages[index]->content_hash);
		if (error == ENOENT) {
			memset(document->pages[index]->content_hash, 0, sizeof(document->pages[index]->content_hash));
			error = 0;
		}

		/* Any other failure is the page's content's. */
		if (error != 0)
			return error;
	}

	/* The edit data, attached. */
	notes_buffer_init(&edit);
	error = notes_encode_document(document, &edit);
	if (error == 0)
		error = pdf_writer_attach_file(writer, NOTES_ATTACHMENT_NAME, NOTES_ATTACHMENT_TYPE, edit.data, edit.length);
	notes_buffer_free(&edit);
	if (error != 0)
		return error;

	/* Succeeded: the writer carries the edit data. */
	return 0;
}

/* Puts a file's bytes on the disk. */
static int
save_sync(
	const char *path)
{
	int descriptor;
	int status;
	int error;

	/* Opens the file. */
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return errno;

	/* Flushes it. */
	error = 0;
	status = fsync(descriptor);
	if (status != 0)
		error = errno;
	(void)close(descriptor);
	if (error != 0)
		return error;

	/* Succeeded: the bytes are on the disk. */
	return 0;
}

/* Puts the folder of a file on the disk, so that a new name survives a crash. */
static int
save_sync_folder(
	const char *path)
{
	char folder[SAVE_PATH_MAX];
	char *slash;
	size_t length;
	int error;

	/* The folder's path; a bare name is in the current folder. */
	length = strlen(path);
	if (length >= sizeof(folder))
		return ENAMETOOLONG;
	memcpy(folder, path, length + 1U);
	slash = strrchr(folder, '/');
	if (slash == NULL)
		return 0;
	if (slash == folder)
		slash[1] = '\0';
	else
		*slash = '\0';

	/* Flushes it. */
	error = save_sync(folder);
	if (error != 0)
		return error;

	/* Succeeded: the name is on the disk. */
	return 0;
}

/*
 * Checks a notebook Notes wrote whole against its file: the file has the
 * pages the edit data describes, and each page whose hash was recorded is
 * as Notes saved it.  On success the document takes the file's identifier.
 * Returns 0, or ESTALE for a file another program changed.
 */
static int
open_own(
	struct notes_document *saved,
	struct pdf_document *file)
{
	size_t pages;
	size_t index;
	int unchanged;
	int error;

	/* The file must have the pages the edit data describes. */
	pages = pdf_document_page_count(file);
	if (pages != saved->page_count)
		return ESTALE;

	/* Each page must be as Notes saved it. */
	for (index = 0; index < saved->page_count; index++) {
		unchanged = page_unchanged(saved->pages[index], file, index);
		if (!unchanged)
			return ESTALE;
	}

	/* The file's permanent identifier, which the next save keeps. */
	error = pdf_document_get_id(file, saved->pdf_id);
	if (error == 0)
		saved->has_pdf_id = 1;

	/* Succeeded: the notebook as it was saved, unchanged since. */
	return 0;
}

/*
 * Gives a notebook written on a base PDF its base back from the file: the
 * file must start with the base's bytes, and its newest revision must be
 * the only one added to them -- the one Notes saved.  Returns 0, or ESTALE
 * when another program changed the file since.
 */
static int
open_base(
	struct notes_document *saved,
	const unsigned char *data,
	size_t size,
	struct pdf_document *file)
{
	struct pdf_document *base;
	unsigned char digest[32];
	size_t base_newest;
	size_t base_previous;
	size_t file_newest;
	size_t file_previous;
	size_t file_pages;
	size_t base_pages;
	size_t index;
	int differs;
	int error;

	/* The file must start with the base's bytes. */
	if (size < saved->base_size)
		return ESTALE;
	hash_bytes(data, (size_t)saved->base_size, digest);
	differs = memcmp(digest, saved->base_hash, sizeof(digest));
	if (differs != 0)
		return ESTALE;

	/* The base, read from those bytes. */
	error = pdf_document_open_memory(data, (size_t)saved->base_size, &base);
	if (error != 0)
		return ESTALE;

	/* The file's newest revision must follow the base's directly. */
	(void)pdf_document_get_revision(base, &base_newest, &base_previous);
	(void)pdf_document_get_revision(file, &file_newest, &file_previous);
	if (file_previous != base_newest) {
		pdf_document_close(base);
		return ESTALE;
	}

	/* The file must have the notebook's pages, and each page of the base a page of it. */
	file_pages = pdf_document_page_count(file);
	if (file_pages != saved->page_count) {
		pdf_document_close(base);
		return ESTALE;
	}

	/* Each page of the base the notebook names must be one the base has. */
	base_pages = pdf_document_page_count(base);
	for (index = 0; index < saved->page_count; index++) {
		if (saved->pages[index]->origin == NOTES_ORIGIN_NEW)
			continue;
		if (saved->pages[index]->source >= base_pages) {
			pdf_document_close(base);
			return ESTALE;
		}
	}

	/* Succeeded: the notebook writes on its base again. */
	saved->base = base;
	saved->dirty = 0;
	return 0;
}

/*
 * Makes a document whose base is a PDF as it stands: each page of the file
 * a page of the base, drawn under the strokes -- except that a page of a
 * notebook Notes saved (the decoded edit data) that no other program
 * changed keeps its strokes, editable, to be written anew in place, when
 * every image inserted on it reads back from the file (design.md [N1]).
 * The pages of saved that stay are taken from it.  With rebase, every page
 * is background (a notebook whose edits did not match).  changed counts
 * the notebook's pages that became background.
 */
static int
open_as_base(
	struct notes_document *document,
	struct notes_document *saved,
	int decoded,
	int rebase,
	const unsigned char *data,
	size_t size,
	struct pdf_document *file,
	size_t *changed)
{
	struct notes_page **pages;
	struct notes_page *page;
	struct pdf_page_box box;
	size_t count;
	size_t index;
	int unchanged;
	int error;

	/* The pages; a file without any gets one blank page of Notes' own. */
	count = pdf_document_page_count(file);
	pages = calloc(count + 1U, sizeof(*pages));
	if (pages == NULL)
		return ENOMEM;

	/* Each page of the file, in order. */
	for (index = 0; index < count; index++) {
		/* A page of the notebook no other program changed keeps its strokes, and its images when they read back. */
		unchanged = 0;
		if (decoded && !rebase && index < saved->page_count) {
			if (saved->pages[index]->origin != NOTES_ORIGIN_OVER)
				unchanged = page_unchanged(saved->pages[index], file, index);
			if (unchanged && saved->pages[index]->edit_count > 0U)
				unchanged = open_images(saved->pages[index], file, index) == 0;
		}

		/* Such a page is taken from the notebook, to be written anew in place. */
		if (unchanged) {
			page = saved->pages[index];
			saved->pages[index] = NULL;
			page->origin = NOTES_ORIGIN_REPLACE;
			page->source = index;
			pages[index] = page;
			continue;
		}

		/* Any other page is the base's, drawn under the strokes, at its size as shown. */
		page = NULL;
		error = pdf_document_page_box(file, index, &box);
		if (error == 0) {
			page = notes_page_create((float)box.width, (float)box.height, NOTES_BACKGROUND_PDF);
			if (page == NULL)
				error = ENOMEM;
		}

		/* A page that cannot be made ends the opening. */
		if (error != 0) {
			for (index = 0; index < count; index++)
				notes_page_free(pages[index]);
			free(pages);
			return error;
		}

		/* The page stands for the file's page; a notebook's page that became background is counted. */
		page->origin = NOTES_ORIGIN_OVER;
		page->source = index;
		pages[index] = page;
		if (decoded)
			(*changed)++;
	}

	/* A file without pages gets a blank one. */
	if (count == 0U) {
		pages[0] = notes_page_create(NOTES_PAGE_WIDTH, NOTES_PAGE_HEIGHT, NOTES_BACKGROUND_PLAIN);
		if (pages[0] == NULL) {
			free(pages);
			return ENOMEM;
		}

		/* The blank page is the notebook's only one. */
		count = 1U;
	}

	/* The document: the notebook's numbering and time when there was one. */
	memset(document, 0, sizeof(*document));
	document->pages = pages;
	document->page_count = count;
	document->page_capacity = count;
	document->next_id = 1U;
	document->time_base = (uint64_t)time(NULL) * 1000U;
	if (decoded) {
		document->next_id = saved->next_id;
		document->time_base = saved->time_base;
	}

	/* The file is the base, by its bytes' length and hash. */
	document->base = file;
	document->base_size = (uint64_t)size;
	hash_bytes(data, size, document->base_hash);

	/* Succeeded: the notebook writes on the file as it stands. */
	return 0;
}

/*
 * Checks a notebook's edits against its file (design.md [H3]): every
 * object an edit names is on its page of the base, and every image reads
 * back from the file's page that uses it.  Returns 0, ESTALE when one does
 * not, or ENOMEM.
 */
static int
open_edits(
	struct notes_document *saved,
	struct pdf_document *file)
{
	size_t index;
	int error;

	/* Each page with edits. */
	for (index = 0; index < saved->page_count; index++) {
		if (saved->pages[index]->edit_count == 0U)
			continue;

		/* Its objects, then its images. */
		error = open_keys(saved, index);
		if (error == 0)
			error = open_images(saved->pages[index], file, index);
		if (error == ENOMEM)
			return ENOMEM;
		if (error != 0)
			return ESTALE;
	}

	/* Succeeded: the edits are the file's. */
	return 0;
}

/*
 * Checks that every object of the page's own an edit names is on the
 * base's page (by its kind, place, length and fingerprint).  Returns 0,
 * ESTALE, or the failure of opening the page's editor.
 */
static int
open_keys(
	struct notes_document *saved,
	size_t index)
{
	struct pdf_page_editor *editor;
	struct notes_page *page;
	size_t found;
	size_t at;
	int error;

	/* A page with edits of its own objects only on a base. */
	page = saved->pages[index];
	error = 0;
	editor = NULL;
	for (at = 0; at < page->edit_count && error == 0; at++) {
		if ((page->edits[at]->flags & NOTES_EDIT_INSERTED) != 0U)
			continue;
		if (page->origin != NOTES_ORIGIN_OVER || saved->base == NULL) {
			error = ESTALE;
			break;
		}

		/* The base's page, scanned once. */
		if (editor == NULL)
			error = pdf_page_editor_open(saved->base, page->source, &editor);
		if (error != 0)
			break;

		/* The object by its key. */
		error = pdf_page_editor_find(editor, &page->edits[at]->key, &found);
		if (error == ENOENT)
			error = ESTALE;
	}

	/* The editor goes. */
	pdf_page_editor_close(editor);
	return error;
}

/*
 * Reads back the images a page's edits hold from the file's page (their
 * private keys, libpdf's pdf_page_editor_read_image): each image without
 * its bytes gets them.  Returns 0, ESTALE when one is not there, ENOMEM,
 * or the failure of opening the page's editor.
 */
static int
open_images(
	struct notes_page *page,
	struct pdf_document *file,
	size_t index)
{
	struct pdf_page_editor *editor;
	struct pdf_image_source source;
	struct notes_image *image;
	void *owned;
	size_t at;
	int error;

	/* Each image without its bytes. */
	error = 0;
	editor = NULL;
	for (at = 0; at < page->edit_count && error == 0; at++) {
		image = page->edits[at]->image;
		if (image == NULL || image->data != NULL)
			continue;

		/* The file's page, scanned once. */
		if (editor == NULL)
			error = pdf_page_editor_open(file, index, &editor);
		if (error != 0)
			break;

		/* The image by its number, as the PDF has it, the same size. */
		memset(&source, 0, sizeof(source));
		source.size = sizeof(source);
		owned = NULL;
		error = pdf_page_editor_read_image(editor, image->id, &source, &owned);
		if (error == 0 && (source.width != image->width || source.height != image->height))
			error = ESTALE;
		if (error == 0)
			error = notes_image_set_bytes(image, &source);
		free(owned);
		if (error != 0 && error != ENOMEM)
			error = ESTALE;
	}

	/* The editor goes. */
	pdf_page_editor_close(editor);
	return error;
}

/*
 * Tells whether a page of a notebook is as Notes saved it: its recorded
 * content hash matches the file's page.  A page without a recorded hash is
 * taken as it is; a page the reader cannot hash (for example compressed by
 * another program) has changed.
 */
static int
page_unchanged(
	const struct notes_page *page,
	struct pdf_document *file,
	size_t index)
{
	unsigned char digest[32];
	unsigned char zero[32];
	int differs;
	int error;

	/* A page whose hash was not recorded is taken as it is. */
	memset(zero, 0, sizeof(zero));
	differs = memcmp(page->content_hash, zero, sizeof(zero));
	if (differs == 0)
		return 1;

	/* The page's content in the file. */
	error = pdf_document_page_content_hash(file, index, digest);
	if (error != 0)
		return 0;

	/* Another program changed the page. */
	differs = memcmp(page->content_hash, digest, sizeof(digest));
	if (differs != 0)
		return 0;

	/* The page is as Notes saved it. */
	return 1;
}

/* Reads a whole file, of at most a limit of bytes, into a new allocation. */
static int
read_file(
	const char *path,
	size_t limit,
	unsigned char **data,
	size_t *size)
{
	unsigned char *buffer;
	unsigned char *grown;
	size_t length;
	size_t capacity;
	size_t got;
	FILE *stream;
	int failed;

	/* Opens the file. */
	stream = fopen(path, "rb");
	if (stream == NULL)
		return errno;

	/* Reads chunks until the end, growing the buffer ahead of each one. */
	buffer = NULL;
	length = 0;
	capacity = 0;
	for (;;) {
		/* Refuses a file past the limit. */
		if (length > limit) {
			free(buffer);
			fclose(stream);
			return EFBIG;
		}

		/* Makes room for the next chunk. */
		if (capacity - length < SAVE_READ_CHUNK) {
			capacity = capacity * 2U + SAVE_READ_CHUNK;
			grown = realloc(buffer, capacity);
			if (grown == NULL) {
				free(buffer);
				fclose(stream);
				return ENOMEM;
			}

			/* The bytes read so far are in the larger buffer. */
			buffer = grown;
		}

		/* Reads the chunk; a short one is the end or an error. */
		got = fread(buffer + length, 1U, SAVE_READ_CHUNK, stream);
		length += got;
		if (got < SAVE_READ_CHUNK)
			break;
	}

	/* Reports a read error rather than a short file. */
	failed = ferror(stream);
	fclose(stream);
	if (failed) {
		free(buffer);
		return EIO;
	}

	/* Succeeded: the caller owns the file's bytes. */
	*data = buffer;
	*size = length;
	return 0;
}

/* Computes the SHA-256 of bytes. */
static void
hash_bytes(
	const unsigned char *data,
	size_t size,
	unsigned char digest[32])
{
	SHA2_CTX context;

	/* The bytes in one pass. */
	SHA256Init(&context);
	SHA256Update(&context, data, size);
	SHA256Final(digest, &context);
}
