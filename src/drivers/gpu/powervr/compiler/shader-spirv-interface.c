/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPIR-V reader's interface: where the vertex inputs, varyings,
 * fragment colours and built-ins live, the reads at the entry and the
 * hand-over at the exit, and the packing and blending of a colour into
 * its attachment's pixel output.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/powervr/compiler/shader.h"
#include "drivers/gpu/powervr/compiler/shader-ir.h"
#include "drivers/gpu/powervr/compiler/shader-private.h"
#include "drivers/gpu/powervr/compiler/shader-spirv.h"

/* The blend factors that are the same for every colour channel (the alpha and constant alpha ones), each worked out once. */
#define BLEND_FACTORS			(FACTOR_SOURCE_ALPHA_SATURATE + 1U)

struct blend_factors {
	struct powervr_ir_operand value[BLEND_FACTORS];
	uint8_t known[BLEND_FACTORS];
};

static int reader_pixel_output(struct shader_spirv_reader *reader, const struct shader_spirv_id *variable, uint32_t words);
static struct powervr_ir_operand blend_factor(struct shader_spirv_reader *reader, uint32_t factor, uint32_t channel, const struct powervr_ir_operand *source, const struct powervr_ir_operand *destination, uint32_t constants, int *error);
static struct powervr_ir_operand blend_factor_kept(struct shader_spirv_reader *reader, struct blend_factors *kept, uint32_t factor, uint32_t channel, const struct powervr_ir_operand *source, const struct powervr_ir_operand *destination, uint32_t constants, int *error);
static struct powervr_ir_operand blend_sum(struct shader_spirv_reader *reader, uint32_t operation, const struct powervr_ir_operand *source, const struct powervr_ir_operand *source_factor, const struct powervr_ir_operand *destination, const struct powervr_ir_operand *destination_factor);
static int is_constant(const struct powervr_ir_operand *operand, uint32_t bits);
static int blend_sum_kind(const struct powervr_shader_blend *blend, uint32_t *kind);
static struct powervr_ir_operand saturate(struct shader_spirv_reader *reader, struct powervr_ir_operand x);
static int attribute_words(struct shader_spirv_reader *reader, uint32_t location, uint32_t words, uint32_t *format_words);
static uint32_t color_format_known(uint32_t format);
static int module_kills(const struct shader_spirv_reader *reader);

/*
 * Places the interface variables: each register-class variable gets
 * values for its words, vertex inputs their registers, varyings their
 * output words or coefficient sets.
 */
int
shader_spirv_reader_layout_interface(
	struct shader_spirv_reader *reader)
{
	struct powervr_shader_program *program;
	struct shader_spirv_id *id;
	uint32_t location;
	uint32_t best;
	uint32_t index;
	uint32_t words;
	uint32_t next_input;
	uint32_t next_output;
	uint32_t next_coefficient;
	uint32_t any_smooth;
	uint32_t done[SHADER_SPIRV_IDS / 32U];
	uint32_t entry;
	uint32_t next_pixel;
	int error;
	uint32_t known;
	uint32_t scalars;

	/* Gives every register-class global its words as values. */
	program = reader->program;
	for (index = 1; index < reader->bound; index++) {
		id = &reader->ids[index];
		if (id->kind != ID_POINTER || id->variable != index)
			continue;
		if (id->storage != STORAGE_INPUT && id->storage != STORAGE_OUTPUT && id->storage != STORAGE_PRIVATE)
			continue;
		words = shader_spirv_scalar_count(reader, id->type);
		if (words == 0 || words > COMPOSITE_MAX * 2U)
			return ENOTSUP;
		id->first = shader_spirv_new_value(reader, 1);
		for (location = 1; location < words; location++)
			(void)shader_spirv_new_value(reader, 1);
		if (reader->error != 0)
			return reader->error;
	}

	/* Does a fragment shader iterate anything with perspective? */
	any_smooth = 0;
	for (index = 1; index < reader->bound; index++) {
		id = &reader->ids[index];
		if (id->kind == ID_POINTER &&
		    id->variable == index &&
		    id->storage == STORAGE_INPUT &&
		    (id->flags & FLAG_LOCATION) != 0 &&
		    (id->flags & FLAG_FLAT) == 0)
			any_smooth = 1;
	}

	/* The w coefficients come first when a varying needs them. */
	next_coefficient = 0;
	if (reader->stage == POWERVR_SHADER_FRAGMENT && any_smooth != 0) {
		program->uses_w = 1;
		program->w_coefficient = 0;
		next_coefficient = 4;
	}

