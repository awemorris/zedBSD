/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One wired interface's configuration asked of networkd (ws089-p022,
 * NETWORKD_OP_LAN_CONFIGURE): the checks of its fields and the edit of
 * net.conf it makes.  Nothing here opens a file, a socket or a device, so
 * the host tests run it alone.
 */

#ifndef KERN_NETWORKD_LAN_CONFIGURE_H
#define KERN_NETWORKD_LAN_CONFIGURE_H

#include "userland/base/net/netconf.h"

#include <stddef.h>

/* The most DNS servers one configuration names. */
#define NETWORKD_LAN_CONFIGURE_DNS_MAX	2U

/*
 * The configuration asked: the interface, a static address and netmask
 * (both empty: DHCP) with its router (may be empty), and the DNS servers
 * (none: the servers DHCP gives).  The texts are the request's, ended.
 */
struct networkd_lan_configure {
	const char *interface;
	const char *address;
	const char *netmask;
	const char *gateway;
	const char *dns[NETWORKD_LAN_CONFIGURE_DNS_MAX];
	size_t dns_count;
};

int networkd_lan_configure_check(const struct networkd_lan_configure *request, char *diagnostic, size_t capacity);
int networkd_lan_configure_edit(struct netconf *configuration, const struct networkd_lan_configure *request, char *diagnostic, size_t capacity);
int networkd_lan_configure_static(const struct networkd_lan_configure *request);

#endif
