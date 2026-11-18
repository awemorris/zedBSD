/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The line pictures of Settings' pages: Wi-Fi's waves, a globe, a
 * speaker, a lock and the others of the list of pages, drawn with the
 * canvas's lines, rings and polygons in a square box, in the stroke the
 * file manager's icons use.
 *
 * Every coordinate below is a fraction of the box, so that a picture
 * draws at any size.
 */

#include "settings.h"

#include <math.h>

/* The stroke of every line, as a fraction of the box, and the thinnest it gets. */
#define GLYPHS_STROKE		0.085f
#define GLYPHS_STROKE_MIN	1.2f

/* How many straight pieces an arc is drawn with. */
#define GLYPHS_ARC_STEPS	24

/* Half a turn. */
#define GLYPHS_PI		3.14159265f

/*
 * One picture's place and pen: the box's corner and side, the stroke,
 * and the colour.  It lives for one call of se_glyph_draw.
 */
struct glyph_pen {
	struct kl_canvas *canvas;
	float x;
	float y;
	float size;
	float thickness;
	kl_color color;
};

static void glyph_segment(const struct glyph_pen *pen, float x0, float y0, float x1, float y1);
static void glyph_polyline(const struct glyph_pen *pen, const float *points, int count);
static void glyph_frame(const struct glyph_pen *pen, float left, float top, float width, float height, float radius);
static void glyph_circle(const struct glyph_pen *pen, float cx, float cy, float radius);
static void glyph_dot(const struct glyph_pen *pen, float cx, float cy, float radius);
static void glyph_arc(const struct glyph_pen *pen, float cx, float cy, float radius, float from, float to);
static void glyph_ellipse(const struct glyph_pen *pen, float cx, float cy, float rx, float ry);
static void glyph_refresh(const struct glyph_pen *pen);

/*
 * Draws a page's line picture in a square box of a size at (x, y).
 */
