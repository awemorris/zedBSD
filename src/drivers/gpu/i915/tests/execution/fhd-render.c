/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The full-HD textured draw into a buffer the display tests own.
 *
 * The draw submits the fixed full-HD batch through the compute test's
 * request path on a fresh context of the GT address space.  Its render
 * target is the caller's object, mapped page by page onto the same backing
 * pages the display binding shows, so what the GPU draws is what the panel
 * scans out.  The release follows the reference's order: every mapping back
 * to scratch, then the GT TLB, and only then the draw's own objects.
 */

#include "fhd-render.h"
#include "eu-test.h"
#include "eu-internal.h"
#include "../fixtures/draw-fixture.h"
#include "../../device-info.h"
#include "../../engine.h"
#include "../../ggtt.h"
#include "../../memory.h"
#include "../../ppgtt.h"
#include "../../submit.h"
#include "../../tlb.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <stddef.h>

/* The GPU page size every mapping of the draw is made in. */
#define I915_TEST_FHD_PAGE_BYTES	4096U

/* The value the CPU fills the target with before the draw; a pixel still holding it was never written. */
#define I915_TEST_FHD_PREFILL		0x5a5a5a5aU

/* The target's span in the address space, rounded up to whole pages. */
#define I915_TEST_FHD_RT_SPAN		((I915_TEX_FHD_RT_BYTES + 4095U) & ~4095U)

/* How many pages the target covers. */
#define I915_TEST_FHD_RT_PAGES		((I915_TEX_FHD_RT_BYTES + 4095U) / 4096U)

/* How many pages the state page, batch and texture reserve, starting at the state page. */
#define I915_TEST_FHD_FIXTURE_RANGE_PAGES	5U

/* How many fixture objects the draw maps: the state page, the batch and the texture. */
#define I915_TEST_FHD_FIXTURE_MAPS	3U

/* The most mappings a release walks: the fixture objects and up to 2048 target pages. */
#define I915_TEST_FHD_MAPS_MAX		(I915_TEST_FHD_FIXTURE_MAPS + 2048U)

/* The batch capacity in dwords, which is one page. */
#define I915_TEST_FHD_BATCH_DWORDS	1024U

/* Each texel column covers 240 pixels and each texel row 135, so the image is 8 texels wide. */
#define I915_TEST_FHD_CELL_WIDTH	240U
#define I915_TEST_FHD_CELL_HEIGHT	135U
#define I915_TEST_FHD_TEXEL_COLUMNS	8U

/* How many target pages the walk check probes: the first, a middle and the last. */
#define I915_TEST_FHD_WALK_PROBES	3U

/*
 * The address layout of the full-HD draw.
 *
 * The state page, batch and texture keep the 32x32 draw's addresses; the two
 * render targets follow at their own fixed addresses.  The table never
 * changes; drv_i915_test_fhd_va_layout() checks and reports it.
 */
static const struct i915_test_fhd_va i915_fhd_va[] = {
	{ "state page", I915_TEST_EU_SHARED_VA, 4096U, 4096U },
	{ "batch", I915_TEST_EU_BATCH_VA, 4096U, 4096U },
	{ "texture", I915_TEX_FIXTURE_TEX_VA, 4096U, 4096U },
	{ "render target = scanout backing", I915_TEX_FHD_RT_VA, I915_TEST_FHD_RT_SPAN, 4096U },
	{ "second render target = scanout backing", I915_TEX_FHD_RT_B_VA, I915_TEST_FHD_RT_SPAN, 4096U },
};

/*
 * Where the draw maps its own objects, in the order it maps them.
 *
 * The release takes the mappings down in the same order.  The table never
 * changes.
 */
static const uint64_t i915_fhd_fixture_va[I915_TEST_FHD_FIXTURE_MAPS] = {
	I915_TEST_EU_SHARED_VA,
	I915_TEST_EU_BATCH_VA,
	I915_TEX_FIXTURE_TEX_VA,
};

static int i915_fhd_map(struct i915_test_fhd_render *x, struct i915_gt_mem *gm, struct i915_gt_ppgtt *vm);
static void i915_fhd_check_rt_walk(struct i915_test_fhd_render *x, struct i915_gt_ppgtt *vm);
static int i915_fhd_upload(struct i915_test_fhd_render *x, uint8_t *pattern);
static int i915_fhd_submit(struct i915_test_fhd_render *x, struct i915_gt_engine *ge, struct i915_execlists *el, struct i915_gt_ppgtt *vm, struct i915_gt_mem *gm, struct i915_mmio *m);
static void i915_fhd_compare(struct i915_test_fhd_render *x, const uint8_t *pattern);
static void i915_fhd_check_texture(struct i915_test_fhd_render *x, const uint8_t *pattern);
static int i915_fhd_passed(const struct i915_test_fhd_render *x);
static unsigned i915_fhd_probe_page(unsigned pages, unsigned probe);
static int i915_fhd_walk_page(struct i915_gt_ppgtt *vm, struct i915_gt_object *rt, uint64_t va, unsigned page, uint64_t *dma);
static int i915_fhd_va_is_scratch(struct i915_gt_ppgtt *vm, uint64_t va);
static unsigned i915_fhd_map_count(const struct i915_test_fhd_render *x);
static uint64_t i915_fhd_map_va(const struct i915_test_fhd_render *x, unsigned index);
static void i915_fhd_keep_context(struct i915_gt_context *ce);

