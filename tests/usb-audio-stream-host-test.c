/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host unit tests for Bresenham rate generation, feedback decoding, and
 * zero-copy DMA ring URB packet planning across wraparound boundaries.
 *
 * Build and run:
 *   cc -Wall -Wextra -Werror -std=c89 -Iinclude \
 *     src/drivers/usb/usb-audio-pcm.c tests/usb-audio-stream-host-test.c \
 *     -o build/usb-audio-stream-host-test && ./build/usb-audio-stream-host-test
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <drivers/usb/usb-audio-pcm.h>

static void test_bresenham_44100_fs_and_hs(void);
static void test_feedback_10_14_and_16_16(void);
static void test_zero_copy_ring_planning(void);

int
main(
	void)
{
	test_bresenham_44100_fs_and_hs();
	test_feedback_10_14_and_16_16();
	test_zero_copy_ring_planning();
	printf("usb-audio-stream-host-test: all tests passed\n");
	return 0;
}

/* Verifies exact average sample counts for 44.1 kHz at FS (1ms) and HS (125us). */
static void
test_bresenham_44100_fs_and_hs(
	void)
{
	struct uac_bresenham state;
	uint32_t total;
	unsigned i;
	uint32_t f;

	usb_audio_bresenham_init(&state, 44100U, 1000U);
	total = 0U;
	for (i = 0U; i < 1000U; i++) {
		f = usb_audio_bresenham_next(&state);
		assert(f == 44U || f == 45U);
		total += f;
	}
	assert(total == 44100U);

	usb_audio_bresenham_init(&state, 44100U, 8000U);
	total = 0U;
	for (i = 0U; i < 8000U; i++) {
		f = usb_audio_bresenham_next(&state);
		assert(f == 5U || f == 6U);
		total += f;
	}
	assert(total == 44100U);
}

/* Verifies 10.14 (3-byte) and 16.16 (4-byte) feedback decoding and bounds. */
static void
test_feedback_10_14_and_16_16(
	void)
{
	/* 44.100 in 10.14 is 44.1 * 16384 = 722534.4 -> 0x0b0666 */
	uint8_t fs_441[3] = { 0x66U, 0x06U, 0x0bU };
	/* 48.000 in 16.16 is 48 * 65536 = 0x00300000 */
	uint8_t hs_480[4] = { 0x00U, 0x00U, 0x30U, 0x00U };
	uint8_t bad_zero[3] = { 0x00U, 0x00U, 0x00U };
	uint8_t bad_high[4] = { 0x00U, 0x00U, 0x60U, 0x00U };
	uint32_t nominal_441;
	uint32_t nominal_480;
	uint32_t q16;
	uint32_t accum;
	uint32_t total;
	unsigned i;

	nominal_441 = (44100U << 16) / 1000U;
	assert(usb_audio_decode_feedback(fs_441, sizeof(fs_441), nominal_441, &q16) == 0);
	assert(q16 == (0x0b0666U << 2));

	accum = 0U;
	total = 0U;
	for (i = 0U; i < 1000U; i++)
		total += usb_audio_feedback_next(q16, &accum);
	assert(total >= 44099U && total <= 44101U);

	nominal_480 = 48U << 16;
	assert(usb_audio_decode_feedback(hs_480, sizeof(hs_480), nominal_480, &q16) == 0);
	assert(q16 == (48U << 16));

	assert(usb_audio_decode_feedback(bad_zero, sizeof(bad_zero), nominal_441, &q16) != 0);
	assert(usb_audio_decode_feedback(bad_high, sizeof(bad_high), nominal_480, &q16) != 0);
}

/* Verifies zero-copy ring packet planning both inside and across wraparound. */
static void
test_zero_copy_ring_planning(
	void)
{
	uint32_t pkt_bytes[4] = { 192U, 192U, 192U, 192U };
	uint32_t iso_off[4];
	uint32_t iso_len[4];
	uint32_t buf_off;
	uint32_t buf_len;
	uint32_t total_ring;
	uint32_t next_pos;

	/* Contiguous case: no wraparound. */
	usb_audio_plan_iso_urb(1024U, 32768U, 4U, 4U, pkt_bytes,
	    &buf_off, &buf_len, iso_off, iso_len, &total_ring, &next_pos);
	assert(buf_off == 1024U);
	assert(buf_len == 768U);
	assert(iso_off[0] == 0U && iso_off[1] == 192U && iso_off[2] == 384U && iso_off[3] == 576U);
	assert(total_ring == 768U);
	assert(next_pos == 1792U);

	/* Wraparound case: 32768 - 32480 = 288 bytes remaining before wrap. */
	usb_audio_plan_iso_urb(32480U, 32768U, 4U, 4U, pkt_bytes,
	    &buf_off, &buf_len, iso_off, iso_len, &total_ring, &next_pos);
	assert(buf_off == 0U);
	assert(buf_len == 32768U);
	assert(iso_off[0] == 32480U && iso_len[0] == 192U);
	assert(iso_off[1] == 32672U && iso_len[1] == 96U);
	assert(iso_off[2] == 0U && iso_len[2] == 192U);
	assert(iso_off[3] == 192U && iso_len[3] == 192U);
	assert(total_ring == 672U);
	assert(next_pos == 384U);

	/* Sub-frame tail case for S24_3LE (6-byte frame, 2 bytes remaining at 32766). */
	usb_audio_plan_iso_urb(32766U, 32768U, 6U, 2U, pkt_bytes,
	    &buf_off, &buf_len, iso_off, iso_len, &total_ring, &next_pos);
	assert(buf_off == 0U);
	assert(buf_len == 32768U);
	assert(iso_off[0] == 0U && iso_len[0] == 192U);
	assert(iso_off[1] == 192U && iso_len[1] == 192U);
	assert(next_pos == 384U);
}
