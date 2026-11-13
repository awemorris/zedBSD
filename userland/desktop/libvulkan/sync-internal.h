/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shares completion payloads between native queue operations and WSI acquisition.
 */

#ifndef VULKAN_SYNC_INTERNAL_H
#define VULKAN_SYNC_INTERNAL_H

#include "internal.h"

struct vulkan_external_fence;
struct vulkan_display;

/* What signals a fence registered for a display event (VK_EXT_display_control). */
enum vulkan_sync_event {
	VULKAN_SYNC_EVENT_NONE = 0,
	VULKAN_SYNC_EVENT_HOTPLUG,
	VULKAN_SYNC_EVENT_REFRESH,
	/* Signaled once and reset since: it observes nothing again. */
	VULKAN_SYNC_EVENT_SPENT
};

/*
 * One kernel registration of a hotplug fence: the display node it lives on
 * (its native identity) and the fence descriptor the kernel returned.  The
 * kernel signals the descriptor when the node's display topology changes,
 * and with an error when the node is withdrawn or fails.
 */
struct vulkan_sync_event_fence {
	uint64_t node;
	int fd;
};

/* A device mutex protects the software payload of one ordinary sync object. */
struct vulkan_sync {
	struct vulkan_object object;
	VkBool32 software_signaled;
	uint64_t notification;
	VkExternalFenceHandleTypeFlags export_types;
	struct vulkan_external_fence *external;
	/*
	 * True from the creation of an unsignaled fence until its first
	 * submission is prepared: the native fence was never signaled, so the
	 * preparation needs no native reset.  A shared (exported) fence is also
	 * true again after vkResetFences reset its native fence.
	 */
	VkBool32 native_unsubmitted;
	/*
	 * A fence registered for a display event is signaled by this library
	 * when its observation (vulkan_display_event_poll) sees the event.  A
	 * HOTPLUG fence sees it when one of its kernel registrations
	 * (event_fences) is signaled.  A REFRESH fence sees a refresh boundary
	 * of event_display after the cursor of generation event_generation;
	 * event_based is false until a cursor was taken.  The device mutex
	 * protects event and the REFRESH fields.
	 */
	enum vulkan_sync_event event;
	VkBool32 event_based;
	struct vulkan_display *event_display;
	uint64_t event_cursor;
	uint64_t event_generation;
	/*
	 * The kernel registrations of a HOTPLUG fence, one for each display
	 * node of the renderer that reports topology events.  They are filled
	 * before the fence is handed to the application and stay until the
	 * fence is destroyed, so a wait may read them without the device mutex;
	 * a reset withdraws them from the kernel but keeps the descriptors open
	 * while a concurrent wait may still poll them.
	 */
	struct vulkan_sync_event_fence *event_fences;
	uint32_t event_fence_count;
};

/* These helpers never acquire a device mutex; their caller already owns it. */
struct vulkan_sync *vulkan_sync_object(uint64_t handle);
VkResult vulkan_sync_device_status_locked(struct VkDevice_T *device);
VkResult vulkan_sync_status_locked(struct VkDevice_T *device, struct vulkan_sync *sync, uint32_t opcode);
void vulkan_sync_device_error(struct VkDevice_T *device, VkResult status);
VkResult vulkan_sync_capacity_query(struct VkQueue_T *queue, uint64_t *sequence);
VkResult vulkan_sync_capacity_wait(struct VkQueue_T *queue, uint64_t sequence);
VkResult vulkan_sync_job_reserve(struct VkQueue_T *queue, struct vulkan_sync *sync, struct vulkan_notification **reserved, uint64_t *capacity_sequence, VkBool32 *prepared);
VkResult vulkan_sync_job_finish(struct VkQueue_T *queue, struct vulkan_sync *sync, struct vulkan_notification *reserved, VkResult native_status, VkBool32 accepted);
VkResult vulkan_sync_job_status(struct vulkan_sync *sync, uint64_t timeout_ns);
VkResult vulkan_sync_ledger_reap(struct vulkan_context *context);
VkResult vulkan_sync_job_retired(struct vulkan_sync *sync);
void vulkan_sync_quiesce(struct vulkan_sync *sync);

/* Display events; the caller owns the fence's device mutex, except for the final release. */
VkBool32 vulkan_display_event_poll(struct VkDevice_T *device, struct vulkan_sync *sync);
void vulkan_display_event_reset_locked(struct VkDevice_T *device, struct vulkan_sync *sync);
void vulkan_display_event_finish(struct vulkan_sync *sync);

/* Shared reference payloads retain native completion proof and caller-owned synchronization. */
VkResult vulkan_external_fence_create(struct VkDevice_T *device, struct vulkan_sync *sync, const VkFenceCreateInfo *create);
VkResult vulkan_external_fence_status_locked(struct VkDevice_T *device, struct vulkan_sync *sync);
VkResult vulkan_external_fence_wait(struct VkDevice_T *device, struct vulkan_sync *sync, uint64_t timeout_ns);
VkResult vulkan_external_fence_reset_locked(struct VkDevice_T *device, struct vulkan_sync *sync);
VkResult vulkan_external_fence_prepare_locked(struct VkDevice_T *device, struct vulkan_sync *sync, int *fd, uint64_t *generation, VkBool32 prepare_native);
VkResult vulkan_external_fence_acquire_locked(struct VkDevice_T *device, struct vulkan_sync *sync);
int vulkan_external_fence_descriptor(struct vulkan_sync *sync);
void vulkan_external_fence_finish(struct vulkan_sync *sync);

/* Clock sampling and cooperative pauses are performed without Vulkan locks. */
VkResult vulkan_sync_clock(uint64_t *nanoseconds);
VkResult vulkan_sync_pause(uint64_t nanoseconds);

#endif