/*
 * Reports the address layout of the full-HD draw.
 *
 * Either output may be NULL.  Returns 0 when every range is aligned and not
 * empty and no two ranges overlap, EINVAL otherwise.  Nothing here touches
 * the hardware.
 */
int
drv_i915_test_fhd_va_layout(
	const struct i915_test_fhd_va **layout,
	unsigned *count)
{
	unsigned entries;
	unsigned first;
	unsigned second;

	/* Hands out the table and its length. */
	entries = (unsigned)(sizeof(i915_fhd_va) / sizeof(i915_fhd_va[0]));
	if (layout != NULL)
		*layout = i915_fhd_va;
	if (count != NULL)
		*count = entries;

	/* Checks every range, and every later range against it. */
	for (first = 0U; first < entries; first++) {
		/* Refuses a range that does not start on its alignment. */
		if ((i915_fhd_va[first].va & (i915_fhd_va[first].align - 1U)) != 0U)
			return EINVAL;

		/* Refuses an empty range. */
		if (i915_fhd_va[first].len == 0U)
			return EINVAL;

		/* Refuses two ranges that share an address. */
		for (second = first + 1U; second < entries; second++) {
			if (i915_fhd_va[first].va < i915_fhd_va[second].va + i915_fhd_va[second].len &&
			    i915_fhd_va[second].va < i915_fhd_va[first].va + i915_fhd_va[first].len)
				return EINVAL;
		}
	}

	/* Succeeded: the layout is aligned and free of overlaps. */
	return 0;
}

/*
 * Counts the wrong pixels of a target against the expected image.
 *
 * The expected image is that of x's texture variant, or of variant 0 when x
 * is NULL.  The target is only read.
 */
uint32_t
drv_i915_test_fhd_render_verify(
	const struct i915_test_fhd_render *x,
	const uint32_t *pixels,
	uint32_t pitch_bytes)
{
	uint8_t pattern[I915_TEX_FIXTURE_TEX_BYTES];
	uint32_t row_expected[I915_TEST_FHD_TEXEL_COLUMNS];
	const uint32_t *row_pixels;
	unsigned variant;
	unsigned texel;
	uint32_t bad;
	uint32_t column;
	uint32_t row;

	/* Builds the texture the draw sampled. */
	variant = 0U;
	if (x != NULL)
		variant = x->variant;
	drv_i915_tex_fixture_pattern(pattern, variant);

	/* Compares every row against the expected colour of each texel column. */
	bad = 0U;
	for (row = 0U; row < I915_TEX_FHD_HEIGHT; row++) {
		row_pixels = (const uint32_t *)(const void *)((const uint8_t *)pixels + (uint64_t)row * pitch_bytes);

		/* Works out the colour of each texel column of this row. */
		for (texel = 0U; texel < I915_TEST_FHD_TEXEL_COLUMNS; texel++)
			row_expected[texel] = drv_i915_tex_fixture_expected_pixel(pattern, 4U * texel, 4U * (row / I915_TEST_FHD_CELL_HEIGHT));

		/* Counts every pixel that differs from its column's colour. */
		for (column = 0U; column < I915_TEX_FHD_WIDTH; column++) {
			if (row_pixels[column] != row_expected[column / I915_TEST_FHD_CELL_WIDTH])
				bad++;
		}
	}

	/* Reports how many pixels are wrong. */
	return bad;
}

/*
 * Draws the full-HD image into the caller's target at the first target address.
 *
 * The same as drv_i915_test_fhd_render_run_ex() with I915_TEX_FHD_RT_VA,
 * texture variant 0 and a target this draw maps itself.
 */
int
drv_i915_test_fhd_render_run(
	struct i915_test_fhd_render *x,
	struct i915_gt_engines *es,
	struct i915_gt_ppgtt *vm,
	struct i915_gt_mem *gm,
	struct i915_mmio *m,
	struct spinlock *uncore_lock,
	unsigned timeout_ms,
	struct i915_gt_object *rt)
{
	int error;

	/* Runs the draw with the default target address, texture and mapping. */
	error = drv_i915_test_fhd_render_run_ex(x, es, vm, gm, m, uncore_lock, timeout_ms, rt, I915_TEX_FHD_RT_VA, 0U, 0);
	if (error != 0)
		return error;

	/* Succeeded: the target holds the expected image. */
	return 0;
}

/*
 * Draws the full-HD image into the caller's target.
 *
 * The target sits at rt_va, which must be I915_TEX_FHD_RT_VA or
 * I915_TEX_FHD_RT_B_VA, and the texture is variant.  Without rt_premapped
 * this draw maps every page of the target itself; with it the pages belong
 * to a struct i915_test_fhd_rt_map and only the walk is checked.  The CPU
 * pre-fills and publishes the target, the GPU draws, and after the request
 * retired and the CPU view was invalidated the target is compared pixel by
 * pixel.  Returns 0 when image, markers and texture guard all match.  After a
 * hang the engine is reset and gpu_done stays 0, so the caller keeps the
 * target.
 */
