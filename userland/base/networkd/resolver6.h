/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The RDNSS servers in resolv.conf (ws177-p045): dhcpc (DHCPv4 and
 * DHCPv6) and networkd's DNS commands write the file as they always did,
 * and networkd puts the servers the Router Advertisements gave back into
 * whatever is there, keeping its first comment line, its search list, its
 * other lines and its other servers.  Pure: the host tests build this file
 * alone.
 */

#ifndef NETWORKD_RESOLVER6_H
#define NETWORKD_RESOLVER6_H

#include <stddef.h>

/* The most servers resolv.conf lists, the most lines kept, the longest line, and the longest server's text (with a zone). */
#define RESOLVER6_SERVERS	3U
#define RESOLVER6_LINES		32U
#define RESOLVER6_LINE		256U
#define RESOLVER6_SERVER_TEXT	64U

/*
 * The servers networkd puts in the file (wanted) and those it put in
 * before (owned), each a server's text such as "2001:db8::53" or
 * "fe80::1%em0".
 */
struct resolver6_servers {
	char text[RESOLVER6_SERVERS][RESOLVER6_SERVER_TEXT];
	unsigned count;
};

int resolver6_merge(const char *current, size_t length, const struct resolver6_servers *owned,
    const struct resolver6_servers *wanted, const char *search, char *output, size_t capacity,
    struct resolver6_servers *now_owned);

#endif
