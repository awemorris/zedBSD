/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * On-the-wire packet layouts.
 *
 * Every structure here mirrors the byte order and packing of a protocol as it
 * appears on the network, so multi-byte fields are kept as byte arrays and
 * converted explicitly by the code that reads or writes them.
 */

#ifndef KERN_KERN_NET_WIRE_H
#define KERN_KERN_NET_WIRE_H

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

/* The IPv6 header (RFC 8200 section 3). */
struct ipv6_wire {
	uint8_t version_class_flow[4];
	uint8_t payload_length[2];
	uint8_t next_header;
	uint8_t hop_limit;
	uint8_t source[16];
	uint8_t destination[16];
} __attribute__((packed));

/* An extension header's start: the next header and the length in 8-byte units after the first eight. */
struct ipv6_extension_wire {
	uint8_t next_header;
	uint8_t length;
} __attribute__((packed));

/* The fragment header: the next header, the offset in 8-byte units with the more-fragments bit, and the identification. */
struct ipv6_fragment_wire {
	uint8_t next_header;
	uint8_t reserved;
	uint8_t offset_flags[2];
	uint8_t identification[4];
} __attribute__((packed));

/* An ICMPv6 message's start, and the four bytes after it (an echo's identifier and sequence, an error's pointer or MTU). */
struct icmp6_wire {
	uint8_t type;
	uint8_t code;
	uint8_t checksum[2];
	uint8_t data[4];
} __attribute__((packed));

/* A Router Solicitation (RFC 4861 section 4.1). */
struct nd6_rs_wire {
	struct icmp6_wire header;
} __attribute__((packed));

/* A Router Advertisement (section 4.2): the ICMPv6 header's data is the hop limit, the flags and the router lifetime. */
struct nd6_ra_wire {
	struct icmp6_wire header;
	uint8_t reachable_time[4];
	uint8_t retrans_timer[4];
} __attribute__((packed));

/* A Neighbor Solicitation or Advertisement (sections 4.3 and 4.4): the data is reserved, or the R, S and O flags. */
struct nd6_neighbor_wire {
	struct icmp6_wire header;
	uint8_t target[16];
} __attribute__((packed));

/* A Redirect (section 4.5). */
struct nd6_redirect_wire {
	struct icmp6_wire header;
	uint8_t target[16];
	uint8_t destination[16];
} __attribute__((packed));

/* A neighbor discovery option's start: its type and length in 8-byte units. */
struct nd6_option_wire {
	uint8_t type;
	uint8_t length;
} __attribute__((packed));

/* The MTU option (type 5). */
struct nd6_option_mtu_wire {
	uint8_t type;
	uint8_t length;
	uint8_t reserved[2];
	uint8_t mtu[4];
} __attribute__((packed));

/* An MLDv2 report (RFC 3810 section 5.2): the ICMPv6 header's data is reserved and the record count. */
struct mld6_report_wire {
	struct icmp6_wire header;
} __attribute__((packed));

/* One record of an MLDv2 report, without sources. */
struct mld6_record_wire {
	uint8_t type;
	uint8_t auxiliary;
	uint8_t sources[2];
	uint8_t group[16];
} __attribute__((packed));

/* An MLD query (RFC 3810 section 5.1, and MLDv1's): the data is the maximum response code and reserved. */
struct mld6_query_wire {
	struct icmp6_wire header;
	uint8_t group[16];
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
