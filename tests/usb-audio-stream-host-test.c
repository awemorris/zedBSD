/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host unit tests for Bresenham rate generation, feedback decoding, and
 * 24-bit packed <-> S32_LE conversion.
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
static void test_24bit_pack_unpack_roundtrip(void);

int
main(
	void)
{
	test_bresenham_44100_fs_and_hs();
	test_feedback_10_14_and_16_16();
	test_24bit_pack_unpack_roundtrip();
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

/* Verifies 24-bit packed <-> S32_LE roundtrip preserves upper 24 bits and sign. */
static void
test_24bit_pack_unpack_roundtrip(
	void)
{
	uint8_t src_s32[12] = {
		0x12U, 0x34U, 0x56U, 0x7fU,
		0xffU, 0x00U, 0x00U, 0x80U,
		0xabU, 0xcdU, 0xefU, 0x00U
	};
	uint8_t packed[9];
	uint8_t dst_s32[12];

	memset(packed, 0, sizeof(packed));
	memset(dst_s32, 0xff, sizeof(dst_s32));

	usb_audio_pack_24_from_s32(src_s32, packed, 3U);
	assert(packed[0] == 0x34U && packed[1] == 0x56U && packed[2] == 0x7fU);
	assert(packed[3] == 0x00U && packed[4] == 0x00U && packed[5] == 0x80U);
	assert(packed[6] == 0xcdU && packed[7] == 0xefU && packed[8] == 0x00U);

	usb_audio_unpack_24_to_s32(packed, dst_s32, 3U);
	assert(dst_s32[0] == 0x00U && dst_s32[1] == 0x34U && dst_s32[2] == 0x56U && dst_s32[3] == 0x7fU);
	assert(dst_s32[4] == 0x00U && dst_s32[5] == 0x00U && dst_s32[6] == 0x00U && dst_s32[7] == 0x80U);
	assert(dst_s32[8] == 0x00U && dst_s32[9] == 0xcdU && dst_s32[10] == 0xefU && dst_s32[11] == 0x00U);
}