	/* Places located inputs and outputs in location order. */
	for (index = 0; index < SHADER_SPIRV_IDS / 32U; index++)
		done[index] = 0;
	next_input = 0;
	next_output = 4;
	next_pixel = 0;
	for (;;) {
		/* Finds the unplaced located interface variable of the lowest location. */
		best = 0;
		for (index = 1; index < reader->bound; index++) {
			id = &reader->ids[index];
			if (id->kind != ID_POINTER || id->variable != index || (id->flags & FLAG_LOCATION) == 0)
				continue;
			if (id->storage != STORAGE_INPUT && id->storage != STORAGE_OUTPUT)
				continue;
			if ((done[index / 32U] & (1U << (index % 32U))) != 0)
				continue;
			if (best == 0 || id->location < reader->ids[best].location)
				best = index;
		}
		if (best == 0)
			break;

		/* Places it by stage and direction. */
		done[best / 32U] |= 1U << (best % 32U);
		id = &reader->ids[best];
		words = shader_spirv_scalar_count(reader, id->type);
		entry = 0;
		if (id->storage == STORAGE_INPUT && reader->stage == POWERVR_SHADER_VERTEX) {
			if (program->input_count >= POWERVR_SHADER_INPUTS)
				return ENOTSUP;
			entry = program->input_count++;
			id->words = words;
			error = attribute_words(reader, id->location, words, &id->words);
			if (error != 0)
				return error;
			program->inputs[entry].location = id->location;
			program->inputs[entry].components = id->words;
			program->inputs[entry].reg = next_input;
			id->interface = next_input;
			next_input += id->words;
		} else if (id->storage == STORAGE_OUTPUT && reader->stage == POWERVR_SHADER_VERTEX) {
			if (program->varying_count >= POWERVR_SHADER_VARYINGS)
				return ENOTSUP;
			entry = program->varying_count++;
			program->varyings[entry].location = id->location;
			program->varyings[entry].components = words;
			program->varyings[entry].flat = (id->flags & FLAG_FLAT) != 0;
			program->varyings[entry].reg = next_output;
			id->interface = next_output;
			next_output += words;
		} else if (id->storage == STORAGE_INPUT && reader->stage == POWERVR_SHADER_FRAGMENT) {
			if (program->varying_count >= POWERVR_SHADER_VARYINGS)
				return ENOTSUP;
			entry = program->varying_count++;
			program->varyings[entry].location = id->location;
			program->varyings[entry].components = words;
			program->varyings[entry].flat = (id->flags & FLAG_FLAT) != 0;
			program->varyings[entry].reg = next_coefficient;
			id->interface = next_coefficient;
			next_coefficient += words * 4U;
		} else if (id->storage == STORAGE_OUTPUT && reader->stage == POWERVR_SHADER_FRAGMENT) {
			if (program->output_count >= POWERVR_SHADER_OUTPUTS || words > 4)
				return ENOTSUP;
			if (reader->key == NULL) {
				/* Floats in temporaries, by location. */
				entry = program->output_count++;
				program->outputs[entry].location = id->location;
				id->interface = id->location * 4U;
			} else if (id->location >= POWERVR_SHADER_OUTPUTS || reader->key->output_formats[id->location] == 0) {
				/* No attachment: the colour is dropped. */
				id->interface = UINT32_MAX;
			} else {
				/* A packed colour in the next pixel output registers (one, or two for half floats). */
				known = color_format_known(reader->key->output_formats[id->location]);
				if (known == 0)
					return ENOTSUP;
				entry = program->output_count++;
				program->outputs[entry].location = id->location;
				program->outputs[entry].format = reader->key->output_formats[id->location];
				program->outputs[entry].reg = next_pixel;
				id->interface = next_pixel;
				next_pixel += known;
				if (next_pixel > PIXEL_OUTPUTS_MAX)
					return ENOTSUP;
			}
		} else {
			/* A located compute variable is not a thing. */
			return ENOTSUP;
		}
	}

	/*
	 * The vertex and instance index, each one vertex input register after
	 * the attributes, written by the driver's data sequencer program.
	 *
	 * Procedure referenced:
	 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_pipeline.c
	 * (pvr_alloc_vs_sysvals, pvr_pds_vertex_attrib_programs_create: the
	 * vertex id and instance id are one vertex input register each, the
	 * data sequencer told their registers) and
	 * src/imagination/pco/pco_nir.c (the compiler options: the instance
	 * id includes the base instance, the vertex id is not zero based).
	 */
	if (reader->stage == POWERVR_SHADER_VERTEX) {
		for (index = 1; index < reader->bound; index++) {
			id = &reader->ids[index];
			if (id->kind != ID_POINTER || id->variable != index || id->storage != STORAGE_INPUT)
				continue;
			if ((id->flags & FLAG_BUILT_IN) == 0)
				continue;
			if (id->builtin != BUILT_IN_VERTEX_INDEX && id->builtin != BUILT_IN_INSTANCE_INDEX)
				continue;
			scalars = shader_spirv_scalar_count(reader, id->type);
			if (scalars != 1)
				return ENOTSUP;
			id->interface = next_input;
			if (id->builtin == BUILT_IN_VERTEX_INDEX)
				program->vertex_index_reg = next_input;
			else
				program->instance_index_reg = next_input;
			next_input++;
		}
	}

	/* Records the register counts of the interface. */
	program->vertex_inputs = next_input;
	program->vertex_outputs = next_output;
	if (reader->stage != POWERVR_SHADER_VERTEX)
		program->vertex_outputs = 0;
	program->coefficients = next_coefficient;