void
se_glyph_draw(
	struct kl_canvas *canvas,
	unsigned glyph,
	float x,
	float y,
	float size,
	kl_color color)
{
	static const float bluetooth[] = { 0.30f, 0.32f, 0.70f, 0.68f, 0.50f, 0.86f, 0.50f, 0.14f, 0.70f, 0.32f, 0.30f, 0.68f };
	static const float shield[] = { 0.50f, 0.10f, 0.82f, 0.22f, 0.80f, 0.50f, 0.66f, 0.74f, 0.50f, 0.90f, 0.34f, 0.74f, 0.20f, 0.50f, 0.18f, 0.22f, 0.50f, 0.10f };
	static const float mountains[] = { 0.18f, 0.72f, 0.40f, 0.50f, 0.56f, 0.64f, 0.66f, 0.56f, 0.82f, 0.72f };
	static const float bell[] = { 0.22f, 0.72f, 0.30f, 0.62f, 0.30f, 0.42f, 0.36f, 0.28f, 0.50f, 0.22f, 0.64f, 0.28f, 0.70f, 0.42f, 0.70f, 0.62f, 0.78f, 0.72f, 0.22f, 0.72f };
	static const float speaker[] = { 0.14f, 0.40f, 0.30f, 0.40f, 0.50f, 0.22f, 0.50f, 0.78f, 0.30f, 0.60f, 0.14f, 0.60f, 0.14f, 0.40f };
	static const float chevron[] = { 0.40f, 0.28f, 0.60f, 0.50f, 0.40f, 0.72f };
	struct glyph_pen pen;
	int row;
	int column;

	/* The pen: the box and a stroke in proportion to it. */
	pen.canvas = canvas;
	pen.x = x;
	pen.y = y;
	pen.size = size;
	pen.color = color;
	pen.thickness = size * GLYPHS_STROKE;
	if (pen.thickness < GLYPHS_STROKE_MIN)
		pen.thickness = GLYPHS_STROKE_MIN;

	/* Each picture's lines. */
	switch (glyph) {
	case SE_GLYPH_GRID:
		/* Four tiles. */
		glyph_frame(&pen, 0.14f, 0.14f, 0.30f, 0.30f, 0.07f);
		glyph_frame(&pen, 0.56f, 0.14f, 0.30f, 0.30f, 0.07f);
		glyph_frame(&pen, 0.14f, 0.56f, 0.30f, 0.30f, 0.07f);
		glyph_frame(&pen, 0.56f, 0.56f, 0.30f, 0.30f, 0.07f);
		break;
	case SE_GLYPH_WIFI:
		/* Three waves over a dot. */
		glyph_arc(&pen, 0.50f, 0.80f, 0.62f, -GLYPHS_PI / 4.0f, GLYPHS_PI / 4.0f);
		glyph_arc(&pen, 0.50f, 0.80f, 0.42f, -GLYPHS_PI / 4.0f, GLYPHS_PI / 4.0f);
		glyph_arc(&pen, 0.50f, 0.80f, 0.22f, -GLYPHS_PI / 4.0f, GLYPHS_PI / 4.0f);
		glyph_dot(&pen, 0.50f, 0.80f, 0.07f);
		break;
	case SE_GLYPH_ETHERNET:
		/* A computer on a wire: a screen and its base. */
		glyph_frame(&pen, 0.20f, 0.20f, 0.60f, 0.42f, 0.06f);
		glyph_segment(&pen, 0.10f, 0.76f, 0.90f, 0.76f);
		break;
	case SE_GLYPH_BLUETOOTH:
		/* The rune. */
		glyph_polyline(&pen, bluetooth, 6);
		break;
	case SE_GLYPH_SHIELD:
		/* A shield. */
		glyph_polyline(&pen, shield, 9);
		break;
	case SE_GLYPH_GLOBE:
		/* A globe: its rim, a meridian, the equator and two parallels. */
		glyph_circle(&pen, 0.50f, 0.50f, 0.38f);
		glyph_ellipse(&pen, 0.50f, 0.50f, 0.16f, 0.38f);
		glyph_segment(&pen, 0.12f, 0.50f, 0.88f, 0.50f);
		glyph_segment(&pen, 0.18f, 0.32f, 0.82f, 0.32f);
		glyph_segment(&pen, 0.18f, 0.68f, 0.82f, 0.68f);
		break;
	case SE_GLYPH_PALETTE:
		/* A palette with three dabs of paint. */
		glyph_circle(&pen, 0.50f, 0.50f, 0.38f);
		glyph_dot(&pen, 0.36f, 0.40f, 0.06f);
		glyph_dot(&pen, 0.56f, 0.32f, 0.06f);
		glyph_dot(&pen, 0.68f, 0.50f, 0.06f);
		glyph_circle(&pen, 0.44f, 0.66f, 0.07f);
		break;
	case SE_GLYPH_PICTURE:
		/* A framed landscape with the sun. */
		glyph_frame(&pen, 0.12f, 0.20f, 0.76f, 0.60f, 0.08f);
		glyph_polyline(&pen, mountains, 5);
		glyph_dot(&pen, 0.66f, 0.36f, 0.06f);
		break;
	case SE_GLYPH_BELL:
		/* A bell and its clapper. */
		glyph_polyline(&pen, bell, 10);
		glyph_segment(&pen, 0.44f, 0.84f, 0.56f, 0.84f);
		glyph_dot(&pen, 0.50f, 0.16f, 0.05f);
		break;
	case SE_GLYPH_SPEAKER:
		/* A speaker and two waves. */
		glyph_polyline(&pen, speaker, 7);
		glyph_arc(&pen, 0.52f, 0.50f, 0.16f, GLYPHS_PI / 4.0f, 3.0f * GLYPHS_PI / 4.0f);
		glyph_arc(&pen, 0.52f, 0.50f, 0.32f, GLYPHS_PI / 4.0f, 3.0f * GLYPHS_PI / 4.0f);
		break;
	case SE_GLYPH_MONITOR:
		/* A screen on its stand. */
		glyph_frame(&pen, 0.10f, 0.16f, 0.80f, 0.54f, 0.08f);
		glyph_segment(&pen, 0.50f, 0.70f, 0.50f, 0.82f);
		glyph_segment(&pen, 0.34f, 0.84f, 0.66f, 0.84f);
		break;
	case SE_GLYPH_DISK:
		/* A drive with its light. */
		glyph_frame(&pen, 0.12f, 0.30f, 0.76f, 0.40f, 0.08f);
		glyph_segment(&pen, 0.24f, 0.50f, 0.50f, 0.50f);
		glyph_dot(&pen, 0.72f, 0.50f, 0.05f);
		break;
	case SE_GLYPH_BATTERY:
		/* A battery, half full. */
		glyph_frame(&pen, 0.10f, 0.30f, 0.70f, 0.40f, 0.08f);
		glyph_frame(&pen, 0.86f, 0.43f, 0.03f, 0.14f, 0.01f);
		kl_canvas_round(canvas, x + 0.19f * size, y + 0.39f * size, 0.38f * size, 0.22f * size, 0.03f * size, color);
		break;
	case SE_GLYPH_KEYBOARD:
		/* A keyboard: two rows of keys and the space bar. */
		glyph_frame(&pen, 0.08f, 0.26f, 0.84f, 0.48f, 0.08f);
		for (row = 0; row < 2; row++) {
			for (column = 0; column < 5; column++)
				glyph_dot(&pen, 0.22f + 0.14f * (float)column, 0.40f + 0.13f * (float)row, 0.035f);
		}

		/* The space bar. */
		glyph_segment(&pen, 0.32f, 0.64f, 0.68f, 0.64f);
		break;
	case SE_GLYPH_MOUSE:
		/* A mouse and the line between its buttons. */
		glyph_frame(&pen, 0.28f, 0.12f, 0.44f, 0.76f, 0.22f);
		glyph_segment(&pen, 0.50f, 0.14f, 0.50f, 0.36f);
		break;
	case SE_GLYPH_TOUCHPAD:
		/* A touchpad and its two buttons. */
		glyph_frame(&pen, 0.14f, 0.18f, 0.72f, 0.64f, 0.10f);
		glyph_segment(&pen, 0.16f, 0.64f, 0.84f, 0.64f);
		glyph_segment(&pen, 0.50f, 0.64f, 0.50f, 0.80f);
		break;
	case SE_GLYPH_PRINTER:
		/* A printer with its paper in and out. */
		glyph_frame(&pen, 0.28f, 0.14f, 0.44f, 0.20f, 0.03f);
		glyph_frame(&pen, 0.12f, 0.36f, 0.76f, 0.32f, 0.06f);
		glyph_frame(&pen, 0.28f, 0.58f, 0.44f, 0.28f, 0.03f);
		break;
	case SE_GLYPH_SHARE:
		/* Three nodes and the lines between them. */
		glyph_segment(&pen, 0.34f, 0.46f, 0.64f, 0.30f);
		glyph_segment(&pen, 0.34f, 0.54f, 0.64f, 0.70f);
		glyph_circle(&pen, 0.26f, 0.50f, 0.09f);
		glyph_circle(&pen, 0.72f, 0.26f, 0.09f);
		glyph_circle(&pen, 0.72f, 0.74f, 0.09f);
		break;
	case SE_GLYPH_PEOPLE:
		/* Two people, one behind the other. */
		glyph_circle(&pen, 0.40f, 0.34f, 0.12f);
		glyph_arc(&pen, 0.40f, 0.86f, 0.26f, -GLYPHS_PI / 2.0f, GLYPHS_PI / 2.0f);
		glyph_circle(&pen, 0.70f, 0.38f, 0.09f);
		glyph_arc(&pen, 0.72f, 0.84f, 0.18f, 0.0f, GLYPHS_PI / 2.0f);
		break;
	case SE_GLYPH_EYE:
		/* An eye. */
		glyph_ellipse(&pen, 0.50f, 0.50f, 0.38f, 0.22f);
		glyph_dot(&pen, 0.50f, 0.50f, 0.10f);
		break;
	case SE_GLYPH_LOCK:
		/* A padlock and its keyhole. */
		glyph_frame(&pen, 0.22f, 0.44f, 0.56f, 0.42f, 0.08f);
		glyph_arc(&pen, 0.50f, 0.40f, 0.17f, -GLYPHS_PI / 2.0f, GLYPHS_PI / 2.0f);
		glyph_segment(&pen, 0.33f, 0.40f, 0.33f, 0.44f);
		glyph_segment(&pen, 0.67f, 0.40f, 0.67f, 0.44f);
		glyph_dot(&pen, 0.50f, 0.64f, 0.05f);
		break;
	case SE_GLYPH_PERSON:
		/* A person with open arms. */
		glyph_dot(&pen, 0.50f, 0.18f, 0.08f);
		glyph_segment(&pen, 0.20f, 0.36f, 0.80f, 0.36f);
		glyph_segment(&pen, 0.50f, 0.36f, 0.50f, 0.60f);
		glyph_segment(&pen, 0.50f, 0.60f, 0.34f, 0.86f);
		glyph_segment(&pen, 0.50f, 0.60f, 0.66f, 0.86f);
		break;
	case SE_GLYPH_REFRESH:
		/* A turning arrow. */
		glyph_refresh(&pen);
		break;
	case SE_GLYPH_INFO:
		/* An i in a circle. */
		glyph_circle(&pen, 0.50f, 0.50f, 0.38f);
		glyph_dot(&pen, 0.50f, 0.32f, 0.05f);
		glyph_segment(&pen, 0.50f, 0.46f, 0.50f, 0.70f);
		break;
	case SE_GLYPH_CHEVRON:
		/* A chevron pointing right. */
		glyph_polyline(&pen, chevron, 3);
		break;
	default:
		break;
	}
}

