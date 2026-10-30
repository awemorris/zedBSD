/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the tests' own bit writer against the Exp-Golomb bit strings of
 * Table 9-2 and Table 9-3 and the escaping of 7.4.1, so the other tests
 * can trust the inputs it builds.
 */

#include "check.h"
#include "bitwriter.h"

/* Reports whether the writer holds exactly the bit string text (of '0' and '1'). */
static int bits_equal(const struct bitwriter *writer, const char *text);

/* Reports whether the writer holds exactly the bit string text (of '0' and '1'). */
static int
bits_equal(
	const struct bitwriter *writer,
	const char *text)
{
	size_t index;
	size_t length;
	unsigned bit;

	/* The lengths agree first. */
	length = strlen(text);
	if (length != writer->bits)
		return 0;

	/* Then every bit, most significant first. */
	for (index = 0U; index < length; index++) {
		bit = (writer->data[index / 8U] >> (7U - index % 8U)) & 1U;
		if (bit != (unsigned)(text[index] - '0'))
			return 0;
	}

	/* Succeeded: the strings are the same. */
	return 1;
}

/* Runs the checks. */
int
main(
	void)
{
	struct bitwriter writer;
	static const uint8_t rbsp[] = { 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U };
	uint8_t nal[16];
	size_t size;
	int status;

	/*
	 * Table 9-2: codeNum 0 is "1", 1 is "010", 2 is "011", 3 is "00100",
	 * 7 is "0001000".
	 */
	bitwriter_init(&writer);
	bitwriter_ue(&writer, 0U);
	CHECK(bits_equal(&writer, "1"));
	bitwriter_init(&writer);
	bitwriter_ue(&writer, 1U);
	CHECK(bits_equal(&writer, "010"));
	bitwriter_init(&writer);
	bitwriter_ue(&writer, 2U);
	CHECK(bits_equal(&writer, "011"));
	bitwriter_init(&writer);
	bitwriter_ue(&writer, 3U);
	CHECK(bits_equal(&writer, "00100"));
	bitwriter_init(&writer);
	bitwriter_ue(&writer, 7U);
	CHECK(bits_equal(&writer, "0001000"));

	/* Table 9-3: se 1 is codeNum 1, -1 is 2, 2 is 3, -2 is 4. */
	bitwriter_init(&writer);
	bitwriter_se(&writer, 1);
	CHECK(bits_equal(&writer, "010"));
	bitwriter_init(&writer);
	bitwriter_se(&writer, -1);
	CHECK(bits_equal(&writer, "011"));
	bitwriter_init(&writer);
	bitwriter_se(&writer, -2);
	CHECK(bits_equal(&writer, "00101"));

	/* The trailing bits pad to the byte boundary after the stop bit. */
	bitwriter_init(&writer);
	bitwriter_u(&writer, 5U, 3U);
	bitwriter_trailing(&writer);
	CHECK(bits_equal(&writer, "10110000"));

	/*
	 * 7.4.1: 00 00 01 becomes 00 00 03 01, and a final 00 00 00 becomes
	 * 00 00 03 00 00 03 (a three-byte escape inside, then the 0x03 after
	 * the final zero byte).
	 */
	size = bitwriter_nal(3U, 5U, rbsp, sizeof(rbsp), nal, sizeof(nal));
	CHECK(size == 10U);
	CHECK(nal[0] == 0x65U);
	CHECK(nal[1] == 0x00U && nal[2] == 0x00U && nal[3] == 0x03U && nal[4] == 0x01U);
	CHECK(nal[5] == 0x00U && nal[6] == 0x00U && nal[7] == 0x03U && nal[8] == 0x00U);
	CHECK(nal[9] == 0x03U);

	/* Reports the result. */
	status = check_finish("bitwriter");
	return status;
}
