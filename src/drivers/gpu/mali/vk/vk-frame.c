/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The frame shader descriptor packers.  See vk-frame.h.
 */

#include <stdint.h>

#include "drivers/gpu/mali/vk/vk-desc.h"
#include "drivers/gpu/mali/vk/vk-frame.h"

/*
 * The flags of a pre-frame draw: it may hide the fragments under it and
 * be hidden by the opaque fragments over it, its pixels are killed and
 * its depth updated early (the program writes no depth), and its colour
 * writes every byte of the pixel.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "DCD Flags 0": allow
 * forward pixel to kill 0, allow forward pixel to be killed 1, pixel
 * kill operation 2..3, ZS update operation 4..5, clean fragment write 9;
 * enum "Pixel Kill": Force Early 0); the choices from
 * src/panfrost/vulkan/panvk_vX_cmd_frame_shaders.c (cmd_emit_dcd, the
 * colour preload's draw).
 */
#define DRAW_FORWARD_KILL		(1U << 0)
#define DRAW_FORWARD_KILLED		(1U << 1)
#define DRAW_KILL_FORCE_EARLY		0U
#define DRAW_CLEAN_FRAGMENT_WRITE	(1U << 9)

/*
 * The draw's word of the render targets its program writes and reads,
 * and the place of its depth and stencil descriptor.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "Draw": flags 2 in
 * word 5, depth/stencil in words 10..11; struct "DCD Flags 2": read mask
 * 0..11, write mask 12..23).
 */
#define DRAW_FLAGS2_WORD		5U
#define DRAW_WRITE_MASK_SHIFT		12U
#define DRAW_DEPTH_WORD			10U

/*
 * A depth and stencil descriptor that passes every fragment and writes
 * neither: the descriptor type, both stencil compares always (the
 * stencil test off), the value masks full, no depth cull, the depth
 * clamped to [0, 1] from the fixed function, the depth compare always.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "Depth/stencil": type
 * 0..3, front compare 4..6, back compare 16..18, front and back value
 * masks 1:16..31, depth cull 4:22, depth clamp mode 4:23..24, depth
 * source 4:25..26, depth function 4:29..31; enums "Descriptor Type"
 * Depth/stencil 7, "Func" Always 7, "Depth Clamp Mode" [0, 1] 1, "Depth
 * Source" Fixed function 2); the choices from
 * src/panfrost/vulkan/panvk_vX_cmd_frame_shaders.c (cmd_emit_dcd, the
 * colour preload's depth/stencil).
 */
#define DEPTH_TYPE			7U
#define DEPTH_FUNC_ALWAYS		7U
#define DEPTH_FRONT_COMPARE_SHIFT	4U
#define DEPTH_BACK_COMPARE_SHIFT	16U
#define DEPTH_VALUE_MASKS		0xffff0000U
#define DEPTH_CLAMP_0_1			(1U << 23)
#define DEPTH_SOURCE_FIXED		(2U << 25)
#define DEPTH_FUNCTION_SHIFT		29U

/*
 * A sampler of exact texels: the descriptor type, every wrap clamped to
 * the edge, round to nearest even, array indices clamped, both filters
 * nearest, no mipmaps, unnormalized coordinates.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "Sampler": type 0..3,
 * wrap R 8..11, T 12..15, S 16..19, round to nearest even 21, clamp
 * integer array indices 26, minify nearest 27, magnify nearest 28,
 * mipmap mode 30..31; enums "Descriptor Type" Sampler 1, "Wrap Mode"
 * Clamp to Edge 9, "Mipmap Mode" None 1); the choices from
 * src/panfrost/vulkan/panvk_vX_cmd_frame_shaders.c (the frame shader's
 * sampler: nearest both ways, coordinates not normalized).
 */
#define SAMPLER_TYPE			1U
#define SAMPLER_WRAP_EDGE		9U
#define SAMPLER_WRAP_R_SHIFT		8U
#define SAMPLER_WRAP_T_SHIFT		12U
#define SAMPLER_WRAP_S_SHIFT		16U
#define SAMPLER_ROUND_EVEN		(1U << 21)
#define SAMPLER_CLAMP_ARRAY		(1U << 26)
#define SAMPLER_MIN_NEAREST		(1U << 27)
#define SAMPLER_MAG_NEAREST		(1U << 28)
#define SAMPLER_MIPMAP_NONE		(1U << 30)

/*
 * The framebuffer's frame shader words: the pre-frame 0 mode, and the
 * address of the frame shader draws.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "Framebuffer
 * Parameters": pre frame 0 at 0:0..2, frame shader DCDs at words 6..7).
 */
#define FRAMEBUFFER_PRE_FRAME_0_MASK	7U
#define FRAMEBUFFER_DRAWS_WORD		6U

/*
 * A render target's clean tile write: its tile is written back even
 * when no primitive touched it.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "RGB Render Target":
 * clean tile write enable 1:31).
 */
#define TARGET_CLEAN_TILE_WORD		1U
#define TARGET_CLEAN_TILE		(1U << 31)

static void put64(uint32_t *words, unsigned at, uint64_t value);
static void clear(uint32_t *words, unsigned count);

