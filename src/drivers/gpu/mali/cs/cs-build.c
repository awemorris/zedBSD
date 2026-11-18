/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Mali command stream's instruction encoders and the writer of a
 * run of them.  See cs-build.h.
 */

#include <stdbool.h>
#include <stdint.h>

#include "drivers/gpu/mali/cs/cs-build.h"

/*
 * The places of the operand fields every instruction shares: the opcode
 * in the top byte, a destination register below it, then a first
 * register operand, a second one, the wait mask and the signal entry.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "CS Base" and the
 * "start" and "size" of each instruction's fields).
 */
#define FIELD_OPCODE		56U
#define FIELD_DESTINATION	48U
#define FIELD_SIGNAL		48U
#define FIELD_FIRST		40U
#define FIELD_SECOND		32U
#define FIELD_WAIT		16U
#define MASK_REGISTER		0xffULL
#define MASK_SLOT		0xfULL
#define MASK_HALF		0xffffULL
#define MASK_WORD		0xffffffffULL
#define MASK_48			0xffffffffffffULL

/* The bytes one loop pass of a copy or a fill moves: sixteen 32-bit registers. */
#define LOOP_STEP_BYTES		64U

static uint64_t base(unsigned opcode);
static uint64_t registers(unsigned first, unsigned second);
static uint64_t async_part(uint32_t wait_mask, unsigned signal_slot);
static uint64_t sync_part(bool flag, unsigned scope, uint32_t wait_mask, unsigned data, unsigned address, unsigned signal_slot);
static void rows_loop(struct mali_cs *cs, bool fill, unsigned data, unsigned scratch, uint64_t destination, int32_t destination_stride, uint64_t source, int32_t source_stride, uint32_t row_bytes, uint32_t rows, unsigned load_store_slot);

/* Builds NOP: nothing. */
uint64_t
mali_cs_nop(void)
{
	return base(MALI_CS_OP_NOP);
}

/* Builds MOVE48: a 48-bit immediate into a register pair. */
uint64_t
mali_cs_move48(
	unsigned destination,
	uint64_t immediate)
{
	uint64_t word;

	/* The immediate in the low 48 bits, the pair below the opcode. */
	word = base(MALI_CS_OP_MOVE48);
	word |= (uint64_t)(destination & MASK_REGISTER) << FIELD_DESTINATION;
	word |= immediate & MASK_48;

	/* Succeeded: the instruction. */
	return word;
}

/* Builds MOVE32: a 32-bit immediate into one register. */
uint64_t
mali_cs_move32(
	unsigned destination,
	uint32_t immediate)
{
	uint64_t word;

	/* The immediate in the low 32 bits. */
	word = base(MALI_CS_OP_MOVE32);
	word |= (uint64_t)(destination & MASK_REGISTER) << FIELD_DESTINATION;
	word |= immediate;

	/* Succeeded: the instruction. */
	return word;
}

/* Builds WAIT: waits for the scoreboard entries of a mask. */
uint64_t
mali_cs_wait(
	uint32_t wait_mask)
{
	uint64_t word;

	/* The mask in bits 16 to 31. */
	word = base(MALI_CS_OP_WAIT);
	word |= (uint64_t)(wait_mask & MASK_HALF) << FIELD_WAIT;

	/* Succeeded: the instruction. */
	return word;
}

/*
 * Builds RUN_COMPUTE: the workgroups of the staging registers, split
 * into tasks along an axis, with which of the four sets of each
 * descriptor pointer to use.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "CS RUN_COMPUTE":
 * task increment 0..13, task axis 14..15, SRT 40, SPD 42, TSD 44,
 * FAU 46).
 */
uint64_t
mali_cs_run_compute(
	unsigned task_increment,
	unsigned task_axis,
	unsigned srt,
	unsigned spd,
	unsigned tsd,
	unsigned fau)
{
	uint64_t word;

	/* The task split, then the four selectors of two bits each. */
	word = base(MALI_CS_OP_RUN_COMPUTE);
	word |= (uint64_t)(task_increment & 0x3fffU);
	word |= (uint64_t)(task_axis & 3U) << 14;
	word |= (uint64_t)(srt & 3U) << 40;
	word |= (uint64_t)(spd & 3U) << 42;
	word |= (uint64_t)(tsd & 3U) << 44;
	word |= (uint64_t)(fau & 3U) << 46;

	/* Succeeded: the instruction. */
	return word;
}

