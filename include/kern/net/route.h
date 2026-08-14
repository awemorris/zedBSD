/*
 * Route
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
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
};

void route_init(void);
int route_add(uint32_t network, uint32_t netmask, uint32_t gateway,
	      struct net_device *device);
int route_delete(uint32_t network, uint32_t netmask, struct net_device *device);
const struct net_route *route_lookup(uint32_t destination);

#endif
