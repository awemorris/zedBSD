/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Independent validation and liveness turn the shared scalar frontend into a bounded native program. */
#include <stdbool.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/videocore/compiler/shader-private.h"
#include "drivers/gpu/compiler/spirv.h"

/* Bounded source programs cannot force unbounded kernel compiler storage or traversal. */
#define SHADER_MAX_INSTRUCTIONS 8192U
#define SHADER_MAX_VALUES 16384U

static int input_interface(struct bcm2711_shader_compiler *compiler);
static uint32_t read_components(const struct drv_gpu_shader_ir *ir, uint32_t location, uint32_t declared);
static int analyze_instruction(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction, uint32_t *skip_depth);
static int instruction_shape(enum drv_gpu_shader_ir_op operation, uint32_t *sources, uint32_t *destinations);
static int compute_shape(const struct drv_gpu_shader_ir_inst *instruction, uint32_t *sources, uint32_t *destinations);
static int storage_check(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction);
static int image_check(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction);
static int image_reads(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction);
static bool size_of_image(const struct drv_gpu_shader_ir *ir, const struct drv_gpu_shader_ir_inst *instruction);
static int graphics_shape(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction);
static int loop_liveness(struct bcm2711_shader_compiler *compiler);
static int note_read(struct bcm2711_shader_compiler *compiler, uint32_t number);
static int output_interface(struct bcm2711_shader_compiler *compiler);

/*
 * Validates scalar source order and retains every final stage output through native program termination.
 */
int
bcm2711_shader_analyze(
	struct bcm2711_shader_compiler *compiler)
{
	const struct drv_gpu_shader_ir *ir;
	uint32_t index;
	uint32_t skip_depth;
	int error;

	/*
	 * The two graphics stages and compute have a native ABI in this
	 * compiler.  A program may be empty: a fragment program that writes
	 * nothing, for a pass without colour attachments, has no instruction
	 * and no value, and compiles to its epilogue alone.
	 */
	ir = compiler->ir;
	if (ir->instruction_count > SHADER_MAX_INSTRUCTIONS)
		return bcm2711_shader_fail(compiler, E2BIG, "program has more instructions than the compiler takes");
	if (ir->value_count > SHADER_MAX_VALUES)
		return bcm2711_shader_fail(compiler, E2BIG, "program has more values than the compiler takes");
	if (ir->stage != DRV_GPU_STAGE_VERTEX &&
	    ir->stage != DRV_GPU_STAGE_FRAGMENT &&
	    ir->stage != DRV_GPU_STAGE_COMPUTE)
		return bcm2711_shader_fail(compiler, ENOTSUP, "program stage has no native ABI");

	/* A compute program needs the compute binary, and the compute binary a compute program. */
	if ((ir->stage == DRV_GPU_STAGE_COMPUTE) != (compiler->binary->stage == BCM2711_SHADER_COMPUTE))
		return EINVAL;

	/* A compute program's workgroup fits one CSD workgroup, and its shared memory is not lowered yet. */
	if (ir->stage == DRV_GPU_STAGE_COMPUTE) {
		if (ir->local_size[0] == 0 || ir->local_size[1] == 0 || ir->local_size[2] == 0)
			return EINVAL;
		if (ir->local_size[0] > 256 || ir->local_size[1] > 256 || ir->local_size[2] > 64)
			return ENOTSUP;
		if (ir->local_size[0] * ir->local_size[1] * ir->local_size[2] > 256)
			return ENOTSUP;
		if (ir->shared_bytes > DRV_GPU_IR_MAX_SHARED_BYTES || (ir->shared_bytes & 3U) != 0)
			return ENOTSUP;
		compiler->binary->shared_bytes = ir->shared_bytes;
		compiler->binary->uses_barrier = ir->uses_barrier;
		compiler->binary->local_size[0] = ir->local_size[0];
		compiler->binary->local_size[1] = ir->local_size[1];
		compiler->binary->local_size[2] = ir->local_size[2];
	}

	/* Every value starts absent; value zero remains an ordinary valid scalar.  An empty program still owns one entry. */
	compiler->values = kern_calloc(ir->value_count + 1U, sizeof(*compiler->values));
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

	/* Values a loop reads or redefines but did not define live across its back edge. */
	error = loop_liveness(compiler);
	if (error != 0)
		return error;

	/* Holds only the final values of declared outputs until the native epilogue consumes them. */
	error = output_interface(compiler);
	if (error != 0)
		return error;

	/* Source and uniform capacities include conservative scheduling and stage epilogues. */
	compiler->code_capacity = ir->instruction_count * 64U + 2048U;
	compiler->uniform_capacity = ir->instruction_count * 8U + 256U;
	compiler->binary->push_bytes = ir->push_bytes;
	compiler->binary->threads = compiler->threads;

	/* The source interface and all scalar lifetimes are now safe for native lowering. */
	return 0;
}

/*
 * Counts the leading components of an input location a program reads: one
 * past the highest component any of its input loads names, at most the
 * declared count, and zero when none reads the location.
 */
