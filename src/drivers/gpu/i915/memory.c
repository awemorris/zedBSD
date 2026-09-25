/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The driver's memory objects (see memory.h).
 *
 * GT objects are zeroed when created, because a context image, a status
 * page and a page-table page are all read by the GPU before anything
 * writes them.  Session objects are zeroed so that a previous owner's data
 * never reaches a new resource.
 */

#include "i915.h"
#include "memory.h"
#include "ggtt.h"
#include "ppgtt.h"
#include <kern/kcrt.h>

#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/pmem.h>
#include <drivers/generic/dma.h>

#include <uapi/errno.h>
#include <stddef.h>

/*
 * Prepares the GT memory over the mapped GGTT page table.
 *
 * The GT window is taken at the top of the GGTT, the end
 * intel_gt_init_scratch() asks for with PIN_HIGH and the end the firmware
 * framebuffer at the bottom does not occupy.  Linux hands out GGTT space
 * from a range allocator over the whole GGTT; this driver uses a fixed
 * window instead.  Returns 0, EINVAL, or ENOSPC when the table is too small
 * for the window.
 */
int
drv_i915_gt_mem_init(
	struct i915_gt_mem *gm,
	struct drv_dma_device *dma,
	uint64_t dma_mask,
	void *table,
	unsigned entries,
	uint64_t scratch_pte,
	struct i915_mmio *m)
{
	/* Refuses a missing memory, DMA device or page table. */
	if (gm == NULL)
		return EINVAL;
	if (dma == NULL)
		return EINVAL;
	if (table == NULL)
		return EINVAL;

	/* Starts from an empty pool and empty windows. */
	kern_memset(gm, 0, sizeof(*gm));

	/* Refuses a table that the window would fill down to its bottom. */
	if (entries <= I915_GT_GGTT_PAGES)
		return ENOSPC;

	/* Binds the DMA device and the page table. */
	gm->dma = dma;
	gm->dma_mask = dma_mask;
	gm->table = (volatile uint8_t *)table;
	gm->entries = entries;
	gm->scratch_pte = scratch_pte;
	gm->m = m;

	/* Places the GT window at the top of the GGTT. */
	gm->window_first = entries - I915_GT_GGTT_PAGES;
	gm->window_pages = I915_GT_GGTT_PAGES;
	gm->inited = 1;

	/* Succeeded: objects can be created and bound. */
	return 0;
}

/*
 * Releases every GT object except those the display may still read.
 *
 * Objects go newest slot first, and each is unbound before its pages are
 * given back, so no live entry names a freed page.
 */
void
drv_i915_gt_mem_fini(
	struct i915_gt_mem *gm)
{
	struct i915_gt_object *object;
	unsigned index;

	/* A memory that was never prepared holds nothing. */
	if (gm == NULL)
		return;
	if (gm->inited == 0)
		return;

	/* Releases the pool from the last slot down. */
	index = I915_GT_MAX_OBJECTS;
	while (index > 0U) {
		index--;
		object = &gm->objects[index];

		/* An empty slot has nothing to release. */
		if (object->in_use == 0)
			continue;

		/*
		 * The display could not be shown to have stopped reading a kept
		 * object, so it is leaked rather than freed under the display.
		 */
		if (object->keep != 0) {
			gm->kept_objects++;
			kern_logf("i915: gt memory: object at GGTT 0x%llx (%u pages) NOT released: still owned by the display\n",
				  (unsigned long long)object->ggtt_offset,
				  object->pages);
			continue;
		}

		/* Unbinds the object and gives its pages back. */
		drv_i915_gt_object_destroy(gm, object);
	}

	gm->inited = 0;
}

/*
 * Creates a zeroed GT object of at least the given size.
 *
 * The size is rounded up to whole pages, because a GGTT binding is page
 * granular.  An object up to the DMA vector cap is backed by a vector; a
 * larger one by one coherent, page-aligned run.  Returns NULL when the
 * pool or the backing ran out.
 */
