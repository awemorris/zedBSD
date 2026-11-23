/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The resource ids, the memory each client holds, and XC-MISC.
 *
 * Every window, pixmap, graphics context and font is found by its id in
 * one open-addressing table, so a client may hold as many as its memory
 * allows.  Each resource charges its owner's memory (and the server's)
 * for its own size and the pixels or data behind it; a request that would
 * take a client or the server past its share is refused with BadAlloc,
 * and the client goes on.  XC-MISC tells a client that has used up its
 * ids which ones are free again.
 */

#include "userland/desktop/xserver/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The table's first size (a power of two), and how full it may be (in quarters) before it grows. */
#define RESOURCE_FIRST		256U
#define RESOURCE_FULL_QUARTERS	3U

/* XC-MISC: its version, and its requests. */
#define XCMISC_MAJOR_VERSION	1U
#define XCMISC_MINOR_VERSION	1U
#define XCMISC_GET_VERSION	0U
#define XCMISC_GET_XID_RANGE	1U
#define XCMISC_GET_XID_LIST	2U

/* The most ids one GetXIDList answers (its reply must fit the client's output). */
#define XCMISC_LIST_MAX		(256U * 1024U)

static size_t resource_slot(uint32_t id, size_t capacity);
static int resource_grow(struct x11server *server);
static struct x11_resource *resource_lookup(struct x11server *server, uint32_t id);
static int resource_compare(const void *left, const void *right);
static int resource_used_ids(struct x11server *server, unsigned index, uint32_t **ids, size_t *count);
static unsigned xcmisc_get_version(struct x11server *server, struct x11_client *client, size_t length);
static unsigned xcmisc_get_range(struct x11server *server, unsigned index, size_t length);
static unsigned xcmisc_get_list(struct x11server *server, unsigned index, const uint8_t *request, size_t length);

/*
 * Adds a resource under its id (which must not be in the table).
 * Returns 0, or ENOMEM.
 */
int
x11_resource_add(
	struct x11server *server,
	uint32_t id,
	unsigned type,
	void *object)
{
	struct x11_resource *entry;
	size_t slot;
	int error;

	/* A table with room to stay sparse after this one. */
	if ((server->resource_used + 1U) * 4U > server->resource_capacity * RESOURCE_FULL_QUARTERS) {
		error = resource_grow(server);
		if (error != 0)
			return error;
	}

	/* The first slot of the probe that holds nothing (a slot whose resource went is taken again). */
	slot = resource_slot(id, server->resource_capacity);
	for (;;) {
		entry = &server->resources[slot];
		if (entry->id == 0U)
			break;
		slot = (slot + 1U) & (server->resource_capacity - 1U);
	}

	/* A slot that was never used before is one more in use. */
	if (entry->type != X11_RESOURCE_GONE)
		server->resource_used++;

	/* Succeeded: the resource is found by its id from now on. */
	entry->id = id;
	entry->type = type;
	entry->object = object;
	server->resource_count++;
	return 0;
}

/*
 * Returns the object of an id when it is of the kind asked for (0: any
 * kind), or NULL.
 */
void *
x11_resource_find(
	struct x11server *server,
	uint32_t id,
	unsigned type)
{
	struct x11_resource *entry;

	/* The id's entry. */
	entry = resource_lookup(server, id);
	if (entry == NULL)
		return NULL;

	/* Another kind than asked for. */
	if (type != 0U && entry->type != type)
		return NULL;

	/* Succeeded: the object. */
	return entry->object;
}

/*
 * Returns the kind of resource an id names, or 0 when it names none.
 */
unsigned
x11_resource_type(
	struct x11server *server,
	uint32_t id)
{
	struct x11_resource *entry;

	/* The id's entry. */
	entry = resource_lookup(server, id);
	if (entry == NULL)
		return 0U;

	/* Succeeded: its kind. */
	return entry->type;
}

