/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fonts of libpdf's reader (stage 2 of design-pdf.md): which glyph a
 * character code of a shown string draws, how far it moves the text
 * position, and the glyph's outline.
 *
 * An embedded TrueType program (/FontFile2, or an OpenType /FontFile3
 * with TrueType outlines) is read through libtruetype: a simple font's
 * codes reach its glyphs through the font's encoding (StandardEncoding,
 * WinAnsiEncoding or MacRomanEncoding and /Differences) and its character
 * maps, a composite font's (Type0 with a CIDFontType2 and the Identity-H
 * or Identity-V encoding) through /CIDToGIDMap.  From stage 3 an embedded
 * Type 1 program (/FontFile, type1.c) or CFF program (/FontFile3 of
 * subtype Type1C, CIDFontType0C or OpenType, cff.c) is read too: a simple
 * font's codes reach its glyphs by the names /Differences gives, the
 * base encoding's characters matched to the glyphs' names, or the
 * program's own encoding; a CIDFontType0's CIDs through the program's
 * charset.  A font without an embedded program, the standard 14 among
 * them, is drawn with a system font of the same kind (sans, serif or
 * monospace, bold and italic by its name and flags) reached through the
 * encoding's Unicode values, while the positions still come from the
 * font's /Widths, so that the layout stays the document's; so is a
 * program the reader cannot read.  A Type 3 font's glyphs are content streams
 * (/CharProcs, by the names its /Differences gives) that the content
 * interpreter runs through pdf_font_type3_glyph(); a composite font the
 * reader cannot map only moves the text position.  A font drawn other than
 * as the document means marks the display list PDF_DISPLAY_SKIPPED.
 *
 * Each document keeps its fonts, the substitute font files it read, and
 * per font the outlines of the glyphs drawn so far, until it is closed.
 * The fonts are not trusted: every count read from them is bounded.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pdf.h>
#include <truetype/truetype.h>

#include "internal.h"

/*
 * Where the substitute fonts are.
 *
 * The desktop's fonts are installed here; a build for another system names
 * its own directory.
 */
#ifndef PDF_FONT_DIRECTORY
#define PDF_FONT_DIRECTORY "/usr/share/fonts"
#endif

/*
 * Whether substitute font files are read at all: 0 in a program that may
 * open no file (keiland-preview in its sandbox, ws168-p003), which then
 * draws no font the document does not embed.
 */
#ifndef PDF_FONT_FILES
#define PDF_FONT_FILES 1
#endif

/*
 * The file beside the substitutes that draws the letters and signs they
 * lack (ws090-p020): the desktop's monospaced fallback.
 */
#define PDF_FONT_COMPANION "keiland-fallback-mono.ttf"

/* The most substitute fonts a program gives the library in memory (pdf_font_memory_add). */
#define FONT_MEMORY_MAX 8

/* The most fonts one document keeps, and the largest font file read. */
#define FONT_COUNT_MAX 4096
#define FONT_FILE_MAX ((size_t)64 * 1024 * 1024)

/* The most path points one font keeps for its drawn glyphs before it starts over. */
#define FONT_CACHE_POINTS_MAX ((size_t)2 * 1024 * 1024)

/* The highest CID a composite font's widths and glyph map are kept for. */
#define FONT_CID_MAX 65535UL

/* The most entries of a /W array and of a /Differences array that are read. */
#define FONT_ARRAY_MAX ((size_t)1048576)

/*
 * A substitute that lacks a Latin ligature (U+FB00 to U+FB04) draws its
 * letters instead, under a glyph number past any a face has: this base
 * plus the ligature's character.
 */
#define FONT_LIGATURE_GLYPH 0x10000U

/* The descriptor flags: fixed pitch, serif, symbolic, italic, force bold. */
#define FONT_FLAG_FIXED 0x1L
#define FONT_FLAG_SERIF 0x2L
#define FONT_FLAG_SYMBOLIC 0x4L
#define FONT_FLAG_ITALIC 0x40L
#define FONT_FLAG_FORCE_BOLD 0x40000L

/* How far a substitute's italic leans, and how thick a substitute's bold stroke is, in ems. */
#define FONT_ITALIC_SHEAR 0.2
#define FONT_BOLD_WIDTH 0.03

/*
 * The kinds of substitute a font asks for.
 */
enum font_family {
	FONT_FAMILY_SANS = 0,
	FONT_FAMILY_SERIF,
	FONT_FAMILY_MONO
};

/*
 * The kinds of font the reader tells apart.
 */
enum font_kind {
	FONT_KIND_SIMPLE = 0,
	FONT_KIND_COMPOSITE,
	FONT_KIND_TYPE3
};

/*
 * One system font file read as a substitute, shared by every font of the
 * document that asks for it.
 *
 * data is the file's bytes and face reads them; both live until the
 * document is closed.  A substitute the program gave in memory
 * (pdf_font_memory_add) is read from memory instead, which stays the
 * program's (data is then NULL).  A file that could not be read is kept
 * with a NULL face, so that it is not tried again.
 */
struct font_file {
	struct font_file *next;
	char name[32];
	unsigned char *data;
	const unsigned char *memory;
	size_t size;
	struct truetype_face *face;
};

/*
 * A substitute font a program holds in memory, under the name of the file
 * it stands for (ws177-p010).
 */
struct font_memory {
	char name[32];
	const unsigned char *data;
	size_t size;
};

/*
 * Where one glyph's outline lies in its font's shared path arrays.
 */
struct glyph_slot {
	unsigned glyph;
	int used;
	double advance;
	size_t verb_start;
	size_t verb_count;
	size_t point_start;
	size_t point_count;
};

/*
 * One font of a document, as the text operators use it.
 *
 * face draws the glyphs of a TrueType program or a substitute, charstrings
 * those of a Type 1 or CFF program (both NULL when the font's glyphs cannot
 * be drawn); program holds the decoded bytes either reads.  A simple font maps each byte code to
 * a glyph and a width; a composite font maps each two-byte CID through its
 * CID-to-glyph map and its widths.  Widths are in text space (thousandths
 * of the /Widths turned into ems); a negative width means the font gave
 * none and the face's advance is used.  The glyph slots and the path arrays
 * keep the outlines drawn so far, in ems with y upward.  status is what the
 * display list is marked with when the font draws.
 */
struct pdf_font {
	struct pdf_font *next;
	const struct pdf_object *dictionary;
	enum font_kind kind;
	unsigned status;
	int vertical;
	struct truetype_face *face;
	struct truetype_face *owned_face;
	struct pdf_charstrings *charstrings;
	unsigned char *program;
	double units_per_em;
	double shear;
	double bold;
	int substituted;
	unsigned code_glyphs[256];
	double code_widths[256];
	double type3_scale;
	double type3_matrix[6];
	struct pdf_object *type3_procedures;
	struct pdf_object *type3_resources;
	const unsigned char *type3_names[256];
	size_t type3_lengths[256];
	float *cid_widths;
	size_t cid_widths_count;
	double default_width;
	double vertical_origin;
	double vertical_advance;
	const unsigned char *cid_map;
	size_t cid_map_size;
	unsigned char *cid_map_owned;
	struct glyph_slot *slots;
	size_t slots_capacity;
	size_t slots_count;
	unsigned char *verbs;
	size_t verbs_count;
	size_t verbs_capacity;
	struct pdf_point *points;
	size_t points_count;
	size_t points_capacity;
	struct truetype_outline_point *outline_points;
	unsigned outline_points_capacity;
	unsigned *outline_ends;
	unsigned outline_ends_capacity;
	unsigned short code_unicode[256];
	int has_code_unicode;
	struct pdf_tounicode *tounicode;
	int tounicode_read;
	uint32_t *glyph_unicode;
	size_t glyph_unicode_count;
	int glyph_unicode_built;
};

/*
 * A document's fonts and the substitute files they read.
 */
struct pdf_font_cache {
	struct pdf_font *fonts;
	size_t fonts_count;
	struct font_file *files;
};

/*
 * The substitute fonts the program gave in memory, in the order given,
 * and how many.  The program fills the table once, before it opens a
 * document (keiland-preview, which opens no file, ws177-p010); the
 * documents only read it.  The bytes are the program's and outlive every
 * document.
 */
static struct font_memory font_memory[FONT_MEMORY_MAX];
static size_t font_memory_count;

static int load_font(struct pdf_document *document, struct pdf_font_cache *cache, struct pdf_object *dictionary, struct pdf_font *font);
static int load_simple(struct pdf_document *document, struct pdf_font_cache *cache, struct pdf_object *dictionary, struct pdf_font *font);
static int load_composite(struct pdf_document *document, struct pdf_object *dictionary, struct pdf_font *font);
static void load_type3(struct pdf_document *document, struct pdf_object *dictionary, struct pdf_font *font);
static int load_program(struct pdf_document *document, struct pdf_object *descriptor, struct pdf_font *font, int *foreign);
static int load_truetype(struct pdf_document *document, struct pdf_object *stream, struct pdf_font *font);
static int load_font_file3(struct pdf_document *document, struct pdf_object *stream, struct pdf_font *font);
static int load_type1(struct pdf_document *document, struct pdf_object *stream, struct pdf_font *font);
static int open_truetype(const unsigned char *data, size_t size, struct pdf_font *font);
static int decode_program(struct pdf_document *document, struct pdf_object *stream, struct pdf_font *font, const unsigned char **data, size_t *size);
static int load_substitute(struct pdf_document *document, struct pdf_font_cache *cache, struct pdf_object *dictionary, struct pdf_object *descriptor, struct pdf_font *font);
static int open_substitute(struct pdf_font_cache *cache, const char *name, struct truetype_face **face);
static int read_font_file(const char *path, unsigned char **data, size_t *size);
static void read_encoding(struct pdf_document *document, struct pdf_object *dictionary, int symbolic, const unsigned short *builtin, unsigned short unicode[256]);
static int read_builtin_encoding(struct pdf_document *document, struct pdf_object *descriptor, unsigned short builtin[256]);
static int tex_family(const struct pdf_object *name, enum font_family *family, int *bold, int *italic);
static void apply_differences(struct pdf_document *document, struct pdf_object *differences, unsigned short unicode[256]);
static int read_difference_names(struct pdf_document *document, struct pdf_object *dictionary, const unsigned char *names[256], size_t lengths[256]);
static void map_program_codes(struct pdf_font *font, const unsigned short unicode[256], const unsigned char *const names[256], const size_t lengths[256], int has_base);
static int emit_program_step(void *context, enum pdf_path_verb verb, const double *coordinates, size_t count);
static const unsigned short *base_encoding(const struct pdf_object *name);
static void map_embedded_codes(struct pdf_font *font, const unsigned short unicode[256], int symbolic, int has_encoding);
static void map_substitute_codes(struct pdf_font *font, const unsigned short unicode[256]);
static unsigned mac_roman_code(unsigned unicode);
static void read_simple_widths(struct pdf_document *document, struct pdf_object *dictionary, struct pdf_object *descriptor, struct pdf_font *font);
static int read_cid_widths(struct pdf_document *document, struct pdf_object *cid_font, struct pdf_font *font);
static void read_vertical_metrics(struct pdf_document *document, struct pdf_object *cid_font, struct pdf_font *font);
static int read_cid_map(struct pdf_document *document, struct pdf_object *cid_font, struct pdf_font *font);
static int font_read_tounicode(struct pdf_document *document, struct pdf_font *font);
static int font_build_glyph_unicode(struct pdf_font *font);
static long descriptor_flags(struct pdf_document *document, struct pdf_object *descriptor);
static int read_number(struct pdf_document *document, struct pdf_object *object, double *number);
static int name_contains(const struct pdf_object *name, const char *part);
static void choose_family(struct pdf_document *document, struct pdf_object *dictionary, struct pdf_object *descriptor, enum font_family *family, int *bold, int *italic);
static unsigned composite_glyph(const struct pdf_font *font, unsigned cid);
static double face_advance(struct pdf_font *font, unsigned glyph);
static int find_outline(struct pdf_font *font, unsigned glyph, struct glyph_slot **slot);
static int read_outline(struct pdf_font *font, unsigned glyph, struct glyph_slot *slot);
static int convert_contours(struct pdf_font *font, const struct truetype_glyph_outline *outline, double offset);
static int append_glyph(struct pdf_font *font, unsigned glyph, double offset, double *advance);
static const char *ligature_letters(unsigned glyph);
static int emit(struct pdf_font *font, enum pdf_path_verb verb, const double *coordinates, size_t count);
static int grow_slots(struct pdf_font *font);
static void free_font(struct pdf_font *font);
static unsigned hex_number(const unsigned char *digits, size_t count);

/*
 * Finds the font a font dictionary describes, reading it the first time a
 * document uses it.
 *
 * A font that cannot be read at all is ENOTSUP (or ENOMEM); one that can be
 * read but not drawn as the document means is returned with its status
 * saying so.
 */
int
pdf_font_get(
	struct pdf_document *document,
	struct pdf_object *dictionary,
	struct pdf_font **font)
{
	struct pdf_font_cache *cache;
	struct pdf_font *found;
	struct pdf_font *created;
	int error;

