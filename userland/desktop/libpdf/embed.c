/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The replacement fonts a document embeds (ws175-p005, plan/ws175/
 * phase001/design.md section 4.2): each font its edited lines use, once a
 * document, as a Type0 font in Identity-H over a CIDFontType2 whose CIDs
 * are the font's glyph numbers (CIDToGIDMap /Identity), its widths (/W)
 * for the glyphs used, its descriptor (from the font's head, hhea, OS/2
 * and post), its program the subset that draws those glyphs (subset.c;
 * the whole font when its fsType forbids subsetting), named with a tag of
 * six capitals from the glyphs used, and a ToUnicode CMap (each glyph's
 * first character), the program and the CMap compressed.
 *
 * The writer reads a font's file the first time a glyph of it is used,
 * and lays the fonts out after the images (pdf_writer_font_layout).
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <pdf.h>

#include "internal.h"
#include "writer.h"

/* The most bfchar entries a CMap section holds. */
#define EMBED_CMAP_SECTION	100U

static int embed_layout(struct pdf_writer *writer, struct pdf_buffer *file, size_t *offsets);
static int embed_font(struct pdf_writer *writer, unsigned index, struct pdf_buffer *file, size_t *offsets);
static void embed_tag(const struct pdf_writer_font *font, char tag[7]);
static void embed_stream(struct pdf_buffer *file, size_t object, const unsigned char *data, size_t size, size_t plain_length, size_t *offsets);
static void embed_cmap(const struct pdf_writer_font *font, struct pdf_buffer *cmap);
static void embed_unicode(struct pdf_buffer *buffer, uint32_t character);

/*
 * Notes a glyph of a replacement font a page uses, and the character it
 * stands for (the first one noted), so that the document embeds it (the
 * font's file read the first time).  Returns 0, EINVAL, ENOENT for a font
 * that is not there, EPERM for one whose license forbids embedding, or
 * ENOMEM.
 */
int
pdf_writer_use_glyph(
	struct pdf_writer *writer,
	unsigned file,
	unsigned glyph,
	uint32_t character)
{
	struct pdf_writer_font *font;
	int allowed;
	int whole;
	int error;

	/* A font of the list, a glyph a font can have. */
	if (writer == NULL || file >= PDF_WRITER_FONT_FILES || glyph >= PDF_WRITER_FONT_GLYPHS)
		return EINVAL;
	font = &writer->fonts[file];

	/* The font's file the first time, which must allow embedding. */
	if (font->data == NULL) {
		error = pdf_edit_font_read(file, &font->data, &font->size);
		if (error != 0)
			return error;
		error = pdf_truetype_permission(font->data, font->size, &allowed, &whole);
		if (error != 0 || !allowed) {
			free(font->data);
			font->data = NULL;
			return EPERM;
		}
	}

	/* The glyphs' marks and characters, made the first time. */
	if (font->used == NULL) {
		font->used = calloc(PDF_WRITER_FONT_GLYPHS, 1U);
		font->unicode = calloc(PDF_WRITER_FONT_GLYPHS, sizeof(*font->unicode));
		if (font->used == NULL || font->unicode == NULL) {
			free(font->used);
			free(font->unicode);
			font->used = NULL;
			font->unicode = NULL;
			return ENOMEM;
		}
	}

	/* Succeeded: the glyph is the document's, the fonts laid out with it. */
	font->used[glyph] = 1U;
	if (font->unicode[glyph] == 0U)
		font->unicode[glyph] = character;
	font->any = 1;
	writer->font_layout = embed_layout;
	return 0;
}

/* Writes each font used: its five objects, numbered when the document was laid out. */
static int
embed_layout(
	struct pdf_writer *writer,
	struct pdf_buffer *file,
	size_t *offsets)
{
	unsigned index;
	int error;

	/* Each font used. */
	for (index = 0; index < PDF_WRITER_FONT_FILES; index++) {
		if (!writer->fonts[index].any)
			continue;
		error = embed_font(writer, index, file, offsets);
		if (error != 0)
			return error;
	}

	/* Succeeded: the fonts are written. */
	return 0;
}

/*
 * Writes one font: the Type0 font, its CIDFont with the used glyphs'
 * widths, the descriptor, the program and the ToUnicode CMap.  Returns 0,
 * PDF_EFORMAT, or ENOMEM.
 */