/*
 * Builds RUN_IDVS: one draw of the staging registers through the vertex
 * shading, the tiler and, later, the fragment shading, with which set of
 * the descriptor pointers each stage takes.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "CS RUN_IDVS": flags
 * override 0..31, malloc enable 33, varying SRT 35, varying FAU 36,
 * varying TSD 37, fragment SRT 38, fragment TSD 39).
 */
uint64_t
mali_cs_run_idvs(
	uint32_t flags_override,
	bool malloc_enable,
	bool varying_srt,
	bool varying_fau,
	bool varying_tsd,
	bool fragment_srt,
	bool fragment_tsd)
{
	uint64_t word;

	/* The override of the draw's flags word, then the stage selectors. */
	word = base(MALI_CS_OP_RUN_IDVS);
	word |= flags_override;

	/* Whether the vertex shading allocates its varyings from the heap. */
	if (malloc_enable)
		word |= 1ULL << 33;

	/* The varying shader's set (0 or 1) of each pointer. */
	if (varying_srt)
		word |= 1ULL << 35;
	if (varying_fau)
		word |= 1ULL << 36;
	if (varying_tsd)
		word |= 1ULL << 37;

	/* The fragment shader's set (0 or 2) of the resource table and storage. */
	if (fragment_srt)
		word |= 1ULL << 38;
	if (fragment_tsd)
		word |= 1ULL << 39;

	/* Succeeded: the instruction. */
	return word;
}

/*
 * Builds RUN_FRAGMENT: the fragment work of the framebuffer the staging
 * registers name, in a tile order.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "CS RUN_FRAGMENT":
 * enable TEM 0, tile order 4..7).
 */
uint64_t
mali_cs_run_fragment(
	bool tile_enable_map,
	unsigned tile_order)
{
	uint64_t word;

	/* The tile order, and whether a tile enable map skips tiles. */
	word = base(MALI_CS_OP_RUN_FRAGMENT);
	word |= (uint64_t)(tile_order & 0xfU) << 4;
	if (tile_enable_map)
		word |= 1ULL;

	/* Succeeded: the instruction. */
	return word;
}

/* Builds FINISH_TILING: ends the tiler's work of the render pass. */
uint64_t
mali_cs_finish_tiling(void)
{
	return base(MALI_CS_OP_FINISH_TILING);
}

/*
 * Builds FINISH_FRAGMENT: frees the heap chunks the pass used, between
 * the chunk addresses of two register pairs, once the wait mask is met.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "CS FINISH_FRAGMENT":
 * increment 0, wait mask 16..31, last chunk 32..39, first chunk 40..47,
 * signal slot 48..51).
 */
uint64_t
mali_cs_finish_fragment(
	bool increment_completed,
	uint32_t wait_mask,
	unsigned last_chunk,
	unsigned first_chunk,
	unsigned signal_slot)
{
	uint64_t word;

	/* The two chunk pairs, the wait and the signal. */
	word = base(MALI_CS_OP_FINISH_FRAGMENT);
	word |= registers(first_chunk, last_chunk);
	word |= async_part(wait_mask, signal_slot);
	if (increment_completed)
		word |= 1ULL;

	/* Succeeded: the instruction. */
	return word;
}

/* Builds ADD_IMMEDIATE32: a register plus a signed immediate into a register. */
uint64_t
mali_cs_add32(
	unsigned destination,
	unsigned source,
	int32_t immediate)
{
	uint64_t word;

	/* The destination, the source and the immediate's 32 bits. */
	word = base(MALI_CS_OP_ADD32);
	word |= (uint64_t)(destination & MASK_REGISTER) << FIELD_DESTINATION;
	word |= (uint64_t)(source & MASK_REGISTER) << FIELD_FIRST;
	word |= (uint64_t)(uint32_t)immediate;

	/* Succeeded: the instruction. */
	return word;
}

