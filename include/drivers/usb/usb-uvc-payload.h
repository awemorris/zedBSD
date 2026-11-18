/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The uvc driver's frame assembler: it reads the payload headers of a
 * video stream and puts the payloads' data together into the camera
 * framework's slots.
 *
 * The assembler is driven by the stream's worker alone and holds no lock
 * of its own.  It calls only the framework's slot interface, so it can be
 * exercised on the host against a stand-in framework.
 */

#ifndef KERN_DRIVERS_USB_UVC_PAYLOAD_H
#define KERN_DRIVERS_USB_UVC_PAYLOAD_H

#include <stddef.h>
#include <stdint.h>

struct drv_cam_device;
struct drv_cam_slot;

/*
 * The bits of a payload header's bmHeaderInfo.
 *
 * Value(s) referenced: USB Video Class 1.1 §2.4.3.3 (Video and Still
 * Image Payload Headers).
 */
#define UVC_PAYLOAD_FID			0x01U
#define UVC_PAYLOAD_EOF			0x02U
#define UVC_PAYLOAD_PTS			0x04U
#define UVC_PAYLOAD_SCR			0x08U
#define UVC_PAYLOAD_RES			0x10U
#define UVC_PAYLOAD_STI			0x20U
#define UVC_PAYLOAD_ERR			0x40U
#define UVC_PAYLOAD_EOH			0x80U

/* Why an isochronous packet carried nothing. */
enum uvc_packet_loss {
	UVC_PACKET_MISSED = 1,
	UVC_PACKET_ERROR = 2
};

/*
 * The state of one stream's frame assembly.
 *
 * last_fid is -1 until the first payload is seen.  current is the slot
 * being filled, or NULL while the frame's data is thrown away; offset is
 * how much of it is filled.  now_ns is the time of the payload being
 * read.  The bulk fields hold the header of a payload that spans
 * transfers, and whether its data is thrown away.  The counters only
 * grow and are read by the driver's counters operation.
 */
struct uvc_assembler {
	struct drv_cam_device *camera;
	uint32_t cam_format;
	unsigned compressed;
	size_t image_bytes;
	unsigned no_fid;
	int last_fid;
	struct drv_cam_slot *current;
	uint8_t *address;
	size_t capacity;
	size_t offset;
	uint32_t sequence;
	unsigned error;
	unsigned has_pts;
	unsigned has_scr;
	uint32_t pts;
	uint64_t scr;
	uint64_t frame_time_ns;
	uint64_t last_done_ns;
	uint64_t now_ns;
	unsigned bulk_in_payload;
	unsigned bulk_discard;
	uint8_t bulk_info;
	size_t bulk_received;
	uint64_t invalid_header;
	uint64_t short_header;
	uint64_t missed_packets;
	uint64_t packet_errors;
	uint64_t frames_done;
};

/*
 * Starts assembling a stream of a format whose frames are at most
 * image_bytes.  no_fid marks a device that does not toggle FID.
 */
void uvc_assembler_start(struct uvc_assembler *assembler, struct drv_cam_device *camera, uint32_t cam_format, size_t image_bytes, unsigned no_fid);

/* Takes one isochronous payload (one packet's bytes) seen at now_ns. */
void uvc_assembler_payload(struct uvc_assembler *assembler, const uint8_t *payload, size_t length, uint64_t now_ns);

/* Takes one isochronous packet that carried nothing. */
void uvc_assembler_packet_lost(struct uvc_assembler *assembler, enum uvc_packet_loss loss);

/*
 * Takes one finished bulk transfer of a stream whose payloads are
 * payload_bytes long.  requested is the transfer's size; a shorter
 * transfer ends the payload.
 */
void uvc_assembler_bulk(struct uvc_assembler *assembler, const uint8_t *data, size_t length, size_t requested, uint32_t payload_bytes, uint64_t now_ns);

/* Gives back the slot being filled, unfinished, and forgets the frame. */
void uvc_assembler_abandon(struct uvc_assembler *assembler);

#endif
