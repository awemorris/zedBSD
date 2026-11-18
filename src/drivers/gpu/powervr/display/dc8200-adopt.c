/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decision whether the display controller's screen, as the loader
 * left it, can be taken over.
 *
 * The screen is taken over only when the controller is the known model
 * and one enabled primary plane on the panel scans exactly the loader's
 * framebuffer: the same address, row pitch and extent, rows in plain
 * order, unrotated, unscaled and uncompressed.  Anything else leaves the
 * screen to the copying display node.
 */

#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/powervr/display/dc8200-adopt.h"

static int find_plane(const struct powervr_dc_state *state, uint32_t panel, unsigned *plane);

/*
 * Decides whether the plane showing the loader's screen on a panel can be
 * taken over.
 */
enum powervr_dc_verdict
powervr_dc_judge(
	const struct powervr_dc_state *state,
	const struct powervr_dc_screen *screen,
	uint32_t panel,
	unsigned *plane)
{
	const struct powervr_dc_plane *chosen;
	uint32_t width;
	uint32_t height;
	uint32_t tile;
	uint32_t rotation;
	unsigned index;
	int found;

	/* Starts with no plane chosen. */
	*plane = POWERVR_DC_PLANES;

	/* Refuses a controller whose registers the driver does not know. */
	if (state->model != POWERVR_DC_MODEL)
		return POWERVR_DC_UNKNOWN_MODEL;

	/* Finds the plane the panel shows; the loader lit none when there is none. */
	found = find_plane(state, panel, &index);
	if (!found)
		return POWERVR_DC_NO_PLANE;

	*plane = index;
	chosen = &state->planes[index];

	/* Refuses a plane scanning other memory than the loader's framebuffer. */
	if (screen->physical > 0xffffffffULL)
		return POWERVR_DC_OTHER_ADDRESS;
	if ((uint64_t)chosen->address != screen->physical)
		return POWERVR_DC_OTHER_ADDRESS;

	/* Refuses rows of another pitch than the framebuffer's. */
	if (chosen->stride != screen->pitch)
		return POWERVR_DC_OTHER_STRIDE;

	/* Refuses an extent other than the framebuffer's. */
	width = chosen->size & POWERVR_DC_SIZE_WIDTH_MASK;
	height = (chosen->size >> POWERVR_DC_SIZE_HEIGHT_SHIFT) & POWERVR_DC_SIZE_WIDTH_MASK;
	if (width != screen->width || height != screen->height)
		return POWERVR_DC_OTHER_SIZE;

	/* Refuses rows the controller reads in tiles; zero is plain rows. */
	tile = (chosen->config >> POWERVR_DC_CONFIG_TILE_SHIFT) & POWERVR_DC_CONFIG_TILE_MASK;
	if (tile != 0)
		return POWERVR_DC_TILED;

	/* Refuses a rotated or mirrored plane. */
	rotation = (chosen->config >> POWERVR_DC_CONFIG_ROTATE_SHIFT) & POWERVR_DC_CONFIG_ROTATE_MASK;
	if (rotation != 0)
		return POWERVR_DC_ROTATED;

	/* Refuses a scaled plane, whose rows are not the framebuffer's pixels. */
	if ((chosen->config & POWERVR_DC_CONFIG_SCALE) != 0)
		return POWERVR_DC_SCALED;

	/* Refuses a plane read through the compression unit. */
	if ((chosen->extra & POWERVR_DC_EXTRA_COMPRESSED) != 0)
		return POWERVR_DC_COMPRESSED;

	/* Succeeded: the plane shows exactly the loader's framebuffer. */
	return POWERVR_DC_GO;
}

/*
 * Names a verdict for the boot log.
 */
const char *
powervr_dc_verdict_name(
	enum powervr_dc_verdict verdict)
{
	/* Names each verdict by the condition that decided it. */
	switch (verdict) {
	case POWERVR_DC_GO:
		return "takeover possible";
	case POWERVR_DC_UNKNOWN_MODEL:
		return "unknown model";
	case POWERVR_DC_NO_PLANE:
		return "no enabled plane on the panel";
	case POWERVR_DC_OTHER_ADDRESS:
		return "plane address is not the framebuffer";
	case POWERVR_DC_OTHER_STRIDE:
		return "plane stride is not the framebuffer pitch";
	case POWERVR_DC_OTHER_SIZE:
		return "plane size is not the framebuffer size";
	case POWERVR_DC_TILED:
		return "plane is tiled";
	case POWERVR_DC_ROTATED:
		return "plane is rotated";
	case POWERVR_DC_SCALED:
		return "plane is scaled";
	case POWERVR_DC_COMPRESSED:
		return "plane is compressed";
	}

	/* A value outside the enumeration. */
	return "unknown verdict";
}

/* Finds the first enabled primary plane shown on a panel. */
static int
find_plane(
	const struct powervr_dc_state *state,
	uint32_t panel,
	unsigned *plane)
{
	const struct powervr_dc_plane *candidate;
	uint32_t shown_on;
	unsigned index;

	/* Looks at the planes in order and stops at the first one lit on the panel. */
	for (index = 0; index < POWERVR_DC_PLANES; index++) {
		candidate = &state->planes[index];
		if ((candidate->extra & POWERVR_DC_EXTRA_ENABLED) == 0)
			continue;

		shown_on = (candidate->extra >> POWERVR_DC_EXTRA_PANEL_SHIFT) & 1U;
		if (shown_on != panel)
			continue;

		*plane = index;
		return 1;
	}

	/* No plane is lit on the panel. */
	return 0;
}