	/* Succeeded: the interface is placed. */
	return 0;
}

/*
 * Reads the stage's inputs into the input variables' values before the
 * first block, so every later read is a plain copy.
 */
int
shader_spirv_reader_entry_prologue(
	struct shader_spirv_reader *reader)
{
	struct powervr_ir_operand number;
	struct powervr_ir_operand group;
	struct powervr_ir_operand size;
	struct powervr_ir_operand word;
	struct shader_spirv_id *id;
	uint32_t index;
	uint32_t component;
	uint32_t words;
	uint32_t request;
	uint32_t builtin;
	int error;

	/* Reads each input variable's words. */
	for (index = 1; index < reader->bound; index++) {
		id = &reader->ids[index];
		if (id->kind != ID_POINTER || id->variable != index || id->storage != STORAGE_INPUT)
			continue;
		words = shader_spirv_scalar_count(reader, id->type);

		/* A located input: a vertex input register or an iterated varying. */
		if ((id->flags & FLAG_LOCATION) != 0) {
			for (component = 0; component < words; component++) {
				if (reader->stage == POWERVR_SHADER_VERTEX && component >= id->words) {
					/* A word the attribute's format lacks: 0, or 1 for w. */
					word = shader_spirv_constant_operand(component == 3 ? FLOAT_ONE : 0);
					shader_spirv_emit(reader, POWERVR_IR_MOVE, id->first + component, &word, NULL, NULL, 0, 0);
				} else if (reader->stage == POWERVR_SHADER_VERTEX) {
					shader_spirv_emit(reader, POWERVR_IR_INPUT, id->first + component, NULL, NULL, NULL, id->interface + component, 0);
				} else {
					shader_spirv_emit(reader, POWERVR_IR_ITERATE, id->first + component, NULL, NULL, NULL, id->interface + component * 4U, 0);
					if ((id->flags & FLAG_FLAT) != 0)
						reader->shader->instructions[reader->shader->instruction_count - 1U].flags = POWERVR_IR_FLAT;
				}
			}
			continue;
		}

		/* Refuses an input that is neither located nor a known built-in. */
		if ((id->flags & FLAG_BUILT_IN) == 0)
			return ENOTSUP;
		builtin = id->builtin;

		/* The vertex or instance index, in the register the layout gave it. */
		if (reader->stage == POWERVR_SHADER_VERTEX && (builtin == BUILT_IN_VERTEX_INDEX || builtin == BUILT_IN_INSTANCE_INDEX)) {
			shader_spirv_emit(reader, POWERVR_IR_INPUT, id->first, NULL, NULL, NULL, id->interface, 0);
			continue;
		}
		if (reader->stage != POWERVR_SHADER_COMPUTE)
			return ENOTSUP;

		/* The compute built-ins, built from the instance number and the workgroup. */
		number = shader_spirv_emit_value(reader, POWERVR_IR_INSTANCE, NULL, NULL, NULL);
		if (builtin == BUILT_IN_LOCAL_INVOCATION_ID || builtin == BUILT_IN_LOCAL_INVOCATION_INDEX) {
			shader_spirv_emit(reader, POWERVR_IR_MOVE, id->first, &number, NULL, NULL, 0, 0);
			for (component = 1; component < words; component++) {
				word = shader_spirv_constant_operand(0);
				shader_spirv_emit(reader, POWERVR_IR_MOVE, id->first + component, &word, NULL, NULL, 0, 0);
			}
		} else if (builtin == BUILT_IN_WORKGROUP_ID || builtin == BUILT_IN_NUM_WORKGROUPS || builtin == BUILT_IN_GLOBAL_INVOCATION_ID) {
			/* The workgroup id or count, three words in shared registers. */
			error = shader_spirv_request_add(reader, builtin == BUILT_IN_NUM_WORKGROUPS ? POWERVR_SHADER_REQUEST_NUM_WORKGROUPS : POWERVR_SHADER_REQUEST_WORKGROUP_ID, 0, 0, 0, 3, &request);
			if (error != 0)
				return error;
			for (component = 0; component < words && component < 3; component++) {
				group = shader_spirv_shared_word(reader, request, component);
				if (builtin != BUILT_IN_GLOBAL_INVOCATION_ID) {
					shader_spirv_emit(reader, POWERVR_IR_MOVE, id->first + component, &group, NULL, NULL, 0, 0);
				} else if (component == 0) {
					/* x is the group times the workgroup's width plus the instance. */
					size = shader_spirv_constant_operand(reader->program->workgroup[0]);
					shader_spirv_emit(reader, POWERVR_IR_INTEGER_MULTIPLY, id->first, &group, &size, NULL, 0, 0);
					word = shader_spirv_value_operand(id->first, 0);
					shader_spirv_emit(reader, POWERVR_IR_INTEGER_ADD, id->first, &word, &number, NULL, 0, 0);
				} else {
					/* y and z are the group's, the workgroup being one deep in each. */
					shader_spirv_emit(reader, POWERVR_IR_MOVE, id->first + component, &group, NULL, NULL, 0, 0);
				}
			}
		} else {
			return ENOTSUP;
		}
	}

