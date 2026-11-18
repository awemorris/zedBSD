/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpdf's private declarations for the writer: the document being written,
 * its pages, images and attachment, and the output helpers that writer.c
 * provides to update.c, which adds a revision to a document being read.
 * None of these leave the library (exports.map).
 */

#ifndef LIBPDF_WRITER_H
#define LIBPDF_WRITER_H

#include <stddef.h>
#include <time.h>

/* The largest coordinate the writer accepts, well inside what any reader handles as a real. */
#define PDF_WRITER_COORDINATE_LIMIT 1000000.0

/* The most distinct graphics states (a fill alpha and a blend mode) one document may use, each an ExtGState object. */
#define PDF_WRITER_ALPHA_MAX 64

/* How deep pdf_writer_save_state() may nest on one page. */
#define PDF_WRITER_SAVED_MAX 16

/*
 * The grains a document may carry (ws216-p002), the largest side of one,
 * and the most colour images and tiling patterns they may be painted in.
 */
#define PDF_WRITER_GRAIN_MAX 8
#define PDF_WRITER_GRAIN_SIDE_MAX 1024
#define PDF_WRITER_TINT_MAX 256
#define PDF_WRITER_PATTERN_MAX 1024

/*
 * The largest page content the writer compresses, and the largest decoded
 * content whose hash the reader takes (reader.c): a page past it is written
 * as it is, so its hash stays readable.
 */
#define PDF_WRITER_FLATE_CONTENT_MAX ((size_t)64 * 1024 * 1024)

/* The bytes of each element of the trailer's file identifier. */
#define PDF_WRITER_ID_SIZE 16

/*
 * How a page of the writer is placed in the document it ends up in.
 *
 * A page of a new document, and a page an update adds, is NEW.  An update
 * (update.c) also lists every page of the document it adds to: KEEP leaves
 * one as it is, OVERLAY draws the page's content over the page's own, and
 * REPLACE draws it instead of the page's own.
 */
#define PDF_WRITER_PLACE_NEW 0
#define PDF_WRITER_PLACE_KEEP 1
#define PDF_WRITER_PLACE_OVERLAY 2
#define PDF_WRITER_PLACE_REPLACE 3

/* An update's page whose own content the editor changed (ws175-p003): its new content, then the drawing over it. */
#define PDF_WRITER_PLACE_EDIT 4

/*
 * A growable run of bytes.
 *
 * It is used for each page's content stream and for the whole file while it
 * is being saved; the owner frees its data.  error holds the first failure of
 * an append.  Once it is set, later appends do nothing, so a paragraph of
 * appends that writes one PDF object is checked once, at its end, and a page
 * whose content failed stays failed.
 */
struct pdf_buffer {
	unsigned char *data;
	size_t length;
	size_t capacity;
	int error;
};

/*
 * One finished or open page.
 *
 * The page owns its content stream and lives until the writer is destroyed.
 * placement is one of PDF_WRITER_PLACE_*; a page of an update that stands
 * for a page of the document it adds to names that page by its index in
 * source, and prefixed says that its content follows a stream of its own
 * that saves the graphics state before the page's own content (OVERLAY on
 * a page that has content).  An EDIT page keeps the editor's new content of
 * the page in edited, written before the page's drawing, compressed when
 * edited_flate is set (ws175-p006, design.md [H6]).  shown is the matrix
 * from the page as its drawing sees it (the top left the origin, y
 * downward) to the page's own space, which a grain's pattern on the page
 * is placed by (ws216-p002).
 */
struct pdf_writer_page {
	double width;
	double height;
	double shown[6];
	struct pdf_buffer content;
	struct pdf_buffer edited;
	int edited_flate;
	int placement;
	size_t source;
	int prefixed;
};

/*
 * The file attached to the document, which is Notes' edit data.
 *
 * The writer holds its own copy, so the caller's buffer may go away.
 */
struct pdf_writer_attachment {
	char *name;
	char *mime_type;
	unsigned char *data;
	size_t size;
};

/*
 * One image the document draws.
 *
 * The writer owns the bytes: JPEG data kept as it came (DCTDecode), or 8-bit
 * RGB samples with an 8-bit alpha mask when some pixel is not opaque.
 * object is the image's object number, assigned when the document is laid
 * out; the mask, if any, is the next object.
 *
 * The editor's images (ws175-p006) may also be compressed: flate says the
 * data is a zlib stream (FlateDecode), png_rows that it holds a PNG's rows
 * (the PNG predictor), alpha_size is the mask's length (0: width times
 * height) and alpha_flate that it is compressed.  An image of Notes has
 * its id, written as the private key /ZedBSDNotesImage, and the digest of
 * what the editor was given, by which one update shares it among pages.
 */
