/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The depth load and store words of a PowerVR render.  See
 * render-depth.h.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libc/vulkan/vulkan_core.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/render-depth.h"
#include "drivers/gpu/powervr/render-pass.h"

/*
 * The fields of CR_ISP_ZLSCTL and the load base.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/cr.xml (ISP_ZLSCTL
 * forcezload 1, forcezstore 2, zlsextent_x_z 3..12, loadtwiddled 13,
 * zloaden 15, storetwiddled 17, zstoreen 19, zloadformat 23..24,
 * zstoreformat 25..26, zlsextent_y_z 27..36; ZLS_FORMAT_TYPE F32Z 0,
 * 16BITINT 2; ISP_ZLOAD_BASE addr 4..39 shifted by 4).
 */
#define ZLS_FORCE_LOAD		(1ULL << 1)
#define ZLS_FORCE_STORE		(1ULL << 2)
#define ZLS_EXTENT_X_SHIFT	3U
#define ZLS_LOAD_TWIDDLED	(1ULL << 13)
#define ZLS_LOAD		(1ULL << 15)
#define ZLS_STORE_TWIDDLED	(1ULL << 17)
#define ZLS_STORE		(1ULL << 19)
#define ZLS_LOAD_FORMAT_SHIFT	23U
#define ZLS_STORE_FORMAT_SHIFT	25U
#define ZLS_EXTENT_Y_SHIFT	27U
#define ZLS_EXTENT_MAX		0x3ffU
#define ZLS_FORMAT_F32		0U
#define ZLS_FORMAT_16		2U
#define BASE_MASK		0xfffffffff0ULL

/*
 * The stencil's fields of CR_ISP_ZLSCTL and the packed depth's format.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/csbgen/rogue/cr.xml (ISP_ZLSCTL sloaden
 * 14, sstoreen 18; ZLS_FORMAT_TYPE 24BITINT 1, packed depth and stencil;
 * ISP_STENCIL_LOAD_BASE addr 4..39 shifted by 4, enable 0).
 */
#define ZLS_STENCIL_LOAD	(1ULL << 14)
#define ZLS_STENCIL_STORE	(1ULL << 18)
#define ZLS_FORMAT_24		1U

/*
 * The tiles of the BXE-4-32: the ISP's 16 x 16 tiles counted in pairs,
 * and the ZLS tile (twice the tile with the simple parameter format
 * version 2) a buffer must reach without alignment transfers.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/include/hwdef/rogue_hw_utils.h
 * (rogue_get_isp_num_tiles_xy: one sample, the edge over 16 rounded up,
 * aligned to 2 with simple_internal_parameter_format;
 * rogue_get_zls_tile_size_xy: the tile doubled with version 2);
 * src/imagination/common/device_info/bxe-4-32.h (tile_size 16,
 * simple_parameter_format_version 2, no zls_subtile).
 */
#define ISP_TILE		16U
#define ZLS_TILE		32U

static uint32_t unorm16_from_float(uint32_t bits);
static uint32_t unorm24_from_float(uint32_t bits);

/*
 * Packs the depth words of a job's depth target.  The depth is stored
 * when its store operation is STORE, loaded when its load operation is
 * LOAD or when a stored depth's render area misses part of it (the load
 * keeps the depth outside the area); a cleared depth starts from the
 * background depth.  A stencil beside the depth (D24_UNORM_S8_UINT) is
 * loaded and stored the same way by its own operations, and a cleared
 * one starts from the background stencil.  Refuses formats other than
 * D32_SFLOAT, D16_UNORM and D24_UNORM_S8_UINT and buffers below a ZLS
 * tile that are loaded or stored (ENOTSUP, they need Mesa's alignment
 * transfers), and a misaligned address (EINVAL).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_sub_cmd_gfx_job_init: zls_format F32Z for D32_SFLOAT, 16BITINT for
 * D16_UNORM; d_store from the store, d_load when stored and the area not
 * tile aligned or from LOAD; process_empty_tiles when ZLS is used;
 * pvr_sub_cmd_gfx_requires_ds_subtile_alignment);
 * src/imagination/vulkan/pvr_arch_job_render.c (pvr_frag_state_stream_init:
 * ZLSCTL with the extent in ISP tiles less one over the physical extent
 * aligned to the 16-pixel IPF tile, twiddled load and store for a
 * twiddled buffer, the formats, zloaden, zstoreen and the forced ones;
 * ZLOAD_BASE the buffer; BGOBJDEPTH the clear value as F32 bits or
 * _mesa_float_to_unorm(depth, 16); zls_stride the layer size);
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_sub_cmd_gfx_job_init: zls_format 24BITINT for D24_UNORM_S8_UINT,
 * the stencil's load and store from its own operations) and
 * pvr_arch_job_render.c (pvr_frag_state_stream_init: sloaden and
 * sstoreen, forcezload and forcezstore of either aspect,
 * ISP_STENCIL_LOAD_BASE the depth's buffer with enable off when the
 * depth is there, BGOBJDEPTH _mesa_float_to_unorm(depth, 24) for
 * 24BITINT, BGOBJVALS stencil the clear value, sls_stride the layer
 * size).
 */
