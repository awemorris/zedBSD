/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Modern PCI and split control-queue transport private to the Venus driver.
 */

#include "internal.h"
#include <kern/kcrt.h>

#include <drivers/gpu/gpu.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/poll.h>
#include <kern/sched.h>
#include <kern/thread.h>

#include <uapi/errno.h>
#include <limits.h>

#define VENUS_RING_AVAILABLE		1024U
#define VENUS_RING_USED			1280U
#ifndef CONFIG_GPU_CONTROL_MS
#define CONFIG_GPU_CONTROL_MS		10000U
#endif
#define VENUS_WAIT_MILLISECONDS		CONFIG_GPU_CONTROL_MS
#define VENUS_WAIT_POLLS		50000000U
#define VENUS_REQUIRED_FEATURES		0x19U
#define VENUS_VENDOR_CAPSET_BYTES	168U
#define VENUS_VENDOR_CAPSET_MAGIC	0x5a424453U
#define VENUS_VENDOR_STRICT_FLAGS	3U
#define VENUS_VENDOR_QUIESCE_FLAGS	7U

static int venus_capabilities(struct venus_transport *transport);
static int venus_capability(struct venus_transport *transport, unsigned offset, unsigned length, unsigned type);
static int venus_map_window(struct venus_transport *transport, struct venus_window *window, size_t minimum);
static int venus_map_aperture(struct venus_transport *transport);
static int venus_reset(struct venus_transport *transport);
static int venus_negotiate(struct venus_transport *transport);
static int venus_queue_start(struct venus_transport *transport);
static int venus_capset_find(struct venus_transport *transport);
static int venus_response_error(uint32_t type);
static int venus_strict_queue_find(struct venus_transport *transport);
static void venus_request_publish_locked(struct venus_transport *transport, struct venus_request *request, unsigned index, uint32_t bytes);
static int venus_interrupt_start(struct venus_transport *transport);
static int venus_interrupt_stop(struct venus_transport *transport);
static int venus_interrupt(void *argument);
static int venus_worker_start(struct venus_transport *transport);
static int venus_worker_stop(struct venus_transport *transport);
static void venus_worker(void *argument);
static int venus_request_post(struct venus_transport *transport, const void *command, uint32_t bytes, unsigned queue_marker, struct drv_gpu_completion *completion, struct venus_request **result);
static int venus_request_wait(struct venus_transport *transport, struct venus_request *request);
static int venus_queue_wait_locked(struct venus_transport *transport, uint64_t deadline, unsigned flags, unsigned long *irq);
static unsigned venus_queue_collect(struct venus_transport *transport);
static void venus_queue_notify(struct venus_transport *transport);
static void venus_capacity_changed(struct venus_transport *transport);
static int venus_request_validate(struct venus_request *request, uint32_t bytes);
static int venus_wait_expired(uint64_t started);


/*
 * Publishes an inventory invalidation without borrowing a common GPU registration.
 *
 * Both IRQ delivery and a query that clears a pending configuration event call
 * this helper. Duplicate invalidations are harmless; no hardware event may be
 * cleared before its software sequence is visible to every independently open fd.
 */
void
drv_venus_transport_display_changed(
	struct venus_transport *transport)
{
	unsigned long irq;

	/* Saturation is a persistent error rather than reuse of an acknowledged sequence. */
	irq = spin_lock_irqsave(&transport->queue_lock);

	if (transport->topology_sequence == UINT64_MAX)
		transport->topology_overflow = 1U;
	else
		transport->topology_sequence++;

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* Pollers may re-enter the backend only after its event lock is released. */
	poll_notify();

	/* Succeeded: transport ownership, independent from sessions, bounds IRQ access. */
	return;
}

/*
 * Retains a published GPU wrapper independently from sessions and callbacks.
 */
void
drv_venus_transport_set_gpu(
	struct venus_transport *transport,
	struct drv_gpu_device *gpu)
{
	struct drv_gpu_device *previous;
	struct drv_gpu_device *failed_gpu;
	unsigned failed;
	unsigned long irq;

	/* The caller protects its supplied handle while this persistent reference is acquired. */
	if (gpu != NULL)
		drv_gpu_retain(gpu);

	/* Failure reporting snapshots a separate reference before publication can be withdrawn. */
	failed_gpu = NULL;
	irq = spin_lock_irqsave(&transport->queue_lock);

	/* The persistent reference changes atomically with respect to fault snapshots. */
	previous = transport->gpu;
	transport->gpu = gpu;

	/* A prepublication failure needs a separate reference for its deferred notification. */
	failed = atomic_raw_load_acquire(&transport->failed);
	if (failed != 0U && gpu != NULL) {
		drv_gpu_retain(gpu);
		failed_gpu = gpu;
	}

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* Wrapper retirement may release storage and therefore occurs outside the request lock. */
	if (previous != NULL)
		drv_gpu_release(previous);

	/* An IRQ failure preceding publication must remain visible to newly opened descriptors. */
	if (failed_gpu != NULL) {
		drv_gpu_report_error(failed_gpu, EIO);
		drv_gpu_release(failed_gpu);
	}

	/* Succeeded: every future fault snapshot can retain a live published wrapper. */
	return;
}

/*
 * Reserves a strict native-fence marker before the producer submits GPU work.
 */
int
drv_venus_transport_job_reserve(
	struct venus_transport *transport,
	uint32_t context,
	uint32_t timeline,
	struct drv_gpu_completion *completion,
	void **reservation)
{
	struct venus_request *request;
	uint8_t *packet;
	unsigned control;
	unsigned limit;
	unsigned index;
	unsigned failed;
	unsigned long irq;

	/* A strict marker needs one stable callback and a nonzero registered queue domain. */
	if (completion == NULL || reservation == NULL)
		return EINVAL;

	/* Refused admission must never leave a usable token in caller storage. */
	*reservation = NULL;

	/* CPU0 decoder progress can never authorize a successful GPU job. */
	if (context == 0U ||
	    timeline == 0U ||
	    timeline >= 64U)
		return EINVAL;

	/* Stock and earlier paired hosts do not promise successful native-fence completion. */
	if (transport->strict_queue == 0U)
		return ENOTSUP;

	/* Keep independent capacity for the decoder command that can unblock pending GPU work. */
	control = transport->slot_count / 8U;
	if (control == 0U)
		control = 1U;

	/* Only the remaining prefix can be consumed by GPU work awaiting future decoder progress. */
	limit = transport->slot_count - control;

	/* Reserve both the descriptor pair and its callback before exposing the producer token. */
	irq = spin_lock_irqsave(&transport->queue_lock);

	failed = atomic_raw_load_acquire(&transport->failed);
	if (failed != 0U ||
	    transport->enabled == 0U ||
	    transport->stopping != 0U) {
		spin_unlock_irqrestore(&transport->queue_lock, irq);
		return ENODEV;
	}

	/* Saturation is reported before native submission and never waits on future producer work. */
	request = NULL;
	for (index = 0U; index < limit; index++) {
		/* A free pair has no descriptor, callback or reservation owner. */
		if (transport->requests[index].state == VENUS_SLOT_FREE) {
			request = &transport->requests[index];
			break;
		}
	}

	/* The caller can retain no token when all admitted marker storage is occupied. */
	if (request == NULL) {
		spin_unlock_irqrestore(&transport->queue_lock, irq);
		return EAGAIN;
	}

	/* Host watermark identities never wrap into an earlier request's lifetime. */
	if (transport->next_fence == 0U) {
		spin_unlock_irqrestore(&transport->queue_lock, irq);
		return EOVERFLOW;
	}

	/* Prepare the complete empty protocol marker without publishing a descriptor yet. */
	packet = request->request.address;
	kern_memset(packet, 0, 32U);
	drv_venus_header(packet, 0x0207U, context);

	/* FENCE and INFO_RING_IDX require an actual queue-domain acknowledgement. */
	drv_venus_store32(packet + 4U, 3U);

	/* The host watermark identity is unique across every slot and context on this transport. */
	request->fence = transport->next_fence++;
	drv_venus_store64(packet + 8U, request->fence);

	/* The nonzero timeline selects the actual native submission fence retained by the host. */
	packet[20U] = (uint8_t)timeline;

	/* A reused response cannot satisfy validation before this marker completes. */
	kern_memset(request->response.address, 0, VENUS_RESPONSE_BYTES);

	/* Common job policy owns the producer and execution deadlines for this retained slot. */
	request->context = context;
	request->flags = 3U;
	request->completion = completion;
	request->notifying = 0U;
	request->bytes = 0U;
	request->error = 0;
	request->started_ms = 0U;
	request->supervised = 1U;
	request->state = VENUS_SLOT_RESERVED;

	/* The common owner receives the exact reserved storage before publishing native work. */
	*reservation = request;
	waitq_wake_all(&transport->queue_waitq);

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* Succeeded: the kernel supervises this exact reservation before GPU work may begin. */
	return 0;
}

/*
 * Publishes a retained marker after the producer receives native submission success.
 */
int
drv_venus_transport_job_commit(
	struct venus_transport *transport,
	void *reservation,
	struct drv_gpu_completion *completion)
{
	struct venus_request *request;
	unsigned index;
	unsigned failed;
	unsigned long irq;

	/* Both identities belong to the same common job and must remain present. */
	if (reservation == NULL || completion == NULL)
		return EINVAL;

	/* The slot and callback together reject stale tokens without dereferencing foreign storage. */
	irq = spin_lock_irqsave(&transport->queue_lock);

	/* Foreign addresses are compared only, never dereferenced as backend tokens. */
	request = NULL;
	for (index = 0U; index < transport->slot_count; index++) {
		/* The matching embedded slot must still retain the expected completion below. */
		if (reservation == &transport->requests[index]) {
			request = &transport->requests[index];
			break;
		}
	}

	/* Only a still-owned unpublished reservation may enter the device queue. */
	if (request == NULL ||
	    request->completion != completion ||
	    request->state != VENUS_SLOT_RESERVED) {
		spin_unlock_irqrestore(&transport->queue_lock, irq);
		return ESTALE;
	}

	/* No successful commit may follow teardown or an independently reported device fault. */
	failed = atomic_raw_load_acquire(&transport->failed);
	if (failed != 0U ||
	    transport->enabled == 0U ||
	    transport->stopping != 0U) {
		spin_unlock_irqrestore(&transport->queue_lock, irq);
		return ENODEV;
	}

	/* Everything fallible was reserved before native work, so publication cannot need new capacity. */
	request->state = VENUS_SLOT_POSTED;
	venus_request_publish_locked(transport, request, index, 32U);

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* Succeeded: the host now waits on the actual submission's native fence. */
	return 0;
}

/*
 * Cancels definite nonacceptance or reports uncertainty without losing GPU ownership.
 */