struct pdf_writer_image {
	unsigned char *data;
	size_t size;
	unsigned char *alpha;
	size_t width;
	size_t height;
	int is_jpeg;
	int components;
	size_t object;
	int flate;
	int png_rows;
	size_t alpha_size;
	int alpha_flate;
	unsigned long id;
	unsigned char digest[32];
};

/*
 * One replacement font the document embeds (ws175-p005, embed.c): its
 * file (the writer's copy), the glyphs its shown strings use (one byte a
 * glyph, by its number, and the first character each stands for, for the
 * ToUnicode CMap), whether any is used, and its first object number (five
 * objects: the Type0 font, its CIDFont, the descriptor, the program and
 * the ToUnicode stream), assigned when the document is laid out.
 */
#define PDF_WRITER_FONT_FILES	4U
#define PDF_WRITER_FONT_GLYPHS	65536U
#define PDF_WRITER_FONT_OBJECTS	5U
struct pdf_writer_font {
	unsigned char *data;
	size_t size;
	unsigned char *used;
	uint32_t *unicode;
	int any;
	size_t object;
};

/*
 * One graphics state the content selects (an ExtGState): the fill alpha
 * and the blend mode (enum pdf_blend_mode), which a page's state also is.
 */
struct pdf_writer_state {
	double alpha;
	int blend;
};

/*
 * The state pdf_writer_save_state() saved: the page's graphics state and
 * the blend mode asked for, which pdf_writer_restore_state() puts back.
 */
struct pdf_writer_saved {
	struct pdf_writer_state state;
	int wanted_blend;
};

/*
 * One grain of the document (ws216-p002, paint.c): an 8-bit coverage, the
 * first row the top one, kept as the alpha mask that every colour image
 * painted with it shares.  data is the mask's bytes, a zlib stream when
 * flate is set; object is its number, assigned when the document is laid
 * out.
 */
struct pdf_writer_grain {
	unsigned char *data;
	size_t size;
	int flate;
	size_t width;
	size_t height;
	size_t object;
};

/*
 * One colour a grain is painted in: an image of the grain's size in that
 * one colour (8-bit DeviceRGB, the levels in rgb), whose mask is the
 * grain.  The tiling patterns of the colour draw it.
 */
struct pdf_writer_tint {
	size_t grain;
	unsigned char rgb[3];
	unsigned char *data;
	size_t size;
	int flate;
	size_t object;
};

/*
 * One tiling pattern of the document: a tint repeated in squares of cell
 * points of a page as shown, from the page's top left.  matrix is the
 * pattern's /Matrix: the square scaled by cell, then the page's shown
 * matrix; shown and cell are kept to find the pattern again.
 */
struct pdf_writer_pattern {
	size_t tint;
	double cell;
	double shown[6];
	double matrix[6];
	size_t object;
};

/* Compresses bytes into a zlib stream (update.c's pdf_writer_pack, set by paint.c). */
typedef int (*pdf_writer_packer)(const unsigned char *data, size_t size, unsigned char **packed, size_t *packed_size);

struct pdf_writer;

/* Writes the embedded fonts' objects (embed.c, set when a font is first used). */
typedef int (*pdf_writer_font_layout)(struct pdf_writer *writer, struct pdf_buffer *file, size_t *offsets);

/*
 * Lays out an update of a document being read (update.c): the document's
 * own bytes followed by the revision the writer's pages make.
 */
typedef int (*pdf_writer_update_layout)(struct pdf_writer *writer, struct pdf_buffer *file, time_t now);

/*
 * A document being written.
 *
 * The page array only grows, and the last page is the open one while
 * page_is_open is set.  The document identifier's first element is chosen
 * once, by the caller or at the first save, and kept by every later save;
 * a zero date means the writer chooses it at the save.
 *
 * An update (pdf_writer_create_update()) names the document it adds to in
 * base, lays out its file through update_layout, and gives the names of its
 * resources name_prefix, so they do not meet the names the pages of the
 * document already use.  first_alpha_object is the object number of the
 * first opacity's ExtGState, set when a file is laid out.  last_source is
 * one past the page of the base the update listed last.
 *
 * ws216-p002: the open page's graphics state is current_alpha and
 * current_blend; wanted_blend is the blend mode the next colour selects,
 * and saved the states pdf_writer_save_state() saved, saved_count deep.
 * Each graphics state of the document is one of states.  The grains, their
 * colours and their tiling patterns are the document's; pack compresses
 * their samples and, when content_flate is set, the pages' content (both
 * set by paint.c, which brings in libz-compat; the writer works without).
 */
