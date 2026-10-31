/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * kl_game_v1 and kl_game_surface_v1, the game mode's lent images: the
 * compositor lends a fullscreen surface that shows a game or a video the
 * images of the display swapchain it shows, and the client's Vulkan WSI
 * draws straight into them (scanout.c), so nothing is copied or composed.
 *
 * A client's WSI makes one game object for its surface and tells it the
 * chain it draws with (chain: a VkFormat, the uses, the size, the image
 * count, zero when it cannot draw into lent images).  In the game mode the
 * compositor sends each image of its game chain (image: the image's fd)
 * and then the lend (lend: the images' count, size, format, uses and
 * layout).  The client makes a wl_buffer of each image with
 * kl_gpu_buffer_v1, as for its own images, and names it (tag); a lent
 * buffer starts in the compositor's hands, and wl_buffer.release gives it
 * to the client to draw into.  A client that cannot draw into the images
 * declines the lend.  withdraw ends a lend: its buffers stay the client's
 * ordinary buffers, composed as any other.
 *
 * A surface has one game object; a newer one takes the surface from an
 * older one, which hears nothing more.
 */

#include "extras.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

/* The requests of the global and of a surface's object, and the events of a surface's object. */
#define GAME_MANAGER_DESTROY		0U
#define GAME_MANAGER_GET		1U
#define GAME_DESTROY			0U
#define GAME_CHAIN			1U
#define GAME_TAG			2U
#define GAME_DECLINE			3U
#define GAME_EVENT_IMAGE		0U
#define GAME_EVENT_LEND			1U
#define GAME_EVENT_WITHDRAW		2U

static int game_create(struct kwl_object *manager, const unsigned char *bytes, size_t size);
static int game_tag(struct kwl_object *game, const unsigned char *bytes, size_t size);
static uint32_t game_word(const unsigned char *bytes, size_t offset);
static int game_emit(struct kwl_object *game, uint32_t opcode, const uint32_t *words, size_t count);

/*
 * Carries out a request of the global or of a surface's game object.
 * Returns 0, or EPROTO for a malformed request.
 */
int
kwl_game_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *surface;
	int error;

	/* The global: it goes, or it makes a surface's object. */
	if (object->kind == KWL_GAME_MANAGER) {
		if (opcode == GAME_MANAGER_DESTROY && size == 0U) {
			kwl_object_destroy(object);
			return 0;
		}

		/* Only get_game_surface is left. */
		if (opcode != GAME_MANAGER_GET)
			return EPROTO;
		error = game_create(object, bytes, size);
		if (error != 0)
			return error;

		/* Succeeded: the surface has its game object. */
		return 0;
	}

	/* A surface's object, by its request. */
	surface = object->surface;
	switch (opcode) {
	case GAME_DESTROY:
		/* The object goes; a lend to it ends (kwl_game_object_gone). */
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(object);
		return 0;
	case GAME_CHAIN:
		/* The chain the client draws with, five words. */
		if (size != 20U)
			return EPROTO;
		object->game_format = game_word(bytes, 0U);
		object->game_usage = game_word(bytes, 4U);
		object->game_width = game_word(bytes, 8U);
		object->game_height = game_word(bytes, 12U);
		object->game_count = game_word(bytes, 16U);
		if (surface != NULL)
			object->client->server->dirty = 1;
		return 0;
	case GAME_TAG:
		/* A buffer that holds a lent image. */
		error = game_tag(object, bytes, size);
		if (error != 0)
			return error;
		return 0;
	case GAME_DECLINE:
		/* A lend the client will not draw into. */
		if (size != 4U)
			return EPROTO;
		if (surface != NULL)
			kwl_scanout_decline(object->client->server, object, game_word(bytes, 0U));
		return 0;
	default:
		break;
	}

	/* No such request. */
	return EPROTO;
}

/*
 * Unties an object that is going: a game object's surface has none, and a
 * surface's game object names nothing.
 */
void
kwl_game_object_gone(
	struct kwl_object *object)
{
	/* A game object: its surface no longer has one. */
	if (object->kind == KWL_GAME_SURFACE) {
		if (object->surface != NULL && object->surface->game_object == object)
			object->surface->game_object = NULL;
		object->surface = NULL;
		return;
	}

	/* A surface: its game object names nothing. */
	if (object->kind == KWL_SURFACE && object->game_object != NULL) {
		object->game_object->surface = NULL;
		object->game_object = NULL;
	}
}

/*
 * Sends a game object one lent image of the lend serial: its index and
 * its fd (which the call takes).  Returns 0 or the send's errno.
 */
