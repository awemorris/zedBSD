/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The encoding of V3D 4.2 QPU instructions.
 */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/compiler/qpu.h"

/*
 * One ALU's native operation fields while a word is assembled; the defaults
 * leave the ALU idle.
 *
 * Values referenced: Mesa 25.0.7 src/broadcom/qpu/qpu_pack.c (the V3D 4.2
 * instruction word's fields and opcodes).
 */

struct qpu_alu {
	uint32_t operation;
	uint32_t left;
	uint32_t right;
	uint32_t address;
	uint32_t peripheral;

	/* How many semantic sources the operation reads; fixed selectors fill the other mux fields. */
	uint32_t sources;

	/* One requires the left mux not above the right; two requires it above (the native min/max and add variants). */
	uint32_t order;

	/* Set for a move on the add ALU: the bit OR of the value with itself reads the one source through both muxes. */
	uint32_t duplicate;
};

/* One temporary word assigns the shared physical read ports before committing a complete 64-bit word. */
struct qpu_encoding {
	struct qpu_alu add;
	struct qpu_alu multiply;
	uint32_t read_a;
	uint32_t read_b;
	uint32_t read_a_used;
	uint32_t read_b_used;
	uint32_t constant;
	uint32_t signal;
	uint32_t conditions;
};

static int instruction_check(const struct bcm2711_qpu_instruction *instruction);
static int operation_fields(const struct bcm2711_qpu_instruction *instruction, enum bcm2711_qpu_unit unit, struct qpu_alu *alu);
static int alu_sources(const struct bcm2711_qpu_instruction *instruction, struct qpu_alu *alu, struct qpu_encoding *encoding);
static int source_fields(const struct bcm2711_qpu_source *source, struct qpu_encoding *encoding, uint32_t *mux);
static int condition_fields(const struct bcm2711_qpu_instruction *add, const struct bcm2711_qpu_instruction *multiply, struct qpu_encoding *encoding);
static int signal_fields(enum bcm2711_qpu_signal signal, const struct bcm2711_qpu_destination *destination, uint32_t thread_switch, struct qpu_encoding *encoding);

/*
 * Encodes one native instruction after validating every operand, signal and flag interaction.
 *
 * The operation runs on its own ALU (a move on the multiply ALU) and the other ALU stays idle.
 */
int
bcm2711_qpu_encode(
	const struct bcm2711_qpu_instruction *instruction,
	uint64_t *word)
{
	struct bcm2711_qpu_bundle bundle;
	enum bcm2711_qpu_unit unit;
	uint64_t packed;
	int error;

	/* Refusal preserves the caller's word, so a partial machine instruction can never be published. */
	if (instruction == NULL || word == NULL)
		return EINVAL;
	error = instruction_check(instruction);
	if (error != 0)
		return error;

	/* An operation the native format does not name is refused before any field is built. */
	unit = bcm2711_qpu_operation_unit(instruction->operation);
	if (instruction->operation != BCM2711_QPU_IDLE && unit == BCM2711_QPU_UNIT_NONE)
		return ENOTSUP;

	/* Idle ALUs cannot meaningfully predicate or update lane flags. */
	if (instruction->operation == BCM2711_QPU_IDLE &&
	    (instruction->predicate != 0 || instruction->push_flags != 0))
		return EINVAL;

	/* The operation takes its own ALU. */
	kern_memset(&bundle, 0, sizeof(bundle));
	if (unit == BCM2711_QPU_UNIT_MULTIPLY || unit == BCM2711_QPU_UNIT_EITHER) {
		bundle.multiply = *instruction;
		bundle.multiply.signal = BCM2711_QPU_SIGNAL_NONE;
	} else {
		bundle.add = *instruction;
		bundle.add.signal = BCM2711_QPU_SIGNAL_NONE;
	}

	/* The instruction's signal becomes the word's signal. */
	bundle.signal = instruction->signal;
	bundle.signal_destination = instruction->signal_destination;

	/* The shared word encoder validates ports, conditions and the signal before publishing anything. */
	error = bcm2711_qpu_encode_bundle(&bundle, &packed);
	if (error != 0)
		return error;
	*word = packed;

	/* Succeeded: the caller owns one complete independently encoded native 4.2 instruction. */
	return 0;
}

