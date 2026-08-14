/*
 * IPv4 routing interface
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_UAPI_ROUTE_H
#define ZEDBSD_UAPI_ROUTE_H

#include <zedbsd/socket.h>
#include <stdint.h>

#define RTF_UP        0x0001U
#define RTF_GATEWAY   0x0002U
#define RTF_HOST      0x0004U
#define RTF_STATIC    0x0800U
#define RTF_DYNAMIC   0x1000U
#define RTF_CONNECTED 0x2000U

struct rtentry {
	uint32_t rt_index;
	uint32_t rt_flags;
	uint32_t rt_ifindex;
	uint32_t rt_reserved;
	struct sockaddr rt_dst;
	struct sockaddr rt_gateway;
	struct sockaddr rt_genmask;
};

#define SIOCADDRT    0x0000890bUL
#define SIOCDELRT    0x0000890cUL
#define SIOCGRTENTRY 0x000089f1UL

#endif
