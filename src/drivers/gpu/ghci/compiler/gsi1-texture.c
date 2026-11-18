/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GSI1 compiler's sampled images: the samples, fetches, gathers and
 * queries of the IR's sampler messages.
 *
 * A texture instruction reads its coordinates from a run of registers
 * (u, v, then the third coordinate of the .3 forms, then the depth
 * reference of the .c forms), its level of detail, bias, level or sample
 * from rs2, and writes the four channels of the texel to a run of four
 * registers.  The compiler copies the message's parameters into a run of
 * scratch registers in that order.  An implicit level of detail is a
 * fragment program's only (texb takes it from the quad); elsewhere a sample
 * reads level 0.  Explicit gradients are turned into a level of detail
 * here: lambda = log2 of the larger of the two scaled gradients' lengths,
 * computed from their squares.  The texture slot names the image's
 * descriptor in set 0 and the sampler slot its sampler's in set 1.  A
 * uniform texel buffer is a 2D image 256 texels wide in its texture slot;
 * a fetch past its element count, which its resource table entry gives,
 * reads zero.
 */

#include "gsi1-private.h"

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <stddef.h>
#include <stdint.h>

/* The most coordinate registers a texture instruction reads: u, v, the third coordinate and Dref. */
#define GSI1_TEXTURE_COORDINATES	4U

/* The value of a parameter a message does not have. */
#define GSI1_NO_VALUE			UINT32_MAX

/* The bits of a texture instruction's `component` that hold the constant texel offset. */
#define GSI1_OFFSET_SHIFT		8U

/*
 * The float 0.5's bits, the weight that turns log2 of a squared length
 * into log2 of the length.
 *
 * Values referenced: IEEE 754-2019 (the binary32 encoding of 0.5).
 */
#define GSI1_TEXTURE_HALF		0x3f000000U

/*
 * What one texture instruction asks of the image: its coordinates (and how
 * many the image has), the layer of an array, the depth reference, the
 * bias, the level (or a multisampled image's sample) and the gradients,
 * GSI1_NO_VALUE for each one the message lacks; the texel offsets; and the
 * operation and the form it takes.
 *
 * Lives on the stack of gsi1_texture_lower().
 */
struct gsi1_sample {
	uint32_t message;
	uint32_t shape;
	uint32_t coordinate[3];
	uint32_t dimensions;
	uint32_t layer;
	uint32_t reference;
	uint32_t bias;
	uint32_t level;
	uint32_t gradient_x[3];
	uint32_t gradient_y[3];
	int32_t offset[3];
	uint32_t channel;
	uint32_t slot;
	uint32_t sampler;
	int three;
	int compare;
	int gathered;
	int gradients;
	int fetch;
};

static int find_slot(struct gsi1_lowering *lowering, uint32_t set, uint32_t binding_number, uint32_t element, struct gsi1_sample *sample, uint32_t *uniform);
static int read_message(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, struct gsi1_sample *sample);
static void read_coordinates(const struct drv_gpu_shader_ir_inst *instruction, struct gsi1_sample *sample, uint32_t first, int with_gradients);
static uint32_t param(const struct drv_gpu_shader_ir_inst *instruction, uint32_t index);
static int place_value(struct gsi1_lowering *lowering, uint32_t reg, uint32_t value);
static int place_coordinates(struct gsi1_lowering *lowering, const struct gsi1_sample *sample, uint32_t *block);
static int gradient_level(struct gsi1_lowering *lowering, const struct gsi1_sample *sample, uint32_t *level);
static int add_scaled_square(struct gsi1_lowering *lowering, uint32_t sum, uint32_t gradient, uint32_t scale, int first);
static int image_size(struct gsi1_lowering *lowering, const struct gsi1_sample *sample, uint32_t *width, uint32_t *height, uint32_t *depth);
static int lower_query(struct gsi1_lowering *lowering, const struct gsi1_sample *sample, const struct gsi1_binding *binding, uint32_t dst);
static int lower_buffer_fetch(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t uniform, struct gsi1_sample *sample, uint32_t dst);
static int lower_storage_query(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t uniform, uint32_t dst);
static void emit_texture(struct gsi1_lowering *lowering, uint32_t funct3, uint32_t funct7, const struct gsi1_sample *sample, uint32_t dst, uint32_t block, uint32_t rs2);
static void emit_texsz(struct gsi1_lowering *lowering, uint32_t immediate, uint32_t slot, uint32_t rd, uint32_t level);
static int32_t signed_nibble(uint32_t bits);

