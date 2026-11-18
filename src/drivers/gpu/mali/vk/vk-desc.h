/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Valhall descriptors the executor writes: shader programs, thread
 * storage, resource tables, buffers, textures and their planes, vertex
 * attributes, blends, render targets, framebuffers, tiler contexts and
 * draws.  Each packer fills an array of 32-bit words, low word first, as
 * the GPU reads them from memory; nothing here touches the GPU, so the
 * packers run unchanged in a host test.
 */

#ifndef KERN_DRIVERS_GPU_MALI_VK_DESC_H
#define KERN_DRIVERS_GPU_MALI_VK_DESC_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The bytes of a descriptor of each kind.  A framebuffer descriptor is
 * its parameters and their padding; the depth and stencil extension and
 * the render targets follow it.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (aggregate "Framebuffer": the
 * parameters at 0 and the 16-word padding at 64);
 * src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c (the extension and the
 * render targets at pan_size(FRAMEBUFFER) past the descriptor).
 */
#define MALI_DESC_BYTES			32U
#define MALI_DESC_RESOURCE_BYTES	16U
#define MALI_DESC_BLEND_BYTES		16U
#define MALI_DESC_RT_BYTES		64U
#define MALI_DESC_FBD_BYTES		128U
#define MALI_DESC_TILER_BYTES		128U
#define MALI_DESC_DRAW_BYTES		128U
#define MALI_DESC_HEAP_BYTES		32U

/* A shader's stage in its program descriptor. */
#define MALI_DESC_STAGE_COMPUTE		1U
#define MALI_DESC_STAGE_FRAGMENT	2U
#define MALI_DESC_STAGE_VERTEX		3U

/*
 * Marks a vertex stage's descriptor as the varying (secondary) program's,
 * which runs full warps; the position program runs half warps.
 */
#define MALI_DESC_STAGE_VARYING		0x100U

/* The resource table entries a table pointer must be a multiple of. */
#define MALI_DESC_TABLE_ALIGN		4U

/* The draw modes of a primitive. */
#define MALI_DESC_DRAW_POINTS		1U
#define MALI_DESC_DRAW_LINES		2U
#define MALI_DESC_DRAW_LINE_STRIP	4U
#define MALI_DESC_DRAW_TRIANGLES	8U
#define MALI_DESC_DRAW_TRIANGLE_STRIP	10U
#define MALI_DESC_DRAW_TRIANGLE_FAN	12U

/* The dimensions of a texture descriptor. */
#define MALI_DESC_DIMENSION_CUBE	0U
#define MALI_DESC_DIMENSION_2D		2U
#define MALI_DESC_DIMENSION_3D		3U

/* The least bytes of a thread's local storage, and the most. */
#define MALI_DESC_TLS_MIN_BYTES		16U
#define MALI_DESC_TLS_MAX_BYTES		(16U << 20)

/* The least bytes of a workgroup's instance of workgroup memory, and the most. */
#define MALI_DESC_WLS_MIN_BYTES		128U
#define MALI_DESC_WLS_MAX_BYTES		(1U << 20)

/*
 * What a texture descriptor describes: its dimension, its pixel format,
 * its extent (a 3D image's depth, 1 otherwise), its levels, its array
 * size (the layers, a cube's count of cubes), its swizzle, whether it is
 * written for storage (one level, no level of detail range) and the
 * address of its plane descriptors (one for each layer and level, layer
 * by layer).
 */
struct mali_desc_view {
	uint32_t dimension;
	uint32_t pixel;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t levels;
	uint32_t array_size;
	uint32_t swizzle;
	bool storage;
	uint64_t planes;
};

/* What a render target of a framebuffer is: its format, its memory and its clear. */
struct mali_desc_target {
	uint32_t internal;
	uint32_t writeback;
	uint32_t swizzle;
	bool srgb;
	bool write;
	uint64_t base;
	uint32_t row_stride;
	uint32_t internal_offset;
	uint32_t clear[4];
};

void mali_desc_shader(uint32_t *words, unsigned stage, uint32_t work_registers, uint32_t preload, uint64_t code);
void mali_desc_local_storage(uint32_t *words);
void mali_desc_resource(uint32_t *words, uint64_t address, uint64_t bytes);
void mali_desc_buffer(uint32_t *words, uint64_t address, uint32_t bytes);
void mali_desc_texture(uint32_t *words, uint32_t pixel, uint32_t width, uint32_t height, uint32_t swizzle, uint64_t planes);
void mali_desc_plane(uint32_t *words, uint32_t texel_bytes, uint64_t address, uint32_t row_stride, uint64_t bytes, uint32_t width, uint32_t height);
void mali_desc_attribute(uint32_t *words, uint32_t pixel, uint32_t table, bool instance, int32_t offset, uint32_t buffer, uint32_t stride);
uint32_t mali_desc_workgroup(uint32_t x, uint32_t y, uint32_t z);
void mali_desc_blend(uint32_t *words, bool enable, uint32_t equation, uint32_t color_mask, unsigned target, uint32_t memory_format);
uint32_t mali_desc_blend_equation(uint32_t vk_equation, uint32_t color_mask, bool *supported);
void mali_desc_target(uint32_t *words, const struct mali_desc_target *target);
void mali_desc_framebuffer(uint32_t *words, uint32_t width, uint32_t height, uint32_t targets, uint32_t tile_size, uint32_t color_buffer_bytes, uint64_t tiler);
void mali_desc_tiler_heap(uint32_t *words, uint64_t base, uint32_t bytes);
void mali_desc_tiler(uint32_t *words, uint32_t width, uint32_t height, uint64_t heap, uint64_t geometry, uint32_t geometry_bytes, uint32_t hierarchy_mask);
uint32_t mali_desc_framebuffer_pointer(uint64_t fbd, uint32_t targets);
void mali_desc_draw(uint32_t *words, uint32_t flags0, uint32_t render_target_mask, uint64_t blend, uint32_t blend_count, uint64_t resources, uint64_t shader, uint64_t storage, uint64_t fau, uint32_t fau_count);
uint32_t mali_desc_primitive(unsigned draw_mode, unsigned index_type);
uint32_t mali_desc_scissor_low(uint32_t x, uint32_t y);
uint32_t mali_desc_scissor_high(uint32_t x, uint32_t y);
uint32_t mali_desc_clump_format(uint32_t texel_bytes);
void mali_desc_texture_view(uint32_t *words, const struct mali_desc_view *view);
void mali_desc_thread_storage(uint32_t *words, uint32_t tls_bytes, uint64_t tls_base);
uint32_t mali_desc_tls_size(uint32_t tls_bytes);
uint32_t mali_desc_wls_bytes(uint32_t shared_bytes);
void mali_desc_workgroup_storage(uint32_t *words, uint32_t instances, uint32_t instance_bytes, uint64_t wls_base);
uint32_t mali_desc_sampler_compare(uint32_t compare_op);

#endif
