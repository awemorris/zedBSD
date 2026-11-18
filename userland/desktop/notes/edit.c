/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The edits of the PDF's objects in Notes (ws175-p007, plan/ws175/
 * phase001/design.md section 6): the images put in an object's place or
 * inserted, and the edits of each page -- the states of the page's own
 * objects (deleted, placed, given an image) named by their keys, and the
 * objects inserted on it named by their numbers.
 *
 * Two primitive changes alter a page's edits, like the strokes': an edit
 * put at a place among them, and an edit taken off by the object it names.
 * The user's edits (document.c) are made of them.  The edits are what the
 * document is; each page's libpdf editor, which draws and writes them, is
 * a cache made from them when it is asked for (notes_page_editor).
 *
 * An image keeps its bytes as compressed as they came (design.md [N3]): a
 * JPEG's, a PNG file's, a PNG's rows read back from a PDF, or RGBA pixels
 * the image compresses itself.  It is counted by the edits that hold it.
 */

#include "notes.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <compat/zlib/zlib.h>

/* The fewest edits a page's array grows to. */
#define EDIT_GROW_MIN		8U

/* The grid of a transform's first four numbers. */
#define EDIT_SCALE_UNITS	65536.0f

/* The largest image side, libpdf's, which keeps width x height x 4 from overflowing. */
#define EDIT_IMAGE_SIDE_MAX	16384U

static int edit_apply(struct pdf_page_editor *editor, const struct notes_edit *edit, unsigned *result);
static int edit_grow(struct notes_page *page);
static float edit_grid(float value, float units);

/*
 * Makes an image of a kind from its bytes (copied; NULL data for an image
 * whose bytes come later, from a PDF), with the document's next number,
 * held once by the caller.  Returns NULL for a kind, size or orientation
 * that is not an image's, or when memory runs out.
 */
struct notes_image *
notes_image_create(
	struct notes_document *document,
	unsigned kind,
	const void *data,
	size_t size,
	size_t width,
	size_t height,
	int components,
	int orientation)
{
	struct notes_image *image;

	/* A known kind, a size within libpdf's, an EXIF orientation. */
	if (kind < NOTES_IMAGE_JPEG || kind > NOTES_IMAGE_RGBA || orientation < 0 || orientation > 8)
		return NULL;
	if (width == 0 || height == 0 || width > EDIT_IMAGE_SIDE_MAX || height > EDIT_IMAGE_SIDE_MAX)
		return NULL;
	if (data != NULL && size == 0)
		return NULL;

	/* The image. */
	image = calloc(1, sizeof(*image));
	if (image == NULL)
		return NULL;

	/* Its bytes. */
	if (data != NULL) {
		image->data = malloc(size);
		if (image->data == NULL) {
			free(image);
			return NULL;
		}

		/* As they came. */
		memcpy(image->data, data, size);
		image->size = size;
	}

	/* Succeeded: the next number, one reference. */
	image->id = document->next_id;
	document->next_id++;
	image->refs = 1U;
	image->kind = kind;
	image->width = width;
	image->height = height;
	image->components = components;
	image->orientation = orientation;
	return image;
}

/*
 * Lets go of one reference to an image; the last frees it.
 */
void
notes_image_release(
	struct notes_image *image)
{
	/* No image. */
	if (image == NULL)
		return;

	/* Still held by another. */
	image->refs--;
	if (image->refs > 0U)
		return;

	/* The last: the bytes and the image. */
	free(image->data);
	free(image);
}

/*
 * Gives an image as libpdf's editor takes it: its own bytes, or RGBA
 * pixels decompressed into a new buffer (*owned, which the caller frees;
 * NULL otherwise).  Returns 0, ENOENT for an image whose bytes are not
 * known, EINVAL for compressed pixels that are not the image's, or
 * ENOMEM.
 */
