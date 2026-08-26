/* Generic no-IOMMU DMA implementation. Copyright (C) 2026 Awe Morris;
 * SPDX-License-Identifier: Zlib */
#include <drivers/dma.h>
#include <errno.h>
#include <hal/hal.h>
#include <kern/lock.h>
#include <limits.h>
#include <string.h>

struct dma_allocation {
	struct hal_pmem memory;
	struct dma_allocation *next;
};

struct drv_dma_device {
	struct drv_dma_constraints constraints;
	struct spinlock lock;
	struct dma_allocation *allocations;
	unsigned active_operations;
	unsigned destroying;
};

struct drv_dma_mapping {
	struct drv_dma_segment segment;
	enum drv_dma_direction direction;
};

/*
 * A DMA device is normally shared by every device on one bus.  In particular,
 * two host controllers can allocate and release coherent buffers from IRQ and
 * process context at the same time.  The spinlock protects only the device
 * lifecycle and allocation-list metadata; the physical-memory allocator and
 * heap allocator must never be entered while it is held.
 */
static int
device_operation_begin(struct drv_dma_device *device, int allow_destroying)
{
	unsigned long irq;
	int error = 0;

	irq = spin_lock_irqsave(&device->lock);
	if (device->destroying && !allow_destroying)
		error = EBUSY;
	else if (device->active_operations == UINT_MAX)
		error = EOVERFLOW;
	else
		device->active_operations++;
	spin_unlock_irqrestore(&device->lock, irq);
	return error;
}

static void
device_operation_end(struct drv_dma_device *device)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&device->lock);
	if (device->active_operations == 0)
		__builtin_trap();
	device->active_operations--;
	spin_unlock_irqrestore(&device->lock, irq);
}

static int
address_fits(const struct drv_dma_device *device, uint64_t address, size_t size)
{
	uint64_t limit;
	if (device->constraints.address_bits >= 64U)
		return 1;
	limit = (uint64_t)1U << device->constraints.address_bits;
	return address < limit && size <= limit - address;
}

static int
is_power_of_two(uint64_t value)
{
	return value != 0 && (value & (value - 1U)) == 0;
}

int
drv_dma_device_create(const struct drv_dma_constraints *constraints,
		      struct drv_dma_device **result)
{
	struct drv_dma_device *device;
	if (constraints == NULL || result == NULL ||
	    constraints->address_bits == 0 || constraints->address_bits > 64 ||
	    constraints->max_segment_size == 0 ||
	    (constraints->segment_boundary != 0 &&
	     (!is_power_of_two(constraints->segment_boundary) ||
	      constraints->max_segment_size > constraints->segment_boundary)))
		return EINVAL;
	device = hal_malloc(sizeof(*device));
	if (device == NULL)
		return ENOMEM;
	memset(device, 0, sizeof(*device));
	device->constraints = *constraints;
	spin_init(&device->lock, LOCK_RANK_DEVICE, "DMA allocation list");
	*result = device;
	return 0;
}

int
drv_dma_device_destroy(struct drv_dma_device *device)
{
	unsigned long irq;

	if (device == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&device->lock);
	device->destroying = 1;
	if (device->allocations != NULL || device->active_operations != 0) {
		spin_unlock_irqrestore(&device->lock, irq);
		return EBUSY;
	}
	spin_unlock_irqrestore(&device->lock, irq);
	hal_free(device);
	return 0;
}

unsigned
drv_dma_device_address_bits(const struct drv_dma_device *device)
{
	unsigned result;

	if (device == NULL || device_operation_begin(
	    (struct drv_dma_device *)device, 0) != 0)
		return 0;
	result = device->constraints.address_bits;
	device_operation_end((struct drv_dma_device *)device);
	return result;
}
size_t
drv_dma_device_max_segment_size(const struct drv_dma_device *device)
{
	size_t result;

	if (device == NULL || device_operation_begin(
	    (struct drv_dma_device *)device, 0) != 0)
		return 0;
	result = device->constraints.max_segment_size;
	device_operation_end((struct drv_dma_device *)device);
	return result;
}
int
drv_dma_device_is_coherent(const struct drv_dma_device *device)
{
	int result;

	if (device == NULL || device_operation_begin(
	    (struct drv_dma_device *)device, 0) != 0)
		return 0;
	result = device->constraints.coherent;
	device_operation_end((struct drv_dma_device *)device);
	return result;
}

