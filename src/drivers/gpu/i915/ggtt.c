/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The global GTT windows and the GGTT entry encoding (see ggtt.h).
 *
 * One Linux behaviour here is easy to get backwards: on graphics version 11
 * and later the GGTT page-table window is mapped uncached, so
 * gen8_ggtt_invalidate() writes no register at all -- needs_wc_ggtt_mapping()
 * is false there, and the flush register GFX_FLSH_CNTL_GEN6 belongs to the
 * gen6 path only.  Writing it here would be a deviation, not a safety net.
 */

#include "i915.h"
#include "ggtt.h"
#include "memory.h"
#include "dma.h"

#include <kern/device-io.h>

#include <uapi/errno.h>
#include <stddef.h>

static int i915_ggtt_encode(i915_dma_addr_t dma, uint64_t length, uint64_t mask, uint64_t flags, uint64_t *pte_out);
static int i915_ggtt_window_bit(const struct i915_gt_mem *gm, unsigned page);
static void i915_ggtt_window_set(struct i915_gt_mem *gm, unsigned page, int used);
static int i915_ggtt_window_alloc(struct i915_gt_mem *gm, unsigned pages, unsigned *first_out);
static void i915_ggtt_write_pte(struct i915_gt_mem *gm, unsigned index, uint64_t pte);
static int i915_ggtt_display_bit(const struct i915_gt_mem *gm, unsigned page);
static void i915_ggtt_display_set(struct i915_gt_mem *gm, unsigned first, unsigned pages, int used);
static int i915_ggtt_display_run_free(const struct i915_gt_mem *gm, unsigned first, unsigned pages);

/*
 * Reports nonzero when a whole DMA range lies within the mask.
 *
 * The range [dma_addr, dma_addr + length) is checked without computing its
 * end, so an address near the top of the space cannot wrap past the test.
 * An empty range is refused.
 */
int
drv_i915_dma_in_range(
	uint64_t dma_addr,
	uint64_t length,
	uint64_t mask)
{
	/* An empty range names nothing. */
	if (length == 0U)
		return 0;

	/* The first byte must be reachable. */
	if (dma_addr > mask)
		return 0;

	/* The last byte must be reachable: length - 1 <= mask - dma_addr. */
	if (length - 1U > mask - dma_addr)
		return 0;

	/* Succeeded: every byte of the range is reachable. */
	return 1;
}

/*
 * Encodes a system-memory GGTT entry.
 *
 * The entry is the DMA address with only PRESENT set; GEN12_GGTT_PTE_LM
 * stays clear.  Reports 1 and writes *pte_out, or reports 0 without writing
 * when the address is not page aligned or out of range, in which case the
 * caller must leave the table as it is.
 */
int
drv_i915_ggtt_pte_encode(
	i915_dma_addr_t dma,
	uint64_t length,
	uint64_t mask,
	uint64_t *pte_out)
{
	int encoded;

	/* Encodes the page with the present bit only. */
	encoded = i915_ggtt_encode(dma, length, mask, I915_GEN8_PAGE_PRESENT, pte_out);
	if (encoded == 0)
		return 0;

	/* Succeeded: *pte_out holds the entry. */
	return 1;
}

/*
 * Binds a GT object into the GT window.
 *
 * Every page is encoded as it is written; a page that cannot be encoded
 * points every entry the bind touched back at scratch and gives the run
 * back.  Returns 0, EINVAL, EBUSY for an object that is already bound,
 * ENOSPC when the window is full, EIO when a page has no DMA address, or
 * ERANGE when a page is out of the DMA mask.
 */
int
drv_i915_gt_ggtt_bind(
	struct i915_gt_mem *gm,
	struct i915_gt_object *o)
{
	uint64_t dma;
	uint64_t pte;
	unsigned first;
	unsigned page;
	int encoded;
	int error;

	/* Refuses a memory that is not prepared and an object that is not live. */
	if (gm == NULL)
		return EINVAL;
	if (gm->inited == 0)
		return EINVAL;
	if (o == NULL)
		return EINVAL;
	if (o->in_use == 0)
		return EINVAL;

	/* An object is bound at most once. */
	if (o->bound != 0)
		return EBUSY;

	/* Takes a free run of the window. */
	first = 0U;
	error = i915_ggtt_window_alloc(gm, o->pages, &first);
	if (error != 0)
		return error;

