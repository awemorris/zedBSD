/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/route.h"
#include "kern/net/byteorder.h"
#include "kern/net/net-device.h"
#include "kern/atomic.h"
#include "kern/uaccess.h"

#include <zedbsd/netinet.h>
#include <zedbsd/route.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#define ROUTE_MAX 16U
#define ROUTE_FLAGS_ALLOWED (RTF_UP | RTF_GATEWAY | RTF_HOST | RTF_STATIC | \
	RTF_DYNAMIC | RTF_CONNECTED)

static struct net_route routes[ROUTE_MAX];
static uint8_t route_used[ROUTE_MAX];
static atomic_uint_t route_guard;

extern bool hal_irq_disable(void) __attribute__((weak));
extern void hal_irq_enable(void) __attribute__((weak));

static bool
route_lock(void)
{
	bool enabled = hal_irq_disable != NULL ? hal_irq_disable() : false;

	while (!atomic_try_acquire_zero(&route_guard))
		__asm__ volatile("" ::: "memory");
	return enabled;
}

static void
route_unlock(bool enabled)
{
	atomic_store_release(&route_guard, 0);
	if (enabled && hal_irq_enable != NULL)
		hal_irq_enable();
}

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
	bool enabled = route_lock();
	unsigned index;

	for (index = 0; index < ROUTE_MAX; index++)
		if (route_used[index] && routes[index].device != NULL)
			net_device_release(routes[index].device);
	memset(routes, 0, sizeof(routes));
	memset(route_used, 0, sizeof(route_used));
	route_unlock(enabled);
}

int
route_add_flags(uint32_t network, uint32_t netmask, uint32_t gateway,
		struct net_device *device, unsigned flags)
{
	bool enabled;
	unsigned index, free_index = ROUTE_MAX;

	if (device == NULL || mask_prefix(netmask, NULL) != 0 ||
	    (network & ~netmask) != 0 || !(flags & RTF_UP) ||
	    (flags & ~ROUTE_FLAGS_ALLOWED) != 0 ||
	    (((flags & RTF_GATEWAY) != 0) != (gateway != 0)) ||
	    ((flags & RTF_HOST) != 0 && netmask != 0xffffffffU))
		return EINVAL;
	enabled = route_lock();
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
			route_unlock(enabled);
			return 0;
		}
	}
	if (free_index == ROUTE_MAX) {
		route_unlock(enabled);
		return ENOSPC;
	}
	route_used[free_index] = 1;
	routes[free_index].network = network;
	routes[free_index].netmask = netmask;
	routes[free_index].gateway = gateway;
	routes[free_index].device = device;
	routes[free_index].flags = flags;
	net_device_ref(device);
	route_unlock(enabled);
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
	bool enabled = route_lock();
	unsigned index;

	for (index = 0; index < ROUTE_MAX; index++)
		if (route_used[index] && routes[index].network == network &&
		    routes[index].netmask == netmask &&
		    routes[index].device == device) {
			net_device_release(routes[index].device);
			route_used[index] = 0;
			memset(&routes[index], 0, sizeof(routes[index]));
			route_unlock(enabled);
			return 0;
		}
	route_unlock(enabled);
	return ENOENT;
}

void
route_purge_device(struct net_device *device)
{
	bool enabled;
	unsigned index;

	if (device == NULL)
		return;
	enabled = route_lock();
	for (index = 0; index < ROUTE_MAX; index++)
		if (route_used[index] && routes[index].device == device) {
			net_device_release(routes[index].device);
			route_used[index] = 0;
			memset(&routes[index], 0, sizeof(routes[index]));
		}
	route_unlock(enabled);
}

int
route_get_ref(unsigned ordinal, struct net_route *result)
{
	bool enabled;
	unsigned index;

	if (result == NULL)
		return EINVAL;
	enabled = route_lock();
	for (index = 0; index < ROUTE_MAX; index++) {
		if (!route_used[index])
			continue;
		if (ordinal-- == 0) {
			*result = routes[index];
			if (result->device != NULL)
				net_device_ref(result->device);
			route_unlock(enabled);
			return 0;
		}
	}
	route_unlock(enabled);
	return ENOENT;
}

int
route_lookup_ref(uint32_t destination, struct net_route *result)
{
	const struct net_route *best = NULL;
	bool enabled;
	unsigned best_prefix = 0, index;

	if (result == NULL)
		return EINVAL;
	enabled = route_lock();
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
	if (best == NULL) {
		route_unlock(enabled);
		return ENETUNREACH;
	}
	*result = *best;
	if (result->device != NULL)
		net_device_ref(result->device);
	route_unlock(enabled);
	return 0;
}

void
route_release(struct net_route *route)
{
	if (route == NULL)
		return;
	if (route->device != NULL)
		net_device_release(route->device);
	memset(route, 0, sizeof(*route));
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
	bool enabled = route_lock();
	unsigned index, matches = 0, selected = ROUTE_MAX;

	for (index = 0; index < ROUTE_MAX; index++)
		if (route_used[index] && routes[index].network == network &&
		    routes[index].netmask == netmask &&
		    (device == NULL || routes[index].device == device)) {
			matches++;
			selected = index;
		}
	if (matches == 0) {
		route_unlock(enabled);
		return ENOENT;
	}
	if (matches != 1) {
		route_unlock(enabled);
		return EBUSY;
	}
	net_device_release(routes[selected].device);
	route_used[selected] = 0;
	memset(&routes[selected], 0, sizeof(routes[selected]));
	route_unlock(enabled);
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
		error = route_get_ref(entry.rt_index, &route);
		if (error != 0)
			return error;
		memset(&entry, 0, sizeof(entry));
		entry.rt_index = ordinal;
		entry.rt_ifindex = route.device != NULL ? route.device->ifindex : 0;
		entry.rt_flags = route.flags;
		set_sockaddr(&entry.rt_dst, route.network);
		set_sockaddr(&entry.rt_genmask, route.netmask);
		set_sockaddr(&entry.rt_gateway, route.gateway);
		error = copyout(&entry, argument, sizeof(entry));
		route_release(&route);
		return error;
	}
	if (request != SIOCADDRT && request != SIOCDELRT)
		return EOPNOTSUPP;
	if ((error = sockaddr_address(&entry.rt_dst, &network)) != 0 ||
	    (error = sockaddr_address(&entry.rt_genmask, &netmask)) != 0 ||
	    (error = sockaddr_address(&entry.rt_gateway, &gateway)) != 0)
		return error;
	device = entry.rt_ifindex != 0 ?
	    net_device_find_by_index_ref(entry.rt_ifindex) : NULL;
	if (entry.rt_ifindex != 0 && device == NULL)
		return ENODEV;
	if (request == SIOCDELRT) {
		error = route_delete_request(network, netmask, device);
		net_device_release(device);
		return error;
	}
	flags = entry.rt_flags;
	if (device == NULL && gateway != 0) {
		struct net_route gateway_route;
		if (route_lookup_ref(gateway, &gateway_route) != 0)
			return ENETUNREACH;
		device = gateway_route.device;
		gateway_route.device = NULL;
		route_release(&gateway_route);
	}
	if (device == NULL)
		return ENODEV;
	error = route_add_flags(network, netmask, gateway, device, flags);
	net_device_release(device);
	return error;
}