/*
 * Encodes one native word carrying an add ALU operation, a multiply ALU operation and one signal.
 *
 * Both operations share the two physical read ports and the one small constant; their predicates and
 * flag pushes share the conditions field, which an addressed signal uses for its destination instead.
 */
int
bcm2711_qpu_encode_bundle(
	const struct bcm2711_qpu_bundle *bundle,
	uint64_t *word)
{
	struct qpu_encoding encoding;
	uint64_t packed;
	int error;

	/* Refusal preserves the caller's word, so a partial machine instruction can never be published. */
	if (bundle == NULL || word == NULL)
		return EINVAL;
	error = instruction_check(&bundle->add);
	if (error != 0)
		return error;
	error = instruction_check(&bundle->multiply);
	if (error != 0)
		return error;

	/* The word's signal is separate; an operation carrying a signal of its own would be lost. */
	if (bundle->add.signal != BCM2711_QPU_SIGNAL_NONE || bundle->multiply.signal != BCM2711_QPU_SIGNAL_NONE)
		return EINVAL;

	/* Both idle ALUs use native no-write destinations; a selected operation replaces its own ALU's. */
	kern_memset(&encoding, 0, sizeof(encoding));
	encoding.add.operation = 187;
	encoding.add.address = 6;
	encoding.add.peripheral = 1;
	encoding.multiply.operation = 15;
	encoding.multiply.right = 4;
	encoding.multiply.address = 6;
	encoding.multiply.peripheral = 1;
	error = operation_fields(&bundle->add, BCM2711_QPU_UNIT_ADD, &encoding.add);
	if (error != 0)
		return error;
	error = operation_fields(&bundle->multiply, BCM2711_QPU_UNIT_MULTIPLY, &encoding.multiply);
	if (error != 0)
		return error;

	/* Actual semantic inputs of both ALUs acquire no more than two physical read ports and one shared small constant. */
	error = alu_sources(&bundle->add, &encoding.add, &encoding);
	if (error != 0)
		return error;
	error = alu_sources(&bundle->multiply, &encoding.multiply, &encoding);
	if (error != 0)
		return error;

	/* Predicates and flag pushes of both ALUs share one field. */
	error = condition_fields(&bundle->add, &bundle->multiply, &encoding);
	if (error != 0)
		return error;

	/* Address-producing signals replace the conditions field and cannot silently discard lane flags. */
	if (bundle->thread_switch > 1)
		return EINVAL;
	error = signal_fields(bundle->signal, &bundle->signal_destination, bundle->thread_switch, &encoding);
	if (error != 0)
		return error;

	/* The fixed instruction format is assembled locally before the caller receives any machine bits. */
	packed = (uint64_t)encoding.multiply.operation << 58;
	packed |= (uint64_t)encoding.signal << 53;
	packed |= (uint64_t)encoding.conditions << 46;
	packed |= (uint64_t)encoding.multiply.peripheral << 45;
	packed |= (uint64_t)encoding.add.peripheral << 44;
	packed |= (uint64_t)encoding.multiply.address << 38;
	packed |= (uint64_t)encoding.add.address << 32;
	packed |= (uint64_t)encoding.add.operation << 24;
	packed |= (uint64_t)encoding.multiply.right << 21;
	packed |= (uint64_t)encoding.multiply.left << 18;
	packed |= (uint64_t)encoding.add.right << 15;
	packed |= (uint64_t)encoding.add.left << 12;
	packed |= (uint64_t)encoding.read_a << 6;
	packed |= encoding.read_b;
	*word = packed;

	/* Succeeded: the caller owns one complete native word issuing both operations and the signal together. */
	return 0;
}

/*
 * Encodes the actual 32-bit bits of a native integer or power-of-two floating small constant.
 */
