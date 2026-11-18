/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The depth load and store (ZLS) words of a PowerVR render's fragment
 * command: which tiles the ISP loads and stores, their format and layout,
 * the depth buffer's address and layer size, and the background depth
 * (the clear value).  D32_SFLOAT, D16_UNORM and D24_UNORM_S8_UINT (with
 * its stencil's load, store and background).  Nothing here holds a
 * lock or memory; the host check builds this file unchanged.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_RENDER_DEPTH_H
#define KERN_DRIVERS_GPU_POWERVR_RENDER_DEPTH_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/gpu/powervr/render-pass.h"

/*
 * The depth words of a fragment command: CR_ISP_ZLSCTL, CR_ISP_ZLOAD_BASE
 * (the load and the store address), CR_ISP_BGOBJDEPTH, the zls_stride
 * (the layer size), and whether the render must process empty tiles (a
 * load or store reaches every tile).  A target with a stencil adds
 * CR_ISP_STENCIL_LOAD_BASE (the stencil's address, in the depth's
 * buffer), the sls_stride, the stencil's background value (the clear
 * value, CR_ISP_BGOBJVALS) and has_stencil.
 */
struct powervr_render_zls {
	uint64_t control;
	uint64_t base;
	uint32_t background_depth;
	uint32_t stride;
	bool process_empty_tiles;
	bool has_stencil;
	uint64_t stencil_base;
	uint32_t stencil_stride;
	uint32_t background_stencil;
};

int powervr_render_zls_words(const struct powervr_render_depth *depth, struct powervr_render_zls *zls);

#endif /* KERN_DRIVERS_GPU_POWERVR_RENDER_DEPTH_H */
