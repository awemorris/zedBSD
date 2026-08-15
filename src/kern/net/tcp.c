/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/tcp-socket.h"
#include "kern/net/byteorder.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/route.h"
#include "kern/kmem.h"
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

static struct tcp_endpoint *tcp_endpoint(struct socket *socket)
{
	return (struct tcp_endpoint *)socket;
}

static void
tcp_listener_remove(struct tcp_endpoint *child)
{
	struct tcp_endpoint *listener;
	struct tcp_socket **link, *previous = NULL;

	if (child == NULL || child->tcp.listener == NULL)
		return;
	listener = (struct tcp_endpoint *)child->tcp.listener;
	for (link = &listener->tcp.half_open_head; *link != NULL;
	    link = &(*link)->queue_next) {
		if (*link == &child->tcp) {
			*link = child->tcp.queue_next;
			if (listener->tcp.half_open_count != 0)
				listener->tcp.half_open_count--;
			child->tcp.queue_next = NULL;
			child->tcp.listener = NULL;
			return;
		}
	}
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
			return;
		}
		previous = *link;
	}
}

static void
tcp_listener_established(struct tcp_endpoint *child)
{
	struct tcp_endpoint *listener;
	struct tcp_socket **link;

	if (child == NULL || child->tcp.listener == NULL)
		return;
	listener = (struct tcp_endpoint *)child->tcp.listener;
	for (link = &listener->tcp.half_open_head; *link != NULL;
	    link = &(*link)->queue_next)
		if (*link == &child->tcp) {
			*link = child->tcp.queue_next;
			if (listener->tcp.half_open_count != 0)
				listener->tcp.half_open_count--;
			break;
		}
	child->tcp.queue_next = NULL;
	if (listener->tcp.accept_tail != NULL)
		listener->tcp.accept_tail->queue_next = &child->tcp;
	else
		listener->tcp.accept_head = &child->tcp;
	listener->tcp.accept_tail = &child->tcp;
	listener->tcp.accept_count++;
	if (listener->tcp.inet.socket.accept_waiter != NULL)
		sched_wakeup(listener->tcp.inet.socket.accept_waiter);
}

static int
tcp_port_in_use(const struct tcp_endpoint *skip, uint32_t address,
		uint16_t port)
{
	const struct tcp_endpoint *endpoint;

	for (endpoint = tcp_sockets; endpoint != NULL; endpoint = endpoint->next)
		if (endpoint != skip && endpoint->tcp.inet.local_port == port &&
		    (endpoint->tcp.inet.local_address == 0 || address == 0 ||
		     endpoint->tcp.inet.local_address == address))
			return 1;
	return 0;
}

static int
tcp_allocate_port(struct tcp_endpoint *endpoint)
{
	unsigned attempts;

	for (attempts = 0; attempts < 16384U; attempts++) {
		uint16_t port = next_ephemeral++;
		if (next_ephemeral < TCP_EPHEMERAL_FIRST)
			next_ephemeral = TCP_EPHEMERAL_FIRST;
		if (!tcp_port_in_use(endpoint, endpoint->tcp.inet.local_address, port)) {
			endpoint->tcp.inet.local_port = port;
			endpoint->tcp.inet.inet_flags |= INET_SOCKET_BOUND;
			return 0;
		}
	}
	return EADDRINUSE;
}

static int
tcp_route(struct tcp_endpoint *endpoint, struct net_device **device,
	  uint32_t *source)
{
	const struct net_route *route =
	    route_lookup(endpoint->tcp.inet.remote_address);
	struct net_device *output = endpoint->tcp.inet.ifindex != 0 ?
	    net_device_find_by_index(endpoint->tcp.inet.ifindex) :
	    (route != NULL ? route->device : NULL);
	int error;

	if (output == NULL)
		return ENETUNREACH;
	error = inet_interface_address(output, source, NULL, NULL);
	if (error != 0)
		return error;
	endpoint->tcp.inet.ifindex = output->ifindex;
	if (endpoint->tcp.inet.local_address == 0)
		endpoint->tcp.inet.local_address = *source;
	*device = output;
	return 0;
}

