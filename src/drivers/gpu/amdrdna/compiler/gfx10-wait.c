/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The waits, the hazard breaks and the assembly of the amdrdna shader
 * compiler (gfx10-wait.h).
 *
 * The waits: an instruction that reads or writes a register a memory
 * instruction has not yet filled waits first, by the counter of that
 * memory instruction, until the counter has fallen far enough.  The
 * vector memory loads and the shared memory accesses finish in the order
 * they were issued, so a register filled by one of them is ready once the
 * counter is no more than the number of those issued after it; a scalar
 * load may finish in any order and an export is only followed to its
 * end, so theirs wait for zero.  The export reads its registers and the
 * lane mask late, so a write to one of them waits for the export counter.
 *
 * The hazards: GFX10.1 has four sequences the hardware does not guard,
 * each broken by an instruction placed between their two halves (the
 * VMEMtoScalarWrite, SMEMtoVectorWrite, VcmpxExecWAR and LdsBranchVmemWAR
 * hazards).
 *
 * The pass walks the list once per pass.  What it knows at a label is
 * everything it knew on each path into it: the line that falls into the
 * label and each branch to it.  A loop's label learns of its back branch
 * only at the end of a pass, so the walk is repeated until no label
 * learns anything new; should that take too many passes, a last pass
 * waits for everything and breaks every hazard at each label and branch
 * instead, which is always correct.
 */

#include "gfx10-wait.h"

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include <stddef.h>
#include <stdint.h>

/* The operand codes a register can have: the scalar ones below 128, the VGPRs from 256. */
#define GFX10_CODES			512U

/* The scalar registers whose reads the hazards follow (SGPRs and the special ones), by code. */
#define GFX10_SCALAR_CODES		128U

/* The words of a bit set of the scalar registers. */
#define GFX10_SCALAR_SET_WORDS		(GFX10_SCALAR_CODES / 32U)

/* The instructions the pass may add before one instruction, and at the end. */
#define GFX10_ADDED_PER_INST		8U
#define GFX10_ADDED_AT_END		8U

/* The counters a register can wait on, indexed by enum amdrdna_gfx10_counter. */
#define GFX10_COUNTER_SLOTS		5U

/* A register no event of a counter has yet to fill. */
#define GFX10_NOT_PENDING		UINT8_MAX

/* The passes after which the labels' states are taken not to settle. */
#define GFX10_SETTLE_PASSES		16U

/*
 * The fields of the counter wait's immediate: the vector memory count's
 * low four bits at bit 0 and its high two bits at bit 14, the export
 * count at bit 4, the shared memory and scalar count at bit 8.  The
 * largest value of each field waits for nothing, so a register is counted
 * behind at most one less than it.
 *
 * Values referenced: Mesa 25.0.7 src/amd/compiler/aco_ir.cpp
 * (wait_imm::pack, the GFX10 case; wait_imm::max: vm 63, exp 7, lgkm 63).
 */
#define GFX10_WAIT_VECTOR_LOW_MASK	0xfU
#define GFX10_WAIT_VECTOR_HIGH_SHIFT	10U
#define GFX10_WAIT_VECTOR_HIGH_MASK	0x30U
#define GFX10_WAIT_EXPORT_SHIFT		4U
#define GFX10_WAIT_EXPORT_FIELD		0x7U
#define GFX10_WAIT_SCALAR_SHIFT		8U
#define GFX10_WAIT_SCALAR_FIELD		0x3fU
#define GFX10_VECTOR_COUNT_MAX		62U
#define GFX10_EXPORT_COUNT_MAX		6U
#define GFX10_SCALAR_COUNT_MAX		62U

/*
 * The fields of the dependency wait's immediate that say whether it waits
 * for the vector memory instructions to read their scalar operands (bits
 * 4:2) and for the scalar instructions to read theirs (bit 0); zero means
 * it waits.
 *
 * Values referenced: Mesa 25.0.7 src/amd/compiler/aco_insert_NOPs.cpp
 * (handle_instruction_gfx10: vm_vsrc = (imm >> 2) & 0x7, sa_sdst =
 * imm & 0x1).
 */
#define GFX10_DEPEND_VM_VSRC_SHIFT	2U
#define GFX10_DEPEND_VM_VSRC_MASK	0x7U
#define GFX10_DEPEND_SA_SDST_MASK	0x1U

/*
 * What the pass knows at one point of the code.
 *
 * One lives for the walk and one for each label; they are allocated by
 * amdrdna_gfx10_place_waits() and freed before it returns.  A label's
 * state only ever learns more (a register pending for fewer events, a
 * hazard open), so the passes end.
 */
struct wait_state {
	/*
	 * For each register (by code) and counter: the count the counter
	 * must fall to before the register is filled, or before an export
	 * has read it; GFX10_NOT_PENDING when no event of the counter holds
	 * it.
	 */
	uint8_t pending[GFX10_CODES][GFX10_COUNTER_SLOTS];

	/* For each register: the scalar counter event that holds it is a scalar load, which may finish in any order. */
	uint8_t unordered[GFX10_CODES];

	/*
	 * The scalar registers read by a vector memory load, by a vector
	 * memory store and by a shared memory access since the last vector
	 * ALU instruction or wait (VMEMtoScalarWrite), and by a scalar load
	 * since the last scalar ALU write (SMEMtoVectorWrite).
	 */
	uint32_t read_by_load[GFX10_SCALAR_SET_WORDS];
	uint32_t read_by_store[GFX10_SCALAR_SET_WORDS];
	uint32_t read_by_shared[GFX10_SCALAR_SET_WORDS];
	uint32_t read_by_scalar_load[GFX10_SCALAR_SET_WORDS];

	/* A non-vector instruction read the lane mask since the last vector ALU SGPR write (VcmpxExecWAR). */
	int lane_mask_read;

	/*
	 * A vector memory access, a shared memory access, and a branch after
	 * either, since the last store counter wait (LdsBranchVmemWAR).
	 */
	int vector_memory;
	int shared_memory;
	int branch_after_vector;
	int branch_after_shared;

	/* The last instruction was a scalar write of M0. */
	int m0_written;
};

/*
 * The list being built.
 *
 * Lives on the stack of amdrdna_gfx10_place_waits(); its array is handed
 * to the caller.
 */
struct wait_output {
	struct amdrdna_gfx10_inst *list;
	uint32_t count;
	uint32_t capacity;
	int track_branch_hazard;
};