	/* Writes one entry per page, refusing rather than masking high bits off. */
	for (page = 0U; page < o->pages; page++) {
		/* Finds the page's DMA address. */
		dma = 0U;
		error = drv_i915_gt_object_page_dma(o, page, &dma);
		if (error != 0)
			goto unwind;

		/* Range-checks and encodes the page. */
		pte = 0U;
		encoded = drv_i915_ggtt_pte_encode(drv_i915_dma_addr(dma), (uint64_t)I915_GT_PAGE_BYTES, gm->dma_mask, &pte);
		if (encoded == 0) {
			error = ERANGE;
			goto unwind;
		}

		i915_ggtt_write_pte(gm, gm->window_first + first + page, pte);
	}

	/* Orders the entries before the object is used. */
	drv_i915_gt_ggtt_flush(gm);

	/* Records the binding. */
	o->ggtt_page = gm->window_first + first;
	o->ggtt_offset = (uint64_t)o->ggtt_page * I915_GT_PAGE_BYTES;
	o->bound = 1;

	/* Succeeded: the engines reach the object at ggtt_offset. */
	return 0;

unwind:
	/* Points every entry of the run back at scratch. */
	for (page = 0U; page < o->pages; page++)
		i915_ggtt_write_pte(gm, gm->window_first + first + page, gm->scratch_pte);
	drv_i915_gt_ggtt_flush(gm);

	/* Gives the run back to the window. */
	for (page = 0U; page < o->pages; page++)
		i915_ggtt_window_set(gm, first + page, 0);
	gm->allocated_pages -= o->pages;

	/* Reports why the page could not be bound. */
	return error;
}

/*
 * Unbinds a GT object from the GGTT.
 *
 * An object in the display window is handed to the display unbind.  The
 * entries are pointed back at scratch before the run is given back.
 */
void
drv_i915_gt_ggtt_unbind(
	struct i915_gt_mem *gm,
	struct i915_gt_object *o)
{
	unsigned first;
	unsigned page;

	/* An object that is not bound has nothing to release. */
	if (gm == NULL)
		return;
	if (o == NULL)
		return;
	if (o->bound == 0)
		return;

	/* A display binding follows the display window's rules. */
	if (o->display != 0) {
		drv_i915_gt_display_unbind(gm, o);
		return;
	}

	/* Points every entry of the object back at scratch. */
	first = o->ggtt_page - gm->window_first;
	for (page = 0U; page < o->pages; page++)
		i915_ggtt_write_pte(gm, o->ggtt_page + page, gm->scratch_pte);
	drv_i915_gt_ggtt_flush(gm);

	/* Gives the run back to the window. */
	for (page = 0U; page < o->pages; page++)
		i915_ggtt_window_set(gm, first + page, 0);
	gm->allocated_pages -= o->pages;

	/* Records that the object is no longer bound. */
	o->bound = 0;
	o->ggtt_page = 0U;
	o->ggtt_offset = 0U;
}

/*
 * Makes the GGTT entries written so far visible to the GPU.
 *
 * gen8_ggtt_invalidate() writes no register on graphics version 11 and
 * later: the table window is uncached and the writes have already landed.
 * The barrier orders them, and the call is counted.
 */
void
drv_i915_gt_ggtt_flush(
	struct i915_gt_mem *gm)
{
	/* A missing memory has nothing to flush. */
	if (gm == NULL)
		return;

	/* Orders the entry writes before what follows. */
	kern_io_write_barrier();
	gm->flushes++;
}

/*
 * Reads one GGTT entry back.
 *
 * The read uses the same 64-bit width as the writes, because the table is
 * a 64-bit register window.  An index outside the table reads as 0.
 */
uint64_t
drv_i915_gt_ggtt_read_pte(
	const struct i915_gt_mem *gm,
	unsigned index)
{
	uint64_t pte;

	/* An index outside the table has no entry. */
	if (gm == NULL)
		return 0U;
	if (index >= gm->entries)
		return 0U;

	/* Reads the entry through the table window. */
	pte = kern_mmio_read64((volatile void *)(gm->table + (size_t)index * 8U));

	/* Succeeded: the entry as the GPU sees it. */
	return pte;
}

/*
 * Claims the display window directly below the GT window.
 *
 * It is claimed explicitly and never as a side effect of the memory init.
 * The size must be a whole number of bitmap words and no larger than
 * I915_GT_DISPLAY_PAGES.  Returns 0, EINVAL, EBUSY when the window is
 * already claimed, or ENOSPC when the GGTT is too small.
 */
