/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The TCP socket implementation.
 *
 * Each socket keeps one outstanding reliable segment, retransmitted by
 * the network worker's timer with exponential backoff until it is
 * acknowledged or the attempt is abandoned.  Listeners hold half-open
 * children until their handshake completes and then queue them for
 * accept.  Payload is delivered in order only; anything else is dropped
 * and re-acknowledged.
 */

#include "kern/net/tcp-socket.h"
#include "kern/net/byteorder.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/route.h"
#include "kern/clock.h"
#include "kern/kmem.h"
#include "kern/poll.h"
#include "kern/sched.h"
#include "kern/signal.h"
#include "kern/thread.h"
#include "internal.h"
#include "wire.h"

#include <zedbsd/netinet.h>
#include <errno.h>
#include <string.h>

#define TCP_FIN 0x01U
#define TCP_SYN 0x02U
#define TCP_RST 0x04U
#define TCP_PSH 0x08U
#define TCP_ACK 0x10U
#define TCP_DEFAULT_WINDOW 4096U
#define TCP_MSS 1024U
#define TCP_EPHEMERAL_FIRST 49152U
#define TCP_INITIAL_RTO 100U
#define TCP_RETRANSMIT_MAX 5U
#define TCP_LISTEN_BACKLOG_MAX 16U

struct tcp_endpoint {
	struct tcp_socket tcp;
	struct tcp_endpoint *next;
};

static struct tcp_endpoint *tcp_sockets;
static uint16_t next_ephemeral;
static struct spinlock tcp_registry_lock;

static struct tcp_endpoint * tcp_endpoint(struct socket *socket);
static void tcp_forget_peer(struct tcp_endpoint *endpoint);
static void tcp_listener_remove(struct tcp_endpoint *child);
static void tcp_listener_established(struct tcp_endpoint *child);
static int tcp_port_in_use(const struct tcp_endpoint *candidate, int strict);
static int tcp_allocate_port_locked(struct tcp_endpoint *endpoint);
static int tcp_route(struct tcp_endpoint *endpoint, struct net_device **device, uint32_t *source);
static int tcp_send_segment_at(struct tcp_endpoint *endpoint, uint32_t sequence, uint8_t flags, const void *data, size_t length);
static int tcp_allocate_port(struct tcp_endpoint *endpoint);
static int tcp_send_segment(struct tcp_endpoint *endpoint, uint8_t flags, const void *data, size_t length);
static void tcp_retransmit_reset(struct tcp_endpoint *endpoint, struct packet_buf **packet);
static void tcp_retransmit_clear(struct tcp_endpoint *endpoint);
static struct packet_buf * tcp_connect_cancel_locked(struct tcp_endpoint *endpoint, uint32_t generation);
static int tcp_send_reliable(struct tcp_endpoint *endpoint, uint8_t flags, const void *data, size_t length);
static int tcp_bind(struct socket *socket, const struct sockaddr *address, socklen_t length);
static int tcp_listen(struct socket *socket, int backlog);
static int tcp_accept(struct socket *socket, struct socket **result, struct sockaddr *address, socklen_t *length, unsigned io_flags);
static int tcp_connect(struct socket *socket, const struct sockaddr *address, socklen_t length, unsigned io_flags);
static ssize_t tcp_sendto(struct socket *socket, const void *buffer, size_t length, int flags, const struct sockaddr *address, socklen_t address_length);
static ssize_t tcp_recvfrom(struct socket *socket, void *buffer, size_t length, int flags, struct sockaddr *address, socklen_t *address_length);
static int tcp_shutdown(struct socket *socket, int how);
static int tcp_getsockname(struct socket *socket, struct sockaddr *address, socklen_t *length);
static int tcp_getpeername(struct socket *socket, struct sockaddr *address, socklen_t *length);
static void tcp_close(struct socket *socket);
static int tcp_poll(struct socket *socket, short events, short *revents);
static struct tcp_endpoint * tcp_lookup(uint32_t source, uint32_t destination, uint16_t source_port, uint16_t destination_port);
static struct tcp_endpoint * tcp_passive_syn(struct tcp_endpoint *listener, uint32_t source, uint32_t destination, uint16_t source_port, uint32_t sequence);
static int tcp_input(struct packet_buf *packet, uint32_t source, uint32_t destination);

static const struct socket_ops tcp_ops = {
	.bind = tcp_bind,
	.connect = tcp_connect,
	.listen = tcp_listen,
	.accept = tcp_accept,
	.sendto = tcp_sendto,
	.recvfrom = tcp_recvfrom,
	.shutdown = tcp_shutdown,
	.getsockname = tcp_getsockname,
	.getpeername = tcp_getpeername,
	.ioctl = inet_socket_ioctl,
	.poll = tcp_poll,
	.close = tcp_close,
};

/*
 * Creates a closed TCP socket and registers it.
 */
int
tcp_socket_create(
	int protocol,
	struct socket **result)
{
	struct tcp_endpoint *endpoint;
	unsigned long irq;

	/* Rejects a missing result or a protocol other than TCP. */
	if (result == NULL || (protocol != 0 && protocol != IPPROTO_TCP))
		return EPROTONOSUPPORT;

	/* Allocates the endpoint and links it into the registry. */
	endpoint = kern_calloc(1, sizeof(*endpoint));
	if (endpoint == NULL)
		return ENOMEM;
	inet_socket_object_init(&endpoint->tcp.inet, SOCK_STREAM, IPPROTO_TCP,
	    &tcp_ops);
	endpoint->tcp.state = TCP_CLOSED;
	irq = spin_lock_irqsave(&tcp_registry_lock);

	endpoint->next = tcp_sockets;
	tcp_sockets = endpoint;

	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	*result = &endpoint->tcp.inet.socket;

	/* Reports the created socket. */
	return 0;
}

/*
 * Initializes the TCP registry and registers the input handler.
 */
