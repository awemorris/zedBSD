/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The TCP socket implementation.
 *
 * A socket may have several segments outstanding at once, held oldest first
 * in a fixed ring.  How many is bounded by that ring and by the window the
 * peer advertises, so a sender does not have to wait a round trip for each
 * segment.  An acknowledgement retires every segment it covers.  The network
 * worker's timer resends the oldest outstanding segment with exponential
 * backoff until it is acknowledged or the attempt is abandoned; the ones
 * behind it follow once it is taken.
 *
 * Listeners hold half-open children until their handshake completes and then
 * queue them for accept.  Payload is delivered in order only; anything else
 * is dropped and re-acknowledged.
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
#include "kern/process.h"
#include "kern/thread.h"
#include "internal.h"
#include "wire.h"
#include <kern/kcrt.h>

#include <uapi/netinet.h>
#include <uapi/errno.h>

#define TCP_FIN 0x01U
#define TCP_SYN 0x02U
#define TCP_RST 0x04U
#define TCP_PSH 0x08U
#define TCP_ACK 0x10U
#define TCP_MSS 1024U
#define TCP_EPHEMERAL_FIRST 49152U
/* The initial retransmission timeout of RFC 6298, one second. */
#define TCP_INITIAL_RTO KERN_MS_TO_TICKS(1000U)
/* How long closing a connection waits for what it sent to be taken. */
#define TCP_CLOSE_DRAIN_MS 10000U

/* What a closing connection waits for. */
enum tcp_drain {
	/* A free entry in the retransmission ring, for the FIN. */
	TCP_DRAIN_ROOM,
	/* Every byte of data acknowledged; a FIN may still be outstanding. */
	TCP_DRAIN_DATA,
	/* Everything acknowledged, the FIN included. */
	TCP_DRAIN_ALL,
};
/* The pause before sending again when buffers are short. */
#define TCP_SYN_RETRY_MS 250U
#define TCP_SEND_RETRY_MS 10U
#define TCP_RETRANSMIT_MAX 5U

/*
 * Idle probing for SO_KEEPALIVE, in clock ticks.  The defaults are the
 * traditional ones: probe after two hours of silence, then every 75 seconds,
 * and give up after nine unanswered probes.  A build may shorten them so the
 * behaviour can be observed in a test that does not run for hours.
 */
#ifndef CONFIG_TCP_KEEPALIVE_IDLE_MS
#define CONFIG_TCP_KEEPALIVE_IDLE_MS 7200000U
#endif
#ifndef CONFIG_TCP_KEEPALIVE_INTERVAL_MS
#define CONFIG_TCP_KEEPALIVE_INTERVAL_MS 75000U
#endif
#ifndef CONFIG_TCP_KEEPALIVE_COUNT
#define CONFIG_TCP_KEEPALIVE_COUNT 9U
#endif

#define TCP_KEEPALIVE_IDLE \
	((uint64_t)CONFIG_TCP_KEEPALIVE_IDLE_MS * KERN_CLOCK_HZ / 1000U)
#define TCP_KEEPALIVE_INTERVAL \
	((uint64_t)CONFIG_TCP_KEEPALIVE_INTERVAL_MS * KERN_CLOCK_HZ / 1000U)
#define TCP_KEEPALIVE_COUNT ((unsigned)CONFIG_TCP_KEEPALIVE_COUNT)
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
struct tcp_discard {
	struct packet_buf *packets[CONFIG_TCP_SEND_QUEUE_MAX];
	unsigned count;
};

static void tcp_retransmit_reset(struct tcp_endpoint *endpoint, struct tcp_discard *discard);
static void tcp_discard_free(struct tcp_discard *discard);
static uint32_t tcp_in_flight(const struct tcp_endpoint *endpoint);
static void tcp_retire_acknowledged(struct tcp_endpoint *endpoint, uint32_t acknowledgement, struct tcp_discard *discard);
static void tcp_retransmit_clear(struct tcp_endpoint *endpoint);
static void tcp_connect_cancel_locked(struct tcp_endpoint *endpoint, uint32_t generation, struct tcp_discard *discard);
static int tcp_send_reliable(struct tcp_endpoint *endpoint, uint8_t flags, const void *data, size_t length);
static int tcp_bind(struct socket *socket, const struct sockaddr *address, socklen_t length);
static int tcp_listen(struct socket *socket, int backlog);
static int tcp_accept(struct socket *socket, struct socket **result, struct sockaddr *address, socklen_t *length, unsigned io_flags);
static int tcp_connect(struct socket *socket, const struct sockaddr *address, socklen_t length, unsigned io_flags);
static ssize_t tcp_sendto(struct socket *socket, const void *buffer, size_t length, int flags, const struct sockaddr *address, socklen_t address_length);
static ssize_t tcp_recvfrom(struct socket *socket, void *buffer, size_t length, int flags, struct sockaddr *address, socklen_t *address_length);
static uint16_t tcp_receive_window(struct tcp_endpoint *endpoint);
static void tcp_send_reset(uint32_t local, uint32_t remote, const struct tcp_wire *segment, size_t payload_length);
static int tcp_shutdown(struct socket *socket, int how);
static int tcp_getsockname(struct socket *socket, struct sockaddr *address, socklen_t *length);
static int tcp_getpeername(struct socket *socket, struct sockaddr *address, socklen_t *length);
static void tcp_close(struct socket *socket);
static void tcp_close_drain(struct tcp_endpoint *endpoint, enum tcp_drain until);
static void tcp_endpoint_close(struct socket *socket);
static int tcp_poll(struct socket *socket, short events, short *revents);
static int tcp_setsockopt(struct socket *socket, int level, int option, const void *value, socklen_t length);
static int tcp_getsockopt(struct socket *socket, int level, int option, void *value, socklen_t *length);
static void tcp_keepalive_changed(struct socket *socket);
static void tcp_keepalive_arm_locked(struct tcp_endpoint *endpoint);
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
	.endpoint_close = tcp_endpoint_close,
	.setsockopt = tcp_setsockopt,
	.getsockopt = tcp_getsockopt,
	.keepalive_changed = tcp_keepalive_changed,
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
 * Decides what an expired idle probe means for one socket.
 *
 * The caller holds the socket lock.  Returns 1 when a probe should be sent,
 * -1 when the peer has failed to answer every probe allowed, and 0 when the
 * socket no longer needs one.
 */
