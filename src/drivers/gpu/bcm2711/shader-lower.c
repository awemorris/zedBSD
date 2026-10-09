/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Scalar lowering schedules one operation at a time and never imports an external compiler implementation. */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/shader-private.h"

static int preload_inputs(struct bcm2711_shader_compiler *compiler);
static int lower_instruction(struct bcm2711_shader_compiler *compiler, const struct i915_shader_ir_inst *instruction);
static int lower_arithmetic(struct bcm2711_shader_compiler *compiler, const struct i915_shader_ir_inst *instruction, uint32_t destination);
static int lower_select(struct bcm2711_shader_compiler *compiler, const struct i915_shader_ir_inst *instruction, uint32_t destination);
static int lower_sample(struct bcm2711_shader_compiler *compiler, const struct i915_shader_ir_inst *instruction);
static int operands(struct bcm2711_shader_compiler *compiler, uint32_t left, uint32_t right, struct bcm2711_qpu_source *first, struct bcm2711_qpu_source *second);
static int force_register(struct bcm2711_shader_compiler *compiler, struct bcm2711_qpu_source *source, uint32_t scratch);
static int arithmetic_operation(enum i915_shader_ir_op operation, enum bcm2711_qpu_operation *native, uint32_t *sources);
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

	/* Two-thread shaders have sixty-four physical registers, including the four reserved scratch registers. */
	compiler->binary->registers_used = 64;
	compiler->binary->starts_final = 0;
	if (compiler->binary->stage != BCM2711_SHADER_FRAGMENT && compiler->samples == 0)
		compiler->binary->starts_final = 1;

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
	int error;

	/* The varying coefficient accumulator survives all explicit idle slots and addressed uniform loads. */
	coefficient.kind = BCM2711_QPU_ACCUMULATOR;
	coefficient.number = 5;

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
			uniform.bits = index;
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

		/* A varying transaction provides its interpolated term and places the constant term in accumulator five. */
		error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_VARYING, BCM2711_SHADER_SCRATCH_LEFT, 2);
		if (error != 0)
			return error;
		component = &compiler->binary->inputs[index];

		/* Flat interpolation takes the provoking vertex's constant term while still advancing the FIFO. */
		if (component->flat != 0) {
			right = bcm2711_shader_bits(0);
			error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, destination, 0, coefficient, right, 0, 0);
			if (error != 0)
				return error;
			continue;
		}

		/* Smooth varyings multiply the interpolated term by fragment payload W; linear varyings preserve it directly. */
		left = bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_LEFT);
		if (component->noperspective == 0) {
			right = bcm2711_shader_rf(0);
			error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_MULTIPLY, BCM2711_SHADER_SCRATCH_WORK, 0, left, right, 0, 0);
			if (error != 0)
				return error;
			left = bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_WORK);
		}

		/* The native constant coefficient is added before a later varying can overwrite accumulator five. */
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_ADD, destination, 0, left, coefficient, 0, 0);
		if (error != 0)
			return error;
	}

	/* All source input aliases now name stable, once-only hardware results. */
	return 0;
}

