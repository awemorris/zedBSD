/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The depth and stencil surface and mip level packers.  See vk-surface.h.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/mali/vk/vk-surface.h"

/*
 * The Vulkan numbers this file reads: the depth and stencil formats, and
 * the stencil ops.
 *
 * Values referenced:
 * the Vulkan specification 1.0 (enums VkFormat: D16_UNORM 124,
 * X8_D24_UNORM_PACK32 125, D32_SFLOAT 126, S8_UINT 127,
 * D24_UNORM_S8_UINT 129; VkStencilOp: KEEP 0 to DECREMENT_AND_WRAP 7;
 * VkCompareOp: NEVER 0 to ALWAYS 7, in the hardware's order;
 * VkSamplerMipmapMode: NEAREST 0, LINEAR 1).
 */
#define VK_D16_UNORM		124U
#define VK_X8_D24_UNORM		125U
#define VK_D32_SFLOAT		126U
#define VK_S8_UINT		127U
#define VK_D24_UNORM_S8_UINT	129U
#define VK_COMPARE_ALWAYS	7U
#define VK_MIPMAP_LINEAR	1U

/*
 * The depth and stencil targets' formats, their linear block format,
 * and the tile buffer's depth formats.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (enum "ZS Format": D16 1,
 * D24X8 4, D24S8 5, D32 14; enum "S Format": S8 1, X24S8 4; enum "Block
 * Format": Linear 2; enum "Z Internal Format": D16 0, D24 1, D32 2);
 * src/panfrost/lib/pan_desc.c (translate_zs_format, translate_s_format);
 * src/panfrost/lib/pan_encoder.h (pan_get_z_internal_format, and the
 * D24 the framebuffer takes without a depth surface, from pan_emit_fbd).
 */
#define ZS_D16			1U
#define ZS_D24X8		4U
#define ZS_D24S8		5U
#define ZS_D32			14U
#define S_S8			1U
#define S_X24S8			4U
#define BLOCK_LINEAR		2U
#define Z_INTERNAL_D16		0U
#define Z_INTERNAL_D24		1U
#define Z_INTERNAL_D32		2U

/*
 * The depth and stencil target extension's words (struct "ZS CRC
 * Extension" of a "ZS Target" and an "S Target"): the depth's write
 * format 0..3 and block format 4..7, the stencil's write format 16..19
 * and block format 20..23, the clean tile write 31 in word 0; the
 * depth's base in words 8..9, row stride 10, surface stride 11; the
 * stencil's in words 12..15.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (structs "ZS Target", "S
 * Target", "ZS CRC Extension").
 */
#define ZS_S_WRITE_SHIFT	16U
#define ZS_S_BLOCK_SHIFT	20U
#define ZS_BLOCK_SHIFT		4U
#define ZS_CLEAN_TILE		(1U << 31)
#define ZS_DEPTH_BASE		8U
#define ZS_STENCIL_BASE		12U

/*
 * The framebuffer's depth and stencil words (struct "Framebuffer
 * Parameters"): word 12 the stencil clear 0..7, the stencil write 8 and
 * preload 9, the tile buffer's depth format 16..17, the depth write 18
 * and preload 19, the extension's presence 21; word 13 the depth clear's
 * float; and the framebuffer pointer's bit of the extension (struct
 * "Framebuffer pointer", ZS CRC Extension present at 1).
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (structs "Framebuffer
 * Parameters", "Framebuffer pointer").
 */
#define FBD_ZS_WORD		12U
#define FBD_Z_CLEAR_WORD	13U
#define FBD_S_WRITE		(1U << 8)
#define FBD_S_PRELOAD		(1U << 9)
#define FBD_Z_INTERNAL_SHIFT	16U
#define FBD_Z_WRITE		(1U << 18)
#define FBD_Z_PRELOAD		(1U << 19)
#define FBD_ZS_EXTENSION	(1U << 21)
#define POINTER_ZS_EXTENSION	(1U << 1)

/*
 * The depth and stencil descriptor's fields (struct "Depth/stencil"):
 * word 0 the type, the front compare 4, stencil fail 7, depth fail 10,
 * depth pass 13, the back's from 16, 19, 22 and 25, the stencil from
 * the shader 30, the stencil test 31; word 1 the write masks 0 and 8,
 * the value masks 16 and 24; word 2 the references 0 and 8; word 4 the
 * depth cull 22, the clamp mode [0, 1] 23, the depth source 25 (fixed
 * function 2, shader 3), the depth write 27, the bias 28, the depth
 * function 29; words 5..7 the bias's units, factor and clamp.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "Depth/stencil",
 * enums "Func", "Stencil Op", "Depth Clamp Mode", "Depth Source").
 */
