/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The graphics pipelines and draws of the PowerVR executor: the fixed
 * state a VkGraphicsPipelineCreateInfo keeps and the compile keys of its
 * vertex and fragment shaders, the coefficient program that iterates the
 * varyings for the fragment shader, the viewport and scissor words, the
 * primitive state block of a draw and its vertex control stream (with
 * and without an index buffer).  Nothing here holds a lock or memory;
 * the callers give the heap offsets and device addresses, and the host
 * check builds this file unchanged.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_RENDER_DRAW_H
#define KERN_DRIVERS_GPU_POWERVR_RENDER_DRAW_H

#include <stdbool.h>
#include <stdint.h>

#include <libc/vulkan/vulkan_core.h>

#include "drivers/gpu/powervr/render-pass.h"
#include "drivers/gpu/powervr/compiler/shader.h"

/* The vertex bindings and attributes a pipeline takes. */
#define POWERVR_DRAW_BINDINGS		8U
#define POWERVR_DRAW_ATTRIBUTES		POWERVR_SHADER_INPUTS

/* The most iterators of a coefficient program (w and the varyings). */
#define POWERVR_DRAW_ITERATORS		(POWERVR_SHADER_VARYINGS + 1U)

/* The most data and code words of a coefficient program. */
#define POWERVR_DRAW_COEFFICIENT_DATA	(POWERVR_DRAW_ITERATORS * 2U + 2U)
#define POWERVR_DRAW_COEFFICIENT_CODE	POWERVR_DRAW_ITERATORS

/* The most words of a draw's primitive state block and of its vertex control stream (without the end). */
#define POWERVR_DRAW_PPP_WORDS		31U
#define POWERVR_DRAW_VDM_WORDS		20U

/*
 * Test switches of a draw's primitive state, a bit each, none as
 * built: the pass type punch-through for opaque draws (Mesa's for a
 * shader that discards).  The former 1 and 2 (no varying base without
 * a coefficient program, no data sequencer temporaries) are built in.
 */
#ifndef POWERVR_DRAW_TRIAL
#define POWERVR_DRAW_TRIAL			0U
#endif
#define POWERVR_DRAW_TRIAL_PUNCH_THROUGH	4U

/*
 * The fragment shader forms a pipeline takes (the compiler's
 * POWERVR_SHADER_FORM_*), as built: the shared program, run once a draw
 * from the shared register program (0x1); the sample that iterates its
 * coordinates (0x4); the colour written by one sum of products (0x8)
 * that clamps the blended colour itself (0x10); and the iterate and
 * sample that waits for its own data (0x20).  Each of these drew right
 * on the board.
 */
#define POWERVR_DRAW_FORMS			0x3dU

/* The words of a viewport transform (a0, m0, a1, m1, a2, m2) and of a scissor. */
#define POWERVR_DRAW_VIEWPORT_WORDS	6U
#define POWERVR_DRAW_SCISSOR_WORDS	2U

/* One vertex binding: its number, its stride and whether it steps per instance rather than per vertex. */
struct powervr_draw_binding {
	uint32_t binding;
	uint32_t stride;
	bool instance;
};

/* One vertex attribute: its location, binding, format and offset. */
struct powervr_draw_attribute {
	uint32_t location;
	uint32_t binding;
	uint32_t format;
	uint32_t offset;
};

/*
 * The fixed state of a graphics pipeline and the keys its shaders are
 * compiled with.  The viewport and scissor are the static ones unless
 * dynamic.  Blending lives in the fragment key.  Discards is set once
 * the fragment shader is compiled, when it may discard its fragment.
 */
struct powervr_draw_fixed {
	uint32_t topology;
	uint32_t cull_mode;
	uint32_t front_face;
	bool depth_test;
	bool depth_write;
	uint32_t depth_compare;
	bool blending;
	bool discards;
	bool dynamic_viewport;
	bool dynamic_scissor;
	VkViewport viewport;
	VkRect2D scissor;
	uint32_t binding_count;
	struct powervr_draw_binding bindings[POWERVR_DRAW_BINDINGS];
	uint32_t attribute_count;
	struct powervr_draw_attribute attributes[POWERVR_DRAW_ATTRIBUTES];
	struct powervr_shader_key vertex_key;
	struct powervr_shader_key fragment_key;