	/* Refuses anything but a font dictionary. */
	if (dictionary == NULL)
		return EINVAL;
	if (dictionary->type != PDF_OBJECT_DICTIONARY)
		return PDF_EFORMAT;

	/* Finds the document's font cache, making it the first time; the document frees it. */
	cache = pdf_reader_font_cache(document);
	if (cache == NULL) {
		cache = calloc(1, sizeof(*cache));
		if (cache == NULL)
			return ENOMEM;
		pdf_reader_set_font_cache(document, cache, pdf_font_cache_free);
	}

	/* Answers from a font the document already read. */
	for (found = cache->fonts; found != NULL; found = found->next) {
		if (found->dictionary == dictionary) {
			*font = found;
			return 0;
		}
	}

	/* Refuses more fonts than a document keeps. */
	if (cache->fonts_count >= FONT_COUNT_MAX)
		return ENOMEM;

	/* Makes the font, which draws nothing until it is read. */
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->dictionary = dictionary;
	created->units_per_em = 1000.0;
	created->default_width = 1.0;
	created->type3_scale = 0.001;

	/* Reads the font from its dictionary. */
	error = load_font(document, cache, dictionary, created);
	if (error != 0) {
		free_font(created);
		return error;
	}

	/* Keeps it for the rest of the document. */
	created->next = cache->fonts;
	cache->fonts = created;
	cache->fonts_count++;

	/* Succeeded: the font is the document's. */
	*font = created;
	return 0;
}

/*
 * Frees a document's fonts and substitute files.
 */
void
pdf_font_cache_free(
	struct pdf_font_cache *cache)
{
	struct pdf_font *font;
	struct pdf_font *next_font;
	struct font_file *file;
	struct font_file *next_file;

	/* Nothing was read. */
	if (cache == NULL)
		return;

	/* Frees each font. */
	for (font = cache->fonts; font != NULL; font = next_font) {
		next_font = font->next;
		free_font(font);
	}

	/* Frees each substitute file and its face. */
	for (file = cache->files; file != NULL; file = next_file) {
		next_file = file->next;
		truetype_close(file->face);
		free(file->data);
		free(file);
	}

	/* The cache itself. */
	free(cache);
}

/*
 * Gives the library a substitute font held in memory, under the name of
 * the file it stands for ("keiland.ttf", "keiland-bold.ttf",
 * "keiland-mono.ttf"): a font a document does not embed is drawn with it
 * before any file of that name is looked for.  For a program that opens
 * no file (keiland-preview, ws177-p010).  It is called before a document
 * is opened, from one thread; the bytes stay the caller's and must live
 * while documents are open.
 *
 * Returns 0, EINVAL for an empty or too long name or no bytes, or ENOSPC
 * when FONT_MEMORY_MAX fonts were given already.
 */
int
pdf_font_memory_add(
	const char *name,
	const void *data,
	size_t size)
{
	struct font_memory *entry;
	size_t length;

	/* Refuses a font without a name or bytes. */
	if (name == NULL || data == NULL || size == 0)
		return EINVAL;

	/* Refuses a name longer than a substitute's. */
	length = strlen(name);
	if (length == 0 || length >= sizeof(font_memory[0].name))
		return EINVAL;

	/* Refuses one more than the table holds. */
	if (font_memory_count == FONT_MEMORY_MAX)
		return ENOSPC;

	/* Keeps the font under its name. */
	entry = &font_memory[font_memory_count];
	memcpy(entry->name, name, length + 1);
	entry->data = data;
	entry->size = size;
	font_memory_count++;

	/* Succeeded: the documents opened from now on draw with it. */
	return 0;
}

/*
 * Reports how a font's use marks a display list: PDF_DISPLAY_SKIPPED when
 * its glyphs are substituted or not drawn, 0 when they are its own.
 */
unsigned
pdf_font_status(
	const struct pdf_font *font)
{
	/* The status decided when the font was read. */
	return font->status;
}

/*
 * Tells whether a font is a Type 3 font, whose glyphs are procedures
 * (ws175-p002b: its text is not changed).
 */
int
pdf_font_type3(
	const struct pdf_font *font)
{
	/* The kind read. */
	if (font != NULL && font->kind == FONT_KIND_TYPE3)
		return 1;
	return 0;
}

/*
 * Gives a font's dictionary (its /BaseFont names it to the editor).
 */
const struct pdf_object *
pdf_font_dictionary(
	const struct pdf_font *font)
{
	/* The dictionary it was read from. */
	if (font == NULL)
		return NULL;
	return font->dictionary;
}

/*
 * Reports whether a font writes vertically (Identity-V).
 */
int
pdf_font_vertical(
	const struct pdf_font *font)
{
	/* Only a composite font can write vertically. */
	return font->vertical;
}

/*
 * Reads the next character code of a shown string.
 *
 * A simple font's codes are single bytes and a composite font's two bytes
 * (the Identity encodings); a last lone byte of a composite font's string
 * is a one-byte code.  Reports how many bytes the code took, and in
 * *single_byte whether it was one byte, which is what word spacing asks.
 */
size_t
pdf_font_next_code(
	const struct pdf_font *font,
	const unsigned char *bytes,
	size_t length,
	unsigned *code,
	int *single_byte)
{
	/* A simple font, a Type 3 font, and a lone last byte take one byte. */
	if (font->kind != FONT_KIND_COMPOSITE || length < 2) {
		*code = bytes[0];
		*single_byte = 1;
		return 1;
	}

	/* A composite font's code is two bytes, the high one first. */
	*code = ((unsigned)bytes[0] << 8) | bytes[1];
	*single_byte = 0;
	return 2;
}

/*
 * Finds the characters one code of a shown string stands for (ws175-p002b,
 * design.md section 3.2; PDF 1.7 section 9.10.2): the font's /ToUnicode
 * CMap, else a simple font's encoding and /Differences, else a composite
 * font's embedded TrueType program's character map read backward (its
 * CID's glyph), else U+FFFD.  single_byte is the code's length as
 * pdf_font_next_code gave it.  Up to capacity characters; count is how
 * many (one at least).  Returns 0, EINVAL, or ENOMEM.
 */
int
pdf_font_unicode(
	struct pdf_document *document,
	struct pdf_font *font,
	unsigned code,
	int single_byte,
	uint32_t *characters,
	size_t capacity,
	size_t *count)
{
	unsigned glyph;
	unsigned length;
	int error;

	/* Refuses missing results. */
	if (font == NULL || characters == NULL || count == NULL || capacity == 0)
		return EINVAL;

	/* The /ToUnicode CMap, read the first time. */
	if (!font->tounicode_read) {
		font->tounicode_read = 1;
		error = font_read_tounicode(document, font);
		if (error == ENOMEM)
			return ENOMEM;
	}

	/* The CMap's entry of the code, by its byte length. */
	length = 2U;
	if (single_byte)
		length = 1U;
	if (font->tounicode != NULL) {
		error = pdf_tounicode_lookup(font->tounicode, code, length, characters, capacity, count);
		if (error == 0 && *count > 0)
			return 0;
	}

	/* A simple font's encoding. */
	if (font->kind != FONT_KIND_COMPOSITE && font->has_code_unicode && code < 256U && font->code_unicode[code] != 0U) {
		characters[0] = font->code_unicode[code];
		*count = 1;
		return 0;
	}

	/* A composite font's TrueType program, its character map read backward. */
	if (font->kind == FONT_KIND_COMPOSITE && font->face != NULL) {
		if (!font->glyph_unicode_built) {
			font->glyph_unicode_built = 1;
			error = font_build_glyph_unicode(font);
			if (error == ENOMEM)
				return ENOMEM;
		}

		/* The CID's glyph, and the glyph's character. */
		glyph = composite_glyph(font, code);
		if (glyph < font->glyph_unicode_count && font->glyph_unicode[glyph] != 0U) {
			characters[0] = font->glyph_unicode[glyph];
			*count = 1;
			return 0;
		}
	}

	/* Nothing known. */
	characters[0] = 0xfffdU;
	*count = 1;
	return 0;
}

/*
 * Finds a code of a font that stands for one character (ws175-p004,
 * design.md section 3.4): pdf_font_unicode's sources read backward -- the
 * /ToUnicode CMap, a simple font's 256 codes, a composite font's TrueType
 * program's character map (its glyph's CID) -- the code checked forward.
 * *length is its bytes (1, or 2 for a composite font).  Returns 0, EINVAL,
 * ENOENT when the font has none, or ENOMEM.
 */
int
pdf_font_code(
	struct pdf_document *document,
	struct pdf_font *font,
	uint32_t character,
	unsigned *code,
	unsigned *length)
{
	uint32_t characters[8];
	unsigned candidate;
	unsigned bytes;
	unsigned glyph;
	unsigned drawn;
	unsigned cid;
	size_t count;
	size_t cids;
	int single_byte;
	int error;

	/* Refuses missing results; a Type 3 font's codes draw procedures. */
	if (font == NULL || code == NULL || length == NULL)
		return EINVAL;
	if (font->kind == FONT_KIND_TYPE3)
		return ENOENT;

	/* The /ToUnicode CMap, read the first time, backward. */
	if (!font->tounicode_read) {
		font->tounicode_read = 1;
		error = font_read_tounicode(document, font);
		if (error == ENOMEM)
			return ENOMEM;
	}

	/* The CMap backward (a composite font's codes are two bytes). */
	if (font->tounicode != NULL) {
		error = pdf_tounicode_reverse(font->tounicode, character, &candidate, &bytes);
		if (error == 0 && (bytes == 2U || font->kind != FONT_KIND_COMPOSITE)) {
			*code = candidate;
			*length = bytes;
			return 0;
		}
	}

	/* A simple font: the code of its 256 whose character it is. */
	single_byte = font->kind != FONT_KIND_COMPOSITE;
	if (single_byte) {
		for (candidate = 0; candidate < 256U; candidate++) {
			error = pdf_font_unicode(document, font, candidate, 1, characters, sizeof(characters) / sizeof(characters[0]), &count);
			if (error == ENOMEM)
				return ENOMEM;
			if (error == 0 && count == 1 && characters[0] == character) {
				*code = candidate;
				*length = 1U;
				return 0;
			}
		}

		/* None of the 256. */
		return ENOENT;
	}

	/* A composite font with a TrueType program: the character's glyph, then the CID that draws it. */
	if (font->face == NULL)
		return ENOENT;
	if (!font->glyph_unicode_built) {
		font->glyph_unicode_built = 1;
		error = font_build_glyph_unicode(font);
		if (error == ENOMEM)
			return ENOMEM;
	}

	/* Each glyph of the character. */
	for (glyph = 1; glyph < font->glyph_unicode_count; glyph++) {
		if (font->glyph_unicode[glyph] != character)
			continue;

		/* The identity map's CID is the glyph; a map's is the first CID that draws it. */
		cids = font->cid_map_size / 2U;
		if (font->cid_map == NULL)
			cids = 0x10000U;
		for (cid = 0; cid < cids; cid++) {
			drawn = composite_glyph(font, cid);
			if (drawn != glyph)
				continue;
			error = pdf_font_unicode(document, font, cid, 0, characters, sizeof(characters) / sizeof(characters[0]), &count);
			if (error == ENOMEM)
				return ENOMEM;
			if (error == 0 && count == 1 && characters[0] == character) {
				*code = cid;
				*length = 2U;
				return 0;
			}

			/* The identity has one CID a glyph. */
			if (font->cid_map == NULL)
				break;
		}
	}

	/* No code. */
	return ENOENT;
}

/*
 * Tells whether a font draws with its own embedded program (not a
 * substitute, design.md section 3.4: a line of a font that is not
 * embedded is written with a replacement font).
 */
int
pdf_font_embedded(
	const struct pdf_font *font)
{
	/* A program of its own, not a stand-in. */
	if (font == NULL || font->substituted)
		return 0;
	return font->face != NULL || font->charstrings != NULL;
}

/*
 * Finds what one character code draws: its displacement in text space
 * (ems of the font size), and its outline in ems with y upward when the
 * font draws it.
 *
 * The outline stays valid until the next call for the same font.
 */
int
pdf_font_glyph(
	struct pdf_font *font,
	unsigned code,
	struct pdf_glyph *glyph)
{
	struct glyph_slot *slot;
	unsigned glyph_index;
	double advance;
	double width;
	int narrower;
	int error;

	/* Nothing is drawn until the outline is found. */
	memset(glyph, 0, sizeof(*glyph));
	glyph->transform[0] = 1.0;
	glyph->transform[3] = 1.0;
	glyph->bold = font->bold;

	/* A Type 3 font only moves the text position, by its glyph-space widths. */
	if (font->kind == FONT_KIND_TYPE3) {
		if (code < 256 && font->code_widths[code] > 0.0)
			glyph->width = font->code_widths[code];
		return 0;
	}

	/* Finds the glyph and its width by the kind of font. */
	if (font->kind == FONT_KIND_COMPOSITE) {
		glyph_index = composite_glyph(font, code);
		width = font->default_width;
		if (code < font->cid_widths_count)
			width = font->cid_widths[code];
		glyph->vertical_advance = font->vertical_advance;
		glyph->origin_x = width / 2.0;
		glyph->origin_y = font->vertical_origin;
	} else {
		glyph_index = font->code_glyphs[code & 0xFF];
		width = font->code_widths[code & 0xFF];
	}

