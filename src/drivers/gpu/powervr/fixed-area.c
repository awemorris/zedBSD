/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The programs of the fixed area of a PowerVR memory context, written
 * into the pages that back it.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/powervr/fixed-area.h"
#include "drivers/gpu/powervr/compiler/pds-encode.h"
#include "drivers/gpu/powervr/compiler/usc-encode.h"

/*
 * The 32-bit words the synchronisation program's data takes before its
 * code, and the words of its code.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_pds.c
 * (ROGUE_PDS_FIXED_PIXEL_SHADER_DATA_SIZE 8, the data size of a task
 * issue program whose data and code are written together; one code
 * word).
 */
#define SYNC_DATA_WORDS		8U
#define SYNC_CODE_WORDS		1U

/*
 * The vertex output signal that emits the vertex and ends the task.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py (F_UVSW_OP emit_endtask
 * 0b101).
 */
#define USC_EMIT_AND_END	5U

/*
 * A group with no operation and no operand, a repeat count of one: the
 * start every group the file builds is filled in from.
 */
static const struct powervr_usc_group blank_group = {
	.repeat = 1
};

static int sync_data_sequencer(volatile uint8_t *page);
static int tile_end_data_sequencer(volatile uint8_t *page);
static int sync_shader(volatile uint8_t *page);
static void put_word(volatile uint8_t *page, uint32_t offset, uint32_t word);

/*
 * Writes the fixed area's programs into the zeroed pages that back the
 * data sequencer heap's and the shading cluster heap's fixed areas.
 *
 * The general heap's page needs nothing written: the fence's contents
 * are never read, and the colour coefficients are only read by a
 * sampler converting from a luma and chroma format, which this driver
 * does not offer, so both stay zero.
 */
int
powervr_fixed_area_write(
	volatile uint8_t *pds_page,
	volatile uint8_t *usc_page)
{
	int error;

	/* Refuses a missing page. */
	if (pds_page == NULL || usc_page == NULL)
		return EINVAL;

	/* The data sequencer program that starts the synchronisation task. */
	error = sync_data_sequencer(pds_page);
	if (error != 0)
		return error;

	/* The end-of-tile code. */
	error = tile_end_data_sequencer(pds_page);
	if (error != 0)
		return error;

	/* The shading cluster program the synchronisation task runs. */
	error = sync_shader(usc_page);
	if (error != 0)
		return error;

	/* Succeeded: the pages hold the fixed area's programs. */
	return 0;
}

/*
 * Writes the synchronisation task's data sequencer program, data then
 * code, at its place.
 *
 * Its one data word issues the shading cluster task at the start of the
 * shading cluster heap's fixed area, with no temporaries and the
 * instance rate; its code issues that task and ends.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/winsys/pvr_winsys_helper.c
 * (pvr_setup_static_vdm_sync: the task word for the program at the
 * shading cluster heap's synchronisation offset with the instance
 * sample rate, then the task issue program with its data and code
 * together); src/imagination/vulkan/pds/pvr_pds.c (pvr_pds_kick_usc:
 * the 64-bit task word as the first constant, the code after the eight
 * data words, a single task issue from that constant that ends the
 * program; pvr_pds_setup_doutu: no temporaries field when none are
 * used).
 */
static int
sync_data_sequencer(
	volatile uint8_t *page)
{
	uint64_t task;
	uint32_t code;
	unsigned index;
	int error;

	/* The task word: the program's offset in its heap, no temporaries, the instance rate, one phase. */
	error = powervr_pds_task_word(POWERVR_FIXED_SYNC_USC_OFFSET, 0, 0, 0, &task);
	if (error != 0)
		return error;

	/* The single code word: the task issue from the first 64-bit constant, ending the program. */
	error = powervr_pds_output(0, 1, POWERVR_PDS_OUTPUT_SHADER_TASK, 0, 0, &code);
	if (error != 0)
		return error;

	/* The data: the task word in the first two words, the rest zero. */
	put_word(page, POWERVR_FIXED_SYNC_PDS_OFFSET, (uint32_t)task);
	put_word(page, POWERVR_FIXED_SYNC_PDS_OFFSET + 4U, (uint32_t)(task >> 32));
	for (index = 2; index < SYNC_DATA_WORDS; index++)
		put_word(page, POWERVR_FIXED_SYNC_PDS_OFFSET + index * 4U, 0);

	/* The code, after the data. */
	put_word(page, POWERVR_FIXED_SYNC_PDS_OFFSET + SYNC_DATA_WORDS * 4U, code);

	/* Succeeded: the program is in place. */
	return 0;
}

