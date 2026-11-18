/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The primary selection between clients (ws035-p100):
 * zwp_primary_selection_device_manager_v1 (version 1) and its source,
 * device and offer, from wayland-protocols' primary-selection-unstable-v1.
 *
 * It is the selection X11 calls PRIMARY: a client that selects text makes a
 * source with the text's types and sets it as the primary selection; the
 * client with the keyboard is told it (a new offer with the types, then the
 * selection event), and is told again whenever it changes while it has the
 * keyboard; a client that gets the keyboard is told first.  A middle click
 * pastes it: the client asks the offer to receive a type into a descriptor,
 * and the source's client is asked to send that type into it.  It works like
 * the clipboard (data.c) but is a selection of its own, and it has no drag
 * and drop.
 */

#include "data.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The requests of zwp_primary_selection_device_manager_v1. */
#define MANAGER_CREATE_SOURCE	0U
#define MANAGER_GET_DEVICE	1U
#define MANAGER_DESTROY		2U

/* The requests and events of zwp_primary_selection_device_v1. */
#define DEVICE_SET_SELECTION	0U
#define DEVICE_DESTROY		1U
#define DEVICE_DATA_OFFER	0U
#define DEVICE_SELECTION	1U

/* The requests and events of zwp_primary_selection_offer_v1. */
#define OFFER_RECEIVE		0U
#define OFFER_DESTROY		1U
#define OFFER_OFFER		0U

/* The requests and events of zwp_primary_selection_source_v1. */
#define SOURCE_OFFER		0U
#define SOURCE_DESTROY		1U
#define SOURCE_SEND		0U
#define SOURCE_CANCELLED	1U

/* The most MIME types one source may offer, and the longest type (with its NUL). */
#define PRIMARY_MIME_MAX	32U
#define PRIMARY_MIME_LENGTH	256U

static int manager_request(struct kwl_object *manager, uint32_t opcode, const unsigned char *bytes, size_t size);
static int source_request(struct kwl_object *source, uint32_t opcode, const unsigned char *bytes, size_t size);
static int device_request(struct kwl_object *device, uint32_t opcode, const unsigned char *bytes, size_t size);
static int offer_request(struct kwl_object *offer, uint32_t opcode, const unsigned char *bytes, size_t size);
static int set_selection(struct kwl_object *device, uint32_t source_id);
static void selection_changed(struct kwl_server *server);
static void send_selection(struct kwl_server *server, struct kwl_client *client);
static void send_device_selection(struct kwl_server *server, struct kwl_object *device);
static uint32_t primary_word(const unsigned char *bytes, size_t offset);

/*
 * Carries out a request of one of the primary selection's interfaces.
 */
int
kwl_primary_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	int error;

	/* Each interface has its own requests. */
	switch (object->kind) {
	case KWL_PRIMARY_MANAGER:
		error = manager_request(object, opcode, bytes, size);
		break;
	case KWL_PRIMARY_SOURCE:
		error = source_request(object, opcode, bytes, size);
		break;
	case KWL_PRIMARY_DEVICE:
		error = device_request(object, opcode, bytes, size);
		break;
	case KWL_PRIMARY_OFFER:
		error = offer_request(object, opcode, bytes, size);
		break;
	default:
		error = EPROTO;
		break;
	}

	/* Reports a request that was refused (EAGAIN: its descriptor has not come yet). */
	if (error != 0)
		return error;

	/* Succeeded: the request was carried out. */
	return 0;
}

/*
 * Tells the client that is getting the keyboard the primary selection (a
 * client that was told it last is not told again).
 */
void
kwl_primary_focus(
	struct kwl_server *server,
	struct kwl_object *focus)
{
	/* No new focus, or the client told last. */
	if (focus == NULL || focus->client->number == server->primary_client)
		return;

	/* The new focus's client hears the primary selection. */
	send_selection(server, focus->client);
}

/*
 * Unties an object that is going from the primary selection: a source's
 * types are freed, its offers lose it, and when it was the selection the
 * selection is empty (the keyboard's client is told).
 */
void
kwl_primary_object_gone(
	struct kwl_object *object)
{
	struct kwl_server *server;
	struct kwl_client *client;
	struct kwl_object *other;
	unsigned index;

	/* Only a source has anything to untie. */
	if (object->kind != KWL_PRIMARY_SOURCE)
		return;
	server = object->client->server;

	/* Its types. */
	for (index = 0; index < object->mime_count; index++)
		free(object->mime_types[index]);
	free(object->mime_types);
	object->mime_types = NULL;
	object->mime_count = 0;

