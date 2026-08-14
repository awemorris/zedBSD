/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/route.h"
#include "kern/net/byteorder.h"
#include "kern/net/net-device.h"
#include "kern/uaccess.h"

#include <zedbsd/netinet.h>
#include <zedbsd/route.h>
#include <errno.h>
#include <string.h>

#define ROUTE_MAX 16U
#define ROUTE_FLAGS_ALLOWED (RTF_UP | RTF_GATEWAY | RTF_HOST | RTF_STATIC | \
	RTF_DYNAMIC | RTF_CONNECTED)

static struct net_route routes[ROUTE_MAX];
static uint8_t route_used[ROUTE_MAX];

static int
mask_prefix(uint32_t mask, unsigned *result)
{
	unsigned count = 0;
	int zero_seen = 0;
	uint32_t bit;

	for (bit = 0x80000000U; bit != 0; bit >>= 1) {
		if (mask & bit) {
			if (zero_seen)
				return EINVAL;
			count++;
		} else {
			zero_seen = 1;
		}
	}
	if (result != NULL)
		*result = count;
	return 0;
}

void
route_init(void)
{
	unsigned index;

	for (index = 0; index < ROUTE_MAX; index++)
		if (route_used[index] && routes[index].device != NULL)
			net_device_release(routes[index].device);
	memset(routes, 0, sizeof(routes));
	memset(route_used, 0, sizeof(route_used));
}

int
route_add_flags(uint32_t network, uint32_t netmask, uint32_t gateway,
		struct net_device *device, unsigned flags)
{
	unsigned index, free_index = ROUTE_MAX;

	if (device == NULL || mask_prefix(netmask, NULL) != 0 ||
	    (network & ~netmask) != 0 || !(flags & RTF_UP) ||
	    (flags & ~ROUTE_FLAGS_ALLOWED) != 0 ||
	    (((flags & RTF_GATEWAY) != 0) != (gateway != 0)) ||
	    ((flags & RTF_HOST) != 0 && netmask != 0xffffffffU))
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
			routes[index].flags = flags;
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
	routes[free_index].flags = flags;
	net_device_ref(device);
	return 0;
}

int
route_add(uint32_t network, uint32_t netmask, uint32_t gateway,
	  struct net_device *device)
{
	return route_add_flags(network, netmask, gateway, device,
	    RTF_UP | (gateway != 0 ? RTF_GATEWAY : 0));
}

int
route_delete(uint32_t network, uint32_t netmask, struct net_device *device)
{
	unsigned index;

	for (index = 0; index < ROUTE_MAX; index++)
		if (route_used[index] && routes[index].network == network &&
		    routes[index].netmask == netmask &&
		    routes[index].device == device) {
			net_device_release(routes[index].device);
			route_used[index] = 0;
			memset(&routes[index], 0, sizeof(routes[index]));
			return 0;
		}
	return ENOENT;
}

void
route_purge_device(struct net_device *device)
{
	unsigned index;

	if (device == NULL)
		return;
	for (index = 0; index < ROUTE_MAX; index++)
		if (route_used[index] && routes[index].device == device) {
			net_device_release(routes[index].device);
			route_used[index] = 0;
			memset(&routes[index], 0, sizeof(routes[index]));
		}
}

int
route_get(unsigned ordinal, struct net_route *result)
{
	unsigned index;

	if (result == NULL)
		return EINVAL;
	for (index = 0; index < ROUTE_MAX; index++) {
		if (!route_used[index])
			continue;
		if (ordinal-- == 0) {
			*result = routes[index];
			return 0;
		}
	}
	return ENOENT;
}

const struct net_route *
route_lookup(uint32_t destination)
{
	const struct net_route *best = NULL;
	unsigned best_prefix = 0, index;

	for (index = 0; index < ROUTE_MAX; index++) {
		unsigned prefix = 0;
		if (!route_used[index] ||
		    (destination & routes[index].netmask) != routes[index].network)
			continue;
		(void)mask_prefix(routes[index].netmask, &prefix);
		if (best == NULL || prefix > best_prefix) {
			best = &routes[index];
			best_prefix = prefix;
		}
	}
	return best;
}

static int
sockaddr_address(const struct sockaddr *address, uint32_t *result)
{
	const struct sockaddr_in *inet = (const struct sockaddr_in *)address;

	if (address->sa_family != AF_INET)
		return EAFNOSUPPORT;
	*result = net_ntohl(inet->sin_addr.s_addr);
	return 0;
}

static void
set_sockaddr(struct sockaddr *address, uint32_t value)
{
	struct sockaddr_in *inet = (struct sockaddr_in *)address;

	memset(address, 0, sizeof(*address));
	inet->sin_family = AF_INET;
	inet->sin_addr.s_addr = net_htonl(value);
}

static int
route_delete_request(uint32_t network, uint32_t netmask,
		     struct net_device *device)
{
	unsigned index, matches = 0, selected = ROUTE_MAX;

	for (index = 0; index < ROUTE_MAX; index++)
		if (route_used[index] && routes[index].network == network &&
		    routes[index].netmask == netmask &&
		    (device == NULL || routes[index].device == device)) {
			matches++;
			selected = index;
		}
	if (matches == 0)
		return ENOENT;
	if (matches != 1)
		return EBUSY;
	net_device_release(routes[selected].device);
	route_used[selected] = 0;
	memset(&routes[selected], 0, sizeof(routes[selected]));
	return 0;
}

int
route_ioctl(unsigned long request, uintptr_t argument)
{
	struct rtentry entry;
	struct net_route route;
	struct net_device *device;
	uint32_t network, netmask, gateway;
	unsigned flags;
	uint32_t ordinal;
	int error;

	if (argument == 0)
		return EFAULT;
	error = copyin(argument, &entry, sizeof(entry));
	if (error != 0)
		return error;
	if (request == SIOCGRTENTRY) {
		ordinal = entry.rt_index;
		error = route_get(entry.rt_index, &route);
		if (error != 0)
			return error;
		memset(&entry, 0, sizeof(entry));
		entry.rt_index = ordinal;
		entry.rt_ifindex = route.device != NULL ? route.device->ifindex : 0;
		entry.rt_flags = route.flags;
		set_sockaddr(&entry.rt_dst, route.network);
		set_sockaddr(&entry.rt_genmask, route.netmask);
		set_sockaddr(&entry.rt_gateway, route.gateway);
		return copyout(&entry, argument, sizeof(entry));
	}
	if (request != SIOCADDRT && request != SIOCDELRT)
		return EOPNOTSUPP;
	if ((error = sockaddr_address(&entry.rt_dst, &network)) != 0 ||
	    (error = sockaddr_address(&entry.rt_genmask, &netmask)) != 0 ||
	    (error = sockaddr_address(&entry.rt_gateway, &gateway)) != 0)
		return error;
	device = entry.rt_ifindex != 0 ?
	    net_device_find_by_index(entry.rt_ifindex) : NULL;
	if (entry.rt_ifindex != 0 && device == NULL)
		return ENODEV;
	if (request == SIOCDELRT)
		return route_delete_request(network, netmask, device);
	flags = entry.rt_flags;
	if (device == NULL && gateway != 0) {
		const struct net_route *gateway_route = route_lookup(gateway);
		if (gateway_route == NULL)
			return ENETUNREACH;
		device = gateway_route->device;
	}
	if (device == NULL)
		return ENODEV;
	return route_add_flags(network, netmask, gateway, device, flags);
}
