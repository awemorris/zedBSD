/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Builds the bounded complete-intent wired reconcile sequence. */

#include "userland/base/net/reconcile.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static const struct netconf_interface *find_interface(const struct netconf *,
	const char *);
static int emit_interface(const struct netconf_interface *,
	netconf_reconcile_emit, void *);
static int prefix_mask(unsigned, char *, size_t);
static int route_ipv6(const struct netconf_route *);
static const struct netconf_route *route_ipv4(const struct netconf *);
static int routes_ipv6(const struct netconf *);
static int emit_ipv6(const struct netconf_interface *, netconf_reconcile_emit, void *);
static int emit_routes6(const struct netconf *, netconf_reconcile_emit, void *);
static int emit_removed6(const struct netconf *, const struct netconf *, netconf_reconcile_emit, void *);
static int address6_named(const struct netconf_interface *, const struct netconf_address *);
static int route6_named(const struct netconf *, const char *);

int
netconf_reconcile_supported(
	const struct netconf *configuration,
	char *error,
	size_t capacity)
{
	const struct netconf_interface *item;
	size_t index;

	if (netconf_validate(configuration, error, capacity) != 0)
		return -1;
	for (index = 0U; index < configuration->interface_count; index++) {
		item = &configuration->interfaces[index];
		if (item->type != NETCONF_INTERFACE_LOOPBACK &&
		    item->type != NETCONF_INTERFACE_ETHERNET) {
			if (error != NULL && capacity != 0U)
				(void)snprintf(error, capacity,
				    "interface %s type is not yet applicable",
				    item->name);
			errno = EOPNOTSUPP;
			return -1;
		}
		if (item->address_count > 1U) {
			if (error != NULL && capacity != 0U)
				(void)snprintf(error, capacity,
				    "interface %s has multiple addresses", item->name);
			errno = EOPNOTSUPP;
			return -1;
		}
	}
	/* One IPv4 route at most, the default; IPv6 routes of any destination (ws130-p005). */
	if (configuration->route_count - (size_t)routes_ipv6(configuration) > 1U) {
		if (error != NULL && capacity != 0U)
			(void)snprintf(error, capacity,
			    "only one default route is currently applicable");
		errno = EOPNOTSUPP;
		return -1;
	}
	for (index = 0U; index < configuration->route_count; index++) {
		if (route_ipv6(&configuration->routes[index]))
			continue;
		if (strcmp(configuration->routes[index].destination,
		    "default") != 0) {
			if (error != NULL && capacity != 0U)
				(void)snprintf(error, capacity,
				    "only a default route is currently applicable");
			errno = EOPNOTSUPP;
			return -1;
		}
	}
	if (error != NULL && capacity != 0U)
		error[0] = '\0';
	return 0;
}

int
netconf_reconcile(
	const struct netconf *previous,
	const struct netconf *target,
	netconf_reconcile_emit emit,
	void *context,
	char *error,
	size_t capacity)
{
	const struct netconf_interface *item;
	char operands[256];
	size_t index;
	size_t used;
	int count;

	if (previous == NULL || target == NULL || emit == NULL ||
	    netconf_reconcile_supported(previous, error, capacity) != 0 ||
	    netconf_reconcile_supported(target, error, capacity) != 0) {
		if (errno == 0)
			errno = EINVAL;
		return -1;
	}
	/* Remove global old intent before DHCP or explicit replacements run. */
	if (emit("DEFAULTROUTE_CLEAR", NULL, context) != 0 ||
	    emit("DNS_CLEAR", NULL, context) != 0)
		return -1;

	/* The IPv6 default route too, when either side has IPv6 routes (ws130-p005). */
	if ((routes_ipv6(previous) != 0 || routes_ipv6(target) != 0) &&
	    emit("ROUTE6_CLEAR", NULL, context) != 0)
		return -1;
	/* The static IPv6 addresses and IPv6 routes the target no longer names go (ws177-p045). */
	if (emit_removed6(previous, target, emit, context) != 0)
		return -1;
	/* Interfaces absent from the target become administratively down. */
	for (index = 0U; index < previous->interface_count; index++) {
		item = &previous->interfaces[index];
		if (find_interface(target, item->name) == NULL &&
		    emit("DOWN", item->name, context) != 0)
			return -1;
	}
	for (index = 0U; index < target->interface_count; index++) {
		if (emit_interface(&target->interfaces[index], emit, context) != 0)
			return -1;
	}
	/* An explicit route wins over any route acquired by DHCP. */
	if (route_ipv4(target) != NULL &&
	    (emit("DEFAULTROUTE_CLEAR", NULL, context) != 0 ||
	    emit("DEFAULTROUTE", route_ipv4(target)->gateway, context) != 0))
		return -1;

	/* The IPv6 routes (ws130-p005). */
	if (emit_routes6(target, emit, context) != 0)
		return -1;
	/* Explicit servers replace any DHCP resolver output. */
	if (target->dns_count != 0U) {
		used = 0U;
		for (index = 0U; index < target->dns_count; index++) {
			count = snprintf(operands + used, sizeof(operands) - used,
			    "%s%s", used == 0U ? "" : " ",
			    target->dns_servers[index]);
			if (count < 0 || (size_t)count >= sizeof(operands) - used) {
				errno = EOVERFLOW;
				return -1;
			}
			used += (size_t)count;
		}
		if (emit("DNS", operands, context) != 0)
			return -1;
	}
	if (error != NULL && capacity != 0U)
		error[0] = '\0';
	return 0;
}

