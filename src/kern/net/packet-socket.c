/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * AF_PACKET sockets.
 *
 * A packet socket sends raw Ethernet frames to a device and receives a
 * copy of every frame that matches its interface and protocol filter.
 * Delivery snapshots the registry so that sockets can close while frames
 * are queued.
 */

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
static struct spinlock packet_registry_lock;

static struct packet_endpoint *packet_endpoint(struct socket *socket);
static int packet_bind(struct socket *socket, const struct sockaddr *address, socklen_t length);
static ssize_t packet_sendto(struct socket *socket, const void *buffer, size_t length, int flags, const struct sockaddr *address, socklen_t address_length);
static ssize_t packet_recvfrom(struct socket *socket, void *buffer, size_t length, int flags, struct sockaddr *address, socklen_t *address_length);
static void packet_close(struct socket *socket);
static int packet_create(int type, int protocol, struct socket **result);

static const struct socket_ops packet_ops = {
	.bind = packet_bind,
	.sendto = packet_sendto,
	.recvfrom = packet_recvfrom,
	.close = packet_close,
};

static const struct socket_family_ops packet_family = {
	.create = packet_create,
};

/*
 * Initializes the packet socket registry and registers the family.
 */
int
packet_socket_init(
	void)
{
	int error;

	/* Starts with no sockets. */
	packet_sockets = NULL;
	spin_init(&packet_registry_lock, LOCK_RANK_SOCKET_REGISTRY,
	    "packet socket registry");

	/* Registers AF_PACKET. */
	error = socket_family_register(AF_PACKET, &packet_family);

	/* Reports the registration result. */
	return error;
}

/*
 * Delivers a copy of a received frame to every matching packet socket.
 *
 * The source address handed to each socket names the device, the frame's
 * protocol and classification, and the sender's hardware address.
 */
void
packet_socket_deliver(
	const struct packet_buf *packet,
	const uint8_t source[6],
	uint8_t packet_type)
{
	struct packet_endpoint *endpoint;
	struct packet_endpoint *snapshot[SOCKET_MAX];
	struct packet_buf *copy;
	struct sockaddr_l2 address;
	unsigned count;
	unsigned index;
	unsigned long irq;

	count = 0;

	/* References every socket under the registry lock. */
	irq = spin_lock_irqsave(&packet_registry_lock);
	for (endpoint = packet_sockets; endpoint != NULL;
	     endpoint = endpoint->next) {
		if (count < SOCKET_MAX && socket_tryref(&endpoint->socket))
			snapshot[count++] = endpoint;
	}
	spin_unlock_irqrestore(&packet_registry_lock, irq);

	/* Queues a copy on each socket whose filter matches the frame. */
	for (index = 0; index < count; index++) {
		endpoint = snapshot[index];

		/* Skips a socket bound to another interface or protocol. */
		if (endpoint->ifindex != 0 &&
		    endpoint->ifindex != packet->device->ifindex) {
			socket_release(&endpoint->socket);
			continue;
		}
		if (endpoint->protocol != 0 &&
		    endpoint->protocol != ETHERNET_TYPE_ALL &&
		    endpoint->protocol != packet->protocol) {
			socket_release(&endpoint->socket);
			continue;
		}

		/* Copies the frame; a socket that gets no copy misses the frame. */
		copy = packet_buf_copy(packet);
		if (copy == NULL) {
			socket_release(&endpoint->socket);
			continue;
		}

		/* Describes the sender in the copy's source address. */
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
		socket_release(&endpoint->socket);
	}
}

/* Converts a socket to its packet endpoint. */
static struct packet_endpoint *
packet_endpoint(
	struct socket *socket)
{
	return (struct packet_endpoint *)socket;
}

/* Binds a packet socket to an interface and protocol filter. */
static int
packet_bind(
	struct socket *socket,
	const struct sockaddr *address,
	socklen_t length)
{
	const struct sockaddr_l2 *l2;
	struct packet_endpoint *endpoint;
	struct net_device *device;
	uint16_t protocol;

	l2 = (const struct sockaddr_l2 *)address;
	endpoint = packet_endpoint(socket);

	/* Rejects anything but a complete AF_PACKET address. */
	if (address == NULL ||
	    length < sizeof(*l2) ||
	    l2->sl2_family != AF_PACKET)
		return EINVAL;

	/* A named interface must exist. */
	if (l2->sl2_ifindex != 0) {
		device = net_device_find_by_index_ref(l2->sl2_ifindex);
		if (device == NULL)
			return ENODEV;
		net_device_release(device);
	}

	/* The protocol chosen at creation cannot be changed to another. */
	protocol = net_ntohs(l2->sl2_protocol);
	if (endpoint->protocol != 0 &&
	    protocol != 0 &&
	    endpoint->protocol != protocol)
		return EINVAL;

	/* Installs the filter. */
	if (protocol != 0)
		endpoint->protocol = protocol;
	endpoint->ifindex = l2->sl2_ifindex;

	/* Reports the bound socket. */
	return 0;
}

