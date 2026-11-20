/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's own quad programs (amdrdna-vk-exec.h), built as IR for
 * the amdrdna compiler: a quad placed by push constants, and either one
 * colour, a texture read at the push constants' texture rectangle, the
 * average of the samples of an image of samples there, the bits of a
 * texel there written as a depth or a stencil reference, or the bits of
 * texels there packed into a colour's bytes.  Copies, blits, clears and
 * resolves are drawn with them.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>

#include "drivers/gpu/amdrdna/vk/amdrdna-vk-exec.h"

/*
 * The float bits of 0, 1 and 0.25.
 *
 * Values referenced: IEEE 754-2008 binary32 (sign 31, biased exponent
 * 30:23 with a bias of 127, fraction 22:0).
 */
#define QUAD_FLOAT_ZERO		0x00000000U
#define QUAD_FLOAT_ONE		0x3f800000U
#define QUAD_FLOAT_QUARTER	0x3e800000U

/* The resolve program's values: each sample's ten from v4 on. */
#define QUAD_RESOLVE_FIRST	4U
#define QUAD_RESOLVE_VALUES	10U

/*
 * The float bits of 1/255 and of 1/65535 (the steps of an 8-bit and of a
 * 16-bit normalized value), and the quarter of a step a value written in
 * one is raised by: the unit that turns the float back into the integer
 * then gives the integer whether it rounds or truncates.
 *
 * Values referenced: IEEE 754-2008 binary32 (1/255 rounded to nearest is
 * 0x3b808081, 1/65535 is 0x37800080).
 */
#define QUAD_FLOAT_STEP_8	0x3b808081U
#define QUAD_FLOAT_STEP_16	0x37800080U

/* The texel fetch's parameters: x, y and the level, as integers. */
#define QUAD_FETCH_PARAMETERS	3U

/* The packing program's values: the shared ones up to v12, then each texel's twelve. */
#define QUAD_PACK_FIRST		12U
#define QUAD_PACK_VALUES	12U

static void quad_ir_load(struct drv_gpu_shader_ir_inst *list, uint32_t *count, uint32_t dst, enum drv_gpu_shader_ir_op op, uint32_t location, uint32_t component, uint32_t immediate);
static void quad_ir_mix(struct drv_gpu_shader_ir_inst *list, uint32_t *count, uint32_t base, uint32_t corner, uint32_t from, uint32_t to);
static void quad_ir_store(struct drv_gpu_shader_ir_inst *list, uint32_t *count, uint32_t location, uint32_t component, uint32_t value);
static void quad_ir_move(struct drv_gpu_shader_ir_inst *list, uint32_t *count, uint32_t dst, uint32_t src);
static void quad_ir_two(struct drv_gpu_shader_ir_inst *list, uint32_t *count, enum drv_gpu_shader_ir_op op, uint32_t dst, uint32_t first, uint32_t second);
static void quad_ir_fetch(struct drv_gpu_shader_ir_inst *list, uint32_t *count, uint32_t base, uint32_t x, uint32_t y);
static void quad_ir_fetch_sample_zero(struct drv_gpu_shader_ir_inst *list, uint32_t *count, uint32_t base, uint32_t x, uint32_t y);
static void quad_ir_texture_input(struct amdrdna_quad_ir *program);

/*
 * Builds the IR of the quad's vertex program, as the frontend would parse
 *
 *   layout(push_constant) uniform Q { vec4 rect; vec4 texture; vec4 colour; } q;
 *   layout(location = 0) in vec2 corner;
 *   layout(location = 0) out vec2 texcoord;      (textured only)
 *   void main() {
 *       gl_Position = vec4(mix(q.rect.xy, q.rect.zw, corner), 0.0, 1.0);
 *       texcoord = mix(q.texture.xy, q.texture.zw, corner);
 *   }
 *
 * with mix written as a + (b - a) * t.
 */
