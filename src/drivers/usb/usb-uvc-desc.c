/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The USB Video Class descriptor parser, the PROBE/COMMIT record and the
 * choice of a streaming alternate.
 *
 * Every offset is from the start of its descriptor or record.  A
 * descriptor shorter than its fixed part stops the parsing of the
 * streaming interface; what was read before it stays usable.
 */

#include <drivers/camera/cam.h>
#include <drivers/usb/usb-uvc-desc.h>

#include <kern/kcrt.h>
#include <uapi/errno.h>

/*
 * The fixed lengths of the descriptors the parser reads.
 *
 * Value(s) referenced: USB Video Class 1.1 §3.7.2 (class-specific VC
 * interface header) and §3.9.2.1 (input header); USB Video Class
 * Uncompressed Payload 1.1 §3.1.1 (format, 27 bytes) and §3.1.2 (frame);
 * USB Video Class Motion-JPEG Payload 1.1 §3.1.1 (format, 11 bytes) and
 * §3.1.2 (frame); USB Video Class 1.1 §3.9.2.6 (colour matching, 6 bytes).
 */
#define UVC_VC_HEADER_LENGTH		12U
#define UVC_VS_INPUT_HEADER_LENGTH	13U
#define UVC_FORMAT_UNCOMPRESSED_LENGTH	27U
#define UVC_FORMAT_MJPEG_LENGTH		11U
#define UVC_FRAME_FIXED_LENGTH		26U
#define UVC_FRAME_CONTINUOUS_LENGTH	38U
#define UVC_COLORFORMAT_LENGTH		6U
#define UVC_PROCESSING_UNIT_ID		3U

/*
 * The host's limit of an isochronous endpoint: full speed with one
 * transaction an interval or high speed with up to three, a packet of at
 * most 1024 bytes and a bInterval of 1 to 16.  It is the condition the
 * xHCI driver's endpoint encoder accepts.
 */
#define UVC_HOST_PACKET_MAX		1024U
#define UVC_HOST_INTERVAL_MAX		16U

/* The bits of wMaxPacketSize: the size, and the extra transactions of a high-speed interval. */
#define UVC_PACKET_SIZE_MASK		0x07ffU
#define UVC_PACKET_EXTRA_SHIFT		11U
#define UVC_PACKET_EXTRA_MASK		0x0003U
#define UVC_PACKET_EXTRA_RESERVED	3U
#define UVC_PACKET_RESERVED_MASK	0xe000U

/* The payload header a payload carries at least, added to an estimated payload. */
#define UVC_PAYLOAD_HEADER_ESTIMATE	12U

/* The smallest estimated payload. */
#define UVC_PAYLOAD_ESTIMATE_MIN	1024U

/* One 100 ns interval unit a second. */
#define UVC_INTERVALS_PER_SECOND	10000000U

/*
 * The uncompressed formats the driver offers, by GUID.
 *
 * The first four bytes are the FourCC; Data2 and Data3 are stored little
 * endian, so the GUIDs are compared as these byte strings.
 *
 * Value(s) referenced: USB Video Class Uncompressed Payload 1.1 §2.2
 * (the table of the format GUIDs).
 */
struct uvc_guid_entry {
	uint8_t guid[16];
	uint32_t cam_format;
};

/* The GUID table; the parser maps a format through it. */
static const struct uvc_guid_entry uvc_guids[] = {
	{
		{ 0x59, 0x55, 0x59, 0x32, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 },
		CAM_FORMAT_YUYV
	},
	{
		{ 0x55, 0x59, 0x56, 0x59, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 },
		CAM_FORMAT_UYVY
	},
	{
		{ 0x4e, 0x56, 0x31, 0x32, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 },
		CAM_FORMAT_NV12
	}
};

/*
 * The controls the driver offers, as UVC controls of the processing unit
 * and the camera terminal.
 *
 * Value(s) referenced: USB Video Class 1.1 §A.9.4 (camera terminal
 * control selectors), §A.9.5 (processing unit control selectors),
 * §3.7.2.3 (the camera terminal's bmControls bits) and §3.7.2.5 (the
 * processing unit's bmControls bits); the value sizes and signs are the
 * controls' tables in §4.2.2.1 and §4.2.2.3.  The power line frequency
 * reports no range; its values are 0 (off), 1 (50 Hz) and 2 (60 Hz), and
 * from UVC 1.5 on also 3 (auto, §4.2.2.3.6 of 1.5).
 */
