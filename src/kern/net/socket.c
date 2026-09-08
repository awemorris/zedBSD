/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The socket core.
 *
 * Address families register their creation hook here, and every socket
 * shares the generic state: reference count, lifecycle, the receive
 * queue with its packet and byte limits, the wait queues, the pending
 * error, and the common socket options.  Closing an endpoint runs the
 * family's close exactly once.
 */

#include "kern/net/socket.h"
#include "kern/net/packet-buf.h"
#include "kern/clock.h"
#include "kern/kmem.h"
#include "kern/poll.h"
#include "kern/sched.h"
#include "kern/signal.h"
#include "kern/syscall.h"
#include "kern/thread.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#define SOCKET_FAMILY_MAX 32U

static const struct socket_family_ops *families[SOCKET_FAMILY_MAX];
static atomic_uint_t socket_count;
static struct spinlock socket_registry_lock;

static void socket_wake_queue(struct socket *socket, struct wait_queue *queue);

/*
 * Initializes the family registry and the socket count.
 */
void
socket_core_init(
	void)
{
	memset(families, 0, sizeof(families));
	atomic_store_release(&socket_count, 0);
	spin_init(&socket_registry_lock, LOCK_RANK_SOCKET_REGISTRY,
	    "socket registry");
}

/*
 * Registers the operations of an address family.
 */