static uint32_t
read_components(
	const struct drv_gpu_shader_ir *ir,
	uint32_t location,
	uint32_t declared)
{
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t index;
	uint32_t count;

	/* The highest component a load of the location names. */
	count = 0;
	for (index = 0; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		if (instruction->op != DRV_GPU_IR_LOAD_INPUT || instruction->location != location)
			continue;
		if (instruction->component >= count)
			count = instruction->component + 1U;
	}

	/* Succeeded: never more than the declaration has. */
	if (count > declared)
		count = declared;
	return count;
}

/* Establishes once-only scalar input FIFO order without relying on frontend declaration order. */
static int
input_interface(
	struct bcm2711_shader_compiler *compiler)
{
	struct bcm2711_shader_binary *binary;
	const struct drv_gpu_shader_ir_io *input;
	struct bcm2711_shader_component *component;
	uint32_t location;
	uint32_t index;
	uint32_t scalar;
	uint32_t match;
	uint32_t components;
	uint32_t first;

	/* A pipeline must supply a complete, bounded varying key before either vertex variant is lowered. */
	binary = compiler->binary;
	if (compiler->key->varying_count > BCM2711_SHADER_INTERFACE_WORDS)
		return bcm2711_shader_fail(compiler, E2BIG, "pipeline has more varying words than the hardware passes");
	if (compiler->ir->input_count > 16)
		return bcm2711_shader_fail(compiler, E2BIG, "program declares more than 16 input locations");

	/*
	 * A fragment program's gl_FragCoord and gl_FrontFacing come from the
	 * payload and the facing flag, gl_PointCoord from the implicit point
	 * varyings of a points pipeline (zero otherwise), not the varying FIFO;
	 * a vertex program's gl_InstanceIndex and gl_VertexIndex from the
	 * hardware's ids ahead of the attributes in the VPM; other built-ins
	 * and malformed declarations are refused.
	 */
	for (index = 0; index < compiler->ir->input_count; index++) {
		input = &compiler->ir->inputs[index];
		if (binary->stage != BCM2711_SHADER_FRAGMENT &&
		    (input->location == DRV_GPU_SHADER_LOCATION_INSTANCE_INDEX ||
		     input->location == DRV_GPU_SHADER_LOCATION_VERTEX_INDEX)) {
			if (input->components != 1)
				return ENOTSUP;
			if (read_components(compiler->ir, input->location, 1) == 0)
				continue;
			if (input->location == DRV_GPU_SHADER_LOCATION_INSTANCE_INDEX)
				binary->instance_index = 1;
			else
				binary->vertex_index = 1;
			continue;
		}
		if (binary->stage == BCM2711_SHADER_FRAGMENT &&
		    (input->location == DRV_GPU_SHADER_LOCATION_FRAG_COORD ||
		     input->location == DRV_GPU_SHADER_LOCATION_FRONT_FACING))
			continue;
		if (binary->stage == BCM2711_SHADER_FRAGMENT && input->location == DRV_GPU_SHADER_LOCATION_POINT_COORD) {
			if (input->components > 2)
				return ENOTSUP;
			if (compiler->key->points != 0)
				binary->point_coord = 1;
			continue;
		}
		if (input->location >= 16 ||
		    input->components == 0 ||
		    input->components > 4 ||
		    input->flat > 1 ||
		    input->noperspective > 1)
			return ENOTSUP;
	}

	/* Traverses sparse locations, so matching FIFO order is independent of declaration order. */
	first = 1;
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

