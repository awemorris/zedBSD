/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Gen12 backend: common scalar shader IR to Intel EU code.
 *
 * The compiler touches no device.  It returns the encoded instruction words
 * together with what a draw has to program around them; placing the words in
 * GPU memory, and checking the vertex and fragment interfaces against each
 * other, stays with the caller.
 */

#ifndef DRIVERS_GPU_I915_COMPILER_COMPILER_H
#define DRIVERS_GPU_I915_COMPILER_COMPILER_H

#include "../../compiler/spirv.h"

#include <stddef.h>
#include <stdint.h>

/* The most sampled images a kernel reads: binding table entries 1 .. 16. */
#define I915_SHADER_MAX_SAMPLERS	16U

/* The most uniform blocks a kernel reads. */
#define I915_SHADER_MAX_BLOCKS		8U

/* The most vertex attributes, and varyings, a kernel reads or writes. */
#define I915_SHADER_MAX_INPUTS		16U

/*
 * The colour locations a fragment shader may write, one render target each,
 * and the binding table entry of render target n: entry 0 for the first
 * (the textures follow it at 1 + m), 16 + n past the sixteen textures for
 * the others.
 */
#define I915_SHADER_MAX_COLOR_OUTPUTS		4U
#define I915_SHADER_RT_BTI(n)			((uint32_t)(n) + 16U * (uint32_t)((n) != 0U))

/*
 * Compute: the registers of per-thread push data after the cross-thread
 * data, in this order -- LocalInvocationID x, y, z and LocalInvocationIndex
 * of the thread's eight channels, a dword each (ws101-p002).
 */
#define I915_SHADER_PER_THREAD_REGS	4U

/*
 * One uniform block a kernel reads, delivered with its push constants.
 *
 * The draw copies bytes [offset, offset + bytes) of the buffer bound at
 * (set, binding) to byte `push_offset` of the stage's push data.
 */
struct i915_shader_block {
	uint32_t set;
	uint32_t binding;
	uint32_t offset;
	uint32_t bytes;
	uint32_t push_offset;

	/*
	 * Nonzero for a storage buffer: the draw puts the 64-bit GPU address of
	 * the buffer's descriptor range at push_offset (the low word first)
	 * instead of a copy of its words.
	 */
	uint32_t address;
};

/*
 * A compiled shader: the EU instruction words and the layout a draw needs.
 *
 * The compiler allocates it and drv_i915_shader_binary_free() releases it.
 * The caller copies `code` into a GPU object before the shader runs.
 */
struct i915_shader_binary {
	uint32_t *code;
	uint32_t code_bytes;
	uint32_t entry_offset;
	enum drv_gpu_shader_stage stage;
	uint32_t grf_used;
	uint32_t simd;
	uint32_t thread_count;

	/* The sampled images: the n-th is binding table entry 1 + n and sampler n. */
	uint32_t sampler_count;
	uint32_t sampler_set[I915_SHADER_MAX_SAMPLERS];
	uint32_t sampler_binding[I915_SHADER_MAX_SAMPLERS];

	/*
	 * What a draw has to program around the kernel (see the register
	 * conventions in compile.c).
	 */

	/* The first payload register after the fixed ones. */
	uint32_t dispatch_grf_start;

	/*
	 * Registers of push data, 32 bytes each: the push constants first
	 * (push_constant_bytes of them, a whole number of registers), then the
	 * uniform blocks.
	 */
	uint32_t push_regs;
	uint32_t push_constant_bytes;

	/* The uniform blocks delivered after the push constants. */
	uint32_t block_count;
	struct i915_shader_block blocks[I915_SHADER_MAX_BLOCKS];

	/* Vertex: attributes; fragment: interpolated inputs; geometry: the located per-vertex inputs it reads. */
	uint32_t input_count;

	/* The input locations in ascending order: the payload order. */
	uint32_t input_locations[I915_SHADER_MAX_INPUTS];

	/* Fragment: bit n is set when input n (in payload order) is Flat, set up as the provoking vertex's value. */
	uint32_t input_flat_mask;

