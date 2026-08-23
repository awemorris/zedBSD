/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Network byteorder operations
 */

#ifndef ZEDBSD_KERN_NET_BYTEORDER_H
#define ZEDBSD_KERN_NET_BYTEORDER_H

#include <stdint.h>

static inline uint16_t
net_bswap16(
	uint16_t value)
{
	return (uint16_t)((value << 8) | (value >> 8));
}

static inline uint32_t
net_bswap32(
	uint32_t value)
{
	return ((value & 0x000000ffU) << 24) | ((value & 0x0000ff00U) << 8) |
	       ((value & 0x00ff0000U) >> 8) | ((value & 0xff000000U) >> 24);
}

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
# define net_htons(v)	((uint16_t)(v))
# define net_ntohs(v)	((uint16_t)(v))
# define net_htonl(v)	((uint32_t)(v))
# define net_ntohl(v)	((uint32_t)(v))
#else
# define net_htons(v)	net_bswap16((uint16_t)(v))
# define net_ntohs(v)	net_bswap16((uint16_t)(v))
# define net_htonl(v)	net_bswap32((uint32_t)(v))
# define net_ntohl(v)	net_bswap32((uint32_t)(v))
#endif

#endif
