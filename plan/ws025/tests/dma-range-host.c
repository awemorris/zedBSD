/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#define main legacy_dma_fixture_main
#define hal_pmem_alloc legacy_fixture_alloc
#define hal_pmem_free legacy_fixture_free
#include "../../ws004/tests/dma-constraints-test.c"
#undef main
#undef hal_pmem_alloc
#undef hal_pmem_free
#include "src/hal/amd64/pmem-range.h"

static struct amd64_pmem_extent pools[2];
static uint64_t metadata[2][64];
static uint64_t observed_maximum;
static uint64_t observed_boundary;
static unsigned constrained_calls;
static int reject_free;

int hal_pmem_alloc(hal_physaddr_t request_paddr, size_t request_size, size_t request_alignment, uint32_t request_type, uint32_t request_attr, struct hal_pmem *memory)
{
	(void)request_paddr; (void)request_size; (void)request_alignment; (void)request_type; (void)request_attr; (void)memory;
	assert(!"DMA must enter the constrained allocator");
	return HAL_ERR_UNSUPPORTED;
}

int hal_pmem_alloc_range(hal_physaddr_t request_paddr, size_t request_size, size_t request_alignment, uint32_t request_type, uint32_t request_attr,
    uint64_t minimum, uint64_t maximum, uint64_t boundary, struct hal_pmem *memory)
{
	(void)request_paddr; (void)request_type; (void)request_attr;

	unsigned i;
	uint64_t physical, size;
	enum amd64_pmem_result result;
	observed_maximum = maximum; observed_boundary = boundary; constrained_calls++;
	for (i = 2; i != 0; i--) {
		result = amd64_pmem_extent_alloc(&pools[i - 1], request_size,
		    request_alignment, minimum, maximum, boundary, &physical, &size);
		if (result == AMD64_PMEM_NOMEM) continue;
		assert(result == AMD64_PMEM_OK);
		memory->vaddr = malloc((size_t)size);
		assert(memory->vaddr != NULL);
		memory->paddr = physical; memory->size = (size_t)size;
		memory->type = HAL_PMEM_TYPE_RAM; memory->attr = 0;
		return HAL_OK;
	}
	return HAL_ERR_NOMEM;
}

int hal_pmem_free(struct hal_pmem *memory)
{
	unsigned i = memory->paddr >= 0x100000000ULL;
	if (reject_free) return HAL_ERR_NOMEM;
	assert(amd64_pmem_extent_free(&pools[i], memory->paddr, memory->size) == AMD64_PMEM_OK);
	free(memory->vaddr);
	memset(memory, 0, sizeof(*memory));
	return HAL_OK;
}

int dma_range_run(void)
{
	struct drv_dma_constraints constraints = {32, 65536, 65536, 1};
	struct drv_dma_device *device;
	struct drv_dma_buffer buffer;
	struct drv_dma_mapping *mapping;
	struct drv_dma_segment segment;
	unsigned i;
	for (i = 0; i < 2; i++)
		assert(amd64_pmem_extent_init(&pools[i], i ? 0x100000000ULL : 0x100000,
		    512 * 4096, metadata[i], sizeof(metadata[i])) == AMD64_PMEM_OK);
	assert(drv_dma_device_create(&constraints, &device) == 0);
	assert(drv_dma_alloc_coherent(device, 5000, 4096, &buffer) == 0);
	assert(observed_maximum == UINT32_MAX && observed_boundary == 65536);
	assert(buffer.device_address < 0x100000000ULL && pools[1].allocated_pages == 0);
	assert(drv_dma_map(device, buffer.address, buffer.size, DRV_DMA_BIDIRECTIONAL, &mapping) == 0);
	assert(drv_dma_mapping_segment(mapping, 0, &segment) == 0);
	assert(segment.address == buffer.device_address && segment.length == buffer.size);
	drv_dma_unmap(device, mapping);
	drv_dma_free_coherent(device, &buffer);
	assert(amd64_pmem_reserve(&pools[0], pools[0].base, pools[0].pages * 4096) == AMD64_PMEM_OK);
	assert(drv_dma_alloc_coherent(device, 4096, 4096, &buffer) == ENOMEM);
	assert(pools[1].free_pages == 512);
	assert(drv_dma_device_destroy(device) == 0);
	constraints.address_bits = 64;
	assert(drv_dma_device_create(&constraints, &device) == 0);
	assert(drv_dma_alloc_coherent(device, 4096, 4096, &buffer) == 0);
	assert(observed_maximum == UINT64_MAX && buffer.device_address >= 0x100000000ULL);
	drv_dma_free_coherent(device, &buffer);
	assert(drv_dma_device_destroy(device) == 0);
	constraints.max_segment_size = 64; constraints.segment_boundary = 64;
	assert(drv_dma_device_create(&constraints, &device) == 0);
	assert(drv_dma_alloc_coherent(device, 64, 16, &buffer) == 0);
	assert(observed_boundary == 0 && buffer.device_address % 64 == 0);
	assert(drv_dma_map(device, buffer.address, 65, DRV_DMA_TO_DEVICE, &mapping) == EINVAL);
	assert(drv_dma_map(device, (char *)buffer.address + 32, 64, DRV_DMA_TO_DEVICE, &mapping) == ENOTSUP);
	assert(drv_dma_map(device, buffer.address, 64, (enum drv_dma_direction)-1, &mapping) == EINVAL);
	assert(drv_dma_map(device, (char *)buffer.address + 32, 32, DRV_DMA_TO_DEVICE, &mapping) == 0);
	drv_dma_unmap(device, mapping);
	drv_dma_free_coherent(device, &buffer);
	assert(drv_dma_device_destroy(device) == 0);
	assert(constrained_calls == 4 && pools[1].free_pages == 512);
	return 0;
}

#ifndef WS025_DMA_RANGE_EMBEDDED
int main(void) { return dma_range_run(); }
#endif
