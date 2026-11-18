/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The driver's own programs for a render context's switch, built with the
 * data sequencer and shading cluster encoders.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/powervr/render-sr-program.h"
#include "drivers/gpu/powervr/compiler/pds-encode.h"
#include "drivers/gpu/powervr/compiler/shader-private.h"
#include "drivers/gpu/powervr/compiler/usc-encode.h"

/*
 * The data write's part of the 64-bit source, and the data sequencer's
 * register numbers the programs name.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_DOUT_FIELDS_DOUTW_SRC1_BSIZE_ALL64 2;
 * PVR_ROGUE_PDSINST_REGS32TP_PTEMP32_LOWER 32, the first persistent
 * temporary in the 32-bit temporaries' numbering, and half of it in the
 * 64-bit one).
 */
#define WRITE_ALL64			2U
#define PERSISTENT_32			32U
#define PERSISTENT_64			16U

/*
 * The most 64-bit words one load moves (seven, the field's limit, less
 * one to keep the next load on a 16-byte boundary) and the most 32-bit
 * words one store moves.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_LD_COUNT8_MASK 7, PVR_ROGUE_PDSINST_ST_COUNT4_MASK
 * 15).
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_pds.c
 * (pvr_pds_generate_single_ldst_instruction: an odd load count lowered
 * by one).
 */
#define LOAD_WORDS_MAX			6U
#define STORE_WORDS_MAX			15U

/*
 * The stream out predicate the resume program clears, the task's sample
 * rate, and the data sequencer's word count of a task issue's own block.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_pds.c
 * (pvr_pds_generate_stream_out_init_program: the clear of mask 0x10;
 * ROGUE_PDS_FIXED_PIXEL_SHADER_DATA_SIZE 8); pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_DOUT_FIELDS_DOUTU_SAMPLE_RATE_INSTANCE 0).
 */
#define STREAM_GLOBAL_PREDICATE		0x10U
#define SAMPLE_INSTANCE			0U
#define TASK_BLOCK_WORDS		8U

/*
 * The fields of the task words in a context's static state.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/vdm.xml (PDS_STATE0:
 * block_type 29..31 with PDS_STATE_UPDATE 1, dm_target 28 with VDM 0,
 * usc_target 25 with ALL 0 and ANY 1, usc_common_size 16..24,
 * usc_unified_size 10..15 in units of 16 bytes, pds_temp_size 6..9,
 * pds_data_size 0..5 in units of 16 bytes; PDS_STATE1: pds_data_addr
 * 4..31 shifted by 4, sd_type 2..3 and sd_next_type 0..1 with PDS 1;
 * PDS_STATE2: pds_code_addr 4..31 shifted by 4);
 * src/imagination/csbgen/rogue/ppp.xml (STATE_STREAM_OUT1: sync 10,
 * pds_data_size 4..9 in units of 16 bytes, pds_temp_size 0..3;
 * STATE_STREAM_OUT2: pds_data_addr 4..31 shifted by 4);
 * src/imagination/csbgen/rogue/cr.xml (VDM_CONTEXT_STORE_TASK0 and
 * RESUME_TASK0: pds_state0 0..31, pds_state1 32..63; _TASK1:
 * pds_state2 0..31; _TASK2: stream_out1 0..31, stream_out2 32..63;
 * VDM_CONTEXT_STATE_BASE and TA_CONTEXT_STATE_BASE: addr 4..39 shifted
 * by 4).
 */
#define STATE_UPDATE_BLOCK		(1U << 29)
#define CDM_PDS_SEQUENCE		(1U << 29)
#define CDM_TARGET_ANY			(1U << 27)
#define CDM_UNIFIED_SHIFT		21U
#define CDM_COMMON_SHARED		(1U << 20)
#define CDM_DATA_SHIFT			1U
#define CDM_COMMON_SHIFT		11U
#define CDM_COMMON_UNIT_BYTES		64U
#define CDM_FENCE			1U
#define TARGET_ANY			(1U << 25)
#define UNIFIED_SHIFT			10U
#define SHARED_TYPE_PDS			((1U << 2) | 1U)
#define STREAM_SYNC			(1U << 10)
#define STREAM_DATA_SHIFT		4U
#define UNIT_BYTES			16U
#define ADDRESS_MASK			0xfffffff0U
#define BASE_MASK			0xfffffffff0ULL

/*
 * The unified store the shared register programs take: four blocks of
 * 16 bytes, enough for the four vertex inputs and kept whole against
 * fragmentation.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_context.c
 * (pvr_ctx_sr_programs_setup: unified_size of 64 bytes).
 */
#define SHARED_UNIFIED_UNITS		4U

/*
 * What the shading cluster programs name: the vertex inputs the data
 * sequencer wrote (the state buffer's address, then its registers'
 * part), the special register holding the task's shared register
 * allocation in blocks of 16 words, the first index register as a
 * destination and the shared registers named through it, and the burst
 * the store and the load move at a time (a store burst of 1024 written
 * as zero).
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/common/pvr_iface.h
 * (PVR_LOAD_STORE_SR_DATA_SIZE_ADDR_LO 0, _HI 1, _STORE_ADDR_LO 2,
 * _HI 3); src/imagination/pco/pco_isa.py (special register
 * sh_alloc_size 103); pco_map.h.py and pco_isa.py (an indexed bank's
 * register: the index bank and the offset shifted by 3, idx 0b101 and
 * shared 0b011); src/imagination/pco/usclib/common.cl
 * (cs_store_sr_1024_common, cs_load_sr_256_common).
 */