	/* The colour blend's constants as the pipeline gives them, the float bits. */
	uint32_t blend_constants[4];
};

/*
 * What the vertex and fragment programs need between them: the
 * coefficient program's data and code, the output select and varying
 * words of the primitive state, and the fragment's coefficient and
 * coefficient program sizes.
 */
struct powervr_draw_varyings {
	uint32_t data[POWERVR_DRAW_COEFFICIENT_DATA];
	uint32_t code[POWERVR_DRAW_COEFFICIENT_CODE];
	uint32_t data_words;
	uint32_t code_words;
	uint32_t output_select;
	uint32_t varying[2];
	uint32_t usc_varying_size;
};

/*
 * The inputs of a draw's primitive state block: the pipeline, whether
 * the job has a depth attachment, the fragment's programs (the task
 * program's data, the shared register program's code and data with its
 * data words and the fragment's shared registers, the coefficient
 * program's data and data words), the viewport words, the scissor index
 * and the region clip words.
 */
struct powervr_draw_ppp {
	const struct powervr_draw_fixed *fixed;
	const struct powervr_draw_varyings *varyings;
	bool has_depth;
	uint32_t fragment_kick_offset;
	uint32_t shared_code_offset;
	uint32_t shared_data_offset;
	uint32_t shared_data_words;
	uint32_t fragment_shareds;
	uint32_t coefficient_offset;
	uint32_t viewport[POWERVR_DRAW_VIEWPORT_WORDS];
	uint32_t scissor_index;
	uint32_t region_clip[2];
};

/*
 * The inputs of a draw's vertex control stream: the primitive state
 * block's address and words, the vertex programs (the attribute program's
 * data and code and its data words and temporaries, the shared register
 * program's code and data and its data words, the vertex shader's shared
 * registers, inputs and outputs), the topology, and the draw (indexed or
 * not, the index buffer's address and index size, the count, instances
 * and the vertex offset).
 */
struct powervr_draw_vdm {
	uint64_t ppp_address;
	uint32_t ppp_words;
	uint32_t vertex_data_offset;
	uint32_t vertex_code_offset;
	uint32_t vertex_data_words;
	uint32_t vertex_temps;
	uint32_t shared_code_offset;
	uint32_t shared_data_offset;
	uint32_t shared_data_words;
	uint32_t vertex_shareds;
	uint32_t vertex_inputs;
	uint32_t vertex_outputs;
	uint32_t topology;
	bool indexed;
	uint64_t index_address;
	uint32_t index_bytes;
	uint32_t count;
	uint32_t instances;
	uint32_t vertex_offset;
};

int powervr_draw_fixed_build(const VkGraphicsPipelineCreateInfo *info, const struct powervr_pass *pass, struct powervr_draw_fixed *fixed);
int powervr_draw_varyings_build(const struct powervr_shader_program *vertex, const struct powervr_shader_program *fragment, struct powervr_draw_varyings *varyings);
int powervr_draw_viewport_words(const VkViewport *viewport, uint32_t words[POWERVR_DRAW_VIEWPORT_WORDS]);
int powervr_draw_scissor_words(const VkViewport *viewport, const VkRect2D *scissor, uint32_t words[POWERVR_DRAW_SCISSOR_WORDS], uint32_t clip[2]);
int powervr_draw_ppp_build(const struct powervr_draw_ppp *input, uint32_t words[POWERVR_DRAW_PPP_WORDS], uint32_t *count);
int powervr_draw_vdm_build(const struct powervr_draw_vdm *input, uint32_t words[POWERVR_DRAW_VDM_WORDS], uint32_t *count);

#endif /* KERN_DRIVERS_GPU_POWERVR_RENDER_DRAW_H */
