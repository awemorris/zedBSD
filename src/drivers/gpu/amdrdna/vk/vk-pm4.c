/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The writer of an indirect buffer of command packets (amdrdna-vk-gfx10.h).
 *
 * Each function writes one whole packet or nothing: a packet that would
 * pass the buffer's end marks the buffer full, and every later packet is
 * dropped too, so a full buffer never holds half a packet.  The caller
 * checks full once the buffer is written.  The packets the driver's ring
 * commands also write (the register writes, the context's reset, the
 * memory wait, the fetcher's sync, the dispatch, the filler) are built by
 * the driver's builders (command-format.c), the others here with the
 * driver's header.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"
#include "drivers/gpu/amdrdna/vk/amdrdna-vk-gfx10.h"

/* The words of the context's reset: its loads' packet (three) and the reset to the clear state (two). */
#define PM4_CONTEXT_START_WORDS		5U

static bool pm4_room(struct amdrdna_vk_pm4 *pm4, uint32_t words);
static void pm4_context_forget(struct amdrdna_vk_pm4 *pm4);
static void pm4_context_note(struct amdrdna_vk_pm4 *pm4, uint32_t reg, const uint32_t *values, uint32_t count);
static bool pm4_user_holds(const struct amdrdna_vk_pm4 *pm4, uint32_t reg, uint32_t value);
static void pm4_user_note(struct amdrdna_vk_pm4 *pm4, uint32_t reg, uint32_t value);
static void pm4_set_run(struct amdrdna_vk_pm4 *pm4, enum amdrdna_register_space space, uint32_t reg, const uint32_t *values, uint32_t count);

/* Starts a buffer over words of capacity words. */
void
amdrdna_vk_pm4_start(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t *words,
	uint32_t capacity)
{
	/* Empty, with room for capacity words. */
	pm4->words = words;
	pm4->count = 0U;
	pm4->capacity = capacity;
	pm4->full = false;

	/* What drew before this buffer is not known, so the first draw flushes first. */
	pm4->vertex_stage = AMDRDNA_VK_STAGE_UNKNOWN;

	/* Nor what its context registers hold. */
	pm4_context_forget(pm4);
}

/*
 * Writes a packet's header: the operation, the words of the body that
 * follows (at least one), and whether it is the compute shader's.  The
 * room for the whole packet is checked here; the body's words follow
 * through amdrdna_vk_pm4_word().
 */
void
amdrdna_vk_pm4_packet(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t operation,
	uint32_t body_words,
	bool compute)
{
	uint32_t header;
	bool room;

	/* The header and the body must fit. */
	room = pm4_room(pm4, 1U + body_words);
	if (!room)
		return;

	/* The type, the body's words less one, the operation, the compute bit. */
	header = amdrdna_pm4_header(operation & 0xffU, body_words, compute);
	pm4->words[pm4->count] = header;
	pm4->count++;
}

/* Writes one word of a packet's body (dropped once the buffer is full). */
void
amdrdna_vk_pm4_word(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t word)
{
	/* A full buffer takes nothing more. */
	if (pm4->full)
		return;

	/* The header made the room for its body. */
	if (pm4->count >= pm4->capacity) {
		pm4->full = true;
		return;
	}
	pm4->words[pm4->count] = word;
	pm4->count++;
}

/*
 * Loads and keeps every register group's state and resets the context
 * registers to the clear state (the driver's two packets).
 */
void
amdrdna_vk_pm4_context_start(
	struct amdrdna_vk_pm4 *pm4)
{
	bool room;

	/* The context's loads, then its reset. */
	room = pm4_room(pm4, PM4_CONTEXT_START_WORDS);
	if (!room)
		return;
	pm4->count += amdrdna_pm4_context_loads(pm4->words + pm4->count);
	pm4->count += amdrdna_pm4_state_reset(pm4->words + pm4->count);

	/* The reset leaves the context registers at their clear state, which is not tracked. */
	pm4_context_forget(pm4);
}

/* Sets one context register (byte offset from 0x28000 on). */
void
amdrdna_vk_pm4_set_context(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t reg,
	uint32_t value)
{
	uint32_t index;
	uint32_t bit;

	/* A register the buffer set to the value already is left as it is. */
	index = (reg >> 2) - AMDRDNA_PM4_CONTEXT_FIRST;
	if (index < AMDRDNA_VK_CONTEXT_REGISTERS) {
		bit = 1U << (index % 32U);
		if ((pm4->context_known[index / 32U] & bit) != 0U && pm4->context_values[index] == value)
			return;
	}

	/* A run of one, then what the register holds. */
	pm4_set_run(pm4, AMDRDNA_SPACE_CONTEXT, reg, &value, 1U);
	pm4_context_note(pm4, reg, &value, 1U);
}

/* Sets count consecutive context registers from reg. */
void
amdrdna_vk_pm4_set_context_run(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t reg,
	const uint32_t *values,
	uint32_t count)
{
	/* One packet for the run, then what the registers hold. */
	pm4_set_run(pm4, AMDRDNA_SPACE_CONTEXT, reg, values, count);
	pm4_context_note(pm4, reg, values, count);
}

/* Sets one shader register (byte offset from 0xb000 on). */
void
amdrdna_vk_pm4_set_shader(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t reg,
	uint32_t value)
{
	/* A run of one. */
	pm4_set_run(pm4, AMDRDNA_SPACE_SHADER, reg, &value, 1U);
}

/* Sets count consecutive shader registers from reg. */
void
amdrdna_vk_pm4_set_shader_run(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t reg,
	const uint32_t *values,
	uint32_t count)
{
	/* One packet for the run. */
	pm4_set_run(pm4, AMDRDNA_SPACE_SHADER, reg, values, count);
}

/* Sets one user configuration register (byte offset from 0x30000 on). */
void
amdrdna_vk_pm4_set_user_config(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t reg,
	uint32_t value)
{
	bool held;

	/* A register the buffer set to the value already is left as it is. */
	held = pm4_user_holds(pm4, reg, value);
	if (held)
		return;

	/* A run of one, then what the register holds. */
	pm4_set_run(pm4, AMDRDNA_SPACE_USER, reg, &value, 1U);
	pm4_user_note(pm4, reg, value);
}

/*
 * Sets one user configuration register through the indexed setter: the
 * register's place with the index above it.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cs.h
 * (radeon_set_uconfig_reg_idx).
 */
void
amdrdna_vk_pm4_set_user_config_indexed(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t reg,
	uint32_t index,
	uint32_t value)
{
	bool held;

	/* A register the buffer set to the value already is left as it is. */
	held = pm4_user_holds(pm4, reg, value);
	if (held)
		return;

	/* The register's place and its index, then the value, then what the register holds. */
	amdrdna_vk_pm4_packet(pm4, AMDRDNA_PM4_OP_USER_REGISTERS_INDEXED, 2U, false);
	amdrdna_vk_pm4_word(pm4, ((reg >> 2) - AMDRDNA_PM4_USER_FIRST) | (index << AMDRDNA_PM4_REGISTER_INDEX_SHIFT));
	amdrdna_vk_pm4_word(pm4, value);
	pm4_user_note(pm4, reg, value);
}

/*
 * Sets one shader register through the indexed setter, the index above
 * the register's place: index 3 has the command processor apply its CU
 * mask to a CU enable register.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cs.h
 * (radeon_set_sh_reg_idx: PKT3_SET_SH_REG_INDEX on GFX10, the index from
 * bit 28) and src/amd/vulkan/radv_cmd_buffer.c (the CU enable words
 * SPI_SHADER_PGM_RSRC3 and RSRC4 written with index 3).
 */
void
amdrdna_vk_pm4_set_shader_indexed(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t reg,
	uint32_t index,
	uint32_t value)
{
	/* The register's place and its index, then the value. */
	amdrdna_vk_pm4_packet(pm4, AMDRDNA_PM4_OP_SHADER_REGISTERS_INDEXED, 2U, false);
	amdrdna_vk_pm4_word(pm4, ((reg >> 2) - AMDRDNA_PM4_SHADER_FIRST) | (index << AMDRDNA_PM4_REGISTER_INDEX_SHIFT));
	amdrdna_vk_pm4_word(pm4, value);
}

/* Sends an event of a type with its index. */
void
amdrdna_vk_pm4_event(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t event,
	uint32_t index)
{
	/* One word: the type and the index. */
	amdrdna_vk_pm4_packet(pm4, AMDRDNA_PM4_OP_EVENT, 1U, false);
	amdrdna_vk_pm4_word(pm4, event | (index << AMDRDNA_PM4_EVENT_INDEX_SHIFT));
}

/*
 * Sends an end-of-pipe event with cache actions that writes a 32-bit value
 * to memory once everything before it has finished.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cs.c
 * (radv_cs_emit_write_event_eop, the GFX9 and later form).
 */
void
amdrdna_vk_pm4_event_fence(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t event,
	uint32_t caches,
	uint64_t address,
	uint32_t value)
{
	/* The event and its cache actions, where the value goes, the value. */
	amdrdna_vk_pm4_packet(pm4, AMDRDNA_PM4_OP_EVENT_FENCE, 7U, false);
	amdrdna_vk_pm4_word(pm4, event | (AMDRDNA_PM4_EVENT_INDEX_TIMESTAMP << AMDRDNA_PM4_EVENT_INDEX_SHIFT) | caches);
	amdrdna_vk_pm4_word(pm4, AMDRDNA_PM4_FENCE_TO_MEMORY | AMDRDNA_PM4_FENCE_DATA_LOW | AMDRDNA_PM4_FENCE_INTERRUPT_AFTER_WRITE);
	amdrdna_vk_pm4_word(pm4, (uint32_t)address);
	amdrdna_vk_pm4_word(pm4, (uint32_t)(address >> 32));
	amdrdna_vk_pm4_word(pm4, value);
	amdrdna_vk_pm4_word(pm4, 0U);
	amdrdna_vk_pm4_word(pm4, 0U);
}

/* Sends an event of a type with its index and the address it writes (the depth units' counts). */
void
amdrdna_vk_pm4_event_address(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t event,
	uint32_t index,
	uint64_t address)
{
	/* The type and the index, then the address. */
	amdrdna_vk_pm4_packet(pm4, AMDRDNA_PM4_OP_EVENT, 3U, false);
	amdrdna_vk_pm4_word(pm4, event | (index << AMDRDNA_PM4_EVENT_INDEX_SHIFT));
	amdrdna_vk_pm4_word(pm4, (uint32_t)address);
	amdrdna_vk_pm4_word(pm4, (uint32_t)(address >> 32));
}

/*
 * Writes the GPU's 64-bit timestamp to memory once everything before it
 * has finished: an end-of-pipe event at the bottom of the pipe without
 * cache actions.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_query.c
 * (radv_write_timestamp: the stages after the top of the pipe) and
 * src/amd/vulkan/radv_cs.c (radv_cs_emit_write_event_eop, the GFX9 and
 * later form: the event at index 5, the timestamp's data select, the
 * interrupt after the write is confirmed).
 */
void
amdrdna_vk_pm4_timestamp(
	struct amdrdna_vk_pm4 *pm4,
	uint64_t address)
{
	/* The event, where the timestamp goes, no immediate value. */
	amdrdna_vk_pm4_packet(pm4, AMDRDNA_PM4_OP_EVENT_FENCE, 7U, false);
	amdrdna_vk_pm4_word(pm4, AMDRDNA_PM4_EVENT_BOTTOM_OF_PIPE | (AMDRDNA_PM4_EVENT_INDEX_TIMESTAMP << AMDRDNA_PM4_EVENT_INDEX_SHIFT));
	amdrdna_vk_pm4_word(pm4, AMDRDNA_PM4_FENCE_TO_MEMORY | AMDRDNA_PM4_FENCE_DATA_TIMESTAMP | AMDRDNA_PM4_FENCE_INTERRUPT_AFTER_WRITE);
	amdrdna_vk_pm4_word(pm4, (uint32_t)address);
	amdrdna_vk_pm4_word(pm4, (uint32_t)(address >> 32));
	amdrdna_vk_pm4_word(pm4, 0U);
	amdrdna_vk_pm4_word(pm4, 0U);
	amdrdna_vk_pm4_word(pm4, 0U);
}

/* Waits until a memory word equals a value (on the micro engine). */
void
amdrdna_vk_pm4_wait_value(
	struct amdrdna_vk_pm4 *pm4,
	uint64_t address,
	uint32_t value)
{
	bool room;

	/* The driver's memory wait: equal, every bit, its poll interval. */
	room = pm4_room(pm4, AMDRDNA_PM4_WAIT_WORDS);
	if (!room)
		return;
	pm4->count += amdrdna_pm4_wait_memory(pm4->words + pm4->count, address, value, false);
}

/*
 * Writes back and invalidates the caches a set of actions names, over the
 * whole address space, and waits for them.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cs.c
 * (gfx10_cs_emit_cache_flush, the acquire of the cache actions).
 */
void
amdrdna_vk_pm4_cache_acquire(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t caches)
{
	/* No coherency control, the whole space, the poll interval, the actions. */
	amdrdna_vk_pm4_packet(pm4, AMDRDNA_PM4_OP_CACHE_SYNC, 7U, false);
	amdrdna_vk_pm4_word(pm4, 0U);
	amdrdna_vk_pm4_word(pm4, AMDRDNA_PM4_SYNC_SIZE_LOW);
	amdrdna_vk_pm4_word(pm4, AMDRDNA_PM4_SYNC_SIZE_HIGH);
	amdrdna_vk_pm4_word(pm4, 0U);
	amdrdna_vk_pm4_word(pm4, 0U);
	amdrdna_vk_pm4_word(pm4, AMDRDNA_PM4_SYNC_POLL);
	amdrdna_vk_pm4_word(pm4, caches);
}

/* Has the fetcher wait for the engine (after a wait the engine did). */
void
amdrdna_vk_pm4_fetcher_sync(
	struct amdrdna_vk_pm4 *pm4)
{
	bool room;

	/* The driver's packet: one word of nothing. */
	room = pm4_room(pm4, AMDRDNA_PM4_FETCH_SYNC_WORDS);
	if (!room)
		return;
	pm4->count += amdrdna_pm4_fetcher_sync(pm4->words + pm4->count);
}

/*
 * Moves bytes from source to destination, two addresses of the session's
 * space, through the level-2 cache; the engine waits for the move before
 * the next packet.  Any alignment and at most AMDRDNA_VK_MOVE_MAX bytes.
 * The driver's packet is built aside and copied in, so the buffer (in
 * the CPU's uncached window) is only written.
 */
void
amdrdna_vk_pm4_move(
	struct amdrdna_vk_pm4 *pm4,
	uint64_t source,
	uint64_t destination,
	uint32_t bytes)
{
	uint32_t words[AMDRDNA_PM4_MOVE_WORDS];
	unsigned count;
	unsigned index;
	bool room;

	/* The driver's packet, built aside. */
	room = pm4_room(pm4, AMDRDNA_PM4_MOVE_WORDS);
	if (!room)
		return;
	count = amdrdna_pm4_move_cached(words, source, destination, bytes);

	/* Copied into the buffer word by word. */
	for (index = 0U; index < count; index++) {
		pm4->words[pm4->count] = words[index];
		pm4->count++;
	}
}

/*
 * Fills bytes at destination, an address of the session's space, with a
 * word, through the level-2 cache; the engine waits for the fill before
 * the next packet.  Whole words, at most AMDRDNA_VK_MOVE_MAX bytes.
 */
void
amdrdna_vk_pm4_fill(
	struct amdrdna_vk_pm4 *pm4,
	uint64_t destination,
	uint32_t value,
	uint32_t bytes)
{
	uint32_t words[AMDRDNA_PM4_MOVE_WORDS];
	unsigned count;
	unsigned index;
	bool room;

	/* The driver's packet, built aside. */
	room = pm4_room(pm4, AMDRDNA_PM4_MOVE_WORDS);
	if (!room)
		return;
	count = amdrdna_pm4_fill_cached(words, destination, value, bytes);

	/* Copied into the buffer word by word. */
	for (index = 0U; index < count; index++) {
		pm4->words[pm4->count] = words[index];
		pm4->count++;
	}
}

/*
 * Draws vertex_count generated vertices of instance_count instances.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_cs_emit_draw_packet and the instance count before it).
 */
void
amdrdna_vk_pm4_draw(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t vertex_count,
	uint32_t instance_count)
{
	/* The instances, unless the processor holds the count already. */
	if (!pm4->instances_known || pm4->instances != instance_count) {
		amdrdna_vk_pm4_packet(pm4, AMDRDNA_PM4_OP_INSTANCES, 1U, false);
		amdrdna_vk_pm4_word(pm4, instance_count);
		pm4->instances = instance_count;
		pm4->instances_known = true;
		if (pm4->full)
			pm4->instances_known = false;
	}

	/* The vertices, their indices generated. */
	amdrdna_vk_pm4_packet(pm4, AMDRDNA_PM4_OP_DRAW_GENERATED, 2U, false);
	amdrdna_vk_pm4_word(pm4, vertex_count);
	amdrdna_vk_pm4_word(pm4, AMDRDNA_PM4_DRAW_GENERATED_INDICES);
}

/*
 * Draws index_count indices read from indices (an address of the
 * session's space; index_room indices may be read from it) for
 * instance_count instances; the index type is set before.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_index_buffer: the buffer's size in indices;
 * radv_cs_emit_draw_indexed_packet: the most indices, the address, the
 * count and the initiator reading memory).
 */
void
amdrdna_vk_pm4_draw_indexed(
	struct amdrdna_vk_pm4 *pm4,
	uint64_t indices,
	uint32_t index_room,
	uint32_t index_count,
	uint32_t instance_count)
{
	/* The instances, unless the processor holds the count already. */
	if (!pm4->instances_known || pm4->instances != instance_count) {
		amdrdna_vk_pm4_packet(pm4, AMDRDNA_PM4_OP_INSTANCES, 1U, false);
		amdrdna_vk_pm4_word(pm4, instance_count);
		pm4->instances = instance_count;
		pm4->instances_known = true;
		if (pm4->full)
			pm4->instances_known = false;
	}

	/* How many indices the buffer holds from the address, unless the processor holds that already. */
	if (!pm4->index_room_known || pm4->index_room != index_room) {
		amdrdna_vk_pm4_packet(pm4, AMDRDNA_PM4_OP_INDEX_BUFFER_SIZE, 1U, false);
		amdrdna_vk_pm4_word(pm4, index_room);
		pm4->index_room = index_room;
		pm4->index_room_known = true;
		if (pm4->full)
			pm4->index_room_known = false;
	}

	/* The indices, read from memory. */
	amdrdna_vk_pm4_packet(pm4, AMDRDNA_PM4_OP_DRAW_INDEXED, 5U, false);
	amdrdna_vk_pm4_word(pm4, index_room);
	amdrdna_vk_pm4_word(pm4, (uint32_t)indices);
	amdrdna_vk_pm4_word(pm4, (uint32_t)(indices >> 32));
	amdrdna_vk_pm4_word(pm4, index_count);
	amdrdna_vk_pm4_word(pm4, AMDRDNA_PM4_DRAW_FROM_MEMORY);
}

/*
 * Dispatches x by y by z workgroups of the compute program set before, in
 * wave32.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_cmd_buffer.c
 * (radv_emit_dispatch_packets: the direct dispatch with the compute bit).
 */
void
amdrdna_vk_pm4_dispatch(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t x,
	uint32_t y,
	uint32_t z)
{
	bool room;

	/* The driver's packet: the counts and the start word. */
	room = pm4_room(pm4, AMDRDNA_PM4_DISPATCH_WORDS);
	if (!room)
		return;
	pm4->count += amdrdna_pm4_dispatch(pm4->words + pm4->count, x, y, z, AMDRDNA_SH_DISPATCH_START);
}

/* Pads the buffer with one-word packets that do nothing to a multiple of words. */
void
amdrdna_vk_pm4_pad(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t multiple)
{
	bool room;

	/* One word at a time until the count is a multiple. */
	while (!pm4->full && (pm4->count % multiple) != 0U) {
		room = pm4_room(pm4, 1U);
		if (!room)
			return;
		pm4->words[pm4->count] = amdrdna_pm4_filler();
		pm4->count++;
	}
}

/* Reports whether words more fit; a buffer they do not fit is marked full. */
static bool
pm4_room(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t words)
{
	/* A full buffer takes nothing more. */
	if (pm4->full)
		return false;

	/* The words past the end make it full. */
	if (words > pm4->capacity - pm4->count) {
		pm4->full = true;
		return false;
	}

	/* Succeeded: they fit. */
	return true;
}

/* Writes the driver's write of count consecutive registers of a space from reg (a byte offset) on. */
static void
pm4_set_run(
	struct amdrdna_vk_pm4 *pm4,
	enum amdrdna_register_space space,
	uint32_t reg,
	const uint32_t *values,
	uint32_t count)
{
	bool room;

	/* The header, the place and each value. */
	room = pm4_room(pm4, 2U + count);
	if (!room)
		return;
	pm4->count += amdrdna_pm4_registers(pm4->words + pm4->count, space, reg >> 2, values, count, false);
}

/* Forgets what every context register holds. */
static void
pm4_context_forget(
	struct amdrdna_vk_pm4 *pm4)
{
	/* No register is known, nor the draw packets' counts. */
	kern_memset(pm4->context_known, 0, sizeof(pm4->context_known));
	kern_memset(pm4->user_known, 0, sizeof(pm4->user_known));
	pm4->instances_known = false;
	pm4->index_room_known = false;
}

/*
 * Notes the values of count context registers from reg written to the
 * buffer; a run the full buffer dropped leaves them unknown.
 */
static void
pm4_context_note(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t reg,
	const uint32_t *values,
	uint32_t count)
{
	uint32_t first;
	uint32_t index;
	uint32_t place;
	uint32_t bit;

	/* Each register of the run inside the context's space. */
	first = (reg >> 2) - AMDRDNA_PM4_CONTEXT_FIRST;
	for (index = 0U; index < count; index++) {
		place = first + index;
		if (place >= AMDRDNA_VK_CONTEXT_REGISTERS)
			break;
		bit = 1U << (place % 32U);

		/* Known when written, unknown when dropped. */
		if (pm4->full) {
			pm4->context_known[place / 32U] &= ~bit;
		} else {
			pm4->context_values[place] = values[index];
			pm4->context_known[place / 32U] |= bit;
		}
	}
}

/* Reports whether the buffer set a tracked user configuration register to value already. */
static bool
pm4_user_holds(
	const struct amdrdna_vk_pm4 *pm4,
	uint32_t reg,
	uint32_t value)
{
	uint32_t index;
	uint32_t bit;

	/* Only a tracked register, known, with the value. */
	index = (reg >> 2) - AMDRDNA_PM4_USER_FIRST;
	if (index >= AMDRDNA_VK_USER_REGISTERS)
		return false;
	bit = 1U << (index % 32U);
	if ((pm4->user_known[index / 32U] & bit) == 0U)
		return false;
	if (pm4->user_values[index] != value)
		return false;

	/* Succeeded: the register holds the value. */
	return true;
}

/* Notes the value of a user configuration register written to the buffer; one the full buffer dropped is unknown. */
static void
pm4_user_note(
	struct amdrdna_vk_pm4 *pm4,
	uint32_t reg,
	uint32_t value)
{
	uint32_t index;
	uint32_t bit;

	/* Only a tracked register. */
	index = (reg >> 2) - AMDRDNA_PM4_USER_FIRST;
	if (index >= AMDRDNA_VK_USER_REGISTERS)
		return;
	bit = 1U << (index % 32U);

	/* Known when written, unknown when dropped. */
	if (pm4->full) {
		pm4->user_known[index / 32U] &= ~bit;
	} else {
		pm4->user_values[index] = value;
		pm4->user_known[index / 32U] |= bit;
	}
}