/* Lowers one already validated scalar instruction without silently discarding unsupported effects. */
static int
lower_instruction(
	struct bcm2711_shader_compiler *compiler,
	const struct i915_shader_ir_inst *instruction)
{
	struct bcm2711_shader_value *scalar;
	struct bcm2711_shader_uniform uniform;
	const struct i915_shader_ir_uniform *block;
	const struct bcm2711_shader_component *component;
	uint32_t index;
	uint32_t destination;
	int error;

	/* Non-executable markers describe already checked source semantics and need no native instruction. */
	switch (instruction->op) {
	case I915_IR_NOP:
	case I915_IR_STORE_OUTPUT:
	case I915_IR_SKIP_BEGIN:
	case I915_IR_SKIP_END:
		return 0;
	default:
		break;
	}

	/* Source constants remain exact bits until a consuming operation chooses a small constant or full uniform. */
	scalar = &compiler->values[instruction->dst];
	if (instruction->op == I915_IR_CONST ||
	    instruction->op == I915_IR_BOOL ||
	    instruction->op == I915_IR_ICONST) {
		scalar->constant = 1;
		scalar->number = instruction->immediate;
		return 0;
	}

	/* Repeated input loads alias the same pinned register and never reread the VPM or varying FIFO. */
	if (instruction->op == I915_IR_LOAD_INPUT) {
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
	if (instruction->op == I915_IR_SAMPLE) {
		error = lower_sample(compiler, instruction);
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

	/* Draw-time push and block words use checked descriptor identities instead of embedding device addresses. */
	if (instruction->op == I915_IR_LOAD_PUSH || instruction->op == I915_IR_LOAD_UBO) {
		kern_memset(&uniform, 0, sizeof(uniform));
		uniform.kind = BCM2711_SHADER_PUSH;
		uniform.offset = instruction->immediate;
		if (instruction->op == I915_IR_LOAD_UBO) {
			block = &compiler->ir->uniforms[instruction->location];
			uniform.kind = BCM2711_SHADER_BLOCK;
			uniform.set = block->set;
			uniform.binding = block->binding;
		}

		/* The runtime word's checked identity follows its actual uniform consumption. */
		error = bcm2711_shader_uniform_load(compiler, &uniform, destination);
		if (error != 0)
			return error;

		/* This source operation has a complete native encoding. */
		return 0;
	}

	/* If-converted selection preserves lane-wise source bits, including integer and Boolean values. */
	if (instruction->op == I915_IR_SELECT) {
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
	enum i915_shader_ir_op operation,
	enum bcm2711_qpu_operation *native,
	uint32_t *sources)
{
	/* Binary source arity is replaced explicitly by unary operation cases. */
	*sources = 2;

	/* This table describes semantic operations rather than importing a hardware encoding table. */
	switch (operation) {
	case I915_IR_FADD:
		*native = BCM2711_QPU_FLOAT_ADD;
		break;
	case I915_IR_FSUB:
		*native = BCM2711_QPU_FLOAT_SUBTRACT;
		break;
	case I915_IR_FMUL:
		*native = BCM2711_QPU_FLOAT_MULTIPLY;
		break;
	case I915_IR_FMIN:
		*native = BCM2711_QPU_FLOAT_MINIMUM;
		break;
	case I915_IR_FMAX:
		*native = BCM2711_QPU_FLOAT_MAXIMUM;
		break;
	case I915_IR_AND:
	case I915_IR_IAND:
		*native = BCM2711_QPU_BITS_AND;
		break;
	case I915_IR_OR:
	case I915_IR_IOR:
		*native = BCM2711_QPU_BITS_OR;
		break;
	case I915_IR_IXOR:
		*native = BCM2711_QPU_BITS_XOR;
		break;
	case I915_IR_IADD:
		*native = BCM2711_QPU_INTEGER_ADD;
		break;
	case I915_IR_ISUB:
		*native = BCM2711_QPU_INTEGER_SUBTRACT;
		break;
	case I915_IR_SHL:
		*native = BCM2711_QPU_SHIFT_LEFT;
		break;
	case I915_IR_SHR:
		*native = BCM2711_QPU_SHIFT_RIGHT;
		break;
	case I915_IR_ASR:
		*native = BCM2711_QPU_SHIFT_SIGNED;
		break;
	case I915_IR_NOT:
	case I915_IR_INOT:
		*native = BCM2711_QPU_BITS_NOT;
		*sources = 1;
		break;
	case I915_IR_MOVE:
		*native = BCM2711_QPU_MOVE;
		*sources = 1;
		break;
	case I915_IR_INEG:
		*native = BCM2711_QPU_INTEGER_NEGATE;
		*sources = 1;
		break;
	case I915_IR_RCP:
		*native = BCM2711_QPU_FLOAT_RECIPROCAL;
		*sources = 1;
		break;
	case I915_IR_RSQ:
		*native = BCM2711_QPU_FLOAT_INVERSE_ROOT;
		*sources = 1;
		break;
	case I915_IR_EXP2:
		*native = BCM2711_QPU_FLOAT_EXPONENT;
		*sources = 1;
		break;
	case I915_IR_LOG2:
		*native = BCM2711_QPU_FLOAT_LOGARITHM;
		*sources = 1;
		break;
	case I915_IR_FLOOR:
		*native = BCM2711_QPU_FLOAT_FLOOR;
		*sources = 1;
		break;
	case I915_IR_FTRUNC:
		*native = BCM2711_QPU_FLOAT_TRUNCATE;
		*sources = 1;
		break;
	case I915_IR_FROUND_EVEN:
		*native = BCM2711_QPU_FLOAT_ROUND;
		*sources = 1;
		break;
	case I915_IR_I2F:
		*native = BCM2711_QPU_SIGNED_TO_FLOAT;
		*sources = 1;
		break;
	case I915_IR_U2F:
		*native = BCM2711_QPU_UNSIGNED_TO_FLOAT;
		*sources = 1;
		break;
	case I915_IR_F2I:
		*native = BCM2711_QPU_FLOAT_TO_SIGNED;
		*sources = 1;
		break;
	case I915_IR_F2U:
		*native = BCM2711_QPU_FLOAT_TO_UNSIGNED;
		*sources = 1;
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
	const struct i915_shader_ir_inst *instruction,
	uint32_t destination)
{
	struct bcm2711_qpu_source left;
	struct bcm2711_qpu_source right;
	struct bcm2711_qpu_source temporary;
	struct bcm2711_shader_uniform uniform;
	enum bcm2711_qpu_operation native;
	uint32_t sources;
	uint32_t flags;
	uint32_t predicate;
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
	if (instruction->op == I915_IR_FNEG || instruction->op == I915_IR_FABS) {
		error = bcm2711_shader_operand(compiler, instruction->src[0], BCM2711_SHADER_SCRATCH_LEFT, &left);
		if (error != 0)
			return error;
		kern_memset(&uniform, 0, sizeof(uniform));
		uniform.kind = BCM2711_SHADER_CONSTANT;
		uniform.bits = 0x80000000U;
		native = BCM2711_QPU_BITS_XOR;
		if (instruction->op == I915_IR_FABS) {
			uniform.bits = 0x7fffffffU;
			native = BCM2711_QPU_BITS_AND;
		}

		/* Full-width sign masks are preserved through the independent second scratch register. */
		error = bcm2711_shader_uniform_load(compiler, &uniform, BCM2711_SHADER_SCRATCH_RIGHT);
		if (error != 0)
			return error;
		right = bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_RIGHT);
		error = bcm2711_shader_operation(compiler, native, destination, 0, left, right, 0, 0);
		if (error != 0)
			return error;

		/* This source operation has a complete native encoding. */
		return 0;
	}

	/* Fraction uses the original scalar after the independent floor result is ready. */
	if (instruction->op == I915_IR_FRACT) {
		error = bcm2711_shader_operand(compiler, instruction->src[0], BCM2711_SHADER_SCRATCH_LEFT, &left);
		if (error != 0)
			return error;
		right = bcm2711_shader_bits(0);
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_FLOOR, BCM2711_SHADER_SCRATCH_WORK, 0, left, right, 0, 0);
		if (error != 0)
			return error;
		right = bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_WORK);
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_SUBTRACT, destination, 0, left, right, 0, 0);
		if (error != 0)
			return error;

		/* This source operation has a complete native encoding. */
		return 0;
	}

	/* Native comparison flags preserve ordered NaN semantics rather than approximating a comparison by subtraction. */
	if (instruction->op == I915_IR_FLT ||
	    instruction->op == I915_IR_FGE ||
	    instruction->op == I915_IR_FEQ ||
	    instruction->op == I915_IR_FNEU) {
		error = operands(compiler, instruction->src[0], instruction->src[1], &left, &right);
		if (error != 0)
			return error;
		flags = 1;
		predicate = 1;
		if (instruction->op == I915_IR_FLT) {
			flags = 2;
		} else if (instruction->op == I915_IR_FGE) {
			flags = 3;
			temporary = left;
			left = right;
			right = temporary;
		} else if (instruction->op == I915_IR_FNEU) {
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
	if (instruction->op == I915_IR_SQRT) {
		error = bcm2711_shader_operand(compiler, instruction->src[0], BCM2711_SHADER_SCRATCH_LEFT, &left);
		if (error != 0)
			return error;
		error = force_register(compiler, &left, BCM2711_SHADER_SCRATCH_LEFT);
		if (error != 0)
			return error;
		right = bcm2711_shader_bits(0);
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_INVERSE_ROOT, BCM2711_SHADER_SCRATCH_WORK, 0, left, right, 0, 0);
		if (error != 0)
			return error;
		right = bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_WORK);
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_FLOAT_MULTIPLY, destination, 0, left, right, 0, 0);
		if (error != 0)
			return error;

		/* Both signs of zero retain their original exact bits. */
		kern_memset(&uniform, 0, sizeof(uniform));
		uniform.kind = BCM2711_SHADER_CONSTANT;
		uniform.bits = 0x7fffffffU;
		error = bcm2711_shader_uniform_load(compiler, &uniform, BCM2711_SHADER_SCRATCH_RIGHT);
		if (error != 0)
			return error;
		right = bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_RIGHT);
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_BITS_AND, 6, 1, left, right, 0, 1);
		if (error != 0)
			return error;
		error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, destination, 0, left, right, 1, 0);
		if (error != 0)
			return error;

		/* Positive infinity must also survive the otherwise indeterminate multiply by its zero reciprocal root. */
		uniform.bits = 0x7f800000U;
		error = bcm2711_shader_uniform_load(compiler, &uniform, BCM2711_SHADER_SCRATCH_RIGHT);
		if (error != 0)
			return error;
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
	const struct i915_shader_ir_inst *instruction,
	uint32_t destination)
{
	struct bcm2711_qpu_source condition;
	struct bcm2711_qpu_source selected;
	struct bcm2711_qpu_source ignored;
	int error;

	/* The zero test records lane flags before materialization transactions that preserve those flags. */
	error = bcm2711_shader_operand(compiler, instruction->src[0], BCM2711_SHADER_SCRATCH_WORK, &condition);
	if (error != 0)
		return error;
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_BITS_OR, 6, 1, condition, condition, 0, 1);
	if (error != 0)
		return error;

	/* False lanes take the default bit-preserving source. */
	error = bcm2711_shader_operand(compiler, instruction->src[2], BCM2711_SHADER_SCRATCH_LEFT, &selected);
	if (error != 0)
		return error;
	ignored = bcm2711_shader_bits(0);
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