int
tcp_init(
	void)
{
	int error;

	tcp_sockets = NULL;
	next_ephemeral = TCP_EPHEMERAL_FIRST;
	spin_init(&tcp_registry_lock, LOCK_RANK_SOCKET_REGISTRY,
	    "TCP socket registry");

	/* Reports why the registration failed. */
	error = ipv4_protocol_register(IPPROTO_TCP, tcp_input);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Retransmits expired segments, abandoning attempts that failed too often.
 */
void
tcp_timer_run(
	void)
{
	struct tcp_endpoint *endpoint;
	struct tcp_endpoint *snapshot[SOCKET_MAX];
	unsigned count;
	unsigned index;
	unsigned long irq;
	uint64_t now;
	struct socket *socket;
	struct packet_buf *discard;
	uint8_t payload[TCP_MSS];
	uint8_t flags;
	uint32_t sequence;
	size_t length;
	uint64_t delay;
	unsigned shift;
	int error;
	int failed;

	count = 0;
	now = sched_ticks();

	/* Snapshots the referenced sockets so no registry lock is held during I/O. */
	irq = spin_lock_irqsave(&tcp_registry_lock);

	for (endpoint = tcp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		if (count < SOCKET_MAX && socket_tryref(&endpoint->tcp.inet.socket))
			snapshot[count++] = endpoint;
	}

	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	/* Handles each socket whose retransmission deadline passed. */
	for (index = 0; index < count; index++) {
		discard = NULL;
		endpoint = snapshot[index];
		socket = &endpoint->tcp.inet.socket;
		irq = spin_lock_irqsave(&socket->lock);
		if (endpoint->tcp.retransmit == NULL ||
		    endpoint->tcp.retransmit_deadline > now) {
			spin_unlock_irqrestore(&socket->lock, irq);
			socket_release(socket);
			continue;
		}

		/* Too many retransmissions abandon the attempt. */
		if (endpoint->tcp.retransmit_count >= TCP_RETRANSMIT_MAX) {
			tcp_retransmit_reset(endpoint, &discard);
			endpoint->tcp.state = TCP_CLOSED;
			endpoint->tcp.active_connect_generation = 0;
			endpoint->tcp.connect_wait_deadline = 0;
			tcp_forget_peer(endpoint);
			spin_unlock_irqrestore(&socket->lock, irq);
			packet_buf_free(discard);
			socket_set_error(socket, ETIMEDOUT);
			if (endpoint->tcp.listener != NULL) {
				tcp_listener_remove(endpoint);
				/* Queue ownership. */
				socket_release(socket);
				/* Timer snapshot ownership. */
				socket_release(socket);
				continue;
			}

			socket_wake_connect(socket);
			socket_wake_receive(socket);
			socket_release(socket);
			continue;
		}

		/* Copies the segment out so it can be sent unlocked. */
		sequence = endpoint->tcp.retransmit_sequence;
		flags = endpoint->tcp.retransmit_flags;
		length = endpoint->tcp.retransmit->length;
		if (length > sizeof(payload)) {
			tcp_retransmit_reset(endpoint, &discard);
			endpoint->tcp.state = TCP_CLOSED;
			endpoint->tcp.active_connect_generation = 0;
			endpoint->tcp.connect_wait_deadline = 0;
			tcp_forget_peer(endpoint);
			spin_unlock_irqrestore(&socket->lock, irq);
			packet_buf_free(discard);
			socket_set_error(socket, EIO);
			socket_release(socket);
			continue;
		}

		if (length != 0)
			memcpy(payload, endpoint->tcp.retransmit->data, length);

		/* Backs the deadline off exponentially, capped at eight times. */
		endpoint->tcp.retransmit_count++;
		shift = endpoint->tcp.retransmit_count;
		if (shift > 3U)
			shift = 3U;
		delay = TCP_INITIAL_RTO << shift;
		endpoint->tcp.retransmit_deadline = now + delay;
		spin_unlock_irqrestore(&socket->lock, irq);

		/* A hard send error abandons the attempt the segment belonged to. */
		error = tcp_send_segment_at(endpoint, sequence, flags, payload, length);
		if (error != 0 && error != EAGAIN && error != ENOBUFS) {
			failed = 0;
			irq = spin_lock_irqsave(&socket->lock);
			if (endpoint->tcp.retransmit != NULL &&
			    endpoint->tcp.retransmit_sequence == sequence) {
				tcp_retransmit_reset(endpoint, &discard);
				endpoint->tcp.state = TCP_CLOSED;
				endpoint->tcp.active_connect_generation = 0;
				endpoint->tcp.connect_wait_deadline = 0;
				tcp_forget_peer(endpoint);
				failed = 1;
			}

			spin_unlock_irqrestore(&socket->lock, irq);
			packet_buf_free(discard);
			if (!failed) {
				socket_release(socket);
				continue;
			}

			socket_set_error(socket, error);
			if (endpoint->tcp.listener != NULL) {
				tcp_listener_remove(endpoint);
				/* Queue ownership. */
				socket_release(socket);
				/* Timer snapshot ownership. */
				socket_release(socket);
				continue;
			}

			socket_wake_connect(socket);
			socket_release(socket);
			continue;
		}

		socket_release(socket);
	}
}

/*
 * Reports the earliest retransmission deadline, or zero without one.
 */
uint64_t
tcp_timer_next_deadline(
	void)
{
	struct tcp_endpoint *endpoint;
	uint64_t deadline;
	unsigned long irq;
	uint64_t candidate;
	unsigned long socket_irq;

	deadline = 0;
	irq = spin_lock_irqsave(&tcp_registry_lock);

	/* Takes the minimum over every socket with a segment outstanding. */
	for (endpoint = tcp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		socket_irq = spin_lock_irqsave(
		    &endpoint->tcp.inet.socket.lock);
		if (endpoint->tcp.retransmit != NULL)
			candidate = endpoint->tcp.retransmit_deadline;
		else
			candidate = 0;
		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		if (candidate != 0 && (deadline == 0 || candidate < deadline))
			deadline = candidate;
	}

	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	/* Reports the earliest deadline. */
	return deadline;
}

/* Converts a socket to its endpoint. */
static struct tcp_endpoint *
tcp_endpoint(
	struct socket *socket)
{
	return (struct tcp_endpoint *)socket;
}

/* Clears the peer address of an endpoint. */
static void
tcp_forget_peer(
	struct tcp_endpoint *endpoint)
{
	endpoint->tcp.inet.remote_address = 0;
	endpoint->tcp.inet.remote_port = 0;
	endpoint->tcp.inet.inet_flags &= ~INET_SOCKET_CONNECTED;
}

/* Removes a child from its listener's half-open or accept queue. */
static void
tcp_listener_remove(
	struct tcp_endpoint *child)
{
	struct tcp_endpoint *listener;
	struct tcp_socket **link;
	struct tcp_socket *previous;
	unsigned long irq;

	previous = NULL;

	/* Ignores a child without a listener. */
	if (child == NULL || child->tcp.listener == NULL)
		return;
	listener = (struct tcp_endpoint *)child->tcp.listener;
	irq = spin_lock_irqsave(&listener->tcp.inet.socket.lock);

	/* Searches the half-open queue first. */
	for (link = &listener->tcp.half_open_head; *link != NULL;
	     link = &(*link)->queue_next) {
		if (*link == &child->tcp) {
			*link = child->tcp.queue_next;
			if (listener->tcp.half_open_count != 0)
				listener->tcp.half_open_count--;
			child->tcp.queue_next = NULL;
			child->tcp.listener = NULL;
			spin_unlock_irqrestore(&listener->tcp.inet.socket.lock, irq);
			return;
		}
	}

	/* Then the accept queue, fixing its tail. */
	for (link = &listener->tcp.accept_head; *link != NULL;
	     link = &(*link)->queue_next) {
		if (*link == &child->tcp) {
			*link = child->tcp.queue_next;
			if (listener->tcp.accept_tail == &child->tcp)
				listener->tcp.accept_tail = previous;
			if (listener->tcp.accept_count != 0)
				listener->tcp.accept_count--;
			child->tcp.queue_next = NULL;
			child->tcp.listener = NULL;
			spin_unlock_irqrestore(&listener->tcp.inet.socket.lock, irq);
			return;
		}

		previous = *link;
	}

	spin_unlock_irqrestore(&listener->tcp.inet.socket.lock, irq);
}

/* Moves a child whose handshake completed to its listener's accept queue. */
static void
tcp_listener_established(
	struct tcp_endpoint *child)
{
	struct tcp_endpoint *listener;
	struct tcp_socket **link;
	unsigned long irq;

	/* Ignores a child without a listener. */
	if (child == NULL || child->tcp.listener == NULL)
		return;
	listener = (struct tcp_endpoint *)child->tcp.listener;
	irq = spin_lock_irqsave(&listener->tcp.inet.socket.lock);

	/* Unlinks the child from the half-open queue. */
	for (link = &listener->tcp.half_open_head; *link != NULL;
	     link = &(*link)->queue_next) {
		if (*link == &child->tcp) {
			*link = child->tcp.queue_next;
			if (listener->tcp.half_open_count != 0)
				listener->tcp.half_open_count--;
			break;
		}
	}

	/* Appends it to the accept queue and wakes the acceptors. */
	child->tcp.queue_next = NULL;
	if (listener->tcp.accept_tail != NULL)
		listener->tcp.accept_tail->queue_next = &child->tcp;
	else
		listener->tcp.accept_head = &child->tcp;
	listener->tcp.accept_tail = &child->tcp;
	listener->tcp.accept_count++;
	waitq_wake_all(&listener->tcp.inet.socket.accept_waitq);
	poll_notify();

	spin_unlock_irqrestore(&listener->tcp.inet.socket.lock, irq);
}

/* Tests whether another socket conflicts with a candidate's local address. */
static int
tcp_port_in_use(
	const struct tcp_endpoint *candidate,
	int strict)
{
	const struct tcp_endpoint *endpoint;
	int conflict;

	/* A strict check ignores address reuse. */
	for (endpoint = tcp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		if (endpoint == candidate)
			continue;
		if (strict)
			conflict = inet_socket_local_conflict(&endpoint->tcp.inet, 0,
			    &candidate->tcp.inet, 0);
		else
			conflict = inet_socket_local_conflict(&endpoint->tcp.inet,
			    endpoint->tcp.inet.bind_reuse_address,
			    &candidate->tcp.inet,
			    candidate->tcp.inet.bind_reuse_address);
		if (conflict)
			return 1;
	}

	return 0;
}

/* Assigns a free ephemeral port; the caller holds the registry lock. */
static int
tcp_allocate_port_locked(
	struct tcp_endpoint *endpoint)
{
	unsigned attempts;
	uint16_t port;

	/* Tries successive ephemeral ports, wrapping within the range. */
	for (attempts = 0; attempts < 16384U; attempts++) {
		port = next_ephemeral;
		next_ephemeral++;
		if (next_ephemeral < TCP_EPHEMERAL_FIRST)
			next_ephemeral = TCP_EPHEMERAL_FIRST;
		endpoint->tcp.inet.local_port = port;
		if (!tcp_port_in_use(endpoint, 1)) {
			endpoint->tcp.inet.inet_flags |= INET_SOCKET_BOUND;
			return 0;
		}

		endpoint->tcp.inet.local_port = 0;
	}

	return EADDRINUSE;
}

/* Finds the output device and source address for an endpoint's peer. */
static int
tcp_route(
	struct tcp_endpoint *endpoint,
	struct net_device **device,
	uint32_t *source)
{
	struct net_route route;
	struct net_device *output;
	int have_route;
	int error;

	/* A bound interface wins over the route's device. */
	have_route = 0;
	if (route_lookup_ref(endpoint->tcp.inet.remote_address, &route) == 0)
		have_route = 1;
	if (endpoint->tcp.inet.ifindex != 0)
		output = net_device_find_by_index_ref(endpoint->tcp.inet.ifindex);
	else if (have_route)
		output = route.device;
	else
		output = NULL;
	if (endpoint->tcp.inet.ifindex == 0 && output != NULL)
		route.device = NULL;
	if (have_route)
		route_release(&route);
	if (output == NULL)
		return ENETUNREACH;

	/* Takes the device's address as the source and remembers the interface. */
	error = inet_interface_address(output, source, NULL, NULL);
	if (error != 0) {
		net_device_release(output);
		return error;
	}

	endpoint->tcp.inet.ifindex = output->ifindex;
	if (endpoint->tcp.inet.local_address == 0)
		endpoint->tcp.inet.local_address = *source;
	*device = output;
	return 0;
}

/* Builds and transmits one segment with an explicit sequence number. */
static int
tcp_send_segment_at(
	struct tcp_endpoint *endpoint,
	uint32_t sequence,
	uint8_t flags,
	const void *data,
	size_t length)
{
	struct net_device *device;
	struct packet_buf *packet;
	struct tcp_wire *tcp;
	uint32_t source;
	uint16_t checksum;
	void *payload;
	int error;

	/* Routes the segment and allocates its buffer. */
	error = tcp_route(endpoint, &device, &source);
	if (error != 0)
		return error;
	packet = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
	if (packet == NULL) {
		net_device_release(device);
		return ENOBUFS;
	}

	tcp = packet_buf_append(packet, sizeof(*tcp));
	payload = packet_buf_append(packet, length);
	if (tcp == NULL || payload == NULL) {
		packet_buf_free(packet);
		net_device_release(device);
		return ENOBUFS;
	}

	/* Fills the header with the current receive state and a fixed window. */
	memset(tcp, 0, sizeof(*tcp));
	if (length != 0)
		memcpy(payload, data, length);
	wire_put16(tcp->source, endpoint->tcp.inet.local_port);
	wire_put16(tcp->destination, endpoint->tcp.inet.remote_port);
	wire_put32(tcp->sequence, sequence);
	wire_put32(tcp->acknowledgement, endpoint->tcp.receive_next);
	tcp->data_offset = 5U << 4;
	tcp->flags = flags;
	wire_put16(tcp->window, TCP_DEFAULT_WINDOW);
	checksum = net_checksum_pseudo(source, endpoint->tcp.inet.remote_address,
	    IPPROTO_TCP, packet->data, packet->length);
	wire_put16(tcp->checksum, checksum);

	/* Hands the segment to IP. */
	error = ipv4_output(device, endpoint->tcp.inet.remote_address,
	    IPPROTO_TCP, packet);
	net_device_release(device);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Assigns a free ephemeral port under the registry lock. */
static int
tcp_allocate_port(
	struct tcp_endpoint *endpoint)
{
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&tcp_registry_lock);

	error = tcp_allocate_port_locked(endpoint);

	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Transmits one segment at the next send sequence. */
static int
tcp_send_segment(
	struct tcp_endpoint *endpoint,
	uint8_t flags,
	const void *data,
	size_t length)
{
	int error;

	/* Reports the failure. */
	error = tcp_send_segment_at(endpoint, endpoint->tcp.send_next, flags,
	    data, length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Takes the retransmission record out of an endpoint for the caller to free. */
static void
tcp_retransmit_reset(
	struct tcp_endpoint *endpoint,
	struct packet_buf **packet)
{
	*packet = endpoint->tcp.retransmit;
	endpoint->tcp.retransmit = NULL;
	endpoint->tcp.retransmit_deadline = 0;
	endpoint->tcp.retransmit_count = 0;
}

/* Drops the retransmission record and wakes senders. */
static void
tcp_retransmit_clear(
	struct tcp_endpoint *endpoint)
{
	struct socket *socket;
	struct packet_buf *packet;
	unsigned long irq;

	/* Takes the queued segment out under the socket lock. */
	socket = &endpoint->tcp.inet.socket;
	irq = spin_lock_irqsave(&socket->lock);

	tcp_retransmit_reset(endpoint, &packet);

	spin_unlock_irqrestore(&socket->lock, irq);

	/* Frees it and lets a blocked sender continue. */
	packet_buf_free(packet);
	socket_wake_send(socket);
}

/* Abandons an active connect of a given generation; the caller holds the socket lock. */
static struct packet_buf *
tcp_connect_cancel_locked(
	struct tcp_endpoint *endpoint,
	uint32_t generation)
{
	struct socket *socket;
	struct packet_buf *packet;

	socket = &endpoint->tcp.inet.socket;

	/*
	 * The generation check prevents a waiter from cancelling a later
	 * attempt which reused the same socket.
	 */
	if (endpoint->tcp.state != TCP_SYN_SENT ||
	    endpoint->tcp.active_connect_generation != generation)
		return NULL;

	/* Closes the socket and wakes everyone waiting on the attempt. */
	tcp_retransmit_reset(endpoint, &packet);
	endpoint->tcp.state = TCP_CLOSED;
	endpoint->tcp.active_connect_generation = 0;
	endpoint->tcp.connect_wait_deadline = 0;
	tcp_forget_peer(endpoint);
	waitq_wake_all(&socket->connect_waitq);
	waitq_wake_all(&socket->send_waitq);
	poll_notify();

	/* Reports the retransmission record for the caller to free. */
	return packet;
}

/* Sends a segment that is retransmitted until acknowledged. */
static int
tcp_send_reliable(
	struct tcp_endpoint *endpoint,
	uint8_t flags,
	const void *data,
	size_t length)
{
	struct packet_buf *copy;
	void *payload;
	uint32_t sequence;
	uint32_t advance;
	unsigned long irq;
	int error;
	struct packet_buf *discard;

	/* Keeps a copy of the payload for retransmission. */
	copy = packet_buf_alloc(0);
	if (copy == NULL)
		return ENOBUFS;
	payload = packet_buf_append(copy, length);
	if (payload == NULL) {
		packet_buf_free(copy);
		return ENOBUFS;
	}

	if (length != 0)
		memcpy(payload, data, length);

	/* SYN and FIN each consume one sequence number. */
	advance = (uint32_t)length;
	if ((flags & (TCP_SYN | TCP_FIN)) != 0)
		advance++;

	/* Only one reliable segment may be outstanding. */
	irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);

	if (endpoint->tcp.retransmit != NULL) {
		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, irq);
		packet_buf_free(copy);
		return EAGAIN;
	}

	sequence = endpoint->tcp.send_next;
	endpoint->tcp.retransmit = copy;
	endpoint->tcp.retransmit_sequence = sequence;
	endpoint->tcp.retransmit_flags = flags;
	endpoint->tcp.retransmit_count = 0;
	endpoint->tcp.retransmit_deadline = sched_ticks() + TCP_INITIAL_RTO;
	endpoint->tcp.send_next += advance;

	spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, irq);

	/*
	 * Publish the retransmission record before output.  A fast ACK may
	 * then safely retire it, and the timer can never observe an unowned
	 * pointer.
	 */
	error = tcp_send_segment_at(endpoint, sequence, flags, data, length);
	if (error != 0) {
		discard = NULL;
		irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);
		if (endpoint->tcp.retransmit == copy) {
			tcp_retransmit_reset(endpoint, &discard);
			endpoint->tcp.send_next = sequence;
		}

		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, irq);
		packet_buf_free(discard);
		return error;
	}

	net_worker_wakeup();

	/* Reports the sent segment. */
	return 0;
}

