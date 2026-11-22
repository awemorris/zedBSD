/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The addresses of the network interfaces, as getifaddrs() lists them.
 *
 * getifaddrs() is not in POSIX; this is the interface the BSDs and Linux
 * share (FreeBSD's getifaddrs(3)).  The list has, for each interface in
 * the order the system lists them, an entry for its link, then one for its
 * IPv4 address if it has one, then one for each IPv6 address:
 *
 * - the link's entry has an AF_PACKET address, a struct sockaddr_l2 (the
 *   form of Linux's struct sockaddr_ll) with the interface's index and, for
 *   an Ethernet interface, its hardware address, and ifa_data points to the
 *   interface's struct if_data (<net/if.h>), as on the BSDs;
 * - an IPv4 entry has the address, its netmask and, on an interface with
 *   IFF_BROADCAST, its broadcast address (ifa_broadaddr) as struct
 *   sockaddr_in;
 * - an IPv6 entry has the address, the netmask of its prefix, and for a
 *   link-local address the interface's index as its scope, as struct
 *   sockaddr_in6.
 *
 * ifa_flags are the interface's IFF_ flags.  The list is freed, all of it,
 * by freeifaddrs() on its first entry.
 */

#ifndef LIBC_IFADDRS_H
#define LIBC_IFADDRS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <uapi/socket.h>

/*
 * One address of one interface in the list getifaddrs() gives.
 *
 * ifa_dstaddr is the address at the other end of a point-to-point link,
 * which no zedBSD interface is, so the member holds the broadcast address
 * (ifa_broadaddr names it so) or is null.
 */
struct ifaddrs {
	struct ifaddrs *ifa_next;
	char *ifa_name;
	unsigned int ifa_flags;
	struct sockaddr *ifa_addr;
	struct sockaddr *ifa_netmask;
	struct sockaddr *ifa_dstaddr;
	void *ifa_data;
};

#define ifa_broadaddr	ifa_dstaddr

int getifaddrs(struct ifaddrs **list);
void freeifaddrs(struct ifaddrs *list);

#ifdef __cplusplus
}
#endif

#endif
