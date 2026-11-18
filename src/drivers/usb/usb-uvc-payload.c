/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The uvc driver's frame assembler.
 *
 * Every payload starts with a header: its length, then bmHeaderInfo with
 * the frame ID (FID), end of frame (EOF), presentation time (PTS) and
 * source clock (SCR) flags and the error bit (ERR).  A change of FID
 * starts a new frame and ends the one before, whether or not it saw EOF;
 * EOF ends a frame early.  Each new frame advances the framework's
 * sequence once, with or without a slot to fill, so a gap in the
 * delivered sequence numbers is the count of the frames lost.
 *
 * A payload with the same FID while no slot is being filled belongs to a
 * frame already ended or thrown away, and its data is dropped.  A device
 * that never toggles FID is followed by its EOF instead.  A frame that
 * would overflow its slot is ended with what fits, marked as an error.
 *
 * An uncompressed frame shorter than the mode's size is marked
 * incomplete; its missing lines still hold an older frame.  An MJPEG
 * frame without SOI at its start is an error, and one without EOI in its
 * last 64 bytes (some devices pad after EOI) is incomplete.
 *
 * Value(s) referenced: USB Video Class 1.1 §2.4.3.3 (payload header:
 * bHeaderLength, bmHeaderInfo, dwPresentationTime at 2, scrSourceClock
 * after it, 32 bits of STC and 11 bits of the SOF counter); ITU-T T.81
 * Annex B.1.1.3 (the SOI marker FF D8 and the EOI marker FF D9).
 */

#include <drivers/camera/cam.h>
#include <drivers/usb/usb-uvc-payload.h>

#include <kern/kcrt.h>
#include <uapi/errno.h>

/* The shortest payload header: its length and bmHeaderInfo. */
#define UVC_HEADER_MIN			2U

/* The lengths of the header's optional fields. */
#define UVC_HEADER_PTS_BYTES		4U
#define UVC_HEADER_SCR_BYTES		6U

/* How far from an MJPEG frame's end its EOI may be. */
#define UVC_JPEG_TAIL			64U

/* The JPEG markers that start and end a frame. */
#define UVC_JPEG_MARKER			0xffU
#define UVC_JPEG_SOI			0xd8U
#define UVC_JPEG_EOI			0xd9U

static unsigned uvc_assembler_header(struct uvc_assembler *assembler, const uint8_t *payload, size_t header_length);
static void uvc_assembler_begin(struct uvc_assembler *assembler);
static void uvc_assembler_data(struct uvc_assembler *assembler, const uint8_t *data, size_t length);
static void uvc_assembler_end(struct uvc_assembler *assembler, uint8_t info);
static void uvc_assembler_finish(struct uvc_assembler *assembler);
static uint32_t uvc_assembler_jpeg_flags(const struct uvc_assembler *assembler);
static unsigned uvc_assembler_header_valid(struct uvc_assembler *assembler, const uint8_t *payload, size_t length);

/*
 * Starts assembling a stream.
 *
 * The counters are kept across streams; everything else starts afresh.
 */
void
uvc_assembler_start(
	struct uvc_assembler *assembler,
	struct drv_cam_device *camera,
	uint32_t cam_format,
	size_t image_bytes,
	unsigned no_fid)
{
	/* The stream's format and the device's habit. */
	assembler->camera = camera;
	assembler->cam_format = cam_format;
	assembler->compressed = 0U;
	if (cam_format == CAM_FORMAT_MJPEG)
		assembler->compressed = 1U;
	assembler->image_bytes = image_bytes;
	assembler->no_fid = no_fid;

	/* No payload seen and no slot taken. */
	assembler->last_fid = -1;
	assembler->current = NULL;
	assembler->address = NULL;
	assembler->capacity = 0U;
	assembler->offset = 0U;
	assembler->error = 0U;
	assembler->has_pts = 0U;
	assembler->has_scr = 0U;
	assembler->bulk_in_payload = 0U;
	assembler->bulk_discard = 0U;
	assembler->bulk_received = 0U;
}

/*
 * Takes one isochronous payload: its header decides the frame, its data
 * goes into the slot, and its EOF may end the frame.
 */
void
uvc_assembler_payload(
	struct uvc_assembler *assembler,
	const uint8_t *payload,
	size_t length,
	uint64_t now_ns)
{
	unsigned keep;
	unsigned valid;
	size_t header_length;

	/* A payload with a broken header is dropped and spoils the frame. */
	assembler->now_ns = now_ns;
	valid = uvc_assembler_header_valid(assembler, payload, length);
	if (valid == 0U)
		return;

	/* The header: a new frame, or data that belongs to none. */
	header_length = payload[0];
	keep = uvc_assembler_header(assembler, payload, header_length);
	if (keep == 0U)
		return;

	/* The data, then the end of the frame when EOF says so. */
	uvc_assembler_data(assembler, payload + header_length, length - header_length);
	uvc_assembler_end(assembler, payload[1]);
}

