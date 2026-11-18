/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Mail's connection to a server (mail.h): a TCP socket to a
 * host and port, TLS from the start or after STARTTLS (tls.c), and reads
 * of a whole line or of a counted run of bytes through a buffer, each
 * waiting at most the connection's timeout for the server.
 *
 * A connection that the server does not take within CONN_CONNECT_MS fails
 * with ETIMEDOUT instead of the system's long wait.
 */

#include "mail.h"

#include <keiland/keiland_socket.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How long a read waits for the server by default (ms). */
#define CONN_TIMEOUT_MS		30000

/* How long a connection waits for the server to take it (ms). */
#define CONN_CONNECT_MS		15000

/* The highest port number. */
#define CONN_PORT_MAX		65535UL

/* The addresses of a server tried in turn. */
#define CONN_ADDRESSES		16U

static int conn_fill(struct ml_conn *conn);
static int conn_connect(const struct kl_address *address, struct kl_socket **socket);

/*
 * Reads a server as "host" or "host:port": the port (the fallback when
 * none is given) and whether TLS starts at once (the ports 993 and 465;
 * every other port is upgraded with STARTTLS).  Returns 0 or EINVAL.
 */
int
ml_server_parse(
	const char *text,
	unsigned fallback_port,
	struct ml_server *server)
{
	const char *colon;
	unsigned long port;
	size_t length;
	char *end;

	/* Nothing yet. */
	memset(server, 0, sizeof(server[0]));

	/* The host, up to a colon. */
	colon = strchr(text, ':');
	length = strlen(text);
	if (colon != NULL)
		length = (size_t)(colon - text);
	if (length == 0U || length >= sizeof(server->host))
		return EINVAL;
	memcpy(server->host, text, length);
	server->host[length] = '\0';

	/* The port given after it, or the fallback. */
	port = fallback_port;
	if (colon != NULL) {
		errno = 0;
		port = strtoul(colon + 1, &end, 10);
		if (errno != 0 || *end != '\0' || end == colon + 1)
			return EINVAL;
	}

	/* A port in range. */
	if (port == 0UL || port > CONN_PORT_MAX)
		return EINVAL;
	server->port = (unsigned)port;

	/* TLS from the start on the TLS ports. */
	if (port == 993UL || port == 465UL)
		server->secure = 1;

	/* Succeeded: the server is read. */
	return 0;
}

/*
 * Connects to a server, with TLS from the start when the server is secure.
 * Returns 0, an errno value of the network, ENOENT for a host not found,
 * or a TLS failure (ml_tls_error says why).
 */
int
ml_conn_open(
	struct ml_conn *conn,
	const struct ml_server *server)
{
	struct kl_address addresses[CONN_ADDRESSES];
	struct kl_socket *socket;
	size_t count;
	size_t index;
	int status;
	int error;

	/* Nothing yet. */
	memset(conn, 0, sizeof(conn[0]));
	conn->timeout_ms = CONN_TIMEOUT_MS;

	/* The host's addresses for a stream; any failure to find them is a name not found, as the window says. */
	status = kl_socket_resolve(
		server->host,
		(uint16_t)server->port,
		KL_ADDRESS_ANY,
		KL_SOCKET_STREAM,
		addresses,
		CONN_ADDRESSES,
		&count);
	if (status != 0)
		return ENOENT;

	/* The first address that takes the connection. */
	error = ECONNREFUSED;
	for (index = 0; index < count; index++) {
		/* Connected in time, or the next address. */
		status = conn_connect(&addresses[index], &socket);
		if (status != 0) {
			error = status;
			continue;
		}

		/* This one. */
		conn->socket = socket;
		break;
	}

	/* No address took it. */
	if (conn->socket == NULL)
		return error;

	/* A secure server: TLS at once. */
	if (server->secure) {
		error = ml_conn_start_tls(conn, server);
		if (error != 0) {
			ml_conn_close(conn);
			return error;
		}
	}

	/* Succeeded: the connection is open. */
	return 0;
}

