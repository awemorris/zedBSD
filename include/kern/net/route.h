/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Route
 */

#ifndef ZEDBSD_KERN_NET_ROUTE_H
#define ZEDBSD_KERN_NET_ROUTE_H

#include <stdint.h>

struct net_device;

struct net_route {
	uint32_t network;
	uint32_t netmask;
	uint32_t gateway;
	struct net_device *device;
	unsigned flags;
};

void
route_init(void);
int
route_add(
	uint32_t network,
	uint32_t netmask,
	uint32_t gateway,
	struct net_device *device);
int
route_add_flags(
	uint32_t network,
	uint32_t netmask,
	uint32_t gateway,
	struct net_device *device,
	unsigned flags);
int
route_delete(
	uint32_t network,
	uint32_t netmask,
	struct net_device *device);
/*
 * Snapshot results own a device reference until route_release().
 */
int
route_lookup_ref(
	uint32_t destination,
	struct net_route *result);
int
route_get_ref(
	unsigned ordinal,
	struct net_route *result);
void
route_release(
	struct net_route *route);
void
route_purge_device(
	struct net_device *device);
int
route_ioctl(
	unsigned long request,
	uintptr_t argument);

#endif
