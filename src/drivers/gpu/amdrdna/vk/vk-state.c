/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The state a draw or a dispatch writes into an indirect buffer
 * (amdrdna-vk-exec.h): the start of every buffer (the registers every draw
 * shares, then the caches emptied of what the CPU wrote), the colour
 * target, the viewport and scissor, the blend, the rasteriser, the
 * programs of a draw and of a dispatch, the draw itself and the barrier
 * between passes.
 *
 * Every image the executor draws into is linear, four bytes a pixel, its
 * rows its width's bytes rounded up to 256 apart: that is how GFX10 lays a
 * linear colour target out, so a target's size alone says its rows.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/vk/amdrdna-vk-exec.h"

/*
 * The registers only the start of a buffer sets (bytes from their space's
 * base): the pixel and vertex programs' accumulators (four each), the
 * compute program's (four), the depth unit's punch-out control and cache
 * policies, the colour unit's cache policies, the binner's second word,
 * the primitive shader's mode, the clipper's NGG control, the line
 * stipple scale, the primitive restart index, the scan converter's second
 * mode word, the second sample mask, the stereo control and the line
 * stipple's value and state.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (SPI_SHADER_USER_ACCUM_PS_0 0xb0c8, SPI_SHADER_USER_ACCUM_VS_0 0xb1c8,
 * SPI_SHADER_USER_ACCUM_ESGS_0 0xb2c8,
 * COMPUTE_USER_ACCUM_0 0xb890, DB_DFSM_CONTROL 0x28038,
 * DB_RMI_L2_CACHE_CONTROL 0x2807c, CB_RMI_GL2_CACHE_CONTROL 0x28410,
 * PA_SC_BINNER_CNTL_1 0x28c48, PA_SC_NGG_MODE_CNTL 0x28c50,
 * PA_CL_NGG_CNTL 0x28838, PA_SU_LINE_STIPPLE_SCALE 0x28828,
 * VGT_MULTI_PRIM_IB_RESET_INDX 0x2840c, PA_SC_MODE_CNTL_1 0x28a4c,
 * PA_SC_AA_MASK_X0Y1_X1Y1 0x28c3c, GE_STEREO_CNTL 0x3097c,
 * PA_SU_LINE_STIPPLE_VALUE 0x30a00, PA_SC_LINE_STIPPLE_STATE 0x30a04,
 * VGT_ESGS_RING_ITEMSIZE 0x28aac, PA_SU_HARDWARE_SCREEN_OFFSET 0x28234,
 * VGT_MULTI_PRIM_IB_RESET_EN 0x3092c, DB_EQAA 0x28804, DB_ALPHA_TO_MASK
 * 0x28b70, PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0 0x28bf8, _X1Y0_0 0x28c08,
 * _X0Y1_0 0x28c18, _X1Y1_0 0x28c28, PA_SC_CENTROID_PRIORITY_0 0x28bd4).
 */
#define STATE_PIXEL_ACCUMULATORS	0xb0c8U
#define STATE_VERTEX_ACCUMULATORS	0xb1c8U
#define STATE_PRIMITIVE_ACCUMULATORS	0xb2c8U
#define STATE_KERNEL_ACCUMULATORS	0xb890U
#define STATE_ACCUMULATORS		4U
#define STATE_DEPTH_PUNCH_OUT		0x28038U
#define STATE_DEPTH_CACHE		0x2807cU
#define STATE_COLOUR_CACHE		0x28410U
#define STATE_BINNER_LIMITS		0x28c48U
#define STATE_PRIMITIVE_SHADER_MODE	0x28c50U
#define STATE_EXPORT_ITEM_SIZE		0x28aacU
#define STATE_SCREEN_OFFSET		0x28234U
#define STATE_CLIP_NGG			0x28838U
#define STATE_STIPPLE_SCALE		0x28828U
#define STATE_RESTART_INDEX		0x2840cU
#define STATE_SCAN_MODE_MORE		0x28a4cU
#define STATE_SAMPLE_MASK_MORE		0x28c3cU
#define STATE_EQAA			0x28804U
#define STATE_ALPHA_TO_MASK		0x28b70U
#define STATE_SAMPLE_LOCATIONS		0x28bf8U
#define STATE_SAMPLE_LOCATIONS_STRIDE	0x10U
#define STATE_CENTROID_PRIORITY		0x28bd4U
#define STATE_STEREO			0x3097cU
#define STATE_RESTART_ON		0x3092cU
#define STATE_STIPPLE_VALUE		0x30a00U
#define STATE_STIPPLE_STATE		0x30a04U

/*
 * The values the start of a buffer sets: punch-out forced off (2); the
 * depth unit streaming its writes (1) and not allocating on reads that
 * miss (1) for depth, stencil and metadata; the colour unit the same for
 * colour and its masks; the binner's 1024 / 3 - 1 batches of 1023
 * primitives (the parameter cache of Navi10 is 1024 lines); 512
 * deallocations a wave; the restart index all ones; the scan converter's
 * walk (the fence of size 3, the supertile and tile orders, the
 * multi-engine discard, the end-of-vector count and rez forced, an
 * out-of-order water mark of 7 and align8); and the line stipple scale
 * of 1.0.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (DB_DFSM_CONTROL: PUNCHOUT_MODE and the enum: FORCE_OFF 2;
 * DB_RMI_L2_CACHE_CONTROL: Z_WR_POLICY 1:0, S_WR_POLICY 3:2,
 * HTILE_WR_POLICY 5:4, ZPCPSD_WR_POLICY 7:6, Z_RD_POLICY 17:16,
 * S_RD_POLICY 19:18, HTILE_RD_POLICY 21:20; CB_RMI_GL2_CACHE_CONTROL:
 * CMASK_WR_POLICY 1:0, FMASK_WR_POLICY 3:2, DCC_WR_POLICY 5:4,
 * COLOR_WR_POLICY 7:6, CMASK_RD_POLICY 17:16, FMASK_RD_POLICY 19:18,
 * DCC_RD_POLICY 21:20, COLOR_RD_POLICY 23:22; the enums WritePolicy:
 * CACHE_STREAM 1, ReadPolicy: CACHE_NOA 1; PA_SC_BINNER_CNTL_1:
 * MAX_ALLOC_COUNT 15:0, MAX_PRIM_PER_BATCH 31:16; PA_SC_NGG_MODE_CNTL:
 * MAX_DEALLOCS_IN_WAVE; PA_SC_MODE_CNTL_1: WALK_ALIGN8_PRIM_FITS_ST 2,
 * WALK_FENCE_ENABLE 3, WALK_FENCE_SIZE 6:4, SUPERTILE_WALK_ORDER_ENABLE 7,
 * TILE_WALK_ORDER_ENABLE 8, MULTI_SHADER_ENGINE_PRIM_DISCARD_ENABLE 17,
 * FORCE_EOV_CNTDWN_ENABLE 25, FORCE_EOV_REZ_ENABLE 26,
 * OUT_OF_ORDER_WATER_MARK 30:28), src/amd/common/ac_cmdbuf.c
 * (gfx10_init_graphics_preamble_state: the policies of a chip of more than
 * four render backends, the binner's count less one, 512 deallocations),
 * src/amd/common/ac_gpu_info.c (pc_lines 1024 for CHIP_NAVI10,
 * pbb_max_alloc_count = pc_lines / 3), src/amd/vulkan/radv_queue.c
 * (radv_emit_graphics: the restart index, the stipple scale) and
 * src/amd/vulkan/radv_cmd_buffer.c (radv_emit_rasterization_samples: the
 * walk of a chip of more than two tile pipes).
 */
#define STATE_PUNCH_OUT_OFF		2U
#define STATE_DEPTH_CACHE_STREAM	(1U | (1U << 2) | (1U << 4) | (1U << 6) | (1U << 16) | (1U << 18) | (1U << 20))
#define STATE_COLOUR_CACHE_STREAM	(1U | (1U << 2) | (1U << 4) | (1U << 6) | (1U << 16) | (1U << 18) | (1U << 20) | \
					 (1U << 22))
#define STATE_BINNER_LIMITS_NAVI10	((1024U / 3U - 1U) | (1023U << 16))
#define STATE_DEALLOCATIONS		512U
#define STATE_RESTART_ALL		0xffffffffU
#define STATE_SCAN_WALK			((1U << 2) | (1U << 3) | (3U << 4) | (1U << 7) | (1U << 8) | (1U << 17) | \
					 (1U << 25) | (1U << 26) | (7U << 28))
#define STATE_FLOAT_ONE			0x3f800000U

/* A float's sign bit, and 1 / 256 (the small-primitive precision of one sample). */
#define STATE_FLOAT_SIGN		0x80000000U
#define STATE_SMALL_PRIMITIVE_PRECISION	0x3b800000U

/*
 * The compute program's start values: the coherency start delay of 0x20,
 * every CU of shader arrays 0 and 1 of shader engines 0 and 1 (Navi10 has
 * two) and none of the others.
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/ac_cmdbuf.c
 * (gfx10_init_compute_preamble_state: CP_COHER_START_DELAY 0x20 below
 * GFX11, the static thread management of the engines there are) and
 * src/amd/registers/gfx9.json (COMPUTE_STATIC_THREAD_MGMT_SE0: SH0_CU_EN
 * 15:0, SH1_CU_EN 31:16).
 */
#define STATE_COHERENCY_DELAY		0x20U
#define STATE_KERNEL_UNITS_ALL		0xffffffffU

/*
 * The window, screen and generic scissors' far corner (16384 in x and y,
 * x in bits 14:0 and y in 30:16).
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/ac_cmdbuf.c
 * (gfx6_init_graphics_preamble_state: the generic scissor 16384, 16384) and
 * src/amd/registers/gfx10.json (PA_SC_WINDOW_SCISSOR_BR,
 * PA_SC_SCREEN_SCISSOR_BR, PA_SC_GENERIC_SCISSOR_BR: BR_X, BR_Y).
 */
#define STATE_SCISSOR_FAR		(16384U | (16384U << 16))
#define STATE_SCISSOR_MAX		16384

/*
 * The pixel program's depth control: early then late (1 in 5:4), or late
 * (0) for a program that writes memory, which then runs whatever the
 * depth test says (9, 10).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (DB_SHADER_CONTROL: Z_ORDER, EXEC_ON_HIER_FAIL, EXEC_ON_NOOP) and
 * src/amd/vulkan/radv_shader.c (the late depth and the two runs of a
 * program that writes memory; DUAL_QUAD_DISABLE where the chip has RB+
 * that is not allowed, as on GFX10.1, per src/amd/common/ac_gpu_info.c
 * rbplus_allowed).
 */
#define STATE_DEPTH_SHADER_WRITES	((1U << 9) | (1U << 10))

/*
 * The fields of the vertex and pixel interface: the four channels each
 * colour export writes (four bits a target), the bit of the primitive
 * type's index field, and the 12.4 limits.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json
 * (CB_SHADER_MASK: OUTPUT0_ENABLE .. OUTPUT7_ENABLE, four bits each;
 * SPI_SHADER_COL_FORMAT: COL0_EXPORT_FORMAT .. COL7, four bits each).
 */
#define STATE_EXPORT_CHANNELS		0xfU
#define STATE_TARGETS			8U

/*
 * The sampling: the depth unit's equal-quality word of every sample count
 * (high quality intersections 16, incoherent reads 17, static anchors
 * 20) and its fields of four samples' powers (the anchors 2:0, the mask
 * export 10:8, the alpha to mask 14:12); the rasteriser's spread of four
 * samples (6, in 16:13) and its exposed samples (22:20); the scan
 * converter's multisampling enable (0); the four standard locations of
 * a pixel (x, y in 1/16 pixel from the centre, four bits each: (-2, -6),
 * (6, -2), (-6, 2), (2, 6)) and their centroid order (0, 1, 2, 3 twice
 * in each word); the 16 bits of a pixel's sample mask; the alpha to
 * mask's dithered offsets (3, 1, 0, 2 in 9:8 .. 15:14, rounded, 16).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10.json (DB_EQAA:
 * MAX_ANCHOR_SAMPLES, MASK_EXPORT_NUM_SAMPLES, ALPHA_TO_MASK_NUM_SAMPLES,
 * HIGH_QUALITY_INTERSECTIONS, INCOHERENT_EQAA_READS,
 * STATIC_ANCHOR_ASSOCIATIONS; PA_SC_AA_CONFIG: MSAA_NUM_SAMPLES,
 * MAX_SAMPLE_DIST, MSAA_EXPOSED_SAMPLES; PA_SC_MODE_CNTL_0: MSAA_ENABLE;
 * PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0: S0_X .. S3_Y; DB_ALPHA_TO_MASK:
 * ALPHA_TO_MASK_ENABLE, ALPHA_TO_MASK_OFFSET0 .. 3, OFFSET_ROUND) and
 * src/amd/vulkan/radv_device.c (FILL_SREG, sample_locs_4x, max_dist_4x,
 * centroid_priority_4x 0x3210321032103210).
 */
#define STATE_EQAA_ALWAYS		((1U << 16) | (1U << 17) | (1U << 20))
#define STATE_EQAA_MASK_EXPORT_SHIFT	8U
#define STATE_EQAA_ALPHA_SHIFT		12U
#define STATE_SAMPLE_DISTANCE_4		6U
#define STATE_SAMPLE_DISTANCE_SHIFT	13U
#define STATE_SAMPLE_EXPOSED_SHIFT	20U
#define STATE_SCAN_MULTISAMPLE		(1U << 0)
#define STATE_SAMPLE_LOCATIONS_4	(0xeU | (0xaU << 4) | (0x6U << 8) | (0xeU << 12) | (0xaU << 16) | (0x2U << 20) | \
					 (0x2U << 24) | (0x6U << 28))
#define STATE_CENTROID_PRIORITY_4	0x32103210U
#define STATE_SAMPLE_MASK_PIXEL		0xffffU
#define STATE_ALPHA_TO_MASK_OFFSETS	((3U << 8) | (1U << 10) | (0U << 12) | (2U << 14) | (1U << 16))

/* The fixed point the viewport is worked in: 16 fractional bits, and 24 for the depths. */
#define STATE_FRACTION			16U
#define STATE_DEPTH_FRACTION		24U

/*
 * The guard band: twice the rasteriser's range of 32767 pixels, and the
 * widest point (8191.875 pixels), in the viewport's fixed point.
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_guardband_state: max_range 32767.0, the point size 8191.875).
 */