/* Builds ADD_IMMEDIATE64: a register pair plus a signed immediate into a pair. */
uint64_t
mali_cs_add64(
	unsigned destination,
	unsigned source,
	int32_t immediate)
{
	uint64_t word;

	/* The destination pair, the source pair and the immediate. */
	word = base(MALI_CS_OP_ADD64);
	word |= (uint64_t)(destination & MASK_REGISTER) << FIELD_DESTINATION;
	word |= (uint64_t)(source & MASK_REGISTER) << FIELD_FIRST;
	word |= (uint64_t)(uint32_t)immediate;

	/* Succeeded: the instruction. */
	return word;
}

/*
 * Builds LOAD_MULTIPLE: the registers of a mask from base_register on,
 * read from the address in a pair plus a signed byte offset.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "CS LOAD_MULTIPLE":
 * offset 0..15, mask 16..31, address 40..47, base register 48..55).
 */
uint64_t
mali_cs_load(
	unsigned base_register,
	unsigned address,
	uint32_t mask,
	int32_t offset)
{
	uint64_t word;

	/* The base register, the address pair, the mask and the offset. */
	word = base(MALI_CS_OP_LOAD);
	word |= (uint64_t)(base_register & MASK_REGISTER) << FIELD_DESTINATION;
	word |= (uint64_t)(address & MASK_REGISTER) << FIELD_FIRST;
	word |= (uint64_t)(mask & MASK_HALF) << FIELD_WAIT;
	word |= (uint64_t)((uint32_t)offset & MASK_HALF);

	/* Succeeded: the instruction. */
	return word;
}

/* Builds STORE_MULTIPLE: the registers of a mask written to the address in a pair plus an offset. */
uint64_t
mali_cs_store(
	unsigned base_register,
	unsigned address,
	uint32_t mask,
	int32_t offset)
{
	uint64_t word;

	/* The same fields as a load. */
	word = base(MALI_CS_OP_STORE);
	word |= (uint64_t)(base_register & MASK_REGISTER) << FIELD_DESTINATION;
	word |= (uint64_t)(address & MASK_REGISTER) << FIELD_FIRST;
	word |= (uint64_t)(mask & MASK_HALF) << FIELD_WAIT;
	word |= (uint64_t)((uint32_t)offset & MASK_HALF);

	/* Succeeded: the instruction. */
	return word;
}

/*
 * Builds BRANCH: moves on by offset instructions from the next one when
 * a register compared with zero meets the condition.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "CS BRANCH": offset
 * 0..15, condition 28..31, value 40..47); cs_builder.h (a backward
 * branch's offset is the target less the branch's own place less one).
 */
uint64_t
mali_cs_branch(
	int32_t offset,
	unsigned condition,
	unsigned value)
{
	uint64_t word;

	/* The offset, the condition and the register tested. */
	word = base(MALI_CS_OP_BRANCH);
	word |= (uint64_t)((uint32_t)offset & MASK_HALF);
	word |= (uint64_t)(condition & 0xfU) << 28;
	word |= (uint64_t)(value & MASK_REGISTER) << FIELD_FIRST;

	/* Succeeded: the instruction. */
	return word;
}

/*
 * Builds SET_SB_ENTRY: the scoreboard entries endpoint work and other
 * asynchronous work (loads, stores) signal from now on.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "CS SET_SB_ENTRY":
 * endpoint entry 0..3, other entry 4..7).
 */
uint64_t
mali_cs_set_scoreboard(
	unsigned endpoint_entry,
	unsigned other_entry)
{
	uint64_t word;

	/* The two entries. */
	word = base(MALI_CS_OP_SET_SCOREBOARD);
	word |= (uint64_t)(endpoint_entry & 0xfU);
	word |= (uint64_t)(other_entry & 0xfU) << 4;

	/* Succeeded: the instruction. */
	return word;
}

/*
 * Builds SET_EXCEPTION_HANDLER: the stream called for an exception, its
 * address in a pair and its length in bytes in a register.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct
 * "CS SET_EXCEPTION_HANDLER": type 0..7, length 32..39, address 40..47).
 */
uint64_t
mali_cs_set_exception(
	unsigned exception,
	unsigned address,
	unsigned length)
{
	uint64_t word;

	/* The exception type and the two registers. */
	word = base(MALI_CS_OP_SET_EXCEPTION);
	word |= (uint64_t)(exception & MASK_REGISTER);
	word |= registers(address, length);

	/* Succeeded: the instruction. */
	return word;
}