/* Binds a socket to a local address and port. */
static int
tcp_bind(
	struct socket *socket,
	const struct sockaddr *address,
	socklen_t length)
{
	struct tcp_endpoint *endpoint;
	int error;
	unsigned long irq;
	unsigned long socket_irq;

	/* A socket binds only once. */
	endpoint = tcp_endpoint(socket);
	if ((endpoint->tcp.inet.inet_flags & INET_SOCKET_BOUND) != 0)
		return EINVAL;

	/* Records the reuse option in force at bind time. */
	socket_irq = spin_lock_irqsave(&socket->lock);

	endpoint->tcp.inet.bind_reuse_address = socket->reuse_address;

	spin_unlock_irqrestore(&socket->lock, socket_irq);

	error = inet_socket_bind(&endpoint->tcp.inet, address, length);
	if (error != 0)
		return error;

	/* Allocates a port for zero, or checks the requested one. */
	irq = spin_lock_irqsave(&tcp_registry_lock);

	if (endpoint->tcp.inet.local_port == 0)
		error = tcp_allocate_port_locked(endpoint);
	else if (tcp_port_in_use(endpoint, 0))
		error = EADDRINUSE;
	if (error != 0) {
		endpoint->tcp.inet.local_address = 0;
		endpoint->tcp.inet.local_port = 0;
		endpoint->tcp.inet.ifindex = 0;
		endpoint->tcp.inet.bind_reuse_address = 0;
		endpoint->tcp.inet.inet_flags &= ~INET_SOCKET_BOUND;
	}

	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	/* Reports why the bind failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Puts a bound socket into the listening state. */
static int
tcp_listen(
	struct socket *socket,
	int backlog)
{
	struct tcp_endpoint *endpoint;
	struct tcp_endpoint *other;
	unsigned long irq;
	int conflict;

	endpoint = tcp_endpoint(socket);
	conflict = 0;

	/* Listening again is harmless; listening on a connection is not. */
	if (endpoint->tcp.state != TCP_CLOSED) {
		if (endpoint->tcp.state == TCP_LISTEN)
			return 0;
		return EISCONN;
	}

	if ((endpoint->tcp.inet.inet_flags & INET_SOCKET_BOUND) == 0 ||
	    endpoint->tcp.inet.local_port == 0)
		return EDESTADDRREQ;

	/* Clamps the backlog. */
	if (backlog < 0)
		backlog = 0;
	if ((unsigned)backlog > TCP_LISTEN_BACKLOG_MAX)
		backlog = TCP_LISTEN_BACKLOG_MAX;

	/* No other listener may cover the same local address. */
	irq = spin_lock_irqsave(&tcp_registry_lock);

	for (other = tcp_sockets; other != NULL; other = other->next) {
		if (other != endpoint &&
		    other->tcp.state == TCP_LISTEN &&
		    inet_socket_local_conflict(&other->tcp.inet, 0,
		    &endpoint->tcp.inet, 0)) {
			conflict = 1;
			break;
		}
	}

	if (!conflict) {
		if (backlog == 0)
			endpoint->tcp.listen_backlog = 1U;
		else
			endpoint->tcp.listen_backlog = (unsigned)backlog;
		endpoint->tcp.state = TCP_LISTEN;
	}

	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	/* Reports the listen result. */
	if (conflict)
		return EADDRINUSE;
	return 0;
}

/* Takes the next established child off a listener. */
static int
tcp_accept(
	struct socket *socket,
	struct socket **result,
	struct sockaddr *address,
	socklen_t *length,
	unsigned io_flags)
{
	struct tcp_endpoint *listener;
	struct tcp_socket *accepted;
	struct thread *thread;
	unsigned long irq;
	int error;
	uint64_t sequence;

	listener = tcp_endpoint(socket);
	thread = thread_current();

	/* Rejects a missing result or a socket that is not listening. */
	if (result == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&socket->lock);

	if (listener->tcp.state != TCP_LISTEN) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return EINVAL;
	}

	/* Waits for a child unless the accept must not block. */
	while (listener->tcp.accept_head == NULL) {
		if ((io_flags & SOCKET_IO_NONBLOCK) != 0 || thread == NULL) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return EAGAIN;
		}

		if (signal_pending_unblocked(thread)) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return EINTR;
		}

		sequence = waitq_sequence(&socket->accept_waitq);
		error = waitq_sleep(&socket->accept_waitq, &socket->lock, sequence,
		    0, WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return EINTR;
		}

		if (socket->error != 0) {
			error = socket->error;
			socket->error = 0;
			spin_unlock_irqrestore(&socket->lock, irq);
			return error;
		}
	}

	/* Reports the peer and dequeues the child. */
	accepted = listener->tcp.accept_head;
	if (address != NULL && length != NULL) {
		error = inet_socket_getpeername(&accepted->inet, address, length);
		if (error != 0) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return error;
		}
	}

	listener->tcp.accept_head = accepted->queue_next;
	if (listener->tcp.accept_head == NULL)
		listener->tcp.accept_tail = NULL;
	if (listener->tcp.accept_count != 0)
		listener->tcp.accept_count--;
	accepted->queue_next = NULL;
	accepted->listener = NULL;
	*result = &accepted->inet.socket;

	spin_unlock_irqrestore(&socket->lock, irq);

	/* Reports the accepted socket. */
	return 0;
}

