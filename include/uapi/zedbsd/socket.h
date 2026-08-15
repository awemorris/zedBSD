/*
 * socket
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_UAPI_SOCKET_H
#define ZEDBSD_UAPI_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/types.h>

typedef uint16_t sa_family_t;
typedef uint32_t socklen_t;

#define AF_UNSPEC 0
#define AF_INET   2
#define AF_PACKET 17

#define PF_UNSPEC AF_UNSPEC
#define PF_INET   AF_INET
#define PF_PACKET AF_PACKET

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3

#define SOL_SOCKET 0xffff
#define SO_BROADCAST 0x0020
#define SO_BINDTODEVICE 0x0019
#define SO_RCVTIMEO  0x1006

#define MSG_DONTWAIT 0x0040

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define L2_PACKET_HOST      0
#define L2_PACKET_BROADCAST 1
#define L2_PACKET_MULTICAST 2
#define L2_PACKET_OUTGOING  4
#define L2_HARDWARE_ETHER   1

struct sockaddr {
	sa_family_t sa_family;
	char sa_data[14];
};

struct sockaddr_storage {
	sa_family_t ss_family;
	uint8_t __storage[126];
};

struct sockaddr_l2 {
	sa_family_t sl2_family;
	uint16_t sl2_protocol;
	uint32_t sl2_ifindex;
	uint16_t sl2_hatype;
	uint8_t sl2_pkttype;
	uint8_t sl2_halen;
	uint8_t sl2_addr[8];
};

int socket(int domain, int type, int protocol);
int bind(int descriptor, const struct sockaddr *address, socklen_t length);
int connect(int descriptor, const struct sockaddr *address, socklen_t length);
int listen(int descriptor, int backlog);
int accept(int descriptor, struct sockaddr *address, socklen_t *length);
ssize_t send(int descriptor, const void *buffer, size_t length, int flags);
ssize_t sendto(int descriptor, const void *buffer, size_t length, int flags,
	       const struct sockaddr *address, socklen_t address_length);
ssize_t recv(int descriptor, void *buffer, size_t length, int flags);
ssize_t recvfrom(int descriptor, void *buffer, size_t length, int flags,
		 struct sockaddr *address, socklen_t *address_length);
int shutdown(int descriptor, int how);
int getsockname(int descriptor, struct sockaddr *address, socklen_t *length);
int getpeername(int descriptor, struct sockaddr *address, socklen_t *length);
int setsockopt(int descriptor, int level, int option, const void *value,
	       socklen_t length);
int getsockopt(int descriptor, int level, int option, void *value,
	       socklen_t *length);

#endif
