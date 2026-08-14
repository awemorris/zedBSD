/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/inet-socket.h"
#include "kern/net/byteorder.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/route.h"
#include "kern/kmem.h"
#include "internal.h"
#include "wire.h"

#include <zedbsd/netinet.h>
#include <errno.h>
#include <string.h>

#define UDP_EPHEMERAL_FIRST 49152U
#define UDP_EPHEMERAL_LAST  65535U
#define DHCP_SERVER_PORT 67U
#define DHCP_CLIENT_PORT 68U

struct udp_endpoint {
	struct inet_socket inet;
	struct udp_endpoint *next;
};

static struct udp_endpoint *udp_sockets;
static uint16_t next_ephemeral;

static struct udp_endpoint *udp_endpoint(struct socket *socket)
{
	return (struct udp_endpoint *)socket;
}

static int
udp_port_in_use(const struct udp_endpoint *skip, uint32_t address,
		uint16_t port)
{
	const struct udp_endpoint *endpoint;

	for (endpoint = udp_sockets; endpoint != NULL; endpoint = endpoint->next)
		if (endpoint != skip && endpoint->inet.local_port == port &&
		    (endpoint->inet.local_address == 0 || address == 0 ||
		     endpoint->inet.local_address == address))
			return 1;
	return 0;
}

static int
udp_allocate_port(struct udp_endpoint *endpoint)
{
	unsigned attempts;

	for (attempts = 0; attempts <= UDP_EPHEMERAL_LAST - UDP_EPHEMERAL_FIRST;
	     attempts++) {
		uint16_t port = next_ephemeral++;
		if (next_ephemeral < UDP_EPHEMERAL_FIRST)
			next_ephemeral = UDP_EPHEMERAL_FIRST;
		if (!udp_port_in_use(endpoint, endpoint->inet.local_address, port)) {
			endpoint->inet.local_port = port;
			endpoint->inet.inet_flags |= INET_SOCKET_BOUND;
			return 0;
		}
	}
	return EADDRINUSE;
}

static int
udp_bind(struct socket *socket, const struct sockaddr *address,
	 socklen_t length)
{
	struct udp_endpoint *endpoint = udp_endpoint(socket);
	int error = inet_socket_bind(&endpoint->inet, address, length);

	if (error != 0)
		return error;
	if (endpoint->inet.local_port == 0)
		return udp_allocate_port(endpoint);
	if (udp_port_in_use(endpoint, endpoint->inet.local_address,
	    endpoint->inet.local_port))
		return EADDRINUSE;
	return 0;
}

static int
udp_connect(struct socket *socket, const struct sockaddr *address,
	    socklen_t length)
{
	struct udp_endpoint *endpoint = udp_endpoint(socket);
	int error = inet_socket_connect(&endpoint->inet, address, length);

	if (error == 0 && endpoint->inet.remote_port == 0)
		error = EADDRNOTAVAIL;
	if (error == 0 && endpoint->inet.local_port == 0)
		error = udp_allocate_port(endpoint);
	return error;
}

