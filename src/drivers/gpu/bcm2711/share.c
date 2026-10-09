/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Exports retain native RAM independently of sessions, mappings and scanout. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/share.h"

/* One immutable capability keeps its page vector and allocation alive together. */
struct bcm2711_shared {
	struct bcm2711_shared *next;
	struct bcm2711_buffer *buffer;
	struct gpu_image_descriptor image;
	uint64_t page_count;
	bool has_image;
	uint64_t pages[1];
};

/* This leaf guard protects the export registry; zero atomic state is unlocked. */
static struct spinlock export_guard = {
	{0}, LOCK_RANK_DEVICE, "bcm2711-allocation-exports", 0, 0};

/* Only live native exports enter this list, protected by export_guard. */
static struct bcm2711_shared *exports;

static bool same_image(const struct gpu_image_descriptor *first, const struct gpu_image_descriptor *second);

/*
 * Allocates under actual native reachability, refusing unsupported coherence.
 */
int
bcm2711_blob_allocate(
	const struct gpu_blob_create *request,
	const struct gpu_placement *placement,
	struct bcm2711_buffer **result)
{
	uint64_t limit;
	size_t alignment;
	int error;

	/* Native blobs are independent allocations, without a host protocol identifier. */
	*result = NULL;
	if (request->blob_id != 0 || request->bytes == 0)
		return EINVAL;
	if ((request->flags & ~(GPU_BLOB_MAPPABLE | GPU_BLOB_SHAREABLE | GPU_BLOB_CROSS_DEVICE)) != 0)
		return EINVAL;
	if ((request->flags & GPU_BLOB_CROSS_DEVICE) != 0 &&
	    (request->flags & GPU_BLOB_SHAREABLE) == 0)
		return EINVAL;
	limit = 0x3fffffffU;
	alignment = 4096;

	/* Cached, nonsnooping RAM cannot satisfy an explicit coherence requirement. */
	if (placement != NULL) {
		if (placement->reserved != 0 ||
		    (placement->flags & ~(GPU_PLACEMENT_DMA32 | GPU_PLACEMENT_CONTIGUOUS | GPU_PLACEMENT_COHERENT)) != 0)
			return EINVAL;
		if ((placement->flags & GPU_PLACEMENT_COHERENT) != 0)
			return ENOTSUP;
		if (placement->max_dma_address != 0 && placement->max_dma_address < limit)
			limit = placement->max_dma_address;
		if (placement->alignment > 0x40000000ULL)
			return ENOTSUP;
		if (placement->alignment > alignment)
			alignment = (size_t)placement->alignment;
		if (placement->alignment != 0 &&
		    (placement->alignment & (placement->alignment - 1U)) != 0)
			return EINVAL;
	}

	/* The allocator verifies the returned run's extent and base alignment. */
	error = bcm2711_buffer_create(request->bytes, limit, alignment, result);
	if (error != 0)
		return error;

	/* Succeeded: one owned allocation meets every accepted physical condition. */
	return 0;
}

/*
 * Creates an immutable page-vector capability without transferring the source.
 */
int
bcm2711_shared_create(
	struct bcm2711_buffer *buffer,
	const struct gpu_image_descriptor *image,
	struct bcm2711_shared **result)
{
	struct bcm2711_shared *shared;
	uint64_t count;
	uint64_t index;
	unsigned long enabled;
	int error;

	/* Image exports have a complete linear layout; allocation-only exports have none. */
	*result = NULL;
	if (image != NULL) {
		error = bcm2711_shared_image(buffer, image);
		if (error != 0)
			return error;
	}

	/* Buffer creation already bounds the page-vector allocation to at most 65536 pages. */
	count = buffer->memory.size / 4096U;
	shared = kern_calloc(1, sizeof(*shared) + (size_t)(count - 1U) * sizeof(uint64_t));
	if (shared == NULL)
		return ENOMEM;
	shared->buffer = buffer;
	shared->page_count = count;
	if (image != NULL) {
		shared->image = *image;
		shared->has_image = true;
	}

	/* Stores actual immutable physical addresses, without aliases or local resource IDs. */
	for (index = 0; index < count; index++)
		shared->pages[index] = buffer->memory.paddr + index * 4096U;

	/* The export's allocation hold exists before another device can find it. */
	bcm2711_buffer_retain(buffer);
	enabled = spin_lock_irqsave(&export_guard);

	shared->next = exports;
	exports = shared;

	spin_unlock_irqrestore(&export_guard, enabled);

	/* Succeeded: the capability owns its pages and allocation independently of the open. */
	*result = shared;
	return 0;
}

/*
 * Withdraws a capability before releasing the storage or immutable vector.
 */
void
bcm2711_shared_release(
	struct bcm2711_shared *shared)
{
	struct bcm2711_shared **position;
	unsigned long enabled;

	/* Joins concurrent native imports before removing their source capability. */
	enabled = spin_lock_irqsave(&export_guard);

	/* Finds the exact live capability, without dereferencing foreign backend objects. */
	position = &exports;
	while (*position != NULL && *position != shared)
		position = &(*position)->next;

	/* A backend release consumes exactly one previously exported capability. */
	if (*position == NULL)
		__builtin_trap();
	*position = shared->next;