static int
tcp_keepalive_expire_locked(
	struct tcp_endpoint *endpoint,
	uint64_t now)
{
	/* Stops probing a connection that is no longer established. */
	if (endpoint->tcp.inet.socket.keepalive == 0 ||
	    endpoint->tcp.state != TCP_ESTABLISHED) {
		endpoint->tcp.keepalive_deadline = 0;
		endpoint->tcp.keepalive_probes = 0;
		return 0;
	}

	/* Declares the peer unreachable once every probe went unanswered. */
	if (endpoint->tcp.keepalive_probes >= TCP_KEEPALIVE_COUNT) {
		endpoint->tcp.keepalive_deadline = 0;
		endpoint->tcp.keepalive_probes = 0;
		endpoint->tcp.state = TCP_CLOSED;
		endpoint->tcp.active_connect_generation = 0;
		endpoint->tcp.connect_wait_deadline = 0;
		return -1;
	}

	/* Schedules the next probe and asks for this one. */
	endpoint->tcp.keepalive_probes++;
	endpoint->tcp.keepalive_deadline = now + TCP_KEEPALIVE_INTERVAL;
	return 1;
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
	struct tcp_discard discard;
	struct tcp_pending *oldest;
	uint8_t payload[TCP_MSS];
	uint8_t flags;
	uint32_t sequence;
	size_t length;
	uint64_t delay;
	unsigned shift;
	int error;
	int failed;
	int probe;

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
		discard.count = 0;
		endpoint = snapshot[index];
		socket = &endpoint->tcp.inet.socket;
		irq = spin_lock_irqsave(&socket->lock);
		if (endpoint->tcp.send_count == 0 ||
		    endpoint->tcp.retransmit_deadline > now) {
			/*
			 * With nothing to retransmit, an armed idle probe is
			 * the other reason this socket may need attention.
			 */
			probe = 0;
			if (endpoint->tcp.send_count == 0 &&
			    endpoint->tcp.keepalive_deadline != 0 &&
			    endpoint->tcp.keepalive_deadline <= now)
				probe = tcp_keepalive_expire_locked(endpoint, now);
			spin_unlock_irqrestore(&socket->lock, irq);

			/* A peer that answered no probe at all is gone. */
			if (probe < 0) {
				tcp_forget_peer(endpoint);
				socket_set_error(socket, ETIMEDOUT);
				socket_wake_connect(socket);
				socket_wake_receive(socket);
				socket_release(socket);
				continue;
			}

			/*
			 * The probe is an acknowledgement for the last byte
			 * already sent, which the peer answers without
			 * delivering anything to its reader.
			 */
			if (probe > 0)
				(void)tcp_send_segment_at(endpoint,
				    endpoint->tcp.send_next - 1U, TCP_ACK,
				    NULL, 0);
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
			tcp_discard_free(&discard);
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

		/*
		 * The oldest outstanding segment is the one a timeout
		 * resends; anything after it follows once the peer
		 * acknowledges this one.  It is copied out so the send can
		 * happen with the lock released.
		 */
		oldest = &endpoint->tcp.send_queue[endpoint->tcp.send_first];
		sequence = oldest->sequence;
		flags = oldest->flags;
		length = oldest->packet->length;
		if (length > sizeof(payload)) {
			tcp_retransmit_reset(endpoint, &discard);
			endpoint->tcp.state = TCP_CLOSED;
			endpoint->tcp.active_connect_generation = 0;
			endpoint->tcp.connect_wait_deadline = 0;
			tcp_forget_peer(endpoint);
			spin_unlock_irqrestore(&socket->lock, irq);
			tcp_discard_free(&discard);
			socket_set_error(socket, EIO);
			socket_release(socket);
			continue;
		}

		if (length != 0)
			kern_memcpy(payload, oldest->packet->data, length);

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
			if (endpoint->tcp.send_count != 0 &&
			    endpoint->tcp.send_queue[endpoint->tcp.send_first]
				.sequence == sequence) {
				tcp_retransmit_reset(endpoint, &discard);
				endpoint->tcp.state = TCP_CLOSED;
				endpoint->tcp.active_connect_generation = 0;
				endpoint->tcp.connect_wait_deadline = 0;
				tcp_forget_peer(endpoint);
				failed = 1;
			}

			spin_unlock_irqrestore(&socket->lock, irq);
			tcp_discard_free(&discard);
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
		if (endpoint->tcp.send_count != 0)
			candidate = endpoint->tcp.retransmit_deadline;
		else
			candidate = endpoint->tcp.keepalive_deadline;
		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		if (candidate != 0 && (deadline == 0 || candidate < deadline))
			deadline = candidate;
	}

	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	/* Reports the earliest deadline. */
	return deadline;
}

/*
 * Arms or disarms the idle probe for one socket.
 *
 * The caller holds the socket lock.  Only an established connection probes:
 * there is nothing to keep alive before it exists or after it is gone.
 */
static void
tcp_keepalive_arm_locked(
	struct tcp_endpoint *endpoint)
{
	endpoint->tcp.keepalive_probes = 0;

	/* Disarms when the option is off or the connection is not up. */
	if (endpoint->tcp.inet.socket.keepalive == 0 ||
	    endpoint->tcp.state != TCP_ESTABLISHED) {
		endpoint->tcp.keepalive_deadline = 0;
		return;
	}

	/* Restarts the idle period. */
	endpoint->tcp.keepalive_deadline = sched_ticks() + TCP_KEEPALIVE_IDLE;
}

