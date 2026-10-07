/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * xdg_positioner and xdg_popup (ws035-p076): the menus, tooltips and
 * drop-downs a toolkit shows as surfaces of their own.
 *
 * A positioner holds the rules a popup is placed by: its size, an anchor
 * rectangle in the parent's window geometry, the anchor edge and the
 * gravity, an offset, and the adjustments (flip, slide, resize) allowed when
 * the popup would leave the output.  xdg_surface.get_popup places the popup
 * once from its positioner (again on reposition); its configure tells the
 * client the place relative to the parent and the size.
 *
 * A popup is not a window: it is never mapped as one (the window lists skip
 * it), it follows its parent, and it is drawn after all the windows in the
 * order popups were made, so that a submenu is over its menu.  A popup that
 * took the seat's grab gets the keyboard; the pointer goes to whichever
 * surface of the grab's chain (its popups and their toplevel) it is over,
 * and a press anywhere else closes the popups (popup_done) and is eaten.
 */

#include "popup.h"
#include "extras.h"
#include "glass.h"
#include "subsurface.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The popups drawn or hit-tested at once, and how deep a chain of popups may be. */
#define POPUP_MAX		32U
#define POPUP_DEPTH		16U

/* xdg_positioner's requests (version 3). */
#define POSITIONER_DESTROY		0U
#define POSITIONER_SET_SIZE		1U
#define POSITIONER_SET_ANCHOR_RECT	2U
#define POSITIONER_SET_ANCHOR		3U
#define POSITIONER_SET_GRAVITY		4U
#define POSITIONER_SET_CONSTRAINT	5U
#define POSITIONER_SET_OFFSET		6U
#define POSITIONER_SET_REACTIVE		7U
#define POSITIONER_SET_PARENT_SIZE	8U
#define POSITIONER_SET_PARENT_CONFIGURE	9U

/* xdg_positioner's error: an impossible size or rectangle. */
#define POSITIONER_ERROR_INVALID_INPUT	0U

/* The anchor and gravity edges (xdg_positioner.anchor and .gravity share them). */
#define EDGE_NONE		0U
#define EDGE_TOP		1U
#define EDGE_BOTTOM		2U
#define EDGE_LEFT		3U
#define EDGE_RIGHT		4U
#define EDGE_TOP_LEFT		5U
#define EDGE_BOTTOM_LEFT	6U
#define EDGE_TOP_RIGHT		7U
#define EDGE_BOTTOM_RIGHT	8U

/* The constraint adjustments. */
#define ADJUST_SLIDE_X		1U
#define ADJUST_SLIDE_Y		2U
#define ADJUST_FLIP_X		4U
#define ADJUST_FLIP_Y		8U
#define ADJUST_RESIZE_X		16U
#define ADJUST_RESIZE_Y		32U
#define ADJUST_ALL		63U

/* xdg_popup's requests and events. */
#define POPUP_DESTROY		0U
#define POPUP_GRAB		1U
#define POPUP_REPOSITION	2U
#define POPUP_CONFIGURE		0U
#define POPUP_DONE		1U
#define POPUP_REPOSITIONED	2U

/* xdg_surface's error for a surface that already has a role object, and its configure event. */
#define XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED	2U
#define XDG_SURFACE_CONFIGURE			0U

/* How far a popup's shadow reaches past it, and its corner. */
#define POPUP_SHADOW		14.0f
#define POPUP_RADIUS		8.0f

static int positioner_request(struct kwl_object *positioner, uint32_t opcode, const unsigned char *bytes, size_t size);
static int popup_object_request(struct kwl_object *popup, uint32_t opcode, const unsigned char *bytes, size_t size);
static int popup_reposition(struct kwl_object *popup, uint32_t positioner_id, uint32_t token);
static void popup_place(struct kwl_object *popup, const struct kwl_positioner *rules);
static void place_axis(int32_t anchor_start, int32_t anchor_length, int side, int direction, int32_t length, int32_t offset, int32_t *start);
static int anchor_side_x(uint32_t edge);
static int anchor_side_y(uint32_t edge);
static uint32_t flip_x(uint32_t edge);
static uint32_t flip_y(uint32_t edge);
static void popup_fit_axis(const struct kwl_positioner *rules, unsigned horizontal, int32_t base, int32_t limit, int32_t *start, int32_t *length);
static struct kwl_object *popup_of(struct kwl_object *surface);
static int surface_visible(struct kwl_server *server, struct kwl_object *surface);
static void geometry_offset(const struct kwl_object *surface, int32_t *x, int32_t *y);
static struct kwl_object *chain_toplevel(struct kwl_object *surface);
static struct kwl_object *chain_surface_at(struct kwl_server *server, int32_t x, int32_t y);
static void popup_dismiss(struct kwl_server *server);
static void popup_grab_end(struct kwl_server *server, struct kwl_object *next);
static struct kwl_object *grab_shown(struct kwl_server *server);
static uint32_t popup_word(const unsigned char *bytes, size_t offset);

/*
 * Makes an xdg_positioner for a client (xdg_wm_base.create_positioner):
 * complete once its size and anchor rectangle are set.
 */
int
kwl_positioner_create(
	struct kwl_object *wm,
	uint32_t id)
{
	struct kwl_object *created;

	/* The object, at the shell's version. */
	created = kwl_create(wm->client, id, KWL_POSITIONER, wm->version);
	if (created == NULL)
		return EPROTO;

	/* Its rules, empty: no size, no anchor rectangle, the edges none, no adjustment. */
	created->positioner = calloc(1, sizeof(*created->positioner));
	if (created->positioner == NULL) {
		kwl_object_destroy(created);
		return EPROTO;
	}

	/* Succeeded: the client can set the rules. */
	return 0;
}

