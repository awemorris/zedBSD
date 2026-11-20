/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Scalar lowering for V3D 7.1, the Raspberry Pi 5's BCM2712.
 *
 * This is the 7.1 copy of shader-lower.c; the board's source list selects
 * it in place of that file.  It differs where the 7.1 word differs: the
 * varying's constant coefficient arrives in rf0 instead of accumulator five,
 * the fragment payload's W is in rf3 instead of rf0, and a word reads at most
 * one small immediate, even when both inputs of an operation are the same
 * constant.
 */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <stdbool.h>

#include "drivers/gpu/videocore/compiler/shader-private.h"
#include "drivers/gpu/compiler/spirv.h"

/*
 * 1 / pi and one half as float bits, the scale and the quarter period of
 * the sine's range reduction.
 */
#define LOWER_INVERSE_PI 0x3ea2f983U
#define LOWER_HALF 0x3f000000U

static int preload_inputs(struct bcm2711_shader_compiler *compiler);
static int preload_ids(struct bcm2711_shader_compiler *compiler, uint32_t *ids);
static int lower_instruction(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction);
static int lower_arithmetic(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction, uint32_t destination);
static int lower_select(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction, uint32_t destination);
static int lower_kill(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction);
static int lower_builtin(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction, uint32_t destination);
static int lower_sine(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction, uint32_t destination);
static int lower_sample(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction);
static int lower_size(struct bcm2711_shader_compiler *compiler, const struct drv_gpu_shader_ir_inst *instruction, uint32_t kind, bool arrayed, uint32_t level);
static int multisample_texel(struct bcm2711_shader_compiler *compiler, const uint32_t *parameters, uint32_t *texel);
static int operands(struct bcm2711_shader_compiler *compiler, uint32_t left, uint32_t right, struct bcm2711_qpu_source *first, struct bcm2711_qpu_source *second);
static int force_register(struct bcm2711_shader_compiler *compiler, struct bcm2711_qpu_source *source, uint32_t scratch);
static int arithmetic_operation(enum drv_gpu_shader_ir_op operation, enum bcm2711_qpu_operation *native, uint32_t *sources);
static void retire_values(struct bcm2711_shader_compiler *compiler);

/*
 * Lowers checked scalar instructions into a conservatively scheduled native graphics program.
 */
int
bcm2711_shader_lower(
	struct bcm2711_shader_compiler *compiler)
{
	uint32_t index;
	int error;

	/* Input FIFOs are read exactly once; repeated LOAD_INPUT scalars alias immutable preloaded registers. */
	error = preload_inputs(compiler);
	if (error != 0)
		return error;

	/* Native arithmetic retains every source register until that source's final validated read. */
	for (index = 0; index < compiler->ir->instruction_count; index++) {
		compiler->instruction = index;
		error = lower_instruction(compiler, &compiler->ir->instructions[index]);
		if (error != 0) {
			if (compiler->diagnostic != NULL && compiler->diagnostic->reason == NULL)
				error = bcm2711_shader_fail(compiler, error, "native instruction or register allocation refused");
			if (error != 0)
				return error;

			/* This source operation has a complete native encoding. */
			return 0;
		}

		/* No expression register is recycled until the current instruction has consumed every source. */
		retire_values(compiler);
	}

	/* Native output and termination instructions remain inside the same code and uniform extent. */
	compiler->instruction = compiler->ir->instruction_count;
	error = bcm2711_shader_finish(compiler);
	if (error != 0)
		return error;

	/* Two-thread programs use the sixty-four physical registers and four-thread programs rf0 to rf31, scratch included. */
	compiler->binary->registers_used = 64;
	if (compiler->threads == 4)
		compiler->binary->registers_used = 32;
	compiler->binary->starts_final = 0;
	if (compiler->binary->stage != BCM2711_SHADER_FRAGMENT && compiler->samples == 0 && compiler->switches == 1)
		compiler->binary->starts_final = 1;

	/* A compute program is a single segment when its only switch is the thread end. */
	if (compiler->binary->stage == BCM2711_SHADER_COMPUTE) {
		compiler->binary->starts_final = 0;
		if (compiler->switches == 1)
			compiler->binary->starts_final = 1;
	}

	/* The completed native program supplies all inputs, outputs and its final switch delay slots. */
	return 0;
}

/* Reads the immutable vertex VPM or interpolated fragment FIFO interface before source arithmetic begins. */
static int
preload_inputs(
	struct bcm2711_shader_compiler *compiler)
{
	const struct bcm2711_shader_component *component;
	struct bcm2711_qpu_source left;
	struct bcm2711_qpu_source right;
	struct bcm2711_qpu_source coefficient;
	struct bcm2711_shader_uniform uniform;
	uint32_t index;
	uint32_t destination;
	uint32_t gaps;
	uint32_t varying;
	uint32_t work;
	uint32_t ids;
	int error;

	/*
	 * The varying's constant coefficient lands in rf0, which no expression
	 * is given; addressed uniform loads do not write it (Mesa 25.3.6
	 * src/broadcom/compiler/nir_to_vir.c lines 1046-1049).
	 */
	coefficient = bcm2711_shader_rf(0);

	/*
	 * A points pipeline's fragment program that reads gl_PointCoord has the
	 * implicit point varyings s and t first, smooth like any other.  The
	 * hardware's t grows upwards, so 1 - t is Vulkan's, which grows
	 * downwards from the upper left.
	 *
	 * Procedure referenced: Mesa 26.2.6 src/broadcom/compiler/nir_to_vir.c
	 * (the point_x and point_y varyings of a points key, lines 4881-4885,
	 * read through emit_smooth_varying, lines 1076-1083) and
	 * src/broadcom/vulkan/v3dv_pipeline.c (lower_point_coord_cb).
	 */
	if (compiler->binary->stage == BCM2711_SHADER_FRAGMENT && compiler->binary->point_coord != 0) {
		for (index = 0; index < 2; index++) {
			error = bcm2711_shader_register(compiler, &destination);
			if (error != 0)
				return error;
			compiler->point_registers[index] = destination;
			varying = bcm2711_shader_scratch(compiler, BCM2711_SHADER_SCRATCH_LEFT);
			error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_VARYING, varying, 0);
			if (error != 0)
				return error;
			work = bcm2711_shader_scratch(compiler, BCM2711_SHADER_SCRATCH_WORK);
			error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_MULTIPLY, work, 0, bcm2711_shader_rf(varying), bcm2711_shader_rf(3), 0, 0);
			if (error != 0)
				return error;
			error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_ADD, destination, 0, bcm2711_shader_rf(work), coefficient, 0, 0);
			if (error != 0)
				return error;
			if (index == 1U) {
				error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_SUBTRACT, destination, 0, bcm2711_shader_bits(0x3f800000U), bcm2711_shader_rf(destination), 0, 0);
				if (error != 0)
					return error;
			}
			bcm2711_shader_release_scratch(compiler);
		}
	}

	/* A vertex program's hardware ids come ahead of its attributes in the VPM. */
	ids = 0;
	if (compiler->binary->stage != BCM2711_SHADER_FRAGMENT) {
		error = preload_ids(compiler, &ids);
		if (error != 0)
			return error;
	}

	/* Each canonical scalar acquires one pinned register before any expression can reuse its storage. */
	for (index = 0; index < compiler->binary->input_count; index++) {
		error = bcm2711_shader_register(compiler, &destination);
		if (error != 0)
			return error;
		compiler->input_registers[index] = destination;

		/* Vertex attributes are consecutive scalar VPM words in the checked draw-time input order. */
		if (compiler->binary->stage != BCM2711_SHADER_FRAGMENT) {
			kern_memset(&uniform, 0, sizeof(uniform));
			uniform.kind = BCM2711_SHADER_CONSTANT;
			uniform.bits = ids + index;
			error = bcm2711_shader_uniform_load(compiler, &uniform, BCM2711_SHADER_SCRATCH_LEFT);
			if (error != 0)
				return error;
			left = bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_LEFT);
			right = bcm2711_shader_bits(0);
			error = bcm2711_shader_operation(compiler, BCM2711_QPU_VPM_LOAD, destination, 0, left, right, 0, 0);
			if (error != 0)
				return error;
			continue;
		}

		/*
		 * A varying transaction provides its interpolated term and places the
		 * constant term in rf0 one instruction later, so nothing reads or
		 * writes rf0 in the instruction right after the load.  A smooth
		 * varying's multiply fills that instruction; the others idle.
		 */
		component = &compiler->binary->inputs[index];
		gaps = 1;
		if (component->flat == 0 && component->noperspective == 0)
			gaps = 0;
		varying = bcm2711_shader_scratch(compiler, BCM2711_SHADER_SCRATCH_LEFT);
		error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_VARYING, varying, gaps);
		if (error != 0)
			return error;

		/* Flat interpolation takes the provoking vertex's constant term while still advancing the FIFO. */
		if (component->flat != 0) {
			right = bcm2711_shader_bits(0);
			error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, destination, 0, coefficient, right, 0, 0);
			if (error != 0)
				return error;
		} else {
			/*
			 * Smooth varyings multiply the interpolated term by the fragment
			 * payload's W, which 7.1 supplies in rf3 (Mesa 25.3.6
			 * nir_to_vir.c lines 4754-4757); linear varyings preserve it directly.
			 */
			left = bcm2711_shader_rf(varying);
			if (component->noperspective == 0) {
				right = bcm2711_shader_rf(3);
				work = bcm2711_shader_scratch(compiler, BCM2711_SHADER_SCRATCH_WORK);
				error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_MULTIPLY, work, 0, left, right, 0, 0);
				if (error != 0)
					return error;
				left = bcm2711_shader_rf(work);
			}

			/* The native constant coefficient is added before a later varying can overwrite rf0. */
			error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_ADD, destination, 0, left, coefficient, 0, 0);
			if (error != 0)
				return error;
		}

		/* The interpolated term's scratch registers have no later reader. */
		bcm2711_shader_release_scratch(compiler);
	}

	/* All source input aliases now name stable, once-only hardware results. */
	return 0;
}

