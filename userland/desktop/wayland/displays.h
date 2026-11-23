/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arithmetic and the file of several displays shown at once
 * (displays.c, ws113-p004b; the design is plan/ws113/phase004/phase.md,
 * "p004b").
 *
 * The compositor shows every connected display in one of two modes
 * (D-MODES): extended, where each display shows its own part of one
 * logical plane, or mirror, where every display shows the same desktop.
 * In the extended mode each display is a rectangle of the plane: signed
 * integer coordinates, half-open (a display at x 0 of width 1920 covers
 * x 0 .. 1919), no two overlapping, and all of them joined by edges they
 * share.  A display connected while the compositor runs goes right of the
 * rightmost one (D-HOTPLUG).  In the mirror mode a display of another
 * size shows the desktop fitted whole into it, centred, with black bars
 * (D01).
 *
 * The choice is kept in ~/.config/keiland/displays.conf (D-STORE), key=value
 * lines:
 *
 *   version=1
 *   mode=extended | mirror
 *   anchor=KEY                the display the desktop is anchored on (mirror)
 *   place=KEY X Y             a display's place in the extended mode (the
 *                             key may hold spaces; X and Y are the last
 *                             two words)
 *   brightness=N              the built-in panel's light, 0 to 100 (ws113-p005)
 *   off=KEY                   a display the user turned off in the extended mode
 *                             (ws113-p014; the mirror shows it still, and it is
 *                             shown when no other display is connected)
 *   rotate=KEY N              a display's rotation, clockwise in degrees: 90,
 *                             180 or 270 (WS217; the key may hold spaces, N is
 *                             the last word; 0 is not written)
 *   resolution=ID W H MHZ     a monitor's resolution and refresh (mHz) the user
 *                             kept (the ID may hold spaces; the last three words
 *                             are the numbers); the line written last is the
 *                             last, and the oldest goes when there are too many
 *   scale=ID N                a monitor's scale the user chose, in percent (100
 *                             to 300, a step of scale.h; the ID may hold spaces, N is the
 *                             last word); a monitor left to the default has no
 *                             line, and the oldest line goes when there are too
 *                             many
 *
 * KEY is a display's persistent connector key (D-ID A2, the display's name,
 * "zedbsd-port-v1:pci:0000:00:02.0:hdmi:B").  ID is a monitor's identity
 * (resolution.h: "edid-v1:...", made from its EDID, or the connector's key
 * for a display without one), so a monitor keeps its resolution on another
 * connector (and its scale); the places, the angles and the displays turned off stay by
 * connector.  Lines of other keys are left for later versions; a file of
 * another version is not read.
 *
 * Nothing here knows the server or Vulkan, so the host tests run it alone.
 */

#ifndef KWL_DISPLAYS_H
#define KWL_DISPLAYS_H

#include <stddef.h>
#include <stdint.h>

/* The modes (D-MODES). */
#define KWL_DISPLAYS_EXTENDED	0U
#define KWL_DISPLAYS_MIRROR	1U

/* The longest key a place keeps, with its terminator, and how many places the file keeps. */
#define KWL_DISPLAYS_KEY	64U
#define KWL_DISPLAYS_PLACES	16U

/* The version of the file this compositor writes and reads. */
#define KWL_DISPLAYS_VERSION	1U

/*
 * The farthest a display's place may be from the origin, either way: a
 * plane far larger than any desk of displays, whose edges no sum of an
 * origin and a size can carry past the range of an int32_t.
 */
#define KWL_DISPLAYS_LIMIT	(1 << 20)

/* A display's rectangle of the logical plane (or of an image). */
struct kwl_display_rect {
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
};

/* A display's rotation (90, 180 or 270), by its key (WS217). */
struct kwl_display_turn {
	char key[KWL_DISPLAYS_KEY];
	unsigned rotation;
};

/* A monitor's resolution and refresh (mHz), by its identity. */
struct kwl_display_resolution {
	char key[KWL_DISPLAYS_KEY];
	uint32_t width;
	uint32_t height;
	uint32_t refresh_mhz;
};

