/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The encoding of V3D 7.1 QPU instructions, for the Raspberry Pi 5's BCM2712.
 *
 * This file implements the semantic instruction interface of qpu.h for the
 * 7.1 instruction word.  The board's source list selects it in place of
 * qpu.c, which encodes the same interface for the Pi 4's V3D 4.2.
 *
 * A 7.1 word has no accumulators: every input of both ALUs names a register
 * of the physical file or a small immediate, and each of the four inputs has
 * a read address of its own, so the two ALUs never compete for read ports.
 * One small immediate may replace one of those read addresses, and a word
 * that reads one carries no other signal.
 */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/compiler/qpu.h"

/*
 * The positions of the fields in the 64-bit instruction word.
 *
 * Values referenced: Mesa 25.3.6 src/broadcom/qpu/qpu_pack.c lines 46-110
 * (the field shifts; the read address fields C and D replace the 4.2 input
 * multiplexers on 7.1).
 */
#define QPU71_MULTIPLY_OPERATION_SHIFT 58
#define QPU71_SIGNAL_SHIFT 53
#define QPU71_CONDITIONS_SHIFT 46
#define QPU71_MULTIPLY_MAGIC_SHIFT 45
#define QPU71_ADD_MAGIC_SHIFT 44
#define QPU71_MULTIPLY_ADDRESS_SHIFT 38
#define QPU71_ADD_ADDRESS_SHIFT 32
#define QPU71_ADD_OPERATION_SHIFT 24
#define QPU71_READ_C_SHIFT 18
#define QPU71_READ_D_SHIFT 12
#define QPU71_READ_A_SHIFT 6
#define QPU71_READ_B_SHIFT 0

/*
 * The magic write address that discards a result, and the bit of the
 * conditions field that makes an addressed signal's destination magic.
 *
 * Values referenced: Mesa 25.3.6 src/broadcom/qpu/qpu_instr.h line 101 (the
 * discarding write address) and qpu_pack.c line 55 (the signal's magic bit).
 */
#define QPU71_MAGIC_DISCARD 6
#define QPU71_SIGNAL_MAGIC 64

/*
 * The opcodes of the idle ALUs: the add ALU's no-operation selects variant
 * zero of opcode 187, the multiply ALU's selects variant 63 of opcode 14.
 *
 * Values referenced: Mesa 25.3.6 src/broadcom/qpu/qpu_pack.c lines 623 and
 * 783 (the v3d71_add_ops NOP and v3d71_mul_ops NOP rows).
 */
#define QPU71_ADD_IDLE 187
#define QPU71_ADD_IDLE_VARIANT 0
#define QPU71_MULTIPLY_IDLE 14
#define QPU71_MULTIPLY_IDLE_VARIANT 63

/*
 * The signal field values of the small immediate on each read address.
 *
 * Values referenced: Mesa 25.3.6 src/broadcom/qpu/qpu_pack.c lines 164-197
 * (v3d71_sig_map rows 14, 15, 30 and 31).
 */
#define QPU71_SIGNAL_CONSTANT_A 14
#define QPU71_SIGNAL_CONSTANT_B 15
#define QPU71_SIGNAL_CONSTANT_C 30
#define QPU71_SIGNAL_CONSTANT_D 31

/* The four read addresses of a word: the add ALU's a and b, the multiply ALU's a and b. */
enum qpu71_port {
	QPU71_PORT_A,
	QPU71_PORT_B,
	QPU71_PORT_C,
	QPU71_PORT_D
};

/*
 * One ALU's native fields while a word is assembled.
 *
 * The defaults leave the ALU idle.  The left input is read through the
 * ALU's first read address (a on the add ALU, c on the multiply ALU) and the
 * right input through its second (b or d); an operation with fewer than two
 * inputs keeps a fixed variant selector in the second one.
 */
struct qpu71_alu {
	uint32_t operation;
	uint32_t left;
	uint32_t right;
	uint32_t address;
	uint32_t magic;

	/* How many semantic sources the operation reads. */
	uint32_t sources;

	/*
	 * One requires the left input's order key not above the right's, two
	 * requires it above: the native float add, minimum and maximum are told
	 * apart from their twins by the order of their inputs.
	 */
	uint32_t order;
};

/*
 * One temporary word assembled completely before any machine bits reach the caller.
 *
 * Every input has its own read address, so the only shared resource of the
 * two ALUs is the one small immediate, recorded with the address it replaces.
 */
struct qpu71_encoding {
	struct qpu71_alu add;
	struct qpu71_alu multiply;
	uint32_t constants;
	enum qpu71_port constant_port;
	uint32_t signal;
	uint32_t conditions;
};

/* One semantic source resolved into the read address it occupies. */
struct qpu71_read {
	uint32_t address;
	uint32_t constant;
};