/*
 * Reacts to SO_KEEPALIVE being set or cleared on an existing socket.
 */
static void
tcp_keepalive_changed(
	struct socket *socket)
{
	struct tcp_endpoint *endpoint;
	unsigned long irq;

	endpoint = tcp_endpoint(socket);

	/* Handles a socket that is not a TCP endpoint. */
	if (endpoint == NULL)
		return;
	irq = spin_lock_irqsave(&socket->lock);
	tcp_keepalive_arm_locked(endpoint);
	spin_unlock_irqrestore(&socket->lock, irq);
}

/*
 * Sets one option at the TCP protocol level.
 */
static int
tcp_setsockopt(
	struct socket *socket,
	int level,
	int option,
	const void *value,
	socklen_t length)
{
	struct tcp_endpoint *endpoint;
	unsigned long irq;
	int enabled;

	/* Only this protocol's own level is handled here. */
	if (socket == NULL || level != IPPROTO_TCP)
		return ENOPROTOOPT;
	if (option != TCP_NODELAY)
		return ENOPROTOOPT;
	if (value == NULL || length != sizeof(enabled))
		return EINVAL;
	kern_memcpy(&enabled, value, sizeof(enabled));

	endpoint = tcp_endpoint(socket);

	/* Handles a socket that is not a TCP endpoint. */
	if (endpoint == NULL)
		return ENOPROTOOPT;

	/*
	 * The stored value is what getsockopt reports.  This implementation
	 * sends each write as its own segment as soon as the previous one is
	 * acknowledged, so it never accumulates small writes; clearing the
	 * option therefore does not introduce the delay the name refers to.
	 */
	irq = spin_lock_irqsave(&socket->lock);
	endpoint->tcp.nodelay = enabled != 0;
	spin_unlock_irqrestore(&socket->lock, irq);

	/* Reports successful completion. */
	return 0;
}

/*
 * Reads one option at the TCP protocol level.
 */
static int
tcp_getsockopt(
	struct socket *socket,
	int level,
	int option,
	void *value,
	socklen_t *length)
{
	struct tcp_endpoint *endpoint;
	unsigned long irq;
	int enabled;

	/* Only this protocol's own level is handled here. */
	if (socket == NULL || level != IPPROTO_TCP)
		return ENOPROTOOPT;
	if (option != TCP_NODELAY)
		return ENOPROTOOPT;
	if (value == NULL || length == NULL || *length < sizeof(enabled))
		return EINVAL;

	endpoint = tcp_endpoint(socket);

	/* Handles a socket that is not a TCP endpoint. */
	if (endpoint == NULL)
		return ENOPROTOOPT;
	irq = spin_lock_irqsave(&socket->lock);
	enabled = endpoint->tcp.nodelay != 0;
	spin_unlock_irqrestore(&socket->lock, irq);
	kern_memcpy(value, &enabled, sizeof(enabled));
	*length = sizeof(enabled);

	/* Reports successful completion. */
	return 0;
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
	uint16_t window;
	uint8_t *option;
	size_t options;
	void *payload;
	int error;

	/*
	 * A SYN carries the maximum segment size this end takes: one receive
	 * slot, TCP_MSS bytes.  The receive window counts a full segment per
	 * free slot, so a peer that sent larger segments would find it closing
	 * faster than it opens; without the option a peer assumes its own size.
	 */
	options = 0;
	if ((flags & TCP_SYN) != 0)
		options = 4U;

	/* Routes the segment and allocates its buffer. */
	error = tcp_route(endpoint, &device, &source);
	if (error != 0)
		return error;
	packet = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
	if (packet == NULL) {
		net_device_release(device);
		return ENOBUFS;
	}

	tcp = packet_buf_append(packet, sizeof(*tcp) + options);
	payload = packet_buf_append(packet, length);
	if (tcp == NULL || payload == NULL) {
		packet_buf_free(packet);
		net_device_release(device);
		return ENOBUFS;
	}

	/* Fills the header with the current receive state and the free space. */
	kern_memset(tcp, 0, sizeof(*tcp));
	if (length != 0)
		kern_memcpy(payload, data, length);
	wire_put16(tcp->source, endpoint->tcp.inet.local_port);
	wire_put16(tcp->destination, endpoint->tcp.inet.remote_port);
	wire_put32(tcp->sequence, sequence);
	wire_put32(tcp->acknowledgement, endpoint->tcp.receive_next);
	tcp->data_offset = (uint8_t)((5U + options / 4U) << 4);
	tcp->flags = flags;

	/* Writes the MSS option (kind 2, length 4) after the fixed header. */
	if (options != 0) {
		option = (uint8_t *)(tcp + 1);
		option[0] = 2U;
		option[1] = 4U;
		option[2] = (uint8_t)(TCP_MSS >> 8);
		option[3] = (uint8_t)(TCP_MSS & 0xffU);
	}

	window = tcp_receive_window(endpoint);
	endpoint->tcp.advertised_window = window;
	wire_put16(tcp->window, window);
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
	struct tcp_discard *discard)
{
	unsigned index;
	unsigned slot;

	/* Hands every outstanding payload to the caller to free unlocked. */
	discard->count = 0;
	for (index = 0; index < endpoint->tcp.send_count; index++) {
		slot = (endpoint->tcp.send_first + index) % TCP_SEND_QUEUE_MAX;
		discard->packets[discard->count++] =
		    endpoint->tcp.send_queue[slot].packet;
		endpoint->tcp.send_queue[slot].packet = NULL;
	}
	endpoint->tcp.send_first = 0;
	endpoint->tcp.send_count = 0;
	endpoint->tcp.retransmit_deadline = 0;
	endpoint->tcp.retransmit_count = 0;
}

/* Frees the payloads a reset handed over, outside any lock. */
static void
tcp_discard_free(
	struct tcp_discard *discard)
{
	unsigned index;

