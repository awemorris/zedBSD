/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* A complete native pass of up to four colour targets borrows independently owned, cleaned DMA storage from its enclosing job. */
#ifndef KERN_DRIVERS_GPU_BCM2711_NATIVE_PASS_H
#define KERN_DRIVERS_GPU_BCM2711_NATIVE_PASS_H

#include <stddef.h>
#include "drivers/gpu/videocore/native-bin.h"

#define BCM2711_NATIVE_PASS_BIN_PREFIX 13U

/* The most tile boxes a pass is narrowed to; more render every tile of its rectangle. */
#define BCM2711_NATIVE_PASS_BOXES 8U

/* The most colour render targets a pass has; V3D 4.2 draws into the first alone. */
#define BCM2711_NATIVE_PASS_TARGETS 4U

/* The tile buffer's Internal Type of a colour render target (Mesa's v3d_packet.xml "Internal Type"). */
#define BCM2711_NATIVE_TYPE_8I 0U
#define BCM2711_NATIVE_TYPE_8UI 1U
#define BCM2711_NATIVE_TYPE_8 2U
#define BCM2711_NATIVE_TYPE_16I 4U
#define BCM2711_NATIVE_TYPE_16UI 5U
#define BCM2711_NATIVE_TYPE_16F 6U
#define BCM2711_NATIVE_TYPE_32I 8U
#define BCM2711_NATIVE_TYPE_32UI 9U
#define BCM2711_NATIVE_TYPE_32F 10U

/* The Output Image Format of the RGBA8 raster images the internal transfers and the compositor use. */
#define BCM2711_NATIVE_OUTPUT_RGBA8 27U

/*
 * One colour render target of a pass: its raster image and how the tile
 * buffer holds it.
 *
 * output 0 is an unused slot, which the tile buffer keeps as an unused
 * target (the shaders write no colour to it) and no tile loads or stores.
 * format is the image's Output Image Format, type and bpp the tile
 * buffer's Internal Type and Internal BPP (0 32, 1 64, 2 128 bits a pixel)
 * and clamp the 7.1 Render Target Type Clamp; texel is the raster's bytes a
 * pixel.  clear is the clear colour as the tile buffer holds it (bpp's
 * words of it count): load 0 starts every tile from it.  In a
 * multisampled pass the raster holds all four samples of each pixel, as a
 * 2x2 block (twice as wide and as high), unless resolve is set: then the
 * store writes a raster of one sample (and nothing is loaded), the
 * samples' average (resolve 1) or, for an integer format, sample 0
 * (resolve 2).
 */
struct bcm2711_native_pass_target {
	uint32_t output;
	uint32_t output_bytes;
	uint32_t pitch;
	uint32_t load;
	uint32_t store;
	uint32_t resolve;
	uint32_t format;
	uint32_t type;
	uint32_t bpp;
	uint32_t clamp;
	uint32_t texel;
	uint32_t clear[4];
};

/*
 * Numerical framebuffer and tile state describe one single-layer raster pass; no public identities are retained.
 *
 * targets is the number of colour slots (render targets) the pass has, up
 * to BCM2711_NATIVE_PASS_TARGETS; zero is a depth-only pass.  tile_width
 * and tile_height are the tile's pixels, which bcm2711_native_pass_tile
 * chooses from the targets; the tile coordinates below count tiles of
 * that size.  msaa set renders four samples a pixel (the tile buffer's 4x
 * multisample mode): the targets, the depth/stencil image and the copy
 * source hold all of them, as 2x2 blocks of their rasters.
 */
struct bcm2711_native_pass {
	uint32_t width;
	uint32_t height;
	uint32_t msaa;
	uint32_t targets;
	struct bcm2711_native_pass_target target[BCM2711_NATIVE_PASS_TARGETS];
	uint32_t tile_width;
	uint32_t tile_height;
	uint32_t pool;
	uint32_t pool_bytes;
	uint32_t state;
	uint32_t state_bytes;
	/* Tile coordinates are inclusive and bounded by the complete framebuffer. */
	uint32_t first_x;
	uint32_t last_x;
	uint32_t first_y;
	uint32_t last_y;
	/* A nonzero source makes target 0's tile load read another raster image at the same pixel coordinates (a TLB copy). */
	uint32_t source;
	uint32_t source_pitch;
	uint32_t source_bytes;
	/*
	 * The depth/stencil attachment: depth 0 has none (the tile buffer's
	 * depth is unused, cleared to 1.0 and stencil 0); 1 a D24S8 image, 2 an
	 * X8_D24 image (depth only), 3 a D16 image (depth only, two bytes a
	 * pixel), 4 a D32_SFLOAT image (depth only, a float a pixel).  Its tile
	 * buffer is cleared to
	 * depth_clear (IEEE bits) and stencil_clear in every tile; depth_load
	 * and depth_store move it from and to the raster image at
	 * depth_output with depth_pitch.
	 */
	uint32_t depth;
	uint32_t depth_clear;
	uint32_t stencil_clear;
	uint32_t depth_load;
	uint32_t depth_store;
	uint32_t depth_output;
	uint32_t depth_pitch;
	uint32_t depth_bytes;
	/*
	 * The resolve of a multisampled depth/stencil attachment (7.1 only):
	 * depth_resolve set stores, after the attachment's own store, sample 0
	 * of each pixel's depth (and stencil, for D24S8) into the raster image
	 * of one sample at depth_resolve_output with depth_resolve_pitch, in
	 * the attachment's memory format.
	 */
	uint32_t depth_resolve;
	uint32_t depth_resolve_output;
	uint32_t depth_resolve_pitch;
	uint32_t depth_resolve_bytes;
	/*
	 * The tile boxes (first x, first y, last x, last y, inclusive, inside
	 * the rectangle above) whose tiles the pass changes: only
	 * the supertiles that meet one of them are rendered.  A tile no box
	 * meets is one whose load and store would write back the bytes it
	 * holds, so the owner names boxes only for a pass that loads its own
	 * output (or copies the tiles it names).  box_count 0: every
	 * supertile of the rectangle.
	 */
	uint32_t box_count;
	uint32_t boxes[BCM2711_NATIVE_PASS_BOXES][4];
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

/*
 * Chooses a pass's tile size from its targets (tile_width and tile_height):
 * the largest whose colour fits the tile buffer.  EINVAL for targets the
 * engine does not draw into.
 */
int bcm2711_native_pass_tile(struct bcm2711_native_pass *state);

/* Draw sequences must already occupy bin.bytes + PREFIX; this operation preserves those complete numerical sequences. */
int bcm2711_native_pass_measure(const struct bcm2711_native_pass *state, uint32_t draws, struct bcm2711_native_pass_sizes *sizes);
int bcm2711_native_pass_encode(const struct bcm2711_native_pass *state, uint32_t draws, struct bcm2711_native_pass_image *bin, struct bcm2711_native_pass_image *render, struct bcm2711_native_pass_image *tile);

#endif
