/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_NETINET_IN_SYSTM_H
#define LIBC_NETINET_IN_SYSTM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The widths a value has while it is on the network.
 *
 * These date from a time when a host's own integers were not a known size,
 * so a separate name was needed for one that had arrived from outside.  The
 * names survive in the headers that describe packets.
 */
typedef uint16_t n_short;
typedef uint32_t n_long;
typedef uint32_t n_time;

#ifdef __cplusplus
}
#endif

#endif
