/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The driver's own compute programs for the PowerVR core, built with the
 * shading cluster and data sequencer encoders.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/powervr/compute-program.h"
#include "drivers/gpu/powervr/compiler/pds-encode.h"
#include "drivers/gpu/powervr/compiler/usc-encode.h"

/*
 * The control stream's block types, task targets and shared data loaders,
 * and the field limits of a kernel's words.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/cdm.xml (BLOCK_TYPE
 * COMPUTE_KERNEL 0 and STREAM_TERMINATE 2 at bits 30..31; USC_TARGET ANY
 * 1; SD_TYPE NONE 0 and PDS 1; KERNEL0 usc_unified_size 12..17 in units
 * of 16 bytes, pds_temp_size 8..11 in units of 16, pds_data_size 2..7 in
 * units of 16, usc_target 1, fence 0; KERNEL1 data_addr 4..31 shifted by
 * 4, sd_type 2..3, usc_common_shared 1; KERNEL2 code_addr 4..31 shifted
 * by 4; KERNEL3 to 5 the workgroup counts less one; KERNEL8
 * max_instances 27..31, workgroup_size_x 18..26, _y 9..17, _z 0..8, each
 * size less one).
 */
#define BLOCK_KERNEL		0U
#define BLOCK_TERMINATE		2U
#define BLOCK_SHIFT		30U
#define KERNEL_UNIT_BYTES	16U
#define KERNEL_UNIFIED_MAX	0x3fU
#define KERNEL_TEMPS_MAX	0xfU
#define KERNEL_DATA_MAX		0x3fU
#define KERNEL_ADDRESS_MASK	0xfffffff0U
#define KERNEL_INSTANCES_MAX	0x1fU
#define KERNEL_SIZE_MAX		0x200U
#define KERNEL_COMMON_UNIT_BYTES	64U
#define KERNEL_COMMON_MAX	0x1ffU
#define KERNEL_GROUPS_MAX	0x10000U

/*
 * The data sequencer's predicates and registers the programs name.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_PREDICATE_IF1 2, _KEEP 8;
 * PVR_ROGUE_PDSINST_REGS64TP_PTEMP64_LOWER 16).
 */
#define PDS_IF1			2U
#define PDS_KEEP		8U
#define PDS_PERSISTENT_0	16U

/*
 * The instance number register, and the store's data size, cache mode
 * and burst.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py (special register inst_num
 * 51; F_DSIZE 32bit 0b10; F_CACHEMODE_ST write_through 0b00; F_COUNT_SRC
 * s2 0b0; F_COUNT_OP byp 0b10; F_SHIFT1_OP byp 0b00).
 */
#define USC_INSTANCE_NUMBER	51U
#define USC_SELECT_FT0		0U
#define USC_SELECT_FTE		3U
#define USC_MUX_S1		4U
#define USC_SIZE_32		2U
#define USC_WRITE_THROUGH	0U
#define USC_COUNT_FROM_S2	0U
#define USC_COUNT_PASS		2U
#define USC_SHIFT_PASS		0U

/* What a store program writes: its first word's address and its steps. */
struct store_shape {
	uint32_t base_low;
	uint32_t base_high;
	unsigned steps;
};

/* The groups every store program starts with, before its first step. */
#define STORE_PROLOGUE		6U

/*
 * The groups of one step: the step's offset, the address as the base
 * pair plus the instance's offset plus the step's, the data's move, the
 * store and the wait.  Every step makes its address afresh from values
 * set once (the base pair and the instance's offset), so no address
 * carries from one step to the next.
 */
#define STORE_STEP		6U

/* The groups of the temporaries program. */
#define TEMPS_GROUPS		10U

/* The groups of the fill program. */
#define FILL_GROUPS		13U

/*
 * What a fill program writes: the word of instance 0's address, and the
 * multiplier and addend that make an instance's number its value.
 */
struct fill_shape {
	uint32_t base_low;
	uint32_t base_high;
	uint32_t multiplier;
	uint32_t addend;
};

static int store_fetch(void *context, size_t index, struct powervr_usc_group *group);
static int temps_fetch(void *context, size_t index, struct powervr_usc_group *group);
static int fill_fetch(void *context, size_t index, struct powervr_usc_group *group);
static int immediate_group(struct powervr_usc_group *group, unsigned destination, uint32_t value);
static int pass_group(struct powervr_usc_group *group, unsigned destination, unsigned bank, unsigned source);
static int special_group(struct powervr_usc_group *group, unsigned destination, unsigned source);
static int multiply_add_group(struct powervr_usc_group *group, unsigned destination, unsigned first, unsigned second, unsigned addend);
static int store_group(struct powervr_usc_group *group, unsigned data, unsigned address);
static int add64_group(struct powervr_usc_group *group, unsigned destination, unsigned base, unsigned offset);
static int wait_group(struct powervr_usc_group *group);
static int end_group(struct powervr_usc_group *group);
static int nothing_group(struct powervr_usc_group *group);
static int probe_with_fillers(const struct powervr_usc_group *groups, size_t count, int nothing, uint8_t *output, size_t room, size_t *length);
static void temporary(struct powervr_usc_register *operand, unsigned index);

/* A group with no operation, no operand, a repeat count of one. */
static const struct powervr_usc_group blank_group = {
	.repeat = 1
};

/*
 * Builds the control stream words of one compute kernel.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_compute_generate_control_stream: KERNEL0, 1, 2, then 3, 4, 5
 * without an indirect buffer, then 8; the sizes in units rounded up).
 * Values referenced:
 * Mesa 25.0.7 src/imagination/csbgen/rogue_cdm.xml (KERNEL0
 * usc_common_size bits 18 to 26 in units of 64 bytes; KERNEL3, 4, 5 the
 * workgroup counts less one); pvr_cmd_buffer.c
 * (pvr_compute_update_kernel: the coefficient registers as the common
 * size, the global size the dispatch's workgroups).
 */