	/* A font that gave no width uses the face's advance; glyph 0 is the missing glyph (ws175-p010: not the character's). */
	if (width < 0.0)
		width = face_advance(font, glyph_index);
	glyph->width = width;
	glyph->missing = glyph_index == 0;

	/* A font without a face or a program draws nothing. */
	if (font->face == NULL && font->charstrings == NULL)
		return 0;

	/* A substitute does not draw its missing-glyph box for a character it lacks. */
	if (font->substituted && glyph_index == 0)
		return 0;

	/*
	 * A substitute wider than the document's width is narrowed to it, so
	 * that its glyphs do not run into each other; it is never widened.
	 */
	if (font->substituted) {
		advance = face_advance(font, glyph_index);
		narrower = 0;
		if (width > 0.0) {
			if (advance > width * 1.05)
				narrower = 1;
		}

		/* Narrows it, but not below six tenths. */
		if (narrower) {
			glyph->transform[0] = width / advance;
			if (glyph->transform[0] < 0.6)
				glyph->transform[0] = 0.6;
		}
	}

	/* A substitute standing in for an italic face leans. */
	glyph->transform[2] = font->shear;

	/* Finds or reads the glyph's outline. */
	error = find_outline(font, glyph_index, &slot);
	if (error != 0)
		return error;

	/* Succeeded: the glyph's outline, which may be empty. */
	glyph->drawable = 1;
	glyph->verbs = font->verbs + slot->verb_start;
	glyph->verb_count = slot->verb_count;
	glyph->points = font->points + slot->point_start;
	glyph->point_count = slot->point_count;
	return 0;
}

/*
 * Finds the glyph procedure of a Type 3 font's code: its content stream,
 * the matrix from its glyph space to text space before the font size (the
 * font matrix), and the resources it runs with (NULL for the page's).
 * ENOENT means the font is not a Type 3 font or has no procedure for the
 * code.
 */
int
pdf_font_type3_glyph(
	struct pdf_document *document,
	struct pdf_font *font,
	unsigned code,
	struct pdf_object **procedure,
	double matrix[6],
	struct pdf_object **resources)
{
	struct pdf_object *procedures;
	struct pdf_object *found;
	size_t index;
	size_t length;
	int differs;
	int error;

	/* Only a Type 3 font with procedures has them, by the code's name. */
	if (font->kind != FONT_KIND_TYPE3)
		return ENOENT;
	procedures = font->type3_procedures;
	if (procedures == NULL)
		return ENOENT;
	if (code > 255)
		return ENOENT;
	if (font->type3_names[code] == NULL)
		return ENOENT;

	/* The procedure of that name, which must be a stream. */
	length = font->type3_lengths[code];
	for (index = 0; index < procedures->count; index++) {
		if (procedures->keys[index]->length != length)
			continue;
		differs = memcmp(procedures->keys[index]->bytes, font->type3_names[code], length);
		if (differs != 0)
			continue;
		error = pdf_reader_resolve(document, procedures->values[index], &found);
		if (error != 0)
			return error;
		if (found->type != PDF_OBJECT_STREAM)
			return ENOENT;

		/* Succeeded: the procedure, the matrix and the resources. */
		*procedure = found;
		memcpy(matrix, font->type3_matrix, 6 * sizeof(matrix[0]));
		*resources = font->type3_resources;
		return 0;
	}

	/* The font has no procedure of the name. */
	return ENOENT;
}

/*
 * Finds the Unicode value a glyph name stands for (0 for none): a name of
 * the glyph list, uniXXXX, or uXXXX, with a variant's suffix (".sc",
 * ".alt") ignored.
 */
unsigned
pdf_glyph_name_unicode(
	const unsigned char *name,
	size_t length)
{
	size_t low;
	size_t high;
	size_t middle;
	size_t index;
	size_t known;
	size_t shorter;
	unsigned value;
	int difference;

	/* A variant's suffix does not change the character. */
	for (index = 1; index < length; index++) {
		if (name[index] == '.') {
			length = index;
			break;
		}
	}

	/* uniXXXX names a character by its four hexadecimal digits. */
	if (length >= 7) {
		difference = memcmp(name, "uni", 3);
		if (difference == 0) {
			value = hex_number(name + 3, 4);
			if (value != 0)
				return value;
		}
	}

	/* uXXXX to uXXXXXX names one by four to six digits, of which the table keeps the first plane. */
	if (length >= 5 &&
	    length <= 7 &&
	    name[0] == 'u') {
		value = hex_number(name + 1, length - 1);
		if (value != 0 && value <= 0xFFFF)
			return value;
	}

	/* Searches the sorted list, comparing bytes and then lengths. */
	low = 0;
	high = pdf_glyph_names_count;
	while (low < high) {
		middle = low + (high - low) / 2;
		known = strlen(pdf_glyph_names[middle].name);
		shorter = known;
		if (length < shorter)
			shorter = length;
		difference = memcmp(pdf_glyph_names[middle].name, name, shorter);
		if (difference == 0 && known < length)
			difference = -1;
		if (difference == 0 && known > length)
			difference = 1;
		if (difference == 0)
			return pdf_glyph_names[middle].unicode;
		if (difference < 0) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}

	/* The name is not one the reader knows. */
	return 0;
}

/* Reads count hexadecimal digits (0 when any is not one). */
static unsigned
hex_number(
	const unsigned char *digits,
	size_t count)
{
	unsigned value;
	unsigned digit;
	size_t index;

	/* Reads each digit, upper-case as glyph names write them (lower-case too). */
	value = 0;
	for (index = 0; index < count; index++) {
		if (digits[index] >= '0' && digits[index] <= '9') {
			digit = (unsigned)(digits[index] - '0');
		} else if (digits[index] >= 'A' && digits[index] <= 'F') {
			digit = (unsigned)(digits[index] - 'A') + 10;
		} else if (digits[index] >= 'a' && digits[index] <= 'f') {
			digit = (unsigned)(digits[index] - 'a') + 10;
		} else {
			return 0;
		}

		/* The digit is the next four bits. */
		value = value * 16 + digit;
	}

	/* The number. */
	return value;
}

/* Reads a font dictionary by its subtype. */
static int
load_font(
	struct pdf_document *document,
	struct pdf_font_cache *cache,
	struct pdf_object *dictionary,
	struct pdf_font *font)
{
	struct pdf_object *subtype;
	int is_name;
	int error;

	/* Finds the subtype. */
	error = pdf_reader_resolve_key(document, dictionary, "Subtype", &subtype);
	if (error != 0)
		return error;

	/* A composite font. */
	is_name = pdf_object_is_name(subtype, "Type0");
	if (is_name) {
		font->kind = FONT_KIND_COMPOSITE;
		error = load_composite(document, dictionary, font);
		if (error != 0)
			return error;
		return 0;
	}

	/* A Type 3 font, whose glyphs are content streams stage 3 runs. */
	is_name = pdf_object_is_name(subtype, "Type3");
	if (is_name) {
		font->kind = FONT_KIND_TYPE3;
		load_type3(document, dictionary, font);
		return 0;
	}

	/* Every other subtype (TrueType, Type1, MMType1, or none) is a simple font. */
	font->kind = FONT_KIND_SIMPLE;
	error = load_simple(document, cache, dictionary, font);
	if (error != 0)
		return error;

	/* Succeeded: the font is read. */
	return 0;
}

/*
 * Reads a simple font: its program or a substitute, its encoding, and its
 * widths.
 */
static int
load_simple(
	struct pdf_document *document,
	struct pdf_font_cache *cache,
	struct pdf_object *dictionary,
	struct pdf_font *font)
{
	unsigned short unicode[256];
	unsigned short builtin[256];
	const unsigned char *names[256];
	size_t lengths[256];
	struct pdf_object *descriptor;
	struct pdf_object *encoding;
	long flags;
	int symbolic;
	int has_encoding;
	int has_builtin;
	int has_base;
	int foreign;
	int error;

	/* Finds the descriptor, which a standard 14 font may lack. */
	error = pdf_reader_resolve_key(document, dictionary, "FontDescriptor", &descriptor);
	if (error != 0)
		return error;
	flags = descriptor_flags(document, descriptor);
	symbolic = 0;
	if ((flags & FONT_FLAG_SYMBOLIC) != 0)
		symbolic = 1;

	/* Reads the codes' Unicode values from the encoding, over a Type 1 program's own. */
	error = pdf_reader_resolve_key(document, dictionary, "Encoding", &encoding);
	if (error != 0)
		return error;
	has_encoding = 0;
	if (encoding->type != PDF_OBJECT_NULL)
		has_encoding = 1;
	has_builtin = read_builtin_encoding(document, descriptor, builtin);
	if (has_builtin) {
		read_encoding(document, dictionary, symbolic, builtin, unicode);
	} else {
		read_encoding(document, dictionary, symbolic, NULL, unicode);
	}

	/* The codes' characters are kept for the text the editor reads (ws175-p002b). */
	memcpy(font->code_unicode, unicode, sizeof(font->code_unicode));
	font->has_code_unicode = 1;

	/* Reads the embedded program, when it is one libtruetype reads. */
	foreign = 0;
	error = load_program(document, descriptor, font, &foreign);
	if (error == ENOMEM)
		return ENOMEM;
	if (error == 0 && font->charstrings != NULL) {
		/* A Type 1 or CFF program's glyphs, by name, character or its own encoding. */
		has_base = read_difference_names(document, dictionary, names, lengths);
		map_program_codes(font, unicode, names, lengths, has_base);
	} else if (error == 0) {
		/* A TrueType program's glyphs, through its character maps. */
		map_embedded_codes(font, unicode, symbolic, has_encoding);
	} else {
		/* A substitute, through the Unicode values; another embedded program is marked as substituted. */
		error = load_substitute(document, cache, dictionary, descriptor, font);
		if (error == ENOMEM)
			return ENOMEM;
		map_substitute_codes(font, unicode);
		if (foreign)
			font->status |= PDF_DISPLAY_SKIPPED;
	}

	/* Reads the widths, which place the glyphs whichever face draws them. */
	read_simple_widths(document, dictionary, descriptor, font);

	/* Succeeded: the font is read. */
	return 0;
}

/*
 * Reads a composite font: its one descendant CIDFont, the Identity
 * encoding, the widths and the CID-to-glyph map.
 */
static int
load_composite(
	struct pdf_document *document,
	struct pdf_object *dictionary,
	struct pdf_font *font)
{
	struct pdf_object *encoding;
	struct pdf_object *descendants;
	struct pdf_object *cid_font;
	struct pdf_object *descriptor;
	int is_horizontal;
	int is_vertical;
	int foreign;
	int error;

	/* The glyphs are not drawn until the font is known to be readable. */
	font->status = PDF_DISPLAY_SKIPPED;

	/* Reads the encoding: Identity-H or Identity-V; any other CMap is not read yet. */
	error = pdf_reader_resolve_key(document, dictionary, "Encoding", &encoding);
	if (error != 0)
		return error;
	is_horizontal = pdf_object_is_name(encoding, "Identity-H");
	is_vertical = pdf_object_is_name(encoding, "Identity-V");
	if (is_vertical)
		font->vertical = 1;

	/* Finds the descendant CIDFont. */
	error = pdf_reader_resolve_key(document, dictionary, "DescendantFonts", &descendants);
	if (error != 0)
		return error;
	if (descendants->type != PDF_OBJECT_ARRAY || descendants->count == 0)
		return 0;
	error = pdf_reader_resolve(document, descendants->values[0], &cid_font);
	if (error != 0)
		return error;
	if (cid_font->type != PDF_OBJECT_DICTIONARY)
		return 0;

	/* Reads the widths, which move the text position even when nothing is drawn. */
	error = read_cid_widths(document, cid_font, font);
	if (error != 0)
		return error;
	read_vertical_metrics(document, cid_font, font);

	/* An encoding that is not an Identity one cannot be mapped yet. */
	if (!is_horizontal && !is_vertical)
		return 0;

	/* Reads the embedded program (TrueType, or CFF for a CIDFontType0); none is not drawn. */
	error = pdf_reader_resolve_key(document, cid_font, "FontDescriptor", &descriptor);
	if (error != 0)
		return error;
	foreign = 0;
	error = load_program(document, descriptor, font, &foreign);
	if (error == ENOMEM)
		return ENOMEM;
	if (error != 0)
		return 0;

	/* A TrueType program's CIDs reach its glyphs through the CID-to-glyph map; a CFF program's through its charset. */
	if (font->face != NULL) {
		error = read_cid_map(document, cid_font, font);
		if (error != 0)
			return error;
	}

	/* Succeeded: the font draws its own glyphs. */
	font->status = 0;
	return 0;
}

/*
 * Reads a Type 3 font: its glyph procedures and the names its encoding
 * gives the codes, its font matrix and resources, and its widths in its
 * glyph space scaled by the matrix.
 */
static void
load_type3(
	struct pdf_document *document,
	struct pdf_object *dictionary,
	struct pdf_font *font)
{
	struct pdf_object *matrix;
	struct pdf_object *procedures;
	struct pdf_object *resources;
	double scale;
	size_t index;
	int has_matrix;
	int error;

