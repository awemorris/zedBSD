/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * netinet
 */

#ifndef KERN_UAPI_NETINET_H
#define KERN_UAPI_NETINET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <uapi/socket.h>
#include <stdint.h>

#define IPPROTO_IP		0
#define IPPROTO_ICMP		1
#define IPPROTO_TCP		6
#define IPPROTO_UDP		17
#define IPPROTO_IPV6		41

/*
 * The second family's own numbers (ws130): its extension headers and its
 * control messages.  Hop-by-hop options share number 0 with IPPROTO_IP.
 */
#define IPPROTO_HOPOPTS		0
#define IPPROTO_ROUTING		43
#define IPPROTO_FRAGMENT	44
#define IPPROTO_ICMPV6		58
#define IPPROTO_NONE		59
#define IPPROTO_DSTOPTS		60

/*
 * Options at IPPROTO_TCP.  TCP_NODELAY is the one POSIX names.
 * TCP_MAXUNACKTIME (an int, seconds; 0, the default, turns it off) drops a
 * connection whose sent data has made no progress for that long, with a
 * reset (ws220-p005; FreeBSD's option of the same name and number).
 */
#define TCP_NODELAY		1
#define TCP_MAXUNACKTIME	68

/*
 * TCP_CONGESTION (FreeBSD's name and number) names a connection's
 * congestion control: "reno" (RFC 5681) or "cubic" (RFC 9438), at most
 * TCP_CA_NAME_MAX - 1 bytes, a NUL after it not needed.  Setting a name
 * this kernel does not have fails with ESRCH.  Reading gives the name and
 * its NUL, cut to the length given.
 */
#define TCP_CONGESTION		64
#define TCP_CA_NAME_MAX		16

/*
 * The idle probes SO_KEEPALIVE turns on (RFC 1122 section 4.2.3.6), each an
 * int, with FreeBSD's names and numbers (Linux has the same names):
 * TCP_KEEPIDLE is the seconds a connection is silent before the first
 * probe, TCP_KEEPINTVL the seconds between probes, and TCP_KEEPCNT the
 * probes that may go unanswered before the connection is dropped.  Each is
 * at least 1 and at most TCP_KEEP_MAX; a new socket starts from the
 * system's values, and one that accept() gives takes its listener's.
 */
#define TCP_KEEPIDLE		256
#define TCP_KEEPINTVL		512
#define TCP_KEEPCNT		1024
#define TCP_KEEP_MAX		2147483

/*
 * Options at IPPROTO_IP, with FreeBSD's names and numbers.  Both are an
 * int, and both are taken by AF_INET and AF_INET6 sockets (the latter for
 * what they send and receive as IPv4).
 *
 * IP_TOS is the Type of Service byte of the IPv4 packets the socket sends:
 * the DSCP (RFC 2474) and the ECN field (RFC 3168).  The low eight bits of
 * the value are kept, as BSD and Linux keep them.  A TCP socket keeps the
 * ECN field clear, since this TCP does not negotiate ECN and a sender that
 * has not may not mark its packets ECN-capable (RFC 3168 section 6.1.1).
 *
 * IP_RECVTOS, set, has recvmsg give the Type of Service byte each IPv4
 * datagram came with: a control message at IPPROTO_IP of type IP_TOS
 * holding one unsigned char.  The type is the one Linux gives (FreeBSD
 * gives IP_RECVTOS), which is the one software that reads both looks for
 * outside Apple's systems.
 */
#define IP_TOS			3
#define IP_RECVTOS		68

#define INADDR_ANY		0x00000000U
#define INADDR_BROADCAST	0xffffffffU
#define INADDR_LOOPBACK		0x7f000001U

/* The network the loopback address belongs to. */
#define IN_LOOPBACKNET		127

/*
 * Ports below this one are for a program that had to be privileged to bind
 * them, which is the whole of what the number means.
 */
#define IPPORT_RESERVED		1024