/*
 * Gives a surface the popup role (xdg_surface.get_popup): the popup is
 * placed now from the positioner, relative to its parent (none puts it
 * relative to the output), and configured on the surface's first commit.
 */
int
kwl_popup_create(
	struct kwl_object *role,
	uint32_t id,
	uint32_t parent_id,
	uint32_t positioner_id)
{
	struct kwl_server *server;
	struct kwl_object *parent;
	struct kwl_object *positioner;
	struct kwl_object *created;
	uint32_t parent_surface;

	/* One role object per xdg_surface. */
	server = role->client->server;
	if (role->top != NULL) {
		(void)kwl_error_code(role->client, role->id, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED, "the xdg_surface already has a role object");
		return EPROTO;
	}

	/* The parent, when there is one, is another xdg_surface of the client with its surface. */
	parent = NULL;
	parent_surface = 0U;
	if (parent_id != 0U) {
		parent = kwl_find(role->client, parent_id);
		if (parent == NULL ||
		    parent->kind != KWL_XDG_SURFACE ||
		    parent->surface == NULL)
			return EPROTO;

		/* Its surface's ID, for the log. */
		parent_surface = parent->surface->id;
	}

	/* The positioner must be complete: a size and an anchor rectangle. */
	positioner = kwl_find(role->client, positioner_id);
	if (positioner == NULL || positioner->kind != KWL_POSITIONER)
		return EPROTO;
	if (!positioner->positioner->size_set || !positioner->positioner->anchor_set)
		return EPROTO;

	/* The popup, tied to its xdg_surface and its surface. */
	created = kwl_create(role->client, id, KWL_POPUP, role->version);
	if (created == NULL)
		return EPROTO;

	/* The popup names its surface and xdg_surface, and the xdg_surface names it as its role object. */
	created->surface = role->surface;
	created->role = role;
	role->top = created;

	/* Its parent surface, and its order among the popups (drawn in that order). */
	created->popup_parent = NULL;
	if (parent != NULL)
		created->popup_parent = parent->surface;
	server->popup_order++;
	created->popup_order = server->popup_order;

	/* Its place and size from the positioner. */
	popup_place(created, positioner->positioner);

	/* Succeeded: the log line the tests read. */
	printf("KWL POPUP create client=%llu popup=%u surface=%u parent=%u x=%d y=%d width=%d height=%d\n",
	       (unsigned long long)role->client->number, created->id, role->surface->id,
	       parent_surface, created->popup_x, created->popup_y, created->popup_width, created->popup_height);
	return 0;
}

/*
 * Carries out a request of xdg_positioner or xdg_popup.
 */
int
kwl_popup_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	int error;

	/* Each interface has its own requests. */
	if (object->kind == KWL_POSITIONER) {
		error = positioner_request(object, opcode, bytes, size);
	} else {
		error = popup_object_request(object, opcode, bytes, size);
	}

	/* Reports a request that was refused. */
	if (error != 0)
		return error;

	/* Succeeded: the request was carried out. */
	return 0;
}

/*
 * Configures a popup surface: xdg_popup.configure with its place relative
 * to the parent and its size, then xdg_surface.configure with a new serial.
 */
int
kwl_popup_send_configure(
	struct kwl_object *surface)
{
	struct kwl_server *server;
	struct kwl_object *popup;
	uint32_t words[4];
	int error;

	/* The popup of the surface. */
	server = surface->client->server;
	popup = surface->role->top;

	/* Its place and size. */
	words[0] = (uint32_t)popup->popup_x;
	words[1] = (uint32_t)popup->popup_y;
	words[2] = (uint32_t)popup->popup_width;
	words[3] = (uint32_t)popup->popup_height;
	error = kwl_emit(surface->client, popup->id, POPUP_CONFIGURE, words, sizeof(words));
	if (error != 0)
		return error;

	/* A nonzero serial names this configure. */
	server->serial++;
	if (server->serial == 0U)
		server->serial++;
	surface->configure_serial = server->serial;

	/* The xdg_surface configure makes the state the client acknowledges. */
	error = kwl_emit(surface->client, surface->role->id, XDG_SURFACE_CONFIGURE, &surface->configure_serial, sizeof(surface->configure_serial));
	if (error != 0)
		return error;

	/* Succeeded: the log line the tests read. */
	printf("KWL POPUP configure surface=%u serial=%u x=%d y=%d width=%d height=%d client=%llu\n", surface->id, surface->configure_serial, popup->popup_x, popup->popup_y, popup->popup_width, popup->popup_height, (unsigned long long)surface->client->number);
	return 0;
}

/*
 * Shows a popup whose surface has its first image, and gives a grabbing
 * popup the keyboard (and the pointer) of its client.
 */
void
kwl_popup_mapped(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	struct kwl_object *popup;
	unsigned grab;
	int32_t x;
	int32_t y;
	int shown;

	/* Its place on the output, for the log and for the pointer. */
	shown = kwl_popup_origin(server, surface, &x, &y);
	if (shown == 0) {
		surface->x = x;
		surface->y = y;
	}

	/* It is drawn from the next frame; the log line names whether it holds the grab. */
	server->dirty = 1;
	popup = popup_of(surface);
	grab = 0U;
	if (popup != NULL)
		grab = popup->popup_grab;
	printf("KWL POPUP map surface=%u x=%d y=%d grab=%u client=%llu\n", surface->id, surface->x, surface->y, grab, (unsigned long long)surface->client->number);

	/* Only the grabbing popup on top takes the focus. */
	if (popup == NULL || server->popup_grab != popup)
		return;

	/* From the first grabbing popup shown, the pointer goes to the surface of the grab's chain under it. */
	if (server->focus != NULL && server->focus->client == popup->client)
		server->pointer_grabbed = 1;

	/* The keyboard goes to the popup (kwl_popup_focus), the pointer to the surface it is over. */
	kwl_seat_focus(server);
}