struct i915_gt_object *
drv_i915_gt_object_create(
	struct i915_gt_mem *gm,
	uint32_t bytes)
{
	struct i915_gt_object *object;
	unsigned index;
	int usable;
	int error;

	/* Refuses a memory that is not prepared and an empty object. */
	if (gm == NULL)
		return NULL;
	if (gm->inited == 0)
		return NULL;
	if (bytes == 0U)
		return NULL;

	/* Rounds the size up to whole pages. */
	bytes = (uint32_t)((bytes + I915_GT_PAGE_BYTES - 1U) & ~(I915_GT_PAGE_BYTES - 1U));

	/* Claims the first free pool slot. */
	object = NULL;
	for (index = 0U; index < I915_GT_MAX_OBJECTS; index++) {
		if (gm->objects[index].in_use == 0) {
			object = &gm->objects[index];
			break;
		}
	}

	/* Reports a pool with no free slot. */
	if (object == NULL) {
		gm->obj_alloc_fail++;
		kern_logf("i915: gt memory: object pool exhausted (%u slots)\n",
			  I915_GT_MAX_OBJECTS);
		return NULL;
	}

	/* Starts the slot with no backing. */
	object->contiguous = 0;
	object->vec = NULL;

	/* Chooses the backing by size: the vector has a size cap. */
	if (bytes > DRV_DMA_VECTOR_MAX_SIZE) {
		/* Allocates one coherent, page-aligned run above the vector cap. */
		error = drv_dma_alloc_coherent(gm->dma, (size_t)bytes, I915_GT_PAGE_BYTES, &object->big);

		/* A run without a CPU view, or not page aligned, cannot back page table entries. */
		if (error != 0) {
			/* The DMA device refused the run. */
			usable = 0;
		} else if (object->big.address == NULL) {
			/* The run has no CPU view to zero. */
			usable = 0;
		} else if ((object->big.device_address & (I915_GT_PAGE_BYTES - 1U)) != 0U) {
			/* A run that is not page aligned cannot be named by an entry. */
			usable = 0;
		} else {
			usable = 1;
		}

		/* Gives back a run that was allocated but cannot be used, and reports the failure. */
		if (usable == 0) {
			if (error == 0) {
				/* The run exists and goes back to the DMA device. */
				drv_dma_free_coherent(gm->dma, &object->big);
			}
			gm->obj_alloc_fail++;
			kern_logf("i915: gt memory: coherent backing for %u bytes failed rc=%d\n",
				  bytes,
				  error);
			return NULL;
		}

		object->contiguous = 1;
		object->cpu = object->big.address;
	} else {
		/* Allocates the DMA vector that backs the object. */
		error = drv_dma_vector_create(gm->dma, (size_t)bytes, &object->vec);
		if (error != 0) {
			gm->obj_alloc_fail++;
			kern_logf("i915: gt memory: backing pages for %u bytes failed rc=%d\n",
				  bytes,
				  error);
			object->vec = NULL;
			return NULL;
		}

		/* Takes the CPU view of the vector; a vector without one is given back. */
		object->cpu = drv_dma_vector_address(object->vec);
		if (object->cpu == NULL) {
			(void)drv_dma_vector_free(object->vec);
			object->vec = NULL;
			gm->obj_alloc_fail++;
			return NULL;
		}
	}

	/* Zeroes the pages the GPU may read before anything writes them. */
	kern_memset(object->cpu, 0, (size_t)bytes);

	/* Publishes the object as live and unbound. */
	object->bytes = bytes;
	object->pages = bytes / I915_GT_PAGE_BYTES;
	object->bound = 0;
	object->ggtt_page = 0U;
	object->ggtt_offset = 0U;
	object->in_use = 1;
	gm->objects_live++;

	/* Succeeded: the caller owns a zeroed, unbound object. */
	return object;
}

/*
 * Unbinds a GT object and gives its pages back.
 *
 * A kept object is refused and counted, because the display may still be
 * reading it.
 */
