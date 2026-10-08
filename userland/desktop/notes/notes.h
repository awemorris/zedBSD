/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of Notes, the handwritten notebook (plan/ws079/design-input-notes.md
 * section 5, plan/ws079/design-pdf.md).
 *
 * document.c keeps the pages and their strokes, and the undo history;
 * edit.c the edits of the PDF's objects and the images (ws175);
 * encode.c turns a document into the edit data (ZNOT) the saved PDF carries
 * and back; journal.c keeps an append-only log of every change since the
 * last save, so that a crash loses nothing; save.c writes the PDF with
 * libpdf; geometry.c turns strokes into the triangles the renderer draws;
 * render.c draws them with Vulkan; ui.c draws the toolbar; window.c holds
 * the Wayland window and turns the seat's input into Notes' own input
 * events; menu.c gives the compositor the window's menus; main.c ties them
 * together.
 *
 * The model, the edit data, the journal and the PDF writer depend on the C
 * library and libpdf only (the host tests build them), and the window
 * system is confined to window.c, render.c and menu.c.
 */

#ifndef NOTES_H
#define NOTES_H

#include <stddef.h>
#include <stdint.h>

#include <pdf.h>

/* The page's default size, A4 portrait in points (design-input-notes.md D4). */
#define NOTES_PAGE_WIDTH	595.276f
#define NOTES_PAGE_HEIGHT	841.890f

/* The kinds of tool a stroke is drawn with (the edit data's tool type). */
#define NOTES_TOOL_PEN		0U
#define NOTES_TOOL_HIGHLIGHTER	1U

/*
 * The page backgrounds (the edit data's background kind): plain, and the
 * page of the PDF Notes writes on, drawn under the strokes.
 */
#define NOTES_BACKGROUND_PLAIN	0U
#define NOTES_BACKGROUND_PDF	3U

/*
 * Where a page comes from (the edit data's SRC chunk): a page Notes made;
 * a page of the PDF Notes writes on, drawn under the strokes and saved by
 * drawing the strokes over it; a page of that PDF whose content is Notes'
 * own strokes (shown editable), saved by drawing them anew in its place.
 */
#define NOTES_ORIGIN_NEW	0U
#define NOTES_ORIGIN_OVER	1U
#define NOTES_ORIGIN_REPLACE	2U

/*
 * What notes_open_pdf() found: a notebook Notes saved; another program's
 * PDF Notes has written on (its strokes editable again); another program's
 * PDF (its pages the background); a PDF whose pages another program
 * changed since Notes saved it (those pages are the background now).
 */
#define NOTES_OPENED_NOTES	0U
#define NOTES_OPENED_ANNOTATED	1U
#define NOTES_OPENED_FOREIGN	2U
#define NOTES_OPENED_CHANGED	3U

/*
 * ws175-p007: a notebook whose edits of the PDF's objects no longer match
 * the file (design.md [H3]): the file is opened as it is shown, all its
 * pages the background (the edits and strokes kept in it as drawn).
 */
#define NOTES_OPENED_REBASED	4U

/* The pressure of one sample runs from 0 to this value. */
#define NOTES_PRESSURE_MAX	65535U

/* The unit the edit data stores lengths in: 1/64 point. */
#define NOTES_UNITS_PER_POINT	64.0f

/* How many changes the undo history keeps. */
#define NOTES_UNDO_LIMIT	1000U

/* How long after the last change the document is saved on its own, in milliseconds. */
#define NOTES_AUTOSAVE_IDLE_MS	5000U

/* The name and media type of the edit data attached to the PDF (design-pdf.md section 2). */
#define NOTES_ATTACHMENT_NAME	"kei-notes.bin"
#define NOTES_ATTACHMENT_TYPE	"application/x-kei-notes"

/* The sources of an input event: a mouse (or a pen without the tablet protocol), a pen's tip, its eraser end. */
#define NOTES_SOURCE_POINTER	0U
#define NOTES_SOURCE_PEN	1U
#define NOTES_SOURCE_ERASER	2U

/*
 * The kinds of input event: contact starts, the point moves in contact,
 * contact ends; a pen moves over the window without touching it, a pen
 * leaves the window.
 */
#define NOTES_INPUT_DOWN	1U
#define NOTES_INPUT_MOTION	2U
#define NOTES_INPUT_UP		3U
#define NOTES_INPUT_HOVER	4U
#define NOTES_INPUT_LEAVE	5U

