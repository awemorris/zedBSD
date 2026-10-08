/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpdf's private declarations for the reader: the objects a document is
 * made of, the arena they live in, and the lexer and parser that object.c
 * provides to reader.c.  None of these leave the library (exports.map).
 */

#ifndef LIBPDF_INTERNAL_H
#define LIBPDF_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/* The deepest nesting of arrays and dictionaries, and of page and name trees (design-pdf.md section 4.3). */
#define PDF_READER_DEPTH_MAX 32

/* The most bytes the objects of one document may take, the decode limit of design-pdf.md section 4.3. */
#define PDF_READER_ARENA_MAX ((size_t)256 * 1024 * 1024)

/* The longest name, twice what the PDF reference's implementation limits allow. */
#define PDF_READER_NAME_MAX 255

/* The longest number token; anything longer is not a number PDF writes. */
#define PDF_READER_NUMBER_MAX 64

/*
 * The kinds of PDF object.
 */
enum pdf_object_type {
	PDF_OBJECT_NULL = 0,
	PDF_OBJECT_BOOLEAN,
	PDF_OBJECT_INTEGER,
	PDF_OBJECT_REAL,
	PDF_OBJECT_NAME,
	PDF_OBJECT_STRING,
	PDF_OBJECT_ARRAY,
	PDF_OBJECT_DICTIONARY,
	PDF_OBJECT_STREAM,
	PDF_OBJECT_REFERENCE
};

/*
 * The kinds of token the lexer reads.
 */
enum pdf_token_type {
	PDF_TOKEN_END = 0,
	PDF_TOKEN_INTEGER,
	PDF_TOKEN_REAL,
	PDF_TOKEN_NAME,
	PDF_TOKEN_STRING,
	PDF_TOKEN_ARRAY_OPEN,
	PDF_TOKEN_ARRAY_CLOSE,
	PDF_TOKEN_DICTIONARY_OPEN,
	PDF_TOKEN_DICTIONARY_CLOSE,
	PDF_TOKEN_KEYWORD
};

/*
 * One PDF object, direct or loaded from an indirect one.
 *
 * Only the fields of its type are meaningful.  A name's or a string's bytes
 * are decoded and followed by a NUL that is not counted.  A dictionary and a
 * stream keep their keys (names) and values side by side; a stream's data
 * is a range of the document's bytes, or of bytes when they are set (an
 * inline image, whose data is in a content stream).  An object loaded from
 * the file keeps its number and generation (an encrypted document's key
 * for it); a reference keeps the ones it names.  Every object lives in the
 * document's arena (or a page run's) and is freed with it.
 */
struct pdf_object {
	enum pdf_object_type type;
	int boolean;
	long integer;
	double real;
	const unsigned char *bytes;
	size_t length;
	struct pdf_object **keys;
	struct pdf_object **values;
	size_t count;
	size_t data_offset;
	size_t data_length;
	unsigned long number;
	unsigned long generation;
};

/*
 * One block of an arena.
 *
 * The block's bytes follow this header in the same allocation.
 */
struct pdf_arena_block {
	struct pdf_arena_block *next;
	size_t used;
	size_t size;
};

/*
 * The memory every object of one document is carved from.
 *
 * Nothing is freed on its own; the whole arena is freed when the document
 * closes.  total counts every byte handed out, which the arena's limit
 * bounds.
 */
struct pdf_arena {
	struct pdf_arena_block *blocks;
	size_t total;
};

/*
 * A position in a document's bytes and the arena its tokens are decoded into.
 */
struct pdf_lexer {
	const unsigned char *data;
	size_t size;
	size_t position;
	struct pdf_arena *arena;
};

/*
 * One token.
 *
 * A keyword's bytes point into the document; a name's and a string's are
 * decoded into the arena.
 */
struct pdf_token {
	enum pdf_token_type type;
	long integer;
	double real;
	const unsigned char *bytes;
	size_t length;
};

/* The most bytes one stream may decode to, the decode limit of design-pdf.md section 4.3. */
#define PDF_FILTER_OUTPUT_MAX ((size_t)256 * 1024 * 1024)

/* The longest side of an image, in samples. */
#define PDF_IMAGE_SIDE_MAX 16384