		/*
		 * Appends each scalar with its interpolation contract preserved; a
		 * coordinate program, which passes on its position alone, takes
		 * only the leading components it reads, so the vertex fetch reads
		 * nothing more for the bin.  One that reads the hardware's ids reads
		 * at least one component of the first attribute, which the
		 * hardware needs then (GFXH-1602).
		 *
		 * Procedure referenced: Mesa 26.2.6 src/broadcom/vulkan/
		 * v3dvx_cmd_buffer.c (GFXH-1602: the first attribute's coordinate
		 * shader values) and src/broadcom/compiler/nir_to_vir.c (the
		 * coordinate shader's VPM offsets past that value).
		 */
		input = &compiler->ir->inputs[match];
		components = input->components;
		if (binary->stage == BCM2711_SHADER_COORDINATE) {
			components = read_components(compiler->ir, location, components);
			if (components == 0 && first != 0 && (binary->instance_index != 0 || binary->vertex_index != 0))
				components = 1;
		}
		first = 0;
		for (scalar = 0; scalar < components; scalar++) {
			if (binary->input_count == BCM2711_SHADER_INTERFACE_WORDS)
				return bcm2711_shader_fail(compiler, E2BIG, "program reads more input words than the hardware passes");
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
		if (component->location >= 16 ||
		    component->component >= 4 ||
		    component->flat > 1 ||
		    component->noperspective > 1)
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
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t *skip_depth)
{
	struct bcm2711_shader_value *scalar;
	const struct drv_gpu_shader_ir_uniform *uniform;
	uint32_t sources;
	uint32_t destinations;
	uint32_t index;
	uint32_t number;
	uint32_t output;
	int error;

	/*
	 * Unsupported control flow, storage effects and texture forms fail
	 * instead of silently disappearing.  The operations of the compute
	 * lowering a graphics program may also have are checked for its stage.
	 */
	error = instruction_shape(instruction->op, &sources, &destinations);
	if (error == ENOTSUP) {
		error = compute_shape(instruction, &sources, &destinations);
		if (error == 0 && compiler->ir->stage != DRV_GPU_STAGE_COMPUTE) {
			error = graphics_shape(compiler, instruction);
			if (error != 0)
				return error;
		}
	}
	if (error != 0) {
		error = bcm2711_shader_fail(compiler, error, "operation has no native graphics lowering");
		return error;
	}

	/* A derivative compares the pixels of a quad, which only a fragment program has. */
	if ((instruction->op == DRV_GPU_IR_DDX ||
	     instruction->op == DRV_GPU_IR_DDX_FINE ||
	     instruction->op == DRV_GPU_IR_DDY ||
	     instruction->op == DRV_GPU_IR_DDY_FINE) &&
	    compiler->ir->stage != DRV_GPU_STAGE_FRAGMENT)
		return bcm2711_shader_fail(compiler, ENOTSUP, "derivative outside a fragment program");

	/* A half of a packed pair is the low or the high one. */
	if (instruction->op == DRV_GPU_IR_UNPACK_HALF && instruction->component > 1)
		return EINVAL;

	/* Every meaningful source must have been defined by an earlier scalar instruction. */
	for (index = 0; index < sources; index++) {
		error = note_read(compiler, instruction->src[index]);
		if (error != 0)
			return error;
	}

	/* A storage image access reads runs of consecutive values: the texel's coordinates, a write's texel. */
	error = image_reads(compiler, instruction);
	if (error != 0)
		return error;

	/* A texture message's parameters are src[1] consecutive values from src[0]. */
	if (instruction->op == DRV_GPU_IR_TEXTURE) {
		if (instruction->src[1] == 0 || instruction->src[1] > DRV_GPU_IR_TEXTURE_MAX_PARAMS)
			return EINVAL;
		for (index = 0; index < instruction->src[1]; index++) {
			error = note_read(compiler, instruction->src[0] + index);
			if (error != 0)
				return error;
		}
	}

	/* The frontend texture guard names an earlier Boolean even when unconditional sampling is safe. */
	if (instruction->guard != 0) {
		if (instruction->op != DRV_GPU_IR_SAMPLE &&
		    instruction->op != DRV_GPU_IR_SAMPLE_BIAS &&
		    instruction->op != DRV_GPU_IR_SAMPLE_LOD &&
		    instruction->op != DRV_GPU_IR_TEXTURE)
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

			/* A loop variable is moved into before its loop and again at its end; it keeps its first definition. */
			if (scalar->definition != BCM2711_SHADER_ABSENT &&
			    instruction->op == DRV_GPU_IR_MOVE &&
			    compiler->ir->stage != DRV_GPU_STAGE_FRAGMENT) {
				scalar->last_read = compiler->instruction;
				continue;
			}
			if (scalar->definition != BCM2711_SHADER_ABSENT)
				return ENOTSUP;
			scalar->definition = compiler->instruction;
			scalar->last_read = compiler->instruction;
		}
	}

	/* Stage-visible output stores are pinned only after all later overwrites have been inspected. */
	if (instruction->op == DRV_GPU_IR_STORE_OUTPUT) {
		if (instruction->component >= 4)
			return EINVAL;
		if (instruction->location == DRV_GPU_IR_LOCATION_POSITION) {
			if (compiler->ir->stage != DRV_GPU_STAGE_VERTEX)
				return ENOTSUP;
			output = BCM2711_SHADER_POSITION + instruction->component;
		} else if (instruction->location == DRV_GPU_IR_LOCATION_POINT_SIZE) {
			/* The point size, which only a points pipeline's vertex programs pass on. */
			if (compiler->ir->stage != DRV_GPU_STAGE_VERTEX || instruction->component != 0)
				return ENOTSUP;
			output = BCM2711_SHADER_POINT_SIZE;
		} else if (instruction->location == DRV_GPU_IR_LOCATION_FRAG_DEPTH) {
			/* A fragment program's depth, written to the tile buffer ahead of its colour. */
			if (compiler->ir->stage != DRV_GPU_STAGE_FRAGMENT || instruction->component != 0)
				return bcm2711_shader_fail(compiler, ENOTSUP, "depth output outside a fragment program");
			output = BCM2711_SHADER_FRAG_DEPTH;
		} else {
			if (instruction->location >= 16)
				return ENOTSUP;
			if (compiler->ir->stage == DRV_GPU_STAGE_FRAGMENT && instruction->location >= BCM2711_SHADER_TARGETS)
				return ENOTSUP;
			output = instruction->location * 4 + instruction->component;
		}

		/* Only the most recent source store supplies this stage-visible output scalar. */
		compiler->outputs[output] = instruction->src[0];
	}

