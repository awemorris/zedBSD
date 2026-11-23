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

/* Why a setup is refused. */
#define SERVER_REFUSED_AUTH	"Invalid MIT-MAGIC-COOKIE-1 key"

/* How many of a stuck client's held bytes its report shows, and the report's head. */
#define SERVER_HELD_BYTES	32U
#define SERVER_HELD_HEAD	"X11SERVER HELD"

static struct x11_client *server_client(struct x11server *server, uint32_t id, unsigned *index);
static int server_handle(struct x11server *server, unsigned index);
static int server_setup(struct x11server *server, unsigned index, size_t *need);
static int server_authorized(const struct x11_client *client, const uint8_t *name, size_t name_length, const uint8_t *data, size_t data_length);
static void server_refuse(struct x11server *server, struct x11_client *client, const char *reason);
static int server_append(struct x11_client *client, const uint8_t *bytes, size_t length);
static void server_report(struct x11server *server);
static void server_report_history(struct x11server *server, const struct x11_client *client);
static void server_vulkan_lost(struct x11server *server);

/*
 * Starts a server: the core fonts' faces and the root window, with the backend
 * the options name.  Returns 0, or an errno value.
 */
int
x11server_create(
	const struct x11server_options *options,
	uint32_t now,
	struct x11server **result)
{
	struct x11server *server;
	int shows;
	int error;

	/* A backend is required. */
	*result = NULL;
	if (options->backend == NULL)
		return EINVAL;

	/* The server, with no grab of the server. */
	server = calloc(1U, sizeof(*server));
	if (server == NULL)
		return ENOMEM;
	server->grabbed = X11_NO_CLIENT;
	server->now = now;
	server->report_ms = now;
	server->generation = 1U;

	/* The backend and the log. */
	server->ops = options->backend;
	server->backend = options->backend_context;
	server->log = options->log;
	server->log_context = options->log_context;

	/* Vulkan when the program gave its loader: windows and pixmaps drawn on the GPU, shown through swapchains when it can. */
	server->vulkan_wsi = options->vulkan_wsi;
	if (options->vulkan_loader != NULL)
		server->vulkan = x11_vulkan_open(server, options->vulkan_loader, options->vulkan_wsi);

	/* Else, or without the WSI, windows are shown through the backend's present. */
	shows = x11_vulkan_shows(server->vulkan);
	if (!shows)
		server->cpu_present = 1;

	/* The client slots, every one free. */
	server->clients_max = options->clients_max;
	if (server->clients_max == 0U)
		server->clients_max = X11_CLIENTS_DEFAULT;
	if (server->clients_max > X11_CLIENTS_LIMIT)
		server->clients_max = X11_CLIENTS_LIMIT;
	server->clients = calloc(server->clients_max, sizeof(*server->clients));
	if (server->clients == NULL) {
		free(server);
		return ENOMEM;
	}

	/* The memory a client and the server may hold. */
	server->client_memory_max = options->client_memory_max;
	if (server->client_memory_max == 0U)
		server->client_memory_max = X11_CLIENT_MEMORY_DEFAULT;
	server->server_memory_max = options->server_memory_max;
	if (server->server_memory_max == 0U)
		server->server_memory_max = X11_SERVER_MEMORY_DEFAULT;

	/* The root window's size. */
	server->width = options->width;
	server->height = options->height;
	if (server->width == 0U || server->height == 0U) {
		server->width = SERVER_WIDTH;
		server->height = SERVER_HEIGHT;
	}

	/* The one monitor, the whole root window, until the backend tells the system's. */
	x11_randr_init(server);

	/* Rootless, or the whole root window one surface. */
	server->mode = X11SERVER_ROOTLESS;
	if (options->mode == X11SERVER_ROOTED)
		server->mode = X11SERVER_ROOTED;

	/* The core fonts' faces over the caller's bytes; without any, text is not drawn. */
	error = x11_faces_open(&server->faces, options->fonts, options->font_count, options->font_fallback_path);
	if (error != 0)
		x11_log(server, "xserver: font: %s", strerror(error));

	/* The core's atoms, and those the server uses itself (selection.c). */
	error = x11_atoms_init(server);
	if (error != 0) {
		x11server_destroy(server);
		return error;
	}
	x11_selection_init(server);

	/* The root window. */
	error = x11_window_root(server, SERVER_ROOT_BACKGROUND);
	if (error != 0) {
		x11server_destroy(server);
		return error;
	}