void
amdrdna_quad_vertex_ir(
	struct amdrdna_quad_ir *program,
	int textured)
{
	struct drv_gpu_shader_ir_inst *list;
	struct drv_gpu_shader_ir *ir;
	uint32_t count;

	/* Starts empty, the instructions appended in order. */
	kern_memset(program, 0, sizeof(*program));
	list = program->instructions;
	ir = &program->ir;
	count = 0U;

	/* v0, v1 = the corner; v2..v5 = the rectangle's corners. */
	quad_ir_load(list, &count, 0U, DRV_GPU_IR_LOAD_INPUT, 0U, 0U, 0U);
	quad_ir_load(list, &count, 1U, DRV_GPU_IR_LOAD_INPUT, 0U, 1U, 0U);
	quad_ir_load(list, &count, 2U, DRV_GPU_IR_LOAD_PUSH, 0U, 0U, AMDRDNA_QUAD_PUSH_RECT);
	quad_ir_load(list, &count, 3U, DRV_GPU_IR_LOAD_PUSH, 0U, 0U, AMDRDNA_QUAD_PUSH_RECT + 4U);
	quad_ir_load(list, &count, 4U, DRV_GPU_IR_LOAD_PUSH, 0U, 0U, AMDRDNA_QUAD_PUSH_RECT + 8U);
	quad_ir_load(list, &count, 5U, DRV_GPU_IR_LOAD_PUSH, 0U, 0U, AMDRDNA_QUAD_PUSH_RECT + 12U);

	/* v8 = x, v11 = y; v12 = 0, v13 = 1; the position. */
	quad_ir_mix(list, &count, 6U, 0U, 2U, 4U);
	quad_ir_mix(list, &count, 9U, 1U, 3U, 5U);
	quad_ir_load(list, &count, 12U, DRV_GPU_IR_CONST, 0U, 0U, QUAD_FLOAT_ZERO);
	quad_ir_load(list, &count, 13U, DRV_GPU_IR_CONST, 0U, 0U, QUAD_FLOAT_ONE);
	quad_ir_store(list, &count, DRV_GPU_IR_LOCATION_POSITION, 0U, 8U);
	quad_ir_store(list, &count, DRV_GPU_IR_LOCATION_POSITION, 1U, 11U);
	quad_ir_store(list, &count, DRV_GPU_IR_LOCATION_POSITION, 2U, 12U);
	quad_ir_store(list, &count, DRV_GPU_IR_LOCATION_POSITION, 3U, 13U);
	ir->value_count = 14U;

	/* Textured: v14..v17 = the texture's part, v20 and v23 = the coordinate at location 0. */
	if (textured != 0) {
		quad_ir_load(list, &count, 14U, DRV_GPU_IR_LOAD_PUSH, 0U, 0U, AMDRDNA_QUAD_PUSH_TEXTURE);
		quad_ir_load(list, &count, 15U, DRV_GPU_IR_LOAD_PUSH, 0U, 0U, AMDRDNA_QUAD_PUSH_TEXTURE + 4U);
		quad_ir_load(list, &count, 16U, DRV_GPU_IR_LOAD_PUSH, 0U, 0U, AMDRDNA_QUAD_PUSH_TEXTURE + 8U);
		quad_ir_load(list, &count, 17U, DRV_GPU_IR_LOAD_PUSH, 0U, 0U, AMDRDNA_QUAD_PUSH_TEXTURE + 12U);
		quad_ir_mix(list, &count, 18U, 0U, 14U, 16U);
		quad_ir_mix(list, &count, 21U, 1U, 15U, 17U);
		quad_ir_store(list, &count, 0U, 0U, 20U);
		quad_ir_store(list, &count, 0U, 1U, 23U);
		ir->value_count = 24U;
		program->output.location = 0U;
		program->output.components = 2U;
		ir->outputs = &program->output;
		ir->output_count = 1U;
	}

	/* The corner is input 0. */
	program->input.location = 0U;
	program->input.components = 2U;

	/* The program. */
	ir->stage = DRV_GPU_STAGE_VERTEX;
	ir->inputs = &program->input;
	ir->input_count = 1U;
	ir->instructions = list;
	ir->instruction_count = count;
	ir->push_bytes = AMDRDNA_QUAD_PUSH_BYTES;
}

/*
 * Builds the IR of the quad's fragment program, as the frontend would
 * parse
 *
 *   layout(location = 0) out vec4 colour;
 *   void main() { colour = q.colour; }                       (one colour)
 *
 *   layout(set = 0, binding = 0) uniform sampler2D image;
 *   layout(location = 0) in vec2 texcoord;
 *   void main() { colour = texture(image, texcoord); }       (textured)
 */