int
notes_image_source(
	const struct notes_image *image,
	struct pdf_image_source *source,
	void **owned)
{
	unsigned char *pixels;
	uLongf length;
	int status;

	/* An image with its bytes. */
	*owned = NULL;
	if (image->data == NULL)
		return ENOENT;

	/* Its bytes, size, orientation and number. */
	memset(source, 0, sizeof(*source));
	source->size = sizeof(*source);
	source->data = image->data;
	source->bytes = image->size;
	source->width = image->width;
	source->height = image->height;
	source->components = image->components;
	source->orientation = image->orientation;
	source->id = image->id;

	/* A JPEG, a PNG file, a PNG's rows: as they are. */
	if (image->kind == NOTES_IMAGE_JPEG) {
		source->kind = PDF_IMAGE_SOURCE_JPEG;
		return 0;
	}

	/* A PNG file. */
	if (image->kind == NOTES_IMAGE_PNG) {
		source->kind = PDF_IMAGE_SOURCE_PNG;
		return 0;
	}

	/* A PNG's rows. */
	if (image->kind == NOTES_IMAGE_ROWS) {
		source->kind = PDF_IMAGE_SOURCE_IDAT;
		return 0;
	}

	/* RGBA: the pixels decompressed, exactly as many as the image has. */
	length = (uLongf)(image->width * image->height * 4U);
	pixels = malloc((size_t)length + 1U);
	if (pixels == NULL)
		return ENOMEM;
	status = uncompress(pixels, &length, image->data, (uLong)image->size);
	if (status != Z_OK || (size_t)length != image->width * image->height * 4U) {
		free(pixels);
		return EINVAL;
	}

	/* Succeeded: the pixels, the caller's to free. */
	source->kind = PDF_IMAGE_SOURCE_RGBA;
	source->data = pixels;
	source->bytes = (size_t)length;
	*owned = pixels;
	return 0;
}

/*
 * Gives an image the bytes read back from a PDF (libpdf's
 * pdf_page_editor_read_image): a JPEG's, a PNG's rows, or RGBA pixels,
 * which it compresses.  Its size and components are the PDF's.  Returns
 * 0, EINVAL for a source of another kind, or ENOMEM.
 */
int
notes_image_set_bytes(
	struct notes_image *image,
	const struct pdf_image_source *source)
{
	unsigned char *bytes;
	uLongf length;
	unsigned kind;
	int status;

	/* A JPEG's bytes or a PNG's rows, copied. */
	if (source->kind == PDF_IMAGE_SOURCE_JPEG || source->kind == PDF_IMAGE_SOURCE_IDAT) {
		bytes = malloc(source->bytes);
		if (bytes == NULL)
			return ENOMEM;
		memcpy(bytes, source->data, source->bytes);
		length = (uLongf)source->bytes;
		kind = NOTES_IMAGE_JPEG;
		if (source->kind == PDF_IMAGE_SOURCE_IDAT)
			kind = NOTES_IMAGE_ROWS;
	} else if (source->kind == PDF_IMAGE_SOURCE_RGBA) {
		/* RGBA, compressed. */
		length = compressBound((uLong)source->bytes);
		bytes = malloc((size_t)length);
		if (bytes == NULL)
			return ENOMEM;
		status = compress2(bytes, &length, source->data, (uLong)source->bytes, Z_DEFAULT_COMPRESSION);
		if (status != Z_OK) {
			free(bytes);
			return ENOMEM;
		}

		/* The compressed pixels. */
		kind = NOTES_IMAGE_RGBA;
	} else {
		return EINVAL;
	}

	/* Succeeded: the image's bytes, as the PDF has them. */
	free(image->data);
	image->data = bytes;
	image->size = (size_t)length;
	image->kind = kind;
	image->width = source->width;
	image->height = source->height;
	image->components = source->components;
	return 0;
}

/*
 * Copies an edit, with another reference to its image.  Returns NULL when
 * memory runs out.
 */
struct notes_edit *
notes_edit_copy(
	const struct notes_edit *edit)
{
	struct notes_edit *copy;
	size_t length;

	/* The copy. */
	copy = malloc(sizeof(*copy));
	if (copy == NULL)
		return NULL;
	*copy = *edit;

	/* Its words, its own copy (ws175-p004). */
	if (edit->text != NULL) {
		length = strlen(edit->text);
		copy->text = malloc(length + 1U);
		if (copy->text == NULL) {
			free(copy);
			return NULL;
		}

		/* The words' bytes. */
		memcpy(copy->text, edit->text, length + 1U);
	}

	/* Its image is held once more. */
	if (copy->image != NULL)
		copy->image->refs++;
	return copy;
}

/*
 * Frees an edit and its reference to its image.
 */