	/* The offers made from it have no source any more. */
	for (client = server->clients; client != NULL; client = client->next) {
		for (other = client->objects; other != NULL; other = other->next) {
			if (other->kind == KWL_PRIMARY_OFFER && other->data_source == object)
				other->data_source = NULL;
		}
	}

	/* The selection it was: the primary selection is empty now. */
	if (server->primary == object) {
		server->primary = NULL;
		printf("KWL PRIMARY selection none (source gone)\n");
		selection_changed(server);
	}
}

/* Carries out a request of the manager: a new source, a seat's device, or destroy. */
static int
manager_request(
	struct kwl_object *manager,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *created;
	struct kwl_object *seat;
	struct kwl_server *server;
	uint32_t id;
	uint32_t seat_id;

	/* The manager goes; what it made stays. */
	if (opcode == MANAGER_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(manager);
		return 0;
	}

	/* A source: its new ID. */
	if (opcode == MANAGER_CREATE_SOURCE) {
		if (size != 4U)
			return EPROTO;
		id = primary_word(bytes, 0U);
		created = kwl_create(manager->client, id, KWL_PRIMARY_SOURCE, manager->version);
		if (created == NULL)
			return EPROTO;
		return 0;
	}

	/* Only get_device is left: its new ID and the client's seat. */
	if (opcode != MANAGER_GET_DEVICE || size != 8U)
		return EPROTO;
	id = primary_word(bytes, 0U);
	seat_id = primary_word(bytes, 4U);
	seat = kwl_find(manager->client, seat_id);
	if (seat == NULL || seat->kind != KWL_SEAT)
		return EPROTO;
	created = kwl_create(manager->client, id, KWL_PRIMARY_DEVICE, manager->version);
	if (created == NULL)
		return EPROTO;

	/* A device of the client with the keyboard hears the primary selection at once. */
	server = manager->client->server;
	if (server->focus != NULL && server->focus->client == manager->client)
		send_device_selection(server, created);

	/* Succeeded: the client has a primary selection device. */
	return 0;
}

/* Carries out a request of a source: a type offered, or destroy. */
static int
source_request(
	struct kwl_object *source,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	const char *text;
	char **types;
	size_t next;
	size_t length;
	int error;

	/* The source goes (kwl_primary_object_gone empties the selection when it held it). */
	if (opcode == SOURCE_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(source);
		return 0;
	}

	/* Only offer is left: one MIME type. */
	if (opcode != SOURCE_OFFER)
		return EPROTO;
	error = kwl_data_read_string(bytes, size, 0U, &text, &next);
	if (error != 0 || next != size)
		return EPROTO;

	/* A type too long, or one too many, is left out. */
	length = strlen(text);
	if (length >= PRIMARY_MIME_LENGTH || source->mime_count == PRIMARY_MIME_MAX)
		return 0;

	/* One more slot for it. */
	types = realloc(source->mime_types, (source->mime_count + 1U) * sizeof(*types));
	if (types == NULL)
		return 0;
	source->mime_types = types;

	/* Its copy. */
	types[source->mime_count] = strdup(text);
	if (types[source->mime_count] == NULL)
		return 0;
	source->mime_count++;

	/* Succeeded: the source has the type. */
	return 0;
}

/* Carries out a request of a device: the selection, or destroy. */
static int
device_request(
	struct kwl_object *device,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	uint32_t source_id;
	int error;

	/* The device goes. */
	if (opcode == DEVICE_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(device);
		return 0;
	}

	/* Only set_selection is left: a source or none, and the serial of the input that asked. */
	if (opcode != DEVICE_SET_SELECTION || size != 8U)
		return EPROTO;
	source_id = primary_word(bytes, 0U);
	error = set_selection(device, source_id);
	if (error != 0)
		return error;

	/* Succeeded: the primary selection is set. */
	return 0;
}

