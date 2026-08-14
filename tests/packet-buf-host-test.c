/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/packet-buf.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
zedbsd_assert_fail(const char *expression, const char *file, int line)
{
	fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line, expression);
	abort();
}

int
main(void)
{
	struct packet_buf *packets[PACKET_BUF_POOL_COUNT];
	struct packet_buf *packet, *copy;
	uint8_t *data;
	unsigned index;

	packet_buf_pool_init();
	for (index = 0; index < PACKET_BUF_POOL_COUNT; index++) {
		packets[index] = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
		assert(packets[index] != NULL);
	}
	assert(packet_buf_alloc(0) == NULL);
	assert(packet_buf_in_use() == PACKET_BUF_POOL_COUNT);
	packet_buf_free(packets[0]);
	assert(packet_buf_in_use() == PACKET_BUF_POOL_COUNT - 1U);
	packets[0] = packet_buf_alloc(32);
	assert(packets[0] != NULL);
	for (index = 0; index < PACKET_BUF_POOL_COUNT; index++)
		packet_buf_free(packets[index]);
	assert(packet_buf_in_use() == 0);

	packet = packet_buf_alloc(16);
	assert(packet != NULL);
	assert(packet_buf_headroom(packet) == 16);
	assert(packet_buf_push(packet, 17) == NULL);
	assert(packet_buf_headroom(packet) == 16 && packet->length == 0);
	data = packet_buf_append(packet, 4);
	assert(data != NULL);
	memcpy(data, "test", 4);
	assert(packet_buf_pull(packet, 5) == NULL);
	assert(packet->length == 4);
	packet->protocol = 0x0800;
	copy = packet_buf_copy(packet);
	assert(copy != NULL && copy->length == 4);
	assert(copy->data != packet->data);
	assert(!memcmp(copy->data, "test", 4));
	copy->data[0] = 'T';
	assert(packet->data[0] == 't');
	packet_buf_ref(packet);
	packet_buf_free(packet);
	assert(packet_buf_in_use() == 2);
	packet_buf_free(packet);
	packet_buf_free(copy);
	assert(packet_buf_in_use() == 0);

	puts("zedBSD packet buffer host tests: PASS");
	return 0;
}
