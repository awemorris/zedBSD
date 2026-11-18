/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The icons of the desktop's applications (Files' icons.c, moved here
 * unchanged by ws090-p002), made of the canvas's shapes.
 *
 * The sidebar and the panels use line icons in one color; items use a
 * filled folder and a page with a colored band that names the file's kind.
 * No icon theme or picture is read: every icon is drawn at the size asked,
 * so they stay sharp at any size.  Coordinates below are fractions of the
 * icon's box (0 is the top or left edge, 1 the bottom or right).
 */

#include "internal.h"

#include <string.h>

/* The line icons' stroke, as a fraction of their size. */
#define ICONS_STROKE		0.085f

/* The folder's colors around its tint. */
#define ICONS_FOLDER_BACK	0.78f
#define ICONS_FOLDER_FRONT_TOP	0.18f

static void icons_polyline(struct kl_canvas *canvas, float x, float y, float size, const float *points, int count, float thickness, kl_color color);
static void icons_segment(struct kl_canvas *canvas, float x, float y, float size, float x0, float y0, float x1, float y1, float thickness, kl_color color);
static void icons_frame(struct kl_canvas *canvas, float x, float y, float size, float left, float top, float width, float height, float radius, float thickness, kl_color color);

/*
 * Draws a line icon in a square box of a size at (x, y).
 */
