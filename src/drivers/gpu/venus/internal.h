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

#define VENUS_PAGE_BYTES 4096U
#define VENUS_RESOURCE_STORAGE 1U
#define VENUS_RESOURCE_BLOB 2U

#define VENUS_COMMAND_BYTES 65568U
#define VENUS_RESPONSE_BYTES 4096U
#define VENUS_QUEUE_SIZE 8U
#define VENUS_HEADER_BYTES 24U
#define VENUS_MAX_RESOURCE_BYTES (256U * 1024U * 1024U)
#define VENUS_MAX_APERTURE_BYTES (256U * 1024U * 1024U)

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

struct venus_controller;
struct venus_display_engine;
struct drv_gpu_display_ops;
struct gpu_present;

/* One context owned by a GPU open until its close callback completes. */
struct venus_session {
	uint32_t context;
};

/*
 * One session resource, retained on the controller list through uncertain DMA.
 *
 * Numeric context identity remains valid after its session wrapper retires.
 * Only acknowledged cleanup or controller reset permits local memory release.
 */
struct venus_resource {
	struct venus_resource *next;
	struct venus_controller *controller;
	struct drv_dma_buffer backing;
	struct drv_pci_mapping mapping;
	uint64_t bytes;
	uint64_t aperture_offset;
	uint64_t aperture_bytes;
	uint32_t context;
	uint32_t identifier;
	uint32_t kind;
	uint32_t width;
	uint32_t height;
	uint32_t format;
	unsigned created;
	unsigned attached;
	unsigned mapped;
};

/*
 * One PCI-owned backend, with GPU callbacks serialized by its mutex.
 *
 * GPU publication borrows this object. Resources include quarantined failures;
 * detach may free them only after GPU withdrawal and checked transport reset.
 */
struct venus_controller {
	struct venus_transport transport;
	struct mutex mutex;
	struct drv_gpu_device *gpu;
	struct venus_resource *resources;
	struct venus_resource *scanout;

	/* The last acknowledged primary scanout is independent from native or legacy lease ownership. */
	struct venus_resource *primary_scanout;
	uint32_t primary_width;
	uint32_t primary_height;
	struct venus_session *display_owner;
	struct venus_display_engine *display;
	uint32_t next_context;
	uint32_t next_resource;
};

extern const struct drv_gpu_display_ops drv_venus_display_operations;

int drv_venus_storage_create_locked(struct venus_controller *controller, struct venus_session *session, uint64_t bytes, struct venus_resource **result);
int drv_venus_storage_prepare_locked(struct venus_controller *controller, struct venus_resource *resource, const struct gpu_present *request);
int drv_venus_resource_release_locked(struct venus_controller *controller, struct venus_resource *resource);
void drv_venus_display_close_locked(struct venus_controller *controller, struct venus_session *session);
void drv_venus_display_finish(struct venus_controller *controller);
int drv_venus_display_stop(struct venus_controller *controller);
int drv_venus_display_legacy_available_locked(struct venus_controller *controller);
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
