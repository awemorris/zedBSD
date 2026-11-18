/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GSI1 compiler's vertex and fragment inputs and outputs.
 *
 * A vertex warp starts with the bound attributes packed into r0 up: the
 * driver puts the attribute locations in order into the pipeline's slots,
 * and slot i's components (its format's) arrive from the sum of the
 * components of the slots before it.  The program copies them where it
 * reads them (a component the format lacks reads 0, or 1 for the fourth),
 * reads the vertex and instance index as operands 120 and 121, and exports
 * each component of the position, the point size and the varyings right
 * after its last store, so that no output holds a register to the end.
 * Location L component c of the varyings is varying word 4 L + c: a vertex
 * program exports it to 0x010 + 4 L + c, and a fragment program reads it
 * from the primitive table's word 4 + 4 L + c of each vertex with lda.  The
 * table's words 0-3 are each vertex's clip-space position.
 *
 * A fragment warp starts with the barycentrics in r0-r2 (perspective
 * correct, against the primitive's vertices), the depth in r3, the pixel
 * centre in r4 and r5, the sample mask in r6 and, when the program asks for
 * it, the centroid's barycentrics in r7-r9.  A smooth input is the
 * vertices' words weighed by the barycentrics (a line's two, a triangle's
 * three); a flat one, and every input of a point, is vertex 0's word (the
 * provoking vertex).  A noperspective input, and FragCoord.w, need each
 * vertex's clip w, the table's word 3: the screen-linear barycentrics are
 * b_i w_i / (b_0 w_0 + b_1 w_1 + b_2 w_2), and 1 / w at the pixel is the
 * reciprocal of that sum (a point's is 1 / w_0).  The start computes them
 * once.
 *
 * Every fragment program ends, with the EXEC of its start, by exporting a
 * sample mask: the program's own, or all ones (which leaves the coverage
 * as it is), so that every lane reaches the render output even when it
 * exported nothing else.
 */

#include "gsi1-private.h"

#include "drivers/gpu/compiler/spirv.h"

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <stddef.h>
#include <stdint.h>

/*
 * The bits of the float 1.0, the fourth component an attribute without
 * one reads.
 *
 * Values referenced: IEEE 754-2019 (the binary32 encoding of 1.0).
 */
#define GSI1_IO_FLOAT_ONE		0x3f800000U

/* What a fragment program reads of the barycentrics, from its inputs. */
struct gsi1_fragment_needs {
	int center;
	int centroid;
	int linear;
	int linear_centroid;
	int reciprocal_w;
};