/* Lowers a sample, a fetch, a gather or a query of a sampled image. */
int
gsi1_texture_lower(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct gsi1_sample sample;
	uint32_t uniform;
	uint32_t block;
	uint32_t rs2;
	uint32_t funct3;
	uint32_t funct7;
	int error;

	/* The image's slot, or the uniform of a storage image a size query reads. */
	uniform = 0U;
	error = find_slot(lowering, instruction->location, instruction->immediate, instruction->element, &sample, &uniform);
	if (error != 0)
		return error;

	/* A query of a storage image or texel buffer reads its descriptor's words. */
	if (lowering->binding[uniform].kind != GSI1_BINDING_SAMPLED_IMAGE) {
		error = lower_storage_query(lowering, instruction, uniform, dst);
		if (error != 0)
			return error;

		/* Succeeded: the size is in dst. */
		return 0;
	}

	/* What the message asks for. */
	error = read_message(lowering, instruction, &sample);
	if (error != 0)
		return error;

	/* The queries. */
	if (sample.message == DRV_GPU_IR_TEXTURE_RESINFO || sample.message == DRV_GPU_IR_TEXTURE_SAMPLES) {
		error = lower_query(lowering, &sample, &lowering->binding[uniform], dst);
		if (error != 0)
			return error;

		/* Succeeded: the answer is in dst. */
		return 0;
	}

	/* A fetch from a uniform texel buffer. */
	if ((sample.shape & DRV_GPU_IR_IMAGE_KIND_MASK) == DRV_GPU_IR_IMAGE_BUFFER) {
		error = lower_buffer_fetch(lowering, instruction, uniform, &sample, dst);
		if (error != 0)
			return error;

		/* Succeeded: the texel is in dst. */
		return 0;
	}

	/* The coordinates' run. */
	error = place_coordinates(lowering, &sample, &block);
	if (error != 0)
		return error;

	/* The instruction's form: the .3 and .c modifiers. */
	funct7 = 0U;
	if (sample.three != 0)
		funct7 |= GSI1_TEX_F7_THREE;
	if (sample.compare != 0)
		funct7 |= GSI1_TEX_F7_COMPARE;

	/* A gather: texg of the channel, or texg.c of the compare. */
	if (sample.gathered != 0) {
		funct7 |= GSI1_TEX_F7_GATHER;
		if (sample.compare == 0)
			funct7 |= sample.channel << GSI1_TEX_F7_COMPONENT_SHIFT;
		emit_texture(lowering, GSI1_F3_TEXL, funct7, &sample, dst, block, 0U);
		return 0;
	}

	/* A fetch: texf, rs2 the level or a multisampled image's sample. */
	if (sample.fetch != 0) {
		rs2 = GSI1_ZERO;
		if (sample.level != GSI1_NO_VALUE) {
			error = gsi1_source(lowering, sample.level, &rs2);
			if (error != 0)
				return error;
		}
		emit_texture(lowering, GSI1_F3_TEXF, funct7, &sample, dst, block, rs2);
		return 0;
	}

	/* Explicit gradients become a level of detail. */
	if (sample.gradients != 0) {
		error = gradient_level(lowering, &sample, &rs2);
		if (error != 0)
			return error;
		emit_texture(lowering, GSI1_F3_TEXL, funct7, &sample, dst, block, rs2);
		return 0;
	}

	/* An explicit level: texl. */
	if (sample.level != GSI1_NO_VALUE) {
		error = gsi1_source(lowering, sample.level, &rs2);
		if (error != 0)
			return error;
		emit_texture(lowering, GSI1_F3_TEXL, funct7, &sample, dst, block, rs2);
		return 0;
	}

	/* An implicit level outside a fragment program is level 0. */
	if (lowering->ir->stage != DRV_GPU_STAGE_FRAGMENT) {
		emit_texture(lowering, GSI1_F3_TEXL, funct7, &sample, dst, block, GSI1_ZERO);
		return 0;
	}

	/* An implicit level from the quad, with the bias or none. */
	rs2 = GSI1_ZERO;
	if (sample.bias != GSI1_NO_VALUE) {
		error = gsi1_source(lowering, sample.bias, &rs2);
		if (error != 0)
			return error;
	}
	funct3 = GSI1_F3_TEXB;
	emit_texture(lowering, funct3, funct7, &sample, dst, block, rs2);

	/* Succeeded: the texel is in dst. */
	return 0;
}

/*
 * Finds the uniform of a set, binding and array element a texture
 * instruction names, and a sampled image's texture and sampler slots.
 */
