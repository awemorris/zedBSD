/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The routing interface: the routes of both families, and the AF_ROUTE
 * sockets' events.
 */

#ifndef KERN_UAPI_ROUTE_H
#define KERN_UAPI_ROUTE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <uapi/socket.h>
#include <uapi/netinet.h>
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

/*
 * A device was published.  Its interface index is new, so a consumer that
 * does not hold it reads every interface again.
 */
#define RTM_IFINFO_ARRIVAL	4U

#define RTM_IFINFO_F_OVERFLOW	0x00000001U

/*
 * IPv6's events (ws130), only on a socket made with protocol AF_INET6
 * (socket(AF_ROUTE, SOCK_RAW, AF_INET6)), which has the RTM_IFINFO events
 * too; a socket made with protocol 0 has RTM_IFINFO alone, as before.
 * Every record starts like struct rtm_header and is rtm_length bytes; a
 * reader skips a type it does not know.  RTM_F_OVERFLOW on a record says
 * records were lost before it.
 */
#define RTM_ROUTERADV	0x0020U
#define RTM_ADDRINFO	0x0021U
#define RTM_NEIGHBOR	0x0022U
#define RTM_F_OVERFLOW	RTM_IFINFO_F_OVERFLOW

/* What every record starts with. */
struct rtm_header {
	uint16_t rtm_version;
	uint16_t rtm_type;
	uint32_t rtm_length;
	uint64_t rtm_sequence;
};

/*
 * A Router Advertisement the kernel took (its hop limit 255, its source
 * link-local, its checksum right) on an interface where IPv6 is on: the
 * router's address and the ICMPv6 message from its type byte, which
 * follows the record (rtm_message_length bytes, at most
 * RTM_ROUTERADV_MESSAGE_MAX).  The kernel has applied the link's values
 * (hop limit, reachable time, retransmission timer, MTU); the prefixes,
 * the routes and the DNS servers are the reader's to act on.
 */
#define RTM_ROUTERADV_MESSAGE_MAX	1460U

struct rtm_routeradv {
	uint16_t rtm_version;
	uint16_t rtm_type;
	uint32_t rtm_length;
	uint64_t rtm_sequence;
	uint32_t rtm_ifindex;
	uint32_t rtm_flags;
	struct in6_addr rtm_source;
	uint32_t rtm_message_length;
	uint32_t rtm_reserved;
};

/* What happened to an IPv6 address: DAD passed, DAD failed, its preferred or its valid lifetime ran out. */
#define RTM_ADDRINFO_PREFERRED	1U
#define RTM_ADDRINFO_DUPLICATE	2U
#define RTM_ADDRINFO_DEPRECATED	3U
#define RTM_ADDRINFO_EXPIRED	4U

struct rtm_addrinfo {
	uint16_t rtm_version;
	uint16_t rtm_type;
	uint32_t rtm_length;
	uint64_t rtm_sequence;
	uint32_t rtm_ifindex;
	uint32_t rtm_flags;
	struct in6_addr rtm_address;
	uint32_t rtm_prefixlen;
	uint32_t rtm_transition;
	uint32_t rtm_addr_flags;
	uint32_t rtm_reserved;
};

/* A neighbor that stopped answering (neighbor unreachability detection): a router, for networkd to choose another. */
#define RTM_NEIGHBOR_UNREACHABLE	1U

struct rtm_neighbor {
	uint16_t rtm_version;
	uint16_t rtm_type;
	uint32_t rtm_length;
	uint64_t rtm_sequence;
	uint32_t rtm_ifindex;
	uint32_t rtm_flags;
	struct in6_addr rtm_address;
	uint32_t rtm_transition;
	uint32_t rtm_router;
};

#define SIOCADDRT	0x0000890bUL
#define SIOCDELRT	0x0000890cUL
#define SIOCGRTENTRY	0x000089f1UL

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

/*
 * An IPv6 route (ws130): the destination and its prefix length, the
 * gateway (a link-local one is the interface's), the RTF_* flags, the
 * interface, the metric (the lower is chosen between equal prefixes) and
 * the seconds left (IN6_LIFETIME_INFINITE for none; the kernel removes the
 * route when they run out).  SIOCGRTENTRY_IN6 takes rt6_index, the
 * ordinal of the route to read, and answers ENOENT past the last.
 */
struct in6_rtentry {
	struct in6_addr rt6_dst;
	struct in6_addr rt6_gateway;
	uint32_t rt6_prefixlen;
	uint32_t rt6_flags;
	uint32_t rt6_ifindex;
	uint32_t rt6_metric;
	uint32_t rt6_lifetime;
	uint32_t rt6_index;
};

#define SIOCADDRT_IN6	0x000089a4UL	/* struct in6_rtentry */
#define SIOCDELRT_IN6	0x000089a5UL	/* struct in6_rtentry (destination, prefix length, interface) */
#define SIOCGRTENTRY_IN6	0x000089a6UL	/* struct in6_rtentry */

struct rtentry {
	uint32_t rt_index;
	uint32_t rt_flags;
	uint32_t rt_ifindex;
	uint32_t rt_reserved;
	struct sockaddr rt_dst;
	struct sockaddr rt_gateway;
	struct sockaddr rt_genmask;
};

_Static_assert(sizeof(struct rtm_ifinfo) == 56U,
    "RTM_IFINFO ABI must remain fixed width");
_Static_assert(sizeof(struct rtm_header) == 16U, "the records' header is fixed");
_Static_assert(sizeof(struct rtm_routeradv) == 48U, "RTM_ROUTERADV's record is fixed before its message");
_Static_assert(sizeof(struct rtm_addrinfo) == 56U, "RTM_ADDRINFO's record is fixed");
_Static_assert(sizeof(struct rtm_neighbor) == 48U, "RTM_NEIGHBOR's record is fixed");
_Static_assert(sizeof(struct in6_rtentry) == 56U, "the IPv6 route entry is fixed");

#ifdef __cplusplus
}
#endif

#endif
