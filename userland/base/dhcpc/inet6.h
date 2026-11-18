/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * `dhcpc -6` (ws130-p007, plan/ws130/phase001/phase.md section 5): DHCPv6
 * on one interface, once.  Without -i, an address (IA_NA) by Solicit,
 * Advertise, Request and Reply, or by Renew and Reply when the interface
 * has a lease from before; with -i, only the DNS servers and search list
 * by Information-Request and Reply.  It records when to run again
 * (/var/db/dhcpc/IF.dhcp6: T1, or the information refresh time), which
 * networkd reads.  ws177-p046: past T2 a Rebind to any server; with -r
 * the lease given back (Release); with -D its address declined (Decline,
 * after duplicate address detection failed) and another one asked for.
 */

#ifndef DHCPC_INET6_H
#define DHCPC_INET6_H

/* What a run does: take or renew a lease, only the information, give the lease back, or decline its address. */
#define DHCPC_INET6_LEASE		0U
#define DHCPC_INET6_INFORMATION		1U
#define DHCPC_INET6_RELEASE		2U
#define DHCPC_INET6_DECLINE		3U

int dhcpc_inet6(const char *interface, unsigned mode, int resolver, unsigned timeout_seconds, int verbose);

#endif
