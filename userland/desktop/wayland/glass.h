/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The glass look's drawing, shared by glass.c (images, glyphs, shapes) and
 * shell.c (windows, title bars, the system bar).
 */

#ifndef KWL_GLASS_H
#define KWL_GLASS_H

#include "compose.h"
#include "icons.h"

/* The atlas's index of the multiplication sign (the close button). */
#define GLASS_CLOSE_GLYPH	95U

/*
 * The atlas's indices of the check mark and the single right angle quote
 * (the System Menu's checked items and submenu arrows).  A font without
 * them leaves them empty: glass_glyph_advance reports 0.
 */
#define GLASS_CHECK_GLYPH	96U
#define GLASS_ARROW_GLYPH	97U

/* The shapes the panel shader draws (shaders/panel.frag). */
#define MODE_GLASS		0.0f
#define MODE_SHADOW		1.0f
#define MODE_IMAGE		2.0f
#define MODE_SOLID		3.0f
#define MODE_RING		4.0f
#define MODE_TEXT		5.0f
#define MODE_BLUR		6.0f

/*
 * The saturation (the greatest channel less the least) from which a colour
 * keeps its hue in the dark appearance (glass.c, ws089-p017); below it the
 * colour's lightness is turned over.
 */
#define GLASS_DARK_SATURATION	0.25f

/* The corner radius of title bars and bodies. */
#define GLASS_RADIUS		14.0f

/*
 * What an application's cut-out picture shows (glass_draw_app_tile,
 * BUG-237): what was drawn under the tile (a dark ground, where the hole
 * reads as a hole), or the blurred scene the light glass under the tile
 * frosts (a near-white ground, where the plain hole would read as a white
 * picture).
 */
enum glass_hole {
	GLASS_HOLE_GROUND,
	GLASS_HOLE_SCENE,
	GLASS_HOLE_WALLPAPER
};

/*
 * The text sizes: the system bar, the titles, the close sign, App Home's
 * icon letters, its search text, and its clock's time (ws181-p006: the
 * digits and the colon only).
 */
enum glass_size {
	SIZE_BAR,
	SIZE_TITLE,
	SIZE_SIGN,
	SIZE_ICON,
	SIZE_SEARCH,
	SIZE_CLOCK
};

/*
 * The Kei mark's colours: the translucent glass of the boot splash (the
 * greeter and the lock screen), or the deeper and less see-through ones
 * that read on the light system bar (the launcher, ws035-p118).
 */
enum glass_mark_look {
	GLASS_MARK_SPLASH,
	GLASS_MARK_BAR
};

/*
 * One shape for the panel shader, in output pixels: the quad drawn, the
 * rounded box the shader measures from (it may reach past the quad), the
 * part of the image, a color, and how the shape is drawn.  opacity fades the
 * whole shape.  A shape with no image of its own gives the shader the
 * blurred wallpaper.
 */
struct glass_shape {
	float quad[4];
	float box[4];
	float uv[4];
	float color[4];
	float radius;
	float mode;
	float soft;
	float opaque;
	float edge;
	float opacity;
	VkDescriptorSet set;
	/* Nonzero for a shape drawn in its light colours in the dark appearance too (a window of a client that does not know the appearance, panels.c). */
	unsigned light;
	/* Nonzero for glass drawn as the dark appearance's dark glass in either appearance (the system bar, ws099-p034). */
	unsigned dark_glass;
};