int
drv_venus_transport_job_cancel(
	struct venus_transport *transport,
	void *reservation,
	struct drv_gpu_completion *completion,
	unsigned fault)
{
	struct venus_request *request;
	unsigned index;
	unsigned long irq;

	/* Only an exact retained reservation can release its callback obligation. */
	if (reservation == NULL ||
	    completion == NULL ||
	    fault > 1U)
		return EINVAL;

	/* Resolve the token by equality while excluding timeout and commit transitions. */
	irq = spin_lock_irqsave(&transport->queue_lock);

	/* Foreign addresses are compared only, never dereferenced as backend tokens. */
	request = NULL;
	for (index = 0U; index < transport->slot_count; index++) {
		/* The matching embedded slot must still retain the expected completion below. */
		if (reservation == &transport->requests[index]) {
			request = &transport->requests[index];
			break;
		}
	}

	/* A completed or recycled token cannot cancel another job's lifetime. */
	if (request == NULL || request->completion != completion) {
		spin_unlock_irqrestore(&transport->queue_lock, irq);
		return ESTALE;
	}

	/* Definite rollback requires an unpublished slot; uncertainty may also end posted work. */
	if (request->state != VENUS_SLOT_RESERVED &&
	    (fault == 0U || request->state != VENUS_SLOT_POSTED)) {
		spin_unlock_irqrestore(&transport->queue_lock, irq);
		return ESTALE;
	}

	/* Common session policy must supervise uncertainty without losing this callback or backing. */
	if (fault != 0U) {
		spin_unlock_irqrestore(&transport->queue_lock, irq);
		return 0;
	}

	/* Definite nonacceptance returns unpublished storage with no future callback. */
	request->completion = NULL;
	request->state = VENUS_SLOT_FREE;
	request->context = 0U;
	waitq_wake_all(&transport->queue_waitq);

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* Common capacity waiters observe only storage whose ownership actually ended. */
	venus_capacity_changed(transport);

	/* Succeeded: rollback removed this reservation without inventing a successful fence. */
	return 0;
}

/*
 * Returns immediately with the backend capacity for one supported queue domain.
 */
int
drv_venus_transport_capacity(
	struct venus_transport *transport,
	uint32_t timeline,
	unsigned *available)
{
	unsigned control;
	unsigned limit;
	unsigned index;
	unsigned failed;
	unsigned long irq;

	/* A snapshot cannot expose capacity for a decoder or unsupported host profile. */
	if (available == NULL || timeline == 0U || timeline >= 64U)
		return EINVAL;

	/* Refused snapshots carry no usable capacity. */
	*available = 0U;
	if (transport->strict_queue == 0U)
		return ENOTSUP;

	/* The same reserved control fraction constrains actual admission and observation. */
	control = transport->slot_count / 8U;
	if (control == 0U)
		control = 1U;
	limit = transport->slot_count - control;

	/* State inspection never waits for a descriptor or submits a renderer command. */
	irq = spin_lock_irqsave(&transport->queue_lock);

	failed = atomic_raw_load_acquire(&transport->failed);
	if (failed != 0U || transport->enabled == 0U || transport->stopping != 0U) {
		spin_unlock_irqrestore(&transport->queue_lock, irq);
		return ENODEV;
	}

	/* A completed callback still owns its chain until its final publication returns. */
	for (index = 0U; index < limit; index++) {
		if (transport->requests[index].state == VENUS_SLOT_FREE)
			(*available)++;
	}

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* Succeeded: common policy may combine this snapshot with its per-open ledger. */
	return 0;
}

/*
 * Confirms actual context retirement after common policy has closed admission.
 */
int
drv_venus_transport_idle(
	struct venus_transport *transport,
	uint32_t context)
{
	struct venus_request *request;
	unsigned failed;
	unsigned index;
	unsigned long irq;

	/* Context zero is global control and cannot be isolated as one renderer session. */
	if (context == 0U)
		return EINVAL;

	/* The caller excludes further session operations before asking for this snapshot. */
	irq = spin_lock_irqsave(&transport->queue_lock);

	failed = atomic_raw_load_acquire(&transport->failed);
	if (failed != 0U) {
		spin_unlock_irqrestore(&transport->queue_lock, irq);
		return ENODEV;
	}

	/* The host-wide quiescence proof precedes this snapshot of posted descriptors and callbacks. */
	for (index = 0U; index < transport->slot_count; index++) {
		request = &transport->requests[index];
		if (request->context != context)
			continue;

		/* An unpublished reservation holds no native work after quiescence; the core withdraws it through cancel. */
		if (request->state == VENUS_SLOT_RESERVED)
			continue;

		/* Both descriptor return and the callback's final access must have completed. */
		if (request->state != VENUS_SLOT_FREE || request->completion != NULL) {
			spin_unlock_irqrestore(&transport->queue_lock, irq);
			return EAGAIN;
		}
	}

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* Succeeded: every supervised native job and target-context DMA chain has retired. */
	return 0;
}

/*
 * Posts or observes a host-verified, context-wide native quiescence barrier.
 */
int
drv_venus_transport_quiesce(
	struct venus_transport *transport,
	uint32_t context,
	struct venus_request **pending,
	unsigned *quiesced)
{
	struct venus_request *request;
	uint8_t *packet;
	uint32_t response_type;
	unsigned index;
	unsigned failed;
	unsigned long irq;

	/* Old strict hosts cannot prove completion of untracked native submissions. */
	if (transport->quiesce == 0U)
		return ENOTSUP;

	/* Only retained per-session storage may own this asynchronous control operation. */
	if (context == 0U ||
	    pending == NULL ||
	    quiesced == NULL)
		return EINVAL;

	/* A previous verified ACK never requires another native idle operation. */
	if (*quiesced != 0U)
		return 0;

	/* A stopped session owns these fields and cannot publish concurrent raw work. */
	irq = spin_lock_irqsave(&transport->queue_lock);

	failed = atomic_raw_load_acquire(&transport->failed);
	if (failed != 0U ||
	    transport->enabled == 0U ||
	    transport->stopping != 0U) {
		spin_unlock_irqrestore(&transport->queue_lock, irq);
		return ENODEV;
	}

	/* An actual fenced response is the only authority to finish this stop operation. */
	request = *pending;
	if (request != NULL) {
		/* Until the device returns this owner, neither proof nor descriptor may be consumed. */
		if (request->state != VENUS_SLOT_COMPLETE) {
			spin_unlock_irqrestore(&transport->queue_lock, irq);
			return EAGAIN;
		}

		/* The matching native idle proof excludes response refusal and unrelated context IDs. */
		if (request->error != 0 || request->context != context) {
			spin_unlock_irqrestore(&transport->queue_lock, irq);
			return EIO;
		}

		/* A longer successful response belongs to another operation, not this stop proof. */
		if (request->bytes != VENUS_HEADER_BYTES) {
			spin_unlock_irqrestore(&transport->queue_lock, irq);
			return EIO;
		}

		/* Only OK_NODATA acknowledges completion of this exact fenced control request. */
		response_type = drv_venus_load32(request->response.address);
		if (response_type != 0x1100U) {
			spin_unlock_irqrestore(&transport->queue_lock, irq);
			return EIO;
		}

		/* Release the returned control descriptor only after recording its actual proof. */
		request->state = VENUS_SLOT_FREE;
		*pending = NULL;
		*quiesced = 1U;

		waitq_wake_all(&transport->queue_waitq);

		spin_unlock_irqrestore(&transport->queue_lock, irq);

		/* The returned control descriptor is capacity again; the core withdraws unpublished reservations itself. */
		venus_capacity_changed(transport);

		/* Succeeded: native idle proves even work without a published job marker has ended. */
		return 0;
	}

	/* Stop admission never waits while common policy is servicing independent sessions. */
	for (index = 0U; index < transport->slot_count; index++) {
		if (transport->requests[index].state == VENUS_SLOT_FREE)
			break;
	}

	/* Full control storage is retryable and cannot overwrite an unrelated session's owner. */
	if (index == transport->slot_count) {
		spin_unlock_irqrestore(&transport->queue_lock, irq);
		return EAGAIN;
	}

	/* Exhausted identities never wrap into a previously acknowledged control request. */
	if (transport->next_fence == 0U) {
		spin_unlock_irqrestore(&transport->queue_lock, irq);
		return EOVERFLOW;
	}

	/* Exact private framing cannot be mistaken for a standard Venus opcode stream. */
	request = &transport->requests[index];
	packet = request->request.address;
	kern_memset(packet, 0, 48U);
	drv_venus_header(packet, 0x0207U, context);
	drv_venus_store32(packet + 4U, 3U);
	request->fence = transport->next_fence++;
	drv_venus_store64(packet + 8U, request->fence);
	drv_venus_store32(packet + 24U, 16U);
	drv_venus_store32(packet + 32U, 0x5a425351U);
	drv_venus_store32(packet + 36U, 1U);
	kern_memset(request->response.address, 0, VENUS_RESPONSE_BYTES);
	request->context = context;
	request->flags = 3U;
	request->completion = NULL;
	request->notifying = 0U;
	request->bytes = 0U;
	request->error = 0;
	request->started_ms = 0U;
	request->supervised = 1U;
	request->state = VENUS_SLOT_POSTED;
	*pending = request;
	venus_request_publish_locked(transport, request, index, 48U);

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* Pending: common stop policy bounds this independently from ordinary control traffic. */
	return EAGAIN;
}

/*
 * Starts one modern PCI transport while retaining every partial acquisition.
 */
int
drv_venus_transport_start(
	struct venus_transport *transport,
	struct drv_pci_device *device)
{
	int error;

	/* Initializes ownership channels before an interrupt or partial teardown can run. */
	spin_init(&transport->queue_lock, LOCK_RANK_DEVICE, "Venus control queue");
	waitq_init(&transport->queue_waitq, "Venus control queue");
	error = mutex_init(&transport->worker_lock, LOCK_RANK_DEVICE, "Venus watchdog start");
	if (error != 0)
		return error;

	/* The initialized flag permits partial teardown to use both wait channels. */
	transport->initialized = 1U;
	transport->next_fence = 1U;
	transport->topology_sequence = 1U;
	transport->topology_overflow = 0U;

	/* Records the parent before any failed step can require rollback. */
	transport->pci = device;
	transport->stage = "dma-provider";
	transport->dma = drv_pci_device_dma(device);
	if (transport->dma == NULL)
		return ENODEV;

	/* Saves the original command bits for final hardware-owner cleanup. */
	transport->stage = "save-pci-state";
	error = drv_pci_device_save_enable_state(device, &transport->enable_state);
	if (error != 0)
		return error;

	/* Saved ownership survives every later attach or detach retry. */
	transport->saved = 1;

	/* Enables register decoding without publishing a userspace GPU. */
	transport->stage = "enable-pci-memory";
	error = drv_pci_device_enable_memory(device);
	if (error != 0)
		return error;

	/* Finds and bounds-checks each vendor capability before mapping it. */
	transport->stage = "pci-capabilities";
	error = venus_capabilities(transport);
	if (error != 0)
		return error;

	/* Maps the complete register BAR and borrows its common-configuration slice. */
	transport->stage = "map-common";
	error = venus_map_window(transport, &transport->common, 56U);
	if (error != 0)
		return error;

	/* Queue notifications need the complete advertised doorbell window. */
	transport->stage = "map-notify";
	error = venus_map_window(transport, &transport->notify, 2U);
	if (error != 0)
		return error;