void
amdrdna_quad_fragment_ir(
	struct amdrdna_quad_ir *program,
	int textured)
{
	struct drv_gpu_shader_ir_inst *list;
	struct drv_gpu_shader_ir *ir;
	uint32_t component;
	uint32_t colour;
	uint32_t count;

	/* Starts empty, the instructions appended in order. */
	kern_memset(program, 0, sizeof(*program));
	list = program->instructions;
	ir = &program->ir;
	count = 0U;

	/*
	 * Textured: v0, v1 = the coordinate, v2..v5 = the sample of image
	 * (0, 0) there.  One colour: v0..v3 = the push constants' colour.
	 */
	if (textured != 0) {
		quad_ir_load(list, &count, 0U, DRV_GPU_IR_LOAD_INPUT, 0U, 0U, 0U);
		quad_ir_load(list, &count, 1U, DRV_GPU_IR_LOAD_INPUT, 0U, 1U, 0U);
		list[count].op = DRV_GPU_IR_SAMPLE;
		list[count].dst = 2U;
		list[count].src[0] = 0U;
		list[count].src[1] = 1U;
		list[count].location = 0U;
		list[count].immediate = 0U;
		count++;
		colour = 2U;
		program->input.location = 0U;
		program->input.components = 2U;
		program->uniform.set = 0U;
		program->uniform.binding = 0U;
		program->uniform.kind = DRV_GPU_IR_UNIFORM_SAMPLED_IMAGE;
		ir->inputs = &program->input;
		ir->input_count = 1U;
		ir->uniforms = &program->uniform;
		ir->uniform_count = 1U;
	} else {
		for (component = 0U; component < 4U; component++)
			quad_ir_load(list, &count, component, DRV_GPU_IR_LOAD_PUSH, 0U, 0U, AMDRDNA_QUAD_PUSH_COLOUR + component * 4U);
		colour = 0U;
		ir->push_bytes = AMDRDNA_QUAD_PUSH_BYTES;
	}

	/* The four channels to colour 0. */
	for (component = 0U; component < 4U; component++)
		quad_ir_store(list, &count, 0U, component, colour + component);
	program->output.location = 0U;
	program->output.components = 4U;

	/* The program. */
	ir->stage = DRV_GPU_STAGE_FRAGMENT;
	ir->outputs = &program->output;
	ir->output_count = 1U;
	ir->instructions = list;
	ir->instruction_count = count;
	ir->value_count = colour + 4U;
}

/*
 * Builds the IR of the resolve's fragment program, as the frontend parses
 *
 *   layout(set = 0, binding = 0) uniform sampler2DMS image;
 *   layout(location = 0) in vec2 texcoord;
 *   void main() {
 *       ivec2 p = ivec2(texcoord);
 *       colour = (texelFetch(image, p, 0) + texelFetch(image, p, 1) +
 *                 texelFetch(image, p, 2) + texelFetch(image, p, 3)) * 0.25;
 *   }
 *
 * the texture coordinate the source's pixel (the quad's texture rectangle
 * in pixels), the average of the AMDRDNA_VK_SAMPLES_MAX samples.  Each
 * fetch's parameters are x, y, a zero and the sample, as the frontend
 * passes a multisampled image's fetch.
 */
void
amdrdna_quad_resolve_ir(
	struct amdrdna_quad_ir *program)
{
	struct drv_gpu_shader_ir_inst *list;
	struct drv_gpu_shader_ir *ir;
	uint32_t component;
	uint32_t sample;
	uint32_t base;
	uint32_t sum;
	uint32_t count;

	/* Starts empty, the instructions appended in order. */
	kern_memset(program, 0, sizeof(*program));
	list = program->instructions;
	ir = &program->ir;
	count = 0U;

	/* v0, v1 = the coordinate; v2, v3 = it as integers. */
	quad_ir_load(list, &count, 0U, DRV_GPU_IR_LOAD_INPUT, 0U, 0U, 0U);
	quad_ir_load(list, &count, 1U, DRV_GPU_IR_LOAD_INPUT, 0U, 1U, 0U);
	for (component = 0U; component < 2U; component++) {
		list[count].op = DRV_GPU_IR_F2I;
		list[count].dst = 2U + component;
		list[count].src[0] = component;
		count++;
	}

