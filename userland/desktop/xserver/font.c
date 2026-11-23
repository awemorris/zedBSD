/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The core fonts: their names, their metrics, their glyphs, and the
 * requests of fonts and of text.
 *
 * The server has no font files of the X kind.  Each core font is a face
 * the caller gave (glyphs.c) at a pixel size, under a name of the X
 * Logical Font Description: the monospaced face answers to misc-fixed (the
 * cells of X's familiar fixed fonts, 6x13 and the others, with the same
 * ascent and descent) and courier, the proportional face to helvetica and
 * times, each medium and bold, in ISO 8859-1 and ISO 10646.  Any pixel
 * size is opened through the scalable names (a 0 in the size fields).  A
 * monospaced font is a cell every character shares, so QueryFont sends no
 * per-character metrics for it; a proportional one has each character's.
 * The cursor font answers the standard cursors' numbers (cursor.c), and
 * zed-unicode is the 8x16 font of the tree's own clients.
 *
 * Glyphs are drawn from the face at the font's size into one bit a pixel
 * (half covered or more) and kept, a few hundred a font.  A monospaced
 * font draws the line-drawing characters itself across its whole cell, at
 * their Unicode places and at 1 to 31, where X's fixed fonts keep the DEC
 * special graphics.
 */

#include "userland/desktop/xserver/internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The requests answered here. */
#define REQUEST_OPEN_FONT		45U
#define REQUEST_CLOSE_FONT		46U
#define REQUEST_QUERY_FONT		47U
#define REQUEST_QUERY_TEXT_EXTENTS	48U
#define REQUEST_LIST_FONTS		49U
#define REQUEST_LIST_FONTS_WITH_INFO	50U
#define REQUEST_SET_FONT_PATH		51U
#define REQUEST_GET_FONT_PATH		52U
#define REQUEST_POLY_TEXT8		74U
#define REQUEST_POLY_TEXT16		75U
#define REQUEST_IMAGE_TEXT8		76U
#define REQUEST_IMAGE_TEXT16		77U

/* The fields of an XLFD name, and those it is read by. */
#define FONT_FIELDS			14U
#define FIELD_FOUNDRY			0U
#define FIELD_FAMILY			1U
#define FIELD_WEIGHT			2U
#define FIELD_SLANT			3U
#define FIELD_SETWIDTH			4U
#define FIELD_ADD_STYLE			5U
#define FIELD_PIXEL			6U
#define FIELD_POINT			7U
#define FIELD_RESOLUTION_X		8U
#define FIELD_RESOLUTION_Y		9U
#define FIELD_SPACING			10U
#define FIELD_AVERAGE			11U
#define FIELD_REGISTRY			12U
#define FIELD_ENCODING			13U

/* The resolution the fonts are named at, and the size a scalable name without one opens at. */
#define FONT_RESOLUTION			75U
#define FONT_PIXELS_DEFAULT		13U

/* The names of the two special fonts. */
#define FONT_CURSOR_NAME		"cursor"
#define FONT_ZED_NAME			"zed-unicode"

/* The font a graphics context without one draws with. */
#define FONT_DEFAULT_NAME		"fixed"

/* The cursor font's characters (the standard cursors and their masks) and its cell. */
#define FONT_CURSOR_CHARS		154U
#define FONT_CURSOR_CELL		16

/* The zed-unicode font: its cell, the size its glyphs are drawn at and how far they are raised. */
#define FONT_ZED_WIDTH			8
#define FONT_ZED_HEIGHT			16
#define FONT_ZED_PIXELS			13U
#define FONT_ZED_RAISE			4

/* The rows of the second byte a proportional ISO 10646 font has metrics for (Latin, Greek, Cyrillic). */
#define FONT_WIDE_ROWS			6U

/* The one directory GetFontPath answers. */
#define FONT_PATH			"built-ins"

/* The glyphs a text request sends to be drawn at once. */
#define FONT_RUN_GLYPHS			256U

/* The most properties a font answers with. */
#define FONT_PROPERTIES_MAX		20U

/* How much coverage sets a pixel of a glyph. */
#define FONT_COVERED			128U

/* The shapes the line-drawing characters are made of: half lines from the cell's middle, and the scan lines. */
#define LINE_UP				0x01U
#define LINE_DOWN			0x02U
#define LINE_LEFT			0x04U
#define LINE_RIGHT			0x08U
#define LINE_SCAN			0x10U

/*
 * A family the fonts are named in: its foundry and name, the face's role
 * of its medium weight (the bold is the next role), and its spacing.
 */
struct font_family {
	const char *foundry;
	const char *family;
	unsigned role;
	char spacing;
};

/*
 * One of X's fixed cells: its width and height, the setwidth it is named
 * with, its point size, and the ascent and descent.
 */
struct font_cell {
	int width;
	int height;
	const char *setwidth;
	unsigned point;
	int ascent;
	int descent;
};

/*
 * An alias: a short name and the name (or pattern) it stands for.
 */
struct font_alias {
	const char *alias;
	const char *name;
};

/*
 * A line-drawing character: its Unicode code point, the half lines it is
 * made of, and for a scan line which of the nine (1 the top, 9 the
 * bottom).
 */
struct font_line {
	uint32_t codepoint;
	unsigned shape;
	unsigned scan;
};

/*
 * What a font name asks for: the family, the weight, the slant (r, o or
 * i: the faces are upright, the letter is kept), the setwidth, the pixel
 * and point sizes and the resolution, the average width in tenths of a
 * pixel (0: the font's own) and whether the characters are ISO 10646's
 * (else ISO 8859-1's).
 */
struct font_spec {
	const struct font_family *family;
	int bold;
	char slant;
	char setwidth[16];
	unsigned pixel;
	unsigned point;
	unsigned resolution_x;
	unsigned resolution_y;
	unsigned average;
	int wide;
};

/*
 * The glyphs of a text request on their way to be drawn: where each goes
 * and its bits, and the box they cover.
 */
struct font_run {
	struct x11_text_glyph *glyphs;
	size_t count;
	size_t capacity;
	uint8_t *bits;
	size_t used;
	size_t size;
	struct x11_box extent;
};

/*
 * One property of a font as QueryFont sends it: its name, and either a
 * number or the name of the atom that is its value.
 */
struct font_property {
	const char *name;
	uint32_t value;
	const char *text;
	size_t text_length;
};

/*
 * The families the fonts are named in.
 */
static const struct font_family font_families[] = {
	{ "misc", "fixed", X11_FACE_MONO, 'c' },
	{ "adobe", "courier", X11_FACE_MONO, 'm' },
	{ "adobe", "helvetica", X11_FACE_SANS, 'p' },
	{ "adobe", "times", X11_FACE_SANS, 'p' }
};

/*
 * X's fixed cells, by which misc-fixed of a size has the same cell and
 * baseline as the bitmap font of that name (xterm and others size their
 * windows by them).
 *
 * Values referenced: X.Org xfonts-base 1.0.5 (Debian 1:1.0.5+nmu1), the
 * misc-fixed and sony-fixed bitmap fonts' FONT_ASCENT and FONT_DESCENT and
 * their names' sizes, as Xvfb of xorg-server 21.1 reports them.
 */
static const struct font_cell font_cells[] = {
	{ 5, 7, "normal", 70U, 6, 1 },
	{ 5, 8, "normal", 80U, 7, 1 },
	{ 6, 9, "normal", 90U, 7, 2 },
	{ 6, 10, "normal", 100U, 8, 2 },
	{ 6, 12, "semicondensed", 110U, 10, 2 },
	{ 6, 13, "semicondensed", 120U, 11, 2 },
	{ 7, 13, "normal", 120U, 11, 2 },
	{ 7, 14, "normal", 130U, 12, 2 },
	{ 8, 13, "normal", 120U, 11, 2 },
	{ 8, 16, "normal", 150U, 14, 2 },
	{ 9, 15, "normal", 140U, 12, 3 },
	{ 9, 18, "normal", 170U, 14, 4 },
	{ 10, 20, "normal", 200U, 16, 4 },
	{ 12, 24, "normal", 230U, 22, 2 }
};

/*
 * The short names of the fixed fonts.
 *
 * Values referenced: X.Org xfonts-base 1.0.5 (Debian 1:1.0.5+nmu1),
 * /usr/share/fonts/X11/misc/fonts.alias (the aliases fixed, variable and
 * 5x7 to 12x24); 8x16 and 12x24 stand for misc-fixed here.
 */
static const struct font_alias font_aliases[] = {
	{ "fixed", "-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso8859-1" },
	{ "variable", "-*-helvetica-bold-r-normal-*-*-120-*-*-*-*-iso8859-1" },
	{ "5x7", "-misc-fixed-medium-r-normal--7-70-75-75-c-50-iso8859-1" },
	{ "5x8", "-misc-fixed-medium-r-normal--8-80-75-75-c-50-iso8859-1" },
	{ "6x9", "-misc-fixed-medium-r-normal--9-90-75-75-c-60-iso8859-1" },
	{ "6x10", "-misc-fixed-medium-r-normal--10-100-75-75-c-60-iso8859-1" },
	{ "6x12", "-misc-fixed-medium-r-semicondensed--12-110-75-75-c-60-iso8859-1" },
	{ "6x13", "-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso8859-1" },
	{ "6x13bold", "-misc-fixed-bold-r-semicondensed--13-120-75-75-c-60-iso8859-1" },
	{ "7x13", "-misc-fixed-medium-r-normal--13-120-75-75-c-70-iso8859-1" },
	{ "7x13bold", "-misc-fixed-bold-r-normal--13-120-75-75-c-70-iso8859-1" },
	{ "7x14", "-misc-fixed-medium-r-normal--14-130-75-75-c-70-iso8859-1" },
	{ "7x14bold", "-misc-fixed-bold-r-normal--14-130-75-75-c-70-iso8859-1" },
	{ "8x13", "-misc-fixed-medium-r-normal--13-120-75-75-c-80-iso8859-1" },
	{ "8x13bold", "-misc-fixed-bold-r-normal--13-120-75-75-c-80-iso8859-1" },
	{ "8x16", "-misc-fixed-medium-r-normal--16-150-75-75-c-80-iso8859-1" },
	{ "9x15", "-misc-fixed-medium-r-normal--15-140-75-75-c-90-iso8859-1" },
	{ "9x15bold", "-misc-fixed-bold-r-normal--15-140-75-75-c-90-iso8859-1" },
	{ "10x20", "-misc-fixed-medium-r-normal--20-200-75-75-c-100-iso8859-1" },
	{ "12x24", "-misc-fixed-medium-r-normal--24-230-75-75-c-120-iso8859-1" }
};

/*
 * The pixel sizes courier, helvetica and times are listed at (any other
 * opens through their scalable names).
 */
static const unsigned font_sizes[] = { 8U, 10U, 12U, 14U, 18U, 24U };

