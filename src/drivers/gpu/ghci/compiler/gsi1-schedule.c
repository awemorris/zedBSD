/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GSI1 compiler's scheduling: the pure instructions sunk to their
 * readers, for a program that does not fit its registers as the parser
 * ordered it.
 *
 * A value lives from its definition to its last read, so a value made
 * long before it is read holds a register all that time.  An instruction
 * that only computes from its sources (the arithmetic, the comparisons,
 * the selections, a push-constant word and a fragment input) can move
 * down to just before the first instruction that reads it.  The
 * instructions are visited from the last one up, so a chain that only
 * feeds one reader moves down with it.
 *
 * The rules of the move:
 * - It never crosses the start or the end of a loop or of a skippable
 *   block: the instructions between the old and the new place run under
 *   the same EXEC, so the value is the same for every lane.  A first
 *   reader inside a block that starts later takes the instruction to just
 *   before that block, and the end of the block the instruction is in
 *   stops it there.
 * - It never crosses a definition of one of its sources (the MOVE into a
 *   loop variable): the sources read the same values where it lands.
 * - Only a value defined once (not a loop variable) moves.
 */

#include "gsi1-private.h"

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include <stddef.h>
#include <stdint.h>

static int movable(const struct drv_gpu_shader_ir *ir, const struct drv_gpu_shader_ir_inst *instruction, const uint8_t *definitions);
static uint32_t find_place(const struct drv_gpu_shader_ir_inst *list, uint32_t count, uint32_t at);
static int reads_value(const struct drv_gpu_shader_ir_inst *instruction, uint32_t value);
static int defines_value(const struct drv_gpu_shader_ir_inst *instruction, uint32_t value);
static int opens_block(const struct drv_gpu_shader_ir_inst *instruction);
static int closes_block(const struct drv_gpu_shader_ir_inst *instruction);

/*
 * Makes a copy of the instructions with each pure one moved down to its
 * first reader.
 *
 * Returns 0 with the copy in *instructions (the caller frees it with
 * kern_free()), or ENOMEM.
 */
int
gsi1_schedule_sink(
	const struct drv_gpu_shader_ir *ir,
	struct drv_gpu_shader_ir_inst **instructions)
{
	struct drv_gpu_shader_ir_inst *list;
	struct drv_gpu_shader_ir_inst moved;
	uint8_t *definitions;
	uint32_t index;
	uint32_t at;
	uint32_t place;
	uint32_t value;
	int defines;
	int can_move;

	/* Nothing is handed over until the copy is made. */
	*instructions = NULL;

	/* Allocates the copy of the instructions. */
	list = kern_calloc(ir->instruction_count + 1U, sizeof(*list));
	if (list == NULL)
		return ENOMEM;

	/* Allocates the count of each value's definitions. */
	definitions = kern_calloc(ir->value_count + 1U, sizeof(*definitions));
	if (definitions == NULL) {
		kern_free(list);
		return ENOMEM;
	}

	/* Copies the instructions as the parser ordered them. */
	if (ir->instruction_count != 0U)
		kern_memcpy(list, ir->instructions, ir->instruction_count * sizeof(*list));

	/* Counts each value's definitions (two for a loop variable), up to two. */
	for (index = 0U; index < ir->instruction_count; index++) {
		defines = gsi1_ir_defines(&list[index]);
		for (value = 0U; value < (uint32_t)defines; value++) {
			if (list[index].dst + value >= ir->value_count)
				continue;
			if (definitions[list[index].dst + value] < 2U)
				definitions[list[index].dst + value]++;
		}

		/* A MOVE's target is a loop variable even when it is defined once. */
		if (list[index].op == DRV_GPU_IR_MOVE && list[index].dst < ir->value_count)
			definitions[list[index].dst] = 2U;
	}

