/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of the host model of TCP (ws220-p001) that shim.c and model.c
 * share: the two hosts' addresses, the clock, and the link.
 */

#ifndef WS220_TCP_MODEL_H
#define WS220_TCP_MODEL_H

#include <stddef.h>
#include <stdint.h>

struct packet_buf;
struct net_device;

/* The rate of the kernel's tick in the model, ticks a second (amd64's). */
#define MODEL_TICK_HZ 1000U

/* Host A (the sender of the transfer) and host B (its receiver), in host order. */
#define MODEL_ADDRESS_A 0x0a000001U
#define MODEL_ADDRESS_B 0x0a000002U

void model_kernel_init(uint64_t tick_base);
void model_clock_set(uint64_t now_us);
uint64_t model_clock_us(void);
int model_deliver(const void *data, size_t length, uint32_t source, uint32_t destination);
struct net_device *model_device(unsigned host);
void model_device_mtu_set(unsigned mtu);
int model_link_output(unsigned host, struct packet_buf *packet, uint32_t source, uint32_t destination);

/* Converts a 32-bit value between network and host order (the host is little-endian). */
static __inline uint32_t
model_ntohl(
	uint32_t value)
{
	return ((value & 0xffU) << 24) | ((value & 0xff00U) << 8) | ((value >> 8) & 0xff00U) | (value >> 24);
}

/* Converts a 16-bit value between network and host order (the host is little-endian). */
static __inline uint16_t
model_ntohs(
	uint16_t value)
{
	return (uint16_t)(((value & 0xffU) << 8) | (value >> 8));
}

#endif