void
notes_edit_free(
	struct notes_edit *edit)
{
	/* No edit. */
	if (edit == NULL)
		return;

	/* The image's reference, the words, then the edit. */
	notes_image_release(edit->image);
	free(edit->text);
	free(edit);
}

/*
 * Puts an edit's transform on its grids: the first four numbers on 1/65536,
 * the offset on the edit data's 1/64 point.
 */
void
notes_edit_quantize(
	struct notes_edit *edit)
{
	size_t at;

	/* The scale and turn, then the offset. */
	for (at = 0; at < 4U; at++)
		edit->transform[at] = edit_grid(edit->transform[at], EDIT_SCALE_UNITS);
	edit->transform[4] = edit_grid(edit->transform[4], NOTES_UNITS_PER_POINT);
	edit->transform[5] = edit_grid(edit->transform[5], NOTES_UNITS_PER_POINT);
}

/*
 * Tells whether two edits name the same object: the same inserted number,
 * or the same key of the page's own.
 */
int
notes_edit_same_object(
	const struct notes_edit *edit,
	const struct notes_edit *other)
{
	int differs;

	/* Inserted: by number. */
	if ((edit->flags & NOTES_EDIT_INSERTED) != 0U || (other->flags & NOTES_EDIT_INSERTED) != 0U) {
		if ((edit->flags & other->flags & NOTES_EDIT_INSERTED) == 0U)
			return 0;
		return edit->id == other->id;
	}

	/* The page's own: by key. */
	if (edit->key.kind != other->key.kind || edit->key.offset != other->key.offset || edit->key.length != other->key.length)
		return 0;
	differs = memcmp(edit->key.fingerprint, other->key.fingerprint, sizeof(edit->key.fingerprint));
	return differs == 0;
}

/*
 * Puts an edit on a page before the edit at a place (the count appends).
 *
 * The primitive change: logged, applied, and not entered in the history.
 * The page owns the edit afterwards, and its editor is made again when
 * asked for.  Returns 0, EINVAL, ENOMEM, or the journal's failure.
 */
int
notes_document_put_edit(
	struct notes_document *document,
	size_t page,
	size_t place,
	struct notes_edit *edit)
{
	struct notes_page *target;
	int error;

	/* A page that is there, a place on it. */
	if (page >= document->page_count || edit == NULL)
		return EINVAL;
	target = document->pages[page];
	if (place > target->edit_count)
		return EINVAL;

	/* Room for one more. */
	error = edit_grow(target);
	if (error != 0)
		return error;

	/* Logs the change before it is made. */
	if (document->journal != NULL) {
		error = notes_journal_put_edit(document->journal, document, page, place, edit);
		if (error != 0)
			return error;
	}

	/* Moves the later edits up and puts the edit in. */
	memmove(&target->edits[place + 1U], &target->edits[place], (target->edit_count - place) * sizeof(target->edits[0]));
	target->edits[place] = edit;
	target->edit_count++;

	/* A replayed object or image may carry a number the document has not handed out yet. */
	if ((edit->flags & NOTES_EDIT_INSERTED) != 0U && edit->id >= document->next_id)
		document->next_id = edit->id + 1U;
	if (edit->image != NULL && edit->image->id >= document->next_id)
		document->next_id = edit->image->id + 1U;

	/* Succeeded: the page's editor is old, the document and its pages' look changed. */
	target->editor_stale = 1;
	document->dirty = 1;
	document->reshaped++;
	document->edit_serial++;
	return 0;
}

/*
 * Takes the edit of an object off a page, and tells where it was.
 *
 * The primitive change: logged, applied, and not entered in the history.
 * Returns the edit, which the caller owns, or NULL when the page has none
 * of that object or the journal could not log the change.
 */
