/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X server process's side of the link, the same on every system:
 * frames, the parent's settings, the X clients and their credit, and the
 * core fonts.
 */

#include "x11-serve.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The monospaced faces' fallback, and the largest font file read. */
#define SERVE_FALLBACK		"share/fonts/keiland-fallback-mono.ttf"
#define SERVE_FONT_MAX		(32U * 1024U * 1024U)

/* The authorization the X clients present (the fake cookie's protocol). */
#define SERVE_AUTH_NAME		"MIT-MAGIC-COOKIE-1"

/* What is read at once, the reading buffer, and the frames waiting before the X clients' replies wait. */
#define SERVE_READ		65536U
#define SERVE_IN_MAX		(X11_LINK_HEADER + X11_LINK_BODY_MAX + SERVE_READ)
#define SERVE_OUT_HIGH		(1024U * 1024U)
#define SERVE_OUT_MAX		(4U * 1024U * 1024U)

/* The HELLO's root window's size and its fixed part. */
#define SERVE_HELLO_ROOT	(12U + X11_HOST_COOKIE)
#define SERVE_HELLO_FIXED	(SERVE_HELLO_ROOT + 8U)

/* The longest side of a root window shown as one window (the core's pixels' longest). */
#define SERVE_ROOT_MAX		16384U

/*
 * The font files the core fonts are drawn from, by their roles: the
 * monospaced face, and the proportional face and its bold (the
 * monospaced bold is the core's own, made bold).
 */
static const struct {
	const char *path;
	unsigned role;
} serve_fonts[X11_SERVE_FONTS] = {
	{ "share/fonts/keiland-mono.ttf", X11SERVER_FONT_MONO },
	{ "share/fonts/keiland.ttf", X11SERVER_FONT_SANS },
	{ "share/fonts/keiland-bold.ttf", X11SERVER_FONT_SANS_BOLD }
};

static int serve_receive(struct x11_serve *serve);
static int serve_take(struct x11_serve *serve, const struct x11_link_frame *frame);
static int serve_hello(struct x11_serve *serve, const struct x11_link_frame *frame);
static void serve_open(struct x11_serve *serve, uint32_t id);
static void serve_end_client(struct x11_serve *serve, uint32_t id, int tell);
static void serve_events(struct x11_serve *serve);
static void serve_credit(struct x11_serve *serve, unsigned slot);
static void serve_output(struct x11_serve *serve);
static int serve_frame(struct x11_serve *serve, unsigned type, uint32_t id, const void *body, uint32_t size);
static int serve_send(struct x11_serve *serve);
static unsigned serve_slot(struct x11_serve *serve, uint32_t id);
static unsigned serve_slot_of_client(struct x11_serve *serve, uint32_t client);

/*
 * Starts the link's state of a new process with the system's part, and
 * queues its HELLO.  Returns 0, or ENOMEM.
 */
int
x11_serve_init(
	struct x11_serve *serve,
	const struct x11_serve_system *system)
{
	uint8_t version[4];
	int error;

	/* Nothing open; the reading buffer. */
	memset(serve, 0, sizeof(*serve));
	serve->system = system;
	x11_link_table_init(&serve->table, X11_LINK_CHILD);
	serve->in = malloc(SERVE_IN_MAX);
	if (serve->in == NULL)
		return ENOMEM;

	/* Says HELLO. */
	x11_link_put32(version, X11_LINK_VERSION);
	error = serve_frame(serve, X11_LINK_HELLO, 0, version, sizeof(version));
	if (error != 0)
		return error;

	/* Succeeded: the HELLO waits for the link. */
	return 0;
}

/*
 * Does one round of work that does not wait: what the parent sent, the
 * core's events and the clients' replies, and the frames.
 */
void
x11_serve_round(
	struct x11_serve *serve)
{
	int error;

	/* What the parent sent. */
	error = serve_receive(serve);
	if (error != 0)
		return;

	/* The core's events and the clients' replies, and the frames. */
	x11_serve_flush(serve);
}

