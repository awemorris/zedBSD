/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "internal.h"

#include <stdint.h>

static uint32_t
checksum_add(uint32_t sum, const uint8_t *data, size_t length)
{
	while (length >= 2U) {
		sum += (uint16_t)((uint16_t)data[0] << 8) | data[1];
		data += 2;
		length -= 2;
	}
	if (length != 0)
		sum += (uint16_t)data[0] << 8;
	return sum;
}

static uint16_t checksum_finish(uint32_t sum)
{
	while ((sum >> 16) != 0)
		sum = (sum & 0xffffU) + (sum >> 16);
	return (uint16_t)~sum;
}

uint16_t net_checksum(const void *data, size_t length)
{
	return checksum_finish(checksum_add(0, data, length));
}

uint16_t
net_checksum_pseudo(uint32_t source, uint32_t destination, uint8_t protocol,
		    const void *data, size_t length)
{
	uint32_t sum = 0;
	uint8_t pseudo[12];

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
	sum = checksum_add(sum, pseudo, sizeof(pseudo));
	sum = checksum_add(sum, data, length);
	return checksum_finish(sum);
}
