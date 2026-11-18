/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The camera framework: the /dev/camN nodes and the interface between the
 * framework and the source drivers that deliver frames to it.
 *
 * The framework owns the nodes, the buffers, the queues, the stream state
 * and the decision to stop a stream.  A source owns its hardware and
 * fills the slots it takes from the framework.  The framework never looks
 * at the pixels.
 */

#ifndef KERN_DRIVERS_CAMERA_CAM_H
#define KERN_DRIVERS_CAMERA_CAM_H

#include <stddef.h>
#include <stdint.h>
#include <uapi/camera.h>

struct drv_cam_device;
struct drv_cam_slot;
struct drv_dma_device;

/* The source writes the frames with the CPU into cached kernel memory. */
#define DRV_CAM_SOURCE_CPU_WRITES	0x00000001U

/* The source's hardware writes the frames by DMA (not supported yet). */
#define DRV_CAM_SOURCE_DMA		0x00000002U

/* The reasons a source gives for a frame it could not deliver. */
#define CAM_DROP_NO_BUFFER		1U
#define CAM_DROP_OVERFLOW		2U
#define CAM_DROP_ERROR			3U
#define CAM_DROP_TRANSPORT		4U

/*
 * A format and frame size a stream is asked to use.
 *
 * interval_100ns 0 asks for the mode's default interval.
 */
struct drv_cam_mode_request {
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t interval_100ns;
};

/*
 * The source's answer to a mode request.
 *
 * image_bytes is the largest frame and becomes the size of every buffer.
 * payload_bytes is what the device needs the transport to carry in one
 * service interval, to be compared with the host's limit.
 */
struct drv_cam_mode_result {
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t interval_100ns;
	uint32_t stride_bytes;
	uint32_t image_bytes;
	uint32_t payload_bytes;
	uint32_t bayer_order;
	uint32_t sample_shift;
	uint32_t mode_index;
};

/*
 * What a source does for the framework.
 *
 * Every operation runs in process context under the device's mutex and
 * may sleep (a USB control transfer, an alternate setting).  stop is
 * called only by the framework, at most once per start, and must be
 * harmless when called again; it discards the active slot before it
 * returns.  The control operations may be NULL, which makes every
 * control unsupported.
 */
struct drv_cam_source_ops {
	int (*enum_format)(void *argument, unsigned index, struct cam_format_desc *desc);
	int (*enum_mode)(void *argument, uint32_t format, unsigned index, struct cam_mode_desc *desc);
	int (*enum_interval)(void *argument, uint32_t format, unsigned mode, unsigned index, uint32_t *interval_100ns);
	int (*try_mode)(void *argument, const struct drv_cam_mode_request *request, struct drv_cam_mode_result *result);
	int (*set_mode)(void *argument, const struct drv_cam_mode_request *request, struct drv_cam_mode_result *result);
	int (*start)(void *argument);
	void (*stop)(void *argument);
	int (*control_info)(void *argument, uint32_t id, struct cam_control_info *info);
	int (*get_control)(void *argument, uint32_t id, int64_t *value);
	int (*set_control)(void *argument, uint32_t id, int64_t value);
	int (*read_counters)(void *argument, struct cam_counters *counters);
	uint32_t (*host_payload_max)(void *argument);
};

/*
 * The description of one camera a source registers.
 *
 * The strings are copied; source and source_argument stay borrowed until
 * drv_cam_unregister() returns.
 */
struct drv_cam_device_desc {
	const char *driver_name;
	char device_name[64];
	char location[32];
	const struct drv_cam_source_ops *source;
	void *source_argument;
	uint32_t flags;
	uint32_t capabilities;
	uint32_t max_buffers;
	uint32_t buffer_alignment;
	uint32_t clock_hz;
	struct drv_dma_device *dma;
	uint64_t dma_bus_offset;
	uint64_t dma_physical_limit;
};

/*
 * The time and flags of one finished frame.
 *
 * timestamp_ns 0 lets the framework take the time of drv_cam_slot_done().
 */
struct drv_cam_frame_info {
	uint64_t timestamp_ns;
	uint32_t flags;
	uint32_t device_pts;
	uint64_t device_scr;
};

/*
 * Registers one camera and publishes its node.
 */
int drv_cam_register(const struct drv_cam_device_desc *desc, struct drv_cam_device **result);

/*
 * Retires one camera.
 *
 * A running stream is stopped once through the source, the node refuses
 * every new request with ENODEV, waiters are woken with ENODEV, and the
 * call waits until no request is in progress.  After it returns the
 * framework never calls the source again.
 */
void drv_cam_unregister(struct drv_cam_device *device);

/*
 * Begins a new frame and reports its sequence number.
 *
 * A source calls it once per frame it sees begin, whether or not a slot
 * is free, so a gap in the sequence counts the frames lost.
 */
uint32_t drv_cam_frame_begin(struct drv_cam_device *device);

/*
 * Takes the first free slot for a frame; EAGAIN when none is free or the
 * stream is not running.  Never sleeps.
 */
int drv_cam_slot_take(struct drv_cam_device *device, uint32_t sequence, struct drv_cam_slot **slot);

/*
 * Reports the kernel address a CPU-writing source fills: cached for the
 * framework's own buffers, an uncached view for imported GPU memory.
 */
void *drv_cam_slot_address(const struct drv_cam_slot *slot);

/* Reports the bus address a DMA source gives its hardware. */
uint64_t drv_cam_slot_bus_address(const struct drv_cam_slot *slot);

/* Reports how many bytes a slot holds. */
size_t drv_cam_slot_capacity(const struct drv_cam_slot *slot);

/* Completes an active slot and wakes its readers.  Never sleeps. */
void drv_cam_slot_done(struct drv_cam_device *device, struct drv_cam_slot *slot, size_t used_bytes, const struct drv_cam_frame_info *info);

/* Returns an active slot unfilled to the head of the free queue.  Never sleeps. */
void drv_cam_slot_discard(struct drv_cam_device *device, struct drv_cam_slot *slot);

/* Counts a frame the source could not deliver. */
void drv_cam_frame_dropped(struct drv_cam_device *device, uint32_t reason);

/*
 * Tells that the source cannot go on.
 *
 * The stream enters FAULT: poll reports POLLERR and a dequeue EIO until
 * the owner stops the stream, which is when the framework calls stop.
 */
void drv_cam_source_fault(struct drv_cam_device *device, int error);

/* Tells pollers that a source counter worth looking at has changed. */
void drv_cam_source_counters_changed(struct drv_cam_device *device);

#endif