	/* Process each remaining element. */
	for (index = 0; index < discard->count; index++)
		packet_buf_free(discard->packets[index]);
	discard->count = 0;
}

/* Reports the sequence space the outstanding segments occupy. */
static uint32_t
tcp_in_flight(
	const struct tcp_endpoint *endpoint)
{
	/* Returns the computed result. */
	return endpoint->tcp.send_next - endpoint->tcp.send_unacknowledged;
}

/*
 * Retires every segment the acknowledgement covers.
 *
 * The caller holds the socket lock and frees the returned payloads once it
 * has released it.  The deadline restarts for whatever is still outstanding.
 */
static void
tcp_retire_acknowledged(
	struct tcp_endpoint *endpoint,
	uint32_t acknowledgement,
	struct tcp_discard *discard)
{
	struct tcp_pending *entry;
	uint32_t end;
	int progressed = 0;
	int recovering;

	/* Process input until it is exhausted. */
	while (endpoint->tcp.send_count != 0) {
		entry = &endpoint->tcp.send_queue[endpoint->tcp.send_first];
		end = entry->sequence + entry->advance;

		/*
		 * Stops at the first segment the peer has not taken.  The
		 * comparison is signed so that it stays correct where the
		 * sequence numbers wrap.
		 */
		if ((int32_t)(acknowledgement - end) < 0)
			break;

		discard->packets[discard->count++] = entry->packet;
		entry->packet = NULL;
		endpoint->tcp.send_first =
		    (endpoint->tcp.send_first + 1U) % TCP_SEND_QUEUE_MAX;
		endpoint->tcp.send_count--;
		progressed = 1;
	}

	/*
	 * The timer follows whatever is now the oldest segment.  An
	 * acknowledgement for a segment that had to be resent shows the peer
	 * is back, and the segments sent after the lost one were most likely
	 * lost with it (the peer keeps only in-order data), so the next one is
	 * resent at once rather than after another timeout.
	 */
	recovering = progressed && endpoint->tcp.retransmit_count != 0;
	endpoint->tcp.retransmit_count = 0;
	if (endpoint->tcp.send_count == 0)
		endpoint->tcp.retransmit_deadline = 0;
	else if (recovering)
		endpoint->tcp.retransmit_deadline = sched_ticks();
	else
		endpoint->tcp.retransmit_deadline =
		    sched_ticks() + TCP_INITIAL_RTO;
}

/* Drops the retransmission record and wakes senders. */
static void
tcp_retransmit_clear(
	struct tcp_endpoint *endpoint)
{
	struct socket *socket;
	struct tcp_discard packet;
	unsigned long irq;

	/* Takes the queued segment out under the socket lock. */
	socket = &endpoint->tcp.inet.socket;
	irq = spin_lock_irqsave(&socket->lock);

	tcp_retransmit_reset(endpoint, &packet);

	spin_unlock_irqrestore(&socket->lock, irq);

	/* Frees them and lets a blocked sender continue. */
	tcp_discard_free(&packet);
	socket_wake_send(socket);
}

/* Abandons an active connect of a given generation; the caller holds the socket lock. */
static void
tcp_connect_cancel_locked(
	struct tcp_endpoint *endpoint,
	uint32_t generation,
	struct tcp_discard *discard)
{
	struct socket *socket;

	socket = &endpoint->tcp.inet.socket;
	discard->count = 0;

	/*
	 * The generation check prevents a waiter from cancelling a later
	 * attempt which reused the same socket.
	 */
	if (endpoint->tcp.state != TCP_SYN_SENT ||
	    endpoint->tcp.active_connect_generation != generation)
		return;

	/* Closes the socket and wakes everyone waiting on the attempt. */
	tcp_retransmit_reset(endpoint, discard);
	endpoint->tcp.state = TCP_CLOSED;
	endpoint->tcp.active_connect_generation = 0;
	endpoint->tcp.connect_wait_deadline = 0;
	tcp_forget_peer(endpoint);
	waitq_wake_all(&socket->connect_waitq);
	waitq_wake_all(&socket->send_waitq);
	poll_notify();
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
	uint32_t window;
	unsigned long irq;
	unsigned slot;
	int control;
	int error;
	struct tcp_discard discard;

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
		kern_memcpy(payload, data, length);

	/* SYN and FIN each consume one sequence number. */
	advance = (uint32_t)length;
	if ((flags & (TCP_SYN | TCP_FIN)) != 0)
		advance++;

