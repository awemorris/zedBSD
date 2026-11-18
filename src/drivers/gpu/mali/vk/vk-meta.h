/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's own programs: the compute program of a blit that
 * scales, flips or exchanges red and blue, the vertex and fragment
 * programs of a clear of attachments inside a render pass, and the
 * compute program that copies or clears one aspect of a depth and
 * stencil image.
 *
 * Each program is compiled once for the node, the first time a
 * submission needs it; its code is copied into each session's space the
 * first time one of the session's submissions needs it.  When either
 * cannot be made, or a submission that ran it failed, the work that
 * needs it is left out as before (with the line said before).
 */

#ifndef KERN_DRIVERS_GPU_MALI_VK_META_H
#define KERN_DRIVERS_GPU_MALI_VK_META_H

#include <stdint.h>

#include "drivers/gpu/mali/vk/vk-build.h"

/* The FAU words an executor's program takes at most. */
#define MALI_VK_META_FAU_WORDS		64U

/*
 * The programs, by number: the blit, the clear's vertex program, the
 * clear's fragment program of colour location L at
 * MALI_VK_META_CLEAR_COLOR + L (eight of them), and the aspect copy.
 */
#define MALI_VK_META_BLIT		0U
#define MALI_VK_META_CLEAR_VERTEX	1U
#define MALI_VK_META_CLEAR_COLOR	2U
#define MALI_VK_META_ASPECT		10U

/*
 * One run of the aspect copy: width by height texels of each of layers
 * layers.  source and target are the word-aligned addresses of the first
 * texel of the first layer; each side has its row pitch and its layer
 * size in bytes, its bytes a texel (1 for packed stencil bytes, 4 for a
 * texel word) and the shift of the field above its byte's place in its
 * word.  mask is the field's bits before the shift.  value is the field
 * written instead of the source's with MALI_META_ASPECT_CONSTANT in
 * flags; MALI_META_ASPECT_SHARED says several texels' fields share the
 * target's words.
 */
struct mali_vk_meta_aspect {
	uint64_t source;
	uint64_t target;
	uint32_t width;
	uint32_t height;
	uint32_t layers;
	uint64_t source_pitch;
	uint64_t target_pitch;
	uint64_t source_layer;
	uint64_t target_layer;
	uint32_t source_step;
	uint32_t target_step;
	uint32_t source_shift;
	uint32_t target_shift;
	uint32_t mask;
	uint32_t value;
	uint32_t flags;
};

/*
 * Gives one of the executor's programs and the address of the session's
 * copy of its code: 0, or ENOTSUP when either is not there (said once).
 * The submission being built is marked as running it.
 */
int mali_vk_meta_program(struct mali_vk_build *build, unsigned which, const struct mali_shader_program **program, uint64_t *code);

/*
 * Writes a program's FAU words (count of them, even) into the stage and
 * reports the FAU pointer with its count, zero for none.
 */
int mali_vk_meta_fau(struct mali_vk_build *build, const uint32_t *words, uint32_t count, uint64_t *fau);

/* Writes a resource table of empty tables into the stage and reports its pointer with its count. */
uint64_t mali_vk_meta_tables(struct mali_vk_build *build);

/*
 * Writes one region of a blit between linear images of four bytes a
 * texel in the RGBA or BGRA order as a compute run: 0 when it is in the
 * stream (or has no texel), ENOTSUP when the formats, the depth range
 * or the program do not let it run, or the stage's error.
 */
int mali_vk_meta_blit(struct mali_vk_build *build, const struct mali_vk_image *source, const struct mali_vk_image *target, const VkImageBlit *region, uint32_t filter);

/*
 * Writes one run of the aspect copy as a compute run: 0 when it is in
 * the stream (or has no texel), ENOTSUP when an address is not word
 * aligned, a texel's byte offset does not fit 31 bits or the program is
 * not there, or the stage's error.
 */
int mali_vk_meta_aspect(struct mali_vk_build *build, const struct mali_vk_meta_aspect *aspect);

#endif
