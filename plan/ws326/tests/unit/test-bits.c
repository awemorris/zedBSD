/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the bit reader of the software H.264 decoder against the
 * descriptors of 7.2, the Exp-Golomb codes of 9.1 and 9.1.1, and the
 * RBSP trailing bits of 7.3.2.11.  Inputs are built with the bit writer
 * from the syntax, or written out by hand from the bit strings.
 */

#include "check.h"
#include "bitwriter.h"

#include "userland/desktop/libmedia/h264sw-bits.h"

#include <errno.h>

/* The values that cross the boundaries of the Exp-Golomb length classes. */
static const uint32_t ue_values[] = {
	0U, 1U, 2U, 254U, 255U, 65536U, 0x7FFFFFFFU, 0xFFFFFFFEU
};

/* Starts a reader over everything a writer holds. */
static void
reader_from_writer(
	struct h264sw_bits *bits,
	const struct bitwriter *writer)
{
	/* The written bytes, with the last partial byte zero padded. */
	h264sw_bits_init(bits, writer->data, bitwriter_bytes(writer));
}

/* Checks u(n) at the widths 0, 1, 17 and 32 and across byte boundaries. */
static void
test_u(
	void)
{
	/*
	 * Bytes A5 5A C3 3C 81 7E F0 are the bits
	 * 10100101 01011010 11000011 00111100 10000001 01111110 11110000.
	 */
	static const uint8_t bytes[] = { 0xA5U, 0x5AU, 0xC3U, 0x3CU, 0x81U, 0x7EU, 0xF0U };
	struct h264sw_bits bits;
	struct bitwriter writer;
	uint32_t value;

	/* Zero bits read nothing, with no error. */
	h264sw_bits_init(&bits, bytes, sizeof(bytes));
	value = h264sw_bits_u(&bits, 0U);
	CHECK(value == 0U);
	CHECK(bits.error == 0);
	CHECK(bits.position == 0U);

	/* One bit is the top bit of the first byte. */
	value = h264sw_bits_u(&bits, 1U);
	CHECK(value == 1U);
	CHECK(bits.position == 1U);

	/* 17 bits from bit 1: 0 1001 0101 0110 1011 (bits 1 to 17). */
	value = h264sw_bits_u(&bits, 17U);
	CHECK(value == 0x956BU);
	CHECK(bits.position == 18U);

	/* 32 bits from bit 18: 0000 1100 1111 0010 0000 0101 1111 1011. */
	value = h264sw_bits_u(&bits, 32U);
	CHECK(value == 0x0CF205FBU);
	CHECK(bits.position == 50U);
	CHECK(bits.error == 0);
	CHECK(h264sw_bits_left(&bits) == 6U);
	CHECK(h264sw_bits_aligned(&bits) == 0);

	/* The u1 read is the same as a one-bit u(n): bit 50 of the data is 1. */
	value = h264sw_bits_u1(&bits);
	CHECK(value == 1U);
	CHECK(bits.position == 51U);

	/* A 32-bit read that starts inside a byte, from the first byte's bit 7. */
	h264sw_bits_init(&bits, bytes, sizeof(bytes));
	value = h264sw_bits_u(&bits, 7U);
	CHECK(value == 0x52U);
	value = h264sw_bits_u(&bits, 32U);
	CHECK(value == 0xAD619E40U);
	CHECK(bits.error == 0);

	/* Written fields come back whatever their width. */
	bitwriter_init(&writer);
	bitwriter_u(&writer, 1U, 1U);
	bitwriter_u(&writer, 0x1ABCDU, 17U);
	bitwriter_u(&writer, 0xDEADBEEFU, 32U);
	bitwriter_u(&writer, 0x5U, 3U);
	bitwriter_trailing(&writer);
	reader_from_writer(&bits, &writer);
	CHECK(h264sw_bits_u(&bits, 1U) == 1U);
	CHECK(h264sw_bits_u(&bits, 17U) == 0x1ABCDU);
	CHECK(h264sw_bits_u(&bits, 32U) == 0xDEADBEEFU);
	CHECK(h264sw_bits_u(&bits, 3U) == 0x5U);
	CHECK(bits.error == 0);
	CHECK(h264sw_bits_trailing(&bits) == 0);
}