static int prepare_vertex(struct gsi1_lowering *lowering);
static int prepare_attributes(struct gsi1_lowering *lowering);
static int prepare_fragment(struct gsi1_lowering *lowering);
static uint32_t primitive_vertices(const struct gsi1_lowering *lowering);
static int input_interpolation(const struct gsi1_lowering *lowering, uint32_t location, uint32_t *first);
static uint32_t output_channel(const struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);
static void needs_of(const struct gsi1_lowering *lowering, struct gsi1_fragment_needs *needs);
static int prologue_fragment(struct gsi1_lowering *lowering);
static int prologue_vertex(struct gsi1_lowering *lowering);
static int take_input(struct gsi1_lowering *lowering, uint32_t input, uint32_t reg);
static int screen_linear(struct gsi1_lowering *lowering, uint32_t first_barycentric, uint32_t first_linear, int keep_linear, int keep_reciprocal);
static int point_reciprocal_w(struct gsi1_lowering *lowering);
static int lower_fragment_input(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_varying(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static void emit_lda(struct gsi1_lowering *lowering, uint32_t rd, uint32_t word, uint32_t vertex);
static void emit_export(struct gsi1_lowering *lowering, uint32_t operand, uint32_t target);
static int export_channel(struct gsi1_lowering *lowering, uint32_t channel, uint32_t target);
static int lower_vertex_output(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t channel);
static int finish_fragment(struct gsi1_lowering *lowering);

/*
 * Lists the inputs an IR instruction reads: the registers the hardware or
 * the start of the program filled.  Returns how many it put in inputs[].
 */
uint32_t
gsi1_io_inputs(
	const struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t *inputs)
{
	uint32_t location;
	uint32_t component;
	uint32_t first;
	int interpolation;

	/* An input attachment's read takes the pixel from FragCoord. */
	if (instruction->op == DRV_GPU_IR_IMAGE_READ &&
	    lowering->ir->stage == DRV_GPU_STAGE_FRAGMENT &&
	    instruction->location < lowering->ir->uniform_count &&
	    lowering->ir->uniforms[instruction->location].kind == DRV_GPU_IR_UNIFORM_INPUT_ATTACHMENT) {
		inputs[0] = GSI1_INPUT_X;
		inputs[1] = GSI1_INPUT_Y;
		return 2U;
	}

	/* Only an input's read reads one otherwise. */
	if (instruction->op != DRV_GPU_IR_LOAD_INPUT)
		return 0U;
	location = instruction->location;
	component = instruction->component & 3U;

	/* A vertex program: an attribute's component (the vertex and instance index are operands). */
	if (lowering->ir->stage == DRV_GPU_STAGE_VERTEX) {
		if (location < GHCI_GSI1_LOCATIONS) {
			inputs[0] = GSI1_INPUT_ATTRIBUTE + 4U * location + component;
			return 1U;
		}

		/* Anything else is an operand or is refused where it is lowered. */
		return 0U;
	}

	/* Only a fragment program has the others. */
	if (lowering->ir->stage != DRV_GPU_STAGE_FRAGMENT)
		return 0U;

	/* FragCoord: x, y, z from the hardware, w from the start. */
	if (location == DRV_GPU_SHADER_LOCATION_FRAG_COORD) {
		if (component == 0U) {
			inputs[0] = GSI1_INPUT_X;
		} else if (component == 1U) {
			inputs[0] = GSI1_INPUT_Y;
		} else if (component == 2U) {
			inputs[0] = GSI1_INPUT_Z;
		} else {
			inputs[0] = GSI1_INPUT_RECIPROCAL_W;
		}
		return 1U;
	}

	/* The sample mask in, when the hardware writes it. */
	if (location == DRV_GPU_SHADER_LOCATION_SAMPLE_MASK) {
		if (lowering->options.sample_mask_input == 0U)
			return 0U;
		inputs[0] = GSI1_INPUT_SAMPLE_MASK;
		return 1U;
	}

	/* A point's PointCoord is in r0 and r1. */
	if (location == DRV_GPU_SHADER_LOCATION_POINT_COORD) {
		if (lowering->options.primitive != GHCI_GSI1_PRIMITIVE_POINT)
			return 0U;
		inputs[0] = GSI1_INPUT_B0 + (component & 1U);
		return 1U;
	}

	/* A varying reads the barycentrics its interpolation takes: a line's two, a triangle's three. */
	if (location >= GHCI_GSI1_LOCATIONS)
		return 0U;
	interpolation = input_interpolation(lowering, location, &first);
	if (interpolation == 0)
		return 0U;
	inputs[0] = first;
	inputs[1] = first + 1U;
	inputs[2] = first + 2U;

	/* The primitive's barycentrics. */
	return primitive_vertices(lowering);
}

/*
 * Checks a graphics program's inputs and outputs and finds what the
 * pipeline state takes from them: the attribute registers, the varyings,
 * the colour outputs and the fragment flags.
 */
int
gsi1_io_prepare(
	struct gsi1_lowering *lowering)
{
	int error;

	/* A compute program has none. */
	if (lowering->ir->stage == DRV_GPU_STAGE_COMPUTE)
		return 0;

	/* The stage's own inputs and outputs. */
	if (lowering->ir->stage == DRV_GPU_STAGE_VERTEX) {
		error = prepare_vertex(lowering);
	} else {
		error = prepare_fragment(lowering);
	}
	if (error != 0)
		return error;

	/* Succeeded: the interface is known. */
	return 0;
}

/*
 * Plans where each output channel is exported and what the export reads.
 *
 * A vertex program exports a channel right after its last store, from the
 * value stored, so that no output holds a register to the end.  A fragment
 * program exports at its end (a discard must come before every export): a
 * channel stored once outside every loop is exported from where its value
 * lives, which then lives to the end; any other gets a register of its own
 * at its first store.
 */
void
gsi1_io_plan_outputs(
	struct gsi1_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	uint8_t stores[GSI1_OUTPUT_CHANNELS];
	uint8_t in_loop[GSI1_OUTPUT_CHANNELS];
	uint32_t value[GSI1_OUTPUT_CHANNELS];
	uint32_t last[GSI1_OUTPUT_CHANNELS];
	uint32_t index;
	uint32_t channel;
	uint32_t depth;

	/* Names the module being scanned. */
	ir = lowering->ir;

	/* No channel is stored yet. */
	kern_memset(stores, 0, sizeof(stores));
	kern_memset(in_loop, 0, sizeof(in_loop));

	/* Counts each channel's stores, notes those inside a loop, and finds each channel's last one. */
	depth = 0U;
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];

		/* The loop nest the instruction is in. */
		if (instruction->op == DRV_GPU_IR_LOOP_BEGIN) {
			depth++;
			continue;
		}

		/* Leaving a loop. */
		if (instruction->op == DRV_GPU_IR_LOOP_END) {
			if (depth != 0U)
				depth--;
			continue;
		}

		/* Only a store to a channel the program has is counted. */
		if (instruction->op != DRV_GPU_IR_STORE_OUTPUT)
			continue;
		channel = output_channel(lowering, instruction);
		if (channel == GSI1_OUTPUT_CHANNELS)
			continue;

		/* The store, with its value, its nest and its place. */
		if (stores[channel] < 2U)
			stores[channel]++;
		value[channel] = instruction->src[0];
		last[channel] = index;
		if (depth != 0U)
			in_loop[channel] = 1U;
	}

	/*
	 * A vertex program exports each channel at its last store.  Every lane
	 * runs that store after any other store to the channel: outside a loop
	 * every lane runs it, and a lane runs the whole body of each pass of a
	 * loop (a lane leaves only at the end of a pass), so the last pass it
	 * runs reaches the last store.  The export there, under the EXEC of
	 * the pass, leaves the value of the lane's last store, as an export at
	 * the end of the program would.
	 */
	if (ir->stage == DRV_GPU_STAGE_VERTEX) {
		for (channel = 0U; channel < GSI1_OUTPUT_CHANNELS; channel++) {
			if (stores[channel] == 0U)
				continue;

			/* The channel's export follows its last store. */
			lowering->output_last[channel] = last[channel];
		}
		return;
	}

	/* A channel stored once outside every loop is exported from its value, which lives to the end. */
	for (channel = 0U; channel < GSI1_OUTPUT_CHANNELS; channel++) {
		if (stores[channel] != 1U)
			continue;
		if (in_loop[channel] != 0U)
			continue;

		/* The export reads the value; a register value lives to it. */
		lowering->output_value[channel] = value[channel];
		if (lowering->kind[value[channel]] == GSI1_VALUE_REGISTER)
			lowering->last_use[value[channel]] = GSI1_END_OF_PROGRAM(ir);
	}
}

/*
 * Takes the inputs' registers before anything else, and computes the
 * inputs the start of a fragment program makes.
 */
int
gsi1_io_prologue(
	struct gsi1_lowering *lowering)
{
	int error;