#define ZSD_TYPE		7U
#define ZSD_FRONT_SHIFT		4U
#define ZSD_BACK_SHIFT		16U
#define ZSD_STENCIL_SHADER	(1U << 30)
#define ZSD_STENCIL_TEST	(1U << 31)
#define ZSD_DEPTH_CULL		(1U << 22)
#define ZSD_CLAMP_0_1		(1U << 23)
#define ZSD_SOURCE_FIXED	(2U << 25)
#define ZSD_SOURCE_SHADER	(3U << 25)
#define ZSD_DEPTH_WRITE		(1U << 27)
#define ZSD_DEPTH_BIAS		(1U << 28)
#define ZSD_DEPTH_FUNC_SHIFT	29U

/*
 * The sampler's level of detail fields (struct "Sampler"): the mipmap
 * mode 0:30..31 (nearest 0, none 1, trilinear 3), the minimum LOD
 * 1:0..12 and maximum LOD 1:16..28 (unsigned, 8 fraction bits), the LOD
 * bias 2:0..15 (signed, 8 fraction bits), each rounded to nearest and
 * clamped to its field.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "Sampler", enum
 * "Mipmap Mode"); genxml/gen_pack.py (ulod and slod as clamped fixed
 * point of 8 fraction bits); src/panfrost/vulkan/panvk_vX_sampler.c
 * (panvk_translate_sampler_mipmap_mode: linear is trilinear).
 */
#define SAMPLER_MIPMAP_SHIFT	30U
#define SAMPLER_MIPMAP_MASK	(3U << 30)
#define SAMPLER_TRILINEAR	3U
#define SAMPLER_LOD_MASK	0x1fffU
#define SAMPLER_MAX_LOD_SHIFT	16U
#define SAMPLER_BIAS_MASK	0xffffU

/*
 * The formats.
 *
 * Values referenced: as the format defines above.
 */
static const struct mali_surface_zs_format zs_formats[] = {
	{ VK_D16_UNORM, MALI_SURFACE_ASPECT_DEPTH, ZS_D16, 0U, Z_INTERNAL_D16 },
	{ VK_X8_D24_UNORM, MALI_SURFACE_ASPECT_DEPTH, ZS_D24X8, 0U, Z_INTERNAL_D24 },
	{ VK_D32_SFLOAT, MALI_SURFACE_ASPECT_DEPTH, ZS_D32, 0U, Z_INTERNAL_D32 },
	{ VK_S8_UINT, MALI_SURFACE_ASPECT_STENCIL, 0U, S_S8, Z_INTERNAL_D24 },
	{ VK_D24_UNORM_S8_UINT, MALI_SURFACE_ASPECT_DEPTH | MALI_SURFACE_ASPECT_STENCIL, ZS_D24S8, S_X24S8, Z_INTERNAL_D24 },
};

static uint32_t stencil_op(uint32_t vk_op);
static uint32_t stencil_face(const struct mali_surface_stencil_face *face);
static bool zs_always_passes(const struct mali_surface_zs_state *state);
static int64_t fixed8_from_float(uint32_t bits);

/* Finds a depth and stencil format the tile buffer takes, or NULL. */
const struct mali_surface_zs_format *
mali_surface_zs_find(
	uint32_t vk_format)
{
	size_t index;

	/* A walk of the table. */
	for (index = 0; index < sizeof(zs_formats) / sizeof(zs_formats[0]); index++) {
		if (zs_formats[index].vk == vk_format)
			return &zs_formats[index];
	}

	/* Not one of them. */
	return NULL;
}

/* Gives an extent at a level: halved once a level, at least one. */
uint32_t
mali_surface_extent(
	uint32_t extent,
	uint32_t level)
{
	/* Past every bit, one. */
	if (level >= 32U)
		return 1U;
	extent >>= level;
	if (extent == 0U)
		return 1U;

	/* The halved extent. */
	return extent;
}

/* Counts the levels of a full chain down to one texel: one more than the base-two logarithm of the larger edge. */
uint32_t
mali_surface_level_count(
	uint32_t width,
	uint32_t height)
{
	uint32_t largest;
	uint32_t levels;

	/* The larger edge, halved to one. */
	largest = width;
	if (height > largest)
		largest = height;
	levels = 1U;
	while (largest > 1U) {
		largest >>= 1;
		levels++;
	}

	/* The levels of the chain. */
	return levels;
}