static int
tcp_send_segment_at(struct tcp_endpoint *endpoint, uint32_t sequence,
		    uint8_t flags, const void *data, size_t length)
{
	struct net_device *device;
	struct packet_buf *packet;
	struct tcp_wire *tcp;
	uint32_t source;
	uint16_t checksum;
	void *payload;
	int error = tcp_route(endpoint, &device, &source);

	if (error != 0)
		return error;
	packet = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
	if (packet == NULL)
		return ENOBUFS;
	tcp = packet_buf_append(packet, sizeof(*tcp));
	payload = packet_buf_append(packet, length);
	if (tcp == NULL || payload == NULL) {
		packet_buf_free(packet);
		return ENOBUFS;
	}
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
	return ipv4_output(device, endpoint->tcp.inet.remote_address,
	    IPPROTO_TCP, packet);
}

static int
tcp_send_segment(struct tcp_endpoint *endpoint, uint8_t flags,
		 const void *data, size_t length)
{
	return tcp_send_segment_at(endpoint, endpoint->tcp.send_next, flags,
	    data, length);
}

static void
tcp_retransmit_clear(struct tcp_endpoint *endpoint)
{
	struct socket *socket = &endpoint->tcp.inet.socket;

	packet_buf_free(endpoint->tcp.retransmit);
	endpoint->tcp.retransmit = NULL;
	endpoint->tcp.retransmit_deadline = 0;
	endpoint->tcp.retransmit_count = 0;
	if (socket->send_waiter != NULL)
		sched_wakeup(socket->send_waiter);
}

static int
tcp_send_reliable(struct tcp_endpoint *endpoint, uint8_t flags,
		  const void *data, size_t length)
{
	struct packet_buf *copy;
	void *payload;
	int error;

	if (endpoint->tcp.retransmit != NULL)
		return EAGAIN;
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
	error = tcp_send_segment(endpoint, flags, data, length);
	if (error != 0) {
		packet_buf_free(copy);
		return error;
	}
	endpoint->tcp.retransmit = copy;
	endpoint->tcp.retransmit_sequence = endpoint->tcp.send_next;
	endpoint->tcp.retransmit_flags = flags;
	endpoint->tcp.retransmit_count = 0;
	endpoint->tcp.retransmit_deadline = sched_ticks() + TCP_INITIAL_RTO;
	net_worker_wakeup();
	return 0;
}

static int
tcp_bind(struct socket *socket, const struct sockaddr *address,
	 socklen_t length)
{
	struct tcp_endpoint *endpoint = tcp_endpoint(socket);
	int error = inet_socket_bind(&endpoint->tcp.inet, address, length);

	if (error != 0)
		return error;
	if (endpoint->tcp.inet.local_port == 0)
		return tcp_allocate_port(endpoint);
	return tcp_port_in_use(endpoint, endpoint->tcp.inet.local_address,
	    endpoint->tcp.inet.local_port) ? EADDRINUSE : 0;
}

static int
tcp_listen(struct socket *socket, int backlog)
{
	struct tcp_endpoint *endpoint = tcp_endpoint(socket);

	if (endpoint->tcp.state != TCP_CLOSED)
		return endpoint->tcp.state == TCP_LISTEN ? 0 : EISCONN;
	if ((endpoint->tcp.inet.inet_flags & INET_SOCKET_BOUND) == 0 ||
	    endpoint->tcp.inet.local_port == 0)
		return EDESTADDRREQ;
	if (backlog < 0)
		backlog = 0;
	if ((unsigned)backlog > TCP_LISTEN_BACKLOG_MAX)
		backlog = TCP_LISTEN_BACKLOG_MAX;
	endpoint->tcp.listen_backlog = backlog == 0 ? 1U : (unsigned)backlog;
	endpoint->tcp.state = TCP_LISTEN;
	return 0;
}

static int
tcp_accept(struct socket *socket, struct socket **result,
	struct sockaddr *address, socklen_t *length, unsigned io_flags)
{
	struct tcp_endpoint *listener = tcp_endpoint(socket);
	struct tcp_socket *accepted;
	struct thread *thread = thread_current();
	int error;