int
powervr_render_zls_words(
	const struct powervr_render_depth *depth,
	struct powervr_render_zls *zls)
{
	uint64_t format;
	uint32_t tiles_x;
	uint32_t tiles_y;
	uint32_t scale;
	bool stencil;
	bool load;
	bool store;
	bool load_stencil;
	bool store_stencil;

	/* Refuses missing input or result. */
	if (depth == NULL || zls == NULL)
		return EINVAL;
	*zls = (struct powervr_render_zls){ 0 };

	/* No depth target: the F32 background of 1.0. */
	if (!depth->present) {
		zls->background_depth = 0x3f800000U;
		return 0;
	}

	/* The format. */
	if (depth->format == NULL)
		return EINVAL;
	if (depth->format->vk_format == VK_FORMAT_D32_SFLOAT) {
		format = ZLS_FORMAT_F32;
		zls->background_depth = depth->clear_depth;
	} else if (depth->format->vk_format == VK_FORMAT_D16_UNORM) {
		format = ZLS_FORMAT_16;
		zls->background_depth = unorm16_from_float(depth->clear_depth);
	} else if (depth->format->vk_format == VK_FORMAT_D24_UNORM_S8_UINT) {
		format = ZLS_FORMAT_24;
		zls->background_depth = unorm24_from_float(depth->clear_depth);
	} else {
		return ENOTSUP;
	}
	stencil = (depth->format->kind & POWERVR_FORMAT_STENCIL) != 0;

	/* What the ISP loads and stores of the depth and of the stencil. */
	store = depth->store_op == VK_ATTACHMENT_STORE_OP_STORE;
	load = depth->load_op == VK_ATTACHMENT_LOAD_OP_LOAD || (store && depth->partial);
	store_stencil = stencil && depth->stencil_store_op == VK_ATTACHMENT_STORE_OP_STORE;
	load_stencil = stencil && (depth->stencil_load_op == VK_ATTACHMENT_LOAD_OP_LOAD || (store_stencil && depth->partial));

	/* The buffer: aligned, at least a ZLS tile when it is loaded or stored. */
	if ((depth->address & ~BASE_MASK) != 0)
		return EINVAL;
	if ((load || store || load_stencil || store_stencil) && (depth->physical_width < ZLS_TILE || depth->physical_height < ZLS_TILE))
		return ENOTSUP;

	/*
	 * The extent in ISP tiles over the physical extent, in pairs, less
	 * one; four samples are two across and two down in the ISP's tiles.
	 *
	 * Procedure referenced: Mesa 26.2.6
	 * src/imagination/include/hwdef/rogue_hw_utils.h
	 * (rogue_get_isp_num_tiles_xy: the edge times
	 * rogue_get_isp_scale_xy_from_samples, 2 x 2 for 4 samples, over the
	 * 16-sample tile of one ISP sample a pixel).
	 */
	if (depth->samples != 0U && depth->samples != 1U && depth->samples != 4U)
		return ENOTSUP;
	scale = depth->samples == 4U ? 2U : 1U;
	tiles_x = (((depth->physical_width * scale + ISP_TILE - 1U) / ISP_TILE) + 1U) & ~1U;
	tiles_y = (((depth->physical_height * scale + ISP_TILE - 1U) / ISP_TILE) + 1U) & ~1U;
	if (tiles_x == 0 || tiles_y == 0 || tiles_x - 1U > ZLS_EXTENT_MAX || tiles_y - 1U > ZLS_EXTENT_MAX)
		return E2BIG;

