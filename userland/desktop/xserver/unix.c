/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD's transport of the core: the AF_UNIX socket the clients connect
 * to (/tmp/.X11-unix/X0), and their connections.
 *
 * Every socket is non-blocking.  What a client sends is handed to the core
 * as it comes, but no more than the longest request's bytes
 * (X11SERVER_REQUEST_MAX) that the core has not yet handled: past that the socket is not read, so a client that sends
 * faster than the core works waits in its own writes.  What the core has
 * for a client is sent as far as its socket takes it.  The caller must
 * ignore SIGPIPE: a client that goes away mid-write is found by the next
 * read or write.
 */

#include "userland/desktop/xserver/program.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* The default directory of the socket. */
#define UNIX_SOCKET_DIR		"/tmp/.X11-unix"

/* The most bytes read from a client at once, and the most it may have handed in that the core has not handled. */
#define UNIX_READ_CHUNK		4096U
#define UNIX_WINDOW		X11SERVER_REQUEST_MAX

/*
 * One client's connection: its socket (-1 for a free slot), its number in
 * the core, and the bytes handed in that the core has not yet handled.
 */
struct unix_connection {
	int fd;
	uint32_t client;
	size_t pending;
};

/*
 * The transport: the listening socket and its path (unlinked at the end),
 * and the connections.
 */
struct x11_unix {
	int listener;
	char path[108];
	struct unix_connection connections[X11_UNIX_CLIENTS];
};

static int unix_listen(struct x11_unix *transport, const char *path);
static struct unix_connection *unix_find(struct x11_unix *transport, uint32_t client);
static void unix_drop(struct unix_connection *connection);
static void unix_hangup(struct x11server *server, struct unix_connection *connection);
static void unix_events(struct x11_unix *transport, struct x11server *server);
static void unix_read_one(struct x11server *server, struct unix_connection *connection);
static void unix_send_one(struct x11server *server, struct unix_connection *connection);

/*
 * Makes the listening socket at a path (a stale one there is replaced).
 * Returns 0, or an errno value.
 */
int
x11_unix_open(
	struct x11_unix **result,
	const char *path)
{
	struct x11_unix *transport;
	unsigned index;
	int error;

	/* The transport, with every slot free. */
	*result = NULL;
	transport = calloc(1U, sizeof(*transport));
	if (transport == NULL)
		return ENOMEM;
	transport->listener = -1;
	for (index = 0U; index < X11_UNIX_CLIENTS; index++)
		transport->connections[index].fd = -1;

	/* The socket. */
	error = unix_listen(transport, path);
	if (error != 0) {
		x11_unix_close(transport);
		return error;
	}

	/* Succeeded: clients can connect. */
	*result = transport;
	return 0;
}

/*
 * Fills the descriptors the caller waits for: the listening socket, and
 * each client, readable while the core has room for more of its input
 * and writable while the core has bytes for it.  Returns the count filled.
 */
unsigned
x11_unix_pollfds(
	struct x11_unix *transport,
	struct x11server *server,
	struct pollfd *descriptors,
	unsigned capacity)
{
	struct unix_connection *connection;
	size_t waiting;
	unsigned count;
	unsigned index;

	/* The listening socket, when there is room. */
	if (capacity < 1U)
		return 0U;
	descriptors[0].fd = transport->listener;
	descriptors[0].events = POLLIN;
	descriptors[0].revents = 0;
	count = 1U;

	/* Each connected client. */
	for (index = 0U; index < X11_UNIX_CLIENTS && count < capacity; index++) {
		connection = &transport->connections[index];
		if (connection->fd < 0)
			continue;

		/* Its readable side while the core has room, and its writable side while it has bytes queued. */
		descriptors[count].fd = connection->fd;
		descriptors[count].events = 0;
		if (connection->pending < UNIX_WINDOW)
			descriptors[count].events |= POLLIN;
		(void)x11server_output(server, connection->client, &waiting);
		if (waiting != 0U)
			descriptors[count].events |= POLLOUT;
		descriptors[count].revents = 0;
		count++;
	}

	/* Succeeded: the descriptors to wait for. */
	return count;
}