static const struct netconf_interface *
find_interface(
	const struct netconf *configuration,
	const char *name)
{
	size_t index;

	for (index = 0U; index < configuration->interface_count; index++) {
		if (strcmp(configuration->interfaces[index].name, name) == 0)
			return &configuration->interfaces[index];
	}
	return NULL;
}

static int
emit_interface(
	const struct netconf_interface *item,
	netconf_reconcile_emit emit,
	void *context)
{
	char mask[32];
	char operands[256];
	int count;

	if (!item->enabled)
		return emit("DOWN", item->name, context);
	if (emit("UP", item->name, context) != 0)
		return -1;

	/* IPv6 named on or off, and the static IPv6 addresses (ws130-p005). */
	if (emit_ipv6(item, emit, context) != 0)
		return -1;
	if (item->dhcp) {
		count = snprintf(operands, sizeof(operands), "%s %u", item->name,
		    item->dhcp_timeout_set ? item->dhcp_timeout : 10U);
		if (count < 0 || (size_t)count >= sizeof(operands)) {
			errno = EOVERFLOW;
			return -1;
		}
		return emit("DHCP", operands, context);
	}
	if (item->address_count != 0U) {
		if (prefix_mask(item->addresses[0].prefix_length, mask,
		    sizeof(mask)) != 0)
			return -1;
		count = snprintf(operands, sizeof(operands),
		    "%s ipv4 %s netmask %s", item->name,
		    item->addresses[0].address, mask);
	} else {
		count = snprintf(operands, sizeof(operands),
		    "%s ipv4 0.0.0.0 netmask 0.0.0.0", item->name);
	}
	if (count < 0 || (size_t)count >= sizeof(operands)) {
		errno = EOVERFLOW;
		return -1;
	}
	return emit("STATIC", operands, context);
}

static int
prefix_mask(
	unsigned prefix,
	char *buffer,
	size_t capacity)
{
	unsigned long value;
	int count;

	if (prefix > 32U) {
		errno = EINVAL;
		return -1;
	}
	value = prefix == 0U ? 0UL : (0xffffffffUL << (32U - prefix)) &
	    0xffffffffUL;
	count = snprintf(buffer, capacity, "%lu.%lu.%lu.%lu",
	    (value >> 24) & 255UL, (value >> 16) & 255UL,
	    (value >> 8) & 255UL, value & 255UL);
	if (count < 0 || (size_t)count >= capacity) {
		errno = EOVERFLOW;
		return -1;
	}
	return 0;
}

/* Tells whether a route is IPv6's: its gateway is an IPv6 address (ws130-p005). */
static int
route_ipv6(
	const struct netconf_route *route)
{
	/* An IPv6 gateway has a colon. */
	return strchr(route->gateway, ':') != NULL;
}

/* Finds the IPv4 route (the default), NULL when there is none. */
static const struct netconf_route *
route_ipv4(
	const struct netconf *configuration)
{
	size_t index;

	/* The first route that is not IPv6's. */
	for (index = 0U; index < configuration->route_count; index++) {
		if (!route_ipv6(&configuration->routes[index]))
			return &configuration->routes[index];
	}

	/* None. */
	return NULL;
}

/* Counts the IPv6 routes. */
static int
routes_ipv6(
	const struct netconf *configuration)
{
	size_t index;
	int count;

	/* Each route of IPv6. */
	count = 0;
	for (index = 0U; index < configuration->route_count; index++) {
		if (route_ipv6(&configuration->routes[index]))
			count++;
	}

	/* The count. */
	return count;
}