/*
 * Reads a vertex program's hardware ids from the VPM, the instance id
 * first, then the vertex id, and adds the draw's bases: gl_InstanceIndex is
 * the instance id and the first instance, gl_VertexIndex the vertex id and
 * what the draw moved its vertices by.  *ids is the VPM words they take.
 *
 * Procedure referenced: Mesa 26.2.6 src/broadcom/compiler/nir_to_vir.c
 * (ntq_setup_vs_inputs: the instance id, base instance and vertex id read
 * in that order ahead of the attributes).
 */
static int
preload_ids(
	struct bcm2711_shader_compiler *compiler,
	uint32_t *ids)
{
	struct bcm2711_shader_uniform uniform;
	uint32_t wanted[2];
	uint32_t index;
	uint32_t destination;
	int error;

	/* The ids the program reads, in their VPM order. */
	*ids = 0;
	wanted[0] = compiler->binary->instance_index;
	wanted[1] = compiler->binary->vertex_index;
	for (index = 0; index < 2U; index++) {
		compiler->id_registers[index] = BCM2711_SHADER_ABSENT;
		if (wanted[index] == 0)
			continue;
		error = bcm2711_shader_register(compiler, &destination);
		if (error != 0)
			return error;
		compiler->id_registers[index] = destination;

		/* The id's VPM word. */
		kern_memset(&uniform, 0, sizeof(uniform));
		uniform.kind = BCM2711_SHADER_CONSTANT;
		uniform.bits = *ids;
		error = bcm2711_shader_uniform_load(compiler, &uniform, BCM2711_SHADER_SCRATCH_LEFT);
		if (error != 0)
			return error;
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_VPM_LOAD, BCM2711_SHADER_SCRATCH_WORK, 0, bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_LEFT), bcm2711_shader_bits(0), 0, 0);
		if (error != 0)
			return error;
		(*ids)++;

		/* The draw's base for it. */
		kern_memset(&uniform, 0, sizeof(uniform));
		uniform.kind = BCM2711_SHADER_INSTANCE_BASE;
		if (index == 1U)
			uniform.kind = BCM2711_SHADER_VERTEX_BASE;
		error = bcm2711_shader_uniform_load(compiler, &uniform, BCM2711_SHADER_SCRATCH_LEFT);
		if (error != 0)
			return error;
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_INTEGER_ADD, destination, 0, bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_WORK), bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_LEFT), 0, 0);
		if (error != 0)
			return error;
	}

	/* Succeeded: the ids are in their registers. */
	return 0;
}

/* Lowers one already validated scalar instruction without silently discarding unsupported effects. */
static int
lower_instruction(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	struct bcm2711_shader_value *scalar;
	struct bcm2711_shader_uniform uniform;
	const struct drv_gpu_shader_ir_uniform *block;
	const struct bcm2711_shader_component *component;
	uint32_t index;
	uint32_t destination;
	uint32_t handled;
	int error;

	/* Non-executable markers describe already checked source semantics and need no native instruction. */
	switch (instruction->op) {
	case DRV_GPU_IR_NOP:
	case DRV_GPU_IR_STORE_OUTPUT:
	case DRV_GPU_IR_SKIP_BEGIN:
	case DRV_GPU_IR_SKIP_END:
		return 0;
	default:
		break;
	}

	/*
	 * A compute program's built-in values, and the storage accesses, loops
	 * and integer operations any program may have, have a lowering of
	 * their own (the analysis admitted them for the stage).
	 */
	handled = 0;
	error = 0;
	if (instruction->op != DRV_GPU_IR_LOAD_SYSTEM || instruction->component != DRV_GPU_IR_SYSTEM_VIEW_INDEX)
		error = bcm2711_shader_compute_lower(compiler, instruction, &handled);
	if (error != 0)
		return error;
	if (handled != 0) {
		/* The scheduler does not model these operations' multiply pairs; a fragment program keeps its source order. */
		if (compiler->binary->stage == BCM2711_SHADER_FRAGMENT)
			compiler->in_order = 1;
		return 0;
	}

	/* Source constants remain exact bits until a consuming operation chooses a small constant or full uniform. */
	scalar = &compiler->values[instruction->dst];
	if (instruction->op == DRV_GPU_IR_CONST ||
	    instruction->op == DRV_GPU_IR_BOOL ||
	    instruction->op == DRV_GPU_IR_ICONST) {
		scalar->constant = 1;
		scalar->number = instruction->immediate;
		return 0;
	}

	/* gl_FragCoord and gl_FrontFacing are read from the payload and the facing flag where they are loaded. */
	if (instruction->op == DRV_GPU_IR_LOAD_INPUT &&
	    compiler->binary->stage == BCM2711_SHADER_FRAGMENT &&
	    (instruction->location == DRV_GPU_SHADER_LOCATION_FRAG_COORD ||
	     instruction->location == DRV_GPU_SHADER_LOCATION_FRONT_FACING)) {
		error = bcm2711_shader_register(compiler, &destination);
		if (error != 0)
			return error;
		scalar->number = destination;
		scalar->register_live = 1;
		error = lower_builtin(compiler, instruction, destination);
		if (error != 0)
			return error;

		/* This source operation has a complete native encoding. */
		return 0;
	}

	/* gl_PointCoord is the implicit point varyings of a points pipeline, and zero for other primitives. */
	if (instruction->op == DRV_GPU_IR_LOAD_INPUT && instruction->location == DRV_GPU_SHADER_LOCATION_POINT_COORD) {
		if (compiler->binary->stage != BCM2711_SHADER_FRAGMENT || instruction->component > 1)
			return EINVAL;
		if (compiler->binary->point_coord == 0) {
			scalar->constant = 1;
			scalar->number = 0;
			return 0;
		}
		scalar->number = compiler->point_registers[instruction->component];
		scalar->register_live = 1;
		scalar->pinned = 1;
		return 0;
	}

	/* gl_InstanceIndex and gl_VertexIndex are the registers the ids were read into. */
	if (instruction->op == DRV_GPU_IR_LOAD_INPUT &&
	    compiler->binary->stage != BCM2711_SHADER_FRAGMENT &&
	    (instruction->location == DRV_GPU_SHADER_LOCATION_INSTANCE_INDEX ||
	     instruction->location == DRV_GPU_SHADER_LOCATION_VERTEX_INDEX)) {
		index = 0;
		if (instruction->location == DRV_GPU_SHADER_LOCATION_VERTEX_INDEX)
			index = 1;
		if (instruction->component != 0 || compiler->id_registers[index] == BCM2711_SHADER_ABSENT)
			return EINVAL;
		scalar->number = compiler->id_registers[index];
		scalar->register_live = 1;
		scalar->pinned = 1;
		return 0;
	}

	/* Repeated input loads alias the same pinned register and never reread the VPM or varying FIFO. */
	if (instruction->op == DRV_GPU_IR_LOAD_INPUT) {
		for (index = 0; index < compiler->binary->input_count; index++) {
			component = &compiler->binary->inputs[index];
			if (component->location == instruction->location && component->component == instruction->component) {
				scalar->number = compiler->input_registers[index];
				scalar->register_live = 1;
				scalar->pinned = 1;
				return 0;
			}
		}

		/* Undeclared inputs cannot alias unrelated native FIFO words. */
		return EINVAL;
	}

	/* Sampling allocates four exact result scalars and completes the native TMU transaction before publication. */
	if (instruction->op == DRV_GPU_IR_SAMPLE ||
	    instruction->op == DRV_GPU_IR_SAMPLE_BIAS ||
	    instruction->op == DRV_GPU_IR_SAMPLE_LOD ||
	    instruction->op == DRV_GPU_IR_TEXTURE) {
		error = lower_sample(compiler, instruction);
		if (error != 0)
			return error;

		/* This source operation has a complete native encoding. */
		return 0;
	}

	/* A discard defines no value; it clears the coverage of the lanes it names. */
	if (instruction->op == DRV_GPU_IR_KILL) {
		error = lower_kill(compiler, instruction);
		if (error != 0)
			return error;

		/* This source operation has a complete native encoding. */
		return 0;
	}

	/* An expression destination cannot overwrite a still-live source register. */
	error = bcm2711_shader_register(compiler, &destination);
	if (error != 0)
		return error;
	scalar->number = destination;
	scalar->register_live = 1;

	/*
	 * Draw-time push and block words use checked descriptor identities
	 * instead of embedding device addresses; the view of a multiview pass
	 * is a draw-time word too.
	 */
	if (instruction->op == DRV_GPU_IR_LOAD_PUSH || instruction->op == DRV_GPU_IR_LOAD_UBO || instruction->op == DRV_GPU_IR_LOAD_SYSTEM) {
		kern_memset(&uniform, 0, sizeof(uniform));
		uniform.kind = BCM2711_SHADER_PUSH;
		uniform.offset = instruction->immediate;
		if (instruction->op == DRV_GPU_IR_LOAD_SYSTEM) {
			if (instruction->component != DRV_GPU_IR_SYSTEM_VIEW_INDEX)
				return EINVAL;
			uniform.kind = BCM2711_SHADER_VIEW_INDEX;
			uniform.offset = 0;
		} else if (instruction->op == DRV_GPU_IR_LOAD_UBO) {
			block = &compiler->ir->uniforms[instruction->location];
			uniform.kind = BCM2711_SHADER_BLOCK;
			uniform.set = block->set;
			uniform.binding = block->binding;
			uniform.element = block->element;
		}

		/* The runtime word's checked identity follows its actual uniform consumption. */
		error = bcm2711_shader_uniform_load(compiler, &uniform, destination);
		if (error != 0)
			return error;

		/* This source operation has a complete native encoding. */
		return 0;
	}

	/* If-converted selection preserves lane-wise source bits, including integer and Boolean values. */
	if (instruction->op == DRV_GPU_IR_SELECT) {
		error = lower_select(compiler, instruction, destination);
		if (error != 0)
			return error;

		/* This source operation has a complete native encoding. */
		return 0;
	}

	/* Every remaining accepted instruction is a completely implemented pure scalar arithmetic operation. */
	error = lower_arithmetic(compiler, instruction, destination);
	if (error != 0)
		return error;

	/* The destination remains live until its checked last reader or stage epilogue. */
	return 0;
}