	/* The pointer in the root window's middle, the keyboard's map, the focus following the pointer, no grab. */
	server->pointer_x = (int)server->width / 2;
	server->pointer_y = (int)server->height / 2;
	x11_input_init(server);

	/* The window manager: its atoms, its own window and the root window's EWMH hints. */
	error = x11_wm_init(server);
	if (error != 0) {
		x11server_destroy(server);
		return error;
	}

	/* A rooted server's root window gets its pixels, painted with its background. */
	x11_rooted_start(server);

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
	for (index = 0U; index < server->clients_max; index++)
		x11_client_close(server, index);

	/* The window manager's own window. */
	x11_wm_fini(server);

	/* The clipboard's copies and the texts being given in pieces. */
	x11_selection_destroy(server);

	/* The extensions' state. */
	x11_extension_fini(server);

	/* The root window, with what is left of it (its properties, the extensions' events on it, its pixels, its surface, the cursor a client gave it and the events clients selected on it). */
	if (server->root != NULL) {
		x11_extension_forget_window(server, server->root);
		x11_property_forget_window(server, server->root);
		x11_rootless_forget(server, server->root);
		x11_draw_forget(server, server->root);
		x11_cursor_release(server, server->root->cursor_held);
		server->root->cursor_held = NULL;
		x11_xinput_window_destroyed(server->root);
		free(server->root->selects);
		free(server->root);
		server->root = NULL;
	}

	/* The resource ids, the atoms, the passive grabs and the save-sets. */
	x11_save_set_fini(server);
	x11_resource_fini(server);
	x11_atoms_fini(server);
	x11_input_fini(server);

	/* Vulkan's state, after every swapchain on it, and the font. */
	x11_vulkan_close(server->vulkan);
	x11_fonts_fini(server);
	x11_faces_close(&server->faces);

	/* The server itself. */
	free(server->clients);
	free(server);
}

/*
 * Takes a new client into a free slot and gives the transport its number.
 * With an authorization, the client's setup must present it (an
 * MIT-MAGIC-COOKIE-1 of those bytes) or is refused.  Returns 0, EAGAIN
 * when every slot is in use (the transport refuses the connection), or
 * EINVAL for an authorization the core cannot check.
 */
int
x11server_connect(
	struct x11server *server,
	const struct x11server_auth *auth,
	uint32_t *client)
{
	struct x11_client *slot;
	unsigned index;
	int differs;

	/* Only MIT-MAGIC-COOKIE-1 of a size the core keeps can be checked. */
	*client = 0U;
	if (auth != NULL) {
		if (auth->name == NULL ||
		    auth->data == NULL ||
		    auth->size == 0U ||
		    auth->size > X11_AUTH_MAX)
			return EINVAL;
		differs = strcmp(auth->name, X11_AUTH_COOKIE);
		if (differs != 0)
			return EINVAL;
	}

	/* The first slot free and not waiting for the transport to hear of a close. */
	for (index = 0U; index < server->clients_max; index++) {
		if (server->clients[index].id == 0U && server->clients[index].closed == 0U)
			break;
	}

	/* No room: the connection is refused, and the log says so. */
	if (index == server->clients_max) {
		x11_log(server, "X11 CONNECT refused: all %u client slots are in use", server->clients_max);
		return EAGAIN;
	}

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
	slot->base = (index + 1U) << X11_ID_SHIFT;
	slot->last_ms = server->now;

	/* The authorization its setup must present. */
	if (auth != NULL) {
		slot->auth_required = 1;
		memcpy(slot->auth_data, auth->data, auth->size);
		slot->auth_size = auth->size;
	}

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

	/* The client; no bytes is nothing to add. */
	connection = server_client(server, client, &index);
	if (connection == NULL)
		return ENOENT;
	if (size == 0U)
		return 0;

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

	/* No more than is waiting, and nothing to take when nothing was sent. */
	if (size > connection->output_used)
		size = connection->output_used;
	if (size == 0U)
		return;

	/* What was sent leaves the queue; the transport hears again of what comes next. */
	memmove(connection->output, connection->output + size, connection->output_used - size);
	connection->output_used -= size;
	connection->output_told = 0;
}