/* Draws one line between two points given as fractions of the box. */
static void
glyph_segment(
	const struct glyph_pen *pen,
	float x0,
	float y0,
	float x1,
	float y1)
{
	/* The points in the canvas's pixels. */
	kl_canvas_line(pen->canvas, pen->x + x0 * pen->size, pen->y + y0 * pen->size, pen->x + x1 * pen->size, pen->y + y1 * pen->size, pen->thickness, pen->color);
}

/* Draws the lines joining a run of points (x, y pairs) given as fractions of the box. */
static void
glyph_polyline(
	const struct glyph_pen *pen,
	const float *points,
	int count)
{
	int index;

	/* Each segment between two consecutive points. */
	for (index = 0; index + 1 < count; index++)
		glyph_segment(pen, points[2 * index], points[2 * index + 1], points[2 * index + 2], points[2 * index + 3]);
}

/* Draws the outline of a rounded rectangle given in fractions of the box. */
static void
glyph_frame(
	const struct glyph_pen *pen,
	float left,
	float top,
	float width,
	float height,
	float radius)
{
	float thickness;

	/* The rectangle in the canvas's pixels, the stroke centred on its edge. */
	thickness = pen->thickness;
	kl_canvas_round_border(pen->canvas, pen->x + left * pen->size - thickness * 0.5f, pen->y + top * pen->size - thickness * 0.5f, width * pen->size + thickness, height * pen->size + thickness, radius * pen->size + thickness * 0.5f, thickness, pen->color);
}