/* Assigns native opcodes only where one QPU instruction implements the scalar operation exactly. */
static int
arithmetic_operation(
	enum drv_gpu_shader_ir_op operation,
	enum bcm2711_qpu_operation *native,
	uint32_t *sources)
{
	/* Binary source arity is replaced explicitly by unary operation cases. */
	*sources = 2;

	/* This table describes semantic operations rather than importing a hardware encoding table. */
	switch (operation) {
	case DRV_GPU_IR_FADD:
		*native = BCM2711_QPU_FLOAT_ADD;
		break;
	case DRV_GPU_IR_FSUB:
		*native = BCM2711_QPU_FLOAT_SUBTRACT;
		break;
	case DRV_GPU_IR_FMUL:
		*native = BCM2711_QPU_FLOAT_MULTIPLY;
		break;
	case DRV_GPU_IR_FMIN:
		*native = BCM2711_QPU_FLOAT_MINIMUM;
		break;
	case DRV_GPU_IR_FMAX:
		*native = BCM2711_QPU_FLOAT_MAXIMUM;
		break;
	case DRV_GPU_IR_AND:
	case DRV_GPU_IR_IAND:
		*native = BCM2711_QPU_BITS_AND;
		break;
	case DRV_GPU_IR_OR:
	case DRV_GPU_IR_IOR:
		*native = BCM2711_QPU_BITS_OR;
		break;
	case DRV_GPU_IR_IXOR:
		*native = BCM2711_QPU_BITS_XOR;
		break;
	case DRV_GPU_IR_IADD:
		*native = BCM2711_QPU_INTEGER_ADD;
		break;
	case DRV_GPU_IR_ISUB:
		*native = BCM2711_QPU_INTEGER_SUBTRACT;
		break;
	case DRV_GPU_IR_SHL:
		*native = BCM2711_QPU_SHIFT_LEFT;
		break;
	case DRV_GPU_IR_SHR:
		*native = BCM2711_QPU_SHIFT_RIGHT;
		break;
	case DRV_GPU_IR_ASR:
		*native = BCM2711_QPU_SHIFT_SIGNED;
		break;
	case DRV_GPU_IR_NOT:
	case DRV_GPU_IR_INOT:
		*native = BCM2711_QPU_BITS_NOT;
		*sources = 1;
		break;
	case DRV_GPU_IR_MOVE:
		*native = BCM2711_QPU_MOVE;
		*sources = 1;
		break;
	case DRV_GPU_IR_INEG:
		*native = BCM2711_QPU_INTEGER_NEGATE;
		*sources = 1;
		break;
	case DRV_GPU_IR_RCP:
		*native = BCM2711_QPU_FLOAT_RECIPROCAL;
		*sources = 1;
		break;
	case DRV_GPU_IR_RSQ:
		*native = BCM2711_QPU_FLOAT_INVERSE_ROOT;
		*sources = 1;
		break;
	case DRV_GPU_IR_EXP2:
		*native = BCM2711_QPU_FLOAT_EXPONENT;
		*sources = 1;
		break;
	case DRV_GPU_IR_LOG2:
		*native = BCM2711_QPU_FLOAT_LOGARITHM;
		*sources = 1;
		break;
	case DRV_GPU_IR_FLOOR:
		*native = BCM2711_QPU_FLOAT_FLOOR;
		*sources = 1;
		break;
	case DRV_GPU_IR_FTRUNC:
		*native = BCM2711_QPU_FLOAT_TRUNCATE;
		*sources = 1;
		break;
	case DRV_GPU_IR_FROUND_EVEN:
		*native = BCM2711_QPU_FLOAT_ROUND;
		*sources = 1;
		break;
	case DRV_GPU_IR_I2F:
		*native = BCM2711_QPU_SIGNED_TO_FLOAT;
		*sources = 1;
		break;
	case DRV_GPU_IR_U2F:
		*native = BCM2711_QPU_UNSIGNED_TO_FLOAT;
		*sources = 1;
		break;
	case DRV_GPU_IR_F2I:
		*native = BCM2711_QPU_FLOAT_TO_SIGNED;
		*sources = 1;
		break;
	case DRV_GPU_IR_F2U:
		*native = BCM2711_QPU_FLOAT_TO_UNSIGNED;
		*sources = 1;
		break;
	case DRV_GPU_IR_DDX:
	case DRV_GPU_IR_DDX_FINE:
		/* The hardware's difference is the fine one, which a coarse derivative may be. */
		*native = BCM2711_QPU_DERIVATIVE_X;
		*sources = 1;
		break;
	case DRV_GPU_IR_DDY:
	case DRV_GPU_IR_DDY_FINE:
		*native = BCM2711_QPU_DERIVATIVE_Y;
		*sources = 1;
		break;
	case DRV_GPU_IR_PACK_HALF:
		/* The first source is the low half. */
		*native = BCM2711_QPU_PACK_HALVES;
		break;
	default:
		return ENOTSUP;
	}

	/* Compound scalar operations are deliberately excluded from this single-instruction contract. */
	return 0;
}

