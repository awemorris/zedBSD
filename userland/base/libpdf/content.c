/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The content interpreter of libpdf's reader: a page's content streams
 * run into a display list (stage 1 of design-pdf.md).
 *
 * It keeps the graphics state stack (q, Q, cm, the line style, the fill and
 * stroke colours in DeviceGray, DeviceRGB and DeviceCMYK and the spaces
 * built on them, the ExtGState's alphas and the Normal and Multiply blend
 * modes), builds paths (m l c v y h re), fills them by either rule, strokes
 * them through the stroker (stroke.c) into fills, clips (W W* n), and draws
 * image XObjects and runs form XObjects (Do), and draws inline images (BI
 * ID EI) the same way.  From stage 2 it shows text:
 * the text state (Tc Tw Tz TL Tf Tr Ts, kept by q and Q), the text and line
 * matrices of a text object (BT ET Td TD Tm T*), and the strings (Tj TJ '
 * "), whose glyph outlines (font.c) are filled, stroked, or added to a clip
 * that ET applies, by the rendering mode.  It paints the axial and radial
 * shadings (shading.c), by sh and as the colour of a shading pattern (cs
 * /Pattern and scn), as images clipped to what they fill.  Tiling
 * patterns and the other shadings are left out and the list says so
 * (PDF_DISPLAY_SKIPPED).
 *
 * The content is not trusted.  Operators, operands, the q nesting, the
 * clips, the forms' nesting and the list's size are bounded; a malformed
 * token ends the content (PDF_DISPLAY_DAMAGED) and a limit stops it
 * (PDF_DISPLAY_LIMITED), and in both cases what came before is drawn.
 *
 * The same run scans a page for the editor (ws175-p002, pdf_content_scan):
 * the images and graphics at the content's top level, with their byte
 * ranges, the matrix in force and the corners they cover, and how q and Q
 * are balanced.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <pdf.h>
#include <sha2.h>

#include "internal.h"

/* The most operands one operator may be given. */
#define PDF_CONTENT_OPERANDS_MAX 64

/* The most entries an inline image's dictionary may have (it has ten keys; the rest is room for unknown ones). */
#define PDF_CONTENT_INLINE_KEYS_MAX 16

/* How deep q may nest, and how deep form XObjects may. */
#define PDF_CONTENT_STACK_MAX 64
#define PDF_CONTENT_FORMS_MAX 12

/* The most operators one page runs, its forms included. */
#define PDF_CONTENT_OPERATORS_MAX 16777216UL

/* The most entries of a dash pattern, and how deep clips may nest. */
#define PDF_CONTENT_DASH_MAX 16
#define PDF_CONTENT_CLIPS_MAX 64

/* The most characters one shown string draws. */
#define PDF_CONTENT_STRING_MAX ((size_t)1048576)

/* How far a flattened curve may stray from the true one, and the width of a zero-width line, in page points. */
#define PDF_CONTENT_TOLERANCE 0.05
#define PDF_CONTENT_HAIRLINE 0.25

/* The kinds of object the scan lists (pdf.h's enum pdf_edit_kind). */
#define CONTENT_SCAN_IMAGE	1U
#define CONTENT_SCAN_GRAPHIC	2U

/*
 * The operators the interpreter knows.
 */
enum content_operator {
	OP_UNKNOWN = 0,
	OP_SAVE,
	OP_RESTORE,
	OP_CONCAT,
	OP_LINE_WIDTH,
	OP_LINE_CAP,
	OP_LINE_JOIN,
	OP_MITER_LIMIT,
	OP_DASH,
	OP_EXTGSTATE,
	OP_MOVE,
	OP_LINE,
	OP_CURVE,
	OP_CURVE_V,
	OP_CURVE_Y,
	OP_CLOSE,
	OP_RECTANGLE,
	OP_STROKE,
	OP_CLOSE_STROKE,
	OP_FILL,
	OP_FILL_EVEN_ODD,
	OP_FILL_STROKE,
	OP_FILL_STROKE_EVEN_ODD,
	OP_CLOSE_FILL_STROKE,
	OP_CLOSE_FILL_STROKE_EVEN_ODD,
	OP_END_PATH,
	OP_CLIP,
	OP_CLIP_EVEN_ODD,
	OP_GRAY_FILL,
	OP_GRAY_STROKE,
	OP_RGB_FILL,
	OP_RGB_STROKE,
	OP_CMYK_FILL,
	OP_CMYK_STROKE,
	OP_SPACE_FILL,
	OP_SPACE_STROKE,
	OP_COLOR_FILL,
	OP_COLOR_STROKE,
	OP_XOBJECT,
	OP_INLINE_IMAGE,
	OP_SHADING,
	OP_TEXT_BEGIN,
	OP_TEXT_END,
	OP_TEXT_FONT,
	OP_TEXT_CHARACTER_SPACING,
	OP_TEXT_WORD_SPACING,
	OP_TEXT_SCALE,
	OP_TEXT_LEADING,
	OP_TEXT_RISE,
	OP_TEXT_RENDER,
	OP_TEXT_MOVE,
	OP_TEXT_MOVE_LEADING,
	OP_TEXT_MATRIX,
	OP_TEXT_NEXT_LINE,
	OP_TEXT_SHOW,
	OP_TEXT_SHOW_ARRAY,
	OP_TEXT_NEXT_SHOW,
	OP_TEXT_SPACED_SHOW,
	OP_IGNORED
};

/*
 * One operator's name and meaning, for the lookup table.
 */
struct content_name {
	const char *name;
	enum content_operator code;
};

/*
 * The kinds of operand.
 */
enum content_operand_type {
	OPERAND_NUMBER = 0,
	OPERAND_NAME,
	OPERAND_OTHER
};

/*
 * One operand: a number, a name (its decoded bytes), or any other object.
 */
struct content_operand {
	enum content_operand_type type;
	double number;
	const unsigned char *bytes;
	size_t length;
	struct pdf_object *object;
};

/*
 * One level of the graphics state.
 *
 * The colours are RGB from 0 to 1; a colour space the interpreter cannot
 * read leaves its colour unusable (fill_usable, stroke_usable), and what
 * would be painted with it is left out.  clips counts the clips pushed on
 * the list at this level, which its Q pops.
 *
 * A colour may instead be a shading pattern (fill_pattern, stroke_pattern,
 * once scn names one in the Pattern space the *_pattern_space flags mark).
 *
 * The text state is part of the level: the font (NULL until Tf names one
 * the reader can use) and its size, the character and word spacing, the
 * horizontal scale (1 is 100%), the leading, the rise, and the rendering
 * mode (0 to 7).
 */
struct content_state {
	double ctm[6];
	double fill[3];
	double stroke[3];
	int fill_components;
	int stroke_components;
	int fill_usable;
	int stroke_usable;
	double fill_alpha;
	double stroke_alpha;
	int fill_pattern_space;
	int stroke_pattern_space;
	struct pdf_object *fill_pattern;
	struct pdf_object *stroke_pattern;
	enum pdf_blend_mode blend;
	double line_width;
	int line_cap;
	int line_join;
	double miter_limit;
	double dash[PDF_CONTENT_DASH_MAX];
	size_t dash_count;
	double dash_phase;
	size_t clips;
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
};

/*
 * The path being built, in user space.
 */
struct content_path {
	unsigned char *verbs;
	size_t verb_count;
	size_t verb_capacity;
	struct pdf_point *points;
	size_t point_count;
	size_t point_capacity;
	struct pdf_point start;
	struct pdf_point current;
	int has_current;
};

/*
 * One page's interpretation: the document, the list being built, the
 * memory tokens are decoded into, the graphics state stack (the level in
 * force is stack[depth]), the path, the clip waiting for the next painting
 * operator, the operands of the next operator, the counts that the limits
 * bound, and the flags the list gets.
 *
 * base_depth is the level a running form started at, below which its Q
 * does not go; ignored_saves counts q past the stack's limit, which the
 * matching Q only uncount.  scratch holds the transformed copy of a path.
 *
 * pattern_base is the matrix of the content stream being run (the page's,
 * or a form's), which a pattern's own matrix is relative to.
 *
 * A text object has its text matrix and line matrix; the glyphs of its
 * strings shown in a clipping mode gather in text_clip (in the page's
 * shown space) until ET makes them the clip.  text_resources are the
 * resources of the text operator being run, which a Type 3 glyph without
 * its own runs with.
 *
 * A scan (scan not NULL) records the page's own objects: content_level is
 * how deep content runs nest (1 for the page's, more inside a form or a
 * Type 3 glyph), scan_saves counts the page's q by their tokens, and the
 * operator being run fills scan_object (scan_pending) when it draws one.
 * ws177-p040: text_forms counts the levels of form_depth that are forms
 * (not Type 3 glyphs), so that the text shown in forms alone is noted;
 * form_serial changes as a form starts or ends, and form_noted_serial is
 * the one the last form character was noted under (a new one starts a
 * line).
 */
struct content_run {
	struct pdf_document *document;
	struct pdf_display_builder *builder;
	struct pdf_arena arena;
	struct content_state stack[PDF_CONTENT_STACK_MAX];
	size_t depth;
	size_t base_depth;
	size_t ignored_saves;
	struct content_path path;
	int clip_pending;
	enum pdf_fill_rule clip_rule;
	struct content_operand operands[PDF_CONTENT_OPERANDS_MAX];
	size_t operand_count;
	unsigned long operators;
	size_t clip_depth;
	int form_depth;
	int text_forms;
	unsigned long form_serial;
	unsigned long form_noted_serial;
	int stopped;
	unsigned flags;
	struct pdf_point *scratch;
	size_t scratch_capacity;
	double pattern_base[6];
	double text_matrix[6];
	double line_matrix[6];
	int text_clipping;
	struct pdf_object *text_resources;
	unsigned char *text_clip_verbs;
	size_t text_clip_verb_count;
	size_t text_clip_verb_capacity;
	struct pdf_point *text_clip_points;
	size_t text_clip_point_count;
	size_t text_clip_point_capacity;
	struct pdf_scan *scan;
	size_t content_level;
	size_t scan_saves;
	int scan_pending;
	struct pdf_scan_object scan_object;
	int scan_show_started;
	double scan_show_start[6];
	size_t scan_show_from;
	unsigned scan_show_flags;
	size_t scan_drawn;
	size_t scan_marked;
	size_t scan_marked_at_text;
};

/*
 * One key of an inline image's dictionary: its abbreviation and the full
 * name an image XObject has for it.
 */
struct content_inline_key {
	const char *abbreviation;
	const char *name;
};

/*
 * The abbreviated keys of an inline image (PDF 1.7 table 93).
 *
 * The full names are accepted as they are; the image decoder reads only
 * the full names.
 */
static const struct content_inline_key content_inline_keys[] = {
	{ "BPC", "BitsPerComponent" },
	{ "CS", "ColorSpace" },
	{ "D", "Decode" },
	{ "DP", "DecodeParms" },
	{ "F", "Filter" },
	{ "H", "Height" },
	{ "IM", "ImageMask" },
	{ "I", "Interpolate" },
	{ "W", "Width" },
	{ "L", "Length" }
};

/*
 * The operators by name, the ones a page uses most first.
 */
static const struct content_name content_names[] = {
	{ "m", OP_MOVE },
	{ "l", OP_LINE },
	{ "c", OP_CURVE },
	{ "h", OP_CLOSE },
	{ "f", OP_FILL },
	{ "re", OP_RECTANGLE },
	{ "q", OP_SAVE },
	{ "Q", OP_RESTORE },
	{ "cm", OP_CONCAT },
	{ "rg", OP_RGB_FILL },
	{ "gs", OP_EXTGSTATE },
	{ "S", OP_STROKE },
	{ "RG", OP_RGB_STROKE },
	{ "w", OP_LINE_WIDTH },
	{ "Do", OP_XOBJECT },
	{ "g", OP_GRAY_FILL },
	{ "G", OP_GRAY_STROKE },
	{ "n", OP_END_PATH },
	{ "W", OP_CLIP },
	{ "W*", OP_CLIP_EVEN_ODD },
	{ "v", OP_CURVE_V },
	{ "y", OP_CURVE_Y },
	{ "f*", OP_FILL_EVEN_ODD },
	{ "F", OP_FILL },
	{ "s", OP_CLOSE_STROKE },
	{ "B", OP_FILL_STROKE },
	{ "B*", OP_FILL_STROKE_EVEN_ODD },
	{ "b", OP_CLOSE_FILL_STROKE },
	{ "b*", OP_CLOSE_FILL_STROKE_EVEN_ODD },
	{ "J", OP_LINE_CAP },
	{ "j", OP_LINE_JOIN },
	{ "M", OP_MITER_LIMIT },
	{ "d", OP_DASH },
	{ "k", OP_CMYK_FILL },
	{ "K", OP_CMYK_STROKE },
	{ "cs", OP_SPACE_FILL },
	{ "CS", OP_SPACE_STROKE },
	{ "sc", OP_COLOR_FILL },
	{ "scn", OP_COLOR_FILL },
	{ "SC", OP_COLOR_STROKE },
	{ "SCN", OP_COLOR_STROKE },
	{ "BI", OP_INLINE_IMAGE },
	{ "sh", OP_SHADING },
	{ "Tj", OP_TEXT_SHOW },
	{ "TJ", OP_TEXT_SHOW_ARRAY },
	{ "'", OP_TEXT_NEXT_SHOW },
	{ "\"", OP_TEXT_SPACED_SHOW },
	{ "BT", OP_TEXT_BEGIN },
	{ "ET", OP_TEXT_END },
	{ "Tf", OP_TEXT_FONT },
	{ "Td", OP_TEXT_MOVE },
	{ "Tm", OP_TEXT_MATRIX },
	{ "TD", OP_TEXT_MOVE_LEADING },
	{ "T*", OP_TEXT_NEXT_LINE },
	{ "Tc", OP_TEXT_CHARACTER_SPACING },
	{ "Tw", OP_TEXT_WORD_SPACING },
	{ "Tz", OP_TEXT_SCALE },
	{ "TL", OP_TEXT_LEADING },
	{ "Tr", OP_TEXT_RENDER },
	{ "Ts", OP_TEXT_RISE },
	{ "ri", OP_IGNORED },
	{ "i", OP_IGNORED },
	{ "d0", OP_IGNORED },
	{ "d1", OP_IGNORED },
	{ "BMC", OP_IGNORED },
	{ "BDC", OP_IGNORED },
	{ "EMC", OP_IGNORED },
	{ "MP", OP_IGNORED },
	{ "DP", OP_IGNORED },
	{ "BX", OP_IGNORED },
	{ "EX", OP_IGNORED }
};

static int read_contents(struct pdf_document *document, struct pdf_object *page, unsigned char **content, size_t *size, unsigned *flags);
static int append_stream(struct pdf_document *document, struct pdf_object *stream, unsigned char **content, size_t *size, unsigned *flags);
static void base_matrix(const struct pdf_page_box *box, double matrix[6]);
static void run_content(struct content_run *run, const unsigned char *data, size_t size, struct pdf_object *resources);
static int read_operand(struct content_run *run, struct pdf_lexer *lexer, const struct pdf_token *token, size_t token_start);
static enum content_operator operator_code(const struct pdf_token *token);
static void execute(struct content_run *run, enum content_operator code, struct pdf_lexer *lexer, struct pdf_object *resources);
static void execute_state(struct content_run *run, enum content_operator code, struct pdf_object *resources);
static void execute_path(struct content_run *run, enum content_operator code);
static void execute_paint(struct content_run *run, enum content_operator code);
static void execute_color(struct content_run *run, enum content_operator code, struct pdf_object *resources);
static void save_state(struct content_run *run);
static void restore_state(struct content_run *run);
static void unwind_to(struct content_run *run, size_t depth);
static void end_clips(struct content_run *run);
static void concat_matrix(double ctm[6], const double matrix[6]);
static int read_numbers(const struct content_run *run, size_t count, double *numbers);
static void set_dash(struct content_state *state, const struct pdf_object *array, double phase);
static void apply_extgstate(struct content_run *run, struct pdf_object *resources);
static int state_number(struct content_run *run, struct pdf_object *dictionary, const char *key, double *number);
static int state_integer(struct content_run *run, struct pdf_object *dictionary, const char *key, long *integer);
static void state_dash(struct content_run *run, struct pdf_object *dictionary);
static void state_blend(struct content_run *run, struct pdf_object *dictionary);
static void apply_blend(struct content_run *run, struct pdf_object *mode);
static int blend_known(struct pdf_object *mode);
static int find_resource(struct content_run *run, struct pdf_object *resources, const char *category, struct pdf_object **found);
static int find_named_resource(struct content_run *run, struct pdf_object *resources, const char *category, const struct content_operand *operand, struct pdf_object **found);
static void execute_text(struct content_run *run, enum content_operator code, struct pdf_object *resources);
static void set_font(struct content_run *run, struct pdf_object *resources);
static void move_text_line(struct content_run *run, double x, double y);
static void begin_text(struct content_run *run);
static void end_text(struct content_run *run);
static void show_operand_string(struct content_run *run, size_t index);
static void show_array(struct content_run *run);
static void show_string(struct content_run *run, const unsigned char *bytes, size_t length);
static int add_glyph(struct content_run *run, const struct pdf_glyph *glyph);
static void advance_text(struct content_run *run, double amount);
static void paint_text(struct content_run *run, double bold);
static void embolden_text(struct content_run *run, double width);
static int add_text_clip(struct content_run *run);
static int is_pattern_space(struct content_run *run, struct pdf_object *resources);
static void set_pattern(struct content_run *run, struct pdf_object *resources, int stroke);
static void paint_shading(struct content_run *run, struct pdf_object *resources);
static void paint_pattern(struct content_run *run, struct pdf_object *pattern, const unsigned char *verbs, size_t verb_count, const struct pdf_point *points, size_t point_count, enum pdf_fill_rule rule, double alpha);
static int add_shading_image(struct content_run *run, struct pdf_object *shading, const double matrix[6], const double bounds[4], double alpha);
static int space_components(struct content_run *run, struct pdf_object *resources, int *components);
static int device_components(const unsigned char *name, size_t length);
static void set_color(struct content_run *run, int stroke, int components, const double *values);
static int path_add(struct content_run *run, enum pdf_path_verb verb, const double *coordinates, size_t count);
static int add_rectangle(struct content_run *run, double left, double bottom, double right, double top);
static void path_clear(struct content_run *run);
static void paint_path(struct content_run *run, int fill, enum pdf_fill_rule rule, int stroke);
static void fill_path(struct content_run *run, enum pdf_fill_rule rule);
static void stroke_path(struct content_run *run);
static void push_clip(struct content_run *run);
static int transform_path(struct content_run *run, const struct pdf_point *points, size_t count, struct pdf_point **transformed);
static double matrix_scale(const double matrix[6]);
static void draw_xobject(struct content_run *run, struct pdf_object *resources);
static void draw_image(struct content_run *run, struct pdf_object *image);
static void run_form(struct content_run *run, struct pdf_object *form, struct pdf_object *resources);
static void draw_type3_glyph(struct content_run *run, unsigned code);
static void draw_inline_image(struct content_run *run, struct pdf_lexer *lexer, struct pdf_object *resources);
static int read_inline_dictionary(struct content_run *run, struct pdf_lexer *lexer, struct pdf_object *resources, struct pdf_object **image);
static int add_inline_entry(struct content_run *run, struct pdf_object *resources, struct pdf_object *image, const struct pdf_token *key, struct pdf_object *value);
static const char *inline_key_name(const struct pdf_token *key);
static size_t inline_sample_bytes(const struct pdf_object *image);
static int find_inline_end(const struct pdf_lexer *lexer, size_t start, size_t *end);
static int inline_device_space(const struct pdf_object *name);
static int inline_device_components(const struct pdf_object *name);
static int is_white(unsigned char character);
static void stop_for(struct content_run *run, int error);
static int page_run(struct pdf_document *document, size_t index, const unsigned char *given, size_t given_size, struct pdf_object *given_resources, struct pdf_scan *scan, struct pdf_display_list **list, unsigned char **kept, size_t *kept_size, unsigned *read_flags);
static int scan_here(const struct content_run *run);
static void scan_image(struct content_run *run, struct pdf_object *image);
static void scan_form(struct content_run *run, struct pdf_object *form);
static void scan_operator(struct content_run *run, enum content_operator code, size_t start, size_t keyword, size_t end, const unsigned char *data);
static void scan_corners(const double matrix[6], const double corners[8], double quad[8]);
static int scan_add_stray(struct pdf_scan *scan, size_t offset);
static void scan_string(struct content_run *run, const unsigned char *bytes, size_t length);
static void scan_code(struct content_run *run, unsigned code, int single_byte, const double before[6]);
static void scan_glyph_quad(const struct content_run *run, const double before[6], double quad[8]);
static int scan_form_here(const struct content_run *run);
static void scan_form_code(struct content_run *run, unsigned code, int single_byte, const double before[6]);
static unsigned scan_form_break(const struct content_run *run, const double quad[8]);
static void scan_show(struct content_run *run, enum content_operator code, size_t start, size_t end, const unsigned char *data);
static void scan_move(struct content_run *run, enum content_operator code, size_t start, size_t end);
static void scan_mark(struct content_run *run, size_t start, size_t keyword, size_t end, const unsigned char *data);
static void scan_text_operator(struct content_run *run, enum content_operator code, size_t keyword, const unsigned char *data);
static int scan_grow(void **items, size_t *capacity, size_t count, size_t size);
static double clamp_unit(double value);

/*
 * Interprets a page's content into a display list.
 *
 * The list is the page's even when a part of it could not be drawn; its
 * flags say so.  An error is reported only for a page that cannot be
 * started at all (no such page, a malformed page box, no memory).
 */
int
pdf_page_render(
	struct pdf_document *document,
	size_t index,
	struct pdf_display_list **list)
{
	int error;

	/* Refuses a missing document or list. */
	if (document == NULL)
		return EINVAL;
	if (list == NULL)
		return EINVAL;

	/* Runs the page without a scan, keeping only the list. */
	error = page_run(document, index, NULL, 0, NULL, NULL, list, NULL, NULL, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the list is the page's drawing. */
	return 0;
}

/*
 * Scans a page's content for the editor (ws175-p002): the page runs as it
 * is drawn, and the scan gets the images and graphics of its top level and
 * how its q and Q are balanced.  content gets the decoded content (a
 * malloc'd buffer the objects' ranges are in, NULL for none) and
 * read_flags the PDF_DISPLAY_* flags of reading its streams alone (a
 * stream left out or damaged, before anything ran).
 */
int
pdf_content_scan(
	struct pdf_document *document,
	size_t index,
	struct pdf_scan *scan,
	unsigned char **content,
	size_t *size,
	unsigned *read_flags)
{
	struct pdf_display_list *list;
	int error;

	/* Refuses a missing document or scan. */
	if (document == NULL || scan == NULL || content == NULL || size == NULL || read_flags == NULL)
		return EINVAL;

	/* Runs the page with the scan; its drawing is not needed. */
	memset(scan, 0, sizeof(*scan));
	error = page_run(document, index, NULL, 0, NULL, scan, &list, content, size, read_flags);
	if (error != 0) {
		pdf_scan_free(scan);
		return error;
	}

	/* The drawing goes. */
	pdf_display_list_destroy(list);

	/* A scan that ran out of memory fails as a whole. */
	if (scan->error != 0) {
		error = scan->error;
		pdf_scan_free(scan);
		free(*content);
		*content = NULL;
		*size = 0;
		return error;
	}

	/* Succeeded: the objects are listed. */
	return 0;
}

/*
 * Interprets a page with a content of the caller's instead of its own (the
 * editor's new content, ws175-p003), on the page's boxes, with the page's
 * resources or the ones given (NULL for the page's).
 */
int
pdf_content_render(
	struct pdf_document *document,
	size_t index,
	const unsigned char *content,
	size_t size,
	struct pdf_object *resources,
	struct pdf_display_list **list)
{
	int error;

	/* Refuses a missing document, content or list. */
	if (document == NULL || list == NULL || (content == NULL && size != 0))
		return EINVAL;

	/* Runs the given content. */
	error = page_run(document, index, content, size, resources, NULL, list, NULL, NULL, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the list is the given content's drawing. */
	return 0;
}

/* Frees what a scan holds. */
void
pdf_scan_free(
	struct pdf_scan *scan)
{
	/* Nothing to free. */
	if (scan == NULL)
		return;

	/* The objects, the stray Q, the shown strings, their characters and the text objects' clips, then nothing. */
	free(scan->objects);
	free(scan->stray_restores);
	free(scan->shows);
	free(scan->characters);
	free(scan->character_quads);
	free(scan->block_clips);
	free(scan->blocks);
	free(scan->moves);
	free(scan->marks);
	free(scan->mark_stack);
	free(scan->form_characters);
	free(scan->form_quads);
	free(scan->form_breaks);
	memset(scan, 0, sizeof(*scan));
}

/*
 * Interprets a page into a display list, with a scan or without one, its
 * own content and resources or the ones given; kept (when not NULL) gets
 * the decoded content instead of its being freed, and read_flags the flags
 * of reading the streams.
 */
static int
page_run(
	struct pdf_document *document,
	size_t index,
	const unsigned char *given,
	size_t given_size,
	struct pdf_object *given_resources,
	struct pdf_scan *scan,
	struct pdf_display_list **list,
	unsigned char **kept,
	size_t *kept_size,
	unsigned *read_flags)
{
	struct pdf_display_builder *builder;
	struct pdf_page_box box;
	struct pdf_object *page;
	struct pdf_object *resources;
	struct content_run *run;
	unsigned char *content;
	size_t size;
	int error;

	/* Reads the page's boxes, which place the content on the page. */
	error = pdf_document_page_box(document, index, &box);
	if (error != 0)
		return error;

	/* Finds the page and its resources (or the ones given). */
	error = pdf_reader_page(document, index, &page, &resources);
	if (error != 0)
		return error;
	if (given_resources != NULL)
		resources = given_resources;

	/* Makes the list the page is drawn into. */
	error = pdf_display_create(&builder);
	if (error != 0)
		return error;
	builder->list.width = box.width;
	builder->list.height = box.height;

	/* Makes the interpretation's state, which is too large for the stack. */
	run = calloc(1, sizeof(*run));
	if (run == NULL) {
		pdf_display_free(builder);
		return ENOMEM;
	}

	/* Starts with the initial graphics state on the page's shown space. */
	run->document = document;
	run->builder = builder;
	run->scan = scan;
	base_matrix(&box, run->stack[0].ctm);
	memcpy(run->pattern_base, run->stack[0].ctm, sizeof(run->pattern_base));
	if (scan != NULL)
		memcpy(scan->base, run->stack[0].ctm, sizeof(scan->base));
	run->stack[0].horizontal_scale = 1.0;
	run->stack[0].fill_components = 1;
	run->stack[0].stroke_components = 1;
	run->stack[0].fill_usable = 1;
	run->stack[0].stroke_usable = 1;
	run->stack[0].fill_alpha = 1.0;
	run->stack[0].stroke_alpha = 1.0;
	run->stack[0].blend = PDF_BLEND_NORMAL;
	run->stack[0].line_width = 1.0;
	run->stack[0].miter_limit = 10.0;

	/* Reads the page's content streams, joined (or the content given, which is the caller's). */
	content = NULL;
	size = 0;
	error = 0;
	if (given == NULL)
		error = read_contents(document, page, &content, &size, &run->flags);
	if (error == ENOMEM) {
		free(run);
		pdf_display_free(builder);
		return ENOMEM;
	}

	/* What reading the streams alone found, for the editor. */
	if (read_flags != NULL)
		*read_flags = run->flags;

	/* Runs the content, then ends every level it left open and the page's own clips. */
	if (error == 0 && given != NULL)
		run_content(run, given, given_size, resources);
	if (error == 0 && given == NULL)
		run_content(run, content, size, resources);
	unwind_to(run, 0);
	end_clips(run);

	/* The scan's end: the q left open, and whether the content stopped before its end. */
	if (scan != NULL) {
		scan->open_saves = run->scan_saves;
		if ((run->flags & (PDF_DISPLAY_DAMAGED | PDF_DISPLAY_LIMITED)) != 0U)
			scan->partial = 1;
	}

	/* The content is kept for the editor, or freed. */
	if (kept != NULL) {
		*kept = content;
		*kept_size = size;
	} else {
		free(content);
	}

	/* Frees the interpretation, keeping its flags for the list. */
	builder->list.flags = run->flags;
	pdf_arena_free(&run->arena);
	free(run->path.verbs);
	free(run->path.points);
	free(run->scratch);
	free(run->text_clip_verbs);
	free(run->text_clip_points);
	free(run);

	/* Succeeded: the list is the page's drawing. */
	pdf_display_finish(builder);
	*list = &builder->list;
	return 0;
}

/*
 * Reads a page's content, one stream or an array of them, decoded and
 * joined with a line end between them (a malloc'd buffer, NULL for none).
 *
 * A stream whose filter is not read yet is left out (PDF_DISPLAY_SKIPPED);
 * a malformed one ends the content there (PDF_DISPLAY_DAMAGED).  Only
 * running out of memory is an error.
 */
static int
read_contents(
	struct pdf_document *document,
	struct pdf_object *page,
	unsigned char **content,
	size_t *size,
	unsigned *flags)
{
	struct pdf_object *contents;
	struct pdf_object *stream;
	size_t item;
	int error;

	/* Finds the page's content. */
	error = pdf_reader_resolve_key(document, page, "Contents", &contents);
	if (error != 0) {
		*flags |= PDF_DISPLAY_DAMAGED;
		return 0;
	}

	/* One stream. */
	if (contents->type == PDF_OBJECT_STREAM) {
		error = append_stream(document, contents, content, size, flags);
		if (error != 0)
			return error;
		return 0;
	}

	/* An array of streams, in order; anything else is no content. */
	if (contents->type != PDF_OBJECT_ARRAY)
		return 0;
	for (item = 0; item < contents->count; item++) {
		/* Finds the stream; a missing or malformed one ends the content. */
		error = pdf_reader_resolve(document, contents->values[item], &stream);
		if (error != 0) {
			*flags |= PDF_DISPLAY_DAMAGED;
			return 0;
		}

		/* An entry that is not a stream ends the content too. */
		if (stream->type != PDF_OBJECT_STREAM) {
			*flags |= PDF_DISPLAY_DAMAGED;
			return 0;
		}

		/* Adds its decoded bytes. */
		error = append_stream(document, stream, content, size, flags);
		if (error != 0)
			return error;
	}

	/* Succeeded: the content is joined. */
	return 0;
}

/* Decodes a content stream and appends it, with a line end, to the content read so far. */
static int
append_stream(
	struct pdf_document *document,
	struct pdf_object *stream,
	unsigned char **content,
	size_t *size,
	unsigned *flags)
{
	const unsigned char *data;
	unsigned char *owned;
	unsigned char *grown;
	size_t data_size;
	int dct;
	int error;

	/* Decodes the stream; one that cannot be decoded is left out. */
	error = pdf_filter_decode(document, stream, 0, &data, &data_size, &owned, &dct);
	if (error == ENOMEM)
		return ENOMEM;
	if (error == ENOTSUP) {
		*flags |= PDF_DISPLAY_SKIPPED;
		return 0;
	}

	/* Any other failure is damage. */
	if (error != 0) {
		*flags |= PDF_DISPLAY_DAMAGED;
		return 0;
	}

	/* Refuses content past the decode limit. */
	if (data_size > PDF_FILTER_OUTPUT_MAX - *size - 1) {
		free(owned);
		*flags |= PDF_DISPLAY_LIMITED;
		return 0;
	}

	/* Grows the content by the stream and a line end. */
	grown = realloc(*content, *size + data_size + 1);
	if (grown == NULL) {
		free(owned);
		return ENOMEM;
	}

	/* Appends the stream's bytes and a line end, so that its last token does not run into the next stream's first. */
	*content = grown;
	memcpy(*content + *size, data, data_size);
	(*content)[*size + data_size] = '\n';
	*size += data_size + 1;
	free(owned);

	/* Succeeded: the stream's bytes are appended. */
	return 0;
}

/*
 * Makes the matrix from the page's user space to its shown space: the crop
 * box's top left at the origin, y downward, the page's rotation applied.
 */
static void
base_matrix(
	const struct pdf_page_box *box,
	double matrix[6])
{
	/* Chooses the matrix by the rotation (clockwise, as shown). */
	switch (box->rotation) {
	case 90:
		matrix[0] = 0.0;
		matrix[1] = 1.0;
		matrix[2] = 1.0;
		matrix[3] = 0.0;
		matrix[4] = -box->crop_bottom;
		matrix[5] = -box->crop_left;
		break;
	case 180:
		matrix[0] = -1.0;
		matrix[1] = 0.0;
		matrix[2] = 0.0;
		matrix[3] = 1.0;
		matrix[4] = box->crop_right;
		matrix[5] = -box->crop_bottom;
		break;
	case 270:
		matrix[0] = 0.0;
		matrix[1] = -1.0;
		matrix[2] = -1.0;
		matrix[3] = 0.0;
		matrix[4] = box->crop_top;
		matrix[5] = box->crop_right;
		break;
	default:
		matrix[0] = 1.0;
		matrix[1] = 0.0;
		matrix[2] = 0.0;
		matrix[3] = -1.0;
		matrix[4] = -box->crop_left;
		matrix[5] = box->crop_top;
		break;
	}
}

/*
 * Runs a content stream: operands gathered until their operator, and the
 * operator executed.
 */
static void
run_content(
	struct content_run *run,
	const unsigned char *data,
	size_t size,
	struct pdf_object *resources)
{
	struct pdf_lexer lexer;
	struct pdf_token token;
	enum content_operator code;
	size_t token_start;
	size_t operator_start;
	size_t first_operand;
	int scanned;
	int error;

	/* Reads the content's tokens with the run's memory; the page's own content is the first level. */
	memset(&lexer, 0, sizeof(lexer));
	lexer.data = data;
	lexer.size = size;
	lexer.arena = &run->arena;
	run->operand_count = 0;
	run->content_level++;
	first_operand = 0;
	scanned = 0;
	if (run->scan != NULL && run->content_level == 1)
		scanned = 1;

	/* Reads tokens until the end, an error or a limit. */
	while (!run->stopped) {
		/* Reads the next token, remembering where it starts. */
		pdf_lexer_skip_space(&lexer);
		token_start = lexer.position;
		error = pdf_lexer_next(&lexer, &token);
		if (error != 0) {
			stop_for(run, error);
			break;
		}

		/* The end of the content ends the run. */
		if (token.type == PDF_TOKEN_END)
			break;

		/* Gathers an operand (the first one starts the operator's bytes). */
		if (token.type != PDF_TOKEN_KEYWORD) {
			if (run->operand_count == 0)
				first_operand = token_start;
			error = read_operand(run, &lexer, &token, token_start);
			if (error != 0) {
				stop_for(run, error);
				break;
			}

			/* The operand waits for its operator. */
			continue;
		}

		/* Counts the operator against the page's limit. */
		run->operators++;
		if (run->operators > PDF_CONTENT_OPERATORS_MAX) {
			run->flags |= PDF_DISPLAY_LIMITED;
			run->stopped = 1;
			break;
		}

		/* Executes the operator with its operands, which it uses up. */
		code = operator_code(&token);
		operator_start = token_start;
		if (run->operand_count > 0)
			operator_start = first_operand;
		if (scanned) {
			run->scan_pending = 0;
			run->scan_show_started = 0;
			run->scan_show_from = run->scan->character_count;
			run->scan_show_flags = 0U;
		}

		/* The operator. */
		execute(run, code, &lexer, resources);
		run->operand_count = 0;

		/* The scan of the page's own content notes what the operator was (its bytes end where the lexer is now). */
		if (scanned)
			scan_operator(run, code, operator_start, token_start, lexer.position, data);
	}

	/* The level ends. */
	run->content_level--;
}

/* Reads one operand: a number, a name, or an array, dictionary or string object. */
static int
read_operand(
	struct content_run *run,
	struct pdf_lexer *lexer,
	const struct pdf_token *token,
	size_t token_start)
{
	struct content_operand *operand;
	struct pdf_object *object;
	int error;

	/* Refuses more operands than any operator takes. */
	if (run->operand_count == PDF_CONTENT_OPERANDS_MAX)
		return PDF_EFORMAT;
	operand = &run->operands[run->operand_count];
	memset(operand, 0, sizeof(*operand));

	/* A number, kept as it is. */
	if (token->type == PDF_TOKEN_INTEGER) {
		operand->type = OPERAND_NUMBER;
		operand->number = (double)token->integer;
		run->operand_count++;
		return 0;
	}

	/* A real number, kept as it is. */
	if (token->type == PDF_TOKEN_REAL) {
		operand->type = OPERAND_NUMBER;
		operand->number = token->real;
		run->operand_count++;
		return 0;
	}

	/* A name, by its decoded bytes. */
	if (token->type == PDF_TOKEN_NAME) {
		operand->type = OPERAND_NAME;
		operand->bytes = token->bytes;
		operand->length = token->length;
		run->operand_count++;
		return 0;
	}

	/* Anything else is parsed as an object from where its token starts. */
	lexer->position = token_start;
	error = pdf_parse_object(lexer, 0, &object);
	if (error != 0)
		return error;
	operand->type = OPERAND_OTHER;
	operand->object = object;
	run->operand_count++;

	/* Succeeded: the operand waits for its operator. */
	return 0;
}

/* Finds an operator's meaning by its name. */
static enum content_operator
operator_code(
	const struct pdf_token *token)
{
	size_t index;
	size_t length;
	int difference;

	/* Compares the name with each known one. */
	for (index = 0; index < sizeof(content_names) / sizeof(content_names[0]); index++) {
		length = strlen(content_names[index].name);
		if (length != token->length)
			continue;
		difference = memcmp(content_names[index].name, token->bytes, length);
		if (difference == 0)
			return content_names[index].code;
	}

	/* An operator the interpreter does not know. */
	return OP_UNKNOWN;
}

/* Executes one operator by its group. */
static void
execute(
	struct content_run *run,
	enum content_operator code,
	struct pdf_lexer *lexer,
	struct pdf_object *resources)
{
	/* Chooses the group of the operator. */
	switch (code) {
	case OP_SAVE:
	case OP_RESTORE:
	case OP_CONCAT:
	case OP_LINE_WIDTH:
	case OP_LINE_CAP:
	case OP_LINE_JOIN:
	case OP_MITER_LIMIT:
	case OP_DASH:
	case OP_EXTGSTATE:
		execute_state(run, code, resources);
		break;
	case OP_MOVE:
	case OP_LINE:
	case OP_CURVE:
	case OP_CURVE_V:
	case OP_CURVE_Y:
	case OP_CLOSE:
	case OP_RECTANGLE:
		execute_path(run, code);
		break;
	case OP_STROKE:
	case OP_CLOSE_STROKE:
	case OP_FILL:
	case OP_FILL_EVEN_ODD:
	case OP_FILL_STROKE:
	case OP_FILL_STROKE_EVEN_ODD:
	case OP_CLOSE_FILL_STROKE:
	case OP_CLOSE_FILL_STROKE_EVEN_ODD:
	case OP_END_PATH:
	case OP_CLIP:
	case OP_CLIP_EVEN_ODD:
		execute_paint(run, code);
		break;
	case OP_GRAY_FILL:
	case OP_GRAY_STROKE:
	case OP_RGB_FILL:
	case OP_RGB_STROKE:
	case OP_CMYK_FILL:
	case OP_CMYK_STROKE:
	case OP_SPACE_FILL:
	case OP_SPACE_STROKE:
	case OP_COLOR_FILL:
	case OP_COLOR_STROKE:
		execute_color(run, code, resources);
		break;
	case OP_XOBJECT:
		draw_xobject(run, resources);
		break;
	case OP_INLINE_IMAGE:
		draw_inline_image(run, lexer, resources);
		break;
	case OP_SHADING:
		paint_shading(run, resources);
		break;
	case OP_TEXT_BEGIN:
	case OP_TEXT_END:
	case OP_TEXT_FONT:
	case OP_TEXT_CHARACTER_SPACING:
	case OP_TEXT_WORD_SPACING:
	case OP_TEXT_SCALE:
	case OP_TEXT_LEADING:
	case OP_TEXT_RISE:
	case OP_TEXT_RENDER:
	case OP_TEXT_MOVE:
	case OP_TEXT_MOVE_LEADING:
	case OP_TEXT_MATRIX:
	case OP_TEXT_NEXT_LINE:
	case OP_TEXT_SHOW:
	case OP_TEXT_SHOW_ARRAY:
	case OP_TEXT_NEXT_SHOW:
	case OP_TEXT_SPACED_SHOW:
		execute_text(run, code, resources);
		break;
	case OP_IGNORED:
	case OP_UNKNOWN:
		break;
	}
}

/* Executes an operator of the graphics state. */
static void
execute_state(
	struct content_run *run,
	enum content_operator code,
	struct pdf_object *resources)
{
	struct content_state *state;
	double numbers[6];
	int error;

	/* The level in force. */
	state = &run->stack[run->depth];

	/* Changes the state by the operator. */
	switch (code) {
	case OP_SAVE:
		save_state(run);
		break;
	case OP_RESTORE:
		restore_state(run);
		break;
	case OP_CONCAT:
		error = read_numbers(run, 6, numbers);
		if (error == 0)
			concat_matrix(state->ctm, numbers);
		break;
	case OP_LINE_WIDTH:
		/* A width that is not negative. */
		error = read_numbers(run, 1, numbers);
		if (error != 0)
			break;
		if (numbers[0] >= 0.0)
			state->line_width = numbers[0];
		break;
	case OP_LINE_CAP:
		/* One of the three caps. */
		error = read_numbers(run, 1, numbers);
		if (error != 0)
			break;
		if (numbers[0] >= 0.0 && numbers[0] <= 2.0)
			state->line_cap = (int)numbers[0];
		break;
	case OP_LINE_JOIN:
		/* One of the three joins. */
		error = read_numbers(run, 1, numbers);
		if (error != 0)
			break;
		if (numbers[0] >= 0.0 && numbers[0] <= 2.0)
			state->line_join = (int)numbers[0];
		break;
	case OP_MITER_LIMIT:
		/* A limit of at least 1. */
		error = read_numbers(run, 1, numbers);
		if (error != 0)
			break;
		if (numbers[0] >= 1.0)
			state->miter_limit = numbers[0];
		break;
	case OP_DASH:
		/* An array and a phase. */
		if (run->operand_count != 2)
			break;
		if (run->operands[0].type != OPERAND_OTHER)
			break;
		if (run->operands[1].type != OPERAND_NUMBER)
			break;
		set_dash(state, run->operands[0].object, run->operands[1].number);
		break;
	case OP_EXTGSTATE:
		apply_extgstate(run, resources);
		break;
	default:
		break;
	}
}

/* Executes a path construction operator. */
static void
execute_path(
	struct content_run *run,
	enum content_operator code)
{
	struct content_path *path;
	double numbers[6];
	int error;

	/* The path being built. */
	path = &run->path;

	/* Adds to the path by the operator; a close takes no coordinates. */
	memset(numbers, 0, sizeof(numbers));
	error = 0;
	switch (code) {
	case OP_MOVE:
		error = read_numbers(run, 2, numbers);
		if (error == 0)
			error = path_add(run, PDF_PATH_MOVE, numbers, 1);
		break;
	case OP_LINE:
		/* A line from the current point, which it needs. */
		error = read_numbers(run, 2, numbers);
		if (error != 0)
			break;
		if (path->has_current)
			error = path_add(run, PDF_PATH_LINE, numbers, 1);
		break;
	case OP_CURVE:
		/* A curve from the current point, which it needs. */
		error = read_numbers(run, 6, numbers);
		if (error != 0)
			break;
		if (path->has_current)
			error = path_add(run, PDF_PATH_CUBIC, numbers, 3);
		break;
	case OP_CURVE_V:
		/* The first control point is the current point. */
		error = read_numbers(run, 4, numbers + 2);
		if (error != 0)
			break;
		if (!path->has_current)
			break;
		numbers[0] = path->current.x;
		numbers[1] = path->current.y;
		error = path_add(run, PDF_PATH_CUBIC, numbers, 3);
		break;
	case OP_CURVE_Y:
		/* The second control point is the end point. */
		error = read_numbers(run, 4, numbers);
		if (error != 0)
			break;
		if (!path->has_current)
			break;
		numbers[4] = numbers[2];
		numbers[5] = numbers[3];
		error = path_add(run, PDF_PATH_CUBIC, numbers, 3);
		break;
	case OP_CLOSE:
		if (path->has_current)
			error = path_add(run, PDF_PATH_CLOSE, numbers, 0);
		break;
	case OP_RECTANGLE:
		/* A rectangle is a closed subpath of its four corners, from its origin. */
		error = read_numbers(run, 4, numbers);
		if (error != 0)
			break;
		error = add_rectangle(run, numbers[0], numbers[1], numbers[0] + numbers[2], numbers[1] + numbers[3]);
		break;
	default:
		break;
	}

	/* A path past the limit stops the page. */
	if (error == ENOMEM)
		stop_for(run, ENOMEM);
}

/* Executes a painting or clipping operator; each painting operator ends the path. */
static void
execute_paint(
	struct content_run *run,
	enum content_operator code)
{
	double none[2];

	/* Paints, or marks the clip that the next painting operator applies. */
	none[0] = 0.0;
	none[1] = 0.0;
	switch (code) {
	case OP_CLIP:
		run->clip_pending = 1;
		run->clip_rule = PDF_FILL_NONZERO;
		return;
	case OP_CLIP_EVEN_ODD:
		run->clip_pending = 1;
		run->clip_rule = PDF_FILL_EVEN_ODD;
		return;
	case OP_STROKE:
		paint_path(run, 0, PDF_FILL_NONZERO, 1);
		break;
	case OP_CLOSE_STROKE:
		if (run->path.has_current)
			(void)path_add(run, PDF_PATH_CLOSE, none, 0);
		paint_path(run, 0, PDF_FILL_NONZERO, 1);
		break;
	case OP_FILL:
		paint_path(run, 1, PDF_FILL_NONZERO, 0);
		break;
	case OP_FILL_EVEN_ODD:
		paint_path(run, 1, PDF_FILL_EVEN_ODD, 0);
		break;
	case OP_FILL_STROKE:
		paint_path(run, 1, PDF_FILL_NONZERO, 1);
		break;
	case OP_FILL_STROKE_EVEN_ODD:
		paint_path(run, 1, PDF_FILL_EVEN_ODD, 1);
		break;
	case OP_CLOSE_FILL_STROKE:
		if (run->path.has_current)
			(void)path_add(run, PDF_PATH_CLOSE, none, 0);
		paint_path(run, 1, PDF_FILL_NONZERO, 1);
		break;
	case OP_CLOSE_FILL_STROKE_EVEN_ODD:
		if (run->path.has_current)
			(void)path_add(run, PDF_PATH_CLOSE, none, 0);
		paint_path(run, 1, PDF_FILL_EVEN_ODD, 1);
		break;
	case OP_END_PATH:
		paint_path(run, 0, PDF_FILL_NONZERO, 0);
		break;
	default:
		break;
	}
}

/* Executes a colour operator. */
static void
execute_color(
	struct content_run *run,
	enum content_operator code,
	struct pdf_object *resources)
{
	struct content_state *state;
	double numbers[4];
	int components;
	int stroke;
	int is_pattern;
	int in_pattern;
	int error;

	/* The level in force, and whether the operator sets the stroke's colour. */
	state = &run->stack[run->depth];
	stroke = 0;
	if (code == OP_GRAY_STROKE ||
	    code == OP_RGB_STROKE ||
	    code == OP_CMYK_STROKE ||
	    code == OP_SPACE_STROKE ||
	    code == OP_COLOR_STROKE)
		stroke = 1;

	/* Sets the colour, and the device space it is in, by the operator. */
	switch (code) {
	case OP_GRAY_FILL:
	case OP_GRAY_STROKE:
		error = read_numbers(run, 1, numbers);
		if (error == 0)
			set_color(run, stroke, 1, numbers);
		break;
	case OP_RGB_FILL:
	case OP_RGB_STROKE:
		error = read_numbers(run, 3, numbers);
		if (error == 0)
			set_color(run, stroke, 3, numbers);
		break;
	case OP_CMYK_FILL:
	case OP_CMYK_STROKE:
		error = read_numbers(run, 4, numbers);
		if (error == 0)
			set_color(run, stroke, 4, numbers);
		break;
	case OP_SPACE_FILL:
	case OP_SPACE_STROKE:
		/* The Pattern space has no colour until scn names a pattern. */
		is_pattern = is_pattern_space(run, resources);
		if (is_pattern) {
			if (stroke) {
				state->stroke_usable = 0;
				state->stroke_pattern_space = 1;
				state->stroke_pattern = NULL;
			} else {
				state->fill_usable = 0;
				state->fill_pattern_space = 1;
				state->fill_pattern = NULL;
			}

			/* The colour waits for scn to name a pattern. */
			break;
		}

		/* A new space starts at its initial colour, black. */
		error = space_components(run, resources, &components);
		numbers[0] = 0.0;
		numbers[1] = 0.0;
		numbers[2] = 0.0;
		numbers[3] = 1.0;
		if (error != 0) {
			/* A space the interpreter cannot read makes the colour unusable. */
			if (stroke) {
				state->stroke_usable = 0;
			} else {
				state->fill_usable = 0;
			}

			break;
		}

		/* The space's initial colour. */
		if (components == 1)
			numbers[0] = 0.0;
		set_color(run, stroke, components, numbers);
		break;
	case OP_COLOR_FILL:
	case OP_COLOR_STROKE:
		/* A pattern's name in the Pattern space. */
		in_pattern = state->fill_pattern_space;
		if (stroke)
			in_pattern = state->stroke_pattern_space;
		if (in_pattern) {
			set_pattern(run, resources, stroke);
			break;
		}

		/* The values of the space in force; a wrong count is not read. */
		components = state->fill_components;
		if (stroke)
			components = state->stroke_components;
		error = read_numbers(run, (size_t)components, numbers);
		if (error == 0)
			set_color(run, stroke, components, numbers);
		break;
	default:
		break;
	}
}

/* Pushes a copy of the graphics state (q). */
static void
save_state(
	struct content_run *run)
{
	/* Past the limit, a q is only counted, so that its Q is matched. */
	if (run->depth + 1 >= PDF_CONTENT_STACK_MAX) {
		run->ignored_saves++;
		run->flags |= PDF_DISPLAY_LIMITED;
		return;
	}

	/* The new level starts as a copy of the old, with no clips of its own. */
	run->stack[run->depth + 1] = run->stack[run->depth];
	run->stack[run->depth + 1].clips = 0;
	run->depth++;
}

/* Pops the graphics state (Q), ending the clips pushed at the level. */
static void
restore_state(
	struct content_run *run)
{
	/* A Q of a q past the limit only uncounts it. */
	if (run->ignored_saves > 0) {
		run->ignored_saves--;
		return;
	}

	/* A Q without its q (or below a running form's start) does nothing. */
	if (run->depth <= run->base_depth)
		return;

	/* Pops the level. */
	unwind_to(run, run->depth - 1);
}

/* Pops the levels above a depth, ending each popped level's clips on the list. */
static void
unwind_to(
	struct content_run *run,
	size_t depth)
{
	/* Pops each level above the depth. */
	while (run->depth > depth) {
		end_clips(run);
		run->depth--;
	}
}

/* Ends the clips pushed at the level in force. */
static void
end_clips(
	struct content_run *run)
{
	size_t clip;
	int error;

	/* Pops each of the level's clips; the pops balance the pushes even past a limit. */
	for (clip = 0; clip < run->stack[run->depth].clips; clip++) {
		error = pdf_display_add_clip_pop(run->builder);
		if (error != 0)
			run->flags |= PDF_DISPLAY_LIMITED;
		run->clip_depth--;
	}

	/* The level holds no clip any more. */
	run->stack[run->depth].clips = 0;
}

/* Concatenates a matrix before the CTM (the new user space is the matrix applied in the old one). */
static void
concat_matrix(
	double ctm[6],
	const double matrix[6])
{
	double result[6];

	/* result = matrix x ctm, in PDF's row-vector convention. */
	result[0] = matrix[0] * ctm[0] + matrix[1] * ctm[2];
	result[1] = matrix[0] * ctm[1] + matrix[1] * ctm[3];
	result[2] = matrix[2] * ctm[0] + matrix[3] * ctm[2];
	result[3] = matrix[2] * ctm[1] + matrix[3] * ctm[3];
	result[4] = matrix[4] * ctm[0] + matrix[5] * ctm[2] + ctm[4];
	result[5] = matrix[4] * ctm[1] + matrix[5] * ctm[3] + ctm[5];

	/* The CTM becomes the product. */
	memcpy(ctm, result, sizeof(result));
}

/*
 * Reads the operator's last count operands as finite numbers.
 *
 * Extra operands before them are ignored; fewer, or anything but numbers,
 * reports PDF_EFORMAT and the operator does nothing.
 */
static int
read_numbers(
	const struct content_run *run,
	size_t count,
	double *numbers)
{
	size_t first;
	size_t index;
	double value;

	/* Refuses too few operands. */
	if (run->operand_count < count)
		return PDF_EFORMAT;
	first = run->operand_count - count;

	/* Takes each number, refusing anything else, infinities and NaN. */
	for (index = 0; index < count; index++) {
		if (run->operands[first + index].type != OPERAND_NUMBER)
			return PDF_EFORMAT;
		value = run->operands[first + index].number;
		if (!(value > -1e30 && value < 1e30))
			return PDF_EFORMAT;
		numbers[index] = value;
	}

	/* Succeeded: numbers holds the operands. */
	return 0;
}

/*
 * Sets the dash pattern from an array and a phase; a pattern that is
 * empty, negative, too long or of no length makes the line solid.
 */
static void
set_dash(
	struct content_state *state,
	const struct pdf_object *array,
	double phase)
{
	double total;
	double value;
	size_t index;
	int error;

	/* A solid line unless the array is a usable pattern. */
	state->dash_count = 0;
	state->dash_phase = 0.0;
	if (array->type != PDF_OBJECT_ARRAY)
		return;
	if (array->count == 0 || array->count > PDF_CONTENT_DASH_MAX)
		return;

	/* Reads each entry, which must be a number that is not negative. */
	total = 0.0;
	for (index = 0; index < array->count; index++) {
		error = pdf_object_number(array->values[index], &value);
		if (error != 0)
			return;
		if (!(value >= 0.0 && value < 1e6))
			return;
		state->dash[index] = value;
		total += value;
	}

	/* A pattern of no length is solid; the phase is kept within one period. */
	if (total < 1e-3)
		return;
	if (!(phase >= 0.0 && phase < 1e9))
		phase = 0.0;
	phase = fmod(phase, total);

	/* An odd count repeats itself to make the dashes and the gaps alternate. */
	if (array->count % 2 == 1 && array->count * 2 <= PDF_CONTENT_DASH_MAX) {
		for (index = 0; index < array->count; index++)
			state->dash[array->count + index] = state->dash[index];
		state->dash_count = array->count * 2;
		state->dash_phase = fmod(phase, total * 2.0);
		return;
	}

	/* An odd count too long to repeat leaves the line solid. */
	if (array->count % 2 == 1)
		return;

	/* The pattern in force. */
	state->dash_count = array->count;
	state->dash_phase = phase;
}

/* Applies an ExtGState resource (gs): the alphas, the blend mode and the line style it sets. */
static void
apply_extgstate(
	struct content_run *run,
	struct pdf_object *resources)
{
	struct content_state *state;
	struct pdf_object *dictionary;
	struct pdf_object *value;
	double number;
	long integer;
	int found;
	int error;

	/* Finds the named ExtGState; a missing one does nothing. */
	state = &run->stack[run->depth];
	error = find_resource(run, resources, "ExtGState", &dictionary);
	if (error != 0)
		return;
	if (dictionary->type != PDF_OBJECT_DICTIONARY)
		return;

	/* The fill alpha. */
	found = state_number(run, dictionary, "ca", &number);
	if (found)
		state->fill_alpha = clamp_unit(number);

	/* The stroke alpha. */
	found = state_number(run, dictionary, "CA", &number);
	if (found)
		state->stroke_alpha = clamp_unit(number);

	/* The line width, which cannot be negative. */
	found = state_number(run, dictionary, "LW", &number);
	if (found && number >= 0.0)
		state->line_width = number;

	/* The line cap, one of the three. */
	found = state_integer(run, dictionary, "LC", &integer);
	if (found &&
	    integer >= 0 &&
	    integer <= 2)
		state->line_cap = (int)integer;

	/* The line join, one of the three. */
	found = state_integer(run, dictionary, "LJ", &integer);
	if (found &&
	    integer >= 0 &&
	    integer <= 2)
		state->line_join = (int)integer;

	/* The miter limit, at least 1. */
	found = state_number(run, dictionary, "ML", &number);
	if (found && number >= 1.0)
		state->miter_limit = number;

	/* The dash pattern: an array and a phase. */
	state_dash(run, dictionary);

	/* The blend mode. */
	state_blend(run, dictionary);

	/* A soft mask is drawn from stage 3. */
	error = pdf_reader_resolve_key(run->document, dictionary, "SMask", &value);
	if (error != 0)
		return;
	if (value->type == PDF_OBJECT_NULL)
		return;
	found = pdf_object_is_name(value, "None");
	if (!found)
		run->flags |= PDF_DISPLAY_SKIPPED;
}

/* Reads a number of an ExtGState; reports whether it has one. */
static int
state_number(
	struct content_run *run,
	struct pdf_object *dictionary,
	const char *key,
	double *number)
{
	struct pdf_object *value;
	int error;

	/* Finds the key's value. */
	error = pdf_reader_resolve_key(run->document, dictionary, key, &value);
	if (error != 0)
		return 0;

	/* Only a number counts. */
	error = pdf_object_number(value, number);
	if (error != 0)
		return 0;

	/* The ExtGState has the number. */
	return 1;
}

/* Reads an integer of an ExtGState; reports whether it has one. */
static int
state_integer(
	struct content_run *run,
	struct pdf_object *dictionary,
	const char *key,
	long *integer)
{
	struct pdf_object *value;
	int error;

	/* Finds the key's value. */
	error = pdf_reader_resolve_key(run->document, dictionary, key, &value);
	if (error != 0)
		return 0;

	/* Only an integer counts. */
	if (value->type != PDF_OBJECT_INTEGER)
		return 0;

	/* The ExtGState has the integer. */
	*integer = value->integer;
	return 1;
}

/* Applies an ExtGState's dash pattern (D: an array and a phase), when it has a usable one. */
static void
state_dash(
	struct content_run *run,
	struct pdf_object *dictionary)
{
	struct pdf_object *value;
	struct pdf_object *pattern;
	struct pdf_object *phase;
	double number;
	int error;

	/* The value must be a pair. */
	error = pdf_reader_resolve_key(run->document, dictionary, "D", &value);
	if (error != 0)
		return;
	if (value->type != PDF_OBJECT_ARRAY)
		return;
	if (value->count != 2)
		return;

	/* Its phase, a number. */
	error = pdf_reader_resolve(run->document, value->values[1], &phase);
	if (error != 0)
		return;
	error = pdf_object_number(phase, &number);
	if (error != 0)
		return;

	/* Its pattern, which set_dash checks. */
	error = pdf_reader_resolve(run->document, value->values[0], &pattern);
	if (error != 0)
		return;
	set_dash(&run->stack[run->depth], pattern, number);
}

/*
 * Applies an ExtGState's blend mode (BM): a name, or an array whose first
 * mode the interpreter draws wins.  Normal and Compatible draw normally,
 * Multiply multiplies; any other mode draws normally and the list says so.
 */
static void
state_blend(
	struct content_run *run,
	struct pdf_object *dictionary)
{
	struct pdf_object *value;
	struct pdf_object *mode;
	size_t index;
	int known;
	int error;

	/* No mode leaves the blend mode as it is. */
	error = pdf_reader_resolve_key(run->document, dictionary, "BM", &value);
	if (error != 0)
		return;
	if (value->type == PDF_OBJECT_NULL)
		return;

	/* A name is the mode itself. */
	if (value->type != PDF_OBJECT_ARRAY) {
		apply_blend(run, value);
		return;
	}

	/* An array's first mode the interpreter knows. */
	for (index = 0; index < value->count; index++) {
		error = pdf_reader_resolve(run->document, value->values[index], &mode);
		if (error != 0)
			return;
		known = blend_known(mode);
		if (known) {
			apply_blend(run, mode);
			return;
		}
	}

	/* An array of unknown modes draws normally. */
	run->stack[run->depth].blend = PDF_BLEND_NORMAL;
	run->flags |= PDF_DISPLAY_SKIPPED;
}

/* Sets the blend mode a name gives; an unknown one draws normally and marks the list. */
static void
apply_blend(
	struct content_run *run,
	struct pdf_object *mode)
{
	int is_multiply;
	int known;

	/* Multiply darkens by multiplying the colours. */
	is_multiply = pdf_object_is_name(mode, "Multiply");
	if (is_multiply) {
		run->stack[run->depth].blend = PDF_BLEND_MULTIPLY;
		return;
	}

	/* Everything else draws normally; a mode other than Normal is left out. */
	run->stack[run->depth].blend = PDF_BLEND_NORMAL;
	known = blend_known(mode);
	if (!known)
		run->flags |= PDF_DISPLAY_SKIPPED;
}

/* Tells whether a blend mode is one the interpreter draws (Normal, Compatible, Multiply). */
static int
blend_known(
	struct pdf_object *mode)
{
	int is_name;

	/* The three names. */
	is_name = pdf_object_is_name(mode, "Normal");
	if (is_name)
		return 1;
	is_name = pdf_object_is_name(mode, "Compatible");
	if (is_name)
		return 1;
	is_name = pdf_object_is_name(mode, "Multiply");
	if (is_name)
		return 1;

	/* Any other mode. */
	return 0;
}

/*
 * Finds the resource the operator's last operand names in a category of
 * the resources (ExtGState, XObject, ColorSpace).
 */
static int
find_resource(
	struct content_run *run,
	struct pdf_object *resources,
	const char *category,
	struct pdf_object **found)
{
	int error;

	/* The name is the last operand. */
	if (run->operand_count == 0)
		return PDF_EFORMAT;

	/* Finds the resource it names. */
	error = find_named_resource(run, resources, category, &run->operands[run->operand_count - 1], found);
	if (error != 0)
		return error;

	/* Succeeded: found is the resource. */
	return 0;
}

/* Finds the resource an operand names in a category of the resources. */
static int
find_named_resource(
	struct content_run *run,
	struct pdf_object *resources,
	const char *category,
	const struct content_operand *operand,
	struct pdf_object **found)
{
	struct pdf_object *dictionary;
	size_t index;
	int differs;
	int error;

	/* Only a name names a resource. */
	if (operand->type != OPERAND_NAME)
		return PDF_EFORMAT;

	/* Finds the category's dictionary. */
	if (resources == NULL)
		return ENOENT;
	if (resources->type != PDF_OBJECT_DICTIONARY)
		return ENOENT;
	error = pdf_reader_resolve_key(run->document, resources, category, &dictionary);
	if (error != 0)
		return error;
	if (dictionary->type != PDF_OBJECT_DICTIONARY)
		return ENOENT;

	/* Finds the name among its keys (compared by bytes, since a name may hold a NUL). */
	for (index = 0; index < dictionary->count; index++) {
		if (dictionary->keys[index]->length != operand->length)
			continue;
		differs = memcmp(dictionary->keys[index]->bytes, operand->bytes, operand->length);
		if (differs != 0)
			continue;
		error = pdf_reader_resolve(run->document, dictionary->values[index], found);
		if (error != 0)
			return error;
		return 0;
	}

	/* The resources do not have the name. */
	return ENOENT;
}

/*
 * Tells how many components the colour space named by the operand has:
 * a device space by its name, or a space of the resources built on one.
 */
static int
space_components(
	struct content_run *run,
	struct pdf_object *resources,
	int *components)
{
	const struct content_operand *operand;
	struct pdf_object *space;
	struct pdf_object *family;
	struct pdf_object *count;
	int is_name;
	int error;

	/* The name is the last operand. */
	if (run->operand_count == 0)
		return PDF_EFORMAT;
	operand = &run->operands[run->operand_count - 1];
	if (operand->type != OPERAND_NAME)
		return PDF_EFORMAT;

	/* The device spaces by name. */
	*components = device_components(operand->bytes, operand->length);
	if (*components != 0)
		return 0;

	/* Any other name is a resource: a device space's name, or an array whose family decides. */
	error = find_resource(run, resources, "ColorSpace", &space);
	if (error != 0)
		return error;
	if (space->type == PDF_OBJECT_NAME) {
		*components = device_components(space->bytes, space->length);
		if (*components == 0)
			return ENOTSUP;
		return 0;
	}

	/* Any other space is an array whose first element names its family. */
	if (space->type != PDF_OBJECT_ARRAY)
		return PDF_EFORMAT;
	if (space->count == 0)
		return PDF_EFORMAT;
	error = pdf_reader_resolve(run->document, space->values[0], &family);
	if (error != 0)
		return error;

	/* The calibrated spaces. */
	is_name = pdf_object_is_name(family, "CalGray");
	if (is_name) {
		*components = 1;
		return 0;
	}

	/* The calibrated RGB space. */
	is_name = pdf_object_is_name(family, "CalRGB");
	if (is_name) {
		*components = 3;
		return 0;
	}

	/* An ICC-based space by its profile's component count; anything else is from later stages. */
	is_name = pdf_object_is_name(family, "ICCBased");
	if (!is_name)
		return ENOTSUP;
	if (space->count < 2)
		return ENOTSUP;
	error = pdf_reader_resolve(run->document, space->values[1], &family);
	if (error != 0)
		return error;
	if (family->type != PDF_OBJECT_STREAM)
		return PDF_EFORMAT;
	error = pdf_reader_resolve_key(run->document, family, "N", &count);
	if (error != 0)
		return error;
	if (count->type != PDF_OBJECT_INTEGER)
		return PDF_EFORMAT;
	if (count->integer != 1 &&
	    count->integer != 3 &&
	    count->integer != 4)
		return ENOTSUP;

	/* Succeeded: the space's component count. */
	*components = (int)count->integer;
	return 0;
}

/* Tells how many components a device colour space's name has (0 for another name). */
static int
device_components(
	const unsigned char *name,
	size_t length)
{
	int differs;

	/* DeviceGray. */
	if (length == 10) {
		differs = memcmp(name, "DeviceGray", 10);
		if (differs == 0)
			return 1;
	}

	/* DeviceRGB. */
	if (length == 9) {
		differs = memcmp(name, "DeviceRGB", 9);
		if (differs == 0)
			return 3;
	}

	/* DeviceCMYK. */
	if (length == 10) {
		differs = memcmp(name, "DeviceCMYK", 10);
		if (differs == 0)
			return 4;
	}

	/* Another name. */
	return 0;
}

/* Sets the fill or stroke colour from a device space's values (1 gray, 3 RGB, 4 CMYK). */
static void
set_color(
	struct content_run *run,
	int stroke,
	int components,
	const double *values)
{
	struct content_state *state;
	double rgb[3];

	/* Converts the values to RGB. */
	if (components == 1) {
		rgb[0] = clamp_unit(values[0]);
		rgb[1] = rgb[0];
		rgb[2] = rgb[0];
	} else if (components == 3) {
		rgb[0] = clamp_unit(values[0]);
		rgb[1] = clamp_unit(values[1]);
		rgb[2] = clamp_unit(values[2]);
	} else {
		rgb[0] = (1.0 - clamp_unit(values[0])) * (1.0 - clamp_unit(values[3]));
		rgb[1] = (1.0 - clamp_unit(values[1])) * (1.0 - clamp_unit(values[3]));
		rgb[2] = (1.0 - clamp_unit(values[2])) * (1.0 - clamp_unit(values[3]));
	}

	/* Stores the colour and its space in the level in force, which is no longer a pattern. */
	state = &run->stack[run->depth];
	if (stroke) {
		memcpy(state->stroke, rgb, sizeof(rgb));
		state->stroke_components = components;
		state->stroke_usable = 1;
		state->stroke_pattern_space = 0;
		state->stroke_pattern = NULL;
	} else {
		memcpy(state->fill, rgb, sizeof(rgb));
		state->fill_components = components;
		state->fill_usable = 1;
		state->fill_pattern_space = 0;
		state->fill_pattern = NULL;
	}
}

/* Adds a segment to the path (count points from coordinates), tracking the current point. */
static int
path_add(
	struct content_run *run,
	enum pdf_path_verb verb,
	const double *coordinates,
	size_t count)
{
	struct content_path *path;
	unsigned char *verbs;
	struct pdf_point *points;
	double start[2];
	size_t capacity;
	size_t index;
	int after_close;
	int error;

	/* Whether the path's last segment closed its subpath. */
	path = &run->path;
	after_close = 0;
	if (path->verb_count > 0 && path->verbs[path->verb_count - 1] == PDF_PATH_CLOSE)
		after_close = 1;

	/* A close right after a close adds nothing. */
	if (verb == PDF_PATH_CLOSE && after_close)
		return 0;

	/* A line or a curve after a close starts a new subpath at the closed subpath's start, as PDF defines. */
	if (after_close &&
	    (verb == PDF_PATH_LINE ||
	     verb == PDF_PATH_CUBIC)) {
		start[0] = path->start.x;
		start[1] = path->start.y;
		error = path_add(run, PDF_PATH_MOVE, start, 1);
		if (error != 0)
			return error;
	}

	/* Refuses a path past the list's point limit. */
	if (path->point_count + count > PDF_DISPLAY_POINTS_MAX)
		return ENOMEM;
	if (path->verb_count + 1 > PDF_DISPLAY_POINTS_MAX)
		return ENOMEM;

	/* Grows the verbs when full. */
	if (path->verb_count == path->verb_capacity) {
		capacity = path->verb_capacity * 2;
		if (capacity == 0)
			capacity = 64;
		verbs = realloc(path->verbs, capacity);
		if (verbs == NULL)
			return ENOMEM;
		path->verbs = verbs;
		path->verb_capacity = capacity;
	}

	/* Grows the points when they do not fit. */
	if (path->point_count + count > path->point_capacity) {
		capacity = path->point_capacity * 2;
		if (capacity == 0)
			capacity = 64;
		while (capacity < path->point_count + count)
			capacity *= 2;
		points = realloc(path->points, capacity * sizeof(*points));
		if (points == NULL)
			return ENOMEM;
		path->points = points;
		path->point_capacity = capacity;
	}

	/* Appends the verb and its points. */
	path->verbs[path->verb_count] = (unsigned char)verb;
	path->verb_count++;
	for (index = 0; index < count; index++) {
		path->points[path->point_count].x = coordinates[index * 2];
		path->points[path->point_count].y = coordinates[index * 2 + 1];
		path->point_count++;
	}

	/* The current point: the segment's end, a move's start, or a closed subpath's start. */
	if (verb == PDF_PATH_CLOSE) {
		path->current = path->start;
	} else {
		path->current = path->points[path->point_count - 1];
	}

	/* A move starts a new subpath there. */
	if (verb == PDF_PATH_MOVE)
		path->start = path->current;
	path->has_current = 1;

	/* Succeeded: the segment is part of the path. */
	return 0;
}

/* Adds a closed rectangle subpath from one corner to the opposite one. */
static int
add_rectangle(
	struct content_run *run,
	double left,
	double bottom,
	double right,
	double top)
{
	double corners[8];
	int error;

	/* The corners from the first, around. */
	corners[0] = left;
	corners[1] = bottom;
	corners[2] = right;
	corners[3] = bottom;
	corners[4] = right;
	corners[5] = top;
	corners[6] = left;
	corners[7] = top;

	/* The move to the first corner. */
	error = path_add(run, PDF_PATH_MOVE, corners, 1);
	if (error != 0)
		return error;

	/* The three sides to the other corners. */
	error = path_add(run, PDF_PATH_LINE, corners + 2, 1);
	if (error != 0)
		return error;
	error = path_add(run, PDF_PATH_LINE, corners + 4, 1);
	if (error != 0)
		return error;
	error = path_add(run, PDF_PATH_LINE, corners + 6, 1);
	if (error != 0)
		return error;

	/* The close back to the first corner. */
	error = path_add(run, PDF_PATH_CLOSE, corners, 0);
	if (error != 0)
		return error;

	/* Succeeded: the rectangle is a subpath. */
	return 0;
}

/* Ends the path being built. */
static void
path_clear(
	struct content_run *run)
{
	/* The arrays are kept for the next path. */
	run->path.verb_count = 0;
	run->path.point_count = 0;
	run->path.has_current = 0;
}

/* Paints the path (fill, stroke, both, or neither), applies a pending clip, and ends the path. */
static void
paint_path(
	struct content_run *run,
	int fill,
	enum pdf_fill_rule rule,
	int stroke)
{
	/* Paints only a path that has something. */
	if (run->path.verb_count > 0 && !run->stopped) {
		if (fill)
			fill_path(run, rule);
		if (stroke && !run->stopped)
			stroke_path(run);
		if (run->clip_pending && !run->stopped)
			push_clip(run);
	}

	/* The clip and the path are used up. */
	run->clip_pending = 0;
	path_clear(run);
}

/* Fills the path in the fill colour. */
static void
fill_path(
	struct content_run *run,
	enum pdf_fill_rule rule)
{
	struct content_state *state;
	struct pdf_display_item style;
	struct pdf_point *transformed;
	int error;

	/* A colour the interpreter cannot read is not painted. */
	state = &run->stack[run->depth];
	if (!state->fill_usable) {
		run->flags |= PDF_DISPLAY_SKIPPED;
		return;
	}

	/* Transforms the path to the page. */
	error = transform_path(run, run->path.points, run->path.point_count, &transformed);
	if (error != 0) {
		stop_for(run, error);
		return;
	}

	/* A shading pattern fills the path with its shading. */
	if (state->fill_pattern != NULL) {
		paint_pattern(run, state->fill_pattern, run->path.verbs, run->path.verb_count, transformed, run->path.point_count, rule, state->fill_alpha);
		return;
	}

	/* Adds the fill. */
	memset(&style, 0, sizeof(style));
	style.rule = rule;
	style.red = state->fill[0];
	style.green = state->fill[1];
	style.blue = state->fill[2];
	style.alpha = state->fill_alpha;
	style.blend = state->blend;
	error = pdf_display_add_path(run->builder, PDF_ITEM_FILL, run->path.verbs, run->path.verb_count, transformed, run->path.point_count, &style);
	if (error != 0)
		stop_for(run, error);
}

/* Strokes the path in the stroke colour: its outline, from the stroker, filled. */
static void
stroke_path(
	struct content_run *run)
{
	struct content_state *state;
	struct pdf_stroke_style stroke;
	struct pdf_display_item style;
	struct pdf_point *transformed;
	unsigned char *verbs;
	struct pdf_point *points;
	size_t verb_count;
	size_t point_count;
	double scale;
	int error;

	/* A colour the interpreter cannot read is not painted. */
	state = &run->stack[run->depth];
	if (!state->stroke_usable) {
		run->flags |= PDF_DISPLAY_SKIPPED;
		return;
	}

	/* A user space squashed to nothing draws nothing. */
	scale = matrix_scale(state->ctm);
	if (scale < 1e-9)
		return;

	/* The style in user space: the tolerance and the thinnest line are the page's, scaled back. */
	memset(&stroke, 0, sizeof(stroke));
	stroke.width = state->line_width;
	if (stroke.width * scale < PDF_CONTENT_HAIRLINE && stroke.width <= 0.0)
		stroke.width = PDF_CONTENT_HAIRLINE / scale;
	stroke.cap = state->line_cap;
	stroke.join = state->line_join;
	stroke.miter_limit = state->miter_limit;
	stroke.dash = state->dash;
	stroke.dash_count = state->dash_count;
	stroke.dash_phase = state->dash_phase;
	stroke.tolerance = PDF_CONTENT_TOLERANCE / scale;

	/* Makes the outline. */
	error = pdf_stroke_path(run->path.verbs, run->path.verb_count, run->path.points, run->path.point_count, &stroke, &verbs, &verb_count, &points, &point_count);
	if (error == PDF_EFORMAT)
		return;
	if (error != 0) {
		stop_for(run, error);
		return;
	}

	/* Transforms it to the page. */
	error = transform_path(run, points, point_count, &transformed);
	if (error != 0) {
		free(verbs);
		free(points);
		stop_for(run, error);
		return;
	}

	/* A shading pattern fills the outline with its shading. */
	if (state->stroke_pattern != NULL) {
		if (verb_count > 0)
			paint_pattern(run, state->stroke_pattern, verbs, verb_count, transformed, point_count, PDF_FILL_NONZERO, state->stroke_alpha);
		free(verbs);
		free(points);
		return;
	}

	/* Adds the outline as a nonzero fill in the stroke's colour and alpha. */
	memset(&style, 0, sizeof(style));
	style.rule = PDF_FILL_NONZERO;
	style.red = state->stroke[0];
	style.green = state->stroke[1];
	style.blue = state->stroke[2];
	style.alpha = state->stroke_alpha;
	style.blend = state->blend;
	error = 0;
	if (verb_count > 0)
		error = pdf_display_add_path(run->builder, PDF_ITEM_FILL, verbs, verb_count, transformed, point_count, &style);
	free(verbs);
	free(points);
	if (error != 0)
		stop_for(run, error);
}

/* Pushes the path as a clip, which the level's Q ends. */
static void
push_clip(
	struct content_run *run)
{
	struct pdf_display_item style;
	struct pdf_point *transformed;
	int error;

	/* Refuses clips nested past the limit. */
	if (run->clip_depth >= PDF_CONTENT_CLIPS_MAX) {
		run->flags |= PDF_DISPLAY_LIMITED;
		return;
	}

	/* Transforms the path to the page. */
	error = transform_path(run, run->path.points, run->path.point_count, &transformed);
	if (error != 0) {
		stop_for(run, error);
		return;
	}

	/* Adds the clip and counts it at the level. */
	memset(&style, 0, sizeof(style));
	style.rule = run->clip_rule;
	error = pdf_display_add_path(run->builder, PDF_ITEM_CLIP_PUSH, run->path.verbs, run->path.verb_count, transformed, run->path.point_count, &style);
	if (error != 0) {
		stop_for(run, error);
		return;
	}

	/* The level counts the clip, which the level's Q pops. */
	run->stack[run->depth].clips++;
	run->clip_depth++;
}

/* Transforms points by the CTM into the run's scratch array. */
static int
transform_path(
	struct content_run *run,
	const struct pdf_point *points,
	size_t count,
	struct pdf_point **transformed)
{
	const double *ctm;
	struct pdf_point *grown;
	size_t index;

	/* Grows the scratch array when the points do not fit. */
	if (count > run->scratch_capacity) {
		if (count > PDF_DISPLAY_POINTS_MAX)
			return ENOMEM;
		grown = realloc(run->scratch, count * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		run->scratch = grown;
		run->scratch_capacity = count;
	}

	/* Applies the CTM to each point. */
	ctm = run->stack[run->depth].ctm;
	for (index = 0; index < count; index++) {
		run->scratch[index].x = ctm[0] * points[index].x + ctm[2] * points[index].y + ctm[4];
		run->scratch[index].y = ctm[1] * points[index].x + ctm[3] * points[index].y + ctm[5];
	}

	/* Succeeded: the transformed copy. */
	*transformed = run->scratch;
	return 0;
}

/* Reports how much a matrix scales lengths on average: the square root of its determinant's size. */
static double
matrix_scale(
	const double matrix[6])
{
	double determinant;
	double scale;

	/* The area scale of the linear part. */
	determinant = matrix[0] * matrix[3] - matrix[1] * matrix[2];
	if (determinant < 0.0)
		determinant = -determinant;

	/* Its square root is the length scale. */
	scale = sqrt(determinant);

	/* Reports the length scale. */
	return scale;
}

/* Executes a text operator: the text state, the text object, positioning, and showing. */
static void
execute_text(
	struct content_run *run,
	enum content_operator code,
	struct pdf_object *resources)
{
	struct content_state *state;
	double numbers[6];
	int error;

	/* The level in force, whose text state the operators change, and the resources a Type 3 glyph may need. */
	state = &run->stack[run->depth];
	run->text_resources = resources;

	/* Changes the text state, or positions or shows text, by the operator. */
	switch (code) {
	case OP_TEXT_BEGIN:
		begin_text(run);
		break;
	case OP_TEXT_END:
		end_text(run);
		break;
	case OP_TEXT_FONT:
		set_font(run, resources);
		break;
	case OP_TEXT_CHARACTER_SPACING:
		error = read_numbers(run, 1, numbers);
		if (error == 0)
			state->character_spacing = numbers[0];
		break;
	case OP_TEXT_WORD_SPACING:
		error = read_numbers(run, 1, numbers);
		if (error == 0)
			state->word_spacing = numbers[0];
		break;
	case OP_TEXT_SCALE:
		/* A percentage. */
		error = read_numbers(run, 1, numbers);
		if (error == 0)
			state->horizontal_scale = numbers[0] / 100.0;
		break;
	case OP_TEXT_LEADING:
		error = read_numbers(run, 1, numbers);
		if (error == 0)
			state->leading = numbers[0];
		break;
	case OP_TEXT_RISE:
		error = read_numbers(run, 1, numbers);
		if (error == 0)
			state->rise = numbers[0];
		break;
	case OP_TEXT_RENDER:
		/* One of the eight modes. */
		error = read_numbers(run, 1, numbers);
		if (error != 0)
			break;
		if (numbers[0] >= 0.0 && numbers[0] <= 7.0)
			state->render_mode = (int)numbers[0];
		break;
	case OP_TEXT_MOVE:
		error = read_numbers(run, 2, numbers);
		if (error == 0)
			move_text_line(run, numbers[0], numbers[1]);
		break;
	case OP_TEXT_MOVE_LEADING:
		/* A move that also sets the leading to the move's negated height. */
		error = read_numbers(run, 2, numbers);
		if (error != 0)
			break;
		state->leading = -numbers[1];
		move_text_line(run, numbers[0], numbers[1]);
		break;
	case OP_TEXT_MATRIX:
		/* Both matrices become the operands. */
		error = read_numbers(run, 6, numbers);
		if (error != 0)
			break;
		memcpy(run->text_matrix, numbers, sizeof(run->text_matrix));
		memcpy(run->line_matrix, numbers, sizeof(run->line_matrix));
		break;
	case OP_TEXT_NEXT_LINE:
		move_text_line(run, 0.0, -state->leading);
		break;
	case OP_TEXT_SHOW:
		show_operand_string(run, run->operand_count - 1);
		break;
	case OP_TEXT_SHOW_ARRAY:
		show_array(run);
		break;
	case OP_TEXT_NEXT_SHOW:
		/* The next line, then the string. */
		move_text_line(run, 0.0, -state->leading);
		show_operand_string(run, run->operand_count - 1);
		break;
	case OP_TEXT_SPACED_SHOW:
		/* The word and character spacing, the next line, then the string. */
		if (run->operand_count < 3)
			break;
		if (run->operands[run->operand_count - 3].type != OPERAND_NUMBER)
			break;
		if (run->operands[run->operand_count - 2].type != OPERAND_NUMBER)
			break;
		state->word_spacing = run->operands[run->operand_count - 3].number;
		state->character_spacing = run->operands[run->operand_count - 2].number;
		move_text_line(run, 0.0, -state->leading);
		show_operand_string(run, run->operand_count - 1);
		break;
	default:
		break;
	}
}

/*
 * Sets the font and its size (Tf).  A font the resources do not have, or
 * one the reader cannot read, leaves no font: its strings only mark the
 * list.
 */
static void
set_font(
	struct content_run *run,
	struct pdf_object *resources)
{
	struct content_state *state;
	struct pdf_object *dictionary;
	struct pdf_font *font;
	double size;
	int error;

	/* A name and a size. */
	state = &run->stack[run->depth];
	if (run->operand_count < 2)
		return;
	error = read_numbers(run, 1, &size);
	if (error != 0)
		return;
	state->font_size = size;

	/* The font's name in the resources, as the editor writes it again (ws175-p005; a name too long is not kept). */
	state->font_resource_length = 0;
	if (run->operands[run->operand_count - 2].length < sizeof(state->font_resource) && run->operands[run->operand_count - 2].bytes != NULL) {
		state->font_resource_length = run->operands[run->operand_count - 2].length;
		memcpy(state->font_resource, run->operands[run->operand_count - 2].bytes, state->font_resource_length);
	}

	/* Finds the font dictionary the name gives. */
	state->font = NULL;
	error = find_named_resource(run, resources, "Font", &run->operands[run->operand_count - 2], &dictionary);
	if (error != 0) {
		run->flags |= PDF_DISPLAY_SKIPPED;
		return;
	}

	/* Reads the font, once per document. */
	error = pdf_font_get(run->document, dictionary, &font);
	if (error == ENOMEM) {
		stop_for(run, ENOMEM);
		return;
	}

	/* A font that cannot be read leaves its text out. */
	if (error != 0) {
		run->flags |= PDF_DISPLAY_SKIPPED;
		return;
	}

	/* The font in force. */
	state->font = font;
}

/* Starts the next line at an offset from the start of the current one (Td, TD, T*). */
static void
move_text_line(
	struct content_run *run,
	double x,
	double y)
{
	double move[6];

	/* The line matrix moves by the offset, and the text matrix starts there. */
	move[0] = 1.0;
	move[1] = 0.0;
	move[2] = 0.0;
	move[3] = 1.0;
	move[4] = x;
	move[5] = y;
	concat_matrix(run->line_matrix, move);
	memcpy(run->text_matrix, run->line_matrix, sizeof(run->text_matrix));
}

/* Begins a text object (BT): both matrices are the identity and no glyph clips yet. */
static void
begin_text(
	struct content_run *run)
{
	/* The identity. */
	memset(run->text_matrix, 0, sizeof(run->text_matrix));
	run->text_matrix[0] = 1.0;
	run->text_matrix[3] = 1.0;
	memcpy(run->line_matrix, run->text_matrix, sizeof(run->line_matrix));

	/* A clip left by an object without its ET is dropped. */
	run->text_clipping = 0;
	run->text_clip_verb_count = 0;
	run->text_clip_point_count = 0;
}

/*
 * Ends a text object (ET): the glyphs shown in a clipping mode become the
 * clip of the level in force (an object that clipped with no glyph clips
 * everything away).
 */
static void
end_text(
	struct content_run *run)
{
	struct pdf_display_item style;
	int error;

	/* Nothing clips unless a string was shown in a clipping mode. */
	if (!run->text_clipping)
		return;
	run->text_clipping = 0;

	/* Refuses clips nested past the limit. */
	if (run->clip_depth >= PDF_CONTENT_CLIPS_MAX) {
		run->flags |= PDF_DISPLAY_LIMITED;
		return;
	}

	/* Adds the glyphs as a nonzero clip, counted at the level. */
	memset(&style, 0, sizeof(style));
	style.rule = PDF_FILL_NONZERO;
	error = pdf_display_add_path(run->builder, PDF_ITEM_CLIP_PUSH, run->text_clip_verbs, run->text_clip_verb_count, run->text_clip_points, run->text_clip_point_count, &style);
	run->text_clip_verb_count = 0;
	run->text_clip_point_count = 0;
	if (error != 0) {
		stop_for(run, error);
		return;
	}

	/* The level's Q, and the page's end, pop the clip. */
	run->stack[run->depth].clips++;
	run->clip_depth++;
}

/* Shows the string operand at an index (Tj, ', "). */
static void
show_operand_string(
	struct content_run *run,
	size_t index)
{
	const struct content_operand *operand;

	/* The operand must be a string. */
	if (run->operand_count == 0 || index >= run->operand_count)
		return;
	operand = &run->operands[index];
	if (operand->type != OPERAND_OTHER)
		return;
	if (operand->object->type != PDF_OBJECT_STRING)
		return;

	/* Shows its bytes. */
	show_string(run, operand->object->bytes, operand->object->length);
}

/*
 * Shows an array of strings and adjustments (TJ): each number moves the
 * text position back by thousandths of the font size.
 */
static void
show_array(
	struct content_run *run)
{
	struct content_state *state;
	struct pdf_object *array;
	struct pdf_object *element;
	double number;
	size_t index;
	int error;

	/* The operand must be an array. */
	state = &run->stack[run->depth];
	if (run->operand_count == 0)
		return;
	if (run->operands[run->operand_count - 1].type != OPERAND_OTHER)
		return;
	array = run->operands[run->operand_count - 1].object;
	if (array->type != PDF_OBJECT_ARRAY)
		return;

	/* Shows each string and applies each adjustment, in order. */
	for (index = 0; index < array->count && !run->stopped; index++) {
		element = array->values[index];
		if (element->type == PDF_OBJECT_STRING) {
			show_string(run, element->bytes, element->length);
			continue;
		}

		/* A number moves the position back by thousandths of the size; anything else is ignored. */
		error = pdf_object_number(element, &number);
		if (error != 0)
			continue;
		if (!(number > -1e9 && number < 1e9))
			continue;
		advance_text(run, -number / 1000.0 * state->font_size);
	}
}

/*
 * Shows one string: each code's glyph is added to the path in user space
 * and the text position moves past it; the path is then painted by the
 * rendering mode.
 */
static void
show_string(
	struct content_run *run,
	const unsigned char *bytes,
	size_t length)
{
	struct content_state *state;
	struct pdf_glyph glyph;
	double before[6];
	unsigned code;
	size_t position;
	size_t used;
	size_t shown;
	double spacing;
	double bold;
	int single_byte;
	int draws;
	int adds;
	int vertical;
	int scanned;
	int form_text;
	int error;

	/* A string without a usable font is not drawn and does not move. */
	state = &run->stack[run->depth];
	if (state->font == NULL) {
		run->flags |= PDF_DISPLAY_SKIPPED;
		return;
	}

	/* The editor's scan notes where the page's own string starts and what it says (ws175-p002b). */
	scanned = scan_here(run);
	if (scanned)
		scan_string(run, bytes, length);

	/* The text a form shows is noted apart, for the page's text (ws177-p040). */
	form_text = scan_form_here(run);

	/* A substituted or unreadable font marks the list; a vertical one moves down. */
	run->flags |= pdf_font_status(state->font);
	vertical = pdf_font_vertical(state->font);

	/* The glyphs are drawn unless the mode is invisible (3) or clipping only (7). */
	draws = 1;
	if (state->render_mode == 3 || state->render_mode == 7)
		draws = 0;
	if (state->render_mode >= 4)
		run->text_clipping = 1;

	/* Starts a path of the string's glyphs; a path left unpainted is dropped. */
	path_clear(run);
	bold = 0.0;

	/* Walks the codes of the string. */
	position = 0;
	for (shown = 0; position < length && shown < PDF_CONTENT_STRING_MAX; shown++) {
		/* Reads the next code and what it draws. */
		used = pdf_font_next_code(state->font, bytes + position, length - position, &code, &single_byte);
		position += used;
		error = pdf_font_glyph(state->font, code, &glyph);
		if (error != 0) {
			stop_for(run, error);
			break;
		}

		/* A Type 3 glyph is its procedure, run where the text position is (it is not added to a clip). */
		if (draws)
			draw_type3_glyph(run, code);

		/* Adds the glyph's outline where the text position is, when it is painted or clips. */
		adds = 0;
		if (glyph.drawable && glyph.verb_count > 0) {
			if (draws || state->render_mode >= 4)
				adds = 1;
		}

		/* Adds the glyph to the list when it is drawn or clips. */
		if (adds) {
			error = add_glyph(run, &glyph);
			if (error != 0) {
				stop_for(run, error);
				break;
			}

			/* A substitute for a bold face is thickened when the string is painted. */
			bold = glyph.bold;
		}

		/* Moves past it: its width, the character spacing, and the word spacing after a one-byte space. */
		memcpy(before, run->text_matrix, sizeof(before));
		spacing = state->character_spacing;
		if (single_byte && code == 32)
			spacing += state->word_spacing;
		if (vertical) {
			advance_text(run, glyph.vertical_advance * state->font_size + spacing);
		} else {
			advance_text(run, glyph.width * state->font_size + spacing);
		}

		/* The scan notes the code's characters, where its glyph was (ws128-p004), the page's own or a form's (ws177-p040). */
		if (scanned)
			scan_code(run, code, single_byte, before);
		if (form_text)
			scan_form_code(run, code, single_byte, before);
	}

	/* Paints and clips with the glyphs. */
	if (run->path.verb_count > 0 && !run->stopped)
		paint_text(run, bold);
	path_clear(run);
}

/*
 * Adds a glyph's outline to the path, from ems at the text position into
 * user space: its own transform, the font size, the horizontal scale, the
 * rise, and the text matrix.
 */
static int
add_glyph(
	struct content_run *run,
	const struct pdf_glyph *glyph)
{
	struct content_state *state;
	const double *text;
	double coordinates[6];
	double glyph_x;
	double glyph_y;
	double text_x;
	double text_y;
	double scale_x;
	double scale_y;
	size_t verb;
	size_t point;
	size_t count;
	size_t index;
	int vertical;
	int error;

	/* The scales from ems to text space, and whether the glyph hangs from its vertical origin. */
	state = &run->stack[run->depth];
	text = run->text_matrix;
	scale_x = state->font_size * state->horizontal_scale;
	scale_y = state->font_size;
	vertical = pdf_font_vertical(state->font);

	/* Adds each step with its points moved into user space. */
	point = 0;
	for (verb = 0; verb < glyph->verb_count; verb++) {
		/* How many points the step takes. */
		count = 0;
		if (glyph->verbs[verb] == PDF_PATH_MOVE || glyph->verbs[verb] == PDF_PATH_LINE)
			count = 1;
		if (glyph->verbs[verb] == PDF_PATH_CUBIC)
			count = 3;
		if (point + count > glyph->point_count)
			return PDF_EFORMAT;

		/* Moves each point: glyph transform, vertical origin, scale and rise, text matrix. */
		for (index = 0; index < count; index++) {
			glyph_x = glyph->transform[0] * glyph->points[point + index].x + glyph->transform[2] * glyph->points[point + index].y;
			glyph_y = glyph->transform[1] * glyph->points[point + index].x + glyph->transform[3] * glyph->points[point + index].y;
			if (vertical) {
				glyph_x -= glyph->origin_x;
				glyph_y -= glyph->origin_y;
			}

			/* Scaled and risen into text space, then into user space. */
			text_x = glyph_x * scale_x;
			text_y = glyph_y * scale_y + state->rise;
			coordinates[index * 2] = text[0] * text_x + text[2] * text_y + text[4];
			coordinates[index * 2 + 1] = text[1] * text_x + text[3] * text_y + text[5];
		}

		/* The next step's points follow. */
		point += count;

		/* Adds the step. */
		error = path_add(run, (enum pdf_path_verb)glyph->verbs[verb], coordinates, count);
		if (error != 0)
			return error;
	}

	/* Succeeded: the glyph is part of the path. */
	return 0;
}

/*
 * Moves the text position along the writing direction by an amount in
 * text space: horizontally scaled by Tz for horizontal writing,
 * downward (negative) for vertical writing.
 */
static void
advance_text(
	struct content_run *run,
	double amount)
{
	struct content_state *state;
	double move[6];
	int vertical;

	/* A translation in text space. */
	state = &run->stack[run->depth];
	move[0] = 1.0;
	move[1] = 0.0;
	move[2] = 0.0;
	move[3] = 1.0;
	move[4] = amount * state->horizontal_scale;
	move[5] = 0.0;

	/* Vertical writing moves down instead, without the horizontal scale. */
	vertical = 0;
	if (state->font != NULL)
		vertical = pdf_font_vertical(state->font);
	if (vertical) {
		move[4] = 0.0;
		move[5] = amount;
	}

	/* The text matrix moves; a nonsensical amount leaves it. */
	if (!(amount > -1e9 && amount < 1e9))
		return;
	concat_matrix(run->text_matrix, move);
}

/*
 * Paints the string's glyphs by the rendering mode: 0 fills, 1 strokes, 2
 * does both, 3 does neither, and 4 to 7 do the same and add the glyphs to
 * the clip ET applies.  bold thickens a substitute's fill.
 */
static void
paint_text(
	struct content_run *run,
	double bold)
{
	struct content_state *state;
	int mode;
	int error;

	/* The mode's painting, without its clipping. */
	state = &run->stack[run->depth];
	mode = state->render_mode % 4;

	/* Fills, thickening a substitute that stands in for a bold face. */
	if (mode == 0 || mode == 2) {
		fill_path(run, PDF_FILL_NONZERO);
		if (bold > 0.0 && !run->stopped)
			embolden_text(run, bold);
	}

	/* Strokes. */
	if (mode == 1 || mode == 2) {
		if (!run->stopped)
			stroke_path(run);
	}

	/* Keeps the glyphs for the clip. */
	if (state->render_mode >= 4 && !run->stopped) {
		error = add_text_clip(run);
		if (error != 0)
			stop_for(run, error);
	}
}

/*
 * Thickens filled glyphs by stroking them in the fill's colour with a
 * width of bold ems of the font size.
 */
static void
embolden_text(
	struct content_run *run,
	double width)
{
	struct content_state *state;
	struct content_state saved;
	double scale;

	/* The stroke's width in user space: the ems in text space, through the text matrix. */
	state = &run->stack[run->depth];
	scale = matrix_scale(run->text_matrix);
	if (state->font_size < 0.0) {
		width *= -state->font_size;
	} else {
		width *= state->font_size;
	}

	/* Strokes with the fill's colour and a round, solid line, then puts the style back. */
	saved = *state;
	memcpy(state->stroke, state->fill, sizeof(state->stroke));
	state->stroke_usable = state->fill_usable;
	state->stroke_alpha = state->fill_alpha;
	state->line_width = width * scale;
	state->line_join = 1;
	state->line_cap = 1;
	state->dash_count = 0;
	stroke_path(run);
	*state = saved;
}

/* Adds the path, moved to the page, to the glyphs the text object's ET makes the clip. */
static int
add_text_clip(
	struct content_run *run)
{
	struct pdf_point *transformed;
	unsigned char *verbs;
	struct pdf_point *points;
	size_t capacity;
	int error;

	/* Refuses glyphs past the list's point limit. */
	if (run->text_clip_point_count + run->path.point_count > PDF_DISPLAY_POINTS_MAX)
		return ENOMEM;

	/* Moves the path to the page. */
	error = transform_path(run, run->path.points, run->path.point_count, &transformed);
	if (error != 0)
		return error;

	/* Grows the steps to hold the path's. */
	if (run->text_clip_verb_count + run->path.verb_count > run->text_clip_verb_capacity) {
		capacity = run->text_clip_verb_capacity * 2 + run->path.verb_count + 64;
		verbs = realloc(run->text_clip_verbs, capacity);
		if (verbs == NULL)
			return ENOMEM;
		run->text_clip_verbs = verbs;
		run->text_clip_verb_capacity = capacity;
	}

	/* Grows the points to hold the path's. */
	if (run->text_clip_point_count + run->path.point_count > run->text_clip_point_capacity) {
		capacity = run->text_clip_point_capacity * 2 + run->path.point_count + 64;
		points = realloc(run->text_clip_points, capacity * sizeof(*points));
		if (points == NULL)
			return ENOMEM;
		run->text_clip_points = points;
		run->text_clip_point_capacity = capacity;
	}

	/* Appends the path. */
	memcpy(run->text_clip_verbs + run->text_clip_verb_count, run->path.verbs, run->path.verb_count);
	memcpy(run->text_clip_points + run->text_clip_point_count, transformed, run->path.point_count * sizeof(*transformed));
	run->text_clip_verb_count += run->path.verb_count;
	run->text_clip_point_count += run->path.point_count;

	/* Succeeded: the glyphs wait for ET. */
	return 0;
}

/* Tells whether the colour space the operand names is the Pattern space (by name, or a resource built on it). */
static int
is_pattern_space(
	struct content_run *run,
	struct pdf_object *resources)
{
	const struct content_operand *operand;
	struct pdf_object *space;
	struct pdf_object *family;
	int is_name;
	int differs;
	int error;

	/* The name is the last operand. */
	if (run->operand_count == 0)
		return 0;
	operand = &run->operands[run->operand_count - 1];
	if (operand->type != OPERAND_NAME)
		return 0;

	/* The space's own name. */
	if (operand->length == 7) {
		differs = memcmp(operand->bytes, "Pattern", 7);
		if (differs == 0)
			return 1;
	}

	/* A resource: the name /Pattern, or an array whose family is /Pattern. */
	error = find_resource(run, resources, "ColorSpace", &space);
	if (error != 0)
		return 0;
	family = space;
	if (space->type == PDF_OBJECT_ARRAY && space->count > 0) {
		error = pdf_reader_resolve(run->document, space->values[0], &family);
		if (error != 0)
			return 0;
	}

	/* The family is the Pattern space's. */
	is_name = pdf_object_is_name(family, "Pattern");
	if (is_name)
		return 1;

	/* Another space. */
	return 0;
}

/*
 * Sets the fill or stroke colour to the pattern the last operand names: a
 * shading pattern is drawn; a tiling pattern, and a name the resources do
 * not have, leave the colour unusable.
 */
static void
set_pattern(
	struct content_run *run,
	struct pdf_object *resources,
	int stroke)
{
	struct content_state *state;
	struct pdf_object *pattern;
	struct pdf_object *type;
	struct pdf_object *chosen;
	int error;

	/* No pattern until one is found. */
	state = &run->stack[run->depth];
	chosen = NULL;

	/* Finds the pattern; only a shading pattern (type 2) is drawn yet. */
	error = find_resource(run, resources, "Pattern", &pattern);
	if (error != 0)
		pattern = NULL;
	if (pattern != NULL) {
		if (pattern->type != PDF_OBJECT_DICTIONARY && pattern->type != PDF_OBJECT_STREAM)
			pattern = NULL;
	}

	/* Its type decides whether it is drawn. */
	if (pattern != NULL) {
		error = pdf_reader_resolve_key(run->document, pattern, "PatternType", &type);
		if (error == 0 && type->type == PDF_OBJECT_INTEGER) {
			if (type->integer == 2)
				chosen = pattern;
		}
	}

	/* The colour is the pattern, usable only when it is drawn. */
	if (stroke) {
		state->stroke_pattern = chosen;
		state->stroke_usable = 0;
		if (chosen != NULL)
			state->stroke_usable = 1;
	} else {
		state->fill_pattern = chosen;
		state->fill_usable = 0;
		if (chosen != NULL)
			state->fill_usable = 1;
	}
}

/*
 * Paints the shading the operand names (sh) over the clip in force: the
 * whole page, or the shading's /BBox on it.
 */
static void
paint_shading(
	struct content_run *run,
	struct pdf_object *resources)
{
	struct content_state *state;
	struct pdf_object *shading;
	struct pdf_object *box_object;
	struct pdf_object *number_object;
	double box[4];
	double bounds[4];
	double x;
	double y;
	double page_x;
	double page_y;
	size_t corner;
	size_t index;
	int has_box;
	int error;

	/* Finds the shading; a missing one is left out. */
	state = &run->stack[run->depth];
	error = find_resource(run, resources, "Shading", &shading);
	if (error != 0) {
		run->flags |= PDF_DISPLAY_SKIPPED;
		return;
	}

	/* The whole page. */
	bounds[0] = 0.0;
	bounds[1] = 0.0;
	bounds[2] = run->builder->list.width;
	bounds[3] = run->builder->list.height;

	/* Within the shading's box, when it has one: its corners' box on the page. */
	error = pdf_reader_resolve_key(run->document, shading, "BBox", &box_object);
	has_box = 0;
	if (error == 0 && box_object->type == PDF_OBJECT_ARRAY) {
		if (box_object->count == 4)
			has_box = 1;
	}

	/* Reads the box. */
	if (has_box) {
		/* The box's four numbers; a box that is not numbers leaves the whole page. */
		for (index = 0; index < 4; index++) {
			error = pdf_reader_resolve(run->document, box_object->values[index], &number_object);
			if (error == 0)
				error = pdf_object_number(number_object, &box[index]);
			if (error != 0)
				break;
		}

		/* The box of its four corners on the page. */
		for (corner = 0; corner < 4 && error == 0; corner++) {
			x = box[(corner & 1) * 2];
			y = box[1 + (corner >> 1) * 2];
			if (corner == 0) {
				bounds[0] = run->builder->list.width;
				bounds[1] = run->builder->list.height;
				bounds[2] = 0.0;
				bounds[3] = 0.0;
			}

			/* The corner on the page widens the box. */
			page_x = state->ctm[0] * x + state->ctm[2] * y + state->ctm[4];
			page_y = state->ctm[1] * x + state->ctm[3] * y + state->ctm[5];
			if (page_x > bounds[2])
				bounds[2] = page_x;
			if (page_x < bounds[0])
				bounds[0] = page_x;
			if (page_y > bounds[3])
				bounds[3] = page_y;
			if (page_y < bounds[1])
				bounds[1] = page_y;
		}

		/* Within the page. */
		if (bounds[0] < 0.0)
			bounds[0] = 0.0;
		if (bounds[1] < 0.0)
			bounds[1] = 0.0;
		if (bounds[2] > run->builder->list.width)
			bounds[2] = run->builder->list.width;
		if (bounds[3] > run->builder->list.height)
			bounds[3] = run->builder->list.height;
	}

	/* Paints the shading in user space. */
	error = add_shading_image(run, shading, state->ctm, bounds, state->fill_alpha);
	if (error != 0)
		stop_for(run, error);
}

/*
 * Fills a path of the page with a shading pattern: the path as a clip,
 * the pattern's shading over the path's box, and the clip's end.
 */
static void
paint_pattern(
	struct content_run *run,
	struct pdf_object *pattern,
	const unsigned char *verbs,
	size_t verb_count,
	const struct pdf_point *points,
	size_t point_count,
	enum pdf_fill_rule rule,
	double alpha)
{
	struct pdf_display_item style;
	struct pdf_object *matrix_object;
	struct pdf_object *shading;
	double matrix[6];
	double combined[6];
	double bounds[4];
	size_t index;
	int has_matrix;
	int error;

	/* Refuses clips nested past the limit. */
	if (run->clip_depth >= PDF_CONTENT_CLIPS_MAX) {
		run->flags |= PDF_DISPLAY_LIMITED;
		return;
	}

	/* The path's box, within the page. */
	if (point_count == 0)
		return;
	bounds[0] = points[0].x;
	bounds[1] = points[0].y;
	bounds[2] = points[0].x;
	bounds[3] = points[0].y;
	for (index = 1; index < point_count; index++) {
		if (points[index].x < bounds[0])
			bounds[0] = points[index].x;
		if (points[index].y < bounds[1])
			bounds[1] = points[index].y;
		if (points[index].x > bounds[2])
			bounds[2] = points[index].x;
		if (points[index].y > bounds[3])
			bounds[3] = points[index].y;
	}

	/* Within the page; a path off the page paints nothing. */
	if (bounds[0] < 0.0)
		bounds[0] = 0.0;
	if (bounds[1] < 0.0)
		bounds[1] = 0.0;
	if (bounds[2] > run->builder->list.width)
		bounds[2] = run->builder->list.width;
	if (bounds[3] > run->builder->list.height)
		bounds[3] = run->builder->list.height;
	if (!(bounds[2] > bounds[0]))
		return;
	if (!(bounds[3] > bounds[1]))
		return;

	/* The pattern's matrix, relative to the content stream's own. */
	matrix[0] = 1.0;
	matrix[1] = 0.0;
	matrix[2] = 0.0;
	matrix[3] = 1.0;
	matrix[4] = 0.0;
	matrix[5] = 0.0;
	error = pdf_reader_resolve_key(run->document, pattern, "Matrix", &matrix_object);
	has_matrix = 0;
	if (error == 0 && matrix_object->type == PDF_OBJECT_ARRAY) {
		if (matrix_object->count == 6)
			has_matrix = 1;
	}

	/* Reads the matrix; one that is not numbers leaves the pattern out. */
	if (has_matrix) {
		for (index = 0; index < 6; index++) {
			error = pdf_object_number(matrix_object->values[index], &matrix[index]);
			if (error != 0)
				return;
		}
	}

	/* The pattern space: the pattern's matrix after the one in force where the content began. */
	memcpy(combined, run->pattern_base, sizeof(combined));
	concat_matrix(combined, matrix);

	/* The pattern's shading. */
	error = pdf_reader_resolve_key(run->document, pattern, "Shading", &shading);
	if (error != 0) {
		run->flags |= PDF_DISPLAY_DAMAGED;
		return;
	}

	/* Clips to the path. */
	memset(&style, 0, sizeof(style));
	style.rule = rule;
	error = pdf_display_add_path(run->builder, PDF_ITEM_CLIP_PUSH, verbs, verb_count, points, point_count, &style);
	if (error != 0) {
		stop_for(run, error);
		return;
	}

	/* Paints the shading, then ends the clip. */
	error = add_shading_image(run, shading, combined, bounds, alpha);
	if (error != 0) {
		stop_for(run, error);
		return;
	}

	/* The clip ends with the shading. */
	error = pdf_display_add_clip_pop(run->builder);
	if (error != 0)
		stop_for(run, error);
}

/*
 * Adds a shading drawn over a region of the page as an image item; a
 * shading the reader cannot draw is left out (only running out of memory
 * is an error).
 */
static int
add_shading_image(
	struct content_run *run,
	struct pdf_object *shading,
	const double matrix[6],
	const double bounds[4],
	double alpha)
{
	struct pdf_display_item style;
	unsigned char *pixels;
	size_t width;
	size_t height;
	int error;

	/* Nothing to paint in a region of no size. */
	if (!(bounds[2] > bounds[0] && bounds[3] > bounds[1]))
		return 0;

	/* Draws the shading. */
	memset(&style, 0, sizeof(style));
	error = pdf_shading_image(run->document, shading, matrix, bounds, &pixels, &width, &height, style.matrix);
	if (error == ENOMEM)
		return ENOMEM;
	if (error != 0) {
		run->flags |= PDF_DISPLAY_SKIPPED;
		return 0;
	}

	/* Adds it as an image, smoothed, in the fill's alpha and blend mode. */
	style.image_width = width;
	style.image_height = height;
	style.alpha = alpha;
	style.blend = run->stack[run->depth].blend;
	style.interpolate = 1;
	error = pdf_display_add_image(run->builder, pixels, &style);
	if (error != 0)
		return error;

	/* Succeeded: the shading is on the list. */
	return 0;
}

/* Draws the XObject the operand names: an image, or a form's content. */
static void
draw_xobject(
	struct content_run *run,
	struct pdf_object *resources)
{
	struct pdf_object *xobject;
	struct pdf_object *subtype;
	int is_image;
	int is_form;
	int scanned;
	int error;

	/* Finds the XObject; a missing one is left out. */
	error = find_resource(run, resources, "XObject", &xobject);
	if (error != 0) {
		run->flags |= PDF_DISPLAY_SKIPPED;
		return;
	}

	/* An XObject that is not a stream is left out too. */
	if (xobject->type != PDF_OBJECT_STREAM) {
		run->flags |= PDF_DISPLAY_SKIPPED;
		return;
	}

	/* Draws it by its subtype. */
	error = pdf_reader_resolve_key(run->document, xobject, "Subtype", &subtype);
	if (error != 0) {
		run->flags |= PDF_DISPLAY_DAMAGED;
		return;
	}

	/* The two kinds the reader draws. */
	is_image = pdf_object_is_name(subtype, "Image");
	is_form = pdf_object_is_name(subtype, "Form");
	if (is_image)
		draw_image(run, xobject);

	/* A form at the page's top level is one graphic for the editor's scan. */
	scanned = scan_here(run);
	if (is_form && scanned)
		scan_form(run, xobject);
	if (is_form)
		run_form(run, xobject, resources);
}

/* Draws an image XObject on the unit square of the user space. */
static void
draw_image(
	struct content_run *run,
	struct pdf_object *image)
{
	struct content_state *state;
	struct pdf_display_item style;
	unsigned char *pixels;
	size_t width;
	size_t height;
	int interpolate;
	int scanned;
	int error;

	/* An image at the page's top level is one object for the editor's scan, drawn or not. */
	scanned = scan_here(run);
	if (scanned)
		scan_image(run, image);

	/* Decodes the image; one the reader cannot decode is left out. */
	state = &run->stack[run->depth];
	error = pdf_image_decode(run->document, image, state->fill, &pixels, &width, &height, &interpolate, &run->flags);
	if (error == ENOMEM) {
		stop_for(run, ENOMEM);
		return;
	}

	/* An image the reader cannot decode is left out. */
	if (error != 0) {
		run->flags |= PDF_DISPLAY_SKIPPED;
		return;
	}

	/*
	 * Places it: the image's first row is at the top of the unit square,
	 * where y is 1 in user space, so (u, v) maps to the user point (u, 1 - v).
	 */
	memset(&style, 0, sizeof(style));
	style.image_width = width;
	style.image_height = height;
	style.matrix[0] = state->ctm[0];
	style.matrix[1] = state->ctm[1];
	style.matrix[2] = -state->ctm[2];
	style.matrix[3] = -state->ctm[3];
	style.matrix[4] = state->ctm[2] + state->ctm[4];
	style.matrix[5] = state->ctm[3] + state->ctm[5];
	style.alpha = state->fill_alpha;
	style.blend = state->blend;
	style.interpolate = interpolate;
	error = pdf_display_add_image(run->builder, pixels, &style);
	if (error != 0)
		stop_for(run, error);
}

/*
 * Runs a form XObject's content in a level of its own: its matrix
 * concatenated, its bounding box as a clip, its own resources (or the
 * caller's when it has none).
 */
static void
run_form(
	struct content_run *run,
	struct pdf_object *form,
	struct pdf_object *resources)
{
	struct pdf_object *matrix_object;
	struct pdf_object *box_object;
	struct pdf_object *form_resources;
	const unsigned char *data;
	unsigned char *owned;
	double matrix[6];
	double box[4];
	size_t data_size;
	double saved_pattern_base[6];
	size_t saved_base;
	size_t saved_ignored;
	size_t form_depth;
	size_t index;
	int dct;
	int error;

	/* Refuses forms nested past the limit (a form that draws itself ends here), or without a level left for them. */
	if (run->form_depth >= PDF_CONTENT_FORMS_MAX) {
		run->flags |= PDF_DISPLAY_LIMITED;
		return;
	}

	/* A form needs a level of the state stack of its own. */
	if (run->depth + 1 >= PDF_CONTENT_STACK_MAX) {
		run->flags |= PDF_DISPLAY_LIMITED;
		return;
	}

	/* Reads the form's matrix, the identity by default. */
	matrix[0] = 1.0;
	matrix[1] = 0.0;
	matrix[2] = 0.0;
	matrix[3] = 1.0;
	matrix[4] = 0.0;
	matrix[5] = 0.0;
	error = pdf_reader_resolve_key(run->document, form, "Matrix", &matrix_object);
	if (error != 0)
		return;
	if (matrix_object->type == PDF_OBJECT_ARRAY && matrix_object->count == 6) {
		for (index = 0; index < 6; index++) {
			error = pdf_object_number(matrix_object->values[index], &matrix[index]);
			if (error != 0)
				return;
		}
	}

	/* Reads its bounding box, which it needs. */
	error = pdf_reader_resolve_key(run->document, form, "BBox", &box_object);
	if (error != 0) {
		run->flags |= PDF_DISPLAY_DAMAGED;
		return;
	}

	/* | box_object->count != 4) {|The box is an array of four numbers. */
	if (box_object->type != PDF_OBJECT_ARRAY || box_object->count != 4) {
		run->flags |= PDF_DISPLAY_DAMAGED;
		return;
	}

	/* Reads the four numbers; any other entry is damage. */
	for (index = 0; index < 4; index++) {
		error = pdf_object_number(box_object->values[index], &box[index]);
		if (error != 0) {
			run->flags |= PDF_DISPLAY_DAMAGED;
			return;
		}
	}

	/* Its resources, or the caller's. */
	error = pdf_reader_resolve_key(run->document, form, "Resources", &form_resources);
	if (error != 0)
		form_resources = resources;
	else if (form_resources->type != PDF_OBJECT_DICTIONARY)
		form_resources = resources;

	/* Decodes its content; one that cannot be decoded is left out. */
	error = pdf_filter_decode(run->document, form, 0, &data, &data_size, &owned, &dct);
	if (error == ENOMEM) {
		stop_for(run, ENOMEM);
		return;
	} else if (error != 0) {
		run->flags |= PDF_DISPLAY_SKIPPED;
		return;
	}

	/* Opens the form's level, which starts from the caller's state; the form's own q past the limit are its own. */
	save_state(run);
	form_depth = run->depth;
	saved_base = run->base_depth;
	saved_ignored = run->ignored_saves;
	run->base_depth = form_depth;
	run->ignored_saves = 0;
	run->form_depth++;

	/* A form's level, whose text is noted, and a new run of its text (ws177-p040). */
	run->text_forms++;
	run->form_serial++;

	/* Applies the form's matrix, which its patterns are relative to, and clips to its bounding box. */
	concat_matrix(run->stack[run->depth].ctm, matrix);
	memcpy(saved_pattern_base, run->pattern_base, sizeof(saved_pattern_base));
	memcpy(run->pattern_base, run->stack[run->depth].ctm, sizeof(run->pattern_base));
	path_clear(run);
	error = add_rectangle(run, box[0], box[1], box[2], box[3]);
	if (error == 0) {
		run->clip_pending = 1;
		run->clip_rule = PDF_FILL_NONZERO;
		paint_path(run, 0, PDF_FILL_NONZERO, 0);
	}

	/* Runs the content, then closes every level it opened and the form's own. */
	run_content(run, data, data_size, form_resources);
	free(owned);
	unwind_to(run, form_depth - 1);
	memcpy(run->pattern_base, saved_pattern_base, sizeof(run->pattern_base));
	run->base_depth = saved_base;
	run->ignored_saves = saved_ignored;
	run->form_depth--;

	/* The form's level ends; what is shown after it starts a new run of text. */
	run->text_forms--;
	run->form_serial++;

	/* The caller's operands were the form's name; the form's own are gone with its content. */
	run->operand_count = 0;
}

/*
 * Draws a Type 3 font's glyph: its procedure runs in a level of its own
 * with the glyph space (the font matrix, the font size and horizontal
 * scale and rise, the text matrix) before the CTM, and the fill colour in
 * force.  The text object's matrices and the operands are kept across it.
 */
static void
draw_type3_glyph(
	struct content_run *run,
	unsigned code)
{
	struct content_state *state;
	struct pdf_object *procedure;
	struct pdf_object *resources;
	const unsigned char *data;
	unsigned char *owned;
	double font_matrix[6];
	double size_matrix[6];
	double saved_text[6];
	double saved_line[6];
	double saved_pattern_base[6];
	size_t data_size;
	size_t saved_base;
	size_t saved_ignored;
	size_t saved_operands;
	size_t glyph_depth;
	int saved_clipping;
	int dct;
	int error;

	/* Only a Type 3 font's code with a procedure has one. */
	state = &run->stack[run->depth];
	error = pdf_font_type3_glyph(run->document, state->font, code, &procedure, font_matrix, &resources);
	if (error != 0)
		return;
	if (resources == NULL)
		resources = run->text_resources;

	/* Refuses glyphs nested past the forms' limit (a glyph that shows itself ends here). */
	if (run->form_depth >= PDF_CONTENT_FORMS_MAX) {
		run->flags |= PDF_DISPLAY_LIMITED;
		return;
	}

	/* And a glyph without a level of the stack left for it. */
	if (run->depth + 1 >= PDF_CONTENT_STACK_MAX) {
		run->flags |= PDF_DISPLAY_LIMITED;
		return;
	}

	/* Decodes the procedure; one that cannot be decoded is left out. */
	error = pdf_filter_decode(run->document, procedure, 0, &data, &data_size, &owned, &dct);
	if (error == ENOMEM) {
		stop_for(run, ENOMEM);
		return;
	} else if (error != 0) {
		run->flags |= PDF_DISPLAY_SKIPPED;
		return;
	}

	/* Keeps what the glyph's content may change of the string being shown. */
	memcpy(saved_text, run->text_matrix, sizeof(saved_text));
	memcpy(saved_line, run->line_matrix, sizeof(saved_line));
	memcpy(saved_pattern_base, run->pattern_base, sizeof(saved_pattern_base));
	saved_operands = run->operand_count;
	saved_clipping = run->text_clipping;

	/* Opens the glyph's level, from the state in force. */
	save_state(run);
	glyph_depth = run->depth;
	saved_base = run->base_depth;
	saved_ignored = run->ignored_saves;
	run->base_depth = glyph_depth;
	run->ignored_saves = 0;
	run->form_depth++;

	/* The glyph space: the font matrix, then the size, horizontal scale and rise, then the text matrix. */
	size_matrix[0] = state->font_size * state->horizontal_scale;
	size_matrix[1] = 0.0;
	size_matrix[2] = 0.0;
	size_matrix[3] = state->font_size;
	size_matrix[4] = 0.0;
	size_matrix[5] = state->rise;
	concat_matrix(run->stack[run->depth].ctm, run->text_matrix);
	concat_matrix(run->stack[run->depth].ctm, size_matrix);
	concat_matrix(run->stack[run->depth].ctm, font_matrix);
	memcpy(run->pattern_base, run->stack[run->depth].ctm, sizeof(run->pattern_base));

	/* Runs the procedure, then closes every level it opened and its own. */
	run_content(run, data, data_size, resources);
	free(owned);
	unwind_to(run, glyph_depth - 1);
	run->base_depth = saved_base;
	run->ignored_saves = saved_ignored;
	run->form_depth--;

	/* The string goes on as it was. */
	memcpy(run->text_matrix, saved_text, sizeof(saved_text));
	memcpy(run->line_matrix, saved_line, sizeof(saved_line));
	memcpy(run->pattern_base, saved_pattern_base, sizeof(saved_pattern_base));
	run->operand_count = saved_operands;
	run->text_clipping = saved_clipping;
}

/*
 * Draws an inline image (BI, its dictionary, ID, its data, EI) as an image
 * XObject with the same dictionary would be drawn.
 *
 * The dictionary is read into a stream object of the page's arena whose
 * data is the bytes between ID and EI in the content itself (the object's
 * bytes point there).  An image the decoder cannot read is left out and
 * marks the list SKIPPED; the content goes on after the EI either way.
 */
static void
draw_inline_image(
	struct content_run *run,
	struct pdf_lexer *lexer,
	struct pdf_object *resources)
{
	struct pdf_object *image;
	size_t start;
	size_t known;
	size_t end;
	int error;

	/* Reads the dictionary up to ID; a damaged dictionary still leaves image NULL and the lexer at ID. */
	error = read_inline_dictionary(run, lexer, resources, &image);
	if (error != 0) {
		stop_for(run, error);
		return;
	}

	/* The data starts after the one white-space byte that follows ID. */
	start = lexer->position + 1;
	if (start > lexer->size)
		start = lexer->size;

	/*
	 * Unfiltered samples have a size the dictionary tells, and the search
	 * for EI starts after them, since binary samples may hold " EI ".
	 */
	known = 0;
	if (image != NULL)
		known = inline_sample_bytes(image);
	if (known > lexer->size - start)
		known = 0;

	/* Finds the EI that ends the data; an image without it damages the rest of the content. */
	error = find_inline_end(lexer, start + known, &end);
	if (error != 0) {
		lexer->position = lexer->size;
		run->flags |= PDF_DISPLAY_DAMAGED;
		return;
	}

	/* The content goes on after the EI. */
	lexer->position = end + 2;

	/* An image whose dictionary could not be read is left out. */
	if (image == NULL) {
		run->flags |= PDF_DISPLAY_SKIPPED;
		return;
	}

	/* The white space before EI is not data; the known size, when there is one, is exact. */
	if (known != 0) {
		end = start + known;
	} else if (end > start) {
		end--;
	}

	/* Draws the samples between. */
	image->bytes = lexer->data + start;
	image->data_offset = 0;
	image->data_length = end - start;
	draw_image(run, image);
}

/*
 * Reads an inline image's dictionary, from BI's operands up to the ID
 * keyword, into a new stream object with the full key names.
 *
 * Only a failure of the lexer (the content ends before ID) or of the arena
 * is an error; a key or value that cannot be read makes *image NULL, and
 * the reading goes on to ID so that the data can be skipped.
 */
static int
read_inline_dictionary(
	struct content_run *run,
	struct pdf_lexer *lexer,
	struct pdf_object *resources,
	struct pdf_object **image)
{
	struct pdf_token key;
	struct pdf_object *value;
	struct pdf_object *created;
	size_t saved;
	int is_id;
	int error;

	/* Allocates the stream object and room for its entries. */
	*image = NULL;
	created = pdf_arena_allocate(&run->arena, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->type = PDF_OBJECT_STREAM;
	created->keys = pdf_arena_allocate(&run->arena, sizeof(*created->keys) * PDF_CONTENT_INLINE_KEYS_MAX);
	if (created->keys == NULL)
		return ENOMEM;
	created->values = pdf_arena_allocate(&run->arena, sizeof(*created->values) * PDF_CONTENT_INLINE_KEYS_MAX);
	if (created->values == NULL)
		return ENOMEM;

	/* Reads key and value pairs until ID. */
	*image = created;
	for (;;) {
		/* The next key, or ID. */
		error = pdf_lexer_next(lexer, &key);
		if (error == ENOMEM)
			return ENOMEM;
		if (error != 0)
			return PDF_EFORMAT;
		if (key.type == PDF_TOKEN_END)
			return PDF_EFORMAT;
		is_id = pdf_token_is_keyword(&key, "ID");
		if (is_id)
			break;

		/* Anything but a name where a key belongs leaves the image out. */
		if (key.type != PDF_TOKEN_NAME) {
			*image = NULL;
			continue;
		}

		/* The value, which must not be a keyword (a missing value before ID leaves the image out). */
		saved = lexer->position;
		error = pdf_parse_object(lexer, 1, &value);
		if (error == ENOMEM)
			return ENOMEM;
		if (error != 0) {
			lexer->position = saved;
			*image = NULL;
			continue;
		}

		/* Keeps the entry under its full name. */
		if (*image == NULL)
			continue;
		error = add_inline_entry(run, resources, created, &key, value);
		if (error == ENOMEM)
			return ENOMEM;
		if (error != 0)
			*image = NULL;
	}

	/* Succeeded: *image is the dictionary, or NULL when it could not be read. */
	return 0;
}

/*
 * Adds one entry to an inline image's dictionary: the key under its full
 * name, and a colour space named in the resources replaced by the space
 * itself.
 */
static int
add_inline_entry(
	struct content_run *run,
	struct pdf_object *resources,
	struct pdf_object *image,
	const struct pdf_token *key,
	struct pdf_object *value)
{
	struct content_operand operand;
	struct pdf_object *name;
	struct pdf_object *space;
	unsigned char *copy;
	const char *full;
	int is_colorspace;
	int is_device;
	int error;

	/* Refuses more entries than an inline image has keys. */
	if (image->count >= PDF_CONTENT_INLINE_KEYS_MAX)
		return PDF_EFORMAT;

	/* Allocates the key's name object. */
	name = pdf_arena_allocate(&run->arena, sizeof(*name));
	if (name == NULL)
		return ENOMEM;
	name->type = PDF_OBJECT_NAME;

	/* The key under its full name, or a copy of its own bytes when it is not an abbreviation. */
	full = inline_key_name(key);
	if (full != NULL) {
		name->bytes = (const unsigned char *)full;
		name->length = strlen(full);
	} else {
		copy = pdf_arena_allocate(&run->arena, key->length + 1);
		if (copy == NULL)
			return ENOMEM;
		memcpy(copy, key->bytes, key->length);
		name->bytes = copy;
		name->length = key->length;
	}

	/*
	 * A colour space given by a name that is not a device space (nor its
	 * abbreviation, which the image decoder reads) is a resource of the
	 * page.
	 */
	is_colorspace = pdf_object_is_name(name, "ColorSpace");
	if (is_colorspace && value->type == PDF_OBJECT_NAME) {
		is_device = inline_device_space(value);
		if (!is_device) {
			memset(&operand, 0, sizeof(operand));
			operand.type = OPERAND_NAME;
			operand.bytes = value->bytes;
			operand.length = value->length;
			error = find_named_resource(run, resources, "ColorSpace", &operand, &space);
			if (error != 0)
				return error;
			value = space;
		}
	}

	/* Succeeded: the entry is the dictionary's. */
	image->keys[image->count] = name;
	image->values[image->count] = value;
	image->count++;
	return 0;
}

/* Tells the full name of an inline image's abbreviated key, or NULL for any other key. */
static const char *
inline_key_name(
	const struct pdf_token *key)
{
	size_t index;
	size_t length;
	int differs;

	/* Looks the key up among the abbreviations. */
	for (index = 0; index < sizeof(content_inline_keys) / sizeof(content_inline_keys[0]); index++) {
		length = strlen(content_inline_keys[index].abbreviation);
		if (key->length != length)
			continue;
		differs = memcmp(key->bytes, content_inline_keys[index].abbreviation, length);
		if (differs == 0)
			return content_inline_keys[index].name;
	}

	/* Any other key is not an abbreviation. */
	return NULL;
}

/*
 * Tells how many bytes an unfiltered inline image's samples take, or 0
 * when that is not known (a filter, a colour space whose components are
 * not known here, or values out of range).
 */
static size_t
inline_sample_bytes(
	const struct pdf_object *image)
{
	const struct pdf_object *filter;
	const struct pdf_object *space;
	const struct pdf_object *family;
	const struct pdf_object *value;
	long width;
	long height;
	long bits;
	long components;
	int is_name;
	int stencil;
	size_t row;

	/* A filtered image's size is known only after decoding. */
	filter = pdf_object_get(image, "Filter");
	if (filter != NULL)
		return 0;

	/* The width and the height. */
	value = pdf_object_get(image, "Width");
	if (value == NULL || value->type != PDF_OBJECT_INTEGER)
		return 0;
	width = value->integer;
	value = pdf_object_get(image, "Height");
	if (value == NULL || value->type != PDF_OBJECT_INTEGER)
		return 0;
	height = value->integer;
	if (width <= 0 || width > PDF_IMAGE_SIDE_MAX)
		return 0;
	if (height <= 0 || height > PDF_IMAGE_SIDE_MAX)
		return 0;

	/* A stencil mask has one bit per sample. */
	value = pdf_object_get(image, "ImageMask");
	stencil = 0;
	if (value != NULL && value->type == PDF_OBJECT_BOOLEAN)
		stencil = value->boolean;
	if (stencil) {
		row = ((size_t)width + 7) / 8;
		return row * (size_t)height;
	}

	/* The bits per component. */
	value = pdf_object_get(image, "BitsPerComponent");
	if (value == NULL || value->type != PDF_OBJECT_INTEGER)
		return 0;
	bits = value->integer;
	if (bits < 1 || bits > 16)
		return 0;

	/* The components of the device spaces, and of an indexed one (a direct array). */
	space = pdf_object_get(image, "ColorSpace");
	if (space == NULL)
		return 0;
	family = space;
	if (space->type == PDF_OBJECT_ARRAY) {
		if (space->count == 0)
			return 0;
		family = space->values[0];
	}

	/* The components, when the family says them. */
	components = 0;
	is_name = inline_device_space(family);
	if (is_name)
		components = inline_device_components(family);
	if (components == 0)
		return 0;

	/* Each row starts on a byte. */
	row = ((size_t)width * (size_t)components * (size_t)bits + 7) / 8;
	return row * (size_t)height;
}

/*
 * Finds the EI that ends an inline image's data, from a position: an E
 * and an I with white space before them and white space or the content's
 * end after them.
 */
static int
find_inline_end(
	const struct pdf_lexer *lexer,
	size_t start,
	size_t *end)
{
	const unsigned char *data;
	size_t position;
	int before_space;
	int after_space;

	/* Looks at every E from the start. */
	data = lexer->data;
	for (position = start; position + 1 < lexer->size; position++) {
		/* Only an E followed by an I can end the data. */
		if (data[position] != 'E')
			continue;
		if (data[position + 1] != 'I')
			continue;

		/* The EI must follow white space (or stand where the data starts, for empty data). */
		before_space = 1;
		if (position > 0)
			before_space = is_white(data[position - 1]);
		if (!before_space)
			continue;

		/* And be followed by white space or the end. */
		after_space = 1;
		if (position + 2 < lexer->size)
			after_space = is_white(data[position + 2]);
		if (!after_space)
			continue;

		/* Succeeded: the position of the E. */
		*end = position;
		return 0;
	}

	/* The content ends without an EI. */
	return PDF_EFORMAT;
}

/*
 * Tells whether a name is a device colour space, or a family the image
 * decoder reads by name: DeviceGray, DeviceRGB, DeviceCMYK, Indexed and
 * their inline-image abbreviations, CalGray and CalRGB.
 */
static int
inline_device_space(
	const struct pdf_object *name)
{
	int components;

	/* The spaces whose components are known by name. */
	components = inline_device_components(name);
	if (components != 0)
		return 1;

	/* Not a space known by name. */
	return 0;
}

/*
 * Tells the components of a colour space known by name (an indexed space's
 * samples have one), or 0 for any other.
 */
static int
inline_device_components(
	const struct pdf_object *name)
{
	static const struct {
		const char *name;
		int components;
	} spaces[] = {
		{ "DeviceGray", 1 },
		{ "G", 1 },
		{ "CalGray", 1 },
		{ "Indexed", 1 },
		{ "I", 1 },
		{ "DeviceRGB", 3 },
		{ "RGB", 3 },
		{ "CalRGB", 3 },
		{ "DeviceCMYK", 4 },
		{ "CMYK", 4 }
	};
	size_t index;
	int is_name;

	/* Looks the name up. */
	for (index = 0; index < sizeof(spaces) / sizeof(spaces[0]); index++) {
		is_name = pdf_object_is_name(name, spaces[index].name);
		if (is_name)
			return spaces[index].components;
	}

	/* Any other space. */
	return 0;
}

/* Tells whether a byte is white space around an inline image's EI. */
static int
is_white(
	unsigned char character)
{
	/* The white-space bytes of PDF. */
	switch (character) {
	case ' ':
	case '\n':
	case '\r':
	case '\t':
	case '\f':
	case '\0':
		return 1;
	default:
		break;
	}

	/* Anything else is part of a token. */
	return 0;
}

/*
 * Stops the page: a limit or no memory marks the list LIMITED, anything
 * else DAMAGED.
 */
static void
stop_for(
	struct content_run *run,
	int error)
{
	/* Marks why the page stops. */
	if (error == ENOMEM) {
		run->flags |= PDF_DISPLAY_LIMITED;
	} else {
		run->flags |= PDF_DISPLAY_DAMAGED;
	}

	/* Nothing more of the page is run. */
	run->stopped = 1;
}

/* Clamps a colour or alpha value to 0..1, NaN to 0. */
static double
clamp_unit(
	double value)
{
	/* Below the range, NaN included. */
	if (!(value > 0.0))
		return 0.0;

	/* Above the range. */
	if (value > 1.0)
		return 1.0;

	/* Within the range as it is. */
	return value;
}

/* Tells whether the operator being run is of the page's own content, which a scan lists the objects of. */
static int
scan_here(
	const struct content_run *run)
{
	/* A scan, at the first level, outside any form or glyph. */
	if (run->scan == NULL || run->content_level != 1 || run->form_depth != 0)
		return 0;

	/* The page's own. */
	return 1;
}

/* Notes an image being drawn as the operator's object: the matrix in force, the unit square's corners, its samples. */
static void
scan_image(
	struct content_run *run,
	struct pdf_object *image)
{
	static const double corners[8] = { 0.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0 };
	struct pdf_scan_object *object;
	struct pdf_object *value;
	double number;
	int error;

	/* The image, where the matrix in force puts it. */
	object = &run->scan_object;
	memset(object, 0, sizeof(*object));
	object->kind = CONTENT_SCAN_IMAGE;
	memcpy(object->ctm, run->stack[run->depth].ctm, sizeof(object->ctm));
	scan_corners(object->ctm, corners, object->quad);
	if (run->clip_depth > 0)
		object->clipped = 1;

	/* Its width in samples, when the dictionary says. */
	error = pdf_reader_resolve_key(run->document, image, "Width", &value);
	if (error == 0)
		error = pdf_object_number(value, &number);
	if (error == 0 && number > 0.0 && number <= (double)PDF_IMAGE_SIDE_MAX)
		object->width = (size_t)number;

	/* And its height. */
	error = pdf_reader_resolve_key(run->document, image, "Height", &value);
	if (error == 0)
		error = pdf_object_number(value, &number);
	if (error == 0 && number > 0.0 && number <= (double)PDF_IMAGE_SIDE_MAX)
		object->height = (size_t)number;

	/* The operator's object, once its bytes are known. */
	run->scan_pending = 1;
}

/* Notes a form being run as the operator's object: its box's corners through its matrix and the matrix in force. */
static void
scan_form(
	struct content_run *run,
	struct pdf_object *form)
{
	struct pdf_scan_object *object;
	struct pdf_object *matrix_object;
	struct pdf_object *box_object;
	double matrix[6];
	double placed[6];
	double box[4];
	double corners[8];
	size_t index;
	int error;

	/* The form's matrix, the identity by default (a malformed one is not the editor's). */
	matrix[0] = 1.0;
	matrix[1] = 0.0;
	matrix[2] = 0.0;
	matrix[3] = 1.0;
	matrix[4] = 0.0;
	matrix[5] = 0.0;
	error = pdf_reader_resolve_key(run->document, form, "Matrix", &matrix_object);
	if (error != 0)
		return;
	if (matrix_object->type == PDF_OBJECT_ARRAY && matrix_object->count == 6) {
		for (index = 0; index < 6; index++) {
			error = pdf_object_number(matrix_object->values[index], &matrix[index]);
			if (error != 0)
				return;
		}
	}

	/* Its box, which a form needs. */
	error = pdf_reader_resolve_key(run->document, form, "BBox", &box_object);
	if (error != 0 || box_object->type != PDF_OBJECT_ARRAY || box_object->count != 4)
		return;
	for (index = 0; index < 4; index++) {
		error = pdf_object_number(box_object->values[index], &box[index]);
		if (error != 0)
			return;
	}

	/* The box's corners (top left, top right, bottom right, bottom left) through the form's matrix and the matrix in force. */
	corners[0] = box[0];
	corners[1] = box[3];
	corners[2] = box[2];
	corners[3] = box[3];
	corners[4] = box[2];
	corners[5] = box[1];
	corners[6] = box[0];
	corners[7] = box[1];
	memcpy(placed, run->stack[run->depth].ctm, sizeof(placed));
	concat_matrix(placed, matrix);
	object = &run->scan_object;
	memset(object, 0, sizeof(*object));
	object->kind = CONTENT_SCAN_GRAPHIC;
	memcpy(object->ctm, run->stack[run->depth].ctm, sizeof(object->ctm));
	scan_corners(placed, corners, object->quad);
	if (run->clip_depth > 0)
		object->clipped = 1;

	/* The operator's object, once its bytes are known. */
	run->scan_pending = 1;
}

/*
 * Notes what an operator of the page's own content did: q and Q counted by
 * their tokens (a Q without a q is noted where it is), BT and ET, and the
 * object it drew, with its bytes from start to end and their fingerprint.
 * An object past the limit of q's nesting is left out (its matrix is not
 * the one in force, design.md [N14]).
 */
static void
scan_operator(
	struct content_run *run,
	enum content_operator code,
	size_t start,
	size_t keyword,
	size_t end,
	const unsigned char *data)
{
	struct pdf_scan *scan;
	struct pdf_scan_object *grown;
	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA2_CTX context;
	size_t capacity;
	int error;

	/* q and Q by their tokens. */
	scan = run->scan;
	if (code == OP_SAVE)
		run->scan_saves++;
	if (code == OP_RESTORE && run->scan_saves > 0) {
		run->scan_saves--;
	} else if (code == OP_RESTORE) {
		error = scan_add_stray(scan, keyword);
		if (error != 0)
			scan->error = error;
	}

	/* Whether a text object is open. */
	if (code == OP_TEXT_BEGIN)
		scan->in_text = 1;
	if (code == OP_TEXT_END)
		scan->in_text = 0;

	/* The text: the shown strings, the text objects, the marked content and what is drawn between (p002b). */
	scan_text_operator(run, code, keyword, data);
	if (code == OP_TEXT_SHOW || code == OP_TEXT_SHOW_ARRAY || code == OP_TEXT_NEXT_SHOW || code == OP_TEXT_SPACED_SHOW)
		scan_show(run, code, start, end, data);

	/* The text objects' BT and ET, and the operators that move the text position in them (ws175-p004). */
	if (code == OP_TEXT_BEGIN && scan->block_count > 0)
		scan->blocks[scan->block_count - 1U].begin = start;
	if (code == OP_TEXT_END && scan->block_count > 0) {
		scan->blocks[scan->block_count - 1U].end = end;
		scan->blocks[scan->block_count - 1U].ended = 1;
	}

	/* The marked content, its BDC and its EMC (design.md [M10]). */
	if (code == OP_IGNORED)
		scan_mark(run, start, keyword, end, data);

	/* The moves of the text position. */
	if (code == OP_TEXT_MOVE || code == OP_TEXT_MOVE_LEADING || code == OP_TEXT_MATRIX || code == OP_TEXT_NEXT_LINE)
		scan_move(run, code, start, end);

	/* Only an operator that drew an object of the page goes on. */
	if (!run->scan_pending)
		return;
	run->scan_pending = 0;

	/* Past the limit of q's nesting the matrix is not known: the object is left out. */
	if (run->ignored_saves > 0) {
		scan->partial = 1;
		return;
	}

	/* Grows the list by half again when it is full. */
	if (scan->count == scan->capacity) {
		capacity = scan->capacity + scan->capacity / 2 + 8;
		grown = realloc(scan->objects, capacity * sizeof(*grown));
		if (grown == NULL) {
			scan->error = ENOMEM;
			return;
		}

		/* The larger list. */
		scan->objects = grown;
		scan->capacity = capacity;
	}

	/* The object, its bytes and their fingerprint. */
	run->scan_object.offset = start;
	run->scan_object.length = end - start;
	SHA256Init(&context);
	SHA256Update(&context, data + start, end - start);
	SHA256Final(digest, &context);
	memcpy(run->scan_object.fingerprint, digest, sizeof(run->scan_object.fingerprint));
	scan->objects[scan->count] = run->scan_object;
	scan->count++;
}

/* Maps four corners (x, y pairs) through a matrix (a point p goes to p times the matrix). */
static void
scan_corners(
	const double matrix[6],
	const double corners[8],
	double quad[8])
{
	size_t index;
	double x;
	double y;

	/* Each corner. */
	for (index = 0; index < 4; index++) {
		x = corners[2 * index];
		y = corners[2 * index + 1];
		quad[2 * index] = matrix[0] * x + matrix[2] * y + matrix[4];
		quad[2 * index + 1] = matrix[1] * x + matrix[3] * y + matrix[5];
	}
}

/* Adds the offset of a Q without its q to a scan.  Returns 0 or ENOMEM. */
static int
scan_add_stray(
	struct pdf_scan *scan,
	size_t offset)
{
	size_t *grown;
	size_t capacity;

	/* Grows the list by half again when it is full. */
	if (scan->stray_count == scan->stray_capacity) {
		capacity = scan->stray_capacity + scan->stray_capacity / 2 + 8;
		grown = realloc(scan->stray_restores, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		scan->stray_restores = grown;
		scan->stray_capacity = capacity;
	}

	/* Succeeded: the Q is noted. */
	scan->stray_restores[scan->stray_count] = offset;
	scan->stray_count++;
	return 0;
}

/*
 * Notes a string of the page's own shown string as it starts: the text
 * matrix before its first glyph (the first string of a TJ), and its font's
 * kind.  The characters its codes stand for are noted code by code as the
 * glyphs are shown (scan_code, ws128-p004).
 */
static void
scan_string(
	struct content_run *run,
	const unsigned char *bytes,
	size_t length)
{
	struct content_state *state;
	int kind;

	/* The bytes are read code by code as they are shown. */
	(void)bytes;
	(void)length;

	/* The first glyph's place. */
	state = &run->stack[run->depth];
	if (!run->scan_show_started) {
		run->scan_show_started = 1;
		memcpy(run->scan_show_start, run->text_matrix, sizeof(run->scan_show_start));
	}

	/* The font's kind. */
	kind = pdf_font_type3(state->font);
	if (kind)
		run->scan_show_flags |= PDF_SCAN_SHOW_TYPE3;
	kind = pdf_font_vertical(state->font);
	if (kind)
		run->scan_show_flags |= PDF_SCAN_SHOW_VERTICAL;
}

/*
 * Notes the characters one code of the page's own shown string stands
 * for, and the corners of its glyph in the shown space: from the text
 * matrix before it to the one after it, from the descent to the ascent
 * (ws128-p004).  An unknown character is noted; a failure of memory fails
 * the scan.
 */
static void
scan_code(
	struct content_run *run,
	unsigned code,
	int single_byte,
	const double before[6])
{
	struct content_state *state;
	struct pdf_scan *scan;
	uint32_t characters[8];
	double quad[8];
	size_t needed;
	size_t count;
	size_t at;
	int error;

	/* The characters of the code. */
	state = &run->stack[run->depth];
	scan = run->scan;
	error = pdf_font_unicode(run->document, state->font, code, single_byte, characters, sizeof(characters) / sizeof(characters[0]), &count);
	if (error != 0) {
		scan->error = error;
		return;
	}

	/* Room for them and their corners. */
	needed = (scan->character_count + count) * 8U;
	error = scan_grow((void **)&scan->characters, &scan->character_capacity, scan->character_count + count, sizeof(*scan->characters));
	if (error == 0)
		error = scan_grow((void **)&scan->character_quads, &scan->character_quad_capacity, needed, sizeof(*scan->character_quads));
	if (error != 0) {
		scan->error = error;
		return;
	}

	/* The glyph's corners. */
	scan_glyph_quad(run, before, quad);

	/* Each character, with the corners; an unknown one is noted. */
	for (at = 0; at < count; at++) {
		if (characters[at] == 0xfffdU)
			run->scan_show_flags |= PDF_SCAN_SHOW_UNKNOWN;
		scan->characters[scan->character_count] = characters[at];
		memcpy(scan->character_quads + scan->character_count * 8U, quad, sizeof(quad));
		scan->character_count++;
	}
}

/*
 * Gives the corners of the glyph just shown, in the shown space: the text
 * space from a descent to an ascent, from where it started (before) to
 * where the next starts (the text matrix now).
 */
static void
scan_glyph_quad(
	const struct content_run *run,
	const double before[6],
	double quad[8])
{
	const struct content_state *state;
	double from[6];
	double to[6];
	double low;
	double high;

	/* The descent and the ascent, and the two places in the shown space. */
	state = &run->stack[run->depth];
	low = (-0.2 * state->font_size) + state->rise;
	high = (0.8 * state->font_size) + state->rise;
	memcpy(from, state->ctm, sizeof(from));
	concat_matrix(from, before);
	memcpy(to, state->ctm, sizeof(to));
	concat_matrix(to, run->text_matrix);

	/* Top left, top right, bottom right, bottom left. */
	quad[0] = from[2] * high + from[4];
	quad[1] = from[3] * high + from[5];
	quad[2] = to[2] * high + to[4];
	quad[3] = to[3] * high + to[5];
	quad[4] = to[2] * low + to[4];
	quad[5] = to[3] * low + to[5];
	quad[6] = from[2] * low + from[4];
	quad[7] = from[3] * low + from[5];
}

/* Tells whether the text being shown is a form's, to be noted for the page's text (every level a form, none a Type 3 glyph). */
static int
scan_form_here(
	const struct content_run *run)
{
	/* A scan, inside a form. */
	if (run->scan == NULL)
		return 0;
	if (run->form_depth == 0)
		return 0;

	/* A Type 3 glyph's procedure among the levels: its text is the glyph's picture. */
	if (run->form_depth != run->text_forms)
		return 0;

	/* A form's text. */
	return 1;
}

/*
 * Notes the characters one code of a form's shown string stands for, with
 * the corners of its glyph and what stands before them (ws177-p040); a
 * failure of memory fails the scan.
 */
static void
scan_form_code(
	struct content_run *run,
	unsigned code,
	int single_byte,
	const double before[6])
{
	struct content_state *state;
	struct pdf_scan *scan;
	uint32_t characters[8];
	double quad[8];
	unsigned what;
	size_t total;
	size_t count;
	size_t at;
	int error;

	/* The characters of the code. */
	state = &run->stack[run->depth];
	scan = run->scan;
	error = pdf_font_unicode(run->document, state->font, code, single_byte, characters, sizeof(characters) / sizeof(characters[0]), &count);
	if (error != 0) {
		scan->error = error;
		return;
	}

	/* A code that stands for nothing adds nothing. */
	if (count == 0U)
		return;

	/* Room for them, their corners and what stands before each. */
	total = scan->form_character_count + count;
	error = scan_grow((void **)&scan->form_characters, &scan->form_character_capacity, total, sizeof(*scan->form_characters));
	if (error == 0)
		error = scan_grow((void **)&scan->form_quads, &scan->form_quad_capacity, total * 8U, sizeof(*scan->form_quads));
	if (error == 0)
		error = scan_grow((void **)&scan->form_breaks, &scan->form_break_capacity, total, sizeof(*scan->form_breaks));
	if (error != 0) {
		scan->error = error;
		return;
	}

	/* The glyph's corners, and what stands before its first character. */
	scan_glyph_quad(run, before, quad);
	what = scan_form_break(run, quad);
	run->form_noted_serial = run->form_serial;

	/* Each character with the corners; the code's others follow its first on the same line. */
	for (at = 0; at < count; at++) {
		scan->form_characters[scan->form_character_count] = characters[at];
		memcpy(scan->form_quads + scan->form_character_count * 8U, quad, sizeof(quad));
		scan->form_breaks[scan->form_character_count] = (unsigned char)what;
		scan->form_character_count++;
		what = PDF_SCAN_FORM_SAME;
	}
}

/*
 * Decides what stands before a form's glyph (PDF_SCAN_FORM_*): the first,
 * one in another run of a form's text (another form, or the same after a
 * nested one), or one whose start is more than half its height off the
 * last one's baseline, or well back along it, starts a line; one that
 * starts along the line more than a fifth of its size past where the last
 * ended has a space before it; anything else follows on.
 */
static unsigned
scan_form_break(
	const struct content_run *run,
	const double quad[8])
{
	const struct pdf_scan *scan;
	const double *last;
	double height;
	double up_x;
	double up_y;
	double along_x;
	double along_y;
	double length;
	double gap_x;
	double gap_y;
	double across;
	double ahead;
	double off;

	/* The first of the page's form text, or the first of another run. */
	scan = run->scan;
	if (scan->form_character_count == 0U)
		return PDF_SCAN_FORM_LINE;
	if (run->form_noted_serial != run->form_serial)
		return PDF_SCAN_FORM_LINE;

	/* The glyph's height and its upward direction (bottom left to top left). */
	up_x = quad[0] - quad[6];
	up_y = quad[1] - quad[7];
	height = sqrt(up_x * up_x + up_y * up_y);
	if (!(height > 1e-9))
		return PDF_SCAN_FORM_LINE;
	up_x /= height;
	up_y /= height;

	/* Its baseline's direction, at a right angle to it. */
	along_x = up_y;
	along_y = -up_x;
	length = sqrt((quad[4] - quad[6]) * (quad[4] - quad[6]) + (quad[5] - quad[7]) * (quad[5] - quad[7]));
	if (length > 1e-9) {
		along_x = (quad[4] - quad[6]) / length;
		along_y = (quad[5] - quad[7]) / length;
	}

	/* From the last glyph's end on its baseline (its bottom right) to this one's start (its bottom left). */
	last = scan->form_quads + (scan->form_character_count - 1U) * 8U;
	gap_x = quad[6] - last[4];
	gap_y = quad[7] - last[5];
	across = gap_x * up_x + gap_y * up_y;
	ahead = gap_x * along_x + gap_y * along_y;

	/* Off the line, or well back along it: a new line. */
	off = fabs(across);
	if (off > height / 2.0)
		return PDF_SCAN_FORM_LINE;
	if (ahead < -height)
		return PDF_SCAN_FORM_LINE;

	/* Apart along the line: a space. */
	if (ahead > height / 5.0)
		return PDF_SCAN_FORM_SPACE;

	/* On the same line, next to the last. */
	return PDF_SCAN_FORM_SAME;
}

/*
 * Notes a shown string of the page's own content (Tj, TJ, ', "): its bytes,
 * its text object, the text matrices before and after it, the state it was
 * shown in, its characters and the corners of its line.
 */
static void
scan_show(
	struct content_run *run,
	enum content_operator code,
	size_t start,
	size_t end,
	const unsigned char *data)
{
	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA2_CTX context;
	struct content_state *state;
	struct pdf_scan_show *show;
	struct pdf_scan *scan;
	double from[6];
	double to[6];
	double corners[8];
	double low;
	double high;
	int error;

	/* Room for it. */
	scan = run->scan;
	state = &run->stack[run->depth];
	error = scan_grow((void **)&scan->shows, &scan->show_capacity, scan->show_count + 1U, sizeof(*scan->shows));
	if (error != 0) {
		scan->error = error;
		return;
	}

	/* Its bytes, its text object and its matrices (an empty string starts and ends where the position is). */
	show = &scan->shows[scan->show_count];
	memset(show, 0, sizeof(*show));
	show->offset = start;
	show->length = end - start;
	show->op = PDF_SCAN_SHOW_TJ;
	if (code == OP_TEXT_SHOW_ARRAY)
		show->op = PDF_SCAN_SHOW_ARRAY;
	if (code == OP_TEXT_NEXT_SHOW)
		show->op = PDF_SCAN_SHOW_NEXT;
	if (code == OP_TEXT_SPACED_SHOW)
		show->op = PDF_SCAN_SHOW_SPACED;
	SHA256Init(&context);
	SHA256Update(&context, data + start, end - start);
	SHA256Final(digest, &context);
	memcpy(show->fingerprint, digest, sizeof(show->fingerprint));
	show->block = 0;
	if (scan->block_count > 0)
		show->block = scan->block_count - 1U;
	memcpy(show->start, run->text_matrix, sizeof(show->start));
	if (run->scan_show_started)
		memcpy(show->start, run->scan_show_start, sizeof(show->start));
	memcpy(show->end, run->text_matrix, sizeof(show->end));
	memcpy(show->ctm, state->ctm, sizeof(show->ctm));

	/* The state it was shown in (the font's name in the resources too, ws175-p005). */
	show->font = state->font;
	show->font_resource_length = state->font_resource_length;
	memcpy(show->font_resource, state->font_resource, state->font_resource_length);
	show->font_size = state->font_size;
	show->character_spacing = state->character_spacing;
	show->word_spacing = state->word_spacing;
	show->horizontal_scale = state->horizontal_scale;
	show->leading = state->leading;
	show->rise = state->rise;
	show->render_mode = state->render_mode;
	if (state->fill_usable && !state->fill_pattern_space) {
		memcpy(show->fill, state->fill, sizeof(show->fill));
		show->fill_known = 1;
	}

	/* Its characters, what came between it and the shown string before, the marked content within its text object. */
	show->characters_from = run->scan_show_from;
	show->characters_count = scan->character_count - run->scan_show_from;
	show->drawn_before = run->scan_drawn;
	run->scan_drawn = 0;
	show->marked_depth = 0;
	if (run->scan_marked > run->scan_marked_at_text)
		show->marked_depth = run->scan_marked - run->scan_marked_at_text;
	show->flags = run->scan_show_flags;

	/* The corners of its line: the text space from a descent to an ascent, from the first glyph's place to the last's end. */
	low = (-0.2 * show->font_size) + show->rise;
	high = (0.8 * show->font_size) + show->rise;
	memcpy(from, show->ctm, sizeof(from));
	concat_matrix(from, show->start);
	memcpy(to, show->ctm, sizeof(to));
	concat_matrix(to, show->end);
	corners[0] = 0.0;
	corners[1] = high;
	corners[2] = 0.0;
	corners[3] = high;
	corners[4] = 0.0;
	corners[5] = low;
	corners[6] = 0.0;
	corners[7] = low;
	show->quad[0] = from[0] * corners[0] + from[2] * corners[1] + from[4];
	show->quad[1] = from[1] * corners[0] + from[3] * corners[1] + from[5];
	show->quad[2] = to[0] * corners[2] + to[2] * corners[3] + to[4];
	show->quad[3] = to[1] * corners[2] + to[3] * corners[3] + to[5];
	show->quad[4] = to[0] * corners[4] + to[2] * corners[5] + to[4];
	show->quad[5] = to[1] * corners[4] + to[3] * corners[5] + to[5];
	show->quad[6] = from[0] * corners[6] + from[2] * corners[7] + from[4];
	show->quad[7] = from[1] * corners[6] + from[3] * corners[7] + from[5];
	scan->show_count++;

	/* A string that clips makes its text object one the editor does not change (design.md [H2]). */
	if (show->render_mode >= 4 && show->block < scan->block_count)
		scan->block_clips[show->block] = 1U;
}

/*
 * Notes the text objects (BT), the marked content (BMC, BDC, EMC) and the
 * operators that draw or change the graphics state, between which the
 * lines of adjacent text objects do not join.
 */
static void
scan_text_operator(
	struct content_run *run,
	enum content_operator code,
	size_t keyword,
	const unsigned char *data)
{
	struct pdf_scan *scan;
	int marked;
	int error;

	/* A text object: its number, its clip not known yet, the marked content open as it starts. */
	scan = run->scan;
	if (code == OP_TEXT_BEGIN) {
		error = scan_grow((void **)&scan->block_clips, &scan->block_clip_capacity, scan->block_count + 1U, 1U);
		if (error != 0) {
			scan->error = error;
			return;
		}

		/* Its record, its BT and ET to come (scan_operator). */
		error = scan_grow((void **)&scan->blocks, &scan->block_capacity, scan->block_count + 1U, sizeof(*scan->blocks));
		if (error != 0) {
			scan->error = error;
			return;
		}

		/* Not known yet. */
		memset(&scan->blocks[scan->block_count], 0, sizeof(*scan->blocks));

		/* The new text object. */
		scan->block_clips[scan->block_count] = 0U;
		scan->block_count++;
		run->scan_marked_at_text = run->scan_marked;
		return;
	}

	/* Marked content: BMC and BDC open one, EMC closes one (the interpreter ignores them). */
	if (code == OP_IGNORED) {
		marked = memcmp(data + keyword, "BMC", 3) == 0 || memcmp(data + keyword, "BDC", 3) == 0;
		if (marked)
			run->scan_marked++;
		marked = memcmp(data + keyword, "EMC", 3) == 0;
		if (marked && run->scan_marked > 0)
			run->scan_marked--;
		return;
	}

	/* The operators that draw or change the graphics state (not the text's own). */
	switch (code) {
	case OP_SAVE:
	case OP_RESTORE:
	case OP_CONCAT:
	case OP_EXTGSTATE:
	case OP_STROKE:
	case OP_CLOSE_STROKE:
	case OP_FILL:
	case OP_FILL_EVEN_ODD:
	case OP_FILL_STROKE:
	case OP_FILL_STROKE_EVEN_ODD:
	case OP_CLOSE_FILL_STROKE:
	case OP_CLOSE_FILL_STROKE_EVEN_ODD:
	case OP_END_PATH:
	case OP_GRAY_FILL:
	case OP_GRAY_STROKE:
	case OP_RGB_FILL:
	case OP_RGB_STROKE:
	case OP_CMYK_FILL:
	case OP_CMYK_STROKE:
	case OP_SPACE_FILL:
	case OP_SPACE_STROKE:
	case OP_COLOR_FILL:
	case OP_COLOR_STROKE:
	case OP_XOBJECT:
	case OP_INLINE_IMAGE:
	case OP_SHADING:
		run->scan_drawn++;
		break;
	default:
		break;
	}
}

/*
 * Notes an operator that moves the text position in a text object of the
 * page's own content (ws175-p004): its bytes, its text object, and the
 * leading a TD sets.  A failure of memory fails the scan.
 */
static void
scan_move(
	struct content_run *run,
	enum content_operator code,
	size_t start,
	size_t end)
{
	struct pdf_scan_move *move;
	struct pdf_scan *scan;
	int error;

	/* Only in a text object. */
	scan = run->scan;
	if (!scan->in_text || scan->block_count == 0)
		return;

	/* Room for it. */
	error = scan_grow((void **)&scan->moves, &scan->move_capacity, scan->move_count + 1U, sizeof(*scan->moves));
	if (error != 0) {
		scan->error = error;
		return;
	}

	/* Its bytes, its text object, and the leading after a TD (the state's now). */
	move = &scan->moves[scan->move_count];
	memset(move, 0, sizeof(*move));
	move->offset = start;
	move->length = end - start;
	move->block = scan->block_count - 1U;
	if (code == OP_TEXT_MOVE_LEADING) {
		move->sets_leading = 1;
		move->leading = run->stack[run->depth].leading;
	}

	/* One more. */
	scan->move_count++;
}

/*
 * Notes the marked content of the page's own content (ws175-p004): a BDC's
 * bytes as it opens (a BMC is counted, to pair the EMC), the end of its
 * EMC as it closes.  A failure of memory fails the scan.
 */
static void
scan_mark(
	struct content_run *run,
	size_t start,
	size_t keyword,
	size_t end,
	const unsigned char *data)
{
	struct pdf_scan *scan;
	size_t record;
	int is_bdc;
	int is_bmc;
	int is_emc;
	int error;

	/* Which operator. */
	scan = run->scan;
	if (end - keyword != 3U)
		return;
	is_bdc = memcmp(data + keyword, "BDC", 3) == 0;
	is_bmc = memcmp(data + keyword, "BMC", 3) == 0;
	is_emc = memcmp(data + keyword, "EMC", 3) == 0;

	/* An EMC closes the last opened; a BDC's records its end. */
	if (is_emc) {
		if (scan->mark_depth == 0)
			return;
		scan->mark_depth--;
		record = scan->mark_stack[scan->mark_depth];
		if (record != (size_t)-1)
			scan->marks[record].end = end;
		return;
	}

	/* An opening: a BDC's record, a BMC's place in the stack. */
	if (!is_bdc && !is_bmc)
		return;
	error = scan_grow((void **)&scan->mark_stack, &scan->mark_stack_capacity, scan->mark_depth + 1U, sizeof(*scan->mark_stack));
	if (error == 0 && is_bdc)
		error = scan_grow((void **)&scan->marks, &scan->mark_capacity, scan->mark_count + 1U, sizeof(*scan->marks));
	if (error != 0) {
		scan->error = error;
		return;
	}

	/* The record. */
	record = (size_t)-1;
	if (is_bdc) {
		record = scan->mark_count;
		scan->marks[record].offset = start;
		scan->marks[record].length = end - start;
		scan->marks[record].end = 0;
		scan->mark_count++;
	}

	/* On the stack. */
	scan->mark_stack[scan->mark_depth] = record;
	scan->mark_depth++;
}

/* Grows an array of items of a size to hold count of them.  Returns 0 or ENOMEM. */
static int
scan_grow(
	void **items,
	size_t *capacity,
	size_t count,
	size_t size)
{
	void *grown;
	size_t larger;

	/* Room enough already. */
	if (count <= *capacity)
		return 0;

	/* Half again, at least the count. */
	larger = *capacity + *capacity / 2U + 16U;
	if (larger < count)
		larger = count;
	grown = realloc(*items, larger * size);
	if (grown == NULL)
		return ENOMEM;

	/* Succeeded: the larger room. */
	*items = grown;
	*capacity = larger;
	return 0;
}