int
kwl_game_send_image(
	struct kwl_object *game,
	uint32_t serial,
	uint32_t index,
	int fd)
{
	uint32_t words[2];
	int error;

	/* A client being retired hears nothing. */
	if (game->client->fatal) {
		close(fd);
		return EPIPE;
	}

	/* The lend, the image's index, and the fd beside them. */
	words[0] = serial;
	words[1] = index;
	error = kwl_emit_fd(game->client, game->id, GAME_EVENT_IMAGE, words, sizeof(words), fd);
	if (error != 0) {
		game->client->fatal = 1;
		game->client->fatal_time = kwl_milliseconds();
		return error;
	}

	/* Succeeded: the image is on its way. */
	return 0;
}

/*
 * Sends a game object the lend serial whose images were all sent: their
 * count, size, VkFormat, uses and layout.  Returns 0 or the send's errno.
 */
int
kwl_game_send_lend(
	struct kwl_object *game,
	uint32_t serial,
	uint32_t count,
	uint32_t width,
	uint32_t height,
	uint32_t format,
	uint32_t usage,
	uint32_t layout)
{
	uint32_t words[7];
	int error;

	/* The lend's seven words. */
	words[0] = serial;
	words[1] = count;
	words[2] = width;
	words[3] = height;
	words[4] = format;
	words[5] = usage;
	words[6] = layout;
	error = game_emit(game, GAME_EVENT_LEND, words, 7U);
	if (error != 0)
		return error;

	/* Succeeded: the lend is offered. */
	return 0;
}

/*
 * Sends a game object the end of the lend serial.  Returns 0 or the send's
 * errno.
 */
int
kwl_game_send_withdraw(
	struct kwl_object *game,
	uint32_t serial)
{
	int error;

	/* The lend that ended. */
	error = game_emit(game, GAME_EVENT_WITHDRAW, &serial, 1U);
	if (error != 0)
		return error;

	/* Succeeded: the client hears that the lend is over. */
	return 0;
}

/* Makes a surface's game object (a newer one takes the surface from an older one). */
static int
game_create(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *surface;
	struct kwl_object *created;
	uint32_t id;
	uint32_t surface_id;

	/* The new ID and the client's surface. */
	if (size != 8U)
		return EPROTO;
	id = game_word(bytes, 0U);
	surface_id = game_word(bytes, 4U);
	surface = kwl_find(manager->client, surface_id);
	if (surface == NULL || surface->kind != KWL_SURFACE)
		return EPROTO;

	/* The object. */
	created = kwl_create(manager->client, id, KWL_GAME_SURFACE, manager->version);
	if (created == NULL)
		return EPROTO;

	/* An older object of the surface lets it go. */
	if (surface->game_object != NULL)
		surface->game_object->surface = NULL;

	/* Succeeded: the object, tied to its surface both ways. */
	created->surface = surface;
	surface->game_object = created;
	return 0;
}

/*
 * Takes a tag: a buffer of the client holds lent image index of the lend
 * serial.  A buffer that is gone or not a GPU image's is passed over (the
 * client's buffer may have gone meanwhile).  Returns 0, or EPROTO for a
 * malformed request.
 */
static int
game_tag(
	struct kwl_object *game,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *buffer;
	uint32_t serial;
	uint32_t index;

	/* The buffer, the lend and the image. */
	if (size != 12U)
		return EPROTO;
	buffer = kwl_find(game->client, game_word(bytes, 0U));
	serial = game_word(bytes, 4U);
	index = game_word(bytes, 8U);

	/* A live GPU buffer of a surface that still has this object. */
	if (game->surface == NULL || buffer == NULL)
		return 0;
	if (buffer->kind != KWL_BUFFER || buffer->dead || buffer->shm != NULL || buffer->import == NULL)
		return 0;

	/* Succeeded: the game mode knows which of its images the buffer holds. */
	kwl_scanout_tag(game->client->server, game, buffer, serial, index);
	return 0;
}

/* Reads a word of a request (the payload need not be aligned). */
static uint32_t
game_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The word's bytes. */
	memcpy(&word, bytes + offset, sizeof(word));

	/* Succeeded: the word. */
	return word;
}

/* Sends an event of words to a game object; a client that cannot take it is retired. */
static int
game_emit(
	struct kwl_object *game,
	uint32_t opcode,
	const uint32_t *words,
	size_t count)
{
	int error;

	/* A client being retired hears nothing. */
	if (game->client->fatal)
		return EPIPE;

	/* The event. */
	error = kwl_emit(game->client, game->id, opcode, words, count * sizeof(words[0]));
	if (error != 0) {
		game->client->fatal = 1;
		game->client->fatal_time = kwl_milliseconds();
		return error;
	}

	/* Succeeded: the event is on its way. */
	return 0;
}