static int walk(const struct amdrdna_gfx10_inst *list, uint32_t count, struct wait_output *output, struct wait_state *state, struct wait_state *labels, int conservative);
static void state_clear(struct wait_state *state);
static int state_join(struct wait_state *into, const struct wait_state *from);
static int join_set(uint32_t *into, const uint32_t *from);
static int join_flag(int *into, int from);
static int ends_line(const struct amdrdna_gfx10_inst *inst);
static void wait_for(struct wait_state *state, struct wait_output *output, const uint8_t *needed);
static void resolve_all(struct wait_state *state, struct wait_output *output);
static void break_hazards(struct wait_state *state, struct wait_output *output, const struct amdrdna_gfx10_inst *inst, const struct amdrdna_gfx10_access *access);
static void note_issued(struct wait_state *state, const struct amdrdna_gfx10_inst *inst, const struct amdrdna_gfx10_access *access);
static void count_behind(struct wait_state *state, unsigned counter);
static void hold_range(struct wait_state *state, const struct amdrdna_gfx10_range *range, unsigned counter, int ordered);
static void note_wait(struct wait_state *state, const struct amdrdna_gfx10_inst *inst);
static void release(struct wait_state *state, unsigned counter, unsigned count);
static void append(struct wait_output *output, const struct amdrdna_gfx10_inst *inst);
static void append_control(struct wait_output *output, unsigned opcode, uint32_t immediate);
static int needed_counts(const struct wait_state *state, const struct amdrdna_gfx10_access *access, uint8_t *needed);
static void need_range(const struct wait_state *state, const struct amdrdna_gfx10_range *range, int written, uint8_t *needed);
static unsigned count_limit(unsigned counter);
static int is_vector_alu(const struct amdrdna_gfx10_inst *inst);
static int is_scalar_alu(const struct amdrdna_gfx10_inst *inst);
static int is_vector_memory(const struct amdrdna_gfx10_inst *inst);
static int reads_range_in(const uint32_t *set, const struct amdrdna_gfx10_range *range);
static void add_range(uint32_t *set, const struct amdrdna_gfx10_range *range);
static int set_any(const uint32_t *set);
static void set_clear(uint32_t *set);
static int writes_code(const struct amdrdna_gfx10_access *access, unsigned code);
static int writes_scalar(const struct amdrdna_gfx10_access *access);
static int writes_any_in(const struct amdrdna_gfx10_access *access, const uint32_t *set);
static int reads_code(const struct amdrdna_gfx10_access *access, unsigned code);
static unsigned instruction_words(const struct amdrdna_gfx10_inst *inst, int *refused);
static int insert_idle_after(struct amdrdna_gfx10_inst **list, uint32_t *count, uint32_t index);

/*
 * Puts the waits and the hazard breaks into an instruction list.
 *
 * Each instruction is preceded by what it needs: a wait for the counters
 * of the registers it reads or writes that a memory instruction has not
 * filled, and the break of any hazard it would complete.  The walk is
 * repeated until every label knows all the paths into it.
 */
int
amdrdna_gfx10_place_waits(
	const struct amdrdna_gfx10_inst *list,
	uint32_t count,
	struct amdrdna_gfx10_inst **out,
	uint32_t *out_count)
{
	struct wait_output output;
	struct wait_state *state;
	struct wait_state *labels;
	uint32_t label_count;
	uint32_t index;
	uint32_t pass;
	int has_vector_memory;
	int has_shared_memory;
	int vector_memory;
	int changed;

	/* Starts with no output. */
	*out = NULL;
	*out_count = 0U;

	/* Makes room for every instruction and what may come before it. */
	kern_memset(&output, 0, sizeof(output));
	output.capacity = count * GFX10_ADDED_PER_INST + GFX10_ADDED_AT_END;
	output.list = kern_calloc(output.capacity, sizeof(*output.list));
	if (output.list == NULL)
		return ENOMEM;

	/* The branch hazard needs both a vector memory access and a shared memory access in the program. */
	has_vector_memory = 0;
	has_shared_memory = 0;
	label_count = 0U;
	for (index = 0U; index < count; index++) {
		vector_memory = is_vector_memory(&list[index]);
		if (vector_memory != 0)
			has_vector_memory = 1;
		if (list[index].kind == AMDRDNA_GFX10_KIND_SHARED)
			has_shared_memory = 1;

		/* The labels are numbered from 0; one past the highest named. */
		if (list[index].kind == AMDRDNA_GFX10_KIND_LABEL || list[index].kind == AMDRDNA_GFX10_KIND_BRANCH) {
			if (list[index].label + 1U > label_count)
				label_count = list[index].label + 1U;
		}
	}

	/* Branch hazards are tracked only when the program has both vector and shared memory. */
	output.track_branch_hazard = 0;
	if (has_vector_memory != 0 && has_shared_memory != 0)
		output.track_branch_hazard = 1;

	/* The state of the walk. */
	state = kern_calloc(1U, sizeof(*state));
	if (state == NULL) {
		kern_free(output.list);
		return ENOMEM;
	}

	/* Each label's state, knowing nothing of any path yet. */
	labels = kern_calloc(label_count + 1U, sizeof(*labels));
	if (labels == NULL) {
		kern_free(state);
		kern_free(output.list);
		return ENOMEM;
	}

	/* Clears the state of every label. */
	for (index = 0U; index <= label_count; index++)
		state_clear(&labels[index]);

	/* Walks until no label learns of a new path state. */
	changed = 1;
	for (pass = 0U; pass < GFX10_SETTLE_PASSES; pass++) {
		output.count = 0U;
		changed = walk(list, count, &output, state, labels, 0);
		if (changed == 0)
			break;
	}

	/* Labels that never settled: a last walk that settles everything at each label and branch. */
	if (changed != 0) {
		output.count = 0U;
		changed = walk(list, count, &output, state, labels, 1);
	}

	/* Releases the label states. */
	kern_free(labels);
	kern_free(state);

	/* Succeeded: the caller owns the new list. */
	*out = output.list;
	*out_count = output.count;
	return 0;
}

/*
 * Writes an instruction list as code.
 *
 * The branch distances are counted in words from the word after the
 * branch to the label.  A branch whose distance is the one GFX10
 * mishandles gets an idle instruction after it, and the distances are
 * counted again, until no branch has it.  The code is then padded with
 * the end-of-code instruction past what the prefetcher reads, to a whole
 * cache line.
 */