static int instruction_check(const struct bcm2711_qpu_instruction *instruction);
static int magic_check(uint32_t number);
static int operation_fields(const struct bcm2711_qpu_instruction *instruction, enum bcm2711_qpu_unit unit, struct qpu71_alu *alu);
static int add_operation(enum bcm2711_qpu_operation operation, struct qpu71_alu *alu);
static int multiply_operation(enum bcm2711_qpu_operation operation, struct qpu71_alu *alu);
static int alu_sources(const struct bcm2711_qpu_instruction *instruction, struct qpu71_alu *alu, enum qpu71_port first_port, struct qpu71_encoding *encoding);
static int read_field(const struct bcm2711_qpu_source *source, struct qpu71_read *read);
static uint32_t read_key(const struct qpu71_read *read);
static int note_constant(struct qpu71_encoding *encoding, enum qpu71_port port);
static int condition_fields(const struct bcm2711_qpu_instruction *add, const struct bcm2711_qpu_instruction *multiply, struct qpu71_encoding *encoding);
static int signal_fields(enum bcm2711_qpu_signal signal, const struct bcm2711_qpu_destination *destination, uint32_t thread_switch, struct qpu71_encoding *encoding);
static int switch_signal(enum bcm2711_qpu_signal signal, uint32_t *encoded);
static int constant_signal(enum qpu71_port port, uint32_t *encoded);

/*
 * Encodes one native instruction after validating every operand, signal and flag interaction.
 *
 * The operation runs on the multiply ALU when only that ALU has it, and on
 * the add ALU otherwise; the other ALU stays idle.
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
	if (instruction == NULL)
		return EINVAL;
	if (word == NULL)
		return EINVAL;

	/* Every field of the instruction must have a native value. */
	error = instruction_check(instruction);
	if (error != 0)
		return error;

	/* An operation the native format does not name is refused before any field is built. */
	unit = bcm2711_qpu_operation_unit(instruction->operation);
	if (instruction->operation != BCM2711_QPU_IDLE && unit == BCM2711_QPU_UNIT_NONE)
		return ENOTSUP;

	/* Idle ALUs cannot meaningfully predicate a write. */
	if (instruction->operation == BCM2711_QPU_IDLE && instruction->predicate != 0)
		return EINVAL;

	/* Idle ALUs cannot push lane flags either. */
	if (instruction->operation == BCM2711_QPU_IDLE && instruction->push_flags != 0)
		return EINVAL;

	/* The operation takes the multiply ALU only when the add ALU lacks it. */
	kern_memset(&bundle, 0, sizeof(bundle));
	if (unit == BCM2711_QPU_UNIT_MULTIPLY) {
		bundle.multiply = *instruction;
		bundle.multiply.signal = BCM2711_QPU_SIGNAL_NONE;
	} else {
		bundle.add = *instruction;
		bundle.add.signal = BCM2711_QPU_SIGNAL_NONE;
	}

	/* The instruction's signal becomes the word's signal. */
	bundle.signal = instruction->signal;
	bundle.signal_destination = instruction->signal_destination;

	/* The shared word encoder validates inputs, conditions and the signal before publishing anything. */
	error = bcm2711_qpu_encode_bundle(&bundle, &packed);
	if (error != 0)
		return error;

	/* The complete word reaches the caller. */
	*word = packed;

	/* Succeeded: the caller owns one complete independently encoded native 7.1 instruction. */
	return 0;
}

/*
 * Encodes one native word carrying an add ALU operation, a multiply ALU operation and one signal.
 *
 * Each ALU reads its inputs through its own two read addresses.  The word
 * reads at most one small immediate; the predicates and flag pushes of both
 * ALUs share the conditions field, which an addressed signal uses for its
 * destination instead.
 */
