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

#include <drivers/generic/dma.h>
#include <drivers/pci/pci.h>
#include <uapi/gpu.h>
#include <kern/lock.h>
#include <kern/waitq.h>
#include <stdint.h>

#define VENUS_PAGE_BYTES		4096U
#define VENUS_RESOURCE_STORAGE		1U
#define VENUS_RESOURCE_BLOB		2U

#define VENUS_COMMAND_BYTES		65568U
#define VENUS_RESPONSE_BYTES		4096U
#define VENUS_QUEUE_SIZE		64U
#define VENUS_REQUEST_SLOTS		32U
#define VENUS_SLOT_FREE			0U
#define VENUS_SLOT_POSTED		1U
#define VENUS_SLOT_COMPLETE		2U
#define VENUS_SLOT_QUARANTINED		3U
#define VENUS_SLOT_RESERVED		4U
#define VENUS_HEADER_BYTES		24U
#define VENUS_FEATURE_EDID		2U
#define VENUS_MAX_RESOURCE_BYTES	(256U * 1024U * 1024U)
#define VENUS_MAX_APERTURE_BYTES	(256U * 1024U * 1024U)

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
 * One DMA chain owned until matching used completion or acknowledged reset.
 * A completion pointer remains retained through its callback, so session drain
 * cannot free embedded common state while an IRQ is publishing its result.
 */
struct venus_request {
	struct drv_dma_buffer request;
	struct drv_dma_buffer response;
	struct drv_gpu_completion *completion;
	uint64_t started_ms;
	uint64_t fence;
	uint32_t context;
	uint32_t flags;
	uint32_t bytes;
	unsigned state;
	unsigned supervised;
	unsigned notifying;
	int error;
};

/*
 * One split queue, protected by a short IRQ-safe request lock.
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
	struct venus_window isr;
	struct venus_window configuration;
	struct venus_window host_visible;
	struct drv_pci_mapping registers[6];
	struct drv_pci_mapping host_mapping;
	struct drv_dma_buffer ring;
	struct venus_request requests[VENUS_REQUEST_SLOTS];
	struct spinlock queue_lock;
	struct wait_queue queue_waitq;
	struct mutex worker_lock;
	struct thread *worker;
	struct drv_gpu_device *gpu;
	struct drv_pci_irq interrupt;
	void *interrupt_cookie;
	unsigned interrupt_count;
	unsigned stopping;
	unsigned initialized;
	uint64_t next_fence;
	uint64_t interrupt_total;

	/* Queue-lock protected topology state remains valid without any registered GPU handle. */
	uint64_t topology_sequence;
	unsigned topology_overflow;
	uint64_t submitted_total;
	uint64_t completed_total;
	uint64_t sleep_total;
	uint32_t notify_multiplier;
	uint32_t capset_size;

	/* Negotiated descriptor capacity and strict native-fence proof remain immutable until reset. */
	unsigned slot_count;
	uint16_t queue_size;
	unsigned strict_queue;
	unsigned quiesce;
	uint32_t features;
	uint16_t available;
	uint16_t used;
	uint16_t notify_offset;

	/* A late chain return from an isolated context freed a slot outside callback publication. */
	unsigned reclaimed;
	unsigned saved;
	unsigned enabled;
	volatile unsigned failed;
	unsigned claimed[6];
};

struct venus_controller;
struct venus_display_engine;
struct drv_gpu_display_ops;
struct gpu_present;
struct venus_resource;
struct venus_shared_context;

/* One context owned by a GPU open until its close callback completes. */
struct venus_session {
	uint32_t context;

	/* Records that the core requested a stop; it validates stop polls and is not an admission barrier. */
	volatile unsigned stopping;
	struct venus_request *stop_request;
	unsigned quiesced;

	/* Set by isolation; destroy and close then retain host state for checked reset without traffic. */
	unsigned quarantined;
};

/*
 * One host allocation retained independently from all renderer contexts.
 * The controller mutex protects references and context memberships. Each
 * session alias, exported capability, or active scanout contributes one hold.
 */
struct venus_share {
	struct venus_resource *storage;
	struct venus_shared_context *contexts;
	struct gpu_image_descriptor image;
	unsigned references;
};

