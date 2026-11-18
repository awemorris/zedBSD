/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * kl_keyboard_inset_v1 (ws102-p015, plan/ws102/design.md section 2.8):
 * a window that asks for it (kl_keyboard_inset_manager_v1.get_inset,
 * libkeiland does) hears how much of it the on-screen keyboard covers, in its
 * own pixels from its right and bottom edges, whenever the keyboard opens,
 * closes or changes the windows (the work area of ws102-p007 calls this
 * before its configure).  A window can then keep what matters -- the
 * caret -- in the part it can be seen in.  A window that does not ask
 * hears only its configures, as before.
 */

#include "inset.h"
#include "extras.h"
#include "popup.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The requests of kl_keyboard_inset_manager_v1 and of kl_keyboard_inset_v1, and the event. */
#define INSET_MANAGER_DESTROY		0U
#define INSET_MANAGER_GET_INSET		1U
#define INSET_DESTROY			0U
#define INSET_EVENT_INSET		0U

static int inset_create(struct kwl_object *manager, const unsigned char *bytes, size_t size);
static void inset_covered(struct kwl_server *server, struct kwl_object *surface, const int32_t *panel, int32_t *right, int32_t *bottom);
static uint32_t inset_word(const unsigned char *bytes, size_t offset);

/*
 * Carries out a request of kl_keyboard_inset_manager_v1 or of a
 * window's kl_keyboard_inset_v1.
 */
int
kwl_inset_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	int error;

	/* The manager: it goes, or it gives a window its inset. */
	if (object->kind == KWL_KEYBOARD_INSET_MANAGER) {
		if (opcode == INSET_MANAGER_DESTROY && size == 0U) {
			kwl_object_destroy(object);
			return 0;
		}

		/* Only get_inset is left. */
		if (opcode != INSET_MANAGER_GET_INSET)
			return EPROTO;
		error = inset_create(object, bytes, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* The inset's only request is destroy. */
	if (opcode != INSET_DESTROY || size != 0U)
		return EPROTO;
	kwl_object_destroy(object);

	/* Succeeded: the window hears no more. */
	return 0;
}

/*
 * Forgets a window's toplevel that goes: its insets name nothing from now
 * on (and are told nothing).
 */
void
kwl_inset_object_gone(
	struct kwl_object *object)
{
	struct kwl_object *other;

	/* Only a toplevel is named by an inset. */
	if (object->kind != KWL_TOPLEVEL)
		return;

	/* Each inset of the client that names it. */
	for (other = object->client->objects; other != NULL; other = other->next) {
		if (other->kind == KWL_KEYBOARD_INSET && other->top == object)
			other->top = NULL;
	}
}

/*
 * Tells every window with an inset how much of it the keyboard covers:
 * panel is the keyboard's rectangle on the output (x, y, width, height),
 * NULL when the keyboard has closed (everything 0).  Called when the
 * keyboard opens or closes, and by the work area before its configures.
 */
void
kwl_keyboard_inset_notify(
	struct kwl_server *server,
	const int32_t *panel)
{
	struct kwl_client *client;
	struct kwl_object *object;
	struct kwl_object *surface;
	int32_t words[3];
	uint32_t reason;
	int32_t right;
	int32_t bottom;

	/* The reason: no keyboard, the right column, or the bottom row (the wider than tall panel). */
	reason = KWL_INSET_REASON_NONE;
	if (panel != NULL && panel[2] >= panel[3])
		reason = KWL_INSET_REASON_BOTTOM;
	else if (panel != NULL)
		reason = KWL_INSET_REASON_RIGHT;

	/* Each live client's insets. */
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (object = client->objects; object != NULL; object = object->next) {
			/* An inset whose window is still there. */
			if (object->kind != KWL_KEYBOARD_INSET || object->dead || object->top == NULL)
				continue;
			surface = object->top->surface;
			if (surface == NULL || surface->dead)
				continue;

			/* What the keyboard covers of it. */
			right = 0;
			bottom = 0;
			if (panel != NULL)
				inset_covered(server, surface, panel, &right, &bottom);

			/* The event, and the line the tests read. */
			words[0] = right;
			words[1] = bottom;
			words[2] = (int32_t)reason;
			(void)kwl_emit(client, object->id, INSET_EVENT_INSET, words, sizeof(words));
			printf("KWL INSET client=%llu surface=%u right=%d bottom=%d reason=%u\n", (unsigned long long)client->number, surface->id, right, bottom, reason);
		}
	}
}

/* Makes a window's inset (get_inset: the new ID and the window's xdg_toplevel). */
static int
inset_create(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *toplevel;
	struct kwl_object *created;
	uint32_t id;

	/* The new ID and one of the client's toplevels. */
	if (size != 8U)
		return EPROTO;
	id = inset_word(bytes, 0U);
	toplevel = kwl_find(manager->client, inset_word(bytes, 4U));
	if (toplevel == NULL || toplevel->kind != KWL_TOPLEVEL)
		return EPROTO;

	/* The inset, naming its window. */
	created = kwl_create(manager->client, id, KWL_KEYBOARD_INSET, manager->version);
	if (created == NULL)
		return EPROTO;
	created->top = toplevel;
	printf("KWL INSET create client=%llu inset=%u toplevel=%u\n", (unsigned long long)manager->client->number, id, toplevel->id);

	/* Succeeded: the window hears the keyboard from now on. */
	return 0;
}

/*
 * Works out how much of a window's body a keyboard panel covers, in the
 * window's pixels: from its right edge for the right column, from its
 * bottom edge for the bottom row.  A window not shown, or not under the
 * panel, is not covered.
 */
static void
inset_covered(
	struct kwl_server *server,
	struct kwl_object *surface,
	const int32_t *panel,
	int32_t *right,
	int32_t *bottom)
{
	uint32_t width;
	uint32_t height;
	int32_t x;
	int32_t y;

	/* Only a window shown on the desktop shown. */
	if (!surface->mapped || surface->minimized || surface->desktop != server->desktop)
		return;

	/* Where its body is, and its size. */
	x = surface->x;
	y = surface->y;
	if (server->glass)
		(void)kwl_glass_body_origin(server, surface, &x, &y);
	kwl_surface_size(surface, &width, &height);

	/* A docked window: the size it is being told (the work area's, ws102-p007), not its image's yet. */
	if (surface->maximized && surface->window_width != 0U) {
		width = surface->window_width;
		height = surface->window_height;
	}

	/* Not under the panel at all. */
	if (panel[0] >= x + (int32_t)width || panel[0] + panel[2] <= x)
		return;
	if (panel[1] >= y + (int32_t)height || panel[1] + panel[3] <= y)
		return;

	/* The bottom row covers it from its bottom edge up to the panel's top. */
	if (panel[2] >= panel[3]) {
		*bottom = y + (int32_t)height - panel[1];
		if (*bottom > (int32_t)height)
			*bottom = (int32_t)height;
		return;
	}

	/* The right column covers it from its right edge left to the panel's left. */
	*right = x + (int32_t)width - panel[0];
	if (*right > (int32_t)width)
		*right = (int32_t)width;
}

/* Reads one native-endian protocol word. */
static uint32_t
inset_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The caller has checked the payload's size. */
	memcpy(&word, bytes + offset, sizeof(word));
	return word;
}
