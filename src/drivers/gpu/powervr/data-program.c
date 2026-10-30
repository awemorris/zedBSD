/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR driver's data sequencer programs that fill a shader's
 * registers: the shared register program from the compiler's request
 * table and the bound state, and the vertex program that loads the
 * attributes and the vertex and instance index.  Nothing here touches
 * the core or memory; the executor calls it with device addresses, and
 * the host check builds it unchanged.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/powervr/data-program.h"
#include "drivers/gpu/powervr/compiler/pds-encode.h"

/*
 * The 32-bit control constant of a data sequencer write.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_DOUT_FIELDS_DOUTW_SRC1_LAST_EN 0x80000000,
 * _DEST_COMMON_STORE 0x10000000 with _UNIFIED_STORE 0, _CMODE_CACHED 0,
 * _AO_SHIFT 13, _BSIZE_LOWER 0, _UPPER 1, _ALL64 2).
 */
#define WRITE_LAST			0x80000000U
#define WRITE_COMMON_STORE		0x10000000U
#define WRITE_ADDRESS_SHIFT		13U
#define WRITE_LOWER			0U
#define WRITE_UPPER			1U
#define WRITE_ALL64			2U

/* The largest store address and DMA size the control constants hold. */
#define STORE_ADDRESS_MAX		0x1fffU
#define DMA_WORDS_MAX			0xfffU

/*
 * The 64-bit data sequencer temporary the vertex data master fills: the
 * vertex index in its low half and the instance index in its high half,
 * as an operand of the 64-bit space (temporaries from 64), and as the
 * index operand of a DMA multiply-add (temporaries from 0).
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_REGS64_TEMP64_LOWER 64,
 * PVR_ROGUE_PDSINST_REGS32T_TEMP32_LOWER 0).
 */
#define VERTEX_INDEX_TEMP64		64U
#define VERTEX_INDEX_TEMP		0U
#define INSTANCE_INDEX_TEMP		1U
#define VERTEX_TEMPS			2U

/*
 * The constants of one attribute load: eight 32-bit words per load, the
 * stride at word 3, the address at words 4 and 5, the control at words 6
 * and 7.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_pipeline_pds.c
 * (pvr_pds_generate_vertex_primary_program: const_base = dma *
 * PVR_PDS_DDMAD_NUM_CONSTS, stride at const_base + 3, address at
 * const_base + 4, control at const_base + 6, zero at const_base + 7) and
 * pvr_pds.h (PVR_PDS_DDMAD_NUM_CONSTS 8).
 */
#define DMA_CONSTANTS			8U
#define DMA_STRIDE			3U
#define DMA_ADDRESS			4U
#define DMA_CONTROL			6U
#define DMA_GROUP_USED			0xf8U

/* The groups of eight constants a vertex program may use. */
#define VERTEX_GROUPS			(POWERVR_DATA_VERTEX_DATA_MAX / DMA_CONSTANTS)

static int load_add(struct powervr_data_load *loads, uint32_t room, uint32_t *count, uint8_t kind, uint32_t shared, uint32_t words, uint64_t source);
static int write_words(struct powervr_data_load *loads, uint32_t room, uint32_t *count, uint32_t shared, const uint32_t *values, uint32_t words);
static int resolve_descriptor(const struct powervr_shader_request *request, const struct powervr_data_state *state, uint64_t *address);
static int find_constant(uint8_t usage[VERTEX_GROUPS], uint32_t *index);
static int vertex_write(struct powervr_data_vertex_program *result, uint8_t usage[VERTEX_GROUPS], uint32_t reg, uint32_t half, bool last);

/*
 * Works out the loads that fill a program's shared registers from the
 * bound state, in the order of its request table.
 *
 * A set's base and a literal, the workgroup id and the workgroup count
 * are written as they are; a descriptor's words are copied from the
 * set's memory, the push constants and the blend constants from their
 * buffers.  Refuses a request whose set, binding or buffer the state
 * does not have, and a request kind the driver does not fill (ENOTSUP).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_pipeline.c
 * (pvr_pds_descriptor_program_create_and_upload: a DMA from each used
 * set's memory, then the push constants and blend constants buffers, to
 * the shared registers the compiler chose).
 */
