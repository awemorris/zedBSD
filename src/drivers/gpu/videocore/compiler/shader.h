/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Private scalar SPIR-V lowering returns native code and its immutable draw-time interface. */
#ifndef KERN_DRIVERS_GPU_BCM2711_SHADER_H
#define KERN_DRIVERS_GPU_BCM2711_SHADER_H

#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/compiler/spirv.h"

/*
 * What the shared frontend lowers for this backend beyond its common
 * profile: arrays of descriptors (of storage images too), a compute
 * program's storage images (images in a compute shader, of which the
 * analysis takes the storage ones' accesses and size queries; a graphics
 * program's are refused), a fragment program's gl_FragDepth, and
 * gl_ViewIndex, which a graphics program reads as a uniform
 * (BCM2711_SHADER_VIEW_INDEX) instead of the frontend's bound constant, so
 * one program serves every view.  Every parse of a program for this backend
 * asks for the same.
 */
#define BCM2711_SHADER_SPIRV_FEATURES (DRV_GPU_SPIRV_FEATURE_SAMPLER_ARRAYS | DRV_GPU_SPIRV_FEATURE_BUFFER_ARRAYS | \
				       DRV_GPU_SPIRV_FEATURE_STORAGE_IMAGE | DRV_GPU_SPIRV_FEATURE_COMPUTE_IMAGES | \
				       DRV_GPU_SPIRV_FEATURE_IMAGE_ARRAYS | DRV_GPU_SPIRV_FEATURE_FRAG_DEPTH | \
				       DRV_GPU_SPIRV_FEATURE_VIEW_INDEX)

/* Thirty-two scalar interface words leave room for expression registers in a two-thread shader. */
#define BCM2711_SHADER_INTERFACE_WORDS 32U

/* Native vertex shaders have distinct coordinate and render-stage VPM output layouts; a compute program runs as a CSD job. */
enum bcm2711_shader_stage {
	BCM2711_SHADER_COORDINATE,
	BCM2711_SHADER_VERTEX,
	BCM2711_SHADER_FRAGMENT,
	BCM2711_SHADER_COMPUTE
};

/* One draw-time uniform describes either exact constant bits or a checked caller-owned binding. */
enum bcm2711_shader_uniform_kind {
	BCM2711_SHADER_CONSTANT,
	BCM2711_SHADER_PUSH,
	BCM2711_SHADER_BLOCK,
	BCM2711_SHADER_TEXTURE,
	BCM2711_SHADER_SAMPLER,
	BCM2711_SHADER_VIEWPORT_X,
	BCM2711_SHADER_VIEWPORT_Y,
	BCM2711_SHADER_VIEWPORT_Z,
	BCM2711_SHADER_DEPTH_OFFSET,

	/*
	 * Compute: the device address of storage buffer (set, binding), and
	 * the largest byte offset an access of `bits` bytes may start at (the
	 * range minus `bits`, zero for a range shorter than `bits`, whose
	 * address the runtime points at the session's own dummy page).  The
	 * shader clamps every offset it computes to that limit.
	 */
	BCM2711_SHADER_STORAGE_BASE,
	BCM2711_SHADER_STORAGE_LIMIT,

	/* Compute: the byte range of storage buffer (set, binding) its descriptor gives. */
	BCM2711_SHADER_STORAGE_SIZE,

	/* Compute: the dispatch's workgroup count along axis `bits` (0 to 2). */
	BCM2711_SHADER_GROUP_COUNT,

	/* Compute: the first workgroup id along axis `bits` of the CSD job, which a split dispatch moves. */
	BCM2711_SHADER_GROUP_BASE,

	/*
	 * Compute: the device address of the job's shared memory arena (one
	 * slot of shared_bytes per workgroup memory slot), and the largest
	 * byte offset in it an access of `bits` bytes may start at.
	 */
	BCM2711_SHADER_SHARED_BASE,
	BCM2711_SHADER_SHARED_LIMIT,

	/*
	 * Vertex: what the draw adds to the hardware's vertex id to make
	 * gl_VertexIndex (nonzero only when the draw fetches from a packed
	 * copy of its vertices, whose first one it moved to zero), and the
	 * draw's first instance, which the hardware's instance id leaves out.
	 */
	BCM2711_SHADER_VERTEX_BASE,
	BCM2711_SHADER_INSTANCE_BASE,

	/* Graphics: the view a multiview pass renders (gl_ViewIndex; 0 outside multiview). */
	BCM2711_SHADER_VIEW_INDEX,

	/*
	 * The size of the view of combined image sampler (set, binding,
	 * element) at its base level along axis `bits`: 0 the width, 1 the
	 * height, 2 a 3D image's depth, 3 an array's layers.
	 */
	BCM2711_SHADER_TEXTURE_SIZE,