static const struct uvc_control_map uvc_controls[] = {
	{ CAM_CONTROL_BRIGHTNESS, UVC_UNIT_PROCESSING, 0x02U, 0U, 2U, UVC_CONTROL_SIGNED, 0, 0, "brightness" },
	{ CAM_CONTROL_CONTRAST, UVC_UNIT_PROCESSING, 0x03U, 1U, 2U, 0U, 0, 0, "contrast" },
	{ CAM_CONTROL_HUE, UVC_UNIT_PROCESSING, 0x06U, 2U, 2U, UVC_CONTROL_SIGNED, 0, 0, "hue" },
	{ CAM_CONTROL_SATURATION, UVC_UNIT_PROCESSING, 0x07U, 3U, 2U, 0U, 0, 0, "saturation" },
	{ CAM_CONTROL_SHARPNESS, UVC_UNIT_PROCESSING, 0x08U, 4U, 2U, 0U, 0, 0, "sharpness" },
	{ CAM_CONTROL_GAMMA, UVC_UNIT_PROCESSING, 0x09U, 5U, 2U, 0U, 0, 0, "gamma" },
	{ CAM_CONTROL_WHITE_BALANCE, UVC_UNIT_PROCESSING, 0x0aU, 6U, 2U, 0U, 0, 0, "white balance temperature" },
	{ CAM_CONTROL_WHITE_BALANCE_AUTO, UVC_UNIT_PROCESSING, 0x0bU, 12U, 1U, UVC_CONTROL_BOOLEAN, 0, 1, "white balance auto" },
	{ CAM_CONTROL_BACKLIGHT, UVC_UNIT_PROCESSING, 0x01U, 8U, 2U, 0U, 0, 0, "backlight compensation" },
	{ CAM_CONTROL_GAIN, UVC_UNIT_PROCESSING, 0x04U, 9U, 2U, 0U, 0, 0, "gain" },
	{ CAM_CONTROL_POWER_LINE, UVC_UNIT_PROCESSING, 0x05U, 10U, 1U, UVC_CONTROL_FIXED_RANGE | UVC_CONTROL_AUTO_FROM_1_5, 0, 2, "power line frequency" },
	{ CAM_CONTROL_EXPOSURE, UVC_UNIT_CAMERA, 0x04U, 3U, 4U, 0U, 0, 0, "exposure time" },
	{ CAM_CONTROL_EXPOSURE_AUTO, UVC_UNIT_CAMERA, 0x02U, 1U, 1U, UVC_CONTROL_BOOLEAN | UVC_CONTROL_AE_MODE, 0, 1, "exposure auto" },
	{ CAM_CONTROL_FOCUS, UVC_UNIT_CAMERA, 0x06U, 5U, 2U, 0U, 0, 0, "focus" },
	{ CAM_CONTROL_FOCUS_AUTO, UVC_UNIT_CAMERA, 0x08U, 17U, 1U, UVC_CONTROL_BOOLEAN, 0, 1, "focus auto" },
	{ CAM_CONTROL_ZOOM, UVC_UNIT_CAMERA, 0x0bU, 9U, 2U, 0U, 0, 0, "zoom" }
};

static uint32_t uvc_desc_bitmap(const uint8_t *descriptor, size_t length, size_t size_offset);
static int uvc_desc_input_header(struct uvc_stream_desc *stream, const uint8_t *descriptor, size_t length);
static int uvc_desc_format(struct uvc_stream_desc *stream, const uint8_t *descriptor, size_t length, enum uvc_format_kind kind);
static int uvc_desc_frame(struct uvc_stream_desc *stream, const uint8_t *descriptor, size_t length, enum uvc_format_kind kind);
static int uvc_desc_colorformat(struct uvc_stream_desc *stream, const uint8_t *descriptor, size_t length);
static struct uvc_format *uvc_desc_last_format(struct uvc_stream_desc *stream);

/*
 * Reads a little-endian 16-bit field.
 */
uint16_t
uvc_get16(
	const uint8_t *bytes,
	size_t offset)
{
	uint16_t value;

	/* The low byte comes first. */
	value = (uint16_t)bytes[offset];
	value |= (uint16_t)((uint16_t)bytes[offset + 1U] << 8);

	/* Succeeded: the field's value. */
	return value;
}

/*
 * Reads a little-endian 32-bit field.
 */
uint32_t
uvc_get32(
	const uint8_t *bytes,
	size_t offset)
{
	uint32_t value;

	/* The lowest byte comes first. */
	value = (uint32_t)bytes[offset];
	value |= (uint32_t)bytes[offset + 1U] << 8;
	value |= (uint32_t)bytes[offset + 2U] << 16;
	value |= (uint32_t)bytes[offset + 3U] << 24;

	/* Succeeded: the field's value. */
	return value;
}

/*
 * Writes a little-endian 16-bit field.
 */
void
uvc_put16(
	uint8_t *bytes,
	size_t offset,
	uint16_t value)
{
	/* The low byte goes first. */
	bytes[offset] = (uint8_t)(value & 0xffU);
	bytes[offset + 1U] = (uint8_t)(value >> 8);
}

/*
 * Writes a little-endian 32-bit field.
 */
void
uvc_put32(
	uint8_t *bytes,
	size_t offset,
	uint32_t value)
{
	/* The lowest byte goes first. */
	bytes[offset] = (uint8_t)(value & 0xffU);
	bytes[offset + 1U] = (uint8_t)((value >> 8) & 0xffU);
	bytes[offset + 2U] = (uint8_t)((value >> 16) & 0xffU);
	bytes[offset + 3U] = (uint8_t)(value >> 24);
}

/*
 * Reads one class-specific descriptor of the VideoControl interface.
 *
 * Value(s) referenced: USB Video Class 1.1 §3.7.2 (header: bcdUVC at 3,
 * wTotalLength at 5, dwClockFrequency at 7, bInCollection at 11,
 * baInterfaceNr at 12) and §3.7.2.5 (processing unit: bUnitID at 3).
 */
