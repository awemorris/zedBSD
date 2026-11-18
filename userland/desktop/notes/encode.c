/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The edit data of Notes (plan/ws079/design-pdf.md section 2.1).
 *
 * A saved PDF carries the whole document as an attached file: the magic
 * "ZNOT", the version 1.1, then chunks (a four-letter tag, a 32-bit length,
 * the body).  DOC gives the page count, the pressure's range, the time base
 * and the next stroke number; TOOL lists the tools (kind, colour, width);
 * each PAGE gives a page's size, its background and its strokes.  Version
 * 1.1 adds, for a notebook written on another program's PDF, BASE (the
 * length and the SHA-256 of that PDF's bytes, which the file starts with)
 * and SRC after each page of that PDF (which page it is, and whether the
 * strokes are drawn over it or replace its content).  A stroke's
 * samples are stored as differences from the sample before, as LEB128
 * numbers (zigzag for signed ones): positions in 1/64 point, the pressure,
 * the tilt in 1/100 degree when the stroke has it, and the time in
 * milliseconds.  A reader skips the chunks it does not know, which is how a
 * later minor version adds to the data.
 *
 * The journal stores one stroke at a time in the same way, with its tool
 * written out instead of an index into TOOL.
 *
 * Version 2.0 (ws175-p007, plan/ws175/phase001/design.md section 6.3) is
 * written when a page has edits of the PDF's objects: IMAG after TOOL, one
 * an image (its number, form, size, components and orientation; its bytes
 * are the PDF's image of that number), and EDIT after a page's PAGE and
 * SRC (each edit's flags, its object -- an inserted object's number, or
 * the key of one of the page's own --, its transform, its image's number).
 * A reader of version 1 refuses it, and opens the file as another
 * program's PDF, the edits drawn in it.  A notebook without edits is
 * written as version 1.1, as before.
 *
 * Version 3.0 (ws216-p003, plan/ws216/phase001/phase.md section 5.4) is
 * written when a stroke is of the brush, the pencil, the watercolour or
 * the fill; it is 2.0 and more.  TOOL's curve byte is the version of the
 * tool's constants, and a stroke's flags may say that its baked width
 * factors follow its samples (FACTORS: the column count, then each
 * sample's factors), its darkness steps after them (STEPS, the pencil),
 * and that its samples are the corners of contours (CONTOURS, a fill:
 * after the sample count, the contour count and each contour's samples).
 * A fill's samples have no times.  A reader of versions 1 and 2 refuses
 * it and opens the file as another program's PDF; a document whose
 * constants this file does not know is refused the same way.
 */

#include "notes.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The version of the edit data this file writes and reads, and the one with edits of the PDF's objects. */
#define ENCODE_MAJOR		1U
#define ENCODE_MINOR		1U
#define ENCODE_MAJOR_EDITS	2U
#define ENCODE_MINOR_EDITS	0U

/* The version with the drawing tools of ws216 (brush, pencil, watercolour, fill). */
#define ENCODE_MAJOR_TOOLS	3U
#define ENCODE_MINOR_TOOLS	0U

/* The journal's version whose stroke records carry the tool's constants (journal.c). */
#define ENCODE_JOURNAL_TOOLS	3U

/* The grid of an edit's transform's first four numbers, and the largest of them. */
#define ENCODE_SCALE_UNITS	65536.0
#define ENCODE_SCALE_MAX	100000.0

/* The most images and edits of a page an edit data may declare. */
#define ENCODE_IMAGES_MAX	100000U
#define ENCODE_EDITS_MAX	100000U

/* The longest new words of a line, in bytes. */
#define ENCODE_TEXT_MAX		65536U

/*
 * The stroke flags: the samples carry the tilt, and their times; version
 * 3.0 adds the baked factors, the pencil's steps and a fill's contours.
 */
#define ENCODE_STROKE_TILT	0x01U
#define ENCODE_STROKE_TIME	0x02U
#define ENCODE_STROKE_FACTORS	0x04U
#define ENCODE_STROKE_STEPS	0x08U
#define ENCODE_STROKE_CONTOURS	0x10U

/* The most contours a fill may declare. */
#define ENCODE_CONTOURS_MAX	65535U

/* The largest counts a document may declare, which keep a damaged file from asking for all memory. */
#define ENCODE_PAGES_MAX	100000U
#define ENCODE_TOOLS_MAX	4096U
#define ENCODE_POINTS_MAX	10000000U

/* The largest page side and stroke width, in points. */
#define ENCODE_LENGTH_MAX	100000.0f

/* The smallest room a buffer grows to. */
#define ENCODE_BUFFER_MIN	256U

/*
 * One tool of the TOOL chunk: a kind, a colour, a width and the version of
 * the kind's constants (ws216-p003).
 */
struct encode_tool {
	unsigned kind;
	uint32_t color;
	float width;
	unsigned constants;
};

/*
 * The tools a document uses, gathered while it is encoded.
 */
struct encode_tools {
	struct encode_tool *tools;
	size_t count;
	size_t capacity;
};

/*
 * The images of an edit data: gathered while it is encoded (each once), and
 * read from IMAG while it is decoded (each held once by the list).
 */
struct encode_images {
	struct notes_image **images;
	size_t count;
	size_t capacity;
};

/*
 * A cursor over bytes being read.
 *
 * error stays nonzero once a read runs past the end or finds a malformed
 * number; every later read then gives zero, and the caller checks error
 * once per step.
 */
struct encode_reader {
	const unsigned char *data;
	size_t length;
	size_t offset;
	int error;
};

static void buffer_reserve(struct notes_buffer *buffer, size_t more);
static int tools_find(struct encode_tools *tools, const struct notes_stroke *stroke, size_t *index);
static void encode_samples(struct notes_buffer *buffer, const struct notes_stroke *stroke, uint64_t first_time, int timed);
static int decode_samples(struct encode_reader *reader, struct notes_stroke *stroke, unsigned flags, size_t count, uint64_t *first_time);
static int stroke_flags(const struct notes_stroke *stroke, unsigned *flags);
static void encode_stroke_tail(struct notes_buffer *buffer, const struct notes_stroke *stroke, unsigned flags, uint64_t first_time);
static int decode_stroke_tail(struct encode_reader *reader, struct notes_stroke *stroke, unsigned flags, int tools, uint64_t *first_time);
static int check_kind(unsigned kind, unsigned constants, int tools);
static int decode_doc(struct encode_reader *reader, struct notes_document *document, size_t *pages);
static int decode_tools(struct encode_reader *reader, struct encode_tools *tools, unsigned major);
static int decode_page(struct encode_reader *reader, struct notes_document *document, const struct encode_tools *tools, size_t index, unsigned major);
static int decode_base(struct encode_reader *reader, struct notes_document *document);
static int decode_source(struct encode_reader *reader, struct notes_document *document);
static int images_add(struct encode_images *images, struct notes_image *image);
static struct notes_image *images_find(const struct encode_images *images, uint32_t id);
static void images_free(struct encode_images *images, int held);
static void encode_edits(struct notes_buffer *chunk, const struct notes_page *page, size_t index);
static int decode_image(struct encode_reader *reader, struct encode_images *images);
static int decode_edits(struct encode_reader *reader, struct notes_document *document, const struct encode_images *images);
static unsigned read_u8(struct encode_reader *reader);
static unsigned read_u16(struct encode_reader *reader);
static uint32_t read_u32(struct encode_reader *reader);
static uint64_t read_u64(struct encode_reader *reader);
static uint64_t read_varint(struct encode_reader *reader);
static int64_t read_zigzag(struct encode_reader *reader);
static float read_length(struct encode_reader *reader);
static int64_t units(float value);

/*
 * Makes an empty buffer.
 */
void
notes_buffer_init(
	struct notes_buffer *buffer)
{
	/* No bytes and no failure. */
	memset(buffer, 0, sizeof(*buffer));
}

/*
 * Frees a buffer's bytes.
 */
void
notes_buffer_free(
	struct notes_buffer *buffer)
{
	/* The bytes, and the buffer is empty again. */
	free(buffer->data);
	memset(buffer, 0, sizeof(*buffer));
}

/*
 * Appends bytes; a buffer that failed takes nothing more.
 */
void
notes_buffer_bytes(
	struct notes_buffer *buffer,
	const void *data,
	size_t length)
{
	/* Room for the bytes, or the buffer's failure. */
	buffer_reserve(buffer, length);
	if (buffer->error != 0)
		return;

	/* Copies them to the end. */
	memcpy(buffer->data + buffer->length, data, length);
	buffer->length += length;
}

/*
 * Appends one byte.
 */
void
notes_buffer_u8(
	struct notes_buffer *buffer,
	unsigned value)
{
	unsigned char byte;

	/* The low eight bits. */
	byte = (unsigned char)(value & 0xffU);
	notes_buffer_bytes(buffer, &byte, 1U);
}

/*
 * Appends a 16-bit number, least significant byte first.
 */
void
notes_buffer_u16(
	struct notes_buffer *buffer,
	unsigned value)
{
	unsigned char bytes[2];

	/* The two bytes, low first. */
	bytes[0] = (unsigned char)(value & 0xffU);
	bytes[1] = (unsigned char)((value >> 8) & 0xffU);
	notes_buffer_bytes(buffer, bytes, sizeof(bytes));
}

/*
 * Appends a 32-bit number, least significant byte first.
 */
void
notes_buffer_u32(
	struct notes_buffer *buffer,
	uint32_t value)
{
	unsigned char bytes[4];
	unsigned index;

	/* The four bytes, low first. */
	for (index = 0; index < 4U; index++)
		bytes[index] = (unsigned char)((value >> (index * 8U)) & 0xffU);
	notes_buffer_bytes(buffer, bytes, sizeof(bytes));
}

/*
 * Appends a 64-bit number, least significant byte first.
 */
void
notes_buffer_u64(
	struct notes_buffer *buffer,
	uint64_t value)
{
	unsigned char bytes[8];
	unsigned index;

	/* The eight bytes, low first. */
	for (index = 0; index < 8U; index++)
		bytes[index] = (unsigned char)((value >> (index * 8U)) & 0xffU);
	notes_buffer_bytes(buffer, bytes, sizeof(bytes));
}

/*
 * Appends an unsigned LEB128 number: seven bits a byte, low first, the top
 * bit set on every byte but the last.
 */
void
notes_buffer_varint(
	struct notes_buffer *buffer,
	uint64_t value)
{
	unsigned char bytes[10];
	size_t count;

	/* Seven bits at a time until what is left fits in one byte. */
	count = 0;
	while (value >= 0x80U) {
		bytes[count] = (unsigned char)((value & 0x7fU) | 0x80U);
		count++;
		value >>= 7;
	}

	/* The last byte, without the continuation bit. */
	bytes[count] = (unsigned char)value;
	count++;
	notes_buffer_bytes(buffer, bytes, count);
}

/*
 * Appends a signed number as a zigzag LEB128 number (0, -1, 1, -2 ... as
 * 0, 1, 2, 3 ...), which keeps small differences of either sign short.
 */
void
notes_buffer_zigzag(
	struct notes_buffer *buffer,
	int64_t value)
{
	uint64_t folded;

	/* Positive values go to the even numbers, negative ones to the odd. */
	if (value < 0)
		folded = ((uint64_t)(-(value + 1)) << 1) | 1U;
	else
		folded = (uint64_t)value << 1;
	notes_buffer_varint(buffer, folded);
}

/*
 * Encodes a whole document as the edit data.
 *
 * Returns 0, or ENOMEM (the buffer's contents are then of no use).
 */
int
notes_encode_document(
	const struct notes_document *document,
	struct notes_buffer *buffer)
{
	struct encode_tools tools;
	struct encode_images images;
	struct notes_buffer chunk;
	const struct notes_page *page;
	const struct notes_stroke *stroke;
	uint64_t first_time;
	size_t page_index;
	size_t stroke_index;
	size_t edit_index;
	size_t tool;
	unsigned flags;
	int edited;
	int drawn;
	int error;

	/*
	 * The header: magic, the lowest version that holds the document (3.0
	 * with the drawing tools of ws216, 2.0 with edits of the PDF's objects,
	 * 1.1 otherwise) and no flags.
	 */
	memset(&tools, 0, sizeof(tools));
	memset(&images, 0, sizeof(images));
	notes_buffer_init(&chunk);
	notes_buffer_bytes(buffer, "ZNOT", 4U);
	edited = notes_document_edited(document);
	drawn = notes_document_new_tools(document);
	if (drawn) {
		notes_buffer_u16(buffer, ENCODE_MAJOR_TOOLS);
		notes_buffer_u16(buffer, ENCODE_MINOR_TOOLS);
	} else if (edited) {
		notes_buffer_u16(buffer, ENCODE_MAJOR_EDITS);
		notes_buffer_u16(buffer, ENCODE_MINOR_EDITS);
	} else {
		notes_buffer_u16(buffer, ENCODE_MAJOR);
		notes_buffer_u16(buffer, ENCODE_MINOR);
	}

	/* No flags. */
	notes_buffer_u32(buffer, 0U);

	/* DOC: the pages, the pressure's range, the time base and the next stroke number. */
	notes_buffer_varint(&chunk, document->page_count);
	notes_buffer_u16(&chunk, NOTES_PRESSURE_MAX);
	notes_buffer_u64(&chunk, document->time_base);
	notes_buffer_varint(&chunk, document->next_id);
	notes_buffer_bytes(buffer, "DOC ", 4U);
	notes_buffer_u32(buffer, (uint32_t)chunk.length);
	notes_buffer_bytes(buffer, chunk.data, chunk.length);
	chunk.length = 0;

	/* BASE (version 1.1): the PDF the notebook writes on, by the length and the SHA-256 of its bytes. */
	if (document->base_size != 0U) {
		notes_buffer_u64(&chunk, document->base_size);
		notes_buffer_bytes(&chunk, document->base_hash, sizeof(document->base_hash));
		notes_buffer_bytes(buffer, "BASE", 4U);
		notes_buffer_u32(buffer, (uint32_t)chunk.length);
		notes_buffer_bytes(buffer, chunk.data, chunk.length);
		chunk.length = 0;
	}

	/* Gathers the tools every stroke uses, so that TOOL comes before the pages. */
	for (page_index = 0; page_index < document->page_count; page_index++) {
		page = document->pages[page_index];
		for (stroke_index = 0; stroke_index < page->stroke_count; stroke_index++) {
			error = tools_find(&tools, page->strokes[stroke_index], &tool);
			if (error != 0) {
				free(tools.tools);
				notes_buffer_free(&chunk);
				return error;
			}
		}
	}

	/* TOOL: each tool's kind, colour (red, green, blue, alpha), width, constants' version (the curve in 1.1) and flags. */
	notes_buffer_varint(&chunk, tools.count);
	for (tool = 0; tool < tools.count; tool++) {
		notes_buffer_u8(&chunk, tools.tools[tool].kind);
		notes_buffer_u8(&chunk, (tools.tools[tool].color >> 24) & 0xffU);
		notes_buffer_u8(&chunk, (tools.tools[tool].color >> 16) & 0xffU);
		notes_buffer_u8(&chunk, (tools.tools[tool].color >> 8) & 0xffU);
		notes_buffer_u8(&chunk, tools.tools[tool].color & 0xffU);
		notes_buffer_varint(&chunk, (uint64_t)units(tools.tools[tool].width));
		notes_buffer_u8(&chunk, tools.tools[tool].constants);
		notes_buffer_u8(&chunk, 0U);
	}

	/* The chunk. */
	notes_buffer_bytes(buffer, "TOOL", 4U);
	notes_buffer_u32(buffer, (uint32_t)chunk.length);
	notes_buffer_bytes(buffer, chunk.data, chunk.length);
	chunk.length = 0;

	/* IMAG (version 2.0): each image an edit holds, once. */
	for (page_index = 0; page_index < document->page_count; page_index++) {
		page = document->pages[page_index];
		for (edit_index = 0; edit_index < page->edit_count; edit_index++) {
			if (page->edits[edit_index]->image == NULL)
				continue;
			error = images_add(&images, page->edits[edit_index]->image);
			if (error != 0) {
				images_free(&images, 0);
				free(tools.tools);
				notes_buffer_free(&chunk);
				return error;
			}
		}
	}

	/* One chunk an image. */
	for (edit_index = 0; edit_index < images.count; edit_index++) {
		notes_encode_image(&chunk, images.images[edit_index]);
		notes_buffer_bytes(buffer, "IMAG", 4U);
		notes_buffer_u32(buffer, (uint32_t)chunk.length);
		notes_buffer_bytes(buffer, chunk.data, chunk.length);
		chunk.length = 0;
	}

	/* The list goes (the pages hold the images). */
	images_free(&images, 0);

	/* PAGE, one a page, with the digest of its content stream as last saved (zero when not known). */
	for (page_index = 0; page_index < document->page_count; page_index++) {
		/* The page's number, size, background, digest and stroke count. */
		page = document->pages[page_index];
		notes_buffer_varint(&chunk, page_index);
		notes_buffer_varint(&chunk, (uint64_t)units(page->width));
		notes_buffer_varint(&chunk, (uint64_t)units(page->height));
		notes_buffer_u8(&chunk, page->background);
		notes_buffer_bytes(&chunk, page->content_hash, sizeof(page->content_hash));
		notes_buffer_varint(&chunk, page->stroke_count);

		/* Each stroke: number, tool, flags, samples (and the baked values, ws216-p003). */
		for (stroke_index = 0; stroke_index < page->stroke_count; stroke_index++) {
			/* The flags; a stroke without the values its tool needs cannot be kept. */
			stroke = page->strokes[stroke_index];
			error = stroke_flags(stroke, &flags);
			if (error != 0) {
				free(tools.tools);
				notes_buffer_free(&chunk);
				return error;
			}

			/* The number, the tool and the flags. */
			(void)tools_find(&tools, stroke, &tool);
			notes_buffer_varint(&chunk, stroke->id);
			notes_buffer_varint(&chunk, tool);
			notes_buffer_u8(&chunk, flags);

			/* The first sample's time counts from the document's time base. */
			first_time = 0U;
			if (stroke->start_ms > document->time_base)
				first_time = stroke->start_ms - document->time_base;
			encode_stroke_tail(&chunk, stroke, flags, first_time);
		}

		/* The chunk. */
		notes_buffer_bytes(buffer, "PAGE", 4U);
		notes_buffer_u32(buffer, (uint32_t)chunk.length);
		notes_buffer_bytes(buffer, chunk.data, chunk.length);
		chunk.length = 0;

		/* SRC (version 1.1), after a page of the PDF the notebook writes on: the page's number, origin and source. */
		if (page->origin != NOTES_ORIGIN_NEW) {
			notes_buffer_varint(&chunk, page_index);
			notes_buffer_u8(&chunk, page->origin);
			notes_buffer_varint(&chunk, page->source);
			notes_buffer_bytes(buffer, "SRC ", 4U);
			notes_buffer_u32(buffer, (uint32_t)chunk.length);
			notes_buffer_bytes(buffer, chunk.data, chunk.length);
			chunk.length = 0;
		}

		/* EDIT (version 2.0), after a page with edits. */
		if (page->edit_count > 0U) {
			encode_edits(&chunk, page, page_index);
			notes_buffer_bytes(buffer, "EDIT", 4U);
			notes_buffer_u32(buffer, (uint32_t)chunk.length);
			notes_buffer_bytes(buffer, chunk.data, chunk.length);
			chunk.length = 0;
		}
	}

	/* The scratch buffers go; a failure of either is the encoding's. */
	free(tools.tools);
	error = chunk.error;
	notes_buffer_free(&chunk);
	if (error != 0)
		return error;
	if (buffer->error != 0)
		return buffer->error;

	/* Succeeded: the buffer holds the edit data. */
	return 0;
}

/*
 * Decodes edit data into an empty document.
 *
 * The document is made here (notes_document_init is not called first);
 * on failure it is freed again.  Returns 0, EINVAL for data that is not
 * edit data of a version this file reads or is damaged, ENOTSUP for a
 * tool whose constants are of a version this file does not know, or
 * ENOMEM.
 */
int
notes_decode_document(
	const void *data,
	size_t size,
	struct notes_document *document)
{
	struct encode_reader reader;
	struct encode_tools tools;
	struct encode_images images;
	unsigned char tag[4];
	size_t declared;
	size_t pages;
	size_t length;
	size_t end;
	unsigned major;
	int seen_doc;
	int matched;
	int is_doc;
	int is_tool;
	int is_page;
	int is_base;
	int is_source;
	int is_image;
	int is_edit;
	int error;

	/* An empty document to fill, and a reader at the start. */
	memset(document, 0, sizeof(*document));
	memset(&tools, 0, sizeof(tools));
	memset(&reader, 0, sizeof(reader));
	reader.data = data;
	reader.length = size;

	/* The magic, after the header's twelve bytes are known to be there. */
	if (size < 12U)
		return EINVAL;
	matched = memcmp(data, "ZNOT", 4U);
	if (matched != 0)
		return EINVAL;
	reader.offset = 4U;

	/*
	 * A major version this file does not know is not read (design-pdf.md
	 * section 2.1; 2 has edits, ws175-p007; 3 the drawing tools, ws216-p003).
	 */
	major = read_u16(&reader);
	(void)read_u16(&reader);
	(void)read_u32(&reader);
	if (major != ENCODE_MAJOR &&
	    major != ENCODE_MAJOR_EDITS &&
	    major != ENCODE_MAJOR_TOOLS)
		return EINVAL;
	memset(&images, 0, sizeof(images));

	/* Each chunk, until the data ends. */
	declared = 0;
	pages = 0;
	seen_doc = 0;
	error = 0;
	while (reader.offset < reader.length && error == 0) {
		/* The tag and the body's length, which must fit in the data. */
		if (reader.length - reader.offset < 8U) {
			error = EINVAL;
			break;
		}

		/* The tag, then the length of the body. */
		memcpy(tag, reader.data + reader.offset, sizeof(tag));
		reader.offset += 4U;
		length = read_u32(&reader);
		if (length > reader.length - reader.offset) {
			error = EINVAL;
			break;
		}

		/* Where the next chunk starts. */
		end = reader.offset + length;

		/*
		 * The chunks this version knows (DOC once, TOOL once, PAGE after
		 * DOC, BASE after DOC, SRC after its page); any other is skipped.
		 */
		is_doc = memcmp(tag, "DOC ", 4U);
		is_tool = memcmp(tag, "TOOL", 4U);
		is_page = memcmp(tag, "PAGE", 4U);
		is_base = memcmp(tag, "BASE", 4U);
		is_source = memcmp(tag, "SRC ", 4U);
		is_image = memcmp(tag, "IMAG", 4U);
		is_edit = memcmp(tag, "EDIT", 4U);
		if (is_doc == 0 && !seen_doc) {
			error = decode_doc(&reader, document, &declared);
			seen_doc = 1;
		} else if (is_tool == 0 && tools.count == 0U) {
			error = decode_tools(&reader, &tools, major);
		} else if (is_page == 0 && seen_doc) {
			error = decode_page(&reader, document, &tools, pages, major);
			pages++;
		} else if (is_base == 0 && seen_doc) {
			error = decode_base(&reader, document);
		} else if (is_source == 0 && seen_doc) {
			error = decode_source(&reader, document);
		} else if (is_image == 0 && seen_doc && major >= ENCODE_MAJOR_EDITS) {
			error = decode_image(&reader, &images);
		} else if (is_edit == 0 && seen_doc && major >= ENCODE_MAJOR_EDITS) {
			error = decode_edits(&reader, document, &images);
		}

		/* A body read past its end is damaged; the next chunk starts after it. */
		if (error == 0 && reader.offset > end)
			error = EINVAL;
		reader.offset = end;
	}

	/* The tools are not needed any more, nor the list's hold on the images (the edits hold theirs). */
	free(tools.tools);
	images_free(&images, 1);

	/* A document needs its DOC chunk and the pages it declared, at least one. */
	if (error == 0 &&
	    (!seen_doc ||
	     pages != declared ||
	     pages == 0U))
		error = EINVAL;
	if (error != 0) {
		notes_document_free(document);
		return error;
	}

	/* Succeeded: the document as it was saved, unchanged since. */
	document->dirty = 0;
	return 0;
}

/*
 * Encodes one stroke with its tool written out, for a journal of a version
 * (2, or 3 with the tool's constants and the baked values, ws216-p003).
 *
 * Returns 0, EINVAL for a stroke the version cannot hold (or without the
 * values its tool needs), or ENOMEM.
 */
int
notes_encode_stroke(
	const struct notes_stroke *stroke,
	unsigned version,
	struct notes_buffer *buffer)
{
	unsigned constants;
	unsigned flags;
	int error;

	/* The flags; the values the tool needs must be there. */
	error = stroke_flags(stroke, &flags);
	if (error != 0)
		return error;

	/* A journal of version 2 holds the pen and the highlighter only. */
	constants = notes_tool_constants(stroke->tool);
	if (version < ENCODE_JOURNAL_TOOLS && constants != 0U)
		return EINVAL;

	/* The stroke's number and its tool, then its constants' version (version 3). */
	notes_buffer_varint(buffer, stroke->id);
	notes_buffer_u8(buffer, stroke->tool);
	if (version >= ENCODE_JOURNAL_TOOLS)
		notes_buffer_u8(buffer, stroke->constants);

	/* Its colour, its width, its flags and when it started. */
	notes_buffer_u32(buffer, stroke->color);
	notes_buffer_varint(buffer, (uint64_t)units(stroke->width));
	notes_buffer_u8(buffer, flags);
	notes_buffer_u64(buffer, stroke->start_ms);

	/* Its samples and values; the first sample's time counts from the stroke's start. */
	encode_stroke_tail(buffer, stroke, flags, 0U);
	if (buffer->error != 0)
		return buffer->error;

	/* Succeeded: the stroke is in the buffer. */
	return 0;
}

/*
 * Decodes one stroke that notes_encode_stroke() wrote for a journal of a
 * version.
 *
 * *used tells how many bytes it took.  Returns 0, EINVAL, ENOTSUP for a
 * tool's constants of a version this file does not know, or ENOMEM.
 */
int
notes_decode_stroke(
	const unsigned char *data,
	size_t size,
	unsigned version,
	size_t *used,
	struct notes_stroke **stroke)
{
	struct encode_reader reader;
	struct notes_stroke *made;
	uint64_t first_time;
	uint32_t id;
	uint32_t color;
	unsigned tool;
	unsigned constants;
	unsigned flags;
	float width;
	uint64_t start;
	int tools;
	int error;

	/* A reader over the bytes. */
	memset(&reader, 0, sizeof(reader));
	reader.data = data;
	reader.length = size;

	/* The number and the tool, then the constants' version (version 3; none before). */
	id = (uint32_t)read_varint(&reader);
	tool = read_u8(&reader);
	tools = 0;
	constants = 0U;
	if (version >= ENCODE_JOURNAL_TOOLS) {
		tools = 1;
		constants = read_u8(&reader);
	}

	/* The colour, width, flags and start. */
	color = read_u32(&reader);
	width = read_length(&reader);
	flags = read_u8(&reader);
	start = read_u64(&reader);
	if (reader.error != 0)
		return EINVAL;

	/* A stroke of no width is damaged. */
	if (!(width > 0.0f))
		return EINVAL;

	/* A tool the version does not have, or constants this file does not know, are refused. */
	error = check_kind(tool, constants, tools);
	if (error != 0)
		return error;

	/* The stroke. */
	made = notes_stroke_create(id, tool, color, width, start);
	if (made == NULL)
		return ENOMEM;

	/* Its samples and values. */
	error = decode_stroke_tail(&reader, made, flags, tools, &first_time);
	if (error != 0) {
		notes_stroke_free(made);
		return error;
	}

	/* Succeeded: the caller owns the stroke. */
	*used = reader.offset;
	*stroke = made;
	return 0;
}

/*
 * Tells whether a document has a stroke of the drawing tools of ws216,
 * which only the edit data's version 3.0 (and the journal's version 3)
 * holds.
 */
int
notes_document_new_tools(
	const struct notes_document *document)
{
	const struct notes_page *page;
	size_t page_index;
	size_t stroke_index;
	unsigned constants;

	/* Each stroke of each page. */
	for (page_index = 0; page_index < document->page_count; page_index++) {
		page = document->pages[page_index];
		for (stroke_index = 0; stroke_index < page->stroke_count; stroke_index++) {
			constants = notes_tool_constants(page->strokes[stroke_index]->tool);
			if (constants != 0U)
				return 1;
		}
	}

	/* Only the pen and the highlighter. */
	return 0;
}

/* Makes room for more bytes, or marks the buffer failed. */
static void
buffer_reserve(
	struct notes_buffer *buffer,
	size_t more)
{
	unsigned char *larger;
	size_t capacity;

	/* A failed buffer stays failed. */
	if (buffer->error != 0)
		return;

	/* Refuses a length that overflows. */
	if (more > (size_t)-1 / 2U - buffer->length) {
		buffer->error = ENOMEM;
		return;
	}

	/* Room enough already. */
	if (buffer->length + more <= buffer->capacity)
		return;

	/* Doubles the room from a small start until the bytes fit. */
	capacity = buffer->capacity;
	if (capacity < ENCODE_BUFFER_MIN)
		capacity = ENCODE_BUFFER_MIN;
	while (capacity < buffer->length + more)
		capacity *= 2U;

	/* Reallocates the bytes. */
	larger = realloc(buffer->data, capacity);
	if (larger == NULL) {
		buffer->error = ENOMEM;
		return;
	}

	/* Succeeded: the buffer has room. */
	buffer->data = larger;
	buffer->capacity = capacity;
}

/* Finds a stroke's tool in the list, adding it when it is new. */
static int
tools_find(
	struct encode_tools *tools,
	const struct notes_stroke *stroke,
	size_t *index)
{
	struct encode_tool *larger;
	size_t capacity;
	size_t tool;

	/* A tool of the same kind, colour, width and constants is the stroke's. */
	for (tool = 0; tool < tools->count; tool++) {
		if (tools->tools[tool].kind != stroke->tool)
			continue;
		if (tools->tools[tool].color != stroke->color)
			continue;
		if (tools->tools[tool].width != stroke->width)
			continue;
		if (tools->tools[tool].constants != stroke->constants)
			continue;
		*index = tool;
		return 0;
	}

	/* Room for another tool. */
	if (tools->count == tools->capacity) {
		capacity = tools->capacity * 2U;
		if (capacity < 8U)
			capacity = 8U;
		larger = realloc(tools->tools, capacity * sizeof(*larger));
		if (larger == NULL)
			return ENOMEM;
		tools->tools = larger;
		tools->capacity = capacity;
	}

	/* Succeeded: the new tool is the last. */
	tools->tools[tools->count].kind = stroke->tool;
	tools->tools[tools->count].color = stroke->color;
	tools->tools[tools->count].width = stroke->width;
	tools->tools[tools->count].constants = stroke->constants;
	*index = tools->count;
	tools->count++;
	return 0;
}

/* Writes a stroke's samples as differences from the sample before, with their times when it is timed. */
static void
encode_samples(
	struct notes_buffer *buffer,
	const struct notes_stroke *stroke,
	uint64_t first_time,
	int timed)
{
	const struct notes_point *point;
	int64_t x;
	int64_t y;
	int64_t last_x;
	int64_t last_y;
	int64_t last_pressure;
	int64_t last_tilt_x;
	int64_t last_tilt_y;
	uint32_t last_time;
	size_t index;

	/* The sample before the first is all zero. */
	last_x = 0;
	last_y = 0;
	last_pressure = 0;
	last_tilt_x = 0;
	last_tilt_y = 0;
	last_time = 0U;

	/* Each sample. */
	for (index = 0; index < stroke->point_count; index++) {
		/* The position, in 1/64 point. */
		point = &stroke->points[index];
		x = units(point->x);
		y = units(point->y);
		notes_buffer_zigzag(buffer, x - last_x);
		notes_buffer_zigzag(buffer, y - last_y);
		last_x = x;
		last_y = y;

		/* The pressure. */
		notes_buffer_zigzag(buffer, (int64_t)point->pressure - last_pressure);
		last_pressure = point->pressure;

		/* The tilt, when the stroke has it. */
		if (stroke->has_tilt) {
			notes_buffer_zigzag(buffer, (int64_t)point->tilt_x - last_tilt_x);
			notes_buffer_zigzag(buffer, (int64_t)point->tilt_y - last_tilt_y);
			last_tilt_x = point->tilt_x;
			last_tilt_y = point->tilt_y;
		}

		/* A fill's samples have no time (ws216-p003). */
		if (!timed)
			continue;

		/* The time: the first from the given base, the others from the sample before (never backwards). */
		if (index == 0U) {
			notes_buffer_varint(buffer, first_time + point->time_ms);
		} else if (point->time_ms >= last_time) {
			notes_buffer_varint(buffer, point->time_ms - last_time);
		} else {
			notes_buffer_varint(buffer, 0U);
		}

		/* A sample that went backwards in time keeps the later time. */
		if (point->time_ms > last_time)
			last_time = point->time_ms;
	}
}

/* Reads a stroke's samples; *first_time gets the first sample's time from its base. */
static int
decode_samples(
	struct encode_reader *reader,
	struct notes_stroke *stroke,
	unsigned flags,
	size_t count,
	uint64_t *first_time)
{
	struct notes_point point;
	int64_t x;
	int64_t y;
	int64_t pressure;
	int64_t tilt_x;
	int64_t tilt_y;
	uint64_t time;
	size_t index;
	int error;

	/* The sample before the first is all zero. */
	x = 0;
	y = 0;
	pressure = 0;
	tilt_x = 0;
	tilt_y = 0;
	time = 0U;
	*first_time = 0U;
	stroke->has_tilt = 0;
	if ((flags & ENCODE_STROKE_TILT) != 0U)
		stroke->has_tilt = 1;

	/* Each sample. */
	for (index = 0; index < count; index++) {
		/* The position and the pressure. */
		x += read_zigzag(reader);
		y += read_zigzag(reader);
		pressure += read_zigzag(reader);

		/* The tilt, when the stroke has it. */
		if (stroke->has_tilt) {
			tilt_x += read_zigzag(reader);
			tilt_y += read_zigzag(reader);
		}

		/* The time: the first sample's is the stroke's start, the others count from it. */
		if ((flags & ENCODE_STROKE_TIME) != 0U) {
			if (index == 0U)
				*first_time = read_varint(reader);
			else
				time += read_varint(reader);
		}

		/* A sample past the data, or out of range, is damaged. */
		if (reader->error != 0)
			return EINVAL;
		if (pressure < 0 || pressure > (int64_t)NOTES_PRESSURE_MAX)
			return EINVAL;
		if (x < -(int64_t)(ENCODE_LENGTH_MAX * NOTES_UNITS_PER_POINT) || x > (int64_t)(ENCODE_LENGTH_MAX * NOTES_UNITS_PER_POINT))
			return EINVAL;
		if (y < -(int64_t)(ENCODE_LENGTH_MAX * NOTES_UNITS_PER_POINT) || y > (int64_t)(ENCODE_LENGTH_MAX * NOTES_UNITS_PER_POINT))
			return EINVAL;
		if (tilt_x < -32768 ||
		    tilt_x > 32767 ||
		    tilt_y < -32768 ||
		    tilt_y > 32767 ||
		    time > 0xffffffffU)
			return EINVAL;

		/* The sample. */
		memset(&point, 0, sizeof(point));
		point.x = (float)x / NOTES_UNITS_PER_POINT;
		point.y = (float)y / NOTES_UNITS_PER_POINT;
		point.pressure = (uint16_t)pressure;
		point.tilt_x = (int16_t)tilt_x;
		point.tilt_y = (int16_t)tilt_y;
		point.time_ms = (uint32_t)time;
		error = notes_stroke_append(stroke, &point);
		if (error != 0)
			return error;
	}

	/* Succeeded: the stroke has its samples. */
	return 0;
}

/*
 * Tells the flags a stroke is written with: its samples' tilt, their times
 * (not a fill's), and the baked values and contours its tool has
 * (ws216-p003).  A stroke without the values its tool needs is refused.
 */
static int
stroke_flags(
	const struct notes_stroke *stroke,
	unsigned *flags)
{
	unsigned columns;
	int baked;

	/* The times, except a fill's, and the tilt when the samples carry it. */
	*flags = 0U;
	if (stroke->tool != NOTES_TOOL_FILL)
		*flags |= ENCODE_STROKE_TIME;
	if (stroke->has_tilt)
		*flags |= ENCODE_STROKE_TILT;

	/* The baked factors, and the pencil's steps; a stroke not baked cannot be kept. */
	columns = notes_tool_columns(stroke->tool);
	if (columns != 0U) {
		baked = notes_stroke_baked(stroke);
		if (!baked)
			return EINVAL;
		*flags |= ENCODE_STROKE_FACTORS;
		if (stroke->tool == NOTES_TOOL_PENCIL)
			*flags |= ENCODE_STROKE_STEPS;
	}

	/* A fill's contours. */
	if (stroke->tool == NOTES_TOOL_FILL) {
		if (stroke->contours == NULL || stroke->contour_count == 0U)
			return EINVAL;
		if (stroke->contour_count > ENCODE_CONTOURS_MAX)
			return EINVAL;
		*flags |= ENCODE_STROKE_CONTOURS;
	}

	/* Succeeded: the flags. */
	return 0;
}

/*
 * Writes what follows a stroke's flags: the sample count, a fill's
 * contours, the samples, then the baked factors and steps.
 */
static void
encode_stroke_tail(
	struct notes_buffer *buffer,
	const struct notes_stroke *stroke,
	unsigned flags,
	uint64_t first_time)
{
	size_t index;
	size_t count;
	int timed;

	/* The sample count. */
	notes_buffer_varint(buffer, stroke->point_count);

	/* A fill's contours: their count, then each one's samples. */
	if ((flags & ENCODE_STROKE_CONTOURS) != 0U) {
		notes_buffer_u16(buffer, (unsigned)stroke->contour_count);
		for (index = 0; index < stroke->contour_count; index++)
			notes_buffer_u32(buffer, (uint32_t)stroke->contours[index]);
	}

	/* The samples, timed unless they are a fill's. */
	timed = 0;
	if ((flags & ENCODE_STROKE_TIME) != 0U)
		timed = 1;
	encode_samples(buffer, stroke, first_time, timed);

	/* The baked factors: the column count, then each sample's. */
	if ((flags & ENCODE_STROKE_FACTORS) != 0U) {
		notes_buffer_u8(buffer, stroke->factor_columns);
		count = stroke->point_count * stroke->factor_columns;
		for (index = 0; index < count; index++)
			notes_buffer_u16(buffer, stroke->factors[index]);
	}

	/* The pencil's steps, one a sample. */
	if ((flags & ENCODE_STROKE_STEPS) != 0U)
		notes_buffer_bytes(buffer, stroke->steps, stroke->point_count);
}

/*
 * Reads what follows a stroke's flags (encode_stroke_tail()).  Only a
 * version with the drawing tools (tools nonzero) reads the values; there
 * the flags must give exactly the values the stroke's tool has.
 */
static int
decode_stroke_tail(
	struct encode_reader *reader,
	struct notes_stroke *stroke,
	unsigned flags,
	int tools,
	uint64_t *first_time)
{
	uint64_t count;
	uint64_t total;
	size_t index;
	size_t values;
	unsigned expected;
	unsigned columns;
	unsigned stored;
	unsigned step;
	int error;

	/* The sample count, which must be sane. */
	count = read_varint(reader);
	if (reader->error != 0 ||
	    count == 0U ||
	    count > ENCODE_POINTS_MAX)
		return EINVAL;

	/* The values the stroke's tool has. */
	columns = notes_tool_columns(stroke->tool);
	expected = 0U;
	if (columns != 0U)
		expected |= ENCODE_STROKE_FACTORS;
	if (stroke->tool == NOTES_TOOL_PENCIL)
		expected |= ENCODE_STROKE_STEPS;
	if (stroke->tool == NOTES_TOOL_FILL)
		expected |= ENCODE_STROKE_CONTOURS;

	/* Before the drawing tools the other flags mean nothing; from them they must match the tool. */
	if (!tools) {
		flags &= ENCODE_STROKE_TILT | ENCODE_STROKE_TIME;
	} else if ((flags & (ENCODE_STROKE_FACTORS | ENCODE_STROKE_STEPS | ENCODE_STROKE_CONTOURS)) != expected) {
		return EINVAL;
	}

	/* A fill's contours: at least one, each of three samples or more, as many samples as the stroke has. */
	if ((flags & ENCODE_STROKE_CONTOURS) != 0U) {
		stroke->contour_count = read_u16(reader);
		if (reader->error != 0 || stroke->contour_count == 0U)
			return EINVAL;
		if (stroke->contour_count * 4U > reader->length - reader->offset)
			return EINVAL;

		/* Allocates the contours. */
		stroke->contours = malloc(stroke->contour_count * sizeof(stroke->contours[0]));
		if (stroke->contours == NULL)
			return ENOMEM;

		/* Each contour's samples. */
		total = 0U;
		for (index = 0; index < stroke->contour_count; index++) {
			stroke->contours[index] = read_u32(reader);
			if (stroke->contours[index] < 3U)
				return EINVAL;
			total += stroke->contours[index];
		}
		if (reader->error != 0 || total != count)
			return EINVAL;
	}

	/* The samples. */
	error = decode_samples(reader, stroke, flags, (size_t)count, first_time);
	if (error != 0)
		return error;

	/* The baked factors, in the tool's columns. */
	if ((flags & ENCODE_STROKE_FACTORS) != 0U) {
		stored = read_u8(reader);
		if (stored != columns)
			return EINVAL;
		values = (size_t)count * columns;
		if (reader->error != 0 || values * 2U > reader->length - reader->offset)
			return EINVAL;

		/* Allocates them. */
		stroke->factors = malloc(values * sizeof(stroke->factors[0]));
		if (stroke->factors == NULL)
			return ENOMEM;
		stroke->factor_columns = columns;

		/* Each factor. */
		for (index = 0; index < values; index++)
			stroke->factors[index] = (uint16_t)read_u16(reader);
	}

	/* The pencil's steps, each within the steps the pencil has. */
	if ((flags & ENCODE_STROKE_STEPS) != 0U) {
		if ((size_t)count > reader->length - reader->offset)
			return EINVAL;

		/* Allocates them. */
		stroke->steps = malloc((size_t)count);
		if (stroke->steps == NULL)
			return ENOMEM;

		/* Each step. */
		for (index = 0; index < (size_t)count; index++) {
			step = read_u8(reader);
			if (step > NOTES_PENCIL_STEPS)
				return EINVAL;
			stroke->steps[index] = (unsigned char)step;
		}
	}

	/* Values past the data are damaged. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the stroke has its samples and values; its shapes follow them. */
	notes_shape_drop(stroke);
	return 0;
}

/*
 * Tells whether a version reads a tool kind with constants of a version:
 * before the drawing tools (tools zero) only the kinds without constants,
 * from them the kinds up to the fill with the constants this Notes draws.
 * Returns 0, EINVAL, or ENOTSUP for constants of another version.
 */
static int
check_kind(
	unsigned kind,
	unsigned constants,
	int tools)
{
	unsigned known;

	/* The constants this Notes draws the kind with. */
	known = notes_tool_constants(kind);

	/* Before the drawing tools, their kinds are not there (and other numbers are read as before). */
	if (!tools) {
		if (known != 0U)
			return EINVAL;
		return 0;
	}

	/* From them, no kind past the fill. */
	if (kind > NOTES_TOOL_FILL)
		return EINVAL;

	/* Constants of another version would be drawn otherwise than they were. */
	if (constants != known)
		return ENOTSUP;

	/* Succeeded: the kind is read. */
	return 0;
}

/* Reads the DOC chunk: the page count, the pressure's range, the time base, the next number. */
static int
decode_doc(
	struct encode_reader *reader,
	struct notes_document *document,
	size_t *pages)
{
	uint64_t count;
	uint64_t next;
	unsigned pressure_max;

	/* The fields. */
	count = read_varint(reader);
	pressure_max = read_u16(reader);
	document->time_base = read_u64(reader);
	next = read_varint(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A count out of range, another pressure range, or a number past 32 bits is not this version's. */
	if (count == 0U || count > ENCODE_PAGES_MAX)
		return EINVAL;
	if (pressure_max != NOTES_PRESSURE_MAX ||
	    next == 0U ||
	    next > 0xffffffffU)
		return EINVAL;

	/* Succeeded: the document knows how many pages follow. */
	document->next_id = (uint32_t)next;
	*pages = (size_t)count;
	return 0;
}

/* Reads the TOOL chunk. */
static int
decode_tools(
	struct encode_reader *reader,
	struct encode_tools *tools,
	unsigned major)
{
	uint64_t count;
	size_t index;
	uint32_t color;
	unsigned constants;
	int error;

	/* The count, which must be sane. */
	count = read_varint(reader);
	if (reader->error != 0 || count > ENCODE_TOOLS_MAX)
		return EINVAL;
	if (count == 0U)
		return 0;

	/* The list. */
	tools->tools = calloc((size_t)count, sizeof(tools->tools[0]));
	if (tools->tools == NULL)
		return ENOMEM;
	tools->capacity = (size_t)count;

	/*
	 * Each tool: kind, colour, width, curve and flags (the last two have one
	 * value in versions 1 and 2; from 3.0 the curve byte is the version of
	 * the kind's constants, ws216-p003).
	 */
	for (index = 0; index < (size_t)count; index++) {
		tools->tools[index].kind = read_u8(reader);
		color = (uint32_t)read_u8(reader) << 24;
		color |= (uint32_t)read_u8(reader) << 16;
		color |= (uint32_t)read_u8(reader) << 8;
		color |= (uint32_t)read_u8(reader);
		tools->tools[index].color = color;
		tools->tools[index].width = read_length(reader);
		constants = read_u8(reader);
		(void)read_u8(reader);
		if (reader->error != 0)
			return EINVAL;

		/* Before 3.0 the curve byte is not read: the kind has the constants of none. */
		if (major < ENCODE_MAJOR_TOOLS)
			constants = notes_tool_constants(tools->tools[index].kind);
		tools->tools[index].constants = constants;

		/* A kind the version does not have, or constants this file does not know, are refused. */
		error = check_kind(tools->tools[index].kind, constants, major >= ENCODE_MAJOR_TOOLS);
		if (error != 0)
			return error;
	}

	/* Succeeded: the strokes can name their tools. */
	tools->count = (size_t)count;
	return 0;
}

/* Reads one PAGE chunk and adds the page to the document. */
static int
decode_page(
	struct encode_reader *reader,
	struct notes_document *document,
	const struct encode_tools *tools,
	size_t index,
	unsigned major)
{
	struct notes_page *page;
	struct notes_page **larger;
	struct notes_stroke **grown;
	struct notes_stroke *stroke;
	unsigned char digest[32];
	uint64_t number;
	uint64_t strokes;
	uint64_t id;
	uint64_t tool;
	uint64_t first_time;
	float width;
	float height;
	unsigned background;
	unsigned flags;
	size_t stroke_index;
	int error;

	/* The page's number, which must be the next, its size, background and digest. */
	number = read_varint(reader);
	width = read_length(reader);
	height = read_length(reader);
	background = read_u8(reader);
	memset(digest, 0, sizeof(digest));
	if (reader->length - reader->offset < sizeof(digest)) {
		reader->error = 1;
	} else {
		memcpy(digest, reader->data + reader->offset, sizeof(digest));
		reader->offset += sizeof(digest);
	}

	/* How many strokes follow, which must be read in full. */
	strokes = read_varint(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The page must be the next one, have a size, and hold no more strokes than the data could. */
	if (number != index ||
	    !(width > 0.0f) ||
	    !(height > 0.0f))
		return EINVAL;
	if (strokes > reader->length)
		return EINVAL;

	/* The page, with the digest its content had when it was saved. */
	page = notes_page_create(width, height, background);
	if (page == NULL)
		return ENOMEM;
	memcpy(page->content_hash, digest, sizeof(digest));

	/* Room for it in the document. */
	larger = realloc(document->pages, (document->page_count + 1U) * sizeof(document->pages[0]));
	if (larger == NULL) {
		notes_page_free(page);
		return ENOMEM;
	}

	/* The page is the document's last. */
	document->pages = larger;
	document->page_capacity = document->page_count + 1U;
	document->pages[document->page_count] = page;
	document->page_count++;

	/* Each stroke; the page owns each as it is read. */
	for (stroke_index = 0; stroke_index < strokes; stroke_index++) {
		/* The number, the tool and the flags. */
		id = read_varint(reader);
		tool = read_varint(reader);
		flags = read_u8(reader);
		if (reader->error != 0 ||
		    tool >= tools->count ||
		    id > 0xffffffffU)
			return EINVAL;

		/* The stroke with its tool and the tool's constants. */
		stroke = notes_stroke_create((uint32_t)id, tools->tools[tool].kind, tools->tools[tool].color, tools->tools[tool].width, 0U);
		if (stroke == NULL)
			return ENOMEM;
		stroke->constants = tools->tools[tool].constants;

		/* The page's array grows to hold it (the count is bounded, so the size cannot overflow). */
		if (page->stroke_count == page->stroke_capacity) {
			grown = realloc(page->strokes, (page->stroke_capacity * 2U + 16U) * sizeof(page->strokes[0]));
			if (grown == NULL) {
				notes_stroke_free(stroke);
				return ENOMEM;
			}

			/* The larger array. */
			page->strokes = grown;
			page->stroke_capacity = page->stroke_capacity * 2U + 16U;
		}

		/* The page owns the stroke from now on. */
		page->strokes[page->stroke_count] = stroke;
		page->stroke_count++;

		/* Its samples (and values), and its start from the time base. */
		error = decode_stroke_tail(reader, stroke, flags, major >= ENCODE_MAJOR_TOOLS, &first_time);
		if (error != 0)
			return error;
		stroke->start_ms = document->time_base + first_time;
	}

	/* Succeeded: the page is the document's last. */
	return 0;
}

/* Reads the BASE chunk: the length and the SHA-256 of the PDF the notebook writes on. */
static int
decode_base(
	struct encode_reader *reader,
	struct notes_document *document)
{
	uint64_t size;

	/* The length, which must not be zero, and the digest. */
	size = read_u64(reader);
	if (reader->error != 0 || size == 0U)
		return EINVAL;
	if (reader->length - reader->offset < sizeof(document->base_hash))
		return EINVAL;
	memcpy(document->base_hash, reader->data + reader->offset, sizeof(document->base_hash));
	reader->offset += sizeof(document->base_hash);

	/* Succeeded: the document knows its base; the file gives the base itself. */
	document->base_size = size;
	return 0;
}

/* Reads one SRC chunk: which page of the base a page is, and how it is written back. */
static int
decode_source(
	struct encode_reader *reader,
	struct notes_document *document)
{
	struct notes_page *page;
	uint64_t number;
	uint64_t source;
	unsigned origin;

	/* The page's number, its origin and its source. */
	number = read_varint(reader);
	origin = read_u8(reader);
	source = read_varint(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The page must have been read, the origin must be a base page's, and the source a page number. */
	if (number >= document->page_count)
		return EINVAL;
	if (origin != NOTES_ORIGIN_OVER && origin != NOTES_ORIGIN_REPLACE)
		return EINVAL;
	if (source > ENCODE_PAGES_MAX)
		return EINVAL;

	/* Succeeded: the page stands for the base's page. */
	page = document->pages[number];
	page->origin = origin;
	page->source = (size_t)source;
	return 0;
}

/* Adds an image to a list once (by its number).  Returns 0 or ENOMEM. */
static int
images_add(
	struct encode_images *images,
	struct notes_image *image)
{
	struct notes_image **grown;
	size_t capacity;
	struct notes_image *found;

	/* An image already there. */
	found = images_find(images, image->id);
	if (found != NULL)
		return 0;

	/* Room for one more. */
	if (images->count == images->capacity) {
		capacity = images->capacity * 2U + 8U;
		grown = realloc(images->images, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		images->images = grown;
		images->capacity = capacity;
	}

	/* Succeeded: the image in the list. */
	images->images[images->count] = image;
	images->count++;
	return 0;
}

/* Finds an image of a list by its number; NULL when it has none. */
static struct notes_image *
images_find(
	const struct encode_images *images,
	uint32_t id)
{
	size_t at;

	/* Each image. */
	for (at = 0; at < images->count; at++) {
		if (images->images[at]->id == id)
			return images->images[at];
	}

	/* None. */
	return NULL;
}

/* Frees a list, letting go of each image when the list holds them. */
static void
images_free(
	struct encode_images *images,
	int held)
{
	size_t at;

	/* The list's references. */
	for (at = 0; held && at < images->count; at++)
		notes_image_release(images->images[at]);

	/* The list. */
	free(images->images);
	memset(images, 0, sizeof(*images));
}

/* Encodes an image's description (the IMAG chunk's body and the journal's): its number, form, size, components and orientation. */
void
notes_encode_image(
	struct notes_buffer *chunk,
	const struct notes_image *image)
{
	/* Its fields, in order. */
	notes_buffer_varint(chunk, image->id);
	notes_buffer_u8(chunk, image->kind);
	notes_buffer_varint(chunk, image->width);
	notes_buffer_varint(chunk, image->height);
	notes_buffer_u8(chunk, (unsigned)image->components);
	notes_buffer_u8(chunk, (unsigned)image->orientation);
}

/*
 * Encodes one EDIT chunk's body: the page's number, its edits' count, and
 * each edit (notes_encode_edit).
 */
static void
encode_edits(
	struct notes_buffer *chunk,
	const struct notes_page *page,
	size_t index)
{
	size_t at;

	/* The page and the count, then each edit. */
	notes_buffer_varint(chunk, index);
	notes_buffer_varint(chunk, page->edit_count);
	for (at = 0; at < page->edit_count; at++)
		notes_encode_edit(chunk, page->edits[at]);
}

/*
 * Encodes one edit (the EDIT chunk's and the journal's): its flags, its
 * object (an inserted one's number, or a page's own key: its kind, offset,
 * length and fingerprint), its transform (a placed or inserted one's: four
 * numbers in 1/65536, two in 1/64 point) and its image's number (an edit
 * with an image).
 */
void
notes_encode_edit(
	struct notes_buffer *buffer,
	const struct notes_edit *edit)
{
	size_t length;
	size_t item;

	/* Its flags and its object. */
	notes_buffer_u8(buffer, edit->flags);
	if ((edit->flags & NOTES_EDIT_INSERTED) != 0U) {
		notes_buffer_varint(buffer, edit->id);
	} else {
		notes_buffer_u8(buffer, (unsigned)edit->key.kind);
		notes_buffer_varint(buffer, edit->key.offset);
		notes_buffer_varint(buffer, edit->key.length);
		notes_buffer_bytes(buffer, edit->key.fingerprint, sizeof(edit->key.fingerprint));
	}

	/* Its transform, when it has one. */
	if ((edit->flags & (NOTES_EDIT_PLACED | NOTES_EDIT_INSERTED)) != 0U) {
		for (item = 0; item < 4U; item++)
			notes_buffer_zigzag(buffer, (int64_t)floor((double)edit->transform[item] * ENCODE_SCALE_UNITS + 0.5));
		notes_buffer_zigzag(buffer, units(edit->transform[4]));
		notes_buffer_zigzag(buffer, units(edit->transform[5]));
	}

	/* A line's new words: their length, bytes and font (ws175-p004); an inserted text's also its size, colour and width (p005). */
	if ((edit->flags & NOTES_EDIT_TEXT) != 0U) {
		length = 0;
		if (edit->text != NULL)
			length = strlen(edit->text);
		notes_buffer_varint(buffer, length);
		notes_buffer_bytes(buffer, edit->text, length);
		notes_buffer_u8(buffer, edit->font);
		if ((edit->flags & NOTES_EDIT_INSERTED) != 0U) {
			notes_buffer_varint(buffer, (uint64_t)units(edit->text_size));
			notes_buffer_u32(buffer, edit->color);
			notes_buffer_varint(buffer, (uint64_t)units(edit->box_width));
		}
	}

	/* Its image's number (0: none, as the journal names an object it takes off). */
	if ((edit->flags & (NOTES_EDIT_IMAGE | NOTES_EDIT_INSERTED)) != 0U) {
		if (edit->image != NULL)
			notes_buffer_varint(buffer, edit->image->id);
		else
			notes_buffer_varint(buffer, 0U);
	}
}

/*
 * Decodes one edit (notes_encode_edit): the edit without its image, whose
 * number (0: none; the caller refuses an edit that needs one) is given
 * apart, and how many bytes it took.  Returns 0 or EINVAL.
 */
int
notes_decode_edit(
	const unsigned char *data,
	size_t size,
	size_t *used,
	struct notes_edit *edit,
	uint32_t *image)
{
	struct encode_reader reader;
	uint64_t value;
	unsigned known;
	size_t item;
	double scale;
	const void *zero;
	int has_zero;
	int error;

	/* A reader over the bytes, an empty edit. */
	memset(&reader, 0, sizeof(reader));
	reader.data = data;
	reader.length = size;
	memset(edit, 0, sizeof(*edit));
	*image = 0U;

	/* Its flags (known ones), and its object. */
	known = NOTES_EDIT_KNOWN;
	edit->flags = read_u8(&reader);
	if (reader.error != 0 || (edit->flags & ~known) != 0U)
		return EINVAL;
	if ((edit->flags & NOTES_EDIT_INSERTED) != 0U) {
		value = read_varint(&reader);
		if (value == 0U || value > 0xffffffffU)
			return EINVAL;
		edit->id = (uint32_t)value;
	} else {
		/* A page's own object by its key. */
		edit->key.kind = (enum pdf_edit_kind)read_u8(&reader);
		edit->key.offset = read_varint(&reader);
		value = read_varint(&reader);
		if (reader.error != 0 || value > 0xffffffffU || reader.length - reader.offset < sizeof(edit->key.fingerprint))
			return EINVAL;
		edit->key.length = (uint32_t)value;
		memcpy(edit->key.fingerprint, reader.data + reader.offset, sizeof(edit->key.fingerprint));
		reader.offset += sizeof(edit->key.fingerprint);
	}

	/* Its transform (the identity when it has none). */
	edit->transform[0] = 1.0f;
	edit->transform[3] = 1.0f;
	if ((edit->flags & (NOTES_EDIT_PLACED | NOTES_EDIT_INSERTED)) != 0U) {
		for (item = 0; item < 4U; item++) {
			scale = (double)read_zigzag(&reader) / ENCODE_SCALE_UNITS;
			if (scale > ENCODE_SCALE_MAX || scale < -ENCODE_SCALE_MAX)
				return EINVAL;
			edit->transform[item] = (float)scale;
		}

		/* The offset, in 1/64 point. */
		for (item = 4U; item < 6U; item++) {
			scale = (double)read_zigzag(&reader) / (double)NOTES_UNITS_PER_POINT;
			if (scale > (double)ENCODE_LENGTH_MAX || scale < -(double)ENCODE_LENGTH_MAX)
				return EINVAL;
			edit->transform[item] = (float)scale;
		}
	}

	/* A line's new words, UTF-8 without a zero, and their font: the edit's own copy. */
	if ((edit->flags & NOTES_EDIT_TEXT) != 0U) {
		value = read_varint(&reader);
		if (reader.error != 0 || value > ENCODE_TEXT_MAX || value > reader.length - reader.offset)
			return EINVAL;
		zero = memchr(reader.data + reader.offset, 0, (size_t)value);
		has_zero = zero != NULL;
		if (has_zero)
			return EINVAL;
		edit->text = malloc((size_t)value + 1U);
		if (edit->text == NULL)
			return ENOMEM;
		memcpy(edit->text, reader.data + reader.offset, (size_t)value);
		edit->text[value] = '\0';
		reader.offset += (size_t)value;
		edit->font = read_u8(&reader);
		if ((edit->flags & NOTES_EDIT_INSERTED) != 0U) {
			edit->text_size = read_length(&reader);
			edit->color = read_u32(&reader);
			edit->box_width = read_length(&reader);
		}
	}

	/* Its image's number (0: none). */
	error = 0;
	if ((edit->flags & (NOTES_EDIT_IMAGE | NOTES_EDIT_INSERTED)) != 0U) {
		value = read_varint(&reader);
		if (value > 0xffffffffU)
			error = EINVAL;
		*image = (uint32_t)value;
	}

	/* All of it read (a failure lets the words go). */
	if (reader.error != 0)
		error = EINVAL;
	if (error != 0) {
		free(edit->text);
		edit->text = NULL;
		return error;
	}

	/* Succeeded: the bytes it took. */
	*used = reader.offset;
	return 0;
}

/*
 * Reads one IMAG chunk: an image without its bytes (they are the PDF's),
 * held by the list.  Returns 0, EINVAL, or ENOMEM.
 */
static int
decode_image(
	struct encode_reader *reader,
	struct encode_images *images)
{
	struct notes_image *image;
	struct notes_image *found;
	size_t used;
	int error;

	/* The image, a number not seen yet. */
	if (images->count >= ENCODE_IMAGES_MAX)
		return EINVAL;
	error = notes_decode_image(reader->data + reader->offset, reader->length - reader->offset, &used, &image);
	if (error != 0)
		return error;
	reader->offset += used;
	found = images_find(images, image->id);
	if (found != NULL) {
		notes_image_release(image);
		return EINVAL;
	}

	/* The list holds it. */
	error = images_add(images, image);
	if (error != 0) {
		notes_image_release(image);
		return error;
	}

	/* Succeeded: one more image. */
	return 0;
}

/*
 * Decodes an image's description (notes_encode_image) into a new image
 * without its bytes, held once by the caller, and how many bytes it took.
 * Returns 0, EINVAL, or ENOMEM.
 */
int
notes_decode_image(
	const unsigned char *data,
	size_t size,
	size_t *used,
	struct notes_image **image)
{
	struct encode_reader reader;
	struct notes_image *made;
	uint64_t id;
	uint64_t width;
	uint64_t height;
	unsigned kind;
	unsigned components;
	unsigned orientation;

	/* Its fields. */
	memset(&reader, 0, sizeof(reader));
	reader.data = data;
	reader.length = size;
	id = read_varint(&reader);
	kind = read_u8(&reader);
	width = read_varint(&reader);
	height = read_varint(&reader);
	components = read_u8(&reader);
	orientation = read_u8(&reader);
	if (reader.error != 0)
		return EINVAL;

	/* A number, a known form, a size libpdf takes, an orientation. */
	if (id == 0U || id > 0xffffffffU || kind < NOTES_IMAGE_JPEG || kind > NOTES_IMAGE_RGBA || orientation > 8U)
		return EINVAL;
	if (width == 0U || height == 0U || width > 16384U || height > 16384U || components > 4U)
		return EINVAL;

	/* The image, its bytes to come. */
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	made->id = (uint32_t)id;
	made->refs = 1U;
	made->kind = kind;
	made->width = (size_t)width;
	made->height = (size_t)height;
	made->components = (int)components;
	made->orientation = (int)orientation;

	/* Succeeded: the image and the bytes it took. */
	*image = made;
	*used = reader.offset;
	return 0;
}

/*
 * Reads one EDIT chunk: a page's edits, each put on the page.  Returns 0,
 * EINVAL, or ENOMEM.
 */
static int
decode_edits(
	struct encode_reader *reader,
	struct notes_document *document,
	const struct encode_images *images)
{
	struct notes_edit read;
	struct notes_edit *edit;
	struct notes_page *page;
	uint64_t number;
	uint64_t count;
	uint32_t image;
	size_t used;
	size_t at;
	int error;

	/* The page, which must have been read and have no edits yet, and the count. */
	number = read_varint(reader);
	count = read_varint(reader);
	if (reader->error != 0 || number >= document->page_count || count == 0U || count > ENCODE_EDITS_MAX)
		return EINVAL;
	page = document->pages[number];
	if (page->edit_count != 0U)
		return EINVAL;

	/* Each edit. */
	for (at = 0; at < (size_t)count; at++) {
		/* The edit; a page's own object only on a page drawn over the base. */
		error = notes_decode_edit(reader->data + reader->offset, reader->length - reader->offset, &used, &read, &image);
		if (error != 0)
			return error;
		reader->offset += used;
		if ((read.flags & NOTES_EDIT_INSERTED) == 0U && page->origin != NOTES_ORIGIN_OVER) {
			free(read.text);
			return EINVAL;
		}

		/* Its image, one of IMAG's, when it needs one (an image's, or an inserted one that is not a text). */
		if ((read.flags & NOTES_EDIT_IMAGE) != 0U || (read.flags & (NOTES_EDIT_INSERTED | NOTES_EDIT_TEXT)) == NOTES_EDIT_INSERTED) {
			read.image = images_find(images, image);
			if (read.image == NULL) {
				free(read.text);
				return EINVAL;
			}
		}

		/* The edit, its image held once more, its words copied, on the page. */
		edit = notes_edit_copy(&read);
		free(read.text);
		if (edit == NULL)
			return ENOMEM;
		error = notes_document_put_edit(document, (size_t)number, page->edit_count, edit);
		if (error != 0) {
			notes_edit_free(edit);
			return error;
		}
	}

	/* Succeeded: the page's edits. */
	return 0;
}

/* Reads one byte. */
static unsigned
read_u8(
	struct encode_reader *reader)
{
	/* Past the end is a failure. */
	if (reader->error != 0 || reader->offset >= reader->length) {
		reader->error = 1;
		return 0U;
	}

	/* Reports the byte and moves past it. */
	reader->offset++;
	return reader->data[reader->offset - 1U];
}

/* Reads a 16-bit number, least significant byte first. */
static unsigned
read_u16(
	struct encode_reader *reader)
{
	unsigned value;

	/* The two bytes, low first. */
	value = read_u8(reader);
	value |= read_u8(reader) << 8;

	/* Reports the number. */
	return value;
}

/* Reads a 32-bit number, least significant byte first. */
static uint32_t
read_u32(
	struct encode_reader *reader)
{
	uint32_t value;
	unsigned index;

	/* The four bytes, low first. */
	value = 0U;
	for (index = 0; index < 4U; index++)
		value |= (uint32_t)read_u8(reader) << (index * 8U);

	/* Reports the number. */
	return value;
}

/* Reads a 64-bit number, least significant byte first. */
static uint64_t
read_u64(
	struct encode_reader *reader)
{
	uint64_t value;
	unsigned index;

	/* The eight bytes, low first. */
	value = 0U;
	for (index = 0; index < 8U; index++)
		value |= (uint64_t)read_u8(reader) << (index * 8U);

	/* Reports the number. */
	return value;
}

/* Reads an unsigned LEB128 number of at most 64 bits. */
static uint64_t
read_varint(
	struct encode_reader *reader)
{
	uint64_t value;
	unsigned shift;
	unsigned byte;

	/* Seven bits a byte, low first, while the continuation bit is set. */
	value = 0U;
	for (shift = 0; shift < 64U; shift += 7U) {
		byte = read_u8(reader);
		value |= (uint64_t)(byte & 0x7fU) << shift;
		if ((byte & 0x80U) == 0U)
			return value;
	}

	/* A number longer than 64 bits is malformed. */
	reader->error = 1;
	return 0U;
}

/* Reads a zigzag LEB128 number. */
static int64_t
read_zigzag(
	struct encode_reader *reader)
{
	uint64_t folded;

	/* The even numbers are the positive values, the odd ones the negative. */
	folded = read_varint(reader);
	if ((folded & 1U) != 0U)
		return -(int64_t)(folded >> 1) - 1;

	/* Reports a positive value. */
	return (int64_t)(folded >> 1);
}

/* Reads a length in 1/64 point, and gives it in points (zero when out of range). */
static float
read_length(
	struct encode_reader *reader)
{
	uint64_t value;

	/* The number, which must stay inside the largest length. */
	value = read_varint(reader);
	if ((float)value > ENCODE_LENGTH_MAX * NOTES_UNITS_PER_POINT)
		return 0.0f;

	/* Reports it in points. */
	return (float)value / NOTES_UNITS_PER_POINT;
}

/* Converts a length in points to whole 1/64 points. */
static int64_t
units(
	float value)
{
	/* The nearest whole number. */
	return (int64_t)floor((double)value * (double)NOTES_UNITS_PER_POINT + 0.5);
}