/* Starts or resumes an active connection to a peer. */
static int
tcp_connect(
	struct socket *socket,
	const struct sockaddr *address,
	socklen_t length,
	unsigned io_flags)
{
	struct tcp_endpoint *endpoint;
	struct thread *thread;
	struct packet_buf *cancelled;
	uint64_t deadline;
	uint64_t timeout;
	uint32_t generation;
	unsigned long irq;
	unsigned attempt;
	int error;
	uint64_t sequence;

	endpoint = tcp_endpoint(socket);
	thread = thread_current();
	cancelled = NULL;
	deadline = 0;

	/*
	 * STOP/CONT is transparent at the syscall layer.  A redispatched connect
	 * resumes the existing attempt; an ordinary second connect still
	 * observes EALREADY/EISCONN as required by the socket API.
	 */
	if (thread != NULL && thread->syscall_stop_redispatch) {
		irq = spin_lock_irqsave(&socket->lock);
		if (endpoint->tcp.state == TCP_ESTABLISHED) {
			endpoint->tcp.connect_wait_deadline = 0;
			spin_unlock_irqrestore(&socket->lock, irq);
			return 0;
		}

		if (socket->error != 0) {
			error = socket->error;
			socket->error = 0;
			endpoint->tcp.connect_wait_deadline = 0;
			spin_unlock_irqrestore(&socket->lock, irq);
			return error;
		}

		if (endpoint->tcp.state != TCP_SYN_SENT ||
		    endpoint->tcp.active_connect_generation == 0) {
			endpoint->tcp.connect_wait_deadline = 0;
			spin_unlock_irqrestore(&socket->lock, irq);
			return ECONNABORTED;
		}

		generation = endpoint->tcp.active_connect_generation;
		deadline = endpoint->tcp.connect_wait_deadline;
		goto wait_for_connect;
	}

	/* Only a closed socket can start connecting. */
	irq = spin_lock_irqsave(&socket->lock);

	if (endpoint->tcp.state == TCP_SYN_SENT)
		error = EALREADY;
	else if (endpoint->tcp.state != TCP_CLOSED)
		error = EISCONN;
	else
		error = 0;

	spin_unlock_irqrestore(&socket->lock, irq);

	if (error != 0)
		return error;

	/* Records the peer and takes a local port when unbound. */
	error = inet_socket_connect(&endpoint->tcp.inet, address, length);
	if (error != 0)
		return error;
	if (endpoint->tcp.inet.remote_port == 0) {
		tcp_forget_peer(endpoint);
		return EADDRNOTAVAIL;
	}

	if (endpoint->tcp.inet.local_port == 0) {
		error = tcp_allocate_port(endpoint);
		if (error != 0) {
			tcp_forget_peer(endpoint);
			return error;
		}
	}

	/* Starts a new attempt with a fresh generation and sequence space. */
	endpoint->tcp.send_next = (uint32_t)sched_ticks() * 1103515245U +
	    endpoint->tcp.inet.local_port;
	irq = spin_lock_irqsave(&socket->lock);

	endpoint->tcp.connect_generation++;
	if (endpoint->tcp.connect_generation == 0)
		endpoint->tcp.connect_generation++;
	generation = endpoint->tcp.connect_generation;
	endpoint->tcp.active_connect_generation = generation;
	endpoint->tcp.connect_wait_deadline = 0;

	/*
	 * Make distinct attempts use distinct wire sequence spaces even when
	 * the scheduler tick did not advance between them.
	 */
	endpoint->tcp.send_next ^= generation * 2654435761U;
	endpoint->tcp.send_unacknowledged = endpoint->tcp.send_next;
	endpoint->tcp.state = TCP_SYN_SENT;

	spin_unlock_irqrestore(&socket->lock, irq);

	/* Sends the SYN, retrying a few times when buffers are short. */
	for (attempt = 0; attempt < 4U; attempt++) {
		error = tcp_send_reliable(endpoint, TCP_SYN, NULL, 0);
		if (error == 0)
			break;
		if (error != EAGAIN && error != EBUSY && error != ENOBUFS) {
			irq = spin_lock_irqsave(&socket->lock);
			cancelled = tcp_connect_cancel_locked(endpoint, generation);
			spin_unlock_irqrestore(&socket->lock, irq);
			packet_buf_free(cancelled);
			return error;
		}

		if (thread != NULL)
			sched_sleep(sched_ticks() + 25U);
	}

	if (error != 0) {
		irq = spin_lock_irqsave(&socket->lock);
		cancelled = tcp_connect_cancel_locked(endpoint, generation);
		spin_unlock_irqrestore(&socket->lock, irq);
		packet_buf_free(cancelled);
		return error;
	}

	/* A non-blocking connect completes through the timer and poll. */
	if ((io_flags & SOCKET_IO_NONBLOCK) != 0)
		return EINPROGRESS;
	if (thread == NULL)
		return EAGAIN;

	/* Applies the send timeout as the wait deadline. */
	irq = spin_lock_irqsave(&socket->lock);

	timeout = socket->send_timeout_ticks;
	if (timeout != 0) {
		error = kern_deadline_after(sched_ticks(), timeout, &deadline);
		if (error != 0) {
			cancelled = tcp_connect_cancel_locked(endpoint, generation);
			spin_unlock_irqrestore(&socket->lock, irq);
			packet_buf_free(cancelled);
			return error;
		}
	}

	endpoint->tcp.connect_wait_deadline = deadline;
wait_for_connect:
	/* Waits for the handshake, a failure, or the deadline. */
	while (endpoint->tcp.state == TCP_SYN_SENT &&
	    endpoint->tcp.active_connect_generation == generation &&
	    socket->error == 0) {
		sequence = waitq_sequence(&socket->connect_waitq);
		error = waitq_sleep(&socket->connect_waitq, &socket->lock,
		    sequence, deadline, WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			/*
			 * POSIX requires an interrupted blocking connect to leave
			 * the request in progress.  The retransmission timer owns
			 * the SYN_SENT attempt from here; poll(POLLOUT) plus
			 * SO_ERROR observes completion.
			 */
			spin_unlock_irqrestore(&socket->lock, irq);
			return EINTR;
		}

		if (error == ETIMEDOUT) {
			cancelled = tcp_connect_cancel_locked(endpoint, generation);
			spin_unlock_irqrestore(&socket->lock, irq);
			packet_buf_free(cancelled);
			return error;
		}
	}

	/* Reports a recorded failure, or the final state. */
	if (socket->error != 0) {
		error = socket->error;
		socket->error = 0;
		endpoint->tcp.connect_wait_deadline = 0;
		spin_unlock_irqrestore(&socket->lock, irq);
		return error;
	}

	if (endpoint->tcp.state == TCP_ESTABLISHED)
		error = 0;
	else
		error = ETIMEDOUT;
	endpoint->tcp.connect_wait_deadline = 0;

	spin_unlock_irqrestore(&socket->lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Sends one segment of data on an established connection. */
static ssize_t
tcp_sendto(
	struct socket *socket,
	const void *buffer,
	size_t length,
	int flags,
	const struct sockaddr *address,
	socklen_t address_length)
{
	struct tcp_endpoint *endpoint;
	struct thread *thread;
	unsigned long irq;
	unsigned attempt;
	int error;
	uint64_t sequence;

	endpoint = tcp_endpoint(socket);
	thread = thread_current();

	(void)address_length;

	/* Rejects unsupported flags, an address, or a missing buffer. */
	if ((flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) != 0 ||
	    address != NULL ||
	    (buffer == NULL && length != 0))
		return -EINVAL;

	/* A shut-down or unconnected socket cannot send. */
	irq = spin_lock_irqsave(&socket->lock);

	if (socket->write_shutdown) {
		spin_unlock_irqrestore(&socket->lock, irq);
		if ((flags & MSG_NOSIGNAL) == 0 && thread != NULL &&
		    thread->proc != NULL)
			(void)signal_send_thread(thread, SIGPIPE);
		return -EPIPE;
	}

	if (endpoint->tcp.state != TCP_ESTABLISHED) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return -ENOTCONN;
	}

	spin_unlock_irqrestore(&socket->lock, irq);

	if (length == 0)
		return 0;
	if (length > TCP_MSS)
		length = TCP_MSS;

	/* Waits for the previous segment to be acknowledged. */
	for (;;) {
		irq = spin_lock_irqsave(&socket->lock);
		if (endpoint->tcp.send_unacknowledged == endpoint->tcp.send_next) {
			spin_unlock_irqrestore(&socket->lock, irq);
			break;
		}

		if ((flags & MSG_DONTWAIT) != 0 || thread == NULL) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return -EAGAIN;
		}

		sequence = waitq_sequence(&socket->send_waitq);
		error = waitq_sleep(&socket->send_waitq, &socket->lock, sequence,
		    0, WAITQ_INTERRUPTIBLE);
		if (socket->error != 0) {
			error = socket->error;
			socket->error = 0;
			spin_unlock_irqrestore(&socket->lock, irq);
			return -error;
		}

		spin_unlock_irqrestore(&socket->lock, irq);
		if (error == EINTR)
			return -EINTR;
	}

	/* Sends the segment, retrying while buffers are short. */
	for (attempt = 0; ; attempt++) {
		error = tcp_send_reliable(endpoint, TCP_ACK | TCP_PSH,
		    buffer, length);
		if (error != EAGAIN && error != EBUSY && error != ENOBUFS)
			break;
		if ((flags & MSG_DONTWAIT) != 0 || thread == NULL || attempt >= 99U)
			return -EAGAIN;
		sched_sleep(sched_ticks() + 1U);
	}

	if (error != 0)
		return -error;

	/* Reports the bytes sent. */
	return (ssize_t)length;
}

