/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * UNIX domain sockets.
 *
 * A stream socket pair shares a connection record whose two ends can
 * disappear independently; stream data is queued at the receiver as
 * coalesced chunks that carry passed file descriptors on a record
 * boundary.  Datagram sockets exchange packets addressed by the bound
 * pathname.  Receiving is a two-phase transaction so that a copy-out
 * failure leaves the queue untouched.
 */

#include "kern/net/socket.h"
#include "kern/net/packet-buf.h"
#include "kern/file.h"
#include "kern/clock.h"
#include "kern/cred.h"
#include "kern/inode.h"
#include "kern/kmem.h"
#include "kern/mount.h"
#include "kern/namei.h"
#include "kern/poll.h"
#include "kern/sched.h"
#include "kern/signal.h"
#include "kern/syscall.h"
#include "kern/test-fault.h"
#include "kern/thread.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>
#include <zedbsd/poll.h>

#define UNIX_STREAM_CHUNK_SIZE 2048U

struct unix_pending {
	struct socket *socket;
	struct unix_pending *next;
};

struct unix_connection {
	refcount_t refs;
	struct spinlock lock;
	struct socket *ends[2];
};

struct unix_rights {
	unsigned count;
	struct file *files[ZEDBSD_MSG_FD_MAX];
};

struct unix_stream_chunk {
	struct unix_stream_chunk *next;
	size_t begin;
	size_t end;
	struct unix_rights *rights;
	uint8_t data[UNIX_STREAM_CHUNK_SIZE];
};

struct unix_socket {
	struct socket socket;
	struct mutex stream_send_lock;
	struct unix_stream_chunk *stream_head;
	struct unix_stream_chunk *stream_tail;
	struct unix_stream_chunk *reserved_stream;
	size_t stream_bytes;
	struct unix_connection *connection;
	struct socket *datagram_peer;
	unsigned side;
	char path[UNIX_PATH_MAX];
	struct path bound_path;
	unsigned bound;
	unsigned binding_in_progress;
	unsigned connected;
	char peer_path[UNIX_PATH_MAX];
	unsigned listening;
	unsigned backlog;
	unsigned pending_count;
	struct unix_pending *pending_head;
	struct unix_pending *pending_tail;
	struct packet_buf *reserved_packet;
	uint64_t reservation_token;
	unsigned endpoint_closed;
	unsigned connecting;
	struct zedbsd_peercred listener_credential;
	struct zedbsd_peercred peer_credential;
	unsigned listener_credential_valid;
	unsigned peer_credential_valid;
};

static void unix_rights_release(void *pointer);
static ssize_t unix_send_failure(struct unix_rights *rights, int error);
static struct unix_socket * unix_endpoint(struct socket *socket);
static int unix_copy_path(const struct sockaddr *address, socklen_t length, char path[UNIX_PATH_MAX]);
static void unix_store_address(const struct unix_socket *endpoint, struct sockaddr *address, socklen_t *length);
static void unix_store_packet_source(const struct unix_socket *endpoint, struct packet_buf *packet);
static int unix_resolve_endpoint(struct cwdinfo *context, const struct ucred *cred, const struct sockaddr *address, socklen_t length, int type, struct socket **result, char path_text[UNIX_PATH_MAX]);
static int unix_connection_create(struct socket *left, struct socket *right, const struct zedbsd_peercred *left_peer, const struct zedbsd_peercred *right_peer);
static void unix_connection_release(struct unix_connection *connection);
static int unix_peer_ref(struct unix_socket *endpoint, struct socket **result);
static ssize_t unix_send_epipe(struct unix_rights *rights, int flags);
static void unix_stream_chunk_free(struct unix_stream_chunk *chunk);
static int unix_stream_wait_space(struct socket *peer, struct unix_socket *endpoint, size_t send_hiwat, int flags, uint64_t deadline, size_t *available);
static ssize_t unix_stream_send(struct socket *socket, const void *buffer, size_t length, int flags, struct unix_rights *rights);
static ssize_t unix_datagram_send(struct socket *socket, const void *buffer, size_t length, int flags, const struct sockaddr *address, struct unix_rights *rights, struct socket *resolved_peer);
static ssize_t unix_send_internal(struct socket *socket, const void *buffer, size_t length, int flags, const struct sockaddr *address, socklen_t address_length, struct unix_rights *rights, struct socket *resolved_peer);
static ssize_t unix_sendto(struct socket *socket, const void *buffer, size_t length, int flags, const struct sockaddr *address, socklen_t address_length);
static ssize_t unix_recvfrom(struct socket *socket, void *buffer, size_t length, int flags, struct sockaddr *address, socklen_t *address_length);
static int unix_shutdown(struct socket *socket, int how);
static int unix_bind(struct socket *socket, const struct sockaddr *address, socklen_t length);
static void unix_connect_cancel(struct socket *socket);
static int unix_connect_resolved(struct socket *socket, struct socket *listener_socket, const struct zedbsd_peercred *connector_credential, const char *path, unsigned io_flags);
static int unix_connect(struct socket *socket, const struct sockaddr *address, socklen_t length, unsigned io_flags);
static int unix_accept(struct socket *socket, struct socket **result, struct sockaddr *address, socklen_t *length, unsigned io_flags);
static int unix_getsockname(struct socket *socket, struct sockaddr *address, socklen_t *length);
static int unix_getpeername(struct socket *socket, struct sockaddr *address, socklen_t *length);
static int unix_getsockopt(struct socket *socket, int level, int option, void *value, socklen_t *length);
static int unix_poll(struct socket *socket, short events, short *revents);
static void unix_buffer_changed(struct socket *socket, int option);
static void unix_endpoint_close(struct socket *socket);
static void unix_close(struct socket *socket);
static int unix_create(int type, int protocol, struct socket **result);

static const struct socket_ops unix_ops = {
	.bind = unix_bind,
	.connect = unix_connect,
	.accept = unix_accept,
	.sendto = unix_sendto,
	.recvfrom = unix_recvfrom,
	.shutdown = unix_shutdown,
	.getsockname = unix_getsockname,
	.getpeername = unix_getpeername,
	.getsockopt = unix_getsockopt,
	.poll = unix_poll,
	.buffer_changed = unix_buffer_changed,
	.endpoint_close = unix_endpoint_close,
	.close = unix_close,
};

/*
 * Tests whether a socket is bound at exactly a resolved path.
 */
int
unix_socket_bound_path_matches(
	struct socket *socket,
	const struct path *path)
{
	struct unix_socket *endpoint;
	unsigned long irq;
	int matches;

	/* Rejects a missing path or a socket of another family. */
	if (socket == NULL || socket->family != AF_UNIX || path == NULL)
		return 0;

	/* Compares the published bound path under the socket lock. */
	endpoint = unix_endpoint(socket);
	irq = spin_lock_irqsave(&socket->lock);
	matches = endpoint->bound && path_equal(&endpoint->bound_path, path);
	spin_unlock_irqrestore(&socket->lock, irq);

	/* Reports the comparison. */
	return matches;
}

/*
 * Sends a message with passed file descriptors.
 *
 * The files are owned by the call and closed on every failure.
 */
ssize_t
unix_socket_send_message(
	struct socket *socket,
	const void *buffer,
	size_t length,
	int flags,
	const struct sockaddr *address,
	socklen_t address_length,
	struct file **files,
	unsigned count)
{
	struct unix_rights *rights;
	unsigned index;
	ssize_t result;

	rights = NULL;

	/* Rejects a socket of another family or too many files. */
	if (socket == NULL ||
	    socket->family != AF_UNIX ||
	    count > ZEDBSD_MSG_FD_MAX) {
		for (index = 0; index < count; index++)
			(void)file_close(files[index]);
		return -(ssize_t)EOPNOTSUPP;
	}

	/* Packages the files as rights that travel with the data. */
	if (count != 0) {
		rights = kern_calloc(1, sizeof(*rights));
		if (rights == NULL) {
			for (index = 0; index < count; index++)
				(void)file_close(files[index]);
			return -(ssize_t)ENOMEM;
		}
		rights->count = count;
		for (index = 0; index < count; index++)
			rights->files[index] = files[index];
	}
	result = unix_send_internal(socket, buffer, length, flags, address,
				  address_length, rights, NULL);

	/* Reports the send result. */
	return result;
}

/*
 * Sends a message with passed file descriptors, resolving a datagram
 * destination in the caller's directory context.
 */
