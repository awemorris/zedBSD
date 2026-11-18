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
	PAINT_IMAGE,
	/* The filters of backdrop-filter applied to what is drawn so far inside the rectangle. */
	PAINT_BACKDROP,
	/* The items up to the matching PAINT_GROUP_END are painted apart from the page (over nothing) and the filters applied to the result. */
	PAINT_GROUP,
	PAINT_GROUP_END,
	/*
	 * The document of a frame that commits apart from the list (in an agent
	 * of its own): its number (frame) and its content box (x, y, width,
	 * height), inside a clip of it.  What it shows is found by its number
	 * when the list is drawn (paint_frames_resolve); a renderer given the
	 * item itself draws nothing for it.
	 */
	PAINT_FRAME
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
	/* Clockwise sideways glyphs on a physical vertical inline axis (1), or counterclockwise ones running up from y (2). */
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
	/* A backdrop item's filter functions (the box's style, kept by the page). */
	const struct css_filter *filters;
	int filter_count;
	/* A backdrop item's rounded corners (horizontal and vertical radius of each, clockwise from the top left), when it has any. */
	int rounded;
	layout_unit radius[4][2];
	/* A frame item's frame number (PAINT_FRAME). */
	uint64_t frame;
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
	/* Whether the document gave the canvas a color of its own (the root's or the body's background). */
	int canvas_set;
	/* The documents of <iframe> elements painted into the items: each laid out tree and its list, which the items point into (struct paint_frame *). */
	struct wb_vector frames;
};

/* One frame's document painted into a list: kept as long as the list is. */
struct paint_frame {
	struct layout_tree *tree;
	struct paint_list list;
	/* The lists and the page's cache that hold it; it is freed when the last lets go (changed atomically: a scene lets go on the drawing thread). */
	unsigned refs;
};

/* Lets go of a reference to a frame (the last frees it). */
void paint_frame_release(struct paint_frame *frame);

/*
 * A display list taken from the page that built it (paint_scene_make), to
 * be drawn on another thread: its items point only into its arena, into the
 * documents of its frames (which it holds), and at copies of the image
 * records over pixels it holds; faces holds the font files of the text
 * system it was built with (face by face, NULL for a face not open), from
 * which a drawing thread opens faces of its own.  It is not changed once
 * made.  refs counts its holders and is changed atomically (it is let go
 * on the drawing thread too).
 */
struct paint_scene {
	unsigned refs;
	struct paint_list list;
	struct wb_vector holds;
	struct text_bytes *faces[TEXT_FACES];
};

/*
 * Finds the latest scene of a frame committed apart (PAINT_FRAME), held for
 * the caller, and how far its document is scrolled; NULL when it has none
 * yet.  Called on the drawing thread.
 */
typedef struct paint_scene *(*paint_frame_lookup)(void *context, uint64_t frame, layout_unit *scroll_x, layout_unit *scroll_y);

/*
 * What one drawing of a scene draws (paint_frames_resolve): when the
 * scene has frame items (resolved), a list of its items with each frame
 * item replaced by its frame's scene, moved to the item's box, which
 * points into the scenes it holds (held, struct paint_scene *) and whose
 * glyphs take their faces from faces (the scene's, with the frames' web
 * faces put in the web faces the scene leaves free).  The drawing's holds
 * are kept until the work recorded from it is done.
 */
struct paint_drawing {
	int resolved;
	struct paint_list list;
	struct wb_vector held;
	struct text_bytes *faces[TEXT_FACES];
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

/*
 * What was painted of an element (or of the page without its named elements) for a view transition: the items
 * with their glyphs, where they were and the size of the border box they belong to.
 */
struct paint_snapshot {
	struct paint_item *items;
	size_t count;
	struct paint_glyph *glyphs;
	size_t glyph_count;
	layout_unit x;
	layout_unit y;
	layout_unit width;
	layout_unit height;
};

/* The display list (list.c) and its scenes (scene.c). */
int paint_transition_capture(const struct layout_tree *tree, const struct layout_transition *transition, const struct layout_box *box, struct paint_snapshot **out);
void paint_snapshot_release(struct paint_snapshot *snapshot);
int paint_build(struct paint_list *list, const struct layout_tree *tree);
void paint_release(struct paint_list *list);
void paint_list_init(struct paint_list *list);
int paint_scene_make(struct paint_list *list, const struct text_system *text, struct paint_scene **out);
void paint_scene_retain(struct paint_scene *scene);
void paint_scene_release(struct paint_scene *scene);
void paint_drawing_init(struct paint_drawing *drawing);
int paint_frames_resolve(const struct paint_scene *scene, paint_frame_lookup lookup, void *context, struct paint_drawing *drawing);
const struct paint_list *paint_drawing_list(const struct paint_scene *scene, const struct paint_drawing *drawing);
void paint_drawing_release(struct paint_drawing *drawing);
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