static ssize_t
udp_sendto(struct socket *socket, const void *buffer, size_t length, int flags,
	   const struct sockaddr *address, socklen_t address_length)
{
	struct udp_endpoint *endpoint = udp_endpoint(socket);
	struct sockaddr_in output;
	const struct net_route *route;
	struct net_device *device;
	struct packet_buf *packet;
	struct udp_wire *udp;
	uint32_t destination, source, broadcast;
	uint16_t destination_port, checksum;
	void *payload;
	int error;

	if (flags != 0 || (buffer == NULL && length != 0))
		return -EINVAL;
	if (address != NULL) {
		if (address_length < sizeof(output) || address->sa_family != AF_INET)
			return -EINVAL;
		memcpy(&output, address, sizeof(output));
		destination = net_ntohl(output.sin_addr.s_addr);
		destination_port = net_ntohs(output.sin_port);
	} else if (endpoint->inet.inet_flags & INET_SOCKET_CONNECTED) {
		destination = endpoint->inet.remote_address;
		destination_port = endpoint->inet.remote_port;
	} else {
		return -EDESTADDRREQ;
	}
	if (destination == 0 || destination_port == 0)
		return -EADDRNOTAVAIL;
	if (endpoint->inet.local_port == 0 &&
	    (error = udp_allocate_port(endpoint)) != 0)
		return -error;
	route = route_lookup(destination);
	device = endpoint->inet.ifindex != 0 ?
	    net_device_find_by_index(endpoint->inet.ifindex) :
	    (route != NULL ? route->device : NULL);
	if (device == NULL)
		return -ENETUNREACH;
	error = inet_interface_address(device, &source, NULL, &broadcast);
	if (error != 0) {
		uint32_t configured = 0;
		(void)inet_interface_configuration(device, &configured, NULL,
		    &broadcast);
		if (configured != 0 || destination != INADDR_BROADCAST ||
		    destination_port != DHCP_SERVER_PORT ||
		    endpoint->inet.local_port != DHCP_CLIENT_PORT ||
		    endpoint->inet.ifindex == 0 ||
		    !(endpoint->inet.inet_flags & INET_SOCKET_BROADCAST) ||
		    (device->flags & (NET_DEVICE_UP | NET_DEVICE_RUNNING |
		     NET_DEVICE_BROADCAST)) != (NET_DEVICE_UP |
		     NET_DEVICE_RUNNING | NET_DEVICE_BROADCAST))
			return -error;
		source = 0;
	}
	if ((destination == INADDR_BROADCAST || destination == broadcast) &&
	    !(endpoint->inet.inet_flags & INET_SOCKET_BROADCAST))
		return -EACCES;
	if (length > device->mtu - sizeof(struct ipv4_wire) - sizeof(*udp))
		return -EMSGSIZE;
	packet = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
	if (packet == NULL)
		return -ENOBUFS;
	udp = packet_buf_append(packet, sizeof(*udp));
	payload = packet_buf_append(packet, length);
	if (udp == NULL || payload == NULL) {
		packet_buf_free(packet);
		return -ENOBUFS;
	}
	memset(udp, 0, sizeof(*udp));
	if (length != 0)
		memcpy(payload, buffer, length);
	wire_put16(udp->source, endpoint->inet.local_port);
	wire_put16(udp->destination, destination_port);
	wire_put16(udp->length, (uint16_t)packet->length);
	checksum = net_checksum_pseudo(source, destination, IPPROTO_UDP,
	    packet->data, packet->length);
	if (checksum == 0)
		checksum = 0xffffU;
	wire_put16(udp->checksum, checksum);
	error = source == 0 ?
	    ipv4_output_source(device, destination, IPPROTO_UDP, source, packet) :
	    ipv4_output(device, destination, IPPROTO_UDP, packet);
	return error == 0 ? (ssize_t)length : -error;
}

static ssize_t
udp_recvfrom(struct socket *socket, void *buffer, size_t length, int flags,
	     struct sockaddr *address, socklen_t *address_length)
{
	struct packet_buf *packet;
	size_t copied;
	int error = socket_dequeue_packet(socket, flags, &packet);

	if (error != 0)
		return -error;
	copied = length < packet->length ? length : packet->length;
	if (copied != 0)
		memcpy(buffer, packet->data, copied);
	if (address != NULL && address_length != NULL) {
		socklen_t actual = packet->source_length;
		socklen_t output = *address_length < actual ? *address_length : actual;
		memcpy(address, packet->source_address, output);
		*address_length = actual;
	}
	packet_buf_free(packet);
	return (ssize_t)copied;
}

static int udp_getsockname(struct socket *socket, struct sockaddr *address,
			   socklen_t *length)
{
	return inet_socket_getsockname(&udp_endpoint(socket)->inet, address, length);
}

static int udp_getpeername(struct socket *socket, struct sockaddr *address,
			   socklen_t *length)
{
	return inet_socket_getpeername(&udp_endpoint(socket)->inet, address, length);
}

static int
udp_setsockopt(struct socket *socket, int level, int option, const void *value,
	       socklen_t length)
{
	struct udp_endpoint *endpoint = udp_endpoint(socket);
	int enabled;

	if (level == SOL_SOCKET && option == SO_BROADCAST) {
		if (value == NULL || length != sizeof(enabled))
			return EINVAL;
		memcpy(&enabled, value, sizeof(enabled));
		if (enabled)
			endpoint->inet.inet_flags |= INET_SOCKET_BROADCAST;
		else
			endpoint->inet.inet_flags &= ~INET_SOCKET_BROADCAST;
		return 0;
	}
	return inet_socket_setsockopt(&endpoint->inet, level, option, value,
	    length);
}

