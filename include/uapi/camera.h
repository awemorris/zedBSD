/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The /dev/camN requests: formats, modes, buffers, streaming and frame
 * metadata of a camera.
 *
 * Every record starts with a version and its own size and keeps its
 * reserved fields zero.  No record holds a pointer, every 64-bit field is
 * aligned to 8 bytes, and the layout is fixed by the static assertions at
 * the end, so the same binary layout serves every architecture.
 */

#ifndef KERN_UAPI_CAMERA_H
#define KERN_UAPI_CAMERA_H

#include <stdint.h>
#include <uapi/ioctl.h>

/* The ioctl group of the camera nodes. */
#define CAM_IOC_GROUP			'C'

/* The record version every request carries. */
#define CAM_ABI_VERSION			1U

/* The number of camera nodes, cam0 to cam3. */
#define CAM_DEVICES_MAX			4U

/* The most buffers one open may ask for. */
#define CAM_BUFFERS_MAX			8U

/* The intervals CAM_ENUM_MODE reports inline; the rest come from CAM_ENUM_INTERVAL. */
#define CAM_MODE_INTERVALS_INLINE	8U

/*
 * Pixel formats.
 *
 * RAW is a Bayer mosaic; the suffix P is the packed form.  YUYV is two
 * pixels in four bytes, Y0 U Y1 V; UYVY is U Y0 V Y1; NV12 is a Y plane
 * followed by an interleaved UV plane.  A compressed format has frames
 * of varying length, reported in used_bytes; MJPEG is one JPEG image
 * from SOI to EOI per frame, which may lack its Huffman tables.
 */
#define CAM_FORMAT_RAW8			0x0101U
#define CAM_FORMAT_RAW10P		0x0102U
#define CAM_FORMAT_RAW10		0x0103U
#define CAM_FORMAT_RAW12P		0x0104U
#define CAM_FORMAT_RAW12		0x0105U
#define CAM_FORMAT_RAW16		0x0106U
#define CAM_FORMAT_YUYV			0x0201U
#define CAM_FORMAT_UYVY			0x0202U
#define CAM_FORMAT_NV12			0x0203U
#define CAM_FORMAT_YUV420P		0x0204U
#define CAM_FORMAT_RGB565		0x0301U
#define CAM_FORMAT_RGB888		0x0302U
#define CAM_FORMAT_XRGB8888		0x0303U
#define CAM_FORMAT_MJPEG		0x0401U
#define CAM_FORMAT_H264			0x0402U

/* The capability bits of cam_info. */
#define CAM_CAP_RAW			0x00000001U
#define CAM_CAP_YUV			0x00000002U
#define CAM_CAP_RGB			0x00000004U
#define CAM_CAP_COMPRESSED		0x00000008U
#define CAM_CAP_IMPORT_GPU		0x00000010U
#define CAM_CAP_EXPORT			0x00000020U
#define CAM_CAP_CONTROLS		0x00000040U

/* The flags of cam_format_desc. */
#define CAM_FORMAT_SUPPORTED		0x00000001U
#define CAM_FORMAT_COMPRESSED		0x00000002U

/* How a mode states its frame intervals. */
#define CAM_INTERVAL_DISCRETE		1U
#define CAM_INTERVAL_CONTINUOUS		2U

/* The Bayer orders of a RAW format. */
#define CAM_BAYER_NONE			0U
#define CAM_BAYER_RGGB			1U
#define CAM_BAYER_GRBG			2U
#define CAM_BAYER_GBRG			3U
#define CAM_BAYER_BGGR			4U

/* Where the memory of the buffers comes from. */
#define CAM_MEMORY_DEVICE		1U
#define CAM_MEMORY_IMPORT		2U

/* The state bits of cam_buffer. */
#define CAM_BUFFER_QUEUED		0x00000001U
#define CAM_BUFFER_ACTIVE		0x00000002U
#define CAM_BUFFER_DONE			0x00000004U
#define CAM_BUFFER_MAPPED		0x00000008U
#define CAM_BUFFER_IMPORTED		0x00000010U

