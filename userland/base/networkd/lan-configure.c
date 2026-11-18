/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One wired interface's configuration asked of networkd (ws089-p022).
 *
 * A member of the network group may ask it as well as root, so every field
 * is checked strictly before anything is written: the interface's name
 * (one to fifteen lower-case letters and digits, not the loopback nor a
 * radio), each address dotted IPv4 of four numbers 0 to 255 without
 * leading zeros, the netmask contiguous from /1 to /30, the address not
 * the subnet's own nor its broadcast, the router another address of the
 * same subnet, the DNS servers addresses a machine may have; and the mode
 * whole: a static address has its netmask, DHCP carries neither nor a
 * router.  The daemon still checks that the interface exists and is wired.
 *
 * The edit touches only what the configuration names: the interface's
 * entry (added when net.conf did not name it), the default route (set by a
 * static address with a router, removed by one without, and by DHCP when
 * no other interface keeps a static address), and the name servers
 * (static when named, else DHCP's).
 */

#include "userland/base/networkd/lan-configure.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The route net.conf names the default by. */
#define LAN_DEFAULT_ROUTE	"default"

static int lan_parse(const char *text, uint32_t *address);
static int lan_prefix(uint32_t mask);
static int lan_unicast(uint32_t address);
static int lan_name_valid(const char *name);
static int lan_fail(char *diagnostic, size_t capacity, const char *reason);
static void lan_copy(char *destination, size_t size, const char *source);
static void lan_subnet(const struct netconf_interface *item, uint32_t *network, uint32_t *mask);
static void lan_drop_default(struct netconf *configuration, int always, const uint32_t *networks, const uint32_t *masks, size_t count);

/*
 * Checks a configuration's fields.  Returns 0, or -1 with errno EINVAL
 * and the reason in diagnostic.
 */
int
networkd_lan_configure_check(
	const struct networkd_lan_configure *request,
	char *diagnostic,
	size_t capacity)
{
	uint32_t address;
	uint32_t mask;
	uint32_t gateway;
	uint32_t server;
	size_t index;
	int prefix;
	int usable;
	int valid;
	int error;

	/* Nothing read yet. */
	address = 0U;
	mask = 0U;

	/* Every text there. */
	if (request == NULL || request->interface == NULL || request->address == NULL ||
	    request->netmask == NULL || request->gateway == NULL ||
	    request->dns_count > NETWORKD_LAN_CONFIGURE_DNS_MAX)
		return lan_fail(diagnostic, capacity, "malformed configuration");
	for (index = 0; index < request->dns_count; index++) {
		if (request->dns[index] == NULL)
			return lan_fail(diagnostic, capacity, "malformed configuration");
	}

	/* The interface's name. */
	valid = lan_name_valid(request->interface);
	if (!valid)
		return lan_fail(diagnostic, capacity, "not a wired interface name");

	/* DHCP: no netmask nor router without an address. */
	if (request->address[0] == '\0') {
		if (request->netmask[0] != '\0' || request->gateway[0] != '\0')
			return lan_fail(diagnostic, capacity, "a netmask or router without an address");
	} else {
		/* A static address: one a machine may have. */
		error = lan_parse(request->address, &address);
		usable = lan_unicast(address);
		if (error != 0 || !usable)
			return lan_fail(diagnostic, capacity, "invalid address");

		/* Its netmask, contiguous from /1 to /30. */
		error = lan_parse(request->netmask, &mask);
		prefix = lan_prefix(mask);
		if (error != 0 || prefix < 1 || prefix > 30)
			return lan_fail(diagnostic, capacity, "invalid netmask");

		/* Not the subnet's own address nor its broadcast. */
		if ((address & ~mask) == 0U || (address & ~mask) == ~mask)
			return lan_fail(diagnostic, capacity, "the subnet's own or broadcast address");
	}

	/* The router, when there is one: another address of the same subnet. */
	if (request->address[0] != '\0' && request->gateway[0] != '\0') {
		error = lan_parse(request->gateway, &gateway);
		usable = lan_unicast(gateway);
		if (error != 0 || !usable)
			return lan_fail(diagnostic, capacity, "invalid router");
		if ((gateway & mask) != (address & mask) ||
		    gateway == address ||
		    (gateway & ~mask) == 0U ||
		    (gateway & ~mask) == ~mask)
			return lan_fail(diagnostic, capacity, "router outside the subnet");
	}

	/* Each DNS server. */
	for (index = 0; index < request->dns_count; index++) {
		error = lan_parse(request->dns[index], &server);
		usable = lan_unicast(server);
		if (error != 0 || !usable)
			return lan_fail(diagnostic, capacity, "invalid DNS server");
	}

	/* Succeeded: the configuration may be written. */
	return 0;
}

/*
 * Tells whether a checked configuration asks for a static address.
 */
int
networkd_lan_configure_static(
	const struct networkd_lan_configure *request)
{
	/* An address named is static. */
	if (request->address[0] != '\0')
		return 1;

	/* None is DHCP. */
	return 0;
}

/*
 * Edits net.conf's configuration for a checked request.  Returns 0, or -1
 * with errno (ENOSPC when no entry is left for the interface) and the
 * reason in diagnostic.
 */
int
networkd_lan_configure_edit(
	struct netconf *configuration,
	const struct networkd_lan_configure *request,
	char *diagnostic,
	size_t capacity)
{
	struct netconf_interface *item;
	struct netconf_route *route;
	uint32_t networks[2];
	uint32_t masks[2];
	uint32_t mask;
	size_t index;
	int differs;
	int fixed;
	int replaced;

	/* The interface's entry. */
	item = NULL;
	for (index = 0; index < configuration->interface_count; index++) {
		differs = strcmp(configuration->interfaces[index].name, request->interface);
		if (differs == 0) {
			item = &configuration->interfaces[index];
			break;
		}
	}

	/* A new one when net.conf does not name it, while there is room. */
	if (item == NULL) {
		if (configuration->interface_count >= NETCONF_MAX_INTERFACES) {
			(void)lan_fail(diagnostic, capacity, "no room for another interface");
			errno = ENOSPC;
			return -1;
		}

		/* The entry, named. */
		item = &configuration->interfaces[configuration->interface_count];
		memset(item, 0, sizeof(*item));
		lan_copy(item->name, sizeof(item->name), request->interface);
		configuration->interface_count++;
	}

	/* The subnet of its static address before (none: a mask of nothing, which holds no router). */
	lan_subnet(item, &networks[0], &masks[0]);

	/* An Ethernet interface, enabled. */
	item->type = NETCONF_INTERFACE_ETHERNET;
	item->enabled = 1;
	item->enabled_set = 1;

	/* DHCP (its timeout kept), or the one static address. */
	fixed = networkd_lan_configure_static(request);
	memset(item->addresses, 0, sizeof(item->addresses));
	item->address_count = 0;
	item->dhcp_set = 1;
	if (!fixed) {
		item->dhcp = 1;
	} else {
		(void)lan_parse(request->netmask, &mask);
		item->dhcp = 0;
		item->dhcp_timeout = 0;
		item->dhcp_timeout_set = 0;
		lan_copy(item->addresses[0].address, sizeof(item->addresses[0].address), request->address);
		item->addresses[0].prefix_length = (unsigned)lan_prefix(mask);
		item->address_count = 1;
	}

	/*
	 * The default route is this interface's when its router lies in the
	 * subnet it had or has now: that one goes (another interface's stays),
	 * and so does any when a router of its own replaces it.
	 */
	lan_subnet(item, &networks[1], &masks[1]);
	replaced = 0;
	if (fixed && request->gateway[0] != '\0')
		replaced = 1;
	lan_drop_default(configuration, replaced, networks, masks, 2U);

	/* A static address's router is the default route. */
	if (fixed && request->gateway[0] != '\0') {
		if (configuration->route_count >= NETCONF_MAX_ROUTES) {
			(void)lan_fail(diagnostic, capacity, "no room for the default route");
			errno = ENOSPC;
			return -1;
		}

		/* The route. */
		route = &configuration->routes[configuration->route_count];
		memset(route, 0, sizeof(*route));
		lan_copy(route->destination, sizeof(route->destination), LAN_DEFAULT_ROUTE);
		lan_copy(route->gateway, sizeof(route->gateway), request->gateway);
		configuration->route_count++;
	}

	/* The name servers: the ones named, or DHCP's. */
	memset(configuration->dns_servers, 0, sizeof(configuration->dns_servers));
	configuration->dns_count = 0;
	configuration->dns_mode = NETCONF_DNS_DHCP;
	for (index = 0; index < request->dns_count && index < NETCONF_MAX_DNS; index++) {
		lan_copy(configuration->dns_servers[index], sizeof(configuration->dns_servers[index]), request->dns[index]);
		configuration->dns_count++;
		configuration->dns_mode = NETCONF_DNS_STATIC;
	}

	/* Succeeded: the configuration says what was asked. */
	return 0;
}

/* Reads a dotted IPv4 address strictly (four numbers 0 to 255, no leading zero but a lone 0); returns 0 or -1. */
static int
lan_parse(
	const char *text,
	uint32_t *address)
{
	const char *cursor;
	uint32_t value;
	unsigned part;
	unsigned digits;
	unsigned number;

	/* Four numbers between dots. */
	value = 0U;
	cursor = text;
	for (part = 0U; part < 4U; part++) {
		/* One to three digits, 0 to 255, no leading zero. */
		number = 0U;
		digits = 0U;
		while (*cursor >= '0' && *cursor <= '9') {
			if (digits == 1U && number == 0U)
				return -1;
			number = number * 10U + (unsigned)(*cursor - '0');
			digits++;
			cursor++;
			if (digits > 3U || number > 255U)
				return -1;
		}

		/* At least one digit makes the number. */
		if (digits == 0U)
			return -1;
		value = (value << 8) | number;

		/* A dot between, nothing after the last. */
		if (part < 3U) {
			if (*cursor != '.')
				return -1;
			cursor++;
		}
	}

	/* Nothing after the fourth number. */
	if (*cursor != '\0')
		return -1;

	/* Succeeded: the address in host order. */
	*address = value;
	return 0;
}

/* Gives a netmask's prefix length, or -1 when its ones are not contiguous. */
static int
lan_prefix(
	uint32_t mask)
{
	uint32_t inverse;
	int prefix;

	/* The ones from the top, then only zeros. */
	inverse = ~mask;
	if ((inverse & (inverse + 1U)) != 0U)
		return -1;
	prefix = 0;
	while (prefix < 32 && (mask & (0x80000000U >> prefix)) != 0U)
		prefix++;

	/* Succeeded: the prefix length. */
	return prefix;
}

/* Tells whether an address can be a machine's: not 0.x, the loopback, multicast or above. */
static int
lan_unicast(
	uint32_t address)
{
	uint32_t first;

	/* The first number. */
	first = address >> 24;
	if (first == 0U || first == 127U || first >= 224U)
		return 0;

	/* Succeeded: an address a machine may have. */
	return 1;
}

/* Tells whether a name can be a wired interface's: 1 to 15 lower-case letters and digits, a letter first, not lo nor wlan. */
static int
lan_name_valid(
	const char *name)
{
	size_t length;
	size_t index;
	int differs;

	/* The length, and a letter first. */
	length = strlen(name);
	if (length == 0U || length > NETCONF_NAME_MAX)
		return 0;
	if (name[0] < 'a' || name[0] > 'z')
		return 0;

	/* Only lower-case letters and digits. */
	for (index = 0; index < length; index++) {
		if ((name[index] < 'a' || name[index] > 'z') && (name[index] < '0' || name[index] > '9'))
			return 0;
	}

	/* Not the loopback nor a radio. */
	differs = strncmp(name, "lo", 2U);
	if (differs == 0)
		return 0;
	differs = strncmp(name, "wlan", 4U);
	if (differs == 0)
		return 0;

	/* Succeeded: a wired interface's name. */
	return 1;
}

/* Says why a configuration is refused; returns -1 with errno EINVAL. */
static int
lan_fail(
	char *diagnostic,
	size_t capacity,
	const char *reason)
{
	/* The reason, when there is room for it. */
	if (diagnostic != NULL && capacity != 0U)
		(void)snprintf(diagnostic, capacity, "%s", reason);

	/* Refused. */
	errno = EINVAL;
	return -1;
}

/* Copies a text into a room, cut to fit (a checked text always fits). */
static void
lan_copy(
	char *destination,
	size_t size,
	const char *source)
{
	size_t length;

	/* As much as fits, and the end. */
	length = strlen(source);
	if (length >= size)
		length = size - 1U;
	memcpy(destination, source, length);
	destination[length] = '\0';
}

/* Gives the subnet of an interface's static address (none: zero with a mask of nothing). */
static void
lan_subnet(
	const struct netconf_interface *item,
	uint32_t *network,
	uint32_t *mask)
{
	uint32_t address;
	unsigned prefix;
	int error;

	/* No static address, no subnet. */
	*network = 0U;
	*mask = 0U;
	if (item->dhcp || item->address_count == 0U)
		return;

	/* Its first address and prefix, when they read. */
	error = lan_parse(item->addresses[0].address, &address);
	prefix = item->addresses[0].prefix_length;
	if (error != 0 || prefix == 0U || prefix > 32U)
		return;

	/* The subnet. */
	*mask = 0xffffffffU << (32U - prefix);
	*network = address & *mask;
}

/*
 * Removes the default route from net.conf's routes: always, or when its
 * router lies in one of the subnets given (a mask of nothing holds none).
 */
static void
lan_drop_default(
	struct netconf *configuration,
	int always,
	const uint32_t *networks,
	const uint32_t *masks,
	size_t count)
{
	uint32_t gateway;
	size_t index;
	size_t which;
	size_t kept;
	int differs;
	int error;
	int owned;

	/* Every route but a default that goes, in their order. */
	kept = 0;
	for (index = 0; index < configuration->route_count; index++) {
		differs = strcmp(configuration->routes[index].destination, LAN_DEFAULT_ROUTE);
		owned = 0;
		if (differs == 0)
			owned = always;

		/* A default whose router lies in a subnet given. */
		error = lan_parse(configuration->routes[index].gateway, &gateway);
		for (which = 0; differs == 0 && error == 0 && which < count; which++) {
			if (masks[which] != 0U && (gateway & masks[which]) == networks[which])
				owned = 1;
		}

		/* Kept unless it goes. */
		if (owned)
			continue;
		configuration->routes[kept] = configuration->routes[index];
		kept++;
	}

	/* The routes kept. */
	configuration->route_count = kept;
}