	/* From the last instruction up: each pure one moves down to its place. */
	for (at = ir->instruction_count; at > 0U; at--) {
		index = at - 1U;
		can_move = movable(ir, &list[index], definitions);
		if (can_move == 0)
			continue;

		/* The place it goes to: before the instruction now at `place`. */
		place = find_place(list, ir->instruction_count, index);
		if (place <= index + 1U)
			continue;

		/* The instructions between move up by one, and the instruction goes before the place. */
		moved = list[index];
		kern_memmove(&list[index], &list[index + 1U], (place - 1U - index) * sizeof(*list));
		list[place - 1U] = moved;
	}

	/* The counts are only the scheduling's. */
	kern_free(definitions);

	/* Succeeded: the caller owns the copy. */
	*instructions = list;
	return 0;
}

/*
 * Reports whether an instruction is pure and may move: it defines one
 * value, defined nowhere else, from its sources alone.
 */
static int
movable(
	const struct drv_gpu_shader_ir *ir,
	const struct drv_gpu_shader_ir_inst *instruction,
	const uint8_t *definitions)
{
	int defines;

	/* One value, of the program, defined only here. */
	defines = gsi1_ir_defines(instruction);
	if (defines != 1)
		return 0;
	if (instruction->dst >= ir->value_count)
		return 0;
	if (definitions[instruction->dst] != 1U)
		return 0;

	/*
	 * The operations that compute from their sources alone.  A fragment
	 * input interpolates the barycentrics, which stay put; a vertex input
	 * is a copy of its attribute's register, which would only live longer
	 * in its place.
	 */
	switch (instruction->op) {
	case DRV_GPU_IR_FADD:
	case DRV_GPU_IR_FSUB:
	case DRV_GPU_IR_FMUL:
	case DRV_GPU_IR_FMIN:
	case DRV_GPU_IR_FMAX:
	case DRV_GPU_IR_PACK_HALF:
	case DRV_GPU_IR_FNEG:
	case DRV_GPU_IR_FABS:
	case DRV_GPU_IR_RSQ:
	case DRV_GPU_IR_RCP:
	case DRV_GPU_IR_SQRT:
	case DRV_GPU_IR_EXP2:
	case DRV_GPU_IR_LOG2:
	case DRV_GPU_IR_SIN:
	case DRV_GPU_IR_COS:
	case DRV_GPU_IR_FLOOR:
	case DRV_GPU_IR_FRACT:
	case DRV_GPU_IR_FTRUNC:
	case DRV_GPU_IR_FROUND_EVEN:
	case DRV_GPU_IR_UNPACK_HALF:
	case DRV_GPU_IR_I2F:
	case DRV_GPU_IR_U2F:
	case DRV_GPU_IR_F2I:
	case DRV_GPU_IR_F2U:
	case DRV_GPU_IR_AND:
	case DRV_GPU_IR_OR:
	case DRV_GPU_IR_NOT:
	case DRV_GPU_IR_IADD:
	case DRV_GPU_IR_ISUB:
	case DRV_GPU_IR_IMUL:
	case DRV_GPU_IR_INEG:
	case DRV_GPU_IR_IAND:
	case DRV_GPU_IR_IOR:
	case DRV_GPU_IR_IXOR:
	case DRV_GPU_IR_INOT:
	case DRV_GPU_IR_SHL:
	case DRV_GPU_IR_SHR:
	case DRV_GPU_IR_ASR:
	case DRV_GPU_IR_UDIV:
	case DRV_GPU_IR_UMOD:
	case DRV_GPU_IR_IDIV:
	case DRV_GPU_IR_IREM:
	case DRV_GPU_IR_FLT:
	case DRV_GPU_IR_FGE:
	case DRV_GPU_IR_FEQ:
	case DRV_GPU_IR_FNEU:
	case DRV_GPU_IR_ILT:
	case DRV_GPU_IR_IGE:
	case DRV_GPU_IR_ULT:
	case DRV_GPU_IR_UGE:
	case DRV_GPU_IR_IEQ:
	case DRV_GPU_IR_INE:
	case DRV_GPU_IR_SELECT:
	case DRV_GPU_IR_LOAD_PUSH:
		return 1;
	case DRV_GPU_IR_LOAD_INPUT:
		/* Only a fragment program's input moves. */
		if (ir->stage == DRV_GPU_STAGE_FRAGMENT)
			return 1;
		return 0;
	default:
		/* Anything else stays where it is. */
		return 0;
	}
}