int
uvc_desc_control_feed(
	struct uvc_control_desc *control,
	const uint8_t *descriptor,
	size_t length)
{
	unsigned collection;
	unsigned index;
	unsigned terminal_type;
	uint8_t subtype;

	/* A descriptor without its type and subtype cannot be read. */
	if (length < 3U)
		return EINVAL;

	/* Only the class-specific interface descriptors are the class's. */
	if (descriptor[1] != UVC_CS_INTERFACE)
		return 0;

	/* Reads the descriptor its subtype names. */
	subtype = descriptor[2];
	switch (subtype) {
	case UVC_VC_HEADER:
		/* The header gives the version, the clock and the streaming interfaces. */
		if (length < UVC_VC_HEADER_LENGTH)
			return EINVAL;

		/* Keeps the version and the clock the header gives. */
		control->has_header = 1U;
		control->bcd_uvc = uvc_get16(descriptor, 3U);
		control->clock_hz = uvc_get32(descriptor, 7U);

		/* Keeps the streaming interfaces the header lists, as far as the descriptor holds them. */
		collection = descriptor[11];
		control->stream_count = 0U;
		for (index = 0U; index < collection; index++) {
			if (UVC_VC_HEADER_LENGTH + index >= length)
				break;
			if (control->stream_count >= UVC_STREAMS_MAX)
				break;
			control->streams[control->stream_count] = descriptor[UVC_VC_HEADER_LENGTH + index];
			control->stream_count++;
		}

		break;
	case UVC_VC_PROCESSING_UNIT:
		/* The processing unit's ID is what its controls are addressed by later. */
		if (length < UVC_PROCESSING_UNIT_ID + 1U)
			return EINVAL;

		/* Keeps the unit, its ID and the controls it carries. */
		control->processing_unit = 1U;
		control->processing_unit_id = descriptor[UVC_PROCESSING_UNIT_ID];
		control->processing_controls = uvc_desc_bitmap(descriptor, length, UVC_PROCESSING_CONTROL_SIZE);

		/* Counts the unit. */
		control->units++;
		break;
	case UVC_VC_INPUT_TERMINAL:
		/* A camera terminal carries the exposure, focus and zoom controls. */
		terminal_type = 0U;
		if (length > UVC_CAMERA_CONTROL_SIZE)
			terminal_type = uvc_get16(descriptor, UVC_TERMINAL_TYPE);
		if (length > UVC_CAMERA_CONTROL_SIZE &&
		    terminal_type == UVC_ITT_CAMERA &&
		    control->camera_terminal == 0U) {
			control->camera_terminal = 1U;
			control->camera_terminal_id = descriptor[3];
			control->camera_controls = uvc_desc_bitmap(descriptor, length, UVC_CAMERA_CONTROL_SIZE);
		}

		/* Counts the terminal. */
		control->units++;
		break;
	case UVC_VC_OUTPUT_TERMINAL:
	case UVC_VC_SELECTOR_UNIT:
	case UVC_VC_EXTENSION_UNIT:
		/* The other terminals and units are counted only. */
		control->units++;
		break;
	default:
		break;
	}

	/* Succeeded: the descriptor is read. */
	return 0;
}

/*
 * Finds the UVC control of a CAM_CONTROL_* id.
 */
const struct uvc_control_map *
uvc_control_lookup(
	uint32_t id)
{
	unsigned index;

	/* The table is short; a straight search. */
	for (index = 0U; index < sizeof(uvc_controls) / sizeof(uvc_controls[0]); index++) {
		if (uvc_controls[index].id == id)
			return &uvc_controls[index];
	}

	/* No such control. */
	return NULL;
}

/*
 * Reports the n-th UVC control the driver knows.
 */
const struct uvc_control_map *
uvc_control_at(
	unsigned index)
{
	/* Past the last there is none. */
	if (index >= sizeof(uvc_controls) / sizeof(uvc_controls[0]))
		return NULL;

	/* Succeeded: the entry. */
	return &uvc_controls[index];
}

/*
 * Tells whether the device declares a control and which unit carries it.
 */
int
uvc_control_present(
	const struct uvc_control_desc *control,
	const struct uvc_control_map *map,
	uint8_t *unit_id)
{
	uint32_t controls;

	/* The unit's bitmap and ID. */
	if (map->unit == UVC_UNIT_PROCESSING && control->processing_unit != 0U) {
		controls = control->processing_controls;
		*unit_id = control->processing_unit_id;
	} else if (map->unit == UVC_UNIT_CAMERA && control->camera_terminal != 0U) {
		controls = control->camera_controls;
		*unit_id = control->camera_terminal_id;
	} else {
		return 0;
	}

	/* A clear bit is a control the device does not have. */
	if ((controls & ((uint32_t)1U << map->bit)) == 0U)
		return 0;

	/* Succeeded: the device has the control. */
	return 1;
}

/*
 * Reports the largest value of a fixed-range control for a class version.
 */
int64_t
uvc_control_fixed_maximum(
	const struct uvc_control_map *map,
	uint16_t bcd_uvc)
{
	/* A device of UVC 1.5 or later has one more value. */
	if ((map->flags & UVC_CONTROL_AUTO_FROM_1_5) != 0U && bcd_uvc >= UVC_VERSION_1_5)
		return map->maximum + 1;

	/* Succeeded: the table's maximum. */
	return map->maximum;
}

/*
 * Reads a control's value from the bytes of a GET request.
 */
int64_t
uvc_control_decode(
	const struct uvc_control_map *map,
	const uint8_t *bytes)
{
	uint32_t raw;

	/* The little-endian field of the control's size. */
	if (map->size == 1U)
		raw = bytes[0];
	else if (map->size == 2U)
		raw = uvc_get16(bytes, 0U);
	else
		raw = uvc_get32(bytes, 0U);

	/* A signed 16-bit field extends its sign; the other fields are unsigned. */
	if ((map->flags & UVC_CONTROL_SIGNED) != 0U && map->size == 2U)
		return (int64_t)(int16_t)(uint16_t)raw;
	if ((map->flags & UVC_CONTROL_SIGNED) != 0U && map->size == 1U)
		return (int64_t)(int8_t)(uint8_t)raw;

	/* Succeeded: the unsigned value. */
	return (int64_t)raw;
}

/*
 * Writes a control's value into the bytes of a SET request.
 */
