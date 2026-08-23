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
	uint64_t segment_boundary;
	int coherent;
};

int
drv_dma_device_create(
	const struct drv_dma_constraints *,
	struct drv_dma_device **);
int
drv_dma_device_destroy(
	struct drv_dma_device *);

unsigned
drv_dma_device_address_bits(
	const struct drv_dma_device *);
size_t
drv_dma_device_max_segment_size(
	const struct drv_dma_device *);
int
drv_dma_device_is_coherent(
	const struct drv_dma_device *);

int
drv_dma_alloc_coherent(
	struct drv_dma_device *,
	size_t,
	size_t,
	struct drv_dma_buffer *);
void
drv_dma_free_coherent(
	struct drv_dma_device *,
	struct drv_dma_buffer *);

int
drv_dma_map(
	struct drv_dma_device *,
	void *,
	size_t,
	enum drv_dma_direction,
	struct drv_dma_mapping **);
void
drv_dma_unmap(
	struct drv_dma_device *,
	struct drv_dma_mapping *);

unsigned
drv_dma_mapping_segment_count(
	const struct drv_dma_mapping *);
int
drv_dma_mapping_segment(
	const struct drv_dma_mapping *,
	unsigned,
	struct drv_dma_segment *);
void
drv_dma_sync_for_cpu(
	struct drv_dma_device *,
	struct drv_dma_mapping *);
void
drv_dma_sync_for_device(
	struct drv_dma_device *,
	struct drv_dma_mapping *);

#endif