int
drv_i915_test_fhd_render_run_ex(
	struct i915_test_fhd_render *x,
	struct i915_gt_engines *es,
	struct i915_gt_ppgtt *vm,
	struct i915_gt_mem *gm,
	struct i915_mmio *m,
	struct spinlock *uncore_lock,
	unsigned timeout_ms,
	struct i915_gt_object *rt,
	uint64_t rt_va,
	unsigned variant,
	int rt_premapped)
{
	uint8_t pattern[I915_TEX_FIXTURE_TEX_BYTES];
	struct i915_test_eu *t;
	struct i915_gt_engine *ge;
	struct i915_execlists *el;
	volatile uint32_t *markers;
	unsigned index;
	int passed;
	int layout;
	int rc;

	/* Refuses a call without the GT pieces or with a target too small for the image. */
	if (x == NULL)
		return EINVAL;
	if (es == NULL)
		return EINVAL;
	if (vm == NULL)
		return EINVAL;
	if (gm == NULL)
		return EINVAL;
	if (m == NULL)
		return EINVAL;
	if (rt == NULL)
		return EINVAL;
	if (rt->bytes < I915_TEX_FHD_RT_BYTES)
		return EINVAL;

	/* Starts a fresh record that borrows the target; no wrong pixel is known yet. */
	kern_memset(x, 0, sizeof(*x));
	t = &x->t;
	x->rt = rt;
	x->first_bad_x = -1;
	x->first_bad_y = -1;
	x->rt_va = rt_va;
	x->variant = variant;
	x->rt_premapped = rt_premapped;

	/* Refuses a target address outside the fixed layout. */
	if (rt_va != I915_TEX_FHD_RT_VA && rt_va != I915_TEX_FHD_RT_B_VA) {
		(void)drv_i915_test_eu_fail(t, EINVAL, "render target VA not in the layout");
		return EINVAL;
	}

	/* Refuses a layout whose ranges overlap. */
	layout = drv_i915_test_fhd_va_layout(NULL, NULL);
	if (layout != 0) {
		(void)drv_i915_test_eu_fail(t, EINVAL, "va layout overlaps");
		return EINVAL;
	}

	/* Finds the render engine, whose context and ring the draw runs on. */
	for (index = 0U; index < es->n; index++) {
		if (es->ge[index].info->class == I915_RENDER_CLASS)
			break;
	}

	/* Refuses a GT without a render engine. */
	if (index == es->n) {
		(void)drv_i915_test_eu_fail(t, ENODEV, "no render engine");
		return ENODEV;
	}

	t->engine_idx = index;
	ge = &es->ge[index];
	el = &es->el[index];

	/* Allocates the state page. */
	t->shared = drv_i915_gt_object_create(gm, 4096U);
	if (t->shared == NULL) {
		(void)drv_i915_test_eu_fail(t, ENOMEM, "gem_create");
		return ENOMEM;
	}

	/* Allocates the batch. */
	t->batch = drv_i915_gt_object_create(gm, 4096U);
	if (t->batch == NULL) {
		(void)drv_i915_test_eu_fail(t, ENOMEM, "gem_create");
		return ENOMEM;
	}

	/* Allocates the texture and its guard. */
	x->tex = drv_i915_gt_object_create(gm, 4096U);
	if (x->tex == NULL) {
		(void)drv_i915_test_eu_fail(t, ENOMEM, "gem_create");
		return ENOMEM;
	}

	/* Maps the draw's objects and, unless premapped, the target. */
	rc = i915_fhd_map(x, gm, vm);
	if (rc != 0)
		return rc;

	/* Checks that the GPU will walk to the target's own pages. */
	i915_fhd_check_rt_walk(x, vm);
	if (!x->rt_walk_ok) {
		(void)drv_i915_test_eu_fail(t, EFAULT, "render target PPGTT walk");
		return EFAULT;
	}

	/* Uploads the texture, the pre-filled target, the state page and the batch. */
	rc = i915_fhd_upload(x, pattern);
	if (rc != 0)
		return rc;

	/* Creates the context and submits the draw's request. */
	rc = i915_fhd_submit(x, ge, el, vm, gm, m);
	if (rc != 0)
		return rc;

	/* Waits for the request to retire; a timeout is a hang, anything else an error. */
	rc = drv_i915_test_eu_wait_retired(t, ge, el, &t->rq, m, timeout_ms);
	if (rc == 0) {
		t->completed = 1;
	} else if (rc == ETIMEDOUT) {
		t->timed_out = 1;
	} else {
		(void)drv_i915_test_eu_fail(t, rc, "i915_request_wait");
	}

	/* Reads the markers the batch and the pixel shader stored. */
	markers = (volatile uint32_t *)t->shared->cpu + I915_DRAW_FIXTURE_MARKER_OFFSET / 4U;
	x->marker_before = markers[0];
	x->marker_after = markers[1];
	x->marker_middraw = markers[2];
	x->ps_marker = markers[4];

	/*
	 * Records a request that did not retire and resets the engine.
	 * gpu_done stays 0: after a hang the GPU is not shown to have let go of
	 * the target, so the caller keeps it.
	 */
	if (!t->completed) {
		if (t->timed_out)
			t->outcome = I915_TEST_EU_HANG;
		drv_i915_test_eu_log_record(t, ge, el, &t->rq, &t->ce, "hang");
		drv_i915_test_eu_hang_dump_reset(t, es, ge, el, m, uncore_lock);

		/* Reports the first error, or the hang when nothing else failed. */
		if (t->err != 0)
			return t->err;
		return ETIMEDOUT;
	}

	/* Records the retired request and switches the engine back to the kernel context. */
	drv_i915_test_eu_log_record(t, ge, el, &t->rq, &t->ce, "completed");
	t->parked = drv_i915_test_eu_park(t, es, ge, el, m, timeout_ms);

	/* A parked engine no longer uses the target, which the release relies on. */
	x->gpu_done = t->parked;

	/* Drops any line the CPU may hold for the target before reading what the GPU wrote. */
	drv_i915_gt_clflush(rt->cpu, I915_TEX_FHD_RT_BYTES);

	/* Compares the target with the expected image and hashes it. */
	i915_fhd_compare(x, pattern);
	x->image_hash = drv_i915_test_fnv1a64(rt->cpu, I915_TEX_FHD_RT_BYTES, I915_TEST_FNV_BASIS);

	/* Checks that the GPU left the texture and its guard alone. */
	i915_fhd_check_texture(x, pattern);

	/* Decides the outcome from the markers, the pixels and the guard. */
	passed = i915_fhd_passed(x);
	t->outcome = I915_TEST_EU_ERROR;
	if (passed)
		t->outcome = I915_TEST_EU_PASS;

	/* Names a failed comparison when nothing else failed first. */
	if (t->outcome == I915_TEST_EU_ERROR && t->err == 0)
		(void)drv_i915_test_eu_fail(t, EIO, "markers/pixels/guard");

	/* Reports the first error of the run. */
	if (t->err != 0)
		return t->err;

	/* Succeeded: the target holds the expected image. */
	return 0;
}

