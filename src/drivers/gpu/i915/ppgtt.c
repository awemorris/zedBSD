/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Per-process GTTs (see ppgtt.h).
 *
 * The kernel PPGTT comes first, then the session PPGTT.  They share no
 * code: the kernel PPGTT builds its tables from GT objects and flushes each
 * entry it writes; the session PPGTT builds its tables from page-manager
 * pages and publishes them with a write barrier.
 */

#include "i915.h"
#include "ppgtt.h"
#include "memory.h"
#include "ggtt.h"
#include <kern/kcrt.h>

#include <kern/device-io.h>
#include <kern/kmem.h>
#include <kern/pmem.h>

#include <uapi/errno.h>
#include <stddef.h>

#include "intel/gt-regs.h"

/* Session object pages: present, writable, PAT index 0 (write-back through the LLC). */
#define I915_PPGTT_PAGE_BITS		(GEN8_PAGE_PRESENT | GEN8_PAGE_RW)

/* Session tables the driver fills: present, writable, PAT index 0. */
#define I915_PPGTT_TABLE_BITS		(GEN8_PAGE_PRESENT | GEN8_PAGE_RW)

/* Session scratch tables mirror Linux's uncached page-directory encoding (PAT index 3). */
#define I915_PPGTT_SCRATCH_TABLE_BITS	(GEN8_PAGE_PRESENT | GEN8_PAGE_RW | GEN12_PPGTT_PTE_PAT0 | GEN12_PPGTT_PTE_PAT1)

static void i915_gt_fill_px(struct i915_gt_object *object, uint64_t value, unsigned count);
static unsigned i915_gt_ppgtt_pd_range(uint64_t start, uint64_t end, int lvl, unsigned *idx);
static unsigned i915_gt_ppgtt_pt_count(uint64_t start, uint64_t end);
static struct i915_gt_ppgtt_table *i915_gt_ppgtt_child_of(struct i915_gt_ppgtt *pp, const struct i915_gt_object *parent, unsigned idx);
static int i915_gt_ppgtt_new_table(struct i915_gt_mem *gm, struct i915_gt_ppgtt *pp, struct i915_gt_object *pd, unsigned idx, int lvl, struct i915_gt_ppgtt_table **result);
static int i915_gt_ppgtt_alloc_level(struct i915_gt_mem *gm, struct i915_gt_ppgtt *pp, struct i915_gt_object *pd, uint64_t *start, uint64_t end, int lvl);
static int i915_gt_ppgtt_foreach_level(struct i915_gt_ppgtt *pp, struct i915_gt_object *pd, uint64_t *start, uint64_t end, int lvl, i915_gt_ppgtt_pt_fn fn, void *data);
static int i915_gt_ppgtt_leaf_table(struct i915_gt_ppgtt *pp, uint64_t idx, struct i915_gt_object **result);
static int i915_ppgtt_insert_bits(struct i915_ppgtt *vm, uint64_t va, uint64_t physical, unsigned pages, uint64_t bits);
static int i915_ppgtt_page_alloc(struct i915_ppgtt *vm, struct kern_pmem *run, uint64_t fill);
static uint64_t *i915_ppgtt_table(uint64_t entry);
static uint64_t *i915_ppgtt_walk(struct i915_ppgtt *vm, uint64_t va, unsigned allocate);
static void i915_ppgtt_fill(void *table, uint64_t value);
static unsigned i915_ppgtt_index(uint64_t va, unsigned level);

/*
 * Encodes a Gen12 PPGTT leaf entry (gen12_pte_encode).
 *
 * Alder Lake-P never passes PTE_READ_ONLY (vm->has_read_only is false on
 * graphics versions 11 and 12) and never PTE_LM (system memory), so only
 * PRESENT, RW and the PAT index bits appear.  MTL_PPGTT_PTE_PAT3 (bit 62)
 * is Meteor Lake only; Alder Lake-P has no index of 8 or more.
 */
uint64_t
drv_i915_gen12_ppgtt_pte_encode(
	uint64_t dma,
	unsigned pat_index)
{
	uint64_t pte;

	/* A system-memory page the GPU may read and write. */
	pte = dma | I915_GEN8_PAGE_PRESENT_B | I915_GEN8_PAGE_RW_B;

	/* Spreads the PAT index over its three bits. */
	if ((pat_index & 1U) != 0U)
		pte |= I915_GEN12_PTE_PAT0;
	if ((pat_index & 2U) != 0U)
		pte |= I915_GEN12_PTE_PAT1;
	if ((pat_index & 4U) != 0U)
		pte |= I915_GEN12_PTE_PAT2;

	/* Succeeded: the leaf entry. */
	return pte;
}

/*
 * Encodes an uncached directory entry (gen8_pde_encode with I915_CACHE_NONE).
 *
 * Every scratch directory level uses it.
 */