	/*
	 * Each sample s (from v4 on, ten values each): the sample and a zero,
	 * the four parameters copied together, the fetch's four channels.
	 */
	for (sample = 0U; sample < AMDRDNA_VK_SAMPLES_MAX; sample++) {
		base = QUAD_RESOLVE_FIRST + sample * QUAD_RESOLVE_VALUES;
		quad_ir_load(list, &count, base, DRV_GPU_IR_ICONST, 0U, 0U, sample);
		quad_ir_load(list, &count, base + 1U, DRV_GPU_IR_ICONST, 0U, 0U, 0U);
		quad_ir_move(list, &count, base + 2U, 2U);
		quad_ir_move(list, &count, base + 3U, 3U);
		quad_ir_move(list, &count, base + 4U, base + 1U);
		quad_ir_move(list, &count, base + 5U, base);
		list[count].op = DRV_GPU_IR_TEXTURE;
		list[count].dst = base + 6U;
		list[count].src[0] = base + 2U;
		list[count].src[1] = 4U;
		list[count].location = 0U;
		list[count].immediate = 0U;
		list[count].component = DRV_GPU_IR_TEXTURE_LD;
		list[count].image_shape = DRV_GPU_IR_IMAGE_2D | DRV_GPU_IR_IMAGE_MULTISAMPLED;
		count++;
	}

	/* The sum of the samples' channels, then a quarter of it. */
	sum = QUAD_RESOLVE_FIRST + AMDRDNA_VK_SAMPLES_MAX * QUAD_RESOLVE_VALUES;
	for (sample = 1U; sample < AMDRDNA_VK_SAMPLES_MAX; sample++) {
		for (component = 0U; component < 4U; component++) {
			list[count].op = DRV_GPU_IR_FADD;
			list[count].dst = sum + 4U * (sample - 1U) + component;
			list[count].src[0] = QUAD_RESOLVE_FIRST + 6U + component;
			if (sample > 1U)
				list[count].src[0] = sum + 4U * (sample - 2U) + component;
			list[count].src[1] = QUAD_RESOLVE_FIRST + sample * QUAD_RESOLVE_VALUES + 6U + component;
			count++;
		}
	}
	base = sum + 4U * (AMDRDNA_VK_SAMPLES_MAX - 1U);
	quad_ir_load(list, &count, base, DRV_GPU_IR_CONST, 0U, 0U, QUAD_FLOAT_QUARTER);
	for (component = 0U; component < 4U; component++) {
		list[count].op = DRV_GPU_IR_FMUL;
		list[count].dst = base + 1U + component;
		list[count].src[0] = base - 4U + component;
		list[count].src[1] = base;
		count++;
	}

	/* The four channels to colour 0. */
	for (component = 0U; component < 4U; component++)
		quad_ir_store(list, &count, 0U, component, base + 1U + component);
	program->input.location = 0U;
	program->input.components = 2U;
	program->output.location = 0U;
	program->output.components = 4U;
	program->uniform.set = 0U;
	program->uniform.binding = 0U;
	program->uniform.kind = DRV_GPU_IR_UNIFORM_SAMPLED_IMAGE;

	/* The program. */
	ir->stage = DRV_GPU_STAGE_FRAGMENT;
	ir->inputs = &program->input;
	ir->input_count = 1U;
	ir->outputs = &program->output;
	ir->output_count = 1U;
	ir->uniforms = &program->uniform;
	ir->uniform_count = 1U;
	ir->instructions = list;
	ir->instruction_count = count;
	ir->value_count = base + 5U;
}