/* The most items, path points and image pixels one display list may hold. */
#define PDF_DISPLAY_ITEMS_MAX ((size_t)1048576)
#define PDF_DISPLAY_POINTS_MAX ((size_t)8388608)
#define PDF_DISPLAY_PIXELS_MAX ((size_t)64 * 1024 * 1024)

/*
 * A display list while a page is interpreted into it.
 *
 * The public list comes first, so the builder is the list the caller
 * receives.  Each item's path is kept as offsets into the shared verb and
 * point arrays while they may still move; pdf_display_finish() turns the
 * offsets into the items' pointers.  The images' pixels belong to the
 * builder and are freed with it.
 */
struct pdf_display_builder {
	struct pdf_display_list list;
	struct pdf_display_item *items;
	size_t *verb_starts;
	size_t *point_starts;
	size_t items_count;
	size_t items_capacity;
	unsigned char *verbs;
	size_t verbs_count;
	size_t verbs_capacity;
	struct pdf_point *points;
	size_t points_count;
	size_t points_capacity;
	unsigned char **images;
	size_t images_count;
	size_t images_capacity;
	size_t pixels_count;
};

void *pdf_arena_allocate(struct pdf_arena *arena, size_t size);
void pdf_arena_free(struct pdf_arena *arena);
int pdf_lexer_next(struct pdf_lexer *lexer, struct pdf_token *token);
void pdf_lexer_skip_space(struct pdf_lexer *lexer);
int pdf_token_is_keyword(const struct pdf_token *token, const char *keyword);
int pdf_parse_object(struct pdf_lexer *lexer, int depth, struct pdf_object **object);
struct pdf_object *pdf_object_get(const struct pdf_object *dictionary, const char *key);
int pdf_object_is_name(const struct pdf_object *object, const char *name);
int pdf_object_number(const struct pdf_object *object, double *number);

/* What the content interpreter needs of a document being read (reader.c). */
int pdf_reader_resolve(struct pdf_document *document, struct pdf_object *object, struct pdf_object **resolved);
int pdf_reader_resolve_key(struct pdf_document *document, const struct pdf_object *dictionary, const char *key, struct pdf_object **resolved);
int pdf_reader_page(struct pdf_document *document, size_t index, struct pdf_object **page, struct pdf_object **resources);
const unsigned char *pdf_reader_bytes(const struct pdf_document *document);

/* What an update needs of the document it adds to (reader.c). */
size_t pdf_reader_size(const struct pdf_document *document);
void pdf_reader_roots(struct pdf_document *document, struct pdf_object **trailer, struct pdf_object **catalog);
unsigned long pdf_reader_next_number(const struct pdf_document *document);
int pdf_reader_page_reference(struct pdf_document *document, size_t index, struct pdf_object **reference);
int pdf_reader_page_inherited(struct pdf_document *document, size_t index, struct pdf_object **media_box, struct pdf_object **crop_box, struct pdf_object **rotate);

/* The stream filters (filter.c). */
int pdf_filter_decode(struct pdf_document *document, const struct pdf_object *stream, int stop_at_dct, const unsigned char **data, size_t *size, unsigned char **owned, int *dct);

/*
 * The parameters of one CCITTFaxDecode filter: its /DecodeParms with the
 * defaults of the PDF reference filled in.  rows 0 means as many rows as
 * the data holds.  The filter (filter.c) fills one for each decode.
 */
struct pdf_ccitt_parameters {
	long k;
	long columns;
	long rows;
	long damaged_rows;
	int end_of_line;
	int byte_align;
	int end_of_block;
	int black_is_1;
};

/* The CCITT fax decoder of Group 3 and Group 4 (ccitt.c, stage 3). */
int pdf_ccitt_decode(const struct pdf_ccitt_parameters *parameters, const unsigned char *input, size_t input_size, unsigned char **output, size_t *output_size);

/* The images (image.c). */
int pdf_image_decode(struct pdf_document *document, const struct pdf_object *stream, const double fill[3], unsigned char **pixels, size_t *width, size_t *height, int *interpolate, unsigned *flags);

/* The display list being built (display.c). */
int pdf_display_create(struct pdf_display_builder **builder);
void pdf_display_free(struct pdf_display_builder *builder);
int pdf_display_add_path(struct pdf_display_builder *builder, enum pdf_item_type type, const unsigned char *verbs, size_t verb_count, const struct pdf_point *points, size_t point_count, const struct pdf_display_item *style);
int pdf_display_add_image(struct pdf_display_builder *builder, unsigned char *pixels, const struct pdf_display_item *style);
int pdf_display_add_clip_pop(struct pdf_display_builder *builder);
void pdf_display_finish(struct pdf_display_builder *builder);