/*
 * Packs a pre-frame draw descriptor (struct "Draw"): the draw flags,
 * every sample, the one render target its program writes, its blend
 * descriptors, its depth range, its depth and stencil descriptor, and
 * the shader environment of a program without FAU words.
 */
void
mali_frame_draw(
	uint32_t *words,
	const struct mali_frame_draw *draw)
{
	uint32_t flags;
	uint32_t target_bit;

	/* The flags of a draw that only fills the tile. */
	flags = DRAW_FORWARD_KILL;
	flags |= DRAW_FORWARD_KILLED;
	flags |= DRAW_KILL_FORCE_EARLY << 2;
	flags |= DRAW_KILL_FORCE_EARLY << 4;
	flags |= DRAW_CLEAN_FRAGMENT_WRITE;

	/* The common words: the flags, the target, the blends and the shader environment. */
	target_bit = 1U << (draw->target & 7U);
	mali_desc_draw(words,
		       flags,
		       target_bit,
		       draw->blend,
		       draw->blend_count,
		       draw->resources,
		       draw->shader,
		       draw->storage,
		       0U,
		       0U);

	/* The render target the program writes, which it does not read. */
	words[DRAW_FLAGS2_WORD] = target_bit << DRAW_WRITE_MASK_SHIFT;

	/* The depth and stencil descriptor. */
	put64(words, DRAW_DEPTH_WORD, draw->depth);
}

/*
 * Packs a depth and stencil descriptor (struct "Depth/stencil") that
 * passes every fragment and writes nothing.
 */
void
mali_frame_depth(
	uint32_t *words)
{
	/* The type and both stencil compares; the stencil test stays off. */
	clear(words, 8);
	words[0] = DEPTH_TYPE;
	words[0] |= DEPTH_FUNC_ALWAYS << DEPTH_FRONT_COMPARE_SHIFT;
	words[0] |= DEPTH_FUNC_ALWAYS << DEPTH_BACK_COMPARE_SHIFT;
	words[1] = DEPTH_VALUE_MASKS;

	/* The depth: no cull, clamped to [0, 1], from the fixed function, always passing. */
	words[4] = DEPTH_CLAMP_0_1;
	words[4] |= DEPTH_SOURCE_FIXED;
	words[4] |= DEPTH_FUNC_ALWAYS << DEPTH_FUNCTION_SHIFT;
}

/*
 * Packs a sampler descriptor (struct "Sampler") that reads exact texels:
 * nearest, clamped to the edge, without mipmaps.
 */
void
mali_frame_sampler(
	uint32_t *words)
{
	/* The first word holds every choice; the others (levels, anisotropy, border) stay zero. */
	clear(words, 8);
	words[0] = SAMPLER_TYPE;
	words[0] |= SAMPLER_WRAP_EDGE << SAMPLER_WRAP_R_SHIFT;
	words[0] |= SAMPLER_WRAP_EDGE << SAMPLER_WRAP_T_SHIFT;
	words[0] |= SAMPLER_WRAP_EDGE << SAMPLER_WRAP_S_SHIFT;
	words[0] |= SAMPLER_ROUND_EVEN;
	words[0] |= SAMPLER_CLAMP_ARRAY;
	words[0] |= SAMPLER_MIN_NEAREST;
	words[0] |= SAMPLER_MAG_NEAREST;
	words[0] |= SAMPLER_MIPMAP_NONE;
}

/*
 * Names the frame shader draws in a framebuffer's parameters (struct
 * "Framebuffer Parameters"), packed before by mali_desc_framebuffer():
 * the pre-frame 0 draw's mode, and the address of the three draws.
 * Pre-frame 1 and the post-frame draw stay never.
 */
void
mali_frame_shaders(
	uint32_t *framebuffer_words,
	uint32_t pre_frame_mode,
	uint64_t draws)
{
	/* The pre-frame 0 mode in the lowest bits of word 0. */
	framebuffer_words[0] &= ~FRAMEBUFFER_PRE_FRAME_0_MASK;
	framebuffer_words[0] |= pre_frame_mode & FRAMEBUFFER_PRE_FRAME_0_MASK;

	/* The draws' address. */
	put64(framebuffer_words, FRAMEBUFFER_DRAWS_WORD, draws);
}

/*
 * Makes a render target (struct "RGB Render Target"), packed before by
 * mali_desc_target(), write back the tiles no primitive touched.
 */
void
mali_frame_clean_tile(
	uint32_t *target_words)
{
	/* A cleared target's every tile holds the clear, drawn on or not. */
	target_words[TARGET_CLEAN_TILE_WORD] |= TARGET_CLEAN_TILE;
}

/* Stores a 64-bit value as two words, low first. */
static void
put64(
	uint32_t *words,
	unsigned at,
	uint64_t value)
{
	words[at] = (uint32_t)value;
	words[at + 1U] = (uint32_t)(value >> 32);
}

/* Clears a number of words. */
static void
clear(
	uint32_t *words,
	unsigned count)
{
	unsigned index;

	/* Every word, in order. */
	for (index = 0; index < count; index++)
		words[index] = 0;
}
