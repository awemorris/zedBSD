/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM43455: the SDPCM framing of the chip's frame function (SDIO F2).
 *
 * Every frame between the host and the chip's firmware starts with a
 * four-byte frame tag (the frame's length and its complement) and an
 * eight-byte software header: a sequence number, the channel the frame
 * belongs to, where its payload starts, and the firmware's flow control
 * and transmit credit.  The functions here only encode and decode; they
 * touch no hardware.
 */

#ifndef KERN_DRIVERS_BCM43455_SDPCM_H
#define KERN_DRIVERS_BCM43455_SDPCM_H

#include <stddef.h>
#include <stdint.h>

/* The frame tag and the software header together. */
#define BCM43455_SDPCM_HEADER_LENGTH	12U

/*
 * The channels a frame belongs to.
 *
 * Values referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.h revision 1.3 (lines
 * 167-172).
 */
#define BCM43455_SDPCM_CHANNEL_CONTROL	0U
#define BCM43455_SDPCM_CHANNEL_EVENT	1U
#define BCM43455_SDPCM_CHANNEL_DATA	2U
#define BCM43455_SDPCM_CHANNEL_GLOM	3U

/*
 * The most frames the host sends ahead of the firmware's credit.  A window
 * of this or more is a credit that went backwards or was garbled, and is
 * treated as none.
 */
#define BCM43455_SDPCM_WINDOW_LIMIT	0x80U

/*
 * One decoded SDPCM header.
 *
 * length is the whole frame, header included; payload_start is where the
 * payload starts, counted from the frame's first byte.  max_sequence is
 * the last sequence number the firmware will accept, which is how it
 * grants credit; flow_control has one bit per priority it has paused.
 */
struct bcm43455_sdpcm_fields {
	uint16_t length;
	uint8_t sequence;
	uint8_t channel;
	uint8_t flags;
	uint8_t next_length;
	uint8_t payload_start;
	uint8_t flow_control;
	uint8_t max_sequence;
};

/*
 * Decodes the header at the start of a received frame.
 *
 * available is how many bytes of the frame were read, at least the
 * header's twelve.  Returns ENOENT for an empty frame tag, which is how
 * the chip says it has nothing to send, EIO for a tag whose complement
 * does not match, and EPROTO for a length or payload offset that cannot
 * be a frame.
 */
int bcm43455_sdpcm_decode(const uint8_t *frame, size_t available, struct bcm43455_sdpcm_fields *header);

/*
 * Writes the header of a frame to send.
 *
 * length is the whole frame, header included, before any padding; the
 * payload follows the header directly.
 */
void bcm43455_sdpcm_encode(uint8_t *frame, uint16_t length, uint8_t sequence, uint8_t channel);

/*
 * Reports how many frames may be sent before the firmware's credit runs
 * out: zero when it has run out or the credit cannot be trusted.
 */
unsigned bcm43455_sdpcm_window(uint8_t max_sequence, uint8_t sequence);

/*
 * Reports how many bytes a frame of length bytes occupies on the bus.
 *
 * A frame up to one block moves in byte mode and is padded to four bytes;
 * a longer one moves in whole blocks of block_size.
 */
size_t bcm43455_sdpcm_padded_length(size_t length, unsigned block_size);

#endif