/*
 * Takes one isochronous packet that carried nothing: counted, and the
 * frame being filled is missing data.
 */
void
uvc_assembler_packet_lost(
	struct uvc_assembler *assembler,
	enum uvc_packet_loss loss)
{
	/* The kind of loss is counted. */
	if (loss == UVC_PACKET_MISSED) {
		assembler->missed_packets++;
	} else {
		assembler->packet_errors++;
	}

	/* The frame being filled has a hole. */
	if (assembler->current != NULL)
		assembler->error = 1U;
}

/*
 * Takes one finished bulk transfer.
 *
 * A payload may span transfers.  Its first transfer carries the header,
 * which is kept; the following ones carry only data.  The payload ends
 * with a short transfer or when payload_bytes have come, and its saved
 * header's EOF is read then.  An empty transfer outside a payload is
 * ignored.
 */
void
uvc_assembler_bulk(
	struct uvc_assembler *assembler,
	const uint8_t *data,
	size_t length,
	size_t requested,
	uint32_t payload_bytes,
	uint64_t now_ns)
{
	unsigned valid;
	unsigned keep;
	size_t header_length;

	/* The first transfer of a payload: its header. */
	assembler->now_ns = now_ns;
	if (assembler->bulk_in_payload == 0U) {
		/* An empty transfer between payloads carries nothing. */
		if (length == 0U)
			return;

		/* A broken header drops the transfer. */
		valid = uvc_assembler_header_valid(assembler, data, length);
		if (valid == 0U)
			return;

		/* The header is kept for the payload's end. */
		header_length = data[0];
		keep = uvc_assembler_header(assembler, data, header_length);
		assembler->bulk_in_payload = 1U;
		assembler->bulk_info = data[1];
		assembler->bulk_received = length;
		assembler->bulk_discard = 0U;
		if (keep == 0U)
			assembler->bulk_discard = 1U;

		/* The data after the header. */
		if (keep != 0U)
			uvc_assembler_data(assembler, data + header_length, length - header_length);
	} else {
		/* A following transfer is data only. */
		assembler->bulk_received += length;
		if (assembler->bulk_discard == 0U)
			uvc_assembler_data(assembler, data, length);
	}

	/* The payload goes on while the transfers are full and more is due. */
	if (length >= requested &&
	    (payload_bytes == 0U ||
	     assembler->bulk_received < payload_bytes))
		return;

	/* The payload's end: its header's EOF, unless its data was thrown away. */
	assembler->bulk_in_payload = 0U;
	if (assembler->bulk_discard == 0U)
		uvc_assembler_end(assembler, assembler->bulk_info);
	assembler->bulk_discard = 0U;
}

/*
 * Gives back the slot being filled, unfinished, and forgets the frame.
 * The next payload is the first again.
 */
void
uvc_assembler_abandon(
	struct uvc_assembler *assembler)
{
	/* The slot goes back to the head of the framework's free queue. */
	if (assembler->current != NULL)
		drv_cam_slot_discard(assembler->camera, assembler->current);

	/* No frame is being filled. */
	assembler->current = NULL;
	assembler->address = NULL;
	assembler->capacity = 0U;
	assembler->offset = 0U;
	assembler->error = 0U;
	assembler->last_fid = -1;
	assembler->bulk_in_payload = 0U;
	assembler->bulk_discard = 0U;
}

/*
 * Checks a payload's header length against the payload.  A broken one
 * is counted and spoils the frame being filled.  Reports 1 for a usable
 * header.
 */
static unsigned
uvc_assembler_header_valid(
	struct uvc_assembler *assembler,
	const uint8_t *payload,
	size_t length)
{
	size_t header_length;

	/* Too short to hold a header. */
	if (length < UVC_HEADER_MIN) {
		assembler->invalid_header++;
		if (assembler->current != NULL)
			assembler->error = 1U;
		return 0U;
	}

	/* A header length shorter than the minimum or past the payload. */
	header_length = payload[0];
	if (header_length < UVC_HEADER_MIN || header_length > length) {
		assembler->invalid_header++;
		if (assembler->current != NULL)
			assembler->error = 1U;
		return 0U;
	}

	/* Succeeded: the header can be read. */
	return 1U;
}

/*
 * Reads a payload's header: a FID change begins a new frame, ERR spoils
 * the frame, and the first PTS and SCR of the frame are kept.  Reports
 * whether the payload's data belongs to the frame being filled.
 */
