/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The file wrapper around a socket.
 *
 * A socket is exposed to user space as a pseudo file whose operations
 * forward to the socket's own operations.  A file can be reserved before
 * its socket exists so that a descriptor is guaranteed before the socket
 * is created.
 */

#include "kern/net/socket.h"
#include "kern/file.h"
#include "kern/filedesc.h"
#include "kern/poll.h"

#include <errno.h>
#include <fcntl.h>

static ssize_t socket_file_read(struct file *file, void *buffer, size_t length);
static ssize_t socket_file_write(struct file *file, const void *buffer, size_t length);
static int socket_file_ioctl(struct file *file, unsigned long request, uintptr_t argument);
static int socket_file_close(struct file *file);
static int socket_file_poll(struct file *file, short events, short *revents);

static const struct file_ops socket_file_ops = {
	.read = socket_file_read,
	.write = socket_file_write,
	.ioctl = socket_file_ioctl,
	.poll = socket_file_poll,
	.close = socket_file_close,
};

/*
 * Creates a file for a socket.
 */
int
socket_file_create(
	struct socket *socket,
	struct file **result)
{
	int error;

	/* Rejects a missing socket or result. */
	if (socket == NULL || result == NULL)
		return EINVAL;

	/* Reserves a file and attaches the socket to it. */
	error = socket_file_reserve(result);
	if (error == 0)
		error = socket_file_attach(*result, socket);

	/* Releases the file when the attachment failed. */
	if (error != 0 && *result != NULL) {
		(void)file_close(*result);
		*result = NULL;
	}

	/* Reports the creation result. */
	return error;
}

/*
 * Reserves a socket file with no socket attached yet.
 */
int
socket_file_reserve(
	struct file **result)
{
	int error;

	/* Rejects a missing result. */
	if (result == NULL)
		return EINVAL;

	/* Creates an empty read-write pseudo file. */
	*result = NULL;
	error = file_create_pseudo(&socket_file_ops, O_RDWR, NULL, result);

	/* Reports the creation result. */
	return error;
}

/*
 * Attaches a socket to a reserved socket file.
 */
int
socket_file_attach(
	struct file *file,
	struct socket *socket)
{
	/* Rejects anything but an empty socket file and a socket. */
	if (file == NULL ||
	    file->f_ops != &socket_file_ops ||
	    file->f_data != NULL ||
	    socket == NULL)
		return EINVAL;

	file->f_data = socket;

	/* Reports the attachment. */
	return 0;
}

/*
 * Finds the socket behind a file, or none for another kind of file.
 */
struct socket *
socket_from_file(
	struct file *file)
{
	/* Only a socket file carries a socket. */
	if (file == NULL)
		return NULL;
	if (file->f_ops != &socket_file_ops)
		return NULL;

	/* Reports the attached socket, or none while reserved. */
	return file->f_data;
}

/*
 * References the socket file behind a descriptor.
 */
int
socket_file_ref_get(
	struct filedesc *fd,
	int descriptor,
	struct socket_file_ref *reference)
{
	/* Rejects a missing reference. */
	if (reference == NULL)
		return EINVAL;

	/* References the file and finds its socket. */
	reference->file = filedesc_get_ref(fd, descriptor);
	reference->socket = socket_from_file(reference->file);

	/* A descriptor without a socket is released again. */
	if (reference->socket == NULL) {
		if (reference->file != NULL)
			(void)file_close(reference->file);
		reference->file = NULL;
		return EBADF;
	}

	/* Reports the referenced socket file. */
	return 0;
}

/*
 * Releases a socket file reference.
 */
void
socket_file_ref_put(
	struct socket_file_ref *reference)
{
	/* Ignores a missing reference. */
	if (reference == NULL)
		return;

	/* Drops the file reference and clears the record. */
	if (reference->file != NULL)
		(void)file_close(reference->file);
	reference->file = NULL;
	reference->socket = NULL;
}

/*
 * Combines a file's non-blocking mode with the message flags of a call.
 */
