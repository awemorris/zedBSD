/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The USB Video Class descriptors and stream negotiation records, as the
 * uvc driver reads them.
 *
 * Everything here works on bytes the caller hands in: the class-specific
 * descriptors of the VideoControl and VideoStreaming interfaces, the
 * PROBE/COMMIT record, and the table of the streaming interface's
 * alternates.  Nothing calls into the USB core, so the parsing and the
 * arithmetic can be exercised on the host.
 */

#ifndef KERN_DRIVERS_USB_UVC_DESC_H
#define KERN_DRIVERS_USB_UVC_DESC_H

#include <stddef.h>
#include <stdint.h>

/* How many streaming interfaces, formats, frames and intervals are kept. */
#define UVC_STREAMS_MAX			4U
#define UVC_FORMATS_MAX			8U
#define UVC_FRAMES_MAX			32U
#define UVC_INTERVALS_MAX		32U
#define UVC_ALTERNATES_MAX		16U

/* The longest PROBE/COMMIT record of any class version. */
#define UVC_PROBE_MAX			48U

/*
 * The descriptor types and subtypes.
 *
 * Value(s) referenced: USB Video Class 1.1 §A.4 (class-specific
 * descriptor types), §A.5 (VideoControl interface subtypes) and §A.6
 * (VideoStreaming interface subtypes).
 */
#define UVC_CS_INTERFACE		0x24U
#define UVC_VC_HEADER			0x01U
#define UVC_VC_INPUT_TERMINAL		0x02U
#define UVC_VC_OUTPUT_TERMINAL		0x03U
#define UVC_VC_SELECTOR_UNIT		0x04U
#define UVC_VC_PROCESSING_UNIT		0x05U
#define UVC_VC_EXTENSION_UNIT		0x06U
#define UVC_VS_INPUT_HEADER		0x01U
#define UVC_VS_FORMAT_UNCOMPRESSED	0x04U
#define UVC_VS_FRAME_UNCOMPRESSED	0x05U
#define UVC_VS_FORMAT_MJPEG		0x06U
#define UVC_VS_FRAME_MJPEG		0x07U
#define UVC_VS_COLORFORMAT		0x0dU
#define UVC_VS_FORMAT_FRAME_BASED	0x10U
#define UVC_VS_FRAME_FRAME_BASED	0x11U

/*
 * The interface class and subclasses.
 *
 * Value(s) referenced: USB Video Class 1.1 §A.1 and §A.2.
 */
#define UVC_INTERFACE_CLASS		0x0eU
#define UVC_SUBCLASS_VIDEOCONTROL	0x01U
#define UVC_SUBCLASS_VIDEOSTREAMING	0x02U

/*
 * The class requests and the VideoStreaming control selectors.
 *
 * Value(s) referenced: USB Video Class 1.1 §A.8 (request codes) and
 * §A.9.7 (VideoStreaming interface control selectors).
 */
#define UVC_SET_CUR			0x01U
#define UVC_GET_CUR			0x81U
#define UVC_GET_MIN			0x82U
#define UVC_GET_MAX			0x83U
#define UVC_GET_RES			0x84U
#define UVC_GET_INFO			0x86U
#define UVC_GET_DEF			0x87U
#define UVC_VS_PROBE_CONTROL		0x01U
#define UVC_VS_COMMIT_CONTROL		0x02U

/*
 * The bits of a control's GET_INFO answer: GET and SET are supported,
 * and the control is disabled while an automatic mode runs.
 *
 * Value(s) referenced: USB Video Class 1.1 §4.1.2 (GET_INFO, the
 * bmCapabilities bits D0 to D2).
 */
#define UVC_INFO_GET			0x01U
#define UVC_INFO_SET			0x02U
#define UVC_INFO_DISABLED		0x04U

/*
 * The camera terminal's type and the layout of the units that carry
 * controls.
 *
 * Value(s) referenced: USB Video Class 1.1 §B.2 (ITT_CAMERA 0x0201),
 * §3.7.2.3 (camera terminal: bTerminalID at 3, wTerminalType at 4,
 * bControlSize at 14, bmControls at 15) and §3.7.2.5 (processing unit:
 * bControlSize at 7, bmControls at 8).
 */
#define UVC_ITT_CAMERA			0x0201U
#define UVC_TERMINAL_TYPE		4U
#define UVC_CAMERA_CONTROL_SIZE		14U
#define UVC_PROCESSING_CONTROL_SIZE	7U