/* Lowers scalar arithmetic, comparisons and finite/special-value square root without kernel floating point. */
static int
lower_arithmetic(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t destination)
{
	struct bcm2711_qpu_source left;
	struct bcm2711_qpu_source right;
	struct bcm2711_qpu_source temporary;
	enum bcm2711_qpu_operation native;
	uint32_t sources;
	uint32_t flags;
	uint32_t predicate;
	uint32_t scratch;
	uint32_t mask;
	int error;

	/* Simple native operations use exactly their classified source arity. */
	error = arithmetic_operation(instruction->op, &native, &sources);
	if (error == 0) {
		if (sources == 2) {
			error = operands(compiler, instruction->src[0], instruction->src[1], &left, &right);
		} else {
			error = bcm2711_shader_operand(compiler, instruction->src[0], BCM2711_SHADER_SCRATCH_LEFT, &left);
			right = bcm2711_shader_bits(0);
		}

		/* A refused source cannot influence any emitted native arithmetic word. */
		if (error != 0)
			return error;

		/* Equal max operands cannot select its distinct mux ordering and are exactly a bit-preserving move. */
		if (native == BCM2711_QPU_FLOAT_MAXIMUM &&
		    left.kind == right.kind &&
		    left.number == right.number)
			native = BCM2711_QPU_MOVE;
		error = bcm2711_shader_operation(compiler, native, destination, 0, left, right, 0, 0);
		if (error != 0)
			return error;

		/* This source operation has a complete native encoding. */
		return 0;
	}

	/* Negation and absolute value change only the IEEE sign bit, including signed zero and NaN payloads. */
	if (instruction->op == DRV_GPU_IR_FNEG || instruction->op == DRV_GPU_IR_FABS) {
		error = bcm2711_shader_operand(compiler, instruction->src[0], BCM2711_SHADER_SCRATCH_LEFT, &left);
		if (error != 0)
			return error;
		mask = 0x80000000U;
		native = BCM2711_QPU_BITS_XOR;
		if (instruction->op == DRV_GPU_IR_FABS) {
			mask = 0x7fffffffU;
			native = BCM2711_QPU_BITS_AND;
		}

		/* Full-width sign masks are preserved through a cached or independent second scratch register. */
		error = bcm2711_shader_constant(compiler, mask, BCM2711_SHADER_SCRATCH_RIGHT, &scratch);
		if (error != 0)
			return error;
		right = bcm2711_shader_rf(scratch);
		error = bcm2711_shader_operation(compiler, native, destination, 0, left, right, 0, 0);
		if (error != 0)
			return error;

		/* This source operation has a complete native encoding. */
		return 0;
	}

	/* One half of a packed pair, unpacked by the float move. */
	if (instruction->op == DRV_GPU_IR_UNPACK_HALF) {
		error = bcm2711_shader_operand(compiler, instruction->src[0], BCM2711_SHADER_SCRATCH_LEFT, &left);
		if (error != 0)
			return error;
		native = BCM2711_QPU_HALF_LOW;
		if (instruction->component != 0)
			native = BCM2711_QPU_HALF_HIGH;
		error = bcm2711_shader_operation(compiler, native, destination, 0, left, bcm2711_shader_bits(0), 0, 0);
		if (error != 0)
			return error;

		/* This source operation has a complete native encoding. */
		return 0;
	}

	/* Sine and cosine reduce the angle to half a period for the special function. */
	if (instruction->op == DRV_GPU_IR_SIN || instruction->op == DRV_GPU_IR_COS) {
		error = lower_sine(compiler, instruction, destination);
		if (error != 0)
			return error;

		/* This source operation has a complete native encoding. */
		return 0;
	}

	/* Fraction uses the original scalar after the independent floor result is ready. */
	if (instruction->op == DRV_GPU_IR_FRACT) {
		error = bcm2711_shader_operand(compiler, instruction->src[0], BCM2711_SHADER_SCRATCH_LEFT, &left);
		if (error != 0)
			return error;
		right = bcm2711_shader_bits(0);
		scratch = bcm2711_shader_scratch(compiler, BCM2711_SHADER_SCRATCH_WORK);
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_FLOOR, scratch, 0, left, right, 0, 0);
		if (error != 0)
			return error;
		right = bcm2711_shader_rf(scratch);
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_SUBTRACT, destination, 0, left, right, 0, 0);
		if (error != 0)
			return error;

		/* This source operation has a complete native encoding. */
		return 0;
	}

	/* Native comparison flags preserve ordered NaN semantics rather than approximating a comparison by subtraction. */
	if (instruction->op == DRV_GPU_IR_FLT ||
	    instruction->op == DRV_GPU_IR_FGE ||
	    instruction->op == DRV_GPU_IR_FEQ ||
	    instruction->op == DRV_GPU_IR_FNEU) {
		error = operands(compiler, instruction->src[0], instruction->src[1], &left, &right);
		if (error != 0)
			return error;
		flags = 1;
		predicate = 1;
		if (instruction->op == DRV_GPU_IR_FLT) {
			flags = 2;
		} else if (instruction->op == DRV_GPU_IR_FGE) {
			flags = 3;
			temporary = left;
			left = right;
			right = temporary;
		} else if (instruction->op == DRV_GPU_IR_FNEU) {
			predicate = 3;
		}

		/* Ordered comparison flags precede the default-false and predicate-true bit moves. */
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_COMPARE, 6, 1, left, right, 0, flags);
		if (error != 0)
			return error;
		left = bcm2711_shader_bits(0);
		right = left;
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, destination, 0, left, right, 0, 0);
		if (error != 0)
			return error;
		left = bcm2711_shader_bits(0xffffffffU);
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, destination, 0, left, right, predicate, 0);
		if (error != 0)
			return error;

		/* This source operation has a complete native encoding. */
		return 0;
	}

	/* Square root preserves zero and positive infinity instead of leaving zero times infinity as NaN. */
	if (instruction->op == DRV_GPU_IR_SQRT) {
		error = bcm2711_shader_operand(compiler, instruction->src[0], BCM2711_SHADER_SCRATCH_LEFT, &left);
		if (error != 0)
			return error;
		error = force_register(compiler, &left, BCM2711_SHADER_SCRATCH_LEFT);
		if (error != 0)
			return error;
		right = bcm2711_shader_bits(0);
		scratch = bcm2711_shader_scratch(compiler, BCM2711_SHADER_SCRATCH_WORK);
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_INVERSE_ROOT, scratch, 0, left, right, 0, 0);
		if (error != 0)
			return error;
		right = bcm2711_shader_rf(scratch);
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_MULTIPLY, destination, 0, left, right, 0, 0);
		if (error != 0)
			return error;

		/* Both signs of zero retain their original exact bits. */
		error = bcm2711_shader_constant(compiler, 0x7fffffffU, BCM2711_SHADER_SCRATCH_RIGHT, &scratch);
		if (error != 0)
			return error;
		right = bcm2711_shader_rf(scratch);
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_BITS_AND, 6, 1, left, right, 0, 1);
		if (error != 0)
			return error;
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, destination, 0, left, right, 1, 0);
		if (error != 0)
			return error;

		/* Positive infinity must also survive the otherwise indeterminate multiply by its zero reciprocal root. */
		error = bcm2711_shader_constant(compiler, 0x7f800000U, BCM2711_SHADER_SCRATCH_RIGHT, &scratch);
		if (error != 0)
			return error;
		right = bcm2711_shader_rf(scratch);
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_BITS_XOR, 6, 1, left, right, 0, 1);
		if (error != 0)
			return error;
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, destination, 0, left, right, 1, 0);
		if (error != 0)
			return error;

		/* This source operation has a complete native encoding. */
		return 0;
	}

	/* Validation and lowering must agree about every accepted operation. */
	return ENOTSUP;
}

/* Chooses source bits lane by lane after an exact Boolean zero test. */
static int
lower_select(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t destination)
{
	struct bcm2711_qpu_source condition;
	struct bcm2711_qpu_source selected;
	struct bcm2711_qpu_source ignored;
	int error;

	/*
	 * The zero test records lane flags before materialization transactions
	 * that preserve those flags.  A move pushes the flags of its one input,
	 * so a constant condition needs no second small immediate.
	 */
	ignored = bcm2711_shader_bits(0);
	error = bcm2711_shader_operand(compiler, instruction->src[0], BCM2711_SHADER_SCRATCH_WORK, &condition);
	if (error != 0)
		return error;
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, 6, 1, condition, ignored, 0, 1);
	if (error != 0)
		return error;

	/* False lanes take the default bit-preserving source. */
	error = bcm2711_shader_operand(compiler, instruction->src[2], BCM2711_SHADER_SCRATCH_LEFT, &selected);
	if (error != 0)
		return error;
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, destination, 0, selected, ignored, 0, 0);
	if (error != 0)
		return error;

	/* Only nonzero Boolean lanes overwrite the destination with their selected true source. */
	error = bcm2711_shader_operand(compiler, instruction->src[1], BCM2711_SHADER_SCRATCH_LEFT, &selected);
	if (error != 0)
		return error;
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, destination, 0, selected, ignored, 3, 0);
	if (error != 0)
		return error;

	/* The selection retains exact scalar bits rather than converting a Boolean or integer into a float. */
	return 0;
}

