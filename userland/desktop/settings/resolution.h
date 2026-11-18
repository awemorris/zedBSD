/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Display page's resolutions (resolution.c): the sizes a display
 * offers, each once and the larger first, the refreshes of one size, the
 * refresh a new size is asked at, the words beside them (the shape and
 * the refresh), the reason said when none can be chosen, and the seconds
 * a trial has left, from what the compositor tells of the display
 * (libkeiland's kl_system_displays_modes and _resolution).  It knows
 * nothing of the page, so the host tests run it alone.
 */

#ifndef SE_RESOLUTION_H
#define SE_RESOLUTION_H

#include <keiland/keiland.h>

#include <stddef.h>
#include <stdint.h>

/* A size's marks: one of its refreshes is the display's recommended mode, or the mode shown now. */
#define SE_RESOLUTION_NATIVE	0x1U
#define SE_RESOLUTION_CURRENT	0x2U

/*
 * The reasons the page says below a display's sizes (none, or the first
 * that applies in this order): the display offers one mode, it is not the
 * display the desktop is shown on, another display is shown beside it, or
 * a game is shown.
 */
#define SE_RESOLUTION_REASON_NONE	0U
#define SE_RESOLUTION_REASON_FIXED	1U
#define SE_RESOLUTION_REASON_ANCHOR	2U
#define SE_RESOLUTION_REASON_ALONE	3U
#define SE_RESOLUTION_REASON_GAME	4U

/* The room of a shape's words ("16:10") and of a refresh's ("59.94 Hz"), with the NUL. */
#define SE_RESOLUTION_ASPECT_MAX	8U
#define SE_RESOLUTION_HERTZ_MAX		16U

/* One size a display offers: its width and height in pixels and its marks (SE_RESOLUTION_*). */
struct se_resolution_size {
	uint32_t width;
	uint32_t height;
	unsigned flags;
};

unsigned se_resolution_sizes(const struct kl_display_mode *modes, size_t count, struct se_resolution_size *sizes, unsigned capacity);
unsigned se_resolution_refreshes(const struct kl_display_mode *modes, size_t count, uint32_t width, uint32_t height, uint32_t *refreshes, unsigned capacity);
uint32_t se_resolution_refresh_for(const struct kl_display_mode *modes, size_t count, uint32_t width, uint32_t height, uint32_t shown_mhz);
int se_resolution_marked(const struct kl_display_mode *modes, size_t count, unsigned flag, struct kl_display_mode *mode);
void se_resolution_aspect(uint32_t width, uint32_t height, char *text, size_t size);
void se_resolution_hertz(uint32_t refresh_mhz, char *text, size_t size);
unsigned se_resolution_reason(unsigned why);
unsigned se_resolution_seconds(uint32_t remaining_ms);

#endif
