/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GSI1 compiler's spills: values kept in the warp's scratch memory
 * when the registers of a lane do not hold them all.
 *
 * A compile that may spill reads the scratch memory's address from the
 * driver's table (set 3, the word of its stage) and computes, at the
 * program's start, the lane's address in its warp's part into a register
 * of its own: address + (warp x words per warp + lane) x 4.  Word s of a lane is
 * then 128 s from that register, so that the 32 lanes of a store or a
 * load are one run of 128 bytes.
 *
 * When a register is asked for and none is free, the values in the
 * registers that the instruction being lowered does not read are the
 * candidates, and the one read again last goes (the run of registers
 * whose values are read again last, for a run).  A spilled value is
 * stored right after each place where it was defined (sw.x, which every
 * lane in EXEC makes, helper and killed lanes too), under the EXEC that
 * defined it, so the memory holds what the register held for every lane;
 * every later read loads it into a scratch register for its instruction,
 * and every later definition (a loop variable's MOVE) stores it again.
 *
 * Two values share a scratch word when the first one's life, from its
 * first definition to its last read or write, ends before the second's
 * begins.
 */

#include "gsi1-private.h"

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include <stddef.h>
#include <stdint.h>

/* The most scratch words: the offset of the last one fits the 22-bit immediate of lw and sw. */
#define GSI1_SPILL_SLOTS_MAX		16383U

/* The scratch words and the definitions the lists grow by. */
#define GSI1_SPILL_GROWTH		64U

/* The registers' values when the lowering looks for room: each register's value, or GSI1_NOT_DEFINED. */
struct gsi1_owners {
	uint32_t value[GSI1_REGISTERS_MAX];
};

static int prepare_lives(struct gsi1_lowering *lowering);
static int prepare_readers(struct gsi1_lowering *lowering);
static void find_owners(const struct gsi1_lowering *lowering, struct gsi1_owners *owners);
static int spillable(const struct gsi1_lowering *lowering, uint32_t value);
static uint32_t next_read(const struct gsi1_lowering *lowering, uint32_t value);
static int spill_value(struct gsi1_lowering *lowering, uint32_t value);
static int take_slot(struct gsi1_lowering *lowering, uint32_t value, uint32_t *slot);
static uint32_t scratch_word(enum drv_gpu_shader_stage stage);
static int add_definition(struct gsi1_lowering *lowering, uint32_t value, uint32_t reg);
static uint64_t store_word(const struct gsi1_lowering *lowering, uint32_t reg, uint32_t slot);

/*
 * Plans the spills of a compile that may spill: each value's first
 * definition, the end of its life, and its readers in order.
 *
 * Does nothing for a compile that does not spill.  Returns 0 or ENOMEM.
 */
int
gsi1_spill_prepare(
	struct gsi1_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	int error;

	/* Only a compile that may spill plans its spills. */
	if (lowering->spill.enabled == 0)
		return 0;

	/* Names the module being planned. */
	ir = lowering->ir;

	/* Allocates each value's scratch word. */
	lowering->spill.slot = kern_calloc(ir->value_count + 1U, sizeof(*lowering->spill.slot));
	if (lowering->spill.slot == NULL)
		return ENOMEM;

	/* Allocates the mark of the values read by the instruction being lowered. */
	lowering->spill.pinned = kern_calloc(ir->value_count + 1U, sizeof(*lowering->spill.pinned));
	if (lowering->spill.pinned == NULL)
		return ENOMEM;

	/* Allocates the mark of the values whose definitions are listed. */
	lowering->spill.recorded = kern_calloc(ir->value_count + 1U, sizeof(*lowering->spill.recorded));
	if (lowering->spill.recorded == NULL)
		return ENOMEM;

	/* Each value's life. */
	error = prepare_lives(lowering);
	if (error != 0)
		return error;

	/* Each value's readers. */
	error = prepare_readers(lowering);
	if (error != 0)
		return error;

	/* The marks start before the first instruction (stamp 1 is the first one's). */
	lowering->spill.pin_stamp = 1U;

	/* Succeeded: the spills can be planned as the lowering goes. */
	return 0;
}

/*
 * Computes, at the program's start, the lane's address in its warp's part
 * of the scratch memory into a register kept to the end, from the
 * address in the driver's table.  The words per warp are filled in by
 * gsi1_spill_finish().
 */
int
gsi1_spill_prologue(
	struct gsi1_lowering *lowering)
{
	uint32_t base;
	uint32_t address;
	uint32_t mark;
	int error;

	/* Only a compile that may spill has the address. */
	if (lowering->spill.enabled == 0)
		return 0;

	/* The register of the lane's address, kept to the end of the program. */
	error = gsi1_take_register(lowering, &base);
	if (error != 0)
		return error;
	lowering->spill.base = base;

	/* The words of a warp's part (a movi filled in at the end), times the warp's number. */
	lowering->spill.patch = lowering->count;
	gsi1_emit(lowering, gsi1_encode_movi(base, 0U));
	gsi1_emit_alu(lowering, GSI1_F3_MUL, GSI1_F7_MULDIV, base, base, GSI1_WARP);

	/* Plus the lane, in bytes. */
	gsi1_emit_alu(lowering, GSI1_F3_ADD, GSI1_F7_BASE, base, base, GSI1_LANE);
	gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SLL, GSI1_F7_BASE, base, base, 2U));

	/* Plus the scratch memory's address, read from the driver's table. */
	mark = gsi1_scratch_mark(lowering);
	error = gsi1_driver_word(lowering, scratch_word(lowering->ir->stage), &address);
	if (error != 0)
		return error;
	gsi1_emit_alu(lowering, GSI1_F3_ADD, GSI1_F7_BASE, base, base, address);
	gsi1_scratch_release(lowering, mark);

	/* A list that could not grow fails the compile. */
	if (lowering->out_of_memory != 0)
		return ENOMEM;

	/* Succeeded: the lane's address is in its register. */
	return 0;
}

