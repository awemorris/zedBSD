/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/socket.h"
#include "kern/net/packet-buf.h"
#include "kern/file.h"
#include "kern/clock.h"
#include "kern/cred.h"
#include "kern/inode.h"
#include "kern/kmem.h"
#include "kern/namei.h"
#include "kern/poll.h"
#include "kern/sched.h"
#include "kern/signal.h"
#include "kern/thread.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>
#include <zedbsd/poll.h>

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

static void
unix_rights_release(void *pointer)
{
	struct unix_rights *rights = pointer;
	unsigned index;
	if (rights == NULL)
		return;
	for (index = 0; index < rights->count; index++)
		if (rights->files[index] != NULL)
			(void)file_close(rights->files[index]);
	kern_free(rights);
}

static ssize_t
unix_send_failure(struct unix_rights *rights, int error)
{
	unix_rights_release(rights);
	return -(ssize_t)error;
}

struct unix_socket {
	struct socket socket;
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
};

static struct unix_socket *
unix_endpoint(struct socket *socket)
{
	return (struct unix_socket *)socket;
}

static int
unix_copy_path(const struct sockaddr *address, socklen_t length,
	char path[UNIX_PATH_MAX])
{
	const struct sockaddr_un *local = (const struct sockaddr_un *)address;
	size_t available, used;
	if (address == NULL || length <= offsetof(struct sockaddr_un, sun_path) ||
	    length > sizeof(*local) || local->sun_family != AF_UNIX)
		return EINVAL;
	available = length - offsetof(struct sockaddr_un, sun_path);
	for (used = 0; used < available && local->sun_path[used] != '\0'; used++)
		;
	if (used == 0 || used == available || used >= UNIX_PATH_MAX)
		return EINVAL;
	memcpy(path, local->sun_path, used);
	path[used] = '\0';
	return 0;
}

static void
unix_store_address(const struct unix_socket *endpoint, struct sockaddr *address,
	socklen_t *length)
{
	struct sockaddr_un local;
	socklen_t needed, capacity, copied;
	memset(&local, 0, sizeof(local));
	local.sun_family = AF_UNIX;
	if (endpoint != NULL && endpoint->bound)
		strncpy(local.sun_path, endpoint->path, sizeof(local.sun_path) - 1U);
	needed = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
	    strlen(local.sun_path) + 1U);
	capacity = *length;
	copied = capacity < needed ? capacity : needed;
	if (copied != 0)
		memcpy(address, &local, copied);
	*length = needed;
}

