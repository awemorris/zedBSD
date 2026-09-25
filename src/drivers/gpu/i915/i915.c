/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i915 PCI driver: registration, attach and detach.
 *
 * Attach creates the device state and registers the device for a deferred
 * start; the hardware is brought up later by the device start (device.c).
 * Detach refuses while the device still owns hardware or a published node.
 */

#include "i915.h"
#include "command.h"
#include "device.h"
#include "display/display.h"
#include "job.h"
#include "request-queue.h"
#include "reset.h"
#include "resource.h"
#include "session.h"
#include "render/render.h"
#include <kern/kcrt.h>

#include <drivers/gpu/gpu.h>
#include <drivers/pci/pci-i915.h>
#include <drivers/pci/pci.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/waitq.h>

#include <uapi/errno.h>
#include <stddef.h>

#include "intel/pci-ids.h"

/* Builds one exact-match identity row for an Intel graphics product. */
#define I915_ID(product)	{ 0x8086U, (product), DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0U, 0U, 0U }

static int i915_attach(struct drv_pci_device *pci, const struct drv_pci_id *id);
static int i915_detach(struct drv_pci_device *pci, unsigned flags);

/*
 * Registers the i915 PCI driver for the Gen12 Xe graphics devices it covers.
 *
 * Tiger Lake, Alder Lake-P/N and Raptor Lake-P/U are matched.  A matched
 * device whose display this driver has no platform data for says so in its
 * start log instead of programming the wrong registers.
 */
int
drv_pci_i915_driver_register(void)
{
	static const struct drv_pci_id identifiers[] = {
		INTEL_TGL_IDS(I915_ID),
		INTEL_ADLP_IDS(I915_ID),
		INTEL_ADLN_IDS(I915_ID),
		INTEL_RPLU_IDS(I915_ID),
		INTEL_RPLP_IDS(I915_ID)
	};
	static struct drv_pci_driver driver = {
		"i915",
		identifiers,
		sizeof(identifiers) / sizeof(identifiers[0]),
		NULL,
		i915_attach,
		i915_detach,
		NULL,
		NULL,
		NULL,
		{ 0U, 0U, 0U, 0U }
	};
	int error;

	/* Hands probing, binding and the device lifecycle to PCI. */
	error = drv_pci_driver_register(&driver);
	if (error != 0)
		return error;

	/* Succeeded: matching devices are attached during PCI scanning. */
	return 0;
}

/*
 * Prepares the software state the GPU node's operations share.
 *
 * Runs once per device before the request worker is created and the node
 * is published: the IRQ lock, the retire wait queue and the session
 * numbering.
 */
void
drv_i915_node_init(
	struct i915_device *device)
{
	/* The IRQ lock guards the request queues; the wait queue is where drain sleeps. */
	spin_init(&device->irq_lock, LOCK_RANK_DEVICE, "i915 irq");
	waitq_init(&device->retire_waitq, "i915 retire");

	/* Session zero is reserved, so a logged identifier of zero is always a bug. */
	device->next_session = 1U;

	/* Starts the frame timing with an empty window. */
	drv_i915_perf_init(&device->perf);
}

/*
 * Publishes the GPU node.
 *
 * Fills the engine records the sessions queue requests on, binds every
 * operation of the node and registers it with the GPU core.  The hardware
 * is already up and the request worker runs the requests.  A node with a
 * panel also offers its display and the display-only pairing.
 */
int
drv_i915_publish(
	struct i915_device *device)
{
	struct drv_gpu_ops *ops;
	struct i915_engine *engine;
	unsigned index;
	int error;

	/*
	 * Fills the engine records.
	 *
	 * XXX: the records only number and queue requests; the request worker
	 * runs them, and only on the render engine.
	 */
	for (index = 0U; index < I915_ENGINE_COUNT; index++) {
		engine = &device->engines[index];
		kern_memset(engine, 0, sizeof(*engine));
		engine->device = device;
		engine->index = index;

		/* The first record is the render engine, the second the copy engine. */
		if (index == I915_ENGINE_RCS0) {
			engine->class = I915_CLASS_RENDER;
		} else {
			engine->class = I915_CLASS_COPY;
		}

		/* Sequence number 0 is never given, and the record is ready for requests. */
		engine->next_seqno = 1U;
		engine->initialized = 1U;
	}

