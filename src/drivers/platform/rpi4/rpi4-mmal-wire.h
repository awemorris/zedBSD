/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The messages of MMAL, the VideoCore firmware's multimedia service, as
 * they travel over VCHIQ.
 *
 * The byte layout is the one the Raspberry Pi userland's BSD-3-Clause
 * headers give (interface/mmal/vc/mmal_vc_msgs.h and the MMAL structures
 * it embeds) when compiled for 32-bit ARM, where every pointer is four
 * bytes and 64-bit fields are eight-byte aligned; the firmware reads that
 * layout.  zedBSD writes each field at its byte offset, so a 64-bit kernel
 * produces the same bytes; the pointer fields carry opaque 32-bit values
 * the firmware echoes back.  The names and the code are zedBSD's own.
 */

#ifndef KERN_DRIVERS_RPI4_MMAL_WIRE_H
#define KERN_DRIVERS_RPI4_MMAL_WIRE_H

#include <stddef.h>
#include <stdint.h>

/* The VCHIQ service: "mmal", and the worker versions this client speaks. */
#define DRV_RPI4_MMAL_SERVICE_FOURCC		0x6d6d616cU
#define DRV_RPI4_MMAL_VERSION			16U
#define DRV_RPI4_MMAL_VERSION_MIN		10U

/* The magic every message and buffer carries: "mmal" read little endian. */
#define DRV_RPI4_MMAL_MAGIC			0x6c616d6dU

/* The worker message ids. */
#define DRV_RPI4_MMAL_MSG_GET_VERSION		3U
#define DRV_RPI4_MMAL_MSG_COMPONENT_CREATE	4U
#define DRV_RPI4_MMAL_MSG_COMPONENT_DESTROY	5U
#define DRV_RPI4_MMAL_MSG_COMPONENT_ENABLE	6U
#define DRV_RPI4_MMAL_MSG_COMPONENT_DISABLE	7U
#define DRV_RPI4_MMAL_MSG_PORT_INFO_GET		8U
#define DRV_RPI4_MMAL_MSG_PORT_INFO_SET		9U
#define DRV_RPI4_MMAL_MSG_PORT_ACTION		10U
#define DRV_RPI4_MMAL_MSG_BUFFER_FROM_HOST	11U
#define DRV_RPI4_MMAL_MSG_BUFFER_TO_HOST	12U
#define DRV_RPI4_MMAL_MSG_PORT_PARAMETER_SET	14U
#define DRV_RPI4_MMAL_MSG_EVENT_TO_HOST		16U
#define DRV_RPI4_MMAL_MSG_PORT_FLUSH		25U

/* The largest message, and the space the client keeps for one. */
#define DRV_RPI4_MMAL_MAX_MESSAGE		512U

/* The port actions. */
#define DRV_RPI4_MMAL_ACTION_ENABLE		1U
#define DRV_RPI4_MMAL_ACTION_DISABLE		2U
#define DRV_RPI4_MMAL_ACTION_FLUSH		3U

/* The port types. */
#define DRV_RPI4_MMAL_PORT_CONTROL		1U
#define DRV_RPI4_MMAL_PORT_INPUT		2U
#define DRV_RPI4_MMAL_PORT_OUTPUT		3U

/* The elementary stream type of video. */
#define DRV_RPI4_MMAL_ES_VIDEO			3U

/* Four-character codes, read little endian: the encodings and the format-changed event. */
#define DRV_RPI4_MMAL_ENCODING_H264		0x34363248U
#define DRV_RPI4_MMAL_ENCODING_NV12		0x3231564eU
#define DRV_RPI4_MMAL_ENCODING_I420		0x30323449U
#define DRV_RPI4_MMAL_EVENT_FORMAT_CHANGED	0x48434645U

/* The buffer flags this client uses. */
#define DRV_RPI4_MMAL_FLAG_EOS			0x0001U
#define DRV_RPI4_MMAL_FLAG_FRAME_START		0x0002U
#define DRV_RPI4_MMAL_FLAG_FRAME_END		0x0004U
#define DRV_RPI4_MMAL_FLAG_KEYFRAME		0x0008U
#define DRV_RPI4_MMAL_FLAG_CONFIG		0x0020U
#define DRV_RPI4_MMAL_FLAG_CORRUPTED		0x0200U
#define DRV_RPI4_MMAL_FLAG_TRANSMISSION_FAILED	0x0400U