ssize_t
unix_socket_send_message_at(
	struct socket *socket,
	struct cwdinfo *context,
	const struct ucred *cred,
	const void *buffer,
	size_t length,
	int flags,
	const struct sockaddr *address,
	socklen_t address_length,
	struct file **files,
	unsigned count)
{
	struct unix_rights *rights;
	struct socket *peer;
	unsigned index;
	int error;
	ssize_t result;

	rights = NULL;
	peer = NULL;

	/* Rejects a socket of another family or too many files. */
	if (socket == NULL ||
	    socket->family != AF_UNIX ||
	    count > ZEDBSD_MSG_FD_MAX) {
		for (index = 0; index < count; index++)
			(void)file_close(files[index]);
		return -(ssize_t)EOPNOTSUPP;
	}

	/* Packages the files as rights that travel with the data. */
	if (count != 0) {
		rights = kern_calloc(1, sizeof(*rights));
		if (rights == NULL) {
			for (index = 0; index < count; index++)
				(void)file_close(files[index]);
			return -(ssize_t)ENOMEM;
		}
		rights->count = count;
		for (index = 0; index < count; index++)
			rights->files[index] = files[index];
	}

	/* Resolves a datagram destination before sending. */
	if (address != NULL && socket->type == SOCK_DGRAM) {
		error = unix_resolve_endpoint(context, cred, address,
					      address_length, SOCK_DGRAM, &peer,
					      NULL);
		if (error != 0) {
			result = unix_send_failure(rights, error);
			return result;
		}
	}
	result = unix_send_internal(socket, buffer, length, flags, address,
				  address_length, rights, peer);

	/* Reports the send result. */
	return result;
}

/*
 * Begins a receive transaction, copying data out without dequeuing it.
 *
 * The queued packet or chunk stays reserved until the transaction is
 * committed or aborted, so a later copy-out failure can leave the
 * queue as it was.
 */
ssize_t
unix_socket_receive_begin(
	struct socket *socket,
	void *buffer,
	size_t length,
	int flags,
	struct sockaddr *address,
	socklen_t *address_length,
	unsigned file_capacity,
	struct unix_recv_transaction *transaction)
{
	struct unix_socket *endpoint;
	struct packet_buf *packet;
	struct unix_stream_chunk *chunk;
	struct unix_rights *rights;
	uint64_t deadline;
	unsigned index;
	unsigned delivered;
	unsigned long irq;
	int datagram;
	int error;
	uint64_t sequence;
	struct unix_stream_chunk *current;
	uint8_t *destination;
	size_t available;
	size_t copied;
	socklen_t actual;
	socklen_t address_copied;

	deadline = 0;

	/* Rejects a socket of another family, a missing transaction, or bad flags. */
	if (socket == NULL ||
	    socket->family != AF_UNIX ||
	    transaction == NULL ||
	    ((address == NULL) != (address_length == NULL)) ||
	    file_capacity > ZEDBSD_MSG_FD_MAX)
		return -EINVAL;
	memset(transaction, 0, sizeof(*transaction));
	datagram = socket->type == SOCK_DGRAM;
	if (!datagram && length == 0)
		return 0;
	if ((!datagram &&
	     (address != NULL ||
	      (flags & ~(MSG_DONTWAIT | MSG_PEEK | MSG_WAITALL)) != 0)) ||
	    (datagram && (flags & ~(MSG_DONTWAIT | MSG_PEEK | MSG_TRUNC)) != 0))
		return -EOPNOTSUPP;

	/* Applies the receive timeout as a deadline. */
	endpoint = unix_endpoint(socket);
	irq = spin_lock_irqsave(&socket->lock);
	if (socket->receive_timeout_ticks != 0 &&
	    syscall_restart_deadline_after(socket->receive_timeout_ticks,
					   &deadline) != 0) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return -EOVERFLOW;
	}

	/* Waits for an unreserved packet or chunk, or enough stream bytes. */
	for (;;) {
		if (datagram) {
			packet = socket->receive_head;
			chunk = NULL;
		} else {
			packet = NULL;
			chunk = endpoint->stream_head;
		}
		if (datagram && packet != NULL &&
		    endpoint->reserved_packet == NULL)
			break;
		if (!datagram && chunk != NULL &&
		    endpoint->reserved_stream == NULL) {
			if ((flags & MSG_WAITALL) == 0 ||
			    length == 0 ||
			    endpoint->stream_bytes >= length ||
			    socket->read_shutdown)
				break;
		}
		if (!datagram && chunk == NULL && socket->read_shutdown) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return 0;
		}
		if (socket->error != 0) {
			error = socket->error;
			socket->error = 0;
			spin_unlock_irqrestore(&socket->lock, irq);
			return -(ssize_t)error;
		}
		if (socket->lifecycle != SOCKET_OPEN) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return -EPIPE;
		}
		if ((flags & MSG_DONTWAIT) != 0 || thread_current() == NULL) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return -EAGAIN;
		}
		if (deadline != 0 && sched_ticks() >= deadline) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return -EAGAIN;
		}
		sequence = waitq_sequence(&socket->receive_waitq);
		error = waitq_sleep(&socket->receive_waitq,
				    &socket->lock, sequence, deadline,
				    WAITQ_INTERRUPTIBLE);
		if (error == EINTR || error == ETIMEDOUT) {
			spin_unlock_irqrestore(&socket->lock, irq);
			if (error == EINTR)
				return -EINTR;
			return -EAGAIN;
		}
	}

	/* Passed rights cannot be peeked. */
	if (datagram)
		rights = packet->control;
	else
		rights = chunk->rights;
	if (rights != NULL && (flags & MSG_PEEK) != 0) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return -EOPNOTSUPP;
	}

	/* Reserves the head of the queue for this transaction. */
	if (datagram)
		endpoint->reserved_packet = packet;
	else
		endpoint->reserved_stream = chunk;
	endpoint->reservation_token++;
	if (endpoint->reservation_token == 0)
		endpoint->reservation_token++;
	transaction->socket = socket;
	if (datagram)
		transaction->packet = (void *)packet;
	else
		transaction->packet = (void *)chunk;
	transaction->token = endpoint->reservation_token;
	transaction->datagram = (unsigned)datagram;
	transaction->active = 1;
	socket_ref(socket);
	spin_unlock_irqrestore(&socket->lock, irq);

	/* Copies the datagram, or as many stream chunks as fit up to a rights boundary. */
	if (datagram) {
		if (length < packet->length)
			transaction->copied = length;
		else
			transaction->copied = packet->length;
		transaction->data_truncated =
		    transaction->copied < packet->length;
		if (transaction->copied != 0)
			memcpy(buffer, packet->data, transaction->copied);
	} else {
		current = chunk;
		destination = buffer;
		while (current != NULL && transaction->copied < length) {
			if (current != chunk && current->rights != NULL)
				break;
			available = current->end - current->begin;
			copied = length - transaction->copied;
			if (copied > available)
				copied = available;
			if (copied != 0)
				memcpy(destination + transaction->copied,
				       current->data + current->begin, copied);
			transaction->copied += copied;
			if (copied < available)
				break;
			current = current->next;
		}
	}

	/* Reports the datagram's source address. */
	if (datagram && address != NULL && packet->source_length != 0) {
		actual = packet->source_length;
		if (*address_length < actual)
			address_copied = *address_length;
		else
			address_copied = actual;
		if (address_copied != 0)
			memcpy(address, packet->source_address, address_copied);
		*address_length = actual;
	}

	/* Hands out as many passed files as the caller can take. */
	if (rights == NULL)
		delivered = 0;
	else if (rights->count < file_capacity)
		delivered = rights->count;
	else
		delivered = file_capacity;
	for (index = 0; index < delivered; index++) {
		transaction->files[index] = rights->files[index];
		file_ref(transaction->files[index]);
	}
	transaction->file_count = delivered;
	transaction->control_truncated =
	    rights != NULL && delivered < rights->count;

	/* Reports the full datagram length with MSG_TRUNC, else the copied bytes. */
	if (datagram && (flags & MSG_TRUNC) != 0)
		return (ssize_t)packet->length;
	return (ssize_t)transaction->copied;
}

/*
 * Abandons a receive transaction, leaving the queue as it was.
 */
void
unix_socket_receive_abort(
	struct unix_recv_transaction *transaction)
{
	struct socket *socket;
	struct unix_socket *endpoint;
	unsigned index;
	unsigned long irq;

	/* Ignores an inactive transaction. */
	if (transaction == NULL || !transaction->active)
		return;

	/* Releases the reservation when it is still this transaction's. */
	socket = transaction->socket;
	endpoint = unix_endpoint(socket);
	irq = spin_lock_irqsave(&socket->lock);
	if (endpoint->reservation_token == transaction->token) {
		if (transaction->datagram &&
		    endpoint->reserved_packet == transaction->packet)
			endpoint->reserved_packet = NULL;
		else if (!transaction->datagram &&
			 endpoint->reserved_stream == transaction->packet)
			endpoint->reserved_stream = NULL;
	}
	waitq_wake_all(&socket->receive_waitq);
	spin_unlock_irqrestore(&socket->lock, irq);

	/* Drops the file references that were handed out. */
	for (index = 0; index < transaction->file_count; index++) {
		if (transaction->files[index] != NULL)
			(void)file_close(transaction->files[index]);
	}
	transaction->active = 0;
	socket_release(socket);
	poll_notify();
}

/*
 * Completes a receive transaction, dequeuing what was copied.
 *
 * The handed-out files now belong to the caller's descriptor table.
 * A reservation that was lost meanwhile turns the commit into an abort.
 */