void
kl_icon_draw(
	struct kl_canvas *canvas,
	enum kl_icon icon,
	float x,
	float y,
	float size,
	kl_color color)
{
	static const float roof[] = { 0.14f, 0.50f, 0.50f, 0.17f, 0.86f, 0.50f };
	static const float house[] = { 0.25f, 0.43f, 0.25f, 0.84f, 0.75f, 0.84f, 0.75f, 0.43f };
	static const float arrow_down[] = { 0.32f, 0.47f, 0.50f, 0.65f, 0.68f, 0.47f };
	static const float tray[] = { 0.18f, 0.66f, 0.18f, 0.84f, 0.82f, 0.84f, 0.82f, 0.66f };
	static const float mountains[] = { 0.16f, 0.74f, 0.40f, 0.50f, 0.56f, 0.64f, 0.68f, 0.54f, 0.84f, 0.70f };
	static const float folder[] = { 0.12f, 0.30f, 0.12f, 0.80f, 0.88f, 0.80f, 0.88f, 0.36f, 0.50f, 0.36f, 0.42f, 0.24f, 0.12f, 0.24f, 0.12f, 0.30f };
	static const float clock_hands[] = { 0.50f, 0.30f, 0.50f, 0.52f, 0.64f, 0.60f };
	static const float can[] = { 0.26f, 0.32f, 0.31f, 0.86f, 0.69f, 0.86f, 0.74f, 0.32f };
	static const float back[] = { 0.60f, 0.24f, 0.36f, 0.50f, 0.60f, 0.76f };
	static const float forward[] = { 0.40f, 0.24f, 0.64f, 0.50f, 0.40f, 0.76f };
	static const float chevron[] = { 0.40f, 0.28f, 0.60f, 0.50f, 0.40f, 0.72f };
	static const float up[] = { 0.30f, 0.60f, 0.50f, 0.40f, 0.70f, 0.60f };
	static const float down[] = { 0.30f, 0.40f, 0.50f, 0.60f, 0.70f, 0.40f };
	float play[6];
	float thickness;

	/* The line pictures are drawn by their own pen (Today and the eject, after them, are this file's). */
	if ((icon >= KL_ICON_TILES && icon <= KL_ICON_DISCLOSURE) || icon == KL_ICON_DISCONNECT) {
		keiui_icon_line_draw(canvas, icon, x, y, size, color);
		return;
	}

	/* The stroke of every line of the icon. */
	thickness = size * ICONS_STROKE;
	if (thickness < 1.2f)
		thickness = 1.2f;

	/* Each icon's lines. */
	switch (icon) {
	case KL_ICON_HOME:
		icons_polyline(canvas, x, y, size, roof, 3, thickness, color);
		icons_polyline(canvas, x, y, size, house, 4, thickness, color);
		icons_segment(canvas, x, y, size, 0.50f, 0.84f, 0.50f, 0.66f, thickness, color);
		break;
	case KL_ICON_DESKTOP:
		icons_frame(canvas, x, y, size, 0.10f, 0.18f, 0.80f, 0.54f, 0.08f, thickness, color);
		icons_segment(canvas, x, y, size, 0.50f, 0.74f, 0.50f, 0.84f, thickness, color);
		icons_segment(canvas, x, y, size, 0.34f, 0.85f, 0.66f, 0.85f, thickness, color);
		break;
	case KL_ICON_DOCUMENTS:
		icons_frame(canvas, x, y, size, 0.22f, 0.10f, 0.56f, 0.80f, 0.08f, thickness, color);
		icons_segment(canvas, x, y, size, 0.36f, 0.34f, 0.64f, 0.34f, thickness, color);
		icons_segment(canvas, x, y, size, 0.36f, 0.50f, 0.64f, 0.50f, thickness, color);
		icons_segment(canvas, x, y, size, 0.36f, 0.66f, 0.54f, 0.66f, thickness, color);
		break;
	case KL_ICON_DOWNLOADS:
		icons_segment(canvas, x, y, size, 0.50f, 0.14f, 0.50f, 0.64f, thickness, color);
		icons_polyline(canvas, x, y, size, arrow_down, 3, thickness, color);
		icons_polyline(canvas, x, y, size, tray, 4, thickness, color);
		break;
	case KL_ICON_PICTURES:
		icons_frame(canvas, x, y, size, 0.10f, 0.18f, 0.80f, 0.64f, 0.10f, thickness, color);
		kl_canvas_circle(canvas, x + 0.35f * size, y + 0.38f * size, 0.07f * size, color);
		icons_polyline(canvas, x, y, size, mountains, 5, thickness, color);
		break;
	case KL_ICON_MUSIC:
		icons_segment(canvas, x, y, size, 0.62f, 0.18f, 0.62f, 0.70f, thickness, color);
		icons_segment(canvas, x, y, size, 0.62f, 0.18f, 0.82f, 0.28f, thickness, color);
		kl_canvas_circle(canvas, x + 0.49f * size, y + 0.72f * size, 0.14f * size, color);
		break;
	case KL_ICON_MOVIES:
		icons_frame(canvas, x, y, size, 0.10f, 0.20f, 0.80f, 0.60f, 0.10f, thickness, color);
		play[0] = x + 0.42f * size;
		play[1] = y + 0.36f * size;
		play[2] = x + 0.64f * size;
		play[3] = y + 0.50f * size;
		play[4] = x + 0.42f * size;
		play[5] = y + 0.64f * size;
		kl_canvas_polygon(canvas, play, 3, color);
		break;
	case KL_ICON_FOLDER_LINE:
		icons_polyline(canvas, x, y, size, folder, 8, thickness, color);
		break;
	case KL_ICON_RECENTS:
		kl_canvas_ring(canvas, x + 0.5f * size, y + 0.5f * size, 0.38f * size, thickness, 1.0f, color);
		icons_polyline(canvas, x, y, size, clock_hands, 3, thickness, color);
		break;
	case KL_ICON_TRASH:
		icons_segment(canvas, x, y, size, 0.18f, 0.26f, 0.82f, 0.26f, thickness, color);
		icons_segment(canvas, x, y, size, 0.40f, 0.26f, 0.42f, 0.16f, thickness, color);
		icons_segment(canvas, x, y, size, 0.42f, 0.16f, 0.58f, 0.16f, thickness, color);
		icons_segment(canvas, x, y, size, 0.58f, 0.16f, 0.60f, 0.26f, thickness, color);
		icons_polyline(canvas, x, y, size, can, 4, thickness, color);
		break;
	case KL_ICON_COMPUTER:
		icons_frame(canvas, x, y, size, 0.16f, 0.20f, 0.68f, 0.48f, 0.08f, thickness, color);
		icons_segment(canvas, x, y, size, 0.06f, 0.80f, 0.94f, 0.80f, thickness, color);
		break;
	case KL_ICON_VOLUME:
		icons_frame(canvas, x, y, size, 0.10f, 0.32f, 0.80f, 0.38f, 0.10f, thickness, color);
		kl_canvas_circle(canvas, x + 0.72f * size, y + 0.51f * size, 0.05f * size, color);
		break;
	case KL_ICON_BACK:
		icons_polyline(canvas, x, y, size, back, 3, thickness, color);
		break;
	case KL_ICON_FORWARD:
		icons_polyline(canvas, x, y, size, forward, 3, thickness, color);
		break;
	case KL_ICON_SEARCH:
		kl_canvas_ring(canvas, x + 0.43f * size, y + 0.43f * size, 0.27f * size, thickness, 1.0f, color);
		icons_segment(canvas, x, y, size, 0.63f, 0.63f, 0.84f, 0.84f, thickness, color);
		break;
	case KL_ICON_GRID:
		kl_canvas_round(canvas, x + 0.14f * size, y + 0.14f * size, 0.31f * size, 0.31f * size, 0.08f * size, color);
		kl_canvas_round(canvas, x + 0.55f * size, y + 0.14f * size, 0.31f * size, 0.31f * size, 0.08f * size, color);
		kl_canvas_round(canvas, x + 0.14f * size, y + 0.55f * size, 0.31f * size, 0.31f * size, 0.08f * size, color);
		kl_canvas_round(canvas, x + 0.55f * size, y + 0.55f * size, 0.31f * size, 0.31f * size, 0.08f * size, color);
		break;
	case KL_ICON_LIST:
		kl_canvas_circle(canvas, x + 0.20f * size, y + 0.26f * size, 0.06f * size, color);
		kl_canvas_circle(canvas, x + 0.20f * size, y + 0.50f * size, 0.06f * size, color);
		kl_canvas_circle(canvas, x + 0.20f * size, y + 0.74f * size, 0.06f * size, color);
		icons_segment(canvas, x, y, size, 0.36f, 0.26f, 0.86f, 0.26f, thickness, color);
		icons_segment(canvas, x, y, size, 0.36f, 0.50f, 0.86f, 0.50f, thickness, color);
		icons_segment(canvas, x, y, size, 0.36f, 0.74f, 0.86f, 0.74f, thickness, color);
		break;
	case KL_ICON_PREVIEW:
		icons_frame(canvas, x, y, size, 0.10f, 0.16f, 0.80f, 0.68f, 0.12f, thickness, color);
		icons_segment(canvas, x, y, size, 0.60f, 0.16f, 0.60f, 0.84f, thickness, color);
		break;
	case KL_ICON_CHEVRON:
		icons_polyline(canvas, x, y, size, chevron, 3, thickness, color);
		break;
	case KL_ICON_CLOSE:
		icons_segment(canvas, x, y, size, 0.28f, 0.28f, 0.72f, 0.72f, thickness, color);
		icons_segment(canvas, x, y, size, 0.72f, 0.28f, 0.28f, 0.72f, thickness, color);
		break;
	case KL_ICON_PLUS:
		icons_segment(canvas, x, y, size, 0.50f, 0.22f, 0.50f, 0.78f, thickness, color);
		icons_segment(canvas, x, y, size, 0.22f, 0.50f, 0.78f, 0.50f, thickness, color);
		break;
	case KL_ICON_UP:
		icons_polyline(canvas, x, y, size, up, 3, thickness, color);
		break;
	case KL_ICON_DOWN:
		icons_polyline(canvas, x, y, size, down, 3, thickness, color);
		break;
	case KL_ICON_TODAY:
		/* A page of a calendar, its two rings, the line under its head and the day marked (Files' sidebar, ws127-p011). */
		icons_frame(canvas, x, y, size, 0.14f, 0.20f, 0.72f, 0.66f, 0.10f, thickness, color);
		icons_segment(canvas, x, y, size, 0.14f, 0.40f, 0.86f, 0.40f, thickness, color);
		icons_segment(canvas, x, y, size, 0.34f, 0.12f, 0.34f, 0.28f, thickness, color);
		icons_segment(canvas, x, y, size, 0.66f, 0.12f, 0.66f, 0.28f, thickness, color);
		kl_canvas_circle(canvas, x + 0.62f * size, y + 0.64f * size, 0.08f * size, color);
		break;
	case KL_ICON_EJECT:
		/* A block over a bar, filled (Files' sidebar's eject, ws090-p023). */
		kl_canvas_round(canvas, x + 0.30f * size, y + 0.25f * size, 0.40f * size, 0.25f * size, 0.075f * size, color);
		kl_canvas_round(canvas, x + 0.25f * size, y + 0.60f * size, 0.50f * size, 0.10f * size, 0.05f * size, color);
		break;
	default:
		break;
	}
}