/*
 * Lowers sin(x), or cos(x) as sin(x + pi / 2): t = x / pi (+ 1/2 for the
 * cosine), and with n = round(t) the special function gives
 * sin(pi (t - n)) for the reduced t - n in [-1/2, 1/2]; an odd n flips the
 * sign, which the low bit of n shifted to bit 31 does.
 *
 * Procedure referenced: Mesa 26.2.6 src/broadcom/compiler/nir_to_vir.c
 * (ntq_fsincos, lines 991-1002: the special function's sine takes its
 * argument in half periods).
 */
static int
lower_sine(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t destination)
{
	struct bcm2711_qpu_source left;
	struct bcm2711_qpu_source none;
	uint32_t scratch;
	uint32_t periods;
	int error;

	/* t = x / pi, a half period later for the cosine. */
	none = bcm2711_shader_bits(0);
	error = bcm2711_shader_operand(compiler, instruction->src[0], BCM2711_SHADER_SCRATCH_LEFT, &left);
	if (error != 0)
		return error;
	error = bcm2711_shader_constant(compiler, LOWER_INVERSE_PI, BCM2711_SHADER_SCRATCH_RIGHT, &scratch);
	if (error != 0)
		return error;
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_MULTIPLY, destination, 0, left, bcm2711_shader_rf(scratch), 0, 0);
	if (error != 0)
		return error;
	if (instruction->op == DRV_GPU_IR_COS) {
		error = bcm2711_shader_constant(compiler, LOWER_HALF, BCM2711_SHADER_SCRATCH_RIGHT, &scratch);
		if (error != 0)
			return error;
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_ADD, destination, 0, bcm2711_shader_rf(destination), bcm2711_shader_rf(scratch), 0, 0);
		if (error != 0)
			return error;
	}

	/* n = round(t); the special function takes t - n. */
	periods = bcm2711_shader_scratch(compiler, BCM2711_SHADER_SCRATCH_WORK);
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_ROUND, periods, 0, bcm2711_shader_rf(destination), none, 0, 0);
	if (error != 0)
		return error;
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_SUBTRACT, destination, 0, bcm2711_shader_rf(destination), bcm2711_shader_rf(periods), 0, 0);
	if (error != 0)
		return error;
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_SINE, destination, 0, bcm2711_shader_rf(destination), none, 0, 0);
	if (error != 0)
		return error;

	/* The low bit of n at bit 31 (a shift by -1 is one by 31) flips the sign of an odd half period. */
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_TO_SIGNED, periods, 0, bcm2711_shader_rf(periods), none, 0, 0);
	if (error != 0)
		return error;
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_SHIFT_LEFT, periods, 0, bcm2711_shader_rf(periods), bcm2711_shader_bits(0xffffffffU), 0, 0);
	if (error != 0)
		return error;
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_BITS_XOR, destination, 0, bcm2711_shader_rf(destination), bcm2711_shader_rf(periods), 0, 0);
	if (error != 0)
		return error;

	/* The work register has no later reader. */
	bcm2711_shader_release_scratch(compiler);
	return 0;
}

/*
 * Lowers a load of gl_FragCoord or gl_FrontFacing.  The coordinate's x and
 * y are the pixel's centre as floats, z is the payload's Z in rf2 and w is
 * 1 / W, the payload's W in rf3; the facing flag is zero on a front face,
 * so adding -1 makes the Boolean true there and false on a back face.
 *
 * Procedure referenced: Mesa 26.2.6 src/broadcom/compiler/nir_to_vir.c
 * (emit_fragcoord_input, lines 1017-1024; load_front_face, lines
 * 3704-3712; the 7.x payload registers, lines 4862-4870).
 */
static int
lower_builtin(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t destination)
{
	struct bcm2711_qpu_source none;
	int error;

	/* The facing flag, made a Boolean. */
	none = bcm2711_shader_bits(0);
	if (instruction->location == DRV_GPU_SHADER_LOCATION_FRONT_FACING) {
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_REVERSE_FACE, destination, 0, none, none, 0, 0);
		if (error != 0)
			return error;
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_INTEGER_ADD, destination, 0, bcm2711_shader_rf(destination), bcm2711_shader_bits(0xffffffffU), 0, 0);
		if (error != 0)
			return error;
		return 0;
	}

	/* One component of the coordinate. */
	switch (instruction->component) {
	case 0:
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_PIXEL_X, destination, 0, none, none, 0, 0);
		break;
	case 1:
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_PIXEL_Y, destination, 0, none, none, 0, 0);
		break;
	case 2:
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, destination, 0, bcm2711_shader_rf(2), none, 0, 0);
		break;
	case 3:
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_RECIPROCAL, destination, 0, bcm2711_shader_rf(3), none, 0, 0);
		break;
	default:
		return EINVAL;
	}
	if (error != 0)
		return error;

	/* Succeeded: the component is in the destination. */
	return 0;
}

/*
 * Discards the lanes whose Boolean is true: the zero test pushes the lane
 * flags, and the lanes found nonzero set their multisample flags to zero,
 * so the tile buffer keeps neither their colour nor their depth.
 *
 * Values referenced: Mesa src/broadcom/compiler/nir_to_vir.c (a
 * conditional discard is a SETMSF of zero to the null address under the
 * condition's flags).
 */
static int
lower_kill(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	struct bcm2711_qpu_source condition;
	struct bcm2711_qpu_source zero;
	int error;

	/* The zero test records the lane flags. */
	zero = bcm2711_shader_bits(0);
	error = bcm2711_shader_operand(compiler, instruction->src[0], BCM2711_SHADER_SCRATCH_WORK, &condition);
	if (error != 0)
		return error;
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, 6, 1, condition, zero, 0, 1);
	if (error != 0)
		return error;

	/* Only the nonzero (true) lanes lose their coverage. */
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_SAMPLE_MASK, 6, 1, zero, zero, 3, 0);
	if (error != 0)
		return error;

	/* The discarded lanes' tile writes are now masked. */
	return 0;
}

/*
 * The TMU's write addresses: the coordinates T, R and the array index I,
 * the bias or level B, the depth reference DREF, and the writes that start
 * a lookup -- S (the level the derivatives choose), SCM (a cube map),
 * SF (a texel fetch) and SLOD (at the level in B).
 * Values referenced: Mesa 26.2.6 src/broadcom/qpu/qpu_instr.h
 * (V3D_QPU_WADDR_TMUS and on).
 */
#define TMU_S 33U
#define TMU_T 34U
#define TMU_R 35U
#define TMU_I 36U
#define TMU_B 37U
#define TMU_DREF 38U
#define TMU_SCM 40U
#define TMU_SF 41U
#define TMU_SLOD 42U

/*
 * The operation field of the TMU's third configuration word (bits 20 to
 * 23) for a regular lookup, which a word left out means.
 * Values referenced: Mesa 26.2.6 src/broadcom/cle/v3d_packet.xml (TMU
 * Config Parameter 2 of 7.1, the TMU Op enum's Regular 15) and
 * src/broadcom/compiler/v3d_tex.c (p2_unpacked_default).
 */
#define TMU_OP_REGULAR (15U << 20)

/*
 * The result component each of a gather's four words lands in.
 * Values referenced: Mesa 26.2.6 src/compiler/nir/nir_lower_tex.c
 * (swizzle_tg4_broadcom: the result is the words swizzled 2, 3, 1, 0).
 */
static const uint32_t gather_component[4] = { 3U, 2U, 0U, 1U };

/* The most TMU writes of one lookup: S, T, R, I, B and DREF. */
#define TMU_WRITES 6U

/*
 * One TMU write: its address and the IR value written, or the float 0.0
 * when value is absent, or the register number holds when it is not
 * absent.
 */
struct tmu_write {
	uint32_t address;
	uint32_t value;
	uint32_t number;
};

