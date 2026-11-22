/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The server's life, its clients' connections, and the transport's side
 * of the core.
 *
 * A client's bytes come in through x11server_input and wait in its input
 * buffer until a tick handles every whole request among them; what the
 * server sends a client waits in its output buffer until the transport
 * takes it, so a client that is slow to read never loses a reply or an
 * event (it would wait for a reply that never comes).  The core tells the
 * transport, through x11server_next, of the clients it closes, of the
 * input it has handled (the transport takes no more from a client than
 * the core has handled) and of output waiting.
 */

#include "userland/desktop/xserver/internal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The root window's default size. */
#define SERVER_WIDTH		1280U
#define SERVER_HEIGHT		800U

/* The root window's colour. */
#define SERVER_ROOT_BACKGROUND	0x203040U

/*
 * How long the caller may wait before the next tick while something is
 * still to be shown or a motion is held back, and how long a client may
 * hold a part of a request or unsent bytes before it is reported.
 */
#define SERVER_TICK_MS		20U
#define SERVER_REPORT_MS	2000U

/* The first size of a client's buffers. */
#define SERVER_BUFFER_FIRST	4096U

/* The longest log line. */
#define SERVER_LOG_LINE		512U

/* The bits of a client's number that are its slot. */
#define SERVER_SLOT_BITS	8U
#define SERVER_SLOT_MASK	0xffU

/* How many of a stuck client's held bytes its report shows, and the report's head. */
#define SERVER_HELD_BYTES	32U
#define SERVER_HELD_HEAD	"X11SERVER HELD"

static struct x11_client *server_client(struct x11server *server, uint32_t id, unsigned *index);
static int server_handle(struct x11server *server, unsigned index);
static int server_append(struct x11_client *client, const uint8_t *bytes, size_t length);
static void server_report(struct x11server *server);
static void server_report_history(struct x11server *server, const struct x11_client *client);

/*
 * Starts a server: the core font and the root window, with the backend
 * the options name.  Returns 0, or an errno value.
 */
int
x11server_create(
	const struct x11server_options *options,
	uint32_t now,
	struct x11server **result)
{
	struct x11server *server;
	unsigned index;
	int error;

	/* A backend is required. */
	*result = NULL;
	if (options->backend == NULL)
		return EINVAL;

	/* The server, with every client slot free and no grab. */
	server = calloc(1U, sizeof(*server));
	if (server == NULL)
		return ENOMEM;
	server->grab_owner = -1;
	server->now = now;
	server->report_ms = now;
	server->generation = 1U;

	/* The backend and the log. */
	server->ops = options->backend;
	server->backend = options->backend_context;
	server->log = options->log;
	server->log_context = options->log_context;

	/* Windows are shown through Vulkan when its WSI is named, else through the backend's present. */
	server->vulkan_wsi = options->vulkan_wsi;
	if (server->vulkan_wsi == NULL)
		server->cpu_present = 1;

	/* The root window's size. */
	server->width = options->width;
	server->height = options->height;
	if (server->width == 0U || server->height == 0U) {
		server->width = SERVER_WIDTH;
		server->height = SERVER_HEIGHT;
	}

	/* The first monospaced font file. */
	for (index = 0U; index < options->font_count; index++) {
		if (options->fonts[index].role == X11SERVER_FONT_MONO)
			break;
	}

	/* The core font's face over the caller's bytes; without one, text is not drawn. */
	if (index < options->font_count) {
		error = x11_glyphs_open(&server->glyphs, options->fonts[index].bytes, options->fonts[index].size, options->font_fallback_path);
		if (error != 0)
			x11_log(server, "xserver: font: %s (no text)", strerror(error));
	}

	/* The atoms the server uses itself (selection.c). */
	x11_selection_init(server);

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
 * Ends a server: its clients and their resources (with their surfaces),
 * Vulkan's state and the font.  The transport closes its own connections.
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