/* Receives data from the socket's queue, keeping an unread remainder. */
static ssize_t
tcp_recvfrom(
	struct socket *socket,
	void *buffer,
	size_t length,
	int flags,
	struct sockaddr *address,
	socklen_t *address_length)
{
	struct tcp_endpoint *endpoint;
	struct packet_buf *packet;
	size_t copied;
	int error;

	endpoint = tcp_endpoint(socket);

	(void)address;
	(void)address_length;

	/* A shut-down or drained closing connection reports end of file. */
	if (socket->read_shutdown)
		return 0;
	if (endpoint->tcp.state == TCP_CLOSE_WAIT && socket->receive_head == NULL)
		return 0;

	/* Takes the next packet and copies what fits. */
	error = socket_dequeue_packet(socket, flags, &packet);
	if (error != 0)
		return -error;
	if (length < packet->length)
		copied = length;
	else
		copied = packet->length;
	if (copied != 0)
		memcpy(buffer, packet->data, copied);

	/* Retain a short unread suffix by placing it back at the queue head. */
	if (copied < packet->length) {
		(void)packet_buf_pull(packet, copied);
		(void)socket_requeue_packet_front(socket, packet);
	} else {
		packet_buf_free(packet);
	}

	/* Reports the bytes received. */
	return (ssize_t)copied;
}

