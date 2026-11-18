/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lowering of the vertex and fragment programs (see lower.h): their
 * inputs and outputs, the texture operations, the derivatives, the packing
 * of 16-bit floats, the discards, and the shader program header.
 */

#include "drivers/gpu/nvrtx/compiler/sm75/lower.h"

#include "drivers/gpu/compiler/spirv.h"

#include <uapi/errno.h>

#include <stddef.h>

/*
 * What goes into one register of a texture instruction's source vectors:
 * a value of the program, zero, an immediate, a float layer (rounded to the
 * nearest integer and clamped to 0..65535), a cube direction's component
 * divided by the direction's largest magnitude, or a TXD's layer (rounded,
 * or none) with its offset's bits above.
 */
#define NVRTX_SM75_PART_VALUE		0U
#define NVRTX_SM75_PART_ZERO		1U
#define NVRTX_SM75_PART_IMMEDIATE	2U
#define NVRTX_SM75_PART_LAYER		3U
#define NVRTX_SM75_PART_CUBE		4U
#define NVRTX_SM75_PART_LAYER_OFFSET	5U

/* The largest layer a texture instruction takes. */
#define NVRTX_SM75_LAYER_MAX		0xffffU

/*
 * One register of a source vector: its kind (NVRTX_SM75_PART_*), the value
 * it is made from (NVRTX_SM75_NOT_DEFINED for none) and its bits.
 */
struct nvrtx_sm75_texture_part {
	uint32_t kind;
	uint32_t value;
	uint32_t bits;
};

/*
 * One texture operation as the hardware takes it
 * (nvrtx_sm75_texture_plan()): the instruction, its dimension, its level of
 * detail mode, the channels it writes, the channel a gather reads, whether
 * it compares and has a constant offset, what each register of its two
 * source vectors holds, and whether its reply is a level of detail to
 * convert.  Lives on the lowering's stack.
 */
struct nvrtx_sm75_texture_plan {
	uint32_t opcode;
	uint32_t dim;
	uint32_t lod_mode;
	uint32_t channels;
	uint32_t channel;
	int compare;
	int multisample;
	int offset;
	int lod_query;
	struct nvrtx_sm75_texture_part parts[2][NVRTX_SM75_TEX_VECTOR];
	uint32_t count[2];
};

static int nvrtx_sm75_texture_plan(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, struct nvrtx_sm75_texture_plan *plan);
static void nvrtx_sm75_texture_part(struct nvrtx_sm75_texture_plan *plan, uint32_t vector, uint32_t kind, uint32_t value, uint32_t bits);
static int nvrtx_sm75_texture_vector(struct nvrtx_sm75_lowering *lowering, uint32_t count, uint32_t *reg);
static void nvrtx_sm75_texture_give(struct nvrtx_sm75_lowering *lowering, uint32_t reg, uint32_t count);
static int nvrtx_sm75_texture_fill(struct nvrtx_sm75_lowering *lowering, const struct nvrtx_sm75_texture_plan *plan, const uint32_t *vectors);
static int nvrtx_sm75_texture_cube(struct nvrtx_sm75_lowering *lowering, const struct nvrtx_sm75_texture_plan *plan, const uint32_t *vectors);
static int nvrtx_sm75_texture_layer(struct nvrtx_sm75_lowering *lowering, uint32_t reg, uint32_t value);
static int nvrtx_sm75_texture_lod_reply(struct nvrtx_sm75_lowering *lowering, uint32_t first);
static int nvrtx_sm75_emit_move(struct nvrtx_sm75_lowering *lowering, uint32_t dst, const struct nvrtx_sm75_src *src);
static const struct drv_gpu_shader_ir_io *nvrtx_sm75_input(const struct drv_gpu_shader_ir *ir, uint32_t location);
static void nvrtx_sm75_sph_set(uint32_t *sph, unsigned low, unsigned bits, uint32_t value);
static void nvrtx_sm75_mark_store(struct nvrtx_sm75_lowering *lowering, uint32_t address);
static int nvrtx_sm75_lower_instance_index(struct nvrtx_sm75_lowering *lowering, uint32_t dst);
static int nvrtx_sm75_lower_pixel_input(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int nvrtx_sm75_vtg_output(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t *address);
static int nvrtx_sm75_prepare_geometry(struct nvrtx_sm75_lowering *lowering);
static int nvrtx_sm75_geometry_prologue(struct nvrtx_sm75_lowering *lowering);
static int nvrtx_sm75_geometry_output(struct nvrtx_sm75_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, const struct nvrtx_sm75_src *value);

/*
 * Prepares a graphics program: a fragment program's colour outputs take
 * R0 onward, four registers for each location it writes in ascending
 * order (the registers the hardware reads at EXIT; a second colour of dual
 * source blending is location 1's), then the sample mask's register when
 * it writes the mask or the depth, then the depth's, which no value may
 * take; a vertex program's store request starts empty.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/from_nir.rs
 * (ShaderFromNir::parse_intrinsic, copy_fs_outputs_nv: the order of the
 * fragment outputs in OpRegOut, four for each location written, in
 * ascending order).
 */
int
nvrtx_sm75_prepare_graphics(
	struct nvrtx_sm75_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t written;
	uint32_t location;
	uint32_t index;
	uint32_t next;
	int second;
	int depth;
	int mask;
	int error;

	/* The program. */
	ir = lowering->ir;

	/* No colour location has registers yet, and nothing is stored. */
	for (location = 0U; location < NVRTX_SM75_COLOR_TARGETS; location++)
		lowering->color_reg[location] = NVRTX_SM75_NO_REG;
	lowering->inverse_w = NVRTX_SM75_NO_REG;
	lowering->mask_reg = NVRTX_SM75_NO_REG;
	lowering->depth_reg = NVRTX_SM75_NO_REG;
	lowering->store_first = 0xffU;
	lowering->store_last = 0U;

	/* A geometry program holds its outputs in registers. */
	if (ir->stage == DRV_GPU_STAGE_GEOMETRY) {
		error = nvrtx_sm75_prepare_geometry(lowering);
		return error;
	}

	/* Only a fragment program has colour outputs. */
	if (ir->stage != DRV_GPU_STAGE_FRAGMENT)
		return 0;

	/* The locations the program writes, and whether it writes the depth and the sample mask. */
	written = 0U;
	depth = 0;
	mask = 0;
	second = 0;
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		if (instruction->op != DRV_GPU_IR_STORE_OUTPUT)
			continue;

		/* An output is a colour word, the second colour's, the depth or the mask, or it is refused at its instruction. */
		lowering->index = index;
		if (instruction->location == DRV_GPU_IR_LOCATION_FRAG_DEPTH) {
			depth = 1;
			continue;
		}
		if (instruction->location == DRV_GPU_IR_LOCATION_SAMPLE_MASK) {
			mask = 1;
			continue;
		}
		if (instruction->location == DRV_GPU_IR_LOCATION_SECOND_COLOR && instruction->component <= 3U) {
			second = 1;
			written |= 1U << 1;
			continue;
		}
		if (instruction->location >= NVRTX_SM75_COLOR_TARGETS || instruction->component > 3U) {
			error = nvrtx_sm75_refuse(lowering, ENOTSUP, "a fragment output other than a colour, a second colour, the depth or the sample mask");
			return error;
		}

		/* The location is written; dual source blending writes location 0 alone besides its second colour. */
		if (instruction->location != 0U && second != 0) {
			error = nvrtx_sm75_refuse(lowering, ENOTSUP, "a second colour with a colour at a location past 0");
			return error;
		}
		written |= 1U << instruction->location;
	}
	if (second != 0 && (written & ~3U) != 0U) {
		error = nvrtx_sm75_refuse(lowering, ENOTSUP, "a second colour with a colour at a location past 0");
		return error;
	}

	/* The lowering starts again at the first instruction. */
	lowering->index = 0U;

	/* Four registers for each, from R0, which stay taken. */
	next = 0U;
	for (location = 0U; location < NVRTX_SM75_COLOR_TARGETS; location++) {
		if ((written & (1U << location)) == 0U)
			continue;
		lowering->color_reg[location] = next;
		for (index = 0U; index < 4U; index++)
			lowering->taken[next + index] = 1U;
		next += 4U;
	}

	/* The mask's register when the mask or the depth is written (they come together), then the depth's. */
	if (mask != 0 || depth != 0) {
		lowering->mask_reg = next;
		lowering->taken[next] = 1U;
		next++;
	}
	if (depth != 0) {
		lowering->depth_reg = next;
		lowering->taken[next] = 1U;
		next++;
	}

	/* The program uses those registers at least. */
	if (next > lowering->highest)
		lowering->highest = next;

	/* Succeeded: the outputs have their registers. */
	return 0;
}

/*
 * Starts a fragment program that interpolates in perspective: w at the
 * pixel (the position's w, passed screen-linear) and its reciprocal, which
 * every perspective input is multiplied by.
 *
 * Procedure referenced: Mesa 25.3.6
 * src/nouveau/compiler/nak_nir_lower_fs_inputs.c (load_frag_w and
 * lower_fs_input_intrin: the SM70 interpolation, w passed screen-linear,
 * then 1 / w).
 */
int
nvrtx_sm75_prologue(
	struct nvrtx_sm75_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	const struct drv_gpu_shader_ir_io *input;
	struct nvrtx_sm75_word word;
	uint32_t index;
	int perspective;
	int error;

	/* The program. */
	ir = lowering->ir;

	/* A geometry program starts its handle, its outputs and its input index. */
	if (ir->stage == DRV_GPU_STAGE_GEOMETRY) {
		error = nvrtx_sm75_geometry_prologue(lowering);
		return error;
	}

	/* Only a fragment program interpolates. */
	if (ir->stage != DRV_GPU_STAGE_FRAGMENT)
		return 0;

	/* Whether any input is read in perspective. */
	perspective = 0;
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		if (instruction->op != DRV_GPU_IR_LOAD_INPUT || instruction->location >= 64U)
			continue;
		input = nvrtx_sm75_input(ir, instruction->location);
		if (input != NULL &&
		    input->flat == 0U &&
		    input->noperspective == 0U)
			perspective = 1;
	}

	/* No input needs 1 / w. */
	if (perspective == 0)
		return 0;

	/* A register for 1 / w that stays taken. */
	error = nvrtx_sm75_take(lowering, &lowering->inverse_w);
	if (error != 0)
		return error;

	/* w, passed screen-linear. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_ipa(&word, lowering->inverse_w, NVRTX_SM75_ATTR_POSITION_W, NVRTX_SM75_IPA_PASS);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	if (error != 0)
		return error;

	/* Its reciprocal. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_mufu(&word, lowering->inverse_w, NVRTX_SM75_MUFU_RCP, lowering->inverse_w);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	if (error != 0)
		return error;

	/* Succeeded: 1 / w is ready. */
	nvrtx_sm75_sph_set(lowering->binary->sph, NVRTX_SM75_SPH_IMAP_AB_LOW + NVRTX_SM75_ATTR_POSITION_W / 4U, 1U, 1U);
	return 0;
}