struct pdf_writer {
	struct pdf_writer_page **pages;
	size_t pages_count;
	size_t pages_capacity;
	int page_is_open;
	double current_alpha;
	int current_blend;
	int wanted_blend;
	struct pdf_writer_saved saved[PDF_WRITER_SAVED_MAX];
	size_t saved_count;
	struct pdf_writer_state states[PDF_WRITER_ALPHA_MAX];
	size_t alphas_count;
	struct pdf_writer_grain grains[PDF_WRITER_GRAIN_MAX];
	size_t grains_count;
	struct pdf_writer_tint *tints;
	size_t tints_count;
	size_t tints_capacity;
	struct pdf_writer_pattern *patterns;
	size_t patterns_count;
	size_t patterns_capacity;
	pdf_writer_packer pack;
	int content_flate;
	struct pdf_writer_image *images;
	size_t images_count;
	size_t images_capacity;
	struct pdf_writer_attachment attachment;
	int has_attachment;
	unsigned char document_id[PDF_WRITER_ID_SIZE];
	int has_document_id;
	time_t creation_time;
	time_t modification_time;
	struct pdf_document *base;
	pdf_writer_update_layout update_layout;
	const char *name_prefix;
	char prefix_buffer[16];
	size_t first_alpha_object;
	size_t last_source;
	struct pdf_writer_font fonts[PDF_WRITER_FONT_FILES];
	pdf_writer_font_layout font_layout;
};

/* A dictionary written without one of its keys (update.c, ws175-p004). */
struct pdf_object;
void pdf_writer_write_dictionary_except(struct pdf_buffer *file, const struct pdf_object *dictionary, const char *key);

/* A name and a real number written as an update writes them (update.c; the clean copy's, ws175-p009). */
void pdf_writer_write_name(struct pdf_buffer *file, const unsigned char *bytes, size_t length);
void pdf_writer_write_real(struct pdf_buffer *file, double value);

/* Bytes compressed into a zlib stream, when that is shorter (update.c, ws175-p006). */
int pdf_writer_pack(const unsigned char *data, size_t size, unsigned char **packed, size_t *packed_size);

/* An update's page with its content changed (update.c; the editor's, ws175-p003). */
int pdf_update_begin_edited(struct pdf_writer *writer, size_t index, struct pdf_buffer *edited);

/* An image the document carries without drawing it (the editor's, ws175-p003): its index names it. */
int pdf_writer_add_image_object(struct pdf_writer *writer, const struct pdf_writer_image *image, size_t *index);

/* The pages (writer.c). */
int pdf_writer_add_page(struct pdf_writer *writer, double width, double height, const struct pdf_buffer *prologue, struct pdf_writer_page **page);

/* The output helpers (writer.c). */
void pdf_buffer_fail(struct pdf_buffer *buffer, int error);
void pdf_buffer_append(struct pdf_buffer *buffer, const void *data, size_t length);
void pdf_buffer_printf(struct pdf_buffer *buffer, const char *format, ...) __attribute__((format(printf, 2, 3)));
void pdf_buffer_append_number(struct pdf_buffer *buffer, double number);
void pdf_buffer_append_literal_string(struct pdf_buffer *buffer, const char *text);
void pdf_buffer_append_hex_string(struct pdf_buffer *buffer, const unsigned char *bytes, size_t length);
void pdf_buffer_append_date(struct pdf_buffer *buffer, time_t when);

/* The embedded replacement fonts (embed.c, ws175-p005): a glyph used, and the fonts' resource entries (writer.c). */
int pdf_writer_use_glyph(struct pdf_writer *writer, unsigned file, unsigned glyph, uint32_t character);
size_t pdf_writer_number_fonts(struct pdf_writer *writer, size_t next);
void pdf_writer_write_font_entries(struct pdf_writer *writer, struct pdf_buffer *file);
size_t pdf_writer_font_count(const struct pdf_writer *writer);

/* The objects every layout writes (writer.c). */
void pdf_writer_write_resources(struct pdf_writer *writer, struct pdf_buffer *file);
void pdf_writer_write_opacity_entries(struct pdf_writer *writer, struct pdf_buffer *file);
size_t pdf_writer_number_paint(struct pdf_writer *writer, size_t next);
void pdf_writer_write_pattern_entries(struct pdf_writer *writer, struct pdf_buffer *file);
void pdf_writer_write_paint_objects(struct pdf_writer *writer, struct pdf_buffer *file, size_t *offsets);
void pdf_writer_write_content_stream(struct pdf_writer *writer, struct pdf_buffer *file, size_t object, const struct pdf_buffer *content);
void pdf_writer_write_image_entries(struct pdf_writer *writer, struct pdf_buffer *file);
void pdf_writer_write_image_objects(struct pdf_writer_image *image, struct pdf_buffer *file, size_t *offsets);
void pdf_writer_write_attachment_objects(struct pdf_writer *writer, struct pdf_buffer *file, size_t *offsets, size_t file_object);
void pdf_writer_write_opacities(struct pdf_writer *writer, struct pdf_buffer *file, size_t *offsets);
void pdf_writer_hash_version_id(const struct pdf_buffer *file, size_t start, unsigned char version_id[PDF_WRITER_ID_SIZE]);

#endif /* LIBPDF_WRITER_H */
