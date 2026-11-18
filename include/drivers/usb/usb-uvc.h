/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The USB Video Class driver "uvc": a camera framework source for USB
 * cameras and capture devices.
 *
 * usb-uvc.c binds the VideoControl interface, claims the streaming
 * interface, reads the descriptors and registers /dev/camN.
 * usb-uvc-stream.c negotiates the mode with PROBE/COMMIT and runs the
 * stream: the transfers, the worker and the halt.  usb-uvc-payload.c puts
 * the frames together.  The structure below is shared by those files and
 * by no one else.
 */

#ifndef KERN_DRIVERS_USB_UVC_H
#define KERN_DRIVERS_USB_UVC_H

#include <stddef.h>
#include <stdint.h>

#include <drivers/camera/cam.h>
#include <drivers/usb/usb-uvc-desc.h>
#include <drivers/usb/usb-uvc-payload.h>
#include <drivers/usb/usb.h>
#include <kern/lock.h>
#include <kern/waitq.h>

struct thread;

/*
 * The transfers of a stream: four isochronous transfers of up to 32
 * packets (an interval's payload each, at most 64 KiB a transfer), or
 * one bulk transfer.
 */
#define UVC_ISOCH_URBS			4U
#define UVC_ISOCH_PACKETS		32U
#define UVC_ISOCH_TRANSFER_MAX		(64U * 1024U)
#define UVC_BULK_URBS			1U
#define UVC_URBS_MAX			UVC_ISOCH_URBS

/* The largest bulk transfer, the host's transfer reservation limit. */
#define UVC_BULK_TRANSFER_MAX		(64U * 1024U)

/*
 * One USB camera.
 *
 * mutex serializes the mode negotiation, the start, the halt and the
 * detach; the framework already holds its own device mutex around each
 * source operation, and the USB core's quiesce takes this one alone.
 *
 * lock guards the stream's handshake with the transfer callbacks and the
 * worker: stopping, done_mask, submitted_mask, submitting, faulted and
 * worker.  The callbacks only set a bit in done_mask and wake waitq.
 *
 * Everything else of the stream belongs to the worker while it runs and
 * to the holder of mutex otherwise.  retained counts the transfers a
 * halt could not get back from the host controller; they and their
 * buffers are kept until a later halt or the detach gets them back.
 */
struct usb_uvc {
	struct drv_usb_interface *control_interface;
	struct drv_usb_interface *stream_interface;
	struct drv_usb_device *device;
	enum uvc_speed speed;
	char tag[16];
	char product[64];
	char location[32];
	struct uvc_control_desc control;
	struct uvc_stream_desc stream;
	enum uvc_transport transport;
	struct drv_usb_endpoint *bulk_endpoint;
	uint16_t bulk_packet_bytes;
	struct uvc_alternate alternates[UVC_ALTERNATES_MAX];
	unsigned alternate_count;
	unsigned stream_claimed;
	struct drv_cam_device *camera;
	struct mutex mutex;

	/* The mode negotiated last: the committed record and what it selects. */
	unsigned committed;
	uint8_t commit_record[UVC_PROBE_MAX];
	size_t commit_length;
	const struct uvc_format *format;
	const struct uvc_frame *frame;
	uint32_t image_bytes;
	uint32_t payload_bytes;
	uint32_t interval_100ns;

	/* The stream's handshake with its transfers and worker, guarded by lock. */
	struct spinlock lock;
	struct wait_queue waitq;
	unsigned running;
	unsigned stopping;
	unsigned faulted;
	unsigned submitting;
	unsigned done_mask;
	unsigned submitted_mask;
	struct thread *worker;
	unsigned urb_count;
	unsigned packet_count;
	size_t packet_bytes;
	size_t urb_bytes;
	unsigned alternate_setting;
	struct drv_usb_endpoint *endpoint;
	struct drv_usb_urb *urbs[UVC_URBS_MAX];
	uint8_t *buffers[UVC_URBS_MAX];
	struct drv_usb_iso_packet packets[UVC_URBS_MAX][UVC_ISOCH_PACKETS];
	unsigned retained;
	unsigned next;
	uint64_t last_completion_ns;
	unsigned no_signal_reported;

	/* The frame assembler and the counters of what went wrong. */
	struct uvc_assembler assembler;
	uint64_t resubmit_failures;
	uint64_t halt_timeouts;
	uint64_t no_signal;
};

/*
 * Registers the driver with the USB core.
 */
int drv_usb_uvc_driver_register(void);

/*
 * Asks the device for a mode with PROBE, and with commit set also
 * commits it.  The answer fills result also when the host cannot carry
 * the mode, which is ENOSPC.
 */
int uvc_stream_negotiate(struct usb_uvc *uvc, const struct drv_cam_mode_request *request, struct drv_cam_mode_result *result, unsigned commit);

/*
 * Starts the committed stream: COMMIT again, the alternate, the
 * transfers and the worker.  ENOSPC when no alternate carries the
 * payload.
 */
int uvc_stream_start(struct usb_uvc *uvc);

/*
 * Stops the stream's transfers and worker and gives the bandwidth back.
 * Harmless when nothing runs.  Returns 0, or the error of a transfer the
 * host controller still holds.
 */
int uvc_stream_halt(struct usb_uvc *uvc);

/* Gives back the slot the stream was filling. */
void uvc_stream_abandon(struct usb_uvc *uvc);

/* Initializes the stream's lock, wait queue and state. */
void uvc_stream_init(struct usb_uvc *uvc);

#endif
