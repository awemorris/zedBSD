/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A check, on the running firmware, of whether the processor's cached
 * view of memory sees what the core's firmware processor writes.
 *
 * The driver shares memory with the core as if the core did not snoop
 * the processor's caches; this check only reports what it sees and
 * changes nothing.  It names the path it tries: stores of the firmware's
 * MIPS processor, through its page table, with the page uncached on the
 * MIPS side, and nothing done about the core's system level cache.  What
 * the core's data masters do is another path.
 *
 * The procedure: a control first shows that the processor alone can be
 * shown a stale line (clean lines loaded through the cached view, other
 * values written through the uncached alias, and the old values read
 * cached again); without it a matching round would prove nothing.  Each
 * round then loads its window's lines clean before the firmware writes,
 * so a write that does not reach the processor's caches reads back
 * stale.  The eight windows of 64 KiB lie 128 KiB apart across 2 MiB,
 * so they pass through different sets of the level-two cache, and each
 * is small enough that no line is evicted and written back to fake a
 * pass.  The verdict comes from the cached view only; the alias only
 * tells whether a write reached memory at all.  The whole range is
 * dropped from the caches afterwards, because the control and a failing
 * round leave stale clean lines that the memory's next owner would read.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/dcache.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/powervr-private.h"
#include "drivers/gpu/powervr/interface.h"
#include "drivers/gpu/powervr/mips-table.h"

/*
 * The run the check uses, the rounds it makes, how far apart they are,
 * and how many bytes each covers.
 */
#define CHECK_BYTES		0x200000U
#define CHECK_ROUNDS		8U
#define CHECK_STRIDE		0x20000U
#define CHECK_WINDOW		0x10000U
#define CHECK_WORDS		(CHECK_WINDOW / 4U)

/* What the control found: whether the processor alone can be shown a stale line. */
enum control_result {
	CONTROL_OK = 0,
	CONTROL_INCONCLUSIVE
};

/*
 * The memory the check runs on: the run, its uncached alias, its cached
 * view, and where the firmware's processor sees it.
 */
struct check_memory {
	struct kern_pmem run;
	volatile uint32_t *alias;
	const volatile uint32_t *cached;
	uint32_t heap_offset;
	int mapped;
};

/* What the rounds found, word by word. */
struct check_counts {
	unsigned rounds_matched;
	uint64_t stale;
	uint64_t unwritten;
	int error;
};

static int take_memory(struct powervr_core *core, struct check_memory *memory);
static void give_memory(struct powervr_core *core, struct check_memory *memory);
static enum control_result run_control(struct check_memory *memory);
static void run_round(struct powervr_core *core, struct check_memory *memory, unsigned round, struct check_counts *counts);
static int write_words(struct powervr_core *core, struct check_memory *memory, uint32_t first, unsigned round);
static uint32_t pattern(unsigned round, uint32_t word);
static uint32_t before(unsigned round, uint32_t word);
static void load_cached(const volatile uint32_t *words, uint32_t count);

/*
 * Checks whether the processor's cached view sees the firmware's
 * processor's writes, and reports the outcome on one line.
 */
void
powervr_coherency_check(
	struct powervr_core *core)
{
	struct check_memory memory;
	struct check_counts counts;
	enum control_result control;
	const char *verdict;
	const char *control_word;
	unsigned round;
	int error;

	/* Takes the run, its views and its place in the firmware's heap. */
	error = take_memory(core, &memory);
	if (error != 0) {
		kern_logf("powervr: coherency (firmware writes): not run (%d)\n", error);
		return;
	}

	/* The control: the processor alone can be shown a stale line. */
	control = run_control(&memory);

	/* Runs the rounds, each 64 KiB further into the run. */
	counts.rounds_matched = 0;
	counts.stale = 0;
	counts.unwritten = 0;
	counts.error = 0;
	for (round = 0; round < CHECK_ROUNDS; round++) {
		run_round(core, &memory, round, &counts);
		if (counts.error != 0)
			break;
	}

	/* Drops the run from the caches and returns it. */
	give_memory(core, &memory);

	/* A round the firmware did not finish says nothing. */
	if (counts.error != 0) {
		kern_logf("powervr: coherency (firmware writes): not finished (%d)\n", counts.error);
		return;
	}

	/* Names the outcome: writes that never reached memory say nothing about the caches. */
	if (counts.unwritten != 0U) {
		verdict = "INCONCLUSIVE, writes missing";
	} else if (counts.rounds_matched == CHECK_ROUNDS && control == CONTROL_OK) {
		verdict = "COHERENT";
	} else if (counts.rounds_matched == CHECK_ROUNDS) {
		verdict = "MATCHED WITHOUT CONTROL";
	} else {
		verdict = "NOT COHERENT";
	}

	/* The control's word. */
	if (control == CONTROL_OK) {
		control_word = "OK";
	} else {
		control_word = "INCONCLUSIVE";
	}