/* Shuts down one or both directions, sending a FIN for the write side. */
static int
tcp_shutdown(
	struct socket *socket,
	int how)
{
	struct tcp_endpoint *endpoint;
	enum tcp_state old_state;
	enum tcp_state closing_state;
	unsigned long irq;
	int error;

	endpoint = tcp_endpoint(socket);

	/* Rejects an unknown direction. */
	if (how != SHUT_WR && how != SHUT_RDWR && how != SHUT_RD)
		return EINVAL;

	/* The read side is shut down by flag alone. */
	irq = spin_lock_irqsave(&socket->lock);

	if (how == SHUT_RD || how == SHUT_RDWR)
		socket->read_shutdown = 1;
	if (how == SHUT_RD) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return 0;
	}

	if (socket->write_shutdown) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return 0;
	}

	/* The write side moves to the closing state and sends a FIN. */
	if (endpoint->tcp.state != TCP_ESTABLISHED &&
	    endpoint->tcp.state != TCP_CLOSE_WAIT) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return ENOTCONN;
	}

	old_state = endpoint->tcp.state;
	if (old_state == TCP_CLOSE_WAIT)
		closing_state = TCP_LAST_ACK;
	else
		closing_state = TCP_FIN_WAIT_1;
	socket->write_shutdown = 1;
	endpoint->tcp.state = closing_state;

	spin_unlock_irqrestore(&socket->lock, irq);

	/* A FIN that could not be sent restores the state. */
	error = tcp_send_reliable(endpoint, TCP_FIN | TCP_ACK, NULL, 0);
	if (error != 0) {
		irq = spin_lock_irqsave(&socket->lock);
		if (endpoint->tcp.state == closing_state) {
			endpoint->tcp.state = old_state;
			socket->write_shutdown = 0;
		}

		spin_unlock_irqrestore(&socket->lock, irq);
	}

	/* Reports why the shutdown failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports the local address. */
