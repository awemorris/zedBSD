/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The server side of the System Menu protocol (WS070, plan/ws070/design.md
 * section 2): xdg_menu_manager_v1, xdg_menu_v1 and xdg_toplevel_menu_v1.
 *
 * A client builds a menu model (a tree of numbered items) in transactions:
 * begin_update copies the shown model, the changes go to the copy, and
 * commit shows the copy at once.  An xdg_toplevel_menu_v1 ties a model to a
 * window; several windows may show one model.  What the user chooses comes
 * back as xdg_toplevel_menu_v1.activated.  Drawing and input are in
 * menu-shell.c.
 */

#include "desktop.h"
#include "menu.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The bounds of one model: its items, how deep its tree is, and its strings in bytes. */
#define MENU_ITEMS_MAX		1024U
#define MENU_DEPTH_MAX		8U
#define MENU_TEXT_MAX		255U

/* The highest role and the modifier bits a shortcut may name. */
#define MENU_ROLE_LAST		19U
#define MENU_MODIFIERS_ALL	15U

/* xdg_menu_manager_v1's requests and error. */
#define MANAGER_DESTROY			0U
#define MANAGER_CREATE_MENU		1U
#define MANAGER_GET_TOPLEVEL_MENU	2U
#define MANAGER_GET_CONTEXT_MENU	3U
#define MANAGER_ERROR_ALREADY_EXISTS	0U
#define MANAGER_ERROR_BAD_SERIAL	1U

/* The requests and events of xdg_context_menu_v1 (version 2). */
#define CONTEXT_DESTROY		0U
#define CONTEXT_ACTIVATED	0U
#define CONTEXT_DONE		1U

/* xdg_menu_v1's requests. */
#define MENU_DESTROY		0U
#define MENU_BEGIN_UPDATE	1U
#define MENU_COMMIT		2U
#define MENU_APPEND_ITEM	3U
#define MENU_INSERT_ITEM	4U
#define MENU_REMOVE_ITEM	5U
#define MENU_SET_LABEL		6U
#define MENU_SET_ACTION		7U
#define MENU_SET_ENABLED	8U
#define MENU_SET_VISIBLE	9U
#define MENU_SET_CHECKED	10U
#define MENU_SET_ROLE		11U
#define MENU_SET_ICON_NAME	12U
#define MENU_SET_SHORTCUT	13U

/* xdg_menu_v1's errors. */
#define MENU_ERROR_INVALID_ID		0U
#define MENU_ERROR_INVALID_PARENT	1U
#define MENU_ERROR_INVALID_TYPE		2U
#define MENU_ERROR_INVALID_VALUE	3U
#define MENU_ERROR_NOT_UPDATING		4U
#define MENU_ERROR_ALREADY_UPDATING	5U
#define MENU_ERROR_BAD_SERIAL		6U
#define MENU_ERROR_TOO_LARGE		7U

/* xdg_toplevel_menu_v1's requests and events. */
#define PLACE_DESTROY		0U
#define PLACE_SET_MENU		1U
#define PLACE_ACTIVATED		0U
#define PLACE_OPENED		1U
#define PLACE_CLOSED		2U

static uint32_t menu_word(const unsigned char *bytes, size_t offset);
static int menu_string(const unsigned char *bytes, size_t size, size_t offset, const char **text, size_t *next);
static int manager_request(struct kwl_object *manager, uint32_t opcode, const unsigned char *bytes, size_t size);
static int model_request(struct kwl_object *menu, uint32_t opcode, const unsigned char *bytes, size_t size);
static int model_edit(struct kwl_object *menu, uint32_t opcode, const unsigned char *bytes, size_t size);
static int place_request(struct kwl_object *place, uint32_t opcode, const unsigned char *bytes, size_t size);
static int context_create(struct kwl_object *manager, const unsigned char *bytes, size_t size);
static int model_begin(struct kwl_object *menu, uint32_t serial);
static int model_commit(struct kwl_object *menu, uint32_t serial);
static int model_add(struct kwl_object *menu, uint32_t id, uint32_t parent, uint32_t before, uint32_t type, const char *label, uint32_t action);
static int model_remove(struct kwl_object *menu, uint32_t id);
static int model_set_number(struct kwl_object *menu, uint32_t opcode, uint32_t id, uint32_t value);
static int model_set_text(struct kwl_object *menu, uint32_t opcode, uint32_t id, const char *text);
static int model_set_shortcut(struct kwl_object *menu, uint32_t id, uint32_t modifiers, uint32_t keysym);
static void model_fail(struct kwl_object *menu, uint32_t code, const char *reason);
static void model_free(struct kwl_menu_model *model);
static int pending_index(const struct kwl_menu_model *model, uint32_t id);
static unsigned pending_depth(const struct kwl_menu_model *model, uint32_t id);
static int pending_room(struct kwl_menu_model *model);
static int items_copy(struct kwl_menu_item *to, const struct kwl_menu_item *from, unsigned count);
static void items_free(struct kwl_menu_item *items, unsigned count);
static char *text_copy(const char *text);
static uint32_t place_seat(struct kwl_object *place);

/*
 * Carries out one request on a System Menu object.
 *
 * Returns 0, or EPROTO once a protocol error has been queued (a specific one
 * when the object's interface names it, otherwise the caller's generic one).
 */