/*
 * Unties an object that is going from the popups: a positioner's rules are
 * freed; a popup ends its grab and leaves its xdg_surface; a surface's
 * popups lose their parent and are told popup_done.
 */
void
kwl_popup_object_gone(
	struct kwl_object *object)
{
	struct kwl_server *server;
	struct kwl_object *other;
	struct kwl_object *next;

	/* A positioner only has its rules. */
	server = object->client->server;
	if (object->kind == KWL_POSITIONER) {
		free(object->positioner);
		object->positioner = NULL;
		return;
	}

	/* A popup: the grab passes to the popup under it (when that one grabbed), and its xdg_surface is free again. */
	if (object->kind == KWL_POPUP) {
		if (server->popup_grab == object) {
			next = NULL;
			other = popup_of(object->popup_parent);
			if (other != NULL && other->popup_grab && !other->popup_closed)
				next = other;

			/* The grab goes on under it, or ends. */
			popup_grab_end(server, next);
		}

		/* The xdg_surface no longer has a role object. */
		if (object->role != NULL && object->role->top == object)
			object->role->top = NULL;

		/* The popup names nothing any more, and the output is drawn without it. */
		object->role = NULL;
		object->surface = NULL;
		object->popup_parent = NULL;
		server->dirty = 1;
		return;
	}

	/* Only a surface is left that popups can name. */
	if (object->kind != KWL_SURFACE)
		return;

	/* Its popups lose their parent and are closed. */
	for (other = object->client->objects; other != NULL; other = other->next) {
		/* Only a live popup of this parent. */
		if (other->kind != KWL_POPUP ||
		    other->dead ||
		    other->popup_parent != object)
			continue;

		/* It is not drawn any more, and its client is told unless it was told already. */
		other->popup_parent = NULL;
		if (!other->popup_closed)
			(void)kwl_emit(other->client, other->id, POPUP_DONE, NULL, 0U);
		other->popup_closed = 1;
	}
}

/*
 * Works out where a surface is drawn on the output: a toplevel where the
 * shell draws its body, a popup at its parent's place plus its own.
 * Returns 0, or -1 when the surface is not shown (a popup whose parent has
 * gone or is hidden).
 */
int
kwl_popup_origin(
	struct kwl_server *server,
	struct kwl_object *surface,
	int32_t *x,
	int32_t *y)
{
	struct kwl_object *popup;
	struct kwl_object *chain[POPUP_DEPTH];
	unsigned depth;
	int32_t parent_x;
	int32_t parent_y;
	int32_t own_x;
	int32_t own_y;
	int32_t base_x;
	int32_t base_y;
	int visible;

	/* The chain from the surface up to its toplevel (or a popup with no parent). */
	depth = 0;
	chain[0] = surface;
	popup = popup_of(surface);
	while (popup != NULL && popup->popup_parent != NULL) {
		/* A chain deeper than any menu is refused. */
		if (depth + 1U >= POPUP_DEPTH)
			return -1;

		/* The parent is the next link. */
		depth++;
		chain[depth] = popup->popup_parent;
		popup = popup_of(popup->popup_parent);
	}

	/* The top of the chain: a shown toplevel's body, or the output's origin for a popup without a parent. */
	base_x = 0;
	base_y = 0;
	if (popup == NULL) {
		visible = surface_visible(server, chain[depth]);
		if (!visible)
			return -1;

		/* Its place; the glass look draws the body where the shell puts it (docked, on its way). */
		base_x = chain[depth]->x;
		base_y = chain[depth]->y;
		if (server->glass)
			(void)kwl_glass_body_origin(server, chain[depth], &base_x, &base_y);
	}

	/* Down the chain: each popup at its parent's window geometry plus its own place, less its own geometry. */
	while (depth > 0U) {
		/* The parent's geometry, the popup's place, the popup's own geometry. */
		geometry_offset(chain[depth], &parent_x, &parent_y);
		popup = popup_of(chain[depth - 1U]);
		geometry_offset(chain[depth - 1U], &own_x, &own_y);
		base_x = base_x + parent_x + popup->popup_x - own_x;
		base_y = base_y + parent_y + popup->popup_y - own_y;
		depth--;
	}

	/* A popup without a parent is placed on the output directly. */
	popup = popup_of(surface);
	if (popup != NULL &&
	    popup->popup_parent == NULL &&
	    chain[0] == surface) {
		geometry_offset(surface, &own_x, &own_y);
		base_x = popup->popup_x - own_x;
		base_y = popup->popup_y - own_y;
	}

	/* Succeeded: where the surface's image is drawn. */
	*x = base_x;
	*y = base_y;
	return 0;
}

/*
 * Finds the toplevel a popup's chain hangs from (its window, ws113-p007);
 * NULL for a popup without one.
 */
struct kwl_object *
kwl_popup_root(
	struct kwl_object *surface)
{
	struct kwl_object *popup;
	struct kwl_object *link;
	unsigned depth;

	/* Up the chain, no deeper than any menu. */
	link = surface;
	popup = popup_of(surface);
	for (depth = 0U; popup != NULL && depth < POPUP_DEPTH; depth++) {
		link = popup->popup_parent;
		if (link == NULL)
			return NULL;
		popup = popup_of(link);
	}

	/* A chain too deep has no window. */
	if (popup != NULL)
		return NULL;

	/* Succeeded: the toplevel. */
	return link;
}

/*
 * Collects the popup surfaces to draw this frame, in the order the popups
 * were made (a submenu after its menu).  Returns how many were stored.
 */