static int
embed_font(
	struct pdf_writer *writer,
	unsigned index,
	struct pdf_buffer *file,
	size_t *offsets)
{
	struct pdf_truetype_metrics metrics;
	struct pdf_writer_font *font;
	struct pdf_buffer cmap;
	unsigned char *program;
	unsigned char *packed;
	size_t program_size;
	size_t packed_size;
	size_t object;
	unsigned glyph;
	unsigned advance;
	double scale;
	char tag[7];
	const char *name;
	int allowed;
	int whole;
	int flags;
	int error;

	/* The font, its measures and its tag. */
	font = &writer->fonts[index];
	object = font->object;
	error = pdf_truetype_metrics(font->data, font->size, &metrics);
	if (error == 0)
		error = pdf_truetype_permission(font->data, font->size, &allowed, &whole);
	if (error != 0)
		return error;
	scale = 1000.0 / (double)metrics.units_per_em;
	embed_tag(font, tag);
	name = pdf_edit_font_name(index);

	/* The Type0 font. */
	offsets[object] = file->length;
	pdf_buffer_printf(file, "%lu 0 obj\n<< /Type /Font /Subtype /Type0 /BaseFont /%s+%s /Encoding /Identity-H /DescendantFonts [%lu 0 R] /ToUnicode %lu 0 R >>\nendobj\n",
			  (unsigned long)object, tag, name, (unsigned long)(object + 1U), (unsigned long)(object + 4U));

	/* The CIDFont: Identity, the default width, each used glyph's width. */
	offsets[object + 1U] = file->length;
	pdf_buffer_printf(file, "%lu 0 obj\n<< /Type /Font /Subtype /CIDFontType2 /BaseFont /%s+%s /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >>"
			  " /FontDescriptor %lu 0 R /CIDToGIDMap /Identity /DW 1000 /W [",
			  (unsigned long)(object + 1U), tag, name, (unsigned long)(object + 2U));
	for (glyph = 0; glyph < metrics.glyphs && glyph < PDF_WRITER_FONT_GLYPHS; glyph++) {
		if (font->used[glyph] == 0U)
			continue;
		error = pdf_truetype_advance(font->data, font->size, glyph, &advance);
		if (error != 0)
			return error;
		pdf_buffer_printf(file, " %u [", glyph);
		pdf_buffer_append_number(file, (double)advance * scale);
		pdf_buffer_printf(file, "]");
	}

	/* The widths end, and the CIDFont. */
	pdf_buffer_printf(file, " ] >>\nendobj\n");

	/* The descriptor, from the font's measures. */
	offsets[object + 2U] = file->length;
	flags = 32;
	if (metrics.fixed)
		flags |= 1;
	pdf_buffer_printf(file, "%lu 0 obj\n<< /Type /FontDescriptor /FontName /%s+%s /Flags %d /FontBBox [", (unsigned long)(object + 2U), tag, name, flags);
	pdf_buffer_append_number(file, (double)metrics.box[0] * scale);
	pdf_buffer_append(file, " ", 1);
	pdf_buffer_append_number(file, (double)metrics.box[1] * scale);
	pdf_buffer_append(file, " ", 1);
	pdf_buffer_append_number(file, (double)metrics.box[2] * scale);
	pdf_buffer_append(file, " ", 1);
	pdf_buffer_append_number(file, (double)metrics.box[3] * scale);
	pdf_buffer_printf(file, "] /ItalicAngle 0 /Ascent ");
	pdf_buffer_append_number(file, (double)metrics.ascent * scale);
	pdf_buffer_printf(file, " /Descent ");
	pdf_buffer_append_number(file, (double)metrics.descent * scale);
	pdf_buffer_printf(file, " /CapHeight ");
	pdf_buffer_append_number(file, (double)metrics.cap_height * scale);
	pdf_buffer_printf(file, " /StemV 80 /FontFile2 %lu 0 R >>\nendobj\n", (unsigned long)(object + 3U));

	/* The program: the subset (the whole font when it may not be subset), compressed. */
	program = font->data;
	program_size = font->size;
	if (!whole) {
		error = pdf_truetype_subset(font->data, font->size, font->used, PDF_WRITER_FONT_GLYPHS, &program, &program_size);
		if (error != 0)
			return error;
	}

	/* Compressed when that is shorter. */
	error = pdf_writer_pack(program, program_size, &packed, &packed_size);
	if (error == 0 && packed != NULL)
		embed_stream(file, object + 3U, packed, packed_size, program_size, offsets);
	if (error == 0 && packed == NULL)
		embed_stream(file, object + 3U, program, program_size, 0, offsets);
	free(packed);
	if (program != font->data)
		free(program);
	if (error != 0)
		return error;

	/* The ToUnicode CMap, compressed. */
	memset(&cmap, 0, sizeof(cmap));
	embed_cmap(font, &cmap);
	if (cmap.error != 0) {
		free(cmap.data);
		return cmap.error;
	}

	/* Compressed when that is shorter. */
	error = pdf_writer_pack(cmap.data, cmap.length, &packed, &packed_size);
	if (error == 0 && packed != NULL)
		embed_stream(file, object + 4U, packed, packed_size, cmap.length, offsets);
	if (error == 0 && packed == NULL)
		embed_stream(file, object + 4U, cmap.data, cmap.length, 0, offsets);
	free(packed);
	free(cmap.data);
	return error;
}