	/* Vulkan's state, after every swapchain on it, and the font. */
	x11_vulkan_close(server->vulkan);
	x11_glyphs_close(&server->glyphs);

	/* The server itself. */
	free(server);
}

/*
 * Takes a new client into a free slot and gives the transport its number.
 * Returns 0, or EAGAIN when every slot is in use (the transport refuses
 * the connection).
 */
int
x11server_connect(
	struct x11server *server,
	const struct x11server_auth *auth,
	uint32_t *client)
{
	struct x11_client *slot;
	unsigned index;

	/* Any authorization is accepted for now. */
	(void)auth;

	/* The first slot free and not waiting for the transport to hear of a close. */
	*client = 0U;
	for (index = 0U; index < X11_MAX_CLIENTS; index++) {
		if (server->clients[index].id == 0U && server->clients[index].closed == 0U)
			break;
	}

	/* No room: the connection is refused. */
	if (index == X11_MAX_CLIENTS)
		return EAGAIN;

	/*
	 * The client's number: its slot below a generation that is never 0,
	 * so that the number of a client gone is not taken for a later one's.
	 */
	slot = &server->clients[index];
	memset(slot, 0, sizeof(*slot));
	slot->id = (server->generation << SERVER_SLOT_BITS) | index;
	server->generation++;
	if ((server->generation << SERVER_SLOT_BITS) == 0U)
		server->generation = 1U;

	/* Its resource ids start at its own base. */
	slot->base = (index + 1U) << 22;
	slot->last_ms = server->now;

	/* Succeeded: the transport hands the client's bytes in by its number. */
	*client = slot->id;
	return 0;
}

/*
 * Adds bytes a client sent to its input; the next tick handles them.
 * Returns 0, ENOENT for a client that is not connected, or EMSGSIZE or
 * ENOMEM when the core cannot hold them (the client is closed, and the
 * transport hears of it).
 */
int
x11server_input(
	struct x11server *server,
	uint32_t client,
	const uint8_t *bytes,
	size_t size)
{
	struct x11_client *connection;
	unsigned index;
	uint8_t *grown;
	size_t capacity;

	/* The client. */
	connection = server_client(server, client, &index);
	if (connection == NULL)
		return ENOENT;

	/* A client that sends more than the core holds breaks the protocol. */
	if (size > X11_INPUT_CAP - connection->used) {
		x11_client_close(server, index);
		return EMSGSIZE;
	}

	/* The buffer, doubled until the bytes fit. */
	if (connection->used + size > connection->capacity) {
		capacity = connection->capacity;
		if (capacity == 0U)
			capacity = SERVER_BUFFER_FIRST;
		while (capacity < connection->used + size)
			capacity *= 2U;
		grown = realloc(connection->input, capacity);
		if (grown == NULL) {
			x11_client_close(server, index);
			return ENOMEM;
		}

		/* The grown buffer. */
		connection->input = grown;
		connection->capacity = capacity;
	}

	/* The bytes behind those already there. */
	memcpy(connection->input + connection->used, bytes, size);
	connection->used += size;

	/* Succeeded: the next tick handles them. */
	return 0;
}

/*
 * Returns the bytes waiting to be sent to a client (and their count in
 * *size), or NULL with 0 when there are none or the client is gone.
 */
const uint8_t *
x11server_output(
	struct x11server *server,
	uint32_t client,
	size_t *size)
{
	struct x11_client *connection;
	unsigned index;

	/* Nothing for a client that is not connected. */
	*size = 0U;
	connection = server_client(server, client, &index);
	if (connection == NULL)
		return NULL;

	/* Nothing waiting. */
	if (connection->output_used == 0U)
		return NULL;

	/* Succeeded: the bytes at the head of the queue. */
	*size = connection->output_used;
	return connection->output;
}

/*
 * Takes the bytes the transport has sent off the head of a client's
 * output.
 */
