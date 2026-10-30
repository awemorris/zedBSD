/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Packet buffer
 */

#ifndef KERN_KERN_NET_PACKET_BUF_H
#define KERN_KERN_NET_PACKET_BUF_H

#include <kern/atomic.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The packets of the whole stack, in two classes of one pool: standard
 * packets of PACKET_BUF_STORAGE_SIZE, and on the 64-bit machines a few
 * jumbo packets large enough for a 9000-byte frame with its headroom.  A
 * 64-bit machine's TCP offers a window of 128 KB to start with, a packet
 * for each kilobyte of it, which grows by itself to a megabyte while the
 * receive queues together take no more than a quarter of the pool, and
 * keeps a copy of each segment it sends until the peer has it (768 of them
 * a connection): 4096 packets, about 9 MB, leave the rest to the senders'
 * copies and the frames that arrive while one connection has the largest
 * window and another sends.  The 32-bit boards keep the small pool their
 * memory map was laid out for and have no jumbo packets.
 *
 * PACKET_BUF_JUMBO_RESERVE jumbo packets are kept for the drivers'
 * receive (PACKET_BUF_ALLOC_RECEIVE): a sender or a copy that would take
 * one of the last of them is refused, so the frames that arrive still
 * find a packet while sockets hold the others.
 *
 * In the same way PACKET_BUF_HOLD_RESERVE standard packets are kept from
 * a caller that holds its packet for as long as a peer takes to answer
 * (PACKET_BUF_ALLOC_HOLD: TCP's copies for retransmission): the senders to
 * slow peers together take no more than the rest, and the frames that
 * arrive, the receive queues and the packets that only pass through
 * (acknowledgements, datagrams, resends) still find one.
 * CONFIG_PACKET_BUF_POOL_COUNT (an unsigned literal) sets another count of
 * standard packets on the 64-bit machines, for a build that holds two
 * hosts' packets in one pool.
 */
#if defined(HAL_ARCH_AMD64) || defined(HAL_ARCH_ARM64) || \
    defined(HAL_ARCH_RISCV64)
#if defined(CONFIG_PACKET_BUF_POOL_COUNT)
#define PACKET_BUF_POOL_COUNT		CONFIG_PACKET_BUF_POOL_COUNT
#else
#define PACKET_BUF_POOL_COUNT		4096U
#endif
#define PACKET_BUF_JUMBO_STORAGE_SIZE	9216U
#define PACKET_BUF_JUMBO_COUNT		32U
#define PACKET_BUF_JUMBO_RESERVE	8U
#define PACKET_BUF_HOLD_RESERVE		256U
#else
#define PACKET_BUF_POOL_COUNT		32U
#define PACKET_BUF_JUMBO_STORAGE_SIZE	2048U
#define PACKET_BUF_JUMBO_COUNT		0U
#define PACKET_BUF_JUMBO_RESERVE	0U
#define PACKET_BUF_HOLD_RESERVE		8U
#endif
#define PACKET_BUF_STORAGE_SIZE	2048U

/* The largest storage any packet has: what one frame and its headroom may take. */
#define PACKET_BUF_MAX_STORAGE_SIZE	PACKET_BUF_JUMBO_STORAGE_SIZE

/* packet_buf_alloc_for: the caller is a driver's receive, which may take the reserved jumbo packets. */
#define PACKET_BUF_ALLOC_RECEIVE	0x1U
/* packet_buf_alloc_for: the caller holds the packet until a peer answers, and leaves the reserved standard packets. */
#define PACKET_BUF_ALLOC_HOLD		0x2U
#define PACKET_BUF_DEFAULT_HEADROOM	64U
#define PACKET_OFFSET_NONE	UINT16_MAX

/*
 * The checksums a device computes or checked, in a packet's flags.
 *
 * On the way out the protocol sets them only for a device whose
 * capabilities say it computes them (NET_DEVICE_CAP_TX_CSUM_IPV4_TCP):
 * PACKET_BUF_TX_CSUM_TCP, the device completes the TCP checksum, whose
 * field holds the folded sum of the pseudo-header, not complemented
 * (net_checksum_pseudo_partial); PACKET_BUF_TX_CSUM_IPV4, the device fills
 * the IPv4 header's checksum, whose field is zero.  l3_offset and
 * l4_offset name the headers.  The device's own copy of the frame is what
 * the checksum goes into; the packet keeps the partial one.
 *
 * On the way in the driver sets them for what the device checked and found
 * right: PACKET_BUF_RX_CSUM_IPV4, the IPv4 header's checksum;
 * PACKET_BUF_RX_CSUM_L4, the TCP or UDP checksum with its pseudo-header,
 * over IPv4 or IPv6.  A checksum the device found wrong, or did not check,
 * is not marked, and the stack checks it itself.
 */
#define PACKET_BUF_TX_CSUM_TCP		0x00000001U
#define PACKET_BUF_TX_CSUM_IPV4		0x00000002U
#define PACKET_BUF_RX_CSUM_IPV4		0x00000004U
#define PACKET_BUF_RX_CSUM_L4		0x00000008U

struct net_device;

/*
 * The packet pool's use at one moment (packet_buf_pool_usage): for the
 * standard packets and then the jumbo ones, the class's size, the packets
 * taken now and the most ever taken, and how many allocations found none
 * free to them (outside the reserve the caller may not take), and the jumbo
 * packets kept for the drivers' receive.
 */
struct packet_buf_pool_usage {
	unsigned total;
	unsigned used;
	unsigned used_max;
	uint64_t alloc_failures;
	unsigned jumbo_total;
	unsigned jumbo_used;
	unsigned jumbo_used_max;
	unsigned jumbo_reserve;
	uint64_t jumbo_alloc_failures;
};

struct packet_buf {
	uint8_t *storage;
	uint8_t *data;
	size_t capacity;
	size_t length;
	uint16_t l2_offset;
	uint16_t l3_offset;
	uint16_t l4_offset;
	uint16_t l3_length;
	uint16_t protocol;
	uint32_t flags;
	struct net_device *device;
	struct packet_buf *next;
	refcount_t refcount;
	/*
	 * Large enough for sockaddr_storage.  Network drivers may use only the

	 * * leading bytes; AF_UNIX datagrams retain their complete source name.
	 */
	uint8_t source_address[128];
	uint8_t source_length;
	/*
	 * The hop limit an IPv6 datagram came with, kept on the copy queued
	 * for a socket that asked for it (IPV6_RECVHOPLIMIT, ws177-p044);
	 * hop_limit_known says it was kept.
	 */
	uint8_t hop_limit;
	uint8_t hop_limit_known;
	void *control;
	void (
		*control_release)(
		void *);
};

void
packet_buf_pool_init(void);
void
packet_buf_checksum_finish(
	struct packet_buf *packet);
struct packet_buf *
packet_buf_alloc(
	size_t headroom);
struct packet_buf *
packet_buf_alloc_for(
	size_t headroom,
	size_t length,
	unsigned flags,
	int *error);
void
packet_buf_ref(
	struct packet_buf *packet);
void
packet_buf_free(
	struct packet_buf *packet);
size_t
packet_buf_headroom(
	const struct packet_buf *packet);
size_t
packet_buf_tailroom(
	const struct packet_buf *packet);
void *
packet_buf_push(
	struct packet_buf *packet,
	size_t length);
void *
packet_buf_pull(
	struct packet_buf *packet,
	size_t length);
void *
packet_buf_append(
	struct packet_buf *packet,
	size_t length);
int
packet_buf_trim(
	struct packet_buf *packet,
	size_t length);
struct packet_buf *
packet_buf_copy(
	const struct packet_buf *packet);
struct packet_buf *
packet_buf_copy_region(
	const struct packet_buf *packet,
	size_t offset,
	size_t length);
unsigned
packet_buf_in_use(void);
void
packet_buf_pool_usage(
	struct packet_buf_pool_usage *usage);
/*
 * The physical address of a packet's data (0 and the address, or ENOENT
 * when it is not known), for a driver that hands the bytes to its device
 * in place; the packet's storage is physically contiguous.
 */
int
packet_buf_physical(
	const struct packet_buf *packet,
	uint64_t *physical);

#endif
