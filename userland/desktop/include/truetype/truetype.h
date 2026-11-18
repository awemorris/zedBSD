/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reading a TrueType font and drawing its glyphs.
 *
 * This reads the tables a screen needs and no more: the character map, the
 * outlines, and the advance widths.  Hinting, kerning and layout are not
 * here; what is here is enough to put readable text on a display.  Colour
 * glyphs are found as the PNG images a colour emoji font stores
 * (truetype_color_glyph); decoding them is the caller's.
 *
 * A face is opened over memory the caller owns and keeps.  Nothing is copied
 * out of the file, so the memory must outlive the face.
 *
 * Sizes are in pixels per em.  A glyph is rendered into a caller-owned
 * 8-bit coverage bitmap: 0 is background and 255 is the glyph, with the
 * values between naming how much of the pixel the outline covers.  Whoever
 * draws decides what those mean in colour.
 */

#ifndef TRUETYPE_H
#define TRUETYPE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct truetype_face;

/*
 * Where one glyph sits and how far the pen moves after it.
 *
 * left and top are the offset from the pen position to the top-left corner
 * of the bitmap, with y growing downward as a framebuffer does.  advance is
 * how far the pen moves, in whole pixels.
 */
struct truetype_glyph {
	unsigned width;
	unsigned height;
	int left;
	int top;
	int advance;
};

/*
 * The vertical measurements of a face at one size, in whole pixels.
 *
 * line_height is what a caller adds between baselines; it is the font's own
 * spacing, not the sum of ascent and descent.
 */
struct truetype_metrics {
	int ascent;
	int descent;
	int line_height;
};

/*
 * Opens a face over font data the caller owns.
 *
 * The data must remain readable and unchanged until the face is closed.
 * index selects a face inside a TrueType collection; it is zero for an
 * ordinary font file.  Returns zero, or an errno value.
 */
/*
 * The face's own vertical measures, in its design units (descent is
 * negative), and the size of its em in the same units.
 */
struct truetype_design_metrics {
	unsigned units_per_em;
	int ascent;
	int descent;
	int line_gap;
};

int truetype_open(const void *data, size_t size, unsigned index,
		  struct truetype_face **face);

void truetype_close(struct truetype_face *face);

/*
 * Chooses the size, in pixels per em.  Every later call reports and renders
 * at this size.  Returns zero, or an errno value.
 */
int truetype_set_pixel_size(struct truetype_face *face, unsigned pixels);

/*
 * Makes every later glyph bold (nonzero) or regular (0), for a face that
 * comes in one weight (BUG-205).  A bold glyph's outline is widened as a
 * vector before it is drawn -- its strokes thicker across by a
 * twenty-second of the size and up and down by less, the way a bold
 * face's are -- and its advance grows by the widening, rounded to whole
 * pixels.
 * Returns zero, or an errno value.
 */
int truetype_set_bold(struct truetype_face *face, int bold);
/*
 * Opens a face's companions (ws090-p020), the files a desktop installs
 * beside its font: bold_path holds the face's own glyphs, in the same
 * order, in a bold weight, and draws them while the face is bold (instead
 * of widening them); next_path is a face that draws the characters this
 * one lacks.  truetype_glyph_index numbers the next face's glyphs after
 * the face's own, and every call given such a number draws from it at the
 * face's size and weight (the calls in design units scale its units to the
 * face's em).  The face's vertical measures stay its own.  Either path may
 * be NULL, and a file that cannot be read, or a bold face of other glyphs,
 * is left out.  The companions close with the face.  Returns zero, or
 * EINVAL for no face or one that has companions already.
 */
int truetype_open_companions(struct truetype_face *face,
			     const char *bold_path, const char *next_path);
/*
 * Tells whether a glyph is drawn from the face's bold companion while the
 * face is bold (1), so that a caller widening bold glyphs itself leaves it
 * as it is, or not (0).
 */
int truetype_glyph_bold_face(const struct truetype_face *face, unsigned glyph);

int truetype_metrics(const struct truetype_face *face,
		     struct truetype_metrics *metrics);

/*
 * Finds the glyph a character is drawn with.  Returns zero when the face has
 * no glyph for it, which is glyph zero: the shape a font shows for what it
 * cannot draw.
 */
unsigned truetype_glyph_index(const struct truetype_face *face,
			      uint32_t codepoint);

/*
 * Reports where a glyph sits without drawing it, so that a caller can
 * measure a line or allocate a bitmap.  Returns zero, or an errno value.
 */
int truetype_glyph_metrics(struct truetype_face *face, unsigned glyph,
			   struct truetype_glyph *metrics);