static void
unix_store_packet_source(const struct unix_socket *endpoint,
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

static int
unix_resolve_endpoint(struct cwdinfo *context, const struct ucred *cred,
	const struct sockaddr *address, socklen_t length, int type,
	struct socket **result, char path_text[UNIX_PATH_MAX])
{
	struct path resolved;
	struct socket *socket = NULL;
	char path[UNIX_PATH_MAX];
	int error = unix_copy_path(address, length, path);
	if (error != 0)
		return error;
	if (context == NULL || cred == NULL)
		return EINVAL;
	path_init(&resolved);
	error = namei_path_at(context, path, &resolved);
	if (error == 0 && resolved.p_inode->i_type != INODE_SOCKET)
		error = ENOTSOCK;
	if (error == 0)
		error = vfs_access(resolved.p_inode, cred, W_OK);
	if (error == 0) {
		mutex_lock(&resolved.p_inode->i_lock);
		socket = resolved.p_inode->i_special;
		if (socket == NULL || socket->type != type ||
		    !socket_tryref(socket))
			socket = NULL;
		mutex_unlock(&resolved.p_inode->i_lock);
		if (socket == NULL)
			error = ECONNREFUSED;
	}
	path_release(&resolved);
	if (error != 0)
		return error;
	if (path_text != NULL)
		strcpy(path_text, path);
	*result = socket;
	return 0;
}

static int
unix_connection_create(struct socket *left, struct socket *right)
{
	struct unix_connection *connection = kern_calloc(1, sizeof(*connection));
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
	return 0;
}

static void
unix_connection_release(struct unix_connection *connection)
{
	if (connection != NULL && refcount_put(&connection->refs))
		kern_free(connection);
}

static int
unix_peer_ref(struct unix_socket *endpoint, struct socket **result)
{
	struct unix_connection *connection = endpoint->connection;
	struct socket *peer;
	unsigned long irq;

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

static ssize_t
unix_send_internal(struct socket *socket, const void *buffer, size_t length,
	int flags, const struct sockaddr *address, socklen_t address_length,
	struct unix_rights *rights, struct socket *resolved_peer)
{
	struct socket *peer;
	struct packet_buf *packet;
	void *data;
	int error;

	struct unix_socket *endpoint = unix_endpoint(socket);
	(void)address_length;

	if ((flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) != 0) {
		if (resolved_peer != NULL)
			socket_release(resolved_peer);
		return unix_send_failure(rights, EOPNOTSUPP);
	}
	if (socket->write_shutdown) {
		if (resolved_peer != NULL)
			socket_release(resolved_peer);
		return unix_send_failure(rights, EPIPE);
	}
	if (length > PACKET_BUF_STORAGE_SIZE) {
		if (resolved_peer != NULL)
			socket_release(resolved_peer);
		return unix_send_failure(rights, EMSGSIZE);
	}
	if (socket->type == SOCK_DGRAM) {
		if (resolved_peer != NULL) {
			peer = resolved_peer;
			resolved_peer = NULL;
			goto have_peer;
		}
		if (address == NULL && endpoint->connection != NULL) {
			error = unix_peer_ref(endpoint, &peer);
			if (error != 0)
				return unix_send_failure(rights, error);
			goto have_peer;
		}
		if (address != NULL) {
			return unix_send_failure(rights, EOPNOTSUPP);
		} else if (endpoint->connected && endpoint->datagram_peer != NULL) {
			peer = socket_tryref(endpoint->datagram_peer) ?
			    endpoint->datagram_peer : NULL;
		} else {
			return unix_send_failure(rights, EDESTADDRREQ);
		}
		if (peer == NULL)
			return unix_send_failure(rights, ECONNREFUSED);
	} else {
		if (address != NULL)
			return unix_send_failure(rights, EISCONN);
		error = unix_peer_ref(endpoint, &peer);
		if (error != 0)
			return unix_send_failure(rights, error);
	}
have_peer:
	packet = packet_buf_alloc(0);
	if (packet == NULL) {
		socket_release(peer);
		return unix_send_failure(rights, ENOBUFS);
	}
	data = packet_buf_append(packet, length);
	if (data == NULL) {
		packet_buf_free(packet);
		socket_release(peer);
		return unix_send_failure(rights, EMSGSIZE);
	}
	if (length != 0)
		memcpy(data, buffer, length);
	if (socket->type == SOCK_DGRAM)
		unix_store_packet_source(unix_endpoint(socket), packet);
	packet->control = rights;
	packet->control_release = rights != NULL ? unix_rights_release : NULL;
	error = socket_enqueue_packet(peer, packet);
	socket_release(peer);
	if (error == ENOBUFS && (flags & MSG_DONTWAIT) != 0)
		error = EAGAIN;
	return error == 0 ? (ssize_t)length : -(ssize_t)error;
}

static ssize_t
unix_sendto(struct socket *socket, const void *buffer, size_t length, int flags,
	const struct sockaddr *address, socklen_t address_length)
{
	return unix_send_internal(socket, buffer, length, flags, address,
	    address_length, NULL, NULL);
}

ssize_t
unix_socket_send_message(struct socket *socket, const void *buffer,
	size_t length, int flags, const struct sockaddr *address,
	socklen_t address_length, struct file **files, unsigned count)
{
	struct unix_rights *rights = NULL;
	unsigned index;

	if (socket == NULL || socket->family != AF_UNIX ||
	    count > ZEDBSD_MSG_FD_MAX) {
		for (index = 0; index < count; index++)
			(void)file_close(files[index]);
		return -(ssize_t)EOPNOTSUPP;
	}
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
	return unix_send_internal(socket, buffer, length, flags, address,
	    address_length, rights, NULL);
}

ssize_t
unix_socket_send_message_at(struct socket *socket, struct cwdinfo *context,
	const struct ucred *cred, const void *buffer, size_t length, int flags,
	const struct sockaddr *address, socklen_t address_length,
	struct file **files, unsigned count)
{
	struct unix_rights *rights = NULL;
	struct socket *peer = NULL;
	unsigned index;
	int error;
	if (socket == NULL || socket->family != AF_UNIX ||
	    count > ZEDBSD_MSG_FD_MAX) {
		for (index = 0; index < count; index++)
			(void)file_close(files[index]);
		return -(ssize_t)EOPNOTSUPP;
	}
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
	if (address != NULL && socket->type == SOCK_DGRAM) {
		error = unix_resolve_endpoint(context, cred, address,
		    address_length, SOCK_DGRAM, &peer, NULL);
		if (error != 0)
			return unix_send_failure(rights, error);
	}
	return unix_send_internal(socket, buffer, length, flags, address,
	    address_length, rights, peer);
}

ssize_t
unix_socket_receive_begin(struct socket *socket, void *buffer, size_t length,
	int flags, struct sockaddr *address, socklen_t *address_length,
	unsigned file_capacity, struct unix_recv_transaction *transaction)
{
	struct unix_socket *endpoint;
	struct packet_buf *packet;
	struct unix_rights *rights;
	uint64_t deadline = 0;
	unsigned index, delivered;
	unsigned long irq;
	int datagram, error;
	if (socket == NULL || socket->family != AF_UNIX || transaction == NULL ||
	    ((address == NULL) != (address_length == NULL)) ||
	    file_capacity > ZEDBSD_MSG_FD_MAX)
		return -EINVAL;
	memset(transaction, 0, sizeof(*transaction));
	datagram = socket->type == SOCK_DGRAM;
	if ((!datagram && (address != NULL ||
	    (flags & ~(MSG_DONTWAIT | MSG_PEEK)) != 0)) ||
	    (datagram &&
	    (flags & ~(MSG_DONTWAIT | MSG_PEEK | MSG_TRUNC)) != 0))
		return -EOPNOTSUPP;
	endpoint = unix_endpoint(socket);
	irq = spin_lock_irqsave(&socket->lock);
	if (socket->receive_timeout_ticks != 0 &&
	    kern_deadline_after(sched_ticks(), socket->receive_timeout_ticks,
	    &deadline) != 0) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return -EOVERFLOW;
	}
	for (;;) {
		packet = socket->receive_head;
		if (packet != NULL && endpoint->reserved_packet == NULL)
			break;
		if (!datagram && packet == NULL && socket->read_shutdown) {
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
		{
			uint64_t sequence = waitq_sequence(&socket->receive_waitq);
			error = waitq_sleep(&socket->receive_waitq, &socket->lock,
			    sequence, deadline, WAITQ_INTERRUPTIBLE);
			if (error == EINTR || error == ETIMEDOUT) {
				spin_unlock_irqrestore(&socket->lock, irq);
				return error == EINTR ? -EINTR : -EAGAIN;
			}
		}
	}
	rights = packet->control;
	if (rights != NULL && (flags & MSG_PEEK) != 0) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return -EOPNOTSUPP;
	}
	endpoint->reserved_packet = packet;
	endpoint->reservation_token++;
	if (endpoint->reservation_token == 0)
		endpoint->reservation_token++;
	transaction->socket = socket;
	transaction->packet = packet;
	transaction->token = endpoint->reservation_token;
	transaction->datagram = (unsigned)datagram;
	transaction->active = 1;
	socket_ref(socket);
	spin_unlock_irqrestore(&socket->lock, irq);

	transaction->copied = length < packet->length ? length : packet->length;
	if (transaction->copied != 0)
		memcpy(buffer, packet->data, transaction->copied);
	if (datagram && address != NULL && packet->source_length != 0) {
		socklen_t actual = packet->source_length;
		socklen_t copied = *address_length < actual ?
		    *address_length : actual;
		if (copied != 0)
			memcpy(address, packet->source_address, copied);
		*address_length = actual;
	}
	delivered = rights != NULL && rights->count < file_capacity ?
	    rights->count : file_capacity;
	if (rights == NULL)
		delivered = 0;
	for (index = 0; index < delivered; index++) {
		transaction->files[index] = rights->files[index];
		file_ref(transaction->files[index]);
	}
	transaction->file_count = delivered;
	transaction->control_truncated =
	    rights != NULL && delivered < rights->count;
	return (ssize_t)transaction->copied;
}

void
unix_socket_receive_abort(struct unix_recv_transaction *transaction)
{
	struct socket *socket;
	struct unix_socket *endpoint;
	unsigned index;
	unsigned long irq;
	if (transaction == NULL || !transaction->active)
		return;
	socket = transaction->socket;
	endpoint = unix_endpoint(socket);
	irq = spin_lock_irqsave(&socket->lock);
	if (endpoint->reserved_packet == transaction->packet &&
	    endpoint->reservation_token == transaction->token)
		endpoint->reserved_packet = NULL;
	waitq_wake_all(&socket->receive_waitq);
	spin_unlock_irqrestore(&socket->lock, irq);
	for (index = 0; index < transaction->file_count; index++)
		if (transaction->files[index] != NULL)
			(void)file_close(transaction->files[index]);
	transaction->active = 0;
	socket_release(socket);
	poll_notify();
}

void
unix_socket_receive_commit(struct unix_recv_transaction *transaction)
{
	struct socket *socket;
	struct unix_socket *endpoint;
	struct packet_buf *packet, *free_packet = NULL;
	void *control = NULL;
	void (*control_release)(void *) = NULL;
	unsigned index;
	unsigned long irq;
	if (transaction == NULL || !transaction->active)
		return;
	socket = transaction->socket;
	endpoint = unix_endpoint(socket);
	packet = transaction->packet;
	irq = spin_lock_irqsave(&socket->lock);
	if (endpoint->reserved_packet != packet ||
	    endpoint->reservation_token != transaction->token ||
	    socket->receive_head != packet) {
		spin_unlock_irqrestore(&socket->lock, irq);
		unix_socket_receive_abort(transaction);
		return;
	}
	if (transaction->datagram || transaction->copied >= packet->length) {
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
		(void)packet_buf_pull(packet, transaction->copied);
		if (socket->receive_bytes >= transaction->copied)
			socket->receive_bytes -= transaction->copied;
		control = packet->control;
		control_release = packet->control_release;
		packet->control = NULL;
		packet->control_release = NULL;
	}
	endpoint->reserved_packet = NULL;
	waitq_wake_all(&socket->receive_waitq);
	spin_unlock_irqrestore(&socket->lock, irq);
	if (free_packet != NULL)
		packet_buf_free(free_packet);
	else if (control != NULL && control_release != NULL)
		control_release(control);
	for (index = 0; index < transaction->file_count; index++)
		transaction->files[index] = NULL; /* ownership moved to filedesc */
	transaction->active = 0;
	socket_release(socket);
	poll_notify();
}

static ssize_t
unix_recvfrom(struct socket *socket, void *buffer, size_t length, int flags,
	struct sockaddr *address, socklen_t *address_length)
{
	struct unix_recv_transaction transaction;
	ssize_t result;
	if (length == 0)
		return 0;
	result = unix_socket_receive_begin(socket, buffer, length, flags,
	    address, address_length, 0, &transaction);
	if (result < 0 || !transaction.active)
		return result;
	if ((flags & MSG_PEEK) != 0)
		unix_socket_receive_abort(&transaction);
	else
		unix_socket_receive_commit(&transaction);
	return result;
}

ssize_t
unix_socket_receive_message(struct socket *socket, void *buffer,
	size_t length, int flags, struct sockaddr *address,
	socklen_t *address_length, struct file **files, unsigned *file_count,
	unsigned *control_truncated)
{
	struct unix_recv_transaction transaction;
	ssize_t result;
	unsigned index, capacity;
	if (socket == NULL || socket->family != AF_UNIX || file_count == NULL ||
	    control_truncated == NULL ||
	    ((address == NULL) != (address_length == NULL)))
		return -(ssize_t)EINVAL;
	capacity = *file_count;
	result = unix_socket_receive_begin(socket, buffer, length, flags,
	    address, address_length, capacity, &transaction);
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
	return result;
}

static int
unix_shutdown(struct socket *socket, int how)
{
	struct socket *peer = NULL;
	unsigned long irq;

	if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR)
		return EINVAL;
	irq = spin_lock_irqsave(&socket->lock);
	if (how == SHUT_RD || how == SHUT_RDWR)
		socket->read_shutdown = 1;
	if (how == SHUT_WR || how == SHUT_RDWR)
		socket->write_shutdown = 1;
	waitq_wake_all(&socket->receive_waitq);
	spin_unlock_irqrestore(&socket->lock, irq);
	if (how == SHUT_WR || how == SHUT_RDWR) {
		if (unix_peer_ref(unix_endpoint(socket), &peer) == 0) {
			irq = spin_lock_irqsave(&peer->lock);
			peer->read_shutdown = 1;
			waitq_wake_all(&peer->receive_waitq);
			spin_unlock_irqrestore(&peer->lock, irq);
			socket_release(peer);
		}
	}
	poll_notify();
	return 0;
}

