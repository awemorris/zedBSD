/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Compiler state has no device ownership and is discarded before publishing a completed program. */
#ifndef KERN_DRIVERS_GPU_BCM2711_SHADER_PRIVATE_H
#define KERN_DRIVERS_GPU_BCM2711_SHADER_PRIVATE_H

#include "drivers/gpu/bcm2711/compiler/qpu.h"
#include "drivers/gpu/bcm2711/compiler/shader.h"
#include "drivers/gpu/compiler/ir.h"

/* User outputs occupy sixteen locations; clip position follows those sixty-four scalar slots. */
#define BCM2711_SHADER_OUTPUT_WORDS 68U
#define BCM2711_SHADER_POSITION 64U

/* An absent definition, last use or output never aliases IR value zero. */
#define BCM2711_SHADER_ABSENT 0xffffffffU

/*
 * Expression storage excludes fragment payload and the four materialization
 * scratch registers.  The scratch registers are named rf60 to rf63 while
 * compiling; a four-thread program, whose threads see rf0 to rf31, encodes
 * them as rf28 to rf31 and keeps its expressions below rf28.
 */
#define BCM2711_SHADER_FIRST_REGISTER 4U
#define BCM2711_SHADER_REGISTER_END 60U
#define BCM2711_SHADER_QUAD_REGISTER_END 28U
#define BCM2711_SHADER_SCRATCH_LEFT 60U
#define BCM2711_SHADER_SCRATCH_RIGHT 61U
#define BCM2711_SHADER_SCRATCH_WORK 62U
#define BCM2711_SHADER_SCRATCH_AUX 63U

/*
 * Expression registers a program keeps free before it borrows one as a
 * short-lived scratch register: a texture sample allocates its four
 * results after its coordinates are materialized.
 */
#define BCM2711_SHADER_SCRATCH_RESERVE 4U

/* Free expression registers a program keeps before it caches another full-width constant in one. */
#define BCM2711_SHADER_CONSTANT_RESERVE 12U

/* What a recorded thread switch is for; the scheduler places each kind under its own rules. */
enum bcm2711_shader_switch {
	BCM2711_SHADER_SWITCH_NONE,
	/* Lets the other threads run while this thread's texture lookup completes. */
	BCM2711_SHADER_SWITCH_SAMPLE,
	/* The first and second of the two consecutive switches that mark the last switch before tile writes. */
	BCM2711_SHADER_SWITCH_LAST_FIRST,
	BCM2711_SHADER_SWITCH_LAST_SECOND,
	/* Ends the thread two words later. */
	BCM2711_SHADER_SWITCH_END
};

/*
 * One recorded operation or signal, in source order, before native words are chosen.
 *
 * Register numbers are already placed in the program's thread window.  A
 * record with an idle ALU and no signal is a conservative delay word, which
 * the scheduler replaces by explicit latencies.
 */
struct bcm2711_shader_op {
	struct bcm2711_qpu_instruction instruction;
	/* The uniform this record's signal or tile write consumes, when has_uniform is set. */
	struct bcm2711_shader_uniform uniform;
	uint32_t has_uniform;
	enum bcm2711_shader_switch role;
};

/* One scalar tracks source order, liveness and whether its exact bits need a physical register. */
struct bcm2711_shader_value {
	uint32_t definition;
	uint32_t last_read;
	uint32_t number;
	uint32_t constant;
	uint32_t register_live;
	uint32_t pinned;
};

/* One compilation keeps bounded storage and checked interfaces private until every native word succeeds. */
struct bcm2711_shader_compiler {
	const struct drv_gpu_shader_ir *ir;
	const struct bcm2711_shader_key *key;
	struct bcm2711_shader_binary *binary;
	struct bcm2711_shader_diagnostic *diagnostic;
	struct bcm2711_shader_value *values;
	uint32_t registers[64];
	uint32_t input_registers[BCM2711_SHADER_INTERFACE_WORDS];
	uint32_t outputs[BCM2711_SHADER_OUTPUT_WORDS];
	uint32_t instruction;
	/* Two or four threads a QPU runs this program as; four leave each thread rf0 to rf31. */
	uint32_t threads;
	/* The first physical register above the expression registers (the four scratch registers' first one in the encoding). */
	uint32_t register_end;
	uint32_t code_capacity;
	uint32_t uniform_capacity;
	uint32_t samples;