uint64_t
drv_i915_gen8_pde_encode(
	uint64_t dma)
{
	/* Succeeded: present, writable, uncached. */
	return dma | I915_GEN8_PAGE_PRESENT_B | I915_GEN8_PAGE_RW_B | I915_PPAT_UNCACHED;
}

/*
 * Encodes a cached directory entry (gen8_pde_encode with I915_CACHE_LLC).
 *
 * PPAT_CACHED_PDE is 0, write-back through the LLC; set_pd_entry() uses it
 * for every table the range allocation adds.
 */
uint64_t
drv_i915_gen8_pde_encode_cached(
	uint64_t dma)
{
	/* Succeeded: present and writable, with no PAT bits. */
	return dma | I915_GEN8_PAGE_PRESENT_B | I915_GEN8_PAGE_RW_B;
}

/*
 * Creates the kernel PPGTT: the scratch tower and the top directory.
 *
 * This is gen8_init_scratch() followed by gen8_alloc_top_pd().  The clone
 * branch of gen8_init_scratch() is not taken: gt->vm does not exist yet when
 * the kernel address space is created, and has_read_only is false on Gen12
 * anyway.  A failure destroys whatever was built.  Returns 0, EINVAL,
 * ENOMEM, EIO, or ERANGE when a table page is out of the DMA mask.
 */
int
drv_i915_gt_ppgtt_create(
	struct i915_gt_mem *gm,
	struct i915_gt_ppgtt *pp)
{
	uint64_t dma;
	unsigned level;
	int in_range;
	int error;

	/* Refuses a missing memory or address space. */
	if (gm == NULL)
		return EINVAL;
	if (pp == NULL)
		return EINVAL;

	/* Starts from an empty four-level space. */
	kern_memset(pp, 0, sizeof(*pp));
	pp->top = I915_PPGTT_TOP;
	pp->top_count = I915_PPGTT_TOP_COUNT;

	/* Creates the scratch data page. */
	pp->scratch[0] = drv_i915_gt_object_create(gm, I915_GT_PAGE_BYTES);
	if (pp->scratch[0] == NULL) {
		error = ENOMEM;
		goto fail;
	}

	/* Finds the data page's DMA address. */
	dma = 0U;
	error = drv_i915_gt_object_page_dma(pp->scratch[0], 0U, &dma);
	if (error != 0)
		goto fail;

	/* Refuses a data page out of the DMA mask. */
	in_range = drv_i915_dma_in_range(dma, (uint64_t)I915_GT_PAGE_BYTES, gm->dma_mask);
	if (in_range == 0) {
		error = ERANGE;
		goto fail;
	}

	/* Encodes the data page uncached: cachelevel_to_pat[I915_CACHE_NONE] is 3. */
	pp->scratch_encode[0] = drv_i915_gen12_ppgtt_pte_encode(dma, I915_PAT_INDEX_CACHE_NONE);

	/* Builds each scratch level as a full page of the level below's encoding. */
	for (level = 1U; level <= (unsigned)I915_PPGTT_TOP; level++) {
		/* Creates the level's table page. */
		pp->scratch[level] = drv_i915_gt_object_create(gm, I915_GT_PAGE_BYTES);
		if (pp->scratch[level] == NULL) {
			error = ENOMEM;
			goto fail;
		}

		/* Fills every entry with the level below and flushes it. */
		i915_gt_fill_px(pp->scratch[level], pp->scratch_encode[level - 1U], I915_GT_PTES_PER_PAGE);

		/* Finds the table page's DMA address. */
		error = drv_i915_gt_object_page_dma(pp->scratch[level], 0U, &dma);
		if (error != 0)
			goto fail;

		/* Refuses a table page out of the DMA mask. */
		in_range = drv_i915_dma_in_range(dma, (uint64_t)I915_GT_PAGE_BYTES, gm->dma_mask);
		if (in_range == 0) {
			error = ERANGE;
			goto fail;
		}

		pp->scratch_encode[level] = drv_i915_gen8_pde_encode(dma);
	}

	/* Creates the top directory. */
	pp->top_pd = drv_i915_gt_object_create(gm, I915_GT_PAGE_BYTES);
	if (pp->top_pd == NULL) {
		error = ENOMEM;
		goto fail;
	}

	/* Points every top entry at the top scratch level and flushes it. */
	i915_gt_fill_px(pp->top_pd, pp->scratch_encode[I915_PPGTT_TOP], pp->top_count);

	/* Finds the top directory's DMA address, which a context's PDP0 holds. */
	error = drv_i915_gt_object_page_dma(pp->top_pd, 0U, &pp->top_pd_dma);
	if (error != 0)
		goto fail;

	/* Refuses a top directory out of the DMA mask. */
	in_range = drv_i915_dma_in_range(pp->top_pd_dma, (uint64_t)I915_GT_PAGE_BYTES, gm->dma_mask);
	if (in_range == 0) {
		error = ERANGE;
		goto fail;
	}

