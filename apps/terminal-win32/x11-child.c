/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * mikuterm's X server process (mikuterm.exe --x11-server): one X server
 * of one SSH connection, its windows on this desktop, and its clients
 * over the link to the mikuterm that started it.
 *
 * The process reads the description of its socket's end from its
 * standard input (a pipe the parent closes), says HELLO, and takes the
 * parent's HELLO with the settings (the host's name, the fake cookie, the
 * scale, the clipboard's policy, the root window shown as one window or
 * not).  Then one thread runs the X server's
 * core and the Windows backend and the link: each OPEN is an X client
 * checked against the cookie, its DATA the client's bytes, and the
 * client's replies go back as DATA within the parent's credit.  The
 * process ends with the link, on SHUTDOWN, or when the parent's frames
 * break the rules; it shows no message of its own.
 */

#include "mikuterm.h"
#include "x11-host.h"
#include "x11-link.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include "userland/desktop/xserver/win32/backend.h"
#include "userland/desktop/xserver/x11server.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The fonts of the core fonts, inside mikuterm's folder: how many, and the monospaced faces' fallback. */
#define CHILD_FONTS		3U
#define CHILD_FALLBACK		"share/fonts/keiland-fallback-mono.ttf"
#define CHILD_FONT_MAX		(32U * 1024U * 1024U)

/* The authorization the X clients present (the fake cookie's protocol). */
#define CHILD_AUTH_NAME		"MIT-MAGIC-COOKIE-1"

/* What is read at once, the reading buffer, and the frames waiting before the X clients' replies wait. */
#define CHILD_READ		65536U
#define CHILD_IN_MAX		(X11_LINK_HEADER + X11_LINK_BODY_MAX + CHILD_READ)
#define CHILD_OUT_HIGH		(1024U * 1024U)
#define CHILD_OUT_MAX		(4U * 1024U * 1024U)

/* The longest wait for the link (ms), and the HELLO's root window's size and its fixed part. */
#define CHILD_WAIT_MS		1000U
#define CHILD_HELLO_ROOT	(12U + X11_HOST_COOKIE)
#define CHILD_HELLO_FIXED	(CHILD_HELLO_ROOT + 8U)

/* The longest side of a root window shown as one window (the core's pixels' longest). */
#define CHILD_ROOT_MAX		16384U

/*
 * The X server process's state: the link's socket and its event, the
 * frames read and those waiting, the link's table and the X client of
 * each slot of it, the settings, the backend, the core and its fonts.
 */
struct child {
	SOCKET socket;
	WSAEVENT event;
	uint8_t *in;
	size_t in_size;
	uint8_t *out;
	size_t out_size;
	size_t out_capacity;
	struct x11_link_table table;
	uint32_t clients[X11_LINK_CHANNELS];
	int hello;
	int ending;
	uint8_t cookie[X11_HOST_COOKIE];
	char host[X11_HOST_NAME_MAX];
	unsigned scale;
	unsigned flags;
	unsigned root_width;
	unsigned root_height;
	struct x11_win32 *backend;
	struct x11server *server;
	char *fonts[CHILD_FONTS];
	size_t font_sizes[CHILD_FONTS];
	char fallback[MIKUTERM_PATH_MAX];
};

/*
 * The font files the core fonts are drawn from, by their roles: the
 * monospaced face, and the proportional face and its bold (the
 * monospaced bold is the core's own, made bold).
 */
static const struct {
	const char *path;
	unsigned role;
} child_fonts[CHILD_FONTS] = {
	{ "share/fonts/keiland-mono.ttf", X11SERVER_FONT_MONO },
	{ "share/fonts/keiland.ttf", X11SERVER_FONT_SANS },
	{ "share/fonts/keiland-bold.ttf", X11SERVER_FONT_SANS_BOLD }
};

