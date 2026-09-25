/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU-free kernel checks of the scanout buffer.
 *
 * They cover the layout, the aligned and guarded GGTT placement, the
 * coexistence with the GT window's objects, the unwinding of a failed pin
 * and the refusal to release a buffer the display still reads.  The
 * backing pages are real DMA allocations of a DMA device of the test's own
 * (the GPU's 39-bit constraints); a sentinel-filled table stands in for
 * the GGTT, so nothing reaches the hardware.
 */

#include "display-ktest.h"
#include <kern/kcrt.h>

#include "../execution/ktest.h"

#include "../../display/internal.h"
#include "../../display/scanout.h"
#include "../../ggtt.h"
#include "../../memory.h"

#include <drivers/generic/dma.h>
#include <kern/klog.h>

#include <uapi/errno.h>
#include <stdint.h>

/* How many entries the stand-in GGTT has (64 MiB of GGTT space). */
#define I915_SCANOUT_KTEST_TABLE_ENTRIES	16384U

/* What an entry nobody wrote holds; any other value is a write. */
#define I915_SCANOUT_KTEST_SENTINEL		0x5a5a5a5a5a5a5a5aULL

/* The scratch encoding the memory writes into a free or guard entry. */
#define I915_SCANOUT_KTEST_SCRATCH_PTE		0x00000000dead0001ULL

/* The format fourcc 'AR24', which the scanout layout does not support. */
#define I915_SCANOUT_KTEST_FOURCC_ARGB8888	0x34325241U

/* The X-tiled modifier, which the scanout layout does not support. */
#define I915_SCANOUT_KTEST_MOD_X_TILED		1ULL

/*
 * The stand-in GGTT.
 *
 * It is refilled with the sentinel at the start of the run, so any entry
 * that differs afterwards was written by the code under test.  Only the
 * one ktest thread uses it.
 */
static uint64_t i915_scanout_ktest_table[I915_SCANOUT_KTEST_TABLE_ENTRIES];

/*
 * The GT memory over the stand-in GGTT.
 *
 * It is prepared at the start of the run and finalised at its end; it is
 * static because it holds the object pool, too large for the stack.
 */
static struct i915_gt_mem i915_scanout_ktest_gm;

/*
 * The two scanout buffers of the run: the one on the panel and its flip
 * partner.
 *
 * Their storage must start zeroed, and destroy returns it to that state.
 */
static struct i915_scanout i915_scanout_ktest_front;
static struct i915_scanout i915_scanout_ktest_back;

static int i915_scanout_ktest_dma_create(struct drv_dma_device **dma);
static void i915_scanout_ktest_table_fill(void);
static unsigned i915_scanout_ktest_stray_writes(unsigned first, unsigned end, unsigned gt_first, unsigned gt_end);
static int i915_scanout_ktest_ptes_match(const struct i915_scanout *so);
static int i915_scanout_ktest_guards_are_scratch(const struct i915_scanout *so);
static int i915_scanout_ktest_apart(const struct i915_scanout *a, const struct i915_scanout *b);
static void i915_scanout_ktest_body(struct i915_ktest *ktest, struct drv_dma_device *dma, uint64_t dma_mask);
static int i915_scanout_ktest_lifetime(struct i915_ktest *ktest, const struct i915_gt_object *ring, const struct i915_gt_object *hwsp);
static void i915_scanout_ktest_refusals(struct i915_ktest *ktest, uint64_t dma_mask);
static void i915_scanout_ktest_abandon(struct i915_ktest *ktest);

/*
 * Checks the scanout buffer on a stand-in GGTT.
 *
 * The checks never touch the hardware; the backing pages come from a DMA
 * device the part creates with the GPU's constraints and destroys at the
 * end.
 */
void
drv_i915_display_ktest_scanout(
	struct i915_ktest *ktest)
{
	struct drv_dma_device *dma;
	int error;

	/* Creates the DMA device the backing pages come from. */
	dma = NULL;
	error = i915_scanout_ktest_dma_create(&dma);
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "scanout: could not create a DMA device for the scanout tests");
		return;
	}

	/* Runs the checks. */
	i915_scanout_ktest_body(ktest, dma, I915_DMA_MAX_ADDRESS);

	/* Gives the DMA device back; every allocation of the run was released. */
	error = drv_dma_device_destroy(dma);
	if (error != 0)
		kern_logf("i915: scanout ktest: DMA device not destroyed (error %d)\n", error);
}