	/*
	 * The table pages are reached by the GPU through the context's PDP0
	 * pair, by DMA address; they are not bound into the GGTT.
	 */
	pp->inited = 1;

	/* Succeeded: every address of the space resolves to the scratch page. */
	return 0;

fail:
	/* Gives back whatever was built. */
	drv_i915_gt_ppgtt_destroy(gm, pp);

	/* Reports why the space could not be built. */
	return error;
}

/*
 * Destroys the kernel PPGTT: its tables, the top directory, then the
 * scratch tower from the top down.
 */
void
drv_i915_gt_ppgtt_destroy(
	struct i915_gt_mem *gm,
	struct i915_gt_ppgtt *pp)
{
	unsigned index;

	/* Nothing is destroyed without a memory and an address space. */
	if (gm == NULL)
		return;
	if (pp == NULL)
		return;

	/* Gives the allocated tables back, newest first. */
	index = pp->n_tables;
	while (index > 0U) {
		index--;

		/* A slot whose table was never created has nothing to give back. */
		if (pp->tables[index].obj != NULL)
			drv_i915_gt_object_destroy(gm, pp->tables[index].obj);
		pp->tables[index].obj = NULL;
	}
	pp->n_tables = 0U;

	/* Gives the top directory back. */
	if (pp->top_pd != NULL) {
		drv_i915_gt_object_destroy(gm, pp->top_pd);
		pp->top_pd = NULL;
	}

	/* Gives the scratch tower back from the top level down. */
	index = (unsigned)I915_PPGTT_TOP + 1U;
	while (index > 0U) {
		index--;

		/* A level that was never created has nothing to give back. */
		if (pp->scratch[index] != NULL) {
			drv_i915_gt_object_destroy(gm, pp->scratch[index]);
			pp->scratch[index] = NULL;
		}
	}

	pp->top_pd_dma = 0U;
	pp->inited = 0;
}

/*
 * Allocates the page tables of a GPU range in the kernel PPGTT.
 *
 * This is gen8_ppgtt_alloc(): every missing table is a page of the level
 * below's scratch, and its parent entry is written and flushed.  The range
 * is page aligned and inside the 48-bit space.  Returns 0, EINVAL, ENOSPC
 * when the table list is full, ENOMEM, EIO, or ERANGE.
 */
int
drv_i915_gt_ppgtt_alloc_range(
	struct i915_gt_mem *gm,
	struct i915_gt_ppgtt *pp,
	uint64_t start,
	uint64_t length)
{
	uint64_t first;
	uint64_t end;
	int error;

	/* Refuses a missing or unbuilt space and an empty range. */
	if (gm == NULL)
		return EINVAL;
	if (pp == NULL)
		return EINVAL;
	if (pp->inited == 0)
		return EINVAL;
	if (length == 0U)
		return EINVAL;

	/* Refuses a range that is not page aligned, wraps, or leaves the 48-bit space. */
	if (((start | length) & (I915_GT_PAGE_BYTES - 1U)) != 0U)
		return EINVAL;
	if (start + length < start)
		return EINVAL;
	if (((start + length) >> 48) != 0U)
		return EINVAL;

	/* The walk works in page indices. */
	first = start >> 12;
	end = first + (length >> 12);

	/* Allocates from the top directory down. */
	error = i915_gt_ppgtt_alloc_level(gm, pp, pp->top_pd, &first, end, pp->top);
	if (error != 0)
		return error;

	/* Succeeded: every page of the range has a leaf entry. */
	return 0;
}

/*
 * Calls a function for every leaf table of a range, in ascending order.
 *
 * This is gen8_ppgtt_foreach(), which walks only allocated ranges: a
 * missing table ends the walk with ENOENT.  Returns 0, EINVAL, or ENOENT.
 */
int
drv_i915_gt_ppgtt_foreach_pt(
	struct i915_gt_ppgtt *pp,
	uint64_t start,
	uint64_t length,
	i915_gt_ppgtt_pt_fn fn,
	void *data)
{
	uint64_t first;
	uint64_t end;
	int error;

	/* Refuses a missing or unbuilt space, a missing function and an empty or unaligned range. */
	if (pp == NULL)
		return EINVAL;
	if (pp->inited == 0)
		return EINVAL;
	if (fn == NULL)
		return EINVAL;
	if (length == 0U)
		return EINVAL;
	if (((start | length) & (I915_GT_PAGE_BYTES - 1U)) != 0U)
		return EINVAL;

	/* The walk works in page indices. */
	first = start >> 12;
	end = first + (length >> 12);

	/* Walks from the top directory down. */
	error = i915_gt_ppgtt_foreach_level(pp, pp->top_pd, &first, end, pp->top, fn, data);
	if (error != 0)
		return error;

	/* Succeeded: every leaf table of the range was visited. */
	return 0;
}