static int
unix_bind(struct socket *socket, const struct sockaddr *address,
	socklen_t length)
{
	(void)socket;
	(void)address;
	(void)length;
	return EOPNOTSUPP;
}

int
unix_socket_bind_path(struct socket *socket, struct cwdinfo *context,
	const struct ucred *cred, mode_t umask, const struct sockaddr *address,
	socklen_t length)
{
	struct unix_socket *endpoint;
	struct path parent;
	struct componentname name;
	struct inode *inode = NULL;
	char path[UNIX_PATH_MAX], storage[NAME_MAX + 1U];
	unsigned long irq;
	int error;
	if (socket == NULL || socket->family != AF_UNIX || context == NULL ||
	    cred == NULL)
		return EINVAL;
	error = unix_copy_path(address, length, path);
	if (error != 0)
		return error;
	endpoint = unix_endpoint(socket);
	irq = spin_lock_irqsave(&socket->lock);
	if (endpoint->bound || endpoint->binding_in_progress) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return EINVAL;
	}
	endpoint->binding_in_progress = 1;
	spin_unlock_irqrestore(&socket->lock, irq);
	path_init(&parent);
	error = namei_parent_path_at(context, path, &parent, &name, storage);
	if (error == 0)
		error = vfs_may_create(parent.p_inode, cred);
	if (error == 0)
		error = inode_mknod(parent.p_inode, &name, INODE_SOCKET,
		    S_IFSOCK | (0777U & ~umask), 0, &inode);
	if (error == 0) {
		path_set(&endpoint->bound_path, parent.p_mount, inode);
		strcpy(endpoint->path, path);
		endpoint->bound = 1;
		mutex_lock(&inode->i_lock);
		if (inode->i_special == NULL)
			inode->i_special = socket;
		else
			error = EADDRINUSE;
		mutex_unlock(&inode->i_lock);
		if (error != 0) {
			endpoint->bound = 0;
			endpoint->path[0] = '\0';
			path_release(&endpoint->bound_path);
		}
	}
	if (error != 0 && inode != NULL)
		(void)inode_unlink(parent.p_inode, &name);
	if (inode != NULL)
		inode_release(inode);
	path_release(&parent);
	irq = spin_lock_irqsave(&socket->lock);
	endpoint->binding_in_progress = 0;
	spin_unlock_irqrestore(&socket->lock, irq);
	return error;
}