int
bcm2711_qpu_small_constant(
	uint32_t bits,
	uint32_t *encoded)
{
	uint32_t immediate;

	/* Native small constants contain sixteen positive and sixteen negative integers plus sixteen floats. */
	if (encoded == NULL)
		return EINVAL;
	if (bits <= 15) {
		immediate = bits;
	} else if (bits >= 0xfffffff0U) {
		immediate = 16 + bits - 0xfffffff0U;
	} else if (bits >= 0x3b800000U &&
		   bits <= 0x43000000U &&
		   (bits & 0x007fffffU) == 0) {
		immediate = 32 + ((bits - 0x3b800000U) >> 23);
	} else {
		/* No approximate float or truncated integer may replace a refused full-width constant. */
		return ENOTSUP;
	}

	/* Publication follows complete classification; failed conversion leaves caller storage unchanged. */
	*encoded = immediate;

	/* Succeeded: a native small-constant index preserves every bit of the requested value. */
	return 0;
}

/*
 * Tells which ALU runs one semantic operation.
 */
enum bcm2711_qpu_unit
bcm2711_qpu_operation_unit(
	enum bcm2711_qpu_operation operation)
{
	/* Multiplication and half unpacking exist only on the multiply ALU; a move exists on both. */
	switch (operation) {
	case BCM2711_QPU_IDLE:
		return BCM2711_QPU_UNIT_NONE;
	case BCM2711_QPU_MOVE:
		return BCM2711_QPU_UNIT_EITHER;
	case BCM2711_QPU_FLOAT_MULTIPLY:
	case BCM2711_QPU_HALF_LOW:
	case BCM2711_QPU_HALF_HIGH:
		return BCM2711_QPU_UNIT_MULTIPLY;
	case BCM2711_QPU_PACK_HALVES:
	case BCM2711_QPU_FLOAT_ADD:
	case BCM2711_QPU_FLOAT_SUBTRACT:
	case BCM2711_QPU_FLOAT_MINIMUM:
	case BCM2711_QPU_FLOAT_MAXIMUM:
	case BCM2711_QPU_FLOAT_COMPARE:
	case BCM2711_QPU_INTEGER_ADD:
	case BCM2711_QPU_INTEGER_SUBTRACT:
	case BCM2711_QPU_INTEGER_MINIMUM:
	case BCM2711_QPU_INTEGER_MAXIMUM:
	case BCM2711_QPU_UNSIGNED_MINIMUM:
	case BCM2711_QPU_UNSIGNED_MAXIMUM:
	case BCM2711_QPU_SHIFT_LEFT:
	case BCM2711_QPU_SHIFT_RIGHT:
	case BCM2711_QPU_SHIFT_SIGNED:
	case BCM2711_QPU_BITS_AND:
	case BCM2711_QPU_BITS_OR:
	case BCM2711_QPU_BITS_XOR:
	case BCM2711_QPU_BITS_NOT:
	case BCM2711_QPU_INTEGER_NEGATE:
	case BCM2711_QPU_FLOAT_RECIPROCAL:
	case BCM2711_QPU_FLOAT_ROUND:
	case BCM2711_QPU_FLOAT_TRUNCATE:
	case BCM2711_QPU_FLOAT_FLOOR:
	case BCM2711_QPU_FLOAT_CEILING:
	case BCM2711_QPU_FLOAT_TO_SIGNED:
	case BCM2711_QPU_FLOAT_TO_UNSIGNED:
	case BCM2711_QPU_SIGNED_TO_FLOAT:
	case BCM2711_QPU_UNSIGNED_TO_FLOAT:
	case BCM2711_QPU_DERIVATIVE_X:
	case BCM2711_QPU_DERIVATIVE_Y:
	case BCM2711_QPU_FLOAT_INVERSE_ROOT:
	case BCM2711_QPU_FLOAT_EXPONENT:
	case BCM2711_QPU_FLOAT_LOGARITHM:
	case BCM2711_QPU_FLOAT_SINE:
	case BCM2711_QPU_VPM_LOAD:
	case BCM2711_QPU_VPM_STORE:
	case BCM2711_QPU_TEXTURE_WAIT:
	case BCM2711_QPU_VPM_WAIT:
	case BCM2711_QPU_PIXEL_X:
	case BCM2711_QPU_PIXEL_Y:
	case BCM2711_QPU_INSTANCE_INDEX:
	case BCM2711_QPU_ELEMENT_INDEX:
	case BCM2711_QPU_SAMPLE_MASK:
		return BCM2711_QPU_UNIT_ADD;
	default:
		break;
	}