void
drv_i915_gt_object_destroy(
	struct i915_gt_mem *gm,
	struct i915_gt_object *o)
{
	/* Nothing is destroyed without a memory and a live object. */
	if (gm == NULL)
		return;
	if (o == NULL)
		return;
	if (o->in_use == 0)
		return;

	/* Refuses an object the display may still read. */
	if (o->keep != 0) {
		gm->keep_refusals++;
		return;
	}

	/* Unbinds the object from whichever window holds it. */
	if (o->bound != 0 && o->display != 0) {
		drv_i915_gt_display_unbind(gm, o);
	} else if (o->bound != 0) {
		drv_i915_gt_ggtt_unbind(gm, o);
	}

	/* Gives the vector back. */
	if (o->vec != NULL)
		(void)drv_dma_vector_free(o->vec);

	/* Gives the coherent run back. */
	if (o->contiguous != 0)
		drv_dma_free_coherent(gm->dma, &o->big);

	/* Returns the slot to the pool. */
	o->contiguous = 0;
	o->vec = NULL;
	o->cpu = NULL;
	o->bytes = 0U;
	o->pages = 0U;
	o->in_use = 0;

	/* The live count only moves down while it has something to count. */
	if (gm->objects_live != 0U)
		gm->objects_live--;
}

/*
 * Reports the DMA address of one page of a GT object.
 *
 * Returns 0, EINVAL for a page the object does not have, or EIO when the
 * backing cannot name the page: a segment that is not page aligned would
 * make an entry point into the middle of a page.
 */
int
drv_i915_gt_object_page_dma(
	const struct i915_gt_object *o,
	unsigned page,
	uint64_t *dma_out)
{
	struct drv_dma_segment segment;
	uint64_t wanted;
	uint64_t seen;
	unsigned segment_count;
	unsigned index;
	int error;

	/* Refuses a missing object, a missing result and a page past the end. */
	if (o == NULL)
		return EINVAL;
	if (o->in_use == 0)
		return EINVAL;
	if (dma_out == NULL)
		return EINVAL;
	if (page >= o->pages)
		return EINVAL;

	/* The page's byte offset inside the backing. */
	wanted = (uint64_t)page * I915_GT_PAGE_BYTES;

	/* A coherent run is one straight range of device addresses. */
	if (o->contiguous != 0) {
		*dma_out = o->big.device_address + wanted;
		return 0;
	}

	/* Finds the vector segment that holds the page. */
	seen = 0U;
	segment_count = drv_dma_vector_count(o->vec);
	for (index = 0U; index < segment_count; index++) {
		/* Reads the segment. */
		error = drv_dma_vector_segment(o->vec, index, &segment);
		if (error != 0)
			return EIO;

		/* Skips a segment that ends before the page. */
		if (wanted >= seen + (uint64_t)segment.length) {
			seen += (uint64_t)segment.length;
			continue;
		}

		/* Refuses a segment whose offsets would not name whole pages. */
		if ((segment.address & (I915_GT_PAGE_BYTES - 1U)) != 0U)
			return EIO;

		/* Succeeded: the page sits inside this segment. */
		*dma_out = segment.address + (wanted - seen);
		return 0;
	}

	/* The segments ended before the page: the vector is shorter than the object. */
	return EIO;
}

/*
 * Writes the CPU cache lines of a range back to memory.
 *
 * The GPU's table walker does not snoop the CPU caches, so every page-table
 * entry is flushed after it is written (write_dma_entry, fill_page_dma and
 * gen8_ppgtt_insert_entry in Linux), and a range the display reads is
 * flushed before the display reads it.
 */