/*
 * Lowers a texture lookup: SAMPLE, SAMPLE_BIAS and SAMPLE_LOD of a 2D image
 * (u, v in src[0], src[1]; the bias or level in src[2]), and TEXTURE's
 * sample, compare, fetch and gather messages of 2D, 3D and cube images and
 * 2D arrays.  Every parameter but the coordinate S is written first, then
 * the configuration words -- the texture state with four 32-bit words
 * asked for, the sampler state, and when needed a third word with the
 * constant offset, the gather and the fixed level -- and S last starts the
 * lookup; the four words come back after a thread switch.  A program other
 * than a fragment one has no derivatives: it samples at the level 0 of the
 * view unless the message gives one, and asks for the lanes' own mask.
 *
 * Procedure referenced: Mesa 26.2.6 src/broadcom/compiler/v3d_tex.c
 * (handle_tex_src and v3d_vir_emit_tex) and src/broadcom/cle/v3d_packet.xml
 * (the TMU Config Parameter 1 and 2 of 7.1).
 */
static int
lower_sample(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	struct tmu_write writes[TMU_WRITES];
	struct bcm2711_shader_uniform uniform;
	struct bcm2711_qpu_source source;
	struct bcm2711_qpu_source ignored;
	uint32_t parameters[DRV_GPU_IR_TEXTURE_MAX_PARAMS];
	uint32_t message;
	uint32_t kind;
	uint32_t coordinates;
	uint32_t count;
	uint32_t first;
	uint32_t offset;
	uint32_t second;
	uint32_t third;
	uint32_t start;
	uint32_t start_value;
	uint32_t start_number;
	uint32_t texel[2];
	uint32_t index;
	uint32_t component;
	uint32_t destination;
	bool arrayed;
	bool level;
	int error;

	/* The message, its parameters and the image's shape; SAMPLE, SAMPLE_BIAS and SAMPLE_LOD are 2D messages of their own. */
	kern_memset(parameters, 0, sizeof(parameters));
	kind = DRV_GPU_IR_IMAGE_2D;
	arrayed = false;
	offset = 0;
	if (instruction->op == DRV_GPU_IR_TEXTURE) {
		message = instruction->component & DRV_GPU_IR_TEXTURE_MESSAGE_MASK;
		kind = instruction->image_shape & DRV_GPU_IR_IMAGE_KIND_MASK;
		if ((instruction->image_shape & DRV_GPU_IR_IMAGE_ARRAYED) != 0)
			arrayed = true;
		offset = (instruction->component >> 8) & 0xfffU;
		count = instruction->src[1];
		if (count == 0 || count > DRV_GPU_IR_TEXTURE_MAX_PARAMS)
			return EINVAL;
		for (index = 0; index < count; index++)
			parameters[index] = instruction->src[0] + index;
	} else {
		message = DRV_GPU_IR_TEXTURE_SAMPLE;
		offset = instruction->component & 0xfffU;
		count = 2;
		first = 0;
		if (instruction->op != DRV_GPU_IR_SAMPLE) {
			message = DRV_GPU_IR_TEXTURE_SAMPLE_BIAS;
			if (instruction->op == DRV_GPU_IR_SAMPLE_LOD)
				message = DRV_GPU_IR_TEXTURE_SAMPLE_LOD;
			parameters[0] = instruction->src[2];
			first = 1;
			count = 3;
		}
		parameters[first] = instruction->src[0];
		parameters[first + 1U] = instruction->src[1];
	}

	/* A size query reads the view's size, not a texel. */
	if (message == DRV_GPU_IR_TEXTURE_RESINFO)
		return lower_size(compiler, instruction, kind, arrayed, parameters[0]);

	/* The image's own coordinates: two for 2D, three for 3D and for a cube's direction; a cube array is not sampled. */
	if (kind == DRV_GPU_IR_IMAGE_2D)
		coordinates = 2;
	else if (kind == DRV_GPU_IR_IMAGE_3D || kind == DRV_GPU_IR_IMAGE_CUBE)
		coordinates = 3;
	else
		return bcm2711_shader_fail(compiler, ENOTSUP, "texture of an image shape the TMU path does not read");
	if (kind == DRV_GPU_IR_IMAGE_CUBE && arrayed)
		return bcm2711_shader_fail(compiler, ENOTSUP, "cube map array sampling");
	if ((instruction->image_shape & DRV_GPU_IR_IMAGE_MULTISAMPLED) != 0 &&
	    (message != DRV_GPU_IR_TEXTURE_LD || kind != DRV_GPU_IR_IMAGE_2D || arrayed || count != 4U))
		return bcm2711_shader_fail(compiler, ENOTSUP, "multisampled image read other than a fetch of one sample");

	/* The messages' leading parameter: the reference of a compare, then a bias or a level. */
	first = 0;
	kern_memset(writes, 0, sizeof(writes));
	third = 0;
	level = false;
	switch (message) {
	case DRV_GPU_IR_TEXTURE_SAMPLE:
	case DRV_GPU_IR_TEXTURE_GATHER4:
		break;
	case DRV_GPU_IR_TEXTURE_SAMPLE_BIAS:
	case DRV_GPU_IR_TEXTURE_SAMPLE_LOD:
		first = 1;
		break;
	case DRV_GPU_IR_TEXTURE_SAMPLE_COMPARE:
	case DRV_GPU_IR_TEXTURE_GATHER4_COMPARE:
		first = 1;
		break;
	case DRV_GPU_IR_TEXTURE_SAMPLE_BIAS_COMPARE:
	case DRV_GPU_IR_TEXTURE_SAMPLE_LOD_COMPARE:
		first = 2;
		break;
	case DRV_GPU_IR_TEXTURE_LD:
		break;
	default:
		return bcm2711_shader_fail(compiler, ENOTSUP, "texture message the TMU path does not lower");
	}

	/* A parameter the message stops before reads as zero. */
	for (index = count; index < DRV_GPU_IR_TEXTURE_MAX_PARAMS; index++)
		parameters[index] = BCM2711_SHADER_ABSENT;
	if (count <= first)
		return EINVAL;
	for (index = 0; index < TMU_WRITES; index++)
		writes[index].number = BCM2711_SHADER_ABSENT;
	start_number = BCM2711_SHADER_ABSENT;

	/* A fetch takes u, v, the level, then r (the slice or the layer); the others the coordinate after the leading values. */
	if (message == DRV_GPU_IR_TEXTURE_LD) {
		if (kind == DRV_GPU_IR_IMAGE_CUBE)
			return bcm2711_shader_fail(compiler, ENOTSUP, "texel fetch of a cube map");
		start_value = parameters[0];
		index = 0;
		writes[index].address = TMU_T;
		writes[index].value = parameters[1];

		/* A multisampled image's sample is a texel of the raster: u and v move to it, and the sample is no layer. */
		if ((instruction->image_shape & DRV_GPU_IR_IMAGE_MULTISAMPLED) != 0) {
			error = multisample_texel(compiler, parameters, texel);
			if (error != 0)
				return error;
			start_number = texel[0];
			writes[index].number = texel[1];
		}
		index++;
		if (kind == DRV_GPU_IR_IMAGE_3D || arrayed) {
			writes[index].address = TMU_R;
			if (arrayed)
				writes[index].address = TMU_I;
			writes[index].value = parameters[3];
			index++;
		}
		writes[index].address = TMU_B;
		writes[index].value = parameters[2];
		index++;
		start = TMU_SF;
	} else {
		/* Without derivatives only a level the message gives is used: a bias needs a fragment. */
		if (compiler->binary->stage != BCM2711_SHADER_FRAGMENT &&
		    (message == DRV_GPU_IR_TEXTURE_SAMPLE_BIAS || message == DRV_GPU_IR_TEXTURE_SAMPLE_BIAS_COMPARE))
			return bcm2711_shader_fail(compiler, ENOTSUP, "texture bias outside a fragment program");
		start_value = parameters[first];
		index = 0;
		writes[index].address = TMU_T;
		writes[index].value = parameters[first + 1U];
		index++;
		if (coordinates == 3U) {
			writes[index].address = TMU_R;
			writes[index].value = parameters[first + 2U];
			index++;
		}
		if (arrayed) {
			writes[index].address = TMU_I;
			writes[index].value = parameters[first + coordinates];
			index++;
		}

		/* The bias or the level, after the reference of a compare that has one. */
		if (message == DRV_GPU_IR_TEXTURE_SAMPLE_BIAS || message == DRV_GPU_IR_TEXTURE_SAMPLE_LOD ||
		    message == DRV_GPU_IR_TEXTURE_SAMPLE_BIAS_COMPARE || message == DRV_GPU_IR_TEXTURE_SAMPLE_LOD_COMPARE) {
			writes[index].address = TMU_B;
			writes[index].value = parameters[first - 1U];
			index++;
			if (message == DRV_GPU_IR_TEXTURE_SAMPLE_LOD || message == DRV_GPU_IR_TEXTURE_SAMPLE_LOD_COMPARE)
				level = true;
		}

		/* A program without derivatives samples at the level 0 of the view, unless the message says otherwise. */
		if (compiler->binary->stage != BCM2711_SHADER_FRAGMENT && !level && message != DRV_GPU_IR_TEXTURE_GATHER4 &&
		    message != DRV_GPU_IR_TEXTURE_GATHER4_COMPARE) {
			writes[index].address = TMU_B;
			writes[index].value = BCM2711_SHADER_ABSENT;
			index++;
			level = true;
		}

		/* The depth reference of a compare. */
		if (message == DRV_GPU_IR_TEXTURE_SAMPLE_COMPARE || message == DRV_GPU_IR_TEXTURE_GATHER4_COMPARE ||
		    message == DRV_GPU_IR_TEXTURE_SAMPLE_BIAS_COMPARE || message == DRV_GPU_IR_TEXTURE_SAMPLE_LOD_COMPARE) {
			writes[index].address = TMU_DREF;
			writes[index].value = parameters[0];
			index++;
		}

		/* The write that starts the lookup; a cube's level is fixed by the third word instead of SLOD. */
		start = TMU_S;
		if (kind == DRV_GPU_IR_IMAGE_CUBE) {
			start = TMU_SCM;
			if (level)
				third |= 1U << 1;
		} else if (level) {
			start = TMU_SLOD;
		}

		/* A gather reads level 0 without automatic level, one channel of the four texels. */
		if (message == DRV_GPU_IR_TEXTURE_GATHER4 || message == DRV_GPU_IR_TEXTURE_GATHER4_COMPARE)
			third |= (1U << 7) | (DRV_GPU_IR_TEXTURE_CHANNEL(instruction->component) << 5) | (1U << 1);
	}

	/* The constant offset: s, t and r, four signed bits each. */
	third |= ((offset >> 8) & 15U) << 8;
	third |= ((offset >> 4) & 15U) << 12;
	third |= (offset & 15U) << 16;

	/* Every write but S, in order; an absent value writes 0.0. */
	ignored = bcm2711_shader_bits(0);
	for (second = 0; second < index; second++) {
		if (writes[second].number != BCM2711_SHADER_ABSENT) {
			source = bcm2711_shader_rf(writes[second].number);
		} else if (writes[second].value == BCM2711_SHADER_ABSENT) {
			source = bcm2711_shader_bits(0);
		} else {
			error = bcm2711_shader_operand(compiler, writes[second].value, BCM2711_SHADER_SCRATCH_LEFT, &source);
			if (error != 0)
				return error;
		}
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, writes[second].address, 1, source, ignored, 0, 0);
		if (error != 0)
			return error;
	}

	/* The texture state with four 32-bit words asked for. */
	kern_memset(&uniform, 0, sizeof(uniform));
	uniform.kind = BCM2711_SHADER_TEXTURE;
	uniform.set = instruction->location;
	uniform.binding = instruction->immediate;
	uniform.element = instruction->element;
	uniform.bits = 15;
	error = bcm2711_shader_uniform_append(compiler, &uniform);
	if (error != 0)
		return error;
	error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_TEXTURE_CONFIG, 0, 0);
	if (error != 0)
		return error;

	/* The sampler state: 32-bit output, and outside a fragment program the lanes' own mask. */
	uniform.kind = BCM2711_SHADER_SAMPLER;
	uniform.bits = 1;
	if (compiler->binary->stage != BCM2711_SHADER_FRAGMENT)
		uniform.bits |= 4U;
	error = bcm2711_shader_uniform_append(compiler, &uniform);
	if (error != 0)
		return error;
	error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_TEXTURE_CONFIG, 0, 0);
	if (error != 0)
		return error;

	/*
	 * The third word only when it says something.  The word left out is
	 * the regular lookup; a word written names its operation (bits 20 to
	 * 23), and 0 there is a prefetch that returns nothing, so the reads
	 * of the result would wait for ever.
	 */
	if (third != 0) {
		kern_memset(&uniform, 0, sizeof(uniform));
		uniform.kind = BCM2711_SHADER_CONSTANT;
		uniform.bits = third | TMU_OP_REGULAR;
		error = bcm2711_shader_uniform_append(compiler, &uniform);
		if (error != 0)
			return error;
		error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_TEXTURE_CONFIG, 0, 0);
		if (error != 0)
			return error;
	}

	/* The S write launches exactly one lookup owned by this compiled shader. */
	if (start_number != BCM2711_SHADER_ABSENT) {
		source = bcm2711_shader_rf(start_number);
	} else {
		error = bcm2711_shader_operand(compiler, start_value, BCM2711_SHADER_SCRATCH_LEFT, &source);
		if (error != 0)
			return error;
	}
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, start, 1, source, ignored, 0, 0);
	if (error != 0)
		return error;

	/* A regular switch plus its two delay slots permits the other thread to run while the lookup completes. */
	error = bcm2711_shader_switch(compiler, BCM2711_SHADER_SWITCH_SAMPLE);
	if (error != 0)
		return error;

	/*
	 * Every requested FIFO result word gets its own independently live
	 * physical register, readable by the next instruction.  A gather's
	 * words come as the texels (i0, j0), (i1, j0), (i0, j1), (i1, j1):
	 * Vulkan's order puts them in w, z, x and y.
	 */
	for (index = 0; index < 4; index++) {
		component = index;
		if (message == DRV_GPU_IR_TEXTURE_GATHER4 || message == DRV_GPU_IR_TEXTURE_GATHER4_COMPARE)
			component = gather_component[index];
		error = bcm2711_shader_register(compiler, &destination);
		if (error != 0)
			return error;
		compiler->values[instruction->dst + component].number = destination;
		compiler->values[instruction->dst + component].register_live = 1;
		error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_TEXTURE, destination, 0);
		if (error != 0)
			return error;
	}

	/* No unconsumed texture result is left in the FIFO for a following shader operation. */
	return 0;
}