int
socket_family_register(
	int family,
	const struct socket_family_ops *ops)
{
	unsigned long irq;
	int error;

	error = 0;

	/* Rejects a family out of range or operations without create. */
	if (family < 0 ||
	    family >= (int)SOCKET_FAMILY_MAX ||
	    ops == NULL ||
	    ops->create == NULL)
		return EINVAL;

	/* Installs the operations unless the family is taken. */
	irq = spin_lock_irqsave(&socket_registry_lock);

	if (families[family] != NULL)
		error = EEXIST;
	else
		families[family] = ops;

	spin_unlock_irqrestore(&socket_registry_lock, irq);

	/* Reports why the registration failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Initializes the generic part of a socket object.
 */
void
socket_init_object(
	struct socket *socket,
	int family,
	int type,
	int protocol,
	const struct socket_ops *ops)
{
	/* Starts open, referenced once, with the default buffer limits. */
	memset(socket, 0, sizeof(*socket));
	socket->family = family;
	socket->type = type;
	socket->protocol = protocol;
	socket->ops = ops;
	refcount_init(&socket->refs, 1);
	spin_init(&socket->lock, LOCK_RANK_SOCKET, "socket");
	waitq_init(&socket->receive_waitq, "socket receive");
	waitq_init(&socket->receive_space_waitq, "socket receive space");
	waitq_init(&socket->send_waitq, "socket send");
	waitq_init(&socket->connect_waitq, "socket connect");
	waitq_init(&socket->accept_waitq, "socket accept");
	socket->lifecycle = SOCKET_OPEN;
	socket->receive_packet_limit = SOCKET_RECEIVE_MESSAGES_MAX;
	socket->receive_hiwat_bytes = SOCKET_BUFFER_DEFAULT;
	socket->send_hiwat_bytes = SOCKET_BUFFER_DEFAULT;
}

/*
 * Creates a socket through its address family.
 *
 * The system-wide socket count is claimed before the family creates the
 * socket and given back when that fails.
 */
int
socket_create(
	int family,
	int type,
	int protocol,
	struct socket **result)
{
	const struct socket_family_ops *family_ops;
	unsigned count;
	unsigned expected;
	unsigned long irq;
	int error;

	/* Rejects a missing result, a family out of range, or an unknown type. */
	if (result == NULL ||
	    family < 0 ||
	    family >= (int)SOCKET_FAMILY_MAX ||
	    (type != SOCK_RAW && type != SOCK_DGRAM && type != SOCK_STREAM))
		return EINVAL;

	/* Looks the family up. */
	irq = spin_lock_irqsave(&socket_registry_lock);

	family_ops = families[family];

	spin_unlock_irqrestore(&socket_registry_lock, irq);

	if (family_ops == NULL)
		return EAFNOSUPPORT;

	/* Claims a slot in the socket count. */
	for (;;) {
		count = atomic_load_acquire(&socket_count);
		if (count >= SOCKET_MAX)
			return ENFILE;
		expected = count;
		if (atomic_compare_exchange(&socket_count, &expected, count + 1U))
			break;
	}

	/* Lets the family create the socket, giving the slot back on failure. */
	error = family_ops->create(type, protocol, result);
	if (error != 0)
		(void)atomic_raw_fetch_add_relaxed(&socket_count.value,
		    (unsigned)-1);

	/* Reports why the creation failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Sets a socket-level option every family shares.
 *
 * SO_REUSEADDR, the buffer sizes, and the timeouts are handled here;
 * anything else is reported as ENOPROTOOPT.
 */
int
socket_setsockopt_common(
	struct socket *socket,
	int level,
	int option,
	const void *value,
	socklen_t length)
{
	struct timeval timeout;
	uint64_t ticks;
	uint64_t fraction;
	unsigned long irq;
	int enabled;
	int requested;

	/* Only socket-level options are handled here. */
	if (socket == NULL || level != SOL_SOCKET)
		return ENOPROTOOPT;

	/* SO_REUSEADDR is a flag consulted at bind time. */
	if (option == SO_REUSEADDR) {
		if (value == NULL || length != sizeof(enabled))
			return EINVAL;
		memcpy(&enabled, value, sizeof(enabled));
		irq = spin_lock_irqsave(&socket->lock);
		socket->reuse_address = enabled != 0;
		spin_unlock_irqrestore(&socket->lock, irq);
		return 0;
	}

	/* The buffer sizes are bounded and wake the waiters they affect. */
	if (option == SO_SNDBUF || option == SO_RCVBUF) {
		if (value == NULL || length != sizeof(requested))
			return EINVAL;
		memcpy(&requested, value, sizeof(requested));
		if (requested < (int)SOCKET_BUFFER_MIN ||
		    requested > (int)SOCKET_BUFFER_MAX)
			return EINVAL;
		irq = spin_lock_irqsave(&socket->lock);
		if (option == SO_SNDBUF)
			socket->send_hiwat_bytes = (size_t)requested;
		else
			socket->receive_hiwat_bytes = (size_t)requested;
		waitq_wake_all(&socket->receive_space_waitq);
		waitq_wake_all(&socket->send_waitq);
		spin_unlock_irqrestore(&socket->lock, irq);
		if (socket->ops != NULL && socket->ops->buffer_changed != NULL)
			socket->ops->buffer_changed(socket, option);
		poll_notify();
		return 0;
	}

	/* The timeouts are converted to ticks, rounding up. */
	if (option != SO_RCVTIMEO && option != SO_SNDTIMEO)
		return ENOPROTOOPT;
	if (value == NULL || length != sizeof(timeout))
		return EINVAL;
	memcpy(&timeout, value, sizeof(timeout));
	if (timeout.tv_sec < 0 ||
	    timeout.tv_usec < 0 ||
	    timeout.tv_usec >= 1000000)
		return EINVAL;
	if ((uint64_t)timeout.tv_sec > UINT64_MAX / KERN_CLOCK_HZ)
		return EOVERFLOW;
	ticks = (uint64_t)timeout.tv_sec * KERN_CLOCK_HZ;
	fraction = ((uint64_t)timeout.tv_usec *
	    KERN_CLOCK_HZ + 999999U) / 1000000U;
	if (ticks > UINT64_MAX - fraction)
		return EOVERFLOW;
	ticks += fraction;

	/* Records the timeout. */
	irq = spin_lock_irqsave(&socket->lock);

	if (option == SO_RCVTIMEO)
		socket->receive_timeout_ticks = ticks;
	else
		socket->send_timeout_ticks = ticks;

	spin_unlock_irqrestore(&socket->lock, irq);

	/* Reports the set timeout. */
	return 0;
}

/*
 * Reads a socket-level option every family shares.
 */
int
socket_getsockopt_common(
	struct socket *socket,
	int level,
	int option,
	void *value,
	socklen_t *length)
{
	struct timeval timeout;
	uint64_t ticks;
	unsigned long irq;
	int error;
	int result;
	int enabled;
	int configured;

	/* Only socket-level options are handled here. */
	if (socket == NULL || level != SOL_SOCKET)
		return ENOPROTOOPT;

	/* SO_ERROR reads and clears the pending error. */
	if (option == SO_ERROR) {
		if (value == NULL || length == NULL || *length < sizeof(error))
			return EINVAL;
		error = socket_take_error(socket);
		memcpy(value, &error, sizeof(error));
		*length = sizeof(error);
		return 0;
	}

	/*
	 * The identity options are constants.  The current protocols do not
	 * implement out-of-band data, hence a live socket can never be
	 * positioned at an OOB mark.
	 */
	if (option == SO_TYPE ||
	    option == SO_DOMAIN ||
	    option == SO_PROTOCOL ||
	    option == SO_ATMARK) {
		if (value == NULL || length == NULL || *length < sizeof(result))
			return EINVAL;
		if (option == SO_TYPE)
			result = socket->type;
		else if (option == SO_DOMAIN)
			result = socket->family;
		else if (option == SO_PROTOCOL)
			result = socket->protocol;
		else
			result = 0;
		memcpy(value, &result, sizeof(result));
		*length = sizeof(result);
		return 0;
	}

	/* SO_REUSEADDR reports the flag. */
	if (option == SO_REUSEADDR) {
		if (value == NULL || length == NULL || *length < sizeof(enabled))
			return EINVAL;
		irq = spin_lock_irqsave(&socket->lock);
		enabled = socket->reuse_address != 0;
		spin_unlock_irqrestore(&socket->lock, irq);
		memcpy(value, &enabled, sizeof(enabled));
		*length = sizeof(enabled);
		return 0;
	}

	/* The buffer sizes report the configured limits. */
	if (option == SO_SNDBUF || option == SO_RCVBUF) {
		if (value == NULL ||
		    length == NULL ||
		    *length < sizeof(configured))
			return EINVAL;
		irq = spin_lock_irqsave(&socket->lock);
		if (option == SO_SNDBUF)
			configured = (int)socket->send_hiwat_bytes;
		else
			configured = (int)socket->receive_hiwat_bytes;
		spin_unlock_irqrestore(&socket->lock, irq);
		memcpy(value, &configured, sizeof(configured));
		*length = sizeof(configured);
		return 0;
	}

	/* The timeouts are converted back from ticks. */
	if (option != SO_RCVTIMEO && option != SO_SNDTIMEO)
		return ENOPROTOOPT;
	if (value == NULL || length == NULL || *length < sizeof(timeout))
		return EINVAL;
	irq = spin_lock_irqsave(&socket->lock);

	if (option == SO_RCVTIMEO)
		ticks = socket->receive_timeout_ticks;
	else
		ticks = socket->send_timeout_ticks;

	spin_unlock_irqrestore(&socket->lock, irq);

	timeout.tv_sec = (time_t)(ticks / KERN_CLOCK_HZ);
	timeout.tv_usec = (long)((ticks % KERN_CLOCK_HZ) *
	    (1000000U / KERN_CLOCK_HZ));
	memcpy(value, &timeout, sizeof(timeout));
	*length = sizeof(timeout);

	/* Reports the read timeout. */
	return 0;
}

/*
 * Reads and clears the pending error of a socket.
 */
int
socket_take_error(
	struct socket *socket)
{
	int error;
	unsigned long irq;

	/* Rejects a missing socket. */
	if (socket == NULL)
		return EINVAL;

	/* Takes the error under the lock. */
	irq = spin_lock_irqsave(&socket->lock);

	error = socket->error;
	socket->error = 0;

	spin_unlock_irqrestore(&socket->lock, irq);

	/* Reports the taken error. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Records a pending error and wakes every waiter to see it.
 *
 * The first error is kept until read.
 */
void
socket_set_error(
	struct socket *socket,
	int error)
{
	unsigned long irq;

	/* Ignores a missing socket or no error. */
	if (socket == NULL || error == 0)
		return;

	/* Keeps the first error and wakes everyone. */
	irq = spin_lock_irqsave(&socket->lock);

	if (socket->error == 0)
		socket->error = error;
	waitq_wake_all(&socket->receive_waitq);
	waitq_wake_all(&socket->receive_space_waitq);
	waitq_wake_all(&socket->send_waitq);
	waitq_wake_all(&socket->connect_waitq);
	waitq_wake_all(&socket->accept_waitq);

	spin_unlock_irqrestore(&socket->lock, irq);

	poll_notify();
}

/*
 * Takes a reference on a socket.
 */
void
socket_ref(
	struct socket *socket)
{
	/* Ignores a missing socket. */
	if (socket != NULL)
		refcount_get(&socket->refs);
}

/*
 * Takes a reference on a socket unless its last reference is going away.
 */
int
socket_tryref(
	struct socket *socket)
{
	int acquired;

	/* A missing socket cannot be referenced. */
	if (socket == NULL)
		return 0;

	/* Tries the reference count. */
	acquired = refcount_tryget(&socket->refs);

	/* Reports whether the reference was taken. */
	return acquired;
}

/*
 * Closes the endpoint of a socket exactly once.
 *
 * The socket is marked closing and shut down in both directions before
 * the family's endpoint close runs, and closed afterwards.
 */
void
socket_close_endpoint(
	struct socket *socket)
{
	unsigned long irq;
	int close;

	close = 0;

	/* A family without an endpoint close needs nothing here. */
	if (socket == NULL ||
	    socket->ops == NULL ||
	    socket->ops->endpoint_close == NULL)
		return;

	/* Only the first closer moves the socket out of the open state. */
	irq = spin_lock_irqsave(&socket->lock);

	if (socket->lifecycle == SOCKET_OPEN) {
		socket->lifecycle = SOCKET_CLOSING;
		socket->read_shutdown = 1;
		socket->write_shutdown = 1;
		waitq_wake_all(&socket->receive_waitq);
		waitq_wake_all(&socket->receive_space_waitq);
		waitq_wake_all(&socket->send_waitq);
		waitq_wake_all(&socket->connect_waitq);
		waitq_wake_all(&socket->accept_waitq);
		close = 1;
	}

	spin_unlock_irqrestore(&socket->lock, irq);

	if (!close)
		return;

	/* Closes the endpoint, then publishes the closed state. */
	socket->ops->endpoint_close(socket);
	irq = spin_lock_irqsave(&socket->lock);

	socket->lifecycle = SOCKET_CLOSED;
	waitq_wake_all(&socket->receive_waitq);
	waitq_wake_all(&socket->receive_space_waitq);
	waitq_wake_all(&socket->send_waitq);

	spin_unlock_irqrestore(&socket->lock, irq);

	poll_notify();
}

/*
 * Drops a reference on a socket and destroys it with the last.
 *
 * The receive queue is drained and the family's close frees the object.
 */
void
socket_release(
	struct socket *socket)
{
	struct packet_buf *packet;
	struct packet_buf *packets;
	unsigned long irq;

	/* Only the last reference destroys. */
	if (socket == NULL || !refcount_put(&socket->refs))
		return;

	/* Closes the endpoint and detaches the receive queue. */
	socket_close_endpoint(socket);
	irq = spin_lock_irqsave(&socket->lock);

	if (socket->lifecycle == SOCKET_OPEN)
		socket->lifecycle = SOCKET_CLOSING;
	packets = socket->receive_head;
	socket->receive_head = NULL;
	socket->receive_tail = NULL;
	socket->receive_packets = 0;
	socket->receive_bytes = 0;
	waitq_wake_all(&socket->receive_waitq);
	waitq_wake_all(&socket->receive_space_waitq);
	waitq_wake_all(&socket->send_waitq);
	waitq_wake_all(&socket->connect_waitq);
	waitq_wake_all(&socket->accept_waitq);

	spin_unlock_irqrestore(&socket->lock, irq);

	poll_notify();

	/* Frees the queued packets. */
	for (;;) {
		packet = packets;
		if (packet == NULL)
			break;
		packets = packet->next;
		packet_buf_free(packet);
	}

	/* Lets the family free the object and gives the count slot back. */
	if (socket->ops != NULL && socket->ops->close != NULL)
		socket->ops->close(socket);
	(void)atomic_raw_fetch_add_relaxed(&socket_count.value, (unsigned)-1);
}

/*
 * Reports the number of sockets.
 */
unsigned
socket_count_current(
	void)
{
	unsigned count;

	count = atomic_load_acquire(&socket_count);

	/* Reports the sampled count. */
	return count;
}

/*
 * Queues a received packet on a socket without waiting.
 *
 * The packet is consumed: dropped with ENOBUFS when the queue is full or
 * EPIPE when the socket is no longer open.
 */
int
socket_enqueue_packet(
	struct socket *socket,
	struct packet_buf *packet)
{
	unsigned long irq;
	int error;

	/* Rejects a missing socket or packet. */
	if (socket == NULL || packet == NULL) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Drops the packet when the socket is closed or the queue is full. */
	irq = spin_lock_irqsave(&socket->lock);

	if (socket->lifecycle != SOCKET_OPEN ||
	    (socket->receive_packet_limit != 0 &&
	    socket->receive_packets >= socket->receive_packet_limit) ||
	    packet->length > socket->receive_hiwat_bytes ||
	    socket->receive_bytes > socket->receive_hiwat_bytes - packet->length) {
		if (socket->lifecycle != SOCKET_OPEN)
			error = EPIPE;
		else
			error = ENOBUFS;
		spin_unlock_irqrestore(&socket->lock, irq);
		packet_buf_free(packet);
		return error;
	}

	/* Appends the packet and wakes one receiver. */
	packet->next = NULL;
	if (socket->receive_tail != NULL)
		socket->receive_tail->next = packet;
	else
		socket->receive_head = packet;
	socket->receive_tail = packet;
	socket->receive_packets++;
	socket->receive_bytes += packet->length;
	waitq_wake_one(&socket->receive_waitq);

	spin_unlock_irqrestore(&socket->lock, irq);

	poll_notify();

	/* Reports the queued packet. */
	return 0;
}

/*
 * Queues a packet on a socket, waiting for room in its receive queue.
 *
 * The packet is consumed either way.  A full queue reports EAGAIN when
 * the caller cannot wait or the timeout expires.
 */
int
socket_enqueue_packet_wait(
	struct socket *socket,
	struct packet_buf *packet,
	int flags,
	uint64_t timeout_ticks)
{
	uint64_t deadline;
	uint64_t sequence;
	unsigned long irq;
	int full;
	int error;

	deadline = 0;
	error = 0;

	/* Rejects a missing socket or packet, or unknown flags. */
	if (socket == NULL || packet == NULL || (flags & ~MSG_DONTWAIT) != 0) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Converts the timeout to a deadline. */
	if (timeout_ticks != 0 &&
	    syscall_restart_deadline_after(timeout_ticks, &deadline) != 0) {
		packet_buf_free(packet);
		return EOVERFLOW;
	}

	/* Queues the packet as soon as it fits. */
	irq = spin_lock_irqsave(&socket->lock);

	for (;;) {
		full = 0;
		if ((socket->receive_packet_limit != 0 &&
		     socket->receive_packets >= socket->receive_packet_limit) ||
		    packet->length > socket->receive_hiwat_bytes ||
		    socket->receive_bytes >
		    socket->receive_hiwat_bytes - packet->length)
			full = 1;

		/* A closed or read-shut socket takes nothing more. */
		if (socket->lifecycle != SOCKET_OPEN || socket->read_shutdown) {
			error = EPIPE;
			break;
		}

		/* Appends the packet and wakes one receiver. */
		if (!full) {
			packet->next = NULL;
			if (socket->receive_tail != NULL)
				socket->receive_tail->next = packet;
			else
				socket->receive_head = packet;
			socket->receive_tail = packet;
			socket->receive_packets++;
			socket->receive_bytes += packet->length;
			waitq_wake_one(&socket->receive_waitq);
			break;
		}

		/* Gives up without a thread to sleep, or past the deadline. */
		if ((flags & MSG_DONTWAIT) != 0 || thread_current() == NULL) {
			error = EAGAIN;
			break;
		}

		if (deadline != 0 && sched_ticks() >= deadline) {
			error = EAGAIN;
			break;
		}

		/* Sleeps until a receiver makes room. */
		sequence = waitq_sequence(&socket->receive_space_waitq);
		error = waitq_sleep(&socket->receive_space_waitq,
		    &socket->lock, sequence, deadline, WAITQ_INTERRUPTIBLE);
		if (error == ETIMEDOUT)
			error = EAGAIN;
		if (error != 0)
			break;
	}

	spin_unlock_irqrestore(&socket->lock, irq);

	/* Frees an unqueued packet, or announces the queued one. */
	if (error != 0)
		packet_buf_free(packet);
	else
		poll_notify();

	/* Reports why the queueing failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Puts a packet back at the front of a socket's receive queue.
 *
 * The packet is consumed; a socket that is no longer open drops it.
 */
int
socket_requeue_packet_front(
	struct socket *socket,
	struct packet_buf *packet)
{
	unsigned long irq;

	/* Rejects a missing socket or packet. */
	if (socket == NULL || packet == NULL) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* A closed socket takes nothing back. */
	irq = spin_lock_irqsave(&socket->lock);

	if (socket->lifecycle != SOCKET_OPEN) {
		spin_unlock_irqrestore(&socket->lock, irq);
		packet_buf_free(packet);
		return EPIPE;
	}

	/* Links the packet at the head and wakes one receiver. */
	packet->next = socket->receive_head;
	socket->receive_head = packet;
	if (socket->receive_tail == NULL)
		socket->receive_tail = packet;
	socket->receive_packets++;
	socket->receive_bytes += packet->length;
	waitq_wake_one(&socket->receive_waitq);

	spin_unlock_irqrestore(&socket->lock, irq);

	poll_notify();

	/* Reports the requeued packet. */
	return 0;
}

/*
 * Takes the next packet from a socket's receive queue.
 *
 * An empty queue reports a pending error, EPIPE on a closed socket, or
 * EAGAIN when the caller cannot wait or the receive timeout expires.
 */
int
socket_dequeue_packet(
	struct socket *socket,
	int flags,
	struct packet_buf **result)
{
	uint64_t deadline;
	uint64_t sequence;
	unsigned long irq;
	int error;

	/* Rejects a missing socket or result, or unknown flags. */
	deadline = 0;
	if (socket == NULL || result == NULL || (flags & ~MSG_DONTWAIT) != 0)
		return EINVAL;

	/* Converts the receive timeout to a deadline. */
	irq = spin_lock_irqsave(&socket->lock);

	if (socket->receive_timeout_ticks != 0 &&
	    syscall_restart_deadline_after(socket->receive_timeout_ticks,
	    &deadline) != 0) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return EOVERFLOW;
	}

	/* Waits for a packet, reporting whatever ends the wait first. */
	while (socket->receive_head == NULL) {
		if (socket->error != 0) {
			error = socket->error;
			socket->error = 0;
			spin_unlock_irqrestore(&socket->lock, irq);
			return error;
		}

		if (socket->lifecycle != SOCKET_OPEN) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return EPIPE;
		}

		if ((flags & MSG_DONTWAIT) != 0 || thread_current() == NULL) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return EAGAIN;
		}

		if (deadline != 0 && sched_ticks() >= deadline) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return EAGAIN;
		}

		/* Sleeps until a packet arrives. */
		sequence = waitq_sequence(&socket->receive_waitq);
		error = waitq_sleep(&socket->receive_waitq, &socket->lock,
		    sequence, deadline, WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return EINTR;
		}

		if (error == ETIMEDOUT) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return EAGAIN;
		}
	}

	/* Unlinks the head packet and wakes the senders waiting for room. */
	*result = socket->receive_head;
	socket->receive_head = (*result)->next;
	if (socket->receive_head == NULL)
		socket->receive_tail = NULL;
	(*result)->next = NULL;
	if (socket->receive_packets != 0)
		socket->receive_packets--;
	if (socket->receive_bytes >= (*result)->length)
		socket->receive_bytes -= (*result)->length;
	waitq_wake_all(&socket->receive_space_waitq);

	spin_unlock_irqrestore(&socket->lock, irq);

	/* Reports the taken packet. */
	return 0;
}