/* Starts the lowering of an instruction: the values read before it may be spilled again. */
void
gsi1_spill_next(
	struct gsi1_lowering *lowering)
{
	/* Only a compile that may spill marks its values. */
	if (lowering->spill.enabled == 0)
		return;

	/*
	 * A new stamp: the values marked with the old one were read by the
	 * instruction just lowered, which no longer holds their registers.
	 */
	lowering->spill.pin_stamp++;
}

/*
 * Keeps a value in its register until the instruction being lowered is
 * done: the code being made reads the register.
 */
void
gsi1_spill_pin(
	struct gsi1_lowering *lowering,
	uint32_t value)
{
	/* Only a compile that may spill marks its values. */
	if (lowering->spill.enabled == 0)
		return;

	/* The value carries the stamp of the instruction being lowered. */
	lowering->spill.pinned[value] = lowering->spill.pin_stamp;
}

/*
 * Frees a run of `count` registers by moving the values in it into the
 * scratch memory: the run where every register is free or holds a value
 * that may go, with the fewest values, of those the one whose values are
 * read again latest.
 *
 * Returns 0 with the run's first register in *first (free, not taken),
 * ENOTSUP when no run can be freed, or ENOMEM.
 */
int
gsi1_spill_make_room(
	struct gsi1_lowering *lowering,
	uint32_t count,
	uint32_t *first)
{
	struct gsi1_owners owners;
	uint32_t start;
	uint32_t reg;
	uint32_t value;
	uint32_t cost;
	uint32_t nearest;
	uint32_t read;
	uint32_t best;
	uint32_t best_cost;
	uint32_t best_nearest;
	int feasible;
	int can_go;
	int error;

	/* A compile that does not spill has no room to make. */
	if (lowering->spill.enabled == 0)
		return ENOTSUP;

	/* Which value each register holds. */
	find_owners(lowering, &owners);

	/* Tries each run in turn. */
	best = GSI1_NOT_DEFINED;
	best_cost = 0U;
	best_nearest = 0U;
	for (start = 0U; start + count <= lowering->register_limit; start++) {
		/* The run's values: each must be allowed to go; the cost is their number, the nearest read decides a tie. */
		feasible = 1;
		cost = 0U;
		nearest = GSI1_NOT_DEFINED;
		for (reg = start; reg < start + count; reg++) {
			if (lowering->taken[reg] == 0U)
				continue;

			/* A register held by anything but a value that may go (an input, a scratch, a saved EXEC) ends the try. */
			value = owners.value[reg];
			if (value == GSI1_NOT_DEFINED) {
				feasible = 0;
				break;
			}
			can_go = spillable(lowering, value);
			if (can_go == 0) {
				feasible = 0;
				break;
			}

			/* The value counts, and its next read. */
			cost++;
			read = next_read(lowering, value);
			if (read < nearest)
				nearest = read;
		}

		/* A run that cannot be freed is passed. */
		if (feasible == 0)
			continue;

		/* The best run so far: fewer values, or as many read again later. */
		if (best == GSI1_NOT_DEFINED) {
			best = start;
			best_cost = cost;
			best_nearest = nearest;
		} else if (cost < best_cost) {
			best = start;
			best_cost = cost;
			best_nearest = nearest;
		} else if (cost == best_cost && nearest > best_nearest) {
			best = start;
			best_nearest = nearest;
		}
	}

	/* No run can be freed: the program needs more registers than spills give. */
	if (best == GSI1_NOT_DEFINED)
		return ENOTSUP;

	/* Spills every value of the run. */
	for (reg = best; reg < best + count; reg++) {
		if (lowering->taken[reg] == 0U)
			continue;

		/* The value goes to the scratch memory. */
		error = spill_value(lowering, owners.value[reg]);
		if (error != 0)
			return error;
	}

	/* Succeeded: the run is free. */
	*first = best;
	return 0;
}

