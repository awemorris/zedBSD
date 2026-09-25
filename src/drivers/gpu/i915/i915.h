/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The root of the i915 driver: the device every part of the driver belongs to.
 *
 * Each part keeps its own state in its own header and reaches the device
 * through the pointer it was given; this header holds only what every part
 * shares.
 */

#ifndef DRIVERS_GPU_I915_I915_H
#define DRIVERS_GPU_I915_I915_H

#include <drivers/gpu/gpu.h>
#include <drivers/pci/pci.h>
#include <kern/lock.h>
#include <kern/waitq.h>
#include <stdint.h>

#include "memory.h"
#include "perf.h"
#include "request-queue.h"
#include "gt.h"

struct i915_display;
struct i915_ppgtt;
struct i915_render_device;
struct i915_worker;

/*
 * Marks a parameter a function receives by contract but does not use.
 *
 * The HAL defines the same macro for its own sources; drivers do not see
 * the HAL definitions, so the driver carries an identical one.
 */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/*
 * One Intel graphics device bound to this driver.
 *
 * PCI attach creates it and PCI detach frees it.  Between the two, the
 * device start runs once from the start worker; every later part hangs its
 * state from here.
 */
struct i915_device {
	/* The PCI device this driver instance owns. */
	struct drv_pci_device *pci;

	/* The PCI product and revision, read at attach. */
	uint16_t product;
	uint8_t revision;

	/* Serializes sessions and device-wide state changes. */
	struct mutex mutex;

	/*
	 * The next device waiting for the start worker.  The start registry
	 * owns this link; it is NULL once the device has been handed to a
	 * worker.
	 */
	struct i915_device *start_next;

	/*
	 * Nonzero once a start worker has been launched for this device, so a
	 * device is never started twice.
	 */
	unsigned start_launched;

	/* The name of the start step in progress, for the failure log. */
	const char *stage;

	/*
	 * The GPU node.
	 *
	 * drv_i915_node_init() prepares these fields, drv_i915_publish() fills
	 * the engine records and registers the node, and drv_i915_unpublish()
	 * withdraws it.  The mutex above serializes the session operations.
	 */

	/*
	 * Protects the engine records' queues and slots, the request worker's
	 * queues, and the counters sessions share with the worker.
	 */
	struct spinlock irq_lock;

	/* Where drain waits for the pending requests of a session to retire. */
	struct wait_queue retire_waitq;

	/* Every live session object; protected by the mutex. */
	struct i915_gem_registry gem;

	/*
	 * The address spaces of sessions that closed while quarantined, kept
	 * until a checked reset proves the GPU no longer names them.
	 */
	struct i915_ppgtt *quarantined_vms;

	/*
	 * The identifier the next session is given.  It starts at 1, so a
	 * logged session 0 is always a bug, and is never reused; 0 after a wrap
	 * refuses further opens.
	 */
	uint32_t next_session;

	/* The engine records the sessions queue requests on. */
	struct i915_engine engines[I915_ENGINE_COUNT];

	/* The node's operation table, borrowed by the GPU core while the node is registered. */
	struct drv_gpu_ops gpu_ops;

	/* The registered node; NULL while the node is not published. */
	struct drv_gpu_device *gpu;

	/*
	 * The Vulkan executor.  drv_i915_publish() attaches it before the node
	 * is registered and drv_i915_unpublish() detaches it after the node is
	 * withdrawn, so every session opens and closes within its lifetime;
	 * NULL while the node is not published.
	 */
	struct i915_render_device *vk;

	/*
	 * Nonzero after a device-wide fault: new sessions, submissions and
	 * reservations are refused until a checked reset clears it.
	 */
	unsigned failed;

	/* The request worker that runs the node's work on the hardware; NULL until created. */
	struct i915_worker *worker;

	/* The hardware state the device start builds and the device stop takes apart. */
	struct i915_gt gt;

	/*
	 * The display: the panel, its modeset and present path, and the
	 * display half of the interrupts.  The device start creates it and the
	 * device stop frees it; NULL for a device started without a display.
	 */
	struct i915_display *display;

	/*
	 * The frame timing totals the executor, the present path and the
	 * worker add to; prepared by drv_i915_node_init() and logged every few
	 * seconds while frames flow.
	 */
	struct i915_perf perf;

	/*
	 * Nonzero once the start worker has returned, so the device stop knows
	 * no start is still using the device.  Protected by the start registry lock.
	 */
	unsigned start_returned;
};

void drv_i915_node_init(struct i915_device *device);
int drv_i915_publish(struct i915_device *device);
int drv_i915_unpublish(struct i915_device *device);

#endif
