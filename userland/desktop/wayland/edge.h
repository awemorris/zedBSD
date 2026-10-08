/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The edges' gestures of the screen (edge.c, WS181, the 2026-10-07 UAT):
 * the swipe up from the bottom edge that opens App Home, the swipe down
 * from the top edge's band that opens Wiseview (a touch only, the band
 * holding its press until it knows), a drag on Home that is mostly down
 * (closing Home) or sideways (the pages), how far a docked title has
 * been pulled out of the system bar, how deep the desktop and Home's
 * content are while Home opens or closes (ws181-p008), and which edge a
 * group of fingers on a touch screen swipes in from (BUG-267).
 *
 * It knows nothing of the server: the caller gives points and distances,
 * and acts on what the rules say (shell.c, home.c).  So the host tests
 * run it alone.
 */

#ifndef KWL_EDGE_H
#define KWL_EDGE_H

#include <stdint.h>

/*
 * The top edge's band: its height, and the launcher's width at the left
 * (the band starts after it; the top-left corner of App Home is inside it).
 * The band ends before the top-right corner of Notes (CORNER_ZONE).
 */
#define KWL_EDGE_BAND			10
#define KWL_EDGE_LAUNCHER_WIDTH		40
#define KWL_EDGE_CORNER			28

/*
 * The band's depth for a finger where the bar holds nothing a finger drags
 * (BUG-270, the 2026-10-08 UAT: a finger swiping in from the top lands 13
 * to 31 pixels down, below KWL_EDGE_BAND): the system bar's whole height
 * (KWL_GLASS_BAR).  Over a docked window's menus, title and buttons the
 * band keeps KWL_EDGE_BAND (a finger there pulls the window or opens a
 * menu); the shell decides which (band_depth).
 */
#define KWL_EDGE_BAND_DEEP		44

/* The bottom edge's strip, where the swipe up to App Home starts. */
#define KWL_EDGE_BOTTOM_HEIGHT		20

/*
 * How far a press in the band moves before it is something: down by
 * KWL_EDGE_BAND_START (more down than across) is Wiseview's swipe, across
 * or up by KWL_EDGE_BAND_SLIP is a press of what is under it after all.
 */
#define KWL_EDGE_BAND_START		12
#define KWL_EDGE_BAND_SLIP		8

/*
 * How long a press in the band rests before it is a long press of what is
 * under it (ws177-p033: the bar's applications show their previews), in
 * milliseconds: a swipe down starts well before.
 */
#define KWL_EDGE_BAND_HOLD_MS		500U

/* Where a press is: nowhere special, the bottom edge's strip, or the top edge's band. */
#define KWL_EDGE_NONE			0U
#define KWL_EDGE_BOTTOM_STRIP		1U
#define KWL_EDGE_TOP_BAND		2U

/* What a press held in the band is after a motion: still waiting, Wiseview's swipe, or the press of what is under it. */
#define KWL_EDGE_BAND_WAIT		0U
#define KWL_EDGE_BAND_WISEVIEW		1U
#define KWL_EDGE_BAND_REPLAY		2U

/* What a drag on Home is: the pages (sideways), closing Home (down), or nothing (up). */
#define KWL_EDGE_DRAG_PAGES		0U
#define KWL_EDGE_DRAG_CLOSE		1U
#define KWL_EDGE_DRAG_NONE		2U

/*
 * The edges a group of fingers on a touch screen swipes in from (BUG-267,
 * the 2026-10-08 UAT: a swipe of two fingers from the edge is the swipe
 * one finger makes there): none, the left side, the right side (both under
 * the system bar: the desktops' swipe), the bottom (App Home's), or the
 * top (Wiseview's from the top band, BUG-270).
 */
#define KWL_EDGE_SIDE_NONE		0U
#define KWL_EDGE_SIDE_LEFT		1U
#define KWL_EDGE_SIDE_RIGHT		2U
#define KWL_EDGE_SIDE_BOTTOM		3U
#define KWL_EDGE_SIDE_TOP		4U

/*
 * The group of fingers: the finger nearest an edge (looked for within
 * KWL_EDGE_GROUP_REACH of each) touches within KWL_EDGE_GROUP_BAND of it
 * (two fingers side by side do not both fit in the one finger's strip),
 * the others anywhere (the 2026-10-08 user's decision), all within
 * KWL_EDGE_GROUP_MS of the first; every finger then moves in from that
 * edge by KWL_EDGE_GROUP_START, more in than across.
 */
#define KWL_EDGE_GROUP_BAND		64
#define KWL_EDGE_GROUP_REACH		192
#define KWL_EDGE_GROUP_MS		150U
#define KWL_EDGE_GROUP_START		12

/* What a finger of the group is after a motion: still waiting, going in, or doing something else. */
#define KWL_EDGE_GROUP_WAIT		0U
#define KWL_EDGE_GROUP_IN		1U
#define KWL_EDGE_GROUP_OTHER		2U

/*
 * App Home's way in and out (ws181-p008, the 2026-10-07 UAT: as on iOS).
 * The desktop layer goes back into the distance: it shrinks about the
 * output's middle to KWL_EDGE_HOME_DESKTOP_DEPTH of its size as Home
 * opens, and fades out between KWL_EDGE_HOME_FADE_START and
 * KWL_EDGE_HOME_FADE_END of the way.  Home's content comes forward from
 * the distance: it grows about the middle from KWL_EDGE_HOME_CONTENT_DEPTH
 * of its size to its own.  Closing is the same way back.
 */
#define KWL_EDGE_HOME_DESKTOP_DEPTH	0.75f
#define KWL_EDGE_HOME_CONTENT_DEPTH	0.85f
#define KWL_EDGE_HOME_FADE_START	0.20f
#define KWL_EDGE_HOME_FADE_END		0.90f

/*
 * Where a layer is drawn at one moment of Home's way in or out: its
 * top-left corner on the output and its scale (a point (px, py) of the
 * layer is drawn at (x + px * scale, y + py * scale)), and its opacity.
 * The caller owns it.
 */
struct kwl_edge_depth {
	float x;
	float y;
	float scale;
	float opacity;
};

unsigned kwl_edge_classify(int32_t x, int32_t y, int32_t width, int32_t height, int touch);
unsigned kwl_edge_classify_band(int32_t x, int32_t y, int32_t width, int32_t height, int32_t depth);
unsigned kwl_edge_band_motion(int32_t dx, int32_t dy);
int kwl_edge_band_held(uint64_t held_ms);
unsigned kwl_edge_drag_axis(int32_t dx, int32_t dy);
int32_t kwl_edge_distance(int32_t dx, int32_t dy);
void kwl_edge_home_desktop(float progress, int32_t width, int32_t height, struct kwl_edge_depth *depth);
void kwl_edge_home_content(float progress, int32_t width, int32_t height, struct kwl_edge_depth *depth);
unsigned kwl_edge_group_side(int32_t x, int32_t y, int32_t width, int32_t height, int32_t top);
int32_t kwl_edge_group_distance(unsigned side, int32_t x, int32_t y, int32_t width, int32_t height, int32_t top);
unsigned kwl_edge_group_motion(unsigned side, int32_t dx, int32_t dy);
void kwl_edge_group_point(unsigned side, int32_t x, int32_t y, int32_t width, int32_t height, int32_t *edge_x, int32_t *edge_y);

#endif