/* The stroker (stroke.c): the outline a stroked path covers, in the path's own space. */
struct pdf_stroke_style {
	double width;
	int cap;
	int join;
	double miter_limit;
	const double *dash;
	size_t dash_count;
	double dash_phase;
	double tolerance;
};
int pdf_stroke_path(const unsigned char *verbs, size_t verb_count, const struct pdf_point *points, size_t point_count, const struct pdf_stroke_style *style, unsigned char **out_verbs, size_t *out_verb_count, struct pdf_point **out_points, size_t *out_point_count);

/*
 * One glyph name of a /Differences array and the character it stands for
 * (encoding.c).
 */
struct pdf_glyph_name {
	const char *name;
	unsigned short unicode;
};

/* The simple fonts' base encodings and glyph names (encoding.c). */
extern const unsigned short pdf_encoding_standard[256];
extern const unsigned short pdf_encoding_win_ansi[256];
extern const unsigned short pdf_encoding_mac_roman[256];
extern const struct pdf_glyph_name pdf_glyph_names[];
extern const size_t pdf_glyph_names_count;

/*
 * A font of a document, and a document's fonts (font.c).
 *
 * The fonts live in the document until it is closed.
 */
struct pdf_font;
struct pdf_font_cache;

/*
 * What one character code of a shown string draws.
 *
 * width is the horizontal displacement in ems of the font size (a vertical
 * font's glyph moves by vertical_advance instead, from the origin
 * origin_x, origin_y).  When drawable, the outline (verbs and points) is in
 * ems with y upward, and transform (a b c d) maps it into text space before
 * the font size: it narrows or leans a substitute.  bold is the width, in
 * ems, of the stroke that thickens a substitute standing in for a bold
 * face.  missing says the code draws the font's missing glyph (glyph 0,
 * ws175-p010).
 */
struct pdf_glyph {
	double width;
	double vertical_advance;
	double origin_x;
	double origin_y;
	double transform[4];
	double bold;
	int drawable;
	int missing;
	const unsigned char *verbs;
	size_t verb_count;
	const struct pdf_point *points;
	size_t point_count;
};

int pdf_font_get(struct pdf_document *document, struct pdf_object *dictionary, struct pdf_font **font);
void pdf_font_cache_free(struct pdf_font_cache *cache);
unsigned pdf_font_status(const struct pdf_font *font);
int pdf_font_vertical(const struct pdf_font *font);
int pdf_font_type3(const struct pdf_font *font);
const struct pdf_object *pdf_font_dictionary(const struct pdf_font *font);
size_t pdf_font_next_code(const struct pdf_font *font, const unsigned char *bytes, size_t length, unsigned *code, int *single_byte);
int pdf_font_glyph(struct pdf_font *font, unsigned code, struct pdf_glyph *glyph);
int pdf_font_unicode(struct pdf_document *document, struct pdf_font *font, unsigned code, int single_byte, uint32_t *characters, size_t capacity, size_t *count);
int pdf_font_code(struct pdf_document *document, struct pdf_font *font, uint32_t character, unsigned *code, unsigned *length);
int pdf_font_embedded(const struct pdf_font *font);
int pdf_font_type3_glyph(struct pdf_document *document, struct pdf_font *font, unsigned code, struct pdf_object **procedure, double matrix[6], struct pdf_object **resources);
unsigned pdf_glyph_name_unicode(const unsigned char *name, size_t length);
struct pdf_font_cache *pdf_reader_font_cache(struct pdf_document *document);

/*
 * A font program whose glyphs are charstrings: Type 1 (/FontFile) or the
 * Compact Font Format (/FontFile3), read by charstrings.c, type1.c and
 * cff.c (stage 3).
 */
struct pdf_charstrings;

/* Where a charstring's outline goes: one path step in ems with y upward, 0 or an errno value. */
typedef int (*pdf_charstrings_emit)(void *context, enum pdf_path_verb verb, const double *coordinates, size_t count);