/*
 * Lowers an input: a vertex program's attribute word (ALD), its vertex or
 * instance number; a fragment program's interpolated word (IPA, in
 * perspective multiplied by 1 / w), or the pixel's position.  The
 * header's input map records each.
 *
 * Procedure referenced: Mesa 25.3.6
 * src/nouveau/compiler/nak_nir_lower_fs_inputs.c (interp_fs_input: from
 * SM70 on, an input in perspective is passed, then multiplied by 1 / w).
 */
int
nvrtx_sm75_lower_input(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	const struct drv_gpu_shader_ir_io *input;
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src b;
	uint32_t *sph;
	uint32_t address;
	uint32_t slot;
	uint32_t mode;
	int error;

	/* The header's maps, and an input of at most four components. */
	sph = lowering->binary->sph;
	if (instruction->component > 3U) {
		error = nvrtx_sm75_refuse(lowering, EINVAL, "an input component past the fourth");
		return error;
	}

	/*
	 * A vertex program: a generic attribute, the vertex number or the
	 * instance number.
	 *
	 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/ir.rs
	 * (VtgIoInfo::mark_attrs: a bit of the input map for each word, the
	 * generic words from 0x080 and the C part of the system values from
	 * 0x2c0).
	 */
	if (lowering->ir->stage == DRV_GPU_STAGE_VERTEX) {
		if (instruction->location < 32U) {
			address = NVRTX_SM75_ATTR_GENERIC + instruction->location * 16U + instruction->component * 4U;
			nvrtx_sm75_sph_set(sph, NVRTX_SM75_SPH_IMAP_GENERIC_LOW + (address - NVRTX_SM75_ATTR_GENERIC) / 4U, 1U, 1U);
		} else if (instruction->location == DRV_GPU_SHADER_LOCATION_VERTEX_INDEX) {
			address = NVRTX_SM75_ATTR_VERTEX_ID;
			nvrtx_sm75_sph_set(sph, NVRTX_SM75_SPH_IMAP_C_VTG_LOW + (address - 0x2c0U) / 4U, 1U, 1U);
		} else if (instruction->location == DRV_GPU_SHADER_LOCATION_INSTANCE_INDEX) {
			address = NVRTX_SM75_ATTR_INSTANCE_ID;
			nvrtx_sm75_sph_set(sph, NVRTX_SM75_SPH_IMAP_C_VTG_LOW + (address - 0x2c0U) / 4U, 1U, 1U);
			error = nvrtx_sm75_lower_instance_index(lowering, dst);
			return error;
		} else {
			error = nvrtx_sm75_refuse(lowering, ENOTSUP, "a vertex input not compiled yet");
			return error;
		}

		/* ALD of the word. */
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_ald(&word, dst, address);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
		if (error != 0)
			return error;

		/* Succeeded: the input is in dst. */
		return 0;
	}

	/* Only the fragment stage reads interpolated inputs. */
	if (lowering->ir->stage != DRV_GPU_STAGE_FRAGMENT) {
		error = nvrtx_sm75_refuse(lowering, ENOTSUP, "inputs of this stage are not compiled yet");
		return error;
	}

	/* The pixel's system inputs: the face, the samples covered, the point's coordinate, the primitive's number. */
	if (instruction->location == DRV_GPU_SHADER_LOCATION_FRONT_FACING ||
	    instruction->location == DRV_GPU_SHADER_LOCATION_SAMPLE_MASK ||
	    instruction->location == DRV_GPU_SHADER_LOCATION_POINT_COORD ||
	    instruction->location == DRV_GPU_SHADER_LOCATION_PRIMITIVE_ID) {
		error = nvrtx_sm75_lower_pixel_input(lowering, instruction, dst);
		return error;
	}

	/* The pixel's position: screen-linear, passed. */
	if (instruction->location == DRV_GPU_SHADER_LOCATION_FRAG_COORD) {
		address = NVRTX_SM75_ATTR_POSITION + instruction->component * 4U;
		nvrtx_sm75_sph_set(sph, NVRTX_SM75_SPH_IMAP_AB_LOW + address / 4U, 1U, 1U);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_ipa(&word, dst, address, NVRTX_SM75_IPA_PASS);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
		if (error != 0)
			return error;

		/* Succeeded: the position's component is in dst. */
		return 0;
	}

	/*
	 * A located input and its interpolation.
	 *
	 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/ir.rs
	 * (FragmentIoInfo::mark_attr_read) and
	 * src/nouveau/compiler/nak/sph.rs
	 * (ShaderProgramHeader::set_imap_vector_ps: two bits of the input map
	 * for each generic word).
	 */
	input = NULL;
	if (instruction->location < 32U)
		input = nvrtx_sm75_input(lowering->ir, instruction->location);
	if (input == NULL) {
		error = nvrtx_sm75_refuse(lowering, ENOTSUP, "a fragment input not compiled yet");
		return error;
	}

	/* Its word, and its interpolation in the header's input map. */
	address = NVRTX_SM75_ATTR_GENERIC + instruction->location * 16U + instruction->component * 4U;
	slot = (address - NVRTX_SM75_ATTR_GENERIC) / 4U;
	mode = NVRTX_SM75_IMAP_PERSPECTIVE;
	if (input->noperspective != 0U)
		mode = NVRTX_SM75_IMAP_SCREEN_LINEAR;
	if (input->flat != 0U)
		mode = NVRTX_SM75_IMAP_CONSTANT;
	nvrtx_sm75_sph_set(sph, NVRTX_SM75_SPH_IMAP_GENERIC_LOW + slot * 2U, 2U, mode);

	/* The interpolation: a flat input is the provoking vertex's, the others are passed. */
	nvrtx_sm75_begin(&word);
	if (mode == NVRTX_SM75_IMAP_CONSTANT) {
		nvrtx_sm75_ipa(&word, dst, address, NVRTX_SM75_IPA_CONSTANT);
	} else {
		nvrtx_sm75_ipa(&word, dst, address, NVRTX_SM75_IPA_PASS);
	}

	/* Appends the IPA. */
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	if (error != 0)
		return error;

	/* In perspective, times 1 / w. */
	if (mode == NVRTX_SM75_IMAP_PERSPECTIVE) {
		a = nvrtx_sm75_reg(dst);
		b = nvrtx_sm75_reg(lowering->inverse_w);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_fmul(&word, dst, &a, &b);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
		if (error != 0)
			return error;
	}

	/* Succeeded: the input is in dst. */
	return 0;
}

/*
 * Lowers an output: a vertex program's attribute word (AST: the position,
 * the point size or a generic attribute), recorded in the header's output
 * map and store request; a fragment program's colour word, moved into the
 * register the hardware reads it from at EXIT and recorded in the header's
 * targets.
 */
int
nvrtx_sm75_lower_output(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src value;
	uint32_t *sph;
	uint32_t address;
	uint32_t location;
	int error;

	/* The header's maps, the value, and an output of at most four components. */
	sph = lowering->binary->sph;
	value = nvrtx_sm75_value(lowering, instruction->src[0]);
	if (instruction->component > 3U) {
		error = nvrtx_sm75_refuse(lowering, EINVAL, "an output component past the fourth");
		return error;
	}

	/* A fragment program's depth or sample mask: its register after the colours, and the header's bit. */
	if (lowering->ir->stage == DRV_GPU_STAGE_FRAGMENT &&
	    (instruction->location == DRV_GPU_IR_LOCATION_FRAG_DEPTH || instruction->location == DRV_GPU_IR_LOCATION_SAMPLE_MASK)) {
		nvrtx_sm75_begin(&word);
		if (instruction->location == DRV_GPU_IR_LOCATION_FRAG_DEPTH) {
			nvrtx_sm75_mov(&word, lowering->depth_reg, &value);
			nvrtx_sm75_sph_set(sph, NVRTX_SM75_SPH_OMAP_DEPTH, 1U, 1U);
		} else {
			nvrtx_sm75_mov(&word, lowering->mask_reg, &value);
			nvrtx_sm75_sph_set(sph, NVRTX_SM75_SPH_OMAP_SAMPLE_MASK, 1U, 1U);
		}
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
		return error;
	}

	/*
	 * A fragment program: the colour's register (location 1's for the
	 * second colour of dual source blending).
	 *
	 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/from_nir.rs
	 * (ShaderFromNir::parse_intrinsic, copy_fs_outputs_nv: writes_color, a
	 * bit for each component of each location) and
	 * src/nouveau/compiler/nak/sph.rs
	 * (ShaderProgramHeader::set_omap_targets).
	 */
	if (lowering->ir->stage == DRV_GPU_STAGE_FRAGMENT) {
		location = instruction->location;
		if (location == DRV_GPU_IR_LOCATION_SECOND_COLOR)
			location = 1U;
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_mov(&word, lowering->color_reg[location] + instruction->component, &value);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
		if (error != 0)
			return error;

		/* Succeeded: the colour word is in its register, and the header's targets say so. */
		nvrtx_sm75_sph_set(sph, NVRTX_SM75_SPH_OMAP_TARGETS_LOW + location * 4U + instruction->component, 1U, 1U);
		return 0;
	}

	/* A geometry program's output word goes into its register, which an emit stores. */
	if (lowering->ir->stage == DRV_GPU_STAGE_GEOMETRY) {
		error = nvrtx_sm75_geometry_output(lowering, instruction, &value);
		return error;
	}

	/* Only the vertex stage stores attributes as it writes them. */
	if (lowering->ir->stage != DRV_GPU_STAGE_VERTEX) {
		error = nvrtx_sm75_refuse(lowering, ENOTSUP, "outputs of this stage are not compiled yet");
		return error;
	}

	/* The attribute's address, its bit of the output map and the store request. */
	error = nvrtx_sm75_vtg_output(lowering, instruction, &address);
	if (error != 0)
		return error;

	/* The store, which reads its data register later. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_ast(&word, address, value.value);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_STORE);
	if (error != 0)
		return error;

	/* Succeeded: the attribute is written. */
	return 0;
}

/*
 * Lowers a texture operation: what each register of the instruction's two
 * source vectors holds (nvrtx_sm75_texture_plan()), the vectors filled, the
 * instruction with the image's handle from constant buffer 0, and its
 * reply into two pairs that become the four results' registers (a level of
 * detail query's reply converted to floats first).
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/compiler/nak_nir_lower_tex.c
 * (lower_tex: the sources of the two vectors and the reply of
 * nir_texop_lod) and src/nouveau/compiler/nak/from_nir.rs (parse_tex: two
 * destination pairs from SM70 on).
 */