/* Reports whether a value is kept in the scratch memory. */
int
gsi1_spill_is_spilled(
	const struct gsi1_lowering *lowering,
	uint32_t value)
{
	/* Only a compile that may spill has spilled values. */
	if (lowering->spill.enabled == 0)
		return 0;

	/* A value with a scratch word lives there. */
	if (lowering->spill.slot[value] != 0U)
		return 1;

	/* The value is in a register, or not a register value. */
	return 0;
}

/*
 * Reads a spilled value into a scratch register of the instruction being
 * lowered: lw from its word of the lane's part.
 */
int
gsi1_spill_reload(
	struct gsi1_lowering *lowering,
	uint32_t value,
	uint32_t *operand)
{
	uint32_t reg;
	int32_t offset;
	int error;

	/* Takes the scratch register the value is read into. */
	error = gsi1_scratch(lowering, &reg);
	if (error != 0)
		return error;

	/* lw reg, 128 s(base). */
	offset = (int32_t)((lowering->spill.slot[value] - 1U) * GSI1_SPILL_SLOT_BYTES);
	gsi1_emit(lowering, gsi1_encode_i(GSI1_OP_LOAD, GSI1_F3_LW, reg, lowering->spill.base, offset, 0U));

	/* Succeeded: the value is in the scratch register. */
	*operand = reg;
	return 0;
}

/*
 * Gives a spilled value (a loop variable defined again) a scratch register
 * to be defined in; gsi1_spill_defined() stores it after the instruction.
 */
int
gsi1_spill_destination(
	struct gsi1_lowering *lowering,
	uint32_t value,
	uint32_t *reg)
{
	int error;

	/* Takes the scratch register the instruction writes. */
	error = gsi1_scratch(lowering, reg);
	if (error != 0)
		return error;

	/* The value is written there for this instruction. */
	lowering->operand[value] = (uint8_t)*reg;

	/* Succeeded: the instruction defines the value in the scratch register. */
	return 0;
}

/*
 * Records, after an instruction is lowered, where each register value it
 * defined lives (a spill later stores it there), and stores the values it
 * defined that are spilled already.
 */