int pdf_type1_open(const unsigned char *data, size_t size, size_t clear_length, struct pdf_charstrings **font);
int pdf_type1_encoding(const unsigned char *data, size_t size, const unsigned char *names[256], size_t lengths[256], int *standard);
int pdf_cff_open(const unsigned char *data, size_t size, struct pdf_charstrings **font);
int pdf_opentype_cff(const unsigned char *data, size_t size, const unsigned char **cff, size_t *cff_size);
void pdf_charstrings_close(struct pdf_charstrings *font);
size_t pdf_charstrings_count(const struct pdf_charstrings *font);
int pdf_charstrings_find(const struct pdf_charstrings *font, const unsigned char *name, size_t length, unsigned *glyph);
int pdf_charstrings_name(const struct pdf_charstrings *font, unsigned glyph, const unsigned char **name, size_t *length);
int pdf_charstrings_builtin(const struct pdf_charstrings *font, unsigned code, unsigned *glyph);
int pdf_charstrings_cid_keyed(const struct pdf_charstrings *font);
int pdf_charstrings_cid(const struct pdf_charstrings *font, unsigned cid, unsigned *glyph);
int pdf_charstrings_outline(struct pdf_charstrings *font, unsigned glyph, pdf_charstrings_emit emit, void *context, double *advance);

/*
 * The standard security handler of an encrypted document (crypt.c,
 * stage 3): the file key of the user or the owner password and the ciphers of
 * strings and streams.
 */
struct pdf_crypt;

int pdf_crypt_open(struct pdf_document *document, struct pdf_object *encrypt, const unsigned char *id, size_t id_length, const unsigned char *password, size_t password_length, struct pdf_crypt **crypt);
void pdf_crypt_close(struct pdf_crypt *crypt);
int pdf_crypt_metadata(const struct pdf_crypt *crypt);
int pdf_crypt_decrypt(const struct pdf_crypt *crypt, int stream, unsigned long number, unsigned long generation, const unsigned char *input, size_t size, unsigned char *output, size_t *output_size);
struct pdf_crypt *pdf_reader_crypt(const struct pdf_document *document);

/* The smooth shadings (shading.c): a shading drawn into an image over a region of the page. */
int pdf_shading_image(struct pdf_document *document, struct pdf_object *object, const double matrix[6], const double bounds[4], unsigned char **pixels, size_t *width, size_t *height, double placement[6]);
void pdf_reader_set_font_cache(struct pdf_document *document, struct pdf_font_cache *cache, void (*release)(struct pdf_font_cache *cache));

/*
 * One object of a page's content the editor can change (ws175-p002, the
 * scan of design.md section 3.1): an image (an image XObject's Do, or an
 * inline image) or a graphic (a form XObject's Do) at the content's top
 * level.  offset and length are its bytes in the page's decoded content
 * (the streams joined as the interpreter reads them), from its first
 * operand to the end of its operator (an inline image from BI to EI); ctm
 * is the matrix in force (user space to the shown space, C_rec), quad the
 * corners it covers in the shown space (the image's top left, top right,
 * bottom right and bottom left; a form's box the same way), width and
 * height an image's samples, clipped whether a clip was in force, and
 * fingerprint the first bytes of the SHA-256 of its bytes.
 */
struct pdf_scan_object {
	unsigned kind;
	size_t offset;
	size_t length;
	double ctm[6];
	double quad[8];
	size_t width;
	size_t height;
	int clipped;
	unsigned char fingerprint[8];
};

/*
 * One shown string of a page's top level (ws175-p002b, design.md section
 * 3.1): the operator's bytes (first operand to the operator: Tj, TJ, ' or
 * "), the text object it is in (its BT's number on the page), the text
 * matrix before its first glyph (after the line move of ' and ") and after
 * its last, the matrix in force, the font and the text state, the fill
 * colour when it is RGB, gray or CMYK (fill_known), the characters it
 * stands for (characters_from and characters_count in the scan's
 * characters), the corners of its glyphs' line (from a descent of 0.2 to an
 * ascent of 0.8 of the size), how many operators that draw or change the
 * graphics state came since the shown string before it (the lines of
 * adjacent text objects join only without any, [M9][N16]), the marked
 * content opened since its text object began (marked_depth, design.md
 * [M10]), and its flags (SCAN_SHOW_*).
 */