/* Checks the sticky error of a read past the end and of a width above 32. */
static void
test_u_error(
	void)
{
	static const uint8_t bytes[] = { 0xFFU, 0xFFU };
	struct h264sw_bits bits;
	uint32_t value;

	/* Reading exactly to the end is fine. */
	h264sw_bits_init(&bits, bytes, sizeof(bytes));
	value = h264sw_bits_u(&bits, 16U);
	CHECK(value == 0xFFFFU);
	CHECK(bits.error == 0);
	CHECK(h264sw_bits_left(&bits) == 0U);

	/* One more bit fails: 0, an error, and the position stays. */
	value = h264sw_bits_u(&bits, 1U);
	CHECK(value == 0U);
	CHECK(bits.error != 0);
	CHECK(bits.position == 16U);

	/* The error is sticky for every kind of read. */
	CHECK(h264sw_bits_u(&bits, 0U) == 0U);
	CHECK(h264sw_bits_u1(&bits) == 0U);
	CHECK(h264sw_bits_ue(&bits) == 0U);
	CHECK(h264sw_bits_se(&bits) == 0);
	CHECK(h264sw_bits_te(&bits, 1U) == 0U);
	CHECK(h264sw_bits_peek(&bits, 8U) == 0U);
	CHECK(h264sw_bits_more_rbsp_data(&bits) == 0);
	CHECK(h264sw_bits_trailing(&bits) == EINVAL);

	/* A read that is partly past the end fails whole and leaves the position. */
	h264sw_bits_init(&bits, bytes, sizeof(bytes));
	value = h264sw_bits_u(&bits, 8U);
	CHECK(value == 0xFFU);
	value = h264sw_bits_u(&bits, 9U);
	CHECK(value == 0U);
	CHECK(bits.error != 0);
	CHECK(bits.position == 8U);

	/* A width above 32 is a mistake of the caller. */
	h264sw_bits_init(&bits, bytes, sizeof(bytes));
	value = h264sw_bits_u(&bits, 33U);
	CHECK(value == 0U);
	CHECK(bits.error != 0);
	CHECK(bits.position == 0U);

	/* An empty RBSP: the reader starts without error and its first read fails. */
	h264sw_bits_init(&bits, NULL, 0U);
	CHECK(bits.error == 0);
	CHECK(h264sw_bits_more_rbsp_data(&bits) == 0);
	value = h264sw_bits_u1(&bits);
	CHECK(value == 0U);
	CHECK(bits.error != 0);
}

/* Checks ue(v) and se(v) on values that cross the length classes of Table 9-1. */
static void
test_exp_golomb(
	void)
{
	struct h264sw_bits bits;
	struct bitwriter writer;
	unsigned index;
	uint32_t code;
	int32_t signed_value;
	static const int32_t se_values[] = {
		0, 1, -1, 2, -2, 127, -128, 255, -256, 32768, -32768,
		2147483647L, -2147483647L
	};

	/* A sequence of ue(v) codes in one RBSP, read back in order. */
	bitwriter_init(&writer);
	for (index = 0U; index < sizeof(ue_values) / sizeof(ue_values[0]); index++)
		bitwriter_ue(&writer, ue_values[index]);
	bitwriter_trailing(&writer);
	CHECK(writer.overflow == 0);
	reader_from_writer(&bits, &writer);
	for (index = 0U; index < sizeof(ue_values) / sizeof(ue_values[0]); index++) {
		code = h264sw_bits_ue(&bits);
		CHECK(code == ue_values[index]);
		CHECK(bits.error == 0);
	}
	CHECK(h264sw_bits_more_rbsp_data(&bits) == 0);
	CHECK(h264sw_bits_trailing(&bits) == 0);

	/* A sequence of se(v) codes. */
	bitwriter_init(&writer);
	for (index = 0U; index < sizeof(se_values) / sizeof(se_values[0]); index++)
		bitwriter_se(&writer, se_values[index]);
	bitwriter_trailing(&writer);
	CHECK(writer.overflow == 0);
	reader_from_writer(&bits, &writer);
	for (index = 0U; index < sizeof(se_values) / sizeof(se_values[0]); index++) {
		signed_value = h264sw_bits_se(&bits);
		CHECK(signed_value == se_values[index]);
		CHECK(bits.error == 0);
	}

	/* Table 9-3 on codeNum 0 to 6: 0, 1, -1, 2, -2, 3, -3. */
	bitwriter_init(&writer);
	for (index = 0U; index < 7U; index++)
		bitwriter_ue(&writer, index);
	bitwriter_trailing(&writer);
	reader_from_writer(&bits, &writer);
	CHECK(h264sw_bits_se(&bits) == 0);
	CHECK(h264sw_bits_se(&bits) == 1);
	CHECK(h264sw_bits_se(&bits) == -1);
	CHECK(h264sw_bits_se(&bits) == 2);
	CHECK(h264sw_bits_se(&bits) == -2);
	CHECK(h264sw_bits_se(&bits) == 3);
	CHECK(h264sw_bits_se(&bits) == -3);
	CHECK(bits.error == 0);

	/* The largest accepted codeNum, 2^32 - 2, is -(2^31 - 1); 2^32 - 3 is 2^31 - 1. */
	bitwriter_init(&writer);
	bitwriter_ue(&writer, 0xFFFFFFFEU);
	bitwriter_ue(&writer, 0xFFFFFFFDU);
	bitwriter_trailing(&writer);
	reader_from_writer(&bits, &writer);
	CHECK(h264sw_bits_se(&bits) == -2147483647L);
	CHECK(h264sw_bits_se(&bits) == 2147483647L);
	CHECK(bits.error == 0);
}

