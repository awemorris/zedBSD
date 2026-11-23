/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The extensions the core answers beside the core protocol: BIG-REQUESTS
 * (here), XC-MISC (resource.c), XKEYBOARD (xkb.c), XInputExtension and
 * the Generic Event Extension (xinput.c), SHAPE (shape-ext.c), SYNC
 * (sync.c), XFIXES (xfixes.c), RANDR (randr.c) and XINERAMA
 * (xinerama.c).  GLX is not in the table: glx.c names it where the
 * server has it.
 *
 * QueryExtension and ListExtensions find them here by name, and a request
 * whose major opcode is one of theirs is handed to its module.  A module
 * of the core's form returns its error to protocol.c, which sends it
 * naming the request's first resource; a module of stage 2 names the
 * value that was wrong (the id or the value) and its error is sent from
 * here.  What a client or a window held in them goes with it.
 */

#include "userland/desktop/xserver/internal.h"

#include <string.h>

/*
 * One extension: its name, its major opcode, its first event and error
 * (0: it has none), and the module that answers its requests, in the
 * core's form (core_handler, its error sent by protocol.c) or in stage
 * 2's (handler, its error sent here); the other is NULL.
 */
struct extension_entry {
	const char *name;
	uint8_t major;
	uint8_t first_event;
	uint8_t first_error;
	unsigned (*core_handler)(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
	unsigned (*handler)(struct x11_ext_request *request);
};

/* BIG-REQUESTS's one request, BigReqEnable. */
#define EXTENSION_BIGREQ_ENABLE		0U

static unsigned extension_big_requests(struct x11server *server, unsigned index, const uint8_t *request, size_t length);

/*
 * The extensions, in the order ListExtensions names them.  Read only,
 * shared by every server of the process.
 */
static const struct extension_entry extension_table[] = {
	{ "BIG-REQUESTS", X11_BIGREQ_MAJOR, 0U, 0U, extension_big_requests, NULL },
	{ "XC-MISC", X11_XCMISC_MAJOR, 0U, 0U, x11_xcmisc_request, NULL },
	{ "XKEYBOARD", X11_XKB_MAJOR, X11_XKB_EVENT, X11_XKB_ERROR, x11_xkb_request, NULL },
	{ "XInputExtension", X11_XINPUT_MAJOR, X11_XINPUT_EVENT, X11_XINPUT_ERROR, x11_xinput_request, NULL },
	{ "Generic Event Extension", X11_GE_MAJOR, 0U, 0U, x11_ge_request, NULL },
	{ "SHAPE", X11_SHAPE_MAJOR, X11_SHAPE_EVENT, 0U, NULL, x11_shape_request },
	{ "SYNC", X11_SYNC_MAJOR, X11_SYNC_EVENT, X11_SYNC_ERROR, NULL, x11_sync_request },
	{ "XFIXES", X11_XFIXES_MAJOR, X11_XFIXES_EVENT, X11_XFIXES_ERROR, NULL, x11_xfixes_request },
	{ "RANDR", X11_RANDR_MAJOR, X11_RANDR_EVENT, X11_RANDR_ERROR, x11_randr_request, NULL },
	{ "XINERAMA", X11_XINERAMA_MAJOR, 0U, 0U, x11_xinerama_request, NULL }
};

/* How many extensions the table has. */
#define EXTENSION_COUNT		(sizeof(extension_table) / sizeof(extension_table[0]))

static const struct extension_entry *extension_of_major(uint8_t major);
static void extension_error(struct x11server *server, struct x11_client *client, unsigned code, uint32_t value, uint8_t major, uint8_t minor);

/*
 * Answers QueryExtension for an extension of the table: the reply's
 * present flag, major opcode, first event and first error are filled in
 * (the reply is 32 bytes, cleared by the caller).  Returns 1 when the
 * name is one of them, 0 when it is not.
 */
int
x11_extension_query(
	const uint8_t *name,
	size_t length,
	uint8_t *reply)
{
	size_t name_length;
	unsigned index;
	int differs;

	/* Each extension by its whole name. */
	for (index = 0U; index < EXTENSION_COUNT; index++) {
		name_length = strlen(extension_table[index].name);
		if (name_length != length)
			continue;
		differs = memcmp(name, extension_table[index].name, length);
		if (differs != 0)
			continue;

		/* Succeeded: present, with its numbers. */
		reply[8] = 1U;
		reply[9] = extension_table[index].major;
		reply[10] = extension_table[index].first_event;
		reply[11] = extension_table[index].first_error;
		return 1;
	}

	/* Not one of them. */
	return 0;
}

/*
 * Writes the extensions' names for ListExtensions, each a counted string,
 * into a list of room bytes, and adds how many to *count.  Returns the
 * bytes written (the names that do not fit are left out).
 */
size_t
x11_extension_names(
	uint8_t *list,
	size_t room,
	unsigned *count)
{
	size_t used;
	size_t name_length;
	unsigned index;

	/* Each name after the last, its length and its bytes. */
	used = 0U;
	for (index = 0U; index < EXTENSION_COUNT; index++) {
		name_length = strlen(extension_table[index].name);
		if (used + 1U + name_length > room)
			break;
		list[used] = (uint8_t)name_length;
		memcpy(list + used + 1U, extension_table[index].name, name_length);
		used += 1U + name_length;
		(*count)++;
	}

	/* Succeeded: the bytes of the names. */
	return used;
}

/*
 * Answers a request of an extension of the table by its major opcode.
 * The error of a module of the core's form is returned; that of a module
 * of stage 2 is sent here, naming the value the module gave.  Returns 0
 * (answered or refused here), the error to send, or BadRequest for a
 * major opcode that is none of theirs.
 */
unsigned
x11_extension_request(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	const struct extension_entry *entry;
	struct x11_ext_request context;
	unsigned code;

	/* The extension of the major opcode. */
	entry = extension_of_major(request[0]);
	if (entry == NULL)
		return X11_BAD_REQUEST;

	/* A module of the core's form: its error goes back to protocol.c. */
	if (entry->core_handler != NULL) {
		code = entry->core_handler(server, index, request, length);
		return code;
	}

	/* The request as the module reads it. */
	memset(&context, 0, sizeof(context));
	context.server = server;
	context.index = index;
	context.client = &server->clients[index];
	context.bytes = request;
	context.length = length;
	context.msb = context.client->order;

	/* The module's answer. */
	code = entry->handler(&context);
	if (code == 0U)
		return 0U;

	/* A client that went with its request hears nothing. */
	if (context.client->id == 0U)
		return 0U;

	/* Succeeded: refused, with the value the module named. */
	extension_error(server, context.client, code, context.bad, request[0], request[1]);
	return 0U;
}

/*
 * Sends an extension's event (32 bytes, already in the client's byte
 * order) with the client's last sequence number.
 */
void
x11_extension_event(
	struct x11server *server,
	struct x11_client *client,
	uint8_t *event)
{
	/* The sequence number of the last request the client sent. */
	x11_write16(event + 2, client->sequence, client->order);

	/* Succeeded: queued. */
	x11_client_send(server, client, event, 32U);
}

/*
 * Forgets what a closed client held in the extensions: its regions,
 * counters, alarms, fences and barriers, and the events it asked for.
 */
void
x11_extension_forget_client(
	struct x11server *server,
	unsigned index)
{
	/* SHAPE's events it asked for. */
	x11_shape_forget_client(server, index);

	/* XFIXES's regions, barriers and events. */
	x11_xfixes_forget_client(server, index);

	/* RANDR's events it asked for. */
	x11_randr_forget_client(server, index);

	/* Succeeded: SYNC's counters, alarms, fences and waits. */
	x11_sync_forget_client(server, index);
}

/*
 * Forgets what the extensions held of a window going away: its shapes
 * and the events clients asked for on it.
 */
void
x11_extension_forget_window(
	struct x11server *server,
	struct x11_window *window)
{
	/* Its shapes. */
	x11_shape_forget_window(server, window);