	/* The stage's inputs. */
	error = 0;
	if (lowering->ir->stage == DRV_GPU_STAGE_FRAGMENT) {
		error = prologue_fragment(lowering);
	} else if (lowering->ir->stage == DRV_GPU_STAGE_VERTEX) {
		error = prologue_vertex(lowering);
	}
	if (error != 0)
		return error;

	/* A list that could not grow fails the compile. */
	if (lowering->out_of_memory != 0)
		return ENOMEM;

	/* Succeeded: the inputs are in their registers. */
	return 0;
}

/* Lowers the read of an input of a vertex or fragment program. */
int
gsi1_io_lower_input(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	uint32_t inputs[GSI1_INPUT_COUNT];
	uint32_t input_count;
	uint32_t location;
	uint32_t component;
	uint32_t bits;
	uint32_t reg;
	int error;

	/* A fragment program's input has a lowering of its own. */
	if (lowering->ir->stage == DRV_GPU_STAGE_FRAGMENT) {
		error = lower_fragment_input(lowering, instruction, dst);
		if (error != 0)
			return error;

		/* Succeeded: the fragment input is in dst. */
		return 0;
	}

	/* A compute program has no inputs. */
	if (lowering->ir->stage != DRV_GPU_STAGE_VERTEX) {
		error = gsi1_refuse(lowering, EINVAL, "an input of a compute shader");
		return error;
	}

	/* The vertex and instance index are operands and need no instruction. */
	if (dst == GSI1_NO_OPERAND)
		return 0;

	/* A vertex program copies the attribute's component. */
	input_count = gsi1_io_inputs(lowering, instruction, inputs);
	if (input_count != 1U) {
		error = gsi1_refuse(lowering, ENOTSUP, "a vertex input GSI1 does not have");
		return error;
	}

	/* A component the bound format lacks: 0, and 1 (1.0 for a float format) for the fourth. */
	location = instruction->location;
	component = instruction->component & 3U;
	if (component >= lowering->attribute_components[location]) {
		bits = 0U;
		if (component == 3U && ((lowering->options.attribute_integer >> location) & 1U) != 0U) {
			bits = 1U;
		} else if (component == 3U) {
			bits = GSI1_IO_FLOAT_ONE;
		}
		gsi1_emit_constant(lowering, dst, bits);
		return 0;
	}

	/* The input's register: the start took one for every component read. */
	reg = lowering->input_reg[inputs[0]];
	if (reg == GSI1_NO_OPERAND) {
		error = gsi1_refuse(lowering, ENOTSUP, "an attribute component without its register");
		return error;
	}

	/* Copies the input. */
	gsi1_emit_move(lowering, dst, reg);

	/* Succeeded: the vertex input is in dst. */
	return 0;
}

/*
 * Lowers the write of an output.  A vertex program exports the value at
 * the channel's last store and does nothing at the others.  A fragment
 * program does nothing when the export at its end reads the value where it
 * lives, and otherwise copies the value into the channel's register, taken
 * at its first store and kept to the end.
 */
int
gsi1_io_lower_output(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	uint32_t channel;
	uint32_t reg;
	uint32_t source;
	int error;

	/* The output channel stored. */
	channel = output_channel(lowering, instruction);
	if (channel == GSI1_OUTPUT_CHANNELS) {
		error = gsi1_refuse(lowering, ENOTSUP, "an output GSI1 does not export");
		return error;
	}
	lowering->output_written[channel] = 1U;

	/* A vertex program's store has a lowering of its own. */
	if (lowering->ir->stage == DRV_GPU_STAGE_VERTEX) {
		error = lower_vertex_output(lowering, instruction, channel);
		if (error != 0)
			return error;

		/* Succeeded: the vertex output is exported, or a later store exports it. */
		return 0;
	}

	/* The export reads the value where it lives. */
	if (lowering->output_value[channel] == instruction->src[0])
		return 0;

	/* The channel's own register, taken at its first store. */
	if (lowering->output_reg[channel] == GSI1_NO_OPERAND) {
		error = gsi1_take_register(lowering, &reg);
		if (error != 0)
			return error;
		lowering->output_reg[channel] = (uint8_t)reg;
	}

	/* A constant goes straight into the register. */
	if (lowering->kind[instruction->src[0]] == GSI1_VALUE_CONSTANT) {
		gsi1_emit_constant(lowering, lowering->output_reg[channel], lowering->constant[instruction->src[0]]);
		return 0;
	}

	/* Finds where the value is read from. */
	error = gsi1_source(lowering, instruction->src[0], &source);
	if (error != 0)
		return error;

	/* Copies the value into the channel's register. */
	gsi1_emit_move(lowering, lowering->output_reg[channel], source);

	/* Succeeded: the output waits for the export. */
	return 0;
}

/*
 * Emits the exports at the end of a fragment program; a vertex program's
 * were made at their stores.
 */
int
gsi1_io_finish(
	struct gsi1_lowering *lowering)
{
	int error;

	/* Only a fragment program exports at its end. */
	if (lowering->ir->stage != DRV_GPU_STAGE_FRAGMENT)
		return 0;

	/* The fragment program's exports. */
	error = finish_fragment(lowering);
	if (error != 0)
		return error;

	/* Succeeded: the outputs are exported. */
	return 0;
}

/*
 * Checks a vertex program's inputs and outputs, places its attributes,
 * and counts the varyings it writes.
 */