/*
 * The flags of a frame.
 *
 * ERROR: the source saw an error while the frame was gathered (a payload
 * error bit, a missed or failed packet, a broken header, an overflow, a
 * compressed frame without its start marker).  INCOMPLETE: the frame is
 * likely short (an uncompressed frame shorter than the image, whose
 * remaining lines keep an older frame's pixels, or a compressed frame
 * without its end marker).  PTS and SCR tell that device_pts and
 * device_scr hold the device's clock values.  APPROXIMATE tells that the
 * timestamp is when the host noticed the frame, not when it was taken.
 */
#define CAM_FRAME_ERROR			0x00000001U
#define CAM_FRAME_INCOMPLETE		0x00000002U
#define CAM_METADATA_PTS		0x00000100U
#define CAM_METADATA_SCR		0x00000200U
#define CAM_METADATA_APPROXIMATE	0x00000400U

/* The value of an exposure or gain the source does not know. */
#define CAM_METADATA_UNKNOWN		0xffffffffU

/* The flags of cam_control_info. */
#define CAM_CONTROL_UNSUPPORTED		0x00000001U
#define CAM_CONTROL_READ_ONLY		0x00000002U
#define CAM_CONTROL_BOOLEAN		0x00000004U

/*
 * The control identifiers.
 *
 * 1 to 10 belong to image sensors and 32 onward to the processing unit
 * and the camera terminal of a USB camera.
 */
#define CAM_CONTROL_SENSOR_EXPOSURE	1U
#define CAM_CONTROL_SENSOR_GAIN		2U
#define CAM_CONTROL_SENSOR_HFLIP	3U
#define CAM_CONTROL_SENSOR_VFLIP	4U
#define CAM_CONTROL_SENSOR_TEST_PATTERN	5U
#define CAM_CONTROL_SENSOR_AUTO_EXPOSURE 6U
#define CAM_CONTROL_SENSOR_AUTO_GAIN	7U
#define CAM_CONTROL_BRIGHTNESS		32U
#define CAM_CONTROL_CONTRAST		33U
#define CAM_CONTROL_HUE			34U
#define CAM_CONTROL_SATURATION		35U
#define CAM_CONTROL_SHARPNESS		36U
#define CAM_CONTROL_GAMMA		37U
#define CAM_CONTROL_WHITE_BALANCE	38U
#define CAM_CONTROL_WHITE_BALANCE_AUTO	39U
#define CAM_CONTROL_BACKLIGHT		40U
#define CAM_CONTROL_GAIN		41U
#define CAM_CONTROL_POWER_LINE		42U
#define CAM_CONTROL_EXPOSURE		48U
#define CAM_CONTROL_EXPOSURE_AUTO	49U
#define CAM_CONTROL_FOCUS		50U
#define CAM_CONTROL_FOCUS_AUTO		51U
#define CAM_CONTROL_ZOOM		52U

/*
 * What a camera is: the driver, the product, where it is attached, what
 * it can deliver and how much one payload transfer of the host may carry.
 *
 * host_payload_max is the largest payload transfer the transport takes in
 * one service interval, to be compared with a format's payload_bytes; 0
 * means no such limit (a bulk transport).
 */
struct cam_info {
	uint32_t version;
	uint32_t size;
	char driver[16];
	char name[64];
	char location[32];
	uint32_t capabilities;
	uint32_t max_buffers;
	uint32_t format_count;
	uint32_t clock_hz;
	uint32_t host_payload_max;
	uint32_t reserved[3];
};

/*
 * One pixel format of a camera, asked for by index.
 *
 * A pixel group is the smallest run of whole pixels in whole bytes (for
 * YUYV two pixels in four bytes).
 */