	spin_unlock_irqrestore(&export_guard, enabled);

	/* Imported resources and native DMA holds own their own allocation references. */
	bcm2711_buffer_release(shared->buffer);
	kern_free(shared);
}

/*
 * Borrows the native allocation while the same-device exported capability lives.
 */
struct bcm2711_buffer *
bcm2711_shared_buffer(
	struct bcm2711_shared *shared)
{
	/* Succeeded: the caller's capability reference keeps this allocation live. */
	return shared->buffer;
}

/*
 * Describes immutable native RAM while its exported capability remains retained.
 */
int
bcm2711_shared_backing(
	struct bcm2711_shared *shared,
	struct drv_gpu_scanout_backing *backing)
{
	/* Allocation-only exports cannot silently acquire a scanout layout. */
	if (!shared->has_image)
		return ENOTSUP;
	backing->pages = shared->pages;
	backing->page_count = shared->page_count;
	backing->bytes = shared->buffer->bytes;
	backing->page_bytes = 4096;
	backing->flags = DRV_GPU_BACKING_CONTIGUOUS;

	/* Succeeded: this is ordinary cached RAM with one verified physical run. */
	return 0;
}

/*
 * Imports only a live native capability, acquiring its own allocation reference.
 */
int
bcm2711_shared_lookup(
	const struct gpu_image_descriptor *image,
	const struct drv_gpu_scanout_backing *backing,
	struct bcm2711_buffer **result)
{
	struct bcm2711_shared *shared;
	unsigned long enabled;
	bool matches;

	/* A foreign descriptor cannot reinterpret arbitrary RAM as an owned native allocation. */
	*result = NULL;
	if (backing->page_bytes != 4096 || backing->flags != DRV_GPU_BACKING_CONTIGUOUS)
		return ENOTSUP;
	enabled = spin_lock_irqsave(&export_guard);

	/* The retained vector's identity selects its native export without following foreign pages. */
	for (shared = exports; shared != NULL; shared = shared->next) {
		if (shared->pages != backing->pages)
			continue;
		if (!shared->has_image || backing->page_count != shared->page_count ||
		    backing->bytes != shared->buffer->bytes)
			continue;

		/* The common capability supplies its authoritative image, with only device_id added. */
		matches = same_image(image, &shared->image);
		if (!matches)
			continue;

		/* A new independent reference survives source-resource and capability destruction. */
		bcm2711_buffer_retain(shared->buffer);
		*result = shared->buffer;
		break;
	}

	spin_unlock_irqrestore(&export_guard, enabled);

	/* Unsupported exporters supply no native allocation lifetime guarantee. */
	if (*result == NULL)
		return ENOTSUP;

	/* Succeeded: the receiver owns one independently retained, exactly described native run. */
	return 0;
}

/*
 * Borrows the original image description while a native capability remains live.
 */
bool
bcm2711_shared_description(
	struct bcm2711_shared *shared,
	struct gpu_image_descriptor *image)
{
	/* Allocation-only sharing must not acquire an invented scanout layout. */
	if (!shared->has_image)
		return false;
	*image = shared->image;

	/* Succeeded: the output describes the original immutable native image. */
	return true;
}

/*
 * Validates the entire linear image and actual native physical extent before import.
 */
int
bcm2711_shared_image(
	const struct bcm2711_buffer *buffer,
	const struct gpu_image_descriptor *image)
{
	uint64_t bytes;

	/* Both native devices accept only the two complete four-byte linear color layouts. */
	if (image->width == 0 || image->width > UINT32_MAX / 4U || image->height == 0)
		return EINVAL;
	if (image->format != GPU_PIXEL_BGRA8888 && image->format != GPU_PIXEL_RGBA8888)
		return ENOTSUP;
	if (image->tiling != GPU_IMAGE_LINEAR || image->allocation_bytes != buffer->bytes)
		return ENOTSUP;
	if (image->stride < image->width * 4U || (image->stride & 3U) != 0)
		return EINVAL;
	if ((image->offset & 3U) != 0 || image->offset > buffer->bytes)
		return EINVAL;
	bytes = (uint64_t)image->stride * image->height;
	if (bytes > buffer->bytes - image->offset)
		return EINVAL;
	if (buffer->memory.paddr > 0x3fffffffU ||
	    buffer->memory.size - 1U > 0x3fffffffU - buffer->memory.paddr)
		return ENOTSUP;

	/* Succeeded: no image row or native DMA address leaves the retained allocation. */
	return 0;
}

/* Compares immutable layout fields while the core supplies the exporting device identity. */
static bool
same_image(
	const struct gpu_image_descriptor *first,
	const struct gpu_image_descriptor *second)
{
	/* Every application-visible layout field must come from the original export. */
	if (first->width != second->width || first->height != second->height ||
	    first->format != second->format || first->stride != second->stride ||
	    first->offset != second->offset || first->allocation_bytes != second->allocation_bytes ||
	    first->memory_type != second->memory_type || first->usage != second->usage ||
	    first->tiling != second->tiling || first->reserved != second->reserved)
		return false;

	/* Succeeded: the two descriptions identify the same immutable image layout. */
	return true;
}