/* Carries out a request of an offer: receive a type into a descriptor, or destroy. */
static int
offer_request(
	struct kwl_object *offer,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *source;
	const char *text;
	size_t next;
	int descriptor;
	int error;

	/* The offer goes. */
	if (opcode == OFFER_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(offer);
		return 0;
	}

	/* Only receive is left: the type, and the descriptor beside the message. */
	if (opcode != OFFER_RECEIVE)
		return EPROTO;
	error = kwl_data_read_string(bytes, size, 0U, &text, &next);
	if (error != 0 || next != size)
		return EPROTO;
	descriptor = kwl_take_fd(offer->client);
	if (descriptor < 0)
		return EAGAIN;

	/* An offer whose source has gone has nothing to send; the descriptor is closed (the reader sees its end). */
	source = offer->data_source;
	if (source == NULL || source->dead || source->client->fatal) {
		close(descriptor);
		printf("KWL PRIMARY receive client=%llu mime=%s source=none\n", (unsigned long long)offer->client->number, text);
		return 0;
	}

	/* Succeeded: the source's client writes the type into the descriptor (the event carries it away). */
	printf("KWL PRIMARY receive client=%llu mime=%s source=%llu\n", (unsigned long long)offer->client->number, text, (unsigned long long)source->client->number);
	(void)kwl_data_emit_string(source->client, source->id, SOURCE_SEND, text, descriptor);
	return 0;
}

/* Sets the primary selection from a client's device: a source of the client, or none; the replaced source is cancelled. */
static int
set_selection(
	struct kwl_object *device,
	uint32_t source_id)
{
	struct kwl_server *server;
	struct kwl_object *source;
	struct kwl_object *previous;
	unsigned types;

	/* The source must be one of the client's. */
	source = NULL;
	if (source_id != 0U) {
		source = kwl_find(device->client, source_id);
		if (source == NULL || source->kind != KWL_PRIMARY_SOURCE)
			return EPROTO;
	}

	/* An unchanged selection tells nobody. */
	server = device->client->server;
	previous = server->primary;
	if (previous == source)
		return 0;

	/* The source replaced hears that it is not the selection any more. */
	if (previous != NULL && !previous->dead)
		(void)kwl_emit(previous->client, previous->id, SOURCE_CANCELLED, NULL, 0U);

	/* The new selection (with how many types it has), and the client with the keyboard hears it. */
	server->primary = source;
	types = 0;
	if (source != NULL)
		types = source->mime_count;
	printf("KWL PRIMARY selection client=%llu source=%u types=%u\n", (unsigned long long)device->client->number, source_id, types);
	selection_changed(server);

	/* Succeeded: the primary selection holds the source. */
	return 0;
}

/* Tells the client with the keyboard that the primary selection changed. */
static void
selection_changed(
	struct kwl_server *server)
{
	/* Without a focus nobody is told now (the next focus is). */
	server->primary_client = 0;
	if (server->focus == NULL || server->focus->dead)
		return;

	/* The focused client hears it. */
	send_selection(server, server->focus->client);
}

/* Tells every primary selection device of a client the selection. */
static void
send_selection(
	struct kwl_server *server,
	struct kwl_client *client)
{
	struct kwl_object *device;

	/* A failed client hears nothing. */
	if (client->fatal)
		return;

	/* Each live device. */
	for (device = client->objects; device != NULL; device = device->next) {
		if (device->kind == KWL_PRIMARY_DEVICE && !device->dead)
			send_device_selection(server, device);
	}

	/* The client has been told. */
	server->primary_client = client->number;
}

/*
 * Tells one device the primary selection: a new offer with the source's
 * types and the selection event naming it, or the selection event naming
 * none.
 */
static void
send_device_selection(
	struct kwl_server *server,
	struct kwl_object *device)
{
	struct kwl_object *source;
	struct kwl_object *offer;
	uint32_t word;
	unsigned index;

	/* An empty selection names no offer. */
	source = server->primary;
	word = 0;
	if (source == NULL || source->dead) {
		(void)kwl_emit(device->client, device->id, DEVICE_SELECTION, &word, sizeof(word));
		return;
	}

	/* The offer, made by the compositor. */
	offer = kwl_create_server(device->client, KWL_PRIMARY_OFFER, device->version);
	if (offer == NULL)
		return;
	offer->data_source = source;

	/* It is introduced, with each of the source's types. */
	word = offer->id;
	(void)kwl_emit(device->client, device->id, DEVICE_DATA_OFFER, &word, sizeof(word));
	for (index = 0; index < source->mime_count; index++)
		(void)kwl_data_emit_string(device->client, offer->id, OFFER_OFFER, source->mime_types[index], -1);

	/* Succeeded: the selection names it. */
	(void)kwl_emit(device->client, device->id, DEVICE_SELECTION, &word, sizeof(word));
	printf("KWL PRIMARY offer client=%llu offer=%u types=%u\n", (unsigned long long)device->client->number, offer->id, source->mime_count);
}

/* Reads one native-endian protocol word. */
static uint32_t
primary_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The payload need not be aligned. */
	memcpy(&word, bytes + offset, sizeof(word));

	/* Succeeded: the word. */
	return word;
}