/*
 * Finds where the instruction at `at` goes: the index of the instruction
 * it is put before.  Returns at + 1 when it stays.
 */
static uint32_t
find_place(
	const struct drv_gpu_shader_ir_inst *list,
	uint32_t count,
	uint32_t at)
{
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t sources[GSI1_MAX_SOURCES];
	uint32_t source_count;
	uint32_t depth;
	uint32_t block_start;
	uint32_t index;
	uint32_t source;
	int listed;
	int reads;
	int defines;
	int opens;
	int closes;

	/* The sources whose definitions the instruction must not cross. */
	instruction = &list[at];
	listed = gsi1_ir_sources(instruction, sources);
	source_count = 0U;
	if (listed > 0)
		source_count = (uint32_t)listed;

	/* Walks down, following the blocks that start below the instruction. */
	depth = 0U;
	block_start = at + 1U;
	for (index = at + 1U; index < count; index++) {
		/* The first reader: just before it, or before the block it is in. */
		reads = reads_value(&list[index], instruction->dst);
		if (reads != 0) {
			if (depth == 0U)
				return index;
			return block_start;
		}

		/* A definition of a source stops the walk the same way. */
		for (source = 0U; source < source_count; source++) {
			defines = defines_value(&list[index], sources[source]);
			if (defines == 0)
				continue;
			if (depth == 0U)
				return index;
			return block_start;
		}

		/* A block that starts here: the first one at this depth is where a reader inside it sends the instruction. */
		opens = opens_block(&list[index]);
		if (opens != 0) {
			if (depth == 0U)
				block_start = index;
			depth++;
			continue;
		}

		/* The end of the instruction's own block stops it before the end. */
		closes = closes_block(&list[index]);
		if (closes != 0) {
			if (depth == 0U)
				return index;
			depth--;
		}
	}

	/* Nothing reads it: it stays (the lowering drops it). */
	return at + 1U;
}

/* Reports whether an instruction reads a value. */
static int
reads_value(
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t value)
{
	uint32_t sources[GSI1_MAX_SOURCES];
	uint32_t source;
	int count;

	/* The values it reads. */
	count = gsi1_ir_sources(instruction, sources);
	for (source = 0U; count > 0 && source < (uint32_t)count; source++) {
		if (sources[source] == value)
			return 1;
	}

	/* The value is not among them. */
	return 0;
}

/* Reports whether an instruction defines a value. */
static int
defines_value(
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t value)
{
	int defines;

	/* The run of values it defines from its destination. */
	defines = gsi1_ir_defines(instruction);
	if (defines == 0)
		return 0;
	if (value < instruction->dst)
		return 0;
	if (value - instruction->dst >= (uint32_t)defines)
		return 0;

	/* The value is in the run. */
	return 1;
}

/* Reports whether an instruction starts a loop or a skippable block. */
static int
opens_block(
	const struct drv_gpu_shader_ir_inst *instruction)
{
	/* The two starts of a block. */
	if (instruction->op == DRV_GPU_IR_LOOP_BEGIN)
		return 1;
	if (instruction->op == DRV_GPU_IR_SKIP_BEGIN)
		return 1;

	/* Anything else is inside a block. */
	return 0;
}

/* Reports whether an instruction ends a loop or a skippable block. */
static int
closes_block(
	const struct drv_gpu_shader_ir_inst *instruction)
{
	/* The two ends of a block. */
	if (instruction->op == DRV_GPU_IR_LOOP_END)
		return 1;
	if (instruction->op == DRV_GPU_IR_SKIP_END)
		return 1;

	/* Anything else is inside a block. */
	return 0;
}