/* Checks ue(v) and se(v) on codes that are too long or cut short. */
static void
test_exp_golomb_error(
	void)
{
	struct h264sw_bits bits;
	struct bitwriter writer;
	static const uint8_t zeros[] = { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U };

	/* 32 leading zero bits (codeNum 2^32 - 1 and above) are refused. */
	bitwriter_init(&writer);
	bitwriter_u(&writer, 0U, 32U);
	bitwriter_u(&writer, 1U, 1U);
	bitwriter_u(&writer, 0U, 32U);
	bitwriter_trailing(&writer);
	reader_from_writer(&bits, &writer);
	CHECK(h264sw_bits_ue(&bits) == 0U);
	CHECK(bits.error != 0);

	/* The same for se(v). */
	reader_from_writer(&bits, &writer);
	CHECK(h264sw_bits_se(&bits) == 0);
	CHECK(bits.error != 0);

	/* 31 leading zeros with a full suffix is codeNum 2^32 - 2 and is accepted. */
	bitwriter_init(&writer);
	bitwriter_u(&writer, 0U, 31U);
	bitwriter_u(&writer, 1U, 1U);
	bitwriter_u(&writer, 0x7FFFFFFFU, 31U);
	bitwriter_trailing(&writer);
	reader_from_writer(&bits, &writer);
	CHECK(h264sw_bits_ue(&bits) == 0xFFFFFFFEU);
	CHECK(bits.error == 0);

	/* A prefix with no 1 before the data ends. */
	h264sw_bits_init(&bits, zeros, sizeof(zeros));
	CHECK(h264sw_bits_ue(&bits) == 0U);
	CHECK(bits.error != 0);

	/* A code whose suffix is cut off: 00001 and only two suffix bits. */
	bitwriter_init(&writer);
	bitwriter_u(&writer, 1U, 8U);
	reader_from_writer(&bits, &writer);
	h264sw_bits_skip(&bits, 3U);
	CHECK(h264sw_bits_ue(&bits) == 0U);
	CHECK(bits.error != 0);
}

/* Checks te(v) for a range of 0, 1 and above 1. */
static void
test_te(
	void)
{
	struct h264sw_bits bits;
	struct bitwriter writer;

	/* Range 1: the bit is inverted, so a 0 bit is 1 and a 1 bit is 0. */
	bitwriter_init(&writer);
	bitwriter_u(&writer, 0U, 1U);
	bitwriter_u(&writer, 1U, 1U);
	bitwriter_trailing(&writer);
	reader_from_writer(&bits, &writer);
	CHECK(h264sw_bits_te(&bits, 1U) == 1U);
	CHECK(h264sw_bits_te(&bits, 1U) == 0U);
	CHECK(bits.error == 0);
	CHECK(bits.position == 2U);

	/* Range 2 and above: ue(v). */
	bitwriter_init(&writer);
	bitwriter_ue(&writer, 2U);
	bitwriter_ue(&writer, 0U);
	bitwriter_ue(&writer, 15U);
	bitwriter_trailing(&writer);
	reader_from_writer(&bits, &writer);
	CHECK(h264sw_bits_te(&bits, 2U) == 2U);
	CHECK(h264sw_bits_te(&bits, 3U) == 0U);
	CHECK(h264sw_bits_te(&bits, 31U) == 15U);
	CHECK(bits.error == 0);

	/* Range 0 means the element is absent: reading it is an error. */
	reader_from_writer(&bits, &writer);
	CHECK(h264sw_bits_te(&bits, 0U) == 0U);
	CHECK(bits.error != 0);

	/* Range 1 at the end of the data is an error, not the inverted 0. */
	h264sw_bits_init(&bits, NULL, 0U);
	CHECK(h264sw_bits_te(&bits, 1U) == 0U);
	CHECK(bits.error != 0);
}