/*
 * Builds CALL: runs the stream at the address in a pair, of the length
 * in bytes in a register, and comes back.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "CS CALL": length
 * 32..39, address 40..47).
 */
uint64_t
mali_cs_call(
	unsigned address,
	unsigned length)
{
	uint64_t word;

	/* The two registers. */
	word = base(MALI_CS_OP_CALL);
	word |= registers(address, length);

	/* Succeeded: the instruction. */
	return word;
}

/* Builds JUMP: goes on in the stream at the address in a pair, of the length in a register. */
uint64_t
mali_cs_jump(
	unsigned address,
	unsigned length)
{
	uint64_t word;

	/* The same fields as a call. */
	word = base(MALI_CS_OP_JUMP);
	word |= registers(address, length);

	/* Succeeded: the instruction. */
	return word;
}

/*
 * Builds REQ_RESOURCE: asks for the endpoints of a mask (compute,
 * fragment, tiler, vertex and tiling); zero gives them back.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "CS REQ_RESOURCE":
 * compute 0, fragment 1, tiler 2, IDVS 3).
 */
uint64_t
mali_cs_request_resource(
	unsigned resources)
{
	uint64_t word;

	/* The four bits. */
	word = base(MALI_CS_OP_REQUEST_RESOURCE);
	word |= (uint64_t)(resources & 0xfU);

	/* Succeeded: the instruction. */
	return word;
}

/*
 * Builds FLUSH_CACHE2: cleans or invalidates the L2, the load-store
 * caches and the others once the wait mask is met, with the flush id of
 * a register.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "CS FLUSH_CACHE2":
 * L2 0..3, LSC 4..7, other 8..11, wait mask 16..31, flush id 40..47,
 * signal slot 48..51).
 */
uint64_t
mali_cs_flush_cache(
	unsigned l2,
	unsigned load_store,
	unsigned other,
	uint32_t wait_mask,
	unsigned flush_id,
	unsigned signal_slot)
{
	uint64_t word;

	/* The three modes, the flush id, the wait and the signal. */
	word = base(MALI_CS_OP_FLUSH_CACHE);
	word |= (uint64_t)(l2 & 0xfU);
	word |= (uint64_t)(load_store & 0xfU) << 4;
	word |= (uint64_t)(other & 0xfU) << 8;
	word |= (uint64_t)(flush_id & MASK_REGISTER) << FIELD_FIRST;
	word |= async_part(wait_mask, signal_slot);

	/* Succeeded: the instruction. */
	return word;
}

/* Builds SYNC_ADD32: adds a register to the 32-bit sync object at a pair's address. */
uint64_t
mali_cs_sync_add32(
	bool propagate,
	unsigned scope,
	uint32_t wait_mask,
	unsigned data,
	unsigned address,
	unsigned signal_slot)
{
	return base(MALI_CS_OP_SYNC_ADD32) | sync_part(propagate, scope, wait_mask, data, address, signal_slot);
}

/* Builds SYNC_SET32: sets the 32-bit sync object at a pair's address to a register. */
uint64_t
mali_cs_sync_set32(
	bool propagate,
	unsigned scope,
	uint32_t wait_mask,
	unsigned data,
	unsigned address,
	unsigned signal_slot)
{
	return base(MALI_CS_OP_SYNC_SET32) | sync_part(propagate, scope, wait_mask, data, address, signal_slot);
}

/*
 * Builds SYNC_WAIT32: waits until the 32-bit sync object at a pair's
 * address compares with a register as the condition says.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "CS SYNC_WAIT32":
 * error reject 0, condition 28..31, data 32..39, address 40..47).
 */
uint64_t
mali_cs_sync_wait32(
	bool reject,
	unsigned condition,
	unsigned data,
	unsigned address)
{
	uint64_t word;

	/* The condition, the two registers and whether an error stops the stream. */
	word = base(MALI_CS_OP_SYNC_WAIT32);
	word |= (uint64_t)(condition & 0xfU) << 28;
	word |= registers(address, data);
	if (reject)
		word |= 1ULL;

	/* Succeeded: the instruction. */
	return word;
}

/* Builds SYNC_ADD64: adds a register pair to the 64-bit sync object at a pair's address. */
uint64_t
mali_cs_sync_add64(
	bool propagate,
	unsigned scope,
	uint32_t wait_mask,
	unsigned data,
	unsigned address,
	unsigned signal_slot)
{
	return base(MALI_CS_OP_SYNC_ADD64) | sync_part(propagate, scope, wait_mask, data, address, signal_slot);
}

