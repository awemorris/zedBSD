/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The IPv4 routing table.
 *
 * A fixed table of routes, each holding a reference on its device, is
 * guarded by a spin lock that also disables interrupts.  Lookups choose
 * the longest matching prefix and hand back a copy with its own device
 * reference.  The route ioctls add, delete, and enumerate entries.
 */

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

static bool route_lock(void);
static void route_unlock(bool enabled);
static int mask_prefix(uint32_t mask, unsigned *result);
static int sockaddr_address(const struct sockaddr *address, uint32_t *result);
static void set_sockaddr(struct sockaddr *address, uint32_t value);
static int route_delete_request(uint32_t network, uint32_t netmask, struct net_device *device);

/*
 * Empties the routing table.
 */
void
route_init(
	void)
{
	bool enabled;
	unsigned index;

	enabled = route_lock();

	/* Releases the devices of any routes left from a previous init. */
	for (index = 0; index < ROUTE_MAX; index++) {
		if (route_used[index] && routes[index].device != NULL)
			net_device_release(routes[index].device);
	}

	/* Starts with an empty table. */
	memset(routes, 0, sizeof(routes));
	memset(route_used, 0, sizeof(route_used));
	route_unlock(enabled);
}

/*
 * Adds a route with explicit flags, or updates an existing one.
 *
 * A route to the same network through the same device takes the new
 * gateway and flags.  A new route holds a reference on its device.
 */
int
route_add_flags(
	uint32_t network,
	uint32_t netmask,
	uint32_t gateway,
	struct net_device *device,
	unsigned flags)
{
	bool enabled;
	unsigned index;
	unsigned free_index;
	int result;

	free_index = ROUTE_MAX;
	result = 0;

	/* Rejects a missing device, a bad mask, or inconsistent flags. */
	if (device == NULL ||
	    mask_prefix(netmask, NULL) != 0 ||
	    (network & ~netmask) != 0 ||
	    !(flags & RTF_UP) ||
	    (flags & ~ROUTE_FLAGS_ALLOWED) != 0 ||
	    (((flags & RTF_GATEWAY) != 0) != (gateway != 0)) ||
	    ((flags & RTF_HOST) != 0 && netmask != 0xffffffffU))
		return EINVAL;

	/* Holds the device for the route. */
	enabled = route_lock();
	if (!net_device_ref_live(device)) {
		route_unlock(enabled);
		return ENODEV;
	}

	/* Updates an existing route, remembering the first free slot. */
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
			goto out_release;
		}
	}

	/* Fills a free slot with the new route. */
	if (free_index == ROUTE_MAX) {
		result = ENOSPC;
		goto out_release;
	}
	route_used[free_index] = 1;
	routes[free_index].network = network;
	routes[free_index].netmask = netmask;
	routes[free_index].gateway = gateway;
	routes[free_index].device = device;
	routes[free_index].flags = flags;
	route_unlock(enabled);

	/* Reports the added route, which keeps the device reference. */
	return 0;

out_release:
	route_unlock(enabled);
	net_device_release(device);

	/* Reports the update or the failure. */
	return result;
}

/*
 * Adds an up route, marking it a gateway route when it has a gateway.
 */
int
route_add(
	uint32_t network,
	uint32_t netmask,
	uint32_t gateway,
	struct net_device *device)
{
	unsigned flags;
	int error;

	/* Derives the flags from the gateway. */
	flags = RTF_UP;
	if (gateway != 0)
		flags |= RTF_GATEWAY;

	error = route_add_flags(network, netmask, gateway, device, flags);

	/* Reports the add result. */
	return error;
}

/*
 * Deletes the route to a network through a device.
 */
int
route_delete(
	uint32_t network,
	uint32_t netmask,
	struct net_device *device)
{
	bool enabled;
	unsigned index;

	enabled = route_lock();

	/* Clears the matching route and drops its device reference. */
	for (index = 0; index < ROUTE_MAX; index++) {
		if (route_used[index] &&
		    routes[index].network == network &&
		    routes[index].netmask == netmask &&
		    routes[index].device == device) {
			net_device_release(routes[index].device);
			route_used[index] = 0;
			memset(&routes[index], 0, sizeof(routes[index]));
			route_unlock(enabled);
			return 0;
		}
	}
	route_unlock(enabled);

	/* Reports a missing route. */
	return ENOENT;
}

/*
 * Deletes every route through a device that is going away.
 */
void
route_purge_device(
	struct net_device *device)
{
	bool enabled;
	unsigned index;

	/* Ignores a missing device. */
	if (device == NULL)
		return;

