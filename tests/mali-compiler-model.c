/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The one-lane model of Valhall and the IR reference of the Mali
 * compiler's host check.
 *
 * The reference runs the shared IR of a module for one invocation.  The
 * model runs the compiled program's words for one lane: each word is
 * named by the generated ISA table (the decoder of the host test) and
 * executed by that name; the fields are read where the ISA description
 * puts them.  The model also checks the program's waits: a register a
 * message writes may not be read or written before the program waits on
 * the message's slot, ATEST and ZS_EMIT need the wait0126 flow before
 * them and BLEND the wait flow; ZS_EMIT and BLEND come after ATEST (and
 * BLEND after ZS_EMIT, the order Mesa's lowering writes) and read the
 * coverage ATEST wrote.  Both sides get the same random attributes, varyings,
 * push constants, buffers and textures; the outputs must agree.
 *
 * The model's arithmetic is that of the host's floats.  FREXPE, FLOGD,
 * FADD_LSCALE and FEXP are modelled as the logarithm and exponent
 * sequences the compiler writes expect them (an inference, not a
 * documented behaviour).  A fragment's preloaded coverage (r60) is
 * modelled as the rasterizer's coverage (bits 15:0 of r61, under the
 * sample number in bits 23:16) less its lowest sample when it has more
 * than one, so reading one for the other shows; ZS_EMIT writes the depth
 * of its first source (with its z bit) and the stencil reference of its
 * second (with its stencil bit) and passes its coverage through, as ATEST
 * does (an inference from Mesa's lowering, not a documented behaviour).
 * The sample number in r61 has garbage above its five bits.
 *
 * The model's texture reply is a fixed function of what the sample is
 * given, not a filter: a level or bias (the 8.8 fixed point value in the
 * low half of its staging register) adds a 1/32 of itself to the second
 * channel, and a gradient descriptor adds a weighted sum of the
 * derivatives TEX_GRADIENT was given to the first and third.  The
 * descriptor's two words are the model's own (the sum, and a token naming
 * the texture and the dimension), not the hardware's.  The reference
 * gives the same functions their values from the IR: the level and bias
 * as Mesa packs them (within 16 levels either way), the derivatives, and
 * a cube's level from the derivatives by Mesa's lower_gradient_cube_map().
 * Nothing here is evidence about the GPU.
 */

#include "tests/mali-compiler-model.h"

#include "drivers/gpu/compiler/ir.h"
#include "drivers/gpu/compiler/spirv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One instruction of the generated ISA table (as in the host test). */
struct isa_entry {
	const char *name;
	uint64_t mask;
	uint64_t value;
	uint32_t sources;
	uint8_t source_flags[4];
	uint16_t source_sizes[4];
	uint32_t dests;
	uint32_t staging_read;
	uint32_t staging_write;
	uint32_t message;
};

static const struct isa_entry model_table[] = {
#include "tests/mali-compiler-isa-table.inc"
};

static const uint32_t model_immediates[32] = ISA_TABLE_IMMEDIATES;

#define WORDS		1024U
#define BUFFERS		MALI_SHADER_BUFFERS
#define OUTPUT_BASE	0x7000000000ULL
#define BUFFER_BASE	0x10000000000ULL
#define SHARED_BASE	0x2000000000ULL
#define OUTPUT_INDEX	0x40U
#define MAX_STEPS	200000U
#define TLS_BASE	0x3000000000ULL
#define TLS_WORDS	4096U
#define IMAGE_BASE	0x40000000000ULL
#define IMAGES		4U
#define IMAGE_TEXELS	4096U
#define CVT_TOKEN	0xC7000000U
#define GRDESC_TOKEN	0x6D000000U
#define OFFSETS_NONE	0xFFFFFFFFU

/* The inputs of one trial, shared by the reference and the model. */
struct trial {
	uint32_t attributes[16][4];
	uint32_t varyings[16][4];
	uint32_t push[64];
	uint32_t viewport[6];
	uint32_t buffers[BUFFERS][WORDS];
	uint32_t shared[WORDS];
	uint32_t local_id[3];
	uint32_t group_id[3];
	uint32_t buffer_bytes[BUFFERS];
	uint32_t texture_size[16][4];
	uint32_t vertex_id;
	uint32_t instance_id;
	uint32_t frag_x;
	uint32_t frag_y;
	uint32_t raster_coverage;
	uint32_t coverage;
	uint32_t sample_id;
	uint32_t images[IMAGES][IMAGE_TEXELS * 4U];
};

/* What one side produced. */
struct result {
	uint32_t position[4];
	uint32_t varyings[16][4];
	uint32_t colors[8][4];
	uint32_t color_written;
	uint32_t depth;
	uint32_t depth_written;
	uint32_t sample_mask;
	uint32_t sample_mask_written;
	uint32_t stencil;
	uint32_t stencil_written;
	uint32_t coverage;
	uint32_t coverage_tested;
	int killed;
	uint32_t buffers[BUFFERS][WORDS];
	uint32_t images[IMAGES][IMAGE_TEXELS * 4U];
	const char *why;
	uint32_t why_at;
};

static uint32_t random_state;

static uint32_t next_random(void);
static float as_float(uint32_t bits);
static uint32_t as_bits(float value);
static uint32_t to_half(float value);
static float from_half(uint32_t half);
static void texture_value(uint32_t slot, float u, float v, uint32_t *out);
static void texel_value(uint32_t slot, float s, float t, float r, uint32_t layer, uint32_t face, uint32_t *out);
static uint32_t cube_face(float x, float y, float z);
static float cube_select_s(float z, float x, uint32_t face);
static float cube_select_t(float y, float z, uint32_t face);
static void message_reply(const uint32_t *texel, int gather, uint32_t channel, int compare, float reference, uint32_t *out);
static int32_t level_fixed(float level);
static void apply_level(uint32_t *texel, int32_t fixed);
static float gradient_sum(const float *derivatives, uint32_t count);
static void apply_gradient(uint32_t *texel, float sum);
static void apply_offset(uint32_t *texel, int32_t s, int32_t t, int32_t r);
static int32_t signed_bits(uint32_t value, uint32_t bits);
static void apply_ir_offset(uint32_t *texel, uint32_t offset);
static float cube_gradient_level(const float *p, const float *dpdx, const float *dpdy, uint32_t width);
static uint32_t image_texel(uint32_t x, uint32_t y, uint32_t layer);
static uint32_t format_components(uint32_t format);
static int format_is_integer(uint32_t format);
static int find_image(const struct mali_shader_program *program, uint32_t set, uint32_t binding);
static uint32_t sampled_layer(float coordinate);
static void reference_texture(const struct mali_shader_program *program, const struct trial *trial, const uint32_t *value, const struct drv_gpu_shader_ir_inst *inst, uint32_t *texel);
static void reference_image(const struct drv_gpu_shader_ir *ir, const struct mali_shader_program *program, uint32_t *value, const struct drv_gpu_shader_ir_inst *inst, struct result *result);
static int find_buffer(const struct mali_shader_program *program, const struct drv_gpu_shader_ir *ir, uint32_t uniform);
static int find_texture(const struct mali_shader_program *program, uint32_t set, uint32_t binding);
static void run_reference(const struct drv_gpu_shader_ir *ir, const struct mali_shader_program *program, const struct trial *trial, struct result *result);
static void run_model(const uint8_t *code, size_t length, uint32_t stage, const struct mali_shader_program *program, const struct trial *trial, struct result *result);
static int close_enough(uint32_t a, uint32_t b);
static int check_load(const char *name, uint64_t word, uint32_t count, uint64_t address);

/* A small linear congruential generator, the same for every run. */
static uint32_t
next_random(void)
{
	random_state = random_state * 1664525U + 1013904223U;
	return random_state;
}

static float
as_float(
	uint32_t bits)
{
	float value;

	memcpy(&value, &bits, 4U);
	return value;
}

static uint32_t
as_bits(
	float value)
{
	uint32_t bits;

	memcpy(&bits, &value, 4U);
	return bits;
}

/* Rounds a float to the nearest 16-bit float (ties to even). */
static uint32_t
to_half(
	float value)
{
	uint32_t bits;
	uint32_t sign;
	int exponent;
	uint32_t mantissa;
	uint32_t half;
	uint32_t round;

	bits = as_bits(value);
	sign = (bits >> 16) & 0x8000U;
	exponent = (int)((bits >> 23) & 0xFFU) - 127 + 15;
	mantissa = bits & 0x7FFFFFU;
	if (((bits >> 23) & 0xFFU) == 0xFFU)
		return sign | 0x7C00U | (mantissa != 0U ? 0x200U : 0U);
	if (exponent >= 31)
		return sign | 0x7C00U;
	if (exponent <= 0) {
		if (exponent < -10)
			return sign;
		mantissa |= 0x800000U;
		round = (uint32_t)(14 - exponent);
		half = mantissa >> round;
		if ((mantissa >> (round - 1U)) & 1U) {
			if ((mantissa & ((1U << (round - 1U)) - 1U)) != 0U || (half & 1U) != 0U)
				half++;
		}
		return sign | half;
	}
	half = sign | ((uint32_t)exponent << 10) | (mantissa >> 13);
	if ((mantissa & 0x1000U) != 0U && ((mantissa & 0xFFFU) != 0U || (half & 1U) != 0U))
		half++;
	return half;
}

static float
from_half(
	uint32_t half)
{
	uint32_t sign;
	uint32_t exponent;
	uint32_t mantissa;
	float value;

	sign = (half >> 15) & 1U;
	exponent = (half >> 10) & 0x1FU;
	mantissa = half & 0x3FFU;
	if (exponent == 0U) {
		value = ldexpf((float)mantissa, -24);
	} else if (exponent == 31U) {
		value = mantissa != 0U ? NAN : INFINITY;
	} else {
		value = ldexpf((float)(mantissa | 0x400U), (int)exponent - 25);
	}
	return sign != 0U ? -value : value;
}

/* The texel both sides return for a texture slot at (u, v). */
static void
texture_value(
	uint32_t slot,
	float u,
	float v,
	uint32_t *out)
{
	out[0] = as_bits(u * 0.5f + (float)slot * 0.125f);
	out[1] = as_bits(v * 0.25f - 0.5f);
	out[2] = as_bits(u * v);
	out[3] = as_bits(1.0f - u * 0.125f);
}

/*
 * The texel both sides return for a texture slot at (s, t), of a 3D
 * image's r, an array's layer and a cube's face: each adds to one
 * channel, so a misplaced coordinate shows.
 */
static void
texel_value(
	uint32_t slot,
	float s,
	float t,
	float r,
	uint32_t layer,
	uint32_t face,
	uint32_t *out)
{
	texture_value(slot, s, t, out);
	if (r != 0.0f)
		out[1] = as_bits(as_float(out[1]) + r * 0.375f);
	if (layer != 0U)
		out[0] = as_bits(as_float(out[0]) + (float)layer * 0.0625f);
	if (face != 0U)
		out[3] = as_bits(as_float(out[3]) + (float)face * 0.125f);
}

/*
 * The face of a cube's direction (+X 0, -X 1, +Y 2, -Y 3, +Z 4, -Z 5),
 * x winning ties over y and y over z: the model's CUBEFACE2 and the
 * reference agree on it (the hardware's choice in a tie is not known).
 */
static uint32_t
cube_face(
	float x,
	float y,
	float z)
{
	if (fabsf(x) >= fabsf(y) && fabsf(x) >= fabsf(z))
		return x < 0.0f ? 1U : 0U;
	if (fabsf(y) >= fabsf(z))
		return y < 0.0f ? 3U : 2U;
	return z < 0.0f ? 5U : 4U;
}

/* A face's s coordinate before the division by the major axis (Vulkan's cube face table). */
static float
cube_select_s(
	float z,
	float x,
	uint32_t face)
{
	switch (face) {
	case 0U: return -z;
	case 1U: return z;
	case 5U: return -x;
	default: return x;
	}
}

/* A face's t coordinate before the division by the major axis. */
static float
cube_select_t(
	float y,
	float z,
	uint32_t face)
{
	switch (face) {
	case 2U: return z;
	case 3U: return -z;
	default: return -y;
	}
}

/*
 * A message's reply from its texel: a gather's four values of its channel
 * (the texel's, then offsets of 0.25 standing for the other three
 * texels), a compare's results of reference <= value (the sampler's
 * compare function, which both sides take as less or equal).
 */
static void
message_reply(
	const uint32_t *texel,
	int gather,
	uint32_t channel,
	int compare,
	float reference,
	uint32_t *out)
{
	uint32_t index;
	float value;

	for (index = 0U; index < 4U; index++) {
		out[index] = texel[index];
		if (gather)
			out[index] = as_bits(as_float(texel[compare ? 0U : channel & 3U]) + 0.25f * (float)index);
		if (compare && (gather || index == 0U)) {
			value = as_float(out[index]);
			out[index] = as_bits(reference <= value ? 1.0f : 0.0f);
		}
	}
}

/*
 * A level or bias as the sampler takes it: 8.8 fixed point, kept within
 * 16 levels either way.
 *
 * Procedure referenced: Mesa 26.2.6
 * src/panfrost/compiler/pan_nir_lower_tex.c (build_sfixed_8_8).
 */
static int32_t
level_fixed(
	float level)
{
	level = fminf(fmaxf(level / 16.0f, -1.0f), 1.0f);
	return (int32_t)rintf(level * 16.0f * 256.0f);
}

/* Adds a level or bias (8.8 fixed point) to a texel: a 1/32 of it to the second channel. */
static void
apply_level(
	uint32_t *texel,
	int32_t fixed)
{
	texel[1] = as_bits(as_float(texel[1]) + (float)fixed / 8192.0f);
}

/* The model's gradient: the derivatives (dx, dy of each coordinate in turn) weighed by their places. */
static float
gradient_sum(
	const float *derivatives,
	uint32_t count)
{
	uint32_t index;
	float sum;

	sum = 0.0f;
	for (index = 0U; index < count; index++)
		sum += derivatives[index] * (float)(index + 1U) * 0.0625f;
	return sum;
}

/* Adds a gradient to a texel: half of it to the first channel, all of it to the third. */
static void
apply_gradient(
	uint32_t *texel,
	float sum)
{
	texel[0] = as_bits(as_float(texel[0]) + sum * 0.5f);
	texel[2] = as_bits(as_float(texel[2]) + sum);
}

/*
 * Adds a constant texel offset (s, t, r) to a texel: each weighed by its
 * place, to the fourth channel, so a lost, swapped or wrongly signed
 * component shows.
 */
static void
apply_offset(
	uint32_t *texel,
	int32_t s,
	int32_t t,
	int32_t r)
{
	texel[3] = as_bits(as_float(texel[3]) + (float)(s + 16 * t + 256 * r) / 4096.0f);
}

/* Gives the low bits of a value as a signed number. */
static int32_t
signed_bits(
	uint32_t value,
	uint32_t bits)
{
	uint32_t sign;

	value &= (1U << bits) - 1U;
	sign = 1U << (bits - 1U);
	return (int32_t)(value ^ sign) - (int32_t)sign;
}

/* Adds the IR's constant texel offset (u in bits 11:8, v in 7:4, r in 3:0) to a texel. */
static void
apply_ir_offset(
	uint32_t *texel,
	uint32_t offset)
{
	apply_offset(texel, signed_bits(offset >> 8, 4U), signed_bits(offset >> 4, 4U), signed_bits(offset, 4U));
}

/*
 * The level of a cube sample with derivatives, from the direction p, its
 * derivatives and the cube's width: the major axis third (z winning a tie
 * over y, y over x), the face coordinates' derivatives by the quotient
 * rule, and -1 + log2(L * L * max(|dx|^2, |dy|^2)) / 2.
 *
 * Procedure referenced: Mesa 26.2.6 src/compiler/nir/nir_lower_tex.c
 * (lower_gradient_cube_map).
 */
static float
cube_gradient_level(
	const float *p,
	const float *dpdx,
	const float *dpdy,
	uint32_t width)
{
	static const uint32_t order[3][3] = { { 1U, 2U, 0U }, { 0U, 2U, 1U }, { 0U, 1U, 2U } };
	const uint32_t *swizzle;
	float q[3];
	float qx[3];
	float qy[3];
	float recip;
	float dx[2];
	float dy[2];
	float m;
	float size;
	uint32_t axis;

	/* Selects the major axis. */
	swizzle = order[0];
	if (fabsf(p[2]) >= fmaxf(fabsf(p[0]), fabsf(p[1])))
		swizzle = order[2];
	else if (fabsf(p[1]) >= fmaxf(fabsf(p[0]), fabsf(p[2])))
		swizzle = order[1];

	/* Reorders the direction and its derivatives so the major axis is third. */
	for (axis = 0U; axis < 3U; axis++) {
		q[axis] = p[swizzle[axis]];
		qx[axis] = dpdx[swizzle[axis]];
		qy[axis] = dpdy[swizzle[axis]];
	}

	/* The quotient rule. */
	recip = 1.0f / q[2];
	for (axis = 0U; axis < 2U; axis++) {
		dx[axis] = recip * (qx[axis] - q[axis] * recip * qx[2]);
		dy[axis] = recip * (qy[axis] - q[axis] * recip * qy[2]);
	}

	/* The level. */
	m = fmaxf(dx[0] * dx[0] + dx[1] * dx[1], dy[0] * dy[0] + dy[1] * dy[1]);
	size = (float)width;
	return -1.0f + 0.5f * log2f(size * size * m);
}

/* A storage image's texel number in the model's memory. */
static uint32_t
image_texel(
	uint32_t x,
	uint32_t y,
	uint32_t layer)
{
	return (x & 15U) + 16U * (y & 15U) + 256U * (layer & 15U);
}

/* The components of a storage image format (VkFormat). */
static uint32_t
format_components(
	uint32_t format)
{
	if (format == 98U || format == 99U || format == 100U)
		return 1U;
	if (format == 101U || format == 102U || format == 103U)
		return 2U;
	return 4U;
}

/* Whether a storage image format holds integers. */
static int
format_is_integer(
	uint32_t format)
{
	switch (format) {
	case 41U: case 42U: case 95U: case 96U: case 98U: case 99U: case 101U: case 102U: case 107U: case 108U:
		return 1;
	default:
		return 0;
	}
}

static int
find_image(
	const struct mali_shader_program *program,
	uint32_t set,
	uint32_t binding)
{
	uint32_t index;

	for (index = 0U; index < program->image_count; index++) {
		if (program->images[index].set == set && program->images[index].binding == binding)
			return (int)index;
	}
	return -1;
}

static int
find_buffer(
	const struct mali_shader_program *program,
	const struct drv_gpu_shader_ir *ir,
	uint32_t uniform)
{
	uint32_t index;

	if (uniform >= ir->uniform_count)
		return -1;
	for (index = 0U; index < program->buffer_count; index++) {
		if (program->buffers[index].set == ir->uniforms[uniform].set &&
		    program->buffers[index].binding == ir->uniforms[uniform].binding)
			return (int)index;
	}
	return -1;
}

static int
find_texture(
	const struct mali_shader_program *program,
	uint32_t set,
	uint32_t binding)
{
	uint32_t index;

	for (index = 0U; index < program->texture_count; index++) {
		if (program->textures[index].set == set && program->textures[index].binding == binding)
			return (int)index;
	}
	return -1;
}

static uint32_t
float_to_int(
	float value,
	int is_signed)
{
	if (isnan(value))
		return 0U;
	value = truncf(value);
	if (is_signed) {
		if (value >= 2147483647.0f)
			return 0x7FFFFFFFU;
		if (value <= -2147483648.0f)
			return 0x80000000U;
		return (uint32_t)(int32_t)value;
	}
	if (value <= 0.0f)
		return 0U;
	if (value >= 4294967295.0f)
		return 0xFFFFFFFFU;
	return (uint32_t)value;
}

/* The sampled array layer of a float coordinate: the nearest integer, at least 0. */
static uint32_t
sampled_layer(
	float coordinate)
{
	coordinate = rintf(coordinate);
	if (!(coordinate > 0.0f))
		return 0U;
	return (uint32_t)coordinate;
}

/*
 * The reference's sampler message: the parameters read in the order the
 * IR states (the reference of a compare, the bias or level, the
 * coordinates, each followed by its two derivatives in a sample with
 * them, the layer; a texel load's u v lod r), the cube's face and
 * coordinates by Vulkan's face selection, the level or bias and the
 * gradient as apply_level() and apply_gradient() (a cube's derivatives
 * as the level of cube_gradient_level()), the reply as message_reply().
 */
static void
reference_texture(
	const struct mali_shader_program *program,
	const struct trial *trial,
	const uint32_t *value,
	const struct drv_gpu_shader_ir_inst *inst,
	uint32_t *texel)
{
	uint32_t base[4];
	uint32_t message;
	uint32_t kind;
	uint32_t first;
	uint32_t stride;
	uint32_t coordinates;
	uint32_t lod_slot;
	uint32_t component;
	uint32_t word;
	uint32_t layer;
	uint32_t face;
	int slot;
	int compare;
	int gather;
	int arrayed;
	float s;
	float t;
	float r;
	float x;
	float y;
	float z;
	float major;
	float reference;
	float derivatives[6];
	float direction[3];
	float dpdx[3];
	float dpdy[3];
	int gradient;

	/* The SAMPLE operations: u, v and the bias or level after them. */
	slot = find_texture(program, inst->location, inst->immediate);
	if (inst->op != DRV_GPU_IR_TEXTURE) {
		texture_value((uint32_t)slot, as_float(value[inst->src[0]]), as_float(value[inst->src[1]]), texel);
		apply_ir_offset(texel, inst->component & 0xFFFU);
		if (inst->op == DRV_GPU_IR_SAMPLE_BIAS || inst->op == DRV_GPU_IR_SAMPLE_LOD)
			apply_level(texel, level_fixed(as_float(value[inst->src[2]])));
		return;
	}
	message = inst->component & DRV_GPU_IR_TEXTURE_MESSAGE_MASK;
	kind = inst->image_shape & DRV_GPU_IR_IMAGE_KIND_MASK;
	arrayed = (inst->image_shape & DRV_GPU_IR_IMAGE_ARRAYED) != 0U;
	if (message == DRV_GPU_IR_TEXTURE_RESINFO) {
		if (slot < 0) {
			slot = find_image(program, inst->location, inst->immediate);
			if (slot >= 0)
				slot += 8;
		}
		for (component = 0U; component < 4U; component++) {
			word = trial->texture_size[slot & 15][component];
			if (component < 2U || (component == 2U && kind == DRV_GPU_IR_IMAGE_3D)) {
				word >>= value[inst->src[0]] & 31U;
				if (word == 0U)
					word = 1U;
			}
			texel[component] = word;
		}
		return;
	}
	if (message == DRV_GPU_IR_TEXTURE_LD) {
		r = 0.0f;
		layer = 0U;
		if (kind == DRV_GPU_IR_IMAGE_3D && inst->src[1] > 3U)
			r = (float)(int32_t)value[inst->src[0] + 3U];
		if (arrayed && inst->src[1] > 3U)
			layer = value[inst->src[0] + 3U];
		texel_value((uint32_t)slot, (float)(int32_t)value[inst->src[0]], (float)(int32_t)value[inst->src[0] + 1U], r, layer, 0U, texel);
		apply_ir_offset(texel, (inst->component >> 8) & 0xFFFU);
		return;
	}
	compare = message == DRV_GPU_IR_TEXTURE_SAMPLE_COMPARE || message == DRV_GPU_IR_TEXTURE_SAMPLE_BIAS_COMPARE ||
		  message == DRV_GPU_IR_TEXTURE_SAMPLE_LOD_COMPARE || message == DRV_GPU_IR_TEXTURE_GATHER4_COMPARE;
	gather = message == DRV_GPU_IR_TEXTURE_GATHER4 || message == DRV_GPU_IR_TEXTURE_GATHER4_COMPARE;
	gradient = message == DRV_GPU_IR_TEXTURE_SAMPLE_DERIVS || message == DRV_GPU_IR_TEXTURE_SAMPLE_DERIV_COMPARE;

	/* A sample with derivatives may compare too. */
	if (message == DRV_GPU_IR_TEXTURE_SAMPLE_DERIV_COMPARE)
		compare = 1;

	/* The places of the reference, the level or bias, and the first coordinate. */
	first = 0U;
	if (compare)
		first++;
	lod_slot = DRV_GPU_IR_TEXTURE_MAX_PARAMS;
	if (message == DRV_GPU_IR_TEXTURE_SAMPLE_BIAS || message == DRV_GPU_IR_TEXTURE_SAMPLE_LOD ||
	    message == DRV_GPU_IR_TEXTURE_SAMPLE_BIAS_COMPARE || message == DRV_GPU_IR_TEXTURE_SAMPLE_LOD_COMPARE) {
		lod_slot = first;
		first++;
	}

	/* A coordinate takes three parameters with its derivatives; a cube and a 3D image have three coordinates. */
	stride = 1U;
	if (gradient)
		stride = 3U;
	coordinates = 2U;
	if (kind == DRV_GPU_IR_IMAGE_CUBE || kind == DRV_GPU_IR_IMAGE_3D)
		coordinates = 3U;

	/* Reads the coordinates and the layer, and makes the texel. */
	reference = compare ? as_float(value[inst->src[0]]) : 0.0f;
#define PARAM(N) ((N) < inst->src[1] ? as_float(value[inst->src[0] + (N)]) : 0.0f)
	s = PARAM(first);
	t = PARAM(first + stride);
	r = 0.0f;
	face = 0U;
	layer = 0U;
	if (kind == DRV_GPU_IR_IMAGE_CUBE) {
		x = s;
		y = t;
		z = PARAM(first + 2U * stride);
		face = cube_face(x, y, z);
		major = fmaxf(fabsf(x), fmaxf(fabsf(y), fabsf(z)));
		s = fminf(fmaxf(cube_select_s(z, x, face) * (0.5f / major) + 0.5f, 0.0f), 1.0f);
		t = fminf(fmaxf(cube_select_t(y, z, face) * (0.5f / major) + 0.5f, 0.0f), 1.0f);
	} else if (kind == DRV_GPU_IR_IMAGE_3D) {
		r = PARAM(first + 2U * stride);
	}

	/* An array's layer follows the coordinates. */
	if (arrayed)
		layer = sampled_layer(PARAM(first + coordinates * stride));
	texel_value((uint32_t)slot, s, t, r, layer, face, base);
	apply_ir_offset(base, (inst->component >> 8) & 0xFFFU);

	/* A level or bias changes the texel. */
	if (lod_slot != DRV_GPU_IR_TEXTURE_MAX_PARAMS)
		apply_level(base, level_fixed(PARAM(lod_slot)));

	/* So do the derivatives (a cube's as its level). */
	if (gradient) {
		/* Reads each coordinate's x and y derivatives. */
		for (component = 0U; component < coordinates; component++) {
			direction[component] = PARAM(first + component * stride);
			dpdx[component] = PARAM(first + component * stride + 1U);
			dpdy[component] = PARAM(first + component * stride + 2U);
			derivatives[2U * component] = dpdx[component];
			derivatives[2U * component + 1U] = dpdy[component];
		}

		/* A cube's level is computed; the others' derivatives are summed. */
		if (kind == DRV_GPU_IR_IMAGE_CUBE)
			apply_level(base, level_fixed(cube_gradient_level(direction, dpdx, dpdy, trial->texture_size[slot & 15][0])));
		else
			apply_gradient(base, gradient_sum(derivatives, 2U * coordinates));
	}
#undef PARAM
	message_reply(base, gather, DRV_GPU_IR_TEXTURE_CHANNEL(inst->component), compare, reference, texel);
}

/*
 * The reference's storage image access: the texel at x, y and the layer
 * (the third coordinate) holds four raw words; a read gives the format's
 * components and (0, 0, 0, 1) for the rest, a write stores the format's
 * components.
 */
static void
reference_image(
	const struct drv_gpu_shader_ir *ir,
	const struct mali_shader_program *program,
	uint32_t *value,
	const struct drv_gpu_shader_ir_inst *inst,
	struct result *result)
{
	const struct drv_gpu_shader_ir_uniform *uniform;
	uint32_t texel;
	uint32_t components;
	uint32_t component;
	uint32_t one;
	int slot;

	if (inst->location >= ir->uniform_count) {
		result->why = "reference: an image without its uniform";
		return;
	}
	uniform = &ir->uniforms[inst->location];
	slot = find_image(program, uniform->set, uniform->binding);
	if (slot < 0 || slot >= (int)IMAGES) {
		result->why = "reference: an image the model does not have";
		return;
	}
	texel = image_texel(value[inst->src[0]], value[inst->src[0] + 1U], value[inst->src[0] + 2U]) * 4U;
	components = format_components(uniform->format);
	if (inst->op == DRV_GPU_IR_IMAGE_READ) {
		if (inst->component == 1U && value[inst->src[2]] == 0U)
			return;
		one = format_is_integer(uniform->format) ? 1U : as_bits(1.0f);
		for (component = 0U; component < 4U; component++) {
			if (component < components)
				value[inst->dst + component] = result->images[slot][texel + component];
			else
				value[inst->dst + component] = component == 3U ? one : 0U;
		}
		return;
	}
	if (inst->component == 1U && value[inst->src[3]] == 0U)
		return;
	for (component = 0U; component < components; component++)
		result->images[slot][texel + component] = value[inst->src[2] + component];
}

/* Runs the IR for one invocation. */
static void
run_reference(
	const struct drv_gpu_shader_ir *ir,
	const struct mali_shader_program *program,
	const struct trial *trial,
	struct result *result)
{
	const struct drv_gpu_shader_ir_inst *inst;
	uint32_t *value;
	uint32_t loop_stack[16];
	uint32_t depth;
	uint32_t index;
	uint32_t steps;
	uint32_t a;
	uint32_t b;
	uint32_t texel[4];
	int slot;
	float fa;
	float fb;
	uint32_t component;

	value = calloc((size_t)ir->value_count + 8U, sizeof(*value));
	if (value == NULL) {
		result->why = "out of memory";
		return;
	}
	depth = 0U;
	steps = 0U;
	for (index = 0U; index < ir->instruction_count && steps < MAX_STEPS; index++, steps++) {
		inst = &ir->instructions[index];
		a = inst->src[0] <= ir->value_count ? value[inst->src[0]] : 0U;
		b = inst->src[1] <= ir->value_count ? value[inst->src[1]] : 0U;
		fa = as_float(a);
		fb = as_float(b);
#define DST(V) do { if (inst->dst <= ir->value_count) value[inst->dst] = (V); } while (0)
#define FDST(V) DST(as_bits(V))
#define BOOLV(C) ((C) ? 0xFFFFFFFFU : 0U)
		switch (inst->op) {
		case DRV_GPU_IR_NOP:
		case DRV_GPU_IR_SKIP_BEGIN:
		case DRV_GPU_IR_SKIP_END:
		case DRV_GPU_IR_FENCE:
		case DRV_GPU_IR_BARRIER:
			break;
		case DRV_GPU_IR_CONST:
		case DRV_GPU_IR_ICONST:
		case DRV_GPU_IR_BOOL:
			DST(inst->immediate);
			break;
		case DRV_GPU_IR_LOAD_INPUT:
			if (inst->location < 16U && ir->stage == DRV_GPU_STAGE_VERTEX) {
				DST(trial->attributes[inst->location][inst->component & 3U]);
			} else if (inst->location < 16U) {
				DST(trial->varyings[inst->location][inst->component & 3U]);
			} else if (inst->location == DRV_GPU_SHADER_LOCATION_VERTEX_INDEX) {
				DST(trial->vertex_id);
			} else if (inst->location == DRV_GPU_SHADER_LOCATION_INSTANCE_INDEX) {
				DST(trial->instance_id);
			} else if (inst->location == DRV_GPU_SHADER_LOCATION_FRONT_FACING) {
				DST(0xFFFFFFFFU);
			} else if (inst->location == DRV_GPU_SHADER_LOCATION_SAMPLE_MASK) {
				DST(trial->raster_coverage);
			} else if (inst->location == DRV_GPU_SHADER_LOCATION_SAMPLE_ID) {
				DST(trial->sample_id);
			} else if (inst->location == DRV_GPU_SHADER_LOCATION_FRAG_COORD) {
				if (inst->component == 0U)
					FDST((float)trial->frag_x + 0.5f);
				else if (inst->component == 1U)
					FDST((float)trial->frag_y + 0.5f);
				else if (inst->component == 2U)
					FDST(0.5f);
				else
					FDST(1.0f);
			} else {
				result->why = "reference: an input it does not model";
			}
			break;
		case DRV_GPU_IR_STORE_OUTPUT:
			if (inst->location == DRV_GPU_IR_LOCATION_POSITION && inst->component < 4U)
				result->position[inst->component] = a;
			else if (inst->location == DRV_GPU_IR_LOCATION_FRAG_DEPTH) {
				result->depth = a;
				result->depth_written = 1U;
			} else if (inst->location == DRV_GPU_IR_LOCATION_SAMPLE_MASK) {
				result->sample_mask = a;
				result->sample_mask_written = 1U;
			} else if (inst->location == DRV_GPU_IR_LOCATION_FRAG_STENCIL) {
				result->stencil = a;
				result->stencil_written = 1U;
			} else if (inst->location < 16U && ir->stage == DRV_GPU_STAGE_VERTEX)
				result->varyings[inst->location][inst->component & 3U] = a;
			else if (inst->location < 8U && ir->stage == DRV_GPU_STAGE_FRAGMENT) {
				result->colors[inst->location][inst->component & 3U] = a;
				result->color_written |= 1U << inst->location;
			}
			break;
		case DRV_GPU_IR_LOAD_PUSH:
			DST(trial->push[(inst->immediate / 4U) & 63U]);
			break;
		case DRV_GPU_IR_LOAD_UBO:
			slot = find_buffer(program, ir, inst->location);
			DST(slot < 0 ? 0U : trial->buffers[slot][(inst->immediate / 4U) % WORDS]);
			break;
		case DRV_GPU_IR_STORAGE_SIZE:
			slot = find_buffer(program, ir, inst->location);
			DST(slot < 0 ? 0U : trial->buffer_bytes[slot]);
			break;
		case DRV_GPU_IR_LOAD_STORAGE:
			slot = find_buffer(program, ir, inst->location);
			if (slot >= 0 && (inst->component != 1U || b != 0U))
				DST(result->buffers[slot][(a / 4U) % WORDS]);
			break;
		case DRV_GPU_IR_STORE_STORAGE:
			slot = find_buffer(program, ir, inst->location);
			if (slot >= 0 && (inst->component != 1U || value[inst->src[2]] != 0U))
				result->buffers[slot][(a / 4U) % WORDS] = b;
			break;
		case DRV_GPU_IR_FADD: FDST(fa + fb); break;
		case DRV_GPU_IR_FSUB: FDST(fa - fb); break;
		case DRV_GPU_IR_FMUL: FDST(fa * fb); break;
		case DRV_GPU_IR_FNEG: FDST(-fa); break;
		case DRV_GPU_IR_RSQ: FDST(1.0f / sqrtf(fa)); break;
		case DRV_GPU_IR_RCP: FDST(1.0f / fa); break;
		case DRV_GPU_IR_SQRT: FDST(sqrtf(fa)); break;
		case DRV_GPU_IR_EXP2: FDST(exp2f(fa)); break;
		case DRV_GPU_IR_SIN: FDST(sinf(fa)); break;
		case DRV_GPU_IR_COS: FDST(cosf(fa)); break;
		case DRV_GPU_IR_DDX: case DRV_GPU_IR_DDX_FINE: case DRV_GPU_IR_DDY: case DRV_GPU_IR_DDY_FINE:
			/* One invocation is a quad whose four pixels agree. */
			FDST(0.0f);
			break;
		case DRV_GPU_IR_LOG2: FDST(log2f(fa)); break;
		case DRV_GPU_IR_FABS: FDST(fabsf(fa)); break;
		case DRV_GPU_IR_FLOOR: FDST(floorf(fa)); break;
		case DRV_GPU_IR_FRACT: FDST(fa - floorf(fa)); break;
		case DRV_GPU_IR_FTRUNC: FDST(truncf(fa)); break;
		case DRV_GPU_IR_FROUND_EVEN: FDST(rintf(fa)); break;
		case DRV_GPU_IR_FMIN: FDST(fminf(fa, fb)); break;
		case DRV_GPU_IR_FMAX: FDST(fmaxf(fa, fb)); break;
		case DRV_GPU_IR_FLT: DST(BOOLV(fa < fb)); break;
		case DRV_GPU_IR_FGE: DST(BOOLV(fa >= fb)); break;
		case DRV_GPU_IR_FEQ: DST(BOOLV(fa == fb)); break;
		case DRV_GPU_IR_FNEU: DST(BOOLV(!(fa == fb))); break;
		case DRV_GPU_IR_AND: case DRV_GPU_IR_IAND: DST(a & b); break;
		case DRV_GPU_IR_OR: case DRV_GPU_IR_IOR: DST(a | b); break;
		case DRV_GPU_IR_IXOR: DST(a ^ b); break;
		case DRV_GPU_IR_NOT: case DRV_GPU_IR_INOT: DST(~a); break;
		case DRV_GPU_IR_SELECT: DST(a != 0U ? b : value[inst->src[2]]); break;
		case DRV_GPU_IR_KILL: if (a != 0U) result->killed = 1; break;
		case DRV_GPU_IR_IADD: DST(a + b); break;
		case DRV_GPU_IR_ISUB: DST(a - b); break;
		case DRV_GPU_IR_IMUL: DST(a * b); break;
		case DRV_GPU_IR_INEG: DST(0U - a); break;
		case DRV_GPU_IR_SHL: DST(a << (b & 31U)); break;
		case DRV_GPU_IR_SHR: DST(a >> (b & 31U)); break;
		case DRV_GPU_IR_ASR: DST((uint32_t)((int32_t)a >> (b & 31U))); break;
		case DRV_GPU_IR_I2F: FDST((float)(int32_t)a); break;
		case DRV_GPU_IR_U2F: FDST((float)a); break;
		case DRV_GPU_IR_F2I: DST(float_to_int(fa, 1)); break;
		case DRV_GPU_IR_F2U: DST(float_to_int(fa, 0)); break;
		case DRV_GPU_IR_ILT: DST(BOOLV((int32_t)a < (int32_t)b)); break;
		case DRV_GPU_IR_IGE: DST(BOOLV((int32_t)a >= (int32_t)b)); break;
		case DRV_GPU_IR_ULT: DST(BOOLV(a < b)); break;
		case DRV_GPU_IR_UGE: DST(BOOLV(a >= b)); break;
		case DRV_GPU_IR_IEQ: DST(BOOLV(a == b)); break;
		case DRV_GPU_IR_INE: DST(BOOLV(a != b)); break;
		case DRV_GPU_IR_MOVE: DST(a); break;
		case DRV_GPU_IR_UDIV: DST(b == 0U ? 0U : a / b); break;
		case DRV_GPU_IR_UMOD: DST(b == 0U ? 0U : a % b); break;
		case DRV_GPU_IR_IDIV: DST(b == 0U || (a == 0x80000000U && b == 0xFFFFFFFFU) ? 0U : (uint32_t)((int32_t)a / (int32_t)b)); break;
		case DRV_GPU_IR_IREM: DST(b == 0U || (a == 0x80000000U && b == 0xFFFFFFFFU) ? 0U : (uint32_t)((int32_t)a % (int32_t)b)); break;
		case DRV_GPU_IR_LOAD_SHARED:
			if (inst->component != 1U || b != 0U)
				DST(result->buffers[BUFFERS - 1U][(a / 4U) % WORDS]);
			break;
		case DRV_GPU_IR_STORE_SHARED:
			if (inst->component != 1U || value[inst->src[2]] != 0U)
				result->buffers[BUFFERS - 1U][(a / 4U) % WORDS] = b;
			break;
		case DRV_GPU_IR_ATOMIC: {
			uint32_t *memory_word_ref;
			uint32_t old;
			uint32_t predicate_source;

			if ((inst->image_shape & DRV_GPU_IR_IMAGE_TEXEL) != 0U) {
				if (inst->location >= ir->uniform_count)
					break;
				slot = find_image(program, ir->uniforms[inst->location].set, ir->uniforms[inst->location].binding);
				if (slot < 0 || slot >= (int)IMAGES)
					break;
				memory_word_ref = &result->images[slot][image_texel(a, value[inst->src[0] + 1U], value[inst->src[0] + 2U]) * 4U];
			} else if (inst->location == DRV_GPU_IR_LOCATION_SHARED) {
				memory_word_ref = &result->buffers[BUFFERS - 1U][(a / 4U) % WORDS];
			} else {
				slot = find_buffer(program, ir, inst->location);
				if (slot < 0)
					break;
				memory_word_ref = &result->buffers[slot][(a / 4U) % WORDS];
			}
			predicate_source = inst->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG ? 3U : 2U;
			if (inst->component == DRV_GPU_IR_ATOMIC_PREDICATED && value[inst->src[predicate_source]] == 0U)
				break;
			old = *memory_word_ref;
			switch (inst->immediate) {
			case DRV_GPU_IR_ATOMIC_ADD: *memory_word_ref = old + b; break;
			case DRV_GPU_IR_ATOMIC_SUB: *memory_word_ref = old - b; break;
			case DRV_GPU_IR_ATOMIC_AND: *memory_word_ref = old & b; break;
			case DRV_GPU_IR_ATOMIC_OR: *memory_word_ref = old | b; break;
			case DRV_GPU_IR_ATOMIC_XOR: *memory_word_ref = old ^ b; break;
			case DRV_GPU_IR_ATOMIC_XCHG: *memory_word_ref = b; break;
			case DRV_GPU_IR_ATOMIC_CMPXCHG: if (old == value[inst->src[2]]) *memory_word_ref = b; break;
			case DRV_GPU_IR_ATOMIC_SMIN: *memory_word_ref = (int32_t)old < (int32_t)b ? old : b; break;
			case DRV_GPU_IR_ATOMIC_SMAX: *memory_word_ref = (int32_t)old > (int32_t)b ? old : b; break;
			case DRV_GPU_IR_ATOMIC_UMIN: *memory_word_ref = old < b ? old : b; break;
			case DRV_GPU_IR_ATOMIC_UMAX: *memory_word_ref = old > b ? old : b; break;
			default: result->why = "reference: an atomic it does not model"; break;
			}
			DST(old);
			break;
		}
		case DRV_GPU_IR_PACK_HALF: DST(to_half(fa) | (to_half(fb) << 16)); break;
		case DRV_GPU_IR_UNPACK_HALF: FDST(from_half((a >> (16U * (inst->component & 1U))) & 0xFFFFU)); break;
		case DRV_GPU_IR_LOOP_BEGIN:
			if (depth < 16U)
				loop_stack[depth++] = index;
			break;
		case DRV_GPU_IR_LOOP_END:
			if (depth == 0U)
				break;
			if (a != 0U)
				index = loop_stack[depth - 1U];
			else
				depth--;
			break;
		case DRV_GPU_IR_LOAD_SYSTEM:
			if (inst->component < 3U)
				DST(trial->local_id[inst->component]);
			else if (inst->component == DRV_GPU_IR_SYSTEM_LOCAL_INDEX)
				DST(trial->local_id[0] + trial->local_id[1] * ir->local_size[0] +
				    trial->local_id[2] * ir->local_size[0] * ir->local_size[1]);
			else if (inst->component <= DRV_GPU_IR_SYSTEM_GROUP_ID_Z)
				DST(trial->group_id[inst->component - DRV_GPU_IR_SYSTEM_GROUP_ID_X]);
			break;
		case DRV_GPU_IR_SAMPLE:
		case DRV_GPU_IR_SAMPLE_BIAS:
		case DRV_GPU_IR_SAMPLE_LOD:
		case DRV_GPU_IR_TEXTURE:
			reference_texture(program, trial, value, inst, texel);
			for (component = 0U; component < 4U; component++) {
				if (inst->dst + component <= ir->value_count)
					value[inst->dst + component] = texel[component];
			}
			break;
		case DRV_GPU_IR_IMAGE_READ:
		case DRV_GPU_IR_IMAGE_WRITE:
			reference_image(ir, program, value, inst, result);
			break;
		default:
			result->why = "reference: an operation it does not model";
			break;
		}
#undef DST
#undef FDST
#undef BOOLV
	}
	free(value);
}

/* Finds the table entry of a word (the most specific match). */
static int
model_decode(
	uint64_t word)
{
	unsigned best_bits;
	unsigned bits;
	unsigned index;
	int best;

	best = -1;
	best_bits = 0U;
	for (index = 0U; index < sizeof(model_table) / sizeof(model_table[0]); index++) {
		if ((word & model_table[index].mask) != model_table[index].value)
			continue;
		bits = (unsigned)__builtin_popcountll(model_table[index].mask);
		if (best < 0 || bits > best_bits) {
			best = (int)index;
			best_bits = bits;
		}
	}
	return best;
}

/* The state of the one-lane model. */
struct model {
	uint32_t r[64];
	uint32_t fau[256];
	uint32_t tls[TLS_WORDS];
	uint64_t pending[8];
	uint32_t tested;
	uint32_t emitted;
	uint32_t coverage_out;
	const struct mali_shader_program *program;
	const struct trial *trial;
	struct result *result;
	uint32_t stage;
};

/* Reads a 32-bit source byte. */
static uint32_t
read_source(
	struct model *m,
	uint8_t byte,
	uint32_t page)
{
	uint32_t number;

	if (byte < 0x80U)
		return m->r[byte & 0x3FU];
	if (byte < 0xC0U)
		return m->fau[(page * 64U + ((byte >> 1) & 0x1FU) * 2U + (byte & 1U)) & 255U];
	if (byte < 0xE0U)
		return model_immediates[byte & 0x1FU];
	number = (byte >> 1) & 0xFU;
	if (page == 1U && number == 3U)
		return (byte & 1U) != 0U ? (uint32_t)(SHARED_BASE >> 32) : (uint32_t)SHARED_BASE;
	if (page == 1U && number == 1U)
		return (byte & 1U) != 0U ? (uint32_t)(TLS_BASE >> 32) : (uint32_t)TLS_BASE;
	if (page == 3U && number == 1U)
		return 0U;
	return 0U;
}

/* Reads a 64-bit source (a register pair or a FAU slot). */
static uint64_t
read_source64(
	struct model *m,
	uint8_t byte,
	uint32_t page)
{
	uint64_t low;
	uint64_t high;

	if (byte < 0x80U) {
		low = m->r[byte & 0x3FU];
		high = m->r[((byte & 0x3FU) + 1U) & 63U];
	} else {
		low = read_source(m, (uint8_t)(byte & ~1U), page);
		high = read_source(m, (uint8_t)(byte | 1U), page);
	}
	return low | (high << 32);
}

/* A float source with its negate and absolute value bits. */
static float
float_source(
	struct model *m,
	uint64_t word,
	unsigned index,
	const struct isa_entry *entry)
{
	float value;
	unsigned neg;
	unsigned abs;

	value = as_float(read_source(m, (uint8_t)(word >> (8U * index)), (unsigned)((word >> 57) & 3U)));
	if ((entry->source_flags[index] & 1U) != 0U) {
		neg = 32U + 2U + (2U - index) * 2U;
		abs = neg + 1U;
		if (((word >> abs) & 1U) != 0U)
			value = fabsf(value);
		if (((word >> neg) & 1U) != 0U)
			value = -value;
	}
	return value;
}

/* Maps a model address to a word of memory, or NULL. */
static uint32_t *
memory_word(
	struct model *m,
	uint64_t address)
{
	uint64_t record;
	uint64_t offset;

	if (address >= OUTPUT_BASE && address < OUTPUT_BASE + 0x100000ULL) {
		record = (address - OUTPUT_BASE) / 256U;
		offset = (address - OUTPUT_BASE) % 256U;
		if (record == OUTPUT_INDEX && m->stage == 1U && offset < 16U)
			return &m->result->position[offset / 4U];
		if (record == OUTPUT_INDEX && m->stage == 2U && offset < 256U)
			return &m->result->varyings[offset / 16U][(offset / 4U) % 4U];
		if (record == OUTPUT_INDEX + 4U)
			return &m->result->varyings[15][3];
		return NULL;
	}
	if (address >= BUFFER_BASE && address < BUFFER_BASE + ((uint64_t)BUFFERS << 32)) {
		record = (address - BUFFER_BASE) >> 32;
		offset = (address - BUFFER_BASE) & 0xFFFFFFFFULL;
		return &m->result->buffers[record][(offset / 4U) % WORDS];
	}
	if (address >= SHARED_BASE && address < SHARED_BASE + 0x100000000ULL)
		return &m->result->buffers[BUFFERS - 1U][((address - SHARED_BASE) / 4U) % WORDS];
	if (address >= TLS_BASE && address < TLS_BASE + (uint64_t)m->program->tls_bytes && (address - TLS_BASE) / 4U < TLS_WORDS)
		return &m->tls[(address - TLS_BASE) / 4U];
	if (address >= IMAGE_BASE && address < IMAGE_BASE + ((uint64_t)IMAGES << 32)) {
		record = (address - IMAGE_BASE) >> 32;
		offset = (address - IMAGE_BASE) & 0xFFFFFFFFULL;
		if (offset / 4U < IMAGE_TEXELS * 4U)
			return &m->result->images[record][offset / 4U];
	}
	return NULL;
}

/* Evaluates a comparison condition. */
static int
compare_float(
	unsigned condition,
	float a,
	float b)
{
	switch (condition) {
	case 0U: return a == b;
	case 1U: return a > b;
	case 2U: return a >= b;
	case 3U: return !(a == b);
	case 4U: return a < b;
	case 5U: return a <= b;
	default: return 0;
	}
}

static int
compare_int(
	unsigned condition,
	uint32_t a,
	uint32_t b,
	int is_signed)
{
	int64_t x;
	int64_t y;

	x = is_signed ? (int64_t)(int32_t)a : (int64_t)a;
	y = is_signed ? (int64_t)(int32_t)b : (int64_t)b;
	switch (condition) {
	case 0U: return x == y;
	case 1U: return x > y;
	case 2U: return x >= y;
	case 3U: return x != y;
	case 4U: return x < y;
	case 5U: return x <= y;
	default: return 0;
	}
}

/* The exponent e of x = m * 2^e with m in [1, 2). */
static int
exponent_of(
	float x)
{
	int e;

	if (x == 0.0f || isinf(x) || isnan(x))
		return 0;
	frexpf(x, &e);
	return e - 1;
}

/*
 * Runs TEX_GRADIENT on the model: the derivatives in its staging
 * registers (dx, dy of each coordinate of its dimension), which must be
 * all of them, from registers (derivative_enable), of a handle whose high
 * word is zero; the descriptor's two words, the model's own, go to the
 * staging destination: gradient_sum() of the derivatives, and a token of
 * the texture and the dimension that a sample checks.
 */
static int
model_gradient(
	struct model *m,
	uint64_t word,
	uint32_t dest,
	uint32_t count)
{
	float derivatives[8];
	uint64_t handle;
	uint32_t dimension;
	uint32_t coordinates;
	uint32_t index;
	uint32_t out;

	/* Checks the fields: derivatives from registers, two words written, as many derivatives as the dimension has. */
	dimension = (uint32_t)((word >> 28) & 3U);
	coordinates = dimension + 1U;
	handle = read_source64(m, (uint8_t)word, (uint32_t)((word >> 57) & 3U));
	if (((word >> 15) & 1U) == 0U ||
	    ((word >> 22) & 0xFU) != 3U ||
	    ((word >> 36) & 7U) != 1U) {
		m->result->why = "TEX_GRADIENT without derivatives in registers or not writing two words";
		return 1;
	}

	/* A cube's level is computed by the program, not by TEX_GRADIENT. */
	if (dimension == 3U || count != 2U * coordinates) {
		m->result->why = "TEX_GRADIENT of a cube, or whose staging count does not match its dimension";
		return 1;
	}

	/* The descriptor is of the texture alone. */
	if ((handle >> 32) != 0U) {
		m->result->why = "TEX_GRADIENT of a handle with offsets or a level";
		return 1;
	}

	/* Makes the descriptor. */
	for (index = 0U; index < count; index++)
		derivatives[index] = as_float(m->r[(dest + index) & 63U]);
	out = (uint32_t)((word >> 16) & 0x3FU);
	m->r[out] = as_bits(gradient_sum(derivatives, count));
	m->r[(out + 1U) & 63U] = GRDESC_TOKEN | (dimension << 12) | ((uint32_t)handle & 0x3FFU);
	return 0;
}

/*
 * Runs a texture instruction on the model: the staging registers read in
 * the order the instruction's fields imply (s, t, r of a 3D image, the
 * array layer, the compare's reference, the offsets word, the level or
 * bias, the gradient descriptor's two words), which must be all of them;
 * a cube's s carries the face in its top three bits (an inference from
 * Mesa's packing); the level or bias and the gradient change the texel as
 * apply_level() and apply_gradient(); the reply goes to the write mask's
 * registers in order.  TEX_GRADIENT runs as model_gradient().
 */
static int
model_texture(
	struct model *m,
	uint64_t word,
	const char *name,
	uint32_t dest,
	uint32_t count)
{
	uint32_t base[4];
	uint32_t reply[4];
	uint32_t staging[8];
	uint32_t used;
	uint32_t dimension;
	uint32_t lod_mode;
	uint32_t layer;
	uint32_t face;
	uint32_t mask;
	uint32_t out;
	uint32_t index;
	uint32_t slot;
	uint32_t descriptor;
	uint32_t offsets;
	uint32_t narrow;
	int32_t level;
	int fetch;
	int gather;
	int compare;
	int levelled;
	float s;
	float t;
	float r;
	float reference;

	for (index = 0U; index < 8U; index++)
		staging[index] = m->r[(dest + index) & 63U];

	/* TEX_GRADIENT makes a descriptor; the others sample. */
	if (strcmp(name, "TEX_GRADIENT") == 0)
		return model_gradient(m, word, dest, count);
	fetch = strcmp(name, "TEX_FETCH") == 0;
	gather = strcmp(name, "TEX_GATHER") == 0;
	if (!fetch && !gather && strcmp(name, "TEX_SINGLE") != 0) {
		m->result->why = name;
		return 1;
	}
	dimension = (uint32_t)((word >> 28) & 3U);
	compare = !fetch && ((word >> 12) & 1U) != 0U;
	lod_mode = gather || fetch ? 0U : (uint32_t)((word >> 13) & 7U);
	used = 2U;
	r = 0.0f;
	layer = 0U;
	face = 0U;
	reference = 0.0f;
	if (dimension == 2U)
		r = fetch ? (float)(int32_t)staging[used++] : as_float(staging[used++]);
	if (((word >> 10) & 1U) != 0U)
		layer = staging[used++];
	if (compare)
		reference = as_float(staging[used++]);
	offsets = OFFSETS_NONE;
	if (((word >> 11) & 1U) != 0U)
		offsets = used++;

	/* An explicit level or a bias is 8.8 fixed point in the low half of its register. */
	levelled = 0;
	level = 0;
	if (lod_mode == 4U || lod_mode == 5U) {
		level = (int32_t)(int16_t)(uint16_t)(staging[used & 7U] & 0xFFFFU);
		levelled = 1;
		used++;
	}

	/* A gradient descriptor's two words come last. */
	descriptor = used;
	if (lod_mode == 6U)
		used += 2U;

	/* Every staging register is one of these. */
	if (used != count) {
		m->result->why = "a texture instruction's staging count does not match its fields";
		return 1;
	}
	if (fetch) {
		s = (float)(int32_t)staging[0];
		t = (float)(int32_t)staging[1];
	} else if (dimension == 3U) {
		face = staging[0] >> 29;
		s = as_float((staging[0] & 0x1FFFFFFFU) | 0x20000000U);
		t = as_float(staging[1]);
	} else {
		s = as_float(staging[0]);
		t = as_float(staging[1]);
	}
	slot = m->r[word & 0x3FU] & 0x3FFU;
	texel_value(slot, s, t, r, layer, face, base);

	/*
	 * The texel offset: signed bytes s, t, r of the offsets word (its top
	 * byte a texel load's level), else signed nibbles s, t, r in the
	 * handle's high word (Mesa's pan_nir_lower_tex.c packs them so); the
	 * high word's other bits (a narrow level or layer) are not modelled.
	 */
	narrow = m->r[((word & 0x3FU) + 1U) & 63U];
	if ((narrow & ~0xFFFU) != 0U) {
		m->result->why = "a texture handle with a narrow level or layer";
		return 1;
	}
	if (offsets != OFFSETS_NONE) {
		if (narrow != 0U) {
			m->result->why = "a texel offset in both the handle and the offsets word";
			return 1;
		}
		apply_offset(base, signed_bits(staging[offsets & 7U], 8U), signed_bits(staging[offsets & 7U] >> 8, 8U), signed_bits(staging[offsets & 7U] >> 16, 8U));
	} else {
		apply_offset(base, signed_bits(narrow, 4U), signed_bits(narrow >> 4, 4U), signed_bits(narrow >> 8, 4U));
	}

	/* The level or bias changes the texel. */
	if (levelled)
		apply_level(base, level);

	/* A gradient descriptor must be the one TEX_GRADIENT made for this texture and dimension. */
	if (lod_mode == 6U) {
		if (staging[(descriptor + 1U) & 7U] != (GRDESC_TOKEN | (dimension << 12) | slot)) {
			m->result->why = "a gradient descriptor TEX_GRADIENT did not make for this texture";
			return 1;
		}
		apply_gradient(base, as_float(staging[descriptor & 7U]));
	}
	message_reply(base, gather, (uint32_t)((word >> 14) & 3U), compare, reference, reply);

	/* The reply's components the write mask names, in order from the staging destination. */
	mask = (uint32_t)((word >> 22) & 0xFU);
	out = (uint32_t)((word >> 16) & 0x3FU);
	for (index = 0U; index < 4U; index++) {
		if ((mask & (1U << index)) == 0U)
			continue;
		m->r[out & 63U] = reply[index];
		out++;
	}
	return 0;
}

/*
 * Checks a LOAD of whole words: its staging registers are its words, its
 * load lane the identity one (w0 for one word), and a load of more than
 * one word is aligned to its bytes rounded up to a power of two (the
 * rule of Mesa's bifrost_nir.c mem_access_size_align_cb): nonzero when
 * it is not so.
 */
static int
check_load(
	const char *name,
	uint64_t word,
	uint32_t count,
	uint64_t address)
{
	uint32_t bits;
	uint32_t lane;

	bits = (uint32_t)strtoul(name + 6, NULL, 10);
	if (bits % 32U != 0U)
		return 0;
	lane = (uint32_t)((word >> 36) & 7U);
	if (bits / 32U != count)
		return 1;
	if ((bits == 32U && lane != 0U) || ((bits == 64U || bits == 128U) && lane != 7U) || (bits == 96U && lane != 6U))
		return 1;
	if (bits == 64U && (address & 7U) != 0U)
		return 1;
	if (bits > 64U && (address & 15U) != 0U)
		return 1;
	return 0;
}

/* Runs one program of the compiled shader on the model. */
static void
run_model(
	const uint8_t *code,
	size_t length,
	uint32_t stage,
	const struct mali_shader_program *program,
	const struct trial *trial,
	struct result *result)
{
	const struct isa_entry *entry;
	struct model m;
	uint64_t word;
	uint64_t reads;
	uint64_t writes;
	uint64_t address;
	uint32_t *memory;
	uint32_t pc;
	uint32_t steps;
	uint32_t flow;
	uint32_t last_flow;
	uint32_t page;
	uint32_t dest;
	uint32_t staging;
	uint32_t count;
	uint32_t slot;
	uint32_t index;
	uint32_t a;
	uint32_t b;
	uint32_t c;
	unsigned shift;
	int found;
	int32_t offset;
	float fa;
	float fb;
	const char *name;

	memset(&m, 0, sizeof(m));
	m.program = program;
	m.trial = trial;
	m.result = result;
	m.stage = stage;

	/* Lays out the FAU: push constants, the viewport, the buffers' addresses. */
	for (index = 0U; index < program->push_bytes / 4U && index < 64U; index++)
		m.fau[index] = trial->push[index];
	if (program->stage == MALI_SHADER_VERTEX) {
		for (index = 0U; index < 6U; index++)
			m.fau[program->viewport_fau_word + index] = trial->viewport[index];
	}
	for (index = 0U; index < program->buffer_count; index++) {
		if (program->buffers[index].size_fau_word != MALI_SHADER_NO_FAU_WORD)
			m.fau[program->buffers[index].size_fau_word & 255U] = trial->buffer_bytes[index];
	}
	for (index = 0U; index < program->texture_count; index++) {
		if (program->textures[index].size_fau_word == MALI_SHADER_NO_FAU_WORD)
			continue;
		for (slot = 0U; slot < 4U; slot++)
			m.fau[(program->textures[index].size_fau_word + slot) & 255U] = trial->texture_size[index][slot];
	}
	for (index = 0U; index < program->image_count; index++) {
		if (program->images[index].size_fau_word == MALI_SHADER_NO_FAU_WORD)
			continue;
		for (slot = 0U; slot < 4U; slot++)
			m.fau[(program->images[index].size_fau_word + slot) & 255U] = trial->texture_size[(8U + index) & 15U][slot];
	}
	for (index = 0U; index < program->buffer_count; index++) {
		address = BUFFER_BASE + ((uint64_t)index << 32);
		m.fau[program->buffers[index].fau_word] = (uint32_t)address;
		m.fau[program->buffers[index].fau_word + 1U] = (uint32_t)(address >> 32);
	}

	/* The preloads. */
	m.r[55] = trial->local_id[0] | (trial->local_id[1] << 16);
	m.r[56] = trial->local_id[2];
	if (program->stage == MALI_SHADER_COMPUTE) {
		m.r[57] = trial->group_id[0];
		m.r[58] = trial->group_id[1];
		m.r[59] = trial->group_id[2];
	} else if (program->stage == MALI_SHADER_VERTEX) {
		m.r[59] = OUTPUT_INDEX;
		m.r[60] = trial->vertex_id;
		m.r[61] = trial->instance_id;
	} else {
		m.r[59] = trial->frag_x | (trial->frag_y << 16);
		m.r[60] = trial->coverage;
		m.r[61] = trial->raster_coverage | (trial->sample_id << 16) | ((trial->frag_y & 7U) << 21);
	}

	pc = 0U;
	last_flow = 0U;
	for (steps = 0U; steps < MAX_STEPS; steps++) {
		if ((size_t)pc * 8U + 8U > length) {
			result->why = "the program runs past its end";
			return;
		}
		memcpy(&word, code + (size_t)pc * 8U, 8U);
		result->why_at = pc * 8U;
		found = model_decode(word);
		if (found < 0) {
			result->why = "a word that does not decode";
			return;
		}
		entry = &model_table[found];
		name = entry->name;
		flow = (uint32_t)((word >> 59) & 0xFU);
		page = (uint32_t)((word >> 57) & 3U);
		dest = (uint32_t)((word >> 40) & 0x3FU);
		count = (uint32_t)((word >> 33) & 7U);

		/* The registers it touches, for the wait check. */
		reads = 0U;
		for (index = 0U; index < entry->sources && index < 3U; index++) {
			/* A conversion's descriptor is the third source byte, its offset the second. */
			shift = 8U * index;
			if (index == 1U && (strcmp(name, "ST_CVT") == 0 || strcmp(name, "LD_CVT") == 0))
				shift = 16U;
			if (((word >> shift) & 0xC0U) < 0x80U) {
				reads |= 1ULL << ((word >> shift) & 0x3FU);
				/* A 64-bit source widened from one word (SHADDX's offset) reads that word alone. */
				if (entry->source_sizes[index] == 64U && !(index == 1U && strcmp(name, "SHADDX.u64") == 0 && ((word >> 26) & 0xFU) != 0U))
					reads |= 2ULL << ((word >> shift) & 0x3FU);
			}
		}
		writes = 0U;
		if (entry->dests != 0U)
			writes |= 1ULL << dest;
		if (entry->staging_read != 0U || entry->staging_write != 0U) {
			for (index = 0U; index < count; index++)
				reads |= 1ULL << ((dest + index) & 63U);
		}
		if (strncmp(name, "TEX_", 4) == 0 || strncmp(name, "ATOM_RETURN", 11) == 0) {
			staging = (uint32_t)((word >> 16) & 0x3FU);
			for (index = 0U; index <= ((word >> 36) & 7U); index++)
				writes |= 1ULL << ((staging + index) & 63U);
		}
		for (slot = 0U; slot < 8U; slot++) {
			if ((m.pending[slot] & (reads | writes)) != 0U) {
				result->why = "an instruction touches a register before the wait on the message writing it";
				return;
			}
		}
		if (strcmp(name, "ATEST") == 0 && last_flow != 8U && last_flow != 9U) {
			result->why = "ATEST without wait0126 before it";
			return;
		}
		if (strcmp(name, "ZS_EMIT") == 0 && last_flow != 8U && last_flow != 9U) {
			result->why = "ZS_EMIT without wait0126 before it";
			return;
		}
		if (strcmp(name, "BLEND") == 0 && last_flow != 9U) {
			result->why = "BLEND without the wait before it";
			return;
		}

		a = read_source(&m, (uint8_t)word, page);
		b = read_source(&m, (uint8_t)(word >> 8), page);
		c = read_source(&m, (uint8_t)(word >> 16), page);
		pc++;

		if (strcmp(name, "NOP") == 0 || strcmp(name, "BARRIER") == 0) {
		} else if (strcmp(name, "MOV.i32") == 0) {
			m.r[dest] = a;
		} else if (strcmp(name, "IADD_IMM.i32") == 0) {
			m.r[dest] = a + (uint32_t)(word >> 8);
		} else if (strcmp(name, "FADD.f32") == 0) {
			m.r[dest] = as_bits(float_source(&m, word, 0U, entry) + float_source(&m, word, 1U, entry));
		} else if (strcmp(name, "FMA.f32") == 0) {
			fa = fmaf(float_source(&m, word, 0U, entry), float_source(&m, word, 1U, entry), float_source(&m, word, 2U, entry));
			if (((word >> 32) & 1U) != 0U)
				fa = fmaxf(fa, 0.0f);
			if (((word >> 33) & 1U) != 0U)
				fa = fminf(fmaxf(fa, -1.0f), 1.0f);
			m.r[dest] = as_bits(fa);
		} else if (strcmp(name, "CUBEFACE1") == 0) {
			m.r[dest] = as_bits(fmaxf(fabsf(float_source(&m, word, 0U, entry)), fmaxf(fabsf(float_source(&m, word, 1U, entry)), fabsf(float_source(&m, word, 2U, entry)))));
		} else if (strcmp(name, "CUBEFACE2_V9") == 0) {
			m.r[dest] = cube_face(float_source(&m, word, 0U, entry), float_source(&m, word, 1U, entry), float_source(&m, word, 2U, entry)) << 29;
		} else if (strcmp(name, "CUBE_SSEL") == 0) {
			m.r[dest] = as_bits(cube_select_s(float_source(&m, word, 0U, entry), float_source(&m, word, 1U, entry), c >> 29));
		} else if (strcmp(name, "CUBE_TSEL") == 0) {
			m.r[dest] = as_bits(cube_select_t(float_source(&m, word, 0U, entry), float_source(&m, word, 1U, entry), c >> 29));
		} else if (strcmp(name, "FMIN.f32") == 0) {
			m.r[dest] = as_bits(fminf(float_source(&m, word, 0U, entry), float_source(&m, word, 1U, entry)));
		} else if (strcmp(name, "FMAX.f32") == 0) {
			m.r[dest] = as_bits(fmaxf(float_source(&m, word, 0U, entry), float_source(&m, word, 1U, entry)));
		} else if (strcmp(name, "FRCP.f32") == 0) {
			m.r[dest] = as_bits(1.0f / float_source(&m, word, 0U, entry));
		} else if (strcmp(name, "FRSQ.f32") == 0) {
			m.r[dest] = as_bits(1.0f / sqrtf(float_source(&m, word, 0U, entry)));
		} else if (strcmp(name, "FROUND.f32") == 0) {
			fa = float_source(&m, word, 0U, entry);
			switch ((word >> 30) & 3U) {
			case 0U: fa = rintf(fa); break;
			case 1U: fa = ceilf(fa); break;
			case 2U: fa = floorf(fa); break;
			default: fa = truncf(fa); break;
			}
			m.r[dest] = as_bits(fa);
		} else if (strcmp(name, "FREXPE.f32") == 0) {
			m.r[dest] = (uint32_t)exponent_of(as_float(a));
		} else if (strcmp(name, "FADD_LSCALE.f32") == 0) {
			fb = float_source(&m, word, 1U, entry);
			m.r[dest] = as_bits(float_source(&m, word, 0U, entry) + ldexpf(fb, -exponent_of(fb)));
		} else if (strcmp(name, "FLOGD.f32") == 0) {
			fa = ldexpf(as_float(a), -exponent_of(as_float(a)));
			if (fa == 1.0f)
				m.r[dest] = as_bits(1.0f / logf(2.0f));
			else
				m.r[dest] = as_bits(log2f(fa) / (fa - 1.0f));
			if (as_float(a) <= 0.0f || isinf(as_float(a)) || isnan(as_float(a)))
				m.r[dest] = as_bits(log2f(as_float(a)));
		} else if (strcmp(name, "FEXP.f32") == 0) {
			fb = float_source(&m, word, 1U, entry);
			if (isnan(fb) || fb > 128.0f || fb < -16777216.0f * 150.0f)
				m.r[dest] = as_bits(exp2f(fb / 16777216.0f));
			else
				m.r[dest] = as_bits(exp2f((float)(int32_t)a / 16777216.0f));
		} else if (strcmp(name, "S32_TO_F32") == 0) {
			m.r[dest] = as_bits((float)(int32_t)a);
		} else if (strcmp(name, "U32_TO_F32") == 0) {
			m.r[dest] = as_bits((float)a);
		} else if (strcmp(name, "F32_TO_S32") == 0 || strcmp(name, "F32_TO_U32") == 0) {
			fa = float_source(&m, word, 0U, entry);
			if (((word >> 30) & 3U) == 0U)
				fa = rintf(fa);
			m.r[dest] = float_to_int(fa, name[7] == 'S');
		} else if (strcmp(name, "F16_TO_F32") == 0) {
			m.r[dest] = as_bits(from_half((a >> (16U * ((word >> 28) & 1U))) & 0xFFFFU));
		} else if (strcmp(name, "U16_TO_U32") == 0) {
			m.r[dest] = (a >> (16U * ((word >> 28) & 1U))) & 0xFFFFU;
		} else if (strcmp(name, "U16_TO_F32") == 0) {
			m.r[dest] = as_bits((float)((a >> (16U * ((word >> 28) & 1U))) & 0xFFFFU));
		} else if (strcmp(name, "V2F32_TO_V2F16") == 0) {
			m.r[dest] = to_half(float_source(&m, word, 0U, entry)) | (to_half(float_source(&m, word, 1U, entry)) << 16);
		} else if (strcmp(name, "MKVEC.v2i16") == 0) {
			m.r[dest] = ((a >> (16U * ((word >> 28) & 1U))) & 0xFFFFU) | (((b >> (16U * ((word >> 26) & 1U))) & 0xFFFFU) << 16);
		} else if (strcmp(name, "FCMP_OR.f32") == 0) {
			m.r[dest] = (compare_float((unsigned)((word >> 32) & 7U), float_source(&m, word, 0U, entry), float_source(&m, word, 1U, entry)) ? 0xFFFFFFFFU : 0U) | c;
		} else if (strcmp(name, "ICMP_OR.u32") == 0 || strcmp(name, "ICMP_OR.s32") == 0) {
			m.r[dest] = (compare_int((unsigned)((word >> 32) & 7U), a, b, name[8] == 's') ? 0xFFFFFFFFU : 0U) | c;
		} else if (strncmp(name, "LSHIFT_", 7) == 0 || strncmp(name, "RSHIFT_", 7) == 0 || strncmp(name, "ARSHIFT_", 8) == 0) {
			shift = b & 0xFFU;
			if (((word >> 35) & 1U) != 0U)
				c = ~c;
			if (name[0] == 'L')
				a = shift >= 32U ? 0U : a << shift;
			else if (((word >> 34) & 1U) != 0U || name[0] == 'A')
				a = (uint32_t)((int32_t)a >> (shift >= 32U ? 31U : shift));
			else
				a = shift >= 32U ? 0U : a >> shift;
			if (strstr(name, "_AND") != NULL)
				m.r[dest] = a & c;
			else if (strstr(name, "_OR") != NULL)
				m.r[dest] = a | c;
			else
				m.r[dest] = a ^ c;
		} else if (strcmp(name, "MUX.i32") == 0) {
			if (((word >> 32) & 3U) == 3U) {
				m.r[dest] = (a & c) | (b & ~c);
				goto next;
			}
			if (((word >> 32) & 3U) != 1U) {
				result->why = "a MUX mode the model does not run";
				return;
			}
			m.r[dest] = c == 0U ? a : b;
		} else if (strcmp(name, "CLPER.i32") == 0) {
			/* One lane is a quad whose four lanes hold the same value. */
			m.r[dest] = a;
		} else if (strcmp(name, "IADD.u32") == 0) {
			m.r[dest] = a + b;
		} else if (strcmp(name, "ISUB.u32") == 0) {
			m.r[dest] = a - b;
		} else if (strcmp(name, "IMUL.i32") == 0) {
			m.r[dest] = a * b;
		} else if (strcmp(name, "SHADDX.u64") == 0) {
			address = read_source64(&m, (uint8_t)word, page) + ((uint64_t)b << ((word >> 20) & 7U));
			m.r[dest] = (uint32_t)address;
			m.r[(dest + 1U) & 63U] = (uint32_t)(address >> 32);
		} else if (strcmp(name, "DISCARD.f32") == 0) {
			if (compare_float((unsigned)((word >> 32) & 7U), float_source(&m, word, 0U, entry), float_source(&m, word, 1U, entry))) {
				result->killed = 1;
				return;
			}
		} else if (strcmp(name, "BRANCHZ") == 0) {
			offset = (int32_t)(((word >> 8) & 0x7FFFFFFU) << 5) >> 5;
			if ((((word >> 36) & 1U) != 0U) == (a == 0U))
				pc = (uint32_t)((int32_t)pc + offset);
		} else if (strcmp(name, "LD_ATTR_IMM") == 0) {
			index = (uint32_t)((word >> 20) & 0xFU);
			if (((word >> 16) & 0xFU) != MALI_SHADER_TABLE_ATTRIBUTES) {
				result->why = "LD_ATTR_IMM of another table";
				return;
			}
			for (slot = 0U; slot < count; slot++)
				m.r[(dest + slot) & 63U] = trial->attributes[index][slot & 3U];
		} else if (strcmp(name, "LD_VAR_BUF_IMM.f32") == 0) {
			index = (uint32_t)((word >> 16) & 0xFFU);
			for (slot = 0U; slot < count; slot++)
				m.r[(dest + slot) & 63U] = trial->varyings[(index / 16U) & 15U][(index / 4U + slot) & 3U];
		} else if (strcmp(name, "LD_VAR_SPECIAL") == 0) {
			index = (uint32_t)((word >> 12) & 0xFU);
			m.r[dest] = as_bits(index == 3U ? 0.5f : 1.0f);
		} else if (strcmp(name, "LEA_BUF_IMM") == 0) {
			if (((word >> 8) & 0xFU) != 13U) {
				result->why = "LEA_BUF_IMM of a table other than 61";
				return;
			}
			address = OUTPUT_BASE + (uint64_t)a * 256U;
			m.r[dest] = (uint32_t)address;
			m.r[(dest + 1U) & 63U] = (uint32_t)(address >> 32);
		} else if (strncmp(name, "LOAD.", 5) == 0 || strncmp(name, "STORE.", 6) == 0) {
			address = read_source64(&m, (uint8_t)word, page) + (uint64_t)(int64_t)(int16_t)(uint16_t)(word >> 8);
			if (name[0] == 'L' && check_load(name, word, count, address) != 0) {
				result->why = "a load whose staging count, lane or alignment does not fit its size";
				return;
			}
			if (address >= TLS_BASE && address < TLS_BASE + 0x100000000ULL && ((word >> 24) & 3U) != 3U) {
				result->why = "a thread local storage access without the force hint";
				return;
			}
			for (slot = 0U; slot < count; slot++) {
				memory = memory_word(&m, address + 4U * slot);
				if (memory == NULL) {
					result->why = "a memory access outside the model's memory";
					return;
				}
				if (name[0] == 'L')
					m.r[(dest + slot) & 63U] = *memory;
				else if (strcmp(name, "STORE.i16") == 0)
					*memory = as_bits(from_half(m.r[(dest + slot) & 63U] & 0xFFFFU));
				else
					*memory = m.r[(dest + slot) & 63U];
			}
		} else if (strcmp(name, "ATOM_RETURN.i32") == 0) {
			address = read_source64(&m, (uint8_t)word, page) + ((word >> 8) & 0xFFU);
			memory = memory_word(&m, address);
			if (memory == NULL) {
				result->why = "an atomic outside the model's memory";
				return;
			}
			a = *memory;
			b = m.r[dest];
			if (((word >> 26) & 1U) != 0U) {
				if (a == m.r[(dest + 1U) & 63U])
					*memory = b;
				m.r[(word >> 16) & 0x3FU] = a;
				goto next;
			}
			switch ((word >> 22) & 0xFU) {
			case 2U: *memory = a + b; break;
			case 8U: *memory = (int32_t)a < (int32_t)b ? a : b; break;
			case 9U: *memory = (int32_t)a > (int32_t)b ? a : b; break;
			case 10U: *memory = a < b ? a : b; break;
			case 11U: *memory = a > b ? a : b; break;
			case 12U: *memory = a & b; break;
			case 13U: *memory = a | b; break;
			case 14U: *memory = a ^ b; break;
			default: *memory = b; break;
			}
			m.r[(word >> 16) & 0x3FU] = a;
		} else if (strncmp(name, "TEX_", 4) == 0) {
			if (model_texture(&m, word, name, dest, count) != 0)
				return;
		} else if (strcmp(name, "LD_TEX_IMM") == 0 || strcmp(name, "LEA_TEX_IMM") == 0) {
			index = (uint32_t)((word >> 20) & 0xFU);
			if (((word >> 16) & 0xFU) != MALI_SHADER_TABLE_IMAGES || index >= program->image_count || index >= IMAGES) {
				result->why = "an image access of another table or entry";
				return;
			}
			if ((b & 0xFFFFU) != 0U) {
				result->why = "a storage image texel with a z coordinate";
				return;
			}
			slot = image_texel(a & 0xFFFFU, a >> 16, b >> 16) * 4U;
			if (name[1] == 'E') {
				address = IMAGE_BASE + ((uint64_t)index << 32) + (uint64_t)slot * 4U;
				m.r[dest] = (uint32_t)address;
				m.r[(dest + 1U) & 63U] = (uint32_t)(address >> 32);
				m.r[(dest + 2U) & 63U] = CVT_TOKEN | index;
				goto next;
			}
			c = format_components(program->images[index].format);
			for (shift = 0U; shift < count; shift++) {
				if (shift < c)
					m.r[(dest + shift) & 63U] = result->images[index][slot + shift];
				else if (shift == 3U)
					m.r[(dest + shift) & 63U] = ((word >> 24) & 7U) == 2U ? as_bits(1.0f) : 1U;
				else
					m.r[(dest + shift) & 63U] = 0U;
			}
		} else if (strcmp(name, "ST_CVT") == 0) {
			address = read_source64(&m, (uint8_t)word, page);
			c = m.r[(word >> 16) & 0x3FU];
			if (address < IMAGE_BASE || c != (CVT_TOKEN | (uint32_t)((address - IMAGE_BASE) >> 32)) || ((word >> 28) & 3U) + 1U != count) {
				result->why = "ST_CVT of an address, conversion or size LEA_TEX_IMM did not give";
				return;
			}
			for (slot = 0U; slot < count; slot++) {
				memory = memory_word(&m, address + 4U * slot);
				if (memory == NULL) {
					result->why = "a converted store outside the model's memory";
					return;
				}
				*memory = m.r[(dest + slot) & 63U];
			}
		} else if (strcmp(name, "ATEST") == 0) {
			if (m.tested != 0U) {
				result->why = "a second ATEST";
				return;
			}
			m.r[dest] = a;
			m.tested = 1U;
			m.coverage_out = a;
			result->coverage = a;
			result->coverage_tested = 1U;
		} else if (strcmp(name, "ZS_EMIT") == 0) {
			if (m.tested == 0U || m.emitted != 0U || c != m.coverage_out) {
				result->why = "ZS_EMIT before ATEST, twice, or of a coverage ATEST did not write";
				return;
			}
			if (((word >> 24) & 3U) == 0U) {
				result->why = "ZS_EMIT that writes neither depth nor stencil";
				return;
			}
			if (((word >> 25) & 1U) != 0U) {
				result->depth = a;
				result->depth_written = 1U;
			}
			if (((word >> 24) & 1U) != 0U) {
				result->stencil = b;
				result->stencil_written = 1U;
			}
			m.r[dest] = c;
			m.emitted = 1U;
		} else if (strcmp(name, "BLEND") == 0) {
			if (m.tested == 0U || c != m.coverage_out || ((program->writes_depth != 0U || program->writes_stencil != 0U) && m.emitted == 0U)) {
				result->why = "BLEND before ATEST or ZS_EMIT, or of a coverage ATEST did not write";
				return;
			}
			index = (((uint32_t)word >> 1) & 0xFU) - 8U;
			for (slot = 0U; slot < 4U && index < 8U; slot++)
				result->colors[index][slot] = m.r[(dest + slot) & 63U];
			if (index < 8U)
				result->color_written |= 1U << index;
		} else {
			result->why = name;
			return;
		}

next:
		/* A message's results arrive at its slot's wait. */
		if (entry->message != 0U && strcmp(name, "BLEND") != 0) {
			slot = (uint32_t)((word >> 30) & 7U);
			m.pending[slot] |= writes;
			if (entry->staging_write != 0U && strncmp(name, "TEX_", 4) != 0 && strncmp(name, "ATOM_RETURN", 11) != 0) {
				for (index = 0U; index < count; index++)
					m.pending[slot] |= 1ULL << ((dest + index) & 63U);
			}
			if (strncmp(name, "STORE", 5) == 0)
				m.pending[slot] &= ~(reads | writes);
		}
		if (flow >= 1U && flow <= 7U) {
			for (slot = 0U; slot < 3U; slot++) {
				if ((flow & (1U << slot)) != 0U)
					m.pending[slot] = 0U;
			}
		} else if (flow == 8U) {
			m.pending[0] = 0U;
			m.pending[1] = 0U;
			m.pending[2] = 0U;
		} else if (flow == 9U) {
			memset(m.pending, 0, sizeof(m.pending));
		} else if (flow == 15U) {
			return;
		}
		last_flow = flow;
	}
	result->why = "the program runs too long";
}

static int
close_enough(
	uint32_t a,
	uint32_t b)
{
	float x;
	float y;
	float scale;

	if (a == b)
		return 1;
	x = as_float(a);
	y = as_float(b);
	if (isnan(x) && isnan(y))
		return 1;
	if (isnan(x) || isnan(y) || isinf(x) || isinf(y))
		return 0;
	scale = fmaxf(1.0f, fmaxf(fabsf(x), fabsf(y)));
	return fabsf(x - y) <= 2e-4f * scale;
}

/* Fills one trial's inputs. */
static void
fill_trial(
	struct trial *trial)
{
	uint32_t index;
	uint32_t component;

	for (index = 0U; index < 16U; index++) {
		for (component = 0U; component < 4U; component++) {
			trial->attributes[index][component] = as_bits(((float)(next_random() % 4001U) - 2000.0f) / 1000.0f);
			trial->varyings[index][component] = as_bits(((float)(next_random() % 2001U)) / 1000.0f);
		}
	}
	for (index = 0U; index < 64U; index++)
		trial->push[index] = as_bits(((float)(next_random() % 4001U) - 2000.0f) / 1000.0f);
	for (index = 0U; index < 6U; index++)
		trial->viewport[index] = as_bits(((float)(next_random() % 2001U)) / 4.0f + 1.0f);
	for (index = 0U; index < BUFFERS; index++) {
		for (component = 0U; component < WORDS; component++)
			trial->buffers[index][component] = as_bits(((float)(next_random() % 4001U) - 2000.0f) / 1000.0f);
	}
	for (index = 0U; index < BUFFERS; index++)
		trial->buffer_bytes[index] = (next_random() % 1024U) * 4U;
	for (index = 0U; index < IMAGES; index++) {
		for (component = 0U; component < IMAGE_TEXELS * 4U; component++)
			trial->images[index][component] = next_random() % 4096U;
	}
	for (index = 0U; index < 16U; index++) {
		for (component = 0U; component < 4U; component++)
			trial->texture_size[index][component] = 1U + next_random() % 4096U;
	}
	for (index = 0U; index < 3U; index++) {
		trial->local_id[index] = next_random() % 4U;
		trial->group_id[index] = next_random() % 8U;
	}
	trial->vertex_id = next_random() % 1000U;
	trial->instance_id = next_random() % 4U;
	trial->frag_x = next_random() % 1920U;
	trial->frag_y = next_random() % 1080U;

	/* The samples covered, from the position (the other draws stay as they were). */
	trial->raster_coverage = 1U + (trial->frag_x + 3U * trial->frag_y) % 15U;
	trial->coverage = trial->raster_coverage;
	if ((trial->raster_coverage & (trial->raster_coverage - 1U)) != 0U)
		trial->coverage = trial->raster_coverage & (trial->raster_coverage - 1U);
	trial->sample_id = trial->frag_x % 16U;
}

/* The screen space position the position program must store for a clip space one. */
static void
expected_position(
	const struct trial *trial,
	const uint32_t *clip,
	uint32_t *screen)
{
	float recip;
	uint32_t axis;

	recip = 1.0f / as_float(clip[3]);
	recip = fminf(recip, 32768.0f);
	recip = fmaxf(recip, -32768.0f);
	for (axis = 0U; axis < 3U; axis++)
		screen[axis] = as_bits(fmaf(as_float(clip[axis]) * recip, as_float(trial->viewport[axis]), as_float(trial->viewport[3U + axis])));
	screen[3] = as_bits(recip);
}

/* A NoPerspective varying is stored multiplied by the clip space w. */
static void
expected_varying(
	const struct drv_gpu_shader_ir *ir,
	uint32_t location,
	struct result *reference)
{
	uint32_t index;
	uint32_t component;

	for (index = 0U; index < ir->output_count; index++) {
		if (ir->outputs[index].location != location || ir->outputs[index].noperspective == 0U)
			continue;
		for (component = 0U; component < 4U; component++)
			reference->varyings[location][component] =
				as_bits(as_float(reference->varyings[location][component]) * as_float(reference->position[3]));
		return;
	}
}

int
mali_model_check(
	const uint32_t *words,
	size_t count,
	uint32_t stage,
	const struct mali_shader_program *program,
	unsigned trials,
	const char *name)
{
	struct drv_gpu_compile_diagnostic diagnostic;
	struct drv_gpu_shader_ir *ir;
	struct trial *trial;
	struct result *reference;
	struct result *model;
	uint32_t screen[4];
	uint32_t location;
	uint32_t component;
	uint32_t buffer;
	uint32_t covered;
	unsigned run;
	int failed;
	enum drv_gpu_shader_stage ir_stage;

	ir_stage = stage == MALI_SHADER_VERTEX ? DRV_GPU_STAGE_VERTEX :
		   stage == MALI_SHADER_FRAGMENT ? DRV_GPU_STAGE_FRAGMENT : DRV_GPU_STAGE_COMPUTE;
	ir = NULL;
	if (drv_gpu_shader_parse_features(words, count, ir_stage, MALI_SHADER_SPIRV_FEATURES, &ir, &diagnostic) != 0) {
		printf("FAIL %s: the reference cannot parse it\n", name);
		return 1;
	}
	trial = malloc(sizeof(*trial));
	reference = malloc(sizeof(*reference));
	model = malloc(sizeof(*model));
	if (trial == NULL || reference == NULL || model == NULL) {
		printf("FAIL %s: out of memory\n", name);
		return 1;
	}
	random_state = 12345U;
	failed = 0;
	for (run = 0U; run < trials && failed == 0; run++) {
		fill_trial(trial);
		memset(reference, 0, sizeof(*reference));
		memcpy(reference->buffers, trial->buffers, sizeof(trial->buffers));
		memcpy(reference->images, trial->images, sizeof(trial->images));
		reference->position[3] = as_bits(1.0f);
		run_reference(ir, program, trial, reference);
		if (reference->why != NULL) {
			printf("FAIL %s: %s\n", name, reference->why);
			failed = 1;
			break;
		}

		memset(model, 0, sizeof(*model));
		memcpy(model->buffers, trial->buffers, sizeof(trial->buffers));
		memcpy(model->images, trial->images, sizeof(trial->images));
		if (stage == MALI_SHADER_VERTEX) {
			run_model(program->code, program->varying_offset != 0U ? program->varying_offset : program->length, 1U, program, trial, model);
			if (model->why == NULL && program->varying_offset != 0U)
				run_model(program->code + program->varying_offset, program->length - program->varying_offset, 2U, program, trial, model);
		} else {
			run_model(program->code, program->length, 0U, program, trial, model);
		}
		if (model->why != NULL) {
			printf("FAIL %s: trial %u: model: %s (at byte %04x of its program)\n", name, run, model->why, model->why_at);
			failed = 1;
			break;
		}

		if (stage == MALI_SHADER_VERTEX) {
			expected_position(trial, reference->position, screen);
			for (component = 0U; component < 4U; component++) {
				if (!close_enough(model->position[component], screen[component])) {
					printf("FAIL %s: trial %u: position %u is %08x, the reference %08x\n", name, run, component,
					       model->position[component], screen[component]);
					failed = 1;
				}
			}
			for (location = 0U; location < 16U; location++) {
				if ((program->varying_mask & (1U << location)) == 0U)
					continue;
				expected_varying(ir, location, reference);
				for (component = 0U; component < 4U; component++) {
					if (!close_enough(model->varyings[location][component], reference->varyings[location][component])) {
						printf("FAIL %s: trial %u: varying %u.%u is %08x, the reference %08x\n", name, run, location, component,
						       model->varyings[location][component], reference->varyings[location][component]);
						failed = 1;
					}
				}
			}
		} else if (stage == MALI_SHADER_FRAGMENT) {
			if (reference->killed != model->killed) {
				printf("FAIL %s: trial %u: discarded %d, the reference %d\n", name, run, model->killed, reference->killed);
				failed = 1;
			} else if (reference->killed == 0) {
				covered = trial->coverage;
				if (reference->sample_mask_written != 0U)
					covered &= reference->sample_mask;
				if (model->coverage_tested == 0U || model->coverage != covered) {
					printf("FAIL %s: trial %u: coverage tested %08x, the reference %08x\n", name, run, model->coverage, covered);
					failed = 1;
				}
				if (model->depth_written != reference->depth_written ||
				    (reference->depth_written != 0U && !close_enough(model->depth, reference->depth))) {
					printf("FAIL %s: trial %u: depth %08x (written %u), the reference %08x (written %u)\n", name, run,
					       model->depth, model->depth_written, reference->depth, reference->depth_written);
					failed = 1;
				}
				if (model->stencil_written != reference->stencil_written ||
				    (reference->stencil_written != 0U && model->stencil != reference->stencil)) {
					printf("FAIL %s: trial %u: stencil %08x (written %u), the reference %08x (written %u)\n", name, run,
					       model->stencil, model->stencil_written, reference->stencil, reference->stencil_written);
					failed = 1;
				}
				for (location = 0U; location < 8U; location++) {
					if ((reference->color_written & (1U << location)) == 0U)
						continue;
					for (component = 0U; component < 4U; component++) {
						if (!close_enough(model->colors[location][component], reference->colors[location][component])) {
							printf("FAIL %s: trial %u: colour %u.%u is %08x, the reference %08x\n", name, run, location, component,
							       model->colors[location][component], reference->colors[location][component]);
							failed = 1;
						}
					}
				}
			}
		}
		for (buffer = 0U; buffer < IMAGES && failed == 0; buffer++) {
			for (location = 0U; location < IMAGE_TEXELS * 4U; location++) {
				if (model->images[buffer][location] != reference->images[buffer][location]) {
					printf("FAIL %s: trial %u: image %u word %u is %08x, the reference %08x\n", name, run, buffer, location,
					       model->images[buffer][location], reference->images[buffer][location]);
					failed = 1;
					break;
				}
			}
		}
		for (buffer = 0U; buffer < BUFFERS && failed == 0; buffer++) {
			for (location = 0U; location < WORDS; location++) {
				if (!close_enough(model->buffers[buffer][location], reference->buffers[buffer][location])) {
					printf("FAIL %s: trial %u: buffer %u word %u is %08x, the reference %08x\n", name, run, buffer, location,
					       model->buffers[buffer][location], reference->buffers[buffer][location]);
					failed = 1;
					break;
				}
			}
		}
	}
	free(trial);
	free(reference);
	free(model);
	drv_gpu_shader_ir_free(ir);
	return failed;
}