static unsigned
uvc_assembler_header(
	struct uvc_assembler *assembler,
	const uint8_t *payload,
	size_t header_length)
{
	size_t needed;
	size_t place;
	uint8_t info;
	int fid;

	/* The fields the flags announce must fit the header; if not, they are not read. */
	info = payload[1];
	needed = UVC_HEADER_MIN;
	if ((info & UVC_PAYLOAD_PTS) != 0U)
		needed += UVC_HEADER_PTS_BYTES;
	if ((info & UVC_PAYLOAD_SCR) != 0U)
		needed += UVC_HEADER_SCR_BYTES;
	if (header_length < needed) {
		assembler->short_header++;
		info &= (uint8_t)~(UVC_PAYLOAD_PTS | UVC_PAYLOAD_SCR);
	}

	/* A FID other than the last one begins a new frame. */
	fid = (int)(info & UVC_PAYLOAD_FID);
	if (fid != assembler->last_fid) {
		uvc_assembler_begin(assembler);
		assembler->last_fid = fid;
	}

	/*
	 * The same FID with no slot: the rest of a frame that ended or is
	 * thrown away.  A device that does not toggle FID is followed by its
	 * EOF, so the next payload begins a frame.
	 */
	if (assembler->current == NULL) {
		if (assembler->no_fid != 0U && (info & UVC_PAYLOAD_EOF) != 0U)
			assembler->last_fid ^= 1;
		return 0U;
	}

	/* ERR keeps the data but marks the frame. */
	if ((info & UVC_PAYLOAD_ERR) != 0U)
		assembler->error = 1U;

	/* The frame's first PTS. */
	place = UVC_HEADER_MIN;
	if ((info & UVC_PAYLOAD_PTS) != 0U) {
		if (assembler->has_pts == 0U) {
			assembler->pts = (uint32_t)payload[place];
			assembler->pts |= (uint32_t)payload[place + 1U] << 8;
			assembler->pts |= (uint32_t)payload[place + 2U] << 16;
			assembler->pts |= (uint32_t)payload[place + 3U] << 24;
			assembler->has_pts = 1U;
		}

		/* Moves past the PTS field to where the SCR would start. */
		place += UVC_HEADER_PTS_BYTES;
	}

	/* The frame's first SCR: 32 bits of the source clock, then the 11-bit SOF counter. */
	if ((info & UVC_PAYLOAD_SCR) != 0U && assembler->has_scr == 0U) {
		assembler->scr = (uint64_t)payload[place];
		assembler->scr |= (uint64_t)payload[place + 1U] << 8;
		assembler->scr |= (uint64_t)payload[place + 2U] << 16;
		assembler->scr |= (uint64_t)payload[place + 3U] << 24;
		assembler->scr |= (uint64_t)payload[place + 4U] << 32;
		assembler->scr |= (uint64_t)(payload[place + 5U] & 0x07U) << 40;
		assembler->has_scr = 1U;
	}

	/* Succeeded: the data is the frame's. */
	return 1U;
}

/*
 * Begins a new frame: ends the one being filled, advances the sequence
 * and takes a slot.
 *
 * A slot that got no data is given back first and taken again, so the
 * new frame carries its own sequence number and an empty frame is never
 * delivered.
 */
static void
uvc_assembler_begin(
	struct uvc_assembler *assembler)
{
	struct drv_cam_slot *slot;
	int error;

	/* The frame being filled ends normally; one without data is given back. */
	if (assembler->current != NULL && assembler->offset > 0U) {
		uvc_assembler_finish(assembler);
	} else if (assembler->current != NULL) {
		drv_cam_slot_discard(assembler->camera, assembler->current);
		assembler->current = NULL;
	}

	/* The new frame's sequence, whether or not a slot is free. */
	assembler->sequence = drv_cam_frame_begin(assembler->camera);
	assembler->frame_time_ns = assembler->now_ns;
	assembler->error = 0U;
	assembler->has_pts = 0U;
	assembler->has_scr = 0U;
	assembler->offset = 0U;

	/* A slot for it; without one the frame is dropped once and its data thrown away. */
	slot = NULL;
	error = drv_cam_slot_take(assembler->camera, assembler->sequence, &slot);
	if (error != 0) {
		drv_cam_frame_dropped(assembler->camera, CAM_DROP_NO_BUFFER);
		assembler->current = NULL;
		assembler->address = NULL;
		assembler->capacity = 0U;
		return;
	}

	/* The slot's memory. */
	assembler->current = slot;
	assembler->address = drv_cam_slot_address(slot);
	assembler->capacity = drv_cam_slot_capacity(slot);
}