/*
 * Releases the draw's mappings, the TLB and then its own objects.
 *
 * Every mapping the draw wrote goes back to scratch and is re-read from the
 * tables; the count is this call's verification, never a sum.  Only when all
 * of them are at scratch is the GT TLB invalidated, and only after that are
 * the draw's own objects freed.  Returns EBUSY when the GPU is not shown to
 * be done (nothing is touched), EIO when a mapping still names a page, or the
 * TLB invalidation's error; ownership stays until a call returns 0.  The
 * render target is never freed here.
 */
int
drv_i915_test_fhd_render_release(
	struct i915_test_fhd_render *x,
	struct i915_gt_mem *gm,
	struct i915_gt_ppgtt *vm,
	struct i915_gt_tlb *tlb,
	struct i915_gt_engines *es,
	struct i915_mmio *m,
	struct spinlock *uncore_lock)
{
	unsigned count;
	unsigned index;
	uint64_t va;
	int scratch;

	/* Refuses a call without the GT pieces. */
	if (x == NULL)
		return EINVAL;
	if (gm == NULL)
		return EINVAL;
	if (vm == NULL)
		return EINVAL;
	if (tlb == NULL)
		return EINVAL;
	if (es == NULL)
		return EINVAL;
	if (m == NULL)
		return EINVAL;

	/* A draw already released has nothing left to give back. */
	if (x->released)
		return 0;

	x->release_calls++;

	/* The GPU may still use the target and the state: nothing is released. */
	if (!x->gpu_done && x->t.submitted)
		return EBUSY;

	/* Puts every mapping the draw wrote back to scratch; the re-read below decides. */
	count = i915_fhd_map_count(x);
	for (index = 0U; index < count; index++) {
		va = i915_fhd_map_va(x, index);
		(void)drv_i915_gt_ppgtt_insert_scratch(vm, va);
	}

	/* Re-reads every mapping from the tables and counts those at scratch afresh. */
	x->maps_total = count;
	x->maps_scratch = 0U;
	x->first_unreleased_va = 0U;
	for (index = 0U; index < count; index++) {
		va = i915_fhd_map_va(x, index);
		scratch = i915_fhd_va_is_scratch(vm, va);
		if (scratch) {
			x->maps_scratch++;
		} else if (x->first_unreleased_va == 0U) {
			x->first_unreleased_va = va;
		}
	}

	/* The target's pages are what is at scratch beyond the three fixture objects. */
	x->rt_pages_cleared = 0U;
	if (x->maps_scratch >= I915_TEST_FHD_FIXTURE_MAPS)
		x->rt_pages_cleared = x->maps_scratch - I915_TEST_FHD_FIXTURE_MAPS;

	/* Some mapping still names a page: nothing is freed and ownership stays. */
	if (x->maps_scratch != count)
		return EIO;

	/* Invalidates the TLB before any page these mappings named may be reused. */
	x->tlb_rc = drv_i915_gt_invalidate_tlb_full(tlb, es, m, uncore_lock);
	if (x->tlb_rc != 0)
		return x->tlb_rc;

	/* Frees the texture, then the request path's objects. */
	if (x->tex != NULL) {
		drv_i915_gt_object_destroy(gm, x->tex);
		x->tex = NULL;
	}
	drv_i915_test_eu_release(&x->t, gm);

	/* The target goes back to its owner, who may now free it. */
	x->rt = NULL;
	x->released = 1;

	/* Succeeded: mappings, TLB and the draw's own objects are all gone. */
	return 0;
}

/*
 * Keeps every object the draw's request may use.
 *
 * Used after a GPU hang that was not shown to be over: the objects are never
 * destroyed or unbound again, and leaking them is the safe side.
 */
void
drv_i915_test_fhd_render_keep(
	struct i915_test_fhd_render *x)
{
	/* Nothing to keep without a draw. */
	if (x == NULL)
		return;

	/* Marks the texture, the state page, the batch and the timeline page as kept. */
	if (x->tex != NULL)
		x->tex->keep = 1;
	if (x->t.shared != NULL)
		x->t.shared->keep = 1;
	if (x->t.batch != NULL)
		x->t.batch->keep = 1;
	if (x->t.tl_page != NULL)
		x->t.tl_page->keep = 1;

	/* Marks the target as kept, which its owner then cannot free. */
	if (x->rt != NULL)
		x->rt->keep = 1;