/* Checks peek, skip, left and aligned. */
static void
test_peek_skip(
	void)
{
	static const uint8_t bytes[] = { 0xF0U, 0x0FU, 0x81U };
	struct h264sw_bits bits;

	/* Peek returns the next bits and leaves the position. */
	h264sw_bits_init(&bits, bytes, sizeof(bytes));
	CHECK(h264sw_bits_peek(&bits, 12U) == 0xF00U);
	CHECK(bits.position == 0U);
	CHECK(h264sw_bits_peek(&bits, 0U) == 0U);
	CHECK(h264sw_bits_peek(&bits, 32U) == 0xF00F8100U);

	/* Skip moves the position, and peek follows it. */
	h264sw_bits_skip(&bits, 4U);
	CHECK(bits.position == 4U);
	CHECK(h264sw_bits_peek(&bits, 8U) == 0x00U);
	CHECK(h264sw_bits_aligned(&bits) == 0);
	h264sw_bits_skip(&bits, 4U);
	CHECK(h264sw_bits_aligned(&bits) == 1);
	CHECK(h264sw_bits_left(&bits) == 16U);

	/* Peeking beyond the end reads zero bits and is not an error. */
	h264sw_bits_skip(&bits, 12U);
	CHECK(h264sw_bits_peek(&bits, 8U) == 0x10U);
	CHECK(h264sw_bits_peek(&bits, 32U) == 0x10000000U);
	h264sw_bits_skip(&bits, 4U);
	CHECK(h264sw_bits_peek(&bits, 16U) == 0U);
	CHECK(bits.error == 0);

	/* Skipping past the end parks the position at the end and sets the error. */
	h264sw_bits_init(&bits, bytes, sizeof(bytes));
	h264sw_bits_skip(&bits, 24U);
	CHECK(bits.error == 0);
	CHECK(bits.position == 24U);
	h264sw_bits_init(&bits, bytes, sizeof(bytes));
	h264sw_bits_skip(&bits, 25U);
	CHECK(bits.error != 0);
	CHECK(bits.position == 24U);
	CHECK(h264sw_bits_left(&bits) == 0U);

	/* A huge skip does not wrap around. */
	h264sw_bits_init(&bits, bytes, sizeof(bytes));
	h264sw_bits_skip(&bits, (size_t)-1);
	CHECK(bits.error != 0);
	CHECK(bits.position == 24U);
}