/*
 * Copies a payload's data into the slot.  Data that would overflow the
 * slot ends the frame with what fits, as an error.
 */
static void
uvc_assembler_data(
	struct uvc_assembler *assembler,
	const uint8_t *data,
	size_t length)
{
	size_t room;

	/* No slot, or nothing to copy. */
	if (assembler->current == NULL || length == 0U)
		return;

	/* What fits, and an overflow ends the frame. */
	room = assembler->capacity - assembler->offset;
	if (length > room) {
		kern_memcpy(assembler->address + assembler->offset, data, room);
		assembler->offset += room;
		assembler->error = 1U;
		drv_cam_frame_dropped(assembler->camera, CAM_DROP_OVERFLOW);
		uvc_assembler_finish(assembler);
		return;
	}

	/* The data at the end of what is filled. */
	kern_memcpy(assembler->address + assembler->offset, data, length);
	assembler->offset += length;
}

/*
 * Reads a payload's EOF: a frame with data ends.  A device that does
 * not toggle FID has its next payload taken as a new frame.
 */
static void
uvc_assembler_end(
	struct uvc_assembler *assembler,
	uint8_t info)
{
	/* No EOF, nothing ends. */
	if ((info & UVC_PAYLOAD_EOF) == 0U)
		return;

	/* A frame with data ends here. */
	if (assembler->current != NULL && assembler->offset > 0U)
		uvc_assembler_finish(assembler);

	/* A device without FID toggling is followed by its EOF. */
	if (assembler->no_fid != 0U)
		assembler->last_fid ^= 1;
}

/*
 * Hands the frame being filled to the framework with its flags, time,
 * PTS and SCR.
 */
static void
uvc_assembler_finish(
	struct uvc_assembler *assembler)
{
	struct drv_cam_frame_info info;

	/* The frame's time and flags; the time is that of its first payload's transfer. */
	kern_memset(&info, 0, sizeof(info));
	info.timestamp_ns = assembler->frame_time_ns;
	info.flags = CAM_METADATA_APPROXIMATE;
	if (assembler->error != 0U)
		info.flags |= CAM_FRAME_ERROR;

	/* An uncompressed frame shorter than the mode is incomplete; an MJPEG one is checked for its markers. */
	if (assembler->compressed == 0U) {
		if (assembler->offset < assembler->image_bytes)
			info.flags |= CAM_FRAME_INCOMPLETE;
	} else {
		info.flags |= uvc_assembler_jpeg_flags(assembler);
	}

	/* The device's times, when the frame carried them. */
	if (assembler->has_pts != 0U) {
		info.flags |= CAM_METADATA_PTS;
		info.device_pts = assembler->pts;
	}

	/* The source clock. */
	if (assembler->has_scr != 0U) {
		info.flags |= CAM_METADATA_SCR;
		info.device_scr = assembler->scr;
	}

	/* The framework takes the frame. */
	drv_cam_slot_done(assembler->camera, assembler->current, assembler->offset, &info);
	assembler->frames_done++;
	assembler->last_done_ns = assembler->now_ns;

	/* No frame is being filled. */
	assembler->current = NULL;
	assembler->address = NULL;
	assembler->capacity = 0U;
	assembler->offset = 0U;
	assembler->error = 0U;
	assembler->has_pts = 0U;
	assembler->has_scr = 0U;
}

/*
 * Reports the flags an MJPEG frame's markers call for: an error without
 * SOI at the start, incomplete without EOI in the last 64 bytes.
 */
static uint32_t
uvc_assembler_jpeg_flags(
	const struct uvc_assembler *assembler)
{
	const uint8_t *bytes;
	size_t start;
	size_t index;
	uint32_t flags;

	/* SOI must open the frame. */
	flags = 0U;
	bytes = assembler->address;
	if (assembler->offset < 2U) {
		flags |= CAM_FRAME_ERROR;
	} else if (bytes[0] != UVC_JPEG_MARKER || bytes[1] != UVC_JPEG_SOI) {
		flags |= CAM_FRAME_ERROR;
	}

	/* Where the search for EOI starts: 64 bytes before the end, or the start. */
	start = 0U;
	if (assembler->offset > UVC_JPEG_TAIL)
		start = assembler->offset - UVC_JPEG_TAIL;

	/* EOI in the tail, which padding may follow. */
	for (index = start; index + 1U < assembler->offset; index++) {
		if (bytes[index] == UVC_JPEG_MARKER && bytes[index + 1U] == UVC_JPEG_EOI)
			return flags;
	}

	/* Succeeded: no EOI, so the frame is incomplete. */
	return flags | CAM_FRAME_INCOMPLETE;
}