	/* Keeps the context image and its ring. */
	i915_fhd_keep_context(&x->t.ce);
}

/*
 * Maps a render target for many draws.
 *
 * Every page of the object's own backing is inserted at va, which must be
 * I915_TEX_FHD_RT_VA or I915_TEX_FHD_RT_B_VA, and the walk of the first, a
 * middle and the last page is checked.  mapped counts what the unmap must
 * take down, even after a partial map.  Returns 0, EINVAL, the mapping error,
 * or EFAULT when the walk does not reach the object's own pages.
 */
int
drv_i915_test_fhd_rt_map(
	struct i915_test_fhd_rt_map *b,
	struct i915_gt_mem *gm,
	struct i915_gt_ppgtt *vm,
	struct i915_gt_object *rt,
	uint64_t va)
{
	unsigned page;
	unsigned probe;
	uint64_t dma;
	int matched;
	int rc;

	/* Refuses a call without the GT pieces or at an address outside the layout. */
	if (b == NULL)
		return EINVAL;
	if (gm == NULL)
		return EINVAL;
	if (vm == NULL)
		return EINVAL;
	if (rt == NULL)
		return EINVAL;
	if (va != I915_TEX_FHD_RT_VA && va != I915_TEX_FHD_RT_B_VA)
		return EINVAL;

	/* Starts a fresh map covering the image, or the whole object when it is shorter. */
	kern_memset(b, 0, sizeof(*b));
	b->rt = rt;
	b->va = va;
	b->pages = I915_TEST_FHD_RT_PAGES;
	if (b->pages > rt->pages)
		b->pages = rt->pages;

	/* Allocates the page tables of the range. */
	rc = drv_i915_gt_ppgtt_alloc_range(gm, vm, va, (uint64_t)b->pages * I915_TEST_FHD_PAGE_BYTES);
	if (rc != 0)
		return rc;

	/* Inserts every page of the object's own backing, counting what the unmap must take down. */
	for (page = 0U; page < b->pages; page++) {
		rc = drv_i915_gt_object_page_dma(rt, page, &dma);
		if (rc != 0)
			break;

		rc = drv_i915_gt_ppgtt_insert_page(vm, dma, va + (uint64_t)page * I915_TEST_FHD_PAGE_BYTES, 0U);
		if (rc != 0)
			break;

		b->mapped++;
	}

	/* Reports a page that could not be mapped. */
	if (rc != 0)
		return rc;

	/* Checks that the first, a middle and the last page walk to the object's own page. */
	b->walk_ok = 1;
	dma = 0U;
	for (probe = 0U; probe < I915_TEST_FHD_WALK_PROBES; probe++) {
		page = i915_fhd_probe_page(b->pages, probe);
		matched = i915_fhd_walk_page(vm, rt, va, page, &dma);
		if (!matched)
			b->walk_ok = 0;
	}

	/* Refuses a map the GPU would not walk to the object's pages. */
	if (!b->walk_ok)
		return EFAULT;

	/* Succeeded: every page of the target is mapped at va. */
	return 0;
}

/*
 * Takes down a map made by drv_i915_test_fhd_rt_map().
 *
 * Every mapping goes back to scratch and is re-read from the tables on each
 * call (never summed); only then is the GT TLB invalidated.  Returns EIO
 * while a mapping still names a page, the TLB invalidation's error, or 0
 * once released is set and the owner may free the object.  The caller must
 * have shown the GPU done with every draw that used the target.
 */
int
drv_i915_test_fhd_rt_unmap(
	struct i915_test_fhd_rt_map *b,
	struct i915_gt_ppgtt *vm,
	struct i915_gt_tlb *tlb,
	struct i915_gt_engines *es,
	struct i915_mmio *m,
	struct spinlock *uncore_lock)
{
	unsigned page;
	uint64_t va;
	int scratch;

	/* Refuses a call without the GT pieces. */
	if (b == NULL)
		return EINVAL;
	if (vm == NULL)
		return EINVAL;
	if (tlb == NULL)
		return EINVAL;
	if (es == NULL)
		return EINVAL;
	if (m == NULL)
		return EINVAL;

	/* A map already taken down has nothing left to undo. */
	if (b->released)
		return 0;

	b->unmap_calls++;

	/* Puts every mapped page back to scratch; the re-read below decides. */
	for (page = 0U; page < b->mapped; page++)
		(void)drv_i915_gt_ppgtt_insert_scratch(vm, b->va + (uint64_t)page * I915_TEST_FHD_PAGE_BYTES);

	/* Re-reads every mapping from the tables and counts those at scratch afresh. */
	b->scratch = 0U;
	b->first_unreleased_va = 0U;
	for (page = 0U; page < b->mapped; page++) {
		va = b->va + (uint64_t)page * I915_TEST_FHD_PAGE_BYTES;
		scratch = i915_fhd_va_is_scratch(vm, va);
		if (scratch) {
			b->scratch++;
		} else if (b->first_unreleased_va == 0U) {
			b->first_unreleased_va = va;
		}
	}

	/* Some mapping still names a page: the owner must not free the object. */
	if (b->scratch != b->mapped)
		return EIO;

	/* Invalidates the TLB before any page these mappings named may be reused. */
	b->tlb_rc = drv_i915_gt_invalidate_tlb_full(tlb, es, m, uncore_lock);
	if (b->tlb_rc != 0)
		return b->tlb_rc;

	/* The target goes back to its owner, who may now free it. */
	b->released = 1;
	b->rt = NULL;