#define STATE_GUARD_RANGE_TWICE		((int64_t)2 * 32767 << STATE_FRACTION)
#define STATE_GUARD_POINT_PIXELS	((int64_t)8191875 * 65536 / 1000)

/*
 * The cache actions of the start of a buffer: everything the CPU wrote
 * since the last buffer (programs, tables, descriptors) read anew; and of
 * a barrier, after the colour unit's caches are written back.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cs.c
 * (gfx10_cs_emit_cache_flush: the instruction, scalar, vector, per-array
 * and level-2 invalidations of the acquire; the colour and depth unit
 * flush with metadata as a timestamped end-of-pipe event that writes back
 * and invalidates the level-2 cache, waited for in memory, then the
 * fetcher's sync).
 */
#define STATE_START_CACHES		(AMDRDNA_PM4_ACQUIRE_INSTRUCTIONS | AMDRDNA_PM4_ACQUIRE_SCALAR | \
					 AMDRDNA_PM4_ACQUIRE_VECTOR | AMDRDNA_PM4_ACQUIRE_LEVEL1 | \
					 AMDRDNA_PM4_ACQUIRE_LEVEL2_INVALIDATE | AMDRDNA_PM4_ACQUIRE_LEVEL2_WRITE_BACK | \
					 AMDRDNA_PM4_ACQUIRE_METADATA_WRITE_BACK | AMDRDNA_PM4_ACQUIRE_METADATA_INVALIDATE)
#define STATE_BARRIER_RELEASE		(AMDRDNA_PM4_RELEASE_METADATA_WRITE_BACK | AMDRDNA_PM4_RELEASE_METADATA_INVALIDATE | \
					 AMDRDNA_PM4_RELEASE_VECTOR_INVALIDATE | AMDRDNA_PM4_RELEASE_LEVEL1_INVALIDATE | \
					 AMDRDNA_PM4_RELEASE_LEVEL2_INVALIDATE | AMDRDNA_PM4_RELEASE_LEVEL2_WRITE_BACK | \
					 AMDRDNA_PM4_RELEASE_IN_ORDER)
#define STATE_BARRIER_ACQUIRE		(AMDRDNA_PM4_ACQUIRE_SCALAR | AMDRDNA_PM4_ACQUIRE_VECTOR | \
					 AMDRDNA_PM4_ACQUIRE_LEVEL1)

/*
 * A format the colour unit draws into: its layout word and how a pixel
 * program exports a colour to it.  The 8-bit normalized and sRGB formats
 * clamp what they blend; the integer formats bypass the blending; the
 * formats that are not normalized round.
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/ac_descriptors.c
 * (ac_init_cb_surface: BLEND_CLAMP for UNORM, SNORM and SRGB,
 * BLEND_BYPASS and no BLEND_CLAMP for UINT and SINT, ROUND_MODE but for
 * UNORM, SNORM and SRGB, SIMPLE_FLOAT always), src/amd/common/ac_formats.c
 * (ac_get_cb_format, ac_get_cb_number_type, ac_translate_colorswap: the
 * formats' COLOR_*, NUMBER_* and SWAP_STD, SWAP_ALT of B8G8R8A8) and
 * src/amd/common/ac_shader_util.c (ac_choose_spi_color_formats).
 */
#define STATE_TARGET_NORMALIZED	(AMDRDNA_CX_TARGET_BLEND_CLAMP | AMDRDNA_CX_TARGET_SIMPLE_FLOAT)
#define STATE_TARGET_INTEGER	(AMDRDNA_CX_TARGET_BLEND_BYPASS | AMDRDNA_CX_TARGET_SIMPLE_FLOAT | AMDRDNA_CX_TARGET_ROUND_MODE)
#define STATE_TARGET_FLOAT	(AMDRDNA_CX_TARGET_SIMPLE_FLOAT | AMDRDNA_CX_TARGET_ROUND_MODE)
#define STATE_COLOUR_FORMATS	19U

struct state_colour_format {
	uint32_t format;
	uint32_t layout;
	uint32_t export;
};

static const struct state_colour_format state_colour_formats[STATE_COLOUR_FORMATS] = {
	{ VK_FORMAT_R8G8B8A8_UNORM,
	  AMDRDNA_CX_TARGET_FORMAT_8888 | AMDRDNA_CX_TARGET_NUMBER_UNORM | AMDRDNA_CX_TARGET_SWAP_STANDARD | STATE_TARGET_NORMALIZED,
	  AMDRDNA_GFX10_COLOUR_HALVES },
	{ VK_FORMAT_B8G8R8A8_UNORM,
	  AMDRDNA_CX_TARGET_FORMAT_8888 | AMDRDNA_CX_TARGET_NUMBER_UNORM | AMDRDNA_CX_TARGET_SWAP_ALTERNATE | STATE_TARGET_NORMALIZED,
	  AMDRDNA_GFX10_COLOUR_HALVES },
	{ VK_FORMAT_R8G8B8A8_SRGB,
	  AMDRDNA_CX_TARGET_FORMAT_8888 | AMDRDNA_CX_TARGET_NUMBER_SRGB | AMDRDNA_CX_TARGET_SWAP_STANDARD | STATE_TARGET_NORMALIZED,
	  AMDRDNA_GFX10_COLOUR_HALVES },
	{ VK_FORMAT_B8G8R8A8_SRGB,
	  AMDRDNA_CX_TARGET_FORMAT_8888 | AMDRDNA_CX_TARGET_NUMBER_SRGB | AMDRDNA_CX_TARGET_SWAP_ALTERNATE | STATE_TARGET_NORMALIZED,
	  AMDRDNA_GFX10_COLOUR_HALVES },
	{ VK_FORMAT_R8G8B8A8_SNORM, AMDRDNA_CX_TARGET_FORMAT_8888 | AMDRDNA_CX_TARGET_NUMBER_SNORM | STATE_TARGET_NORMALIZED,
	  AMDRDNA_GFX10_COLOUR_HALVES },
	{ VK_FORMAT_R8G8B8A8_UINT, AMDRDNA_CX_TARGET_FORMAT_8888 | AMDRDNA_CX_TARGET_NUMBER_UINT | STATE_TARGET_INTEGER,
	  AMDRDNA_GFX10_COLOUR_UINT8 },
	{ VK_FORMAT_R8G8B8A8_SINT, AMDRDNA_CX_TARGET_FORMAT_8888 | AMDRDNA_CX_TARGET_NUMBER_SINT | STATE_TARGET_INTEGER,
	  AMDRDNA_GFX10_COLOUR_SINT8 },
	{ VK_FORMAT_R32_UINT, AMDRDNA_CX_TARGET_FORMAT_32 | AMDRDNA_CX_TARGET_NUMBER_UINT | STATE_TARGET_INTEGER, AMDRDNA_GFX10_COLOUR_R },
	{ VK_FORMAT_R32_SINT, AMDRDNA_CX_TARGET_FORMAT_32 | AMDRDNA_CX_TARGET_NUMBER_SINT | STATE_TARGET_INTEGER, AMDRDNA_GFX10_COLOUR_R },
	{ VK_FORMAT_R32_SFLOAT, AMDRDNA_CX_TARGET_FORMAT_32 | AMDRDNA_CX_TARGET_NUMBER_FLOAT | STATE_TARGET_FLOAT, AMDRDNA_GFX10_COLOUR_R },
	{ VK_FORMAT_R16G16B16A16_UINT, AMDRDNA_CX_TARGET_FORMAT_16_16_16_16 | AMDRDNA_CX_TARGET_NUMBER_UINT | STATE_TARGET_INTEGER,
	  AMDRDNA_GFX10_COLOUR_UINT16 },
	{ VK_FORMAT_R16G16B16A16_SINT, AMDRDNA_CX_TARGET_FORMAT_16_16_16_16 | AMDRDNA_CX_TARGET_NUMBER_SINT | STATE_TARGET_INTEGER,
	  AMDRDNA_GFX10_COLOUR_SINT16 },
	{ VK_FORMAT_R16G16B16A16_SFLOAT, AMDRDNA_CX_TARGET_FORMAT_16_16_16_16 | AMDRDNA_CX_TARGET_NUMBER_FLOAT | STATE_TARGET_FLOAT,
	  AMDRDNA_GFX10_COLOUR_HALVES },
	{ VK_FORMAT_R32G32_UINT, AMDRDNA_CX_TARGET_FORMAT_32_32 | AMDRDNA_CX_TARGET_NUMBER_UINT | STATE_TARGET_INTEGER, AMDRDNA_GFX10_COLOUR_GR },
	{ VK_FORMAT_R32G32_SINT, AMDRDNA_CX_TARGET_FORMAT_32_32 | AMDRDNA_CX_TARGET_NUMBER_SINT | STATE_TARGET_INTEGER, AMDRDNA_GFX10_COLOUR_GR },
	{ VK_FORMAT_R32G32_SFLOAT, AMDRDNA_CX_TARGET_FORMAT_32_32 | AMDRDNA_CX_TARGET_NUMBER_FLOAT | STATE_TARGET_FLOAT, AMDRDNA_GFX10_COLOUR_GR },
	{ VK_FORMAT_R32G32B32A32_UINT, AMDRDNA_CX_TARGET_FORMAT_32_32_32_32 | AMDRDNA_CX_TARGET_NUMBER_UINT | STATE_TARGET_INTEGER,
	  AMDRDNA_GFX10_COLOUR_ABGR },
	{ VK_FORMAT_R32G32B32A32_SINT, AMDRDNA_CX_TARGET_FORMAT_32_32_32_32 | AMDRDNA_CX_TARGET_NUMBER_SINT | STATE_TARGET_INTEGER,
	  AMDRDNA_GFX10_COLOUR_ABGR },
	{ VK_FORMAT_R32G32B32A32_SFLOAT, AMDRDNA_CX_TARGET_FORMAT_32_32_32_32 | AMDRDNA_CX_TARGET_NUMBER_FLOAT | STATE_TARGET_FLOAT,
	  AMDRDNA_GFX10_COLOUR_ABGR },
};

static int state_topology(struct amdrdna_vk_pm4 *pm4, uint32_t topology, bool primitive_shader);
static void state_vertex_program(struct amdrdna_vk_pm4 *pm4, const struct amdrdna_vk_program *vertex, uint64_t vertex_table, uint32_t base_vertex, uint32_t first_instance, uint32_t instance_mask);
static void state_primitive_shader(struct amdrdna_vk_pm4 *pm4, const struct amdrdna_vk_program *vertex, uint64_t vertex_table, uint32_t base_vertex, uint32_t first_instance, uint32_t instance_mask, const uint32_t *culling);
static void state_vertex_data(struct amdrdna_vk_pm4 *pm4, const struct amdrdna_vk_program *vertex, uint64_t vertex_table, uint32_t base_vertex, uint32_t first_instance, uint32_t instance_mask, const uint32_t *culling);
static void state_start_graphics(struct amdrdna_vk_pm4 *pm4);
static void state_start_compute(struct amdrdna_vk_pm4 *pm4);
static uint32_t state_stencil_ops(const struct amdrdna_vk_stencil_face *face);
static uint32_t state_stencil_op(uint32_t op);
static uint32_t state_stencil_masks(const struct amdrdna_vk_stencil_face *face);
static int state_blend_words(const struct amdrdna_vk_blend *blend, uint32_t *control_word, uint32_t *shortcut_word);
static bool state_blend_factor(uint32_t factor, uint32_t *code);
static bool state_factor_dual(uint32_t factor);
static bool state_blend_op(uint32_t op, uint32_t *code);
static uint32_t state_blend_shortcut(uint32_t op);
static void state_target_samples(struct amdrdna_vk_pm4 *pm4, const struct amdrdna_vk_target *target, uint32_t layout, uint32_t block, uint32_t run);
static uint32_t state_log2(uint32_t value);
static int32_t state_floor(int64_t value);
static int32_t state_ceil(int64_t value);
static uint32_t state_clamp(int32_t value);

/*
 * Starts a buffer: the context control and the registers' defaults, the
 * registers every draw and dispatch shares, then the caches emptied of
 * what the CPU wrote.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_queue.c
 * (radv_emit_graphics: the context control loading and keeping every
 * register group, the clear to the defaults, then the preamble; the
 * compute preamble after it).
 */
void
amdrdna_vk_state_start(
	struct amdrdna_vk_pm4 *pm4)
{
	/* The context control and the defaults. */
	amdrdna_vk_pm4_context_start(pm4);

	/* What draws and dispatches share. */
	state_start_graphics(pm4);
	state_start_compute(pm4);

	/* Everything the CPU wrote read anew. */
	amdrdna_vk_pm4_cache_acquire(pm4, STATE_START_CACHES);
}

/*
 * Starts a buffer of the compute queue, which takes no graphics state: the
 * registers every dispatch shares, then the caches emptied of what the
 * CPU wrote.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_queue.c
 * (radv_emit_compute: the compute preamble alone on a compute queue).
 */
void
amdrdna_vk_state_start_kernel(
	struct amdrdna_vk_pm4 *pm4)
{
	/* What dispatches share, then everything the CPU wrote read anew. */
	state_start_compute(pm4);
	amdrdna_vk_pm4_cache_acquire(pm4, STATE_START_CACHES);
}

/*
 * Reports the colour target's layout word of a format (its format,
 * number type and component swap, the blending and rounding bits, simple
 * floats); false for a format the executor does not draw into.
 */
bool
amdrdna_vk_target_format(
	uint32_t format,
	uint32_t *layout)
{
	uint32_t index;

	/* The format's entry. */
	for (index = 0U; index < STATE_COLOUR_FORMATS; index++) {
		if (state_colour_formats[index].format != format)
			continue;

		/* Succeeded: the target is drawn in the format. */
		*layout = state_colour_formats[index].layout;
		return true;
	}

	/* Any other format is not drawn into. */
	return false;
}

/*
 * Reports how a pixel program exports a colour to a target of a format
 * (AMDRDNA_GFX10_COLOUR_*), with its alpha too when the alpha covers
 * samples; four 16-bit floats for a format the executor does not draw
 * into.
 */
uint32_t
amdrdna_vk_target_export(
	uint32_t format,
	bool alpha)
{
	uint32_t index;

	/* The format's entry, the red of a 32-bit target with its alpha. */
	for (index = 0U; index < STATE_COLOUR_FORMATS; index++) {
		if (state_colour_formats[index].format != format)
			continue;
		if (alpha && state_colour_formats[index].export == AMDRDNA_GFX10_COLOUR_R)
			return AMDRDNA_GFX10_COLOUR_AR;

		/* Succeeded: the target's export. */
		return state_colour_formats[index].export;
	}

	/* Any other format as four 16-bit floats. */
	return AMDRDNA_GFX10_COLOUR_HALVES;
}