	/* Clears the output variables, so an unwritten word is zero rather than stale. */
	for (index = 1; index < reader->bound; index++) {
		id = &reader->ids[index];
		if (id->kind != ID_POINTER || id->variable != index)
			continue;
		if (id->storage != STORAGE_OUTPUT && id->storage != STORAGE_PRIVATE)
			continue;
		words = shader_spirv_scalar_count(reader, id->type);
		for (component = 0; component < words; component++) {
			word = shader_spirv_constant_operand(0);
			shader_spirv_emit(reader, POWERVR_IR_MOVE, id->first + component, &word, NULL, NULL, 0, 0);
		}
	}

	/*
	 * A fragment shader that discards keeps its discard in a value of its
	 * own, false from the start; a kill makes it true and the end gives
	 * it to the depth and visibility unit (see the epilogue).
	 *
	 * Procedure referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_nir_pvfio.c (lower_isp_fb and
	 * its discard_cond_reg: false before the first block, true at each
	 * demote, a discard being a demote there (pco_nir.c
	 * discard_is_demote), read by the one isp_feedback_pco).
	 */
	if (reader->stage == POWERVR_SHADER_FRAGMENT && module_kills(reader)) {
		reader->discard = shader_spirv_new_value(reader, 1);
		word = shader_spirv_constant_operand(0);
		shader_spirv_emit(reader, POWERVR_IR_MOVE, reader->discard, &word, NULL, NULL, 0, 0);
		reader->program->discards = 1;
	}

	/* Refuses a workgroup the core's task cannot hold as one row. */
	if (reader->stage == POWERVR_SHADER_COMPUTE) {
		if (reader->program->workgroup[1] != 1 || reader->program->workgroup[2] != 1)
			return ENOTSUP;
	}

	/* Reports what the reads came to. */
	return reader->error;
}

/*
 * Hands the output variables' words over after the last block: the
 * vertex position and varyings to their output words, the fragment
 * colours to their outputs.
 */
int
shader_spirv_reader_entry_epilogue(
	struct shader_spirv_reader *reader)
{
	struct powervr_ir_operand word;
	struct shader_spirv_member *member;
	struct shader_spirv_id *type;
	struct shader_spirv_id *id;
	uint32_t index;
	uint32_t member_index;
	uint32_t component;
	uint32_t words;
	int error;

	/*
	 * The discard, before the colours, as the feedback that keeps or drops the fragment.
	 *
	 * Procedure referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_nir_pvfio.c (lower_isp_fb:
	 * the isp_feedback_pco before the first output store).
	 */
	if (reader->discard != 0) {
		word = shader_spirv_value_operand(reader->discard, 0);
		shader_spirv_emit(reader, POWERVR_IR_FEEDBACK, 0, &word, NULL, NULL, 0, 0);
	}

	/* Each output variable. */
	for (index = 1; index < reader->bound; index++) {
		id = &reader->ids[index];
		if (id->kind != ID_POINTER || id->variable != index || id->storage != STORAGE_OUTPUT)
			continue;
		words = shader_spirv_scalar_count(reader, id->type);

		/* A colour packed into its attachment's pixel output, blended. */
		if ((id->flags & FLAG_LOCATION) != 0 && reader->stage == POWERVR_SHADER_FRAGMENT && reader->key != NULL) {
			if (id->interface == UINT32_MAX)
				continue;
			error = reader_pixel_output(reader, id, words);
			if (error != 0)
				return error;
			continue;
		}

		/* A located output: a varying's or a colour's words. */
		if ((id->flags & FLAG_LOCATION) != 0) {
			for (component = 0; component < words; component++) {
				word = shader_spirv_value_operand(id->first + component, 0);
				shader_spirv_emit(reader, POWERVR_IR_OUTPUT, 0, &word, NULL, NULL, id->interface + component, 0);
			}
			continue;
		}

		/* A built-in position, alone or in the per-vertex block. */
		if ((id->flags & FLAG_BUILT_IN) != 0) {
			if (id->builtin != BUILT_IN_POSITION || reader->stage != POWERVR_SHADER_VERTEX)
				continue;
			for (component = 0; component < 4; component++) {
				word = shader_spirv_value_operand(id->first + component, 0);
				shader_spirv_emit(reader, POWERVR_IR_OUTPUT, 0, &word, NULL, NULL, component, 0);
			}
			continue;
		}

		/* The per-vertex block: its position member; point size and clip distances are dropped. */
		type = &reader->ids[id->type];
		if (type->type_kind != TYPE_STRUCT)
			return ENOTSUP;
		for (member_index = 0; member_index < type->count; member_index++) {
			member = &reader->members[type->first + member_index];
			if (member->has_builtin == 0 || member->builtin != BUILT_IN_POSITION)
				continue;
			if (reader->stage != POWERVR_SHADER_VERTEX)
				return ENOTSUP;
			for (component = 0; component < 4; component++) {
				word = shader_spirv_value_operand(id->first + member->scalar_offset + component, 0);
				shader_spirv_emit(reader, POWERVR_IR_OUTPUT, 0, &word, NULL, NULL, component, 0);
			}
		}
	}