unsigned
kwl_popup_collect(
	struct kwl_server *server,
	struct kwl_object **popups,
	unsigned capacity)
{
	const struct kwl_import *image;
	struct kwl_client *client;
	struct kwl_object *object;
	struct kwl_object *surface;
	unsigned count;
	unsigned index;
	unsigned at;
	int32_t x;
	int32_t y;
	int shown;

	/* Every live popup whose surface has an image and whose chain is shown. */
	count = 0;
	for (client = server->clients; client != NULL; client = client->next) {
		/* A failed client shows nothing. */
		if (client->fatal)
			continue;

		/* Its popups. */
		for (object = client->objects; object != NULL; object = object->next) {
			/* Only a live popup that was not closed, with a surface. */
			if (object->kind != KWL_POPUP ||
			    object->dead ||
			    object->popup_closed ||
			    object->surface == NULL)
				continue;

			/* Its surface must have an image. */
			surface = object->surface;
			if (surface->dead || surface->current == NULL)
				continue;

			/* One window mode can sample. */
			image = kwl_compose_surface_image(surface);
			if (image == NULL)
				continue;

			/* And a place on the output. */
			shown = kwl_popup_origin(server, surface, &x, &y);
			if (shown != 0)
				continue;

			/* Room for it. */
			if (count == capacity)
				break;

			/* Its place by the order made (every surface stored has a popup object). */
			at = count;
			while (at > 0U && popups[at - 1U]->role->top->popup_order > object->popup_order)
				at--;

			/* Those after it move up, and it goes in. */
			for (index = count; index > at; index--)
				popups[index] = popups[index - 1U];
			popups[at] = surface;
			count++;
		}
	}

	/* Succeeded: the popups to draw. */
	return count;
}

/*
 * Draws the shown popups over the windows (and under the system bar), each
 * with a soft shadow in the glass look; each surface's place is kept for
 * the pointer.
 */
void
kwl_popup_draw(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	struct kwl_object *popups[POPUP_MAX];
	const struct kwl_import *image;
	struct glass_shape shape;
	uint32_t width;
	uint32_t height;
	unsigned count;
	unsigned index;
	int32_t x;
	int32_t y;
	unsigned output;
	int shown;

	/* The popups in the order they were made. */
	count = kwl_popup_collect(server, popups, POPUP_MAX);
	for (index = 0; index < count; index++) {
		/* Only those of the output drawn: of its windows (ws113-p007). */
		output = kwl_window_output(popups[index]);
		if (output != server->view_output)
			continue;

		/* Its place, kept for the pointer's surface-local position. */
		shown = kwl_popup_origin(server, popups[index], &x, &y);
		if (shown != 0)
			continue;

		/* The place, the image the frame samples and the popup's size (a viewport's, viewport.c). */
		popups[index]->x = x;
		popups[index]->y = y;
		image = kwl_compose_surface_image(popups[index]);
		kwl_surface_size(popups[index], &width, &height);

		/* The glass look gives it a soft shadow. */
		if (server->glass) {
			glass_shape_init(&shape, (float)x, (float)y + 4.0f, (float)width, (float)height);
			shape.quad[0] -= 2.0f * POPUP_SHADOW;
			shape.quad[1] -= 2.0f * POPUP_SHADOW;
			shape.quad[2] += 4.0f * POPUP_SHADOW;
			shape.quad[3] += 4.0f * POPUP_SHADOW;
			shape.mode = MODE_SHADOW;
			shape.radius = POPUP_RADIUS;
			shape.soft = POPUP_SHADOW;
			shape.color[0] = 0.05f;
			shape.color[1] = 0.08f;
			shape.color[2] = 0.16f;
			shape.color[3] = 0.30f;
			glass_shape_draw(server, command, &shape);
		}

		/* The image itself, between its sub-surfaces below and above it (subsurface.c). */
		kwl_subsurface_draw(server, command, popups[index], (float)x, (float)y, 1.0f, 1.0f, 0U);
		kwl_compose_surface_quad(server, command, popups[index], image, x, y);
		kwl_subsurface_draw(server, command, popups[index], (float)x, (float)y, 1.0f, 1.0f, 1U);
	}
}

/*
 * Chooses the surface that has the keyboard: the grabbing popup when the
 * focus would go to a surface of its client, otherwise the target itself.
 */
struct kwl_object *
kwl_popup_focus(
	struct kwl_server *server,
	struct kwl_object *target)
{
	struct kwl_object *grab;

	/* No grab, nothing changes. */
	if (server->popup_grab == NULL || target == NULL)
		return target;

	/*
	 * The grabbing popup shown on top: a submenu that asked for the grab
	 * but has no image yet leaves the keyboard with its menu.
	 */
	grab = grab_shown(server);
	if (grab == NULL || grab->client != target->client)
		return target;

	/* Succeeded: the popup has the keyboard. */
	return grab->surface;
}

/*
 * Handles a button while a popup holds the grab: a press over the grab's
 * chain goes to the surface under the pointer, a press elsewhere closes
 * the popups (popup_done) and is eaten with its release.  Returns 1 when
 * the button was taken.
 */
int
kwl_popup_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	struct kwl_object *over;

	/* The release of a press that closed the popups is eaten too. */
	if (state == 0U &&
	    server->popup_eaten_button == button &&
	    button != 0U) {
		server->popup_eaten_button = 0;
		return 1;
	}

	/* Without a grab the buttons are the shell's and the focus's. */
	if (server->popup_grab == NULL)
		return 0;

	/* A release goes where the pointer is. */
	if (state == 0U)
		return 0;

	/* A press over the chain goes to the surface under the pointer (seat.c). */
	over = chain_surface_at(server, server->pointer_x, server->pointer_y);
	if (over != NULL)
		return 0;

	/* A press anywhere else closes the popups, and is theirs. */
	server->popup_eaten_button = button;
	popup_dismiss(server);
	return 1;
}