int
uvc_control_encode(
	const struct uvc_control_map *map,
	int64_t value,
	uint8_t *bytes)
{
	int64_t minimum;
	int64_t maximum;

	/* The range the control's bytes can hold. */
	if (map->size == 1U) {
		minimum = 0;
		maximum = 0xff;
	} else if (map->size == 2U) {
		minimum = 0;
		maximum = 0xffff;
	} else {
		minimum = 0;
		maximum = 0xffffffffLL;
	}

	/* A signed control spans the negative half of the range as well. */
	if ((map->flags & UVC_CONTROL_SIGNED) != 0U) {
		minimum = -(maximum / 2) - 1;
		maximum = maximum / 2;
	}

	/* Refuses a value the field cannot hold. */
	if (value < minimum || value > maximum)
		return EINVAL;

	/* The little-endian field. */
	if (map->size == 1U)
		bytes[0] = (uint8_t)value;
	else if (map->size == 2U)
		uvc_put16(bytes, 0U, (uint16_t)value);
	else
		uvc_put32(bytes, 0U, (uint32_t)value);

	/* Succeeded: the bytes hold the value. */
	return 0;
}

/*
 * Chooses the auto-exposure mode to set for a 0/1 switch.
 */
uint8_t
uvc_ae_mode_choose(
	uint8_t supported,
	int64_t automatic)
{
	/* Manual exposure. */
	if (automatic == 0) {
		if ((supported & UVC_AE_MANUAL) != 0U)
			return UVC_AE_MANUAL;
		return 0U;
	}

	/* Aperture priority is what webcams call automatic; full auto otherwise. */
	if ((supported & UVC_AE_APERTURE_PRIORITY) != 0U)
		return UVC_AE_APERTURE_PRIORITY;
	if ((supported & UVC_AE_AUTO) != 0U)
		return UVC_AE_AUTO;

	/* The device has no automatic mode. */
	return 0U;
}

/*
 * Tells whether an auto-exposure mode is automatic.
 */
int64_t
uvc_ae_mode_automatic(
	uint8_t mode)
{
	/* Full auto and aperture priority set the exposure time themselves. */
	if ((mode & (UVC_AE_AUTO | UVC_AE_APERTURE_PRIORITY)) != 0U)
		return 1;

	/* Manual and shutter priority leave the exposure time to the user. */
	return 0;
}

/*
 * Reads one class-specific descriptor of the VideoStreaming interface.
 */
int
uvc_desc_stream_feed(
	struct uvc_stream_desc *stream,
	const uint8_t *descriptor,
	size_t length)
{
	uint8_t subtype;
	int error;

	/* Nothing more is read once a descriptor stopped the parsing. */
	if (stream->stopped != 0U)
		return EINVAL;

	/* A descriptor without its type and subtype stops the parsing. */
	if (length < 3U) {
		stream->stopped = 1U;
		return EINVAL;
	}

	/* Only the class-specific interface descriptors are the class's. */
	if (descriptor[1] != UVC_CS_INTERFACE) {
		stream->ignored++;
		return 0;
	}

	/* Reads the descriptor its subtype names. */
	error = 0;
	subtype = descriptor[2];
	switch (subtype) {
	case UVC_VS_INPUT_HEADER:
		error = uvc_desc_input_header(stream, descriptor, length);
		break;
	case UVC_VS_FORMAT_UNCOMPRESSED:
		error = uvc_desc_format(stream, descriptor, length, UVC_FORMAT_UNCOMPRESSED);
		break;
	case UVC_VS_FORMAT_MJPEG:
		error = uvc_desc_format(stream, descriptor, length, UVC_FORMAT_MJPEG);
		break;
	case UVC_VS_FRAME_UNCOMPRESSED:
		error = uvc_desc_frame(stream, descriptor, length, UVC_FORMAT_UNCOMPRESSED);
		break;
	case UVC_VS_FRAME_MJPEG:
		error = uvc_desc_frame(stream, descriptor, length, UVC_FORMAT_MJPEG);
		break;
	case UVC_VS_COLORFORMAT:
		error = uvc_desc_colorformat(stream, descriptor, length);
		break;
	default:
		/* Frame-based formats and every other subtype are skipped by their length. */
		stream->ignored++;
		break;
	}

	/* A descriptor too short to read stops the parsing here. */
	if (error != 0) {
		stream->stopped = 1U;
		return error;
	}

	/* Succeeded: the descriptor is read. */
	return 0;
}

/*
 * Maps an uncompressed format's GUID to a CAM_FORMAT_* code, or 0.
 */
uint32_t
uvc_desc_guid_format(
	const uint8_t *guid)
{
	unsigned index;
	int difference;

	/* Compares the GUID with each one the driver offers, byte by byte. */
	for (index = 0U; index < sizeof(uvc_guids) / sizeof(uvc_guids[0]); index++) {
		difference = kern_memcmp(guid, uvc_guids[index].guid, sizeof(uvc_guids[index].guid));
		if (difference == 0)
			return uvc_guids[index].cam_format;
	}

	/* The format is not one the driver offers. */
	return 0U;
}

/*
 * Finds the n-th format the driver offers, or NULL.
 */
const struct uvc_format *
uvc_desc_offered_format(
	const struct uvc_stream_desc *stream,
	unsigned index)
{
	unsigned position;
	unsigned seen;

	/* Counts only the formats with a CAM_FORMAT_* code. */
	seen = 0U;
	for (position = 0U; position < stream->format_count; position++) {
		if (stream->formats[position].cam_format == 0U)
			continue;
		if (seen == index)
			return &stream->formats[position];
		seen++;
	}

	/* The index is past the last offered format. */
	return NULL;
}

/*
 * Finds the first format the driver offers for a CAM_FORMAT_* code, or NULL.
 */