/* Creates a DMA device with the GPU's DMA constraints. */
static int
i915_scanout_ktest_dma_create(
	struct drv_dma_device **dma)
{
	static const struct drv_dma_constraints constraints = {
		39U,
		0xffffffffU,
		0U,
		1
	};
	int error;

	/* Declares the GPU's 39-bit DMA capability, as the device start does. */
	error = drv_dma_device_create(&constraints, dma);
	if (error != 0)
		return error;

	/* Succeeded: the device hands out pages the GPU can address. */
	return 0;
}

/* Fills the stand-in GGTT with the sentinel. */
static void
i915_scanout_ktest_table_fill(void)
{
	unsigned index;

	/* Marks every entry as not written. */
	for (index = 0U; index < I915_SCANOUT_KTEST_TABLE_ENTRIES; index++)
		i915_scanout_ktest_table[index] = I915_SCANOUT_KTEST_SENTINEL;
}

/* Counts the written entries outside one allocation and outside the GT window. */
static unsigned
i915_scanout_ktest_stray_writes(
	unsigned first,
	unsigned end,
	unsigned gt_first,
	unsigned gt_end)
{
	unsigned index;
	unsigned count;

	/* Every entry outside [first, end) and the GT window must still be the sentinel. */
	count = 0U;
	for (index = 0U; index < I915_SCANOUT_KTEST_TABLE_ENTRIES; index++) {
		/* The allocation's own entries were meant to be written. */
		if (index >= first && index < end)
			continue;

		/* The GT window's live objects were written before. */
		if (index >= gt_first && index < gt_end)
			continue;

		/* Counts an entry nobody should have written. */
		if (i915_scanout_ktest_table[index] != I915_SCANOUT_KTEST_SENTINEL)
			count++;
	}

	/* Reports how many entries were written by mistake. */
	return count;
}

/* Reports whether every page of a buffer is mapped to its own DMA address. */
static int
i915_scanout_ktest_ptes_match(
	const struct i915_scanout *so)
{
	uint64_t dma;
	uint64_t expected;
	unsigned page;
	int error;

	/* A buffer without backing maps nothing. */
	if (so->obj == NULL)
		return 0;

	/* Compares each entry with an independent expectation. */
	for (page = 0U; page < so->obj->pages; page++) {
		/* Asks the backing where the page lives. */
		dma = 0U;
		error = drv_i915_gt_object_page_dma(so->obj, page, &dma);
		if (error != 0)
			return 0;

		/* The page address and PRESENT, nothing else on this generation. */
		expected = (dma & ~0xfffULL) | 1ULL;
		if (i915_scanout_ktest_table[so->obj->ggtt_page + page] != expected)
			return 0;
	}

	/* Succeeded: every page is mapped where it lives. */
	return 1;
}

/* Reports whether both guards of a buffer hold the scratch encoding. */
static int
i915_scanout_ktest_guards_are_scratch(
	const struct i915_scanout *so)
{
	unsigned below;
	unsigned above;
	unsigned page;

	/* A buffer without backing has no guards. */
	if (so->obj == NULL)
		return 0;

	/* Compares the guard entries on both sides with the scratch encoding. */
	for (page = 0U; page < so->guard_pages; page++) {
		below = so->obj->ggtt_page - so->guard_pages + page;
		above = so->obj->ggtt_page + so->obj->pages + page;

		/* The guard below the buffer. */
		if (i915_scanout_ktest_table[below] != I915_SCANOUT_KTEST_SCRATCH_PTE)
			return 0;

		/* The guard above the buffer. */
		if (i915_scanout_ktest_table[above] != I915_SCANOUT_KTEST_SCRATCH_PTE)
			return 0;
	}

	/* Succeeded: both guards are scratch. */
	return 1;
}