static int
unix_listen(struct socket *socket, int backlog)
{
	struct unix_socket *endpoint = unix_endpoint(socket);
	if (socket->type != SOCK_STREAM)
		return EOPNOTSUPP;
	if (!endpoint->bound)
		return EDESTADDRREQ;
	if (endpoint->connection != NULL)
		return EISCONN;
	if (backlog < 1)
		backlog = 1;
	if (backlog > 16)
		backlog = 16;
	endpoint->backlog = (unsigned)backlog;
	endpoint->listening = 1;
	return 0;
}

static int
unix_connect_resolved(struct socket *socket, struct socket *listener_socket,
	const char *path, unsigned io_flags)
{
	struct unix_socket *client = unix_endpoint(socket);
	struct unix_socket *listener = unix_endpoint(listener_socket);
	struct unix_pending *pending;
	struct unix_connection *connection;
	struct socket *accepted = NULL;
	unsigned long irq;
	int error;
	(void)io_flags;
	if (client->connection != NULL) {
		socket_release(listener_socket);
		return EISCONN;
	}
	if (socket->type == SOCK_DGRAM) {
		struct socket *old = client->datagram_peer;
		client->datagram_peer = listener_socket;
		strcpy(client->peer_path, path);
		client->connected = 1;
		if (old != NULL)
			socket_release(old);
		return 0;
	}
	error = socket_create(AF_UNIX, SOCK_STREAM, 0, &accepted);
	if (error != 0) {
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
		return ENOMEM;
	}
	refcount_init(&connection->refs, 2);
	spin_init(&connection->lock, LOCK_RANK_UNIX_CONNECTION,
	    "unix connection");
	irq = spin_lock_irqsave(&listener->socket.lock);
	if (!listener->listening) {
		error = ECONNREFUSED;
	} else if (listener->pending_count >= listener->backlog) {
		error = EAGAIN;
	} else {
		connection->ends[0] = socket;
		connection->ends[1] = accepted;
		client->connection = connection;
		client->side = 0;
		unix_endpoint(accepted)->connection = connection;
		unix_endpoint(accepted)->side = 1;
		pending->socket = accepted;
		if (listener->pending_tail != NULL)
			listener->pending_tail->next = pending;
		else
			listener->pending_head = pending;
		listener->pending_tail = pending;
		listener->pending_count++;
		waitq_wake_one(&listener->socket.accept_waitq);
		poll_notify();
	}
	spin_unlock_irqrestore(&listener->socket.lock, irq);
	socket_release(listener_socket);
	if (error != 0) {
		kern_free(pending);
		kern_free(connection);
		socket_release(accepted);
	}
	return error;
}