/* Draws the outline of a circle given in fractions of the box, the stroke centred on it. */
static void
glyph_circle(
	const struct glyph_pen *pen,
	float cx,
	float cy,
	float radius)
{
	/* A whole ring. */
	kl_canvas_ring(pen->canvas, pen->x + cx * pen->size, pen->y + cy * pen->size, radius * pen->size + pen->thickness * 0.5f, pen->thickness, 1.0f, pen->color);
}

/* Draws a filled dot given in fractions of the box. */
static void
glyph_dot(
	const struct glyph_pen *pen,
	float cx,
	float cy,
	float radius)
{
	/* A filled circle. */
	kl_canvas_circle(pen->canvas, pen->x + cx * pen->size, pen->y + cy * pen->size, radius * pen->size, pen->color);
}

/*
 * Draws an arc of a circle given in fractions of the box, between two
 * angles measured clockwise from twelve o'clock, as one band (so that a
 * translucent colour is laid once) with round ends.
 */
static void
glyph_arc(
	const struct glyph_pen *pen,
	float cx,
	float cy,
	float radius,
	float from,
	float to)
{
	float points[2 * (2 * (GLYPHS_ARC_STEPS + 1))];
	float centre_x;
	float centre_y;
	float outer;
	float inner;
	float angle;
	int step;
	int count;

	/* The circle in the canvas's pixels, and the band's two edges. */
	centre_x = pen->x + cx * pen->size;
	centre_y = pen->y + cy * pen->size;
	outer = radius * pen->size + pen->thickness * 0.5f;
	inner = radius * pen->size - pen->thickness * 0.5f;

	/* The outer edge from the first angle to the last. */
	count = 0;
	for (step = 0; step <= GLYPHS_ARC_STEPS; step++) {
		angle = from + (to - from) * (float)step / (float)GLYPHS_ARC_STEPS;
		points[2 * count] = centre_x + outer * sinf(angle);
		points[2 * count + 1] = centre_y - outer * cosf(angle);
		count++;
	}

	/* The inner edge back. */
	for (step = GLYPHS_ARC_STEPS; step >= 0; step--) {
		angle = from + (to - from) * (float)step / (float)GLYPHS_ARC_STEPS;
		points[2 * count] = centre_x + inner * sinf(angle);
		points[2 * count + 1] = centre_y - inner * cosf(angle);
		count++;
	}

	/* The band, then its round ends. */
	kl_canvas_polygon(pen->canvas, points, count, pen->color);
	kl_canvas_circle(pen->canvas, centre_x + radius * pen->size * sinf(from), centre_y - radius * pen->size * cosf(from), pen->thickness * 0.5f, pen->color);
	kl_canvas_circle(pen->canvas, centre_x + radius * pen->size * sinf(to), centre_y - radius * pen->size * cosf(to), pen->thickness * 0.5f, pen->color);
}