/* Reports the bytes between the rows of a linear image of four-byte pixels a width wide. */
uint32_t
amdrdna_vk_pitch(
	uint32_t width)
{
	/* The width's bytes, rounded up to 256. */
	return (width * AMDRDNA_VK_PIXEL_BYTES + AMDRDNA_VK_PITCH_ALIGN - 1U) & ~(AMDRDNA_VK_PITCH_ALIGN - 1U);
}

/*
 * Writes colour target 0 and no other (an internal quad's): as
 * amdrdna_vk_state_targets() with one target.
 */
int
amdrdna_vk_state_target(
	struct amdrdna_vk_pm4 *pm4,
	const struct amdrdna_vk_target *target)
{
	int error;

	/* The one target, the others off. */
	error = amdrdna_vk_state_targets(pm4, target, 1U);
	if (error != 0)
		return error;

	/* Succeeded: the draws after it write target 0 alone. */
	return 0;
}

/*
 * Writes the colour targets of the draws after it: each of the first
 * count (target i the i-th) with its address, view, layout, attributes,
 * size and surface, or off (its layout's format invalid) when its address
 * is 0; every target after them off.  Returns 0, or EINVAL for a format, a
 * size or an address a target cannot have.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_framebuffer_state: each bound target's registers, and
 * CB_COLORi_INFO V_028C70_COLOR_INVALID for the targets without an
 * attachment).
 */
int
amdrdna_vk_state_targets(
	struct amdrdna_vk_pm4 *pm4,
	const struct amdrdna_vk_target *targets,
	uint32_t count)
{
	const struct amdrdna_vk_target *target;
	uint32_t layout;
	uint32_t block;
	uint32_t run;
	uint32_t index;

	/* At most the targets there are. */
	if (count > STATE_TARGETS)
		return EINVAL;

	/*
	 * Each target given: a format drawn into, a size the target holds, an
	 * address on 256 bytes; one sample, or the most on a 64 KiB block with
	 * a slice it has.
	 */
	for (index = 0U; index < count; index++) {
		target = &targets[index];
		if (target->address == 0U)
			continue;
		if (!amdrdna_vk_target_format(target->format, &layout))
			return EINVAL;
		if (target->width == 0U || target->height == 0U || target->width > AMDRDNA_VK_IMAGE_SIDE_MAX ||
		    target->height > AMDRDNA_VK_IMAGE_SIDE_MAX)
			return EINVAL;
		if ((target->address & (AMDRDNA_VK_RESOURCE_ALIGN - 1U)) != 0U)
			return EINVAL;
		if (target->samples > 1U) {
			if (target->samples != AMDRDNA_VK_SAMPLES_MAX || (target->address & (AMDRDNA_VK_BLOCK_BYTES - 1U)) != 0U)
				return EINVAL;
			if (target->layers == 0U || target->slice >= target->layers || target->layers > AMDRDNA_VK_LAYERS_MAX)
				return EINVAL;
		}
	}

	/* Each target: its registers, or its layout invalid. */
	for (index = 0U; index < STATE_TARGETS; index++) {
		block = index * AMDRDNA_CX_TARGET_STRIDE;
		run = index * AMDRDNA_CX_TARGET_RUN_STRIDE;
		if (index >= count || targets[index].address == 0U) {
			amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_TARGET_LAYOUT + block, 0U);
			continue;
		}
		target = &targets[index];
		(void)amdrdna_vk_target_format(target->format, &layout);

		/* The address in two parts, the first slice and level, the layout, one sample. */
		amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_TARGET_ADDRESS + block, (uint32_t)(target->address >> 8));
		amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_TARGET_ADDRESS_HIGH + run, (uint32_t)(target->address >> 40) & 0xffU);
		if (target->samples <= 1U) {
			amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_TARGET_VIEW + block, 0U);
			amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_TARGET_LAYOUT + block, layout);
			amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_TARGET_ATTRIBUTES + block, 0U);

			/* The size, and a linear 2D surface. */
			amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_TARGET_SIZE + run,
						   (target->height - 1U) | ((target->width - 1U) << AMDRDNA_CX_TARGET_WIDTH_SHIFT));
			amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_TARGET_SURFACE + run, AMDRDNA_CX_TARGET_SURFACE_2D);
			continue;
		}
		state_target_samples(pm4, target, layout, block, run);
	}

	/* Succeeded: the draws after it write the targets. */
	return 0;
}

/*
 * Writes the registers of colour target block / run of samples
 * (AMDRDNA_VK_SAMPLES_MAX) after its address: the slice it draws (start
 * and end), the layout without compression (no FMASK), the samples and
 * fragments, the size, the surface (its layers, the render swizzle, 2D)
 * and the FMASK's and CMASK's addresses those of the image (neither is
 * used).
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/common/ac_descriptors.c
 * (ac_init_gfx10_cb_surface: SLICE_START, SLICE_MAX, COMPRESSION only
 * with FMASK, NUM_SAMPLES and NUM_FRAGMENTS the log2 of the samples,
 * MIP0_DEPTH the layers less one, RESOURCE_TYPE, RESOURCE_LEVEL 1;
 * ac_set_mutable_cb_surface_fields: COLOR_SW_MODE, CB_COLOR_CMASK and
 * CB_COLOR_FMASK the base without CMASK and FMASK) and
 * src/amd/vulkan/radv_cmd_buffer.c (radv_emit_fb_color_state: the CMASK
 * and FMASK bases and their high bytes).
 */
static void
state_target_samples(
	struct amdrdna_vk_pm4 *pm4,
	const struct amdrdna_vk_target *target,
	uint32_t layout,
	uint32_t block,
	uint32_t run)
{
	uint32_t samples;
	uint32_t base;
	uint32_t high;

	/* The slice drawn, the layout, the samples and fragments. */
	samples = state_log2(target->samples);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_TARGET_VIEW + block,
				   target->slice | (target->slice << AMDRDNA_CX_TARGET_SLICE_MAX_SHIFT));
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_TARGET_LAYOUT + block, layout);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_TARGET_ATTRIBUTES + block,
				   (samples << AMDRDNA_CX_TARGET_SAMPLES_SHIFT) | (samples << AMDRDNA_CX_TARGET_FRAGMENTS_SHIFT));

	/* The size, and a 2D surface of its layers in the render swizzle. */
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_TARGET_SIZE + run,
				   (target->height - 1U) | ((target->width - 1U) << AMDRDNA_CX_TARGET_WIDTH_SHIFT));
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_TARGET_SURFACE + run,
				   AMDRDNA_CX_TARGET_SURFACE_2D | AMDRDNA_CX_TARGET_SWIZZLE_RENDER | (target->layers - 1U));

	/* No CMASK, no FMASK: their addresses the image's. */
	base = (uint32_t)(target->address >> 8);
	high = (uint32_t)(target->address >> 40) & 0xffU;
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_TARGET_CMASK + block, base);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_TARGET_FMASK + block, base);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_TARGET_CMASK_HIGH + run, high);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_TARGET_FMASK_HIGH + run, high);
}

/*
 * Writes the sampling of the draws after it: samples a pixel (1 or
 * AMDRDNA_VK_SAMPLES_MAX), the samples' mask (the low 16 bits for each
 * pixel of a quad) and whether colour 0's alpha covers samples.  The
 * rasteriser's samples, their spread and centroid order, the standard
 * positions, the depth unit's anchors and masks of the samples, the
 * multisampling enable, and the alpha to mask with RADV's dithered
 * offsets.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_msaa_state: PA_SC_AA_CONFIG MSAA_NUM_SAMPLES,
 * MSAA_EXPOSED_SAMPLES and MAX_SAMPLE_DIST above one sample, DB_EQAA
 * HIGH_QUALITY_INTERSECTIONS, INCOHERENT_EQAA_READS,
 * STATIC_ANCHOR_ASSOCIATIONS and with samples MAX_ANCHOR_SAMPLES,
 * MASK_EXPORT_NUM_SAMPLES, ALPHA_TO_MASK_NUM_SAMPLES (PS_ITER_SAMPLES 0
 * without sample shading), PA_SC_MODE_CNTL_0 MSAA_ENABLE;
 * radv_emit_sample_mask: the mask in both halves of both
 * PA_SC_AA_MASK words; radv_emit_alpha_to_coverage_enable: the offsets
 * 3, 1, 0, 2 with OFFSET_ROUND) and src/amd/vulkan/radv_device.c
 * (sample_locs_4x, max_dist_4x, centroid_priority_4x,
 * radv_emit_default_sample_locations: the four pixels' locations and
 * PA_SC_CENTROID_PRIORITY_0 and _1).
 */
void
amdrdna_vk_state_samples(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t samples,
	uint32_t sample_mask,
	bool alpha_to_coverage)
{
	uint32_t locations[4];
	uint32_t priority[2];
	uint32_t config;
	uint32_t equal;
	uint32_t mask;
	uint32_t scan;
	uint32_t power;
	uint32_t index;

	/* One sample: its location the pixel's centre, no order, no spread. */
	power = state_log2(samples);
	config = 0U;
	equal = STATE_EQAA_ALWAYS;
	scan = AMDRDNA_CX_SCAN_VIEWPORT_SCISSOR;
	for (index = 0U; index < 4U; index++)
		locations[index] = 0U;
	priority[0] = 0U;
	priority[1] = 0U;

	/* Four: the standard locations in each pixel of a quad, their order and spread, the depth unit's samples. */
	if (samples == AMDRDNA_VK_SAMPLES_MAX) {
		config = power | (STATE_SAMPLE_DISTANCE_4 << STATE_SAMPLE_DISTANCE_SHIFT) | (power << STATE_SAMPLE_EXPOSED_SHIFT);
		equal |= power | (power << STATE_EQAA_MASK_EXPORT_SHIFT) | (power << STATE_EQAA_ALPHA_SHIFT);
		scan |= STATE_SCAN_MULTISAMPLE;
		for (index = 0U; index < 4U; index++)
			locations[index] = STATE_SAMPLE_LOCATIONS_4;
		priority[0] = STATE_CENTROID_PRIORITY_4;
		priority[1] = STATE_CENTROID_PRIORITY_4;
	}

	/* The rasteriser's samples, the depth unit's, the enable, the locations and order. */
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_SAMPLE_CONFIG, config);
	amdrdna_vk_pm4_set_context(pm4, STATE_EQAA, equal);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_SCAN_MODE, scan);
	for (index = 0U; index < 4U; index++)
		amdrdna_vk_pm4_set_context(pm4, STATE_SAMPLE_LOCATIONS + index * STATE_SAMPLE_LOCATIONS_STRIDE, locations[index]);
	amdrdna_vk_pm4_set_context_run(pm4, STATE_CENTROID_PRIORITY, priority, 2U);

	/* The mask in both pixels of each word, the alpha to mask. */
	mask = (sample_mask & STATE_SAMPLE_MASK_PIXEL) | ((sample_mask & STATE_SAMPLE_MASK_PIXEL) << 16);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_SAMPLE_MASK, mask);
	amdrdna_vk_pm4_set_context(pm4, STATE_SAMPLE_MASK_MORE, mask);
	amdrdna_vk_pm4_set_context(pm4, STATE_ALPHA_TO_MASK, STATE_ALPHA_TO_MASK_OFFSETS | (alpha_to_coverage ? 1U : 0U));
}

/* Reports the power of two a sample count is (0 for 0 or 1). */
static uint32_t
state_log2(
	uint32_t value)
{
	uint32_t power;

	/* Halved to 1. */
	power = 0U;
	while (value > 1U) {
		value >>= 1;
		power++;
	}

	/* Succeeded: the power. */
	return power;
}

/*
 * Works out a viewport's x and y transform as floats (as bits): the x
 * scale, x offset, y scale and y offset, half the size about the middle.
 */
static void
state_screen_transform(
	const uint32_t *viewport,
	uint32_t *transform)
{
	int64_t x;
	int64_t y;
	int64_t width;
	int64_t height;

	/* The floats in fixed point, then half the size and the middle. */
	x = amdrdna_vk_fixed(viewport[0], STATE_FRACTION);
	y = amdrdna_vk_fixed(viewport[1], STATE_FRACTION);
	width = amdrdna_vk_fixed(viewport[2], STATE_FRACTION);
	height = amdrdna_vk_fixed(viewport[3], STATE_FRACTION);
	transform[0] = amdrdna_vk_float_fixed(width, STATE_FRACTION + 1U);
	transform[1] = amdrdna_vk_float_fixed(2 * x + width, STATE_FRACTION + 1U);
	transform[2] = amdrdna_vk_float_fixed(height, STATE_FRACTION + 1U);
	transform[3] = amdrdna_vk_float_fixed(2 * y + height, STATE_FRACTION + 1U);
}

/*
 * Works out the six words of a culling primitive shader's user SGPRs for
 * a draw: the viewport's x and y scale and x and y offset (a viewport
 * flipped in y turned back), the small-primitive precision of one sample
 * (1 / 256, the vertex rounding's quantum), and the flags: the faces the
 * pipeline culls, whether the front face is counter-clockwise on the
 * screen (the pipeline's front face, turned by a flip), the small-primitive
 * test, culling at all for a topology of triangles, and the vertices a
 * primitive of the topology has.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_get_ngg_culling_settings and radv_emit_ngg_culling_state, lines
 * 10329-10425: no culling for points and lines, the counter-clockwise
 * front turned by the flip, the precision as samples / 2^8, the scale and
 * offset of y negated for a flip; one sample here).
 */