void
drv_i915_gt_clflush(
	const volatile void *address,
	size_t bytes)
{
#if defined(__x86_64__) || defined(__i386__)
	const volatile char *line;
	const volatile char *end;

	/* Covers every 64-byte line the range touches. */
	line = (const volatile char *)((uintptr_t)address & ~(uintptr_t)63U);
	end = (const volatile char *)address + bytes;

	/* Orders the earlier stores before the flush. */
	__asm__ volatile("mfence" : : : "memory");

	/* Flushes every line of the range. */
	while (line < end) {
		__asm__ volatile("clflush (%0)" : : "r"(line) : "memory");
		line += 64;
	}

	/* Orders the flush before anything that follows. */
	__asm__ volatile("mfence" : : : "memory");
#else
	UNUSED_PARAMETER(address);
	UNUSED_PARAMETER(bytes);
#endif
}

/*
 * Creates a zeroed contiguous session object of the requested size.
 *
 * Returns 0, EINVAL for an empty or oversized request, ENOMEM, EFAULT when
 * the run is outside the direct map, or the page manager's error.
 */
int
drv_i915_gem_create(
	struct i915_gem_registry *registry,
	uint64_t bytes,
	struct i915_gem_object **result)
{
	struct i915_gem_object *object;
	uint64_t rounded;
	int error;

	/* A failed creation transfers nothing. */
	*result = NULL;

	/* Objects are whole pages; the size limit bounds the contiguous allocation. */
	if (bytes == 0U)
		return EINVAL;
	if (bytes > I915_MAX_RESOURCE_BYTES)
		return EINVAL;
	rounded = (bytes + I915_PAGE_BYTES - 1U) & ~((uint64_t)I915_PAGE_BYTES - 1U);

	/* Allocates the object record. */
	object = kern_calloc(1U, sizeof(*object));
	if (object == NULL)
		return ENOMEM;

	/* Allocates a run the GPU's address bits reach. */
	error = kern_pmem_alloc_limited((size_t)rounded, I915_PAGE_BYTES, I915_DMA_MAX_ADDRESS, 0U, &object->run);
	if (error != 0) {
		kern_free(object);
		return error;
	}

	/* Takes the CPU view from the direct map; a run outside managed RAM cannot be used. */
	object->address = kern_pmem_to_kernel(object->run.paddr);
	if (object->address == NULL) {
		(void)kern_pmem_free(&object->run);
		kern_free(object);
		return EFAULT;
	}

	/* Zeroes the run so previous owners' data stays out of a new resource. */
	kern_memset(object->address, 0, (size_t)rounded);
	object->bytes = rounded;
	object->pages = (unsigned)(rounded / I915_PAGE_BYTES);

	/* Links the object into the registry so detach and reset find it. */
	object->next = registry->objects;
	registry->objects = object;
	registry->object_count++;
	*result = object;

	/* Succeeded: the caller owns an unbound zeroed object. */
	return 0;
}

/*
 * Releases a session object after its bindings retired.
 *
 * An object that is still bound, or quarantined, is retained on the
 * registry instead, because hardware may still address it until the next
 * checked reset.  An alias frees only itself; a shared object keeps its
 * backing until the last reference goes.
 */
void
drv_i915_gem_destroy(
	struct i915_gem_registry *registry,
	struct i915_gem_object *object)
{
	struct i915_gem_object **position;
	struct i915_gem_object *source;

	/* A binding that is still live is a driver bug; the object is retained, not freed. */
	if (object->ggtt_pages != 0U || object->vm != NULL) {
		kern_logf("i915: object destroyed while bound; retained\n");
		object->quarantined = 1U;
	}

	/* Hardware may still address a quarantined object until the next checked reset. */
	if (object->quarantined != 0U) {
		registry->quarantined_objects++;
		return;
	}

	/* An alias frees only itself; its reference on the exported object goes with it. */
	if (object->alias_of != NULL) {
		source = object->alias_of;
		kern_free(object);
		drv_i915_gem_share_put(registry, source);
		return;
	}

	/* Finds the object's link in the registry. */
	position = &registry->objects;
	while (*position != NULL && *position != object)
		position = &(*position)->next;

	/* Unlinks the object when the registry holds it. */
	if (*position == object) {
		*position = object->next;
		registry->object_count--;
	}