	if (result == NULL || listener->tcp.state != TCP_LISTEN)
		return EINVAL;
	while (listener->tcp.accept_head == NULL) {
		if ((io_flags & SOCKET_IO_NONBLOCK) != 0 || thread == NULL)
			return EAGAIN;
		if (signal_pending_unblocked(thread))
			return EINTR;
		socket->accept_waiter = thread;
		sched_sleep(0);
		socket->accept_waiter = NULL;
		if (socket->error != 0)
			return socket_take_error(socket);
	}
	accepted = listener->tcp.accept_head;
	if (address != NULL && length != NULL) {
		error = inet_socket_getpeername(&accepted->inet, address, length);
		if (error != 0)
			return error;
	}
	listener->tcp.accept_head = accepted->queue_next;
	if (listener->tcp.accept_head == NULL)
		listener->tcp.accept_tail = NULL;
	if (listener->tcp.accept_count != 0)
		listener->tcp.accept_count--;
	accepted->queue_next = NULL;
	accepted->listener = NULL;
	*result = &accepted->inet.socket;
	return 0;
}

static int
tcp_connect(struct socket *socket, const struct sockaddr *address,
	    socklen_t length, unsigned io_flags)
{
	struct tcp_endpoint *endpoint = tcp_endpoint(socket);
	struct thread *thread = thread_current();
	unsigned attempt;
	int error;

	if (endpoint->tcp.state == TCP_SYN_SENT)
		return EALREADY;
	if (endpoint->tcp.state != TCP_CLOSED)
		return EISCONN;
	error = inet_socket_connect(&endpoint->tcp.inet, address, length);
	if (error != 0)
		return error;
	if (endpoint->tcp.inet.remote_port == 0)
		return EADDRNOTAVAIL;
	if (endpoint->tcp.inet.local_port == 0 &&
	    (error = tcp_allocate_port(endpoint)) != 0)
		return error;
	endpoint->tcp.send_next = (uint32_t)sched_ticks() * 1103515245U +
	    endpoint->tcp.inet.local_port;
	endpoint->tcp.send_unacknowledged = endpoint->tcp.send_next;
	endpoint->tcp.state = TCP_SYN_SENT;
	for (attempt = 0; attempt < 4U; attempt++) {
		error = tcp_send_reliable(endpoint, TCP_SYN, NULL, 0);
		if (error == 0)
			break;
		if (error != EAGAIN && error != EBUSY && error != ENOBUFS) {
			endpoint->tcp.state = TCP_CLOSED;
			return error;
		}
		if (thread != NULL)
			sched_sleep(sched_ticks() + 25U);
	}
	if (error != 0) {
		endpoint->tcp.state = TCP_CLOSED;
		return error;
	}
	endpoint->tcp.send_next++;
	if ((io_flags & SOCKET_IO_NONBLOCK) != 0)
		return EINPROGRESS;
	if (thread == NULL)
		return EAGAIN;
	socket->connect_waiter = thread;
	while (endpoint->tcp.state == TCP_SYN_SENT && socket->error == 0)
		sched_sleep(0);
	socket->connect_waiter = NULL;
	if (socket->error != 0) {
		error = socket_take_error(socket);
		return error;
	}
	return endpoint->tcp.state == TCP_ESTABLISHED ? 0 : ETIMEDOUT;
}

static ssize_t
tcp_sendto(struct socket *socket, const void *buffer, size_t length, int flags,
	   const struct sockaddr *address, socklen_t address_length)
{
	struct tcp_endpoint *endpoint = tcp_endpoint(socket);
	struct thread *thread = thread_current();
	unsigned attempt;
	int error;

	(void)address_length;
	if ((flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) != 0 || address != NULL ||
	    (buffer == NULL && length != 0))
		return -EINVAL;
	if (socket->write_shutdown) {
		if ((flags & MSG_NOSIGNAL) == 0 && thread != NULL &&
		    thread->proc != NULL)
			(void)signal_send_process(thread->proc, SIGPIPE);
		return -EPIPE;
	}
	if (endpoint->tcp.state != TCP_ESTABLISHED)
		return -ENOTCONN;
	if (length == 0)
		return 0;
	if (length > TCP_MSS)
		length = TCP_MSS;
	while (endpoint->tcp.send_unacknowledged != endpoint->tcp.send_next) {
		if ((flags & MSG_DONTWAIT) != 0 || thread == NULL)
			return -EAGAIN;
		socket->send_waiter = thread;
		sched_sleep(0);
		socket->send_waiter = NULL;
		if (socket->error != 0) {
			error = socket_take_error(socket);
			return -error;
		}
	}
	for (attempt = 0; ; attempt++) {
		error = tcp_send_reliable(endpoint, TCP_ACK | TCP_PSH,
		    buffer, length);
		if (error != EBUSY && error != ENOBUFS)
			break;
		if ((flags & MSG_DONTWAIT) != 0 || thread == NULL || attempt >= 99U)
			return -EAGAIN;
		sched_sleep(sched_ticks() + 1U);
	}
	if (error != 0)
		return -error;
	endpoint->tcp.send_next += (uint32_t)length;
	return (ssize_t)length;
}