struct notes_edit *
notes_document_take_edit(
	struct notes_document *document,
	size_t page,
	const struct notes_edit *which,
	size_t *place)
{
	struct notes_page *target;
	struct notes_edit *edit;
	size_t index;
	int same;
	int error;

	/* A page that is there. */
	if (page >= document->page_count || which == NULL)
		return NULL;
	target = document->pages[page];

	/* The object's edit. */
	for (index = 0; index < target->edit_count; index++) {
		same = notes_edit_same_object(target->edits[index], which);
		if (same)
			break;
	}

	/* None. */
	if (index == target->edit_count)
		return NULL;

	/* Logs the change before it is made. */
	if (document->journal != NULL) {
		error = notes_journal_take_edit(document->journal, document, page, target->edits[index]);
		if (error != 0)
			return NULL;
	}

	/* Moves the later edits down over it. */
	edit = target->edits[index];
	memmove(&target->edits[index], &target->edits[index + 1U], (target->edit_count - index - 1U) * sizeof(target->edits[0]));
	target->edit_count--;

	/* Succeeded: the page's editor is old, the document and its pages' look changed. */
	target->editor_stale = 1;
	document->dirty = 1;
	document->reshaped++;
	document->edit_serial++;
	*place = index;
	return edit;
}

/*
 * Gives the editor of a page with its edits applied: the page of the base
 * PDF for a page drawn over it, a blank editor for a page of Notes' own.
 * It is made when there is none or the edits changed since.  Returns 0,
 * EINVAL (a page drawn over a base the document does not have), ESTALE
 * when an edit's object is not on the page, or the editor's failure.
 */
int
notes_page_editor(
	struct notes_document *document,
	size_t page,
	struct pdf_page_editor **editor)
{
	struct notes_page *target;
	struct pdf_page_editor *made;
	size_t index;
	unsigned result;
	int error;

	/* A page that is there; its editor when it is still good. */
	if (page >= document->page_count)
		return EINVAL;
	target = document->pages[page];
	if (target->editor != NULL && !target->editor_stale) {
		*editor = target->editor;
		return 0;
	}

	/* The old editor goes; the new one is of the base's page, or blank. */
	notes_page_close_editor(target);
	if (target->origin == NOTES_ORIGIN_OVER) {
		if (document->base == NULL)
			return EINVAL;
		error = pdf_page_editor_open(document->base, target->source, &made);
	} else {
		error = pdf_page_editor_blank((double)target->width, (double)target->height, &made);
	}

	/* An editor that could not be made. */
	if (error != 0)
		return error;

	/* Each edit, in order. */
	for (index = 0; index < target->edit_count; index++) {
		error = edit_apply(made, target->edits[index], &result);
		if (error != 0) {
			pdf_page_editor_close(made);
			return error;
		}
	}

	/* Succeeded: the page's editor, good until its edits change. */
	target->editor = made;
	target->editor_stale = 0;
	*editor = made;
	return 0;
}

/*
 * Tries an object's new state on its page's editor before the change is
 * made (ws175-p008: the text box asks the editor before the words are the
 * model's): the state is applied as a change would apply it, and what the
 * editor did with the words is told (PDF_EDIT_TEXT_*: MISSING when it left
 * out characters no font has).  The try is not kept: the editor is made
 * again from the edits when next asked for.  Returns 0, ESTALE when the
 * page does not have the object, or the editor's failure (ENOTSUP: no
 * replacement font is installed).
 */
int
notes_page_try_edit(
	struct notes_document *document,
	size_t page,
	const struct notes_edit *state,
	unsigned *result)
{
	struct pdf_page_editor *editor;
	int error;

	/* The page's editor. */
	*result = 0U;
	error = notes_page_editor(document, page, &editor);
	if (error != 0)
		return error;

	/* The state applied, then the editor made again when next asked for. */
	error = edit_apply(editor, state, result);
	document->pages[page]->editor_stale = 1;
	return error;
}

/*
 * Gives the state of an object of a page's editor by its index there: its
 * edit, or (an object without one) its key as the page has it.  The
 * state's image and words are the edit's, not held or copied again.  Returns 0, ENOENT for an
 * index the editor does not have, or the failure of making the editor.
 */
int
notes_page_object(
	struct notes_document *document,
	size_t page,
	size_t index,
	struct notes_edit *state)
{
	struct pdf_page_editor *editor;
	struct notes_page *target;
	struct notes_edit named;
	size_t inserted;
	size_t own;
	size_t at;
	size_t seen;
	int same;
	int error;

	/* The page's editor, and how many of its objects are the page's own. */
	error = notes_page_editor(document, page, &editor);
	if (error != 0)
		return error;
	target = document->pages[page];
	inserted = 0;
	for (at = 0; at < target->edit_count; at++) {
		if ((target->edits[at]->flags & NOTES_EDIT_INSERTED) != 0U)
			inserted++;
	}

