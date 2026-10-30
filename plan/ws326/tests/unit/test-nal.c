/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the NAL unit functions of the software H.264 decoder against
 * Annex B (start codes and trailing zero bytes), 7.3.1 (the NAL unit
 * header) and 7.4.1 (emulation prevention).  The byte streams are written
 * out by hand or escaped by the bit writer from the rule of 7.4.1.
 */

#include "check.h"
#include "bitwriter.h"

#include "userland/desktop/libmedia/h264sw-nal.h"

#include <errno.h>

/* Checks the byte stream cases of vcodec_nal_find. */
static void test_find(void);

/* Checks the header fields and the removal of emulation prevention bytes. */
static void test_parse(void);

/* Checks the errors of h264sw_nal_parse and the types with a longer header. */
static void test_parse_error(void);

/* Checks that find and parse together recover RBSPs the bit writer escaped. */
static void test_round_trip(void);

/* Checks that arbitrary bytes never make find or parse leave their buffers. */
static void test_garbage(void);

/* Checks the byte stream cases of vcodec_nal_find. */
static void
test_find(
	void)
{
	/*
	 * A garbage byte pair, leading zeros, a four-byte start code and a
	 * NAL unit (67 42 01); a three-byte start code and a NAL unit
	 * (68 CE) followed by trailing zero bytes; a four-byte start code and
	 * a NAL unit (65 88) with trailing zero bytes at the end.
	 */
	static const uint8_t stream[] = {
		0xAAU, 0xBBU, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x67U, 0x42U, 0x01U,
		0x00U, 0x00U, 0x01U, 0x68U, 0xCEU, 0x00U, 0x00U, 0x00U,
		0x00U, 0x00U, 0x00U, 0x01U, 0x65U, 0x88U, 0x00U, 0x00U, 0x00U
	};
	static const uint8_t twice[] = { 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x01U, 0x41U, 0x99U };
	static const uint8_t inner[] = { 0x00U, 0x00U, 0x01U, 0x65U, 0x00U, 0x00U, 0x03U, 0x01U, 0xAAU };
	static const uint8_t none[] = { 0x01U, 0x02U, 0x00U, 0x00U, 0x02U, 0x00U, 0x01U, 0x00U, 0x00U };
	static const uint8_t only_prefix[] = { 0x00U, 0x00U, 0x01U };
	const uint8_t *nal;
	size_t nal_size;
	size_t cursor;
	int found;

	/* The first NAL unit follows the four-byte start code; the cursor stops at the next prefix. */
	cursor = 0U;
	found = vcodec_nal_find(stream, sizeof(stream), &cursor, &nal, &nal_size);
	CHECK(found == 1);
	CHECK(nal == stream + 7);
	CHECK(nal_size == 3U);
	CHECK(cursor == 10U);

	/* The second ends before its trailing zero bytes and the zero_byte of the next start code. */
	found = vcodec_nal_find(stream, sizeof(stream), &cursor, &nal, &nal_size);
	CHECK(found == 1);
	CHECK(nal == stream + 13);
	CHECK(nal_size == 2U);
	CHECK(cursor == 19U);

	/* The third runs to the end of the data without its trailing zero bytes. */
	found = vcodec_nal_find(stream, sizeof(stream), &cursor, &nal, &nal_size);
	CHECK(found == 1);
	CHECK(nal == stream + 22);
	CHECK(nal_size == 2U);
	CHECK(cursor == sizeof(stream));

	/* Nothing is left: 0, and the cursor stays at the end. */
	found = vcodec_nal_find(stream, sizeof(stream), &cursor, &nal, &nal_size);
	CHECK(found == 0);
	CHECK(cursor == sizeof(stream));

	/* Two start codes in a row delimit an empty NAL unit, which is skipped. */
	cursor = 0U;
	found = vcodec_nal_find(twice, sizeof(twice), &cursor, &nal, &nal_size);
	CHECK(found == 1);
	CHECK(nal == twice + 6);
	CHECK(nal_size == 2U);
	CHECK(cursor == sizeof(twice));

	/* Bytes 00 00 03 inside a NAL unit are not a start code. */
	cursor = 0U;
	found = vcodec_nal_find(inner, sizeof(inner), &cursor, &nal, &nal_size);
	CHECK(found == 1);
	CHECK(nal == inner + 3);
	CHECK(nal_size == 6U);

	/* No start code: 0, and the cursor goes to the end. */
	cursor = 0U;
	found = vcodec_nal_find(none, sizeof(none), &cursor, &nal, &nal_size);
	CHECK(found == 0);
	CHECK(cursor == sizeof(none));

	/* A start code with nothing after it delimits an empty NAL unit. */
	cursor = 0U;
	found = vcodec_nal_find(only_prefix, sizeof(only_prefix), &cursor, &nal, &nal_size);
	CHECK(found == 0);
	CHECK(cursor == sizeof(only_prefix));

	/* A cursor in the middle starts the search there. */
	cursor = 8U;
	found = vcodec_nal_find(stream, sizeof(stream), &cursor, &nal, &nal_size);
	CHECK(found == 1);
	CHECK(nal == stream + 13);

	/* A cursor beyond the data, and no data at all, find nothing. */
	cursor = 1000U;
	found = vcodec_nal_find(stream, sizeof(stream), &cursor, &nal, &nal_size);
	CHECK(found == 0);
	CHECK(cursor == sizeof(stream));
	cursor = 0U;
	found = vcodec_nal_find(NULL, 0U, &cursor, &nal, &nal_size);
	CHECK(found == 0);
	CHECK(cursor == 0U);
}