	/* Reports the outcome on one line; the sharing stays as it is whatever it says. */
	kern_logf("powervr: coherency (firmware writes): %s, control %s, %u/%u rounds, %llu stale, %llu missing\n",
		  verdict,
		  control_word,
		  counts.rounds_matched,
		  (unsigned)CHECK_ROUNDS,
		  (unsigned long long)counts.stale,
		  (unsigned long long)counts.unwritten);
}

/* Takes the run, its uncached alias, its cached view, and maps it for the firmware. */
static int
take_memory(
	struct powervr_core *core,
	struct check_memory *memory)
{
	struct powervr_heap *heap;
	uint32_t first;
	void *view;
	int error;

	/* Takes the run where the core can name every byte. */
	heap = &core->firmware.heap;
	memory->mapped = 0;
	error = kern_pmem_alloc_limited(CHECK_BYTES, POWERVR_MIPS_PAGE_BYTES, heap->dma_limit, 0, &memory->run);
	if (error != 0)
		return error;

	/* Makes the uncached alias; the run's lines are pushed out first. */
	error = kern_pmem_map_uncached(&memory->run, &view);
	if (error != 0) {
		(void)kern_pmem_free(&memory->run);
		return error;
	}
	memory->alias = view;
	memory->cached = kern_pmem_to_kernel(memory->run.paddr);

	/* Holds a place in the firmware's heap and maps the run there, uncached on the MIPS side. */
	mutex_lock(&core->lock);

	error = powervr_heap_pages_take(heap->pages, CHECK_BYTES / POWERVR_MIPS_PAGE_BYTES, &first);
	if (error == 0) {
		memory->heap_offset = first * POWERVR_MIPS_PAGE_BYTES;
		error = powervr_mips_map((volatile uint32_t *)heap->table.view, memory->heap_offset, memory->run.paddr, CHECK_BYTES, POWERVR_MIPS_UNCACHED, heap->bus_bits);
		if (error != 0)
			powervr_heap_pages_give(heap->pages, first, CHECK_BYTES / POWERVR_MIPS_PAGE_BYTES);
	}

	mutex_unlock(&core->lock);

	/* Without a place the check cannot run. */
	if (error != 0) {
		(void)kern_pmem_unmap_uncached((void *)memory->alias, CHECK_BYTES);
		(void)kern_pmem_free(&memory->run);
		return error;
	}

	/* Succeeded: the run is reachable three ways. */
	memory->mapped = 1;
	return 0;
}

/* Unmaps the run from the firmware, drops its lines and returns it. */
static void
give_memory(
	struct powervr_core *core,
	struct check_memory *memory)
{
	struct powervr_heap *heap;

	/* Removes the firmware's mapping and frees the place. */
	heap = &core->firmware.heap;
	mutex_lock(&core->lock);

	powervr_mips_unmap((volatile uint32_t *)heap->table.view, memory->heap_offset, CHECK_BYTES);
	powervr_heap_pages_give(heap->pages, memory->heap_offset / POWERVR_MIPS_PAGE_BYTES, CHECK_BYTES / POWERVR_MIPS_PAGE_BYTES);

	mutex_unlock(&core->lock);

	/* Drops every line the cached reads loaded, so nothing stale outlives the check. */
	kern_dcache_invalidate_range((const void *)memory->cached, CHECK_BYTES);

	/* Ends the alias and returns the run. */
	(void)kern_pmem_unmap_uncached((void *)memory->alias, CHECK_BYTES);
	(void)kern_pmem_free(&memory->run);
	memory->mapped = 0;
}

/*
 * The control: loads clean lines through the cached view, writes other
 * values through the alias, and reads the cached view again.  Seeing the
 * old values shows the processor alone can be shown a stale line, so a
 * matching round later means something.
 */
static enum control_result
run_control(
	struct check_memory *memory)
{
	const volatile uint32_t *cached;
	volatile uint32_t *alias;
	uint32_t word;
	uint32_t stale;

	/* Fills the first window through the alias and drops any line of it. */
	alias = memory->alias;
	cached = memory->cached;
	for (word = 0; word < CHECK_WORDS; word++)
		alias[word] = before(0, word);
	kern_dcache_invalidate_range((const void *)cached, CHECK_WINDOW);

	/* Loads the window into the cache, clean. */
	load_cached(cached, CHECK_WORDS);

	/* Writes other values through the alias, then counts the old ones the cached view still shows. */
	for (word = 0; word < CHECK_WORDS; word++)
		alias[word] = pattern(0, word);
	stale = 0;
	for (word = 0; word < CHECK_WORDS; word++) {
		if (cached[word] == before(0, word))
			stale++;
	}

	/* Drops the lines again, so the rounds start clean. */
	kern_dcache_invalidate_range((const void *)cached, CHECK_WINDOW);

	/* Every word stale: the control holds. */
	if (stale == CHECK_WORDS)
		return CONTROL_OK;

	/* The control did not hold. */
	return CONTROL_INCONCLUSIVE;
}

