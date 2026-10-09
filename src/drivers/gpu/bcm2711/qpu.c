/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Independent single-operation 4.2 encoding uses fixed MIT Mesa hardware format facts. */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/qpu.h"

/* One temporary instruction assigns physical read ports before committing a complete 64-bit word. */
struct qpu_encoding {
	uint32_t add_operation;
	uint32_t multiply_operation;
	uint32_t add_left;
	uint32_t add_right;
	uint32_t multiply_left;
	uint32_t multiply_right;
	uint32_t add_address;
	uint32_t multiply_address;
	uint32_t add_peripheral;
	uint32_t multiply_peripheral;
	uint32_t read_a;
	uint32_t read_b;
	uint32_t read_a_used;
	uint32_t read_b_used;
	uint32_t constant;
	uint32_t signal;
	uint32_t conditions;
	uint32_t sources;
	uint32_t multiply;
	uint32_t order;
};

static int operation_fields(const struct bcm2711_qpu_instruction *instruction, struct qpu_encoding *encoding);
static int source_fields(const struct bcm2711_qpu_source *source, struct qpu_encoding *encoding, uint32_t *mux);
static int signal_fields(const struct bcm2711_qpu_instruction *instruction, struct qpu_encoding *encoding);

/*
 * Encodes one native instruction after validating every operand, signal and flag interaction.
 */
