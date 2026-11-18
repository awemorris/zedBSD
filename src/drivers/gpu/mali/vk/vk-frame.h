/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The descriptors of a render pass's frame shaders: the draw that runs a
 * program on each tile before the pass's own draws (the pre-frame draw
 * that reads a loaded attachment back into the tile buffer), its depth
 * and stencil state, its sampler, and the framebuffer's and render
 * targets' words that name it.  Each packer fills 32-bit words, low word
 * first, as the GPU reads them; nothing here touches the GPU, so the
 * packers run unchanged in a host test.
 */

#ifndef KERN_DRIVERS_GPU_MALI_VK_FRAME_H
#define KERN_DRIVERS_GPU_MALI_VK_FRAME_H

#include <stdint.h>

/* The frame shader draws a framebuffer names: pre-frame 0, pre-frame 1, post-frame. */
#define MALI_FRAME_DRAWS		3U

/* The bytes of the frame shader draws together. */
#define MALI_FRAME_DRAWS_BYTES		(MALI_FRAME_DRAWS * 128U)

/*
 * When the hardware runs a frame shader on a tile: never, on every tile
 * of the fragment run, or only on the tiles a primitive touches.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (enum "Pre Post Frame Shader
 * Mode": Never 0, Always 1, Intersect 2).
 */
#define MALI_FRAME_NEVER		0U
#define MALI_FRAME_ALWAYS		1U
#define MALI_FRAME_INTERSECT		2U

/*
 * What one pre-frame draw runs: its blend descriptors and their count,
 * its resource table pointer (the address with its table count), its
 * program descriptor, the pass's thread storage, its depth and stencil
 * descriptor, and the render target the program's colour 0 is written
 * to.
 */
struct mali_frame_draw {
	uint64_t blend;
	uint32_t blend_count;
	uint64_t resources;
	uint64_t shader;
	uint64_t storage;
	uint64_t depth;
	uint32_t target;
};

void mali_frame_draw(uint32_t *words, const struct mali_frame_draw *draw);
void mali_frame_depth(uint32_t *words);
void mali_frame_sampler(uint32_t *words);
void mali_frame_shaders(uint32_t *framebuffer_words, uint32_t pre_frame_mode, uint64_t draws);
void mali_frame_clean_tile(uint32_t *target_words);

#endif