int
gsi1_spill_defined(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	uint32_t channel;
	uint32_t value;
	int defines;
	int error;

	/* Only a compile that may spill records its definitions. */
	if (lowering->spill.enabled == 0)
		return 0;

	/* Each value the instruction defined. */
	defines = gsi1_ir_defines(instruction);
	for (channel = 0U; channel < (uint32_t)defines; channel++) {
		value = instruction->dst + channel;
		if (lowering->kind[value] != GSI1_VALUE_REGISTER)
			continue;
		if (lowering->operand[value] == GSI1_NO_OPERAND)
			continue;

		/* A spilled value goes from its scratch register to its word, and lives there again. */
		if (lowering->spill.slot[value] != 0U) {
			gsi1_emit(lowering, store_word(lowering, lowering->operand[value], lowering->spill.slot[value] - 1U));
			lowering->operand[value] = GSI1_NO_OPERAND;
			continue;
		}

		/* A value in a register: where it was written, for a spill later. */
		error = add_definition(lowering, value, lowering->operand[value]);
		if (error != 0)
			return error;
	}

	/* Succeeded: the definitions are recorded. */
	return 0;
}

/*
 * Fills in the words of a warp's part in the start's movi and the bytes
 * per warp of the program, once every spill is known.  A compile that
 * may spill reads the driver's table at its start, so it asks for one
 * scratch word at least.
 */
int
gsi1_spill_finish(
	struct gsi1_lowering *lowering)
{
	uint32_t words;
	uint32_t slots;

	/* Only a compile that may spill has the size to fill in. */
	if (lowering->spill.enabled == 0)
		return 0;

	/* The words of a warp's part: 32 for each scratch word. */
	slots = lowering->spill.slot_count;
	if (slots == 0U)
		slots = 1U;
	words = slots * (GSI1_SPILL_SLOT_BYTES / 4U);
	lowering->code[lowering->spill.patch] = gsi1_encode_movi(lowering->spill.base, words);

	/* The program's bytes per warp. */
	lowering->spill.bytes = words * 4U;

	/* Succeeded: the scratch memory's size is in the program. */
	return 0;
}

/* Frees the spill tables. */
void
gsi1_spill_free(
	struct gsi1_lowering *lowering)
{
	/* The per-value tables. */
	kern_free(lowering->spill.slot);
	kern_free(lowering->spill.first_definition);
	kern_free(lowering->spill.last_touch);
	kern_free(lowering->spill.recorded);
	kern_free(lowering->spill.pinned);

	/* The readers, the scratch words and the definitions. */
	kern_free(lowering->spill.reader_start);
	kern_free(lowering->spill.reader);
	kern_free(lowering->spill.slot_busy);
	kern_free(lowering->spill.definition);
}

/*
 * Finds each value's first definition and the end of its life: after its
 * last read and after its last definition (a loop variable may be moved
 * into after its last read).
 */
static int
prepare_lives(
	struct gsi1_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t index;
	uint32_t channel;
	uint32_t value;
	int defines;

	/* Names the module being planned. */
	ir = lowering->ir;

	/* Allocates each value's first definition. */
	lowering->spill.first_definition = kern_calloc(ir->value_count + 1U, sizeof(*lowering->spill.first_definition));
	if (lowering->spill.first_definition == NULL)
		return ENOMEM;

	/* Allocates the end of each value's life. */
	lowering->spill.last_touch = kern_calloc(ir->value_count + 1U, sizeof(*lowering->spill.last_touch));
	if (lowering->spill.last_touch == NULL)
		return ENOMEM;

	/* No value is defined before the instructions are read; each lives at least to its last read. */
	for (value = 0U; value <= ir->value_count; value++) {
		lowering->spill.first_definition[value] = GSI1_NOT_DEFINED;
		lowering->spill.last_touch[value] = lowering->last_use[value];
	}

	/* Each definition that is lowered: the first one, and the life to past the last one. */
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		if (lowering->dead[index] != 0U)
			continue;

		/* The values the instruction defines. */
		defines = gsi1_ir_defines(instruction);
		for (channel = 0U; channel < (uint32_t)defines; channel++) {
			value = instruction->dst + channel;
			if (lowering->spill.first_definition[value] == GSI1_NOT_DEFINED)
				lowering->spill.first_definition[value] = index;
			if (lowering->spill.last_touch[value] < index + 1U)
				lowering->spill.last_touch[value] = index + 1U;
		}
	}

	/* Succeeded: every value's life is known. */
	return 0;
}