	/* The rest are the page's own. */
	own = pdf_page_editor_count(editor) - inserted;
	if (index >= own + inserted)
		return ENOENT;

	/* An inserted object: the edit of that rank among the inserted ones. */
	if (index >= own) {
		seen = 0;
		for (at = 0; at < target->edit_count; at++) {
			if ((target->edits[at]->flags & NOTES_EDIT_INSERTED) == 0U)
				continue;
			if (seen == index - own) {
				*state = *target->edits[at];
				return 0;
			}

			/* The next inserted one. */
			seen++;
		}
	}

	/* The page's own: its key, and its edit when it has one. */
	memset(&named, 0, sizeof(named));
	error = pdf_page_editor_key(editor, index, &named.key);
	if (error != 0)
		return error;
	named.transform[0] = 1.0f;
	named.transform[3] = 1.0f;
	for (at = 0; at < target->edit_count; at++) {
		same = notes_edit_same_object(target->edits[at], &named);
		if (same) {
			*state = *target->edits[at];
			return 0;
		}
	}

	/* An object as the page has it. */
	*state = named;
	return 0;
}

/*
 * Finds the index in a page's editor of the object an edit names
 * (ws175-p008): an inserted one by its rank among the page's inserted
 * edits, one of the page's own by its key.  Returns 0, ENOENT when the
 * page does not have it, or the failure of making the editor.
 */
int
notes_page_object_index(
	struct notes_document *document,
	size_t page,
	const struct notes_edit *which,
	size_t *index)
{
	struct pdf_page_editor *editor;
	struct notes_page *target;
	size_t inserted;
	size_t rank;
	size_t at;
	int error;

	/* The page's editor. */
	error = notes_page_editor(document, page, &editor);
	if (error != 0)
		return error;
	target = document->pages[page];

	/* One of the page's own, by its key. */
	if ((which->flags & NOTES_EDIT_INSERTED) == 0U)
		return pdf_page_editor_find(editor, &which->key, index);

	/* An inserted one: its rank among the inserted, after the page's own objects. */
	inserted = 0;
	rank = (size_t)-1;
	for (at = 0; at < target->edit_count; at++) {
		if ((target->edits[at]->flags & NOTES_EDIT_INSERTED) == 0U)
			continue;
		if (target->edits[at]->id == which->id)
			rank = inserted;
		inserted++;
	}

	/* Not on the page. */
	if (rank == (size_t)-1)
		return ENOENT;

	/* Succeeded: its index. */
	*index = pdf_page_editor_count(editor) - inserted + rank;
	return 0;
}

/*
 * Closes a page's editor (made again when asked for).
 */
void
notes_page_close_editor(
	struct notes_page *page)
{
	/* The editor, if any. */
	pdf_page_editor_close(page->editor);
	page->editor = NULL;
	page->editor_stale = 0;
}

/*
 * Tells whether any page has an edit (the edit data then needs its major
 * version 2).
 */
int
notes_document_edited(
	const struct notes_document *document)
{
	size_t index;

	/* Each page's edits. */
	for (index = 0; index < document->page_count; index++) {
		if (document->pages[index]->edit_count > 0U)
			return 1;
	}

	/* None. */
	return 0;
}

/*
 * Checks that every page's edits apply (ws175-p007, design.md [N8]): each
 * page with edits gets its editor made again from them.  Returns 0, ESTALE
 * when an edit's object or image is not there, or ENOMEM.
 */
int
notes_document_check_edits(
	struct notes_document *document)
{
	struct pdf_page_editor *editor;
	size_t index;
	int error;

	/* Each page with edits, its editor made anew. */
	for (index = 0; index < document->page_count; index++) {
		if (document->pages[index]->edit_count == 0U)
			continue;
		notes_page_close_editor(document->pages[index]);
		error = notes_page_editor(document, index, &editor);
		if (error == ENOMEM)
			return ENOMEM;
		if (error != 0)
			return ESTALE;
	}

	/* Succeeded: the edits are the file's. */
	return 0;
}

/*
 * Applies an edit to an editor: an inserted image or text put on the
 * page, or an object of the page (by its key) deleted, given an image or
 * words, placed.  What the editor did with the words is added to *result
 * (PDF_EDIT_TEXT_*).  Returns 0, ESTALE when the page does not have the
 * object, or the editor's failure.
 */
