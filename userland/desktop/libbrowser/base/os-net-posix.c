/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The network of the operating-system layer on POSIX systems.
 */

#include "base/os-net.h"
#include "base/base.h"

#include <errno.h>
#include <stddef.h>

/* Not implemented yet. */
int
wb_os_resolve(
	const char *host,
	const char *port,
	struct wb_os_addresses **addresses)
{
	UNUSED_PARAMETER(host);
	UNUSED_PARAMETER(port);
	UNUSED_PARAMETER(addresses);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
size_t
wb_os_addresses_count(
	const struct wb_os_addresses *addresses)
{
	UNUSED_PARAMETER(addresses);

	/* A skeleton: the function is not written yet. */
	return 0;
}

/* Not implemented yet. */
void
wb_os_addresses_free(
	struct wb_os_addresses *addresses)
{
	UNUSED_PARAMETER(addresses);
}

/* Not implemented yet. */
int
wb_os_socket_connect(
	const struct wb_os_addresses *addresses,
	size_t index,
	int blocking,
	wb_os_socket *socket)
{
	UNUSED_PARAMETER(addresses);
	UNUSED_PARAMETER(index);
	UNUSED_PARAMETER(blocking);
	UNUSED_PARAMETER(socket);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_socket_connect_result(
	wb_os_socket socket)
{
	UNUSED_PARAMETER(socket);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_socket_send(
	wb_os_socket socket,
	const void *bytes,
	size_t length,
	size_t *sent)
{
	UNUSED_PARAMETER(socket);
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(length);
	UNUSED_PARAMETER(sent);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_socket_receive(
	wb_os_socket socket,
	void *bytes,
	size_t length,
	size_t *received)
{
	UNUSED_PARAMETER(socket);
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(length);
	UNUSED_PARAMETER(received);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
void
wb_os_socket_close(
	wb_os_socket socket)
{
	UNUSED_PARAMETER(socket);
}

/* Not implemented yet. */
int
wb_os_socket_descriptor(
	wb_os_socket socket,
	int *descriptor)
{
	UNUSED_PARAMETER(socket);
	UNUSED_PARAMETER(descriptor);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_socket_last_error(
	void)
{
	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_poll(
	struct wb_os_poll_item *items,
	size_t count,
	int timeout_ms,
	size_t *ready)
{
	UNUSED_PARAMETER(items);
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(timeout_ms);
	UNUSED_PARAMETER(ready);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_waker_open(
	struct wb_os_waker **waker)
{
	UNUSED_PARAMETER(waker);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
void
wb_os_waker_ring(
	struct wb_os_waker *waker)
{
	UNUSED_PARAMETER(waker);
}

/* Not implemented yet. */
void
wb_os_waker_drain(
	struct wb_os_waker *waker)
{
	UNUSED_PARAMETER(waker);
}

/* Not implemented yet. */
wb_os_socket
wb_os_waker_socket(
	const struct wb_os_waker *waker)
{
	UNUSED_PARAMETER(waker);

	/* A skeleton: the function is not written yet. */
	return WB_OS_SOCKET_NONE;
}

/* Not implemented yet. */
void
wb_os_waker_close(
	struct wb_os_waker *waker)
{
	UNUSED_PARAMETER(waker);
}