/*
 * One sample of a stroke, in page coordinates.
 *
 * The position is in points with the origin at the page's top left and y
 * growing down; it is kept on the 1/64 point grid the edit data uses, so
 * that saving and reading back gives the same stroke.
 */
struct notes_point {
	float x;
	float y;
	uint16_t pressure;
	int16_t tilt_x;
	int16_t tilt_y;
	uint32_t time_ms;
};

/*
 * One stroke of ink: the samples and the tool they were drawn with.
 *
 * A stroke belongs to one page, or to an undo entry while it is off the
 * page.  Its outline, the polygon both the screen and the PDF fill, is made
 * from the samples by libpdf's pdf_outline_stroke() and kept until the
 * samples change.
 */
struct notes_stroke {
	/* The stroke's number, unique in its document and never reused. */
	uint32_t id;

	/* The tool (NOTES_TOOL_*), its colour as 0xRRGGBBAA and its full width in points. */
	unsigned tool;
	uint32_t color;
	float width;

	/* Whether the samples carry the pen's tilt, and when the stroke started (UNIX milliseconds). */
	int has_tilt;
	uint64_t start_ms;

	/* The samples, in the order they were drawn. */
	struct notes_point *points;
	size_t point_count;
	size_t point_capacity;

	/* The box the outline lies in (left, top, right, bottom, points); valid with the outline. */
	float bounds[4];

	/* The outline polygon in points (NULL until made), and its corner count. */
	struct pdf_point *outline;
	size_t outline_count;
};

/*
 * The forms of an image Notes keeps (ws175-p007, plan/ws175/phase001/
 * design.md section 6.1 and [N3]), each as compressed as it came: a
 * JPEG's bytes; a PNG file whose rows a PDF takes as they are; such rows
 * as they were read back from a PDF (a zlib stream of the filtered rows);
 * RGBA pixels compressed into a zlib stream.
 */
#define NOTES_IMAGE_JPEG	1U
#define NOTES_IMAGE_PNG		2U
#define NOTES_IMAGE_ROWS	3U
#define NOTES_IMAGE_RGBA	4U

/*
 * One image put in a page's object's place or inserted on a page.
 *
 * Its number is unique in its document (from the strokes' numbers) and is
 * the image's private key in the PDF, by which it is read back.  Several
 * edits (of pages and of the undo history) share it by counting their
 * references; the last to let it go frees it.  data is NULL while the
 * bytes are not known yet (an edit data read before the PDF's images).
 */
struct notes_image {
	uint32_t id;
	unsigned refs;
	unsigned kind;
	unsigned char *data;
	size_t size;
	size_t width;
	size_t height;
	int components;
	int orientation;
};

/*
 * What an edit does: the page's object deleted, placed by a map of the
 * page's shown space, given an image in its place; or an image inserted
 * over the page's objects.  ws175-p004: a line of text given new words
 * (TEXT); 0x20 and up are kept for p005.
 */
#define NOTES_EDIT_DELETED	0x01U
#define NOTES_EDIT_PLACED	0x02U
#define NOTES_EDIT_IMAGE	0x04U
#define NOTES_EDIT_INSERTED	0x08U
#define NOTES_EDIT_TEXT		0x10U
#define NOTES_EDIT_KNOWN	0x1fU

/*
 * One edit of a page (ws175-p007, design.md section 6.1): the state of one
 * of the page's own objects, named by its key, or of an object inserted on
 * the page, named by its number.
 *
 * transform is, for a placed object, the map of the page's shown space
 * (points, the top left the origin, y downward) from where the object was
 * to where it is; for an inserted one, the map of the image's unit square
 * onto the shown space (its top left where (0, 1) goes).  Its first four
 * numbers are kept on a 1/65536 grid and its last two on the edit data's
 * 1/64 point, so that saving and reading back gives the same edit.  image
 * holds a reference.  A line's new words (TEXT, ws175-p004) are text, UTF-8
 * the edit owns, written in font (enum pdf_edit_font; 0 the line's own).
 * ws175-p005: an inserted text (INSERTED and TEXT) also has its size in
 * points, its colour (0xRRGGBBAA) and the width its lines wrap at (0: not
 * wrapped); its transform maps its box's space (points, the top left the
 * origin, y downward) onto the page's shown space.
 */