	/* Succeeded: no mapping and no translation of the target is left. */
	return 0;
}

/* Maps the state page, batch and texture and, unless premapped, every page of the target. */
static int
i915_fhd_map(
	struct i915_test_fhd_render *x,
	struct i915_gt_mem *gm,
	struct i915_gt_ppgtt *vm)
{
	struct i915_gt_object *objects[I915_TEST_FHD_FIXTURE_MAPS];
	uint64_t dma;
	unsigned index;
	unsigned page;
	int rc;

	/* Allocates the page tables of the fixture range. */
	rc = drv_i915_gt_ppgtt_alloc_range(gm, vm, I915_TEST_EU_SHARED_VA, (uint64_t)I915_TEST_FHD_FIXTURE_RANGE_PAGES * I915_TEST_FHD_PAGE_BYTES);
	if (rc != 0) {
		(void)drv_i915_test_eu_fail(&x->t, rc, "allocate_va_range");
		return rc;
	}

	/* Allocates the page tables of the target's range unless its map already did. */
	if (!x->rt_premapped) {
		rc = drv_i915_gt_ppgtt_alloc_range(gm, vm, x->rt_va, (uint64_t)x->rt->pages * I915_TEST_FHD_PAGE_BYTES);
		if (rc != 0) {
			(void)drv_i915_test_eu_fail(&x->t, rc, "allocate_va_range");
			return rc;
		}
	}

	/* Inserts the first page of each fixture object at its fixed address. */
	objects[0] = x->t.shared;
	objects[1] = x->t.batch;
	objects[2] = x->tex;
	for (index = 0U; index < I915_TEST_FHD_FIXTURE_MAPS; index++) {
		rc = drv_i915_gt_object_page_dma(objects[index], 0U, &dma);
		if (rc != 0)
			break;

		rc = drv_i915_gt_ppgtt_insert_page(vm, dma, i915_fhd_fixture_va[index], 0U);
		if (rc != 0)
			break;
	}

	/* Inserts every page of the scanout buffer's own backing, counting what the release must take down. */
	x->rt_pages = I915_TEST_FHD_RT_PAGES;
	if (rc == 0 && !x->rt_premapped) {
		for (page = 0U; page < x->rt_pages; page++) {
			rc = drv_i915_gt_object_page_dma(x->rt, page, &dma);
			if (rc != 0)
				break;

			rc = drv_i915_gt_ppgtt_insert_page(vm, dma, x->rt_va + (uint64_t)page * I915_TEST_FHD_PAGE_BYTES, 0U);
			if (rc != 0)
				break;

			x->rt_pages_mapped++;
		}
	}

	/* Reports a page that could not be mapped. */
	if (rc != 0) {
		(void)drv_i915_test_eu_fail(&x->t, rc, "ppgtt_insert");
		return rc;
	}

	/* Succeeded: the draw's objects and its target are mapped. */
	return 0;
}

/* Checks that the first, a middle and the last target page walk to the object's own page. */
static void
i915_fhd_check_rt_walk(
	struct i915_test_fhd_render *x,
	struct i915_gt_ppgtt *vm)
{
	unsigned probe;
	unsigned page;
	uint64_t dma;
	int matched;

	/* Walks each probe page and records the DMA addresses of the first and the last. */
	x->rt_walk_ok = 1;
	dma = 0U;
	for (probe = 0U; probe < I915_TEST_FHD_WALK_PROBES; probe++) {
		page = i915_fhd_probe_page(x->rt_pages, probe);
		matched = i915_fhd_walk_page(vm, x->rt, x->rt_va, page, &dma);
		if (!matched)
			x->rt_walk_ok = 0;
		if (probe == 0U)
			x->rt_first_dma = dma;
		if (probe == I915_TEST_FHD_WALK_PROBES - 1U)
			x->rt_last_dma = dma;
	}
}

/* Uploads the texture, the pre-filled target, the state page and the batch for the GPU. */
static int
i915_fhd_upload(
	struct i915_test_fhd_render *x,
	uint8_t *pattern)
{
	struct i915_test_eu *t;
	volatile uint8_t *texture;
	uint32_t *target;
	const uint32_t *state;
	unsigned index;

	t = &x->t;

	/* Picks the MOCS index and builds the texture of the requested variant. */
	x->mocs = drv_i915_draw_fixture_mocs();
	drv_i915_tex_fixture_pattern(pattern, x->variant);

	/* Writes the texture, followed by the guard the GPU must leave alone. */
	texture = (volatile uint8_t *)x->tex->cpu;
	for (index = 0U; index < 4096U; index++) {
		if (index < I915_TEX_FIXTURE_TEX_BYTES) {
			texture[index] = pattern[index];
		} else {
			texture[index] = (uint8_t)I915_TEST_TEX_GUARD_BYTE;
		}
	}

	/* Pre-fills the target and publishes it to the GPU. */
	target = (uint32_t *)x->rt->cpu;
	for (index = 0U; index < I915_TEX_FHD_RT_BYTES / 4U; index++)
		target[index] = I915_TEST_FHD_PREFILL;
	drv_i915_gt_clflush(x->rt->cpu, I915_TEX_FHD_RT_BYTES);

	/* Writes the state page and reads back the MOCS the target's surface state carries. */
	drv_i915_tex_fixture_fhd_write_state(t->shared->cpu, x->rt_va, I915_TEX_FIXTURE_TEX_VA, x->mocs);
	state = (const uint32_t *)t->shared->cpu;
	x->rt_rss_mocs = (state[64U / 4U + 1U] >> 24) & 0x7fU;
	if (x->rt_rss_mocs != x->mocs) {
		(void)drv_i915_test_eu_fail(t, EINVAL, "render target surface state MOCS");
		return EINVAL;
	}

	/* Builds the batch, which must fit in its page. */
	t->batch_dwords = drv_i915_tex_fixture_fhd_build_batch((uint32_t *)t->batch->cpu, I915_TEST_FHD_BATCH_DWORDS, I915_TEST_EU_SHARED_VA, x->mocs);
	if (t->batch_dwords == 0U || t->batch_dwords >= I915_TEST_FHD_BATCH_DWORDS) {
		(void)drv_i915_test_eu_fail(t, ENOSPC, "build_batch");
		return ENOSPC;
	}

	/* Checks the PIPELINE_SELECT words of the batch as it will be submitted. */
	t->pipesel_rc = drv_i915_test_draw_check_pipeline_select((const uint32_t *)t->batch->cpu, t->batch_dwords, &t->pipesel);
	if (t->pipesel_rc != 0) {
		(void)drv_i915_test_eu_fail(t, t->pipesel_rc, "pipeline_select_verify");
		return t->pipesel_rc;
	}

	/* Records the batch's identity. */
	t->batch_hash = drv_i915_test_fnv1a64(t->batch->cpu, (size_t)t->batch_dwords * 4U, I915_TEST_FNV_BASIS);

	/* Succeeded: everything the GPU reads is in place. */
	return 0;
}