/*
 * Computes the raster texel of a multisampled fetch's sample into two
 * registers borrowed for the instruction: a pixel's four samples are a
 * 2x2 block, so u' = 2u + (s & 1) and v' = 2v + ((s >> 1) & 1), from the
 * fetch's u, v and sample (its parameters 0, 1 and 3).
 *
 * Procedure referenced: Mesa 26.2.6 src/broadcom/compiler/
 * v3d_nir_lower_txf_ms.c (v3d_nir_lower_txf_ms_instr).
 */
static int
multisample_texel(
	struct bcm2711_shader_compiler *compiler,
	const uint32_t *parameters,
	uint32_t *texel)
{
	struct bcm2711_qpu_source coordinate;
	struct bcm2711_qpu_source sample;
	struct bcm2711_qpu_source one;
	uint32_t axis;
	int error;

	/* Each axis: twice the coordinate, plus the sample's bit of that axis. */
	one = bcm2711_shader_bits(1);
	for (axis = 0; axis < 2U; axis++) {
		/* A register for the axis's texel, kept until the lookup is sent. */
		error = bcm2711_shader_register(compiler, &texel[axis]);
		if (error != 0)
			return error;
		compiler->borrowed[texel[axis]] = 1;

		/* The sample's bit of the axis, in the work scratch register. */
		error = bcm2711_shader_operand(compiler, parameters[3], BCM2711_SHADER_SCRATCH_RIGHT, &sample);
		if (error != 0)
			return error;
		error = force_register(compiler, &sample, BCM2711_SHADER_SCRATCH_RIGHT);
		if (error != 0)
			return error;
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_SHIFT_RIGHT, BCM2711_SHADER_SCRATCH_WORK, 0, sample, bcm2711_shader_bits(axis), 0, 0);
		if (error != 0)
			return error;
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_BITS_AND, BCM2711_SHADER_SCRATCH_WORK, 0, bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_WORK), one, 0, 0);
		if (error != 0)
			return error;

		/* Twice the coordinate, plus the bit. */
		error = bcm2711_shader_operand(compiler, parameters[axis], BCM2711_SHADER_SCRATCH_LEFT, &coordinate);
		if (error != 0)
			return error;
		error = force_register(compiler, &coordinate, BCM2711_SHADER_SCRATCH_LEFT);
		if (error != 0)
			return error;
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_SHIFT_LEFT, texel[axis], 0, coordinate, one, 0, 0);
		if (error != 0)
			return error;
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_INTEGER_ADD, texel[axis], 0, bcm2711_shader_rf(texel[axis]), bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_WORK), 0, 0);
		if (error != 0)
			return error;
	}

	/* Succeeded: the texel's u and v are in their registers. */
	return 0;
}