struct notes_edit {
	struct pdf_edit_key key;
	uint32_t id;
	unsigned flags;
	float transform[6];
	struct notes_image *image;
	char *text;
	unsigned font;
	float text_size;
	uint32_t color;
	float box_width;
};

/*
 * One page: its size, its background and its strokes, bottom first.
 *
 * content_hash is the SHA-256 of the page's content stream as the PDF
 * last saved or opened it (all zero: not known).  The edit data records
 * it, and opening a PDF compares it with the page in the file to learn
 * whether another program changed the page (design-pdf.md section 3).
 *
 * origin (NOTES_ORIGIN_*) says whether the page is one of the PDF Notes
 * writes on, and source which page of that PDF (the document's base) it
 * is.  A page Notes made has origin NOTES_ORIGIN_NEW and no source.
 *
 * edits (ws175-p007) are the states of the page's own objects and the
 * objects inserted on it, the inserted ones in the order they are drawn.
 * editor is the libpdf editor of the page with the edits applied, made
 * when it is asked for and made again after a change (editor_stale): a
 * cache, which the edits can always rebuild.
 */
struct notes_page {
	float width;
	float height;
	unsigned background;
	unsigned origin;
	size_t source;
	unsigned char content_hash[32];
	struct notes_stroke **strokes;
	size_t stroke_count;
	size_t stroke_capacity;
	struct notes_edit **edits;
	size_t edit_count;
	size_t edit_capacity;
	struct pdf_page_editor *editor;
	int editor_stale;
};

/* The kinds of undo entry. */
#define NOTES_UNDO_ADD_STROKE		1U
#define NOTES_UNDO_REMOVE_STROKES	2U
#define NOTES_UNDO_ADD_PAGE		3U
#define NOTES_UNDO_ERASE_PARTS		4U
#define NOTES_UNDO_EDIT_OBJECT		5U

/*
 * One change the undo history can take back.
 *
 * An added stroke is named by its page and its place on the page.  Removed
 * strokes are kept with the places they were removed from, in the order
 * they were removed.  owned says whether the entry holds the strokes (or
 * the page) now: the strokes of a removal while it stands, the stroke of an
 * addition or the page of a page's addition while they are taken back.
 *
 * An eraser drag that cuts strokes (NOTES_UNDO_ERASE_PARTS) is kept as the
 * primitive changes it made, in order: each stroke taken off a page and
 * each piece put in its place, with the place and, in inserted, which of
 * the two it was.  Taking it back undoes them in the opposite order.  While
 * it stands (owned) the entry holds the strokes it took off; while it is
 * taken back it holds the pieces it had put in.
 *
 * An edit of an object (NOTES_UNDO_EDIT_OBJECT, ws175-p007) keeps the
 * object's state before and after it (NULL: none, an object as the page
 * has it, or an inserted object not there) and, in place, where the
 * state stands among the page's edits (the drawing order of the inserted
 * ones).  The entry always holds both, copies of the page's.
 */
struct notes_undo {
	unsigned kind;
	size_t page;
	size_t place;
	struct notes_stroke **strokes;
	size_t *places;
	unsigned char *inserted;
	size_t count;
	size_t capacity;
	struct notes_page *page_held;
	int owned;
	struct notes_edit *edit_before;
	struct notes_edit *edit_after;
};

struct notes_journal;

/*
 * A notebook: its pages and the history of its changes.
 *
 * One lives for as long as Notes shows it.  Every change goes through
 * document.c, which first tells the journal (when there is one) and then
 * applies it, so the journal always ends with the change the document has
 * just made.
 */
struct notes_document {
	/* The pages, in order. */
	struct notes_page **pages;
	size_t page_count;
	size_t page_capacity;

	/* The number the next stroke gets, and when the document was created (UNIX milliseconds). */
	uint32_t next_id;
	uint64_t time_base;

	/* The PDF's permanent identifier once the document has been saved (design-pdf.md section 1). */
	unsigned char pdf_id[16];
	int has_pdf_id;

	/*
	 * The PDF this notebook writes on, when it is another program's (or a
	 * notebook another program changed): the document read from the first
	 * base_size bytes of the file, whose SHA-256 is base_hash.  Each save
	 * writes those bytes unchanged and adds one revision with the strokes
	 * (libpdf's update), so the revision of the last save is replaced
	 * rather than piled up.  base is NULL for a notebook Notes writes
	 * whole; base_size is 0 then.  A document recovered from its journal
	 * knows base_size and base_hash but gets base from the file
	 * (notes_attach_base()).
	 */
	struct pdf_document *base;
	uint64_t base_size;
	unsigned char base_hash[32];