/*
 * What a fixed font keeps at 1 to 31: the DEC special graphics, the
 * characters a VT100 shows for 0x5f to 0x7e in its line-drawing set, in
 * the order of 0x60 to 0x7e (diamond, checker board, the signs of HT, FF,
 * CR and LF, degree, plus-minus, NL, VT, the corners, the cross, the scan
 * lines, the tees, the vertical line, less and greater or equal, pi, not
 * equal, pound and the middle dot).
 *
 * Values referenced: the Unicode code points of the DEC Special Graphics
 * set as xterm 397 maps them (charsets.c, the table of the '0' set).
 */
static const uint16_t font_dec_graphics[31] = {
	0x25c6U, 0x2592U, 0x2409U, 0x240cU, 0x240dU, 0x240aU, 0x00b0U, 0x00b1U,
	0x2424U, 0x240bU, 0x2518U, 0x2510U, 0x250cU, 0x2514U, 0x253cU, 0x23baU,
	0x23bbU, 0x2500U, 0x23bcU, 0x23bdU, 0x251cU, 0x2524U, 0x2534U, 0x252cU,
	0x2502U, 0x2264U, 0x2265U, 0x03c0U, 0x2260U, 0x00a3U, 0x00b7U
};

/*
 * The line-drawing characters a fixed font draws itself, so that they
 * meet the next cell's.
 */
static const struct font_line font_lines[] = {
	{ 0x2500U, LINE_LEFT | LINE_RIGHT, 0U },
	{ 0x2502U, LINE_UP | LINE_DOWN, 0U },
	{ 0x250cU, LINE_RIGHT | LINE_DOWN, 0U },
	{ 0x2510U, LINE_LEFT | LINE_DOWN, 0U },
	{ 0x2514U, LINE_UP | LINE_RIGHT, 0U },
	{ 0x2518U, LINE_UP | LINE_LEFT, 0U },
	{ 0x251cU, LINE_UP | LINE_DOWN | LINE_RIGHT, 0U },
	{ 0x2524U, LINE_UP | LINE_DOWN | LINE_LEFT, 0U },
	{ 0x252cU, LINE_LEFT | LINE_RIGHT | LINE_DOWN, 0U },
	{ 0x2534U, LINE_LEFT | LINE_RIGHT | LINE_UP, 0U },
	{ 0x253cU, LINE_UP | LINE_DOWN | LINE_LEFT | LINE_RIGHT, 0U },
	{ 0x23baU, LINE_SCAN, 1U },
	{ 0x23bbU, LINE_SCAN, 3U },
	{ 0x23bcU, LINE_SCAN, 7U },
	{ 0x23bdU, LINE_SCAN, 9U }
};

