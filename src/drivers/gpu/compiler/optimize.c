/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Optimization of the scalar shader IR before a backend lowers it.
 *
 * The frontend leaves the scalar program much as the SPIR-V module wrote
 * it: a push-constant word is loaded again at each use, a term such as
 * 1 - t is computed once per colour channel, and a value nothing reads is
 * still computed.  This file rewrites a private copy of the program:
 * repeated pure operations are computed once (common subexpressions),
 * operations whose results reach no needed output are removed (dead code),
 * and, when the backend asks, operations on known bits are folded and exact
 * identities removed.
 *
 * Only rewrites that leave every output bit the same are made.  Floating
 * point arithmetic on two constants is not folded (the target's rounding
 * and the kernel's lack of floating point make the result unprovable here),
 * and no rewrite relies on reassociation, on x * 0 being zero (NaN), or on
 * x + 0 being x (negative zero).  The one floating point identity used,
 * x * 1.0, x + -0.0 and x - 0.0 being x, is applied only where every reader
 * of the result is itself a floating point add, subtract or multiply, which
 * reads x through the same input handling the removed operation applied.
 *
 * The pass covers straight-line programs of the pure scalar operations, the
 * loads, the texture sample and the output store; a program with any other
 * operation, a loop or a redefined value is left unchanged for the backend
 * to lower or refuse as written.
 */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/compiler/optimize.h"

/* The largest program optimized; a larger one is left unchanged. */
#define OPTIMIZE_MAX_INSTRUCTIONS 8192U
#define OPTIMIZE_MAX_VALUES 16384U

/* An instruction index or value no table entry names. */
#define OPTIMIZE_ABSENT 0xffffffffU

/* Rounds of rewriting; each round can only remove operations, so a few reach the fixed point. */
#define OPTIMIZE_ROUNDS 8U

/* A value some kept operation reads. */
#define OPTIMIZE_READ 1U

/* A value read by something other than a floating point add, subtract or multiply. */
#define OPTIMIZE_READ_OTHER 2U

/* A value a needed output depends on. */
#define OPTIMIZE_LIVE 4U

/* The bits of the floating point constants the identities name. */
#define OPTIMIZE_FLOAT_ONE 0x3f800000U
#define OPTIMIZE_FLOAT_NEGATIVE_ZERO 0x80000000U
#define OPTIMIZE_FLOAT_SIGN 0x80000000U

/* The Boolean true of the scalar IR. */
#define OPTIMIZE_TRUE 0xffffffffU

/*
 * What one source operation is to the optimizer.
 *
 * A pure operation has no effect besides defining its results, so a copy
 * with the same sources may share them and one whose results nobody reads
 * may go.  A commutative one computes the same bits with its operands
 * exchanged; a floating point add, multiply, minimum or maximum is not
 * counted so, because which of two NaN operands it returns depends on the
 * order.  A program with an operation the table does not cover is left
 * unchanged, so the backend lowers or refuses it with its own diagnostic.
 */
struct optimize_shape {
	uint32_t sources;
	uint32_t results;
	uint32_t pure;
	uint32_t commutative;
};

/*
 * One optimization of one source program.
 *
 * It owns the private copy of the instructions and the per-value tables
 * until the copy is handed to the caller or the optimization is abandoned.
 */
struct optimizer {
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_optimize_options *options;

	/* The rewritten copy; a removed operation becomes a NOP until the copy is compacted. */
	struct drv_gpu_shader_ir_inst *code;
	uint32_t count;

	/*
	 * The value each value is replaced by, itself when it is kept.  A
	 * replacement always names a value defined earlier, so following the
	 * chain ends.
	 */
	uint32_t *replace;

	/* The instruction defining each value, or OPTIMIZE_ABSENT. */
	uint32_t *definition;

	/* OPTIMIZE_READ, OPTIMIZE_READ_OTHER and OPTIMIZE_LIVE of each value. */
	uint32_t *flags;

	/* Open-addressed table of the pure operations seen so far in one pass, by instruction index plus one. */
	uint32_t *table;
	uint32_t table_size;

	/* Set by every rewrite of a round; a round without one ends the optimization. */
	uint32_t changed;
};

static int allocate(struct optimizer *optimizer);
static void release(struct optimizer *optimizer);
static int describe(enum drv_gpu_shader_ir_op operation, struct optimize_shape *shape);
static int check_program(struct optimizer *optimizer);
static uint32_t resolve(const struct optimizer *optimizer, uint32_t value);
static void substitute(struct optimizer *optimizer, struct drv_gpu_shader_ir_inst *instruction);
static void forward(struct optimizer *optimizer);
static void simplify(struct optimizer *optimizer, struct drv_gpu_shader_ir_inst *instruction);
static void simplify_logic(struct optimizer *optimizer, struct drv_gpu_shader_ir_inst *instruction);
static void simplify_integer(struct optimizer *optimizer, struct drv_gpu_shader_ir_inst *instruction);
static void simplify_float(struct optimizer *optimizer, struct drv_gpu_shader_ir_inst *instruction);
static int constant_bits(const struct optimizer *optimizer, uint32_t value, uint32_t *bits);
static void make_copy(struct optimizer *optimizer, struct drv_gpu_shader_ir_inst *instruction, uint32_t value);
static void make_constant(struct optimizer *optimizer, struct drv_gpu_shader_ir_inst *instruction, enum drv_gpu_shader_ir_op operation, uint32_t bits);
static int ordinary_float(uint32_t bits, int64_t *order);
static int integer_float(uint32_t magnitude, uint32_t negative, uint32_t *bits);
static void share(struct optimizer *optimizer, uint32_t index);
static uint32_t operation_class(enum drv_gpu_shader_ir_op operation);
static uint32_t operation_hash(const struct drv_gpu_shader_ir_inst *instruction, const struct optimize_shape *shape);
static int same_operation(const struct drv_gpu_shader_ir_inst *first, const struct drv_gpu_shader_ir_inst *second, const struct optimize_shape *shape);
static void remove_dead(struct optimizer *optimizer);
static int output_needed(const struct optimizer *optimizer, const struct drv_gpu_shader_ir_inst *instruction, uint32_t *stored);
static void remove_identities(struct optimizer *optimizer);
static int compact(struct optimizer *optimizer, struct drv_gpu_shader_ir *optimized);

/*
 * Rewrites a private copy of one parsed program for a backend.
 *
 * On success *optimized is the program's description with its own
 * instruction list, which the caller releases with kern_free() and which
 * shares every other list with ir; when the program is one the pass does
 * not cover, its instruction list is NULL and the caller lowers ir itself.
 * Only an allocation failure is reported.
 */
int
drv_gpu_shader_optimize(
	const struct drv_gpu_shader_ir *ir,
	const struct drv_gpu_shader_optimize_options *options,
	struct drv_gpu_shader_ir *optimized)
{
	struct optimizer optimizer;
	uint32_t round;
	int error;

	/* A program the pass leaves alone is lowered as parsed. */
	*optimized = *ir;
	optimized->instructions = NULL;
	if (ir->instruction_count == 0 ||
	    ir->instruction_count > OPTIMIZE_MAX_INSTRUCTIONS ||
	    ir->value_count == 0 ||
	    ir->value_count > OPTIMIZE_MAX_VALUES)
		return 0;

	/* Describes the optimization of this program for the backend. */
	kern_memset(&optimizer, 0, sizeof(optimizer));
	optimizer.ir = ir;
	optimizer.options = options;
	optimizer.count = ir->instruction_count;

	/* Allocates the copy and the tables; a refusal releases what was allocated. */
	error = allocate(&optimizer);
	if (error != 0) {
		release(&optimizer);
		return error;
	}

	/* Copies the program; a program whose shape the optimizer cannot vouch for stays as it is. */
	kern_memcpy(optimizer.code, ir->instructions, ir->instruction_count * sizeof(*optimizer.code));
	error = check_program(&optimizer);
	if (error != 0) {
		release(&optimizer);
		return 0;
	}

	/* Rewrites the copy until a round changes nothing. */
	for (round = 0; round < OPTIMIZE_ROUNDS; round++) {
		optimizer.changed = 0;
		forward(&optimizer);
		remove_dead(&optimizer);

		/* Removes the identities when the backend asks for simplification. */
		if (options->simplify != 0)
			remove_identities(&optimizer);

		/* A round that changed nothing reached the fixed point. */
		if (optimizer.changed == 0)
			break;
	}

	/* Brings every source up to the last replacements and drops what they left unread. */
	forward(&optimizer);
	remove_dead(&optimizer);

	/* Hands the compacted copy to the caller and releases the rest. */
	error = compact(&optimizer, optimized);
	release(&optimizer);

	/* Reports an allocation failure; the caller has no instruction list to release. */
	if (error != 0)
		return error;

	/* Succeeded: *optimized is the rewritten program, or names no instructions. */
	return 0;
}

/* Allocates the private copy of the instructions and the per-value and sharing tables. */
static int
allocate(
	struct optimizer *optimizer)
{
	const struct drv_gpu_shader_ir *ir;

	/* Allocates the private copy of the instructions. */
	ir = optimizer->ir;
	optimizer->code = kern_calloc(ir->instruction_count, sizeof(*optimizer->code));
	if (optimizer->code == NULL)
		return ENOMEM;

	/* Allocates the replacement of each value. */
	optimizer->replace = kern_calloc(ir->value_count, sizeof(*optimizer->replace));
	if (optimizer->replace == NULL)
		return ENOMEM;

	/* Allocates the defining instruction of each value. */
	optimizer->definition = kern_calloc(ir->value_count, sizeof(*optimizer->definition));
	if (optimizer->definition == NULL)
		return ENOMEM;

	/* Allocates the reader and liveness flags of each value. */
	optimizer->flags = kern_calloc(ir->value_count, sizeof(*optimizer->flags));
	if (optimizer->flags == NULL)
		return ENOMEM;

	/* Sizes the sharing table to a power of two at least twice the operations, so probes stay short. */
	optimizer->table_size = 16;
	while (optimizer->table_size < ir->instruction_count * 2U)
		optimizer->table_size *= 2U;

	/* Allocates the sharing table. */
	optimizer->table = kern_calloc(optimizer->table_size, sizeof(*optimizer->table));
	if (optimizer->table == NULL)
		return ENOMEM;

	/* Succeeded: every table exists. */
	return 0;
}

/* Releases the copy and the tables an optimization allocated; a table never allocated is NULL. */
static void
release(
	struct optimizer *optimizer)
{
	/* Each table is released on its own; the caller's list, if any, was allocated apart. */
	kern_free(optimizer->table);
	kern_free(optimizer->flags);
	kern_free(optimizer->definition);
	kern_free(optimizer->replace);
	kern_free(optimizer->code);

	/* Succeeded: the optimization owns no storage. */
	return;
}

/* Tells the optimizer's view of one operation the pass covers. */
static int
describe(
	enum drv_gpu_shader_ir_op operation,
	struct optimize_shape *shape)
{
	/* Most operations are pure and define one value from their sources. */
	shape->sources = 0;
	shape->results = 1;
	shape->pure = 1;
	shape->commutative = 0;

	/* Classifies the operation by what it reads and defines. */
	switch (operation) {
	case DRV_GPU_IR_NOP:
	case DRV_GPU_IR_SKIP_END:
		shape->results = 0;
		shape->pure = 0;
		break;
	case DRV_GPU_IR_STORE_OUTPUT:
	case DRV_GPU_IR_SKIP_BEGIN:
		shape->sources = 1;
		shape->results = 0;
		shape->pure = 0;
		break;
	case DRV_GPU_IR_CONST:
	case DRV_GPU_IR_BOOL:
	case DRV_GPU_IR_ICONST:
	case DRV_GPU_IR_LOAD_INPUT:
	case DRV_GPU_IR_LOAD_PUSH:
	case DRV_GPU_IR_LOAD_UBO:
		break;
	case DRV_GPU_IR_SAMPLE:
		shape->sources = 2;
		shape->results = 4;
		break;
	case DRV_GPU_IR_SELECT:
		shape->sources = 3;
		break;
	case DRV_GPU_IR_FEQ:
	case DRV_GPU_IR_FNEU:
	case DRV_GPU_IR_AND:
	case DRV_GPU_IR_OR:
	case DRV_GPU_IR_IADD:
	case DRV_GPU_IR_IAND:
	case DRV_GPU_IR_IOR:
	case DRV_GPU_IR_IXOR:
		shape->sources = 2;
		shape->commutative = 1;
		break;
	case DRV_GPU_IR_FADD:
	case DRV_GPU_IR_FSUB:
	case DRV_GPU_IR_FMUL:
	case DRV_GPU_IR_FMIN:
	case DRV_GPU_IR_FMAX:
	case DRV_GPU_IR_FLT:
	case DRV_GPU_IR_FGE:
	case DRV_GPU_IR_ISUB:
	case DRV_GPU_IR_SHL:
	case DRV_GPU_IR_SHR:
	case DRV_GPU_IR_ASR:
		shape->sources = 2;
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
		shape->sources = 1;
		break;
	default:
		return ENOTSUP;
	}

	/* Succeeded: the shape describes the operation. */
	return 0;
}

/*
 * Checks that the copy is a straight-line SSA program of known operations.
 *
 * The backend checks its program again after the optimization; this check
 * only keeps the optimizer from reading outside its tables, merging values
 * a later instruction redefines, or moving work across a skippable block
 * the backend may skip.
 */
static int
check_program(
	struct optimizer *optimizer)
{
	struct drv_gpu_shader_ir_inst *instruction;
	struct optimize_shape shape;
	uint32_t index;
	uint32_t source;
	uint32_t result;
	uint32_t value;
	int error;

	/* Every value starts undefined and kept as itself. */
	for (value = 0; value < optimizer->ir->value_count; value++) {
		optimizer->definition[value] = OPTIMIZE_ABSENT;
		optimizer->replace[value] = value;
	}

	/* Checks each operation's sources against earlier definitions and records its results. */
	for (index = 0; index < optimizer->count; index++) {
		instruction = &optimizer->code[index];
		error = describe(instruction->op, &shape);
		if (error != 0)
			return error;

		/* A source must name a value an earlier operation defined. */
		for (source = 0; source < shape.sources; source++) {
			value = instruction->src[source];
			if (value >= optimizer->ir->value_count)
				return EINVAL;
			if (optimizer->definition[value] == OPTIMIZE_ABSENT)
				return EINVAL;
		}

		/* A skippable block stays whole unless the backend runs every block. */
		if (instruction->op == DRV_GPU_IR_SKIP_BEGIN || instruction->op == DRV_GPU_IR_SKIP_END) {
			if (optimizer->options->runs_every_block == 0)
				return ENOTSUP;
		}

		/*
		 * A texture guard only names the channels whose result is used; a
		 * backend that samples for every channel ignores it, so the copy
		 * drops it, and another backend's program stays unchanged.
		 */
		if (instruction->guard != 0) {
			if (instruction->op != DRV_GPU_IR_SAMPLE)
				return EINVAL;
			if (optimizer->options->runs_every_block == 0)
				return ENOTSUP;
			instruction->guard = 0;
		}

		/* Each result is defined exactly once. */
		if (shape.results != 0) {
			if (instruction->dst >= optimizer->ir->value_count)
				return EINVAL;
			if (shape.results > optimizer->ir->value_count - instruction->dst)
				return EINVAL;
		}

		/* Records the operation as the definition of each result. */
		for (result = 0; result < shape.results; result++) {
			value = instruction->dst + result;
			if (optimizer->definition[value] != OPTIMIZE_ABSENT)
				return EINVAL;
			optimizer->definition[value] = index;
		}
	}

	/* Succeeded: the copy can be rewritten. */
	return 0;
}

/* Tells the value that stands for a value after the replacements so far. */
static uint32_t
resolve(
	const struct optimizer *optimizer,
	uint32_t value)
{
	/* Follows the chain toward earlier definitions until a kept value. */
	while (optimizer->replace[value] != value)
		value = optimizer->replace[value];

	/* Reports the kept value. */
	return value;
}

/* Makes an operation read the kept values in place of replaced ones. */
static void
substitute(
	struct optimizer *optimizer,
	struct drv_gpu_shader_ir_inst *instruction)
{
	struct optimize_shape shape;
	uint32_t source;
	int error;

	/* The copy holds only operations check_program() described. */
	error = describe(instruction->op, &shape);
	if (error != 0)
		return;

	/* Rewrites each source the operation reads. */
	for (source = 0; source < shape.sources; source++)
		instruction->src[source] = resolve(optimizer, instruction->src[source]);
}

/*
 * Passes once over the program in source order.
 *
 * Each operation first reads the kept values, then, when the backend asks,
 * is folded or simplified, and finally shares the results of an earlier
 * operation that computes the same.
 */
static void
forward(
	struct optimizer *optimizer)
{
	struct drv_gpu_shader_ir_inst *instruction;
	uint32_t index;

	/* A pass starts with no operation to share. */
	kern_memset(optimizer->table, 0, optimizer->table_size * sizeof(*optimizer->table));

	/* Visits each operation after every operation it reads. */
	for (index = 0; index < optimizer->count; index++) {
		instruction = &optimizer->code[index];
		if (instruction->op == DRV_GPU_IR_NOP)
			continue;
		substitute(optimizer, instruction);

		/* Folds and simplifies when the backend asks for it. */
		if (optimizer->options->simplify != 0)
			simplify(optimizer, instruction);

		/* A simplification may have removed the operation. */
		if (instruction->op == DRV_GPU_IR_NOP)
			continue;
		share(optimizer, index);
	}
}

/*
 * Folds or simplifies one operation whose results are bit-exact.
 *
 * Each rule leaves the result's bits as the operation defines them.
 */
static void
simplify(
	struct optimizer *optimizer,
	struct drv_gpu_shader_ir_inst *instruction)
{
	/* Chooses the rules by the kind of operation. */
	switch (instruction->op) {
	case DRV_GPU_IR_NOT:
	case DRV_GPU_IR_INOT:
	case DRV_GPU_IR_AND:
	case DRV_GPU_IR_IAND:
	case DRV_GPU_IR_OR:
	case DRV_GPU_IR_IOR:
	case DRV_GPU_IR_IXOR:
	case DRV_GPU_IR_SELECT:
	case DRV_GPU_IR_MOVE:
		simplify_logic(optimizer, instruction);
		break;
	case DRV_GPU_IR_IADD:
	case DRV_GPU_IR_ISUB:
	case DRV_GPU_IR_INEG:
	case DRV_GPU_IR_SHL:
	case DRV_GPU_IR_SHR:
	case DRV_GPU_IR_ASR:
	case DRV_GPU_IR_I2F:
	case DRV_GPU_IR_U2F:
		simplify_integer(optimizer, instruction);
		break;
	case DRV_GPU_IR_FNEG:
	case DRV_GPU_IR_FABS:
	case DRV_GPU_IR_FMIN:
	case DRV_GPU_IR_FMAX:
	case DRV_GPU_IR_FLT:
	case DRV_GPU_IR_FGE:
	case DRV_GPU_IR_FEQ:
	case DRV_GPU_IR_FNEU:
		simplify_float(optimizer, instruction);
		break;
	default:
		break;
	}
}

/* Folds and simplifies the bitwise operations, Boolean logic and selections. */
static void
simplify_logic(
	struct optimizer *optimizer,
	struct drv_gpu_shader_ir_inst *instruction)
{
	const struct drv_gpu_shader_ir_inst *definition;
	uint32_t left;
	uint32_t right;
	uint32_t condition;
	int left_known;
	int right_known;
	int condition_known;

	/* Reads which sources have known bits. */
	left_known = constant_bits(optimizer, instruction->src[0], &left);
	right_known = constant_bits(optimizer, instruction->src[1], &right);

	/* A move copies the bits of its source. */
	if (instruction->op == DRV_GPU_IR_MOVE) {
		make_copy(optimizer, instruction, instruction->src[0]);
		return;
	}

	/* A complement of known bits is known; a complement of a complement is the original bits. */
	definition = &optimizer->code[optimizer->definition[instruction->src[0]]];
	if (instruction->op == DRV_GPU_IR_NOT || instruction->op == DRV_GPU_IR_INOT) {
		if (left_known != 0) {
			make_constant(optimizer, instruction, DRV_GPU_IR_ICONST, ~left);
		} else if (definition->op == instruction->op) {
			make_copy(optimizer, instruction, definition->src[0]);
		}

		/* No other rule applies to this operation. */
		return;
	}

	/* A selection by a known condition, or between equal values, is the value selected. */
	if (instruction->op == DRV_GPU_IR_SELECT) {
		condition_known = left_known;
		condition = left;
		if (instruction->src[1] == instruction->src[2]) {
			make_copy(optimizer, instruction, instruction->src[1]);
		} else if (condition_known != 0 && condition != 0) {
			make_copy(optimizer, instruction, instruction->src[1]);
		} else if (condition_known != 0) {
			make_copy(optimizer, instruction, instruction->src[2]);
		}

		/* No other rule applies to this operation. */
		return;
	}

	/* Two known operands give known bits. */
	if (left_known != 0 && right_known != 0) {
		if (instruction->op == DRV_GPU_IR_IXOR) {
			make_constant(optimizer, instruction, DRV_GPU_IR_ICONST, left ^ right);
		} else if (instruction->op == DRV_GPU_IR_AND || instruction->op == DRV_GPU_IR_IAND) {
			make_constant(optimizer, instruction, DRV_GPU_IR_ICONST, left & right);
		} else {
			make_constant(optimizer, instruction, DRV_GPU_IR_ICONST, left | right);
		}

		/* No other rule applies to this operation. */
		return;
	}

	/* An exclusive or with zero keeps the other operand. */
	if (instruction->op == DRV_GPU_IR_IXOR) {
		if (left_known != 0 && left == 0) {
			make_copy(optimizer, instruction, instruction->src[1]);
		} else if (right_known != 0 && right == 0) {
			make_copy(optimizer, instruction, instruction->src[0]);
		} else if (instruction->src[0] == instruction->src[1]) {
			make_constant(optimizer, instruction, DRV_GPU_IR_ICONST, 0);
		}

		/* No other rule applies to this operation. */
		return;
	}

	/* An and or an or of a value with itself is the value. */
	if (instruction->src[0] == instruction->src[1]) {
		make_copy(optimizer, instruction, instruction->src[0]);
		return;
	}

	/* An and keeps the other operand under all ones and is zero under zero. */
	if (instruction->op == DRV_GPU_IR_AND || instruction->op == DRV_GPU_IR_IAND) {
		if (left_known != 0 && left == OPTIMIZE_TRUE) {
			make_copy(optimizer, instruction, instruction->src[1]);
		} else if (right_known != 0 && right == OPTIMIZE_TRUE) {
			make_copy(optimizer, instruction, instruction->src[0]);
		} else if (left_known != 0 && left == 0) {
			make_constant(optimizer, instruction, DRV_GPU_IR_ICONST, 0);
		} else if (right_known != 0 && right == 0) {
			make_constant(optimizer, instruction, DRV_GPU_IR_ICONST, 0);
		}

		/* No other rule applies to this operation. */
		return;
	}

	/* An or keeps the other operand under zero and is all ones under all ones. */
	if (left_known != 0 && left == 0) {
		make_copy(optimizer, instruction, instruction->src[1]);
	} else if (right_known != 0 && right == 0) {
		make_copy(optimizer, instruction, instruction->src[0]);
	} else if (left_known != 0 && left == OPTIMIZE_TRUE) {
		make_constant(optimizer, instruction, DRV_GPU_IR_ICONST, OPTIMIZE_TRUE);
	} else if (right_known != 0 && right == OPTIMIZE_TRUE) {
		make_constant(optimizer, instruction, DRV_GPU_IR_ICONST, OPTIMIZE_TRUE);
	}
}

/* Folds and simplifies the integer arithmetic, shifts and exact integer conversions. */
static void
simplify_integer(
	struct optimizer *optimizer,
	struct drv_gpu_shader_ir_inst *instruction)
{
	uint32_t left;
	uint32_t right;
	uint32_t shift;
	uint32_t bits;
	int left_known;
	int right_known;
	int error;

	/* Reads which sources have known bits. */
	left_known = constant_bits(optimizer, instruction->src[0], &left);
	right_known = constant_bits(optimizer, instruction->src[1], &right);

	/* A negation of known bits is known modulo two to the thirty-second. */
	if (instruction->op == DRV_GPU_IR_INEG) {
		if (left_known != 0)
			make_constant(optimizer, instruction, DRV_GPU_IR_ICONST, 0U - left);
		return;
	}

	/* A known integer converts exactly when its significant bits fit a float's 24. */
	if (instruction->op == DRV_GPU_IR_I2F || instruction->op == DRV_GPU_IR_U2F) {
		if (left_known == 0)
			return;
		if (instruction->op == DRV_GPU_IR_I2F && (left & OPTIMIZE_FLOAT_SIGN) != 0) {
			error = integer_float(0U - left, 1, &bits);
		} else {
			error = integer_float(left, 0, &bits);
		}

		/* An exact conversion becomes its float. */
		if (error == 0)
			make_constant(optimizer, instruction, DRV_GPU_IR_CONST, bits);
		return;
	}

	/* An addition of zero keeps the other operand; two known operands are known. */
	if (instruction->op == DRV_GPU_IR_IADD) {
		if (left_known != 0 && right_known != 0) {
			make_constant(optimizer, instruction, DRV_GPU_IR_ICONST, left + right);
		} else if (left_known != 0 && left == 0) {
			make_copy(optimizer, instruction, instruction->src[1]);
		} else if (right_known != 0 && right == 0) {
			make_copy(optimizer, instruction, instruction->src[0]);
		}

		/* No other rule applies to this operation. */
		return;
	}

	/* A subtraction of zero keeps the first operand, and of itself is zero. */
	if (instruction->op == DRV_GPU_IR_ISUB) {
		if (left_known != 0 && right_known != 0) {
			make_constant(optimizer, instruction, DRV_GPU_IR_ICONST, left - right);
		} else if (right_known != 0 && right == 0) {
			make_copy(optimizer, instruction, instruction->src[0]);
		} else if (instruction->src[0] == instruction->src[1]) {
			make_constant(optimizer, instruction, DRV_GPU_IR_ICONST, 0);
		}

		/* No other rule applies to this operation. */
		return;
	}

	/* A shift counts modulo 32; a count of zero keeps the operand. */
	if (right_known == 0)
		return;
	shift = right & 31U;
	if (shift == 0) {
		make_copy(optimizer, instruction, instruction->src[0]);
		return;
	}

	/* Shifts known bits left, right with zeros, or right with copies of the sign bit. */
	if (left_known == 0)
		return;
	if (instruction->op == DRV_GPU_IR_SHL) {
		bits = left << shift;
	} else if (instruction->op == DRV_GPU_IR_SHR) {
		bits = left >> shift;
	} else if ((left & OPTIMIZE_FLOAT_SIGN) != 0) {
		bits = ~(~left >> shift);
	} else {
		bits = left >> shift;
	}

	/* The shifted bits are the result. */
	make_constant(optimizer, instruction, DRV_GPU_IR_ICONST, bits);
}

/*
 * Folds the floating point operations whose result is exact.
 *
 * Negation and absolute value change only the sign bit, which is how they
 * are lowered.  The minimum, the maximum and the comparisons are folded only
 * for ordinary operands (no NaN, no denormal, not two zeros for a minimum or
 * a maximum), whose order is plain.
 */
static void
simplify_float(
	struct optimizer *optimizer,
	struct drv_gpu_shader_ir_inst *instruction)
{
	const struct drv_gpu_shader_ir_inst *definition;
	uint32_t left;
	uint32_t right;
	uint32_t truth;
	int64_t left_order;
	int64_t right_order;
	int left_known;
	int right_known;
	int left_ordinary;
	int right_ordinary;

	/* Reads which sources have known bits, and the operation defining the first. */
	left_known = constant_bits(optimizer, instruction->src[0], &left);
	right_known = constant_bits(optimizer, instruction->src[1], &right);
	definition = &optimizer->code[optimizer->definition[instruction->src[0]]];

	/* A negation flips the sign bit, and a second negation restores it. */
	if (instruction->op == DRV_GPU_IR_FNEG) {
		if (left_known != 0) {
			make_constant(optimizer, instruction, DRV_GPU_IR_CONST, left ^ OPTIMIZE_FLOAT_SIGN);
		} else if (definition->op == DRV_GPU_IR_FNEG) {
			make_copy(optimizer, instruction, definition->src[0]);
		}

		/* No other rule applies to this operation. */
		return;
	}

	/* An absolute value clears the sign bit, whatever an earlier negation or absolute value left in it. */
	if (instruction->op == DRV_GPU_IR_FABS) {
		if (left_known != 0) {
			make_constant(optimizer, instruction, DRV_GPU_IR_CONST, left & ~OPTIMIZE_FLOAT_SIGN);
		} else if (definition->op == DRV_GPU_IR_FABS) {
			make_copy(optimizer, instruction, instruction->src[0]);
		} else if (definition->op == DRV_GPU_IR_FNEG) {
			instruction->src[0] = definition->src[0];
			optimizer->changed = 1;
		}

		/* No other rule applies to this operation. */
		return;
	}

	/* The remaining folds need two ordinary known operands. */
	if (left_known == 0 || right_known == 0)
		return;
	left_ordinary = ordinary_float(left, &left_order);
	right_ordinary = ordinary_float(right, &right_order);
	if (left_ordinary == 0 || right_ordinary == 0)
		return;

	/* A minimum or a maximum is the operand on its side; two zeros differ only in a sign the order does not choose. */
	if (instruction->op == DRV_GPU_IR_FMIN || instruction->op == DRV_GPU_IR_FMAX) {
		if (left_order == 0 && right_order == 0)
			return;
		if (instruction->op == DRV_GPU_IR_FMIN && left_order <= right_order) {
			make_constant(optimizer, instruction, DRV_GPU_IR_CONST, left);
		} else if (instruction->op == DRV_GPU_IR_FMIN) {
			make_constant(optimizer, instruction, DRV_GPU_IR_CONST, right);
		} else if (left_order >= right_order) {
			make_constant(optimizer, instruction, DRV_GPU_IR_CONST, left);
		} else {
			make_constant(optimizer, instruction, DRV_GPU_IR_CONST, right);
		}

		/* No other rule applies to this operation. */
		return;
	}

	/* A comparison of ordered operands is a Boolean of their order. */
	truth = 0;
	if (instruction->op == DRV_GPU_IR_FLT && left_order < right_order) {
		truth = OPTIMIZE_TRUE;
	} else if (instruction->op == DRV_GPU_IR_FGE && left_order >= right_order) {
		truth = OPTIMIZE_TRUE;
	} else if (instruction->op == DRV_GPU_IR_FEQ && left_order == right_order) {
		truth = OPTIMIZE_TRUE;
	} else if (instruction->op == DRV_GPU_IR_FNEU && left_order != right_order) {
		truth = OPTIMIZE_TRUE;
	}

	/* The Boolean is the result. */
	make_constant(optimizer, instruction, DRV_GPU_IR_BOOL, truth);
}

/* Tells whether a value is a constant, and its bits. */
static int
constant_bits(
	const struct optimizer *optimizer,
	uint32_t value,
	uint32_t *bits)
{
	const struct drv_gpu_shader_ir_inst *definition;

	/* An unused source field names no definition. */
	*bits = 0;
	if (value >= optimizer->ir->value_count)
		return 0;
	if (optimizer->definition[value] == OPTIMIZE_ABSENT)
		return 0;

	/* The three constant operations all define exact bits. */
	definition = &optimizer->code[optimizer->definition[value]];
	if (definition->op != DRV_GPU_IR_CONST &&
	    definition->op != DRV_GPU_IR_ICONST &&
	    definition->op != DRV_GPU_IR_BOOL)
		return 0;

	/* Succeeded: the value is these bits. */
	*bits = definition->immediate;
	return 1;
}

/* Removes an operation whose result is an existing value. */
static void
make_copy(
	struct optimizer *optimizer,
	struct drv_gpu_shader_ir_inst *instruction,
	uint32_t value)
{
	/* Later readers take the existing value; the operation leaves nothing to lower. */
	optimizer->replace[instruction->dst] = value;
	instruction->op = DRV_GPU_IR_NOP;
	optimizer->changed = 1;
}

/* Turns an operation into the constant of its known result. */
static void
make_constant(
	struct optimizer *optimizer,
	struct drv_gpu_shader_ir_inst *instruction,
	enum drv_gpu_shader_ir_op operation,
	uint32_t bits)
{
	/* The constant keeps the destination and reads nothing. */
	instruction->op = operation;
	instruction->immediate = bits;
	instruction->src[0] = 0;
	instruction->src[1] = 0;
	instruction->src[2] = 0;
	instruction->src[3] = 0;
	instruction->location = 0;
	instruction->component = 0;
	optimizer->changed = 1;
}

/*
 * Tells whether a float is ordinary and gives its place in the order.
 *
 * NaNs and denormals are not ordinary: a NaN has no order, and how the
 * target operations treat a denormal is not something this file folds.
 * Both zeros take place zero.
 */
static int
ordinary_float(
	uint32_t bits,
	int64_t *order)
{
	uint32_t exponent;
	uint32_t magnitude;

	/* Splits the exponent field and the magnitude. */
	exponent = (bits >> 23) & 0xffU;
	magnitude = bits & ~OPTIMIZE_FLOAT_SIGN;
	*order = 0;

	/* Refuses a NaN and a denormal. */
	if (exponent == 0xffU && magnitude != 0x7f800000U)
		return 0;
	if (exponent == 0 && magnitude != 0)
		return 0;

	/* Orders by magnitude, below zero for a negative sign. */
	*order = (int64_t)magnitude;
	if ((bits & OPTIMIZE_FLOAT_SIGN) != 0)
		*order = -(int64_t)magnitude;

	/* Succeeded: the float is ordinary. */
	return 1;
}

/* Converts an integer magnitude to float bits when the conversion is exact. */
static int
integer_float(
	uint32_t magnitude,
	uint32_t negative,
	uint32_t *bits)
{
	uint32_t top;
	uint32_t fraction;

	/* Zero converts to positive zero. */
	*bits = 0;
	if (magnitude == 0)
		return 0;

	/* Finds the most significant set bit. */
	top = 31;
	while ((magnitude >> top) == 0)
		top--;

	/* Refuses a magnitude with significant bits below a float's 24. */
	if (top > 23 && (magnitude & ((1U << (top - 23)) - 1U)) != 0)
		return EINVAL;

	/* Places the bits below the leading one in the fraction field. */
	if (top > 23) {
		fraction = magnitude >> (top - 23);
	} else {
		fraction = magnitude << (23 - top);
	}

	/* Packs the exponent of the leading one, the fraction and the sign. */
	*bits = ((127U + top) << 23) | (fraction & 0x7fffffU);
	if (negative != 0)
		*bits |= OPTIMIZE_FLOAT_SIGN;

	/* Succeeded: the float is the integer exactly. */
	return 0;
}

/*
 * Shares the results of an earlier operation that computes the same.
 *
 * Pure operations of one class with the same meaningful fields and the
 * same sources (in either order for a commutative one) compute the same
 * bits.  The first such operation of a pass is kept and the later ones read
 * its results.
 */
static void
share(
	struct optimizer *optimizer,
	uint32_t index)
{
	struct drv_gpu_shader_ir_inst *instruction;
	struct drv_gpu_shader_ir_inst *earlier;
	struct optimize_shape shape;
	uint32_t slot;
	uint32_t entry;
	uint32_t result;
	int same;
	int error;

	/* Only a pure operation with results can be shared. */
	instruction = &optimizer->code[index];
	error = describe(instruction->op, &shape);
	if (error != 0)
		return;
	if (shape.pure == 0 || shape.results == 0)
		return;

	/* Probes the table from the operation's hash. */
	slot = operation_hash(instruction, &shape) & (optimizer->table_size - 1U);
	while (optimizer->table[slot] != 0) {
		entry = optimizer->table[slot] - 1U;
		earlier = &optimizer->code[entry];
		same = same_operation(earlier, instruction, &shape);

		/*
		 * An earlier operation computing the same takes over every
		 * result, kept apart when either was (NoContraction).
		 */
		if (earlier->op != DRV_GPU_IR_NOP && same != 0) {
			for (result = 0; result < shape.results; result++)
				optimizer->replace[instruction->dst + result] = earlier->dst + result;
			earlier->no_contraction |= instruction->no_contraction;
			instruction->op = DRV_GPU_IR_NOP;
			optimizer->changed = 1;
			return;
		}

		/* Probes the next slot. */
		slot = (slot + 1U) & (optimizer->table_size - 1U);
	}

	/* The operation is the first of its kind in this pass. */
	optimizer->table[slot] = index + 1U;
}

/* Tells the class operations share results within: the three constant operations are one class. */
static uint32_t
operation_class(
	enum drv_gpu_shader_ir_op operation)
{
	/* Constants of every kind are bits alike. */
	if (operation == DRV_GPU_IR_ICONST || operation == DRV_GPU_IR_BOOL)
		return DRV_GPU_IR_CONST;

	/* Every other operation is its own class. */
	return operation;
}

/* Hashes the fields that decide what an operation computes. */
static uint32_t
operation_hash(
	const struct drv_gpu_shader_ir_inst *instruction,
	const struct optimize_shape *shape)
{
	uint32_t hash;
	uint32_t first;
	uint32_t second;

	/* Starts from the class and the immediate fields. */
	hash = operation_class(instruction->op) * 0x9e3779b1U;
	hash ^= instruction->immediate * 0x85ebca6bU;
	hash ^= instruction->location * 0xc2b2ae35U;
	hash ^= instruction->component * 0x27d4eb2fU;

	/* Mixes the sources in an order a commutative operation's operands cannot change. */
	first = instruction->src[0];
	second = instruction->src[1];
	if (shape->commutative != 0 && second < first) {
		first = instruction->src[1];
		second = instruction->src[0];
	}

	/* Mixes as many sources as the operation reads. */
	if (shape->sources > 0)
		hash = (hash ^ first) * 0x165667b1U;
	if (shape->sources > 1)
		hash = (hash ^ second) * 0x165667b1U;
	if (shape->sources > 2)
		hash = (hash ^ instruction->src[2]) * 0x165667b1U;

	/* Spreads the high bits into the low ones the table indexes by. */
	hash ^= hash >> 15;

	/* Reports the hash. */
	return hash;
}

/*
 * Tells whether two pure operations compute the same bits.
 *
 * The constant, load and sample operations are told apart by the fields
 * that name their bits or their source; the others by their sources.
 */
static int
same_operation(
	const struct drv_gpu_shader_ir_inst *first,
	const struct drv_gpu_shader_ir_inst *second,
	const struct optimize_shape *shape)
{
	uint32_t source;
	uint32_t straight;
	uint32_t first_class;
	uint32_t second_class;

	/* Operations of different classes compute different things. */
	first_class = operation_class(first->op);
	second_class = operation_class(second->op);
	if (first_class != second_class)
		return 0;

	/* The fields that name a constant, a load or a texture must agree. */
	if (first->immediate != second->immediate)
		return 0;
	if (first->location != second->location)
		return 0;
	if (first->component != second->component)
		return 0;

	/* Compares the sources in order. */
	straight = 1;
	for (source = 0; source < shape->sources; source++) {
		if (first->src[source] != second->src[source])
			straight = 0;
	}

	/* Sources agreeing in order give the same operation. */
	if (straight != 0)
		return 1;

	/* A commutative operation also agrees with its operands exchanged. */
	if (shape->commutative == 0)
		return 0;
	if (first->src[0] != second->src[1])
		return 0;
	if (first->src[1] != second->src[0])
		return 0;

	/* Succeeded: the operands are the same pair. */
	return 1;
}

/*
 * Removes operations whose results reach no needed output.
 *
 * The program is visited backward: an output store the backend needs is
 * kept, and an operation is kept when a kept operation reads one of its
 * results.  Skip markers, which reach here only when the backend runs every
 * block, describe nothing it uses and are removed.
 */
static void
remove_dead(
	struct optimizer *optimizer)
{
	struct drv_gpu_shader_ir_inst *instruction;
	struct optimize_shape shape;
	uint32_t stored[DRV_GPU_IR_OPTIMIZE_SLOTS];
	uint32_t index;
	uint32_t source;
	uint32_t result;
	uint32_t value;
	uint32_t keep;
	int error;

	/* No value is known live and no output slot stored yet. */
	for (value = 0; value < optimizer->ir->value_count; value++)
		optimizer->flags[value] &= ~OPTIMIZE_LIVE;
	kern_memset(stored, 0, sizeof(stored));

	/* Visits each operation after every operation that reads it. */
	index = optimizer->count;
	while (index > 0) {
		index--;
		instruction = &optimizer->code[index];
		error = describe(instruction->op, &shape);
		if (error != 0)
			continue;

		/* Decides whether the operation is kept. */
		keep = 0;
		if (instruction->op == DRV_GPU_IR_STORE_OUTPUT) {
			keep = (uint32_t)output_needed(optimizer, instruction, stored);
		} else if (shape.pure != 0) {
			for (result = 0; result < shape.results; result++) {
				if ((optimizer->flags[instruction->dst + result] & OPTIMIZE_LIVE) != 0)
					keep = 1;
			}
		}

		/* A dropped operation leaves nothing to lower. */
		if (keep == 0) {
			if (instruction->op != DRV_GPU_IR_NOP) {
				instruction->op = DRV_GPU_IR_NOP;
				optimizer->changed = 1;
			}

			/* Nothing it read is kept for it. */
			continue;
		}

		/* What a kept operation reads is live. */
		for (source = 0; source < shape.sources; source++)
			optimizer->flags[instruction->src[source]] |= OPTIMIZE_LIVE;
	}
}

/*
 * Tells whether the backend needs one output store.
 *
 * A store is not needed when a later store replaces its slot, or when the
 * backend's consumer does not read the slot.  A store to a location outside
 * the slots (a builtin, or a slot the backend may refuse) is kept as it is.
 * stored marks the slots a later store already supplies.
 */
static int
output_needed(
	const struct optimizer *optimizer,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t *stored)
{
	uint32_t slot;

	/* Finds the store's slot; a store to no slot is kept. */
	if (instruction->component >= 4)
		return 1;
	if (instruction->location == DRV_GPU_IR_LOCATION_POSITION) {
		slot = DRV_GPU_IR_OPTIMIZE_POSITION + instruction->component;
	} else if (instruction->location < 16) {
		slot = instruction->location * 4U + instruction->component;
	} else {
		return 1;
	}

	/* A later store to the same slot supplies the output instead. */
	if (stored[slot] != 0)
		return 0;
	stored[slot] = 1;

	/* The last store to a slot is needed when the consumer reads the slot. */
	if (optimizer->options->needed[slot] == 0)
		return 0;

	/* Succeeded: the store supplies a needed output. */
	return 1;
}

/*
 * Removes floating point identities whose readers cannot tell the difference.
 *
 * x * 1.0, x + -0.0 and x - 0.0 are x in exact arithmetic, but a target
 * operation may also flush a denormal x to zero and quiet a signalling NaN.
 * A floating point add, subtract or multiply reading the result applies the
 * same input handling to x itself, so when those are the only readers, the
 * operation can go; any other reader (an output, a comparison, a bit move,
 * a texture coordinate, a special function) keeps it.
 */
static void
remove_identities(
	struct optimizer *optimizer)
{
	struct drv_gpu_shader_ir_inst *instruction;
	struct optimize_shape shape;
	uint32_t index;
	uint32_t source;
	uint32_t value;
	uint32_t left;
	uint32_t right;
	int left_known;
	int right_known;
	int error;

	/* Collects how each value is read by the kept operations. */
	for (value = 0; value < optimizer->ir->value_count; value++)
		optimizer->flags[value] &= ~(OPTIMIZE_READ | OPTIMIZE_READ_OTHER);
	for (index = 0; index < optimizer->count; index++) {
		instruction = &optimizer->code[index];
		error = describe(instruction->op, &shape);
		if (error != 0)
			continue;
		for (source = 0; source < shape.sources; source++) {
			value = instruction->src[source];
			optimizer->flags[value] |= OPTIMIZE_READ;
			if (instruction->op != DRV_GPU_IR_FADD &&
			    instruction->op != DRV_GPU_IR_FSUB &&
			    instruction->op != DRV_GPU_IR_FMUL)
				optimizer->flags[value] |= OPTIMIZE_READ_OTHER;
		}
	}

	/* Removes each identity whose result only floating point arithmetic reads. */
	for (index = 0; index < optimizer->count; index++) {
		instruction = &optimizer->code[index];
		if (instruction->op != DRV_GPU_IR_FADD &&
		    instruction->op != DRV_GPU_IR_FSUB &&
		    instruction->op != DRV_GPU_IR_FMUL)
			continue;
		if ((optimizer->flags[instruction->dst] & OPTIMIZE_READ_OTHER) != 0)
			continue;

		/* Reads which operands have known bits. */
		left_known = constant_bits(optimizer, instruction->src[0], &left);
		right_known = constant_bits(optimizer, instruction->src[1], &right);

		/* A product with one, a sum with negative zero and a difference of positive zero keep the other operand. */
		if (instruction->op == DRV_GPU_IR_FMUL && right_known != 0 && right == OPTIMIZE_FLOAT_ONE) {
			make_copy(optimizer, instruction, instruction->src[0]);
		} else if (instruction->op == DRV_GPU_IR_FMUL && left_known != 0 && left == OPTIMIZE_FLOAT_ONE) {
			make_copy(optimizer, instruction, instruction->src[1]);
		} else if (instruction->op == DRV_GPU_IR_FADD && right_known != 0 && right == OPTIMIZE_FLOAT_NEGATIVE_ZERO) {
			make_copy(optimizer, instruction, instruction->src[0]);
		} else if (instruction->op == DRV_GPU_IR_FADD && left_known != 0 && left == OPTIMIZE_FLOAT_NEGATIVE_ZERO) {
			make_copy(optimizer, instruction, instruction->src[1]);
		} else if (instruction->op == DRV_GPU_IR_FSUB && right_known != 0 && right == 0) {
			make_copy(optimizer, instruction, instruction->src[0]);
		}
	}
}

/* Moves the kept operations into the caller's own instruction list. */
static int
compact(
	struct optimizer *optimizer,
	struct drv_gpu_shader_ir *optimized)
{
	struct drv_gpu_shader_ir_inst *kept;
	uint32_t index;
	uint32_t count;

	/* Counts the kept operations; a program left with none is lowered from its source, which the analysis refuses. */
	count = 0;
	for (index = 0; index < optimizer->count; index++) {
		if (optimizer->code[index].op != DRV_GPU_IR_NOP)
			count++;
	}

	/* Leaves the caller's list empty when nothing is kept. */
	if (count == 0)
		return 0;

	/* Allocates the caller's list. */
	kept = kern_calloc(count, sizeof(*kept));
	if (kept == NULL)
		return ENOMEM;

	/* Copies the kept operations in source order. */
	count = 0;
	for (index = 0; index < optimizer->count; index++) {
		if (optimizer->code[index].op == DRV_GPU_IR_NOP)
			continue;
		kept[count] = optimizer->code[index];
		count++;
	}

	/* Publishes the list; every other field still describes the source program. */
	optimized->instructions = kept;
	optimized->instruction_count = count;

	/* Succeeded: the caller owns the list. */
	return 0;
}