	/* Reports what the hand-over came to. */
	return reader->error;
}

/*
 * Writes one colour output into its pixel output register: clamped to
 * the attachment's range, blended with the colour already there, its
 * unwritten channels kept, packed (as 8-bit unsigned normalised in one
 * register, or as half floats in two for a 16-bit float attachment, not
 * clamped).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_nir_pvfio.c (lower_pfo_store,
 * to_pbe_format, pack_to_format: a B8G8R8A8 attachment is packed as
 * R8G8B8A8, the channels' float values packed to 8-bit unsigned
 * normalised) and the Vulkan 1.3 specification's blending equations
 * (the source and destination factors, the five operations, unsigned
 * normalised values clamped to [0, 1]).
 */
static int
reader_pixel_output(
	struct shader_spirv_reader *reader,
	const struct shader_spirv_id *variable,
	uint32_t words)
{
	const struct powervr_shader_blend *blend;
	struct powervr_ir_operand source[4];
	struct powervr_ir_operand destination[4];
	struct powervr_ir_operand result[4];
	struct powervr_ir_operand a;
	struct powervr_ir_operand b;
	struct powervr_ir_instruction *store;
	struct blend_factors kept;
	uint32_t channel;
	uint32_t factor;
	uint32_t operation;
	uint32_t constants;
	uint32_t loaded;
	uint32_t needs_destination;
	uint32_t sum_kind;
	uint32_t half;
	int held;
	int error;

	/* Half floats (a 16-bit float attachment) are neither clamped nor normalised. */
	half = 0;
	if (reader->key->output_formats[variable->location] == FORMAT_R16G16B16A16_SFLOAT)
		half = 1;

	/*
	 * The shader's colour, clamped when the attachment is normalised and
	 * blended; a missing channel is 0, a missing alpha 1.  Without
	 * blending the pack clamps (its scaled 8-bit unsigned normalised
	 * form saturates, as Mesa's pco relies on: pck.scale.u8888 with no
	 * clamp before it), so the clamps are left out (they were eight of a
	 * flat quad's 24 operations a pixel).
	 */
	blend = &reader->key->blend[variable->location];
	for (channel = 0; channel < 4; channel++) {
		if (channel < words && (half || blend->enable == 0)) {
			source[channel] = shader_spirv_value_operand(variable->first + channel, 0);
		} else if (channel < words) {
			source[channel] = saturate(reader, shader_spirv_value_operand(variable->first + channel, 0));
		} else {
			source[channel] = shader_spirv_constant_operand(channel == 3 ? FLOAT_ONE : 0);
		}
	}

	/*
	 * The sum-of-products form: a blend one operation does, with
	 * every channel a value or a constant the hardware holds (zero or
	 * one), is one instruction of the clamped colour; with the raw form,
	 * of the colour as the shader left it, for the operation to clamp
	 * (the clamps above then go as dead writes).
	 */
	if ((reader->key->forms & POWERVR_SHADER_FORM_SUM_OF_PRODUCTS) != 0 && half == 0 && blend->enable != 0) {
		error = blend_sum_kind(blend, &sum_kind);
		for (channel = 0; channel < 4 && error == 0; channel++) {
			/* A constant channel must be one the hardware holds. */
			if (source[channel].kind != POWERVR_IR_OPERAND_CONSTANT)
				continue;
			held = is_constant(&source[channel], 0);
			if (!held)
				held = is_constant(&source[channel], FLOAT_ONE);
			if (!held)
				error = ENOTSUP;
		}
		if (error == 0) {
			if ((reader->key->forms & POWERVR_SHADER_FORM_SUM_RAW) != 0) {
				/* The raw form: the colour as the shader left it. */
				for (channel = 0; channel < words && channel < 4; channel++)
					source[channel] = shader_spirv_value_operand(variable->first + channel, 0);
			}

			/* The blend, reading the fourth channel beside the three the emit takes. */
			shader_spirv_emit(reader, POWERVR_IR_PIXEL_BLEND, 0, &source[0], &source[1], &source[2], variable->interface, sum_kind);
			if (reader->error != 0)
				return reader->error;
			store = &reader->shader->instructions[reader->shader->instruction_count - 1U];
			store->source[3] = source[3];

			/* Reports what the colour came to. */
			return reader->error;
		}
	}

	/* The colour already there, when blending or a write mask needs it. */
	needs_destination = 0;
	if (blend->enable != 0 || (blend->write_mask & 0xfU) != 0xfU)
		needs_destination = 1;
	loaded = 0;
	if (needs_destination != 0) {
		loaded = shader_spirv_new_value(reader, 4);
		shader_spirv_emit(reader, POWERVR_IR_PIXEL_LOAD, loaded, NULL, NULL, NULL, variable->interface, half);
	}
	for (channel = 0; channel < 4; channel++)
		destination[channel] = shader_spirv_value_operand(loaded, channel);

	/* The blend constants, asked for only when a factor names them. */
	constants = POWERVR_SHADER_REQUESTS;
	if (blend->enable != 0) {
		if ((blend->source_color >= FACTOR_CONSTANT_COLOR && blend->source_color <= FACTOR_ONE_MINUS_CONSTANT_ALPHA) ||
		    (blend->destination_color >= FACTOR_CONSTANT_COLOR && blend->destination_color <= FACTOR_ONE_MINUS_CONSTANT_ALPHA) ||
		    (blend->source_alpha >= FACTOR_CONSTANT_COLOR && blend->source_alpha <= FACTOR_ONE_MINUS_CONSTANT_ALPHA) ||
		    (blend->destination_alpha >= FACTOR_CONSTANT_COLOR && blend->destination_alpha <= FACTOR_ONE_MINUS_CONSTANT_ALPHA)) {
			error = shader_spirv_request_add(reader, POWERVR_SHADER_REQUEST_BLEND_CONSTANTS, 0, 0, 0, 4, &constants);
			if (error != 0)
				return error;
		}
	}

	/* Each channel: the blend, then the write mask. */
	for (channel = 0; channel < BLEND_FACTORS; channel++)
		kept.known[channel] = 0;
	for (channel = 0; channel < 4; channel++) {
		result[channel] = source[channel];
		if (blend->enable != 0) {
			/* The operation of the colour or the alpha. */
			operation = channel < 3 ? blend->color_operation : blend->alpha_operation;
			if (operation == OPERATION_MINIMUM) {
				result[channel] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MINIMUM, &source[channel], &destination[channel], NULL);
			} else if (operation == OPERATION_MAXIMUM) {
				result[channel] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MAXIMUM, &source[channel], &destination[channel], NULL);
			} else if (operation <= OPERATION_REVERSE_SUBTRACT) {
				/* The source and destination terms, each its value times its factor (the pack clamps the sum, see above). */
				error = 0;
				factor = channel < 3 ? blend->source_color : blend->source_alpha;
				a = blend_factor_kept(reader, &kept, factor, channel, source, destination, constants, &error);
				factor = channel < 3 ? blend->destination_color : blend->destination_alpha;
				b = blend_factor_kept(reader, &kept, factor, channel, source, destination, constants, &error);
				if (error != 0)
					return error;
				result[channel] = blend_sum(reader, operation, &source[channel], &a, &destination[channel], &b);
			} else {
				return ENOTSUP;
			}
		}
		if ((blend->write_mask & (1U << channel)) == 0)
			result[channel] = destination[channel];
	}

	/* The packed colour into its register. */
	shader_spirv_emit(reader, POWERVR_IR_PIXEL_STORE, 0, &result[0], &result[1], &result[2], variable->interface, half);
	if (reader->error != 0)
		return reader->error;
	store = &reader->shader->instructions[reader->shader->instruction_count - 1U];
	store->source[3] = result[3];

	/* Reports what the colour came to. */
	return reader->error;
}