/*
 * POSIX names for the address and port types carried in struct sockaddr_in.
 * Portable software declares variables with them rather than with uint32_t.
 */
typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

struct in_addr {
	in_addr_t s_addr;
};

struct sockaddr_in {
	sa_family_t sin_family;
	uint16_t sin_port;
	struct in_addr sin_addr;
	uint8_t sin_zero[8];
};

/*
 * The second family's address, and the socket address that carries it.
 * The scope word names the interface of a link-local address.
 */
struct in6_addr {
	union {
		uint8_t __u6_addr8[16];
		uint16_t __u6_addr16[8];
		uint32_t __u6_addr32[4];
	} __u6_addr;
};

#define s6_addr		__u6_addr.__u6_addr8
#define s6_addr16	__u6_addr.__u6_addr16
#define s6_addr32	__u6_addr.__u6_addr32

struct sockaddr_in6 {
	sa_family_t sin6_family;
	uint16_t sin6_port;
	uint32_t sin6_flowinfo;
	struct in6_addr sin6_addr;
	uint32_t sin6_scope_id;
};

/* Joining and leaving a group, for software that names the structure. */
struct ipv6_mreq {
	struct in6_addr ipv6mr_multiaddr;
	unsigned ipv6mr_interface;
};

/* The two addresses every implementation publishes by name. */
extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;

#define IN6ADDR_ANY_INIT	{ { { 0, 0, 0, 0, 0, 0, 0, 0, \
				      0, 0, 0, 0, 0, 0, 0, 0 } } }
#define IN6ADDR_LOOPBACK_INIT	{ { { 0, 0, 0, 0, 0, 0, 0, 0, \
				      0, 0, 0, 0, 0, 0, 0, 1 } } }

/*
 * Options at IPPROTO_IPV6 (ws130; the numbers are FreeBSD's, as the
 * SOL_SOCKET options are).  IPV6_V6ONLY is 0 by default: a socket bound
 * to the unspecified address also takes the first family's peers, as
 * v4-mapped addresses.
 */
#define IPV6_UNICAST_HOPS	4
#define IPV6_MULTICAST_IF	9
#define IPV6_MULTICAST_HOPS	10
#define IPV6_MULTICAST_LOOP	11
#define IPV6_JOIN_GROUP		12
#define IPV6_LEAVE_GROUP	13
#define IPV6_V6ONLY		27
#define IPV6_RECVPKTINFO	36
#define IPV6_RECVHOPLIMIT	37
#define IPV6_PKTINFO		46
#define IPV6_HOPLIMIT		47

/*
 * The traffic class of an IPv6 socket (RFC 3542 section 6.5; FreeBSD's
 * numbers), each an int.  IPV6_TCLASS is the class of the packets it
 * sends, 0 to 255, or -1 for the default, 0.  A TCP socket keeps the ECN
 * field clear, as with IP_TOS.  IPV6_RECVTCLASS, set, has recvmsg give the
 * class each IPv6 datagram came with: a control message at IPPROTO_IPV6 of
 * type IPV6_TCLASS holding an int.
 */
#define IPV6_RECVTCLASS		57
#define IPV6_TCLASS		61

/*
 * The destination address and the interface a datagram came in on: the
 * control message IPV6_PKTINFO that IPV6_RECVPKTINFO asks recvmsg for.
 */
struct in6_pktinfo {
	struct in6_addr ipi6_addr;
	unsigned ipi6_ifindex;
};

/* A lifetime (seconds) that does not run out: an address's, or a route's. */
#define IN6_LIFETIME_INFINITE	0xffffffffU

/*
 * Room for the longest written form of an address of the second family.
 * The first family keeps its own in <arpa/inet.h>, where it already was.
 */
#define INET6_ADDRSTRLEN	46

uint16_t
htons(
	uint16_t value);

uint16_t
ntohs(
	uint16_t value);

uint32_t
htonl(
	uint32_t value);

uint32_t
ntohl(
	uint32_t value);

#ifdef __cplusplus
}
#endif

#endif
