/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM43455: encoding and decoding the SDPCM frame headers.
 *
 * The layout (frame tag, then the software header) is the one the chip's
 * firmware uses on its SDIO frame function.
 */

#include "drivers/wifi/bcm43455/bcm43455-sdpcm.h"
#include "drivers/wifi/bcm43455/bcm43455-bytes.h"
#include <uapi/errno.h>

/*
 * Where each field of the header sits in the frame, and the channel byte's
 * split.
 *
 * Values referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.h revision 1.3 (struct
 * bwfm_sdio_hwhdr and struct bwfm_sdio_swhdr, lines 150-179).
 */
#define TAG_LENGTH		0U
#define TAG_CHECK		2U
#define HEADER_SEQUENCE		4U
#define HEADER_CHANNEL		5U
#define HEADER_NEXT_LENGTH	6U
#define HEADER_PAYLOAD_START	7U
#define HEADER_FLOW_CONTROL	8U
#define HEADER_MAX_SEQUENCE	9U

/*
 * The channel byte: the channel in its low four bits, flags in its high four.
 */
#define CHANNEL_MASK		0x0fU
#define FLAGS_SHIFT		4U

/* The bus moves frames up to one block in four-byte units. */
#define BYTE_MODE_UNIT		4U

/*
 * Decodes the header at the start of a received frame.
 */
int
bcm43455_sdpcm_decode(
	const uint8_t *frame,
	size_t available,
	struct bcm43455_sdpcm_fields *header)
{
	uint16_t length;
	uint16_t check;

	/* Refuses a read too short to hold the header. */
	if (frame == NULL || header == NULL)
		return EINVAL;
	if (available < BCM43455_SDPCM_HEADER_LENGTH)
		return EINVAL;

	/* Reads the frame tag. */
	length = bcm43455_get_le16(frame + TAG_LENGTH);
	check = bcm43455_get_le16(frame + TAG_CHECK);

	/* An all-zero tag is the chip saying it has no frame. */
	if (length == 0 && check == 0)
		return ENOENT;

	/* The second half of the tag is the complement of the first. */
	if ((uint16_t)(length ^ check) != 0xffffU)
		return EIO;

	/* A frame holds at least its own header. */
	if (length < BCM43455_SDPCM_HEADER_LENGTH)
		return EPROTO;

	/* Reads the software header. */
	header->length = length;
	header->sequence = frame[HEADER_SEQUENCE];
	header->channel = (uint8_t)(frame[HEADER_CHANNEL] & CHANNEL_MASK);
	header->flags = (uint8_t)(frame[HEADER_CHANNEL] >> FLAGS_SHIFT);
	header->next_length = frame[HEADER_NEXT_LENGTH];
	header->payload_start = frame[HEADER_PAYLOAD_START];
	header->flow_control = frame[HEADER_FLOW_CONTROL];
	header->max_sequence = frame[HEADER_MAX_SEQUENCE];

	/*
	 * A frame of only a header carries the credit and no payload; its
	 * offset is not used.
	 */
	if (length == BCM43455_SDPCM_HEADER_LENGTH)
		return 0;

	/* The payload starts after the header and inside the frame. */
	if (header->payload_start < BCM43455_SDPCM_HEADER_LENGTH)
		return EPROTO;
	if (header->payload_start > length)
		return EPROTO;

	/* Succeeded: header describes the frame. */
	return 0;
}

/*
 * Writes the header of a frame to send.
 */
void
bcm43455_sdpcm_encode(
	uint8_t *frame,
	uint16_t length,
	uint8_t sequence,
	uint8_t channel)
{
	/* The frame tag: the length and its complement. */
	bcm43455_put_le16(frame + TAG_LENGTH, length);
	bcm43455_put_le16(frame + TAG_CHECK, (uint16_t)~length);

	/* The software header: the payload follows it directly, no flags. */
	frame[HEADER_SEQUENCE] = sequence;
	frame[HEADER_CHANNEL] = (uint8_t)(channel & CHANNEL_MASK);
	frame[HEADER_NEXT_LENGTH] = 0;
	frame[HEADER_PAYLOAD_START] = (uint8_t)BCM43455_SDPCM_HEADER_LENGTH;
	frame[HEADER_FLOW_CONTROL] = 0;
	frame[HEADER_MAX_SEQUENCE] = 0;
	frame[10] = 0;
	frame[11] = 0;
}

/*
 * Reports how many frames may be sent before the firmware's credit runs out.
 *
 * Procedure referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.c revision 1.44
 * (bwfm_sdio_tx_ok(), lines 1076-1081).
 */
unsigned
bcm43455_sdpcm_window(
	uint8_t max_sequence,
	uint8_t sequence)
{
	unsigned window;

	/*
	 * The credit is the distance from the next sequence to the last one
	 * granted, modulo 256.
	 */
	window = (unsigned)((uint8_t)(max_sequence - sequence));

	/*
	 * A distance of half the sequence space or more means the grant is
	 * behind the host or garbled; sending on it would flood the chip.
	 */
	if ((window & BCM43455_SDPCM_WINDOW_LIMIT) != 0)
		return 0;

	/* Succeeded: the frames the firmware will still take. */
	return window;
}

/*
 * Reports how many bytes a frame occupies on the bus.
 *
 * Procedure referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.c revision 1.44
 * (lines 1112-1117).
 */
size_t
bcm43455_sdpcm_padded_length(
	size_t length,
	unsigned block_size)
{
	size_t remainder;

	/*
	 * A frame that fits one block moves in byte mode, in four-byte units.
	 */
	if (block_size == 0 || length <= block_size) {
		remainder = length % BYTE_MODE_UNIT;
		if (remainder != 0)
			return length + (BYTE_MODE_UNIT - remainder);
		return length;
	}

	/* A longer frame moves in whole blocks. */
	remainder = length % block_size;
	if (remainder != 0)
		return length + (block_size - remainder);

	/* Succeeded: the frame is already a whole number of blocks. */
	return length;
}