/* Lists each value's readers among the instructions that are lowered, in order. */
static int
prepare_readers(
	struct gsi1_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	uint32_t sources[GSI1_MAX_SOURCES];
	uint32_t *fill;
	uint32_t index;
	uint32_t source;
	uint32_t value;
	uint32_t total;
	int count;

	/* Names the module being planned. */
	ir = lowering->ir;

	/* Allocates where each value's readers start in the list. */
	lowering->spill.reader_start = kern_calloc(ir->value_count + 2U, sizeof(*lowering->spill.reader_start));
	if (lowering->spill.reader_start == NULL)
		return ENOMEM;

	/* Counts each value's readers. */
	for (index = 0U; index < ir->instruction_count; index++) {
		if (lowering->dead[index] != 0U)
			continue;
		count = gsi1_ir_sources(&ir->instructions[index], sources);
		for (source = 0U; count > 0 && source < (uint32_t)count; source++)
			lowering->spill.reader_start[sources[source] + 1U]++;
	}

	/* Turns the counts into where each value's readers start. */
	for (value = 0U; value <= ir->value_count; value++)
		lowering->spill.reader_start[value + 1U] += lowering->spill.reader_start[value];
	total = lowering->spill.reader_start[ir->value_count + 1U];

	/* Allocates the list of readers. */
	lowering->spill.reader = kern_calloc(total + 1U, sizeof(*lowering->spill.reader));
	if (lowering->spill.reader == NULL)
		return ENOMEM;

	/* Allocates where each value's next reader goes while the list is filled. */
	fill = kern_calloc(ir->value_count + 1U, sizeof(*fill));
	if (fill == NULL)
		return ENOMEM;

	/* Fills the list in instruction order, so each value's readers are in order. */
	for (value = 0U; value <= ir->value_count; value++)
		fill[value] = lowering->spill.reader_start[value];
	for (index = 0U; index < ir->instruction_count; index++) {
		if (lowering->dead[index] != 0U)
			continue;
		count = gsi1_ir_sources(&ir->instructions[index], sources);
		for (source = 0U; count > 0 && source < (uint32_t)count; source++) {
			lowering->spill.reader[fill[sources[source]]] = index;
			fill[sources[source]]++;
		}
	}

	/* The fill positions are only the listing's. */
	kern_free(fill);

	/* Succeeded: every value's readers are listed. */
	return 0;
}

/* Finds which value each register holds. */
static void
find_owners(
	const struct gsi1_lowering *lowering,
	struct gsi1_owners *owners)
{
	uint32_t reg;
	uint32_t value;
	uint32_t operand;

	/* No register holds a value until one is found in it. */
	for (reg = 0U; reg < GSI1_REGISTERS_MAX; reg++)
		owners->value[reg] = GSI1_NOT_DEFINED;

	/* Each register value that has a register of its own. */
	for (value = 0U; value < lowering->ir->value_count; value++) {
		if (lowering->kind[value] != GSI1_VALUE_REGISTER)
			continue;
		if (lowering->spill.slot[value] != 0U)
			continue;
		operand = lowering->operand[value];
		if (operand >= lowering->register_limit)
			continue;

		/* The register holds the value. */
		owners->value[operand] = value;
	}
}

/*
 * Reports whether a value in a register may go to the scratch memory: the
 * instruction being lowered does not read or define it, its definitions
 * are known, and no code made already reads its register again.
 *
 * The last is a loop's: a value made before a loop being lowered that the
 * loop's body read before this instruction is read there again by the
 * next pass, from the register, which a spill would give to something
 * else.
 */