int
powervr_compute_kernel_words(
	const struct powervr_compute_kernel *kernel,
	uint32_t words[POWERVR_COMPUTE_KERNEL_WORDS])
{
	unsigned unified;
	unsigned common;
	unsigned temps;
	unsigned data;
	unsigned groups[3];
	unsigned axis;

	/* Refuses a missing kernel or result. */
	if (kernel == NULL || words == NULL)
		return EINVAL;

	/* Refuses flags that are not single bits. */
	if (kernel->fence > 1 ||
	    kernel->shared_from_pds > 1 ||
	    kernel->shared_from_usc > 1 ||
	    kernel->shared_from_pds + kernel->shared_from_usc > 1 ||
	    kernel->common_shared > 1 ||
	    kernel->any_cluster > 1)
		return EINVAL;

	/* Refuses program places off the 16-byte alignment. */
	if ((kernel->data_offset & ~KERNEL_ADDRESS_MASK) != 0 || (kernel->code_offset & ~KERNEL_ADDRESS_MASK) != 0)
		return EINVAL;

	/* Refuses a workgroup or task size the fields cannot hold. */
	if (kernel->instances == 0 || kernel->instances > KERNEL_SIZE_MAX || kernel->max_instances > KERNEL_INSTANCES_MAX)
		return EINVAL;

	/* The sizes in 16-byte units, rounded up, each within its field. */
	unified = (kernel->usc_inputs * 4U + KERNEL_UNIT_BYTES - 1U) / KERNEL_UNIT_BYTES;
	temps = (kernel->pds_temporaries * 4U + KERNEL_UNIT_BYTES - 1U) / KERNEL_UNIT_BYTES;
	data = (kernel->data_words * 4U + KERNEL_UNIT_BYTES - 1U) / KERNEL_UNIT_BYTES;
	common = (kernel->usc_common * 4U + KERNEL_COMMON_UNIT_BYTES - 1U) / KERNEL_COMMON_UNIT_BYTES;
	if (unified > KERNEL_UNIFIED_MAX || temps > KERNEL_TEMPS_MAX || data > KERNEL_DATA_MAX || common > KERNEL_COMMON_MAX)
		return EINVAL;

	/* The workgroup counts, zero taken as one. */
	for (axis = 0; axis < 3U; axis++) {
		/* A count of zero means one workgroup on that axis. */
		groups[axis] = kernel->workgroups[axis];
		if (groups[axis] == 0)
			groups[axis] = 1U;

		/* Refuses a count the field cannot hold. */
		if (groups[axis] > KERNEL_GROUPS_MAX)
			return EINVAL;
	}

	/* The first word: the kernel block, its sizes, its target and its fence. */
	words[0] = BLOCK_KERNEL << BLOCK_SHIFT;
	words[0] |= common << 18;
	words[0] |= unified << 12;
	words[0] |= temps << 8;
	words[0] |= data << 2;
	words[0] |= kernel->any_cluster << 1;
	words[0] |= kernel->fence;

	/* The data segment, who loads the shared registers, and whether the kernel frees them. */
	words[1] = kernel->data_offset;
	words[1] |= kernel->shared_from_pds << 2;
	words[1] |= (kernel->shared_from_usc * 2U) << 2;
	words[1] |= kernel->common_shared << 1;

	/* The code segment. */
	words[2] = kernel->code_offset;

	/* The workgroups in each dimension, written less one. */
	words[3] = groups[0] - 1U;
	words[4] = groups[1] - 1U;
	words[5] = groups[2] - 1U;

	/* The task size and the workgroup's size, all in x, written less one. */
	words[6] = (uint32_t)kernel->max_instances << 27;
	words[6] |= (uint32_t)(kernel->instances - 1U) << 18;

	/* Succeeded: the kernel's words. */
	return 0;
}

/*
 * Reports the control stream word that ends a stream.
 */
uint32_t
powervr_compute_terminate_word(void)
{
	uint32_t word;

	/* A terminate block has no field but its type. */
	word = BLOCK_TERMINATE << BLOCK_SHIFT;

	/* Succeeded: the word. */
	return word;
}

/*
 * Works out the task issue's temporaries field for a count of
 * registers, assuming the field counts in blocks of unit registers.
 *
 * The unit is still unproven on the hardware (1, 2 or 4), so the
 * caller names the one it assumes.
 */
int
powervr_compute_temporaries_field(
	unsigned temporaries,
	unsigned unit,
	unsigned *field)
{
	unsigned blocks;

	/* Refuses a unit other than the ones in question. */
	if ((unit != 1U && unit != 2U && unit != 4U) || field == NULL)
		return EINVAL;

	/* Counts the blocks, rounded up, refusing more than the six-bit field holds. */
	blocks = (temporaries + unit - 1U) / unit;
	if (blocks > 0x3fU)
		return EINVAL;

	/* Succeeded: the field. */
	*field = blocks;
	return 0;
}

/*
 * Works out the task issue's temporaries field for a program that uses
 * the given number of temporaries (its highest register plus one): in
 * blocks of four registers (Mesa's pvr_pds_setup_doutu,
 * PVR_ROGUE_PDSINST_DOUT_FIELDS_DOUTU_SRC0_TEMPS_ALIGNSIZE 4), for one
 * register more than the program uses.  The check's hand-written
 * kernels, issued by the plain kick program, ran with one more and not
 * without it (all 16384 words written with it, 6944 of 16384 without);
 * the workgroup program (Mesa's) runs with either.
 */
int
powervr_compute_task_field(
	unsigned temporaries,
	unsigned *field)
{
	int error;

	/* Blocks of four for one register more. */
	error = powervr_compute_temporaries_field(temporaries + 1U, 4, field);
	if (error != 0)
		return error;

	/* Succeeded: the field. */
	return 0;
}

/*
 * Builds the data sequencer program that issues one shading cluster task
 * per instance group: its data word and its code.
 *
 * The code skips the coefficient update pass and issues the task; the
 * temporaries field is written as given.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_pds.c
 * (pvr_pds_compute_shader with only kick_usc: the branch on IF1 negated
 * past the halt of the coefficient update pass, the task issue from the
 * first 64-bit constant ending the program, then a halt;
 * pvr_pds_setup_doutu: the instance sample rate).
 */
int
powervr_compute_kick_program(
	uint64_t usc_offset,
	unsigned temporaries_field,
	uint32_t data[POWERVR_COMPUTE_PDS_DATA_WORDS],
	uint32_t code[POWERVR_COMPUTE_PDS_CODE_MAX],
	unsigned *code_words)
{
	uint64_t task;
	int error;

	/* Refuses a missing result or a field wider than six bits. */
	if (data == NULL || code == NULL || code_words == NULL || temporaries_field > 0x3fU)
		return EINVAL;

	/* The task issue word with no temporaries, then the field as given. */
	error = powervr_pds_task_word(usc_offset, 0, 0, 0, &task);
	if (error != 0)
		return error;

	task |= (uint64_t)temporaries_field << 35;
	data[0] = (uint32_t)task;
	data[1] = (uint32_t)(task >> 32);

	/* Skips the coefficient update pass's halt. */
	error = powervr_pds_branch(PDS_IF1, 1, PDS_KEEP, 2, &code[0]);
	if (error != 0)
		return error;

	error = powervr_pds_halt(0, &code[1]);
	if (error != 0)
		return error;

	/* Issues the task from the first 64-bit constant, ending the program. */
	error = powervr_pds_output(0, 1, POWERVR_PDS_OUTPUT_SHADER_TASK, 0, 0, &code[2]);
	if (error != 0)
		return error;

	error = powervr_pds_halt(0, &code[3]);
	if (error != 0)
		return error;

	/* Succeeded: four code words. */
	*code_words = 4;
	return 0;
}

