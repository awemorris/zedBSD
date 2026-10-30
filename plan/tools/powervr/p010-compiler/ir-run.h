/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* A host interpreter of the PowerVR compiler's intermediate form (host test only). */

#ifndef WS269_P010_IR_RUN_H
#define WS269_P010_IR_RUN_H

#include <stdint.h>

#include "drivers/gpu/powervr/compiler/shader-ir.h"

/* The output words the interpreter keeps. */
#define IR_RUN_OUTPUTS	64

/*
 * One run: the values on each lane, the outputs, and what the shader
 * reads from outside (shared words, inputs, memory, textures).
 */
struct ir_run {
	uint32_t values[POWERVR_IR_VALUES][4][32];
	uint32_t outputs[IR_RUN_OUTPUTS][32];
	uint32_t pixels[4][32];
	uint32_t dropped[32];
	uint32_t (*shared)(struct ir_run *run, uint32_t request, uint32_t word);
	uint32_t (*input)(struct ir_run *run, unsigned lane, uint32_t reg);
	uint32_t (*iterate)(struct ir_run *run, unsigned lane, uint32_t coefficient, unsigned flags);
	uint32_t (*load)(struct ir_run *run, uint64_t address);
	void (*store)(struct ir_run *run, uint64_t address, uint32_t word);
	void (*sample)(struct ir_run *run, uint32_t image, uint32_t sampler, uint32_t u, uint32_t v, uint32_t result[4]);
	void *context;
};

uint32_t ir_run_pack(const uint32_t words[4]);
void ir_run_unpack(uint32_t packed, uint32_t words[4]);
uint32_t ir_run_blend(uint32_t kind, const uint32_t source[4], uint32_t packed);
uint32_t ir_run_pack_half(uint32_t low, uint32_t high);
void ir_run_unpack_half(uint32_t packed, uint32_t words[2]);
int ir_run(struct ir_run *run, const struct powervr_ir_shader *shader, uint32_t lanes);

#endif /* WS269_P010_IR_RUN_H */