int
powervr_data_resolve(
	const struct powervr_shader_program *program,
	const struct powervr_data_state *state,
	struct powervr_data_load *loads,
	uint32_t room,
	uint32_t *count)
{
	const struct powervr_shader_request *request;
	uint32_t values[3];
	uint64_t address;
	uint32_t index;
	int error;

	/* Refuses a missing program, state or result. */
	if (program == NULL || state == NULL || loads == NULL || count == NULL)
		return EINVAL;
	if (program->request_count > POWERVR_SHADER_REQUESTS)
		return EINVAL;

	/* Each request's loads; a request of no words has none. */
	*count = 0;
	for (index = 0; index < program->request_count; index++) {
		request = &program->requests[index];
		if (request->words == 0)
			continue;

		switch (request->kind) {
		case POWERVR_SHADER_REQUEST_SET_BASE:
			/* The set's memory's address, low word first. */
			if (request->set >= POWERVR_DATA_SETS || request->words != 2)
				return EINVAL;
			address = state->set_addresses[request->set];
			values[0] = (uint32_t)address;
			values[1] = (uint32_t)(address >> 32);
			error = write_words(loads, room, count, request->shared, values, 2);
			break;
		case POWERVR_SHADER_REQUEST_DESCRIPTOR:
			/* The descriptor's part, copied from the set's memory. */
			error = resolve_descriptor(request, state, &address);
			if (error == 0)
				error = load_add(loads, room, count, POWERVR_DATA_LOAD_DMA, request->shared, request->words, address);
			break;
		case POWERVR_SHADER_REQUEST_PUSH_CONSTANTS:
			/* The range of the push constants' bytes. */
			if (state->push_address == 0 || (request->offset & 3U) != 0)
				return EINVAL;
			error = load_add(loads, room, count, POWERVR_DATA_LOAD_DMA, request->shared, request->words, state->push_address + request->offset);
			break;
		case POWERVR_SHADER_REQUEST_LITERAL:
			/* The constant as it is. */
			if (request->words != 1)
				return EINVAL;
			values[0] = request->literal;
			error = write_words(loads, room, count, request->shared, values, 1);
			break;
		case POWERVR_SHADER_REQUEST_WORKGROUP_ID:
		case POWERVR_SHADER_REQUEST_NUM_WORKGROUPS:
			/* Up to three counts as they are. */
			if (request->words > 3)
				return EINVAL;
			if (request->kind == POWERVR_SHADER_REQUEST_WORKGROUP_ID) {
				values[0] = state->workgroup_id[0];
				values[1] = state->workgroup_id[1];
				values[2] = state->workgroup_id[2];
			} else {
				values[0] = state->workgroups[0];
				values[1] = state->workgroups[1];
				values[2] = state->workgroups[2];
			}
			error = write_words(loads, room, count, request->shared, values, request->words);
			break;
		case POWERVR_SHADER_REQUEST_BLEND_CONSTANTS:
			/* The four floats, copied from their buffer. */
			if (state->blend_address == 0 || request->words != 4)
				return EINVAL;
			error = load_add(loads, room, count, POWERVR_DATA_LOAD_DMA, request->shared, 4, state->blend_address);
			break;
		case POWERVR_SHADER_REQUEST_COMPUTED:
			/* The program's shared program writes these words itself. */
			error = 0;
			break;
		default:
			return ENOTSUP;
		}

		if (error != 0)
			return error;
	}

	/* Succeeded: the loads in order. */
	return 0;
}

/*
 * Builds a shared register program from its loads: a data output per
 * load, each from a 64-bit constant (the address or the written words)
 * and a 32-bit control constant, the last one marked and ending the
 * program.  A program of no loads is a halt.
 *
 * The 64-bit constants come first, in the order of the loads, and the
 * 32-bit ones after them.
 */
int
powervr_data_shared_build(
	const struct powervr_data_load *loads,
	uint32_t count,
	struct powervr_data_shared_program *result)
{
	int error;

	/* The program without a shared task. */
	error = powervr_data_shared_build_task(loads, count, NULL, result);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Builds a shared register program from its loads and, when task is not
 * NULL, the issue of a shared task after them (the fragment program's
 * shared program, which computes words into the shared
 * registers from the ones the loads fill): the last load is still
 * marked, but the issue ends the program.  The issue's task word is the
 * 64-bit constant after the loads'.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_pipeline_pds.c
 * (pvr_pds_generate_descriptor_upload_program: data_size_in_dwords =
 * consts64 * 2 + consts32, next_const32 starting at consts64 * 2, the
 * last DMA with LAST_EN and the halt bit unless secondary_program_present,
 * then for it a 64-bit constant and pvr_pds_encode_doutu without cc and
 * with end, a halt when empty and must_not_be_empty; pvr_encode_burst:
 * the DOUTD control of size, address, cached common store;
 * pvr_encode_direct_write: the DOUTW control with the store, address and
 * write mask).
 */
int
powervr_data_shared_build_task(
	const struct powervr_data_load *loads,
	uint32_t count,
	const uint64_t *task,
	struct powervr_data_shared_program *result)
{
	const struct powervr_data_load *load;
	enum powervr_pds_output output;
	uint64_t address;
	uint32_t control;
	uint32_t index;
	uint32_t constants64;
	uint32_t constant32;
	unsigned last;
	unsigned end;
	int error;