/*
 * One allocation anchor or session alias in the Venus resource namespace.
 *
 * Controller-linked anchors retain DMA and mapping ownership through uncertain
 * commands. Shared aliases borrow those views and own context memberships.
 * Only acknowledged cleanup or controller reset permits anchor memory release.
 */
struct venus_resource {
	struct venus_resource *next;
	struct venus_controller *controller;
	struct venus_share *share;
	struct drv_dma_buffer backing;
	struct drv_pci_mapping mapping;
	uint64_t bytes;
	uint64_t aperture_offset;
	uint64_t aperture_bytes;
	uint32_t context;
	uint32_t identifier;
	uint32_t kind;
	uint32_t blob_flags;
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
	unsigned recovering;
};

extern const struct drv_gpu_display_ops drv_venus_display_operations;
extern const struct drv_gpu_scanout_ops drv_venus_scanout_operations;

extern const struct drv_gpu_share_ops drv_venus_share_operations;
int drv_venus_resource_request_locked(struct venus_controller *controller, uint32_t command, uint32_t context, uint32_t identifier);
int drv_venus_share_hold_locked(struct venus_share *share);
void drv_venus_share_put_locked(struct venus_controller *controller, struct venus_share *share);
void drv_venus_share_resource_destroy_locked(struct venus_controller *controller, struct venus_session *session, struct venus_resource *resource);

int drv_venus_storage_create_locked(struct venus_controller *controller, struct venus_session *session, uint64_t bytes, struct venus_resource **result);
int drv_venus_storage_prepare_locked(struct venus_controller *controller, struct venus_resource *resource, const struct gpu_present *request);
int drv_venus_resource_release_locked(struct venus_controller *controller, struct venus_resource *resource);
void drv_venus_display_close_locked(struct venus_controller *controller, struct venus_session *session);
void drv_venus_display_finish(struct venus_controller *controller);
int drv_venus_display_stop(struct venus_controller *controller);
void drv_venus_display_console_changed_locked(struct venus_controller *controller);
int drv_venus_display_legacy_available_locked(struct venus_controller *controller);
int drv_venus_transport_start(struct venus_transport *transport, struct drv_pci_device *device);
int drv_venus_transport_stop(struct venus_transport *transport);
int drv_venus_transport_command(struct venus_transport *transport, const void *command, uint32_t command_bytes, void *response, uint32_t capacity, uint32_t *response_bytes);
int drv_venus_transport_submit(struct venus_transport *transport, uint32_t context, const void *command, uint32_t bytes, uint32_t flags, uint32_t timeline, struct drv_gpu_completion *completion);
void drv_venus_transport_drain(struct venus_transport *transport, uint32_t context);
void drv_venus_transport_set_gpu(struct venus_transport *transport, struct drv_gpu_device *gpu);
int drv_venus_transport_job_reserve(struct venus_transport *transport, uint32_t context, uint32_t timeline, struct drv_gpu_completion *completion, void **reservation);
int drv_venus_transport_job_commit(struct venus_transport *transport, void *reservation, struct drv_gpu_completion *completion);
int drv_venus_transport_job_cancel(struct venus_transport *transport, void *reservation, struct drv_gpu_completion *completion, unsigned fault);
int drv_venus_transport_capacity(struct venus_transport *transport, uint32_t timeline, unsigned *available);
int drv_venus_transport_idle(struct venus_transport *transport, uint32_t context);
int drv_venus_transport_quiesce(struct venus_transport *transport, uint32_t context, struct venus_request **request, unsigned *quiesced);
void drv_venus_transport_fail(struct venus_transport *transport, int error);
int drv_venus_transport_isolate(struct venus_transport *transport, uint32_t context, int error);
void drv_venus_display_forget_locked(struct venus_controller *controller, struct venus_session *session);
void drv_venus_transport_display_changed(struct venus_transport *transport);
void drv_venus_header(void *buffer, uint32_t command, uint32_t context);
uint16_t drv_venus_load16(const volatile void *buffer);
uint32_t drv_venus_load32(const volatile void *buffer);
uint64_t drv_venus_load64(const volatile void *buffer);
void drv_venus_store16(void *buffer, uint16_t word);
void drv_venus_store32(void *buffer, uint32_t word);
void drv_venus_store64(void *buffer, uint64_t word);

#endif