/* Reports whether two pinned buffers' ranges, guards included, do not overlap. */
static int
i915_scanout_ktest_apart(
	const struct i915_scanout *a,
	const struct i915_scanout *b)
{
	unsigned a_first;
	unsigned a_end;
	unsigned b_first;
	unsigned b_end;

	/* A buffer without backing has no range. */
	if (a->obj == NULL || b->obj == NULL)
		return 0;

	/* The ranges with both guards. */
	a_first = a->obj->ggtt_page - a->guard_pages;
	a_end = a->obj->ggtt_page + a->obj->pages + a->guard_pages;
	b_first = b->obj->ggtt_page - b->guard_pages;
	b_end = b->obj->ggtt_page + b->obj->pages + b->guard_pages;

	/* The second range lies wholly above the first. */
	if (b_first >= a_end)
		return 1;

	/* The second range lies wholly below the first. */
	if (a_first >= b_end)
		return 1;

	/* The ranges overlap. */
	return 0;
}

/* Runs the checks over a fresh stand-in GGTT. */
static void
i915_scanout_ktest_body(
	struct i915_ktest *ktest,
	struct drv_dma_device *dma,
	uint64_t dma_mask)
{
	struct i915_gt_mem *gm;
	struct i915_gt_object *ring;
	struct i915_gt_object *hwsp;
	int error;

	gm = &i915_scanout_ktest_gm;

	/* A GT window with live objects comes first: they must not notice the display side. */
	i915_scanout_ktest_table_fill();
	error = drv_i915_gt_mem_init(gm, dma, dma_mask, i915_scanout_ktest_table, I915_SCANOUT_KTEST_TABLE_ENTRIES, I915_SCANOUT_KTEST_SCRATCH_PTE, NULL);
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "scanout: SCANOUT-SETUP GT window objects");
		return;
	}

	/* Creates a ring and a status page in the GT window. */
	ring = drv_i915_gt_object_create(gm, 16U * 4096U);
	hwsp = drv_i915_gt_object_create(gm, 4096U);
	if (ring == NULL || hwsp == NULL) {
		drv_i915_ktest_check(ktest, 0, "scanout: SCANOUT-SETUP GT window objects");
		drv_i915_gt_mem_fini(gm);
		return;
	}

	/* Binds the ring. */
	error = drv_i915_gt_ggtt_bind(gm, ring);
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "scanout: SCANOUT-SETUP GT window objects");
		drv_i915_gt_mem_fini(gm);
		return;
	}

	/* Binds the status page. */
	error = drv_i915_gt_ggtt_bind(gm, hwsp);
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "scanout: SCANOUT-SETUP GT window objects");
		drv_i915_gt_mem_fini(gm);
		return;
	}

	/* The window, the layout, the pin, the second buffer and the release order. */
	error = i915_scanout_ktest_lifetime(ktest, ring, hwsp);
	if (error != 0) {
		drv_i915_gt_mem_fini(gm);
		return;
	}

	/* The refusals and the unwinding of a failed pin. */
	i915_scanout_ktest_refusals(ktest, dma_mask);

	/* The stop that could not be confirmed, and the teardown that follows it. */
	i915_scanout_ktest_abandon(ktest);
}