static int
prepare_vertex(
	struct gsi1_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t index;
	uint32_t location;
	int error;

	/* Names the module being scanned. */
	ir = lowering->ir;

	/* Each input read and output written by an instruction that is lowered. */
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		lowering->index = index;
		location = instruction->location;
		if (lowering->dead[index] != 0U)
			continue;

		/* An input: an attribute's location or an index. */
		if (instruction->op == DRV_GPU_IR_LOAD_INPUT) {
			if (location >= GHCI_GSI1_LOCATIONS &&
			    location != DRV_GPU_SHADER_LOCATION_VERTEX_INDEX &&
			    location != DRV_GPU_SHADER_LOCATION_INSTANCE_INDEX) {
				error = gsi1_refuse(lowering, ENOTSUP, "a vertex input GSI1 does not have");
				return error;
			}
			continue;
		}

		/* Only the outputs are looked at further. */
		if (instruction->op != DRV_GPU_IR_STORE_OUTPUT)
			continue;

		/* The position and the point size are exported to their own targets. */
		if (location == DRV_GPU_IR_LOCATION_POSITION)
			continue;
		if (location == DRV_GPU_IR_LOCATION_POINT_SIZE)
			continue;

		/* Any other output is a varying location's. */
		if (location >= GHCI_GSI1_LOCATIONS) {
			error = gsi1_refuse(lowering, ENOTSUP, "a vertex output GSI1 does not export");
			return error;
		}

		/* The varyings run up to the highest location written. */
		if (location + 1U > lowering->varying_count)
			lowering->varying_count = location + 1U;
	}

	/* The attributes' registers. */
	error = prepare_attributes(lowering);
	if (error != 0)
		return error;

	/* Succeeded: the vertex interface is known. */
	lowering->index = 0U;
	return 0;
}

/*
 * Places the attributes: each bound location's components (the pipeline's
 * map, or the variable's) from the sum of the components of the bound
 * locations below it, at most 60 registers in all.
 */
static int
prepare_attributes(
	struct gsi1_lowering *lowering)
{
	const struct drv_gpu_shader_ir_io *io;
	uint32_t location;
	uint32_t components;
	uint32_t base;
	int error;

	/* Each location in order, as the driver fills the pipeline's slots. */
	base = 0U;
	for (location = 0U; location < GHCI_GSI1_LOCATIONS; location++) {
		/* The bound attribute's components: the pipeline's, or the variable's when the pipeline gives none. */
		components = 0U;
		if (lowering->options.has_attribute_map != 0U) {
			components = lowering->options.attribute_components[location];
		} else {
			io = gsi1_find_input(lowering->ir, location);
			if (io != NULL) {
				components = io->components;
				if (components == 0U || components > 4U)
					components = 4U;
			}
		}

		/* A format has one to four components. */
		if (components > 4U) {
			error = gsi1_refuse(lowering, EINVAL, "an attribute of more than four components");
			return error;
		}

		/* The location's registers. */
		lowering->attribute_base[location] = base;
		lowering->attribute_components[location] = components;
		base += components;
	}

	/* The fetcher writes 60 registers at most. */
	if (base > GSI1_ABI_ATTRIBUTE_REGISTERS) {
		error = gsi1_refuse(lowering, ENOTSUP, "the attributes take more than 60 registers");
		return error;
	}

	/* Succeeded: the attributes have their registers. */
	lowering->attribute_registers = base;
	return 0;
}

/*
 * Checks a fragment program's inputs and outputs, and finds the varyings
 * it reads, the colour attachments it exports and its flags.
 */
static int
prepare_fragment(
	struct gsi1_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	struct gsi1_fragment_needs needs;
	uint32_t index;
	uint32_t location;
	int error;

	/* Names the module being scanned. */
	ir = lowering->ir;

	/* Each input read and output written by an instruction that is lowered. */
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		lowering->index = index;
		location = instruction->location;
		if (lowering->dead[index] != 0U)
			continue;

		/* An output: a colour attachment's component, the second colour, the depth or the sample mask. */
		if (instruction->op == DRV_GPU_IR_STORE_OUTPUT) {
			if (location == DRV_GPU_IR_LOCATION_FRAG_DEPTH) {
				lowering->flags |= GHCI_GSI1_FLAG_DEPTH_EXPORT;
			} else if (location == DRV_GPU_IR_LOCATION_SECOND_COLOR) {
				lowering->color_outputs |= GHCI_GSI1_OUTPUT_SECOND_COLOR;
			} else if (location < GSI1_COLOR_ATTACHMENTS) {
				lowering->color_outputs |= 1U << location;
			} else if (location != DRV_GPU_IR_LOCATION_SAMPLE_MASK) {
				error = gsi1_refuse(lowering, ENOTSUP, "a fragment output GSI1 does not export");
				return error;
			}
			continue;
		}

		/* Only the inputs are looked at further. */
		if (instruction->op != DRV_GPU_IR_LOAD_INPUT)
			continue;

		/* The built-ins GSI1 has. */
		if (location == DRV_GPU_SHADER_LOCATION_FRAG_COORD ||
		    location == DRV_GPU_SHADER_LOCATION_FRONT_FACING ||
		    location == DRV_GPU_SHADER_LOCATION_PRIMITIVE_ID ||
		    location == DRV_GPU_SHADER_LOCATION_POINT_COORD ||
		    location == DRV_GPU_SHADER_LOCATION_SAMPLE_MASK) {
			continue;
		}

		/* Any other input is a varying location's. */
		if (location >= GHCI_GSI1_LOCATIONS) {
			error = gsi1_refuse(lowering, ENOTSUP, "a fragment input GSI1 does not have");
			return error;
		}

		/* The varyings run up to the highest location read. */
		if (location + 1U > lowering->varying_count)
			lowering->varying_count = location + 1U;
	}

	/* The centroid's barycentrics are asked of the rasterizer. */
	needs_of(lowering, &needs);
	if (needs.centroid != 0 || needs.linear_centroid != 0)
		lowering->flags |= GHCI_GSI1_FLAG_CENTROID;

	/* Succeeded: the fragment interface is known. */
	lowering->index = 0U;
	return 0;
}

/* Reports the vertices whose words a fragment program's smooth input weighs: a line's two, a triangle's three. */
static uint32_t
primitive_vertices(
	const struct gsi1_lowering *lowering)
{
	/* A line has no vertex 2 (b2 is zero). */
	if (lowering->options.primitive == GHCI_GSI1_PRIMITIVE_LINE)
		return 2U;

	/* Succeeded: a triangle's three. */
	return 3U;
}