	/* XFIXES's events on it. */
	x11_xfixes_forget_window(server, window);

	/* Succeeded: RANDR's events on it. */
	x11_randr_forget_window(server, window->id);
}

/*
 * Does the extensions' work that time brings: SYNC's waits and alarms on
 * the server's time, and the window manager's resizes that waited too
 * long for a client's counter.  *timed is nonzero while such work waits
 * for a later time.  Returns nonzero when a client may go on with its
 * requests (a wait of it ended).
 */
int
x11_extension_tick(
	struct x11server *server,
	int *timed)
{
	int sync_timed;
	int woke;
	int waiting;

	/* SYNC's waits and alarms. */
	sync_timed = 0;
	woke = x11_sync_tick(server, &sync_timed);

	/* The window manager's resizes held for a counter. */
	waiting = x11_wm_sync_tick(server);

	/* XFIXES's clients told of a new cursor shown. */
	x11_xfixes_cursor_tick(server);

	/* Succeeded: whether work waits for a later time, and whether a client woke. */
	*timed = 0;
	if (sync_timed || waiting)
		*timed = 1;
	return woke;
}

/*
 * Frees the extensions' state when the server ends (after every client
 * is closed).
 */
void
x11_extension_fini(
	struct x11server *server)
{
	/* XFIXES's state. */
	x11_xfixes_fini(server);

	/* RANDR's selections. */
	x11_randr_fini(server);

	/* Succeeded: SYNC's state. */
	x11_sync_fini(server);
}

/*
 * BIG-REQUESTS: BigReqEnable answers the longest request in words (the
 * input the core holds for a client) and lets the client send requests of
 * length 0 with a 32-bit length.
 */
static unsigned
extension_big_requests(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[32];

	/* BigReqEnable is the only request, the header alone. */
	client = &server->clients[index];
	if (request[1] != EXTENSION_BIGREQ_ENABLE)
		return X11_BAD_REQUEST;
	if (length != 4U)
		return X11_BAD_LENGTH;

	/* Succeeded: the longest request, and big requests from now on. */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, X11_INPUT_CAP / 4U, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	client->big_requests = 1;
	return 0U;
}

/* Returns the extension of a major opcode, or NULL. */
static const struct extension_entry *
extension_of_major(
	uint8_t major)
{
	unsigned index;

	/* Each extension. */
	for (index = 0U; index < EXTENSION_COUNT; index++) {
		if (extension_table[index].major == major)
			return &extension_table[index];
	}

	/* None has it. */
	return NULL;
}

/* Sends an error in the client's byte order: its code, the sequence number, the value, and the minor and major opcodes. */
static void
extension_error(
	struct x11server *server,
	struct x11_client *client,
	unsigned code,
	uint32_t value,
	uint8_t major,
	uint8_t minor)
{
	uint8_t error[32];

	/* The error. */
	memset(error, 0, sizeof(error));
	error[1] = (uint8_t)code;
	x11_write16(error + 2, client->sequence, client->order);
	x11_write32(error + 4, value, client->order);
	x11_write16(error + 8, minor, client->order);
	error[10] = major;

	/* Succeeded: queued. */
	x11_client_send(server, client, error, sizeof(error));
}
