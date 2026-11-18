/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The depth and stencil surfaces and the mip levels of the executor's
 * linear images: which depth and stencil formats the tile buffer takes
 * and how, the levels' places in an image, the framebuffer's depth and
 * stencil words, the depth and stencil target extension, the depth and
 * stencil descriptor of a draw, the draw's kill and update operations,
 * and the sampler's level of detail words.  Each packer fills 32-bit
 * words, low word first, as the GPU reads them; nothing here touches the
 * GPU, so the packers run unchanged in a host test.
 */

#ifndef KERN_DRIVERS_GPU_MALI_VK_SURFACE_H
#define KERN_DRIVERS_GPU_MALI_VK_SURFACE_H

#include <stdbool.h>
#include <stdint.h>

/* The most levels of an image: an edge of 8192 texels has fourteen. */
#define MALI_SURFACE_LEVELS		14U

/* The bytes of the depth and stencil target extension. */
#define MALI_SURFACE_ZS_BYTES		64U

/* The aspects a depth and stencil format has (Vulkan's aspect bits). */
#define MALI_SURFACE_ASPECT_DEPTH	0x2U
#define MALI_SURFACE_ASPECT_STENCIL	0x4U

/*
 * A draw's pixel kill and depth and stencil update operations.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (enum "Pixel Kill": Force
 * Early 0, Weak Early 2, Force Late 3).
 */
#define MALI_SURFACE_KILL_FORCE_EARLY	0U
#define MALI_SURFACE_KILL_WEAK_EARLY	2U
#define MALI_SURFACE_KILL_FORCE_LATE	3U

/*
 * A depth and stencil format the tile buffer takes: its Vulkan number,
 * its aspects, the depth target's write format (zero without depth),
 * the stencil target's write format (zero without stencil), and the
 * tile buffer's depth format.
 */
struct mali_surface_zs_format {
	uint32_t vk;
	uint32_t aspects;
	uint32_t zs_write;
	uint32_t s_write;
	uint32_t z_internal;
};

/* One level of an image: its offset in a layer, its row pitch, its extent and its bytes. */
struct mali_surface_level {
	uint64_t offset;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
	uint64_t bytes;
};

/*
 * A pass's depth and stencil attachment as the framebuffer takes it: the
 * format, the memory of the depth and of the stencil (the same for a
 * format holding both), their row pitches and surface bytes, whether
 * each is loaded, stored and cleared, and the clear values (the depth's
 * float bits, the stencil's byte).
 */
struct mali_surface_zs_target {
	const struct mali_surface_zs_format *format;
	uint64_t base;
	uint32_t pitch;
	uint64_t surface_bytes;
	bool depth_load;
	bool depth_store;
	bool depth_clear;
	bool stencil_load;
	bool stencil_store;
	bool stencil_clear;
	uint32_t depth_clear_bits;
	uint32_t stencil_clear_value;
};

/* One face's stencil state: Vulkan's ops and compare, its masks and its reference. */
struct mali_surface_stencil_face {
	uint32_t fail_op;
	uint32_t pass_op;
	uint32_t depth_fail_op;
	uint32_t compare_op;
	uint32_t compare_mask;
	uint32_t write_mask;
	uint32_t reference;
};

/*
 * A draw's depth and stencil state: whether the pass has a depth and a
 * stencil aspect to test against; the pipeline's depth test, write and
 * compare, its stencil test and faces, its depth bias (enabled, and the
 * constant, clamp and slope factors' float bits); and whether the
 * fragment program writes the depth or the stencil reference.
 */
struct mali_surface_zs_state {
	bool has_depth;
	bool has_stencil;
	bool depth_test;
	bool depth_write;
	uint32_t depth_compare;
	bool stencil_test;
	struct mali_surface_stencil_face front;
	struct mali_surface_stencil_face back;
	bool bias;
	uint32_t bias_constant;
	uint32_t bias_clamp;
	uint32_t bias_slope;
	bool shader_depth;
	bool shader_stencil;
};

/*
 * What decides a draw's kill and update operations: the depth and
 * stencil state, whether the fragment program changes its coverage
 * (writes the sample mask or discards), and whether it writes memory
 * (storage buffers or images).
 */
struct mali_surface_kill {
	const struct mali_surface_zs_state *state;
	bool coverage;
	bool writes_memory;
};

const struct mali_surface_zs_format *mali_surface_zs_find(uint32_t vk_format);
uint32_t mali_surface_extent(uint32_t extent, uint32_t level);
uint32_t mali_surface_level_count(uint32_t width, uint32_t height);
uint64_t mali_surface_levels(uint32_t width, uint32_t height, uint32_t texel_bytes, uint32_t row_align, uint32_t levels, struct mali_surface_level *out);
void mali_surface_zs_extension(uint32_t *words, const struct mali_surface_zs_target *target);
void mali_surface_zs_framebuffer(uint32_t *framebuffer_words, const struct mali_surface_zs_target *target);
uint32_t mali_surface_zs_pointer(void);
void mali_surface_depth_stencil(uint32_t *words, const struct mali_surface_zs_state *state);
bool mali_surface_zs_writes(const struct mali_surface_zs_state *state);
uint32_t mali_surface_kill_flags(const struct mali_surface_kill *kill);
bool mali_surface_sampler_lod(uint32_t *words, uint32_t mipmap_mode, uint32_t min_lod_bits, uint32_t max_lod_bits, uint32_t bias_bits);

#endif
