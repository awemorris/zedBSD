/*
 * Packet buffer
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_NET_PACKET_BUF_H
#define ZEDBSD_KERN_NET_PACKET_BUF_H

#include <stddef.h>
#include <stdint.h>

#define PACKET_BUF_POOL_COUNT       32U
#define PACKET_BUF_STORAGE_SIZE     2048U
#define PACKET_BUF_DEFAULT_HEADROOM 64U
#define PACKET_OFFSET_NONE          UINT16_MAX

struct net_device;

struct packet_buf {
	uint8_t *storage;
	uint8_t *data;
	size_t capacity;
	size_t length;
	uint16_t l2_offset;
	uint16_t l3_offset;
	uint16_t l4_offset;
	uint16_t protocol;
	uint32_t flags;
	struct net_device *device;
	struct packet_buf *next;
	unsigned refcount;
	uint8_t source_address[32];
	uint8_t source_length;
};

void packet_buf_pool_init(void);
struct packet_buf *packet_buf_alloc(size_t headroom);
void packet_buf_ref(struct packet_buf *packet);
void packet_buf_free(struct packet_buf *packet);
size_t packet_buf_headroom(const struct packet_buf *packet);
size_t packet_buf_tailroom(const struct packet_buf *packet);
void *packet_buf_push(struct packet_buf *packet, size_t length);
void *packet_buf_pull(struct packet_buf *packet, size_t length);
void *packet_buf_append(struct packet_buf *packet, size_t length);
int packet_buf_trim(struct packet_buf *packet, size_t length);
struct packet_buf *packet_buf_copy(const struct packet_buf *packet);
unsigned packet_buf_in_use(void);

#endif