/*
 * Writes one leaf entry of the kernel PPGTT (gen8_ppgtt_insert_entry).
 *
 * The tables above the entry must already exist.  The entry is flushed
 * after it is written (drm_clflush_virt_range(&vaddr[idx], 8)).  Returns 0,
 * EINVAL, or ENOENT when a table is missing.
 */
int
drv_i915_gt_ppgtt_insert_page(
	struct i915_gt_ppgtt *pp,
	uint64_t dma,
	uint64_t offset,
	unsigned pat_index)
{
	struct i915_gt_object *table;
	uint64_t *entry;
	int error;

	/* Refuses a missing or unbuilt space and an unaligned offset. */
	if (pp == NULL)
		return EINVAL;
	if (pp->inited == 0)
		return EINVAL;
	if ((offset & (I915_GT_PAGE_BYTES - 1U)) != 0U)
		return EINVAL;

	/* Finds the leaf table that holds the entry. */
	error = i915_gt_ppgtt_leaf_table(pp, offset >> 12, &table);
	if (error != 0)
		return error;

	/* Writes the entry and flushes it for the table walker. */
	entry = &((uint64_t *)table->cpu)[(offset >> 12) & 511U];
	*entry = drv_i915_gen12_ppgtt_pte_encode(dma, pat_index);
	drv_i915_gt_clflush(entry, sizeof(uint64_t));

	/* Succeeded: the GPU translates the page to dma. */
	return 0;
}

/*
 * Points one leaf entry of the kernel PPGTT back at the scratch page.
 *
 * This is gen8_ppgtt_clear() for one page: the entry takes the space's
 * scratch encoding and is flushed; the tables stay.  Returns 0, EINVAL, or
 * ENOENT when a table is missing.
 */
int
drv_i915_gt_ppgtt_insert_scratch(
	struct i915_gt_ppgtt *pp,
	uint64_t offset)
{
	struct i915_gt_object *table;
	uint64_t *entry;
	int error;

	/* Refuses a missing or unbuilt space and an unaligned offset. */
	if (pp == NULL)
		return EINVAL;
	if (pp->inited == 0)
		return EINVAL;
	if ((offset & (I915_GT_PAGE_BYTES - 1U)) != 0U)
		return EINVAL;

	/* Finds the leaf table that holds the entry. */
	error = i915_gt_ppgtt_leaf_table(pp, offset >> 12, &table);
	if (error != 0)
		return error;

	/* Writes the scratch encoding and flushes it for the table walker. */
	entry = &((uint64_t *)table->cpu)[(offset >> 12) & 511U];
	*entry = pp->scratch_encode[0];
	drv_i915_gt_clflush(entry, sizeof(uint64_t));

	/* Succeeded: the page now reads the scratch page. */
	return 0;
}

/*
 * Builds an empty session address space whose every range resolves to the
 * scratch page.
 *
 * Returns 0, EBUSY for a space that already exists, or the page
 * allocation's error.
 */
int
drv_i915_ppgtt_create(
	struct i915_ppgtt *vm)
{
	unsigned level;
	int error;

	/* A space is created once; the caller destroys it before reuse. */
	if (vm->created != 0U)
		return EBUSY;
	kern_memset(vm, 0, sizeof(*vm));

	/* Allocates the zeroed data page every unmapped address reads. */
	error = i915_ppgtt_page_alloc(vm, &vm->scratch[0], 0U);
	if (error != 0) {
		drv_i915_ppgtt_destroy(vm);
		return error;
	}

	vm->scratch_entry[0] = (uint64_t)vm->scratch[0].paddr | I915_PPGTT_PAGE_BITS;

	/* Builds each higher scratch level as a table whose entries all name the level below. */
	for (level = 1U; level < I915_PPGTT_LEVELS; level++) {
		/* Allocates the table filled with the previous level's scratch entry. */
		error = i915_ppgtt_page_alloc(vm, &vm->scratch[level], vm->scratch_entry[level - 1U]);
		if (error != 0) {
			drv_i915_ppgtt_destroy(vm);
			return error;
		}

		vm->scratch_entry[level] = (uint64_t)vm->scratch[level].paddr | I915_PPGTT_SCRATCH_TABLE_BITS;
	}

	/* Allocates the top table, pointing every 512 GiB slice at the scratch directory pointer. */
	error = i915_ppgtt_page_alloc(vm, &vm->pml4, vm->scratch_entry[I915_PPGTT_LEVELS - 1U]);
	if (error != 0) {
		drv_i915_ppgtt_destroy(vm);
		return error;
	}

	/* Places objects above the first 4 GiB so a small stray address faults, and publishes the tables. */
	vm->next_va = I915_PPGTT_VA_START;
	vm->created = 1U;
	kern_io_write_barrier();

	/* Succeeded: the PML4 physical address can be written into a context image. */
	return 0;
}