int
nvrtx_sm75_lower_sample(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	struct nvrtx_sm75_texture_plan plan;
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_binary *binary;
	uint32_t vectors[2];
	uint32_t first;
	uint32_t second;
	uint32_t texture;
	uint32_t index;
	uint32_t kind;
	int error;

	/* The result being built. */
	binary = lowering->binary;

	/* The image's texture handle: its binding's element. */
	texture = NVRTX_SM75_NO_SLOT;
	for (index = 0U; index < binary->texture_count; index++) {
		if (binary->textures[index].set == instruction->location &&
		    binary->textures[index].binding == instruction->immediate &&
		    binary->textures[index].element == instruction->element)
			texture = index;
	}

	/* An image the program does not use is refused. */
	if (texture == NVRTX_SM75_NO_SLOT) {
		error = nvrtx_sm75_refuse(lowering, EINVAL, "a sample names no sampled image");
		return error;
	}

	/* The results must be new values. */
	for (index = 0U; index < 4U; index++) {
		if (lowering->reg[instruction->dst + index] != NVRTX_SM75_NO_REG) {
			error = nvrtx_sm75_refuse(lowering, EINVAL, "a sample's result defined before");
			return error;
		}
	}

	/* What the instruction reads. */
	error = nvrtx_sm75_texture_plan(lowering, instruction, &plan);
	if (error != 0)
		return error;

	/* The two source vectors (RZ for an empty one). */
	vectors[0] = NVRTX_SM75_RZ;
	vectors[1] = NVRTX_SM75_RZ;
	error = nvrtx_sm75_texture_vector(lowering, plan.count[0], &vectors[0]);
	if (error == 0)
		error = nvrtx_sm75_texture_vector(lowering, plan.count[1], &vectors[1]);

	/* The results' two pairs. */
	first = NVRTX_SM75_RZ;
	second = NVRTX_SM75_RZ;
	if (error == 0)
		error = nvrtx_sm75_take_pair(lowering, &first);
	if (error == 0)
		error = nvrtx_sm75_take_pair(lowering, &second);

	/* Without them the registers taken go back. */
	if (error != 0) {
		nvrtx_sm75_texture_give(lowering, vectors[0], plan.count[0]);
		nvrtx_sm75_texture_give(lowering, vectors[1], plan.count[1]);
		nvrtx_sm75_texture_give(lowering, first, 2U);
		nvrtx_sm75_texture_give(lowering, second, 2U);
		return error;
	}

	/* The vectors' registers. */
	error = nvrtx_sm75_texture_fill(lowering, &plan, vectors);

	/* The instruction, writing the second pair only for more than two channels. */
	if (error == 0) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_texture(&word, plan.opcode, first, (plan.channels > 3U) ? second : NVRTX_SM75_RZ, vectors[0], vectors[1], plan.dim,
				   plan.channels, NVRTX_SM75_CB_ROOT, NVRTX_SM75_CB_TEXTURES + texture * 4U);
		if (plan.opcode == NVRTX_SM75_OP_TEX || plan.opcode == NVRTX_SM75_OP_TLD)
			nvrtx_sm75_tex_lod(&word, plan.lod_mode);
		if (plan.compare != 0)
			nvrtx_sm75_tex_compare(&word);
		if (plan.multisample != 0)
			nvrtx_sm75_tld_multisample(&word);
		if (plan.offset != 0)
			nvrtx_sm75_tex_offset(&word);
		if (plan.opcode == NVRTX_SM75_OP_TLD4)
			nvrtx_sm75_tld4_channel(&word, plan.channel);
		if (plan.opcode == NVRTX_SM75_OP_TXQ)
			nvrtx_sm75_set(&word, NVRTX_SM75_TXQ_QUERY_LOW, 2U, NVRTX_SM75_TXQ_DIMENSIONS);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	}

	/* The sources go back. */
	nvrtx_sm75_texture_give(lowering, vectors[0], plan.count[0]);
	nvrtx_sm75_texture_give(lowering, vectors[1], plan.count[1]);

	/* A level of detail query's reply becomes the two floats. */
	if (error == 0 && plan.lod_query != 0)
		error = nvrtx_sm75_texture_lod_reply(lowering, first);

	/*
	 * The results' registers are the four values': a level of detail
	 * query's first value is the second word of its reply.
	 */
	kind = 0U;
	if (plan.lod_query != 0)
		kind = 1U;
	lowering->reg[instruction->dst] = (uint16_t)(first + kind);
	lowering->reg[instruction->dst + 1U] = (uint16_t)(first + 1U - kind);
	lowering->reg[instruction->dst + 2U] = (uint16_t)second;
	lowering->reg[instruction->dst + 3U] = (uint16_t)(second + 1U);

	/* Reports an emission past the code's room. */
	if (error != 0)
		return error;

	/* Succeeded: the reply is in the results' registers. */
	return 0;
}

/*
 * Lowers a derivative across the pixel's 2x2 quad (lanes 0 and 1 the top
 * row, 2 and 3 the bottom one): a fine one is the butterfly of the lane
 * beside (x) or below (y) and FSWZADD subtracting each lane's way; a coarse
 * one is the quad's lane 1 (x) or 2 (y) minus its lane 0, for all four.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/compiler/nak/from_nir.rs
 * (nir_intrinsic_ddx and nir_intrinsic_ddy: SHFL.BFLY and FSWZADD).
 */
int
nvrtx_sm75_lower_derivative(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src b;
	uint32_t value;
	uint32_t lane;
	uint32_t ops;
	uint32_t scratch;
	uint32_t corner;
	int fine;
	int error;

	/* Only a fragment program has a quad. */
	if (lowering->ir->stage != DRV_GPU_STAGE_FRAGMENT) {
		error = nvrtx_sm75_refuse(lowering, ENOTSUP, "a derivative outside a fragment program");
		return error;
	}

	/* The value, the direction and the kind. */
	value = nvrtx_sm75_value(lowering, instruction->src[0]).value;
	lane = 1U;
	ops = NVRTX_SM75_FSWZADD_DDX;
	if (instruction->op == DRV_GPU_IR_DDY || instruction->op == DRV_GPU_IR_DDY_FINE) {
		lane = 2U;
		ops = NVRTX_SM75_FSWZADD_DDY;
	}
	fine = (instruction->op == DRV_GPU_IR_DDX_FINE || instruction->op == DRV_GPU_IR_DDY_FINE) ? 1 : 0;

	/* A register for the neighbour's value. */
	error = nvrtx_sm75_take(lowering, &scratch);
	if (error != 0)
		return error;

	/* Fine: the neighbour by a butterfly, then each lane's subtraction. */
	if (fine != 0) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_shfl(&word, scratch, value, lane, NVRTX_SM75_SHFL_BUTTERFLY);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
		if (error == 0) {
			nvrtx_sm75_begin(&word);
			nvrtx_sm75_fswzadd(&word, dst, scratch, value, ops);
			error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
		}
		nvrtx_sm75_give(lowering, scratch);
		return error;
	}

	/* Coarse: the quad's first lane and its neighbour, the same for all four. */
	error = nvrtx_sm75_take(lowering, &corner);
	if (error != 0) {
		nvrtx_sm75_give(lowering, scratch);
		return error;
	}
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_shfl(&word, corner, value, 0U, NVRTX_SM75_SHFL_INDEX);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	if (error == 0) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_shfl(&word, scratch, value, lane, NVRTX_SM75_SHFL_INDEX);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	}
	if (error == 0) {
		a = nvrtx_sm75_reg(scratch);
		b = nvrtx_sm75_reg(corner);
		b.neg = 1;
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_fadd(&word, dst, &a, &b);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}
	nvrtx_sm75_give(lowering, scratch);
	nvrtx_sm75_give(lowering, corner);
	if (error != 0)
		return error;

	/* Succeeded: the derivative is in dst. */
	return 0;
}

/*
 * Lowers the packing of two floats into the low and the high 16-bit half of
 * one word (two F2F to 16 bits, rounded to nearest, joined by PRMT), and
 * the unpacking of the float of one half (F2F from the low or the high
 * half).
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/compiler/nak/from_nir.rs
 * (nir_op_pack_half_2x16_split before SM86, nir_op_unpack_half_2x16_split_x
 * and _y).
 */
int
nvrtx_sm75_lower_half(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct nvrtx_sm75_word word;
	uint32_t high;
	int error;

	/* The unpacking: one conversion from the half the component names. */
	if (instruction->op == DRV_GPU_IR_UNPACK_HALF) {
		if (instruction->component > 1U) {
			error = nvrtx_sm75_refuse(lowering, EINVAL, "an unpacked half past the second");
			return error;
		}
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_f2f(&word, dst, nvrtx_sm75_value(lowering, instruction->src[0]).value, 0, (int)instruction->component);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
		return error;
	}

	/* The packing: the high half's register. */
	error = nvrtx_sm75_take(lowering, &high);
	if (error != 0)
		return error;

	/* The low half into dst, the high one into its register. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_f2f(&word, dst, nvrtx_sm75_value(lowering, instruction->src[0]).value, 1, 0);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	if (error == 0) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_f2f(&word, high, nvrtx_sm75_value(lowering, instruction->src[1]).value, 1, 0);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	}

	/* Bytes 0 and 1 of each. */
	if (error == 0) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_prmt(&word, dst, dst, NVRTX_SM75_PRMT_LOW_HALVES, high);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}
	nvrtx_sm75_give(lowering, high);
	if (error != 0)
		return error;

	/* Succeeded: the halves are packed in dst. */
	return 0;
}

/*
 * Lowers a discard of the fragments where the Boolean holds: @P0 KILL.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/from_nir.rs
 * (ShaderFromNir::parse_intrinsic: nir_intrinsic_demote and
 * nir_intrinsic_demote_if) and src/nouveau/compiler/nak/sph.rs
 * (encode_header: set_kills_pixels).
 */
int
nvrtx_sm75_lower_kill(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	struct nvrtx_sm75_word word;
	int error;

	/* Only a fragment program discards. */
	if (lowering->ir->stage != DRV_GPU_STAGE_FRAGMENT) {
		error = nvrtx_sm75_refuse(lowering, EINVAL, "a discard outside a fragment program");
		return error;
	}

	/* P0, then the KILL under it. */
	error = nvrtx_sm75_predicate(lowering, instruction->src[0]);
	if (error != 0)
		return error;

	/* @P0 KILL. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_kill(&word);
	nvrtx_sm75_guard(&word, NVRTX_SM75_P0, 0);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_CONTROL);
	if (error != 0)
		return error;

	/* Succeeded: the header says the program discards. */
	nvrtx_sm75_sph_set(lowering->binary->sph, NVRTX_SM75_SPH_KILLS_PIXELS, 1U, 1U);
	return 0;
}

/*
 * Completes a graphics program's header (version 4):
 * the type and version, the SASS version 1, a fragment program's
 * multiple-target bit and its input map's position w, the global memory
 * bits (a uniform buffer read through the storage table loads it), a
 * vertex or geometry program's store request, a geometry program's stream,
 * invocations, output primitive and most vertices.  The other maps were
 * set as the program was lowered.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sph.rs
 * (encode_header).
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sph.rs
 * (encode_header: the SASS version and the fields set).
 */
void
nvrtx_sm75_header(
	struct nvrtx_sm75_lowering *lowering)
{
	struct nvrtx_sm75_binary *binary;
	const struct drv_gpu_shader_ir *ir;
	uint32_t index;
	uint32_t topology;
	int global;
	int stores;

	/* The result and the program. */
	binary = lowering->binary;
	ir = lowering->ir;

	/* A compute program has none. */
	if (ir->stage == DRV_GPU_STAGE_COMPUTE)
		return;
	binary->graphics = 1U;