/* Builds SYNC_SET64: sets the 64-bit sync object at a pair's address to a register pair. */
uint64_t
mali_cs_sync_set64(
	bool propagate,
	unsigned scope,
	uint32_t wait_mask,
	unsigned data,
	unsigned address,
	unsigned signal_slot)
{
	return base(MALI_CS_OP_SYNC_SET64) | sync_part(propagate, scope, wait_mask, data, address, signal_slot);
}

/* Builds SYNC_WAIT64: as SYNC_WAIT32, on a 64-bit sync object and a register pair. */
uint64_t
mali_cs_sync_wait64(
	bool reject,
	unsigned condition,
	unsigned data,
	unsigned address)
{
	uint64_t word;

	/* The same fields as the 32-bit wait. */
	word = base(MALI_CS_OP_SYNC_WAIT64);
	word |= (uint64_t)(condition & 0xfU) << 28;
	word |= registers(address, data);
	if (reject)
		word |= 1ULL;

	/* Succeeded: the instruction. */
	return word;
}

/*
 * Builds STORE_STATE: writes a state of the front end (a timestamp, the
 * cycle count, the error status) at a pair's address plus an offset.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "CS STORE_STATE":
 * offset 0..15, wait mask 16..31, state 32..39, address 40..47, signal
 * slot 48..51).
 */
uint64_t
mali_cs_store_state(
	int32_t offset,
	uint32_t wait_mask,
	unsigned state,
	unsigned address,
	unsigned signal_slot)
{
	uint64_t word;

	/* The offset, the state, the address, the wait and the signal. */
	word = base(MALI_CS_OP_STORE_STATE);
	word |= (uint64_t)((uint32_t)offset & MASK_HALF);
	word |= registers(address, state);
	word |= async_part(wait_mask, signal_slot);

	/* Succeeded: the instruction. */
	return word;
}

/* Builds ERROR_BARRIER: the place a stream recovers from an earlier fault at. */
uint64_t
mali_cs_error_barrier(void)
{
	return base(MALI_CS_OP_ERROR_BARRIER);
}

/*
 * Builds HEAP_SET: the tiler heap context at a pair's address.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "CS HEAP_SET":
 * address 40..47).
 */
uint64_t
mali_cs_heap_set(
	unsigned address)
{
	uint64_t word;

	/* The address pair. */
	word = base(MALI_CS_OP_HEAP_SET);
	word |= (uint64_t)(address & MASK_REGISTER) << FIELD_FIRST;

	/* Succeeded: the instruction. */
	return word;
}

/*
 * Builds HEAP_OPERATION: tells the heap a pass's vertex and tiler work
 * started or ended, or its fragment work ended.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "CS HEAP_OPERATION":
 * wait mask 16..31, operation 32..34, signal slot 48..51).
 */
uint64_t
mali_cs_heap_operation(
	uint32_t wait_mask,
	unsigned operation,
	unsigned signal_slot)
{
	uint64_t word;

	/* The operation, the wait and the signal. */
	word = base(MALI_CS_OP_HEAP_OPERATION);
	word |= (uint64_t)(operation & 7U) << FIELD_SECOND;
	word |= async_part(wait_mask, signal_slot);

	/* Succeeded: the instruction. */
	return word;
}

/* Reports the opcode of an instruction: its top byte. */
unsigned
mali_cs_opcode(
	uint64_t instruction)
{
	return (unsigned)(instruction >> FIELD_OPCODE);
}

/*
 * Starts a writer on a buffer of capacity instructions.
 */
void
mali_cs_init(
	struct mali_cs *cs,
	volatile uint64_t *words,
	uint32_t capacity)
{
	/* Empty, with no failure. */
	cs->words = words;
	cs->capacity = capacity;
	cs->count = 0;
	cs->error = 0;
}

/*
 * Appends one instruction, or latches the overflow.
 */
void
mali_cs_emit(
	struct mali_cs *cs,
	uint64_t instruction)
{
	/* Nothing after a failure, nothing past the end. */
	if (cs->error != 0)
		return;
	if (cs->count >= cs->capacity) {
		cs->error = 1;
		return;
	}

	/* The instruction in the next place. */
	cs->words[cs->count] = instruction;
	cs->count++;
}

