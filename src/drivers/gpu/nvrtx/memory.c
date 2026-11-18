/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Register access and DMA memory of the nvrtx driver.
 *
 * The registers are the GPU's first memory window, mapped uncached for as
 * long as the PCI binding lasts.  The memory the GSP reads is coherent DMA
 * from the PCI function's DMA device: physically contiguous, page aligned,
 * and up to the bus's segment limit each.
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/dma.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gpu.h"

/*
 * Reads a 32-bit register of the register window.
 */
uint32_t
nvrtx_rd32(
	const struct nvrtx_gpu *gpu,
	uint32_t offset)
{
	const uint8_t *base;
	uint32_t value;

	/* Reads the register through the uncached mapping. */
	base = (const uint8_t *)gpu->registers.address;
	value = kern_mmio_read32(base + offset);

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Writes a 32-bit register of the register window.
 */
void
nvrtx_wr32(
	const struct nvrtx_gpu *gpu,
	uint32_t offset,
	uint32_t value)
{
	uint8_t *base;

	/* Writes the register through the uncached mapping. */
	base = (uint8_t *)gpu->registers.address;
	kern_mmio_write32(base + offset, value);
}

/*
 * Allocates one zeroed coherent DMA buffer of whole pages.
 */
int
nvrtx_buffer_alloc(
	struct nvrtx_gpu *gpu,
	size_t size,
	struct nvrtx_buffer *buffer)
{
	size_t pages;
	int error;

	/* Rounds the size up to whole GSP pages. */
	pages = (size + NVRTX_GSP_PAGE_SIZE - 1U) / NVRTX_GSP_PAGE_SIZE;

	/* Allocates the buffer page aligned. */
	error = drv_dma_alloc_coherent(gpu->dma, pages * NVRTX_GSP_PAGE_SIZE, NVRTX_GSP_PAGE_SIZE, &buffer->dma);
	if (error != 0)
		return error;

	/* Clears it, so every field the driver does not write is zero for the GSP. */
	kern_memset(buffer->dma.address, 0, pages * NVRTX_GSP_PAGE_SIZE);
	buffer->allocated = 1;

	/* Succeeded: the buffer is the caller's until it is freed. */
	return 0;
}

/*
 * Frees a buffer; a buffer never allocated is left alone.
 */
void
nvrtx_buffer_free(
	struct nvrtx_gpu *gpu,
	struct nvrtx_buffer *buffer)
{
	/* Only an allocated buffer has memory to give back. */
	if (buffer->allocated == 0)
		return;

	/* Gives the memory back and forgets it. */
	drv_dma_free_coherent(gpu->dma, &buffer->dma);
	buffer->allocated = 0;
}