/* Completes a normalized two-dimensional, four-word TMU lookup before its result values become reusable. */
static int
lower_sample(
	struct bcm2711_shader_compiler *compiler,
	const struct i915_shader_ir_inst *instruction)
{
	struct bcm2711_shader_uniform uniform;
	struct bcm2711_qpu_source coordinate;
	struct bcm2711_qpu_source ignored;
	uint32_t index;
	uint32_t destination;
	int error;

	/* Coordinate T is written before the final S write triggers the lookup. */
	error = bcm2711_shader_operand(compiler, instruction->src[1], BCM2711_SHADER_SCRATCH_LEFT, &coordinate);
	if (error != 0)
		return error;
	ignored = bcm2711_shader_bits(0);
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, 34, 1, coordinate, ignored, 0, 0);
	if (error != 0)
		return error;

	/* Two configuration words bind the checked texture/sampler descriptors and request four float words. */
	kern_memset(&uniform, 0, sizeof(uniform));
	uniform.kind = BCM2711_SHADER_TEXTURE;
	uniform.set = instruction->location;
	uniform.binding = instruction->immediate;
	uniform.bits = 15;
	error = bcm2711_shader_uniform_append(compiler, &uniform);
	if (error != 0)
		return error;
	error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_TEXTURE_CONFIG, 0, 2);
	if (error != 0)
		return error;

	/* Thirty-two-bit normalized sampling is explicit in the second native configuration word. */
	uniform.kind = BCM2711_SHADER_SAMPLER;
	uniform.bits = 1;
	error = bcm2711_shader_uniform_append(compiler, &uniform);
	if (error != 0)
		return error;
	error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_TEXTURE_CONFIG, 0, 2);
	if (error != 0)
		return error;

	/* The final S coordinate write launches exactly one lookup owned by this compiled shader. */
	error = bcm2711_shader_operand(compiler, instruction->src[0], BCM2711_SHADER_SCRATCH_LEFT, &coordinate);
	if (error != 0)
		return error;
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, 33, 1, coordinate, ignored, 0, 0);
	if (error != 0)
		return error;

	/* A regular switch plus its two delay slots permits the other thread to run while the lookup completes. */
	error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_SWITCH, 0, 2);
	if (error != 0)
		return error;

	/* Every requested FIFO result word gets its own independently live physical register. */
	for (index = 0; index < 4; index++) {
		error = bcm2711_shader_register(compiler, &destination);
		if (error != 0)
			return error;
		compiler->values[instruction->dst + index].number = destination;
		compiler->values[instruction->dst + index].register_live = 1;
		error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_TEXTURE, destination, 2);
		if (error != 0)
			return error;
	}

	/* No unconsumed texture result is left in the FIFO for a following shader operation. */
	return 0;
}

/* Materializes two sources without letting distinct small constants compete for the single native constant port. */
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

	/* Two different native constants cannot share read port B and therefore require one explicit register move. */
	if (first->kind == BCM2711_QPU_CONSTANT &&
	    second->kind == BCM2711_QPU_CONSTANT &&
	    first->number != second->number) {
		error = force_register(compiler, first, BCM2711_SHADER_SCRATCH_LEFT);
		if (error != 0)
			return error;
	}

	/* At most one distinct small constant remains in the consuming instruction. */
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
	int error;

	/* An existing physical source already satisfies the requested register contract. */
	if (source->kind == BCM2711_QPU_REGISTER)
		return 0;

	/* A bit-preserving move introduces no floating-point rounding or flag change. */
	ignored = bcm2711_shader_bits(0);
	error = bcm2711_shader_operation(compiler, BCM2711_QPU_MOVE, scratch, 0, *source, ignored, 0, 0);
	if (error != 0)
		return error;
	*source = bcm2711_shader_rf(scratch);

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
			compiler->registers[scalar->number] = 0;
			scalar->register_live = 0;
		}
	}

	/* Succeeded: the consumed scalar values no longer occupy compiler registers. */
	return;
}