/*
 * Appends the move of a 64-bit value into a register pair: one MOVE48
 * for a value of 48 bits, else a MOVE48 of its low half and a MOVE32 of
 * its high half.
 */
void
mali_cs_move64(
	struct mali_cs *cs,
	unsigned destination,
	uint64_t value)
{
	/* A GPU address fits 48 bits, and the move clears the high register. */
	if ((value >> 48) == 0U) {
		mali_cs_emit(cs, mali_cs_move48(destination, value));
		return;
	}

	/* A wider value in two steps. */
	mali_cs_emit(cs, mali_cs_move32(destination, (uint32_t)value));
	mali_cs_emit(cs, mali_cs_move32(destination + 1U, (uint32_t)(value >> 32)));
}

/* Reports the bytes the writer has written. */
uint32_t
mali_cs_bytes(
	const struct mali_cs *cs)
{
	return cs->count * MALI_CS_INSTRUCTION_BYTES;
}

/*
 * Appends a copy loop.  Each pass loads sixteen registers from the
 * source, waits for the load, stores them at the destination, moves both
 * addresses on by 64 and counts down; the store is waited for before the
 * loop's end, so later work sees the copy.
 */
void
mali_cs_copy(
	struct mali_cs *cs,
	unsigned data,
	unsigned scratch,
	uint64_t destination,
	uint64_t source,
	uint64_t bytes,
	unsigned load_store_slot)
{
	unsigned source_pair;
	unsigned destination_pair;
	unsigned count;
	uint32_t passes;
	uint32_t slot_mask;

	/* Nothing to copy, or a size the loop cannot step through. */
	if (bytes == 0U)
		return;
	if ((bytes % LOOP_STEP_BYTES) != 0U || bytes / LOOP_STEP_BYTES > 0xffffffffULL) {
		cs->error = 1;
		return;
	}

	/* The scratch registers: the source pair, the destination pair, then the pass count after them. */
	source_pair = scratch;
	destination_pair = scratch + 2U;
	count = scratch + 4U;
	passes = (uint32_t)(bytes / LOOP_STEP_BYTES);
	slot_mask = 1U << load_store_slot;
	mali_cs_move64(cs, source_pair, source);
	mali_cs_move64(cs, destination_pair, destination);
	mali_cs_emit(cs, mali_cs_move32(count, passes));

	/* The loop body: eight instructions, the branch back to its first. */
	mali_cs_emit(cs, mali_cs_load(data, source_pair, 0xffffU, 0));
	mali_cs_emit(cs, mali_cs_wait(slot_mask));
	mali_cs_emit(cs, mali_cs_store(data, destination_pair, 0xffffU, 0));
	mali_cs_emit(cs, mali_cs_wait(slot_mask));
	mali_cs_emit(cs, mali_cs_add64(source_pair, source_pair, (int32_t)LOOP_STEP_BYTES));
	mali_cs_emit(cs, mali_cs_add64(destination_pair, destination_pair, (int32_t)LOOP_STEP_BYTES));
	mali_cs_emit(cs, mali_cs_add32(count, count, -1));
	mali_cs_emit(cs, mali_cs_branch(-8, MALI_CS_NEQUAL, count));
}

/*
 * Appends a fill loop: the pattern into sixteen registers once, then a
 * store of them per pass.
 */