/*
 * Writes the end-of-tile code at its place.
 *
 * The code has no pixel back end words to send, so it only issues the
 * task its data names and ends; the data comes with each render.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/winsys/pvr_winsys_helper.c
 * (pvr_setup_static_pixel_event_program: an empty pixel event program,
 * code only); src/imagination/vulkan/pds/pvr_pds.c
 * (pvr_pds_generate_pixel_event: with no emit word pairs, the code is a
 * single task issue from the first 64-bit constant that ends the
 * program).
 */
static int
tile_end_data_sequencer(
	volatile uint8_t *page)
{
	uint32_t code;
	int error;

	/* The task issue from the first 64-bit constant, ending the program. */
	error = powervr_pds_output(0, 1, POWERVR_PDS_OUTPUT_SHADER_TASK, 0, 0, &code);
	if (error != 0)
		return error;

	/* The code, alone at its place. */
	put_word(page, POWERVR_FIXED_TILE_END_OFFSET, code);

	/* Succeeded: the code is in place. */
	return 0;
}

/*
 * Writes the shading cluster program the synchronisation task runs: a
 * vertex program that does nothing, so it only emits and ends its task.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/winsys/pvr_winsys_helper.c
 * (pvr_setup_static_vdm_sync: the common no-operation vertex program);
 * src/imagination/pco/usclib/common.cl (vs_nop_common: an empty body);
 * src/imagination/pco/uscgen/pco_clc.c (get_shader_stage: the vs_
 * prefix makes it a vertex program); src/imagination/pco/pco_end.c
 * (pco_end: a vertex program ends with the emit and end-of-task signal
 * marked as the end); src/imagination/pco/pco_map.py (group_map
 * O_UVSW_EMIT_ENDTASK: a main group with the back end only, always
 * executed, the end set, no atomic, a repeat count of one).
 */
static int
sync_shader(
	volatile uint8_t *page)
{
	struct powervr_usc_group group;
	uint8_t program[POWERVR_FIXED_SYNC_USC_BYTES];
	size_t length;
	size_t index;
	int error;

	/* The one group: the back end's emit and end of the task, marked as the program's end. */
	group = blank_group;
	group.unit = POWERVR_USC_UNIT_MAIN;
	group.condition = POWERVR_USC_RUN_ENABLED;
	group.end = 1;
	error = powervr_usc_vertex_output(USC_EMIT_AND_END, 0, 0, 0, 0, &group.operation[POWERVR_USC_PHASE_BACKEND]);
	if (error != 0)
		return error;

	/* Encodes it, padded for the instruction cache. */
	error = powervr_usc_program_encode(&group, 1, program, sizeof(program), &length);
	if (error != 0)
		return error;

	/* Copies the program to its place. */
	for (index = 0; index < length; index++)
		page[POWERVR_FIXED_SYNC_USC_OFFSET + index] = program[index];

	/* Succeeded: the program is in place. */
	return 0;
}

/* Stores one little-endian word at an offset of a page. */
static void
put_word(
	volatile uint8_t *page,
	uint32_t offset,
	uint32_t word)
{
	/* The bytes from the lowest. */
	page[offset + 0U] = (uint8_t)word;
	page[offset + 1U] = (uint8_t)(word >> 8);
	page[offset + 2U] = (uint8_t)(word >> 16);
	page[offset + 3U] = (uint8_t)(word >> 24);
}