static int
unix_connect(struct socket *socket, const struct sockaddr *address,
	socklen_t length, unsigned io_flags)
{
	(void)socket;
	(void)address;
	(void)length;
	(void)io_flags;
	return EOPNOTSUPP;
}

int
unix_socket_connect_path(struct socket *socket, struct cwdinfo *context,
	const struct ucred *cred, const struct sockaddr *address,
	socklen_t length, unsigned io_flags)
{
	struct socket *listener;
	char path[UNIX_PATH_MAX];
	int error;
	if (socket == NULL || socket->family != AF_UNIX)
		return EINVAL;
	error = unix_resolve_endpoint(context, cred, address, length,
	    socket->type, &listener, path);
	if (error != 0)
		return error;
	return unix_connect_resolved(socket, listener, path, io_flags);
}

static int
unix_accept(struct socket *socket, struct socket **result,
	struct sockaddr *address, socklen_t *length, unsigned io_flags)
{
	struct unix_socket *listener = unix_endpoint(socket);
	struct unix_pending *pending;
	struct thread *thread = thread_current();
	unsigned long irq;
	int error;
	if (result == NULL)
		return EINVAL;
	if (socket->type != SOCK_STREAM)
		return EOPNOTSUPP;
	irq = spin_lock_irqsave(&socket->lock);
	if (!listener->listening) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return EINVAL;
	}
	while (listener->pending_head == NULL) {
		uint64_t sequence;
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
	pending = listener->pending_head;
	listener->pending_head = pending->next;
	if (listener->pending_head == NULL)
		listener->pending_tail = NULL;
	listener->pending_count--;
	*result = pending->socket;
	if (address != NULL && length != NULL)
		unix_store_address(NULL, address, length);
	spin_unlock_irqrestore(&socket->lock, irq);
	kern_free(pending);
	return 0;
}

