/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Mali compiler's last pass: dependency slots, waits, branch offsets
 * and the program's end, then the instruction words.
 *
 * Valhall has no clauses.  A message (a memory access, a varying or
 * attribute load, a texture, ATEST, ZS_EMIT, BLEND) is issued with one of
 * the dependency slots 0 to 2 and its results arrive later; an
 * instruction that reads or writes a register a pending message writes
 * must first wait on that message's slot.  The wait is the flow control
 * of the instruction before it (a NOP when that instruction's flow
 * control is taken), and covers every message issued on the slot so far.
 * The pass also serializes memory accesses and varying loads, waits on
 * every slot before ATEST, ZS_EMIT and BLEND as the hardware requires,
 * flushes every slot at branches and at their targets, gives the
 * instruction before a branch target its reconvergence, and ends the
 * program.
 *
 * Procedure referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/va_insert_flow.c (the rules of
 * the scoreboard: waits before reading or writing a slot's results,
 * serialized memory, the varying hidden register, wait0126 before ATEST
 * and ZS_EMIT, wait before BLEND and after BARRIER, and va_assign_slots:
 * slot 0 for ATEST and ZS_EMIT) and va_merge_flow.c (a wait moved
 * onto the instruction before, reconvergence and end on the last
 * instruction of a block).
 */

#include "drivers/gpu/mali/compiler/va-private.h"

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include <stddef.h>
#include <stdint.h>

/* The most instructions the pass adds before one instruction, and at the end. */
#define VA_SCHEDULE_ADDED	3U
#define VA_SCHEDULE_TRAILER	1U

/* The padding after a program, before rounding to the code alignment. */
#define VA_SCHEDULE_PADDING	16U

/* The bits of a branch offset (signed, in instructions after the next). */
#define VA_BRANCH_OFFSET_MASK	0x7FFFFFFULL

/*
 * One instruction as the pass writes it: its word without the flow, its
 * flow control, and for a branch the list index of its target.
 */
struct va_out {
	uint64_t bits;
	int32_t target;
	uint8_t flow;
};

/*
 * What the pass knows while it walks the list: the registers the pending
 * messages of each general slot write, the slots with a pending memory
 * access or varying load, the slot the next message takes, and the
 * instructions written so far.
 */
struct va_schedule {
	uint64_t pending[VA_GENERAL_SLOTS];
	uint32_t memory;
	uint32_t varying;
	uint32_t next_slot;
	struct va_out *out;
	uint32_t count;
};

static uint32_t schedule_pending_slots(const struct va_schedule *state);
static void schedule_clear(struct va_schedule *state, uint32_t slots);
static void schedule_append(struct va_schedule *state, uint64_t bits, uint8_t flow, int32_t target);
static void schedule_wait(struct va_schedule *state, uint32_t slots);
static void schedule_special_wait(struct va_schedule *state, uint8_t flow);
static void schedule_reconverge(struct va_schedule *state);
static uint32_t schedule_needed_slots(const struct va_schedule *state, const struct va_inst *inst);
static uint32_t schedule_slot(struct va_schedule *state, const struct va_inst *inst);
static void schedule_record(struct va_schedule *state, const struct va_inst *inst, uint32_t slot);

/*
 * Reports the bytes the pass may write for a list, at most.
 */
size_t
va_schedule_bound(
	const struct va_code *code)
{
	size_t instructions;

	/* Every instruction may gain its NOPs, and the program its end. */
	instructions = (size_t)code->count * (1U + VA_SCHEDULE_ADDED) + VA_SCHEDULE_TRAILER;

	/* Reports the words and the padding. */
	return instructions * 8U + VA_SCHEDULE_PADDING + MALI_SHADER_CODE_ALIGNMENT;
}

/*
 * Gives the list its slots, waits, branch offsets and end, and writes it
 * as little-endian words followed by the padding.
 */