void
unix_socket_receive_commit(
	struct unix_recv_transaction *transaction)
{
	struct socket *socket;
	struct unix_socket *endpoint;
	struct packet_buf *packet;
	struct packet_buf *free_packet;
	struct unix_stream_chunk *chunk;
	struct unix_stream_chunk *free_chunks;
	struct unix_stream_chunk **free_tail;
	void *control;
	void (*control_release)(void *);
	unsigned index;
	unsigned long irq;
	size_t remaining;
	struct unix_rights *stream_rights;
	size_t available;
	size_t consumed;

	free_packet = NULL;
	free_chunks = NULL;
	control = NULL;
	control_release = NULL;

	/* Ignores an inactive transaction. */
	if (transaction == NULL || !transaction->active)
		return;
	socket = transaction->socket;
	endpoint = unix_endpoint(socket);
	if (transaction->datagram) {
		packet = transaction->packet;
		chunk = NULL;
	} else {
		packet = NULL;
		chunk = transaction->packet;
	}
	free_tail = &free_chunks;

	/* The reservation must still cover the head of the queue. */
	irq = spin_lock_irqsave(&socket->lock);
	if (endpoint->reservation_token != transaction->token ||
	    (transaction->datagram && (endpoint->reserved_packet != packet ||
				       socket->receive_head != packet)) ||
	    (!transaction->datagram && (endpoint->reserved_stream != chunk ||
					endpoint->stream_head != chunk))) {
		spin_unlock_irqrestore(&socket->lock, irq);
		unix_socket_receive_abort(transaction);
		return;
	}

	/* Dequeues the datagram, or consumes the copied stream bytes. */
	if (transaction->datagram) {
		socket->receive_head = packet->next;
		if (socket->receive_head == NULL)
			socket->receive_tail = NULL;
		packet->next = NULL;
		if (socket->receive_packets != 0)
			socket->receive_packets--;
		if (socket->receive_bytes >= packet->length)
			socket->receive_bytes -= packet->length;
		free_packet = packet;
	} else {
		remaining = transaction->copied;
		stream_rights = chunk->rights;
		chunk->rights = NULL;
		control = stream_rights;
		if (stream_rights != NULL)
			control_release = unix_rights_release;
		else
			control_release = NULL;
		while (chunk != NULL && remaining != 0) {
			available = chunk->end - chunk->begin;
			if (remaining < available)
				consumed = remaining;
			else
				consumed = available;
			chunk->begin += consumed;
			remaining -= consumed;
			if (chunk->begin != chunk->end)
				break;
			endpoint->stream_head = chunk->next;
			chunk->next = NULL;
			*free_tail = chunk;
			free_tail = &chunk->next;
			chunk = endpoint->stream_head;
		}

		/* An empty chunk that only carried rights is released too. */
		if (transaction->copied == 0 &&
		    chunk != NULL &&
		    chunk->begin == chunk->end &&
		    chunk->rights == NULL) {
			endpoint->stream_head = chunk->next;
			chunk->next = NULL;
			*free_tail = chunk;
			free_tail = &chunk->next;
		}
		if (endpoint->stream_head == NULL)
			endpoint->stream_tail = NULL;
		if (endpoint->stream_bytes >= transaction->copied)
			endpoint->stream_bytes -= transaction->copied;
		else
			endpoint->stream_bytes = 0;
	}

	/* Releases the reservation and wakes the waiters. */
	if (transaction->datagram) {
		endpoint->reserved_packet = NULL;
	} else {
		endpoint->reserved_stream = NULL;
		waitq_wake_all(&socket->receive_space_waitq);
	}
	waitq_wake_all(&socket->receive_waitq);
	spin_unlock_irqrestore(&socket->lock, irq);

	/* Frees the consumed storage outside the lock. */
	if (free_packet != NULL)
		packet_buf_free(free_packet);
	else if (control != NULL && control_release != NULL)
		control_release(control);
	chunk = free_chunks;
	while (chunk != NULL) {
		free_chunks = chunk->next;
		chunk->next = NULL;
		unix_stream_chunk_free(chunk);
		chunk = free_chunks;
	}

	/* Ownership of the files moved to the descriptor table. */
	for (index = 0; index < transaction->file_count; index++)
		transaction->files[index] = NULL;
	transaction->active = 0;
	socket_release(socket);
	poll_notify();
}

/*
 * Receives a message with its passed file descriptors in one step.
 */
ssize_t
unix_socket_receive_message(
	struct socket *socket,
	void *buffer,
	size_t length,
	int flags,
	struct sockaddr *address,
	socklen_t *address_length,
	struct file **files,
	unsigned *file_count,
	unsigned *control_truncated)
{
	struct unix_recv_transaction transaction;
	ssize_t result;
	unsigned index;
	unsigned capacity;

	/* Rejects a socket of another family or a missing result. */
	if (socket == NULL ||
	    socket->family != AF_UNIX ||
	    file_count == NULL ||
	    control_truncated == NULL ||
	    ((address == NULL) != (address_length == NULL)))
		return -(ssize_t)EINVAL;

	/* Runs the transaction, committing unless the caller only peeked. */
	capacity = *file_count;
	result =
	    unix_socket_receive_begin(socket, buffer, length, flags, address,
				      address_length, capacity, &transaction);
	if (result < 0 || !transaction.active)
		return result;
	for (index = 0; index < transaction.file_count; index++)
		files[index] = transaction.files[index];
	*file_count = transaction.file_count;
	*control_truncated = transaction.control_truncated;
	if ((flags & MSG_PEEK) != 0)
		unix_socket_receive_abort(&transaction);
	else
		unix_socket_receive_commit(&transaction);

	/* Reports the received length. */
	return result;
}

/*
 * Binds a socket to a pathname, creating the socket inode.
 *
 * The inode carries the socket before the name is published, and the
 * bound path is published under the socket lock afterwards, so a
 * resolver never sees a half-bound endpoint.
 */
int
unix_socket_bind_path(
	struct socket *socket,
	struct cwdinfo *context,
	const struct ucred *cred,
	mode_t umask,
	const struct sockaddr *address,
	socklen_t length)
{
	struct unix_socket *endpoint;
	struct path parent;
	struct path committed_path;
	struct componentname name;
	struct inode_creation_request creation;
	struct inode *inode;
	char path[UNIX_PATH_MAX];
	char storage[NAME_MAX + 1U];
	unsigned long irq;
	int error;

	inode = NULL;

	/* Rejects a socket of another family or a missing context. */
	if (socket == NULL ||
	    socket->family != AF_UNIX ||
	    context == NULL ||
	    cred == NULL)
		return EINVAL;
	error = unix_copy_path(address, length, path);
	if (error != 0)
		return error;

	/* Only one bind may be in progress and none may have succeeded. */
	endpoint = unix_endpoint(socket);
	irq = spin_lock_irqsave(&socket->lock);
	if (endpoint->bound || endpoint->binding_in_progress) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return EINVAL;
	}
	endpoint->binding_in_progress = 1;
	spin_unlock_irqrestore(&socket->lock, irq);

	/* Creates the socket inode under the parent's VFS transaction. */
	path_init(&parent);
	path_init(&committed_path);
	error = namei_parent_path_at(context, path, &parent, &name, storage);
	if (error == 0)
		mount_vfs_transaction_enter(parent.p_mount);
	if (error == 0)
		error = inode_creation_request_user(parent.p_inode, cred,
		    INODE_SOCKET, 0777U & ~umask, 0, socket, &creation);
	if (error == 0)
		error = inode_mknod(parent.p_inode, &name, &creation, &inode);
	if (error == 0) {
		/*
		 * Acquire references before the socket spin lock.  Ownership of
		 * this temporary path is then transferred as one lock-protected
		 * publication, paired with unix_socket_bound_path_matches() in
		 * every resolver.
		 */
		path_set(&committed_path, parent.p_mount, inode);
		irq = spin_lock_irqsave(&socket->lock);
		endpoint->bound_path = committed_path;
		path_init(&committed_path);
		strcpy(endpoint->path, path);
		endpoint->bound = 1;
		spin_unlock_irqrestore(&socket->lock, irq);
	}

	/* Undoes a creation that could not be published. */
	if (error != 0 && inode != NULL)
		(void)inode_unlink(parent.p_inode, &name);
	if (inode != NULL)
		inode_release(inode);
	if (parent.p_mount != NULL)
		mount_vfs_transaction_leave(parent.p_mount);
	path_release(&parent);
	path_release(&committed_path);
	irq = spin_lock_irqsave(&socket->lock);
	endpoint->binding_in_progress = 0;
	spin_unlock_irqrestore(&socket->lock, irq);

	/* Reports the bind result. */
	return error;
}

/*
 * Puts a bound stream socket into the listening state.
 *
 * The listener's credential is recorded for the peers it accepts.
 */