/*
 * Reports how a fragment program interpolates a varying location: 0 when
 * it reads vertex 0's word (flat, or any input of a point), 1 with the
 * barycentrics whose first input is *first.
 */
static int
input_interpolation(
	const struct gsi1_lowering *lowering,
	uint32_t location,
	uint32_t *first)
{
	const struct drv_gpu_shader_ir_io *io;
	int centroid;

	/* Every input of a point is vertex 0's. */
	if (lowering->options.primitive == GHCI_GSI1_PRIMITIVE_POINT)
		return 0;

	/* A flat input is vertex 0's (the provoking vertex). */
	io = gsi1_find_input(lowering->ir, location);
	if (io != NULL && io->flat != 0U)
		return 0;

	/* The centroid differs from the centre only with several samples. */
	centroid = 0;
	if (io != NULL && io->centroid != 0U && lowering->options.samples > 1U)
		centroid = 1;

	/* Screen-linear or perspective, at the centre or the centroid. */
	if (io != NULL && io->noperspective != 0U) {
		*first = GSI1_INPUT_LINEAR_B0;
		if (centroid != 0)
			*first = GSI1_INPUT_LINEAR_CENTROID_B0;
	} else {
		*first = GSI1_INPUT_B0;
		if (centroid != 0)
			*first = GSI1_INPUT_CENTROID_B0;
	}

	/* Succeeded: the input is interpolated. */
	return 1;
}

/*
 * Reports the output channel an output write stores (GSI1_OUTPUT_CHANNELS
 * for one GSI1 does not export).
 */
static uint32_t
output_channel(
	const struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	uint32_t location;
	uint32_t component;

	/* The location and the component written. */
	location = instruction->location;
	component = instruction->component;
	if (component > 3U)
		return GSI1_OUTPUT_CHANNELS;

	/* A vertex program: the position, the point size, or a varying location. */
	if (lowering->ir->stage == DRV_GPU_STAGE_VERTEX) {
		if (location == DRV_GPU_IR_LOCATION_POSITION)
			return GSI1_OUTPUT_POSITION + component;
		if (location == DRV_GPU_IR_LOCATION_POINT_SIZE)
			return GSI1_OUTPUT_POINT_SIZE;
		if (location < GHCI_GSI1_LOCATIONS)
			return GSI1_OUTPUT_LOCATIONS + 4U * location + component;
		return GSI1_OUTPUT_CHANNELS;
	}

	/* Only a fragment program has the others. */
	if (lowering->ir->stage != DRV_GPU_STAGE_FRAGMENT)
		return GSI1_OUTPUT_CHANNELS;

	/* A fragment program: a colour attachment, the second colour, the depth or the sample mask. */
	if (location == DRV_GPU_IR_LOCATION_FRAG_DEPTH)
		return GSI1_OUTPUT_DEPTH;
	if (location == DRV_GPU_IR_LOCATION_SAMPLE_MASK)
		return GSI1_OUTPUT_SAMPLE_MASK;
	if (location == DRV_GPU_IR_LOCATION_SECOND_COLOR)
		return GSI1_OUTPUT_SECOND_COLOR + component;
	if (location < GSI1_COLOR_ATTACHMENTS)
		return GSI1_OUTPUT_COLOR + 4U * location + component;

	/* Anything else is not exported. */
	return GSI1_OUTPUT_CHANNELS;
}

/* Finds what a fragment program reads of the barycentrics, from its inputs' lives. */
static void
needs_of(
	const struct gsi1_lowering *lowering,
	struct gsi1_fragment_needs *needs)
{
	/* An input whose life ends after the start is read by an instruction. */
	kern_memset(needs, 0, sizeof(*needs));
	if (lowering->input_until[GSI1_INPUT_B0] != 0U)
		needs->center = 1;
	if (lowering->input_until[GSI1_INPUT_CENTROID_B0] != 0U)
		needs->centroid = 1;
	if (lowering->input_until[GSI1_INPUT_LINEAR_B0] != 0U)
		needs->linear = 1;
	if (lowering->input_until[GSI1_INPUT_LINEAR_CENTROID_B0] != 0U)
		needs->linear_centroid = 1;
	if (lowering->input_until[GSI1_INPUT_RECIPROCAL_W] != 0U)
		needs->reciprocal_w = 1;
}

/*
 * The start of a fragment program: takes the registers of the hardware's
 * inputs the program reads, and computes the screen-linear barycentrics
 * and 1 / w when the program reads them.
 */
static int
prologue_fragment(
	struct gsi1_lowering *lowering)
{
	struct gsi1_fragment_needs needs;
	uint32_t input;
	uint32_t vertices;
	int point;
	int keep;
	int error;

	/* What the start computes, and from the barycentrics of how many vertices. */
	needs_of(lowering, &needs);
	vertices = primitive_vertices(lowering);
	point = 0;
	if (lowering->options.primitive == GHCI_GSI1_PRIMITIVE_POINT)
		point = 1;

	/*
	 * The hardware's inputs: those an instruction reads, and the
	 * barycentrics the start computes from (a point's 1 / w needs none).
	 */
	for (input = GSI1_INPUT_B0; input <= GSI1_INPUT_CENTROID_B2; input++) {
		keep = 0;
		if (lowering->input_until[input] != 0U)
			keep = 1;
		if (input < GSI1_INPUT_B0 + vertices && (needs.linear != 0 || (needs.reciprocal_w != 0 && point == 0)))
			keep = 1;
		if (input >= GSI1_INPUT_CENTROID_B0 && input < GSI1_INPUT_CENTROID_B0 + vertices && needs.linear_centroid != 0)
			keep = 1;
		if (keep == 0)
			continue;

		/* The input's register is the hardware's: r0-r9 in the order of the inputs. */
		error = take_input(lowering, input, input - GSI1_INPUT_B0);
		if (error != 0)
			return error;
	}

