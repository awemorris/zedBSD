/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * How a window lights where a drop would land (ws189-p002, plan/ws189/
 * phase001/phase.md section 2.4), the same in every application: a frame
 * of the theme's accent around a place -- a picture's frame, a file's
 * cell -- with a faint fill of the accent inside, and a caret of the
 * accent with a soft glow beside it where text would go.  The compositor
 * shows on the drag whether the drop is taken; the window shows where.
 */

#include <keiland/keiland.h>

/* The ring's alpha, and the caret's glow's width on each side and its alpha. */
#define DROP_RING_ALPHA		230U
#define DROP_GLOW_WIDTH		3
#define DROP_GLOW_ALPHA		64U

/*
 * Draws the frame around a place a drop would land on: a ring of the
 * accent KL_DROP_RING pixels wide and the accent at KL_DROP_FILL_ALPHA
 * inside.
 */
void
kl_drop_frame(
	struct kl_canvas *canvas,
	const struct kl_theme *theme,
	float x,
	float y,
	float width,
	float height,
	float radius)
{
	kl_color fill;
	kl_color ring;

	/* Nothing to draw on, or no place. */
	if (canvas == NULL || theme == NULL)
		return;
	if (width <= 0.0f || height <= 0.0f)
		return;

	/* The accent faint inside, then its ring over the edge. */
	fill = KL_RGBA(theme->accent, KL_DROP_FILL_ALPHA);
	ring = KL_RGBA(theme->accent, DROP_RING_ALPHA);
	kl_canvas_round(canvas, x, y, width, height, radius, fill);
	kl_canvas_round_border(canvas, x, y, width, height, radius, (float)KL_DROP_RING, ring);
}

/*
 * Draws the caret where dropped text would go: a line of the accent
 * KL_DROP_CARET pixels wide from (x, y) down by height, with a glow of the
 * accent DROP_GLOW_WIDTH pixels wide on each side.  It does not blink.
 */
void
kl_drop_caret(
	struct kl_canvas *canvas,
	const struct kl_theme *theme,
	float x,
	float y,
	float height)
{
	struct kl_rect line;
	kl_color glow;
	float half;

	/* Nothing to draw on, or no height. */
	if (canvas == NULL || theme == NULL)
		return;
	if (height <= 0.0f)
		return;

	/* The glow, centred on the line. */
	half = (float)KL_DROP_CARET / 2.0f;
	glow = KL_RGBA(theme->accent, DROP_GLOW_ALPHA);
	kl_canvas_round(canvas, x - half - (float)DROP_GLOW_WIDTH, y, (float)(KL_DROP_CARET + 2 * DROP_GLOW_WIDTH), height, (float)DROP_GLOW_WIDTH, glow);

	/* The line over it, in whole pixels. */
	line.x = (int)(x - half);
	line.y = (int)y;
	line.width = KL_DROP_CARET;
	line.height = (int)height;
	kl_canvas_fill(canvas, &line, KL_RGB(theme->accent));
}