#define PDF_SCAN_SHOW_UNKNOWN	0x1U
#define PDF_SCAN_SHOW_TYPE3	0x2U
#define PDF_SCAN_SHOW_VERTICAL	0x4U

/* The longest font name of the resources a shown string keeps (ws175-p005). */
#define PDF_SCAN_FONT_NAME_MAX	64

/* The operator of a shown string (ws175-p004): Tj, TJ, ' or ". */
#define PDF_SCAN_SHOW_TJ	0U
#define PDF_SCAN_SHOW_ARRAY	1U
#define PDF_SCAN_SHOW_NEXT	2U
#define PDF_SCAN_SHOW_SPACED	3U
struct pdf_scan_show {
	size_t offset;
	size_t length;
	unsigned op;
	size_t block;
	double start[6];
	double end[6];
	double ctm[6];
	struct pdf_font *font;
	char font_resource[PDF_SCAN_FONT_NAME_MAX];
	size_t font_resource_length;
	double font_size;
	double character_spacing;
	double word_spacing;
	double horizontal_scale;
	double leading;
	double rise;
	int render_mode;
	double fill[3];
	int fill_known;
	size_t characters_from;
	size_t characters_count;
	double quad[8];
	size_t drawn_before;
	size_t marked_depth;
	unsigned flags;
	unsigned char fingerprint[8];
};

/*
 * One text object of a page's top level (ws175-p004): the offset of its BT
 * and the end of its ET (ended: there is one) in the decoded content.
 */
struct pdf_scan_block {
	size_t begin;
	size_t end;
	int ended;
};

/*
 * One operator that moves the text position inside a text object of the
 * page's top level (Td, TD, T* or Tm; ws175-p004, design.md section 3.4):
 * its bytes, its text object, and for TD the leading it sets (sets_leading).
 */
struct pdf_scan_move {
	size_t offset;
	size_t length;
	size_t block;
	int sets_leading;
	double leading;
};

/*
 * One marked content of the page's top level opened by BDC (ws175-p004,
 * design.md [M10]): the BDC's bytes (its tag, its properties and the
 * operator) and the end of its EMC (0: not closed).
 */
struct pdf_scan_mark {
	size_t offset;
	size_t length;
	size_t end;
};

/*
 * What the scan of a page's content found: its objects in the order of the
 * content, the q left open at its end, the Q that had no q to restore
 * (their offsets; each is one byte, "Q"), whether the content ended inside
 * a text object (BT without its ET), whether objects were left out (past
 * the limit of q's nesting, or after the content stopped), and the
 * page's matrix from its user space to the shown space (B, the matrix in
 * force where the content starts).  ws128-p004: each character's corners
 * in the shown space (character_quads, eight numbers a character: its
 * code's glyph from the descent to the ascent, the characters of one code
 * sharing them), for the page's text.  ws177-p032: the characters shown
 * inside the form XObjects the page draws (not inside a Type 3 glyph),
 * apart from the page's own, which the editor does not read: their corners
 * as character_quads', and what stands before each (form_breaks:
 * PDF_SCAN_FORM_*), decided as each is noted.
 */
#define PDF_SCAN_FORM_SAME	0U
#define PDF_SCAN_FORM_SPACE	1U
#define PDF_SCAN_FORM_LINE	2U
struct pdf_scan {
	struct pdf_scan_object *objects;
	size_t count;
	size_t capacity;
	size_t open_saves;
	size_t *stray_restores;
	size_t stray_count;
	size_t stray_capacity;
	int in_text;
	int partial;
	int error;
	double base[6];
	struct pdf_scan_show *shows;
	size_t show_count;
	size_t show_capacity;
	uint32_t *characters;
	size_t character_count;
	size_t character_capacity;
	double *character_quads;
	size_t character_quad_capacity;
	size_t block_count;
	unsigned char *block_clips;
	size_t block_clip_capacity;
	struct pdf_scan_block *blocks;
	size_t block_capacity;
	struct pdf_scan_move *moves;
	size_t move_count;
	size_t move_capacity;
	struct pdf_scan_mark *marks;
	size_t mark_count;
	size_t mark_capacity;
	size_t *mark_stack;
	size_t mark_depth;
	size_t mark_stack_capacity;
	uint32_t *form_characters;
	size_t form_character_count;
	size_t form_character_capacity;
	double *form_quads;
	size_t form_quad_capacity;
	unsigned char *form_breaks;
	size_t form_break_capacity;
};