void
amdrdna_vk_state_culling(
	const uint32_t *viewport,
	uint32_t topology,
	uint32_t cull_mode,
	uint32_t front_face,
	uint32_t *words)
{
	uint32_t transform[4];
	uint32_t flags;
	bool flipped;
	bool counter_clockwise;

	/* The transform, y turned back when the viewport is flipped (its y scale negative). */
	state_screen_transform(viewport, transform);
	flipped = (transform[2] & STATE_FLOAT_SIGN) != 0U;
	if (flipped) {
		transform[2] ^= STATE_FLOAT_SIGN;
		transform[3] ^= STATE_FLOAT_SIGN;
	}
	words[0] = transform[0];
	words[1] = transform[2];
	words[2] = transform[1];
	words[3] = transform[3];
	words[4] = STATE_SMALL_PRIMITIVE_PRECISION;

	/* The faces, the front's turn, the small primitives; only triangles are culled. */
	flags = AMDRDNA_GFX10_CULL_SMALL;
	if ((cull_mode & VK_CULL_MODE_FRONT_BIT) != 0U)
		flags |= AMDRDNA_GFX10_CULL_FRONT;
	if ((cull_mode & VK_CULL_MODE_BACK_BIT) != 0U)
		flags |= AMDRDNA_GFX10_CULL_BACK;
	counter_clockwise = front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE;
	if (counter_clockwise != flipped)
		flags |= AMDRDNA_GFX10_CULL_FRONT_CCW;
	if (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
	    topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN)
		flags |= AMDRDNA_GFX10_CULL_ENABLED | AMDRDNA_GFX10_CULL_VERTEX_1 | AMDRDNA_GFX10_CULL_VERTEX_2;
	if (topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST || topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP)
		flags |= AMDRDNA_GFX10_CULL_VERTEX_1;
	words[5] = flags;
}

/*
 * Works out one axis of the guard band in fixed point (STATE_FRACTION
 * bits) from twice the viewport's scale and twice its offset (both in
 * fixed point): the clip adjustment, how far past the viewport (in its
 * half sizes) a primitive is left to the rasteriser's own clipping, up to
 * the 32767 pixels of its range; and the discard adjustment, past which a
 * primitive is dropped whole, 1.0 widened by half the widest point or line
 * of the topology, never past the clip adjustment.
 */
static void
state_guard_axis(
	int64_t scale2,
	int64_t offset2,
	int64_t pixels,
	int64_t *clip,
	int64_t *discard)
{
	int64_t one;
	int64_t room;

	/* The scale no smaller than half a pixel, the offset's size. */
	one = (int64_t)1 << STATE_FRACTION;
	if (scale2 < 0)
		scale2 = -scale2;
	if (offset2 < 0)
		offset2 = -offset2;
	if (scale2 < one)
		scale2 = one;

	/* The clip adjustment: the room left in the range over the scale, at least 1.0. */
	room = STATE_GUARD_RANGE_TWICE - offset2;
	*clip = one;
	if (room > scale2)
		*clip = (room << STATE_FRACTION) / scale2;

	/* The discard adjustment: half the widest primitive over the scale added to 1.0. */
	*discard = one + (pixels << STATE_FRACTION) / scale2;
	if (*discard > *clip)
		*discard = *clip;
}

/*
 * Writes the guard band of viewport 0 for a topology: the clip and
 * discard adjustments of y, then of x, as floats.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_guardband_state, lines 4698-4760: the range 32767 less the
 * offset over the scale (at least 0.5); the discard 1.0, widened for
 * points by 8191.875 pixels and for lines by their width over twice the
 * scale, at most the clip; PA_CL_GB_VERT_CLIP_ADJ, VERT_DISC_ADJ,
 * HORZ_CLIP_ADJ, HORZ_DISC_ADJ in that order).
 */
void
amdrdna_vk_state_guard_band(
	struct amdrdna_vk_pm4 *pm4,
	const uint32_t *viewport,
	uint32_t topology)
{
	uint32_t words[4];
	int64_t x;
	int64_t y;
	int64_t width;
	int64_t height;
	int64_t pixels;
	int64_t clip;
	int64_t discard;

	/* The widest primitive: a point of the largest size, a line of width 1.0, or none. */
	pixels = 0;
	if (topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
		pixels = STATE_GUARD_POINT_PIXELS;
	if (topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST || topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP)
		pixels = (int64_t)1 << STATE_FRACTION;

	/* The viewport in fixed point: twice the scale is the size, twice the offset the start doubled plus the size. */
	x = amdrdna_vk_fixed(viewport[0], STATE_FRACTION);
	y = amdrdna_vk_fixed(viewport[1], STATE_FRACTION);
	width = amdrdna_vk_fixed(viewport[2], STATE_FRACTION);
	height = amdrdna_vk_fixed(viewport[3], STATE_FRACTION);
	state_guard_axis(height, 2 * y + height, pixels, &clip, &discard);
	words[0] = amdrdna_vk_float_fixed(clip, STATE_FRACTION);
	words[1] = amdrdna_vk_float_fixed(discard, STATE_FRACTION);
	state_guard_axis(width, 2 * x + width, pixels, &clip, &discard);
	words[2] = amdrdna_vk_float_fixed(clip, STATE_FRACTION);
	words[3] = amdrdna_vk_float_fixed(discard, STATE_FRACTION);
	amdrdna_vk_pm4_set_context_run(pm4, AMDRDNA_CX_GUARD_BAND, words, 4U);
}

/*
 * Writes viewport 0 from a VkViewport's floats (as bits: x, y, width,
 * height, minimum and maximum depth): the transform, the depth range, and
 * the viewport's scissor around it.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_viewport: the scale and the translation of x, y and z, the
 * depth range as the smaller and the larger depth; radv_emit_scissor: the
 * viewport's scissor, its corners rounded out).
 */
void
amdrdna_vk_state_viewport(
	struct amdrdna_vk_pm4 *pm4,
	const uint32_t *viewport)
{
	uint32_t transform[6];
	uint32_t range[2];
	int64_t x;
	int64_t y;
	int64_t width;
	int64_t height;
	int64_t near;
	int64_t far;
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;

	/* The floats in fixed point. */
	x = amdrdna_vk_fixed(viewport[0], STATE_FRACTION);
	y = amdrdna_vk_fixed(viewport[1], STATE_FRACTION);
	width = amdrdna_vk_fixed(viewport[2], STATE_FRACTION);
	height = amdrdna_vk_fixed(viewport[3], STATE_FRACTION);
	near = amdrdna_vk_fixed(viewport[4], STATE_DEPTH_FRACTION);
	far = amdrdna_vk_fixed(viewport[5], STATE_DEPTH_FRACTION);

	/* x: half the width about its middle; y the same; z from the near depth to the far. */
	state_screen_transform(viewport, transform);
	transform[4] = amdrdna_vk_float_fixed(far - near, STATE_DEPTH_FRACTION);
	transform[5] = viewport[4];
	amdrdna_vk_pm4_set_context_run(pm4, AMDRDNA_CX_VIEWPORT_TRANSFORM, transform, 6U);

	/* The depth range, the smaller first. */
	range[0] = viewport[4];
	range[1] = viewport[5];
	if (far < near) {
		range[0] = viewport[5];
		range[1] = viewport[4];
	}
	amdrdna_vk_pm4_set_context_run(pm4, AMDRDNA_CX_VIEWPORT_DEPTH_RANGE, range, 2U);

	/* The viewport's scissor: its corners rounded out, on the screen (a negative height spans upward). */
	x0 = state_floor(width < 0 ? x + width : x);
	x1 = state_ceil(width < 0 ? x : x + width);
	y0 = state_floor(height < 0 ? y + height : y);
	y1 = state_ceil(height < 0 ? y : y + height);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_VIEWPORT_SCISSOR,
				state_clamp(x0) | (state_clamp(y0) << AMDRDNA_CX_SCISSOR_Y_SHIFT) | AMDRDNA_CX_SCISSOR_NO_WINDOW_OFFSET);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_VIEWPORT_SCISSOR + 4U, state_clamp(x1) | (state_clamp(y1) << AMDRDNA_CX_SCISSOR_Y_SHIFT));
}

/* Writes the scissor: the pixels from (x0, y0) up to, not including, (x1, y1). */
void
amdrdna_vk_state_scissor(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t x0,
	uint32_t y0,
	uint32_t x1,
	uint32_t y1)
{
	/* The near corner without the window offset, the far corner. */
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_GENERIC_SCISSOR,
				x0 | (y0 << AMDRDNA_CX_SCISSOR_Y_SHIFT) | AMDRDNA_CX_SCISSOR_NO_WINDOW_OFFSET);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_GENERIC_SCISSOR + 4U, x1 | (y1 << AMDRDNA_CX_SCISSOR_Y_SHIFT));
}

/*
 * Writes the blend of colour target 0 alone (an internal quad's): as
 * amdrdna_vk_state_blends() with one blend.
 */
int
amdrdna_vk_state_blend(
	struct amdrdna_vk_pm4 *pm4,
	const struct amdrdna_vk_blend *blend)
{
	int error;

	/* The one blend, no channel of another target written. */
	error = amdrdna_vk_state_blends(pm4, blend, 1U);
	if (error != 0)
		return error;

	/* Succeeded: the draws after it blend so. */
	return 0;
}

/*
 * Writes the blends of the first count colour targets (blend i target
 * i's), each with its blend shortcut, the channels written of each (none
 * of a target after them) and the colour control.  A dual-source blend
 * (target 0 blending with a factor of the second colour) blends target 0
 * alone, every shortcut off and the colour unit's dual quads off.
 * Returns 0, or EINVAL for a factor or a function a target does not blend
 * with.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_color_blend: per target, the alpha fields and the separate
 * alpha bit only when the alpha's factors or function differ from the
 * colour's, the factors of a minimum or maximum taken as one, the
 * shortcut off without blending; with MRT0 dual-source the other targets'
 * blends 0 and every SX_MRTi_BLEND_OPT OPT_COMB_NONE (the chip has RB+
 * registers); radv_emit_cb_render_state: CB_COLOR_CONTROL
 * DISABLE_DUAL_QUAD for a dual-source blend; radv_emit_color_write:
 * CB_TARGET_MASK four bits a target), src/amd/vulkan/radv_pipeline_graphics.h
 * (radv_can_enable_dual_src: a SRC1 factor of the colour or the alpha
 * other than with a minimum or maximum) and src/amd/common/ac_gpu_info.c
 * (has_rbplus from GFX9 on).
 */
int
amdrdna_vk_state_blends(
	struct amdrdna_vk_pm4 *pm4,
	const struct amdrdna_vk_blend *blends,
	uint32_t count)
{
	uint32_t controls[STATE_TARGETS];
	uint32_t shortcuts[STATE_TARGETS];
	uint32_t written;
	uint32_t index;
	uint32_t colour_control;
	bool dual;
	int error;

	/* At most the targets there are. */
	if (count == 0U || count > STATE_TARGETS)
		return EINVAL;

	/* Each target's blend and shortcut, and its channels written. */
	written = 0U;
	for (index = 0U; index < count; index++) {
		error = state_blend_words(&blends[index], &controls[index], &shortcuts[index]);
		if (error != 0)
			return error;
		written |= (blends[index].write_mask & STATE_EXPORT_CHANNELS) << (4U * index);
	}

	/* A dual-source blend: target 0's alone, no shortcut, no dual quads. */
	colour_control = AMDRDNA_CX_COLOUR_CONTROL_NORMAL;
	dual = amdrdna_vk_blend_dual(&blends[0]);
	if (dual) {
		for (index = 0U; index < count; index++) {
			if (index != 0U)
				controls[index] = 0U;
			shortcuts[index] = AMDRDNA_CX_BLEND_SHORTCUT_NONE;
		}
		colour_control |= AMDRDNA_CX_COLOUR_CONTROL_NO_DUAL_QUAD;
	}

	/* The blends, their shortcuts, the channels written, the normal colour mode. */
	amdrdna_vk_pm4_set_context_run(pm4, AMDRDNA_CX_BLEND, controls, count);
	amdrdna_vk_pm4_set_context_run(pm4, AMDRDNA_CX_BLEND_SHORTCUT, shortcuts, count);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_COLOUR_WRITE_MASK, written);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_COLOUR_CONTROL, colour_control);

	/* Succeeded: the draws after it blend so. */
	return 0;
}

/*
 * Works out one target's blend control word and its blend shortcut.
 * Returns 0, or EINVAL for a factor or a function the target does not
 * blend with.
 */
static int
state_blend_words(
	const struct amdrdna_vk_blend *blend,
	uint32_t *control_word,
	uint32_t *shortcut_word)
{
	uint32_t control;
	uint32_t shortcut;
	uint32_t source;
	uint32_t destination;
	uint32_t function;
	uint32_t alpha_source;
	uint32_t alpha_destination;
	uint32_t alpha_function;
	bool known;

	/* Without blending, the target is written as the program exports it. */
	control = 0U;
	shortcut = AMDRDNA_CX_BLEND_SHORTCUT_OFF;
	if (blend->enable != 0U) {
		/* The colour's and the alpha's factors and functions. */
		known = state_blend_factor(blend->colour_source, &source);
		known = state_blend_factor(blend->colour_destination, &destination) && known;
		known = state_blend_op(blend->colour_op, &function) && known;
		known = state_blend_factor(blend->alpha_source, &alpha_source) && known;
		known = state_blend_factor(blend->alpha_destination, &alpha_destination) && known;
		known = state_blend_op(blend->alpha_op, &alpha_function) && known;
		if (!known)
			return EINVAL;

		/* A minimum or a maximum takes no factors. */
		if (function == AMDRDNA_BLEND_MINIMUM || function == AMDRDNA_BLEND_MAXIMUM) {
			source = AMDRDNA_BLEND_ONE;
			destination = AMDRDNA_BLEND_ONE;
		}
		if (alpha_function == AMDRDNA_BLEND_MINIMUM || alpha_function == AMDRDNA_BLEND_MAXIMUM) {
			alpha_source = AMDRDNA_BLEND_ONE;
			alpha_destination = AMDRDNA_BLEND_ONE;
		}

		/* The colour, and the alpha apart when it differs. */
		control = source | (function << AMDRDNA_CX_BLEND_FUNCTION_SHIFT) | (destination << AMDRDNA_CX_BLEND_DESTINATION_SHIFT);
		if (alpha_source != source || alpha_destination != destination || alpha_function != function) {
			control |= (alpha_source | (alpha_function << AMDRDNA_CX_BLEND_FUNCTION_SHIFT) |
				    (alpha_destination << AMDRDNA_CX_BLEND_DESTINATION_SHIFT))
				   << AMDRDNA_CX_BLEND_ALPHA_SHIFT;
			control |= AMDRDNA_CX_BLEND_SEPARATE_ALPHA;
		}
		control |= AMDRDNA_CX_BLEND_ENABLE;

		/* The export unit keeps every channel and combines as the blend does. */
		shortcut = AMDRDNA_CX_BLEND_SHORTCUT_KEEP_ALL;
		shortcut |= state_blend_shortcut(blend->colour_op) << AMDRDNA_CX_BLEND_SHORTCUT_COLOUR_SHIFT;
		shortcut |= state_blend_shortcut(blend->alpha_op) << AMDRDNA_CX_BLEND_SHORTCUT_ALPHA_SHIFT;
	}

	/* Succeeded: the target's words. */
	*control_word = control;
	*shortcut_word = shortcut;
	return 0;
}