	/* The glyph procedures; a font without them only moves the text position. */
	font->status = PDF_DISPLAY_SKIPPED;
	error = pdf_reader_resolve_key(document, dictionary, "CharProcs", &procedures);
	if (error == 0 && procedures->type == PDF_OBJECT_DICTIONARY) {
		font->type3_procedures = procedures;
		font->status = 0;
	}

	/* The names of the codes, from /Differences. */
	(void)read_difference_names(document, dictionary, font->type3_names, font->type3_lengths);

	/* The resources the procedures use (else the page's). */
	error = pdf_reader_resolve_key(document, dictionary, "Resources", &resources);
	if (error == 0 && resources->type == PDF_OBJECT_DICTIONARY)
		font->type3_resources = resources;

	/* The whole font matrix, a thousandth by default. */
	memset(font->type3_matrix, 0, sizeof(font->type3_matrix));
	font->type3_matrix[0] = 0.001;
	font->type3_matrix[3] = 0.001;
	error = pdf_reader_resolve_key(document, dictionary, "FontMatrix", &matrix);
	has_matrix = 0;
	if (error == 0 && matrix->type == PDF_OBJECT_ARRAY) {
		if (matrix->count == 6)
			has_matrix = 1;
	}

	/* Reads its six numbers; any that is not a number leaves the default. */
	if (has_matrix) {
		for (index = 0; index < 6; index++) {
			error = read_number(document, matrix->values[index], &font->type3_matrix[index]);
			if (error != 0)
				break;
			if (!(font->type3_matrix[index] > -1e6 && font->type3_matrix[index] < 1e6))
				error = PDF_EFORMAT;
			if (error != 0)
				break;
		}

		/* A matrix that cannot be read is the default. */
		if (error != 0) {
			memset(font->type3_matrix, 0, sizeof(font->type3_matrix));
			font->type3_matrix[0] = 0.001;
			font->type3_matrix[3] = 0.001;
		}
	}

	/* The horizontal scale of the font matrix, 0.001 by default. */
	scale = 0.001;
	error = pdf_reader_resolve_key(document, dictionary, "FontMatrix", &matrix);
	if (error != 0)
		matrix = NULL;
	if (matrix != NULL && matrix->type == PDF_OBJECT_ARRAY) {
		if (matrix->count == 6) {
			error = read_number(document, matrix->values[0], &scale);
			if (error != 0)
				scale = 0.001;
		}
	}

	/* A scale of a whole em or more (or not a number) is not a font matrix's. */
	if (!(scale > -1.0 && scale < 1.0))
		scale = 0.001;
	font->type3_scale = scale;

	/* The widths, scaled into text space. */
	read_simple_widths(document, dictionary, NULL, font);
}

/*
 * Reads a font's embedded program: TrueType (/FontFile2), Type 1
 * (/FontFile), or CFF or OpenType (/FontFile3).
 *
 * ENOENT means the descriptor has no program; *foreign says it has one
 * the reader could not read, which is substituted and marked.
 */
static int
load_program(
	struct pdf_document *document,
	struct pdf_object *descriptor,
	struct pdf_font *font,
	int *foreign)
{
	struct pdf_object *program;
	int error;

	/* A font without a descriptor has no program. */
	*foreign = 0;
	if (descriptor->type != PDF_OBJECT_DICTIONARY)
		return ENOENT;

	/* The TrueType program. */
	error = pdf_reader_resolve_key(document, descriptor, "FontFile2", &program);
	if (error != 0)
		return error;
	if (program->type == PDF_OBJECT_STREAM) {
		error = load_truetype(document, program, font);
		if (error != 0)
			*foreign = 1;
		return error;
	}

	/* The CFF or OpenType program. */
	error = pdf_reader_resolve_key(document, descriptor, "FontFile3", &program);
	if (error != 0)
		return error;
	if (program->type == PDF_OBJECT_STREAM) {
		error = load_font_file3(document, program, font);
		if (error != 0)
			*foreign = 1;
		return error;
	}

	/* The Type 1 program. */
	error = pdf_reader_resolve_key(document, descriptor, "FontFile", &program);
	if (error != 0)
		return error;
	if (program->type == PDF_OBJECT_STREAM) {
		error = load_type1(document, program, font);
		if (error != 0)
			*foreign = 1;
		return error;
	}

	/* The font has no embedded program. */
	return ENOENT;
}

/* Reads a TrueType program into a face. */
static int
load_truetype(
	struct pdf_document *document,
	struct pdf_object *stream,
	struct pdf_font *font)
{
	const unsigned char *data;
	size_t size;
	int error;

	/* Decodes it; the face reads the bytes in place, so they live with the font. */
	error = decode_program(document, stream, font, &data, &size);
	if (error != 0)
		return error;

	/* Opens the face. */
	error = open_truetype(data, size, font);
	if (error != 0)
		return error;

	/* Succeeded: the font draws with its own program. */
	return 0;
}

/*
 * Reads a /FontFile3: CFF (subtype Type1C or CIDFontType0C), or OpenType,
 * whose outlines are CFF or TrueType.
 */
static int
load_font_file3(
	struct pdf_document *document,
	struct pdf_object *stream,
	struct pdf_font *font)
{
	struct pdf_object *subtype;
	const unsigned char *data;
	const unsigned char *cff;
	size_t size;
	size_t cff_size;
	int is_opentype;
	int error;

	/* Decodes it; the program reads the bytes in place, so they live with the font. */
	error = decode_program(document, stream, font, &data, &size);
	if (error != 0)
		return error;

	/* An OpenType font's CFF table, or its TrueType outlines. */
	error = pdf_reader_resolve_key(document, stream, "Subtype", &subtype);
	if (error != 0)
		return error;
	is_opentype = pdf_object_is_name(subtype, "OpenType");
	cff = data;
	cff_size = size;
	if (is_opentype) {
		error = pdf_opentype_cff(data, size, &cff, &cff_size);
		if (error == ENOENT) {
			error = open_truetype(data, size, font);
			if (error != 0)
				return error;
			return 0;
		}

		/* A damaged table directory is not read. */
		if (error != 0)
			return error;
	}

	/* Reads the CFF program. */
	error = pdf_cff_open(cff, cff_size, &font->charstrings);
	if (error == ENOMEM)
		return ENOMEM;
	if (error != 0)
		return ENOTSUP;

	/* Succeeded: the program's outlines are in ems. */
	font->units_per_em = 1.0;
	return 0;
}

/* Reads a Type 1 program: its clear text is /Length1 bytes long. */
static int
load_type1(
	struct pdf_document *document,
	struct pdf_object *stream,
	struct pdf_font *font)
{
	struct pdf_object *clear_length;
	const unsigned char *data;
	size_t size;
	size_t clear;
	int error;

	/* Decodes it (the program keeps its own decrypted copy). */
	error = decode_program(document, stream, font, &data, &size);
	if (error != 0)
		return error;

	/* The clear text's length, when the stream gives a usable one. */
	clear = 0;
	error = pdf_reader_resolve_key(document, stream, "Length1", &clear_length);
	if (error == 0 && clear_length->type == PDF_OBJECT_INTEGER) {
		if (clear_length->integer > 0 && (unsigned long)clear_length->integer < size)
			clear = (size_t)clear_length->integer;
	}

	/* Reads the program. */
	error = pdf_type1_open(data, size, clear, &font->charstrings);
	if (error == ENOMEM)
		return ENOMEM;
	if (error != 0)
		return ENOTSUP;

	/* Succeeded: the program's outlines are in ems. */
	font->units_per_em = 1.0;
	return 0;
}

/* Opens a TrueType face over a program's bytes and reads its em. */
static int
open_truetype(
	const unsigned char *data,
	size_t size,
	struct pdf_font *font)
{
	struct truetype_design_metrics metrics;
	int error;

	/* Opens the face, which may lack a character map. */
	error = truetype_open_embedded(data, size, &font->owned_face);
	if (error != 0)
		return ENOTSUP;
	font->face = font->owned_face;

	/* Reads the em, which every outline and advance is divided by. */
	error = truetype_design_metrics(font->face, &metrics);
	if (error != 0) {
		font->face = NULL;
		return ENOTSUP;
	}

	/* An em of no units cannot scale anything. */
	if (metrics.units_per_em == 0) {
		font->face = NULL;
		return ENOTSUP;
	}

	/* Succeeded: the font draws with its own program. */
	font->units_per_em = (double)metrics.units_per_em;
	return 0;
}

/* Decodes a program's stream; the decoded bytes (if copied) live with the font. */
static int
decode_program(
	struct pdf_document *document,
	struct pdf_object *stream,
	struct pdf_font *font,
	const unsigned char **data,
	size_t *size)
{
	unsigned char *owned;
	int dct;
	int error;

	/* Decodes the filters. */
	error = pdf_filter_decode(document, stream, 0, data, size, &owned, &dct);
	if (error != 0)
		return error;

	/* Succeeded: the font keeps the decoded copy. */
	font->program = owned;
	return 0;
}

/*
 * Chooses and opens the system font that stands in for a font without a
 * readable program: sans, serif or monospace, bold and italic as the font
 * asks, from the most exact file the system has to its plain sans.  A
 * style the file lacks is drawn with a lean or a thicker outline.
 */
static int
load_substitute(
	struct pdf_document *document,
	struct pdf_font_cache *cache,
	struct pdf_object *dictionary,
	struct pdf_object *descriptor,
	struct pdf_font *font)
{
	static const char *const families[3] = { "keiland", "keiland-serif", "keiland-mono" };
	static const char *const styles[4] = { "", "-bold", "-italic", "-bolditalic" };
	struct truetype_design_metrics metrics;
	struct truetype_face *face;
	struct pdf_object *base_font;
	enum font_family family;
	char name[32];
	int style;
	int bold;
	int italic;
	int symbol;
	int error;

	/*
	 * A symbol font's codes are not characters a text face has, so it is
	 * not substituted; its text is only placed.
	 */
	font->status |= PDF_DISPLAY_SKIPPED;
	error = pdf_reader_resolve_key(document, dictionary, "BaseFont", &base_font);
	if (error != 0)
		base_font = NULL;
	symbol = name_contains(base_font, "Symbol");
	if (symbol)
		return 0;
	symbol = name_contains(base_font, "Dingbats");
	if (symbol)
		return 0;
	font->status &= ~PDF_DISPLAY_SKIPPED;

	/* Chooses the kind of substitute. */
	choose_family(document, dictionary, descriptor, &family, &bold, &italic);
	style = 0;
	if (bold)
		style |= 1;
	if (italic)
		style |= 2;
	font->substituted = 1;

	/* The exact family and style. */
	snprintf(name, sizeof(name), "%s%s.ttf", families[family], styles[style]);
	error = open_substitute(cache, name, &face);

	/* The family's plain face, drawn in the style. */
	if (error == ENOENT && style != 0) {
		snprintf(name, sizeof(name), "%s.ttf", families[family]);
		error = open_substitute(cache, name, &face);
		if (error == 0 && bold)
			font->bold = FONT_BOLD_WIDTH;
		if (error == 0 && italic)
			font->shear = FONT_ITALIC_SHEAR;
	}

	/* The sans in the style. */
	if (error == ENOENT && family != FONT_FAMILY_SANS) {
		snprintf(name, sizeof(name), "%s%s.ttf", families[0], styles[style]);
		error = open_substitute(cache, name, &face);
	}

	/* The plain sans, drawn in the style. */
	if (error == ENOENT &&
	    family != FONT_FAMILY_SANS &&
	    style != 0) {
		snprintf(name, sizeof(name), "%s.ttf", families[0]);
		error = open_substitute(cache, name, &face);
		if (error == 0 && bold)
			font->bold = FONT_BOLD_WIDTH;
		if (error == 0 && italic)
			font->shear = FONT_ITALIC_SHEAR;
	}

	/* A system without a substitute draws no text, but still places it. */
	if (error == ENOENT) {
		font->status |= PDF_DISPLAY_SKIPPED;
		return 0;
	}

	/* Reports a failure other than a missing file. */
	if (error != 0)
		return error;

	/* Reads the em of the substitute. */
	error = truetype_design_metrics(face, &metrics);
	if (error != 0 || metrics.units_per_em == 0) {
		font->status |= PDF_DISPLAY_SKIPPED;
		return 0;
	}

	/* Succeeded: the substitute draws the font's glyphs. */
	font->face = face;
	font->units_per_em = (double)metrics.units_per_em;
	return 0;
}

/*
 * Opens a substitute font file by its name in the font directory, once
 * per document; a font the program gave in memory under that name comes
 * first.  ENOENT means the system has no such file (or one that cannot be
 * read as a TrueType font).
 */
static int
open_substitute(
	struct pdf_font_cache *cache,
	const char *name,
	struct truetype_face **face)
{
	struct font_file *file;
	char path[256];
	size_t index;
	int difference;
	int error;

	/* Answers from a file the document already tried. */
	for (file = cache->files; file != NULL; file = file->next) {
		difference = strcmp(file->name, name);
		if (difference != 0)
			continue;
		if (file->face == NULL)
			return ENOENT;
		*face = file->face;
		return 0;
	}

