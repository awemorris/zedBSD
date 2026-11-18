/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host check of the Mali command stream encoders
 * (src/drivers/gpu/mali/cs/cs-build.c): every instruction against words
 * packed by hand from the field places of Mesa 26.2.6
 * src/panfrost/genxml/v10.xml, the ring's call sequence against the
 * words a CSF driver writes for one job, and the copy and fill loops run
 * by a small interpreter of the front end over host memory.
 *
 * cc -std=c11 -Wall -Wextra -Werror -Isrc -Iinclude \
 *    tests/mali-cs-host-test.c src/drivers/gpu/mali/cs/cs-build.c -o <out> && <out>
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/gpu/mali/cs/cs-build.h"

/* The interpreter's memory: a window of GPU addresses from MEMORY_BASE. */
#define MEMORY_BASE	0x1000000ULL
#define MEMORY_BYTES	0x10000U

/* How many checks failed and ran. */
static int failures;
static int checks;

/* The interpreter's registers and memory. */
static uint32_t work[128];
static uint8_t memory[MEMORY_BYTES];

static void expect(const char *name, uint64_t got, uint64_t want);
static uint64_t pair(unsigned index);
static void set_pair(unsigned index, uint64_t value);
static int run(const uint64_t *words, uint32_t count);
static void check_encoders(void);
static void check_ring_sequence(void);
static void check_loops(void);

/* Runs every check and reports the count. */
int
main(void)
{
	check_encoders();
	check_ring_sequence();
	check_loops();
	if (failures != 0) {
		printf("%d of %d checks failed\n", failures, checks);
		return 1;
	}
	printf("all checks passed (%d)\n", checks);
	return 0;
}

/* Compares one word and records a failure. */
static void
expect(
	const char *name,
	uint64_t got,
	uint64_t want)
{
	checks++;
	if (got != want) {
		printf("FAIL %s: got %016llx want %016llx\n", name, (unsigned long long)got, (unsigned long long)want);
		failures++;
	}
}

/* Reads a register pair of the interpreter. */
static uint64_t
pair(
	unsigned index)
{
	return (uint64_t)work[index] | ((uint64_t)work[index + 1U] << 32);
}

/* Writes a register pair of the interpreter. */
static void
set_pair(
	unsigned index,
	uint64_t value)
{
	work[index] = (uint32_t)value;
	work[index + 1U] = (uint32_t)(value >> 32);
}

/*
 * Runs the instructions the loops use (MOVE48, MOVE32, LOAD, STORE,
 * WAIT, ADD32, ADD64, BRANCH) over the window; a nonzero return names a
 * fault.
 */
static int
run(
	const uint64_t *words,
	uint32_t count)
{
	uint32_t at;
	uint32_t steps;
	uint64_t word;
	unsigned opcode;
	unsigned destination;
	unsigned source;
	unsigned index;
	uint32_t mask;
	int16_t offset;
	uint64_t address;

	at = 0;
	steps = 0;
	while (at < count) {
		word = words[at];
		opcode = (unsigned)(word >> 56);
		destination = (unsigned)(word >> 48) & 0xffU;
		source = (unsigned)(word >> 40) & 0xffU;
		at++;
		steps++;
		if (steps > 1000000U)
			return 1;
		switch (opcode) {
		case MALI_CS_OP_MOVE48:
			set_pair(destination, word & 0xffffffffffffULL);
			break;
		case MALI_CS_OP_MOVE32:
			work[destination] = (uint32_t)word;
			break;
		case MALI_CS_OP_WAIT:
			break;
		case MALI_CS_OP_ADD32:
			work[destination] = work[source] + (uint32_t)word;
			break;
		case MALI_CS_OP_ADD64:
			set_pair(destination, pair(source) + (uint64_t)(int64_t)(int32_t)(uint32_t)word);
			break;
		case MALI_CS_OP_LOAD:
		case MALI_CS_OP_STORE:
			mask = (uint32_t)(word >> 16) & 0xffffU;
			offset = (int16_t)(uint16_t)word;
			address = pair(source) + (uint64_t)(int64_t)offset;
			for (index = 0; index < 16U; index++) {
				if ((mask & (1U << index)) == 0U)
					continue;
				if (address < MEMORY_BASE || address + index * 4U + 4U > MEMORY_BASE + MEMORY_BYTES)
					return 2;
				if (opcode == MALI_CS_OP_LOAD)
					memcpy(&work[destination + index], &memory[address - MEMORY_BASE + index * 4U], 4);
				else
					memcpy(&memory[address - MEMORY_BASE + index * 4U], &work[destination + index], 4);
			}
			break;
		case MALI_CS_OP_BRANCH:
			offset = (int16_t)(uint16_t)word;
			if (((word >> 28) & 0xfU) == MALI_CS_NEQUAL && work[source] != 0U)
				at = (uint32_t)((int32_t)at + offset);
			break;
		default:
			return 3;
		}
	}
	return 0;
}

