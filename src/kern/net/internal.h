/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declarations private to the network implementation.
 *
 * These interfaces connect the protocol files to each other and are not part
 * of the kernel's public network API.  A protocol registers its input
 * handler, hands finished datagrams to the layer below, and wakes the shared
 * network worker through this header.
 */

#ifndef KERN_KERN_NET_INTERNAL_H
#define KERN_KERN_NET_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

struct net_device;
struct packet_buf;
struct socket;

void net_worker_wakeup(void);

struct net_device *
net_loopback_ref(void);

int
inet_address_is_local(
	uint32_t address);

typedef int (*ipv4_input_fn)(struct packet_buf *, uint32_t source, uint32_t destination);

uint16_t
net_checksum(
	const void *,
	size_t);

uint16_t
net_checksum_pseudo(
	uint32_t source,
	uint32_t destination,
	uint8_t protocol,
	const void *,
	size_t);

uint16_t
net_checksum_pseudo_partial(
	uint32_t source,
	uint32_t destination,
	uint8_t protocol,
	size_t length);

uint16_t
net_checksum_pseudo6(
	const uint8_t source[16],
	const uint8_t destination[16],
	uint8_t next_header,
	const void *,
	size_t);

int
arp_init(void);

int
arp_resolve(
	struct net_device *,
	uint32_t address,
	uint8_t hardware[6]);

int
arp_resolve_wait(
	struct net_device *,
	uint32_t address,
	uint8_t hardware[6]);

int
arp_request(
	struct net_device *,
	uint32_t address);

void
arp_purge_device(
	struct net_device *);

int
ipv4_init(void);

int
ipv4_protocol_register(
	uint8_t protocol,
	ipv4_input_fn input);

int
ipv4_output(
	struct net_device *,
	uint32_t destination,
	uint8_t protocol,
	struct packet_buf *);

int
ipv4_output_routed(
	struct net_device *,
	uint32_t destination,
	uint32_t next_hop,
	uint8_t protocol,
	uint32_t source,
	struct packet_buf *);

int
ipv4_output_sourced(
	struct net_device *,
	uint32_t destination,
	uint8_t protocol,
	uint32_t source,
	struct packet_buf *);

int
ipv4_output_wait(
	struct net_device *,
	uint32_t destination,
	uint8_t protocol,
	struct packet_buf *);

int
ipv4_output_source(
	struct net_device *,
	uint32_t destination,
	uint8_t protocol,
	uint32_t source,
	struct packet_buf *);

int
ipv4_output_from(
	struct net_device *,
	uint32_t destination,
	uint8_t protocol,
	uint32_t source,
	struct packet_buf *);

int
icmp_init(void);

int
icmp_socket_create(
	int protocol,
	struct socket **result);

int
icmp6_socket_create(
	int protocol,
	struct socket **result);

int
udp_init(void);

int
udp_socket_create(
	int protocol,
	struct socket **result);

int
tcp_init(void);

int
tcp_socket_create(
	int protocol,
	struct socket **result);

void
tcp_timer_run(void);

/*
 * Sends the acknowledgements TCP delayed for the end of a batch of input
 * (each connection that took a single segment since it last sent): the
 * network worker calls it when it has taken every packet that had arrived.
 */
void
tcp_ack_flush(void);

uint64_t
tcp_timer_next_deadline(void);

#endif