/* Draws the outline of an ellipse given in fractions of the box, as a run of short lines. */
static void
glyph_ellipse(
	const struct glyph_pen *pen,
	float cx,
	float cy,
	float rx,
	float ry)
{
	float angle;
	float next;
	int step;

	/* Each piece of the ellipse, around the whole turn. */
	for (step = 0; step < GLYPHS_ARC_STEPS; step++) {
		angle = 2.0f * GLYPHS_PI * (float)step / (float)GLYPHS_ARC_STEPS;
		next = 2.0f * GLYPHS_PI * (float)(step + 1) / (float)GLYPHS_ARC_STEPS;
		glyph_segment(pen, cx + rx * sinf(angle), cy - ry * cosf(angle), cx + rx * sinf(next), cy - ry * cosf(next));
	}
}

/* Draws the turning arrow: most of a circle, and the arrow's head at its end. */
static void
glyph_refresh(
	const struct glyph_pen *pen)
{
	float end;
	float tip_x;
	float tip_y;
	float along_x;
	float along_y;
	float out_x;
	float out_y;

	/* Most of a circle, open at its upper left. */
	end = 1.7f * GLYPHS_PI;
	glyph_arc(pen, 0.50f, 0.50f, 0.32f, 0.2f * GLYPHS_PI, end);

	/* The arc's end, the direction it runs there, and the direction out from the centre. */
	tip_x = 0.50f + 0.32f * sinf(end);
	tip_y = 0.50f - 0.32f * cosf(end);
	along_x = cosf(end);
	along_y = sinf(end);
	out_x = sinf(end);
	out_y = -cosf(end);

	/* The head's two wings, behind the end on either side of the arc. */
	glyph_segment(pen, tip_x, tip_y, tip_x - 0.14f * along_x + 0.11f * out_x, tip_y - 0.14f * along_y + 0.11f * out_y);
	glyph_segment(pen, tip_x, tip_y, tip_x - 0.14f * along_x - 0.11f * out_x, tip_y - 0.14f * along_y - 0.11f * out_y);
}