int
kwl_menu_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	int error;

	/* Each interface has its own requests. */
	switch (object->kind) {
	case KWL_MENU_MANAGER:
		error = manager_request(object, opcode, bytes, size);
		break;
	case KWL_MENU:
		error = model_request(object, opcode, bytes, size);
		break;
	case KWL_TOPLEVEL_MENU:
		error = place_request(object, opcode, bytes, size);
		break;
	case KWL_CONTEXT_MENU:
		/* A context menu has only destroy; an open one closes without an event (kwl_menu_forget). */
		error = EPROTO;
		if (opcode == CONTEXT_DESTROY && size == 0U) {
			kwl_object_destroy(object);
			error = 0;
		}

		/* Nothing else is asked of it. */
		break;
	default:
		error = EPROTO;
		break;
	}

	/* Reports a request that was refused. */
	if (error != 0)
		return error;

	/* Succeeded: the request was carried out. */
	return 0;
}

/*
 * Unties an object that is going from the System Menu objects that name it.
 *
 * A toplevel, an xdg_toplevel_menu_v1 or an xdg_menu_v1 may go before the
 * others; the survivors stop naming it, and a menu open on it closes first.
 * A model goes with its xdg_menu_v1.
 */
void
kwl_menu_object_gone(
	struct kwl_object *object)
{
	struct kwl_object *other;

	/* A menu drawn or open from the object closes while its links still say whose it was. */
	kwl_menu_forget(object->client->server, object);

	/* A toplevel's place for a menu is left without a window. */
	if (object->kind == KWL_TOPLEVEL && object->toplevel_menu != NULL) {
		object->toplevel_menu->top = NULL;
		object->toplevel_menu = NULL;
		return;
	}

	/* A context menu stops naming its menu. */
	if (object->kind == KWL_CONTEXT_MENU) {
		object->shown_menu = NULL;
		return;
	}

	/* A place leaves its window, and shows nothing. */
	if (object->kind == KWL_TOPLEVEL_MENU) {
		if (object->top != NULL)
			object->top->toplevel_menu = NULL;
		object->top = NULL;
		object->shown_menu = NULL;
		return;
	}

	/* Nothing else is tied to anything but a model. */
	if (object->kind != KWL_MENU)
		return;

	/* The places and context menus that showed this menu show nothing. */
	for (other = object->client->objects; other != NULL; other = other->next) {
		/* Only a place or a context menu can show a menu. */
		if (other->kind != KWL_TOPLEVEL_MENU && other->kind != KWL_CONTEXT_MENU)
			continue;

		/* It stops naming the menu. */
		if (other->shown_menu == object)
			other->shown_menu = NULL;
	}

	/* The model goes with its object, and the windows are drawn without it. */
	model_free(object->menu_model);
	object->menu_model = NULL;
	object->client->server->dirty = 1;
}

/*
 * Finds the menu a window shows: its committed model, and the
 * xdg_toplevel_menu_v1 (place) its activations go to.  Returns NULL when the
 * window shows none.
 */
struct kwl_menu_model *
kwl_menu_of_surface(
	struct kwl_object *surface,
	struct kwl_object **place)
{
	struct kwl_object *toplevel;
	struct kwl_object *menu;

	/* Nothing is found until the whole chain is. */
	*place = NULL;

	/* The surface's xdg_surface and its toplevel. */
	if (surface == NULL ||
	    surface->dead ||
	    surface->role == NULL)
		return NULL;
	toplevel = surface->role->top;
	if (toplevel == NULL || toplevel->dead)
		return NULL;

	/* The toplevel's place for a menu, and the menu in it. */
	if (toplevel->toplevel_menu == NULL)
		return NULL;
	menu = toplevel->toplevel_menu->shown_menu;
	if (menu == NULL ||
	    menu->dead ||
	    menu->menu_model == NULL)
		return NULL;

	/* Succeeded: the model and where its activations go. */
	*place = toplevel->toplevel_menu;
	return menu->menu_model;
}

/*
 * Finds a committed item by its ID; NULL when the model has none.
 */
const struct kwl_menu_item *
kwl_menu_item(
	const struct kwl_menu_model *model,
	uint32_t id)
{
	unsigned index;

	/* The model has at most MENU_ITEMS_MAX items, so a search is cheap. */
	for (index = 0; index < model->count; index++) {
		/* The item with this ID. */
		if (model->items[index].id == id)
			return &model->items[index];
	}

	/* No item has the ID. */
	return NULL;
}

/*
 * Lists the visible children of an item (KWL_MENU_ROOT for the top level)
 * in their order, as they are shown: a separator only between two other
 * items, never two together.  Returns how many were stored.
 */
unsigned
kwl_menu_children(
	const struct kwl_menu_model *model,
	uint32_t parent,
	const struct kwl_menu_item **children,
	unsigned capacity)
{
	const struct kwl_menu_item *separator;
	const struct kwl_menu_item *item;
	unsigned index;
	unsigned count;

	/* A separator waits until an item follows it. */
	separator = NULL;
	count = 0;
	for (index = 0; index < model->count; index++) {
		/* Only the parent's visible children, while there is room. */
		item = &model->items[index];
		if (item->parent != parent || !item->visible)
			continue;
		if (count == capacity)
			break;

		/* A separator is kept only after something, and once. */
		if (item->type == KWL_MENU_SEPARATOR) {
			if (count != 0)
				separator = item;
			continue;
		}

		/* An item brings the separator before it. */
		if (separator != NULL && count + 1U < capacity) {
			children[count] = separator;
			count++;
		}

		/* The separator is used up. */
		separator = NULL;

		/* The item itself. */
		children[count] = item;
		count++;
	}

	/* Succeeded: the children as shown. */
	return count;
}

