/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The machine's physical RAM, gathered page by page.
 *
 * The ROM maps the RAM banks one after another from logical 0; the
 * loader translates each page of that range and adds its physical
 * address here.  A page next to a run, after or before it, extends it;
 * the runs are sorted and merged at the end, when the walk may have met
 * the banks out of order.
 */

#include "bootloader/mac68k/stage2.h"

static void sort_banks(struct mac68k_memory *memory);
static void merge_banks(struct mac68k_memory *memory);

/*
 * Starts an empty record of the RAM.
 */
void
mac68k_memory_init(
	struct mac68k_memory *memory)
{
	/* No run and no dropped page yet. */
	mac68k_fill_bytes(memory, 0, sizeof(*memory));
}

/*
 * Adds one page of RAM, at its physical address.
 */
void
mac68k_memory_add_page(
	struct mac68k_memory *memory,
	uint32_t physical)
{
	struct mac68k_bank *bank;
	unsigned index;

	/* Extends a run the page continues, or one the page comes just before. */
	for (index = 0; index < memory->count; index++) {
		bank = &memory->banks[index];
		if (bank->base + bank->size == physical) {
			bank->size += MAC68K_PAGE_SIZE;
			return;
		}
		if (physical + MAC68K_PAGE_SIZE == bank->base) {
			bank->base = physical;
			bank->size += MAC68K_PAGE_SIZE;
			return;
		}
	}

	/* Counts a page the record has no room left for. */
	if (memory->count >= MAC68K_RUNS_MAX) {
		memory->dropped_pages++;
		return;
	}

	/* Starts a run with the page. */
	bank = &memory->banks[memory->count];
	bank->base = physical;
	bank->size = MAC68K_PAGE_SIZE;
	memory->count++;
}

/*
 * Sorts the runs by address and merges those that touch.
 */
void
mac68k_memory_finish(
	struct mac68k_memory *memory)
{
	/* Orders the runs, then joins neighbours. */
	sort_banks(memory);
	merge_banks(memory);
}

/*
 * Reports whether a physical range lies entirely inside one run of RAM.
 */
int
mac68k_memory_holds(
	const struct mac68k_memory *memory,
	uint32_t base,
	uint32_t size)
{
	const struct mac68k_bank *bank;
	unsigned index;

	/* Looks for the run that holds the whole range. */
	for (index = 0; index < memory->count; index++) {
		bank = &memory->banks[index];
		if (base < bank->base)
			continue;
		if (base - bank->base >= bank->size)
			continue;
		if (bank->size - (base - bank->base) < size)
			continue;

		/* Succeeded: this run holds it. */
		return 1;
	}

	/* No run holds it. */
	return 0;
}

/* Sorts the runs by their base, by insertion: there are few. */
static void
sort_banks(
	struct mac68k_memory *memory)
{
	struct mac68k_bank moving;
	unsigned index;
	unsigned place;

	/* Inserts each run among the sorted ones before it. */
	for (index = 1; index < memory->count; index++) {
		moving = memory->banks[index];
		place = index;
		while (place > 0U && memory->banks[place - 1U].base > moving.base) {
			memory->banks[place] = memory->banks[place - 1U];
			place--;
		}
		memory->banks[place] = moving;
	}
}

/* Joins each run with the next when the next starts where it ends. */
static void
merge_banks(
	struct mac68k_memory *memory)
{
	struct mac68k_bank *kept;
	unsigned index;
	unsigned count;

	/* Nothing to merge in an empty record. */
	if (memory->count == 0U)
		return;

	/* Folds every following run into the last kept one when they touch. */
	count = 1;
	for (index = 1; index < memory->count; index++) {
		kept = &memory->banks[count - 1U];
		if (kept->base + kept->size == memory->banks[index].base) {
			kept->size += memory->banks[index].size;
			continue;
		}

		/* Keeps a run that does not touch the one before. */
		memory->banks[count] = memory->banks[index];
		count++;
	}
	memory->count = count;
}