#define INPUT_SIZE_LOW			0U
#define INPUT_SIZE_HIGH			1U
#define INPUT_STORE_LOW			2U
#define INPUT_STORE_HIGH		3U
#define SPECIAL_SHARED_SIZE		103U
#define INDEX_REGISTER_0		5U
#define SHARED_THROUGH_INDEX		3U
#define STORE_BURST			1024U
#define LOAD_BURST			256U
#define SHARED_BLOCK_WORDS		16U

/*
 * The compiler's test and branch fields the program uses, with the same
 * numbers.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py (F_TST_OP g 5;
 * F_TST_TYPE u32 5; F_BPRED cc 0).
 */
#define TEST_GREATER			5U

/*
 * The data sequencer's condition inputs, and the condition bit of an
 * instruction.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_PREDICATE_IF0 1, _IF1 2; the CC field at bit 27
 * of every instruction class).
 */
#define PREDICATE_IF0			1U
#define PREDICATE_IF1			2U
#define CONDITION_BIT			(1U << 27)
#define TYPE_UNSIGNED			5U
#define BRANCH_ON_CONDITION		0U

/* The temporaries of the shared register programs. */
#define T_SIZE_ADDRESS			0U
#define T_COUNT				2U
#define T_TEST				3U
#define T_ADDRESS			4U
#define T_LEFT				6U
#define T_BURST				7U
#define T_OFFSET			8U
#define T_ONE				9U
#define T_BLOCK				10U
#define T_BACK				11U
#define T_BYTES				12U

/* The most steps of a shared register program. */
#define SHARED_STEPS_MAX		40U

/* What a step is beyond the compiler's own groups. */
enum shared_step_kind {
	STEP_MACHINE,
	STEP_INDEX,
	STEP_BURST_STORE,
	STEP_BURST_LOAD
};

/* One step of a shared register program: a compiler group or one of the bursts. */
struct shared_step {
	enum shared_step_kind kind;
	struct shader_machine_operation operation;
};

/* A shared register program being written: its steps and where they start. */
struct shared_plan {
	struct shared_step steps[SHARED_STEPS_MAX];
	uint32_t offsets[SHARED_STEPS_MAX];
	unsigned count;
	int error;
};

static void persistent_loads(uint64_t buffer, uint32_t data[POWERVR_PERSISTENT_DATA_WORDS], unsigned *loads);
static int shared_plan_make(unsigned loading, struct shared_plan *plan);
static unsigned shared_add(struct shared_plan *plan, enum shared_step_kind kind, unsigned machine);
static struct shader_machine_register temporary(unsigned index);
static void shared_immediate(struct shared_plan *plan, unsigned destination, uint32_t value, unsigned condition);
static void shared_input(struct shared_plan *plan, unsigned destination, unsigned input);
static void shared_multiply_add(struct shared_plan *plan, unsigned destination, unsigned first, unsigned second, unsigned addend);
static void shared_test_zero(struct shared_plan *plan, unsigned source);
static unsigned shared_branch(struct shared_plan *plan, unsigned condition);
static int shared_group(const struct shared_step *step, int32_t offset, struct powervr_usc_group *group);
static int shared_fetch(void *context, size_t index, struct powervr_usc_group *group);
static uint32_t state0_word(uint32_t target, uint32_t data_bytes);
static uint64_t place_pair(uint32_t data, uint32_t code);
static uint32_t state1_word(uint32_t data);
static uint32_t stream_out1_word(uint32_t data_bytes);

/*
 * Builds the data sequencer program that starts a shared register store
 * or load.
 *
 * The data puts each 64-bit address with its high half first, as the
 * program this follows does; the task's control word sits in a block of
 * its own after the writes.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_context.c
 * (pvr_pds_ctx_sr_program_setup: two 64-bit writes into the unified
 * store, the state buffer's address and that address plus the reserved
 * head, neither the last instruction; the task at the instance rate);
 * src/imagination/vulkan/pds/pvr_pds.c (pvr_pds_generate_doutw: each
 * value and its control word in 64-bit constants of their own, the
 * value through pvr_pds_write_constant64 with its high half first, the
 * control word naming the store address twice the pair's number, the
 * last one marked; pvr_pds_kick_usc: the task word low half first in
 * the block after them, its issue ending the program;
 * pvr_pds_generate_shared_storing_program: the writes, then the issue).
 */