	/* An unknown operation runs nowhere and is refused by the encoder. */
	return BCM2711_QPU_UNIT_NONE;
}

/*
 * Tells how many semantic sources one operation reads.
 */
uint32_t
bcm2711_qpu_operation_sources(
	enum bcm2711_qpu_operation operation)
{
	/* Fixed selectors replace the operands of the operations that read none or one. */
	switch (operation) {
	case BCM2711_QPU_IDLE:
	case BCM2711_QPU_TEXTURE_WAIT:
	case BCM2711_QPU_VPM_WAIT:
	case BCM2711_QPU_PIXEL_X:
	case BCM2711_QPU_PIXEL_Y:
	case BCM2711_QPU_INSTANCE_INDEX:
	case BCM2711_QPU_ELEMENT_INDEX:
		return 0;
	case BCM2711_QPU_MOVE:
	case BCM2711_QPU_HALF_LOW:
	case BCM2711_QPU_HALF_HIGH:
	case BCM2711_QPU_BITS_NOT:
	case BCM2711_QPU_INTEGER_NEGATE:
	case BCM2711_QPU_FLOAT_RECIPROCAL:
	case BCM2711_QPU_FLOAT_ROUND:
	case BCM2711_QPU_FLOAT_TRUNCATE:
	case BCM2711_QPU_FLOAT_FLOOR:
	case BCM2711_QPU_FLOAT_CEILING:
	case BCM2711_QPU_FLOAT_TO_SIGNED:
	case BCM2711_QPU_FLOAT_TO_UNSIGNED:
	case BCM2711_QPU_SIGNED_TO_FLOAT:
	case BCM2711_QPU_UNSIGNED_TO_FLOAT:
	case BCM2711_QPU_DERIVATIVE_X:
	case BCM2711_QPU_DERIVATIVE_Y:
	case BCM2711_QPU_FLOAT_INVERSE_ROOT:
	case BCM2711_QPU_FLOAT_EXPONENT:
	case BCM2711_QPU_FLOAT_LOGARITHM:
	case BCM2711_QPU_FLOAT_SINE:
	case BCM2711_QPU_VPM_LOAD:
	case BCM2711_QPU_SAMPLE_MASK:
		return 1;
	default:
		break;
	}

	/* Every other operation combines two sources. */
	return 2;
}

/* Refuses an instruction whose destination, predicate or flag push has no native field value. */
static int
instruction_check(
	const struct bcm2711_qpu_instruction *instruction)
{
	/* Destinations have sixty-four selectors and one peripheral bit; four predicates and three pushes exist. */
	if (instruction->destination.number >= 64 ||
	    instruction->destination.peripheral > 1 ||
	    instruction->predicate > 4 ||
	    instruction->push_flags > 3)
		return EINVAL;

	/* One ALU cannot both predicate its write and push new lane flags in the native conditions field. */
	if (instruction->predicate != 0 && instruction->push_flags != 0)
		return ENOTSUP;

	/* Succeeded: every field of the instruction has a native value. */
	return 0;
}

/* Assigns one native operation and its fixed mux selectors to one ALU without copying an upstream encoder table. */
static int
operation_fields(
	const struct bcm2711_qpu_instruction *instruction,
	enum bcm2711_qpu_unit unit,
	struct qpu_alu *alu)
{
	enum bcm2711_qpu_unit runs;

	/* An idle slot keeps the ALU's native no-operation fields and cannot predicate or push flags. */
	if (instruction->operation == BCM2711_QPU_IDLE) {
		if (instruction->predicate != 0 || instruction->push_flags != 0)
			return EINVAL;

		/* The ALU stays idle. */
		return 0;
	}