/*
 * Builds the IR of a fragment program that writes the bits of a texel as
 * the depth unit's depth or stencil, as the frontend would parse
 *
 *   layout(set = 0, binding = 0) uniform usampler2D bits;
 *   layout(location = 0) in vec2 texcoord;
 *   void main() {
 *       uint t = texelFetch(bits, ivec2(texcoord), 0).x;
 *       gl_FragDepth = uintBitsToFloat(t);               (AMDRDNA_QUAD_DEPTH_32)
 *       gl_FragDepth = (float(t) + 0.25) / 65535.0;      (AMDRDNA_QUAD_DEPTH_16)
 *       gl_FragStencilRefARB = int(t);                   (AMDRDNA_QUAD_STENCIL)
 *   }
 *
 * the texture coordinate the source's texel (the quad's texture rectangle
 * in texels), the texel read as an unsigned integer of its bits.  A
 * 32-bit float depth is written as its bits; a 16-bit normalized one
 * raised a quarter of a step, so that the depth unit's conversion back
 * gives the same integer; a stencil value replaces the stencil (the
 * draw's stencil test replaces it by the reference).  With
 * AMDRDNA_QUAD_SAMPLE_ZERO in mode the image is a usampler2DMS and the
 * fetch reads its sample 0 (texelFetch(bits, ivec2(texcoord), 0) of a
 * multisampled image), its parameters x, y, a zero and the sample, as the
 * frontend passes a multisampled image's fetch.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/meta/radv_meta_blit2d.c
 * (build_nir_copy_fragment_shader_depth and _stencil: a texel fetched
 * and written as FRAG_RESULT_DEPTH or FRAG_RESULT_STENCIL) and
 * radv_meta_resolve_fs.c (build_depth_stencil_resolve_fragment_shader:
 * sample 0 of the source fetched for VK_RESOLVE_MODE_SAMPLE_ZERO_BIT and
 * written as the depth or the stencil).
 */
void
amdrdna_quad_depth_ir(
	struct amdrdna_quad_ir *program,
	uint32_t mode)
{
	struct drv_gpu_shader_ir_inst *list;
	struct drv_gpu_shader_ir *ir;
	uint32_t count;
	uint32_t texel;
	uint32_t next;
	bool sample_zero;

	/* Starts empty, the instructions appended in order. */
	kern_memset(program, 0, sizeof(*program));
	list = program->instructions;
	ir = &program->ir;
	count = 0U;
	sample_zero = (mode & AMDRDNA_QUAD_SAMPLE_ZERO) != 0U;
	mode &= ~AMDRDNA_QUAD_SAMPLE_ZERO;

	/*
	 * v0, v1 = the coordinate; v2, v3 = it as integers; the texel fetched
	 * with v4..v6 into v7..v10, or sample 0 with v4..v7 into v8..v11.
	 */
	quad_ir_load(list, &count, 0U, DRV_GPU_IR_LOAD_INPUT, 0U, 0U, 0U);
	quad_ir_load(list, &count, 1U, DRV_GPU_IR_LOAD_INPUT, 0U, 1U, 0U);
	quad_ir_two(list, &count, DRV_GPU_IR_F2I, 2U, 0U, 0U);
	quad_ir_two(list, &count, DRV_GPU_IR_F2I, 3U, 1U, 0U);
	if (sample_zero) {
		quad_ir_fetch_sample_zero(list, &count, 4U, 2U, 3U);
		texel = 4U + QUAD_FETCH_PARAMETERS + 1U;
	} else {
		quad_ir_fetch(list, &count, 4U, 2U, 3U);
		texel = 4U + QUAD_FETCH_PARAMETERS;
	}
	next = texel + 4U;
	ir->value_count = next;

	/* The texel's bits as the depth, raised and scaled for a 16-bit depth, or as the stencil reference. */
	if (mode == AMDRDNA_QUAD_DEPTH_32) {
		quad_ir_store(list, &count, DRV_GPU_IR_LOCATION_FRAG_DEPTH, 0U, texel);
	} else if (mode == AMDRDNA_QUAD_DEPTH_16) {
		quad_ir_two(list, &count, DRV_GPU_IR_U2F, next, texel, 0U);
		quad_ir_load(list, &count, next + 1U, DRV_GPU_IR_CONST, 0U, 0U, QUAD_FLOAT_QUARTER);
		quad_ir_two(list, &count, DRV_GPU_IR_FADD, next + 2U, next, next + 1U);
		quad_ir_load(list, &count, next + 3U, DRV_GPU_IR_CONST, 0U, 0U, QUAD_FLOAT_STEP_16);
		quad_ir_two(list, &count, DRV_GPU_IR_FMUL, next + 4U, next + 2U, next + 3U);
		quad_ir_store(list, &count, DRV_GPU_IR_LOCATION_FRAG_DEPTH, 0U, next + 4U);
		ir->value_count = next + 5U;
	} else {
		quad_ir_store(list, &count, DRV_GPU_IR_LOCATION_FRAG_STENCIL, 0U, texel);
	}

	/* The program: the coordinate in, the image read, no colour out. */
	quad_ir_texture_input(program);
	ir->instructions = list;
	ir->instruction_count = count;
}

