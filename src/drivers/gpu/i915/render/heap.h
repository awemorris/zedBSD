/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The layout of the render paths' session objects, shared by state.c,
 * draw.c and blit.c.
 *
 * The state object is a row of slots, one for each operation (a draw or a
 * rectangle) of the batch in progress.  A slot holds every heap its
 * operation points at, at fixed offsets from the slot's start; each
 * operation programs STATE_BASE_ADDRESS at its own slot, so the offsets
 * inside the heaps are the pointers the state packets carry, and a draw and
 * a rectangle must agree on every offset here.
 *
 *   0x0000  surface state heap: the binding table, the render target's
 *           surface state, then one surface state per sampled texture
 *   0x1000  dynamic state heap: colour calc, blend, viewports, scissor,
 *           coarse pixel state and the samplers
 *   0x2000  the vertex stage's push data (absolute address in
 *           3DSTATE_CONSTANT_VS): push constants, then uniform blocks
 *   0x2400  the pixel stage's push data, laid out the same way
 *   0x2800  the three vertices of a rectangle
 *   0x2c00  the geometry stage's push data (ws075-p007b), laid out the
 *           same way
 *   0x3000  scratch for the post-sync writes
 *
 * The kernel object is a row of instruction windows, each the instruction
 * heap of the operations that run one set of kernels: the vertex kernel at
 * 0, the geometry kernel at 16 KiB, the pixel kernel at 32 KiB, and
 * threads that only end themselves everywhere else.  A pipeline's kernels,
 * and the rectangle kernels, are placed in a window once and found there by
 * every later operation.
 */

#ifndef DRIVERS_GPU_I915_RENDER_HEAP_H
#define DRIVERS_GPU_I915_RENDER_HEAP_H

/* The size of one operation's slot, and how many slots the state object has. */
#define I915_GFX_SLOT_BYTES		0x4000U
#define I915_GFX_SLOTS			128U

/* The size of the state object. */
#define I915_GFX_STATE_BYTES		(I915_GFX_SLOTS * I915_GFX_SLOT_BYTES)

/*
 * The surface state base, and the offsets from it: the binding table, the
 * first render target's surface state, and texture n's at RSS_TEXTURE + n *
 * RSS_BYTES for the MAX_TEXTURES textures a pixel kernel may sample.
 */
#define I915_GFX_SURFACE_HEAP		0x0000U
#define I915_GFX_BINDING_TABLE		0x0000U
#define I915_GFX_RSS_TARGET		0x0080U
#define I915_GFX_RSS_TEXTURE		0x00c0U
#define I915_GFX_RSS_BYTES		0x0040U
#define I915_GFX_MAX_TEXTURES		16U

/* The surface states of render targets 1 to 3 (I915_SHADER_RT_BTI), after the textures'. */
#define I915_GFX_RSS_EXTRA_TARGET	0x04c0U

/* The dynamic state base, and the offsets from it. */
#define I915_GFX_DYNAMIC_HEAP		0x1000U
#define I915_GFX_DYN_COLOR_CALC		0x0000U
#define I915_GFX_DYN_BLEND		0x0040U
#define I915_GFX_DYN_CC_VIEWPORT	0x0080U
#define I915_GFX_DYN_SF_CLIP_VIEWPORT	0x00c0U
#define I915_GFX_DYN_SCISSOR		0x0100U
#define I915_GFX_DYN_CPS		0x0180U

/* The samplers, SAMPLER_BYTES apart, one per texture. */
#define I915_GFX_DYN_SAMPLER		0x0200U
#define I915_GFX_SAMPLER_BYTES		16U

/* The samplers' border colours, BORDER_BYTES apart (64-byte aligned), one per texture. */
#define I915_GFX_DYN_BORDER		0x0400U
#define I915_GFX_BORDER_BYTES		64U

/*
 * The push data of the vertex, the pixel and the geometry stage, addressed
 * absolutely by 3DSTATE_CONSTANT_VS, _PS and _GS, at most PUSH_DATA_BYTES
 * each; the geometry stage's follows the rectangle's vertices.
 */
#define I915_GFX_PUSH_BUFFER		0x2000U
#define I915_GFX_PS_PUSH_BUFFER		0x2400U
#define I915_GFX_GS_PUSH_BUFFER		0x2c00U
#define I915_GFX_PUSH_DATA_BYTES	0x0400U

/* The three vertices of a rectangle, twelve floats each. */
#define I915_GFX_RECT_VERTICES		0x2800U
#define I915_GFX_RECT_VERTEX_BYTES	48U

/* Where the post-sync writes of the depth workaround land. */
#define I915_GFX_SCRATCH		0x3000U