/*
 * Works out one channel's blend factor, once a blend for the factors
 * that are the same for every colour channel (otherwise the one-minus
 * source alpha of premultiplied blending is worked out once a channel).
 */
static struct powervr_ir_operand
blend_factor_kept(
	struct shader_spirv_reader *reader,
	struct blend_factors *kept,
	uint32_t factor,
	uint32_t channel,
	const struct powervr_ir_operand *source,
	const struct powervr_ir_operand *destination,
	uint32_t constants,
	int *error)
{
	struct powervr_ir_operand value;
	int shared;

	/* The alpha and constant alpha factors, and the saturate one for a colour channel, are the same a channel. */
	shared = factor == FACTOR_SOURCE_ALPHA || factor == FACTOR_ONE_MINUS_SOURCE_ALPHA ||
		 factor == FACTOR_DESTINATION_ALPHA || factor == FACTOR_ONE_MINUS_DESTINATION_ALPHA ||
		 factor == FACTOR_CONSTANT_ALPHA || factor == FACTOR_ONE_MINUS_CONSTANT_ALPHA ||
		 (factor == FACTOR_SOURCE_ALPHA_SATURATE && channel < 3);
	if (shared && kept->known[factor] != 0)
		return kept->value[factor];

	/* Works it out, and keeps a shared one. */
	value = blend_factor(reader, factor, channel, source, destination, constants, error);
	if (shared && *error == 0) {
		kept->value[factor] = value;
		kept->known[factor] = 1;
	}
	return value;
}

/*
 * Works out source * source factor (+, - or reversed -) destination *
 * destination factor: a factor of one or zero takes no multiply, and a
 * destination term with a factor is one multiply-add with the source
 * term.
 */
