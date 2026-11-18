/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The text drawn from TrueType fonts, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_TEXT_H
#define KEILAND_TEXT_H

#include <stddef.h>
#include <stdint.h>
#include <keiland/keiland_canvas.h>

#ifdef __cplusplus
extern "C" {
#endif

struct truetype_face;

/*
 * How many fonts the text draws from: the main one, a fallback, and the
 * colour emoji font (KL_TEXT_EMOJI, opened the first time a character neither
 * of the others has is drawn).
 */
#define KL_TEXT_FACES		3
#define KL_TEXT_EMOJI		"/usr/share/fonts/keiland-emoji.ttf"

/*
 * One glyph drawn at one size, kept for the next time.
 *
 * key is zero for an empty slot; the bitmap is the glyph's coverage, or
 * pixels its colour (premultiplied 0xAARRGGBB, a colour emoji) with bitmap
 * NULL.
 */
struct kl_glyph {
	uint32_t key;
	int width;
	int height;
	int left;
	int top;
	int advance;
	uint8_t *bitmap;
	uint32_t *pixels;
};

/*
 * One font file: its bytes (kept for the face) and the face.
 */
struct kl_text_face {
	void *data;
	size_t size;
	struct truetype_face *face;
	unsigned pixels;
};

/*
 * The text of the window: the fonts and every glyph drawn so far.
 *
 * One lives for the whole program.  The cache is emptied when it fills up,
 * which only costs drawing the glyphs again.  emoji_tried says the emoji font
 * (faces[2]) was looked for.
 */
struct kl_text {
	struct kl_text_face faces[KL_TEXT_FACES];
	int face_count;
	int emoji_tried;
	struct kl_glyph *cache;
	unsigned cache_size;
	unsigned cache_used;
	uint8_t *scratch;
	size_t scratch_size;
};

/*
 * The vertical measurements of text at one size, in pixels.
 */
struct kl_text_line {
	int ascent;
	int descent;
	int height;
};

/* The text (text.c). */
int kl_text_open(struct kl_text *text, const char *primary, const char *fallback);
/* KL_VERSION 48: the companions' files for fonts opened from now on (a host test with the tree's fonts); NULL: the installed ones. */
void kl_text_companions(const char *bold, const char *mono);
void kl_text_close(struct kl_text *text);
void kl_text_metrics(struct kl_text *text, unsigned pixels, struct kl_text_line *line);
int kl_text_center(unsigned pixels, int top, int height);
int kl_text_width(struct kl_text *text, const char *string, size_t length, unsigned pixels, int bold);
int kl_text_draw(struct kl_text *text, struct kl_canvas *canvas, int x, int baseline, const char *string, size_t length, unsigned pixels, int bold, kl_color color);
int kl_text_draw_fit(struct kl_text *text, struct kl_canvas *canvas, int x, int baseline, const char *string, unsigned pixels, int bold, int width, kl_color color);
size_t kl_text_fit(struct kl_text *text, const char *string, unsigned pixels, int bold, int width, char *out, size_t size);
size_t kl_text_break(struct kl_text *text, const char *string, unsigned pixels, int bold, int width);
uint32_t kl_utf8_next(const char *string, size_t length, size_t *index);

#ifdef __cplusplus
}
#endif

#endif
