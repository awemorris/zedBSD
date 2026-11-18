/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The dependence marking of the nvrtx shader compiler (see schedule.h).
 *
 * The reordering (nvrtx_sm75_reorder()) works inside each block of
 * instructions between the fixed ones (branches, convergence, barriers,
 * discards, EXIT, NOPs) and branch targets: an instruction may move past
 * another only when neither reads or writes what the other writes, and
 * memory accesses keep their order when either one stores.  Among the
 * instructions whose predecessors have issued, the one with the longest
 * latency-weighted path to the block's end goes first (a list scheduler),
 * the latencies of variable-latency instructions estimated.
 *
 * The marking walks the program once in order.  For every register and
 * predicate it keeps who wrote it last and when (a cycle count from the
 * start of the block), or which scoreboard will say the write is done, and
 * which scoreboards will say the reads of it are done.  An instruction
 * issues at the first cycle every one of its reads and writes allows, and
 * the instruction before it gets that many cycles as its delay.  The
 * tables' few values that depend on predication or on finer classes are
 * taken at their largest, so the delays are never short.
 *
 * Ampere and Ada (SM86, SM89) differ from Turing only in the read after
 * write of a fixed-latency result: a variable-latency reader waits 4
 * cycles (5 for the address unit's) instead of 6, an IMAD.WIDE's addend
 * waits 4 for another IMAD.WIDE instead of 2, and a guard, a branch or a
 * variable-latency reader of a comparison waits 13 instead of 12.  Their
 * write after write, write after read and worst latencies are within the
 * values below, which both use; BAR and MEMBAR execute in 6 cycles, which
 * NVRTX_SM75_DELAY_SYNC_MIN already waits.
 */

#include "drivers/gpu/nvrtx/compiler/sm75/encode.h"
#include "drivers/gpu/nvrtx/compiler/sm75/schedule.h"

#include <kern/kcrt.h>
#include <kern/kmem.h>

#include <stddef.h>

/*
 * The scoreboards an instruction may set, and the mask of all of them.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/calc_instr_deps.rs,
 * BarAlloc (lines 186-196: six scoreboards).
 */
#define NVRTX_SM75_SCOREBOARDS		6U
#define NVRTX_SM75_SCOREBOARD_ALL	0x3fU

/*
 * The general registers and predicates the pass tracks.
 *
 * zedBSD's own.
 */
#define NVRTX_SM75_TRACKED_GPRS		256U
#define NVRTX_SM75_TRACKED_PREDICATES	8U

/*
 * The cycles after a scoreboard is set before an instruction may wait for it.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/calc_instr_deps.rs,
 * calc_delays (lines 692-698: a scoreboard is waited for 2 cycles after it is
 * set at the earliest).
 */
#define NVRTX_SM75_SCOREBOARD_SETTLE	2U

/*
 * The worst latency of a fixed-latency general register and predicate result,
 * which a block's end waits out.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/compiler/nak/sm75_instr_latencies.rs, read_after_write and
 * pred_read_after_write with no reader (lines 205-412, 695-776),
 * src/nouveau/compiler/nak/sm80_instr_latencies.rs, the same (lines 259-576,
 * 889-958), and src/nouveau/compiler/nak/sm70.rs, worst_latency and
 * instr_latency (lines 24-67, 263-273).
 */
#define NVRTX_SM75_WORST_GPR		6U
#define NVRTX_SM75_WORST_PREDICATE	13U

/*
 * A write after a write, and a variable-latency write after a fixed-latency
 * write or read.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/compiler/nak/sm75_instr_latencies.rs, write_after_write and
 * write_after_read (lines 414-693), and
 * src/nouveau/compiler/nak/sm80_instr_latencies.rs, the same (lines 578-842),
 * at their largest.
 */
#define NVRTX_SM75_WAW_FIXED		2U
#define NVRTX_SM75_WAW_DECOUPLED	6U
#define NVRTX_SM75_WAR_DECOUPLED	2U

/*
 * The latencies the reordering assumes for the variable-latency instructions
 * (only priorities, never correctness).
 *
 * zedBSD's own estimates.
 */
#define NVRTX_SM75_GUESS_TEXTURE	100U
#define NVRTX_SM75_GUESS_GLOBAL		300U
#define NVRTX_SM75_GUESS_SHARED		30U
#define NVRTX_SM75_GUESS_CONSTANT	25U
#define NVRTX_SM75_GUESS_UNIT		15U
#define NVRTX_SM75_GUESS_SPECIAL	20U

/*
 * A pair the reordering must keep in order, and a block not yet ended.
 *
 * zedBSD's own.
 */
#define NVRTX_SM75_NO_DEPENDENCE	0xffffffffU

/*
 * What the pass knows of one general register or predicate: whether a
 * fixed-latency write is in flight, its class and its cycle; the
 * scoreboard of a variable-latency write in flight; the scoreboards of the
 * variable-latency reads in flight; and the last fixed-latency read.
 *
 * zedBSD's own.
 */
struct nvrtx_sm75_track {
	int written;
	uint32_t write_class;
	uint32_t write_cycle;
	uint32_t write_bar;
	uint32_t read_bars;
	uint32_t read_cycle;
};

/*
 * The state of one pass: the shader model, the registers and predicates,
 * each scoreboard's use (busy) and the cycle it was set, and the cycle of
 * the instruction before.  Allocated by nvrtx_sm75_schedule() for one
 * call.
 */
struct nvrtx_sm75_scheduler {
	uint32_t sm;
	struct nvrtx_sm75_track gpr[NVRTX_SM75_TRACKED_GPRS];
	struct nvrtx_sm75_track predicate[NVRTX_SM75_TRACKED_PREDICATES];
	uint32_t bar_busy;
	uint32_t bar_set_cycle[NVRTX_SM75_SCOREBOARDS];
	uint32_t cycle;
};

/*
 * One instruction of a block being reordered: its operands, its opcode, the
 * latency-weighted path to the block's end, the predecessors not yet
 * issued, the earliest cycle they allow, and whether it has issued.  An
 * array of them lives for one call of nvrtx_sm75_reorder().
 */
struct nvrtx_sm75_node {
	struct nvrtx_sm75_operands operands;
	uint32_t opcode;
	uint32_t words[NVRTX_SM75_INSTRUCTION_WORDS];
	uint32_t priority;
	uint32_t waiting;
	uint32_t earliest;
	int issued;
};

static int nvrtx_sm75_reorder_block(struct nvrtx_sm75_node *nodes, uint32_t count, uint32_t sm, uint32_t *code);
static uint32_t nvrtx_sm75_dependence(const struct nvrtx_sm75_node *before, const struct nvrtx_sm75_node *after, uint32_t sm);
static int nvrtx_sm75_memory_kind(uint32_t opcode);
static uint32_t nvrtx_sm75_guess(const struct nvrtx_sm75_node *node);
static int nvrtx_sm75_fixed(const struct nvrtx_sm75_operands *operands);
static void nvrtx_sm75_mark_targets(const uint32_t *code, uint32_t count, uint8_t *target);
static void nvrtx_sm75_flush(struct nvrtx_sm75_scheduler *scheduler, uint32_t *earliest, uint32_t *wait);
static void nvrtx_sm75_clear(struct nvrtx_sm75_scheduler *scheduler);
static void nvrtx_sm75_constrain(struct nvrtx_sm75_scheduler *scheduler, const struct nvrtx_sm75_operands *operands, uint32_t *earliest, uint32_t *wait);
static void nvrtx_sm75_take_bars(struct nvrtx_sm75_scheduler *scheduler, const struct nvrtx_sm75_operands *operands, uint32_t *wait, uint32_t *write_bar, uint32_t *read_bar);
static void nvrtx_sm75_record(struct nvrtx_sm75_scheduler *scheduler, const struct nvrtx_sm75_operands *operands, uint32_t issue, uint32_t write_bar, uint32_t read_bar);
static void nvrtx_sm75_release(struct nvrtx_sm75_scheduler *scheduler, uint32_t wait);
static uint32_t nvrtx_sm75_raw(uint32_t sm, uint32_t writer, uint32_t reader);
static uint32_t nvrtx_sm75_raw_turing(uint32_t writer, uint32_t reader);
static uint32_t nvrtx_sm75_raw_ampere(uint32_t writer, uint32_t reader);
static uint32_t nvrtx_sm75_predicate_raw(uint32_t sm, uint32_t reader);
static uint32_t nvrtx_sm75_later(uint32_t a, uint32_t b);

/*
 * Reorders the instructions inside each block of a program.
 *
 * A block is a run of instructions that are not fixed (see
 * nvrtx_sm75_fixed()), cut at every branch target.  Returns 0, or 1 with
 * the instruction and the reason.
 */
int
nvrtx_sm75_reorder(
	uint32_t *code,
	uint32_t count,
	uint32_t sm,
	uint32_t *instruction,
	const char **reason)
{
	struct nvrtx_sm75_node *nodes;
	uint8_t *cut;
	uint32_t index;
	uint32_t start;
	uint32_t end;
	int unknown;
	int fixed;
	int error;

	/* The nodes, one per instruction. */
	nodes = kern_calloc(count + 1U, sizeof(*nodes));
	if (nodes == NULL) {
		*instruction = 0U;
		*reason = "no memory for the reordering";
		return 1;
	}

	/* Where blocks are cut: before a branch target; a fixed instruction is a cut of its own. */
	cut = kern_calloc(count + 1U, sizeof(*cut));
	if (cut == NULL) {
		kern_free(nodes);
		*instruction = 0U;
		*reason = "no memory for the branch targets";
		return 1;
	}

	/* Each branch target is a cut. */
	nvrtx_sm75_mark_targets(code, count, cut);

	/* Every instruction's operands and words. */
	for (index = 0U; index < count; index++) {
		unknown = nvrtx_sm75_operands(code + (size_t)index * NVRTX_SM75_INSTRUCTION_WORDS, &nodes[index].operands);
		if (unknown != 0) {
			kern_free(cut);
			kern_free(nodes);
			*instruction = index;
			*reason = "the reordering does not know an instruction";
			return 1;
		}

		/* Its words, and the opcode of the encodings of their own. */
		kern_memcpy(nodes[index].words, code + (size_t)index * NVRTX_SM75_INSTRUCTION_WORDS, sizeof(nodes[index].words));
		nodes[index].opcode = nodes[index].words[0] & 0xfffU;
	}

	/* Each block: from a cut or after a fixed instruction, up to the next of either. */
	error = 0;
	start = 0U;
	while (start < count && error == 0) {
		/* A fixed instruction stays. */
		fixed = nvrtx_sm75_fixed(&nodes[start].operands);
		if (fixed != 0) {
			start++;
			continue;
		}

		/* The block's end: the end of the code, a branch target or a fixed instruction. */
		end = start + 1U;
		while (end < count) {
			if (cut[end] != 0U)
				break;
			fixed = nvrtx_sm75_fixed(&nodes[end].operands);
			if (fixed != 0)
				break;
			end++;
		}

		/* The block's instructions in their new order. */
		if (end - start > 1U) {
			error = nvrtx_sm75_reorder_block(nodes + start, end - start, sm, code + (size_t)start * NVRTX_SM75_INSTRUCTION_WORDS);
			if (error != 0) {
				*instruction = start;
				*reason = "no memory for the reordering";
			}
		}

		/* The next block starts at this one's end. */
		start = end;
	}

	/* The cuts and the nodes are only this call's. */
	kern_free(cut);
	kern_free(nodes);

	/* Reports a failure. */
	if (error != 0)
		return 1;

	/* Succeeded: every block is reordered. */
	return 0;
}

/*
 * Sets the scheduling control of every instruction of a program.
 *
 * Returns 0, or 1 with the instruction and the reason when an instruction
 * is not known or the wait before one would not fit the delay field.
 *
 * Procedure referenced: Mesa 25.3.6
 * src/nouveau/compiler/nak/calc_instr_deps.rs (assign_barriers and
 * calc_delays).
 */
int
nvrtx_sm75_schedule(
	uint32_t *code,
	uint32_t count,
	uint32_t sm,
	uint32_t *instruction,
	const char **reason)
{
	struct nvrtx_sm75_scheduler *scheduler;
	struct nvrtx_sm75_operands operands;
	struct nvrtx_sm75_word word;
	uint8_t *target;
	uint32_t *slot;
	uint32_t *previous;
	uint32_t index;
	uint32_t earliest;
	uint32_t wait;
	uint32_t write_bar;
	uint32_t read_bar;
	uint32_t bar;
	uint32_t previous_min;
	uint32_t delay;
	int previous_sync;
	int unknown;

	/* The pass's state, which is too large for a kernel stack. */
	scheduler = kern_calloc(1U, sizeof(*scheduler));
	if (scheduler == NULL) {
		*instruction = 0U;
		*reason = "no memory for the dependence marking";
		return 1;
	}

	/* The branch targets, which start blocks. */
	target = kern_calloc(count + 1U, sizeof(*target));
	if (target == NULL) {
		kern_free(scheduler);
		*instruction = 0U;
		*reason = "no memory for the branch targets";
		return 1;
	}

	/* Each branch target is marked. */
	nvrtx_sm75_mark_targets(code, count, target);

	/* The shader model's latencies, and nothing in flight at the program's start. */
	scheduler->sm = sm;
	nvrtx_sm75_clear(scheduler);
	scheduler->cycle = 0U;
	previous = NULL;
	previous_min = 0U;
	previous_sync = 0;

	/* Each instruction in order. */
	for (index = 0U; index < count; index++) {
		slot = code + (size_t)index * NVRTX_SM75_INSTRUCTION_WORDS;

		/* What it reads and writes. */
		unknown = nvrtx_sm75_operands(slot, &operands);
		if (unknown != 0) {
			kern_free(target);
			kern_free(scheduler);
			*instruction = index;
			*reason = "the dependence marking does not know an instruction";
			return 1;
		}

		/* The NOP after a barrier waits 2 cycles. */
		if (previous_sync != 0 && operands.class_of == NVRTX_SM75_CLASS_DISPATCH)
			operands.min_delay = NVRTX_SM75_DELAY_AFTER_SYNC;

		/* The earliest it may issue after the instruction before. */
		earliest = scheduler->cycle + previous_min;
		wait = 0U;

		/* A branch target or a block's last instruction waits for everything. */
		if (target[index] != 0U || operands.boundary != 0) {
			nvrtx_sm75_flush(scheduler, &earliest, &wait);
			nvrtx_sm75_clear(scheduler);
		}

		/* Its own reads and writes, then the scoreboards it sets. */
		nvrtx_sm75_constrain(scheduler, &operands, &earliest, &wait);
		nvrtx_sm75_take_bars(scheduler, &operands, &wait, &write_bar, &read_bar);

		/* A scoreboard is waited for only once it has settled. */
		for (bar = 0U; bar < NVRTX_SM75_SCOREBOARDS; bar++) {
			if ((wait & (1U << bar)) == 0U)
				continue;
			earliest = nvrtx_sm75_later(earliest, scheduler->bar_set_cycle[bar] + NVRTX_SM75_SCOREBOARD_SETTLE);
		}

		/* The instruction before waits that many cycles; the field holds 15 at most. */
		if (previous != NULL) {
			delay = earliest - scheduler->cycle;
			if (delay > NVRTX_SM75_DELAY_MAX) {
				kern_free(target);
				kern_free(scheduler);
				*instruction = index;
				*reason = "a wait longer than the delay field holds";
				return 1;
			}

			/* The delay into the instruction before. */
			kern_memcpy(word.w, previous, sizeof(word.w));
			nvrtx_sm75_set(&word, NVRTX_SM75_DELAY_LOW, NVRTX_SM75_DELAY_BITS, delay);
			kern_memcpy(previous, word.w, sizeof(word.w));
		}

		/* The waits are done when it issues; then its own results and scoreboards are in flight. */
		nvrtx_sm75_release(scheduler, wait);
		nvrtx_sm75_record(scheduler, &operands, earliest, write_bar, read_bar);

		/* Its scoreboards, its waits and its yield; the delay comes with the next one. */
		kern_memcpy(word.w, slot, sizeof(word.w));
		nvrtx_sm75_control(&word, operands.min_delay, operands.boundary, write_bar, read_bar, wait);
		kern_memcpy(slot, word.w, sizeof(word.w));

		/* A block's last instruction leaves nothing in flight behind it. */
		if (operands.boundary != 0)
			nvrtx_sm75_clear(scheduler);

		/* The next instruction counts from this one: its cycle, its fewest cycles, and whether it is a barrier. */
		scheduler->cycle = earliest;
		previous = slot;
		previous_min = operands.min_delay;
		previous_sync = 0;
		if (operands.min_delay == NVRTX_SM75_DELAY_SYNC_MIN)
			previous_sync = 1;
	}

	/* The targets and the state are only this call's. */
	kern_free(target);
	kern_free(scheduler);

	/* Succeeded: every instruction has its control. */
	return 0;
}

/*
 * Reorders one block by list scheduling and writes it back over its code.
 * Returns 0, or ENOMEM-like 1 when the order cannot be kept.
 */
static int
nvrtx_sm75_reorder_block(
	struct nvrtx_sm75_node *nodes,
	uint32_t count,
	uint32_t sm,
	uint32_t *code)
{
	uint32_t *order;
	uint32_t before;
	uint32_t after;
	uint32_t latency;
	uint32_t path;
	uint32_t issued;
	uint32_t best;
	uint32_t soonest;
	uint32_t cycle;

	/* The new order. */
	order = kern_calloc(count, sizeof(*order));
	if (order == NULL)
		return 1;

	/* Each instruction's longest latency-weighted path to the block's end, from the last back. */
	for (before = count; before > 0U; before--) {
		path = nvrtx_sm75_guess(&nodes[before - 1U]);
		for (after = before; after < count; after++) {
			latency = nvrtx_sm75_dependence(&nodes[before - 1U], &nodes[after], sm);
			if (latency == NVRTX_SM75_NO_DEPENDENCE)
				continue;
			if (latency + nodes[after].priority > path)
				path = latency + nodes[after].priority;
		}

		/* The longest path is its priority. */
		nodes[before - 1U].priority = path;
	}

	/* Each instruction's predecessors. */
	for (after = 0U; after < count; after++) {
		nodes[after].waiting = 0U;
		nodes[after].earliest = 0U;
		nodes[after].issued = 0;
		for (before = 0U; before < after; before++) {
			latency = nvrtx_sm75_dependence(&nodes[before], &nodes[after], sm);
			if (latency != NVRTX_SM75_NO_DEPENDENCE)
				nodes[after].waiting++;
		}
	}

	/* Issues one instruction at a time: the ready one of the highest priority that can issue now, else the soonest. */
	cycle = 0U;
	for (issued = 0U; issued < count; issued++) {
		best = count;
		soonest = count;
		for (after = 0U; after < count; after++) {
			if (nodes[after].issued != 0 || nodes[after].waiting != 0U)
				continue;

			/* The soonest ready one, the first in the old order among equals. */
			if (soonest == count || nodes[after].earliest < nodes[soonest].earliest)
				soonest = after;

			/* The ready one of the highest priority that can issue at this cycle. */
			if (nodes[after].earliest > cycle)
				continue;
			if (best == count || nodes[after].priority > nodes[best].priority)
				best = after;
		}

		/* Nothing can issue now: the soonest one, later. */
		if (best == count) {
			best = soonest;
			cycle = nodes[best].earliest;
		}

		/* A ready instruction always exists in a block without cycles; none means the dependences are wrong. */
		if (best == count) {
			kern_free(order);
			return 1;
		}

		/* It issues, and its successors may issue after its latency. */
		order[issued] = best;
		nodes[best].issued = 1;
		for (after = best + 1U; after < count; after++) {
			latency = nvrtx_sm75_dependence(&nodes[best], &nodes[after], sm);
			if (latency == NVRTX_SM75_NO_DEPENDENCE)
				continue;
			nodes[after].waiting--;
			if (cycle + latency > nodes[after].earliest)
				nodes[after].earliest = cycle + latency;
		}

		/* The next issues a cycle later at the earliest. */
		cycle++;
	}

	/* The block's words in the new order. */
	for (issued = 0U; issued < count; issued++)
		kern_memcpy(code + (size_t)issued * NVRTX_SM75_INSTRUCTION_WORDS, nodes[order[issued]].words, sizeof(nodes[0].words));

	/* The order is only this call's. */
	kern_free(order);

	/* Succeeded: the block is reordered. */
	return 0;
}

/*
 * Reports the cycles an instruction must issue after an earlier one of its
 * block (an estimate for a variable-latency result), or
 * NVRTX_SM75_NO_DEPENDENCE when the two may swap: a read of a register or
 * predicate the earlier one writes, a write of one it reads or writes, and
 * two memory accesses of which one stores.
 */
static uint32_t
nvrtx_sm75_dependence(
	const struct nvrtx_sm75_node *before,
	const struct nvrtx_sm75_node *after,
	uint32_t sm)
{
	const struct nvrtx_sm75_operands *first;
	const struct nvrtx_sm75_operands *second;
	uint32_t latency;
	uint32_t i;
	uint32_t j;
	int first_memory;
	int second_memory;

	/* The two instructions' operands, and no dependence found yet. */
	first = &before->operands;
	second = &after->operands;
	latency = NVRTX_SM75_NO_DEPENDENCE;

	/* A register the second reads that the first writes: the first's result latency. */
	for (i = 0U; i < first->write_count; i++) {
		for (j = 0U; j < second->read_count; j++) {
			if (first->writes[i] != second->reads[j])
				continue;

			/* A variable-latency result: the estimate of its latency. */
			if (first->class_of == NVRTX_SM75_CLASS_DECOUPLED) {
				latency = nvrtx_sm75_guess(before);
				return latency;
			}

			/* A fixed-latency result: the shader model's table. */
			latency = nvrtx_sm75_raw(sm, first->class_of, second->read_class[j]);
			return latency;
		}
	}

	/* A predicate the second reads (as its guard or its source) that the first writes. */
	if (first->predicate_write != NVRTX_SM75_NO_PREDICATE) {
		/* A guard waits as a variable-latency reader does. */
		if (first->predicate_write == second->guard) {
			latency = nvrtx_sm75_predicate_raw(sm, NVRTX_SM75_CLASS_DECOUPLED);
			return latency;
		}

		/* A predicate source waits as its instruction's class does. */
		if (first->predicate_write == second->predicate_read) {
			latency = nvrtx_sm75_predicate_raw(sm, second->class_of);
			return latency;
		}
	}

	/* A register the second writes that the first reads or writes. */
	for (j = 0U; j < second->write_count; j++) {
		/* A write after a read. */
		for (i = 0U; i < first->read_count; i++) {
			if (first->reads[i] == second->writes[j])
				latency = 1U;
		}

		/* A write after a write. */
		for (i = 0U; i < first->write_count; i++) {
			if (first->writes[i] == second->writes[j])
				latency = 1U;
		}
	}

	/* A predicate the second writes that the first reads or writes. */
	if (second->predicate_write != NVRTX_SM75_NO_PREDICATE) {
		if (second->predicate_write == first->guard ||
		    second->predicate_write == first->predicate_read ||
		    second->predicate_write == first->predicate_write)
			latency = 1U;
	}

	/* Two memory accesses, one of them a store or an atomic. */
	first_memory = nvrtx_sm75_memory_kind(before->opcode);
	second_memory = nvrtx_sm75_memory_kind(after->opcode);
	if (first_memory != 0 &&
	    second_memory != 0 &&
	    (first_memory == 2 || second_memory == 2))
		latency = 1U;

	/* Succeeded: the order the pair needs, or none. */
	return latency;
}

/* Reports 2 for an instruction that stores to memory (or an attribute, or emits a vertex), 1 for one that loads it, 0 for neither. */
static int
nvrtx_sm75_memory_kind(
	uint32_t opcode)
{
	/* The stores and the atomics. */
	if (opcode == NVRTX_SM75_OP_STG ||
	    opcode == NVRTX_SM75_OP_STS ||
	    opcode == NVRTX_SM75_OP_AST)
		return 2;

	/* A geometry program's emits and cuts, which close the attribute stores before them. */
	if (opcode == NVRTX_SM75_OP_OUT_REG)
		return 2;
	if (opcode == NVRTX_SM75_OP_ATOMG ||
	    opcode == NVRTX_SM75_OP_ATOMG_CAS ||
	    opcode == NVRTX_SM75_OP_ATOMS ||
	    opcode == NVRTX_SM75_OP_ATOMS_CAS)
		return 2;

	/* The loads. */
	if (opcode == NVRTX_SM75_OP_LDG ||
	    opcode == NVRTX_SM75_OP_LDS ||
	    opcode == NVRTX_SM75_OP_ALD)
		return 1;

	/* Succeeded: no memory access. */
	return 0;
}

/* Reports the latency the reordering assumes for an instruction's result. */
static uint32_t
nvrtx_sm75_guess(
	const struct nvrtx_sm75_node *node)
{
	/* A fixed-latency result. */
	if (node->operands.class_of != NVRTX_SM75_CLASS_DECOUPLED)
		return NVRTX_SM75_WORST_GPR;

	/* The variable-latency ones by their opcode. */
	if (node->opcode == NVRTX_SM75_OP_TEX ||
	    node->opcode == NVRTX_SM75_OP_TLD ||
	    node->opcode == NVRTX_SM75_OP_TLD4 ||
	    node->opcode == NVRTX_SM75_OP_TMML ||
	    node->opcode == NVRTX_SM75_OP_TXD ||
	    node->opcode == NVRTX_SM75_OP_TXQ)
		return NVRTX_SM75_GUESS_TEXTURE;
	if (node->opcode == NVRTX_SM75_OP_LDG ||
	    node->opcode == NVRTX_SM75_OP_ATOMG ||
	    node->opcode == NVRTX_SM75_OP_ATOMG_CAS)
		return NVRTX_SM75_GUESS_GLOBAL;
	if (node->opcode == NVRTX_SM75_OP_LDS ||
	    node->opcode == NVRTX_SM75_OP_ATOMS ||
	    node->opcode == NVRTX_SM75_OP_ATOMS_CAS)
		return NVRTX_SM75_GUESS_SHARED;
	if (node->opcode == NVRTX_SM75_OP_LDC)
		return NVRTX_SM75_GUESS_CONSTANT;
	if (node->opcode == NVRTX_SM75_OP_S2R ||
	    node->opcode == NVRTX_SM75_OP_PIXLD ||
	    node->opcode == NVRTX_SM75_OP_SHFL)
		return NVRTX_SM75_GUESS_SPECIAL;
	/* Succeeded: the special function unit, the conversions, the attribute loads and the interpolation. */
	return NVRTX_SM75_GUESS_UNIT;
}

/* Reports nonzero for an instruction that keeps its place: one that ends a block, or a NOP. */
static int
nvrtx_sm75_fixed(
	const struct nvrtx_sm75_operands *operands)
{
	/* The ends of blocks. */
	if (operands->boundary != 0)
		return 1;

	/* NOPs (the padding, and the one after a barrier). */
	if (operands->class_of == NVRTX_SM75_CLASS_DISPATCH)
		return 1;

	/* Succeeded: it may move. */
	return 0;
}

/* Marks the instructions a branch or a convergence barrier leads to. */
static void
nvrtx_sm75_mark_targets(
	const uint32_t *code,
	uint32_t count,
	uint8_t *target)
{
	struct nvrtx_sm75_word word;
	uint64_t offset;
	int64_t to;
	uint32_t opcode;
	uint32_t index;

	/* Each branch and convergence barrier: its target counts in words from the next instruction. */
	for (index = 0U; index < count; index++) {
		kern_memcpy(word.w, code + (size_t)index * NVRTX_SM75_INSTRUCTION_WORDS, sizeof(word.w));
		opcode = (uint32_t)nvrtx_sm75_get(&word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS);

		/* The offset field and its sign. */
		if (opcode == NVRTX_SM75_OP_BRA) {
			offset = nvrtx_sm75_get(&word, NVRTX_SM75_BRA_OFFSET_LOW, NVRTX_SM75_BRA_OFFSET_BITS);
			if ((offset >> (NVRTX_SM75_BRA_OFFSET_BITS - 1U)) != 0U)
				offset |= ~((1ULL << NVRTX_SM75_BRA_OFFSET_BITS) - 1U);
		} else if (opcode == NVRTX_SM75_OP_BSSY) {
			offset = nvrtx_sm75_get(&word, NVRTX_SM75_BSSY_OFFSET_LOW, NVRTX_SM75_BSSY_OFFSET_BITS);
			if ((offset >> (NVRTX_SM75_BSSY_OFFSET_BITS - 1U)) != 0U)
				offset |= ~((1ULL << NVRTX_SM75_BSSY_OFFSET_BITS) - 1U);
		} else {
			continue;
		}

		/* A target inside the program starts a block. */
		to = (int64_t)index + 1 + (int64_t)offset / (int64_t)NVRTX_SM75_INSTRUCTION_WORDS;
		if (to >= 0 && to < (int64_t)count)
			target[to] = 1U;
	}
}

/*
 * Makes an instruction wait for everything in flight: every busy
 * scoreboard, every fixed-latency result's worst latency.
 *
 * Procedure referenced: Mesa 25.3.6
 * src/nouveau/compiler/nak/calc_instr_deps.rs (assign_barriers and
 * calc_delays).
 */
static void
nvrtx_sm75_flush(
	struct nvrtx_sm75_scheduler *scheduler,
	uint32_t *earliest,
	uint32_t *wait)
{
	uint32_t index;

	/* Every scoreboard in use. */
	*wait |= scheduler->bar_busy;

	/* Every fixed-latency register result. */
	for (index = 0U; index < NVRTX_SM75_TRACKED_GPRS; index++) {
		if (scheduler->gpr[index].written == 0)
			continue;
		if (scheduler->gpr[index].write_bar != NVRTX_SM75_NO_BAR)
			continue;
		*earliest = nvrtx_sm75_later(*earliest, scheduler->gpr[index].write_cycle + NVRTX_SM75_WORST_GPR);
	}

	/* Every predicate result. */
	for (index = 0U; index < NVRTX_SM75_TRACKED_PREDICATES; index++) {
		if (scheduler->predicate[index].written == 0)
			continue;
		*earliest = nvrtx_sm75_later(*earliest, scheduler->predicate[index].write_cycle + NVRTX_SM75_WORST_PREDICATE);
	}
}

/* Forgets everything in flight (after a block's end has waited it out); the scoreboards' set cycles stay. */
static void
nvrtx_sm75_clear(
	struct nvrtx_sm75_scheduler *scheduler)
{
	uint32_t index;

	/* The registers and predicates. */
	for (index = 0U; index < NVRTX_SM75_TRACKED_GPRS; index++) {
		kern_memset(&scheduler->gpr[index], 0, sizeof(scheduler->gpr[index]));
		scheduler->gpr[index].write_bar = NVRTX_SM75_NO_BAR;
	}

	/* The predicates likewise. */
	for (index = 0U; index < NVRTX_SM75_TRACKED_PREDICATES; index++) {
		kern_memset(&scheduler->predicate[index], 0, sizeof(scheduler->predicate[index]));
		scheduler->predicate[index].write_bar = NVRTX_SM75_NO_BAR;
	}

	/* No scoreboard is in use. */
	scheduler->bar_busy = 0U;
}

/*
 * Applies an instruction's reads and writes: a result in flight on a
 * scoreboard is waited for, a fixed-latency one is waited out by cycles.
 *
 * Procedure referenced: Mesa 25.3.6
 * src/nouveau/compiler/nak/calc_instr_deps.rs (assign_barriers and
 * calc_delays).
 */
static void
nvrtx_sm75_constrain(
	struct nvrtx_sm75_scheduler *scheduler,
	const struct nvrtx_sm75_operands *operands,
	uint32_t *earliest,
	uint32_t *wait)
{
	const struct nvrtx_sm75_track *track;
	uint32_t index;
	uint32_t reader;

	/* Each register read: after its writer. */
	for (index = 0U; index < operands->read_count; index++) {
		track = &scheduler->gpr[operands->reads[index]];
		if (track->written == 0)
			continue;
		if (track->write_bar != NVRTX_SM75_NO_BAR) {
			*wait |= 1U << track->write_bar;
			continue;
		}

		/* A fixed-latency writer is waited out by the shader model's latency. */
		*earliest = nvrtx_sm75_later(*earliest, track->write_cycle + nvrtx_sm75_raw(scheduler->sm, track->write_class, operands->read_class[index]));
	}

	/* Each register write: after the write and the reads before it. */
	for (index = 0U; index < operands->write_count; index++) {
		track = &scheduler->gpr[operands->writes[index]];
		*wait |= track->read_bars;
		if (track->written != 0 && track->write_bar != NVRTX_SM75_NO_BAR)
			*wait |= 1U << track->write_bar;

		/* A fixed-latency write before it. */
		if (track->written != 0 && track->write_bar == NVRTX_SM75_NO_BAR) {
			if (operands->class_of == NVRTX_SM75_CLASS_DECOUPLED) {
				*earliest = nvrtx_sm75_later(*earliest, track->write_cycle + NVRTX_SM75_WAW_DECOUPLED);
			} else {
				*earliest = nvrtx_sm75_later(*earliest, track->write_cycle + NVRTX_SM75_WAW_FIXED);
			}
		}

		/* A fixed-latency read before a variable-latency write. */
		if (operands->class_of == NVRTX_SM75_CLASS_DECOUPLED)
			*earliest = nvrtx_sm75_later(*earliest, track->read_cycle + NVRTX_SM75_WAR_DECOUPLED);
	}

	/* The guard and the predicate source: after the comparison that wrote them. */
	if (operands->guard != NVRTX_SM75_NO_PREDICATE) {
		track = &scheduler->predicate[operands->guard];
		if (track->written != 0)
			*earliest = nvrtx_sm75_later(*earliest, track->write_cycle + nvrtx_sm75_predicate_raw(scheduler->sm, NVRTX_SM75_CLASS_DECOUPLED));
	}

	/* A predicate source waits as its instruction's class does, a branch's as a variable-latency reader. */
	if (operands->predicate_read != NVRTX_SM75_NO_PREDICATE) {
		track = &scheduler->predicate[operands->predicate_read];
		reader = operands->class_of;
		if (operands->boundary != 0)
			reader = NVRTX_SM75_CLASS_DECOUPLED;
		if (track->written != 0)
			*earliest = nvrtx_sm75_later(*earliest, track->write_cycle + nvrtx_sm75_predicate_raw(scheduler->sm, reader));
	}

	/* The predicate written: after the write and the reads before it. */
	if (operands->predicate_write != NVRTX_SM75_NO_PREDICATE) {
		track = &scheduler->predicate[operands->predicate_write];
		if (track->written != 0)
			*earliest = nvrtx_sm75_later(*earliest, track->write_cycle + NVRTX_SM75_WAW_FIXED);
		*earliest = nvrtx_sm75_later(*earliest, track->read_cycle + NVRTX_SM75_WAR_DECOUPLED);
	}
}

/*
 * Chooses the scoreboards a variable-latency instruction sets: one its
 * result releases, when it writes a register, and one the read of its
 * sources releases, when it reads one.  A scoreboard is free when nothing
 * in flight names it and the instruction does not wait for it already
 * ending its use; when none is free, the one set longest ago is waited for
 * by this instruction and reused.
 *
 * Procedure referenced: Mesa 25.3.6
 * src/nouveau/compiler/nak/calc_instr_deps.rs (BarAlloc, try_find_free_bar
 * and free_some_bar, lines 214-228: the lowest free scoreboard, else the one
 * set longest ago).
 */
static void
nvrtx_sm75_take_bars(
	struct nvrtx_sm75_scheduler *scheduler,
	const struct nvrtx_sm75_operands *operands,
	uint32_t *wait,
	uint32_t *write_bar,
	uint32_t *read_bar)
{
	uint32_t free_bars;
	uint32_t bar;
	uint32_t oldest;
	uint32_t candidate;
	uint32_t need;
	uint32_t taken;

	/* A fixed-latency instruction sets none. */
	*write_bar = NVRTX_SM75_BAR_NONE;
	*read_bar = NVRTX_SM75_BAR_NONE;
	if (operands->class_of != NVRTX_SM75_CLASS_DECOUPLED)
		return;

	/* How many it needs. */
	need = 0U;
	if (operands->write_count != 0U)
		need++;
	if (operands->read_count != 0U)
		need++;

	/* Each one needed, the lowest free first, else the one set longest ago. */
	taken = 0U;
	while (need > 0U) {
		free_bars = NVRTX_SM75_SCOREBOARD_ALL & ~(scheduler->bar_busy & ~*wait) & ~taken;
		bar = NVRTX_SM75_SCOREBOARDS;
		oldest = 0xffffffffU;
		for (candidate = 0U; candidate < NVRTX_SM75_SCOREBOARDS; candidate++) {
			if ((taken & (1U << candidate)) != 0U)
				continue;

			/* The lowest free one. */
			if ((free_bars & (1U << candidate)) != 0U) {
				bar = candidate;
				break;
			}

			/* Else the one set longest ago so far. */
			if (scheduler->bar_set_cycle[candidate] < oldest) {
				oldest = scheduler->bar_set_cycle[candidate];
				bar = candidate;
			}
		}

		/* A busy one is waited for by this instruction, which ends its use. */
		if ((free_bars & (1U << bar)) == 0U)
			*wait |= 1U << bar;
		taken |= 1U << bar;

		/* The result's first, then the sources'. */
		if (*write_bar == NVRTX_SM75_BAR_NONE && operands->write_count != 0U) {
			*write_bar = bar;
		} else {
			*read_bar = bar;
		}

		/* One fewer is needed. */
		need--;
	}
}

/*
 * Records an instruction issued at a cycle: what it reads and writes is in
 * flight, on its scoreboards when it has them.
 */
static void
nvrtx_sm75_record(
	struct nvrtx_sm75_scheduler *scheduler,
	const struct nvrtx_sm75_operands *operands,
	uint32_t issue,
	uint32_t write_bar,
	uint32_t read_bar)
{
	struct nvrtx_sm75_track *track;
	uint32_t index;

	/* The reads: on the read scoreboard, or at this cycle. */
	for (index = 0U; index < operands->read_count; index++) {
		track = &scheduler->gpr[operands->reads[index]];
		if (read_bar != NVRTX_SM75_BAR_NONE) {
			track->read_bars |= 1U << read_bar;
		} else {
			track->read_cycle = issue;
		}
	}

	/* The writes: on the result scoreboard, or fixed-latency of the instruction's class. */
	for (index = 0U; index < operands->write_count; index++) {
		track = &scheduler->gpr[operands->writes[index]];
		track->written = 1;
		track->write_class = operands->class_of;
		track->write_cycle = issue;
		track->write_bar = NVRTX_SM75_NO_BAR;
		if (write_bar != NVRTX_SM75_BAR_NONE)
			track->write_bar = write_bar;
	}

	/* The predicates. */
	if (operands->guard != NVRTX_SM75_NO_PREDICATE)
		scheduler->predicate[operands->guard].read_cycle = issue;
	if (operands->predicate_read != NVRTX_SM75_NO_PREDICATE)
		scheduler->predicate[operands->predicate_read].read_cycle = issue;
	if (operands->predicate_write != NVRTX_SM75_NO_PREDICATE) {
		scheduler->predicate[operands->predicate_write].written = 1;
		scheduler->predicate[operands->predicate_write].write_cycle = issue;
	}

	/* The scoreboards it set are in use from this cycle. */
	if (write_bar != NVRTX_SM75_BAR_NONE) {
		scheduler->bar_busy |= 1U << write_bar;
		scheduler->bar_set_cycle[write_bar] = issue;
	}

	/* The sources' scoreboard likewise. */
	if (read_bar != NVRTX_SM75_BAR_NONE) {
		scheduler->bar_busy |= 1U << read_bar;
		scheduler->bar_set_cycle[read_bar] = issue;
	}
}

/* Ends the use of the scoreboards an instruction waited for: what they guarded is complete. */
static void
nvrtx_sm75_release(
	struct nvrtx_sm75_scheduler *scheduler,
	uint32_t wait)
{
	struct nvrtx_sm75_track *track;
	uint32_t index;

	/* Nothing waited. */
	if (wait == 0U)
		return;

	/* Each register: a result on a waited scoreboard is ready, reads on them are done. */
	for (index = 0U; index < NVRTX_SM75_TRACKED_GPRS; index++) {
		track = &scheduler->gpr[index];
		track->read_bars &= ~wait;
		if (track->write_bar != NVRTX_SM75_NO_BAR && (wait & (1U << track->write_bar)) != 0U) {
			track->written = 0;
			track->write_bar = NVRTX_SM75_NO_BAR;
		}
	}

	/* The scoreboards are free. */
	scheduler->bar_busy &= ~wait;
}

/*
 * Reports the cycles from a fixed-latency register write to a read in a
 * shader model's latency table.  An IMAD.WIDE writes as its upper half,
 * and its addend is read as NVRTX_SM75_CLASS_DISPATCH here.
 */
static uint32_t
nvrtx_sm75_raw(
	uint32_t sm,
	uint32_t writer,
	uint32_t reader)
{
	uint32_t latency;

	/* Ampere and Ada share one table. */
	if (sm >= NVRTX_SM_AMPERE_FIRST) {
		latency = nvrtx_sm75_raw_ampere(writer, reader);

		/* Succeeded: Ampere's and Ada's latency. */
		return latency;
	}

	/* Turing's. */
	latency = nvrtx_sm75_raw_turing(writer, reader);

	/* Succeeded: Turing's latency. */
	return latency;
}

/*
 * Reports Turing's cycles from a fixed-latency register write to a read
 * (the addend of an IMAD.WIDE reads late).
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/compiler/nak/sm75_instr_latencies.rs, read_after_write (lines
 * 205-412).
 */
static uint32_t
nvrtx_sm75_raw_turing(
	uint32_t writer,
	uint32_t reader)
{
	/* A variable-latency reader, of the address unit or not, waits 6 for any fixed-latency writer. */
	if (reader == NVRTX_SM75_CLASS_DECOUPLED || reader == NVRTX_SM75_CLASS_AGU)
		return 6U;

	/* An IMAD.WIDE's addend (the lower half reader). */
	if (reader == NVRTX_SM75_CLASS_DISPATCH) {
		if (writer == NVRTX_SM75_CLASS_ALU)
			return 5U;
		if (writer == NVRTX_SM75_CLASS_IMAD_WIDE)
			return 2U;
		return 4U;
	}

	/* An IMAD.WIDE's factors. */
	if (reader == NVRTX_SM75_CLASS_IMAD_WIDE) {
		if (writer == NVRTX_SM75_CLASS_ALU)
			return 5U;
		if (writer == NVRTX_SM75_CLASS_IMAD_WIDE)
			return 6U;
		return 4U;
	}

	/* A float or IMAD reader. */
	if (reader == NVRTX_SM75_CLASS_FMA || reader == NVRTX_SM75_CLASS_IMAD) {
		if (writer == NVRTX_SM75_CLASS_ALU || writer == NVRTX_SM75_CLASS_DISPATCH)
			return 5U;
		return 4U;
	}

	/* An ALU reader. */
	if (writer == NVRTX_SM75_CLASS_ALU || writer == NVRTX_SM75_CLASS_DISPATCH)
		return 4U;

	/* Succeeded: an FMA, IMAD or IMAD.WIDE writer of an ALU reader. */
	return 5U;
}

/*
 * Reports Ampere's and Ada's cycles from a fixed-latency register write to
 * a read (sm80_instr_latencies.rs, read_after_write: the writers are
 * CoupledAlu, CoupledFMA and IMADWideWriteDH, the addend is the
 * IMADWideReadCL reader, which reads as CoupledFMA does).
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/compiler/nak/sm80_instr_latencies.rs, read_after_write (lines
 * 259-576).
 */
static uint32_t
nvrtx_sm75_raw_ampere(
	uint32_t writer,
	uint32_t reader)
{
	/* A variable-latency reader of the address unit (DecoupledAgu) waits 5, any other (Decoupled) 4. */
	if (reader == NVRTX_SM75_CLASS_AGU)
		return 5U;
	if (reader == NVRTX_SM75_CLASS_DECOUPLED)
		return 4U;

	/* An IMAD.WIDE's factors (IMADWideReadAB). */
	if (reader == NVRTX_SM75_CLASS_IMAD_WIDE) {
		if (writer == NVRTX_SM75_CLASS_ALU || writer == NVRTX_SM75_CLASS_DISPATCH)
			return 5U;
		if (writer == NVRTX_SM75_CLASS_IMAD_WIDE)
			return 6U;
		return 4U;
	}

	/* A float or IMAD reader, or an IMAD.WIDE's addend. */
	if (reader == NVRTX_SM75_CLASS_FMA ||
	    reader == NVRTX_SM75_CLASS_IMAD ||
	    reader == NVRTX_SM75_CLASS_DISPATCH) {
		if (writer == NVRTX_SM75_CLASS_ALU || writer == NVRTX_SM75_CLASS_DISPATCH)
			return 5U;
		return 4U;
	}

	/* An ALU reader. */
	if (writer == NVRTX_SM75_CLASS_ALU || writer == NVRTX_SM75_CLASS_DISPATCH)
		return 4U;

	/* Succeeded: an FMA, IMAD or IMAD.WIDE writer of an ALU reader. */
	return 5U;
}

/*
 * Reports the cycles from a comparison's predicate (an ALU write) to a
 * read: 4 for an ALU reader, 5 for a float or IMAD one, and for a guard, a
 * branch or a variable-latency reader 12 on Turing (pred_read_after_write)
 * and 13 on Ampere and Ada (PredLatencySM80's Guard and Decoupled).
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/compiler/nak/sm75_instr_latencies.rs, pred_read_after_write
 * (lines 695-776), and src/nouveau/compiler/nak/sm80_instr_latencies.rs,
 * PredLatencySM80::pred_read_after_write (lines 889-958).
 */
static uint32_t
nvrtx_sm75_predicate_raw(
	uint32_t sm,
	uint32_t reader)
{
	/* An ALU reader. */
	if (reader == NVRTX_SM75_CLASS_ALU)
		return 4U;

	/* A float or IMAD reader. */
	if (reader == NVRTX_SM75_CLASS_FMA ||
	    reader == NVRTX_SM75_CLASS_IMAD ||
	    reader == NVRTX_SM75_CLASS_IMAD_WIDE)
		return 5U;

	/* A guard, a branch or a variable-latency reader on Ampere and Ada. */
	if (sm >= NVRTX_SM_AMPERE_FIRST)
		return 13U;

	/* Succeeded: a guard, a branch or a variable-latency reader on Turing. */
	return 12U;
}

/* Reports the later of two cycles. */
static uint32_t
nvrtx_sm75_later(
	uint32_t a,
	uint32_t b)
{
	/* The first when it is later. */
	if (a > b)
		return a;

	/* Succeeded: the second. */
	return b;
}