int
bcm2711_qpu_encode_bundle(
	const struct bcm2711_qpu_bundle *bundle,
	uint64_t *word)
{
	struct qpu71_encoding encoding;
	uint64_t packed;
	int error;

	/* Refusal preserves the caller's word, so a partial machine instruction can never be published. */
	if (bundle == NULL)
		return EINVAL;
	if (word == NULL)
		return EINVAL;

	/* The add ALU's instruction must have native field values. */
	error = instruction_check(&bundle->add);
	if (error != 0)
		return error;

	/* So must the multiply ALU's. */
	error = instruction_check(&bundle->multiply);
	if (error != 0)
		return error;

	/* The word's signal is separate; an operation carrying a signal of its own would be lost. */
	if (bundle->add.signal != BCM2711_QPU_SIGNAL_NONE)
		return EINVAL;
	if (bundle->multiply.signal != BCM2711_QPU_SIGNAL_NONE)
		return EINVAL;

	/*
	 * Both ALUs start idle, writing to the discarding magic address; a
	 * selected operation replaces its own ALU's fields.
	 */
	kern_memset(&encoding, 0, sizeof(encoding));
	encoding.add.operation = QPU71_ADD_IDLE;
	encoding.add.right = QPU71_ADD_IDLE_VARIANT;
	encoding.add.address = QPU71_MAGIC_DISCARD;
	encoding.add.magic = 1;
	encoding.multiply.operation = QPU71_MULTIPLY_IDLE;
	encoding.multiply.right = QPU71_MULTIPLY_IDLE_VARIANT;
	encoding.multiply.address = QPU71_MAGIC_DISCARD;
	encoding.multiply.magic = 1;

	/* The add ALU's operation takes its opcode and variant. */
	error = operation_fields(&bundle->add, BCM2711_QPU_UNIT_ADD, &encoding.add);
	if (error != 0)
		return error;

	/* So does the multiply ALU's. */
	error = operation_fields(&bundle->multiply, BCM2711_QPU_UNIT_MULTIPLY, &encoding.multiply);
	if (error != 0)
		return error;

	/* The add ALU reads its inputs through read addresses a and b. */
	error = alu_sources(&bundle->add, &encoding.add, QPU71_PORT_A, &encoding);
	if (error != 0)
		return error;

	/* The multiply ALU reads its inputs through read addresses c and d. */
	error = alu_sources(&bundle->multiply, &encoding.multiply, QPU71_PORT_C, &encoding);
	if (error != 0)
		return error;

	/* Predicates and flag pushes of both ALUs share one field. */
	error = condition_fields(&bundle->add, &bundle->multiply, &encoding);
	if (error != 0)
		return error;

	/* A word carries at most one thread switch. */
	if (bundle->thread_switch > 1)
		return EINVAL;

	/* The signal field states the word's signal, switch and small immediate together. */
	error = signal_fields(bundle->signal, &bundle->signal_destination, bundle->thread_switch, &encoding);
	if (error != 0)
		return error;

	/* The fixed instruction format is assembled locally before the caller receives any machine bits. */
	packed = (uint64_t)encoding.multiply.operation << QPU71_MULTIPLY_OPERATION_SHIFT;
	packed |= (uint64_t)encoding.signal << QPU71_SIGNAL_SHIFT;
	packed |= (uint64_t)encoding.conditions << QPU71_CONDITIONS_SHIFT;
	packed |= (uint64_t)encoding.multiply.magic << QPU71_MULTIPLY_MAGIC_SHIFT;
	packed |= (uint64_t)encoding.add.magic << QPU71_ADD_MAGIC_SHIFT;
	packed |= (uint64_t)encoding.multiply.address << QPU71_MULTIPLY_ADDRESS_SHIFT;
	packed |= (uint64_t)encoding.add.address << QPU71_ADD_ADDRESS_SHIFT;
	packed |= (uint64_t)encoding.add.operation << QPU71_ADD_OPERATION_SHIFT;
	packed |= (uint64_t)encoding.multiply.left << QPU71_READ_C_SHIFT;
	packed |= (uint64_t)encoding.multiply.right << QPU71_READ_D_SHIFT;
	packed |= (uint64_t)encoding.add.left << QPU71_READ_A_SHIFT;
	packed |= (uint64_t)encoding.add.right << QPU71_READ_B_SHIFT;

	/* The complete word reaches the caller. */
	*word = packed;

	/* Succeeded: the caller owns one complete native word issuing both operations and the signal together. */
	return 0;
}

/*
 * Encodes the actual 32-bit bits of a native integer or power-of-two floating small constant.
 *
 * Values referenced: Mesa 25.3.6 src/broadcom/qpu/qpu_pack.c lines 239-264
 * (the small_immediates table: sixteen positive and sixteen negative
 * integers, then the powers of two from 2^-8 to 2^7).  The table is the
 * same on V3D 4.2 and 7.1.
 */