/*
 * Lays out the levels of one layer of a linear image, level after level
 * from offset zero: each row a multiple of row_align bytes, each level's
 * rows together, each level starting row_align aligned.  Fills out[0]
 * to out[levels - 1] and reports the layer's bytes (the array stride),
 * a multiple of 64.  One level gives the pitch and bytes of a one-level
 * image: the width's bytes aligned, times the height.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/lib/pan_layout.c (pan_image_layout_init: the
 * slices after one another, the array stride their sum aligned to 64)
 * and pan_mod.c (pan_mod_linear_init_slice_layout: the row stride and
 * the surface stride aligned to the cache line).
 */
uint64_t
mali_surface_levels(
	uint32_t width,
	uint32_t height,
	uint32_t texel_bytes,
	uint32_t row_align,
	uint32_t levels,
	struct mali_surface_level *out)
{
	uint64_t offset;
	uint64_t pitch;
	uint64_t bytes;
	uint32_t level;

	/* Each level after the one before. */
	offset = 0;
	for (level = 0; level < levels && level < MALI_SURFACE_LEVELS; level++) {
		out[level].width = mali_surface_extent(width, level);
		out[level].height = mali_surface_extent(height, level);
		pitch = ((uint64_t)out[level].width * texel_bytes + row_align - 1U) & ~(uint64_t)(row_align - 1U);
		bytes = (pitch * out[level].height + row_align - 1U) & ~(uint64_t)(row_align - 1U);
		out[level].offset = offset;
		out[level].pitch = (uint32_t)pitch;
		out[level].bytes = bytes;
		offset += bytes;
	}

	/* The layer's bytes, cache line aligned. */
	return (offset + 63U) & ~(uint64_t)63U;
}

/*
 * Packs the depth and stencil target extension (struct "ZS CRC
 * Extension", no CRC): the depth target of the format's depth, the
 * stencil target of its stencil (a format of both has them at the same
 * memory), linear, and the clean tile write when an aspect is cleared
 * and kept.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/lib/pan_desc.c (pan_emit_zs_crc_ext,
 * pan_emit_linear_zs_attachment, pan_emit_linear_s_attachment,
 * pan_get_clean_tile_info: a cleared aspect that is not discarded writes
 * back every tile).
 */
void
mali_surface_zs_extension(
	uint32_t *words,
	const struct mali_surface_zs_target *target)
{
	const struct mali_surface_zs_format *format;
	unsigned index;

	/* Nothing but the targets. */
	for (index = 0; index < MALI_SURFACE_ZS_BYTES / 4U; index++)
		words[index] = 0;
	format = target->format;

	/* The depth target. */
	if ((format->aspects & MALI_SURFACE_ASPECT_DEPTH) != 0U) {
		words[0] |= format->zs_write | (BLOCK_LINEAR << ZS_BLOCK_SHIFT);
		words[ZS_DEPTH_BASE] = (uint32_t)target->base;
		words[ZS_DEPTH_BASE + 1U] = (uint32_t)(target->base >> 32);
		words[ZS_DEPTH_BASE + 2U] = target->pitch;
		words[ZS_DEPTH_BASE + 3U] = (uint32_t)target->surface_bytes;
		if (target->depth_clear && target->depth_store)
			words[0] |= ZS_CLEAN_TILE;
	}

	/* The stencil target. */
	if ((format->aspects & MALI_SURFACE_ASPECT_STENCIL) != 0U) {
		words[0] |= (format->s_write << ZS_S_WRITE_SHIFT) | (BLOCK_LINEAR << ZS_S_BLOCK_SHIFT);
		words[ZS_STENCIL_BASE] = (uint32_t)target->base;
		words[ZS_STENCIL_BASE + 1U] = (uint32_t)(target->base >> 32);
		words[ZS_STENCIL_BASE + 2U] = target->pitch;
		words[ZS_STENCIL_BASE + 3U] = (uint32_t)target->surface_bytes;
		if (target->stencil_clear && target->stencil_store)
			words[0] |= ZS_CLEAN_TILE;
	}
}

/*
 * Adds a depth and stencil attachment to framebuffer parameters (words
 * 12 and 13 of struct "Framebuffer Parameters"): the stencil's clear,
 * write back and preload, the tile buffer's depth format, the depth's
 * write back, preload and clear, and the extension's presence.  An
 * aspect is written back when it is stored and preloaded when it is
 * loaded.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/lib/pan_desc.c (pan_emit_fbd for PAN_ARCH 6
 * and on: the depth format, the clears, the write enables of the aspects
 * not discarded).
 */
