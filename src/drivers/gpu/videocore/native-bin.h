/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* One complete state sequence resets inherited binning state before one triangle-list draw. */
#ifndef KERN_DRIVERS_GPU_BCM2711_NATIVE_BIN_H
#define KERN_DRIVERS_GPU_BCM2711_NATIVE_BIN_H

#include <stddef.h>
#include "drivers/gpu/bcm2711/native-viewport.h"

/*
 * The state takes 123 bytes; the draw after it at most 32 (Index Buffer
 * Setup, Base Vertex Base Instance and Indexed Instanced Prim List), and a
 * shorter draw is followed by NOPs, so every draw has the same length.
 */
#define BCM2711_NATIVE_BIN_STATE_BYTES 123U
#define BCM2711_NATIVE_BIN_BYTES 155U

/*
 * One face's stencil test in the hardware's codes: the operations are the
 * V3D Stencil Op values (zero 0, keep 1, replace 2, increment 3, decrement 4,
 * invert 5, increment-wrap 6, decrement-wrap 7) and the compare function is
 * the V3D Compare Function, whose values are Vulkan's VkCompareOp.
 */
struct bcm2711_native_stencil {
	uint32_t fail;
	uint32_t pass;
	uint32_t depth_fail;
	uint32_t compare;
	uint32_t compare_mask;
	uint32_t write_mask;
	uint32_t reference;
};

/* All addresses borrow complete cleaned storage from the enclosing native draw owner; numerical state owns no DMA lifetime. */
struct bcm2711_native_bin {
	struct bcm2711_native_viewport viewport;
	struct bcm2711_native_viewport_clip clipper;
	uint32_t window[4];
	uint32_t shader;
	uint32_t attributes;
	/* The VCM Cache Size packet's byte (bcm2711_native_shader_vcm); zero takes two batches each. */
	uint32_t vcm;
	/* The vertices, or for an indexed draw the indices, of one instance. */
	uint32_t vertices;
	uint32_t instances;
	/* The first vertex; for an indexed draw the first index's byte offset from index_address. */
	uint32_t first;
	/* Added to every index of an indexed draw (Vulkan's vertexOffset, two's complement). */
	uint32_t base_vertex;
	uint32_t base_instance;
	/* One: the draw reads index_bytes of indices of index_size (2 or 4) bytes at index_address. */
	uint32_t indexed;
	uint32_t index_address;
	uint32_t index_bytes;
	uint32_t index_size;
	uint32_t forward;
	uint32_t reverse;
	uint32_t clockwise;
	/* One: the tile buffer blends with blend_factors; zero: the fragment replaces the target. */
	uint32_t blend;
	/*
	 * The blend equation: the Blend Cfg's colour destination factor (bits
	 * 20-23), source factor (16-19) and mode (12-15), and the alpha's
	 * (8-11, 4-7, 0-3).
	 */
	uint32_t blend_factors;
	/* The colour channels of target zero the draw leaves unwritten: bit 0 red through bit 3 alpha. */
	uint32_t colour_skip;
	/* One: the tile buffer's depth test runs with depth_compare, and depth_write stores passing depths; zero: every fragment passes. */
	uint32_t depth_test;
	uint32_t depth_compare;
	uint32_t depth_write;
	/* One: the stencil test runs, with front for forward-facing and back for reverse-facing primitives. */
	uint32_t stencil;
	struct bcm2711_native_stencil front;
	struct bcm2711_native_stencil back;
	uint32_t flat;
	uint32_t noperspective;
};

int bcm2711_native_bin_encode(const struct bcm2711_native_bin *state, uint8_t *bytes, size_t capacity);

#endif
