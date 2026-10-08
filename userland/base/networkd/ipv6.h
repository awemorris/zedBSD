/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * networkd's IPv6 (ws130-p006): each wired interface's IPv6 from net.conf
 * (on or off, its static addresses) and its link-local address (RFC 7217)
 * when it comes up, and the Router Advertisements the kernel publishes on
 * an AF_INET6 route socket: the SLAAC addresses (stable and temporary), the
 * default route, and the DNS servers (RDNSS, after the IPv4 ones in
 * resolv.conf, H5), and DHCPv6 (ws130-p007) as the advertisements' M and
 * O flags and net.conf ask: `dhcpc -6` run then, and again at T1 or the
 * information refresh time.  ws177-p045: duplicates made again, temporary
 * addresses made anew before they stop being preferred, the preferred
 * interface's router alone as the default route, the RDNSS servers put
 * back into resolv.conf after another writer, and dhcpc run as a child.
 */

#ifndef NETWORKD_IPV6_H
#define NETWORKD_IPV6_H

int networkd_ipv6_open(void);
int networkd_ipv6_events(int descriptor);
void networkd_ipv6_start(void);
int networkd_ipv6_link_local(const char *name);
int networkd_ipv6_poll_timeout(void);
void networkd_ipv6_run_due(void);

/* From main.c: an interface's rank for the default route and the DNS servers (the lower is preferred: wired before Wi-Fi). */
unsigned networkd_interface_rank(const char *name);

#endif
