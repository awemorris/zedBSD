/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the zedBSD userland netutil interface.
 */

#ifndef ZEDBSD_USER_NETUTIL_H
#define ZEDBSD_USER_NETUTIL_H

#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>

int netutil_ifreq(struct ifreq *, const char *);
int netutil_ifindex(int, const char *, uint32_t *);
int netutil_ifname(int, uint32_t, char *);
int netutil_interfaces(int, struct ifreq **, unsigned *);
int netutil_parse_ipv4(const char *, struct in_addr *);
int netutil_parse_cidr(const char *, struct in_addr *, struct in_addr *,
		       unsigned *);
int netutil_mask_prefix(struct in_addr, unsigned *);
uint64_t netutil_monotonic_us(void);
int netutil_parse_milliseconds(const char *, uint32_t *);

#endif
