/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The internet sockets: IPv4, and IPv6 (ws130-p003) on the same record.
 */

#ifndef KERN_KERN_NET_INET_SOCKET_H
#define KERN_KERN_NET_INET_SOCKET_H

#include "kern/net/socket.h"

#include <uapi/netinet.h>
#include <stdint.h>

struct net_device;

#define INET_SOCKET_BOUND	0x0001U
#define INET_SOCKET_CONNECTED	0x0002U
#define INET_SOCKET_BROADCAST	0x0004U
/* An IPv6 socket's local6 was chosen for its peer by a connect, not bound: the next connect chooses again. */
#define INET_SOCKET_SOURCE_CHOSEN	0x0008U

/*
 * One internet socket of UDP, TCP or ICMP.
 *
 * An AF_INET socket's endpoints are local_address and remote_address.  An
 * AF_INET6 socket (ws130-p003) has its IPv6 endpoints in local6 and
 * remote6, the interface of a link-local peer in scope6, and v6only
 * (IPV6_V6ONLY, 0 by default: a socket on [::] takes IPv4 too).  One that
 * is bound or connected to an IPv4-mapped address (mapped) speaks IPv4:
 * its IPv4 endpoints are then in local_address and remote_address, and it
 * goes the IPv4 way.  A socket on [::] that has neither is open to both,
 * with local_address 0 for IPv4.
 */
struct inet_socket {
	struct socket socket;
	uint32_t local_address;
	uint32_t remote_address;
	uint16_t local_port;
	uint16_t remote_port;
	unsigned ifindex;
	unsigned inet_flags;
	unsigned bind_reuse_address;
	int family;
	struct in6_addr local6;
	struct in6_addr remote6;
	unsigned scope6;
	unsigned v6only;
	unsigned mapped;
};

int
inet_socket_init(void);
void
inet_socket_object_init(
	struct inet_socket *inet,
	int type,
	int protocol,
	const struct socket_ops *ops);
int
inet_socket_bind(
	struct inet_socket *inet,
	const struct sockaddr *address,
	socklen_t length);
int
inet_socket_connect(
	struct inet_socket *inet,
	const struct sockaddr *address,
	socklen_t length);
int
inet_socket_local_conflict(
	const struct inet_socket *existing,
	unsigned existing_reuse,
	const struct inet_socket *candidate,
	unsigned candidate_reuse);
int
inet_socket_getsockname(
	struct inet_socket *inet,
	struct sockaddr *address,
	socklen_t *length);
int
inet_socket_getpeername(
	struct inet_socket *inet,
	struct sockaddr *address,
	socklen_t *length);
int
inet_socket_accepts_ipv4(
	const struct inet_socket *inet);
int
inet_socket_accepts_ipv6(
	const struct inet_socket *inet);
int
inet_socket_speaks_ipv6(
	const struct inet_socket *inet);
uint32_t
inet_socket_unmapped(
	const struct in6_addr *address);
void
inet_socket_peer_name(
	const struct inet_socket *inet,
	uint32_t address,
	const struct in6_addr *address6,
	unsigned scope,
	uint16_t port,
	uint8_t *output,
	uint8_t *length);
int
inet_socket_ioctl(
	struct socket *socket,
	unsigned long command,
	uintptr_t argument);
int
inet_socket_setsockopt(
	struct inet_socket *inet,
	int level,
	int option,
	const void *value,
	socklen_t length);
int
inet_socket_getsockopt(
	struct inet_socket *inet,
	int level,
	int option,
	void *value,
	socklen_t *length);

int
inet_interface_address(
	struct net_device *device,
	uint32_t *address,
	uint32_t *netmask,
	uint32_t *broadcast);
int
inet_interface_configuration(
	struct net_device *device,
	uint32_t *address,
	uint32_t *netmask,
	uint32_t *broadcast);

#endif
