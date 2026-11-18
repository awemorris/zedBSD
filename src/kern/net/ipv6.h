/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * IPv6 inside the network stack: the layer (ipv6.c: the interfaces'
 * addresses and link values, the routes, input and output, the timers),
 * the reassembly of fragments (ipv6-frag.c), ICMPv6 (icmp6.c), neighbor
 * discovery (nd6.c) and multicast listener reports (mld6.c).  The tables'
 * pure parts are in in6.h.
 */

#ifndef KERN_NET_IPV6_H
#define KERN_NET_IPV6_H

#include "in6.h"

#include <stddef.h>
#include <stdint.h>

struct net_device;
struct packet_buf;

/* The Ethernet type, the header's size, and the smallest link MTU (RFC 8200 section 5). */
#define ETHERNET_TYPE_IPV6	0x86ddU
#define IPV6_HEADER_LENGTH	40U
#define IPV6_MINIMUM_MTU	1280U

/* The next headers IPv6 itself knows (the same numbers as IPPROTO_*). */
#define IPV6_NEXT_HOPOPTS	0U
#define IPV6_NEXT_ROUTING	43U
#define IPV6_NEXT_FRAGMENT	44U
#define IPV6_NEXT_ICMPV6	58U
#define IPV6_NEXT_NONE		59U
#define IPV6_NEXT_DSTOPTS	60U

/* The hop limit neighbor discovery and MLD send and require, and the default for the rest. */
#define IPV6_HOP_LIMIT_ND	255U
#define IPV6_HOP_LIMIT_MLD	1U
#define IPV6_HOP_LIMIT_DEFAULT	64U

/* The ICMPv6 types (RFC 4443, 4861, 3810). */
#define ICMP6_DESTINATION_UNREACHABLE	1U
#define ICMP6_PACKET_TOO_BIG		2U
#define ICMP6_TIME_EXCEEDED		3U
#define ICMP6_PARAMETER_PROBLEM		4U
#define ICMP6_ECHO_REQUEST		128U
#define ICMP6_ECHO_REPLY		129U
#define ICMP6_MLD_QUERY			130U
#define ICMP6_MLD1_REPORT		131U
#define ICMP6_MLD1_DONE			132U
#define ICMP6_ROUTER_SOLICITATION	133U
#define ICMP6_ROUTER_ADVERTISEMENT	134U
#define ICMP6_NEIGHBOR_SOLICITATION	135U
#define ICMP6_NEIGHBOR_ADVERTISEMENT	136U
#define ICMP6_REDIRECT			137U
#define ICMP6_MLD2_REPORT		143U

/* The Parameter Problem's codes. */
#define ICMP6_PARAMETER_HEADER		0U
#define ICMP6_PARAMETER_NEXT_HEADER	1U
#define ICMP6_PARAMETER_OPTION		2U
#define ICMP6_PARAMETER_FIRST_FRAGMENT	3U

/* The neighbor discovery options (RFC 4861 section 4.6). */
#define ND6_OPTION_SOURCE_LINK		1U
#define ND6_OPTION_TARGET_LINK		2U
#define ND6_OPTION_PREFIX		3U
#define ND6_OPTION_REDIRECTED		4U
#define ND6_OPTION_MTU			5U

/* The Neighbor Advertisement's flags (the first byte of its data). */
#define ND6_ADVERT_ROUTER		0x80U
#define ND6_ADVERT_SOLICITED		0x40U
#define ND6_ADVERT_OVERRIDE		0x20U

/* Router solicitations: how many, and how far apart (RFC 4861 section 10). */
#define ND6_RTR_SOLICITATIONS		3U
#define ND6_RTR_SOLICITATION_MS		4000U

/* How long a redirect's host route and a path MTU learned from Packet Too Big last. */
#define IPV6_REDIRECT_MS		600000U
#define IPV6_PATH_MTU_MS		600000U

/* The input function of a next header (a transport). */
typedef int (*ipv6_input_fn)(struct packet_buf *packet, const struct in6_addr *source, const struct in6_addr *destination);

/*
 * The link values of an interface that neighbor discovery and the
 * output use: the hop limit, the MTU, the reachable time and the
 * retransmission timer.
 */
struct ipv6_link {
	unsigned hop_limit;
	unsigned mtu;
	unsigned reachable_ms;
	unsigned retrans_ms;
};

/*
 * The way a unicast packet to a destination goes, as ipv6_route_way
 * found it: the next hop on the interface, the hop limit, and the path's
 * MTU.  It stands while route_generation() does not move.
 */
struct ipv6_way {
	struct in6_addr next_hop;
	unsigned hop_limit;
	unsigned mtu;
};