void
mali_surface_zs_framebuffer(
	uint32_t *framebuffer_words,
	const struct mali_surface_zs_target *target)
{
	const struct mali_surface_zs_format *format;
	uint32_t word;

	/* The stencil's clear, the depth format and the extension. */
	format = target->format;
	word = (format->z_internal & 3U) << FBD_Z_INTERNAL_SHIFT;
	word |= FBD_ZS_EXTENSION;
	if ((format->aspects & MALI_SURFACE_ASPECT_STENCIL) != 0U) {
		if (target->stencil_clear)
			word |= target->stencil_clear_value & 0xffU;
		if (target->stencil_store)
			word |= FBD_S_WRITE;
		if (target->stencil_load)
			word |= FBD_S_PRELOAD;
	}

	/* The depth's write back, preload and clear. */
	if ((format->aspects & MALI_SURFACE_ASPECT_DEPTH) != 0U) {
		if (target->depth_store)
			word |= FBD_Z_WRITE;
		if (target->depth_load)
			word |= FBD_Z_PRELOAD;
		if (target->depth_clear)
			framebuffer_words[FBD_Z_CLEAR_WORD] = target->depth_clear_bits;
	}

	framebuffer_words[FBD_ZS_WORD] |= word;
}

/* Gives the framebuffer pointer's bit that says the extension follows the parameters. */
uint32_t
mali_surface_zs_pointer(void)
{
	return POINTER_ZS_EXTENSION;
}

/*
 * Packs a draw's depth and stencil descriptor (struct "Depth/stencil").
 * Without a depth aspect the depth test always passes and writes
 * nothing; without a stencil aspect or test, both stencil compares are
 * always, nothing is written and the value masks are full -- so a
 * state of nothing gives the descriptor that passes everything.  The
 * depth comes from the program when it writes it.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c (build_zsd:
 * the tests of the aspects the pass has, the faces' ops and masks, the
 * depth source, the bias's factors).
 */
void
mali_surface_depth_stencil(
	uint32_t *words,
	const struct mali_surface_zs_state *state)
{
	uint32_t compare;
	unsigned index;

	/* The type, and the stencil that passes everything. */
	for (index = 0; index < 8U; index++)
		words[index] = 0;
	words[0] = ZSD_TYPE | (VK_COMPARE_ALWAYS << ZSD_FRONT_SHIFT) | (VK_COMPARE_ALWAYS << ZSD_BACK_SHIFT);
	words[1] = 0xffff0000U;

	/* The stencil test of a pass with a stencil aspect. */
	if (state->has_stencil && state->stencil_test) {
		words[0] = ZSD_TYPE | ZSD_STENCIL_TEST;
		words[0] |= stencil_face(&state->front) << ZSD_FRONT_SHIFT;
		words[0] |= stencil_face(&state->back) << ZSD_BACK_SHIFT;
		words[1] = (state->front.write_mask & 0xffU) | ((state->back.write_mask & 0xffU) << 8);
		words[1] |= ((state->front.compare_mask & 0xffU) << 16) | ((state->back.compare_mask & 0xffU) << 24);
		words[2] = (state->front.reference & 0xffU) | ((state->back.reference & 0xffU) << 8);
	}

	/* A stencil reference the program writes. */
	if (state->shader_stencil)
		words[0] |= ZSD_STENCIL_SHADER;

	/* The depth: culled, clamped to [0, 1], from the fixed function or the program. */
	words[4] = ZSD_DEPTH_CULL | ZSD_CLAMP_0_1 | ZSD_SOURCE_FIXED;
	if (state->shader_depth)
		words[4] = ZSD_DEPTH_CULL | ZSD_CLAMP_0_1 | ZSD_SOURCE_SHADER;

	/* The depth test and write of a pass with a depth aspect, always without. */
	compare = VK_COMPARE_ALWAYS;
	if (state->has_depth && state->depth_test) {
		compare = state->depth_compare & 7U;
		if (state->depth_write)
			words[4] |= ZSD_DEPTH_WRITE;
	}

	words[4] |= compare << ZSD_DEPTH_FUNC_SHIFT;

	/* The depth bias's units, factor and clamp, as floats. */
	if (state->has_depth && state->bias) {
		words[4] |= ZSD_DEPTH_BIAS;
		words[5] = state->bias_constant;
		words[6] = state->bias_slope;
		words[7] = state->bias_clamp;
	}
}

