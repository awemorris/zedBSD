/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The migrate context (see migrate.h).
 */

#include "migrate.h"
#include "context.h"
#include "device-info.h"
#include "engine.h"
#include "i915.h"
#include "memory.h"
#include "ppgtt.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <stddef.h>

#include "intel/gt-regs.h"

static void i915_migrate_fail(struct i915_gt_migrate *m, int error, const char *where);
static void i915_migrate_insert_pte(struct i915_gt_ppgtt *pp, struct i915_gt_object *pt, uint64_t pt_dma, void *data);
static int i915_migrate_first_copy_engine(struct i915_gt_engines *es, unsigned *index);
static int i915_migrate_populate(struct i915_gt_migrate *m, struct i915_gt_engine *ge, struct i915_gt_mem *gm);

/*
 * Builds the migrate address space and its pinned context on the first copy
 * engine (intel_migrate_init()).
 *
 * Returns 0, EINVAL, ENODEV when the GT has no copy engine, or the failure of
 * the address space or the context, whose step is in m->err_where; on failure
 * nothing stays allocated.
 */
int
drv_i915_migrate_init(
	struct i915_gt_migrate *m,
	struct i915_gt_engines *es,
	struct i915_gt_mem *gm)
{
	struct i915_gt_engine *ge;
	int found;
	int error;

	/* Refuses a migrate context without the engines or a pool. */
	if (m == NULL ||
	    es == NULL ||
	    gm == NULL)
		return EINVAL;

	/* Starts from an empty migrate state. */
	kern_memset(m, 0, sizeof(*m));

	/* Finds the first copy engine (first_copy_engine()). */
	found = i915_migrate_first_copy_engine(es, &m->engine_idx);
	if (found == 0) {
		i915_migrate_fail(m, ENODEV, "first_copy_engine");
		return ENODEV;
	}

	/* The context goes on that engine. */
	m->has_engine = 1;
	ge = &es->ge[m->engine_idx];

	/* Creates the migrate address space (migrate_vm(): i915_ppgtt_create(gt, I915_BO_ALLOC_PM_EARLY)). */
	error = drv_i915_gt_ppgtt_create(gm, &m->vm);
	if (error != 0) {
		i915_migrate_fail(m, error, "i915_ppgtt_create");
		return error;
	}

	/* Lays out the windows and creates the pinned context; a failure gives the address space back. */
	error = i915_migrate_populate(m, ge, gm);
	if (error != 0) {
		i915_migrate_fail(m, error, "migrate_vm");
		drv_i915_gt_ppgtt_destroy(gm, &m->vm);
		return error;
	}

	/*
	 * The pinned timeline lives in the engine's status page
	 * (intel_timeline_create_from_engine(engine, I915_GEM_HWS_MIGRATE)) and
	 * has no initial breadcrumb.
	 */
	m->hwsp_ggtt = (uint32_t)ge->hwsp_ggtt + I915_GEM_HWS_MIGRATE;
	m->hwsp_cpu = &ge->hwsp[I915_GEM_HWS_MIGRATE / 4U];
	m->tl_seqno = 0U;

	/* Marks the context ready for copies. */
	m->inited = 1;

	/* Succeeded: the migrate context is pinned. */
	return 0;
}

/*
 * Gives back the migrate context and its address space
 * (intel_engine_destroy_pinned_context(), then the address space).
 */
void
drv_i915_migrate_fini(
	struct i915_gt_migrate *m,
	struct i915_gt_mem *gm)
{
	/* Nothing to release without the state or a pool. */
	if (m == NULL || gm == NULL)
		return;

	/* Unpins and releases the context. */
	if (m->ce.allocated != 0)
		drv_i915_lrc_release(&m->ce, gm);

	/* Destroys the address space. */
	drv_i915_gt_ppgtt_destroy(gm, &m->vm);

	/* Marks the migrate state empty. */
	m->inited = 0;
	m->has_engine = 0;
}

/* Records the first failure and the step it came from; a later one is not recorded. */
static void
i915_migrate_fail(
	struct i915_gt_migrate *m,
	int error,
	const char *where)
{
	/* Only the first failure names the step. */
	if (m->err == 0) {
		m->err = error;
		m->err_where = where;
	}
}

/* Maps one page table of the windows itself into the PTE window, uncached (insert_pte()). */
static void
i915_migrate_insert_pte(
	struct i915_gt_ppgtt *pp,
	struct i915_gt_object *pt,
	uint64_t pt_dma,
	void *data)
{
	struct i915_gt_migrate *m;
	uint64_t offset;
	int error;

	UNUSED_PARAMETER(pt);

	/* The next free page of the PTE window takes the table. */
	m = data;
	offset = m->pte_window + (uint64_t)m->exposed_pts * I915_GT_PAGE_BYTES;
	error = drv_i915_gt_ppgtt_insert_page(pp, pt_dma, offset, I915_PAT_INDEX_CACHE_NONE);
	if (error != 0)
		i915_migrate_fail(m, error, "insert_pte");

	/* The window moves on even after a failure, as the reference's d.offset does. */
	m->exposed_pts++;
}

/*
 * Finds the first copy engine: every copy engine supports migration
 * (MI_ARB_ON_OFF, MI_STORE_DATA and the blits).
 */
static int
i915_migrate_first_copy_engine(
	struct i915_gt_engines *es,
	unsigned *index)
{
	unsigned candidate;

	/* Takes the lowest engine of the copy class. */
	for (candidate = 0U; candidate < es->n; candidate++) {
		if (es->ge[candidate].info->class == I915_COPY_ENGINE_CLASS) {
			*index = candidate;
			return 1;
		}
	}

	/* The GT has no copy engine. */
	return 0;
}

/* Lays out the windows, exposes their page tables, and creates the pinned context. */
static int
i915_migrate_populate(
	struct i915_gt_migrate *m,
	struct i915_gt_engine *ge,
	struct i915_gt_mem *gm)
{
	uint64_t base;
	uint64_t size;
	int error;

	/*
	 * Copies go in 8 MiB chunks through a source and a destination window,
	 * and the PTE window follows them with one PTE per window page.  Only
	 * the first copy instance exists, so base is 0.
	 */
	base = 0U;
	size = 2U * (uint64_t)I915_MIGRATE_CHUNK_SZ;
	m->window_bytes = size;
	m->pte_window = base + size;
	size += (size >> 12) * sizeof(uint64_t);

	/* Creates the page tables of the whole range (allocate_va_range(base, sz)). */
	error = drv_i915_gt_ppgtt_alloc_range(gm, &m->vm, base, size);
	if (error != 0)
		return error;

	/* Lets the GPU rewrite the windows' PTEs through its own address space. */
	error = drv_i915_gt_ppgtt_foreach_pt(&m->vm, base, m->pte_window - base, i915_migrate_insert_pte, m);
	if (error == 0 && m->err != 0)
		error = m->err;
	if (error != 0)
		return error;

	/*
	 * Creates the pinned context (intel_engine_create_pinned_context(engine,
	 * vm, SZ_512K, I915_GEM_HWS_MIGRATE, "migrate")).
	 */
	error = drv_i915_lrc_alloc(&m->ce, ge, &m->vm, gm, I915_MIGRATE_RING_BYTES, 0U);
	if (error != 0) {
		i915_migrate_fail(m, error, "intel_engine_create_pinned_context");
		return error;
	}

	/* Pins it: builds the image and writes the empty ring into it. */
	drv_i915_lrc_init_state(&m->ce);
	(void)drv_i915_lrc_update_regs(&m->ce, m->ce.ring.tail);

	/* Succeeded: the windows are mapped and the context is pinned. */
	return 0;
}