/* Checks each encoder against words packed by hand from the field places. */
static void
check_encoders(void)
{
	expect("nop", mali_cs_nop(), 0);
	expect("move48", mali_cs_move48(0x5c, 0x123456789abcULL), (1ULL << 56) | (0x5cULL << 48) | 0x123456789abcULL);
	expect("move32", mali_cs_move32(0x5e, 0xdeadbeefU), (2ULL << 56) | (0x5eULL << 48) | 0xdeadbeefULL);
	expect("wait", mali_cs_wait(0x00ff), (3ULL << 56) | (0xffULL << 16));
	expect("run_compute", mali_cs_run_compute(1, MALI_CS_AXIS_Z, 1, 2, 3, 1),
	       (4ULL << 56) | 1ULL | (2ULL << 14) | (1ULL << 40) | (2ULL << 42) | (3ULL << 44) | (1ULL << 46));
	expect("run_idvs", mali_cs_run_idvs(0x12, true, true, false, true, true, false),
	       (6ULL << 56) | 0x12ULL | (1ULL << 33) | (1ULL << 35) | (1ULL << 37) | (1ULL << 38));
	expect("run_fragment", mali_cs_run_fragment(false, 0), 7ULL << 56);
	expect("finish_tiling", mali_cs_finish_tiling(), 9ULL << 56);
	expect("finish_fragment", mali_cs_finish_fragment(true, 0x6, 0x44, 0x42, 2),
	       (11ULL << 56) | 1ULL | (0x6ULL << 16) | (0x44ULL << 32) | (0x42ULL << 40) | (2ULL << 48));
	expect("add32", mali_cs_add32(4, 5, -1), (16ULL << 56) | (4ULL << 48) | (5ULL << 40) | 0xffffffffULL);
	expect("add64", mali_cs_add64(6, 6, 64), (17ULL << 56) | (6ULL << 48) | (6ULL << 40) | 64ULL);
	expect("load", mali_cs_load(0x10, 0x20, 0x3, -8),
	       (20ULL << 56) | (0x10ULL << 48) | (0x20ULL << 40) | (0x3ULL << 16) | 0xfff8ULL);
	expect("store", mali_cs_store(0x10, 0x20, 0xffff, 16),
	       (21ULL << 56) | (0x10ULL << 48) | (0x20ULL << 40) | (0xffffULL << 16) | 16ULL);
	expect("branch", mali_cs_branch(-8, MALI_CS_NEQUAL, 0x22),
	       (22ULL << 56) | 0xfff8ULL | (3ULL << 28) | (0x22ULL << 40));
	expect("set_sb", mali_cs_set_scoreboard(2, 0), (23ULL << 56) | 2ULL);
	expect("set_exception", mali_cs_set_exception(MALI_CS_EXCEPTION_TILER_OOM, 0x30, 0x32),
	       (25ULL << 56) | 2ULL | (0x30ULL << 40) | (0x32ULL << 32));
	expect("call", mali_cs_call(0x5c, 0x5e), (32ULL << 56) | (0x5cULL << 40) | (0x5eULL << 32));
	expect("jump", mali_cs_jump(0x5c, 0x5e), (33ULL << 56) | (0x5cULL << 40) | (0x5eULL << 32));
	expect("req_resource", mali_cs_request_resource(0xf), (34ULL << 56) | 0xfULL);
	expect("flush", mali_cs_flush_cache(3, 3, 2, 0, 0x5e, 0), (36ULL << 56) | (0x5eULL << 40) | 0x233ULL);
	expect("sync_add32", mali_cs_sync_add32(false, MALI_CS_SCOPE_GROUP, 0x3, 0x5e, 0x5c, 1),
	       (37ULL << 56) | (2ULL << 1) | (0x3ULL << 16) | (0x5eULL << 32) | (0x5cULL << 40) | (1ULL << 48));
	expect("sync_set32", mali_cs_sync_set32(true, 0, 0, 1, 2, 0), (38ULL << 56) | 1ULL | (1ULL << 32) | (2ULL << 40));
	expect("sync_wait32", mali_cs_sync_wait32(true, MALI_CS_GREATER, 4, 6),
	       (39ULL << 56) | 1ULL | (1ULL << 28) | (4ULL << 32) | (6ULL << 40));
	expect("store_state", mali_cs_store_state(8, 0x1, MALI_CS_STATE_TIMESTAMP, 0x20, 0),
	       (40ULL << 56) | 8ULL | (1ULL << 16) | (0x20ULL << 40));
	expect("error_barrier", mali_cs_error_barrier(), 47ULL << 56);
	expect("heap_set", mali_cs_heap_set(0x58), (48ULL << 56) | (0x58ULL << 40));
	expect("heap_op", mali_cs_heap_operation(0x2, MALI_CS_HEAP_FRAGMENT_COMPLETED, 0),
	       (49ULL << 56) | (2ULL << 16) | (3ULL << 32));
	expect("sync_add64", mali_cs_sync_add64(true, MALI_CS_SCOPE_SYSTEM, 0, 0x5e, 0x5c, 0),
	       (51ULL << 56) | 1ULL | (0x5eULL << 32) | (0x5cULL << 40));
	expect("sync_set64", mali_cs_sync_set64(false, 0, 0, 2, 4, 0), (52ULL << 56) | (2ULL << 32) | (4ULL << 40));
	expect("sync_wait64", mali_cs_sync_wait64(false, MALI_CS_LEQUAL, 2, 4), (53ULL << 56) | (2ULL << 32) | (4ULL << 40));
	expect("opcode", mali_cs_opcode(mali_cs_call(1, 2)), MALI_CS_OP_CALL);
}