static int
edit_apply(
	struct pdf_page_editor *editor,
	const struct notes_edit *edit,
	unsigned *result)
{
	struct pdf_image_source source;
	struct pdf_edit_text words;
	double transform[6];
	void *owned;
	size_t index;
	size_t at;
	unsigned done;
	int error;

	/* The transform as libpdf takes it. */
	for (at = 0; at < 6U; at++)
		transform[at] = (double)edit->transform[at];

	/* An inserted text (ws175-p005): its words, font, size, colour and width, its box placed by the transform. */
	if ((edit->flags & (NOTES_EDIT_INSERTED | NOTES_EDIT_TEXT)) == (NOTES_EDIT_INSERTED | NOTES_EDIT_TEXT)) {
		memset(&words, 0, sizeof(words));
		words.size = sizeof(words);
		words.utf8 = edit->text;
		words.font = (enum pdf_edit_font)edit->font;
		words.font_size = (double)edit->text_size;
		words.red = (double)((edit->color >> 24) & 0xffU) / 255.0;
		words.green = (double)((edit->color >> 16) & 0xffU) / 255.0;
		words.blue = (double)((edit->color >> 8) & 0xffU) / 255.0;
		words.box_width = (double)edit->box_width;
		done = 0U;
		error = pdf_page_editor_insert_text(editor, &words, transform, &index, &done);
		*result |= done;
		return error;
	}

	/* An inserted image. */
	if ((edit->flags & NOTES_EDIT_INSERTED) != 0U) {
		if (edit->image == NULL)
			return EINVAL;
		error = notes_image_source(edit->image, &source, &owned);
		if (error != 0)
			return error;
		error = pdf_page_editor_insert_image(editor, &source, transform, &index);
		free(owned);
		return error;
	}

	/* An object of the page, by its key. */
	error = pdf_page_editor_find(editor, &edit->key, &index);
	if (error == ENOENT)
		return ESTALE;
	if (error != 0)
		return error;

	/* Deleted. */
	if ((edit->flags & NOTES_EDIT_DELETED) != 0U)
		return pdf_page_editor_delete(editor, index);

	/* An image in its place. */
	if ((edit->flags & NOTES_EDIT_IMAGE) != 0U && edit->image != NULL) {
		error = notes_image_source(edit->image, &source, &owned);
		if (error != 0)
			return error;
		error = pdf_page_editor_set_image(editor, index, &source);
		free(owned);
		if (error != 0)
			return error;
	}

	/* New words, in the font asked for (ws175-p004). */
	if ((edit->flags & NOTES_EDIT_TEXT) != 0U && edit->text != NULL) {
		memset(&words, 0, sizeof(words));
		words.size = sizeof(words);
		words.utf8 = edit->text;
		words.font = (enum pdf_edit_font)edit->font;
		done = 0U;
		error = pdf_page_editor_set_text(editor, index, &words, &done);
		*result |= done;
		if (error != 0)
			return error;
	}

	/* Placed. */
	if ((edit->flags & NOTES_EDIT_PLACED) != 0U) {
		error = pdf_page_editor_place(editor, index, transform);
		if (error != 0)
			return error;
	}

	/* Succeeded: the edit is the editor's. */
	return 0;
}

/* Makes room for one more edit on a page. */
static int
edit_grow(
	struct notes_page *page)
{
	struct notes_edit **grown;
	size_t capacity;

	/* Room already. */
	if (page->edit_count < page->edit_capacity)
		return 0;

	/* Twice as much, from a small start. */
	capacity = page->edit_capacity * 2U;
	if (capacity < EDIT_GROW_MIN)
		capacity = EDIT_GROW_MIN;
	grown = realloc(page->edits, capacity * sizeof(*grown));
	if (grown == NULL)
		return ENOMEM;

	/* Succeeded: the larger array. */
	page->edits = grown;
	page->edit_capacity = capacity;
	return 0;
}

/* Puts a number on a grid of units to the whole. */
static float
edit_grid(
	float value,
	float units)
{
	/* The nearest step. */
	return (float)(floor((double)value * (double)units + 0.5) / (double)units);
}