/* The time stamp that says "unknown". */
#define DRV_RPI4_MMAL_TIME_UNKNOWN		((int64_t)((uint64_t)1U << 63))

/* The common header: magic, id, control service, waiter, status, padding. */
#define DRV_RPI4_MMAL_HEADER_BYTES		24U
#define DRV_RPI4_MMAL_HEADER_MAGIC		0U
#define DRV_RPI4_MMAL_HEADER_MSGID		4U
#define DRV_RPI4_MMAL_HEADER_WAITER		12U
#define DRV_RPI4_MMAL_HEADER_STATUS		16U

/* The version reply: flags, major, minor, minimum. */
#define DRV_RPI4_MMAL_VERSION_BYTES		40U
#define DRV_RPI4_MMAL_VERSION_MAJOR		28U
#define DRV_RPI4_MMAL_VERSION_MINOR		32U

/* The component create request and its reply. */
#define DRV_RPI4_MMAL_CREATE_BYTES		160U
#define DRV_RPI4_MMAL_CREATE_CLIENT		24U
#define DRV_RPI4_MMAL_CREATE_NAME		28U
#define DRV_RPI4_MMAL_CREATE_NAME_BYTES		128U
#define DRV_RPI4_MMAL_CREATE_REPLY_BYTES	44U
#define DRV_RPI4_MMAL_CREATE_REPLY_STATUS	24U
#define DRV_RPI4_MMAL_CREATE_REPLY_HANDLE	28U
#define DRV_RPI4_MMAL_CREATE_REPLY_INPUTS	32U
#define DRV_RPI4_MMAL_CREATE_REPLY_OUTPUTS	36U

/* The requests that name only a component (destroy, enable, disable). */
#define DRV_RPI4_MMAL_COMPONENT_BYTES		28U
#define DRV_RPI4_MMAL_COMPONENT_HANDLE		24U

/* The plain reply: the header and a status. */
#define DRV_RPI4_MMAL_REPLY_BYTES		28U
#define DRV_RPI4_MMAL_REPLY_STATUS		24U

/* The port info request: the component, the port type and the index. */
#define DRV_RPI4_MMAL_INFO_GET_BYTES		36U
#define DRV_RPI4_MMAL_INFO_GET_COMPONENT	24U
#define DRV_RPI4_MMAL_INFO_GET_TYPE		28U
#define DRV_RPI4_MMAL_INFO_GET_INDEX		32U

/* The port info reply: status, component, type, index, found, port handle, then the port, format, es and extra data. */
#define DRV_RPI4_MMAL_INFO_BYTES		316U
#define DRV_RPI4_MMAL_INFO_STATUS		24U
#define DRV_RPI4_MMAL_INFO_FOUND		40U
#define DRV_RPI4_MMAL_INFO_PORT_HANDLE		44U
#define DRV_RPI4_MMAL_INFO_PORT			48U

/* The port info set request: component, type, index, then the port, format, es and extra data. */
#define DRV_RPI4_MMAL_INFO_SET_BYTES		304U
#define DRV_RPI4_MMAL_INFO_SET_COMPONENT	24U
#define DRV_RPI4_MMAL_INFO_SET_TYPE		28U
#define DRV_RPI4_MMAL_INFO_SET_INDEX		32U
#define DRV_RPI4_MMAL_INFO_SET_PORT		36U

/*
 * The port, format and es blocks, which both port info messages carry in
 * the same order, and their fields as offsets inside the blocks.
 */