static int
find_slot(
	struct gsi1_lowering *lowering,
	uint32_t set,
	uint32_t binding_number,
	uint32_t element,
	struct gsi1_sample *sample,
	uint32_t *uniform)
{
	const struct drv_gpu_shader_ir *ir;
	uint32_t index;
	uint32_t kind;
	int error;

	/* Names the module being lowered. */
	ir = lowering->ir;

	/* The uniform of the set, binding and element: a sampled image, or a storage image a query reads. */
	for (index = 0U; index < ir->uniform_count; index++) {
		if (ir->uniforms[index].set != set)
			continue;
		if (ir->uniforms[index].binding != binding_number)
			continue;
		if (ir->uniforms[index].element != element)
			continue;
		kind = lowering->binding[index].kind;
		if (kind != GSI1_BINDING_SAMPLED_IMAGE &&
		    kind != GSI1_BINDING_STORAGE_IMAGE &&
		    kind != GSI1_BINDING_TEXEL_BUFFER)
			continue;

		/* The uniform and its slots. */
		sample->slot = lowering->binding[index].slot;
		sample->sampler = lowering->binding[index].sampler;
		*uniform = index;
		return 0;
	}

	/* No sampled image has the binding. */
	error = gsi1_refuse(lowering, EINVAL, "a texture instruction on a binding that is not an image");
	return error;
}

/*
 * Reads a texture instruction's message into a sample: the parameters in
 * the order the message takes them (ir.h), the shape's coordinates and
 * layer, and the texel offsets.
 */
static int
read_message(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	struct gsi1_sample *sample)
{
	uint32_t index;
	uint32_t offsets;
	uint32_t kind;
	int error;

	/* Nothing is asked yet. */
	kern_memset(sample->offset, 0, sizeof(sample->offset));
	sample->coordinate[0] = GSI1_NO_VALUE;
	sample->coordinate[1] = GSI1_NO_VALUE;
	sample->coordinate[2] = GSI1_NO_VALUE;
	for (index = 0U; index < 3U; index++) {
		sample->gradient_x[index] = GSI1_NO_VALUE;
		sample->gradient_y[index] = GSI1_NO_VALUE;
	}
	sample->layer = GSI1_NO_VALUE;
	sample->reference = GSI1_NO_VALUE;
	sample->bias = GSI1_NO_VALUE;
	sample->level = GSI1_NO_VALUE;
	sample->channel = 0U;
	sample->three = 0;
	sample->compare = 0;
	sample->gathered = 0;
	sample->gradients = 0;
	sample->fetch = 0;

	/* The older sample operations: (u, v) and a bias or a level of a 2D image. */
	sample->shape = instruction->image_shape;
	if (instruction->op != DRV_GPU_IR_TEXTURE) {
		sample->shape = DRV_GPU_IR_IMAGE_2D;
		sample->dimensions = 2U;
		sample->coordinate[0] = instruction->src[0];
		sample->coordinate[1] = instruction->src[1];
		sample->message = DRV_GPU_IR_TEXTURE_SAMPLE;
		if (instruction->op == DRV_GPU_IR_SAMPLE_BIAS)
			sample->bias = instruction->src[2];
		if (instruction->op == DRV_GPU_IR_SAMPLE_LOD)
			sample->level = instruction->src[2];

		/* The offset: u in bits 11:8, v in 7:4. */
		sample->offset[0] = signed_nibble(instruction->component >> 8);
		sample->offset[1] = signed_nibble(instruction->component >> 4);
		return 0;
	}

	/* The message and the shape's coordinates. */
	sample->message = instruction->component & DRV_GPU_IR_TEXTURE_MESSAGE_MASK;
	kind = sample->shape & DRV_GPU_IR_IMAGE_KIND_MASK;
	sample->dimensions = 2U;
	if (kind == DRV_GPU_IR_IMAGE_3D || kind == DRV_GPU_IR_IMAGE_CUBE)
		sample->dimensions = 3U;

	/* A cube array is not offered (imageCubeArray is optional). */
	if (kind == DRV_GPU_IR_IMAGE_CUBE && (sample->shape & DRV_GPU_IR_IMAGE_ARRAYED) != 0U) {
		error = gsi1_refuse(lowering, ENOTSUP, "a cube map array (imageCubeArray is not offered)");
		return error;
	}

	/* The constant texel offset: u in bits 11:8, v in 7:4, r in 3:0 of the field. */
	offsets = (instruction->component >> GSI1_OFFSET_SHIFT) & 0xfffU;
	sample->offset[0] = signed_nibble(offsets >> 8);
	sample->offset[1] = signed_nibble(offsets >> 4);
	sample->offset[2] = signed_nibble(offsets);

	/* The parameters, by the message. */
	switch (sample->message) {
	case DRV_GPU_IR_TEXTURE_SAMPLE:
		read_coordinates(instruction, sample, 0U, 0);
		break;
	case DRV_GPU_IR_TEXTURE_SAMPLE_BIAS:
		sample->bias = param(instruction, 0U);
		read_coordinates(instruction, sample, 1U, 0);
		break;
	case DRV_GPU_IR_TEXTURE_SAMPLE_LOD:
		sample->level = param(instruction, 0U);
		read_coordinates(instruction, sample, 1U, 0);
		break;
	case DRV_GPU_IR_TEXTURE_SAMPLE_COMPARE:
		sample->reference = param(instruction, 0U);
		sample->compare = 1;
		read_coordinates(instruction, sample, 1U, 0);
		break;
	case DRV_GPU_IR_TEXTURE_SAMPLE_BIAS_COMPARE:
		sample->reference = param(instruction, 0U);
		sample->bias = param(instruction, 1U);
		sample->compare = 1;
		read_coordinates(instruction, sample, 2U, 0);
		break;
	case DRV_GPU_IR_TEXTURE_SAMPLE_LOD_COMPARE:
		sample->reference = param(instruction, 0U);
		sample->level = param(instruction, 1U);
		sample->compare = 1;
		read_coordinates(instruction, sample, 2U, 0);
		break;
	case DRV_GPU_IR_TEXTURE_SAMPLE_DERIVS:
		sample->gradients = 1;
		read_coordinates(instruction, sample, 0U, 1);
		break;
	case DRV_GPU_IR_TEXTURE_SAMPLE_DERIV_COMPARE:
		sample->reference = param(instruction, 0U);
		sample->compare = 1;
		sample->gradients = 1;
		read_coordinates(instruction, sample, 1U, 1);
		break;
	case DRV_GPU_IR_TEXTURE_GATHER4:
		sample->gathered = 1;
		sample->channel = DRV_GPU_IR_TEXTURE_CHANNEL(instruction->component);
		read_coordinates(instruction, sample, 0U, 0);
		break;
	case DRV_GPU_IR_TEXTURE_GATHER4_COMPARE:
		sample->reference = param(instruction, 0U);
		sample->compare = 1;
		sample->gathered = 1;
		read_coordinates(instruction, sample, 1U, 0);
		break;
	case DRV_GPU_IR_TEXTURE_LD:
		/* u, v, the level, then r (a layer, a 3D image's w, or a multisampled image's sample). */
		sample->fetch = 1;
		sample->coordinate[0] = param(instruction, 0U);
		sample->coordinate[1] = param(instruction, 1U);
		sample->level = param(instruction, 2U);
		if ((sample->shape & DRV_GPU_IR_IMAGE_MULTISAMPLED) != 0U &&
		    (sample->shape & DRV_GPU_IR_IMAGE_ARRAYED) != 0U) {
			sample->layer = param(instruction, 3U);
			sample->level = param(instruction, 4U);
		} else if ((sample->shape & DRV_GPU_IR_IMAGE_MULTISAMPLED) != 0U) {
			sample->level = param(instruction, 3U);
		} else if (kind == DRV_GPU_IR_IMAGE_3D) {
			sample->coordinate[2] = param(instruction, 3U);
		} else if ((sample->shape & DRV_GPU_IR_IMAGE_ARRAYED) != 0U) {
			sample->layer = param(instruction, 3U);
		}
		break;
	case DRV_GPU_IR_TEXTURE_RESINFO:
		sample->level = param(instruction, 0U);
		break;
	case DRV_GPU_IR_TEXTURE_SAMPLES:
		break;
	default:
		error = gsi1_refuse(lowering, ENOTSUP, "a sampler message GSI1 does not have");
		return error;
	}

	/* A cube's direction and a 3D image's w take the .3 forms, and so does an array's layer. */
	if (sample->dimensions == 3U || sample->layer != GSI1_NO_VALUE)
		sample->three = 1;

	/* Succeeded: the message is read. */
	return 0;
}