/* Checks the window, the layout, the pin, a second buffer and the order of release; EIO when no buffer could be made. */
static int
i915_scanout_ktest_lifetime(
	struct i915_ktest *ktest,
	const struct i915_gt_object *ring,
	const struct i915_gt_object *hwsp)
{
	static uint64_t gt_snapshot[I915_GT_GGTT_PAGES];
	struct i915_gt_mem *gm;
	struct i915_scanout *so;
	struct i915_scanout *so2;
	unsigned gt_first;
	unsigned gt_end;
	unsigned index;
	unsigned stray;
	unsigned first;
	unsigned pages;
	unsigned live_before;
	int second_claim;
	int ptes_ok;
	int guards_ok;
	int same;
	int scratch_ok;
	int unpin_error;
	int destroy_error;
	int destroy_error2;
	int passed;
	int error;
	int error2;

	gm = &i915_scanout_ktest_gm;
	so = &i915_scanout_ktest_front;
	so2 = &i915_scanout_ktest_back;

	/* Takes a copy of the GT window's entries to compare with later. */
	gt_first = gm->window_first;
	gt_end = gt_first + I915_GT_GGTT_PAGES;
	for (index = 0U; index < I915_GT_GGTT_PAGES; index++)
		gt_snapshot[index] = i915_scanout_ktest_table[gt_first + index];

	/* Claims the display window, then claims it again. */
	second_claim = 0;
	error = drv_i915_gt_display_window_init(gm, I915_GT_DISPLAY_PAGES);
	if (error == 0)
		second_claim = drv_i915_gt_display_window_init(gm, I915_GT_DISPLAY_PAGES);
	stray = i915_scanout_ktest_stray_writes(0U, 0U, gt_first, gt_end);

	/* The display window sits directly below the GT window, is claimed once, and claiming it writes nothing. */
	passed = 0;
	if (error == 0 &&
	    gm->display_first + gm->display_pages == gm->window_first &&
	    second_claim == EBUSY &&
	    stray == 0U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "scanout: SCANOUT-WINDOW the display window sits directly below the GT window; claiming it writes no PTE");

	/* Lays out a full-HD buffer: the memory image, not the link's 18 bpp. */
	error = drv_i915_scanout_create(gm, 1920U, 1080U, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, so);

	/* 4 bytes a pixel, a 64-byte aligned pitch, whole pages, 256 KiB alignment and 168-entry guards. */
	passed = 0;
	if (error == 0 &&
	    so->cpp == 4U &&
	    so->pitch == 7680U &&
	    so->stride_units == 120U &&
	    so->size == 8294400U &&
	    so->obj->pages == 2025U &&
	    so->alignment == 262144U &&
	    so->guard_pages == 168U &&
	    so->cpu != NULL &&
	    so->state == I915_SCANOUT_ALLOCATED)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "scanout: SCANOUT-LAYOUT 1920x1080 XRGB8888 linear: 4 bytes/pixel, pitch 7680 (120 x 64), 8294400 bytes = 2025 pages, 256 KiB alignment, 168-PTE guards");

	/* Nothing else can be checked without the buffer. */
	if (error != 0)
		return EIO;

	/* Pins the buffer. */
	error = drv_i915_scanout_pin(so, "ktest");

	/* The surface address is aligned, 32-bit, inside the window with room for both guards, and published once. */
	passed = 0;
	if (error == 0 &&
	    so->state == I915_SCANOUT_PINNED &&
	    (so->surf & 0x3ffffULL) == 0U &&
	    (so->surf >> 32) == 0U &&
	    so->obj->ggtt_page >= gm->display_first + so->guard_pages &&
	    so->obj->ggtt_page + so->obj->pages + so->guard_pages <= gm->window_first &&
	    so->publishes == 1U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "scanout: SCANOUT-PIN the surface address is 256 KiB aligned, below 4 GiB, inside the display window with room for both guards; the cache was flushed for the display");

	/* Reads back the pinned buffer's entries and guards. */
	ptes_ok = 0;
	guards_ok = 0;
	if (error == 0) {
		ptes_ok = i915_scanout_ktest_ptes_match(so);
		guards_ok = i915_scanout_ktest_guards_are_scratch(so);
	}

	/* Every page maps to its own DMA address, and both guards are scratch. */
	passed = 0;
	if (error == 0 && ptes_ok && guards_ok)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "scanout: SCANOUT-PTES every backing page maps to its own DMA address (address | present); 168 scratch PTEs on each side");

	/* Compares the GT window with its copy. */
	same = 1;
	for (index = 0U; index < I915_GT_GGTT_PAGES; index++) {
		/* One changed entry is enough to fail. */
		if (i915_scanout_ktest_table[gt_first + index] != gt_snapshot[index])
			same = 0;
	}

	/* Counts the entries written outside the buffer, its guards and the GT window. */
	stray = 1U;
	if (error == 0) {
		first = so->obj->ggtt_page - so->guard_pages;
		stray = i915_scanout_ktest_stray_writes(first, so->obj->ggtt_page + so->obj->pages + so->guard_pages, gt_first, gt_end);
	}

	/* The GT window's entries and bindings are untouched, and nothing else was written. */
	passed = 0;
	if (error == 0 &&
	    same &&
	    ring->bound &&
	    hwsp->bound &&
	    stray == 0U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "scanout: SCANOUT-COEXIST the GT window's PTEs are byte-identical, its objects still bound, and no PTE outside the allocation was written");

	/* A second buffer, the flip partner, is made and pinned next to it. */
	error2 = drv_i915_scanout_create(gm, 1920U, 1080U, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, so2);
	if (error2 == 0)
		error2 = drv_i915_scanout_pin(so2, "ktest-2");

	/* Reads back both buffers' entries. */
	passed = 0;
	if (error2 == 0 && (so2->surf & 0x3ffffULL) == 0U) {
		ptes_ok = i915_scanout_ktest_ptes_match(so2);
		guards_ok = i915_scanout_ktest_guards_are_scratch(so2);
		same = i915_scanout_ktest_ptes_match(so);
		if (ptes_ok && guards_ok && same)
			passed = i915_scanout_ktest_apart(so, so2);
	}

	drv_i915_ktest_check(ktest, passed, "scanout: SCANOUT-SECOND a second full-HD buffer fits with its own alignment and guards, without overlap");

	/* The display starts reading the first buffer; its release is then attempted. */
	unpin_error = 0;
	destroy_error = 0;
	ptes_ok = 0;
	error = drv_i915_scanout_begin(so);
	if (error == 0) {
		unpin_error = drv_i915_scanout_unpin(so);
		destroy_error = drv_i915_scanout_destroy(so);
		ptes_ok = i915_scanout_ktest_ptes_match(so);
	}

	/* Unpin and destroy are both refused and counted; the entries stay. */
	passed = 0;
	if (error == 0 &&
	    unpin_error == EBUSY &&
	    destroy_error == EBUSY &&
	    so->refused_unpin == 1U &&
	    so->refused_destroy == 1U &&
	    ptes_ok)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "scanout: SCANOUT-IN-USE unpin and destroy are refused while the display reads the buffer; its PTEs stay");

	/* The display stopped; destroy is tried before unpin. */
	drv_i915_scanout_end(so);
	destroy_error = drv_i915_scanout_destroy(so);
	unpin_error = EINVAL;
	if (destroy_error == EBUSY)
		unpin_error = drv_i915_scanout_unpin(so);

	/* Destroy before unpin is refused; the unpin then succeeds and the window's first entry was never written. */
	passed = 0;
	if (destroy_error == EBUSY &&
	    unpin_error == 0 &&
	    i915_scanout_ktest_table[gm->display_first + 0U] == I915_SCANOUT_KTEST_SENTINEL)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "scanout: SCANOUT-ORDER destroy before unpin is refused; after the scanout ended the unpin succeeds");

	/* Unpins the second buffer and reads its former entries back. */
	first = 0U;
	pages = 0U;
	if (so2->obj != NULL) {
		first = so2->obj->ggtt_page;
		pages = so2->obj->pages;
	}

	error = drv_i915_scanout_unpin(so2);
	scratch_ok = 1;
	for (index = 0U; index < pages; index++) {
		/* One entry left pointing at the page is enough to fail. */
		if (i915_scanout_ktest_table[first + index] != I915_SCANOUT_KTEST_SCRATCH_PTE)
			scratch_ok = 0;
	}

	/* The entries go back to scratch and the whole range, guards included, is free again. */
	passed = 0;
	if (error == 0 &&
	    scratch_ok &&
	    gm->display_allocated_pages == 0U &&
	    so2->surf == 0U)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "scanout: SCANOUT-UNPIN the pages' PTEs go back to scratch and the whole range (guards included) is free again");

	/* Destroys both buffers. */
	live_before = gm->objects_live;
	destroy_error = drv_i915_scanout_destroy(so);
	destroy_error2 = EINVAL;
	if (destroy_error == 0)
		destroy_error2 = drv_i915_scanout_destroy(so2);

	/* Both backings went back to the pool. */
	passed = 0;
	if (destroy_error == 0 && destroy_error2 == 0 && gm->objects_live + 2U == live_before)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "scanout: SCANOUT-DESTROY both backings are returned");

	/* Succeeded: the buffers were made, used and released. */
	return 0;
}