	/* The type, the version and the stage. */
	if (ir->stage == DRV_GPU_STAGE_FRAGMENT) {
		nvrtx_sm75_sph_set(binary->sph, NVRTX_SM75_SPH_TYPE_LOW, 5U, NVRTX_SM75_SPH_TYPE_PS);
		nvrtx_sm75_sph_set(binary->sph, NVRTX_SM75_SPH_SHADER_TYPE_LOW, 4U, NVRTX_SM75_SPH_PIXEL);
		nvrtx_sm75_sph_set(binary->sph, NVRTX_SM75_SPH_MRT_ENABLE, 1U, 1U);

		/*
		 * The position's w is mapped in even when the program does not
		 * read it, as a trap (a shader program header error) follows
		 * without it.
		 *
		 * Value referenced: Mesa 25.3.6
		 * src/nouveau/compiler/nak/from_nir.rs (init_info_from_nir: bit
		 * 31 of a fragment program's sysvals_in.ab, the position's w in
		 * its input map).
		 */
		nvrtx_sm75_sph_set(binary->sph, NVRTX_SM75_SPH_IMAP_AB_LOW + NVRTX_SM75_ATTR_POSITION_W / 4U, 1U, 1U);
	} else {
		nvrtx_sm75_sph_set(binary->sph, NVRTX_SM75_SPH_TYPE_LOW, 5U, NVRTX_SM75_SPH_TYPE_VTG);
		nvrtx_sm75_sph_set(binary->sph, NVRTX_SM75_SPH_SHADER_TYPE_LOW, 4U, NVRTX_SM75_SPH_VERTEX);
		nvrtx_sm75_sph_set(binary->sph, NVRTX_SM75_SPH_STORE_REQ_START_LOW, 8U, lowering->store_first);
		nvrtx_sm75_sph_set(binary->sph, NVRTX_SM75_SPH_STORE_REQ_END_LOW, 8U, lowering->store_last);
	}

	/*
	 * A geometry program: its type, stream 0, its invocations, the
	 * primitive it emits and its most vertices.
	 */
	if (ir->stage == DRV_GPU_STAGE_GEOMETRY) {
		nvrtx_sm75_sph_set(binary->sph, NVRTX_SM75_SPH_SHADER_TYPE_LOW, 4U, NVRTX_SM75_SPH_GEOMETRY);
		nvrtx_sm75_sph_set(binary->sph, NVRTX_SM75_SPH_STREAM_OUT_LOW, 4U, 1U);
		nvrtx_sm75_sph_set(binary->sph, NVRTX_SM75_SPH_THREADS_LOW, 8U, ir->invocations);
		topology = NVRTX_SM75_SPH_POINTLIST;
		if (ir->output_topology == DRV_GPU_IR_OUTPUT_LINE_STRIP)
			topology = NVRTX_SM75_SPH_LINESTRIP;
		if (ir->output_topology == DRV_GPU_IR_OUTPUT_TRIANGLE_STRIP)
			topology = NVRTX_SM75_SPH_TRIANGLESTRIP;
		nvrtx_sm75_sph_set(binary->sph, NVRTX_SM75_SPH_TOPOLOGY_LOW, 4U, topology);
		nvrtx_sm75_sph_set(binary->sph, NVRTX_SM75_SPH_MAX_VERTICES_LOW, 12U, ir->max_vertices);
	}

	/* The header's version and the SASS version. */
	nvrtx_sm75_sph_set(binary->sph, NVRTX_SM75_SPH_VERSION_LOW, 5U, NVRTX_SM75_SPH_VERSION);
	nvrtx_sm75_sph_set(binary->sph, NVRTX_SM75_SPH_SASS_VERSION_LOW, 4U, 1U);

	/* Whether it loads or stores global memory. */
	global = 0;
	stores = 0;
	for (index = 0U; index < ir->instruction_count; index++) {
		if (ir->instructions[index].op == DRV_GPU_IR_LOAD_STORAGE)
			global = 1;
		if (ir->instructions[index].op == DRV_GPU_IR_LOAD_UBO &&
		    ir->instructions[index].location < ir->uniform_count &&
		    (lowering->slot[ir->instructions[index].location] & NVRTX_SM75_SLOT_TABLE) != 0U)
			global = 1;
		if (ir->instructions[index].op == DRV_GPU_IR_STORE_STORAGE || ir->instructions[index].op == DRV_GPU_IR_ATOMIC) {
			global = 1;
			stores = 1;
		}
	}

	/* The header's two global memory bits. */
	nvrtx_sm75_sph_set(binary->sph, NVRTX_SM75_SPH_LOAD_OR_STORE, 1U, (uint32_t)global);
	nvrtx_sm75_sph_set(binary->sph, NVRTX_SM75_SPH_GLOBAL_STORE, 1U, (uint32_t)stores);
}

/*
 * Reports nonzero for the texture operations, which define four results.
 */
int
nvrtx_sm75_is_sample(
	enum drv_gpu_shader_ir_op op)
{
	/* The implicit, the explicit and the biased level of detail. */
	if (op == DRV_GPU_IR_SAMPLE)
		return 1;
	if (op == DRV_GPU_IR_SAMPLE_LOD)
		return 1;
	if (op == DRV_GPU_IR_SAMPLE_BIAS)
		return 1;

	/* The general sampler message. */
	if (op == DRV_GPU_IR_TEXTURE)
		return 1;

	/* Succeeded: another operation. */
	return 0;
}

/*
 * Plans what a texture operation's instruction reads (isa.h,
 * NVRTX_SM75_TEX_VECTOR): from a 2D sample of its own operation, or from
 * the general message, its parameters in the order the message takes them
 * (ir.h) and its image's shape.  A sample is TEX (with the reference of a
 * compare, mask the one channel), a sample with derivatives TXD, a fetch
 * TLD (its layer an integer), a gather TLD4, a level of detail query TMML
 * (layer zero, its two channels converted after), a size query TXQ.  A
 * float layer is rounded; a cube direction is divided by its largest
 * magnitude.  An implicit level of detail outside a fragment program is
 * level zero.  The texel offset is packed four bits a component (u lowest),
 * eight for a gather.  A texel buffer is fetched (TLD of its one
 * coordinate at level zero) and its size queried as a 1D image's.  A
 * sample of a multisampled 2D image is fetched (TLD.MS at level zero, the
 * sample first in the second vector), its size queried in pixels.  Another message of a texel buffer
 * or of a multisampled image, a multisampled array, and derivatives
 * of a 3D or cube image or with a compare (which the hardware does not
 * take) are refused.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/compiler/nak_nir_lower_tex.c
 * (lower_tex: the array index first, +0.5 and clamped for all but a fetch,
 * zero for nir_texop_lod; the offsets packed 4 bits a component, 8 for
 * nir_texop_tg4; TXD's offset above its layer by PRMT 0x1054),
 * src/nouveau/compiler/nak/from_nir.rs (GLSL_SAMPLER_DIM_BUF is
 * TexDim::_1D, GLSL_SAMPLER_DIM_MS is TexDim::_2D, is_ms for
 * nir_texop_txf_ms), src/nouveau/compiler/nak_nir_lower_tex.c (the
 * ms_index pushed first to the second vector, level zero) and
 * src/nouveau/compiler/nak_nir.c (nak_preprocess_nir:
 * nir_lower_tex with lower_txd_3d, lower_txd_cube_map and
 * lower_txd_shadow, and nir_normalize_cubemap_coords).
 */