void glass_shape_init(struct glass_shape *shape, float x, float y, float width, float height);
void glass_shape_draw(struct kwl_server *server, VkCommandBuffer command, const struct glass_shape *shape);
void glass_draw_solid(struct kwl_server *server, VkCommandBuffer command, float x, float y, float width, float height, float radius, const float *color);
int32_t glass_text_width(struct kwl_server *server, enum glass_size size, const char *text);
void glass_draw_text(struct kwl_server *server, VkCommandBuffer command, enum glass_size size, int32_t x, int32_t baseline, const char *text, int32_t limit, const float *color);
void glass_draw_text_middle(struct kwl_server *server, VkCommandBuffer command, enum glass_size size, int32_t x, int32_t baseline, const char *text, int32_t limit, const float *color);
void glass_draw_glyph(struct kwl_server *server, VkCommandBuffer command, enum glass_size size, unsigned index, int32_t x, int32_t baseline, const float *color);
int32_t glass_glyph_advance(struct kwl_server *server, enum glass_size size, unsigned index);
int glass_large_prepare(struct kwl_server *server, unsigned pixels);
int32_t glass_large_text_width(struct kwl_server *server, const char *text);
void glass_draw_large_text(struct kwl_server *server, VkCommandBuffer command, int32_t x, int32_t baseline, const char *text, const float *color);
void glass_draw_icon(struct kwl_server *server, VkCommandBuffer command, unsigned icon, int32_t x, int32_t y, unsigned pixels, const float *color);
void glass_draw_app_tile(struct kwl_server *server, VkCommandBuffer command, unsigned icon, float x, float y, float pixels, float opacity, float lighten, enum glass_hole hole);
void glass_draw_app_tile_reflection(struct kwl_server *server, VkCommandBuffer command, unsigned icon, float x, float y, float pixels, float height, float opacity);
void glass_draw_mark(struct kwl_server *server, VkCommandBuffer command, int32_t x, int32_t y, unsigned pixels, enum glass_mark_look look, float opacity);
VkDescriptorSet glass_wallpaper_set(struct kwl_server *server);

/*
 * The system bar's colours in the appearance shown (ws099-p034b, the
 * 2026-10-06 user decision): in the dark appearance light ink on the dark
 * glass of p034, in the light appearance the floating title bar's dark ink
 * on its white glass.  The ink, a faint line, a group pill's fill and edge,
 * a lit place, a faint place, and whether the bar is light.
 */
struct glass_bar_colours {
	float ink[4];
	float line[4];
	float fill[4];
	float edge[4];
	float lit[4];
	float faint[4];
	unsigned light;
};

void kwl_glass_bar_colours(struct kwl_server *server, struct glass_bar_colours *colours);

/* The applications' icons in the system bar and their previews (apps-bar.c), drawn with the shell's marks and Wiseview's tiles (shell.c). */
int kwl_apps_bar_draw(struct kwl_server *server, VkCommandBuffer command);
void kwl_apps_bar_draw_popup(struct kwl_server *server, VkCommandBuffer command);
void kwl_glass_draw_app_mark(struct kwl_server *server, VkCommandBuffer command, struct kwl_object *surface, int32_t x, int32_t middle, int32_t size, float alpha);
void kwl_switch_draw(struct kwl_server *server, VkCommandBuffer command);

/* The power dialog over everything but the cursor (power-dialog.c, ws099-p037). */
void kwl_power_dialog_draw(struct kwl_server *server, VkCommandBuffer command);
void kwl_glass_draw_preview(struct kwl_server *server, VkCommandBuffer command, struct kwl_object *surface, int32_t x, int32_t y, int32_t width, int32_t height, int over);

/* The login screen in place of the desktop (greeter.c). */
void kwl_greeter_draw(struct kwl_server *server, VkCommandBuffer command);

/* The network's icon in the system bar and its menu (network.c). */
void kwl_network_draw_icon(struct kwl_server *server, VkCommandBuffer command, int32_t x, const float *ink);
void kwl_network_draw_menu(struct kwl_server *server, VkCommandBuffer command);
void kwl_arrange_draw(struct kwl_server *server, VkCommandBuffer command);
int kwl_arrange_showing(void);
void kwl_volume_draw_icon(struct kwl_server *server, VkCommandBuffer command, int32_t x, const float *ink);
void kwl_volume_draw_popup(struct kwl_server *server, VkCommandBuffer command);

/* App Home under the desktop layer (home.c). */
void kwl_home_draw(struct kwl_server *server, VkCommandBuffer command, float progress);
void kwl_home_draw_head(struct kwl_server *server, VkCommandBuffer command);
void kwl_corner_draw(struct kwl_server *server, VkCommandBuffer command);

/* The notifications' popup over the windows and the bar (notify-popup.c, ws156-p003). */
void kwl_notify_popup_draw(struct kwl_server *server, VkCommandBuffer command);

/* The on-screen keyboard over everything (keyboard.c). */
void kwl_keyboard_draw(struct kwl_server *server, VkCommandBuffer command);

#endif
