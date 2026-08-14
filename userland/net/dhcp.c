/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/net/dhcp.h"

#include <string.h>

static uint32_t read32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	    (uint32_t)p[2] << 8 | p[3];
}
static void write16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void write32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static int
option(uint8_t *packet, size_t capacity, size_t *offset, uint8_t code,
	const void *value, uint8_t length)
{
	if (*offset + 2U + length > capacity) return -1;
	packet[(*offset)++] = code; packet[(*offset)++] = length;
	memcpy(packet + *offset, value, length); *offset += length;
	return 0;
}

int
dhcp_build(uint8_t *packet, size_t capacity, size_t *length, uint8_t type,
	uint32_t xid, const uint8_t mac[6], uint32_t requested,
	uint32_t server_identifier)
{
	static const uint8_t client_prefix = 1;
	static const uint8_t parameters[] = { 1, 3, 6, 28, 51, 58, 59 };
	uint8_t client[7], max_size[2];
	size_t offset = 240;
	if (capacity < 300U || packet == NULL || length == NULL) return -1;
	memset(packet, 0, capacity);
	packet[0] = 1; packet[1] = 1; packet[2] = 6;
	write32(packet + 4, xid); write16(packet + 10, 0x8000U);
	memcpy(packet + 28, mac, 6);
	write32(packet + 236, 0x63825363U);
	if (option(packet, capacity, &offset, 53, &type, 1) != 0) return -1;
	client[0] = client_prefix; memcpy(client + 1, mac, 6);
	if (option(packet, capacity, &offset, 61, client, sizeof(client)) != 0 ||
	    option(packet, capacity, &offset, 55, parameters, sizeof(parameters)) != 0)
		return -1;
	write16(max_size, 576U);
	if (option(packet, capacity, &offset, 57, max_size, 2) != 0) return -1;
	if (type == DHCP_REQUEST) {
		uint8_t value[4];
		memcpy(value, &requested, 4U);
		if (option(packet, capacity, &offset, 50, value, 4) != 0) return -1;
		memcpy(value, &server_identifier, 4U);
		if (option(packet, capacity, &offset, 54, value, 4) != 0) return -1;
	}
	if (offset >= capacity) return -1;
	packet[offset++] = 255;
	if (offset < 300U) offset = 300U;
	*length = offset;
	return 0;
}

int
dhcp_parse(const uint8_t *packet, size_t length, uint32_t xid,
	const uint8_t mac[6], struct dhcp_lease *lease)
{
	size_t offset = 240;
	if (length < 241U || packet[0] != 2 || packet[1] != 1 || packet[2] != 6 ||
	    read32(packet + 4) != xid || memcmp(packet + 28, mac, 6) != 0 ||
	    read32(packet + 236) != 0x63825363U) return -1;
	memset(lease, 0, sizeof(*lease));
	memcpy(&lease->address, packet + 16, 4U);
	while (offset < length) {
		uint8_t code = packet[offset++], size;
		unsigned i;
		if (code == 0) continue;
		if (code == 255) break;
		if (offset >= length) return -1;
		size = packet[offset++];
		if (offset + size > length) return -1;
		switch (code) {
		case 1: if (size == 4) memcpy(&lease->netmask, packet + offset, 4U); break;
		case 3:
			for (i = 0; i + 4U <= size && lease->router_count < 4U; i += 4U)
				memcpy(&lease->routers[lease->router_count++], packet + offset + i, 4U);
			break;
		case 6:
			for (i = 0; i + 4U <= size && lease->dns_count < 3U; i += 4U)
				memcpy(&lease->dns_servers[lease->dns_count++], packet + offset + i, 4U);
			break;
		case 28: if (size == 4) memcpy(&lease->broadcast, packet + offset, 4U); break;
		case 51: if (size == 4) lease->lease_time = read32(packet + offset); break;
		case 53: if (size == 1) lease->message_type = packet[offset]; break;
		case 54: if (size == 4) memcpy(&lease->server_identifier, packet + offset, 4U); break;
		case 58: if (size == 4) lease->renewal_time = read32(packet + offset); break;
		case 59: if (size == 4) lease->rebinding_time = read32(packet + offset); break;
		default: break;
		}
		offset += size;
	}
	if (lease->message_type == 0 || lease->address == 0) return -1;
	if (lease->broadcast == 0 && lease->netmask != 0)
		lease->broadcast = lease->address | ~lease->netmask;
	return 0;
}
