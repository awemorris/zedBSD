/*
 * USB Attached SCSI descriptor and high-speed command protocol.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <drivers/usb-uas.h>

#include <errno.h>
#include <string.h>

struct uas_endpoint_description {
	struct drv_usb_uas_pipe pipe;
	uint8_t pipe_id;
	unsigned companion_present;
};

static unsigned uas_le16(const uint8_t *bytes);
static int uas_decode_alternate(const uint8_t *bytes, size_t length,
    enum drv_usb_uas_profile profile, struct drv_usb_uas_capabilities *result);

/* Encode the fixed Command IU without depending on host packing/endian order. */
int
drv_usb_uas_command_begin(struct drv_usb_uas_command *command,
    uint16_t tag, unsigned lun, const void *cdb, size_t cdb_length,
    enum drv_usb_uas_direction direction, size_t expected, uint8_t wire[32])
{
	if (command == NULL || wire == NULL || cdb == NULL)
		return EINVAL;
	if (command->state != DRV_USB_UAS_IDLE)
		return EBUSY;
	if (tag == 0 || lun > 255 || cdb_length == 0 || cdb_length > 16)
		return EINVAL;
	if (direction != DRV_USB_UAS_NO_DATA &&
	    direction != DRV_USB_UAS_READ && direction != DRV_USB_UAS_WRITE)
		return EINVAL;
	if ((direction == DRV_USB_UAS_NO_DATA) != (expected == 0))
		return EINVAL;

	memset(wire, 0, 32);
	wire[0] = 1; /* Command IU, SIMPLE task attribute and no additional CDB. */
	wire[2] = (uint8_t)(tag >> 8);
	wire[3] = (uint8_t)tag;
	wire[9] = (uint8_t)lun; /* Peripheral-device addressing. */
	memcpy(wire + 16, cdb, cdb_length);

	memset(command, 0, sizeof(*command));
	command->tag = tag;
	command->direction = direction;
	command->expected = expected;
	command->state = DRV_USB_UAS_WAIT_STATUS;
	return 0;
}

void
drv_usb_uas_command_fail(struct drv_usb_uas_command *command)
{
	if (command != NULL)
		command->state = DRV_USB_UAS_FAILED;
}

int
drv_usb_uas_command_data(struct drv_usb_uas_command *command, size_t actual)
{
	if (command == NULL)
		return EINVAL;
	if (command->state != DRV_USB_UAS_DATA || actual > command->expected) {
		command->state = DRV_USB_UAS_FAILED;
		return EIO;
	}

	command->transferred = actual;
	command->state = DRV_USB_UAS_WAIT_STATUS;
	return 0;
}

int
drv_usb_uas_command_status(struct drv_usb_uas_command *command,
    const void *wire, size_t length)
{
	const uint8_t *bytes;
	unsigned tag;
	unsigned sense_length;

	if (command == NULL)
		return EINVAL;
	if (command->state != DRV_USB_UAS_WAIT_STATUS)
		goto malformed;
	if (wire == NULL || length < 4)
		goto malformed;
	bytes = wire;
	tag = ((unsigned)bytes[2] << 8) | bytes[3];
	if (tag != command->tag)
		goto malformed;

	/* High-speed READY grants exactly one data phase in the planned direction. */
	if (bytes[0] == 6 || bytes[0] == 7) {
		if (length != 4 || command->data_seen)
			goto malformed;
		if (command->direction == DRV_USB_UAS_NO_DATA)
			goto malformed;
		if (bytes[0] == 6 && command->direction != DRV_USB_UAS_READ)
			goto malformed;
		if (bytes[0] == 7 && command->direction != DRV_USB_UAS_WRITE)
			goto malformed;
		command->data_seen = 1;
		command->state = DRV_USB_UAS_DATA;
		return 0;
	}

	/* Sense IU may complete a rejected CDB without a READY/data transfer. */
	if (bytes[0] != 3 || length < 16)
		goto malformed;
	sense_length = ((unsigned)bytes[14] << 8) | bytes[15];
	if (length - 16 != sense_length)
		goto malformed;
	/* Qualified status needs handling beyond the current SIMPLE-task owner. */
	if (bytes[4] != 0 || bytes[5] != 0)
		goto malformed;
	if (bytes[6] == 0 && command->expected != 0 && !command->data_seen)
		goto malformed;

	command->scsi_status = bytes[6];
	command->state = DRV_USB_UAS_COMPLETE;
	return 0;

malformed:
	command->state = DRV_USB_UAS_FAILED;
	return EIO;
}

/* Validate the configuration framing before decoding the selected alternate. */
int
drv_usb_uas_decode_configuration(
	const void *raw,
	size_t length,
	unsigned interface_number,
	unsigned alternate_setting,
	enum drv_usb_uas_profile profile,
	struct drv_usb_uas_capabilities *result)
{
	const uint8_t *bytes;
	struct drv_usb_uas_capabilities decoded;
	size_t offset;
	size_t selected;
	size_t end;
	unsigned size;
	int error;

	/* Leaves no partially decoded capabilities after any failure. */
	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	if (raw == NULL || length < 9 || length > 65535U)
		return EINVAL;
	if (interface_number > 255U || alternate_setting > 255U)
		return EINVAL;
	if (profile != DRV_USB_UAS_HIGH_SPEED &&
	    profile != DRV_USB_UAS_SUPER_SPEED)
		return EOPNOTSUPP;
	bytes = raw;
	if (bytes[0] != 9 || bytes[1] != 2 || bytes[4] == 0 ||
	    uas_le16(bytes + 2) != length)
		return EINVAL;