/*
 * Sends what the core has: its events (a client closed, more credit for
 * the parent), the clients' replies within the parent's credit, the tabs'
 * frames, and the frames queued.
 */
void
x11_serve_flush(
	struct x11_serve *serve)
{
	int error;

	/* The core's events and replies, and the tabs' pixels after them. */
	if (serve->server != NULL) {
		serve_events(serve);
		serve_output(serve);
		serve_events(serve);
	}
	if (serve->system->tab_send != NULL)
		serve->system->tab_send(serve->system->context);

	/* The frames; a link that cannot take them ends the process. */
	error = serve_send(serve);
	if (error != 0)
		serve->ending = X11_SERVE_ENDED;
}

/*
 * Gives the core's options the core fonts: each font file's bytes that
 * can be read from mikuterm's folder (a missing one's role is drawn with
 * another face), and the monospaced faces' fallback's path.
 */
void
x11_serve_options(
	struct x11_serve *serve,
	struct x11server_options *options)
{
	char path[MIKUTERM_PATH_MAX];
	unsigned count;
	unsigned index;
	int error;

	/* The files, read once. */
	count = 0U;
	for (index = 0U; index < X11_SERVE_FONTS; index++) {
		if (serve->fonts[index] == NULL) {
			error = mikuterm_path(path, sizeof(path), serve_fonts[index].path);
			if (error == 0)
				error = mikuterm_file_read(path, SERVE_FONT_MAX, &serve->fonts[index], &serve->font_sizes[index]);
			if (error != 0) {
				serve->fonts[index] = NULL;
				serve->font_sizes[index] = 0;
				continue;
			}
		}
		serve->font_files[count].bytes = (const uint8_t *)serve->fonts[index];
		serve->font_files[count].size = serve->font_sizes[index];
		serve->font_files[count].role = serve_fonts[index].role;
		count++;
	}
	if (count != 0U) {
		options->fonts = serve->font_files;
		options->font_count = count;
	}

	/* The fallback. */
	error = mikuterm_path(serve->fallback, sizeof(serve->fallback), SERVE_FALLBACK);
	if (error != 0)
		serve->fallback[0] = '\0';
	if (serve->fallback[0] != '\0')
		options->font_fallback_path = serve->fallback;
}

/* Queues a frame for the parent after the others (the tabs' frames).  Returns 0, or ENOMEM. */
int
x11_serve_frame(
	struct x11_serve *serve,
	unsigned type,
	uint32_t id,
	const void *body,
	uint32_t size)
{
	int error;

	/* The frame after the others. */
	error = serve_frame(serve, type, id, body, size);
	if (error != 0)
		return error;

	/* Succeeded: queued. */
	return 0;
}

/* Gives how many bytes of frames wait for the link. */
size_t
x11_serve_waiting(
	const struct x11_serve *serve)
{
	/* The frames queued. */
	return serve->out_size;
}

/*
 * Ends the link's state: the core (before the system's part ends its
 * backend), the fonts' bytes and the buffers.
 */
void
x11_serve_end(
	struct x11_serve *serve)
{
	unsigned index;

	/* The core. */
	if (serve->server != NULL)
		x11server_destroy(serve->server);
	serve->server = NULL;

	/* The fonts' bytes and the buffers. */
	for (index = 0U; index < X11_SERVE_FONTS; index++) {
		free(serve->fonts[index]);
		serve->fonts[index] = NULL;
	}
	free(serve->in);
	free(serve->out);
	serve->in = NULL;
	serve->out = NULL;
}

/*
 * Reads what the parent sent and takes each whole frame.  Returns 0, or
 * EPIPE when the link ended (serve->ending set).
 */
static int
serve_receive(
	struct x11_serve *serve)
{
	struct x11_link_frame frame;
	size_t used;
	size_t offset;
	size_t received;
	int error;

