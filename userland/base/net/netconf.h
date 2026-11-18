/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the zedBSD userland netconf interface.
 */

#ifndef KERN_NETCONF_H
#define KERN_NETCONF_H

#include <stddef.h>
#include <stdio.h>

#ifndef NETCONF_PATH
#define NETCONF_PATH "/etc/net.conf"
#endif
#ifndef NETCONF_LOCK_PATH
#define NETCONF_LOCK_PATH "/run/net.conf.lock"
#endif
#define NETCONF_MAX_INTERFACES 16
#define NETCONF_MAX_ADDRESSES 8
#define NETCONF_MAX_MEMBERS 16
#define NETCONF_MAX_ROUTES 16
#define NETCONF_MAX_DNS 8
#define NETCONF_NAME_MAX 15
#define NETCONF_IPV4_MAX 15
/* The longest text of an address either family (an IPv6 one, ws130-p005). */
#define NETCONF_ADDRESS_MAX 45
/* The bounds of an interface's "mtu" (the device's own limit applies when it is set). */
#define NETCONF_MTU_MIN 1280U
#define NETCONF_MTU_MAX 65535U

enum netconf_interface_type {
	NETCONF_INTERFACE_UNSET,
	NETCONF_INTERFACE_LOOPBACK,
	NETCONF_INTERFACE_ETHERNET,
	NETCONF_INTERFACE_VLAN,
	NETCONF_INTERFACE_BRIDGE
};

enum netconf_dns_mode {
	NETCONF_DNS_UNSET,
	NETCONF_DNS_DHCP,
	NETCONF_DNS_STATIC,
	NETCONF_DNS_MERGE
};

struct netconf_address {
	char address[NETCONF_ADDRESS_MAX + 1];
	unsigned prefix_length;
};

/* How an interface asks DHCPv6 (ws130-p005): by the Router Advertisement's flags, for options only, for addresses, never. */
enum netconf_ipv6_dhcp {
	NETCONF_IPV6_DHCP_AUTO,
	NETCONF_IPV6_DHCP_STATELESS,
	NETCONF_IPV6_DHCP_STATEFUL,
	NETCONF_IPV6_DHCP_OFF
};

/*
 * An interface's IPv6 (ws130-p005, the "ipv6:" section of net.conf): each
 * key with whether the file named it (a key it does not name is its
 * default: IPv6, SLAAC, a stable address and a temporary one on, DHCPv6 by
 * the Router Advertisement), and its static addresses.
 */
struct netconf_ipv6 {
	int enabled;
	int enabled_set;
	int autoconf;
	int autoconf_set;
	enum netconf_ipv6_dhcp dhcp;
	int dhcp_set;
	int stable_address;
	int stable_address_set;
	int temporary;
	int temporary_set;
	struct netconf_address addresses[NETCONF_MAX_ADDRESSES];
	size_t address_count;
};

struct netconf_interface {
	char name[NETCONF_NAME_MAX + 1];
	enum netconf_interface_type type;
	int enabled;
	int enabled_set;
	int dhcp;
	int dhcp_set;
	unsigned dhcp_timeout;
	int dhcp_timeout_set;
	struct netconf_address addresses[NETCONF_MAX_ADDRESSES];
	size_t address_count;
	char parent[NETCONF_NAME_MAX + 1];
	unsigned vlan_id;
	int vlan_id_set;
	char members[NETCONF_MAX_MEMBERS][NETCONF_NAME_MAX + 1];
	size_t member_count;
	struct netconf_ipv6 ipv6;
	unsigned mtu;
	int mtu_set;
};

/* A route: its destination ("default" or a prefix of either family), its gateway, and the interface of a link-local IPv6 gateway (ws130-p005). */
struct netconf_route {
	char destination[NETCONF_ADDRESS_MAX + 5];
	char gateway[NETCONF_ADDRESS_MAX + 1];
	char interface[NETCONF_NAME_MAX + 1];
};

struct netconf {
	unsigned version;
	struct netconf_interface interfaces[NETCONF_MAX_INTERFACES];
	size_t interface_count;
	struct netconf_route routes[NETCONF_MAX_ROUTES];
	size_t route_count;
	enum netconf_dns_mode dns_mode;
	char dns_servers[NETCONF_MAX_DNS][NETCONF_ADDRESS_MAX + 1];
	size_t dns_count;
};

int netconf_parse(FILE *, struct netconf *, char *, size_t);
int netconf_load(const char *, struct netconf *, char *, size_t);
int netconf_validate(const struct netconf *, char *, size_t);
int netconf_write(FILE *, const struct netconf *);
int netconf_writer_lock(char *, size_t);
int netconf_writer_unlock(int);
int netconf_save_atomic(const char *, const struct netconf *, char *, size_t);
int netconf_save_atomic_locked(const char *, const struct netconf *, char *,
	size_t);

/* An interface's IPv6 settings with their defaults (ws130-p005). */
int netconf_ipv6_enabled(const struct netconf_interface *);
int netconf_ipv6_autoconf(const struct netconf_interface *);
int netconf_ipv6_stable_address(const struct netconf_interface *);
int netconf_ipv6_temporary(const struct netconf_interface *);
enum netconf_ipv6_dhcp netconf_ipv6_dhcp(const struct netconf_interface *);

#endif