/*
 * Reports whether a target's blend reads the second colour of a
 * dual-source blend: it blends, and a factor of its colour or its alpha
 * is one of the second colour's (other than with a minimum or a maximum,
 * which take no factors).
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_pipeline_graphics.h
 * (radv_can_enable_dual_src, radv_is_dual_src).
 */
bool
amdrdna_vk_blend_dual(
	const struct amdrdna_vk_blend *blend)
{
	bool source;
	bool destination;
	bool takes;

	/* No blend reads nothing. */
	if (blend->enable == 0U)
		return false;

	/* The colour's factors, unless its function takes none. */
	takes = true;
	if (blend->colour_op == VK_BLEND_OP_MIN || blend->colour_op == VK_BLEND_OP_MAX)
		takes = false;
	source = state_factor_dual(blend->colour_source);
	destination = state_factor_dual(blend->colour_destination);
	if (takes && (source || destination))
		return true;

	/* The alpha's, the same. */
	takes = true;
	if (blend->alpha_op == VK_BLEND_OP_MIN || blend->alpha_op == VK_BLEND_OP_MAX)
		takes = false;
	source = state_factor_dual(blend->alpha_source);
	destination = state_factor_dual(blend->alpha_destination);
	if (takes && (source || destination))
		return true;

	/* Succeeded: the second colour is not read. */
	return false;
}

/* Reports whether a VkBlendFactor is one of the second colour's. */
static bool
state_factor_dual(
	uint32_t factor)
{
	/* The four factors of the second colour. */
	switch (factor) {
	case VK_BLEND_FACTOR_SRC1_COLOR:
	case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:
	case VK_BLEND_FACTOR_SRC1_ALPHA:
	case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:
		return true;
	default:
		break;
	}

	/* Any other is not one. */
	return false;
}

/* Writes the blend constant of the draws after it: red, green, blue and alpha (floats as their bits). */
void
amdrdna_vk_state_blend_constants(
	struct amdrdna_vk_pm4 *pm4,
	const uint32_t *constants)
{
	/* The four channels, one after the other. */
	amdrdna_vk_pm4_set_context_run(pm4, AMDRDNA_CX_BLEND_CONSTANTS, constants, 4U);
}

/*
 * Writes the rasteriser's culling (VkCullModeFlags), front face
 * (VkFrontFace) and whether it biases the depth.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_culling: CULL_FRONT and CULL_BACK from the cull mode's bits,
 * FACE from the front face as it is, the three depth bias enables from the
 * bias's).
 */
void
amdrdna_vk_state_raster(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t cull_mode,
	uint32_t front_face,
	bool depth_bias)
{
	uint32_t mode;

	/* Filled triangles, the faces culled and the face in front. */
	mode = AMDRDNA_CX_RASTER_FILLED;
	if ((cull_mode & VK_CULL_MODE_FRONT_BIT) != 0U)
		mode |= AMDRDNA_CX_RASTER_CULL_FRONT;
	if ((cull_mode & VK_CULL_MODE_BACK_BIT) != 0U)
		mode |= AMDRDNA_CX_RASTER_CULL_BACK;
	if (front_face == VK_FRONT_FACE_CLOCKWISE)
		mode |= AMDRDNA_CX_RASTER_CLOCKWISE;
	if (depth_bias)
		mode |= AMDRDNA_CX_RASTER_DEPTH_BIAS;
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_RASTER_MODE, mode);
}

/*
 * Writes the programs of a draw: each one's code address, register
 * counts and inputs, and user SGPRs (the resource table's address; the
 * vertex program's base vertex, first instance and the mask of the
 * locations fetched by instance), and what the two programs decide of the
 * vertex export, the pixel inputs, the colour exports and the depth unit.
 *
 * A vertex program run as a primitive shader is written in the geometry
 * stage's registers instead; one that culls also takes the six words of
 * amdrdna_vk_state_culling() (culling, which it then may not be NULL).
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_hw_vs, radv_emit_fragment_shader_state, radv_emit_ps_inputs,
 * radv_emit_vgt_shader_config_gfx6 and radv_emit_db_shader_control: the
 * registers each sets; the pixel inputs enabled and addressed together).
 */
void
amdrdna_vk_state_programs(
	struct amdrdna_vk_pm4 *pm4,
	const struct amdrdna_vk_program *vertex,
	const struct amdrdna_vk_program *fragment,
	uint64_t vertex_table,
	uint64_t fragment_table,
	uint32_t base_vertex,
	uint32_t first_instance,
	uint32_t instance_mask,
	const uint32_t *culling)
{
	const struct amdrdna_gfx10_registers *vs;
	const struct amdrdna_gfx10_registers *ps;
	uint32_t words[4];
	uint32_t depth;

	vs = &vertex->registers;
	ps = &fragment->registers;

	/* The vertex program, as a primitive shader or a legacy one. */
	if (vs->primitive_shader != 0U) {
		state_primitive_shader(pm4, vertex, vertex_table, base_vertex, first_instance, instance_mask, culling);
	} else {
		state_vertex_program(pm4, vertex, vertex_table, base_vertex, first_instance, instance_mask);
	}

	/* The pixel program: its code, counts and inputs, its two user SGPRs. */
	words[0] = (uint32_t)(fragment->va >> AMDRDNA_SH_CODE_LOW_SHIFT);
	words[1] = (uint32_t)(fragment->va >> AMDRDNA_SH_CODE_HIGH_SHIFT) & 0xffU;
	words[2] = ps->counts;
	words[3] = ps->inputs;
	amdrdna_vk_pm4_set_shader_run(pm4, AMDRDNA_SH_PIXEL_CODE_LOW, words, 4U);
	words[0] = (uint32_t)fragment_table;
	words[1] = (uint32_t)(fragment_table >> 32);
	amdrdna_vk_pm4_set_shader_run(pm4, AMDRDNA_SH_PIXEL_USER_DATA, words, 2U);

	/* The vertex export: the parameters, the positions, the point size, the primitive number. */
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_VERTEX_EXPORT_CONFIG, vs->vertex_export_config);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_POSITION_FORMATS, vs->position_formats);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_VERTEX_OUTPUT_CONTROL, vs->vertex_output_control);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_PRIMITIVE_ID_ENABLE, vs->primitive_id_enable);

	/* The pixel inputs: enabled and addressed alike, the interpolants, each attribute, the sprite. */
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_PIXEL_INPUT_ENABLE, ps->pixel_input_enable);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_PIXEL_INPUT_ADDRESS, ps->pixel_input_enable);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_PIXEL_INPUT_CONTROL, ps->interpolant_control);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_BARYCENTRIC_CONTROL, 0U);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_INTERPOLATION_CONTROL, ps->interpolation_control);
	if (ps->attribute_count != 0U)
		amdrdna_vk_pm4_set_context_run(pm4, AMDRDNA_CX_ATTRIBUTE_CONTROL, ps->attribute_control, ps->attribute_count);

	/* The colour exports and the channels the colour unit takes of them; the depth export. */
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_COLOUR_EXPORT_FORMATS, ps->color_formats);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_COLOUR_EXPORT_MASK, ps->color_mask);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_DEPTH_EXPORT_FORMAT, ps->depth_format);

	/*
	 * The depth unit: early then late, or late and always run for a
	 * program that writes memory; the kill for one that discards; the
	 * depth and the sample mask the program exports; no dual quads.
	 */
	depth = AMDRDNA_CX_DEPTH_SHADER_EARLY | AMDRDNA_CX_DEPTH_SHADER_NO_DUAL_QUAD;
	if (fragment->compiled != NULL && fragment->compiled->storage_used != 0U)
		depth = (depth & ~AMDRDNA_CX_DEPTH_SHADER_EARLY) | STATE_DEPTH_SHADER_WRITES;
	if (ps->discards != 0U)
		depth |= AMDRDNA_CX_DEPTH_SHADER_KILL;
	depth |= ps->depth_control;
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_DEPTH_SHADER_CONTROL, depth);

	/* No geometry program. */
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_GEOMETRY_MODE, 0U);
}

/*
 * Writes a legacy vertex program: its code, counts and inputs, its five
 * user SGPRs, and the stages of a legacy vertex program in wave32.  After
 * a primitive shader's draw (or at the buffer's first draw) the geometry
 * engine is flushed first, which GFX10 needs.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_hw_vs; radv_CmdBindPipeline: VGT_FLUSH on a change from NGG
 * to legacy, lines 7958-7967) and src/amd/common/ac_gpu_info.c
 * (has_vgt_flush_ngg_legacy_bug on GFX10, line 1256).
 */
static void
state_vertex_program(
	struct amdrdna_vk_pm4 *pm4,
	const struct amdrdna_vk_program *vertex,
	uint64_t vertex_table,
	uint32_t base_vertex,
	uint32_t first_instance,
	uint32_t instance_mask)
{
	uint32_t words[5];

	/* The flush after a primitive shader's draws, or at the buffer's first draw. */
	if (pm4->vertex_stage != AMDRDNA_VK_STAGE_LEGACY) {
		amdrdna_vk_pm4_event(pm4, AMDRDNA_PM4_EVENT_GEOMETRY_FLUSHED, 0U);
		pm4->vertex_stage = AMDRDNA_VK_STAGE_LEGACY;
	}

	/* The parameter cache not oversubscribed. */
	amdrdna_vk_pm4_set_user_config(pm4, AMDRDNA_UC_PARAMETER_CACHE, AMDRDNA_UC_PARAMETER_CACHE_PLAIN);

	/* The code, counts and inputs. */
	words[0] = (uint32_t)(vertex->va >> AMDRDNA_SH_CODE_LOW_SHIFT);
	words[1] = (uint32_t)(vertex->va >> AMDRDNA_SH_CODE_HIGH_SHIFT) & 0xffU;
	words[2] = vertex->registers.counts;
	words[3] = vertex->registers.inputs;
	amdrdna_vk_pm4_set_shader_run(pm4, AMDRDNA_SH_VERTEX_CODE_LOW, words, 4U);

	/* The table, the base vertex, the first instance and the instance mask. */
	state_vertex_data(pm4, vertex, vertex_table, base_vertex, first_instance, instance_mask, NULL);

	/* A legacy vertex program in wave32. */
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_STAGES, AMDRDNA_CX_STAGES_LEGACY_VERTEX);
}

/*
 * Writes a vertex program run as a primitive shader in wave32: its code, counts and inputs, the table in the stage's user
 * data address, its three user SGPRs (nine when it culls), the CUs it runs on, the subgroup's
 * size and output, and the stages of a passthrough primitive shader.
 * After a legacy vertex program's draw (or at the buffer's first draw) the
 * geometry engine is flushed first.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_hw_ngg, the GFX10 path; radv_emit_vgt_shader_config_gfx6,
 * the NGG stages) and src/amd/vulkan/radv_shader.c
 * (radv_precompute_registers_hw_ngg: the CU masks with a wave limit of 63,
 * the primitive export of one channel, one primitive from each, no
 * geometry instancing).
 */
static void
state_primitive_shader(
	struct amdrdna_vk_pm4 *pm4,
	const struct amdrdna_vk_program *vertex,
	uint64_t vertex_table,
	uint32_t base_vertex,
	uint32_t first_instance,
	uint32_t instance_mask,
	const uint32_t *culling)
{
	uint32_t words[2];
	uint32_t stages;
	uint32_t factor;

	/* The flush after a legacy vertex program's draws, or at the buffer's first draw. */
	if (pm4->vertex_stage != AMDRDNA_VK_STAGE_PRIMITIVE) {
		amdrdna_vk_pm4_event(pm4, AMDRDNA_PM4_EVENT_GEOMETRY_FLUSHED, 0U);
		pm4->vertex_stage = AMDRDNA_VK_STAGE_PRIMITIVE;
	}

	/* The code: its address, then the counts and inputs words. */
	words[0] = (uint32_t)(vertex->va >> AMDRDNA_SH_CODE_LOW_SHIFT);
	words[1] = (uint32_t)(vertex->va >> AMDRDNA_SH_CODE_HIGH_SHIFT) & 0xffU;
	amdrdna_vk_pm4_set_shader_run(pm4, AMDRDNA_SH_PRIMITIVE_CODE_LOW, words, 2U);
	words[0] = vertex->registers.counts;
	words[1] = vertex->registers.inputs;
	amdrdna_vk_pm4_set_shader_run(pm4, AMDRDNA_SH_PRIMITIVE_RESOURCES, words, 2U);

	/* The table, the base vertex, the first instance, the instance mask and the culling's words. */
	state_vertex_data(pm4, vertex, vertex_table, base_vertex, first_instance, instance_mask, culling);

	/*
	 * RADV's CUs and late allocation, the parameter cache oversubscribed;
	 * more of both when the program culls.
	 */
	amdrdna_vk_pm4_set_shader_indexed(pm4, AMDRDNA_SH_PRIMITIVE_UNITS, AMDRDNA_SH_UNITS_INDEX, AMDRDNA_SH_PRIMITIVE_UNITS_NGG);
	if (vertex->registers.primitive_culling != 0U) {
		factor = 2U;
		if (vertex->registers.parameter_count > 4U) {
			factor = 4U;
		} else if (vertex->registers.parameter_count > 2U) {
			factor = 3U;
		}
		amdrdna_vk_pm4_set_shader_indexed(pm4, AMDRDNA_SH_PRIMITIVE_UNITS_HIGH, AMDRDNA_SH_UNITS_INDEX, AMDRDNA_SH_PRIMITIVE_UNITS_HIGH_CULL);
		amdrdna_vk_pm4_set_user_config(pm4,
					       AMDRDNA_UC_PARAMETER_CACHE,
					       AMDRDNA_UC_PARAMETER_CACHE_OVERSUB |
					       ((AMDRDNA_NGG_CULL_PARAMETER_LINES * factor - 1U) << AMDRDNA_UC_PARAMETER_CACHE_LINES_SHIFT));
	} else {
		amdrdna_vk_pm4_set_shader_indexed(pm4, AMDRDNA_SH_PRIMITIVE_UNITS_HIGH, AMDRDNA_SH_UNITS_INDEX, AMDRDNA_SH_PRIMITIVE_UNITS_HIGH_NGG);
		amdrdna_vk_pm4_set_user_config(pm4, AMDRDNA_UC_PARAMETER_CACHE, AMDRDNA_UC_PARAMETER_CACHE_NGG);
	}

	/* The subgroup: its size, its output, one primitive from each, the primitive export's format, one instance. */
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_SUBGROUP_SIZE, AMDRDNA_CX_SUBGROUP_SIZE_NGG);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_SUBGROUP_OUTPUT, AMDRDNA_NGG_VERTICES);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_SUBGROUP_CONTROL, AMDRDNA_CX_SUBGROUP_ONE_PRIMITIVE);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_PRIMITIVE_EXPORT_FORMAT, AMDRDNA_CX_PRIMITIVE_EXPORT_ONE);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_GEOMETRY_INSTANCES, AMDRDNA_CX_GEOMETRY_ONE_INSTANCE);

	/* The stages of a primitive shader in wave32: passthrough, unless it culls (it packs its own primitives then). */
	stages = AMDRDNA_CX_STAGES_PRIMITIVE | AMDRDNA_CX_STAGES_PASSTHROUGH;
	if (vertex->registers.primitive_culling != 0U)
		stages = AMDRDNA_CX_STAGES_PRIMITIVE;
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_STAGES, stages);
}