int
unix_socket_listen(
	struct socket *socket,
	int backlog,
	const struct zedbsd_peercred *listener_credential)
{
	struct unix_socket *endpoint;
	unsigned long irq;

	/* Rejects a socket of another family or type, or a missing credential. */
	if (socket == NULL ||
	    socket->family != AF_UNIX ||
	    listener_credential == NULL)
		return EINVAL;
	if (socket->type != SOCK_STREAM)
		return EOPNOTSUPP;

	/* A listener must be bound and not connected. */
	endpoint = unix_endpoint(socket);
	irq = spin_lock_irqsave(&socket->lock);
	if (!endpoint->bound) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return EDESTADDRREQ;
	}
	if (endpoint->connection != NULL) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return EISCONN;
	}

	/* Clamps the backlog and records the credential on the first listen. */
	if (backlog < 1)
		backlog = 1;
	if (backlog > 16)
		backlog = 16;
	endpoint->backlog = (unsigned)backlog;
	if (!endpoint->listening) {
		endpoint->listener_credential = *listener_credential;
		endpoint->listener_credential_valid = 1;
	}
	endpoint->listening = 1;
	spin_unlock_irqrestore(&socket->lock, irq);

	/* Reports the listening socket. */
	return 0;
}

/*
 * Connects a socket to a pathname resolved in the caller's context.
 */
int
unix_socket_connect_path(
	struct socket *socket,
	struct cwdinfo *context,
	const struct ucred *cred,
	const struct zedbsd_peercred *connector_credential,
	const struct sockaddr *address,
	socklen_t length,
	unsigned io_flags)
{
	struct unix_socket *endpoint;
	struct socket *listener;
	char path[UNIX_PATH_MAX];
	unsigned long irq;
	int error;

	/* Rejects a socket of another family or a missing credential. */
	if (socket == NULL ||
	    socket->family != AF_UNIX ||
	    connector_credential == NULL)
		return EINVAL;

	/* Only one connect may be in progress, and none once connected. */
	endpoint = unix_endpoint(socket);
	irq = spin_lock_irqsave(&socket->lock);
	if (endpoint->connecting) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return EALREADY;
	}
	if (endpoint->connection != NULL) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return EISCONN;
	}
	endpoint->connecting = 1;
	spin_unlock_irqrestore(&socket->lock, irq);

	/* Resolves the listener and completes the connection. */
	error = unix_resolve_endpoint(context, cred, address, length,
				      socket->type, &listener, path);
	if (error != 0) {
		unix_connect_cancel(socket);
		return error;
	}
	error = unix_connect_resolved(socket, listener, connector_credential, path,
				     io_flags);

	/* Reports the connect result. */
	return error;
}

/*
 * Creates a connected pair of sockets.
 */
int
unix_socket_pair_create(
	int type,
	int protocol,
	const struct zedbsd_peercred *creator,
	struct socket **left_result,
	struct socket **right_result)
{
	struct socket *left;
	struct socket *right;
	int error;

	left = NULL;
	right = NULL;

	/* Rejects a missing credential or result. */
	if (creator == NULL || left_result == NULL || right_result == NULL)
		return EINVAL;

	/* Creates both ends and joins them. */
	error = socket_create(AF_UNIX, type, protocol, &left);
	if (error != 0)
		return error;
	error = socket_create(AF_UNIX, type, protocol, &right);
	if (error != 0) {
		socket_release(left);
		return error;
	}
	error = unix_connection_create(left, right, creator, creator);
	if (error != 0) {
		socket_release(left);
		socket_release(right);
		return error;
	}
	*left_result = left;
	*right_result = right;

	/* Reports the connected pair. */
	return 0;
}

/*
 * Registers the UNIX socket family.
 */
int
unix_socket_init(
	void)
{
	static const struct socket_family_ops family_ops = {
		.create = unix_create
	};
	int error;

	error = socket_family_register(AF_UNIX, &family_ops);

	/* Reports the registration result. */
	return error;
}

/* Closes the files of a rights record and frees it. */
static void
unix_rights_release(
	void *pointer)
{
	struct unix_rights *rights;
	unsigned index;

	rights = pointer;

	/* Ignores a missing record. */
	if (rights == NULL)
		return;

	/* Closes each file and frees the record. */
	for (index = 0; index < rights->count; index++) {
		if (rights->files[index] != NULL)
			(void)file_close(rights->files[index]);
	}
	kern_free(rights);
}

/* Releases unsent rights and reports a send error. */
static ssize_t
unix_send_failure(
	struct unix_rights *rights,
	int error)
{
	unix_rights_release(rights);
	return -(ssize_t)error;
}

/* Converts a socket to its UNIX endpoint. */
static struct unix_socket *
unix_endpoint(
	struct socket *socket)
{
	return (struct unix_socket *)socket;
}

/* Copies the pathname out of a socket address. */
static int
unix_copy_path(
	const struct sockaddr *address,
	socklen_t length,
	char path[UNIX_PATH_MAX])
{
	const struct sockaddr_un *local;
	size_t available;
	size_t used;

	local = (const struct sockaddr_un *)address;

	/* Rejects a missing, short, long, or foreign address. */
	if (address == NULL ||
	    length <= offsetof(struct sockaddr_un, sun_path) ||
	    length > sizeof(*local) ||
	    local->sun_family != AF_UNIX)
		return EINVAL;

	/* The path must be non-empty and terminated within the address. */
	available = length - offsetof(struct sockaddr_un, sun_path);
	used = 0;
	while (used < available && local->sun_path[used] != '\0')
		used++;
	if (used == 0 || used == available || used >= UNIX_PATH_MAX)
		return EINVAL;
	memcpy(path, local->sun_path, used);
	path[used] = '\0';
	return 0;
}

/* Stores an endpoint's bound pathname as a socket address. */
static void
unix_store_address(
	const struct unix_socket *endpoint,
	struct sockaddr *address,
	socklen_t *length)
{
	struct sockaddr_un local;
	socklen_t needed;
	socklen_t capacity;
	socklen_t copied;

	/* Builds the address, empty for an unbound endpoint. */
	memset(&local, 0, sizeof(local));
	local.sun_family = AF_UNIX;
	if (endpoint != NULL && endpoint->bound)
		strncpy(local.sun_path, endpoint->path,
			sizeof(local.sun_path) - 1U);
	needed = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
			     strlen(local.sun_path) + 1U);

	/* Copies what fits and reports the full length. */
	capacity = *length;
	if (capacity < needed)
		copied = capacity;
	else
		copied = needed;
	if (copied != 0)
		memcpy(address, &local, copied);
	*length = needed;
}

/* Records the sender's bound pathname in a datagram. */
static void
unix_store_packet_source(
	const struct unix_socket *endpoint,
	struct packet_buf *packet)
{
	struct sockaddr_un source;
	size_t length;

	memset(&source, 0, sizeof(source));
	source.sun_family = AF_UNIX;
	if (endpoint->bound)
		strncpy(source.sun_path, endpoint->path,
			sizeof(source.sun_path) - 1U);
	length = offsetof(struct sockaddr_un, sun_path) +
		 strlen(source.sun_path) + 1U;
	memcpy(packet->source_address, &source, length);
	packet->source_length = (uint8_t)length;
}

/* Resolves a socket address to the referenced endpoint bound at that path. */
static int
unix_resolve_endpoint(
	struct cwdinfo *context,
	const struct ucred *cred,
	const struct sockaddr *address,
	socklen_t length,
	int type,
	struct socket **result,
	char path_text[UNIX_PATH_MAX])
{
	struct path resolved;
	struct socket *socket;
	char path[UNIX_PATH_MAX];
	int error;

	socket = NULL;

	/* Rejects a bad address or a missing context. */
	error = unix_copy_path(address, length, path);
	if (error != 0)
		return error;
	if (context == NULL || cred == NULL)
		return EINVAL;

	/* The path must name a writable socket inode. */
	path_init(&resolved);
	error = namei_path_at(context, path, &resolved);
	if (error == 0 && resolved.p_inode->i_type != INODE_SOCKET)
		error = ENOTSOCK;
	if (error == 0)
		error = vfs_access(resolved.p_inode, cred, W_OK);
	if (error == 0) {
		mutex_lock(&resolved.p_inode->i_lock);
		socket = resolved.p_inode->i_special;
		if (socket == NULL ||
		    socket->type != type ||
		    !socket_tryref(socket))
			socket = NULL;
		mutex_unlock(&resolved.p_inode->i_lock);

		/*
		 * Creation attaches i_special before publishing the pathname so
		 * that lookup can never observe a socket inode without its
		 * endpoint.  The inverse half of that contract is checked here:
		 * the endpoint is not usable until bind() has atomically
		 * published this exact bound path.
		 */
		if (socket != NULL &&
		    !unix_socket_bound_path_matches(socket, &resolved)) {
			socket_release(socket);
			socket = NULL;
		}
		if (socket == NULL)
			error = ECONNREFUSED;
	}
	path_release(&resolved);
	if (error != 0)
		return error;

	/* Reports the endpoint and the text of its path. */
	if (path_text != NULL)
		strcpy(path_text, path);
	*result = socket;
	return 0;
}