int
drv_i915_gt_display_window_init(
	struct i915_gt_mem *gm,
	unsigned pages)
{
	unsigned word;

	/* Refuses a memory that is not prepared and a size the bitmap cannot track. */
	if (gm == NULL)
		return EINVAL;
	if (gm->inited == 0)
		return EINVAL;
	if (pages == 0U)
		return EINVAL;
	if (pages > I915_GT_DISPLAY_PAGES)
		return EINVAL;
	if ((pages % 32U) != 0U)
		return EINVAL;

	/* The window is claimed once. */
	if (gm->display_pages != 0U)
		return EBUSY;

	/* Refuses a window that would reach the bottom of the table. */
	if (gm->window_first <= pages)
		return ENOSPC;

	/* Places the window below the GT window. */
	gm->display_first = gm->window_first - pages;
	gm->display_pages = pages;

	/* Starts with every page of the window free. */
	for (word = 0U; word < I915_GT_DISPLAY_WORDS; word++)
		gm->display_bitmap[word] = 0U;
	gm->display_allocated_pages = 0U;

	/* Succeeded: display buffers can be bound. */
	return 0;
}

/*
 * Binds a GT object into the display window.
 *
 * The object's first page is aligned to align_pages (a power of two) as an
 * absolute GGTT page, because the alignment is a property of the address
 * the plane is given.  guard_pages of scratch are reserved and written on
 * each side.  Every page is encoded before any entry is touched, so a
 * buffer that cannot be mapped changes nothing.  Returns 0, EINVAL, EBUSY,
 * ENOSPC, EIO, or ERANGE.
 */
int
drv_i915_gt_display_bind(
	struct i915_gt_mem *gm,
	struct i915_gt_object *o,
	unsigned align_pages,
	unsigned guard_pages)
{
	uint64_t dma;
	uint64_t pte;
	unsigned object_page;
	unsigned start;
	unsigned span;
	unsigned page;
	int found;
	int run_free;
	int encoded;
	int error;

	/* Refuses a missing window, an object that is not live and an alignment that is not a power of two. */
	if (gm == NULL)
		return EINVAL;
	if (gm->inited == 0)
		return EINVAL;
	if (o == NULL)
		return EINVAL;
	if (o->in_use == 0)
		return EINVAL;
	if (gm->display_pages == 0U)
		return EINVAL;
	if (align_pages == 0U)
		return EINVAL;
	if ((align_pages & (align_pages - 1U)) != 0U)
		return EINVAL;

	/* An object is bound at most once. */
	if (o->bound != 0)
		return EBUSY;

	/* Refuses a buffer and its guards that could never fit the window. */
	span = guard_pages + o->pages + guard_pages;
	if (span > gm->display_pages)
		return ENOSPC;

	/* Finds the first aligned place where the guards and the object are all free. */
	start = 0U;
	found = 0;
	object_page = (gm->display_first + guard_pages + align_pages - 1U) & ~(align_pages - 1U);
	for (;
	     object_page + o->pages + guard_pages <= gm->display_first + gm->display_pages;
	     object_page += align_pages) {
		start = object_page - guard_pages - gm->display_first;
		run_free = i915_ggtt_display_run_free(gm, start, span);
		if (run_free != 0) {
			found = 1;
			break;
		}
	}

	/* Reports a window with no aligned room. */
	if (found == 0) {
		gm->ggtt_alloc_fail++;
		return ENOSPC;
	}

	/* Encodes every page before touching an entry. */
	for (page = 0U; page < o->pages; page++) {
		/* Finds the page's DMA address. */
		dma = 0U;
		error = drv_i915_gt_object_page_dma(o, page, &dma);
		if (error != 0)
			return error;

		/* Refuses a page out of the DMA mask. */
		pte = 0U;
		encoded = drv_i915_ggtt_pte_encode(drv_i915_dma_addr(dma), (uint64_t)I915_GT_PAGE_BYTES, gm->dma_mask, &pte);
		if (encoded == 0)
			return ERANGE;
	}

	/* Takes the guards and the object's pages. */
	i915_ggtt_display_set(gm, start, span, 1);
	gm->display_allocated_pages += span;

	/* Writes scratch into the guards on both sides. */
	for (page = 0U; page < guard_pages; page++) {
		i915_ggtt_write_pte(gm, gm->display_first + start + page, gm->scratch_pte);
		i915_ggtt_write_pte(gm, gm->display_first + start + guard_pages + o->pages + page, gm->scratch_pte);
		gm->display_pte_writes += 2U;
	}

	/* Writes the object's pages; each was proven encodable above. */
	for (page = 0U; page < o->pages; page++) {
		dma = 0U;
		pte = 0U;
		(void)drv_i915_gt_object_page_dma(o, page, &dma);
		(void)drv_i915_ggtt_pte_encode(drv_i915_dma_addr(dma), (uint64_t)I915_GT_PAGE_BYTES, gm->dma_mask, &pte);
		i915_ggtt_write_pte(gm, gm->display_first + start + guard_pages + page, pte);
		gm->display_pte_writes++;
	}

	/* Orders the entries before the plane is given the address. */
	drv_i915_gt_ggtt_flush(gm);

	/* Records the binding. */
	o->ggtt_page = gm->display_first + start + guard_pages;
	o->ggtt_offset = (uint64_t)o->ggtt_page * I915_GT_PAGE_BYTES;
	o->display = 1;
	o->display_guard = guard_pages;
	o->bound = 1;

	/* Succeeded: the plane reaches the buffer at ggtt_offset. */
	return 0;
}