/*
 * Reads the coordinates from the message's parameter `first` on: each of
 * the image's coordinates (followed by its two gradients, x then y, when
 * the message has them), then the layer of an array.
 */
static void
read_coordinates(
	const struct drv_gpu_shader_ir_inst *instruction,
	struct gsi1_sample *sample,
	uint32_t first,
	int with_gradients)
{
	uint32_t next;
	uint32_t index;

	/* Each coordinate of the image, with its gradients. */
	next = first;
	for (index = 0U; index < sample->dimensions; index++) {
		sample->coordinate[index] = param(instruction, next);
		next++;
		if (with_gradients == 0)
			continue;

		/* The coordinate's gradients along x and y. */
		sample->gradient_x[index] = param(instruction, next);
		sample->gradient_y[index] = param(instruction, next + 1U);
		next += 2U;
	}

	/* The layer of an array (a 2D array's r). */
	if ((sample->shape & DRV_GPU_IR_IMAGE_ARRAYED) != 0U)
		sample->layer = param(instruction, next);
}

/* Reports a message's parameter `index`, or GSI1_NO_VALUE past the last (it reads as zero). */
static uint32_t
param(
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t index)
{
	/* A parameter the message stopped before. */
	if (index >= instruction->src[1])
		return GSI1_NO_VALUE;

	/* Succeeded: the parameter's value. */
	return instruction->src[0] + index;
}