/*
 * Takes an id out of the table (its object is the caller's to free).
 */
void
x11_resource_remove(
	struct x11server *server,
	uint32_t id)
{
	struct x11_resource *entry;

	/* The id's entry. */
	entry = resource_lookup(server, id);
	if (entry == NULL)
		return;

	/* The slot keeps its place in the probes that pass it. */
	entry->id = 0U;
	entry->type = X11_RESOURCE_GONE;
	entry->object = NULL;
	server->resource_count--;
}

/*
 * Lists the ids a client owns of a kind (0: every kind) into a malloc'd
 * array the caller frees (NULL when there are none).  Returns 0, or
 * ENOMEM when there is no memory for the list.
 */
int
x11_resource_owned(
	struct x11server *server,
	unsigned owner,
	unsigned type,
	uint32_t **ids,
	size_t *count)
{
	struct x11_resource *entry;
	uint32_t base;
	size_t found;
	size_t slot;

	/* The ids under the client's base. */
	*ids = NULL;
	*count = 0U;
	base = (owner + 1U) << X11_ID_SHIFT;
	found = 0U;
	for (slot = 0U; slot < server->resource_capacity; slot++) {
		entry = &server->resources[slot];
		if (entry->id == 0U || (entry->id & ~X11_ID_MASK) != base)
			continue;
		if (type != 0U && entry->type != type)
			continue;
		found++;
	}

	/* None, or no room to list them. */
	if (found == 0U)
		return 0;
	*ids = malloc(found * sizeof(**ids));
	if (*ids == NULL)
		return ENOMEM;

	/* The same ids, listed. */
	for (slot = 0U; slot < server->resource_capacity; slot++) {
		entry = &server->resources[slot];
		if (entry->id == 0U || (entry->id & ~X11_ID_MASK) != base)
			continue;
		if (type != 0U && entry->type != type)
			continue;
		(*ids)[*count] = entry->id;
		(*count)++;
	}

	/* Succeeded: the list. */
	return 0;
}

/*
 * Frees the table when the server ends (every object in it has been freed
 * by its owner's release before).
 */
void
x11_resource_fini(
	struct x11server *server)
{
	/* The table. */
	free(server->resources);
	server->resources = NULL;
	server->resource_capacity = 0U;
	server->resource_used = 0U;
	server->resource_count = 0U;
}

/*
 * Charges a client (X11_NO_CLIENT: the server alone) for bytes it is to
 * hold.  Returns 0, or ENOMEM when that would take the client or the
 * server past its share (nothing is charged then).
 */
int
x11_memory_charge(
	struct x11server *server,
	unsigned owner,
	size_t bytes)
{
	struct x11_client *client;

	/* The server's share. */
	if (bytes > server->server_memory_max - server->memory)
		return ENOMEM;

	/* The client's share, when a client holds it. */
	client = x11_client_of(server, owner);
	if (client != NULL && bytes > server->client_memory_max - client->memory)
		return ENOMEM;

	/* Succeeded: both hold the bytes. */
	server->memory += bytes;
	if (client != NULL)
		client->memory += bytes;
	return 0;
}

/*
 * Gives back bytes a client (X11_NO_CLIENT: the server alone) was charged
 * for.  A client that has gone gave its share back when it went.
 */
void
x11_memory_release(
	struct x11server *server,
	unsigned owner,
	size_t bytes)
{
	struct x11_client *client;

	/* The server's share, never below nothing. */
	if (bytes > server->memory)
		bytes = server->memory;
	server->memory -= bytes;

	/* The client's, when it is still there. */
	client = x11_client_of(server, owner);
	if (client == NULL)
		return;
	if (bytes > client->memory)
		bytes = client->memory;
	client->memory -= bytes;
}

/*
 * XC-MISC: GetVersion, GetXIDRange (the largest run of ids the client may
 * still choose) and GetXIDList (that many free ids).  Returns 0 when it
 * answered, or an error.
 */