	/* The operation must exist on the ALU whose slot it occupies. */
	runs = bcm2711_qpu_operation_unit(instruction->operation);
	if (runs == BCM2711_QPU_UNIT_NONE)
		return ENOTSUP;
	if (runs != BCM2711_QPU_UNIT_EITHER && runs != unit)
		return EINVAL;

	/* Each operation names its opcode; fixed selectors occupy the mux fields its sources leave unused. */
	alu->sources = bcm2711_qpu_operation_sources(instruction->operation);
	alu->left = 0;
	alu->right = 0;
	switch (instruction->operation) {
	case BCM2711_QPU_MOVE:
		/* The multiply ALU's move, or on the add ALU the bit OR of the value with itself. */
		if (unit == BCM2711_QPU_UNIT_MULTIPLY) {
			alu->operation = 15;
			alu->right = 7;
		} else {
			alu->operation = 182;
			alu->duplicate = 1;
		}

		/* The move's opcode is chosen. */
		break;
	case BCM2711_QPU_HALF_LOW:
		alu->operation = 14;
		alu->right = 2;
		break;
	case BCM2711_QPU_HALF_HIGH:
		alu->operation = 14;
		alu->right = 3;
		break;
	case BCM2711_QPU_PACK_HALVES:
		/* The left float becomes the low half and the right float the high half, both rounded to sixteen bits. */
		alu->operation = 53;
		break;
	case BCM2711_QPU_FLOAT_ADD:
		alu->operation = 5;
		alu->order = 1;
		break;
	case BCM2711_QPU_FLOAT_SUBTRACT:
		alu->operation = 69;
		break;
	case BCM2711_QPU_FLOAT_MULTIPLY:
		alu->operation = 21;
		break;
	case BCM2711_QPU_FLOAT_MINIMUM:
		alu->operation = 133;
		alu->order = 1;
		break;
	case BCM2711_QPU_FLOAT_MAXIMUM:
		alu->operation = 133;
		alu->order = 2;
		break;
	case BCM2711_QPU_FLOAT_COMPARE:
		alu->operation = 197;
		break;
	case BCM2711_QPU_INTEGER_ADD:
		alu->operation = 56;
		break;
	case BCM2711_QPU_INTEGER_SUBTRACT:
		alu->operation = 60;
		break;
	case BCM2711_QPU_INTEGER_MINIMUM:
		alu->operation = 120;
		break;
	case BCM2711_QPU_INTEGER_MAXIMUM:
		alu->operation = 121;
		break;
	case BCM2711_QPU_UNSIGNED_MINIMUM:
		alu->operation = 122;
		break;
	case BCM2711_QPU_UNSIGNED_MAXIMUM:
		alu->operation = 123;
		break;
	case BCM2711_QPU_SHIFT_LEFT:
		alu->operation = 124;
		break;
	case BCM2711_QPU_SHIFT_RIGHT:
		alu->operation = 125;
		break;
	case BCM2711_QPU_SHIFT_SIGNED:
		alu->operation = 126;
		break;
	case BCM2711_QPU_BITS_AND:
		alu->operation = 181;
		break;
	case BCM2711_QPU_BITS_OR:
		alu->operation = 182;
		break;
	case BCM2711_QPU_BITS_XOR:
		alu->operation = 183;
		break;
	case BCM2711_QPU_BITS_NOT:
		alu->operation = 186;
		break;
	case BCM2711_QPU_INTEGER_NEGATE:
		alu->operation = 186;
		alu->right = 1;
		break;
	case BCM2711_QPU_FLOAT_RECIPROCAL:
		alu->operation = 186;
		alu->right = 5;
		break;
	case BCM2711_QPU_FLOAT_ROUND:
		alu->operation = 245;
		break;
	case BCM2711_QPU_FLOAT_TRUNCATE:
		alu->operation = 245;
		alu->right = 4;
		break;
	case BCM2711_QPU_FLOAT_FLOOR:
		alu->operation = 246;
		break;
	case BCM2711_QPU_FLOAT_CEILING:
		alu->operation = 246;
		alu->right = 4;
		break;
	case BCM2711_QPU_FLOAT_TO_SIGNED:
		alu->operation = 245;
		alu->right = 7;
		break;
	case BCM2711_QPU_FLOAT_TO_UNSIGNED:
		alu->operation = 246;
		alu->right = 3;
		break;
	case BCM2711_QPU_SIGNED_TO_FLOAT:
		alu->operation = 252;
		break;
	case BCM2711_QPU_UNSIGNED_TO_FLOAT:
		alu->operation = 252;
		alu->right = 4;
		break;
	case BCM2711_QPU_DERIVATIVE_X:
		alu->operation = 247;
		break;
	case BCM2711_QPU_DERIVATIVE_Y:
		alu->operation = 247;
		alu->right = 4;
		break;
	case BCM2711_QPU_FLOAT_INVERSE_ROOT:
		alu->operation = 188;
		alu->right = 3;
		break;
	case BCM2711_QPU_FLOAT_EXPONENT:
		alu->operation = 188;
		alu->right = 4;
		break;
	case BCM2711_QPU_FLOAT_LOGARITHM:
		alu->operation = 188;
		alu->right = 5;
		break;
	case BCM2711_QPU_FLOAT_SINE:
		alu->operation = 188;
		alu->right = 6;
		break;
	case BCM2711_QPU_VPM_LOAD:
		alu->operation = 188;
		break;
	case BCM2711_QPU_VPM_STORE:
		alu->operation = 248;
		break;
	case BCM2711_QPU_TEXTURE_WAIT:
		alu->operation = 187;
		alu->left = 5;
		alu->right = 2;
		break;
	case BCM2711_QPU_VPM_WAIT:
		alu->operation = 187;
		alu->left = 6;
		alu->right = 2;
		break;
	case BCM2711_QPU_PIXEL_X:
		alu->operation = 187;
		alu->right = 1;
		break;
	case BCM2711_QPU_PIXEL_Y:
		alu->operation = 187;
		alu->left = 4;
		alu->right = 1;
		break;
	case BCM2711_QPU_INSTANCE_INDEX:
		alu->operation = 187;
		alu->left = 2;
		alu->right = 2;
		break;
	case BCM2711_QPU_ELEMENT_INDEX:
		alu->operation = 187;
		alu->left = 2;
		break;
	case BCM2711_QPU_SAMPLE_MASK:
		/* SETMSF: variant 6 of opcode 186 (Mesa qpu_pack.c, the add table). */
		alu->operation = 186;
		alu->right = 6;
		break;
	default:
		return ENOTSUP;
	}