static int
spillable(
	const struct gsi1_lowering *lowering,
	uint32_t value)
{
	uint32_t depth;
	uint32_t start;
	uint32_t low;
	uint32_t high;
	uint32_t middle;

	/* A value the instruction being lowered reads or defines stays. */
	if (lowering->spill.pinned[value] == lowering->spill.pin_stamp)
		return 0;

	/* A value whose definitions are not listed cannot be stored where it was made. */
	if (lowering->spill.recorded[value] == 0U)
		return 0;

	/* The outermost loop being lowered that started after the value was first defined. */
	start = GSI1_NOT_DEFINED;
	for (depth = 0U; depth < lowering->loop_depth; depth++) {
		if (lowering->loop_start[depth] > lowering->spill.first_definition[value]) {
			start = lowering->loop_start[depth];
			break;
		}
	}

	/* No such loop: nothing made already runs again. */
	if (start == GSI1_NOT_DEFINED)
		return 1;

	/* The value's first reader from the loop's start on. */
	low = lowering->spill.reader_start[value];
	high = lowering->spill.reader_start[value + 1U];
	while (low < high) {
		middle = low + (high - low) / 2U;
		if (lowering->spill.reader[middle] < start) {
			low = middle + 1U;
		} else {
			high = middle;
		}
	}

	/* A reader in the loop before this instruction reads the register again in the next pass. */
	if (low < lowering->spill.reader_start[value + 1U] && lowering->spill.reader[low] < lowering->index)
		return 0;

	/* The value may go. */
	return 1;
}

/*
 * Reports the IR instruction that reads a value next after the one being
 * lowered: its next reader, or the end of its life (a value a loop reads
 * again at its start, or one the exports read).
 */
static uint32_t
next_read(
	const struct gsi1_lowering *lowering,
	uint32_t value)
{
	uint32_t low;
	uint32_t high;
	uint32_t middle;

	/* Searches the value's ordered readers for the first one after the instruction being lowered. */
	low = lowering->spill.reader_start[value];
	high = lowering->spill.reader_start[value + 1U];
	while (low < high) {
		middle = low + (high - low) / 2U;
		if (lowering->spill.reader[middle] <= lowering->index) {
			low = middle + 1U;
		} else {
			high = middle;
		}
	}

	/* A reader after this instruction. */
	if (low < lowering->spill.reader_start[value + 1U])
		return lowering->spill.reader[low];

	/* None: the end of the value's life. */
	return lowering->last_use[value];
}

/*
 * Moves a value from its register into a scratch word: a store after each
 * place it was defined, and the register goes back.
 */
static int
spill_value(
	struct gsi1_lowering *lowering,
	uint32_t value)
{
	uint32_t slot;
	uint32_t index;
	int error;

	/* The value's scratch word (none until take_slot() gives one). */
	slot = 0U;
	error = take_slot(lowering, value, &slot);
	if (error != 0)
		return error;

	/*
	 * A store after each definition, from the register written there, so
	 * the word holds what the register held for every lane (each
	 * insertion moves the later definitions, which are read again).
	 */
	for (index = 0U; index < lowering->spill.definition_count; index++) {
		if (lowering->spill.definition[index].value != value)
			continue;

		/* The store goes right after the definition's instructions. */
		gsi1_insert(lowering,
			    lowering->spill.definition[index].position,
			    store_word(lowering, lowering->spill.definition[index].reg, slot));
	}

	/* The register goes back, and the value lives in its word from now on. */
	gsi1_give_register(lowering, lowering->operand[value]);
	lowering->operand[value] = GSI1_NO_OPERAND;
	lowering->spill.slot[value] = slot + 1U;

	/* A list that could not grow fails the compile. */
	if (lowering->out_of_memory != 0)
		return ENOMEM;

	/* Succeeded: the value is spilled. */
	return 0;
}

/*
 * Takes a scratch word for a value: the first one whose values' lives all
 * end before the value is first defined, or a new one.
 */
