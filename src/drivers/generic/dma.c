/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Generic no-IOMMU DMA implementation.
 */

#include <drivers/dma.h>
#include <errno.h>
#include <kern/io-stats.h>
#include <kern/cache-memory.h>
#include <hal/hal.h>
#include <kern/lock.h>
#include <limits.h>
#include <string.h>
#include <kern/pmem.h>
#include "kern/kmem.h"

struct dma_allocation {
	struct kern_pmem memory;
	size_t payload_size;
	struct dma_allocation *next;
};

struct drv_dma_device {
	struct drv_dma_constraints constraints;
	struct spinlock lock;
	struct dma_allocation *allocations;
	unsigned active_operations;
	unsigned vector_count;
	unsigned destroying;
};

struct drv_dma_mapping {
	struct drv_dma_segment segment;
	enum drv_dma_direction direction;
};

struct drv_dma_vector {
	struct drv_dma_device *device;
	struct drv_dma_buffer contiguous;
	void *address;
	size_t size, charged;
	unsigned count;
	struct drv_dma_segment segments[DRV_DMA_VECTOR_MAX_SEGMENTS];
};

/*
 * Weak
 */
extern int cache_memory_reserve(enum cache_memory_kind, size_t, int) __attribute__((weak));
extern void cache_memory_commit(enum cache_memory_kind, size_t) __attribute__((weak));
extern void cache_memory_cancel(enum cache_memory_kind, size_t) __attribute__((weak));
extern void cache_memory_release(enum cache_memory_kind, size_t) __attribute__((weak));
extern size_t cache_memory_reclaim(size_t) __attribute__((weak));

/*
 * Forward declaration
 */
static int device_operation_begin(struct drv_dma_device *device, int allow_destroying);
static int dma_vector_segments(struct drv_dma_vector *vector);
static int dma_vector_backing_free(struct drv_dma_vector *vector);

static void device_operation_end(struct drv_dma_device *device);
static int address_fits(const struct drv_dma_device *device, uint64_t address, size_t size);
static int is_power_of_two(uint64_t value);

/*
 * Implements the drv dma device create operation.
 */
int
drv_dma_device_create(
	const struct drv_dma_constraints *constraints,
	struct drv_dma_device **result)
{
	struct drv_dma_device *device;

	/* Checks the power of two result. */
	if (constraints == NULL || result == NULL ||
	    constraints->address_bits == 0 || constraints->address_bits > 64 ||
	    constraints->max_segment_size == 0 ||
	    (constraints->segment_boundary != 0 &&
	     (!is_power_of_two(constraints->segment_boundary) ||
	      constraints->max_segment_size > constraints->segment_boundary))) {
		/* Failed. */
		return EINVAL;
	}

	/* Handles the device availability. */
	device = kern_malloc(sizeof(*device));
	if (device == NULL)
		return ENOMEM;
	memset(device, 0, sizeof(*device));
	device->constraints = *constraints;
	spin_init(&device->lock, LOCK_RANK_DEVICE, "DMA allocation list");
	*result = device;
	/* Succeeded. */
	return 0;
}

/*
 * Implements the drv dma device destroy operation.
 */
int
drv_dma_device_destroy(
	struct drv_dma_device *device)
{
	unsigned long irq;

	/* Handles the device availability. */
	if (device == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&device->lock);

	device->destroying = 1;

	/* Handles the allocations availability. */
	if (device->allocations != NULL || device->active_operations != 0 ||
	    device->vector_count != 0) {
		spin_unlock_irqrestore(&device->lock, irq);

		/* Failed. */
		return EBUSY;
	}

	spin_unlock_irqrestore(&device->lock, irq);

	kern_free(device);

	/* Succeeded. */
	return 0;
}

/*
 * Implements the drv dma device address bits operation.
 */
unsigned
drv_dma_device_address_bits(
	const struct drv_dma_device *device)
{
	unsigned result;

	/* Checks the device operation begin result. */
	if (device == NULL ||
	    device_operation_begin((struct drv_dma_device *)device, 0) != 0) {
		/* Succeeded. */
		return 0;
	}
	result = device->constraints.address_bits;
	device_operation_end((struct drv_dma_device *)device);

	/* Returns the computed result. */
	return result;
}
/*
 * Implements the drv dma device max segment size operation.
 */
