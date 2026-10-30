/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* A host simulator of the PowerVR compiler's machine operations (host test only). */

#ifndef WS269_P010_MACHINE_RUN_H
#define WS269_P010_MACHINE_RUN_H

#include <stdint.h>

#include "drivers/gpu/powervr/compiler/shader.h"
#include "drivers/gpu/powervr/compiler/shader-private.h"

/* The output words the simulator keeps, and the shared registers a shared program may write. */
#define MACHINE_RUN_OUTPUTS	64
#define MACHINE_RUN_SHAREDS	256

/*
 * One run of a compiled program over 32 instances: the registers of
 * each instance, the predicates, and what the program reads from
 * outside, answered the same way as for the interpreter.  A shared
 * program's writes to shared registers are kept (shareds) for the
 * program that reads them as its computed request (q1546).
 */
struct machine_run {
	const struct powervr_shader_program *program;
	uint32_t temps[SHADER_MACHINE_TEMPORARIES][32];
	uint32_t predicate[32];
	uint32_t outputs[MACHINE_RUN_OUTPUTS][32];
	uint32_t pixels[4][32];
	uint32_t dropped[32];
	uint32_t shareds[MACHINE_RUN_SHAREDS];
	uint8_t shared_written[MACHINE_RUN_SHAREDS];
	uint32_t (*shared_word)(const struct powervr_shader_program *program, uint32_t request, uint32_t word);
	uint32_t (*input_word)(unsigned lane, uint32_t reg);
	uint32_t (*iterate_word)(unsigned lane, uint32_t coefficient, unsigned flags);
	uint32_t (*coefficient_word)(uint32_t reg);
	void (*sample_words)(uint32_t image, uint32_t sampler, uint32_t u, uint32_t v, uint32_t result[4]);
	uint32_t *memory;
	uint64_t memory_base;
	uint32_t memory_words;
};

int machine_run_program(struct machine_run *run, const struct shader_backend *backend, uint32_t lanes);

#endif /* WS269_P010_MACHINE_RUN_H */