/*
 * Starts TLS on an open plain connection (after STARTTLS), checking the
 * certificate against the server's host, or against its pin.  Returns 0
 * or a TLS failure.
 */
int
ml_conn_start_tls(
	struct ml_conn *conn,
	const struct ml_server *server)
{
	intptr_t native;
	int error;

	/* Bytes the server sent before the handshake would be read as plain text after it. */
	if (conn->start != conn->end)
		return EPROTO;

	/* The handshake. */
	native = kl_socket_native(conn->socket);
	error = ml_tls_open(native, server->host, server->pin, &conn->tls);
	if (error != 0)
		return error;

	/* Succeeded: the connection is TLS from here. */
	return 0;
}

/*
 * Sends bytes.  Returns 0 or an errno value.
 */
int
ml_conn_write(
	struct ml_conn *conn,
	const char *bytes,
	size_t length)
{
	size_t written;
	size_t done;
	int error;

	/* Through TLS. */
	if (conn->tls != NULL) {
		error = ml_tls_write(conn->tls, (const unsigned char *)bytes, length);
		if (error != 0)
			return error;
		return 0;
	}

	/* Plain, until everything is sent. */
	done = 0;
	while (done < length) {
		error = kl_socket_send(conn->socket, bytes + done, length - done, &written);
		if (error != 0 || written == 0U)
			return EIO;
		done += written;
	}

	/* Succeeded: everything is sent. */
	return 0;
}

/*
 * Reads a line without its line end (CR LF or LF) into a room; a longer
 * line is cut to the room and the rest of it is dropped.  Returns 0,
 * EPIPE when the server closed, ETIMEDOUT, or an errno value.
 */
int
ml_conn_line(
	struct ml_conn *conn,
	char *line,
	size_t size)
{
	size_t length;
	unsigned char byte;
	int error;

	/* Byte by byte from the buffer, refilled as it empties. */
	length = 0;
	for (;;) {
		/* An empty buffer is filled. */
		if (conn->start == conn->end) {
			error = conn_fill(conn);
			if (error != 0)
				return error;
		}

		/* The next byte; a line feed ends the line. */
		byte = conn->buffer[conn->start];
		conn->start++;
		if (byte == '\n')
			break;

		/* Kept while there is room (a CR is dropped at the end below). */
		if (length + 1U < size) {
			line[length] = (char)byte;
			length++;
		}
	}

	/* The line without its CR. */
	if (length > 0U && line[length - 1U] == '\r')
		length--;
	line[length] = '\0';

	/* Succeeded: one line read. */
	return 0;
}

/*
 * Reads exactly count bytes (an IMAP literal).  Returns 0, EPIPE when the
 * server closed first, ETIMEDOUT, or an errno value.
 */
int
ml_conn_bytes(
	struct ml_conn *conn,
	char *bytes,
	size_t count)
{
	size_t taken;
	size_t part;
	int error;

	/* From the buffer, refilled as it empties. */
	taken = 0;
	while (taken < count) {
		/* An empty buffer is filled. */
		if (conn->start == conn->end) {
			error = conn_fill(conn);
			if (error != 0)
				return error;
		}

		/* As much of the buffer as is wanted. */
		part = conn->end - conn->start;
		if (part > count - taken)
			part = count - taken;
		memcpy(bytes + taken, conn->buffer + conn->start, part);
		conn->start += part;
		taken += part;
	}

	/* Succeeded: the bytes are read. */
	return 0;
}

/*
 * Tells whether bytes wait without reading the socket: in the buffer, or
 * decrypted by TLS (then a poll of the socket would not see them).
 */
int
ml_conn_ready(
	const struct ml_conn *conn)
{
	int pending;

	/* In the buffer. */
	if (conn->start != conn->end)
		return 1;

	/* Held by TLS. */
	if (conn->tls != NULL) {
		pending = ml_tls_pending(conn->tls);
		if (pending)
			return 1;
	}

	/* Nothing waits. */
	return 0;
}

