/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Compiled immutable graphics pipelines retain exact shader interfaces independently of module and layout identities. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_PIPELINE_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_PIPELINE_H

#include "drivers/gpu/videocore/compiler/shader.h"
#include "drivers/gpu/videocore/native-storage.h"
#include "drivers/gpu/videocore/vk/vulkan-layout.h"
#include "drivers/gpu/videocore/vk/vulkan-target.h"

#define BCM2711_VULKAN_VERTEX_BINDINGS 16U
#define BCM2711_VULKAN_VERTEX_ATTRIBUTES 16U

/* The core dynamic states a pipeline may name, VIEWPORT (0) through STENCIL_REFERENCE (8). */
#define BCM2711_VULKAN_DYNAMIC_STATES 9U

/*
 * The values the dynamic-state commands last recorded, as IEEE bits for the
 * floats; a pipeline uses the ones it declares dynamic instead of its own.
 * The stencil arrays are indexed by face, front first.
 */
struct bcm2711_vulkan_dynamic {
	uint32_t line_width;
	uint32_t depth_bias[3];
	uint32_t blend_constants[4];
	uint32_t compare_mask[2];
	uint32_t write_mask[2];
	uint32_t reference[2];
};

/* One pipeline owns three native programs (a compute pipeline one) and immutable fetch/raster state until its final recorded/prepared owner retires. */
struct bcm2711_vulkan_pipeline {
	struct bcm2711_vulkan_input_owner owner;
	struct bcm2711_vulkan_object *pass;
	struct bcm2711_shader_binary *programs[3];
	/*
	 * Each program's code in GPU storage, uploaded by the first draw that
	 * uses it and kept for every later pass until the pipeline is released
	 * (which happens only after every job of a command buffer naming it has
	 * retired); null until then, and always null in an internal draw's copy.
	 */
	struct bcm2711_native_storage *code[3];
	VkVertexInputBindingDescription bindings[BCM2711_VULKAN_VERTEX_BINDINGS];
	VkVertexInputAttributeDescription attributes[BCM2711_VULKAN_VERTEX_ATTRIBUTES];
	uint32_t binding_count;
	uint32_t attribute_count;
	VkCullModeFlags cull;
	VkFrontFace front;
	/* The rasterizer discards every primitive: the bin enables neither facing, so no primitive is drawn. */
	bool discard;
	/*
	 * The samples a pixel the pipeline rasterizes (1 or 4; 0 for a
	 * discarding pipeline without multisample state) and the samples it
	 * covers (bit n: sample n of four).
	 */
	uint32_t samples;
	uint32_t sample_mask;
	/*
	 * A static viewport (its six IEEE words, as a SetViewport record has
	 * them) and scissor, which the draws take when the pipeline does not
	 * declare them dynamic.
	 */
	bool static_viewport;
	bool static_scissor;
	uint32_t viewport[6];
	VkRect2D scissor;
	/*
	 * The primitive topology (a list or strip of points, lines or
	 * triangles, or a triangle fan) and whether an index of all ones
	 * restarts a strip or fan.
	 */
	VkPrimitiveTopology topology;
	bool restart;
	/* The static line width (IEEE bits), used unless LINE_WIDTH is dynamic. */
	uint32_t line_width;
	/*
	 * Depth bias: whether the pipeline offsets depths, and its constant
	 * factor, clamp and slope factor (IEEE bits), which the draw's
	 * SetDepthBias replaces when DEPTH_BIAS is dynamic.
	 */
	bool depth_bias;
	uint32_t depth_bias_constant;
	uint32_t depth_bias_clamp;
	uint32_t depth_bias_slope;
	/* The dynamic states the pipeline declares: bit n is VkDynamicState n. */
	uint32_t dynamic;
	/*
	 * The attachments that blend (bit n: colour attachment n of the blend
	 * state), and the targets (bit n: render target n) the tile buffer
	 * blends: those of the pass's colour slots whose formats it blends.
	 */
	uint32_t blend;
	uint32_t blend_targets;
	/*
	 * Each attachment's blend equation (the native Blend Cfg's factor and
	 * mode fields, bits 0 to 23); premultiplied source-over for one that
	 * does not blend.
	 */
	uint32_t blend_factors[BCM2711_VULKAN_PASS_COLOURS];
	/*
	 * The colour attachments the blend state names, and the colour channels
	 * the pipeline leaves unwritten in the native mask's order, four bits a
	 * render target (bit 4n red through bit 4n + 3 alpha of target n): each
	 * attachment's mask in its slot (until the build spreads them over the
	 * pass's slots, attachment n's in bits 4n to 4n + 3), every channel of
	 * the targets the pass leaves unused.
	 */
	uint32_t colour_count;
	uint32_t colour_skip;
	/* The static blend constants as IEEE bits, used unless BLEND_CONSTANTS is dynamic. */
	uint32_t blend_constants[4];
	/*
	 * The depth/stencil state, which takes effect only in a
	 * pass whose depth/stencil attachment has the aspect: the depth test,
	 * whether it writes, and the stencil test of each face.
	 */
	bool depth_test;
	bool depth_write;
	VkCompareOp depth_compare;
	bool stencil;
	VkStencilOpState stencil_front;
	VkStencilOpState stencil_back;

	/*
	 * A compute pipeline: programs[0] is its compute program and code[0]
	 * its code once a dispatch has uploaded it; it has no render pass and
	 * binds only to the compute bind point.
	 */
	bool compute;
};

int bcm2711_vulkan_pipeline_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_pipeline_state(struct bcm2711_vulkan_pipeline *pipeline, const VkGraphicsPipelineCreateInfo *info);

/* The four-byte words of a vertex attribute format the fetch reads (32-bit floats or integers), zero for another format. */
uint32_t bcm2711_vulkan_attribute_components(VkFormat format);

/* The bits of the one a missing fourth component of a vertex attribute format reads: 1.0 for floats, 1 for integers. */
uint32_t bcm2711_vulkan_attribute_one(VkFormat format);
int bcm2711_vulkan_pipeline_build(struct bcm2711_vulkan_session *session, struct bcm2711_vulkan_object *device, const VkGraphicsPipelineCreateInfo *info, struct bcm2711_vulkan_pipeline **pipeline);
int bcm2711_vulkan_pipeline_build_compute(struct bcm2711_vulkan_session *session, struct bcm2711_vulkan_object *device, const VkComputePipelineCreateInfo *info, struct bcm2711_vulkan_pipeline **pipeline);
int bcm2711_vulkan_pipeline_release(struct bcm2711_vulkan_session *session, void *payload);

#endif