/* Creates the draw's context and timeline and submits the request that runs the batch. */
static int
i915_fhd_submit(
	struct i915_test_fhd_render *x,
	struct i915_gt_engine *ge,
	struct i915_execlists *el,
	struct i915_gt_ppgtt *vm,
	struct i915_gt_mem *gm,
	struct i915_mmio *m)
{
	struct i915_test_eu *t;
	int rc;

	t = &x->t;

	/* Creates a fresh context of the GT address space on the render engine. */
	rc = drv_i915_lrc_alloc(&t->ce, ge, vm, gm, 4096U, 0U);
	if (rc != 0) {
		(void)drv_i915_test_eu_fail(t, rc, "intel_context_create");
		return rc;
	}

	/* Allocates the timeline page the request's breadcrumbs land in. */
	t->tl_page = drv_i915_gt_object_create(gm, 4096U);
	if (t->tl_page == NULL) {
		(void)drv_i915_test_eu_fail(t, ENOMEM, "intel_timeline_create");
		return ENOMEM;
	}

	/* Binds the timeline page into the GGTT, where the breadcrumbs address it. */
	rc = drv_i915_gt_ggtt_bind(gm, t->tl_page);
	if (rc != 0) {
		(void)drv_i915_test_eu_fail(t, rc, "intel_timeline_pin");
		return rc;
	}

	/* Lays out the context image and points it at the ring. */
	drv_i915_lrc_init_state(&t->ce);
	(void)drv_i915_lrc_update_regs(&t->ce, t->ce.ring.tail);

	/* Builds the request that starts the batch; the helper records its own failure. */
	t->tl_seqno = 2U;
	rc = drv_i915_test_eu_build_request(t, &t->rq, &t->ce, t->tl_page, t->tl_seqno, I915_TEST_EU_BATCH_VA);
	if (rc != 0)
		return rc;

	/* Submits the request to the render engine. */
	rc = drv_i915_execlists_submit(ge, el, m, &t->rq);
	if (rc != 0) {
		(void)drv_i915_test_eu_fail(t, rc, "execlists_submit");
		return rc;
	}

	/* Marks the request as handed to the GPU, which the release must then wait out. */
	t->submitted = 1;

	/* Succeeded: the draw is on the engine. */
	return 0;
}

/* Compares every pixel of the target with the expected image and remembers the first wrong one. */
static void
i915_fhd_compare(
	struct i915_test_fhd_render *x,
	const uint8_t *pattern)
{
	const uint32_t *pixels;
	uint32_t expected;
	uint32_t observed;
	uint32_t column;
	uint32_t row;

	/* Walks the target row by row. */
	pixels = (const uint32_t *)x->rt->cpu;
	x->px_total = I915_TEX_FHD_WIDTH * I915_TEX_FHD_HEIGHT;
	for (row = 0U; row < I915_TEX_FHD_HEIGHT; row++) {
		for (column = 0U; column < I915_TEX_FHD_WIDTH; column++) {
			expected = drv_i915_tex_fixture_expected_pixel(pattern, 4U * (column / I915_TEST_FHD_CELL_WIDTH), 4U * (row / I915_TEST_FHD_CELL_HEIGHT));
			observed = pixels[row * (I915_TEX_FHD_PITCH / 4U) + column];

			/* Counts a pixel that matches. */
			if (observed == expected) {
				x->px_match++;
				continue;
			}

			/* Counts a pixel the GPU never wrote. */
			if (observed == I915_TEST_FHD_PREFILL)
				x->px_stale++;

			/* Remembers the first wrong pixel. */
			if (x->first_bad_x < 0) {
				x->first_bad_x = (int)column;
				x->first_bad_y = (int)row;
				x->first_bad_expected = expected;
				x->first_bad_observed = observed;
			}
		}
	}
}