static int
tcp_getsockname(
	struct socket *socket,
	struct sockaddr *address,
	socklen_t *length)
{
	struct tcp_endpoint *endpoint;
	int error;

	endpoint = tcp_endpoint(socket);

	/* Reports the failure. */
	error = inet_socket_getsockname(&endpoint->tcp.inet, address, length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports the peer address. */
static int
tcp_getpeername(
	struct socket *socket,
	struct sockaddr *address,
	socklen_t *length)
{
	struct tcp_endpoint *endpoint;
	int error;

	endpoint = tcp_endpoint(socket);

	/* Reports the failure. */
	error = inet_socket_getpeername(&endpoint->tcp.inet, address, length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Closes a socket, aborting its queued children and unregistering it. */
static void
tcp_close(
	struct socket *socket)
{
	struct tcp_endpoint *endpoint;
	struct tcp_endpoint **link;
	struct tcp_socket *queued;
	unsigned long irq;

	/* A connection sends its FIN; a child leaves its listener. */
	endpoint = tcp_endpoint(socket);
	if (endpoint->tcp.state == TCP_ESTABLISHED ||
	    endpoint->tcp.state == TCP_CLOSE_WAIT)
		(void)tcp_shutdown(socket, SHUT_RDWR);
	if (endpoint->tcp.listener != NULL)
		tcp_listener_remove(endpoint);

	/* A listener aborts every half-open and unaccepted child. */
	queued = endpoint->tcp.half_open_head;
	while (queued != NULL) {
		endpoint->tcp.half_open_head = queued->queue_next;
		queued->queue_next = NULL;
		queued->listener = NULL;
		socket_set_error(&queued->inet.socket, ECONNABORTED);
		socket_release(&queued->inet.socket);
		queued = endpoint->tcp.half_open_head;
	}

	queued = endpoint->tcp.accept_head;
	while (queued != NULL) {
		endpoint->tcp.accept_head = queued->queue_next;
		queued->queue_next = NULL;
		queued->listener = NULL;
		socket_set_error(&queued->inet.socket, ECONNABORTED);
		socket_release(&queued->inet.socket);
		queued = endpoint->tcp.accept_head;
	}

	/* Unregisters the endpoint and frees it. */
	irq = spin_lock_irqsave(&tcp_registry_lock);

	for (link = &tcp_sockets; *link != NULL; link = &(*link)->next) {
		if (*link == endpoint) {
			*link = endpoint->next;
			break;
		}
	}

	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	tcp_retransmit_clear(endpoint);
	kern_free(endpoint);
}

/* Reports the readiness of a socket. */
static int
tcp_poll(
	struct socket *socket,
	short events,
	short *revents)
{
	struct tcp_endpoint *endpoint;
	short result;
	unsigned long irq;

	endpoint = tcp_endpoint(socket);
	result = 0;

	/* Rejects a missing socket or result. */
	if (socket == NULL || revents == NULL)
		return EINVAL;

	/* Derives readiness from the state under the socket lock. */
	irq = spin_lock_irqsave(&socket->lock);

	if (socket->error != 0)
		result |= POLLERR;
	if (endpoint->tcp.state == TCP_LISTEN) {
		/* A listener is readable when a child awaits accept. */
		if (endpoint->tcp.accept_head != NULL)
			result |= events & (POLLIN | POLLRDNORM);
	} else {
		/* Data, end of file, or a closed peer makes a connection readable. */
		if (socket->receive_head != NULL ||
		    socket->read_shutdown ||
		    endpoint->tcp.state == TCP_CLOSE_WAIT ||
		    endpoint->tcp.state == TCP_TIME_WAIT ||
		    (endpoint->tcp.state == TCP_CLOSED &&
		     (endpoint->tcp.inet.inet_flags & INET_SOCKET_CONNECTED) != 0))
			result |= events & (POLLIN | POLLRDNORM);

		/* An acknowledged connection, or a failed connect, is writable. */
		if (!socket->write_shutdown &&
		    endpoint->tcp.state == TCP_ESTABLISHED &&
		    endpoint->tcp.send_unacknowledged == endpoint->tcp.send_next)
			result |= events & (POLLOUT | POLLWRNORM);
		if (endpoint->tcp.state == TCP_SYN_SENT && socket->error != 0)
			result |= events & (POLLOUT | POLLWRNORM);
		if (socket->read_shutdown ||
		    socket->write_shutdown ||
		    endpoint->tcp.state == TCP_CLOSE_WAIT ||
		    endpoint->tcp.state == TCP_TIME_WAIT ||
		    socket->lifecycle != SOCKET_OPEN)
			result |= POLLHUP;
	}

	if (socket->lifecycle != SOCKET_OPEN)
		result |= POLLHUP;

	spin_unlock_irqrestore(&socket->lock, irq);

	*revents = result;

	/* Reports the derived readiness. */
	return 0;
}

/* Finds the referenced connection or listener for an incoming segment. */
static struct tcp_endpoint *
tcp_lookup(
	uint32_t source,
	uint32_t destination,
	uint16_t source_port,
	uint16_t destination_port)
{
	struct tcp_endpoint *endpoint;
	struct tcp_endpoint *wildcard;
	unsigned long irq;
	unsigned long socket_irq;
	int match;
	int listening;
	uint32_t local;
	uint16_t port;

	wildcard = NULL;
	irq = spin_lock_irqsave(&tcp_registry_lock);

	/* A connection matches on the full four-tuple. */
	for (endpoint = tcp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		socket_irq = spin_lock_irqsave(
		    &endpoint->tcp.inet.socket.lock);
		match = 0;
		if (endpoint->tcp.state != TCP_CLOSED &&
		    endpoint->tcp.state != TCP_LISTEN &&
		    endpoint->tcp.inet.local_port == destination_port &&
		    endpoint->tcp.inet.remote_port == source_port &&
		    endpoint->tcp.inet.remote_address == source &&
		    (endpoint->tcp.inet.local_address == 0 ||
		     endpoint->tcp.inet.local_address == destination))
			match = 1;
		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		if (match)
			goto found;
	}

	/* A listener matches on the port, preferring an exact address. */
	for (endpoint = tcp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		socket_irq = spin_lock_irqsave(
		    &endpoint->tcp.inet.socket.lock);
		listening = endpoint->tcp.state == TCP_LISTEN;
		local = endpoint->tcp.inet.local_address;
		port = endpoint->tcp.inet.local_port;
		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		if (!listening || port != destination_port)
			continue;
		if (local == destination)
			goto found;
		if (local == 0)
			wildcard = endpoint;
	}

	endpoint = wildcard;
found:
	/* References the match unless it is being closed. */
	if (endpoint != NULL && !socket_tryref(&endpoint->tcp.inet.socket))
		endpoint = NULL;

	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	/* Reports the referenced endpoint, or NULL. */
	return endpoint;
}

/* Creates a half-open child for a SYN that reached a listener. */
static struct tcp_endpoint *
tcp_passive_syn(
	struct tcp_endpoint *listener,
	uint32_t source,
	uint32_t destination,
	uint16_t source_port,
	uint32_t sequence)
{
	struct socket *created;
	struct tcp_endpoint *child;
	unsigned long irq;
	int error;

	/* Creates the child socket. */
	error = socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP, &created);
	if (error != 0)
		return NULL;
	child = tcp_endpoint(created);

	/* The listener must still listen and have backlog room. */
	irq = spin_lock_irqsave(&listener->tcp.inet.socket.lock);

	if (listener->tcp.state != TCP_LISTEN ||
	    listener->tcp.half_open_count + listener->tcp.accept_count >=
	    listener->tcp.listen_backlog) {
		spin_unlock_irqrestore(&listener->tcp.inet.socket.lock, irq);
		socket_release(created);
		return NULL;
	}

	/* The child inherits the listener's address, options, and timeouts. */
	if (listener->tcp.inet.local_address == 0)
		child->tcp.inet.local_address = destination;
	else
		child->tcp.inet.local_address = listener->tcp.inet.local_address;
	child->tcp.inet.local_port = listener->tcp.inet.local_port;
	child->tcp.inet.remote_address = source;
	child->tcp.inet.remote_port = source_port;
	child->tcp.inet.ifindex = listener->tcp.inet.ifindex;
	child->tcp.inet.inet_flags = INET_SOCKET_BOUND | INET_SOCKET_CONNECTED;
	child->tcp.receive_next = sequence + 1U;
	child->tcp.send_next = (uint32_t)sched_ticks() * 1103515245U +
	    child->tcp.inet.local_port + source_port;
	child->tcp.send_unacknowledged = child->tcp.send_next;
	child->tcp.listener = &listener->tcp;
	child->tcp.inet.socket.receive_timeout_ticks =
	    listener->tcp.inet.socket.receive_timeout_ticks;
	child->tcp.inet.socket.send_timeout_ticks =
	    listener->tcp.inet.socket.send_timeout_ticks;
	child->tcp.inet.socket.reuse_address =
	    listener->tcp.inet.socket.reuse_address;
	child->tcp.inet.bind_reuse_address =
	    listener->tcp.inet.bind_reuse_address;
	child->tcp.queue_next = listener->tcp.half_open_head;
	listener->tcp.half_open_head = &child->tcp;
	listener->tcp.half_open_count++;

	spin_unlock_irqrestore(&listener->tcp.inet.socket.lock, irq);

	/* Answers with a SYN-ACK, dropping the child when that fails. */
	irq = spin_lock_irqsave(&child->tcp.inet.socket.lock);

	child->tcp.state = TCP_SYN_RECEIVED;

	spin_unlock_irqrestore(&child->tcp.inet.socket.lock, irq);

	error = tcp_send_reliable(child, TCP_SYN | TCP_ACK, NULL, 0);
	if (error != 0) {
		tcp_listener_remove(child);
		socket_release(created);
		return NULL;
	}

	/* Reports the half-open child. */
	return child;
}

/* Handles an incoming TCP segment delivered by IP. */
static int
tcp_input(
	struct packet_buf *packet,
	uint32_t source,
	uint32_t destination)
{
	const struct tcp_wire *tcp;
	struct tcp_endpoint *endpoint;
	uint32_t sequence;
	uint32_t acknowledgement;
	uint16_t source_port;
	uint16_t destination_port;
	size_t header_length;
	size_t payload_length;
	uint8_t flags;
	enum tcp_state state;
	unsigned long socket_irq;
	struct packet_buf *retransmit;
	uint32_t resend_sequence;
	int resend;
	int established;
	uint32_t ack_sequence;
	int accept_payload;
	int discard_payload;
	int accept_fin;
	struct packet_buf *eof;

	/* Drops a short or corrupt segment. */
	if (packet == NULL ||
	    packet->length < sizeof(*tcp) ||
	    net_checksum_pseudo(source, destination, IPPROTO_TCP,
	    packet->data, packet->length) != 0) {
		packet_buf_free(packet);
		return EINVAL;
	}

	tcp = (const struct tcp_wire *)packet->data;
	header_length = (size_t)(tcp->data_offset >> 4) * 4U;
	if (header_length < sizeof(*tcp) || header_length > packet->length) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Finds the connection or listener; an unmatched segment is dropped. */
	source_port = wire_get16(tcp->source);
	destination_port = wire_get16(tcp->destination);
	endpoint = tcp_lookup(source, destination, source_port, destination_port);
	if (endpoint == NULL) {
		packet_buf_free(packet);
		return 0;
	}

	sequence = wire_get32(tcp->sequence);
	acknowledgement = wire_get32(tcp->acknowledgement);
	flags = tcp->flags;
	payload_length = packet->length - header_length;
	socket_irq = spin_lock_irqsave(

	    &endpoint->tcp.inet.socket.lock);
	state = endpoint->tcp.state;

	spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);

	/* A listener answers a bare SYN with a half-open child. */
	if (state == TCP_LISTEN) {
		if ((flags & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN)
			(void)tcp_passive_syn(endpoint, source, destination,
			    source_port, sequence);
		packet_buf_free(packet);
		socket_release(&endpoint->tcp.inet.socket);
		return 0;
	}

	/* A reset closes the connection and fails any pending connect. */
	if (flags & TCP_RST) {
		retransmit = NULL;
		socket_irq = spin_lock_irqsave(
		    &endpoint->tcp.inet.socket.lock);
		tcp_retransmit_reset(endpoint, &retransmit);
		endpoint->tcp.state = TCP_CLOSED;
		endpoint->tcp.active_connect_generation = 0;
		endpoint->tcp.connect_wait_deadline = 0;
		tcp_forget_peer(endpoint);
		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		packet_buf_free(retransmit);
		socket_set_error(&endpoint->tcp.inet.socket, ECONNRESET);
		if (endpoint->tcp.listener != NULL) {
			tcp_listener_remove(endpoint);
			packet_buf_free(packet);
			/* Drop the listener queue and lookup references. */
			socket_release(&endpoint->tcp.inet.socket);
			socket_release(&endpoint->tcp.inet.socket);
			return 0;
		}

		socket_wake_connect(&endpoint->tcp.inet.socket);
		packet_buf_free(packet);
		socket_release(&endpoint->tcp.inet.socket);
		return 0;
	}

	/* A half-open child completes on the handshake ACK, or repeats its SYN-ACK. */
	if (state == TCP_SYN_RECEIVED) {
		retransmit = NULL;
		resend_sequence = 0;
		resend = 0;
		established = 0;
		socket_irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);
		if ((flags & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN &&
		    sequence + 1U == endpoint->tcp.receive_next) {
			resend = 1;
			resend_sequence = endpoint->tcp.retransmit_sequence;
		}

		if ((flags & TCP_ACK) != 0 &&
		    acknowledgement == endpoint->tcp.send_next) {
			endpoint->tcp.send_unacknowledged = acknowledgement;
			tcp_retransmit_reset(endpoint, &retransmit);
			endpoint->tcp.state = TCP_ESTABLISHED;
			established = 1;
		}

		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		packet_buf_free(retransmit);
		if (resend)
			(void)tcp_send_segment_at(endpoint, resend_sequence,
			    TCP_SYN | TCP_ACK, NULL, 0);
		if (established) {
			socket_wake_send(&endpoint->tcp.inet.socket);
			tcp_listener_established(endpoint);
		}

		packet_buf_free(packet);
		socket_release(&endpoint->tcp.inet.socket);
		return 0;
	}

	/* An active connect completes on the SYN-ACK for its sequence. */
	if (state == TCP_SYN_SENT) {
		retransmit = NULL;
		established = 0;
		socket_irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);
		if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
		    endpoint->tcp.active_connect_generation != 0 &&
		    acknowledgement == endpoint->tcp.send_next) {
			endpoint->tcp.send_unacknowledged = acknowledgement;
			tcp_retransmit_reset(endpoint, &retransmit);
			endpoint->tcp.receive_next = sequence + 1U;
			endpoint->tcp.peer_window = wire_get16(tcp->window);
			endpoint->tcp.state = TCP_ESTABLISHED;
			endpoint->tcp.active_connect_generation = 0;
			endpoint->tcp.connect_wait_deadline = 0;
			waitq_wake_all(&endpoint->tcp.inet.socket.connect_waitq);
			waitq_wake_all(&endpoint->tcp.inet.socket.send_waitq);
			poll_notify();
			established = 1;
		}

		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		packet_buf_free(retransmit);
		if (established)
			(void)tcp_send_segment(endpoint, TCP_ACK, NULL, 0);
		packet_buf_free(packet);
		socket_release(&endpoint->tcp.inet.socket);
		return 0;
	}

	/* An acknowledgement of the outstanding segment retires it. */
	if ((flags & TCP_ACK) != 0) {
		retransmit = NULL;
		socket_irq = spin_lock_irqsave(
		    &endpoint->tcp.inet.socket.lock);
		if (acknowledgement >= endpoint->tcp.send_unacknowledged &&
		    acknowledgement <= endpoint->tcp.send_next) {
			endpoint->tcp.send_unacknowledged = acknowledgement;
			if (acknowledgement == endpoint->tcp.send_next)
				tcp_retransmit_reset(endpoint, &retransmit);
		}

		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		packet_buf_free(retransmit);
		if (retransmit != NULL)
			socket_wake_send(&endpoint->tcp.inet.socket);
	}

	/* In-order payload is queued and acknowledged; a read shutdown discards it. */
	if (payload_length != 0) {
		ack_sequence = 0;
		socket_irq = spin_lock_irqsave(
		    &endpoint->tcp.inet.socket.lock);
		accept_payload = sequence == endpoint->tcp.receive_next;
		discard_payload = endpoint->tcp.inet.socket.read_shutdown;
		if (accept_payload) {
			endpoint->tcp.receive_next += (uint32_t)payload_length;
			ack_sequence = endpoint->tcp.send_next;
		}

		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		if (!accept_payload)
			goto payload_done;
		if (packet_buf_pull(packet, header_length) == NULL) {
			packet_buf_free(packet);
			socket_release(&endpoint->tcp.inet.socket);
			return EINVAL;
		}

		if (!discard_payload) {
			(void)socket_enqueue_packet(&endpoint->tcp.inet.socket, packet);
			packet = NULL;
		}

		(void)tcp_send_segment_at(endpoint, ack_sequence, TCP_ACK, NULL, 0);
	}

payload_done:
	/* An in-order FIN advances the state and queues an end-of-file marker. */
	if ((flags & TCP_FIN) != 0) {
		ack_sequence = 0;
		socket_irq = spin_lock_irqsave(
		    &endpoint->tcp.inet.socket.lock);
		accept_fin = sequence + (uint32_t)payload_length ==
		    endpoint->tcp.receive_next;
		if (accept_fin) {
			endpoint->tcp.receive_next++;
			if (endpoint->tcp.state == TCP_ESTABLISHED)
				endpoint->tcp.state = TCP_CLOSE_WAIT;
			else if (endpoint->tcp.state == TCP_FIN_WAIT_1 ||
			    endpoint->tcp.state == TCP_FIN_WAIT_2)
				endpoint->tcp.state = TCP_TIME_WAIT;
			ack_sequence = endpoint->tcp.send_next;
		}

		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		if (!accept_fin)
			goto fin_done;
		(void)tcp_send_segment_at(endpoint, ack_sequence, TCP_ACK, NULL, 0);
		eof = packet_buf_alloc(0);
		if (eof != NULL)
			(void)socket_enqueue_packet(&endpoint->tcp.inet.socket,
			    eof);
		else
			socket_wake_receive(&endpoint->tcp.inet.socket);
	}

fin_done:
	/* A fully acknowledged FIN of ours advances the closing states. */
	socket_irq = spin_lock_irqsave(

	    &endpoint->tcp.inet.socket.lock);
	if (endpoint->tcp.state == TCP_FIN_WAIT_1 &&
	    endpoint->tcp.send_unacknowledged == endpoint->tcp.send_next)
		endpoint->tcp.state = TCP_FIN_WAIT_2;
	if (endpoint->tcp.state == TCP_LAST_ACK &&
	    endpoint->tcp.send_unacknowledged == endpoint->tcp.send_next)
		endpoint->tcp.state = TCP_CLOSED;

	spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);

	/* Frees a packet that was not queued and drops the lookup reference. */
	if (packet != NULL)
		packet_buf_free(packet);
	socket_release(&endpoint->tcp.inet.socket);
	return 0;
}
