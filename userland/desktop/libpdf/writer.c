/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PDF writer of libpdf.
 *
 * Pages are drawn into content streams kept in memory.  Saving numbers
 * every object, writes them in order with a classic cross-reference table,
 * and stores the result in one write of the whole file.  The layout is the
 * one plan/ws079/design-pdf.md section 1 describes.  A writer made by
 * pdf_writer_create_update() lays its file out through update.c instead
 * (the types they share are in writer.h).
 */

#include <errno.h>
#include <sha2.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pdf.h>

#include "writer.h"

#include "include/kern/os-name.h"

/* The most images one document may hold, each an Image XObject. */
#define PDF_WRITER_IMAGE_MAX 4096

/* The largest side of an image in pixels, which keeps width x height x 4 far from overflowing. */
#define PDF_WRITER_IMAGE_SIDE_MAX 16384

/* The objects the writer places before the pages: the catalog, the page tree and the information dictionary. */
#define PDF_WRITER_OBJECT_CATALOG 1
#define PDF_WRITER_OBJECT_PAGES 2
#define PDF_WRITER_OBJECT_INFO 3
#define PDF_WRITER_OBJECT_FIRST_PAGE 4

/* The FNV-1a prime and offset basis of the 32-bit hashes that make the identifier's second element. */
#define PDF_WRITER_FNV_PRIME 16777619UL
#define PDF_WRITER_FNV_BASIS 2166136261UL

static int buffer_reserve(struct pdf_buffer *buffer, size_t extra);
static int check_coordinate(double value);
static struct pdf_buffer *open_content(struct pdf_writer *writer);
static int append_point(struct pdf_writer *writer, double x, double y, const char *operator_name);
static int select_state(struct pdf_writer *writer, struct pdf_buffer *content, double alpha);
static int find_or_add_state(struct pdf_writer *writer, double alpha, int blend, size_t *index);
static int find_or_add_tint(struct pdf_writer *writer, size_t grain, const unsigned char rgb[3], size_t *index);
static int find_or_add_pattern(struct pdf_writer *writer, size_t tint, double cell, const double shown[6], size_t *index);
static int uses_multiply(const struct pdf_writer *writer);
static unsigned char color_level(double value);
static int copy_string(const char *source, char **copy);
static int add_image(struct pdf_writer *writer, struct pdf_writer_image *image, double x, double y, double draw_width, double draw_height);
static int mime_type_is_valid(const char *mime_type);
static int write_document(struct pdf_writer *writer, struct pdf_buffer *file, time_t now);
static void write_catalog(struct pdf_writer *writer, struct pdf_buffer *file, size_t spec_object);
static void write_page_tree(struct pdf_writer *writer, struct pdf_buffer *file);
static void write_information(struct pdf_writer *writer, struct pdf_buffer *file, time_t now);
static void write_page_objects(struct pdf_writer *writer, struct pdf_buffer *file, size_t *offsets, size_t page_index);
static void write_cross_reference(struct pdf_writer *writer, struct pdf_buffer *file, const size_t *offsets, size_t object_count);

/*
 * Creates an empty document with no pages.
 */
int
pdf_writer_create(
	struct pdf_writer **writer)
{
	struct pdf_writer *created;

	/* Allocates the document with every list empty and both dates left to the save. */
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;

	/* A new page starts opaque, so the first translucent fill must select its alpha. */
	created->current_alpha = 1.0;

	/* A new document's resources have the plain names; an update gives them its own prefix. */
	created->name_prefix = "";

	/* Succeeded: the caller owns the new document. */
	*writer = created;
	return 0;
}

/*
 * Destroys a document and every page and attachment it holds.
 */
void
pdf_writer_destroy(
	struct pdf_writer *writer)
{
	size_t index;

	/* Nothing was created. */
	if (writer == NULL)
		return;

	/* Frees each page's content stream and the page itself. */
	for (index = 0; index < writer->pages_count; index++) {
		free(writer->pages[index]->content.data);
		free(writer->pages[index]->edited.data);
		free(writer->pages[index]);
	}

	/* Frees each image's samples and mask. */
	for (index = 0; index < writer->images_count; index++) {
		free(writer->images[index].data);
		free(writer->images[index].alpha);
	}

	/* Frees each grain's mask and each colour's samples (ws216-p002). */
	for (index = 0; index < writer->grains_count; index++)
		free(writer->grains[index].data);
	for (index = 0; index < writer->tints_count; index++)
		free(writer->tints[index].data);

	/* Frees each embedded font's file and glyphs (ws175-p005). */
	for (index = 0; index < PDF_WRITER_FONT_FILES; index++) {
		free(writer->fonts[index].data);
		free(writer->fonts[index].used);
		free(writer->fonts[index].unicode);
	}

	/* Frees the page, image, colour and pattern arrays, the attachment and the document. */
	free(writer->tints);
	free(writer->patterns);
	free(writer->images);
	free(writer->pages);
	free(writer->attachment.name);
	free(writer->attachment.mime_type);
	free(writer->attachment.data);
	free(writer);
}

/*
 * Opens a new page of the given size in points.
 *
 * Drawing uses the page's top-left corner as the origin with y growing
 * downward, as Notes' model does.  In an update (pdf_writer_create_update())
 * the page is added to the document after the pages listed so far.
 */
int
pdf_writer_begin_page(
	struct pdf_writer *writer,
	double width,
	double height)
{
	struct pdf_writer_page *page;
	struct pdf_buffer prologue;
	int error;

	/* Flips the y axis so the content uses the model's top-left origin. */
	memset(&prologue, 0, sizeof(prologue));
	pdf_buffer_printf(&prologue, "1 0 0 -1 0 ");
	pdf_buffer_append_number(&prologue, height);
	pdf_buffer_printf(&prologue, " cm\n");
	if (prologue.error != 0) {
		free(prologue.data);
		return ENOMEM;
	}

	/* Adds the page, which starts with the flip. */
	error = pdf_writer_add_page(writer, width, height, &prologue, &page);
	free(prologue.data);
	if (error != 0)
		return error;

	/* The flip is also the matrix from the page as drawn to its own space, which places its patterns. */
	page->shown[0] = 1.0;
	page->shown[1] = 0.0;
	page->shown[2] = 0.0;
	page->shown[3] = -1.0;
	page->shown[4] = 0.0;
	page->shown[5] = height;

	/* Succeeded: drawing now goes to the new page. */
	return 0;
}

/*
 * Finishes the open page.
 */
int
pdf_writer_end_page(
	struct pdf_writer *writer)
{
	struct pdf_buffer *content;

	/* Refuses to end a page that is not open. */
	content = open_content(writer);
	if (content == NULL)
		return EINVAL;

	/* Restores every state the page saved and left saved, so that its content is balanced. */
	while (writer->saved_count > 0) {
		pdf_buffer_printf(content, "Q\n");
		writer->saved_count--;
	}
	if (content->error != 0)
		return content->error;

	/* Later drawing is refused until another page opens. */
	writer->page_is_open = 0;

	/* Succeeded: the page is final. */
	return 0;
}

/*
 * Selects the color and opacity of the following fills.
 *
 * The components run from 0 to 1.  An opacity (or, ws216-p002, a blend
 * mode asked for) other than the current one is selected through an
 * ExtGState that every page's resources list.
 */
int
pdf_writer_set_fill_color(
	struct pdf_writer *writer,
	double red,
	double green,
	double blue,
	double alpha)
{
	struct pdf_buffer *content;
	int error;

	/* Refuses drawing outside a page. */
	content = open_content(writer);
	if (content == NULL)
		return EINVAL;

	/* Refuses a component outside 0 to 1; the negations also refuse a NaN. */
	if (!(red >= 0.0 && red <= 1.0))
		return EINVAL;
	if (!(green >= 0.0 && green <= 1.0))
		return EINVAL;
	if (!(blue >= 0.0 && blue <= 1.0))
		return EINVAL;
	if (!(alpha >= 0.0 && alpha <= 1.0))
		return EINVAL;

	/* Writes the DeviceRGB fill color. */
	pdf_buffer_append_number(content, red);
	pdf_buffer_printf(content, " ");
	pdf_buffer_append_number(content, green);
	pdf_buffer_printf(content, " ");
	pdf_buffer_append_number(content, blue);
	pdf_buffer_printf(content, " rg\n");
	if (content->error != 0)
		return content->error;

	/* Selects the opacity and the blend mode asked for, when the page's state differs. */
	error = select_state(writer, content, alpha);
	if (error != 0)
		return error;

	/* Succeeded: the next fills use the new color and opacity. */
	return 0;
}

/*
 * Chooses the blend mode of the following fills and images (ws216-p002):
 * Normal or Multiply.
 *
 * It takes effect, as an opacity does, at the next
 * pdf_writer_set_fill_color() or pdf_writer_set_fill_grain(), which select
 * an ExtGState only when the opacity or the blend mode changed.  Each page
 * starts with Normal.  A document that uses Multiply writes /BM in every
 * one of its ExtGStates, so a state that returns to Normal does.
 */
int
pdf_writer_set_blend_mode(
	struct pdf_writer *writer,
	enum pdf_blend_mode blend)
{
	/* Refuses drawing outside a page. */
	if (!writer->page_is_open)
		return EINVAL;

	/* Refuses a mode the writer does not write. */
	if (blend != PDF_BLEND_NORMAL && blend != PDF_BLEND_MULTIPLY)
		return EINVAL;

	/* The next colour selects it. */
	writer->wanted_blend = (int)blend;

	/* Succeeded: the next colour carries the mode. */
	return 0;
}

/*
 * Saves the graphics state of the open page (q), which
 * pdf_writer_restore_state() restores (ws216-p002).
 *
 * The writer saves with it what it knows of the state -- the opacity, the
 * blend mode, the mode asked for -- so a fill after the restore selects
 * its state again when it differs from the restored one.  The saves nest
 * sixteen deep; a page ended with states saved restores them first.
 */