static int child_connect(struct child *child);
static void child_round(struct child *child);
static void child_flush(struct child *child);
static void child_service(void *context);
static int child_receive(struct child *child);
static int child_take(struct child *child, const struct x11_link_frame *frame);
static int child_hello(struct child *child, const struct x11_link_frame *frame);
static int child_start_server(struct child *child);
static void child_open(struct child *child, uint32_t id);
static void child_end_client(struct child *child, uint32_t id, int tell);
static void child_events(struct child *child);
static void child_credit(struct child *child, unsigned slot);
static void child_output(struct child *child);
static int child_frame(struct child *child, unsigned type, uint32_t id, const void *body, uint32_t size);
static int child_send(struct child *child);
static unsigned child_slot(struct child *child, uint32_t id);
static unsigned child_slot_of_client(struct child *child, uint32_t client);

/*
 * Runs mikuterm's X server process until its link ends.  Returns the
 * process's exit code: 0 for an end the parent asked for or the link's
 * end, 1 for a start that failed, 2 for a parent that broke the rules.
 */
int
mikuterm_x11_child(void)
{
	static struct child child;
	uint8_t version[4];
	DWORD now;
	uint32_t wake;
	DWORD timeout;
	HANDLE handles[1];
	unsigned waited;
	unsigned index;
	int error;
	int status;

	/* The folder (for the fonts) and the link. */
	memset(&child, 0, sizeof(child));
	child.socket = INVALID_SOCKET;
	x11_link_table_init(&child.table, X11_LINK_CHILD);
	(void)mikuterm_folder_init();
	error = child_connect(&child);
	if (error != 0)
		return 1;

	/* Says HELLO. */
	x11_link_put32(version, X11_LINK_VERSION);
	(void)child_frame(&child, X11_LINK_HELLO, 0, version, sizeof(version));

	/* Round after round until the link ends. */
	status = 0;
	for (;;) {
		child_round(&child);
		if (child.ending)
			break;

		/* The wait: the socket, the windows' messages, the core's next time. */
		timeout = CHILD_WAIT_MS;
		if (child.server != NULL) {
			now = GetTickCount();
			wake = (uint32_t)now + CHILD_WAIT_MS;
			(void)x11server_tick(child.server, (uint32_t)now, &wake);
			if (wake - (uint32_t)now < timeout)
				timeout = wake - (uint32_t)now;

			/* The replies and events the tick made go to the parent before the wait, not after it. */
			child_flush(&child);
			if (child.ending)
				break;
		}
		handles[0] = child.event;
		if (child.backend != NULL) {
			waited = x11_win32_wait(child.backend, (void *const *)handles, 1, timeout);
			if (waited == X11_WIN32_WAIT_QUIT)
				break;
		} else {
			(void)WaitForSingleObject(child.event, timeout);
		}
	}
	if (child.ending == 2)
		status = 2;

	/* The end: the core, the backend, the link, the fonts' bytes. */
	if (child.server != NULL)
		x11server_destroy(child.server);
	if (child.backend != NULL)
		x11_win32_destroy(child.backend);
	closesocket(child.socket);
	WSACloseEvent(child.event);
	WSACleanup();
	for (index = 0U; index < CHILD_FONTS; index++)
		free(child.fonts[index]);
	free(child.in);
	free(child.out);
	return status;
}

/*
 * Opens the link: the socket's description from standard input, the
 * socket, its event (which makes it non-blocking).  Returns 0, or an
 * errno value.
 */
static int
child_connect(
	struct child *child)
{
	WSADATA data;
	WSAPROTOCOL_INFOW information;
	HANDLE input;
	DWORD got;
	DWORD total;
	BOOL done;

	/* WinSock. */
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
		return EIO;

	/* The description, whole, from the pipe. */
	input = GetStdHandle(STD_INPUT_HANDLE);
	total = 0;
	while (total < sizeof(information)) {
		done = ReadFile(input, (uint8_t *)&information + total, sizeof(information) - total, &got, NULL);
		if (!done || got == 0)
			return EIO;
		total += got;
	}
	CloseHandle(input);

	/* The socket, and its event. */
	child->socket = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &information, 0, 0);
	if (child->socket == INVALID_SOCKET)
		return EIO;
	child->event = WSACreateEvent();
	if (child->event == WSA_INVALID_EVENT)
		return EIO;
	if (WSAEventSelect(child->socket, child->event, FD_READ | FD_WRITE | FD_CLOSE) != 0)
		return EIO;

	/* The buffers. */
	child->in = malloc(CHILD_IN_MAX);
	if (child->in == NULL)
		return ENOMEM;

	/* Succeeded: the link is open. */
	return 0;
}

