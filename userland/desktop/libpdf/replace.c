/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The replacement fonts of the editor (ws175-p005, plan/ws175/phase001/
 * design.md section 4.1, the fonts as the user chose them on 2026-10-06):
 * the desktop's Mahora Regular (Sans) and Mahora Mono (Mono), and the two
 * fallbacks that draw what a font lacks, JetBrains Mono and then Droid Sans
 * Fallback (which is also the font of Japanese, CJK).  They are the files
 * the desktop installs in PDF_FONT_DIRECTORY.
 *
 * A document keeps them for its editors (pdf_edit_fonts_of): each file's
 * bytes, its face (libtruetype: the characters' glyphs and their advances)
 * and the font objects the preview draws it through -- a Type0 font of the
 * whole file, its glyphs named by their numbers (Identity-H, CIDToGIDMap
 * /Identity) and its widths the face's own (/DW -1) -- in an arena of
 * their own, freed when the document closes: the font cache knows a font
 * by its dictionary, so a preview's font must live as long as the
 * document (design.md [H5]).  A writer reads a file again for itself
 * (pdf_edit_font_read), to embed its subset.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pdf.h>
#include <truetype/truetype.h>

#include "userland/desktop/paths.h"
#include "internal.h"

/* Where the desktop's fonts are (font.c's substitutes too; a host test names its own folder). */
#ifndef PDF_FONT_DIRECTORY
#define PDF_FONT_DIRECTORY KEILAND_DATADIR "/fonts"
#endif
#ifndef PDF_EDIT_FONT_DIRECTORY
#define PDF_EDIT_FONT_DIRECTORY PDF_FONT_DIRECTORY
#endif

/* The largest font file read. */
#define REPLACE_FILE_MAX	((size_t)64 * 1024 * 1024)

/* The files and the PostScript names the fonts are embedded under. */
static const char *const replace_files[PDF_EDIT_FILES] = {
	"keiland.ttf", "keiland-mono.ttf", "keiland-fallback-mono.ttf", "keiland-fallback.ttf"
};
static const char *const replace_names[PDF_EDIT_FILES] = {
	"Mahora-Regular", "Mahora-Mono", "JetBrainsMono-Regular", "DroidSansFallback"
};

/* One file: whether it was tried, its bytes and face (NULL: none), its em, and the preview's font. */
struct replace_file {
	int tried;
	unsigned char *data;
	size_t size;
	struct truetype_face *face;
	double units_per_em;
	struct pdf_object *font;
};

/* A document's replacement fonts. */
struct pdf_edit_fonts {
	struct replace_file files[PDF_EDIT_FILES];
	struct pdf_arena arena;
};

static void replace_release(struct pdf_edit_fonts *fonts);
static int replace_open(struct pdf_edit_fonts *fonts, unsigned file);
static struct pdf_object *replace_object(struct pdf_arena *arena, int type, const char *name, long integer);
static struct pdf_object *replace_dictionary(struct pdf_arena *arena, size_t capacity);
static int replace_put(struct pdf_object *dictionary, size_t capacity, struct pdf_arena *arena, const char *key, struct pdf_object *value);

/*
 * Gives a document's replacement fonts, made the first time (their files
 * read when first asked for).  Returns 0, EINVAL, or ENOMEM.
 */
int
pdf_edit_fonts_of(
	struct pdf_document *document,
	struct pdf_edit_fonts **fonts)
{
	struct pdf_edit_fonts *made;

	/* The document's, when it has them. */
	if (document == NULL || fonts == NULL)
		return EINVAL;
	made = pdf_reader_edit_fonts(document);
	if (made != NULL) {
		*fonts = made;
		return 0;
	}

	/* New, the document's from now. */
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	pdf_reader_set_edit_fonts(document, made, replace_release);
	*fonts = made;
	return 0;
}

/*
 * Finds the glyph of a character in a file (0 and ENOENT when the file
 * is not there or lacks it).  Returns 0, ENOENT, or ENOMEM.
 */
int
pdf_edit_font_glyph(
	struct pdf_edit_fonts *fonts,
	unsigned file,
	uint32_t character,
	unsigned *glyph)
{
	int error;

	/* The file. */
	*glyph = 0;
	error = replace_open(fonts, file);
	if (error != 0)
		return error;

	/* The character's glyph. */
	*glyph = truetype_glyph_index(fonts->files[file].face, character);
	if (*glyph == 0)
		return ENOENT;
	return 0;
}

/* Tells whether a file is installed (it is read the first time). */
int
pdf_edit_font_present(
	struct pdf_edit_fonts *fonts,
	unsigned file)
{
	int error;

	/* Opened, or not there. */
	error = replace_open(fonts, file);
	return error == 0;
}

/* Gives a glyph's advance in ems.  Returns 0, ENOENT, or ENOMEM. */
int
pdf_edit_font_advance(
	struct pdf_edit_fonts *fonts,
	unsigned file,
	unsigned glyph,
	double *ems)
{
	int advance;
	int error;

	/* The file, and the glyph's advance in its units. */
	error = replace_open(fonts, file);
	if (error == 0)
		error = truetype_glyph_design_advance(fonts->files[file].face, glyph, &advance);
	if (error != 0)
		return error;

	/* In ems. */
	*ems = (double)advance / fonts->files[file].units_per_em;
	return 0;
}

/*
 * Gives the font object the preview draws a file through: a Type0 font of
 * the whole file (made the first time, kept with the document).  Returns
 * 0, ENOENT for a file that is not there, or ENOMEM.
 */
int
pdf_edit_font_object(
	struct pdf_edit_fonts *fonts,
	unsigned file,
	struct pdf_object **font)
{
	struct replace_file *entry;
	struct pdf_object *type0;
	struct pdf_object *cid;
	struct pdf_object *info;
	struct pdf_object *descriptor;
	struct pdf_object *program;
	struct pdf_object *descendants;
	struct pdf_arena *arena;
	int error;

	/* The file, and its font made already. */
	error = replace_open(fonts, file);
	if (error != 0)
		return error;
	entry = &fonts->files[file];
	if (entry->font != NULL) {
		*font = entry->font;
		return 0;
	}

	/* The program: a stream of the file's bytes. */
	arena = &fonts->arena;
	program = replace_dictionary(arena, 2);
	if (program == NULL)
		return ENOMEM;
	program->type = PDF_OBJECT_STREAM;
	program->bytes = entry->data;
	program->data_length = entry->size;

	/* The descriptor, the descendant CIDFontType2 (its widths the face's: /DW -1) and the Type0 font. */
	descriptor = replace_dictionary(arena, 4);
	info = replace_dictionary(arena, 4);
	cid = replace_dictionary(arena, 8);
	type0 = replace_dictionary(arena, 8);
	descendants = replace_object(arena, PDF_OBJECT_ARRAY, NULL, 0);
	if (descriptor == NULL || info == NULL || cid == NULL || type0 == NULL || descendants == NULL)
		return ENOMEM;
	descendants->values = pdf_arena_allocate(arena, sizeof(*descendants->values));
	if (descendants->values == NULL)
		return ENOMEM;
	descendants->values[0] = cid;
	descendants->count = 1;
	error = replace_put(descriptor, 4, arena, "Type", replace_object(arena, PDF_OBJECT_NAME, "FontDescriptor", 0));
	if (error == 0)
		error = replace_put(descriptor, 4, arena, "FontName", replace_object(arena, PDF_OBJECT_NAME, replace_names[file], 0));
	if (error == 0)
		error = replace_put(descriptor, 4, arena, "Flags", replace_object(arena, PDF_OBJECT_INTEGER, NULL, 32L));
	if (error == 0)
		error = replace_put(descriptor, 4, arena, "FontFile2", program);
	if (error == 0)
		error = replace_put(info, 4, arena, "Registry", replace_object(arena, PDF_OBJECT_STRING, "Adobe", 0));
	if (error == 0)
		error = replace_put(info, 4, arena, "Ordering", replace_object(arena, PDF_OBJECT_STRING, "Identity", 0));
	if (error == 0)
		error = replace_put(info, 4, arena, "Supplement", replace_object(arena, PDF_OBJECT_INTEGER, NULL, 0L));
	if (error == 0)
		error = replace_put(cid, 8, arena, "Type", replace_object(arena, PDF_OBJECT_NAME, "Font", 0));
	if (error == 0)
		error = replace_put(cid, 8, arena, "Subtype", replace_object(arena, PDF_OBJECT_NAME, "CIDFontType2", 0));
	if (error == 0)
		error = replace_put(cid, 8, arena, "BaseFont", replace_object(arena, PDF_OBJECT_NAME, replace_names[file], 0));
	if (error == 0)
		error = replace_put(cid, 8, arena, "CIDSystemInfo", info);
	if (error == 0)
		error = replace_put(cid, 8, arena, "FontDescriptor", descriptor);
	if (error == 0)
		error = replace_put(cid, 8, arena, "CIDToGIDMap", replace_object(arena, PDF_OBJECT_NAME, "Identity", 0));
	if (error == 0)
		error = replace_put(cid, 8, arena, "DW", replace_object(arena, PDF_OBJECT_INTEGER, NULL, -1L));
	if (error == 0)
		error = replace_put(type0, 8, arena, "Type", replace_object(arena, PDF_OBJECT_NAME, "Font", 0));
	if (error == 0)
		error = replace_put(type0, 8, arena, "Subtype", replace_object(arena, PDF_OBJECT_NAME, "Type0", 0));
	if (error == 0)
		error = replace_put(type0, 8, arena, "BaseFont", replace_object(arena, PDF_OBJECT_NAME, replace_names[file], 0));
	if (error == 0)
		error = replace_put(type0, 8, arena, "Encoding", replace_object(arena, PDF_OBJECT_NAME, "Identity-H", 0));
	if (error == 0)
		error = replace_put(type0, 8, arena, "DescendantFonts", descendants);
	if (error != 0)
		return error;

	/* Succeeded: kept for the document's life. */
	entry->font = type0;
	*font = type0;
	return 0;
}

/*
 * Reads a replacement font's file into a new buffer the caller frees (a
 * writer's own copy, to embed its subset).  Returns 0, ENOENT, EFBIG, or
 * ENOMEM.
 */
int
pdf_edit_font_read(
	unsigned file,
	unsigned char **data,
	size_t *size)
{
	char path[512];
	unsigned char *bytes;
	FILE *stream;
	long length;
	size_t got;

	/* The file. */
	if (file >= PDF_EDIT_FILES)
		return ENOENT;
	(void)snprintf(path, sizeof(path), "%s/%s", PDF_EDIT_FONT_DIRECTORY, replace_files[file]);
	stream = fopen(path, "rb");
	if (stream == NULL)
		return ENOENT;

	/* Its length, within the largest read. */
	(void)fseek(stream, 0L, SEEK_END);
	length = ftell(stream);
	(void)fseek(stream, 0L, SEEK_SET);
	if (length <= 0 || (unsigned long)length > REPLACE_FILE_MAX) {
		fclose(stream);
		return EFBIG;
	}

	/* Its bytes. */
	bytes = malloc((size_t)length);
	got = 0;
	if (bytes != NULL)
		got = fread(bytes, 1, (size_t)length, stream);
	fclose(stream);
	if (bytes == NULL)
		return ENOMEM;
	if (got != (size_t)length) {
		free(bytes);
		return ENOENT;
	}

	/* Succeeded: the caller's copy. */
	*data = bytes;
	*size = got;
	return 0;
}

/* Names a replacement font as it is embedded (its PostScript name). */
const char *
pdf_edit_font_name(
	unsigned file)
{
	/* The file's name, or the first's for a file past them. */
	if (file >= PDF_EDIT_FILES)
		file = 0;
	return replace_names[file];
}

/* Frees a document's replacement fonts. */
static void
replace_release(
	struct pdf_edit_fonts *fonts)
{
	unsigned file;

	/* Nothing to free. */
	if (fonts == NULL)
		return;

	/* Each file's face and bytes, the preview's objects, the record. */
	for (file = 0; file < PDF_EDIT_FILES; file++) {
		truetype_close(fonts->files[file].face);
		free(fonts->files[file].data);
	}

	/* The objects and the record. */
	pdf_arena_free(&fonts->arena);
	free(fonts);
}

/* Reads a file and opens its face, the first time.  Returns 0, ENOENT (not there or not a font), or ENOMEM. */
static int
replace_open(
	struct pdf_edit_fonts *fonts,
	unsigned file)
{
	struct truetype_design_metrics metrics;
	struct replace_file *entry;
	int error;

	/* A file of the list, tried once. */
	if (fonts == NULL || file >= PDF_EDIT_FILES)
		return ENOENT;
	entry = &fonts->files[file];
	if (entry->tried) {
		if (entry->face == NULL)
			return ENOENT;
		return 0;
	}

	/* Tried from now. */
	entry->tried = 1;

	/* Its bytes and face, and its em. */
	error = pdf_edit_font_read(file, &entry->data, &entry->size);
	if (error == ENOMEM)
		return ENOMEM;
	if (error != 0)
		return ENOENT;
	error = truetype_open(entry->data, entry->size, 0, &entry->face);
	if (error == 0)
		error = truetype_design_metrics(entry->face, &metrics);
	if (error != 0 || metrics.units_per_em == 0) {
		truetype_close(entry->face);
		entry->face = NULL;
		free(entry->data);
		entry->data = NULL;
		return ENOENT;
	}

	/* Succeeded: the face. */
	entry->units_per_em = (double)metrics.units_per_em;
	return 0;
}

/* Makes a name (text), a string (text) or an integer object in an arena; NULL when memory runs out. */
static struct pdf_object *
replace_object(
	struct pdf_arena *arena,
	int type,
	const char *name,
	long integer)
{
	struct pdf_object *object;
	unsigned char *bytes;
	size_t length;

	/* The object. */
	object = pdf_arena_allocate(arena, sizeof(*object));
	if (object == NULL)
		return NULL;
	memset(object, 0, sizeof(*object));
	object->type = (enum pdf_object_type)type;
	object->integer = integer;

	/* Its bytes, with the NUL the reader's names have. */
	if (name != NULL) {
		length = strlen(name);
		bytes = pdf_arena_allocate(arena, length + 1U);
		if (bytes == NULL)
			return NULL;
		memcpy(bytes, name, length + 1U);
		object->bytes = bytes;
		object->length = length;
	}

	/* Succeeded: the object. */
	return object;
}

/* Makes an empty dictionary with room for some entries; NULL when memory runs out. */
static struct pdf_object *
replace_dictionary(
	struct pdf_arena *arena,
	size_t capacity)
{
	struct pdf_object *dictionary;

	/* The dictionary and its two arrays. */
	dictionary = replace_object(arena, PDF_OBJECT_DICTIONARY, NULL, 0);
	if (dictionary == NULL)
		return NULL;
	dictionary->keys = pdf_arena_allocate(arena, capacity * sizeof(*dictionary->keys));
	dictionary->values = pdf_arena_allocate(arena, capacity * sizeof(*dictionary->values));
	if (dictionary->keys == NULL || dictionary->values == NULL)
		return NULL;

	/* Succeeded: an empty dictionary. */
	return dictionary;
}

/* Adds an entry to a dictionary made with room for capacity entries.  Returns 0, ENOMEM for a missing object, or ENOSPC. */
static int
replace_put(
	struct pdf_object *dictionary,
	size_t capacity,
	struct pdf_arena *arena,
	const char *key,
	struct pdf_object *value)
{
	struct pdf_object *name;

	/* The key, the value, and room. */
	name = replace_object(arena, PDF_OBJECT_NAME, key, 0);
	if (name == NULL || value == NULL)
		return ENOMEM;
	if (dictionary->count >= capacity)
		return ENOSPC;

	/* Succeeded: the entry. */
	dictionary->keys[dictionary->count] = name;
	dictionary->values[dictionary->count] = value;
	dictionary->count++;
	return 0;
}
