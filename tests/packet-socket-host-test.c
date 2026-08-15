/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/byteorder.h"
#include "kern/net/ethernet.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/socket.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct thread;
void
zedbsd_assert_fail(const char *expression, const char *file, int line)
{
	fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line, expression);
	abort();
}
void *kern_calloc(size_t count, size_t size) { return calloc(count, size); }
void kern_free(void *pointer) { free(pointer); }
bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) { }
struct thread *thread_current(void) { return NULL; }
int signal_pending_unblocked(const struct thread *thread)
{
	(void)thread;
	return 0;
}
void sched_sleep(uint64_t ticks) { (void)ticks; }
void sched_wakeup(struct thread *thread) { (void)thread; }

uint64_t
sched_ticks(void)
{
	static uint64_t ticks;
	return ticks++;
}

int kern_deadline_after(uint64_t now, uint64_t delta, uint64_t *deadline)
{
	if (now > UINT64_MAX - delta) return EOVERFLOW;
	*deadline = now + delta;
	return 0;
}

static struct packet_buf *transmitted;

int net_input_enqueue(struct net_device *device, struct packet_buf *packet)
{
	(void)device; (void)packet; return ENOBUFS;
}
void net_worker_wakeup(void) { }

static int
fake_transmit(struct net_device *device, struct packet_buf *packet)
{
	(void)device;
	transmitted = packet;
	return 0;
}

static const struct net_device_ops fake_ops = { .transmit = fake_transmit };

int
main(void)
{
	static const uint8_t frame[] = {
		0x02,0,0,0,0,1, 0x02,0,0,0,0,2, 0x08,0x00,
		0x45,0,0,20, 0,0,0,0, 64,1,0,0, 10,0,2,2, 10,0,2,15
	};
	struct net_device *device;
	struct socket *socket;
	struct sockaddr_l2 bind_address, source;
	struct packet_buf *packet;
	uint8_t received[64];
	socklen_t source_length;
	ssize_t count;

	packet_buf_pool_init();
	net_device_registry_init();
	socket_core_init();
	ethernet_init();
	assert(packet_socket_init() == 0);
	device = net_device_alloc();
	assert(device != NULL);
	strcpy(device->name, "ne0");
	device->mtu = 1500;
	device->hwaddr_len = 6;
	memcpy(device->hwaddr, frame, 6);
	device->ops = &fake_ops;
	assert(net_device_create(device) == 0);
	assert(net_device_open(device) == 0);
	assert(socket_create(AF_PACKET, SOCK_RAW, net_htons(ETHERNET_TYPE_ALL),
			     &socket) == 0);
	memset(&bind_address, 0, sizeof(bind_address));
	bind_address.sl2_family = AF_PACKET;
	bind_address.sl2_protocol = net_htons(ETHERNET_TYPE_ALL);
	bind_address.sl2_ifindex = device->ifindex;
	assert(socket->ops->bind(socket, (struct sockaddr *)&bind_address,
				 sizeof(bind_address)) == 0);
	packet = packet_buf_alloc(0);
	assert(packet != NULL);
	assert(packet_buf_append(packet, sizeof(frame)) != NULL);
	memcpy(packet->data, frame, sizeof(frame));
	packet->device = device;
	assert(ethernet_input(packet) == 0);
	source_length = sizeof(source);
	count = socket->ops->recvfrom(socket, received, sizeof(received),
				      MSG_DONTWAIT,
				      (struct sockaddr *)&source,
				      &source_length);
	assert(count == (ssize_t)sizeof(frame));
	assert(!memcmp(received, frame, sizeof(frame)));
	assert(source.sl2_family == AF_PACKET);
	assert(source.sl2_ifindex == device->ifindex);
	assert(source.sl2_protocol == net_htons(ETHERNET_TYPE_IPV4));
	assert(!memcmp(source.sl2_addr, frame + 6, 6));

	count = socket->ops->sendto(socket, frame, sizeof(frame), 0,
				    (struct sockaddr *)&bind_address,
				    sizeof(bind_address));
	assert(count == (ssize_t)sizeof(frame));
	assert(transmitted != NULL && transmitted->length == sizeof(frame));
	packet_buf_free(transmitted);
	transmitted = NULL;
	socket_release(socket);
	net_device_close(device);
	net_device_gone(device);
	net_device_destroy(device);
	assert(packet_buf_in_use() == 0);
	puts("zedBSD AF_PACKET host tests: PASS");
	return 0;
}