int
bcm2711_qpu_encode(
	const struct bcm2711_qpu_instruction *instruction,
	uint64_t *word)
{
	struct qpu_encoding encoding;
	uint64_t packed;
	uint32_t left;
	uint32_t right;
	uint32_t exchanged;
	int error;

	/* Refusal preserves the caller's word, so a partial machine instruction can never be published. */
	if (instruction == NULL || word == NULL)
		return EINVAL;
	if (instruction->destination.number >= 64 || instruction->destination.peripheral > 1 ||
	    instruction->predicate > 4 || instruction->push_flags > 3)
		return EINVAL;
	if (instruction->predicate != 0 && instruction->push_flags != 0)
		return ENOTSUP;

	/* Both idle ALUs use native no-write destinations; a selected operation replaces exactly one. */
	kern_memset(&encoding, 0, sizeof(encoding));
	encoding.add_operation = 187;
	encoding.multiply_operation = 15;
	encoding.multiply_right = 4;
	encoding.add_address = 6;
	encoding.multiply_address = 6;
	encoding.add_peripheral = 1;
	encoding.multiply_peripheral = 1;
	error = operation_fields(instruction, &encoding);
	if (error != 0)
		return error;

	/* Actual semantic inputs acquire no more than two physical read ports and one shared small constant. */
	left = 0;
	right = 0;
	if (encoding.sources >= 1) {
		error = source_fields(&instruction->left, &encoding, &left);
		if (error != 0)
			return error;
	}

	/* Every fallible input is checked before its mux can influence the encoded operation. */
	if (encoding.sources >= 2) {
		error = source_fields(&instruction->right, &encoding, &right);
		if (error != 0)
			return error;
	}

	/* The ordering of equal-unpack commutative operands selects the native min/max or add variant. */
	if (encoding.order == 1 && left > right) {
		exchanged = left;
		left = right;
		right = exchanged;
	} else if (encoding.order == 2 && left <= right) {
		/* Equal muxes cannot express max; its caller must preserve the value through a separate move. */
		if (left == right)
			return ENOTSUP;
		exchanged = left;
		left = right;
		right = exchanged;
	}

	/* Multiplication and bit-preserving moves consume the multiply ALU; all other operations use add. */
	if (encoding.multiply != 0) {
		if (encoding.sources >= 1)
			encoding.multiply_left = left;
		if (encoding.sources >= 2)
			encoding.multiply_right = right;
	} else {
		if (encoding.sources >= 1)
			encoding.add_left = left;
		if (encoding.sources >= 2)
			encoding.add_right = right;
	}

	/* Flag predicates and pushes use different fields for the two selected ALUs. */
	if (instruction->predicate != 0) {
		encoding.conditions = 0x20 + ((instruction->predicate - 1) << 2);
		if (encoding.multiply != 0)
			encoding.conditions |= 0x10;
	} else if (instruction->push_flags != 0) {
		encoding.conditions = instruction->push_flags;
		if (encoding.multiply != 0)
			encoding.conditions |= 0x10;
	}

	/* Address-producing signals replace the conditions field and cannot silently discard lane flags. */
	error = signal_fields(instruction, &encoding);
	if (error != 0)
		return error;

	/* The fixed instruction format is assembled locally before the caller receives any machine bits. */
	packed = (uint64_t)encoding.multiply_operation << 58;
	packed |= (uint64_t)encoding.signal << 53;
	packed |= (uint64_t)encoding.conditions << 46;
	packed |= (uint64_t)encoding.multiply_peripheral << 45;
	packed |= (uint64_t)encoding.add_peripheral << 44;
	packed |= (uint64_t)encoding.multiply_address << 38;
	packed |= (uint64_t)encoding.add_address << 32;
	packed |= (uint64_t)encoding.add_operation << 24;
	packed |= (uint64_t)encoding.multiply_right << 21;
	packed |= (uint64_t)encoding.multiply_left << 18;
	packed |= (uint64_t)encoding.add_right << 15;
	packed |= (uint64_t)encoding.add_left << 12;
	packed |= (uint64_t)encoding.read_a << 6;
	packed |= encoding.read_b;
	*word = packed;

	/* Succeeded: the caller owns one complete independently encoded native 4.2 instruction. */
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
	} else if (bits >= 0x3b800000U && bits <= 0x43000000U && (bits & 0x007fffffU) == 0) {
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

/* Assigns one native operation and its fixed mux selectors without copying an upstream encoder table. */
static int
operation_fields(
	const struct bcm2711_qpu_instruction *instruction,
	struct qpu_encoding *encoding)
{
	/* Each operation states its arity; unused mux bits remain available for native opcode selectors. */
	encoding->sources = 2;
	switch (instruction->operation) {
	case BCM2711_QPU_IDLE:
		encoding->sources = 0;
		break;
	case BCM2711_QPU_MOVE:
		encoding->sources = 1;
		encoding->multiply = 1;
		encoding->multiply_operation = 15;
		encoding->multiply_right = 7;
		break;
	case BCM2711_QPU_FLOAT_ADD:
		encoding->add_operation = 5;
		encoding->order = 1;
		break;
	case BCM2711_QPU_FLOAT_SUBTRACT:
		encoding->add_operation = 69;
		break;
	case BCM2711_QPU_FLOAT_MULTIPLY:
		encoding->multiply = 1;
		encoding->multiply_operation = 21;
		break;
	case BCM2711_QPU_FLOAT_MINIMUM:
		encoding->add_operation = 133;
		encoding->order = 1;
		break;
	case BCM2711_QPU_FLOAT_MAXIMUM:
		encoding->add_operation = 133;
		encoding->order = 2;
		break;
	case BCM2711_QPU_FLOAT_COMPARE:
		encoding->add_operation = 197;
		break;
	case BCM2711_QPU_INTEGER_ADD:
		encoding->add_operation = 56;
		break;
	case BCM2711_QPU_INTEGER_SUBTRACT:
		encoding->add_operation = 60;
		break;
	case BCM2711_QPU_INTEGER_MINIMUM:
		encoding->add_operation = 120;
		break;
	case BCM2711_QPU_INTEGER_MAXIMUM:
		encoding->add_operation = 121;
		break;
	case BCM2711_QPU_UNSIGNED_MINIMUM:
		encoding->add_operation = 122;
		break;
	case BCM2711_QPU_UNSIGNED_MAXIMUM:
		encoding->add_operation = 123;
		break;
	case BCM2711_QPU_SHIFT_LEFT:
		encoding->add_operation = 124;
		break;
	case BCM2711_QPU_SHIFT_RIGHT:
		encoding->add_operation = 125;
		break;
	case BCM2711_QPU_SHIFT_SIGNED:
		encoding->add_operation = 126;
		break;
	case BCM2711_QPU_BITS_AND:
		encoding->add_operation = 181;
		break;
	case BCM2711_QPU_BITS_OR:
		encoding->add_operation = 182;
		break;
	case BCM2711_QPU_BITS_XOR:
		encoding->add_operation = 183;
		break;
	case BCM2711_QPU_BITS_NOT:
		encoding->sources = 1;
		encoding->add_operation = 186;
		break;
	case BCM2711_QPU_INTEGER_NEGATE:
		encoding->sources = 1;
		encoding->add_operation = 186;
		encoding->add_right = 1;
		break;
	case BCM2711_QPU_FLOAT_RECIPROCAL:
		encoding->sources = 1;
		encoding->add_operation = 186;
		encoding->add_right = 5;
		break;
	case BCM2711_QPU_FLOAT_ROUND:
		encoding->sources = 1;
		encoding->add_operation = 245;
		break;
	case BCM2711_QPU_FLOAT_TRUNCATE:
		encoding->sources = 1;
		encoding->add_operation = 245;
		encoding->add_right = 4;
		break;
	case BCM2711_QPU_FLOAT_FLOOR:
		encoding->sources = 1;
		encoding->add_operation = 246;
		break;
	case BCM2711_QPU_FLOAT_CEILING:
		encoding->sources = 1;
		encoding->add_operation = 246;
		encoding->add_right = 4;
		break;
	case BCM2711_QPU_FLOAT_TO_SIGNED:
		encoding->sources = 1;
		encoding->add_operation = 245;
		encoding->add_right = 7;
		break;
	case BCM2711_QPU_FLOAT_TO_UNSIGNED:
		encoding->sources = 1;
		encoding->add_operation = 246;
		encoding->add_right = 3;
		break;
	case BCM2711_QPU_SIGNED_TO_FLOAT:
		encoding->sources = 1;
		encoding->add_operation = 252;
		break;
	case BCM2711_QPU_UNSIGNED_TO_FLOAT:
		encoding->sources = 1;
		encoding->add_operation = 252;
		encoding->add_right = 4;
		break;
	case BCM2711_QPU_DERIVATIVE_X:
		encoding->sources = 1;
		encoding->add_operation = 247;
		break;
	case BCM2711_QPU_DERIVATIVE_Y:
		encoding->sources = 1;
		encoding->add_operation = 247;
		encoding->add_right = 4;
		break;
	case BCM2711_QPU_FLOAT_INVERSE_ROOT:
		encoding->sources = 1;
		encoding->add_operation = 188;
		encoding->add_right = 3;
		break;
	case BCM2711_QPU_FLOAT_EXPONENT:
		encoding->sources = 1;
		encoding->add_operation = 188;
		encoding->add_right = 4;
		break;
	case BCM2711_QPU_FLOAT_LOGARITHM:
		encoding->sources = 1;
		encoding->add_operation = 188;
		encoding->add_right = 5;
		break;
	case BCM2711_QPU_FLOAT_SINE:
		encoding->sources = 1;
		encoding->add_operation = 188;
		encoding->add_right = 6;
		break;
	case BCM2711_QPU_VPM_LOAD:
		encoding->sources = 1;
		encoding->add_operation = 188;
		break;
	case BCM2711_QPU_VPM_STORE:
		encoding->add_operation = 248;
		break;
	case BCM2711_QPU_TEXTURE_WAIT:
		encoding->sources = 0;
		encoding->add_operation = 187;
		encoding->add_left = 5;
		encoding->add_right = 2;
		break;
	case BCM2711_QPU_VPM_WAIT:
		encoding->sources = 0;
		encoding->add_operation = 187;
		encoding->add_left = 6;
		encoding->add_right = 2;
		break;
	case BCM2711_QPU_PIXEL_X:
		encoding->sources = 0;
		encoding->add_operation = 187;
		encoding->add_right = 1;
		break;
	case BCM2711_QPU_PIXEL_Y:
		encoding->sources = 0;
		encoding->add_operation = 187;
		encoding->add_left = 4;
		encoding->add_right = 1;
		break;
	case BCM2711_QPU_INSTANCE_INDEX:
		encoding->sources = 0;
		encoding->add_operation = 187;
		encoding->add_left = 2;
		encoding->add_right = 2;
		break;
	case BCM2711_QPU_ELEMENT_INDEX:
		encoding->sources = 0;
		encoding->add_operation = 187;
		encoding->add_left = 2;
		break;
	default:
		return ENOTSUP;
	}

	/* The selected functional unit owns its explicit per-thread or native peripheral destination. */
	if (instruction->operation != BCM2711_QPU_IDLE) {
		if (encoding->multiply != 0) {
			encoding->multiply_address = instruction->destination.number;
			encoding->multiply_peripheral = instruction->destination.peripheral;
		} else {
			encoding->add_address = instruction->destination.number;
			encoding->add_peripheral = instruction->destination.peripheral;
		}
	}

	/* VPM input loads distinguish input/output reads with the add peripheral bit. */
	if (instruction->operation == BCM2711_QPU_VPM_LOAD && instruction->destination.peripheral != 0)
		return EINVAL;
	if (instruction->operation == BCM2711_QPU_VPM_STORE) {
		/* A vector VPM store encodes its storage layout in the write-address field, not a register. */
		encoding->add_address = 0;
		encoding->add_peripheral = 0;
	}

	/* Idle ALUs cannot meaningfully predicate or update lane flags. */
	if (instruction->operation == BCM2711_QPU_IDLE &&
	    (instruction->predicate != 0 || instruction->push_flags != 0))
		return EINVAL;

	/* Succeeded: one supported native operation and its exact source selectors are ready. */
	return 0;
}

/* Assigns one semantic source to an accumulator, read port or exact native small constant. */
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

	/* A small constant occupies port B, so another operand cannot already own that physical port. */
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
	if (encoding->read_a_used == 0) {
		encoding->read_a = source->number;
		encoding->read_a_used = 1;
		*mux = 6;
	} else if (encoding->read_b_used == 0) {
		encoding->read_b = source->number;
		encoding->read_b_used = 1;
		*mux = 7;
	} else {
		/* Single-operation instructions cannot require a third physical input port. */
		return ENOTSUP;
	}

	/* Succeeded: this semantic register is read through one complete native port selector. */
	return 0;
}