int
amdrdna_gfx10_assemble(
	struct amdrdna_gfx10_inst **list,
	uint32_t *count,
	uint32_t **words,
	uint32_t *word_count,
	uint32_t *failed)
{
	struct amdrdna_gfx10_inst *inst;
	uint32_t *offset;
	uint32_t *label_offset;
	uint32_t *code;
	uint32_t label_count;
	uint32_t index;
	uint32_t total;
	uint32_t padded;
	uint32_t written;
	uint32_t moved;
	int32_t distance;
	unsigned size;
	int refused;
	int again;
	int error;

	/* Starts with nothing laid out. */
	*words = NULL;
	*word_count = 0U;
	*failed = 0U;

	/* Counts the labels: one past the highest number used. */
	label_count = 0U;
	for (index = 0U; index < *count; index++) {
		inst = &(*list)[index];
		if (inst->kind == AMDRDNA_GFX10_KIND_LABEL || inst->kind == AMDRDNA_GFX10_KIND_BRANCH) {
			if (inst->label + 1U > label_count)
				label_count = inst->label + 1U;
		}
	}

	/* The label offsets live as long as the assembly. */
	label_offset = kern_calloc(label_count + 1U, sizeof(*label_offset));
	if (label_offset == NULL)
		return ENOMEM;

	/* Lays the code out until no branch has the mishandled distance. */
	offset = NULL;
	moved = 0U;
	do {
		again = 0;

		/* The offsets are counted afresh for the current list. */
		kern_free(offset);
		offset = kern_calloc(*count + 1U, sizeof(*offset));
		if (offset == NULL) {
			kern_free(label_offset);
			return ENOMEM;
		}

		/* Places every instruction and every label. */
		total = 0U;
		for (index = 0U; index < *count; index++) {
			inst = &(*list)[index];
			offset[index] = total;
			if (inst->kind == AMDRDNA_GFX10_KIND_LABEL)
				label_offset[inst->label] = total;

			/* An instruction the encoder refuses is reported. */
			size = instruction_words(inst, &refused);
			if (refused != 0) {
				kern_free(offset);
				kern_free(label_offset);
				*failed = index;
				return EINVAL;
			}

			/* Adds the instruction's words to the total. */
			total += size;
		}

		/* Resolves every branch, moving the first one that lands on the mishandled distance. */
		for (index = 0U; index < *count; index++) {
			inst = &(*list)[index];
			if (inst->kind != AMDRDNA_GFX10_KIND_BRANCH)
				continue;

			/* The distance from the word after the branch. */
			distance = (int32_t)label_offset[inst->label] - (int32_t)offset[index] - 1;
			if (distance < -32768 || distance > 32767) {
				kern_free(offset);
				kern_free(label_offset);
				*failed = index;
				return EINVAL;
			}

			/* Stores the distance in the branch. */
			inst->immediate = (uint32_t)distance & 0xffffU;

			/* The mishandled distance gets an idle instruction after the branch, and everything is laid out again. */
			if (distance == (int32_t)AMDRDNA_GFX10_UNSAFE_BRANCH_DISTANCE) {
				error = insert_idle_after(list, count, index);
				if (error != 0) {
					kern_free(offset);
					kern_free(label_offset);
					return error;
				}

				/* Counts the idle instruction and lays the program out again. */
				moved++;
				again = 1;
				break;
			}
		}

		/* Each move lengthens the code, so the moves end; a runaway is refused. */
		if (moved > *count) {
			kern_free(offset);
			kern_free(label_offset);
			return EINVAL;
		}
	} while (again != 0);

	/* The code, padded for the prefetcher to a whole line of 16 words. */
	padded = (total + AMDRDNA_GFX10_PREFETCH_WORDS + 15U) & ~15U;
	code = kern_calloc(padded, sizeof(*code));
	if (code == NULL) {
		kern_free(offset);
		kern_free(label_offset);
		return ENOMEM;
	}

	/* Writes every instruction. */
	written = 0U;
	for (index = 0U; index < *count; index++) {
		inst = &(*list)[index];
		if (inst->kind == AMDRDNA_GFX10_KIND_LABEL)
			continue;
		size = amdrdna_gfx10_encode(inst, &code[written]);
		written += size;
	}

	/* Fills the rest with the end-of-code instruction. */
	for (index = written; index < padded; index++)
		code[index] = AMDRDNA_GFX10_FORM_CONTROL | ((uint32_t)AMDRDNA_GFX10_S_CODE_END << 16);

	/* Releases the offset tables. */
	kern_free(offset);
	kern_free(label_offset);

	/* Succeeded: the caller owns the code. */
	*words = code;
	*word_count = padded;
	return 0;
}

/*
 * Walks the list once, building the output with its waits and breaks.
 *
 * Each label takes, as its own state joined with the line falling into
 * it, what each branch to it carried; conservative instead settles
 * everything before each label and branch.  Returns nonzero when a
 * label's state learned something new, so another walk is needed.
 */
static int
walk(
	const struct amdrdna_gfx10_inst *list,
	uint32_t count,
	struct wait_output *output,
	struct wait_state *state,
	struct wait_state *labels,
	int conservative)
{
	struct amdrdna_gfx10_access access;
	const struct amdrdna_gfx10_inst *inst;
	uint8_t needed[GFX10_COUNTER_SLOTS];
	uint32_t index;
	int changed;
	int reached;
	int joined;
	int ends;
	int needs;

	/* Starts with nothing outstanding, at a place the program reaches. */
	state_clear(state);
	changed = 0;
	reached = 1;

	/* Walks the list in order. */
	for (index = 0U; index < count; index++) {
		inst = &list[index];

		/* A label settles everything, or joins the paths into it. */
		if (inst->kind == AMDRDNA_GFX10_KIND_LABEL) {
			if (conservative != 0) {
				resolve_all(state, output);
			} else {
				/* The line falls into the label unless it ended just before. */
				if (reached != 0) {
					joined = state_join(&labels[inst->label], state);
					if (joined != 0)
						changed = 1;
				}

				/* The state continues from the label's. */
				*state = labels[inst->label];
			}

			/* The label is reached from here on. */
			reached = 1;
			append(output, inst);
			continue;
		}

		/* A branch settles everything before it in the conservative walk. */
		if (conservative != 0 && inst->kind == AMDRDNA_GFX10_KIND_BRANCH)
			resolve_all(state, output);

		/* Waits for the registers the instruction uses that memory has not filled. */
		amdrdna_gfx10_describe(inst, &access);
		needs = needed_counts(state, &access, needed);
		if (needs != 0)
			wait_for(state, output, needed);

		/* Breaks the hazards the instruction would complete. */
		break_hazards(state, output, inst, &access);

		/* Places the instruction and records what it leaves outstanding. */
		append(output, inst);
		note_wait(state, inst);
		note_issued(state, inst, &access);

		/* A branch carries the state to its label. */
		if (conservative == 0 && inst->kind == AMDRDNA_GFX10_KIND_BRANCH) {
			joined = state_join(&labels[inst->label], state);
			if (joined != 0)
				changed = 1;
		}

		/* After the end or a branch taken always, the next code is reached only through a label. */
		ends = ends_line(inst);
		if (ends != 0) {
			state_clear(state);
			reached = 0;
		}
	}

	/* Reports whether a label learned something. */
	return changed;
}

/* Empties a state: nothing pending, no hazard open. */
static void
state_clear(
	struct wait_state *state)
{
	/* Everything zero, then every register free of every counter. */
	kern_memset(state, 0, sizeof(*state));
	kern_memset(state->pending, GFX10_NOT_PENDING, sizeof(state->pending));
}