/* Checks the header fields and the removal of emulation prevention bytes. */
static void
test_parse(
	void)
{
	uint8_t buffer[64];
	struct h264sw_nal out;
	int error;

	/* A header only: nal_ref_idc 3, nal_unit_type 7, and an empty RBSP. */
	static const uint8_t header_only[] = { 0x67U };
	/* An escape 00 00 03 01 gives 00 00 01. */
	static const uint8_t escaped[] = { 0x65U, 0x11U, 0x00U, 0x00U, 0x03U, 0x01U, 0x22U };
	/* The escapes before 00, 01, 02 and 03 each lose their 03. */
	static const uint8_t four[] = {
		0x41U, 0x00U, 0x00U, 0x03U, 0x00U, 0x07U, 0x00U, 0x00U, 0x03U, 0x01U, 0x07U,
		0x00U, 0x00U, 0x03U, 0x02U, 0x07U, 0x00U, 0x00U, 0x03U, 0x03U, 0x07U
	};
	/* A final 00 00 03: the cabac_zero_word case. */
	static const uint8_t cabac_zero[] = { 0x25U, 0xAAU, 0x80U, 0x00U, 0x00U, 0x03U };
	/* Only the final 03 after two zero bytes. */
	static const uint8_t only_final[] = { 0x65U, 0x00U, 0x00U, 0x03U };
	/* An escape followed by two zero bytes and a final 03. */
	static const uint8_t both[] = { 0x65U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x03U };
	/* A 03 not preceded by two zero bytes stays. */
	static const uint8_t plain03[] = { 0x65U, 0x00U, 0x03U, 0x03U };

	/* The header fields. */
	error = h264sw_nal_parse(header_only, sizeof(header_only), buffer, sizeof(buffer), &out);
	CHECK(error == 0);
	CHECK(out.type == 7U);
	CHECK(out.ref_idc == 3U);
	CHECK(out.rbsp_size == 0U);

	/* The removal of one escape byte. */
	error = h264sw_nal_parse(escaped, sizeof(escaped), buffer, sizeof(buffer), &out);
	CHECK(error == 0);
	CHECK(out.type == 5U);
	CHECK(out.ref_idc == 3U);
	CHECK(out.rbsp == buffer);
	CHECK(out.rbsp_size == 5U);
	CHECK(buffer[0] == 0x11U && buffer[1] == 0x00U && buffer[2] == 0x00U);
	CHECK(buffer[3] == 0x01U && buffer[4] == 0x22U);

	/* The escapes before 0, 1, 2 and 3. */
	error = h264sw_nal_parse(four, sizeof(four), buffer, sizeof(buffer), &out);
	CHECK(error == 0);
	CHECK(out.type == 1U);
	CHECK(out.ref_idc == 2U);
	CHECK(out.rbsp_size == 16U);
	CHECK(buffer[0] == 0x00U && buffer[1] == 0x00U && buffer[2] == 0x00U && buffer[3] == 0x07U);
	CHECK(buffer[4] == 0x00U && buffer[5] == 0x00U && buffer[6] == 0x01U && buffer[7] == 0x07U);
	CHECK(buffer[8] == 0x00U && buffer[9] == 0x00U && buffer[10] == 0x02U && buffer[11] == 0x07U);
	CHECK(buffer[12] == 0x00U && buffer[13] == 0x00U && buffer[14] == 0x03U && buffer[15] == 0x07U);

	/* A NAL unit ending in 00 00 03 is valid and its final 03 is dropped. */
	error = h264sw_nal_parse(cabac_zero, sizeof(cabac_zero), buffer, sizeof(buffer), &out);
	CHECK(error == 0);
	CHECK(out.type == 5U);
	CHECK(out.ref_idc == 1U);
	CHECK(out.rbsp_size == 4U);
	CHECK(buffer[0] == 0xAAU && buffer[1] == 0x80U && buffer[2] == 0x00U && buffer[3] == 0x00U);
	error = h264sw_nal_parse(only_final, sizeof(only_final), buffer, sizeof(buffer), &out);
	CHECK(error == 0);
	CHECK(out.rbsp_size == 2U);
	error = h264sw_nal_parse(both, sizeof(both), buffer, sizeof(buffer), &out);
	CHECK(error == 0);
	CHECK(out.rbsp_size == 4U);

	/* A 03 after a single zero byte is data. */
	error = h264sw_nal_parse(plain03, sizeof(plain03), buffer, sizeof(buffer), &out);
	CHECK(error == 0);
	CHECK(out.rbsp_size == 3U);
	CHECK(buffer[0] == 0x00U && buffer[1] == 0x03U && buffer[2] == 0x03U);
}