unsigned
x11_xcmisc_request(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	unsigned code;

	/* The minor opcode. */
	client = &server->clients[index];

	/* Each request of the extension. */
	switch (request[1]) {
	case XCMISC_GET_VERSION:
		code = xcmisc_get_version(server, client, length);
		break;
	case XCMISC_GET_XID_RANGE:
		code = xcmisc_get_range(server, index, length);
		break;
	case XCMISC_GET_XID_LIST:
		code = xcmisc_get_list(server, index, request, length);
		break;
	default:
		code = X11_BAD_REQUEST;
		break;
	}

	/* Succeeded: answered, or the error to send. */
	return code;
}

/* Returns the first slot of an id's probe in a table of a power-of-two size. */
static size_t
resource_slot(
	uint32_t id,
	size_t capacity)
{
	uint32_t mixed;

	/* The id's bits spread over the word (Knuth's multiplicative hash). */
	mixed = id * 2654435761U;
	mixed ^= mixed >> 16;

	/* Succeeded: the slot. */
	return (size_t)mixed & (capacity - 1U);
}

/*
 * Makes the table twice as large (or as large again, when most of what is
 * in use is slots whose resource went) and puts every resource back.
 * Returns 0, or ENOMEM.
 */
static int
resource_grow(
	struct x11server *server)
{
	struct x11_resource *table;
	struct x11_resource *old;
	size_t capacity;
	size_t old_capacity;
	size_t index;
	size_t slot;

	/* The new size: the first one, twice the old, or the same when the resources fit in half. */
	old = server->resources;
	old_capacity = server->resource_capacity;
	capacity = RESOURCE_FIRST;
	if (old_capacity != 0U) {
		capacity = old_capacity * 2U;
		if (server->resource_count * 4U < old_capacity)
			capacity = old_capacity;
	}

	/* The new table. */
	table = calloc(capacity, sizeof(*table));
	if (table == NULL)
		return ENOMEM;

	/* Each resource at its place in the new table. */
	for (index = 0U; index < old_capacity; index++) {
		if (old[index].id == 0U)
			continue;
		slot = resource_slot(old[index].id, capacity);
		while (table[slot].id != 0U)
			slot = (slot + 1U) & (capacity - 1U);
		table[slot] = old[index];
	}

	/* Succeeded: the new table, with no slot whose resource went. */
	free(old);
	server->resources = table;
	server->resource_capacity = capacity;
	server->resource_used = server->resource_count;
	return 0;
}

/* Returns the entry of an id in the table, or NULL. */
static struct x11_resource *
resource_lookup(
	struct x11server *server,
	uint32_t id)
{
	struct x11_resource *entry;
	size_t slot;

	/* None is never an id, and an empty table has none. */
	if (id == 0U || server->resource_capacity == 0U)
		return NULL;

	/* The probe, up to the first slot that was never used. */
	slot = resource_slot(id, server->resource_capacity);
	for (;;) {
		entry = &server->resources[slot];
		if (entry->id == id)
			break;
		if (entry->id == 0U && entry->type != X11_RESOURCE_GONE)
			return NULL;
		slot = (slot + 1U) & (server->resource_capacity - 1U);
	}

	/* Succeeded: the entry. */
	return entry;
}

/* Orders two ids for qsort. */
static int
resource_compare(
	const void *left,
	const void *right)
{
	uint32_t first;
	uint32_t second;

	/* The two ids. */
	memcpy(&first, left, sizeof(first));
	memcpy(&second, right, sizeof(second));

	/* Smaller first. */
	if (first < second)
		return -1;
	if (first > second)
		return 1;

	/* Succeeded: the same. */
	return 0;
}

/*
 * Lists the ids a client holds, in order, into a malloc'd array (*count of
 * them).  Returns 0, or ENOMEM when there is no memory for the list.
 */