	/* Reads scanout and capset counts from the bounded device configuration. */
	transport->stage = "map-configuration";
	error = venus_map_window(transport, &transport->configuration, 16U);
	if (error != 0)
		return error;

	/* INTx acknowledges only this function's read-to-clear Virtio interrupt status. */
	transport->stage = "map-isr";
	error = venus_map_window(transport, &transport->isr, 1U);
	if (error != 0)
		return error;

	/* Removes any firmware queue ownership before writing new addresses. */
	transport->stage = "reset";
	error = venus_reset(transport);
	if (error != 0)
		return error;

	/* Maps the complete bounded aperture before any context may create host blobs. */
	transport->stage = "map-host-visible";
	error = venus_map_aperture(transport);
	if (error != 0)
		return error;

	/* Negotiates only modern split queues and Venus-required GPU features. */
	transport->stage = "negotiate-features";
	error = venus_negotiate(transport);
	if (error != 0)
		return error;

	/* Allocates the persistent control queue and its two DMA directions. */
	transport->stage = "allocate-control-queue";
	error = venus_queue_start(transport);
	if (error != 0)
		return error;

	/* Establishes a checked PCI interrupt source before enabling device work. */
	transport->stage = "control-interrupt";
	error = venus_interrupt_start(transport);
	if (error != 0)
		return error;

	/* Permits device accesses only after all queue addresses are initialized. */
	transport->stage = "enable-bus-master";
	error = drv_pci_device_set_bus_master(device, true);
	if (error != 0)
		return error;

	/* DRIVER_OK transfers queue ownership to the virtual device. */
	transport->enabled = 1;
	kern_mmio_write8((uint8_t *)transport->common.mapping.address + 20U, 15U);

	/* Requires the advertised Vulkan capset instead of a VirGL-only device. */
	transport->stage = "find-venus-capset";
	error = venus_capset_find(transport);
	if (error != 0)
		return error;

	/* Succeeded: the initialized control queue can serve Venus contexts. */
	return 0;
}

/*
 * Stops DMA before releasing transport memory or restoring PCI command bits.
 */
int
drv_venus_transport_stop(
	struct venus_transport *transport)
{
	unsigned index;
	int error;

	/* No watchdog may inspect queue storage after hardware and mappings retire. */
	error = venus_worker_stop(transport);
	if (error != 0)
		return error;

	/* A mapped common window permits a checked virtual-device reset. */
	if (transport->common.mapping.address != NULL) {
		/* Reset completion ends all access to the old queue and backing. */
		error = venus_reset(transport);
		if (error != 0)
			return error;
	}

	/* Checked IRQ removal retains mappings while any dispatch is still in flight. */
	error = venus_interrupt_stop(transport);
	if (error != 0)
		return error;

	/* Removes bus-master permission before any DMA allocation can retire. */
	if (transport->saved != 0) {
		error = drv_pci_device_set_bus_master(transport->pci, false);
		if (error != 0)
			return error;
	}

	/* Releases each independently retained DMA direction only after reset and IRQ drain. */
	for (index = 0U; index < VENUS_REQUEST_SLOTS; index++) {
		/* Completed reset is the ownership barrier for every writable response. */
		if (transport->requests[index].response.address != NULL) {
			drv_dma_free_coherent(transport->dma, &transport->requests[index].response);
			if (transport->requests[index].response.address != NULL)
				return EBUSY;
		}

		/* Request bytes remain pinned even when their original caller timed out. */
		if (transport->requests[index].request.address != NULL) {
			drv_dma_free_coherent(transport->dma, &transport->requests[index].request);
			if (transport->requests[index].request.address != NULL)
				return EBUSY;
		}
	}

	/* The descriptor ring is last because it names the other allocations. */
	if (transport->ring.address != NULL) {
		drv_dma_free_coherent(transport->dma, &transport->ring);
		if (transport->ring.address != NULL)
			return EBUSY;
	}

	/* Releases the sole aperture mapping after reset ends all blob accesses. */
	if (transport->host_mapping.address != NULL) {
		drv_pci_device_unmap_bar(transport->pci, &transport->host_mapping);
	}

	/* Invalidates the capability view borrowed by every now-inactive blob. */
	kern_memset(&transport->host_visible.mapping, 0, sizeof(transport->host_visible.mapping));

	/* Releases each complete register BAR only once after all queue work ends. */
	for (index = 0; index < 6U; index++) {
		/* Capability windows borrow slices of these independently owned mappings. */
		if (transport->registers[index].address != NULL) {
			drv_pci_device_unmap_bar(transport->pci, &transport->registers[index]);
		}
	}

	/* Invalidates borrowed capability views after their containing BARs retire. */
	kern_memset(&transport->configuration.mapping, 0, sizeof(transport->configuration.mapping));
	kern_memset(&transport->notify.mapping, 0, sizeof(transport->notify.mapping));
	kern_memset(&transport->isr.mapping, 0, sizeof(transport->isr.mapping));
	kern_memset(&transport->common.mapping, 0, sizeof(transport->common.mapping));

	/* Restores the saved command state only after all allocations retire. */
	if (transport->saved != 0) {
		error = drv_pci_device_restore_enable_state(
			transport->pci,
			&transport->enable_state);
		if (error != 0)
			return error;

		/* The saved token is no longer owned by a teardown retry. */
		transport->saved = 0;
	}

	/* Gives every claimed register or host-memory BAR back to PCI. */
	for (index = 0; index < 6U; index++) {
		/* Only successfully claimed BARs belong to this transport. */
		if (transport->claimed[index] != 0) {
			drv_pci_device_release_bar(transport->pci, index);
			transport->claimed[index] = 0;
		}
	}

	/* Reset and IRQ drain make the final persistent GPU-wrapper reference unnecessary. */
	if (transport->initialized != 0U)
		drv_venus_transport_set_gpu(transport, NULL);

	/* Succeeded: no transport allocation or PCI lease remains owned. */
	return 0;
}

/*
 * Exchanges one bounded control request using its independent DMA chain.
 */
int
drv_venus_transport_command(
	struct venus_transport *transport,
	const void *command,
	uint32_t command_bytes,
	void *response,
	uint32_t capacity,
	uint32_t *response_bytes)
{
	struct venus_request *request;
	unsigned long irq;
	int error;

	/* Requires complete bounded command and reply storage from the caller. */
	if (command == NULL || response_bytes == NULL)
		return EINVAL;

	/* Every GPU control request contains a complete common header. */
	if (command_bytes < VENUS_HEADER_BYTES || command_bytes > VENUS_COMMAND_BYTES)
		return EINVAL;

	/* Reply storage must be representable before the queue is touched. */
	if (capacity > VENUS_RESPONSE_BYTES ||
	    (capacity != 0U && response == NULL))
		return EINVAL;

	/* Posts a stable DMA copy without retaining transient caller storage. */
	*response_bytes = 0U;
	error = venus_request_post(transport, command, command_bytes, 0U, NULL, &request);
	if (error != 0)
		return error;

	/* Runtime completion sleeps on IRQ state; only early idle-thread discovery polls. */
	error = venus_request_wait(transport, request);
	if (error != 0)
		return error;

	/* Copies only the validated completed response before releasing its slot. */
	irq = spin_lock_irqsave(&transport->queue_lock);

	error = request->error;
	*response_bytes = request->bytes;
	if (error == 0 && request->bytes > capacity)
		error = EMSGSIZE;

	/* Device completion makes these exact bytes immutable for the original caller. */
	if (error == 0 && request->bytes != 0U)
		kern_memcpy(response, request->response.address, request->bytes);

	/* Checked used completion permits reuse; uncertain DMA stays quarantined. */
	if (request->state == VENUS_SLOT_COMPLETE)
		request->state = VENUS_SLOT_FREE;

	/* Slot availability is independent of a particular session's completion ledger. */
	waitq_wake_all(&transport->queue_waitq);

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* The synchronous owner has released the actual descriptor pair before waking policy. */
	venus_capacity_changed(transport);

	/* Reports a protocol refusal or response truncation without inventing GPU success. */
	if (error != 0)
		return error;

	/* Succeeded: QEMU acknowledged this control operation, not arbitrary Vulkan work. */
	return 0;
}

/*
 * Submits an independent Venus stream with an optional context-timeline marker.
 */
int
drv_venus_transport_submit(
	struct venus_transport *transport,
	uint32_t context,
	const void *command,
	uint32_t bytes,
	uint32_t flags,
	uint32_t timeline,
	struct drv_gpu_completion *completion)
{
	struct venus_request *request;
	uint8_t *packet;
	unsigned queue_marker;
	int error;

	/* Only accepted asynchronous work may retain a common completion obligation. */
	if (completion == NULL || bytes > GPU_COMMAND_MAX)
		return EINVAL;

	/* The watchdog handles a missing IRQ even if userspace never waits explicitly. */
	error = venus_worker_start(transport);
	if (error != 0)
		return error;

	/* Builds the protocol wrapper before entering the short DMA publication lock. */
	packet = kern_calloc(1U, (size_t)bytes + 32U);
	if (packet == NULL)
		return ENOMEM;

	/* The independent open chooses the renderer namespace, never userspace header bytes. */
	drv_venus_header(packet, 0x0207U, context);
	drv_venus_store32(packet + 24U, bytes);
	if (bytes != 0U)
		kern_memcpy(packet + 32U, command, bytes);

	/* Context fencing requires both protocol flags; FENCE alone names the legacy GL path. */
	queue_marker = 0U;
	if ((flags & GPU_COMMAND_CONTEXT_FENCE) != 0U) {
		drv_venus_store32(packet + 4U, 3U);
		packet[20U] = (uint8_t)timeline;

		/* Queue markers cannot consume the descriptor reserved for future decoder work. */
		if (timeline != 0U)
			queue_marker = 1U;
	}

	/* A successful post owns its own copy before the wrapper is freed. */
	error = venus_request_post(
		transport,
		packet,
		bytes + 32U,
		queue_marker,
		completion,
		&request);
	kern_free(packet);
	if (error != 0)
		return error;

	/* Succeeded: the callback will publish the selected transport boundary exactly once. */
	return 0;
}

/*
 * Ends every accepted callback before an independent renderer session closes.
 */
void
drv_venus_transport_drain(
	struct venus_transport *transport,
	uint32_t context)
{
	struct venus_request *request;
	unsigned index;
	unsigned pending;
	unsigned long irq;

	/* Common supervision remains active through close and bounds uncertain managed jobs. */
	irq = spin_lock_irqsave(&transport->queue_lock);

	while (1) {
		/* Pending includes a callback currently publishing outside the transport lock. */
		pending = 0U;
		for (index = 0U; index < transport->slot_count; index++) {
			request = &transport->requests[index];
			if (request->context == context && request->completion != NULL)
				pending++;
		}

		/* No IRQ or worker retains this session's embedded completion records. */
		if (pending == 0U)
			break;

		/* The caller owns no controller lock while draining asynchronous requests. */
		(void)venus_queue_wait_locked(transport, 0U, 0U, &irq);
	}

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* Succeeded: final close can retire common and backend session storage. */
	return;
}

