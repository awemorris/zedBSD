/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arithmetic of CUBIC's window function (RFC 9438 section 4).
 *
 * W_cubic(t) = C (t - K)^3 + W_max with C = 0.4 segments a second cubed,
 * taken in bytes and milliseconds: (t - K)^3 in ms^3 is 1e9 times the
 * cube in s^3, so the term is (t - K)^3 / 2.5e9 segments, and K is the
 * cube root of the shortfall in segments times 2.5e9.  The products are
 * formed before the divisions, within 64 bits for every window up to
 * TCP_CUBIC_WINDOW_MAX and every distance up to TCP_CUBIC_DISTANCE_MAX.
 */

#include "tcp-cubic.h"

/*
 * Gives the integer cube root of a value, rounded down: the largest r
 * whose cube is no more than it, found by halving the range.  The root of
 * any 64-bit value is at most 2642245, whose cube still fits, so no
 * candidate's cube overflows.
 */
uint32_t
tcp_cubic_root(
	uint64_t value)
{
	uint64_t low;
	uint64_t high;
	uint64_t middle;
	uint64_t cube;

	/* The root lies in [low, high): 0 cubed is no more than any value, 2642246 cubed is more than all. */
	low = 0;
	high = 2642246U;

	/* Halves the range until it holds one candidate. */
	while (high - low > 1U) {
		middle = low + (high - low) / 2U;
		cube = middle * middle * middle;
		if (cube <= value)
			low = middle;
		else
			high = middle;
	}

	/* Succeeded: the largest root whose cube fits. */
	return (uint32_t)low;
}

/*
 * Gives K (RFC 9438 section 4.2), in milliseconds: the time the window
 * function takes to come back from cwnd to w_max, the cube root of the
 * shortfall in segments over C; 0 when cwnd is not short of w_max.
 */
uint32_t
tcp_cubic_k_ms(
	uint32_t w_max,
	uint32_t cwnd,
	uint32_t mss)
{
	uint64_t scaled;
	uint32_t root;

	/* No shortfall, or no segment size to count it in. */
	if (cwnd >= w_max || mss == 0)
		return 0;

	/*
	 * Value referenced:
	 * RFC 9438 section 5.1 (C = 0.4): 1 / C = 2.5, and 1e9 ms^3 to the
	 * s^3, so the shortfall in bytes times 2.5e9 over the segment size.
	 */
	scaled = (uint64_t)(w_max - cwnd) * 2500000000ULL / mss;

	/* Succeeded: its cube root. */
	root = tcp_cubic_root(scaled);
	return root;
}

/*
 * Gives W_cubic(t) (RFC 9438 section 4.2) in bytes: origin less the cubed
 * term before K, origin with it after, never below 0 or above
 * TCP_CUBIC_WINDOW_MAX.  t_ms is the time since the epoch began.
 */
uint32_t
tcp_cubic_window(
	uint32_t origin,
	uint32_t k_ms,
	uint64_t t_ms,
	uint32_t mss)
{
	uint64_t distance;
	uint64_t cube;
	uint64_t milli_segments;
	uint64_t offset;
	uint64_t window;
	int before;

	/* How far from K, and on which side, no farther than the cube fits. */
	before = 0;
	if (t_ms < k_ms)
		before = 1;
	if (before)
		distance = k_ms - t_ms;
	else
		distance = t_ms - k_ms;
	if (distance > TCP_CUBIC_DISTANCE_MAX)
		distance = TCP_CUBIC_DISTANCE_MAX;

	/*
	 * Value referenced:
	 * RFC 9438 section 5.1 (C = 0.4): the cube in ms^3 over 2.5e9 is
	 * segments; over 2.5e6 it is thousandths of a segment, which the
	 * segment size turns into bytes.
	 */
	cube = distance * distance * distance;
	milli_segments = cube / 2500000U;
	offset = milli_segments * mss / 1000U;

	/* Before K the term is taken off the origin, never below nothing. */
	if (before) {
		if (offset >= origin)
			return 0;
		window = origin - offset;
		return (uint32_t)window;
	}

	/* After K it is added, within the largest window. */
	window = (uint64_t)origin + offset;
	if (window > TCP_CUBIC_WINDOW_MAX)
		window = TCP_CUBIC_WINDOW_MAX;

	/* Succeeded: the window at t. */
	return (uint32_t)window;
}