/*
 * Builds the IR of a fragment program that packs the bits of texels texels
 * (1, 2 or 4, each of 32 / texels bits) side by side into the four bytes of
 * a colour target's pixel, as the frontend would parse
 *
 *   layout(set = 0, binding = 0) uniform usampler2D bits;
 *   layout(push_constant) uniform Q { vec4 rect; vec4 texture; uint x; } q;
 *   layout(location = 0) in vec2 texcoord;
 *   void main() {
 *       ivec2 p = ivec2(texcoord);
 *       uint word = 0;
 *       for (uint k = 0; k < texels; k++)
 *           word |= texelFetch(bits, ivec2(q.x + p.x * texels + k, p.y), 0).x << (32 / texels * k);
 *       for (uint i = 0; i < 4; i++)
 *           colour[i] = (float((word >> (8 * i)) & 0xff) + 0.25) / 255.0;
 *   }
 *
 * the quad's texture rectangle the pixels of the target in x (from 0) and
 * the source's rows in y; the push constants' colour word 0 the source's
 * first texel in x.  Each byte is written raised a quarter of a step, so
 * that the colour unit's conversion to an 8-bit normalized value gives the
 * byte whether it rounds or truncates.
 */
void
amdrdna_quad_pack_ir(
	struct amdrdna_quad_ir *program,
	uint32_t texels)
{
	struct drv_gpu_shader_ir_inst *list;
	struct drv_gpu_shader_ir *ir;
	uint32_t texel;
	uint32_t base;
	uint32_t word;
	uint32_t byte;
	uint32_t value;
	uint32_t count;

	/* Starts empty, the instructions appended in order. */
	kern_memset(program, 0, sizeof(*program));
	list = program->instructions;
	ir = &program->ir;
	count = 0U;

	/*
	 * v0, v1 = the coordinate; v2, v3 = it as integers; v4 = the source's
	 * first x, v5 = texels, v6 = the first texel's x; v7 = 0xff, v8 = a
	 * quarter, v9 = 1/255.
	 */
	quad_ir_load(list, &count, 0U, DRV_GPU_IR_LOAD_INPUT, 0U, 0U, 0U);
	quad_ir_load(list, &count, 1U, DRV_GPU_IR_LOAD_INPUT, 0U, 1U, 0U);
	quad_ir_two(list, &count, DRV_GPU_IR_F2I, 2U, 0U, 0U);
	quad_ir_two(list, &count, DRV_GPU_IR_F2I, 3U, 1U, 0U);
	quad_ir_load(list, &count, 4U, DRV_GPU_IR_LOAD_PUSH, 0U, 0U, AMDRDNA_QUAD_PUSH_COLOUR);
	quad_ir_load(list, &count, 5U, DRV_GPU_IR_ICONST, 0U, 0U, texels);
	quad_ir_two(list, &count, DRV_GPU_IR_IMUL, 10U, 2U, 5U);
	quad_ir_two(list, &count, DRV_GPU_IR_IADD, 6U, 10U, 4U);
	quad_ir_load(list, &count, 7U, DRV_GPU_IR_ICONST, 0U, 0U, 0xffU);
	quad_ir_load(list, &count, 8U, DRV_GPU_IR_CONST, 0U, 0U, QUAD_FLOAT_QUARTER);
	quad_ir_load(list, &count, 9U, DRV_GPU_IR_CONST, 0U, 0U, QUAD_FLOAT_STEP_8);

	/*
	 * Each texel k (twelve values from base): k, its x, the fetch's
	 * parameters and reply, its shift and its bits shifted; the word the
	 * texels' bits or-ed together.
	 */
	word = 0U;
	for (texel = 0U; texel < texels; texel++) {
		base = QUAD_PACK_FIRST + texel * QUAD_PACK_VALUES;
		quad_ir_load(list, &count, base, DRV_GPU_IR_ICONST, 0U, 0U, texel);
		quad_ir_two(list, &count, DRV_GPU_IR_IADD, base + 1U, 6U, base);
		quad_ir_fetch(list, &count, base + 2U, base + 1U, 3U);
		quad_ir_load(list, &count, base + 9U, DRV_GPU_IR_ICONST, 0U, 0U, texel * (32U / texels));
		quad_ir_two(list, &count, DRV_GPU_IR_SHL, base + 10U, base + 5U, base + 9U);
		if (texel == 0U) {
			word = base + 10U;
		} else {
			quad_ir_two(list, &count, DRV_GPU_IR_IOR, base + 11U, word, base + 10U);
			word = base + 11U;
		}
	}

	/* Each byte of the word (six values from the texels' end): its shift, the byte, its float raised and scaled, to colour 0. */
	value = QUAD_PACK_FIRST + texels * QUAD_PACK_VALUES;
	for (byte = 0U; byte < 4U; byte++) {
		base = value + byte * 6U;
		quad_ir_load(list, &count, base, DRV_GPU_IR_ICONST, 0U, 0U, byte * 8U);
		quad_ir_two(list, &count, DRV_GPU_IR_SHR, base + 1U, word, base);
		quad_ir_two(list, &count, DRV_GPU_IR_IAND, base + 2U, base + 1U, 7U);
		quad_ir_two(list, &count, DRV_GPU_IR_U2F, base + 3U, base + 2U, 0U);
		quad_ir_two(list, &count, DRV_GPU_IR_FADD, base + 4U, base + 3U, 8U);
		quad_ir_two(list, &count, DRV_GPU_IR_FMUL, base + 5U, base + 4U, 9U);
		quad_ir_store(list, &count, 0U, byte, base + 5U);
	}
	program->output.location = 0U;
	program->output.components = 4U;

	/* The program: the coordinate in, the image read, the push constants, colour 0 out. */
	quad_ir_texture_input(program);
	ir->outputs = &program->output;
	ir->output_count = 1U;
	ir->push_bytes = AMDRDNA_QUAD_PUSH_BYTES;
	ir->instructions = list;
	ir->instruction_count = count;
	ir->value_count = value + 4U * 6U;
}

