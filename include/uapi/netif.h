/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * netif
 */

#ifndef KERN_UAPI_NETIF_H
#define KERN_UAPI_NETIF_H

#include <uapi/socket.h>
#include <uapi/netinet.h>
#include <stdint.h>

#define IFNAMSIZ 16

#define IFF_UP 0x0001U
#define IFF_BROADCAST 0x0002U
#define IFF_LOOPBACK 0x0008U
#define IFF_RUNNING 0x0040U
#define IFF_MULTICAST 0x1000U

struct if_data {
	uint32_t ifi_mtu;
	uint32_t ifi_link_mbps;	/* the link's speed in Mb/s, 0 while not known */
	uint64_t ifi_ipackets;
	uint64_t ifi_ibytes;
	uint64_t ifi_ierrors;
	uint64_t ifi_iqdrops;
	uint64_t ifi_opackets;
	uint64_t ifi_obytes;
	uint64_t ifi_oerrors;
	uint64_t ifi_oqdrops;
};

struct ifreq {
	char ifr_name[IFNAMSIZ];
	union {
		struct sockaddr address;
		int flags;
		int ifindex;
		uint8_t hardware_address[8];
		int mtu;
		struct if_data data;
	} ifr_ifru;
};

#define ifr_addr ifr_ifru.address
#define ifr_flags ifr_ifru.flags
#define ifr_ifindex ifr_ifru.ifindex
#define ifr_hwaddr ifr_ifru.hardware_address
#define ifr_mtu ifr_ifru.mtu
#define ifr_data ifr_ifru.data

struct ifconf {
	uint32_t ifc_len;
	uint32_t ifc_reserved;
	uint64_t ifc_buf;
};

#define SIOCGIFINDEX 0x00008933UL
#define SIOCGIFNAME 0x00008910UL
#define SIOCGIFCONF 0x00008912UL
#define SIOCGIFFLAGS 0x00008913UL
#define SIOCSIFFLAGS 0x00008914UL
#define SIOCGIFHWADDR 0x00008927UL
#define SIOCGIFADDR 0x00008915UL
#define SIOCSIFADDR 0x00008916UL
#define SIOCGIFNETMASK 0x0000891bUL
#define SIOCSIFNETMASK 0x0000891cUL
#define SIOCGIFBRDADDR 0x00008917UL
#define SIOCSIFBRDADDR 0x00008919UL
#define SIOCGIFMTU 0x00008921UL
/*
 * Sets an interface's MTU from ifr_mtu, the superuser only: between 1280
 * and what the interface takes (EINVAL outside), with the interface up or
 * down; 0 sets it back to the interface's own.  EBUSY while it opens or
 * closes, or when its driver needs it down first.
 */
#define SIOCSIFMTU 0x00008922UL
#define SIOCGIFSTATS 0x000089f0UL

/*
 * The IPv6 addresses of an interface (ws130).  networkd and net add and
 * remove them; the kernel runs duplicate address detection on a new one
 * (unless IN6_IFF_NODAD), counts its lifetimes down, makes it deprecated
 * when the preferred lifetime runs out and removes it when the valid one
 * does, and says so on AF_ROUTE sockets (route.h, RTM_ADDRINFO).  The
 * ioctls are taken on AF_INET and AF_INET6 sockets; the changes need the
 * superuser.
 */
#define IN6_IFF_TENTATIVE	0x0001U	/* duplicate address detection under way (kernel) */
#define IN6_IFF_DUPLICATED	0x0002U	/* another node has it: not used (kernel) */
#define IN6_IFF_DEPRECATED	0x0004U	/* preferred lifetime over: not chosen as a source (kernel) */
#define IN6_IFF_TEMPORARY	0x0008U	/* RFC 8981 */
#define IN6_IFF_AUTOCONF	0x0010U	/* made from a Router Advertisement's prefix */
#define IN6_IFF_DHCP		0x0020U	/* leased by DHCPv6 */
#define IN6_IFF_NODAD		0x0040U	/* no duplicate address detection */

/* The most IPv6 addresses an interface has. */
#define IN6_IFADDRS_MAX		16U

/*
 * An address to add (or whose lifetimes and flags to renew, when the
 * interface has it already) or to remove: the interface, the address, its
 * prefix length, the caller's IN6_IFF_TEMPORARY, _AUTOCONF, _DHCP and
 * _NODAD, and the lifetimes in seconds (IN6_LIFETIME_INFINITE for none).
 */
struct in6_aliasreq {
	char ifra_name[IFNAMSIZ];
	struct sockaddr_in6 ifra_addr;
	uint32_t ifra_prefixlen;
	uint32_t ifra_flags;
	uint32_t ifra_valid;
	uint32_t ifra_preferred;
};

/* One address of an interface as SIOCGIFADDRS_IN6 lists it: the seconds left of each lifetime. */
struct in6_ifaddr_entry {
	struct in6_addr ife_addr;
	uint32_t ife_prefixlen;
	uint32_t ife_flags;
	uint32_t ife_valid;
	uint32_t ife_preferred;
};

/* An interface's IPv6 addresses (ifa_name in, the rest out), and whether IPv6 is on there. */
struct in6_ifaddrs {
	char ifa_name[IFNAMSIZ];
	uint32_t ifa_count;
	uint32_t ifa_enabled;
	struct in6_ifaddr_entry ifa_list[IN6_IFADDRS_MAX];
};

#define SIOCAIFADDR_IN6 0x000089a0UL	/* struct in6_aliasreq */
#define SIOCDIFADDR_IN6 0x000089a1UL	/* struct in6_aliasreq (name, address) */
#define SIOCGIFADDRS_IN6 0x000089a2UL	/* struct in6_ifaddrs */
#define SIOCSIFINET6 0x000089a3UL	/* struct ifreq: ifr_flags 1 turns IPv6 on, 0 off (its addresses go) */

#endif