/* Checks the errors of h264sw_nal_parse and the types with a longer header. */
static void
test_parse_error(
	void)
{
	uint8_t buffer[64];
	struct h264sw_nal out;
	int error;
	static const uint8_t forbidden[] = { 0xE5U, 0x11U };
	static const uint8_t byte_02[] = { 0x65U, 0x11U, 0x00U, 0x00U, 0x02U };
	static const uint8_t byte_01[] = { 0x65U, 0x11U, 0x00U, 0x00U, 0x01U };
	static const uint8_t byte_00[] = { 0x65U, 0x11U, 0x00U, 0x00U, 0x00U };
	static const uint8_t over_03[] = { 0x65U, 0x11U, 0x00U, 0x00U, 0x03U, 0x04U };
	static const uint8_t four_bytes[] = { 0x65U, 0x01U, 0x02U, 0x03U, 0x04U };
	static const uint8_t prefix[] = { 0x6EU, 0xFFU, 0x00U, 0x00U, 0x00U };
	static const uint8_t extension[] = { 0x14U, 0x00U, 0x00U, 0x01U };
	static const uint8_t depth[] = { 0x35U, 0x00U, 0x00U, 0x04U };

	/* A NAL unit of 0 bytes. */
	error = h264sw_nal_parse(forbidden, 0U, buffer, sizeof(buffer), &out);
	CHECK(error == EINVAL);

	/* forbidden_zero_bit 1. */
	error = h264sw_nal_parse(forbidden, sizeof(forbidden), buffer, sizeof(buffer), &out);
	CHECK(error == EINVAL);

	/* Sequences that cannot occur in a NAL unit: 00 00 00, 00 00 01, 00 00 02, 00 00 03 04. */
	error = h264sw_nal_parse(byte_00, sizeof(byte_00), buffer, sizeof(buffer), &out);
	CHECK(error == EINVAL);
	error = h264sw_nal_parse(byte_01, sizeof(byte_01), buffer, sizeof(buffer), &out);
	CHECK(error == EINVAL);
	error = h264sw_nal_parse(byte_02, sizeof(byte_02), buffer, sizeof(buffer), &out);
	CHECK(error == EINVAL);
	error = h264sw_nal_parse(over_03, sizeof(over_03), buffer, sizeof(buffer), &out);
	CHECK(error == EINVAL);

	/* A capacity smaller than the RBSP; an exact capacity is enough. */
	error = h264sw_nal_parse(four_bytes, sizeof(four_bytes), buffer, 3U, &out);
	CHECK(error == ENOBUFS);
	error = h264sw_nal_parse(four_bytes, sizeof(four_bytes), buffer, 4U, &out);
	CHECK(error == 0);
	CHECK(out.rbsp_size == 4U);
	error = h264sw_nal_parse(four_bytes, sizeof(four_bytes), NULL, 0U, &out);
	CHECK(error == ENOBUFS);

	/* Types 14, 20 and 21: only the header is read, whatever follows. */
	error = h264sw_nal_parse(prefix, sizeof(prefix), buffer, sizeof(buffer), &out);
	CHECK(error == 0);
	CHECK(out.type == 14U);
	CHECK(out.ref_idc == 3U);
	CHECK(out.rbsp_size == 0U);
	error = h264sw_nal_parse(extension, sizeof(extension), buffer, sizeof(buffer), &out);
	CHECK(error == 0);
	CHECK(out.type == 20U);
	CHECK(out.ref_idc == 0U);
	CHECK(out.rbsp_size == 0U);
	error = h264sw_nal_parse(depth, sizeof(depth), buffer, sizeof(buffer), &out);
	CHECK(error == 0);
	CHECK(out.type == 21U);
	CHECK(out.ref_idc == 1U);
	CHECK(out.rbsp_size == 0U);
}