	/* Refuses a missing table or result, and more loads than the constants hold. */
	if ((count != 0 && loads == NULL) || result == NULL)
		return EINVAL;
	if (count > POWERVR_DATA_LOADS_MAX)
		return E2BIG;

	/* No load and no task: the program only halts. */
	if (count == 0 && task == NULL) {
		result->data_words = 0;
		error = powervr_pds_halt(0, &result->code[0]);
		if (error != 0)
			return error;
		result->code_words = 1;
		return 0;
	}

	/* The 64-bit constants: one a load, and the task word's. */
	constants64 = count;
	if (task != NULL)
		constants64++;

	/* Each load's constants and its output. */
	for (index = 0; index < count; index++) {
		load = &loads[index];
		last = index + 1U == count ? 1U : 0U;
		end = last;
		if (task != NULL)
			end = 0;
		constant32 = constants64 * 2U + index;
		if (load->shared > STORE_ADDRESS_MAX || load->words == 0)
			return EINVAL;

		if (load->kind == POWERVR_DATA_LOAD_DMA) {
			/* The source address and the copy's control. */
			error = powervr_pds_dma_address_word(load->source, 0, &address);
			if (error != 0)
				return error;
			if (load->words > DMA_WORDS_MAX)
				return EINVAL;
			error = powervr_pds_dma_control_word(last, 1, 1, 0, load->shared, 0, load->words, &control);
			if (error != 0)
				return error;
			output = POWERVR_PDS_OUTPUT_DMA;
		} else if (load->kind == POWERVR_DATA_LOAD_WRITE) {
			/* The words and the write's control: one word from the low half, or both to an even register. */
			if (load->words > 2 || (load->words == 2 && (load->shared & 1U) != 0))
				return EINVAL;
			address = load->source;
			control = load->words == 2 ? WRITE_ALL64 : WRITE_LOWER;
			control |= WRITE_COMMON_STORE;
			control |= (uint32_t)load->shared << WRITE_ADDRESS_SHIFT;
			if (last != 0)
				control |= WRITE_LAST;
			output = POWERVR_PDS_OUTPUT_WRITE;
		} else {
			return EINVAL;
		}

		/* The constants in their places. */
		result->data[index * 2U] = (uint32_t)address;
		result->data[index * 2U + 1U] = (uint32_t)(address >> 32);
		result->data[constant32] = control;

		/* The output from the 64-bit constant and the 32-bit one; the last ends the program unless a task follows. */
		error = powervr_pds_output(0, end, output, index, constant32, &result->code[index]);
		if (error != 0)
			return error;
	}

	/* The shared task's issue from its constant, ending the program. */
	if (task != NULL) {
		result->data[count * 2U] = (uint32_t)*task;
		result->data[count * 2U + 1U] = (uint32_t)(*task >> 32);
		error = powervr_pds_output(0, 1, POWERVR_PDS_OUTPUT_SHADER_TASK, count, 0, &result->code[count]);
		if (error != 0)
			return error;
	}

	/* Succeeded: the constants and a code word a load and for the issue. */
	result->data_words = constants64 * 2U + count;
	result->code_words = constants64;
	return 0;
}

/*
 * Builds a vertex program: a DMA multiply-add per attribute from the
 * vertex index (or the instance index for an attribute stepped per
 * instance), a write of the vertex index and of the instance index
 * to the registers the shader reads them from (POWERVR_SHADER_NO_REG
 * for none), then the vertex shader's issue from the task word.
 *
 * A per instance attribute steps once an instance (a divisor of one,
 * from instance 0); other divisors, a base instance and robust bounds
 * are not taken here.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_pipeline_pds.c
 * (pvr_pds_generate_vertex_primary_program without robustness, draw
 * indirect or base instance: temporaries 0 and 1 the index and instance
 * the vertex data master fills, constants 0 and 1 kept for the task
 * word, eight constants per DDMAD with the zero upper control word of
 * a DDMADT without a test, the DDMAD from the index temporary, or from
 * the instance temporary for an instance rate DMA of divisor 1, the
 * DOUTWs of the vertex id from the low half and the instance id from
 * the high half of the 64-bit temporary 0 with controls found by
 * pvr_find_constant, LAST_EN on the last DMA, the DOUTU with cc and
 * end, a halt, and the data size from the used constants).
 */
int
powervr_data_vertex_build(
	const struct powervr_data_vertex_dma *dmas,
	uint32_t count,
	uint32_t vertex_index_reg,
	uint32_t instance_index_reg,
	uint64_t task_word,
	struct powervr_data_vertex_program *result)
{
	uint8_t usage[VERTEX_GROUPS];
	const struct powervr_data_vertex_dma *dma;
	uint64_t address;
	uint64_t control64;
	uint32_t control;
	uint32_t base;
	uint32_t index;
	uint32_t total;
	uint32_t done;
	uint32_t code;
	uint32_t temp;
	uint32_t top;
	int error;

