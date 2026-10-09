/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Independent validation and liveness turn the shared scalar frontend into a bounded native program. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/shader-private.h"

/* Bounded source programs cannot force unbounded kernel compiler storage or traversal. */
#define SHADER_MAX_INSTRUCTIONS 8192U
#define SHADER_MAX_VALUES 16384U

static int input_interface(struct bcm2711_shader_compiler *compiler);
static int analyze_instruction(struct bcm2711_shader_compiler *compiler, const struct i915_shader_ir_inst *instruction, uint32_t *skip_depth);
static int instruction_shape(enum i915_shader_ir_op operation, uint32_t *sources, uint32_t *destinations);
static int note_read(struct bcm2711_shader_compiler *compiler, uint32_t number);
static int output_interface(struct bcm2711_shader_compiler *compiler);

/*
 * Validates scalar source order and retains every final stage output through native program termination.
 */
int
bcm2711_shader_analyze(
	struct bcm2711_shader_compiler *compiler)
{
	const struct i915_shader_ir *ir;
	uint32_t index;
	uint32_t skip_depth;
	int error;

	/* Only the two graphics stages have a native ABI in this compiler. */
	ir = compiler->ir;
	if (ir->instruction_count == 0 || ir->instruction_count > SHADER_MAX_INSTRUCTIONS ||
	    ir->value_count == 0 || ir->value_count > SHADER_MAX_VALUES)
		return E2BIG;
	if (ir->stage != I915_STAGE_VERTEX && ir->stage != I915_STAGE_FRAGMENT)
		return ENOTSUP;

	/* Every value starts absent; value zero remains an ordinary valid scalar. */
	compiler->values = kern_calloc(ir->value_count, sizeof(*compiler->values));
	if (compiler->values == NULL)
		return ENOMEM;

	/* Marks source identities independently of the allocator's initial zero-filled storage. */
	for (index = 0; index < ir->value_count; index++) {
		compiler->values[index].definition = BCM2711_SHADER_ABSENT;
		compiler->values[index].last_read = BCM2711_SHADER_ABSENT;
	}

	/* A missing output must never accidentally resolve to the first constant in a shader. */
	for (index = 0; index < BCM2711_SHADER_OUTPUT_WORDS; index++)
		compiler->outputs[index] = BCM2711_SHADER_ABSENT;

	/* The hardware consumes inputs once in a canonical location/component order. */
	error = input_interface(compiler);
	if (error != 0)
		return error;

	/* Checks each operation before native allocation or encoding can use any of its fields. */
	skip_depth = 0;
	for (index = 0; index < ir->instruction_count; index++) {
		compiler->instruction = index;
		error = analyze_instruction(compiler, &ir->instructions[index], &skip_depth);
		if (error != 0)
			return error;
	}

	/* Skippable blocks are optimization markers, but malformed nesting is still refused. */
	if (skip_depth != 0)
		return EINVAL;

	/* Holds only the final values of declared outputs until the native epilogue consumes them. */
	error = output_interface(compiler);
	if (error != 0)
		return error;

	/* Source and uniform capacities include conservative scheduling and stage epilogues. */
	compiler->code_capacity = ir->instruction_count * 64U + 2048U;
	compiler->uniform_capacity = ir->instruction_count * 8U + 256U;
	compiler->binary->push_bytes = ir->push_bytes;
	compiler->binary->threads = 2;

	/* The source interface and all scalar lifetimes are now safe for native lowering. */
	return 0;
}

/* Establishes once-only scalar input FIFO order without relying on frontend declaration order. */
static int
input_interface(
	struct bcm2711_shader_compiler *compiler)
{
	struct bcm2711_shader_binary *binary;
	const struct i915_shader_ir_io *input;
	struct bcm2711_shader_component *component;
	uint32_t location;
	uint32_t index;
	uint32_t scalar;
	uint32_t match;

	/* A pipeline must supply a complete, bounded varying key before either vertex variant is lowered. */
	binary = compiler->binary;
	if (compiler->key->varying_count > BCM2711_SHADER_INTERFACE_WORDS)
		return E2BIG;
	if (compiler->ir->input_count > 16)
		return E2BIG;