int
pdf_writer_save_state(
	struct pdf_writer *writer)
{
	struct pdf_buffer *content;

	/* Refuses drawing outside a page. */
	content = open_content(writer);
	if (content == NULL)
		return EINVAL;

	/* Refuses saves nested past the limit. */
	if (writer->saved_count == PDF_WRITER_SAVED_MAX)
		return EINVAL;

	/* Writes the save. */
	pdf_buffer_printf(content, "q\n");
	if (content->error != 0)
		return content->error;

	/* Remembers the state the restore returns to. */
	writer->saved[writer->saved_count].state.alpha = writer->current_alpha;
	writer->saved[writer->saved_count].state.blend = writer->current_blend;
	writer->saved[writer->saved_count].wanted_blend = writer->wanted_blend;
	writer->saved_count++;

	/* Succeeded: the state is saved. */
	return 0;
}

/*
 * Restores the graphics state pdf_writer_save_state() saved last (Q).
 */
int
pdf_writer_restore_state(
	struct pdf_writer *writer)
{
	struct pdf_buffer *content;
	struct pdf_writer_saved *saved;

	/* Refuses drawing outside a page. */
	content = open_content(writer);
	if (content == NULL)
		return EINVAL;

	/* Refuses a restore without a save. */
	if (writer->saved_count == 0)
		return EINVAL;

	/* Writes the restore. */
	pdf_buffer_printf(content, "Q\n");
	if (content->error != 0)
		return content->error;

	/* The page's state is the saved one again. */
	writer->saved_count--;
	saved = &writer->saved[writer->saved_count];
	writer->current_alpha = saved->state.alpha;
	writer->current_blend = saved->state.blend;
	writer->wanted_blend = saved->wanted_blend;

	/* Succeeded: the state is restored. */
	return 0;
}

/*
 * Selects a grain as the colour of the following fills (ws216-p002): the
 * grain (pdf_writer_add_grain()) in one colour, repeated in squares of
 * cell points of the page as drawn, from its top left, and painted at an
 * opacity and the blend mode asked for, as pdf_writer_set_fill_color()
 * paints.
 *
 * The squares are a tiling pattern of the page: a page of another size or
 * another rotation has patterns of its own, so the grain lands on the same
 * points of every page as drawn.  pdf_writer_set_fill_color() returns to
 * a plain colour.
 */
int
pdf_writer_set_fill_grain(
	struct pdf_writer *writer,
	size_t grain,
	double red,
	double green,
	double blue,
	double alpha,
	double cell)
{
	struct pdf_writer_page *page;
	struct pdf_buffer *content;
	unsigned char rgb[3];
	size_t tint;
	size_t pattern;
	int error;

	/* Refuses drawing outside a page. */
	content = open_content(writer);
	if (content == NULL)
		return EINVAL;
	page = writer->pages[writer->pages_count - 1];

	/* Refuses a grain the document does not have. */
	if (grain >= writer->grains_count)
		return EINVAL;

	/* Refuses a component outside 0 to 1; the negations also refuse a NaN. */
	if (!(red >= 0.0 && red <= 1.0))
		return EINVAL;
	if (!(green >= 0.0 && green <= 1.0))
		return EINVAL;
	if (!(blue >= 0.0 && blue <= 1.0))
		return EINVAL;
	if (!(alpha >= 0.0 && alpha <= 1.0))
		return EINVAL;

	/* Refuses a square without size or past the limit. */
	if (!(cell > 0.0 && cell <= PDF_WRITER_COORDINATE_LIMIT))
		return EINVAL;

	/* The colour as the 8-bit levels its image is drawn in. */
	rgb[0] = color_level(red);
	rgb[1] = color_level(green);
	rgb[2] = color_level(blue);

	/* Finds the grain's image in this colour. */
	error = find_or_add_tint(writer, grain, rgb, &tint);
	if (error != 0)
		return error;

	/* Finds its pattern on a page placed as this one. */
	error = find_or_add_pattern(writer, tint, cell, page->shown, &pattern);
	if (error != 0)
		return error;

	/* Selects the pattern as the colour. */
	pdf_buffer_printf(content, "/Pattern cs /%sP%lu scn\n", writer->name_prefix, (unsigned long)pattern);
	if (content->error != 0)
		return content->error;

	/* Selects the opacity and the blend mode asked for, when the page's state differs. */
	error = select_state(writer, content, alpha);
	if (error != 0)
		return error;

	/* Succeeded: the next fills are painted with the grain. */
	return 0;
}

/*
 * Starts a new subpath at a point.
 */
int
pdf_writer_move_to(
	struct pdf_writer *writer,
	double x,
	double y)
{
	int error;

	/* Writes the point with the move operator. */
	error = append_point(writer, x, y, " m\n");
	if (error != 0)
		return error;

	/* Succeeded: a subpath starts at the point. */
	return 0;
}

/*
 * Adds a straight segment to the current subpath.
 */
int
pdf_writer_line_to(
	struct pdf_writer *writer,
	double x,
	double y)
{
	int error;

	/* Writes the point with the line operator. */
	error = append_point(writer, x, y, " l\n");
	if (error != 0)
		return error;

	/* Succeeded: the subpath ends at the point. */
	return 0;
}

/*
 * Adds a cubic Bezier segment to the current subpath.
 */
int
pdf_writer_curve_to(
	struct pdf_writer *writer,
	double x1,
	double y1,
	double x2,
	double y2,
	double x3,
	double y3)
{
	int error;

	/* Writes the first control point. */
	error = append_point(writer, x1, y1, " ");
	if (error != 0)
		return error;

	/* Writes the second control point. */
	error = append_point(writer, x2, y2, " ");
	if (error != 0)
		return error;

	/* Writes the end point with the curve operator. */
	error = append_point(writer, x3, y3, " c\n");
	if (error != 0)
		return error;

	/* Succeeded: the subpath ends at the curve's end point. */
	return 0;
}

/*
 * Closes the current subpath back to its start.
 */
int
pdf_writer_close_path(
	struct pdf_writer *writer)
{
	struct pdf_buffer *content;

	/* Refuses drawing outside a page. */
	content = open_content(writer);
	if (content == NULL)
		return EINVAL;

	/* Writes the close operator. */
	pdf_buffer_printf(content, "h\n");
	if (content->error != 0)
		return content->error;

	/* Succeeded: the subpath is closed. */
	return 0;
}

/*
 * Fills the current path with the current color and ends it.
 */
int
pdf_writer_fill(
	struct pdf_writer *writer,
	enum pdf_fill_rule rule)
{
	struct pdf_buffer *content;
	const char *operator_text;

	/* Refuses drawing outside a page. */
	content = open_content(writer);
	if (content == NULL)
		return EINVAL;

	/* Chooses the fill operator of the rule. */
	if (rule == PDF_FILL_NONZERO) {
		operator_text = "f\n";
	} else if (rule == PDF_FILL_EVEN_ODD) {
		operator_text = "f*\n";
	} else {
		return EINVAL;
	}

	/* Writes the fill operator. */
	pdf_buffer_printf(content, "%s", operator_text);
	if (content->error != 0)
		return content->error;

	/* Succeeded: the path is painted and gone. */
	return 0;
}

/*
 * Fills an outline polygon, such as pdf_outline_stroke() makes, with the current color.
 *
 * The polygon is one closed subpath filled with the nonzero rule, so a stroke
 * that crosses itself is painted once and a translucent stroke does not
 * darken where it overlaps itself.
 */
int
pdf_writer_fill_outline(
	struct pdf_writer *writer,
	const struct pdf_point *outline,
	size_t count)
{
	size_t index;
	int error;

	/* Refuses an outline that encloses nothing. */
	if (count < 3)
		return EINVAL;

	/* Starts the subpath at the first corner. */
	error = pdf_writer_move_to(writer, outline[0].x, outline[0].y);
	if (error != 0)
		return error;

	/* Draws an edge to each following corner. */
	for (index = 1; index < count; index++) {
		error = pdf_writer_line_to(writer, outline[index].x, outline[index].y);
		if (error != 0)
			return error;
	}

	/* Closes the polygon back to its first corner. */
	error = pdf_writer_close_path(writer);
	if (error != 0)
		return error;

	/* Paints it. */
	error = pdf_writer_fill(writer, PDF_FILL_NONZERO);
	if (error != 0)
		return error;

	/* Succeeded: the outline is painted. */
	return 0;
}

/*
 * Adds an image the document carries without drawing it (ws175-p003: the
 * editor names it in its own content as name_prefix, "Im" and the index).
 * The writer takes the image's bytes.  Returns 0, ENOSPC past the
 * document's limit, or ENOMEM.
 */