/*
 * Returns every table page, the scratch chain and the top table of a
 * session address space to the pool.
 */
void
drv_i915_ppgtt_destroy(
	struct i915_ppgtt *vm)
{
	struct i915_ppgtt_page *page;
	unsigned level;

	/* Releases the tables inserts allocated first; their entries are not walked. */
	while (vm->pages != NULL) {
		page = vm->pages;
		vm->pages = page->next;
		(void)kern_pmem_free(&page->run);
		kern_free(page);
		vm->page_count--;
	}

	/* Releases the top table next, so no entry names a released scratch table. */
	if (vm->pml4.size != 0U) {
		(void)kern_pmem_free(&vm->pml4);
		vm->pml4.size = 0U;
	}

	/* Releases the scratch chain last, from the directory pointer down to the data page. */
	for (level = I915_PPGTT_LEVELS; level > 0U; level--) {
		/* A level that was never allocated has nothing to free. */
		if (vm->scratch[level - 1U].size != 0U) {
			(void)kern_pmem_free(&vm->scratch[level - 1U]);
			vm->scratch[level - 1U].size = 0U;
		}
	}

	vm->created = 0U;
}

/*
 * Reserves an aligned range of a session address space.
 *
 * Every range starts on a 2 MiB boundary so later huge-page use stays
 * possible; ranges are never reused within the space.  Returns 0, EINVAL
 * for an empty request, or ENOSPC when the space is exhausted.
 */
int
drv_i915_ppgtt_va_alloc(
	struct i915_ppgtt *vm,
	uint64_t bytes,
	uint64_t *va)
{
	uint64_t rounded;
	uint64_t limit;

	/* No caller receives an address for an empty or failed reservation. */
	*va = 0U;
	if (bytes == 0U)
		return EINVAL;

	/* Refuses a range that does not fit below the top of the space. */
	rounded = (bytes + I915_PPGTT_VA_ALIGN - 1ULL) & ~(I915_PPGTT_VA_ALIGN - 1ULL);
	limit = 1ULL << I915_PPGTT_ADDRESS_BITS;
	if (rounded > limit - vm->next_va)
		return ENOSPC;

	/* Advances the bump pointer past the reserved range. */
	*va = vm->next_va;
	vm->next_va += rounded;

	/* Succeeded: the range belongs to the caller for the life of the space. */
	return 0;
}

/*
 * Maps physically contiguous pages into a session address space, write-back.
 *
 * The tables grow as needed.  Returns 0, EINVAL, or ENOMEM.
 */
int
drv_i915_ppgtt_insert(
	struct i915_ppgtt *vm,
	uint64_t va,
	uint64_t physical,
	unsigned pages)
{
	int error;

	/* Maps the pages with PAT index 0. */
	error = i915_ppgtt_insert_bits(vm, va, physical, pages, I915_PPGTT_PAGE_BITS);
	if (error != 0)
		return error;

	/* Succeeded: the GPU translates the range to the given pages. */
	return 0;
}

/*
 * Maps physically contiguous pages into a session address space, uncached.
 *
 * PAT index 3 is Linux's I915_CACHE_NONE: pages the display engine reads,
 * which does not snoop the LLC, so a GPU write must reach memory rather
 * than stay in the cache.  Returns 0, EINVAL, or ENOMEM.
 */
int
drv_i915_ppgtt_insert_uncached(
	struct i915_ppgtt *vm,
	uint64_t va,
	uint64_t physical,
	unsigned pages)
{
	int error;

	/* Maps the pages with PAT index 3. */
	error = i915_ppgtt_insert_bits(vm, va, physical, pages, I915_PPGTT_PAGE_BITS | GEN12_PPGTT_PTE_PAT0 | GEN12_PPGTT_PTE_PAT1);
	if (error != 0)
		return error;

	/* Succeeded: the GPU translates the range to the given pages, uncached. */
	return 0;
}

/*
 * Points a mapped range of a session address space back at the scratch
 * page; the tables stay allocated.
 *
 * A range outside the space is ignored rather than walking garbage.
 */
void
drv_i915_ppgtt_clear(
	struct i915_ppgtt *vm,
	uint64_t va,
	unsigned pages)
{
	uint64_t *entry;
	uint64_t page_va;
	unsigned index;

	/* Ignores a range that is not page aligned or leaves the space. */
	if ((va % I915_PAGE_BYTES) != 0U)
		return;
	if (va + (uint64_t)pages * I915_PAGE_BYTES > (1ULL << I915_PPGTT_ADDRESS_BITS))
		return;

	/* Points every mapped leaf at scratch; a leaf that was never allocated already resolves to it. */
	for (index = 0U; index < pages; index++) {
		page_va = va + (uint64_t)index * I915_PAGE_BYTES;
		entry = i915_ppgtt_walk(vm, page_va, 0U);
		if (entry != NULL)
			*entry = vm->scratch_entry[0];
	}

	/* Publishes the cleared entries. */
	kern_io_write_barrier();
}

