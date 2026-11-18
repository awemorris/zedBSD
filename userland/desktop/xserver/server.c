/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The server's life and its clients' connections.
 *
 * Every socket is non-blocking.  What a client sends is gathered until a
 * whole request is there; what the server sends a client is queued and
 * written as the socket takes it, so a client that is slow to read never
 * loses a reply or an event (it would wait for a reply that never comes).
 * The caller must ignore SIGPIPE: a client that goes away mid-write is
 * found by the next read.
 */

#include "userland/desktop/xserver/internal.h"
#include "userland/desktop/paths.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* The defaults of the options. */
#define SERVER_SOCKET_PATH	"/tmp/.X11-unix/X0"
#define SERVER_SOCKET_DIR	"/tmp/.X11-unix"
#define SERVER_FONT_PATH	KEILAND_DATADIR "/fonts/keiland-mono.ttf"
#define SERVER_WIDTH		1280U
#define SERVER_HEIGHT		800U

/* The root window's colour. */
#define SERVER_ROOT_BACKGROUND	0x203040U

/* How long poll may wait, and how long a client may hold a part of a request or unsent bytes before it is reported. */
#define SERVER_POLL_MS		20
#define SERVER_REPORT_MS	2000U

/* The most bytes read from a client at once. */
#define SERVER_READ_CHUNK	4096U

static int server_listen(struct x11server *server, const char *path);
static void server_accept(struct x11server *server);
static void server_read(struct x11server *server, unsigned index);
static void server_handle(struct x11server *server, unsigned index);
static int server_append(struct x11_client *client, const uint8_t *bytes, size_t length);
static void server_flush(struct x11_client *client);
static void server_report(struct x11server *server);
static void server_report_history(const struct x11_client *client);

/*
 * Starts a server: the core font, the connection to the desktop, the
 * listening socket and the root window.  Returns 0, or an errno value.
 */
int
x11server_create(
	const struct x11server_options *options,
	struct x11server **result)
{
	struct x11server *server;
	const char *path;
	const char *font;
	unsigned index;
	int error;

	/* The server, with every client slot free and no grab. */
	*result = NULL;
	server = calloc(1U, sizeof(*server));
	if (server == NULL)
		return ENOMEM;

	/* Nothing is open yet. */
	server->listener = -1;
	server->grab_owner = -1;
	for (index = 0U; index < X11_MAX_CLIENTS; index++)
		server->clients[index].fd = -1;

	/* The root window's size. */
	server->width = options->width;
	server->height = options->height;
	if (server->width == 0U || server->height == 0U) {
		server->width = SERVER_WIDTH;
		server->height = SERVER_HEIGHT;
	}

	/* The core font; without one, text is not drawn. */
	font = options->font_path;
	if (font == NULL)
		font = SERVER_FONT_PATH;
	error = x11_glyphs_open(&server->glyphs, font);
	if (error != 0)
		fprintf(stderr, "xserver: %s: %s (no text)\n", font, strerror(error));

	/* The atoms the server uses itself (selection.c). */
	x11_selection_init(server);

	/* The connection to the desktop, which shows the windows and gives the input. */
	error = x11_wayland_open(&server->wayland, options->wayland_display, options->shm, &x11_rootless_callbacks, server);
	if (error != 0) {
		x11server_destroy(server);
		return error;
	}

	/* The socket the clients connect to. */
	path = options->socket_path;
	if (path == NULL)
		path = SERVER_SOCKET_PATH;
	error = server_listen(server, path);
	if (error != 0) {
		x11server_destroy(server);
		return error;
	}

	/* The root window: the server's, mapped, with no pixels (nothing shows it). */
	server->windows[0].id = X11_ROOT_XID;
	server->windows[0].owner = X11_NO_CLIENT;
	server->windows[0].background = SERVER_ROOT_BACKGROUND;
	server->windows[0].width = (uint16_t)server->width;
	server->windows[0].height = (uint16_t)server->height;
	server->windows[0].mapped = 1;
	server->window_count = 1U;

	/* The focus on the root window, the pointer in its middle. */
	server->focus = X11_ROOT_XID;
	server->pointer_x = (int)server->width / 2;
	server->pointer_y = (int)server->height / 2;