static int
resource_used_ids(
	struct x11server *server,
	unsigned index,
	uint32_t **ids,
	size_t *count)
{
	int error;

	/* The ids it owns, of every kind. */
	error = x11_resource_owned(server, index, 0U, ids, count);
	if (error != 0)
		return error;

	/* Succeeded: in order. */
	if (*count > 1U)
		qsort(*ids, *count, sizeof(**ids), resource_compare);
	return 0;
}

/* XC-MISC GetVersion: the extension's version. */
static unsigned
xcmisc_get_version(
	struct x11server *server,
	struct x11_client *client,
	size_t length)
{
	uint8_t reply[32];

	/* The request is the header and the client's version. */
	if (length != 8U)
		return X11_BAD_LENGTH;

	/* Succeeded: the version the server speaks. */
	memset(reply, 0, sizeof(reply));
	x11_write16(reply + 8, XCMISC_MAJOR_VERSION, client->order);
	x11_write16(reply + 10, XCMISC_MINOR_VERSION, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* XC-MISC GetXIDRange: the largest run of ids the client holds none of (a count of 0 when there is none). */
static unsigned
xcmisc_get_range(
	struct x11server *server,
	unsigned index,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[32];
	uint32_t *ids;
	uint32_t best_start;
	uint32_t best_count;
	uint32_t next;
	uint32_t last;
	uint32_t end;
	size_t count;
	size_t at;
	int error;

	/* The request is the header alone. */
	client = &server->clients[index];
	if (length != 4U)
		return X11_BAD_LENGTH;

	/* The ids in use, in order. */
	error = resource_used_ids(server, index, &ids, &count);
	if (error != 0)
		return X11_BAD_ALLOC;

	/* The largest gap between them, from the client's base to the last id it may choose. */
	best_start = 0U;
	best_count = 0U;
	next = client->base;
	last = client->base | X11_ID_MASK;
	for (at = 0U; at <= count; at++) {
		end = last + 1U;
		if (at < count)
			end = ids[at];
		if (end > next && end - next > best_count) {
			best_start = next;
			best_count = end - next;
		}
		if (at < count)
			next = ids[at] + 1U;
	}
	free(ids);

	/* Succeeded: the run. */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, best_start, client->order);
	x11_write32(reply + 12, best_count, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* XC-MISC GetXIDList: up to the count asked for of the ids the client holds none of, in order. */
static unsigned
xcmisc_get_list(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	uint8_t *reply;
	uint32_t *ids;
	uint32_t wanted;
	uint32_t given;
	uint32_t candidate;
	uint32_t last;
	size_t count;
	size_t at;
	int error;

	/* The request is the header and the count. */
	client = &server->clients[index];
	if (length != 8U)
		return X11_BAD_LENGTH;
	wanted = x11_read32(request + 4, client->order);
	if (wanted > XCMISC_LIST_MAX)
		wanted = XCMISC_LIST_MAX;

	/* The ids in use, in order, and the reply's room. */
	error = resource_used_ids(server, index, &ids, &count);
	if (error != 0)
		return X11_BAD_ALLOC;
	reply = calloc(1U, 32U + (size_t)wanted * 4U);
	if (reply == NULL) {
		free(ids);
		return X11_BAD_ALLOC;
	}

	/* The free ids from the client's base, past each one in use. */
	given = 0U;
	at = 0U;
	candidate = client->base;
	last = client->base | X11_ID_MASK;
	while (given < wanted && candidate <= last) {
		while (at < count && ids[at] < candidate)
			at++;
		if (at < count && ids[at] == candidate) {
			candidate++;
			continue;
		}
		x11_write32(reply + 32 + (size_t)given * 4U, candidate, client->order);
		given++;
		if (candidate == last)
			break;
		candidate++;
	}
	free(ids);

	/* Succeeded: the count and the ids. */
	x11_write32(reply + 8, given, client->order);
	x11_reply(server, client, reply, 32U + (size_t)given * 4U);
	free(reply);
	return 0U;
}
