/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/socket.h"
#include "kern/file.h"
#include "kern/filedesc.h"
#include "kern/poll.h"

#include <errno.h>
#include <fcntl.h>

static ssize_t
socket_file_read(struct file *file, void *buffer, size_t length)
{
	struct socket *socket = socket_from_file(file);

	if (socket == NULL)
		return -EBADF;
	if ((socket->type != SOCK_STREAM && socket->type != SOCK_DGRAM) ||
	    socket->ops == NULL ||
	    socket->ops->recvfrom == NULL)
		return -EOPNOTSUPP;
	return socket->ops->recvfrom(socket, buffer, length,
	    (file_status_flags_get(file) & O_NONBLOCK) != 0 ?
	    MSG_DONTWAIT : 0, NULL, NULL);
}

static ssize_t
socket_file_write(struct file *file, const void *buffer, size_t length)
{
	struct socket *socket = socket_from_file(file);

	if (socket == NULL)
		return -EBADF;
	if ((socket->type != SOCK_STREAM && socket->type != SOCK_DGRAM) ||
	    socket->ops == NULL ||
	    socket->ops->sendto == NULL)
		return -EOPNOTSUPP;
	return socket->ops->sendto(socket, buffer, length,
	    (file_status_flags_get(file) & O_NONBLOCK) != 0 ?
	    MSG_DONTWAIT : 0, NULL, 0);
}

static int
socket_file_ioctl(struct file *file, unsigned long request, uintptr_t argument)
{
	struct socket *socket = socket_from_file(file);

	/* A reserved wrapper may be discarded before an endpoint is attached. */
	if (socket == NULL)
		return 0;
	if (socket->ops == NULL || socket->ops->ioctl == NULL)
		return EOPNOTSUPP;
	return socket->ops->ioctl(socket, request, argument);
}

static int
socket_file_close(struct file *file)
{
	struct socket *socket = socket_from_file(file);

	if (socket == NULL)
		return EBADF;
	file->f_data = NULL;
	socket_close_endpoint(socket);
	socket_release(socket);
	return 0;
}

static int
socket_file_poll(struct file *file, short events, short *revents)
{
	struct socket *socket = socket_from_file(file);
	if (socket == NULL)
		return EBADF;
	return socket->ops != NULL && socket->ops->poll != NULL ?
		socket->ops->poll(socket, events, revents) :
		socket_poll_common(socket, events, revents);
}

static const struct file_ops socket_file_ops = {
	.read = socket_file_read,
	.write = socket_file_write,
	.ioctl = socket_file_ioctl,
	.poll = socket_file_poll,
	.close = socket_file_close,
};

int
socket_file_create(struct socket *socket, struct file **result)
{
	int error;

	if (socket == NULL || result == NULL)
		return EINVAL;
	error = socket_file_reserve(result);
	if (error == 0)
		error = socket_file_attach(*result, socket);
	if (error != 0 && *result != NULL) {
		(void)file_close(*result);
		*result = NULL;
	}
	return error;
}

int
socket_file_reserve(struct file **result)
{
	if (result == NULL)
		return EINVAL;
	*result = NULL;
	return file_create_pseudo(&socket_file_ops, O_RDWR, NULL, result);
}

int
socket_file_attach(struct file *file, struct socket *socket)
{
	if (file == NULL || file->f_ops != &socket_file_ops ||
	    file->f_data != NULL || socket == NULL)
		return EINVAL;
	file->f_data = socket;
	return 0;
}

struct socket *
socket_from_file(struct file *file)
{
	return file != NULL && file->f_ops == &socket_file_ops ?
		file->f_data : NULL;
}

int
socket_file_ref_get(struct filedesc *fd, int descriptor,
	struct socket_file_ref *reference)
{
	if (reference == NULL)
		return EINVAL;
	reference->file = filedesc_get_ref(fd, descriptor);
	reference->socket = socket_from_file(reference->file);
	if (reference->socket == NULL) {
		if (reference->file != NULL)
			(void)file_close(reference->file);
		reference->file = NULL;
		return EBADF;
	}
	return 0;
}

void
socket_file_ref_put(struct socket_file_ref *reference)
{
	if (reference == NULL)
		return;
	if (reference->file != NULL)
		(void)file_close(reference->file);
	reference->file = NULL;
	reference->socket = NULL;
}

unsigned
socket_file_effective_flags(const struct socket_file_ref *reference,
	int message_flags)
{
	unsigned flags = 0;
	if (reference != NULL && reference->file != NULL &&
	    (file_status_flags_get(reference->file) & O_NONBLOCK) != 0)
		flags |= MSG_DONTWAIT;
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
	return flags;
}