static ssize_t
tcp_recvfrom(struct socket *socket, void *buffer, size_t length, int flags,
	     struct sockaddr *address, socklen_t *address_length)
{
	struct tcp_endpoint *endpoint = tcp_endpoint(socket);
	struct packet_buf *packet;
	size_t copied;
	int error;

	(void)address;
	(void)address_length;
	if (socket->read_shutdown)
		return 0;
	if (endpoint->tcp.state == TCP_CLOSE_WAIT && socket->receive_head == NULL)
		return 0;
	error = socket_dequeue_packet(socket, flags, &packet);
	if (error != 0)
		return -error;
	copied = length < packet->length ? length : packet->length;
	if (copied != 0)
		memcpy(buffer, packet->data, copied);
	/* Retain a short unread suffix by placing it back at the queue head. */
	if (copied < packet->length) {
		(void)packet_buf_pull(packet, copied);
		(void)socket_requeue_packet_front(socket, packet);
	} else {
		packet_buf_free(packet);
	}
	return (ssize_t)copied;
}

static int tcp_shutdown(struct socket *socket, int how)
{
	struct tcp_endpoint *endpoint = tcp_endpoint(socket);
	int error;

	if (how != SHUT_WR && how != SHUT_RDWR && how != SHUT_RD)
		return EINVAL;
	if (how == SHUT_RD || how == SHUT_RDWR)
		socket->read_shutdown = 1;
	if (how == SHUT_RD)
		return 0;
	if (socket->write_shutdown)
		return 0;
	if (endpoint->tcp.state != TCP_ESTABLISHED &&
	    endpoint->tcp.state != TCP_CLOSE_WAIT)
		return ENOTCONN;
	error = tcp_send_reliable(endpoint, TCP_FIN | TCP_ACK, NULL, 0);
	if (error == 0) {
		socket->write_shutdown = 1;
		endpoint->tcp.send_next++;
		endpoint->tcp.state = endpoint->tcp.state == TCP_CLOSE_WAIT ?
		    TCP_LAST_ACK : TCP_FIN_WAIT_1;
	}
	return error;
}

static int tcp_getsockname(struct socket *socket, struct sockaddr *address,
			   socklen_t *length)
{
	return inet_socket_getsockname(&tcp_endpoint(socket)->tcp.inet,
	    address, length);
}

static int tcp_getpeername(struct socket *socket, struct sockaddr *address,
			   socklen_t *length)
{
	return inet_socket_getpeername(&tcp_endpoint(socket)->tcp.inet,
	    address, length);
}

static void
tcp_close(struct socket *socket)
{
	struct tcp_endpoint *endpoint = tcp_endpoint(socket);
	struct tcp_endpoint **link;
	struct tcp_socket *queued;

	if (endpoint->tcp.state == TCP_ESTABLISHED ||
	    endpoint->tcp.state == TCP_CLOSE_WAIT)
		(void)tcp_shutdown(socket, SHUT_RDWR);
	if (endpoint->tcp.listener != NULL)
		tcp_listener_remove(endpoint);
	while ((queued = endpoint->tcp.half_open_head) != NULL) {
		endpoint->tcp.half_open_head = queued->queue_next;
		queued->queue_next = NULL;
		queued->listener = NULL;
		socket_set_error(&queued->inet.socket, ECONNABORTED);
		socket_release(&queued->inet.socket);
	}
	while ((queued = endpoint->tcp.accept_head) != NULL) {
		endpoint->tcp.accept_head = queued->queue_next;
		queued->queue_next = NULL;
		queued->listener = NULL;
		socket_set_error(&queued->inet.socket, ECONNABORTED);
		socket_release(&queued->inet.socket);
	}
	for (link = &tcp_sockets; *link != NULL; link = &(*link)->next)
		if (*link == endpoint) {
			*link = endpoint->next;
			break;
		}
	tcp_retransmit_clear(endpoint);
	kern_free(endpoint);
}

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
	.close = tcp_close,
};