/* Puts a value (zero for GSI1_NO_VALUE) into a register. */
static int
place_value(
	struct gsi1_lowering *lowering,
	uint32_t reg,
	uint32_t value)
{
	uint32_t source;
	int error;

	/* A parameter the message lacks reads as zero. */
	if (value == GSI1_NO_VALUE) {
		gsi1_emit_move(lowering, reg, GSI1_ZERO);
		return 0;
	}

	/* A constant goes straight into the register. */
	if (lowering->kind[value] == GSI1_VALUE_CONSTANT) {
		gsi1_emit_constant(lowering, reg, lowering->constant[value]);
		return 0;
	}

	/* Finds where the value lives. */
	error = gsi1_source(lowering, value, &source);
	if (error != 0)
		return error;

	/* Copies it. */
	gsi1_emit_move(lowering, reg, source);

	/* Succeeded: the value is in the register. */
	return 0;
}

/*
 * Copies the coordinates into a run of scratch registers: u, v, the third
 * coordinate of a .3 form, then the depth reference of a .c form.
 */
static int
place_coordinates(
	struct gsi1_lowering *lowering,
	const struct gsi1_sample *sample,
	uint32_t *block)
{
	uint32_t values[GSI1_TEXTURE_COORDINATES];
	uint32_t count;
	uint32_t index;
	int error;

	/* u and v. */
	values[0] = sample->coordinate[0];
	values[1] = sample->coordinate[1];
	count = 2U;

	/* The third coordinate: a 3D image's w, a cube's z, or an array's layer. */
	if (sample->three != 0) {
		values[2] = sample->coordinate[2];
		if (sample->dimensions == 2U)
			values[2] = sample->layer;
		count = 3U;
	}

	/* The depth reference after the coordinates. */
	if (sample->compare != 0) {
		values[count] = sample->reference;
		count++;
	}

	/* Takes the run. */
	error = gsi1_scratch_block(lowering, count, block);
	if (error != 0)
		return error;

	/* Copies each value into its place. */
	for (index = 0U; index < count; index++) {
		error = place_value(lowering, *block + index, values[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: the coordinates are in the run. */
	return 0;
}

/*
 * Computes the level of detail of explicit gradients into a scratch
 * register: the image's size at level 0 scales the gradients, lambda =
 * log2(max(|d/dx|, |d/dy|)) = 0.5 log2(max(|d/dx|^2, |d/dy|^2)).  A cube's
 * gradients are of the direction; they are scaled by half the face size
 * over the direction's largest magnitude.
 */
static int
gradient_level(
	struct gsi1_lowering *lowering,
	const struct gsi1_sample *sample,
	uint32_t *level)
{
	uint32_t scale[3];
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t along_x;
	uint32_t along_y;
	uint32_t major;
	uint32_t operand;
	uint32_t index;
	int cube;
	int error;

	/* The image's size at level 0. */
	error = image_size(lowering, sample, &width, &height, &depth);
	if (error != 0)
		return error;

	/* The scale of each coordinate's gradients: its size, or a cube's face size over its major axis. */
	scale[0] = width;
	scale[1] = height;
	scale[2] = depth;
	cube = 0;
	if ((sample->shape & DRV_GPU_IR_IMAGE_KIND_MASK) == DRV_GPU_IR_IMAGE_CUBE)
		cube = 1;
	if (cube != 0) {
		error = gsi1_scratch(lowering, &major);
		if (error != 0)
			return error;

		/* The direction's largest magnitude. */
		for (index = 0U; index < 3U; index++) {
			error = gsi1_source(lowering, sample->coordinate[index], &operand);
			if (error != 0)
				return error;
			if (index == 0U) {
				gsi1_emit_float(lowering, GSI1_F7_FSGNJ, 0U, major, operand, operand, GSI1_W1_A1 | GSI1_W1_A2);
			} else {
				gsi1_emit_float(lowering, GSI1_F7_FMINMAX, 1U, major, major, operand, GSI1_W1_A2);
			}
		}

		/* Half the face size over it, for every coordinate. */
		gsi1_emit_transcendental(lowering, GSI1_XU_RCP, major, major);
		gsi1_emit_float(lowering, GSI1_F7_FMUL, GSI1_RM_RNE, major, major, width, 0U);
		gsi1_emit_float(lowering, GSI1_F7_FMUL, GSI1_RM_RNE, major, major, GSI1_HALF, 0U);
		scale[0] = major;
		scale[1] = major;
		scale[2] = major;
	}

	/* Takes the scratch registers of the two squared lengths. */
	error = gsi1_scratch(lowering, &along_x);
	if (error != 0)
		return error;

	/* Takes the second one. */
	error = gsi1_scratch(lowering, &along_y);
	if (error != 0)
		return error;

	/* Each coordinate's scaled gradients, squared and summed. */
	for (index = 0U; index < sample->dimensions; index++) {
		error = add_scaled_square(lowering, along_x, sample->gradient_x[index], scale[index], index == 0U);
		if (error != 0)
			return error;
		error = add_scaled_square(lowering, along_y, sample->gradient_y[index], scale[index], index == 0U);
		if (error != 0)
			return error;
	}

	/* lambda = 0.5 log2(max(x, y)). */
	gsi1_emit_float(lowering, GSI1_F7_FMINMAX, 1U, along_x, along_x, along_y, 0U);
	gsi1_emit_transcendental(lowering, GSI1_XU_LOG2, along_x, along_x);
	gsi1_emit_float(lowering, GSI1_F7_FMUL, GSI1_RM_RNE, along_x, along_x, GSI1_HALF, 0U);

	/* Succeeded: the level of detail is in a scratch register. */
	*level = along_x;
	return 0;
}

/*
 * Adds (gradient x scale)^2 to a running sum (sets it for the first
 * coordinate).
 */
static int
add_scaled_square(
	struct gsi1_lowering *lowering,
	uint32_t sum,
	uint32_t gradient,
	uint32_t scale,
	int first)
{
	uint32_t operand;
	uint32_t scaled;
	int error;

	/* A gradient the message lacks adds nothing. */
	if (gradient == GSI1_NO_VALUE) {
		if (first != 0)
			gsi1_emit_move(lowering, sum, GSI1_ZERO);
		return 0;
	}

	/* The gradient. */
	error = gsi1_source(lowering, gradient, &operand);
	if (error != 0)
		return error;

	/* Takes a scratch register for the scaled gradient. */
	error = gsi1_scratch(lowering, &scaled);
	if (error != 0)
		return error;

	/* scaled = gradient x scale, then sum = scaled^2 (+ sum). */
	gsi1_emit_float(lowering, GSI1_F7_FMUL, GSI1_RM_RNE, scaled, operand, scale, 0U);
	if (first != 0) {
		gsi1_emit_float(lowering, GSI1_F7_FMUL, GSI1_RM_RNE, sum, scaled, scaled, 0U);
	} else {
		gsi1_emit_fused(lowering, GSI1_OP_MADD, sum, scaled, scaled, sum, 0U);
	}

	/* Succeeded: the square is added. */
	return 0;
}

/*
 * Reads the image's size at level 0 as floats into scratch registers: the
 * width and height from texsz, and a 3D image's depth from texsz.d (one
 * for the others).
 */
static int
image_size(
	struct gsi1_lowering *lowering,
	const struct gsi1_sample *sample,
	uint32_t *width,
	uint32_t *height,
	uint32_t *depth)
{
	int error;

	/* Takes the scratch registers of the three sizes. */
	error = gsi1_scratch(lowering, width);
	if (error != 0)
		return error;

	/* Takes the height's. */
	error = gsi1_scratch(lowering, height);
	if (error != 0)
		return error;

	/* Takes the depth's. */
	error = gsi1_scratch(lowering, depth);
	if (error != 0)
		return error;

	/* {height, width} of level 0, unpacked and converted. */
	emit_texsz(lowering, GSI1_TEXSZ_SIZE, sample->slot, *width, GSI1_ZERO);
	gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SRL, GSI1_F7_BASE, *height, *width, 16U));
	gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SLL, GSI1_F7_BASE, *width, *width, 16U));
	gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SRL, GSI1_F7_BASE, *width, *width, 16U));
	gsi1_emit_float(lowering, GSI1_F7_FCVT_S_W, GSI1_RM_RNE, *width, *width, 1U, 0U);
	gsi1_emit_float(lowering, GSI1_F7_FCVT_S_W, GSI1_RM_RNE, *height, *height, 1U, 0U);

	/* The depth of a 3D image; 1.0 for the others. */
	if ((sample->shape & DRV_GPU_IR_IMAGE_KIND_MASK) == DRV_GPU_IR_IMAGE_3D) {
		emit_texsz(lowering, GSI1_TEXSZ_DEPTH, sample->slot, *depth, GSI1_ZERO);
		gsi1_emit_float(lowering, GSI1_F7_FCVT_S_W, GSI1_RM_RNE, *depth, *depth, 1U, 0U);
	} else {
		gsi1_emit_move(lowering, *depth, GSI1_ONE);
	}

	/* Succeeded: the sizes are in their registers. */
	return 0;
}

