/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the zedBSD userland dhcp interface.
 */

#ifndef ZEDBSD_USER_DHCP_H
#define ZEDBSD_USER_DHCP_H

#include <stddef.h>
#include <stdint.h>

#define DHCP_DISCOVER 1U
#define DHCP_OFFER 2U
#define DHCP_REQUEST 3U
#define DHCP_ACK 5U
#define DHCP_NAK 6U

struct dhcp_lease {
	uint32_t address;
	uint32_t netmask;
	uint32_t broadcast;
	uint32_t routers[4];
	unsigned router_count;
	uint32_t dns_servers[3];
	unsigned dns_count;
	uint32_t server_identifier;
	uint32_t lease_time;
	uint32_t renewal_time;
	uint32_t rebinding_time;
	uint8_t message_type;
};

int dhcp_build(uint8_t *, size_t, size_t *, uint8_t, uint32_t, const uint8_t[6],
	       uint32_t, uint32_t);
int dhcp_parse(const uint8_t *, size_t, uint32_t, const uint8_t[6],
	       struct dhcp_lease *);

#endif