	/*
	 * An exported object keeps its backing while an export or an alias
	 * still names it; the orphan mark tells the last reference to free it.
	 */
	if (object->share_refs != 0U) {
		object->share_orphan = 1U;
		return;
	}

	/* Returns the backing to the pool; no GPU mapping names it any more. */
	(void)kern_pmem_free(&object->run);
	kern_free(object);
}

/*
 * Drops one reference of an exported session object.
 *
 * The last reference frees a backing whose object was already destroyed.
 * The device mutex is held.
 */
void
drv_i915_gem_share_put(
	struct i915_gem_registry *registry,
	struct i915_gem_object *object)
{
	UNUSED_PARAMETER(registry);

	/* An object with no references has nothing to drop. */
	if (object->share_refs == 0U)
		return;

	/* The count is the exports and aliases that still name the backing. */
	object->share_refs--;

	/* The last reference of a destroyed object frees its backing. */
	if (object->share_refs == 0U && object->share_orphan != 0U) {
		(void)kern_pmem_free(&object->run);
		kern_free(object);
	}
}

/*
 * Maps a session object into one session's private address space.
 *
 * Returns 0, EBUSY when the object is already bound, or the address
 * space's error.
 */
int
drv_i915_gem_bind_vm(
	struct i915_ppgtt *vm,
	struct i915_gem_object *object)
{
	uint64_t va;
	int error;

	/* An object belongs to at most one address space. */
	if (object->vm != NULL)
		return EBUSY;

	/* Reserves the object's own aligned range; ranges are never reused. */
	error = drv_i915_ppgtt_va_alloc(vm, object->bytes, &va);
	if (error != 0)
		return error;

	/* Maps the run, growing the page tables as needed to cover it. */
	error = drv_i915_ppgtt_insert(vm, va, (uint64_t)object->run.paddr, object->pages);
	if (error != 0)
		return error;

	/* Records the binding. */
	object->vm = vm;
	object->va = va;

	/* Succeeded: GPU commands in this context address the object at va. */
	return 0;
}

/*
 * Removes a session object's private address space mapping.
 */
void
drv_i915_gem_unbind_vm(
	struct i915_gem_object *object)
{
	/* An object without a context mapping has nothing to clear. */
	if (object->vm == NULL)
		return;

	/* Points the range back at scratch so a stale command reads zeros. */
	drv_i915_ppgtt_clear(object->vm, object->va, object->pages);
	object->vm = NULL;
	object->va = 0U;
}

/*
 * Copies bytes out of a session object through its CPU view.
 *
 * Returns 0, or EINVAL for a range outside the object.
 */
int
drv_i915_gem_read(
	struct i915_gem_object *object,
	uint64_t offset,
	void *buffer,
	uint32_t bytes)
{
	const uint8_t *source;

	/* Refuses a range outside the object, even though the caller checked its own view. */
	if (offset > object->bytes)
		return EINVAL;
	if (bytes > object->bytes - offset)
		return EINVAL;

	/* GPU writes are visible through the LLC; the barrier orders against later reads. */
	kern_io_read_barrier();
	source = object->address;
	kern_memcpy(buffer, source + offset, bytes);

	/* Succeeded: the caller's buffer holds a snapshot of the object. */
	return 0;
}

/*
 * Copies bytes into a session object through its CPU view.
 *
 * Returns 0, or EINVAL for a range outside the object.
 */
int
drv_i915_gem_write(
	struct i915_gem_object *object,
	uint64_t offset,
	const void *buffer,
	uint32_t bytes)
{
	uint8_t *destination;

	/* Refuses a range outside the object. */
	if (offset > object->bytes)
		return EINVAL;
	if (bytes > object->bytes - offset)
		return EINVAL;

	/* The barrier publishes the bytes before a later submission can consume them. */
	destination = object->address;
	kern_memcpy(destination + offset, buffer, bytes);
	kern_io_write_barrier();

	/* Succeeded: the GPU sees the new contents on its next access. */
	return 0;
}
