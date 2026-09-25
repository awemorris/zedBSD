/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The driver's memory objects.
 *
 * Two kinds of object live here, and they are kept apart on purpose:
 *
 *   GT objects       the pages the GT itself owns -- status pages, rings,
 *                    context images, page-table pages, the scratch page and
 *                    display buffers.  They come from the kernel DMA device
 *                    (a DMA vector, or one coherent run above the vector
 *                    cap), are handed out from a fixed pool, and are bound
 *                    into the GGTT windows of ggtt.h.
 *   session objects  the storage of GPU sessions.  Each one is a single
 *                    physically contiguous run from the page manager, seen
 *                    through the kernel direct map, and is bound into a
 *                    session's private address space (ppgtt.h).
 *
 * The first stands for Linux's drm_i915_gem_object plus i915_vma for the
 * objects intel_gt_init() creates; the second is the software VM of the
 * sessions.  They are unified later; until then neither borrows the other's
 * allocator.
 */

#ifndef DRIVERS_GPU_I915_MEMORY_H
#define DRIVERS_GPU_I915_MEMORY_H

#include <drivers/generic/dma.h>
#include <kern/pmem.h>
#include <stddef.h>
#include <stdint.h>

struct i915_mmio;
struct i915_ppgtt;

/* The page every GT object and every GT page-table page is made of. */
#define I915_GT_PAGE_BYTES		4096U

/* How many 64-bit entries one table page holds. */
#define I915_GT_PTES_PER_PAGE		(I915_GT_PAGE_BYTES / 8U)

/*
 * The GT window: GGTT pages at the top of the GGTT for the GT's own
 * objects (1 MiB), and the bitmap words that track it.
 */
#define I915_GT_GGTT_PAGES		256U
#define I915_GT_GGTT_WORDS		(I915_GT_GGTT_PAGES / 32U)

/*
 * The display window: GGTT pages for scanout buffers directly below the GT
 * window (32 MiB: two full-HD XRGB8888 buffers with 256 KiB alignment and
 * guards), and the bitmap words that track it.
 */
#define I915_GT_DISPLAY_PAGES		8192U
#define I915_GT_DISPLAY_WORDS		(I915_GT_DISPLAY_PAGES / 32U)

/*
 * How many GT objects the pool holds: a status page, a ring and a context
 * image per engine, the scratch page, the page-table pages, and the
 * objects of the draw paths.  64 ran out once three contexts and four user
 * objects were live together.
 */
#define I915_GT_MAX_OBJECTS		128U

/* The page every session object and session page-table page is made of. */
#define I915_PAGE_BYTES			4096U

/* The largest contiguous session object handed to a session. */
#define I915_MAX_RESOURCE_BYTES		(16U * 1024U * 1024U)

/* The highest physical address the GPU reaches on Alder Lake-P (39 bits, dma_mask_size). */
#define I915_DMA_MAX_ADDRESS		((1ULL << 39) - 1ULL)

/*
 * One GT object: backing pages with a CPU-contiguous view and a DMA
 * mapping, optionally bound into one of the GGTT windows.
 *
 * It is a slot of the pool in struct i915_gt_mem; create claims it and
 * destroy gives it back.  An object marked keep is never destroyed or
 * unbound, because the display may still be reading it.
 */
struct i915_gt_object {
	/* The DMA vector that backs an object up to DRV_DMA_VECTOR_MAX_SIZE. */
	struct drv_dma_vector *vec;

	/*
	 * The one coherent run that backs an object above the vector cap (the
	 * 512 KiB migrate ring).  Either way the pages are page aligned.
	 */
	struct drv_dma_buffer big;

	/* Nonzero when the backing is the coherent run rather than the vector. */
	int contiguous;

	/* The CPU-contiguous view of the backing pages. */
	void *cpu;

	/* The size in bytes and in pages; always whole pages. */
	uint32_t bytes;
	unsigned pages;

	/* The GGTT index of the first page while bound. */
	unsigned ggtt_page;

	/* Nonzero when the binding is in the display window rather than the GT window. */
	int display;

	/* How many scratch guard pages sit on each side of a display binding. */
	unsigned display_guard;

	/*
	 * Nonzero while the display may still read the object: fini, destroy
	 * and the display unbind all refuse it, and leaking is the safe side.
	 */
	int keep;

	/* The GPU address the engines use; valid while bound. */
	uint64_t ggtt_offset;

	/* Nonzero while the object is bound into a GGTT window. */
	int bound;

	/* Nonzero while the pool slot holds an object. */
	int in_use;
};

/*
 * The GT's memory: the object pool and the two GGTT windows.
 *
 * It lives inside the device from the GGTT probe to the device stop and is
 * used only by the device start and the request worker, which never run at
 * once.  The GGTT page table itself is the upper half of BAR0.
 */
