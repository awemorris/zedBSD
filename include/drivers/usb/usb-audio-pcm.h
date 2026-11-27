/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pure PCM rate generation, feedback decoding, and sample packing helpers
 * for the USB Audio Class driver.
 */

#ifndef KERN_DRIVERS_USB_AUDIO_PCM_H
#define KERN_DRIVERS_USB_AUDIO_PCM_H

#include <stddef.h>
#include <stdint.h>

/* Bresenham accumulator state for fractional frames-per-packet generation. */
struct uac_bresenham {
	uint32_t base_frames;
	uint32_t rem_frames;
	uint32_t packets_per_sec;
	uint32_t accum;
};

/*
 * Initializes a Bresenham rate generator for sample_rate and packets_per_sec.
 */
void
usb_audio_bresenham_init(
	struct uac_bresenham *state,
	uint32_t sample_rate,
	uint32_t packets_per_sec);

/*
 * Returns the number of PCM frames to place in the next isochronous packet.
 */
uint32_t
usb_audio_bresenham_next(
	struct uac_bresenham *state);

/*
 * Decodes a USB Audio 10.14 (3-byte) or 16.16 (4-byte) feedback value into
 * 16.16 fixed-point frames per packet, validating against nominal_q16.
 */
int
usb_audio_decode_feedback(
	const uint8_t *buf,
	size_t len,
	uint32_t nominal_q16,
	uint32_t *out_q16);

/*
 * Computes the next packet frame count from a 16.16 feedback rate and
 * updates the fractional accumulator.
 */
uint32_t
usb_audio_feedback_next(
	uint32_t rate_q16,
	uint32_t *accum_q16);

/*
 * Packs S32_LE samples (upper 24 bits significant) into 3-byte LE samples.
 */
void
usb_audio_pack_24_from_s32(
	const uint8_t *src_s32,
	uint8_t *dst_24,
	unsigned samples);

/*
 * Unpacks 3-byte LE samples into S32_LE samples (placed in upper 24 bits).
 */
void
usb_audio_unpack_24_to_s32(
	const uint8_t *src_24,
	uint8_t *dst_s32,
	unsigned samples);

#endif
