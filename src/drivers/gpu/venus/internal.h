/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Private PCI transport shared only by the Venus backend implementation.
 */

#ifndef DRIVERS_GPU_VENUS_INTERNAL_H
#define DRIVERS_GPU_VENUS_INTERNAL_H

#include <drivers/dma.h>
#include <drivers/pci.h>
#include <kern/lock.h>
#include <stdint.h>

#define VENUS_COMMAND_BYTES 65568U
#define VENUS_RESPONSE_BYTES 4096U
#define VENUS_QUEUE_SIZE 8U
#define VENUS_HEADER_BYTES 24U
#define VENUS_MAX_RESOURCE_BYTES (16U * 1024U * 1024U)
#define VENUS_MAX_APERTURE_BYTES (8U * 1024U * 1024U)

/*
 * One PCI capability window, retained until the device reset completes.
 */
struct venus_window {
	struct drv_pci_mapping mapping;
	unsigned bar;
	uint64_t offset;
	uint64_t length;
};

/*
 * One transport, serialized by its backend mutex during GPU callbacks.
 *
 * Coherent request, response and ring allocations survive command timeouts.
 * Stop must observe device reset before any allocation or mapping is freed.
 */
struct venus_transport {
	struct drv_pci_device *pci;
	const char *stage;
	struct drv_dma_device *dma;
	struct drv_pci_enable_state enable_state;
	struct venus_window common;
	struct venus_window notify;
	struct venus_window configuration;
	struct venus_window host_visible;
	struct drv_pci_mapping registers[6];
	struct drv_pci_mapping host_mapping;
	struct drv_dma_buffer ring;
	struct drv_dma_buffer request;
	struct drv_dma_buffer response;
	uint32_t notify_multiplier;
	uint32_t capset_size;
	uint32_t features;
	uint16_t available;
	uint16_t used;
	uint16_t notify_offset;
	unsigned saved;
	unsigned enabled;
	unsigned failed;
	unsigned claimed[6];
};

int drv_venus_transport_start(struct venus_transport *transport, struct drv_pci_device *device);
int drv_venus_transport_stop(struct venus_transport *transport);
int drv_venus_transport_command(struct venus_transport *transport, const void *command, uint32_t command_bytes, void *response, uint32_t capacity, uint32_t *response_bytes);
void drv_venus_header(void *buffer, uint32_t command, uint32_t context);
uint16_t drv_venus_load16(const volatile void *buffer);
uint32_t drv_venus_load32(const volatile void *buffer);
uint64_t drv_venus_load64(const volatile void *buffer);
void drv_venus_store16(void *buffer, uint16_t word);
void drv_venus_store32(void *buffer, uint32_t word);
void drv_venus_store64(void *buffer, uint64_t word);

#endif