/*
 * Mesa's data sequencer program of a compute dispatch, as its driver
 * wrote it for vkcompute on the BXE-4-32 (Mesa 26.2.5 on the host with
 * the drm shim):
 * data words 2 to 7 (the two writes of the workgroup id into the common
 * store, the write of the local id into the unified store, the base
 * workgroup's zeros), and the ten code words: the branch past the
 * coefficient update pass, three adds of the base workgroup to the ids,
 * the two writes, a halt; then the instance pass's write of the local id,
 * the task issue ending the program, a halt.  The code names data words
 * by their places, so it holds for any address and field.
 */
static const uint32_t group_data[POWERVR_COMPUTE_PDS_GROUP_DATA_WORDS] = {
	0x00000000, 0x00000000, 0x10000002, 0x90004001, 0x80000000, 0x00000000, 0x00000000, 0x00000000,
};

static const uint32_t group_code[POWERVR_COMPUTE_PDS_GROUP_CODE_WORDS] = {
	0xc2c00007, 0x90016000, 0x9001a041, 0x9001e0c3, 0xf0024001,
	0xf0034101, 0xd3000000, 0xf0044001, 0xfc000002, 0xd3000000,
};

/*
 * Builds the data sequencer program of a kernel of many workgroups,
 * Mesa's for a compute dispatch (above): the task issue word (the
 * program's address, its temporaries field) in data words 0 and 1, the
 * rest as Mesa has them.  Its kernel takes POWERVR_COMPUTE_PDS_GROUP_DATA_WORDS
 * data words, its code POWERVR_COMPUTE_PDS_GROUP_CODE bytes after them
 * and POWERVR_COMPUTE_GROUP_USC_INPUTS unified store registers (the local
 * id).  On the board the earlier program (the task issue alone in the
 * instance pass) let the instances' registers overlap over several
 * workgroups, and Mesa's ran 4096/4096.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_pds.c (pvr_pds_compute_shader
 * with flattened work groups, the work-group ids and the local id in
 * use, a base workgroup) and pvr_pds_setup_doutu (the task word).
 */
int
powervr_compute_workgroup_program(
	uint64_t usc_offset,
	unsigned temporaries_field,
	uint32_t data[POWERVR_COMPUTE_PDS_GROUP_DATA_WORDS],
	uint32_t code[POWERVR_COMPUTE_PDS_CODE_MAX],
	unsigned *code_words)
{
	uint64_t task;
	unsigned i;
	int error;

	/* Refuses a missing result or a field wider than six bits. */
	if (data == NULL || code == NULL || code_words == NULL || temporaries_field > 0x3fU)
		return EINVAL;

	/* The task issue word, as the plain kick's, then Mesa's words. */
	error = powervr_pds_task_word(usc_offset, 0, 0, 0, &task);
	if (error != 0)
		return error;

	/* Mesa's data words with the task word, its temporaries field added, in the first two. */
	task |= (uint64_t)temporaries_field << 35;
	for (i = 0; i < POWERVR_COMPUTE_PDS_GROUP_DATA_WORDS; i++)
		data[i] = group_data[i];
	data[0] = (uint32_t)task;
	data[1] = (uint32_t)(task >> 32);

	/* Mesa's code. */
	for (i = 0; i < POWERVR_COMPUTE_PDS_GROUP_CODE_WORDS; i++)
		code[i] = group_code[i];

	/* Succeeded: ten code words. */
	*code_words = POWERVR_COMPUTE_PDS_GROUP_CODE_WORDS;
	return 0;
}

/*
 * Builds the shared register allocation kernel's data sequencer
 * program: two zero data words and a halt (compute-program.h).
 */
int
powervr_compute_shared_program(
	uint32_t data[POWERVR_COMPUTE_PDS_DATA_WORDS],
	uint32_t code[POWERVR_COMPUTE_PDS_CODE_MAX],
	unsigned *code_words)
{
	int error;

	/* Refuses a missing result. */
	if (data == NULL || code == NULL || code_words == NULL)
		return EINVAL;

	/* Zero data, then a halt. */
	data[0] = 0;
	data[1] = 0;
	error = powervr_pds_halt(0, &code[0]);
	if (error != 0)
		return error;

	/* Succeeded: one code word. */
	*code_words = 1;
	return 0;
}

/*
 * Builds the shared register allocation kernel's words for its data
 * sequencer program at the given places (compute-program.h).
 */
int
powervr_compute_shared_kernel_words(
	uint32_t data_offset,
	uint32_t code_offset,
	uint32_t words[POWERVR_COMPUTE_KERNEL_WORDS])
{
	static const struct powervr_compute_kernel cleared;
	struct powervr_compute_kernel kernel;
	int error;

	/* One instance on every cluster, the common store kept for the next kernels, no shared data. */
	kernel = cleared;
	kernel.data_offset = data_offset;
	kernel.code_offset = code_offset;
	kernel.data_words = POWERVR_COMPUTE_PDS_DATA_WORDS;
	kernel.usc_common = POWERVR_COMPUTE_SHARED_COMMON;
	kernel.instances = 1;
	kernel.max_instances = POWERVR_COMPUTE_FENCE_INSTANCES;
	kernel.common_shared = 1;
	kernel.any_cluster = 0;

	/* Builds the kernel's words. */
	error = powervr_compute_kernel_words(&kernel, words);
	if (error != 0)
		return error;

	/* Succeeded: the shared allocation kernel's words. */
	return 0;
}

/*
 * Builds the data sequencer program of the fence that ends a compute
 * stream: its zero data word and its code.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_pds.c
 * (pvr_pds_compute_shader with fence and clear_pds_barrier: the branch
 * past the coefficient update pass's halt, the 64-bit add of the zero
 * constant into the first persistent temporary, the compute fence
 * output, then a halt).
 */
int
powervr_compute_fence_program(
	uint32_t data[POWERVR_COMPUTE_PDS_DATA_WORDS],
	uint32_t code[POWERVR_COMPUTE_PDS_CODE_MAX],
	unsigned *code_words)
{
	int error;

	/* Refuses a missing result. */
	if (data == NULL || code == NULL || code_words == NULL)
		return EINVAL;

	/* The zero constant. */
	data[0] = 0;
	data[1] = 0;

	/* Skips the coefficient update pass's halt. */
	error = powervr_pds_branch(PDS_IF1, 1, PDS_KEEP, 2, &code[0]);
	if (error != 0)
		return error;

	error = powervr_pds_halt(0, &code[1]);
	if (error != 0)
		return error;

	/* Clears the persistent temporary the barrier uses. */
	error = powervr_pds_add64(0, 0, 0, 0, 0, PDS_PERSISTENT_0, &code[2]);
	if (error != 0)
		return error;

	/* The fence, then the end. */
	error = powervr_pds_output(0, 0, POWERVR_PDS_OUTPUT_COMPUTE_FENCE, 0, 0, &code[3]);
	if (error != 0)
		return error;

	error = powervr_pds_halt(0, &code[4]);
	if (error != 0)
		return error;

	/* Succeeded: five code words. */
	*code_words = 5;
	return 0;
}

