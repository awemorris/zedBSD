#include <drivers/dma.h>
#include <hal/hal.h>

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

static size_t observed_alignment;

void *
hal_malloc(size_t size)
{
	return malloc(size);
}

void
hal_free(void *pointer)
{
	free(pointer);
}

size_t
hal_page_get_page_size(int level)
{
	(void)level;
	return 4096;
}

int
hal_pmem_alloc(const struct hal_pmem_request *request, struct hal_pmem *memory)
{
	observed_alignment = request->alignment;
	memory->vaddr = malloc(request->size);
	if (memory->vaddr == NULL)
		return HAL_ERR_NOMEM;
	memory->paddr = request->alignment;
	memory->size = request->size;
	memory->type = request->type;
	memory->attr = request->attr;
	return HAL_OK;
}

int
hal_pmem_free(struct hal_pmem *memory)
{
	free(memory->vaddr);
	return HAL_OK;
}

int
main(void)
{
	struct drv_dma_device *device;
	struct drv_dma_buffer buffer;
	struct drv_dma_constraints constraints = {
	    .address_bits = 32,
	    .max_segment_size = 65536,
	    .segment_boundary = 65536,
	    .coherent = 1,
	};

	assert(drv_dma_device_create(&constraints, &device) == 0);
	assert(drv_dma_alloc_coherent(device, 4096, 16, &buffer) == 0);
	assert(observed_alignment == 65536);
	drv_dma_free_coherent(device, &buffer);
	assert(drv_dma_device_destroy(device) == 0);

	constraints.segment_boundary = 60000;
	assert(drv_dma_device_create(&constraints, &device) == EINVAL);
	constraints.segment_boundary = 32768;
	assert(drv_dma_device_create(&constraints, &device) == EINVAL);
	return 0;
}