/*
 * Lowers the size and sample queries: the width and height of a level
 * from texsz, the depth or the layers from texsz.d (one for an image that
 * has neither), the level count from texsz.l, the samples from texsz.s.
 */
static int
lower_query(
	struct gsi1_lowering *lowering,
	const struct gsi1_sample *sample,
	const struct gsi1_binding *binding,
	uint32_t dst)
{
	uint32_t level;
	uint32_t kind;
	int error;

	/* The samples, in the reply's first value. */
	if (sample->message == DRV_GPU_IR_TEXTURE_SAMPLES) {
		emit_texsz(lowering, GSI1_TEXSZ_SAMPLES, sample->slot, dst, 0U);
		return 0;
	}

	/* A uniform texel buffer's size is its element count, from its entry. */
	if ((sample->shape & DRV_GPU_IR_IMAGE_KIND_MASK) == DRV_GPU_IR_IMAGE_BUFFER) {
		error = gsi1_resource_field(lowering, binding, GSI1_FIELD_ELEMENTS, &level);
		if (error != 0)
			return error;
		gsi1_emit_move(lowering, dst, level);
		gsi1_emit_move(lowering, dst + 1U, GSI1_ONEI);
		gsi1_emit_move(lowering, dst + 2U, GSI1_ONEI);
		gsi1_emit_move(lowering, dst + 3U, GSI1_ONEI);
		return 0;
	}

	/* The level asked for. */
	level = GSI1_ZERO;
	if (sample->level != GSI1_NO_VALUE) {
		error = gsi1_source(lowering, sample->level, &level);
		if (error != 0)
			return error;
	}

	/* {height, width}: the height into dst + 1, the width's 16 bits into dst. */
	emit_texsz(lowering, GSI1_TEXSZ_SIZE, sample->slot, dst, level);
	gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SRL, GSI1_F7_BASE, dst + 1U, dst, 16U));
	gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SLL, GSI1_F7_BASE, dst, dst, 16U));
	gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SRL, GSI1_F7_BASE, dst, dst, 16U));

	/* The depth of a 3D image or the layers of an array, one otherwise. */
	kind = sample->shape & DRV_GPU_IR_IMAGE_KIND_MASK;
	if (kind == DRV_GPU_IR_IMAGE_3D || (sample->shape & DRV_GPU_IR_IMAGE_ARRAYED) != 0U) {
		emit_texsz(lowering, GSI1_TEXSZ_DEPTH, sample->slot, dst + 2U, level);
	} else {
		gsi1_emit_move(lowering, dst + 2U, GSI1_ONEI);
	}

	/* The level count. */
	emit_texsz(lowering, GSI1_TEXSZ_LEVELS, sample->slot, dst + 3U, 0U);

	/* Succeeded: the size is in dst. */
	return 0;
}