	/* The ALU owns its explicit per-thread or native peripheral destination. */
	alu->address = instruction->destination.number;
	alu->peripheral = instruction->destination.peripheral;

	/* VPM input loads distinguish input/output reads with the add peripheral bit. */
	if (instruction->operation == BCM2711_QPU_VPM_LOAD && instruction->destination.peripheral != 0)
		return EINVAL;
	if (instruction->operation == BCM2711_QPU_VPM_STORE) {
		/* A vector VPM store encodes its storage layout in the write-address field, not a register. */
		alu->address = 0;
		alu->peripheral = 0;
	}

	/* Succeeded: one supported native operation and its exact selectors are ready on its ALU. */
	return 0;
}

/* Assigns one operation's semantic sources to read ports and orders the muxes its opcode distinguishes. */
static int
alu_sources(
	const struct bcm2711_qpu_instruction *instruction,
	struct qpu_alu *alu,
	struct qpu_encoding *encoding)
{
	uint32_t left;
	uint32_t right;
	uint32_t exchanged;
	int error;

	/* Every fallible input is checked before its mux can influence the encoded operation. */
	left = 0;
	right = 0;
	if (alu->sources >= 1) {
		error = source_fields(&instruction->left, encoding, &left);
		if (error != 0)
			return error;
	}

	/* A second source takes its port after the first. */
	if (alu->sources >= 2) {
		error = source_fields(&instruction->right, encoding, &right);
		if (error != 0)
			return error;
	}

