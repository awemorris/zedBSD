/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_CATALOG_FORMAT_H
#define ZEDBSD_CATALOG_FORMAT_H

#include <stdint.h>

#define ZEDBSD_CATALOG_MAGIC "ZMCAT01\0"
#define ZEDBSD_CATALOG_MAGIC_SIZE 8U
#define ZEDBSD_CATALOG_VERSION 1U
#define ZEDBSD_CATALOG_HEADER_SIZE 28U
#define ZEDBSD_CATALOG_ENTRY_SIZE 16U

static inline uint32_t
zedbsd_catalog_get32(const unsigned char *bytes)
{
	return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
	       (uint32_t)bytes[2] << 8 | (uint32_t)bytes[3];
}

static inline void
zedbsd_catalog_put32(unsigned char *bytes, uint32_t value)
{
	bytes[0] = (unsigned char)(value >> 24);
	bytes[1] = (unsigned char)(value >> 16);
	bytes[2] = (unsigned char)(value >> 8);
	bytes[3] = (unsigned char)value;
}

#endif
