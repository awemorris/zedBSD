/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pure PCM rate generation, feedback decoding, and zero-copy ring packet
 * planning helpers for the USB Audio Class driver.
 */

#include <drivers/usb/usb-audio-pcm.h>

#include <stddef.h>
#include <stdint.h>
#include <uapi/errno.h>

/*
 * Initializes a Bresenham rate generator for sample_rate and packets_per_sec.
 */
void
usb_audio_bresenham_init(
	struct uac_bresenham *state,
	uint32_t sample_rate,
	uint32_t packets_per_sec)
{
	if (state == NULL)
		return;

	if (packets_per_sec == 0U)
		packets_per_sec = 1000U;

	state->base_frames = sample_rate / packets_per_sec;
	state->rem_frames = sample_rate % packets_per_sec;
	state->packets_per_sec = packets_per_sec;
	state->accum = 0U;
}

/*
 * Returns the number of PCM frames to place in the next isochronous packet.
 */
uint32_t
usb_audio_bresenham_next(
	struct uac_bresenham *state)
{
	uint32_t frames;

	if (state == NULL)
		return 0U;

	frames = state->base_frames;
	state->accum += state->rem_frames;
	if (state->accum >= state->packets_per_sec) {
		state->accum -= state->packets_per_sec;
		frames++;
	}

	/* Succeeded: frames for this packet. */
	return frames;
}

/*
 * Decodes a USB Audio 10.14 (3-byte) or 16.16 (4-byte) feedback value into
 * 16.16 fixed-point frames per packet, validating against nominal_q16.
 */
int
usb_audio_decode_feedback(
	const uint8_t *buf,
	size_t len,
	uint32_t nominal_q16,
	uint32_t *out_q16)
{
	uint32_t raw;
	uint32_t q16;
	uint32_t margin;

	if (buf == NULL || out_q16 == NULL || nominal_q16 == 0U)
		return EINVAL;

	if (len == 3U) {
		raw = (uint32_t)buf[0] |
		      ((uint32_t)buf[1] << 8) |
		      ((uint32_t)buf[2] << 16);
		q16 = raw << 2;
	} else if (len >= 4U) {
		q16 = (uint32_t)buf[0] |
		      ((uint32_t)buf[1] << 8) |
		      ((uint32_t)buf[2] << 16) |
		      ((uint32_t)buf[3] << 24);
	} else {
		return EINVAL;
	}

	/* Rejects zero or feedback values deviating more than 10% from nominal. */
	margin = nominal_q16 / 10U;
	if (margin == 0U)
		margin = 1U;

	if (q16 == 0U ||
	    q16 < nominal_q16 - margin ||
	    q16 > nominal_q16 + margin) {
		return ERANGE;
	}

	*out_q16 = q16;

	/* Succeeded: feedback value decoded within nominal bounds. */
	return 0;
}

/*
 * Computes the next packet frame count from a 16.16 feedback rate and
 * updates the fractional accumulator.
 */
uint32_t
usb_audio_feedback_next(
	uint32_t rate_q16,
	uint32_t *accum_q16)
{
	uint32_t sum;

	if (accum_q16 == NULL)
		return rate_q16 >> 16;

	sum = rate_q16 + *accum_q16;
	*accum_q16 = sum & 0xffffU;

	/* Succeeded: integer frames for this packet. */
	return sum >> 16;
}

/*
 * Plans zero-copy isochronous packet offsets and lengths directly on a DMA
 * ring buffer, splitting at ring_bytes wraparound without copying.
 */
void
usb_audio_plan_iso_urb(
	uint32_t ring_pos,
	uint32_t ring_bytes,
	uint32_t frame_bytes,
	unsigned packet_count,
	const uint32_t *pkt_bytes,
	uint32_t *buf_offset,
	uint32_t *buf_length,
	uint32_t *iso_offsets,
	uint32_t *iso_lengths,
	uint32_t *total_ring_bytes,
	uint32_t *next_pos)
{
	unsigned i;
	uint32_t total;
	uint32_t pos;
	uint32_t rem;
	uint32_t take;

	if (ring_bytes == 0U || frame_bytes == 0U || packet_count == 0U ||
	    pkt_bytes == NULL || buf_offset == NULL || buf_length == NULL ||
	    iso_offsets == NULL || iso_lengths == NULL ||
	    total_ring_bytes == NULL || next_pos == NULL)
		return;

	total = 0U;
	for (i = 0U; i < packet_count; i++)
		total += pkt_bytes[i];

	/* Fast path: entire URB fits contiguously without crossing ring_bytes. */
	if (ring_pos + total <= ring_bytes) {
		*buf_offset = ring_pos;
		*buf_length = total;
		pos = 0U;
		for (i = 0U; i < packet_count; i++) {
			iso_offsets[i] = pos;
			iso_lengths[i] = pkt_bytes[i];
			pos += pkt_bytes[i];
		}
		*total_ring_bytes = total;
		*next_pos = (ring_pos + total) % ring_bytes;
		return;
	}

	/* Wraparound path: point URB at full ring and wrap packet offsets at 0. */
	*buf_offset = 0U;
	*buf_length = ring_bytes;
	pos = ring_pos;
	total = 0U;
	for (i = 0U; i < packet_count; i++) {
		rem = ring_bytes - pos;
		if (pkt_bytes[i] <= rem) {
			iso_offsets[i] = pos;
			iso_lengths[i] = pkt_bytes[i];
			pos += pkt_bytes[i];
			if (pos == ring_bytes)
				pos = 0U;
			total += pkt_bytes[i];
		} else if (rem >= frame_bytes) {
			take = (rem / frame_bytes) * frame_bytes;
			iso_offsets[i] = pos;
			iso_lengths[i] = take;
			total += rem;
			pos = 0U;
		} else {
			total += rem;
			pos = 0U;
			iso_offsets[i] = 0U;
			iso_lengths[i] = pkt_bytes[i];
			pos = pkt_bytes[i];
			total += pkt_bytes[i];
		}
	}

	*total_ring_bytes = total;
	*next_pos = pos;
}