/*
 * Does one round of work that does not wait: what the parent sent, the
 * core's events and the clients' replies, and the frames.
 */
static void
child_round(
	struct child *child)
{
	WSANETWORKEVENTS events;
	int error;

	/* Clears the socket's event; what it says is read below. */
	(void)WSAEnumNetworkEvents(child->socket, child->event, &events);

	/* What the parent sent. */
	error = child_receive(child);
	if (error != 0)
		return;

	/* The core's events and the clients' replies, and the frames. */
	child_flush(child);
}

/*
 * Sends what the core has: its events (a client closed, more credit for
 * the parent), the clients' replies within the parent's credit, and the
 * frames queued.
 */
static void
child_flush(
	struct child *child)
{
	int error;

	/* The core's events and replies. */
	if (child->server != NULL) {
		child_events(child);
		child_output(child);
		child_events(child);
	}

	/* The frames; a link that cannot take them ends the child. */
	error = child_send(child);
	if (error != 0)
		child->ending = 1;
}

/* Keeps the server and the link going while Windows runs its own loop (a window moved or sized). */
static void
child_service(
	void *context)
{
	struct child *child;
	uint32_t wake;

	/* The core's time, and a round. */
	child = context;
	if (child->server != NULL)
		(void)x11server_tick(child->server, (uint32_t)GetTickCount(), &wake);
	child_round(child);
}

/*
 * Reads what the parent sent and takes each whole frame.  Returns 0, or
 * EPIPE when the link ended (child->ending set).
 */
static int
child_receive(
	struct child *child)
{
	struct x11_link_frame frame;
	size_t used;
	size_t offset;
	int received;
	int error;

	/* Reads until nothing waits. */
	for (;;) {
		if (CHILD_IN_MAX - child->in_size < CHILD_READ)
			break;
		received = recv(child->socket, (char *)child->in + child->in_size, CHILD_READ, 0);
		if (received == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
			break;
		if (received <= 0) {
			child->ending = 1;
			return EPIPE;
		}
		child->in_size += (size_t)received;
	}

	/* Takes each whole frame. */
	offset = 0;
	for (;;) {
		error = x11_link_parse(child->in + offset, child->in_size - offset, &frame, &used);
		if (error == EAGAIN)
			break;
		if (error != 0) {
			child->ending = 2;
			return EPIPE;
		}
		error = child_take(child, &frame);
		if (error != 0) {
			child->ending = 2;
			return EPIPE;
		}
		offset += used;
		if (child->ending)
			break;
	}

	/* Keeps the part of a frame not yet whole. */
	if (offset != 0) {
		memmove(child->in, child->in + offset, child->in_size - offset);
		child->in_size -= offset;
	}
	return 0;
}

/*
 * Takes one of the parent's frames.  Returns 0, or EPROTO for a frame
 * against the rules (the link ends).
 */
static int
child_take(
	struct child *child,
	const struct x11_link_frame *frame)
{
	unsigned slot;
	int verdict;
	int error;

	/* The rules, and the HELLO first. */
	verdict = x11_link_check(&child->table, frame);
	if (verdict == X11_LINK_VIOLATION)
		return EPROTO;
	if (!child->hello && frame->type != X11_LINK_HELLO)
		return EPROTO;
	if (verdict == X11_LINK_DROP)
		return 0;

	/* What it says. */
	switch (frame->type) {
	case X11_LINK_HELLO:
		error = child_hello(child, frame);
		return error;
	case X11_LINK_PING:
		(void)child_frame(child, X11_LINK_PONG, 0, NULL, 0);
		return 0;
	case X11_LINK_SHUTDOWN:
		child->ending = 1;
		return 0;
	case X11_LINK_OPEN:
		child_open(child, frame->id);
		return 0;
	case X11_LINK_CREDIT:
		return 0;
	case X11_LINK_EOF:
	case X11_LINK_CLOSE:
		/* The remote client went: the core lets it go (an EOF is answered with CLOSE). */
		child_end_client(child, frame->id, frame->type == X11_LINK_EOF);
		return 0;
	default:
		break;
	}

	/* DATA: the remote client's bytes for the core. */
	slot = child_slot(child, frame->id);
	if (slot < X11_LINK_CHANNELS && child->server != NULL)
		(void)x11server_input(child->server, child->clients[slot], frame->body, frame->size);
	return 0;
}

/*
 * Takes the parent's HELLO (once): the version, the scale, the flags, the
 * cookie, the root window's size and the host's name; then the X server
 * starts.  Returns 0, or EPROTO.
 */
static int
child_hello(
	struct child *child,
	const struct x11_link_frame *frame)
{
	size_t name;
	int error;

	/* Once, of this version, with every fixed field. */
	if (child->hello || frame->size < CHILD_HELLO_FIXED)
		return EPROTO;
	if (x11_link_get32(frame->body) != X11_LINK_VERSION)
		return EPROTO;

	/* The settings. */
	child->scale = x11_link_get32(frame->body + 4);
	child->flags = x11_link_get32(frame->body + 8);
	memcpy(child->cookie, frame->body + 12, X11_HOST_COOKIE);
	child->root_width = x11_link_get32(frame->body + CHILD_HELLO_ROOT);
	child->root_height = x11_link_get32(frame->body + CHILD_HELLO_ROOT + 4U);
	if (child->root_width > CHILD_ROOT_MAX || child->root_height > CHILD_ROOT_MAX)
		return EPROTO;
	name = frame->size - CHILD_HELLO_FIXED;
	if (name >= sizeof(child->host))
		name = sizeof(child->host) - 1U;
	memcpy(child->host, frame->body + CHILD_HELLO_FIXED, name);
	child->host[name] = '\0';
	child->hello = 1;

	/* The X server. */
	error = child_start_server(child);
	if (error != 0)
		return EPROTO;

	/* Succeeded. */
	return 0;
}

/*
 * Starts the backend and the core: the windows of the host, the root the
 * size of the virtual screen (or, shown as one window, the size the
 * settings give), the core fonts from mikuterm's folder, the windows shown
 * through GDI.  Returns 0, or an errno value.
 */
static int
child_start_server(
	struct child *child)
{
	struct x11_win32_options backend_options;
	struct x11server_options options;
	struct x11server_font_file fonts[CHILD_FONTS];
	char path[MIKUTERM_PATH_MAX];
	unsigned count;
	unsigned index;
	unsigned width;
	unsigned height;
	int error;