int
tcp_socket_create(int protocol, struct socket **result)
{
	struct tcp_endpoint *endpoint;

	if (result == NULL || (protocol != 0 && protocol != IPPROTO_TCP))
		return EPROTONOSUPPORT;
	endpoint = kern_calloc(1, sizeof(*endpoint));
	if (endpoint == NULL)
		return ENOMEM;
	inet_socket_object_init(&endpoint->tcp.inet, SOCK_STREAM, IPPROTO_TCP,
	    &tcp_ops);
	endpoint->tcp.state = TCP_CLOSED;
	endpoint->next = tcp_sockets;
	tcp_sockets = endpoint;
	*result = &endpoint->tcp.inet.socket;
	return 0;
}

static struct tcp_endpoint *
tcp_lookup(uint32_t source, uint32_t destination, uint16_t source_port,
	   uint16_t destination_port)
{
	struct tcp_endpoint *endpoint, *wildcard = NULL;

	for (endpoint = tcp_sockets; endpoint != NULL; endpoint = endpoint->next)
		if (endpoint->tcp.state != TCP_LISTEN &&
		    endpoint->tcp.inet.local_port == destination_port &&
		    endpoint->tcp.inet.remote_port == source_port &&
		    endpoint->tcp.inet.remote_address == source &&
		    (endpoint->tcp.inet.local_address == 0 ||
		     endpoint->tcp.inet.local_address == destination))
			return endpoint;
	for (endpoint = tcp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		if (endpoint->tcp.state != TCP_LISTEN ||
		    endpoint->tcp.inet.local_port != destination_port)
			continue;
		if (endpoint->tcp.inet.local_address == destination)
			return endpoint;
		if (endpoint->tcp.inet.local_address == 0)
			wildcard = endpoint;
	}
	return wildcard;
}

static struct tcp_endpoint *
tcp_passive_syn(struct tcp_endpoint *listener, uint32_t source,
	uint32_t destination, uint16_t source_port, uint32_t sequence)
{
	struct socket *created;
	struct tcp_endpoint *child;
	int error;

	if (listener->tcp.half_open_count + listener->tcp.accept_count >=
	    listener->tcp.listen_backlog)
		return NULL;
	error = socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP, &created);
	if (error != 0)
		return NULL;
	child = tcp_endpoint(created);
	child->tcp.inet.local_address = listener->tcp.inet.local_address == 0 ?
	    destination : listener->tcp.inet.local_address;
	child->tcp.inet.local_port = listener->tcp.inet.local_port;
	child->tcp.inet.remote_address = source;
	child->tcp.inet.remote_port = source_port;
	child->tcp.inet.ifindex = listener->tcp.inet.ifindex;
	child->tcp.inet.inet_flags = INET_SOCKET_BOUND | INET_SOCKET_CONNECTED;
	child->tcp.receive_next = sequence + 1U;
	child->tcp.send_next = (uint32_t)sched_ticks() * 1103515245U +
	    child->tcp.inet.local_port + source_port;
	child->tcp.send_unacknowledged = child->tcp.send_next;
	child->tcp.state = TCP_SYN_RECEIVED;
	child->tcp.listener = &listener->tcp;
	child->tcp.inet.socket.receive_timeout_ticks =
	    listener->tcp.inet.socket.receive_timeout_ticks;
	child->tcp.inet.socket.send_timeout_ticks =
	    listener->tcp.inet.socket.send_timeout_ticks;
	child->tcp.inet.socket.reuse_address =
	    listener->tcp.inet.socket.reuse_address;
	child->tcp.queue_next = listener->tcp.half_open_head;
	listener->tcp.half_open_head = &child->tcp;
	listener->tcp.half_open_count++;
	error = tcp_send_reliable(child, TCP_SYN | TCP_ACK, NULL, 0);
	if (error != 0) {
		tcp_listener_remove(child);
		socket_release(created);
		return NULL;
	}
	child->tcp.send_next++;
	return child;
}

