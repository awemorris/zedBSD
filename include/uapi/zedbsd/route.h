/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * IPv4 routing interface
 */

#ifndef ZEDBSD_UAPI_ROUTE_H
#define ZEDBSD_UAPI_ROUTE_H

#include <zedbsd/socket.h>
#include <stdint.h>

#define RTF_UP	0x0001U
#define RTF_GATEWAY	0x0002U
#define RTF_HOST	0x0004U
#define RTF_STATIC	0x0800U
#define RTF_DYNAMIC	0x1000U
#define RTF_CONNECTED	0x2000U

/*
 * Read-only interface event records returned by PF_ROUTE sockets.  This is a
 * deliberately small, fixed-width ABI.  Consumers must reject an unknown
 * version or length and resnapshot all interfaces after RTM_IFINFO_F_OVERFLOW.
 */
#define RTM_VERSION	1U
#define RTM_IFINFO	0x000eU

#define RTM_IFINFO_CARRIER_UP	1U
#define RTM_IFINFO_CARRIER_DOWN	2U
#define RTM_IFINFO_REMOVAL	3U

#define RTM_IFINFO_F_OVERFLOW	0x00000001U

struct rtm_ifinfo {
	uint16_t rtm_version;
	uint16_t rtm_type;
	uint32_t rtm_length;
	uint64_t rtm_sequence;
	uint64_t rtm_device_generation;
	uint32_t rtm_ifindex;
	uint32_t rtm_if_flags;
	uint32_t rtm_transition;
	uint32_t rtm_flags;
	uint64_t rtm_reserved[2];
};

_Static_assert(sizeof(struct rtm_ifinfo) == 56U,
    "RTM_IFINFO ABI must remain fixed width");

struct rtentry {
	uint32_t rt_index;
	uint32_t rt_flags;
	uint32_t rt_ifindex;
	uint32_t rt_reserved;
	struct sockaddr rt_dst;
	struct sockaddr rt_gateway;
	struct sockaddr rt_genmask;
};

#define SIOCADDRT	0x0000890bUL
#define SIOCDELRT	0x0000890cUL
#define SIOCGRTENTRY	0x000089f1UL

#endif