	/* The ordering of equal-unpack commutative operands selects the native min/max or add variant. */
	if (alu->order == 1 && left > right) {
		exchanged = left;
		left = right;
		right = exchanged;
	} else if (alu->order == 2 && left <= right) {
		/* Equal muxes cannot express max; its caller must preserve the value through a separate move. */
		if (left == right)
			return ENOTSUP;
		exchanged = left;
		left = right;
		right = exchanged;
	}

	/* Fixed selectors stay where the operation has no source of its own; a move on the add ALU reads its source twice. */
	if (alu->sources >= 1)
		alu->left = left;
	if (alu->sources >= 2)
		alu->right = right;
	if (alu->duplicate != 0)
		alu->right = left;

	/* Succeeded: the operation's sources are read through native muxes. */
	return 0;
}

/* Assigns one semantic source to an accumulator, a shared read port or the word's one small constant. */
static int
source_fields(
	const struct bcm2711_qpu_source *source,
	struct qpu_encoding *encoding,
	uint32_t *mux)
{
	uint32_t immediate;
	int error;

	/* An accumulator needs no physical read port and has six valid selectors on 4.2. */
	if (source->kind == BCM2711_QPU_ACCUMULATOR) {
		if (source->number >= 6)
			return EINVAL;
		*mux = source->number;
		return 0;
	}

	/* A small constant occupies port B, which another register or a different constant cannot already own. */
	if (source->kind == BCM2711_QPU_CONSTANT) {
		error = bcm2711_qpu_small_constant(source->number, &immediate);
		if (error != 0)
			return error;
		if (encoding->read_b_used != 0 &&
		    (encoding->constant == 0 || encoding->read_b != immediate))
			return ENOTSUP;
		encoding->constant = 1;
		encoding->read_b = immediate;
		encoding->read_b_used = 1;
		*mux = 7;
		return 0;
	}

	/* Native physical registers have sixty-four selectors; no truncation may alias another register. */
	if (source->kind != BCM2711_QPU_REGISTER || source->number >= 64)
		return EINVAL;

	/* A register another source of this word already reads shares that port. */
	if (encoding->read_a_used != 0 && encoding->read_a == source->number) {
		*mux = 6;
		return 0;
	}

	/* Port B is shared only while it holds a register rather than the small constant. */
	if (encoding->read_b_used != 0 &&
	    encoding->constant == 0 &&
	    encoding->read_b == source->number) {
		*mux = 7;
		return 0;
	}

	/* A new register takes the first free port. */
	if (encoding->read_a_used == 0) {
		encoding->read_a = source->number;
		encoding->read_a_used = 1;
		*mux = 6;
	} else if (encoding->read_b_used == 0) {
		encoding->read_b = source->number;
		encoding->read_b_used = 1;
		*mux = 7;
	} else {
		/* One word cannot read a third physical register. */
		return ENOTSUP;
	}

	/* Succeeded: this semantic register is read through one complete native port selector. */
	return 0;
}

/* Combines both ALUs' predicates and flag pushes into the one native conditions field. */
static int
condition_fields(
	const struct bcm2711_qpu_instruction *add,
	const struct bcm2711_qpu_instruction *multiply,
	struct qpu_encoding *encoding)
{
	/* Only one ALU of a word can push new lane flags. */
	if (add->push_flags != 0 && multiply->push_flags != 0)
		return ENOTSUP;

	/*
	 * The field holds both predicates; or one predicate with the other
	 * ALU's push; or one ALU's push or predicate alone.  Predicates one to
	 * four name A, B, not-A and not-B.
	 */
	if (add->predicate != 0 && multiply->predicate != 0) {
		encoding->conditions = 0x40 | ((multiply->predicate - 1) << 4) | (add->predicate - 1);
	} else if (add->predicate != 0) {
		encoding->conditions = 0x20 | ((add->predicate - 1) << 2) | multiply->push_flags;
	} else if (multiply->predicate != 0) {
		encoding->conditions = 0x30 | ((multiply->predicate - 1) << 2) | add->push_flags;
	} else if (add->push_flags != 0) {
		encoding->conditions = add->push_flags;
	} else if (multiply->push_flags != 0) {
		encoding->conditions = 0x10 | multiply->push_flags;
	} else {
		/* Neither ALU is predicated or pushes flags. */
		encoding->conditions = 0;
	}

