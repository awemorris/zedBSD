/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Compiler state has no device ownership and is discarded before publishing a completed program. */
#ifndef KERN_DRIVERS_GPU_BCM2711_SHADER_PRIVATE_H
#define KERN_DRIVERS_GPU_BCM2711_SHADER_PRIVATE_H

#include "drivers/gpu/bcm2711/qpu.h"
#include "drivers/gpu/bcm2711/shader.h"
#include "drivers/gpu/i915/compiler/ir.h"

/* User outputs occupy sixteen locations; clip position follows those sixty-four scalar slots. */
#define BCM2711_SHADER_OUTPUT_WORDS 68U
#define BCM2711_SHADER_POSITION 64U

/* An absent definition, last use or output never aliases IR value zero. */
#define BCM2711_SHADER_ABSENT 0xffffffffU

/* Expression storage excludes fragment payload and the four materialization scratch registers. */
#define BCM2711_SHADER_FIRST_REGISTER 4U
#define BCM2711_SHADER_REGISTER_END 60U
#define BCM2711_SHADER_SCRATCH_LEFT 60U
#define BCM2711_SHADER_SCRATCH_RIGHT 61U
#define BCM2711_SHADER_SCRATCH_WORK 62U
#define BCM2711_SHADER_SCRATCH_AUX 63U

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
	const struct i915_shader_ir *ir;
	const struct bcm2711_shader_key *key;
	struct bcm2711_shader_binary *binary;
	struct bcm2711_shader_diagnostic *diagnostic;
	struct bcm2711_shader_value *values;
	uint32_t registers[64];
	uint32_t input_registers[BCM2711_SHADER_INTERFACE_WORDS];
	uint32_t outputs[BCM2711_SHADER_OUTPUT_WORDS];
	uint32_t instruction;
	uint32_t code_capacity;
	uint32_t uniform_capacity;
	uint32_t samples;
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
int bcm2711_shader_operand(struct bcm2711_shader_compiler *compiler, uint32_t value, uint32_t scratch, struct bcm2711_qpu_source *source);
struct bcm2711_qpu_source bcm2711_shader_rf(uint32_t number);
struct bcm2711_qpu_source bcm2711_shader_bits(uint32_t bits);
int bcm2711_shader_fail(struct bcm2711_shader_compiler *compiler, int error, const char *reason);

#endif