void
x11server_written(
	struct x11server *server,
	uint32_t client,
	size_t size)
{
	struct x11_client *connection;
	unsigned index;

	/* Nothing for a client that is not connected. */
	connection = server_client(server, client, &index);
	if (connection == NULL)
		return;

	/* No more than is waiting. */
	if (size > connection->output_used)
		size = connection->output_used;

	/* What was sent leaves the queue; the transport hears again of what comes next. */
	memmove(connection->output, connection->output + size, connection->output_used - size);
	connection->output_used -= size;
	connection->output_told = 0;
}

/*
 * Ends a client whose connection the transport lost or closed: its
 * resources go at once, and the transport needs no word back.
 */
void
x11server_hangup(
	struct x11server *server,
	uint32_t client)
{
	struct x11_client *connection;
	unsigned index;

	/* Nothing for a client that is not connected. */
	connection = server_client(server, client, &index);
	if (connection == NULL)
		return;

	/* The client goes; the transport, which closed it, is not told. */
	x11_client_close(server, index);
	server->clients[index].closed = 0U;
}

/*
 * Gives the transport the next thing it must hear: a client the core
 * closed, input handled, output waiting.  Returns 1 with the event, or 0
 * when there is nothing more.
 */
int
x11server_next(
	struct x11server *server,
	struct x11server_event *event)
{
	struct x11_client *connection;
	unsigned index;

	/* Nothing yet. */
	memset(event, 0, sizeof(*event));

	/* A client the core closed comes first: its connection goes, and the slot is free again. */
	for (index = 0U; index < X11_MAX_CLIENTS; index++) {
		connection = &server->clients[index];
		if (connection->closed == 0U)
			continue;
		event->type = X11SERVER_EVENT_CLOSED;
		event->client = connection->closed;
		connection->closed = 0U;
		return 1;
	}

	/* Input handled: the transport may take as much more. */
	for (index = 0U; index < X11_MAX_CLIENTS; index++) {
		connection = &server->clients[index];
		if (connection->id == 0U || connection->consumed == 0U)
			continue;
		event->type = X11SERVER_EVENT_CONSUMED;
		event->client = connection->id;
		event->bytes = connection->consumed;
		connection->consumed = 0U;
		return 1;
	}

	/* Output waiting that the transport has not heard of. */
	for (index = 0U; index < X11_MAX_CLIENTS; index++) {
		connection = &server->clients[index];
		if (connection->id == 0U || connection->output_used == 0U)
			continue;
		if (connection->output_told)
			continue;
		event->type = X11SERVER_EVENT_OUTPUT;
		event->client = connection->id;
		event->bytes = connection->output_used;
		connection->output_told = 1;
		return 1;
	}

	/* Nothing more. */
	return 0;
}

/*
 * Does the work due: the windows the backend asked to close, a motion
 * held back, each client's whole requests, the windows' new pixels shown,
 * the clients that stopped taking their output closed, and the report of
 * stuck clients.  *wake is when the caller is to call again.  Returns 0.
 */
int
x11server_tick(
	struct x11server *server,
	uint32_t now,
	uint32_t *wake)
{
	struct x11_client *client;
	unsigned index;
	int more;
	int left;

	/* The server's time never goes back. */
	if ((int32_t)(now - server->now) > 0)
		server->now = now;

	/* The windows the backend asked to close, and a motion held back. */
	x11_rootless_closing(server);
	x11_rootless_flush_motion(server);

	/* Each client's whole requests, up to a tick's share. */
	more = 0;
	for (index = 0U; index < X11_MAX_CLIENTS; index++) {
		if (server->clients[index].id == 0U)
			continue;
		left = server_handle(server, index);
		if (left)
			more = 1;
	}

	/* What changed is shown. */
	x11_rootless_present(server);

	/* A client that stopped taking its output is closed. */
	for (index = 0U; index < X11_MAX_CLIENTS; index++) {
		client = &server->clients[index];
		if (client->id != 0U && client->broken)
			x11_client_close(server, index);
	}

	/* A client holding a part of a request, or bytes it does not take, is reported. */
	server_report(server);

	/* At once while requests wait, soon while something is to be shown or held back, else at the next report. */
	if (more) {
		*wake = server->now;
	} else if (server->dirty || server->motion_pending) {
		*wake = server->now + SERVER_TICK_MS;
	} else {
		*wake = server->now + SERVER_REPORT_MS;
	}

	/* Succeeded: the work due is done. */
	return 0;
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
	if (server->clients[owner].id == 0U)
		return NULL;

	/* Succeeded: the client. */
	return &server->clients[owner];
}