/* Joins two sockets with a connection record carrying each other's credential. */
static int
unix_connection_create(
	struct socket *left,
	struct socket *right,
	const struct zedbsd_peercred *left_peer,
	const struct zedbsd_peercred *right_peer)
{
	struct unix_connection *connection;

	/* Rejects a missing credential. */
	if (left_peer == NULL || right_peer == NULL)
		return EINVAL;

	/* Each end holds one reference to the record. */
	connection = kern_calloc(1, sizeof(*connection));
	if (connection == NULL)
		return ENOMEM;
	refcount_init(&connection->refs, 2);
	spin_init(&connection->lock, LOCK_RANK_UNIX_CONNECTION,
		  "unix connection");
	connection->ends[0] = left;
	connection->ends[1] = right;
	unix_endpoint(left)->connection = connection;
	unix_endpoint(left)->side = 0;
	unix_endpoint(right)->connection = connection;
	unix_endpoint(right)->side = 1;
	unix_endpoint(left)->peer_credential = *left_peer;
	unix_endpoint(left)->peer_credential_valid = 1;
	unix_endpoint(right)->peer_credential = *right_peer;
	unix_endpoint(right)->peer_credential_valid = 1;
	return 0;
}

/* Drops one end's reference to a connection record. */
static void
unix_connection_release(
	struct unix_connection *connection)
{
	if (connection != NULL && refcount_put(&connection->refs))
		kern_free(connection);
}

/* Takes a reference to the peer of a connected endpoint. */
static int
unix_peer_ref(
	struct unix_socket *endpoint,
	struct socket **result)
{
	struct unix_connection *connection;
	struct socket *peer;
	unsigned long irq;

	connection = endpoint->connection;

	/* An unconnected endpoint has no peer; a closed one reports EPIPE. */
	if (connection == NULL)
		return ENOTCONN;
	irq = spin_lock_irqsave(&connection->lock);
	peer = connection->ends[endpoint->side ^ 1U];
	if (peer == NULL || !socket_tryref(peer))
		peer = NULL;
	spin_unlock_irqrestore(&connection->lock, irq);
	if (peer == NULL)
		return EPIPE;
	*result = peer;
	return 0;
}

/* Releases unsent rights and reports EPIPE, raising SIGPIPE unless suppressed. */
static ssize_t
unix_send_epipe(
	struct unix_rights *rights,
	int flags)
{
	struct thread *thread;

	thread = thread_current();
	unix_rights_release(rights);
	if ((flags & MSG_NOSIGNAL) == 0 && thread != NULL &&
	    thread->proc != NULL)
		(void)signal_send_thread(thread, SIGPIPE);
	return -(ssize_t)EPIPE;
}

/* Frees a stream chunk and the rights it carried. */
static void
unix_stream_chunk_free(
	struct unix_stream_chunk *chunk)
{
	if (chunk == NULL)
		return;
	unix_rights_release(chunk->rights);
	kern_free(chunk);
}

/* Waits until the peer's stream queue has room, reporting how much. */
static int
unix_stream_wait_space(
	struct socket *peer,
	struct unix_socket *endpoint,
	size_t send_hiwat,
	int flags,
	uint64_t deadline,
	size_t *available)
{
	unsigned long irq;
	int error;
	size_t high;
	size_t space;
	uint64_t sequence;

	error = 0;
	irq = spin_lock_irqsave(&peer->lock);

	/* The limit is the smaller of the send and receive high-water marks. */
	for (;;) {
		if (send_hiwat < peer->receive_hiwat_bytes)
			high = send_hiwat;
		else
			high = peer->receive_hiwat_bytes;
		if (endpoint->stream_bytes < high)
			space = high - endpoint->stream_bytes;
		else
			space = 0;
		if (peer->lifecycle != SOCKET_OPEN || peer->read_shutdown) {
			error = EPIPE;
			break;
		}
		if (space != 0) {
			*available = space;
			break;
		}

		/* Sleeps for space unless the send must not block. */
		if ((flags & MSG_DONTWAIT) != 0 || thread_current() == NULL) {
			error = EAGAIN;
			break;
		}
		if (deadline != 0 && sched_ticks() >= deadline) {
			error = EAGAIN;
			break;
		}
		if (signal_pending_unblocked(thread_current())) {
			error = EINTR;
			break;
		}
		sequence =
		    waitq_sequence(&peer->receive_space_waitq);
		error = waitq_sleep(&peer->receive_space_waitq,
				    &peer->lock, sequence, deadline,
				    WAITQ_INTERRUPTIBLE);
		if (error == ETIMEDOUT)
			error = EAGAIN;
		if (error != 0)
			break;
	}
	spin_unlock_irqrestore(&peer->lock, irq);
	return error;
}

/* Sends stream data to the peer, chunk by chunk, coalescing plain writes. */
static ssize_t
unix_stream_send(
	struct socket *socket,
	const void *buffer,
	size_t length,
	int flags,
	struct unix_rights *rights)
{
	struct unix_socket *endpoint;
	struct unix_socket *peer_endpoint;
	struct socket *peer;
	const uint8_t *bytes;
	uint64_t deadline;
	size_t offset;
	size_t send_hiwat;
	size_t amount;
	size_t available;
	unsigned long irq;
	int error;
	struct unix_stream_chunk *chunk;
	struct kern_test_fault_result fault;
	size_t high;
	size_t space;
	struct unix_stream_chunk *tail;
	size_t room;
	ssize_t result;

	endpoint = unix_endpoint(socket);
	peer = NULL;
	bytes = buffer;
	deadline = 0;
	offset = 0;

	/* An empty write cannot carry rights. */
	if (length == 0) {
		if (rights != NULL) {
			result = unix_send_failure(rights, EINVAL);
			return result;
		}
		return 0;
	}

	/* A shut-down socket cannot send; the send timeout sets the deadline. */
	irq = spin_lock_irqsave(&socket->lock);
	if (socket->write_shutdown || socket->lifecycle != SOCKET_OPEN) {
		spin_unlock_irqrestore(&socket->lock, irq);
		result = unix_send_epipe(rights, flags);
		return result;
	}
	send_hiwat = socket->send_hiwat_bytes;
	if (socket->send_timeout_ticks != 0 &&
	    syscall_restart_deadline_after(socket->send_timeout_ticks,
					   &deadline) != 0) {
		spin_unlock_irqrestore(&socket->lock, irq);
		result = unix_send_failure(rights, EOVERFLOW);
		return result;
	}
	spin_unlock_irqrestore(&socket->lock, irq);

	/* Serializes senders on this end. */
	if ((flags & MSG_DONTWAIT) != 0) {
		if (!mutex_trylock(&endpoint->stream_send_lock)) {
			result = unix_send_failure(rights, EAGAIN);
			return result;
		}
		error = 0;
	} else {
		error = mutex_lock_interruptible(&endpoint->stream_send_lock);
	}
	if (error != 0) {
		result = unix_send_failure(rights, error);
		return result;
	}
	error = unix_peer_ref(endpoint, &peer);
	if (error != 0) {
		mutex_unlock(&endpoint->stream_send_lock);
		if (error == EPIPE)
			result = unix_send_epipe(rights, flags);
		else
			result = unix_send_failure(rights, error);
		return result;
	}
	peer_endpoint = unix_endpoint(peer);

	/* Queues one chunk per pass while data and space remain. */
	while (offset < length) {
		amount = length - offset;
		available = 0;
		error = unix_stream_wait_space(peer, peer_endpoint, send_hiwat,
					       flags, deadline, &available);
		if (error != 0)
			break;
		if (amount > UNIX_STREAM_CHUNK_SIZE)
			amount = UNIX_STREAM_CHUNK_SIZE;
		if (amount > available)
			amount = available;
		if (KERN_TEST_FAULT(KERN_TEST_FAULT_UNIX_STREAM_ALLOC,
				    UINT32_MAX, UINT32_MAX, &fault)) {
			if (fault.error != 0)
				error = fault.error;
			else
				error = ENOBUFS;
			break;
		}
		chunk = kern_calloc(1, sizeof(*chunk));
		if (chunk == NULL) {
			error = ENOBUFS;
			break;
		}
		memcpy(chunk->data, bytes + offset, amount);
		chunk->end = amount;

		/* Re-checks the space under the peer lock before queuing. */
		irq = spin_lock_irqsave(&peer->lock);
		if (send_hiwat < peer->receive_hiwat_bytes)
			high = send_hiwat;
		else
			high = peer->receive_hiwat_bytes;
		if (peer_endpoint->stream_bytes < high)
			space = high - peer_endpoint->stream_bytes;
		else
			space = 0;
		if (peer->lifecycle != SOCKET_OPEN ||
		    peer->read_shutdown) {
			error = EPIPE;
		} else if (space == 0) {
			/* SO_RCVBUF may have changed after the first check. */
			error = EAGAIN;
		} else {
			tail = peer_endpoint->stream_tail;
			if (amount > space)
				amount = space;

			/*
			 * Plain stream writes have no record boundary.
			 * Coalesce them into the last chunk so that many small
			 * writes consume memory in proportion to queued bytes
			 * rather than calls.  Ancillary rights retain an
			 * explicit byte boundary.
			 */
			if (rights == NULL &&
			    tail != NULL &&
			    tail->rights == NULL &&
			    tail->end < UNIX_STREAM_CHUNK_SIZE) {
				room = UNIX_STREAM_CHUNK_SIZE - tail->end;
				if (amount > room)
					amount = room;
				memcpy(tail->data + tail->end,
				       chunk->data, amount);
				tail->end += amount;
				peer_endpoint->stream_bytes += amount;
				waitq_wake_one(&peer->receive_waitq);
				error = 0;
			} else {
				chunk->end = amount;
				chunk->rights = rights;
				rights = NULL;
				if (peer_endpoint->stream_tail != NULL)
					peer_endpoint->stream_tail->next = chunk;
				else
					peer_endpoint->stream_head = chunk;
				peer_endpoint->stream_tail = chunk;
				peer_endpoint->stream_bytes += amount;
				waitq_wake_one(&peer->receive_waitq);
				chunk = NULL;
				error = 0;
			}
		}
		spin_unlock_irqrestore(&peer->lock, irq);
		unix_stream_chunk_free(chunk);
		if (error == EAGAIN && (flags & MSG_DONTWAIT) == 0)
			continue;
		if (error != 0)
			break;
		offset += amount;
		poll_notify();
	}
	socket_release(peer);
	mutex_unlock(&endpoint->stream_send_lock);