	/*
	 * The segment joins the outstanding ones if both the ring and the
	 * peer's advertised window have room for it.  A control segment is
	 * always admitted: refusing a SYN or a FIN for want of window would
	 * stall the connection itself rather than the data on it.
	 */
	irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);

	control = (flags & (TCP_SYN | TCP_FIN)) != 0;
	if (endpoint->tcp.send_count >= TCP_SEND_QUEUE_MAX) {
		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, irq);
		packet_buf_free(copy);
		return EAGAIN;
	}
	if (!control && length != 0) {
		window = endpoint->tcp.peer_window;

		/* An unknown or closed window still allows one segment. */
		if (window == 0)
			window = (uint32_t)TCP_MSS;
		if (tcp_in_flight(endpoint) != 0 &&
		    tcp_in_flight(endpoint) + (uint32_t)length > window) {
			spin_unlock_irqrestore(
			    &endpoint->tcp.inet.socket.lock, irq);
			packet_buf_free(copy);
			return EAGAIN;
		}
	}

	sequence = endpoint->tcp.send_next;
	slot = (endpoint->tcp.send_first + endpoint->tcp.send_count) %
	    TCP_SEND_QUEUE_MAX;
	endpoint->tcp.send_queue[slot].packet = copy;
	endpoint->tcp.send_queue[slot].sequence = sequence;
	endpoint->tcp.send_queue[slot].advance = advance;
	endpoint->tcp.send_queue[slot].length = (uint16_t)length;
	endpoint->tcp.send_queue[slot].flags = flags;

	/* The deadline belongs to the oldest segment, so only it starts one. */
	if (endpoint->tcp.send_count == 0) {
		endpoint->tcp.retransmit_count = 0;
		endpoint->tcp.retransmit_deadline =
		    sched_ticks() + TCP_INITIAL_RTO;
	}
	endpoint->tcp.send_count++;
	endpoint->tcp.send_next += advance;

	spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, irq);

	/*
	 * Publish the retransmission record before output.  A fast ACK may
	 * then safely retire it, and the timer can never observe an unowned
	 * pointer.
	 */
	error = tcp_send_segment_at(endpoint, sequence, flags, data, length);

	/*
	 * A SYN or FIN the device or the pool had no room for is a segment
	 * lost on its way out: it stays in the ring and the retransmission
	 * timer sends it again, as it would one lost on the wire.  Taken back,
	 * a FIN would be lost for good, since the close that sent it has no
	 * one to try again.  A data segment is still taken back, because its
	 * writer retries at once, which is quicker than the timer.
	 */
	if (control && (error == ENOBUFS || error == EAGAIN || error == EBUSY))
		error = 0;

	if (error != 0) {
		discard.count = 0;
		irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);

		/*
		 * Only the segment just added can be taken back: an earlier
		 * one may already be on the wire.  It is the newest, so it
		 * is at the end of the ring.
		 */
		slot = (endpoint->tcp.send_first + endpoint->tcp.send_count -
		    1U) % TCP_SEND_QUEUE_MAX;
		if (endpoint->tcp.send_count != 0 &&
		    endpoint->tcp.send_queue[slot].packet == copy) {
			discard.packets[discard.count++] = copy;
			endpoint->tcp.send_queue[slot].packet = NULL;
			endpoint->tcp.send_count--;
			endpoint->tcp.send_next = sequence;
			if (endpoint->tcp.send_count == 0)
				endpoint->tcp.retransmit_deadline = 0;
		}

		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, irq);
		tcp_discard_free(&discard);
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
	struct tcp_discard cancelled;
	uint64_t deadline;
	uint64_t timeout;
	uint32_t generation;
	unsigned long irq;
	unsigned attempt;
	int error;
	uint64_t sequence;

	endpoint = tcp_endpoint(socket);
	thread = thread_current();
	cancelled.count = 0;
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
			tcp_connect_cancel_locked(endpoint, generation, &cancelled);
			spin_unlock_irqrestore(&socket->lock, irq);
			tcp_discard_free(&cancelled);
			return error;
		}

		if (thread != NULL)
			sched_sleep(sched_ticks() +
			    kern_ms_to_ticks(TCP_SYN_RETRY_MS));
	}

	if (error != 0) {
		irq = spin_lock_irqsave(&socket->lock);
		tcp_connect_cancel_locked(endpoint, generation, &cancelled);
		spin_unlock_irqrestore(&socket->lock, irq);
		tcp_discard_free(&cancelled);
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
			tcp_connect_cancel_locked(endpoint, generation, &cancelled);
			spin_unlock_irqrestore(&socket->lock, irq);
			tcp_discard_free(&cancelled);
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
			tcp_connect_cancel_locked(endpoint, generation, &cancelled);
			spin_unlock_irqrestore(&socket->lock, irq);
			tcp_discard_free(&cancelled);
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
	enum tcp_state state;
	uint32_t window;
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

	/*
	 * Waits for room rather than for silence: several segments may be
	 * outstanding, so the send proceeds as long as the ring has a free
	 * entry and the peer's window has space for this many more bytes.
	 */
	for (;;) {
		irq = spin_lock_irqsave(&socket->lock);
		window = endpoint->tcp.peer_window;

		/* An unknown or closed window still allows one segment. */
		if (window == 0)
			window = (uint32_t)TCP_MSS;
		if (endpoint->tcp.send_count < TCP_SEND_QUEUE_MAX &&
		    (tcp_in_flight(endpoint) == 0 ||
		     tcp_in_flight(endpoint) + (uint32_t)length <= window)) {
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

	/* Our own traffic also restarts the idle period. */
	irq = spin_lock_irqsave(&socket->lock);
	if (endpoint->tcp.state == TCP_ESTABLISHED && socket->keepalive != 0) {
		endpoint->tcp.keepalive_probes = 0;
		endpoint->tcp.keepalive_deadline =
		    sched_ticks() + TCP_KEEPALIVE_IDLE;
	}
	spin_unlock_irqrestore(&socket->lock, irq);

	/*
	 * Sends the segment, retrying while buffers are short.  The packets
	 * come from one small pool the whole stack shares, so a blocking send
	 * waits for them as long as the connection lasts, as it waits for
	 * window above; only a signal, an error or the end of the connection
	 * ends the wait.
	 */
	for (;;) {
		error = tcp_send_reliable(endpoint, TCP_ACK | TCP_PSH,
		    buffer, length);
		if (error != EAGAIN && error != EBUSY && error != ENOBUFS)
			break;
		if ((flags & MSG_DONTWAIT) != 0 || thread == NULL)
			return -EAGAIN;
		if (signal_pending_unblocked(thread))
			return -EINTR;
		irq = spin_lock_irqsave(&socket->lock);
		error = socket->error;
		socket->error = 0;
		state = endpoint->tcp.state;
		spin_unlock_irqrestore(&socket->lock, irq);
		if (error != 0)
			return -error;
		if (state != TCP_ESTABLISHED && state != TCP_CLOSE_WAIT)
			return -EPIPE;
		sched_sleep(sched_ticks() + kern_ms_to_ticks(TCP_SEND_RETRY_MS));
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
	uint32_t window;
	int update;
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
		kern_memcpy(buffer, packet->data, copied);

	/* Retain a short unread suffix by placing it back at the queue head. */
	if (copied < packet->length) {
		(void)packet_buf_pull(packet, copied);
		(void)socket_requeue_packet_front(socket, packet);
	} else {
		packet_buf_free(packet);
	}

	/*
	 * Tells the peer about room the read has made (RFC 1122 4.2.3.3) when
	 * the window it was told of was nearly closed: too small for a full
	 * segment and now able to take one, or under two segments and now two
	 * segments larger.  A sender whose segments are larger than ours
	 * (QEMU's user network sends 1440 bytes whatever MSS we offer) avoids
	 * sending into a small window and would otherwise wait seconds for its
	 * persist timer each time the queue fills.  A window that was not
	 * nearly closed is left to the next acknowledgement: each update that
	 * acknowledges nothing tells this stack's own sender that its oldest
	 * segment was refused, and it sends that segment again.
	 */
	window = tcp_receive_window(endpoint);
	update = 0;
	if (endpoint->tcp.advertised_window < TCP_MSS && window >= TCP_MSS)
		update = 1;
	else if (endpoint->tcp.advertised_window < 2U * TCP_MSS &&
	    window >= (uint32_t)endpoint->tcp.advertised_window + 2U * TCP_MSS)
		update = 1;

	if (update != 0 &&
	    (endpoint->tcp.state == TCP_ESTABLISHED ||
	     endpoint->tcp.state == TCP_FIN_WAIT_1 ||
	     endpoint->tcp.state == TCP_FIN_WAIT_2))
		(void)tcp_send_segment_at(endpoint, endpoint->tcp.send_next,
		    TCP_ACK, NULL, 0);

	/* Reports the bytes received. */
	return (ssize_t)copied;
}

/*
 * Reports the window to advertise: the room left in the receive queue.
 *
 * The queue is bounded twice, by bytes and by how many packets it may hold
 * (the packets come from one small pool the whole stack shares), so the
 * window is the smaller of the bytes left and a full segment for each packet
 * left.  A peer that keeps within it is never refused.  It is read without
 * the socket lock, because segments are also sent with that lock held; a
 * value a moment old is what any peer sees anyway.  With no window scaling
 * the field holds at most 65535.
 */
static uint16_t
tcp_receive_window(
	struct tcp_endpoint *endpoint)
{
	const struct socket *socket = &endpoint->tcp.inet.socket;
	size_t limit = socket->receive_hiwat_bytes;
	size_t used = socket->receive_bytes;
	size_t room;
	size_t packets;

	/* The bytes left. */
	room = used < limit ? limit - used : 0;

	/* A full segment for each packet the queue may still take. */
	if (socket->receive_packet_limit != 0) {
		packets = socket->receive_packets < socket->receive_packet_limit ?
		    socket->receive_packet_limit - socket->receive_packets : 0;
		if (room > packets * TCP_MSS)
			room = packets * TCP_MSS;
	}

	/* Clamps to what the header can carry. */
	if (room > 0xffffU)
		room = 0xffffU;
	return (uint16_t)room;
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

/* Reports whether the wait a closing connection is making is over. */
static int
tcp_drain_done(
	const struct tcp_endpoint *endpoint,
	enum tcp_drain until)
{
	unsigned index;

	/* Room for one more segment, or nothing outstanding at all. */
	if (until == TCP_DRAIN_ROOM)
		return endpoint->tcp.send_count < TCP_SEND_QUEUE_MAX;
	if (until == TCP_DRAIN_ALL)
		return endpoint->tcp.send_count == 0;

	/* No outstanding segment carries data; a FIN alone does not count. */
	for (index = 0; index < endpoint->tcp.send_count; index++) {
		if (endpoint->tcp.send_queue[(endpoint->tcp.send_first + index) %
		    TCP_SEND_QUEUE_MAX].length != 0)
			return 0;
	}
	return 1;
}

/*
 * Waits, for a bounded time, while a closing connection still needs the
 * peer to take what it sent.
 *
 * The endpoint is freed when the last reference goes, and with it the copies
 * kept for retransmission, so the data sent must be taken before then.  The
 * wait happens while the closing file still holds its reference, so that
 * acknowledgements and the retransmission timer reach the endpoint.  It ends
 * when the condition holds, the connection has failed, or TCP_CLOSE_DRAIN_MS
 * has passed.  A kernel thread (the network
 * worker among them) does not wait: it is what would deliver the
 * acknowledgements.
 */
static void
tcp_close_drain(
	struct tcp_endpoint *endpoint,
	enum tcp_drain until)
{
	struct socket *socket = &endpoint->tcp.inet.socket;
	struct thread *thread = thread_current();
	uint64_t deadline;
	uint64_t sequence;
	unsigned long irq;

	/* Only a process may wait for the network. */
	if (thread == NULL || thread->proc == NULL || thread->proc == &process0)
		return;

	deadline = sched_ticks() + kern_ms_to_ticks(TCP_CLOSE_DRAIN_MS);
	irq = spin_lock_irqsave(&socket->lock);

	/* Waits while the condition does not hold on a connection still alive. */
	while (!tcp_drain_done(endpoint, until) &&
	    endpoint->tcp.state != TCP_CLOSED && socket->error == 0 &&
	    sched_ticks() < deadline) {
		sequence = waitq_sequence(&socket->send_waitq);
		(void)waitq_sleep(&socket->send_waitq, &socket->lock, sequence,
		    deadline, 0);
	}

	spin_unlock_irqrestore(&socket->lock, irq);
}

/*
 * Closes a connection when its last file goes away: sends the FIN and lets
 * what was sent reach the peer.
 *
 * The socket layer has already marked both directions shut, so the FIN is
 * sent here rather than through tcp_shutdown(), which would take the mark to
 * mean it had been sent.  A full retransmission ring is first given time to
 * make room for it.
 */
static void
tcp_endpoint_close(
	struct socket *socket)
{
	struct tcp_endpoint *endpoint = tcp_endpoint(socket);
	enum tcp_state old_state;
	enum tcp_state closing_state;
	unsigned long irq;
	int error;

	/* Only a connection that has not sent its FIN owes one. */
	irq = spin_lock_irqsave(&socket->lock);
	old_state = endpoint->tcp.state;
	spin_unlock_irqrestore(&socket->lock, irq);
	if (old_state != TCP_ESTABLISHED && old_state != TCP_CLOSE_WAIT)
		return;

	/* Makes room for the FIN, then sends it. */
	tcp_close_drain(endpoint, TCP_DRAIN_ROOM);
	closing_state = old_state == TCP_CLOSE_WAIT ? TCP_LAST_ACK :
	    TCP_FIN_WAIT_1;
	irq = spin_lock_irqsave(&socket->lock);
	if (endpoint->tcp.state == old_state)
		endpoint->tcp.state = closing_state;
	spin_unlock_irqrestore(&socket->lock, irq);
	error = tcp_send_reliable(endpoint, TCP_FIN | TCP_ACK, NULL, 0);
	if (error != 0) {
		irq = spin_lock_irqsave(&socket->lock);
		if (endpoint->tcp.state == closing_state)
			endpoint->tcp.state = old_state;
		spin_unlock_irqrestore(&socket->lock, irq);
		return;
	}

	/*
	 * Lets what was sent be taken, the FIN included, for at most the drain
	 * time.  The FIN goes out through the retransmission ring, and a first
	 * transmission can be lost (a USB adapter drops a frame while its one
	 * transfer is busy); freeing the endpoint before the FIN is taken would
	 * leave it never sent again.  Closing second, a peer that has gone
	 * costs only that bounded wait.
	 */
	tcp_close_drain(endpoint, TCP_DRAIN_ALL);
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

		/*
		 * A connection is writable while the send ring has room: a
		 * write does not have to wait for everything already sent to
		 * be acknowledged, only for space to put one more segment.
		 */
		if (!socket->write_shutdown &&
		    endpoint->tcp.state == TCP_ESTABLISHED &&
		    endpoint->tcp.send_count < TCP_SEND_QUEUE_MAX)
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
	struct tcp_discard retransmit;
	uint32_t resend_sequence;
	int resend;
	int established;
	uint32_t ack_sequence;
	int old_payload;
	int accept_payload;
	int discard_payload;
	int queued;
	int window_opened;
	int retired;
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
		tcp_send_reset(destination, source, tcp,
		    packet->length - header_length);
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
		retransmit.count = 0;
		socket_irq = spin_lock_irqsave(
		    &endpoint->tcp.inet.socket.lock);
		tcp_retransmit_reset(endpoint, &retransmit);
		endpoint->tcp.state = TCP_CLOSED;
		endpoint->tcp.active_connect_generation = 0;
		endpoint->tcp.connect_wait_deadline = 0;
		tcp_forget_peer(endpoint);
		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		tcp_discard_free(&retransmit);
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
		retransmit.count = 0;
		resend_sequence = 0;
		resend = 0;
		established = 0;
		socket_irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);
		if ((flags & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN &&
		    sequence + 1U == endpoint->tcp.receive_next) {
			resend = 1;

			/*
			 * The SYN-ACK is the oldest thing outstanding on a
			 * half-open connection, so that is what a repeated
			 * SYN asks to have sent again.
			 */
			resend_sequence = endpoint->tcp.send_count != 0
			    ? endpoint->tcp.send_queue[
				  endpoint->tcp.send_first].sequence
			    : endpoint->tcp.send_next;
		}

		if ((flags & TCP_ACK) != 0 &&
		    acknowledgement == endpoint->tcp.send_next) {
			endpoint->tcp.send_unacknowledged = acknowledgement;
			tcp_retransmit_reset(endpoint, &retransmit);
			endpoint->tcp.state = TCP_ESTABLISHED;
			tcp_keepalive_arm_locked(endpoint);
			established = 1;
		}

		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		tcp_discard_free(&retransmit);
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
		retransmit.count = 0;
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
			tcp_keepalive_arm_locked(endpoint);
			waitq_wake_all(&endpoint->tcp.inet.socket.connect_waitq);
			waitq_wake_all(&endpoint->tcp.inet.socket.send_waitq);
			poll_notify();
			established = 1;
		}

		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		tcp_discard_free(&retransmit);
		if (established)
			(void)tcp_send_segment(endpoint, TCP_ACK, NULL, 0);
		packet_buf_free(packet);
		socket_release(&endpoint->tcp.inet.socket);
		return 0;
	}

	/*
	 * Anything arriving from the peer is proof the connection is alive, so
	 * the idle period starts over and the unanswered probes are forgotten.
	 * This includes the peer's answer to a probe of our own.
	 */
	socket_irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);
	if (endpoint->tcp.state == TCP_ESTABLISHED &&
	    endpoint->tcp.inet.socket.keepalive != 0) {
		endpoint->tcp.keepalive_probes = 0;
		endpoint->tcp.keepalive_deadline =
		    sched_ticks() + TCP_KEEPALIVE_IDLE;
	}
	spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);

	/*
	 * An acknowledgement retires every segment it covers, not only the
	 * oldest, and carries the peer's current window so the next send
	 * knows how much it may put in flight.
	 */
	if ((flags & TCP_ACK) != 0) {
		retransmit.count = 0;
		socket_irq = spin_lock_irqsave(
		    &endpoint->tcp.inet.socket.lock);
		if (acknowledgement >= endpoint->tcp.send_unacknowledged &&
		    acknowledgement <= endpoint->tcp.send_next) {
			endpoint->tcp.send_unacknowledged = acknowledgement;
			tcp_retire_acknowledged(endpoint, acknowledgement,
			    &retransmit);
		}
		window_opened = wire_get16(tcp->window) >
		    endpoint->tcp.peer_window;
		endpoint->tcp.peer_window = wire_get16(tcp->window);

		/*
		 * A window that reopens while segments are still outstanding
		 * and this acknowledgement took none of them means the peer
		 * had no room for the oldest: it was refused, not lost in
		 * flight.  It is sent again now rather than after a timeout.
		 */
		if (window_opened && retransmit.count == 0 &&
		    endpoint->tcp.send_count != 0)
			endpoint->tcp.retransmit_deadline = sched_ticks();

		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		retired = retransmit.count != 0;
		if (retired)
			tcp_discard_free(&retransmit);

		/*
		 * A writer waits for room in flight, which an acknowledgement
		 * makes by retiring segments or by reopening the window; a
		 * window update alone retires nothing but must wake it too.
		 */
		if (retired || window_opened)
			socket_wake_send(&endpoint->tcp.inet.socket);
	}

	/*
	 * In-order payload is queued and then acknowledged; a read shutdown
	 * discards it.  Only payload that was actually queued moves
	 * receive_next: a segment the receive queue has no room for is left
	 * unacknowledged, so the peer sends it again instead of believing it
	 * delivered.  The acknowledgement goes out either way, carrying the
	 * window as it now stands.
	 */
	if (payload_length != 0) {
		socket_irq = spin_lock_irqsave(
		    &endpoint->tcp.inet.socket.lock);
		accept_payload = sequence == endpoint->tcp.receive_next;
		discard_payload = endpoint->tcp.inet.socket.read_shutdown;
		old_payload = (int32_t)(sequence + (uint32_t)payload_length -
		    endpoint->tcp.receive_next) <= 0;
		ack_sequence = endpoint->tcp.send_next;
		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);

		/*
		 * A segment wholly before receive_next was already taken; the
		 * peer sends it again because it did not see the acknowledgement,
		 * so it gets one (RFC 793).  A segment ahead of receive_next is
		 * left unanswered: the peer resends from the gap on its timer, and
		 * answering each such segment makes the two ends of a loopback
		 * connection feed each other.
		 */
		if (!accept_payload) {
			if (old_payload)
				(void)tcp_send_segment_at(endpoint, ack_sequence, TCP_ACK, NULL, 0);
			goto payload_done;
		}

		if (packet_buf_pull(packet, header_length) == NULL) {
			packet_buf_free(packet);
			socket_release(&endpoint->tcp.inet.socket);
			return EINVAL;
		}

		queued = 1;
		if (!discard_payload) {
			queued = socket_enqueue_stream(&endpoint->tcp.inet.socket,
			    packet) == 0;
			packet = NULL;
		}

		socket_irq = spin_lock_irqsave(
		    &endpoint->tcp.inet.socket.lock);
		if (queued && endpoint->tcp.receive_next == sequence)
			endpoint->tcp.receive_next += (uint32_t)payload_length;
		ack_sequence = endpoint->tcp.send_next;
		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		(void)tcp_send_segment_at(endpoint, ack_sequence, TCP_ACK, NULL, 0);
		if (!queued)
			goto fin_done;
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

		if (!accept_fin)
			ack_sequence = endpoint->tcp.send_next;
		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);

		/*
		 * A FIN that is not the next thing, most often one sent again
		 * because the acknowledgement of the first was lost, is answered
		 * the same way; left unanswered, the peer repeats it forever.
		 */
		if (!accept_fin) {
			(void)tcp_send_segment_at(endpoint, ack_sequence, TCP_ACK, NULL, 0);
			goto fin_done;
		}

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