/*
 * Joins what one path knows into a label's state: a register is pending
 * for the fewer of the two counts, a hazard is open when it is on either.
 * Returns nonzero when the label's state changed.
 */
static int
state_join(
	struct wait_state *into,
	const struct wait_state *from)
{
	uint32_t code;
	uint32_t counter;
	int changed;
	int joined;

	/* Starts with nothing changed. */
	changed = 0;

	/* Each register's counts: the smaller one waits longer, which serves both paths. */
	for (code = 0U; code < GFX10_CODES; code++) {
		for (counter = 0U; counter < GFX10_COUNTER_SLOTS; counter++) {
			if (from->pending[code][counter] < into->pending[code][counter]) {
				into->pending[code][counter] = from->pending[code][counter];
				changed = 1;
			}
		}

		/* A scalar load on either path keeps the register's wait at zero. */
		if (from->unordered[code] != 0U && into->unordered[code] == 0U) {
			into->unordered[code] = 1U;
			changed = 1;
		}
	}

	/* The hazards open on either path. */
	joined = join_set(into->read_by_load, from->read_by_load);
	joined |= join_set(into->read_by_store, from->read_by_store);
	joined |= join_set(into->read_by_shared, from->read_by_shared);
	joined |= join_set(into->read_by_scalar_load, from->read_by_scalar_load);
	joined |= join_flag(&into->lane_mask_read, from->lane_mask_read);
	joined |= join_flag(&into->vector_memory, from->vector_memory);
	joined |= join_flag(&into->shared_memory, from->shared_memory);
	joined |= join_flag(&into->branch_after_vector, from->branch_after_vector);
	joined |= join_flag(&into->branch_after_shared, from->branch_after_shared);
	joined |= join_flag(&into->m0_written, from->m0_written);
	if (joined != 0)
		changed = 1;

	/* Reports whether the label learned something. */
	return changed;
}

/* Adds a set's registers to another; returns nonzero when that one grew. */
static int
join_set(
	uint32_t *into,
	const uint32_t *from)
{
	uint32_t index;
	int changed;

	/* Each word of the sets. */
	changed = 0;
	for (index = 0U; index < GFX10_SCALAR_SET_WORDS; index++) {
		if ((from[index] & ~into[index]) != 0U) {
			into[index] |= from[index];
			changed = 1;
		}
	}

	/* Reports whether the set grew. */
	return changed;
}

/* Sets a flag that another path has set; returns nonzero when it was clear. */
static int
join_flag(
	int *into,
	int from)
{
	/* Only a flag set on the other path and clear here changes. */
	if (from == 0)
		return 0;
	if (*into != 0)
		return 0;

	/* The flag is now set. */
	*into = 1;
	return 1;
}

/* Reports whether an instruction is never followed by the next one: the end, or a branch taken always. */
static int
ends_line(
	const struct amdrdna_gfx10_inst *inst)
{
	/* The end of the program. */
	if (inst->kind == AMDRDNA_GFX10_KIND_CONTROL && inst->opcode == AMDRDNA_GFX10_S_ENDPGM)
		return 1;

	/* A branch without a condition. */
	if (inst->kind == AMDRDNA_GFX10_KIND_BRANCH && inst->opcode == AMDRDNA_GFX10_S_BRANCH)
		return 1;

	/* Anything else falls through. */
	return 0;
}

/* Places one counter wait for the counts needed, and lets go of the registers it settles. */
static void
wait_for(
	struct wait_state *state,
	struct wait_output *output,
	const uint8_t *needed)
{
	struct amdrdna_gfx10_inst inst;
	uint32_t immediate;
	unsigned vector_count;

	/* Each counter's field gets its count; the others keep the value that waits for nothing. */
	immediate = AMDRDNA_GFX10_WAIT_NONE;
	if (needed[AMDRDNA_GFX10_COUNTER_VECTOR_LOAD] != GFX10_NOT_PENDING) {
		vector_count = needed[AMDRDNA_GFX10_COUNTER_VECTOR_LOAD];
		immediate &= ~AMDRDNA_GFX10_WAIT_VECTOR_MASK;
		immediate |= vector_count & GFX10_WAIT_VECTOR_LOW_MASK;
		immediate |= (vector_count & GFX10_WAIT_VECTOR_HIGH_MASK) << GFX10_WAIT_VECTOR_HIGH_SHIFT;
	}

	/* A wait for the exports. */
	if (needed[AMDRDNA_GFX10_COUNTER_EXPORT] != GFX10_NOT_PENDING) {
		immediate &= ~AMDRDNA_GFX10_WAIT_EXPORT_MASK;
		immediate |= (uint32_t)needed[AMDRDNA_GFX10_COUNTER_EXPORT] << GFX10_WAIT_EXPORT_SHIFT;
	}

	/* A wait for the scalar loads. */
	if (needed[AMDRDNA_GFX10_COUNTER_SCALAR] != GFX10_NOT_PENDING) {
		immediate &= ~AMDRDNA_GFX10_WAIT_SCALAR_MASK;
		immediate |= (uint32_t)needed[AMDRDNA_GFX10_COUNTER_SCALAR] << GFX10_WAIT_SCALAR_SHIFT;
	}

	/* Places the counter wait, when it waits for anything, and notes what it lets go. */
	if (immediate != AMDRDNA_GFX10_WAIT_NONE) {
		append_control(output, AMDRDNA_GFX10_S_WAITCNT, immediate);
		kern_memset(&inst, 0, sizeof(inst));
		inst.kind = AMDRDNA_GFX10_KIND_CONTROL;
		inst.opcode = AMDRDNA_GFX10_S_WAITCNT;
		inst.immediate = immediate;
		note_wait(state, &inst);
	}
}

/* Waits for every counter and breaks every open hazard, so that what follows starts clean. */
static void
resolve_all(
	struct wait_state *state,
	struct wait_output *output)
{
	struct amdrdna_gfx10_inst inst;
	uint8_t needed[GFX10_COUNTER_SLOTS];
	uint32_t code;
	uint32_t counter;
	uint32_t depend;
	int open;
	int waits;

	/* Waits for zero on every counter a register still waits on. */
	waits = 0;
	for (counter = 0U; counter < GFX10_COUNTER_SLOTS; counter++)
		needed[counter] = GFX10_NOT_PENDING;
	for (code = 0U; code < GFX10_CODES; code++) {
		for (counter = 0U; counter < GFX10_COUNTER_SLOTS; counter++) {
			if (state->pending[code][counter] != GFX10_NOT_PENDING) {
				needed[counter] = 0U;
				waits = 1;
			}
		}
	}

	/* Emits the wait the pending registers need. */
	if (waits != 0)
		wait_for(state, output, needed);

