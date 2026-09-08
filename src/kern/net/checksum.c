/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Internet checksum.
 */

#include "internal.h"

#include <stdint.h>

static uint32_t checksum_add(uint32_t sum, const uint8_t *data, size_t length);
static uint16_t checksum_finish(uint32_t sum);

/*
 * Computes the Internet checksum of one buffer.
 */
uint16_t
net_checksum(
	const void *data,
	size_t length)
{
	uint32_t sum;
	uint16_t checksum;

	/* Sums the buffer and folds the carries. */
	sum = checksum_add(0, data, length);
	checksum = checksum_finish(sum);

	/* Reports the checksum. */
	return checksum;
}

/*
 * Computes the Internet checksum of a transport segment with its IPv4
 * pseudo-header.
 */
uint16_t
net_checksum_pseudo(
	uint32_t source,
	uint32_t destination,
	uint8_t protocol,
	const void *data,
	size_t length)
{
	uint8_t pseudo[12];
	uint32_t sum;
	uint16_t checksum;

	/* Builds the pseudo-header in network byte order. */
	pseudo[0] = (uint8_t)(source >> 24);
	pseudo[1] = (uint8_t)(source >> 16);
	pseudo[2] = (uint8_t)(source >> 8);
	pseudo[3] = (uint8_t)source;
	pseudo[4] = (uint8_t)(destination >> 24);
	pseudo[5] = (uint8_t)(destination >> 16);
	pseudo[6] = (uint8_t)(destination >> 8);
	pseudo[7] = (uint8_t)destination;
	pseudo[8] = 0;
	pseudo[9] = protocol;
	pseudo[10] = (uint8_t)(length >> 8);
	pseudo[11] = (uint8_t)length;

	/* Sums the pseudo-header and the segment, then folds the carries. */
	sum = 0;
	sum = checksum_add(sum, pseudo, sizeof(pseudo));
	sum = checksum_add(sum, data, length);
	checksum = checksum_finish(sum);

	/* Reports the checksum. */
	return checksum;
}

/* Adds the big-endian 16-bit words of a buffer to a running sum. */
static uint32_t
checksum_add(
	uint32_t sum,
	const uint8_t *data,
	size_t length)
{
	/* Adds every complete word. */
	while (length >= 2U) {
		sum += (uint16_t)((uint16_t)data[0] << 8) | data[1];
		data += 2;
		length -= 2;
	}

	/* Adds a trailing odd byte as the high half of a word. */
	if (length != 0)
		sum += (uint16_t)data[0] << 8;

	/* Reports the running sum. */
	return sum;
}

/* Folds the carries of a running sum and complements it. */
static uint16_t
checksum_finish(
	uint32_t sum)
{
	/* Folds the carry bits back into the low word until none remain. */
	while ((sum >> 16) != 0)
		sum = (sum & 0xffffU) + (sum >> 16);

	/* Reports the one's complement. */
	return (uint16_t)~sum;
}