/*
 * The kernel offsets from the start of an instruction window, and the
 * window's size: 16 KiB for the vertex kernel, 16 KiB for the geometry
 * kernel (ws075-p007b) and 32 KiB for the pixel kernel.  A compute kernel,
 * or a blit's, has a window to itself.
 */
#define I915_GFX_VS_KERNEL		0x0000U
#define I915_GFX_GS_KERNEL		0x4000U
#define I915_GFX_PS_KERNEL		0x8000U
#define I915_GFX_INSTRUCTION_BYTES	0x10000U

/* How many instruction windows the kernel object has. */
#define I915_GFX_KERNEL_WINDOWS		32U

/* The size of the kernel object. */
#define I915_GFX_KERNEL_BYTES		(I915_GFX_KERNEL_WINDOWS * I915_GFX_INSTRUCTION_BYTES)

/*
 * The size of the batch object the operations of a submission are written
 * into, and the room one operation may take at most; an operation that
 * does not find that much room left first runs what is recorded.
 */
#define I915_GFX_BATCH_BYTES		(1024U * 1024U)
#define I915_GFX_OP_MAX_DWORDS		4096U

/*
 * The largest vertex shader thread count of the target (intel_device_info
 * max_vs_threads of Alder Lake-P GT2).
 *
 * XXX: a property of the device, not of the render path; it belongs to the
 * device information and is fixed here for the one target.
 */
#define I915_GFX_MAX_VS_THREADS		546U

/*
 * The largest geometry shader thread count of the target (intel_device_info
 * max_gs_threads of Gen12, GFX12_HW_INFO).
 *
 * XXX: a property of the device fixed here for the one target, as above.
 */
#define I915_GFX_MAX_GS_THREADS		336U

/*
 * The thread ids the scratch memory of a stage has to cover (Mesa 25.0.7
 * src/intel/dev/intel_device_info.c, sha256
 * 1a3c7c6d87c4a60af5d1003096b4add0a34c4a02afc12b40a378cc060540afd8,
 * init_max_scratch_ids()): the vertex stage's max_vs_threads, the pixel
 * stage's max_wm_threads, which on Gen12 is 128 threads per PSD on each of
 * the 8 subslices of every slice present.
 */
#define I915_GFX_VS_SCRATCH_IDS			I915_GFX_MAX_VS_THREADS
#define I915_GFX_PS_SCRATCH_IDS_PER_SLICE	(128U * 8U)

/*
 * The session's scratch buffer: an unused first page, so no stage's
 * scratch pointer is zero, then the vertex stage's part, the pixel
 * stage's, the compute stage's and the geometry stage's, each
 * page-aligned.  A draw whose kernels spill makes the buffer
 * the general state base, whose size is 4 GiB less a page.
 */
#define I915_GFX_SCRATCH_GUARD			4096U
#define I915_GFX_SCRATCH_ALIGN			4096U
#define I915_GFX_GENERAL_STATE_BYTES		0xfffff000ULL

/*
 * The thread ids the scratch memory of the compute stage has to cover
 * (Mesa 25.0.7 intel_device_info.c, init_max_scratch_ids() on Gen12:
 * 16 * 8 ids a subslice, and 6 subslices for a GT2 whatever the fuses left)
 * (ws101-p004).
 */
#define I915_GFX_CS_SCRATCH_IDS			(16U * 8U * 6U)

/*
 * The thread ids the scratch memory of the geometry stage has to cover:
 * its max_gs_threads (Mesa 25.0.7 intel_device_info.c,
 * init_max_scratch_ids(), as for the vertex stage) (ws075-p007b b4).
 */
#define I915_GFX_GS_SCRATCH_IDS			I915_GFX_MAX_GS_THREADS

/*
 * Where a dispatch writes in its slot (ws101-p004), as offsets from the
 * dynamic state base (the slot's dynamic heap): the interface descriptor,
 * the three group counts gl_NumWorkGroups reads, and the CURBE -- the
 * cross-thread push data, then each thread's own -- which may take up to
 * the slot's post-sync scratch (I915_GFX_SCRATCH).
 */
#define I915_GFX_DYN_INTERFACE			0x0000U
#define I915_GFX_DYN_GROUP_COUNTS		0x0040U
#define I915_GFX_DYN_CURBE			0x1000U
#define I915_GFX_CURBE_BYTES			(I915_GFX_SCRATCH - I915_GFX_DYNAMIC_HEAP - I915_GFX_DYN_CURBE)

/*
 * The MOCS table entry the render paths use for their surfaces, vertex
 * buffers and state: index 3, uncached (see GEN12_MOCS()).
 */
#define I915_MOCS_UNCACHED_INDEX	3U

#endif