/*
 * Checks the job sequence a ring takes against the words a CSF driver
 * writes: flush, call, then a 64-bit sync add after waiting for every
 * entry, and an error barrier.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_sched.c (queue_run_job: the
 * call instruction words, with the address pair and the value register
 * of the stream's unpreserved registers).
 */
static void
check_ring_sequence(void)
{
	uint64_t words[16];
	struct mali_cs cs;
	unsigned address;
	unsigned value;

	address = 92;
	value = 94;
	mali_cs_init(&cs, words, 16);
	mali_cs_emit(&cs, mali_cs_flush_cache(MALI_CS_FLUSH_CLEAN_INVALIDATE, MALI_CS_FLUSH_CLEAN_INVALIDATE, MALI_CS_FLUSH_OTHER_INVALIDATE, 0, value, 0));
	mali_cs_emit(&cs, mali_cs_move48(address, 0x1000000ULL));
	mali_cs_emit(&cs, mali_cs_move32(value, 0x200));
	mali_cs_emit(&cs, mali_cs_wait(1));
	mali_cs_emit(&cs, mali_cs_call(address, value));
	mali_cs_emit(&cs, mali_cs_sync_add64(true, MALI_CS_SCOPE_SYSTEM, 0, value, address, 0));
	mali_cs_emit(&cs, mali_cs_error_barrier());
	expect("ring count", cs.count, 7);
	expect("ring bytes", mali_cs_bytes(&cs), 56);
	expect("ring flush", words[0], (36ULL << 56) | ((uint64_t)value << 40) | 0x233ULL);
	expect("ring move48", words[1], (1ULL << 56) | ((uint64_t)address << 48) | 0x1000000ULL);
	expect("ring move32", words[2], (2ULL << 56) | ((uint64_t)value << 48) | 0x200ULL);
	expect("ring wait", words[3], (3ULL << 56) | (1ULL << 16));
	expect("ring call", words[4], (32ULL << 56) | ((uint64_t)address << 40) | ((uint64_t)value << 32));
	expect("ring sync", words[5], (51ULL << 56) | ((uint64_t)address << 40) | ((uint64_t)value << 32) | 1ULL);
	expect("ring barrier", words[6], 47ULL << 56);

	/* An overflow latches and writes nothing more. */
	mali_cs_init(&cs, words, 1);
	mali_cs_emit(&cs, mali_cs_nop());
	mali_cs_emit(&cs, mali_cs_nop());
	expect("overflow error", (uint64_t)cs.error, 1);
	expect("overflow count", cs.count, 1);

	/* A 64-bit value past 48 bits takes two moves. */
	mali_cs_init(&cs, words, 4);
	mali_cs_move64(&cs, 10, 0x0001000200030004ULL);
	expect("move64 count", cs.count, 2);
	expect("move64 low", words[0], (2ULL << 56) | (10ULL << 48) | 0x00030004ULL);
	expect("move64 high", words[1], (2ULL << 56) | (11ULL << 48) | 0x00010002ULL);
}

