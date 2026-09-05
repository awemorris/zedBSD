/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * socket
 */

#ifndef ZEDBSD_UAPI_SOCKET_H
#define ZEDBSD_UAPI_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <zedbsd/types.h>
#include <sys/time.h>
#include <sys/types.h>

typedef uint16_t sa_family_t;
typedef uint32_t socklen_t;

#define AF_UNSPEC	0
#define AF_UNIX	1
#define AF_INET	2
#define AF_PACKET	17
#define AF_ROUTE	18

#define PF_UNSPEC	AF_UNSPEC
#define PF_UNIX	AF_UNIX
#define PF_INET	AF_INET
#define PF_PACKET	AF_PACKET
#define PF_ROUTE	AF_ROUTE

#define SOCK_STREAM	1
#define SOCK_DGRAM	2
#define SOCK_RAW	3
#define SOCK_NONBLOCK	0x2000
#define SOCK_CLOEXEC	0x4000
#define SOCK_CLOFORK	0x80000

#define SOL_SOCKET	0xffff
#define SO_BROADCAST	0x0020
#define SO_REUSEADDR	0x0004
#define SO_SNDBUF	0x1001
#define SO_RCVBUF	0x1002
#define SO_ERROR	0x1007
#define SO_TYPE	0x1008
#define SO_ATMARK	0x1009
#define SO_SNDTIMEO	0x1005
#define SO_BINDTODEVICE	0x0019
#define SO_RCVTIMEO	0x1006
#define SO_DOMAIN	0x100a
#define SO_PROTOCOL	0x100b
#define SO_PEERCRED	0x0011

/*
 * Immutable connection-time peer identity for connected AF_UNIX streams.
 * This is a zedBSD extension and deliberately does not expose the kernel's
 * internal credential object.
 */
struct zedbsd_peercred {
	int32_t pid;
	uint32_t euid;
	uint32_t egid;
};

_Static_assert(sizeof(struct zedbsd_peercred) == 12U,
    "zedBSD peer credential ABI must remain 12 bytes");
_Static_assert(offsetof(struct zedbsd_peercred, pid) == 0U,
    "zedBSD peer credential pid offset changed");
_Static_assert(offsetof(struct zedbsd_peercred, euid) == 4U,
    "zedBSD peer credential euid offset changed");
_Static_assert(offsetof(struct zedbsd_peercred, egid) == 8U,
    "zedBSD peer credential egid offset changed");
_Static_assert(sizeof(pid_t) == sizeof(int32_t) && (pid_t)-1 < (pid_t)0,
    "native pid_t no longer fits peer credential pid");
_Static_assert(sizeof(uid_t) == sizeof(uint32_t) && (uid_t)-1 > (uid_t)0,
    "native uid_t no longer fits peer credential euid");
_Static_assert(sizeof(gid_t) == sizeof(uint32_t) && (gid_t)-1 > (gid_t)0,
    "native gid_t no longer fits peer credential egid");

#define MSG_DONTWAIT	0x0040
#define MSG_NOSIGNAL	0x4000
#define MSG_PEEK	0x0002
#define MSG_TRUNC	0x0020
#define MSG_CTRUNC	0x0008
#define MSG_WAITALL	0x0100
#define MSG_CMSG_CLOEXEC	0x8000
#define MSG_CMSG_CLOFORK	0x10000

#define SCM_RIGHTS	0x0001

#define ZEDBSD_MSG_FD_MAX	8U

/*
 * Normalized sendmsg/recvmsg ABI.  libc flattens iovecs and validates
 * cmsghdr objects, so the kernel never follows native nested pointers.
 */
struct sendmsg_args {
	uapi_ptr_t data;
	uint64_t data_length;
	uapi_ptr_t name;
	uint32_t name_length;
	uint32_t flags;
	uapi_ptr_t descriptors;
	uint32_t descriptor_count;
	uint32_t reserved;
};

struct recvmsg_args {
	uapi_ptr_t data;
	uint64_t data_capacity;
	uapi_ptr_t name;
	uint32_t name_capacity;
	uint32_t flags;
	uapi_ptr_t descriptors;
	uint32_t descriptor_capacity;
	uint32_t reserved;
	uint64_t data_length;
	uint32_t name_length;
	uint32_t descriptor_count;
	uint32_t output_flags;
	uint32_t reserved2;
};

#define SHUT_RD	0
#define SHUT_WR	1
#define SHUT_RDWR	2

#define L2_PACKET_HOST	0
#define L2_PACKET_BROADCAST	1
#define L2_PACKET_MULTICAST	2
#define L2_PACKET_OUTGOING	4
#define L2_HARDWARE_ETHER	1

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

struct iovec;
struct msghdr {
	void *msg_name;
	socklen_t msg_namelen;
	struct iovec *msg_iov;
	size_t msg_iovlen;
	void *msg_control;
	size_t msg_controllen;
	int msg_flags;
};
struct cmsghdr {
	size_t cmsg_len;
	int cmsg_level;
	int cmsg_type;
};
#define CMSG_ALIGN(n)	(((n) + sizeof(size_t) - 1U) & ~(sizeof(size_t) - 1U))
#define CMSG_DATA(c)	((unsigned char *)(c) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_LEN(n)	(CMSG_ALIGN(sizeof(struct cmsghdr)) + (n))
#define CMSG_SPACE(n)	(CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(n))

int
socket(
	int domain,
	int type,
	int protocol);
int
socketpair(
	int domain,
	int type,
	int protocol,
	int descriptors[2]);
int
bind(
	int descriptor,
	const struct sockaddr *address,
	socklen_t length);
int
connect(
	int descriptor,
	const struct sockaddr *address,
	socklen_t length);
int
listen(
	int descriptor,
	int backlog);
int
accept(
	int descriptor,
	struct sockaddr *address,
	socklen_t *length);
int
accept4(
	int descriptor,
	struct sockaddr *address,
	socklen_t *length,
	int flags);
ssize_t
send(
	int descriptor,
	const void *buffer,
	size_t length,
	int flags);
ssize_t
sendto(
	int descriptor,
	const void *buffer,
	size_t length,
	int flags,
	const struct sockaddr *address,
	socklen_t address_length);
ssize_t
recv(
	int descriptor,
	void *buffer,
	size_t length,
	int flags);
ssize_t
recvfrom(
	int descriptor,
	void *buffer,
	size_t length,
	int flags,
	struct sockaddr *address,
	socklen_t *address_length);
ssize_t
sendmsg(
	int descriptor,
	const struct msghdr *message,
	int flags);
ssize_t
recvmsg(
	int descriptor,
	struct msghdr *message,
	int flags);
int
shutdown(
	int descriptor,
	int how);
int
getsockname(
	int descriptor,
	struct sockaddr *address,
	socklen_t *length);
int
getpeername(
	int descriptor,
	struct sockaddr *address,
	socklen_t *length);
int
setsockopt(
	int descriptor,
	int level,
	int option,
	const void *value,
	socklen_t length);
int
getsockopt(
	int descriptor,
	int level,
	int option,
	void *value,
	socklen_t *length);
int
sockatmark(
	int descriptor);

#endif