int
powervr_shared_pds_program(
	uint64_t state,
	uint64_t usc_offset,
	unsigned temporaries,
	uint32_t data[POWERVR_SHARED_PDS_DATA_WORDS],
	uint32_t code[POWERVR_SHARED_PDS_CODE_WORDS])
{
	uint64_t addresses[2];
	uint64_t task;
	uint32_t control;
	unsigned pair;
	unsigned index;
	int error;

	/* Refuses a missing result. */
	if (data == NULL || code == NULL)
		return EINVAL;

	/* Clears the data, then the task word in its own block. */
	for (index = 0; index < POWERVR_SHARED_PDS_DATA_WORDS; index++)
		data[index] = 0;
	error = powervr_pds_task_word(usc_offset, temporaries, SAMPLE_INSTANCE, 0, &task);
	if (error != 0)
		return error;

	data[TASK_BLOCK_WORDS] = (uint32_t)task;
	data[TASK_BLOCK_WORDS + 1U] = (uint32_t)(task >> 32);

	/* Each write: the address, then its control word into the next pair of vertex inputs. */
	addresses[0] = state;
	addresses[1] = state + POWERVR_SWITCH_SHARED_RESERVE;
	for (pair = 0; pair < 2U; pair++) {
		data[pair * 4U] = (uint32_t)(addresses[pair] >> 32);
		data[pair * 4U + 1U] = (uint32_t)addresses[pair];
		error = powervr_pds_write_control_word(pair == 1U, 0, 0, pair * 2U, WRITE_ALL64, &control);
		if (error != 0)
			return error;

		data[pair * 4U + 2U] = control;
		error = powervr_pds_output(0, 0, POWERVR_PDS_OUTPUT_WRITE, pair * 2U, pair * 4U + 2U, &code[pair]);
		if (error != 0)
			return error;
	}

	/* The task issue from the block after the writes, ending the program. */
	error = powervr_pds_output(0, 1, POWERVR_PDS_OUTPUT_SHADER_TASK, TASK_BLOCK_WORDS / 2U, 0, &code[2]);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Builds the data sequencer program that stores the persistent
 * temporaries: 32 words in stores of at most 15, a wait, the end.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_pds.c
 * (pvr_pds_generate_stream_out_terminate_program and
 * pvr_pds_generate_single_ldst_instruction: the stores from the first
 * persistent temporary, written through, each source word in a 64-bit
 * constant of its own, then the wait and the halt; the data rounded up
 * to 16 bytes); src/imagination/vulkan/pvr_arch_job_context.c
 * (pvr_pds_pt_store_program_create_and_upload: the whole buffer).
 */
int
powervr_persistent_store_program(
	uint64_t buffer,
	uint32_t data[POWERVR_PERSISTENT_DATA_WORDS],
	uint32_t code[POWERVR_PERSISTENT_STORE_CODE_WORDS])
{
	uint64_t word;
	unsigned remaining;
	unsigned count;
	unsigned store;
	unsigned index;
	int error;

	/* Refuses a missing result or a misaligned buffer. */
	if (data == NULL || code == NULL || (buffer & (POWERVR_SWITCH_ALIGN - 1U)) != 0)
		return EINVAL;

	/* Clears the data. */
	for (index = 0; index < POWERVR_PERSISTENT_DATA_WORDS; index++)
		data[index] = 0;

	/* Each store: its source word, then the instruction naming it. */
	remaining = POWERVR_SWITCH_PERSISTENT_BYTES / 4U;
	index = 0;
	for (store = 0; store < 3U; store++) {
		count = remaining;
		if (count > STORE_WORDS_MAX)
			count = STORE_WORDS_MAX;
		error = powervr_pds_store_word(buffer + (uint64_t)index * 4U, count, 0, PERSISTENT_32 + index, &word);
		if (error != 0)
			return error;

		data[store * 2U] = (uint32_t)word;
		data[store * 2U + 1U] = (uint32_t)(word >> 32);
		error = powervr_pds_store(0, store, &code[store]);
		if (error != 0)
			return error;

		index += count;
		remaining -= count;
	}

	/* The wait for the stores, then the end. */
	error = powervr_pds_wait_data(0, &code[3]);
	if (error != 0)
		return error;

	error = powervr_pds_halt(0, &code[4]);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Builds the data sequencer program that resumes the persistent
 * temporaries: a clear of the global stream predicate, 16 64-bit words
 * in loads of at most six, a wait, the end.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_pds.c
 * (pvr_pds_generate_stream_out_init_program with one buffer, not storing:
 * the clear of the stream out predicate, the loads into the persistent
 * temporaries, cached, then the wait and the halt);
 * src/imagination/vulkan/pvr_arch_job_context.c
 * (pvr_pds_pt_resume_program_create_and_upload: the whole buffer).
 */
int
powervr_persistent_resume_program(
	uint64_t buffer,
	uint32_t data[POWERVR_PERSISTENT_DATA_WORDS],
	uint32_t code[POWERVR_PERSISTENT_RESUME_CODE_WORDS])
{
	unsigned loads;
	unsigned load;
	int error;

	/* Refuses a missing result or a misaligned buffer. */
	if (data == NULL || code == NULL || (buffer & (POWERVR_SWITCH_ALIGN - 1U)) != 0)
		return EINVAL;

	/* The clear of the stream predicate. */
	error = powervr_pds_stream_clear(0, STREAM_GLOBAL_PREDICATE, &code[0]);
	if (error != 0)
		return error;

	/* The loads' source words, then one instruction each. */
	persistent_loads(buffer, data, &loads);
	for (load = 0; load < loads; load++) {
		error = powervr_pds_load(0, load, &code[1U + load]);
		if (error != 0)
			return error;
	}

	/* The wait for the loads, then the end. */
	error = powervr_pds_wait_data(0, &code[4]);
	if (error != 0)
		return error;

	error = powervr_pds_halt(0, &code[5]);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Builds the shading cluster program that stores or loads the task's
 * shared registers.
 *
 * Both read the address of the state buffer's head from their vertex
 * inputs; the store writes the allocation's size there, the load reads
 * it back.  When it is not zero, the registers move in bursts from the
 * address after the head, the last burst only what is left.  The task
 * runs one instance, so a predicate and plain branches carry the loop.
 * The plan is kept in one static block: the driver builds these
 * programs once, while the render node starts and nothing else runs.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/usclib/common.cl (load_store_sr with
 * a block of 1024 words for the store and 256 for the load, the load
 * taking the size it finds); pco_trans_nir.c (dma_ld_shregs_pco,
 * dma_st_shregs_pco: the offset moved into index register 0 and the
 * shared registers named through it; dma_ld_pco, dma_st_pco: the
 * 32-bit load and store); pco_schedule.c (a wait for the data return
 * counter after each memory access).
 */
int
powervr_shared_usc_program(
	unsigned loading,
	uint8_t *output,
	size_t room,
	size_t *length)
{
	static struct shared_plan plan;
	struct powervr_usc_group group;
	size_t size;
	size_t total;
	unsigned index;
	int error;

	/* Refuses a flag that is not a bit. */
	if (loading > 1)
		return EINVAL;

	/* The steps. */
	error = shared_plan_make(loading, &plan);
	if (error != 0)
		return error;

	/* Where each step starts; a branch's size does not depend on its offset. */
	total = 0;
	for (index = 0; index < plan.count; index++) {
		error = shared_group(&plan.steps[index], 0, &group);
		if (error != 0)
			return error;

		error = powervr_usc_group_size(&group, &size);
		if (error != 0)
			return error;

		plan.offsets[index] = (uint32_t)total;
		total += size;
	}

	/* The encoder pads before the last group so the program ends aligned; the last group moves by that. */
	if ((total % POWERVR_USC_PROGRAM_ALIGN) != 0)
		plan.offsets[plan.count - 1U] += (uint32_t)(POWERVR_USC_PROGRAM_ALIGN - total % POWERVR_USC_PROGRAM_ALIGN);

	/* Encodes the steps, the branches with their offsets. */
	error = powervr_usc_program_build(plan.count, shared_fetch, &plan, output, room, length);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Builds one buffer's store and resume task words.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_context.c
 * (pvr_render_ctx_ws_static_state_init: task 0 of the store from the
 * shared register store program for any cluster, of the resume from the
 * load program for all of them, both with no common store or data
 * sequencer temporaries; task 1 the program's code; task 2 the
 * persistent temporaries' program, synchronised;
 * pvr_rogue_get_vdmctrl_pds_state_words and
 * pvr_rogue_get_geom_state_stream_out_words).
 */
void
powervr_switch_task_words(
	const struct powervr_switch_programs *programs,
	struct powervr_switch_tasks *tasks)
{
	uint32_t shared_bytes;
	uint32_t persistent_bytes;

	/* The data sizes the words name, in bytes. */
	shared_bytes = POWERVR_SHARED_PDS_DATA_WORDS * 4U;
	persistent_bytes = POWERVR_PERSISTENT_DATA_WORDS * 4U;

	/* The store: the shared registers' store program, then the persistent temporaries' store. */
	tasks->store[0] = state0_word(TARGET_ANY, shared_bytes);
	tasks->store[0] |= (uint64_t)state1_word(programs->shared_store_data) << 32;
	tasks->store[1] = programs->shared_store_code & ADDRESS_MASK;
	tasks->store[2] = stream_out1_word(persistent_bytes);
	tasks->store[2] |= (uint64_t)(programs->persistent_store_data & ADDRESS_MASK) << 32;

	/* The resume: the shared registers' load program, then the persistent temporaries' resume. */
	tasks->resume[0] = state0_word(0, shared_bytes);
	tasks->resume[0] |= (uint64_t)state1_word(programs->shared_load_data) << 32;
	tasks->resume[1] = programs->shared_load_code & ADDRESS_MASK;
	tasks->resume[2] = stream_out1_word(persistent_bytes);
	tasks->resume[2] |= (uint64_t)(programs->persistent_resume_data & ADDRESS_MASK) << 32;
}

/*
 * Builds the data sequencer program that starts a compute context's
 * shared register store or load.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_PREDICATE_IF0 1, _IF1 2); bxe-4-32.h
 * (has_ern35421, so the task issue is conditional; no
 * compute_morton_capable, so the load takes the store's form).
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_context.c
 * (pvr_pds_compute_ctx_sr_program_create_and_upload: the shared storing
 * program's data, its code behind pvr_pds_generate_compute_barrier_conditional,
 * cc_enable from ERN 35421); src/imagination/vulkan/pds/pvr_pds.c
 * (pvr_pds_generate_compute_barrier_conditional: a branch on IF0 setting
 * IF1 to address 1, a conditional halt, a branch on IF0 setting IF0 to
 * address 1).
 */
int
powervr_compute_shared_pds_program(
	uint64_t state,
	uint64_t usc_offset,
	unsigned temporaries,
	uint32_t data[POWERVR_SHARED_PDS_DATA_WORDS],
	uint32_t code[POWERVR_COMPUTE_SHARED_CODE_WORDS])
{
	uint32_t render_code[POWERVR_SHARED_PDS_CODE_WORDS];
	int error;

	/* The render context's program: the same data, the writes and the issue. */
	error = powervr_shared_pds_program(state, usc_offset, temporaries, data, render_code);
	if (error != 0)
		return error;

	/* The coefficient pass's skip. */
	error = powervr_pds_branch(PREDICATE_IF0, 0, PREDICATE_IF1, 1, &code[0]);
	if (error != 0)
		return error;

	error = powervr_pds_halt(1, &code[1]);
	if (error != 0)
		return error;

	error = powervr_pds_branch(PREDICATE_IF0, 0, PREDICATE_IF0, 1, &code[2]);
	if (error != 0)
		return error;

	/* The writes, then the issue under the condition. */
	code[3] = render_code[0];
	code[4] = render_code[1];
	code[5] = render_code[2] | CONDITION_BIT;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Builds the fence terminate program: a zero 64-bit constant and a zero
 * control word for the fence's unused sources; the coefficient pass's
 * skip, the compute fence, the end.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_pds.c
 * (pvr_pds_generate_fence_terminate_program without the software
 * barrier, pvr_pds_generate_doutc).
 */
int
powervr_compute_terminate_program(
	uint32_t data[POWERVR_COMPUTE_TERMINATE_DATA_WORDS],
	uint32_t code[POWERVR_COMPUTE_TERMINATE_CODE_WORDS])
{
	unsigned index;
	int error;

	/* The zero sources. */
	for (index = 0; index < POWERVR_COMPUTE_TERMINATE_DATA_WORDS; index++)
		data[index] = 0;

	/* The skip, the fence, the end. */
	error = powervr_pds_branch(PREDICATE_IF0, 0, PREDICATE_IF1, 1, &code[0]);
	if (error != 0)
		return error;

	error = powervr_pds_halt(1, &code[1]);
	if (error != 0)
		return error;

	error = powervr_pds_branch(PREDICATE_IF0, 0, PREDICATE_IF0, 1, &code[2]);
	if (error != 0)
		return error;

	error = powervr_pds_output(0, 0, POWERVR_PDS_OUTPUT_COMPUTE_FENCE, 0, 0, &code[3]);
	if (error != 0)
		return error;

	error = powervr_pds_halt(0, &code[4]);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Builds the compute context's static state.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/cr.xml (CDM_CONTEXT_PDS0,
 * CDM_CONTEXT_LOAD_PDS0, CDM_TERMINATE_PDS: data_addr 36..63, code_addr
 * 4..31, each shifted by 4; CDM_CONTEXT_PDS1 and CDM_TERMINATE_PDS1:
 * pds_seq_dep 29, usc_seq_dep 28, target 27, unified_size 21..26,
 * common_shared 20, common_size 11..19, temp_size 7..10, data_size 1..6
 * in units of 16 bytes, fence 0); Linux v7.3
 * drivers/gpu/drm/imagination/pvr_rogue_fwif_shared_check.h (struct
 * rogue_fwif_cdm_registers_cswitch: context_pds0 0, context_pds1 8,
 * terminate_pds 16, terminate_pds1 24, resume_pds0 32, context_pds0_b
 * 40, resume_pds0_b 48).
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_context.c
 * (pvr_compute_ctx_ws_static_state_init: the store programs' places,
 * PDS1 with the data sequencer's dependency, any target, the unified
 * store, the store's data size and the fence; the terminate program's
 * places and PDS1 freeing the common store, any target on a core
 * without Morton order; the load programs' places).
 */
void
powervr_compute_switch_words(
	const struct powervr_compute_switch_programs *programs,
	uint64_t words[POWERVR_COMPUTE_SWITCH_WORDS])
{
	/* The first buffer's store, its control word, the terminate and its control word. */
	words[0] = place_pair(programs->store_data[0], programs->store_code[0]);
	words[1] = CDM_PDS_SEQUENCE | CDM_TARGET_ANY | (SHARED_UNIFIED_UNITS << CDM_UNIFIED_SHIFT) | ((POWERVR_SHARED_PDS_DATA_WORDS * 4U / UNIT_BYTES) << CDM_DATA_SHIFT) | CDM_FENCE;
	words[2] = place_pair(programs->terminate_data, programs->terminate_code);
	words[3] = CDM_PDS_SEQUENCE | CDM_TARGET_ANY | CDM_COMMON_SHARED | ((POWERVR_COMPUTE_TERMINATE_DATA_WORDS * 4U / UNIT_BYTES) << CDM_DATA_SHIFT) | CDM_FENCE;

	/* The first buffer's load, then the second buffer's store and load. */
	words[4] = place_pair(programs->load_data[0], programs->load_code[0]);
	words[5] = place_pair(programs->store_data[1], programs->store_code[1]);
	words[6] = place_pair(programs->load_data[1], programs->load_code[1]);
}

/* Builds the register value of a VDM or TA context state base. */
uint64_t
powervr_switch_base_word(
	uint64_t address)
{
	uint64_t word;

	/* The address keeps its own bits 4 to 39. */
	word = address & BASE_MASK;

	/* Succeeded: the word. */
	return word;
}

/* Puts the source words of the persistent temporaries' loads in the data and counts them. */
static void
persistent_loads(
	uint64_t buffer,
	uint32_t data[POWERVR_PERSISTENT_DATA_WORDS],
	unsigned *loads)
{
	uint64_t word;
	unsigned remaining;
	unsigned count;
	unsigned index;
	unsigned load;

	/* Clears the data. */
	for (index = 0; index < POWERVR_PERSISTENT_DATA_WORDS; index++)
		data[index] = 0;

	/* Each load takes up to six 64-bit words, cached, into the next persistent temporaries. */
	remaining = POWERVR_SWITCH_PERSISTENT_BYTES / 8U;
	index = 0;
	load = 0;
	while (remaining != 0U) {
		count = remaining;
		if (count > LOAD_WORDS_MAX)
			count = LOAD_WORDS_MAX;
		/* The buffer is aligned and short, so the word always builds. */
		(void)powervr_pds_load_word(buffer + (uint64_t)index * 8U, count, 0, PERSISTENT_64 + index, &word);
		data[load * 2U] = (uint32_t)word;
		data[load * 2U + 1U] = (uint32_t)(word >> 32);
		index += count;
		remaining -= count;
		load++;
	}

	/* The count. */
	*loads = load;
}

/*
 * Writes the steps of a shared register program.
 *
 * Temporaries: 0 and 1 the head's address, 2 the size in blocks of 16
 * words, 3 the burst test, 4 and 5 the registers' address, 6 the words
 * left, 7 the burst, 8 the first register of the burst, 9 one, 10 the
 * block, 11 the block negated, 12 the block in bytes.
 */
static int
shared_plan_make(
	unsigned loading,
	struct shared_plan *plan)
{
	struct shader_machine_operation *operation;
	unsigned to_end[3];
	unsigned loop;
	unsigned at;
	uint32_t block;
	unsigned index;

	/* The block each burst moves. */
	plan->count = 0;
	plan->error = 0;
	block = STORE_BURST;
	if (loading)
		block = LOAD_BURST;

	/* The head's address. */
	shared_input(plan, T_SIZE_ADDRESS, INPUT_SIZE_LOW);
	shared_input(plan, T_SIZE_ADDRESS + 1U, INPUT_SIZE_HIGH);

	/* The size: the store reads the allocation and writes it to the head, the load reads it back. */
	if (!loading) {
		at = shared_add(plan, STEP_MACHINE, MACHINE_MOVE_SPECIAL);
		plan->steps[at].operation.destination[0] = temporary(T_COUNT);
		plan->steps[at].operation.source[0].bank = POWERVR_USC_BANK_SPECIAL;
		plan->steps[at].operation.source[0].index = SPECIAL_SHARED_SIZE;
		at = shared_add(plan, STEP_MACHINE, MACHINE_STORE);
		plan->steps[at].operation.source[0] = temporary(T_COUNT);
		plan->steps[at].operation.source[1] = temporary(T_SIZE_ADDRESS);
	} else {
		at = shared_add(plan, STEP_MACHINE, MACHINE_LOAD);
		plan->steps[at].operation.destination[0] = temporary(T_COUNT);
		plan->steps[at].operation.source[0] = temporary(T_SIZE_ADDRESS);
	}
	(void)shared_add(plan, STEP_MACHINE, MACHINE_WAIT);

	/* Nothing allocated: done. */
	shared_test_zero(plan, T_COUNT);
	to_end[0] = shared_branch(plan, POWERVR_USC_RUN_IF_PREDICATE);

	/* The registers' address, the words left, the first register and the constants. */
	shared_input(plan, T_ADDRESS, INPUT_STORE_LOW);
	shared_input(plan, T_ADDRESS + 1U, INPUT_STORE_HIGH);
	shared_immediate(plan, T_ONE, SHARED_BLOCK_WORDS, POWERVR_USC_RUN_ENABLED);
	shared_immediate(plan, T_OFFSET, 0, POWERVR_USC_RUN_ENABLED);
	shared_multiply_add(plan, T_LEFT, T_COUNT, T_ONE, T_OFFSET);
	shared_immediate(plan, T_ONE, 1, POWERVR_USC_RUN_ENABLED);
	shared_immediate(plan, T_BLOCK, block, POWERVR_USC_RUN_ENABLED);
	shared_immediate(plan, T_BACK, 0U - block, POWERVR_USC_RUN_ENABLED);
	shared_immediate(plan, T_BYTES, block * 4U, POWERVR_USC_RUN_ENABLED);

	/* The loop: the predicate says this burst is the last, which moves only what is left. */
	loop = plan->count;
	at = shared_add(plan, STEP_MACHINE, MACHINE_COMPARE);
	operation = &plan->steps[at].operation;
	operation->variant = TEST_GREATER;
	operation->immediate = TYPE_UNSIGNED;
	operation->destination[0] = temporary(T_TEST);
	operation->source[0] = temporary(T_LEFT);
	operation->source[1] = temporary(T_BLOCK);
	shared_test_zero(plan, T_TEST);
	shared_immediate(plan, T_BURST, block == STORE_BURST ? 0U : block, POWERVR_USC_RUN_ENABLED);
	at = shared_add(plan, STEP_MACHINE, MACHINE_MOVE);
	plan->steps[at].operation.condition = POWERVR_USC_RUN_IF_PREDICATE;
	plan->steps[at].operation.destination[0] = temporary(T_BURST);
	plan->steps[at].operation.source[0] = temporary(T_LEFT);

	/* The burst through index register 0, and the wait for it. */
	(void)shared_add(plan, STEP_INDEX, 0);
	(void)shared_add(plan, loading ? STEP_BURST_LOAD : STEP_BURST_STORE, 0);
	(void)shared_add(plan, STEP_MACHINE, MACHINE_WAIT);

	/* What is left (none after the last burst), the next register and the next address. */
	shared_multiply_add(plan, T_LEFT, T_LEFT, T_ONE, T_BACK);
	shared_immediate(plan, T_LEFT, 0, POWERVR_USC_RUN_IF_PREDICATE);
	shared_multiply_add(plan, T_OFFSET, T_OFFSET, T_ONE, T_BLOCK);
	at = shared_add(plan, STEP_MACHINE, MACHINE_ADD64);
	operation = &plan->steps[at].operation;
	operation->destination[0] = temporary(T_ADDRESS);
	operation->destination[1] = temporary(T_ADDRESS + 1U);
	operation->source[0] = temporary(T_ADDRESS);
	operation->source[1] = temporary(T_ADDRESS + 1U);
	operation->source[2] = temporary(T_BYTES);

	/* Out of the loop once nothing is left, else round again. */
	shared_test_zero(plan, T_LEFT);
	to_end[1] = shared_branch(plan, POWERVR_USC_RUN_IF_PREDICATE);
	to_end[2] = shared_branch(plan, POWERVR_USC_RUN_ENABLED);
	plan->steps[to_end[2]].operation.target = loop;

	/* The end, which the branches out reach. */
	at = shared_add(plan, STEP_MACHINE, MACHINE_END);
	for (index = 0; index < 2U; index++)
		plan->steps[to_end[index]].operation.target = at;

	/* A step that did not fit. */
	if (plan->error != 0)
		return plan->error;

	/* Succeeded: the steps. */
	return 0;
}

/* Appends one step of a kind, or notes that there is no room; reports its place. */
static unsigned
shared_add(
	struct shared_plan *plan,
	enum shared_step_kind kind,
	unsigned machine)
{
	struct shared_step *step;
	unsigned at;

	/* No room: the last step is overwritten and the plan fails. */
	if (plan->count >= SHARED_STEPS_MAX) {
		plan->error = E2BIG;
		return SHARED_STEPS_MAX - 1U;
	}

	/* A cleared step of the kind. */
	at = plan->count;
	step = &plan->steps[at];
	*step = (struct shared_step){ 0 };
	step->kind = kind;
	step->operation.kind = (uint8_t)machine;
	plan->count++;

	/* Succeeded: its place. */
	return at;
}

/* Names a temporary as a machine register. */
static struct shader_machine_register
temporary(
	unsigned index)
{
	struct shader_machine_register reg;

	/* The temporary bank and the number. */
	reg.bank = POWERVR_USC_BANK_TEMPORARY;
	reg.index = (uint16_t)index;

	/* Succeeded: the register. */
	return reg;
}

/* Appends a move of a 32-bit value into a temporary, under a condition. */
static void
shared_immediate(
	struct shared_plan *plan,
	unsigned destination,
	uint32_t value,
	unsigned condition)
{
	unsigned at;

	/* The immediate move. */
	at = shared_add(plan, STEP_MACHINE, MACHINE_IMMEDIATE);
	plan->steps[at].operation.condition = (uint8_t)condition;
	plan->steps[at].operation.destination[0] = temporary(destination);
	plan->steps[at].operation.immediate = value;
}

/* Appends a copy of a vertex input into a temporary. */
static void
shared_input(
	struct shared_plan *plan,
	unsigned destination,
	unsigned input)
{
	unsigned at;

	/* The move from the vertex input bank. */
	at = shared_add(plan, STEP_MACHINE, MACHINE_MOVE);
	plan->steps[at].operation.destination[0] = temporary(destination);
	plan->steps[at].operation.source[0].bank = POWERVR_USC_BANK_VERTEX_INPUT;
	plan->steps[at].operation.source[0].index = (uint16_t)input;
}

/* Appends a 32-bit multiply-add of temporaries. */
static void
shared_multiply_add(
	struct shared_plan *plan,
	unsigned destination,
	unsigned first,
	unsigned second,
	unsigned addend)
{
	unsigned at;

	/* The multiply-add. */
	at = shared_add(plan, STEP_MACHINE, MACHINE_INTEGER);
	plan->steps[at].operation.destination[0] = temporary(destination);
	plan->steps[at].operation.source[0] = temporary(first);
	plan->steps[at].operation.source[1] = temporary(second);
	plan->steps[at].operation.source[2] = temporary(addend);
}

/* Appends a test that sets the predicate when a temporary is zero. */
static void
shared_test_zero(
	struct shared_plan *plan,
	unsigned source)
{
	unsigned at;

	/* The zero test. */
	at = shared_add(plan, STEP_MACHINE, MACHINE_TEST_ZERO);
	plan->steps[at].operation.source[0] = temporary(source);
}

/* Appends a branch under a condition; its target is set later.  Reports its place. */
static unsigned
shared_branch(
	struct shared_plan *plan,
	unsigned condition)
{
	unsigned at;

	/* The branch on the group's own condition. */
	at = shared_add(plan, STEP_MACHINE, MACHINE_BRANCH);
	plan->steps[at].operation.condition = (uint8_t)condition;
	plan->steps[at].operation.variant = BRANCH_ON_CONDITION;

	/* Succeeded: its place. */
	return at;
}

/*
 * Builds the group of one step: the compiler's group for its own kinds,
 * and the index move and the bursts here.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_map.py (O_MBYP into an index
 * register; O_LD_REGBL: the address in s0, the burst in s1, the shared
 * registers as s3; O_ST32_REGBL: the shared registers as s0, the burst
 * in s2, the address in s3; both through data return counter 0 with the
 * burst from a register).
 */
static int
shared_group(
	const struct shared_step *step,
	int32_t offset,
	struct powervr_usc_group *group)
{
	int error;

	/* The compiler's kinds. */
	if (step->kind == STEP_MACHINE) {
		error = shader_backend_group(&step->operation, offset, group);
		if (error != 0)
			return error;

		return 0;
	}

	/* A main group with no operand yet. */
	*group = (struct powervr_usc_group){ 0 };
	group->unit = POWERVR_USC_UNIT_MAIN;
	group->repeat = 1;

	/* The step's operands and its operation. */
	if (step->kind == STEP_INDEX) {
		/* Index register 0 from the burst's first register, through the source selector. */
		group->write_0 = 1;
		group->internal[4] = 3;
		group->mux_used = 1;
		group->mux = 0;
		group->source[0].used = 1;
		group->source[0].bank = POWERVR_USC_BANK_TEMPORARY;
		group->source[0].index = T_OFFSET;
		group->destination[0].used = 1;
		group->destination[0].bank = POWERVR_USC_BANK_INDEXED_0;
		group->destination[0].index = INDEX_REGISTER_0;
		error = powervr_usc_single(POWERVR_USC_SINGLE_PASS, 0, 0, &group->operation[POWERVR_USC_PHASE_0]);
	} else if (step->kind == STEP_BURST_LOAD) {
		/* The load of a burst into the shared registers. */
		group->source[0].used = 1;
		group->source[0].bank = POWERVR_USC_BANK_TEMPORARY;
		group->source[0].index = T_ADDRESS;
		group->source[1].used = 1;
		group->source[1].bank = POWERVR_USC_BANK_TEMPORARY;
		group->source[1].index = T_BURST;
		group->source[3].used = 1;
		group->source[3].bank = POWERVR_USC_BANK_INDEXED_0;
		group->source[3].index = SHARED_THROUGH_INDEX;
		error = powervr_usc_load(0, 0, 1, 0, 0, &group->operation[POWERVR_USC_PHASE_BACKEND]);
	} else {
		/* The store of a burst from the shared registers. */
		group->source[0].used = 1;
		group->source[0].bank = POWERVR_USC_BANK_INDEXED_0;
		group->source[0].index = SHARED_THROUGH_INDEX;
		group->source[2].used = 1;
		group->source[2].bank = POWERVR_USC_BANK_TEMPORARY;
		group->source[2].index = T_BURST;
		group->source[3].used = 1;
		group->source[3].bank = POWERVR_USC_BANK_TEMPORARY;
		group->source[3].index = T_ADDRESS;
		error = powervr_usc_store(3, 0, 2, 0, 2, 0, 0, &group->operation[POWERVR_USC_PHASE_BACKEND]);
	}

	/* An operation the encoder refused. */
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/* Hands over one step's group, a branch with its offset to its target. */
static int
shared_fetch(
	void *context,
	size_t index,
	struct powervr_usc_group *group)
{
	const struct shared_plan *plan;
	const struct shared_step *step;
	int32_t offset;
	int error;

	/* A branch's offset is from its own start to its target's. */
	plan = context;
	step = &plan->steps[index];
	offset = 0;
	if (step->kind == STEP_MACHINE && step->operation.kind == MACHINE_BRANCH)
		offset = (int32_t)plan->offsets[step->operation.target] - (int32_t)plan->offsets[index];

	/* The group. */
	error = shared_group(step, offset, group);
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/* Builds a PDS0 word: the data's place in the high half, the code's in the low, each on 16 bytes. */
static uint64_t
place_pair(
	uint32_t data,
	uint32_t code)
{
	uint64_t word;

	/* The two places. */
	word = (uint64_t)(data & ADDRESS_MASK) << 32;
	word |= code & ADDRESS_MASK;

	/* Succeeded: the word. */
	return word;
}

/* Builds task 0's first word: a data sequencer state update for a target with its data's size. */
static uint32_t
state0_word(
	uint32_t target,
	uint32_t data_bytes)
{
	uint32_t word;

	/* The block, the target, the unified store and the data size; no common store or temporaries. */
	word = STATE_UPDATE_BLOCK;
	word |= target;
	word |= SHARED_UNIFIED_UNITS << UNIFIED_SHIFT;
	word |= data_bytes / UNIT_BYTES;

	/* Succeeded: the word. */
	return word;
}

/* Builds task 0's second word: the data's place, loaded by the data sequencer, as the next. */
static uint32_t
state1_word(
	uint32_t data)
{
	uint32_t word;

	/* The place and the two loader types. */
	word = data & ADDRESS_MASK;
	word |= SHARED_TYPE_PDS;

	/* Succeeded: the word. */
	return word;
}

/* Builds task 2's first word: a synchronised stream program with its data's size. */
static uint32_t
stream_out1_word(
	uint32_t data_bytes)
{
	uint32_t word;

	/* The sync and the data size; no temporaries. */
	word = STREAM_SYNC;
	word |= (data_bytes / UNIT_BYTES) << STREAM_DATA_SHIFT;

	/* Succeeded: the word. */
	return word;
}

/*
 * Builds the compute command's resume control word, CDM_CONTEXT_PDS1 for
 * the load of the shared registers when the context comes back: no
 * dependency, any target off, the switch's unified store, the common
 * store of common_bytes shared, the load program's data.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/cr.xml (CDM_CONTEXT_PDS1:
 * unified_size 21..26, common_shared 20, common_size 11..19 in units of
 * 64 bytes, data_size 1..6 in units of 16 bytes); Linux v7.3
 * drivers/gpu/drm/imagination/pvr_rogue_fwif_client_check.h (struct
 * rogue_fwif_compute_regs: cdm_resume_pds1 52, after the command's
 * 8-byte head).
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_compute.c
 * (pvr_submit_info_stream_init: CR_CDM_CONTEXT_PDS1 with pds_seq_dep,
 * usc_seq_dep, target and fence false, the switch's unified size,
 * common_shared, the shared registers' common size and the load
 * program's data size).
 */
uint32_t
powervr_compute_resume_pds1(
	uint32_t common_bytes)
{
	uint32_t word;

	/* The switch's unified store, the common store shared and its size, the load's data. */
	word = SHARED_UNIFIED_UNITS << CDM_UNIFIED_SHIFT;
	word |= CDM_COMMON_SHARED;
	word |= ((common_bytes + CDM_COMMON_UNIT_BYTES - 1U) / CDM_COMMON_UNIT_BYTES) << CDM_COMMON_SHIFT;
	word |= (POWERVR_SHARED_PDS_DATA_WORDS * 4U / UNIT_BYTES) << CDM_DATA_SHIFT;

	/* Succeeded: the word. */
	return word;
}