static int
take_slot(
	struct gsi1_lowering *lowering,
	uint32_t value,
	uint32_t *slot)
{
	uint32_t *busy;
	uint32_t index;
	uint32_t start;
	uint32_t capacity;
	int error;

	/* Where the value's life starts, as the busy marks count (instruction plus one). */
	start = lowering->spill.first_definition[value] + 1U;

	/* A word that is free from the value's first definition on. */
	for (index = 0U; index < lowering->spill.slot_count; index++) {
		if (lowering->spill.slot_busy[index] > start)
			continue;

		/* The word is the value's until its life ends. */
		lowering->spill.slot_busy[index] = lowering->spill.last_touch[value];
		*slot = index;
		return 0;
	}

	/* The scratch memory has a bound. */
	if (lowering->spill.slot_count == GSI1_SPILL_SLOTS_MAX) {
		error = gsi1_refuse(lowering, ENOTSUP, "the program needs more scratch memory than an offset reaches");
		return error;
	}

	/* A full list of words grows. */
	if (lowering->spill.slot_count == lowering->spill.slot_capacity) {
		capacity = lowering->spill.slot_capacity + GSI1_SPILL_GROWTH;

		/* Allocates the larger list. */
		busy = kern_calloc(capacity, sizeof(*busy));
		if (busy == NULL)
			return ENOMEM;

		/* Moves the marks into it. */
		if (lowering->spill.slot_count != 0U)
			kern_memcpy(busy, lowering->spill.slot_busy, lowering->spill.slot_count * sizeof(*busy));
		kern_free(lowering->spill.slot_busy);
		lowering->spill.slot_busy = busy;
		lowering->spill.slot_capacity = capacity;
	}

	/* A new word, the value's until its life ends. */
	index = lowering->spill.slot_count;
	lowering->spill.slot_busy[index] = lowering->spill.last_touch[value];
	lowering->spill.slot_count++;

	/* Succeeded: the word is the value's. */
	*slot = index;
	return 0;
}

/* Reports the driver table's word that holds the scratch memory's address for a stage. */
static uint32_t
scratch_word(
	enum drv_gpu_shader_stage stage)
{
	/* The vertex and fragment warps have parts of their own. */
	if (stage == DRV_GPU_STAGE_VERTEX)
		return GSI1_ABI_DRIVER_SCRATCH_VERTEX;
	if (stage == DRV_GPU_STAGE_FRAGMENT)
		return GSI1_ABI_DRIVER_SCRATCH_FRAGMENT;

	/* Succeeded: a compute program's. */
	return GSI1_ABI_DRIVER_SCRATCH_COMPUTE;
}

/* Adds a definition of a register value at the end of the code so far. */
static int
add_definition(
	struct gsi1_lowering *lowering,
	uint32_t value,
	uint32_t reg)
{
	struct gsi1_definition *list;
	uint32_t capacity;

	/* A full list grows. */
	if (lowering->spill.definition_count == lowering->spill.definition_capacity) {
		capacity = lowering->spill.definition_capacity + GSI1_SPILL_GROWTH;

		/* Allocates the larger list. */
		list = kern_calloc(capacity, sizeof(*list));
		if (list == NULL)
			return ENOMEM;

		/* Moves the definitions into it. */
		if (lowering->spill.definition_count != 0U)
			kern_memcpy(list, lowering->spill.definition, lowering->spill.definition_count * sizeof(*list));
		kern_free(lowering->spill.definition);
		lowering->spill.definition = list;
		lowering->spill.definition_capacity = capacity;
	}

	/* The definition: the value, its register, and the code index after its instructions. */
	list = &lowering->spill.definition[lowering->spill.definition_count];
	list->value = value;
	list->reg = reg;
	list->position = lowering->count;
	lowering->spill.definition_count++;
	lowering->spill.recorded[value] = 1U;

	/* Succeeded: the definition is listed. */
	return 0;
}

/* Encodes the store of a register to a scratch word of the lane: sw.x reg, 128 s(base). */
static uint64_t
store_word(
	const struct gsi1_lowering *lowering,
	uint32_t reg,
	uint32_t slot)
{
	uint64_t instruction;
	int32_t offset;

	/* sw with the word's offset from the lane's address. */
	offset = (int32_t)(slot * GSI1_SPILL_SLOT_BYTES);
	instruction = gsi1_encode_s(GSI1_OP_STORE, GSI1_F3_SW, lowering->spill.base, reg, offset);

	/* The sub field 1, sw.x: every lane in EXEC stores, helper and killed lanes too. */
	instruction |= (uint64_t)(GSI1_SW_X << GSI1_W1_SUB_SHIFT) << 32;

	/* Succeeded: the store's instruction. */
	return instruction;
}
