/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR driver's data sequencer programs that fill a shader's
 * registers before it runs: the shared register program, built from the
 * compiler's request table (descriptors from set memory, push and blend
 * constants from their buffers, addresses and counts written as they
 * are), and the vertex program, which loads each vertex's attributes
 * into vertex input registers, writes the vertex and instance index the
 * shader reads and issues the vertex shader.  This part only computes; it
 * holds no lock and no memory, and it builds on the host too.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_DATA_PROGRAM_H
#define KERN_DRIVERS_GPU_POWERVR_DATA_PROGRAM_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/gpu/powervr/descriptor-set.h"
#include "drivers/gpu/powervr/compiler/shader.h"

/* The most sets a shader reads. */
#define POWERVR_DATA_SETS		4U

/*
 * The most loads of a shared register program: each takes a 64-bit and a
 * 32-bit constant, and the 32-bit constants are numbered to 127.
 */
#define POWERVR_DATA_LOADS_MAX		42U

/*
 * The most data and code words of a shared register program: three and
 * one a load, and two and one for the issue of a shared task.
 */
#define POWERVR_DATA_SHARED_DATA_MAX	(POWERVR_DATA_LOADS_MAX * 3U + 2U)
#define POWERVR_DATA_SHARED_CODE_MAX	(POWERVR_DATA_LOADS_MAX + 1U)

/* The most attribute loads of a vertex program, and its data and code words. */
#define POWERVR_DATA_VERTEX_DMAS_MAX	POWERVR_SHADER_INPUTS
#define POWERVR_DATA_VERTEX_DATA_MAX	(POWERVR_DATA_VERTEX_DMAS_MAX * 8U + 8U)
#define POWERVR_DATA_VERTEX_CODE_MAX	(POWERVR_DATA_VERTEX_DMAS_MAX + 4U)

/* How a load fills shared registers: copied from memory, or written as given. */
enum powervr_data_load_kind {
	POWERVR_DATA_LOAD_DMA = 0,
	POWERVR_DATA_LOAD_WRITE
};

/*
 * One load of a shared register program: words registers from shared
 * on.  A copy reads them from the address source; a write gives one or
 * two words in value (the first in its low half), and two only to an
 * even register.
 */
struct powervr_data_load {
	uint8_t kind;
	uint16_t shared;
	uint16_t words;
	uint64_t source;
};

/*
 * What the bound state gives the shared register requests: each set's
 * layout and the device address of its memory, the device addresses of
 * the push constants' bytes and of the four blend constants, and a
 * compute task's workgroup id and workgroup count.
 */
struct powervr_data_state {
	const struct powervr_set_layout *layouts[POWERVR_DATA_SETS];
	uint64_t set_addresses[POWERVR_DATA_SETS];
	uint64_t push_address;
	uint64_t blend_address;
	uint32_t workgroup_id[3];
	uint32_t workgroups[3];
};

/*
 * One attribute load of a vertex program: the device address of the
 * attribute of vertex (or instance) 0, the bytes between vertices (or
 * instances), the words of one attribute, the first vertex input
 * register they land in, and whether the instance index rather than the
 * vertex index picks the element (a divisor of one).
 */
struct powervr_data_vertex_dma {
	uint64_t address;
	uint32_t stride;
	uint32_t words;
	uint32_t reg;
	bool instance;
};

/* A vertex program: its data and code words and the data sequencer temporaries it uses. */
struct powervr_data_vertex_program {
	uint32_t data[POWERVR_DATA_VERTEX_DATA_MAX];
	uint32_t code[POWERVR_DATA_VERTEX_CODE_MAX];
	uint32_t data_words;
	uint32_t code_words;
	uint32_t temps;
};

/* A shared register program: its data and code words. */
struct powervr_data_shared_program {
	uint32_t data[POWERVR_DATA_SHARED_DATA_MAX];
	uint32_t code[POWERVR_DATA_SHARED_CODE_MAX];
	uint32_t data_words;
	uint32_t code_words;
};

int powervr_data_resolve(const struct powervr_shader_program *program, const struct powervr_data_state *state, struct powervr_data_load *loads, uint32_t room, uint32_t *count);
int powervr_data_shared_build(const struct powervr_data_load *loads, uint32_t count, struct powervr_data_shared_program *result);
int powervr_data_shared_build_task(const struct powervr_data_load *loads, uint32_t count, const uint64_t *task, struct powervr_data_shared_program *result);
int powervr_data_vertex_build(const struct powervr_data_vertex_dma *dmas, uint32_t count, uint32_t vertex_index_reg, uint32_t instance_index_reg, uint64_t task_word, struct powervr_data_vertex_program *result);

#endif /* KERN_DRIVERS_GPU_POWERVR_DATA_PROGRAM_H */