	/*
	 * The undo history: the entries in order, how many stand (the rest
	 * were taken back and can be redone), and how many there are.
	 */
	struct notes_undo *undo;
	size_t undo_done;
	size_t undo_count;

	/*
	 * The eraser drag: 0 when none is under way, 1 while one has removed
	 * nothing yet, 2 once its removals gather in the last entry of the history.
	 */
	int erasing;

	/* A change since the last save; cleared by a save. */
	int dirty;

	/*
	 * Counts the changes other than a stroke put on top of a page: a
	 * stroke inserted below others or removed, a page inserted or removed.
	 * It only ever grows.  The screen keeps the finished strokes of a page
	 * in a picture and adds the strokes put on top to it; a new count tells
	 * it that the picture must be drawn again from the start.
	 */
	uint64_t reshaped;

	/*
	 * Counts the changes of the pages' edits (ws175-p008): the screen draws
	 * a page's background again when it grew.  It only ever grows.
	 */
	uint64_t edit_serial;

	/* The journal every change is logged to (NULL: none). */
	struct notes_journal *journal;
};

/*
 * One input event, from a pointer or a pen, in the surface's pixels.
 *
 * The pointer gives a fixed pressure; the tablet protocol (ws079-p003)
 * gives the pen's pressure and tilt through the same event.
 */
struct notes_input {
	unsigned kind;
	unsigned source;
	float x;
	float y;
	float pressure;
	float tilt_x;
	float tilt_y;
	uint32_t time_ms;
};

/* A growable byte buffer: the edit data and the journal's records are built in one. */
struct notes_buffer {
	unsigned char *data;
	size_t length;
	size_t capacity;
	int error;
};

/* The document (document.c). */
int notes_document_init(struct notes_document *document, uint64_t time_base);
void notes_document_free(struct notes_document *document);
struct notes_stroke *notes_stroke_create(uint32_t id, unsigned tool, uint32_t color, float width, uint64_t start_ms);
void notes_stroke_free(struct notes_stroke *stroke);
int notes_stroke_append(struct notes_stroke *stroke, const struct notes_point *point);
int notes_stroke_outline(struct notes_stroke *stroke);
float notes_quantize(float value);
struct notes_page *notes_page_create(float width, float height, unsigned background);
void notes_page_free(struct notes_page *page);
int notes_document_insert_page(struct notes_document *document, size_t index, struct notes_page *page);
struct notes_page *notes_document_remove_page(struct notes_document *document, size_t index);
int notes_document_insert_stroke(struct notes_document *document, size_t page, size_t place, struct notes_stroke *stroke);
struct notes_stroke *notes_document_remove_stroke(struct notes_document *document, size_t page, uint32_t id, size_t *place);
int notes_document_add_stroke(struct notes_document *document, size_t page, struct notes_stroke *stroke);
int notes_document_add_page(struct notes_document *document, size_t index);
void notes_document_erase_begin(struct notes_document *document);
int notes_document_erase_at(struct notes_document *document, size_t page, float x, float y, float radius, size_t *removed);
int notes_document_erase_parts_at(struct notes_document *document, size_t page, float x, float y, float radius, size_t *cut);
void notes_document_erase_end(struct notes_document *document);
int notes_document_undo(struct notes_document *document, size_t *page);
int notes_document_redo(struct notes_document *document, size_t *page);
size_t notes_document_stroke_total(const struct notes_document *document);

/* The images and the edits of the PDF's objects (edit.c, ws175-p007). */
struct notes_image *notes_image_create(struct notes_document *document, unsigned kind, const void *data, size_t size, size_t width, size_t height, int components, int orientation);
void notes_image_release(struct notes_image *image);
int notes_image_source(const struct notes_image *image, struct pdf_image_source *source, void **owned);