int
pdf_writer_add_image_object(
	struct pdf_writer *writer,
	const struct pdf_writer_image *image,
	size_t *index)
{
	struct pdf_writer_image *grown;
	size_t capacity;

	/* Refuses an image past the document's limit. */
	if (writer->images_count == PDF_WRITER_IMAGE_MAX)
		return ENOSPC;

	/* Grows the image array when it is full. */
	if (writer->images_count == writer->images_capacity) {
		capacity = writer->images_capacity * 2;
		if (capacity == 0)
			capacity = 8;
		grown = realloc(writer->images, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		writer->images = grown;
		writer->images_capacity = capacity;
	}

	/* Succeeded: the image is the document's, by its index. */
	writer->images[writer->images_count] = *image;
	*index = writer->images_count;
	writer->images_count++;
	return 0;
}

/*
 * Draws an RGBA image into a rectangle of the open page.
 *
 * The pixels are 8-bit red, green, blue and alpha, row by row from the top.
 * The rectangle's top-left corner is (x, y) in the page's y-down space.  An
 * image with a pixel that is not opaque carries an alpha mask.  Like every
 * image, it is painted with the opacity pdf_writer_set_fill_color() selected
 * last.
 */
int
pdf_writer_draw_rgba_image(
	struct pdf_writer *writer,
	const unsigned char *pixels,
	size_t width,
	size_t height,
	double x,
	double y,
	double draw_width,
	double draw_height)
{
	struct pdf_writer_image image;
	size_t pixel_count;
	size_t index;
	int translucent;
	int error;

	/* Refuses missing pixels and an image without area or past the side limit. */
	if (pixels == NULL)
		return EINVAL;
	if (width == 0 || height == 0)
		return EINVAL;
	if (width > PDF_WRITER_IMAGE_SIDE_MAX || height > PDF_WRITER_IMAGE_SIDE_MAX)
		return EINVAL;

	/* Allocates the RGB samples. */
	memset(&image, 0, sizeof(image));
	pixel_count = width * height;
	image.data = malloc(pixel_count * 3);
	if (image.data == NULL)
		return ENOMEM;

	/* Allocates the alpha mask. */
	image.alpha = malloc(pixel_count);
	if (image.alpha == NULL) {
		free(image.data);
		return ENOMEM;
	}

	/* Splits each pixel into its color and its alpha, noting whether any is not opaque. */
	translucent = 0;
	for (index = 0; index < pixel_count; index++) {
		image.data[index * 3 + 0] = pixels[index * 4 + 0];
		image.data[index * 3 + 1] = pixels[index * 4 + 1];
		image.data[index * 3 + 2] = pixels[index * 4 + 2];
		image.alpha[index] = pixels[index * 4 + 3];
		if (pixels[index * 4 + 3] != 255)
			translucent = 1;
	}

	/* An opaque image needs no mask. */
	if (!translucent) {
		free(image.alpha);
		image.alpha = NULL;
	}

	/* Describes the samples. */
	image.size = pixel_count * 3;
	image.width = width;
	image.height = height;
	image.components = 3;

	/* Adds the image to the document and draws it; a refusal frees the samples. */
	error = add_image(writer, &image, x, y, draw_width, draw_height);
	if (error != 0) {
		free(image.data);
		free(image.alpha);
		return error;
	}

	/* Succeeded: the image is drawn. */
	return 0;
}

/*
 * Draws a JPEG image into a rectangle of the open page.
 *
 * The JPEG bytes are stored unchanged and decoded by the reader
 * (DCTDecode).  The caller gives the image's size and its number of color
 * components, 1 (gray) or 3 (RGB), from the JPEG's frame header.
 */
int
pdf_writer_draw_jpeg_image(
	struct pdf_writer *writer,
	const void *data,
	size_t size,
	size_t width,
	size_t height,
	int components,
	double x,
	double y,
	double draw_width,
	double draw_height)
{
	struct pdf_writer_image image;
	int error;

	/* Refuses missing data, an image without area or past the side limit, and other color spaces. */
	if (data == NULL || size == 0)
		return EINVAL;
	if (width == 0 || height == 0)
		return EINVAL;
	if (width > PDF_WRITER_IMAGE_SIDE_MAX || height > PDF_WRITER_IMAGE_SIDE_MAX)
		return EINVAL;
	if (components != 1 && components != 3)
		return EINVAL;

	/* Copies the JPEG bytes. */
	memset(&image, 0, sizeof(image));
	image.data = malloc(size);
	if (image.data == NULL)
		return ENOMEM;
	memcpy(image.data, data, size);

	/* Describes the image. */
	image.size = size;
	image.width = width;
	image.height = height;
	image.is_jpeg = 1;
	image.components = components;

	/* Adds the image to the document and draws it; a refusal frees the bytes. */
	error = add_image(writer, &image, x, y, draw_width, draw_height);
	if (error != 0) {
		free(image.data);
		return error;
	}

	/* Succeeded: the image is drawn. */
	return 0;
}

/*
 * Attaches a file to the document, as Notes does with its edit data.
 *
 * The file is an embedded file stream that the catalog lists both in the
 * EmbeddedFiles name tree and in its associated files.  One document holds
 * one attachment.
 */
int
pdf_writer_attach_file(
	struct pdf_writer *writer,
	const char *name,
	const char *mime_type,
	const void *data,
	size_t size)
{
	struct pdf_writer_attachment attachment;
	int valid;
	int error;

	/* Refuses a second attachment. */
	if (writer->has_attachment)
		return EEXIST;

	/* Refuses an empty name, and data that is missing although it has a size. */
	if (name == NULL)
		return EINVAL;
	if (name[0] == '\0')
		return EINVAL;
	if (data == NULL && size != 0)
		return EINVAL;

	/* Refuses a media type that cannot be written as a name. */
	valid = mime_type_is_valid(mime_type);
	if (!valid)
		return EINVAL;

	/* Copies the name. */
	memset(&attachment, 0, sizeof(attachment));
	error = copy_string(name, &attachment.name);
	if (error != 0)
		return error;

	/* Copies the media type. */
	error = copy_string(mime_type, &attachment.mime_type);
	if (error != 0) {
		free(attachment.name);
		return error;
	}

	/* Copies the data; one extra byte keeps an empty file's allocation valid. */
	attachment.data = malloc(size + 1);
	if (attachment.data == NULL) {
		free(attachment.name);
		free(attachment.mime_type);
		return ENOMEM;
	}

	/* The bytes and their count. */
	if (size != 0)
		memcpy(attachment.data, data, size);
	attachment.size = size;

	/* Publishes the attachment in the document. */
	writer->attachment = attachment;
	writer->has_attachment = 1;

	/* Succeeded: the document carries the file. */
	return 0;
}

/*
 * Sets the permanent part of the document's file identifier.
 *
 * Notes passes the identifier of the file it opened, so a saved revision
 * keeps being recognized as the same document.  Without this call the first
 * save chooses a random identifier.
 */
int
pdf_writer_set_document_id(
	struct pdf_writer *writer,
	const unsigned char id[16])
{
	/* Keeps the identifier for every later save. */
	memcpy(writer->document_id, id, PDF_WRITER_ID_SIZE);
	writer->has_document_id = 1;

	/* Succeeded: the saves carry this identifier. */
	return 0;
}

/*
 * Reports the permanent part of the document's file identifier.
 *
 * It is known once the caller set it or the document was saved; before
 * that the call reports ENOENT.
 */
int
pdf_writer_get_document_id(
	const struct pdf_writer *writer,
	unsigned char id[16])
{
	/* Refuses a document whose identifier is not chosen yet. */
	if (!writer->has_document_id)
		return ENOENT;

	/* Copies the identifier out. */
	memcpy(id, writer->document_id, PDF_WRITER_ID_SIZE);

	/* Succeeded: id holds the document's identifier. */
	return 0;
}

/*
 * Sets the creation and modification dates of the information dictionary.
 *
 * A zero leaves the date to the save: the creation date then becomes the
 * time of the first save and the modification date the time of each save.
 * Notes passes the creation date of the file it opened.
 */
int
pdf_writer_set_dates(
	struct pdf_writer *writer,
	time_t creation,
	time_t modification)
{
	/* Refuses a date before the epoch, which the writer does not format. */
	if (creation < 0)
		return EINVAL;
	if (modification < 0)
		return EINVAL;

	/* Keeps both dates for the saves. */
	writer->creation_time = creation;
	writer->modification_time = modification;

	/* Succeeded: the saves carry these dates. */
	return 0;
}

/*
 * Computes the SHA-256 of a page's content stream.
 *
 * index counts the pages from 0.  The hash is the one the reader's
 * pdf_document_page_content_hash() gives for the same page of the saved
 * file, so Notes can record it in its edit data before saving.  A page
 * whose content failed to be recorded reports that failure.  In an update,
 * a page kept or drawn over (whose content includes the base page's own)
 * reports ENOENT: its hash is not known.
 */
int
pdf_writer_get_page_content_hash(
	const struct pdf_writer *writer,
	size_t index,
	unsigned char digest[32])
{
	const struct pdf_writer_page *page;
	SHA2_CTX context;

	/* Refuses a page the document does not have. */
	if (index >= writer->pages_count)
		return EINVAL;
	page = writer->pages[index];

	/* Refuses a page whose content is incomplete. */
	if (page->content.error != 0)
		return page->content.error;

	/* A page of an update kept or drawn over carries the base page's content too, which the writer does not hash. */
	if (page->placement == PDF_WRITER_PLACE_KEEP)
		return ENOENT;
	if (page->placement == PDF_WRITER_PLACE_OVERLAY)
		return ENOENT;
	if (page->placement == PDF_WRITER_PLACE_EDIT)
		return ENOENT;

	/* Hashes the content stream as it will be saved. */
	SHA256Init(&context);
	SHA256Update(&context, page->content.data, page->content.length);
	SHA256Final(digest, &context);

	/* Succeeded: digest is the page's content hash. */
	return 0;
}

/*
 * Saves the document to a file, replacing the file.
 *
 * The open page, if any, is saved as it stands.  The document stays usable,
 * so more pages may be added and the document saved again.  An update
 * (pdf_writer_create_update()) is saved as the bytes of the document it
 * adds to, unchanged, followed by the revision it makes.
 */
int
pdf_writer_save(
	struct pdf_writer *writer,
	const char *path)
{
	struct pdf_buffer file;
	FILE *stream;
	size_t written;
	time_t now;
	int closed;
	int error;

	/* Refuses a document without pages, which is not a valid PDF. */
	if (writer->pages_count == 0)
		return EINVAL;

	/* Takes the time of the save for the dates the caller left open. */
	now = time(NULL);

	/* Chooses the document's permanent identifier at its first save. */
	if (!writer->has_document_id) {
		arc4random_buf(writer->document_id, PDF_WRITER_ID_SIZE);
		writer->has_document_id = 1;
	}

	/* Fixes the creation date at the first save, so later saves keep it. */
	if (writer->creation_time == 0)
		writer->creation_time = now;

	/* Lays out the whole file in memory: a new document, or the update of one being read. */
	memset(&file, 0, sizeof(file));
	if (writer->update_layout != NULL) {
		error = writer->update_layout(writer, &file, now);
	} else {
		error = write_document(writer, &file, now);
	}

	/* A file that could not be laid out is not written. */
	if (error != 0) {
		free(file.data);
		return error;
	}

	/* Opens the destination. */
	stream = fopen(path, "wb");
	if (stream == NULL) {
		error = errno;
		free(file.data);
		return error;
	}

	/* Writes the file in one pass and closes it, which reports a late write failure. */
	written = fwrite(file.data, 1, file.length, stream);
	closed = fclose(stream);
	free(file.data);

	/* Reports a short write or a failed close. */
	if (written != file.length)
		return EIO;
	if (closed != 0)
		return EIO;

	/* Succeeded: the file holds the document. */
	return 0;
}

/*
 * Adds a page of a size in points, opens it for drawing and starts its
 * content with a prologue (copied).
 *
 * The page is a new one (PDF_WRITER_PLACE_NEW); an update changes its
 * placement afterwards.  Returns 0, EINVAL for an open page or a size no
 * reader accepts, or ENOMEM.
 */
int
pdf_writer_add_page(
	struct pdf_writer *writer,
	double width,
	double height,
	const struct pdf_buffer *prologue,
	struct pdf_writer_page **added)
{
	struct pdf_writer_page *page;
	struct pdf_writer_page **grown;
	size_t capacity;

	/* Refuses a second open page. */
	if (writer->page_is_open)
		return EINVAL;

	/* Refuses a page without area or larger than any reader accepts; the negations also refuse a NaN. */
	if (!(width > 0.0))
		return EINVAL;
	if (!(height > 0.0))
		return EINVAL;
	if (width > PDF_WRITER_COORDINATE_LIMIT)
		return EINVAL;
	if (height > PDF_WRITER_COORDINATE_LIMIT)
		return EINVAL;

	/* Grows the page array when it is full. */
	if (writer->pages_count == writer->pages_capacity) {
		capacity = writer->pages_capacity * 2;
		if (capacity == 0)
			capacity = 8;
		grown = realloc(writer->pages, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		writer->pages = grown;
		writer->pages_capacity = capacity;
	}

	/* Allocates the page with an empty content stream. */
	page = calloc(1, sizeof(*page));
	if (page == NULL)
		return ENOMEM;
	page->width = width;
	page->height = height;
	page->placement = PDF_WRITER_PLACE_NEW;

	/* Starts the content with the prologue. */
	pdf_buffer_append(&page->content, prologue->data, prologue->length);
	if (page->content.error != 0) {
		free(page->content.data);
		free(page);
		return ENOMEM;
	}

	/* Until its caller places it, the page as drawn is the page's own space. */
	page->shown[0] = 1.0;
	page->shown[1] = 0.0;
	page->shown[2] = 0.0;
	page->shown[3] = 1.0;
	page->shown[4] = 0.0;
	page->shown[5] = 0.0;

	/* Publishes the page as the one drawing goes to; its graphics state starts opaque, Normal and unsaved. */
	writer->pages[writer->pages_count] = page;
	writer->pages_count++;
	writer->page_is_open = 1;
	writer->current_alpha = 1.0;
	writer->current_blend = PDF_BLEND_NORMAL;
	writer->wanted_blend = PDF_BLEND_NORMAL;
	writer->saved_count = 0;

	/* Succeeded: drawing now goes to the new page. */
	*added = page;
	return 0;
}

/*
 * Records a buffer's failure, keeping the first one when several occur.
 */
void
pdf_buffer_fail(
	struct pdf_buffer *buffer,
	int error)
{
	/* An earlier failure is the one the paragraph reports. */
	if (buffer->error != 0)
		return;

	/* Later appends to the buffer now do nothing. */
	buffer->error = error;
}

/*
 * Appends bytes to a buffer, or records why they could not be appended.
 */
void
pdf_buffer_append(
	struct pdf_buffer *buffer,
	const void *data,
	size_t length)
{
	int error;

	/* A buffer that already failed stays failed and unchanged. */
	if (buffer->error != 0)
		return;

	/* Makes room for the bytes. */
	error = buffer_reserve(buffer, length);
	if (error != 0) {
		pdf_buffer_fail(buffer, error);
		return;
	}

	/* Copies the bytes to the end. */
	if (length != 0)
		memcpy(buffer->data + buffer->length, data, length);
	buffer->length += length;
}

/*
 * Appends formatted text, which is always short, to a buffer.
 */
void
pdf_buffer_printf(
	struct pdf_buffer *buffer,
	const char *format,
	...)
{
	char text[256];
	va_list arguments;
	int length;

	/* Formats the text. */
	va_start(arguments, format);
	length = vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);

	/* Records text that failed or did not fit as a failure; the writer only formats short tokens. */
	if (length < 0 || (size_t)length >= sizeof(text)) {
		pdf_buffer_fail(buffer, EINVAL);
		return;
	}

	/* Appends the text. */
	pdf_buffer_append(buffer, text, (size_t)length);
}

/*
 * Appends a real number as PDF writes one.
 *
 * PDF has no exponent notation, so the number is written with four decimals
 * and the trailing zeros removed.
 */
void
pdf_buffer_append_number(
	struct pdf_buffer *buffer,
	double number)
{
	char text[64];
	int length;

	/* Formats the number with a fixed number of decimals. */
	length = snprintf(text, sizeof(text), "%.4f", number);
	if (length < 0 || (size_t)length >= sizeof(text)) {
		pdf_buffer_fail(buffer, EINVAL);
		return;
	}

	/* Removes the trailing zeros, then a trailing point. */
	while (length > 1 && text[length - 1] == '0')
		length--;
	if (length > 1 && text[length - 1] == '.')
		length--;

	/* A negative number that rounded to zero is written as zero. */
	if (length == 2 &&
	    text[0] == '-' &&
	    text[1] == '0') {
		text[0] = '0';
		length = 1;
	}

	/* Appends the number. */
	pdf_buffer_append(buffer, text, (size_t)length);
}

/*
 * Appends text as a PDF literal string, escaping the characters that end or escape one.
 */
void
pdf_buffer_append_literal_string(
	struct pdf_buffer *buffer,
	const char *text)
{
	const char *character;
	char escaped[2];

	/* Opens the string. */
	pdf_buffer_append(buffer, "(", 1);

	/* Copies each character, escaping parentheses and backslashes. */
	for (character = text; *character != '\0'; character++) {
		if (*character == '(' ||
		    *character == ')' ||
		    *character == '\\') {
			escaped[0] = '\\';
			escaped[1] = *character;
			pdf_buffer_append(buffer, escaped, 2);
		} else {
			pdf_buffer_append(buffer, character, 1);
		}
	}

	/* Closes the string. */
	pdf_buffer_append(buffer, ")", 1);
}

/*
 * Appends bytes as a PDF hexadecimal string.
 */
void
pdf_buffer_append_hex_string(
	struct pdf_buffer *buffer,
	const unsigned char *bytes,
	size_t length)
{
	static const char digits[] = "0123456789ABCDEF";
	char pair[2];
	size_t index;

	/* Opens the string. */
	pdf_buffer_append(buffer, "<", 1);

	/* Writes each byte as two digits, the high one first. */
	for (index = 0; index < length; index++) {
		pair[0] = digits[bytes[index] >> 4];
		pair[1] = digits[bytes[index] & 0x0f];
		pdf_buffer_append(buffer, pair, 2);
	}

	/* Closes the string. */
	pdf_buffer_append(buffer, ">", 1);
}

/*
 * Appends a time as a PDF date string in universal time, (D:YYYYMMDDHHmmSSZ).
 */
void
pdf_buffer_append_date(
	struct pdf_buffer *buffer,
	time_t when)
{
	struct tm broken_down;
	struct tm *converted;

	/* Breaks the time down in universal time. */
	converted = gmtime_r(&when, &broken_down);
	if (converted == NULL) {
		pdf_buffer_fail(buffer, EINVAL);
		return;
	}

	/* Writes the date with its fields in the order PDF defines. */
	pdf_buffer_printf(buffer,
		      "(D:%04d%02d%02d%02d%02d%02dZ)",
		      broken_down.tm_year + 1900,
		      broken_down.tm_mon + 1,
		      broken_down.tm_mday,
		      broken_down.tm_hour,
		      broken_down.tm_min,
		      broken_down.tm_sec);
}

/*
 * Writes the resource dictionary every page shares, which lists the opacities, the images, the patterns and the fonts.
 *
 * The opacities' objects start at first_alpha_object and each image's
 * object is numbered, so the layout has numbered the objects already.
 */
void
pdf_writer_write_resources(
	struct pdf_writer *writer,
	struct pdf_buffer *file)
{
	size_t fonts;

	/* Opens the dictionary; a document without translucent fills or images leaves it empty. */
	pdf_buffer_printf(file, "<<");

	/* Lists each ExtGState under the name the content streams use. */
	if (writer->alphas_count != 0) {
		pdf_buffer_printf(file, " /ExtGState <<");
		pdf_writer_write_opacity_entries(writer, file);
		pdf_buffer_printf(file, " >>");
	}

	/* Lists each image under the name the content streams use. */
	if (writer->images_count != 0) {
		pdf_buffer_printf(file, " /XObject <<");
		pdf_writer_write_image_entries(writer, file);
		pdf_buffer_printf(file, " >>");
	}

	/* Lists each grain's tiling pattern (ws216-p002). */
	if (writer->patterns_count != 0) {
		pdf_buffer_printf(file, " /Pattern <<");
		pdf_writer_write_pattern_entries(writer, file);
		pdf_buffer_printf(file, " >>");
	}

	/* Lists each embedded font (ws175-p005). */
	fonts = pdf_writer_font_count(writer);
	if (fonts != 0) {
		pdf_buffer_printf(file, " /Font <<");
		pdf_writer_write_font_entries(writer, file);
		pdf_buffer_printf(file, " >>");
	}

	/* Closes the dictionary. */
	pdf_buffer_printf(file, " >>");
}

/*
 * Writes the entries of an ExtGState dictionary that name the opacities'
 * objects by the names the content streams select them with.
 */
void
pdf_writer_write_opacity_entries(
	struct pdf_writer *writer,
	struct pdf_buffer *file)
{
	size_t index;

	/* Names each opacity's object: the name prefix, GS and the opacity's index. */
	for (index = 0; index < writer->alphas_count; index++) {
		pdf_buffer_printf(file,
				  " /%sGS%lu %lu 0 R",
				  writer->name_prefix,
				  (unsigned long)index,
				  (unsigned long)(writer->first_alpha_object + index));
	}
}

/*
 * Writes the entries of an XObject dictionary that name the images'
 * objects by the names the content streams draw them with.
 */
void
pdf_writer_write_image_entries(
	struct pdf_writer *writer,
	struct pdf_buffer *file)
{
	size_t index;

	/* Names each image's object: the name prefix, Im and the image's index. */
	for (index = 0; index < writer->images_count; index++) {
		pdf_buffer_printf(file,
				  " /%sIm%lu %lu 0 R",
				  writer->name_prefix,
				  (unsigned long)index,
				  (unsigned long)writer->images[index].object);
	}
}

/*
 * Counts the replacement fonts a document embeds (ws175-p005).
 */
size_t
pdf_writer_font_count(
	const struct pdf_writer *writer)
{
	size_t count;
	unsigned file;

	/* Each font used. */
	count = 0;
	for (file = 0; file < PDF_WRITER_FONT_FILES; file++) {
		if (writer->fonts[file].any)
			count++;
	}

	/* The count. */
	return count;
}

/*
 * Numbers the embedded fonts' objects from next (five a font).  Returns
 * the number after them.
 */
size_t
pdf_writer_number_fonts(
	struct pdf_writer *writer,
	size_t next)
{
	unsigned file;

	/* Each font used. */
	for (file = 0; file < PDF_WRITER_FONT_FILES; file++) {
		if (!writer->fonts[file].any)
			continue;
		writer->fonts[file].object = next;
		next += PDF_WRITER_FONT_OBJECTS;
	}

	/* The number after them. */
	return next;
}

/*
 * Writes the entries of a Font dictionary that name the embedded fonts by
 * the names the content streams select them with: the name prefix, F and
 * the font's file.
 */
void
pdf_writer_write_font_entries(
	struct pdf_writer *writer,
	struct pdf_buffer *file)
{
	unsigned font;

	/* Each font used, by its Type0 font object. */
	for (font = 0; font < PDF_WRITER_FONT_FILES; font++) {
		if (!writer->fonts[font].any)
			continue;
		pdf_buffer_printf(file, " /%sF%u %lu 0 R", writer->name_prefix, font, (unsigned long)writer->fonts[font].object);
	}
}

/*
 * Numbers the objects of the grains, their colours' images and their
 * tiling patterns from next (ws216-p002).  Returns the number after them.
 */
size_t
pdf_writer_number_paint(
	struct pdf_writer *writer,
	size_t next)
{
	size_t index;

	/* The grains' masks. */
	for (index = 0; index < writer->grains_count; index++) {
		writer->grains[index].object = next;
		next++;
	}

	/* The colours' images. */
	for (index = 0; index < writer->tints_count; index++) {
		writer->tints[index].object = next;
		next++;
	}

	/* The patterns. */
	for (index = 0; index < writer->patterns_count; index++) {
		writer->patterns[index].object = next;
		next++;
	}

	/* The number after them. */
	return next;
}

/*
 * Writes the entries of a Pattern dictionary that name the patterns'
 * objects by the names the content streams select them with: the name
 * prefix, P and the pattern's index.
 */
void
pdf_writer_write_pattern_entries(
	struct pdf_writer *writer,
	struct pdf_buffer *file)
{
	size_t index;

	/* Names each pattern's object. */
	for (index = 0; index < writer->patterns_count; index++) {
		pdf_buffer_printf(file,
				  " /%sP%lu %lu 0 R",
				  writer->name_prefix,
				  (unsigned long)index,
				  (unsigned long)writer->patterns[index].object);
	}
}

/*
 * Writes the objects of the grains (their masks), their colours' images
 * and their tiling patterns, and records where each starts in offsets.
 *
 * A pattern's cell is the unit square with the colour's image drawn on it,
 * its first row at the top of the square (the page as drawn has y
 * downward), and the pattern carries its own resources, so that whatever
 * copies the pattern (the clean copy) finds the image by the pattern's
 * own content.
 */
void
pdf_writer_write_paint_objects(
	struct pdf_writer *writer,
	struct pdf_buffer *file,
	size_t *offsets)
{
	static const char cell[] = "q 1 0 0 -1 0 1 cm /G Do Q";
	struct pdf_writer_grain *grain;
	struct pdf_writer_tint *tint;
	struct pdf_writer_pattern *pattern;
	size_t index;
	size_t item;

	/* Writes each grain as an 8-bit gray image of its coverage, the mask of its colours. */
	for (index = 0; index < writer->grains_count; index++) {
		grain = &writer->grains[index];
		offsets[grain->object] = file->length;
		pdf_buffer_printf(file,
			      "%lu 0 obj\n<< /Type /XObject /Subtype /Image /Width %lu /Height %lu /ColorSpace /DeviceGray /BitsPerComponent 8",
			      (unsigned long)grain->object,
			      (unsigned long)grain->width,
			      (unsigned long)grain->height);
		if (grain->flate)
			pdf_buffer_printf(file, " /Filter /FlateDecode");
		pdf_buffer_printf(file, " /Length %lu >>\nstream\n", (unsigned long)grain->size);
		pdf_buffer_append(file, grain->data, grain->size);
		pdf_buffer_printf(file, "\nendstream\nendobj\n");
	}

	/* Writes each colour's image, whose mask is its grain. */
	for (index = 0; index < writer->tints_count; index++) {
		tint = &writer->tints[index];
		grain = &writer->grains[tint->grain];
		offsets[tint->object] = file->length;
		pdf_buffer_printf(file,
			      "%lu 0 obj\n<< /Type /XObject /Subtype /Image /Width %lu /Height %lu /ColorSpace /DeviceRGB /BitsPerComponent 8 /SMask %lu 0 R",
			      (unsigned long)tint->object,
			      (unsigned long)grain->width,
			      (unsigned long)grain->height,
			      (unsigned long)grain->object);
		if (tint->flate)
			pdf_buffer_printf(file, " /Filter /FlateDecode");
		pdf_buffer_printf(file, " /Length %lu >>\nstream\n", (unsigned long)tint->size);
		pdf_buffer_append(file, tint->data, tint->size);
		pdf_buffer_printf(file, "\nendstream\nendobj\n");
	}

	/* Writes each pattern: a colored tiling of the unit square, its matrix, and its own resources. */
	for (index = 0; index < writer->patterns_count; index++) {
		pattern = &writer->patterns[index];
		offsets[pattern->object] = file->length;
		pdf_buffer_printf(file,
			      "%lu 0 obj\n<< /Type /Pattern /PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 1 1] /XStep 1 /YStep 1 /Matrix [",
			      (unsigned long)pattern->object);
		for (item = 0; item < 6; item++) {
			if (item != 0)
				pdf_buffer_append(file, " ", 1);
			pdf_buffer_append_number(file, pattern->matrix[item]);
		}
		pdf_buffer_printf(file,
			      "] /Resources << /XObject << /G %lu 0 R >> >> /Length %lu >>\nstream\n",
			      (unsigned long)writer->tints[pattern->tint].object,
			      (unsigned long)(sizeof(cell) - 1));
		pdf_buffer_append(file, cell, sizeof(cell) - 1);
		pdf_buffer_printf(file, "\nendstream\nendobj\n");
	}
}

/*
 * Writes a page's content stream as one object: compressed when the writer
 * compresses content (ws216-p002) and the content is within the limit the
 * reader hashes, otherwise as it is.
 */
void
pdf_writer_write_content_stream(
	struct pdf_writer *writer,
	struct pdf_buffer *file,
	size_t object,
	const struct pdf_buffer *content)
{
	unsigned char *packed;
	size_t packed_size;
	int error;

	/* Compresses the content when asked to and the reader can hash it decoded. */
	packed = NULL;
	packed_size = 0;
	if (writer->content_flate &&
	    writer->pack != NULL &&
	    content->length <= PDF_WRITER_FLATE_CONTENT_MAX) {
		error = writer->pack(content->data, content->length, &packed, &packed_size);
		if (error != 0) {
			pdf_buffer_fail(file, error);
			return;
		}
	}

	/* Writes the compressed stream, or the content as it is. */
	if (packed != NULL) {
		pdf_buffer_printf(file, "%lu 0 obj\n<< /Filter /FlateDecode /Length %lu >>\nstream\n", (unsigned long)object, (unsigned long)packed_size);
		pdf_buffer_append(file, packed, packed_size);
		free(packed);
	} else {
		pdf_buffer_printf(file, "%lu 0 obj\n<< /Length %lu >>\nstream\n", (unsigned long)object, (unsigned long)content->length);
		pdf_buffer_append(file, content->data, content->length);
	}
	pdf_buffer_printf(file, "\nendstream\nendobj\n");
}

/*
 * Writes each graphics state's ExtGState object, numbered from
 * first_alpha_object, and records where each starts in offsets.
 *
 * A document that never multiplies writes only the fill alpha, as it always
 * did; one that does names the blend mode of every state, so that a state
 * of Normal ends a Multiply.
 */
void
pdf_writer_write_opacities(
	struct pdf_writer *writer,
	struct pdf_buffer *file,
	size_t *offsets)
{
	size_t object;
	size_t index;
	int named;

	/* Whether the states name their blend modes. */
	named = uses_multiply(writer);

	/* Writes each state as the fill alpha, and the blend mode, of its own graphics state. */
	for (index = 0; index < writer->alphas_count; index++) {
		object = writer->first_alpha_object + index;
		offsets[object] = file->length;
		pdf_buffer_printf(file, "%lu 0 obj\n<< /Type /ExtGState /ca ", (unsigned long)object);
		pdf_buffer_append_number(file, writer->states[index].alpha);
		if (writer->states[index].blend == PDF_BLEND_MULTIPLY) {
			pdf_buffer_printf(file, " /BM /Multiply");
		} else if (named) {
			pdf_buffer_printf(file, " /BM /Normal");
		}
		pdf_buffer_printf(file, " >>\nendobj\n");
	}
}

/*
 * Writes one Image XObject and, when it has one, its alpha mask.
 */
void
pdf_writer_write_image_objects(
	struct pdf_writer_image *image,
	struct pdf_buffer *file,
	size_t *offsets)
{
	const char *color_space;
	size_t mask_size;

	/* Names the color space of the samples. */
	color_space = "/DeviceRGB";
	if (image->components == 1)
		color_space = "/DeviceGray";

	/* Opens the image's dictionary with its size and color space. */
	offsets[image->object] = file->length;
	pdf_buffer_printf(file,
		      "%lu 0 obj\n<< /Type /XObject /Subtype /Image /Width %lu /Height %lu /ColorSpace %s /BitsPerComponent 8",
		      (unsigned long)image->object,
		      (unsigned long)image->width,
		      (unsigned long)image->height,
		      color_space);

	/* Names the JPEG filter, or the Flate filter and a PNG's predictor (ws175-p006). */
	if (image->is_jpeg)
		pdf_buffer_printf(file, " /Filter /DCTDecode");
	if (image->flate)
		pdf_buffer_printf(file, " /Filter /FlateDecode");
	if (image->png_rows) {
		pdf_buffer_printf(file,
			      " /DecodeParms << /Predictor 15 /Colors %d /BitsPerComponent 8 /Columns %lu >>",
			      image->components,
			      (unsigned long)image->width);
	}

	/* The mask that follows the image, and Notes' id of the image. */
	if (image->alpha != NULL)
		pdf_buffer_printf(file, " /SMask %lu 0 R", (unsigned long)(image->object + 1));
	if (image->id != 0UL)
		pdf_buffer_printf(file, " /ZedBSDNotesImage %lu", image->id);

	/* Writes the samples. */
	pdf_buffer_printf(file, " /Length %lu >>\nstream\n", (unsigned long)image->size);
	pdf_buffer_append(file, image->data, image->size);
	pdf_buffer_printf(file, "\nendstream\nendobj\n");

	/* An opaque image has no mask. */
	if (image->alpha == NULL)
		return;

	/* The mask's length: its bytes, or (compressed) the length given. */
	mask_size = image->width * image->height;
	if (image->alpha_size != 0)
		mask_size = image->alpha_size;

	/* Writes the mask as an 8-bit gray image of the alpha values. */
	offsets[image->object + 1] = file->length;
	pdf_buffer_printf(file,
		      "%lu 0 obj\n<< /Type /XObject /Subtype /Image /Width %lu /Height %lu /ColorSpace /DeviceGray /BitsPerComponent 8",
		      (unsigned long)(image->object + 1),
		      (unsigned long)image->width,
		      (unsigned long)image->height);
	if (image->alpha_flate)
		pdf_buffer_printf(file, " /Filter /FlateDecode");
	pdf_buffer_printf(file, " /Length %lu >>\nstream\n", (unsigned long)mask_size);
	pdf_buffer_append(file, image->alpha, mask_size);
	pdf_buffer_printf(file, "\nendstream\nendobj\n");
}

/*
 * Writes the embedded file stream and the file specification that names it.
 */
void
pdf_writer_write_attachment_objects(
	struct pdf_writer *writer,
	struct pdf_buffer *file,
	size_t *offsets,
	size_t file_object)
{
	const char *character;

	/* Opens the embedded file stream's dictionary. */
	offsets[file_object] = file->length;
	pdf_buffer_printf(file, "%lu 0 obj\n<< /Type /EmbeddedFile /Subtype /", (unsigned long)file_object);

	/* Writes the media type as a name, whose slash must be escaped. */
	for (character = writer->attachment.mime_type; *character != '\0'; character++) {
		if (*character == '/') {
			pdf_buffer_append(file, "#2F", 3);
		} else {
			pdf_buffer_append(file, character, 1);
		}
	}

	/* Writes the size and the file's bytes. */
	pdf_buffer_printf(file,
		      " /Params << /Size %lu >> /Length %lu >>\nstream\n",
		      (unsigned long)writer->attachment.size,
		      (unsigned long)writer->attachment.size);
	pdf_buffer_append(file, writer->attachment.data, writer->attachment.size);
	pdf_buffer_printf(file, "\nendstream\nendobj\n");

	/* Writes the file specification, which marks the file as the source of the drawing. */
	offsets[file_object + 1] = file->length;
	pdf_buffer_printf(file, "%lu 0 obj\n<< /Type /Filespec /F ", (unsigned long)(file_object + 1));
	pdf_buffer_append_literal_string(file, writer->attachment.name);
	pdf_buffer_printf(file, " /UF ");
	pdf_buffer_append_literal_string(file, writer->attachment.name);
	pdf_buffer_printf(file,
		      " /Desc (" ZEDBSD_OS_NAME " Notes edit data) /AFRelationship /Source /EF << /F %lu 0 R >> >>\nendobj\n",
		      (unsigned long)file_object);
}

/*
 * Hashes a laid-out file, from a byte on, into the identifier of its revision.
 *
 * Four 32-bit FNV-1a hashes with different starting values fill the sixteen
 * bytes.  The hash is not cryptographic; it only tells revisions apart.  A
 * new document hashes all its bytes, an update the bytes of its revision.
 */
void
pdf_writer_hash_version_id(
	const struct pdf_buffer *file,
	size_t start,
	unsigned char version_id[PDF_WRITER_ID_SIZE])
{
	unsigned long hash;
	size_t lane;
	size_t index;

	/* Hashes the bytes once per four-byte lane of the identifier. */
	for (lane = 0; lane < PDF_WRITER_ID_SIZE / 4; lane++) {
		hash = (PDF_WRITER_FNV_BASIS + lane) & 0xffffffffUL;
		for (index = start; index < file->length; index++) {
			hash ^= file->data[index];
			hash = (hash * PDF_WRITER_FNV_PRIME) & 0xffffffffUL;
		}

		/* Stores the lane's hash with its high byte first. */
		version_id[lane * 4 + 0] = (unsigned char)(hash >> 24);
		version_id[lane * 4 + 1] = (unsigned char)(hash >> 16);
		version_id[lane * 4 + 2] = (unsigned char)(hash >> 8);
		version_id[lane * 4 + 3] = (unsigned char)hash;
	}
}

/* Makes room for extra more bytes in a buffer. */
static int
buffer_reserve(
	struct pdf_buffer *buffer,
	size_t extra)
{
	unsigned char *grown;
	size_t capacity;

	/* The buffer already has room. */
	if (buffer->capacity - buffer->length >= extra)
		return 0;

	/* Refuses a size that would overflow. */
	if (extra > (size_t)-1 / 2 - buffer->length)
		return ENOMEM;

	/* Doubles the capacity until the bytes fit. */
	capacity = buffer->capacity;
	if (capacity < 256)
		capacity = 256;
	while (capacity - buffer->length < extra)
		capacity *= 2;

	/* Moves the bytes into the larger allocation. */
	grown = realloc(buffer->data, capacity);
	if (grown == NULL)
		return ENOMEM;
	buffer->data = grown;
	buffer->capacity = capacity;

	/* Succeeded: extra bytes fit. */
	return 0;
}

/* Reports whether a coordinate is finite and inside the writer's limit. */
static int
check_coordinate(
	double value)
{
	/* Refuses a NaN and anything past the limit on either side. */
	if (!(value >= -PDF_WRITER_COORDINATE_LIMIT && value <= PDF_WRITER_COORDINATE_LIMIT))
		return EINVAL;

	/* Succeeded: the coordinate can be written. */
	return 0;
}

/* Finds the content stream drawing goes to, or NULL when no page is open. */
static struct pdf_buffer *
open_content(
	struct pdf_writer *writer)
{
	/* Drawing outside a page has nowhere to go. */
	if (!writer->page_is_open)
		return NULL;

	/* Succeeded: the open page is the last one. */
	return &writer->pages[writer->pages_count - 1]->content;
}

/* Appends one point followed by an operator or a separator to the open page. */
static int
append_point(
	struct pdf_writer *writer,
	double x,
	double y,
	const char *operator_name)
{
	struct pdf_buffer *content;
	int error;

	/* Refuses drawing outside a page. */
	content = open_content(writer);
	if (content == NULL)
		return EINVAL;

	/* Refuses an x that cannot be written. */
	error = check_coordinate(x);
	if (error != 0)
		return error;

	/* Refuses a y that cannot be written. */
	error = check_coordinate(y);
	if (error != 0)
		return error;

	/* Writes the two coordinates and the operator. */
	pdf_buffer_append_number(content, x);
	pdf_buffer_append(content, " ", 1);
	pdf_buffer_append_number(content, y);
	pdf_buffer_printf(content, "%s", operator_name);
	if (content->error != 0)
		return content->error;

	/* Succeeded: the point is written. */
	return 0;
}

/*
 * Selects, on the open page, the ExtGState of an opacity and the blend mode
 * asked for, unless the page's state is that already.
 */
static int
select_state(
	struct pdf_writer *writer,
	struct pdf_buffer *content,
	double alpha)
{
	size_t state_index;
	int error;

	/* An unchanged opacity and blend mode need no graphics state. */
	if (alpha == writer->current_alpha && writer->wanted_blend == writer->current_blend)
		return 0;

	/* Finds the ExtGState that holds this opacity and mode. */
	error = find_or_add_state(writer, alpha, writer->wanted_blend, &state_index);
	if (error != 0)
		return error;

	/* Selects that ExtGState. */
	pdf_buffer_printf(content, "/%sGS%lu gs\n", writer->name_prefix, (unsigned long)state_index);
	if (content->error != 0)
		return content->error;

	/* The page's fills now carry this opacity and mode. */
	writer->current_alpha = alpha;
	writer->current_blend = writer->wanted_blend;

	/* Succeeded: the state is in force. */
	return 0;
}

/* Finds the ExtGState index of an opacity and a blend mode, adding one when the document has none yet. */
static int
find_or_add_state(
	struct pdf_writer *writer,
	double alpha,
	int blend,
	size_t *index)
{
	size_t candidate;

	/* Reuses the ExtGState that already holds this opacity and mode. */
	for (candidate = 0; candidate < writer->alphas_count; candidate++) {
		if (writer->states[candidate].alpha == alpha && writer->states[candidate].blend == blend) {
			*index = candidate;
			return 0;
		}
	}

	/* Refuses a state past the document's limit. */
	if (writer->alphas_count == PDF_WRITER_ALPHA_MAX)
		return ENOSPC;

	/* Adds the state as a new ExtGState. */
	writer->states[writer->alphas_count].alpha = alpha;
	writer->states[writer->alphas_count].blend = blend;
	*index = writer->alphas_count;
	writer->alphas_count++;

	/* Succeeded: index names the ExtGState. */
	return 0;
}

/*
 * Finds the image of a grain in a colour, making it when the document has
 * none yet: the colour's samples over the grain's size, compressed when the
 * writer can compress.
 */
static int
find_or_add_tint(
	struct pdf_writer *writer,
	size_t grain,
	const unsigned char rgb[3],
	size_t *index)
{
	struct pdf_writer_tint *grown;
	struct pdf_writer_tint tint;
	unsigned char *samples;
	unsigned char *packed;
	size_t packed_size;
	size_t pixels;
	size_t candidate;
	size_t capacity;
	size_t pixel;
	int differs;
	int error;

	/* Reuses the colour's image of the grain. */
	for (candidate = 0; candidate < writer->tints_count; candidate++) {
		if (writer->tints[candidate].grain != grain)
			continue;
		differs = memcmp(writer->tints[candidate].rgb, rgb, 3);
		if (differs != 0)
			continue;

		/* The image is the one asked for. */
		*index = candidate;
		return 0;
	}

	/* Refuses a colour past the document's limit. */
	if (writer->tints_count == PDF_WRITER_TINT_MAX)
		return ENOSPC;

	/* Grows the colour array when it is full. */
	if (writer->tints_count == writer->tints_capacity) {
		capacity = writer->tints_capacity * 2;
		if (capacity == 0)
			capacity = 8;
		grown = realloc(writer->tints, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		writer->tints = grown;
		writer->tints_capacity = capacity;
	}

	/* Allocates the colour's samples over the grain's size. */
	pixels = writer->grains[grain].width * writer->grains[grain].height;
	samples = malloc(pixels * 3);
	if (samples == NULL)
		return ENOMEM;

	/* Fills every sample with the colour. */
	for (pixel = 0; pixel < pixels; pixel++) {
		samples[pixel * 3 + 0] = rgb[0];
		samples[pixel * 3 + 1] = rgb[1];
		samples[pixel * 3 + 2] = rgb[2];
	}

	/* Describes the image: the samples as they are, until they are compressed. */
	memset(&tint, 0, sizeof(tint));
	tint.grain = grain;
	memcpy(tint.rgb, rgb, 3);
	tint.data = samples;
	tint.size = pixels * 3;

	/* Compresses the samples (one colour, so they shrink to almost nothing) when the writer can. */
	if (writer->pack != NULL) {
		error = writer->pack(samples, pixels * 3, &packed, &packed_size);
		if (error != 0) {
			free(samples);
			return error;
		}
		if (packed != NULL) {
			free(samples);
			tint.data = packed;
			tint.size = packed_size;
			tint.flate = 1;
		}
	}

	/* Succeeded: the colour's image is the document's. */
	writer->tints[writer->tints_count] = tint;
	*index = writer->tints_count;
	writer->tints_count++;
	return 0;
}

/*
 * Finds the tiling pattern of a colour's image in squares of a size on a
 * page placed by a shown matrix, making it when the document has none yet.
 */
static int
find_or_add_pattern(
	struct pdf_writer *writer,
	size_t tint,
	double cell,
	const double shown[6],
	size_t *index)
{
	struct pdf_writer_pattern *grown;
	struct pdf_writer_pattern *pattern;
	size_t candidate;
	size_t capacity;
	int differs;

	/* Reuses the pattern of the same image, squares and placement. */
	for (candidate = 0; candidate < writer->patterns_count; candidate++) {
		pattern = &writer->patterns[candidate];
		if (pattern->tint != tint || pattern->cell != cell)
			continue;
		differs = memcmp(pattern->shown, shown, sizeof(pattern->shown));
		if (differs != 0)
			continue;

		/* The pattern is the one asked for. */
		*index = candidate;
		return 0;
	}

	/* Refuses a pattern past the document's limit. */
	if (writer->patterns_count == PDF_WRITER_PATTERN_MAX)
		return ENOSPC;

	/* Grows the pattern array when it is full. */
	if (writer->patterns_count == writer->patterns_capacity) {
		capacity = writer->patterns_capacity * 2;
		if (capacity == 0)
			capacity = 8;
		grown = realloc(writer->patterns, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		writer->patterns = grown;
		writer->patterns_capacity = capacity;
	}

	/* Describes the pattern by what finds it again. */
	pattern = &writer->patterns[writer->patterns_count];
	memset(pattern, 0, sizeof(*pattern));
	pattern->tint = tint;
	pattern->cell = cell;
	memcpy(pattern->shown, shown, sizeof(pattern->shown));

	/*
	 * Its matrix: the unit square scaled to the cell, in the page as drawn,
	 * then the page's matrix to its own space (a point p of the pattern
	 * lands on p * cell * shown).
	 */
	pattern->matrix[0] = cell * shown[0];
	pattern->matrix[1] = cell * shown[1];
	pattern->matrix[2] = cell * shown[2];
	pattern->matrix[3] = cell * shown[3];
	pattern->matrix[4] = shown[4];
	pattern->matrix[5] = shown[5];

	/* Succeeded: the pattern is the document's. */
	*index = writer->patterns_count;
	writer->patterns_count++;
	return 0;
}

/* Tells whether any graphics state of the document multiplies. */
static int
uses_multiply(
	const struct pdf_writer *writer)
{
	size_t index;

	/* Looks for a state of Multiply. */
	for (index = 0; index < writer->alphas_count; index++) {
		if (writer->states[index].blend == PDF_BLEND_MULTIPLY)
			return 1;
	}

	/* Every state is Normal. */
	return 0;
}

/* Converts a colour component from 0..1 to the nearest 8-bit level. */
static unsigned char
color_level(
	double value)
{
	/* Rounds to the nearest of the 256 levels; the caller checked the range. */
	return (unsigned char)(value * 255.0 + 0.5);
}

/* Copies a string into a new allocation. */
static int
copy_string(
	const char *source,
	char **copy)
{
	size_t length;
	char *created;

	/* Allocates the copy with its terminator. */
	length = strlen(source);
	created = malloc(length + 1);
	if (created == NULL)
		return ENOMEM;

	/* Copies the characters and the terminator. */
	memcpy(created, source, length + 1);

	/* Succeeded: the caller owns the copy. */
	*copy = created;
	return 0;
}

/*
 * Adds an image to the document and draws it on the open page.
 *
 * On success the document owns the image's bytes; on a refusal the caller
 * still does.
 */
static int
add_image(
	struct pdf_writer *writer,
	struct pdf_writer_image *image,
	double x,
	double y,
	double draw_width,
	double draw_height)
{
	struct pdf_writer_image *grown;
	struct pdf_buffer *content;
	size_t capacity;
	int error;

	/* Refuses drawing outside a page. */
	content = open_content(writer);
	if (content == NULL)
		return EINVAL;

	/* Refuses an x that cannot be written. */
	error = check_coordinate(x);
	if (error != 0)
		return error;

	/* Refuses a y that cannot be written. */
	error = check_coordinate(y);
	if (error != 0)
		return error;

	/* Refuses a rectangle without area or past the limit; the negations also refuse a NaN. */
	if (!(draw_width > 0.0 && draw_width <= PDF_WRITER_COORDINATE_LIMIT))
		return EINVAL;
	if (!(draw_height > 0.0 && draw_height <= PDF_WRITER_COORDINATE_LIMIT))
		return EINVAL;

	/* Refuses an image past the document's limit. */
	if (writer->images_count == PDF_WRITER_IMAGE_MAX)
		return ENOSPC;

	/* Grows the image array when it is full. */
	if (writer->images_count == writer->images_capacity) {
		capacity = writer->images_capacity * 2;
		if (capacity == 0)
			capacity = 8;
		grown = realloc(writer->images, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		writer->images = grown;
		writer->images_capacity = capacity;
	}

	/*
	 * Maps the image's unit square onto the rectangle.  The page's y axis
	 * points down and an image's first row is at the top of its square, so
	 * the square's y is flipped again: its top edge (y = 1) lands on y.
	 */
	pdf_buffer_printf(content, "q ");
	pdf_buffer_append_number(content, draw_width);
	pdf_buffer_printf(content, " 0 0 ");
	pdf_buffer_append_number(content, -draw_height);
	pdf_buffer_append(content, " ", 1);
	pdf_buffer_append_number(content, x);
	pdf_buffer_append(content, " ", 1);
	pdf_buffer_append_number(content, y + draw_height);
	pdf_buffer_printf(content, " cm /%sIm%lu Do Q\n", writer->name_prefix, (unsigned long)writer->images_count);
	if (content->error != 0)
		return content->error;

	/* Publishes the image under the name the content just used. */
	writer->images[writer->images_count] = *image;
	writer->images_count++;

	/* Succeeded: the document owns the image. */
	return 0;
}

/*
 * Reports whether a media type can be written as a PDF name.
 *
 * Only the characters of ordinary media types are accepted; the slash is
 * written as #2F.
 */
static int
mime_type_is_valid(
	const char *mime_type)
{
	const char *character;

	/* A missing or empty type is not a media type. */
	if (mime_type == NULL)
		return 0;
	if (mime_type[0] == '\0')
		return 0;

	/* Accepts letters, digits and the punctuation of media types. */
	for (character = mime_type; *character != '\0'; character++) {
		if (*character >= 'a' && *character <= 'z')
			continue;
		if (*character >= 'A' && *character <= 'Z')
			continue;
		if (*character >= '0' && *character <= '9')
			continue;
		if (*character == '.' ||
		    *character == '+' ||
		    *character == '-' ||
		    *character == '/')
			continue;
		return 0;
	}

	/* Every character can be written in a name. */
	return 1;
}

/*
 * Lays out the whole document.
 *
 * The objects are numbered as follows: 1 the catalog, 2 the page tree, 3 the
 * information dictionary, then a page object and its content stream for each
 * page, then the ExtGStates, the images, the grains with their colours
 * and patterns, the fonts, then the embedded file stream and its file
 * specification.
 */
static int
write_document(
	struct pdf_writer *writer,
	struct pdf_buffer *file,
	time_t now)
{
	static const char header[] = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
	size_t *offsets;
	size_t object_count;
	size_t first_alpha_object;
	size_t next_object;
	size_t file_object;
	size_t index;
	int error;

	/* Numbers the objects after the pages: the opacities, then each image and its mask, the fonts, then the attachment. */
	first_alpha_object = PDF_WRITER_OBJECT_FIRST_PAGE + writer->pages_count * 2;
	writer->first_alpha_object = first_alpha_object;
	next_object = first_alpha_object + writer->alphas_count;
	for (index = 0; index < writer->images_count; index++) {
		writer->images[index].object = next_object;
		next_object++;
		if (writer->images[index].alpha != NULL)
			next_object++;
	}

	/* The grains, their colours and their patterns (ws216-p002), then the embedded fonts' objects (ws175-p005). */
	next_object = pdf_writer_number_paint(writer, next_object);
	next_object = pdf_writer_number_fonts(writer, next_object);

	/* The attached file and its specification come last. */
	file_object = next_object;
	object_count = file_object;
	if (writer->has_attachment)
		object_count += 2;

	/* Allocates the offset of each object; entry 0 is the free list head. */
	offsets = calloc(object_count, sizeof(*offsets));
	if (offsets == NULL)
		return ENOMEM;

	/* Writes the header, whose binary comment marks the file as binary. */
	pdf_buffer_append(file, header, sizeof(header) - 1);

	/* Writes the catalog, the page tree and the information dictionary. */
	offsets[PDF_WRITER_OBJECT_CATALOG] = file->length;
	write_catalog(writer, file, file_object + 1);
	offsets[PDF_WRITER_OBJECT_PAGES] = file->length;
	write_page_tree(writer, file);
	offsets[PDF_WRITER_OBJECT_INFO] = file->length;
	write_information(writer, file, now);

	/* Writes each page and its content stream. */
	for (index = 0; index < writer->pages_count; index++)
		write_page_objects(writer, file, offsets, index);

	/* Writes each opacity's ExtGState. */
	pdf_writer_write_opacities(writer, file, offsets);

	/* Writes each image and its mask, the grains and their patterns (ws216-p002), and the embedded fonts (ws175-p005). */
	for (index = 0; index < writer->images_count; index++)
		pdf_writer_write_image_objects(&writer->images[index], file, offsets);
	pdf_writer_write_paint_objects(writer, file, offsets);
	if (writer->font_layout != NULL) {
		error = writer->font_layout(writer, file, offsets);
		if (error != 0)
			pdf_buffer_fail(file, error);
	}

	/* Writes the attachment. */
	if (writer->has_attachment)
		pdf_writer_write_attachment_objects(writer, file, offsets, file_object);

	/* Writes the cross-reference table and the trailer; the offsets are no longer needed. */
	write_cross_reference(writer, file, offsets, object_count);
	free(offsets);

	/* Reports why the document could not be laid out. */
	if (file->error != 0)
		return file->error;

	/* Succeeded: file holds the whole document. */
	return 0;
}

/* Writes the catalog, which lists the attachment when there is one. */
static void
write_catalog(
	struct pdf_writer *writer,
	struct pdf_buffer *file,
	size_t spec_object)
{
	/* Opens the catalog and names the page tree. */
	pdf_buffer_printf(file, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R");

	/* Lists the attachment's file specification in the name tree and as an associated file. */
	if (writer->has_attachment) {
		pdf_buffer_printf(file, " /Names << /EmbeddedFiles << /Names [");
		pdf_buffer_append_literal_string(file, writer->attachment.name);
		pdf_buffer_printf(file, " %lu 0 R] >> >> /AF [%lu 0 R]", (unsigned long)spec_object, (unsigned long)spec_object);
	}

	/* Closes the catalog. */
	pdf_buffer_printf(file, " >>\nendobj\n");
}

/* Writes the page tree, which lists every page object. */
static void
write_page_tree(
	struct pdf_writer *writer,
	struct pdf_buffer *file)
{
	size_t index;

	/* Opens the tree's list of kids. */
	pdf_buffer_printf(file, "2 0 obj\n<< /Type /Pages /Kids [");

	/* Names each page object; a space separates it from the one before. */
	for (index = 0; index < writer->pages_count; index++) {
		if (index != 0)
			pdf_buffer_append(file, " ", 1);
		pdf_buffer_printf(file, "%lu 0 R", (unsigned long)(PDF_WRITER_OBJECT_FIRST_PAGE + index * 2));
	}

	/* Closes the list with the page count. */
	pdf_buffer_printf(file, "] /Count %lu >>\nendobj\n", (unsigned long)writer->pages_count);
}

/* Writes the information dictionary with the producer and both dates. */
static void
write_information(
	struct pdf_writer *writer,
	struct pdf_buffer *file,
	time_t now)
{
	time_t modification;

	/* Uses the caller's modification date, or the time of this save. */
	modification = writer->modification_time;
	if (modification == 0)
		modification = now;

	/* Writes the producer and the two dates. */
	pdf_buffer_printf(file, "3 0 obj\n<< /Producer (" ZEDBSD_OS_NAME " Notes) /CreationDate ");
	pdf_buffer_append_date(file, writer->creation_time);
	pdf_buffer_printf(file, " /ModDate ");
	pdf_buffer_append_date(file, modification);
	pdf_buffer_printf(file, " >>\nendobj\n");
}

/* Writes one page object and its content stream. */
static void
write_page_objects(
	struct pdf_writer *writer,
	struct pdf_buffer *file,
	size_t *offsets,
	size_t page_index)
{
	struct pdf_writer_page *page;
	size_t page_object;

	/* Finds the page and its object number; the content stream follows it. */
	page = writer->pages[page_index];
	page_object = PDF_WRITER_OBJECT_FIRST_PAGE + page_index * 2;

	/* A page whose content failed to be recorded cannot be saved. */
	if (page->content.error != 0) {
		pdf_buffer_fail(file, page->content.error);
		return;
	}

	/* Writes the page with its media box and resources. */
	offsets[page_object] = file->length;
	pdf_buffer_printf(file, "%lu 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ", (unsigned long)page_object);
	pdf_buffer_append_number(file, page->width);
	pdf_buffer_append(file, " ", 1);
	pdf_buffer_append_number(file, page->height);
	pdf_buffer_printf(file, "] /Resources ");
	pdf_writer_write_resources(writer, file);
	pdf_buffer_printf(file, " /Contents %lu 0 R >>\nendobj\n", (unsigned long)(page_object + 1));

	/* Writes the content stream, compressed when the writer compresses content. */
	offsets[page_object + 1] = file->length;
	pdf_writer_write_content_stream(writer, file, page_object + 1, &page->content);
}

/*
 * Writes the classic cross-reference table, the trailer and the end marker.
 *
 * The trailer's identifier pairs the document's permanent identifier with
 * one that changes with the bytes of this revision.
 */
static void
write_cross_reference(
	struct pdf_writer *writer,
	struct pdf_buffer *file,
	const size_t *offsets,
	size_t object_count)
{
	unsigned char version_id[PDF_WRITER_ID_SIZE];
	size_t table_offset;
	size_t index;

	/* Derives this revision's identifier from every byte laid out so far. */
	pdf_writer_hash_version_id(file, 0, version_id);

	/* Writes the table's header and the free list head. */
	table_offset = file->length;
	pdf_buffer_printf(file, "xref\n0 %lu\n0000000000 65535 f \n", (unsigned long)object_count);

	/* Writes each object's offset as a twenty-byte entry. */
	for (index = 1; index < object_count; index++)
		pdf_buffer_printf(file, "%010lu 00000 n \n", (unsigned long)offsets[index]);

	/* Writes the trailer with the identifier pair. */
	pdf_buffer_printf(file, "trailer\n<< /Size %lu /Root 1 0 R /Info 3 0 R /ID [", (unsigned long)object_count);
	pdf_buffer_append_hex_string(file, writer->document_id, PDF_WRITER_ID_SIZE);
	pdf_buffer_append(file, " ", 1);
	pdf_buffer_append_hex_string(file, version_id, PDF_WRITER_ID_SIZE);
	pdf_buffer_printf(file, "] >>\n");

	/* Writes the offset of the table and the end marker. */
	pdf_buffer_printf(file, "startxref\n%lu\n%%%%EOF\n", (unsigned long)table_offset);
}