/*
 * Unbinds a GT object from the display window.
 *
 * A kept object is refused and counted.  The object's entries go back to
 * scratch; the guards already hold scratch.  The run and its guards are
 * given back together.
 */
void
drv_i915_gt_display_unbind(
	struct i915_gt_mem *gm,
	struct i915_gt_object *o)
{
	unsigned first;
	unsigned span;
	unsigned page;

	/* Only a display binding is released here. */
	if (gm == NULL)
		return;
	if (o == NULL)
		return;
	if (o->bound == 0)
		return;
	if (o->display == 0)
		return;

	/* Refuses a buffer the display may still read. */
	if (o->keep != 0) {
		gm->keep_refusals++;
		return;
	}

	/* The run the binding took, guards included. */
	first = o->ggtt_page - o->display_guard - gm->display_first;
	span = o->display_guard + o->pages + o->display_guard;

	/* Points the object's entries back at scratch. */
	for (page = 0U; page < o->pages; page++) {
		i915_ggtt_write_pte(gm, o->ggtt_page + page, gm->scratch_pte);
		gm->display_pte_writes++;
	}
	drv_i915_gt_ggtt_flush(gm);

	/* Gives the run back to the window. */
	i915_ggtt_display_set(gm, first, span, 0);
	gm->display_allocated_pages -= span;

	/* Records that the object is no longer bound. */
	o->bound = 0;
	o->display = 0;
	o->display_guard = 0U;
	o->ggtt_page = 0U;
	o->ggtt_offset = 0U;
}

/*
 * Maps pages this driver does not own into the display window.
 *
 * This is the framebuffer the firmware left: its backing belongs to the
 * firmware and is never allocated, freed or written here.  phys is the
 * first page of that backing, must be page aligned, and is used as the DMA
 * address.  The range is marked used so no buffer of this driver lands on
 * it; no guards are placed, because the backing is not ours to guard.
 * Returns 0 and the first GGTT page, EINVAL, ENOSPC when the window has no
 * room, or ERANGE when a page is out of the DMA mask.
 */
