/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Generic device DMA mapping interface
 *
 * This interface was designed with reference to the DMA mapping APIs of
 * Linux, FreeBSD, NetBSD, and OpenBSD.  It is an independent zedBSD
 * interface; no source code from those kernels is included in this file.
 */

#ifndef ZEDBSD_DRIVERS_DMA_H
#define ZEDBSD_DRIVERS_DMA_H

#include <stddef.h>
#include <stdint.h>

struct drv_dma_device;
struct drv_dma_mapping;

enum drv_dma_direction {
	DRV_DMA_TO_DEVICE,
	DRV_DMA_FROM_DEVICE,
	DRV_DMA_BIDIRECTIONAL
};

struct drv_dma_segment {
	uint64_t address;
	size_t length;
};

struct drv_dma_buffer {
	void *address;
	uint64_t device_address;
	size_t size;
	uintptr_t private_data[2];
};

struct drv_dma_constraints {
	unsigned address_bits;
	size_t max_segment_size;
	/* Power-of-two byte boundary that a segment must not cross, or zero. */
	uint64_t segment_boundary;
	int coherent;
};

int drv_dma_device_create(const struct drv_dma_constraints *constraints,
			  struct drv_dma_device **result);
int drv_dma_device_destroy(struct drv_dma_device *device);

unsigned drv_dma_device_address_bits(const struct drv_dma_device *device);
size_t drv_dma_device_max_segment_size(const struct drv_dma_device *device);
int drv_dma_device_is_coherent(const struct drv_dma_device *device);

int drv_dma_alloc_coherent(struct drv_dma_device *device, size_t size,
			   size_t alignment, struct drv_dma_buffer *buffer);
void drv_dma_free_coherent(struct drv_dma_device *device,
			   struct drv_dma_buffer *buffer);

int drv_dma_map(struct drv_dma_device *device, void *address, size_t size,
		enum drv_dma_direction direction,
		struct drv_dma_mapping **result);
void drv_dma_unmap(struct drv_dma_device *device,
		   struct drv_dma_mapping *mapping);

unsigned drv_dma_mapping_segment_count(const struct drv_dma_mapping *mapping);
int drv_dma_mapping_segment(const struct drv_dma_mapping *mapping,
			    unsigned index, struct drv_dma_segment *segment);
void drv_dma_sync_for_cpu(struct drv_dma_device *device,
			  struct drv_dma_mapping *mapping);
void drv_dma_sync_for_device(struct drv_dma_device *device,
			     struct drv_dma_mapping *mapping);

#endif