/* The units a control belongs to. */
#define UVC_UNIT_PROCESSING		1U
#define UVC_UNIT_CAMERA			2U

/*
 * How a control's value is coded: signed, a 0/1 switch, the camera's
 * auto-exposure mode bitmap, or a range the device does not report.
 */
#define UVC_CONTROL_SIGNED		0x01U
#define UVC_CONTROL_BOOLEAN		0x02U
#define UVC_CONTROL_AE_MODE		0x04U
#define UVC_CONTROL_FIXED_RANGE		0x08U

/*
 * A fixed range one longer from UVC 1.5 on: the power line frequency's
 * 3 (auto).
 *
 * Value(s) referenced: USB Video Class 1.5 §4.2.2.3.6 (Table 4-35).
 */
#define UVC_CONTROL_AUTO_FROM_1_5	0x10U
#define UVC_VERSION_1_5			0x0150U

/*
 * The auto-exposure modes of the camera terminal's AE mode control.
 *
 * Value(s) referenced: USB Video Class 1.1 §4.2.2.1.2 (bAutoExposureMode:
 * D0 manual, D1 auto, D2 shutter priority, D3 aperture priority).
 */
#define UVC_AE_MANUAL			0x01U
#define UVC_AE_AUTO			0x02U
#define UVC_AE_SHUTTER_PRIORITY		0x04U
#define UVC_AE_APERTURE_PRIORITY	0x08U

/*
 * The fields of the PROBE/COMMIT record, by offset.
 *
 * Value(s) referenced: USB Video Class 1.1 §4.3.1.1 (Video Probe and
 * Commit Controls, the table of the control's fields).
 */
#define UVC_PROBE_HINT			0U
#define UVC_PROBE_FORMAT_INDEX		2U
#define UVC_PROBE_FRAME_INDEX		3U
#define UVC_PROBE_FRAME_INTERVAL	4U
#define UVC_PROBE_MAX_FRAME_SIZE	18U
#define UVC_PROBE_MAX_PAYLOAD		22U
#define UVC_PROBE_LENGTH_1_0		26U
#define UVC_PROBE_LENGTH_1_1		34U
#define UVC_PROBE_LENGTH_1_5		48U

/* bmHint bit 0: the frame interval is to be kept as asked. */
#define UVC_PROBE_HINT_FRAME_INTERVAL	0x0001U

/* What one VS format descriptor describes. */
enum uvc_format_kind {
	UVC_FORMAT_UNCOMPRESSED = 1,
	UVC_FORMAT_MJPEG = 2
};

/* The bus speed of the device, as far as the alternates care. */
enum uvc_speed {
	UVC_SPEED_OTHER = 0,
	UVC_SPEED_FULL = 1,
	UVC_SPEED_HIGH = 2,
	UVC_SPEED_SUPER = 3
};

/* How the device moves its video data. */
enum uvc_transport {
	UVC_TRANSPORT_NONE = 0,
	UVC_TRANSPORT_ISOCHRONOUS = 1,
	UVC_TRANSPORT_BULK = 2
};

/*
 * The parts of the VideoControl interface the driver uses.
 *
 * It is filled descriptor by descriptor from the interface's extra
 * descriptors and is read only after that.  units counts the terminals
 * and units seen, which are only told in the log for now.
 */
struct uvc_control_desc {
	unsigned has_header;
	uint16_t bcd_uvc;
	uint32_t clock_hz;
	unsigned stream_count;
	uint8_t streams[UVC_STREAMS_MAX];
	unsigned units;
	unsigned processing_unit;
	uint8_t processing_unit_id;
	uint32_t processing_controls;
	unsigned camera_terminal;
	uint8_t camera_terminal_id;
	uint32_t camera_controls;
};

/*
 * One framework control as a UVC control: the unit, the selector, the
 * bit of the unit's bmControls that says the device has it, the bytes of
 * its value and how they are coded (UVC_CONTROL_*).  minimum and maximum
 * are the range of a UVC_CONTROL_FIXED_RANGE control.
 */
struct uvc_control_map {
	uint32_t id;
	uint8_t unit;
	uint8_t selector;
	uint8_t bit;
	uint8_t size;
	uint8_t flags;
	int64_t minimum;
	int64_t maximum;
	const char *name;
};