/*
 * Draws a folder in a square box of a size at (x, y), in a tint (the
 * accent blue for ordinary folders).
 */
void
kl_icon_folder(
	struct kl_canvas *canvas,
	float x,
	float y,
	float size,
	kl_color tint)
{
	kl_color back;
	kl_color front_top;
	kl_color front_bottom;
	kl_color edge;

	/* The back is the tint darkened, the front light at the top and the tint at the bottom. */
	back = kl_color_mix(KL_RGB(0x000000), tint, ICONS_FOLDER_BACK);
	front_top = kl_color_mix(tint, KL_RGB(0xffffff), ICONS_FOLDER_FRONT_TOP);
	front_bottom = tint;
	edge = KL_RGBA(0xffffff, 110);

	/* A soft shadow under the whole folder. */
	kl_canvas_shadow(canvas, x + 0.08f * size, y + 0.26f * size, 0.84f * size, 0.62f * size, 0.08f * size, 0.06f * size, KL_RGBA(0x1f3a66, 40));

	/* The back with its tab. */
	kl_canvas_round(canvas, x + 0.06f * size, y + 0.14f * size, 0.38f * size, 0.16f * size, 0.05f * size, back);
	kl_canvas_round(canvas, x + 0.06f * size, y + 0.20f * size, 0.88f * size, 0.64f * size, 0.08f * size, back);

	/* The front, lighter, over most of the back. */
	kl_canvas_round_gradient(canvas, x + 0.06f * size, y + 0.30f * size, 0.88f * size, 0.56f * size, 0.08f * size, front_top, front_bottom);

	/* A thin light line along the front's top edge. */
	kl_canvas_round(canvas, x + 0.10f * size, y + 0.30f * size, 0.80f * size, 0.012f * size + 1.0f, 0.5f, edge);
}

