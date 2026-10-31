/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host model of one V3D 4.2 QPU lane running a native program (WS141 p013).
 *
 * The words are read with the unmodified MIT Mesa decoder.  Both ALUs of a
 * word read their sources when the word starts and write when it ends; a
 * special function's result lands two words later, a varying's coefficient
 * in accumulator five one word later.  A read of a register whose write is
 * still on its way, a write landing in another thread, accumulators or flags
 * used across a thread switch, and the word, switch, TMU, tile and thread end
 * rules the hardware sets are failures.  It is a test model, not a cycle
 * model.
 */
#ifndef WS141_QPU_MACHINE_H
#define WS141_QPU_MACHINE_H

#include <stdint.h>

#include "broadcom/common/v3d_device_info.h"
#include "broadcom/qpu/qpu_instr.h"
#include "drivers/gpu/bcm2711/compiler/shader.h"

/* What the program's environment supplies; the callbacks receive the context. */
struct qpu_machine_setup {
	const struct bcm2711_shader_binary *binary;
	/* Fragment varyings in FIFO order, and their interpolation from the pipeline key. */
	const uint32_t *varyings;
	const struct bcm2711_shader_key *key;
	/* Vertex attributes in VPM order. */
	const uint32_t *attributes;
	/* The fragment payload: W, centroid W and Z. */
	uint32_t payload[3];
	/* Fills rf3 to rf63 before the program runs, so a read of an unwritten register is visible. */
	uint32_t garbage_seed;
	/* The program's words decoded once by qpu_machine_decode, or null to decode each word as it runs. */
	const struct v3d_qpu_instr *decoded;
	void *context;
	uint32_t (*uniform)(void *context, const struct bcm2711_shader_uniform *uniform);
	void (*sample)(void *context, uint32_t set, uint32_t binding, uint32_t s, uint32_t t, uint32_t *texel);
};

/* What one run produced. */
struct qpu_machine_result {
	uint32_t tile[2];
	uint32_t tile_writes;
	uint32_t vpm[68];
	uint32_t lookups;
	/* Set when a SETMSF cleared the lane's coverage, so its tile writes are dropped. */
	uint32_t discarded;
	/* Counts of words that paired both ALUs and that did nothing. */
	uint32_t paired;
	uint32_t idle;
};

/* Decodes and checks every word of a program once, for runs that share it. */
void qpu_machine_decode(const struct v3d_device_info *device, const struct bcm2711_shader_binary *binary, struct v3d_qpu_instr *decoded);

/* Runs one lane; a broken rule prints the word and stops the test. */
void qpu_machine_run(const struct v3d_device_info *device, const struct qpu_machine_setup *setup, struct qpu_machine_result *result);

#endif