void
mali_cs_fill(
	struct mali_cs *cs,
	unsigned data,
	unsigned scratch,
	uint64_t destination,
	uint32_t pattern,
	uint64_t bytes,
	unsigned load_store_slot)
{
	unsigned destination_pair;
	unsigned count;
	unsigned index;
	uint32_t passes;
	uint32_t slot_mask;

	/* Nothing to fill, or a size the loop cannot step through. */
	if (bytes == 0U)
		return;
	if ((bytes % LOOP_STEP_BYTES) != 0U || bytes / LOOP_STEP_BYTES > 0xffffffffULL) {
		cs->error = 1;
		return;
	}

	/* The pattern in every data register. */
	for (index = 0; index < MALI_CS_MOVE_REGISTERS; index++)
		mali_cs_emit(cs, mali_cs_move32(data + index, pattern));

	/* The destination pair and the pass count. */
	destination_pair = scratch;
	count = scratch + 2U;
	passes = (uint32_t)(bytes / LOOP_STEP_BYTES);
	slot_mask = 1U << load_store_slot;
	mali_cs_move64(cs, destination_pair, destination);
	mali_cs_emit(cs, mali_cs_move32(count, passes));

	/* The loop body: five instructions, the branch back to its first. */
	mali_cs_emit(cs, mali_cs_store(data, destination_pair, 0xffffU, 0));
	mali_cs_emit(cs, mali_cs_wait(slot_mask));
	mali_cs_emit(cs, mali_cs_add64(destination_pair, destination_pair, (int32_t)LOOP_STEP_BYTES));
	mali_cs_emit(cs, mali_cs_add32(count, count, -1));
	mali_cs_emit(cs, mali_cs_branch(-5, MALI_CS_NEQUAL, count));
}

/* Appends a copy of rows (see cs-build.h). */
void
mali_cs_copy_rows(
	struct mali_cs *cs,
	unsigned data,
	unsigned scratch,
	uint64_t destination,
	int32_t destination_stride,
	uint64_t source,
	int32_t source_stride,
	uint32_t row_bytes,
	uint32_t rows,
	unsigned load_store_slot)
{
	rows_loop(cs, false, data, scratch, destination, destination_stride, source, source_stride, row_bytes, rows, load_store_slot);
}

/* Appends a fill of rows (see cs-build.h): the pattern into the data registers, then the loop. */
void
mali_cs_fill_rows(
	struct mali_cs *cs,
	unsigned data,
	unsigned scratch,
	uint64_t destination,
	int32_t stride,
	uint32_t pattern,
	uint32_t row_bytes,
	uint32_t rows,
	unsigned load_store_slot)
{
	unsigned index;

	/* The pattern in every data register. */
	for (index = 0; index < MALI_CS_MOVE_REGISTERS; index++)
		mali_cs_emit(cs, mali_cs_move32(data + index, pattern));

	/* The loop. */
	rows_loop(cs, true, data, scratch, destination, stride, 0, 0, row_bytes, rows, load_store_slot);
}

/*
 * Appends a two-level loop over rows: the outer loop walks the row
 * starts, the inner one moves 64 bytes a pass, and the row's tail of
 * fewer than 64 bytes is moved with one masked load and store.  With
 * fill, nothing is loaded: the data registers hold the pattern.
 */