/* The scan of a page's content (content.c): the objects, and the decoded content they are ranges of. */
int pdf_content_scan(struct pdf_document *document, size_t index, struct pdf_scan *scan, unsigned char **content, size_t *size, unsigned *read_flags);
void pdf_scan_free(struct pdf_scan *scan);
int pdf_content_render(struct pdf_document *document, size_t index, const unsigned char *content, size_t size, struct pdf_object *resources, struct pdf_display_list **list);

/* A font's /ToUnicode CMap (tounicode.c, ws175-p002b): which characters each code stands for. */
struct pdf_tounicode;
int pdf_tounicode_parse(const unsigned char *data, size_t size, struct pdf_tounicode **map);
int pdf_tounicode_lookup(const struct pdf_tounicode *map, unsigned code, unsigned length, uint32_t *characters, size_t capacity, size_t *count);
int pdf_tounicode_reverse(const struct pdf_tounicode *map, uint32_t character, unsigned *code, unsigned *length);
void pdf_tounicode_free(struct pdf_tounicode *map);

/*
 * The replacement fonts the editor writes new words in (replace.c,
 * ws175-p005, design.md section 4.1 as updated 2026-10-06): the desktop's
 * Mahora Regular (Sans) and Mahora Mono (Mono), and for the characters a
 * font lacks its fallbacks JetBrains Mono, then Droid Sans Fallback (CJK).
 * A document keeps them (their faces and the preview's font objects) until
 * it closes, design.md [H5].
 */
#define PDF_EDIT_FILE_SANS		0U
#define PDF_EDIT_FILE_MONO		1U
#define PDF_EDIT_FILE_FALLBACK_MONO	2U
#define PDF_EDIT_FILE_FALLBACK		3U
#define PDF_EDIT_FILES			4U
struct pdf_edit_fonts;
struct pdf_edit_fonts *pdf_reader_edit_fonts(struct pdf_document *document);
void pdf_reader_set_edit_fonts(struct pdf_document *document, struct pdf_edit_fonts *fonts, void (*release)(struct pdf_edit_fonts *fonts));
int pdf_edit_fonts_of(struct pdf_document *document, struct pdf_edit_fonts **fonts);
int pdf_edit_font_glyph(struct pdf_edit_fonts *fonts, unsigned file, uint32_t character, unsigned *glyph);
int pdf_edit_font_present(struct pdf_edit_fonts *fonts, unsigned file);
int pdf_edit_font_advance(struct pdf_edit_fonts *fonts, unsigned file, unsigned glyph, double *ems);
int pdf_edit_font_object(struct pdf_edit_fonts *fonts, unsigned file, struct pdf_object **font);
int pdf_edit_font_read(unsigned file, unsigned char **data, size_t *size);
const char *pdf_edit_font_name(unsigned file);

/* The subset of a TrueType font a document embeds, and the font's embedding permission (subset.c, ws175-p005). */
int pdf_truetype_subset(const unsigned char *data, size_t size, const unsigned char *used, size_t glyph_count, unsigned char **out, size_t *out_size);
int pdf_truetype_permission(const unsigned char *data, size_t size, int *allowed, int *whole);
struct pdf_truetype_metrics {
	unsigned units_per_em;
	int box[4];
	int ascent;
	int descent;
	int cap_height;
	int fixed;
	unsigned metrics_count;
	unsigned glyphs;
};
int pdf_truetype_metrics(const unsigned char *data, size_t size, struct pdf_truetype_metrics *metrics);
int pdf_truetype_advance(const unsigned char *data, size_t size, unsigned glyph, unsigned *advance);

/* A PNG whose compressed rows a PDF image takes as they are, and the check of such rows (intake.c, ws175-p006). */
int pdf_png_rows(const unsigned char *png, size_t size, size_t *width, size_t *height, int *components, unsigned char **rows, size_t *length);
int pdf_png_check_rows(const unsigned char *rows, size_t length, size_t width, size_t height, int components);

/* The editor's new content (editor.c). */
struct pdf_buffer;
int pdf_editor_content(const struct pdf_page_editor *editor, size_t hidden, const char *prefix, const size_t *names, const char *font_prefix, struct pdf_buffer *out);

#endif /* LIBPDF_INTERNAL_H */