int
drv_dma_alloc_coherent(struct drv_dma_device *device, size_t size,
		       size_t alignment, struct drv_dma_buffer *buffer)
{
	struct hal_pmem_request request;
	struct dma_allocation *allocation;
	unsigned long irq;
	int error;
	if (device == NULL || buffer == NULL || size == 0)
		return EINVAL;
	error = device_operation_begin(device, 0);
	if (error != 0)
		return error;
	if (size > device->constraints.max_segment_size) {
		device_operation_end(device);
		return EINVAL;
	}
	allocation = hal_malloc(sizeof(*allocation));
	if (allocation == NULL) {
		device_operation_end(device);
		return ENOMEM;
	}
	memset(allocation, 0, sizeof(*allocation));
	request.paddr = HAL_PMEM_PADDR_ANY;
	request.size = size;
	if (alignment < hal_page_get_page_size(1))
		alignment = hal_page_get_page_size(1);
	if (device->constraints.segment_boundary != 0 &&
	    alignment < device->constraints.segment_boundary)
		alignment = device->constraints.segment_boundary;
	request.alignment = alignment;
	request.type = HAL_PMEM_TYPE_RAM;
	request.attr = 0;
	error = hal_pmem_alloc(&request, &allocation->memory);
	if (error != HAL_OK || !address_fits(device, allocation->memory.paddr,
					     allocation->memory.size)) {
		if (error == HAL_OK)
			(void)hal_pmem_free(&allocation->memory);
		hal_free(allocation);
		device_operation_end(device);
		return error == HAL_OK ? ENOMEM : ENOMEM;
	}
	irq = spin_lock_irqsave(&device->lock);
	if (device->destroying) {
		spin_unlock_irqrestore(&device->lock, irq);
		(void)hal_pmem_free(&allocation->memory);
		hal_free(allocation);
		device_operation_end(device);
		return EBUSY;
	}
	allocation->next = device->allocations;
	device->allocations = allocation;
	spin_unlock_irqrestore(&device->lock, irq);
	buffer->address = allocation->memory.vaddr;
	buffer->device_address = allocation->memory.paddr;
	buffer->size = size;
	buffer->private_data[0] = (uintptr_t)allocation;
	buffer->private_data[1] = 0;
	device_operation_end(device);
	return 0;
}

void
drv_dma_free_coherent(struct drv_dma_device *device,
		      struct drv_dma_buffer *buffer)
{
	struct dma_allocation **link, *allocation;
	unsigned long irq;
	int found = 0;

	if (device == NULL || buffer == NULL || buffer->private_data[0] == 0)
		return;
	if (device_operation_begin(device, 1) != 0)
		return;
	irq = spin_lock_irqsave(&device->lock);
	allocation = (struct dma_allocation *)buffer->private_data[0];
	for (link = &device->allocations; *link != NULL; link = &(*link)->next)
		if (*link == allocation) {
			*link = allocation->next;
			found = 1;
			break;
		}
	spin_unlock_irqrestore(&device->lock, irq);
	if (found) {
		(void)hal_pmem_free(&allocation->memory);
		hal_free(allocation);
		memset(buffer, 0, sizeof(*buffer));
	}
	device_operation_end(device);
}

int
drv_dma_map(struct drv_dma_device *device, void *address, size_t size,
	    enum drv_dma_direction direction, struct drv_dma_mapping **result)
{
	struct dma_allocation *allocation;
	struct drv_dma_mapping *mapping;
	unsigned long irq;
	uintptr_t start = (uintptr_t)address;
	int error;

	if (device == NULL || address == NULL || size == 0 || result == NULL ||
	    direction > DRV_DMA_BIDIRECTIONAL)
		return EINVAL;
	error = device_operation_begin(device, 0);
	if (error != 0)
		return error;
	mapping = hal_malloc(sizeof(*mapping));
	if (mapping == NULL) {
		device_operation_end(device);
		return ENOMEM;
	}
	irq = spin_lock_irqsave(&device->lock);
	if (device->destroying) {
		spin_unlock_irqrestore(&device->lock, irq);
		hal_free(mapping);
		device_operation_end(device);
		return EBUSY;
	}
	for (allocation = device->allocations; allocation != NULL;
	     allocation = allocation->next) {
		uintptr_t base = (uintptr_t)allocation->memory.vaddr;
		if (start < base || start - base > allocation->memory.size ||
		    size > allocation->memory.size - (start - base))
			continue;
		mapping->segment.address =
		    allocation->memory.paddr + start - base;
		mapping->segment.length = size;
		mapping->direction = direction;
		spin_unlock_irqrestore(&device->lock, irq);
		*result = mapping;
		device_operation_end(device);
		return 0;
	}
	spin_unlock_irqrestore(&device->lock, irq);
	hal_free(mapping);
	device_operation_end(device);
	return ENOTSUP;
}

void
drv_dma_unmap(struct drv_dma_device *device, struct drv_dma_mapping *mapping)
{
	(void)device;
	if (mapping != NULL)
		hal_free(mapping);
}
unsigned
drv_dma_mapping_segment_count(const struct drv_dma_mapping *mapping)
{
	return mapping == NULL ? 0U : 1U;
}
int
drv_dma_mapping_segment(const struct drv_dma_mapping *mapping, unsigned index,
			struct drv_dma_segment *segment)
{
	if (mapping == NULL || segment == NULL || index != 0)
		return EINVAL;
	*segment = mapping->segment;
	return 0;
}
void
drv_dma_sync_for_cpu(struct drv_dma_device *device,
		     struct drv_dma_mapping *mapping)
{
	(void)device;
	(void)mapping;
}
void
drv_dma_sync_for_device(struct drv_dma_device *device,
			struct drv_dma_mapping *mapping)
{
	(void)device;
	(void)mapping;
}