/* Encodes one supported signal and its checked interaction with conditions or small constants. */
static int
signal_fields(
	const struct bcm2711_qpu_instruction *instruction,
	struct qpu_encoding *encoding)
{
	uint32_t addressed;

	/* The signal's destination replaces the native conditions field only for address-producing loads. */
	addressed = 0;
	switch (instruction->signal) {
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
		if (instruction->signal_destination.number >= 64 || instruction->signal_destination.peripheral > 1)
			return EINVAL;
		encoding->conditions = instruction->signal_destination.number;
		if (instruction->signal_destination.peripheral != 0)
			encoding->conditions |= 64;
	}

	/* Small constants use their exact signal encoding; only native varying or TMU reads can combine with it. */
	if (encoding->constant != 0) {
		if (instruction->signal == BCM2711_QPU_SIGNAL_NONE) {
			encoding->signal = 15;
		} else if (instruction->signal == BCM2711_QPU_SIGNAL_VARYING) {
			encoding->signal = 14;
		} else if (instruction->signal == BCM2711_QPU_SIGNAL_TEXTURE) {
			encoding->signal = 31;
		} else {
			/* Unsupported combinations never silently drop a uniform read, switch or texture configuration. */
			return ENOTSUP;
		}
	}

	/* Succeeded: one exact native signal preserves both input interpretation and destination ownership. */
	return 0;
}