struct cam_format_desc {
	uint32_t version;
	uint32_t size;
	uint32_t index;
	uint32_t format;
	uint32_t flags;
	uint32_t bits_per_pixel;
	uint32_t bytes_per_group;
	uint32_t pixels_per_group;
	uint32_t mode_count;
	uint32_t reserved[3];
};

/*
 * One frame size of a format, asked for by format and index, with its
 * frame intervals in units of 100 ns.
 *
 * A discrete mode reports up to eight intervals here, the default first,
 * and interval_count tells how many there are in all.  A continuous mode
 * reports the range.  The RAW fields describe a sensor's native readout.
 */
struct cam_mode_desc {
	uint32_t version;
	uint32_t size;
	uint32_t format;
	uint32_t index;
	uint32_t width;
	uint32_t height;
	uint32_t interval_type;
	uint32_t interval_count;
	uint32_t intervals[CAM_MODE_INTERVALS_INLINE];
	uint32_t interval_min;
	uint32_t interval_max;
	uint32_t interval_step;
	uint32_t max_frame_bytes;
	uint32_t bayer_order;
	uint32_t native_format;
	uint32_t crop_x;
	uint32_t crop_y;
	uint32_t crop_width;
	uint32_t crop_height;
	uint32_t reserved[2];
};

/*
 * One interval of a discrete mode beyond the eight CAM_ENUM_MODE reports.
 */
struct cam_interval_desc {
	uint32_t version;
	uint32_t size;
	uint32_t format;
	uint32_t mode_index;
	uint32_t index;
	uint32_t interval_100ns;
	uint32_t reserved[2];
};

/*
 * The format a stream is to use or uses.
 *
 * The caller gives the format, the frame size and the interval (0 for the
 * mode's default); the camera answers the interval it will use, the line
 * stride (0 for a compressed format), the largest frame, and the payload
 * transfer the device needs per service interval.
 */
struct cam_format {
	uint32_t version;
	uint32_t size;
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
	uint32_t reserved[2];
};

/*
 * A request for the buffers of a stream: how many and of what memory;
 * the answer is how many were allocated and the size of each.
 */
struct cam_request_buffers {
	uint32_t version;
	uint32_t size;
	uint32_t count;
	uint32_t memory;
	uint32_t bytes;
	uint32_t reserved[3];
};

/*
 * What a finished frame carries besides its pixels.
 *
 * sequence counts every frame the source began, including those dropped
 * for want of a buffer, so a gap in it is the number of frames lost.
 */
struct cam_frame_metadata {
	uint64_t timestamp_ns;
	uint64_t device_scr;
	uint32_t sequence;
	uint32_t used_bytes;
	uint32_t device_pts;
	uint32_t flags;
	uint32_t exposure;
	uint32_t analogue_gain;
};

/*
 * One buffer, for asking where it is, queueing it and taking it back.
 *
 * fd is the memory to import on the first queue of an imported buffer
 * and -1 otherwise.  timeout_ms bounds a dequeue's wait; 0 waits without
 * a limit.
 */
struct cam_buffer {
	uint32_t version;
	uint32_t size;
	uint32_t index;
	uint32_t memory;
	uint32_t flags;
	uint32_t bytes;
	uint64_t mmap_offset;
	int32_t fd;
	uint32_t timeout_ms;
	struct cam_frame_metadata metadata;
	uint32_t reserved[2];
};

/*
 * The range of one control.
 */
struct cam_control_info {
	uint32_t version;
	uint32_t size;
	uint32_t id;
	uint32_t flags;
	int64_t minimum;
	int64_t maximum;
	int64_t step;
	int64_t default_value;
	char name[32];
	uint32_t reserved[2];
};

/*
 * The value of one control.
 */
struct cam_control {
	uint32_t version;
	uint32_t size;
	uint32_t id;
	uint32_t reserved;
	int64_t value;
};

/*
 * What a camera counted since it was attached.
 *
 * The frame counts come from the framework, the transport counts from
 * the source.  no_signal counts the times no frame came for longer than
 * the source's watchdog allows; last_frame_age_ms is how long ago the
 * last frame finished.
 */