/* Fills every given entry of a kernel table page with one value, then flushes the page (fill_page_dma). */
static void
i915_gt_fill_px(
	struct i915_gt_object *object,
	uint64_t value,
	unsigned count)
{
	uint64_t *entries;
	unsigned index;

	/* Stores the value into the first count entries. */
	entries = (uint64_t *)object->cpu;
	for (index = 0U; index < count; index++)
		entries[index] = value;

	/* Writes the whole page back for the table walker. */
	drv_i915_gt_clflush(object->cpu, I915_GT_PAGE_BYTES);
}

/* Reports how many entries of one directory level a page-index range covers, and the first one. */
static unsigned
i915_gt_ppgtt_pd_range(
	uint64_t start,
	uint64_t end,
	int lvl,
	unsigned *idx)
{
	unsigned shift;
	uint64_t mask;

	/* Each level indexes with 9 bits, level 0 being the PT. */
	shift = (unsigned)lvl * 9U;
	mask = ~(uint64_t)0 << ((unsigned)(lvl + 1) * 9U);

	/* Rounds the end up to the entry that holds it. */
	end += (~mask) >> 9;
	*idx = (unsigned)((start >> shift) & 511U);

	/* A range that leaves this directory covers every entry to its end. */
	if (((start ^ end) & mask) != 0U)
		return 512U - *idx;

	/* Succeeded: the range ends inside this directory. */
	return (unsigned)((end >> shift) & 511U) - *idx;
}

/* Reports how many pages of a page-index range one leaf table covers. */
static unsigned
i915_gt_ppgtt_pt_count(
	uint64_t start,
	uint64_t end)
{
	/* A range that leaves the table covers it to its end. */
	if (((start ^ end) >> 9) != 0U)
		return 512U - (unsigned)(start & 511U);

	/* Succeeded: the range ends inside the table. */
	return (unsigned)(end - start);
}

/* Finds the table a directory entry names among the tables the kernel PPGTT allocated. */
static struct i915_gt_ppgtt_table *
i915_gt_ppgtt_child_of(
	struct i915_gt_ppgtt *pp,
	const struct i915_gt_object *parent,
	unsigned idx)
{
	unsigned index;

	/* Looks for the table recorded under this parent and entry. */
	for (index = 0U; index < pp->n_tables; index++) {
		if (pp->tables[index].parent != parent)
			continue;
		if (pp->tables[index].idx != idx)
			continue;

		/* Succeeded: the entry names this table. */
		return &pp->tables[index];
	}

	/* The entry still names scratch. */
	return NULL;
}

/* Allocates a new kernel table filled with the level below's scratch and links it into its parent entry. */
static int
i915_gt_ppgtt_new_table(
	struct i915_gt_mem *gm,
	struct i915_gt_ppgtt *pp,
	struct i915_gt_object *pd,
	unsigned idx,
	int lvl,
	struct i915_gt_ppgtt_table **result)
{
	struct i915_gt_ppgtt_table *table;
	uint64_t *entry;
	uint64_t dma;
	int in_range;
	int error;

	/* Refuses a table list that is full. */
	if (pp->n_tables == I915_PPGTT_MAX_TABLES)
		return ENOSPC;

	/* Creates the table page in the next list slot. */
	table = &pp->tables[pp->n_tables];
	table->obj = drv_i915_gt_object_create(gm, I915_GT_PAGE_BYTES);
	if (table->obj == NULL)
		return ENOMEM;

	/* Fills every entry with the level below's scratch and flushes it. */
	i915_gt_fill_px(table->obj, pp->scratch_encode[lvl], I915_GT_PTES_PER_PAGE);

	/* Finds the table page's DMA address and checks it against the mask. */
	dma = 0U;
	error = drv_i915_gt_object_page_dma(table->obj, 0U, &dma);
	if (error == 0) {
		/* A page the mask does not reach cannot be named by the parent. */
		in_range = drv_i915_dma_in_range(dma, (uint64_t)I915_GT_PAGE_BYTES, gm->dma_mask);
		if (in_range == 0)
			error = ERANGE;
	}

	/* Gives back a table the parent cannot name. */
	if (error != 0) {
		drv_i915_gt_object_destroy(gm, table->obj);
		table->obj = NULL;
		return error;
	}

	/* Records the table in the list. */
	table->parent = pd;
	table->idx = idx;
	table->lvl = lvl;
	table->dma = dma;
	pp->n_tables++;

	/* Writes the parent entry and flushes it (set_pd_entry, write_dma_entry). */
	entry = &((uint64_t *)pd->cpu)[idx];
	*entry = drv_i915_gen8_pde_encode_cached(dma);
	drv_i915_gt_clflush(entry, sizeof(uint64_t));

	*result = table;

	/* Succeeded: the parent entry names the new table. */
	return 0;
}