	/* Reads until nothing waits. */
	for (;;) {
		if (SERVE_IN_MAX - serve->in_size < SERVE_READ)
			break;
		error = serve->system->receive(serve->system->context, serve->in + serve->in_size, SERVE_READ, &received);
		if (error == EAGAIN)
			break;
		if (error != 0 || received == 0) {
			serve->ending = X11_SERVE_ENDED;
			return EPIPE;
		}
		serve->in_size += received;
	}

	/* Takes each whole frame. */
	offset = 0;
	for (;;) {
		error = x11_link_parse(serve->in + offset, serve->in_size - offset, &frame, &used);
		if (error == EAGAIN)
			break;
		if (error != 0) {
			serve->ending = X11_SERVE_BROKEN;
			return EPIPE;
		}
		error = serve_take(serve, &frame);
		if (error != 0) {
			serve->ending = X11_SERVE_BROKEN;
			return EPIPE;
		}
		offset += used;
		if (serve->ending)
			break;
	}

	/* Keeps the part of a frame not yet whole. */
	if (offset != 0) {
		memmove(serve->in, serve->in + offset, serve->in_size - offset);
		serve->in_size -= offset;
	}
	return 0;
}

/*
 * Takes one of the parent's frames.  Returns 0, or EPROTO for a frame
 * against the rules (the link ends).
 */
static int
serve_take(
	struct x11_serve *serve,
	const struct x11_link_frame *frame)
{
	unsigned slot;
	int verdict;
	int error;

	/* The rules, and the HELLO first. */
	verdict = x11_link_check(&serve->table, frame);
	if (verdict == X11_LINK_VIOLATION)
		return EPROTO;
	if (!serve->hello && frame->type != X11_LINK_HELLO)
		return EPROTO;
	if (verdict == X11_LINK_DROP)
		return 0;

	/* What it says. */
	switch (frame->type) {
	case X11_LINK_HELLO:
		error = serve_hello(serve, frame);
		return error;
	case X11_LINK_PING:
		(void)serve_frame(serve, X11_LINK_PONG, 0, NULL, 0);
		return 0;
	case X11_LINK_SHUTDOWN:
		serve->ending = X11_SERVE_ENDED;
		return 0;
	case X11_LINK_OPEN:
		serve_open(serve, frame->id);
		return 0;
	case X11_LINK_CREDIT:
		return 0;
	case X11_LINK_TAB_INPUT:
	case X11_LINK_TAB_CONFIGURE:
	case X11_LINK_TAB_CLOSE:
	case X11_LINK_TAB_REFUSE:
		/* A tab's: the tabs take it (an X server without tabs has none to take). */
		if (serve->system->tab_take == NULL)
			return 0;
		error = serve->system->tab_take(serve->system->context, frame);
		return error;
	case X11_LINK_EOF:
	case X11_LINK_CLOSE:
		/* The remote client went: the core lets it go (an EOF is answered with CLOSE). */
		serve_end_client(serve, frame->id, frame->type == X11_LINK_EOF);
		return 0;
	default:
		break;
	}

	/* DATA: the remote client's bytes for the core. */
	slot = serve_slot(serve, frame->id);
	if (slot < X11_LINK_CHANNELS && serve->server != NULL)
		(void)x11server_input(serve->server, serve->clients[slot], frame->body, frame->size);
	return 0;
}

/*
 * Takes the parent's HELLO (once): the version, the scale, the flags, the
 * cookie, the root window's size and the host's name; then the X server
 * starts.  Returns 0, or EPROTO.
 */
