/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * netif
 */

#ifndef ZEDBSD_UAPI_NETIF_H
#define ZEDBSD_UAPI_NETIF_H

#include <zedbsd/socket.h>
#include <stdint.h>

#define IFNAMSIZ	16

#define IFF_UP	0x0001U
#define IFF_BROADCAST	0x0002U
#define IFF_RUNNING	0x0040U
#define IFF_MULTICAST	0x1000U

struct if_data {
	uint32_t ifi_mtu;
	uint32_t ifi_reserved;
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

#define ifr_addr	ifr_ifru.address
#define ifr_flags	ifr_ifru.flags
#define ifr_ifindex	ifr_ifru.ifindex
#define ifr_hwaddr	ifr_ifru.hardware_address
#define ifr_mtu	ifr_ifru.mtu
#define ifr_data	ifr_ifru.data

struct ifconf {
	uint32_t ifc_len;
	uint32_t ifc_reserved;
	uint64_t ifc_buf;
};

#define SIOCGIFINDEX	0x00008933UL
#define SIOCGIFNAME	0x00008910UL
#define SIOCGIFCONF	0x00008912UL
#define SIOCGIFFLAGS	0x00008913UL
#define SIOCSIFFLAGS	0x00008914UL
#define SIOCGIFHWADDR	0x00008927UL
#define SIOCGIFADDR	0x00008915UL
#define SIOCSIFADDR	0x00008916UL
#define SIOCGIFNETMASK	0x0000891bUL
#define SIOCSIFNETMASK	0x0000891cUL
#define SIOCGIFBRDADDR	0x00008917UL
#define SIOCSIFBRDADDR	0x00008919UL
#define SIOCGIFMTU	0x00008921UL
#define SIOCGIFSTATS	0x000089f0UL

#endif
