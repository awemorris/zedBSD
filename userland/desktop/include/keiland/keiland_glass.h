/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The glass panels, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_GLASS_H
#define KEILAND_GLASS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct wl_surface;

/*
 * The glass panels.
 *
 * A window whose Vulkan swapchain is see-through
 * (VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) names the parts of itself
 * that stand on the system's frosted glass: cards floating in the window.
 * The compositor draws the glass under them -- the desktop behind, blurred and
 * lightened, a bright rim, the card's shadow --
 * and the window's image over it by its alpha; between the panels the
 * desktop shows as it is.  The window says what its parts are, not how
 * the glass looks.
 *
 * The panels, in the surface's coordinates, take effect with the surface's
 * next commit (a Vulkan present), so they move with the frame drawn for
 * them.  Every call returns 0 or an errno value, and a refused call sends
 * nothing: EINVAL (an empty panel, a radius past the largest, an unknown
 * kind), E2BIG (too many panels).
 */
struct kl_glass;

/* The kind of panel (the only one so far): a card floating in the window. */
#define KL_GLASS_CARD		0U

/* The most panels a surface has, and the largest corner radius. */
#define KL_GLASS_PANELS_MAX	32U
#define KL_GLASS_RADIUS_MAX	64

/*
 * One panel: its rectangle in the surface's coordinates, the radius of
 * its corners and its kind.
 */
struct kl_glass_panel {
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
	int32_t radius;
	unsigned kind;
};

/*
 * Gives a surface its glass, with no panels yet.  Returns NULL with errno
 * set: ENOTSUP for a compositor without glass, ENOMEM.
 */
struct kl_glass *kl_glass_create(struct wl_display *display, struct wl_surface *surface);

/*
 * Sets the surface's panels for its next commit (count 0: none).
 */
int kl_glass_set_panels(struct kl_glass *glass, const struct kl_glass_panel *panels, size_t count);

/*
 * Chooses whether the surface's glass (its panels and its title bar) shows
 * the windows under it blurred (enabled) or only the blurred wallpaper (the
 * default: the compositor draws nothing again for it), from the surface's
 * next commit (KL_VERSION 17).  Returns ENOTSUP when the compositor's
 * glass has no choice.
 */
int kl_glass_set_blur(struct kl_glass *glass, int enabled);

/*
 * Takes the glass away: the surface's next commit shows it without panels.
 */
void kl_glass_destroy(struct kl_glass *glass);

#ifdef __cplusplus
}
#endif

#endif