	/* Locates exactly one selected alternate without crossing its boundary. */
	selected = 0;
	end = length;
	for (offset = 9; offset < length; offset += size) {
		if (length - offset < 2)
			return EINVAL;
		size = bytes[offset];
		if (size < 2 || size > length - offset)
			return EINVAL;
		if (bytes[offset + 1] == 2)
			return EINVAL;
		if (bytes[offset + 1] != 4)
			continue;
		if (size != 9)
			return EINVAL;
		if (selected != 0 && end == length)
			end = offset;
		if (bytes[offset + 2] != interface_number ||
		    bytes[offset + 3] != alternate_setting)
			continue;
		if (selected != 0)
			return EINVAL;
		selected = offset;
	}
	if (selected == 0)
		return ENOENT;

	/* Commits the complete value only after the pipe contract is valid. */
	memset(&decoded, 0, sizeof(decoded));
	error = uas_decode_alternate(bytes + selected, end - selected,
	    profile, &decoded);
	if (error != 0)
		return error;
	*result = decoded;
	return 0;
}

/* Reads an unaligned little-endian descriptor field. */
static unsigned
uas_le16(
	const uint8_t *bytes)
{
	return (unsigned)bytes[0] | ((unsigned)bytes[1] << 8);
}

/* Associates each Pipe Usage descriptor with its preceding bulk endpoint. */
static int
uas_decode_alternate(
	const uint8_t *bytes,
	size_t length,
	enum drv_usb_uas_profile profile,
	struct drv_usb_uas_capabilities *result)
{
	struct uas_endpoint_description endpoints[4];
	struct uas_endpoint_description *endpoint;
	const uint8_t *item;
	size_t offset;
	unsigned size;
	unsigned kind;
	unsigned previous_kind;
	unsigned count;
	unsigned index;
	unsigned other;
	unsigned pipe_mask;
	unsigned id;
	unsigned expected_direction;
	unsigned minimum;

	if (length < 9 || bytes[4] != 4 || bytes[5] != 8 ||
	    bytes[6] != 6 || bytes[7] != 0x62)
		return EINVAL;
	memset(endpoints, 0, sizeof(endpoints));
	endpoint = NULL;
	count = 0;
	previous_kind = 4;

	/* Framing was checked in the outer pass; semantic sizes are checked here. */
	for (offset = 9; offset < length; offset += size) {
		item = bytes + offset;
		size = item[0];
		kind = item[1];
		if (profile == DRV_USB_UAS_SUPER_SPEED &&
		    previous_kind == 5 && kind != 0x30)
			return EINVAL;
		if (kind == 5) {
			if (size != 7 || count == 4)
				return EINVAL;
			if ((item[2] & 15U) == 0 || (item[2] & 0x70U) != 0 ||
			    item[3] != 2)
				return EINVAL;
			endpoint = &endpoints[count++];
			endpoint->pipe.address = item[2];
			endpoint->pipe.maximum_packet = uas_le16(item + 4);
		} else if (kind == 0x30) {
			if (profile != DRV_USB_UAS_SUPER_SPEED || size != 6 ||
			    previous_kind != 5 || endpoint == NULL)
				return EINVAL;
			if (item[2] > 15 || item[3] > 16 || uas_le16(item + 4) != 0)
				return EINVAL;
			endpoint->companion_present = 1;
			endpoint->pipe.maximum_burst = item[2];
			endpoint->pipe.stream_exponent = item[3];
		} else if (kind == 0x24) {
			if (size != 4 || endpoint == NULL || endpoint->pipe_id != 0)
				return EINVAL;
			if (item[2] < 1 || item[2] > 4 || item[3] != 0)
				return EINVAL;
			endpoint->pipe_id = item[2];
		}
		previous_kind = kind;
	}
	if (count != 4)
		return EINVAL;

	/* Rejects aliases, missing pipes, direction errors and mixed speed profiles. */
	pipe_mask = 0;
	minimum = 16;
	for (index = 0; index < count; index++) {
		endpoint = &endpoints[index];
		id = endpoint->pipe_id;
		if (id == 0 || (pipe_mask & (1U << id)) != 0)
			return EINVAL;
		pipe_mask |= 1U << id;
		for (other = 0; other < index; other++) {
			if (endpoints[other].pipe.address == endpoint->pipe.address)
				return EINVAL;
		}
		expected_direction = (id == 2 || id == 3) ? 0x80U : 0;
		if ((endpoint->pipe.address & 0x80U) != expected_direction)
			return EINVAL;
		if (profile == DRV_USB_UAS_SUPER_SPEED) {
			if (!endpoint->companion_present ||
			    endpoint->pipe.maximum_packet != 1024)
				return EINVAL;
			if ((id == 1) != (endpoint->pipe.stream_exponent == 0))
				return EINVAL;
			if (id != 1 && endpoint->pipe.stream_exponent < minimum)
				minimum = endpoint->pipe.stream_exponent;
		} else if (endpoint->pipe.maximum_packet != 512) {
			return EINVAL;
		}
		result->pipes[id - 1] = endpoint->pipe;
	}

	result->interface_number = bytes[2];
	result->alternate_setting = bytes[3];
	if (profile == DRV_USB_UAS_SUPER_SPEED)
		result->minimum_stream_exponent = minimum;
	return 0;
}
