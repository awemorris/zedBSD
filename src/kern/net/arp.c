/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/ethernet.h"
#include "kern/net/inet-socket.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "internal.h"
#include "wire.h"

#include <zedbsd/netinet.h>
#include <errno.h>
#include <string.h>

#define ARP_CACHE_MAX 16U
#define ARP_HARDWARE_ETHERNET 1U
#define ARP_OPERATION_REQUEST 1U
#define ARP_OPERATION_REPLY 2U

struct arp_entry {
	struct net_device *device;
	uint32_t address;
	uint8_t hardware[6];
	uint8_t valid;
};

static struct arp_entry cache[ARP_CACHE_MAX];
static unsigned replacement;

static void
arp_learn(struct net_device *device, uint32_t address, const uint8_t hardware[6])
{
	unsigned index, slot = ARP_CACHE_MAX;

	if (address == 0)
		return;
	for (index = 0; index < ARP_CACHE_MAX; index++) {
		if (cache[index].valid && cache[index].device == device &&
		    cache[index].address == address) {
			slot = index;
			break;
		}
		if (!cache[index].valid && slot == ARP_CACHE_MAX)
			slot = index;
	}
	if (slot == ARP_CACHE_MAX) {
		slot = replacement++ % ARP_CACHE_MAX;
	}
	cache[slot].device = device;
	cache[slot].address = address;
	memcpy(cache[slot].hardware, hardware, 6);
	cache[slot].valid = 1;
}

int
arp_resolve(struct net_device *device, uint32_t address, uint8_t hardware[6])
{
	unsigned index;
	uint32_t local, mask, broadcast;

	if (device == NULL || hardware == NULL)
		return EINVAL;
	if (address == INADDR_BROADCAST ||
	    (inet_interface_address(device, &local, &mask, &broadcast) == 0 &&
	     address == broadcast)) {
		memset(hardware, 0xff, 6);
		return 0;
	}
	for (index = 0; index < ARP_CACHE_MAX; index++)
		if (cache[index].valid && cache[index].device == device &&
		    cache[index].address == address) {
			memcpy(hardware, cache[index].hardware, 6);
			return 0;
		}
	return ENOENT;
}

static int
arp_send(struct net_device *device, uint16_t operation,
	 uint32_t target_address, const uint8_t target_hardware[6])
{
	static const uint8_t broadcast[6] =
	    { 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU };
	struct packet_buf *packet;
	struct arp_wire *arp;
	uint32_t source;

	if (inet_interface_address(device, &source, NULL, NULL) != 0)
		return EADDRNOTAVAIL;
	packet = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
	if (packet == NULL)
		return ENOBUFS;
	arp = packet_buf_append(packet, sizeof(*arp));
	if (arp == NULL) {
		packet_buf_free(packet);
		return ENOBUFS;
	}
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
	return ethernet_output(device,
	    target_hardware != NULL ? target_hardware : broadcast,
	    ETHERNET_TYPE_ARP, packet);
}

int arp_request(struct net_device *device, uint32_t address)
{
	return arp_send(device, ARP_OPERATION_REQUEST, address, NULL);
}

static int
arp_input(struct packet_buf *packet)
{
	const struct arp_wire *arp;
	uint16_t operation;
	uint32_t sender, target, local;

	if (packet == NULL || packet->device == NULL ||
	    packet->length < sizeof(*arp)) {
		packet_buf_free(packet);
		return EINVAL;
	}
	arp = (const struct arp_wire *)packet->data;
	if (wire_get16(arp->hardware_type) != ARP_HARDWARE_ETHERNET ||
	    wire_get16(arp->protocol_type) != ETHERNET_TYPE_IPV4 ||
	    arp->hardware_length != 6 || arp->protocol_length != 4) {
		packet_buf_free(packet);
		return EINVAL;
	}
	operation = wire_get16(arp->operation);
	sender = wire_get32(arp->sender_protocol);
	target = wire_get32(arp->target_protocol);
	arp_learn(packet->device, sender, arp->sender_hardware);
	if (operation == ARP_OPERATION_REQUEST &&
	    inet_interface_address(packet->device, &local, NULL, NULL) == 0 &&
	    target == local)
		(void)arp_send(packet->device, ARP_OPERATION_REPLY, sender,
		    arp->sender_hardware);
	packet_buf_free(packet);
	return operation == ARP_OPERATION_REQUEST ||
	    operation == ARP_OPERATION_REPLY ? 0 : EINVAL;
}

int
arp_init(void)
{
	memset(cache, 0, sizeof(cache));
	replacement = 0;
	return ethernet_protocol_register(ETHERNET_TYPE_ARP, arp_input);
}