	/* Refuses builtins and malformed declarations until their distinct native payload ABIs are implemented. */
	for (index = 0; index < compiler->ir->input_count; index++) {
		input = &compiler->ir->inputs[index];
		if (input->location >= 16 || input->components == 0 || input->components > 4 ||
		    input->flat > 1 || input->noperspective > 1)
			return ENOTSUP;
	}

	/* Traverses sparse locations, so matching FIFO order is independent of declaration order. */
	for (location = 0; location < 16; location++) {
		match = BCM2711_SHADER_ABSENT;
		for (index = 0; index < compiler->ir->input_count; index++) {
			/* Duplicate declarations cannot alias one hardware FIFO component. */
			if (compiler->ir->inputs[index].location == location) {
				if (match != BCM2711_SHADER_ABSENT)
					return EINVAL;
				match = index;
			}
		}

		/* Absent attribute locations consume no words. */
		if (match == BCM2711_SHADER_ABSENT)
			continue;

		/* Appends each scalar with its interpolation contract preserved. */
		input = &compiler->ir->inputs[match];
		for (scalar = 0; scalar < input->components; scalar++) {
			if (binary->input_count == BCM2711_SHADER_INTERFACE_WORDS)
				return E2BIG;
			component = &binary->inputs[binary->input_count];
			component->location = location;
			component->component = scalar;
			component->flat = input->flat;
			component->noperspective = input->noperspective;
			binary->input_count++;
		}
	}

	/* The shared varying key uses the same canonical scalar identities on both native stages. */
	for (index = 0; index < compiler->key->varying_count; index++) {
		component = &binary->varyings[index];
		*component = compiler->key->varyings[index];
		if (component->location >= 16 || component->component >= 4 ||
		    component->flat > 1 || component->noperspective > 1)
			return EINVAL;
		if (index != 0) {
			/* Strict ordering rejects duplicate or reversed FIFO identities. */
			if (component->location < binary->varyings[index - 1].location)
				return EINVAL;
			if (component->location == binary->varyings[index - 1].location &&
			    component->component <= binary->varyings[index - 1].component)
				return EINVAL;
		}
	}

	/* Fragment input preload consumes exactly the declared key, including interpolation qualifiers. */
	binary->varying_count = compiler->key->varying_count;
	if (binary->stage == BCM2711_SHADER_FRAGMENT) {
		if (binary->input_count != binary->varying_count)
			return EINVAL;
		for (index = 0; index < binary->input_count; index++) {
			component = &binary->inputs[index];
			if (component->location != binary->varyings[index].location ||
			    component->component != binary->varyings[index].component ||
			    component->flat != binary->varyings[index].flat ||
			    component->noperspective != binary->varyings[index].noperspective)
				return EINVAL;
		}
	}

	/* The caller can now use these scalar identities for attribute and varying setup. */
	return 0;
}