/*
 * One frame size of a format, with its frame intervals.
 *
 * interval_type is the descriptor's bFrameIntervalType: zero for a
 * continuous range (interval_min, interval_max, interval_step), else the
 * number of discrete intervals the device declares.  interval_count is
 * how many of those are kept in intervals[].
 */
struct uvc_frame {
	uint8_t index;
	uint8_t capabilities;
	uint16_t width;
	uint16_t height;
	uint32_t max_frame_bytes;
	uint32_t default_interval;
	uint8_t interval_type;
	unsigned interval_count;
	uint32_t intervals[UVC_INTERVALS_MAX];
	uint32_t interval_min;
	uint32_t interval_max;
	uint32_t interval_step;
};

/*
 * One format of the streaming interface and its frames.
 *
 * cam_format is the framework's CAM_FORMAT_* code, or zero for a format
 * the driver does not offer.  The colour fields come from a COLORFORMAT
 * descriptor that follows the format's frames, when there is one.
 */
struct uvc_format {
	uint8_t index;
	enum uvc_format_kind kind;
	uint32_t cam_format;
	uint8_t guid[16];
	uint8_t bits_per_pixel;
	uint8_t default_frame;
	uint8_t declared_frames;
	unsigned frame_count;
	struct uvc_frame frames[UVC_FRAMES_MAX];
	unsigned has_color;
	uint8_t color_primaries;
	uint8_t color_transfer;
	uint8_t color_matrix;
};

/*
 * The VideoStreaming interface as its alternate 0 describes it.
 *
 * stopped is set when a descriptor was too short to read: parsing stops
 * there and the formats read before it remain usable.  ignored counts
 * the descriptors of kinds the driver does not read.
 */
struct uvc_stream_desc {
	unsigned has_header;
	uint8_t declared_formats;
	uint8_t endpoint_address;
	uint8_t terminal_link;
	unsigned format_count;
	struct uvc_format formats[UVC_FORMATS_MAX];
	unsigned stopped;
	unsigned ignored;
	unsigned dropped_frames;
	unsigned dropped_intervals;
};

/*
 * One alternate of the streaming interface with an isochronous IN
 * endpoint.
 *
 * capacity is what one service interval carries, the packet size times
 * the transactions an interval; accepted says whether the host takes the
 * endpoint as it is.
 */
struct uvc_alternate {
	uint8_t setting;
	uint8_t endpoint_address;
	uint8_t interval;
	uint16_t max_packet_field;
	uint16_t packet_bytes;
	uint8_t transactions;
	uint32_t capacity;
	unsigned accepted;
};

/* What uvc_probe_fixup() changed, for the log. */
#define UVC_FIXUP_FRAME_SIZE		0x0001U
#define UVC_FIXUP_SIGN_EXTENDED		0x0002U
#define UVC_FIXUP_CLAMPED		0x0004U
#define UVC_FIXUP_ESTIMATED		0x0008U

/* Reads a little-endian 16-bit field. */
uint16_t uvc_get16(const uint8_t *bytes, size_t offset);

/* Reads a little-endian 32-bit field. */
uint32_t uvc_get32(const uint8_t *bytes, size_t offset);

/* Writes a little-endian 16-bit field. */
void uvc_put16(uint8_t *bytes, size_t offset, uint16_t value);

/* Writes a little-endian 32-bit field. */
void uvc_put32(uint8_t *bytes, size_t offset, uint32_t value);

/*
 * Reads one class-specific descriptor of the VideoControl interface.
 * Returns 0, or EINVAL for a descriptor too short to read.
 */
int uvc_desc_control_feed(struct uvc_control_desc *control, const uint8_t *descriptor, size_t length);

/*
 * Reads one class-specific descriptor of the VideoStreaming interface.
 * Returns 0, or EINVAL when parsing has to stop (stream->stopped).
 */
int uvc_desc_stream_feed(struct uvc_stream_desc *stream, const uint8_t *descriptor, size_t length);

/* Finds the UVC control of a CAM_CONTROL_* id, or NULL. */
const struct uvc_control_map *uvc_control_lookup(uint32_t id);

/* Reports the n-th UVC control the driver knows, or NULL past the last. */
const struct uvc_control_map *uvc_control_at(unsigned index);

/*
 * Tells whether the device declares a control, and the ID of the unit
 * that carries it.  Returns 1 or 0.
 */
