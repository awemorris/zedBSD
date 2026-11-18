/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM43455: the BCDC messages inside SDPCM frames.
 *
 * On the control channel a sixteen-byte header carries one firmware
 * command (an ioctl) and its answer; a named firmware variable (an
 * iovar) is a GET_VAR or SET_VAR command whose payload is the name, a
 * NUL and the value.  On the data and event channels a four-byte header
 * precedes the Ethernet frame.  The functions here only encode and
 * decode.
 */

#ifndef KERN_DRIVERS_BCM43455_BCDC_H
#define KERN_DRIVERS_BCM43455_BCDC_H

#include <stddef.h>
#include <stdint.h>

/* The control header and the data header. */
#define BCM43455_BCDC_CONTROL_LENGTH	16U
#define BCM43455_BCDC_DATA_LENGTH	4U

/* The most payload one control message carries either way. */
#define BCM43455_BCDC_CONTROL_MAX	2048U

/*
 * The firmware commands this driver uses.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines 248-311)
 * and Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/include/whd_wlioctl.h (lines 1305-1539).
 */
#define BCM43455_COMMAND_RADIO_UP		2U
#define BCM43455_COMMAND_RADIO_DOWN		3U
#define BCM43455_COMMAND_POWER_SAVE		86U
#define BCM43455_COMMAND_CHANNEL_LIST		217U
#define BCM43455_COMMAND_READ_VARIABLE		262U
#define BCM43455_COMMAND_WRITE_VARIABLE		263U

/*
 * A decoded control answer.
 *
 * id is the request it answers; failed is the firmware's error flag and
 * status its error code (negative, the firmware's own numbering).
 * payload_offset and payload_length say where the answer's data is in the
 * message, already bounded by what was received.
 */
struct bcm43455_bcdc_answer {
	uint32_t command;
	uint32_t length;
	uint16_t id;
	unsigned interface;
	unsigned set;
	unsigned failed;
	int32_t status;
	size_t payload_offset;
	size_t payload_length;
};

/*
 * A decoded data or event header.
 *
 * frame_offset is where the Ethernet frame starts, counted from the
 * header's first byte, and frame_length its length.
 */
struct bcm43455_bcdc_data {
	unsigned priority;
	unsigned interface;
	size_t frame_offset;
	size_t frame_length;
};

/*
 * Writes the control header of a command.
 *
 * length is the payload's length: the input for a set, the room for the
 * answer for a get.
 */
void bcm43455_bcdc_encode_control(uint8_t *header, uint32_t command, uint32_t length, int set, uint16_t id);

/*
 * Decodes a control message received from the firmware.
 *
 * Returns EPROTO for a message shorter than its header.
 */
int bcm43455_bcdc_decode_control(const uint8_t *message, size_t length, struct bcm43455_bcdc_answer *answer);

/*
 * Writes an iovar's payload, the name, a NUL and the value, into buffer.
 *
 * value may be NULL for a get that sends no argument.  length receives the
 * payload's length.  Returns EINVAL for an empty name and ENOSPC when the
 * payload does not fit capacity.
 */
int bcm43455_bcdc_iovar(uint8_t *buffer, size_t capacity, const char *name, const void *value, size_t value_length, size_t *length);

/*
 * Writes the four-byte data header in front of an Ethernet frame to send.
 */
void bcm43455_bcdc_encode_data(uint8_t *header, unsigned priority, unsigned interface);

/*
 * Decodes the data header of a received data or event payload.
 *
 * Returns EPROTO for an unknown header version or a frame offset past the
 * payload.
 */
int bcm43455_bcdc_decode_data(const uint8_t *payload, size_t length, struct bcm43455_bcdc_data *data);

#endif