const struct uvc_format *
uvc_desc_find_format(
	const struct uvc_stream_desc *stream,
	uint32_t cam_format)
{
	unsigned position;

	/* A zero code names no format. */
	if (cam_format == 0U)
		return NULL;

	/* The first format with the code. */
	for (position = 0U; position < stream->format_count; position++) {
		if (stream->formats[position].cam_format == cam_format)
			return &stream->formats[position];
	}

	/* The device has no such format. */
	return NULL;
}

/*
 * Finds a format's frame of an exact size, or NULL.
 */
const struct uvc_frame *
uvc_desc_find_frame(
	const struct uvc_format *format,
	uint32_t width,
	uint32_t height,
	unsigned *position)
{
	unsigned index;

	/* The frame whose width and height are both the ones asked for. */
	for (index = 0U; index < format->frame_count; index++) {
		if (format->frames[index].width != width)
			continue;
		if (format->frames[index].height != height)
			continue;
		if (position != NULL)
			*position = index;
		return &format->frames[index];
	}

	/* The format has no frame of that size. */
	return NULL;
}

/*
 * Finds a format's frame by its bFrameIndex, or NULL.
 */
const struct uvc_frame *
uvc_desc_frame_by_index(
	const struct uvc_format *format,
	unsigned frame_index)
{
	unsigned index;

	/* The frame the device numbers so. */
	for (index = 0U; index < format->frame_count; index++) {
		if (format->frames[index].index == frame_index)
			return &format->frames[index];
	}

	/* No frame has that number. */
	return NULL;
}

/*
 * Reports the bytes a frame's lines take.
 *
 * UVC does not pad the lines of an uncompressed frame, so a packed
 * format's stride is its width times its bytes a pixel.  NV12's stride is
 * that of its luma plane, one byte a pixel.  A compressed frame has none.
 */
uint32_t
uvc_desc_stride(
	const struct uvc_format *format,
	const struct uvc_frame *frame)
{
	/* A compressed frame has no lines. */
	if (format->kind != UVC_FORMAT_UNCOMPRESSED)
		return 0U;

	/* NV12's lines are its luma plane's. */
	if (format->cam_format == CAM_FORMAT_NV12)
		return frame->width;

	/* Succeeded: a packed line's bytes. */
	return (uint32_t)frame->width * format->bits_per_pixel / 8U;
}

/*
 * Reports the size of the largest frame the buffers must hold.
 *
 * An uncompressed frame is its computed size, or the frame descriptor's
 * buffer size when that is larger.  An MJPEG frame is the device's
 * dwMaxVideoFrameSize; when that is zero or less than a quarter byte a
 * pixel, the frame descriptor's buffer size, and when that is too small
 * as well, two bytes a pixel.
 */
uint32_t
uvc_desc_image_bytes(
	const struct uvc_format *format,
	const struct uvc_frame *frame,
	uint32_t probe_frame_size)
{
	uint64_t computed;
	uint32_t smallest;

	/* An uncompressed frame: the pixels, or the device's larger buffer size. */
	if (format->kind == UVC_FORMAT_UNCOMPRESSED) {
		computed = (uint64_t)frame->width * frame->height * format->bits_per_pixel / 8U;
		if (frame->max_frame_bytes > computed)
			computed = frame->max_frame_bytes;
		if (computed > UINT32_MAX)
			computed = UINT32_MAX;
		return (uint32_t)computed;
	}

	/* The smallest size an MJPEG frame is believed to fit. */
	smallest = (uint32_t)(((uint64_t)frame->width * frame->height) / 4U);

	/* The device's answer, when it is believable. */
	if (probe_frame_size != 0U && probe_frame_size >= smallest)
		return probe_frame_size;

	/* The frame descriptor's buffer size, when that is believable. */
	if (frame->max_frame_bytes != 0U && frame->max_frame_bytes >= smallest)
		return frame->max_frame_bytes;

	/* Two bytes a pixel, which a JPEG of the frame does not exceed. */
	computed = (uint64_t)frame->width * frame->height * 2U;
	if (computed > UINT32_MAX)
		computed = UINT32_MAX;

	/* Succeeded: the estimated size. */
	return (uint32_t)computed;
}

/*
 * Reports the length of the PROBE/COMMIT record for a class version.
 *
 * The length is decided by bcdUVC alone: 26 bytes before 1.1, 34 before
 * 1.5, 48 from 1.5 on.
 *
 * Value(s) referenced: USB Video Class 1.1 §4.3.1.1 and USB Video Class
 * 1.5 §4.3.1.1 (the record's length in each version).
 */
size_t
uvc_probe_length(
	uint16_t bcd_uvc)
{
	/* A 1.0 device's record. */
	if (bcd_uvc < 0x0110U)
		return UVC_PROBE_LENGTH_1_0;

	/* A 1.1 device's record. */
	if (bcd_uvc < 0x0150U)
		return UVC_PROBE_LENGTH_1_1;

	/* Succeeded: a 1.5 device's record. */
	return UVC_PROBE_LENGTH_1_5;
}

/*
 * Fills a PROBE request for a format, a frame and an interval.
 *
 * Every other field is zero, which lets the device choose it.
 */
void
uvc_probe_request(
	uint8_t *record,
	size_t length,
	uint8_t format_index,
	uint8_t frame_index,
	uint32_t interval_100ns)
{
	/* An empty record. */
	kern_memset(record, 0, length);

	/* The frame interval is the one asked for. */
	uvc_put16(record, UVC_PROBE_HINT, UVC_PROBE_HINT_FRAME_INTERVAL);

	/* The format, the frame and the interval. */
	record[UVC_PROBE_FORMAT_INDEX] = format_index;
	record[UVC_PROBE_FRAME_INDEX] = frame_index;
	uvc_put32(record, UVC_PROBE_FRAME_INTERVAL, interval_100ns);
}