	/* Clears the device's routes and drops their references. */
	enabled = route_lock();
	for (index = 0; index < ROUTE_MAX; index++) {
		if (route_used[index] && routes[index].device == device) {
			net_device_release(routes[index].device);
			route_used[index] = 0;
			memset(&routes[index], 0, sizeof(routes[index]));
		}
	}
	route_unlock(enabled);
}

/*
 * Copies the route at an ordinal position with a device reference.
 *
 * Routes whose device is no longer live are skipped, so ordinals count
 * only usable routes.
 */
int
route_get_ref(
	unsigned ordinal,
	struct net_route *result)
{
	bool enabled;
	unsigned index;

	/* Rejects a missing result. */
	if (result == NULL)
		return EINVAL;

	/* Counts down the ordinal over the live routes. */
	enabled = route_lock();
	for (index = 0; index < ROUTE_MAX; index++) {
		if (!route_used[index])
			continue;
		if (!net_device_ref_live(routes[index].device))
			continue;
		if (ordinal-- == 0) {
			*result = routes[index];
			route_unlock(enabled);
			return 0;
		}
		net_device_release(routes[index].device);
	}
	route_unlock(enabled);

	/* Reports an ordinal past the end. */
	return ENOENT;
}

/*
 * Finds the longest-prefix route to a destination with a device reference.
 */
int
route_lookup_ref(
	uint32_t destination,
	struct net_route *result)
{
	const struct net_route *best;
	bool enabled;
	unsigned best_prefix;
	unsigned index;
	unsigned prefix;

	best = NULL;
	best_prefix = 0;

	/* Rejects a missing result. */
	if (result == NULL)
		return EINVAL;

	/* Keeps the matching live route with the longest prefix. */
	enabled = route_lock();
	for (index = 0; index < ROUTE_MAX; index++) {
		prefix = 0;
		if (!route_used[index] ||
		    (destination & routes[index].netmask) != routes[index].network)
			continue;
		if (!net_device_ref_live(routes[index].device))
			continue;
		(void)mask_prefix(routes[index].netmask, &prefix);
		if (best == NULL || prefix > best_prefix) {
			if (best != NULL)
				net_device_release(best->device);
			best = &routes[index];
			best_prefix = prefix;
		} else {
			net_device_release(routes[index].device);
		}
	}

	/* Reports an unreachable destination. */
	if (best == NULL) {
		route_unlock(enabled);
		return ENETUNREACH;
	}

	/* Copies the route; the caller owns the device reference. */
	*result = *best;
	route_unlock(enabled);

	/* Reports the found route. */
	return 0;
}

/*
 * Releases a route copy and its device reference.
 */
void
route_release(
	struct net_route *route)
{
	/* Ignores a missing route. */
	if (route == NULL)
		return;

	/* Drops the device reference and clears the copy. */
	if (route->device != NULL)
		net_device_release(route->device);
	memset(route, 0, sizeof(*route));
}

/*
 * Handles the route ioctls: get by index, add, and delete.
 *
 * An added route without an interface takes the interface of the route
 * to its gateway.
 */
int
route_ioctl(
	unsigned long request,
	uintptr_t argument)
{
	struct rtentry entry;
	struct net_route route;
	struct net_route gateway_route;
	struct net_device *device;
	uint32_t network;
	uint32_t netmask;
	uint32_t gateway;
	uint32_t ordinal;
	unsigned flags;
	int error;

	/* Reads the request entry. */
	if (argument == 0)
		return EFAULT;
	error = copyin(argument, &entry, sizeof(entry));
	if (error != 0)
		return error;

	/* Describes the route at the requested index. */
	if (request == SIOCGRTENTRY) {
		ordinal = entry.rt_index;
		error = route_get_ref(entry.rt_index, &route);
		if (error != 0)
			return error;
		memset(&entry, 0, sizeof(entry));
		entry.rt_index = ordinal;
		if (route.device != NULL)
			entry.rt_ifindex = route.device->ifindex;
		else
			entry.rt_ifindex = 0;
		entry.rt_flags = route.flags;
		set_sockaddr(&entry.rt_dst, route.network);
		set_sockaddr(&entry.rt_genmask, route.netmask);
		set_sockaddr(&entry.rt_gateway, route.gateway);
		error = copyout(&entry, argument, sizeof(entry));
		route_release(&route);
		return error;
	}

	/* Only add and delete remain. */
	if (request != SIOCADDRT && request != SIOCDELRT)
		return EOPNOTSUPP;

	/* Takes the addresses and the named interface. */
	error = sockaddr_address(&entry.rt_dst, &network);
	if (error != 0)
		return error;
	error = sockaddr_address(&entry.rt_genmask, &netmask);
	if (error != 0)
		return error;
	error = sockaddr_address(&entry.rt_gateway, &gateway);
	if (error != 0)
		return error;
	if (entry.rt_ifindex != 0)
		device = net_device_find_by_index_ref(entry.rt_ifindex);
	else
		device = NULL;
	if (entry.rt_ifindex != 0 && device == NULL)
		return ENODEV;

	/* Deletes the route, on any interface when none was named. */
	if (request == SIOCDELRT) {
		error = route_delete_request(network, netmask, device);
		net_device_release(device);
		return error;
	}

	/* Without an interface, uses the one that reaches the gateway. */
	flags = entry.rt_flags;
	if (device == NULL && gateway != 0) {
		if (route_lookup_ref(gateway, &gateway_route) != 0)
			return ENETUNREACH;
		device = gateway_route.device;
		gateway_route.device = NULL;
		route_release(&gateway_route);
	}
	if (device == NULL)
		return ENODEV;

	/* Adds the route. */
	error = route_add_flags(network, netmask, gateway, device, flags);
	net_device_release(device);

	/* Reports the add result. */
	return error;
}