/*
 * Accepts every waiting connection when the listening socket is readable;
 * one the core has no room for is closed.
 */
void
x11_unix_accept(
	struct x11_unix *transport,
	struct x11server *server,
	const struct pollfd *descriptors,
	unsigned count)
{
	struct unix_connection *connection;
	uint32_t client;
	unsigned index;
	int descriptor;
	int error;

	/* Only when a connection waits. */
	if (count < 1U || (descriptors[0].revents & POLLIN) == 0)
		return;

	/* Each waiting connection. */
	for (;;) {
		descriptor = accept4(transport->listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
		if (descriptor < 0)
			return;

		/* A free slot of the transport's. */
		for (index = 0U; index < X11_UNIX_CLIENTS; index++) {
			if (transport->connections[index].fd < 0)
				break;
		}

		/* No room here: the connection is refused. */
		if (index == X11_UNIX_CLIENTS) {
			(void)close(descriptor);
			continue;
		}

		/* The core's client; no room there refuses the connection too. */
		error = x11server_connect(server, NULL, &client);
		if (error != 0) {
			(void)close(descriptor);
			continue;
		}

		/* The connection, with nothing handed in yet. */
		connection = &transport->connections[index];
		connection->fd = descriptor;
		connection->client = client;
		connection->pending = 0U;
	}
}

/*
 * Reads every client, not only the readable ones (a read that finds
 * nothing costs little, and no request is left waiting for a wakeup that
 * may not come), and hands what came to the core.
 */
void
x11_unix_read(
	struct x11_unix *transport,
	struct x11server *server)
{
	struct unix_connection *connection;
	unsigned index;

	/* What the core said first: clients it closed, input it handled. */
	unix_events(transport, server);

	/* Each connected client. */
	for (index = 0U; index < X11_UNIX_CLIENTS; index++) {
		connection = &transport->connections[index];
		if (connection->fd >= 0)
			unix_read_one(server, connection);
	}
}

/*
 * Hears what the core said (clients it closed, input it handled) and
 * sends each client what the core has for it, as far as its socket takes
 * it.
 */
void
x11_unix_write(
	struct x11_unix *transport,
	struct x11server *server)
{
	struct unix_connection *connection;
	unsigned index;

	/* What the core said. */
	unix_events(transport, server);

	/* Each connected client's bytes. */
	for (index = 0U; index < X11_UNIX_CLIENTS; index++) {
		connection = &transport->connections[index];
		if (connection->fd >= 0)
			unix_send_one(server, connection);
	}
}

/*
 * Closes every connection and the listening socket, and unlinks its path.
 * The core's clients are the caller's to end (x11server_destroy).
 */
void
x11_unix_close(
	struct x11_unix *transport)
{
	unsigned index;

	/* Nothing was made. */
	if (transport == NULL)
		return;

	/* Each connection. */
	for (index = 0U; index < X11_UNIX_CLIENTS; index++)
		unix_drop(&transport->connections[index]);

	/* The listening socket and its name. */
	if (transport->listener >= 0) {
		(void)close(transport->listener);
		(void)unlink(transport->path);
	}

	/* The transport itself. */
	free(transport);
}

/* Makes the listening socket at a path, and remembers the path. */
static int
unix_listen(
	struct x11_unix *transport,
	const char *path)
{
	struct sockaddr_un address;
	size_t length;
	int error;

	/* The path must fit an address. */
	length = strlen(path);
	if (length >= sizeof(address.sun_path) || length >= sizeof(transport->path))
		return ENAMETOOLONG;
	memcpy(transport->path, path, length + 1U);

	/* The default directory, and no stale socket in the way. */
	(void)mkdir(UNIX_SOCKET_DIR, 0777);
	(void)unlink(path);

	/* The socket. */
	transport->listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (transport->listener < 0)
		return errno;

	/* Bound to the path. */
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	memcpy(address.sun_path, path, length + 1U);
	error = bind(transport->listener, (struct sockaddr *)&address, sizeof(address));
	if (error != 0)
		return errno;

	/* Listening. */
	error = listen(transport->listener, (int)X11_UNIX_CLIENTS);
	if (error != 0)
		return errno;

	/* Succeeded: clients can connect. */
	return 0;
}

/* Returns the connection of a core's client, or NULL. */
static struct unix_connection *
unix_find(
	struct x11_unix *transport,
	uint32_t client)
{
	unsigned index;

	/* The connected slot with the number. */
	for (index = 0U; index < X11_UNIX_CLIENTS; index++) {
		if (transport->connections[index].fd >= 0 && transport->connections[index].client == client)
			return &transport->connections[index];
	}

	/* None. */
	return NULL;
}

/* Closes a connection's socket, and the slot is free again. */
static void
unix_drop(
	struct unix_connection *connection)
{
	/* A free slot has nothing to close. */
	if (connection->fd < 0)
		return;

	/* The socket. */
	(void)close(connection->fd);
	connection->fd = -1;
	connection->client = 0U;
	connection->pending = 0U;
}

/* Ends a connection the client closed or that failed: the core's client goes with it. */
static void
unix_hangup(
	struct x11server *server,
	struct unix_connection *connection)
{
	/* The core first, then the socket. */
	x11server_hangup(server, connection->client);
	unix_drop(connection);
}

/* Hears every event of the core: a closed client's connection is closed, handled input makes room. */
static void
unix_events(
	struct x11_unix *transport,
	struct x11server *server)
{
	struct x11server_event event;
	struct unix_connection *connection;
	int more;

	/* Each event in turn. */
	for (;;) {
		more = x11server_next(server, &event);
		if (!more)
			return;

		/* The connection the event is about (a closed one may be gone already). */
		connection = unix_find(transport, event.client);
		if (connection == NULL)
			continue;

		/* A client the core closed: its connection goes too. */
		if (event.type == X11SERVER_EVENT_CLOSED) {
			unix_drop(connection);
			continue;
		}

		/* Input handled: room for as much more. */
		if (event.type == X11SERVER_EVENT_CONSUMED) {
			if (event.bytes > connection->pending)
				event.bytes = connection->pending;
			connection->pending -= event.bytes;
		}
	}
}

/* Reads what one client has sent, while the core has room, and hands it in; a closed or failed connection is ended. */
static void
unix_read_one(
	struct x11server *server,
	struct unix_connection *connection)
{
	uint8_t chunk[UNIX_READ_CHUNK];
	ssize_t count;
	size_t room;
	int error;

	/* Everything the socket has, while the core has room. */
	count = -1;
	errno = EAGAIN;
	while (connection->pending < UNIX_WINDOW) {
		room = UNIX_WINDOW - connection->pending;
		if (room > sizeof(chunk))
			room = sizeof(chunk);
		count = recv(connection->fd, chunk, room, MSG_DONTWAIT);
		if (count <= 0)
			break;

		/* The bytes, handed in; a core that cannot hold them has closed the client. */
		error = x11server_input(server, connection->client, chunk, (size_t)count);
		if (error != 0) {
			unix_drop(connection);
			return;
		}

		/* Handed in and not yet handled. */
		connection->pending += (size_t)count;
	}

	/* The client closed its end. */
	if (count == 0) {
		unix_hangup(server, connection);
		return;
	}

	/* A failure other than having nothing more to read (or no room) ends the connection. */
	if (count < 0 &&
	    errno != EAGAIN &&
	    errno != EWOULDBLOCK &&
	    errno != EINTR)
		unix_hangup(server, connection);
}

/* Sends one client's waiting bytes as far as its socket takes them; a failed socket ends the connection. */
static void
unix_send_one(
	struct x11server *server,
	struct unix_connection *connection)
{
	const uint8_t *bytes;
	size_t waiting;
	ssize_t sent;

	/* Until nothing waits or the socket is full. */
	for (;;) {
		bytes = x11server_output(server, connection->client, &waiting);
		if (bytes == NULL || waiting == 0U)
			return;
		sent = send(connection->fd, bytes, waiting, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (sent < 0) {
			/* A full socket takes the rest later; an interruption tries again. */
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			if (errno == EINTR)
				continue;

			/* Anything else is a connection that is gone. */
			unix_hangup(server, connection);
			return;
		}

		/* What was sent leaves the core's queue. */
		x11server_written(server, connection->client, (size_t)sent);
	}
}
