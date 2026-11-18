/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fixed 3D draws the hardware tests submit.
 *
 * Each fixture is a state page (surface heap, dynamic heap, pixel shader,
 * vertices and markers) and a batch that names it by GPU address only, so a
 * test can submit the same command list and state bytes on any context of the
 * GT's address space.  Nothing here touches the hardware.
 *
 * The single-colour draw is a RECTLIST over a 32x32 B8G8R8A8_UNORM render
 * target.  The textured draw samples an 8x8 R8G8B8A8_UNORM texture (nearest,
 * clamp, explicit LOD 0) at uv = (pixel + 0.5) / 32.  Origin is upper left, y
 * grows downwards in both the target and the texture, and row 0 is first in
 * memory.
 *
 * The single-colour draw is built for Gen12 or for Gen9 (the _gen
 * functions; graphics version 9 selects Gen9, any other Gen12).  The
 * textured draws carry pixel shaders and surface states made for Gen12
 * only, so a Gen9 GT cannot run them (drv_i915_tex_fixture_supported).
 */

#ifndef DRIVERS_GPU_I915_TESTS_FIXTURES_DRAW_FIXTURE_H
#define DRIVERS_GPU_I915_TESTS_FIXTURES_DRAW_FIXTURE_H

#include <stdint.h>

/*
 * Where the state page must sit: the pixel shader stores its entry marker
 * with an A64 write to the absolute address 0x100400c10.
 */
#define I915_DRAW_FIXTURE_STATE_VA		0x100400000ULL

/* The render target's size in pixels. */
#define I915_DRAW_FIXTURE_WIDTH			32U
#define I915_DRAW_FIXTURE_HEIGHT		32U

/* The markers: +0 before the draw, +4 after it, +8 mid-draw, +16 from the pixel shader. */
#define I915_DRAW_FIXTURE_MARKER_OFFSET		3072U
#define I915_DRAW_FIXTURE_MARKER_BEFORE		0xa5a50001U
#define I915_DRAW_FIXTURE_MARKER_AFTER		0xd7a3f00dU
#define I915_DRAW_FIXTURE_MARKER_MIDDRAW	0xc5c50003U
#define I915_DRAW_FIXTURE_PS_MARKER		0xc0ffee01U

/* The colour every pixel of the single-colour draw ends up with. */
#define I915_DRAW_FIXTURE_EXPECTED_PIXEL	0xffff0000U

/* Where the pixel shader sits in the state page and how long the Gen12 and the Gen9 ones are. */
#define I915_DRAW_FIXTURE_PS_OFFSET		1024U
#define I915_DRAW_FIXTURE_PS_BYTES		496U
#define I915_DRAW_FIXTURE_PS_GEN9_BYTES		192U

/* Where the texture sits: the address its generated surface state names. */
#define I915_TEX_FIXTURE_TEX_VA			0x100404000ULL

/* The texture's size in texels and in bytes (linear, row pitch 32). */
#define I915_TEX_FIXTURE_TEX_W			8U
#define I915_TEX_FIXTURE_TEX_H			8U
#define I915_TEX_FIXTURE_TEX_BYTES		256U

/* Binding table entry 1 names the surface state here; the sampler is in the dynamic heap. */
#define I915_TEX_FIXTURE_TEX_RSS_OFFSET		128U
#define I915_TEX_FIXTURE_SAMPLER_OFFSET		896U

/*
 * The test images.
 *
 * RGBA bytes in memory order, texel (u,v) at rgba[(v * 8 + u) * 4]:
 *   variant 0: R=16+32u G=16+32v B=16+32((u+3v)&7) A=255 (position-identifying, asymmetric)
 *   variant 1: R=239-32v G=16+32u B=16+32((3u+v)&7) A=255 (for update and binding switches)
 *   variant 2: R=240-32u G=240-32v B=16+32((u^v)&7) A=255 (a third, again distinct, image)
 *   variant 3: R=64(u&3) G=64(v&3) B=64((u>>2)+2(v>>2)) A=255 (every channel a multiple of 64)
 */