/*
 * Draws a file as a page with a folded corner and a band in the color of
 * its kind, with a label (the extension) on the band when there is room.
 */
void
kl_icon_file(
	struct kl_canvas *canvas,
	struct kl_text *text,
	float x,
	float y,
	float size,
	kl_color band,
	const char *label)
{
	struct kl_text_line line;
	float corner[6];
	float fold;
	float page_x;
	float page_y;
	float page_width;
	float page_height;
	unsigned pixels;
	int width;
	int label_x;
	int baseline;

	/* The page's box inside the icon's. */
	page_x = x + 0.20f * size;
	page_y = y + 0.08f * size;
	page_width = 0.60f * size;
	page_height = 0.84f * size;
	fold = 0.16f * size;

	/* The page's shadow, the page and its edge. */
	kl_canvas_shadow(canvas, page_x, page_y + 0.02f * size, page_width, page_height, 0.06f * size, 0.05f * size, KL_RGBA(0x1f3a66, 36));
	kl_canvas_round(canvas, page_x, page_y, page_width, page_height, 0.06f * size, KL_RGB(0xffffff));
	kl_canvas_round_border(canvas, page_x, page_y, page_width, page_height, 0.06f * size, 1.0f, KL_RGB(0xd6dce6));

	/* The folded corner at the top right. */
	corner[0] = page_x + page_width - fold;
	corner[1] = page_y;
	corner[2] = page_x + page_width;
	corner[3] = page_y + fold;
	corner[4] = page_x + page_width - fold;
	corner[5] = page_y + fold;
	kl_canvas_polygon(canvas, corner, 3, KL_RGB(0xe3e8ef));

	/* The band of the kind's color near the bottom. */
	kl_canvas_round(canvas, page_x + 0.06f * size, page_y + page_height - 0.30f * size, page_width - 0.12f * size, 0.18f * size, 0.04f * size, band);

	/* The label on the band, when the icon is large enough to read it. */
	if (text == NULL ||
	    label == NULL ||
	    label[0] == '\0' ||
	    size < 40.0f)
		return;

	/* The label's size, and its place centred on the band. */
	pixels = (unsigned)(size * 0.13f);
	kl_text_metrics(text, pixels, &line);
	width = kl_text_width(text, label, strlen(label), pixels, 1);
	if ((float)width > page_width - 0.16f * size)
		return;

	/* Draws the label in white, its middle on the band's. */
	label_x = (int)(page_x + (page_width - (float)width) * 0.5f);
	baseline = (int)(page_y + page_height - 0.21f * size + (float)line.ascent * 0.5f - 1.0f);
	(void)kl_text_draw(text, canvas, label_x, baseline, label, strlen(label), pixels, 1, KL_RGB(0xffffff));
}

