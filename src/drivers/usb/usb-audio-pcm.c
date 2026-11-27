/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pure PCM rate generation, feedback decoding, and 24-bit sample packing
 * helpers for the USB Audio Class driver.
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
 * Packs S32_LE samples (upper 24 bits significant) into 3-byte LE samples.
 */
void
usb_audio_pack_24_from_s32(
	const uint8_t *src_s32,
	uint8_t *dst_24,
	unsigned samples)
{
	unsigned i;
	const uint8_t *s;
	uint8_t *d;

	if (src_s32 == NULL || dst_24 == NULL)
		return;

	s = src_s32;
	d = dst_24;
	for (i = 0U; i < samples; i++) {
		d[0] = s[1];
		d[1] = s[2];
		d[2] = s[3];
		s += 4U;
		d += 3U;
	}
}

/*
 * Unpacks 3-byte LE samples into S32_LE samples (placed in upper 24 bits).
 */
void
usb_audio_unpack_24_to_s32(
	const uint8_t *src_24,
	uint8_t *dst_s32,
	unsigned samples)
{
	unsigned i;
	const uint8_t *s;
	uint8_t *d;

	if (src_24 == NULL || dst_s32 == NULL)
		return;

	s = src_24;
	d = dst_s32;
	for (i = 0U; i < samples; i++) {
		d[0] = 0U;
		d[1] = s[0];
		d[2] = s[1];
		d[3] = s[2];
		s += 3U;
		d += 4U;
	}
}