#define I915_TEX_FIXTURE_VARIANTS		4U

/*
 * The second texture of the binding switch.
 *
 * Texture A keeps its surface state at +128, texture B gets one at +192;
 * binding table entry 1 names the one that is bound.  Nothing else in the
 * state page or in the batch changes between the two bindings.
 */
#define I915_TEX_FIXTURE_TEX_B_VA		0x100405000ULL
#define I915_TEX_FIXTURE_TEX_B_RSS_OFFSET	192U

/*
 * The full-HD textured draw into a buffer the caller owns.
 *
 * The pixel shader computes uv = (pixel + 0.5) / (1920, 1080); the render
 * target is B8G8R8A8_UNORM, linear, pitch 7680.  The texture, its surface
 * state and the sampler are the 32x32 draw's, so nearest sampling gives
 * pixel (x, y) = texel (x / 240, y / 135), which the 32x32 expectation gives
 * at (4 * (x / 240), 4 * (y / 135)).  The state page, batch and texture keep
 * their addresses; the target sits at I915_TEX_FHD_RT_VA (or at
 * I915_TEX_FHD_RT_B_VA for a second buffer, which starts after the first ends
 * at 0x100fe9000).
 */
#define I915_TEX_FHD_RT_VA			0x100800000ULL
#define I915_TEX_FHD_RT_B_VA			0x101000000ULL
#define I915_TEX_FHD_WIDTH			1920U
#define I915_TEX_FHD_HEIGHT			1080U
#define I915_TEX_FHD_PITCH			7680U
#define I915_TEX_FHD_RT_BYTES			8294400U

void drv_i915_draw_fixture_write_state(void *state_page, uint64_t rt_va, uint32_t mocs);
unsigned drv_i915_draw_fixture_build_batch(uint32_t *cmds, unsigned capacity, uint64_t state_va, uint32_t mocs);
uint32_t drv_i915_draw_fixture_mocs(void);
void drv_i915_draw_fixture_write_state_gen(void *state_page, uint64_t rt_va, uint32_t mocs, unsigned graphics_ver);
unsigned drv_i915_draw_fixture_build_batch_gen(uint32_t *cmds, unsigned capacity, uint64_t state_va, uint32_t mocs, unsigned graphics_ver);
uint32_t drv_i915_draw_fixture_mocs_gen(unsigned graphics_ver);
int drv_i915_tex_fixture_supported(unsigned graphics_ver);

void drv_i915_tex_fixture_write_state(void *state_page, uint64_t rt_va, uint64_t tex_va, uint32_t mocs);
unsigned drv_i915_tex_fixture_build_batch(uint32_t *cmds, unsigned capacity, uint64_t state_va, uint32_t mocs);
void drv_i915_tex_fixture_pattern(uint8_t *rgba, unsigned variant);
uint32_t drv_i915_tex_fixture_expected_pixel(const uint8_t *rgba, unsigned x, unsigned y);
void drv_i915_tex_fixture_write_state_ab(void *state_page, uint64_t rt_va, uint64_t tex_a_va, uint64_t tex_b_va, unsigned bind_b, uint32_t mocs);
void drv_i915_tex_fixture_write_state_ab_filter(void *state_page, uint64_t rt_va, uint64_t tex_a_va, uint64_t tex_b_va, unsigned bind_b, unsigned linear, uint32_t mocs);
uint32_t drv_i915_tex_fixture_expected_pixel_linear(const uint8_t *rgba, unsigned x, unsigned y, int *inexact);

void drv_i915_tex_fixture_fhd_write_state(void *state_page, uint64_t rt_va, uint64_t tex_va, uint32_t mocs);
unsigned drv_i915_tex_fixture_fhd_build_batch(uint32_t *cmds, unsigned capacity, uint64_t state_va, uint32_t mocs);
const uint32_t *drv_i915_tex_fixture_fhd_rt_rss(void);
unsigned drv_i915_tex_fixture_fhd_ps_bytes(void);
int drv_i915_tex_fixture_fhd_same_texture_state(void);

#endif