	/* Refuses a missing table or result, and more attributes than the constants hold. */
	if ((count != 0 && dmas == NULL) || result == NULL)
		return EINVAL;
	if (count > POWERVR_DATA_VERTEX_DMAS_MAX)
		return E2BIG;

	/* No constant used but the task word's two. */
	for (index = 0; index < VERTEX_GROUPS; index++)
		usage[index] = 0;
	for (index = 0; index < POWERVR_DATA_VERTEX_DATA_MAX; index++)
		result->data[index] = 0;
	usage[0] = 0x03U;
	result->data[0] = (uint32_t)task_word;
	result->data[1] = (uint32_t)(task_word >> 32);

	/* Every attribute load's group of constants. */
	for (index = 0; index < count; index++)
		usage[index] |= DMA_GROUP_USED;

	/* The DMAs and the writes; the last of them is marked. */
	total = count;
	if (vertex_index_reg != POWERVR_SHADER_NO_REG)
		total++;
	if (instance_index_reg != POWERVR_SHADER_NO_REG)
		total++;
	done = 0;
	code = 0;

	/* Each attribute: its stride, its address and its control, then the DMA. */
	for (index = 0; index < count; index++) {
		dma = &dmas[index];
		base = index * DMA_CONSTANTS;
		done++;
		if (dma->words == 0 || dma->words > DMA_WORDS_MAX || dma->reg > STORE_ADDRESS_MAX)
			return EINVAL;

		error = powervr_pds_dma_address_word(dma->address, 0, &address);
		if (error != 0)
			return error;
		error = powervr_pds_dma_control_word(done == total ? 1U : 0U, 1, 0, 0, dma->reg, 0, dma->words, &control);
		if (error != 0)
			return error;
		error = powervr_pds_dma_add_control_word(control, 0, 0, &control64);
		if (error != 0)
			return error;

		result->data[base + DMA_STRIDE] = dma->stride;
		result->data[base + DMA_ADDRESS] = (uint32_t)address;
		result->data[base + DMA_ADDRESS + 1U] = (uint32_t)(address >> 32);
		result->data[base + DMA_CONTROL] = (uint32_t)control64;
		result->data[base + DMA_CONTROL + 1U] = (uint32_t)(control64 >> 32);

		/* The address is the base plus the vertex (or instance) index times the stride. */
		temp = VERTEX_INDEX_TEMP;
		if (dma->instance)
			temp = INSTANCE_INDEX_TEMP;
		error = powervr_pds_dma_add(0, 0, base + DMA_STRIDE, temp, (base + DMA_ADDRESS) >> 1, (base + DMA_CONTROL) >> 1, &result->code[code]);
		if (error != 0)
			return error;
		code++;
	}
	result->code_words = code;

	/* The vertex index from the low half of the temporary. */
	if (vertex_index_reg != POWERVR_SHADER_NO_REG) {
		done++;
		error = vertex_write(result, usage, vertex_index_reg, WRITE_LOWER, done == total);
		if (error != 0)
			return error;
	}

	/* The instance index from the high half. */
	if (instance_index_reg != POWERVR_SHADER_NO_REG) {
		done++;
		error = vertex_write(result, usage, instance_index_reg, WRITE_UPPER, done == total);
		if (error != 0)
			return error;
	}

	/* The vertex shader's issue, ending the program, then a halt. */
	code = result->code_words;
	error = powervr_pds_output(1, 1, POWERVR_PDS_OUTPUT_SHADER_TASK, 0, 0, &result->code[code]);
	if (error != 0)
		return error;
	error = powervr_pds_halt(0, &result->code[code + 1U]);
	if (error != 0)
		return error;
	result->code_words = code + 2U;

	/* The data words reach the highest used constant of the last group in use. */
	result->data_words = 0;
	for (index = 0; index < VERTEX_GROUPS && usage[index] != 0; index++) {
		top = 8;
		while ((usage[index] & (1U << (top - 1U))) == 0)
			top--;
		result->data_words = index * DMA_CONSTANTS + top;
	}