/* Checks the refusals of unsupported layouts and the unwinding of a pin that cannot be completed. */
static void
i915_scanout_ktest_refusals(
	struct i915_ktest *ktest,
	uint64_t dma_mask)
{
	struct i915_gt_mem *gm;
	struct i915_scanout *so;
	struct i915_scanout *so2;
	unsigned live_before;
	unsigned writes_before;
	unsigned allocated_before;
	int format_error;
	int modifier_error;
	int size_error;
	int ptes_ok;
	int guards_ok;
	int passed;
	int error;
	int error2;

	gm = &i915_scanout_ktest_gm;
	so = &i915_scanout_ktest_front;
	so2 = &i915_scanout_ktest_back;

	/* Asks for another format, a tiled modifier and a zero size. */
	live_before = gm->objects_live;
	modifier_error = 0;
	size_error = 0;
	format_error = drv_i915_scanout_create(gm, 1920U, 1080U, I915_SCANOUT_KTEST_FOURCC_ARGB8888, I915_MOD_LINEAR, so);
	if (format_error == EINVAL)
		modifier_error = drv_i915_scanout_create(gm, 1920U, 1080U, I915_FOURCC_XRGB8888, I915_SCANOUT_KTEST_MOD_X_TILED, so);
	if (modifier_error == EINVAL)
		size_error = drv_i915_scanout_create(gm, 0U, 1080U, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, so);

	/* Each is refused before anything is allocated. */
	passed = 0;
	if (format_error == EINVAL &&
	    modifier_error == EINVAL &&
	    size_error == EINVAL &&
	    gm->objects_live == live_before)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "scanout: SCANOUT-REFUSE another format, a tiled modifier or a zero size is refused before anything is allocated");

	/*
	 * A backing the GGTT cannot address: the DMA mask is narrowed for the
	 * one pin (a test-only change of the memory's bookkeeping), so no page
	 * of the buffer is in range.
	 */
	error = drv_i915_scanout_create(gm, 1920U, 1080U, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, so);
	writes_before = gm->display_pte_writes;
	allocated_before = gm->display_allocated_pages;
	gm->dma_mask = 0xfffULL;
	error2 = -1;
	if (error == 0)
		error2 = drv_i915_scanout_pin(so, "ktest-range");
	gm->dma_mask = dma_mask;

	/* The pin fails with nothing reserved and no entry written. */
	passed = 0;
	if (error == 0 &&
	    error2 == ERANGE &&
	    so->state == I915_SCANOUT_ALLOCATED &&
	    gm->display_pte_writes == writes_before &&
	    gm->display_allocated_pages == allocated_before &&
	    so->obj->bound == 0)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "scanout: SCANOUT-UNWIND a page outside the DMA range fails the pin with nothing reserved and no PTE written");

	/*
	 * A window too small for the second buffer.  Nothing is allocated
	 * now, so the test shrinks what the allocator may use.
	 */
	gm->display_pages = 4096U;
	gm->display_first = gm->window_first - 4096U;
	if (error == 0)
		error = drv_i915_scanout_pin(so, "ktest");

	/* Makes the second buffer and tries to pin it. */
	error2 = drv_i915_scanout_create(gm, 1920U, 1080U, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, so2);
	allocated_before = gm->display_allocated_pages;
	writes_before = gm->display_pte_writes;
	if (error2 == 0)
		error2 = drv_i915_scanout_pin(so2, "ktest-2");

	/* Reads back the first buffer's entries and guards. */
	ptes_ok = 0;
	guards_ok = 0;
	if (error == 0) {
		ptes_ok = i915_scanout_ktest_ptes_match(so);
		guards_ok = i915_scanout_ktest_guards_are_scratch(so);
	}

	/* ENOSPC with nothing reserved or written; the pinned one is untouched. */
	passed = 0;
	if (error == 0 &&
	    error2 == ENOSPC &&
	    gm->display_allocated_pages == allocated_before &&
	    gm->display_pte_writes == writes_before &&
	    ptes_ok &&
	    guards_ok)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "scanout: SCANOUT-FULL no room for another buffer is ENOSPC with nothing reserved or written; the pinned one is untouched");

	/* Releases the second buffer and the first one's pin, and restores the whole window. */
	(void)drv_i915_scanout_destroy(so2);
	(void)drv_i915_scanout_unpin(so);
	gm->display_pages = I915_GT_DISPLAY_PAGES;
	gm->display_first = gm->window_first - I915_GT_DISPLAY_PAGES;

	/* Pins the first buffer again for the abandon check. */
	(void)drv_i915_scanout_pin(so, "ktest");
}