/*
 * Finds the surface of the grab's chain under the pointer (the client's
 * shown popups, the latest first, then the toplevel's body); NULL outside
 * them all, or without a grab.
 */
struct kwl_object *
kwl_popup_chain_at(
	struct kwl_server *server)
{
	struct kwl_object *over;

	/* Without a grab there is no chain. */
	if (server->popup_grab == NULL)
		return NULL;

	/* The surface under the pointer. */
	over = chain_surface_at(server, server->pointer_x, server->pointer_y);

	/* Succeeded: the surface, or none. */
	return over;
}

/* Carries out a request of xdg_positioner. */
static int
positioner_request(
	struct kwl_object *positioner,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_positioner *rules;
	int32_t first;
	int32_t second;

	/* The positioner goes; the popups placed by it keep their places. */
	rules = positioner->positioner;
	if (opcode == POSITIONER_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(positioner);
		return 0;
	}

	/* Each rule has its own arguments. */
	switch (opcode) {
	case POSITIONER_SET_SIZE:
		/* The popup's size, which must not be empty. */
		if (size != 8U)
			return EPROTO;
		first = (int32_t)popup_word(bytes, 0U);
		second = (int32_t)popup_word(bytes, 4U);
		if (first <= 0 || second <= 0) {
			(void)kwl_error_code(positioner->client, positioner->id, POSITIONER_ERROR_INVALID_INPUT, "a positioner size that is not positive");
			return EPROTO;
		}

		/* The size is set. */
		rules->width = first;
		rules->height = second;
		rules->size_set = 1;
		break;
	case POSITIONER_SET_ANCHOR_RECT:
		/* The anchor rectangle, which must not have a negative size. */
		if (size != 16U)
			return EPROTO;
		first = (int32_t)popup_word(bytes, 8U);
		second = (int32_t)popup_word(bytes, 12U);
		if (first < 0 || second < 0) {
			(void)kwl_error_code(positioner->client, positioner->id, POSITIONER_ERROR_INVALID_INPUT, "an anchor rectangle with a negative size");
			return EPROTO;
		}

		/* The rectangle is set. */
		rules->anchor_x = (int32_t)popup_word(bytes, 0U);
		rules->anchor_y = (int32_t)popup_word(bytes, 4U);
		rules->anchor_width = first;
		rules->anchor_height = second;
		rules->anchor_set = 1;
		break;
	case POSITIONER_SET_ANCHOR:
	case POSITIONER_SET_GRAVITY:
		/* One of the nine edges. */
		if (size != 4U)
			return EPROTO;
		first = (int32_t)popup_word(bytes, 0U);
		if (first < 0 || first > (int32_t)EDGE_BOTTOM_RIGHT) {
			(void)kwl_error_code(positioner->client, positioner->id, POSITIONER_ERROR_INVALID_INPUT, "an unknown anchor or gravity");
			return EPROTO;
		}

		/* The anchor or the gravity. */
		if (opcode == POSITIONER_SET_ANCHOR) {
			rules->anchor = (uint32_t)first;
		} else {
			rules->gravity = (uint32_t)first;
		}

		/* The edge is set. */
		break;
	case POSITIONER_SET_CONSTRAINT:
		/* The adjustments allowed; unknown bits are ignored. */
		if (size != 4U)
			return EPROTO;
		rules->constraint = popup_word(bytes, 0U) & ADJUST_ALL;
		break;
	case POSITIONER_SET_OFFSET:
		/* The offset from where the anchor and gravity put the popup. */
		if (size != 8U)
			return EPROTO;
		rules->offset_x = (int32_t)popup_word(bytes, 0U);
		rules->offset_y = (int32_t)popup_word(bytes, 4U);
		break;
	case POSITIONER_SET_REACTIVE:
		/* Version 3: the popup would follow a moving parent (the compositor places popups relative to the parent anyway). */
		if (size != 0U)
			return EPROTO;
		rules->reactive = 1;
		break;
	case POSITIONER_SET_PARENT_SIZE:
		/* Version 3: the parent's size while it is being resized (a width and a height), which the compositor does not use. */
		if (size != 8U)
			return EPROTO;
		break;
	case POSITIONER_SET_PARENT_CONFIGURE:
		/* Version 3: the parent's configure serial the rules belong to, which the compositor does not use. */
		if (size != 4U)
			return EPROTO;
		break;
	default:
		return EPROTO;
	}

	/* Succeeded: the rule is set. */
	return 0;
}

/* Carries out a request of xdg_popup. */
static int
popup_object_request(
	struct kwl_object *popup,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_server *server;
	struct kwl_object *seat;
	uint32_t first;
	uint32_t second;
	uint32_t surface;
	int error;

	/* The popup goes (kwl_popup_object_gone ends its grab). */
	server = popup->client->server;
	if (opcode == POPUP_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(popup);
		return 0;
	}

	/* A reposition (version 3) places the popup again from a positioner (its ID, then a token). */
	if (opcode == POPUP_REPOSITION) {
		if (size != 8U)
			return EPROTO;
		first = popup_word(bytes, 0U);
		second = popup_word(bytes, 4U);
		error = popup_reposition(popup, first, second);
		if (error != 0)
			return error;

		/* Succeeded: the popup is placed again. */
		return 0;
	}

	/* Only grab is left, with the client's seat and the serial of the input that opened the popup. */
	if (opcode != POPUP_GRAB || size != 8U)
		return EPROTO;
	first = popup_word(bytes, 0U);
	seat = kwl_find(popup->client, first);
	if (seat == NULL || seat->kind != KWL_SEAT)
		return EPROTO;

	/* The popup on top holds the grab; its first image gives it the keyboard (kwl_popup_mapped). */
	popup->popup_grab = 1;
	server->popup_grab = popup;

	/* The log line the tests read. */
	surface = 0U;
	if (popup->surface != NULL)
		surface = popup->surface->id;
	printf("KWL POPUP grab popup=%u surface=%u\n", popup->id, surface);

	/* Succeeded: the popup holds the grab. */
	return 0;
}