	/* The scalar registers vector memory or shared memory read, and the lane mask a scalar read, are let go. */
	depend = 0xffffU;
	open = set_any(state->read_by_load);
	open |= set_any(state->read_by_store);
	open |= set_any(state->read_by_shared);
	if (open != 0)
		depend &= AMDRDNA_GFX10_DEPEND_VECTOR_MEMORY_READS;
	if (state->lane_mask_read != 0)
		depend &= AMDRDNA_GFX10_DEPEND_LANE_MASK_READS;
	if (depend != 0xffffU)
		append_control(output, AMDRDNA_GFX10_S_WAITCNT_DEPCTR, depend);
	set_clear(state->read_by_load);
	set_clear(state->read_by_store);
	set_clear(state->read_by_shared);
	state->lane_mask_read = 0;

	/* The scalar registers a scalar load read are let go by a scalar write. */
	open = set_any(state->read_by_scalar_load);
	if (open != 0) {
		kern_memset(&inst, 0, sizeof(inst));
		inst.kind = AMDRDNA_GFX10_KIND_SCALAR_ONE;
		inst.opcode = AMDRDNA_GFX10_S_MOV_B32;
		inst.dst = AMDRDNA_GFX10_NULL;
		inst.src[0] = AMDRDNA_GFX10_ZERO;
		append(output, &inst);
		set_clear(state->read_by_scalar_load);
	}

	/* A memory access before the branch is separated from one after it. */
	if (output->track_branch_hazard != 0) {
		if (state->vector_memory != 0 || state->shared_memory != 0 ||
		    state->branch_after_vector != 0 || state->branch_after_shared != 0) {
			kern_memset(&inst, 0, sizeof(inst));
			inst.kind = AMDRDNA_GFX10_KIND_SCALAR_CONSTANT;
			inst.opcode = AMDRDNA_GFX10_S_WAITCNT_VSCNT;
			inst.dst = AMDRDNA_GFX10_NULL;
			append(output, &inst);
		}
	}

	/* After the break no memory access or branch is left to separate from the next. */
	state->vector_memory = 0;
	state->shared_memory = 0;
	state->branch_after_vector = 0;
	state->branch_after_shared = 0;
	state->m0_written = 0;
}

/*
 * Places, before an instruction, the break of each GFX10 hazard it would
 * complete.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/compiler/aco_insert_NOPs.cpp
 * (handle_instruction_gfx10: VMEMtoScalarWriteHazard,
 * VcmpxExecWARHazard, SMEMtoVectorWriteHazard, LdsBranchVmemWARHazard)
 * and src/amd/compiler/README-ISA.md ("RDNA / GFX10 hazards").
 */
static void
break_hazards(
	struct wait_state *state,
	struct wait_output *output,
	const struct amdrdna_gfx10_inst *inst,
	const struct amdrdna_gfx10_access *access)
{
	struct amdrdna_gfx10_inst nop;
	int vector_alu;
	int scalar_alu;
	int vector_memory;
	int written;
	int writes_lane_mask;
	int writes_sgpr;

	/* Classifies the instruction. */
	vector_alu = is_vector_alu(inst);
	scalar_alu = is_scalar_alu(inst);
	vector_memory = is_vector_memory(inst);

	/* A scalar write of a register a memory access still reads waits for the reads to finish. */
	if (scalar_alu != 0 || inst->kind == AMDRDNA_GFX10_KIND_SCALAR_LOAD) {
		written = writes_any_in(access, state->read_by_load);
		written |= writes_any_in(access, state->read_by_store);
		written |= writes_any_in(access, state->read_by_shared);
		if (written != 0) {
			append_control(output, AMDRDNA_GFX10_S_WAITCNT_DEPCTR, AMDRDNA_GFX10_DEPEND_VECTOR_MEMORY_READS);
			set_clear(state->read_by_load);
			set_clear(state->read_by_store);
			set_clear(state->read_by_shared);
		}
	}

	/* A vector write of the lane mask a non-vector instruction read waits for the read to finish. */
	if (vector_alu != 0 && state->lane_mask_read != 0) {
		writes_lane_mask = writes_code(access, AMDRDNA_GFX10_EXEC_LO);
		if (writes_lane_mask != 0) {
			append_control(output, AMDRDNA_GFX10_S_WAITCNT_DEPCTR, AMDRDNA_GFX10_DEPEND_LANE_MASK_READS);
			state->lane_mask_read = 0;
		}
	}

	/* A vector write of a scalar register a scalar load read is separated by a scalar write. */
	if (vector_alu != 0) {
		written = writes_any_in(access, state->read_by_scalar_load);
		if (written != 0) {
			kern_memset(&nop, 0, sizeof(nop));
			nop.kind = AMDRDNA_GFX10_KIND_SCALAR_ONE;
			nop.opcode = AMDRDNA_GFX10_S_MOV_B32;
			nop.dst = AMDRDNA_GFX10_NULL;
			nop.src[0] = AMDRDNA_GFX10_ZERO;
			append(output, &nop);
			set_clear(state->read_by_scalar_load);
		}
	}

	/* A memory access of the other kind after a branch waits for the stores. */
	if (output->track_branch_hazard != 0) {
		if ((vector_memory != 0 && state->branch_after_shared != 0) ||
		    (inst->kind == AMDRDNA_GFX10_KIND_SHARED && state->branch_after_vector != 0)) {
			kern_memset(&nop, 0, sizeof(nop));
			nop.kind = AMDRDNA_GFX10_KIND_SCALAR_CONSTANT;
			nop.opcode = AMDRDNA_GFX10_S_WAITCNT_VSCNT;
			nop.dst = AMDRDNA_GFX10_NULL;
			append(output, &nop);
			state->vector_memory = 0;
			state->shared_memory = 0;
			state->branch_after_vector = 0;
			state->branch_after_shared = 0;
		}
	}

	/*
	 * An interpolation right after a scalar write of M0 is given one idle
	 * instruction between them.  This is zedBSD's own caution, not a
	 * documented GFX10 hazard.
	 */
	if (inst->kind == AMDRDNA_GFX10_KIND_INTERPOLATE && state->m0_written != 0)
		append_control(output, AMDRDNA_GFX10_S_NOP, 0U);

	/*
	 * A message right after a scalar write of M0 is given one idle
	 * instruction between them.
	 *
	 * Procedure referenced: Mesa 25.0.7 src/amd/compiler/aco_insert_NOPs.cpp
	 * (NOP_ctx_gfx6: a SALU write of M0 followed by s_sendmsg needs one wait
	 * state, line 95).
	 */
	if (inst->kind == AMDRDNA_GFX10_KIND_CONTROL &&
	    inst->opcode == AMDRDNA_GFX10_S_SENDMSG &&
	    state->m0_written != 0)
		append_control(output, AMDRDNA_GFX10_S_NOP, 0U);

	/* A vector write of a scalar register is noted for the lane mask hazard. */
	writes_sgpr = writes_scalar(access);
	if (vector_alu != 0 && writes_sgpr != 0)
		state->lane_mask_read = 0;
}