/*
 * Writes a vertex program's user SGPRs: a legacy one's table, base
 * vertex, first instance and instance mask in one run; a primitive
 * shader's table in s[0:1], the base vertex, the first instance and the
 * instance mask in s8 .. s10 and, when it culls, the six words of
 * amdrdna_vk_state_culling() (culling) in s11 .. s16.
 */
static void
state_vertex_data(
	struct amdrdna_vk_pm4 *pm4,
	const struct amdrdna_vk_program *vertex,
	uint64_t vertex_table,
	uint32_t base_vertex,
	uint32_t first_instance,
	uint32_t instance_mask,
	const uint32_t *culling)
{
	uint32_t words[AMDRDNA_GFX10_NGG_CULL_USER_SGPRS];
	uint32_t count;
	uint32_t index;

	/* A legacy vertex program's run. */
	if (vertex->registers.primitive_shader == 0U) {
		words[0] = (uint32_t)vertex_table;
		words[1] = (uint32_t)(vertex_table >> 32);
		words[2] = base_vertex;
		words[3] = first_instance;
		words[4] = instance_mask;
		amdrdna_vk_pm4_set_shader_run(pm4, AMDRDNA_SH_VERTEX_USER_DATA, words, 5U);
		return;
	}

	/* A primitive shader's table, then its user SGPRs. */
	words[0] = (uint32_t)vertex_table;
	words[1] = (uint32_t)(vertex_table >> 32);
	amdrdna_vk_pm4_set_shader_run(pm4, AMDRDNA_SH_PRIMITIVE_TABLE, words, 2U);
	words[0] = base_vertex;
	words[1] = first_instance;
	words[2] = instance_mask;
	count = AMDRDNA_GFX10_NGG_USER_SGPRS;
	if (vertex->registers.primitive_culling != 0U) {
		for (index = 0U; index < AMDRDNA_GFX10_NGG_CULL_USER_SGPRS - AMDRDNA_GFX10_NGG_USER_SGPRS; index++)
			words[AMDRDNA_GFX10_NGG_USER_SGPRS + index] = culling[index];
		count = AMDRDNA_GFX10_NGG_CULL_USER_SGPRS;
	}
	amdrdna_vk_pm4_set_shader_run(pm4, AMDRDNA_SH_PRIMITIVE_USER_DATA, words, count);
}

/*
 * Writes only what changes between two draws of the same programs, after
 * amdrdna_vk_state_programs() wrote everything of them for an earlier
 * draw: the vertex program's user SGPRs (as there) and the pixel
 * program's table.
 */
void
amdrdna_vk_state_program_data(
	struct amdrdna_vk_pm4 *pm4,
	const struct amdrdna_vk_program *vertex,
	uint64_t vertex_table,
	uint64_t fragment_table,
	uint32_t base_vertex,
	uint32_t first_instance,
	uint32_t instance_mask,
	const uint32_t *culling)
{
	uint32_t words[2];

	/* The vertex program's, then the pixel program's table. */
	state_vertex_data(pm4, vertex, vertex_table, base_vertex, first_instance, instance_mask, culling);
	words[0] = (uint32_t)fragment_table;
	words[1] = (uint32_t)(fragment_table >> 32);
	amdrdna_vk_pm4_set_shader_run(pm4, AMDRDNA_SH_PIXEL_USER_DATA, words, 2U);
}

/*
 * Draws vertex_count generated vertices of instance_count instances as a
 * VkPrimitiveTopology.  Returns 0, or EINVAL for a topology the executor
 * does not draw.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_primitive_topology: the primitive type through the indexed
 * setter; gfx10_emit_ge_cntl; the instance count and the draw) and
 * src/amd/vulkan/radv_pipeline_graphics.h (radv_translate_prim: which
 * topology is which type).
 */
int
amdrdna_vk_state_draw(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t topology,
	bool primitive_shader,
	uint32_t vertex_count,
	uint32_t instance_count)
{
	int error;

	/* The type and the groups. */
	error = state_topology(pm4, topology, primitive_shader);
	if (error != 0)
		return error;

	/* The instances and the vertices. */
	amdrdna_vk_pm4_draw(pm4, vertex_count, instance_count);

	/* Succeeded: the draw is written. */
	return 0;
}

/*
 * Draws index_count indices of index_type (VkIndexType: 16 or 32 bits)
 * read from indices, index_room of them readable there, for
 * instance_count instances, as a VkPrimitiveTopology.  Returns 0, or
 * EINVAL for a topology or an index type the executor does not draw.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_index_buffer: the index type through the indexed setter;
 * radv_emit_draw_packets_indexed: the address of the first index, the
 * indices left in the buffer from it).
 */
int
amdrdna_vk_state_draw_indexed(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t topology,
	bool primitive_shader,
	uint32_t index_type,
	uint64_t indices,
	uint32_t index_room,
	uint32_t index_count,
	uint32_t instance_count)
{
	uint32_t type;
	int error;

	/* The index type. */
	if (index_type == VK_INDEX_TYPE_UINT16) {
		type = AMDRDNA_INDEX_16;
	} else if (index_type == VK_INDEX_TYPE_UINT32) {
		type = AMDRDNA_INDEX_32;
	} else {
		return EINVAL;
	}

	/* The primitive type and the groups. */
	error = state_topology(pm4, topology, primitive_shader);
	if (error != 0)
		return error;

	/* The index type, then the instances and the indices. */
	amdrdna_vk_pm4_set_user_config_indexed(pm4, AMDRDNA_UC_INDEX_TYPE, AMDRDNA_UC_INDEX_TYPE_INDEX, type);
	amdrdna_vk_pm4_draw_indexed(pm4, indices, index_room, index_count, instance_count);

	/* Succeeded: the draw is written. */
	return 0;
}

/*
 * Writes the depth unit's surface for the draws after it: a depth
 * target's address, size and layout (Z_32_FLOAT, or Z_16 for D16_UNORM,
 * in the 64 KiB Z swizzle, no hierarchical tiles), and its stencil plane (STENCIL_8 in the same
 * swizzle) when it has one, else no stencil; or with target NULL no depth
 * surface at all.  The depth control is the draws' (amdrdna_vk_state_depth).
 * Returns 0, or EINVAL for an address or a size the surface cannot have.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_fb_ds_state on GFX10: the render control, the view, the
 * hierarchical tile surface and base, the size, the info words and the
 * bases, the stencil's on the depth's without a stencil plane) and
 * src/amd/common/ac_descriptors.c (ac_init_gfx9_ds_surface: the format,
 * the swizzle mode, the size less one, the stencil base at its plane;
 * ac_init_ds_surface: STENCIL_8 for a surface with stencil).
 */
int
amdrdna_vk_state_depth_target(
	struct amdrdna_vk_pm4 *pm4,
	const struct amdrdna_vk_target *target)
{
	uint32_t words[7];
	uint32_t high[5];
	uint32_t base;
	uint32_t base_high;
	uint32_t stencil;
	uint32_t stencil_high;
	uint32_t stencil_layout;
	uint32_t depth_layout;

	/* No depth surface: the layouts invalid, the test off. */
	if (target == NULL) {
		amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_DEPTH_LAYOUT, 0U);
		amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_STENCIL_LAYOUT, 0U);
		amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_DEPTH_CONTROL, 0U);
		return 0;
	}

	/* A size the surface holds, an address on the 64 KiB block. */
	if (target->width == 0U || target->height == 0U || target->width > AMDRDNA_VK_IMAGE_SIDE_MAX ||
	    target->height > AMDRDNA_VK_IMAGE_SIDE_MAX)
		return EINVAL;
	if (target->address == 0U || (target->address & (AMDRDNA_VK_DEPTH_ALIGN - 1U)) != 0U)
		return EINVAL;
	if ((target->stencil_address & (AMDRDNA_VK_DEPTH_ALIGN - 1U)) != 0U)
		return EINVAL;

	/* No compression, the first slice and level, no hierarchical tiles, the size. */
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_DEPTH_RENDER, AMDRDNA_CX_DEPTH_RENDER_UNCOMPRESSED);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_DEPTH_VIEW, 0U);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_DEPTH_TILES_SURFACE, 0U);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_DEPTH_OVERRIDE, 0U);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_DEPTH_OVERRIDE2, 0U);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_DEPTH_TILES_BASE, 0U);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_DEPTH_SIZE,
				   (target->width - 1U) | ((target->height - 1U) << AMDRDNA_CX_DEPTH_SIZE_Y_SHIFT));

	/* The stencil plane, or none (its bases then the depth's). */
	base = (uint32_t)(target->address >> 8);
	base_high = (uint32_t)(target->address >> 40) & 0xffU;
	stencil = base;
	stencil_high = base_high;
	stencil_layout = AMDRDNA_CX_DEPTH_SWIZZLE_Z;
	if (target->stencil_address != 0U) {
		stencil = (uint32_t)(target->stencil_address >> 8);
		stencil_high = (uint32_t)(target->stencil_address >> 40) & 0xffU;
		stencil_layout |= AMDRDNA_CX_STENCIL_FORMAT_8;
	}

	/* The info, the layouts of depth (with its samples) and of the stencil, the bases: depth read, stencil read, depth written, stencil written. */
	words[0] = AMDRDNA_CX_DEPTH_INFO_LEVEL;
	depth_layout = AMDRDNA_CX_DEPTH_FORMAT_FLOAT | AMDRDNA_CX_DEPTH_SWIZZLE_Z;
	if (target->format == VK_FORMAT_D16_UNORM)
		depth_layout = AMDRDNA_CX_DEPTH_FORMAT_16 | AMDRDNA_CX_DEPTH_SWIZZLE_Z;
	depth_layout |= state_log2(target->samples) << AMDRDNA_CX_DEPTH_SAMPLES_SHIFT;
	words[1] = depth_layout;
	words[2] = stencil_layout;
	words[3] = base;
	words[4] = stencil;
	words[5] = base;
	words[6] = stencil;
	amdrdna_vk_pm4_set_context_run(pm4, AMDRDNA_CX_DEPTH_INFO, words, 7U);
	high[0] = base_high;
	high[1] = stencil_high;
	high[2] = base_high;
	high[3] = stencil_high;
	high[4] = 0U;
	amdrdna_vk_pm4_set_context_run(pm4, AMDRDNA_CX_DEPTH_BASE_HIGH, high, 5U);

	/* Succeeded: the draws after it test and write the surface as their depth control says. */
	return 0;
}

/*
 * Writes the depth and stencil control of the draws after it: the depth
 * test, its write and compare (VkCompareOp), and the stencil test of both
 * faces with each face's operations, compare, masks and reference (the
 * stencil words written whether the test is on or not).  Everything is off
 * without a depth surface (the caller then gives none).
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_depth_stencil_state below GFX12: the depth control with the
 * stencil and back face tests together, the stencil control and both
 * faces' reference and masks) and src/amd/vulkan/radv_pipeline_graphics.h
 * (radv_translate_stencil_op).
 */
void
amdrdna_vk_state_depth(
	struct amdrdna_vk_pm4 *pm4,
	const struct amdrdna_vk_depth_stencil *depth)
{
	uint32_t stencil[3];
	uint32_t control;

	/* The depth test with its compare, and the write. */
	control = 0U;
	if (depth->test != 0U)
		control |= AMDRDNA_CX_DEPTH_TEST | ((depth->compare & 7U) << AMDRDNA_CX_DEPTH_COMPARE_SHIFT);
	if (depth->write != 0U)
		control |= AMDRDNA_CX_DEPTH_WRITE;

	/* The stencil test of both faces, with each face's compare. */
	if (depth->stencil != 0U) {
		control |= AMDRDNA_CX_DEPTH_STENCIL_TEST | AMDRDNA_CX_DEPTH_STENCIL_BACK;
		control |= (depth->front.compare & 7U) << AMDRDNA_CX_DEPTH_STENCIL_COMPARE_SHIFT;
		control |= (depth->back.compare & 7U) << AMDRDNA_CX_DEPTH_STENCIL_BACK_COMPARE_SHIFT;
	}
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_DEPTH_CONTROL, control);

	/* Each face's operations, then each face's reference and masks. */
	stencil[0] = state_stencil_ops(&depth->front) | (state_stencil_ops(&depth->back) << AMDRDNA_CX_STENCIL_BACK_SHIFT);
	stencil[1] = state_stencil_masks(&depth->front);
	stencil[2] = state_stencil_masks(&depth->back);
	amdrdna_vk_pm4_set_context_run(pm4, AMDRDNA_CX_STENCIL_CONTROL, stencil, 3U);
}

/*
 * Writes the depth bias of the draws after it (the rasteriser turns it on
 * with the culling): the constant factor, the clamp and the slope factor
 * (bias, floats as their bits), for a depth surface of a format
 * (VkFormat: a 16-bit normalized depth, a 32-bit float one, none for 0).
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_depth_bias: the clamp, the slope times 16.0 and the constant
 * for both faces, the depth format's control of a float depth).
 */