/* Disables interrupts, when the HAL is present, and takes the table lock. */
static bool
route_lock(
	void)
{
	bool enabled;

	/* Without the HAL there are no interrupts to disable. */
	if (hal_irq_disable != NULL)
		enabled = hal_irq_disable();
	else
		enabled = false;

	/* Spins for the lock. */
	while (!atomic_try_acquire_zero(&route_guard))
		__asm__ volatile("" ::: "memory");

	/* Reports whether interrupts were enabled. */
	return enabled;
}

/* Releases the table lock and restores the interrupt state. */
static void
route_unlock(
	bool enabled)
{
	atomic_store_release(&route_guard, 0);

	/* Re-enables interrupts only when they were enabled before. */
	if (enabled && hal_irq_enable != NULL)
		hal_irq_enable();
}

/* Counts the prefix bits of a netmask, refusing a non-contiguous one. */
static int
mask_prefix(
	uint32_t mask,
	unsigned *result)
{
	unsigned count;
	int zero_seen;
	uint32_t bit;

	count = 0;
	zero_seen = 0;

	/* Counts the leading ones; a one after a zero is invalid. */
	for (bit = 0x80000000U; bit != 0; bit >>= 1) {
		if (mask & bit) {
			if (zero_seen)
				return EINVAL;
			count++;
		} else {
			zero_seen = 1;
		}
	}

	/* Reports the prefix length when asked. */
	if (result != NULL)
		*result = count;

	/* Reports a valid mask. */
	return 0;
}

/* Extracts the IPv4 address of a socket address in host order. */
static int
sockaddr_address(
	const struct sockaddr *address,
	uint32_t *result)
{
	const struct sockaddr_in *inet;

	inet = (const struct sockaddr_in *)address;

	/* Only AF_INET addresses are understood. */
	if (address->sa_family != AF_INET)
		return EAFNOSUPPORT;

	*result = net_ntohl(inet->sin_addr.s_addr);

	/* Reports the extracted address. */
	return 0;
}

/* Fills a socket address with an IPv4 address given in host order. */
static void
set_sockaddr(
	struct sockaddr *address,
	uint32_t value)
{
	struct sockaddr_in *inet;

	inet = (struct sockaddr_in *)address;
	memset(address, 0, sizeof(*address));
	inet->sin_family = AF_INET;
	inet->sin_addr.s_addr = net_htonl(value);
}

/* Deletes the single route matching an ioctl request. */
static int
route_delete_request(
	uint32_t network,
	uint32_t netmask,
	struct net_device *device)
{
	bool enabled;
	unsigned index;
	unsigned matches;
	unsigned selected;

	matches = 0;
	selected = ROUTE_MAX;

	/* Counts the routes to the network, on the device when one was named. */
	enabled = route_lock();
	for (index = 0; index < ROUTE_MAX; index++) {
		if (route_used[index] &&
		    routes[index].network == network &&
		    routes[index].netmask == netmask &&
		    (device == NULL || routes[index].device == device)) {
			matches++;
			selected = index;
		}
	}

	/* The request must name exactly one route. */
	if (matches == 0) {
		route_unlock(enabled);
		return ENOENT;
	}
	if (matches != 1) {
		route_unlock(enabled);
		return EBUSY;
	}

	/* Clears the route and drops its device reference. */
	net_device_release(routes[selected].device);
	route_used[selected] = 0;
	memset(&routes[selected], 0, sizeof(routes[selected]));
	route_unlock(enabled);

	/* Reports the deleted route. */
	return 0;
}