size_t
drv_dma_device_max_segment_size(
	const struct drv_dma_device *device)
{
	size_t result;

	/* Checks the device operation begin result. */
	if (device == NULL ||
	    device_operation_begin((struct drv_dma_device *)device, 0) != 0) {
		/* Succeeded. */
		return 0;
	}
	result = device->constraints.max_segment_size;
	device_operation_end((struct drv_dma_device *)device);

	/* Returns the computed result. */
	return result;
}
/*
 * Implements the drv dma device is coherent operation.
 */
int
drv_dma_device_is_coherent(
	const struct drv_dma_device *device)
{
	int result;

	/* Checks the device operation begin result. */
	if (device == NULL ||
	    device_operation_begin((struct drv_dma_device *)device, 0) != 0) {
		/* Succeeded. */
		return 0;
	}
	result = device->constraints.coherent;
	device_operation_end((struct drv_dma_device *)device);

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the drv dma alloc coherent operation.
 */
int
drv_dma_alloc_coherent(
	struct drv_dma_device *device,
	size_t size,
	size_t alignment,
	struct drv_dma_buffer *buffer)
{
	struct dma_allocation *allocation;
	unsigned long irq;
	size_t allocation_bytes;
	uint64_t maximum;
	uint64_t boundary;
	int error;

	/* Handles the device availability. */
	if (device == NULL || buffer == NULL || size == 0)
		return EINVAL;
	io_stats_record(IO_DMA_REQUEST, size);

	/* Checks the operation status. */
	error = device_operation_begin(device, 0);
	if (error != 0)
		return error;

	/* Checks the current data size. */
	if (size > device->constraints.max_segment_size) {
		device_operation_end(device);

		/* Failed. */
		return EINVAL;
	}

	/* Handles the allocation availability. */
	allocation = kern_malloc(sizeof(*allocation));
	if (allocation == NULL) {
		device_operation_end(device);

		/* Failed. */
		return ENOMEM;
	}

	memset(allocation, 0, sizeof(*allocation));

	/* Checks the hal page get page size result. */
	if (alignment < hal_space_get_page_size(1))
		alignment = hal_space_get_page_size(1);
	maximum = device->constraints.address_bits == 64U
			  ? UINT64_MAX
			  : ((UINT64_C(1) << device->constraints.address_bits) -
			     1U);
	boundary = device->constraints.segment_boundary;

	/*
	 * A sub-page segment constrains the exposed payload, not unused
	 * backing.
	 */
	if (boundary < hal_space_get_page_size(1))
		boundary = 0;

	/* Checks the operation status. */
	allocation->memory.size = size;
	error = hal_pmem_alloc_limited(
		size,
		alignment,
		(hal_physaddr_t)maximum,
		(size_t)boundary,
		&allocation->memory.paddr);
	if (error != HAL_OK ||
	    !address_fits(device, allocation->memory.paddr,
			  allocation->memory.size) ||
	    (device->constraints.segment_boundary != 0 &&
	     size > device->constraints.segment_boundary -
			     allocation->memory.paddr %
				     device->constraints.segment_boundary)) {
		/* Checks the hal pmem free result. */
		if (allocation->memory.size != 0 &&
		    hal_pmem_free(&allocation->memory.paddr, allocation->memory.size) != HAL_OK)
			__builtin_trap();
		kern_free(allocation);
		device_operation_end(device);

		/* Failed. */
		return ENOMEM;
	}

	allocation_bytes = allocation->memory.size;

	/* Accounts mandatory backing before publishing device ownership. */
	if (cache_memory_reserve != NULL) {
		/* Checks the cache memory reserve result. */
		if (cache_memory_reserve(CACHE_MEMORY_DMA,
					 allocation->memory.size, 0) != 0) {
			/* Checks the hal pmem free result. */
			if (hal_pmem_free(&allocation->memory.paddr, allocation->memory.size) != HAL_OK)
				__builtin_trap();
			kern_free(allocation);
			device_operation_end(device);

			/* Failed. */
			return ENOMEM;
		}
	}

	/* Handles the device condition. */
	irq = spin_lock_irqsave(&device->lock);
	if (device->destroying) {
		spin_unlock_irqrestore(&device->lock, irq);

		/* Checks the hal pmem free result. */
		if (hal_pmem_free(&allocation->memory.paddr, allocation->memory.size) != HAL_OK)
			__builtin_trap();

		/* Handles the cache memory cancel availability. */
		if (cache_memory_cancel != NULL)
			cache_memory_cancel(CACHE_MEMORY_DMA, allocation_bytes);
		kern_free(allocation);
		device_operation_end(device);

		/* Failed. */
		return EBUSY;
	}

	allocation->payload_size = size;
	allocation->next = device->allocations;
	device->allocations = allocation;

	spin_unlock_irqrestore(&device->lock, irq);

	buffer->address = hal_pmem_to_kernel(allocation->memory.paddr);
	buffer->device_address = allocation->memory.paddr;
	buffer->size = size;
	buffer->private_data[0] = (uintptr_t)allocation;
	buffer->private_data[1] = 0;

	/* Handles the cache memory commit availability. */
	if (cache_memory_commit != NULL)
		cache_memory_commit(CACHE_MEMORY_DMA, allocation->memory.size);
	io_stats_record(IO_DMA_ALLOC, allocation->memory.size);
	device_operation_end(device);

	/* Succeeded. */
	return 0;
}

/*
 * Implements the drv dma free coherent operation.
 */
void
drv_dma_free_coherent(
	struct drv_dma_device *device,
	struct drv_dma_buffer *buffer)
{
	struct dma_allocation **link, *allocation;
	unsigned long irq;
	int found = 0;
	size_t released_size;

	/* Handles the device availability. */
	if (device == NULL || buffer == NULL || buffer->private_data[0] == 0)
		return;

	/* Checks the device operation begin result. */
	if (device_operation_begin(device, 1) != 0)
		return;
	irq = spin_lock_irqsave(&device->lock);

	allocation = (struct dma_allocation *)buffer->private_data[0];
	/* Process each linked entry. */
	for (link = &device->allocations; *link != NULL;
	     link = &(*link)->next) {
		/* Handles the link condition. */
		if (*link == allocation) {
			*link = allocation->next;
			found = 1;
			break;
		}
	}

	spin_unlock_irqrestore(&device->lock, irq);

	/* Handles the found condition. */
	if (found) {
		released_size = allocation->memory.size;

		/* Checks the hal pmem free result. */
		if (hal_pmem_free(&allocation->memory.paddr, allocation->memory.size) == HAL_OK) {
			/* Handles the cache memory release availability. */
			if (cache_memory_release != NULL) {
				cache_memory_release(CACHE_MEMORY_DMA,
						     released_size);
			}

			io_stats_record(IO_DMA_FREE, released_size);
		} else {
			/*
			 * Keeps failed retirement owned and retryable by the
			 * caller.
			 */
			irq = spin_lock_irqsave(&device->lock);
			allocation->next = device->allocations;
			device->allocations = allocation;
			spin_unlock_irqrestore(&device->lock, irq);
			device_operation_end(device);

			/* Returns the computed result. */
			return;
		}

		kern_free(allocation);
		memset(buffer, 0, sizeof(*buffer));
	}

	device_operation_end(device);
}

/*
 * Implements the drv dma map operation.
 */
int
drv_dma_map(
	struct drv_dma_device *device,
	void *address,
	size_t size,
	enum drv_dma_direction direction,
	struct drv_dma_mapping **result)
{
	uintptr_t base;
	struct dma_allocation *allocation;
	struct drv_dma_mapping *mapping;
	unsigned long irq;
	uintptr_t start = (uintptr_t)address;
	int error;

	/* Handles the device availability. */
	if (device == NULL || address == NULL || size == 0 || result == NULL ||
	    direction < DRV_DMA_TO_DEVICE || direction > DRV_DMA_BIDIRECTIONAL) {
		/* Failed. */
		return EINVAL;
	}

	/* Checks the operation status. */
	error = device_operation_begin(device, 0);
	if (error != 0)
		return error;

	/* Checks the current data size. */
	if (size > device->constraints.max_segment_size) {
		device_operation_end(device);

		/* Failed. */
		return EINVAL;
	}

	/* Handles the mapping availability. */
	mapping = kern_malloc(sizeof(*mapping));
	if (mapping == NULL) {
		device_operation_end(device);

		/* Failed. */
		return ENOMEM;
	}

	/* Handles the device condition. */
	irq = spin_lock_irqsave(&device->lock);
	if (device->destroying) {
		spin_unlock_irqrestore(&device->lock, irq);
		kern_free(mapping);
		device_operation_end(device);

		/* Failed. */
		return EBUSY;
	}

	/* Process each linked entry. */
	for (allocation = device->allocations; allocation != NULL;
	     allocation = allocation->next) {
		/* Handles the start condition. */
		base = (uintptr_t)hal_pmem_to_kernel(
			allocation->memory.paddr);
		if (start < base || start - base > allocation->payload_size ||
		    size > allocation->payload_size - (start - base))
			continue;
		mapping->segment.address =
			allocation->memory.paddr + start - base;
		mapping->segment.length = size;
		mapping->direction = direction;
		spin_unlock_irqrestore(&device->lock, irq);
		*result = mapping;
		device_operation_end(device);

		/* Succeeded. */
		return 0;
	}

	spin_unlock_irqrestore(&device->lock, irq);

	kern_free(mapping);
	device_operation_end(device);

	/* Failed. */
	return ENOTSUP;
}

/*
 * Implements the drv dma unmap operation.
 */
void
drv_dma_unmap(
	struct drv_dma_device *device,
	struct drv_dma_mapping *mapping)
{
	(void)device;

	/* Handles the mapping availability. */
	if (mapping != NULL)
		kern_free(mapping);
}
/*
 * Implements the drv dma mapping segment count operation.
 */
unsigned
drv_dma_mapping_segment_count(
	const struct drv_dma_mapping *mapping)
{
	/* Returns the computed result. */
	return mapping == NULL ? 0U : 1U;
}
/*
 * Implements the drv dma mapping segment operation.
 */
int
drv_dma_mapping_segment(
	const struct drv_dma_mapping *mapping,
	unsigned index,
	struct drv_dma_segment *segment)
{
	/* Handles the mapping availability. */
	if (mapping == NULL || segment == NULL || index != 0)
		return EINVAL;
	*segment = mapping->segment;
	/* Succeeded. */
	return 0;
}
/*
 * Implements the drv dma sync for cpu operation.
 */
void
drv_dma_sync_for_cpu(
	struct drv_dma_device *device,
	struct drv_dma_mapping *mapping)
{
	(void)device;
	(void)mapping;
}
/*
 * Implements the drv dma sync for device operation.
 */
void
drv_dma_sync_for_device(
	struct drv_dma_device *device,
	struct drv_dma_mapping *mapping)
{
	(void)device;
	(void)mapping;
}

/*
 * Owns isolated DMA staging, never an arbitrary caller's reusable buffer.
 */
int
drv_dma_vector_create(
	struct drv_dma_device *device,
	size_t size,
	struct drv_dma_vector **result)
{
	struct drv_dma_vector *vector;
	unsigned long irq;
	int error;

	/* Handles the result availability. */
	if (result == NULL)
		return EINVAL;
	*result = NULL;
	/* Handles the device availability. */
	if (device == NULL || size == 0 || size > DRV_DMA_VECTOR_MAX_SIZE)
		return EINVAL;

	/* Checks the operation status. */
	error = device_operation_begin(device, 0);
	if (error != 0)
		return error;

	/* Handles the device condition. */
	if (!device->constraints.coherent) {
		device_operation_end(device);

		/* Failed. */
		return EOPNOTSUPP;
	}

	/* Handles the vector availability. */
	vector = kern_malloc(sizeof(*vector));
	if (vector == NULL) {
		device_operation_end(device);

		/* Failed. */
		return ENOMEM;
	}

	memset(vector, 0, sizeof(*vector));
	vector->device = device;
	vector->size = size;
	vector->charged = sizeof(*vector);

	/* Coherent storage already satisfies the device mask and boundaries. */
	error = drv_dma_alloc_coherent(device, size, 64U, &vector->contiguous);
	if (error == 0) {
		vector->address = vector->contiguous.address;
		error = dma_vector_segments(vector);
	}

	/* Checks the operation status. */
	if (error != 0)
		goto fail;

	/* Handles the cache memory reserve availability. */
	if (cache_memory_reserve != NULL) {
		/* Checks the operation status. */
		error = cache_memory_reserve(CACHE_MEMORY_DMA, vector->charged,
					     0);
		if (error != 0)
			goto fail;
	}

	/* Handles the device condition. */
	irq = spin_lock_irqsave(&device->lock);
	if (device->destroying || device->vector_count == UINT_MAX) {
		spin_unlock_irqrestore(&device->lock, irq);

		/* Handles the cache memory cancel availability. */
		if (cache_memory_cancel != NULL)
			cache_memory_cancel(CACHE_MEMORY_DMA, vector->charged);
		error = EBUSY;
		goto fail;
	}

	device->vector_count++;

	spin_unlock_irqrestore(&device->lock, irq);

	/* Handles the cache memory commit availability. */
	if (cache_memory_commit != NULL)
		cache_memory_commit(CACHE_MEMORY_DMA, vector->charged);
	io_stats_record(IO_DMA_ALLOC, vector->charged);
	*result = vector;
	device_operation_end(device);

	/* Succeeded. */
	return 0;

fail:

	/* Checks the dma vector backing free result. */
	if (dma_vector_backing_free(vector) != 0)
		HAL_FATAL("DMA vector allocation rollback failed");
	kern_free(vector);
	device_operation_end(device);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Implements the drv dma vector free operation.
 */
int
drv_dma_vector_free(
	struct drv_dma_vector *vector)
{
	struct drv_dma_device *device;
	unsigned long irq;
	int error;

	/* Handles the vector availability. */
	if (vector == NULL)
		return EINVAL;
	device = vector->device;

	/* Checks the operation status. */
	error = device_operation_begin(device, 1);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = dma_vector_backing_free(vector);
	if (error != 0) {
		device_operation_end(device);

		/* Failed. */
		return error;
	}

	/* Handles the cache memory release availability. */
	if (cache_memory_release != NULL)
		cache_memory_release(CACHE_MEMORY_DMA, vector->charged);
	io_stats_record(IO_DMA_FREE, vector->charged);
	irq = spin_lock_irqsave(&device->lock);

	/* Handles the device condition. */
	if (device->vector_count == 0)
		HAL_FATAL("DMA vector owner underflow");
	device->vector_count--;

	spin_unlock_irqrestore(&device->lock, irq);

	kern_free(vector);
	device_operation_end(device);

	/* Succeeded. */
	return 0;
}

/*
 * Implements the drv dma vector address operation.
 */
void *
drv_dma_vector_address(
	const struct drv_dma_vector *vector)
{
	/* Returns the computed result. */
	return vector != NULL ? vector->address : NULL;
}

/*
 * Implements the drv dma vector count operation.
 */
unsigned
drv_dma_vector_count(
	const struct drv_dma_vector *vector)
{
	/* Returns the computed result. */
	return vector != NULL ? vector->count : 0;
}

/*
 * Implements the drv dma vector segment operation.
 */
int
drv_dma_vector_segment(
	const struct drv_dma_vector *vector,
	unsigned index,
	struct drv_dma_segment *segment)
{
	/* Handles the vector availability. */
	if (vector == NULL || segment == NULL || index >= vector->count)
		return EINVAL;
	*segment = vector->segments[index];
	/* Succeeded. */
	return 0;
}

/*
 * A DMA device is normally shared by every device on one bus.  In
 * particular, two host controllers can allocate and release coherent
 * buffers from IRQ and process context at the same time.  The
 * spinlock protects only the device lifecycle and allocation-list
 * metadata; the physical-memory allocator and heap allocator must
 * never be entered while it is held.
 */
static int
device_operation_begin(
	struct drv_dma_device *device,
	int allow_destroying)
{
	unsigned long irq;
	int error = 0;

	irq = spin_lock_irqsave(&device->lock);

	/* Handles the device condition. */
	if (device->destroying && !allow_destroying)
		error = EBUSY;
	else if (device->active_operations == UINT_MAX)
		error = EOVERFLOW;
	else
		device->active_operations++;

	spin_unlock_irqrestore(&device->lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the device operation end operation. */
static void
device_operation_end(
	struct drv_dma_device *device)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&device->lock);

	/* Handles the device condition. */
	if (device->active_operations == 0)
		__builtin_trap();
	device->active_operations--;

	spin_unlock_irqrestore(&device->lock, irq);
}

/* Supports the address fits operation. */
static int
address_fits(
	const struct drv_dma_device *device,
	uint64_t address,
	size_t size)
{
	uint64_t limit;

	/* Checks the current data size. */
	if (size == 0)
		return 0;

	/* Handles the device condition. */
	if (device->constraints.address_bits >= 64U)
		return (uint64_t)size - 1U <= UINT64_MAX - address;
	limit = (uint64_t)1U << device->constraints.address_bits;

	/* Returns the computed result. */
	return address < limit && size <= limit - address;
}

/* Supports the is power of two operation. */
static int
is_power_of_two(
	uint64_t value)
{
	/* Returns the computed result. */
	return value != 0 && (value & (value - 1U)) == 0;
}

/* Splits every page by mask, maximum segment length and device boundaries. */
static int
dma_vector_segments(
	struct drv_dma_vector *vector)
{
	struct drv_dma_device *device;
	struct drv_dma_segment *previous;
	hal_physaddr_t physical;
	uint64_t boundary;
	size_t offset, remaining, length;

	device = vector->device;
	boundary = device->constraints.segment_boundary;
	vector->count = 0;
	offset = 0;
	/* Splits the contiguous run wherever a device limit requires it. */
	while (offset < vector->size) {
		physical = vector->contiguous.device_address + offset;
		length = vector->size - offset;

		/* Checks the current data length. */
		remaining = vector->size - offset;
		if (length > remaining)
			length = remaining;

		/* Checks the current data length. */
		if (length > device->constraints.max_segment_size)
			length = device->constraints.max_segment_size;

		/* Handles the boundary condition. */
		if (boundary != 0 && length > boundary - physical % boundary)
			length = (size_t)(boundary - physical % boundary);

		/* Checks the address fits result. */
		if (!address_fits(device, physical, length))
			return EOVERFLOW;

		/* Handles the previous availability. */
		previous = vector->count != 0
				   ? &vector->segments[vector->count - 1U]
				   : NULL;
		if (previous != NULL &&
		    previous->length <= UINT64_MAX - previous->address &&
		    previous->address + previous->length == physical &&
		    length <= device->constraints.max_segment_size -
				      previous->length &&
		    (boundary == 0 ||
		     previous->address / boundary == physical / boundary)) {
			previous->length += length;
		} else {
			/* Handles the vector condition. */
			if (vector->count == DRV_DMA_VECTOR_MAX_SEGMENTS)
				return E2BIG;
			vector->segments[vector->count].address = physical;
			vector->segments[vector->count++].length = length;
		}

		offset += length;
	}

	/* Succeeded. */
	return 0;
}

/* Supports the dma vector backing free operation. */
static int
dma_vector_backing_free(
	struct drv_dma_vector *vector)
{
	/* Returns the coherent storage; a refused release keeps the vector. */
	if (vector->contiguous.address != NULL) {
		drv_dma_free_coherent(vector->device, &vector->contiguous);

		/* Handles the address availability. */
		if (vector->contiguous.address != NULL)
			return EBUSY;
	}

	vector->address = NULL;

	/* Succeeded. */
	return 0;
}