/* Places a popup again from a positioner and tells its client (repositioned, then the configure). */
static int
popup_reposition(
	struct kwl_object *popup,
	uint32_t positioner_id,
	uint32_t token)
{
	struct kwl_object *positioner;
	int error;

	/* The positioner must be complete. */
	positioner = kwl_find(popup->client, positioner_id);
	if (positioner == NULL || positioner->kind != KWL_POSITIONER)
		return EPROTO;
	if (!positioner->positioner->size_set || !positioner->positioner->anchor_set)
		return EPROTO;

	/* The new place and size. */
	popup_place(popup, positioner->positioner);
	popup->client->server->dirty = 1;

	/* A popup whose surface has gone is only placed. */
	if (popup->surface == NULL)
		return 0;

	/* The token names the reposition the configure answers. */
	error = kwl_emit(popup->client, popup->id, POPUP_REPOSITIONED, &token, sizeof(token));
	if (error != 0)
		return error;

	/* The configure with the new place. */
	error = kwl_popup_send_configure(popup->surface);
	if (error != 0)
		return error;

	/* Succeeded: the client configures the popup again. */
	return 0;
}

/*
 * Places a popup by a positioner's rules: the anchor point on the anchor
 * rectangle, the popup on the gravity's side of it, the offset; then kept
 * on the output by the adjustments allowed.  The place is relative to the
 * parent's window geometry.
 */
static void
popup_place(
	struct kwl_object *popup,
	const struct kwl_positioner *rules)
{
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];
	struct kwl_server *server;
	unsigned count;
	unsigned slot;
	int32_t base_x;
	int32_t base_y;
	int32_t own_x;
	int32_t own_y;
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
	int anchor_x;
	int gravity_x;
	int anchor_y;
	int gravity_y;
	int shown;

	/* The sides the anchor and the gravity name on each axis. */
	server = popup->client->server;
	anchor_x = anchor_side_x(rules->anchor);
	gravity_x = anchor_side_x(rules->gravity);
	anchor_y = anchor_side_y(rules->anchor);
	gravity_y = anchor_side_y(rules->gravity);

	/* The place by the rules alone. */
	width = rules->width;
	height = rules->height;
	place_axis(rules->anchor_x, rules->anchor_width, anchor_x, gravity_x, width, rules->offset_x, &x);
	place_axis(rules->anchor_y, rules->anchor_height, anchor_y, gravity_y, height, rules->offset_y, &y);

	/* The parent's window geometry on the output, which the output's edges are measured from. */
	base_x = 0;
	base_y = 0;
	if (popup->popup_parent != NULL) {
		shown = kwl_popup_origin(server, popup->popup_parent, &base_x, &base_y);
		if (shown != 0) {
			base_x = popup->popup_parent->x;
			base_y = popup->popup_parent->y;
		}

		/* The parent's window geometry starts inside its image. */
		geometry_offset(popup->popup_parent, &own_x, &own_y);
		base_x += own_x;
		base_y += own_y;
	}

	/* Kept on the output of its window (ws113-p007), one axis at a time. */
	count = kwl_outputs(server, outputs);
	slot = KWL_PLANE_ANCHOR;
	if (popup->popup_parent != NULL)
		slot = kwl_window_output(popup->popup_parent);
	if (slot >= count || outputs[slot].width == 0U)
		slot = KWL_PLANE_ANCHOR;
	popup_fit_axis(rules, 1U, base_x - outputs[slot].x, (int32_t)outputs[slot].width, &x, &width);
	popup_fit_axis(rules, 0U, base_y - outputs[slot].y, (int32_t)outputs[slot].height, &y, &height);

	/* The popup's place and size. */
	popup->popup_x = x;
	popup->popup_y = y;
	popup->popup_width = width;
	popup->popup_height = height;
}

/*
 * Places one axis of a popup: the anchor point is the start, the middle or
 * the end of the anchor rectangle (side -1, 0, +1), and the popup lies
 * before it, across it or after it (direction -1, 0, +1), then the offset.
 */
static void
place_axis(
	int32_t anchor_start,
	int32_t anchor_length,
	int side,
	int direction,
	int32_t length,
	int32_t offset,
	int32_t *start)
{
	int32_t point;

	/* The anchor point on the rectangle. */
	point = anchor_start + anchor_length / 2;
	if (side < 0)
		point = anchor_start;
	if (side > 0)
		point = anchor_start + anchor_length;

	/* The popup on the gravity's side of the point. */
	*start = point - length / 2;
	if (direction < 0)
		*start = point - length;
	if (direction > 0)
		*start = point;

	/* The offset last. */
	*start += offset;
}

/* Tells which horizontal side an edge names: -1 left, +1 right, 0 neither. */
static int
anchor_side_x(
	uint32_t edge)
{
	/* The three left edges and the three right ones. */
	switch (edge) {
	case EDGE_LEFT:
	case EDGE_TOP_LEFT:
	case EDGE_BOTTOM_LEFT:
		return -1;
	case EDGE_RIGHT:
	case EDGE_TOP_RIGHT:
	case EDGE_BOTTOM_RIGHT:
		return 1;
	default:
		break;
	}

	/* The middle. */
	return 0;
}

/* Tells which vertical side an edge names: -1 top, +1 bottom, 0 neither. */
static int
anchor_side_y(
	uint32_t edge)
{
	/* The three top edges and the three bottom ones. */
	switch (edge) {
	case EDGE_TOP:
	case EDGE_TOP_LEFT:
	case EDGE_TOP_RIGHT:
		return -1;
	case EDGE_BOTTOM:
	case EDGE_BOTTOM_LEFT:
	case EDGE_BOTTOM_RIGHT:
		return 1;
	default:
		break;
	}

	/* The middle. */
	return 0;
}

