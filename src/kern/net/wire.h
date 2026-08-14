/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_NET_WIRE_H
#define ZEDBSD_KERN_NET_WIRE_H

#include <stdint.h>

struct arp_wire {
	uint8_t hardware_type[2];
	uint8_t protocol_type[2];
	uint8_t hardware_length;
	uint8_t protocol_length;
	uint8_t operation[2];
	uint8_t sender_hardware[6];
	uint8_t sender_protocol[4];
	uint8_t target_hardware[6];
	uint8_t target_protocol[4];
} __attribute__((packed));

struct ipv4_wire {
	uint8_t version_ihl;
	uint8_t tos;
	uint8_t total_length[2];
	uint8_t identification[2];
	uint8_t fragment[2];
	uint8_t ttl;
	uint8_t protocol;
	uint8_t checksum[2];
	uint8_t source[4];
	uint8_t destination[4];
} __attribute__((packed));

struct icmp_wire {
	uint8_t type;
	uint8_t code;
	uint8_t checksum[2];
} __attribute__((packed));

struct udp_wire {
	uint8_t source[2];
	uint8_t destination[2];
	uint8_t length[2];
	uint8_t checksum[2];
} __attribute__((packed));

struct tcp_wire {
	uint8_t source[2];
	uint8_t destination[2];
	uint8_t sequence[4];
	uint8_t acknowledgement[4];
	uint8_t data_offset;
	uint8_t flags;
	uint8_t window[2];
	uint8_t checksum[2];
	uint8_t urgent[2];
} __attribute__((packed));

static inline uint16_t wire_get16(const uint8_t value[2])
{
	return (uint16_t)((uint16_t)value[0] << 8) | value[1];
}

static inline uint32_t wire_get32(const uint8_t value[4])
{
	return (uint32_t)value[0] << 24 | (uint32_t)value[1] << 16 |
	    (uint32_t)value[2] << 8 | value[3];
}

static inline void wire_put16(uint8_t value[2], uint16_t number)
{
	value[0] = (uint8_t)(number >> 8);
	value[1] = (uint8_t)number;
}

static inline void wire_put32(uint8_t value[4], uint32_t number)
{
	value[0] = (uint8_t)(number >> 24);
	value[1] = (uint8_t)(number >> 16);
	value[2] = (uint8_t)(number >> 8);
	value[3] = (uint8_t)number;
}

#endif