/* Says whether a draw writes the depth or the stencil (or its program does). */
bool
mali_surface_zs_writes(
	const struct mali_surface_zs_state *state)
{
	/* The program's own depth or stencil. */
	if (state->shader_depth || state->shader_stencil)
		return true;

	/* A depth test that writes. */
	if (state->has_depth && state->depth_test && state->depth_write)
		return true;

	/* A stencil test with a face whose write mask lets a bit through. */
	if (state->has_stencil && state->stencil_test && (state->front.write_mask & 0xffU) != 0U)
		return true;
	if (state->has_stencil && state->stencil_test && (state->back.write_mask & 0xffU) != 0U)
		return true;

	/* Nothing written. */
	return false;
}

/*
 * Chooses a draw's pixel kill and depth and stencil update operations
 * (DCD Flags 0 bits 2..3 and 4..5) in a pass with a depth and stencil
 * attachment: late when the program writes the depth or the stencil,
 * the update late too when a changed coverage meets a depth or stencil
 * write, the kill late too when the program writes memory; otherwise
 * weak early when the tests always pass, force early when they do not.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/lib/pan_earlyzs.c (analyze, best_early_mode)
 * and src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c (prepare_dcd:
 * ds_test_always_passes, the writes of the aspects).
 */
uint32_t
mali_surface_kill_flags(
	const struct mali_surface_kill *kill)
{
	const struct mali_surface_zs_state *state;
	uint32_t early;
	uint32_t pixel;
	uint32_t update;
	bool shader_zs;
	bool writes;

	/* The early operation the tests allow. */
	state = kill->state;
	early = MALI_SURFACE_KILL_FORCE_EARLY;
	if (zs_always_passes(state))
		early = MALI_SURFACE_KILL_WEAK_EARLY;
	pixel = early;
	update = early;

	/* A depth or stencil the program writes is known only late. */
	shader_zs = false;
	if (state->shader_depth || state->shader_stencil)
		shader_zs = true;
	if (shader_zs) {
		pixel = MALI_SURFACE_KILL_FORCE_LATE;
		update = MALI_SURFACE_KILL_FORCE_LATE;
	}

	/* A coverage the program changes decides what a depth or stencil write keeps. */
	writes = mali_surface_zs_writes(state);
	if (kill->coverage && writes)
		update = MALI_SURFACE_KILL_FORCE_LATE;

	/* A program that writes memory runs before it may be killed. */
	if (kill->writes_memory)
		pixel = MALI_SURFACE_KILL_FORCE_LATE;

	/* The two operations in their bits. */
	return (pixel << 2) | (update << 4);
}

/*
 * Writes a sampler's level of detail range and bias and its mipmap mode
 * into its words (the minimum and maximum LOD and the bias as float
 * bits), keeping the other fields.  Says false and leaves the words for
 * a range of level zero alone with no bias (no mipmap: one level is
 * read).
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/vulkan/panvk_vX_sampler.c (panvk_per_arch
 * create_sampler: the mipmap mode, the minimum and maximum LOD, the
 * bias).
 */
bool
mali_surface_sampler_lod(
	uint32_t *words,
	uint32_t mipmap_mode,
	uint32_t min_lod_bits,
	uint32_t max_lod_bits,
	uint32_t bias_bits)
{
	int64_t minimum;
	int64_t maximum;
	int64_t bias;
	uint32_t mode;

	/* The range and the bias in 8.8 fixed point. */
	minimum = fixed8_from_float(min_lod_bits);
	maximum = fixed8_from_float(max_lod_bits);
	bias = fixed8_from_float(bias_bits);
	if (minimum <= 0 && maximum <= 0 && bias == 0)
		return false;

	/* Clamped to the fields: unsigned 13 bits, signed 16 bits. */
	if (minimum < 0)
		minimum = 0;
	if (minimum > (int64_t)SAMPLER_LOD_MASK)
		minimum = SAMPLER_LOD_MASK;
	if (maximum < 0)
		maximum = 0;
	if (maximum > (int64_t)SAMPLER_LOD_MASK)
		maximum = SAMPLER_LOD_MASK;
	if (bias < -32768)
		bias = -32768;
	if (bias > 32767)
		bias = 32767;

	/* The mipmap mode: nearest, or trilinear for linear. */
	mode = 0U;
	if (mipmap_mode == VK_MIPMAP_LINEAR)
		mode = SAMPLER_TRILINEAR;
	words[0] = (words[0] & ~SAMPLER_MIPMAP_MASK) | (mode << SAMPLER_MIPMAP_SHIFT);

	/* The range around the compare function, the bias under the anisotropy. */
	words[1] &= ~(SAMPLER_LOD_MASK | (SAMPLER_LOD_MASK << SAMPLER_MAX_LOD_SHIFT));
	words[1] |= (uint32_t)minimum | ((uint32_t)maximum << SAMPLER_MAX_LOD_SHIFT);
	words[2] = (words[2] & ~SAMPLER_BIAS_MASK) | ((uint32_t)(int32_t)bias & SAMPLER_BIAS_MASK);

	/* Succeeded: the words take levels. */
	return true;
}

