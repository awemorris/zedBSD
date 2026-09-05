/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Builds the bounded complete-intent wired reconcile sequence. */

#include "userland/base/net/reconcile.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static const struct netconf_interface *find_interface(const struct netconf *,
	const char *);
static int emit_interface(const struct netconf_interface *,
	netconf_reconcile_emit, void *);
static int prefix_mask(unsigned, char *, size_t);

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
	if (configuration->route_count > 1U) {
		if (error != NULL && capacity != 0U)
			(void)snprintf(error, capacity,
			    "only one default route is currently applicable");
		errno = EOPNOTSUPP;
		return -1;
	}
	for (index = 0U; index < configuration->route_count; index++) {
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
	if (target->route_count != 0U &&
	    (emit("DEFAULTROUTE_CLEAR", NULL, context) != 0 ||
	    emit("DEFAULTROUTE", target->routes[0].gateway, context) != 0))
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