static struct powervr_ir_operand
blend_sum(
	struct shader_spirv_reader *reader,
	uint32_t operation,
	const struct powervr_ir_operand *source,
	const struct powervr_ir_operand *source_factor,
	const struct powervr_ir_operand *destination,
	const struct powervr_ir_operand *destination_factor)
{
	struct powervr_ir_operand a;
	struct powervr_ir_operand b;
	struct powervr_ir_operand result;
	int has_a;
	int has_b;

	/* The source term: none for a zero factor, the source for one, else the product. */
	has_a = !is_constant(source_factor, 0);
	a = *source;
	if (has_a && !is_constant(source_factor, FLOAT_ONE))
		a = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, source, source_factor, NULL);
	if (operation == OPERATION_REVERSE_SUBTRACT)
		a = shader_spirv_float_negate(a);

	/* The destination term: none for a zero factor; negated when subtracted. */
	has_b = !is_constant(destination_factor, 0);
	b = *destination;
	if (operation == OPERATION_SUBTRACT)
		b = shader_spirv_float_negate(b);

	/* No destination term: the source term, or zero. */
	if (!has_b) {
		if (has_a)
			return a;
		result = shader_spirv_constant_operand(0);
		return result;
	}

	/* A destination term of factor one: added, or alone. */
	if (is_constant(destination_factor, FLOAT_ONE)) {
		if (!has_a)
			return b;
		result = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &a, &b, NULL);
		return result;
	}

	/* Alone: the product; with a source term: one multiply-add. */
	if (!has_a) {
		result = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &b, destination_factor, NULL);
		return result;
	}
	result = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &b, destination_factor, &a);
	return result;
}

/*
 * Names the blend of POWERVR_IR_PIXEL_BLEND a pipeline's blend is, when
 * it is one: every channel written, and the colour and the
 * alpha the same equation; ENOTSUP otherwise.
 */
static int
blend_sum_kind(
	const struct powervr_shader_blend *blend,
	uint32_t *kind)
{
	/* Every channel written. */
	if ((blend->write_mask & 0xfU) != 0xfU)
		return ENOTSUP;

	/* One and one minus the source alpha, added or subtracted: over. */
	if (blend->source_color == FACTOR_ONE &&
	    blend->destination_color == FACTOR_ONE_MINUS_SOURCE_ALPHA &&
	    blend->source_alpha == FACTOR_ONE &&
	    blend->destination_alpha == FACTOR_ONE_MINUS_SOURCE_ALPHA &&
	    blend->color_operation == blend->alpha_operation) {
		if (blend->color_operation == OPERATION_ADD) {
			*kind = POWERVR_IR_BLEND_OVER;
			return 0;
		}
		if (blend->color_operation == OPERATION_SUBTRACT) {
			*kind = POWERVR_IR_BLEND_SUBTRACT;
			return 0;
		}
		return ENOTSUP;
	}

	/* One and one, added. */
	if (blend->source_color == FACTOR_ONE &&
	    blend->destination_color == FACTOR_ONE &&
	    blend->source_alpha == FACTOR_ONE &&
	    blend->destination_alpha == FACTOR_ONE &&
	    blend->color_operation == OPERATION_ADD &&
	    blend->alpha_operation == OPERATION_ADD) {
		*kind = POWERVR_IR_BLEND_ADD;
		return 0;
	}

	/* The destination's colour (its alpha for the alpha) and zero, added: the product. */
	if (blend->source_color == FACTOR_DESTINATION_COLOR &&
	    blend->destination_color == FACTOR_ZERO &&
	    (blend->source_alpha == FACTOR_DESTINATION_ALPHA || blend->source_alpha == FACTOR_DESTINATION_COLOR) &&
	    blend->destination_alpha == FACTOR_ZERO &&
	    blend->color_operation == OPERATION_ADD &&
	    blend->alpha_operation == OPERATION_ADD) {
		*kind = POWERVR_IR_BLEND_MULTIPLY;
		return 0;
	}

	/* Any other blend takes the arithmetic. */
	return ENOTSUP;
}

/* Reports whether an operand is the constant of these bits. */
static int
is_constant(
	const struct powervr_ir_operand *operand,
	uint32_t bits)
{
	/* A constant operand without modifiers. */
	return operand->kind == POWERVR_IR_OPERAND_CONSTANT && operand->modifiers == 0 && operand->constant == bits;
}