	/* Succeeded: the server waits for clients. */
	*result = server;
	return 0;
}

/*
 * Fills the descriptors the caller waits for (the listening socket, the
 * desktop's connection, and each client, writable too while it has bytes
 * to take) and how long it may wait.  Returns the count filled.
 */
unsigned
x11server_pollfds(
	struct x11server *server,
	struct pollfd *descriptors,
	unsigned capacity,
	int *timeout_ms)
{
	struct x11_client *client;
	unsigned count;
	unsigned index;

	/* The listening socket and the desktop's connection, when there is room. */
	*timeout_ms = SERVER_POLL_MS;
	if (capacity < 2U)
		return 0U;
	descriptors[0].fd = server->listener;
	descriptors[0].events = POLLIN;
	descriptors[0].revents = 0;
	descriptors[1].fd = x11_wayland_fd(server->wayland);
	descriptors[1].events = POLLIN;
	descriptors[1].revents = 0;
	count = 2U;

	/* Each connected client, remembering its slot. */
	for (index = 0U; index < X11_MAX_CLIENTS && count < capacity; index++) {
		client = &server->clients[index];
		if (client->fd < 0)
			continue;

		/* Its readable side, and its writable side while it has bytes queued. */
		descriptors[count].fd = client->fd;
		descriptors[count].events = POLLIN;
		if (client->output_used != 0U)
			descriptors[count].events |= POLLOUT;
		descriptors[count].revents = 0;
		server->poll_clients[count] = index;
		count++;
	}

	/* Succeeded: the descriptors to wait for. */
	return count;
}

/*
 * Handles what the wait found: new clients, the desktop's events, the
 * clients' requests and the sockets that can take more; then shows what
 * changed and sends what is queued.
 */
void
x11server_dispatch(
	struct x11server *server,
	const struct pollfd *descriptors,
	unsigned count)
{
	struct x11_client *client;
	unsigned index;
	int readable;
	int failed;

	/* New clients. */
	if (count >= 1U && (descriptors[0].revents & POLLIN) != 0)
		server_accept(server);

	/* The desktop's events (those already read, too); a lost connection stops the server. */
	readable = 0;
	if (count >= 2U && (descriptors[1].revents & POLLIN) != 0)
		readable = 1;
	failed = x11_wayland_dispatch(server->wayland, readable);
	if (failed != 0)
		server->stopped = 1;

	/* The windows the desktop asked to close, and a motion held back. */
	x11_rootless_closing(server);
	x11_rootless_flush_motion(server);

	/*
	 * Every client is read, not only the readable ones: a read that finds
	 * nothing costs little, and no request is left waiting for a wakeup
	 * that may not come.
	 */
	for (index = 0U; index < X11_MAX_CLIENTS; index++) {
		if (server->clients[index].fd >= 0)
			server_read(server, index);
	}

	/* What changed is shown. */
	x11_rootless_present(server);

	/* Each client's queue is sent as far as its socket takes it; a broken connection is closed. */
	for (index = 0U; index < X11_MAX_CLIENTS; index++) {
		client = &server->clients[index];
		if (client->fd < 0)
			continue;
		server_flush(client);
		if (client->broken)
			x11_client_close(server, index);
	}

	/* A client holding a part of a request, or bytes it does not take, is reported. */
	server_report(server);
}

/*
 * Reports whether the server has stopped (its desktop's connection is
 * lost); the caller then destroys it.
 */
int
x11server_stopped(
	const struct x11server *server)
{
	/* Succeeded: the flag. */
	return server->stopped;
}

/*
 * Ends a server: its clients and their resources, the socket, the
 * desktop's connection and the font.
 */
void
x11server_destroy(
	struct x11server *server)
{
	unsigned index;

	/* Nothing was made. */
	if (server == NULL)
		return;

	/* Every client, with its windows, pixmaps, graphics contexts and fonts. */
	for (index = 0U; index < X11_MAX_CLIENTS; index++)
		x11_client_close(server, index);

	/* The listening socket and its name. */
	if (server->listener >= 0) {
		(void)close(server->listener);
		(void)unlink(server->socket_path);
	}

	/* The desktop's connection and the font. */
	if (server->wayland != NULL)
		x11_wayland_close(server->wayland);
	x11_glyphs_close(&server->glyphs);