static int
serve_hello(
	struct x11_serve *serve,
	const struct x11_link_frame *frame)
{
	size_t name;
	int error;

	/* Once, of this version, with every fixed field. */
	if (serve->hello || frame->size < SERVE_HELLO_FIXED)
		return EPROTO;
	if (x11_link_get32(frame->body) != X11_LINK_VERSION)
		return EPROTO;

	/* The settings. */
	serve->scale = x11_link_get32(frame->body + 4);
	serve->flags = x11_link_get32(frame->body + 8);
	memcpy(serve->cookie, frame->body + 12, X11_HOST_COOKIE);
	serve->root_width = x11_link_get32(frame->body + SERVE_HELLO_ROOT);
	serve->root_height = x11_link_get32(frame->body + SERVE_HELLO_ROOT + 4U);
	if (serve->root_width > SERVE_ROOT_MAX || serve->root_height > SERVE_ROOT_MAX)
		return EPROTO;
	name = frame->size - SERVE_HELLO_FIXED;
	if (name >= sizeof(serve->host))
		name = sizeof(serve->host) - 1U;
	memcpy(serve->host, frame->body + SERVE_HELLO_FIXED, name);
	serve->host[name] = '\0';
	serve->hello = 1;

	/* The X server. */
	error = serve->system->start(serve->system->context, serve);
	if (error != 0)
		return EPROTO;

	/* Succeeded. */
	return 0;
}

/* Connects an X client the parent opened, which must present the fake cookie; a refused one is closed. */
static void
serve_open(
	struct x11_serve *serve,
	uint32_t id)
{
	struct x11server_auth auth;
	uint32_t client;
	unsigned slot;
	int error;

	/* The slot the table opened. */
	slot = serve_slot(serve, id);
	if (slot >= X11_LINK_CHANNELS || serve->server == NULL)
		return;

	/* The client, with the authorization it must present. */
	auth.name = SERVE_AUTH_NAME;
	auth.data = serve->cookie;
	auth.size = X11_HOST_COOKIE;
	error = x11server_connect(serve->server, &auth, &client);
	if (error != 0) {
		serve_end_client(serve, id, 1);
		return;
	}

	/* Succeeded: the slot's client. */
	serve->clients[slot] = client;
}

/* Lets an X client go: the core hangs it up, the channel closes, and the parent is told when asked. */
static void
serve_end_client(
	struct x11_serve *serve,
	uint32_t id,
	int tell)
{
	unsigned slot;

	/* The core lets the client go. */
	slot = serve_slot(serve, id);
	if (slot >= X11_LINK_CHANNELS)
		return;
	if (serve->server != NULL && serve->clients[slot] != 0)
		x11server_hangup(serve->server, serve->clients[slot]);
	serve->clients[slot] = 0;
	x11_link_close(&serve->table, id);

	/* The parent closes the SSH channel. */
	if (tell)
		(void)serve_frame(serve, X11_LINK_CLOSE, id, NULL, 0);
}

/*
 * Takes the core's events: a client it closed (the parent is told).  Then
 * gives each open client's parent more credit as the core takes its bytes.
 */
static void
serve_events(
	struct x11_serve *serve)
{
	struct x11server_event event;
	unsigned slot;
	uint32_t id;
	int found;

	/* Each event in turn (x11server_next gives 1 with one, 0 when none is left). */
	for (;;) {
		found = x11server_next(serve->server, &event);
		if (found == 0)
			break;
		slot = serve_slot_of_client(serve, event.client);
		if (slot >= X11_LINK_CHANNELS)
			continue;
		id = serve->table.channels[slot].id;

		/* A client the core closed: the channel closes. */
		if (event.type == X11SERVER_EVENT_CLOSED) {
			serve->clients[slot] = 0;
			x11_link_close(&serve->table, id);
			(void)serve_frame(serve, X11_LINK_CLOSE, id, NULL, 0);
		}
	}