/* Validates one operation and records only the source fields whose semantics actually read a value. */
static int
analyze_instruction(
	struct bcm2711_shader_compiler *compiler,
	const struct i915_shader_ir_inst *instruction,
	uint32_t *skip_depth)
{
	struct bcm2711_shader_value *scalar;
	const struct i915_shader_ir_uniform *uniform;
	uint32_t sources;
	uint32_t destinations;
	uint32_t index;
	uint32_t number;
	uint32_t output;
	int error;

	/* Unsupported control flow, storage effects and texture forms fail instead of silently disappearing. */
	error = instruction_shape(instruction->op, &sources, &destinations);
	if (error != 0) {
		error = bcm2711_shader_fail(compiler, error, "operation has no native graphics lowering");
		return error;
	}

	/* Every meaningful source must have been defined by an earlier scalar instruction. */
	for (index = 0; index < sources; index++) {
		error = note_read(compiler, instruction->src[index]);
		if (error != 0)
			return error;
	}

	/* The frontend texture guard names an earlier Boolean even when unconditional sampling is safe. */
	if (instruction->guard != 0) {
		if (instruction->op != I915_IR_SAMPLE)
			return EINVAL;
		error = note_read(compiler, instruction->guard - 1);
		if (error != 0)
			return error;
	}

	/* This backend accepts SSA only; mutable loop-carried destinations require a separate control-flow ABI. */
	if (destinations != 0) {
		if (instruction->dst >= compiler->ir->value_count ||
		    destinations > compiler->ir->value_count - instruction->dst)
			return EINVAL;
		for (index = 0; index < destinations; index++) {
			scalar = &compiler->values[instruction->dst + index];
			if (scalar->definition != BCM2711_SHADER_ABSENT)
				return ENOTSUP;
			scalar->definition = compiler->instruction;
			scalar->last_read = compiler->instruction;
		}
	}

	/* Stage-visible output stores are pinned only after all later overwrites have been inspected. */
	if (instruction->op == I915_IR_STORE_OUTPUT) {
		if (instruction->component >= 4)
			return EINVAL;
		if (instruction->location == I915_IR_LOCATION_POSITION) {
			if (compiler->ir->stage != I915_STAGE_VERTEX)
				return ENOTSUP;
			output = BCM2711_SHADER_POSITION + instruction->component;
		} else {
			if (instruction->location >= 16)
				return ENOTSUP;
			if (compiler->ir->stage == I915_STAGE_FRAGMENT && instruction->location != 0)
				return ENOTSUP;
			output = instruction->location * 4 + instruction->component;
		}

		/* Only the most recent source store supplies this stage-visible output scalar. */
		compiler->outputs[output] = instruction->src[0];
	}

	/* Push loads consume exactly one aligned, declared word. */
	if (instruction->op == I915_IR_LOAD_PUSH) {
		if ((instruction->immediate & 3) != 0 || compiler->ir->push_bytes < 4 ||
		    instruction->immediate > compiler->ir->push_bytes - 4)
			return EINVAL;
	}

	/* Uniform block metadata remains a checked descriptor identity, never a userspace native address. */
	if (instruction->op == I915_IR_LOAD_UBO) {
		if (instruction->location >= compiler->ir->uniform_count || (instruction->immediate & 3) != 0)
			return EINVAL;
		uniform = &compiler->ir->uniforms[instruction->location];
		if (uniform->kind != I915_IR_UNIFORM_BLOCK || uniform->size < 4 ||
		    instruction->immediate < uniform->offset ||
		    instruction->immediate - uniform->offset > uniform->size - 4)
			return EINVAL;
	}

	/* Only normalized two-dimensional, zero-offset sampling has a complete native transaction here. */
	if (instruction->op == I915_IR_SAMPLE) {
		if (compiler->ir->stage != I915_STAGE_FRAGMENT || instruction->component != 0)
			return ENOTSUP;
		number = 0;
		for (index = 0; index < compiler->ir->uniform_count; index++) {
			uniform = &compiler->ir->uniforms[index];
			if (uniform->kind == I915_IR_UNIFORM_SAMPLED_IMAGE &&
			    uniform->set == instruction->location && uniform->binding == instruction->immediate)
				number++;
		}

		/* Exactly one descriptor declaration must own the requested combined image sampler. */
		if (number != 1)
			return EINVAL;
		compiler->samples++;
	}

	/* Skip markers omit a proven optimization only; matching pure operations still execute exactly. */
	if (instruction->op == I915_IR_SKIP_BEGIN) {
		(*skip_depth)++;
	} else if (instruction->op == I915_IR_SKIP_END) {
		if (*skip_depth == 0)
			return EINVAL;
		(*skip_depth)--;
	}

	/* Every operation's consumed and defined scalars are now ordered and bounded. */
	return 0;
}