void
amdrdna_vk_state_depth_bias(
	struct amdrdna_vk_pm4 *pm4,
	const uint32_t *bias,
	uint32_t depth_format)
{
	uint32_t words[5];
	uint32_t slope;
	uint32_t format;

	/* The clamp, then each face's scale (the slope sixteen times larger) and offset. */
	slope = amdrdna_vk_float_scale(bias[2], AMDRDNA_CX_DEPTH_BIAS_SLOPE_POWER);
	words[0] = bias[1];
	words[1] = slope;
	words[2] = bias[0];
	words[3] = slope;
	words[4] = bias[0];
	amdrdna_vk_pm4_set_context_run(pm4, AMDRDNA_CX_DEPTH_BIAS, words, 5U);

	/* The depth's format: 16 bits normalized, a 32-bit float, or none. */
	format = 0U;
	if (depth_format == VK_FORMAT_D16_UNORM)
		format = AMDRDNA_CX_DEPTH_BIAS_UNORM16;
	else if (depth_format != 0U)
		format = AMDRDNA_CX_DEPTH_BIAS_FLOAT32;
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_DEPTH_BIAS_FORMAT, format);
}


/*
 * Writes a compute program for the dispatches after it: its code address,
 * register counts and inputs, the workgroup's size, the start at 0 and
 * the resource table's address in its user SGPRs.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_compute_shader: the code address, the resource words, the
 * thread counts) and src/amd/common/ac_cmdbuf.c
 * (gfx10_init_compute_preamble_state: the start at 0 is the dispatch's).
 */
void
amdrdna_vk_state_kernel(
	struct amdrdna_vk_pm4 *pm4,
	const struct amdrdna_vk_program *kernel,
	uint64_t table)
{
	uint32_t words[6];

	/* The code, the counts and inputs, no shared VGPRs, no limits. */
	words[0] = (uint32_t)(kernel->va >> AMDRDNA_SH_CODE_LOW_SHIFT);
	words[1] = (uint32_t)(kernel->va >> AMDRDNA_SH_CODE_HIGH_SHIFT) & 0xffU;
	amdrdna_vk_pm4_set_shader_run(pm4, AMDRDNA_SH_KERNEL_CODE_LOW, words, 2U);
	words[0] = kernel->registers.counts;
	words[1] = kernel->registers.inputs;
	amdrdna_vk_pm4_set_shader_run(pm4, AMDRDNA_SH_KERNEL_RESOURCES, words, 2U);
	amdrdna_vk_pm4_set_shader(pm4, AMDRDNA_SH_KERNEL_MORE_RESOURCES, 0U);
	amdrdna_vk_pm4_set_shader(pm4, AMDRDNA_SH_KERNEL_LIMITS, 0U);

	/* The start at 0, 0, 0, then the workgroup's size. */
	words[0] = 0U;
	words[1] = 0U;
	words[2] = 0U;
	words[3] = kernel->compiled->local_size[0];
	words[4] = kernel->compiled->local_size[1];
	words[5] = kernel->compiled->local_size[2];
	amdrdna_vk_pm4_set_shader_run(pm4, AMDRDNA_SH_KERNEL_START, words, 6U);

	/* The resource table. */
	words[0] = (uint32_t)table;
	words[1] = (uint32_t)(table >> 32);
	amdrdna_vk_pm4_set_shader_run(pm4, AMDRDNA_SH_KERNEL_USER_DATA, words, 2U);
}

/*
 * Writes a barrier between passes and transfers: the pixel and compute
 * programs drained, the colour unit's metadata flushed, then an
 * end-of-pipe event that flushes the colour and depth caches, writes back
 * and invalidates the level-2 cache and writes sync_value to
 * sync_address; the engine waits for the value, the fetcher for the
 * engine, and the shader caches are invalidated.  sync_value must differ
 * from what the word held before (the session's counter).
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cs.c
 * (gfx10_cs_emit_cache_flush: the partial flushes, the metadata flush,
 * the timestamped flush and invalidation with the release's cache actions
 * waited for in memory, PFP_SYNC_ME, then the acquire of the shader
 * caches).
 */
void
amdrdna_vk_state_barrier(
	struct amdrdna_vk_pm4 *pm4,
	uint64_t sync_address,
	uint32_t sync_value)
{
	/* The programs drained, the metadata flushed. */
	amdrdna_vk_pm4_event(pm4, AMDRDNA_PM4_EVENT_PIXELS_DRAINED, AMDRDNA_PM4_EVENT_INDEX_DRAIN);
	amdrdna_vk_pm4_event(pm4, AMDRDNA_PM4_EVENT_COMPUTE_DRAINED, AMDRDNA_PM4_EVENT_INDEX_DRAIN);
	amdrdna_vk_pm4_event(pm4, AMDRDNA_PM4_EVENT_COLOUR_META_FLUSHED, 0U);

	/* The caches flushed at the end of the pipe, the value written, waited for. */
	amdrdna_vk_pm4_event_fence(pm4, AMDRDNA_PM4_EVENT_CACHES_FLUSHED, STATE_BARRIER_RELEASE, sync_address, sync_value);
	amdrdna_vk_pm4_wait_value(pm4, sync_address, sync_value);
	amdrdna_vk_pm4_fetcher_sync(pm4);

	/* The shader caches read anew. */
	amdrdna_vk_pm4_cache_acquire(pm4, STATE_BARRIER_ACQUIRE);
}

/*
 * Writes a barrier of the compute queue: the compute programs drained,
 * then an end-of-pipe event that writes back and invalidates the level-2
 * cache and writes sync_value to sync_address, waited for, and the shader
 * caches invalidated.  sync_value must differ from what the word held
 * before (the session's counter).
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cs.c
 * (gfx10_cs_emit_cache_flush on a compute queue: the compute partial
 * flush, the end-of-pipe release waited for in memory, no fetcher's
 * sync, then the acquire of the shader caches).
 */
void
amdrdna_vk_state_barrier_kernel(
	struct amdrdna_vk_pm4 *pm4,
	uint64_t sync_address,
	uint32_t sync_value)
{
	/* The compute programs drained. */
	amdrdna_vk_pm4_event(pm4, AMDRDNA_PM4_EVENT_COMPUTE_DRAINED, AMDRDNA_PM4_EVENT_INDEX_DRAIN);

	/* The caches written back at the end of the pipe, the value written, waited for. */
	amdrdna_vk_pm4_event_fence(pm4, AMDRDNA_PM4_EVENT_CACHES_FLUSHED, STATE_BARRIER_RELEASE, sync_address, sync_value);
	amdrdna_vk_pm4_wait_value(pm4, sync_address, sync_value);

	/* The shader caches read anew. */
	amdrdna_vk_pm4_cache_acquire(pm4, STATE_BARRIER_ACQUIRE);
}

/*
 * Writes the graphics registers every draw shares: the program units and
 * accumulators, the caches' policies, the binner, the scissors, the
 * rasteriser's fixed state, one sample, no depth, and the geometry
 * engine's index limits.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/common/ac_cmdbuf.c
 * (gfx10_init_graphics_preamble_state) and src/amd/vulkan/radv_queue.c
 * (radv_emit_graphics: the point size and limits, the vertex rounding, the
 * viewport enables, the interpolation control, the NGG control, the
 * restart index and the stipple scale).
 */
static void
state_start_graphics(
	struct amdrdna_vk_pm4 *pm4)
{
	static const uint32_t zeros[STATE_ACCUMULATORS] = { 0U, 0U, 0U, 0U };
	static const uint32_t guard_band[4] = { STATE_FLOAT_ONE, STATE_FLOAT_ONE, STATE_FLOAT_ONE, STATE_FLOAT_ONE };

	/* The programs run on every CU; the pixel program's requests; no accumulators; no late allocation. */
	amdrdna_vk_pm4_set_shader_indexed(pm4, AMDRDNA_SH_PIXEL_UNITS, AMDRDNA_SH_UNITS_INDEX, AMDRDNA_SH_UNITS_ALL);
	amdrdna_vk_pm4_set_shader_indexed(pm4, AMDRDNA_SH_PIXEL_UNITS_HIGH, AMDRDNA_SH_UNITS_INDEX, AMDRDNA_SH_UNITS_HIGH_ALL);
	amdrdna_vk_pm4_set_shader(pm4, AMDRDNA_SH_PIXEL_REQUESTS, AMDRDNA_SH_PIXEL_REQUEST_CONTROL);
	amdrdna_vk_pm4_set_shader_run(pm4, STATE_PIXEL_ACCUMULATORS, zeros, STATE_ACCUMULATORS);
	amdrdna_vk_pm4_set_shader_indexed(pm4, AMDRDNA_SH_VERTEX_UNITS, AMDRDNA_SH_UNITS_INDEX, AMDRDNA_SH_UNITS_ALL);
	amdrdna_vk_pm4_set_shader_indexed(pm4, AMDRDNA_SH_VERTEX_UNITS_HIGH, AMDRDNA_SH_UNITS_INDEX, AMDRDNA_SH_UNITS_HIGH_ALL);
	amdrdna_vk_pm4_set_shader(pm4, AMDRDNA_SH_VERTEX_REQUESTS, 0U);
	amdrdna_vk_pm4_set_shader(pm4, AMDRDNA_SH_VERTEX_LATE_ALLOC, 0U);
	amdrdna_vk_pm4_set_user_config(pm4, AMDRDNA_UC_PARAMETER_CACHE, AMDRDNA_UC_PARAMETER_CACHE_PLAIN);
	amdrdna_vk_pm4_set_shader_run(pm4, STATE_VERTEX_ACCUMULATORS, zeros, STATE_ACCUMULATORS);
	amdrdna_vk_pm4_set_shader_run(pm4, STATE_PRIMITIVE_ACCUMULATORS, zeros, STATE_ACCUMULATORS);

	/* The depth unit's punch-out off, both units' cache policies. */
	amdrdna_vk_pm4_set_context(pm4, STATE_DEPTH_PUNCH_OUT, STATE_PUNCH_OUT_OFF);
	amdrdna_vk_pm4_set_context(pm4, STATE_DEPTH_CACHE, STATE_DEPTH_CACHE_STREAM);
	amdrdna_vk_pm4_set_context(pm4, STATE_COLOUR_CACHE, STATE_COLOUR_CACHE_STREAM);

	/* The binner off, its limits, the primitive shader's deallocations, the reuse block. */
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_BINNER_CONTROL, AMDRDNA_CX_BINNER_OFF);
	amdrdna_vk_pm4_set_context(pm4, STATE_BINNER_LIMITS, STATE_BINNER_LIMITS_NAVI10);
	amdrdna_vk_pm4_set_context(pm4, STATE_PRIMITIVE_SHADER_MODE, STATE_DEALLOCATIONS);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_VERTEX_REUSE, 14U);
	amdrdna_vk_pm4_set_context(pm4, STATE_CLIP_NGG, 0U);
	amdrdna_vk_pm4_set_context(pm4, STATE_RESTART_INDEX, STATE_RESTART_ALL);

	/*
	 * The export stage's item size of one word and no screen offset, as
	 * RADV's preamble has them.
	 *
	 * Values referenced: Mesa 25.0.7 src/amd/common/ac_cmdbuf.c
	 * (gfx10_init_graphics_preamble_state: VGT_ESGS_RING_ITEMSIZE 1, line
	 * 480) and src/amd/vulkan/radv_queue.c (radv_emit_graphics:
	 * PA_SU_HARDWARE_SCREEN_OFFSET 0, line 771).
	 */
	amdrdna_vk_pm4_set_context(pm4, STATE_EXPORT_ITEM_SIZE, 1U);
	amdrdna_vk_pm4_set_context(pm4, STATE_SCREEN_OFFSET, 0U);

	/* The window and screen scissors over everything, the generic one until a draw sets it. */
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_WINDOW_OFFSET, 0U);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_WINDOW_SCISSOR, AMDRDNA_CX_SCISSOR_NO_WINDOW_OFFSET);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_WINDOW_SCISSOR + 4U, STATE_SCISSOR_FAR);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_CLIP_RECTANGLE_RULE, AMDRDNA_CX_CLIP_RECTANGLE_ALL);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_SCREEN_SCISSOR, 0U);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_SCREEN_SCISSOR + 4U, STATE_SCISSOR_FAR);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_GENERIC_SCISSOR, AMDRDNA_CX_SCISSOR_NO_WINDOW_OFFSET);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_GENERIC_SCISSOR + 4U, STATE_SCISSOR_FAR);

	/* The rasteriser: the edge rule, the viewport's scissor, the walk, the clipping, the viewport enables. */
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_EDGE_RULE, AMDRDNA_CX_EDGE_RULE_PLAIN);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_SCAN_MODE, AMDRDNA_CX_SCAN_VIEWPORT_SCISSOR);
	amdrdna_vk_pm4_set_context(pm4, STATE_SCAN_MODE_MORE, STATE_SCAN_WALK);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_CLIP_CONTROL, AMDRDNA_CX_CLIP_D3D);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_VIEWPORT_ENABLE, AMDRDNA_CX_VIEWPORT_ALL);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_RASTER_MODE, 0U);

	/* The vertex rounding, the guard band of 1.0 until a draw's viewport widens it, one sample with every bit of the mask. */
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_SAMPLE_CONFIG, 0U);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_VERTEX_ROUNDING, AMDRDNA_CX_VERTEX_ROUNDING_PLAIN);
	amdrdna_vk_pm4_set_context_run(pm4, AMDRDNA_CX_GUARD_BAND, guard_band, 4U);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_SAMPLE_MASK, AMDRDNA_CX_SAMPLE_MASK_ALL);
	amdrdna_vk_pm4_set_context(pm4, STATE_SAMPLE_MASK_MORE, AMDRDNA_CX_SAMPLE_MASK_ALL);

	/* Points and lines of 1.0, the small primitive filter, the stipple scale. */
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_POINT_SIZE, AMDRDNA_CX_POINT_SIZE_ONE);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_POINT_LIMITS, AMDRDNA_CX_POINT_LIMITS_ALL);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_LINE_WIDTH, AMDRDNA_CX_LINE_WIDTH_ONE);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_SMALL_PRIMITIVE_FILTER, AMDRDNA_CX_SMALL_PRIMITIVE_FILTER_ON);
	amdrdna_vk_pm4_set_context(pm4, STATE_STIPPLE_SCALE, STATE_FLOAT_ONE);

	/* No depth or stencil; the normal colour mode; no reuse off, the export unconverted. */
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_DEPTH_RENDER, 0U);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_DEPTH_LAYOUT, 0U);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_STENCIL_LAYOUT, 0U);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_DEPTH_CONTROL, 0U);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_COLOUR_CONTROL, AMDRDNA_CX_COLOUR_CONTROL_NORMAL);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_REUSE_OFF, 0U);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_EXPORT_CONVERSION, 0U);

	/* The geometry engine: every index, no offset, instance 0, no user VGPRs, no stereo, no stipple. */
	amdrdna_vk_pm4_set_user_config(pm4, AMDRDNA_UC_INDEX_MINIMUM, 0U);
	amdrdna_vk_pm4_set_user_config(pm4, AMDRDNA_UC_INDEX_OFFSET, 0U);
	amdrdna_vk_pm4_set_user_config(pm4, AMDRDNA_UC_INDEX_MAXIMUM, 0xffffffffU);
	amdrdna_vk_pm4_set_user_config(pm4, AMDRDNA_UC_INSTANCE_BASE, 0U);
	amdrdna_vk_pm4_set_user_config(pm4, STATE_STEREO, 0U);
	amdrdna_vk_pm4_set_user_config(pm4, AMDRDNA_UC_USER_VGPRS, 0U);
	amdrdna_vk_pm4_set_user_config(pm4, STATE_STIPPLE_VALUE, 0U);
	amdrdna_vk_pm4_set_user_config(pm4, STATE_STIPPLE_STATE, 0U);

	/*
	 * No primitive restart (its reset value is on), as RADV writes it for
	 * a pipeline without primitiveRestartEnable.
	 *
	 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
	 * (radv_emit_primitive_restart_enable: VGT_MULTI_PRIM_IB_RESET_EN from
	 * the dynamic state on GFX9 to GFX10.3, line 3400).
	 */
	amdrdna_vk_pm4_set_user_config(pm4, STATE_RESTART_ON, 0U);
}

