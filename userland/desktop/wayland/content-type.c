/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * wp_content_type_v1 (ws122-p005b): a client tells what a surface shows --
 * nothing in particular, a photo, a video or a game.  The compositor's
 * game mode (scanout.c) shows a fullscreen video or game without composing
 * it.
 *
 * The type is double-buffered: set_content_type changes the pending type,
 * and the surface's commit applies it.  A content type object that goes
 * sets the pending type back to none.  One object per surface.
 */

#include "extras.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The requests of the manager and of a surface's object. */
#define CONTENT_MANAGER_DESTROY		0U
#define CONTENT_MANAGER_GET		1U
#define CONTENT_DESTROY			0U
#define CONTENT_SET			1U

/* The manager's error: the surface has its object already. */
#define CONTENT_ERROR_ALREADY_CONSTRUCTED	0U

/* The types, and the largest. */
#define CONTENT_TYPE_GAME		3U

static int content_create(struct kwl_object *manager, const unsigned char *bytes, size_t size);
static uint32_t content_word(const unsigned char *bytes, size_t offset);
static const char *content_name(uint32_t type);

/*
 * Carries out a request of the manager or of a surface's content type
 * object.  Returns 0, or EPROTO for a malformed request.
 */
int
kwl_content_type_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *surface;
	uint32_t type;
	int error;

	/* The manager: it goes, or it makes a surface's object. */
	if (object->kind == KWL_CONTENT_TYPE_MANAGER) {
		if (opcode == CONTENT_MANAGER_DESTROY && size == 0U) {
			kwl_object_destroy(object);
			return 0;
		}

		/* Only get_surface_content_type is left. */
		if (opcode != CONTENT_MANAGER_GET)
			return EPROTO;
		error = content_create(object, bytes, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* The object goes; the surface's next commit shows no type (kwl_content_type_object_gone). */
	if (opcode == CONTENT_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(object);
		return 0;
	}

	/* Only set_content_type is left, with one word. */
	if (opcode != CONTENT_SET || size != 4U)
		return EPROTO;

	/* A type the protocol has; anything else is none. */
	type = content_word(bytes, 0U);
	if (type > CONTENT_TYPE_GAME)
		type = 0U;

	/* The pending type of the surface, when it is still there. */
	surface = object->surface;
	if (surface == NULL)
		return 0;
	surface->pending_content_type = type;
	surface->content_type_changed = 1U;

	/* Succeeded: the next commit applies it. */
	return 0;
}

/*
 * Applies a surface's pending content type with its commit.
 */
void
kwl_content_type_commit(
	struct kwl_object *surface)
{
	/* Nothing changed since the last commit. */
	if (!surface->content_type_changed)
		return;

	/* The pending type becomes the surface's. */
	surface->content_type_changed = 0U;
	if (surface->content_type == surface->pending_content_type)
		return;
	surface->content_type = surface->pending_content_type;
	surface->client->server->dirty = 1;
	printf("KWL CONTENT surface=%u type=%s client=%llu\n", surface->id, content_name(surface->content_type), (unsigned long long)surface->client->number);
}

/*
 * Unties an object that is going: a content type object's surface goes
 * back to none with its next commit; a surface's object names nothing.
 */
void
kwl_content_type_object_gone(
	struct kwl_object *object)
{
	struct kwl_object *surface;

	/* A content type object: its surface's pending type is none. */
	if (object->kind == KWL_CONTENT_TYPE) {
		surface = object->surface;
		object->surface = NULL;
		if (surface == NULL)
			return;
		surface->content_type_object = NULL;
		surface->pending_content_type = 0U;
		surface->content_type_changed = 1U;
		return;
	}

	/* A surface: its object names nothing. */
	if (object->kind == KWL_SURFACE && object->content_type_object != NULL) {
		object->content_type_object->surface = NULL;
		object->content_type_object = NULL;
	}
}

/* Makes a surface's content type object (one per surface). */
static int
content_create(
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
	id = content_word(bytes, 0U);
	surface_id = content_word(bytes, 4U);
	surface = kwl_find(manager->client, surface_id);
	if (surface == NULL || surface->kind != KWL_SURFACE)
		return EPROTO;

	/* One object per surface. */
	if (surface->content_type_object != NULL) {
		(void)kwl_error_code(manager->client, manager->id, CONTENT_ERROR_ALREADY_CONSTRUCTED, "the surface has a content type object");
		return EPROTO;
	}

	/* Succeeded: the object, tied to its surface both ways. */
	created = kwl_create(manager->client, id, KWL_CONTENT_TYPE, manager->version);
	if (created == NULL)
		return EPROTO;
	created->surface = surface;
	surface->content_type_object = created;
	return 0;
}

/* Reads a word of a request. */
static uint32_t
content_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The payload need not be aligned. */
	memcpy(&word, bytes + offset, sizeof(word));

	/* Succeeded: the word. */
	return word;
}

/* Names a content type for the log. */
static const char *
content_name(
	uint32_t type)
{
	/* Each type of the protocol. */
	switch (type) {
	case 1U:
		return "photo";
	case 2U:
		return "video";
	case 3U:
		return "game";
	default:
		break;
	}

	/* None. */
	return "none";
}
