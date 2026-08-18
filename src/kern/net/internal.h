/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_NET_INTERNAL_H
#define ZEDBSD_KERN_NET_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

struct net_device;
struct packet_buf;
struct socket;

void net_worker_wakeup(void);

typedef int (*ipv4_input_fn)(struct packet_buf *, uint32_t source,
			     uint32_t destination);

uint16_t net_checksum(const void *, size_t);
uint16_t net_checksum_pseudo(uint32_t source, uint32_t destination,
			     uint8_t protocol, const void *, size_t);

int arp_init(void);
int arp_resolve(struct net_device *, uint32_t address, uint8_t hardware[6]);
int arp_resolve_wait(struct net_device *, uint32_t address,
		     uint8_t hardware[6]);
int arp_request(struct net_device *, uint32_t address);

int ipv4_init(void);
int ipv4_protocol_register(uint8_t protocol, ipv4_input_fn input);
int ipv4_output(struct net_device *, uint32_t destination, uint8_t protocol,
		struct packet_buf *);
int ipv4_output_wait(struct net_device *, uint32_t destination,
		     uint8_t protocol, struct packet_buf *);
int ipv4_output_source(struct net_device *, uint32_t destination,
		       uint8_t protocol, uint32_t source,
		       struct packet_buf *);

int icmp_init(void);
int icmp_socket_create(int protocol, struct socket **result);
int udp_init(void);
int udp_socket_create(int protocol, struct socket **result);
int tcp_init(void);
int tcp_socket_create(int protocol, struct socket **result);
void tcp_timer_run(void);
uint64_t tcp_timer_next_deadline(void);

#endif