/* Counts the texel and guard bytes that differ from what the CPU wrote. */
static void
i915_fhd_check_texture(
	struct i915_test_fhd_render *x,
	const uint8_t *pattern)
{
	volatile uint8_t *texture;
	uint8_t expected;
	unsigned index;

	/* Compares the texture and the guard after it byte by byte. */
	texture = (volatile uint8_t *)x->tex->cpu;
	for (index = 0U; index < 4096U; index++) {
		if (index < I915_TEX_FIXTURE_TEX_BYTES) {
			expected = pattern[index];
		} else {
			expected = (uint8_t)I915_TEST_TEX_GUARD_BYTE;
		}

		/* Sorts a changed byte into the texture or the guard. */
		if (texture[index] != expected) {
			if (index < I915_TEX_FIXTURE_TEX_BYTES) {
				x->tex_changed_bytes++;
			} else {
				x->guard_bad_bytes++;
			}
		}
	}
}

/* Tells whether the engine parked and every marker, pixel and guard byte is as promised. */
static int
i915_fhd_passed(
	const struct i915_test_fhd_render *x)
{
	/* The engine must be back on the kernel context. */
	if (!x->t.parked)
		return 0;

	/* The batch must have run before, during and after the draw. */
	if (x->marker_before != I915_DRAW_FIXTURE_MARKER_BEFORE)
		return 0;
	if (x->marker_middraw != I915_DRAW_FIXTURE_MARKER_MIDDRAW)
		return 0;
	if (x->marker_after != I915_DRAW_FIXTURE_MARKER_AFTER)
		return 0;

	/* The pixel shader must have run. */
	if (x->ps_marker != I915_DRAW_FIXTURE_PS_MARKER)
		return 0;

	/* Every pixel must match. */
	if (x->px_match != x->px_total)
		return 0;

	/* The texture and its guard must be untouched. */
	if (x->tex_changed_bytes != 0U)
		return 0;
	if (x->guard_bad_bytes != 0U)
		return 0;

	/* The draw did everything the fixture promises. */
	return 1;
}

/* Names the target page of one walk probe: the first, the middle or the last. */
static unsigned
i915_fhd_probe_page(
	unsigned pages,
	unsigned probe)
{
	/* The first probe is the first page. */
	if (probe == 0U)
		return 0U;

	/* The second probe is the middle page. */
	if (probe == 1U)
		return pages / 2U;

	/* The last probe is the last page. */
	return pages - 1U;
}

/* Tells whether one target page walks to the object's own page, reporting that page's DMA address. */
static int
i915_fhd_walk_page(
	struct i915_gt_ppgtt *vm,
	struct i915_gt_object *rt,
	uint64_t va,
	unsigned page,
	uint64_t *dma)
{
	struct i915_test_ppgtt_walk walk;
	int rc;

	/* Looks up the DMA address the page really has. */
	rc = drv_i915_gt_object_page_dma(rt, page, dma);
	if (rc != 0)
		return 0;

	/* Walks the tables the way the GPU will. */
	rc = drv_i915_test_ppgtt_walk(vm, va + (uint64_t)page * I915_TEST_FHD_PAGE_BYTES, &walk);
	if (rc != 0)
		return 0;

	/* The walk must reach a present page entry. */
	if (walk.levels != 4)
		return 0;
	if (!walk.leaf_present)
		return 0;

	/* The page entry must name the object's own page. */
	if (walk.leaf_dma != (*dma & ~0xfffULL))
		return 0;

	/* The GPU reaches the object's own page. */
	return 1;
}

/* Tells whether the tables map an address to scratch. */
static int
i915_fhd_va_is_scratch(
	struct i915_gt_ppgtt *vm,
	uint64_t va)
{
	struct i915_test_ppgtt_walk walk;
	int rc;

	/* Walks the tables the way the GPU will. */
	rc = drv_i915_test_ppgtt_walk(vm, va, &walk);
	if (rc != 0)
		return 0;

	/* The walk must reach the page entry, and that entry must be the scratch encoding. */
	if (walk.levels != 4)
		return 0;
	if (!walk.scratch[3])
		return 0;

	/* The address no longer names a page. */
	return 1;
}

/* Counts the mappings the draw wrote: the fixture objects and the target pages it mapped itself. */
static unsigned
i915_fhd_map_count(
	const struct i915_test_fhd_render *x)
{
	unsigned count;

	/* A premapped target adds nothing: its pages belong to its map. */
	count = I915_TEST_FHD_FIXTURE_MAPS + x->rt_pages_mapped;
	if (count > I915_TEST_FHD_MAPS_MAX)
		count = I915_TEST_FHD_MAPS_MAX;

	/* Reports how many mappings the release must take down. */
	return count;
}

/* Names one mapping the draw wrote, in the order it wrote them. */
static uint64_t
i915_fhd_map_va(
	const struct i915_test_fhd_render *x,
	unsigned index)
{
	/* The fixture objects come first. */
	if (index < I915_TEST_FHD_FIXTURE_MAPS)
		return i915_fhd_fixture_va[index];

	/* The target's pages follow in page order. */
	return x->rt_va + (uint64_t)(index - I915_TEST_FHD_FIXTURE_MAPS) * I915_TEST_FHD_PAGE_BYTES;
}

/* Keeps a context's image and ring, which a hung engine may still read. */
static void
i915_fhd_keep_context(
	struct i915_gt_context *ce)
{
	/* Marks the ring as kept. */
	if (ce->ring.obj != NULL)
		ce->ring.obj->keep = 1;

	/* Marks the context image as kept. */
	if (ce->state != NULL)
		ce->state->keep = 1;
}