/*
 * Quarantines uncertain DMA and publishes terminal failure without early reuse.
 */
void
drv_venus_transport_fail(
	struct venus_transport *transport,
	int error)
{
	unsigned failed;
	struct venus_request *request;
	struct drv_gpu_device *gpu;
	unsigned index;
	unsigned first;
	unsigned long irq;

	/* An uninitialized attach has no queue or callback state to publish. */
	if (transport->initialized == 0U) {
		atomic_raw_store_release(&transport->failed, 1U);
		return;
	}

	/* Failure prevents new publication before waking any existing request owner. */
	first = 0U;
	irq = spin_lock_irqsave(&transport->queue_lock);

	/* The publication reference protects faults even when no command callback is pending. */
	gpu = transport->gpu;
	if (gpu != NULL)
		drv_gpu_retain(gpu);

	failed = atomic_raw_load_acquire(&transport->failed);
	if (failed == 0U)
		first = 1U;

	/* Every posted chain remains unavailable until checked reset ends device ownership. */
	atomic_raw_store_release(&transport->failed, 1U);
	for (index = 0U; index < transport->slot_count; index++) {
		request = &transport->requests[index];

		/* Reserved producers may already have accepted native work before their marker commit. */
		if (request->state != VENUS_SLOT_POSTED && request->state != VENUS_SLOT_RESERVED)
			continue;

		/* A terminal user result does not authorize releasing possibly active DMA. */
		request->state = VENUS_SLOT_QUARANTINED;
		request->error = error;
	}

	/* Every waiter rechecks failure or its exact stable request state. */
	waitq_wake_all(&transport->queue_waitq);

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* Explicit device failure is independent from callback count and publication withdrawal. */
	if (gpu != NULL) {
		drv_gpu_report_error(gpu, error);
		drv_gpu_release(gpu);
	}

	/* Callback pointers retain their sessions and GPU wrapper through the preceding fault report. */
	venus_queue_notify(transport);

	/* A single diagnostic identifies retained DMA without repeatedly logging observers. */
	if (first != 0U)
		kern_logf("venus: transport failed error=%d; DMA retained for checked reset\n", error);

	/* Succeeded: user observation is terminal while hardware ownership remains safe. */
	return;
}

/*
 * Quarantines one context's chains and ends their callbacks while the transport continues.
 */
int
drv_venus_transport_isolate(
	struct venus_transport *transport,
	uint32_t context,
	int error)
{
	struct venus_request *request;
	unsigned failed;
	unsigned index;
	unsigned long irq;

	/* Context zero is global control, and a quarantine needs a real terminal error. */
	if (context == 0U || error == 0)
		return EINVAL;

	/* A failed or stopped transport already quarantined everything; the core escalates instead. */
	irq = spin_lock_irqsave(&transport->queue_lock);

	failed = atomic_raw_load_acquire(&transport->failed);
	if (failed != 0U || transport->enabled == 0U) {
		spin_unlock_irqrestore(&transport->queue_lock, irq);
		return ENODEV;
	}

	/* Posted and reserved chains of this context keep their storage until the device returns them or reset. */
	for (index = 0U; index < transport->slot_count; index++) {
		request = &transport->requests[index];
		if (request->context != context)
			continue;

		/* Free and completed chains owe nothing; only live device ownership is quarantined. */
		if (request->state != VENUS_SLOT_POSTED && request->state != VENUS_SLOT_RESERVED)
			continue;

		/* The retained callback publishes the context's terminal error while the slot stays owned. */
		request->state = VENUS_SLOT_QUARANTINED;
		request->error = error;
	}

	waitq_wake_all(&transport->queue_waitq);

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* Callback publication runs outside the queue lock, exactly as after a device-wide fault. */
	venus_queue_notify(transport);

	/* Succeeded: this context owns no live callback while its DMA remains retained. */
	return 0;
}

/*
 * Initializes an unfenced control header for one context-scoped request.
 */
void
drv_venus_header(
	void *buffer,
	uint32_t command,
	uint32_t context)
{
	uint8_t *bytes;

	/* Clears flags, fence and ring fields instead of inventing GPU completion. */
	bytes = buffer;
	kern_memset(bytes, 0, VENUS_HEADER_BYTES);
	drv_venus_store32(bytes, command);
	drv_venus_store32(bytes + 16U, context);

	/* Succeeded: the remaining payload may be encoded by the backend. */
	return;
}

/*
 * Decodes a little-endian queue field without alignment assumptions.
 */