/* Appends dst = op(first, second): an operation of one source (second unused) or two. */
static void
quad_ir_two(
	struct drv_gpu_shader_ir_inst *list,
	uint32_t *count,
	enum drv_gpu_shader_ir_op op,
	uint32_t dst,
	uint32_t first,
	uint32_t second)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The next instruction. */
	instruction = &list[(*count)++];
	instruction->op = op;
	instruction->dst = dst;
	instruction->src[0] = first;
	instruction->src[1] = second;
}

/*
 * Appends the fetch of the texel at the integers x and y, level 0, of the
 * image at binding 0: its three parameters copied together into base ..
 * base + 2, its four channels into base + 3 .. base + 6.
 */
static void
quad_ir_fetch(
	struct drv_gpu_shader_ir_inst *list,
	uint32_t *count,
	uint32_t base,
	uint32_t x,
	uint32_t y)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The parameters together: x, y, level 0. */
	quad_ir_move(list, count, base, x);
	quad_ir_move(list, count, base + 1U, y);
	quad_ir_load(list, count, base + 2U, DRV_GPU_IR_ICONST, 0U, 0U, 0U);

	/* The fetch. */
	instruction = &list[(*count)++];
	instruction->op = DRV_GPU_IR_TEXTURE;
	instruction->dst = base + QUAD_FETCH_PARAMETERS;
	instruction->src[0] = base;
	instruction->src[1] = QUAD_FETCH_PARAMETERS;
	instruction->location = 0U;
	instruction->immediate = 0U;
	instruction->component = DRV_GPU_IR_TEXTURE_LD;
	instruction->image_shape = DRV_GPU_IR_IMAGE_2D;
}

/*
 * Appends the fetch of sample 0 of the texel at the integers x and y of
 * the image of samples at binding 0: its four parameters (x, y, a zero
 * and the sample, 0) copied together into base .. base + 3, its four
 * channels into base + 4 .. base + 7.
 */
