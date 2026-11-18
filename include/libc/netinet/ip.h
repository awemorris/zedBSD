/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_NETINET_IP_H
#define LIBC_NETINET_IP_H

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The header of an Internet datagram, as it is carried.
 *
 * The first byte holds the version and the header length together, which is
 * why it is one member here rather than two bit-fields: bit-fields have no
 * defined order, and this one has to match the wire.
 */
struct ip {
	uint8_t ip_vhl;		/* Version in the high nibble, length in the low. */
	uint8_t ip_tos;
	uint16_t ip_len;
	uint16_t ip_id;
	uint16_t ip_off;
	uint8_t ip_ttl;
	uint8_t ip_p;
	uint16_t ip_sum;
	struct in_addr ip_src;
	struct in_addr ip_dst;
};

#define IP_MAXPACKET 65535

/*
 * The type of service a sender asks for.
 *
 * zedBSD carries these no further than the socket option: the network stack
 * does not yet act on them, so setting one is accepted and changes nothing.
 * The names are here because software chooses among them at compile time.
 */
#define IPTOS_LOWDELAY     0x10
#define IPTOS_THROUGHPUT   0x08
#define IPTOS_RELIABILITY  0x04
#define IPTOS_MINCOST      0x02

/* The differentiated services code points, in the upper six bits. */
#define IPTOS_DSCP_CS0  0x00
#define IPTOS_DSCP_CS1  0x20
#define IPTOS_DSCP_CS2  0x40
#define IPTOS_DSCP_CS3  0x60
#define IPTOS_DSCP_CS4  0x80
#define IPTOS_DSCP_CS5  0xa0
#define IPTOS_DSCP_CS6  0xc0
#define IPTOS_DSCP_CS7  0xe0
#define IPTOS_DSCP_AF11 0x28
#define IPTOS_DSCP_AF12 0x30
#define IPTOS_DSCP_AF13 0x38
#define IPTOS_DSCP_AF21 0x48
#define IPTOS_DSCP_AF22 0x50
#define IPTOS_DSCP_AF23 0x58
#define IPTOS_DSCP_AF31 0x68
#define IPTOS_DSCP_AF32 0x70
#define IPTOS_DSCP_AF33 0x78
#define IPTOS_DSCP_AF41 0x88
#define IPTOS_DSCP_AF42 0x90
#define IPTOS_DSCP_AF43 0x98
#define IPTOS_DSCP_EF   0xb8
#define IPTOS_DSCP_LE   0x04

#define IPTOS_PREC_ROUTINE         0x00
#define IPTOS_PREC_PRIORITY        0x20
#define IPTOS_PREC_IMMEDIATE       0x40
#define IPTOS_PREC_FLASH           0x60
#define IPTOS_PREC_FLASHOVERRIDE   0x80
#define IPTOS_PREC_CRITIC_ECP      0xa0
#define IPTOS_PREC_INTERNETCONTROL 0xc0
#define IPTOS_PREC_NETCONTROL      0xe0

#ifdef __cplusplus
}
#endif

#endif