static int
unix_getsockname(struct socket *socket, struct sockaddr *address,
	socklen_t *length)
{
	if (address == NULL || length == NULL)
		return EINVAL;
	unix_store_address(unix_endpoint(socket), address, length);
	return 0;
}

static int
unix_getpeername(struct socket *socket, struct sockaddr *address,
	socklen_t *length)
{
	struct socket *peer;
	int error;
	if (address == NULL || length == NULL)
		return EINVAL;
	if (socket->type == SOCK_DGRAM) {
		struct unix_socket temporary;
		struct unix_socket *endpoint = unix_endpoint(socket);
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
	error = unix_peer_ref(unix_endpoint(socket), &peer);
	if (error != 0)
		return error;
	unix_store_address(unix_endpoint(peer), address, length);
	socket_release(peer);
	return 0;
}

static int
unix_poll(struct socket *socket, short events, short *revents)
{
	struct unix_socket *endpoint = unix_endpoint(socket);
	int error = socket_poll_common(socket, events, revents);
	unsigned long irq;
	if (error != 0 || !endpoint->listening)
		return error;
	irq = spin_lock_irqsave(&socket->lock);
	if (endpoint->pending_head != NULL)
		*revents |= events & (POLLIN | POLLRDNORM);
	*revents &= (short)~(POLLOUT | POLLWRNORM);
	spin_unlock_irqrestore(&socket->lock, irq);
	return 0;
}

static void
unix_close(struct socket *socket)
{
	struct unix_socket *endpoint = unix_endpoint(socket);
	struct unix_connection *connection = endpoint->connection;
	struct unix_pending *pending;
	struct socket *peer = NULL;
	unsigned long irq;

	if (endpoint->bound) {
		struct inode *inode = endpoint->bound_path.p_inode;
		if (inode != NULL) {
			mutex_lock(&inode->i_lock);
			if (inode->i_special == socket)
				inode->i_special = NULL;
			mutex_unlock(&inode->i_lock);
		}
		endpoint->bound = 0;
		path_release(&endpoint->bound_path);
	}
	if (endpoint->datagram_peer != NULL)
		socket_release(endpoint->datagram_peer);
	while ((pending = endpoint->pending_head) != NULL) {
		endpoint->pending_head = pending->next;
		socket_release(pending->socket);
		kern_free(pending);
	}

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
			spin_unlock_irqrestore(&peer->lock, irq);
			poll_notify();
			socket_release(peer);
		}
		unix_connection_release(connection);
	}
	kern_free(endpoint);
}

