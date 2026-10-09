/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Private renderer descriptors and caller-serialized native allocation lookup. */
#ifndef KERN_DRIVERS_GPU_BCM2711_RENDER_DEVICE_H
#define KERN_DRIVERS_GPU_BCM2711_RENDER_DEVICE_H

#include <drivers/gpu/gpu.h>
#include "drivers/gpu/bcm2711/v3d-memory.h"
#include "drivers/gpu/bcm2711/render-worker.h"

struct bcm2711_render_device;
struct bcm2711_vulkan_session;
struct bcm2711_vulkan_native_job;

/* A retained session owns its descriptor list until the common core retires all pins. */
struct bcm2711_render_session {
	/* The controller retains a closed descriptor while uncertain prepared Vulkan owners still borrow it. */
	struct bcm2711_render_session *closed_next;
	struct bcm2711_render_device *device;
	struct bcm2711_render_resource *resources;

	/* Protocol state stays private until the complete native executor is bound. */
	struct bcm2711_vulkan_session *vulkan;
	uint32_t count;
	uint32_t next_identifier;

	/* The native IRQ guard keeps the descriptor alive until every accepted callback ends. */
	uint32_t pending;

	/* The native IRQ guard protects the Vulkan queues that own nonzero completion domains. */
	uint64_t timelines;

	/* The native IRQ guard protects namespace stop publication against ioctl admission. */
	bool stopping;
};

/* One descriptor owns a native VA view; jobs hold independent mapped references. */
struct bcm2711_render_resource {
	struct bcm2711_render_resource *next;
	struct bcm2711_render_session *owner;
	struct bcm2711_v3d_view *view;
	uint32_t identifier;
	bool blob;
	bool mappable;
	bool shareable;
};

/* The controller mutex serializes resources, translations and the single native worker. */
struct bcm2711_render_device {
	struct mutex mutex;
	struct bcm2711_v3d_space space;
	struct bcm2711_render_worker worker;
	struct drv_gpu_device *gpu;
	struct drv_gpu_ops operations;
	struct drv_gpu_share_ops share_operations;
	struct drv_gpu_scanout_ops scanout_operations;
	struct drv_gpu_recovery_ops recovery_operations;
	/* Live Vulkan allocation declarations reserve this aggregate logical heap budget. */
	uint64_t vulkan_memory_bytes;
	/* Callback disposal transfers uncertain whole payloads here without dropping their pending primary or session. */
	struct bcm2711_vulkan_native_job *quarantine;
	/* Internally held closed descriptors do not count as live external sessions or prevent checked reset. */
	struct bcm2711_render_session *closed;
	uint32_t sessions;
	bool registered;
};

int bcm2711_render_register(struct bcm2711_v3d *engine);
struct bcm2711_render_resource *bcm2711_render_find(struct bcm2711_render_session *session, uint32_t identifier);
void bcm2711_render_fail(struct bcm2711_render_device *controller, int error);

#endif