	/* The backend. */
	memset(&backend_options, 0, sizeof(backend_options));
	backend_options.host = child->host;
	backend_options.scale_percent = child->scale;
	backend_options.trusted = (child->flags & X11_HOST_TRUSTED) != 0;
	backend_options.primary_to_clipboard = (child->flags & X11_HOST_PRIMARY) != 0;
	backend_options.service = child_service;
	backend_options.service_context = child;
	error = x11_win32_create(&backend_options, &child->backend);
	if (error != 0)
		return error;

	/* Each font file's bytes that can be read (a missing one's role is drawn with another face). */
	count = 0U;
	for (index = 0U; index < CHILD_FONTS; index++) {
		error = mikuterm_path(path, sizeof(path), child_fonts[index].path);
		if (error == 0)
			error = mikuterm_file_read(path, CHILD_FONT_MAX, &child->fonts[index], &child->font_sizes[index]);
		if (error != 0) {
			child->fonts[index] = NULL;
			child->font_sizes[index] = 0;
			continue;
		}
		fonts[count].bytes = (const uint8_t *)child->fonts[index];
		fonts[count].size = child->font_sizes[index];
		fonts[count].role = child_fonts[index].role;
		count++;
	}

	/* The monospaced faces' fallback's path. */
	error = mikuterm_path(child->fallback, sizeof(child->fallback), CHILD_FALLBACK);
	if (error != 0)
		child->fallback[0] = '\0';

	/* The core, its root the virtual screen in X's pixels, or one window of the size the settings give. */
	x11_win32_root_size(child->backend, &width, &height);
	memset(&options, 0, sizeof(options));
	options.mode = X11SERVER_ROOTLESS;
	if (child->root_width != 0 && child->root_height != 0) {
		width = child->root_width;
		height = child->root_height;
		options.mode = X11SERVER_ROOTED;
	}
	options.width = width;
	options.height = height;
	options.backend = &x11_win32_ops;
	options.backend_context = child->backend;
	options.vulkan_wsi = NULL;
	if (count != 0U) {
		options.fonts = fonts;
		options.font_count = count;
	}
	if (child->fallback[0] != '\0')
		options.font_fallback_path = child->fallback;
	error = x11server_create(&options, (uint32_t)GetTickCount(), &child->server);
	if (error != 0)
		return error;