int uvc_control_present(const struct uvc_control_desc *control, const struct uvc_control_map *map, uint8_t *unit_id);

/* Reports the largest value of a fixed-range control for a class version. */
int64_t uvc_control_fixed_maximum(const struct uvc_control_map *map, uint16_t bcd_uvc);

/* Reads a control's value from the bytes of a GET request. */
int64_t uvc_control_decode(const struct uvc_control_map *map, const uint8_t *bytes);

/*
 * Writes a control's value into the bytes of a SET request.  Returns 0,
 * or EINVAL for a value the control's bytes cannot hold.
 */
int uvc_control_encode(const struct uvc_control_map *map, int64_t value, uint8_t *bytes);

/*
 * Chooses the auto-exposure mode to set: manual for 0, otherwise the
 * automatic mode the device's GET_RES bitmap offers (aperture priority,
 * then full auto).  Returns 0 when the device offers none.
 */
uint8_t uvc_ae_mode_choose(uint8_t supported, int64_t automatic);

/* Tells whether an auto-exposure mode is an automatic one (1) or manual (0). */
int64_t uvc_ae_mode_automatic(uint8_t mode);

/* Maps an uncompressed format's GUID to a CAM_FORMAT_* code, or 0. */
uint32_t uvc_desc_guid_format(const uint8_t *guid);

/* Finds the n-th format the driver offers, or NULL. */
const struct uvc_format *uvc_desc_offered_format(const struct uvc_stream_desc *stream, unsigned index);

/* Finds the first format the driver offers for a CAM_FORMAT_* code, or NULL. */
const struct uvc_format *uvc_desc_find_format(const struct uvc_stream_desc *stream, uint32_t cam_format);

/* Finds a format's frame of an exact size, or NULL; *position is its place in frames[]. */
const struct uvc_frame *uvc_desc_find_frame(const struct uvc_format *format, uint32_t width, uint32_t height, unsigned *position);

/* Finds a format's frame by its bFrameIndex, or NULL. */
const struct uvc_frame *uvc_desc_frame_by_index(const struct uvc_format *format, unsigned frame_index);

/* Reports the bytes a frame's lines take: the stride of an uncompressed format, 0 for MJPEG. */
uint32_t uvc_desc_stride(const struct uvc_format *format, const struct uvc_frame *frame);

/*
 * Reports the size of the largest frame the buffers must hold.
 * probe_frame_size is the device's dwMaxVideoFrameSize after the fixup.
 */
uint32_t uvc_desc_image_bytes(const struct uvc_format *format, const struct uvc_frame *frame, uint32_t probe_frame_size);

/* Reports the length of the PROBE/COMMIT record for a class version. */
size_t uvc_probe_length(uint16_t bcd_uvc);

/* Fills a PROBE request for a format, a frame and an interval. */
void uvc_probe_request(uint8_t *record, size_t length, uint8_t format_index, uint8_t frame_index, uint32_t interval_100ns);

/*
 * Corrects a device's PROBE answer before it is used or committed, and
 * reports what it changed (UVC_FIXUP_*).
 */
unsigned uvc_probe_fixup(uint8_t *record, const struct uvc_format *format, const struct uvc_frame *frame, uint16_t bcd_uvc, unsigned isochronous, enum uvc_speed speed, uint32_t device_capacity_max);

/*
 * Decodes one isochronous IN endpoint into an alternate entry, and marks
 * whether the host takes it.  superspeed_bytes is the companion's
 * wBytesPerInterval, read only at SuperSpeed.
 */
void uvc_alternate_describe(struct uvc_alternate *alternate, uint8_t setting, uint8_t endpoint_address, uint16_t max_packet_field, uint8_t interval, enum uvc_speed speed, uint32_t superspeed_bytes);

/* Reports the largest capacity among the alternates the host takes, 0 without one. */
uint32_t uvc_alternate_host_max(const struct uvc_alternate *alternates, unsigned count);

/* Reports the largest capacity among all the device's alternates. */
uint32_t uvc_alternate_device_max(const struct uvc_alternate *alternates, unsigned count);

/*
 * Chooses the smallest alternate the host takes whose capacity holds a
 * payload.  Returns 0 with *position, or ENOSPC without one.
 */
int uvc_alternate_choose(const struct uvc_alternate *alternates, unsigned count, uint32_t payload_bytes, unsigned *position);

#endif
