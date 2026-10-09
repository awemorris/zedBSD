/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Private 4.2 instruction construction accepts semantic operands, never userspace machine words. */
#ifndef KERN_DRIVERS_GPU_BCM2711_QPU_H
#define KERN_DRIVERS_GPU_BCM2711_QPU_H

#include <stdint.h>

/* One source names a physical register, an accumulator or the actual bits of a small constant. */
enum bcm2711_qpu_source_kind {
	BCM2711_QPU_REGISTER,
	BCM2711_QPU_ACCUMULATOR,
	BCM2711_QPU_CONSTANT
};

/* One semantic source remains immutable while its instruction assigns the two physical read ports. */
struct bcm2711_qpu_source {
	enum bcm2711_qpu_source_kind kind;
	uint32_t number;
};

/* One destination names a per-thread physical register or a trusted native peripheral write address. */
struct bcm2711_qpu_destination {
	uint32_t number;
	uint32_t peripheral;
};

/* Single-operation instructions deliberately leave the other ALU idle for conservative scheduling. */
enum bcm2711_qpu_operation {
	BCM2711_QPU_IDLE,
	BCM2711_QPU_MOVE,
	BCM2711_QPU_FLOAT_ADD,
	BCM2711_QPU_FLOAT_SUBTRACT,
	BCM2711_QPU_FLOAT_MULTIPLY,
	BCM2711_QPU_FLOAT_MINIMUM,
	BCM2711_QPU_FLOAT_MAXIMUM,
	BCM2711_QPU_FLOAT_COMPARE,
	BCM2711_QPU_INTEGER_ADD,
	BCM2711_QPU_INTEGER_SUBTRACT,
	BCM2711_QPU_INTEGER_MINIMUM,
	BCM2711_QPU_INTEGER_MAXIMUM,
	BCM2711_QPU_UNSIGNED_MINIMUM,
	BCM2711_QPU_UNSIGNED_MAXIMUM,
	BCM2711_QPU_SHIFT_LEFT,
	BCM2711_QPU_SHIFT_RIGHT,
	BCM2711_QPU_SHIFT_SIGNED,
	BCM2711_QPU_BITS_AND,
	BCM2711_QPU_BITS_OR,
	BCM2711_QPU_BITS_XOR,
	BCM2711_QPU_BITS_NOT,
	BCM2711_QPU_INTEGER_NEGATE,
	BCM2711_QPU_FLOAT_RECIPROCAL,
	BCM2711_QPU_FLOAT_ROUND,
	BCM2711_QPU_FLOAT_TRUNCATE,
	BCM2711_QPU_FLOAT_FLOOR,
	BCM2711_QPU_FLOAT_CEILING,
	BCM2711_QPU_FLOAT_TO_SIGNED,
	BCM2711_QPU_FLOAT_TO_UNSIGNED,
	BCM2711_QPU_SIGNED_TO_FLOAT,
	BCM2711_QPU_UNSIGNED_TO_FLOAT,
	BCM2711_QPU_DERIVATIVE_X,
	BCM2711_QPU_DERIVATIVE_Y,
	BCM2711_QPU_FLOAT_INVERSE_ROOT,
	BCM2711_QPU_FLOAT_EXPONENT,
	BCM2711_QPU_FLOAT_LOGARITHM,
	BCM2711_QPU_FLOAT_SINE,
	BCM2711_QPU_VPM_LOAD,
	BCM2711_QPU_VPM_STORE,
	BCM2711_QPU_TEXTURE_WAIT,
	BCM2711_QPU_VPM_WAIT,
	BCM2711_QPU_PIXEL_X,
	BCM2711_QPU_PIXEL_Y,
	BCM2711_QPU_INSTANCE_INDEX,
	BCM2711_QPU_ELEMENT_INDEX
};

/* Signals describe one hardware transaction; delay and termination scheduling stays with the compiler. */
enum bcm2711_qpu_signal {
	BCM2711_QPU_SIGNAL_NONE,
	BCM2711_QPU_SIGNAL_SWITCH,
	BCM2711_QPU_SIGNAL_UNIFORM,
	BCM2711_QPU_SIGNAL_TEXTURE,
	BCM2711_QPU_SIGNAL_VARYING,
	BCM2711_QPU_SIGNAL_UNIFORM_REGISTER,
	BCM2711_QPU_SIGNAL_TILE,
	BCM2711_QPU_SIGNAL_TILE_CONFIGURED,
	BCM2711_QPU_SIGNAL_TEXTURE_CONFIG,
	BCM2711_QPU_SIGNAL_UNIFORM_ADDRESS,
	BCM2711_QPU_SIGNAL_UNIFORM_ADDRESS_REGISTER
};

/* One fully checked instruction can be encoded atomically without changing a refused output word. */
struct bcm2711_qpu_instruction {
	enum bcm2711_qpu_operation operation;
	struct bcm2711_qpu_destination destination;
	struct bcm2711_qpu_source left;
	struct bcm2711_qpu_source right;

	/* Zero disables predication; one through four select A, B, not-A or not-B lane flags. */
	uint32_t predicate;

	/* Zero preserves flags; one through three push zero, negative or carry flags. */
	uint32_t push_flags;
	enum bcm2711_qpu_signal signal;
	struct bcm2711_qpu_destination signal_destination;
};

int bcm2711_qpu_encode(const struct bcm2711_qpu_instruction *instruction, uint64_t *word);
int bcm2711_qpu_small_constant(uint32_t bits, uint32_t *encoded);

#endif
