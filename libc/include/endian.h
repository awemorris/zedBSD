/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_ENDIAN_H
#define ZEDBSD_ENDIAN_H

#include <stdint.h>

#define LITTLE_ENDIAN	1234
#define BIG_ENDIAN	4321
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define BYTE_ORDER	LITTLE_ENDIAN
#else
#define BYTE_ORDER	BIG_ENDIAN
#endif

uint16_t htobe16(uint16_t value);
uint32_t htobe32(uint32_t value);
uint64_t htobe64(uint64_t value);
uint16_t htole16(uint16_t value);
uint32_t htole32(uint32_t value);
uint64_t htole64(uint64_t value);
uint16_t be16toh(uint16_t value);
uint32_t be32toh(uint32_t value);
uint64_t be64toh(uint64_t value);
uint16_t le16toh(uint16_t value);
uint32_t le32toh(uint32_t value);
uint64_t le64toh(uint64_t value);

#endif