int
drv_i915_gt_display_bind_foreign(
	struct i915_gt_mem *gm,
	uint64_t phys,
	unsigned pages,
	unsigned *ggtt_page_out)
{
	uint64_t pte;
	unsigned start;
	unsigned page;
	int found;
	int run_free;
	int encoded;

	/* Refuses a missing window, an empty range and a missing result. */
	if (gm == NULL)
		return EINVAL;
	if (gm->inited == 0)
		return EINVAL;
	if (gm->display_pages == 0U)
		return EINVAL;
	if (pages == 0U)
		return EINVAL;
	if (ggtt_page_out == NULL)
		return EINVAL;

	/* Refuses a backing that does not start on a page. */
	if ((phys & (uint64_t)(I915_GT_PAGE_BYTES - 1U)) != 0U)
		return EINVAL;

	/* Refuses a range the window could never hold. */
	if (pages > gm->display_pages)
		return ENOSPC;

	/* Finds the first free run of the window. */
	found = 0;
	for (start = 0U; start + pages <= gm->display_pages; start++) {
		run_free = i915_ggtt_display_run_free(gm, start, pages);
		if (run_free != 0) {
			found = 1;
			break;
		}
	}

	/* Reports a window with no room. */
	if (found == 0) {
		gm->ggtt_alloc_fail++;
		return ENOSPC;
	}

	/* Encodes every page before touching an entry. */
	for (page = 0U; page < pages; page++) {
		pte = 0U;
		encoded = drv_i915_ggtt_pte_encode(drv_i915_dma_addr(phys + (uint64_t)page * I915_GT_PAGE_BYTES), (uint64_t)I915_GT_PAGE_BYTES, gm->dma_mask, &pte);
		if (encoded == 0)
			return ERANGE;
	}

	/* Takes the run. */
	i915_ggtt_display_set(gm, start, pages, 1);
	gm->display_allocated_pages += pages;

	/* Writes the borrowed pages; each was proven encodable above. */
	for (page = 0U; page < pages; page++) {
		pte = 0U;
		(void)drv_i915_ggtt_pte_encode(drv_i915_dma_addr(phys + (uint64_t)page * I915_GT_PAGE_BYTES), (uint64_t)I915_GT_PAGE_BYTES, gm->dma_mask, &pte);
		i915_ggtt_write_pte(gm, gm->display_first + start + page, pte);
		gm->display_pte_writes++;
	}

	/* Orders the entries before the plane is given the address. */
	drv_i915_gt_ggtt_flush(gm);
	*ggtt_page_out = gm->display_first + start;

	/* Succeeded: the plane reaches the firmware framebuffer at the reported page. */
	return 0;
}

/*
 * Points a borrowed display range back at scratch.
 *
 * The backing is left exactly as it was found.  A range outside the display
 * window is ignored.
 */
void
drv_i915_gt_display_unbind_foreign(
	struct i915_gt_mem *gm,
	unsigned ggtt_page,
	unsigned pages)
{
	unsigned start;
	unsigned page;

	/* Ignores a missing window, an empty range and a range that starts below the window. */
	if (gm == NULL)
		return;
	if (gm->inited == 0)
		return;
	if (pages == 0U)
		return;
	if (ggtt_page < gm->display_first)
		return;

	/* Ignores a range that runs past the window. */
	start = ggtt_page - gm->display_first;
	if (start + pages > gm->display_pages)
		return;

	/* Points the borrowed entries back at scratch. */
	for (page = 0U; page < pages; page++) {
		i915_ggtt_write_pte(gm, ggtt_page + page, gm->scratch_pte);
		gm->display_pte_writes++;
	}
	drv_i915_gt_ggtt_flush(gm);

	/* Gives the run back to the window. */
	i915_ggtt_display_set(gm, start, pages, 0);
	gm->display_allocated_pages -= pages;
}

/*
 * Creates the GT scratch page and pins it into the GT window.
 *
 * This is intel_gt_init_scratch(): a 4 KiB internal object (graphics
 * version is not 2) bound with PIN_HIGH, which the fixed window at the top
 * of the GGTT already provides.  Returns 0, EINVAL, ENOMEM, or the bind's
 * error.
 */
int
drv_i915_gt_init_scratch(
	struct i915_gt_mem *gm,
	struct i915_gt_object **out)
{
	struct i915_gt_object *object;
	int error;

	/* Refuses a missing memory or result. */
	if (gm == NULL)
		return EINVAL;
	if (out == NULL)
		return EINVAL;
	*out = NULL;

	/* Creates the scratch page. */
	object = drv_i915_gt_object_create(gm, I915_GT_PAGE_BYTES);
	if (object == NULL)
		return ENOMEM;

	/* Pins the page into the GT window: i915_ggtt_pin(vma, NULL, 0, PIN_HIGH). */
	error = drv_i915_gt_ggtt_bind(gm, object);
	if (error != 0) {
		drv_i915_gt_object_destroy(gm, object);
		return error;
	}

	*out = object;

	/* Succeeded: the caller owns the bound scratch page. */
	return 0;
}