	/* A point's 1 / w is vertex 0's. */
	if (point != 0 && needs.reciprocal_w != 0) {
		error = point_reciprocal_w(lowering);
		if (error != 0)
			return error;
	}

	/* The screen-linear barycentrics and 1 / w at the centre. */
	if (needs.linear != 0 || (needs.reciprocal_w != 0 && point == 0)) {
		error = screen_linear(lowering, GSI1_INPUT_B0, GSI1_INPUT_LINEAR_B0, needs.linear, needs.reciprocal_w);
		if (error != 0)
			return error;
	}

	/* The screen-linear barycentrics at the centroid. */
	if (needs.linear_centroid != 0) {
		error = screen_linear(lowering, GSI1_INPUT_CENTROID_B0, GSI1_INPUT_LINEAR_CENTROID_B0, 1, 0);
		if (error != 0)
			return error;
	}

	/* The scratch registers of the start go back. */
	for (input = 0U; input < lowering->scratch_count; input++)
		gsi1_give_register(lowering, lowering->scratch[input]);
	lowering->scratch_count = 0U;

	/* The inputs only the start read go back. */
	for (input = 0U; input < GSI1_INPUT_COUNT; input++) {
		if (lowering->input_reg[input] == GSI1_NO_OPERAND)
			continue;
		if (lowering->input_until[input] != 0U)
			continue;

		/* The input's register goes back. */
		gsi1_give_register(lowering, lowering->input_reg[input]);
		lowering->input_reg[input] = GSI1_NO_OPERAND;
	}

	/* Succeeded: the fragment inputs are in their registers. */
	return 0;
}

/*
 * The start of a vertex program: takes the registers of the attribute
 * components it reads, where the fetcher writes them.
 */
static int
prologue_vertex(
	struct gsi1_lowering *lowering)
{
	uint32_t location;
	uint32_t component;
	uint32_t input;
	int error;