/* Allocates the missing tables below one directory for a page-index range (__gen8_ppgtt_alloc). */
static int
i915_gt_ppgtt_alloc_level(
	struct i915_gt_mem *gm,
	struct i915_gt_ppgtt *pp,
	struct i915_gt_object *pd,
	uint64_t *start,
	uint64_t end,
	int lvl)
{
	struct i915_gt_ppgtt_table *table;
	unsigned idx;
	unsigned len;
	int error;

	/* Finds the entries of this directory the range covers; their tables are one level down. */
	len = i915_gt_ppgtt_pd_range(*start, end, lvl, &idx);
	lvl--;

	/* Visits every covered entry. */
	for (;;) {
		/* Allocates the entry's table when it still names scratch. */
		table = i915_gt_ppgtt_child_of(pp, pd, idx);
		if (table == NULL) {
			error = i915_gt_ppgtt_new_table(gm, pp, pd, idx, lvl, &table);
			if (error != 0)
				return error;
		}

		/* Descends into a directory, or moves past the pages a leaf table covers. */
		if (lvl != 0) {
			error = i915_gt_ppgtt_alloc_level(gm, pp, table->obj, start, end, lvl);
			if (error != 0)
				return error;
		} else {
			*start += i915_gt_ppgtt_pt_count(*start, end);
		}

		/* Moves to the next entry until every covered one is done. */
		idx++;
		len--;
		if (len == 0U)
			break;
	}

	/* Succeeded: every covered entry names a table. */
	return 0;
}

/* Calls the function for every leaf table below one directory for a page-index range (__gen8_ppgtt_foreach). */
static int
i915_gt_ppgtt_foreach_level(
	struct i915_gt_ppgtt *pp,
	struct i915_gt_object *pd,
	uint64_t *start,
	uint64_t end,
	int lvl,
	i915_gt_ppgtt_pt_fn fn,
	void *data)
{
	struct i915_gt_ppgtt_table *table;
	unsigned idx;
	unsigned len;
	int error;

	/* Finds the entries of this directory the range covers; their tables are one level down. */
	len = i915_gt_ppgtt_pd_range(*start, end, lvl, &idx);
	lvl--;

	/* Visits every covered entry. */
	for (;;) {
		/* Linux walks only allocated ranges; a missing table ends the walk. */
		table = i915_gt_ppgtt_child_of(pp, pd, idx);
		if (table == NULL)
			return ENOENT;

		/* Descends into a directory, or hands a leaf table to the function. */
		if (lvl != 0) {
			error = i915_gt_ppgtt_foreach_level(pp, table->obj, start, end, lvl, fn, data);
			if (error != 0)
				return error;
		} else {
			fn(pp, table->obj, table->dma, data);
			*start += i915_gt_ppgtt_pt_count(*start, end);
		}

		/* Moves to the next entry until every covered one is done. */
		idx++;
		len--;
		if (len == 0U)
			break;
	}

	/* Succeeded: every leaf table below this directory was visited. */
	return 0;
}

/* Finds the kernel leaf table that holds a page index. */
static int
i915_gt_ppgtt_leaf_table(
	struct i915_gt_ppgtt *pp,
	uint64_t idx,
	struct i915_gt_object **result)
{
	struct i915_gt_ppgtt_table *child;
	struct i915_gt_object *table;
	int lvl;

	/* Descends from the top directory through the allocated tables. */
	table = pp->top_pd;
	for (lvl = pp->top; lvl > 0; lvl--) {
		/* A directory entry that still names scratch has no table below it. */
		child = i915_gt_ppgtt_child_of(pp, table, (unsigned)((idx >> ((unsigned)lvl * 9U)) & 511U));
		if (child == NULL)
			return ENOENT;
		table = child->obj;
	}

	*result = table;

	/* Succeeded: the leaf table holds the page's entry. */
	return 0;
}

/* Maps physically contiguous session pages with the given entry bits. */
static int
i915_ppgtt_insert_bits(
	struct i915_ppgtt *vm,
	uint64_t va,
	uint64_t physical,
	unsigned pages,
	uint64_t bits)
{
	uint64_t *entry;
	uint64_t page_va;
	uint64_t page_physical;
	unsigned index;

	/* Refuses unaligned addresses and an empty range. */
	if ((va % I915_PAGE_BYTES) != 0U)
		return EINVAL;
	if ((physical % I915_PAGE_BYTES) != 0U)
		return EINVAL;
	if (pages == 0U)
		return EINVAL;

	/* Refuses a range outside the space or beyond the GPU's physical reach. */
	if (va + (uint64_t)pages * I915_PAGE_BYTES > (1ULL << I915_PPGTT_ADDRESS_BITS))
		return EINVAL;
	if (physical + (uint64_t)pages * I915_PAGE_BYTES > I915_DMA_MAX_ADDRESS)
		return EINVAL;

	/* Writes each page's leaf entry, allocating the tables above it on demand. */
	for (index = 0U; index < pages; index++) {
		page_va = va + (uint64_t)index * I915_PAGE_BYTES;
		page_physical = physical + (uint64_t)index * I915_PAGE_BYTES;

		/* Finds the leaf; a failed table allocation clears what was mapped so far. */
		entry = i915_ppgtt_walk(vm, page_va, 1U);
		if (entry == NULL) {
			drv_i915_ppgtt_clear(vm, va, index);
			return ENOMEM;
		}

		*entry = page_physical | bits;
	}

	/* Makes the table stores globally visible before a context using them is submitted. */
	kern_io_write_barrier();

	/* Succeeded: the GPU translates the range to the given physical pages. */
	return 0;
}