	/* Vertex and geometry: VUE slots after the position; fragment: equal to input_count. */
	uint32_t varying_count;

	/* Vertex and geometry: the location each VUE slot after the position holds, ascending. */
	uint32_t varying_locations[I915_SHADER_MAX_INPUTS];

	/*
	 * Fragment: nonzero when the kernel discards pixels, which the draw
	 * declares in 3DSTATE_PS_EXTRA (Pixel Shader Kills Pixel).
	 */
	uint32_t uses_kill;

	/*
	 * Fragment: nonzero when the kernel writes a second colour (Location 0
	 * Index 1) with one dual-source render-target write (ws031-p032).
	 */
	uint32_t dual_source;

	/*
	 * Vertex: nonzero when the kernel writes the point size into its VUE
	 * header, which the draw has the setup read (3DSTATE_SF Point Width
	 * Source).
	 */
	uint32_t writes_point_size;

	/*
	 * Fragment: nonzero when the kernel reads an input without perspective,
	 * gl_FragCoord.z or gl_FragCoord.w, which the draw has the payload carry:
	 * the linear barycentrics (3DSTATE_WM Barycentric Interpolation Mode),
	 * the source depth and the source w (3DSTATE_PS_EXTRA).
	 */
	uint32_t uses_linear_barycentrics;
	uint32_t uses_source_depth;
	uint32_t uses_source_w;

	/*
	 * The scratch memory each thread of the kernel needs for the values it
	 * spills: a power of two from 1 KiB to 2 MiB, or 0 for a kernel that
	 * spills nothing.  The draw programs it, and a buffer of it for every
	 * thread the stage may run at once, in 3DSTATE_VS / PS.
	 */
	uint32_t scratch_bytes;

	/*
	 * Compute (ws101-p002): the workgroup's size along x, y and z, and the
	 * payload the dispatch delivers after r0 -- `cross_thread_regs`
	 * registers of push data every thread of a group reads (the push
	 * constants and the blocks above, from r1), then `per_thread_regs`
	 * registers of its own: the x, y and z of each channel's invocation in
	 * the group and its linear index, one register each
	 * (I915_SHADER_PER_THREAD_REGS).  A block whose set is
	 * DRV_GPU_IR_SYSTEM_SET takes the address of the three group counts.
	 * Zero for another stage.
	 */
	uint32_t local_size[3];
	uint32_t cross_thread_regs;
	uint32_t per_thread_regs;

	/*
	 * Compute (ws101-p006): the bytes of shared memory the workgroup needs
	 * (0 for none), and nonzero when the kernel waits at a workgroup
	 * barrier; the dispatch programs both in the interface descriptor.
	 */
	uint32_t shared_bytes;
	uint32_t uses_barrier;

	/*
	 * Geometry (ws075-p007a): what the draw programs in 3DSTATE_GS and the
	 * URB -- the vertices of one input primitive, the primitive emitted
	 * (3D_Prim_Topo_Type, DRV_GPU_IR_OUTPUT_*), one output vertex and the
	 * control data header in 32-byte units, the header's format (0 cut
	 * bits, 1 stream IDs), and the output URB entry in 64-byte units; and
	 * nonzero when the kernel reads the input primitive's number (Include
	 * Primitive ID) or writes the layer (the clipper then takes the render
	 * target array index from the VUE).  Zero for another stage.
	 */
	uint32_t vertices_in;
	uint32_t output_topology;
	uint32_t output_vertex_hwords;
	uint32_t control_data_hwords;
	uint32_t control_data_format;
	uint32_t urb_entry_size;
	uint32_t uses_primitive_id;
	uint32_t writes_layer;
};

int drv_i915_shader_compile(const struct drv_gpu_shader_ir *ir, struct i915_shader_binary **out);
int drv_i915_shader_compile_stage(const struct drv_gpu_shader_ir *ir, const struct i915_shader_binary *producer, struct i915_shader_binary **out);
void drv_i915_shader_binary_free(struct i915_shader_binary *binary);

#endif /* DRIVERS_GPU_I915_COMPILER_COMPILER_H */
