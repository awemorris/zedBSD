/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

extern unsigned hal_vmap_capabilities(void) __attribute__((weak));
extern int hal_vmap_reserve(size_t, struct hal_vmap **) __attribute__((weak));
extern int hal_vmap_populate(struct hal_vmap *, uint64_t, uint64_t) __attribute__((weak));
extern int hal_vmap_pin(struct hal_vmap *, void **) __attribute__((weak));
extern void hal_vmap_unpin(struct hal_vmap *) __attribute__((weak));
extern int hal_vmap_release(struct hal_vmap *) __attribute__((weak));
extern int hal_kernel_page_lookup(const void *, hal_physaddr_t *) __attribute__((weak));

struct drv_dma_vector {
	struct drv_dma_device *device;
	struct hal_vmap *mapping;
	struct drv_dma_buffer contiguous;
	void *address;
	size_t size, charged;
	unsigned count;
	struct drv_dma_segment segments[DRV_DMA_VECTOR_MAX_SEGMENTS];
};

static int dma_vector_segments(struct drv_dma_vector *vector);
static int dma_vector_backing_free(struct drv_dma_vector *vector);

/* Owns isolated DMA staging, never an arbitrary caller's reusable buffer. */
int
drv_dma_vector_create(
	struct drv_dma_device *device,
	size_t size,
	struct drv_dma_vector **result)
{
	struct drv_dma_vector *vector;
	uint64_t maximum;
	size_t rounded;
	unsigned long irq;
	int error;

	if (result == NULL)
		return EINVAL;
	*result = NULL;
	if (device == NULL || size == 0 || size > DRV_DMA_VECTOR_MAX_SIZE)
		return EINVAL;
	error = device_operation_begin(device, 0);
	if (error != 0)
		return error;
	if (!device->constraints.coherent) {
		device_operation_end(device);
		return EOPNOTSUPP;
	}
	vector = hal_malloc(sizeof(*vector));
	if (vector == NULL) {
		device_operation_end(device);
		return ENOMEM;
	}
	memset(vector, 0, sizeof(*vector));
	vector->device = device;
	vector->size = size;
	vector->charged = sizeof(*vector);
	maximum = device->constraints.address_bits == 64U ? UINT64_MAX :
	    ((UINT64_C(1) << device->constraints.address_bits) - 1U);
	rounded = (size + 4095U) & ~(size_t)4095U;
	error = EOPNOTSUPP;

	/* The optional HAL owns page lifetime and supplies checked per-page PAs. */
	if (hal_vmap_capabilities != NULL && hal_vmap_reserve != NULL &&
	    hal_vmap_populate != NULL && hal_vmap_pin != NULL &&
	    hal_vmap_unpin != NULL && hal_vmap_release != NULL &&
	    hal_kernel_page_lookup != NULL && hal_vmap_capabilities() != 0 &&
	    hal_vmap_reserve(rounded, &vector->mapping) == HAL_OK) {
		error = hal_vmap_populate(vector->mapping, 0, maximum);
		if (error == HAL_OK)
			error = hal_vmap_pin(vector->mapping, &vector->address);
		if (error == HAL_OK)
			error = dma_vector_segments(vector);
		if (error != 0) {
			if (dma_vector_backing_free(vector) != 0)
				HAL_FATAL("DMA vector preparation rollback failed");
		} else {
			vector->charged += rounded;
		}
	}

	/* Constrained coherent storage is the mask/capability fallback. */
	if (vector->mapping == NULL) {
		error = drv_dma_alloc_coherent(device, size, 64U, &vector->contiguous);
		if (error == 0) {
			vector->address = vector->contiguous.address;
			error = dma_vector_segments(vector);
		}
	}
	if (error != 0)
		goto fail;
	if (cache_memory_reserve != NULL) {
		error = cache_memory_reserve(CACHE_MEMORY_DMA, vector->charged, 0);
		if (error != 0)
			goto fail;
	}
	irq = spin_lock_irqsave(&device->lock);
	if (device->destroying || device->vector_count == UINT_MAX) {
		spin_unlock_irqrestore(&device->lock, irq);
		if (cache_memory_cancel != NULL)
			cache_memory_cancel(CACHE_MEMORY_DMA, vector->charged);
		error = EBUSY;
		goto fail;
	}
	device->vector_count++;
	spin_unlock_irqrestore(&device->lock, irq);
	if (cache_memory_commit != NULL)
		cache_memory_commit(CACHE_MEMORY_DMA, vector->charged);
	io_stats_record(IO_DMA_ALLOC, vector->charged);
	*result = vector;
	device_operation_end(device);
	return 0;

fail:
	if (dma_vector_backing_free(vector) != 0)
		HAL_FATAL("DMA vector allocation rollback failed");
	hal_free(vector);
	device_operation_end(device);
	return error;
}

