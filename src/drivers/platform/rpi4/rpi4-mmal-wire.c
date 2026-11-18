/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MMAL messages' byte layout and builders (see rpi4-mmal-wire.h).
 */

#include "drivers/platform/rpi4/rpi4-mmal-wire.h"
#include <kern/kcrt.h>

/*
 * Stores a little-endian 32-bit word at a byte offset.
 */
void
drv_rpi4_mmal_put32(
	uint8_t *message,
	size_t offset,
	uint32_t value)
{
	/* Lowest byte first. */
	message[offset] = (uint8_t)value;
	message[offset + 1U] = (uint8_t)(value >> 8);
	message[offset + 2U] = (uint8_t)(value >> 16);
	message[offset + 3U] = (uint8_t)(value >> 24);
}

/*
 * Stores a little-endian 64-bit word at a byte offset.
 */
void
drv_rpi4_mmal_put64(
	uint8_t *message,
	size_t offset,
	uint64_t value)
{
	/* The low word first, then the high word. */
	drv_rpi4_mmal_put32(message, offset, (uint32_t)value);
	drv_rpi4_mmal_put32(message, offset + 4U, (uint32_t)(value >> 32));
}

/*
 * Loads a little-endian 32-bit word from a byte offset.
 */
uint32_t
drv_rpi4_mmal_get32(
	const uint8_t *message,
	size_t offset)
{
	uint32_t value;

	/* Lowest byte first. */
	value = message[offset];
	value |= (uint32_t)message[offset + 1U] << 8;
	value |= (uint32_t)message[offset + 2U] << 16;
	value |= (uint32_t)message[offset + 3U] << 24;

	/* Succeeded: the word. */
	return value;
}

/*
 * Loads a little-endian 64-bit word from a byte offset.
 */
uint64_t
drv_rpi4_mmal_get64(
	const uint8_t *message,
	size_t offset)
{
	uint64_t value;

	/* The low word, then the high word. */
	value = drv_rpi4_mmal_get32(message, offset);
	value |= (uint64_t)drv_rpi4_mmal_get32(message, offset + 4U) << 32;

	/* Succeeded: the word. */
	return value;
}

/*
 * Writes the common header of a request.
 *
 * The waiter is the client's request number; the firmware copies it into
 * the reply, which is how the reply finds its request.  The control
 * service, the status and the padding are zero.
 */
void
drv_rpi4_mmal_header(
	uint8_t *message,
	uint32_t msgid,
	uint32_t waiter)
{
	/* Clears the header, then fills its three meaningful words. */
	kern_memset(message, 0, DRV_RPI4_MMAL_HEADER_BYTES);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_HEADER_MAGIC, DRV_RPI4_MMAL_MAGIC);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_HEADER_MSGID, msgid);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_HEADER_WAITER, waiter);
}

/*
 * Builds a component create request.
 *
 * client is echoed in the component's events; name is the firmware's
 * component name (for example "ril.video_decode"), cut to 127 bytes.
 */
size_t
drv_rpi4_mmal_build_create(
	uint8_t *message,
	uint32_t waiter,
	uint32_t client,
	const char *name)
{
	size_t index;

	/* The header and a zero body: the name is NUL padded and the pid is zero. */
	kern_memset(message, 0, DRV_RPI4_MMAL_CREATE_BYTES);
	drv_rpi4_mmal_header(message, DRV_RPI4_MMAL_MSG_COMPONENT_CREATE, waiter);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_CREATE_CLIENT, client);

	/* Copies the name, leaving its terminating NUL. */
	for (index = 0; index + 1U < DRV_RPI4_MMAL_CREATE_NAME_BYTES && name[index] != '\0'; index++)
		message[DRV_RPI4_MMAL_CREATE_NAME + index] = (uint8_t)name[index];

	/* Succeeded: the request's length. */
	return DRV_RPI4_MMAL_CREATE_BYTES;
}

/*
 * Builds a request that names only a component.
 */