/*
 * Tells a window's client that the user chose an item: its ID, its action,
 * the client's seat and a new serial for the input that chose it.
 */
void
kwl_menu_send_activated(
	struct kwl_object *place,
	const struct kwl_menu_item *item,
	const char *via)
{
	struct kwl_server *server;
	uint32_t words[4];
	int error;

	/* A place whose client has failed hears nothing. */
	server = place->client->server;
	if (place->dead || place->client->fatal)
		return;

	/* The item, its action, the seat (or none) and the serial. */
	words[0] = item->id;
	words[1] = item->action;
	words[2] = place_seat(place);
	words[3] = kwl_next_serial(server);

	/* The event; a client that cannot take it is failed. */
	error = kwl_emit(place->client, place->id, PLACE_ACTIVATED, words, sizeof(words));
	if (error != 0) {
		kwl_client_fail(place->client);
	}

	/* The log line the tests read. */
	printf("KWL MENU activate client=%llu place=%u item=%u action=%u serial=%u via=%s\n",
	       (unsigned long long)place->client->number, place->id, item->id, item->action, words[3], via);
}

/*
 * Tells a context menu's client that the user chose an item: its ID, its
 * action and a new serial for the input that chose it.
 */
void
kwl_menu_send_context_activated(
	struct kwl_object *context,
	const struct kwl_menu_item *item,
	const char *via)
{
	struct kwl_server *server;
	uint32_t words[3];
	int error;

	/* A context menu whose client has failed hears nothing. */
	server = context->client->server;
	if (context->dead || context->client->fatal)
		return;

	/* The item, its action and the serial. */
	words[0] = item->id;
	words[1] = item->action;
	words[2] = kwl_next_serial(server);

	/* The event; a client that cannot take it is failed. */
	error = kwl_emit(context->client, context->id, CONTEXT_ACTIVATED, words, sizeof(words));
	if (error != 0) {
		kwl_client_fail(context->client);
	}

	/* The log line the tests read. */
	printf("KWL MENU context-activate client=%llu context=%u item=%u action=%u serial=%u via=%s\n",
	       (unsigned long long)context->client->number, context->id, item->id, item->action, words[2], via);
}

/*
 * Tells a context menu's client that it closed (after a choice or without
 * one); it is told once, and shows nothing afterwards.
 */
void
kwl_menu_send_context_done(
	struct kwl_object *context)
{
	int error;

	/* A context menu whose client has failed hears nothing. */
	if (context->dead || context->client->fatal)
		return;

	/* The event has no arguments. */
	error = kwl_emit(context->client, context->id, CONTEXT_DONE, NULL, 0U);
	if (error != 0) {
		kwl_client_fail(context->client);
	}

	/* It shows nothing more. */
	context->shown_menu = NULL;
	printf("KWL MENU context-done client=%llu context=%u\n", (unsigned long long)context->client->number, context->id);
}

/*
 * Tells a window's client that the popup of a submenu opened or closed.
 */
void
kwl_menu_send_popup(
	struct kwl_object *place,
	uint32_t item,
	unsigned opened)
{
	uint32_t opcode;
	int error;

	/* A place whose client has failed hears nothing. */
	if (place->dead || place->client->fatal)
		return;

	/* opened or closed, with the submenu's item. */
	opcode = PLACE_CLOSED;
	if (opened)
		opcode = PLACE_OPENED;
	error = kwl_emit(place->client, place->id, opcode, &item, sizeof(item));
	if (error != 0) {
		kwl_client_fail(place->client);
	}
}

/* Reads one native-endian protocol word the caller has checked is there. */
static uint32_t
menu_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The payload need not be aligned. */
	memcpy(&word, bytes + offset, sizeof(word));

	/* Succeeded: the word. */
	return word;
}

/* Reads one non-null string argument and where the next argument starts; EPROTO when it is malformed. */
static int
menu_string(
	const unsigned char *bytes,
	size_t size,
	size_t offset,
	const char **text,
	size_t *next)
{
	uint32_t length;
	size_t aligned;
	size_t actual;

	/* The length word must be in the payload. */
	if (offset > size || size - offset < 4U)
		return EPROTO;

	/* A non-null string has at least its terminator, within the payload. */
	length = menu_word(bytes, offset);
	if (length == 0U || length > size - offset - 4U)
		return EPROTO;

	/* Its storage is padded to a word, which must be in the payload too. */
	aligned = ((size_t)length + 3U) & ~(size_t)3U;
	if (aligned > size - offset - 4U)
		return EPROTO;

	/* It ends with its terminator. */
	*text = (const char *)bytes + offset + 4U;
	if ((*text)[length - 1U] != '\0')
		return EPROTO;

	/* And has no other NUL, which could hide bytes. */
	actual = strlen(*text);
	if (actual + 1U != length)
		return EPROTO;

	/* Succeeded: the next argument follows the padded string. */
	*next = offset + 4U + aligned;
	return 0;
}