/* Mirrors an edge left for right. */
static uint32_t
flip_x(
	uint32_t edge)
{
	/* Each edge with a horizontal side, the other way. */
	switch (edge) {
	case EDGE_LEFT:
		return EDGE_RIGHT;
	case EDGE_RIGHT:
		return EDGE_LEFT;
	case EDGE_TOP_LEFT:
		return EDGE_TOP_RIGHT;
	case EDGE_TOP_RIGHT:
		return EDGE_TOP_LEFT;
	case EDGE_BOTTOM_LEFT:
		return EDGE_BOTTOM_RIGHT;
	case EDGE_BOTTOM_RIGHT:
		return EDGE_BOTTOM_LEFT;
	default:
		break;
	}

	/* An edge without a horizontal side stays. */
	return edge;
}

/* Mirrors an edge top for bottom. */
static uint32_t
flip_y(
	uint32_t edge)
{
	/* Each edge with a vertical side, the other way. */
	switch (edge) {
	case EDGE_TOP:
		return EDGE_BOTTOM;
	case EDGE_BOTTOM:
		return EDGE_TOP;
	case EDGE_TOP_LEFT:
		return EDGE_BOTTOM_LEFT;
	case EDGE_BOTTOM_LEFT:
		return EDGE_TOP_LEFT;
	case EDGE_TOP_RIGHT:
		return EDGE_BOTTOM_RIGHT;
	case EDGE_BOTTOM_RIGHT:
		return EDGE_TOP_RIGHT;
	default:
		break;
	}

	/* An edge without a vertical side stays. */
	return edge;
}

/*
 * Keeps one axis of a popup on the output (from 0 to limit, the parent's
 * geometry at base): flipped to the other side when that fits, else slid
 * back inside, else cut to fit, as far as the adjustments allow.
 */
static void
popup_fit_axis(
	const struct kwl_positioner *rules,
	unsigned horizontal,
	int32_t base,
	int32_t limit,
	int32_t *start,
	int32_t *length)
{
	uint32_t flip;
	uint32_t slide;
	uint32_t resize;
	uint32_t anchor;
	uint32_t gravity;
	int anchor_side;
	int gravity_side;
	int32_t flipped;
	int32_t low;
	int32_t high;

	/* The adjustments of this axis. */
	flip = ADJUST_FLIP_Y;
	slide = ADJUST_SLIDE_Y;
	resize = ADJUST_RESIZE_Y;
	if (horizontal) {
		flip = ADJUST_FLIP_X;
		slide = ADJUST_SLIDE_X;
		resize = ADJUST_RESIZE_X;
	}

	/* Inside the output: nothing to do. */
	low = base + *start;
	high = low + *length;
	if (low >= 0 && high <= limit)
		return;

	/* Flipped to the other side of the anchor (the edges and the offset mirrored), when it then fits. */
	if ((rules->constraint & flip) != 0U) {
		if (horizontal) {
			anchor = flip_x(rules->anchor);
			gravity = flip_x(rules->gravity);
			anchor_side = anchor_side_x(anchor);
			gravity_side = anchor_side_x(gravity);
			place_axis(rules->anchor_x, rules->anchor_width, anchor_side, gravity_side, *length, -rules->offset_x, &flipped);
		} else {
			anchor = flip_y(rules->anchor);
			gravity = flip_y(rules->gravity);
			anchor_side = anchor_side_y(anchor);
			gravity_side = anchor_side_y(gravity);
			place_axis(rules->anchor_y, rules->anchor_height, anchor_side, gravity_side, *length, -rules->offset_y, &flipped);
		}

		/* The flip is kept only when it is inside. */
		if (base + flipped >= 0 && base + flipped + *length <= limit) {
			*start = flipped;
			return;
		}
	}

	/* Slid back inside: first so that the end is inside. */
	if ((rules->constraint & slide) != 0U) {
		if (base + *start + *length > limit)
			*start = limit - base - *length;

		/* Then so that the start is (a popup larger than the output keeps its start). */
		if (base + *start < 0)
			*start = -base;
		return;
	}

	/* Cut to what is inside: the part before the output's start. */
	if ((rules->constraint & resize) != 0U) {
		if (base + *start < 0) {
			*length += base + *start;
			*start = -base;
		}

		/* The part past its end. */
		if (base + *start + *length > limit)
			*length = limit - base - *start;

		/* At least one pixel stays. */
		if (*length < 1)
			*length = 1;
	}
}

/* Finds the popup object of a surface; NULL when the surface is not a popup. */
static struct kwl_object *
popup_of(
	struct kwl_object *surface)
{
	/* A surface with an xdg_surface whose role object is a popup. */
	if (surface == NULL || surface->role == NULL || surface->role->top == NULL)
		return NULL;
	if (surface->role->top->kind != KWL_POPUP)
		return NULL;

	/* Succeeded: the popup. */
	return surface->role->top;
}

/* Tells whether a toplevel window is shown now, so that its popups are. */
static int
surface_visible(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	float home;

	/* A live, mapped window on the desktop shown, not minimized. */
	if (surface->dead || !surface->mapped || surface->minimized)
		return 0;
	if (surface->desktop != server->desktop)
		return 0;

	/* Wiseview and App Home show tiles and icons, not windows with their popups. */
	if (server->wiseview > 0.0f || server->wiseview_moving || server->wiseview_gesture)
		return 0;
	home = kwl_home_progress(server);
	if (home > 0.0f)
		return 0;

	/* Succeeded: the window is shown. */
	return 1;
}