/* Runs the copy and fill loops in the interpreter and checks the memory. */
static void
check_loops(void)
{
	uint64_t words[64];
	struct mali_cs cs;
	unsigned index;
	int fault;
	int same;

	/* A copy of 1 KiB from the window's start to its middle. */
	for (index = 0; index < MEMORY_BYTES; index++)
		memory[index] = (uint8_t)(index * 7U + 3U);
	mali_cs_init(&cs, words, 64);
	mali_cs_copy(&cs, 64, 80, MEMORY_BASE + 0x8000U, MEMORY_BASE, 1024, 0);
	expect("copy built", (uint64_t)cs.error, 0);
	fault = run(words, cs.count);
	expect("copy ran", (uint64_t)fault, 0);
	same = memcmp(&memory[0x8000], &memory[0], 1024) == 0;
	expect("copy bytes", (uint64_t)same, 1);
	expect("copy stopped", memory[0x8000 + 1024], (uint8_t)((0x8000U + 1024U) * 7U + 3U));

	/* A fill of 256 bytes with a pattern. */
	mali_cs_init(&cs, words, 64);
	mali_cs_fill(&cs, 64, 80, MEMORY_BASE + 0x100U, 0xa5a5a5a5U, 256, 0);
	expect("fill built", (uint64_t)cs.error, 0);
	fault = run(words, cs.count);
	expect("fill ran", (uint64_t)fault, 0);
	same = 1;
	for (index = 0x100; index < 0x200; index++) {
		if (memory[index] != 0xa5U)
			same = 0;
	}
	expect("fill bytes", (uint64_t)same, 1);
	expect("fill stopped", memory[0x200], (uint8_t)(0x200U * 7U + 3U));

	/* A copy of 3 rows of 100 bytes, strides 256 and 128. */
	for (index = 0; index < MEMORY_BYTES; index++)
		memory[index] = (uint8_t)(index * 13U + 1U);
	mali_cs_init(&cs, words, 64);
	mali_cs_copy_rows(&cs, 64, 80, MEMORY_BASE + 0x9000U, 128, MEMORY_BASE + 0x100U, 256, 100, 3, 0);
	expect("rows built", (uint64_t)cs.error, 0);
	fault = run(words, cs.count);
	expect("rows ran", (uint64_t)fault, 0);
	same = 1;
	for (index = 0; index < 3U; index++) {
		if (memcmp(&memory[0x9000U + index * 128U], &memory[0x100U + index * 256U], 100) != 0)
			same = 0;
	}
	expect("rows bytes", (uint64_t)same, 1);
	expect("rows gap kept", memory[0x9000U + 100U], (uint8_t)((0x9000U + 100U) * 13U + 1U));

	/* A fill of 2 rows of 8 bytes (only a tail), stride 64. */
	mali_cs_init(&cs, words, 64);
	mali_cs_fill_rows(&cs, 64, 80, MEMORY_BASE + 0xa000U, 64, 0x11223344U, 8, 2, 0);
	fault = run(words, cs.count);
	expect("fill rows ran", (uint64_t)fault, 0);
	expect("fill rows word", (uint64_t)memory[0xa000U + 64U + 4U], 0x44);
	expect("fill rows gap", memory[0xa000U + 8U], (uint8_t)((0xa000U + 8U) * 13U + 1U));

	/* A size that is not whole passes is refused. */
	mali_cs_init(&cs, words, 64);
	mali_cs_copy(&cs, 64, 80, MEMORY_BASE, MEMORY_BASE, 100, 0);
	expect("copy odd size", (uint64_t)cs.error, 1);
}