uint16_t
drv_venus_load16(
	const volatile void *buffer)
{
	const volatile uint8_t *bytes;

	/* Reads each wire byte through the device-visible volatile view. */
	bytes = buffer;

	/* Succeeded: returns the host representation of the wire field. */
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

/*
 * Decodes a little-endian control field without alignment assumptions.
 */
uint32_t
drv_venus_load32(
	const volatile void *buffer)
{
	const volatile uint8_t *bytes;

	/* Reads each wire byte through the device-visible volatile view. */
	bytes = buffer;

	/* Succeeded: returns the host representation of the wire field. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
		((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/*
 * Decodes one split 64-bit wire field.
 */
uint64_t
drv_venus_load64(
	const volatile void *buffer)
{
	const volatile uint8_t *bytes;
	uint32_t low;
	uint32_t high;

	/* Combines two independently decoded halves in wire order. */
	bytes = buffer;
	low = drv_venus_load32(bytes);
	high = drv_venus_load32(bytes + 4U);

	/* Succeeded: returns the complete wire value in host representation. */
	return (uint64_t)low | ((uint64_t)high << 32);
}

/*
 * Encodes one little-endian queue field.
 */
void
drv_venus_store16(
	void *buffer,
	uint16_t word)
{
	uint8_t *bytes;

	/* Writes the low and high wire bytes without unaligned stores. */
	bytes = buffer;
	bytes[0] = (uint8_t)word;
	bytes[1] = (uint8_t)(word >> 8);

	/* Succeeded: the complete field is ready for its publication barrier. */
	return;
}

/*
 * Encodes one little-endian control field.
 */
void
drv_venus_store32(
	void *buffer,
	uint32_t word)
{
	uint8_t *bytes;

	/* Writes every wire byte explicitly instead of relying on host layout. */
	bytes = buffer;
	bytes[0] = (uint8_t)word;
	bytes[1] = (uint8_t)(word >> 8);
	bytes[2] = (uint8_t)(word >> 16);
	bytes[3] = (uint8_t)(word >> 24);

	/* Succeeded: the complete field is ready for its publication barrier. */
	return;
}

/*
 * Encodes one split 64-bit wire field.
 */
void
drv_venus_store64(
	void *buffer,
	uint64_t word)
{
	uint8_t *bytes;

	/* Places the least significant half before the most significant half. */
	bytes = buffer;
	drv_venus_store32(bytes, (uint32_t)word);
	drv_venus_store32(bytes + 4U, (uint32_t)(word >> 32));

	/* Succeeded: both halves use the protocol's little-endian ordering. */
	return;
}

/* Walks the bounded PCI capability chain once and rejects cycles. */
static int
venus_capabilities(
	struct venus_transport *transport)
{
	uint8_t visited[256];
	uint8_t offset;
	uint8_t next;
	uint8_t kind;
	uint8_t length;
	uint8_t type;
	int error;

	/* Tracks each capability offset to reject malformed linked cycles. */
	kern_memset(visited, 0, sizeof(visited));
	error = drv_pci_device_config_read8(transport->pci, 0x34U, &offset);
	if (error != 0)
		return error;

	/* Visits only conventional capabilities inside the PCI header space. */
	while (offset != 0) {
		/* Rejects unaligned or truncated capability headers. */
		if (offset < 0x40U ||
		    offset > 0xfcU ||
		    (offset & 3U) != 0)
			return EINVAL;

		/* A repeated offset would keep enumeration from making progress. */
		if (visited[offset] != 0)
			return EINVAL;

		/* Marks the header before following any device-controlled next link. */
		visited[offset] = 1;
		error = drv_pci_device_config_read8(transport->pci, offset, &kind);
		if (error != 0)
			return error;

		/* Saves the next link before decoding an optional vendor payload. */
		error = drv_pci_device_config_read8(transport->pci, offset + 1U, &next);
		if (error != 0)
			return error;

		/* Only Virtio vendor capabilities describe this device's windows. */
		if (kind == 9U) {
			/* Reads the bounded payload length before its type or BAR fields. */
			error = drv_pci_device_config_read8(transport->pci, offset + 2U, &length);
			if (error != 0)
				return error;

			/* Vendor payloads must contain the standard 16-byte prefix. */
			if (length < 16U || (unsigned)offset + length > 256U)
				return EINVAL;

			/* Classifies the window before decoding its type-specific suffix. */
			error = drv_pci_device_config_read8(transport->pci, offset + 3U, &type);
			if (error != 0)
				return error;

			/* Retains a validated window or ignores unrelated vendor types. */
			error = venus_capability(transport, offset, length, type);
			if (error != 0)
				return error;
		}

		/* Continues from the saved link, including non-Virtio capabilities. */
		offset = next;
	}

	/* Venus requires a host-visible aperture for mapped reply resources. */
	if (transport->host_visible.length == 0)
		return EOPNOTSUPP;

	/* Succeeded: all available windows are recorded with bounded offsets. */
	return 0;
}

/* Records one known window and its type-specific addressing suffix. */
static int
venus_capability(
	struct venus_transport *transport,
	unsigned offset,
	unsigned length,
	unsigned type)
{
	struct venus_window *window;
	struct drv_pci_bar bar;
	uint8_t bar_index;
	uint8_t identifier;
	uint32_t low;
	uint32_t high;
	int error;

	/* Selects only windows consumed by this private transport. */
	window = NULL;
	switch (type) {
	case 1U:
		window = &transport->common;
		break;
	case 2U:
		window = &transport->notify;
		break;
	case 3U:
		window = &transport->isr;
		break;
	case 4U:
		window = &transport->configuration;
		break;
	case 8U:
		/* Shared-memory IDs distinguish host visibility from other apertures. */
		error = drv_pci_device_config_read8(transport->pci, offset + 5U, &identifier);
		if (error != 0)
			return error;

		/* Other shared memory has no role in this Venus backend. */
		if (identifier != 1U)
			return 0;

		/* A host-visible capability contains the two high address halves. */
		if (length < 24U)
			return EINVAL;

		/* Keeps the host-visible window for checked blob views of one whole BAR. */
		window = &transport->host_visible;
		break;
	default:
		/* Succeeded: an unrelated capability needs no driver ownership. */
		return 0;
	}

	/* Rejects duplicate windows instead of silently replacing ownership. */
	if (window->length != 0)
		return EINVAL;

	/* Reads the BAR selector before its physical range. */
	error = drv_pci_device_config_read8(transport->pci, offset + 4U, &bar_index);
	if (error != 0)
		return error;

	/* A selector must name one of the conventional BAR positions. */
	if (bar_index >= 6U)
		return EINVAL;

	/* Reads the low offset and retains it in a 64-bit bounded value. */
	error = drv_pci_device_config_read32(transport->pci, offset + 8U, &low);
	if (error != 0)
		return error;

	/* Initializes the complete window before decoding optional high halves. */
	window->bar = bar_index;
	window->offset = low;

	/* Reads the low length independently of the offset transaction. */
	error = drv_pci_device_config_read32(transport->pci, offset + 12U, &low);
	if (error != 0)
		return error;

	/* Conventional windows occupy only their low 32-bit advertised length. */
	window->length = low;

	/* Shared memory may extend above the conventional 32-bit BAR range. */
	if (type == 8U) {
		/* Adds the high offset without narrowing the advertised location. */
		error = drv_pci_device_config_read32(transport->pci, offset + 16U, &high);
		if (error != 0)
			return error;

		/* Completes the offset before reading the separate length suffix. */
		window->offset |= (uint64_t)high << 32;
		error = drv_pci_device_config_read32(transport->pci, offset + 20U, &high);
		if (error != 0)
			return error;

		/* Completes the shared-memory size in its advertised width. */
		window->length |= (uint64_t)high << 32;
	}

	/* Validates the window against the actual BAR rather than trusting config. */
	error = drv_pci_device_bar(transport->pci, bar_index, &bar);
	if (error != 0)
		return error;

	/* Device-memory windows cannot be implemented by a port I/O BAR. */
	if (bar.type != DRV_PCI_BAR_MEMORY32 && bar.type != DRV_PCI_BAR_MEMORY64)
		return EINVAL;

	/* Bounds subtraction avoids overflow in a device-controlled offset+size. */
	if (window->offset > bar.size || window->length > bar.size - window->offset)
		return EINVAL;

	/* Claims shared BARs only once even when several capabilities use them. */
	if (transport->claimed[bar_index] == 0) {
		error = drv_pci_device_claim_bar(transport->pci, bar_index);
		if (error != 0)
			return error;

		/* Records the claim immediately so every failure can release it. */
		transport->claimed[bar_index] = 1;
	}

	/* Notification offsets are multiplied by the capability's extra field. */
	if (type == 2U) {
		/* The multiplier is unavailable in a short vendor capability. */
		if (length < 20U)
			return EINVAL;

		/* Reads the doorbell scale without assuming a fixed QEMU layout. */
		error = drv_pci_device_config_read32(
			transport->pci,
			offset + 16U,
			&transport->notify_multiplier);
		if (error != 0)
			return error;
	}

	/* Succeeded: the window and its BAR lifetime now belong to the transport. */
	return 0;
}

/* Borrows a checked capability slice from one complete mapped register BAR. */
static int
venus_map_window(
	struct venus_transport *transport,
	struct venus_window *window,
	size_t minimum)
{
	struct drv_pci_bar bar;
	struct drv_pci_mapping *mapping;
	int error;

	/* Requires the exact field prefix consumed by the caller. */
	if (window->length < minimum || window->length > SIZE_MAX)
		return EINVAL;

	/* Resolves the complete BAR so page-sized mapping and relocation stay sound. */
	error = drv_pci_device_bar(transport->pci, window->bar, &bar);
	if (error != 0)
		return error;

	/* Register BARs are small; the separate host-visible aperture is never mapped here. */
	if (bar.size == 0U || bar.size > 65536U)
		return EOPNOTSUPP;

	/* Checks the borrowed view against its containing BAR before pointer arithmetic. */
	if (window->offset > bar.size || window->length > bar.size - window->offset)
		return EINVAL;

	/* Maps each register BAR once, allowing several capability slices to share it. */
	mapping = &transport->registers[window->bar];
	if (mapping->address == NULL) {
		error = drv_pci_device_map_bar(
			transport->pci,
			window->bar,
			DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
			mapping);
		if (error != 0)
			return error;
	}

	/* A slice has no independent unmap ownership; the register array retains it. */
	window->mapping.address = (uint8_t *)mapping->address + (size_t)window->offset;
	window->mapping.size = (size_t)window->length;
	window->mapping.type = mapping->type;

	/* Succeeded: the capability is addressable without mapping an unaligned fragment. */
	return 0;
}

/* Maps one bounded whole host BAR so blob slices never trigger BAR relocation. */
static int
venus_map_aperture(
	struct venus_transport *transport)
{
	struct venus_window *window;
	struct drv_pci_bar bar;
	int error;

	/* Resolves the complete PCI allocation behind the host-visible capability. */
	window = &transport->host_visible;
	error = drv_pci_device_bar(transport->pci, window->bar, &bar);
	if (error != 0)
		return error;

	/* The backend bounds its whole BAR within the verified dynamic device window. */
	if (bar.size == 0U || bar.size > VENUS_MAX_APERTURE_BYTES)
		return EOPNOTSUPP;

	/* Register and host-memory ownership must not describe the same mapped BAR. */
	if (transport->registers[window->bar].address != NULL)
		return EOPNOTSUPP;

	/* Bounds the shared-memory view before establishing any CPU access. */
	if (window->offset > bar.size || window->length > bar.size - window->offset)
		return EINVAL;

	/* Whole-BAR mapping preserves alignment if the PCI platform relocates it. */
	error = drv_pci_device_map_bar(
		transport->pci,
		window->bar,
		DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
		&transport->host_mapping);
	if (error != 0)
		return error;

	/* Blob resources borrow slices; only transport stop owns the final unmap. */
	window->mapping.address = (uint8_t *)transport->host_mapping.address + (size_t)window->offset;
	window->mapping.size = (size_t)window->length;
	window->mapping.type = transport->host_mapping.type;

	/* Succeeded: future blob mappings never enter the subrange relocation path. */
	return 0;
}

/* Observes reset completion before declaring old DMA ownership finished. */
static int
venus_reset(
	struct venus_transport *transport)
{
	uint8_t *common;
	uint8_t status;
	unsigned attempts;

	/* Withdraws DRIVER_OK before polling the required zero-status barrier. */
	common = transport->common.mapping.address;
	kern_mmio_write8(common + 20U, 0U);

	/* A finite register-read bound also works before the first kernel tick. */
	for (attempts = 0; attempts < 1000000U; attempts++) {
		/* Reads the device acknowledgment of complete transport reset. */
		status = kern_mmio_read8(common + 20U);
		if (status == 0U)
			break;
	}

	/* Failed reset leaves queue memory and backend resources quarantined. */
	if (status != 0U) {
		atomic_raw_store_release(&transport->failed, 1U);
		return EBUSY;
	}

	/* No queue is enabled after the acknowledged reset. */
	transport->enabled = 0;

	/* Succeeded: transport-owned DMA allocations can be retired safely. */
	return 0;
}

/* Negotiates modern split queues and the Vulkan context/blob requirements. */
static int
venus_negotiate(
	struct venus_transport *transport)
{
	uint8_t *common;
	uint32_t high;
	uint32_t low;
	uint8_t status;

	/* Acknowledges the device and selects this transport as its driver. */
	common = transport->common.mapping.address;
	kern_mmio_write8(common + 20U, 3U);
	kern_mmio_write32(common, 0U);
	low = kern_mmio_read32(common + 4U);
	kern_mmio_write32(common, 1U);
	high = kern_mmio_read32(common + 4U);

	/* Records the offered feature words when negotiation cannot proceed. */
	kern_logf("venus: offered features %08x:%08x\n", high, low);

	/* VERSION_1 makes the modern little-endian split-queue contract mandatory. */
	if ((high & 1U) == 0)
		return EOPNOTSUPP;

	/* VIRGL, RESOURCE_BLOB and CONTEXT_INIT are required for Venus contexts. */
	if ((low & VENUS_REQUIRED_FEATURES) != VENUS_REQUIRED_FEATURES)
		return EOPNOTSUPP;

	/* Declines packed rings, indirect descriptors, and unimplemented features. */
	transport->features = VENUS_REQUIRED_FEATURES;

	/* EDID is optional; negotiation enables real timing discovery only when offered. */
	if ((low & VENUS_FEATURE_EDID) != 0U)
		transport->features |= VENUS_FEATURE_EDID;

	/* Publish the exact implemented feature subset before FEATURES_OK. */
	kern_mmio_write32(common + 8U, 0U);
	kern_mmio_write32(common + 12U, transport->features);
	kern_mmio_write32(common + 8U, 1U);
	kern_mmio_write32(common + 12U, 1U);
	kern_mmio_write16(common + 16U, 0xffffU);
	kern_mmio_write8(common + 20U, 11U);
	status = kern_mmio_read8(common + 20U);

	/* The device may reject a feature combination even when bits were offered. */
	if ((status & 8U) == 0)
		return EOPNOTSUPP;

	/* Succeeded: both sides accepted the exact feature subset used below. */
	return 0;
}

/* Prepares a small queue using two persistent descriptors per transaction. */
static int
venus_queue_start(
	struct venus_transport *transport)
{
	uint8_t *common;
	uint8_t *ring;
	uint64_t address;
	uint64_t notify_byte;
	uint16_t maximum;
	unsigned index;
	int error;

	/* Selects control queue zero and checks its advertised descriptor capacity. */
	common = transport->common.mapping.address;
	kern_mmio_write16(common + 22U, 0U);
	maximum = kern_mmio_read16(common + 24U);
	if (maximum < 8U)
		return EOPNOTSUPP;

	/* Use the largest supported power of two within the approved descriptor budget. */
	transport->queue_size = VENUS_QUEUE_SIZE;
	while (transport->queue_size > maximum)
		transport->queue_size /= 2U;

	/* Each request owns exactly one readable and one writable descriptor. */
	transport->slot_count = transport->queue_size / 2U;

	/* Allocates every shared object before enabling the selected queue. */
	error = drv_dma_alloc_coherent(transport->dma, 4096U, 4096U, &transport->ring);
	if (error != 0)
		return error;

	/* Zeroes initial indices and enables normal used-ring interrupt notification. */
	ring = transport->ring.address;
	kern_memset(ring, 0, 4096U);
	drv_venus_store16(ring + VENUS_RING_AVAILABLE, 0U);

	/* Every descriptor pair owns independent persistent command and reply bytes. */
	for (index = 0U; index < transport->slot_count; index++) {
		/* Request storage remains retained if a later allocation or host operation fails. */
		error = drv_dma_alloc_coherent(
			transport->dma,
			VENUS_COMMAND_BYTES,
			4096U,
			&transport->requests[index].request);
		if (error != 0)
			return error;

		/* Response allocation is checked before constructing the next independent pair. */
		error = drv_dma_alloc_coherent(
			transport->dma,
			VENUS_RESPONSE_BYTES,
			4096U,
			&transport->requests[index].response);
		if (error != 0)
			return error;
	}

	/* Selects the negotiated queue shape without MSI or event-index features. */
	kern_mmio_write16(common + 24U, transport->queue_size);
	kern_mmio_write16(common + 26U, 0xffffU);
	transport->notify_offset = kern_mmio_read16(common + 30U);
	notify_byte = (uint64_t)transport->notify_offset * transport->notify_multiplier;
	if (notify_byte > transport->notify.length - 2U)
		return EINVAL;

	/* Publishes descriptor addresses in their required low/high register pairs. */
	address = transport->ring.device_address;
	kern_mmio_write32(common + 32U, (uint32_t)address);
	kern_mmio_write32(common + 36U, (uint32_t)(address >> 32));
	address = transport->ring.device_address + VENUS_RING_AVAILABLE;
	kern_mmio_write32(common + 40U, (uint32_t)address);
	kern_mmio_write32(common + 44U, (uint32_t)(address >> 32));
	address = transport->ring.device_address + VENUS_RING_USED;
	kern_mmio_write32(common + 48U, (uint32_t)address);
	kern_mmio_write32(common + 52U, (uint32_t)(address >> 32));
	kern_io_write_barrier();
	kern_mmio_write16(common + 28U, 1U);

	/* Succeeded: DRIVER_OK may now allow this initialized queue to run. */
	return 0;
}

/* Finds the Venus capset and checks that its response fits bounded storage. */
static int
venus_capset_find(
	struct venus_transport *transport)
{
	uint8_t command[32];
	uint8_t response[40];
	uint8_t *configuration;
	uint32_t count;
	uint32_t index;
	uint32_t bytes;
	uint32_t identifier;
	uint32_t type;
	unsigned found;
	int error;

	/* Reads a bounded count before issuing capability queries. */
	configuration = transport->configuration.mapping.address;
	count = kern_mmio_read32(configuration + 12U);
	if (count == 0 || count > 64U)
		return EOPNOTSUPP;

	/* Selects Venus by capset identity instead of assuming enumeration order. */
	found = 0U;
	for (index = 0; index < count; index++) {
		/* Requests one capability descriptor in the device-advertised range. */
		kern_memset(command, 0, sizeof(command));
		drv_venus_header(command, 0x0108U, 0U);
		drv_venus_store32(command + 24U, index);
		error = drv_venus_transport_command(
			transport,
			command,
			sizeof(command),
			response,
			sizeof(response),
			&bytes);
		if (error != 0)
			return error;

		/* A capset-info response must contain its full identity and size. */
		type = drv_venus_load32(response);
		if (bytes != sizeof(response) || type != 0x1102U)
			return EIO;

		/* Only capset four carries the Venus Vulkan serialization protocol. */
		identifier = drv_venus_load32(response + 24U);
		if (identifier != 4U)
			continue;

		/* Retains only a bounded capability payload supported by the UAPI. */
		transport->capset_size = drv_venus_load32(response + 32U);
		if (transport->capset_size == 0 || transport->capset_size > 256U)
			return EOPNOTSUPP;

		/* A matching complete capset ends the search without further commands. */
		found = 1U;
		break;
	}

	/* Reports a VirGL-only device without attempting an incompatible context. */
	if (found == 0U)
		return EOPNOTSUPP;

	/* Trust strict GPU completion only after reading the paired host's complete capability. */
	error = venus_strict_queue_find(transport);
	if (error != 0)
		return error;

	/* Succeeded: the negotiated profile precedes every independent GPU session. */
	return 0;
}

/* Recognizes the exact paired-host contract before granting kernel-owned success authority. */
static int
venus_strict_queue_find(
	struct venus_transport *transport)
{
	uint8_t command[32];
	uint8_t response[VENUS_HEADER_BYTES + GPU_CAPSET_MAX];
	uint32_t bytes;
	uint32_t type;
	uint32_t magic;
	uint32_t flags;
	int error;

	/* Unknown and stock capability layouts preserve discovery without claiming strict completion. */
	transport->strict_queue = 0U;
	transport->quiesce = 0U;
	if (transport->capset_size != VENUS_VENDOR_CAPSET_BYTES)
		return 0;

	/* Query the full payload instead of trusting its advertised length as proof of semantics. */
	kern_memset(command, 0, sizeof(command));
	drv_venus_header(command, 0x0109U, 0U);
	drv_venus_store32(command + 24U, 4U);
	error = drv_venus_transport_command(
		transport,
		command,
		sizeof(command),
		response,
		sizeof(response),
		&bytes);
	if (error != 0)
		return error;

	/* Exact response framing prevents a short tail from borrowing stale success flags. */
	type = drv_venus_load32(response);
	if (type != 0x1103U || bytes != VENUS_HEADER_BYTES + VENUS_VENDOR_CAPSET_BYTES)
		return EIO;

	/* Only the acknowledged paired profile proves native-fence success and safe failed retirement. */
	magic = drv_venus_load32(response + VENUS_HEADER_BYTES + 160U);
	flags = drv_venus_load32(response + VENUS_HEADER_BYTES + 164U);
	if (magic == VENUS_VENDOR_CAPSET_MAGIC &&
	    (flags == VENUS_VENDOR_STRICT_FLAGS || flags == VENUS_VENDOR_QUIESCE_FLAGS)) {
		transport->strict_queue = 1U;
		if (flags == VENUS_VENDOR_QUIESCE_FLAGS)
			transport->quiesce = 1U;
	}

	/* Succeeded: legacy discovery remains usable when strict jobs are unavailable. */
	return 0;
}

/* Maps protocol refusal codes to the kernel's ordinary error convention. */
static int
venus_response_error(
	uint32_t type)
{
	/* Preserves the renderer's allocation-failure distinction. */
	if (type == 0x1201U)
		return ENOMEM;

	/* A defined protocol refusal indicates an invalid operation or identity. */
	if (type >= 0x1202U && type <= 0x1205U)
		return EINVAL;

	/* Refuses unrecognized or unspecified device failures. */
	if (type < 0x1100U || type > 0x1106U)
		return EIO;

	/* Succeeded: the response belongs to the defined success range. */
	return 0;
}

/* Posts one persistent two-descriptor chain while preserving a control-only reserve. */
static int
venus_request_post(
	struct venus_transport *transport,
	const void *command,
	uint32_t bytes,
	unsigned queue_marker,
	struct drv_gpu_completion *completion,
	struct venus_request **result)
{
	unsigned failed;
	struct venus_request *request;
	uint64_t started;
	uint64_t deadline;
	unsigned index;
	unsigned limit;
	unsigned long irq;
	int expired;
	int error;

	/* The reserved final chain can always carry the command that unblocks queued GPU work. */
	*result = NULL;
	limit = transport->slot_count;
	if (queue_marker != 0U) {
		/* Reserve at least one control chain, and four at the full queue size. */
		index = transport->slot_count / 8U;
		if (index == 0U)
			index = 1U;
		limit -= index;
	}

	/* Bounds queue-space admission independently from later device execution. */
	started = clock_milliseconds(NULL);
	deadline = sched_ticks() + KERN_CLOCK_HZ * VENUS_WAIT_MILLISECONDS / 1000U;
	irq = spin_lock_irqsave(&transport->queue_lock);

	while (1) {
		/* Failed or stopping hardware cannot acquire fresh command ownership. */
		failed = atomic_raw_load_acquire(&transport->failed);
		if (failed != 0U ||
	    transport->enabled == 0U ||
	    transport->stopping != 0U) {
			spin_unlock_irqrestore(&transport->queue_lock, irq);
			return ENODEV;
		}

		/* Reserves only a chain whose prior callback and DMA ownership have ended. */
		request = NULL;
		for (index = 0U; index < limit; index++) {
			if (transport->requests[index].state == VENUS_SLOT_FREE) {
				request = &transport->requests[index];
				break;
			}
		}

		/* Found storage remains protected until the complete descriptor chain is published. */
		if (request != NULL)
			break;

		/* Optional GPU notification must never block future work that makes its fence runnable. */
		if (queue_marker != 0U) {
			spin_unlock_irqrestore(&transport->queue_lock, irq);
			return EAGAIN;
		}

		/* Ordinary control traffic sleeps until an IRQ releases one of the bounded slots. */
		error = venus_queue_wait_locked(transport, deadline, WAITQ_INTERRUPTIBLE, &irq);
		if (error != 0 && error != EAGAIN) {
			spin_unlock_irqrestore(&transport->queue_lock, irq);
			return error;
		}

		/* The idle-thread fallback also observes the finite queue-space deadline. */
		expired = venus_wait_expired(started);
		if (expired != 0) {
			spin_unlock_irqrestore(&transport->queue_lock, irq);
			return ETIMEDOUT;
		}
	}

	/* Fenced identities never wrap into a still-observable renderer timeline value. */
	request->flags = drv_venus_load32((const uint8_t *)command + 4U);
	request->fence = drv_venus_load64((const uint8_t *)command + 8U);
	if ((request->flags & 1U) != 0U && request->fence == 0U) {
		/* Context-marker allocation owns a global transport sequence until checked reset. */
		if (transport->next_fence == 0U) {
			spin_unlock_irqrestore(&transport->queue_lock, irq);
			return EOVERFLOW;
		}

		/* Zero after the last allocation permanently refuses further marker reuse. */
		request->fence = transport->next_fence++;
	}

	/* Copies bounded bytes while the queue lock excludes descriptor publication races. */
	kern_memcpy(request->request.address, command, bytes);
	kern_memset(request->response.address, 0, VENUS_RESPONSE_BYTES);
	drv_venus_store64((uint8_t *)request->request.address + 8U, request->fence);
	request->context = drv_venus_load32((const uint8_t *)command + 16U);
	request->completion = completion;
	request->notifying = 0U;
	request->bytes = 0U;
	request->error = 0;
	request->started_ms = clock_milliseconds(NULL);
	request->supervised = 0U;
	request->state = VENUS_SLOT_POSTED;

	/* Publication reuses the capacity retained by this request's admitted owner. */
	venus_request_publish_locked(transport, request, index, bytes);
	*result = request;

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* Succeeded: only this request's matching used completion may release its DMA chain. */
	return 0;
}

/* Publishes an already reserved DMA chain without allocating or waiting for capacity. */
static void
venus_request_publish_locked(
	struct venus_transport *transport,
	struct venus_request *request,
	unsigned index,
	uint32_t bytes)
{
	uint8_t *ring;
	uint8_t *descriptor;
	uint64_t notify_byte;
	uint16_t head;
	uint16_t published;

	/* Each slot owns an even readable descriptor followed by one writable response. */
	ring = transport->ring.address;
	head = (uint16_t)(index * 2U);
	descriptor = ring + head * 16U;
	drv_venus_store64(descriptor, request->request.device_address);
	drv_venus_store32(descriptor + 8U, bytes);
	drv_venus_store16(descriptor + 12U, 1U);
	drv_venus_store16(descriptor + 14U, head + 1U);

	/* The response descriptor is not reused before its matching used entry is consumed. */
	descriptor += 16U;
	drv_venus_store64(descriptor, request->response.device_address);
	drv_venus_store32(descriptor + 8U, VENUS_RESPONSE_BYTES);
	drv_venus_store16(descriptor + 12U, 2U);
	drv_venus_store16(descriptor + 14U, 0U);

	/* Publishes the chain only after all request and descriptor stores are visible. */
	drv_venus_store16(
		ring + VENUS_RING_AVAILABLE + 4U + (transport->available % transport->queue_size) * 2U,
		head);
	kern_io_write_barrier();
	transport->available++;
	published = transport->available;
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
	/* The aligned avail index is published once in little-endian wire order. */
	published = __builtin_bswap16(published);
#endif
	__atomic_store_n((uint16_t *)(ring + VENUS_RING_AVAILABLE + 2U), published, __ATOMIC_RELEASE);
	kern_io_barrier();

	/* Rings the validated control doorbell after transferring DMA ownership to the device. */
	notify_byte = (uint64_t)transport->notify_offset * transport->notify_multiplier;
	kern_mmio_write16((uint8_t *)transport->notify.mapping.address + (size_t)notify_byte, 0U);
	transport->submitted_total++;
	waitq_wake_all(&transport->queue_waitq);

	/* Succeeded: the device owns this exact initialized descriptor chain. */
	return;
}

/* Waits for a synchronous request without spinning in an ordinary runtime thread. */
static int
venus_request_wait(
	struct venus_transport *transport,
	struct venus_request *request)
{
	uint64_t deadline;
	uint64_t last;
	uint64_t now;
	unsigned stalled;
	unsigned long irq;
	int error;

	/* A finite hardware deadline also bounds synchronous context/resource teardown. */
	deadline = sched_ticks() + KERN_CLOCK_HZ * VENUS_WAIT_MILLISECONDS / 1000U;
	last = clock_milliseconds(NULL);
	stalled = 0U;
	error = 0;
	irq = spin_lock_irqsave(&transport->queue_lock);

	while (request->state == VENUS_SLOT_POSTED) {
		/* The normal path sleeps; the boot idle fallback drains without trapping IRQs disabled. */
		error = venus_queue_wait_locked(transport, deadline, 0U, &irq);
		if (error != 0 && error != EAGAIN)
			break;

		/* A stopped early clock cannot turn capability discovery into an unbounded spin. */
		now = clock_milliseconds(NULL);
		if (now == last) {
			stalled++;
		} else {
			last = now;
			stalled = 0U;
		}

		/* Deadline expiry never reuses a chain still owned by the device. */
		if (now - request->started_ms >= VENUS_WAIT_MILLISECONDS || stalled >= VENUS_WAIT_POLLS) {
			error = ETIMEDOUT;
			break;
		}
	}

	/* A callback-free synchronous slot may have been failed by the shared watchdog. */
	if (request->state == VENUS_SLOT_QUARANTINED)
		error = request->error;

	/* A registration race that already completed the command needs no further sleep. */
	if (error == EAGAIN && request->state == VENUS_SLOT_COMPLETE)
		error = 0;

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* Every missing response closes submission and wakes all other retained requests. */
	if (error != 0) {
		drv_venus_transport_fail(transport, error);
		return error;
	}

	/* Succeeded: the matching response remains owned by the synchronous caller. */
	return 0;
}

/* Sleeps on queue state while keeping the early idle-thread discovery path finite. */
static int
venus_queue_wait_locked(
	struct venus_transport *transport,
	uint64_t deadline,
	unsigned flags,
	unsigned long *irq)
{
	struct thread *thread;
	uint64_t observed;
	unsigned collected;
	int error;

	/* Ordinary threads register their sleep atomically with the completion condition. */
	thread = thread_current();
	if (thread != NULL && (thread->flags & THREAD_FLAG_IDLE) == 0U) {
		observed = waitq_sequence(&transport->queue_waitq);
		transport->sleep_total++;
		error = waitq_sleep(&transport->queue_waitq, &transport->queue_lock, observed, deadline, flags);
		if (error != 0)
			return error;

		/* Succeeded: the caller rechecks its own condition after this wakeup. */
		return 0;
	}

	/* Idle discovery cannot sleep with the only runnable CPU retaining disabled interrupts. */
	spin_unlock_irqrestore(&transport->queue_lock, *irq);
	collected = venus_queue_collect(transport);
	if (collected == UINT_MAX)
		drv_venus_transport_fail(transport, EIO);

	/* Completion publication happens outside the transport lock even before normal scheduling. */
	venus_queue_notify(transport);
	kern_compiler_barrier();
	*irq = spin_lock_irqsave(&transport->queue_lock);

	/* Succeeded: the caller's own clock and stalled-clock bounds control this boot-only retry. */
	return 0;
}

/* Consumes bounded used entries while matching independent descriptor ownership. */
static unsigned
venus_queue_collect(
	struct venus_transport *transport)
{
	unsigned failed;
	struct venus_request *request;
	uint8_t *ring;
	uint16_t published;
	uint32_t head;
	uint32_t bytes;
	unsigned count;
	unsigned long irq;
	int error;

	/* Queue storage remains mapped until checked IRQ removal has drained every handler. */
	count = 0U;
	irq = spin_lock_irqsave(&transport->queue_lock);

	failed = atomic_raw_load_acquire(&transport->failed);
	if (transport->ring.address == NULL || failed != 0U) {
		spin_unlock_irqrestore(&transport->queue_lock, irq);
		return 0U;
	}

	/* Acquire publication before reading any response bytes written by the device. */
	ring = transport->ring.address;
	published = __atomic_load_n((uint16_t *)(ring + VENUS_RING_USED + 2U), __ATOMIC_ACQUIRE);
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
	/* The aligned used index is one little-endian device publication. */
	published = __builtin_bswap16(published);
#endif

	/* A device cannot return more simultaneously owned chains than the queue contains. */
	if ((uint16_t)(published - transport->used) > transport->slot_count) {
		spin_unlock_irqrestore(&transport->queue_lock, irq);
		return UINT_MAX;
	}

	/* Out-of-order contexts are legal; each used head selects its exact retained slot. */
	while (transport->used != published) {
		kern_io_read_barrier();
		head = drv_venus_load32(ring + VENUS_RING_USED + 4U + (transport->used % transport->queue_size) * 8U);
		bytes = drv_venus_load32(ring + VENUS_RING_USED + 8U + (transport->used % transport->queue_size) * 8U);

		/* Odd, out-of-range and already retired descriptor heads cannot release another request. */
		if (head >= transport->queue_size || (head & 1U) != 0U) {
			spin_unlock_irqrestore(&transport->queue_lock, irq);
			return UINT_MAX;
		}

		/* A late return from an isolated context ends device ownership of that chain. */
		request = &transport->requests[head / 2U];
		if (request->state == VENUS_SLOT_QUARANTINED) {
			/* A callback still owed publication retires through ordinary completion; otherwise nobody reads the reply. */
			if (request->completion != NULL) {
				request->state = VENUS_SLOT_COMPLETE;
			} else {
				request->state = VENUS_SLOT_FREE;
				request->context = 0U;
				request->supervised = 0U;
				transport->reclaimed = 1U;
			}

			/* The device has released the chain either way. */
			transport->used++;
			transport->completed_total++;
			count++;
			continue;
		}

		/* Only a currently posted chain authorizes consumption of its response. */
		if (request->state != VENUS_SLOT_POSTED) {
			spin_unlock_irqrestore(&transport->queue_lock, irq);
			return UINT_MAX;
		}

		/* Bounds and fence identity are checked before any caller sees completed bytes. */
		error = venus_request_validate(request, bytes);
		request->bytes = bytes;
		request->error = error;
		request->state = VENUS_SLOT_COMPLETE;
		transport->used++;
		transport->completed_total++;
		count++;

		/* A malformed or misattributed reply proves protocol corruption; a defined refusal retires only its own request. */
		if (error == EIO) {
			spin_unlock_irqrestore(&transport->queue_lock, irq);
			return UINT_MAX;
		}
	}

	/* All waiters examine their own request, so one IRQ may retire several contexts. */
	if (count != 0U)
		waitq_wake_all(&transport->queue_waitq);

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* Succeeded: the count describes exact chains consumed by this bounded drain. */
	return count;
}

/* Publishes asynchronous results while retaining callback storage through final-close drain. */
static void
venus_queue_notify(
	struct venus_transport *transport)
{
	struct venus_request *request;
	struct drv_gpu_completion *completion;
	unsigned index;
	unsigned freed;
	unsigned long irq;
	int error;

	freed = 0U;

	/* Each independent chain can owe at most one common terminal publication. */
	for (index = 0U; index < transport->slot_count; index++) {
		irq = spin_lock_irqsave(&transport->queue_lock);

		request = &transport->requests[index];
		if (request->completion == NULL ||
		    request->state == VENUS_SLOT_POSTED ||
		    request->state == VENUS_SLOT_RESERVED ||
		    request->notifying != 0U) {
			spin_unlock_irqrestore(&transport->queue_lock, irq);
			continue;
		}

		/* The pointer remains in the slot while the callback executes outside this lock. */
		request->notifying = 1U;
		completion = request->completion;
		error = request->error;

		spin_unlock_irqrestore(&transport->queue_lock, irq);

		/* Common publication may wake close, which still sees the retained callback pointer. */
		drv_gpu_complete(completion, error);

		/* Matching used completion permits reuse only after callback publication has returned. */
		irq = spin_lock_irqsave(&transport->queue_lock);

		request->completion = NULL;
		request->notifying = 0U;
		if (request->state == VENUS_SLOT_COMPLETE) {
			request->state = VENUS_SLOT_FREE;
			freed = 1U;
		}

		/* Quarantined DMA remains unavailable even though userspace has a terminal result. */
		waitq_wake_all(&transport->queue_waitq);

		spin_unlock_irqrestore(&transport->queue_lock, irq);
	}

	/* A chain returned late by an isolated context became free without any callback. */
	irq = spin_lock_irqsave(&transport->queue_lock);

	if (transport->reclaimed != 0U) {
		transport->reclaimed = 0U;
		freed = 1U;
	}

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* Only final callback retirement makes a completed chain available to common waiters. */
	if (freed != 0U)
		venus_capacity_changed(transport);

	/* Succeeded: all claimable terminal callbacks have been published exactly once. */
	return;
}

/* Notifies common capacity waiters through an independently retained publication. */
static void
venus_capacity_changed(
	struct venus_transport *transport)
{
	struct drv_gpu_device *gpu;
	unsigned long irq;

	/* Publisher withdrawal cannot race the callback's final access to its wrapper. */
	irq = spin_lock_irqsave(&transport->queue_lock);

	gpu = transport->gpu;
	if (gpu != NULL)
		drv_gpu_retain(gpu);

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* Common policy can re-enter the backend snapshot without inheriting the queue lock. */
	if (gpu != NULL) {
		drv_gpu_capacity_changed(gpu);
		drv_gpu_release(gpu);
	}

	/* Succeeded: actual storage changes are visible independently from ledger consumption. */
	return;
}

/* Validates the complete response and only the fence fields QEMU actually echoes. */
static int
venus_request_validate(
	struct venus_request *request,
	uint32_t bytes)
{
	uint8_t *response;
	uint32_t flags;
	uint32_t context;
	uint32_t type;
	uint64_t fence;
	int error;

	/* Device-controlled lengths cannot authorize a read beyond the response allocation. */
	if (bytes < VENUS_HEADER_BYTES || bytes > VENUS_RESPONSE_BYTES)
		return EIO;

	/* QEMU echoes FENCE, fence_id and ctx_id, but not INFO_RING_IDX or ring_idx. */
	response = request->response.address;
	if ((request->flags & 1U) != 0U) {
		flags = drv_venus_load32(response + 4U);
		fence = drv_venus_load64(response + 8U);
		context = drv_venus_load32(response + 16U);
		if ((flags & 1U) == 0U || fence != request->fence || context != request->context)
			return EIO;
	}

	/* A defined protocol refusal remains distinct from malformed completion or timeout. */
	type = drv_venus_load32(response);
	error = venus_response_error(type);
	if (error != 0)
		return error;

	/* Succeeded: the complete reply belongs to this exact submitted control request. */
	return 0;
}

/* Checks the finite elapsed bound without treating a backward clock as fresh time. */
static int
venus_wait_expired(
	uint64_t started)
{
	uint64_t now;

	/* Unsigned elapsed arithmetic also refuses a clock that moved behind its start. */
	now = clock_milliseconds(NULL);
	if (now - started >= VENUS_WAIT_MILLISECONDS)
		return 1;

	/* Succeeded: this request remains within its finite transport deadline. */
	return 0;
}

/* Establishes one modern MSI-X vector or the device's shared INTx source. */
static int
venus_interrupt_start(
	struct venus_transport *transport)
{
	uint8_t *common;
	uint16_t vector;
	unsigned count;
	int error;

	/* PCI owns capability selection, platform routing and interrupt resource allocation. */
	count = 0U;
	error = drv_pci_device_allocate_irqs(
		transport->pci,
		DRV_PCI_IRQ_ALLOW_MSIX | DRV_PCI_IRQ_ALLOW_INTX,
		1U,
		1U,
		&transport->interrupt,
		&count);
	if (error != 0)
		return error;

	/* A partial allocation remains recorded for checked attach rollback. */
	transport->interrupt_count = count;
	if (count != 1U)
		return EIO;

	/* Registration precedes DRIVER_OK and any device-owned descriptor processing. */
	error = drv_pci_device_establish_irq(
		transport->pci,
		&transport->interrupt,
		venus_interrupt,
		transport,
		"venus",
		&transport->interrupt_cookie);
	if (error != 0)
		return error;

	/* MSI-X table indices are distinct from CPU interrupt vectors. */
	common = transport->common.mapping.address;
	if (transport->interrupt.type == DRV_PCI_IRQ_MSIX) {
		vector = (uint16_t)transport->interrupt.index;
		kern_mmio_write16(common + 16U, vector);
		kern_mmio_write16(common + 22U, 0U);
		kern_mmio_write16(common + 26U, vector);

		/* Readback refuses a device that did not accept the selected config vector. */
		vector = kern_mmio_read16(common + 16U);
		if (vector == 0xffffU)
			return EIO;

		/* The control queue must independently accept its shared MSI-X table entry. */
		vector = kern_mmio_read16(common + 26U);
		if (vector == 0xffffU)
			return EIO;
	}

	/* The selected source is part of runtime evidence, not a claim of Vulkan completion. */
	kern_logf("venus: control IRQ type=%u vector=%u slots=%u\n",
		(unsigned)transport->interrupt.type,
		transport->interrupt.vector,
		transport->slot_count);

	/* Succeeded: the enabled queue may now notify this retained transport owner. */
	return 0;
}

/* Removes IRQ dispatch before releasing any transport register or DMA mapping. */
static int
venus_interrupt_stop(
	struct venus_transport *transport)
{
	int error;

	/* Checked removal leaves the cookie and all handler storage intact while dispatch drains. */
	if (transport->interrupt_cookie != NULL) {
		error = drv_pci_device_disestablish_irq_checked(transport->pci, transport->interrupt_cookie);
		if (error != 0)
			return error;

		/* A null cookie now proves no interrupt callback can access the transport. */
		transport->interrupt_cookie = NULL;
	}

	/* PCI vector ownership retires only after handler detachment succeeded. */
	if (transport->interrupt_count != 0U) {
		drv_pci_device_free_irqs(transport->pci, &transport->interrupt, transport->interrupt_count);
		transport->interrupt_count = 0U;
	}

	/* Succeeded: later teardown can safely release the handler's borrowed mappings. */
	return 0;
}

/* Acknowledges this Virtio source and publishes bounded control completions. */
static int
venus_interrupt(
	void *argument)
{
	struct venus_transport *transport;
	unsigned collected;
	unsigned long irq;
	uint8_t status;

	/* INTx ownership is decided only by this function's read-to-clear status byte. */
	transport = argument;
	status = kern_mmio_read8(transport->isr.mapping.address);
	if (transport->interrupt.type == DRV_PCI_IRQ_INTX && status == 0U)
		return 0;

	/* Counts actual driver IRQ dispatches independently from command submissions. */
	irq = spin_lock_irqsave(&transport->queue_lock);

	transport->interrupt_total++;

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* Config IRQs and a shared MSI-X vector expose display invalidation independently of work. */
	if ((status & 2U) != 0U) {
		drv_venus_transport_display_changed(transport);
	} else if (transport->interrupt.type == DRV_PCI_IRQ_MSIX) {
		/* MSI-X need not populate ISR bits, so sample its still-latched device event. */
		status = (uint8_t)kern_mmio_read32(transport->configuration.mapping.address);
		if ((status & 1U) != 0U)
			drv_venus_transport_display_changed(transport);
	}

	/* A single interrupt may retire several independent contexts in any used-ring order. */
	collected = venus_queue_collect(transport);
	if (collected == UINT_MAX)
		drv_venus_transport_fail(transport, EIO);

	/* Completion callbacks run outside the transport lock and never acquire its controller mutex. */
	venus_queue_notify(transport);

	/* Succeeded: this MSI-X vector or asserted INTx source belongs to the device. */
	return 1;
}

/* Starts one deadline worker when the first runtime asynchronous command is submitted. */
static int
venus_worker_start(
	struct venus_transport *transport)
{
	struct thread *worker;
	int error;

	/* A separate start gate cannot block controller commands during a GPU fence wait. */
	mutex_lock(&transport->worker_lock);

	if (transport->worker != NULL) {
		mutex_unlock(&transport->worker_lock);
		return 0;
	}

	/* A teardown request cannot create a new worker after its stop barrier. */
	if (transport->stopping != 0U) {
		mutex_unlock(&transport->worker_lock);
		return ENODEV;
	}

	/* The worker sleeps on deadline changes rather than polling device memory. */
	error = kthread_create(venus_worker, transport, SCHED_PRIORITY_DEFAULT, &worker);
	if (error != 0) {
		mutex_unlock(&transport->worker_lock);
		return error;
	}

	/* Publication precedes scheduling so final teardown can always find its owner. */
	transport->worker = worker;
	thread_start(worker);

	mutex_unlock(&transport->worker_lock);

	/* Succeeded: accepted async commands have a bounded failure owner even without userspace waits. */
	return 0;
}

/* Stops and reaps the watchdog before reset teardown can release its request array. */
static int
venus_worker_stop(
	struct venus_transport *transport)
{
	struct thread *worker;
	uint64_t deadline;
	uint64_t now;
	unsigned long irq;
	int error;

	/* An early attach failure may precede initialization of the wait channels. */
	if (transport->initialized == 0U)
		return 0;

	/* Stops the worker and wakes an otherwise indefinite empty-queue sleep. */
	mutex_lock(&transport->worker_lock);

	irq = spin_lock_irqsave(&transport->queue_lock);

	transport->stopping = 1U;
	worker = transport->worker;
	waitq_wake_all(&transport->queue_waitq);

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* Boot-only use may never have created an asynchronous deadline worker. */
	if (worker == NULL) {
		mutex_unlock(&transport->worker_lock);
		return 0;
	}

	/* The retained thread must exit before mappings and locks can be destroyed or reset. */
	now = sched_ticks();
	if (now > UINT64_MAX - KERN_CLOCK_HZ * 15U) {
		mutex_unlock(&transport->worker_lock);
		return EOVERFLOW;
	}

	/* A successful thread_wait consumes the creator's retained thread reference. */
	deadline = now + KERN_CLOCK_HZ * 15U;
	while (1) {
		error = thread_wait(worker, NULL);
		if (error == 0)
			break;

		/* Only a still-running thread permits a bounded reap retry. */
		if (error != EBUSY) {
			mutex_unlock(&transport->worker_lock);
			return error;
		}

		/* Retains worker and mappings rather than freeing a stalled callback owner. */
		now = sched_ticks();
		if (now >= deadline) {
			mutex_unlock(&transport->worker_lock);
			return ETIMEDOUT;
		}

		/* Lets the woken worker finish its final condition check and exit. */
		sched_sleep(now + 1U);
	}

	/* Null now proves the thread no longer retains transport storage. */
	transport->worker = NULL;

	mutex_unlock(&transport->worker_lock);

	/* Succeeded: no watchdog callback can inspect the transport during teardown. */
	return 0;
}

/* Sleeps until a pending command's deadline or a queue state transition. */
static void
venus_worker(
	void *argument)
{
	struct venus_transport *transport;
	struct venus_request *request;
	uint64_t now;
	uint64_t remaining;
	uint64_t shortest;
	uint64_t deadline;
	uint64_t observed;
	unsigned index;
	unsigned expired;
	unsigned long irq;

	/* The PCI transport explicitly stops and reaps this worker before releasing its state. */
	transport = argument;
	irq = spin_lock_irqsave(&transport->queue_lock);

	while (transport->stopping == 0U) {
		/* Common policy owns jobs; this worker supervises only ordinary transport commands. */
		now = clock_milliseconds(NULL);
		shortest = UINT64_MAX;
		expired = 0U;
		for (index = 0U; index < transport->slot_count; index++) {
			request = &transport->requests[index];
			if (request->state != VENUS_SLOT_POSTED || request->supervised != 0U)
				continue;

			/* A stalled host cannot retain all GPU waiters beyond the finite transport bound. */
			if (now - request->started_ms >= VENUS_WAIT_MILLISECONDS) {
				expired = 1U;
				break;
			}

			/* The earliest request owns the next watchdog wakeup. */
			remaining = VENUS_WAIT_MILLISECONDS - (now - request->started_ms);
			if (remaining < shortest)
				shortest = remaining;
		}

		/* Failure publication can wake common waiters and therefore runs outside the queue lock. */
		if (expired != 0U) {
			spin_unlock_irqrestore(&transport->queue_lock, irq);
			drv_venus_transport_fail(transport, ETIMEDOUT);
			irq = spin_lock_irqsave(&transport->queue_lock);
			continue;
		}

		/* An empty queue sleeps indefinitely until publication or teardown changes its sequence. */
		deadline = 0U;
		if (shortest != UINT64_MAX)
			deadline = sched_ticks() + (shortest * KERN_CLOCK_HZ + 999U) / 1000U;

		/* Lost-wakeup-safe waiting sends no recurring hardware or renderer traffic. */
		observed = waitq_sequence(&transport->queue_waitq);
		(void)waitq_sleep(&transport->queue_waitq, &transport->queue_lock, observed, deadline, 0U);
	}

	spin_unlock_irqrestore(&transport->queue_lock, irq);

	/* Succeeded: the thread trampoline publishes exit for checked teardown to reap. */
	return;
}
