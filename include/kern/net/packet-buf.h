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
 * The packets of the whole stack, one pool.  A 64-bit machine's TCP offers
 * a window of tens of kilobytes, a packet for each segment of it (BUG-222);
 * the 32-bit boards keep the small pool their memory map was laid out for.
 */
#if defined(HAL_ARCH_AMD64) || defined(HAL_ARCH_ARM64)
#define PACKET_BUF_POOL_COUNT	256U
#else
#define PACKET_BUF_POOL_COUNT	32U
#endif
#define PACKET_BUF_STORAGE_SIZE	2048U
#define PACKET_BUF_DEFAULT_HEADROOM	64U
#define PACKET_OFFSET_NONE	UINT16_MAX

struct net_device;

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
struct packet_buf *
packet_buf_alloc(
	size_t headroom);
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

#endif