/* ipv6.c */
int ipv6_init(void);
int ipv6_protocol_register(uint8_t next_header, ipv6_input_fn input);
int ipv6_output(struct net_device *device, const struct in6_addr *source, const struct in6_addr *destination, uint8_t next_header, unsigned hop_limit, struct packet_buf *packet);
int ipv6_source_select(struct net_device *device, const struct in6_addr *destination, struct in6_addr *source);
int ipv6_route_source(struct net_device *device, const struct in6_addr *destination, struct in6_addr *source, unsigned *mtu);
int ipv6_route_way(struct net_device *device, const struct in6_addr *destination, struct net_device **output, struct ipv6_way *way);
int ipv6_output_way(struct net_device *device, const struct in6_addr *source, const struct in6_addr *destination, const struct ipv6_way *way, uint8_t next_header, struct packet_buf *packet);
int ipv6_address_state(struct net_device *device, const struct in6_addr *address, unsigned *flags);
int ipv6_address_is_local(const struct in6_addr *address);
int ipv6_link_get(struct net_device *device, struct ipv6_link *link);
void ipv6_link_update(struct net_device *device, const struct ipv6_link *values);
void ipv6_device_mtu_changed(struct net_device *device);
void ipv6_duplicate(struct net_device *device, const struct in6_addr *address);
void ipv6_router_advertised(struct net_device *device);
unsigned ipv6_groups(struct net_device *device, struct in6_addr *groups, unsigned capacity);
int ipv6_route_add(const struct in6_route *route);
int ipv6_route_redirect(struct net_device *device, const struct in6_addr *destination, const struct in6_addr *gateway, const struct in6_addr *source);
int ipv6_address_add(struct net_device *device, const struct in6_addr *address, unsigned prefixlen, unsigned flags, uint32_t valid_s, uint32_t preferred_s);
int ipv6_address_remove(struct net_device *device, const struct in6_addr *address);
int ipv6_address_list(struct net_device *device, struct in6_address *entries, unsigned capacity, unsigned *count, int *enabled);
int ipv6_enable(struct net_device *device, int enabled);
int ipv6_route_delete(const struct in6_addr *destination, unsigned prefixlen, unsigned ifindex);
int ipv6_route_get(unsigned ordinal, struct in6_route *route);
int ipv6_ioctl(unsigned long command, uintptr_t argument);
int ipv6_ioctl_is_query(unsigned long command);
int ipv6_ioctl_handles(unsigned long command);
void ipv6_timer_run(void);
uint64_t ipv6_timer_next_deadline(void);
void ipv6_purge_device(struct net_device *device);
uint64_t ipv6_now_ms(void);

/* ipv6-frag.c (ipv6_fragment_input consumes the packet unless it returns 0) */
void ipv6_fragment_init(void);
int ipv6_fragment_input(struct packet_buf **packet, uint8_t *next_header, size_t *offset, size_t *field);
void ipv6_fragment_timer_run(uint64_t now_ms);
uint64_t ipv6_fragment_deadline(void);
void ipv6_fragment_purge_device(struct net_device *device);

/* icmp6.c (icmp6_error consumes the packet; its l3_offset is the invoking packet's header) */
void icmp6_init(void);
int icmp6_input(struct packet_buf *packet, const struct in6_addr *source, const struct in6_addr *destination);
int icmp6_send(struct net_device *device, const struct in6_addr *source, const struct in6_addr *destination, unsigned hop_limit, struct packet_buf *packet);
void icmp6_error(struct packet_buf *packet, uint8_t type, uint8_t code, uint32_t data);
unsigned icmp6_path_mtu(const struct in6_addr *destination, unsigned link_mtu);
void icmp6_path_mtu_purge(void);

/* icmp.c: the raw ICMPv6 sockets, given a copy of every message (packet->data its first byte) */
void icmp6_raw_deliver(struct packet_buf *packet, const struct in6_addr *source, const struct in6_addr *destination);

/* udp.c, tcp.c: an ICMPv6 error about a datagram or segment this host sent (its addresses and ports as sent) */
void udp6_error(const struct in6_addr *source, const struct in6_addr *destination, uint16_t source_port, uint16_t destination_port, int error);
void tcp6_error(const struct in6_addr *source, const struct in6_addr *destination, uint16_t source_port, uint16_t destination_port, int error);

/* tcp.c: a Packet Too Big lowered the MTU of the path to a destination; the connections to it cut their segments to it */
void tcp6_path_mtu_lowered(const struct in6_addr *destination, unsigned mtu);

/* nd6.c */
int nd6_init(void);
int nd6_output(struct net_device *device, const struct in6_addr *next_hop, struct packet_buf *packet);
void nd6_input(struct packet_buf *packet, const struct in6_addr *source, const struct in6_addr *destination, unsigned hop_limit);
int nd6_send_solicitation(struct net_device *device, const struct in6_addr *target, const struct in6_addr *destination, const uint8_t *link, int detection);
int nd6_send_router_solicitation(struct net_device *device);
void nd6_timer_run(uint64_t now_ms);
uint64_t nd6_timer_next_deadline(void);
void nd6_purge_device(struct net_device *device);

/* mld6.c */
void mld6_input(struct packet_buf *packet, const struct in6_addr *source, const struct in6_addr *destination);
int mld6_report(struct net_device *device, const struct in6_addr *groups, unsigned count);

#endif
