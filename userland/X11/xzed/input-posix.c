/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/X11/xzed/input.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <zedbsd/input.h>

static int
system_open(void *context, const char *path)
{
	(void)context;
	return open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
}

static int
system_get_bits(void *context, int fd, unsigned type, void *bits, size_t size)
{
	(void)context;
	return ioctl(fd, EVIOCGBIT(type, size), bits);
}

static int
system_get_key_state(void *context, int fd, void *bits, size_t size)
{
	(void)context;
	return ioctl(fd, EVIOCGKEY(size), bits);
}

static int
system_get_abs(void *context, int fd, unsigned axis,
	struct input_absinfo *information)
{
	(void)context;
	return ioctl(fd, EVIOCGABS(axis), information);
}

static ssize_t
system_read(void *context, int fd, void *buffer, size_t size)
{
	(void)context;
	return read(fd, buffer, size);
}

static int
system_close(void *context, int fd)
{
	(void)context;
	return close(fd);
}

static unsigned
system_pause(void *context, unsigned seconds)
{
	(void)context;
	return sleep(seconds);
}

int
xzed_input_open(struct xzed_input **result, unsigned width, unsigned height,
	const struct xzed_input_handlers *handlers, void *handler_context)
{
	static const struct xzed_input_io system_io = {
	    system_open,
	    system_get_bits,
	    system_get_key_state,
	    system_get_abs,
	    system_read,
	    system_close,
	    system_pause,
	};
	return xzed_input_open_with_io(result, "/dev/input", width, height,
	    handlers, handler_context, &system_io, NULL);
}