/* An image from bytes in memory, and an image as a PNG for a drag (picture-file.c, ws189-p003). */
int notes_picture_load_bytes(struct notes_document *document, const unsigned char *data, size_t size, struct notes_image **image);
int notes_picture_png(const struct notes_image *image, unsigned char **png, size_t *size);
int notes_image_set_bytes(struct notes_image *image, const struct pdf_image_source *source);
struct notes_edit *notes_edit_copy(const struct notes_edit *edit);
void notes_edit_free(struct notes_edit *edit);
void notes_edit_quantize(struct notes_edit *edit);
int notes_edit_same_object(const struct notes_edit *edit, const struct notes_edit *other);
int notes_document_put_edit(struct notes_document *document, size_t page, size_t place, struct notes_edit *edit);
struct notes_edit *notes_document_take_edit(struct notes_document *document, size_t page, const struct notes_edit *which, size_t *place);
int notes_document_edit_object(struct notes_document *document, size_t page, const struct notes_edit *state);
int notes_document_reset_object(struct notes_document *document, size_t page, const struct notes_edit *which);
int notes_page_editor(struct notes_document *document, size_t page, struct pdf_page_editor **editor);
int notes_page_object(struct notes_document *document, size_t page, size_t index, struct notes_edit *state);
int notes_page_try_edit(struct notes_document *document, size_t page, const struct notes_edit *state, unsigned *result);
int notes_page_object_index(struct notes_document *document, size_t page, const struct notes_edit *which, size_t *index);
void notes_page_close_editor(struct notes_page *page);
int notes_document_edited(const struct notes_document *document);
int notes_document_check_edits(struct notes_document *document);

/* The byte buffer (encode.c). */
void notes_buffer_init(struct notes_buffer *buffer);
void notes_buffer_free(struct notes_buffer *buffer);
void notes_buffer_bytes(struct notes_buffer *buffer, const void *data, size_t length);
void notes_buffer_u8(struct notes_buffer *buffer, unsigned value);
void notes_buffer_u16(struct notes_buffer *buffer, unsigned value);
void notes_buffer_u32(struct notes_buffer *buffer, uint32_t value);
void notes_buffer_u64(struct notes_buffer *buffer, uint64_t value);
void notes_buffer_varint(struct notes_buffer *buffer, uint64_t value);
void notes_buffer_zigzag(struct notes_buffer *buffer, int64_t value);

/* The edit data (encode.c). */
int notes_encode_document(const struct notes_document *document, struct notes_buffer *buffer);
int notes_decode_document(const void *data, size_t size, struct notes_document *document);
int notes_encode_stroke(const struct notes_stroke *stroke, struct notes_buffer *buffer);
int notes_decode_stroke(const unsigned char *data, size_t size, size_t *used, struct notes_stroke **stroke);
void notes_encode_edit(struct notes_buffer *buffer, const struct notes_edit *edit);
int notes_decode_edit(const unsigned char *data, size_t size, size_t *used, struct notes_edit *edit, uint32_t *image);
void notes_encode_image(struct notes_buffer *buffer, const struct notes_image *image);
int notes_decode_image(const unsigned char *data, size_t size, size_t *used, struct notes_image **image);

/* The journal (journal.c). */
int notes_journal_path(const char *document_path, char *path, size_t size);
int notes_journal_folder(char *path, size_t size);
struct notes_journal *notes_journal_create(const char *document_path);
void notes_journal_destroy(struct notes_journal *journal);
const char *notes_journal_document_path(const struct notes_journal *journal);
int notes_journal_add_stroke(struct notes_journal *journal, const struct notes_document *document, size_t page, size_t place, const struct notes_stroke *stroke);
int notes_journal_remove_stroke(struct notes_journal *journal, const struct notes_document *document, size_t page, uint32_t id);
int notes_journal_add_page(struct notes_journal *journal, const struct notes_document *document, size_t index);
int notes_journal_remove_page(struct notes_journal *journal, const struct notes_document *document, size_t index);
int notes_journal_discard(struct notes_journal *journal);
int notes_journal_put_edit(struct notes_journal *journal, const struct notes_document *document, size_t page, size_t place, const struct notes_edit *edit);
int notes_journal_take_edit(struct notes_journal *journal, const struct notes_document *document, size_t page, const struct notes_edit *which);
int notes_journal_set_aside(const char *journal_path);
int notes_journal_recover(const char *journal_path, struct notes_document *document, char *document_path, size_t size, size_t *records);
int notes_journal_newest(char *path, size_t size);

/* The PDF (save.c). */
int notes_save_pdf(struct notes_document *document, const char *path, size_t *bytes);
int notes_open_pdf(const char *path, struct notes_document *document, unsigned *opened);
int notes_attach_base(const char *path, struct notes_document *document);
int notes_save_clean_copy(const char *from, const char *path, size_t *bytes, size_t *objects, size_t *dropped);

#endif