/*
 * Lowers a fetch from a uniform texel buffer: element i is texel (i mod
 * 256, i / 256) of level 0; an element past the buffer's count (from its
 * resource table entry) reads zero.
 */
static int
lower_buffer_fetch(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t uniform,
	struct gsi1_sample *sample,
	uint32_t dst)
{
	const struct gsi1_binding *binding;
	uint32_t element;
	uint32_t block;
	uint32_t inside;
	uint32_t count;
	uint32_t clamped;
	uint32_t channel;
	int error;

	/* The element's index. */
	error = gsi1_source(lowering, param(instruction, 0U), &element);
	if (error != 0)
		return error;

	/* The buffer's elements. */
	binding = &lowering->binding[uniform];
	error = gsi1_resource_field(lowering, binding, GSI1_FIELD_ELEMENTS, &count);
	if (error != 0)
		return error;

	/* Takes the run of the coordinates and the register of the clamped index. */
	error = gsi1_scratch_block(lowering, 2U, &block);
	if (error != 0)
		return error;
	error = gsi1_scratch(lowering, &clamped);
	if (error != 0)
		return error;

	/*
	 * The index the fetch reads: i clamped to the last element (to 0 for
	 * an empty buffer), so that no fetch reads past the view; a texel
	 * past the count reads zero below.
	 */
	gsi1_emit_alu_immediate(lowering, GSI1_F3_ADD, clamped, count, -1);
	gsi1_emit_alu(lowering, GSI1_F3_MINU, GSI1_F7_MINMAX, clamped, clamped, count);
	gsi1_emit_alu(lowering, GSI1_F3_MINU, GSI1_F7_MINMAX, clamped, element, clamped);

	/* x = i & 255, y = i >> 8. */
	gsi1_emit_alu_immediate(lowering, GSI1_F3_AND, block, clamped, (int32_t)(GSI1_ABI_TEXEL_BUFFER_WIDTH - 1U));
	gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SRL, GSI1_F7_BASE, block + 1U, clamped, GSI1_ABI_TEXEL_BUFFER_SHIFT));

	/* The texel of level 0. */
	sample->three = 0;
	sample->compare = 0;
	sample->offset[0] = 0;
	sample->offset[1] = 0;
	sample->offset[2] = 0;
	emit_texture(lowering, GSI1_F3_TEXF, 0U, sample, dst, block, GSI1_ZERO);

	/* Whether the element is inside the buffer: i < count, unsigned. */
	error = gsi1_scratch(lowering, &inside);
	if (error != 0)
		return error;
	gsi1_emit_alu(lowering, GSI1_F3_SLTU, GSI1_F7_BASE, inside, element, count);

	/* A texel past the count reads zero. */
	for (channel = 0U; channel < 4U; channel++)
		gsi1_emit_alu(lowering, GSI1_F3_AND, GSI1_F7_BASE, dst + channel, dst + channel, inside);

	/* Succeeded: the element is in dst. */
	return 0;
}