/*
 * Writes the compute registers every dispatch shares: the coherency
 * delay, the CUs of each engine, no accumulators, no tunnel.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/common/ac_cmdbuf.c
 * (gfx10_init_compute_preamble_state).
 */
static void
state_start_compute(
	struct amdrdna_vk_pm4 *pm4)
{
	static const uint32_t zeros[STATE_ACCUMULATORS] = { 0U, 0U, 0U, 0U };

	/* The delay, the CUs of engines 0 and 1. */
	amdrdna_vk_pm4_set_user_config(pm4, AMDRDNA_UC_COHERENCY_DELAY, STATE_COHERENCY_DELAY);
	amdrdna_vk_pm4_set_shader(pm4, AMDRDNA_SH_KERNEL_UNITS_ENGINE0, STATE_KERNEL_UNITS_ALL);
	amdrdna_vk_pm4_set_shader(pm4, AMDRDNA_SH_KERNEL_UNITS_ENGINE1, STATE_KERNEL_UNITS_ALL);
	amdrdna_vk_pm4_set_shader(pm4, AMDRDNA_SH_KERNEL_UNITS_ENGINE2, 0U);
	amdrdna_vk_pm4_set_shader(pm4, AMDRDNA_SH_KERNEL_UNITS_ENGINE3, 0U);

	/* No accumulators, no tunnel. */
	amdrdna_vk_pm4_set_shader_run(pm4, STATE_KERNEL_ACCUMULATORS, zeros, STATE_ACCUMULATORS);
	amdrdna_vk_pm4_set_shader(pm4, AMDRDNA_SH_KERNEL_TUNNEL, 0U);
}

/* Gives a face's three stencil operations as the stencil control's front face fields hold them. */
static uint32_t
state_stencil_ops(
	const struct amdrdna_vk_stencil_face *face)
{
	uint32_t ops;

	/* The failed stencil test's, the passed test's and the failed depth test's. */
	ops = state_stencil_op(face->fail);
	ops |= state_stencil_op(face->pass) << AMDRDNA_CX_STENCIL_PASS_SHIFT;
	ops |= state_stencil_op(face->depth_fail) << AMDRDNA_CX_STENCIL_DEPTH_FAIL_SHIFT;

	/* Succeeded: the face's operations. */
	return ops;
}

/*
 * Gives the depth unit's stencil operation of a VkStencilOp (keep for one
 * it does not know).
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_pipeline_graphics.h
 * (radv_translate_stencil_op: which VkStencilOp is which; replace is the
 * test's reference).
 */
static uint32_t
state_stencil_op(
	uint32_t op)
{
	uint32_t code;

	/* Each operation's own. */
	switch (op) {
	case VK_STENCIL_OP_ZERO:
		code = AMDRDNA_STENCIL_ZERO;
		break;
	case VK_STENCIL_OP_REPLACE:
		code = AMDRDNA_STENCIL_REPLACE;
		break;
	case VK_STENCIL_OP_INCREMENT_AND_CLAMP:
		code = AMDRDNA_STENCIL_ADD_CLAMP;
		break;
	case VK_STENCIL_OP_DECREMENT_AND_CLAMP:
		code = AMDRDNA_STENCIL_SUBTRACT_CLAMP;
		break;
	case VK_STENCIL_OP_INVERT:
		code = AMDRDNA_STENCIL_INVERT;
		break;
	case VK_STENCIL_OP_INCREMENT_AND_WRAP:
		code = AMDRDNA_STENCIL_ADD_WRAP;
		break;
	case VK_STENCIL_OP_DECREMENT_AND_WRAP:
		code = AMDRDNA_STENCIL_SUBTRACT_WRAP;
		break;
	default:
		code = AMDRDNA_STENCIL_KEEP;
		break;
	}

	/* Succeeded: the unit's operation. */
	return code;
}

/* Gives a face's reference, compare mask and write mask as its reference and mask word holds them, the operation value 1. */
static uint32_t
state_stencil_masks(
	const struct amdrdna_vk_stencil_face *face)
{
	uint32_t word;

	/* The reference, the masks, the operation value. */
	word = face->reference & 0xffU;
	word |= (face->compare_mask & 0xffU) << AMDRDNA_CX_STENCIL_COMPARE_MASK_SHIFT;
	word |= (face->write_mask & 0xffU) << AMDRDNA_CX_STENCIL_WRITE_MASK_SHIFT;
	word |= AMDRDNA_CX_STENCIL_OPERATION_ONE;

	/* Succeeded: the face's word. */
	return word;
}

/*
 * Reports the blend factor of a VkBlendFactor; false for one the colour
 * unit does not have.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_pipeline_graphics.h
 * (radv_translate_blend_factor: which VkBlendFactor is which factor).
 */
static bool
state_blend_factor(
	uint32_t factor,
	uint32_t *code)
{
	/* Each factor's own. */
	switch (factor) {
	case VK_BLEND_FACTOR_ZERO:
		*code = AMDRDNA_BLEND_ZERO;
		break;
	case VK_BLEND_FACTOR_ONE:
		*code = AMDRDNA_BLEND_ONE;
		break;
	case VK_BLEND_FACTOR_SRC_COLOR:
		*code = AMDRDNA_BLEND_SOURCE_COLOUR;
		break;
	case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
		*code = AMDRDNA_BLEND_ONE_MINUS_SOURCE_COLOUR;
		break;
	case VK_BLEND_FACTOR_DST_COLOR:
		*code = AMDRDNA_BLEND_TARGET_COLOUR;
		break;
	case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
		*code = AMDRDNA_BLEND_ONE_MINUS_TARGET_COLOUR;
		break;
	case VK_BLEND_FACTOR_SRC_ALPHA:
		*code = AMDRDNA_BLEND_SOURCE_ALPHA;
		break;
	case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
		*code = AMDRDNA_BLEND_ONE_MINUS_SOURCE_ALPHA;
		break;
	case VK_BLEND_FACTOR_DST_ALPHA:
		*code = AMDRDNA_BLEND_TARGET_ALPHA;
		break;
	case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
		*code = AMDRDNA_BLEND_ONE_MINUS_TARGET_ALPHA;
		break;
	case VK_BLEND_FACTOR_CONSTANT_COLOR:
		*code = AMDRDNA_BLEND_CONSTANT_COLOUR;
		break;
	case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
		*code = AMDRDNA_BLEND_ONE_MINUS_CONSTANT_COLOUR;
		break;
	case VK_BLEND_FACTOR_CONSTANT_ALPHA:
		*code = AMDRDNA_BLEND_CONSTANT_ALPHA;
		break;
	case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:
		*code = AMDRDNA_BLEND_ONE_MINUS_CONSTANT_ALPHA;
		break;
	case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:
		*code = AMDRDNA_BLEND_SOURCE_ALPHA_SATURATED;
		break;
	case VK_BLEND_FACTOR_SRC1_COLOR:
		*code = AMDRDNA_BLEND_SECOND_COLOUR;
		break;
	case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:
		*code = AMDRDNA_BLEND_ONE_MINUS_SECOND_COLOUR;
		break;
	case VK_BLEND_FACTOR_SRC1_ALPHA:
		*code = AMDRDNA_BLEND_SECOND_ALPHA;
		break;
	case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:
		*code = AMDRDNA_BLEND_ONE_MINUS_SECOND_ALPHA;
		break;
	default:
		return false;
	}

	/* Succeeded: the factor is the unit's. */
	return true;
}

/*
 * Reports the blend function of a VkBlendOp; false for one the colour
 * unit does not have.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_pipeline_graphics.h
 * (radv_translate_blend_function: subtract is the source less the target,
 * reverse subtract the target less the source).
 */
static bool
state_blend_op(
	uint32_t op,
	uint32_t *code)
{
	/* Each function's own. */
	switch (op) {
	case VK_BLEND_OP_ADD:
		*code = AMDRDNA_BLEND_ADD;
		break;
	case VK_BLEND_OP_SUBTRACT:
		*code = AMDRDNA_BLEND_SUBTRACT;
		break;
	case VK_BLEND_OP_REVERSE_SUBTRACT:
		*code = AMDRDNA_BLEND_REVERSE_SUBTRACT;
		break;
	case VK_BLEND_OP_MIN:
		*code = AMDRDNA_BLEND_MINIMUM;
		break;
	case VK_BLEND_OP_MAX:
		*code = AMDRDNA_BLEND_MAXIMUM;
		break;
	default:
		return false;
	}

	/* Succeeded: the function is the unit's. */
	return true;
}

/* Gives the export unit's combination of a blend function (one the blend function check took). */
static uint32_t
state_blend_shortcut(
	uint32_t op)
{
	uint32_t code;

	/* Each function's own; add for any other (the blend refused it before). */
	switch (op) {
	case VK_BLEND_OP_SUBTRACT:
		code = AMDRDNA_SHORTCUT_SUBTRACT;
		break;
	case VK_BLEND_OP_REVERSE_SUBTRACT:
		code = AMDRDNA_SHORTCUT_REVERSE_SUBTRACT;
		break;
	case VK_BLEND_OP_MIN:
		code = AMDRDNA_SHORTCUT_MINIMUM;
		break;
	case VK_BLEND_OP_MAX:
		code = AMDRDNA_SHORTCUT_MAXIMUM;
		break;
	default:
		code = AMDRDNA_SHORTCUT_ADD;
		break;
	}

	/* Succeeded: the export unit's combination. */
	return code;
}

/* Reports the largest whole number at or below a fixed point value of STATE_FRACTION fractional bits. */
static int32_t
state_floor(
	int64_t value)
{
	/* Toward minus infinity on either side of zero. */
	if (value >= 0)
		return (int32_t)(value >> STATE_FRACTION);
	return -(int32_t)((-value + ((int64_t)1 << STATE_FRACTION) - 1) >> STATE_FRACTION);
}

/* Reports the smallest whole number at or above a fixed point value of STATE_FRACTION fractional bits. */
static int32_t
state_ceil(
	int64_t value)
{
	/* Toward plus infinity on either side of zero. */
	if (value >= 0)
		return (int32_t)((value + ((int64_t)1 << STATE_FRACTION) - 1) >> STATE_FRACTION);
	return -(int32_t)((-value) >> STATE_FRACTION);
}

/* Reports a coordinate on the screen: from 0 to 16384. */
static uint32_t
state_clamp(
	int32_t value)
{
	/* Within the screen. */
	if (value < 0)
		return 0U;
	if (value > STATE_SCISSOR_MAX)
		return (uint32_t)STATE_SCISSOR_MAX;
	return (uint32_t)value;
}

/*
 * Writes the primitive type of a VkPrimitiveTopology and the geometry
 * engine's groups.  Returns 0, or EINVAL for a topology the executor does
 * not draw.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_primitive_topology: the primitive type through the indexed
 * setter; gfx10_emit_ge_cntl) and src/amd/vulkan/radv_pipeline_graphics.h
 * (radv_translate_prim: which topology is which type).
 */
static int
state_topology(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t topology,
	bool primitive_shader)
{
	uint32_t type;
	uint32_t output;
	uint32_t groups;

	/*
	 * The primitive type.
	 *
	 * Value referenced: Mesa 25.0.7 src/amd/registers/gfx10.json (the enum
	 * VGT_DI_PRIM_TYPE: DI_PT_TRIFAN 5).
	 */
	switch (topology) {
	case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
		type = AMDRDNA_PRIMITIVE_POINTS;
		output = AMDRDNA_OUTPUT_POINTS;
		break;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
		type = AMDRDNA_PRIMITIVE_LINES;
		output = AMDRDNA_OUTPUT_LINES;
		break;
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
		type = AMDRDNA_PRIMITIVE_LINE_STRIP;
		output = AMDRDNA_OUTPUT_LINES;
		break;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
		type = AMDRDNA_PRIMITIVE_TRIANGLES;
		output = AMDRDNA_OUTPUT_TRIANGLES;
		break;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
		type = AMDRDNA_PRIMITIVE_TRIANGLE_STRIP;
		output = AMDRDNA_OUTPUT_TRIANGLES;
		break;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
		type = 5U;
		output = AMDRDNA_OUTPUT_TRIANGLES;
		break;
	default:
		return EINVAL;
	}

	/* The groups: a primitive shader's subgroup, or the legacy groups. */
	groups = AMDRDNA_UC_GEOMETRY_GROUPS;
	if (primitive_shader)
		groups = AMDRDNA_UC_GEOMETRY_GROUPS_NGG;

	/* The type, the type a primitive shader exports, and the groups. */
	amdrdna_vk_pm4_set_user_config_indexed(pm4, AMDRDNA_UC_PRIMITIVE_TYPE, AMDRDNA_UC_PRIMITIVE_TYPE_INDEX, type);
	amdrdna_vk_pm4_set_context(pm4, AMDRDNA_CX_OUTPUT_PRIMITIVE, output);
	amdrdna_vk_pm4_set_user_config(pm4, AMDRDNA_UC_GEOMETRY_CONTROL, groups);

	/* Succeeded: the type is written. */
	return 0;
}