/*
 * Writes a stream object: compressed (plain_length its bytes before, for a
 * font program's /Length1) or as it is (plain_length 0).
 */
static void
embed_stream(
	struct pdf_buffer *file,
	size_t object,
	const unsigned char *data,
	size_t size,
	size_t plain_length,
	size_t *offsets)
{
	/* The dictionary: the filter and the plain length when compressed, the length. */
	offsets[object] = file->length;
	pdf_buffer_printf(file, "%lu 0 obj\n<<", (unsigned long)object);
	if (plain_length != 0)
		pdf_buffer_printf(file, " /Filter /FlateDecode /Length1 %lu", (unsigned long)plain_length);
	pdf_buffer_printf(file, " /Length %lu >>\nstream\n", (unsigned long)size);

	/* The bytes. */
	pdf_buffer_append(file, data, size);
	pdf_buffer_printf(file, "\nendstream\nendobj\n");
}

/* Makes a subset's tag: six capitals from a hash of the glyphs used (the same glyphs, the same tag). */
static void
embed_tag(
	const struct pdf_writer_font *font,
	char tag[7])
{
	uint32_t hash;
	unsigned glyph;
	unsigned at;

	/* FNV-1a over the used glyphs' numbers. */
	hash = 0x811c9dc5UL;
	for (glyph = 0; glyph < PDF_WRITER_FONT_GLYPHS; glyph++) {
		if (font->used[glyph] == 0U)
			continue;
		hash = ((hash ^ (glyph & 0xffU)) * 0x01000193UL) & 0xffffffffUL;
		hash = ((hash ^ (glyph >> 8)) * 0x01000193UL) & 0xffffffffUL;
	}

	/* Six letters from it. */
	for (at = 0; at < 6U; at++) {
		tag[at] = (char)('A' + hash % 26U);
		hash /= 26U;
	}

	/* The end of the tag. */
	tag[6] = '\0';
}

/* Writes a ToUnicode CMap: each used glyph (its CID) to its character, in sections of EMBED_CMAP_SECTION. */
static void
embed_cmap(
	const struct pdf_writer_font *font,
	struct pdf_buffer *cmap)
{
	unsigned glyph;
	unsigned section;
	unsigned count;
	unsigned next;

	/* The header and the code space of two bytes. */
	pdf_buffer_printf(cmap, "/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
			  "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
			  "/CMapName /Adobe-Identity-UCS def /CMapType 2 def\n"
			  "1 begincodespacerange <0000> <FFFF> endcodespacerange\n");

	/* The glyphs with a character, a section at a time. */
	glyph = 0;
	while (glyph < PDF_WRITER_FONT_GLYPHS) {
		/* How many go in this section. */
		count = 0;
		for (next = glyph; next < PDF_WRITER_FONT_GLYPHS && count < EMBED_CMAP_SECTION; next++) {
			if (font->used[next] != 0U && font->unicode[next] != 0U)
				count++;
		}

		/* None left. */
		if (count == 0)
			break;

		/* The section. */
		pdf_buffer_printf(cmap, "%u beginbfchar\n", count);
		section = 0;
		while (section < count) {
			if (font->used[glyph] != 0U && font->unicode[glyph] != 0U) {
				pdf_buffer_printf(cmap, "<%04X> <", glyph);
				embed_unicode(cmap, font->unicode[glyph]);
				pdf_buffer_printf(cmap, ">\n");
				section++;
			}

			/* The next glyph. */
			glyph++;
		}

		/* The section's end. */
		pdf_buffer_printf(cmap, "endbfchar\n");
	}

	/* The end. */
	pdf_buffer_printf(cmap, "endcmap CMapName currentdict /CMap defineresource pop end end\n");
}

/* Writes a character in UTF-16BE hexadecimal (a surrogate pair past U+FFFF). */
static void
embed_unicode(
	struct pdf_buffer *buffer,
	uint32_t character)
{
	uint32_t value;

	/* One unit. */
	if (character < 0x10000UL) {
		pdf_buffer_printf(buffer, "%04lX", (unsigned long)character);
		return;
	}

	/* Two. */
	value = character - 0x10000UL;
	pdf_buffer_printf(buffer, "%04lX%04lX", (unsigned long)(0xd800UL + (value >> 10)), (unsigned long)(0xdc00UL + (value & 0x3ffUL)));
}