size_t
va_schedule_write(
	const struct va_code *code,
	uint8_t *out,
	size_t room)
{
	struct va_schedule state;
	const struct va_inst *inst;
	uint32_t *where;
	uint32_t needed;
	uint32_t slot;
	uint32_t index;
	uint32_t byte;
	int64_t offset;
	uint64_t word;
	uint8_t flow;
	size_t length;
	size_t padded;

	/* Allocates the written instructions and the place of each list instruction. */
	kern_memset(&state, 0, sizeof(state));
	state.out = kern_malloc(((size_t)code->count * (1U + VA_SCHEDULE_ADDED) + VA_SCHEDULE_TRAILER) * sizeof(*state.out));
	if (state.out == NULL)
		return 0U;
	where = kern_malloc(((size_t)code->count + 1U) * sizeof(*where));
	if (where == NULL) {
		kern_free(state.out);
		return 0U;
	}

	/* Walks the list, giving each instruction its waits and its slot. */
	for (index = 0U; index < code->count; index++) {
		inst = &code->list[index];

		/*
		 * A branch target starts with nothing pending: every slot is
		 * waited on, and the block before it reconverges.
		 */
		if ((inst->flags & VA_INST_TARGET) != 0U) {
			schedule_wait(&state, schedule_pending_slots(&state));
			schedule_clear(&state, schedule_pending_slots(&state));
			schedule_reconverge(&state);
		}

		/* Waits before ATEST (and ZS_EMIT) and BLEND cover every general slot. */
		if ((inst->flags & VA_INST_ATEST) != 0U) {
			schedule_special_wait(&state, VA_FLOW_WAIT0126);
			schedule_clear(&state, schedule_pending_slots(&state));
		} else if ((inst->flags & VA_INST_BLEND) != 0U) {
			schedule_special_wait(&state, VA_FLOW_WAIT_ALL);
			schedule_clear(&state, schedule_pending_slots(&state));
		}

		/* Waits on the slots whose results the instruction depends on. */
		needed = schedule_needed_slots(&state, inst);
		schedule_wait(&state, needed);
		schedule_clear(&state, needed);

		/* Writes the instruction with its slot; a barrier waits for itself. */
		slot = schedule_slot(&state, inst);
		flow = inst->flow;
		if ((inst->flags & VA_INST_BARRIER) != 0U)
			flow = VA_FLOW_WAIT_ALL;
		where[index] = state.count;
		schedule_append(&state, inst->bits | va_field(slot, VA_SHIFT_SLOT), flow, inst->target);

		/* Remembers what the message leaves pending. */
		schedule_record(&state, inst, slot);
	}
	where[code->count] = state.count;

	/* Ends the program on its last instruction, or on a NOP when that instruction's flow is taken. */
	if (state.count != 0U && state.out[state.count - 1U].flow == VA_FLOW_NONE) {
		state.out[state.count - 1U].flow = VA_FLOW_END;
	} else {
		schedule_append(&state, VA_OP_NOP | va_dest(0U), VA_FLOW_END, VA_NO_TARGET);
	}

	/* Measures the words and the padding. */
	length = (size_t)state.count * 8U;
	padded = length + VA_SCHEDULE_PADDING;
	padded = (padded + MALI_SHADER_CODE_ALIGNMENT - 1U) & ~((size_t)MALI_SHADER_CODE_ALIGNMENT - 1U);
	if (padded > room) {
		kern_free(where);
		kern_free(state.out);
		return 0U;
	}

	/* Writes each instruction, with a branch's offset to its target's place. */
	for (index = 0U; index < state.count; index++) {
		word = state.out[index].bits;
		if (state.out[index].target != VA_NO_TARGET) {
			offset = (int64_t)where[state.out[index].target] - (int64_t)(index + 1U);
			word |= ((uint64_t)offset & VA_BRANCH_OFFSET_MASK) << VA_SHIFT_SOURCE1;
		}
		word |= va_field(state.out[index].flow, VA_SHIFT_FLOW);

		/* Stores the word little-endian. */
		for (byte = 0U; byte < 8U; byte++)
			out[(size_t)index * 8U + byte] = (uint8_t)(word >> (8U * byte));
	}

	/* Clears the padding the prefetch may read. */
	kern_memset(out + length, 0, padded - length);

	/* Succeeded: reports the bytes written. */
	kern_free(where);
	kern_free(state.out);
	return padded;
}

/* Reports the general slots with anything pending. */
static uint32_t
schedule_pending_slots(
	const struct va_schedule *state)
{
	uint32_t slots;
	uint32_t slot;

	/* A slot is pending while it writes registers, accesses memory or loads a varying. */
	slots = state->memory | state->varying;
	for (slot = 0U; slot < VA_GENERAL_SLOTS; slot++) {
		if (state->pending[slot] != 0U)
			slots |= 1U << slot;
	}

	/* Reports the slots. */
	return slots;
}

/* Forgets what the waited slots had pending. */
static void
schedule_clear(
	struct va_schedule *state,
	uint32_t slots)
{
	uint32_t slot;

	/* Clears each waited slot's registers and its memory and varying marks. */
	for (slot = 0U; slot < VA_GENERAL_SLOTS; slot++) {
		if ((slots & (1U << slot)) != 0U)
			state->pending[slot] = 0U;
	}
	state->memory &= ~slots;
	state->varying &= ~slots;
}

/* Appends one written instruction. */
static void
schedule_append(
	struct va_schedule *state,
	uint64_t bits,
	uint8_t flow,
	int32_t target)
{
	/* Stores the instruction after the others. */
	state->out[state->count].bits = bits;
	state->out[state->count].flow = flow;
	state->out[state->count].target = target;
	state->count++;
}

/*
 * Waits on general slots: on the instruction before when its flow
 * control is free or already a wait on general slots, else on a NOP.
 */