/*
 * Lowers a size query (OpImageQuerySizeLod): the view's width, height and
 * a 3D image's depth at the level queried, max(size >> level, 1), then an
 * array's layers; the values past them read zero.  The sizes are uniforms
 * the draw fills from the view, as Mesa's V3D compiler reads them.
 *
 * Procedure referenced: Mesa 26.2.6 src/broadcom/compiler/v3d_compiler.h
 * (QUNIFORM_TEXTURE_WIDTH, HEIGHT, DEPTH and ARRAY_SIZE) and nir_to_vir.c
 * (the size query's uniforms, with the level's shift lowered before).
 */
static int
lower_size(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t kind,
	bool arrayed,
	uint32_t level)
{
	struct bcm2711_shader_uniform uniform;
	struct bcm2711_qpu_source source;
	struct bcm2711_qpu_source shift;
	uint32_t axes[4];
	uint32_t count;
	uint32_t sized;
	uint32_t index;
	uint32_t destination;
	bool shifted;
	bool storage;
	int error;

	/* A storage image's size, which a compute program asks for (imageSize), comes from its own uniform. */
	storage = false;
	for (index = 0; index < compiler->ir->uniform_count; index++) {
		if (compiler->ir->uniforms[index].kind == DRV_GPU_IR_UNIFORM_STORAGE_IMAGE &&
		    compiler->ir->uniforms[index].set == instruction->location &&
		    compiler->ir->uniforms[index].binding == instruction->immediate)
			storage = true;
	}

	/* The view's axes: width and height, a 3D image's depth, then an array's layers (bits 0 to 3 of the uniform). */
	if (kind == DRV_GPU_IR_IMAGE_3D) {
		if (arrayed)
			return EINVAL;
		sized = 3;
	} else if (kind == DRV_GPU_IR_IMAGE_2D || kind == DRV_GPU_IR_IMAGE_CUBE) {
		sized = 2;
	} else {
		return bcm2711_shader_fail(compiler, ENOTSUP, "size query of an image shape the TMU path does not read");
	}
	if (kind == DRV_GPU_IR_IMAGE_CUBE && arrayed)
		return bcm2711_shader_fail(compiler, ENOTSUP, "size query of a cube map array");
	count = sized;
	for (index = 0; index < sized; index++)
		axes[index] = index;
	if (arrayed) {
		axes[count] = 3;
		count++;
	}

	/* The level shifts the sizes unless it is the constant zero. */
	shifted = true;
	if (level == BCM2711_SHADER_ABSENT ||
	    (compiler->values[level].constant != 0 && compiler->values[level].number == 0))
		shifted = false;

	/* Each of the four results. */
	for (index = 0; index < 4U; index++) {
		error = bcm2711_shader_register(compiler, &destination);
		if (error != 0)
			return error;
		compiler->values[instruction->dst + index].number = destination;
		compiler->values[instruction->dst + index].register_live = 1;

		/* A value past the view's axes is zero. */
		if (index >= count) {
			error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, destination, 0, bcm2711_shader_bits(0), bcm2711_shader_bits(0), 0, 0);
			if (error != 0)
				return error;
			continue;
		}

		/* The axis's size at the view's base level: a sampled image's, or a storage image's. */
		kern_memset(&uniform, 0, sizeof(uniform));
		uniform.kind = BCM2711_SHADER_TEXTURE_SIZE;
		if (storage)
			uniform.kind = BCM2711_SHADER_IMAGE_SIZE;
		uniform.set = instruction->location;
		uniform.binding = instruction->immediate;
		uniform.element = instruction->element;
		uniform.bits = axes[index];
		error = bcm2711_shader_uniform_load(compiler, &uniform, destination);
		if (error != 0)
			return error;
		if (!shifted || axes[index] == 3U)
			continue;

		/* At the level queried, no smaller than one texel. */
		error = bcm2711_shader_operand(compiler, level, BCM2711_SHADER_SCRATCH_LEFT, &shift);
		if (error != 0)
			return error;
		error = force_register(compiler, &shift, BCM2711_SHADER_SCRATCH_LEFT);
		if (error != 0)
			return error;
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_SHIFT_RIGHT, destination, 0, bcm2711_shader_rf(destination), shift, 0, 0);
		if (error != 0)
			return error;
		source = bcm2711_shader_bits(1);
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_INTEGER_MAXIMUM, destination, 0, bcm2711_shader_rf(destination), source, 0, 0);
		if (error != 0)
			return error;
		bcm2711_shader_release_scratch(compiler);
	}

	/* Succeeded: the sizes are in the results. */
	return 0;
}

/* Materializes two sources without letting two small constants compete for the word's one small immediate. */
static int
operands(
	struct bcm2711_shader_compiler *compiler,
	uint32_t left,
	uint32_t right,
	struct bcm2711_qpu_source *first,
	struct bcm2711_qpu_source *second)
{
	int error;

	/* Separate scratch registers keep full-width materialization of the second operand from replacing the first. */
	error = bcm2711_shader_operand(compiler, left, BCM2711_SHADER_SCRATCH_LEFT, first);
	if (error != 0)
		return error;
	error = bcm2711_shader_operand(compiler, right, BCM2711_SHADER_SCRATCH_RIGHT, second);
	if (error != 0)
		return error;

	/* A 7.1 word reads one small immediate, so a second constant, even an equal one, moves to a register. */
	if (first->kind == BCM2711_QPU_CONSTANT &&
	    second->kind == BCM2711_QPU_CONSTANT) {
		error = force_register(compiler, first, BCM2711_SHADER_SCRATCH_LEFT);
		if (error != 0)
			return error;
	}

	/* At most one small constant remains in the consuming instruction. */
	return 0;
}

/* Copies an exact non-register source into caller-owned scratch storage when native port constraints require it. */
static int
force_register(
	struct bcm2711_shader_compiler *compiler,
	struct bcm2711_qpu_source *source,
	uint32_t scratch)
{
	struct bcm2711_qpu_source ignored;
	uint32_t number;
	int error;

	/* An existing physical source already satisfies the requested register contract. */
	if (source->kind == BCM2711_QPU_REGISTER)
		return 0;

	/* A bit-preserving move introduces no floating-point rounding or flag change. */
	ignored = bcm2711_shader_bits(0);
	number = bcm2711_shader_scratch(compiler, scratch);
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, number, 0, *source, ignored, 0, 0);
	if (error != 0)
		return error;
	*source = bcm2711_shader_rf(number);

	/* The caller may now address both native read ports independently. */
	return 0;
}

/* Recycles only physical expression storage whose final source use has passed. */
static void
retire_values(
	struct bcm2711_shader_compiler *compiler)
{
	struct bcm2711_shader_value *scalar;
	uint32_t index;

	/* Input aliases remain pinned; final stage outputs have a last reader after the source instruction stream. */
	for (index = 0; index < compiler->ir->value_count; index++) {
		scalar = &compiler->values[index];
		if (scalar->register_live != 0 &&
		    scalar->pinned == 0 &&
		    scalar->last_read == compiler->instruction) {
			bcm2711_shader_release(compiler, scalar->number);
			scalar->register_live = 0;
		}
	}

	/* Scratch values borrowed for this source instruction have no later reader. */
	bcm2711_shader_release_scratch(compiler);

	/* Succeeded: the consumed scalar values no longer occupy compiler registers. */
	return;
}

/*
 * Names the register holding one word of a compute invocation's payload on
 * V3D 7.1: the first word (the workgroup's x and y ids) in rf3, the second
 * (its z id, memory slot and the local index) in rf2.
 *
 * Values referenced: Mesa 26.2.6 src/broadcom/compiler/nir_to_vir.c lines
 * 4899-4905 (rf3 and rf2 on 7.x, rf0 and rf2 on 4.2).  The
 * compiler gives no expression these registers, so the payload is read
 * where it arrives.
 */
uint32_t
bcm2711_shader_compute_payload(
	uint32_t word)
{
	/* The second word. */
	if (word != 0)
		return 2;

	/* The first word. */
	return 3;
}