/*
 * Builds the shading cluster program that writes a run of words.
 *
 * Each of the 32 instances of a task writes every 32nd word, starting
 * at its instance number, for steps steps; each word is written with
 * the low 32 bits of its own device address.  The run must not cross a
 * 4 GiB boundary, as only the low half of the address is advanced.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_trans_nir.c (trans_store_global:
 * the address pair followed by the data in consecutive registers, a
 * 32-bit store with data return counter 0 and burst 1;
 * nir_intrinsic_load_instance_num_pco: a move from the instance number
 * register); pco_schedule.c (a wait for the data return counter after
 * each store); pco_end.c (a no-operation that ends the program).
 */
int
powervr_compute_store_program(
	uint64_t base,
	unsigned steps,
	uint8_t *output,
	size_t room,
	size_t *length)
{
	struct store_shape shape;
	uint64_t last;
	size_t count;
	int error;

	/* Refuses an empty run, a misaligned one, or one whose low half would wrap. */
	if (steps == 0 || (base & 3U) != 0)
		return EINVAL;

	last = (base & 0xffffffffULL) + (uint64_t)steps * POWERVR_COMPUTE_STORE_STRIDE;
	if (last > 0x100000000ULL)
		return EINVAL;

	/* The shape the groups are built from. */
	shape.base_low = (uint32_t)base;
	shape.base_high = (uint32_t)(base >> 32);
	shape.steps = steps;
	count = STORE_PROLOGUE + (size_t)steps * STORE_STEP + 1U;

	/* Encodes the groups one at a time. */
	error = powervr_usc_program_build(count, store_fetch, &shape, output, room, length);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Builds the shading cluster program of the temporaries check.
 *
 * Each of the 32 instances writes its own instance number to the word
 * at base plus four times that number, using exactly eight temporaries
 * (r0 to r7), each written once.  With a task issue field too small for
 * eight, neighbouring instances share registers and the words go wrong.
 */
int
powervr_compute_temps_program(
	uint64_t base,
	uint8_t *output,
	size_t room,
	size_t *length)
{
	struct store_shape shape;
	int error;

	/* Refuses a misaligned base or one whose low half would wrap. */
	if ((base & 3U) != 0 || (base & 0xffffffffULL) + POWERVR_COMPUTE_INSTANCES * 4U > 0x100000000ULL)
		return EINVAL;

	/* The shape the groups are built from. */
	shape.base_low = (uint32_t)base;
	shape.base_high = (uint32_t)(base >> 32);
	shape.steps = 1;

	/* Encodes the groups one at a time. */
	error = powervr_usc_program_build(TEMPS_GROUPS, temps_fetch, &shape, output, room, length);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Builds one of the compute check's probe programs, each writing at
 * base (only the low half of the address is computed):
 *
 *   MADD           every instance writes 3 * 5 + 7, the multiply-add of
 *                  temporaries alone (22 when it works);
 *   INSTANCE       every instance writes its instance number (the last
 *                  writer's stays);
 *   INSTANCE_MADD  every instance writes its number times four through
 *                  the multiply-add;
 *   ADDRESS        each instance writes its number to the word at base
 *                  plus four times it (the temporaries program without
 *                  its fill);
 *   STRIDE_16, _64, _128  the same with 16, 64 or 128 bytes between the
 *                  instances' words (memory granules);
 *   ADDRESS_WAIT   ADDRESS with a wait for the data return counter
 *                  between the address's computation and the store;
 *   COMPUTED       each instance computes ADDRESS's address and writes
 *                  its low word to base, its high word to base + 4 (the
 *                  last writer's address, base + 124 for instance 31);
 *   ZERO_MADD      the address computed as the instance number times 0
 *                  plus base, the same for all, and the number written;
 *   CONSTANT       the address computed as 1 * 4 + base, not from the
 *                  instance number, and the instance number written;
 *   MOVED          ADDRESS's address computed into another temporary
 *                  and moved into the address pair before the store;
 *   LIKE_STORE     the one-step store program's way, which the board
 *                  ran right: the offset by the multiply-add into r8,
 *                  the base in r10 and r11, the pair r0 and r1 made by
 *                  the 64-bit add from them, the address's low word
 *                  written to it;
 *   LIKE_STORE_NUMBER    the same writing the instance number instead;
 *   LIKE_STORE_IN_PLACE  the same with the base in r0 and r1 and the
 *                  64-bit add writing the pair it reads;
 *   LIKE_STORE_EARLY     the same with r0 to r11 set before the
 *                  multiply-add instead of after;
 *   REWRITE        LIKE_STORE_NUMBER with the multiply-add's zero addend
 *                  (r6) overwritten by the very next group;
 *   REWRITE_LATER  the same with two other groups between them;
 *   NOP_BETWEEN    LIKE_STORE_NUMBER with two control no-operation groups
 *                  (the compiler's spacing, without the end mark) after
 *                  each group before the store;
 *   WAIT_BETWEEN   the same with two waits for the data return counter,
 *                  the group every program here already runs.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_trans_nir.c (trans_store_global
 * and nir_intrinsic_load_instance_num_pco, as for the store program).
 */
int
powervr_compute_probe_program(
	unsigned kind,
	uint64_t base,
	uint8_t *output,
	size_t room,
	size_t *length)
{
	struct powervr_usc_group groups[20];
	uint32_t constant_index;
	unsigned address;
	uint32_t stride;
	size_t count;
	int error;

	/* Refuses an unknown probe, a misaligned base or one whose words would wrap. */
	if (kind >= POWERVR_COMPUTE_PROBES || (base & 3U) != 0)
		return EINVAL;
	stride = powervr_compute_probe_stride(kind);
	if ((base & 0xffffffffULL) + (uint64_t)POWERVR_COMPUTE_INSTANCES * stride > 0x100000000ULL)
		return EINVAL;

	/* The address in r0 and r1 (r12 and r13 for the ADDRESS kinds). */
	count = 0;
	address = 0;
	error = immediate_group(&groups[count++], 0, (uint32_t)base);
	if (error == 0)
		error = immediate_group(&groups[count++], 1, (uint32_t)(base >> 32));

	/* The value in r2 (and, for ADDRESS, the instance's word in r0). */
	if (error == 0 && kind == POWERVR_COMPUTE_PROBE_MADD) {
		error = immediate_group(&groups[count++], 3, 3);
		if (error == 0)
			error = immediate_group(&groups[count++], 4, 5);
		if (error == 0)
			error = immediate_group(&groups[count++], 5, 7);
		if (error == 0)
			error = multiply_add_group(&groups[count++], 2, 3, 4, 5);
	} else if (error == 0 && kind == POWERVR_COMPUTE_PROBE_INSTANCE) {
		error = special_group(&groups[count++], 2, USC_INSTANCE_NUMBER);
	} else if (error == 0 && kind == POWERVR_COMPUTE_PROBE_INSTANCE_MADD) {
		error = special_group(&groups[count++], 5, USC_INSTANCE_NUMBER);
		if (error == 0)
			error = immediate_group(&groups[count++], 4, 4);
		if (error == 0)
			error = immediate_group(&groups[count++], 6, 0);
		if (error == 0)
			error = multiply_add_group(&groups[count++], 2, 5, 4, 6);
	} else if (error == 0 && kind == POWERVR_COMPUTE_PROBE_COMPUTED) {
		/* The address in r0 as ADDRESS makes it, then r0 stored at base (r8, r9, the data in r10) and r1 at base + 4 (r12, r13, the data in r14). */
		error = special_group(&groups[count++], 5, USC_INSTANCE_NUMBER);
		if (error == 0)
			error = immediate_group(&groups[count++], 4, 4);
		if (error == 0)
			error = multiply_add_group(&groups[count++], 0, 5, 4, 0);
		if (error == 0)
			error = immediate_group(&groups[count++], 8, (uint32_t)base);
		if (error == 0)
			error = immediate_group(&groups[count++], 9, (uint32_t)(base >> 32));
		if (error == 0)
			error = pass_group(&groups[count++], 10, POWERVR_USC_BANK_TEMPORARY, 0);
		if (error == 0)
			error = immediate_group(&groups[count++], 12, (uint32_t)base + 4U);
		if (error == 0)
			error = immediate_group(&groups[count++], 13, (uint32_t)(base >> 32));
		if (error == 0)
			error = pass_group(&groups[count++], 14, POWERVR_USC_BANK_TEMPORARY, 1);
		if (error == 0)
			error = store_group(&groups[count++], 10, 8);
		if (error == 0)
			error = wait_group(&groups[count++]);
		if (error == 0)
			error = store_group(&groups[count++], 14, 12);
		if (error == 0)
			error = wait_group(&groups[count++]);
		if (error == 0)
			error = end_group(&groups[count++]);
		if (error != 0)
			return error;

		/* Encodes them; this probe has its own stores. */
		error = powervr_usc_program_encode(groups, count, output, room, length);
		if (error != 0)
			return error;
		return 0;
	} else if (error == 0 && (kind == POWERVR_COMPUTE_PROBE_ZERO_MADD || kind == POWERVR_COMPUTE_PROBE_CONSTANT)) {
		/* The number as data, and the address from a constant factor or index. */
		error = special_group(&groups[count++], 2, USC_INSTANCE_NUMBER);
		if (error == 0 && kind == POWERVR_COMPUTE_PROBE_CONSTANT)
			error = immediate_group(&groups[count++], 5, 1);
		if (error == 0 && kind == POWERVR_COMPUTE_PROBE_ZERO_MADD)
			error = special_group(&groups[count++], 5, USC_INSTANCE_NUMBER);

		/* The index register's value: four for CONSTANT, zero for ZERO_MADD. */
		constant_index = 0U;
		if (kind == POWERVR_COMPUTE_PROBE_CONSTANT)
			constant_index = 4U;

		/* The index, then the multiply-add that makes the address. */
		if (error == 0)
			error = immediate_group(&groups[count++], 4, constant_index);
		if (error == 0)
			error = multiply_add_group(&groups[count++], 0, 5, 4, 0);
	} else if (error == 0 && kind >= POWERVR_COMPUTE_PROBE_LIKE_STORE) {
		/* The one-step store program's set-up. */
		count = 0;
		if (kind == POWERVR_COMPUTE_PROBE_LIKE_STORE_EARLY) {
			error = immediate_group(&groups[count++], 10, (uint32_t)base);
			if (error == 0)
				error = immediate_group(&groups[count++], 11, (uint32_t)(base >> 32));
			if (error == 0)
				error = immediate_group(&groups[count++], 0, 0);
			if (error == 0)
				error = immediate_group(&groups[count++], 1, 0);
		}

		if (error == 0)
			error = special_group(&groups[count++], 5, USC_INSTANCE_NUMBER);
		if (error == 0)
			error = immediate_group(&groups[count++], 4, 4);
		if (error == 0)
			error = immediate_group(&groups[count++], 6, 0);
		if (error == 0)
			error = multiply_add_group(&groups[count++], 8, 5, 4, 6);
		if (error == 0 && kind == POWERVR_COMPUTE_PROBE_REWRITE)
			error = immediate_group(&groups[count++], 6, 0x12340U);
		if (error == 0 && kind == POWERVR_COMPUTE_PROBE_REWRITE_LATER) {
			error = immediate_group(&groups[count++], 9, 0);
			if (error == 0)
				error = immediate_group(&groups[count++], 12, 0);
			if (error == 0)
				error = immediate_group(&groups[count++], 6, 0x12340U);
		}

		if (error == 0 && kind == POWERVR_COMPUTE_PROBE_LIKE_STORE_IN_PLACE) {
			error = immediate_group(&groups[count++], 0, (uint32_t)base);
			if (error == 0)
				error = immediate_group(&groups[count++], 1, (uint32_t)(base >> 32));
			if (error == 0)
				error = add64_group(&groups[count++], 0, 0, 8);
		} else if (error == 0) {
			if (kind != POWERVR_COMPUTE_PROBE_LIKE_STORE_EARLY) {
				error = immediate_group(&groups[count++], 10, (uint32_t)base);
				if (error == 0)
					error = immediate_group(&groups[count++], 11, (uint32_t)(base >> 32));
			}

			if (error == 0)
				error = add64_group(&groups[count++], 0, 10, 8);
		}

		if (error == 0 && kind == POWERVR_COMPUTE_PROBE_LIKE_STORE)
			error = pass_group(&groups[count++], 2, POWERVR_USC_BANK_TEMPORARY, 0);
		else if (error == 0)
			error = pass_group(&groups[count++], 2, POWERVR_USC_BANK_TEMPORARY, 5);
	} else if (error == 0 && kind == POWERVR_COMPUTE_PROBE_MOVED) {
		/* The address in r6, moved into r0, the number as data. */
		error = special_group(&groups[count++], 5, USC_INSTANCE_NUMBER);
		if (error == 0)
			error = immediate_group(&groups[count++], 4, 4);
		if (error == 0)
			error = multiply_add_group(&groups[count++], 6, 5, 4, 0);
		if (error == 0)
			error = pass_group(&groups[count++], 0, POWERVR_USC_BANK_TEMPORARY, 6);
		if (error == 0)
			error = pass_group(&groups[count++], 2, POWERVR_USC_BANK_TEMPORARY, 5);
	} else if (error == 0) {
		/* The offset in r8 by the multiply-add, then the address pair by the 64-bit add. */
		error = special_group(&groups[count++], 5, USC_INSTANCE_NUMBER);
		if (error == 0)
			error = immediate_group(&groups[count++], 4, stride);
		if (error == 0)
			error = immediate_group(&groups[count++], 6, 0);
		if (error == 0)
			error = multiply_add_group(&groups[count++], 8, 5, 4, 6);
		if (error == 0)
			error = immediate_group(&groups[count++], 0, (uint32_t)base);
		if (error == 0)
			error = immediate_group(&groups[count++], 1, (uint32_t)(base >> 32));
		if (error == 0)
			error = add64_group(&groups[count++], 12, 0, 8);
		if (error == 0)
			error = pass_group(&groups[count++], 14, POWERVR_USC_BANK_TEMPORARY, 5);
		address = 12;
		if (error == 0 && kind == POWERVR_COMPUTE_PROBE_ADDRESS_WAIT)
			error = wait_group(&groups[count++]);
	}

	/* The store, the wait for it, the end. */
	if (error == 0)
		error = store_group(&groups[count++], address + 2U, address);
	if (error == 0)
		error = wait_group(&groups[count++]);
	if (error == 0)
		error = end_group(&groups[count++]);
	if (error != 0)
		return error;

	/* With fillers between the groups before the store, for the two filler probes. */
	if (kind == POWERVR_COMPUTE_PROBE_NOP_BETWEEN || kind == POWERVR_COMPUTE_PROBE_WAIT_BETWEEN) {
		error = probe_with_fillers(groups, count, kind == POWERVR_COMPUTE_PROBE_NOP_BETWEEN, output, room, length);
		if (error != 0)
			return error;
		return 0;
	}

	/* Encodes them. */
	error = powervr_usc_program_encode(groups, count, output, room, length);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Encodes a probe's groups with two filler groups (no-operations or
 * waits) after each group before the last three (the store, its wait
 * and the end).
 */
static int
probe_with_fillers(
	const struct powervr_usc_group *groups,
	size_t count,
	int nothing,
	uint8_t *output,
	size_t room,
	size_t *length)
{
	struct powervr_usc_group spaced[64];
	size_t used;
	size_t index;
	unsigned filler;
	int error;

	/* Each group, then its fillers while the store is still ahead. */
	used = 0;
	for (index = 0; index < count; index++) {
		/* Refuses a program longer than the room. */
		if (used >= 64U)
			return E2BIG;

		/* The group itself. */
		spaced[used++] = groups[index];

		/* No fillers after the last three groups. */
		if (index + 3U >= count)
			continue;

		/* Two fillers: no-operations or waits. */
		for (filler = 0; filler < 2; filler++) {
			/* Refuses a program longer than the room. */
			if (used >= 64U)
				return E2BIG;

			/* The filler of the chosen kind. */
			if (nothing)
				error = nothing_group(&spaced[used++]);
			else
				error = wait_group(&spaced[used++]);
			if (error != 0)
				return error;
		}
	}

	/* Encodes them. */
	error = powervr_usc_program_encode(spaced, used, output, room, length);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Builds a control group that does nothing: the no-operation without
 * the end mark.
 */
static int
nothing_group(
	struct powervr_usc_group *group)
{
	int error;

	/* A control group naming the no-operation. */
	*group = blank_group;
	group->unit = POWERVR_USC_UNIT_CONTROL;
	group->control = POWERVR_USC_CONTROL_NOTHING;

	/* The no-operation's single byte. */
	error = powervr_usc_nothing(&group->operation[POWERVR_USC_PHASE_0]);
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/* Reports the bytes between two instances' words in a probe: 16, 64 or 128 for the strided ones, else 4. */
uint32_t
powervr_compute_probe_stride(
	unsigned kind)
{
	/* The strided probes. */
	if (kind == POWERVR_COMPUTE_PROBE_STRIDE_16)
		return 16;
	if (kind == POWERVR_COMPUTE_PROBE_STRIDE_64)
		return 64;
	if (kind == POWERVR_COMPUTE_PROBE_STRIDE_128)
		return 128;

	/* Succeeded: a word apart. */
	return 4;
}

/*
 * Builds the shading cluster program of one workgroup of a fill kernel.
 *
 * Each instance writes the word at base plus four times its instance
 * number, with the value instance * multiplier + addend (modulo 2^32).
 * The words of the workgroup must not cross a 4 GiB boundary, as only
 * the low half of the address is computed.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_trans_nir.c (trans_store_global
 * and nir_intrinsic_load_instance_num_pco, as for the store program);
 * pco_schedule.c and pco_end.c (the wait after the store, the end).
 */
int
powervr_compute_fill_program(
	uint64_t base,
	unsigned instances,
	uint32_t multiplier,
	uint32_t addend,
	uint8_t *output,
	size_t room,
	size_t *length)
{
	struct fill_shape shape;
	uint64_t last;
	int error;

	/* Refuses an empty or oversized workgroup, a misaligned base, or words whose low half would wrap. */
	if (instances == 0 || instances > POWERVR_COMPUTE_INSTANCES || (base & 3U) != 0)
		return EINVAL;

	last = (base & 0xffffffffULL) + (uint64_t)instances * 4U;
	if (last > 0x100000000ULL)
		return EINVAL;

	/* The shape the groups are built from. */
	shape.base_low = (uint32_t)base;
	shape.base_high = (uint32_t)(base >> 32);
	shape.multiplier = multiplier;
	shape.addend = addend;

	/* Encodes the groups one at a time. */
	error = powervr_usc_program_build(FILL_GROUPS, fill_fetch, &shape, output, room, length);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Builds the firmware's compute command for a control stream and a
 * context resume buffer; every other field is zero.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_compute.c
 * (pvr_submit_info_stream_init: the control stream base and the context
 * state base as their register values, the address itself for both)
 * and src/imagination/csbgen/rogue/cr.xml (CDM_CTRL_STREAM_BASE addr
 * 2..39 shifted by 2, CDM_CONTEXT_STATE_BASE addr 4..39 shifted by 4).
 */
void
powervr_compute_command(
	uint64_t stream,
	uint64_t resume,
	uint32_t words[POWERVR_COMPUTE_COMMAND_WORDS])
{
	unsigned i;

	/* Clears the command. */
	for (i = 0; i < POWERVR_COMPUTE_COMMAND_WORDS; i++)
		words[i] = 0;

	/* The control stream and the resume buffer, each a 64-bit register value. */
	words[10] = (uint32_t)stream;
	words[11] = (uint32_t)(stream >> 32);
	words[12] = (uint32_t)resume;
	words[13] = (uint32_t)(resume >> 32);
}

/*
 * Hands over one group of the store program: the set-up, then the
 * steps, then the end.
 */
static int
store_fetch(
	void *context,
	size_t index,
	struct powervr_usc_group *group)
{
	const struct store_shape *shape;
	size_t step_index;
	size_t step;
	int error;

	/* The set-up: the instance's offset, made once by the multiply-add. */
	shape = context;
	error = 0;
	if (index == 0) {
		/* r5: the instance number. */
		error = special_group(group, 5, USC_INSTANCE_NUMBER);
	} else if (index == 1) {
		/* r4: four, the bytes of a word. */
		error = immediate_group(group, 4, 4);
	} else if (index == 2) {
		/* r6: zero. */
		error = immediate_group(group, 6, 0);
	} else if (index == 3) {
		/* r8: the instance's offset. */
		error = multiply_add_group(group, 8, 5, 4, 6);
	} else if (index == 4) {
		/* r10: the low half of the base. */
		error = immediate_group(group, 10, shape->base_low);
	} else if (index == 5) {
		/* r11: the high half. */
		error = immediate_group(group, 11, shape->base_high);
	} else if (index < STORE_PROLOGUE + (size_t)shape->steps * STORE_STEP) {
		/*
		 * One step: its address from the base, the instance's offset and
		 * the step's, each 64-bit add into a pair it does not read; the
		 * data; the store and the wait.
		 */
		step = (index - STORE_PROLOGUE) / STORE_STEP;
		step_index = (index - STORE_PROLOGUE) % STORE_STEP;
		if (step_index == 0) {
			error = immediate_group(group, 9, (uint32_t)(step * POWERVR_COMPUTE_STORE_STRIDE));
		} else if (step_index == 1) {
			error = add64_group(group, 0, 10, 8);
		} else if (step_index == 2) {
			error = add64_group(group, 12, 0, 9);
		} else if (step_index == 3) {
			error = pass_group(group, 14, POWERVR_USC_BANK_TEMPORARY, 12);
		} else if (step_index == 4) {
			error = store_group(group, 14, 12);
		} else {
			error = wait_group(group);
		}
	} else {
		/* The end. */
		error = end_group(group);
	}

	/* A group the encoders refused. */
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/*
 * Hands over one group of the temporaries program.
 */
static int
temps_fetch(
	void *context,
	size_t index,
	struct powervr_usc_group *group)
{
	const struct store_shape *shape;
	int error;

	/*
	 * Every register is written once, before anything reads it, and
	 * never after: on the board a register the multiply-add read was
	 * seen holding the value the next group wrote to it (2026-10-20).
	 * The eight registers: the base pair, four, zero, the offset, the
	 * address pair and the number, which is the data after the pair.
	 */
	shape = context;
	error = 0;
	if (index == 0) {
		/* r7: the instance number, the data. */
		error = special_group(group, 7, USC_INSTANCE_NUMBER);
	} else if (index == 1) {
		/* r0: the low half of the base. */
		error = immediate_group(group, 0, shape->base_low);
	} else if (index == 2) {
		/* r1: the high half. */
		error = immediate_group(group, 1, shape->base_high);
	} else if (index == 3) {
		/* r2: four, the bytes of a word. */
		error = immediate_group(group, 2, 4);
	} else if (index == 4) {
		/* r3: zero. */
		error = immediate_group(group, 3, 0);
	} else if (index == 5) {
		/* r4: the instance's offset. */
		error = multiply_add_group(group, 4, 7, 2, 3);
	} else if (index == 6) {
		/* r5 and r6: the instance's word, the data r7 right after them. */
		error = add64_group(group, 5, 0, 4);
	} else if (index == 7) {
		/* The store. */
		error = store_group(group, 7, 5);
	} else if (index == 8) {
		/* The wait for it. */
		error = wait_group(group);
	} else {
		/* The end. */
		error = end_group(group);
	}

	/* A group the encoders refused. */
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/*
 * Hands over one group of the fill program.
 */
static int
fill_fetch(
	void *context,
	size_t index,
	struct powervr_usc_group *group)
{
	const struct fill_shape *shape;
	int error;

	/* Every register written once, before it is read (as for the temporaries program). */
	shape = context;
	error = 0;
	if (index == 0) {
		/* r7: the instance number. */
		error = special_group(group, 7, USC_INSTANCE_NUMBER);
	} else if (index == 1) {
		/* r0: the low half of the base. */
		error = immediate_group(group, 0, shape->base_low);
	} else if (index == 2) {
		/* r1: the high half. */
		error = immediate_group(group, 1, shape->base_high);
	} else if (index == 3) {
		/* r2: four, the bytes of a word. */
		error = immediate_group(group, 2, 4);
	} else if (index == 4) {
		/* r3: zero. */
		error = immediate_group(group, 3, 0);
	} else if (index == 5) {
		/* r4: the multiplier. */
		error = immediate_group(group, 4, shape->multiplier);
	} else if (index == 6) {
		/* r5: the addend. */
		error = immediate_group(group, 5, shape->addend);
	} else if (index == 7) {
		/* r8: the instance's offset. */
		error = multiply_add_group(group, 8, 7, 2, 3);
	} else if (index == 8) {
		/* r12: the value, the data after the address pair. */
		error = multiply_add_group(group, 12, 7, 4, 5);
	} else if (index == 9) {
		/* r10 and r11: the instance's word. */
		error = add64_group(group, 10, 0, 8);
	} else if (index == 10) {
		/* The store. */
		error = store_group(group, 12, 10);
	} else if (index == 11) {
		/* The wait for it. */
		error = wait_group(group);
	} else {
		/* The end. */
		error = end_group(group);
	}

	/* A group the encoders refused. */
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/*
 * Builds a bitwise group that moves a 32-bit immediate into a temporary.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_map.py (group_map O_MOVI32: a
 * bitwise group with phase 0 only, the first write enabled, the
 * immediate bypass with the count from s2, the destination from the
 * phase's second output).
 */
static int
immediate_group(
	struct powervr_usc_group *group,
	unsigned destination,
	uint32_t value)
{
	int error;

	/* A bitwise group writing one temporary. */
	*group = blank_group;
	group->unit = POWERVR_USC_UNIT_BITWISE;
	group->write_0 = 1;
	temporary(&group->destination[0], destination);

	/* The first phase carries the immediate. */
	error = powervr_usc_bit_immediate(value, 1, USC_COUNT_FROM_S2, USC_COUNT_PASS, USC_SHIFT_PASS, &group->operation[POWERVR_USC_PHASE_0]);
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/*
 * Builds a main group that copies a register into a temporary.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_map.py (group_map O_MBYP: phase 0
 * only, the first write enabled, the source in s0, is4 from ft0).
 */
static int
pass_group(
	struct powervr_usc_group *group,
	unsigned destination,
	unsigned bank,
	unsigned source)
{
	int error;

	/* A main group from one source to one temporary. */
	*group = blank_group;
	group->unit = POWERVR_USC_UNIT_MAIN;
	group->write_0 = 1;
	group->source[0].used = 1;
	group->source[0].bank = (uint8_t)bank;
	group->source[0].index = (uint16_t)source;
	temporary(&group->destination[0], destination);

	/* The first phase passes the source through. */
	error = powervr_usc_single(POWERVR_USC_SINGLE_PASS, 0, 0, &group->operation[POWERVR_USC_PHASE_0]);
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/*
 * Builds a main group that copies a special register into a temporary.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_map.py (group_map O_MOVS1: a
 * special register only in s1, s2 or s4; the source in s1 through the
 * mux, is4 from fte, the move in phase 2 with the first write enabled).
 */
static int
special_group(
	struct powervr_usc_group *group,
	unsigned destination,
	unsigned source)
{
	int error;

	/* A main group from the special register in s1 to one temporary. */
	*group = blank_group;
	group->unit = POWERVR_USC_UNIT_MAIN;
	group->write_0 = 1;
	group->source[1].used = 1;
	group->source[1].bank = POWERVR_USC_BANK_SPECIAL;
	group->source[1].index = (uint16_t)source;
	temporary(&group->destination[0], destination);
	group->mux_used = 1;
	group->mux = USC_MUX_S1;
	group->internal[4] = USC_SELECT_FTE;

	/* The third phase moves fte to the first write. */
	error = powervr_usc_move(USC_SELECT_FT0, USC_SELECT_FTE, 0xf, 1, 1, 1, &group->operation[POWERVR_USC_PHASE_2]);
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/*
 * Builds a main group of a 32-bit unsigned multiply-add of temporaries.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_map.py (group_map O_IMADD32:
 * phase 0 only, the first write enabled, the sources in s0, s1 and s2,
 * is4 from ft0; encode_map O_IMADD32: the short form without a carry
 * input).
 */
static int
multiply_add_group(
	struct powervr_usc_group *group,
	unsigned destination,
	unsigned first,
	unsigned second,
	unsigned addend)
{
	int error;

	/* A main group from three temporaries to one. */
	*group = blank_group;
	group->unit = POWERVR_USC_UNIT_MAIN;
	group->write_0 = 1;
	temporary(&group->source[0], first);
	temporary(&group->source[1], second);
	temporary(&group->source[2], addend);
	temporary(&group->destination[0], destination);
	group->internal[4] = USC_SELECT_FT0;

	/* The first phase multiplies and adds. */
	error = powervr_usc_integer(POWERVR_USC_INTEGER_MULTIPLY_ADD_32, 0, 0, 0, 0, 0, 0, 0, 0, &group->operation[POWERVR_USC_PHASE_0]);
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/*
 * Builds a main group of a 64-bit add of a 32-bit word: the pair at base
 * and base + 1 plus the temporary offset, written to the pair at
 * destination and destination + 1 (the low half from ft0, the high from
 * fte), as an address is made.
 *
 * The destination pair may be the base pair: the add in place was wrong
 * on the board only while the workgroup's data sequencer program let
 * instances' registers overlap; with Mesa's program it is right (the
 * rule probe shows it), as Mesa's compiler emits it.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_map.py (group_map O_ADD64_32:
 * phase 0, both writes, the sources in s0, s1 and s2, is4 from ft0 and
 * is5 from fte; encode_map O_ADD64_32: the short form, add6432);
 * zedBSD src/drivers/gpu/powervr/compiler/shader-backend.c (MACHINE_ADD64).
 */
static int
add64_group(
	struct powervr_usc_group *group,
	unsigned destination,
	unsigned base,
	unsigned offset)
{
	int error;

	/* A main group from the base pair and the offset to the destination pair. */
	*group = blank_group;
	group->unit = POWERVR_USC_UNIT_MAIN;
	group->write_0 = 1;
	group->write_1 = 1;
	temporary(&group->source[0], base);
	temporary(&group->source[1], base + 1U);
	temporary(&group->source[2], offset);
	temporary(&group->destination[0], destination);
	temporary(&group->destination[1], destination + 1U);
	group->internal[4] = USC_SELECT_FT0;
	group->internal[5] = USC_SELECT_FTE;

	/* The first phase adds. */
	error = powervr_usc_integer(POWERVR_USC_INTEGER_ADD_64_FROM_32, 0, 0, 0, 0, 0, 0, 0, 0, &group->operation[POWERVR_USC_PHASE_0]);
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/*
 * Builds a main group that stores one 32-bit word.
 *
 * The data must be in the register after the address pair (address + 2),
 * as Mesa lays the address and the data out in one vector for st32: the
 * board stored another register's value when the data was elsewhere
 * (2026-10-20), whatever the data selector named.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_map.py (group_map O_ST32: the
 * back end only, no write, the data in s0 and the address pair in s3;
 * encode_map O_ST: the immediate burst, the data return counter, the
 * address and data selectors and the size).
 */
static int
store_group(
	struct powervr_usc_group *group,
	unsigned data,
	unsigned address)
{
	int error;

	/* Refuses data that is not right after the address pair. */
	if (data != address + 2U)
		return EINVAL;

	/* A main group reading the data and the address pair. */
	*group = blank_group;
	group->unit = POWERVR_USC_UNIT_MAIN;
	temporary(&group->source[0], data);
	temporary(&group->source[3], address);

	/* The back end stores one word through data return counter 0. */
	error = powervr_usc_store(3, 1, 0, 0, USC_SIZE_32, USC_WRITE_THROUGH, 0, &group->operation[POWERVR_USC_PHASE_BACKEND]);
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/*
 * Builds a control group that waits for data return counter 0.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_map.py (group_map O_WDF: the
 * counter in the header's miscellaneous bit, no operation bytes).
 */
static int
wait_group(
	struct powervr_usc_group *group)
{
	int error;

	/* A control group naming the wait. */
	*group = blank_group;
	group->unit = POWERVR_USC_UNIT_CONTROL;
	group->control = POWERVR_USC_CONTROL_WAIT_DATA;

	/* The wait has no fields. */
	error = powervr_usc_empty(&group->operation[POWERVR_USC_PHASE_0]);
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/*
 * Builds the control group that ends the program.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_map.py (group_map O_NOP: the end
 * in the header's miscellaneous bit).
 */
static int
end_group(
	struct powervr_usc_group *group)
{
	int error;

	/* A control group naming the no-operation, marked as the end. */
	*group = blank_group;
	group->unit = POWERVR_USC_UNIT_CONTROL;
	group->control = POWERVR_USC_CONTROL_NOTHING;
	group->control_misc = 1;

	/* The no-operation's single byte. */
	error = powervr_usc_nothing(&group->operation[POWERVR_USC_PHASE_0]);
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/* Names a temporary as an operand. */
static void
temporary(
	struct powervr_usc_register *operand,
	unsigned index)
{
	/* The temporary bank and the register's number. */
	operand->used = 1;
	operand->bank = POWERVR_USC_BANK_TEMPORARY;
	operand->index = (uint16_t)index;
}