	/* A partial write is a success; nothing sent reports the error. */
	if (offset != 0) {
		unix_rights_release(rights);
		return (ssize_t)offset;
	}
	if (error == EPIPE) {
		result = unix_send_epipe(rights, flags);
		return result;
	}
	result = unix_send_failure(rights, error);
	return result;
}

/* Sends a datagram to a resolved, connected, or paired peer. */
static ssize_t
unix_datagram_send(
	struct socket *socket,
	const void *buffer,
	size_t length,
	int flags,
	const struct sockaddr *address,
	struct unix_rights *rights,
	struct socket *resolved_peer)
{
	struct unix_socket *endpoint;
	struct socket *peer;
	struct packet_buf *packet;
	void *data;
	uint64_t timeout_ticks;
	unsigned long irq;
	int error;
	ssize_t result;

	endpoint = unix_endpoint(socket);

	/* A shut-down socket cannot send, and a datagram must fit a packet. */
	if (socket->write_shutdown || socket->lifecycle != SOCKET_OPEN) {
		if (resolved_peer != NULL)
			socket_release(resolved_peer);
		result = unix_send_epipe(rights, flags);
		return result;
	}
	if (length > PACKET_BUF_STORAGE_SIZE) {
		if (resolved_peer != NULL)
			socket_release(resolved_peer);
		result = unix_send_failure(rights, EMSGSIZE);
		return result;
	}

	/* Takes the resolved peer, the pair's peer, or the connected peer. */
	if (resolved_peer != NULL) {
		peer = resolved_peer;
		resolved_peer = NULL;
		goto have_peer;
	}
	if (address == NULL && endpoint->connection != NULL) {
		error = unix_peer_ref(endpoint, &peer);
		if (error != 0) {
			result = unix_send_failure(rights, error);
			return result;
		}
		goto have_peer;
	}
	if (address != NULL) {
		result = unix_send_failure(rights, EOPNOTSUPP);
		return result;
	}
	if (!endpoint->connected || endpoint->datagram_peer == NULL) {
		result = unix_send_failure(rights, EDESTADDRREQ);
		return result;
	}
	if (socket_tryref(endpoint->datagram_peer))
		peer = endpoint->datagram_peer;
	else
		peer = NULL;
	if (peer == NULL) {
		result = unix_send_failure(rights, ECONNREFUSED);
		return result;
	}
have_peer:
	/* Builds the packet with the sender's address and the rights. */
	irq = spin_lock_irqsave(&socket->lock);
	timeout_ticks = socket->send_timeout_ticks;
	spin_unlock_irqrestore(&socket->lock, irq);
	packet = packet_buf_alloc(0);
	if (packet == NULL) {
		socket_release(peer);
		result = unix_send_failure(rights, ENOBUFS);
		return result;
	}
	data = packet_buf_append(packet, length);
	if (data == NULL) {
		packet_buf_free(packet);
		socket_release(peer);
		result = unix_send_failure(rights, EMSGSIZE);
		return result;
	}
	if (length != 0)
		memcpy(data, buffer, length);
	unix_store_packet_source(unix_endpoint(socket), packet);
	packet->control = rights;
	if (rights != NULL)
		packet->control_release = unix_rights_release;
	else
		packet->control_release = NULL;

	/* Queues it at the peer, waiting for room within the timeout. */
	error = socket_enqueue_packet_wait(peer, packet, flags & MSG_DONTWAIT,
					   timeout_ticks);
	socket_release(peer);
	if (error != 0)
		return -(ssize_t)error;
	return (ssize_t)length;
}

/* Dispatches a send to the stream or datagram path. */
static ssize_t
unix_send_internal(
	struct socket *socket,
	const void *buffer,
	size_t length,
	int flags,
	const struct sockaddr *address,
	socklen_t address_length,
	struct unix_rights *rights,
	struct socket *resolved_peer)
{
	ssize_t result;

	(void)address_length;

	/* Rejects a missing socket or buffer, or unsupported flags. */
	if (socket == NULL ||
	    (buffer == NULL && length != 0) ||
	    (flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) != 0) {
		if (resolved_peer != NULL)
			socket_release(resolved_peer);
		result = unix_send_failure(rights, EOPNOTSUPP);
		return result;
	}

	/* A stream socket takes no destination address. */
	if (socket->type == SOCK_STREAM) {
		if (resolved_peer != NULL)
			socket_release(resolved_peer);
		if (address != NULL) {
			result = unix_send_failure(rights, EISCONN);
			return result;
		}
		result = unix_stream_send(socket, buffer, length, flags, rights);
		return result;
	}
	result = unix_datagram_send(socket, buffer, length, flags, address,
				  rights, resolved_peer);
	return result;
}

/* Sends data without passed descriptors. */
static ssize_t
unix_sendto(
	struct socket *socket,
	const void *buffer,
	size_t length,
	int flags,
	const struct sockaddr *address,
	socklen_t address_length)
{
	ssize_t result;

	result = unix_send_internal(socket, buffer, length, flags, address,
				  address_length, NULL, NULL);
	return result;
}

/* Receives data without passed descriptors. */
static ssize_t
unix_recvfrom(
	struct socket *socket,
	void *buffer,
	size_t length,
	int flags,
	struct sockaddr *address,
	socklen_t *address_length)
{
	struct unix_recv_transaction transaction;
	ssize_t result;

	/* Runs the transaction, committing unless the caller only peeked. */
	result =
	    unix_socket_receive_begin(socket, buffer, length, flags, address,
				      address_length, 0, &transaction);
	if (result < 0 || !transaction.active)
		return result;
	if ((flags & MSG_PEEK) != 0)
		unix_socket_receive_abort(&transaction);
	else
		unix_socket_receive_commit(&transaction);
	return result;
}

/* Shuts down one or both directions, telling the peer about the write side. */
static int
unix_shutdown(
	struct socket *socket,
	int how)
{
	struct socket *peer;
	unsigned long irq;

	peer = NULL;

	/* Rejects an unknown direction. */
	if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR)
		return EINVAL;

	/* Sets the flags and wakes everyone waiting on this end. */
	irq = spin_lock_irqsave(&socket->lock);
	if (how == SHUT_RD || how == SHUT_RDWR)
		socket->read_shutdown = 1;
	if (how == SHUT_WR || how == SHUT_RDWR)
		socket->write_shutdown = 1;
	waitq_wake_all(&socket->receive_waitq);
	waitq_wake_all(&socket->receive_space_waitq);
	spin_unlock_irqrestore(&socket->lock, irq);

	/* A write shutdown is an end of file for the peer. */
	if (how == SHUT_WR || how == SHUT_RDWR) {
		if (unix_peer_ref(unix_endpoint(socket), &peer) == 0) {
			irq = spin_lock_irqsave(&peer->lock);
			peer->read_shutdown = 1;
			waitq_wake_all(&peer->receive_waitq);
			waitq_wake_all(&peer->receive_space_waitq);
			spin_unlock_irqrestore(&peer->lock, irq);
			socket_release(peer);
		}
	}
	poll_notify();
	return 0;
}

/* Rejects the generic bind; binding needs the caller's directory context. */
static int
unix_bind(
	struct socket *socket,
	const struct sockaddr *address,
	socklen_t length)
{
	(void)socket;
	(void)address;
	(void)length;
	return EOPNOTSUPP;
}

/* Clears the in-progress flag of a failed connect. */
static void
unix_connect_cancel(
	struct socket *socket)
{
	struct unix_socket *endpoint;
	unsigned long irq;

	endpoint = unix_endpoint(socket);
	irq = spin_lock_irqsave(&socket->lock);
	endpoint->connecting = 0;
	spin_unlock_irqrestore(&socket->lock, irq);
}