	/* The server itself. */
	free(server);
}

/*
 * Returns the client of a resource's owner, or NULL when the owner is not
 * a connected client.
 */
struct x11_client *
x11_client_of(
	struct x11server *server,
	unsigned owner)
{
	/* The root window's owner and a free slot have no client. */
	if (owner >= X11_MAX_CLIENTS)
		return NULL;
	if (server->clients[owner].fd < 0)
		return NULL;

	/* Succeeded: the client. */
	return &server->clients[owner];
}

/*
 * Queues bytes for a client and sends as many as its socket takes now.
 * A client that stops reading past X11_OUTPUT_CAP is broken (closed after
 * the pass).
 */
void
x11_client_send(
	struct x11server *server,
	struct x11_client *client,
	const void *bytes,
	size_t length)
{
	int error;

	/* A closed or broken connection takes nothing. */
	(void)server;
	if (client->fd < 0 || client->broken)
		return;

	/* The bytes behind those already waiting. */
	error = server_append(client, bytes, length);
	if (error != 0) {
		client->broken = 1;
		return;
	}

	/* As many as the socket takes now. */
	server_flush(client);
}

/*
 * Closes a client's connection and releases every resource it owns.
 */
void
x11_client_close(
	struct x11server *server,
	unsigned index)
{
	struct x11_client *client;

	/* A free slot has nothing to close. */
	client = &server->clients[index];
	if (client->fd < 0)
		return;

	/* The connection. */
	(void)close(client->fd);
	client->fd = -1;

	/* Its windows (with their desktop windows), pixmaps, graphics contexts and fonts. */
	x11_resources_release(server, index);

	/* A grab or a held-back motion of its own goes. */
	if (server->grab_owner == (int)index) {
		server->grab_owner = -1;
		server->grab_window = 0U;
	}

	/* A motion held back for it is dropped. */
	if (server->motion_pending && server->motion_client == index)
		server->motion_pending = 0;

	/* Its buffers, and the slot is free again. */
	free(client->input);
	free(client->output);
	memset(client, 0, sizeof(*client));
	client->fd = -1;
}

/*
 * Returns the monotonic time in milliseconds.
 */