	/* Succeeded: the backend hears the core. */
	x11_win32_attach(child->backend, child->server);
	return 0;
}

/* Connects an X client the parent opened, which must present the fake cookie; a refused one is closed. */
static void
child_open(
	struct child *child,
	uint32_t id)
{
	struct x11server_auth auth;
	uint32_t client;
	unsigned slot;
	int error;

	/* The slot the table opened. */
	slot = child_slot(child, id);
	if (slot >= X11_LINK_CHANNELS || child->server == NULL)
		return;

	/* The client, with the authorization it must present. */
	auth.name = CHILD_AUTH_NAME;
	auth.data = child->cookie;
	auth.size = X11_HOST_COOKIE;
	error = x11server_connect(child->server, &auth, &client);
	if (error != 0) {
		child_end_client(child, id, 1);
		return;
	}

	/* Succeeded: the slot's client. */
	child->clients[slot] = client;
}

/* Lets an X client go: the core hangs it up, the channel closes, and the parent is told when asked. */
static void
child_end_client(
	struct child *child,
	uint32_t id,
	int tell)
{
	unsigned slot;

	/* The core lets the client go. */
	slot = child_slot(child, id);
	if (slot >= X11_LINK_CHANNELS)
		return;
	if (child->server != NULL && child->clients[slot] != 0)
		x11server_hangup(child->server, child->clients[slot]);
	child->clients[slot] = 0;
	x11_link_close(&child->table, id);

	/* The parent closes the SSH channel. */
	if (tell)
		(void)child_frame(child, X11_LINK_CLOSE, id, NULL, 0);
}

/*
 * Takes the core's events: a client it closed (the parent is told).  Then
 * gives each open client's parent more credit as the core takes its bytes.
 */
static void
child_events(
	struct child *child)
{
	struct x11server_event event;
	unsigned slot;
	uint32_t id;
	int found;

	/* Each event in turn (x11server_next gives 1 with one, 0 when none is left). */
	for (;;) {
		found = x11server_next(child->server, &event);
		if (found == 0)
			break;
		slot = child_slot_of_client(child, event.client);
		if (slot >= X11_LINK_CHANNELS)
			continue;
		id = child->table.channels[slot].id;

		/* A client the core closed: the channel closes. */
		if (event.type == X11SERVER_EVENT_CLOSED) {
			child->clients[slot] = 0;
			x11_link_close(&child->table, id);
			(void)child_frame(child, X11_LINK_CLOSE, id, NULL, 0);
		}
	}

	/* Each open client's credit, topped up to what the core may now take of it. */
	for (slot = 0; slot < X11_LINK_CHANNELS; slot++) {
		if (!child->table.channels[slot].used || child->clients[slot] == 0)
			continue;
		child_credit(child, slot);
	}
}

/*
 * Gives the parent credit on one client's channel, so that what the core
 * holds of the client and what the parent may still send come to the
 * channel's starting credit, or to the rest of a long request the core
 * holds a part of (the core handles none of it, and reports nothing
 * handled, until it is whole).  Never past X11SERVER_REQUEST_MAX, the
 * most the core holds.
 */
static void
child_credit(
	struct child *child,
	unsigned slot)
{
	struct x11_link_channel *channel;
	uint8_t credit[4];
	size_t held;
	size_t wanted;
	size_t target;
	size_t allowed;
	uint32_t given;
	int error;

	/* What the core holds of the client, and how much more its front request needs. */
	channel = &child->table.channels[slot];
	held = x11server_held(child->server, child->clients[slot], &wanted);

	/* The bytes the core may have: the starting credit, or the whole of a longer request. */
	target = X11_LINK_CREDIT_TO_CHILD;
	if (held + wanted > target)
		target = held + wanted;
	if (target > X11SERVER_REQUEST_MAX)
		target = X11SERVER_REQUEST_MAX;

	/* What the parent may already send counts; nothing to give when it reaches the target. */
	allowed = held + channel->credit_in;
	if (allowed >= target)
		return;
	given = (uint32_t)(target - allowed);

	/* The parent may send that much more, counted once the frame is queued (the next round tries again). */
	x11_link_put32(credit, given);
	error = child_frame(child, X11_LINK_CREDIT, channel->id, credit, 4);
	if (error != 0)
		return;
	x11_link_gave(&child->table, channel->id, given);
}

/*
 * Sends the clients' replies within the parent's credit, while the frames
 * waiting stay below CHILD_OUT_HIGH.
 */
static void
child_output(
	struct child *child)
{
	const uint8_t *bytes;
	size_t size;
	uint32_t room;
	unsigned slot;
	uint32_t id;
	int error;