struct i915_gt_mem {
	/* The kernel DMA device the backing pages come from, and its address mask. */
	struct drv_dma_device *dma;
	uint64_t dma_mask;

	/* The mapped GGTT page table and how many entries it holds. */
	volatile uint8_t *table;
	unsigned entries;

	/* What a free GGTT entry holds: the scratch page's encoding. */
	uint64_t scratch_pte;

	/* The register block; kept for a flush register the Gen12 flush does not write. */
	struct i915_mmio *m;

	/* How many destroy and unbind requests were refused because the object is kept. */
	unsigned keep_refusals;

	/* The GT window: its first GGTT page, its size, and which pages are taken. */
	unsigned window_first;
	unsigned window_pages;
	uint32_t bitmap[I915_GT_GGTT_WORDS];
	unsigned allocated_pages;

	/* The display window: valid once display_pages is nonzero. */
	unsigned display_first;
	unsigned display_pages;
	uint32_t display_bitmap[I915_GT_DISPLAY_WORDS];
	unsigned display_allocated_pages;
	unsigned display_pte_writes;

	/* How many objects fini had to leave alone because they are kept. */
	unsigned kept_objects;

	/* The object pool and how many of its slots are in use. */
	struct i915_gt_object objects[I915_GT_MAX_OBJECTS];
	unsigned objects_live;

	/* Diagnostics; never a substitute for a return value. */
	unsigned pte_writes;
	unsigned flushes;
	unsigned obj_alloc_fail;
	unsigned ggtt_alloc_fail;

	/* Nonzero between init and fini. */
	int inited;
};

/*
 * One session object: a contiguous run with a kernel CPU view.
 *
 * The registry keeps every live object reachable for detach and reset.  An
 * object destroyed while its context is quarantined stays on the registry
 * until a checked reset proves the GPU no longer names it.
 */
struct i915_gem_object {
	/* The physical run and its size in bytes and pages. */
	struct kern_pmem run;
	uint64_t bytes;
	unsigned pages;

	/* The CPU view through the kernel direct map. */
	void *address;

	/* The GGTT binding; never made any more, but a live one still blocks destroy. */
	uint32_t ggtt_offset;
	unsigned ggtt_pages;

	/* The address space the object is bound into and where. */
	struct i915_ppgtt *vm;
	uint64_t va;

	/* The session's slot and handle for the object. */
	uint32_t slot;
	uint64_t handle;

	/* Nonzero while the GPU may still name the object after its destroy. */
	unsigned quarantined;

	/* Nonzero while a submitted request uses the object. */
	unsigned busy;

	/* The registry list and the session list. */
	struct i915_gem_object *next;
	struct i915_gem_object *session_next;

	/*
	 * Sharing: an exported object counts its exports and the aliases
	 * imported from them, and its backing outlives its own destroy until
	 * the last of them is gone.  An alias borrows the backing (run,
	 * address) and frees only itself.
	 */
	struct i915_gem_object *alias_of;
	unsigned share_refs;

	/* Nonzero once the object was destroyed while shared: the backing waits for the last reference. */
	unsigned share_orphan;
};

/*
 * Every live session object of one device.
 *
 * It belongs to the device and is protected by the device mutex.
 */
struct i915_gem_registry {
	/* The live objects, newest first. */
	struct i915_gem_object *objects;

	/* How many objects the list holds. */
	unsigned object_count;

	/* How many objects were retained because the GPU may still name them. */
	unsigned quarantined_objects;
};

int drv_i915_gt_mem_init(struct i915_gt_mem *gm, struct drv_dma_device *dma, uint64_t dma_mask, void *table, unsigned entries, uint64_t scratch_pte, struct i915_mmio *m);
void drv_i915_gt_mem_fini(struct i915_gt_mem *gm);

struct i915_gt_object *drv_i915_gt_object_create(struct i915_gt_mem *gm, uint32_t bytes);
void drv_i915_gt_object_destroy(struct i915_gt_mem *gm, struct i915_gt_object *o);
int drv_i915_gt_object_page_dma(const struct i915_gt_object *o, unsigned page, uint64_t *dma_out);
void drv_i915_gt_clflush(const volatile void *address, size_t bytes);

int drv_i915_gem_create(struct i915_gem_registry *registry, uint64_t bytes, struct i915_gem_object **result);
void drv_i915_gem_destroy(struct i915_gem_registry *registry, struct i915_gem_object *object);
void drv_i915_gem_share_put(struct i915_gem_registry *registry, struct i915_gem_object *object);
int drv_i915_gem_bind_vm(struct i915_ppgtt *vm, struct i915_gem_object *object);
void drv_i915_gem_unbind_vm(struct i915_gem_object *object);
int drv_i915_gem_read(struct i915_gem_object *object, uint64_t offset, void *buffer, uint32_t bytes);
int drv_i915_gem_write(struct i915_gem_object *object, uint64_t offset, const void *buffer, uint32_t bytes);

#endif