static int
nvrtx_sm75_texture_plan(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	struct nvrtx_sm75_texture_plan *plan)
{
	uint32_t params[DRV_GPU_IR_TEXTURE_MAX_PARAMS];
	uint32_t coordinates[3];
	uint32_t derivatives[6];
	uint32_t message;
	uint32_t shape;
	uint32_t kind;
	uint32_t count;
	uint32_t next;
	uint32_t dims;
	uint32_t layer;
	uint32_t reference;
	uint32_t sample;
	uint32_t lod;
	uint32_t raw;
	uint32_t bits;
	uint32_t index;
	int arrayed;
	int error;

	/* Nothing yet. */
	plan->opcode = NVRTX_SM75_OP_TEX;
	plan->lod_mode = NVRTX_SM75_TEX_LOD_AUTO;
	plan->channels = 0xfU;
	plan->channel = 0U;
	plan->compare = 0;
	plan->multisample = 0;
	plan->offset = 0;
	plan->lod_query = 0;
	plan->count[0] = 0U;
	plan->count[1] = 0U;
	for (index = 0U; index < DRV_GPU_IR_TEXTURE_MAX_PARAMS; index++)
		params[index] = NVRTX_SM75_NOT_DEFINED;

	/* The 2D samples of their own operations: u, v, then the level or the bias; their offset u in bits 11..8, v in 7..4. */
	if (instruction->op != DRV_GPU_IR_TEXTURE) {
		message = DRV_GPU_IR_TEXTURE_SAMPLE;
		params[0] = instruction->src[0];
		params[1] = instruction->src[1];
		if (instruction->op != DRV_GPU_IR_SAMPLE) {
			message = (instruction->op == DRV_GPU_IR_SAMPLE_LOD) ? DRV_GPU_IR_TEXTURE_SAMPLE_LOD : DRV_GPU_IR_TEXTURE_SAMPLE_BIAS;
			params[0] = instruction->src[2];
			params[1] = instruction->src[0];
			params[2] = instruction->src[1];
		}
		shape = DRV_GPU_IR_IMAGE_2D;
		raw = instruction->component & 0xff0U;
	} else {
		/* The general message: src[1] parameters from src[0], the rest zero; its offset shifted by 8. */
		message = instruction->component & DRV_GPU_IR_TEXTURE_MESSAGE_MASK;
		count = instruction->src[1];
		if (count > DRV_GPU_IR_TEXTURE_MAX_PARAMS) {
			error = nvrtx_sm75_refuse(lowering, EINVAL, "a texture message with too many parameters");
			return error;
		}
		for (index = 0U; index < count; index++)
			params[index] = instruction->src[0] + index;
		shape = instruction->image_shape;
		raw = (instruction->component >> 8) & 0xfffU;
		plan->channel = DRV_GPU_IR_TEXTURE_CHANNEL(instruction->component);
	}

	/* The image's kind: 2D, 3D, cube or a texel buffer, an array or not. */
	kind = shape & DRV_GPU_IR_IMAGE_KIND_MASK;
	arrayed = ((shape & DRV_GPU_IR_IMAGE_ARRAYED) != 0U) ? 1 : 0;
	if (kind > DRV_GPU_IR_IMAGE_BUFFER) {
		error = nvrtx_sm75_refuse(lowering, ENOTSUP, "an image of a kind the compiler does not sample");
		return error;
	}
	if ((shape & DRV_GPU_IR_IMAGE_MULTISAMPLED) != 0U &&
	    (kind != DRV_GPU_IR_IMAGE_2D ||
	     arrayed != 0 ||
	     (message != DRV_GPU_IR_TEXTURE_LD && message != DRV_GPU_IR_TEXTURE_RESINFO))) {
		error = nvrtx_sm75_refuse(lowering, ENOTSUP, "a multisampled image read other than by a fetch or a size query of a 2D one");
		return error;
	}
	if ((shape & DRV_GPU_IR_IMAGE_MULTISAMPLED) != 0U && message == DRV_GPU_IR_TEXTURE_LD)
		plan->multisample = 1;
	if (kind == DRV_GPU_IR_IMAGE_BUFFER &&
	    (arrayed != 0 ||
	     (message != DRV_GPU_IR_TEXTURE_LD && message != DRV_GPU_IR_TEXTURE_RESINFO))) {
		error = nvrtx_sm75_refuse(lowering, ENOTSUP, "a texel buffer read other than by a fetch or a size query");
		return error;
	}
	dims = 2U;
	plan->dim = NVRTX_SM75_TEX_DIM_2D;
	if (kind == DRV_GPU_IR_IMAGE_BUFFER) {
		dims = 1U;
		plan->dim = NVRTX_SM75_TEX_DIM_1D;
	} else if (kind == DRV_GPU_IR_IMAGE_3D) {
		dims = 3U;
		plan->dim = NVRTX_SM75_TEX_DIM_3D;
	} else if (kind == DRV_GPU_IR_IMAGE_CUBE) {
		dims = 3U;
		plan->dim = NVRTX_SM75_TEX_DIM_CUBE;
	}
	if (arrayed != 0)
		plan->dim |= NVRTX_SM75_TEX_DIM_ARRAY;

	/* The reference of a compare, then the level or the bias, by the message. */
	next = 0U;
	reference = NVRTX_SM75_NOT_DEFINED;
	lod = NVRTX_SM75_NOT_DEFINED;
	switch (message) {
	case DRV_GPU_IR_TEXTURE_SAMPLE_COMPARE:
	case DRV_GPU_IR_TEXTURE_SAMPLE_BIAS_COMPARE:
	case DRV_GPU_IR_TEXTURE_SAMPLE_LOD_COMPARE:
	case DRV_GPU_IR_TEXTURE_GATHER4_COMPARE:
		reference = params[next];
		next++;
		break;
	case DRV_GPU_IR_TEXTURE_SAMPLE:
	case DRV_GPU_IR_TEXTURE_SAMPLE_BIAS:
	case DRV_GPU_IR_TEXTURE_SAMPLE_LOD:
	case DRV_GPU_IR_TEXTURE_SAMPLE_DERIVS:
	case DRV_GPU_IR_TEXTURE_LD:
	case DRV_GPU_IR_TEXTURE_GATHER4:
	case DRV_GPU_IR_TEXTURE_LOD:
	case DRV_GPU_IR_TEXTURE_RESINFO:
		break;
	default:
		error = nvrtx_sm75_refuse(lowering, ENOTSUP, "a texture message the compiler does not lower (derivatives with a compare, the sample count)");
		return error;
	}
	if (message == DRV_GPU_IR_TEXTURE_SAMPLE_BIAS || message == DRV_GPU_IR_TEXTURE_SAMPLE_BIAS_COMPARE) {
		lod = params[next];
		next++;
		plan->lod_mode = NVRTX_SM75_TEX_LOD_BIAS;
	}
	if (message == DRV_GPU_IR_TEXTURE_SAMPLE_LOD || message == DRV_GPU_IR_TEXTURE_SAMPLE_LOD_COMPARE) {
		lod = params[next];
		next++;
		plan->lod_mode = NVRTX_SM75_TEX_LOD_EXPLICIT;
	}

	/* A size query: TXQ of the level. */
	if (message == DRV_GPU_IR_TEXTURE_RESINFO) {
		plan->opcode = NVRTX_SM75_OP_TXQ;
		nvrtx_sm75_texture_part(plan, 0U, NVRTX_SM75_PART_VALUE, params[0], 0U);
		return 0;
	}

	/* Derivatives: of a 2D image without a compare, each coordinate followed by its two. */
	if (message == DRV_GPU_IR_TEXTURE_SAMPLE_DERIVS) {
		if (kind != DRV_GPU_IR_IMAGE_2D) {
			error = nvrtx_sm75_refuse(lowering, ENOTSUP, "a sample with derivatives of a 3D or cube image, which the hardware does not take");
			return error;
		}
		for (index = 0U; index < 2U; index++) {
			coordinates[index] = params[next];
			derivatives[index * 2U] = params[next + 1U];
			derivatives[index * 2U + 1U] = params[next + 2U];
			next += 3U;
		}
		layer = (arrayed != 0) ? params[next] : NVRTX_SM75_NOT_DEFINED;

		/* TXD: u, v, then the offset with the layer; du/dx, du/dy, dv/dx, dv/dy. */
		plan->opcode = NVRTX_SM75_OP_TXD;
		for (index = 0U; index < 2U; index++)
			nvrtx_sm75_texture_part(plan, 0U, NVRTX_SM75_PART_VALUE, coordinates[index], 0U);
		bits = ((raw >> 8) & 0xfU) | (raw & 0xf0U);
		plan->offset = (bits != 0U) ? 1 : 0;
		if (arrayed != 0 || bits != 0U)
			nvrtx_sm75_texture_part(plan, 0U, NVRTX_SM75_PART_LAYER_OFFSET, layer, bits << 16);
		for (index = 0U; index < 4U; index++)
			nvrtx_sm75_texture_part(plan, 1U, NVRTX_SM75_PART_VALUE, derivatives[index], 0U);
		return 0;
	}

	/* The coordinates, then the layer of an array (none for a level of detail query); a fetch's level after v. */
	for (index = 0U; index < dims; index++) {
		coordinates[index] = params[next];
		next++;
		if (message == DRV_GPU_IR_TEXTURE_LD && index == 1U) {
			lod = params[next];
			next++;
		}
	}
	layer = NVRTX_SM75_NOT_DEFINED;
	if (arrayed != 0 && message != DRV_GPU_IR_TEXTURE_LOD)
		layer = params[next];

	/* A multisampled image's sample comes where a layer would (the frontend's r). */
	sample = NVRTX_SM75_NOT_DEFINED;
	if (plan->multisample != 0)
		sample = params[next];

	/* The instruction by the message: a texel buffer's fetch is at level zero (TLD.LZ, no level register). */
	if (message == DRV_GPU_IR_TEXTURE_LD) {
		plan->opcode = NVRTX_SM75_OP_TLD;
		plan->lod_mode = NVRTX_SM75_TEX_LOD_EXPLICIT;
		if (kind == DRV_GPU_IR_IMAGE_BUFFER || plan->multisample != 0)
			plan->lod_mode = NVRTX_SM75_TEX_LOD_ZERO;
	} else if (message == DRV_GPU_IR_TEXTURE_GATHER4 || message == DRV_GPU_IR_TEXTURE_GATHER4_COMPARE) {
		plan->opcode = NVRTX_SM75_OP_TLD4;
		if (message == DRV_GPU_IR_TEXTURE_GATHER4_COMPARE)
			plan->channel = 0U;
	} else if (message == DRV_GPU_IR_TEXTURE_LOD) {
		plan->opcode = NVRTX_SM75_OP_TMML;
		plan->channels = 0x3U;
		plan->lod_query = 1;
		if (lowering->ir->stage != DRV_GPU_STAGE_FRAGMENT) {
			error = nvrtx_sm75_refuse(lowering, EINVAL, "a level of detail query outside a fragment program");
			return error;
		}
	}

	/* An implicit level of detail outside a fragment program is level zero. */
	if (plan->opcode == NVRTX_SM75_OP_TEX && plan->lod_mode == NVRTX_SM75_TEX_LOD_AUTO && lowering->ir->stage != DRV_GPU_STAGE_FRAGMENT)
		plan->lod_mode = NVRTX_SM75_TEX_LOD_ZERO;

	/* A compare: its reference, and a sample's one channel. */
	if (reference != NVRTX_SM75_NOT_DEFINED) {
		plan->compare = 1;
		if (plan->opcode == NVRTX_SM75_OP_TEX)
			plan->channels = 0x1U;
	}

	/* The first vector: the layer (an integer for a fetch, zero for a level of detail query), then the coordinates. */
	if (arrayed != 0) {
		if (plan->opcode == NVRTX_SM75_OP_TLD)
			nvrtx_sm75_texture_part(plan, 0U, NVRTX_SM75_PART_VALUE, layer, 0U);
		else if (plan->opcode == NVRTX_SM75_OP_TMML)
			nvrtx_sm75_texture_part(plan, 0U, NVRTX_SM75_PART_ZERO, NVRTX_SM75_NOT_DEFINED, 0U);
		else
			nvrtx_sm75_texture_part(plan, 0U, NVRTX_SM75_PART_LAYER, layer, 0U);
	}
	for (index = 0U; index < dims; index++) {
		if (kind == DRV_GPU_IR_IMAGE_CUBE)
			nvrtx_sm75_texture_part(plan, 0U, NVRTX_SM75_PART_CUBE, coordinates[index], index);
		else
			nvrtx_sm75_texture_part(plan, 0U, NVRTX_SM75_PART_VALUE, coordinates[index], 0U);
	}

	/* The second vector: a multisampled fetch's sample, then the level or the bias of a TEX or a TLD. */
	if (plan->multisample != 0)
		nvrtx_sm75_texture_part(plan, 1U, NVRTX_SM75_PART_VALUE, sample, 0U);
	if ((plan->opcode == NVRTX_SM75_OP_TEX || plan->opcode == NVRTX_SM75_OP_TLD) &&
	    (plan->lod_mode == NVRTX_SM75_TEX_LOD_BIAS || plan->lod_mode == NVRTX_SM75_TEX_LOD_EXPLICIT))
		nvrtx_sm75_texture_part(plan, 1U, NVRTX_SM75_PART_VALUE, lod, 0U);

	/* The constant offset: u, v and r in 3..0, 7..4, 11..8, or a gather's u and v in 7..0 and 15..8. */
	if (raw != 0U && plan->opcode != NVRTX_SM75_OP_TMML) {
		if (plan->opcode == NVRTX_SM75_OP_TLD4) {
			bits = (((uint32_t)(((int32_t)(raw << 20)) >> 28)) & 0xffU) |
			       ((((uint32_t)(((int32_t)(raw << 24)) >> 28)) & 0xffU) << 8);
		} else {
			bits = ((raw >> 8) & 0xfU) | (raw & 0xf0U) | ((raw & 0xfU) << 8);
		}
		plan->offset = 1;
		nvrtx_sm75_texture_part(plan, 1U, NVRTX_SM75_PART_IMMEDIATE, NVRTX_SM75_NOT_DEFINED, bits);
	}

	/* The reference last. */
	if (reference != NVRTX_SM75_NOT_DEFINED)
		nvrtx_sm75_texture_part(plan, 1U, NVRTX_SM75_PART_VALUE, reference, 0U);

	/* Succeeded: the instruction is planned. */
	return 0;
}

/* Appends one register's part to a source vector of a plan; a value not passed is zero. */
static void
nvrtx_sm75_texture_part(
	struct nvrtx_sm75_texture_plan *plan,
	uint32_t vector,
	uint32_t kind,
	uint32_t value,
	uint32_t bits)
{
	struct nvrtx_sm75_texture_part *part;

	/* The next register of the vector (the plans never pass four). */
	if (plan->count[vector] >= NVRTX_SM75_TEX_VECTOR)
		return;
	part = &plan->parts[vector][plan->count[vector]];
	plan->count[vector]++;

	/* A value or a cube coordinate the message did not pass reads as zero. */
	if ((kind == NVRTX_SM75_PART_VALUE || kind == NVRTX_SM75_PART_CUBE || kind == NVRTX_SM75_PART_LAYER) && value == NVRTX_SM75_NOT_DEFINED)
		kind = NVRTX_SM75_PART_ZERO;
	part->kind = kind;
	part->value = value;
	part->bits = bits;
}