struct cam_counters {
	uint32_t version;
	uint32_t size;
	uint64_t frames;
	uint64_t dropped_no_buffer;
	uint64_t dropped_overflow;
	uint64_t dropped_error;
	uint64_t dropped_transport;
	uint64_t invalid_header;
	uint64_t short_header;
	uint64_t missed_packets;
	uint64_t packet_errors;
	uint64_t sequence_gaps;
	uint64_t no_signal;
	uint64_t resubmit_failures;
	uint64_t halt_timeouts;
	uint32_t last_frame_age_ms;
	uint32_t reserved;
};

/* The requests of a camera node. */
#define CAM_GET_INFO		_IOWR(CAM_IOC_GROUP, 1, struct cam_info)
#define CAM_ENUM_FORMAT		_IOWR(CAM_IOC_GROUP, 2, struct cam_format_desc)
#define CAM_ENUM_MODE		_IOWR(CAM_IOC_GROUP, 3, struct cam_mode_desc)
#define CAM_GET_FORMAT		_IOWR(CAM_IOC_GROUP, 4, struct cam_format)
#define CAM_SET_FORMAT		_IOWR(CAM_IOC_GROUP, 5, struct cam_format)
#define CAM_TRY_FORMAT		_IOWR(CAM_IOC_GROUP, 6, struct cam_format)
#define CAM_REQUEST_BUFFERS	_IOWR(CAM_IOC_GROUP, 7, struct cam_request_buffers)
#define CAM_QUERY_BUFFER	_IOWR(CAM_IOC_GROUP, 8, struct cam_buffer)
#define CAM_QUEUE_BUFFER	_IOWR(CAM_IOC_GROUP, 9, struct cam_buffer)
#define CAM_DEQUEUE_BUFFER	_IOWR(CAM_IOC_GROUP, 10, struct cam_buffer)
#define CAM_STREAM_ON		_IO(CAM_IOC_GROUP, 11)
#define CAM_STREAM_OFF		_IO(CAM_IOC_GROUP, 12)
#define CAM_GET_CONTROL_INFO	_IOWR(CAM_IOC_GROUP, 13, struct cam_control_info)
#define CAM_GET_CONTROL		_IOWR(CAM_IOC_GROUP, 14, struct cam_control)
#define CAM_SET_CONTROL		_IOWR(CAM_IOC_GROUP, 15, struct cam_control)
#define CAM_GET_COUNTERS	_IOWR(CAM_IOC_GROUP, 16, struct cam_counters)
#define CAM_EXPORT_BUFFER	_IOWR(CAM_IOC_GROUP, 17, struct cam_buffer)
#define CAM_ENUM_INTERVAL	_IOWR(CAM_IOC_GROUP, 18, struct cam_interval_desc)

/*
 * The layout of every record is part of the ABI.  These sizes change only
 * with CAM_ABI_VERSION.
 */
_Static_assert(sizeof(struct cam_info) == 152, "cam_info layout");
_Static_assert(sizeof(struct cam_format_desc) == 48, "cam_format_desc layout");
_Static_assert(sizeof(struct cam_mode_desc) == 112, "cam_mode_desc layout");
_Static_assert(sizeof(struct cam_interval_desc) == 32, "cam_interval_desc layout");
_Static_assert(sizeof(struct cam_format) == 56, "cam_format layout");
_Static_assert(sizeof(struct cam_request_buffers) == 32, "cam_request_buffers layout");
_Static_assert(sizeof(struct cam_frame_metadata) == 40, "cam_frame_metadata layout");
_Static_assert(sizeof(struct cam_buffer) == 88, "cam_buffer layout");
_Static_assert(sizeof(struct cam_control_info) == 88, "cam_control_info layout");
_Static_assert(sizeof(struct cam_control) == 24, "cam_control layout");
_Static_assert(sizeof(struct cam_counters) == 120, "cam_counters layout");

#endif