/*
 * Corrects a device's PROBE answer before it is used or committed.
 *
 * Procedure referenced: linux drivers/media/usb/uvc/uvc_video.c
 * (uvc_fixup_video_ctrl): which answers devices are known to get wrong.
 * Value(s) referenced: the same function, for the payload estimate's
 * formula (the pixels' bytes times the frames a second plus one, in
 * thousandths, an eighth of that at high speed, plus 12, at least 1024).
 */
unsigned
uvc_probe_fixup(
	uint8_t *record,
	const struct uvc_format *format,
	const struct uvc_frame *frame,
	uint16_t bcd_uvc,
	unsigned isochronous,
	enum uvc_speed speed,
	uint32_t device_capacity_max)
{
	uint64_t estimate;
	uint32_t interval;
	uint32_t payload;
	uint32_t frame_size;
	unsigned changed;

	/*
	 * An uncompressed frame's size is the frame descriptor's, never the
	 * device's answer; an old device's compressed answer of zero is
	 * replaced the same way.
	 */
	changed = 0U;
	frame_size = uvc_get32(record, UVC_PROBE_MAX_FRAME_SIZE);
	if (format->kind == UVC_FORMAT_UNCOMPRESSED) {
		uvc_put32(record, UVC_PROBE_MAX_FRAME_SIZE, frame->max_frame_bytes);
		changed |= UVC_FIXUP_FRAME_SIZE;
	} else if (frame_size == 0U && bcd_uvc < 0x0110U) {
		uvc_put32(record, UVC_PROBE_MAX_FRAME_SIZE, frame->max_frame_bytes);
		changed |= UVC_FIXUP_FRAME_SIZE;
	}

	/* A payload size whose upper half is all ones was sign-extended from 16 bits. */
	payload = uvc_get32(record, UVC_PROBE_MAX_PAYLOAD);
	if ((payload & 0xffff0000U) == 0xffff0000U) {
		payload = 0U;
		changed |= UVC_FIXUP_SIGN_EXTENDED;
	}

	/* An isochronous device cannot ask for more than its own largest alternate carries. */
	if (isochronous != 0U &&
	    device_capacity_max != 0U &&
	    payload > device_capacity_max) {
		payload = device_capacity_max;
		changed |= UVC_FIXUP_CLAMPED;
	}

	/* A payload of zero is estimated: from the pixels when uncompressed, else the largest alternate. */
	if (payload == 0U) {
		if (format->kind == UVC_FORMAT_UNCOMPRESSED) {
			interval = uvc_get32(record, UVC_PROBE_FRAME_INTERVAL);
			if (interval == 0U)
				interval = frame->default_interval;
			if (interval == 0U)
				interval = UVC_INTERVALS_PER_SECOND;
			estimate = (uint64_t)frame->width * frame->height / 8U * format->bits_per_pixel;
			estimate *= UVC_INTERVALS_PER_SECOND / interval + 1U;
			estimate /= 1000U;
			if (speed >= UVC_SPEED_HIGH)
				estimate /= 8U;
			estimate += UVC_PAYLOAD_HEADER_ESTIMATE;
			if (estimate < UVC_PAYLOAD_ESTIMATE_MIN)
				estimate = UVC_PAYLOAD_ESTIMATE_MIN;
			if (estimate > UINT32_MAX)
				estimate = UINT32_MAX;
			payload = (uint32_t)estimate;
		} else {
			payload = device_capacity_max;
		}

		/* Tells the caller the size was estimated, not read. */
		changed |= UVC_FIXUP_ESTIMATED;
	}

	/* The corrected payload size goes back into the record. */
	uvc_put32(record, UVC_PROBE_MAX_PAYLOAD, payload);

	/* Succeeded: what was changed. */
	return changed;
}

/*
 * Decodes one isochronous IN endpoint into an alternate entry.
 *
 * At high speed bits 12:11 of wMaxPacketSize count the extra
 * transactions an interval; at SuperSpeed the companion's
 * wBytesPerInterval is the capacity.  The host takes an endpoint at full
 * speed with one transaction an interval or at high speed with up to
 * three (an extra count of 3 is reserved), a packet of at most 1024
 * bytes and a bInterval of 1 to 16.
 *
 * Value(s) referenced: USB 2.0 §9.6.6 (wMaxPacketSize bits 10:0 and
 * 12:11) and USB 3.2 §9.6.7 (wBytesPerInterval).
 */
void
uvc_alternate_describe(
	struct uvc_alternate *alternate,
	uint8_t setting,
	uint8_t endpoint_address,
	uint16_t max_packet_field,
	uint8_t interval,
	enum uvc_speed speed,
	uint32_t superspeed_bytes)
{
	unsigned extra;

	/* The endpoint as the descriptor gives it. */
	kern_memset(alternate, 0, sizeof(*alternate));
	alternate->setting = setting;
	alternate->endpoint_address = endpoint_address;
	alternate->interval = interval;
	alternate->max_packet_field = max_packet_field;
	alternate->packet_bytes = (uint16_t)(max_packet_field & UVC_PACKET_SIZE_MASK);

	/* The transactions an interval: extra ones only at high speed. */
	extra = 0U;
	if (speed == UVC_SPEED_HIGH)
		extra = (max_packet_field >> UVC_PACKET_EXTRA_SHIFT) & UVC_PACKET_EXTRA_MASK;
	alternate->transactions = (uint8_t)(extra + 1U);

	/* The bytes an interval carries. */
	if (speed >= UVC_SPEED_SUPER) {
		alternate->capacity = superspeed_bytes;
	} else {
		alternate->capacity = (uint32_t)alternate->packet_bytes * alternate->transactions;
	}

	/* The host takes only what its endpoint encoder takes. */
	alternate->accepted = 0U;
	if (speed != UVC_SPEED_FULL && speed != UVC_SPEED_HIGH)
		return;
	if ((max_packet_field & UVC_PACKET_RESERVED_MASK) != 0U)
		return;
	if (speed == UVC_SPEED_FULL && ((max_packet_field >> UVC_PACKET_EXTRA_SHIFT) & UVC_PACKET_EXTRA_MASK) != 0U)
		return;
	if (extra == UVC_PACKET_EXTRA_RESERVED)
		return;
	if (alternate->packet_bytes == 0U || alternate->packet_bytes > UVC_HOST_PACKET_MAX)
		return;
	if (interval == 0U || interval > UVC_HOST_INTERVAL_MAX)
		return;

	/* Succeeded: the host takes the alternate. */
	alternate->accepted = 1U;
}

