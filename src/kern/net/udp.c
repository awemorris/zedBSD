/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * UDP sockets.
 *
 * Every UDP socket is an internet socket on a registry list.  Binding and
 * connecting allocate an ephemeral port when none was chosen, subject to
 * the address reuse rules; input delivers each datagram to the best
 * matching socket, preferring a connected one.  The DHCP client is
 * allowed to broadcast from an unconfigured interface.
 *
 * An AF_INET6 socket (ws130-p003) sends and receives IPv6 datagrams, whose
 * checksum is mandatory; one that takes IPv4 too (IPV6_V6ONLY 0) gets the
 * IPv4 datagrams for its port, named by their IPv4-mapped sources, and
 * sends to an IPv4-mapped destination over IPv4.
 */

#include "kern/net/inet-socket.h"
#include "kern/net/byteorder.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/route.h"
#include "kern/kmem.h"
#include "internal.h"
#include "ipv6.h"
#include "wire.h"
#include <kern/kcrt.h>

#include <uapi/netinet.h>
#include <uapi/errno.h>

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
static struct spinlock udp_registry_lock;

static struct udp_endpoint *udp_endpoint(struct socket *socket);
static int udp_port_in_use_locked(const struct udp_endpoint *candidate, int strict);
static int udp_allocate_port_locked(struct udp_endpoint *endpoint);
static int udp_allocate_port(struct udp_endpoint *endpoint);
static int udp_bind(struct socket *socket, const struct sockaddr *address, socklen_t length);
static int udp_connect(struct socket *socket, const struct sockaddr *address, socklen_t length, unsigned io_flags);
static ssize_t udp_sendto(struct socket *socket, const void *buffer, size_t length, int flags, const struct sockaddr *address, socklen_t address_length);
static ssize_t udp_recvfrom(struct socket *socket, void *buffer, size_t length, int flags, struct sockaddr *address, socklen_t *address_length);
static int udp_getsockname(struct socket *socket, struct sockaddr *address, socklen_t *length);
static int udp_getpeername(struct socket *socket, struct sockaddr *address, socklen_t *length);
static int udp_setsockopt(struct socket *socket, int level, int option, const void *value, socklen_t length);
static int udp_getsockopt(struct socket *socket, int level, int option, void *value, socklen_t *length);
static void udp_close(struct socket *socket);
static int udp_input(struct packet_buf *packet, uint32_t source, uint32_t destination);
static int udp6_input(struct packet_buf *packet, const struct in6_addr *source, const struct in6_addr *destination);
static int udp6_connect_source(struct udp_endpoint *endpoint);
static ssize_t udp6_sendto(struct udp_endpoint *endpoint, const void *buffer, size_t length, int flags, const struct sockaddr *address, socklen_t address_length);
static ssize_t udp_send_ipv4(struct udp_endpoint *endpoint, const void *buffer, size_t length, int flags, uint32_t destination, uint16_t destination_port);
static ssize_t udp_send_ipv6(struct udp_endpoint *endpoint, const void *buffer, size_t length, const struct in6_addr *destination, unsigned scope, uint16_t destination_port);

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

/*
 * Creates a UDP socket.
 */
int
udp_socket_create(
	int protocol,
	struct socket **result)
{
	struct udp_endpoint *endpoint;
	unsigned long irq;

	/* Rejects a missing result or another protocol. */
	if (result == NULL || (protocol != 0 && protocol != IPPROTO_UDP))
		return EPROTONOSUPPORT;

	/* Allocates the endpoint as a datagram internet socket. */
	endpoint = kern_calloc(1, sizeof(*endpoint));
	if (endpoint == NULL)
		return ENOMEM;
	inet_socket_object_init(&endpoint->inet, SOCK_DGRAM, IPPROTO_UDP,
	    &udp_ops);

	/* Registers it for delivery. */
	irq = spin_lock_irqsave(&udp_registry_lock);

	endpoint->next = udp_sockets;
	udp_sockets = endpoint;

	spin_unlock_irqrestore(&udp_registry_lock, irq);

	*result = &endpoint->inet.socket;

	/* Reports the created socket. */
	return 0;
}

/*
 * Initializes the socket registry and registers UDP with IPv4.
 */