	/* Each component of each attribute location that a lowered instruction reads. */
	for (location = 0U; location < GHCI_GSI1_LOCATIONS; location++) {
		for (component = 0U; component < 4U; component++) {
			input = GSI1_INPUT_ATTRIBUTE + 4U * location + component;
			if (lowering->input_until[input] == 0U)
				continue;

			/* A component the format lacks is made where it is read. */
			if (component >= lowering->attribute_components[location])
				continue;

			/* The component's register is the fetcher's. */
			error = take_input(lowering, input, lowering->attribute_base[location] + component);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the vertex inputs are in their registers. */
	return 0;
}

/* Takes a fixed register for an input. */
static int
take_input(
	struct gsi1_lowering *lowering,
	uint32_t input,
	uint32_t reg)
{
	int error;

	/* The register must be free and within the lane's. */
	if (reg >= lowering->register_limit || lowering->taken[reg] != 0U) {
		error = gsi1_refuse(lowering, ENOTSUP, "an input register is not available");
		return error;
	}

	/* The register is the input's. */
	lowering->taken[reg] = 1U;
	if (reg + 1U > lowering->register_highest)
		lowering->register_highest = reg + 1U;
	lowering->input_reg[input] = (uint8_t)reg;

	/* Succeeded: the input has its register. */
	return 0;
}

/*
 * Computes the screen-linear barycentrics s_i = b_i w_i / D, D = b_0 w_0
 * + b_1 w_1 + b_2 w_2 (a line's two terms), from the perspective
 * barycentrics starting at the input `first_barycentric` and each
 * vertex's clip w (the primitive table's word 3), into the inputs from
 * `first_linear`; and 1 / D, the reciprocal of w at the pixel, when it is
 * kept.
 */
static int
screen_linear(
	struct gsi1_lowering *lowering,
	uint32_t first_barycentric,
	uint32_t first_linear,
	int keep_linear,
	int keep_reciprocal)
{
	uint32_t weighted[3];
	uint32_t reciprocal;
	uint32_t sum;
	uint32_t reg;
	uint32_t vertex;
	uint32_t vertices;
	int error;

	/* Each vertex's clip w, weighed by its barycentric: b_i w_i. */
	vertices = primitive_vertices(lowering);
	for (vertex = 0U; vertex < vertices; vertex++) {
		error = gsi1_scratch(lowering, &weighted[vertex]);
		if (error != 0)
			return error;
		emit_lda(lowering, weighted[vertex], GSI1_PT_POSITION(3U), vertex);
		gsi1_emit_float(lowering,
				GSI1_F7_FMUL,
				GSI1_RM_RNE,
				weighted[vertex],
				weighted[vertex],
				lowering->input_reg[first_barycentric + vertex],
				0U);
	}

	/* Takes the scratch register of the sum. */
	error = gsi1_scratch(lowering, &sum);
	if (error != 0)
		return error;

	/* The sum, w at the pixel. */
	gsi1_emit_float(lowering, GSI1_F7_FADD, GSI1_RM_RNE, sum, weighted[0], weighted[1], 0U);
	if (vertices == 3U)
		gsi1_emit_float(lowering, GSI1_F7_FADD, GSI1_RM_RNE, sum, sum, weighted[2], 0U);

	/* Its reciprocal: 1 / w, kept as the input FragCoord.w reads, or scratch. */
	if (keep_reciprocal != 0) {
		error = gsi1_take_register(lowering, &reciprocal);
		if (error != 0)
			return error;
		lowering->input_reg[GSI1_INPUT_RECIPROCAL_W] = (uint8_t)reciprocal;
	} else {
		error = gsi1_scratch(lowering, &reciprocal);
		if (error != 0)
			return error;
	}
	gsi1_emit_transcendental(lowering, GSI1_XU_RCP, reciprocal, sum);

	/* The screen-linear barycentrics, when an input reads them. */
	if (keep_linear == 0)
		return 0;

	/* Each one into a register of its own. */
	for (vertex = 0U; vertex < vertices; vertex++) {
		error = gsi1_take_register(lowering, &reg);
		if (error != 0)
			return error;
		lowering->input_reg[first_linear + vertex] = (uint8_t)reg;
		gsi1_emit_float(lowering, GSI1_F7_FMUL, GSI1_RM_RNE, reg, weighted[vertex], reciprocal, 0U);
	}

	/* Succeeded: the screen-linear barycentrics are computed. */
	return 0;
}

/* Computes a point's 1 / w, the reciprocal of vertex 0's clip w, into a register of its own. */
static int
point_reciprocal_w(
	struct gsi1_lowering *lowering)
{
	uint32_t w;
	uint32_t reciprocal;
	int error;

	/* Takes a scratch register for vertex 0's w. */
	error = gsi1_scratch(lowering, &w);
	if (error != 0)
		return error;

	/* Takes the register of the reciprocal, which FragCoord.w reads. */
	error = gsi1_take_register(lowering, &reciprocal);
	if (error != 0)
		return error;
	lowering->input_reg[GSI1_INPUT_RECIPROCAL_W] = (uint8_t)reciprocal;

	/* rcp of the primitive table's word 3 of vertex 0. */
	emit_lda(lowering, w, GSI1_PT_POSITION(3U), 0U);
	gsi1_emit_transcendental(lowering, GSI1_XU_RCP, reciprocal, w);

	/* Succeeded: 1 / w is in its register. */
	return 0;
}

/* Lowers the read of a fragment program's input. */
static int
lower_fragment_input(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	uint32_t location;
	uint32_t component;
	uint32_t reg;
	int error;

	/* Names the location and component read. */
	location = instruction->location;
	component = instruction->component & 3U;

	/* FragCoord: the pixel centre, the depth clamped to [0, 1], and 1 / w. */
	if (location == DRV_GPU_SHADER_LOCATION_FRAG_COORD) {
		if (component == 0U) {
			gsi1_emit_move(lowering, dst, lowering->input_reg[GSI1_INPUT_X]);
		} else if (component == 1U) {
			gsi1_emit_move(lowering, dst, lowering->input_reg[GSI1_INPUT_Y]);
		} else if (component == 2U) {
			/*
			 * The rasterizer leaves r3 unclamped when it clips by depth
			 * and with several samples; FragCoord.z is within [0, 1].
			 */
			gsi1_emit_float(lowering,
					GSI1_F7_FADD,
					GSI1_RM_RNE,
					dst,
					lowering->input_reg[GSI1_INPUT_Z],
					GSI1_ZERO,
					GSI1_W1_SAT);
		} else {
			gsi1_emit_move(lowering, dst, lowering->input_reg[GSI1_INPUT_RECIPROCAL_W]);
		}
		return 0;
	}

	/* The front face and the primitive's number are special operands. */
	if (location == DRV_GPU_SHADER_LOCATION_FRONT_FACING) {
		gsi1_emit_move(lowering, dst, GSI1_FRONT);
		return 0;
	}

	/* The primitive's number. */
	if (location == DRV_GPU_SHADER_LOCATION_PRIMITIVE_ID) {
		gsi1_emit_move(lowering, dst, GSI1_PRIM);
		return 0;
	}

	/* The sample mask in: r6, or one sample on a GPU that does not write it. */
	if (location == DRV_GPU_SHADER_LOCATION_SAMPLE_MASK) {
		reg = GSI1_ONEI;
		if (lowering->options.sample_mask_input != 0U)
			reg = lowering->input_reg[GSI1_INPUT_SAMPLE_MASK];
		gsi1_emit_move(lowering, dst, reg);
		return 0;
	}

	/* PointCoord: r0 and r1 of a point; nothing defined otherwise (zero). */
	if (location == DRV_GPU_SHADER_LOCATION_POINT_COORD) {
		reg = GSI1_ZERO;
		if (lowering->options.primitive == GHCI_GSI1_PRIMITIVE_POINT)
			reg = lowering->input_reg[GSI1_INPUT_B0 + (component & 1U)];
		gsi1_emit_move(lowering, dst, reg);
		return 0;
	}

	/* A varying. */
	error = lower_varying(lowering, instruction, dst);
	if (error != 0)
		return error;

	/* Succeeded: the input is in dst. */
	return 0;
}

/*
 * Lowers the read of a varying: vertex 0's word, or the vertices' words
 * weighed by the barycentrics (lda, fmul and an fmadd for each further
 * vertex).
 */
static int
lower_varying(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	uint32_t word;
	uint32_t first;
	uint32_t first_word;
	uint32_t second_word;
	int interpolation;
	int error;

	/* The primitive table's word of the location's component. */
	word = GSI1_PT_VARYING(4U * instruction->location + (instruction->component & 3U));

	/* Vertex 0's word, read as it is. */
	interpolation = input_interpolation(lowering, instruction->location, &first);
	if (interpolation == 0) {
		emit_lda(lowering, dst, word, 0U);
		return 0;
	}

	/* Two scratch registers for the vertices' words. */
	error = gsi1_scratch(lowering, &first_word);
	if (error != 0)
		return error;

	/* Takes the second one. */
	error = gsi1_scratch(lowering, &second_word);
	if (error != 0)
		return error;

	/* v0 b0 + v1 b1, and + v2 b2 for a triangle (a line has no vertex 2). */
	emit_lda(lowering, first_word, word, 0U);
	emit_lda(lowering, second_word, word, 1U);
	gsi1_emit_float(lowering, GSI1_F7_FMUL, GSI1_RM_RNE, dst, first_word, lowering->input_reg[first], 0U);
	gsi1_emit_fused(lowering, GSI1_OP_MADD, dst, second_word, lowering->input_reg[first + 1U], dst, 0U);
	if (primitive_vertices(lowering) == 3U) {
		emit_lda(lowering, first_word, word, 2U);
		gsi1_emit_fused(lowering, GSI1_OP_MADD, dst, first_word, lowering->input_reg[first + 2U], dst, 0U);
	}

	/* Succeeded: the interpolated input is in dst. */
	return 0;
}

/* Emits lda: a primitive-table word of one vertex. */
static void
emit_lda(
	struct gsi1_lowering *lowering,
	uint32_t rd,
	uint32_t word,
	uint32_t vertex)
{
	uint32_t immediate;

	/* The table's word and the vertex. */
	immediate = GSI1_LDA(word, vertex);
	gsi1_emit(lowering, gsi1_encode_i(GSI1_OP_CUSTOM1, GSI1_F3_LDA, rd, 0U, (int32_t)immediate, 0U));
}

/* Emits exp: an operand exported to a target. */
static void
emit_export(
	struct gsi1_lowering *lowering,
	uint32_t operand,
	uint32_t target)
{
	/* The S form: the value in rs2, the target in the immediate. */
	gsi1_emit(lowering, gsi1_encode_s(GSI1_OP_CUSTOM1, GSI1_F3_EXP, 0U, operand, (int32_t)target));
}

/* Exports one output channel the program wrote to a target. */
static int
export_channel(
	struct gsi1_lowering *lowering,
	uint32_t channel,
	uint32_t target)
{
	uint32_t operand;
	int error;

	/* The value where it lives, or the channel's register. */
	if (lowering->output_value[channel] != GSI1_NOT_DEFINED) {
		error = gsi1_source(lowering, lowering->output_value[channel], &operand);
		if (error != 0)
			return error;
	} else {
		operand = lowering->output_reg[channel];
	}

	/* The export. */
	emit_export(lowering, operand, target);

	/* Succeeded: the channel is exported. */
	return 0;
}

/*
 * Lowers a vertex program's store to an output channel: at the channel's
 * last store, exports the value stored to the channel's target; at an
 * earlier store, nothing, as the last one exports what every lane ends
 * with.
 */
static int
lower_vertex_output(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t channel)
{
	uint32_t source;
	uint32_t location;
	uint32_t component;
	int error;

	/* Only the channel's last store exports. */
	if (lowering->output_last[channel] != lowering->index)
		return 0;

	/* Finds where the value stored is read from. */
	error = gsi1_source(lowering, instruction->src[0], &source);
	if (error != 0)
		return error;

	/* A position component goes to its own target. */
	if (channel < GSI1_OUTPUT_POINT_SIZE) {
		component = channel - GSI1_OUTPUT_POSITION;
		emit_export(lowering, source, GSI1_EXP_POS(component));

		/* Succeeded: the position component is exported. */
		return 0;
	}

	/* The point size goes to its own target. */
	if (channel == GSI1_OUTPUT_POINT_SIZE) {
		emit_export(lowering, source, GSI1_EXP_POINT_SIZE);
		return 0;
	}

	/* A varying component, to its word 4 L + c. */
	location = (channel - GSI1_OUTPUT_LOCATIONS) / 4U;
	component = (channel - GSI1_OUTPUT_LOCATIONS) % 4U;
	emit_export(lowering, source, GSI1_EXP_VARYING(4U * location + component));

	/* Succeeded: the varying component is exported. */
	return 0;
}

/*
 * Exports a fragment program's outputs: the colour attachments, the
 * second colour of attachment 0, the depth, and last the sample mask (the
 * program's, or all ones), which every lane exports.
 */
static int
finish_fragment(
	struct gsi1_lowering *lowering)
{
	uint32_t channel;
	uint32_t attachment;
	uint32_t component;
	uint32_t reg;
	int error;

	/* Each colour component written. */
	for (attachment = 0U; attachment < GSI1_COLOR_ATTACHMENTS; attachment++) {
		for (component = 0U; component < 4U; component++) {
			channel = GSI1_OUTPUT_COLOR + 4U * attachment + component;
			if (lowering->output_written[channel] == 0U)
				continue;

			/* Exports the component. */
			error = export_channel(lowering, channel, GSI1_EXP_COLOR(attachment, component));
			if (error != 0)
				return error;
		}
	}

	/* Each component of the second colour written. */
	for (component = 0U; component < 4U; component++) {
		channel = GSI1_OUTPUT_SECOND_COLOR + component;
		if (lowering->output_written[channel] == 0U)
			continue;

		/* Exports the component. */
		error = export_channel(lowering, channel, GSI1_EXP_COLOR0_SRC1(component));
		if (error != 0)
			return error;
	}

	/* The depth the program writes. */
	if (lowering->output_written[GSI1_OUTPUT_DEPTH] != 0U) {
		error = export_channel(lowering, GSI1_OUTPUT_DEPTH, GSI1_EXP_DEPTH);
		if (error != 0)
			return error;
	}

	/* The program's own sample mask, exported last. */
	if (lowering->output_written[GSI1_OUTPUT_SAMPLE_MASK] != 0U) {
		error = export_channel(lowering, GSI1_OUTPUT_SAMPLE_MASK, GSI1_EXP_SAMPLE_MASK);
		if (error != 0)
			return error;
		return 0;
	}

	/* Takes a register for the mask of all ones. */
	error = gsi1_scratch(lowering, &reg);
	if (error != 0)
		return error;

	/* All ones (an integer: mone is -1.0), which leaves the coverage as it is. */
	gsi1_emit_constant(lowering, reg, 0xffffffffU);
	emit_export(lowering, reg, GSI1_EXP_SAMPLE_MASK);

	/* Succeeded: every lane reaches the render output. */
	return 0;
}