/*
 * Closes a connection (TLS first, then the socket).
 */
void
ml_conn_close(
	struct ml_conn *conn)
{
	/* TLS's close. */
	if (conn->tls != NULL)
		ml_tls_close(conn->tls);
	conn->tls = NULL;

	/* The socket. */
	kl_socket_close(conn->socket);
	conn->socket = NULL;
	conn->start = 0;
	conn->end = 0;
}

/* Fills the empty buffer with what the server sends, waiting at most the timeout. */
static int
conn_fill(
	struct ml_conn *conn)
{
	struct kl_socket_wait watched;
	size_t received;
	size_t decrypted;
	size_t ready;
	int pending;
	int error;

	/* Bytes TLS already holds need no wait; otherwise the socket must become readable in time. */
	pending = 0;
	if (conn->tls != NULL)
		pending = ml_tls_pending(conn->tls);

	/* Waits until the socket can be read, or the timeout passes. */
	if (!pending) {
		watched.socket = conn->socket;
		watched.waker = NULL;
		watched.events = KL_SOCKET_READABLE;
		error = kl_socket_poll(&watched, 1U, conn->timeout_ms, &ready);
		if (error != 0)
			return error;
		if (ready == 0U)
			return ETIMEDOUT;
	}

	/* The buffer starts empty. */
	conn->start = 0;
	conn->end = 0;

	/* Through TLS. */
	if (conn->tls != NULL) {
		error = ml_tls_read(conn->tls, conn->buffer, sizeof(conn->buffer), &decrypted);
		if (error != 0)
			return error;
		if (decrypted == 0U)
			return EPIPE;
		conn->end = decrypted;

		/* Succeeded: the buffer has decrypted bytes. */
		return 0;
	}

	/* Plain. */
	error = kl_socket_receive(conn->socket, conn->buffer, sizeof(conn->buffer), &received);
	if (error != 0)
		return error;
	if (received == 0U)
		return EPIPE;

	/* The bytes received are what the buffer holds. */
	conn->end = received;

	/* Succeeded: the buffer has bytes. */
	return 0;
}

/*
 * Connects a new socket to an address, waiting at most CONN_CONNECT_MS for
 * the server to take it; the socket blocks again after.  Returns 0 with
 * *socket set, ETIMEDOUT, or the connection's errno value.
 */
static int
conn_connect(
	const struct kl_address *address,
	struct kl_socket **socket)
{
	struct kl_socket_wait watched;
	struct kl_socket *made;
	size_t ready;
	int status;
	int error;

	/* A socket of the address's family that does not block while it connects. */
	*socket = NULL;
	error = kl_socket_open(address->family, KL_SOCKET_STREAM, KL_SOCKET_NONBLOCKING, &made);
	if (error != 0)
		return error;

	/* The connection started (a local one may be made at once). */
	status = kl_socket_connect(made, address);
	if (status != 0 && status != EINPROGRESS) {
		kl_socket_close(made);
		return status;
	}

	/* Its end, waited for in time, and how it ended. */
	if (status != 0) {
		watched.socket = made;
		watched.waker = NULL;
		watched.events = KL_SOCKET_WRITABLE;
		error = kl_socket_poll(&watched, 1U, CONN_CONNECT_MS, &ready);
		if (error == 0 && ready == 0U)
			error = ETIMEDOUT;
		if (error == 0)
			error = kl_socket_connect_result(made);
		if (error != 0) {
			kl_socket_close(made);
			return error;
		}
	}

	/* The socket blocks again, as the reads and writes expect. */
	error = kl_socket_set_nonblocking(made, 0);
	if (error != 0) {
		kl_socket_close(made);
		return error;
	}

	/* The connected socket goes to the caller. */
	*socket = made;

	/* Succeeded: the server took the connection. */
	return 0;
}