/*
 * Answers a segment that belongs to no connection with a reset (RFC 793).
 *
 * The peer of a connection this end has already freed would otherwise
 * repeat its segment, a FIN most often, until its own timers give up.  A
 * reset is never answered, so two ends cannot exchange them forever.
 */
static void
tcp_send_reset(
	uint32_t local,
	uint32_t remote,
	const struct tcp_wire *segment,
	size_t payload_length)
{
	struct net_route route;
	struct net_device *device;
	struct packet_buf *packet;
	struct tcp_wire *tcp;
	uint32_t sequence;
	uint32_t acknowledgement;
	uint16_t checksum;
	uint8_t flags;
	int error;

	/* Never answers a reset. */
	if ((segment->flags & TCP_RST) != 0)
		return;

	/*
	 * An acknowledging segment is answered from the sequence it expects;
	 * any other is acknowledged past its payload, SYN and FIN.
	 */
	sequence = 0;
	acknowledgement = 0;
	flags = TCP_RST;
	if ((segment->flags & TCP_ACK) != 0) {
		sequence = wire_get32(segment->acknowledgement);
	} else {
		acknowledgement = wire_get32(segment->sequence) + (uint32_t)payload_length;
		if ((segment->flags & TCP_SYN) != 0)
			acknowledgement++;
		if ((segment->flags & TCP_FIN) != 0)
			acknowledgement++;
		flags |= TCP_ACK;
	}

	/* Finds the interface the peer is reached through. */
	error = route_lookup_ref(remote, &route);
	if (error != 0)
		return;

	device = route.device;
	route.device = NULL;
	route_release(&route);
	if (device == NULL)
		return;

	/* Builds the reset. */
	packet = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
	if (packet == NULL) {
		net_device_release(device);
		return;
	}

	tcp = packet_buf_append(packet, sizeof(*tcp));
	if (tcp == NULL) {
		packet_buf_free(packet);
		net_device_release(device);
		return;
	}

	kern_memset(tcp, 0, sizeof(*tcp));
	wire_put16(tcp->source, wire_get16(segment->destination));
	wire_put16(tcp->destination, wire_get16(segment->source));
	wire_put32(tcp->sequence, sequence);
	wire_put32(tcp->acknowledgement, acknowledgement);
	tcp->data_offset = 5U << 4;
	tcp->flags = flags;
	checksum = net_checksum_pseudo(local, remote, IPPROTO_TCP,
	    packet->data, packet->length);
	wire_put16(tcp->checksum, checksum);

	/* Sends it; a reset that is lost is sent again for the next segment. */
	(void)ipv4_output(device, remote, IPPROTO_TCP, packet);
	net_device_release(device);
}
