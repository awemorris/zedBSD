/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of the unit-test tools themselves: wire.h's encoders give the
 * bytes RFC 4251 section 5 gives (its uint32, string and name-list
 * examples, written by hand here), wire_hex reads hexadecimal text,
 * and every module links (the skeletons included).
 */

#include <stdint.h>
#include <string.h>

#include "check.h"
#include "wire.h"

/* A buffer for one check at a time. */
static struct wire buffer;

/*
 * Runs the checks of the tools.
 */
int
main(void)
{
	uint8_t bytes[8];

	/* uint32 0x29b7f4aa is 29 b7 f4 aa (RFC 4251 5, uint32). */
	wire_reset(&buffer);
	wire_u32(&buffer, 0x29b7f4aaU);
	CHECK_EQ(buffer.size, 4);
	CHECK_EQ(wire_hex("29b7f4aa", bytes, sizeof(bytes)), 4);
	CHECK_BYTES(buffer.data, bytes, 4);

	/* The string "testing" is 00 00 00 07 t e s t i n g (RFC 4251 5, string). */
	wire_reset(&buffer);
	wire_text(&buffer, "testing");
	CHECK_EQ(buffer.size, 11);
	CHECK_BYTES(buffer.data, "\0\0\0\7testing", 11);

	/* The name-list ("zlib", "none") is 00 00 00 09 7a 6c 69 62 2c 6e 6f 6e 65 (RFC 4251 5, name-list). */
	wire_reset(&buffer);
	wire_text(&buffer, "zlib,none");
	CHECK_BYTES(buffer.data, "\0\0\0\x09zlib,none", 13);

	/* uint64 and boolean. */
	wire_reset(&buffer);
	wire_u64(&buffer, ((uint64_t)0x01020304U << 32) | 0x05060708U);
	wire_bool(&buffer, 1);
	CHECK_EQ(buffer.size, 9);
	CHECK_BYTES(buffer.data, "\1\2\3\4\5\6\7\x08\1", 9);

	/* An odd number of digits and a bad digit are refused; colons and spaces are skipped. */
	CHECK_EQ(wire_hex("abc", bytes, sizeof(bytes)), 0);
	CHECK_EQ(wire_hex("zz", bytes, sizeof(bytes)), 0);
	CHECK_EQ(wire_hex("01:02 03", bytes, sizeof(bytes)), 3);

	/* A write past the end sets overflow and writes nothing. */
	wire_reset(&buffer);
	buffer.size = WIRE_MAX - 2;
	wire_u32(&buffer, 1);
	CHECK(buffer.overflow);
	CHECK_EQ(buffer.size, WIRE_MAX - 2);

	/* Reports the tools' test. */
	return check_finish("tools");
}