/*
 * Lowers the size query of a storage image or a storage texel buffer from
 * its resource table entry: an image's width and height and layers, a
 * texel buffer's elements.
 */
static int
lower_storage_query(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t uniform,
	uint32_t dst)
{
	const struct gsi1_binding *binding;
	uint32_t word;
	int error;

	/* Only the size query reads a storage image's descriptor. */
	if ((instruction->component & DRV_GPU_IR_TEXTURE_MESSAGE_MASK) != DRV_GPU_IR_TEXTURE_RESINFO) {
		error = gsi1_refuse(lowering, EINVAL, "a sampler message on a storage image");
		return error;
	}
	binding = &lowering->binding[uniform];

	/* A texel buffer: its elements. */
	if (binding->kind == GSI1_BINDING_TEXEL_BUFFER) {
		error = gsi1_resource_field(lowering, binding, GSI1_FIELD_ELEMENTS, &word);
		if (error != 0)
			return error;
		gsi1_emit_move(lowering, dst, word);
		gsi1_emit_move(lowering, dst + 1U, GSI1_ONEI);
		gsi1_emit_move(lowering, dst + 2U, GSI1_ONEI);
		gsi1_emit_move(lowering, dst + 3U, GSI1_ONEI);
		return 0;
	}

	/* An image: width and height from the packed word. */
	error = gsi1_resource_field(lowering, binding, GSI1_FIELD_EXTENT, &word);
	if (error != 0)
		return error;
	gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SRL, GSI1_F7_BASE, dst + 1U, word, 16U));
	gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SLL, GSI1_F7_BASE, dst, word, 16U));
	gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SRL, GSI1_F7_BASE, dst, dst, 16U));

	/* The layers (or a 3D image's depth). */
	error = gsi1_resource_field(lowering, binding, GSI1_FIELD_LAYERS, &word);
	if (error != 0)
		return error;
	gsi1_emit_move(lowering, dst + 2U, word);

	/* One level. */
	gsi1_emit_move(lowering, dst + 3U, GSI1_ONEI);

	/* Succeeded: the size is in dst. */
	return 0;
}

/* Emits a texture R-form instruction: rd..rd+3, the coordinates' run, rs2, the slot and the offsets. */
static void
emit_texture(
	struct gsi1_lowering *lowering,
	uint32_t funct3,
	uint32_t funct7,
	const struct gsi1_sample *sample,
	uint32_t dst,
	uint32_t block,
	uint32_t rs2)
{
	struct gsi1_extra extra;

	/* The slots and the texel offsets. */
	kern_memset(&extra, 0, sizeof(extra));
	extra.sub = sample->slot;
	extra.sampler = sample->sampler;
	extra.offset_u = sample->offset[0];
	extra.offset_v = sample->offset[1];
	extra.offset_w = sample->offset[2];

	/* A gather has no rs2. */
	if ((funct7 & GSI1_TEX_F7_GATHER) != 0U && funct3 == GSI1_F3_TEXL)
		rs2 = 0U;

	/* The R form. */
	gsi1_emit(lowering, gsi1_encode_r(GSI1_OP_CUSTOM1, funct3, funct7, dst, block, rs2, &extra));
}

/* Emits texsz with its immediate: the size, the level count, the depth or the samples. */
static void
emit_texsz(
	struct gsi1_lowering *lowering,
	uint32_t immediate,
	uint32_t slot,
	uint32_t rd,
	uint32_t level)
{
	uint32_t rs1;

	/* texsz.l and texsz.s have no rs1. */
	rs1 = level;
	if (immediate == GSI1_TEXSZ_LEVELS || immediate == GSI1_TEXSZ_SAMPLES)
		rs1 = 0U;

	/* texsz.s needs multisampling (DEVICE_CAPS[31]); the pipeline is refused without it. */
	if (immediate == GSI1_TEXSZ_SAMPLES)
		lowering->flags |= GHCI_GSI1_FLAG_SAMPLE_QUERY;

	/* The I form: the slot in sub. */
	gsi1_emit(lowering, gsi1_encode_i(GSI1_OP_CUSTOM1, GSI1_F3_TEXSZ, rd, rs1, (int32_t)immediate, slot));
}

/* Reports the signed value of the low four bits. */
static int32_t
signed_nibble(
	uint32_t bits)
{
	int32_t value;

	/* The four bits, with bit 3 as the sign. */
	value = (int32_t)(bits & 0xfU);
	if (value >= 8)
		value -= 16;

	/* Succeeded: the value from -8 to 7. */
	return value;
}
