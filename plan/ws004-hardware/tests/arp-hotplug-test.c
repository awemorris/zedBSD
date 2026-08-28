/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/lock.h"
#include "kern/net/ethernet.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "wire.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static ethernet_input_fn arp_input_fn;
static struct packet_buf tx_packet;
static uint8_t tx_storage[128];
static _Thread_local int irq_enabled = 1;

bool
hal_irq_disable(void)
{
	int previous = irq_enabled;

	irq_enabled = 0;
	return previous != 0;
}

void
hal_irq_enable(void)
{
	irq_enabled = 1;
}

void
spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	assert(lock != NULL);
	return 0;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{
	(void)enabled;
	assert(lock != NULL);
}

void
waitq_init(struct wait_queue *queue, const char *name)
{
	memset(queue, 0, sizeof(*queue));
	queue->name = name;
}

uint64_t
waitq_sequence(const struct wait_queue *queue)
{
	return queue->sequence;
}

void
waitq_wake_all(struct wait_queue *queue)
{
	queue->sequence++;
}

int
waitq_sleep(struct wait_queue *queue, struct spinlock *condition_lock,
	    uint64_t observed, uint64_t deadline, unsigned flags)
{
	(void)queue;
	(void)condition_lock;
	(void)observed;
	(void)deadline;
	(void)flags;
	return ETIMEDOUT;
}

struct thread *
thread_current(void)
{
	return NULL;
}

uint64_t
sched_ticks(void)
{
	return 0;
}

void
route_purge_device(struct net_device *device)
{
	assert(device != NULL);
}

void
inet_interface_purge_device(struct net_device *device)
{
	assert(device != NULL);
}

int
inet_interface_address(struct net_device *device, uint32_t *address,
		       uint32_t *netmask, uint32_t *broadcast)
{
	assert(device != NULL);
	if (address != NULL)
		*address = 0x0a000001U;
	if (netmask != NULL)
		*netmask = 0xffffff00U;
	if (broadcast != NULL)
		*broadcast = 0x0a0000ffU;
	return 0;
}

int
net_input_enqueue(struct net_device *device, struct packet_buf *packet)
{
	(void)device;
	packet_buf_free(packet);
	return 0;
}

void
net_worker_wakeup(void)
{
}

struct packet_buf *
packet_buf_alloc(size_t headroom)
{
	assert(headroom < sizeof(tx_storage));
	memset(&tx_packet, 0, sizeof(tx_packet));
	tx_packet.storage = tx_storage;
	tx_packet.data = tx_storage + headroom;
	tx_packet.capacity = sizeof(tx_storage);
	return &tx_packet;
}

void *
packet_buf_append(struct packet_buf *packet, size_t length)
{
	void *result;

	assert(packet != NULL);
	assert((size_t)(packet->data - packet->storage) + packet->length + length <=
	       packet->capacity);
	result = packet->data + packet->length;
	packet->length += length;
	return result;
}

void
packet_buf_free(struct packet_buf *packet)
{
	struct net_device *device;

	if (packet == NULL)
		return;
	device = packet->device;
	packet->device = NULL;
	if (device != NULL)
		net_device_release(device);
}

int
ethernet_protocol_register(uint16_t type, ethernet_input_fn input)
{
	assert(type == ETHERNET_TYPE_ARP);
	arp_input_fn = input;
	return 0;
}

int
ethernet_output(struct net_device *device, const uint8_t destination[6],
		uint16_t type, struct packet_buf *packet)
{
	(void)device;
	(void)destination;
	assert(type == ETHERNET_TYPE_ARP);
	packet_buf_free(packet);
	return 0;
}

static int
dummy_transmit(struct net_device *device, struct packet_buf *packet)
{
	(void)device;
	packet_buf_free(packet);
	return 0;
}

static const struct net_device_ops dummy_ops = {
	.transmit = dummy_transmit,
};

static struct net_device *
create_device(const char *name)
{
	struct net_device *device = net_device_alloc();

	assert(device != NULL);
	strcpy(device->name, name);
	device->mtu = 1500;
	device->hwaddr_len = 6;
	device->hwaddr[5] = 1;
	device->flags = NET_DEVICE_BROADCAST;
	device->ops = &dummy_ops;
	assert(net_device_create(device) == 0);
	return device;
}

static void
learn(struct net_device *device, uint32_t sender, const uint8_t hardware[6])
{
	struct packet_buf packet;
	struct arp_wire wire;
	uint8_t storage[sizeof(wire)];

	memset(&wire, 0, sizeof(wire));
	wire_put16(wire.hardware_type, 1);
	wire_put16(wire.protocol_type, ETHERNET_TYPE_IPV4);
	wire.hardware_length = 6;
	wire.protocol_length = 4;
	wire_put16(wire.operation, 2);
	memcpy(wire.sender_hardware, hardware, 6);
	wire_put32(wire.sender_protocol, sender);
	wire_put32(wire.target_protocol, 0x0a000002U);
	memcpy(storage, &wire, sizeof(wire));
	memset(&packet, 0, sizeof(packet));
	packet.storage = storage;
	packet.data = storage;
	packet.capacity = sizeof(storage);
	packet.length = sizeof(storage);
	packet.device = device;
	net_device_ref(device);
	assert(arp_input_fn(&packet) == 0);
}

extern int arp_init(void);
extern int arp_resolve(struct net_device *, uint32_t, uint8_t[6]);

int
main(void)
{
	static const uint8_t hardware[6] = {0x02, 0, 0, 0, 0, 0x2a};
	struct net_device *first, *second;
	uint8_t result[6];

	net_device_registry_init();
	assert(arp_init() == 0);
	assert(arp_input_fn != NULL);
	first = create_device("ue0");
	learn(first, 0x0a00002aU, hardware);
	assert(arp_resolve(first, 0x0a00002aU, result) == 0);
	assert(memcmp(result, hardware, sizeof(result)) == 0);

	assert(net_device_gone(first) == 0);
	assert(arp_resolve(first, 0x0a00002aU, result) == ENODEV);
	net_device_destroy(first);
	second = create_device("ue1");
	assert(second == first);
	/* Slot identity reuse must not alias the removed interface's ARP row. */
	assert(arp_resolve(second, 0x0a00002aU, result) == ENOENT);
	assert(net_device_gone(second) == 0);
	net_device_destroy(second);
	puts("ARP hotplug tests: PASS");
	return 0;
}