/*
 * Records what an instruction leaves outstanding and which hazards it
 * opens.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/compiler/aco_insert_waitcnt.cpp
 * (update_counters and target_info: the scalar loads are the events that
 * finish out of order; the vector memory and the shared memory events
 * finish in the order they were issued).
 */
static void
note_issued(
	struct wait_state *state,
	const struct amdrdna_gfx10_inst *inst,
	const struct amdrdna_gfx10_access *access)
{
	uint32_t index;
	uint32_t *read_set;
	unsigned counter;
	int ordered;
	int vector_alu;
	int scalar_alu;
	int vector_memory;
	int reads_lane_mask;

	/* Classifies the instruction. */
	vector_alu = is_vector_alu(inst);
	scalar_alu = is_scalar_alu(inst);
	vector_memory = is_vector_memory(inst);

	/*
	 * A memory event: the registers held by earlier events of its counter
	 * that finish in order are now one more event behind, and the
	 * registers it fills (an export: reads) wait for it.  The stores fill
	 * nothing, and are waited for only by fences.
	 */
	counter = (unsigned)access->counter;
	if (counter != AMDRDNA_GFX10_COUNTER_NONE && counter != AMDRDNA_GFX10_COUNTER_VECTOR_STORE) {
		ordered = 0;
		if (counter == AMDRDNA_GFX10_COUNTER_VECTOR_LOAD)
			ordered = 1;
		if (counter == AMDRDNA_GFX10_COUNTER_SCALAR && inst->kind == AMDRDNA_GFX10_KIND_SHARED)
			ordered = 1;

		/* An event that finishes in order counts the earlier ones of its kind behind it. */
		if (ordered != 0)
			count_behind(state, counter);

		/*
		 * The writes of a load, and the VGPRs and the lane mask an export
		 * reads: an export reads the lane mask only once it is granted,
		 * well after it is issued, so the lane mask is held like its VGPRs.
		 *
		 * Procedure referenced: Mesa 25.0.7
		 * src/amd/compiler/aco_insert_waitcnt.cpp (gen, Format::EXP: the
		 * exec mask is entered as read by the export).
		 */
		if (counter == AMDRDNA_GFX10_COUNTER_EXPORT) {
			for (index = 0U; index < access->reads_used; index++) {
				if (access->reads[index].first >= GFX10_SCALAR_CODES ||
				    access->reads[index].first == AMDRDNA_GFX10_EXEC_LO)
					hold_range(state, &access->reads[index], counter, ordered);
			}
		} else {
			for (index = 0U; index < access->writes_used; index++)
				hold_range(state, &access->writes[index], counter, ordered);
		}
	}

	/* The scalar registers a vector memory or shared memory access reads stay read for a while. */
	read_set = NULL;
	if (vector_memory != 0) {
		read_set = state->read_by_load;
		if (access->writes_used == 0U)
			read_set = state->read_by_store;
	} else if (inst->kind == AMDRDNA_GFX10_KIND_SHARED) {
		read_set = state->read_by_shared;
	} else if (inst->kind == AMDRDNA_GFX10_KIND_SCALAR_LOAD) {
		read_set = state->read_by_scalar_load;
	}

	/* Records the registers the access reads in the set that guards them. */
	if (read_set != NULL) {
		for (index = 0U; index < access->reads_used; index++)
			add_range(read_set, &access->reads[index]);
	}

	/* A vector ALU instruction ends the vector memory reads' hazard. */
	if (vector_alu != 0) {
		set_clear(state->read_by_load);
		set_clear(state->read_by_store);
		set_clear(state->read_by_shared);
	}

	/* A scalar write (not a control instruction) ends the scalar load reads' hazard. */
	if (scalar_alu != 0 && access->writes_used != 0U &&
	    inst->kind != AMDRDNA_GFX10_KIND_CONTROL && inst->kind != AMDRDNA_GFX10_KIND_BRANCH)
		set_clear(state->read_by_scalar_load);

	/* A non-vector read of the lane mask opens its hazard. */
	reads_lane_mask = reads_code(access, AMDRDNA_GFX10_EXEC_LO);
	if (vector_alu == 0 && reads_lane_mask != 0)
		state->lane_mask_read = 1;

	/* The memory accesses and the branches between them. */
	if (vector_memory != 0) {
		state->vector_memory = 1;
		state->shared_memory = 0;
		state->branch_after_vector = 0;
		state->branch_after_shared = 0;
	} else if (inst->kind == AMDRDNA_GFX10_KIND_SHARED) {
		state->shared_memory = 1;
		state->vector_memory = 0;
		state->branch_after_vector = 0;
		state->branch_after_shared = 0;
	} else if (inst->kind == AMDRDNA_GFX10_KIND_BRANCH) {
		state->branch_after_vector |= state->vector_memory;
		state->branch_after_shared |= state->shared_memory;
		state->vector_memory = 0;
		state->shared_memory = 0;
	}

	/* A scalar write of M0. */
	state->m0_written = 0;
	if (scalar_alu != 0) {
		state->m0_written = writes_code(access, AMDRDNA_GFX10_M0);
	}
}

/*
 * Counts every register an in-order event of a counter holds one event
 * further behind, up to the most a wait field can count.  A register held
 * by a scalar load stays at zero: that event may finish last.
 */
static void
count_behind(
	struct wait_state *state,
	unsigned counter)
{
	uint32_t code;
	unsigned limit;

	/* Looks up how many events the counter can hold. */
	limit = count_limit(counter);

	/* Each register held by the counter. */
	for (code = 0U; code < GFX10_CODES; code++) {
		if (state->pending[code][counter] == GFX10_NOT_PENDING)
			continue;
		if (counter == AMDRDNA_GFX10_COUNTER_SCALAR && state->unordered[code] != 0U)
			continue;
		if (state->pending[code][counter] < limit)
			state->pending[code][counter]++;
	}
}

/* Holds a range of registers on an event that was just issued. */
static void
hold_range(
	struct wait_state *state,
	const struct amdrdna_gfx10_range *range,
	unsigned counter,
	int ordered)
{
	uint32_t code;

	/* Each register of the range: ready once the counter falls to zero, sooner for later in-order events. */
	for (code = range->first; code < (uint32_t)range->first + range->count; code++) {
		if (code >= GFX10_CODES)
			break;
		state->pending[code][counter] = 0U;
		if (counter == AMDRDNA_GFX10_COUNTER_SCALAR) {
			state->unordered[code] = 1U;
			if (ordered != 0)
				state->unordered[code] = 0U;
		}
	}
}

