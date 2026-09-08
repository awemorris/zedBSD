/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Address resolution for IPv4 over Ethernet.
 *
 * A small cache maps a device and IPv4 address to a hardware address.
 * Entries are learned from every ARP packet received, requests for a
 * local address are answered, and a waiting resolver retries the request
 * a few times while sleeping on the cache's wait queue.
 */

#include "kern/net/ethernet.h"
#include "kern/net/inet-socket.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/clock.h"
#include "kern/lock.h"
#include "kern/sched.h"
#include "kern/thread.h"
#include "internal.h"
#include "wire.h"

#include <zedbsd/netinet.h>
#include <errno.h>
#include <string.h>

#define ARP_CACHE_MAX 16U
#define ARP_HARDWARE_ETHERNET 1U
#define ARP_OPERATION_REQUEST 1U
#define ARP_OPERATION_REPLY 2U
#define ARP_RETRY_COUNT 4U
#define ARP_RETRY_TICKS (KERN_CLOCK_HZ / 4U)

struct arp_entry {
	struct net_device *device;
	uint32_t address;
	uint8_t hardware[6];
	uint8_t valid;
};

static struct arp_entry cache[ARP_CACHE_MAX];
static unsigned replacement;
static struct spinlock cache_lock;
static struct wait_queue cache_waitq;

static int arp_lookup_locked(struct net_device *device, uint32_t address, uint8_t hardware[6]);
static void arp_learn(struct net_device *device, uint32_t address, const uint8_t hardware[6]);
static int arp_send(struct net_device *device, uint16_t operation, uint32_t target_address, const uint8_t target_hardware[6]);
static int arp_input(struct packet_buf *packet);

/*
 * Drops every cache entry of a device that is going away.
 *
 * The device references are released outside the cache lock.
 */
void
arp_purge_device(
	struct net_device *device)
{
	struct net_device *references[ARP_CACHE_MAX];
	unsigned count;
	unsigned index;
	unsigned long irq;

	count = 0;

	/* Ignores a missing device. */
	if (device == NULL)
		return;

	/* Clears the device's entries, collecting their references. */
	irq = spin_lock_irqsave(&cache_lock);

	for (index = 0; index < ARP_CACHE_MAX; index++) {
		if (!cache[index].valid || cache[index].device != device)
			continue;
		references[count++] = cache[index].device;
		memset(&cache[index], 0, sizeof(cache[index]));
	}

	if (count != 0)
		waitq_wake_all(&cache_waitq);

	spin_unlock_irqrestore(&cache_lock, irq);

	/* Releases the references. */
	for (index = 0; index < count; index++)
		net_device_release(references[index]);
}

/*
 * Resolves an IPv4 address from the cache without waiting.
 *
 * The limited and the subnet broadcast resolve to the broadcast hardware
 * address; anything else must already be cached.
 */