	/* Each open client. */
	for (slot = 0; slot < X11_LINK_CHANNELS; slot++) {
		if (!child->table.channels[slot].used || child->clients[slot] == 0)
			continue;
		id = child->table.channels[slot].id;

		/* Its bytes, frame by frame, within the credit. */
		for (;;) {
			if (child->out_size >= CHILD_OUT_HIGH)
				return;
			bytes = x11server_output(child->server, child->clients[slot], &size);
			if (bytes == NULL || size == 0)
				break;
			room = x11_link_room(&child->table, id);
			if (room == 0)
				break;
			if (size > room)
				size = room;
			if (size > X11_LINK_BODY_MAX)
				size = X11_LINK_BODY_MAX;
			error = child_frame(child, X11_LINK_DATA, id, bytes, (uint32_t)size);
			if (error != 0)
				return;
			x11_link_sent(&child->table, id, (uint32_t)size);
			x11server_written(child->server, child->clients[slot], size);
		}
	}
}

/* Queues one frame for the parent.  Returns 0, or ENOMEM. */
static int
child_frame(
	struct child *child,
	unsigned type,
	uint32_t id,
	const void *body,
	uint32_t size)
{
	size_t needed;
	size_t capacity;
	uint8_t *grown;

	/* Room for it. */
	needed = child->out_size + X11_LINK_HEADER + size;
	if (needed > CHILD_OUT_MAX)
		return ENOMEM;
	if (needed > child->out_capacity) {
		capacity = child->out_capacity * 2U;
		if (capacity < needed)
			capacity = needed + CHILD_READ;
		grown = realloc(child->out, capacity);
		if (grown == NULL)
			return ENOMEM;
		child->out = grown;
		child->out_capacity = capacity;
	}

	/* The header and the body. */
	x11_link_header(child->out + child->out_size, type, id, size);
	if (size != 0)
		memcpy(child->out + child->out_size + X11_LINK_HEADER, body, size);
	child->out_size = needed;
	return 0;
}

/* Sends the frames waiting, as far as the socket takes them.  Returns 0, or EPIPE. */
static int
child_send(
	struct child *child)
{
	int sent;

	/* Sends until all went or the socket is full. */
	while (child->out_size != 0) {
		sent = send(child->socket, (const char *)child->out, (int)child->out_size, 0);
		if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
			break;
		if (sent <= 0)
			return EPIPE;
		memmove(child->out, child->out + sent, child->out_size - (size_t)sent);
		child->out_size -= (size_t)sent;
	}
	return 0;
}

/* Gives the slot of a link id (X11_LINK_CHANNELS: none). */
static unsigned
child_slot(
	struct child *child,
	uint32_t id)
{
	struct x11_link_channel *channel;

	/* The table's slot. */
	channel = x11_link_find(&child->table, id);
	if (channel == NULL)
		return X11_LINK_CHANNELS;
	return (unsigned)(channel - child->table.channels);
}

/* Gives the slot of a core's client (X11_LINK_CHANNELS: none). */
static unsigned
child_slot_of_client(
	struct child *child,
	uint32_t client)
{
	unsigned slot;

	/* Looks through the slots. */
	for (slot = 0; slot < X11_LINK_CHANNELS; slot++) {
		if (child->table.channels[slot].used && child->clients[slot] == client && client != 0)
			return slot;
	}
	return X11_LINK_CHANNELS;
}