/* Encodes one page with the given flags after checking its alignment and range. */
static int
i915_ggtt_encode(
	i915_dma_addr_t dma,
	uint64_t length,
	uint64_t mask,
	uint64_t flags,
	uint64_t *pte_out)
{
	uint64_t address;
	int in_range;

	/* The bare device address the entry holds. */
	address = drv_i915_dma_addr_raw(dma);

	/* Refuses an address that does not start a page. */
	if ((address & (I915_PAGE_SIZE - 1U)) != 0U)
		return 0;

	/* Refuses a range the DMA mask does not reach. */
	in_range = drv_i915_dma_in_range(address, length, mask);
	if (in_range == 0)
		return 0;

	/* The full 64-bit address is kept; it is never truncated to 32 bits. */
	*pte_out = address | flags;

	/* Succeeded: *pte_out holds the entry. */
	return 1;
}

/* Reports whether one page of the GT window is taken. */
static int
i915_ggtt_window_bit(
	const struct i915_gt_mem *gm,
	unsigned page)
{
	uint32_t word;

	/* Picks the page's bit out of its bitmap word. */
	word = gm->bitmap[page / 32U] >> (page % 32U);

	/* Succeeded: 1 when the page is taken. */
	return (int)(word & 1U);
}

/* Marks one page of the GT window taken or free. */
static void
i915_ggtt_window_set(
	struct i915_gt_mem *gm,
	unsigned page,
	int used)
{
	/* Sets or clears the page's bit. */
	if (used != 0) {
		gm->bitmap[page / 32U] |= (uint32_t)1U << (page % 32U);
	} else {
		gm->bitmap[page / 32U] &= ~((uint32_t)1U << (page % 32U));
	}
}

/* Takes the first free run of the GT window and reports its window-relative first page. */
static int
i915_ggtt_window_alloc(
	struct i915_gt_mem *gm,
	unsigned pages,
	unsigned *first_out)
{
	unsigned start;
	unsigned run;
	unsigned page;
	int taken;

	/* Refuses an empty run and a run larger than the window. */
	if (pages == 0U)
		return EINVAL;
	if (pages > gm->window_pages)
		return EINVAL;

	/* Scans for the first run of free pages long enough. */
	start = 0U;
	run = 0U;
	for (page = 0U; page < gm->window_pages; page++) {
		/* A taken page restarts the run after itself. */
		taken = i915_ggtt_window_bit(gm, page);
		if (taken != 0) {
			run = 0U;
			start = page + 1U;
			continue;
		}

		/* A free page extends the run; a long enough run ends the scan. */
		run++;
		if (run == pages)
			break;
	}

	/* Reports a window with no run long enough. */
	if (run != pages) {
		gm->ggtt_alloc_fail++;
		return ENOSPC;
	}

	/* Takes the run. */
	for (page = start; page < start + pages; page++)
		i915_ggtt_window_set(gm, page, 1);
	gm->allocated_pages += pages;
	*first_out = start;

	/* Succeeded: the run belongs to the caller. */
	return 0;
}

/* Writes one GGTT entry through the table window. */
static void
i915_ggtt_write_pte(
	struct i915_gt_mem *gm,
	unsigned index,
	uint64_t pte)
{
	/* Stores the entry with one 64-bit write and counts it. */
	kern_mmio_write64((volatile void *)(gm->table + (size_t)index * 8U), pte);
	gm->pte_writes++;
}

/* Reports whether one page of the display window is taken. */
static int
i915_ggtt_display_bit(
	const struct i915_gt_mem *gm,
	unsigned page)
{
	uint32_t word;

	/* Picks the page's bit out of its bitmap word. */
	word = gm->display_bitmap[page / 32U] >> (page % 32U);

	/* Succeeded: 1 when the page is taken. */
	return (int)(word & 1U);
}

/* Marks a run of the display window taken or free. */
static void
i915_ggtt_display_set(
	struct i915_gt_mem *gm,
	unsigned first,
	unsigned pages,
	int used)
{
	unsigned page;

	/* Sets or clears every bit of the run. */
	for (page = first; page < first + pages; page++) {
		if (used != 0) {
			gm->display_bitmap[page / 32U] |= (uint32_t)1U << (page % 32U);
		} else {
			gm->display_bitmap[page / 32U] &= ~((uint32_t)1U << (page % 32U));
		}
	}
}

/* Reports whether every page of a run of the display window is free. */
static int
i915_ggtt_display_run_free(
	const struct i915_gt_mem *gm,
	unsigned first,
	unsigned pages)
{
	unsigned page;
	int taken;

	/* Stops at the first taken page. */
	for (page = first; page < first + pages; page++) {
		taken = i915_ggtt_display_bit(gm, page);
		if (taken != 0)
			return 0;
	}

	/* Succeeded: the whole run is free. */
	return 1;
}