int
drv_dma_vector_free(
	struct drv_dma_vector *vector)
{
	struct drv_dma_device *device;
	unsigned long irq;
	int error;

	if (vector == NULL)
		return EINVAL;
	device = vector->device;
	error = device_operation_begin(device, 1);
	if (error != 0)
		return error;
	error = dma_vector_backing_free(vector);
	if (error != 0) {
		device_operation_end(device);
		return error;
	}
	if (cache_memory_release != NULL)
		cache_memory_release(CACHE_MEMORY_DMA, vector->charged);
	io_stats_record(IO_DMA_FREE, vector->charged);
	irq = spin_lock_irqsave(&device->lock);
	if (device->vector_count == 0)
		HAL_FATAL("DMA vector owner underflow");
	device->vector_count--;
	spin_unlock_irqrestore(&device->lock, irq);
	hal_free(vector);
	device_operation_end(device);
	return 0;
}

void *
drv_dma_vector_address(
	const struct drv_dma_vector *vector)
{
	return vector != NULL ? vector->address : NULL;
}

unsigned
drv_dma_vector_count(
	const struct drv_dma_vector *vector)
{
	return vector != NULL ? vector->count : 0;
}

int
drv_dma_vector_segment(
	const struct drv_dma_vector *vector,
	unsigned index,
	struct drv_dma_segment *segment)
{
	if (vector == NULL || segment == NULL || index >= vector->count)
		return EINVAL;
	*segment = vector->segments[index];
	return 0;
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
	while (offset < vector->size) {
		if (vector->mapping != NULL) {
			if (hal_kernel_page_lookup((char *)vector->address + offset,
			    &physical) != HAL_OK)
				return EIO;
			length = 4096U - offset % 4096U;
		} else {
			physical = vector->contiguous.device_address + offset;
			length = vector->size - offset;
		}
		remaining = vector->size - offset;
		if (length > remaining)
			length = remaining;
		if (length > device->constraints.max_segment_size)
			length = device->constraints.max_segment_size;
		if (boundary != 0 && length > boundary - physical % boundary)
			length = (size_t)(boundary - physical % boundary);
		if (!address_fits(device, physical, length))
			return EOVERFLOW;
		previous = vector->count != 0 ? &vector->segments[vector->count - 1U] : NULL;
		if (previous != NULL && previous->length <= UINT64_MAX - previous->address &&
		    previous->address + previous->length == physical &&
		    length <= device->constraints.max_segment_size - previous->length &&
		    (boundary == 0 || previous->address / boundary == physical / boundary)) {
			previous->length += length;
		} else {
			if (vector->count == DRV_DMA_VECTOR_MAX_SEGMENTS)
				return E2BIG;
			vector->segments[vector->count].address = physical;
			vector->segments[vector->count++].length = length;
		}
		offset += length;
	}
	return 0;
}

static int
dma_vector_backing_free(
	struct drv_dma_vector *vector)
{
	if (vector->mapping != NULL) {
		if (vector->address != NULL)
			hal_vmap_unpin(vector->mapping);
		if (hal_vmap_release(vector->mapping) != HAL_OK) {
			if (vector->address != NULL &&
			    hal_vmap_pin(vector->mapping, &vector->address) != HAL_OK)
				HAL_FATAL("DMA vector release lost its pin");
			return EBUSY;
		}
		vector->mapping = NULL;
	} else if (vector->contiguous.address != NULL) {
		drv_dma_free_coherent(vector->device, &vector->contiguous);
		if (vector->contiguous.address != NULL)
			return EBUSY;
	}
	vector->address = NULL;
	return 0;
}
