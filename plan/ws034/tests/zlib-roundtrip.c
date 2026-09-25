/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the zlib package on the target: compresses a buffer, inflates it
 * back, and compares.  It also names the zlib the loader found, so a run
 * shows which library answered.  Prints one PASS or FAIL line.
 */

#include <stdio.h>
#include <string.h>
#include <zlib.h>

int
main(
	void)
{
	static unsigned char input[65536];
	static unsigned char packed[70000];
	static unsigned char unpacked[65536];
	uLongf packed_length = sizeof(packed);
	uLongf unpacked_length = sizeof(unpacked);
	unsigned i;
	int error;

	/* Fills the input with text that compresses, and some that does not. */
	for (i = 0; i < sizeof(input); i++)
		input[i] = (unsigned char)(i < 32768 ? "zedBSD "[i % 7] : (i * 2654435761U) >> 24);

	/* Compresses, then inflates, and compares. */
	error = compress2(packed, &packed_length, input, sizeof(input), Z_BEST_COMPRESSION);
	if (error != Z_OK) {
		printf("ZLIB FAIL compress=%d\n", error);
		return 1;
	}
	error = uncompress(unpacked, &unpacked_length, packed, packed_length);
	if (error != Z_OK || unpacked_length != sizeof(input) ||
	    memcmp(input, unpacked, sizeof(input)) != 0) {
		printf("ZLIB FAIL uncompress=%d length=%lu\n", error, (unsigned long)unpacked_length);
		return 1;
	}
	printf("ZLIB PASS version=%s in=%u packed=%lu crc32=%08lx\n", zlibVersion(),
	       (unsigned)sizeof(input), (unsigned long)packed_length,
	       crc32(0L, input, sizeof(input)));
	return 0;
}