/* Emits an interface's IPv6 named on or off, and its static IPv6 addresses while IPv6 is on. */
static int
emit_ipv6(
	const struct netconf_interface *item,
	netconf_reconcile_emit emit,
	void *context)
{
	char operands[256];
	size_t index;
	int count;
	int on;

	/* On or off, when the file names it. */
	on = netconf_ipv6_enabled(item);
	if (item->ipv6.enabled_set) {
		count = snprintf(operands, sizeof(operands), "%s %s", item->name, on ? "on" : "off");
		if (count < 0 || (size_t)count >= sizeof(operands)) {
			errno = EOVERFLOW;
			return -1;
		}
		if (emit("IPV6", operands, context) != 0)
			return -1;
	}
	if (!on)
		return 0;

	/* Each static address with its length. */
	for (index = 0U; index < item->ipv6.address_count; index++) {
		count = snprintf(operands, sizeof(operands), "%s %s/%u", item->name, item->ipv6.addresses[index].address,
		    item->ipv6.addresses[index].prefix_length);
		if (count < 0 || (size_t)count >= sizeof(operands)) {
			errno = EOVERFLOW;
			return -1;
		}
		if (emit("STATIC6", operands, context) != 0)
			return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Emits each IPv6 route: its destination, gateway, and the interface of a link-local gateway. */
static int
emit_routes6(
	const struct netconf *configuration,
	netconf_reconcile_emit emit,
	void *context)
{
	const struct netconf_route *route;
	char operands[256];
	size_t index;
	int count;

	/* Each IPv6 route. */
	for (index = 0U; index < configuration->route_count; index++) {
		route = &configuration->routes[index];
		if (!route_ipv6(route))
			continue;
		count = snprintf(operands, sizeof(operands), "%s %s%s%s", route->destination, route->gateway,
		    route->interface[0] != '\0' ? " " : "", route->interface);
		if (count < 0 || (size_t)count >= sizeof(operands)) {
			errno = EOVERFLOW;
			return -1;
		}
		if (emit("ROUTE6", operands, context) != 0)
			return -1;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Emits the removal of each static IPv6 address the previous program gave
 * an interface and the target does not (an interface whose IPv6 the
 * target turns off loses them all already), and of each IPv6 route the
 * previous program named whose destination the target does not route (the
 * default is ROUTE6_CLEAR's; a route the target names again is replaced).
 */
static int
emit_removed6(
	const struct netconf *previous,
	const struct netconf *target,
	netconf_reconcile_emit emit,
	void *context)
{
	const struct netconf_interface *item;
	const struct netconf_interface *next;
	const struct netconf_route *route;
	char operands[256];
	size_t index;
	size_t address;
	int named;
	int count;
	int on;

	/* Each interface's addresses the target drops. */
	for (index = 0U; index < previous->interface_count; index++) {
		item = &previous->interfaces[index];
		on = netconf_ipv6_enabled(item);
		if (!on)
			continue;
		next = find_interface(target, item->name);
		if (next != NULL) {
			on = netconf_ipv6_enabled(next);
			if (!on)
				continue;
		}

		/* Each address the target does not name again. */
		for (address = 0U; address < item->ipv6.address_count; address++) {
			named = 0;
			if (next != NULL)
				named = address6_named(next, &item->ipv6.addresses[address]);
			if (named)
				continue;
			count = snprintf(operands, sizeof(operands), "%s %s/%u", item->name, item->ipv6.addresses[address].address,
			    item->ipv6.addresses[address].prefix_length);
			if (count < 0 || (size_t)count >= sizeof(operands)) {
				errno = EOVERFLOW;
				return -1;
			}

			/* Taken away. */
			count = emit("STATIC6_REMOVE", operands, context);
			if (count != 0)
				return -1;
		}
	}

	/* Each IPv6 route whose destination the target does not route. */
	for (index = 0U; index < previous->route_count; index++) {
		route = &previous->routes[index];
		on = route_ipv6(route);
		if (!on)
			continue;
		count = strcmp(route->destination, "default");
		if (count == 0)
			continue;
		named = route6_named(target, route->destination);
		if (named)
			continue;
		count = emit("ROUTE6_REMOVE", route->destination, context);
		if (count != 0)
			return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Tells whether an interface of the target names a static IPv6 address (the same address and length). */
static int
address6_named(
	const struct netconf_interface *item,
	const struct netconf_address *wanted)
{
	struct in6_addr left;
	struct in6_addr right;
	size_t index;
	int parsed;
	int same;

	/* The address sought. */
	parsed = inet_pton(AF_INET6, wanted->address, &left);
	if (parsed != 1)
		return 0;

	/* Each of the interface's. */
	for (index = 0U; index < item->ipv6.address_count; index++) {
		if (item->ipv6.addresses[index].prefix_length != wanted->prefix_length)
			continue;
		parsed = inet_pton(AF_INET6, item->ipv6.addresses[index].address, &right);
		if (parsed != 1)
			continue;
		same = memcmp(&left, &right, sizeof(left));
		if (same == 0)
			return 1;
	}

	/* Not named. */
	return 0;
}

/* Tells whether the target has an IPv6 route to a destination. */
static int
route6_named(
	const struct netconf *configuration,
	const char *destination)
{
	size_t index;
	int ipv6;
	int same;

	/* Each IPv6 route. */
	for (index = 0U; index < configuration->route_count; index++) {
		ipv6 = route_ipv6(&configuration->routes[index]);
		if (!ipv6)
			continue;
		same = strcmp(configuration->routes[index].destination, destination);
		if (same == 0)
			return 1;
	}

	/* Not routed. */
	return 0;
}