/* Checks that find and parse together recover RBSPs the bit writer escaped. */
static void
test_round_trip(
	void)
{
	static uint8_t rbsp[3000];
	static uint8_t nal[8192];
	static uint8_t stream[8192 + 16];
	static uint8_t buffer[8192];
	const uint8_t *found_nal;
	size_t found_size;
	size_t cursor;
	size_t nal_size;
	size_t index;
	size_t used;
	struct h264sw_nal out;
	int error;
	int found;

	/* An RBSP with runs of zero bytes, small values and ordinary bytes, ending in zeros. */
	for (index = 0U; index < sizeof(rbsp); index++) {
		if (index % 7U < 4U) {
			rbsp[index] = 0U;
		} else if (index % 7U == 4U) {
			rbsp[index] = (uint8_t)(index % 4U);
		} else {
			rbsp[index] = (uint8_t)(index * 31U + 5U);
		}
	}

	/* Ends the RBSP with two zero bytes, so the escaping also has to protect zeros at the end. */
	rbsp[sizeof(rbsp) - 1U] = 0U;
	rbsp[sizeof(rbsp) - 2U] = 0U;

	/* Escapes it into a NAL unit and puts it in a stream between start codes. */
	nal_size = bitwriter_nal(2U, 1U, rbsp, sizeof(rbsp), nal, sizeof(nal));
	CHECK(nal_size > sizeof(rbsp));
	stream[0] = 0U;
	stream[1] = 0U;
	stream[2] = 0U;
	stream[3] = 1U;
	memcpy(stream + 4, nal, nal_size);
	used = 4U + nal_size;
	stream[used] = 0U;
	used++;

	/* The NAL unit is found whole: the escaping never makes a start code. */
	cursor = 0U;
	found = vcodec_nal_find(stream, used, &cursor, &found_nal, &found_size);
	CHECK(found == 1);
	CHECK(found_size == nal_size);
	CHECK(memcmp(found_nal, nal, nal_size) == 0);

	/* And parsing it returns the original RBSP. */
	error = h264sw_nal_parse(found_nal, found_size, buffer, sizeof(buffer), &out);
	CHECK(error == 0);
	CHECK(out.type == 1U);
	CHECK(out.ref_idc == 2U);
	CHECK(out.rbsp_size == sizeof(rbsp));
	CHECK(memcmp(out.rbsp, rbsp, sizeof(rbsp)) == 0);
}

/* Checks that arbitrary bytes never make find or parse leave their buffers. */
static void
test_garbage(
	void)
{
	uint8_t bytes[40];
	uint8_t buffer[40];
	const uint8_t *nal;
	size_t nal_size;
	size_t cursor;
	size_t previous;
	struct h264sw_nal out;
	uint32_t state;
	unsigned round;
	unsigned index;
	unsigned size;
	int stuck;
	int bad_range;
	int found;
	int error;

	/* A fixed linear congruential sequence makes the streams repeatable. */
	state = 987U;
	stuck = 0;
	bad_range = 0;
	for (round = 0U; round < 3000U; round++) {
		/* Bytes drawn mostly from 0, 1, 2 and 3 so the start codes and escapes occur. */
		state = state * 1664525U + 1013904223U;
		size = (state >> 16) % 41U;
		for (index = 0U; index < size; index++) {
			state = state * 1664525U + 1013904223U;
			bytes[index] = (uint8_t)((state >> 24) % 5U);
		}

		/* Every NAL unit found lies inside the data, and the cursor advances. */
		cursor = 0U;
		found = 1;
		while (found != 0) {
			previous = cursor;
			found = vcodec_nal_find(bytes, size, &cursor, &nal, &nal_size);
			if (found == 0)
				break;
			if (cursor <= previous)
				stuck = 1;
			if (nal < bytes || nal_size == 0U || nal + nal_size > bytes + size)
				bad_range = 1;

			/* Parsing it either fails with a known error or fits in its own size. */
			error = h264sw_nal_parse(nal, nal_size, buffer, sizeof(buffer), &out);
			if (error != 0 && error != EINVAL && error != ENOBUFS)
				bad_range = 1;
			if (error == 0 && out.rbsp_size > nal_size)
				bad_range = 1;
		}
	}

	/* Requires that every search advanced and every result stayed inside its buffer. */
	CHECK(stuck == 0);
	CHECK(bad_range == 0);
}

/* Runs the checks. */
int
main(
	void)
{
	int status;

	/* Runs every group of checks. */
	test_find();
	test_parse();
	test_parse_error();
	test_round_trip();
	test_garbage();

	/* Reports the result. */
	status = check_finish("nal");
	return status;
}