/*
 * Queues bytes for a client; the transport takes them.  A client that
 * stops reading past X11_OUTPUT_CAP is broken (closed at the end of the
 * tick).
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
	if (client->id == 0U || client->broken)
		return;

	/* The bytes behind those already waiting. */
	error = server_append(client, bytes, length);
	if (error != 0)
		client->broken = 1;
}

/*
 * Closes a client and releases every resource it owns; the transport
 * hears of it through x11server_next.
 */
void
x11_client_close(
	struct x11server *server,
	unsigned index)
{
	struct x11_client *client;
	uint32_t id;

	/* A free slot has nothing to close. */
	client = &server->clients[index];
	if (client->id == 0U)
		return;

	/* The client is gone from now on. */
	id = client->id;
	client->id = 0U;

	/* Its windows (with their surfaces), pixmaps, graphics contexts and fonts. */
	x11_resources_release(server, index);

	/* A grab of its own goes. */
	if (server->grab_owner == (int)index) {
		server->grab_owner = -1;
		server->grab_window = 0U;
	}

	/* A motion held back for it is dropped. */
	if (server->motion_pending && server->motion_client == index)
		server->motion_pending = 0;

	/* Its buffers; the slot waits until the transport has heard of the close. */
	free(client->input);
	free(client->output);
	memset(client, 0, sizeof(*client));
	client->closed = id;
}

/*
 * Writes a log line (printf's format) to the log the options named.
 */
void
x11_log(
	struct x11server *server,
	const char *format,
	...)
{
	char line[SERVER_LOG_LINE];
	va_list arguments;

	/* No log, no line. */
	if (server->log == NULL)
		return;

	/* The line, cut to its room. */
	va_start(arguments, format);
	(void)vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);

	/* Said. */
	server->log(server->log_context, line);
}

/* Returns the connected client a number names (and its slot), or NULL. */
static struct x11_client *
server_client(
	struct x11server *server,
	uint32_t id,
	unsigned *index)
{
	unsigned slot;

	/* The slot the number names. */
	slot = id & SERVER_SLOT_MASK;
	if (id == 0U || slot >= X11_MAX_CLIENTS)
		return NULL;

	/* A slot another client has taken since. */
	if (server->clients[slot].id != id)
		return NULL;

	/* Succeeded: the client. */
	*index = slot;
	return &server->clients[slot];
}

/*
 * Answers a client's setup, then each whole request it has sent, in
 * order, up to a tick's share.  Returns nonzero when whole requests may
 * still be waiting.
 */