	/* Succeeded: the two temporaries the data master fills. */
	result->temps = VERTEX_TEMPS;
	return 0;
}

/* Appends one load, refusing a full table or a load the controls cannot hold. */
static int
load_add(
	struct powervr_data_load *loads,
	uint32_t room,
	uint32_t *count,
	uint8_t kind,
	uint32_t shared,
	uint32_t words,
	uint64_t source)
{
	struct powervr_data_load *load;

	/* Refuses a full table and a register or size past the fields. */
	if (*count >= room)
		return E2BIG;
	if (shared > STORE_ADDRESS_MAX || words == 0 || words > DMA_WORDS_MAX)
		return EINVAL;

	/* The load in the next entry. */
	load = &loads[*count];
	load->kind = kind;
	load->shared = (uint16_t)shared;
	load->words = (uint16_t)words;
	load->source = source;
	(*count)++;

	/* Succeeded: the load is in the table. */
	return 0;
}

/*
 * Appends the writes of words given values from shared on: two words
 * at a time to an even register, one at a time otherwise.
 */
static int
write_words(
	struct powervr_data_load *loads,
	uint32_t room,
	uint32_t *count,
	uint32_t shared,
	const uint32_t *values,
	uint32_t words)
{
	uint64_t value;
	uint32_t index;
	uint32_t step;
	int error;

	/* Each write, a pair where the register allows it. */
	for (index = 0; index < words; index += step) {
		step = 1;
		value = values[index];
		if (((shared + index) & 1U) == 0 && index + 1U < words) {
			step = 2;
			value |= (uint64_t)values[index + 1U] << 32;
		}

		error = load_add(loads, room, count, POWERVR_DATA_LOAD_WRITE, shared + index, step, value);
		if (error != 0)
			return error;
	}

	/* Succeeded: the words are in the table. */
	return 0;
}

/* Works out the address of the part of a descriptor a request names, in its set's memory. */
static int
resolve_descriptor(
	const struct powervr_shader_request *request,
	const struct powervr_data_state *state,
	uint64_t *address)
{
	const struct powervr_set_layout *layout;
	uint32_t offset;
	int error;

	/* A set the state has. */
	if (request->set >= POWERVR_DATA_SETS)
		return EINVAL;
	layout = state->layouts[request->set];
	if (layout == NULL || state->set_addresses[request->set] == 0)
		return EINVAL;

	/* The part's place, and room for its words in the set. */
	error = powervr_set_descriptor_offset(layout, request->binding, request->array_index, request->part, &offset);
	if (error != 0)
		return error;
	if ((uint64_t)offset + (uint64_t)request->words * 4U > layout->bytes)
		return EINVAL;

	/* Succeeded: the set's address plus the place. */
	*address = state->set_addresses[request->set] + offset;
	return 0;
}

/*
 * Finds the first free 32-bit constant in groups of eight, marking it
 * used.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_pipeline_pds.c
 * (pvr_find_constant2 with one word: the first clear bit of the first
 * group that has one).
 */
static int
find_constant(
	uint8_t usage[VERTEX_GROUPS],
	uint32_t *index)
{
	uint32_t group;
	uint32_t bit;

	/* The first clear bit, group by group. */
	for (group = 0; group < VERTEX_GROUPS; group++) {
		for (bit = 0; bit < DMA_CONSTANTS; bit++) {
			if ((usage[group] & (1U << bit)) != 0)
				continue;
			usage[group] |= (uint8_t)(1U << bit);
			*index = group * DMA_CONSTANTS + bit;
			return 0;
		}
	}

	/* No constant is free. */
	return E2BIG;
}

/* Adds a vertex program's write of one half of the index temporary to a vertex input register. */
static int
vertex_write(
	struct powervr_data_vertex_program *result,
	uint8_t usage[VERTEX_GROUPS],
	uint32_t reg,
	uint32_t half,
	bool last)
{
	uint32_t constant;
	uint32_t control;
	int error;

	/* Refuses a register past the field. */
	if (reg > STORE_ADDRESS_MAX)
		return EINVAL;

	/* The control in a free constant: the unified store, the register, the half. */
	error = find_constant(usage, &constant);
	if (error != 0)
		return error;
	control = half | (reg << WRITE_ADDRESS_SHIFT);
	if (last)
		control |= WRITE_LAST;
	result->data[constant] = control;

	/* The write from the 64-bit temporary. */
	error = powervr_pds_output(0, 0, POWERVR_PDS_OUTPUT_WRITE, VERTEX_INDEX_TEMP64, constant, &result->code[result->code_words]);
	if (error != 0)
		return error;
	result->code_words++;

	/* Succeeded: the write is in the program. */
	return 0;
}
