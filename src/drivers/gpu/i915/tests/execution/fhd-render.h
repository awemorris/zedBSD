/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The full-HD textured draw into a buffer the display tests own.
 *
 * The draw maps the caller's object (the scanout buffer) into the GT address
 * space page by page, so the GPU writes the same backing pages the display
 * binding of the object shows.  The CPU pre-fills and publishes the target,
 * the GPU draws the fixed textured image over all of it, and after the
 * request retired and the CPU view was invalidated the target is compared
 * pixel by pixel with the expected image.  The CPU never writes the target
 * after the draw, and the draw never frees it.
 *
 * The address layout is fixed: the state page at 0x100400000, the batch at
 * 0x100401000, the texture at 0x100404000, the render target at
 * I915_TEX_FHD_RT_VA and a second render target at I915_TEX_FHD_RT_B_VA.
 * drv_i915_test_fhd_va_layout() in eu-test.h reports the table and checks
 * that no two ranges overlap.
 */

#ifndef DRIVERS_GPU_I915_TESTS_EXECUTION_FHD_RENDER_H
#define DRIVERS_GPU_I915_TESTS_EXECUTION_FHD_RENDER_H

#include "eu-internal.h"

#include <stdint.h>

struct i915_gt_engines;
struct i915_gt_mem;
struct i915_gt_object;
struct i915_gt_ppgtt;
struct i915_gt_tlb;
struct i915_mmio;
struct spinlock;

/*
 * One full-HD draw into an object the caller owns.
 *
 * It holds the request path, the texture, and what the draw found; the
 * render target is only borrowed.  An instance lives from the run until its
 * release returned 0, or for ever once it was kept after a hang.  The release
 * re-reads the page tables on every call and gives up nothing until every
 * mapping it wrote is back at scratch and the TLB was invalidated.
 */
struct i915_test_fhd_render {
	/* The request path: context, request, state page, batch and the hang record. */
	struct i915_test_eu t;

	/* The texture, owned by the draw, and the render target, which the caller owns. */
	struct i915_gt_object *tex;
	struct i915_gt_object *rt;

	/* The MOCS index the fixture uses, and the one the render target's surface state carries. */
	uint32_t mocs;
	uint32_t rt_rss_mocs;

	/* The markers the batch and the pixel shader store in the state page. */
	uint32_t marker_before;
	uint32_t marker_middraw;
	uint32_t marker_after;
	uint32_t ps_marker;

	/* How many pixels matched, how many still held the pre-fill, and how many were compared. */
	unsigned px_match;
	unsigned px_stale;
	unsigned px_total;

	/* The first wrong pixel (-1 when none) with what was expected and what was found. */
	int first_bad_x;
	int first_bad_y;
	uint32_t first_bad_expected;
	uint32_t first_bad_observed;

	/* The texel bytes that differ from what the CPU wrote, and the guard bytes after the image that changed. */
	unsigned tex_changed_bytes;
	unsigned guard_bad_bytes;

	/* How many pages the target spans, how many this draw mapped, and how many the last release found at scratch. */
	unsigned rt_pages;
	unsigned rt_pages_mapped;
	unsigned rt_pages_cleared;

	/* Nonzero when the leaf of the first, a middle and the last page is the object's own page. */
	int rt_walk_ok;

	/* The DMA addresses of the target's first and last page. */
	uint64_t rt_first_dma;
	uint64_t rt_last_dma;

	/* Nonzero once the request retired and the engine parked: the GPU no longer uses the target. */
	int gpu_done;

	/* The mappings this draw wrote, and how many the last release call found back at scratch (never a sum). */
	unsigned maps_total;
	unsigned maps_scratch;

	/* What the TLB invalidation reported, and nonzero once mappings, TLB and own objects are all gone. */
	int tlb_rc;
	int released;

	/* The first mapping that still names a page, 0 when none. */
	uint64_t first_unreleased_va;

	/* How many times the release was called. */
	unsigned release_calls;

	/* FNV-1a 64 over the whole target as the GPU left it. */
	uint64_t image_hash;

	/* Where the target sits in the GT address space. */
	uint64_t rt_va;

	/* The texture variant drawn; the expected image follows it. */
	unsigned variant;

	/* Nonzero when the target's mappings belong to a struct i915_test_fhd_rt_map, not to this draw. */
	int rt_premapped;
};

/*
 * A render target mapped for a whole run.
 *
 * The display test that flips between two buffers maps each of them once, at
 * its own address, and draws into it many times.  The map inserts every page
 * of the object's own backing and checks the walk of the first, a middle and
 * the last page.  The unmap puts every mapping back to scratch, re-reads the
 * tables on each call (never summing), and then invalidates the GT TLB; only
 * then is released set and the owner may free the object.  The caller must
 * have shown the GPU done with every draw that used the target.
 */