int
arp_resolve(
	struct net_device *device,
	uint32_t address,
	uint8_t hardware[6])
{
	uint32_t local;
	uint32_t mask;
	uint32_t broadcast;
	unsigned long irq;
	int error;

	/* Rejects a missing operand or a device that is not live. */
	if (device == NULL || hardware == NULL)
		return EINVAL;
	if (!net_device_is_live(device))
		return ENODEV;

	/* A broadcast address resolves without the cache. */
	if (address == INADDR_BROADCAST ||
	    (inet_interface_address(device, &local, &mask, &broadcast) == 0 &&
	     address == broadcast)) {
		memset(hardware, 0xff, 6);
		return 0;
	}

	/* Looks the address up in the cache. */
	irq = spin_lock_irqsave(&cache_lock);

	error = arp_lookup_locked(device, address, hardware);

	spin_unlock_irqrestore(&cache_lock, irq);

	/* Reports why the lookup failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Resolves an IPv4 address, sending requests and waiting for the reply.
 *
 * Up to ARP_RETRY_COUNT requests are sent a quarter second apart.  A
 * signal interrupts the wait, and a caller without a thread cannot wait.
 */
int
arp_resolve_wait(
	struct net_device *device,
	uint32_t address,
	uint8_t hardware[6])
{
	unsigned attempt;
	uint64_t sequence;
	uint64_t deadline;
	unsigned long irq;
	int error;

	/* Rejects a missing operand. */
	if (device == NULL || hardware == NULL)
		return EINVAL;

	/* A cached address needs no request. */
	if (arp_resolve(device, address, hardware) == 0)
		return 0;

	/* Only a thread can wait for the reply. */
	if (thread_current() == NULL)
		return EAGAIN;

	/* Requests and waits, re-checking the cache around every step. */
	for (attempt = 0; attempt < ARP_RETRY_COUNT; attempt++) {
		irq = spin_lock_irqsave(&cache_lock);
		if (arp_lookup_locked(device, address, hardware) == 0) {
			spin_unlock_irqrestore(&cache_lock, irq);
			return 0;
		}

		sequence = waitq_sequence(&cache_waitq);
		spin_unlock_irqrestore(&cache_lock, irq);

		/* Sends the request with the lock dropped. */
		error = arp_request(device, address);
		if (error != 0)
			return error;

		/* Sleeps until the cache changes or the retry interval expires. */
		irq = spin_lock_irqsave(&cache_lock);
		if (arp_lookup_locked(device, address, hardware) == 0) {
			spin_unlock_irqrestore(&cache_lock, irq);
			return 0;
		}

		deadline = sched_ticks() + ARP_RETRY_TICKS;
		error = waitq_sleep(&cache_waitq, &cache_lock, sequence,
		    deadline, WAITQ_INTERRUPTIBLE);
		if (arp_lookup_locked(device, address, hardware) == 0) {
			spin_unlock_irqrestore(&cache_lock, irq);
			return 0;
		}

		spin_unlock_irqrestore(&cache_lock, irq);
		if (error == EINTR)
			return EINTR;
	}

	/* Reports a host that never answered. */
	return EHOSTUNREACH;
}

/*
 * Broadcasts an ARP request for an IPv4 address.
 */
int
arp_request(
	struct net_device *device,
	uint32_t address)
{
	int error;

	/* Reports why the send failed. */
	error = arp_send(device, ARP_OPERATION_REQUEST, address, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Initializes the cache and registers ARP with Ethernet.
 */
int
arp_init(
	void)
{
	unsigned index;
	int error;

	/* Releases the devices of any entries left from a previous init. */
	for (index = 0; index < ARP_CACHE_MAX; index++) {
		if (cache[index].valid && cache[index].device != NULL)
			net_device_release(cache[index].device);
	}

	/* Starts with an empty cache. */
	memset(cache, 0, sizeof(cache));
	replacement = 0;
	spin_init(&cache_lock, LOCK_RANK_NETWORK, "ARP cache");
	waitq_init(&cache_waitq, "ARP resolution");

	/* Receives ARP frames from Ethernet. */

	/* Reports why the registration failed. */
	error = ethernet_protocol_register(ETHERNET_TYPE_ARP, arp_input);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Finds a cached hardware address; the caller holds the cache lock. */
static int
arp_lookup_locked(
	struct net_device *device,
	uint32_t address,
	uint8_t hardware[6])
{
	unsigned index;

	/* Searches for a valid entry of the device and address. */
	for (index = 0; index < ARP_CACHE_MAX; index++) {
		if (cache[index].valid &&
		    cache[index].device == device &&
		    cache[index].address == address) {
			memcpy(hardware, cache[index].hardware, 6);
			return 0;
		}
	}

	/* Reports a missing entry. */
	return ENOENT;
}

/* Records a hardware address learned from a received ARP packet. */
static void
arp_learn(
	struct net_device *device,
	uint32_t address,
	const uint8_t hardware[6])
{
	struct net_device *release_device;
	unsigned index;
	unsigned slot;
	unsigned long irq;
	int insert_reference;

	release_device = NULL;
	slot = ARP_CACHE_MAX;
	insert_reference = 1;

	/* Ignores an unspecified address or a device that is not live. */
	if (address == 0 || !net_device_ref_live(device))
		return;

	irq = spin_lock_irqsave(&cache_lock);

	/*
	 * Serializes the final live-state check with cache publication.  If
	 * GONE wins first, its purge may already have run; if it wins after
	 * this check, its purge waits for cache_lock and observes this entry.
	 */
	if (!net_device_is_live(device)) {
		spin_unlock_irqrestore(&cache_lock, irq);
		net_device_release(device);
		return;
	}

	/* Reuses the existing entry, else the first free slot. */
	for (index = 0; index < ARP_CACHE_MAX; index++) {
		if (cache[index].valid &&
		    cache[index].device == device &&
		    cache[index].address == address) {
			slot = index;
			insert_reference = 0;
			break;
		}

		if (!cache[index].valid && slot == ARP_CACHE_MAX)
			slot = index;
	}

	/* With no free slot, replaces entries round-robin. */
	if (slot == ARP_CACHE_MAX)
		slot = replacement++ % ARP_CACHE_MAX;
	if (insert_reference && cache[slot].valid)
		release_device = cache[slot].device;

	/* Publishes the entry and wakes the waiting resolvers. */
	cache[slot].device = device;
	cache[slot].address = address;
	memcpy(cache[slot].hardware, hardware, 6);
	cache[slot].valid = 1;
	waitq_wake_all(&cache_waitq);

	spin_unlock_irqrestore(&cache_lock, irq);

	/* Keeps one reference per entry: drops ours or the replaced one. */
	if (!insert_reference)
		net_device_release(device);
	if (release_device != NULL)
		net_device_release(release_device);
}

/* Sends an ARP request or reply; a reply goes to the target's address. */
static int
arp_send(
	struct net_device *device,
	uint16_t operation,
	uint32_t target_address,
	const uint8_t target_hardware[6])
{
	static const uint8_t broadcast[6] =
	    { 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU };
	struct packet_buf *packet;
	struct arp_wire *arp;
	const uint8_t *destination;
	uint32_t source;
	int error;

	/* The device must have an address to answer with. */
	if (inet_interface_address(device, &source, NULL, NULL) != 0)
		return EADDRNOTAVAIL;

	/* Allocates the packet. */
	packet = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
	if (packet == NULL)
		return ENOBUFS;
	arp = packet_buf_append(packet, sizeof(*arp));
	if (arp == NULL) {
		packet_buf_free(packet);
		return ENOBUFS;
	}

	/* Fills the Ethernet/IPv4 ARP body. */
	memset(arp, 0, sizeof(*arp));
	wire_put16(arp->hardware_type, ARP_HARDWARE_ETHERNET);
	wire_put16(arp->protocol_type, ETHERNET_TYPE_IPV4);
	arp->hardware_length = 6;
	arp->protocol_length = 4;
	wire_put16(arp->operation, operation);
	memcpy(arp->sender_hardware, device->hwaddr, 6);
	wire_put32(arp->sender_protocol, source);
	if (target_hardware != NULL)
		memcpy(arp->target_hardware, target_hardware, 6);
	wire_put32(arp->target_protocol, target_address);

	/* A request is broadcast; a reply goes to the requester. */
	destination = broadcast;
	if (target_hardware != NULL)
		destination = target_hardware;

	/* Reports why the send failed. */
	error = ethernet_output(device, destination, ETHERNET_TYPE_ARP, packet);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Receives an ARP packet: learns the sender and answers a request for us. */
static int
arp_input(
	struct packet_buf *packet)
{
	const struct arp_wire *arp;
	uint16_t operation;
	uint32_t sender;
	uint32_t target;
	uint32_t local;

	/* Drops a packet without a device or a complete body. */
	if (packet == NULL ||
	    packet->device == NULL ||
	    packet->length < sizeof(*arp)) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Drops anything but Ethernet/IPv4 ARP. */
	arp = (const struct arp_wire *)packet->data;
	if (wire_get16(arp->hardware_type) != ARP_HARDWARE_ETHERNET ||
	    wire_get16(arp->protocol_type) != ETHERNET_TYPE_IPV4 ||
	    arp->hardware_length != 6 ||
	    arp->protocol_length != 4) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Learns the sender from every packet. */
	operation = wire_get16(arp->operation);
	sender = wire_get32(arp->sender_protocol);
	target = wire_get32(arp->target_protocol);
	arp_learn(packet->device, sender, arp->sender_hardware);

	/* Answers a request for the device's own address. */
	if (operation == ARP_OPERATION_REQUEST &&
	    inet_interface_address(packet->device, &local, NULL, NULL) == 0 &&
	    target == local)
		(void)arp_send(packet->device, ARP_OPERATION_REPLY, sender,
		    arp->sender_hardware);
	packet_buf_free(packet);

	/* Reports an unknown operation as invalid. */
	if (operation == ARP_OPERATION_REQUEST)
		return 0;
	if (operation == ARP_OPERATION_REPLY)
		return 0;
	return EINVAL;
}