/*
 * Draws a tag's dot: a circle of its color with a light rim.
 */
void
kl_icon_tag(
	struct kl_canvas *canvas,
	float cx,
	float cy,
	float radius,
	kl_color color)
{
	/* The rim, then the dot inside it. */
	kl_canvas_circle(canvas, cx, cy, radius + 1.0f, KL_RGBA(0xffffff, 200));
	kl_canvas_circle(canvas, cx, cy, radius, color);
}

/* Draws joined line segments through points given as fractions of the icon's box. */
static void
icons_polyline(
	struct kl_canvas *canvas,
	float x,
	float y,
	float size,
	const float *points,
	int count,
	float thickness,
	kl_color color)
{
	int index;

	/* Each segment between two consecutive points. */
	for (index = 0; index + 1 < count; index++) {
		icons_segment(canvas, x, y, size, points[2 * index], points[2 * index + 1], points[2 * index + 2], points[2 * index + 3], thickness, color);
	}
}

/* Draws one line between two points given as fractions of the icon's box. */
static void
icons_segment(
	struct kl_canvas *canvas,
	float x,
	float y,
	float size,
	float x0,
	float y0,
	float x1,
	float y1,
	float thickness,
	kl_color color)
{
	/* The points in the canvas's pixels. */
	kl_canvas_line(canvas, x + x0 * size, y + y0 * size, x + x1 * size, y + y1 * size, thickness, color);
}

/* Draws the outline of a rounded rectangle given in fractions of the icon's box. */
static void
icons_frame(
	struct kl_canvas *canvas,
	float x,
	float y,
	float size,
	float left,
	float top,
	float width,
	float height,
	float radius,
	float thickness,
	kl_color color)
{
	/* The rectangle in the canvas's pixels, the stroke centred on its edge. */
	kl_canvas_round_border(canvas, x + left * size - thickness * 0.5f, y + top * size - thickness * 0.5f, width * size + thickness, height * size + thickness, radius * size + thickness * 0.5f, thickness, color);
}