int
udp_init(
	void)
{
	int error;

	/* Starts with no sockets and the first ephemeral port. */
	udp_sockets = NULL;
	next_ephemeral = UDP_EPHEMERAL_FIRST;
	spin_init(&udp_registry_lock, LOCK_RANK_SOCKET_REGISTRY,
	    "UDP socket registry");

	/* Receives UDP datagrams from IPv4. */

	/* Reports why the registration failed. */
	error = ipv4_protocol_register(IPPROTO_UDP, udp_input);
	if (error != 0)
		return error;

	/* And from IPv6 (ws130-p003). */
	error = ipv6_protocol_register(IPPROTO_UDP, udp6_input);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Takes an ICMPv6 error about a datagram this host sent (ws130-p003): the
 * socket connected from its source port to its destination has it as its
 * error, which the next receive or send reports.  An unconnected socket
 * is not told, as on the other systems.
 */
void
udp6_error(
	const struct in6_addr *source,
	const struct in6_addr *destination,
	uint16_t source_port,
	uint16_t destination_port,
	int error)
{
	struct udp_endpoint *endpoint;
	struct udp_endpoint *found;
	unsigned long irq;
	int speaks;
	int same;
	int referenced;

	/* The connected IPv6 socket the datagram went from. */
	found = NULL;
	irq = spin_lock_irqsave(&udp_registry_lock);

	for (endpoint = udp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		speaks = inet_socket_speaks_ipv6(&endpoint->inet);
		if (!speaks ||
		    endpoint->inet.local_port != source_port ||
		    endpoint->inet.remote_port != destination_port)
			continue;
		same = in6_equal(&endpoint->inet.remote6, destination);
		if (!same)
			continue;
		same = in6_equal(&endpoint->inet.local6, source);
		if (!same)
			continue;
		referenced = socket_tryref(&endpoint->inet.socket);
		if (referenced)
			found = endpoint;
		break;
	}

	spin_unlock_irqrestore(&udp_registry_lock, irq);

	/* None: nobody is told. */
	if (found == NULL)
		return;

	/* The socket's error. */
	socket_set_error(&found->inet.socket, error);
	socket_release(&found->inet.socket);
}

/* Converts a socket to its UDP endpoint. */
static struct udp_endpoint *
udp_endpoint(
	struct socket *socket)
{
	return (struct udp_endpoint *)socket;
}

/* Tests whether another socket conflicts with a candidate's local address. */
static int
udp_port_in_use_locked(
	const struct udp_endpoint *candidate,
	int strict)
{
	const struct udp_endpoint *endpoint;
	int conflict;

	/* Compares against every other registered socket. */
	for (endpoint = udp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		if (endpoint == candidate)
			continue;

		/* A strict check ignores the address reuse options. */
		if (strict)
			conflict = inet_socket_local_conflict(&endpoint->inet, 0,
			    &candidate->inet, 0);
		else
			conflict = inet_socket_local_conflict(&endpoint->inet,
			    endpoint->inet.bind_reuse_address, &candidate->inet,
			    candidate->inet.bind_reuse_address);
		if (conflict)
			return 1;
	}

	/* Reports a free address. */
	return 0;
}

/* Binds an endpoint to a free ephemeral port; the caller holds the lock. */
static int
udp_allocate_port_locked(
	struct udp_endpoint *endpoint)
{
	unsigned attempts;
	uint16_t port;

	/* Tries each port in the ephemeral range once, wrapping around. */
	for (attempts = 0; attempts <= UDP_EPHEMERAL_LAST - UDP_EPHEMERAL_FIRST;
	     attempts++) {
		port = next_ephemeral++;
		if (next_ephemeral < UDP_EPHEMERAL_FIRST)
			next_ephemeral = UDP_EPHEMERAL_FIRST;
		endpoint->inet.local_port = port;
		if (!udp_port_in_use_locked(endpoint, 1)) {
			endpoint->inet.inet_flags |= INET_SOCKET_BOUND;
			return 0;
		}

		endpoint->inet.local_port = 0;
	}

	/* Reports an exhausted range. */
	return EADDRINUSE;
}

/* Binds an endpoint to a free ephemeral port. */
static int
udp_allocate_port(
	struct udp_endpoint *endpoint)
{
	unsigned long irq;
	int error;

	/* Allocates under the registry lock. */
	irq = spin_lock_irqsave(&udp_registry_lock);

	error = udp_allocate_port_locked(endpoint);

	spin_unlock_irqrestore(&udp_registry_lock, irq);

	/* Reports why the allocation failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Binds a socket to a local address, allocating a port when none is given. */
static int
udp_bind(
	struct socket *socket,
	const struct sockaddr *address,
	socklen_t length)
{
	struct udp_endpoint *endpoint;
	unsigned long irq;
	unsigned long socket_irq;
	int error;

	/* A socket binds only once. */
	endpoint = udp_endpoint(socket);
	if ((endpoint->inet.inet_flags & INET_SOCKET_BOUND) != 0)
		return EINVAL;

	/* Freezes the reuse option, then takes the address. */
	socket_irq = spin_lock_irqsave(&socket->lock);

	endpoint->inet.bind_reuse_address = socket->reuse_address;

	spin_unlock_irqrestore(&socket->lock, socket_irq);

	error = inet_socket_bind(&endpoint->inet, address, length);
	if (error != 0)
		return error;

	/* Allocates or validates the port, undoing the bind on failure. */
	irq = spin_lock_irqsave(&udp_registry_lock);

	if (endpoint->inet.local_port == 0)
		error = udp_allocate_port_locked(endpoint);
	else if (udp_port_in_use_locked(endpoint, 0))
		error = EADDRINUSE;
	if (error != 0) {
		endpoint->inet.local_address = 0;
		kern_memset(&endpoint->inet.local6, 0, sizeof(endpoint->inet.local6));
		endpoint->inet.mapped = 0;
		endpoint->inet.local_port = 0;
		endpoint->inet.ifindex = 0;
		endpoint->inet.bind_reuse_address = 0;
		endpoint->inet.inet_flags &= ~INET_SOCKET_BOUND;
	}

	spin_unlock_irqrestore(&udp_registry_lock, irq);

	/* Reports why the bind failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Sets the remote address, allocating a local port when none is bound. */
static int
udp_connect(
	struct socket *socket,
	const struct sockaddr *address,
	socklen_t length,
	unsigned io_flags)
{
	struct udp_endpoint *endpoint;
	unsigned long irq;
	unsigned mapped;
	int speaks;
	int error;

	(void)io_flags;

	endpoint = udp_endpoint(socket);

	/* Takes the remote address under the registry lock. */
	irq = spin_lock_irqsave(&udp_registry_lock);

	/* A source chosen for an earlier peer is chosen again for this one. */
	if ((endpoint->inet.inet_flags & INET_SOCKET_SOURCE_CHOSEN) != 0) {
		kern_memset(&endpoint->inet.local6, 0, sizeof(endpoint->inet.local6));
		endpoint->inet.inet_flags &= ~INET_SOCKET_SOURCE_CHOSEN;
	}

	/* A remote port is required, and a local one is allocated if missing. */
	mapped = endpoint->inet.mapped;
	error = inet_socket_connect(&endpoint->inet, address, length);
	if (error == 0 && endpoint->inet.remote_port == 0)
		error = EADDRNOTAVAIL;
	if (error == 0 && endpoint->inet.local_port == 0)
		error = udp_allocate_port_locked(endpoint);

	/* Undoes the connection on failure (an IPv4-mapped bind stays). */
	if (error != 0) {
		endpoint->inet.remote_address = 0;
		kern_memset(&endpoint->inet.remote6, 0, sizeof(endpoint->inet.remote6));
		endpoint->inet.scope6 = 0;
		endpoint->inet.mapped = mapped;
		endpoint->inet.remote_port = 0;
		endpoint->inet.inet_flags &= ~INET_SOCKET_CONNECTED;
	}

	spin_unlock_irqrestore(&udp_registry_lock, irq);

	/* Reports why the connect failed. */
	if (error != 0)
		return error;

	/* An IPv6 peer fixes the local address the datagrams go from (RFC 6724). */
	speaks = inet_socket_speaks_ipv6(&endpoint->inet);
	if (speaks) {
		error = udp6_connect_source(endpoint);
		if (error != 0)
			return error;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Chooses the source address of a connected IPv6 socket that is bound to
 * [::], for its peer, as getsockname then tells it.  The connection is
 * undone when there is no way to the peer.
 */
static int
udp6_connect_source(
	struct udp_endpoint *endpoint)
{
	struct net_device *device;
	struct in6_addr source;
	unsigned long irq;
	unsigned ifindex;
	int unspecified;
	int error;

	/* A socket bound to an address keeps it. */
	unspecified = in6_is_unspecified(&endpoint->inet.local6);
	if (!unspecified)
		return 0;

	/* The interface asked for: the bound one, or a link-local peer's. */
	device = NULL;
	ifindex = endpoint->inet.ifindex;
	if (ifindex == 0)
		ifindex = endpoint->inet.scope6;
	if (ifindex != 0) {
		device = net_device_find_by_index_ref(ifindex);
		if (device == NULL) {
			error = ENXIO;
			goto undo;
		}
	}

	/* The source for the peer. */
	error = ipv6_route_source(device, &endpoint->inet.remote6, &source, NULL);
	if (device != NULL)
		net_device_release(device);
	if (error != 0)
		goto undo;

	/* The socket takes datagrams to it from now on. */
	irq = spin_lock_irqsave(&udp_registry_lock);

	endpoint->inet.local6 = source;
	endpoint->inet.inet_flags |= INET_SOCKET_SOURCE_CHOSEN;

	spin_unlock_irqrestore(&udp_registry_lock, irq);

	/* Succeeded: the source chosen. */
	return 0;

undo:
	irq = spin_lock_irqsave(&udp_registry_lock);

	kern_memset(&endpoint->inet.remote6, 0, sizeof(endpoint->inet.remote6));
	endpoint->inet.scope6 = 0;
	endpoint->inet.remote_port = 0;
	endpoint->inet.inet_flags &= ~INET_SOCKET_CONNECTED;

	spin_unlock_irqrestore(&udp_registry_lock, irq);

	/* Reports why there is no source. */
	return error;
}

/* Sends a datagram to the addressed or connected destination. */
static ssize_t
udp_sendto(
	struct socket *socket,
	const void *buffer,
	size_t length,
	int flags,
	const struct sockaddr *address,
	socklen_t address_length)
{
	struct udp_endpoint *endpoint;
	struct sockaddr_in output;
	uint32_t destination;
	uint16_t destination_port;
	ssize_t sent;

	endpoint = udp_endpoint(socket);

	/* Rejects unsupported flags or a missing buffer with a length. */
	if ((flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) != 0 ||
	    (buffer == NULL && length != 0))
		return -EINVAL;

	/* An IPv6 socket takes an IPv6 destination (ws130-p003). */
	if (endpoint->inet.family == AF_INET6) {
		sent = udp6_sendto(endpoint, buffer, length, flags, address, address_length);
		if (sent < 0)
			return sent;
		return sent;
	}

	/* Takes the destination from the address, else from the connection. */
	if (address != NULL) {
		if (address_length < sizeof(output) || address->sa_family != AF_INET)
			return -EINVAL;
		kern_memcpy(&output, address, sizeof(output));
		destination = net_ntohl(output.sin_addr.s_addr);
		destination_port = net_ntohs(output.sin_port);
	} else if (endpoint->inet.inet_flags & INET_SOCKET_CONNECTED) {
		destination = endpoint->inet.remote_address;
		destination_port = endpoint->inet.remote_port;
	} else {
		return -EDESTADDRREQ;
	}

	/* Sends it over IPv4. */
	sent = udp_send_ipv4(endpoint, buffer, length, flags, destination, destination_port);
	if (sent < 0)
		return sent;

	/* Succeeded: the sent length. */
	return sent;
}

/*
 * Sends a datagram from an AF_INET6 socket: to the address given or the
 * peer, over IPv4 when it is IPv4-mapped (for a socket that takes IPv4).
 */
static ssize_t
udp6_sendto(
	struct udp_endpoint *endpoint,
	const void *buffer,
	size_t length,
	int flags,
	const struct sockaddr *address,
	socklen_t address_length)
{
	struct sockaddr_in6 output;
	struct in6_addr destination;
	uint32_t destination4;
	uint16_t destination_port;
	unsigned scope;
	ssize_t sent;
	int mapped;
	int accepts;

	/* An IPv4-mapped peer: the socket speaks IPv4 to it. */
	if (address == NULL &&
	    (endpoint->inet.inet_flags & INET_SOCKET_CONNECTED) != 0 &&
	    endpoint->inet.mapped) {
		sent = udp_send_ipv4(endpoint, buffer, length, flags, endpoint->inet.remote_address,
		    endpoint->inet.remote_port);
		if (sent < 0)
			return sent;
		return sent;
	}

	/* Takes the destination from the address, else from the connection. */
	if (address != NULL) {
		if (address_length < sizeof(output) || address->sa_family != AF_INET6)
			return -EINVAL;
		kern_memcpy(&output, address, sizeof(output));
		destination = output.sin6_addr;
		destination_port = net_ntohs(output.sin6_port);
		scope = output.sin6_scope_id;
	} else if (endpoint->inet.inet_flags & INET_SOCKET_CONNECTED) {
		destination = endpoint->inet.remote6;
		destination_port = endpoint->inet.remote_port;
		scope = endpoint->inet.scope6;
	} else {
		return -EDESTADDRREQ;
	}

	/* An IPv4-mapped destination goes over IPv4, from a socket that may speak it. */
	mapped = in6_is_v4mapped(&destination);
	if (mapped) {
		accepts = inet_socket_accepts_ipv4(&endpoint->inet);
		if (!accepts)
			return -ENETUNREACH;
		destination4 = inet_socket_unmapped(&destination);
		sent = udp_send_ipv4(endpoint, buffer, length, flags, destination4, destination_port);
		if (sent < 0)
			return sent;
		return sent;
	}

	/* An IPv6 one, from a socket that is not IPv4-mapped. */
	accepts = inet_socket_accepts_ipv6(&endpoint->inet);
	if (!accepts)
		return -ENETUNREACH;
	sent = udp_send_ipv6(endpoint, buffer, length, &destination, scope, destination_port);
	if (sent < 0)
		return sent;

	/* Succeeded: the sent length. */
	return sent;
}

/* Sends a datagram over IPv4 (an AF_INET socket's, or an IPv4-mapped AF_INET6 one's). */
static ssize_t
udp_send_ipv4(
	struct udp_endpoint *endpoint,
	const void *buffer,
	size_t length,
	int flags,
	uint32_t destination,
	uint16_t destination_port)
{
	struct net_route route;
	struct net_device *device;
	struct packet_buf *packet;
	struct udp_wire *udp;
	uint32_t source;
	uint32_t broadcast;
	uint32_t configured;
	uint16_t checksum;
	void *payload;
	int have_route;
	int error;

	/* A specific destination and port. */
	if (destination == 0 || destination_port == 0)
		return -EADDRNOTAVAIL;

	/* Allocates a local port for a socket that never bound. */
	if (endpoint->inet.local_port == 0) {
		error = udp_allocate_port(endpoint);
		if (error != 0)
			return -error;
	}

	/* Takes the bound interface, or the one the route names. */
	have_route = 0;
	if (route_lookup_ref(destination, &route) == 0)
		have_route = 1;
	if (endpoint->inet.ifindex != 0)
		device = net_device_find_by_index_ref(endpoint->inet.ifindex);
	else if (have_route)
		device = route.device;
	else
		device = NULL;
	if (endpoint->inet.ifindex == 0 && device != NULL)
		route.device = NULL;
	if (have_route)
		route_release(&route);
	if (device == NULL)
		return -ENETUNREACH;

	/* An unconfigured interface may only carry a DHCP client broadcast. */
	error = inet_interface_address(device, &source, NULL, &broadcast);
	if (error != 0) {
		configured = 0;
		(void)inet_interface_configuration(device, &configured, NULL,
		    &broadcast);
		if (configured != 0 ||
		    destination != INADDR_BROADCAST ||
		    destination_port != DHCP_SERVER_PORT ||
		    endpoint->inet.local_port != DHCP_CLIENT_PORT ||
		    endpoint->inet.ifindex == 0 ||
		    !(endpoint->inet.inet_flags & INET_SOCKET_BROADCAST) ||
		    (net_device_flags_get(device) &
		     (NET_DEVICE_UP | NET_DEVICE_RUNNING |
		     NET_DEVICE_BROADCAST)) != (NET_DEVICE_UP |
		     NET_DEVICE_RUNNING | NET_DEVICE_BROADCAST))
			goto fail;
		source = 0;
	}

	/* Broadcasting needs SO_BROADCAST. */
	if ((destination == INADDR_BROADCAST || destination == broadcast) &&
	    !(endpoint->inet.inet_flags & INET_SOCKET_BROADCAST)) {
		error = EACCES;
		goto fail;
	}

	/* Rejects a datagram that cannot fit the MTU with its headers. */
	if (length > device->mtu - sizeof(struct ipv4_wire) - sizeof(*udp)) {
		error = EMSGSIZE;
		goto fail;
	}

	/* Builds the datagram in a packet of the class its length needs. */
	packet = packet_buf_alloc_for(PACKET_BUF_DEFAULT_HEADROOM, sizeof(struct udp_wire) + length, 0, &error);
	if (packet == NULL)
		goto fail;

	udp = packet_buf_append(packet, sizeof(*udp));
	payload = packet_buf_append(packet, length);
	if (udp == NULL || payload == NULL) {
		packet_buf_free(packet);
		error = ENOBUFS;
		goto fail;
	}

	kern_memset(udp, 0, sizeof(*udp));
	if (length != 0)
		kern_memcpy(payload, buffer, length);
	wire_put16(udp->source, endpoint->inet.local_port);
	wire_put16(udp->destination, destination_port);
	wire_put16(udp->length, (uint16_t)packet->length);
	checksum = net_checksum_pseudo(source, destination, IPPROTO_UDP,
	    packet->data, packet->length);
	if (checksum == 0)
		checksum = 0xffffU;
	wire_put16(udp->checksum, checksum);

	/* Sends it: unconfigured sources broadcast, others may wait for ARP. */
	if (source == 0)
		error = ipv4_output_source(device, destination, IPPROTO_UDP, source, packet);
	else if ((flags & MSG_DONTWAIT) != 0)
		error = ipv4_output(device, destination, IPPROTO_UDP, packet);
	else
		error = ipv4_output_wait(device, destination, IPPROTO_UDP, packet);
	net_device_release(device);

	/* Reports the sent length or the error. */
	if (error == 0)
		return (ssize_t)length;
	return -error;

fail:
	net_device_release(device);

	/* Reports the failure. */
	return -error;
}

/*
 * Sends a datagram over IPv6: from the bound address or the one chosen
 * for the destination (RFC 6724), out of the bound interface or the one a
 * link-local or group destination's scope names, with the checksum IPv6
 * makes mandatory.
 */
static ssize_t
udp_send_ipv6(
	struct udp_endpoint *endpoint,
	const void *buffer,
	size_t length,
	const struct in6_addr *destination,
	unsigned scope,
	uint16_t destination_port)
{
	struct net_device *device;
	struct packet_buf *packet;
	struct udp_wire *udp;
	struct in6_addr source;
	struct in6_addr chosen;
	unsigned ifindex;
	unsigned mtu;
	uint16_t checksum;
	void *payload;
	int unspecified;
	int linklocal;
	int multicast;
	int error;

	/* A specific destination and port. */
	unspecified = in6_is_unspecified(destination);
	if (unspecified || destination_port == 0)
		return -EADDRNOTAVAIL;

	/* Allocates a local port for a socket that never bound. */
	if (endpoint->inet.local_port == 0) {
		error = udp_allocate_port(endpoint);
		if (error != 0)
			return -error;
	}

	/* The interface: the bound one, or the scope of a destination on a link. */
	ifindex = endpoint->inet.ifindex;
	linklocal = in6_is_linklocal(destination);
	multicast = in6_is_multicast(destination);
	if (ifindex == 0 &&
	    (linklocal ||
	     multicast))
		ifindex = scope;
	if (ifindex == 0 && linklocal)
		return -EINVAL;

	/* The device of that interface, held for the send. */
	device = NULL;
	if (ifindex != 0) {
		device = net_device_find_by_index_ref(ifindex);
		if (device == NULL)
			return -ENXIO;
	}

	/* The way there: the source it would be sent from, and the path MTU. */
	error = ipv6_route_source(device, destination, &chosen, &mtu);
	if (error != 0)
		goto fail;

	/* The bound address, or the chosen one. */
	source = chosen;
	unspecified = in6_is_unspecified(&endpoint->inet.local6);
	if (!unspecified)
		source = endpoint->inet.local6;

	/* Refuses a datagram that cannot fit the path with its headers (no fragments are made). */
	if (length > 0xffffU - sizeof(*udp) ||
	    length + sizeof(*udp) + IPV6_HEADER_LENGTH > mtu) {
		error = EMSGSIZE;
		goto fail;
	}

	/* Builds the datagram in a packet of the class its length needs. */
	packet = packet_buf_alloc_for(PACKET_BUF_DEFAULT_HEADROOM, sizeof(struct udp_wire) + length, 0, &error);
	if (packet == NULL)
		goto fail;

	udp = packet_buf_append(packet, sizeof(*udp));
	payload = packet_buf_append(packet, length);
	if (udp == NULL || payload == NULL) {
		packet_buf_free(packet);
		error = ENOBUFS;
		goto fail;
	}

	/* Its header, and the checksum over the IPv6 pseudo-header (0 is sent as all ones). */
	kern_memset(udp, 0, sizeof(*udp));
	if (length != 0)
		kern_memcpy(payload, buffer, length);
	wire_put16(udp->source, endpoint->inet.local_port);
	wire_put16(udp->destination, destination_port);
	wire_put16(udp->length, (uint16_t)packet->length);
	checksum = net_checksum_pseudo6(source.s6_addr, destination->s6_addr, IPPROTO_UDP, packet->data, packet->length);
	if (checksum == 0)
		checksum = 0xffffU;
	wire_put16(udp->checksum, checksum);

	/* Sends it (neighbor discovery holds it until the next hop is known). */
	error = ipv6_output(device, &source, destination, IPPROTO_UDP, 0U, packet);
	if (device != NULL)
		net_device_release(device);
	if (error != 0)
		return -error;

	/* Succeeded: the sent length. */
	return (ssize_t)length;

fail:
	if (device != NULL)
		net_device_release(device);

	/* Reports the failure. */
	return -error;
}

/* Receives one queued datagram and its source address. */
static ssize_t
udp_recvfrom(
	struct socket *socket,
	void *buffer,
	size_t length,
	int flags,
	struct sockaddr *address,
	socklen_t *address_length)
{
	struct packet_buf *packet;
	size_t copied;
	socklen_t actual;
	socklen_t output;
	ssize_t result;
	int error;

	/* Rejects unsupported flags. */
	if ((flags & ~(MSG_DONTWAIT | MSG_TRUNC)) != 0)
		return -EOPNOTSUPP;

	/* Takes the next queued datagram. */
	error = socket_dequeue_packet(socket, flags & MSG_DONTWAIT, &packet);
	if (error != 0)
		return -error;

	/* Copies as much of the datagram as fits. */
	if (length < packet->length)
		copied = length;
	else
		copied = packet->length;
	if (copied != 0)
		kern_memcpy(buffer, packet->data, copied);

	/* Copies the source address, reporting its full length. */
	if (address != NULL && address_length != NULL) {
		actual = packet->source_length;
		if (*address_length < actual)
			output = *address_length;
		else
			output = actual;
		kern_memcpy(address, packet->source_address, output);
		*address_length = actual;
	}

	/* With MSG_TRUNC the full datagram length is reported instead. */
	if ((flags & MSG_TRUNC) != 0)
		result = (ssize_t)packet->length;
	else
		result = (ssize_t)copied;
	packet_buf_free(packet);

	/* Reports the received length. */
	return result;
}

/* Reports the socket's local address. */
static int
udp_getsockname(
	struct socket *socket,
	struct sockaddr *address,
	socklen_t *length)
{
	struct udp_endpoint *endpoint;
	int error;

	endpoint = udp_endpoint(socket);

	/* Reports why the lookup failed. */
	error = inet_socket_getsockname(&endpoint->inet, address, length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports the socket's remote address. */
static int
udp_getpeername(
	struct socket *socket,
	struct sockaddr *address,
	socklen_t *length)
{
	struct udp_endpoint *endpoint;
	int error;

	endpoint = udp_endpoint(socket);

	/* Reports why the lookup failed. */
	error = inet_socket_getpeername(&endpoint->inet, address, length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Sets SO_BROADCAST here and forwards every other option. */
static int
udp_setsockopt(
	struct socket *socket,
	int level,
	int option,
	const void *value,
	socklen_t length)
{
	struct udp_endpoint *endpoint;
	int enabled;
	int error;

	endpoint = udp_endpoint(socket);

	/* SO_BROADCAST is kept in the endpoint flags. */
	if (level == SOL_SOCKET && option == SO_BROADCAST) {
		if (value == NULL || length != sizeof(enabled))
			return EINVAL;
		kern_memcpy(&enabled, value, sizeof(enabled));
		if (enabled)
			endpoint->inet.inet_flags |= INET_SOCKET_BROADCAST;
		else
			endpoint->inet.inet_flags &= ~INET_SOCKET_BROADCAST;
		return 0;
	}

	/* Forwards the other options to the internet socket layer. */

	/* Reports why the option failed. */
	error = inet_socket_setsockopt(&endpoint->inet, level, option, value,
	    length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reads SO_BROADCAST here and forwards every other option. */
static int
udp_getsockopt(
	struct socket *socket,
	int level,
	int option,
	void *value,
	socklen_t *length)
{
	struct udp_endpoint *endpoint;
	int enabled;
	int error;

	/* SO_BROADCAST is read from the endpoint flags. */
	endpoint = udp_endpoint(socket);
	if (level == SOL_SOCKET && option == SO_BROADCAST) {
		if (value == NULL || length == NULL || *length < sizeof(enabled))
			return EINVAL;
		enabled = (endpoint->inet.inet_flags & INET_SOCKET_BROADCAST) != 0;
		kern_memcpy(value, &enabled, sizeof(enabled));
		*length = sizeof(enabled);
		return 0;
	}

	/* Forwards the other options to the internet socket layer. */

	/* Reports why the option failed. */
	error = inet_socket_getsockopt(&endpoint->inet, level, option, value,
	    length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Unregisters and frees a UDP socket. */
static void
udp_close(
	struct socket *socket)
{
	struct udp_endpoint *endpoint;
	struct udp_endpoint **link;
	unsigned long irq;

	endpoint = udp_endpoint(socket);

	/* Unlinks the endpoint from the registry. */
	irq = spin_lock_irqsave(&udp_registry_lock);

	for (link = &udp_sockets; *link != NULL; link = &(*link)->next) {
		if (*link == endpoint) {
			*link = endpoint->next;
			break;
		}
	}

	spin_unlock_irqrestore(&udp_registry_lock, irq);

	kern_free(endpoint);
}

/* Receives a datagram and queues it on the best matching socket. */
static int
udp_input(
	struct packet_buf *packet,
	uint32_t source,
	uint32_t destination)
{
	const struct udp_wire *udp;
	struct udp_endpoint *endpoint;
	struct udp_endpoint *best;
	unsigned long irq;
	uint16_t source_port;
	uint16_t destination_port;
	uint16_t udp_length;
	uint16_t checksum;
	int accepts;
	int error;

	best = NULL;

	/* Drops a datagram without a complete header. */
	if (packet == NULL || packet->length < sizeof(*udp)) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Drops a datagram with a bad length or a failing checksum. */
	udp = (const struct udp_wire *)packet->data;
	udp_length = wire_get16(udp->length);
	if (udp_length < sizeof(*udp) || udp_length > packet->length) {
		packet_buf_free(packet);
		return EINVAL;
	}

	checksum = wire_get16(udp->checksum);
	if (checksum != 0 && (packet->flags & PACKET_BUF_RX_CSUM_L4) == 0 &&
	    net_checksum_pseudo(source, destination, IPPROTO_UDP,
	    packet->data, udp_length) != 0) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Prefers a socket connected to the sender over a merely bound one. */
	source_port = wire_get16(udp->source);
	destination_port = wire_get16(udp->destination);
	irq = spin_lock_irqsave(&udp_registry_lock);

	for (endpoint = udp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		accepts = inet_socket_accepts_ipv4(&endpoint->inet);
		if (!accepts)
			continue;
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

	if (best != NULL && !socket_tryref(&best->inet.socket))
		best = NULL;

	spin_unlock_irqrestore(&udp_registry_lock, irq);

	/* Drops a datagram nobody listens for. */
	if (best == NULL) {
		packet_buf_free(packet);
		return 0;
	}

	/* Strips the header and trailing padding. */
	(void)packet_buf_trim(packet, udp_length);
	if (packet_buf_pull(packet, sizeof(*udp)) == NULL) {
		socket_release(&best->inet.socket);
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Names the sender in the packet's source address, IPv4-mapped for an IPv6 socket. */
	inet_socket_peer_name(&best->inet, source, NULL, 0, source_port, packet->source_address, &packet->source_length);

	/* Queues the datagram on the socket. */
	error = socket_enqueue_packet(&best->inet.socket, packet);
	socket_release(&best->inet.socket);

	/* Reports why the queueing failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Receives an IPv6 datagram and queues it on the best matching socket:
 * one that takes IPv6, on the destination port and address (or [::]), on
 * the interface it came in on when it is bound to one, preferring one
 * connected to the sender.  A datagram without a checksum is dropped.
 */
static int
udp6_input(
	struct packet_buf *packet,
	const struct in6_addr *source,
	const struct in6_addr *destination)
{
	const struct udp_wire *udp;
	struct udp_endpoint *endpoint;
	struct udp_endpoint *best;
	unsigned long irq;
	unsigned arrived;
	unsigned device_flags;
	uint16_t source_port;
	uint16_t destination_port;
	uint16_t udp_length;
	uint16_t checksum;
	void *pulled;
	int accepts;
	int unspecified;
	int same;
	int looped;
	int referenced;
	int error;

	best = NULL;

	/* Drops a datagram without a complete header. */
	if (packet == NULL || packet->length < sizeof(*udp)) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Drops a datagram with a bad length. */
	udp = (const struct udp_wire *)packet->data;
	udp_length = wire_get16(udp->length);
	if (udp_length < sizeof(*udp) || udp_length > packet->length) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Drops one without the checksum (RFC 8200 section 8.1) or with a failing one. */
	checksum = wire_get16(udp->checksum);
	if (checksum == 0) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Checks the checksum, unless the device found it right. */
	checksum = 0;
	if ((packet->flags & PACKET_BUF_RX_CSUM_L4) == 0) {
		checksum = net_checksum_pseudo6(source->s6_addr, destination->s6_addr, IPPROTO_UDP, packet->data,
		    udp_length);
	}

	if (checksum != 0) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* The interface it came in on (round the loopback device: any). */
	arrived = 0;
	looped = 1;
	if (packet->device != NULL) {
		arrived = packet->device->ifindex;
		device_flags = net_device_flags_get(packet->device);
		looped = 0;
		if ((device_flags & NET_DEVICE_LOOPBACK) != 0U)
			looped = 1;
	}

	/* Prefers a socket connected to the sender over a merely bound one. */
	source_port = wire_get16(udp->source);
	destination_port = wire_get16(udp->destination);
	irq = spin_lock_irqsave(&udp_registry_lock);

	for (endpoint = udp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		accepts = inet_socket_accepts_ipv6(&endpoint->inet);
		if (!accepts || endpoint->inet.local_port != destination_port)
			continue;

		/* On the destination address or [::], and on the interface it came in on. */
		unspecified = in6_is_unspecified(&endpoint->inet.local6);
		same = in6_equal(&endpoint->inet.local6, destination);
		if (!unspecified && !same)
			continue;
		if (endpoint->inet.ifindex != 0 &&
		    !looped &&
		    endpoint->inet.ifindex != arrived)
			continue;

		/* A connected socket only from its peer, and before the bound ones. */
		if (endpoint->inet.inet_flags & INET_SOCKET_CONNECTED) {
			same = in6_equal(&endpoint->inet.remote6, source);
			if (!same || endpoint->inet.remote_port != source_port)
				continue;
			best = endpoint;
			break;
		}

		if (best == NULL)
			best = endpoint;
	}

	/* The socket held for the queueing, unless it is closing. */
	if (best != NULL) {
		referenced = socket_tryref(&best->inet.socket);
		if (!referenced)
			best = NULL;
	}

	spin_unlock_irqrestore(&udp_registry_lock, irq);

	/* Drops a datagram nobody listens for. */
	if (best == NULL) {
		packet_buf_free(packet);
		return 0;
	}

	/* Strips the header and trailing padding. */
	(void)packet_buf_trim(packet, udp_length);
	pulled = packet_buf_pull(packet, sizeof(*udp));
	if (pulled == NULL) {
		socket_release(&best->inet.socket);
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Names the sender, with the interface of a link-local one. */
	inet_socket_peer_name(&best->inet, 0, source, arrived, source_port, packet->source_address, &packet->source_length);

	/* Queues the datagram on the socket. */
	error = socket_enqueue_packet(&best->inet.socket, packet);
	socket_release(&best->inet.socket);

	/* Reports why the queueing failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