	/* A discard clears the coverage of a fragment's tile writes, which only a fragment program has. */
	if (instruction->op == DRV_GPU_IR_KILL && compiler->ir->stage != DRV_GPU_STAGE_FRAGMENT)
		return ENOTSUP;
	if (instruction->op == DRV_GPU_IR_KILL)
		compiler->discards = 1;

	/* Push loads consume exactly one aligned, declared word. */
	if (instruction->op == DRV_GPU_IR_LOAD_PUSH) {
		if ((instruction->immediate & 3) != 0 ||
		    compiler->ir->push_bytes < 4 ||
		    instruction->immediate > compiler->ir->push_bytes - 4)
			return EINVAL;
	}

	/* Uniform block metadata remains a checked descriptor identity, never a userspace native address. */
	if (instruction->op == DRV_GPU_IR_LOAD_UBO) {
		if (instruction->location >= compiler->ir->uniform_count || (instruction->immediate & 3) != 0)
			return EINVAL;
		uniform = &compiler->ir->uniforms[instruction->location];
		if (uniform->kind != DRV_GPU_IR_UNIFORM_BLOCK ||
		    uniform->size < 4 ||
		    instruction->immediate < uniform->offset ||
		    instruction->immediate - uniform->offset > uniform->size - 4)
			return EINVAL;
	}

	/* A compute program's size query of a storage image reads its view's size alone. */
	if (size_of_image(compiler->ir, instruction)) {
		if (compiler->ir->stage != DRV_GPU_STAGE_COMPUTE)
			return bcm2711_shader_fail(compiler, ENOTSUP, "storage image access outside a compute program");
		return 0;
	}

	/* Sampling reads one declared combined image sampler, in a vertex or a fragment program. */
	if (instruction->op == DRV_GPU_IR_SAMPLE ||
	    instruction->op == DRV_GPU_IR_SAMPLE_BIAS ||
	    instruction->op == DRV_GPU_IR_SAMPLE_LOD ||
	    instruction->op == DRV_GPU_IR_TEXTURE) {
		if (compiler->ir->stage != DRV_GPU_STAGE_FRAGMENT && compiler->ir->stage != DRV_GPU_STAGE_VERTEX)
			return ENOTSUP;
		number = 0;
		for (index = 0; index < compiler->ir->uniform_count; index++) {
			uniform = &compiler->ir->uniforms[index];
			if (uniform->kind == DRV_GPU_IR_UNIFORM_SAMPLED_IMAGE &&
			    uniform->set == instruction->location && uniform->binding == instruction->immediate &&
			    uniform->element == instruction->element)
				number++;
		}

		/* Exactly one descriptor declaration must own the requested combined image sampler. */
		if (number != 1)
			return EINVAL;
		compiler->samples++;
	}

	/* Storage accesses name a storage buffer or image of the uniform list; built-in values name a compute value. */
	error = storage_check(compiler, instruction);
	if (error != 0)
		return error;
	error = image_check(compiler, instruction);
	if (error != 0)
		return error;