/* Completes a connect to a resolved listener, consuming its reference. */
static int
unix_connect_resolved(
	struct socket *socket,
	struct socket *listener_socket,
	const struct zedbsd_peercred *connector_credential,
	const char *path,
	unsigned io_flags)
{
	struct unix_socket *client;
	struct unix_socket *listener;
	struct unix_pending *pending;
	struct unix_connection *connection;
	struct zedbsd_peercred listener_credential;
	struct socket *accepted;
	unsigned long irq;
	int error;
	struct socket *old;

	client = unix_endpoint(socket);
	listener = unix_endpoint(listener_socket);
	accepted = NULL;

	(void)io_flags;

	/* Rejects a missing credential. */
	if (connector_credential == NULL) {
		unix_connect_cancel(socket);
		socket_release(listener_socket);
		return EINVAL;
	}

	/* A datagram socket only records its default destination. */
	if (socket->type == SOCK_DGRAM) {
		irq = spin_lock_irqsave(&socket->lock);
		old = client->datagram_peer;
		client->datagram_peer = listener_socket;
		strcpy(client->peer_path, path);
		client->connected = 1;
		client->connecting = 0;
		spin_unlock_irqrestore(&socket->lock, irq);
		if (old != NULL)
			socket_release(old);
		return 0;
	}

	/* Creates the accepted end and the connection record. */
	error = socket_create(AF_UNIX, SOCK_STREAM, 0, &accepted);
	if (error != 0) {
		unix_connect_cancel(socket);
		socket_release(listener_socket);
		return error;
	}
	pending = kern_calloc(1, sizeof(*pending));
	connection = kern_calloc(1, sizeof(*connection));
	if (pending == NULL || connection == NULL) {
		kern_free(connection);
		kern_free(pending);
		socket_release(accepted);
		socket_release(listener_socket);
		unix_connect_cancel(socket);
		return ENOMEM;
	}
	refcount_init(&connection->refs, 2);
	spin_init(&connection->lock, LOCK_RANK_UNIX_CONNECTION,
		  "unix connection");

	/* Queues the accepted end at a listener with backlog room. */
	error = 0;
	irq = spin_lock_irqsave(&listener->socket.lock);
	if (!listener->listening) {
		error = ECONNREFUSED;
	} else if (!listener->listener_credential_valid) {
		error = ECONNREFUSED;
	} else if (listener->pending_count >= listener->backlog) {
		error = EAGAIN;
	} else {
		listener_credential = listener->listener_credential;
		connection->ends[0] = socket;
		connection->ends[1] = accepted;
		unix_endpoint(accepted)->connection = connection;
		unix_endpoint(accepted)->side = 1;
		unix_endpoint(accepted)->peer_credential =
		    *connector_credential;
		unix_endpoint(accepted)->peer_credential_valid = 1;
		pending->socket = accepted;
		if (listener->pending_tail != NULL)
			listener->pending_tail->next = pending;
		else
			listener->pending_head = pending;
		listener->pending_tail = pending;
		listener->pending_count++;
		waitq_wake_one(&listener->socket.accept_waitq);
	}
	spin_unlock_irqrestore(&listener->socket.lock, irq);

	/*
	 * The accepted end is complete before it enters the listener queue.
	 * If it is accepted and closed before this publication, its
	 * connection reference is released while the reserved client
	 * reference remains.
	 */
	if (error == 0) {
		irq = spin_lock_irqsave(&socket->lock);
		client->connection = connection;
		client->side = 0;
		client->peer_credential = listener_credential;
		client->peer_credential_valid = 1;
		client->connecting = 0;
		spin_unlock_irqrestore(&socket->lock, irq);
		poll_notify();
	} else {
		unix_connect_cancel(socket);
	}
	socket_release(listener_socket);
	if (error != 0) {
		kern_free(pending);
		kern_free(connection);
		socket_release(accepted);
	}
	return error;
}

/* Rejects the generic connect; connecting needs the caller's directory context. */
static int
unix_connect(
	struct socket *socket,
	const struct sockaddr *address,
	socklen_t length,
	unsigned io_flags)
{
	(void)socket;
	(void)address;
	(void)length;
	(void)io_flags;
	return EOPNOTSUPP;
}

/* Takes the next queued connection off a listener. */
static int
unix_accept(
	struct socket *socket,
	struct socket **result,
	struct sockaddr *address,
	socklen_t *length,
	unsigned io_flags)
{
	struct unix_socket *listener;
	struct unix_pending *pending;
	struct thread *thread;
	unsigned long irq;
	int error;
	uint64_t sequence;
	struct socket *peer;

	listener = unix_endpoint(socket);
	thread = thread_current();

	/* Rejects a missing result or a socket that cannot listen. */
	if (result == NULL)
		return EINVAL;
	if (socket->type != SOCK_STREAM)
		return EOPNOTSUPP;
	irq = spin_lock_irqsave(&socket->lock);
	if (!listener->listening) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return EINVAL;
	}

	/* Waits for a connection unless the accept must not block. */
	while (listener->pending_head == NULL) {
		if ((io_flags & SOCKET_IO_NONBLOCK) != 0 || thread == NULL) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return EAGAIN;
		}
		if (signal_pending_unblocked(thread)) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return EINTR;
		}
		sequence = waitq_sequence(&socket->accept_waitq);
		error = waitq_sleep(&socket->accept_waitq, &socket->lock,
				    sequence, 0, WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return EINTR;
		}
	}

	/* Dequeues the connection. */
	pending = listener->pending_head;
	listener->pending_head = pending->next;
	if (listener->pending_head == NULL)
		listener->pending_tail = NULL;
	listener->pending_count--;
	*result = pending->socket;
	spin_unlock_irqrestore(&socket->lock, irq);

	/* Reports the peer's address when asked. */
	if (address != NULL && length != NULL) {
		peer = NULL;
		if (unix_peer_ref(unix_endpoint(*result), &peer) == 0) {
			unix_store_address(unix_endpoint(peer), address,
					   length);
			socket_release(peer);
		} else {
			unix_store_address(NULL, address, length);
		}
	}
	kern_free(pending);
	return 0;
}

/* Reports the bound pathname. */
static int
unix_getsockname(
	struct socket *socket,
	struct sockaddr *address,
	socklen_t *length)
{
	if (address == NULL || length == NULL)
		return EINVAL;
	unix_store_address(unix_endpoint(socket), address, length);
	return 0;
}

/* Reports the peer's pathname. */
static int
unix_getpeername(
	struct socket *socket,
	struct sockaddr *address,
	socklen_t *length)
{
	struct socket *peer;
	int error;
	struct unix_socket temporary;
	struct unix_socket *endpoint;

	/* Rejects a missing result. */
	if (address == NULL || length == NULL)
		return EINVAL;

	/* A connected datagram socket reports the path it connected to. */
	if (socket->type == SOCK_DGRAM) {
		endpoint = unix_endpoint(socket);
		if (endpoint->connection != NULL)
			goto connected_pair;
		if (!endpoint->connected)
			return ENOTCONN;
		memset(&temporary, 0, sizeof(temporary));
		temporary.bound = 1;
		strcpy(temporary.path, endpoint->peer_path);
		unix_store_address(&temporary, address, length);
		return 0;
	}
connected_pair:
	/* A connection reports the peer end's bound path. */
	error = unix_peer_ref(unix_endpoint(socket), &peer);
	if (error != 0)
		return error;
	unix_store_address(unix_endpoint(peer), address, length);
	socket_release(peer);
	return 0;
}

/* Reports the peer credential of a stream connection. */
static int
unix_getsockopt(
	struct socket *socket,
	int level,
	int option,
	void *value,
	socklen_t *length)
{
	struct unix_socket *endpoint;
	struct zedbsd_peercred credential;
	unsigned long irq;

	/* Only SO_PEERCRED of a stream socket is handled here. */
	if (socket == NULL || level != SOL_SOCKET || option != SO_PEERCRED)
		return ENOPROTOOPT;
	if (socket->type != SOCK_STREAM)
		return ENOPROTOOPT;
	if (value == NULL || length == NULL || *length < sizeof(credential))
		return EINVAL;

	/* Copies the credential recorded at connection time. */
	endpoint = unix_endpoint(socket);
	irq = spin_lock_irqsave(&socket->lock);
	if (!endpoint->peer_credential_valid) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return ENOTCONN;
	}
	credential = endpoint->peer_credential;
	spin_unlock_irqrestore(&socket->lock, irq);
	memcpy(value, &credential, sizeof(credential));
	*length = sizeof(credential);
	return 0;
}