/*
 * One round: loads a window's clean lines, has the firmware write every
 * word of it, and counts what the cached view and the alias show.
 */
static void
run_round(
	struct powervr_core *core,
	struct check_memory *memory,
	unsigned round,
	struct check_counts *counts)
{
	const volatile uint32_t *cached;
	volatile uint32_t *alias;
	uint32_t first;
	uint32_t word;
	uint32_t matched;
	uint32_t value;
	int error;

	/* The window of this round. */
	first = round * (CHECK_STRIDE / 4U);
	alias = memory->alias + first;
	cached = memory->cached + first;

	/* Fills it through the alias with the values from before, and drops any line of it. */
	for (word = 0; word < CHECK_WORDS; word++)
		alias[word] = before(round, word);
	kern_dcache_invalidate_range((const void *)cached, CHECK_WINDOW);

	/* Loads its lines into the cache, clean. */
	load_cached(cached, CHECK_WORDS);

	/* Has the firmware write every word; no cache of the core is flushed. */
	error = write_words(core, memory, first, round);
	if (error != 0) {
		counts->error = error;
		return;
	}

	/* Counts the words the cached view shows written, and those it shows stale. */
	matched = 0;
	for (word = 0; word < CHECK_WORDS; word++) {
		value = cached[word];
		if (value == pattern(round, word)) {
			matched++;
		} else if (value == before(round, word)) {
			counts->stale++;
		}
	}

	/* Counts the words that never reached memory, through the alias. */
	for (word = 0; word < CHECK_WORDS; word++) {
		if (alias[word] != pattern(round, word))
			counts->unwritten++;
	}

	/* The round matched when the cached view showed every word written. */
	if (matched == CHECK_WORDS)
		counts->rounds_matched++;
}

/*
 * Has the firmware's processor write each word of a window: one MMU
 * cache command per word, whose sync word is that word and whose value
 * is the word's pattern.  Commands go in batches as long as the kernel's
 * buffer takes them; each batch is waited on through its last slot.
 */
static int
write_words(
	struct powervr_core *core,
	struct check_memory *memory,
	uint32_t first,
	unsigned round)
{
	uint32_t command[POWERVR_COMMAND_WORDS];
	uint32_t index;
	uint32_t word;
	uint32_t slot;
	uint32_t answer;
	unsigned pending;
	int error;

	/* The parts of the command that stay the same: its type and the caches it names. */
	for (index = 0; index < POWERVR_COMMAND_WORDS; index++)
		command[index] = 0;
	command[POWERVR_COMMAND_TYPE / 4U] = POWERVR_COMMAND_MMU_CACHE;
	command[(POWERVR_COMMAND_DATA + POWERVR_MMU_CACHE_FLAGS) / 4U] = POWERVR_MMU_CACHE_ALL;

	/* Sends one command per word, waiting on the batch whenever the buffer is full. */
	mutex_lock(&core->lock);

	error = 0;
	pending = 0;
	slot = 0;
	word = 0;
	while (word < CHECK_WORDS) {
		command[(POWERVR_COMMAND_DATA + POWERVR_MMU_CACHE_SYNC) / 4U] = powervr_mips_address(memory->heap_offset + (first + word) * 4U);
		command[(POWERVR_COMMAND_DATA + POWERVR_MMU_CACHE_VALUE) / 4U] = pattern(round, word);
		error = powervr_command_send(core, command, &slot);
		if (error == 0) {
			pending++;
			word++;
			continue;
		}

		/* A full buffer with nothing of ours in it cannot drain by waiting. */
		if (error != EBUSY || pending == 0U)
			break;

		/* Waits for the batch, through its last command. */
		error = powervr_command_wait(core, slot, POWERVR_COMMAND_WAIT_MS, &answer);
		if (error != 0)
			break;
		pending = 0;
	}

	/* Waits for the last batch. */
	if (error == 0 && pending != 0U)
		error = powervr_command_wait(core, slot, POWERVR_COMMAND_WAIT_MS, &answer);

	mutex_unlock(&core->lock);

	/* The firmware did not execute every command. */
	if (error != 0)
		return error;

	/* Succeeded: the firmware executed a command for every word. */
	return 0;
}

/* The value the firmware writes to a word of a round. */
static uint32_t
pattern(
	unsigned round,
	uint32_t word)
{
	/* Differs per round and per word, and never equals the value from before. */
	return 0x5a000000U ^ ((uint32_t)round << 20) ^ word;
}

/* The value a word holds before the firmware writes it. */
static uint32_t
before(
	unsigned round,
	uint32_t word)
{
	/* The pattern's complement. */
	return ~pattern(round, word);
}

/* Reads every word through the cached view, so each line is in the cache. */
static void
load_cached(
	const volatile uint32_t *words,
	uint32_t count)
{
	uint32_t index;
	uint32_t sum;

	/* The volatile loads cannot be dropped; the sum only keeps them in order. */
	sum = 0;
	for (index = 0; index < count; index++)
		sum += words[index];
	(void)sum;
}