	/* Skip markers omit a proven optimization only; matching pure operations still execute exactly. */
	if (instruction->op == DRV_GPU_IR_SKIP_BEGIN) {
		(*skip_depth)++;
	} else if (instruction->op == DRV_GPU_IR_SKIP_END) {
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
	enum drv_gpu_shader_ir_op operation,
	uint32_t *sources,
	uint32_t *destinations)
{
	/* Most scalar operations define one value; explicit non-defining markers replace that default. */
	*sources = 0;
	*destinations = 1;

	/* A shape is an interface contract, rather than an inference from unused zero-filled fields. */
	switch (operation) {
	case DRV_GPU_IR_NOP:
	case DRV_GPU_IR_SKIP_END:
		*destinations = 0;
		break;
	case DRV_GPU_IR_STORE_OUTPUT:
	case DRV_GPU_IR_SKIP_BEGIN:
	case DRV_GPU_IR_KILL:
		*sources = 1;
		*destinations = 0;
		break;
	case DRV_GPU_IR_CONST:
	case DRV_GPU_IR_BOOL:
	case DRV_GPU_IR_ICONST:
	case DRV_GPU_IR_LOAD_INPUT:
	case DRV_GPU_IR_LOAD_PUSH:
	case DRV_GPU_IR_LOAD_UBO:
		break;
	case DRV_GPU_IR_SAMPLE:
		*sources = 2;
		*destinations = 4;
		break;
	case DRV_GPU_IR_SAMPLE_BIAS:
	case DRV_GPU_IR_SAMPLE_LOD:
		*sources = 3;
		*destinations = 4;
		break;
	case DRV_GPU_IR_TEXTURE:
		/* The message's parameters are a run of values (texture_parameters). */
		*destinations = 4;
		break;
	case DRV_GPU_IR_SELECT:
		*sources = 3;
		break;
	case DRV_GPU_IR_FADD:
	case DRV_GPU_IR_FSUB:
	case DRV_GPU_IR_FMUL:
	case DRV_GPU_IR_FMIN:
	case DRV_GPU_IR_FMAX:
	case DRV_GPU_IR_FLT:
	case DRV_GPU_IR_FGE:
	case DRV_GPU_IR_FEQ:
	case DRV_GPU_IR_FNEU:
	case DRV_GPU_IR_AND:
	case DRV_GPU_IR_OR:
	case DRV_GPU_IR_IADD:
	case DRV_GPU_IR_ISUB:
	case DRV_GPU_IR_IAND:
	case DRV_GPU_IR_IOR:
	case DRV_GPU_IR_IXOR:
	case DRV_GPU_IR_SHL:
	case DRV_GPU_IR_SHR:
	case DRV_GPU_IR_ASR:
		*sources = 2;
		break;
	case DRV_GPU_IR_FNEG:
	case DRV_GPU_IR_RSQ:
	case DRV_GPU_IR_RCP:
	case DRV_GPU_IR_SQRT:
	case DRV_GPU_IR_EXP2:
	case DRV_GPU_IR_LOG2:
	case DRV_GPU_IR_FABS:
	case DRV_GPU_IR_FLOOR:
	case DRV_GPU_IR_FRACT:
	case DRV_GPU_IR_NOT:
	case DRV_GPU_IR_FTRUNC:
	case DRV_GPU_IR_INEG:
	case DRV_GPU_IR_INOT:
	case DRV_GPU_IR_I2F:
	case DRV_GPU_IR_U2F:
	case DRV_GPU_IR_F2I:
	case DRV_GPU_IR_F2U:
	case DRV_GPU_IR_MOVE:
	case DRV_GPU_IR_FROUND_EVEN:
	case DRV_GPU_IR_SIN:
	case DRV_GPU_IR_COS:
	case DRV_GPU_IR_DDX:
	case DRV_GPU_IR_DDX_FINE:
	case DRV_GPU_IR_DDY:
	case DRV_GPU_IR_DDY_FINE:
	case DRV_GPU_IR_UNPACK_HALF:
		*sources = 1;
		break;
	case DRV_GPU_IR_PACK_HALF:
		*sources = 2;
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

	/* A compute program has no stage output; its effects are its stores. */
	if (compiler->ir->stage == DRV_GPU_STAGE_COMPUTE)
		return 0;

	/*
	 * A fragment program's colour outputs, at locations 0 to 3, and its
	 * depth are held until the tile writes; a colour component it leaves
	 * out is written as zero.
	 */
	if (compiler->ir->stage == DRV_GPU_STAGE_FRAGMENT) {
		for (index = 0; index < BCM2711_SHADER_TARGETS * 4U; index++) {
			number = compiler->outputs[index];
			if (number != BCM2711_SHADER_ABSENT)
				compiler->values[number].last_read = compiler->ir->instruction_count;
		}
		number = compiler->outputs[BCM2711_SHADER_FRAG_DEPTH];
		if (number != BCM2711_SHADER_ABSENT) {
			compiler->binary->writes_depth = 1;
			compiler->values[number].last_read = compiler->ir->instruction_count;
		}
		return 0;
	}

	/* Four native position words form the mandatory vertex output, held until all source instructions have executed. */
	output = BCM2711_SHADER_POSITION;
	for (index = 0; index < 4; index++) {
		number = compiler->outputs[output + index];
		if (number == BCM2711_SHADER_ABSENT)
			return EINVAL;
		compiler->values[number].last_read = compiler->ir->instruction_count;
	}

	/* Both vertex programs of a points pipeline pass on the point size a shader writes. */
	number = compiler->outputs[BCM2711_SHADER_POINT_SIZE];
	if (compiler->ir->stage == DRV_GPU_STAGE_VERTEX && compiler->key->points != 0 && number != BCM2711_SHADER_ABSENT) {
		compiler->binary->point_size = 1;
		compiler->values[number].last_read = compiler->ir->instruction_count;
	}

	/* A coordinate shader exports clip/viewport position and the point size only; fragment exports no VPM varyings. */
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

/*
 * Classifies the operations only a compute program has: built-in values,
 * storage buffer accesses and the integer operations its addressing needs.
 */
static int
compute_shape(
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t *sources,
	uint32_t *destinations)
{
	/* A predicated access reads its Boolean predicate after its other sources. */
	*sources = 0;
	*destinations = 1;
	switch (instruction->op) {
	case DRV_GPU_IR_LOAD_SYSTEM:
	case DRV_GPU_IR_STORAGE_SIZE:
		break;
	case DRV_GPU_IR_LOOP_BEGIN:
	case DRV_GPU_IR_BARRIER:
	case DRV_GPU_IR_FENCE:
		*destinations = 0;
		break;
	case DRV_GPU_IR_LOAD_SHARED:
		*sources = 1;
		if (instruction->component == 1)
			*sources = 2;
		break;
	case DRV_GPU_IR_STORE_SHARED:
		*sources = 2;
		if (instruction->component == 1)
			*sources = 3;
		*destinations = 0;
		break;
	case DRV_GPU_IR_LOOP_END:
		*sources = 1;
		*destinations = 0;
		break;
	case DRV_GPU_IR_LOAD_STORAGE:
		*sources = 1;
		if (instruction->component == 1)
			*sources = 2;
		break;
	case DRV_GPU_IR_STORE_STORAGE:
		*sources = 2;
		if (instruction->component == 1)
			*sources = 3;
		*destinations = 0;
		break;
	case DRV_GPU_IR_ATOMIC:
		/*
		 * The offset (a storage image texel's first coordinate) and the
		 * value, a compare and exchange's comparator, then a predicate.
		 */
		if ((instruction->image_shape != 0 && (instruction->image_shape & DRV_GPU_IR_IMAGE_TEXEL) == 0) ||
		    instruction->component > DRV_GPU_IR_ATOMIC_PREDICATED)
			return ENOTSUP;
		*sources = 2;
		if (instruction->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG)
			*sources = 3;
		if (instruction->component == DRV_GPU_IR_ATOMIC_PREDICATED)
			*sources += 1;
		break;
	case DRV_GPU_IR_IMUL:
	case DRV_GPU_IR_UDIV:
	case DRV_GPU_IR_UMOD:
	case DRV_GPU_IR_IDIV:
	case DRV_GPU_IR_IREM:
	case DRV_GPU_IR_ILT:
	case DRV_GPU_IR_IGE:
	case DRV_GPU_IR_ULT:
	case DRV_GPU_IR_UGE:
	case DRV_GPU_IR_IEQ:
	case DRV_GPU_IR_INE:
		*sources = 2;
		break;
	case DRV_GPU_IR_IMAGE_READ:
		/* The texel's four components; its coordinates and predicate are read as runs (image_reads). */
		*destinations = 4;
		break;
	case DRV_GPU_IR_IMAGE_WRITE:
		/* Its coordinates, texel and predicate are read as runs (image_reads). */
		*destinations = 0;
		break;
	default:
		return ENOTSUP;
	}

	/* A predicate flag other than zero or one has no meaning. */
	if ((instruction->op == DRV_GPU_IR_LOAD_STORAGE ||
	     instruction->op == DRV_GPU_IR_STORE_STORAGE ||
	     instruction->op == DRV_GPU_IR_LOAD_SHARED ||
	     instruction->op == DRV_GPU_IR_STORE_SHARED ||
	     instruction->op == DRV_GPU_IR_IMAGE_READ ||
	     instruction->op == DRV_GPU_IR_IMAGE_WRITE) &&
	    instruction->component > 1)
		return EINVAL;

	/* Succeeded: the operation's sources and results are classified. */
	return 0;
}

/*
 * Checks an operation of the compute lowering in a graphics program: the
 * integer operations in every stage; loops and storage buffer accesses in
 * a vertex program only (a fragment program is scheduled, and the
 * scheduler places no branch; a fragment program's stores would need its
 * coverage); nothing of a workgroup (built-in ids, shared memory,
 * barriers).  Storage buffer accesses also run in the coordinate variant,
 * so every vertex is stored by the bin, and again by the render for the
 * vertices of drawn primitives, which Vulkan permits.
 */
static int
graphics_shape(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	/* Each kind of operation by the stage. */
	switch (instruction->op) {
	case DRV_GPU_IR_LOAD_SYSTEM:
		/* The view a multiview pass renders is a uniform of any graphics program. */
		if (instruction->component == DRV_GPU_IR_SYSTEM_VIEW_INDEX)
			break;
		return bcm2711_shader_fail(compiler, ENOTSUP, "workgroup operation in a graphics program");
	case DRV_GPU_IR_LOAD_SHARED:
	case DRV_GPU_IR_STORE_SHARED:
	case DRV_GPU_IR_BARRIER:
	case DRV_GPU_IR_FENCE:
		return bcm2711_shader_fail(compiler, ENOTSUP, "workgroup operation in a graphics program");
	case DRV_GPU_IR_IMAGE_READ:
	case DRV_GPU_IR_IMAGE_WRITE:
		return bcm2711_shader_fail(compiler, ENOTSUP, "storage image access outside a compute program");
	case DRV_GPU_IR_ATOMIC:
	case DRV_GPU_IR_LOAD_STORAGE:
	case DRV_GPU_IR_STORE_STORAGE:
	case DRV_GPU_IR_STORAGE_SIZE:
		/* A storage image's texel is a compute program's alone. */
		if (instruction->op == DRV_GPU_IR_ATOMIC && (instruction->image_shape & DRV_GPU_IR_IMAGE_TEXEL) != 0)
			return bcm2711_shader_fail(compiler, ENOTSUP, "storage image access outside a compute program");

		/* A buffer of the program's own set, not shared memory or the dispatch's group counts. */
		if (instruction->location == DRV_GPU_IR_LOCATION_SHARED)
			return bcm2711_shader_fail(compiler, ENOTSUP, "workgroup operation in a graphics program");
		if (instruction->location < compiler->ir->uniform_count &&
		    compiler->ir->uniforms[instruction->location].set == DRV_GPU_IR_SYSTEM_SET)
			return bcm2711_shader_fail(compiler, ENOTSUP, "workgroup operation in a graphics program");
		if (compiler->ir->stage != DRV_GPU_STAGE_VERTEX)
			return bcm2711_shader_fail(compiler, ENOTSUP, "storage buffer access outside a vertex program");
		break;
	case DRV_GPU_IR_LOOP_BEGIN:
	case DRV_GPU_IR_LOOP_END:
		if (compiler->ir->stage != DRV_GPU_STAGE_VERTEX)
			return bcm2711_shader_fail(compiler, ENOTSUP, "loop in a fragment program");
		break;
	default:
		break;
	}

	/* Succeeded: the stage has a lowering of the operation. */
	return 0;
}

/* Checks the uniform a storage access names and the built-in value a system load reads. */
static int
storage_check(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	const struct drv_gpu_shader_ir_uniform *uniform;

	/*
	 * A built-in value of a compute invocation, or the view of a graphics
	 * one; the primitive id belongs to geometry.
	 */
	if (instruction->op == DRV_GPU_IR_LOAD_SYSTEM) {
		if (instruction->component == DRV_GPU_IR_SYSTEM_VIEW_INDEX && compiler->ir->stage != DRV_GPU_STAGE_COMPUTE)
			return 0;
		if (instruction->component > DRV_GPU_IR_SYSTEM_GROUP_ID_Z)
			return ENOTSUP;
		return 0;
	}

	/* An atomic on shared memory names no buffer; one on a storage image's texel names the image (image_check). */
	if (instruction->op == DRV_GPU_IR_ATOMIC && instruction->location == DRV_GPU_IR_LOCATION_SHARED)
		return 0;
	if (instruction->op == DRV_GPU_IR_ATOMIC && (instruction->image_shape & DRV_GPU_IR_IMAGE_TEXEL) != 0)
		return 0;

	/* Only the storage operations name a storage buffer. */
	if (instruction->op != DRV_GPU_IR_LOAD_STORAGE &&
	    instruction->op != DRV_GPU_IR_STORE_STORAGE &&
	    instruction->op != DRV_GPU_IR_STORAGE_SIZE &&
	    instruction->op != DRV_GPU_IR_ATOMIC)
		return 0;
	if (instruction->location >= compiler->ir->uniform_count)
		return EINVAL;
	uniform = &compiler->ir->uniforms[instruction->location];
	if (uniform->kind != DRV_GPU_IR_UNIFORM_STORAGE)
		return EINVAL;

	/* The dispatch's group counts are read, never written or sized. */
	if (uniform->set == DRV_GPU_IR_SYSTEM_SET && instruction->op != DRV_GPU_IR_LOAD_STORAGE)
		return ENOTSUP;

	/* Succeeded: the access names a storage buffer of the program. */
	return 0;
}

/*
 * Checks the storage image an image access names: a storage image of the
 * uniform list, of a format the lowering converts; an atomic's of a 32-bit
 * integer format.  A multisampled image or one of the buffer kind has no
 * storage access.
 */
static int
image_check(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	const struct drv_gpu_shader_ir_uniform *uniform;
	uint32_t kind;

	/* Only the image accesses name a storage image. */
	if (instruction->op != DRV_GPU_IR_IMAGE_READ &&
	    instruction->op != DRV_GPU_IR_IMAGE_WRITE &&
	    (instruction->op != DRV_GPU_IR_ATOMIC || (instruction->image_shape & DRV_GPU_IR_IMAGE_TEXEL) == 0))
		return 0;
	if (instruction->location >= compiler->ir->uniform_count)
		return EINVAL;
	uniform = &compiler->ir->uniforms[instruction->location];
	if (uniform->kind != DRV_GPU_IR_UNIFORM_STORAGE_IMAGE)
		return bcm2711_shader_fail(compiler, ENOTSUP, "image access of something that is not a storage image");

	/* A 2D, 3D or cube image, or an array of 2D images, of one sample. */
	kind = instruction->image_shape & DRV_GPU_IR_IMAGE_KIND_MASK;
	if (kind == DRV_GPU_IR_IMAGE_BUFFER || (instruction->image_shape & DRV_GPU_IR_IMAGE_MULTISAMPLED) != 0)
		return bcm2711_shader_fail(compiler, ENOTSUP, "storage image of a shape the lowering does not address");

	/* A format the lowering converts; an atomic's a 32-bit integer one (VK_FORMAT_R32_UINT or R32_SINT). */
	if (bcm2711_shader_storage_texel(uniform->format) == 0)
		return bcm2711_shader_fail(compiler, ENOTSUP, "storage image of a format the lowering does not convert");
	if (instruction->op == DRV_GPU_IR_ATOMIC && uniform->format != 98U && uniform->format != 99U)
		return bcm2711_shader_fail(compiler, ENOTSUP, "image atomic on an image that is not R32_UINT or R32_SINT");

	/* Succeeded: the access names a storage image the lowering serves. */
	return 0;
}

/*
 * Notes the runs of consecutive values a storage image access reads: the
 * texel's three coordinates from src[0]; a write's four components from
 * src[2] and its predicate src[3]; a read's predicate src[2].  An
 * atomic's first coordinate is src[0] itself (compute_shape).
 */
static int
image_reads(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	uint32_t index;
	uint32_t first;
	int error;

	/* The coordinates: all three of a read and a write, the two after the first of an atomic. */
	first = 0;
	if (instruction->op == DRV_GPU_IR_ATOMIC && (instruction->image_shape & DRV_GPU_IR_IMAGE_TEXEL) != 0)
		first = 1;
	else if (instruction->op != DRV_GPU_IR_IMAGE_READ && instruction->op != DRV_GPU_IR_IMAGE_WRITE)
		return 0;
	if ((instruction->op == DRV_GPU_IR_IMAGE_READ || instruction->op == DRV_GPU_IR_IMAGE_WRITE) && instruction->src[1] != 3U)
		return EINVAL;
	for (index = first; index < 3U; index++) {
		error = note_read(compiler, instruction->src[0] + index);
		if (error != 0)
			return error;
	}

	/* A write's texel, then either access's predicate. */
	if (instruction->op == DRV_GPU_IR_IMAGE_WRITE) {
		for (index = 0; index < 4U; index++) {
			error = note_read(compiler, instruction->src[2] + index);
			if (error != 0)
				return error;
		}
		if (instruction->component == 1) {
			error = note_read(compiler, instruction->src[3]);
			if (error != 0)
				return error;
		}
	} else if (instruction->op == DRV_GPU_IR_IMAGE_READ && instruction->component == 1) {
		error = note_read(compiler, instruction->src[2]);
		if (error != 0)
			return error;
	}

	/* Succeeded: every value the access reads is defined before it. */
	return 0;
}

/* Tells whether an instruction is a size query of a storage image (imageSize). */
static bool
size_of_image(
	const struct drv_gpu_shader_ir *ir,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	const struct drv_gpu_shader_ir_uniform *uniform;
	uint32_t index;

	/* The resinfo message of a TEXTURE instruction. */
	if (instruction->op != DRV_GPU_IR_TEXTURE ||
	    (instruction->component & DRV_GPU_IR_TEXTURE_MESSAGE_MASK) != DRV_GPU_IR_TEXTURE_RESINFO)
		return false;

	/* A storage image of the set and binding it names. */
	for (index = 0; index < ir->uniform_count; index++) {
		uniform = &ir->uniforms[index];
		if (uniform->kind == DRV_GPU_IR_UNIFORM_STORAGE_IMAGE &&
		    uniform->set == instruction->location &&
		    uniform->binding == instruction->immediate)
			return true;
	}

	/* Succeeded: a sampled image's query, or another message. */
	return false;
}

/*
 * Extends the lives of the values a loop reads or redefines without
 * defining them first inside it to the loop's end, so their registers
 * survive the back edge, and checks that the loops nest.
 */
static int
loop_liveness(
	struct bcm2711_shader_compiler *compiler)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	struct bcm2711_shader_value *scalar;
	uint32_t starts[BCM2711_SHADER_LOOP_DEPTH];
	uint32_t depth;
	uint32_t index;
	uint32_t value;
	uint32_t begin;

	/* The loops in order of their ends: the innermost open loop ends first. */
	ir = compiler->ir;
	depth = 0;
	for (index = 0; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		if (instruction->op == DRV_GPU_IR_LOOP_BEGIN) {
			if (ir->stage == DRV_GPU_STAGE_FRAGMENT)
				return bcm2711_shader_fail(compiler, ENOTSUP, "loop in a fragment program");
			if (depth == BCM2711_SHADER_LOOP_DEPTH)
				return bcm2711_shader_fail(compiler, ENOTSUP, "loops nest too deeply");
			starts[depth] = index;
			depth++;
			continue;
		}
		/*
		 * A barrier is in control flow uniform across its workgroup (the
		 * SPIR-V rule for OpControlBarrier), so every batch of a workgroup
		 * runs a loop around it the same number of times.  Workgroups
		 * may not: a supergroup that packs several would deadlock, so the
		 * program is marked to run one workgroup per supergroup.
		 */
		if (instruction->op == DRV_GPU_IR_BARRIER && depth != 0)
			compiler->binary->barrier_in_loop = 1;
		if (instruction->op != DRV_GPU_IR_LOOP_END)
			continue;
		if (depth == 0)
			return EINVAL;
		depth--;
		begin = starts[depth];

		/* A value defined before the loop and still read or redefined inside it lives at least to the loop's end. */
		for (value = 0; value < ir->value_count; value++) {
			scalar = &compiler->values[value];
			if (scalar->definition == BCM2711_SHADER_ABSENT || scalar->definition >= begin)
				continue;
			if (scalar->last_read <= begin || scalar->last_read >= index)
				continue;
			scalar->last_read = index;
		}
	}

	/* Every loop ends. */
	if (depth != 0)
		return EINVAL;

	/* Succeeded: loop-carried values keep their registers through every iteration. */
	return 0;
}
