/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The icons the compositor draws for the titlebar's controls (WS070 p009,
 * plan/ws070/titlebar-design.md section 7): back, forward, home, search,
 * the views, and the other roles; and the pictures on App Home's tiles
 * (ws035-p123): Files, Notes, Terminal, PDF Viewer, Browser, the model
 * viewer, Gears, the X terminal, Lock Screen, Log Out, Text Editor and
 * Settings.  They are drawn
 * here as line art, not taken from any icon set or font.
 *
 * Each icon is a few strokes, arcs, rings, dots and rounded boxes (filled
 * or outlined) on a grid of 24 units, and a hole may be cut out of what
 * they cover.  kwl_icon_raster turns one into a square of coverage at a
 * size in pixels by measuring, for each pixel's centre, how far it is from
 * the nearest part: a stroke covers the pixel as far as the pixel lies
 * within half its width, which gives smooth edges without an outline
 * rasterizer.  glass.c places the results in the glyph atlas when the look
 * opens.
 *
 * An application's picture is shown on a tile of its own (ws128-p012, the
 * user's montage 4): a square with rounded corners in three diagonal bands
 * of two pastel-leaning colours with a light stripe across the middle one,
 * the picture cut out of it so that whatever is behind the tile shows
 * through.  kwl_icon_tile draws the whole tile in colour; glass.c keeps the
 * tiles at the sizes the compositor draws them.
 */

#include "icons.h"

#include <math.h>
#include <string.h>

/* The grid the icons are drawn on, and the width of their strokes, in units. */
#define ICON_GRID		24.0f
#define ICON_STROKE		1.8f

/* The narrowest line of an application's picture, in pixels. */
#define ICON_APP_MIN_STROKE	1.6f

/* The most parts one icon has. */
#define ICON_PARTS		16

/*
 * A part whose kind carries this bit is cut out of what the parts before it
 * cover, instead of covering (ws128-p012: the applications' white shapes
 * with their details knocked out to the coloured ground).
 */
#define ICON_CUT		0x100U

/*
 * An application's tile (its corner radius is GLASS_ICON_TILE_RADIUS): its
 * picture's side as a part of its side, the subsamples a side of a pixel its
 * colour is averaged over, and the light stripe across its middle band
 * (where along the diagonal, half its width, and how far towards white).
 */
#define ICON_TILE_PICTURE	0.66f
#define ICON_TILE_SAMPLES	4U
#define ICON_TILE_BANDS		3U
#define ICON_TILE_STRIPE	0.58f
#define ICON_TILE_STRIPE_HALF	0.035f
#define ICON_TILE_STRIPE_LIGHT	0.2f

/* Degrees in a turn, and radians in a degree. */
#define ICON_TURN		360.0f
#define ICON_RADIANS		0.017453293f

/*
 * The kinds of part an icon is made of: a stroke between two points, a
 * ring (a stroked circle), a dot (a filled circle), a filled box with
 * rounded corners, the outline of such a box (a frame), a stroked arc of
 * a circle, a hole: a filled circle cut out of everything else the
 * icon covers, and a filled triangle.  Any kind with ICON_CUT is cut out
 * of what the parts before it cover; the parts are applied in order.
 */
enum icon_kind {
	ICON_END,
	ICON_SEGMENT,
	ICON_RING,
	ICON_DOT,
	ICON_BOX,
	ICON_FRAME,
	ICON_ARC,
	ICON_HOLE,
	ICON_TRIANGLE
};

/*
 * One part of an icon, in grid units: a segment's two ends (a, b, c, d)
 * and its own width (e, or the icons' stroke when 0); a ring's, a dot's or
 * a hole's centre and radius (a, b, c); a box's or a frame's corners and
 * corner radius (a, b, c, d, e); an arc's centre and radius (a, b, c), the
 * angle it starts at and the angle it sweeps through (d, e), in degrees
 * clockwise from the right (y grows downwards); a triangle's three
 * corners (a, b), (c, d) and (e, f).
 */
struct icon_part {
	unsigned kind;
	float a;
	float b;
	float c;
	float d;
	float e;
	float f;
};

/*
 * The application IDs of the windows whose mark is a picture (ws035-p124):
 * the ID each program gives its windows (an X11 window's is its class) and
 * the picture.
 */
struct icon_app_id {
	const char *app_id;
	unsigned icon;
};

/*
 * The two colours (0xRRGGBB) of an application's tile: the light one of the
 * upper right band and the deep one of the lower left; the middle band is
 * half way between them.
 */
struct icon_bands {
	uint32_t light;
	uint32_t deep;
};