/* Allocates one session table page and fills every entry with one value. */
static int
i915_ppgtt_page_alloc(
	struct i915_ppgtt *vm,
	struct kern_pmem *run,
	uint64_t fill)
{
	void *table;
	int error;

	UNUSED_PARAMETER(vm);

	/* Allocates a page the GPU's address bits reach. */
	error = kern_pmem_alloc_limited(I915_PAGE_BYTES, I915_PAGE_BYTES, I915_DMA_MAX_ADDRESS, 0U, run);
	if (error != 0)
		return error;

	/* Takes the page's CPU view from the direct map. */
	table = kern_pmem_to_kernel(run->paddr);
	if (table == NULL) {
		(void)kern_pmem_free(run);
		run->size = 0U;
		return EFAULT;
	}

	/* A fresh table names only what the caller asked for. */
	i915_ppgtt_fill(table, fill);

	/* Succeeded: the page holds 512 identical entries. */
	return 0;
}

/* Resolves the CPU view of the session table an entry points at. */
static uint64_t *
i915_ppgtt_table(
	uint64_t entry)
{
	uint64_t physical;
	uint64_t *table;

	/* Entry bits below the page and above the address width are attributes. */
	physical = entry & I915_PPGTT_ADDRESS_MASK;
	table = kern_pmem_to_kernel((hal_physaddr_t)physical);

	/* Succeeded: the child table's CPU view. */
	return table;
}

/* Walks a session space to the leaf entry of a page, optionally allocating the missing tables. */
static uint64_t *
i915_ppgtt_walk(
	struct i915_ppgtt *vm,
	uint64_t va,
	unsigned allocate)
{
	struct i915_ppgtt_page *page;
	uint64_t *table;
	uint64_t *entry;
	unsigned level;
	int error;

	/*
	 * The walk starts at the top table and descends three times to reach
	 * the leaf table.  An entry at a level either names a real child table
	 * or holds that level's scratch entry, which names the scratch table
	 * below it.
	 */
	table = kern_pmem_to_kernel(vm->pml4.paddr);
	for (level = I915_PPGTT_LEVELS - 1U; level > 0U; level--) {
		/* Enters a real child directly. */
		entry = &table[i915_ppgtt_index(va, level)];
		if (*entry != vm->scratch_entry[level]) {
			table = i915_ppgtt_table(*entry);
			continue;
		}

		/* A lookup or clear stops at the first scratch table. */
		if (allocate == 0U)
			return NULL;

		/* Allocates the record of the new child. */
		page = kern_calloc(1U, sizeof(*page));
		if (page == NULL)
			return NULL;

		/* Allocates the child with every entry the scratch entry one level down. */
		error = i915_ppgtt_page_alloc(vm, &page->run, vm->scratch_entry[level - 1U]);
		if (error != 0) {
			kern_free(page);
			return NULL;
		}

		/* Keeps the child reachable for destroy. */
		page->next = vm->pages;
		vm->pages = page;
		vm->page_count++;

		/* Points the parent at the real table with cacheable table bits. */
		*entry = (uint64_t)page->run.paddr | I915_PPGTT_TABLE_BITS;
		table = kern_pmem_to_kernel(page->run.paddr);
	}

	/* The leaf table holds the page entry itself. */
	entry = &table[i915_ppgtt_index(va, 0U)];

	/* Succeeded: the caller may read or write the leaf entry. */
	return entry;
}

/* Stores one value into all 512 entries of a session table page. */
static void
i915_ppgtt_fill(
	void *table,
	uint64_t value)
{
	uint64_t *entries;
	unsigned index;

	/* The direct-map store is coherent with the GPU's later table walk. */
	entries = table;
	for (index = 0U; index < I915_PPGTT_ENTRIES; index++)
		entries[index] = value;
}

/* Reports one level's entry index within the walk of a session address. */
static unsigned
i915_ppgtt_index(
	uint64_t va,
	unsigned level)
{
	/* Succeeded: each level indexes with 9 bits above the 12-bit page offset. */
	return (unsigned)((va >> (12U + 9U * level)) & (I915_PPGTT_ENTRIES - 1U));
}