#define DRV_RPI4_MMAL_PORT_BYTES		64U
#define DRV_RPI4_MMAL_PORT_BUFFER_NUM_MIN	24U
#define DRV_RPI4_MMAL_PORT_BUFFER_SIZE_MIN	28U
#define DRV_RPI4_MMAL_PORT_BUFFER_NUM_RECOMMENDED	36U
#define DRV_RPI4_MMAL_PORT_BUFFER_SIZE_RECOMMENDED	40U
#define DRV_RPI4_MMAL_PORT_BUFFER_NUM		44U
#define DRV_RPI4_MMAL_PORT_BUFFER_SIZE		48U
#define DRV_RPI4_MMAL_FORMAT_BYTES		32U
#define DRV_RPI4_MMAL_FORMAT_TYPE		0U
#define DRV_RPI4_MMAL_FORMAT_ENCODING		4U
#define DRV_RPI4_MMAL_FORMAT_VARIANT		8U
#define DRV_RPI4_MMAL_FORMAT_BITRATE		16U
#define DRV_RPI4_MMAL_FORMAT_FLAGS		20U
#define DRV_RPI4_MMAL_FORMAT_EXTRADATA_SIZE	24U
#define DRV_RPI4_MMAL_ES_BYTES			44U
#define DRV_RPI4_MMAL_ES_WIDTH			0U
#define DRV_RPI4_MMAL_ES_HEIGHT			4U
#define DRV_RPI4_MMAL_ES_CROP_X			8U
#define DRV_RPI4_MMAL_ES_CROP_Y			12U
#define DRV_RPI4_MMAL_ES_CROP_WIDTH		16U
#define DRV_RPI4_MMAL_ES_CROP_HEIGHT		20U
#define DRV_RPI4_MMAL_ES_FRAME_RATE_NUM		24U
#define DRV_RPI4_MMAL_ES_FRAME_RATE_DEN		28U
#define DRV_RPI4_MMAL_ES_PAR_NUM		32U
#define DRV_RPI4_MMAL_ES_PAR_DEN		36U

/* The port action request: component, port, action, and the port block for ENABLE. */
#define DRV_RPI4_MMAL_ACTION_BYTES		100U
#define DRV_RPI4_MMAL_ACTION_COMPONENT		24U
#define DRV_RPI4_MMAL_ACTION_PORT_HANDLE	28U
#define DRV_RPI4_MMAL_ACTION_ACTION		32U
#define DRV_RPI4_MMAL_ACTION_PORT		36U

/* The parameter set request: component, port, the parameter's id and size, then its value. */
#define DRV_RPI4_MMAL_PARAMETER_COMPONENT	24U
#define DRV_RPI4_MMAL_PARAMETER_PORT_HANDLE	28U
#define DRV_RPI4_MMAL_PARAMETER_ID		32U
#define DRV_RPI4_MMAL_PARAMETER_SIZE		36U
#define DRV_RPI4_MMAL_PARAMETER_VALUE		40U

/*
 * The buffer message, both directions: the driver area (magic, component,
 * port, client context), the referenced buffer's driver area, the buffer
 * header (next, private, cmd, data, allocated size, length, offset, flags,
 * pts, dts, type, user data), the video planes, and the payload fields.
 */
#define DRV_RPI4_MMAL_BUFFER_BYTES		296U
#define DRV_RPI4_MMAL_BUFFER_DRIVER_MAGIC	24U
#define DRV_RPI4_MMAL_BUFFER_COMPONENT		28U
#define DRV_RPI4_MMAL_BUFFER_PORT_HANDLE	32U
#define DRV_RPI4_MMAL_BUFFER_CONTEXT		36U
#define DRV_RPI4_MMAL_BUFFER_CMD		64U
#define DRV_RPI4_MMAL_BUFFER_DATA		68U
#define DRV_RPI4_MMAL_BUFFER_ALLOC_SIZE		72U
#define DRV_RPI4_MMAL_BUFFER_LENGTH		76U
#define DRV_RPI4_MMAL_BUFFER_OFFSET		80U
#define DRV_RPI4_MMAL_BUFFER_FLAGS		84U
#define DRV_RPI4_MMAL_BUFFER_PTS		88U
#define DRV_RPI4_MMAL_BUFFER_DTS		96U
#define DRV_RPI4_MMAL_BUFFER_PLANES		112U
#define DRV_RPI4_MMAL_BUFFER_PLANE_OFFSET	116U
#define DRV_RPI4_MMAL_BUFFER_PLANE_PITCH	132U
#define DRV_RPI4_MMAL_BUFFER_ZERO_COPY		152U
#define DRV_RPI4_MMAL_BUFFER_PAYLOAD_IN_MESSAGE	160U
#define DRV_RPI4_MMAL_BUFFER_SHORT_DATA		164U