/* Checks that a buffer whose stop was not confirmed survives every release, the teardown included. */
static void
i915_scanout_ktest_abandon(
	struct i915_ktest *ktest)
{
	struct i915_gt_mem *gm;
	struct i915_scanout *so;
	struct i915_gt_object *object;
	int unpin_error;
	int destroy_error;
	int ptes_ok;
	int passed;

	gm = &i915_scanout_ktest_gm;
	so = &i915_scanout_ktest_front;

	/* The display reads the buffer, and its stop cannot be confirmed. */
	(void)drv_i915_scanout_begin(so);
	drv_i915_scanout_abandon(so);

	/* Tries to release it. */
	unpin_error = drv_i915_scanout_unpin(so);
	destroy_error = 0;
	if (unpin_error == EBUSY)
		destroy_error = drv_i915_scanout_destroy(so);

	/* Neither unpin nor destroy is allowed any more. */
	passed = 0;
	if (so->state == I915_SCANOUT_ABANDONED &&
	    unpin_error == EBUSY &&
	    destroy_error == EBUSY)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "scanout: SCANOUT-ABANDON a buffer whose scanout stop was not confirmed can no longer be unpinned or destroyed");

	/* Tears the GT memory down around the abandoned buffer. */
	drv_i915_gt_mem_fini(gm);
	object = so->obj;
	ptes_ok = 0;
	if (object != NULL)
		ptes_ok = i915_scanout_ktest_ptes_match(so);

	/* The teardown released the other objects and left the kept one's pages, mapping and entries alone. */
	passed = 0;
	if (object != NULL &&
	    gm->kept_objects == 1U &&
	    object->in_use &&
	    ptes_ok)
		passed = 1;

	drv_i915_ktest_check(ktest, passed, "scanout: SCANOUT-KEPT teardown leaves its pages, mapping and PTEs alone and says so (the other objects are released)");

	/*
	 * Gives the memory back now that nothing can read it; only the test
	 * may clear keep, because no display ever scanned this buffer out.
	 */
	if (object != NULL) {
		object->keep = 0;
		drv_i915_gt_object_destroy(gm, object);
	}

	kern_memset(so, 0, sizeof(*so));
}
