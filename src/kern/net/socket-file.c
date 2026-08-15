/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/socket.h"
#include "kern/file.h"
#include "kern/filedesc.h"

#include <errno.h>
#include <fcntl.h>

static ssize_t
socket_file_read(struct file *file, void *buffer, size_t length)
{
	struct socket *socket = socket_from_file(file);

	if (socket == NULL)
		return -EBADF;
	if (socket->type != SOCK_STREAM || socket->ops == NULL ||
	    socket->ops->recvfrom == NULL)
		return -EOPNOTSUPP;
	return socket->ops->recvfrom(socket, buffer, length,
	    (file->f_flags & O_NONBLOCK) != 0 ? MSG_DONTWAIT : 0, NULL, NULL);
}

static ssize_t
socket_file_write(struct file *file, const void *buffer, size_t length)
{
	struct socket *socket = socket_from_file(file);

	if (socket == NULL)
		return -EBADF;
	if (socket->type != SOCK_STREAM || socket->ops == NULL ||
	    socket->ops->sendto == NULL)
		return -EOPNOTSUPP;
	return socket->ops->sendto(socket, buffer, length,
	    (file->f_flags & O_NONBLOCK) != 0 ? MSG_DONTWAIT : 0, NULL, 0);
}

static int
socket_file_ioctl(struct file *file, unsigned long request, uintptr_t argument)
{
	struct socket *socket = socket_from_file(file);

	if (socket == NULL)
		return EBADF;
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
	socket_release(socket);
	return 0;
}

static const struct file_ops socket_file_ops = {
	.read = socket_file_read,
	.write = socket_file_write,
	.ioctl = socket_file_ioctl,
	.close = socket_file_close,
};

int
socket_file_create(struct socket *socket, struct file **result)
{
	if (socket == NULL || result == NULL)
		return EINVAL;
	return file_create_pseudo(&socket_file_ops, O_RDWR, socket, result);
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
	    (reference->file->f_flags & O_NONBLOCK) != 0)
		flags |= MSG_DONTWAIT;
	if ((message_flags & MSG_DONTWAIT) != 0)
		flags |= MSG_DONTWAIT;
	if ((message_flags & MSG_NOSIGNAL) != 0)
		flags |= MSG_NOSIGNAL;
	return flags;
}
