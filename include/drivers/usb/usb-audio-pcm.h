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
	uint32_t *next_pos);

#endif
