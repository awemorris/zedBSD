/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * IPv4 socket
 */

#ifndef ZEDBSD_KERN_NET_INET_SOCKET_H
#define ZEDBSD_KERN_NET_INET_SOCKET_H

#include "kern/net/socket.h"

#include <stdint.h>

struct net_device;

#define INET_SOCKET_BOUND	0x0001U
#define INET_SOCKET_CONNECTED	0x0002U
#define INET_SOCKET_BROADCAST	0x0004U

struct inet_socket {
	struct socket socket;
	uint32_t local_address;
	uint32_t remote_address;
	uint16_t local_port;
	uint16_t remote_port;
	unsigned ifindex;
	unsigned inet_flags;
	unsigned bind_reuse_address;
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
