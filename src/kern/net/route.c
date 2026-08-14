/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/route.h"

#include <errno.h>
#include <string.h>

#define ROUTE_MAX 16U

static struct net_route routes[ROUTE_MAX];
static uint8_t route_used[ROUTE_MAX];

void route_init(void)
{
	memset(routes, 0, sizeof(routes));
	memset(route_used, 0, sizeof(route_used));
}

int
route_add(uint32_t network, uint32_t netmask, uint32_t gateway,
	  struct net_device *device)
{
	unsigned index, free_index = ROUTE_MAX;

	if (device == NULL || (network & ~netmask) != 0)
		return EINVAL;
	for (index = 0; index < ROUTE_MAX; index++) {
		if (!route_used[index]) {
			if (free_index == ROUTE_MAX)
				free_index = index;
			continue;
		}
		if (routes[index].network == network &&
		    routes[index].netmask == netmask &&
		    routes[index].device == device) {
			routes[index].gateway = gateway;
			return 0;
		}
	}
	if (free_index == ROUTE_MAX)
		return ENOSPC;
	route_used[free_index] = 1;
	routes[free_index].network = network;
	routes[free_index].netmask = netmask;
	routes[free_index].gateway = gateway;
	routes[free_index].device = device;
	return 0;
}

int
route_delete(uint32_t network, uint32_t netmask, struct net_device *device)
{
	unsigned index;

	for (index = 0; index < ROUTE_MAX; index++)
		if (route_used[index] && routes[index].network == network &&
		    routes[index].netmask == netmask &&
		    routes[index].device == device) {
			route_used[index] = 0;
			memset(&routes[index], 0, sizeof(routes[index]));
			return 0;
		}
	return ENOENT;
}

static unsigned prefix_length(uint32_t mask)
{
	unsigned count = 0;
	while ((mask & 0x80000000U) != 0) {
		count++;
		mask <<= 1;
	}
	return mask == 0 ? count : 0;
}

const struct net_route *route_lookup(uint32_t destination)
{
	const struct net_route *best = NULL;
	unsigned best_prefix = 0, index;

	for (index = 0; index < ROUTE_MAX; index++) {
		unsigned prefix;
		if (!route_used[index] ||
		    (destination & routes[index].netmask) != routes[index].network)
			continue;
		prefix = prefix_length(routes[index].netmask);
		if (best == NULL || prefix > best_prefix) {
			best = &routes[index];
			best_prefix = prefix;
		}
	}
	return best;
}