/* Works out one channel's blend factor. */
static struct powervr_ir_operand
blend_factor(
	struct shader_spirv_reader *reader,
	uint32_t factor,
	uint32_t channel,
	const struct powervr_ir_operand *source,
	const struct powervr_ir_operand *destination,
	uint32_t constants,
	int *error)
{
	struct powervr_ir_operand one;
	struct powervr_ir_operand value;
	struct powervr_ir_operand other;

	/* The factor's value before any one-minus. */
	one = shader_spirv_constant_operand(FLOAT_ONE);
	switch (factor) {
	case FACTOR_ZERO:
		value = shader_spirv_constant_operand(0);

		/* Succeeded: the zero factor. */
		return value;
	case FACTOR_ONE:
		return one;
	case FACTOR_SOURCE_COLOR:
	case FACTOR_ONE_MINUS_SOURCE_COLOR:
		value = source[channel];
		break;
	case FACTOR_DESTINATION_COLOR:
	case FACTOR_ONE_MINUS_DESTINATION_COLOR:
		value = destination[channel];
		break;
	case FACTOR_SOURCE_ALPHA:
	case FACTOR_ONE_MINUS_SOURCE_ALPHA:
		value = source[3];
		break;
	case FACTOR_DESTINATION_ALPHA:
	case FACTOR_ONE_MINUS_DESTINATION_ALPHA:
		value = destination[3];
		break;
	case FACTOR_CONSTANT_COLOR:
	case FACTOR_ONE_MINUS_CONSTANT_COLOR:
		value = shader_spirv_shared_word(reader, constants, channel);
		break;
	case FACTOR_CONSTANT_ALPHA:
	case FACTOR_ONE_MINUS_CONSTANT_ALPHA:
		value = shader_spirv_shared_word(reader, constants, 3);
		break;
	case FACTOR_SOURCE_ALPHA_SATURATE:
		/* min(As, 1 - Ad) for the colour, 1 for the alpha. */
		if (channel == 3)
			return one;
		other = shader_spirv_float_negate(destination[3]);
		other = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &one, &other, NULL);
		value = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MINIMUM, &source[3], &other, NULL);

		/* Succeeded: the saturated alpha factor. */
		return value;
	default:
		*error = ENOTSUP;
		return one;
	}

	/* The one-minus factors are the odd ones from 3 to 13. */
	if ((factor & 1U) != 0) {
		value = shader_spirv_float_negate(value);
		value = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &one, &value, NULL);
	}
	return value;
}

/*
 * Clamps a float to [0, 1]: one add of zero with the saturate (the
 * core's saturate in place of a maximum and a minimum, as Mesa's
 * pco lowers fsat).
 */
static struct powervr_ir_operand
saturate(
	struct shader_spirv_reader *reader,
	struct powervr_ir_operand x)
{
	struct powervr_ir_operand zero;
	struct powervr_ir_operand result;

	/* x + 0, saturated. */
	zero = shader_spirv_constant_operand(0);
	result = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &x, &zero, NULL);
	if (reader->error == 0 && reader->shader->instruction_count != 0)
		reader->shader->instructions[reader->shader->instruction_count - 1U].flags |= POWERVR_IR_SATURATE;
	return result;
}

/*
 * Works out how many words of a vertex input its attribute's format
 * gives: all the shader reads without a key or a format, else the
 * format's float words (the rest are filled in by the shader).
 */
static int
attribute_words(
	struct shader_spirv_reader *reader,
	uint32_t location,
	uint32_t words,
	uint32_t *format_words)
{
	uint32_t format;

	/* No key, no format: the words as the shader reads them. */
	*format_words = words;
	if (reader->key == NULL || location >= POWERVR_SHADER_INPUTS)
		return 0;
	format = reader->key->attribute_formats[location];
	if (format == 0)
		return 0;

	/* The 32-bit float formats, one word a channel. */
	if (format == FORMAT_R32_SFLOAT) {
		*format_words = 1;
	} else if (format == FORMAT_R32G32_SFLOAT) {
		*format_words = 2;
	} else if (format == FORMAT_R32G32B32_SFLOAT) {
		*format_words = 3;
	} else if (format == FORMAT_R32G32B32A32_SFLOAT) {
		*format_words = 4;
	} else {
		/* Other formats need the shader to convert them; outside the subset for now. */
		return ENOTSUP;
	}

	/* The shader never reads past its own words. */
	if (*format_words > words)
		*format_words = words;
	return 0;
}

/* Reports the pixel output registers a colour attachment format takes (0 when the compiler does not pack it). */
static uint32_t
color_format_known(
	uint32_t format)
{
	/* The 8-bit unsigned normalised four-channel formats: one register. */
	if (format == FORMAT_R8G8B8A8_UNORM)
		return 1;
	if (format == FORMAT_B8G8R8A8_UNORM)
		return 1;

	/* The 16-bit float four-channel format: two registers, two channels each. */
	if (format == FORMAT_R16G16B16A16_SFLOAT)
		return 2;

	/* Not a format the pixel output packs. */
	return 0;
}

/* Reports whether the module has a kill (OpKill or OpTerminateInvocation) anywhere past its header. */
static int
module_kills(
	const struct shader_spirv_reader *reader)
{
	size_t at;
	uint32_t length;
	uint32_t opcode;

	/* Each instruction in turn. */
	at = SPIRV_HEADER_WORDS;
	while (at < reader->count) {
		length = reader->words[at] >> 16;
		opcode = reader->words[at] & 0xffffU;
		if (opcode == OP_KILL || opcode == OP_TERMINATE_INVOCATION)
			return 1;
		if (length == 0)
			return 0;
		at += length;
	}

	/* No kill. */
	return 0;
}
