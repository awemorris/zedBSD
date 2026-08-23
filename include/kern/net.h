/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Network stack
 */

#ifndef ZEDBSD_KERN_NET_H
#define ZEDBSD_KERN_NET_H

#include <stddef.h>
#include <stdint.h>

struct net_stats {
	uint64_t input_packets;
	uint64_t input_dropped;
	uint64_t input_errors;
};

int
net_init(void);

void
net_shutdown_for_boot(void);

void
net_get_stats(
	struct net_stats *stats);

#endif