/*
 * Gives the hardware's stencil op of a Vulkan stencil op.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (enum "Stencil Op": Keep 0,
 * Replace 1, Zero 2, Invert 3, Incr Wrap 4, Decr Wrap 5, Incr Sat 6,
 * Decr Sat 7); src/panfrost/vulkan/panvk_vX_cmd_draw.c
 * (translate_stencil_op).
 */
static uint32_t
stencil_op(
	uint32_t vk_op)
{
	static const uint32_t ops[8] = { 0U, 2U, 1U, 6U, 7U, 3U, 4U, 5U };

	/* Another number keeps. */
	if (vk_op >= 8U)
		return 0U;

	/* The op of the table. */
	return ops[vk_op];
}

/* Packs one face's compare and ops as the descriptor's 12 bits: compare 0, stencil fail 3, depth fail 6, depth pass 9. */
static uint32_t
stencil_face(
	const struct mali_surface_stencil_face *face)
{
	uint32_t bits;

	/* The compare in the hardware's order, then the three ops. */
	bits = face->compare_op & 7U;
	bits |= stencil_op(face->fail_op) << 3;
	bits |= stencil_op(face->depth_fail_op) << 6;
	bits |= stencil_op(face->pass_op) << 9;

	/* The face. */
	return bits;
}

/* Says whether the depth and stencil tests always pass (none, or every compare always). */
static bool
zs_always_passes(
	const struct mali_surface_zs_state *state)
{
	/* A depth test of another compare may fail. */
	if (state->has_depth && state->depth_test && (state->depth_compare & 7U) != VK_COMPARE_ALWAYS)
		return false;

	/* A stencil test whose face compares otherwise may fail. */
	if (state->has_stencil && state->stencil_test && (state->front.compare_op & 7U) != VK_COMPARE_ALWAYS)
		return false;
	if (state->has_stencil && state->stencil_test && (state->back.compare_op & 7U) != VK_COMPARE_ALWAYS)
		return false;

	/* Every fragment passes. */
	return true;
}

/*
 * Converts a float's bits to a signed fixed-point value of 8 fraction
 * bits, rounded to nearest (a half away from zero) and saturating,
 * without floating-point instructions.
 *
 * Procedure referenced:
 * Mesa src/util/bitpack_helpers.h (util_bitpack_ufixed_clamp and
 * util_bitpack_sfixed_clamp: llroundf of the clamped value times 256;
 * read in Mesa 25.0.7, the helpers gen_pack.py of 26.2.6 calls).
 */
static int64_t
fixed8_from_float(
	uint32_t bits)
{
	int64_t magnitude;
	int32_t exponent;
	int32_t shift;
	uint64_t mantissa;

	/* Zero and a denormal are zero. */
	if (((bits >> 23) & 0xffU) == 0U)
		return 0;
	exponent = (int32_t)((bits >> 23) & 0xffU) - 127;
	mantissa = (uint64_t)((bits & 0x7fffffU) | 0x800000U);

	/* (1.m * 2^e) * 2^8 = m * 2^(e + 8 - 23); a large value (and an infinity or NaN) saturates. */
	shift = exponent + 8 - 23;
	if (exponent > 30) {
		magnitude = (int64_t)1 << 40;
	} else if (shift >= 0) {
		magnitude = (int64_t)(mantissa << shift);
	} else if (shift > -63) {
		magnitude = (int64_t)((mantissa + ((uint64_t)1 << (-shift - 1))) >> -shift);
	} else {
		magnitude = 0;
	}

	/* The sign. */
	if ((bits & 0x80000000U) != 0U)
		return -magnitude;
	return magnitude;
}