/* The most bytes of a payload the firmware carries in the buffer message itself (no bulk follows). */
#define DRV_RPI4_MMAL_SHORT_DATA_BYTES		128U

/* The event message: client component, port type, port number, command, length, data. */
#define DRV_RPI4_MMAL_EVENT_BYTES		304U
#define DRV_RPI4_MMAL_EVENT_CLIENT		24U
#define DRV_RPI4_MMAL_EVENT_PORT_TYPE		28U
#define DRV_RPI4_MMAL_EVENT_CMD			36U
#define DRV_RPI4_MMAL_EVENT_LENGTH		40U
#define DRV_RPI4_MMAL_EVENT_DATA		44U
#define DRV_RPI4_MMAL_EVENT_DATA_BYTES		256U

/*
 * The format-changed event's data: four buffer counts and sizes, a
 * pointer, then the new format and es blocks.
 */
#define DRV_RPI4_MMAL_CHANGED_SIZE_MIN		0U
#define DRV_RPI4_MMAL_CHANGED_NUM_MIN		4U
#define DRV_RPI4_MMAL_CHANGED_SIZE_RECOMMENDED	8U
#define DRV_RPI4_MMAL_CHANGED_NUM_RECOMMENDED	12U
#define DRV_RPI4_MMAL_CHANGED_FORMAT		20U
#define DRV_RPI4_MMAL_CHANGED_ES		52U
#define DRV_RPI4_MMAL_CHANGED_BYTES		96U

/* Stores a little-endian 32-bit word at a byte offset. */
void drv_rpi4_mmal_put32(uint8_t *message, size_t offset, uint32_t value);

/* Stores a little-endian 64-bit word at a byte offset. */
void drv_rpi4_mmal_put64(uint8_t *message, size_t offset, uint64_t value);

/* Loads a little-endian 32-bit word from a byte offset. */
uint32_t drv_rpi4_mmal_get32(const uint8_t *message, size_t offset);

/* Loads a little-endian 64-bit word from a byte offset. */
uint64_t drv_rpi4_mmal_get64(const uint8_t *message, size_t offset);

/* Writes the common header of a request. */
void drv_rpi4_mmal_header(uint8_t *message, uint32_t msgid, uint32_t waiter);

/* Builds a component create request. */
size_t drv_rpi4_mmal_build_create(uint8_t *message, uint32_t waiter, uint32_t client, const char *name);

/* Builds a request that names only a component. */
size_t drv_rpi4_mmal_build_component(uint8_t *message, uint32_t msgid, uint32_t waiter, uint32_t component);

/* Builds a port info request. */
size_t drv_rpi4_mmal_build_info_get(uint8_t *message, uint32_t waiter, uint32_t component, uint32_t type, uint32_t index);

/* Builds a port info set request from a port info reply the caller edited. */
size_t drv_rpi4_mmal_build_info_set(uint8_t *message, uint32_t waiter, uint32_t component, const uint8_t *info);

/* Builds a port action request; ENABLE carries the port block of a port info reply. */
size_t drv_rpi4_mmal_build_action(uint8_t *message, uint32_t waiter, uint32_t component, uint32_t port_handle, uint32_t action, const uint8_t *info);

/* Builds a 32-bit parameter set request. */
size_t drv_rpi4_mmal_build_parameter32(uint8_t *message, uint32_t waiter, uint32_t component, uint32_t port_handle, uint32_t id, uint32_t value);

/* Builds a buffer message for a port. */
size_t drv_rpi4_mmal_build_buffer(uint8_t *message, uint32_t msgid, uint32_t waiter, uint32_t component, uint32_t port_handle, uint32_t context, uint32_t alloc_size, uint32_t length, uint32_t flags, int64_t pts);

/*
 * Copies samples of a decoded frame, undoing the firmware's footroom error.
 *
 * The decoder's NV12 output gives every sample whose true value is 2 to 15
 * one less, in every plane; 0 and every value of 16 and above come out
 * exact (T1-629, T1-637: every differing sample of three streams).  So
 * a sample of 1 to 14 is written one more.  A true 1 comes out 0 and stays
 * 0: the firmware writes 0 for both.
 */
void drv_rpi4_mmal_copy_samples(uint8_t *destination, const uint8_t *source, size_t bytes);

#endif