static int
udp_getsockopt(struct socket *socket, int level, int option, void *value,
	       socklen_t *length)
{
	struct udp_endpoint *endpoint = udp_endpoint(socket);
	int enabled;

	if (level == SOL_SOCKET && option == SO_BROADCAST) {
		if (value == NULL || length == NULL || *length < sizeof(enabled))
			return EINVAL;
		enabled = (endpoint->inet.inet_flags & INET_SOCKET_BROADCAST) != 0;
		memcpy(value, &enabled, sizeof(enabled));
		*length = sizeof(enabled);
		return 0;
	}
	return inet_socket_getsockopt(&endpoint->inet, level, option, value,
	    length);
}

static void
udp_close(struct socket *socket)
{
	struct udp_endpoint *endpoint = udp_endpoint(socket);
	struct udp_endpoint **link;

	for (link = &udp_sockets; *link != NULL; link = &(*link)->next)
		if (*link == endpoint) {
			*link = endpoint->next;
			break;
		}
	kern_free(endpoint);
}

static const struct socket_ops udp_ops = {
	.bind = udp_bind,
	.connect = udp_connect,
	.sendto = udp_sendto,
	.recvfrom = udp_recvfrom,
	.getsockname = udp_getsockname,
	.getpeername = udp_getpeername,
	.setsockopt = udp_setsockopt,
	.getsockopt = udp_getsockopt,
	.ioctl = inet_socket_ioctl,
	.close = udp_close,
};

int
udp_socket_create(int protocol, struct socket **result)
{
	struct udp_endpoint *endpoint;

	if (result == NULL || (protocol != 0 && protocol != IPPROTO_UDP))
		return EPROTONOSUPPORT;
	endpoint = kern_calloc(1, sizeof(*endpoint));
	if (endpoint == NULL)
		return ENOMEM;
	inet_socket_object_init(&endpoint->inet, SOCK_DGRAM, IPPROTO_UDP,
	    &udp_ops);
	endpoint->next = udp_sockets;
	udp_sockets = endpoint;
	*result = &endpoint->inet.socket;
	return 0;
}

static int
udp_input(struct packet_buf *packet, uint32_t source, uint32_t destination)
{
	const struct udp_wire *udp;
	struct udp_endpoint *endpoint, *best = NULL;
	uint16_t source_port, destination_port, udp_length, checksum;

	if (packet == NULL || packet->length < sizeof(*udp)) {
		packet_buf_free(packet);
		return EINVAL;
	}
	udp = (const struct udp_wire *)packet->data;
	udp_length = wire_get16(udp->length);
	if (udp_length < sizeof(*udp) || udp_length > packet->length) {
		packet_buf_free(packet);
		return EINVAL;
	}
	checksum = wire_get16(udp->checksum);
	if (checksum != 0 && net_checksum_pseudo(source, destination,
	    IPPROTO_UDP, packet->data, udp_length) != 0) {
		packet_buf_free(packet);
		return EINVAL;
	}
	source_port = wire_get16(udp->source);
	destination_port = wire_get16(udp->destination);
	for (endpoint = udp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		if (endpoint->inet.local_port != destination_port ||
		    (endpoint->inet.local_address != 0 &&
		     endpoint->inet.local_address != destination))
			continue;
		if (endpoint->inet.inet_flags & INET_SOCKET_CONNECTED) {
			if (endpoint->inet.remote_address != source ||
			    endpoint->inet.remote_port != source_port)
				continue;
			best = endpoint;
			break;
		}
		if (best == NULL)
			best = endpoint;
	}
	if (best == NULL) {
		packet_buf_free(packet);
		return 0;
	}
	(void)packet_buf_trim(packet, udp_length);
	if (packet_buf_pull(packet, sizeof(*udp)) == NULL) {
		packet_buf_free(packet);
		return EINVAL;
	}
	{
		struct sockaddr_in address;
		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_port = net_htons(source_port);
		address.sin_addr.s_addr = net_htonl(source);
		memcpy(packet->source_address, &address, sizeof(address));
		packet->source_length = sizeof(address);
	}
	return socket_enqueue_packet(&best->inet.socket, packet);
}

int
udp_init(void)
{
	udp_sockets = NULL;
	next_ephemeral = UDP_EPHEMERAL_FIRST;
	return ipv4_protocol_register(IPPROTO_UDP, udp_input);
}
