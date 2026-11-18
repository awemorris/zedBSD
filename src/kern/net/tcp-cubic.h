/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arithmetic of CUBIC's window function (RFC 9438 section 4), apart
 * from TCP's state so that it can be checked on its own: the time K the
 * function takes to reach the window before the loss, and the window the
 * function gives at a time.  Windows are in bytes and times in
 * milliseconds; everything is integer.
 */

#ifndef KERN_KERN_NET_TCP_CUBIC_H
#define KERN_KERN_NET_TCP_CUBIC_H

#include <stdint.h>

/* The largest window the function gives (TCP_CWND_MAX in tcp.c). */
#define TCP_CUBIC_WINDOW_MAX 0x40000000U

/* The longest distance from K the function is evaluated at, in milliseconds (its cube fits 64 bits). */
#define TCP_CUBIC_DISTANCE_MAX (1U << 20)

uint32_t
tcp_cubic_root(
	uint64_t value);

uint32_t
tcp_cubic_k_ms(
	uint32_t w_max,
	uint32_t cwnd,
	uint32_t mss);

uint32_t
tcp_cubic_window(
	uint32_t origin,
	uint32_t k_ms,
	uint64_t t_ms,
	uint32_t mss);

#endif
