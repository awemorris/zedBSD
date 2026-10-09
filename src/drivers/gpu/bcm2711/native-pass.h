/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* A complete single-colour native pass borrows independently owned, cleaned DMA storage from its enclosing job. */
#ifndef KERN_DRIVERS_GPU_BCM2711_NATIVE_PASS_H
#define KERN_DRIVERS_GPU_BCM2711_NATIVE_PASS_H

#include <stddef.h>
#include "drivers/gpu/bcm2711/native-bin.h"

#define BCM2711_NATIVE_PASS_BIN_PREFIX 13U

/* Numerical framebuffer and tile state describe one single-layer, single-sample raster pass; no public identities are retained. */
struct bcm2711_native_pass {
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t output;
	uint32_t output_bytes;
	uint32_t pool;
	uint32_t pool_bytes;
	uint32_t state;
	uint32_t state_bytes;
	/* Tile coordinates are inclusive and bounded by the complete framebuffer. */
	uint32_t first_x;
	uint32_t last_x;
	uint32_t first_y;
	uint32_t last_y;
	uint32_t load;
	uint32_t store;
	uint32_t clear_colour;
};

/* Exact active list lengths and minimum binning storage are measured before mapped owners are allocated. */
struct bcm2711_native_pass_sizes {
	uint32_t bin;
	uint32_t render;
	uint32_t tile;
	uint32_t pool;
	uint32_t state;
	/* Complete supertile geometry limits the total native configuration to at most 256 groups. */
	uint32_t supertile_width;
	uint32_t supertile_height;
	uint32_t columns;
	uint32_t rows;
};

/* One caller-owned CPU/GPU interval publishes its active byte count only after the entire three-list preparation succeeds. */
struct bcm2711_native_pass_image {
	uint8_t *bytes;
	uint32_t address;
	uint32_t capacity;
	uint32_t used;
};

/* Draw sequences must already occupy bin.bytes + PREFIX; this operation preserves those complete numerical sequences. */
int bcm2711_native_pass_measure(const struct bcm2711_native_pass *state, uint32_t draws, struct bcm2711_native_pass_sizes *sizes);
int bcm2711_native_pass_encode(const struct bcm2711_native_pass *state, uint32_t draws, struct bcm2711_native_pass_image *bin, struct bcm2711_native_pass_image *render, struct bcm2711_native_pass_image *tile);

#endif