	/* Recorded operations in source order, at most code_capacity, turned into native words at the end. */
	struct bcm2711_shader_op *ops;
	uint32_t op_count;

	/* A uniform described before the transaction that consumes it; the next consuming record takes it. */
	struct bcm2711_shader_uniform pending_uniform;
	uint32_t has_pending_uniform;

	/*
	 * When each expression register was last released, by a clock that only
	 * increases; allocation takes the register released longest ago, so a
	 * value's register is not reused at once and the scheduler can move
	 * operations past each other.  Zero means never used.
	 */
	uint32_t released[64];
	uint32_t release_clock;

	/* Expression registers borrowed as scratch registers until the current source instruction is lowered. */
	uint32_t borrowed[64];

	/*
	 * Expression registers holding a full-width constant for later reads:
	 * its bits, when it was last read (by the release clock), and whether
	 * the current source instruction reads it, which keeps it from being
	 * given up for another value until the instruction is lowered.
	 */
	uint32_t constant_held[64];
	uint32_t constant_bits[64];
	uint32_t constant_clock[64];
	uint32_t constant_touched[64];
};

int bcm2711_shader_analyze(struct bcm2711_shader_compiler *compiler);
int bcm2711_shader_lower(struct bcm2711_shader_compiler *compiler);
int bcm2711_shader_finish(struct bcm2711_shader_compiler *compiler);
int bcm2711_shader_append(struct bcm2711_shader_compiler *compiler, const struct bcm2711_qpu_instruction *instruction);
int bcm2711_shader_nops(struct bcm2711_shader_compiler *compiler, uint32_t count);
int bcm2711_shader_operation(struct bcm2711_shader_compiler *compiler, enum bcm2711_qpu_operation operation, uint32_t destination, uint32_t peripheral, struct bcm2711_qpu_source left, struct bcm2711_qpu_source right, uint32_t predicate, uint32_t flags);
int bcm2711_shader_uniform_load(struct bcm2711_shader_compiler *compiler, const struct bcm2711_shader_uniform *uniform, uint32_t destination);
int bcm2711_shader_uniform_append(struct bcm2711_shader_compiler *compiler, const struct bcm2711_shader_uniform *uniform);
int bcm2711_shader_signal(struct bcm2711_shader_compiler *compiler, enum bcm2711_qpu_signal signal, uint32_t destination, uint32_t gaps);
int bcm2711_shader_register(struct bcm2711_shader_compiler *compiler, uint32_t *number);
void bcm2711_shader_release(struct bcm2711_shader_compiler *compiler, uint32_t number);
uint32_t bcm2711_shader_scratch(struct bcm2711_shader_compiler *compiler, uint32_t preferred);
int bcm2711_shader_constant(struct bcm2711_shader_compiler *compiler, uint32_t bits, uint32_t preferred, uint32_t *number);
void bcm2711_shader_release_scratch(struct bcm2711_shader_compiler *compiler);
int bcm2711_shader_switch(struct bcm2711_shader_compiler *compiler, enum bcm2711_shader_switch role);
int bcm2711_shader_emit(struct bcm2711_shader_compiler *compiler);
int bcm2711_shader_emit_in_order(struct bcm2711_shader_compiler *compiler);
int bcm2711_shader_schedule(struct bcm2711_shader_compiler *compiler);
int bcm2711_shader_operand(struct bcm2711_shader_compiler *compiler, uint32_t value, uint32_t scratch, struct bcm2711_qpu_source *source);
struct bcm2711_qpu_source bcm2711_shader_rf(uint32_t number);
struct bcm2711_qpu_source bcm2711_shader_bits(uint32_t bits);
int bcm2711_shader_fail(struct bcm2711_shader_compiler *compiler, int error, const char *reason);

#endif