/* Takes a source vector of count registers (one, an even pair, or four from a multiple of four); RZ for none. */
static int
nvrtx_sm75_texture_vector(
	struct nvrtx_sm75_lowering *lowering,
	uint32_t count,
	uint32_t *reg)
{
	int error;

	/* By its size. */
	*reg = NVRTX_SM75_RZ;
	if (count == 0U)
		return 0;
	if (count == 1U) {
		error = nvrtx_sm75_take(lowering, reg);
		return error;
	}
	if (count == 2U) {
		error = nvrtx_sm75_take_pair(lowering, reg);
		return error;
	}
	error = nvrtx_sm75_take_quad(lowering, reg);
	if (error != 0)
		return error;

	/* Three of four: the last goes back. */
	if (count == 3U)
		nvrtx_sm75_give(lowering, *reg + 3U);

	/* Succeeded: the vector is the caller's. */
	return 0;
}

/* Gives a vector of count registers back (none for RZ). */
static void
nvrtx_sm75_texture_give(
	struct nvrtx_sm75_lowering *lowering,
	uint32_t reg,
	uint32_t count)
{
	uint32_t index;

	/* Each register of it. */
	if (reg == NVRTX_SM75_RZ)
		return;
	for (index = 0U; index < count; index++)
		nvrtx_sm75_give(lowering, reg + index);
}

/* Fills the registers of a plan's source vectors: the cube coordinates first, then each other part. */
static int
nvrtx_sm75_texture_fill(
	struct nvrtx_sm75_lowering *lowering,
	const struct nvrtx_sm75_texture_plan *plan,
	const uint32_t *vectors)
{
	const struct nvrtx_sm75_texture_part *part;
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src source;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src b;
	struct nvrtx_sm75_src zero;
	uint32_t vector;
	uint32_t index;
	uint32_t reg;
	int error;

	/* The cube direction divided by its largest magnitude. */
	error = nvrtx_sm75_texture_cube(lowering, plan, vectors);
	if (error != 0)
		return error;

	/* Each other register. */
	zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);
	for (vector = 0U; vector < 2U; vector++) {
		for (index = 0U; index < plan->count[vector]; index++) {
			part = &plan->parts[vector][index];
			reg = vectors[vector] + index;
			switch (part->kind) {
			case NVRTX_SM75_PART_VALUE:
				source = nvrtx_sm75_value(lowering, part->value);
				error = nvrtx_sm75_emit_move(lowering, reg, &source);
				break;
			case NVRTX_SM75_PART_ZERO:
				error = nvrtx_sm75_emit_move(lowering, reg, &zero);
				break;
			case NVRTX_SM75_PART_IMMEDIATE:
				source = nvrtx_sm75_imm(part->bits);
				error = nvrtx_sm75_emit_move(lowering, reg, &source);
				break;
			case NVRTX_SM75_PART_LAYER:
				error = nvrtx_sm75_texture_layer(lowering, reg, part->value);
				break;
			case NVRTX_SM75_PART_LAYER_OFFSET:
				/* The layer (or zero), then the offset's bits added above it. */
				if (part->value == NVRTX_SM75_NOT_DEFINED) {
					source = nvrtx_sm75_imm(part->bits);
					error = nvrtx_sm75_emit_move(lowering, reg, &source);
					break;
				}
				error = nvrtx_sm75_texture_layer(lowering, reg, part->value);
				if (error != 0 || part->bits == 0U)
					break;
				a = nvrtx_sm75_reg(reg);
				b = nvrtx_sm75_imm(part->bits);
				nvrtx_sm75_begin(&word);
				nvrtx_sm75_iadd3(&word, reg, &a, &b, &zero);
				error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
				break;
			default:
				/* The cube coordinates are in place. */
				error = 0;
				break;
			}
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the vectors hold what the instruction reads. */
	return 0;
}

/*
 * Divides a cube direction by its largest magnitude into the registers of
 * its parts: the largest of |x|, |y| and |z| (FMNMX), its reciprocal, each
 * component times it.
 *
 * Procedure referenced: Mesa 25.0.7 src/compiler/nir/nir_normalize_cubemap_coords.c
 * (normalize_cubemap_coords: the coordinate times the reciprocal of the
 * largest magnitude).
 */
static int
nvrtx_sm75_texture_cube(
	struct nvrtx_sm75_lowering *lowering,
	const struct nvrtx_sm75_texture_plan *plan,
	const uint32_t *vectors)
{
	const struct nvrtx_sm75_texture_part *part;
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src b;
	uint32_t places[3];
	uint32_t values[3];
	uint32_t found;
	uint32_t index;
	uint32_t largest;
	int error;

	/* The direction's parts, in its order. */
	found = 0U;
	for (index = 0U; index < plan->count[0]; index++) {
		part = &plan->parts[0][index];
		if (part->kind != NVRTX_SM75_PART_CUBE || part->bits > 2U)
			continue;
		places[part->bits] = vectors[0] + index;
		values[part->bits] = nvrtx_sm75_value(lowering, part->value).value;
		found++;
	}

	/* None, or a direction with a part the message did not pass (zero), which the hardware takes as it is. */
	if (found != 3U) {
		for (index = 0U; index < plan->count[0]; index++) {
			part = &plan->parts[0][index];
			if (part->kind != NVRTX_SM75_PART_CUBE)
				continue;
			a = nvrtx_sm75_value(lowering, part->value);
			error = nvrtx_sm75_emit_move(lowering, vectors[0] + index, &a);
			if (error != 0)
				return error;
		}
		return 0;
	}

	/* A register for the largest magnitude, then its reciprocal. */
	error = nvrtx_sm75_take(lowering, &largest);
	if (error != 0)
		return error;
	a = nvrtx_sm75_reg(values[0]);
	a.abs = 1;
	b = nvrtx_sm75_reg(values[1]);
	b.abs = 1;
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_fmnmx(&word, largest, &a, &b, 0);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	if (error == 0) {
		a = nvrtx_sm75_reg(largest);
		b = nvrtx_sm75_reg(values[2]);
		b.abs = 1;
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_fmnmx(&word, largest, &a, &b, 0);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}
	if (error == 0) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_mufu(&word, largest, NVRTX_SM75_MUFU_RCP, largest);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	}

	/* Each component times the reciprocal, into its place. */
	for (index = 0U; index < 3U && error == 0; index++) {
		a = nvrtx_sm75_reg(values[index]);
		b = nvrtx_sm75_reg(largest);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_fmul(&word, places[index], &a, &b);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}
	nvrtx_sm75_give(lowering, largest);
	if (error != 0)
		return error;

	/* Succeeded: the direction is divided in place. */
	return 0;
}

/*
 * Makes a float layer the integer a texture instruction takes, in reg: plus
 * one half, converted to an unsigned integer toward zero (a negative one is
 * zero), and at most 65535.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/compiler/nak_nir_lower_tex.c
 * (lower_tex: fadd 0.5, f2u32, umin UINT16_MAX).
 */
static int
nvrtx_sm75_texture_layer(
	struct nvrtx_sm75_lowering *lowering,
	uint32_t reg,
	uint32_t value)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src b;
	int error;

	/* Plus one half. */
	a = nvrtx_sm75_value(lowering, value);
	b = nvrtx_sm75_imm(0x3f000000U);
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_fadd(&word, reg, &a, &b);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	if (error != 0)
		return error;

	/* To an unsigned integer. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_f2i(&word, reg, reg, 0);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	if (error != 0)
		return error;

	/* At most 65535. */
	a = nvrtx_sm75_reg(reg);
	b = nvrtx_sm75_imm(NVRTX_SM75_LAYER_MAX);
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_isetp(&word, NVRTX_SM75_P0, NVRTX_SM75_ICMP_LT, 0, &a, &b);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED_PREDICATE);
	if (error != 0)
		return error;
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_sel(&word, reg, &a, &b, NVRTX_SM75_P0, 0);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	if (error != 0)
		return error;

	/* Succeeded: the layer is an integer in reg. */
	return 0;
}

/*
 * Converts a TMML's reply in the pair first into the two floats: the
 * second word's unsigned 16 bits and the first word's signed 16 bits, each
 * divided by 256.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/compiler/nak_nir_lower_tex.c
 * (lower_tex: u2f32 of the low 16 bits of the second, i2f32 of the
 * sign-extended low 16 bits of the first, times 1/256).
 */
static int
nvrtx_sm75_texture_lod_reply(
	struct nvrtx_sm75_lowering *lowering,
	uint32_t first)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src b;
	struct nvrtx_sm75_src zero;
	struct nvrtx_sm75_src sixteen;
	struct nvrtx_sm75_src fraction;
	int error;

	/* The operands. */
	zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);
	sixteen = nvrtx_sm75_imm(16U);
	fraction = nvrtx_sm75_imm(0x3b800000U);

	/* The level accessed: the low 16 bits of the second word, unsigned. */
	a = nvrtx_sm75_reg(first + 1U);
	b = nvrtx_sm75_imm(0xffffU);
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_lop3(&word, first + 1U, &a, &b, &zero, NVRTX_SM75_LUT_AND);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	if (error == 0) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_i2f(&word, first + 1U, first + 1U, 0);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	}
	if (error == 0) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_fmul(&word, first + 1U, &a, &fraction);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}

	/* The level computed: the low 16 bits of the first word, signed. */
	if (error == 0) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_shift(&word, first, first, &sixteen, 0, 0);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}
	if (error == 0) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_shift(&word, first, first, &sixteen, 1, 1);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}
	if (error == 0) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_i2f(&word, first, first, 1);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	}
	if (error == 0) {
		a = nvrtx_sm75_reg(first);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_fmul(&word, first, &a, &fraction);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}
	if (error != 0)
		return error;

	/* Succeeded: the pair holds the two floats. */
	return 0;
}

/* Appends a MOV of a source into a register. */
static int
nvrtx_sm75_emit_move(
	struct nvrtx_sm75_lowering *lowering,
	uint32_t dst,
	const struct nvrtx_sm75_src *src)
{
	struct nvrtx_sm75_word word;
	int error;

	/* MOV. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_mov(&word, dst, src);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	if (error != 0)
		return error;

	/* Succeeded: the register holds the source. */
	return 0;
}

/* Finds a fragment program's located input, or NULL. */
static const struct drv_gpu_shader_ir_io *
nvrtx_sm75_input(
	const struct drv_gpu_shader_ir *ir,
	uint32_t location)
{
	uint32_t index;

	/* The input at the location. */
	for (index = 0U; index < ir->input_count; index++) {
		if (ir->inputs[index].location == location)
			return &ir->inputs[index];
	}

	/* Succeeded in finding none. */
	return NULL;
}