static int
tcp_input(struct packet_buf *packet, uint32_t source, uint32_t destination)
{
	const struct tcp_wire *tcp;
	struct tcp_endpoint *endpoint;
	uint32_t sequence, acknowledgement;
	uint16_t source_port, destination_port;
	size_t header_length, payload_length;
	uint8_t flags;

	if (packet == NULL || packet->length < sizeof(*tcp) ||
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
	if (endpoint->tcp.state == TCP_LISTEN) {
		if ((flags & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN)
			(void)tcp_passive_syn(endpoint, source, destination,
			    source_port, sequence);
		packet_buf_free(packet);
		return 0;
	}
	if (flags & TCP_RST) {
		socket_set_error(&endpoint->tcp.inet.socket, ECONNRESET);
		endpoint->tcp.state = TCP_CLOSED;
		if (endpoint->tcp.listener != NULL) {
			tcp_listener_remove(endpoint);
			packet_buf_free(packet);
			socket_release(&endpoint->tcp.inet.socket);
			return 0;
		}
		if (endpoint->tcp.inet.socket.connect_waiter != NULL)
			sched_wakeup(endpoint->tcp.inet.socket.connect_waiter);
		packet_buf_free(packet);
		return 0;
	}
	if (endpoint->tcp.state == TCP_SYN_RECEIVED) {
		if ((flags & TCP_ACK) != 0 &&
		    acknowledgement == endpoint->tcp.send_next) {
			endpoint->tcp.send_unacknowledged = acknowledgement;
			tcp_retransmit_clear(endpoint);
			endpoint->tcp.state = TCP_ESTABLISHED;
			tcp_listener_established(endpoint);
		}
		packet_buf_free(packet);
		return 0;
	}
	if (endpoint->tcp.state == TCP_SYN_SENT) {
		if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
		    acknowledgement == endpoint->tcp.send_next) {
			endpoint->tcp.send_unacknowledged = acknowledgement;
			tcp_retransmit_clear(endpoint);
			endpoint->tcp.receive_next = sequence + 1U;
			endpoint->tcp.peer_window = wire_get16(tcp->window);
			endpoint->tcp.state = TCP_ESTABLISHED;
			(void)tcp_send_segment(endpoint, TCP_ACK, NULL, 0);
			if (endpoint->tcp.inet.socket.connect_waiter != NULL)
				sched_wakeup(endpoint->tcp.inet.socket.connect_waiter);
		}
		packet_buf_free(packet);
		return 0;
	}
	if ((flags & TCP_ACK) != 0 &&
	    acknowledgement >= endpoint->tcp.send_unacknowledged &&
	    acknowledgement <= endpoint->tcp.send_next) {
		endpoint->tcp.send_unacknowledged = acknowledgement;
		if (acknowledgement == endpoint->tcp.send_next)
			tcp_retransmit_clear(endpoint);
	}
	if (payload_length != 0 && sequence == endpoint->tcp.receive_next) {
		if (packet_buf_pull(packet, header_length) == NULL) {
			packet_buf_free(packet);
			return EINVAL;
		}
		endpoint->tcp.receive_next += (uint32_t)payload_length;
		if (!endpoint->tcp.inet.socket.read_shutdown) {
			(void)socket_enqueue_packet(&endpoint->tcp.inet.socket, packet);
			packet = NULL;
		}
		(void)tcp_send_segment(endpoint, TCP_ACK, NULL, 0);
	}
	if ((flags & TCP_FIN) != 0 &&
	    sequence + (uint32_t)payload_length == endpoint->tcp.receive_next) {
		endpoint->tcp.receive_next++;
		if (endpoint->tcp.state == TCP_ESTABLISHED)
			endpoint->tcp.state = TCP_CLOSE_WAIT;
		else if (endpoint->tcp.state == TCP_FIN_WAIT_1 ||
		    endpoint->tcp.state == TCP_FIN_WAIT_2)
			endpoint->tcp.state = TCP_TIME_WAIT;
		(void)tcp_send_segment(endpoint, TCP_ACK, NULL, 0);
		if (endpoint->tcp.inet.socket.receive_head == NULL) {
			struct packet_buf *eof = packet_buf_alloc(0);
			if (eof != NULL)
				(void)socket_enqueue_packet(&endpoint->tcp.inet.socket,
				    eof);
		} else if (endpoint->tcp.inet.socket.receive_waiter != NULL) {
			sched_wakeup(endpoint->tcp.inet.socket.receive_waiter);
		}
	}
	if (endpoint->tcp.state == TCP_FIN_WAIT_1 &&
	    endpoint->tcp.send_unacknowledged == endpoint->tcp.send_next)
		endpoint->tcp.state = TCP_FIN_WAIT_2;
	if (endpoint->tcp.state == TCP_LAST_ACK &&
	    endpoint->tcp.send_unacknowledged == endpoint->tcp.send_next)
		endpoint->tcp.state = TCP_CLOSED;
	if (packet != NULL)
		packet_buf_free(packet);
	return 0;
}

int
tcp_init(void)
{
	tcp_sockets = NULL;
	next_ephemeral = TCP_EPHEMERAL_FIRST;
	return ipv4_protocol_register(IPPROTO_TCP, tcp_input);
}

void
tcp_timer_run(void)
{
	struct tcp_endpoint *endpoint, *next;
	uint64_t now = sched_ticks();

	for (endpoint = tcp_sockets; endpoint != NULL; endpoint = next) {
		struct socket *socket = &endpoint->tcp.inet.socket;
		uint64_t delay;
		int error;
		next = endpoint->next;

		if (endpoint->tcp.retransmit == NULL ||
		    endpoint->tcp.retransmit_deadline > now)
			continue;
		if (endpoint->tcp.retransmit_count >= TCP_RETRANSMIT_MAX) {
			tcp_retransmit_clear(endpoint);
			endpoint->tcp.state = TCP_CLOSED;
			socket_set_error(socket, ETIMEDOUT);
			if (endpoint->tcp.listener != NULL) {
				tcp_listener_remove(endpoint);
				socket_release(socket);
				continue;
			}
			if (socket->connect_waiter != NULL)
				sched_wakeup(socket->connect_waiter);
			if (socket->receive_waiter != NULL)
				sched_wakeup(socket->receive_waiter);
			continue;
		}
		error = tcp_send_segment_at(endpoint,
		    endpoint->tcp.retransmit_sequence,
		    endpoint->tcp.retransmit_flags,
		    endpoint->tcp.retransmit->data,
		    endpoint->tcp.retransmit->length);
		if (error != 0 && error != EAGAIN && error != ENOBUFS) {
			tcp_retransmit_clear(endpoint);
			endpoint->tcp.state = TCP_CLOSED;
			socket_set_error(socket, error);
			if (endpoint->tcp.listener != NULL) {
				tcp_listener_remove(endpoint);
				socket_release(socket);
				continue;
			}
			if (socket->connect_waiter != NULL)
				sched_wakeup(socket->connect_waiter);
			continue;
		}
		endpoint->tcp.retransmit_count++;
		delay = TCP_INITIAL_RTO <<
		    (endpoint->tcp.retransmit_count < 3U ?
		    endpoint->tcp.retransmit_count : 3U);
		endpoint->tcp.retransmit_deadline = now + delay;
	}
}

uint64_t
tcp_timer_next_deadline(void)
{
	const struct tcp_endpoint *endpoint;
	uint64_t deadline = 0;

	for (endpoint = tcp_sockets; endpoint != NULL; endpoint = endpoint->next)
		if (endpoint->tcp.retransmit != NULL &&
		    (deadline == 0 ||
		    endpoint->tcp.retransmit_deadline < deadline))
			deadline = endpoint->tcp.retransmit_deadline;
	return deadline;
}
