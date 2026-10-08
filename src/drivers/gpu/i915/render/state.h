/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Gen12 3D state: the heaps a draw points at and the packets that program
 * the pipeline around them.
 *
 * The draw path and the rectangle path share these writers.  The state is
 * written into the session's state object at the offsets of heap.h and the
 * packets into a batch of batch.h.  Bit positions are those of Mesa's genxml
 * (gen120); the packets follow what anv programs for an ordinary Vulkan
 * pipeline.
 */

#ifndef DRIVERS_GPU_I915_RENDER_STATE_H
#define DRIVERS_GPU_I915_RENDER_STATE_H

#include <stdint.h>

#include "gfx.h"

#include "../compiler/compiler.h"

struct i915_gfx_batch;

/*
 * What one stage's push data carries: the push constants from the start of
 * the command buffer's block, then the ranges of the uniform blocks the
 * kernel reads.
 *
 * It is part of struct i915_gfx_kernels; `blocks` borrows the binary's
 * list.
 */
struct i915_gfx_push_layout {
	/* The registers of push data the kernel reads, 32 bytes each. */
	uint32_t regs;

	/* The bytes of push constants at the start. */
	uint32_t constant_bytes;

	/* The uniform blocks after them, as the compiler laid them out. */
	uint32_t block_count;
	const struct i915_shader_block *blocks;
};

/*
 * The kernels of a draw (vertex, optional geometry, pixel) and what has to
 * be programmed around them.
 *
 * It is filled from a pipeline's compiled binaries, or for a rectangle from
 * the transfer kernel, just before the state is written, and lives on the
 * caller's stack.  The code pointers borrow the binaries' code.
 */
struct i915_gfx_kernels {
	/* The vertex kernel's code and size. */
	const uint32_t *vs_code;
	uint32_t vs_bytes;

	/* The pixel kernel's code and size. */
	const uint32_t *ps_code;
	uint32_t ps_bytes;

	/* The vertex kernel's first payload register and push data. */
	uint32_t vs_grf_start;
	uint32_t vs_push_regs;
	struct i915_gfx_push_layout vs_push;

	/* The attribute locations the vertex kernel reads, in payload order. */
	uint32_t vs_input_count;
	uint32_t vs_inputs[I915_GFX_MAX_VERTEX_ATTRIBUTES];

	/*
	 * The VUE slots after the position the vertex kernel itself writes,
	 * which size its URB entries when a geometry kernel follows it (then
	 * `varyings` below is the geometry kernel's).
	 */
	uint32_t vs_varyings;

	/*
	 * The geometry kernel (ws075-p007b); NULL and zeros for a draw without
	 * one.  Its code and size, first payload register and push data, then
	 * what 3DSTATE_GS and the URB take from it: the vertices of an input
	 * primitive, the 3D_Prim_Topo_Type it emits, one output vertex and the
	 * control data header in 32-byte units, the control data format, the
	 * output URB entry in 64-byte units, and nonzero when it reads the
	 * input primitive's number or writes the layer.
	 */
	const uint32_t *gs_code;
	uint32_t gs_bytes;
	uint32_t gs_grf_start;
	uint32_t gs_push_regs;
	struct i915_gfx_push_layout gs_push;
	uint32_t gs_vertices_in;
	uint32_t gs_output_topology;
	uint32_t gs_output_vertex_hwords;
	uint32_t gs_control_hwords;
	uint32_t gs_control_format;
	uint32_t gs_urb_entry_size;
	uint32_t gs_primitive_id;
	uint32_t gs_writes_layer;

	/* The VUE slots after the position the last stage before the pixel stage writes (the geometry kernel when there is one). */
	uint32_t varyings;

	/* Nonzero when that last stage writes the point size into its VUE header (0 for a rectangle). */
	uint32_t vs_point_size;

	/*
	 * The fragment inputs: when ps_inputs_mapped is nonzero, the pixel
	 * kernel reads ps_input_count inputs and input n (in its payload order)
	 * comes from VUE slot ps_input_slots[n] after the position of the last
	 * stage's VUE; otherwise (a rectangle kernel) input n is slot n of the
	 * `varyings` slots.
	 */
	uint32_t ps_inputs_mapped;
	uint32_t ps_input_count;
	uint32_t ps_input_slots[I915_GFX_MAX_VARYINGS];

	/* Bit n: fragment input n is Flat, set up as the provoking vertex's value (0 for a rectangle). */
	uint32_t ps_flat_mask;

	/* Bit n: fragment input n is gl_PointCoord, the point sprite's texture coordinate (0 for a rectangle). */
	uint32_t ps_point_sprite_mask;

	/*
	 * Bit n: fragment input n is gl_PrimitiveID, which no stage before
	 * writes, so the setup gives it (ws075-p007b b4; 0 for a rectangle).
	 */
	uint32_t ps_primitive_id_mask;

	/*
	 * Nonzero when the pixel kernel's payload carries the linear
	 * barycentrics, the source depth and the source w (0 for a rectangle).
	 */
	uint32_t ps_linear_barycentrics;
	uint32_t ps_source_depth;
	uint32_t ps_source_w;

	/* The pixel kernel's first payload register and sampled images. */
	uint32_t ps_grf_start;
	uint32_t ps_samplers;

	/* The (set, binding) of each sampled image, in the kernel's order; NULL for a rectangle. */
	const uint32_t *ps_sampler_sets;
	const uint32_t *ps_sampler_bindings;