/* Reports the readiness of a socket. */
static int
unix_poll(
	struct socket *socket,
	short events,
	short *revents)
{
	struct unix_socket *endpoint;
	struct socket *peer;
	short result;
	size_t send_hiwat;
	int local_writable;
	int error;
	unsigned long irq;
	struct unix_socket *peer_endpoint;
	size_t high;

	endpoint = unix_endpoint(socket);
	peer = NULL;
	result = 0;

	/* A listener is readable when a connection awaits accept. */
	if (endpoint->listening) {
		error = socket_poll_common(socket, events, &result);
		if (error != 0)
			return error;
		irq = spin_lock_irqsave(&socket->lock);
		if (endpoint->pending_head != NULL)
			result |= events & (POLLIN | POLLRDNORM);
		result &= (short)~(POLLOUT | POLLWRNORM);
		spin_unlock_irqrestore(&socket->lock, irq);
		*revents = result;
		return 0;
	}

	/* A datagram socket is writable only while its peer has queue room. */
	if (socket->type != SOCK_STREAM) {
		error = socket_poll_common(socket, events, &result);
		if (error != 0)
			return error;
		if ((result & (POLLOUT | POLLWRNORM)) != 0 &&
		    (endpoint->connection != NULL || endpoint->connected)) {
			if (endpoint->connection != NULL) {
				error = unix_peer_ref(endpoint, &peer);
			} else {
				if (endpoint->datagram_peer != NULL &&
				    socket_tryref(endpoint->datagram_peer))
					peer = endpoint->datagram_peer;
				else
					peer = NULL;
				if (peer != NULL)
					error = 0;
				else
					error = ECONNREFUSED;
			}
			if (error == 0) {
				irq = spin_lock_irqsave(&peer->lock);
				if (peer->lifecycle != SOCKET_OPEN ||
				    peer->read_shutdown ||
				    (peer->receive_packet_limit != 0 &&
				     peer->receive_packets >=
					 peer->receive_packet_limit) ||
				    peer->receive_bytes >=
					peer->receive_hiwat_bytes)
					result &=
					    (short)~(POLLOUT | POLLWRNORM);
				spin_unlock_irqrestore(&peer->lock, irq);
				socket_release(peer);
			} else {
				result &= (short)~(POLLOUT | POLLWRNORM);
				result |= POLLERR | POLLHUP;
			}
		}
		*revents = result;
		return 0;
	}

	/* A stream socket is readable with data, end of file, or an error. */
	irq = spin_lock_irqsave(&socket->lock);
	if (endpoint->stream_head != NULL ||
	    socket->read_shutdown ||
	    socket->lifecycle != SOCKET_OPEN)
		result |= events & (POLLIN | POLLRDNORM);
	if (socket->error != 0)
		result |= POLLERR;
	if (socket->read_shutdown || socket->lifecycle != SOCKET_OPEN)
		result |= POLLHUP;
	if (socket->write_shutdown || socket->lifecycle != SOCKET_OPEN)
		result |= POLLERR;
	send_hiwat = socket->send_hiwat_bytes;
	local_writable =
	    !socket->write_shutdown && socket->lifecycle == SOCKET_OPEN;
	spin_unlock_irqrestore(&socket->lock, irq);

	/* It is writable while the peer's queue is below the limit. */
	if (local_writable && unix_peer_ref(endpoint, &peer) == 0) {
		peer_endpoint = unix_endpoint(peer);
		irq = spin_lock_irqsave(&peer->lock);
		if (peer->lifecycle == SOCKET_OPEN && !peer->read_shutdown) {
			if (send_hiwat < peer->receive_hiwat_bytes)
				high = send_hiwat;
			else
				high = peer->receive_hiwat_bytes;
			if (peer_endpoint->stream_bytes < high)
				result |= events & (POLLOUT | POLLWRNORM);
		} else {
			result |= POLLERR | POLLHUP;
		}
		spin_unlock_irqrestore(&peer->lock, irq);
		socket_release(peer);
	} else if (local_writable) {
		result |= POLLERR | POLLHUP;
	}
	*revents = result;
	return 0;
}

/* Wakes the peer's senders after the send buffer size changed. */
static void
unix_buffer_changed(
	struct socket *socket,
	int option)
{
	struct socket *peer;
	unsigned long irq;

	/* Only a stream socket's send buffer bounds the peer's queue. */
	if (option != SO_SNDBUF || socket->type != SOCK_STREAM)
		return;
	if (unix_peer_ref(unix_endpoint(socket), &peer) != 0)
		return;

	/* Wakes the senders blocked on space. */
	irq = spin_lock_irqsave(&peer->lock);
	waitq_wake_all(&peer->receive_space_waitq);
	spin_unlock_irqrestore(&peer->lock, irq);
	socket_release(peer);
}

/* Detaches an endpoint from its path, peers, and pending connections. */
static void
unix_endpoint_close(
	struct socket *socket)
{
	struct unix_socket *endpoint;
	struct unix_connection *connection;
	struct unix_pending *pending;
	struct unix_pending *pending_list;
	struct socket *datagram_peer;
	struct path bound_path;
	struct inode *inode;
	int had_bound;
	struct socket *peer;
	unsigned long irq;

	endpoint = unix_endpoint(socket);
	peer = NULL;

	/* Takes everything out of the endpoint under its lock, once. */
	path_init(&bound_path);
	irq = spin_lock_irqsave(&socket->lock);
	if (endpoint->endpoint_closed) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return;
	}
	endpoint->endpoint_closed = 1;
	connection = endpoint->connection;
	endpoint->connection = NULL;
	had_bound = endpoint->bound;
	endpoint->bound = 0;
	if (had_bound) {
		bound_path = endpoint->bound_path;
		path_init(&endpoint->bound_path);
	}
	datagram_peer = endpoint->datagram_peer;
	endpoint->datagram_peer = NULL;
	pending_list = endpoint->pending_head;
	endpoint->pending_head = NULL;
	endpoint->pending_tail = NULL;
	endpoint->pending_count = 0;
	endpoint->listening = 0;
	waitq_wake_all(&socket->receive_space_waitq);
	spin_unlock_irqrestore(&socket->lock, irq);

	/* Unhooks the socket from its inode and drops the path. */
	if (had_bound) {
		inode = bound_path.p_inode;
		if (inode != NULL) {
			mutex_lock(&inode->i_lock);
			if (inode->i_special == socket)
				inode->i_special = NULL;
			mutex_unlock(&inode->i_lock);
		}
		path_release(&bound_path);
	}

	/* Drops the datagram peer and the unaccepted connections. */
	if (datagram_peer != NULL)
		socket_release(datagram_peer);
	pending = pending_list;
	while (pending != NULL) {
		pending_list = pending->next;
		socket_release(pending->socket);
		kern_free(pending);
		pending = pending_list;
	}

	/* Leaves the connection, giving the peer an end of file. */
	if (connection != NULL) {
		irq = spin_lock_irqsave(&connection->lock);
		if (connection->ends[endpoint->side] == socket)
			connection->ends[endpoint->side] = NULL;
		peer = connection->ends[endpoint->side ^ 1U];
		if (peer != NULL && !socket_tryref(peer))
			peer = NULL;
		spin_unlock_irqrestore(&connection->lock, irq);
		if (peer != NULL) {
			irq = spin_lock_irqsave(&peer->lock);
			peer->read_shutdown = 1;
			waitq_wake_all(&peer->receive_waitq);
			waitq_wake_all(&peer->receive_space_waitq);
			spin_unlock_irqrestore(&peer->lock, irq);
			poll_notify();
			socket_release(peer);
		}
		unix_connection_release(connection);
	}
}

/* Frees a closed endpoint and its queued stream data. */
static void
unix_close(
	struct socket *socket)
{
	struct unix_socket *endpoint;
	struct unix_stream_chunk *chunk;
	struct unix_stream_chunk *chunks;
	unsigned long irq;

	endpoint = unix_endpoint(socket);

	/* Detaches the endpoint and takes its queue. */
	unix_endpoint_close(socket);
	irq = spin_lock_irqsave(&socket->lock);
	chunks = endpoint->stream_head;
	endpoint->stream_head = NULL;
	endpoint->stream_tail = NULL;
	endpoint->stream_bytes = 0;
	endpoint->reserved_stream = NULL;
	spin_unlock_irqrestore(&socket->lock, irq);

	/* Frees the queue and the endpoint. */
	chunk = chunks;
	while (chunk != NULL) {
		chunks = chunk->next;
		chunk->next = NULL;
		unix_stream_chunk_free(chunk);
		chunk = chunks;
	}
	kern_free(endpoint);
}

/* Creates a stream or datagram endpoint. */
static int
unix_create(
	int type,
	int protocol,
	struct socket **result)
{
	struct unix_socket *endpoint;

	/* Only stream and datagram sockets with the default protocol exist. */
	if ((type != SOCK_STREAM && type != SOCK_DGRAM) || protocol != 0)
		return EPROTONOSUPPORT;

	/* Allocates and initializes the endpoint. */
	endpoint = kern_calloc(1, sizeof(*endpoint));
	if (endpoint == NULL)
		return ENOMEM;
	socket_init_object(&endpoint->socket, AF_UNIX, type, protocol,
			   &unix_ops);
	(void)mutex_init(&endpoint->stream_send_lock,
			 LOCK_RANK_UNIX_STREAM_SEND, "unix stream send");
	path_init(&endpoint->bound_path);
	*result = &endpoint->socket;
	return 0;
}