/* Sends a raw Ethernet frame to the addressed or bound interface. */
static ssize_t
packet_sendto(
	struct socket *socket,
	const void *buffer,
	size_t length,
	int flags,
	const struct sockaddr *address,
	socklen_t address_length)
{
	const struct sockaddr_l2 *l2;
	struct packet_endpoint *endpoint;
	struct net_device *device;
	struct packet_buf *packet;
	unsigned ifindex;
	void *data;
	int error;

	l2 = (const struct sockaddr_l2 *)address;
	endpoint = packet_endpoint(socket);

	/* Rejects unsupported flags, a missing buffer, or a headerless frame. */
	if ((flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) != 0 ||
	    buffer == NULL ||
	    length < ETHERNET_HEADER_LENGTH)
		return -EINVAL;

	/* Takes the interface from the address, or from the binding. */
	if (address != NULL) {
		if (address_length < sizeof(*l2) || l2->sl2_family != AF_PACKET)
			return -EINVAL;
		ifindex = l2->sl2_ifindex;
	} else {
		ifindex = endpoint->ifindex;
	}
	device = net_device_find_by_index_ref(ifindex);
	if (device == NULL)
		return -ENODEV;

	/* Rejects a frame larger than the interface can carry. */
	if (length > device->mtu + ETHERNET_HEADER_LENGTH) {
		net_device_release(device);
		return -EMSGSIZE;
	}

	/* Copies the frame into a packet buffer. */
	packet = packet_buf_alloc(0);
	if (packet == NULL) {
		net_device_release(device);
		return -ENOBUFS;
	}
	data = packet_buf_append(packet, length);
	if (data == NULL) {
		packet_buf_free(packet);
		net_device_release(device);
		return -EMSGSIZE;
	}
	memcpy(data, buffer, length);

	/* Transmits it as is. */
	error = net_device_transmit(device, packet);
	net_device_release(device);

	/* Reports the sent length or the error. */
	if (error == 0)
		return (ssize_t)length;
	return -error;
}

/* Receives one queued frame and its source address. */
static ssize_t
packet_recvfrom(
	struct socket *socket,
	void *buffer,
	size_t length,
	int flags,
	struct sockaddr *address,
	socklen_t *address_length)
{
	struct packet_buf *packet;
	size_t copied;
	socklen_t source_length;
	socklen_t output;
	ssize_t result;
	int error;

	/* Rejects a missing buffer or unsupported flags. */
	if (buffer == NULL)
		return -EINVAL;
	if ((flags & ~(MSG_DONTWAIT | MSG_TRUNC)) != 0)
		return -EOPNOTSUPP;

	/* Takes the next queued frame. */
	error = socket_dequeue_packet(socket, flags & MSG_DONTWAIT, &packet);
	if (error != 0)
		return -error;

	/* Copies as much of the frame as fits. */
	if (length < packet->length)
		copied = length;
	else
		copied = packet->length;
	memcpy(buffer, packet->data, copied);

	/* Copies the source address, reporting its full length. */
	if (address != NULL && address_length != NULL) {
		source_length = packet->source_length;
		if (*address_length < source_length)
			output = *address_length;
		else
			output = source_length;
		memcpy(address, packet->source_address, output);
		*address_length = source_length;
	}

	/* With MSG_TRUNC the full frame length is reported instead. */
	if ((flags & MSG_TRUNC) != 0)
		result = (ssize_t)packet->length;
	else
		result = (ssize_t)copied;
	packet_buf_free(packet);

	/* Reports the received length. */
	return result;
}

/* Unregisters and frees a packet socket. */
static void
packet_close(
	struct socket *socket)
{
	struct packet_endpoint *endpoint;
	struct packet_endpoint **link;
	unsigned long irq;

	endpoint = packet_endpoint(socket);

	/* Unlinks the endpoint from the registry. */
	irq = spin_lock_irqsave(&packet_registry_lock);
	for (link = &packet_sockets; *link != NULL; link = &(*link)->next) {
		if (*link != endpoint)
			continue;
		*link = endpoint->next;
		break;
	}
	spin_unlock_irqrestore(&packet_registry_lock, irq);

	kern_free(endpoint);
}

/* Creates a raw packet socket with an optional protocol filter. */
static int
packet_create(
	int type,
	int protocol,
	struct socket **result)
{
	struct packet_endpoint *endpoint;
	unsigned long irq;

	/* Only raw sockets exist in this family. */
	if (type != SOCK_RAW)
		return EPROTONOSUPPORT;

	/* Allocates the endpoint with the network-order protocol filter. */
	endpoint = kern_calloc(1, sizeof(*endpoint));
	if (endpoint == NULL)
		return ENOMEM;
	socket_init_object(&endpoint->socket, AF_PACKET, type, protocol,
			   &packet_ops);
	endpoint->protocol = net_ntohs((uint16_t)protocol);

	/* Registers it for delivery. */
	irq = spin_lock_irqsave(&packet_registry_lock);
	endpoint->next = packet_sockets;
	packet_sockets = endpoint;
	spin_unlock_irqrestore(&packet_registry_lock, irq);
	*result = &endpoint->socket;

	/* Reports the created socket. */
	return 0;
}
