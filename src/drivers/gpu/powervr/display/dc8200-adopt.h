/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decision whether the display controller's screen, as the loader
 * left it, can be taken over: the controller's identity and its primary
 * planes' settings, read once, against the screen the loader handed over.
 *
 * Only values go in and out, so the decision is the same in the kernel
 * and in a host test.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_DC8200_ADOPT_H
#define KERN_DRIVERS_GPU_POWERVR_DC8200_ADOPT_H

#include <stdint.h>

/* The controller's primary planes. */
#define POWERVR_DC_PLANES	2U

/*
 * The model register's value of the controller the driver knows.
 *
 * Value referenced:
 * Linux torvalds/linux master drivers/gpu/drm/verisilicon/vs_hwdb.c
 * (the chip table's model 0x8200).
 */
#define POWERVR_DC_MODEL	0x8200U

/*
 * The fields of a primary plane's settings the decision reads.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/vs_dc_hw.c (the primary plane's update:
 * format at bit 26, swizzle at 23, tile mode at 17, rotation at 11,
 * scaling at 22; the extended word's compression at bit 1, enable at 13,
 * display at 19; the size's height at bit 15); Linux torvalds/linux master
 * drivers/gpu/drm/verisilicon/vs_primary_plane_regs.h (the commit at bit
 * 12 of the extended word).
 */
#define POWERVR_DC_CONFIG_TILE_SHIFT	17U
#define POWERVR_DC_CONFIG_TILE_MASK	0x1fU
#define POWERVR_DC_CONFIG_ROTATE_SHIFT	11U
#define POWERVR_DC_CONFIG_ROTATE_MASK	0x7U
#define POWERVR_DC_CONFIG_SCALE		(1U << 22)
#define POWERVR_DC_EXTRA_COMPRESSED	(1U << 1)
#define POWERVR_DC_EXTRA_COMMIT		(1U << 12)
#define POWERVR_DC_EXTRA_ENABLED	(1U << 13)
#define POWERVR_DC_EXTRA_PANEL_SHIFT	19U
#define POWERVR_DC_SIZE_WIDTH_MASK	0x7fffU
#define POWERVR_DC_SIZE_HEIGHT_SHIFT	15U

/*
 * What the decision found: the screen can be taken over, or the first
 * condition that failed.
 */
enum powervr_dc_verdict {
	POWERVR_DC_GO = 0,
	POWERVR_DC_UNKNOWN_MODEL,
	POWERVR_DC_NO_PLANE,
	POWERVR_DC_OTHER_ADDRESS,
	POWERVR_DC_OTHER_STRIDE,
	POWERVR_DC_OTHER_SIZE,
	POWERVR_DC_TILED,
	POWERVR_DC_ROTATED,
	POWERVR_DC_SCALED,
	POWERVR_DC_COMPRESSED
};

/*
 * One primary plane's registers as read at the takeover's decision.
 *
 * The same values put back are the loader's screen again, so they are
 * kept as the place to return to.
 */
struct powervr_dc_plane {
	uint32_t address;
	uint32_t stride;
	uint32_t config;
	uint32_t extra;
	uint32_t size;
	uint32_t top_left;
	uint32_t bottom_right;
	uint32_t background;
	uint32_t blend;
};

/*
 * The controller as read once at attach: its identity and both primary
 * planes.
 */
struct powervr_dc_state {
	uint32_t model;
	uint32_t revision;
	uint32_t customer;
	struct powervr_dc_plane planes[POWERVR_DC_PLANES];
};

/*
 * The screen the loader handed over, as the kernel names it.
 */
struct powervr_dc_screen {
	uint64_t physical;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
};

/*
 * Decides whether the plane showing the loader's screen on a panel can be
 * taken over.
 *
 * The plane is the first enabled one on the panel; its number is stored
 * through plane, or POWERVR_DC_PLANES when there is none.
 */
enum powervr_dc_verdict powervr_dc_judge(const struct powervr_dc_state *state, const struct powervr_dc_screen *screen, uint32_t panel, unsigned *plane);

/*
 * Names a verdict for the boot log.
 */
const char *powervr_dc_verdict_name(enum powervr_dc_verdict verdict);

#endif