unsigned
socket_file_effective_flags(
	const struct socket_file_ref *reference,
	int message_flags)
{
	unsigned flags;

	flags = 0;

	/* A non-blocking file makes every call non-blocking. */
	if (reference != NULL &&
	    reference->file != NULL &&
	    (file_status_flags_get(reference->file) & O_NONBLOCK) != 0)
		flags |= MSG_DONTWAIT;

	/* Copies the supported message flags. */
	if ((message_flags & MSG_DONTWAIT) != 0)
		flags |= MSG_DONTWAIT;
	if ((message_flags & MSG_NOSIGNAL) != 0)
		flags |= MSG_NOSIGNAL;
	if ((message_flags & MSG_PEEK) != 0)
		flags |= MSG_PEEK;
	if ((message_flags & MSG_TRUNC) != 0)
		flags |= MSG_TRUNC;
	if ((message_flags & MSG_WAITALL) != 0)
		flags |= MSG_WAITALL;
	if ((message_flags & MSG_CMSG_CLOEXEC) != 0)
		flags |= MSG_CMSG_CLOEXEC;
	if ((message_flags & MSG_CMSG_CLOFORK) != 0)
		flags |= MSG_CMSG_CLOFORK;

	/* Reports the combined flags. */
	return flags;
}

/* Reads from a socket file as a receive without an address. */
static ssize_t
socket_file_read(
	struct file *file,
	void *buffer,
	size_t length)
{
	struct socket *socket;
	ssize_t result;
	int flags;

	socket = socket_from_file(file);

	/* Rejects a file without a socket or a socket that cannot receive. */
	if (socket == NULL)
		return -EBADF;
	if ((socket->type != SOCK_STREAM &&
	     socket->type != SOCK_DGRAM &&
	     socket->type != SOCK_RAW) ||
	    socket->ops == NULL ||
	    socket->ops->recvfrom == NULL)
		return -EOPNOTSUPP;

	/* A non-blocking file receives without waiting. */
	if ((file_status_flags_get(file) & O_NONBLOCK) != 0)
		flags = MSG_DONTWAIT;
	else
		flags = 0;

	/* Receives through the socket. */
	result = socket->ops->recvfrom(socket, buffer, length, flags, NULL, NULL);

	/* Reports the receive result. */
	return result;
}

/* Writes to a socket file as a send without an address. */
static ssize_t
socket_file_write(
	struct file *file,
	const void *buffer,
	size_t length)
{
	struct socket *socket;
	ssize_t result;
	int flags;

	socket = socket_from_file(file);

	/* Rejects a file without a socket or a socket that cannot send. */
	if (socket == NULL)
		return -EBADF;
	if ((socket->type != SOCK_STREAM && socket->type != SOCK_DGRAM) ||
	    socket->ops == NULL ||
	    socket->ops->sendto == NULL)
		return -EOPNOTSUPP;

	/* A non-blocking file sends without waiting. */
	if ((file_status_flags_get(file) & O_NONBLOCK) != 0)
		flags = MSG_DONTWAIT;
	else
		flags = 0;

	/* Sends through the socket. */
	result = socket->ops->sendto(socket, buffer, length, flags, NULL, 0);

	/* Reports the send result. */
	return result;
}

/* Forwards an ioctl to the socket. */
static int
socket_file_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	struct socket *socket;
	int error;

	socket = socket_from_file(file);

	/* A reserved wrapper may be discarded before an endpoint is attached. */
	if (socket == NULL)
		return 0;

	/* Rejects a socket without an ioctl operation. */
	if (socket->ops == NULL || socket->ops->ioctl == NULL)
		return EOPNOTSUPP;

	/* Forwards the request. */
	error = socket->ops->ioctl(socket, request, argument);

	/* Reports the socket's result. */
	return error;
}

/* Closes the socket when its file is closed. */
static int
socket_file_close(
	struct file *file)
{
	struct socket *socket;

	socket = socket_from_file(file);

	/* A file without a socket has nothing to close. */
	if (socket == NULL)
		return EBADF;

	/* Detaches the socket, closes its endpoint, and drops the file's reference. */
	file->f_data = NULL;
	socket_close_endpoint(socket);
	socket_release(socket);

	/* Reports the closed socket. */
	return 0;
}

/* Polls the socket, through its own poll operation when it has one. */
static int
socket_file_poll(
	struct file *file,
	short events,
	short *revents)
{
	struct socket *socket;
	int error;

	socket = socket_from_file(file);

	/* A file without a socket cannot be polled. */
	if (socket == NULL)
		return EBADF;

	/* Uses the socket's poll, or the common queue-based one. */
	if (socket->ops != NULL && socket->ops->poll != NULL)
		error = socket->ops->poll(socket, events, revents);
	else
		error = socket_poll_common(socket, events, revents);

	/* Reports the poll result. */
	return error;
}