	/* The pixel kernel's push data, which comes in front of its setup data. */
	uint32_t ps_push_regs;
	struct i915_gfx_push_layout ps_push;

	/* Nonzero when the pixel kernel discards pixels (3DSTATE_PS_EXTRA Pixel Shader Kills Pixel). */
	uint32_t ps_kills;

	/*
	 * The scratch memory a thread of each kernel spills to (a power of two
	 * from 1 KiB, 0 for a kernel that spills nothing); the session's scratch
	 * buffer the draw makes the general state base (0 when no kernel
	 * spills), and where each stage's part starts in it.
	 */
	uint32_t vs_scratch_bytes;
	uint32_t gs_scratch_bytes;
	uint32_t ps_scratch_bytes;
	uint64_t scratch_base;
	uint64_t vs_scratch_offset;
	uint64_t gs_scratch_offset;
	uint64_t ps_scratch_offset;

	/* A compute kernel's scratch space and where its part starts (ws101-p004); zero for a draw. */
	uint32_t cs_scratch_bytes;
	uint64_t cs_scratch_offset;
};

/*
 * The 3DPRIMITIVE of one draw.
 *
 * It lives on the caller's stack while the batch is written.
 */
struct i915_gfx_primitive {
	/* The 3D_Prim_Topo_Type. */
	uint32_t topology;

	/* Nonzero when the vertices are fetched through the index buffer. */
	int random_access;

	/* The vertices, or the indices, of each instance, and the first of them. */
	uint32_t vertex_count;
	uint32_t start_vertex;

	/* The instances and the first of them. */
	uint32_t instance_count;
	uint32_t start_instance;

	/* What is added to every index of a random-access draw. */
	int32_t base_vertex;
};

int drv_i915_gfx_vertex_format_supported(uint32_t format);
int drv_i915_gfx_texel_buffer_format(uint32_t format, uint32_t *surface_format, uint32_t *bytes);

void drv_i915_gfx_pipeline_kernels(const struct i915_gfx_pipeline *pipeline, struct i915_gfx_kernels *kernels);

int drv_i915_gfx_write_state(uint8_t *page, const struct i915_gfx_draw_state *state, const struct i915_gfx_kernels *kernels, const struct i915_gfx_image *target, uint32_t mocs);
int drv_i915_gfx_write_push(uint8_t *data, const struct i915_gfx_draw_state *state, const struct i915_gfx_push_layout *layout);
int drv_i915_gfx_surface_write(uint32_t *rss, const struct i915_gfx_surface *surface, uint32_t mocs);
void drv_i915_gfx_sampler_write(uint32_t *state, const struct i915_gfx_sampler *sampler);
void drv_i915_gfx_sampler_border_write(uint32_t *state, uint32_t *border, uint32_t offset, const struct i915_gfx_sampler *sampler);
void drv_i915_gfx_instruction_heap_clear(uint8_t *window);

void drv_i915_gfx_emit_context_setup(struct i915_gfx_batch *batch, uint64_t state_va, uint64_t instruction_va, uint64_t general_va, uint32_t mocs);
uint32_t drv_i915_gfx_topology(const struct i915_gfx_pipeline *pipeline);
uint32_t drv_i915_gfx_topology_vertices(uint32_t topology);
int drv_i915_gfx_emit_vertex_input(struct i915_gfx_batch *batch, const struct i915_gfx_draw_state *state, const struct i915_gfx_kernels *kernels, uint32_t mocs);
int drv_i915_gfx_emit_index_buffer(struct i915_gfx_batch *batch, const struct i915_gfx_draw_state *state, uint32_t mocs);
int drv_i915_gfx_emit_urb(struct i915_gfx_batch *batch, uint32_t vs_entry_size, uint32_t gs_entry_size);
void drv_i915_gfx_emit_constants(struct i915_gfx_batch *batch, uint64_t vs_push_va, uint32_t vs_push_regs, uint64_t gs_push_va, uint32_t gs_push_regs, uint64_t ps_push_va, uint32_t ps_push_regs, uint32_t mocs);
uint32_t drv_i915_gfx_samples_log2(uint32_t samples);
void drv_i915_gfx_emit_raster(struct i915_gfx_batch *batch, const struct i915_gfx_pipeline *pipeline, const struct i915_gfx_kernels *kernels);
int drv_i915_gfx_emit_depth(struct i915_gfx_batch *batch, const struct i915_gfx_draw_state *state, const struct i915_gfx_image *depth, uint64_t scratch_va, uint32_t mocs);
void drv_i915_gfx_emit_vertex_shader(struct i915_gfx_batch *batch, const struct i915_gfx_kernels *kernels);
void drv_i915_gfx_emit_geometry_shader(struct i915_gfx_batch *batch, const struct i915_gfx_kernels *kernels);
void drv_i915_gfx_emit_pixel_shader(struct i915_gfx_batch *batch, const struct i915_gfx_kernels *kernels);
void drv_i915_gfx_emit_ps_blend(struct i915_gfx_batch *batch, const struct i915_gfx_pipeline *pipeline);
void drv_i915_gfx_emit_primitive(struct i915_gfx_batch *batch, uint32_t width, uint32_t height, const struct i915_gfx_primitive *primitive);
void drv_i915_gfx_emit_draw(struct i915_gfx_batch *batch, uint32_t width, uint32_t height, const struct i915_gfx_primitive *primitive);
void drv_i915_gfx_emit_batch_end(struct i915_gfx_batch *batch);

#endif
