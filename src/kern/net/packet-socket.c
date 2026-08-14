/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/socket.h"
#include "kern/net/byteorder.h"
#include "kern/net/ethernet.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/kmem.h"

#include <errno.h>
#include <string.h>

struct packet_endpoint {
	struct socket socket;
	uint16_t protocol;
	unsigned ifindex;
	struct packet_endpoint *next;
};

static struct packet_endpoint *packet_sockets;

static struct packet_endpoint *
packet_endpoint(struct socket *socket)
{
	return (struct packet_endpoint *)socket;
}

static int
packet_bind(struct socket *socket, const struct sockaddr *address,
	    socklen_t length)
{
	const struct sockaddr_l2 *l2 = (const struct sockaddr_l2 *)address;
	struct packet_endpoint *endpoint = packet_endpoint(socket);
	uint16_t protocol;

	if (address == NULL || length < sizeof(*l2) ||
	    l2->sl2_family != AF_PACKET)
		return EINVAL;
	if (l2->sl2_ifindex != 0 &&
	    net_device_find_by_index(l2->sl2_ifindex) == NULL)
		return ENODEV;
	protocol = net_ntohs(l2->sl2_protocol);
	if (endpoint->protocol != 0 && protocol != 0 &&
	    endpoint->protocol != protocol)
		return EINVAL;
	if (protocol != 0)
		endpoint->protocol = protocol;
	endpoint->ifindex = l2->sl2_ifindex;
	return 0;
}

static ssize_t
packet_sendto(struct socket *socket, const void *buffer, size_t length,
	      int flags, const struct sockaddr *address, socklen_t address_length)
{
	const struct sockaddr_l2 *l2 = (const struct sockaddr_l2 *)address;
	struct packet_endpoint *endpoint = packet_endpoint(socket);
	struct net_device *device;
	struct packet_buf *packet;
	unsigned ifindex;
	void *data;
	int error;

	if (flags != 0 || buffer == NULL || length < ETHERNET_HEADER_LENGTH)
		return -EINVAL;
	if (address != NULL) {
		if (address_length < sizeof(*l2) || l2->sl2_family != AF_PACKET)
			return -EINVAL;
		ifindex = l2->sl2_ifindex;
	} else {
		ifindex = endpoint->ifindex;
	}
	device = net_device_find_by_index(ifindex);
	if (device == NULL)
		return -ENODEV;
	if (length > device->mtu + ETHERNET_HEADER_LENGTH)
		return -EMSGSIZE;
	packet = packet_buf_alloc(0);
	if (packet == NULL)
		return -ENOBUFS;
	data = packet_buf_append(packet, length);
	if (data == NULL) {
		packet_buf_free(packet);
		return -EMSGSIZE;
	}
	memcpy(data, buffer, length);
	error = net_device_transmit(device, packet);
	return error == 0 ? (ssize_t)length : -error;
}

static ssize_t
packet_recvfrom(struct socket *socket, void *buffer, size_t length,
		int flags, struct sockaddr *address, socklen_t *address_length)
{
	struct packet_buf *packet;
	size_t copied;
	int error;

	if (buffer == NULL)
		return -EINVAL;
	error = socket_dequeue_packet(socket, flags, &packet);
	if (error != 0)
		return -error;
	copied = length < packet->length ? length : packet->length;
	memcpy(buffer, packet->data, copied);
	if (address != NULL && address_length != NULL) {
		socklen_t source_length = packet->source_length;
		socklen_t output = *address_length < source_length ?
			*address_length : source_length;

		memcpy(address, packet->source_address, output);
		*address_length = source_length;
	}
	packet_buf_free(packet);
	return (ssize_t)copied;
}

static void
packet_close(struct socket *socket)
{
	struct packet_endpoint *endpoint = packet_endpoint(socket);
	struct packet_endpoint **link;

	for (link = &packet_sockets; *link != NULL; link = &(*link)->next) {
		if (*link != endpoint)
			continue;
		*link = endpoint->next;
		break;
	}
	kern_free(endpoint);
}

static const struct socket_ops packet_ops = {
	.bind = packet_bind,
	.sendto = packet_sendto,
	.recvfrom = packet_recvfrom,
	.close = packet_close,
};

static int
packet_create(int type, int protocol, struct socket **result)
{
	struct packet_endpoint *endpoint;

	if (type != SOCK_RAW)
		return EPROTONOSUPPORT;
	endpoint = kern_calloc(1, sizeof(*endpoint));
	if (endpoint == NULL)
		return ENOMEM;
	socket_init_object(&endpoint->socket, AF_PACKET, type, protocol,
			   &packet_ops);
	endpoint->protocol = net_ntohs((uint16_t)protocol);
	endpoint->next = packet_sockets;
	packet_sockets = endpoint;
	*result = &endpoint->socket;
	return 0;
}

static const struct socket_family_ops packet_family = {
	.create = packet_create,
};

int
packet_socket_init(void)
{
	packet_sockets = NULL;
	return socket_family_register(AF_PACKET, &packet_family);
}

void
packet_socket_deliver(const struct packet_buf *packet,
		      const uint8_t source[6], uint8_t packet_type)
{
	struct packet_endpoint *endpoint;

	for (endpoint = packet_sockets; endpoint != NULL;
	     endpoint = endpoint->next) {
		struct packet_buf *copy;
		struct sockaddr_l2 address;

		if (endpoint->ifindex != 0 &&
		    endpoint->ifindex != packet->device->ifindex)
			continue;
		if (endpoint->protocol != 0 &&
		    endpoint->protocol != ETHERNET_TYPE_ALL &&
		    endpoint->protocol != packet->protocol)
			continue;
		copy = packet_buf_copy(packet);
		if (copy == NULL)
			continue;
		memset(&address, 0, sizeof(address));
		address.sl2_family = AF_PACKET;
		address.sl2_protocol = net_htons(packet->protocol);
		address.sl2_ifindex = packet->device->ifindex;
		address.sl2_hatype = L2_HARDWARE_ETHER;
		address.sl2_pkttype = packet_type;
		address.sl2_halen = 6;
		memcpy(address.sl2_addr, source, 6);
		memcpy(copy->source_address, &address, sizeof(address));
		copy->source_length = sizeof(address);
		(void)socket_enqueue_packet(&endpoint->socket, copy);
	}
}