static void
rows_loop(
	struct mali_cs *cs,
	bool fill,
	unsigned data,
	unsigned scratch,
	uint64_t destination,
	int32_t destination_stride,
	uint64_t source,
	int32_t source_stride,
	uint32_t row_bytes,
	uint32_t rows,
	unsigned load_store_slot)
{
	unsigned source_row;
	unsigned destination_row;
	unsigned source_pair;
	unsigned destination_pair;
	unsigned row_count;
	unsigned pass_count;
	uint32_t passes;
	uint32_t tail_mask;
	uint32_t slot_mask;
	uint32_t outer;
	uint32_t inner;

	/* Nothing to move, or a row the loop cannot step through. */
	if (rows == 0U || row_bytes == 0U)
		return;
	if ((row_bytes & 3U) != 0U) {
		cs->error = 1;
		return;
	}

	/* The scratch: the row starts, the moving addresses, then the two counts. */
	source_row = scratch;
	destination_row = scratch + 2U;
	source_pair = scratch + 4U;
	destination_pair = scratch + 6U;
	row_count = scratch + 8U;
	pass_count = scratch + 9U;
	passes = row_bytes / LOOP_STEP_BYTES;
	tail_mask = (1U << ((row_bytes % LOOP_STEP_BYTES) / 4U)) - 1U;
	slot_mask = 1U << load_store_slot;
	if (!fill)
		mali_cs_move64(cs, source_row, source);
	mali_cs_move64(cs, destination_row, destination);
	mali_cs_emit(cs, mali_cs_move32(row_count, rows));

	/* The outer loop's start: this row's addresses. */
	outer = cs->count;
	if (!fill)
		mali_cs_emit(cs, mali_cs_add64(source_pair, source_row, 0));
	mali_cs_emit(cs, mali_cs_add64(destination_pair, destination_row, 0));

	/* The inner loop over the row's whole passes. */
	if (passes != 0U) {
		mali_cs_emit(cs, mali_cs_move32(pass_count, passes));
		inner = cs->count;
		if (!fill) {
			mali_cs_emit(cs, mali_cs_load(data, source_pair, 0xffffU, 0));
			mali_cs_emit(cs, mali_cs_wait(slot_mask));
			mali_cs_emit(cs, mali_cs_add64(source_pair, source_pair, (int32_t)LOOP_STEP_BYTES));
		}
		mali_cs_emit(cs, mali_cs_store(data, destination_pair, 0xffffU, 0));
		mali_cs_emit(cs, mali_cs_wait(slot_mask));
		mali_cs_emit(cs, mali_cs_add64(destination_pair, destination_pair, (int32_t)LOOP_STEP_BYTES));
		mali_cs_emit(cs, mali_cs_add32(pass_count, pass_count, -1));
		mali_cs_emit(cs, mali_cs_branch((int32_t)inner - (int32_t)cs->count - 1, MALI_CS_NEQUAL, pass_count));
	}

	/* The tail of the row. */
	if (tail_mask != 0U) {
		if (!fill) {
			mali_cs_emit(cs, mali_cs_load(data, source_pair, tail_mask, 0));
			mali_cs_emit(cs, mali_cs_wait(slot_mask));
		}
		mali_cs_emit(cs, mali_cs_store(data, destination_pair, tail_mask, 0));
		mali_cs_emit(cs, mali_cs_wait(slot_mask));
	}

	/* The next row, until the rows are done. */
	if (!fill)
		mali_cs_emit(cs, mali_cs_add64(source_row, source_row, source_stride));
	mali_cs_emit(cs, mali_cs_add64(destination_row, destination_row, destination_stride));
	mali_cs_emit(cs, mali_cs_add32(row_count, row_count, -1));
	mali_cs_emit(cs, mali_cs_branch((int32_t)outer - (int32_t)cs->count - 1, MALI_CS_NEQUAL, row_count));
}

/* Gives an instruction with only its opcode. */
static uint64_t
base(
	unsigned opcode)
{
	return (uint64_t)(opcode & MASK_REGISTER) << FIELD_OPCODE;
}

/* Gives the two register operands of bits 40..47 and 32..39. */
static uint64_t
registers(
	unsigned first,
	unsigned second)
{
	uint64_t word;

	/* The first above the second. */
	word = (uint64_t)(first & MASK_REGISTER) << FIELD_FIRST;
	word |= (uint64_t)(second & MASK_REGISTER) << FIELD_SECOND;

	/* Succeeded: the two fields. */
	return word;
}

/*
 * Gives the wait mask and signal entry of an asynchronous instruction.
 * An instruction that waits for nothing signals nothing, as the front
 * end runs it at once.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/genxml/cs_builder.h (cs_apply_async for
 * PAN_ARCH 10).
 */
static uint64_t
async_part(
	uint32_t wait_mask,
	unsigned signal_slot)
{
	uint64_t word;

	/* The wait mask, and the signal entry. */
	word = (uint64_t)(wait_mask & MASK_HALF) << FIELD_WAIT;
	word |= (uint64_t)(signal_slot & MASK_SLOT) << FIELD_SIGNAL;

	/* Succeeded: the two fields. */
	return word;
}

/*
 * Gives the fields every sync add and set shares.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (structs "CS SYNC_ADD32",
 * "CS SYNC_SET32", "CS SYNC_ADD64", "CS SYNC_SET64": error propagate 0,
 * scope 1..2, wait mask 16..31, data 32..39, address 40..47, signal slot
 * 48..51).
 */
static uint64_t
sync_part(
	bool flag,
	unsigned scope,
	uint32_t wait_mask,
	unsigned data,
	unsigned address,
	unsigned signal_slot)
{
	uint64_t word;

	/* The scope, the two registers, the wait and the signal. */
	word = (uint64_t)(scope & 3U) << 1;
	word |= registers(address, data);
	word |= async_part(wait_mask, signal_slot);

	/* Whether an error of the work waited for goes into the sync object. */
	if (flag)
		word |= 1ULL;

	/* Succeeded: the fields. */
	return word;
}