	/* Each open client's credit, topped up to what the core may now take of it. */
	for (slot = 0; slot < X11_LINK_CHANNELS; slot++) {
		if (!serve->table.channels[slot].used || serve->clients[slot] == 0)
			continue;
		serve_credit(serve, slot);
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
serve_credit(
	struct x11_serve *serve,
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
	channel = &serve->table.channels[slot];
	held = x11server_held(serve->server, serve->clients[slot], &wanted);

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
	error = serve_frame(serve, X11_LINK_CREDIT, channel->id, credit, 4);
	if (error != 0)
		return;
	x11_link_gave(&serve->table, channel->id, given);
}

/*
 * Sends the clients' replies within the parent's credit, while the frames
 * waiting stay below SERVE_OUT_HIGH.
 */
static void
serve_output(
	struct x11_serve *serve)
{
	const uint8_t *bytes;
	size_t size;
	uint32_t room;
	unsigned slot;
	uint32_t id;
	int error;

	/* Each open client. */
	for (slot = 0; slot < X11_LINK_CHANNELS; slot++) {
		if (!serve->table.channels[slot].used || serve->clients[slot] == 0)
			continue;
		id = serve->table.channels[slot].id;

		/* Its bytes, frame by frame, within the credit. */
		for (;;) {
			if (serve->out_size >= SERVE_OUT_HIGH)
				return;
			bytes = x11server_output(serve->server, serve->clients[slot], &size);
			if (bytes == NULL || size == 0)
				break;
			room = x11_link_room(&serve->table, id);
			if (room == 0)
				break;
			if (size > room)
				size = room;
			if (size > X11_LINK_BODY_MAX)
				size = X11_LINK_BODY_MAX;
			error = serve_frame(serve, X11_LINK_DATA, id, bytes, (uint32_t)size);
			if (error != 0)
				return;
			x11_link_sent(&serve->table, id, (uint32_t)size);
			x11server_written(serve->server, serve->clients[slot], size);
		}
	}
}

/* Queues one frame for the parent.  Returns 0, or ENOMEM. */
static int
serve_frame(
	struct x11_serve *serve,
	unsigned type,
	uint32_t id,
	const void *body,
	uint32_t size)
{
	size_t needed;
	size_t capacity;
	uint8_t *grown;

	/* Room for it. */
	needed = serve->out_size + X11_LINK_HEADER + size;
	if (needed > SERVE_OUT_MAX)
		return ENOMEM;
	if (needed > serve->out_capacity) {
		capacity = serve->out_capacity * 2U;
		if (capacity < needed)
			capacity = needed + SERVE_READ;
		grown = realloc(serve->out, capacity);
		if (grown == NULL)
			return ENOMEM;
		serve->out = grown;
		serve->out_capacity = capacity;
	}

	/* The header and the body. */
	x11_link_header(serve->out + serve->out_size, type, id, size);
	if (size != 0)
		memcpy(serve->out + serve->out_size + X11_LINK_HEADER, body, size);
	serve->out_size = needed;
	return 0;
}

/* Sends the frames waiting, as far as the socket takes them.  Returns 0, or EPIPE. */
static int
serve_send(
	struct x11_serve *serve)
{
	size_t sent;
	int error;

	/* Sends until all went or the socket is full. */
	while (serve->out_size != 0) {
		error = serve->system->send(serve->system->context, serve->out, serve->out_size, &sent);
		if (error == EAGAIN)
			break;
		if (error != 0 || sent == 0)
			return EPIPE;
		memmove(serve->out, serve->out + sent, serve->out_size - sent);
		serve->out_size -= sent;
	}
	return 0;
}

/* Gives the slot of a link id (X11_LINK_CHANNELS: none). */
static unsigned
serve_slot(
	struct x11_serve *serve,
	uint32_t id)
{
	struct x11_link_channel *channel;

	/* The table's slot. */
	channel = x11_link_find(&serve->table, id);
	if (channel == NULL)
		return X11_LINK_CHANNELS;
	return (unsigned)(channel - serve->table.channels);
}

/* Gives the slot of a core's client (X11_LINK_CHANNELS: none). */
static unsigned
serve_slot_of_client(
	struct x11_serve *serve,
	uint32_t client)
{
	unsigned slot;

	/* Looks through the slots. */
	for (slot = 0; slot < X11_LINK_CHANNELS; slot++) {
		if (serve->table.channels[slot].used && serve->clients[slot] == client && client != 0)
			return slot;
	}
	return X11_LINK_CHANNELS;
}
