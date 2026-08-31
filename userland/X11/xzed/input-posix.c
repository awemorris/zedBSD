/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD X11 input posix component.
 */

#include "userland/X11/xzed/input.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <zedbsd/input.h>

static int system_open(void *context, const char *path);
static int system_get_bits(void *context, int fd, unsigned type, void *bits, size_t size);
static int system_get_key_state(void *context, int fd, void *bits, size_t size);
static int system_get_abs(void *context, int fd, unsigned axis, struct input_absinfo *information);
static ssize_t system_read(void *context, int fd, void *buffer, size_t size);
static int system_close(void *context, int fd);
static unsigned system_pause(void *context, unsigned seconds);

/*
 * Implements the xzed input open operation.
 */
int
xzed_input_open(
	struct xzed_input **result,
	unsigned width,
	unsigned height,
	const struct xzed_input_handlers *handlers,
	void *handler_context)
{
	int function_result;
	static const struct xzed_input_io system_io = {
	    system_open,
	    system_get_bits,
	    system_get_key_state,
	    system_get_abs,
	    system_read,
	    system_close,
	    system_pause,
	};

	/* Obtains the xzed input open with io result. */
	function_result = xzed_input_open_with_io(result, "/dev/input", width, height,
	    handlers, handler_context, &system_io, NULL);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the system open operation. */
static int
system_open(
	void *context,
	const char *path)
{
	int function_result;

	(void)context;

	/* Obtains the open result. */
	function_result = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the system get bits operation. */
static int
system_get_bits(
	void *context,
	int fd,
	unsigned type,
	void *bits,
	size_t size)
{
	int function_result;

	(void)context;

	/* Obtains the ioctl result. */
	function_result = ioctl(fd, EVIOCGBIT(type, size), bits);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the system get key state operation. */
static int
system_get_key_state(
	void *context,
	int fd,
	void *bits,
	size_t size)
{
	int function_result;

	(void)context;

	/* Obtains the ioctl result. */
	function_result = ioctl(fd, EVIOCGKEY(size), bits);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the system get abs operation. */
static int
system_get_abs(
	void *context,
	int fd,
	unsigned axis,
	struct input_absinfo *information)
{
	int function_result;

	(void)context;

	/* Obtains the ioctl result. */
	function_result = ioctl(fd, EVIOCGABS(axis), information);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the system read operation. */
static ssize_t
system_read(
	void *context,
	int fd,
	void *buffer,
	size_t size)
{
	ssize_t function_result;

	(void)context;

	/* Obtains the read result. */
	function_result = read(fd, buffer, size);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the system close operation. */
static int
system_close(
	void *context,
	int fd)
{
	int function_result;

	(void)context;

	/* Obtains the close result. */
	function_result = close(fd);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the system pause operation. */
static unsigned
system_pause(
	void *context,
	unsigned seconds)
{
	unsigned function_result;

	(void)context;

	/* Obtains the sleep result. */
	function_result = sleep(seconds);

	/* Returns the computed result. */
	return function_result;
}