/* Sets a field of the header. */
static void
nvrtx_sm75_sph_set(
	uint32_t *sph,
	unsigned low,
	unsigned bits,
	uint32_t value)
{
	unsigned bit;
	uint32_t mask;

	/* Each bit of the field into its word. */
	for (bit = 0U; bit < bits; bit++) {
		mask = 1U << ((low + bit) % 32U);
		if (((value >> bit) & 1U) != 0U) {
			sph[(low + bit) / 32U] |= mask;
		} else {
			sph[(low + bit) / 32U] &= ~mask;
		}
	}
}

/*
 * Widens a vertex program's store request to an attribute word's address.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/ir.rs
 * (VtgIoInfo::mark_store_req: the first and the last word, by address / 4).
 */
static void
nvrtx_sm75_mark_store(
	struct nvrtx_sm75_lowering *lowering,
	uint32_t address)
{
	/* The first and the last word stored. */
	if (address / 4U < lowering->store_first)
		lowering->store_first = address / 4U;
	if (address / 4U > lowering->store_last)
		lowering->store_last = address / 4U;
}

/*
 * Lowers a pixel program's system input: the face (the constant word of
 * its attribute, all ones when it is not zero, as the IR's Booleans are),
 * the covered samples' mask (PIXLD), the point sprite's coordinate
 * (screen-linear, in the C part of the input map) or the primitive's
 * number (constant, in the A and B part).
 *
 * Procedure referenced: Mesa 25.3.6
 * src/nouveau/compiler/nak_nir_lower_fs_inputs.c (lower_fs_input_intrin:
 * load_point_coord, load_front_face) and src/nouveau/compiler/nak/
 * from_nir.rs (load_sample_mask_in).
 */
static int
nvrtx_sm75_lower_pixel_input(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src zero;
	struct nvrtx_sm75_src ones;
	uint32_t *sph;
	uint32_t address;
	int error;

	/* The header's maps. */
	sph = lowering->binary->sph;

	/* The samples covered: PIXLD. */
	if (instruction->location == DRV_GPU_SHADER_LOCATION_SAMPLE_MASK) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_pixld(&word, dst, NVRTX_SM75_PIXLD_COVERAGE);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_SPECIAL);
		return error;
	}

	/* The point's coordinate: passed screen-linear. */
	if (instruction->location == DRV_GPU_SHADER_LOCATION_POINT_COORD) {
		if (instruction->component > 1U) {
			error = nvrtx_sm75_refuse(lowering, EINVAL, "a point coordinate component past the second");
			return error;
		}
		address = NVRTX_SM75_ATTR_POINT_SPRITE + instruction->component * 4U;
		nvrtx_sm75_sph_set(sph, NVRTX_SM75_SPH_IMAP_C_PS_LOW + (address - NVRTX_SM75_ATTR_SYSTEM_C) / 4U, 1U, 1U);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_ipa(&word, dst, address, NVRTX_SM75_IPA_PASS);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
		return error;
	}

	/* The primitive's number: constant. */
	if (instruction->location == DRV_GPU_SHADER_LOCATION_PRIMITIVE_ID) {
		nvrtx_sm75_sph_set(sph, NVRTX_SM75_SPH_IMAP_AB_LOW + NVRTX_SM75_ATTR_PRIMITIVE_ID / 4U, 1U, 1U);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_ipa(&word, dst, NVRTX_SM75_ATTR_PRIMITIVE_ID, NVRTX_SM75_IPA_CONSTANT);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
		return error;
	}

	/* The face: its word, constant, then all ones when it is not zero. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_ipa(&word, dst, NVRTX_SM75_ATTR_FRONT_FACE, NVRTX_SM75_IPA_CONSTANT);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	if (error != 0)
		return error;
	a = nvrtx_sm75_reg(dst);
	zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_isetp(&word, NVRTX_SM75_P0, NVRTX_SM75_ICMP_NE, 0, &a, &zero);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED_PREDICATE);
	if (error != 0)
		return error;
	ones = nvrtx_sm75_imm(0xffffffffU);
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_sel(&word, dst, &zero, &ones, NVRTX_SM75_P0, 1);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	return error;
}

/*
 * Lowers a vertex program's gl_InstanceIndex: the instance number (ALD of
 * its attribute, from 0 in each draw) plus the draw's first instance from
 * constant buffer 0.
 */
static int
nvrtx_sm75_lower_instance_index(
	struct nvrtx_sm75_lowering *lowering,
	uint32_t dst)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src number;
	struct nvrtx_sm75_src first;
	struct nvrtx_sm75_src zero;
	int error;

	/* The instance number. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_ald(&word, dst, NVRTX_SM75_ATTR_INSTANCE_ID);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	if (error != 0)
		return error;

	/* Plus the first instance. */
	number = nvrtx_sm75_reg(dst);
	first = nvrtx_sm75_cb(NVRTX_SM75_CB_ROOT, NVRTX_SM75_CB_BASE_INSTANCE);
	zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_iadd3(&word, dst, &number, &first, &zero);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	return error;
}

/*
 * Finds a vertex or geometry program's output attribute word: its address
 * (the position, the point size, a generic word; a geometry program's layer
 * and primitive number too), its bit of the header's output map and the
 * store request.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/ir.rs
 * (VtgIoInfo::mark_attrs: a bit of the output map for each word, the
 * system values below 0x080 and the generic words from 0x080).
 */
static int
nvrtx_sm75_vtg_output(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t *address)
{
	uint32_t *sph;
	int geometry;
	int error;

	/* The header's maps; a geometry program's own outputs. */
	sph = lowering->binary->sph;
	geometry = 0;
	if (lowering->ir->stage == DRV_GPU_STAGE_GEOMETRY)
		geometry = 1;

	/* The word by its location. */
	if (instruction->location == DRV_GPU_IR_LOCATION_POSITION) {
		*address = NVRTX_SM75_ATTR_POSITION + instruction->component * 4U;
	} else if (instruction->location == DRV_GPU_IR_LOCATION_POINT_SIZE) {
		*address = NVRTX_SM75_ATTR_POINT_SIZE;
	} else if (geometry != 0 && instruction->location == DRV_GPU_IR_LOCATION_LAYER) {
		*address = NVRTX_SM75_ATTR_LAYER;
	} else if (geometry != 0 && instruction->location == DRV_GPU_SHADER_LOCATION_PRIMITIVE_ID) {
		*address = NVRTX_SM75_ATTR_PRIMITIVE_ID;
	} else if (instruction->location < 32U) {
		*address = NVRTX_SM75_ATTR_GENERIC + instruction->location * 16U + instruction->component * 4U;
	} else {
		error = nvrtx_sm75_refuse(lowering, ENOTSUP, "a vertex or geometry output not compiled yet");
		return error;
	}

	/* The system values' part of the map, or the generic part. */
	if (*address < NVRTX_SM75_ATTR_GENERIC) {
		nvrtx_sm75_sph_set(sph, NVRTX_SM75_SPH_OMAP_AB_LOW + *address / 4U, 1U, 1U);
	} else {
		nvrtx_sm75_sph_set(sph, NVRTX_SM75_SPH_OMAP_GENERIC_LOW + (*address - NVRTX_SM75_ATTR_GENERIC) / 4U, 1U, 1U);
	}

	/* Succeeded: the word is in the store request. */
	nvrtx_sm75_mark_store(lowering, *address);
	return 0;
}

/*
 * Prepares a geometry program: the output words it writes, each given a
 * register that holds it until an emit stores it (what the outputs hold
 * is what an emit sends, as the IR says), within what one thread's
 * vertices may take; the registers of the output vertex's handle and of
 * the first input vertex's index.
 *
 * Procedure referenced: Mesa 25.0.7
 * src/nouveau/compiler/nak_nir_lower_gs_intrinsics.c (the handle a
 * variable from 0, each emit and cut giving the next) and
 * src/nouveau/compiler/nak_nir_lower_vtg_io.c (the input vertex's index).
 */
static int
nvrtx_sm75_prepare_geometry(
	struct nvrtx_sm75_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t address;
	uint32_t index;
	uint32_t output;
	int error;

	/* The program, of a primitive it emits and at most the vertices the IR allows. */
	ir = lowering->ir;
	lowering->gs_output_count = 0U;
	if (ir->max_vertices == 0U || ir->max_vertices > DRV_GPU_IR_MAX_OUTPUT_VERTICES) {
		error = nvrtx_sm75_refuse(lowering, ENOTSUP, "a geometry program's most vertices out of range");
		return error;
	}
	if (ir->invocations == 0U || ir->invocations > DRV_GPU_IR_MAX_GEOMETRY_INVOCATIONS) {
		error = nvrtx_sm75_refuse(lowering, ENOTSUP, "a geometry program's invocations out of range");
		return error;
	}

	/* The words its stores write, each once. */
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		if (instruction->op != DRV_GPU_IR_STORE_OUTPUT)
			continue;

		/* The word, an output component of at most four. */
		lowering->index = index;
		if (instruction->component > 3U) {
			error = nvrtx_sm75_refuse(lowering, EINVAL, "an output component past the fourth");
			return error;
		}
		error = nvrtx_sm75_vtg_output(lowering, instruction, &address);
		if (error != 0)
			return error;

		/* A word not listed yet takes the next place. */
		for (output = 0U; output < lowering->gs_output_count; output++) {
			if (lowering->gs_output_address[output] == address)
				break;
		}
		if (output < lowering->gs_output_count)
			continue;
		if (lowering->gs_output_count == NVRTX_SM75_GS_OUTPUTS) {
			error = nvrtx_sm75_refuse(lowering, ENOTSUP, "more geometry output words than the compiler holds");
			return error;
		}
		lowering->gs_output_address[lowering->gs_output_count] = address;
		lowering->gs_output_count++;
	}

	/* What one thread's vertices take is within the hardware's. */
	lowering->index = 0U;
	if (ir->max_vertices * lowering->gs_output_count > NVRTX_SM75_GS_TOTAL_WORDS) {
		error = nvrtx_sm75_refuse(lowering, ENOTSUP, "a geometry program's vertices take more output words than one thread has");
		return error;
	}

	/* The handle's and the input index's registers, then each output's, which stay taken. */
	error = nvrtx_sm75_take(lowering, &lowering->gs_handle);
	if (error == 0)
		error = nvrtx_sm75_take(lowering, &lowering->gs_base);
	for (output = 0U; error == 0 && output < lowering->gs_output_count; output++)
		error = nvrtx_sm75_take(lowering, &lowering->gs_output_reg[output]);
	if (error != 0)
		return error;

	/* Succeeded: the program's registers are set aside. */
	return 0;
}

/*
 * Starts a geometry program: the handle 0, every output word 0 until it is
 * written, and the first input vertex's index (the primitive's place among
 * the warp's, the low byte of the invocation information, times the
 * vertices of one, its byte 2).
 *
 * Procedure referenced: Mesa 25.0.7
 * src/nouveau/compiler/nak_nir_lower_gs_intrinsics.c
 * (nak_nir_lower_gs_intrinsics: the handle starts at 0) and
 * src/nouveau/compiler/nak_nir_lower_vtg_io.c (lower_vtg_io_intrin:
 * NAK_SV_INVOCATION_INFO's bytes 0 and 2).
 */
