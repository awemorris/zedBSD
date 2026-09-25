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
 * outlines, and the advance widths.  Hinting, kerning, colour and layout are
 * not here; what is here is enough to put readable text on a display.
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
int truetype_open(const void *data, size_t size, unsigned index,
		  struct truetype_face **face);

void truetype_close(struct truetype_face *face);

/*
 * Chooses the size, in pixels per em.  Every later call reports and renders
 * at this size.  Returns zero, or an errno value.
 */
int truetype_set_pixel_size(struct truetype_face *face, unsigned pixels);

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

#ifdef __cplusplus
}
#endif

#endif