	/* Remembers the file, found or not. */
	file = calloc(1, sizeof(*file));
	if (file == NULL)
		return ENOMEM;
	snprintf(file->name, sizeof(file->name), "%s", name);
	file->next = cache->files;
	cache->files = file;

	/* Takes a font the program gave in memory under the name (ws177-p010). */
	for (index = 0; index < font_memory_count; index++) {
		difference = strcmp(font_memory[index].name, name);
		if (difference != 0)
			continue;
		file->memory = font_memory[index].data;
		file->size = font_memory[index].size;
		break;
	}

	/* Else reads the file; a missing or unreadable file is no substitute. */
	if (file->memory == NULL) {
		snprintf(path, sizeof(path), "%s/%s", PDF_FONT_DIRECTORY, name);
		error = read_font_file(path, &file->data, &file->size);
		if (error == ENOMEM)
			return ENOMEM;
		if (error != 0)
			return ENOENT;
		file->memory = file->data;
	}

	/* Opens its face, which must map Unicode. */
	error = truetype_open(file->memory, file->size, 0, &file->face);
	if (error != 0) {
		file->face = NULL;
		return ENOENT;
	}

	/* A font in memory is a program's that opens no file: it has no companion file. */
	if (file->data == NULL) {
		*face = file->face;
		return 0;
	}

	/* Its companion, the system's monospaced fallback, for the letters and signs it lacks (ws090-p020: Mahora is ASCII). */
	snprintf(path, sizeof(path), "%s/%s", PDF_FONT_DIRECTORY, PDF_FONT_COMPANION);
	(void)truetype_open_companions(file->face, NULL, path);

	/* Succeeded: the file's face. */
	*face = file->face;
	return 0;
}