/* Carries out a request of xdg_menu_manager_v1. */
static int
manager_request(
	struct kwl_object *manager,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *created;
	struct kwl_object *toplevel;
	uint32_t id;
	int error;

	/* The binding goes; its menus and places stay. */
	if (opcode == MANAGER_DESTROY) {
		if (size != 0U)
			return EPROTO;

		/* Succeeded: the binding is gone. */
		kwl_object_destroy(manager);
		return 0;
	}

	/* A new, empty menu model. */
	if (opcode == MANAGER_CREATE_MENU) {
		if (size != 4U)
			return EPROTO;

		/* The object; its model is allocated with it. */
		id = menu_word(bytes, 0U);
		created = kwl_create(manager->client, id, KWL_MENU, manager->version);
		if (created == NULL)
			return EPROTO;
		created->menu_model = calloc(1, sizeof(*created->menu_model));
		if (created->menu_model == NULL) {
			kwl_object_destroy(created);
			return EPROTO;
		}

		/* Succeeded: the client owns the menu. */
		return 0;
	}

	/* Version 2: a context menu at a point of a surface. */
	if (opcode == MANAGER_GET_CONTEXT_MENU) {
		error = context_create(manager, bytes, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* Only get_toplevel_menu is left. */
	if (opcode != MANAGER_GET_TOPLEVEL_MENU || size != 8U)
		return EPROTO;

	/* The window must be one of the client's toplevels. */
	id = menu_word(bytes, 4U);
	toplevel = kwl_find(manager->client, id);
	if (toplevel == NULL || toplevel->kind != KWL_TOPLEVEL)
		return EPROTO;

	/* A window has one place for a menu at a time. */
	if (toplevel->toplevel_menu != NULL) {
		(void)kwl_error_code(manager->client, manager->id, MANAGER_ERROR_ALREADY_EXISTS, "the toplevel already has an xdg_toplevel_menu_v1");
		return EPROTO;
	}

	/* The place, tied to the window from both ends. */
	id = menu_word(bytes, 0U);
	created = kwl_create(manager->client, id, KWL_TOPLEVEL_MENU, manager->version);
	if (created == NULL)
		return EPROTO;
	created->top = toplevel;
	toplevel->toplevel_menu = created;

	/* Succeeded: the window has a place for a menu. */
	return 0;
}

/* Carries out a request of xdg_menu_v1. */
static int
model_request(
	struct kwl_object *menu,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	uint32_t serial;
	int error;

	/* The menu goes; windows showing it show nothing (kwl_menu_object_gone). */
	if (opcode == MENU_DESTROY) {
		if (size != 0U)
			return EPROTO;

		/* Succeeded: the menu is gone. */
		kwl_object_destroy(menu);
		return 0;
	}

	/* A transaction starts. */
	if (opcode == MENU_BEGIN_UPDATE) {
		if (size != 4U)
			return EPROTO;
		serial = menu_word(bytes, 0U);
		error = model_begin(menu, serial);

		/* Reports a refused transaction. */
		if (error != 0)
			return error;

		/* Succeeded: the transaction is open. */
		return 0;
	}

	/* A transaction is shown. */
	if (opcode == MENU_COMMIT) {
		if (size != 4U)
			return EPROTO;
		serial = menu_word(bytes, 0U);
		error = model_commit(menu, serial);

		/* Reports a refused commit. */
		if (error != 0)
			return error;

		/* Succeeded: the new model is shown. */
		return 0;
	}

	/* Every other request changes the model, which only a transaction may. */
	if (opcode > MENU_SET_SHORTCUT)
		return EPROTO;
	if (!menu->menu_model->updating) {
		model_fail(menu, MENU_ERROR_NOT_UPDATING, "a change outside begin_update and commit");
		return EPROTO;
	}

	/* The change itself. */
	error = model_edit(menu, opcode, bytes, size);
	if (error != 0)
		return error;

	/* Succeeded: the change waits for the commit. */
	return 0;
}

/* Decodes one change of a model in a transaction and applies it to the pending copy. */
static int
model_edit(
	struct kwl_object *menu,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	const char *text;
	uint32_t id;
	uint32_t parent;
	uint32_t before;
	uint32_t type;
	uint32_t action;
	uint32_t first;
	uint32_t second;
	size_t next;
	int error;

	/* Each change has its own arguments. */
	switch (opcode) {
	case MENU_APPEND_ITEM:
		/* id, parent, type, label, action. */
		error = menu_string(bytes, size, 12U, &text, &next);
		if (error != 0 || next + 4U != size)
			return EPROTO;
		id = menu_word(bytes, 0U);
		parent = menu_word(bytes, 4U);
		type = menu_word(bytes, 8U);
		action = menu_word(bytes, next);
		error = model_add(menu, id, parent, 0U, type, text, action);
		break;
	case MENU_INSERT_ITEM:
		/* id, parent, the sibling it goes before, type, label, action. */
		error = menu_string(bytes, size, 16U, &text, &next);
		if (error != 0 || next + 4U != size)
			return EPROTO;
		id = menu_word(bytes, 0U);
		parent = menu_word(bytes, 4U);
		before = menu_word(bytes, 8U);
		type = menu_word(bytes, 12U);
		action = menu_word(bytes, next);
		error = model_add(menu, id, parent, before, type, text, action);
		break;
	case MENU_REMOVE_ITEM:
		/* The item. */
		if (size != 4U)
			return EPROTO;
		id = menu_word(bytes, 0U);
		error = model_remove(menu, id);
		break;
	case MENU_SET_LABEL:
	case MENU_SET_ICON_NAME:
		/* The item and a string. */
		error = menu_string(bytes, size, 4U, &text, &next);
		if (error != 0 || next != size)
			return EPROTO;
		id = menu_word(bytes, 0U);
		error = model_set_text(menu, opcode, id, text);
		break;
	case MENU_SET_SHORTCUT:
		/* The item, the modifiers and the keysym. */
		if (size != 12U)
			return EPROTO;
		id = menu_word(bytes, 0U);
		first = menu_word(bytes, 4U);
		second = menu_word(bytes, 8U);
		error = model_set_shortcut(menu, id, first, second);
		break;
	default:
		/* The item and one number: action, enabled, visible, checked, role. */
		if (size != 8U)
			return EPROTO;
		id = menu_word(bytes, 0U);
		first = menu_word(bytes, 4U);
		error = model_set_number(menu, opcode, id, first);
		break;
	}

	/* Reports a change that was refused. */
	if (error != 0)
		return error;

	/* Succeeded: the pending copy has the change. */
	return 0;
}

/* Carries out a request of xdg_toplevel_menu_v1. */
static int
place_request(
	struct kwl_object *place,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *menu;
	uint32_t id;

	/* The place goes; its window shows no menu (kwl_menu_object_gone). */
	if (opcode == PLACE_DESTROY) {
		if (size != 0U)
			return EPROTO;

		/* Succeeded: the place is gone, and the window is drawn without its menu. */
		place->client->server->dirty = 1;
		kwl_object_destroy(place);
		return 0;
	}

	/* Only set_menu is left, with one nullable menu. */
	if (opcode != PLACE_SET_MENU || size != 4U)
		return EPROTO;

	/* A menu named must be one of the client's. */
	id = menu_word(bytes, 0U);
	menu = NULL;
	if (id != 0U) {
		menu = kwl_find(place->client, id);
		if (menu == NULL || menu->kind != KWL_MENU)
			return EPROTO;
	}

	/* A place whose window has gone keeps nothing. */
	if (place->top == NULL)
		return 0;

	/* The window shows the menu's committed state from the next frame. */
	place->shown_menu = menu;
	place->client->server->dirty = 1;
	printf("KWL MENU set client=%llu place=%u menu=%u\n", (unsigned long long)place->client->number, place->id, id);

	/* Succeeded: the window shows the menu. */
	return 0;
}

/*
 * Makes a context menu (xdg_menu_manager_v1.get_context_menu, version 2)
 * and opens it: the menu's top-level items as a popup at a point of one of
 * the client's windows, answering the press whose serial it gives.  A
 * request that does not answer the latest press still makes the object,
 * which is told done at once.
 */
static int
context_create(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *created;
	struct kwl_object *menu;
	struct kwl_object *surface;
	struct kwl_object *desktop;
	struct kwl_server *server;
	uint32_t serial;
	int32_t x;
	int32_t y;
	int answers;
	int error;

	/* New ID, menu, surface, x, y, seat and serial: seven words, from version 2. */
	server = manager->client->server;
	if (manager->version < 2U || size != 28U)
		return EPROTO;

	/* The menu must be one of the client's. */
	menu = kwl_find(manager->client, menu_word(bytes, 4U));
	if (menu == NULL || menu->kind != KWL_MENU)
		return EPROTO;

	/* The surface must be one of the client's. */
	surface = kwl_find(manager->client, menu_word(bytes, 8U));
	if (surface == NULL || surface->kind != KWL_SURFACE)
		return EPROTO;

	/* The point on the surface and the press's serial. */
	x = (int32_t)menu_word(bytes, 12U);
	y = (int32_t)menu_word(bytes, 16U);
	serial = menu_word(bytes, 24U);

	/* The context menu, showing the menu on the surface. */
	created = kwl_create(manager->client, menu_word(bytes, 0U), KWL_CONTEXT_MENU, manager->version);
	if (created == NULL)
		return EPROTO;
	created->shown_menu = menu;

	/* Only a window (a toplevel's surface) or the desktop surface (desktop.c) shows one. */
	desktop = kwl_desktop_surface(server);
	if ((surface->role == NULL || surface->role->top == NULL) &&
	    surface != desktop) {
		printf("KWL MENU context-refused client=%llu context=%u surface=%u reason=window\n", (unsigned long long)manager->client->number, created->id, surface->id);
		kwl_menu_send_context_done(created);
		return 0;
	}

	/* Only the latest press opens a menu, or the last drop's enter for the client dropped on (ask, data.c); any other is told done at once. */
	answers = 0;
	if (serial != 0U && serial == server->press_serial)
		answers = 1;
	if (serial != 0U && serial == server->dnd_drop_serial && manager->client->number == server->dnd_drop_client)
		answers = 1;
	if (!answers) {
		printf("KWL MENU context-refused client=%llu context=%u serial=%u press=%u\n", (unsigned long long)manager->client->number, created->id, serial, server->press_serial);
		kwl_menu_send_context_done(created);
		return 0;
	}

	/* A drop's window comes to the top for its choice (a menu is only open on the window on top). */
	if (server->glass && serial != server->press_serial)
		kwl_glass_raise(server, surface);

	/* The popup at the point (menu-shell.c); one with nothing to show is done at once. */
	error = kwl_menu_open_context(server, created, surface, x, y);
	if (error != 0)
		kwl_menu_send_context_done(created);

	/* Succeeded: the client owns the context menu. */
	return 0;
}

/* Starts a transaction: the changes that follow go to a copy of the shown model. */
static int
model_begin(
	struct kwl_object *menu,
	uint32_t serial)
{
	struct kwl_menu_model *model;
	unsigned capacity;
	int error;

	/* One transaction at a time. */
	model = menu->menu_model;
	if (model->updating) {
		model_fail(menu, MENU_ERROR_ALREADY_UPDATING, "begin_update inside a transaction");
		return EPROTO;
	}

	/* The copy has room for the shown items and a few more. */
	capacity = model->count + 16U;
	model->pending = calloc(capacity, sizeof(*model->pending));
	if (model->pending == NULL)
		return EPROTO;
	model->pending_capacity = capacity;

	/* Every item, with its strings. */
	error = items_copy(model->pending, model->items, model->count);
	if (error != 0) {
		free(model->pending);
		model->pending = NULL;
		model->pending_capacity = 0;
		return EPROTO;
	}

	/* The transaction is open under the client's serial. */
	model->pending_count = model->count;
	model->updating = 1;
	model->update_serial = serial;

	/* Succeeded: changes may follow. */
	return 0;
}

/* Ends a transaction: the copy becomes the shown model at once. */
static int
model_commit(
	struct kwl_object *menu,
	uint32_t serial)
{
	struct kwl_menu_model *model;

	/* Only an open transaction, under the serial it was opened with. */
	model = menu->menu_model;
	if (!model->updating) {
		model_fail(menu, MENU_ERROR_NOT_UPDATING, "commit without begin_update");
		return EPROTO;
	}

	/* The serial pairs the commit with its begin_update. */
	if (serial != model->update_serial) {
		model_fail(menu, MENU_ERROR_BAD_SERIAL, "commit names another serial than begin_update");
		return EPROTO;
	}

	/* The old model goes and the copy takes its place. */
	items_free(model->items, model->count);
	free(model->items);
	model->items = model->pending;
	model->count = model->pending_count;
	model->capacity = model->pending_capacity;
	model->pending = NULL;
	model->pending_count = 0;
	model->pending_capacity = 0;

	/*
	 * The generation tells the drawing that the model changed; the open
	 * popups are checked against it on the next pass (menu-shell.c).
	 */
	model->updating = 0;
	model->generation++;
	menu->client->server->dirty = 1;

	/* Succeeded: the log line the tests read. */
	printf("KWL MENU commit client=%llu menu=%u serial=%u items=%u generation=%llu\n",
	       (unsigned long long)menu->client->number, menu->id, serial, model->count, (unsigned long long)model->generation);
	return 0;
}

/* Adds an item to the pending copy, before a sibling or after the last child. */
static int
model_add(
	struct kwl_object *menu,
	uint32_t id,
	uint32_t parent,
	uint32_t before,
	uint32_t type,
	const char *label,
	uint32_t action)
{
	struct kwl_menu_model *model;
	struct kwl_menu_item *item;
	unsigned depth;
	size_t length;
	int position;
	int found;
	int error;

	/* A new ID: not zero, not in use. */
	model = menu->menu_model;
	found = pending_index(model, id);
	if (id == 0U || found >= 0) {
		model_fail(menu, MENU_ERROR_INVALID_ID, "an item ID that is zero or already in use");
		return EPROTO;
	}

	/* One of the five types. */
	if (type > KWL_MENU_SUBMENU) {
		model_fail(menu, MENU_ERROR_INVALID_TYPE, "an unknown item type");
		return EPROTO;
	}

	/* The parent is the top level or a submenu. */
	if (parent != KWL_MENU_ROOT) {
		found = pending_index(model, parent);
		if (found < 0 || model->pending[found].type != KWL_MENU_SUBMENU) {
			model_fail(menu, MENU_ERROR_INVALID_PARENT, "a parent that is not a submenu");
			return EPROTO;
		}
	}

	/* The model stays within its bounds. */
	depth = pending_depth(model, parent) + 1U;
	length = strlen(label);
	if (model->pending_count >= MENU_ITEMS_MAX ||
	    depth > MENU_DEPTH_MAX ||
	    length > MENU_TEXT_MAX) {
		model_fail(menu, MENU_ERROR_TOO_LARGE, "too many items, too deep, or too long a label");
		return EPROTO;
	}

	/* The sibling it goes before, or the end. */
	position = (int)model->pending_count;
	if (before != 0U) {
		position = pending_index(model, before);
		if (position < 0 || model->pending[position].parent != parent) {
			model_fail(menu, MENU_ERROR_INVALID_PARENT, "before_id is not a child of the parent");
			return EPROTO;
		}
	}

	/* Room for one more. */
	error = pending_room(model);
	if (error != 0)
		return EPROTO;

	/* The item's place in the array, made free. */
	memmove(&model->pending[position + 1], &model->pending[position], (model->pending_count - (unsigned)position) * sizeof(*model->pending));
	item = &model->pending[position];
	memset(item, 0, sizeof(*item));

	/* The item, with the defaults for what the request does not give. */
	item->id = id;
	item->parent = parent;
	item->type = type;
	item->action = action;
	item->enabled = 1;
	item->visible = 1;
	item->label = text_copy(label);
	model->pending_count++;
	if (item->label == NULL)
		return EPROTO;

	/* No icon's name yet (an empty one). */
	item->icon_name = text_copy("");
	if (item->icon_name == NULL)
		return EPROTO;

	/* Succeeded: the item is in the pending copy. */
	return 0;
}

/* Removes an item and everything under it from the pending copy. */
static int
model_remove(
	struct kwl_object *menu,
	uint32_t id)
{
	struct kwl_menu_model *model;
	unsigned char *doomed;
	unsigned index;
	unsigned kept;
	unsigned more;
	int found;

	/* The item must be there. */
	model = menu->menu_model;
	found = pending_index(model, id);
	if (found < 0) {
		model_fail(menu, MENU_ERROR_INVALID_ID, "remove_item names no item");
		return EPROTO;
	}

	/* A mark for each item that goes. */
	doomed = calloc(model->pending_count, 1);
	if (doomed == NULL)
		return EPROTO;
	doomed[found] = 1;

	/* The marks spread to children until no new one is made (the tree is at most MENU_DEPTH_MAX deep). */
	more = 1;
	while (more) {
		more = 0;
		for (index = 0; index < model->pending_count; index++) {
			/* A child of a doomed item goes too. */
			if (doomed[index])
				continue;
			found = pending_index(model, model->pending[index].parent);
			if (found >= 0 && doomed[found]) {
				doomed[index] = 1;
				more = 1;
			}
		}
	}

	/* The marked items are freed and the rest close up in their order. */
	kept = 0;
	for (index = 0; index < model->pending_count; index++) {
		if (doomed[index]) {
			items_free(&model->pending[index], 1U);
			continue;
		}

		/* A kept item moves down over the freed ones. */
		model->pending[kept] = model->pending[index];
		kept++;
	}

	/* Succeeded: the item and its subtree are gone from the copy. */
	model->pending_count = kept;
	free(doomed);
	return 0;
}

/* Sets one numeric attribute of an item in the pending copy. */
static int
model_set_number(
	struct kwl_object *menu,
	uint32_t opcode,
	uint32_t id,
	uint32_t value)
{
	struct kwl_menu_item *item;
	int found;

	/* The item must be there. */
	found = pending_index(menu->menu_model, id);
	if (found < 0) {
		model_fail(menu, MENU_ERROR_INVALID_ID, "a change names no item");
		return EPROTO;
	}

	/* The action is any number the client chooses. */
	item = &menu->menu_model->pending[found];
	if (opcode == MENU_SET_ACTION) {
		item->action = value;
		return 0;
	}

	/* A role is one of those the protocol names. */
	if (opcode == MENU_SET_ROLE) {
		if (value > MENU_ROLE_LAST) {
			model_fail(menu, MENU_ERROR_INVALID_VALUE, "an unknown role");
			return EPROTO;
		}

		/* Succeeded: the role is kept (v1 draws nothing from it). */
		item->role = value;
		return 0;
	}

	/* The rest are true or false. */
	if (value > 1U) {
		model_fail(menu, MENU_ERROR_INVALID_VALUE, "a boolean that is not 0 or 1");
		return EPROTO;
	}

	/* enabled, visible, or checked (which only a checkbox or a radio item has). */
	if (opcode == MENU_SET_ENABLED) {
		item->enabled = value;
	} else if (opcode == MENU_SET_VISIBLE) {
		item->visible = value;
	} else {
		if (item->type != KWL_MENU_CHECKBOX && item->type != KWL_MENU_RADIO) {
			model_fail(menu, MENU_ERROR_INVALID_TYPE, "set_checked on an item that cannot be checked");
			return EPROTO;
		}

		/* The client's state of the item. */
		item->checked = value;
	}

	/* Succeeded: the copy has the value. */
	return 0;
}

/* Sets an item's label or icon name in the pending copy. */
static int
model_set_text(
	struct kwl_object *menu,
	uint32_t opcode,
	uint32_t id,
	const char *text)
{
	struct kwl_menu_item *item;
	char *copy;
	size_t length;
	int found;

	/* The item must be there. */
	found = pending_index(menu->menu_model, id);
	if (found < 0) {
		model_fail(menu, MENU_ERROR_INVALID_ID, "a change names no item");
		return EPROTO;
	}

	/* The text must be within bounds. */
	length = strlen(text);
	if (length > MENU_TEXT_MAX) {
		model_fail(menu, MENU_ERROR_TOO_LARGE, "too long a string");
		return EPROTO;
	}

	/* The item keeps its own copy. */
	copy = text_copy(text);
	if (copy == NULL)
		return EPROTO;

	/* The new text replaces the label or the icon name. */
	item = &menu->menu_model->pending[found];
	if (opcode == MENU_SET_LABEL) {
		free(item->label);
		item->label = copy;
	} else {
		free(item->icon_name);
		item->icon_name = copy;
	}

	/* Succeeded: the copy has the text. */
	return 0;
}

/* Sets an item's shortcut in the pending copy (keysym 0 removes it). */
static int
model_set_shortcut(
	struct kwl_object *menu,
	uint32_t id,
	uint32_t modifiers,
	uint32_t keysym)
{
	struct kwl_menu_item *item;
	int found;

	/* The item must be there. */
	found = pending_index(menu->menu_model, id);
	if (found < 0) {
		model_fail(menu, MENU_ERROR_INVALID_ID, "a change names no item");
		return EPROTO;
	}

	/* Only the four modifiers the protocol names. */
	if ((modifiers & ~MENU_MODIFIERS_ALL) != 0U) {
		model_fail(menu, MENU_ERROR_INVALID_VALUE, "an unknown modifier");
		return EPROTO;
	}

	/* The shortcut, or none. */
	item = &menu->menu_model->pending[found];
	item->modifiers = modifiers;
	item->keysym = keysym;
	if (keysym == 0U)
		item->modifiers = 0U;

	/* Succeeded: the copy has the shortcut. */
	return 0;
}

/* Queues a menu's protocol error with its code and returns EPROTO. */
static void
model_fail(
	struct kwl_object *menu,
	uint32_t code,
	const char *reason)
{
	/* The error names the menu and the xdg_menu_v1 error code; the caller refuses the request with EPROTO. */
	(void)kwl_error_code(menu->client, menu->id, code, reason);
}

/* Frees a model: its shown items and any pending copy. */
static void
model_free(
	struct kwl_menu_model *model)
{
	/* No model, nothing to free. */
	if (model == NULL)
		return;

	/* Both arrays with their strings, then the record. */
	items_free(model->items, model->count);
	free(model->items);
	items_free(model->pending, model->pending_count);
	free(model->pending);
	free(model);
}

/* Finds an item of the pending copy by its ID; -1 when there is none (and for the root, 0). */
static int
pending_index(
	const struct kwl_menu_model *model,
	uint32_t id)
{
	unsigned index;

	/* The root is not an item. */
	if (id == KWL_MENU_ROOT)
		return -1;

	/* A search of at most MENU_ITEMS_MAX items. */
	for (index = 0; index < model->pending_count; index++) {
		/* The item with this ID. */
		if (model->pending[index].id == id)
			return (int)index;
	}

	/* None has it. */
	return -1;
}

/* Tells how deep an item of the pending copy is (the root 0, a top-level item 1). */
static unsigned
pending_depth(
	const struct kwl_menu_model *model,
	uint32_t id)
{
	unsigned depth;
	int found;

	/* Up the parents to the root; the bound keeps a broken chain from looping. */
	depth = 0;
	while (id != KWL_MENU_ROOT && depth <= MENU_DEPTH_MAX) {
		found = pending_index(model, id);
		if (found < 0)
			break;
		depth++;
		id = model->pending[found].parent;
	}

	/* Succeeded: the depth. */
	return depth;
}

/* Makes room for one more item in the pending copy; ENOMEM when it cannot. */
static int
pending_room(
	struct kwl_menu_model *model)
{
	struct kwl_menu_item *grown;
	unsigned capacity;

	/* There is room already. */
	if (model->pending_count < model->pending_capacity)
		return 0;

	/* The array doubles. */
	capacity = model->pending_capacity * 2U;
	if (capacity < 16U)
		capacity = 16U;
	grown = realloc(model->pending, capacity * sizeof(*grown));
	if (grown == NULL)
		return ENOMEM;

	/* Succeeded: the copy has room. */
	model->pending = grown;
	model->pending_capacity = capacity;
	return 0;
}

/* Copies items with their strings into storage with room for them; ENOMEM when a string cannot be copied. */
static int
items_copy(
	struct kwl_menu_item *to,
	const struct kwl_menu_item *from,
	unsigned count)
{
	unsigned index;

	/* Each item, then its own copies of its strings. */
	for (index = 0; index < count; index++) {
		to[index] = from[index];
		to[index].icon_name = NULL;
		to[index].label = text_copy(from[index].label);
		if (to[index].label == NULL) {
			items_free(to, index + 1U);
			return ENOMEM;
		}

		/* Its icon's name. */
		to[index].icon_name = text_copy(from[index].icon_name);
		if (to[index].icon_name == NULL) {
			items_free(to, index + 1U);
			return ENOMEM;
		}
	}

	/* Succeeded: the copy owns its strings. */
	return 0;
}

/* Frees the strings of items (not the array they are in). */
static void
items_free(
	struct kwl_menu_item *items,
	unsigned count)
{
	unsigned index;

	/* Each item's label and icon name. */
	for (index = 0; index < count; index++) {
		free(items[index].label);
		free(items[index].icon_name);
		items[index].label = NULL;
		items[index].icon_name = NULL;
	}
}

/* Returns a malloc'ed copy of a string, or NULL. */
static char *
text_copy(
	const char *text)
{
	char *copy;
	size_t length;

	/* The bytes and the terminator. */
	length = strlen(text) + 1U;
	copy = malloc(length);
	if (copy == NULL)
		return NULL;
	memcpy(copy, text, length);

	/* Succeeded: the copy. */
	return copy;
}

/* Finds the ID of a seat the place's client bound (0 when it bound none). */
static uint32_t
place_seat(
	struct kwl_object *place)
{
	struct kwl_object *object;

	/* The first live seat of the client. */
	for (object = place->client->objects; object != NULL; object = object->next) {
		/* Only a live wl_seat. */
		if (object->kind == KWL_SEAT && !object->dead)
			return object->id;
	}

	/* The client has no seat: the argument is null. */
	return 0;
}
