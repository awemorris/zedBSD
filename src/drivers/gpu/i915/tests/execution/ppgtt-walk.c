/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A walk of the GT address space, as the GPU would do it.
 *
 * The tests read the page tables they are about to submit and check that each
 * fixture address resolves to the page the CPU wrote, with the attributes the
 * reference driver gives it.  The walk only reads the tables.
 */

#include "eu-test.h"
#include <kern/kcrt.h>

#include "../../memory.h"
#include "../../ppgtt.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/* The address bits of a page-table entry. */
#define I915_TEST_WALK_ADDRESS_MASK	0x0000fffffffff000ULL

static struct i915_gt_ppgtt_table *i915_walk_table_by_dma(struct i915_gt_ppgtt *pp, uint64_t dma);
static int i915_walk_is_scratch(const struct i915_gt_ppgtt *pp, int lvl, uint64_t raw);
static void i915_walk_read_leaf(struct i915_test_ppgtt_walk *walk, uint64_t raw);

/*
 * Walks the GT address space for one address.
 *
 * Reads each level's entry the way the walker would read it, top level first,
 * after flushing the CPU cache line that holds it.  The walk ends at the page
 * entry, or at the first directory entry that names no table of this space
 * (scratch or foreign).  Returns 0, or EINVAL for a space that was never
 * created.
 */
int
drv_i915_test_ppgtt_walk(
	struct i915_gt_ppgtt *pp,
	uint64_t va,
	struct i915_test_ppgtt_walk *walk)
{
	struct i915_gt_ppgtt_table *child;
	struct i915_gt_object *table;
	volatile uint64_t *entries;
	uint64_t page_index;
	uint64_t raw;
	unsigned index;
	int level;
	int lvl;

	/* Refuses a space that was never created. */
	if (pp == NULL || walk == NULL)
		return EINVAL;
	if (!pp->inited)
		return EINVAL;

	/* Starts an empty record at the top directory, which PDP0 of a context names. */
	kern_memset(walk, 0, sizeof(*walk));
	walk->va = va;
	walk->top_dma = pp->top_pd_dma;
	page_index = va >> 12;
	table = pp->top_pd;

	/* Reads one entry per level, from the top directory down to the page entry. */
	level = 0;
	for (lvl = pp->top; lvl >= 0; lvl--) {
		/* Reads the entry the walker would read: the table as submitted. */
		index = (unsigned)((page_index >> ((unsigned)lvl * 9U)) & 511U);
		entries = (volatile uint64_t *)table->cpu;
		drv_i915_gt_clflush(&entries[index], sizeof(uint64_t));
		raw = entries[index];
		walk->idx[level] = index;
		walk->raw[level] = raw;
		walk->levels = level + 1;
		walk->scratch[level] = i915_walk_is_scratch(pp, lvl, raw);

		/* The page entry ends the walk with what it maps. */
		if (lvl == 0) {
			i915_walk_read_leaf(walk, raw);
			break;
		}

		/* A directory entry that names none of this space's tables ends the walk. */
		walk->child_dma[level] = raw & I915_TEST_WALK_ADDRESS_MASK;
		child = i915_walk_table_by_dma(pp, walk->child_dma[level]);
		if (child == NULL)
			break;

		walk->child_known[level] = 1;
		table = child->obj;
		level++;
	}

	/* Succeeded: the record says what the GPU would find. */
	return 0;
}

/* Finds the table of this space that sits at a DMA address. */
static struct i915_gt_ppgtt_table *
i915_walk_table_by_dma(
	struct i915_gt_ppgtt *pp,
	uint64_t dma)
{
	unsigned i;

	/* Looks through the tables allocated below the top directory. */
	for (i = 0U; i < pp->n_tables; i++) {
		if (pp->tables[i].dma == dma)
			return &pp->tables[i];
	}

	/* The address is scratch or belongs to no table of this space. */
	return NULL;
}

/* Tells whether an entry of a level is that level's scratch encoding. */
static int
i915_walk_is_scratch(
	const struct i915_gt_ppgtt *pp,
	int lvl,
	uint64_t raw)
{
	/* A directory entry is scratch when it names the scratch table of the level below. */
	if (lvl > 0) {
		if (raw == pp->scratch_encode[lvl])
			return 1;
		return 0;
	}

	/* A page entry is scratch when it names the scratch data page. */
	if (raw == pp->scratch_encode[0])
		return 1;

	return 0;
}

/* Decodes the page entry: the page, presence, writability and the PAT index. */
static void
i915_walk_read_leaf(
	struct i915_test_ppgtt_walk *walk,
	uint64_t raw)
{
	unsigned pat;

	/* The page the entry maps. */
	walk->leaf_dma = raw & I915_TEST_WALK_ADDRESS_MASK;

	/* Whether the entry is present and writable. */
	walk->leaf_present = 0;
	if ((raw & I915_GEN8_PAGE_PRESENT_B) != 0U)
		walk->leaf_present = 1;
	walk->leaf_rw = 0;
	if ((raw & I915_GEN8_PAGE_RW_B) != 0U)
		walk->leaf_rw = 1;

	/* The PAT index from its three scattered bits. */
	pat = 0U;
	if ((raw & I915_GEN12_PTE_PAT0) != 0U)
		pat |= 1U;
	if ((raw & I915_GEN12_PTE_PAT1) != 0U)
		pat |= 2U;
	if ((raw & I915_GEN12_PTE_PAT2) != 0U)
		pat |= 4U;
	walk->leaf_pat = pat;
}