/* Records what a wait the list already has lets go. */
static void
note_wait(
	struct wait_state *state,
	const struct amdrdna_gfx10_inst *inst)
{
	unsigned vector_count;
	unsigned export_count;
	unsigned scalar_count;
	unsigned depend_vector;

	/* A counter wait: each register held no further behind than its counter's field is done. */
	if (inst->kind == AMDRDNA_GFX10_KIND_CONTROL && inst->opcode == AMDRDNA_GFX10_S_WAITCNT) {
		vector_count = (inst->immediate & GFX10_WAIT_VECTOR_LOW_MASK) |
			       ((inst->immediate >> GFX10_WAIT_VECTOR_HIGH_SHIFT) & GFX10_WAIT_VECTOR_HIGH_MASK);
		export_count = (inst->immediate >> GFX10_WAIT_EXPORT_SHIFT) & GFX10_WAIT_EXPORT_FIELD;
		scalar_count = (inst->immediate >> GFX10_WAIT_SCALAR_SHIFT) & GFX10_WAIT_SCALAR_FIELD;
		release(state, AMDRDNA_GFX10_COUNTER_VECTOR_LOAD, vector_count);
		release(state, AMDRDNA_GFX10_COUNTER_EXPORT, export_count);
		release(state, AMDRDNA_GFX10_COUNTER_SCALAR, scalar_count);

		/* The counters waited to zero also end the hazards of the reads their events made. */
		if (vector_count == 0U)
			set_clear(state->read_by_load);
		if (scalar_count == 0U) {
			set_clear(state->read_by_shared);
			set_clear(state->read_by_scalar_load);
		}

		/* A counter wait ends here: nothing else in it ends a hazard. */
		return;
	}

	/* The store counter wait to zero ends the store reads' hazard and the branch hazard. */
	if (inst->kind == AMDRDNA_GFX10_KIND_SCALAR_CONSTANT &&
	    inst->opcode == AMDRDNA_GFX10_S_WAITCNT_VSCNT &&
	    inst->dst == AMDRDNA_GFX10_NULL &&
	    inst->immediate == 0U) {
		set_clear(state->read_by_store);
		state->vector_memory = 0;
		state->shared_memory = 0;
		state->branch_after_vector = 0;
		state->branch_after_shared = 0;
		return;
	}

	/* A dependency wait lets go of the reads it waits for. */
	if (inst->kind == AMDRDNA_GFX10_KIND_CONTROL && inst->opcode == AMDRDNA_GFX10_S_WAITCNT_DEPCTR) {
		depend_vector = (inst->immediate >> GFX10_DEPEND_VM_VSRC_SHIFT) & GFX10_DEPEND_VM_VSRC_MASK;
		if (depend_vector == 0U) {
			set_clear(state->read_by_load);
			set_clear(state->read_by_store);
			set_clear(state->read_by_shared);
		}

		/* A zero scalar-destination field ends the lane mask read. */
		if ((inst->immediate & GFX10_DEPEND_SA_SDST_MASK) == 0U)
			state->lane_mask_read = 0;
	}
}

/*
 * Lets go of the registers a counter wait to count settles: those held at
 * least count events behind (a scalar load's, held at zero, only by a
 * wait to zero).
 */
static void
release(
	struct wait_state *state,
	unsigned counter,
	unsigned count)
{
	uint32_t code;

	/* Each register the counter holds. */
	for (code = 0U; code < GFX10_CODES; code++) {
		if (state->pending[code][counter] == GFX10_NOT_PENDING)
			continue;
		if (count > state->pending[code][counter])
			continue;

		/* The register's event is done. */
		state->pending[code][counter] = GFX10_NOT_PENDING;
		if (counter == AMDRDNA_GFX10_COUNTER_SCALAR)
			state->unordered[code] = 0U;
	}
}

/* Appends an instruction to the list being built; the room was made for it. */
static void
append(
	struct wait_output *output,
	const struct amdrdna_gfx10_inst *inst)
{
	/* The capacity covers every instruction the pass adds, so this never runs out. */
	if (output->count == output->capacity)
		return;

	/* Appends the instruction. */
	output->list[output->count] = *inst;
	output->count++;
}

/* Appends a control instruction with a 16-bit immediate. */
static void
append_control(
	struct wait_output *output,
	unsigned opcode,
	uint32_t immediate)
{
	struct amdrdna_gfx10_inst inst;

	/* Builds the instruction. */
	kern_memset(&inst, 0, sizeof(inst));
	inst.kind = AMDRDNA_GFX10_KIND_CONTROL;
	inst.opcode = (uint16_t)opcode;
	inst.immediate = immediate;

	/* Places it. */
	append(output, &inst);
}

/*
 * Finds the count each counter must fall to before an instruction may
 * read or write its registers.  Returns nonzero when any counter must.
 */
static int
needed_counts(
	const struct wait_state *state,
	const struct amdrdna_gfx10_access *access,
	uint8_t *needed)
{
	uint32_t index;
	uint32_t counter;

	/* Nothing is needed yet. */
	for (counter = 0U; counter < GFX10_COUNTER_SLOTS; counter++)
		needed[counter] = GFX10_NOT_PENDING;

	/* Every register read. */
	for (index = 0U; index < access->reads_used; index++)
		need_range(state, &access->reads[index], 0, needed);

	/* Every register written: a late result must not land after the new value, nor before an export's late read. */
	for (index = 0U; index < access->writes_used; index++)
		need_range(state, &access->writes[index], 1, needed);

	/* Reports whether any counter is waited for. */
	for (counter = 0U; counter < GFX10_COUNTER_SLOTS; counter++) {
		if (needed[counter] != GFX10_NOT_PENDING)
			return 1;
	}

	/* Reports that no counter is waited for. */
	return 0;
}

/*
 * Lowers each counter's needed count to what a range of registers needs.
 * A register an export still reads may be read again at once; only a
 * write waits for it.
 */
static void
need_range(
	const struct wait_state *state,
	const struct amdrdna_gfx10_range *range,
	int written,
	uint8_t *needed)
{
	uint32_t code;
	uint32_t counter;

	/* Each register of the range, on each counter. */
	for (code = range->first; code < (uint32_t)range->first + range->count; code++) {
		if (code >= GFX10_CODES)
			break;
		for (counter = 0U; counter < GFX10_COUNTER_SLOTS; counter++) {
			if (counter == AMDRDNA_GFX10_COUNTER_EXPORT && written == 0)
				continue;
			if (state->pending[code][counter] < needed[counter])
				needed[counter] = state->pending[code][counter];
		}
	}
}

/* Reports the most events a register of a counter is counted behind. */
static unsigned
count_limit(
	unsigned counter)
{
	/* Picks the limit by the counter's field. */
	switch (counter) {
	case AMDRDNA_GFX10_COUNTER_VECTOR_LOAD:
		return GFX10_VECTOR_COUNT_MAX;
	case AMDRDNA_GFX10_COUNTER_EXPORT:
		return GFX10_EXPORT_COUNT_MAX;
	default:
		return GFX10_SCALAR_COUNT_MAX;
	}
}