static void
quad_ir_fetch_sample_zero(
	struct drv_gpu_shader_ir_inst *list,
	uint32_t *count,
	uint32_t base,
	uint32_t x,
	uint32_t y)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The parameters together: x, y, a zero, sample 0. */
	quad_ir_move(list, count, base, x);
	quad_ir_move(list, count, base + 1U, y);
	quad_ir_load(list, count, base + 2U, DRV_GPU_IR_ICONST, 0U, 0U, 0U);
	quad_ir_load(list, count, base + 3U, DRV_GPU_IR_ICONST, 0U, 0U, 0U);

	/* The fetch of a multisampled image. */
	instruction = &list[(*count)++];
	instruction->op = DRV_GPU_IR_TEXTURE;
	instruction->dst = base + QUAD_FETCH_PARAMETERS + 1U;
	instruction->src[0] = base;
	instruction->src[1] = QUAD_FETCH_PARAMETERS + 1U;
	instruction->location = 0U;
	instruction->immediate = 0U;
	instruction->component = DRV_GPU_IR_TEXTURE_LD;
	instruction->image_shape = DRV_GPU_IR_IMAGE_2D | DRV_GPU_IR_IMAGE_MULTISAMPLED;
}

/* Makes a fragment program read the coordinate at location 0 and the image at binding 0. */
static void
quad_ir_texture_input(
	struct amdrdna_quad_ir *program)
{
	struct drv_gpu_shader_ir *ir;

	/* The coordinate's two channels, the sampled image of set 0's binding 0, a fragment program. */
	ir = &program->ir;
	program->input.location = 0U;
	program->input.components = 2U;
	program->uniform.set = 0U;
	program->uniform.binding = 0U;
	program->uniform.kind = DRV_GPU_IR_UNIFORM_SAMPLED_IMAGE;
	ir->stage = DRV_GPU_STAGE_FRAGMENT;
	ir->inputs = &program->input;
	ir->input_count = 1U;
	ir->uniforms = &program->uniform;
	ir->uniform_count = 1U;
}

/* Appends dst = src, a copy of a value. */
static void
quad_ir_move(
	struct drv_gpu_shader_ir_inst *list,
	uint32_t *count,
	uint32_t dst,
	uint32_t src)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The next instruction. */
	instruction = &list[(*count)++];
	instruction->op = DRV_GPU_IR_MOVE;
	instruction->dst = dst;
	instruction->src[0] = src;
}

/* Appends base = to - from, base + 1 = base * corner, base + 2 = from + base + 1: from mixed toward to by corner. */
static void
quad_ir_mix(
	struct drv_gpu_shader_ir_inst *list,
	uint32_t *count,
	uint32_t base,
	uint32_t corner,
	uint32_t from,
	uint32_t to)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The span. */
	instruction = &list[(*count)++];
	instruction->op = DRV_GPU_IR_FSUB;
	instruction->dst = base;
	instruction->src[0] = to;
	instruction->src[1] = from;

	/* The part of it the corner takes. */
	instruction = &list[(*count)++];
	instruction->op = DRV_GPU_IR_FMUL;
	instruction->dst = base + 1U;
	instruction->src[0] = base;
	instruction->src[1] = corner;

	/* From the start. */
	instruction = &list[(*count)++];
	instruction->op = DRV_GPU_IR_FADD;
	instruction->dst = base + 2U;
	instruction->src[0] = from;
	instruction->src[1] = base + 1U;
}

/* Appends an instruction that defines dst without sources: an input, a push constant word or a constant. */
static void
quad_ir_load(
	struct drv_gpu_shader_ir_inst *list,
	uint32_t *count,
	uint32_t dst,
	enum drv_gpu_shader_ir_op op,
	uint32_t location,
	uint32_t component,
	uint32_t immediate)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The next instruction. */
	instruction = &list[(*count)++];
	instruction->op = op;
	instruction->dst = dst;
	instruction->location = location;
	instruction->component = component;
	instruction->immediate = immediate;
}

/* Appends the store of a value to an output's component. */
static void
quad_ir_store(
	struct drv_gpu_shader_ir_inst *list,
	uint32_t *count,
	uint32_t location,
	uint32_t component,
	uint32_t value)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The next instruction. */
	instruction = &list[(*count)++];
	instruction->op = DRV_GPU_IR_STORE_OUTPUT;
	instruction->location = location;
	instruction->component = component;
	instruction->src[0] = value;
}