static unsigned font_open(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned font_close(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned font_query(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned font_text_extents(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned font_list(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned font_list_with_info(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned font_set_path(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned font_get_path(struct x11server *server, unsigned index, size_t length);
static unsigned font_image_text(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned font_poly_text(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static struct x11_font *font_fontable(struct x11server *server, uint32_t id);
static struct x11_font *font_of_gc(struct x11server *server, const struct x11_gc *gc);
static int font_resolve(struct x11server *server, const char *name, size_t length, unsigned owner, struct x11_font **result);
static int font_names(struct x11server *server);
static int font_names_add(char **names, size_t *size, size_t *capacity, const char *name);
static int font_names_family(struct x11server *server, char **names, size_t *size, size_t *capacity, const struct font_family *family, int wide);
static int font_glob(const char *pattern, size_t pattern_length, const char *name);
static int font_split(const char *name, const char **fields, size_t *lengths);
static int font_parse(const char *name, struct font_spec *spec);
static int font_number(const char *field, size_t length, unsigned *value);
static int font_field_is(const char *field, size_t length, const char *word);
static void font_name(const struct font_spec *spec, char *name, size_t size);
static int font_scalable(struct x11server *server, const char *pattern, size_t length, unsigned owner, struct x11_font **result);
static int font_make(struct x11server *server, struct font_spec *spec, unsigned owner, struct x11_font **result);
static int font_make_special(struct x11server *server, const char *name, unsigned owner, struct x11_font **result);
static void font_make_mono(struct x11server *server, struct x11_font *font, struct font_spec *spec);
static int font_make_proportional(struct x11server *server, struct x11_font *font, struct font_spec *spec);
static int font_charge(struct x11server *server, struct x11_font *font);
static const struct font_cell *font_cell_of(const struct font_spec *spec);
static int font_char(const struct x11_font *font, uint32_t character, struct x11_char_metrics *metrics);
static int font_char_or_default(const struct x11_font *font, uint32_t *character, struct x11_char_metrics *metrics);
static uint32_t font_codepoint(const struct x11_font *font, uint32_t character);
static const struct font_line *font_line_of(uint32_t codepoint);
static void font_draw_line(const struct font_line *line, struct x11_font_glyph *glyph);
static size_t font_info(struct x11server *server, const struct x11_font *font, struct x11_client *client, uint8_t **reply, size_t extra);
static unsigned font_properties(const struct x11_font *font, struct font_property *properties);
static void font_write_metrics(uint8_t *bytes, const struct x11_char_metrics *metrics, int msb);
static void font_run_init(struct font_run *run);
static void font_run_fini(struct font_run *run);
static int font_run_add(struct x11server *server, struct font_run *run, struct x11_font *font, uint32_t character, int x, int y);
static unsigned font_run_draw(struct x11server *server, struct font_run *run, uint32_t drawable, uint32_t gc);

/*
 * Answers a request of fonts or of text: OpenFont, CloseFont, QueryFont,
 * QueryTextExtents, ListFonts, ListFontsWithInfo, SetFontPath,
 * GetFontPath, PolyText8, PolyText16, ImageText8 and ImageText16.
 * Returns 0, or the error to send.
 */
unsigned
x11_font_request(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	unsigned code;

	/* The request's own handler. */
	switch (request[0]) {
	case REQUEST_OPEN_FONT:
		code = font_open(server, index, request, length);
		break;
	case REQUEST_CLOSE_FONT:
		code = font_close(server, index, request, length);
		break;
	case REQUEST_QUERY_FONT:
		code = font_query(server, index, request, length);
		break;
	case REQUEST_QUERY_TEXT_EXTENTS:
		code = font_text_extents(server, index, request, length);
		break;
	case REQUEST_LIST_FONTS:
		code = font_list(server, index, request, length);
		break;
	case REQUEST_LIST_FONTS_WITH_INFO:
		code = font_list_with_info(server, index, request, length);
		break;
	case REQUEST_SET_FONT_PATH:
		code = font_set_path(server, index, request, length);
		break;
	case REQUEST_GET_FONT_PATH:
		code = font_get_path(server, index, length);
		break;
	case REQUEST_IMAGE_TEXT8:
	case REQUEST_IMAGE_TEXT16:
		code = font_image_text(server, index, request, length);
		break;
	case REQUEST_POLY_TEXT8:
	case REQUEST_POLY_TEXT16:
		code = font_poly_text(server, index, request, length);
		break;
	default:
		code = X11_BAD_REQUEST;
		break;
	}

	/* Succeeded: answered, or the error to send. */
	return code;
}

/*
 * Finds an open font by its id.  Returns it, or NULL.
 */
struct x11_font *
x11_font_find(
	struct x11server *server,
	uint32_t id)
{
	struct x11_font *font;

	/* The id's resource, when it is a font. */
	font = x11_resource_find(server, id, X11_RESOURCE_FONT);
	if (font == NULL)
		return NULL;

	/* Succeeded: the font. */
	return font;
}

/*
 * Frees a font (out of the resource ids already): its glyphs, its metrics
 * and its charge.
 */
void
x11_font_free(
	struct x11server *server,
	struct x11_font *font)
{
	unsigned slot;

	/* The glyphs kept. */
	for (slot = 0U; slot < X11_FONT_CACHE; slot++)
		free(font->cache[slot].bits);

	/* The metrics, the charge and the font. */
	free(font->metrics);
	x11_memory_release(server, font->owner, font->charged);
	free(font);
}

/*
 * Frees what the server holds of fonts itself: the default font and the
 * names ListFonts answers.
 */
void
x11_fonts_fini(
	struct x11server *server)
{
	/* The default font. */
	if (server->default_font != NULL) {
		x11_font_free(server, server->default_font);
		server->default_font = NULL;
	}

	/* The names. */
	free(server->font_names);
	server->font_names = NULL;
	server->font_names_size = 0U;
}

/*
 * Tells whether a font has a character (the first byte in its high byte):
 * 1, or 0.
 */
int
x11_font_has_char(
	const struct x11_font *font,
	uint32_t character)
{
	struct x11_char_metrics metrics;
	int present;

	/* The character's metrics, when it has any. */
	present = font_char(font, character, &metrics);
	if (!present)
		return 0;

	/* Succeeded: the font has it. */
	return 1;
}

/*
 * Gives a character's glyph as the font draws it (kept until the font's
 * next glyph is drawn in its slot).  Returns it, or NULL for a character
 * with no bits (none in the font, or drawn empty).
 */
const struct x11_font_glyph *
x11_font_glyph(
	struct x11server *server,
	struct x11_font *font,
	uint32_t character)
{
	struct x11_face_glyph drawn;
	struct x11_char_metrics metrics;
	struct x11_font_glyph *glyph;
	const struct font_line *line;
	uint32_t codepoint;
	size_t bytes;
	int present;
	int error;
	int x;
	int y;
	int column;
	int row;

	/* The cursor font's glyphs are the standard cursors, shown by the backend, not drawn. */
	if (font->kind == X11_FONT_CURSOR)
		return NULL;

	/* A glyph kept. */
	glyph = &font->cache[character % X11_FONT_CACHE];
	if (glyph->used && glyph->codepoint == character) {
		if (glyph->bits == NULL)
			return NULL;
		return glyph;
	}

	/* Its box: the cell of a monospaced font, the ink of a proportional one's character. */
	present = font_char(font, character, &metrics);
	if (!present)
		return NULL;
	free(glyph->bits);
	memset(glyph, 0, sizeof(*glyph));
	glyph->codepoint = character;
	glyph->used = 1;
	glyph->left = metrics.left;
	glyph->top = metrics.ascent;
	glyph->width = metrics.right - metrics.left;
	glyph->height = metrics.ascent + metrics.descent;
	if (glyph->width <= 0 || glyph->height <= 0)
		return NULL;

	/* Its bits, all clear. */
	glyph->stride = ((size_t)glyph->width + 7U) / 8U;
	bytes = glyph->stride * (size_t)glyph->height;
	glyph->bits = calloc(1U, bytes);
	if (glyph->bits == NULL) {
		glyph->used = 0;
		return NULL;
	}

	/* A line-drawing character of a monospaced font, across its cell. */
	codepoint = font_codepoint(font, character);
	line = NULL;
	if (font->line_drawing)
		line = font_line_of(codepoint);
	if (line != NULL) {
		font_draw_line(line, glyph);
		return glyph;
	}

	/* The face's coverage (a glyph it cannot draw stays clear). */
	error = x11_face_draw(&server->faces, font->role, font->pixels, codepoint, &drawn);
	if (error != 0)
		return glyph;

	/*
	 * Each bit set where the face covers half the pixel or more: the
	 * glyph's pixel (x, y) is the face's pixel moved right by the font's
	 * shift and up by its raise, kept inside the box.
	 */
	for (row = 0; row < glyph->height; row++) {
		y = row - glyph->top + drawn.top + font->raise;
		if (y < 0 || y >= drawn.height)
			continue;
		for (column = 0; column < glyph->width; column++) {
			x = column + glyph->left - font->shift_x - drawn.left;
			if (x < 0 || x >= drawn.width)
				continue;
			if (drawn.coverage[(size_t)y * drawn.stride + (size_t)x] >= FONT_COVERED)
				glyph->bits[(size_t)row * glyph->stride + (size_t)column / 8U] |= (uint8_t)(0x80U >> (column % 8));
		}
	}

	/* Succeeded: drawn and kept. */
	return glyph;
}

/* OpenFont: a font by its name, a pattern or an alias (the first that matches). */
static unsigned
font_open(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_font *font;
	size_t name_length;
	uint32_t id;
	unsigned taken;
	int chosen;
	int error;

	/* The name, filling the request to its last word. */
	client = &server->clients[index];
	if (length < 12U)
		return X11_BAD_LENGTH;
	name_length = x11_read16(request + 8, client->order);
	if (12U + ((name_length + 3U) & ~(size_t)3U) != length)
		return X11_BAD_LENGTH;

	/* An id of the client's range not in use. */
	id = x11_read32(request + 4, client->order);
	chosen = x11_id_chosen(client, id);
	taken = x11_resource_type(server, id);
	if (!chosen || taken != 0U)
		return X11_BAD_ID_CHOICE;

	/* The font the name stands for, charged to the client. */
	error = font_resolve(server, (const char *)request + 12, name_length, index, &font);
	if (error == ENOENT)
		return X11_BAD_NAME;
	if (error != 0)
		return X11_BAD_ALLOC;

	/* Found by its id. */
	font->id = id;
	error = x11_resource_add(server, id, X11_RESOURCE_FONT, font);
	if (error != 0) {
		x11_font_free(server, font);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: the font is open. */
	return 0U;
}

/* CloseFont: the font goes (a graphics context that names it draws with the default font). */
static unsigned
font_close(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_font *font;

	/* The font of the id. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	font = x11_font_find(server, x11_read32(request + 4, server->clients[index].order));
	if (font == NULL)
		return X11_BAD_FONT;

	/* Succeeded: gone. */
	x11_resource_remove(server, font->id);
	x11_font_free(server, font);
	return 0U;
}

/*
 * QueryFont: a font's (or a graphics context's font's) metrics, bounds,
 * properties and, for a proportional font, each character's metrics.
 */
static unsigned
font_query(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_font *font;
	uint8_t *reply;
	size_t reply_length;
	size_t offset;
	size_t item;

	/* The font, or a graphics context's. */
	client = &server->clients[index];
	if (length != 8U)
		return X11_BAD_LENGTH;
	font = font_fontable(server, x11_read32(request + 4, client->order));
	if (font == NULL)
		return X11_BAD_FONT;

	/* The font's information, with room after it for each character's metrics. */
	reply_length = font_info(server, font, client, &reply, font->metric_count * 12U);
	if (reply_length == 0U)
		return X11_BAD_ALLOC;

	/* The number of the characters' metrics, and each one. */
	x11_write32(reply + 56, (uint32_t)font->metric_count, client->order);
	offset = reply_length - font->metric_count * 12U;
	for (item = 0U; item < font->metric_count; item++)
		font_write_metrics(reply + offset + item * 12U, &font->metrics[item], client->order);

	/* Succeeded: answered. */
	x11_reply(server, client, reply, reply_length);
	free(reply);
	return 0U;
}

/*
 * QueryTextExtents: the box and width of a string of two-byte characters
 * in a font (or a graphics context's font), as the protocol sums them.
 */
static unsigned
font_text_extents(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_char_metrics metrics;
	struct x11_client *client;
	struct x11_font *font;
	uint8_t reply[32];
	uint32_t character;
	size_t count;
	size_t item;
	int32_t width;
	int32_t left;
	int32_t right;
	int ascent;
	int descent;
	int present;
	int first;
	int msb;

	/* The font, and the characters (one fewer than their room when the request says the length is odd). */
	client = &server->clients[index];
	msb = client->order;
	if (length < 8U)
		return X11_BAD_LENGTH;
	count = (length - 8U) / 2U;
	if (request[1] != 0U) {
		if (count == 0U)
			return X11_BAD_LENGTH;
		count--;
	}
	font = font_fontable(server, x11_read32(request + 4, msb));
	if (font == NULL)
		return X11_BAD_FONT;

	/* Each character's place on the line: the width so far, and the ink's edges and rows. */
	width = 0;
	left = 0;
	right = 0;
	ascent = 0;
	descent = 0;
	first = 1;
	for (item = 0U; item < count; item++) {
		character = ((uint32_t)request[8U + item * 2U] << 8) | (uint32_t)request[8U + item * 2U + 1U];
		present = font_char_or_default(font, &character, &metrics);
		if (!present)
			continue;
		if (first || width + metrics.left < left)
			left = width + metrics.left;
		if (first || width + metrics.right > right)
			right = width + metrics.right;
		first = 0;
		if (metrics.ascent > ascent)
			ascent = metrics.ascent;
		if (metrics.descent > descent)
			descent = metrics.descent;
		width += metrics.width;
	}

	/* Succeeded: the font's ascent and descent and the string's. */
	memset(reply, 0, sizeof(reply));
	reply[1] = 0U;
	x11_write16(reply + 8, (uint16_t)font->ascent, msb);
	x11_write16(reply + 10, (uint16_t)font->descent, msb);
	x11_write16(reply + 12, (uint16_t)ascent, msb);
	x11_write16(reply + 14, (uint16_t)descent, msb);
	x11_write32(reply + 16, (uint32_t)width, msb);
	x11_write32(reply + 20, (uint32_t)left, msb);
	x11_write32(reply + 24, (uint32_t)right, msb);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* ListFonts: the names that match a pattern, at most as many as asked. */
static unsigned
font_list(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	const char *name;
	uint8_t *reply;
	size_t pattern_length;
	size_t name_length;
	size_t used;
	size_t offset;
	unsigned maximum;
	unsigned count;
	int matches;
	int error;

	/* The pattern, filling the request to its last word, and how many names at most. */
	client = &server->clients[index];
	if (length < 8U)
		return X11_BAD_LENGTH;
	maximum = x11_read16(request + 4, client->order);
	pattern_length = x11_read16(request + 6, client->order);
	if (8U + ((pattern_length + 3U) & ~(size_t)3U) != length)
		return X11_BAD_LENGTH;

	/* The names the server answers to. */
	error = font_names(server);
	if (error != 0)
		return X11_BAD_ALLOC;

	/* Room for every name, in the worst case. */
	reply = malloc(32U + server->font_names_size + 4U);
	if (reply == NULL)
		return X11_BAD_ALLOC;
	memset(reply, 0, 32U);

	/* Each name that matches, as its length and its bytes. */
	used = 32U;
	count = 0U;
	for (offset = 0U;
	     offset < server->font_names_size && count < maximum;
	     offset += name_length + 1U) {
		name = server->font_names + offset;
		name_length = strlen(name);
		matches = font_glob((const char *)request + 8, pattern_length, name);
		if (!matches)
			continue;
		reply[used] = (uint8_t)name_length;
		memcpy(reply + used + 1U, name, name_length);
		used += 1U + name_length;
		count++;
	}

	/* Succeeded: the names, padded to a word. */
	while ((used % 4U) != 0U)
		reply[used++] = 0U;
	x11_write16(reply + 8, (uint16_t)count, client->order);
	x11_reply(server, client, reply, used);
	free(reply);
	return 0U;
}

/*
 * ListFontsWithInfo: a reply for each name that matches a pattern (at most
 * as many as asked), with the font's information, and a last empty one.
 */
static unsigned
font_list_with_info(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_font *font;
	const char *name;
	uint8_t last[60];
	uint8_t *reply;
	size_t pattern_length;
	size_t name_length;
	size_t reply_length;
	size_t offset;
	unsigned maximum;
	unsigned count;
	unsigned left;
	int matches;
	int error;

	/* The pattern, filling the request to its last word, and how many fonts at most. */
	client = &server->clients[index];
	if (length < 8U)
		return X11_BAD_LENGTH;
	maximum = x11_read16(request + 4, client->order);
	pattern_length = x11_read16(request + 6, client->order);
	if (8U + ((pattern_length + 3U) & ~(size_t)3U) != length)
		return X11_BAD_LENGTH;

	/* The names the server answers to, and how many match (the hint each reply gives). */
	error = font_names(server);
	if (error != 0)
		return X11_BAD_ALLOC;
	left = 0U;
	for (offset = 0U; offset < server->font_names_size; offset += strlen(server->font_names + offset) + 1U) {
		matches = font_glob((const char *)request + 8, pattern_length, server->font_names + offset);
		if (matches)
			left++;
	}
	if (left > maximum)
		left = maximum;

	/* A reply for each name that matches, opened for its information. */
	count = 0U;
	for (offset = 0U;
	     offset < server->font_names_size && count < maximum;
	     offset += name_length + 1U) {
		name = server->font_names + offset;
		name_length = strlen(name);
		matches = font_glob((const char *)request + 8, pattern_length, name);
		if (!matches)
			continue;
		count++;
		left--;
		error = font_resolve(server, name, name_length, X11_NO_CLIENT, &font);
		if (error != 0)
			continue;
		reply_length = font_info(server, font, client, &reply, (name_length + 3U) & ~(size_t)3U);
		if (reply_length == 0U) {
			x11_font_free(server, font);
			return X11_BAD_ALLOC;
		}
		reply[1] = (uint8_t)name_length;
		x11_write32(reply + 56, left, client->order);
		memcpy(reply + reply_length - ((name_length + 3U) & ~(size_t)3U), name, name_length);
		x11_reply(server, client, reply, reply_length);
		free(reply);
		x11_font_free(server, font);
	}

	/* Succeeded: the last reply, which names no font. */
	memset(last, 0, sizeof(last));
	x11_reply(server, client, last, sizeof(last));
	return 0U;
}

/* SetFontPath: a list of directories, taken and not used (the fonts are the server's own). */
static unsigned
font_set_path(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	size_t offset;
	unsigned count;
	unsigned item;

	/* The number of directories. */
	if (length < 8U)
		return X11_BAD_LENGTH;
	count = x11_read16(request + 4, server->clients[index].order);

	/* Each directory, a length and its bytes, inside the request. */
	offset = 8U;
	for (item = 0U; item < count; item++) {
		if (offset >= length)
			return X11_BAD_LENGTH;
		offset += 1U + (size_t)request[offset];
		if (offset > length)
			return X11_BAD_LENGTH;
	}

	/* Succeeded: the list fills the request to its last word. */
	if (((offset + 3U) & ~(size_t)3U) != length)
		return X11_BAD_LENGTH;
	return 0U;
}

/* GetFontPath: the one directory the fonts are in, the server's own. */
static unsigned
font_get_path(
	struct x11server *server,
	unsigned index,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[32U + 12U];

	/* The header alone. */
	client = &server->clients[index];
	if (length != 4U)
		return X11_BAD_LENGTH;

	/* Succeeded: one name, as its length and its bytes. */
	memset(reply, 0, sizeof(reply));
	x11_write16(reply + 8, 1U, client->order);
	reply[32] = (uint8_t)(sizeof(FONT_PATH) - 1U);
	memcpy(reply + 33, FONT_PATH, sizeof(FONT_PATH) - 1U);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * ImageText8 and ImageText16: characters in the graphics context's font,
 * on a box of its background as tall as the font and as wide as the
 * characters' widths.  The zed-unicode font draws no box: the tree's
 * libX11 sends XDrawString as ImageText, over a background its clients
 * have painted already.
 */
static unsigned
font_image_text(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_char_metrics metrics;
	struct x11_text text;
	struct font_run run;
	struct x11_font *font;
	struct x11_gc *gc;
	uint32_t character;
	size_t count;
	size_t bytes;
	size_t item;
	unsigned code;
	int present;
	int wide;
	int width;
	int x;
	int y;
	int msb;
	int error;

	/* The characters fill the request to its last word (two bytes each for ImageText16). */
	msb = server->clients[index].order;
	if (length < 16U)
		return X11_BAD_LENGTH;
	count = request[1];
	wide = 0;
	if (request[0] == REQUEST_IMAGE_TEXT16)
		wide = 1;
	bytes = count;
	if (wide)
		bytes = count * 2U;
	if (16U + ((bytes + 3U) & ~(size_t)3U) != length)
		return X11_BAD_LENGTH;

	/* The drawable, the context and the font. */
	memset(&text, 0, sizeof(text));
	text.drawable = x11_read32(request + 4, msb);
	text.gc = x11_read32(request + 8, msb);
	gc = x11_gc_find(server, text.gc);
	if (gc == NULL) {
		code = x11_draw_text(server, &text);
		return code;
	}
	font = font_of_gc(server, gc);
	x = (int16_t)x11_read16(request + 12, msb);
	y = (int16_t)x11_read16(request + 14, msb);

	/* Each character's glyph along the line (none without a font). */
	font_run_init(&run);
	width = 0;
	if (font == NULL)
		count = 0U;
	for (item = 0U; item < count; item++) {
		character = request[16U + item];
		if (wide)
			character = ((uint32_t)request[16U + item * 2U] << 8) | (uint32_t)request[16U + item * 2U + 1U];
		present = font_char_or_default(font, &character, &metrics);
		if (!present)
			continue;
		error = font_run_add(server, &run, font, character, x + width, y);
		if (error != 0) {
			font_run_fini(&run);
			return X11_BAD_ALLOC;
		}
		width += metrics.width;
	}

	/* The background's box: as wide as the characters, as tall as the font. */
	text.image = 1;
	if (font == NULL || font->keeps_background)
		text.image = 0;
	text.background.x0 = x;
	text.background.x1 = x + width;
	if (width < 0) {
		text.background.x0 = x + width;
		text.background.x1 = x;
	}
	text.background.y0 = y;
	text.background.y1 = y;
	if (font != NULL) {
		text.background.y0 = y - font->ascent;
		text.background.y1 = y + font->descent;
	}

	/* The box all of it covers. */
	text.extent = run.extent;
	if (text.image) {
		if (run.count == 0U || text.background.x0 < text.extent.x0)
			text.extent.x0 = text.background.x0;
		if (run.count == 0U || text.background.y0 < text.extent.y0)
			text.extent.y0 = text.background.y0;
		if (run.count == 0U || text.background.x1 > text.extent.x1)
			text.extent.x1 = text.background.x1;
		if (run.count == 0U || text.background.y1 > text.extent.y1)
			text.extent.y1 = text.background.y1;
	}

	/* Drawn. */
	text.glyphs = run.glyphs;
	text.count = run.count;
	text.bits = run.bits;
	code = x11_draw_text(server, &text);
	font_run_fini(&run);

	/* Reports a drawable or context that cannot be drawn on. */
	if (code != 0U)
		return code;

	/* Succeeded: drawn. */
	return 0U;
}

/*
 * PolyText8 and PolyText16: items of characters, each moving the pen
 * first, and items that change the graphics context's font, drawn as the
 * context fills.  The items before one that names no font are drawn.
 */
static unsigned
font_poly_text(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_char_metrics metrics;
	struct x11_text text;
	struct font_run run;
	struct x11_font *font;
	struct x11_gc *gc;
	uint32_t character;
	uint32_t font_id;
	size_t offset;
	size_t size;
	size_t count;
	size_t item;
	unsigned code;
	int present;
	int wide;
	int x;
	int y;
	int msb;
	int error;

	/* The fixed part, and the drawable and the context, checked before any item. */
	msb = server->clients[index].order;
	if (length < 16U)
		return X11_BAD_LENGTH;
	memset(&text, 0, sizeof(text));
	text.drawable = x11_read32(request + 4, msb);
	text.gc = x11_read32(request + 8, msb);
	code = x11_draw_text(server, &text);
	if (code != 0U)
		return code;
	gc = x11_gc_find(server, text.gc);
	if (gc == NULL)
		return X11_BAD_GC;
	wide = 0;
	if (request[0] == REQUEST_POLY_TEXT16)
		wide = 1;
	x = (int16_t)x11_read16(request + 12, msb);
	y = (int16_t)x11_read16(request + 14, msb);

	/* Each item while more than its header is left (what is left after them is padding). */
	font_run_init(&run);
	code = 0U;
	offset = 16U;
	while (length - offset > 2U) {
		/* A change of font: the font's id, most significant byte first. */
		if (request[offset] == 255U) {
			if (length - offset < 5U) {
				code = X11_BAD_LENGTH;
				break;
			}
			font_id = ((uint32_t)request[offset + 1U] << 24) | ((uint32_t)request[offset + 2U] << 16) |
			    ((uint32_t)request[offset + 3U] << 8) | (uint32_t)request[offset + 4U];
			font = x11_font_find(server, font_id);
			if (font == NULL) {
				code = X11_BAD_FONT;
				break;
			}
			gc->font = font_id;
			offset += 5U;
			continue;
		}

		/* A string: its length, the pen's move, and its characters. */
		count = request[offset];
		size = count;
		if (wide)
			size = count * 2U;
		if (length - offset - 2U < size) {
			code = X11_BAD_LENGTH;
			break;
		}
		x += (int8_t)request[offset + 1U];
		font = font_of_gc(server, gc);
		for (item = 0U; item < count && font != NULL; item++) {
			character = request[offset + 2U + item];
			if (wide)
				character = ((uint32_t)request[offset + 2U + item * 2U] << 8) | (uint32_t)request[offset + 3U + item * 2U];
			present = font_char_or_default(font, &character, &metrics);
			if (!present)
				continue;

			/* Its glyph, the glyphs gathered drawn when there are enough. */
			error = font_run_add(server, &run, font, character, x, y);
			if (error != 0) {
				code = X11_BAD_ALLOC;
				break;
			}
			x += metrics.width;
			if (run.count >= FONT_RUN_GLYPHS)
				(void)font_run_draw(server, &run, text.drawable, text.gc);
		}
		if (code != 0U)
			break;
		offset += 2U + size;
	}

	/* The glyphs left, and what stopped the items. */
	(void)font_run_draw(server, &run, text.drawable, text.gc);
	font_run_fini(&run);
	if (code != 0U)
		return code;

	/* Succeeded: drawn. */
	return 0U;
}

/* Finds a font by its id, or a graphics context's font by the context's (NULL: neither). */
static struct x11_font *
font_fontable(
	struct x11server *server,
	uint32_t id)
{
	struct x11_font *font;
	struct x11_gc *gc;

	/* A font. */
	font = x11_font_find(server, id);
	if (font != NULL)
		return font;

	/* A graphics context's font. */
	gc = x11_gc_find(server, id);
	if (gc == NULL)
		return NULL;
	font = font_of_gc(server, gc);
	if (font == NULL)
		return NULL;

	/* Succeeded: the context's font. */
	return font;
}

/*
 * Finds the font a graphics context draws with: its own while open, else
 * the server's default font, opened when first needed.  Returns NULL when
 * the default font cannot be made.
 */
static struct x11_font *
font_of_gc(
	struct x11server *server,
	const struct x11_gc *gc)
{
	struct x11_font *font;
	int error;

	/* The context's own font. */
	if (gc->font != 0U) {
		font = x11_font_find(server, gc->font);
		if (font != NULL)
			return font;
	}

	/* The default font, made once. */
	if (server->default_font == NULL) {
		error = font_resolve(server, FONT_DEFAULT_NAME, sizeof(FONT_DEFAULT_NAME) - 1U, X11_NO_CLIENT, &font);
		if (error != 0)
			return NULL;
		server->default_font = font;
	}

	/* Succeeded: the default font. */
	return server->default_font;
}

/*
 * Makes the font a name stands for, charged to an owner: an alias's
 * font, the special fonts, the first listed name that matches, or a
 * scalable name at the size the name gives.  Returns 0, ENOENT for a
 * name that matches no font, or ENOMEM.
 */
static int
font_resolve(
	struct x11server *server,
	const char *name,
	size_t length,
	unsigned owner,
	struct x11_font **result)
{
	struct font_spec spec;
	const char *pattern;
	const char *listed;
	size_t pattern_length;
	size_t offset;
	size_t index;
	int matches;
	int error;

	/* An alias stands for its name. */
	pattern = name;
	pattern_length = length;
	for (index = 0U; index < sizeof(font_aliases) / sizeof(font_aliases[0]); index++) {
		matches = font_field_is(name, length, font_aliases[index].alias);
		if (matches) {
			pattern = font_aliases[index].name;
			pattern_length = strlen(pattern);
			break;
		}
	}

	/* The special fonts by their names. */
	matches = font_field_is(pattern, pattern_length, FONT_CURSOR_NAME);
	if (matches) {
		error = font_make_special(server, FONT_CURSOR_NAME, owner, result);
		return error;
	}
	matches = font_field_is(pattern, pattern_length, FONT_ZED_NAME);
	if (matches) {
		error = font_make_special(server, FONT_ZED_NAME, owner, result);
		return error;
	}

	/* The first of the listed names (not the aliases' or special ones) the pattern matches. */
	error = font_names(server);
	if (error != 0)
		return error;
	for (offset = 0U; offset < server->font_names_size; offset += strlen(listed) + 1U) {
		listed = server->font_names + offset;
		if (listed[0] != '-')
			continue;
		matches = font_glob(pattern, pattern_length, listed);
		if (!matches)
			continue;
		error = font_parse(listed, &spec);
		if (error != 0)
			continue;
		error = font_make(server, &spec, owner, result);
		return error;
	}

	/* A scalable name at the size the pattern gives. */
	error = font_scalable(server, pattern, pattern_length, owner, result);
	if (error != 0)
		return error;

	/* Succeeded: the font. */
	return 0;
}

/*
 * Makes the names ListFonts answers, once: the aliases, the special fonts,
 * each family's listed sizes and its scalable names.  Returns 0, or
 * ENOMEM.
 */
static int
font_names(
	struct x11server *server)
{
	const struct font_cell *cell;
	struct font_spec spec;
	char name[X11_FONT_NAME_MAX + 1U];
	char *names;
	size_t size;
	size_t capacity;
	size_t index;
	int bold;
	int wide;
	int error;

	/* Made already. */
	if (server->font_names != NULL)
		return 0;

	/* The aliases and the special fonts. */
	names = NULL;
	size = 0U;
	capacity = 0U;
	error = 0;
	for (index = 0U; index < sizeof(font_aliases) / sizeof(font_aliases[0]) && error == 0; index++)
		error = font_names_add(&names, &size, &capacity, font_aliases[index].alias);
	if (error == 0)
		error = font_names_add(&names, &size, &capacity, FONT_CURSOR_NAME);
	if (error == 0)
		error = font_names_add(&names, &size, &capacity, FONT_ZED_NAME);

	/* misc-fixed in each of X's cells, both weights, both character sets. */
	for (wide = 0; wide <= 1 && error == 0; wide++) {
		for (index = 0U; index < sizeof(font_cells) / sizeof(font_cells[0]) && error == 0; index++) {
			cell = &font_cells[index];
			for (bold = 0; bold <= 1 && error == 0; bold++) {
				memset(&spec, 0, sizeof(spec));
				spec.family = &font_families[0];
				spec.bold = bold;
				spec.slant = 'r';
				(void)snprintf(spec.setwidth, sizeof(spec.setwidth), "%s", cell->setwidth);
				spec.pixel = (unsigned)cell->height;
				spec.point = cell->point;
				spec.resolution_x = FONT_RESOLUTION;
				spec.resolution_y = FONT_RESOLUTION;
				spec.average = (unsigned)cell->width * 10U;
				spec.wide = wide;
				font_name(&spec, name, sizeof(name));
				error = font_names_add(&names, &size, &capacity, name);
			}
		}
	}

	/* Each family's listed sizes and scalable names. */
	for (wide = 0; wide <= 1 && error == 0; wide++) {
		for (index = 0U; index < sizeof(font_families) / sizeof(font_families[0]) && error == 0; index++)
			error = font_names_family(server, &names, &size, &capacity, &font_families[index], wide);
	}

	/* Refuses the list without memory for it. */
	if (error != 0) {
		free(names);
		return error;
	}

	/* Succeeded: kept for the server's life. */
	server->font_names = names;
	server->font_names_size = size;
	return 0;
}

/* Appends a name (and the NUL after it) to the list of names. */
static int
font_names_add(
	char **names,
	size_t *size,
	size_t *capacity,
	const char *name)
{
	char *grown;
	size_t length;
	size_t wanted;

	/* Room for the name, the list doubled when it is full. */
	length = strlen(name);
	if (*size + length + 1U > *capacity) {
		wanted = *capacity * 2U;
		if (wanted < 4096U)
			wanted = 4096U;
		while (wanted < *size + length + 1U)
			wanted *= 2U;
		grown = realloc(*names, wanted);
		if (grown == NULL)
			return ENOMEM;
		*names = grown;
		*capacity = wanted;
	}

	/* Succeeded: the name, after the others. */
	memcpy(*names + *size, name, length + 1U);
	*size += length + 1U;
	return 0;
}

/*
 * Appends a family's names: its listed sizes (but misc-fixed's, which are
 * X's cells) and its scalable names, both weights, in one character set.
 */
static int
font_names_family(
	struct x11server *server,
	char **names,
	size_t *size,
	size_t *capacity,
	const struct font_family *family,
	int wide)
{
	struct x11_font *font;
	struct font_spec spec;
	char name[X11_FONT_NAME_MAX + 1U];
	size_t index;
	int bold;
	int error;

	/* Each weight. */
	for (bold = 0; bold <= 1; bold++) {
		/* The scalable name (and for misc-fixed, its semicondensed one too). */
		memset(&spec, 0, sizeof(spec));
		spec.family = family;
		spec.bold = bold;
		spec.slant = 'r';
		(void)snprintf(spec.setwidth, sizeof(spec.setwidth), "normal");
		spec.resolution_x = FONT_RESOLUTION;
		spec.resolution_y = FONT_RESOLUTION;
		spec.wide = wide;
		font_name(&spec, name, sizeof(name));
		error = font_names_add(names, size, capacity, name);
		if (error != 0)
			return error;
		if (family == &font_families[0]) {
			(void)snprintf(spec.setwidth, sizeof(spec.setwidth), "semicondensed");
			font_name(&spec, name, sizeof(name));
			error = font_names_add(names, size, capacity, name);
			if (error != 0)
				return error;
			continue;
		}

		/* The listed sizes, each named with the width the font has at it. */
		for (index = 0U; index < sizeof(font_sizes) / sizeof(font_sizes[0]); index++) {
			spec.pixel = font_sizes[index];
			spec.point = font_sizes[index] * 10U;
			spec.average = 0U;
			error = font_make(server, &spec, X11_NO_CLIENT, &font);
			if (error != 0)
				return error;
			error = font_names_add(names, size, capacity, font->name);
			x11_font_free(server, font);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the family's names. */
	return 0;
}

/*
 * Tells whether a name matches a pattern (1, or 0): letters of either
 * case match, ? matches any one character and * any run of them.
 */
static int
font_glob(
	const char *pattern,
	size_t pattern_length,
	const char *name)
{
	size_t at;
	size_t place;
	size_t star;
	size_t star_place;
	int starred;
	char wanted;
	char found;

	/* Each character of the name against the pattern, going back to the last * on a mismatch. */
	at = 0U;
	place = 0U;
	star = 0U;
	star_place = 0U;
	starred = 0;
	while (name[place] != '\0') {
		if (at < pattern_length && pattern[at] == '*') {
			starred = 1;
			star = at;
			star_place = place;
			at++;
			continue;
		}
		if (at < pattern_length) {
			wanted = pattern[at];
			found = name[place];
			if (wanted >= 'A' && wanted <= 'Z')
				wanted = (char)(wanted - 'A' + 'a');
			if (found >= 'A' && found <= 'Z')
				found = (char)(found - 'A' + 'a');
			if (wanted == '?' || wanted == found) {
				at++;
				place++;
				continue;
			}
		}
		if (!starred)
			return 0;
		at = star + 1U;
		star_place++;
		place = star_place;
	}

	/* What is left of the pattern must be stars. */
	while (at < pattern_length && pattern[at] == '*')
		at++;
	if (at != pattern_length)
		return 0;

	/* Succeeded: it matches. */
	return 1;
}

/*
 * Splits an XLFD name (or pattern) into its fourteen fields.  Returns 0,
 * or -1 for a name of another shape.
 */
static int
font_split(
	const char *name,
	const char **fields,
	size_t *lengths)
{
	unsigned field;
	const char *start;
	const char *place;

	/* A name starts with its first field's dash. */
	if (name[0] != '-')
		return -1;

	/* Each field up to the next dash or the end. */
	start = name + 1;
	field = 0U;
	for (place = start; ; place++) {
		if (*place != '-' && *place != '\0')
			continue;
		if (field >= FONT_FIELDS)
			return -1;
		fields[field] = start;
		lengths[field] = (size_t)(place - start);
		field++;
		if (*place == '\0')
			break;
		start = place + 1;
	}

	/* Succeeded only with all fourteen. */
	if (field != FONT_FIELDS)
		return -1;
	return 0;
}

/*
 * Reads a concrete XLFD name of one of the families.  Returns 0, or -1
 * for a name that is none of them.
 */
static int
font_parse(
	const char *name,
	struct font_spec *spec)
{
	const char *fields[FONT_FIELDS];
	size_t lengths[FONT_FIELDS];
	size_t index;
	int same;
	int error;

	/* The fields. */
	memset(spec, 0, sizeof(*spec));
	error = font_split(name, fields, lengths);
	if (error != 0)
		return -1;

	/* The family, by its foundry and name. */
	for (index = 0U; index < sizeof(font_families) / sizeof(font_families[0]); index++) {
		same = font_field_is(fields[FIELD_FOUNDRY], lengths[FIELD_FOUNDRY], font_families[index].foundry);
		if (!same)
			continue;
		same = font_field_is(fields[FIELD_FAMILY], lengths[FIELD_FAMILY], font_families[index].family);
		if (same)
			break;
	}
	if (index == sizeof(font_families) / sizeof(font_families[0]))
		return -1;
	spec->family = &font_families[index];

	/* The weight: medium or bold. */
	same = font_field_is(fields[FIELD_WEIGHT], lengths[FIELD_WEIGHT], "bold");
	if (same) {
		spec->bold = 1;
	} else {
		same = font_field_is(fields[FIELD_WEIGHT], lengths[FIELD_WEIGHT], "medium");
		if (!same)
			return -1;
	}

	/* The slant: upright, oblique or italic (one letter). */
	if (lengths[FIELD_SLANT] != 1U)
		return -1;
	spec->slant = fields[FIELD_SLANT][0];
	if (spec->slant >= 'A' && spec->slant <= 'Z')
		spec->slant = (char)(spec->slant - 'A' + 'a');
	if (spec->slant != 'r' && spec->slant != 'o' && spec->slant != 'i')
		return -1;

	/* The setwidth: normal, or semicondensed. */
	same = font_field_is(fields[FIELD_SETWIDTH], lengths[FIELD_SETWIDTH], "normal");
	if (!same) {
		same = font_field_is(fields[FIELD_SETWIDTH], lengths[FIELD_SETWIDTH], "semicondensed");
		if (!same)
			return -1;
	}
	if (lengths[FIELD_SETWIDTH] >= sizeof(spec->setwidth) || lengths[FIELD_ADD_STYLE] != 0U)
		return -1;
	memcpy(spec->setwidth, fields[FIELD_SETWIDTH], lengths[FIELD_SETWIDTH]);
	spec->setwidth[lengths[FIELD_SETWIDTH]] = '\0';

	/* The sizes, the resolution and the average width (numbers; 0 in a scalable name). */
	error = font_number(fields[FIELD_PIXEL], lengths[FIELD_PIXEL], &spec->pixel);
	if (error != 0)
		return -1;
	error = font_number(fields[FIELD_POINT], lengths[FIELD_POINT], &spec->point);
	if (error != 0)
		return -1;
	error = font_number(fields[FIELD_RESOLUTION_X], lengths[FIELD_RESOLUTION_X], &spec->resolution_x);
	if (error != 0)
		return -1;
	error = font_number(fields[FIELD_RESOLUTION_Y], lengths[FIELD_RESOLUTION_Y], &spec->resolution_y);
	if (error != 0)
		return -1;
	error = font_number(fields[FIELD_AVERAGE], lengths[FIELD_AVERAGE], &spec->average);
	if (error != 0)
		return -1;

	/* The family's spacing. */
	if (lengths[FIELD_SPACING] != 1U)
		return -1;
	if (fields[FIELD_SPACING][0] != spec->family->spacing && fields[FIELD_SPACING][0] != spec->family->spacing - 'a' + 'A')
		return -1;

	/* The character set: ISO 8859-1 or ISO 10646-1. */
	same = font_field_is(fields[FIELD_ENCODING], lengths[FIELD_ENCODING], "1");
	if (!same)
		return -1;
	same = font_field_is(fields[FIELD_REGISTRY], lengths[FIELD_REGISTRY], "iso10646");
	if (same) {
		spec->wide = 1;
		return 0;
	}
	same = font_field_is(fields[FIELD_REGISTRY], lengths[FIELD_REGISTRY], "iso8859");
	if (!same)
		return -1;

	/* Succeeded: one of the families. */
	return 0;
}

/* Reads a field of decimal digits.  Returns 0, or -1 for another field or one too large. */
static int
font_number(
	const char *field,
	size_t length,
	unsigned *value)
{
	size_t index;

	/* At least one digit, and few enough to fit. */
	if (length == 0U || length > 6U)
		return -1;

	/* Each digit. */
	*value = 0U;
	for (index = 0U; index < length; index++) {
		if (field[index] < '0' || field[index] > '9')
			return -1;
		*value = *value * 10U + (unsigned)(field[index] - '0');
	}

	/* Succeeded: the number. */
	return 0;
}

/* Tells whether a field is a word, letters of either case alike (1, or 0). */
static int
font_field_is(
	const char *field,
	size_t length,
	const char *word)
{
	size_t index;
	char letter;

	/* As long as the word. */
	if (strlen(word) != length)
		return 0;

	/* Each letter. */
	for (index = 0U; index < length; index++) {
		letter = field[index];
		if (letter >= 'A' && letter <= 'Z')
			letter = (char)(letter - 'A' + 'a');
		if (letter != word[index])
			return 0;
	}

	/* Succeeded: the same word. */
	return 1;
}

/* Writes the XLFD name of what a font name asks for. */
static void
font_name(
	const struct font_spec *spec,
	char *name,
	size_t size)
{
	const char *weight;
	const char *registry;

	/* The words of the weight and the character set. */
	weight = "medium";
	if (spec->bold)
		weight = "bold";
	registry = "iso8859";
	if (spec->wide)
		registry = "iso10646";

	/* The fourteen fields. */
	(void)snprintf(
		name,
		size,
		"-%s-%s-%s-%c-%s--%u-%u-%u-%u-%c-%u-%s-1",
		spec->family->foundry,
		spec->family->family,
		weight,
		spec->slant,
		spec->setwidth,
		spec->pixel,
		spec->point,
		spec->resolution_x,
		spec->resolution_y,
		spec->family->spacing,
		spec->average,
		registry);
}

/*
 * Makes a font through a scalable name: the first family's scalable name
 * the pattern matches once the pattern's sizes, resolution, slant and
 * average width are put in it.  Returns 0, ENOENT when none matches (or
 * the pattern gives no size), or ENOMEM.
 */
static int
font_scalable(
	struct x11server *server,
	const char *pattern,
	size_t length,
	unsigned owner,
	struct x11_font **result)
{
	const char *fields[FONT_FIELDS];
	size_t lengths[FONT_FIELDS];
	char copy[X11_FONT_NAME_MAX + 1U];
	struct font_spec spec;
	struct x11_font *font;
	const char *listed;
	size_t offset;
	unsigned pixel;
	unsigned point;
	unsigned resolution_x;
	unsigned resolution_y;
	unsigned average;
	int pixel_error;
	int point_error;
	int matches;
	int error;

	/* The pattern's fields (a pattern of another shape names no scalable font). */
	if (length > X11_FONT_NAME_MAX)
		return ENOENT;
	memcpy(copy, pattern, length);
	copy[length] = '\0';
	error = font_split(copy, fields, lengths);
	if (error != 0)
		return ENOENT;

	/* The sizes it gives: the pixel size, else the point size at the resolution. */
	resolution_x = FONT_RESOLUTION;
	resolution_y = FONT_RESOLUTION;
	(void)font_number(fields[FIELD_RESOLUTION_X], lengths[FIELD_RESOLUTION_X], &resolution_x);
	(void)font_number(fields[FIELD_RESOLUTION_Y], lengths[FIELD_RESOLUTION_Y], &resolution_y);
	if (resolution_x == 0U || resolution_y == 0U)
		return ENOENT;
	pixel_error = font_number(fields[FIELD_PIXEL], lengths[FIELD_PIXEL], &pixel);
	point_error = font_number(fields[FIELD_POINT], lengths[FIELD_POINT], &point);
	if (pixel_error != 0 && point_error != 0)
		return ENOENT;
	if (pixel_error != 0)
		pixel = (point * resolution_y + 360U) / 720U;
	if (point_error != 0)
		point = (pixel * 720U + resolution_y / 2U) / resolution_y;
	if (pixel == 0U || pixel > X11_FONT_PIXELS_MAX)
		return ENOENT;
	average = 0U;
	(void)font_number(fields[FIELD_AVERAGE], lengths[FIELD_AVERAGE], &average);

	/* Each scalable name with those put in it, until one matches. */
	for (offset = 0U; offset < server->font_names_size; offset += strlen(listed) + 1U) {
		listed = server->font_names + offset;
		error = font_parse(listed, &spec);
		if (error != 0 || spec.pixel != 0U)
			continue;
		spec.pixel = pixel;
		spec.point = point;
		spec.resolution_x = resolution_x;
		spec.resolution_y = resolution_y;
		spec.average = average;
		if (lengths[FIELD_SLANT] == 1U)
			spec.slant = fields[FIELD_SLANT][0];
		if (spec.slant != 'o' && spec.slant != 'i')
			spec.slant = 'r';
		error = font_make(server, &spec, owner, &font);
		if (error == ENOENT)
			continue;
		if (error != 0)
			return error;
		matches = font_glob(pattern, length, font->name);
		if (matches) {
			*result = font;
			return 0;
		}
		x11_font_free(server, font);
	}

	/* No scalable name matches. */
	return ENOENT;
}

/*
 * Makes a text font of a family at a size, its name the XLFD of what was
 * asked with the average width it has.  Returns 0, ENOENT for a size or
 * width the server does not make, or ENOMEM.
 */
static int
font_make(
	struct x11server *server,
	struct font_spec *spec,
	unsigned owner,
	struct x11_font **result)
{
	struct x11_font *font;
	int error;

	/* A size the server makes (a scalable name opens at the default size). */
	if (spec->pixel == 0U) {
		spec->pixel = FONT_PIXELS_DEFAULT;
		spec->point = (FONT_PIXELS_DEFAULT * 720U + spec->resolution_y / 2U) / spec->resolution_y;
	}
	if (spec->pixel > X11_FONT_PIXELS_MAX || spec->average > X11_FONT_PIXELS_MAX * 20U)
		return ENOENT;

	/* The font of its face's role and character set. */
	font = calloc(1U, sizeof(*font));
	if (font == NULL)
		return ENOMEM;
	font->owner = owner;
	font->kind = X11_FONT_TEXT;
	font->role = spec->family->role;
	if (spec->bold)
		font->role++;
	font->min_byte1 = 0U;
	font->max_byte1 = 0U;
	font->min_byte2 = 0U;
	font->max_byte2 = 255U;
	if (spec->wide)
		font->max_byte1 = 255U;

	/* Its metrics: one cell of the family's, or each character's of the face. */
	if (spec->family->spacing != 'p') {
		font_make_mono(server, font, spec);
	} else {
		error = font_make_proportional(server, font, spec);
		if (error != 0) {
			x11_font_free(server, font);
			return error;
		}
	}

	/* Its name, with the width it has when none was asked. */
	font_name(spec, font->name, sizeof(font->name));

	/* Its charge. */
	error = font_charge(server, font);
	if (error != 0) {
		font->charged = 0U;
		x11_font_free(server, font);
		return error;
	}

	/* Succeeded: the font. */
	*result = font;
	return 0;
}

/* Makes the cursor font or zed-unicode, charged to an owner.  Returns 0, or ENOMEM. */
static int
font_make_special(
	struct x11server *server,
	const char *name,
	unsigned owner,
	struct x11_font **result)
{
	struct x11_font *font;
	int same;
	int error;

	/* The font, named as it was opened. */
	font = calloc(1U, sizeof(*font));
	if (font == NULL)
		return ENOMEM;
	font->owner = owner;
	(void)snprintf(font->name, sizeof(font->name), "%s", name);

	/* The cursor font: the standard cursors' numbers, one cell each. */
	same = strcmp(name, FONT_CURSOR_NAME) == 0;
	if (same) {
		font->kind = X11_FONT_CURSOR;
		font->max_byte2 = (uint8_t)(FONT_CURSOR_CHARS - 1U);
		font->cell_width = FONT_CURSOR_CELL;
		font->ascent = FONT_CURSOR_CELL;
		font->descent = 0;
	} else {
		/*
		 * zed-unicode: the tree's 8x16 cells over the whole of Unicode's
		 * first plane.  ImageText with it leaves the background as it is:
		 * the tree's libX11 sends XDrawString as ImageText (not as
		 * PolyText, as Xlib does), and its clients paint each cell's
		 * background before they draw the characters over it.
		 */
		font->kind = X11_FONT_TEXT;
		font->role = X11_FACE_MONO;
		font->pixels = FONT_ZED_PIXELS;
		font->raise = FONT_ZED_RAISE;
		font->max_byte1 = 255U;
		font->max_byte2 = 255U;
		font->cell_width = FONT_ZED_WIDTH;
		font->ascent = FONT_ZED_HEIGHT;
		font->descent = 0;
		font->keeps_background = 1;
	}

	/* Every character's metrics: the cell. */
	font->min_bounds.left = 0;
	font->min_bounds.right = (int16_t)font->cell_width;
	font->min_bounds.width = (int16_t)font->cell_width;
	font->min_bounds.ascent = (int16_t)font->ascent;
	font->min_bounds.descent = (int16_t)font->descent;
	font->max_bounds = font->min_bounds;

	/* Its charge. */
	error = font_charge(server, font);
	if (error != 0) {
		font->charged = 0U;
		x11_font_free(server, font);
		return error;
	}

	/* Succeeded: the font. */
	*result = font;
	return 0;
}

/*
 * Makes a monospaced font's cell: X's cell of that name when there is
 * one, else the size's ascent and descent and the width asked or the
 * face's own; the face is drawn at the largest size that fits the cell.
 */
static void
font_make_mono(
	struct x11server *server,
	struct x11_font *font,
	struct font_spec *spec)
{
	const struct font_cell *cell;
	unsigned pixels;
	int has_face;
	int ascent;
	int descent;
	int advance;
	int width;
	int error;

	/* The cell's height and baseline: X's, or a fifth of the size below the baseline. */
	cell = font_cell_of(spec);
	if (cell != NULL) {
		font->ascent = cell->ascent;
		font->descent = cell->descent;
	} else {
		font->descent = (int)spec->pixel / 5;
		font->ascent = (int)spec->pixel - font->descent;
	}

	/* The width: the one asked, else the face's own at the cell's height (three fifths of it without a face). */
	width = ((int)spec->average + 5) / 10;
	has_face = x11_face_present(&server->faces, font->role);
	if (width == 0) {
		width = ((int)spec->pixel * 6 + 5) / 10;
		for (pixels = spec->pixel + 2U; pixels >= 1U && has_face; pixels--) {
			error = x11_face_line(&server->faces, font->role, pixels, &ascent, &descent, &advance);
			if (error != 0)
				break;
			if (ascent <= font->ascent + 1 && descent <= font->descent + 1) {
				width = advance;
				break;
			}
		}
	}
	if (width < 1)
		width = 1;
	font->cell_width = width;
	if (spec->average == 0U)
		spec->average = (unsigned)width * 10U;

	/* The size the face is drawn at: the largest that fits the cell, centred across it. */
	font->pixels = spec->pixel;
	font->shift_x = 0;
	for (pixels = spec->pixel + 2U; pixels >= 1U && has_face; pixels--) {
		error = x11_face_line(&server->faces, font->role, pixels, &ascent, &descent, &advance);
		if (error != 0)
			break;
		if (ascent > font->ascent + 1 || descent > font->descent + 1 || advance > width)
			continue;
		font->pixels = pixels;
		font->shift_x = (width - advance) / 2;
		break;
	}

	/* Every character's metrics: the cell; the line-drawing characters drawn across it. */
	font->min_bounds.left = 0;
	font->min_bounds.right = (int16_t)width;
	font->min_bounds.width = (int16_t)width;
	font->min_bounds.ascent = (int16_t)font->ascent;
	font->min_bounds.descent = (int16_t)font->descent;
	font->max_bounds = font->min_bounds;
	font->line_drawing = 1;
}

/*
 * Makes a proportional font's metrics, each character's as the face has
 * it at the size, and the average width its name gives when none was
 * asked (without a face, the cell of a monospaced font three fifths of
 * the size wide).  Returns 0, or ENOMEM.
 */
static int
font_make_proportional(
	struct x11server *server,
	struct x11_font *font,
	struct font_spec *spec)
{
	struct x11_char_metrics *metrics;
	unsigned long total;
	unsigned present;
	uint32_t character;
	size_t index;
	int has_face;
	int advance;
	int error;

	/* The face at the size, its line's ascent and descent. */
	font->pixels = spec->pixel;
	has_face = x11_face_present(&server->faces, font->role);
	error = ENOENT;
	if (has_face)
		error = x11_face_line(&server->faces, font->role, spec->pixel, &font->ascent, &font->descent, &advance);

	/* Without a face: one cell for all. */
	if (error != 0) {
		font->descent = (int)spec->pixel / 5;
		font->ascent = (int)spec->pixel - font->descent;
		font->cell_width = ((int)spec->pixel * 6 + 5) / 10;
		font->min_bounds.right = (int16_t)font->cell_width;
		font->min_bounds.width = (int16_t)font->cell_width;
		font->min_bounds.ascent = (int16_t)font->ascent;
		font->min_bounds.descent = (int16_t)font->descent;
		font->max_bounds = font->min_bounds;
		if (spec->average == 0U)
			spec->average = (unsigned)font->cell_width * 10U;
		return 0;
	}

	/* Each character's metrics: the first byte's rows Latin, Greek and Cyrillic take in ISO 10646. */
	if (font->max_byte1 > 0U)
		font->max_byte1 = (uint8_t)(FONT_WIDE_ROWS - 1U);
	font->metric_count = ((size_t)font->max_byte1 + 1U) * 256U;
	metrics = calloc(font->metric_count, sizeof(*metrics));
	if (metrics == NULL)
		return ENOMEM;
	font->metrics = metrics;

	/* Each one the face has, but the control characters, which a font has none of. */
	total = 0UL;
	present = 0U;
	for (index = 0U; index < font->metric_count; index++) {
		character = (uint32_t)index;
		if (character < 0x20U || (character >= 0x7fU && character < 0xa0U))
			continue;
		(void)x11_face_char(&server->faces, font->role, font->pixels, character, &metrics[index]);
		if (metrics[index].width == 0 && metrics[index].right == metrics[index].left)
			continue;

		/* The bounds of them all. */
		if (present == 0U) {
			font->min_bounds = metrics[index];
			font->max_bounds = metrics[index];
		}
		if (metrics[index].left < font->min_bounds.left)
			font->min_bounds.left = metrics[index].left;
		if (metrics[index].right < font->min_bounds.right)
			font->min_bounds.right = metrics[index].right;
		if (metrics[index].width < font->min_bounds.width)
			font->min_bounds.width = metrics[index].width;
		if (metrics[index].ascent < font->min_bounds.ascent)
			font->min_bounds.ascent = metrics[index].ascent;
		if (metrics[index].descent < font->min_bounds.descent)
			font->min_bounds.descent = metrics[index].descent;
		if (metrics[index].left > font->max_bounds.left)
			font->max_bounds.left = metrics[index].left;
		if (metrics[index].right > font->max_bounds.right)
			font->max_bounds.right = metrics[index].right;
		if (metrics[index].width > font->max_bounds.width)
			font->max_bounds.width = metrics[index].width;
		if (metrics[index].ascent > font->max_bounds.ascent)
			font->max_bounds.ascent = metrics[index].ascent;
		if (metrics[index].descent > font->max_bounds.descent)
			font->max_bounds.descent = metrics[index].descent;
		total += (unsigned long)metrics[index].width;
		present++;
	}

	/* The default character, the space; the average width the name gives. */
	font->default_char = 0x20U;
	if (spec->average == 0U && present != 0U)
		spec->average = (unsigned)((total * 10UL + present / 2U) / present);

	/* Succeeded: measured. */
	return 0;
}

/*
 * Charges a font's owner with the font, its metrics and the most its kept
 * glyphs can take.  Returns 0, or ENOMEM.
 */
static int
font_charge(
	struct x11server *server,
	struct x11_font *font)
{
	size_t glyph;
	size_t width;
	size_t height;
	int error;

	/* The largest glyph: the bounds' box, one bit a pixel. */
	width = 0U;
	height = 0U;
	if (font->max_bounds.right > font->min_bounds.left)
		width = (size_t)(font->max_bounds.right - font->min_bounds.left);
	if (font->max_bounds.ascent + font->max_bounds.descent > 0)
		height = (size_t)(font->max_bounds.ascent + font->max_bounds.descent);
	glyph = (width + 7U) / 8U * height;

	/* The font, its metrics and its glyphs. */
	font->charged = sizeof(*font) + font->metric_count * sizeof(*font->metrics) + glyph * X11_FONT_CACHE;
	error = x11_memory_charge(server, font->owner, font->charged);
	if (error != 0)
		return error;

	/* Succeeded: charged. */
	return 0;
}

/* Finds X's cell a misc-fixed name names (its height, and its width when it gives one), or NULL. */
static const struct font_cell *
font_cell_of(
	const struct font_spec *spec)
{
	size_t index;

	/* Only misc-fixed has X's cells. */
	if (spec->family != &font_families[0])
		return NULL;

	/* The first cell of the height (and width). */
	for (index = 0U; index < sizeof(font_cells) / sizeof(font_cells[0]); index++) {
		if (font_cells[index].height != (int)spec->pixel)
			continue;
		if (spec->average != 0U && (unsigned)font_cells[index].width * 10U != spec->average)
			continue;
		return &font_cells[index];
	}

	/* None of that size. */
	return NULL;
}

/*
 * Gives a character's metrics (the first byte in its high byte).  Returns
 * 1, or 0 for a character the font does not have.
 */
static int
font_char(
	const struct x11_font *font,
	uint32_t character,
	struct x11_char_metrics *metrics)
{
	unsigned byte1;
	unsigned byte2;
	size_t index;

	/* Inside the font's rows and columns. */
	memset(metrics, 0, sizeof(*metrics));
	byte1 = (character >> 8) & 0xffU;
	byte2 = character & 0xffU;
	if (character > 0xffffU)
		return 0;
	if (byte1 < font->min_byte1 || byte1 > font->max_byte1)
		return 0;
	if (byte2 < font->min_byte2 || byte2 > font->max_byte2)
		return 0;

	/* A cell every character shares. */
	if (font->metrics == NULL) {
		*metrics = font->max_bounds;
		return 1;
	}

	/* The character's own, all zero when it is not there. */
	index = (size_t)(byte1 - font->min_byte1) * ((size_t)font->max_byte2 - font->min_byte2 + 1U) + (byte2 - font->min_byte2);
	*metrics = font->metrics[index];
	if (metrics->width == 0 && metrics->left == 0 && metrics->right == 0 && metrics->ascent == 0 && metrics->descent == 0)
		return 0;

	/* Succeeded: the font has it. */
	return 1;
}

/*
 * Gives a character's metrics, or the default character's in its place
 * (*character becomes it).  Returns 1, or 0 when neither is in the font.
 */
static int
font_char_or_default(
	const struct x11_font *font,
	uint32_t *character,
	struct x11_char_metrics *metrics)
{
	int present;

	/* The character itself. */
	present = font_char(font, *character, metrics);
	if (present)
		return 1;

	/* The default character. */
	present = font_char(font, font->default_char, metrics);
	if (!present)
		return 0;

	/* Succeeded: the default character stands in. */
	*character = font->default_char;
	return 1;
}

/*
 * Gives the Unicode code point a character of a font is drawn as: its
 * own in ISO 8859-1 and ISO 10646, but the DEC special graphics at 1 to
 * 31 of a fixed font.
 */
static uint32_t
font_codepoint(
	const struct x11_font *font,
	uint32_t character)
{
	/* A fixed font's line-drawing set. */
	if (font->line_drawing && character >= 1U && character <= 31U)
		return font_dec_graphics[character - 1U];

	/* Succeeded: the character's own. */
	return character;
}

/* Finds the line-drawing character a fixed font draws itself (NULL: the face draws it). */
static const struct font_line *
font_line_of(
	uint32_t codepoint)
{
	size_t index;

	/* The table's. */
	for (index = 0U; index < sizeof(font_lines) / sizeof(font_lines[0]); index++) {
		if (font_lines[index].codepoint == codepoint)
			return &font_lines[index];
	}

	/* Not one of them. */
	return NULL;
}

/*
 * Draws a line-drawing character across a fixed font's cell: the half
 * lines from the cell's middle to its edges, or a scan line (1 the top
 * row, 9 the bottom).
 */
static void
font_draw_line(
	const struct font_line *line,
	struct x11_font_glyph *glyph)
{
	int middle_x;
	int middle_y;
	int from;
	int to;
	int row;
	int column;

	/* The cell's middle. */
	middle_x = (glyph->width - 1) / 2;
	middle_y = (glyph->height - 1) / 2;

	/* A scan line: one row, from the top to the bottom in eight steps. */
	if ((line->shape & LINE_SCAN) != 0U) {
		row = ((int)line->scan - 1) * (glyph->height - 1) / 8;
		for (column = 0; column < glyph->width; column++)
			glyph->bits[(size_t)row * glyph->stride + (size_t)column / 8U] |= (uint8_t)(0x80U >> (column % 8));
		return;
	}

	/* The horizontal half lines, along the middle row. */
	from = middle_x;
	to = middle_x;
	if ((line->shape & LINE_LEFT) != 0U)
		from = 0;
	if ((line->shape & LINE_RIGHT) != 0U)
		to = glyph->width - 1;
	if ((line->shape & (LINE_LEFT | LINE_RIGHT)) != 0U) {
		for (column = from; column <= to; column++)
			glyph->bits[(size_t)middle_y * glyph->stride + (size_t)column / 8U] |= (uint8_t)(0x80U >> (column % 8));
	}

	/* The vertical half lines, down the middle column. */
	from = middle_y;
	to = middle_y;
	if ((line->shape & LINE_UP) != 0U)
		from = 0;
	if ((line->shape & LINE_DOWN) != 0U)
		to = glyph->height - 1;
	if ((line->shape & (LINE_UP | LINE_DOWN)) != 0U) {
		for (row = from; row <= to; row++)
			glyph->bits[(size_t)row * glyph->stride + (size_t)middle_x / 8U] |= (uint8_t)(0x80U >> (middle_x % 8));
	}
}

/*
 * Makes the reply that tells a font's information (QueryFont's and
 * ListFontsWithInfo's): the bounds, the character range, the default
 * character, the ascent and descent and the properties, with extra bytes
 * after them for the caller.  Returns its length (0 without memory).
 */
static size_t
font_info(
	struct x11server *server,
	const struct x11_font *font,
	struct x11_client *client,
	uint8_t **reply,
	size_t extra)
{
	struct font_property properties[FONT_PROPERTIES_MAX];
	uint8_t *bytes;
	uint32_t name;
	uint32_t value;
	size_t length;
	unsigned count;
	unsigned kept;
	unsigned item;
	int msb;

	/* The properties, and the reply with room for them and the extra bytes. */
	msb = client->order;
	count = font_properties(font, properties);
	length = 60U + (size_t)count * 8U + extra;
	bytes = calloc(1U, length);
	if (bytes == NULL)
		return 0U;

	/* The bounds and the characters' range. */
	font_write_metrics(bytes + 8, &font->min_bounds, msb);
	font_write_metrics(bytes + 24, &font->max_bounds, msb);
	x11_write16(bytes + 40, font->min_byte2, msb);
	x11_write16(bytes + 42, font->max_byte2, msb);
	x11_write16(bytes + 44, font->default_char, msb);
	bytes[48] = 0U;
	bytes[49] = font->min_byte1;
	bytes[50] = font->max_byte1;
	bytes[51] = 0U;
	if (font->metrics == NULL)
		bytes[51] = 1U;
	x11_write16(bytes + 52, (uint16_t)font->ascent, msb);
	x11_write16(bytes + 54, (uint16_t)font->descent, msb);

	/* Each property whose atoms the server has room for, its value a number or an atom. */
	kept = 0U;
	for (item = 0U; item < count; item++) {
		name = x11_atom_intern(server, properties[item].name, strlen(properties[item].name), 0);
		if (name == 0U)
			continue;
		value = properties[item].value;
		if (properties[item].text != NULL) {
			value = x11_atom_intern(server, properties[item].text, properties[item].text_length, 0);
			if (value == 0U)
				continue;
		}
		x11_write32(bytes + 60U + (size_t)kept * 8U, name, msb);
		x11_write32(bytes + 64U + (size_t)kept * 8U, value, msb);
		kept++;
	}

	/* The extra bytes follow the properties kept. */
	if (kept != count) {
		memmove(bytes + 60U + (size_t)kept * 8U, bytes + 60U + (size_t)count * 8U, extra);
		length -= (size_t)(count - kept) * 8U;
	}
	x11_write16(bytes + 46, (uint16_t)kept, msb);

	/* Succeeded: the reply. */
	*reply = bytes;
	return length;
}

/*
 * Lists a font's properties: its name, the fields of its XLFD name, and
 * its ascent, descent and default character.  Returns how many.
 */
static unsigned
font_properties(
	const struct x11_font *font,
	struct font_property *properties)
{
	static const char *const field_names[FONT_FIELDS] = {
		"FOUNDRY", "FAMILY_NAME", "WEIGHT_NAME", "SLANT", "SETWIDTH_NAME", "ADD_STYLE_NAME", "PIXEL_SIZE",
		"POINT_SIZE", "RESOLUTION_X", "RESOLUTION_Y", "SPACING", "AVERAGE_WIDTH", "CHARSET_REGISTRY",
		"CHARSET_ENCODING"
	};
	const char *fields[FONT_FIELDS];
	size_t lengths[FONT_FIELDS];
	unsigned count;
	unsigned field;
	unsigned number;
	int error;

	/* The name itself. */
	count = 0U;
	properties[count].name = "FONT";
	properties[count].value = 0U;
	properties[count].text = font->name;
	properties[count].text_length = strlen(font->name);
	count++;

	/* A name of another shape (the special fonts) has nothing more. */
	error = font_split(font->name, fields, lengths);
	if (error != 0)
		return count;

	/* Each field: the numbers as numbers, the words as atoms. */
	for (field = 0U; field < FONT_FIELDS; field++) {
		properties[count].name = field_names[field];
		properties[count].value = 0U;
		properties[count].text = NULL;
		properties[count].text_length = 0U;
		error = font_number(fields[field], lengths[field], &number);
		if (error == 0 && field != FIELD_ENCODING) {
			properties[count].value = number;
		} else {
			properties[count].text = fields[field];
			properties[count].text_length = lengths[field];
		}
		count++;
	}

	/* The ascent, the descent and the default character. */
	properties[count].name = "FONT_ASCENT";
	properties[count].value = (uint32_t)font->ascent;
	properties[count].text = NULL;
	count++;
	properties[count].name = "FONT_DESCENT";
	properties[count].value = (uint32_t)font->descent;
	properties[count].text = NULL;
	count++;
	properties[count].name = "DEFAULT_CHAR";
	properties[count].value = font->default_char;
	properties[count].text = NULL;
	count++;

	/* Succeeded: listed. */
	return count;
}

/* Writes a character's metrics as the protocol's CHARINFO (no attributes). */
static void
font_write_metrics(
	uint8_t *bytes,
	const struct x11_char_metrics *metrics,
	int msb)
{
	/* The ink's edges, the width, the ink above and below the baseline. */
	x11_write16(bytes, (uint16_t)metrics->left, msb);
	x11_write16(bytes + 2, (uint16_t)metrics->right, msb);
	x11_write16(bytes + 4, (uint16_t)metrics->width, msb);
	x11_write16(bytes + 6, (uint16_t)metrics->ascent, msb);
	x11_write16(bytes + 8, (uint16_t)metrics->descent, msb);
	x11_write16(bytes + 10, 0U, msb);
}

/* Starts a run of glyphs with none. */
static void
font_run_init(
	struct font_run *run)
{
	/* No glyph and no bits. */
	memset(run, 0, sizeof(*run));
}

/* Ends a run of glyphs. */
static void
font_run_fini(
	struct font_run *run)
{
	/* The glyphs and their bits. */
	free(run->glyphs);
	free(run->bits);
	memset(run, 0, sizeof(*run));
}

/*
 * Adds a character's glyph to a run, its origin on the baseline at (x, y)
 * of the drawable (a character without bits adds nothing).  Returns 0, or
 * ENOMEM.
 */
static int
font_run_add(
	struct x11server *server,
	struct font_run *run,
	struct x11_font *font,
	uint32_t character,
	int x,
	int y)
{
	const struct x11_font_glyph *glyph;
	struct x11_text_glyph *glyphs;
	struct x11_box box;
	uint8_t *bits;
	size_t bytes;
	size_t wanted;

	/* The glyph's bits. */
	glyph = x11_font_glyph(server, font, character);
	if (glyph == NULL)
		return 0;
	bytes = glyph->stride * (size_t)glyph->height;

	/* Room for one more glyph. */
	if (run->count == run->capacity) {
		wanted = run->capacity * 2U;
		if (wanted < 64U)
			wanted = 64U;
		glyphs = realloc(run->glyphs, wanted * sizeof(*glyphs));
		if (glyphs == NULL)
			return ENOMEM;
		run->glyphs = glyphs;
		run->capacity = wanted;
	}

	/* Room for its bits. */
	if (run->used + bytes > run->size) {
		wanted = run->size * 2U;
		if (wanted < 4096U)
			wanted = 4096U;
		while (wanted < run->used + bytes)
			wanted *= 2U;
		bits = realloc(run->bits, wanted);
		if (bits == NULL)
			return ENOMEM;
		run->bits = bits;
		run->size = wanted;
	}

	/* The glyph at its place, and its bits copied. */
	run->glyphs[run->count].x = x + glyph->left;
	run->glyphs[run->count].y = y - glyph->top;
	run->glyphs[run->count].width = glyph->width;
	run->glyphs[run->count].height = glyph->height;
	run->glyphs[run->count].stride = glyph->stride;
	run->glyphs[run->count].offset = run->used;
	memcpy(run->bits + run->used, glyph->bits, bytes);
	run->used += bytes;

	/* The box the run covers grows to the glyph's. */
	box.x0 = x + glyph->left;
	box.y0 = y - glyph->top;
	box.x1 = box.x0 + glyph->width;
	box.y1 = box.y0 + glyph->height;
	if (run->count == 0U) {
		run->extent = box;
	} else {
		if (box.x0 < run->extent.x0)
			run->extent.x0 = box.x0;
		if (box.y0 < run->extent.y0)
			run->extent.y0 = box.y0;
		if (box.x1 > run->extent.x1)
			run->extent.x1 = box.x1;
		if (box.y1 > run->extent.y1)
			run->extent.y1 = box.y1;
	}

	/* Succeeded: one more glyph. */
	run->count++;
	return 0;
}

/* Draws a run's glyphs as the graphics context fills, and empties the run.  Returns 0, or the error. */
static unsigned
font_run_draw(
	struct x11server *server,
	struct font_run *run,
	uint32_t drawable,
	uint32_t gc)
{
	struct x11_text text;
	unsigned code;

	/* Nothing gathered. */
	if (run->count == 0U)
		return 0U;

	/* The glyphs. */
	memset(&text, 0, sizeof(text));
	text.drawable = drawable;
	text.gc = gc;
	text.extent = run->extent;
	text.glyphs = run->glyphs;
	text.count = run->count;
	text.bits = run->bits;
	code = x11_draw_text(server, &text);

	/* The run is empty again. */
	run->count = 0U;
	run->used = 0U;

	/* Reports a drawable or context that cannot be drawn on. */
	if (code != 0U)
		return code;

	/* Succeeded: drawn. */
	return 0U;
}
