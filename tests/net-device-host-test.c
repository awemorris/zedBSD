/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
__libc_assert_fail(const char *expression, const char *file, int line)
{
	fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line, expression);
	abort();
}

static struct packet_buf *received;
static unsigned wakeups;
static unsigned transmitted;

int
net_input_enqueue(struct net_device *device, struct packet_buf *packet)
{
	assert(device != NULL && packet != NULL);
	received = packet;
	return 0;
}

void
net_worker_wakeup(void)
{
	wakeups++;
}

static int
fake_transmit(struct net_device *device, struct packet_buf *packet)
{
	assert(device != NULL && packet != NULL);
	transmitted++;
	packet_buf_free(packet);
	return 0;
}

static unsigned
fake_poll(struct net_device *device, unsigned budget)
{
	assert(device != NULL);
	return budget > 2 ? 2 : budget;
}

static const struct net_device_ops fake_ops = {
	.transmit = fake_transmit,
	.poll_receive = fake_poll,
};

int
main(void)
{
	struct net_device *device, *duplicate;
	struct packet_buf *packet;

	packet_buf_pool_init();
	net_device_registry_init();
	device = net_device_alloc();
	assert(device != NULL);
	strcpy(device->name, "ne0");
	device->mtu = 1500;
	device->hwaddr_len = 6;
	device->hwaddr[0] = 0x02;
	device->ops = &fake_ops;
	assert(net_device_create(device) == 0);
	assert(device->ifindex == 1);
	{
		struct net_device *found = net_device_find_ref("ne0");
		assert(found == device);
		net_device_release(found);
		found = net_device_find_by_index_ref(1);
		assert(found == device);
		net_device_release(found);
	}

	duplicate = net_device_alloc();
	assert(duplicate != NULL);
	strcpy(duplicate->name, "ne0");
	duplicate->mtu = 1500;
	duplicate->hwaddr_len = 6;
	duplicate->ops = &fake_ops;
	assert(net_device_create(duplicate) == EEXIST);

	packet = packet_buf_alloc(0);
	assert(packet != NULL && packet_buf_append(packet, 64) != NULL);
	assert(net_device_transmit(device, packet) == ENETDOWN);
	assert(net_device_open(device) == 0);
	packet = packet_buf_alloc(0);
	assert(packet != NULL && packet_buf_append(packet, 64) != NULL);
	assert(net_device_transmit(device, packet) == 0);
	assert(transmitted == 1 && device->tx_packets == 1);

	packet = packet_buf_alloc(0);
	assert(packet != NULL && packet_buf_append(packet, 60) != NULL);
	net_device_receive(device, packet);
	assert(received == packet && packet->device == device);
	assert(device->rx_packets == 1 && device->rx_bytes == 60);
	packet_buf_free(received);
	received = NULL;

	net_device_schedule_poll(device);
	assert(device->poll_scheduled && wakeups == 1);
	net_device_close(device);
	net_device_gone(device);
	net_device_destroy(device);
	assert(net_device_count() == 0);

	/* An object whose create failed remains allocated and can be retired. */
	net_device_destroy(duplicate);
	assert(packet_buf_in_use() == 0);
	puts("zedBSD net device host tests: PASS");
	return 0;
}