/*
 * Reports the largest capacity among the alternates the host takes.
 */
uint32_t
uvc_alternate_host_max(
	const struct uvc_alternate *alternates,
	unsigned count)
{
	uint32_t largest;
	unsigned index;

	/* The largest capacity of an accepted alternate. */
	largest = 0U;
	for (index = 0U; index < count; index++) {
		if (alternates[index].accepted == 0U)
			continue;
		if (alternates[index].capacity > largest)
			largest = alternates[index].capacity;
	}

	/* Succeeded: 0 when the host takes none. */
	return largest;
}

/*
 * Reports the largest capacity among all the device's alternates.
 */
uint32_t
uvc_alternate_device_max(
	const struct uvc_alternate *alternates,
	unsigned count)
{
	uint32_t largest;
	unsigned index;

	/* The largest capacity of any alternate. */
	largest = 0U;
	for (index = 0U; index < count; index++) {
		if (alternates[index].capacity > largest)
			largest = alternates[index].capacity;
	}

	/* Succeeded: 0 without an alternate. */
	return largest;
}

/*
 * Chooses the smallest alternate the host takes whose capacity holds a
 * payload.
 */
int
uvc_alternate_choose(
	const struct uvc_alternate *alternates,
	unsigned count,
	uint32_t payload_bytes,
	unsigned *position)
{
	unsigned chosen;
	unsigned found;
	unsigned index;

	/* The smallest accepted alternate that is large enough. */
	found = 0U;
	chosen = 0U;
	for (index = 0U; index < count; index++) {
		if (alternates[index].accepted == 0U)
			continue;
		if (alternates[index].capacity < payload_bytes)
			continue;
		if (found != 0U && alternates[index].capacity >= alternates[chosen].capacity)
			continue;
		chosen = index;
		found = 1U;
	}

	/* No alternate the host takes carries the payload. */
	if (found == 0U)
		return ENOSPC;

	/* Succeeded: the alternate to select. */
	*position = chosen;
	return 0;
}

/*
 * Reads the VS input header: the formats it declares, the video
 * endpoint and the terminal it is linked to.
 *
 * Value(s) referenced: USB Video Class 1.1 §3.9.2.1 (bNumFormats at 3,
 * bEndpointAddress at 6, bTerminalLink at 8).
 */
static int
uvc_desc_input_header(
	struct uvc_stream_desc *stream,
	const uint8_t *descriptor,
	size_t length)
{
	/* The header's fixed part must be there. */
	if (length < UVC_VS_INPUT_HEADER_LENGTH)
		return EINVAL;

	/* The fields the driver uses. */
	stream->has_header = 1U;
	stream->declared_formats = descriptor[3];
	stream->endpoint_address = descriptor[6];
	stream->terminal_link = descriptor[8];

	/* Succeeded: the header is read. */
	return 0;
}

/*
 * Reads a format descriptor and opens a new format for the frames that
 * follow it.
 *
 * Value(s) referenced: USB Video Class Uncompressed Payload 1.1 §3.1.1
 * (bFormatIndex 3, bNumFrameDescriptors 4, guidFormat 5 to 20,
 * bBitsPerPixel 21, bDefaultFrameIndex 22) and USB Video Class
 * Motion-JPEG Payload 1.1 §3.1.1 (bFormatIndex 3, bNumFrameDescriptors 4,
 * bmFlags 5, bDefaultFrameIndex 6; there is no GUID).
 */
static int
uvc_desc_format(
	struct uvc_stream_desc *stream,
	const uint8_t *descriptor,
	size_t length,
	enum uvc_format_kind kind)
{
	struct uvc_format *format;

	/* The format's fixed part must be there. */
	if (kind == UVC_FORMAT_UNCOMPRESSED && length < UVC_FORMAT_UNCOMPRESSED_LENGTH)
		return EINVAL;
	if (kind == UVC_FORMAT_MJPEG && length < UVC_FORMAT_MJPEG_LENGTH)
		return EINVAL;

	/* A format beyond the table is left out, and so are its frames. */
	if (stream->format_count >= UVC_FORMATS_MAX) {
		stream->ignored++;
		return 0;
	}

	/* Opens the format. */
	format = &stream->formats[stream->format_count];
	kern_memset(format, 0, sizeof(*format));
	format->index = descriptor[3];
	format->kind = kind;
	format->declared_frames = descriptor[4];

	/* An uncompressed format: its GUID names the pixels. */
	if (kind == UVC_FORMAT_UNCOMPRESSED) {
		kern_memcpy(format->guid, descriptor + 5U, sizeof(format->guid));
		format->bits_per_pixel = descriptor[21];
		format->default_frame = descriptor[22];
		format->cam_format = uvc_desc_guid_format(format->guid);
	} else {
		format->bits_per_pixel = 0U;
		format->default_frame = descriptor[6];
		format->cam_format = CAM_FORMAT_MJPEG;
	}