/* Checks more_rbsp_data and the trailing bits. */
static void
test_trailing(
	void)
{
	struct bitwriter writer;
	struct h264sw_bits bits;
	uint8_t padded[5];
	static const uint8_t no_stop[] = { 0x00U, 0x00U };

	/* Three data bits 101, then the stop bit and zeros: 1011 0000. */
	bitwriter_init(&writer);
	bitwriter_u(&writer, 5U, 3U);
	bitwriter_trailing(&writer);
	reader_from_writer(&bits, &writer);
	CHECK(bits.stop == 3U);

	/* There is more data up to the stop bit and none at it. */
	CHECK(h264sw_bits_more_rbsp_data(&bits) == 1);
	h264sw_bits_u(&bits, 2U);
	CHECK(h264sw_bits_more_rbsp_data(&bits) == 1);
	h264sw_bits_u1(&bits);
	CHECK(h264sw_bits_more_rbsp_data(&bits) == 0);

	/* The trailing bits are valid there, and the reader ends byte aligned. */
	CHECK(h264sw_bits_trailing(&bits) == 0);
	CHECK(h264sw_bits_aligned(&bits) == 1);
	CHECK(bits.position == 8U);
	CHECK(h264sw_bits_left(&bits) == 0U);

	/* Trailing zero bytes (cabac_zero_word) do not move the stop bit. */
	memset(padded, 0, sizeof(padded));
	padded[0] = 0xB0U;
	h264sw_bits_init(&bits, padded, sizeof(padded));
	CHECK(bits.stop == 3U);
	h264sw_bits_skip(&bits, 3U);
	CHECK(h264sw_bits_more_rbsp_data(&bits) == 0);
	CHECK(h264sw_bits_trailing(&bits) == 0);
	CHECK(bits.position == 8U);

	/* The stop bit in the last bit of a byte leaves no zero bits after it. */
	padded[0] = 0x01U;
	h264sw_bits_init(&bits, padded, 1U);
	CHECK(bits.stop == 7U);
	h264sw_bits_skip(&bits, 7U);
	CHECK(h264sw_bits_trailing(&bits) == 0);
	CHECK(bits.position == 8U);

	/* A reader that is not at the stop bit is refused: before it (data left)... */
	padded[0] = 0xB0U;
	h264sw_bits_init(&bits, padded, 1U);
	CHECK(h264sw_bits_trailing(&bits) == EINVAL);

	/* ...on a bit equal to 1 that is not the last one... */
	h264sw_bits_skip(&bits, 2U);
	CHECK(h264sw_bits_trailing(&bits) == EINVAL);

	/* ...or after it. */
	h264sw_bits_skip(&bits, 2U);
	CHECK(h264sw_bits_trailing(&bits) == EINVAL);

	/* An RBSP with no bit equal to 1 has no stop bit. */
	h264sw_bits_init(&bits, no_stop, sizeof(no_stop));
	CHECK(bits.stop == 16U);
	CHECK(h264sw_bits_trailing(&bits) == EINVAL);
	h264sw_bits_skip(&bits, 16U);
	CHECK(h264sw_bits_trailing(&bits) == EINVAL);
}

/* Checks that arbitrary bytes and arbitrary read sequences never move the reader out of its data. */
static void
test_garbage(
	void)
{
	uint8_t bytes[24];
	struct h264sw_bits bits;
	uint32_t state;
	unsigned round;
	unsigned step;
	unsigned size;
	unsigned index;
	int outside;

	/* A fixed linear congruential sequence makes the bytes and the operations repeatable. */
	state = 12345U;
	outside = 0;
	for (round = 0U; round < 2000U; round++) {
		/* Fills a buffer of 0 to 24 bytes, with zero runs and ones in turn. */
		state = state * 1664525U + 1013904223U;
		size = (state >> 16) % 25U;
		for (index = 0U; index < size; index++) {
			state = state * 1664525U + 1013904223U;
			bytes[index] = (uint8_t)((state >> 24) & ((round % 3U == 0U) ? 0x03U : 0xFFU));
		}
		h264sw_bits_init(&bits, size == 0U ? NULL : bytes, size);

		/* Runs a few operations chosen by the sequence. */
		for (step = 0U; step < 12U; step++) {
			state = state * 1664525U + 1013904223U;
			switch ((state >> 20) % 8U) {
			case 0:
				h264sw_bits_u(&bits, (state >> 8) % 40U);
				break;
			case 1:
				h264sw_bits_ue(&bits);
				break;
			case 2:
				h264sw_bits_se(&bits);
				break;
			case 3:
				h264sw_bits_te(&bits, (state >> 8) % 5U);
				break;
			case 4:
				h264sw_bits_skip(&bits, (state >> 8) % 70U);
				break;
			case 5:
				h264sw_bits_peek(&bits, (state >> 8) % 33U);
				break;
			case 6:
				h264sw_bits_more_rbsp_data(&bits);
				break;
			default:
				h264sw_bits_trailing(&bits);
				break;
			}
			if (bits.position > bits.size * 8U)
				outside = 1;
		}
	}
	CHECK(outside == 0);
}

/* Runs the checks. */
int
main(
	void)
{
	test_u();
	test_u_error();
	test_exp_golomb();
	test_exp_golomb_error();
	test_te();
	test_peek_skip();
	test_trailing();
	test_garbage();

	/* Reports the result. */
	return check_finish("bits");
}