	/*
	 * Compute: the view of storage image (set, binding) at its base level,
	 * a raster of rows: the device address of its first layer's first
	 * texel, the largest byte offset from there an access of `bits` bytes
	 * may start at (zero when the view is smaller), the bytes between its
	 * rows and between its layers (a 3D image's slices), and its size
	 * along axis `bits` as TEXTURE_SIZE gives it.
	 */
	BCM2711_SHADER_IMAGE_BASE,
	BCM2711_SHADER_IMAGE_LIMIT,
	BCM2711_SHADER_IMAGE_PITCH,
	BCM2711_SHADER_IMAGE_LAYER,
	BCM2711_SHADER_IMAGE_SIZE
};

/* One varying component identifies the canonical FIFO order shared by both graphics stages. */
struct bcm2711_shader_component {
	uint32_t location;
	uint32_t component;
	uint32_t flat;
	uint32_t noperspective;
};

/* The most colour render targets a fragment program writes (its outputs' locations 0 to 3). */
#define BCM2711_SHADER_TARGETS 4U

/*
 * One pipeline key keeps interpolation and tile-output choices out of mutable shader modules.
 *
 * points is set when the pipeline draws points: its vertex programs then
 * pass on the point size a shader writes, and its fragment program reads
 * gl_PointCoord from the point's implicit varyings.
 *
 * The fragment program writes output location n to render target n, for
 * each of the target_count targets (zero is one target, target 0) that
 * target_unused (bit n) does not leave out.  A target is written as four
 * half floats unless target_wide (bit n) makes it target_components[n]
 * 32-bit words (the raw bits of 32-bit floats and of integers);
 * swap_red_blue (bit n) exchanges red and blue first (a BGRA target).
 */
struct bcm2711_shader_key {
	struct bcm2711_shader_component varyings[BCM2711_SHADER_INTERFACE_WORDS];
	uint32_t varying_count;
	uint32_t swap_red_blue;
	uint32_t points;
	uint32_t target_count;
	uint32_t target_unused;
	uint32_t target_wide;
	uint32_t target_components[BCM2711_SHADER_TARGETS];
};

/*
 * One uniform belongs to one emitted hardware consumption, including implicit tile writes.
 *
 * A descriptor's uniform names its set, binding and, for a binding that is
 * an array of descriptors, the element (zero otherwise).
 */
struct bcm2711_shader_uniform {
	enum bcm2711_shader_uniform_kind kind;
	uint32_t bits;
	uint32_t set;
	uint32_t binding;
	uint32_t element;
	uint32_t offset;
};

/* One fully compiled program owns its arrays until the caller releases it after copying into GPU storage. */
struct bcm2711_shader_binary {
	uint64_t *code;
	uint32_t code_count;
	struct bcm2711_shader_uniform *uniforms;
	uint32_t uniform_count;
	enum bcm2711_shader_stage stage;
	struct bcm2711_shader_component inputs[BCM2711_SHADER_INTERFACE_WORDS];
	uint32_t input_count;
	struct bcm2711_shader_component varyings[BCM2711_SHADER_INTERFACE_WORDS];
	uint32_t varying_count;
	uint32_t push_bytes;
	uint32_t vpm_output_words;
	uint32_t registers_used;
	uint32_t threads;
	uint32_t starts_final;

	/*
	 * A vertex program of a points pipeline: one when its VPM output has
	 * the point size after the header (the shader record's "point size in
	 * shaded vertex data").  A fragment program of a points pipeline: one
	 * when it reads gl_PointCoord, which the implicit point varyings carry
	 * ahead of the others.
	 */
	uint32_t point_size;
	uint32_t point_coord;

	/*
	 * A vertex program that reads gl_InstanceIndex or gl_VertexIndex:
	 * the hardware writes the instance id, then the vertex id, into the
	 * VPM ahead of the attributes (the shader record's "instance ID read"
	 * and "vertex ID read" bits).
	 */
	uint32_t instance_index;
	uint32_t vertex_index;

	/*
	 * A fragment program whose tile writes start with the depth (the
	 * shader record's "fragment shader does Z writes"): one that writes
	 * gl_FragDepth, or on a 7.1 engine one that discards, which writes the
	 * rasterized depth itself.
	 */
	uint32_t writes_depth;

	/*
	 * Compute: the workgroup size; the shared memory bytes and whether the
	 * program waits at a workgroup barrier (and at one inside a loop, which
	 * fixes the dispatch to one workgroup per supergroup).  starts_final
	 * set means the program has no thread switch before its end, a single
	 * segment (CFG5 SINGLE_SEG).
	 */
	uint32_t local_size[3];
	uint32_t shared_bytes;
	uint32_t uses_barrier;
	uint32_t barrier_in_loop;
};

/* One refusal names the source instruction; its reason points to immutable implementation text. */
struct bcm2711_shader_diagnostic {
	uint32_t instruction;
	const char *reason;
};

int bcm2711_shader_preflight(const uint32_t *words, size_t word_count, enum bcm2711_shader_stage stage, struct bcm2711_shader_diagnostic *diagnostic);
int bcm2711_shader_compile(const uint32_t *words, size_t word_count, enum bcm2711_shader_stage stage, const struct bcm2711_shader_key *key, struct bcm2711_shader_binary **binary, struct bcm2711_shader_diagnostic *diagnostic);
void bcm2711_shader_binary_free(struct bcm2711_shader_binary *binary);

#endif