int
bcm2711_qpu_small_constant(
	uint32_t bits,
	uint32_t *encoded)
{
	uint32_t immediate;

	/* Without storage for the index nothing can be reported. */
	if (encoded == NULL)
		return EINVAL;

	/* Classifies the bits as one of the three runs of the table. */
	if (bits <= 15) {
		/* The integers zero to fifteen index themselves. */
		immediate = bits;
	} else if (bits >= 0xfffffff0U) {
		/* The integers minus sixteen to minus one follow them. */
		immediate = 16 + bits - 0xfffffff0U;
	} else if (bits >= 0x3b800000U &&
		   bits <= 0x43000000U &&
		   (bits & 0x007fffffU) == 0) {
		/* The powers of two have no mantissa bits and consecutive exponents. */
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
 * Tells which ALU runs one semantic operation on V3D 7.1.
 *
 * Moves, half-float unpacks and integer addition and subtraction exist on
 * both ALUs; multiplication only on the multiply ALU; everything else,
 * including the special functions, only on the add ALU.
 */
enum bcm2711_qpu_unit
bcm2711_qpu_operation_unit(
	enum bcm2711_qpu_operation operation)
{
	/* Each semantic operation names the ALUs that have it. */
	switch (operation) {
	case BCM2711_QPU_IDLE:
		return BCM2711_QPU_UNIT_NONE;
	case BCM2711_QPU_MOVE:
	case BCM2711_QPU_HALF_LOW:
	case BCM2711_QPU_HALF_HIGH:
	case BCM2711_QPU_INTEGER_ADD:
	case BCM2711_QPU_INTEGER_SUBTRACT:
		return BCM2711_QPU_UNIT_EITHER;
	case BCM2711_QPU_FLOAT_MULTIPLY:
		return BCM2711_QPU_UNIT_MULTIPLY;
	case BCM2711_QPU_PACK_HALVES:
	case BCM2711_QPU_FLOAT_ADD:
	case BCM2711_QPU_FLOAT_SUBTRACT:
	case BCM2711_QPU_FLOAT_MINIMUM:
	case BCM2711_QPU_FLOAT_MAXIMUM:
	case BCM2711_QPU_FLOAT_COMPARE:
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
	/* Fixed variant selectors replace the inputs of the operations that read none or one. */
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
	int error;

	/* Destinations have sixty-four selectors and one magic bit. */
	if (instruction->destination.number >= 64)
		return EINVAL;
	if (instruction->destination.peripheral > 1)
		return EINVAL;

	/* Four predicates and three flag pushes exist. */
	if (instruction->predicate > 4)
		return EINVAL;
	if (instruction->push_flags > 3)
		return EINVAL;

	/* One ALU cannot both predicate its write and push new lane flags in the native conditions field. */
	if (instruction->predicate != 0 && instruction->push_flags != 0)
		return ENOTSUP;

	/* A magic destination must be one the 7.1 word still names. */
	if (instruction->destination.peripheral != 0) {
		error = magic_check(instruction->destination.number);
		if (error != 0)
			return error;
	}

	/* Succeeded: every field of the instruction has a native value. */
	return 0;
}

/*
 * Refuses a magic write address that V3D 7.1 reserves.
 *
 * Values referenced: Mesa 25.3.6 src/broadcom/qpu/qpu_instr.h lines 94-137
 * (addresses zero to four, the 4.2 accumulators, and nineteen to
 * twenty-four, the 4.2 special-function addresses, are reserved on 7.x).
 */
static int
magic_check(
	uint32_t number)
{
	/* The accumulators of 4.2 do not exist. */
	if (number <= 4)
		return EINVAL;

	/* The special functions are add ALU operations instead of write addresses. */
	if (number >= 19 && number <= 24)
		return EINVAL;

	/* Succeeded: the magic address exists on 7.1. */
	return 0;
}

/* Assigns one native operation and its fixed variant selector to one ALU. */
static int
operation_fields(
	const struct bcm2711_qpu_instruction *instruction,
	enum bcm2711_qpu_unit unit,
	struct qpu71_alu *alu)
{
	enum bcm2711_qpu_unit runs;
	int error;

	/* An idle slot keeps the ALU's native no-operation fields. */
	if (instruction->operation == BCM2711_QPU_IDLE) {
		/* An idle ALU cannot predicate a write or push flags. */
		if (instruction->predicate != 0)
			return EINVAL;
		if (instruction->push_flags != 0)
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

	/* Each operation names its opcode; the variant selector fills the second read address of the narrower ones. */
	alu->sources = bcm2711_qpu_operation_sources(instruction->operation);
	alu->left = 0;
	alu->right = 0;
	alu->order = 0;
	if (unit == BCM2711_QPU_UNIT_ADD) {
		error = add_operation(instruction->operation, alu);
	} else {
		error = multiply_operation(instruction->operation, alu);
	}

	/* An operation without a native form on this ALU is refused. */
	if (error != 0)
		return error;

	/* The ALU owns its explicit per-thread or magic destination. */
	alu->address = instruction->destination.number;
	alu->magic = instruction->destination.peripheral;

	/* A VPM input load writes a register of the thread. */
	if (instruction->operation == BCM2711_QPU_VPM_LOAD && instruction->destination.peripheral != 0)
		return EINVAL;

	/*
	 * A VPM store's write address field selects the store's layout, zero
	 * for a vector of the VPM, and is not a register; its magic bit stays
	 * clear (Mesa 25.3.6 qpu_pack.c lines 1799-1810).
	 */
	if (instruction->operation == BCM2711_QPU_VPM_STORE) {
		alu->address = 0;
		alu->magic = 0;
	}

	/* Succeeded: one supported native operation and its exact selector are ready on its ALU. */
	return 0;
}

/*
 * Chooses the add ALU's opcode and variant selector for one operation.
 *
 * Values referenced: Mesa 25.3.6 src/broadcom/qpu/qpu_pack.c lines 585-745
 * (v3d71_add_ops) and lines 1772-2041 (v3d71_qpu_add_pack: a float
 * operation's opcode carries the output pack in bits 4-5 and the input
 * unpacks in bits 2-3 and 0-1, where one means no unpack; a one-input
 * operation's variant selector carries the output pack in bits 0-1 and the
 * input unpack in bits 2-3).  Every operation here uses no pack and no
 * unpack except the half-float unpacks.
 */
static int
add_operation(
	enum bcm2711_qpu_operation operation,
	struct qpu71_alu *alu)
{
	/* Each case names the opcode and, for a narrow operation, its variant. */
	switch (operation) {
	case BCM2711_QPU_MOVE:
		/* The integer move without unpack. */
		alu->operation = 249;
		alu->right = 3;
		break;
	case BCM2711_QPU_HALF_LOW:
		/* The float move unpacking the low half float. */
		alu->operation = 249;
		alu->right = 8;
		break;
	case BCM2711_QPU_HALF_HIGH:
		/* The float move unpacking the high half float. */
		alu->operation = 249;
		alu->right = 12;
		break;
	case BCM2711_QPU_PACK_HALVES:
		/* The left float becomes the low half and the right float the high half. */
		alu->operation = 53;
		break;
	case BCM2711_QPU_FLOAT_ADD:
		alu->operation = 5;
		alu->order = 1;
		break;
	case BCM2711_QPU_FLOAT_SUBTRACT:
		alu->operation = 69;
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
		alu->right = 0;
		break;
	case BCM2711_QPU_INTEGER_NEGATE:
		alu->operation = 186;
		alu->right = 1;
		break;
	case BCM2711_QPU_FLOAT_RECIPROCAL:
		/* The special functions are variants 32 to 37 of opcode 188 on 7.1. */
		alu->operation = 188;
		alu->right = 32;
		break;
	case BCM2711_QPU_FLOAT_INVERSE_ROOT:
		alu->operation = 188;
		alu->right = 33;
		break;
	case BCM2711_QPU_FLOAT_EXPONENT:
		alu->operation = 188;
		alu->right = 34;
		break;
	case BCM2711_QPU_FLOAT_LOGARITHM:
		alu->operation = 188;
		alu->right = 35;
		break;
	case BCM2711_QPU_FLOAT_SINE:
		alu->operation = 188;
		alu->right = 36;
		break;
	case BCM2711_QPU_FLOAT_ROUND:
		alu->operation = 245;
		alu->right = 4;
		break;
	case BCM2711_QPU_FLOAT_TRUNCATE:
		alu->operation = 245;
		alu->right = 20;
		break;
	case BCM2711_QPU_FLOAT_FLOOR:
		alu->operation = 245;
		alu->right = 36;
		break;
	case BCM2711_QPU_FLOAT_CEILING:
		alu->operation = 245;
		alu->right = 52;
		break;
	case BCM2711_QPU_FLOAT_TO_SIGNED:
		/* The conversion toward zero. */
		alu->operation = 245;
		alu->right = 23;
		break;
	case BCM2711_QPU_FLOAT_TO_UNSIGNED:
		/* The unsigned conversion toward zero. */
		alu->operation = 245;
		alu->right = 39;
		break;
	case BCM2711_QPU_SIGNED_TO_FLOAT:
		alu->operation = 246;
		alu->right = 32;
		break;
	case BCM2711_QPU_UNSIGNED_TO_FLOAT:
		alu->operation = 246;
		alu->right = 36;
		break;
	case BCM2711_QPU_DERIVATIVE_X:
		alu->operation = 246;
		alu->right = 4;
		break;
	case BCM2711_QPU_DERIVATIVE_Y:
		alu->operation = 246;
		alu->right = 20;
		break;
	case BCM2711_QPU_VPM_LOAD:
		/* The VPM vector input load. */
		alu->operation = 188;
		alu->right = 0;
		break;
	case BCM2711_QPU_VPM_STORE:
		/* The VPM vector store; its write address field selects the vector layout. */
		alu->operation = 190;
		break;
	case BCM2711_QPU_TEXTURE_WAIT:
		alu->operation = 187;
		alu->right = 15;
		break;
	case BCM2711_QPU_VPM_WAIT:
		alu->operation = 187;
		alu->right = 16;
		break;
	case BCM2711_QPU_PIXEL_X:
		/* The pixel's X coordinate as a float. */
		alu->operation = 187;
		alu->right = 32;
		break;
	case BCM2711_QPU_PIXEL_Y:
		/* The pixel's Y coordinate as a float. */
		alu->operation = 187;
		alu->right = 36;
		break;
	case BCM2711_QPU_INSTANCE_INDEX:
		alu->operation = 187;
		alu->right = 12;
		break;
	case BCM2711_QPU_ELEMENT_INDEX:
		alu->operation = 187;
		alu->right = 2;
		break;
	case BCM2711_QPU_SAMPLE_MASK:
		/* SETMSF: variant 6 of opcode 186 (Mesa qpu_pack.c, the 7.1 add table). */
		alu->operation = 186;
		alu->right = 6;
		break;
	default:
		return ENOTSUP;
	}

	/* Succeeded: the add ALU's opcode and variant are chosen. */
	return 0;
}

/*
 * Chooses the multiply ALU's opcode and variant selector for one operation.
 *
 * Values referenced: Mesa 25.3.6 src/broadcom/qpu/qpu_pack.c lines 747-790
 * (v3d71_mul_ops) and lines 2160-2285 (v3d71_qpu_mul_pack: the float
 * multiply's opcode is 16 plus the output pack in bits 4-5 and the input
 * unpacks in bits 2-3 and 0-1; a move's variant selector carries its unpack
 * in bits 2-3 and its pack in bits 0-1).
 */
static int
multiply_operation(
	enum bcm2711_qpu_operation operation,
	struct qpu71_alu *alu)
{
	/* Each case names the opcode and, for a move, its variant. */
	switch (operation) {
	case BCM2711_QPU_MOVE:
		/* The integer move without unpack. */
		alu->operation = 14;
		alu->right = 3;
		break;
	case BCM2711_QPU_HALF_LOW:
		/* The float move unpacking the low half float. */
		alu->operation = 14;
		alu->right = 8;
		break;
	case BCM2711_QPU_HALF_HIGH:
		/* The float move unpacking the high half float. */
		alu->operation = 14;
		alu->right = 12;
		break;
	case BCM2711_QPU_FLOAT_MULTIPLY:
		alu->operation = 21;
		break;
	case BCM2711_QPU_INTEGER_ADD:
		alu->operation = 1;
		break;
	case BCM2711_QPU_INTEGER_SUBTRACT:
		alu->operation = 2;
		break;
	default:
		return ENOTSUP;
	}

	/* Succeeded: the multiply ALU's opcode and variant are chosen. */
	return 0;
}

/*
 * Places one operation's semantic sources in its ALU's two read addresses.
 *
 * The first read address of the ALU is first_port, the second the one after
 * it.  A small immediate occupies the read address of its input and counts
 * against the word's one small immediate; an operation that tells its twin
 * apart by the order of its inputs has them exchanged when needed.
 */
static int
alu_sources(
	const struct bcm2711_qpu_instruction *instruction,
	struct qpu71_alu *alu,
	enum qpu71_port first_port,
	struct qpu71_encoding *encoding)
{
	struct qpu71_read left;
	struct qpu71_read right;
	struct qpu71_read exchanged;
	enum qpu71_port second_port;
	uint32_t left_key;
	uint32_t right_key;
	int error;

	/* An operation without inputs keeps its variant selector and reads nothing. */
	if (alu->sources == 0)
		return 0;

	/* The first input is resolved before it can influence the word. */
	error = read_field(&instruction->left, &left);
	if (error != 0)
		return error;

	/* A one-input operation reads through its first read address alone. */
	if (alu->sources == 1) {
		alu->left = left.address;
		if (left.constant != 0) {
			error = note_constant(encoding, first_port);
			if (error != 0)
				return error;
		}

		/* The operation's input is placed. */
		return 0;
	}

	/* The second input is resolved too. */
	error = read_field(&instruction->right, &right);
	if (error != 0)
		return error;

	/*
	 * A float add or minimum needs the left order key not above the right
	 * one; a float maximum needs it above.  Equal keys cannot express a
	 * maximum, so the caller must replace it by a move of the one input.
	 */
	left_key = read_key(&left);
	right_key = read_key(&right);
	if (alu->order == 1 && left_key > right_key) {
		exchanged = left;
		left = right;
		right = exchanged;
	} else if (alu->order == 2 && left_key == right_key) {
		return ENOTSUP;
	} else if (alu->order == 2 && left_key < right_key) {
		exchanged = left;
		left = right;
		right = exchanged;
	}

	/* The inputs take the ALU's two read addresses. */
	alu->left = left.address;
	alu->right = right.address;

	/* A small immediate on the first read address is noted. */
	if (left.constant != 0) {
		error = note_constant(encoding, first_port);
		if (error != 0)
			return error;
	}

	/* The ALU's second read address follows its first: b after a, d after c. */
	second_port = QPU71_PORT_D;
	if (first_port == QPU71_PORT_A)
		second_port = QPU71_PORT_B;

	/* A small immediate on the second read address is noted too. */
	if (right.constant != 0) {
		error = note_constant(encoding, second_port);
		if (error != 0)
			return error;
	}

	/* Succeeded: the operation's inputs are placed in its read addresses. */
	return 0;
}

/* Resolves one semantic source into a read address, or into the index of a small immediate. */
static int
read_field(
	const struct bcm2711_qpu_source *source,
	struct qpu71_read *read)
{
	uint32_t immediate;
	int error;

	/* A small constant takes the index of its exact bits in the native table. */
	if (source->kind == BCM2711_QPU_CONSTANT) {
		error = bcm2711_qpu_small_constant(source->number, &immediate);
		if (error != 0)
			return error;
		read->address = immediate;
		read->constant = 1;

		/* Succeeded: the source is the small immediate of this index. */
		return 0;
	}

	/* V3D 7.1 has no accumulators; only physical registers remain. */
	if (source->kind != BCM2711_QPU_REGISTER)
		return EINVAL;

	/* Native physical registers have sixty-four selectors; no truncation may alias another register. */
	if (source->number >= 64)
		return EINVAL;

	/* The register is read through its own address. */
	read->address = source->number;
	read->constant = 0;

	/* Succeeded: the source names one read address. */
	return 0;
}

/*
 * Reports the order key of one input: its small immediate mark, unpack and address.
 *
 * Values referenced: Mesa 25.3.6 src/broadcom/qpu/qpu_pack.c lines 1864-1866
 * (the key is the small immediate mark times 256, plus the unpack field
 * times 64, plus the read address).  Every input here has the unpack field
 * value one, which means no unpack.
 */
static uint32_t
read_key(
	const struct qpu71_read *read)
{
	uint32_t key;

	/* A small immediate orders above every register. */
	key = 64 + read->address;
	if (read->constant != 0)
		key += 256;

	/* Reports the key the hardware compares. */
	return key;
}

/* Records the word's one small immediate and the read address it replaces. */
static int
note_constant(
	struct qpu71_encoding *encoding,
	enum qpu71_port port)
{
	/* The signal field names only one read address as a small immediate. */
	if (encoding->constants != 0)
		return ENOTSUP;

	/* This read address takes the word's small immediate. */
	encoding->constants = 1;
	encoding->constant_port = port;

	/* Succeeded: the small immediate is recorded. */
	return 0;
}

/*
 * Combines both ALUs' predicates and flag pushes into the one native conditions field.
 *
 * Values referenced: Mesa 25.3.6 src/broadcom/qpu/qpu_pack.c lines 345-430
 * (v3d_qpu_flags_pack's table; the field is the same on 4.2 and 7.1).
 */
static int
condition_fields(
	const struct bcm2711_qpu_instruction *add,
	const struct bcm2711_qpu_instruction *multiply,
	struct qpu71_encoding *encoding)
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
		/* Both ALUs are predicated. */
		encoding->conditions = 0x40 | ((multiply->predicate - 1) << 4) | (add->predicate - 1);
	} else if (add->predicate != 0) {
		/* The add ALU is predicated; the multiply ALU may push. */
		encoding->conditions = 0x20 | ((add->predicate - 1) << 2) | multiply->push_flags;
	} else if (multiply->predicate != 0) {
		/* The multiply ALU is predicated; the add ALU may push. */
		encoding->conditions = 0x30 | ((multiply->predicate - 1) << 2) | add->push_flags;
	} else if (add->push_flags != 0) {
		/* Only the add ALU pushes. */
		encoding->conditions = add->push_flags;
	} else if (multiply->push_flags != 0) {
		/* Only the multiply ALU pushes. */
		encoding->conditions = 0x10 | multiply->push_flags;
	} else {
		/* Neither ALU is predicated or pushes flags. */
		encoding->conditions = 0;
	}

	/* Succeeded: the conditions field states both ALUs' lane flag use. */
	return 0;
}

/*
 * Encodes the word's signal, its thread switch and its small immediate in the signal field.
 *
 * Values referenced: Mesa 25.3.6 src/broadcom/qpu/qpu_pack.c lines 164-197
 * (v3d71_sig_map) and src/broadcom/qpu/qpu_instr.c (v3d_qpu_sig_writes_address:
 * the loads of a register, a texture result, a varying and the tile buffer
 * use the conditions field for their destination).
 */
static int
signal_fields(
	enum bcm2711_qpu_signal signal,
	const struct bcm2711_qpu_destination *destination,
	uint32_t thread_switch,
	struct qpu71_encoding *encoding)
{
	uint32_t addressed;
	int error;

	/* Each signal has one field value; the addressed loads also take the conditions field. */
	addressed = 0;
	switch (signal) {
	case BCM2711_QPU_SIGNAL_NONE:
		encoding->signal = 0;
		break;
	case BCM2711_QPU_SIGNAL_SWITCH:
		encoding->signal = 1;
		break;
	case BCM2711_QPU_SIGNAL_UNIFORM:
		/* The uniform load that writes rf0 implicitly on 7.1. */
		encoding->signal = 2;
		break;
	case BCM2711_QPU_SIGNAL_TEXTURE:
		encoding->signal = 4;
		addressed = 1;
		break;
	case BCM2711_QPU_SIGNAL_VARYING:
		/* The varying load; its constant coefficient lands in rf0 one word later on 7.1. */
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
		/* The uniform address load that writes rf0 implicitly on 7.1. */
		encoding->signal = 24;
		break;
	case BCM2711_QPU_SIGNAL_UNIFORM_ADDRESS_REGISTER:
		encoding->signal = 25;
		addressed = 1;
		break;
	default:
		return ENOTSUP;
	}

	/* An addressed load's destination replaces the conditions field. */
	if (addressed != 0) {
		/* Lane conditions, including a flag push on an otherwise idle ALU, would be lost. */
		if (encoding->conditions != 0)
			return ENOTSUP;

		/* The destination has sixty-four selectors and one magic bit. */
		if (destination->number >= 64)
			return EINVAL;
		if (destination->peripheral > 1)
			return EINVAL;

		/* A magic destination must exist on 7.1. */
		if (destination->peripheral != 0) {
			error = magic_check(destination->number);
			if (error != 0)
				return error;
		}

		/* The destination takes the field. */
		encoding->conditions = destination->number;
		if (destination->peripheral != 0)
			encoding->conditions |= QPU71_SIGNAL_MAGIC;
	}

	/* A thread switch combines with no signal, a uniform load, a texture result read, a varying load or a texture configuration. */
	if (thread_switch != 0) {
		/* The signal table has no switch beside a small immediate. */
		if (encoding->constants != 0)
			return ENOTSUP;

		/* The combined field value replaces the signal's own. */
		error = switch_signal(signal, &encoding->signal);
		if (error != 0)
			return error;
	}

	/* A small immediate has a field value of its own, which no other signal shares on 7.1. */
	if (encoding->constants != 0) {
		/* Unsupported combinations never silently drop a load, a switch or a texture configuration. */
		if (signal != BCM2711_QPU_SIGNAL_NONE)
			return ENOTSUP;

		/* The field names the read address that holds the immediate. */
		error = constant_signal(encoding->constant_port, &encoding->signal);
		if (error != 0)
			return error;
	}

	/* Succeeded: one exact native signal preserves both input interpretation and destination ownership. */
	return 0;
}

/*
 * Reports the signal field value of one signal combined with a thread switch.
 *
 * Values referenced: Mesa 25.3.6 src/broadcom/qpu/qpu_pack.c lines 164-197
 * (v3d71_sig_map rows 1, 3, 5, 9, 13 and 19).
 */
static int
switch_signal(
	enum bcm2711_qpu_signal signal,
	uint32_t *encoded)
{
	/* Only these signals have a row with the switch beside them. */
	switch (signal) {
	case BCM2711_QPU_SIGNAL_NONE:
		*encoded = 1;
		break;
	case BCM2711_QPU_SIGNAL_UNIFORM:
		*encoded = 3;
		break;
	case BCM2711_QPU_SIGNAL_TEXTURE:
		*encoded = 5;
		break;
	case BCM2711_QPU_SIGNAL_VARYING:
		*encoded = 9;
		break;
	case BCM2711_QPU_SIGNAL_UNIFORM_REGISTER:
		*encoded = 13;
		break;
	case BCM2711_QPU_SIGNAL_TEXTURE_CONFIG:
		*encoded = 19;
		break;
	default:
		/* No other signal, nor a second switch, combines with a switch. */
		return ENOTSUP;
	}

	/* Succeeded: the combined value is reported. */
	return 0;
}

/* Reports the signal field value that marks one read address as a small immediate. */
static int
constant_signal(
	enum qpu71_port port,
	uint32_t *encoded)
{
	/* Each read address has its own row. */
	switch (port) {
	case QPU71_PORT_A:
		*encoded = QPU71_SIGNAL_CONSTANT_A;
		break;
	case QPU71_PORT_B:
		*encoded = QPU71_SIGNAL_CONSTANT_B;
		break;
	case QPU71_PORT_C:
		*encoded = QPU71_SIGNAL_CONSTANT_C;
		break;
	case QPU71_PORT_D:
		*encoded = QPU71_SIGNAL_CONSTANT_D;
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: the row of the read address is reported. */
	return 0;
}