	/* The format is kept. */
	stream->format_count++;

	/* Succeeded: frames that follow belong to it. */
	return 0;
}

/*
 * Reads a frame descriptor into the last format of its kind.
 *
 * Value(s) referenced: USB Video Class Uncompressed Payload 1.1 §3.1.2
 * and USB Video Class Motion-JPEG Payload 1.1 §3.1.2 (bFrameIndex 3,
 * bmCapabilities 4, wWidth 5, wHeight 7, dwMaxVideoFrameBufferSize 17,
 * dwDefaultFrameInterval 21, bFrameIntervalType 25; then
 * dwMinFrameInterval 26, dwMaxFrameInterval 30 and dwFrameIntervalStep
 * 34, or dwFrameInterval[n] from 26).
 */
static int
uvc_desc_frame(
	struct uvc_stream_desc *stream,
	const uint8_t *descriptor,
	size_t length,
	enum uvc_format_kind kind)
{
	struct uvc_format *format;
	struct uvc_frame *frame;
	unsigned declared;
	unsigned index;
	size_t needed;

	/* The frame's fixed part must be there. */
	if (length < UVC_FRAME_FIXED_LENGTH)
		return EINVAL;

	/* The intervals must be there as well. */
	declared = descriptor[25];
	if (declared == 0U) {
		needed = UVC_FRAME_CONTINUOUS_LENGTH;
	} else {
		needed = UVC_FRAME_FIXED_LENGTH + 4U * (size_t)declared;
	}

	/* A frame whose intervals are cut off stops the parsing. */
	if (length < needed)
		return EINVAL;

	/* The format it belongs to: the last one, and of the same kind. */
	format = uvc_desc_last_format(stream);
	if (format == NULL || format->kind != kind) {
		stream->ignored++;
		return 0;
	}

	/* A frame beyond the table is left out. */
	if (format->frame_count >= UVC_FRAMES_MAX) {
		stream->dropped_frames++;
		return 0;
	}

	/* Records the frame's size and buffer. */
	frame = &format->frames[format->frame_count];
	kern_memset(frame, 0, sizeof(*frame));
	frame->index = descriptor[3];
	frame->capabilities = descriptor[4];
	frame->width = uvc_get16(descriptor, 5U);
	frame->height = uvc_get16(descriptor, 7U);
	frame->max_frame_bytes = uvc_get32(descriptor, 17U);
	frame->default_interval = uvc_get32(descriptor, 21U);
	frame->interval_type = (uint8_t)declared;

	/* A continuous range of intervals. */
	if (declared == 0U) {
		frame->interval_min = uvc_get32(descriptor, 26U);
		frame->interval_max = uvc_get32(descriptor, 30U);
		frame->interval_step = uvc_get32(descriptor, 34U);
		format->frame_count++;
		return 0;
	}

	/* The discrete intervals, as many as the table holds. */
	for (index = 0U; index < declared; index++) {
		if (index >= UVC_INTERVALS_MAX) {
			stream->dropped_intervals++;
			continue;
		}

		/* Keeps the interval the descriptor lists. */
		frame->intervals[index] = uvc_get32(descriptor, UVC_FRAME_FIXED_LENGTH + 4U * (size_t)index);
		frame->interval_count++;
	}

	/* The frame is kept. */
	format->frame_count++;

	/* Succeeded: the frame is read. */
	return 0;
}

/*
 * Reads a colour matching descriptor into the last format.
 *
 * Value(s) referenced: USB Video Class 1.1 §3.9.2.6 (bColorPrimaries 3,
 * bTransferCharacteristics 4, bMatrixCoefficients 5).
 */
static int
uvc_desc_colorformat(
	struct uvc_stream_desc *stream,
	const uint8_t *descriptor,
	size_t length)
{
	struct uvc_format *format;

	/* The descriptor's fields must be there. */
	if (length < UVC_COLORFORMAT_LENGTH)
		return EINVAL;

	/* A colour descriptor before any format describes nothing. */
	format = uvc_desc_last_format(stream);
	if (format == NULL) {
		stream->ignored++;
		return 0;
	}

	/* The colour of the format's frames. */
	format->has_color = 1U;
	format->color_primaries = descriptor[3];
	format->color_transfer = descriptor[4];
	format->color_matrix = descriptor[5];

	/* Succeeded: the colour is recorded. */
	return 0;
}

/*
 * Reads a unit's bmControls: bControlSize at size_offset, the bitmap
 * after it, at most four bytes and no further than the descriptor.
 */
static uint32_t
uvc_desc_bitmap(
	const uint8_t *descriptor,
	size_t length,
	size_t size_offset)
{
	uint32_t bitmap;
	size_t count;
	size_t index;

	/* No size field, no controls. */
	if (length <= size_offset)
		return 0U;

	/* The bytes the descriptor declares and holds, little endian. */
	count = descriptor[size_offset];
	if (count > 4U)
		count = 4U;
	bitmap = 0U;
	for (index = 0U; index < count; index++) {
		if (size_offset + 1U + index >= length)
			break;
		bitmap |= (uint32_t)descriptor[size_offset + 1U + index] << (8U * index);
	}

	/* Succeeded: the controls' bits. */
	return bitmap;
}

/* Finds the format opened last, or NULL. */
static struct uvc_format *
uvc_desc_last_format(
	struct uvc_stream_desc *stream)
{
	/* No format is open yet. */
	if (stream->format_count == 0U)
		return NULL;

	/* Succeeded: the last format. */
	return &stream->formats[stream->format_count - 1U];
}