static int
nvrtx_sm75_geometry_prologue(
	struct nvrtx_sm75_lowering *lowering)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src zero;
	struct nvrtx_sm75_src a;
	struct nvrtx_sm75_src b;
	uint32_t vertices;
	uint32_t output;
	int error;

	/* The handle 0. */
	zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_mov(&word, lowering->gs_handle, &zero);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	if (error != 0)
		return error;

	/* Each output word 0. */
	for (output = 0U; output < lowering->gs_output_count; output++) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_mov(&word, lowering->gs_output_reg[output], &zero);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
		if (error != 0)
			return error;
	}

	/* The invocation information. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_s2r(&word, lowering->gs_base, NVRTX_SM75_SR_INVOCATION_INFO);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_SPECIAL);
	if (error != 0)
		return error;

	/* Its byte 2 (the vertices of a primitive) into a temporary, its low byte (the primitive) in place. */
	error = nvrtx_sm75_take(lowering, &vertices);
	if (error != 0)
		return error;
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_prmt(&word, vertices, lowering->gs_base, NVRTX_SM75_PRMT_BYTE_2, NVRTX_SM75_RZ);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	if (error == 0) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_prmt(&word, lowering->gs_base, lowering->gs_base, NVRTX_SM75_PRMT_BYTE_0, NVRTX_SM75_RZ);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}

	/* The product, the index of the primitive's first vertex. */
	if (error == 0) {
		a = nvrtx_sm75_reg(lowering->gs_base);
		b = nvrtx_sm75_reg(vertices);
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_imad(&word, lowering->gs_base, &a, &b, &zero, 0);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	}
	nvrtx_sm75_give(lowering, vertices);

	/* Reports an emission past the code's room. */
	if (error != 0)
		return error;

	/* Succeeded: the program can read its inputs and emit. */
	return 0;
}

/* Holds a geometry program's output word in its register until an emit stores it. */
static int
nvrtx_sm75_geometry_output(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	const struct nvrtx_sm75_src *value)
{
	struct nvrtx_sm75_word word;
	uint32_t address;
	uint32_t output;
	int error;

	/* The word's register, set aside when the program was prepared. */
	error = nvrtx_sm75_vtg_output(lowering, instruction, &address);
	if (error != 0)
		return error;
	for (output = 0U; output < lowering->gs_output_count; output++) {
		if (lowering->gs_output_address[output] == address)
			break;
	}
	if (output == lowering->gs_output_count) {
		error = nvrtx_sm75_refuse(lowering, EINVAL, "a geometry output word without its register");
		return error;
	}

	/* The value into it. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_mov(&word, lowering->gs_output_reg[output], value);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	if (error != 0)
		return error;

	/* Succeeded: the word waits for the next emit. */
	return 0;
}

/*
 * Lowers a geometry program's read of an input vertex's attribute word (a
 * located input, the position or the point size): the vertex's index (the
 * first input vertex's plus the vertex number, a constant or a value),
 * ISBERD of it for the vertex's handle, and ALD through the handle.  The
 * header's input map records the word.
 *
 * Procedure referenced: Mesa 25.0.7
 * src/nouveau/compiler/nak_nir_lower_vtg_io.c (lower_vtg_io_intrin: a
 * per-vertex input's vertex through nir_isberd_nv, read by ald_nv) and
 * src/nouveau/compiler/nak/ir.rs (VtgIoInfo::mark_attrs_read).
 */
int
nvrtx_sm75_lower_vertex_input(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct nvrtx_sm75_word word;
	struct nvrtx_sm75_src base;
	struct nvrtx_sm75_src vertex;
	struct nvrtx_sm75_src zero;
	uint32_t *sph;
	uint32_t address;
	int error;

	/* Only a geometry program reads input vertices; nothing to do for a word nothing reads. */
	sph = lowering->binary->sph;
	if (lowering->ir->stage != DRV_GPU_STAGE_GEOMETRY) {
		error = nvrtx_sm75_refuse(lowering, EINVAL, "an input vertex read outside a geometry program");
		return error;
	}
	if (instruction->component > 3U) {
		error = nvrtx_sm75_refuse(lowering, EINVAL, "an input component past the fourth");
		return error;
	}

	/* The word's address and its bit of the input map. */
	if (instruction->location == DRV_GPU_IR_LOCATION_POSITION) {
		address = NVRTX_SM75_ATTR_POSITION + instruction->component * 4U;
		nvrtx_sm75_sph_set(sph, NVRTX_SM75_SPH_IMAP_AB_LOW + address / 4U, 1U, 1U);
	} else if (instruction->location == DRV_GPU_IR_LOCATION_POINT_SIZE) {
		address = NVRTX_SM75_ATTR_POINT_SIZE;
		nvrtx_sm75_sph_set(sph, NVRTX_SM75_SPH_IMAP_AB_LOW + address / 4U, 1U, 1U);
	} else if (instruction->location < 32U) {
		address = NVRTX_SM75_ATTR_GENERIC + instruction->location * 16U + instruction->component * 4U;
		nvrtx_sm75_sph_set(sph, NVRTX_SM75_SPH_IMAP_GENERIC_LOW + (address - NVRTX_SM75_ATTR_GENERIC) / 4U, 1U, 1U);
	} else {
		error = nvrtx_sm75_refuse(lowering, ENOTSUP, "an input vertex's word not compiled yet");
		return error;
	}
	if (dst == NVRTX_SM75_RZ)
		return 0;

	/* The vertex's index: the first's plus the vertex number, a value or a constant. */
	base = nvrtx_sm75_reg(lowering->gs_base);
	zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);
	if ((instruction->immediate & DRV_GPU_IR_VERTEX_DYNAMIC) != 0U) {
		vertex = nvrtx_sm75_value(lowering, instruction->src[0]);
	} else {
		vertex = nvrtx_sm75_imm(instruction->immediate);
	}
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_iadd3(&word, dst, &base, &vertex, &zero);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
	if (error != 0)
		return error;

	/* Its handle. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_isberd(&word, dst, dst);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	if (error != 0)
		return error;

	/* The word through the handle. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_ald_vertex(&word, dst, address, dst);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	if (error != 0)
		return error;

	/* Succeeded: the word is in dst. */
	return 0;
}

/*
 * Lowers a geometry program's emit of a vertex (every output word stored to
 * the vertex the handle names, then OUT.EMIT for the next handle) and end
 * of a strip (OUT.CUT), each for the channels whose Boolean holds when it
 * is predicated.
 *
 * Procedure referenced: Mesa 25.0.7
 * src/nouveau/compiler/nak_nir_lower_gs_intrinsics.c (rewrite_emit_vertex,
 * rewrite_end_primitive, rewrite_ast_nv) and
 * src/nouveau/compiler/nak/from_nir.rs (emit_vertex_nv and
 * end_primitive_nv as OpOut).
 */
int
nvrtx_sm75_lower_emit(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	struct nvrtx_sm75_word word;
	uint32_t output;
	uint32_t type;
	int guarded;
	int error;

	/* Only a geometry program emits. */
	if (lowering->ir->stage != DRV_GPU_STAGE_GEOMETRY) {
		error = nvrtx_sm75_refuse(lowering, EINVAL, "an emit outside a geometry program");
		return error;
	}

	/* P0 from the Boolean of a predicated one. */
	guarded = 0;
	if (instruction->component == 1U) {
		error = nvrtx_sm75_predicate(lowering, instruction->src[0]);
		if (error != 0)
			return error;
		guarded = 1;
	}

	/* An emit stores every output word to the vertex first. */
	type = NVRTX_SM75_OUT_CUT;
	if (instruction->op == DRV_GPU_IR_EMIT_VERTEX) {
		type = NVRTX_SM75_OUT_EMIT;
		for (output = 0U; output < lowering->gs_output_count; output++) {
			nvrtx_sm75_begin(&word);
			nvrtx_sm75_ast_vertex(&word, lowering->gs_output_address[output], lowering->gs_output_reg[output], lowering->gs_handle);
			if (guarded != 0)
				nvrtx_sm75_guard(&word, NVRTX_SM75_P0, 0);
			error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_STORE);
			if (error != 0)
				return error;
		}
	}

	/* OUT, the next handle in the handle's register. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_out(&word, lowering->gs_handle, lowering->gs_handle, type);
	if (guarded != 0)
		nvrtx_sm75_guard(&word, NVRTX_SM75_P0, 0);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
	if (error != 0)
		return error;

	/* Succeeded: the vertex is emitted or the strip ended. */
	return 0;
}

/*
 * Lowers a geometry program's built-in values: the input primitive's number
 * (the attribute word ALD reads with no vertex) and which of the
 * primitive's invocations the thread is (its special register).
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/compiler/nak_nir.c
 * (nak_nir_lower_system_value_intrin: load_primitive_id as ald_nv of
 * vertex 0, load_invocation_id as NAK_SV_INVOCATION_ID).
 */
int
nvrtx_sm75_lower_geometry_system(
	struct nvrtx_sm75_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct nvrtx_sm75_word word;
	int error;

	/* The primitive's number, read and recorded in the input map. */
	if (instruction->component == DRV_GPU_IR_SYSTEM_PRIMITIVE_ID) {
		nvrtx_sm75_sph_set(lowering->binary->sph, NVRTX_SM75_SPH_IMAP_AB_LOW + NVRTX_SM75_ATTR_PRIMITIVE_ID / 4U, 1U, 1U);
		if (dst == NVRTX_SM75_RZ)
			return 0;
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_ald(&word, dst, NVRTX_SM75_ATTR_PRIMITIVE_ID);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_WRITE);
		return error;
	}

	/* The invocation, or nothing else a geometry program has. */
	if (instruction->component != DRV_GPU_IR_SYSTEM_INVOCATION_ID) {
		error = nvrtx_sm75_refuse(lowering, ENOTSUP, "a built-in value a geometry shader does not have");
		return error;
	}
	if (dst == NVRTX_SM75_RZ)
		return 0;
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_s2r(&word, dst, NVRTX_SM75_SR_INVOCATION_ID);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_SPECIAL);
	if (error != 0)
		return error;

	/* Succeeded: the value is in dst. */
	return 0;
}

/*
 * Ends a geometry program's output before its EXIT: OUT of the final kind
 * at the last handle (nothing for another program).
 *
 * Procedure referenced: Mesa 25.0.7
 * src/nouveau/compiler/nak_nir_lower_gs_intrinsics.c
 * (append_final_primitive_nv) and src/nouveau/compiler/nak/from_nir.rs
 * (final_primitive_nv as OpOutFinal).
 */
int
nvrtx_sm75_epilogue(
	struct nvrtx_sm75_lowering *lowering)
{
	struct nvrtx_sm75_word word;
	int error;

	/* Only a geometry program has an output to end. */
	if (lowering->ir->stage != DRV_GPU_STAGE_GEOMETRY)
		return 0;

	/* OUT.FINAL at the handle, which gives nothing back. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_out(&word, NVRTX_SM75_RZ, lowering->gs_handle, NVRTX_SM75_OUT_FINAL);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_VARIABLE_STORE);
	if (error != 0)
		return error;

	/* Succeeded: the output is ended. */
	return 0;
}