/* A monitor's scale the user chose (120ths, scale.h), by its identity. */
struct kwl_display_scale {
	char key[KWL_DISPLAYS_KEY];
	unsigned scale120;
};

/* A display's place in the extended mode, by its key. */
struct kwl_display_place {
	char key[KWL_DISPLAYS_KEY];
	int32_t x;
	int32_t y;
};

/*
 * The choice the file keeps: the mode, the mirror's anchor (empty: none),
 * the built-in panel's light when one was chosen, the places, the
 * displays turned off (ws113-p014), the displays' rotations with the
 * number of rotate lines the reading dropped (WS217: not an angle, a key
 * too long, one too many), the monitors' resolutions, the one written
 * last at the end, and the monitors' scales the same way.
 */
struct kwl_display_config {
	unsigned mode;
	char anchor[KWL_DISPLAYS_KEY];
	unsigned has_brightness;
	unsigned brightness;
	unsigned count;
	struct kwl_display_place places[KWL_DISPLAYS_PLACES];
	unsigned off_count;
	char off[KWL_DISPLAYS_PLACES][KWL_DISPLAYS_KEY];
	unsigned rotation_count;
	struct kwl_display_turn rotations[KWL_DISPLAYS_PLACES];
	unsigned rotation_dropped;
	unsigned resolution_count;
	struct kwl_display_resolution resolutions[KWL_DISPLAYS_PLACES];
	unsigned scale_count;
	struct kwl_display_scale scales[KWL_DISPLAYS_PLACES];
};

void kwl_displays_fit(uint32_t source_width, uint32_t source_height, uint32_t width, uint32_t height, struct kwl_display_rect *fitted);
void kwl_displays_cover(uint32_t source_width, uint32_t source_height, uint32_t width, uint32_t height, float *uv);
int kwl_displays_validate(const struct kwl_display_rect *rects, unsigned count);
void kwl_displays_place_right(const struct kwl_display_rect *rects, unsigned count, int32_t *x, int32_t *y);
unsigned kwl_displays_rearrange(struct kwl_display_rect *rects, unsigned count, unsigned changed);
void kwl_displays_config_init(struct kwl_display_config *config);
int kwl_displays_parse(const char *text, size_t length, struct kwl_display_config *config);
size_t kwl_displays_format(const struct kwl_display_config *config, char *text, size_t size);
int kwl_displays_parse_place(const char *text, char *key, size_t size, int32_t *x, int32_t *y);
int kwl_displays_find(const struct kwl_display_config *config, const char *key);
int kwl_displays_set(struct kwl_display_config *config, const char *key, int32_t x, int32_t y);
int kwl_displays_is_off(const struct kwl_display_config *config, const char *key);
int kwl_displays_set_off(struct kwl_display_config *config, const char *key, unsigned off);
int kwl_displays_parse_rotation(const char *text, char *key, size_t size, unsigned *rotation);
unsigned kwl_displays_rotation(const struct kwl_display_config *config, const char *key);
int kwl_displays_set_rotation(struct kwl_display_config *config, const char *key, unsigned rotation);
int kwl_displays_parse_resolution(const char *text, char *key, size_t size, uint32_t *width, uint32_t *height, uint32_t *refresh_mhz);
int kwl_displays_resolution(const struct kwl_display_config *config, const char *key, uint32_t *width, uint32_t *height, uint32_t *refresh_mhz);
int kwl_displays_set_resolution(struct kwl_display_config *config, const char *key, uint32_t width, uint32_t height, uint32_t refresh_mhz);
void kwl_displays_forget_resolution(struct kwl_display_config *config, const char *key);
int kwl_displays_parse_scale(const char *text, char *key, size_t size, unsigned *scale120);
unsigned kwl_displays_scale(const struct kwl_display_config *config, const char *key);
int kwl_displays_set_scale(struct kwl_display_config *config, const char *key, unsigned scale120);

#endif