/*
 * Draws one glyph into an 8-bit coverage bitmap.
 *
 * The bitmap is written in full, including the background, and metrics is
 * filled in with the box that was drawn.
 *
 * size is how many bytes the bitmap holds.  It is a parameter, rather than
 * something the caller is trusted to have worked out, because the size a
 * glyph needs is only known once the font has been read -- and the font
 * came from somewhere else.  A glyph that does not fit reports EINVAL and
 * draws nothing; metrics still says how big it would have been, so that a
 * caller can allocate and ask again.  truetype_glyph_metrics() answers the
 * same question without drawing.
 *
 * Returns zero, or an errno value.
 */
int truetype_render_glyph(struct truetype_face *face, unsigned glyph,
			  struct truetype_glyph *metrics,
			  uint8_t *bitmap, size_t stride, size_t size);

/*
 * Reports the face's design measures (the units a fractional-size layout
 * scales itself).
 */
int truetype_design_metrics(const struct truetype_face *face,
			    struct truetype_design_metrics *metrics);

/*
 * Reports a glyph's advance width in design units.
 */
int truetype_glyph_design_advance(const struct truetype_face *face,
				  unsigned glyph, int *advance);

/*
 * One point of a glyph's outline, in design units with y growing upward.
 *
 * on_curve is 1 for a point the outline passes through and 0 for the
 * control point of a quadratic curve.  Two control points in a row imply an
 * on-curve point halfway between them, exactly as the glyf table stores it.
 * The coordinates are whole numbers for a simple glyph; a scaled or rotated
 * component of a composite glyph can make them fractional.
 */
struct truetype_outline_point {
	float x;
	float y;
	unsigned on_curve;
};

/*
 * One glyph's outline, as TrueType quadratic contours in design units.
 *
 * The caller supplies the two arrays and their capacities.  contour_ends[i]
 * is the index of the last point of contour i, so contour i runs from the
 * point after contour_ends[i - 1] (or point 0) to contour_ends[i] and closes
 * back to its first point.
 *
 * advance and left_side_bearing come from hmtx; x_min to y_max are the box
 * the glyf entry records.  A glyph with no outline, such as a space, reports
 * no contours and a zero box.
 */
struct truetype_glyph_outline {
	struct truetype_outline_point *points;
	unsigned point_capacity;
	unsigned point_count;
	unsigned *contour_ends;
	unsigned contour_capacity;
	unsigned contour_count;
	int advance;
	int left_side_bearing;
	int x_min;
	int y_min;
	int x_max;
	int y_max;
};

/*
 * Reads one glyph's contours out of glyf, in design units.
 *
 * A composite glyph is flattened: each component's contours are appended
 * with the component's offset and scale or matrix applied.  Hinting
 * instructions are ignored.
 *
 * When the arrays are too small, ENOSPC is returned and point_count and
 * contour_count still say how many the glyph needs, so that a caller can
 * allocate and ask again.  Returns zero, or an errno value.
 */
int truetype_glyph_outline(const struct truetype_face *face, unsigned glyph,
			   struct truetype_glyph_outline *outline);

/*
 * Opens a face embedded in a document.
 *
 * It differs from truetype_open() only in that a face without a cmap, or
 * without a Unicode subtable this reader chooses, still opens: a font a
 * document carries is often addressed by glyph number.  For such a face
 * truetype_glyph_index() finds nothing.  Returns zero, or an errno value.
 */
int truetype_open_embedded(const void *data, size_t size,
			   struct truetype_face **face);

/*
 * A colour glyph kept as a bitmap (the CBDT and CBLC tables of a colour
 * emoji font, ws102-p019): its image as the PNG file the font stores
 * (inside the font's memory; the caller decodes it), the image's size in
 * pixels, where its top-left corner sits from the pen (left, and top up
 * from the baseline) and how far the pen moves, all at the size of the
 * font's strike, ppem pixels per em.  A caller drawing at another size
 * scales them by its size over ppem.
 */
struct truetype_color_glyph {
	const uint8_t *png;
	size_t png_size;
	unsigned width;
	unsigned height;
	int left;
	int top;
	int advance;
	unsigned ppem;
};

/*
 * Finds a glyph's colour image at the strike nearest the face's pixel
 * size (the smallest at least as large, else the largest).  Returns 0,
 * ENOENT when the face or the glyph has none, or EINVAL for a damaged
 * table.
 */
int truetype_color_glyph(const struct truetype_face *face, unsigned glyph,
			 struct truetype_color_glyph *out);

/*
 * Looks a code up in the cmap subtable of one platform and encoding, such
 * as the Windows symbol map (3, 0) or the Macintosh Roman map (1, 0).
 * Formats 0, 4, 6 and 12 are read.  Returns ENOENT when the face has no
 * such subtable; otherwise zero, with *glyph the glyph or 0 for a code the
 * subtable does not map.
 */
int truetype_cmap_lookup(const struct truetype_face *face, unsigned platform,
			 unsigned encoding, uint32_t code, unsigned *glyph);

#ifdef __cplusplus
}
#endif

#endif