	/* Succeeded: the conditions field states both ALUs' lane flag use. */
	return 0;
}

/* Encodes the word's one signal and its checked interaction with conditions or small constants. */
static int
signal_fields(
	enum bcm2711_qpu_signal signal,
	const struct bcm2711_qpu_destination *destination,
	uint32_t thread_switch,
	struct qpu_encoding *encoding)
{
	uint32_t addressed;

	/* The signal's destination replaces the native conditions field only for address-producing loads. */
	addressed = 0;
	switch (signal) {
	case BCM2711_QPU_SIGNAL_NONE:
		encoding->signal = 0;
		break;
	case BCM2711_QPU_SIGNAL_SWITCH:
		encoding->signal = 1;
		break;
	case BCM2711_QPU_SIGNAL_UNIFORM:
		encoding->signal = 2;
		break;
	case BCM2711_QPU_SIGNAL_TEXTURE:
		encoding->signal = 4;
		addressed = 1;
		break;
	case BCM2711_QPU_SIGNAL_VARYING:
		encoding->signal = 8;
		addressed = 1;
		break;
	case BCM2711_QPU_SIGNAL_UNIFORM_REGISTER:
		encoding->signal = 12;
		addressed = 1;
		break;
	case BCM2711_QPU_SIGNAL_TILE:
		encoding->signal = 16;
		addressed = 1;
		break;
	case BCM2711_QPU_SIGNAL_TILE_CONFIGURED:
		encoding->signal = 17;
		addressed = 1;
		break;
	case BCM2711_QPU_SIGNAL_TEXTURE_CONFIG:
		encoding->signal = 18;
		break;
	case BCM2711_QPU_SIGNAL_UNIFORM_ADDRESS:
		encoding->signal = 24;
		break;
	case BCM2711_QPU_SIGNAL_UNIFORM_ADDRESS_REGISTER:
		encoding->signal = 25;
		addressed = 1;
		break;
	default:
		return ENOTSUP;
	}

	/* Addressed loads cannot share lane conditions, including a flag push on an otherwise idle ALU. */
	if (addressed != 0) {
		if (encoding->conditions != 0)
			return ENOTSUP;
		if (destination->number >= 64 || destination->peripheral > 1)
			return EINVAL;
		encoding->conditions = destination->number;
		if (destination->peripheral != 0)
			encoding->conditions |= 64;
	}

	/* A thread switch combines with no signal, a uniform load, a texture result read, a varying load or a texture configuration. */
	if (thread_switch != 0) {
		if (encoding->constant != 0)
			return ENOTSUP;
		switch (signal) {
		case BCM2711_QPU_SIGNAL_NONE:
			encoding->signal = 1;
			break;
		case BCM2711_QPU_SIGNAL_UNIFORM:
			encoding->signal = 3;
			break;
		case BCM2711_QPU_SIGNAL_TEXTURE:
			encoding->signal = 5;
			break;
		case BCM2711_QPU_SIGNAL_VARYING:
			encoding->signal = 9;
			break;
		case BCM2711_QPU_SIGNAL_UNIFORM_REGISTER:
			encoding->signal = 13;
			break;
		case BCM2711_QPU_SIGNAL_TEXTURE_CONFIG:
			encoding->signal = 19;
			break;
		default:
			/* No other signal, nor a second switch, combines with a switch. */
			return ENOTSUP;
		}
	}

	/* Small constants use their exact signal encoding; only native varying or TMU reads can combine with it. */
	if (encoding->constant != 0) {
		if (signal == BCM2711_QPU_SIGNAL_NONE) {
			encoding->signal = 15;
		} else if (signal == BCM2711_QPU_SIGNAL_VARYING) {
			encoding->signal = 14;
		} else if (signal == BCM2711_QPU_SIGNAL_TEXTURE) {
			encoding->signal = 31;
		} else {
			/* Unsupported combinations never silently drop a uniform read, switch or texture configuration. */
			return ENOTSUP;
		}
	}

	/* Succeeded: one exact native signal preserves both input interpretation and destination ownership. */
	return 0;
}