	/* The control word. */
	zls->control = ((uint64_t)(tiles_x - 1U) << ZLS_EXTENT_X_SHIFT) | ((uint64_t)(tiles_y - 1U) << ZLS_EXTENT_Y_SHIFT);
	zls->control |= (format << ZLS_LOAD_FORMAT_SHIFT) | (format << ZLS_STORE_FORMAT_SHIFT);
	if (depth->twiddled)
		zls->control |= ZLS_LOAD_TWIDDLED | ZLS_STORE_TWIDDLED;
	if (load)
		zls->control |= ZLS_LOAD | ZLS_FORCE_LOAD;
	if (store)
		zls->control |= ZLS_STORE | ZLS_FORCE_STORE;
	if (load_stencil)
		zls->control |= ZLS_STENCIL_LOAD | ZLS_FORCE_LOAD;
	if (store_stencil)
		zls->control |= ZLS_STENCIL_STORE | ZLS_FORCE_STORE;

	/* The buffer, its layer size, and the empty tiles a load or store reaches. */
	zls->base = depth->address;
	if (depth->layer_bytes > 0xffffffffULL)
		return E2BIG;
	zls->stride = (uint32_t)depth->layer_bytes;
	zls->process_empty_tiles = load || store || load_stencil || store_stencil;

	/*
	 * The stencil: in the same buffer, packed with the depth (the base's
	 * separate-stencil enable off, as the depth is there too), the same
	 * layer size, its clear value the background's.
	 */
	if (stencil) {
		zls->has_stencil = true;
		zls->stencil_base = depth->address;
		zls->stencil_stride = (uint32_t)depth->layer_bytes;
		zls->background_stencil = depth->clear_stencil & 0xffU;
	}

	/* Succeeded: the depth words. */
	return 0;
}

/*
 * A float's bits as a 16-bit unorm: 0 below zero and for NaN, 65535 at
 * 1.0 or more, else floor(value * 65535 + 0.5) exactly.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/util/format/format_utils.h (_mesa_float_to_unorm: 0
 * for NaN and below zero, the maximum at 1.0 or more, else
 * _mesa_lroundevenf(x * max)).
 */
static uint32_t
unorm16_from_float(
	uint32_t bits)
{
	uint64_t scaled;
	uint64_t whole;
	uint64_t rest;
	uint32_t exponent;
	uint32_t shift;

	/* NaN, below zero, 1.0 or more. */
	exponent = (bits >> 23) & 0xffU;
	if (exponent == 0xffU && (bits & 0x7fffffU) != 0)
		return 0;
	if ((bits & 0x80000000U) != 0)
		return 0;
	if (exponent >= 127U)
		return 65535;

	/* Denormals and tiny values round to zero. */
	shift = 150U - exponent;
	if (exponent == 0 || shift > 60U)
		return 0;

	/* value * 65535 = mantissa * 65535 / 2^shift, rounded to nearest, ties to even. */
	scaled = (uint64_t)((bits & 0x7fffffU) | 0x800000U) * 65535U;
	whole = scaled >> shift;
	rest = scaled & ((1ULL << shift) - 1U);
	if (rest > (1ULL << (shift - 1U)) || (rest == (1ULL << (shift - 1U)) && (whole & 1U) != 0))
		whole++;

	/* The value. */
	return (uint32_t)whole;
}

/*
 * A float's bits as a 24-bit unorm: 0 below zero and for NaN, 0xffffff
 * at 1.0 or more, else floor(value * 0xffffff + 0.5) exactly.
 *
 * Procedure referenced: Mesa 26.2.6 src/util/format/format_utils.h
 * (_mesa_float_to_unorm of 24 bits: 0 for NaN and below zero, the
 * maximum at 1.0 or more, else the nearest step).
 */
static uint32_t
unorm24_from_float(
	uint32_t bits)
{
	uint64_t scaled;
	uint32_t exponent;
	uint32_t shift;

	/* NaN, below zero, 1.0 or more. */
	exponent = (bits >> 23) & 0xffU;
	if (exponent == 0xffU && (bits & 0x7fffffU) != 0)
		return 0;
	if ((bits & 0x80000000U) != 0)
		return 0;
	if (exponent >= 127U)
		return 0xffffffU;

	/* Denormals and tiny values round to zero. */
	shift = 150U - exponent;
	if (exponent == 0 || shift > 60U)
		return 0;

	/* floor(mantissa * 0xffffff / 2^shift + 0.5). */
	scaled = (uint64_t)((bits & 0x7fffffU) | 0x800000U) * 0xffffffU;
	return (uint32_t)((scaled + (1ULL << (shift - 1U))) >> shift);
}