static const struct socket_ops unix_ops = {
	.bind = unix_bind,
	.connect = unix_connect,
	.listen = unix_listen,
	.accept = unix_accept,
	.sendto = unix_sendto,
	.recvfrom = unix_recvfrom,
	.shutdown = unix_shutdown,
	.getsockname = unix_getsockname,
	.getpeername = unix_getpeername,
	.poll = unix_poll,
	.close = unix_close,
};

static int
unix_create(int type, int protocol, struct socket **result)
{
	struct unix_socket *endpoint;

	if ((type != SOCK_STREAM && type != SOCK_DGRAM) || protocol != 0)
		return EPROTONOSUPPORT;
	endpoint = kern_calloc(1, sizeof(*endpoint));
	if (endpoint == NULL)
		return ENOMEM;
	socket_init_object(&endpoint->socket, AF_UNIX, type, protocol, &unix_ops);
	path_init(&endpoint->bound_path);
	*result = &endpoint->socket;
	return 0;
}

int
unix_socket_pair_create(int type, int protocol, struct socket **left_result,
	struct socket **right_result)
{
	struct socket *left = NULL, *right = NULL;
	int error;

	if (left_result == NULL || right_result == NULL)
		return EINVAL;
	error = socket_create(AF_UNIX, type, protocol, &left);
	if (error != 0)
		return error;
	error = socket_create(AF_UNIX, type, protocol, &right);
	if (error != 0) {
		socket_release(left);
		return error;
	}
	error = unix_connection_create(left, right);
	if (error != 0) {
		socket_release(left);
		socket_release(right);
		return error;
	}
	*left_result = left;
	*right_result = right;
	return 0;
}

int
unix_socket_init(void)
{
	static const struct socket_family_ops family_ops = { .create = unix_create };
	return socket_family_register(AF_UNIX, &family_ops);
}