struct i915_test_fhd_rt_map {
	/* The object mapped, which the caller owns, and where it is mapped. */
	struct i915_gt_object *rt;
	uint64_t va;

	/* How many pages the map covers, how many were mapped, and how many the last unmap found at scratch. */
	unsigned pages;
	unsigned mapped;
	unsigned scratch;

	/* How many times the unmap was called. */
	unsigned unmap_calls;

	/* Whether the walk matched, what the TLB invalidation reported, and nonzero once everything is undone. */
	int walk_ok;
	int tlb_rc;
	int released;

	/* The first mapping that still names a page, 0 when none. */
	uint64_t first_unreleased_va;
};

/*
 * Draws into rt at I915_TEX_FHD_RT_VA with texture variant 0, mapping the
 * target itself.  Returns 0 when the image, the markers and the texture
 * guard all match; ETIMEDOUT or the first error otherwise.
 */
int drv_i915_test_fhd_render_run(struct i915_test_fhd_render *x, struct i915_gt_engines *es, struct i915_gt_ppgtt *vm, struct i915_gt_mem *gm, struct i915_mmio *m, struct spinlock *uncore_lock, unsigned timeout_ms, struct i915_gt_object *rt);

/*
 * Draws into rt at rt_va (I915_TEX_FHD_RT_VA or I915_TEX_FHD_RT_B_VA) with
 * texture variant.  With rt_premapped the target's pages were inserted by
 * drv_i915_test_fhd_rt_map() and stay mapped after this draw's release; the
 * walk of the first, a middle and the last page is still checked.
 */
int drv_i915_test_fhd_render_run_ex(struct i915_test_fhd_render *x, struct i915_gt_engines *es, struct i915_gt_ppgtt *vm, struct i915_gt_mem *gm, struct i915_mmio *m, struct spinlock *uncore_lock, unsigned timeout_ms, struct i915_gt_object *rt, uint64_t rt_va, unsigned variant, int rt_premapped);

/*
 * Maps rt at va (I915_TEX_FHD_RT_VA or I915_TEX_FHD_RT_B_VA) for many draws.
 * Returns 0, EINVAL, the mapping error, or EFAULT when the walk does not reach
 * the object's own pages.
 */
int drv_i915_test_fhd_rt_map(struct i915_test_fhd_rt_map *b, struct i915_gt_mem *gm, struct i915_gt_ppgtt *vm, struct i915_gt_object *rt, uint64_t va);

/*
 * Takes down a map made by drv_i915_test_fhd_rt_map().  Returns 0 once every
 * mapping is at scratch and the TLB was invalidated (again 0 on later calls),
 * EIO while a mapping still names a page, or the TLB invalidation's error.
 */
int drv_i915_test_fhd_rt_unmap(struct i915_test_fhd_rt_map *b, struct i915_gt_ppgtt *vm, struct i915_gt_tlb *tlb, struct i915_gt_engines *es, struct i915_mmio *m, struct spinlock *uncore_lock);

/*
 * Counts the wrong pixels of a target against the expected image of x's
 * variant (variant 0 when x is NULL); the CPU does not write the target.
 */
uint32_t drv_i915_test_fhd_render_verify(const struct i915_test_fhd_render *x, const uint32_t *pixels, uint32_t pitch_bytes);

/*
 * Releases the draw in the reference's order.
 *
 * With the request already retired, every mapping the draw wrote (state,
 * batch, texture and, unless premapped, the render target) goes back to
 * scratch, the GT TLB is invalidated, and only then are the draw's own
 * objects freed.  It may be called again after a failure: the mappings are
 * re-read and counted afresh each time and ownership stays until everything
 * is done.  EBUSY means the GPU is not shown to be done and nothing was
 * touched; EIO means a mapping still names a page and nothing was freed.  The
 * render target is never freed here: its owner may free it once this
 * returned 0.
 */
int drv_i915_test_fhd_render_release(struct i915_test_fhd_render *x, struct i915_gt_mem *gm, struct i915_gt_ppgtt *vm, struct i915_gt_tlb *tlb, struct i915_gt_engines *es, struct i915_mmio *m, struct spinlock *uncore_lock);

/*
 * Keeps every object the request may use for ever, after a GPU hang that was
 * not shown to be over.
 */
void drv_i915_test_fhd_render_keep(struct i915_test_fhd_render *x);

#endif
