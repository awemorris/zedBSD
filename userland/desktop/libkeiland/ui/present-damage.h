/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The stale parts of a swapchain's image (ws210-p013 E2): what of the
 * frame changed since the image was last written, as a few rectangles,
 * so that a frame copies into the image it is given only those parts of
 * the staging buffer.  Plain C, so the host tests can run it without
 * Vulkan.
 */

#ifndef KEILAND_UI_PRESENT_DAMAGE_H
#define KEILAND_UI_PRESENT_DAMAGE_H

#include <keiland/keiland.h>

#include <stdint.h>

/* The most rectangles an image's stale parts are kept as. */
#define KEIUI_STALE_RECTS	4U

/* The columns the rectangles' left and right edges are rounded to (64 bytes of a row of 4-byte pixels). */
#define KEIUI_STALE_ALIGN	16

/* The share of the frame (percent) above which the stale parts are written as the whole frame. */
#define KEIUI_STALE_WHOLE_PERCENT	30U

/*
 * An image's stale parts: whether its contents are not known (the whole
 * frame is written, from an undefined layout), and else up to
 * KEIUI_STALE_RECTS rectangles that do not overlap, their edges rounded to
 * KEIUI_STALE_ALIGN columns (none: the image holds the last frame).
 */
struct keiui_stale {
	int whole;
	unsigned count;
	struct kl_rect rects[KEIUI_STALE_RECTS];
};

void keiui_stale_unknown(struct keiui_stale *stale);
void keiui_stale_clear(struct keiui_stale *stale);
void keiui_stale_add(struct keiui_stale *stale, uint32_t width, uint32_t height, const struct kl_rect *part);
unsigned keiui_stale_regions(const struct keiui_stale *stale, uint32_t width, uint32_t height, struct kl_rect *regions);

#endif
