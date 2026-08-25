/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_NETCONF_H
#define ZEDBSD_NETCONF_H

#include <stddef.h>
#include <stdio.h>

#define NETCONF_PATH "/etc/net.conf"
#define NETCONF_MAX_INTERFACES 16
#define NETCONF_MAX_ADDRESSES 8
#define NETCONF_MAX_MEMBERS 16
#define NETCONF_MAX_ROUTES 16
#define NETCONF_MAX_DNS 8
#define NETCONF_NAME_MAX 15
#define NETCONF_IPV4_MAX 15

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
	char address[NETCONF_IPV4_MAX + 1];
	unsigned prefix_length;
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
};

struct netconf_route {
	char destination[NETCONF_IPV4_MAX + 4];
	char gateway[NETCONF_IPV4_MAX + 1];
};

struct netconf {
	unsigned version;
	struct netconf_interface interfaces[NETCONF_MAX_INTERFACES];
	size_t interface_count;
	struct netconf_route routes[NETCONF_MAX_ROUTES];
	size_t route_count;
	enum netconf_dns_mode dns_mode;
	char dns_servers[NETCONF_MAX_DNS][NETCONF_IPV4_MAX + 1];
	size_t dns_count;
};

int netconf_parse(FILE *, struct netconf *, char *, size_t);
int netconf_load(const char *, struct netconf *, char *, size_t);
int netconf_validate(const struct netconf *, char *, size_t);
int netconf_write(FILE *, const struct netconf *);

#endif
