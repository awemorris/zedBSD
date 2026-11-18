/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of the IPv6 probe: the kernel's core (main.c, ws130-p002)
 * the transports (transport.c, ws130-p003) and the C library
 * (libc.c, ws130-p004).
 */

#ifndef IPV6_PROBE_H
#define IPV6_PROBE_H

#include <uapi/netinet.h>

int probe_fail(const char *step, int error);
int probe_transport_local(void);
int probe_transport_router(const struct in6_addr *router, unsigned ifindex, const struct in6_addr *prefix);
int probe_libc(const char *name);

#endif