static int
server_handle(
	struct x11server *server,
	unsigned index)
{
	struct x11_client *client;
	uint16_t name_length;
	uint16_t data_length;
	uint16_t units;
	size_t handled;
	size_t need;
	int error;

	/* Each whole message at the front of the buffer. */
	client = &server->clients[index];
	handled = 0U;
	for (;;) {
		/* The rest waits for the next tick once the tick's share is used. */
		if (handled >= X11_TICK_BYTES)
			break;

		/* A request's length is in its header; the setup's in its fixed part. */
		if (!client->setup) {
			/* The fixed part, and the byte order it names. */
			if (client->used < 12U)
				return 0;
			if (client->input[0] != 'l' && client->input[0] != 'B') {
				x11_client_close(server, index);
				return 0;
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
				return 0;

			/* The setup is answered (any authorization is accepted). */
			error = x11_setup_reply(server, client);
			if (error != 0) {
				x11_client_close(server, index);
				return 0;
			}

			/* Requests follow. */
			client->setup = 1;
		} else {
			/* The header and the length it gives (in words; zero is not a length). */
			if (client->used < 4U)
				return 0;
			units = x11_read16(client->input + 2, client->order);
			if (units == 0U) {
				x11_client_close(server, index);
				return 0;
			}

			/* The request's length in bytes. */
			need = (size_t)units * 4U;
			if (client->used < need)
				return 0;

			/* The request; a request that ended its own client leaves nothing more to do. */
			x11_request(server, index, client->input, need);
			if (client->id == 0U)
				return 0;

			/* The request, remembered for the stuck-client report. */
			client->last_opcode = client->input[0];
			client->last_ms = server->now;
			client->history_opcode[client->history_next] = client->input[0];
			client->history_length[client->history_next] = (uint32_t)need;
			client->history_sequence[client->history_next] = client->sequence;
			client->history_next = (client->history_next + 1U) % X11_HISTORY;
		}

		/* The message is done: the rest moves to the front, and the transport may hand in as much more. */
		memmove(client->input, client->input + need, client->used - need);
		client->used -= need;
		client->consumed += need;
		handled += need;
	}

	/* Whole requests may still be waiting. */
	return 1;
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
			capacity = SERVER_BUFFER_FIRST;
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
	uint32_t now;
	size_t need;
	unsigned opcode;
	unsigned index;

	/* Not more often than the report's interval. */
	now = server->now;
	if (now - server->report_ms < SERVER_REPORT_MS)
		return;
	server->report_ms = now;

	/* Each client quiet for that long with something held. */
	for (index = 0U; index < X11_MAX_CLIENTS; index++) {
		client = &server->clients[index];
		if (client->id == 0U || !client->setup)
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
		x11_log(server, "X11SERVER STUCK client=%u held=%lu need=%lu opcode=%u last=%u unsent=%lu quiet_ms=%lu sequence=%u", index,
			(unsigned long)client->used, (unsigned long)need, opcode, (unsigned)client->last_opcode, (unsigned long)client->output_used,
			(unsigned long)(now - client->last_ms), (unsigned)client->sequence);

		/* The requests before it, oldest first, and the head of what is held. */
		server_report_history(server, client);
	}
}

/* Says a stuck client's last requests (opcode, length, sequence number), oldest first, and the first bytes it holds. */
static void
server_report_history(
	struct x11server *server,
	const struct x11_client *client)
{
	char held[sizeof(SERVER_HELD_HEAD) + SERVER_HELD_BYTES * 3U];
	size_t used;
	unsigned count;
	unsigned slot;
	size_t byte;

	/* The ring, from the oldest entry. */
	for (count = 0U; count < X11_HISTORY; count++) {
		slot = (client->history_next + count) % X11_HISTORY;
		if (client->history_length[slot] == 0U)
			continue;
		x11_log(server, "X11SERVER HISTORY opcode=%u length=%u sequence=%u", (unsigned)client->history_opcode[slot], (unsigned)client->history_length[slot],
			(unsigned)client->history_sequence[slot]);
	}

	/* The held bytes' head, three characters a byte after the line's head. */
	memcpy(held, SERVER_HELD_HEAD, sizeof(SERVER_HELD_HEAD));
	used = sizeof(SERVER_HELD_HEAD) - 1U;
	for (byte = 0U; byte < client->used && byte < SERVER_HELD_BYTES; byte++) {
		(void)snprintf(held + used, sizeof(held) - used, " %02x", (unsigned)client->input[byte]);
		used += 3U;
	}

	/* Said. */
	x11_log(server, "%s", held);
}