size_t
drv_rpi4_mmal_build_component(
	uint8_t *message,
	uint32_t msgid,
	uint32_t waiter,
	uint32_t component)
{
	/* The header and the component's handle. */
	drv_rpi4_mmal_header(message, msgid, waiter);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_COMPONENT_HANDLE, component);

	/* Succeeded: the request's length. */
	return DRV_RPI4_MMAL_COMPONENT_BYTES;
}

/*
 * Builds a port info request.
 */
size_t
drv_rpi4_mmal_build_info_get(
	uint8_t *message,
	uint32_t waiter,
	uint32_t component,
	uint32_t type,
	uint32_t index)
{
	/* The header, the component, and which port of which type. */
	drv_rpi4_mmal_header(message, DRV_RPI4_MMAL_MSG_PORT_INFO_GET, waiter);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_INFO_GET_COMPONENT, component);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_INFO_GET_TYPE, type);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_INFO_GET_INDEX, index);

	/* Succeeded: the request's length. */
	return DRV_RPI4_MMAL_INFO_GET_BYTES;
}

/*
 * Builds a port info set request from a port info reply the caller edited.
 *
 * The reply carries the port's type and index, then the port, format, es
 * and extra data blocks; the request carries the same blocks right after
 * the type and index.
 */
size_t
drv_rpi4_mmal_build_info_set(
	uint8_t *message,
	uint32_t waiter,
	uint32_t component,
	const uint8_t *info)
{
	size_t blocks;

	/* The header, the component, and the port's type and index from the reply. */
	kern_memset(message, 0, DRV_RPI4_MMAL_INFO_SET_BYTES);
	drv_rpi4_mmal_header(message, DRV_RPI4_MMAL_MSG_PORT_INFO_SET, waiter);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_INFO_SET_COMPONENT, component);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_INFO_SET_TYPE, drv_rpi4_mmal_get32(info, 32U));
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_INFO_SET_INDEX, drv_rpi4_mmal_get32(info, 36U));

	/* The port, format, es and extra data blocks, unchanged in order and size. */
	blocks = DRV_RPI4_MMAL_INFO_SET_BYTES - DRV_RPI4_MMAL_INFO_SET_PORT;
	kern_memcpy(message + DRV_RPI4_MMAL_INFO_SET_PORT, info + DRV_RPI4_MMAL_INFO_PORT, blocks);

	/* Succeeded: the request's length. */
	return DRV_RPI4_MMAL_INFO_SET_BYTES;
}

/*
 * Builds a port action request.
 *
 * ENABLE carries the port block (with the buffer count and size the client
 * chose) of a port info reply; the other actions carry no block.
 */
size_t
drv_rpi4_mmal_build_action(
	uint8_t *message,
	uint32_t waiter,
	uint32_t component,
	uint32_t port_handle,
	uint32_t action,
	const uint8_t *info)
{
	/* The header, the component, the port and the action. */
	kern_memset(message, 0, DRV_RPI4_MMAL_ACTION_BYTES);
	drv_rpi4_mmal_header(message, DRV_RPI4_MMAL_MSG_PORT_ACTION, waiter);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_ACTION_COMPONENT, component);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_ACTION_PORT_HANDLE, port_handle);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_ACTION_ACTION, action);

	/* An enable carries the port's settings. */
	if (info != NULL)
		kern_memcpy(message + DRV_RPI4_MMAL_ACTION_PORT, info + DRV_RPI4_MMAL_INFO_PORT, DRV_RPI4_MMAL_PORT_BYTES);

	/* Succeeded: the request's length. */
	return DRV_RPI4_MMAL_ACTION_BYTES;
}

/*
 * Builds a 32-bit parameter set request.
 *
 * The parameter is its id, its size (the eight-byte parameter header and
 * the value) and the value; the message ends after the value.
 */