/* Reports whether an instruction is a vector ALU instruction (interpolation included). */
static int
is_vector_alu(
	const struct amdrdna_gfx10_inst *inst)
{
	/* Picks by the kind. */
	switch (inst->kind) {
	case AMDRDNA_GFX10_KIND_VECTOR_ONE:
	case AMDRDNA_GFX10_KIND_VECTOR_TWO:
	case AMDRDNA_GFX10_KIND_VECTOR_COMPARE:
	case AMDRDNA_GFX10_KIND_VECTOR_WIDE:
	case AMDRDNA_GFX10_KIND_INTERPOLATE:
		return 1;
	default:
		return 0;
	}
}

/* Reports whether an instruction is a scalar ALU, control or branch instruction. */
static int
is_scalar_alu(
	const struct amdrdna_gfx10_inst *inst)
{
	/* Picks by the kind. */
	switch (inst->kind) {
	case AMDRDNA_GFX10_KIND_SCALAR_TWO:
	case AMDRDNA_GFX10_KIND_SCALAR_CONSTANT:
	case AMDRDNA_GFX10_KIND_SCALAR_ONE:
	case AMDRDNA_GFX10_KIND_SCALAR_COMPARE:
	case AMDRDNA_GFX10_KIND_CONTROL:
	case AMDRDNA_GFX10_KIND_BRANCH:
		return 1;
	default:
		return 0;
	}
}

/* Reports whether an instruction is a vector memory (buffer or image) access. */
static int
is_vector_memory(
	const struct amdrdna_gfx10_inst *inst)
{
	/* Buffers and images. */
	if (inst->kind == AMDRDNA_GFX10_KIND_BUFFER)
		return 1;
	if (inst->kind == AMDRDNA_GFX10_KIND_IMAGE)
		return 1;

	/* Anything else. */
	return 0;
}

/* Reports whether a set holds any scalar register of a range. */
static int
reads_range_in(
	const uint32_t *set,
	const struct amdrdna_gfx10_range *range)
{
	uint32_t code;

	/* Each scalar register of the range. */
	for (code = range->first; code < (uint32_t)range->first + range->count; code++) {
		if (code >= GFX10_SCALAR_CODES)
			break;
		if ((set[code / 32U] & (1U << (code % 32U))) != 0U)
			return 1;
	}

	/* None of them. */
	return 0;
}

/* Adds the scalar registers of a range to a set. */
static void
add_range(
	uint32_t *set,
	const struct amdrdna_gfx10_range *range)
{
	uint32_t code;

	/* Each scalar register of the range; VGPRs are not followed. */
	for (code = range->first; code < (uint32_t)range->first + range->count; code++) {
		if (code >= GFX10_SCALAR_CODES)
			break;
		set[code / 32U] |= 1U << (code % 32U);
	}
}

/* Reports whether a set holds any register. */
static int
set_any(
	const uint32_t *set)
{
	uint32_t index;

	/* Each word of the set. */
	for (index = 0U; index < GFX10_SCALAR_SET_WORDS; index++) {
		if (set[index] != 0U)
			return 1;
	}

	/* An empty set. */
	return 0;
}

/* Empties a set. */
static void
set_clear(
	uint32_t *set)
{
	uint32_t index;

	/* Each word of the set. */
	for (index = 0U; index < GFX10_SCALAR_SET_WORDS; index++)
		set[index] = 0U;
}

/* Reports whether an instruction writes a register. */
static int
writes_code(
	const struct amdrdna_gfx10_access *access,
	unsigned code)
{
	uint32_t index;

	/* Each range written. */
	for (index = 0U; index < access->writes_used; index++) {
		if (code >= access->writes[index].first && code < (unsigned)access->writes[index].first + access->writes[index].count)
			return 1;
	}

	/* Not written. */
	return 0;
}

/* Reports whether an instruction writes a scalar register. */
static int
writes_scalar(
	const struct amdrdna_gfx10_access *access)
{
	uint32_t index;

	/* Each range written. */
	for (index = 0U; index < access->writes_used; index++) {
		if (access->writes[index].first < GFX10_SCALAR_CODES)
			return 1;
	}

	/* Only VGPRs, or nothing. */
	return 0;
}

/* Reports whether an instruction writes a scalar register a set holds. */
static int
writes_any_in(
	const struct amdrdna_gfx10_access *access,
	const uint32_t *set)
{
	uint32_t index;
	int found;

	/* Each range written. */
	for (index = 0U; index < access->writes_used; index++) {
		found = reads_range_in(set, &access->writes[index]);
		if (found != 0)
			return 1;
	}

	/* None of them. */
	return 0;
}

/* Reports whether an instruction reads a register. */
static int
reads_code(
	const struct amdrdna_gfx10_access *access,
	unsigned code)
{
	uint32_t index;

	/* Each range read. */
	for (index = 0U; index < access->reads_used; index++) {
		if (code >= access->reads[index].first && code < (unsigned)access->reads[index].first + access->reads[index].count)
			return 1;
	}

	/* Not read. */
	return 0;
}

/* Reports the words an instruction is written as (0 for a label); *refused is 1 when the encoder refuses it. */
static unsigned
instruction_words(
	const struct amdrdna_gfx10_inst *inst,
	int *refused)
{
	uint32_t scratch[AMDRDNA_GFX10_MAX_WORDS];
	unsigned size;

	/* Starts with nothing refused. */
	*refused = 0;

	/* A label is no word. */
	if (inst->kind == AMDRDNA_GFX10_KIND_LABEL)
		return 0U;

	/* Writes the instruction aside to count its words. */
	size = amdrdna_gfx10_encode(inst, scratch);
	if (size == 0U)
		*refused = 1;

	/* Reports the count. */
	return size;
}

/* Inserts an idle instruction after the instruction at index, growing the list. */
static int
insert_idle_after(
	struct amdrdna_gfx10_inst **list,
	uint32_t *count,
	uint32_t index)
{
	struct amdrdna_gfx10_inst *grown;

	/* A list one longer. */
	grown = kern_calloc(*count + 1U, sizeof(*grown));
	if (grown == NULL)
		return ENOMEM;

	/* Copies the instructions up to the branch, the idle one, and the rest. */
	kern_memcpy(grown, *list, (index + 1U) * sizeof(*grown));
	grown[index + 1U].kind = AMDRDNA_GFX10_KIND_CONTROL;
	grown[index + 1U].opcode = AMDRDNA_GFX10_S_NOP;
	kern_memcpy(&grown[index + 2U], &(*list)[index + 1U], (*count - index - 1U) * sizeof(*grown));

	/* The new list replaces the old. */
	kern_free(*list);
	*list = grown;
	*count = *count + 1U;

	/* Succeeded: the branch is followed by the idle instruction. */
	return 0;
}