/* The parts of each icon, in the order of enum glass_icon, each list ended by ICON_END. */
static const struct icon_part icon_parts[GLASS_ICON_COUNT][ICON_PARTS] = {
	/* Back: a chevron pointing left. */
	{
		{ ICON_SEGMENT, 15.0f, 5.0f, 8.0f, 12.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 8.0f, 12.0f, 15.0f, 19.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Forward: a chevron pointing right. */
	{
		{ ICON_SEGMENT, 9.0f, 5.0f, 16.0f, 12.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 16.0f, 12.0f, 9.0f, 19.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Up: a chevron pointing up. */
	{
		{ ICON_SEGMENT, 5.0f, 15.0f, 12.0f, 8.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 12.0f, 8.0f, 19.0f, 15.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Home: a roof over a house. */
	{
		{ ICON_SEGMENT, 3.5f, 11.5f, 12.0f, 4.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 12.0f, 4.0f, 20.5f, 11.5f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 6.0f, 9.5f, 6.0f, 20.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 6.0f, 20.0f, 18.0f, 20.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 18.0f, 20.0f, 18.0f, 9.5f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 10.5f, 20.0f, 10.5f, 15.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 10.5f, 15.0f, 13.5f, 15.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 13.5f, 15.0f, 13.5f, 20.0f, 0.0f, 0.0f }
	},
	/* Search: a magnifier. */
	{
		{ ICON_RING, 10.0f, 10.0f, 6.0f, 0.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 14.5f, 14.5f, 20.0f, 20.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Icons (grid): four rounded squares. */
	{
		{ ICON_BOX, 4.0f, 4.0f, 10.8f, 10.8f, 1.6f, 0.0f },
		{ ICON_BOX, 13.2f, 4.0f, 20.0f, 10.8f, 1.6f, 0.0f },
		{ ICON_BOX, 4.0f, 13.2f, 10.8f, 20.0f, 1.6f, 0.0f },
		{ ICON_BOX, 13.2f, 13.2f, 20.0f, 20.0f, 1.6f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* List: three dots and three lines. */
	{
		{ ICON_DOT, 5.0f, 6.0f, 1.4f, 0.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 9.0f, 6.0f, 20.0f, 6.0f, 0.0f, 0.0f },
		{ ICON_DOT, 5.0f, 12.0f, 1.4f, 0.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 9.0f, 12.0f, 20.0f, 12.0f, 0.0f, 0.0f },
		{ ICON_DOT, 5.0f, 18.0f, 1.4f, 0.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 9.0f, 18.0f, 20.0f, 18.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Columns: a frame cut in three. */
	{
		{ ICON_SEGMENT, 3.5f, 5.0f, 20.5f, 5.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 20.5f, 5.0f, 20.5f, 19.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 20.5f, 19.0f, 3.5f, 19.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 3.5f, 19.0f, 3.5f, 5.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 9.2f, 5.0f, 9.2f, 19.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 14.8f, 5.0f, 14.8f, 19.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Sort: three lines, shorter and shorter. */
	{
		{ ICON_SEGMENT, 4.0f, 6.0f, 20.0f, 6.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 4.0f, 12.0f, 15.0f, 12.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 4.0f, 18.0f, 10.0f, 18.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Filter: three centred lines, narrower and narrower. */
	{
		{ ICON_SEGMENT, 4.0f, 6.0f, 20.0f, 6.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 7.5f, 12.0f, 16.5f, 12.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 10.5f, 18.0f, 13.5f, 18.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Sidebar: a frame with a panel on the left. */
	{
		{ ICON_SEGMENT, 3.5f, 5.0f, 20.5f, 5.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 20.5f, 5.0f, 20.5f, 19.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 20.5f, 19.0f, 3.5f, 19.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 3.5f, 19.0f, 3.5f, 5.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 9.0f, 5.0f, 9.0f, 19.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Preview: a frame with a panel on the right. */
	{
		{ ICON_SEGMENT, 3.5f, 5.0f, 20.5f, 5.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 20.5f, 5.0f, 20.5f, 19.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 20.5f, 19.0f, 3.5f, 19.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 3.5f, 19.0f, 3.5f, 5.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 15.0f, 5.0f, 15.0f, 19.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Plus. */
	{
		{ ICON_SEGMENT, 12.0f, 5.0f, 12.0f, 19.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 5.0f, 12.0f, 19.0f, 12.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Close: a cross. */
	{
		{ ICON_SEGMENT, 7.0f, 7.0f, 17.0f, 17.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 17.0f, 7.0f, 7.0f, 17.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Overflow: three dots. */
	{
		{ ICON_DOT, 6.0f, 12.0f, 1.9f, 0.0f, 0.0f, 0.0f },
		{ ICON_DOT, 12.0f, 12.0f, 1.9f, 0.0f, 0.0f, 0.0f },
		{ ICON_DOT, 18.0f, 12.0f, 1.9f, 0.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Volume 0: a speaker (ws100-p004). */
	{
		{ ICON_SEGMENT, 3.5f, 9.5f, 3.5f, 14.5f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 3.5f, 9.5f, 7.0f, 9.5f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 3.5f, 14.5f, 7.0f, 14.5f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 7.0f, 9.5f, 12.0f, 5.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 7.0f, 14.5f, 12.0f, 19.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 12.0f, 5.0f, 12.0f, 19.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Volume 1: a speaker and one wave. */
	{
		{ ICON_SEGMENT, 3.5f, 9.5f, 3.5f, 14.5f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 3.5f, 9.5f, 7.0f, 9.5f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 3.5f, 14.5f, 7.0f, 14.5f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 7.0f, 9.5f, 12.0f, 5.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 7.0f, 14.5f, 12.0f, 19.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 12.0f, 5.0f, 12.0f, 19.0f, 0.0f, 0.0f },
		{ ICON_ARC, 12.0f, 12.0f, 3.5f, 315.0f, 90.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Volume 2: a speaker and two waves. */
	{
		{ ICON_SEGMENT, 3.5f, 9.5f, 3.5f, 14.5f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 3.5f, 9.5f, 7.0f, 9.5f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 3.5f, 14.5f, 7.0f, 14.5f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 7.0f, 9.5f, 12.0f, 5.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 7.0f, 14.5f, 12.0f, 19.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 12.0f, 5.0f, 12.0f, 19.0f, 0.0f, 0.0f },
		{ ICON_ARC, 12.0f, 12.0f, 3.5f, 315.0f, 90.0f, 0.0f },
		{ ICON_ARC, 12.0f, 12.0f, 6.5f, 315.0f, 90.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Volume 3: a speaker and three waves. */
	{
		{ ICON_SEGMENT, 3.5f, 9.5f, 3.5f, 14.5f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 3.5f, 9.5f, 7.0f, 9.5f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 3.5f, 14.5f, 7.0f, 14.5f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 7.0f, 9.5f, 12.0f, 5.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 7.0f, 14.5f, 12.0f, 19.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 12.0f, 5.0f, 12.0f, 19.0f, 0.0f, 0.0f },
		{ ICON_ARC, 12.0f, 12.0f, 3.5f, 315.0f, 90.0f, 0.0f },
		{ ICON_ARC, 12.0f, 12.0f, 6.5f, 315.0f, 90.0f, 0.0f },
		{ ICON_ARC, 12.0f, 12.0f, 9.5f, 315.0f, 90.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Volume muted: a speaker and a cross. */
	{
		{ ICON_SEGMENT, 3.5f, 9.5f, 3.5f, 14.5f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 3.5f, 9.5f, 7.0f, 9.5f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 3.5f, 14.5f, 7.0f, 14.5f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 7.0f, 9.5f, 12.0f, 5.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 7.0f, 14.5f, 12.0f, 19.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 12.0f, 5.0f, 12.0f, 19.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 15.0f, 9.0f, 21.0f, 15.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 21.0f, 9.0f, 15.0f, 15.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/*
	 * USB (the removable media in the system bar, 2026-10-05 user
	 * decision): the trident, a stem with an arrow on top and a dot below,
	 * a branch to a circle on the left and one to a square on the right.
	 */
	{
		{ ICON_SEGMENT, 12.0f, 4.5f, 12.0f, 18.5f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 9.2f, 7.6f, 12.0f, 4.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 12.0f, 4.0f, 14.8f, 7.6f, 0.0f, 0.0f },
		{ ICON_DOT, 12.0f, 19.8f, 2.4f, 0.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 12.0f, 16.0f, 6.8f, 12.4f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 6.8f, 12.4f, 6.8f, 10.6f, 0.0f, 0.0f },
		{ ICON_DOT, 6.8f, 9.4f, 1.9f, 0.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 12.0f, 13.6f, 17.2f, 10.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 17.2f, 10.0f, 17.2f, 8.6f, 0.0f, 0.0f },
		{ ICON_BOX, 15.4f, 5.6f, 19.0f, 9.2f, 0.4f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Wi-Fi: the dot alone, the arcs about the dot's centre (each arc two side by side, for a wider line). */
	{
		{ ICON_DOT, 12.0f, 19.0f, 2.0f, 0.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Wi-Fi: the dot and the nearest arc, the arcs about the dot's centre (each arc two side by side, for a wider line). */
	{
		{ ICON_DOT, 12.0f, 19.0f, 2.0f, 0.0f, 0.0f, 0.0f },
		{ ICON_ARC, 12.0f, 19.0f, 5.6f, 225.0f, 90.0f, 0.0f },
		{ ICON_ARC, 12.0f, 19.0f, 6.3f, 225.0f, 90.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Wi-Fi: the dot and two arcs, the arcs about the dot's centre (each arc two side by side, for a wider line). */
	{
		{ ICON_DOT, 12.0f, 19.0f, 2.0f, 0.0f, 0.0f, 0.0f },
		{ ICON_ARC, 12.0f, 19.0f, 5.6f, 225.0f, 90.0f, 0.0f },
		{ ICON_ARC, 12.0f, 19.0f, 6.3f, 225.0f, 90.0f, 0.0f },
		{ ICON_ARC, 12.0f, 19.0f, 9.6f, 225.0f, 90.0f, 0.0f },
		{ ICON_ARC, 12.0f, 19.0f, 10.3f, 225.0f, 90.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Wi-Fi: the dot and all three arcs, the arcs about the dot's centre (each arc two side by side, for a wider line). */
	{
		{ ICON_DOT, 12.0f, 19.0f, 2.0f, 0.0f, 0.0f, 0.0f },
		{ ICON_ARC, 12.0f, 19.0f, 5.6f, 225.0f, 90.0f, 0.0f },
		{ ICON_ARC, 12.0f, 19.0f, 6.3f, 225.0f, 90.0f, 0.0f },
		{ ICON_ARC, 12.0f, 19.0f, 9.6f, 225.0f, 90.0f, 0.0f },
		{ ICON_ARC, 12.0f, 19.0f, 10.3f, 225.0f, 90.0f, 0.0f },
		{ ICON_ARC, 12.0f, 19.0f, 13.6f, 225.0f, 90.0f, 0.0f },
		{ ICON_ARC, 12.0f, 19.0f, 14.3f, 225.0f, 90.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Bluetooth (ws143-p006): the rune, a stem with two arrowheads on its right and the cross that runs back through it. */
	{
		{ ICON_SEGMENT, 12.0f, 3.5f, 12.0f, 20.5f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 12.0f, 3.5f, 16.5f, 8.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 16.5f, 8.0f, 7.5f, 16.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 12.0f, 20.5f, 16.5f, 16.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 16.5f, 16.0f, 7.5f, 8.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* The left desktop: a cat sitting, seen from the front, its two ears, its body, and its tail curled up at the right. */
	{
		{ ICON_DOT, 11.0f, 8.8f, 4.4f, 0.0f, 0.0f, 0.0f },
		{ ICON_TRIANGLE, 6.9f, 7.0f, 7.4f, 2.6f, 10.2f, 4.8f },
		{ ICON_TRIANGLE, 15.1f, 7.0f, 14.6f, 2.6f, 11.8f, 4.8f },
		{ ICON_BOX, 6.6f, 12.0f, 15.4f, 21.4f, 4.2f, 0.0f },
		{ ICON_SEGMENT, 13.0f, 20.4f, 18.6f, 20.0f, 2.2f, 0.0f },
		{ ICON_SEGMENT, 18.6f, 20.0f, 19.8f, 14.6f, 2.2f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* The middle desktop: a bird perched, facing right, its round body, its head and beak, its tail and its legs. */
	{
		{ ICON_DOT, 10.8f, 13.2f, 5.8f, 0.0f, 0.0f, 0.0f },
		{ ICON_DOT, 15.8f, 8.6f, 3.4f, 0.0f, 0.0f, 0.0f },
		{ ICON_TRIANGLE, 18.6f, 7.2f, 21.8f, 8.8f, 18.6f, 10.2f },
		{ ICON_TRIANGLE, 5.8f, 11.4f, 2.4f, 9.0f, 3.0f, 15.8f },
		{ ICON_SEGMENT, 9.6f, 18.4f, 9.0f, 21.4f, 1.4f, 0.0f },
		{ ICON_SEGMENT, 12.6f, 18.4f, 13.2f, 21.4f, 1.4f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* The right desktop: a rabbit sitting, facing right, its two long ears, its head, its body, its tail and its feet. */
	{
		{ ICON_DOT, 10.6f, 15.4f, 5.6f, 0.0f, 0.0f, 0.0f },
		{ ICON_DOT, 15.8f, 10.4f, 3.6f, 0.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 14.8f, 7.8f, 13.6f, 3.4f, 2.4f, 0.0f },
		{ ICON_SEGMENT, 17.2f, 7.8f, 18.4f, 3.4f, 2.4f, 0.0f },
		{ ICON_DOT, 5.2f, 16.4f, 1.9f, 0.0f, 0.0f, 0.0f },
		{ ICON_BOX, 8.6f, 19.4f, 17.4f, 21.6f, 1.1f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Files: a folder, its tab on the upper left, a line knocked out under the tab. */
	{
		{ ICON_BOX, 3.0f, 5.0f, 11.0f, 9.0f, 1.6f, 0.0f },
		{ ICON_BOX, 3.0f, 7.0f, 21.0f, 19.0f, 2.2f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 3.0f, 10.2f, 21.0f, 10.2f, 1.4f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Notes: a notepad with two written lines, a pencil writing across it. */
	{
		{ ICON_BOX, 5.0f, 3.5f, 16.5f, 20.5f, 2.2f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 8.0f, 8.0f, 13.0f, 8.0f, 1.6f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 8.0f, 11.5f, 11.5f, 11.5f, 1.6f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 21.0f, 5.5f, 12.2f, 14.3f, 6.0f, 0.0f },
		{ ICON_SEGMENT, 20.4f, 6.1f, 13.4f, 13.1f, 3.0f, 0.0f },
		{ ICON_TRIANGLE, 12.4f, 12.1f, 14.4f, 14.1f, 11.2f, 15.3f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Terminal: a screen with a prompt and a cursor knocked out. */
	{
		{ ICON_BOX, 3.0f, 4.5f, 21.0f, 19.5f, 2.8f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 7.0f, 9.0f, 10.0f, 12.0f, 1.9f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 10.0f, 12.0f, 7.0f, 15.0f, 1.9f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 12.5f, 15.0f, 17.0f, 15.0f, 1.9f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* PDF Viewer: a page with its corner folded and three lines of text knocked out. */
	{
		{ ICON_BOX, 5.0f, 3.0f, 19.0f, 21.0f, 2.0f, 0.0f },
		{ ICON_TRIANGLE | ICON_CUT, 14.0f, 1.5f, 22.0f, 1.5f, 22.0f, 9.5f },
		{ ICON_TRIANGLE, 14.3f, 3.0f, 14.3f, 8.2f, 19.5f, 8.2f },
		{ ICON_SEGMENT | ICON_CUT, 8.3f, 12.0f, 15.7f, 12.0f, 1.7f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 8.3f, 15.2f, 15.7f, 15.2f, 1.7f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 8.3f, 18.4f, 12.5f, 18.4f, 1.7f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Image Viewer: a picture, two hills and a sun knocked out. */
	{
		{ ICON_BOX, 3.0f, 4.5f, 21.0f, 19.5f, 2.8f, 0.0f },
		{ ICON_TRIANGLE | ICON_CUT, 5.2f, 17.3f, 10.2f, 9.8f, 15.2f, 17.3f },
		{ ICON_TRIANGLE | ICON_CUT, 11.6f, 17.3f, 15.0f, 12.6f, 18.8f, 17.3f },
		{ ICON_DOT | ICON_CUT, 16.0f, 8.6f, 1.8f, 0.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Browser: a globe, its equator and meridians knocked out. */
	{
		{ ICON_DOT, 12.0f, 12.0f, 8.8f, 0.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 3.0f, 12.0f, 21.0f, 12.0f, 1.5f, 0.0f },
		{ ICON_ARC | ICON_CUT, 4.97f, 12.0f, 11.03f, -50.4f, 100.8f, 0.0f },
		{ ICON_ARC | ICON_CUT, 19.03f, 12.0f, 11.03f, 129.6f, 100.8f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 12.0f, 3.0f, 12.0f, 21.0f, 1.5f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Model viewer: a cube seen from above a corner, its edges knocked out. */
	{
		{ ICON_TRIANGLE, 12.0f, 3.0f, 19.8f, 7.5f, 19.8f, 16.5f },
		{ ICON_TRIANGLE, 12.0f, 3.0f, 19.8f, 16.5f, 12.0f, 21.0f },
		{ ICON_TRIANGLE, 12.0f, 3.0f, 12.0f, 21.0f, 4.2f, 16.5f },
		{ ICON_TRIANGLE, 12.0f, 3.0f, 4.2f, 16.5f, 4.2f, 7.5f },
		{ ICON_SEGMENT | ICON_CUT, 4.2f, 7.5f, 12.0f, 12.0f, 1.5f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 19.8f, 7.5f, 12.0f, 12.0f, 1.5f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 12.0f, 12.0f, 12.0f, 21.0f, 1.5f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Gears: a gear wheel, eight teeth round a disc with a hole in its middle. */
	{
		{ ICON_DOT, 12.0f, 12.0f, 6.2f, 0.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 17.8f, 12.0f, 20.4f, 12.0f, 3.6f, 0.0f },
		{ ICON_SEGMENT, 16.1f, 16.1f, 17.94f, 17.94f, 3.6f, 0.0f },
		{ ICON_SEGMENT, 12.0f, 17.8f, 12.0f, 20.4f, 3.6f, 0.0f },
		{ ICON_SEGMENT, 7.9f, 16.1f, 6.06f, 17.94f, 3.6f, 0.0f },
		{ ICON_SEGMENT, 6.2f, 12.0f, 3.6f, 12.0f, 3.6f, 0.0f },
		{ ICON_SEGMENT, 7.9f, 7.9f, 6.06f, 6.06f, 3.6f, 0.0f },
		{ ICON_SEGMENT, 12.0f, 6.2f, 12.0f, 3.6f, 3.6f, 0.0f },
		{ ICON_SEGMENT, 16.1f, 7.9f, 17.94f, 6.06f, 3.6f, 0.0f },
		{ ICON_DOT | ICON_CUT, 12.0f, 12.0f, 2.6f, 0.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* X terminal: a screen with an X and a cursor knocked out. */
	{
		{ ICON_BOX, 3.0f, 4.5f, 21.0f, 19.5f, 2.8f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 7.0f, 9.0f, 11.0f, 15.0f, 1.9f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 11.0f, 9.0f, 7.0f, 15.0f, 1.9f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 13.5f, 15.0f, 17.0f, 15.0f, 1.9f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Lock Screen: a padlock with its keyhole knocked out. */
	{
		{ ICON_ARC, 12.0f, 8.6f, 4.1f, 180.0f, 180.0f, 0.0f },
		{ ICON_ARC, 12.0f, 8.6f, 3.4f, 180.0f, 180.0f, 0.0f },
		{ ICON_SEGMENT, 7.9f, 8.6f, 7.9f, 11.0f, 1.8f, 0.0f },
		{ ICON_SEGMENT, 16.1f, 8.6f, 16.1f, 11.0f, 1.8f, 0.0f },
		{ ICON_BOX, 5.0f, 10.5f, 19.0f, 20.5f, 2.5f, 0.0f },
		{ ICON_DOT | ICON_CUT, 12.0f, 14.6f, 1.6f, 0.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 12.0f, 14.6f, 12.0f, 17.6f, 1.4f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/*
	 * Power Off (ws099-p037, BUG-235): the power sign, a ring open at the
	 * top (two arcs side by side, for a wider line) and a bar down into
	 * the opening.
	 */
	{
		{ ICON_ARC, 12.0f, 13.0f, 7.2f, 300.0f, 300.0f, 0.0f },
		{ ICON_ARC, 12.0f, 13.0f, 6.4f, 300.0f, 300.0f, 0.0f },
		{ ICON_SEGMENT, 12.0f, 3.6f, 12.0f, 11.6f, 2.4f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Text Editor (WS092): a capital T and a text cursor beside it. */
	{
		{ ICON_SEGMENT, 6.0f, 5.5f, 17.0f, 5.5f, 3.2f, 0.0f },
		{ ICON_SEGMENT, 11.5f, 5.5f, 11.5f, 18.5f, 3.2f, 0.0f },
		{ ICON_SEGMENT, 17.8f, 13.0f, 17.8f, 19.5f, 1.7f, 0.0f },
		{ ICON_SEGMENT, 16.3f, 13.0f, 19.3f, 13.0f, 1.4f, 0.0f },
		{ ICON_SEGMENT, 16.3f, 19.5f, 19.3f, 19.5f, 1.4f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Settings (WS089): three sliders, each knob ringed by the ground. */
	{
		{ ICON_SEGMENT, 5.0f, 7.0f, 19.0f, 7.0f, 2.0f, 0.0f },
		{ ICON_SEGMENT, 5.0f, 12.0f, 19.0f, 12.0f, 2.0f, 0.0f },
		{ ICON_SEGMENT, 5.0f, 17.0f, 19.0f, 17.0f, 2.0f, 0.0f },
		{ ICON_DOT | ICON_CUT, 9.0f, 7.0f, 3.4f, 0.0f, 0.0f, 0.0f },
		{ ICON_DOT, 9.0f, 7.0f, 2.4f, 0.0f, 0.0f, 0.0f },
		{ ICON_DOT | ICON_CUT, 15.0f, 12.0f, 3.4f, 0.0f, 0.0f, 0.0f },
		{ ICON_DOT, 15.0f, 12.0f, 2.4f, 0.0f, 0.0f, 0.0f },
		{ ICON_DOT | ICON_CUT, 11.0f, 17.0f, 3.4f, 0.0f, 0.0f, 0.0f },
		{ ICON_DOT, 11.0f, 17.0f, 2.4f, 0.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Video Player: a screen with a play sign knocked out. */
	{
		{ ICON_BOX, 3.0f, 5.5f, 21.0f, 18.5f, 3.2f, 0.0f },
		{ ICON_TRIANGLE | ICON_CUT, 10.0f, 8.8f, 10.0f, 15.2f, 15.6f, 12.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Phone: a handset, its handle a wide band (arcs side by side) curving round the lower left, the earpiece and the mouthpiece turned in at its ends. */
	{
		{ ICON_ARC, 19.4f, 4.4f, 11.8f, 98.0f, 74.0f, 0.0f },
		{ ICON_ARC, 19.4f, 4.4f, 12.73f, 98.0f, 74.0f, 0.0f },
		{ ICON_ARC, 19.4f, 4.4f, 13.67f, 98.0f, 74.0f, 0.0f },
		{ ICON_ARC, 19.4f, 4.4f, 14.6f, 98.0f, 74.0f, 0.0f },
		{ ICON_SEGMENT, 17.66f, 16.78f, 18.18f, 13.11f, 6.4f, 0.0f },
		{ ICON_SEGMENT, 7.02f, 6.14f, 10.69f, 5.62f, 6.4f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Calendar: a page of a calendar, its binding rings on top, a line under the month and the days knocked out. */
	{
		{ ICON_BOX, 4.0f, 6.0f, 20.0f, 20.5f, 2.6f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 4.0f, 10.3f, 20.0f, 10.3f, 1.3f, 0.0f },
		{ ICON_DOT | ICON_CUT, 8.5f, 13.8f, 1.15f, 0.0f, 0.0f, 0.0f },
		{ ICON_DOT | ICON_CUT, 12.0f, 13.8f, 1.15f, 0.0f, 0.0f, 0.0f },
		{ ICON_DOT | ICON_CUT, 15.5f, 13.8f, 1.15f, 0.0f, 0.0f, 0.0f },
		{ ICON_DOT | ICON_CUT, 8.5f, 17.2f, 1.15f, 0.0f, 0.0f, 0.0f },
		{ ICON_DOT | ICON_CUT, 12.0f, 17.2f, 1.15f, 0.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 8.5f, 3.0f, 8.5f, 7.6f, 3.4f, 0.0f },
		{ ICON_SEGMENT, 8.5f, 3.4f, 8.5f, 7.6f, 1.8f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 15.5f, 3.0f, 15.5f, 7.6f, 3.4f, 0.0f },
		{ ICON_SEGMENT, 15.5f, 3.4f, 15.5f, 7.6f, 1.8f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Mail: an envelope, its flap knocked out. */
	{
		{ ICON_BOX, 3.0f, 5.5f, 21.0f, 18.5f, 2.2f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 4.2f, 7.2f, 12.0f, 12.8f, 1.7f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 12.0f, 12.8f, 19.8f, 7.2f, 1.7f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* System Monitor: a display on its stand, a pulse of activity knocked out across it. */
	{
		{ ICON_BOX, 3.0f, 4.0f, 21.0f, 16.0f, 2.4f, 0.0f },
		{ ICON_SEGMENT, 12.0f, 16.0f, 12.0f, 19.5f, 2.2f, 0.0f },
		{ ICON_SEGMENT, 8.0f, 19.8f, 16.0f, 19.8f, 2.2f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 6.0f, 10.5f, 8.8f, 10.5f, 1.6f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 8.8f, 10.5f, 10.4f, 7.2f, 1.6f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 10.4f, 7.2f, 13.2f, 13.6f, 1.6f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 13.2f, 13.6f, 14.8f, 10.5f, 1.6f, 0.0f },
		{ ICON_SEGMENT | ICON_CUT, 14.8f, 10.5f, 18.0f, 10.5f, 1.6f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Music: two notes joined by a beam. */
	{
		{ ICON_DOT, 7.6f, 17.6f, 3.0f, 0.0f, 0.0f, 0.0f },
		{ ICON_DOT, 16.6f, 15.6f, 3.0f, 0.0f, 0.0f, 0.0f },
		{ ICON_SEGMENT, 9.6f, 17.4f, 9.6f, 6.6f, 2.0f, 0.0f },
		{ ICON_SEGMENT, 18.6f, 15.4f, 18.6f, 4.6f, 2.0f, 0.0f },
		{ ICON_SEGMENT, 9.6f, 6.4f, 18.6f, 4.4f, 3.2f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	},
	/* Photos: two photos stacked, a hill and a sun knocked out of the front one. */
	{
		{ ICON_BOX, 7.0f, 3.0f, 21.5f, 15.5f, 2.2f, 0.0f },
		{ ICON_BOX | ICON_CUT, 1.4f, 6.4f, 18.6f, 22.0f, 3.0f, 0.0f },
		{ ICON_BOX, 2.5f, 7.5f, 17.5f, 20.8f, 2.4f, 0.0f },
		{ ICON_TRIANGLE | ICON_CUT, 4.4f, 18.8f, 9.4f, 12.4f, 14.4f, 18.8f },
		{ ICON_TRIANGLE | ICON_CUT, 10.6f, 18.8f, 13.2f, 15.4f, 15.8f, 18.8f },
		{ ICON_DOT | ICON_CUT, 13.4f, 11.0f, 1.5f, 0.0f, 0.0f, 0.0f },
		{ ICON_END, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
	}
};

/*
 * The names App Home's list (/etc/keiland/apps.conf) gives the pictures
 * by, in the order of enum glass_icon from GLASS_ICON_FIRST_APP.
 */
static const char *const icon_app_names[GLASS_ICON_APPS] = {
	"files",
	"notes",
	"terminal",
	"pdf",
	"image",
	"browser",
	"model",
	"gears",
	"xterm",
	"lock",
	"power",
	"text",
	"settings",
	"video",
	"phone",
	"calendar",
	"mail",
	"monitor",
	"music",
	"photos"
};

/*
 * The colours of each application's tile (the user's montage 4, ws128-p012:
 * Files the off-yellow of a folder, Calendar blue; Music yamabuki,
 * #F8B500 between its two bands, the UAT of 2026-10-07), in the order of
 * icon_app_names.
 */
static const struct icon_bands icon_app_bands[GLASS_ICON_APPS] = {
	{ 0xffd86bU, 0xf2b53aU },
	{ 0xffb38aU, 0xff8a65U },
	{ 0x5b6b8cU, 0x3b4660U },
	{ 0xff8a80U, 0xf0605aU },
	{ 0x7ee0b5U, 0x3cc48dU },
	{ 0x7fd8f0U, 0x3ab3d8U },
	{ 0xffc09fU, 0xf59a73U },
	{ 0xe6b48aU, 0xc98a55U },
	{ 0x8a93b8U, 0x626c96U },
	{ 0xc7cdd8U, 0xa0a8b8U },
	{ 0xc7cdd8U, 0xa0a8b8U },
	{ 0x9be3e0U, 0x4cc4c0U },
	{ 0xb8c4d6U, 0x8e9bb3U },
	{ 0xb39dffU, 0x8c6cf2U },
	{ 0x7ee89aU, 0x3fcb6bU },
	{ 0x7fb3ffU, 0x4a8bf5U },
	{ 0xc49bffU, 0x9c6cf0U },
	{ 0xff9ec4U, 0xf06a9bU },
	{ 0xffc933U, 0xf2a900U },
	{ 0xffd27aU, 0xf59e2bU }
};

/* The known programs' windows, found by their exact application ID. */
static const struct icon_app_id icon_app_ids[] = {
	{ "files", GLASS_ICON_APP_FILES },
	{ "notes", GLASS_ICON_APP_NOTES },
	{ "terminal", GLASS_ICON_APP_TERMINAL },
	{ "pdfviewer", GLASS_ICON_APP_PDF },
	{ "imageview", GLASS_ICON_APP_IMAGE },
	{ "browser", GLASS_ICON_APP_BROWSER },
	{ "mview", GLASS_ICON_APP_MODEL },
	{ "Gears", GLASS_ICON_APP_GEARS },
	{ "XTerminal", GLASS_ICON_APP_XTERM },
	{ "textedit", GLASS_ICON_APP_TEXT },
	{ "settings", GLASS_ICON_APP_SETTINGS },
	{ "videoplayer", GLASS_ICON_APP_VIDEO },
	{ "phone", GLASS_ICON_APP_PHONE },
	{ "calendar", GLASS_ICON_APP_CALENDAR },
	{ "mailer", GLASS_ICON_APP_MAIL },
	{ "monitor", GLASS_ICON_APP_MONITOR },
	{ "music", GLASS_ICON_APP_MUSIC },
	{ "photos", GLASS_ICON_APP_PHOTOS }
};

static float icon_distance(const struct icon_part *part, float x, float y);
static float icon_segment_distance(float x, float y, float x0, float y0, float x1, float y1);
static float icon_arc_distance(const struct icon_part *part, float x, float y);
static float icon_box_distance(float x, float y, float x0, float y0, float x1, float y1, float radius);
static float icon_triangle_distance(const struct icon_part *part, float x, float y);
static float icon_clamp(float value);
static void icon_tile_colour(const struct icon_bands *bands, unsigned pixels, unsigned column, unsigned row, float *colour);
static void icon_channels(uint32_t rgb, float *channels);

/*
 * Draws an icon (GLASS_ICON_*) into a square of coverage of a size in
 * pixels, a byte a pixel (255 fully covered), rows stride bytes apart.
 */
void
kwl_icon_raster(
	unsigned icon,
	unsigned pixels,
	uint8_t *coverage,
	size_t stride)
{
	const struct icon_part *part;
	float scale;
	float half;
	float part_half;
	float distance;
	float amount;
	float best;
	float x;
	float y;
	unsigned column;
	unsigned row;
	unsigned index;
	unsigned kind;

	/* An icon the table does not have is left empty. */
	for (row = 0; row < pixels; row++)
		memset(coverage + (size_t)row * stride, 0, pixels);
	if (icon >= GLASS_ICON_COUNT || pixels == 0U)
		return;

	/* Pixels a unit, and half a stroke's width in pixels. */
	scale = (float)pixels / ICON_GRID;
	half = ICON_STROKE * scale * 0.5f;

	/*
	 * An application's picture drawn small (a window's mark, ws035-p124)
	 * keeps its lines at least ICON_APP_MIN_STROKE pixels wide, so they
	 * stay white on the coloured square instead of fading to grey.
	 */
	if (icon >= GLASS_ICON_FIRST_APP && half < ICON_APP_MIN_STROKE * 0.5f)
		half = ICON_APP_MIN_STROKE * 0.5f;

	/* Each pixel: how much of it the nearest part covers. */
	for (row = 0; row < pixels; row++) {
		for (column = 0; column < pixels; column++) {
			x = ((float)column + 0.5f) / scale;
			y = ((float)row + 0.5f) / scale;
			best = 0.0f;

			/* Each part of the icon, in order: a covering part adds, a cut part takes away. */
			for (index = 0; index < ICON_PARTS; index++) {
				part = &icon_parts[icon][index];
				if (part->kind == ICON_END)
					break;
				kind = part->kind & ~ICON_CUT;
				distance = icon_distance(part, x, y) * scale;

				/* A segment may be wider or narrower than the icons' stroke. */
				part_half = half;
				if (kind == ICON_SEGMENT && part->e > 0.0f)
					part_half = part->e * scale * 0.5f;

				/* A stroke, a ring, a frame or an arc covers within half its width, a dot, a hole, a box or a triangle inside it. */
				if (kind == ICON_SEGMENT ||
				    kind == ICON_RING ||
				    kind == ICON_FRAME ||
				    kind == ICON_ARC) {
					amount = icon_clamp(part_half - distance + 0.5f);
				} else {
					amount = icon_clamp(0.5f - distance);
				}

				/* A hole, or a part marked to be cut, takes its cover out of what is there so far. */
				if (kind == ICON_HOLE || (part->kind & ICON_CUT) != 0U) {
					best = best * (1.0f - amount);
					continue;
				}

				/* Otherwise the part that covers most wins. */
				if (amount > best)
					best = amount;
			}

			/* The pixel's coverage. */
			coverage[(size_t)row * stride + column] = (uint8_t)(best * 255.0f + 0.5f);
		}
	}
}

/*
 * Draws an application's tile (an icon from GLASS_ICON_FIRST_APP) into a
 * square of a size in pixels, premultiplied 0xAARRGGBB, rows stride
 * pixels apart: the rounded square in its diagonal bands, its picture
 * cut out (transparent).  Any other icon, or a size above
 * GLASS_ICON_TILE_MOST, leaves the square transparent.
 */
void
kwl_icon_tile(
	unsigned icon,
	unsigned pixels,
	uint32_t *argb,
	size_t stride)
{
	static uint8_t picture[GLASS_ICON_TILE_MOST * GLASS_ICON_TILE_MOST];
	const struct icon_bands *bands;
	float colour[3];
	float distance;
	float radius;
	float shape;
	float cut;
	float alpha;
	uint32_t red;
	uint32_t green;
	uint32_t blue;
	uint32_t opacity;
	unsigned picture_pixels;
	unsigned offset;
	unsigned column;
	unsigned row;

	/* The square starts transparent; only an application's picture at a size the buffer holds has a tile. */
	for (row = 0; row < pixels; row++)
		memset(argb + (size_t)row * stride, 0, (size_t)pixels * sizeof(*argb));
	if (icon < GLASS_ICON_FIRST_APP || icon >= GLASS_ICON_COUNT)
		return;
	if (pixels == 0U || pixels > GLASS_ICON_TILE_MOST)
		return;

	/*
	 * The picture, about two thirds of the side and one pixel larger when
	 * that leaves an odd margin, so that it sits in the middle on whole
	 * pixels.
	 */
	picture_pixels = (unsigned)((float)pixels * ICON_TILE_PICTURE + 0.5f);
	if (((pixels - picture_pixels) & 1U) != 0U)
		picture_pixels++;
	offset = (pixels - picture_pixels) / 2U;
	kwl_icon_raster(icon, picture_pixels, picture, picture_pixels);

	/* Each pixel of the tile: its band colour, as much of it as the rounded square covers less the picture's cover. */
	bands = &icon_app_bands[icon - GLASS_ICON_FIRST_APP];
	radius = (float)pixels * GLASS_ICON_TILE_RADIUS;
	for (row = 0; row < pixels; row++) {
		for (column = 0; column < pixels; column++) {
			/* How much of the pixel the rounded square covers; outside it the pixel stays transparent. */
			distance = icon_box_distance((float)column + 0.5f, (float)row + 0.5f, 0.0f, 0.0f, (float)pixels, (float)pixels, radius);
			shape = icon_clamp(0.5f - distance);
			if (shape <= 0.0f)
				continue;

			/* The picture's cover there, cut out of the tile. */
			cut = 0.0f;
			if (column >= offset &&
			    column < offset + picture_pixels &&
			    row >= offset &&
			    row < offset + picture_pixels)
				cut = (float)picture[(row - offset) * picture_pixels + column - offset] / 255.0f;
			alpha = shape * (1.0f - cut);

			/* The bands' colour, premultiplied by what is left. */
			icon_tile_colour(bands, pixels, column, row, colour);
			opacity = (uint32_t)(alpha * 255.0f + 0.5f);
			red = (uint32_t)(colour[0] * alpha * 255.0f + 0.5f);
			green = (uint32_t)(colour[1] * alpha * 255.0f + 0.5f);
			blue = (uint32_t)(colour[2] * alpha * 255.0f + 0.5f);
			argb[(size_t)row * stride + column] = (opacity << 24) | (red << 16) | (green << 8) | blue;
		}
	}
}

/*
 * Finds the App Home picture a name stands for ("files", "notes", ...;
 * see icon_app_names), for a line of App Home's list.
 *
 * Returns the icon (GLASS_ICON_APP_*), or -1 for a name no picture has.
 */
int
kwl_icon_named(
	const char *name)
{
	unsigned index;
	int differs;

	/* Each picture's name, until one is the name asked for. */
	for (index = 0; index < GLASS_ICON_APPS; index++) {
		differs = strcmp(name, icon_app_names[index]);
		if (differs == 0)
			return (int)(GLASS_ICON_FIRST_APP + index);
	}

	/* No picture has that name. */
	return -1;
}

/*
 * Finds the picture of a window's mark from its application ID.
 *
 * Returns the icon (GLASS_ICON_APP_*), or -1 for an ID no picture belongs
 * to.
 */
int
kwl_icon_for_app_id(
	const char *app_id)
{
	unsigned index;
	int differs;

	/* Each known ID, until one is the window's. */
	for (index = 0; index < sizeof(icon_app_ids) / sizeof(icon_app_ids[0]); index++) {
		differs = strcmp(app_id, icon_app_ids[index].app_id);
		if (differs != 0)
			continue;

		/* Succeeded: the window's picture. */
		return (int)icon_app_ids[index].icon;
	}

	/* No picture belongs to that ID. */
	return -1;
}

/* Measures how far a point (in units) is from a part: from a stroke's line, a ring's circle, a frame's outline or an arc, or outside a dot, a hole, a box or a triangle (negative inside). */
static float
icon_distance(
	const struct icon_part *part,
	float x,
	float y)
{
	float distance;
	float dx;
	float dy;
	float from_centre;

	/* No part is as far as can be; the centre's distance serves the ring and the dot. */
	distance = 1000.0f;
	dx = x - part->a;
	dy = y - part->b;
	from_centre = sqrtf(dx * dx + dy * dy);

	/* Each kind of part (a cut part is measured as its kind). */
	switch (part->kind & ~ICON_CUT) {
	case ICON_SEGMENT:
		distance = icon_segment_distance(x, y, part->a, part->b, part->c, part->d);
		break;
	case ICON_RING:
		distance = fabsf(from_centre - part->c);
		break;
	case ICON_DOT:
	case ICON_HOLE:
		distance = from_centre - part->c;
		break;
	case ICON_BOX:
		distance = icon_box_distance(x, y, part->a, part->b, part->c, part->d, part->e);
		break;
	case ICON_FRAME:
		distance = fabsf(icon_box_distance(x, y, part->a, part->b, part->c, part->d, part->e));
		break;
	case ICON_ARC:
		distance = icon_arc_distance(part, x, y);
		break;
	case ICON_TRIANGLE:
		distance = icon_triangle_distance(part, x, y);
		break;
	default:
		break;
	}

	/* Reports the distance. */
	return distance;
}

/* Measures how far a point is from a segment. */
static float
icon_segment_distance(
	float x,
	float y,
	float x0,
	float y0,
	float x1,
	float y1)
{
	float distance;
	float along;
	float length;
	float dx;
	float dy;
	float px;
	float py;

	/* The segment's direction, and where along it the point falls (clamped to its ends). */
	dx = x1 - x0;
	dy = y1 - y0;
	length = dx * dx + dy * dy;
	along = 0.0f;
	if (length > 0.0f)
		along = ((x - x0) * dx + (y - y0) * dy) / length;
	if (along < 0.0f)
		along = 0.0f;
	if (along > 1.0f)
		along = 1.0f;

	/* The distance to that nearest point. */
	px = x - (x0 + along * dx);
	py = y - (y0 + along * dy);
	distance = sqrtf(px * px + py * py);

	/* Reports the distance. */
	return distance;
}

/*
 * Measures how far a point is from an arc: from its circle where the
 * point's angle falls within the arc's sweep, else from the nearer end.
 */
static float
icon_arc_distance(
	const struct icon_part *part,
	float x,
	float y)
{
	float angle;
	float along;
	float start;
	float end;
	float dx;
	float dy;
	float to_start;
	float to_end;

	/* The point's angle about the centre, in degrees, and how far past the arc's start it lies (0 up to a turn). */
	dx = x - part->a;
	dy = y - part->b;
	angle = atan2f(dy, dx) / ICON_RADIANS;
	along = fmodf(angle - part->d, ICON_TURN);
	if (along < 0.0f)
		along += ICON_TURN;

	/* Within the sweep the arc is as far as its circle. */
	if (along <= part->e)
		return fabsf(sqrtf(dx * dx + dy * dy) - part->c);

	/* Outside it, the nearer of the arc's two ends. */
	start = part->d * ICON_RADIANS;
	end = (part->d + part->e) * ICON_RADIANS;
	dx = x - (part->a + part->c * cosf(start));
	dy = y - (part->b + part->c * sinf(start));
	to_start = sqrtf(dx * dx + dy * dy);
	dx = x - (part->a + part->c * cosf(end));
	dy = y - (part->b + part->c * sinf(end));
	to_end = sqrtf(dx * dx + dy * dy);

	/* The start is nearer. */
	if (to_start < to_end)
		return to_start;

	/* The end is nearer, or as near. */
	return to_end;
}

/* Measures the signed distance from a point to a box with rounded corners (negative inside). */
static float
icon_box_distance(
	float x,
	float y,
	float x0,
	float y0,
	float x1,
	float y1,
	float radius)
{
	float cx;
	float cy;
	float qx;
	float qy;
	float outside;
	float inside;

	/* The point folded into the box's first quadrant, from the inner rectangle the corners round off. */
	cx = (x0 + x1) * 0.5f;
	cy = (y0 + y1) * 0.5f;
	qx = fabsf(x - cx) - ((x1 - x0) * 0.5f - radius);
	qy = fabsf(y - cy) - ((y1 - y0) * 0.5f - radius);

	/* Outside: the distance to the inner rectangle's corner; inside: how deep. */
	outside = sqrtf(fmaxf(qx, 0.0f) * fmaxf(qx, 0.0f) + fmaxf(qy, 0.0f) * fmaxf(qy, 0.0f));
	inside = fminf(fmaxf(qx, qy), 0.0f);

	/* Reports the distance, less the corners' radius. */
	return outside + inside - radius;
}

/*
 * Measures the signed distance from a point to a triangle (negative
 * inside): outside, the nearest of its three sides; inside, the same
 * distance made negative.  The corners may go round either way.
 */
static float
icon_triangle_distance(
	const struct icon_part *part,
	float x,
	float y)
{
	float nearest;
	float side;
	float first;
	float second;
	float third;

	/* The nearest of the three sides. */
	nearest = icon_segment_distance(x, y, part->a, part->b, part->c, part->d);
	side = icon_segment_distance(x, y, part->c, part->d, part->e, part->f);
	if (side < nearest)
		nearest = side;
	side = icon_segment_distance(x, y, part->e, part->f, part->a, part->b);
	if (side < nearest)
		nearest = side;

	/* Which side of each side's line the point is on (the cross product). */
	first = (part->c - part->a) * (y - part->b) - (part->d - part->b) * (x - part->a);
	second = (part->e - part->c) * (y - part->d) - (part->f - part->d) * (x - part->c);
	third = (part->a - part->e) * (y - part->f) - (part->b - part->f) * (x - part->e);

	/* Inside: on the same side of all three. */
	if (first >= 0.0f && second >= 0.0f && third >= 0.0f)
		return -nearest;
	if (first <= 0.0f && second <= 0.0f && third <= 0.0f)
		return -nearest;

	/* Outside. */
	return nearest;
}

/* Keeps a coverage between 0 and 1. */
static float
icon_clamp(
	float value)
{
	/* Below and above the range. */
	if (value < 0.0f)
		return 0.0f;
	if (value > 1.0f)
		return 1.0f;

	/* Inside it. */
	return value;
}

/*
 * Gives the colour of a tile's pixel, averaged over a grid of subsamples:
 * the band each falls in along the "/" diagonal (0 at the lower left, 1 at
 * the upper right), the deep colour first, and the light stripe across the
 * middle band.
 */
static void
icon_tile_colour(
	const struct icon_bands *bands,
	unsigned pixels,
	unsigned column,
	unsigned row,
	float *colour)
{
	float steps[ICON_TILE_BANDS][3];
	float light[3];
	float deep[3];
	float sample[3];
	float along;
	float stripe;
	float x;
	float y;
	unsigned across;
	unsigned down;
	unsigned band;
	unsigned channel;

	/* The three bands' colours: deep, half way, light. */
	icon_channels(bands->light, light);
	icon_channels(bands->deep, deep);
	for (channel = 0; channel < 3U; channel++) {
		steps[0][channel] = deep[channel];
		steps[1][channel] = (deep[channel] + light[channel]) * 0.5f;
		steps[2][channel] = light[channel];
		colour[channel] = 0.0f;
	}

	/* Each subsample's band (along lies between 0 and 1 inside the square), the stripe lightening it, summed. */
	for (down = 0; down < ICON_TILE_SAMPLES; down++) {
		for (across = 0; across < ICON_TILE_SAMPLES; across++) {
			x = (float)column + ((float)across + 0.5f) / (float)ICON_TILE_SAMPLES;
			y = (float)row + ((float)down + 0.5f) / (float)ICON_TILE_SAMPLES;
			along = (x + ((float)pixels - y)) / (2.0f * (float)pixels);
			band = (unsigned)(along * (float)ICON_TILE_BANDS);
			if (band >= ICON_TILE_BANDS)
				band = ICON_TILE_BANDS - 1U;
			memcpy(sample, steps[band], sizeof(sample));

			/* The light stripe across the middle band. */
			stripe = fabsf(along - ICON_TILE_STRIPE);
			if (stripe < ICON_TILE_STRIPE_HALF) {
				for (channel = 0; channel < 3U; channel++)
					sample[channel] = sample[channel] * (1.0f - ICON_TILE_STRIPE_LIGHT) + ICON_TILE_STRIPE_LIGHT;
			}

			/* The subsample into the sum. */
			for (channel = 0; channel < 3U; channel++)
				colour[channel] += sample[channel];
		}
	}

	/* The average. */
	for (channel = 0; channel < 3U; channel++)
		colour[channel] /= (float)(ICON_TILE_SAMPLES * ICON_TILE_SAMPLES);
}

/* Splits a colour (0xRRGGBB) into its red, green and blue, 0 to 1. */
static void
icon_channels(
	uint32_t rgb,
	float *channels)
{
	/* Each channel's byte. */
	channels[0] = (float)((rgb >> 16) & 0xffU) / 255.0f;
	channels[1] = (float)((rgb >> 8) & 0xffU) / 255.0f;
	channels[2] = (float)(rgb & 0xffU) / 255.0f;
}