size_t
drv_rpi4_mmal_build_parameter32(
	uint8_t *message,
	uint32_t waiter,
	uint32_t component,
	uint32_t port_handle,
	uint32_t id,
	uint32_t value)
{
	/* The header, the component, the port, then the parameter. */
	drv_rpi4_mmal_header(message, DRV_RPI4_MMAL_MSG_PORT_PARAMETER_SET, waiter);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_PARAMETER_COMPONENT, component);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_PARAMETER_PORT_HANDLE, port_handle);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_PARAMETER_ID, id);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_PARAMETER_SIZE, 12U);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_PARAMETER_VALUE, value);

	/* Succeeded: the message ends with the value. */
	return DRV_RPI4_MMAL_PARAMETER_VALUE + 4U;
}

/*
 * Builds a buffer message for a port.
 *
 * context is the client's name for the buffer, which the firmware returns
 * in the driver area and in the data pointer; the payload is never in the
 * message (it travels as a bulk transfer), the buffer is never zero copy,
 * and the decode time is unknown.
 */
size_t
drv_rpi4_mmal_build_buffer(
	uint8_t *message,
	uint32_t msgid,
	uint32_t waiter,
	uint32_t component,
	uint32_t port_handle,
	uint32_t context,
	uint32_t alloc_size,
	uint32_t length,
	uint32_t flags,
	int64_t pts)
{
	/* The header and a zero body. */
	kern_memset(message, 0, DRV_RPI4_MMAL_BUFFER_BYTES);
	drv_rpi4_mmal_header(message, msgid, waiter);

	/* The driver area: whose buffer it is, and the client's name for it. */
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_BUFFER_DRIVER_MAGIC, DRV_RPI4_MMAL_MAGIC);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_BUFFER_COMPONENT, component);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_BUFFER_PORT_HANDLE, port_handle);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_BUFFER_CONTEXT, context);

	/* The buffer header: its size, what it holds, and when. */
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_BUFFER_DATA, context);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_BUFFER_ALLOC_SIZE, alloc_size);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_BUFFER_LENGTH, length);
	drv_rpi4_mmal_put32(message, DRV_RPI4_MMAL_BUFFER_FLAGS, flags);
	drv_rpi4_mmal_put64(message, DRV_RPI4_MMAL_BUFFER_PTS, (uint64_t)pts);
	drv_rpi4_mmal_put64(message, DRV_RPI4_MMAL_BUFFER_DTS, (uint64_t)DRV_RPI4_MMAL_TIME_UNKNOWN);

	/* Succeeded: the message's length. */
	return DRV_RPI4_MMAL_BUFFER_BYTES;
}

/*
 * Copies samples of a decoded frame, undoing the firmware's footroom error.
 *
 * Eight samples go at a time while both addresses are aligned: per byte,
 * the high bit of 0x8e minus the low seven bits says "at most 14", the high
 * bit of the low seven bits plus 0x7f says "at least 1", and a clear high
 * bit of the sample "below 128".  No byte borrows or carries into the next
 * in any of the three, nor in the final add of one.
 */
void
drv_rpi4_mmal_copy_samples(
	uint8_t *destination,
	const uint8_t *source,
	size_t bytes)
{
	uint64_t word;
	uint64_t low;
	uint64_t at_most_14;
	uint64_t at_least_1;
	uint64_t mask;
	size_t index;
	uint8_t sample;

	/* Eight samples at a time while both addresses are aligned. */
	index = 0;
	if ((((uintptr_t)destination | (uintptr_t)source) & 7U) == 0) {
		for (; index + 8U <= bytes; index += 8U) {
			word = *(const uint64_t *)(source + index);
			low = word & 0x7f7f7f7f7f7f7f7fULL;
			at_most_14 = 0x8e8e8e8e8e8e8e8eULL - low;
			at_least_1 = low + 0x7f7f7f7f7f7f7f7fULL;
			mask = at_most_14 & at_least_1 & ~word & 0x8080808080808080ULL;
			*(uint64_t *)(destination + index) = word + (mask >> 7);
		}
	}

	/* The rest one at a time. */
	for (; index < bytes; index++) {
		sample = source[index];
		if (sample >= 1U && sample <= 14U)
			sample++;
		destination[index] = sample;
	}
}