	/* Attaches the Vulkan executor before the node is registered, so every session can reach it. */
	error = drv_i915_render_attach(device, &device->vk);
	if (error != 0)
		return error;

	/* Describes the node: its interface, its capabilities and every operation. */
	ops = &device->gpu_ops;
	kern_memset(ops, 0, sizeof(*ops));
	ops->version = DRV_GPU_INTERFACE_VERSION;
	ops->size = sizeof(struct drv_gpu_ops);
	ops->capabilities = I915_CAPABILITIES;
	ops->reserved = 0U;

	/* Binds each part's operations. */
	drv_i915_session_bind_ops(ops);
	drv_i915_resource_bind_ops(ops);
	drv_i915_command_bind_ops(ops);
	drv_i915_job_bind_ops(ops);
	drv_i915_recovery_bind_ops(ops);

	/* The display and scanout operations and their capabilities, when the node has a panel. */
	drv_i915_display_bind_ops(device, ops);

	/* Registers the node; the GPU core borrows the table and the device until unregister. */
	error = drv_gpu_register(ops, device, &device->gpu);
	if (error != 0) {
		drv_i915_render_detach(device->vk);
		device->vk = NULL;
		return error;
	}

	/* Names the backend without claiming any rendering works. */
	kern_logf("i915: registered native GPU node (storage, native streams, jobs)\n");

	/* Succeeded: the GPU node accepts sessions. */
	return 0;
}

/*
 * Withdraws the GPU node.
 *
 * Returns EBUSY while sessions are still open; the node stays registered
 * for a retry.
 */
int
drv_i915_unpublish(
	struct i915_device *device)
{
	int error;

	/* A node that was never published has nothing to withdraw. */
	if (device->gpu == NULL)
		return 0;

	/* Unregisters the node; open sessions keep the registration alive. */
	error = drv_gpu_unregister(device->gpu);
	if (error != 0)
		return error;

	/* NULL tells detach and the completions that the node is gone. */
	device->gpu = NULL;

	/* Detaches the Vulkan executor, which no session can reach any more. */
	drv_i915_render_detach(device->vk);
	device->vk = NULL;

	/* Succeeded: the hardware may now be stopped. */
	return 0;
}

/* Creates the device state and registers the device for its deferred start. */
static int
i915_attach(
	struct drv_pci_device *pci,
	const struct drv_pci_id *id)
{
	struct i915_device *device;
	int error;

	UNUSED_PARAMETER(id);

	/* Allocates the device state before any hardware is touched. */
	device = kern_calloc(1U, sizeof(*device));
	if (device == NULL)
		return ENOMEM;

	/* Records the identity so every later log names the device. */
	device->pci = pci;
	device->product = drv_pci_device_product(pci);
	device->revision = drv_pci_device_revision(pci);
	device->stage = "attach";

	/* Creates the mutex that serializes sessions and device-wide changes. */
	error = mutex_init(&device->mutex, LOCK_RANK_DEVICE, "i915");
	if (error != 0) {
		kern_free(device);
		return error;
	}

	/* Prepares the node state: its locks, wait queue and session numbering. */
	drv_i915_node_init(device);

	/* Makes the device state PCI's to hand back at detach. */
	error = drv_pci_device_set_driver_data(pci, device);
	if (error != 0) {
		kern_free(device);
		return error;
	}

	kern_logf("i915: device 8086:%04x rev %02x attached\n",
	    (unsigned)device->product,
	    (unsigned)device->revision);

	/* Defers the bring-up to a context where it may sleep and wait. */
	drv_i915_device_schedule_start(device);

	/* Succeeded: the device is registered and starts once the kernel is ready. */
	return 0;
}

/* Stops the device and frees its state. */
static int
i915_detach(
	struct drv_pci_device *pci,
	unsigned flags)
{
	struct i915_device *device;
	int error;

	UNUSED_PARAMETER(flags);

	/* An attach that already cleaned up has nothing left to detach. */
	device = drv_pci_device_driver_data(pci);
	if (device == NULL)
		return 0;

	/* Withdraws a pending start and gives back every hardware lease the start took. */
	error = drv_i915_device_stop(device);
	if (error != 0)
		return error;

	/* Removes PCI's pointer before the state goes away. */
	error = drv_pci_device_set_driver_data(pci, NULL);
	if (error != 0)
		return error;

	kern_free(device);

	/* Succeeded: PCI may clear the driver binding. */
	return 0;
}