/*
 * Gives how many bytes of a client's input the core holds unhandled, and
 * in *wanted how many more it waits for before the request at their
 * front is whole.
 *
 * A transport that holds a client back by a window smaller than the
 * longest request uses *wanted to let the rest of a long request in: the
 * core handles nothing of a request until it is whole, so no input is
 * reported handled while it waits.  *wanted is 0 when the front request
 * is whole, when not even its length has come yet, or for a request too
 * long to hold (its bytes are dropped as they come and reported handled).
 * A client that is not connected holds nothing.
 */
size_t
x11server_held(
	struct x11server *server,
	uint32_t client,
	size_t *wanted)
{
	struct x11_client *connection;
	unsigned index;
	uint32_t words;
	uint16_t units;
	size_t need;

	/* Nothing wanted until the front request says how long it is. */
	*wanted = 0U;

	/* Nothing held for a client that is not connected. */
	connection = server_client(server, client, &index);
	if (connection == NULL)
		return 0U;

	/* The setup, the bytes being dropped, and a request without its length leave nothing to wait for. */
	if (!connection->setup)
		return connection->used;
	if (connection->discard > 0U)
		return connection->used;
	if (connection->used < 4U)
		return connection->used;

	/* The length the header gives, in words; zero means a 32-bit length follows for a BIG-REQUESTS client. */
	units = x11_read16(connection->input + 2, connection->order);
	need = (size_t)units * 4U;
	if (units == 0U) {
		if (!connection->big_requests)
			return connection->used;
		if (connection->used < 8U)
			return connection->used;
		words = x11_read32(connection->input + 4, connection->order);
		if (words > X11_INPUT_CAP / 4U)
			return connection->used;
		need = (size_t)words * 4U;
	}

	/* The rest of the front request, when part of it is still to come. */
	if (need > connection->used)
		*wanted = need - connection->used;

	/* Succeeded: the bytes held. */
	return connection->used;
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
	for (index = 0U; index < server->clients_max; index++) {
		connection = &server->clients[index];
		if (connection->closed == 0U)
			continue;
		event->type = X11SERVER_EVENT_CLOSED;
		event->client = connection->closed;
		connection->closed = 0U;
		return 1;
	}

	/* Input handled: the transport may take as much more. */
	for (index = 0U; index < server->clients_max; index++) {
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
	for (index = 0U; index < server->clients_max; index++) {
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
 * held back, each client's whole requests, the windows' new pixels and
 * the cursor shown (an animated one moved on), the clients that stopped
 * taking their output closed, and the report of stuck clients.  *wake is
 * when the caller is to call again (by an animated cursor's next frame).
 * Returns 0.
 */
int
x11server_tick(
	struct x11server *server,
	uint32_t now,
	uint32_t *wake)
{
	struct x11_client *client;
	unsigned grabbed;
	unsigned index;
	uint32_t due;
	int animated;
	int waiting;
	int timed;
	int more;
	int left;
	int lost;

	/* The server's time never goes back. */
	if ((int32_t)(now - server->now) > 0)
		server->now = now;

	/* A device that was lost ends the clients that drew on it, and the server goes on without Vulkan. */
	lost = x11_vulkan_lost(server->vulkan);
	if (lost)
		server_vulkan_lost(server);

	/* The windows the backend asked to close, and a motion held back. */
	x11_rootless_closing(server);
	x11_rootless_flush_motion(server);

	/* The extensions' waits that time ended (SYNC's, the window manager's resizes): a client woken goes on now. */
	more = x11_extension_tick(server, &timed);

	/*
	 * Each client's whole requests, up to a tick's share; while a client
	 * holds the server, only its own.  A grab that ends in the pass lets
	 * the others go on at once.
	 */
	grabbed = server->grabbed;
	for (index = 0U; index < server->clients_max; index++) {
		if (server->clients[index].id == 0U)
			continue;
		if (server->grabbed != X11_NO_CLIENT && server->grabbed != index)
			continue;
		left = server_handle(server, index);
		if (left)
			more = 1;
	}
	if (grabbed != X11_NO_CLIENT && server->grabbed == X11_NO_CLIENT)
		more = 1;

	/* The pointer's crossing events of the windows that changed under it. */
	x11_input_tick(server);

	/* An X owner's text in pieces starts once the owner listens. */
	waiting = x11_selection_tick(server);

	/* The cursor under the pointer, and what changed, shown. */
	x11_cursor_update(server);
	x11_rootless_present(server);

	/* A client that stopped taking its output is closed, and one refused once its refusal is taken. */
	for (index = 0U; index < server->clients_max; index++) {
		client = &server->clients[index];
		if (client->id == 0U)
			continue;
		if (client->broken)
			x11_client_close(server, index);
		else if (client->ending && client->output_used == 0U)
			x11_client_close(server, index);
	}

	/* A client holding a part of a request, or bytes it does not take, is reported. */
	server_report(server);

	/* At once while requests wait, soon while something is to be shown or held back, else at the next report. */
	if (more) {
		*wake = server->now;
	} else if (server->dirty || server->motion_pending || waiting || timed) {
		*wake = server->now + SERVER_TICK_MS;
	} else {
		*wake = server->now + SERVER_REPORT_MS;
	}

	/* Sooner when the animated cursor shown moves on before then. */
	animated = x11_cursor_due(server, &due);
	if (animated && (int32_t)(due - *wake) < 0)
		*wake = due;

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
	/* The server and a free slot have no client. */
	if (owner >= server->clients_max)
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

	/* The windows of its save-set leave its windows and are mapped, while it is still there to map them. */
	x11_save_set_restore(server, index);

	/* The client is gone from now on. */
	id = client->id;
	client->id = 0U;

	/*
	 * Its grabs of the devices, the selections it owned (before its
	 * windows, so that XFIXES's clients hear the client closed, as the
	 * X.Org server has it), and its windows (with their surfaces),
	 * pixmaps, graphics contexts and fonts.
	 */
	x11_input_forget_client(server, index);
	x11_selection_forget_client(server, index);
	x11_resources_release(server, index);

	/* What it held in the extensions (regions, counters, alarms, waits, events it asked for). */
	x11_extension_forget_client(server, index);

	/* Its grab of the server goes. */
	if (server->grabbed == index)
		server->grabbed = X11_NO_CLIENT;

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
	if (id == 0U || slot >= server->clients_max)
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
	const uint8_t *request;
	uint32_t words;
	uint16_t units;
	uint8_t opcode;
	size_t handled;
	size_t dropped;
	size_t need;
	size_t length;
	int whole;

	/* Each whole message at the front of the buffer. */
	client = &server->clients[index];
	handled = 0U;
	for (;;) {
		/* The rest waits for the next tick once the tick's share is used; a refused client sends nothing more. */
		if (handled >= X11_TICK_BYTES)
			break;
		if (client->ending)
			return 0;

		/* A client waiting for counters or fences (SYNC's Await) sends nothing more until the wait ends. */
		if (client->blocked)
			return 0;

		/* The bytes of a request refused for its length go as they come. */
		if (client->discard > 0U) {
			dropped = client->used;
			if (dropped > client->discard)
				dropped = client->discard;
			memmove(client->input, client->input + dropped, client->used - dropped);
			client->used -= dropped;
			client->consumed += dropped;
			client->discard -= dropped;
			handled += dropped;
			if (client->discard > 0U)
				return 0;
			continue;
		}

		/* The setup, answered whole or not at all. */
		if (!client->setup) {
			whole = server_setup(server, index, &need);
			if (!whole)
				return 0;
			if (client->id == 0U || client->ending) {
				if (client->id != 0U)
					client->consumed += need;
				return 0;
			}
		} else {
			/* The header and the length it gives (in words). */
			if (client->used < 4U)
				return 0;
			units = x11_read16(client->input + 2, client->order);
			request = client->input;
			need = (size_t)units * 4U;
			length = need;

			/* Zero is a length only for a client that enabled BIG-REQUESTS: a 32-bit length in words follows the header. */
			if (units == 0U) {
				if (!client->big_requests) {
					x11_client_close(server, index);
					return 0;
				}
				if (client->used < 8U)
					return 0;
				words = x11_read32(client->input + 4, client->order);
				if (words < 2U) {
					x11_client_close(server, index);
					return 0;
				}
				need = (size_t)words * 4U;

				/* Longer than the core holds: refused, and its bytes dropped as they come. */
				if (words > X11_INPUT_CAP / 4U) {
					x11_request_refuse(server, index, client->input[0], client->input[1], X11_BAD_LENGTH);
					client->discard = need;
					continue;
				}
			}

			/* The whole request. */
			if (client->used < need)
				return 0;

			/*
			 * A big request is handed on as an ordinary one: its header
			 * moves over the 32-bit length, and the request starts there.
			 */
			if (units == 0U) {
				memmove(client->input + 4, client->input, 4U);
				request = client->input + 4;
				length = need - 4U;
			}

			/* The request; a request that ended its own client leaves nothing more to do. */
			opcode = request[0];
			x11_request(server, index, request, length);
			if (client->id == 0U)
				return 0;

			/* The request, remembered for the stuck-client report. */
			client->last_opcode = opcode;
			client->last_ms = server->now;
			client->history_opcode[client->history_next] = opcode;
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

/*
 * Answers a client's setup once it is whole: accepted, or refused when it
 * does not present the authorization asked for (the client then ends once
 * the refusal is taken).  Returns nonzero when the setup was whole (*need:
 * its length), 0 when more is to come or the client was closed.
 */
static int
server_setup(
	struct x11server *server,
	unsigned index,
	size_t *need)
{
	struct x11_client *client;
	uint16_t name_length;
	uint16_t data_length;
	size_t name_padded;
	int authorized;
	int error;

	/* The fixed part, and the byte order it names. */
	client = &server->clients[index];
	*need = 0U;
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
	name_padded = ((size_t)name_length + 3U) & ~(size_t)3U;
	*need = 12U + name_padded + (((size_t)data_length + 3U) & ~(size_t)3U);
	if (client->used < *need)
		return 0;

	/* A client asked for an authorization must present it. */
	if (client->auth_required) {
		authorized = server_authorized(client, client->input + 12, name_length, client->input + 12 + name_padded, data_length);
		if (!authorized) {
			x11_log(server, "X11SERVER SETUP refused client=%u (authorization)", index);
			server_refuse(server, client, SERVER_REFUSED_AUTH);
			client->ending = 1;
			return 1;
		}
	}

	/* The setup is answered. */
	error = x11_setup_reply(server, client);
	if (error != 0) {
		x11_client_close(server, index);
		return 0;
	}

	/* Succeeded: requests follow. */
	client->setup = 1;
	return 1;
}

/*
 * Tells whether a setup's authorization is the one asked for: the name
 * MIT-MAGIC-COOKIE-1 and the same bytes, compared in a time that does not
 * depend on where they differ.
 */
static int
server_authorized(
	const struct x11_client *client,
	const uint8_t *name,
	size_t name_length,
	const uint8_t *data,
	size_t data_length)
{
	unsigned difference;
	size_t index;
	int differs;

	/* The name. */
	if (name_length != sizeof(X11_AUTH_COOKIE) - 1U)
		return 0;
	differs = memcmp(name, X11_AUTH_COOKIE, name_length);
	if (differs != 0)
		return 0;

	/* The data's size. */
	if (data_length != client->auth_size)
		return 0;

	/* Every byte, each one looked at. */
	difference = 0U;
	for (index = 0U; index < data_length; index++)
		difference |= (unsigned)(data[index] ^ client->auth_data[index]);
	if (difference != 0U)
		return 0;

	/* Succeeded: the authorization asked for. */
	return 1;
}

/* Queues a refused setup's answer: Failed, protocol 11.0, and the reason. */
static void
server_refuse(
	struct x11server *server,
	struct x11_client *client,
	const char *reason)
{
	uint8_t reply[8U + 64U];
	size_t length;
	size_t padded;

	/* The reason, padded to words. */
	length = strlen(reason);
	if (length > 64U)
		length = 64U;
	padded = (length + 3U) & ~(size_t)3U;

	/* The answer. */
	memset(reply, 0, sizeof(reply));
	reply[0] = 0U;
	reply[1] = (uint8_t)length;
	x11_write16(reply + 2, 11U, client->order);
	x11_write16(reply + 4, 0U, client->order);
	x11_write16(reply + 6, (uint16_t)(padded / 4U), client->order);
	memcpy(reply + 8, reason, length);

	/* Queued. */
	x11_client_send(server, client, reply, 8U + padded);
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
	for (index = 0U; index < server->clients_max; index++) {
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

/*
 * Gives up Vulkan after its device was lost: what the clients drew there
 * is gone, so each one is ended (they may connect again); the server then
 * draws in memory and shows through the backend.
 */
static void
server_vulkan_lost(
	struct x11server *server)
{
	unsigned index;

	/* Every client, with its windows (their swapchains) and pixmaps. */
	x11_log(server, "X11SERVER VULKAN lost: every client is ended, drawing goes on in memory");
	for (index = 0U; index < server->clients_max; index++) {
		if (server->clients[index].id != 0U)
			x11_client_close(server, index);
	}

	/* Vulkan, after everything on it. */
	x11_vulkan_close(server->vulkan);
	server->vulkan = NULL;
	server->cpu_present = 1;
}