/* Classifies only the pure scalar operations for which the native emitter supplies complete semantics. */
static int
instruction_shape(
	enum i915_shader_ir_op operation,
	uint32_t *sources,
	uint32_t *destinations)
{
	/* Most scalar operations define one value; explicit non-defining markers replace that default. */
	*sources = 0;
	*destinations = 1;

	/* A shape is an interface contract, rather than an inference from unused zero-filled fields. */
	switch (operation) {
	case I915_IR_NOP:
	case I915_IR_SKIP_END:
		*destinations = 0;
		break;
	case I915_IR_STORE_OUTPUT:
	case I915_IR_SKIP_BEGIN:
		*sources = 1;
		*destinations = 0;
		break;
	case I915_IR_CONST:
	case I915_IR_BOOL:
	case I915_IR_ICONST:
	case I915_IR_LOAD_INPUT:
	case I915_IR_LOAD_PUSH:
	case I915_IR_LOAD_UBO:
		break;
	case I915_IR_SAMPLE:
		*sources = 2;
		*destinations = 4;
		break;
	case I915_IR_SELECT:
		*sources = 3;
		break;
	case I915_IR_FADD:
	case I915_IR_FSUB:
	case I915_IR_FMUL:
	case I915_IR_FMIN:
	case I915_IR_FMAX:
	case I915_IR_FLT:
	case I915_IR_FGE:
	case I915_IR_FEQ:
	case I915_IR_FNEU:
	case I915_IR_AND:
	case I915_IR_OR:
	case I915_IR_IADD:
	case I915_IR_ISUB:
	case I915_IR_IAND:
	case I915_IR_IOR:
	case I915_IR_IXOR:
	case I915_IR_SHL:
	case I915_IR_SHR:
	case I915_IR_ASR:
		*sources = 2;
		break;
	case I915_IR_FNEG:
	case I915_IR_RSQ:
	case I915_IR_RCP:
	case I915_IR_SQRT:
	case I915_IR_EXP2:
	case I915_IR_LOG2:
	case I915_IR_FABS:
	case I915_IR_FLOOR:
	case I915_IR_FRACT:
	case I915_IR_NOT:
	case I915_IR_FTRUNC:
	case I915_IR_INEG:
	case I915_IR_INOT:
	case I915_IR_I2F:
	case I915_IR_U2F:
	case I915_IR_F2I:
	case I915_IR_F2U:
	case I915_IR_MOVE:
	case I915_IR_FROUND_EVEN:
		*sources = 1;
		break;
	default:
		return ENOTSUP;
	}

	/* The emitter may use only this operation's explicitly classified scalar fields. */
	return 0;
}

/* Records a source read after proving a strictly earlier definition exists. */
static int
note_read(
	struct bcm2711_shader_compiler *compiler,
	uint32_t number)
{
	struct bcm2711_shader_value *scalar;

	/* Invalid value IDs cannot address compiler storage. */
	if (number >= compiler->ir->value_count)
		return EINVAL;

	/* An absent or same-instruction destination cannot be its own SSA source. */
	scalar = &compiler->values[number];
	if (scalar->definition == BCM2711_SHADER_ABSENT || scalar->definition >= compiler->instruction)
		return EINVAL;

	/* The final use determines when a physical expression register may be recycled. */
	scalar->last_read = compiler->instruction;

	/* This read cannot observe an uninitialized or already replaced scalar. */
	return 0;
}

/* Verifies the native output ABI and keeps every actually consumed output live through its epilogue. */
static int
output_interface(
	struct bcm2711_shader_compiler *compiler)
{
	const struct bcm2711_shader_component *component;
	uint32_t index;
	uint32_t output;
	uint32_t number;

	/* Four native position words or four fragment color words form the mandatory stage output. */
	output = 0;
	if (compiler->ir->stage == I915_STAGE_VERTEX)
		output = BCM2711_SHADER_POSITION;

	/* Holds each complete position or RGBA value until all source instructions have executed. */
	for (index = 0; index < 4; index++) {
		number = compiler->outputs[output + index];
		if (number == BCM2711_SHADER_ABSENT)
			return EINVAL;
		compiler->values[number].last_read = compiler->ir->instruction_count;
	}

	/* A coordinate shader exports clip/viewport position only; fragment exports no VPM varyings. */
	if (compiler->binary->stage != BCM2711_SHADER_VERTEX)
		return 0;

	/* Every canonical fragment input must have a matching final vertex output scalar. */
	for (index = 0; index < compiler->binary->varying_count; index++) {
		component = &compiler->binary->varyings[index];
		output = component->location * 4 + component->component;
		number = compiler->outputs[output];
		if (number == BCM2711_SHADER_ABSENT)
			return EINVAL;
		compiler->values[number].last_read = compiler->ir->instruction_count;
	}

	/* The render vertex epilogue can consume all varyings without reading a recycled expression register. */
	return 0;
}