/* Reads a whole font file, up to the size a font file may have. */
static int
read_font_file(
	const char *path,
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

	/* A program that opens no file reads none. */
	if (!PDF_FONT_FILES)
		return ENOENT;

	/* Opens the file. */
	stream = fopen(path, "rb");
	if (stream == NULL)
		return ENOENT;

	/* Reads it in growing chunks. */
	buffer = NULL;
	length = 0;
	capacity = 0;
	for (;;) {
		/* Refuses a file larger than a font. */
		if (length >= FONT_FILE_MAX) {
			free(buffer);
			fclose(stream);
			return EFBIG;
		}

		/* Makes room for the next chunk. */
		if (capacity - length < 65536) {
			capacity = capacity * 2 + 65536;
			grown = realloc(buffer, capacity);
			if (grown == NULL) {
				free(buffer);
				fclose(stream);
				return ENOMEM;
			}

			/* The grown buffer holds what was read. */
			buffer = grown;
		}

		/* Reads the chunk; a short one is the end or an error. */
		got = fread(buffer + length, 1, capacity - length, stream);
		length += got;
		if (got == 0)
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

/*
 * Reads a simple font's encoding into the Unicode value of each code (0
 * for a code that names nothing): the base encoding, else the program's
 * own (builtin, when known), else StandardEncoding for a non-symbolic
 * font, and the /Differences over it.
 */
static void
read_encoding(
	struct pdf_document *document,
	struct pdf_object *dictionary,
	int symbolic,
	const unsigned short *builtin,
	unsigned short unicode[256])
{
	const unsigned short *base;
	struct pdf_object *encoding;
	struct pdf_object *base_name;
	struct pdf_object *differences;
	int error;

	/* The program's own encoding; else a symbolic font's codes name nothing unless its encoding says so. */
	memset(unicode, 0, 256 * sizeof(unicode[0]));
	if (builtin != NULL) {
		memcpy(unicode, builtin, 256 * sizeof(unicode[0]));
	} else if (!symbolic) {
		memcpy(unicode, pdf_encoding_standard, 256 * sizeof(unicode[0]));
	}

	/* Finds the encoding: a base encoding's name, or a dictionary. */
	error = pdf_reader_resolve_key(document, dictionary, "Encoding", &encoding);
	if (error != 0)
		return;
	if (encoding->type == PDF_OBJECT_NAME) {
		base = base_encoding(encoding);
		if (base != NULL)
			memcpy(unicode, base, 256 * sizeof(unicode[0]));
		return;
	}

	/* Anything but a name or a dictionary leaves the default. */
	if (encoding->type != PDF_OBJECT_DICTIONARY)
		return;

	/* A dictionary's base encoding. */
	error = pdf_reader_resolve_key(document, encoding, "BaseEncoding", &base_name);
	if (error == 0) {
		base = base_encoding(base_name);
		if (base != NULL)
			memcpy(unicode, base, 256 * sizeof(unicode[0]));
	}

	/* Its differences over the base. */
	error = pdf_reader_resolve_key(document, encoding, "Differences", &differences);
	if (error != 0)
		return;
	apply_differences(document, differences, unicode);
}

/*
 * Reads the encoding a Type 1 program (/FontFile) defines in its clear
 * text, as the Unicode value of each code: StandardEncoding, or its
 * "dup code /name put" entries.  Reports whether the program has one.
 */
static int
read_builtin_encoding(
	struct pdf_document *document,
	struct pdf_object *descriptor,
	unsigned short builtin[256])
{
	const unsigned char *names[256];
	size_t lengths[256];
	struct pdf_object *program;
	const unsigned char *data;
	unsigned char *owned;
	size_t size;
	size_t code;
	int standard;
	int dct;
	int error;

	/* Finds and decodes the Type 1 program. */
	if (descriptor->type != PDF_OBJECT_DICTIONARY)
		return 0;
	error = pdf_reader_resolve_key(document, descriptor, "FontFile", &program);
	if (error != 0)
		return 0;
	if (program->type != PDF_OBJECT_STREAM)
		return 0;
	error = pdf_filter_decode(document, program, 0, &data, &size, &owned, &dct);
	if (error != 0)
		return 0;

	/* Reads its clear text's encoding. */
	error = pdf_type1_encoding(data, size, names, lengths, &standard);
	if (error != 0) {
		free(owned);
		return 0;
	}

	/* StandardEncoding, or each entry's character by its name. */
	if (standard) {
		memcpy(builtin, pdf_encoding_standard, 256 * sizeof(builtin[0]));
	} else {
		memset(builtin, 0, 256 * sizeof(builtin[0]));
		for (code = 0; code < 256; code++) {
			if (names[code] != NULL)
				builtin[code] = (unsigned short)pdf_glyph_name_unicode(names[code], lengths[code]);
		}
	}

	/* The decoded program goes; the characters are kept. */
	free(owned);

	/* The program has its own encoding. */
	return 1;
}

/*
 * Applies a /Differences array: each number is the code of the name that
 * follows it, and each further name the next code's.
 */
static void
apply_differences(
	struct pdf_document *document,
	struct pdf_object *differences,
	unsigned short unicode[256])
{
	struct pdf_object *entry;
	double number;
	long code;
	size_t index;
	unsigned value;
	int error;

	/* Only an array of numbers and names is read. */
	if (differences->type != PDF_OBJECT_ARRAY)
		return;

	/* Walks the array, a number moving the code and a name naming the code's glyph. */
	code = -1;
	for (index = 0; index < differences->count && index < FONT_ARRAY_MAX; index++) {
		error = pdf_reader_resolve(document, differences->values[index], &entry);
		if (error != 0)
			return;

		/* A number starts a run of codes. */
		if (entry->type == PDF_OBJECT_INTEGER || entry->type == PDF_OBJECT_REAL) {
			error = pdf_object_number(entry, &number);
			if (error != 0)
				return;
			code = -1;
			if (number >= 0.0 && number < 256.0)
				code = (long)number;
			continue;
		}

		/* A name names the glyph of the next code of the run. */
		if (entry->type != PDF_OBJECT_NAME)
			continue;
		if (code < 0 || code > 255)
			continue;
		value = pdf_glyph_name_unicode(entry->bytes, entry->length);
		unicode[code] = (unsigned short)value;
		code++;
	}
}

/* Finds a base encoding's table by its name (NULL for another name). */
static const unsigned short *
base_encoding(
	const struct pdf_object *name)
{
	int is_name;

	/* The three base encodings of PDF; the Expert one is not a text encoding. */
	is_name = pdf_object_is_name(name, "WinAnsiEncoding");
	if (is_name)
		return pdf_encoding_win_ansi;
	is_name = pdf_object_is_name(name, "MacRomanEncoding");
	if (is_name)
		return pdf_encoding_mac_roman;
	is_name = pdf_object_is_name(name, "StandardEncoding");
	if (is_name)
		return pdf_encoding_standard;

	/* Another name leaves the encoding as it is. */
	return NULL;
}

/*
 * Reads the glyph names a simple font's /Differences gives its codes
 * (NULL for a code it does not name).  Reports whether the font's
 * encoding names a base encoding (a name, or /BaseEncoding), which then
 * comes before the program's own encoding.
 */
static int
read_difference_names(
	struct pdf_document *document,
	struct pdf_object *dictionary,
	const unsigned char *names[256],
	size_t lengths[256])
{
	struct pdf_object *encoding;
	struct pdf_object *base;
	struct pdf_object *differences;
	struct pdf_object *entry;
	double number;
	long code;
	size_t index;
	int has_base;
	int error;

	/* No names yet. */
	memset(names, 0, 256 * sizeof(names[0]));
	memset(lengths, 0, 256 * sizeof(lengths[0]));

	/* The encoding: a base encoding's name, or a dictionary. */
	error = pdf_reader_resolve_key(document, dictionary, "Encoding", &encoding);
	if (error != 0)
		return 0;
	if (encoding->type == PDF_OBJECT_NAME)
		return 1;
	if (encoding->type != PDF_OBJECT_DICTIONARY)
		return 0;

	/* A dictionary's base encoding. */
	has_base = 0;
	error = pdf_reader_resolve_key(document, encoding, "BaseEncoding", &base);
	if (error == 0 && base->type == PDF_OBJECT_NAME)
		has_base = 1;

	/* Its differences: a number starts a run of codes, each name the next code's. */
	error = pdf_reader_resolve_key(document, encoding, "Differences", &differences);
	if (error != 0)
		return has_base;
	if (differences->type != PDF_OBJECT_ARRAY)
		return has_base;
	code = -1;
	for (index = 0; index < differences->count && index < FONT_ARRAY_MAX; index++) {
		error = pdf_reader_resolve(document, differences->values[index], &entry);
		if (error != 0)
			break;

		/* A number starts a run. */
		if (entry->type == PDF_OBJECT_INTEGER || entry->type == PDF_OBJECT_REAL) {
			error = pdf_object_number(entry, &number);
			code = -1;
			if (error != 0)
				continue;
			if (number >= 0.0 && number < 256.0)
				code = (long)number;
			continue;
		}

		/* A name names the next code of the run. */
		if (entry->type != PDF_OBJECT_NAME)
			continue;
		if (code < 0 || code > 255)
			continue;
		names[code] = entry->bytes;
		lengths[code] = entry->length;
		code++;
	}

	/* Reports whether a base encoding was named. */
	return has_base;
}

/*
 * Maps each code of a simple font with a Type 1 or CFF program to a
 * glyph: by the name /Differences gives it, then by the program's own
 * encoding when the font names no base encoding, then by the character
 * the encoding gives it matched to the glyphs' names, then by the
 * program's own encoding.
 */
static void
map_program_codes(
	struct pdf_font *font,
	const unsigned short unicode[256],
	const unsigned char *const names[256],
	const size_t lengths[256],
	int has_base)
{
	const unsigned char *name;
	unsigned *by_character;
	unsigned character;
	unsigned glyph;
	size_t count;
	size_t length;
	size_t index;
	unsigned code;
	int error;

	/* The glyph of each character the glyphs' names stand for, the first glyph of a character winning. */
	by_character = calloc(65536, sizeof(*by_character));
	count = pdf_charstrings_count(font->charstrings);
	if (by_character != NULL) {
		for (index = count; index > 0; index--) {
			error = pdf_charstrings_name(font->charstrings, (unsigned)(index - 1), &name, &length);
			if (error != 0)
				continue;
			character = pdf_glyph_name_unicode(name, length);
			if (character != 0 && character < 65536)
				by_character[character] = (unsigned)(index - 1);
		}
	}

	/* Maps each code, trying the ways in turn. */
	for (code = 0; code < 256; code++) {
		glyph = 0;
		error = ENOENT;

		/* The name /Differences gives. */
		if (names[code] != NULL)
			error = pdf_charstrings_find(font->charstrings, names[code], lengths[code], &glyph);

		/* The program's own encoding, which is the base when the font names none. */
		if (error != 0 && !has_base)
			error = pdf_charstrings_builtin(font->charstrings, code, &glyph);

		/* The encoding's character, by the glyphs' names. */
		if (error != 0 && by_character != NULL) {
			glyph = by_character[unicode[code]];
			if (unicode[code] != 0 && glyph != 0)
				error = 0;
		}

		/* The program's own encoding as the last resort. */
		if (error != 0)
			error = pdf_charstrings_builtin(font->charstrings, code, &glyph);

		/* The code draws the glyph found (0, the missing glyph, when none was). */
		if (error != 0)
			glyph = 0;
		font->code_glyphs[code] = glyph;
	}

	/* The characters' table goes. */
	free(by_character);
}

/*
 * Maps each code of a simple font with an embedded TrueType program to a
 * glyph, as PDF defines for such fonts and as other readers do for fonts
 * that do not follow it: through the Unicode map by the encoding, the
 * symbol map by the code (also moved to 0xF000, 0xF100, 0xF200), the
 * Macintosh map by the encoding or the code, and the glyph number itself
 * for a program without any map.
 */
static void
map_embedded_codes(
	struct pdf_font *font,
	const unsigned short unicode[256],
	int symbolic,
	int has_encoding)
{
	unsigned glyph;
	unsigned code;
	unsigned shift;
	unsigned mac_code;
	int has_unicode;
	int has_symbol;
	int has_mac;
	int by_character;
	int error;

	/* Finds which maps the program has. */
	error = truetype_cmap_lookup(font->face, 3, 1, 32, &glyph);
	has_unicode = 0;
	if (error == 0)
		has_unicode = 1;
	error = truetype_cmap_lookup(font->face, 3, 0, 32, &glyph);
	has_symbol = 0;
	if (error == 0)
		has_symbol = 1;
	error = truetype_cmap_lookup(font->face, 1, 0, 32, &glyph);
	has_mac = 0;
	if (error == 0)
		has_mac = 1;

	/* Maps each code, trying the maps in turn. */
	for (code = 0; code < 256; code++) {
		glyph = 0;

		/* The Unicode map, by the character the encoding names (a symbolic font's only when it has an encoding). */
		by_character = 0;
		if (has_unicode && unicode[code] != 0) {
			by_character = 1;
			if (symbolic && !has_encoding)
				by_character = 0;
		}

		/* Looks the character up. */
		if (by_character)
			(void)truetype_cmap_lookup(font->face, 3, 1, unicode[code], &glyph);

		/* The symbol map, by the code in each of the ranges symbol fonts use. */
		for (shift = 0; has_symbol && shift < 4; shift++) {
			if (glyph != 0)
				break;
			if (shift == 0) {
				(void)truetype_cmap_lookup(font->face, 3, 0, code, &glyph);
			} else {
				(void)truetype_cmap_lookup(font->face, 3, 0, 0xEF00U + shift * 0x100U + code, &glyph);
			}
		}

		/* The Macintosh map, by the encoding's character when it has one there, or by the code. */
		if (glyph == 0 && has_mac) {
			mac_code = code;
			if (unicode[code] != 0 && has_encoding)
				mac_code = mac_roman_code(unicode[code]);
			(void)truetype_cmap_lookup(font->face, 1, 0, mac_code, &glyph);
		}

		/* The Unicode map by the code itself, for a symbolic font. */
		if (glyph == 0 && has_unicode)
			(void)truetype_cmap_lookup(font->face, 3, 1, code, &glyph);

		/* A program with no map at all is addressed by glyph number. */
		if (glyph == 0 &&
		    !has_unicode &&
		    !has_symbol &&
		    !has_mac)
			glyph = code;

		/* The code draws the glyph found (0, the missing glyph, when none was). */
		font->code_glyphs[code] = glyph;
	}
}

/*
 * Maps each code of a substituted simple font to the substitute's glyph of
 * the character the encoding names (the code itself for a code that names
 * nothing).
 */
static void
map_substitute_codes(
	struct pdf_font *font,
	const unsigned short unicode[256])
{
	unsigned code;
	unsigned character;

	/* Nothing to map without a substitute. */
	if (font->face == NULL)
		return;

	/* Maps each code through the substitute's Unicode map; a ligature it lacks is drawn as its letters. */
	for (code = 0; code < 256; code++) {
		character = unicode[code];
		if (character == 0)
			character = code;
		font->code_glyphs[code] = truetype_glyph_index(font->face, character);
		if (font->code_glyphs[code] != 0)
			continue;
		if (character >= 0xFB00 && character <= 0xFB04)
			font->code_glyphs[code] = FONT_LIGATURE_GLYPH + character;
	}
}

/* Finds the MacRomanEncoding code of a character, or 0. */
static unsigned
mac_roman_code(
	unsigned unicode)
{
	unsigned code;

	/* Searches the table for the character. */
	for (code = 32; code < 256; code++) {
		if (pdf_encoding_mac_roman[code] == unicode)
			return code;
	}

	/* The character has no MacRoman code. */
	return 0;
}

/*
 * Reads a simple font's /FirstChar and /Widths into the codes' widths in
 * text space; codes outside them take the descriptor's /MissingWidth,
 * and a font without /Widths (a standard 14 font) takes the face's
 * advances, except that Courier is 600 wide throughout.
 */
static void
read_simple_widths(
	struct pdf_document *document,
	struct pdf_object *dictionary,
	struct pdf_object *descriptor,
	struct pdf_font *font)
{
	struct pdf_object *first_object;
	struct pdf_object *widths;
	struct pdf_object *base_font;
	struct pdf_object *missing_object;
	double first;
	double missing;
	double width;
	double scale;
	size_t index;
	long code;
	int is_courier;
	int error;

	/* Glyph-space widths turn into text space by the font's scale. */
	scale = 0.001;
	if (font->kind == FONT_KIND_TYPE3)
		scale = font->type3_scale;

	/* Without widths, every code takes the face's advance. */
	for (code = 0; code < 256; code++)
		font->code_widths[code] = -1.0;

	/* Courier's advance is known without its widths. */
	error = pdf_reader_resolve_key(document, dictionary, "BaseFont", &base_font);
	is_courier = 0;
	if (error == 0)
		is_courier = name_contains(base_font, "Courier");
	if (is_courier) {
		for (code = 0; code < 256; code++)
			font->code_widths[code] = 0.6;
	}

	/* Finds the widths and the code of the first. */
	error = pdf_reader_resolve_key(document, dictionary, "Widths", &widths);
	if (error != 0 || widths->type != PDF_OBJECT_ARRAY)
		return;
	error = pdf_reader_resolve_key(document, dictionary, "FirstChar", &first_object);
	if (error != 0)
		return;
	error = pdf_object_number(first_object, &first);
	if (error != 0)
		first = 0.0;
	if (!(first >= 0.0 && first < 256.0))
		first = 0.0;

	/* The missing width covers the codes the widths do not. */
	missing = 0.0;
	if (descriptor != NULL && descriptor->type == PDF_OBJECT_DICTIONARY) {
		error = pdf_reader_resolve_key(document, descriptor, "MissingWidth", &missing_object);
		if (error == 0)
			(void)pdf_object_number(missing_object, &missing);
	}

	/* Every code starts at the missing width. */
	for (code = 0; code < 256; code++)
		font->code_widths[code] = missing * scale;

	/* Each width, for its code. */
	for (index = 0; index < widths->count; index++) {
		code = (long)first + (long)index;
		if (code > 255)
			break;
		error = read_number(document, widths->values[index], &width);
		if (error != 0)
			continue;
		if (!(width > -1e6 && width < 1e6))
			continue;
		font->code_widths[code] = width * scale;
	}
}

/*
 * Reads a CIDFont's /DW and /W into the CIDs' widths in text space.
 *
 * /W holds runs of the form c [w1 w2 ...] and c_first c_last w; CIDs past
 * FONT_CID_MAX are ignored.
 */
static int
read_cid_widths(
	struct pdf_document *document,
	struct pdf_object *cid_font,
	struct pdf_font *font)
{
	struct pdf_object *default_object;
	struct pdf_object *widths;
	struct pdf_object *entry;
	struct pdf_object *next;
	struct pdf_object *list;
	double number;
	double last;
	double width;
	size_t index;
	size_t item;
	unsigned long cid;
	unsigned long highest;
	unsigned long end;
	int error;

	/* The default width, 1000 unless the font says. */
	font->default_width = 1.0;
	error = pdf_reader_resolve_key(document, cid_font, "DW", &default_object);
	if (error == 0) {
		error = pdf_object_number(default_object, &number);
		if (error != 0)
			number = 1000.0;
		if (number > -1e6 && number < 1e6)
			font->default_width = number * 0.001;
	}

	/* Finds the width runs. */
	error = pdf_reader_resolve_key(document, cid_font, "W", &widths);
	if (error != 0)
		return 0;
	if (widths->type != PDF_OBJECT_ARRAY || widths->count == 0)
		return 0;

	/* Keeps a width for every CID up to the limit, each starting at the default. */
	font->cid_widths_count = FONT_CID_MAX + 1;
	font->cid_widths = malloc(font->cid_widths_count * sizeof(*font->cid_widths));
	if (font->cid_widths == NULL)
		return ENOMEM;

	/* Every CID starts at the default. */
	for (cid = 0; cid < font->cid_widths_count; cid++)
		font->cid_widths[cid] = (float)font->default_width;

	/* Reads each run. */
	highest = 0;
	index = 0;
	while (index + 1 < widths->count && index < FONT_ARRAY_MAX) {
		/* The run's first CID. */
		error = read_number(document, widths->values[index], &number);
		if (error != 0)
			break;
		if (!(number >= 0.0 && number <= (double)FONT_CID_MAX))
			break;
		cid = (unsigned long)number;
		error = pdf_reader_resolve(document, widths->values[index + 1], &next);
		if (error != 0)
			break;

		/* c [w1 w2 ...]: consecutive CIDs from c. */
		if (next->type == PDF_OBJECT_ARRAY) {
			list = next;
			for (item = 0; item < list->count && cid + item <= FONT_CID_MAX; item++) {
				error = read_number(document, list->values[item], &width);
				if (error != 0)
					continue;
				if (!(width > -1e6 && width < 1e6))
					continue;
				font->cid_widths[cid + item] = (float)(width * 0.001);
				if (cid + item > highest)
					highest = cid + item;
			}

			/* The run is read; the next starts after its array. */
			index += 2;
			continue;
		}

		/* c_first c_last w: one width for a range. */
		if (index + 2 >= widths->count)
			break;
		error = pdf_object_number(next, &last);
		if (error != 0)
			break;
		if (!(last >= 0.0))
			break;
		error = pdf_reader_resolve(document, widths->values[index + 2], &entry);
		if (error != 0)
			break;

		/* A width that is not a number skips the range. */
		error = pdf_object_number(entry, &width);
		if (error != 0)
			width = 1e9;
		if (!(width > -1e6 && width < 1e6)) {
			index += 3;
			continue;
		}

		/* Each CID of the range, up to the last one kept. */
		end = FONT_CID_MAX;
		if (last < (double)FONT_CID_MAX)
			end = (unsigned long)last;
		for (; cid <= end; cid++)
			font->cid_widths[cid] = (float)(width * 0.001);
		if (end > highest)
			highest = end;
		index += 3;
	}

	/* Keeps only the widths the runs reach; the rest are the default. */
	font->cid_widths_count = highest + 1;

	/* Succeeded: the widths are read. */
	return 0;
}

/*
 * Reads a vertical CIDFont's /DW2: the vertical origin and advance, 880
 * and -1000 by default.
 */
static void
read_vertical_metrics(
	struct pdf_document *document,
	struct pdf_object *cid_font,
	struct pdf_font *font)
{
	struct pdf_object *metrics;
	double origin;
	double advance;
	int error;

	/* The defaults of PDF. */
	font->vertical_origin = 0.88;
	font->vertical_advance = -1.0;

	/* The font's own pair. */
	error = pdf_reader_resolve_key(document, cid_font, "DW2", &metrics);
	if (error != 0)
		return;
	if (metrics->type != PDF_OBJECT_ARRAY || metrics->count != 2)
		return;
	error = read_number(document, metrics->values[0], &origin);
	if (error != 0)
		return;
	error = read_number(document, metrics->values[1], &advance);
	if (error != 0)
		return;
	if (!(origin > -1e6 && origin < 1e6))
		return;
	if (!(advance > -1e6 && advance < 1e6))
		return;

	/* The pair, in text space. */
	font->vertical_origin = origin * 0.001;
	font->vertical_advance = advance * 0.001;
}

/*
 * Reads a CIDFontType2's /CIDToGIDMap: the identity (the default), or a
 * stream of two-byte glyph numbers indexed by CID.
 */
static int
read_cid_map(
	struct pdf_document *document,
	struct pdf_object *cid_font,
	struct pdf_font *font)
{
	struct pdf_object *map;
	const unsigned char *data;
	unsigned char *owned;
	size_t size;
	int dct;
	int error;

	/* The identity, unless the font has a stream. */
	error = pdf_reader_resolve_key(document, cid_font, "CIDToGIDMap", &map);
	if (error != 0)
		return 0;
	if (map->type != PDF_OBJECT_STREAM)
		return 0;

	/* Decodes the stream; one that cannot be decoded leaves the identity. */
	error = pdf_filter_decode(document, map, 0, &data, &size, &owned, &dct);
	if (error == ENOMEM)
		return ENOMEM;
	if (error != 0) {
		font->status |= PDF_DISPLAY_DAMAGED;
		return 0;
	}

	/* Succeeded: the map, which lives with the font. */
	font->cid_map = data;
	font->cid_map_size = size;
	font->cid_map_owned = owned;
	return 0;
}

/* Reads a font descriptor's /Flags (0 when it has none). */
static long
descriptor_flags(
	struct pdf_document *document,
	struct pdf_object *descriptor)
{
	struct pdf_object *flags;
	int error;

	/* A missing descriptor or flags is 0. */
	if (descriptor->type != PDF_OBJECT_DICTIONARY)
		return 0;
	error = pdf_reader_resolve_key(document, descriptor, "Flags", &flags);
	if (error != 0)
		return 0;
	if (flags->type != PDF_OBJECT_INTEGER)
		return 0;

	/* The flags. */
	return flags->integer;
}

/* Reads a number that may be a reference. */
static int
read_number(
	struct pdf_document *document,
	struct pdf_object *object,
	double *number)
{
	struct pdf_object *resolved;
	int error;

	/* Resolves the object. */
	error = pdf_reader_resolve(document, object, &resolved);
	if (error != 0)
		return error;

	/* Reads its value. */
	error = pdf_object_number(resolved, number);
	if (error != 0)
		return error;

	/* Succeeded: number holds the value. */
	return 0;
}

/* Tells whether a name contains a part, ignoring case. */
static int
name_contains(
	const struct pdf_object *name,
	const char *part)
{
	size_t length;
	size_t start;
	size_t index;
	int left;
	int right;

	/* Only a name has bytes to search. */
	if (name == NULL || name->type != PDF_OBJECT_NAME)
		return 0;
	length = strlen(part);
	if (length > name->length)
		return 0;

	/* Tries each position of the name. */
	for (start = 0; start + length <= name->length; start++) {
		for (index = 0; index < length; index++) {
			left = name->bytes[start + index];
			right = (unsigned char)part[index];
			if (left >= 'A' && left <= 'Z')
				left += 'a' - 'A';
			if (right >= 'A' && right <= 'Z')
				right += 'a' - 'A';
			if (left != right)
				break;
		}

		/* Every byte matched at this position. */
		if (index == length)
			return 1;
	}

	/* The name does not contain the part. */
	return 0;
}

/*
 * Chooses the substitute's family and style from the font's name
 * (/BaseFont, without a subset's prefix, matters only through what it
 * contains) and its descriptor's flags.
 */
static void
choose_family(
	struct pdf_document *document,
	struct pdf_object *dictionary,
	struct pdf_object *descriptor,
	enum font_family *family,
	int *bold,
	int *italic)
{
	struct pdf_object *base_font;
	struct pdf_object *weight;
	double weight_value;
	long flags;
	int found;
	int error;

	/* The name and the flags. */
	error = pdf_reader_resolve_key(document, dictionary, "BaseFont", &base_font);
	if (error != 0)
		base_font = NULL;
	flags = descriptor_flags(document, descriptor);

	/* TeX's fonts say their family and style in a code of their own. */
	found = tex_family(base_font, family, bold, italic);
	if (found)
		return;

	/* Monospace by name or fixed pitch, serif by name or flag, sans otherwise. */
	*family = FONT_FAMILY_SANS;
	if ((flags & FONT_FLAG_SERIF) != 0)
		*family = FONT_FAMILY_SERIF;
	found = name_contains(base_font, "Times");
	if (found)
		*family = FONT_FAMILY_SERIF;
	found = name_contains(base_font, "Serif");
	if (found)
		*family = FONT_FAMILY_SERIF;
	found = name_contains(base_font, "Sans");
	if (found)
		*family = FONT_FAMILY_SANS;
	found = name_contains(base_font, "Arial");
	if (found)
		*family = FONT_FAMILY_SANS;
	found = name_contains(base_font, "Helvetica");
	if (found)
		*family = FONT_FAMILY_SANS;
	if ((flags & FONT_FLAG_FIXED) != 0)
		*family = FONT_FAMILY_MONO;
	found = name_contains(base_font, "Courier");
	if (found)
		*family = FONT_FAMILY_MONO;
	found = name_contains(base_font, "Mono");
	if (found)
		*family = FONT_FAMILY_MONO;

	/* Bold by name, flag or weight. */
	*bold = 0;
	if ((flags & FONT_FLAG_FORCE_BOLD) != 0)
		*bold = 1;
	found = name_contains(base_font, "Bold");
	if (found)
		*bold = 1;
	found = name_contains(base_font, "Black");
	if (found)
		*bold = 1;
	found = name_contains(base_font, "Heavy");
	if (found)
		*bold = 1;
	if (descriptor->type == PDF_OBJECT_DICTIONARY) {
		error = pdf_reader_resolve_key(document, descriptor, "FontWeight", &weight);
		if (error == 0) {
			error = pdf_object_number(weight, &weight_value);
			if (error != 0)
				weight_value = 400.0;
			if (weight_value >= 600.0)
				*bold = 1;
		}
	}

	/* Italic by name or flag. */
	*italic = 0;
	if ((flags & FONT_FLAG_ITALIC) != 0)
		*italic = 1;
	found = name_contains(base_font, "Italic");
	if (found)
		*italic = 1;
	found = name_contains(base_font, "Oblique");
	if (found)
		*italic = 1;
}

/*
 * Reads the family and style of a font of TeX (Computer Modern, cm-super,
 * Latin Modern) from its name: CMR10, CMBX12, CMTI9, CMSS10, CMTT8,
 * SFRM1000, LMRoman10-Bold.  Reports whether the name is one of them.
 */
static int
tex_family(
	const struct pdf_object *name,
	enum font_family *family,
	int *bold,
	int *italic)
{
	const unsigned char *bytes;
	size_t length;
	size_t index;
	int differs;
	int found;

	/* Leaves out a subset's prefix: six capitals and a plus sign. */
	if (name == NULL || name->type != PDF_OBJECT_NAME)
		return 0;
	bytes = name->bytes;
	length = name->length;
	if (length > 7 && bytes[6] == '+') {
		bytes += 7;
		length -= 7;
	}

	/* Latin Modern names its family and style in words. */
	differs = 1;
	if (length > 2)
		differs = memcmp(bytes, "LM", 2);
	if (differs == 0) {
		*family = FONT_FAMILY_SERIF;
		*bold = 0;
		*italic = 0;
		found = name_contains(name, "Sans");
		if (found)
			*family = FONT_FAMILY_SANS;
		found = name_contains(name, "Mono");
		if (found)
			*family = FONT_FAMILY_MONO;
		found = name_contains(name, "Bold");
		if (found)
			*bold = 1;
		found = name_contains(name, "Italic");
		if (found)
			*italic = 1;
		found = name_contains(name, "Oblique");
		if (found)
			*italic = 1;
		return 1;
	}

	/* Computer Modern and cm-super: CM or SF, then two or three letters of family and style, then the size. */
	if (length < 4)
		return 0;
	differs = memcmp(bytes, "CM", 2);
	if (differs != 0)
		differs = memcmp(bytes, "SF", 2);
	if (differs != 0)
		return 0;

	/* The capitals end at the size's first digit. */
	for (index = 2; index < length; index++) {
		if (bytes[index] < 'A' || bytes[index] > 'Z')
			break;
	}

	/* A name of capitals only, or without the size, is not TeX's. */
	if (index == length)
		return 0;
	if (bytes[index] < '0' || bytes[index] > '9')
		return 0;

	/* The letters: SS sans, TT and VTT monospace, BX and B bold, TI, SL, MI and IT italic. */
	*family = FONT_FAMILY_SERIF;
	*bold = 0;
	*italic = 0;
	found = name_contains(name, "SS");
	if (found)
		*family = FONT_FAMILY_SANS;
	found = name_contains(name, "TT");
	if (found)
		*family = FONT_FAMILY_MONO;
	found = name_contains(name, "BX");
	if (found)
		*bold = 1;
	found = name_contains(name, "TI");
	if (found)
		*italic = 1;
	found = name_contains(name, "SL");
	if (found)
		*italic = 1;
	found = name_contains(name, "MI");
	if (found)
		*italic = 1;

	/* The name is TeX's. */
	return 1;
}

/* Finds the glyph of a composite font's CID. */
static unsigned
composite_glyph(
	const struct pdf_font *font,
	unsigned cid)
{
	size_t position;
	unsigned glyph;
	int cid_keyed;
	int error;

	/* A CID-keyed CFF program's charset gives each CID its glyph; another CFF program's glyphs are the CIDs. */
	if (font->charstrings != NULL) {
		cid_keyed = pdf_charstrings_cid_keyed(font->charstrings);
		if (!cid_keyed)
			return cid;
		error = pdf_charstrings_cid(font->charstrings, cid, &glyph);
		if (error != 0)
			return 0;
		return glyph;
	}

	/* The identity when the font has no map. */
	if (font->cid_map == NULL)
		return cid;

	/* A CID past the map has no glyph. */
	position = (size_t)cid * 2;
	if (position + 2 > font->cid_map_size)
		return 0;

	/* The map's two bytes, the high one first. */
	return ((unsigned)font->cid_map[position] << 8) | font->cid_map[position + 1];
}

/* Reports a glyph's advance in the face, in ems (0 when unknown). */
static double
face_advance(
	struct pdf_font *font,
	unsigned glyph)
{
	struct glyph_slot *slot;
	const char *letters;
	unsigned letter;
	double total;
	int advance;
	int error;

	/* A Type 1 or CFF program's advance comes with the glyph's outline. */
	if (font->charstrings != NULL) {
		error = find_outline(font, glyph, &slot);
		if (error != 0)
			return 0.0;
		return slot->advance;
	}

	/* A font without a face has no advances. */
	if (font->face == NULL)
		return 0.0;

	/* A ligature drawn as its letters advances by all of them. */
	letters = ligature_letters(glyph);
	if (letters != NULL) {
		total = 0.0;
		for (; *letters != '\0'; letters++) {
			letter = truetype_glyph_index(font->face, (unsigned char)*letters);
			total += face_advance(font, letter);
		}

		/* The letters' advances together. */
		return total;
	}

	/* The face's advance, in design units. */
	error = truetype_glyph_design_advance(font->face, glyph, &advance);
	if (error != 0)
		return 0.0;

	/* In ems. */
	return (double)advance / font->units_per_em;
}

/*
 * Finds a glyph's outline among those the font keeps, reading it the
 * first time.  A glyph the face does not have is an empty outline.
 */
static int
find_outline(
	struct pdf_font *font,
	unsigned glyph,
	struct glyph_slot **slot)
{
	struct glyph_slot *found;
	size_t index;
	size_t mask;
	int error;

	/* Makes room in the table, which is kept at most half full. */
	if ((font->slots_count + 1) * 2 > font->slots_capacity) {
		error = grow_slots(font);
		if (error != 0)
			return error;
	}

	/* Looks the glyph up from its hash, stepping to the next slot on a collision. */
	mask = font->slots_capacity - 1;
	index = ((size_t)glyph * 2654435761UL) & mask;
	for (;;) {
		found = &font->slots[index];
		if (!found->used)
			break;
		if (found->glyph == glyph) {
			*slot = found;
			return 0;
		}

		/* A collision: the next slot. */
		index = (index + 1) & mask;
	}

	/* Starts over when the kept outlines have grown past the limit. */
	if (font->points_count > FONT_CACHE_POINTS_MAX) {
		memset(font->slots, 0, font->slots_capacity * sizeof(*font->slots));
		font->slots_count = 0;
		font->verbs_count = 0;
		font->points_count = 0;
		index = ((size_t)glyph * 2654435761UL) & mask;
		found = &font->slots[index];
	}

	/* Reads the outline into the slot. */
	error = read_outline(font, glyph, found);
	if (error != 0)
		return error;
	found->used = 1;
	found->glyph = glyph;
	font->slots_count++;

	/* Succeeded: the slot holds the glyph's outline. */
	*slot = found;
	return 0;
}

/*
 * Reads a glyph's outline from the face into the font's path arrays: the
 * quadratic contours as cubic curves, in ems with y upward.  A ligature
 * drawn as its letters is each letter's outline after the one before.
 */
static int
read_outline(
	struct pdf_font *font,
	unsigned glyph,
	struct glyph_slot *slot)
{
	const char *letters;
	unsigned letter;
	double offset;
	double advance;
	int error;

	/* The outline starts where the arrays end. */
	slot->verb_start = font->verbs_count;
	slot->point_start = font->points_count;
	slot->verb_count = 0;
	slot->point_count = 0;
	slot->advance = 0.0;

	/* Appends the glyph, or each letter of a ligature (only a substitute has them). */
	letters = ligature_letters(glyph);
	if (letters == NULL) {
		error = append_glyph(font, glyph, 0.0, &slot->advance);
	} else {
		error = 0;
		offset = 0.0;
		for (; *letters != '\0' && error == 0; letters++) {
			letter = truetype_glyph_index(font->face, (unsigned char)*letters);
			error = append_glyph(font, letter, offset, &advance);
			offset += face_advance(font, letter);
		}
	}

	/* A failure leaves the arrays as they were. */
	if (error != 0) {
		font->verbs_count = slot->verb_start;
		font->points_count = slot->point_start;
		return error;
	}

	/* Succeeded: the outline is the arrays' newest part. */
	slot->verb_count = font->verbs_count - slot->verb_start;
	slot->point_count = font->points_count - slot->point_start;
	return 0;
}

/* Tells the letters a ligature glyph stands for (NULL for a glyph of the face). */
static const char *
ligature_letters(
	unsigned glyph)
{
	static const char *const letters[5] = { "ff", "fi", "fl", "ffi", "ffl" };

	/* Only the numbers past the face's are ligatures. */
	if (glyph < FONT_LIGATURE_GLYPH + 0xFB00 || glyph > FONT_LIGATURE_GLYPH + 0xFB04)
		return NULL;

	/* The ligature's letters. */
	return letters[glyph - FONT_LIGATURE_GLYPH - 0xFB00];
}

/*
 * Appends one glyph's outline from the face or the program, moved right
 * by offset ems (a program's glyphs are never moved: only a substitute
 * draws ligatures as letters).  *advance is a program glyph's advance in
 * ems.  A glyph the face cannot give adds nothing.
 */
static int
append_glyph(
	struct pdf_font *font,
	unsigned glyph,
	double offset,
	double *advance)
{
	struct truetype_glyph_outline outline;
	struct truetype_outline_point *points;
	unsigned *ends;
	int error;

	/* A Type 1 or CFF program runs the glyph's charstring into the arrays. */
	*advance = 0.0;
	if (font->charstrings != NULL) {
		error = pdf_charstrings_outline(font->charstrings, glyph, emit_program_step, font, advance);
		if (error != 0)
			return error;
		return 0;
	}

	/* Reads the contours into the font's scratch arrays, growing them once when they are short. */
	memset(&outline, 0, sizeof(outline));
	outline.points = font->outline_points;
	outline.point_capacity = font->outline_points_capacity;
	outline.contour_ends = font->outline_ends;
	outline.contour_capacity = font->outline_ends_capacity;
	error = truetype_glyph_outline(font->face, glyph, &outline);
	if (error == ENOSPC) {
		/* Grows the points to what the glyph needs. */
		if (outline.point_count > font->outline_points_capacity) {
			points = realloc(font->outline_points, outline.point_count * sizeof(*points));
			if (points == NULL)
				return ENOMEM;
			font->outline_points = points;
			font->outline_points_capacity = outline.point_count;
		}

		/* Grows the contour ends to what the glyph needs. */
		if (outline.contour_count > font->outline_ends_capacity) {
			ends = realloc(font->outline_ends, outline.contour_count * sizeof(*ends));
			if (ends == NULL)
				return ENOMEM;
			font->outline_ends = ends;
			font->outline_ends_capacity = outline.contour_count;
		}

		/* Reads again into the grown arrays. */
		memset(&outline, 0, sizeof(outline));
		outline.points = font->outline_points;
		outline.point_capacity = font->outline_points_capacity;
		outline.contour_ends = font->outline_ends;
		outline.contour_capacity = font->outline_ends_capacity;
		error = truetype_glyph_outline(font->face, glyph, &outline);
	}

	/* A glyph the face cannot give is drawn as nothing. */
	if (error != 0)
		return 0;

	/* Converts the contours. */
	error = convert_contours(font, &outline, offset);
	if (error != 0)
		return error;

	/* Succeeded: the glyph's outline is appended. */
	return 0;
}

/*
 * Converts TrueType contours into path steps: each contour starts at an
 * on-curve point (or between two control points), a control point
 * between on-curve points is a quadratic curve raised to a cubic one, and
 * two control points in a row have an on-curve point halfway between.
 */
static int
convert_contours(
	struct pdf_font *font,
	const struct truetype_glyph_outline *outline,
	double offset)
{
	const struct truetype_outline_point *points;
	double scale;
	double start[2];
	double current[2];
	double control[2];
	double point[2];
	double target[2];
	double curve[6];
	unsigned contour;
	unsigned first;
	unsigned last;
	unsigned count;
	unsigned step;
	unsigned index;
	int pending;
	int on_curve;
	int error;

	/* Design units become ems. */
	scale = 1.0 / font->units_per_em;
	points = outline->points;

	/* Defines control coordinates before any pending curve (gcc dataflow). */
	control[0] = 0.0;
	control[1] = 0.0;

	/* Converts each contour. */
	first = 0;
	for (contour = 0; contour < outline->contour_count; contour++) {
		/* The contour's points; a malformed end ends the outline. */
		last = outline->contour_ends[contour];
		if (last < first || last >= outline->point_count)
			break;
		count = last - first + 1;

		/*
		 * The start: the first point when it is on the curve, else the last
		 * when it is, else halfway between the two control points.
		 */
		index = 0;
		if (points[first].on_curve) {
			start[0] = points[first].x;
			start[1] = points[first].y;
			index = 1;
		} else if (points[last].on_curve) {
			start[0] = points[last].x;
			start[1] = points[last].y;
			count--;
		} else {
			start[0] = (points[first].x + points[last].x) / 2.0;
			start[1] = (points[first].y + points[last].y) / 2.0;
		}

		/* Moves to the start, in ems. */
		start[0] = start[0] * scale + offset;
		start[1] *= scale;
		error = emit(font, PDF_PATH_MOVE, start, 1);
		if (error != 0)
			return error;
		current[0] = start[0];
		current[1] = start[1];

		/* Walks the rest of the points, then back to the start. */
		pending = 0;
		error = 0;
		for (step = index; step <= count; step++) {
			/* The point, or the start again after the last. */
			if (step == count) {
				target[0] = start[0];
				target[1] = start[1];
				on_curve = 1;
			} else {
				target[0] = points[first + step].x * scale + offset;
				target[1] = points[first + step].y * scale;
				on_curve = (int)points[first + step].on_curve;
			}

			/* A control point waits for the point after it. */
			if (!on_curve) {
				if (pending) {
					/* Two control points: the curve ends halfway between them. */
					point[0] = (control[0] + target[0]) / 2.0;
					point[1] = (control[1] + target[1]) / 2.0;
					curve[0] = current[0] + 2.0 / 3.0 * (control[0] - current[0]);
					curve[1] = current[1] + 2.0 / 3.0 * (control[1] - current[1]);
					curve[2] = point[0] + 2.0 / 3.0 * (control[0] - point[0]);
					curve[3] = point[1] + 2.0 / 3.0 * (control[1] - point[1]);
					curve[4] = point[0];
					curve[5] = point[1];
					error = emit(font, PDF_PATH_CUBIC, curve, 3);
					if (error != 0)
						return error;
					current[0] = point[0];
					current[1] = point[1];
				}

				/* The point is the control point of the next curve. */
				control[0] = target[0];
				control[1] = target[1];
				pending = 1;
				continue;
			}

			/* An on-curve point ends a curve through the waiting control point, or a line. */
			if (pending) {
				curve[0] = current[0] + 2.0 / 3.0 * (control[0] - current[0]);
				curve[1] = current[1] + 2.0 / 3.0 * (control[1] - current[1]);
				curve[2] = target[0] + 2.0 / 3.0 * (control[0] - target[0]);
				curve[3] = target[1] + 2.0 / 3.0 * (control[1] - target[1]);
				curve[4] = target[0];
				curve[5] = target[1];
				error = emit(font, PDF_PATH_CUBIC, curve, 3);
				pending = 0;
			} else if (step != count) {
				error = emit(font, PDF_PATH_LINE, target, 1);
			}

			/* The point is where the next step starts. */
			if (error != 0)
				return error;
			current[0] = target[0];
			current[1] = target[1];
		}

		/* Closes the contour. */
		error = emit(font, PDF_PATH_CLOSE, start, 0);
		if (error != 0)
			return error;
		first = last + 1;
	}

	/* Succeeded: every contour is a closed subpath. */
	return 0;
}

/* Appends one step of a charstring's outline to the font's arrays (the sink the charstring fonts draw into). */
static int
emit_program_step(
	void *context,
	enum pdf_path_verb verb,
	const double *coordinates,
	size_t count)
{
	int error;

	/* The font is the context. */
	error = emit((struct pdf_font *)context, verb, coordinates, count);
	if (error != 0)
		return error;

	/* Succeeded: the step is appended. */
	return 0;
}

/* Appends one path step to the font's arrays. */
static int
emit(
	struct pdf_font *font,
	enum pdf_path_verb verb,
	const double *coordinates,
	size_t count)
{
	unsigned char *verbs;
	struct pdf_point *points;
	size_t capacity;
	size_t index;

	/* Grows the steps when full. */
	if (font->verbs_count == font->verbs_capacity) {
		capacity = font->verbs_capacity * 2 + 256;
		verbs = realloc(font->verbs, capacity);
		if (verbs == NULL)
			return ENOMEM;
		font->verbs = verbs;
		font->verbs_capacity = capacity;
	}

	/* Grows the points when they do not fit. */
	if (font->points_count + count > font->points_capacity) {
		capacity = font->points_capacity * 2 + 1024;
		points = realloc(font->points, capacity * sizeof(*points));
		if (points == NULL)
			return ENOMEM;
		font->points = points;
		font->points_capacity = capacity;
	}

	/* Appends the step and its points. */
	font->verbs[font->verbs_count] = (unsigned char)verb;
	font->verbs_count++;
	for (index = 0; index < count; index++) {
		font->points[font->points_count].x = coordinates[index * 2];
		font->points[font->points_count].y = coordinates[index * 2 + 1];
		font->points_count++;
	}

	/* Succeeded: the step is the arrays' last. */
	return 0;
}

/* Doubles the glyph table and puts every kept glyph back in its place. */
static int
grow_slots(
	struct pdf_font *font)
{
	struct glyph_slot *old;
	struct glyph_slot *slot;
	size_t old_capacity;
	size_t capacity;
	size_t index;
	size_t position;

	/* Allocates the larger, empty table. */
	capacity = font->slots_capacity * 2;
	if (capacity == 0)
		capacity = 64;
	old = font->slots;
	old_capacity = font->slots_capacity;
	font->slots = calloc(capacity, sizeof(*font->slots));
	if (font->slots == NULL) {
		font->slots = old;
		return ENOMEM;
	}

	/* The new table is the font's. */
	font->slots_capacity = capacity;

	/* Moves each kept glyph to its place in the new table. */
	for (index = 0; index < old_capacity; index++) {
		if (!old[index].used)
			continue;
		position = ((size_t)old[index].glyph * 2654435761UL) & (capacity - 1);
		for (;;) {
			slot = &font->slots[position];
			if (!slot->used)
				break;
			position = (position + 1) & (capacity - 1);
		}

		/* The glyph's slot in the new table. */
		*slot = old[index];
	}

	/* The old table goes. */
	free(old);

	/* Succeeded: the table has room. */
	return 0;
}

/* Frees one font and everything it owns. */
static void
free_font(
	struct pdf_font *font)
{
	/* The face or charstrings over the program, then the program's bytes, then the tables. */
	truetype_close(font->owned_face);
	pdf_charstrings_close(font->charstrings);
	free(font->program);
	free(font->cid_widths);
	free(font->cid_map_owned);
	free(font->slots);
	free(font->verbs);
	free(font->points);
	free(font->outline_points);
	free(font->outline_ends);
	pdf_tounicode_free(font->tounicode);
	free(font->glyph_unicode);
	free(font);
}

/*
 * Reads a font's /ToUnicode CMap, when it has one that can be decoded.
 * Returns 0 (a CMap that cannot be read leaves the font without one) or
 * ENOMEM.
 */
static int
font_read_tounicode(
	struct pdf_document *document,
	struct pdf_font *font)
{
	struct pdf_object *stream;
	const unsigned char *data;
	unsigned char *owned;
	size_t size;
	int dct;
	int error;

	/* The stream the font's dictionary names. */
	if (document == NULL || font->dictionary == NULL)
		return 0;
	error = pdf_reader_resolve_key(document, font->dictionary, "ToUnicode", &stream);
	if (error != 0 || stream->type != PDF_OBJECT_STREAM)
		return 0;

	/* Its decoded bytes, read as a CMap. */
	error = pdf_filter_decode(document, stream, 0, &data, &size, &owned, &dct);
	if (error == ENOMEM)
		return ENOMEM;
	if (error != 0)
		return 0;
	error = pdf_tounicode_parse(data, size, &font->tounicode);
	free(owned);
	if (error == ENOMEM)
		return ENOMEM;

	/* Read (or not a CMap: left without one). */
	return 0;
}

/*
 * Reads a composite font's TrueType character map backward: for each
 * character of the Basic Multilingual Plane the program maps, its glyph's
 * character (the first, from U+0020 up, then the C0 controls left out).
 * Returns 0 or ENOMEM.
 */
static int
font_build_glyph_unicode(
	struct pdf_font *font)
{
	uint32_t *table;
	unsigned character;
	unsigned glyph;
	size_t count;

	/* Room for every glyph a 16-bit index names. */
	count = 65536U;
	table = calloc(count, sizeof(*table));
	if (table == NULL)
		return ENOMEM;

	/* Each character, its glyph's first one kept. */
	for (character = 0x20U; character <= 0xffffU; character++) {
		/* Not the surrogates, which no character map names. */
		if (character >= 0xd800U && character <= 0xdfffU)
			continue;
		glyph = truetype_glyph_index(font->face, character);
		if (glyph != 0U && glyph < count && table[glyph] == 0U)
			table[glyph] = character;
	}

	/* Succeeded: the table. */
	font->glyph_unicode = table;
	font->glyph_unicode_count = count;
	return 0;
}
