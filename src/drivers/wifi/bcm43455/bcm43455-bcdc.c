/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM43455: encoding and decoding the BCDC control and data headers.
 */

#include "drivers/wifi/bcm43455/bcm43455-bcdc.h"
#include "drivers/wifi/bcm43455/bcm43455-bytes.h"
#include <kern/kcrt.h>
#include <uapi/errno.h>

/*
 * Where each field of the control header sits, and its flag word.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (struct
 * bwfm_proto_bcdc_dcmd, lines 314-329).
 */
#define CONTROL_COMMAND		0U
#define CONTROL_LENGTH		4U
#define CONTROL_FLAGS		8U
#define CONTROL_STATUS		12U

/* The control header's flag word. */
#define CONTROL_FLAG_ERROR	0x00000001U
#define CONTROL_FLAG_SET	0x00000002U
#define CONTROL_INTERFACE_MASK	0x0000f000U
#define CONTROL_INTERFACE_SHIFT	12U
#define CONTROL_ID_SHIFT	16U

/*
 * Where each field of the data header sits, its version, and its priority and
 * interface fields.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (struct
 * bwfm_proto_bcdc_hdr, lines 331-342).
 */
#define DATA_FLAGS		0U
#define DATA_PRIORITY		1U
#define DATA_FLAGS2		2U
#define DATA_SKIP_WORDS		3U

/* The data header's version, in the high four bits of its flags byte. */
#define DATA_VERSION		2U
#define DATA_VERSION_SHIFT	4U

/* The priority and interface fields of the data header. */
#define DATA_PRIORITY_MASK	0x07U
#define DATA_INTERFACE_MASK	0x0fU

/* The data header's offset counts four-byte words. */
#define DATA_SKIP_UNIT	4U

/* The longest iovar name this driver sends, its NUL excluded. */
#define IOVAR_NAME_MAX		64U

/*
 * Writes the control header of a command.
 */
void
bcm43455_bcdc_encode_control(
	uint8_t *header,
	uint32_t command,
	uint32_t length,
	int set,
	uint16_t id)
{
	uint32_t flags;

	/*
	 * The request id lets the answer be matched; interface 0 is the
	 * station.
	 */
	flags = (uint32_t)id << CONTROL_ID_SHIFT;
	if (set)
		flags |= CONTROL_FLAG_SET;

	/* Writes the four words; the status is the firmware's to fill. */
	bcm43455_put_le32(header + CONTROL_COMMAND, command);
	bcm43455_put_le32(header + CONTROL_LENGTH, length);
	bcm43455_put_le32(header + CONTROL_FLAGS, flags);
	bcm43455_put_le32(header + CONTROL_STATUS, 0);
}

/*
 * Decodes a control message received from the firmware.
 */
int
bcm43455_bcdc_decode_control(
	const uint8_t *message,
	size_t length,
	struct bcm43455_bcdc_answer *answer)
{
	uint32_t flags;
	size_t received;

	/* Refuses a message without the whole header. */
	if (message == NULL || answer == NULL)
		return EINVAL;
	if (length < BCM43455_BCDC_CONTROL_LENGTH)
		return EPROTO;

	/* Reads the four words. */
	answer->command = bcm43455_get_le32(message + CONTROL_COMMAND);
	answer->length = bcm43455_get_le32(message + CONTROL_LENGTH);
	flags = bcm43455_get_le32(message + CONTROL_FLAGS);
	answer->status = (int32_t)bcm43455_get_le32(message + CONTROL_STATUS);

	/* Splits the flag word. */
	answer->id = (uint16_t)(flags >> CONTROL_ID_SHIFT);
	answer->interface = (unsigned)((flags & CONTROL_INTERFACE_MASK) >> CONTROL_INTERFACE_SHIFT);
	answer->set = 0;
	if ((flags & CONTROL_FLAG_SET) != 0)
		answer->set = 1;
	answer->failed = 0;
	if ((flags & CONTROL_FLAG_ERROR) != 0)
		answer->failed = 1;

	/*
	 * The payload is what the header claims, but never past what was
	 * received.
	 */
	received = length - BCM43455_BCDC_CONTROL_LENGTH;
	answer->payload_offset = BCM43455_BCDC_CONTROL_LENGTH;
	answer->payload_length = received;
	if ((size_t)answer->length < received)
		answer->payload_length = (size_t)answer->length;

	/* Succeeded: answer describes the message. */
	return 0;
}

/*
 * Writes an iovar's payload.
 */
int
bcm43455_bcdc_iovar(
	uint8_t *buffer,
	size_t capacity,
	const char *name,
	const void *value,
	size_t value_length,
	size_t *length)
{
	size_t name_length;

	/* Refuses a missing buffer or name. */
	if (buffer == NULL ||
	    name == NULL ||
	    length == NULL)
		return EINVAL;
	if (value == NULL && value_length != 0)
		return EINVAL;

	/* Measures the name, which is short by construction. */
	name_length = 0;
	while (name_length < IOVAR_NAME_MAX && name[name_length] != '\0')
		name_length++;
	if (name_length == 0 || name_length == IOVAR_NAME_MAX)
		return EINVAL;

	/* Refuses a payload that does not fit. */
	if (capacity < name_length + 1U)
		return ENOSPC;
	if (value_length > capacity - name_length - 1U)
		return ENOSPC;

	/* The name and its NUL. */
	kern_memcpy(buffer, name, name_length);
	buffer[name_length] = 0;

	/* The value, if any, follows directly. */
	if (value_length != 0)
		kern_memcpy(buffer + name_length + 1U, value, value_length);

	/* Succeeded: the payload is ready to send. */
	*length = name_length + 1U + value_length;
	return 0;
}

/*
 * Writes the four-byte data header in front of an Ethernet frame.
 */
void
bcm43455_bcdc_encode_data(
	uint8_t *header,
	unsigned priority,
	unsigned interface)
{
	/*
	 * The version, the priority, the interface, and a frame right after the
	 * header.
	 */
	header[DATA_FLAGS] = (uint8_t)(DATA_VERSION << DATA_VERSION_SHIFT);
	header[DATA_PRIORITY] = (uint8_t)(priority & DATA_PRIORITY_MASK);
	header[DATA_FLAGS2] = (uint8_t)(interface & DATA_INTERFACE_MASK);
	header[DATA_SKIP_WORDS] = 0;
}

/*
 * Decodes the data header of a received data or event payload.
 */
int
bcm43455_bcdc_decode_data(
	const uint8_t *payload,
	size_t length,
	struct bcm43455_bcdc_data *data)
{
	size_t offset;

	/* Refuses a payload without the whole header. */
	if (payload == NULL || data == NULL)
		return EINVAL;
	if (length < BCM43455_BCDC_DATA_LENGTH)
		return EPROTO;

	/* Refuses a header of another version, whose layout is not known. */
	if ((unsigned)(payload[DATA_FLAGS] >> DATA_VERSION_SHIFT) != DATA_VERSION)
		return EPROTO;

	/* The frame starts after the header and the firmware's extra words. */
	offset = BCM43455_BCDC_DATA_LENGTH + (size_t)payload[DATA_SKIP_WORDS] * DATA_SKIP_UNIT;
	if (offset > length)
		return EPROTO;

	/* Keeps the priority, the interface and where the frame is. */
	data->priority = (unsigned)(payload[DATA_PRIORITY] & DATA_PRIORITY_MASK);
	data->interface = (unsigned)(payload[DATA_FLAGS2] & DATA_INTERFACE_MASK);
	data->frame_offset = offset;
	data->frame_length = length - offset;

	/* Succeeded: data says where the frame is. */
	return 0;
}