/*
 * Wakes the receivers of a socket.
 */
void
socket_wake_receive(
	struct socket *socket)
{
	/* Ignores a missing socket. */
	if (socket != NULL)
		socket_wake_queue(socket, &socket->receive_waitq);
}

/*
 * Wakes the senders of a socket.
 */
void
socket_wake_send(
	struct socket *socket)
{
	/* Ignores a missing socket. */
	if (socket != NULL)
		socket_wake_queue(socket, &socket->send_waitq);
}

/*
 * Wakes the connectors of a socket.
 */
void
socket_wake_connect(
	struct socket *socket)
{
	/* Ignores a missing socket. */
	if (socket != NULL)
		socket_wake_queue(socket, &socket->connect_waitq);
}

/*
 * Wakes the acceptors of a socket.
 */
void
socket_wake_accept(
	struct socket *socket)
{
	/* Ignores a missing socket. */
	if (socket != NULL)
		socket_wake_queue(socket, &socket->accept_waitq);
}

/*
 * Reports the readiness of a socket from its generic state.
 */
int
socket_poll_common(
	struct socket *socket,
	short events,
	short *revents)
{
	short result;
	unsigned long irq;

	result = 0;

	/* Rejects a missing socket or result. */
	if (socket == NULL || revents == NULL)
		return EINVAL;

	/* Derives readiness from the queue, the shutdowns, and the error. */
	irq = spin_lock_irqsave(&socket->lock);

	if (socket->receive_head != NULL ||
	    socket->read_shutdown ||
	    socket->lifecycle != SOCKET_OPEN)
		result |= events & (POLLIN | POLLRDNORM);
	if (socket->error != 0)
		result |= POLLERR;
	if (socket->lifecycle != SOCKET_OPEN || socket->read_shutdown)
		result |= POLLHUP;
	if (!socket->write_shutdown && socket->lifecycle == SOCKET_OPEN)
		result |= events & (POLLOUT | POLLWRNORM);
	else if (socket->write_shutdown)
		result |= POLLERR;

	spin_unlock_irqrestore(&socket->lock, irq);

	*revents = result;

	/* Reports the derived readiness. */
	return 0;
}

/* Wakes one wait queue of a socket under its lock and notifies pollers. */
static void
socket_wake_queue(
	struct socket *socket,
	struct wait_queue *queue)
{
	unsigned long irq;

	/* Ignores a missing socket. */
	if (socket == NULL)
		return;

	/* Wakes under the lock so that a sleeper never misses the sequence. */
	irq = spin_lock_irqsave(&socket->lock);

	waitq_wake_all(queue);

	spin_unlock_irqrestore(&socket->lock, irq);

	poll_notify();
}