uint64_t
x11_now_ms(void)
{
	struct timespec now;

	/* The clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: seconds and nanoseconds as milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Makes the listening socket at a path, and remembers the path. */
static int
server_listen(
	struct x11server *server,
	const char *path)
{
	struct sockaddr_un address;
	size_t length;
	int error;

	/* The path must fit an address. */
	length = strlen(path);
	if (length >= sizeof(address.sun_path) || length >= sizeof(server->socket_path))
		return ENAMETOOLONG;
	memcpy(server->socket_path, path, length + 1U);

	/* The default directory, and no stale socket in the way. */
	(void)mkdir(SERVER_SOCKET_DIR, 0777);
	(void)unlink(path);

	/* The socket. */
	server->listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (server->listener < 0)
		return errno;

	/* Bound to the path. */
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	memcpy(address.sun_path, path, length + 1U);
	error = bind(server->listener, (struct sockaddr *)&address, sizeof(address));
	if (error != 0)
		return errno;

	/* Listening. */
	error = listen(server->listener, (int)X11_MAX_CLIENTS);
	if (error != 0)
		return errno;

	/* Succeeded: clients can connect. */
	return 0;
}

/* Accepts every waiting connection into a free slot; one with no slot is closed. */
static void
server_accept(
	struct x11server *server)
{
	struct x11_client *client;
	unsigned index;
	int descriptor;

	/* Each waiting connection. */
	for (;;) {
		descriptor = accept4(server->listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
		if (descriptor < 0)
			return;

		/* The first free slot. */
		for (index = 0U; index < X11_MAX_CLIENTS; index++) {
			if (server->clients[index].fd < 0)
				break;
		}

		/* No room: the connection is refused. */
		if (index == X11_MAX_CLIENTS) {
			(void)close(descriptor);
			continue;
		}

		/* The client, whose resource ids start at its own base. */
		client = &server->clients[index];
		memset(client, 0, sizeof(*client));
		client->fd = descriptor;
		client->base = (index + 1U) << 22;
		client->last_ms = x11_now_ms();
	}
}

/* Reads what a client has sent and handles every whole request; a closed or failed connection is closed. */
static void
server_read(
	struct x11server *server,
	unsigned index)
{
	uint8_t chunk[SERVER_READ_CHUNK];
	struct x11_client *client;
	uint8_t *grown;
	size_t capacity;
	ssize_t count;

	/* Everything the socket has. */
	client = &server->clients[index];
	for (;;) {
		count = recv(client->fd, chunk, sizeof(chunk), MSG_DONTWAIT);
		if (count <= 0)
			break;

		/* A client that sends more than a request can be is refused. */
		if (client->used + (size_t)count > X11_INPUT_CAP) {
			x11_client_close(server, index);
			return;
		}

		/* The buffer, doubled until the bytes fit. */
		if (client->used + (size_t)count > client->capacity) {
			capacity = client->capacity;
			if (capacity == 0U)
				capacity = SERVER_READ_CHUNK;
			while (capacity < client->used + (size_t)count)
				capacity *= 2U;
			grown = realloc(client->input, capacity);
			if (grown == NULL) {
				x11_client_close(server, index);
				return;
			}

			/* The grown buffer. */
			client->input = grown;
			client->capacity = capacity;
		}

		/* The bytes behind those already there. */
		memcpy(client->input + client->used, chunk, (size_t)count);
		client->used += (size_t)count;
	}

	/* The client closed its end. */
	if (count == 0) {
		x11_client_close(server, index);
		return;
	}

	/* A failure other than having nothing more to read ends the connection. */
	if (errno != EAGAIN &&
	    errno != EWOULDBLOCK &&
	    errno != EINTR) {
		x11_client_close(server, index);
		return;
	}

	/* Every whole request. */
	server_handle(server, index);
}

/* Answers a client's setup, then each whole request it has sent, in order. */
static void
server_handle(
	struct x11server *server,
	unsigned index)
{
	struct x11_client *client;
	uint16_t name_length;
	uint16_t data_length;
	uint16_t units;
	size_t need;
	int error;

	/* Each whole message at the front of the buffer. */
	client = &server->clients[index];
	for (;;) {
		/* A request's length is in its header; the setup's in its fixed part. */
		if (!client->setup) {
			/* The fixed part, and the byte order it names. */
			if (client->used < 12U)
				return;
			if (client->input[0] != 'l' && client->input[0] != 'B') {
				x11_client_close(server, index);
				return;
			}

			/* The byte order it names. */
			client->order = 0;
			if (client->input[0] == 'B')
				client->order = 1;

			/* The authorization's name and data, padded to words. */
			name_length = x11_read16(client->input + 6, client->order);
			data_length = x11_read16(client->input + 8, client->order);
			need = 12U + (((size_t)name_length + 3U) & ~(size_t)3U) + (((size_t)data_length + 3U) & ~(size_t)3U);
			if (client->used < need)
				return;

			/* The setup is answered (any authorization is accepted). */
			error = x11_setup_reply(server, client);
			if (error != 0) {
				x11_client_close(server, index);
				return;
			}

			/* Requests follow. */
			client->setup = 1;
		} else {
			/* The header and the length it gives (in words; zero is not a length). */
			if (client->used < 4U)
				return;
			units = x11_read16(client->input + 2, client->order);
			if (units == 0U) {
				x11_client_close(server, index);
				return;
			}

			/* The request's length in bytes. */
			need = (size_t)units * 4U;
			if (client->used < need)
				return;

			/* The request, remembered for the stuck-client report. */
			x11_request(server, index, client->input, need);
			client->last_opcode = client->input[0];
			client->last_ms = x11_now_ms();
			client->history_opcode[client->history_next] = client->input[0];
			client->history_length[client->history_next] = (uint32_t)need;
			client->history_sequence[client->history_next] = client->sequence;
			client->history_next = (client->history_next + 1U) % X11_HISTORY;
		}

		/* The message is done: the rest moves to the front. */
		memmove(client->input, client->input + need, client->used - need);
		client->used -= need;
	}
}

/* Adds bytes to a client's queue, growing it; ENOBUFS past X11_OUTPUT_CAP, ENOMEM. */
static int
server_append(
	struct x11_client *client,
	const uint8_t *bytes,
	size_t length)
{
	uint8_t *grown;
	size_t capacity;

	/* A client that has stopped reading is not queued without end. */
	if (client->output_used + length > X11_OUTPUT_CAP)
		return ENOBUFS;

	/* The queue, doubled until the bytes fit. */
	if (client->output_used + length > client->output_capacity) {
		capacity = client->output_capacity;
		if (capacity == 0U)
			capacity = SERVER_READ_CHUNK;
		while (capacity < client->output_used + length)
			capacity *= 2U;
		grown = realloc(client->output, capacity);
		if (grown == NULL)
			return ENOMEM;
		client->output = grown;
		client->output_capacity = capacity;
	}

	/* The bytes behind those waiting. */
	memcpy(client->output + client->output_used, bytes, length);
	client->output_used += length;

	/* Succeeded: the bytes wait their turn. */
	return 0;
}

/* Sends a client's queue as far as its socket takes it; a failed socket breaks the client. */
static void
server_flush(
	struct x11_client *client)
{
	ssize_t sent;

	/* Until the queue is empty or the socket is full. */
	while (client->output_used != 0U && !client->broken) {
		sent = send(client->fd, client->output, client->output_used, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (sent < 0) {
			/* A full socket takes the rest later; an interruption tries again. */
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			if (errno == EINTR)
				continue;

			/* Anything else is a connection that is gone. */
			client->broken = 1;
			return;
		}

		/* What was sent leaves the queue. */
		memmove(client->output, client->output + sent, client->output_used - (size_t)sent);
		client->output_used -= (size_t)sent;
	}
}

/*
 * Reports, now and then, each client that has held a part of a request or
 * bytes it does not take for a while: a client waiting for a reply the
 * server never sends looks like one of these.
 */
static void
server_report(
	struct x11server *server)
{
	struct x11_client *client;
	uint64_t now;
	size_t need;
	unsigned opcode;
	unsigned index;

	/* Not more often than the report's interval. */
	now = x11_now_ms();
	if (now - server->report_ms < SERVER_REPORT_MS)
		return;
	server->report_ms = now;

	/* Each client quiet for that long with something held. */
	for (index = 0U; index < X11_MAX_CLIENTS; index++) {
		client = &server->clients[index];
		if (client->fd < 0 || !client->setup)
			continue;
		if (client->used == 0U && client->output_used == 0U)
			continue;
		if (now - client->last_ms < SERVER_REPORT_MS)
			continue;

		/* The first request held: its opcode and the length it needs. */
		need = 0U;
		opcode = 0U;
		if (client->used > 0U)
			opcode = client->input[0];
		if (client->used >= 4U)
			need = (size_t)x11_read16(client->input + 2, client->order) * 4U;

		/* Said. */
		fprintf(stderr, "X11SERVER STUCK client=%u held=%zu need=%zu opcode=%u last=%u unsent=%zu quiet_ms=%llu sequence=%u\n", index, client->used, need,
			opcode, (unsigned)client->last_opcode, client->output_used, (unsigned long long)(now - client->last_ms), (unsigned)client->sequence);

		/* The requests before it, oldest first, and the head of what is held. */
		server_report_history(client);
	}
}

/* Says a stuck client's last requests (opcode, length, sequence number), oldest first, and the first bytes it holds. */
static void
server_report_history(
	const struct x11_client *client)
{
	unsigned count;
	unsigned slot;
	size_t byte;

	/* The ring, from the oldest entry. */
	for (count = 0U; count < X11_HISTORY; count++) {
		slot = (client->history_next + count) % X11_HISTORY;
		if (client->history_length[slot] == 0U)
			continue;
		fprintf(stderr, "X11SERVER HISTORY opcode=%u length=%u sequence=%u\n", (unsigned)client->history_opcode[slot], (unsigned)client->history_length[slot],
			(unsigned)client->history_sequence[slot]);
	}

	/* The held bytes' head. */
	fprintf(stderr, "X11SERVER HELD");
	for (byte = 0U; byte < client->used && byte < 32U; byte++)
		fprintf(stderr, " %02x", (unsigned)client->input[byte]);
	fprintf(stderr, "\n");
}