static void
schedule_wait(
	struct va_schedule *state,
	uint32_t slots)
{
	struct va_out *previous;

	/* Nothing to wait for. */
	if (slots == 0U)
		return;

	/* Joins the wait to the instruction before when its flow allows it. */
	if (state->count != 0U) {
		previous = &state->out[state->count - 1U];
		if (previous->flow <= VA_FLOW_WAIT_SLOTS) {
			previous->flow = (uint8_t)(previous->flow | slots);
			return;
		}
		if (previous->flow == VA_FLOW_WAIT0126 || previous->flow == VA_FLOW_WAIT_ALL)
			return;
	}

	/* Waits on a NOP of its own. */
	schedule_append(state, VA_OP_NOP | va_dest(0U), (uint8_t)slots, VA_NO_TARGET);
}

/*
 * Waits with one of the flows that cover every general slot (before
 * ATEST or BLEND), on the instruction before when it allows it.
 */
static void
schedule_special_wait(
	struct va_schedule *state,
	uint8_t flow)
{
	struct va_out *previous;

	/* Replaces a free flow, or a wait on general slots, of the instruction before. */
	if (state->count != 0U) {
		previous = &state->out[state->count - 1U];
		if (previous->flow <= VA_FLOW_WAIT_SLOTS) {
			previous->flow = flow;
			return;
		}

		/* A wait on every slot also covers the wait before ATEST. */
		if (previous->flow == flow || previous->flow == VA_FLOW_WAIT_ALL)
			return;
	}

	/* Waits on a NOP of its own. */
	schedule_append(state, VA_OP_NOP | va_dest(0U), flow, VA_NO_TARGET);
}

/* Gives the instruction before a branch target its reconvergence. */
static void
schedule_reconverge(
	struct va_schedule *state)
{
	struct va_out *previous;

	/* A target at the start of the program follows no block. */
	if (state->count == 0U)
		return;

	/* Reconverges on the instruction before when its flow is free, else on a NOP. */
	previous = &state->out[state->count - 1U];
	if (previous->flow == VA_FLOW_RECONVERGE)
		return;
	if (previous->flow == VA_FLOW_NONE) {
		previous->flow = VA_FLOW_RECONVERGE;
		return;
	}
	schedule_append(state, VA_OP_NOP | va_dest(0U), VA_FLOW_RECONVERGE, VA_NO_TARGET);
}

/* Reports the general slots an instruction must wait on before it runs. */
static uint32_t
schedule_needed_slots(
	const struct va_schedule *state,
	const struct va_inst *inst)
{
	uint64_t touched;
	uint32_t slots;
	uint32_t slot;

	/* Waits on the slots writing a register the instruction reads or writes. */
	touched = inst->reads | inst->writes;
	slots = 0U;
	for (slot = 0U; slot < VA_GENERAL_SLOTS; slot++) {
		if ((state->pending[slot] & touched) != 0U)
			slots |= 1U << slot;
	}

	/* A memory access waits for the accesses before it. */
	if ((inst->flags & VA_INST_MEMORY) != 0U)
		slots |= state->memory;

	/* A varying load waits for the varying loads before it (their hidden register). */
	if ((inst->flags & VA_INST_VARYING) != 0U)
		slots |= state->varying;

	/* A branch and a barrier leave nothing pending behind them. */
	if ((inst->flags & (VA_INST_BRANCH | VA_INST_BARRIER)) != 0U)
		slots |= schedule_pending_slots(state);

	/* Reports the slots. */
	return slots;
}

/* Chooses a message's dependency slot. */
static uint32_t
schedule_slot(
	struct va_schedule *state,
	const struct va_inst *inst)
{
	uint32_t slot;

	/* An arithmetic instruction takes no slot. */
	if ((inst->flags & VA_INST_MESSAGE) == 0U)
		return 0U;

	/* ATEST and ZS_EMIT take slot 0, a barrier slot 7. */
	if ((inst->flags & VA_INST_ATEST) != 0U)
		return 0U;
	if ((inst->flags & VA_INST_BARRIER) != 0U)
		return VA_SLOT_BARRIER;

	/* The other messages take the general slots in turn. */
	slot = state->next_slot;
	state->next_slot = (state->next_slot + 1U) % VA_GENERAL_SLOTS;

	/* Reports the slot. */
	return slot;
}

/* Remembers what a message leaves pending on its slot. */
static void
schedule_record(
	struct va_schedule *state,
	const struct va_inst *inst,
	uint32_t slot)
{
	/* Only messages on general slots leave anything pending. */
	if ((inst->flags & VA_INST_MESSAGE) == 0U)
		return;
	if (slot >= VA_GENERAL_SLOTS)
		return;

	/* Records the registers it writes, its memory access and its varying load. */
	state->pending[slot] |= inst->writes;
	if ((inst->flags & VA_INST_MEMORY) != 0U)
		state->memory |= 1U << slot;
	if ((inst->flags & VA_INST_VARYING) != 0U)
		state->varying |= 1U << slot;
}
