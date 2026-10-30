/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Painting (plan/ws074/design.md §8): the display list made from a laid
 * out page, and the CPU reference renderer that draws it into memory.
 *
 * The display list is the one definition of what a page looks like: the
 * GPU renderer of the window and the CPU renderer of the headless mode and
 * the tests both draw it, with the same coverage rules, so that the two
 * can be compared pixel by pixel.  Positions are the layout's units (1/64
 * of a pixel) in document coordinates; the renderers subtract the scroll.
 *
 * The first pass paints normal flow: the canvas color, each block's
 * background and borders (every style drawn solid), the text of the lines
 * with its underline, and the images of <img> elements.  Rounded corners,
 * background images, opacity and shadows come later.
 */

#ifndef KEILAND_BROWSER_PAINT_H
#define KEILAND_BROWSER_PAINT_H

#include "layout/layout.h"

/*
 * The kinds of display item.
 */
enum paint_kind {
	PAINT_RECT,
	PAINT_TEXT,
	PAINT_CLIP,
	PAINT_UNCLIP,
	PAINT_IMAGE
};

/* The deepest nesting of clips the renderers keep (deeper clips are ignored). */
#define PAINT_CLIP_DEPTH	64

/*
 * One glyph of a text item: the code point it draws (the text system
 * finds its face and bitmap) and its pen position from the item's origin.
 */
struct paint_glyph {
	uint32_t code_point;
	layout_unit x;
};

/*
 * One display item.
 *
 * A rectangle is filled with a color, its edges covering the pixels they
 * cross in proportion.  A text item draws its glyphs from x along baseline
 * y; a vertical item uses x as its physical baseline and advances down from
 * y.  An image item stretches its image
 * over its rectangle: each pixel takes the image's pixel under its centre
 * (no filtering), and its edges cover the pixels they cross in proportion
 * as a rectangle's do.  The image belongs to the page, which keeps it for
 * as long as the list lives.
 */
struct paint_item {
	int kind;
	/* Clockwise sideways glyphs on a physical vertical inline axis. */
	int vertical;
	layout_unit x;
	layout_unit y;
	layout_unit width;
	layout_unit height;
	uint32_t color;
	struct text_font font;
	const struct paint_glyph *glyphs;
	size_t glyph_count;
	const struct img_bitmap *image;
};

/*
 * A display list: the items in painting order, the arena their glyphs
 * live in, the color under everything and the size of the document.
 */
struct paint_list {
	struct wb_arena arena;
	struct wb_vector items;
	uint32_t canvas_color;
	layout_unit width;
	layout_unit height;
};

/*
 * A bitmap the CPU renderer draws into: width by height pixels, each
 * 0xAARRGGBB with the alpha always opaque, rows top to bottom.
 */
struct paint_bitmap {
	uint32_t *pixels;
	int width;
	int height;
};

/*
 * One clip while a list is drawn: its rectangle in the target's pixels
 * (the document scrolled), and the same rounded to whole pixels, which
 * glyphs (drawn on whole pixels) are clipped by.
 */
struct paint_clip {
	float left;
	float top;
	float right;
	float bottom;
	int pixel_left;
	int pixel_top;
	int pixel_right;
	int pixel_bottom;
};

/*
 * The clips a renderer is inside while it draws a list: a stack whose top
 * is the intersection of every clip started and not ended (the first entry
 * is the whole target).  Clips nested deeper than the stack holds are
 * counted but do not narrow it.
 */
struct paint_clips {
	struct paint_clip stack[PAINT_CLIP_DEPTH + 1];
	int depth;
	int ignored;
};

/* The display list (list.c). */
int paint_build(struct paint_list *list, const struct layout_tree *tree);
void paint_release(struct paint_list *list);
int paint_add_ring(struct paint_list *list, layout_unit x, layout_unit y, layout_unit width, layout_unit height, layout_unit thickness, uint32_t color);
int paint_add_rect(struct paint_list *list, layout_unit x, layout_unit y, layout_unit width, layout_unit height, uint32_t color);
int paint_gradient_render(struct wb_arena *arena, const struct css_gradient *gradient, int width, int height, struct img_bitmap *bitmap);
int paint_dump(const struct paint_list *list, struct wb_buffer *out);
void paint_clips_init(struct paint_clips *clips, int width, int height);
void paint_clips_push(struct paint_clips *clips, const struct paint_item *item, layout_unit scroll_y);
void paint_clips_pop(struct paint_clips *clips);
const struct paint_clip *paint_clips_top(const struct paint_clips *clips);

/* The CPU reference renderer (software.c). */
int paint_bitmap_create(struct paint_bitmap *bitmap, int width, int height);
void paint_bitmap_release(struct paint_bitmap *bitmap);
int paint_software(const struct paint_list *list, struct text_system *text, layout_unit scroll_y, struct paint_bitmap *bitmap);
int paint_write_ppm(const struct paint_bitmap *bitmap, const char *path);
uint32_t paint_canvas_pixel(uint32_t color);

#endif
