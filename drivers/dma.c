/* Generic no-IOMMU DMA implementation. Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <drivers/dma.h>
#include <errno.h>
#include <hal/hal.h>
#include <string.h>

struct dma_allocation {
	struct hal_pmem memory;
	struct dma_allocation *next;
};

struct drv_dma_device {
	struct drv_dma_constraints constraints;
	struct dma_allocation *allocations;
};

struct drv_dma_mapping {
	struct drv_dma_segment segment;
	enum drv_dma_direction direction;
};

static int address_fits(const struct drv_dma_device *device,
	uint64_t address, size_t size)
{
	uint64_t limit;
	if (device->constraints.address_bits >= 64U)
		return 1;
	limit = (uint64_t)1U << device->constraints.address_bits;
	return address < limit && size <= limit - address;
}

int drv_dma_device_create(const struct drv_dma_constraints *constraints,
	struct drv_dma_device **result)
{
	struct drv_dma_device *device;
	if (constraints == NULL || result == NULL ||
	    constraints->address_bits == 0 || constraints->address_bits > 64 ||
	    constraints->max_segment_size == 0)
		return EINVAL;
	device = hal_malloc(sizeof(*device));
	if (device == NULL)
		return ENOMEM;
	memset(device, 0, sizeof(*device));
	device->constraints = *constraints;
	*result = device;
	return 0;
}

int drv_dma_device_destroy(struct drv_dma_device *device)
{
	if (device == NULL)
		return EINVAL;
	if (device->allocations != NULL)
		return EBUSY;
	hal_free(device);
	return 0;
}

unsigned drv_dma_device_address_bits(const struct drv_dma_device *device)
{ return device == NULL ? 0 : device->constraints.address_bits; }
size_t drv_dma_device_max_segment_size(const struct drv_dma_device *device)
{ return device == NULL ? 0 : device->constraints.max_segment_size; }
int drv_dma_device_is_coherent(const struct drv_dma_device *device)
{ return device != NULL && device->constraints.coherent; }

int drv_dma_alloc_coherent(struct drv_dma_device *device, size_t size,
	size_t alignment, struct drv_dma_buffer *buffer)
{
	struct hal_pmem_request request;
	struct dma_allocation *allocation;
	int error;
	if (device == NULL || buffer == NULL || size == 0 ||
	    size > device->constraints.max_segment_size)
		return EINVAL;
	allocation = hal_malloc(sizeof(*allocation));
	if (allocation == NULL)
		return ENOMEM;
	memset(allocation, 0, sizeof(*allocation));
	request.paddr = HAL_PMEM_PADDR_ANY;
	request.size = size;
	request.alignment = alignment;
	request.type = HAL_PMEM_TYPE_RAM;
	request.attr = 0;
	error = hal_pmem_alloc(&request, &allocation->memory);
	if (error != HAL_OK || !address_fits(device, allocation->memory.paddr,
	    allocation->memory.size)) {
		if (error == HAL_OK)
			(void)hal_pmem_free(&allocation->memory);
		hal_free(allocation);
		return error == HAL_OK ? ENOMEM : ENOMEM;
	}
	allocation->next = device->allocations;
	device->allocations = allocation;
	buffer->address = allocation->memory.vaddr;
	buffer->device_address = allocation->memory.paddr;
	buffer->size = size;
	buffer->private_data[0] = (uintptr_t)allocation;
	buffer->private_data[1] = 0;
	return 0;
}

void drv_dma_free_coherent(struct drv_dma_device *device,
	struct drv_dma_buffer *buffer)
{
	struct dma_allocation **link, *allocation;
	if (device == NULL || buffer == NULL || buffer->private_data[0] == 0)
		return;
	allocation = (struct dma_allocation *)buffer->private_data[0];
	for (link = &device->allocations; *link != NULL; link = &(*link)->next)
		if (*link == allocation) {
			*link = allocation->next;
			(void)hal_pmem_free(&allocation->memory);
			hal_free(allocation);
			memset(buffer, 0, sizeof(*buffer));
			return;
		}
}

int drv_dma_map(struct drv_dma_device *device, void *address, size_t size,
	enum drv_dma_direction direction, struct drv_dma_mapping **result)
{
	struct dma_allocation *allocation;
	struct drv_dma_mapping *mapping;
	uintptr_t start = (uintptr_t)address;
	if (device == NULL || address == NULL || size == 0 || result == NULL ||
	    direction > DRV_DMA_BIDIRECTIONAL)
		return EINVAL;
	for (allocation = device->allocations; allocation != NULL;
	    allocation = allocation->next) {
		uintptr_t base = (uintptr_t)allocation->memory.vaddr;
		if (start < base || start - base > allocation->memory.size ||
		    size > allocation->memory.size - (start - base))
			continue;
		mapping = hal_malloc(sizeof(*mapping));
		if (mapping == NULL)
			return ENOMEM;
		mapping->segment.address = allocation->memory.paddr + start - base;
		mapping->segment.length = size;
		mapping->direction = direction;
		*result = mapping;
		return 0;
	}
	return ENOTSUP;
}

void drv_dma_unmap(struct drv_dma_device *device,
	struct drv_dma_mapping *mapping)
{ (void)device; if (mapping != NULL) hal_free(mapping); }
unsigned drv_dma_mapping_segment_count(const struct drv_dma_mapping *mapping)
{ return mapping == NULL ? 0U : 1U; }
int drv_dma_mapping_segment(const struct drv_dma_mapping *mapping,
	unsigned index, struct drv_dma_segment *segment)
{
	if (mapping == NULL || segment == NULL || index != 0)
		return EINVAL;
	*segment = mapping->segment;
	return 0;
}
void drv_dma_sync_for_cpu(struct drv_dma_device *device,
	struct drv_dma_mapping *mapping) { (void)device; (void)mapping; }
void drv_dma_sync_for_device(struct drv_dma_device *device,
	struct drv_dma_mapping *mapping) { (void)device; (void)mapping; }
