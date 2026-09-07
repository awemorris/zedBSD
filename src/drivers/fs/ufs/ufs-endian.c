/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "ufs-endian.h"

uint16_t ufs_get16(const void *buffer, size_t offset, int swapped)
{
	const uint8_t *p = (const uint8_t *)buffer + offset;
	return swapped ? ((uint16_t)p[0] << 8) | p[1] :
	    (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
uint32_t ufs_get32(const void *buffer, size_t offset, int swapped)
{
	const uint8_t *p = (const uint8_t *)buffer + offset;
	return swapped ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | p[3] : (uint32_t)p[0] |
	    ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	    ((uint32_t)p[3] << 24);
}
uint64_t ufs_get64(const void *buffer, size_t offset, int swapped)
{
	uint64_t low, high;
	if (swapped) {
		high = ufs_get32(buffer, offset, 1);
		low = ufs_get32(buffer, offset + 4U, 1);
	} else {
		low = ufs_get32(buffer, offset, 0);
		high = ufs_get32(buffer, offset + 4U, 0);
	}
	return low | (high << 32);
}
void ufs_put16(void *buffer, size_t offset, uint16_t value, int swapped)
{
	uint8_t *p = (uint8_t *)buffer + offset;
	if (swapped) { p[0] = value >> 8; p[1] = value; }
	else { p[0] = value; p[1] = value >> 8; }
}
void ufs_put32(void *buffer, size_t offset, uint32_t value, int swapped)
{
	uint8_t *p = (uint8_t *)buffer + offset;
	if (swapped) {
		p[0] = value >> 24; p[1] = value >> 16;
		p[2] = value >> 8; p[3] = value;
	} else {
		p[0] = value; p[1] = value >> 8;
		p[2] = value >> 16; p[3] = value >> 24;
	}
}
void ufs_put64(void *buffer, size_t offset, uint64_t value, int swapped)
{
	if (swapped) {
		ufs_put32(buffer, offset, (uint32_t)(value >> 32), 1);
		ufs_put32(buffer, offset + 4U, (uint32_t)value, 1);
	} else {
		ufs_put32(buffer, offset, (uint32_t)value, 0);
		ufs_put32(buffer, offset + 4U, (uint32_t)(value >> 32), 0);
	}
}