/* Finds where a surface's window geometry starts in its image (0, 0 without one). */
static void
geometry_offset(
	const struct kwl_object *surface,
	int32_t *x,
	int32_t *y)
{
	/* Without a geometry the whole image is the window. */
	*x = 0;
	*y = 0;
	if (!surface->geometry_set)
		return;

	/* The geometry's origin. */
	*x = surface->geometry[0];
	*y = surface->geometry[1];
}

/* Finds the toplevel at the top of a surface's chain of popups. */
static struct kwl_object *
chain_toplevel(
	struct kwl_object *surface)
{
	struct kwl_object *popup;
	unsigned depth;

	/* Up the parents, within the deepest chain. */
	popup = popup_of(surface);
	for (depth = 0; popup != NULL && depth < POPUP_DEPTH; depth++) {
		/* A popup without a parent has no toplevel. */
		if (popup->popup_parent == NULL)
			return NULL;
		surface = popup->popup_parent;
		popup = popup_of(surface);
	}

	/* Succeeded: the toplevel (or NULL for a chain too deep). */
	if (popup != NULL)
		return NULL;
	return surface;
}

/*
 * Finds the surface of the grab's chain under a point: its client's shown
 * popups, the latest first, then the toplevel's body.  NULL when the point
 * is outside them all.
 */
static struct kwl_object *
chain_surface_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	struct kwl_object *popups[POPUP_MAX];
	struct kwl_object *toplevel;
	const struct kwl_import *image;
	uint32_t width;
	uint32_t height;
	unsigned count;
	unsigned index;
	int32_t left;
	int32_t top;
	int shown;

	/* The shown popups of the grab's client, the latest first. */
	count = kwl_popup_collect(server, popups, POPUP_MAX);
	for (index = count; index > 0U; index--) {
		/* Only the grab's client's. */
		if (popups[index - 1U]->client != server->popup_grab->client)
			continue;

		/* The popup's rectangle on the output. */
		shown = kwl_popup_origin(server, popups[index - 1U], &left, &top);
		if (shown != 0)
			continue;
		image = kwl_compose_surface_image(popups[index - 1U]);
		if (image == NULL)
			continue;

		/* The point inside it (its size, a viewport's, viewport.c). */
		kwl_surface_size(popups[index - 1U], &width, &height);
		if (x >= left &&
		    x < left + (int32_t)width &&
		    y >= top &&
		    y < top + (int32_t)height) {
			popups[index - 1U]->x = left;
			popups[index - 1U]->y = top;
			return popups[index - 1U];
		}
	}

	/* The toplevel the grab's chain belongs to. */
	if (server->popup_grab->surface == NULL)
		return NULL;
	toplevel = chain_toplevel(server->popup_grab->surface);
	if (toplevel == NULL || toplevel->current == NULL)
		return NULL;

	/* Its body's rectangle. */
	shown = kwl_popup_origin(server, toplevel, &left, &top);
	if (shown != 0)
		return NULL;
	image = kwl_compose_surface_image(toplevel);
	if (image == NULL)
		return NULL;

	/* The point inside the body at its size (not its title bar, which is the compositor's). */
	kwl_surface_size(toplevel, &width, &height);
	if (x >= left &&
	    x < left + (int32_t)width &&
	    y >= top &&
	    y < top + (int32_t)height)
		return toplevel;

	/* Outside the chain. */
	return NULL;
}

/* Closes every popup of the grab's client (popup_done, the latest first) and ends the grab. */
static void
popup_dismiss(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *object;

	/* Each live popup of the client is told. */
	client = server->popup_grab->client;
	printf("KWL POPUP dismiss client=%llu\n", (unsigned long long)client->number);
	for (object = client->objects; object != NULL; object = object->next) {
		/* Only a live popup. */
		if (object->kind != KWL_POPUP || object->dead)
			continue;

		/* popup_done, once; the popup is not shown any more, and the client destroys it. */
		if (!object->popup_closed)
			(void)kwl_emit(client, object->id, POPUP_DONE, NULL, 0U);
		object->popup_grab = 0;
		object->popup_closed = 1;
	}

	/* No popup holds the grab any more. */
	popup_grab_end(server, NULL);
}

/*
 * Ends (or passes on) the grab: the keyboard moves to the next grabbing
 * popup, or back to the window; with no grab left, the pointer follows the
 * focus again (seat.c moves it).
 */
static void
popup_grab_end(
	struct kwl_server *server,
	struct kwl_object *next)
{
	/* The next grab, or none (the pointer is the grab's no more). */
	server->popup_grab = next;
	if (next == NULL)
		server->pointer_grabbed = 0;
	server->dirty = 1;

	/* The keyboard's focus, and the pointer's surface, follow. */
	kwl_seat_focus(server);
}

/*
 * Finds the grabbing popup shown on top: the one holding the grab, or,
 * while it has no image yet, the grabbing popup under it that has one.
 * NULL when none is shown.
 */
static struct kwl_object *
grab_shown(
	struct kwl_server *server)
{
	struct kwl_object *popup;
	unsigned depth;

	/* Down the grabbing chain from the top, within the deepest chain. */
	popup = server->popup_grab;
	for (depth = 0; popup != NULL && depth < POPUP_DEPTH; depth++) {
		/* A grabbing popup that is shown. */
		if (popup->popup_grab &&
		    !popup->popup_closed &&
		    popup->surface != NULL &&
		    popup->surface->current != NULL)
			return popup;

		/* Its parent popup, when there is one. */
		popup = popup_of(popup->popup_parent);
	}

	/* None is shown. */
	return NULL;
}

/* Reads one native-endian protocol word. */
static uint32_t
popup_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The payload need not be aligned. */
	memcpy(&word, bytes + offset, sizeof(word));

	/* Succeeded: the word. */
	return word;
}
