/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * wl_subcompositor and wl_subsurface (ws035-p077): the video, GL parts and
 * client-side decorations a toolkit draws on surfaces of their own.
 *
 * A sub-surface is shown with its parent (a window, a popup, or another
 * sub-surface), at a position relative to the parent's image, below or
 * above it; its own children go with it.  The position set is applied when
 * the parent's state is applied.  A synchronized sub-surface (the default,
 * and every sub-surface under a synchronized one) keeps its commit in a
 * cache until then; a desynchronized one shows its commit at once.
 *
 * The stacking requests (place_above, place_below) are applied at once,
 * not with the parent's next commit, and a sub-surface's whole image takes
 * the pointer (input regions are not kept).
 */

#include "subsurface.h"
#include "glass.h"
#include "popup.h"
#include "extras.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The requests of wl_subcompositor. */
#define SUBCOMPOSITOR_DESTROY		0U
#define SUBCOMPOSITOR_GET_SUBSURFACE	1U

/* wl_subcompositor's errors: a surface that cannot take the role, a parent that cannot be one. */
#define SUBCOMPOSITOR_ERROR_BAD_SURFACE	0U
#define SUBCOMPOSITOR_ERROR_BAD_PARENT	1U

/* The requests of wl_subsurface. */
#define SUBSURFACE_DESTROY		0U
#define SUBSURFACE_SET_POSITION		1U
#define SUBSURFACE_PLACE_ABOVE		2U
#define SUBSURFACE_PLACE_BELOW		3U
#define SUBSURFACE_SET_SYNC		4U
#define SUBSURFACE_SET_DESYNC		5U

/* wl_subsurface's error: a sibling that is neither a sibling nor the parent. */
#define SUBSURFACE_ERROR_BAD_SURFACE	0U

/* How deep sub-surfaces may be nested, which bounds every walk of a tree. */
#define SUBSURFACE_DEPTH		16U

static int subsurface_create(struct kwl_object *subcompositor, const unsigned char *bytes, size_t size);
static int subsurface_place(struct kwl_object *subsurface, struct kwl_object *surface, uint32_t opcode, const unsigned char *bytes, size_t size);
static unsigned is_ancestor(const struct kwl_object *surface, const struct kwl_object *other);
static unsigned effective_sync(const struct kwl_object *surface);
static void cache_commit(struct kwl_object *surface);
static void flush_cached(struct kwl_object *surface);
static void drop_cached(struct kwl_object *surface);
static void unlink_child(struct kwl_object *surface);
static void insert_child(struct kwl_object *parent, struct kwl_object *surface, struct kwl_object *before, unsigned above);
static void draw_tree(struct kwl_server *server, struct kwl_list *command, struct kwl_object *surface, float x, float y, float scale_x, float scale_y, unsigned depth);
static void draw_image(struct kwl_server *server, struct kwl_list *command, struct kwl_object *surface, const struct kwl_import *image, float x, float y, float scale_x, float scale_y);
static void draw_group(struct kwl_server *server, struct kwl_list *command, struct kwl_object *parent, float x, float y, float scale_x, float scale_y, unsigned above, unsigned depth);
static struct kwl_object *tree_at(struct kwl_object *surface, int32_t x, int32_t y, unsigned depth);
static struct kwl_object *group_at(struct kwl_object *parent, int32_t x, int32_t y, unsigned above, unsigned depth);
static struct kwl_object *tree_root(struct kwl_object *surface);
static uint32_t subsurface_word(const unsigned char *bytes, size_t offset);

/*
 * Carries out a request of wl_subcompositor: destroy, or get_subsurface.
 */
int
kwl_subcompositor_request(
	struct kwl_object *subcompositor,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	int error;

	/* The binding goes; the sub-surfaces it made stay. */
	if (opcode == SUBCOMPOSITOR_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(subcompositor);
		return 0;
	}

	/* Only get_subsurface is left. */
	if (opcode != SUBCOMPOSITOR_GET_SUBSURFACE)
		return EPROTO;

	/* The new sub-surface. */
	error = subsurface_create(subcompositor, bytes, size);
	if (error != 0)
		return error;

	/* Succeeded: the surface has the sub-surface role. */
	return 0;
}

/*
 * Carries out a request of wl_subsurface: destroy, set_position,
 * place_above, place_below, set_sync and set_desync.
 */
int
kwl_subsurface_request(
	struct kwl_object *subsurface,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *surface;
	unsigned synchronized;
	int error;

	/* The object goes; its surface loses the role and is not shown any more. */
	surface = subsurface->surface;
	if (opcode == SUBSURFACE_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(subsurface);
		return 0;
	}

	/* A sub-surface whose surface has gone takes no other request. */
	if (surface == NULL)
		return 0;

	/* Each request by its opcode. */
	switch (opcode) {
	case SUBSURFACE_SET_POSITION:
		/* The position for the parent's next commit. */
		if (size != 8U)
			return EPROTO;
		surface->sub_pending_x = (int32_t)subsurface_word(bytes, 0U);
		surface->sub_pending_y = (int32_t)subsurface_word(bytes, 4U);
		surface->sub_moved = 1;
		break;
	case SUBSURFACE_PLACE_ABOVE:
	case SUBSURFACE_PLACE_BELOW:
		error = subsurface_place(subsurface, surface, opcode, bytes, size);
		if (error != 0)
			return error;
		break;
	case SUBSURFACE_SET_SYNC:
		/* Its commits wait for the parent's from now. */
		if (size != 0U)
			return EPROTO;
		surface->sub_sync = 1;
		break;
	case SUBSURFACE_SET_DESYNC:
		/* Its commits show at once, and one cached shows now (unless an ancestor is synchronized). */
		if (size != 0U)
			return EPROTO;
		surface->sub_sync = 0;
		synchronized = effective_sync(surface);
		if (!synchronized)
			flush_cached(surface);
		break;
	default:
		/* No other request exists in version 1. */
		return EPROTO;
	}

	/* Succeeded: the request was carried out. */
	return 0;
}

/*
 * Commits a sub-surface: a synchronized one keeps the commit until its
 * parent's state is applied; a desynchronized one applies it now, with its
 * own children's.
 */
int
kwl_subsurface_commit(
	struct kwl_object *surface)
{
	unsigned synchronized;
	int error;

	/* A synchronized sub-surface keeps the commit. */
	synchronized = effective_sync(surface);
	if (synchronized) {
		cache_commit(surface);
		return 0;
	}

	/* A desynchronized one shows it now. */
	error = kwl_surface_queue(surface);
	if (error != 0)
		return error;

	/* Succeeded: its children's positions and cached commits go with it. */
	kwl_subsurface_applied(surface);
	return 0;
}

/*
 * Applies a surface's children with its state: the positions set for this
 * commit, and the cached commits of the synchronized children (with their
 * own children).
 */
void
kwl_subsurface_applied(
	struct kwl_object *surface)
{
	struct kwl_object *child;
	unsigned synchronized;

	/* Each child. */
	for (child = surface->sub_children; child != NULL; child = child->sub_next) {
		/* The position set for this commit. */
		if (child->sub_moved) {
			child->sub_x = child->sub_pending_x;
			child->sub_y = child->sub_pending_y;
			child->sub_moved = 0;
			surface->client->server->dirty = 1;
		}

		/* A synchronized child's cached commit shows with its parent. */
		synchronized = effective_sync(child);
		if (synchronized)
			flush_cached(child);
	}
}

/*
 * Unties an object that is going from the sub-surfaces: a wl_subsurface's
 * surface loses the role; a surface leaves its parent, and its children
 * lose their parent (they are not shown until they get another).
 */
void
kwl_subsurface_object_gone(
	struct kwl_object *object)
{
	struct kwl_object *surface;
	struct kwl_object *child;
	struct kwl_object *next;

	/* A wl_subsurface: its surface is role-free again and not shown. */
	if (object->kind == KWL_SUBSURFACE) {
		surface = object->surface;
		object->surface = NULL;
		if (surface == NULL)
			return;
		unlink_child(surface);
		drop_cached(surface);
		surface->sub_role = NULL;
		surface->sub_parent = NULL;
		surface->client->server->dirty = 1;
		return;
	}

	/* Only a surface is left that sub-surfaces can name. */
	if (object->kind != KWL_SURFACE)
		return;

	/* A sub-surface's surface leaves its parent, and its wl_subsurface names nothing. */
	if (object->sub_role != NULL) {
		unlink_child(object);
		drop_cached(object);
		object->sub_role->surface = NULL;
		object->sub_role = NULL;
		object->sub_parent = NULL;
	}

	/* Its children lose their parent. */
	child = object->sub_children;
	while (child != NULL) {
		next = child->sub_next;
		child->sub_parent = NULL;
		child->sub_next = NULL;
		child = next;
	}

	/* It has no children now, and the output is drawn without them. */
	object->sub_children = NULL;
	object->client->server->dirty = 1;
}

/*
 * Draws a parent's children below it (above 0) or above it (above 1), each
 * with its own children, the parent's image being at (x, y) and drawn
 * scale_x, scale_y times its size.
 */
void
kwl_subsurface_draw(
	struct kwl_server *server,
	struct kwl_list *command,
	struct kwl_object *parent,
	float x,
	float y,
	float scale_x,
	float scale_y,
	unsigned above)
{
	/* The group, from the first level down. */
	draw_group(server, command, parent, x, y, scale_x, scale_y, above, 0U);
}

/*
 * Collects the sub-surfaces with an image whose tree's top surface has one,
 * for the frame to hold and to give their frame callbacks.  Returns how many
 * were stored.
 */
unsigned
kwl_subsurface_collect(
	struct kwl_server *server,
	struct kwl_object **surfaces,
	unsigned capacity)
{
	struct kwl_client *client;
	struct kwl_object *surface;
	struct kwl_object *root;
	const struct kwl_import *image;
	unsigned count;

	/* Every client's live sub-surfaces. */
	count = 0;
	for (client = server->clients; client != NULL; client = client->next) {
		/* A failed client shows nothing. */
		if (client->fatal)
			continue;

		/* Its sub-surfaces. */
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only a live sub-surface in a tree, with an image. */
			if (surface->kind != KWL_SURFACE ||
			    surface->dead ||
			    surface->sub_parent == NULL ||
			    surface->current == NULL)
				continue;

			/* One window mode can sample. */
			image = kwl_compose_surface_image(surface);
			if (image == NULL)
				continue;

			/* Whose tree is shown (its top surface has an image). */
			root = tree_root(surface);
			if (root == NULL || root->current == NULL)
				continue;

			/* Room for it. */
			if (count == capacity)
				return count;
			surfaces[count] = surface;
			count++;
		}
	}

	/* Succeeded: the sub-surfaces the frame holds. */
	return count;
}

/*
 * Finds the topmost surface of a tree (the root and its sub-surfaces) at a
 * point on the output; NULL when the point is outside all of them.  Each
 * surface's place on the output is kept for the pointer's surface-local
 * position.
 */
struct kwl_object *
kwl_subsurface_at(
	struct kwl_object *root,
	int32_t x,
	int32_t y)
{
	struct kwl_object *found;

	/* No tree. */
	if (root == NULL || root->dead)
		return NULL;

	/* The tree from its root, whose place is the window's or the popup's. */
	found = tree_at(root, x, y, 0U);

	/* Succeeded: the surface under the point, or none. */
	return found;
}

/* Gives a surface the sub-surface role under a parent (wl_subcompositor.get_subsurface). */
static int
subsurface_create(
	struct kwl_object *subcompositor,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *created;
	struct kwl_object *surface;
	struct kwl_object *parent;
	uint32_t id;
	uint32_t surface_id;
	uint32_t parent_id;
	unsigned ancestor;

	/* The new object, the surface and the parent. */
	if (size != 12U)
		return EPROTO;
	id = subsurface_word(bytes, 0U);
	surface_id = subsurface_word(bytes, 4U);
	parent_id = subsurface_word(bytes, 8U);

	/* The surface must be one of the client's without a role. */
	surface = kwl_find(subcompositor->client, surface_id);
	if (surface == NULL ||
	    surface->kind != KWL_SURFACE ||
	    surface->role != NULL ||
	    surface->sub_role != NULL ||
	    surface->cursor_role) {
		(void)kwl_error_code(subcompositor->client, subcompositor->id, SUBCOMPOSITOR_ERROR_BAD_SURFACE, "the surface cannot be a sub-surface");
		return EPROTO;
	}

	/* The parent must be another surface of the client, not one under the surface. */
	parent = kwl_find(subcompositor->client, parent_id);
	if (parent == NULL || parent->kind != KWL_SURFACE || parent == surface) {
		(void)kwl_error_code(subcompositor->client, subcompositor->id, SUBCOMPOSITOR_ERROR_BAD_PARENT, "the parent cannot be one");
		return EPROTO;
	}

	/* A parent under the surface would make a loop. */
	ancestor = is_ancestor(surface, parent);
	if (ancestor) {
		(void)kwl_error_code(subcompositor->client, subcompositor->id, SUBCOMPOSITOR_ERROR_BAD_PARENT, "the parent is under the surface");
		return EPROTO;
	}

	/* The wl_subsurface, at the subcompositor's version. */
	created = kwl_create(subcompositor->client, id, KWL_SUBSURFACE, subcompositor->version);
	if (created == NULL)
		return EPROTO;

	/* The role, synchronized, at the parent's top-left corner. */
	created->surface = surface;
	surface->sub_role = created;
	surface->sub_parent = parent;
	surface->sub_sync = 1;
	surface->sub_x = 0;
	surface->sub_y = 0;
	surface->sub_moved = 0;

	/* On top of the parent's children. */
	insert_child(parent, surface, NULL, 1U);

	/* Succeeded: the log line the tests read. */
	printf("KWL SUBSURFACE create client=%llu surface=%u parent=%u\n", (unsigned long long)surface->client->number, surface->id, parent->id);
	return 0;
}

/* Places a sub-surface right above or right below a sibling or its parent. */
static int
subsurface_place(
	struct kwl_object *subsurface,
	struct kwl_object *surface,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *sibling;
	struct kwl_object *parent;
	struct kwl_object *before;
	uint32_t id;
	unsigned above;

	/* One surface. */
	if (size != 4U)
		return EPROTO;

	/* A sub-surface whose parent has gone has nothing to be placed by. */
	parent = surface->sub_parent;
	if (parent == NULL)
		return 0;

	/* The other surface must be the parent or another child of it. */
	id = subsurface_word(bytes, 0U);
	sibling = kwl_find(subsurface->client, id);
	if (sibling == NULL ||
	    sibling == surface ||
	    (sibling != parent && sibling->sub_parent != parent)) {
		(void)kwl_error_code(subsurface->client, subsurface->id, SUBSURFACE_ERROR_BAD_SURFACE, "not a sibling or the parent");
		return EPROTO;
	}

	/* Out of its place in the stack. */
	unlink_child(surface);

	/* Next to the parent: the lowest above it, or the highest below it. */
	if (sibling == parent) {
		above = 0;
		before = parent->sub_children;
		while (before != NULL && !before->sub_above)
			before = before->sub_next;
		if (opcode == SUBSURFACE_PLACE_ABOVE)
			above = 1;
		insert_child(parent, surface, before, above);
		parent->client->server->dirty = 1;
		return 0;
	}

	/* Next to a sibling: right before it (below) or right after it (above), on its side of the parent. */
	before = sibling;
	if (opcode == SUBSURFACE_PLACE_ABOVE)
		before = sibling->sub_next;
	insert_child(parent, surface, before, sibling->sub_above);
	parent->client->server->dirty = 1;

	/* Succeeded: the sub-surface is in its new place. */
	return 0;
}

/* Tells whether a surface is an ancestor of another (their chain of parents). */
static unsigned
is_ancestor(
	const struct kwl_object *surface,
	const struct kwl_object *other)
{
	const struct kwl_object *link;
	unsigned depth;

	/* Up the other's parents. */
	link = other->sub_parent;
	for (depth = 0; link != NULL && depth < SUBSURFACE_DEPTH; depth++) {
		/* The surface is one of them. */
		if (link == surface)
			return 1;
		link = link->sub_parent;
	}

	/* A chain deeper than allowed is treated as containing it. */
	if (link != NULL)
		return 1;

	/* Succeeded: it is not. */
	return 0;
}

/* Tells whether a sub-surface is synchronized: itself, or any sub-surface above it. */
static unsigned
effective_sync(
	const struct kwl_object *surface)
{
	const struct kwl_object *link;
	unsigned depth;

	/* Up the chain of sub-surfaces. */
	link = surface;
	for (depth = 0; link != NULL && depth < SUBSURFACE_DEPTH; depth++) {
		/* A surface that is not a sub-surface ends the chain. */
		if (link->sub_role == NULL)
			return 0;

		/* A synchronized one makes all under it so. */
		if (link->sub_sync)
			return 1;
		link = link->sub_parent;
	}

	/* Succeeded: none is. */
	return 0;
}

/* Keeps a synchronized sub-surface's commit (its buffer and frame callbacks) until its parent's state is applied. */
static void
cache_commit(
	struct kwl_object *surface)
{
	struct kwl_object *previous;
	struct kwl_object **tail;

	/* A newly attached buffer replaces the cached one. */
	if (surface->attached) {
		previous = surface->sub_cached_buffer;
		surface->sub_cached_buffer = surface->pending;
		surface->sub_cached_attached = 1;
		surface->pending = NULL;
		surface->attached = 0;
		kwl_buffer_put(previous);
	}

	/* The frame callbacks follow those already cached. */
	tail = &surface->sub_cached_callbacks;
	while (*tail != NULL)
		tail = &(*tail)->callback_next;
	*tail = surface->callbacks;
	surface->callbacks = NULL;

	/* A commit is cached (the damage stays pending and goes with it). */
	surface->sub_cached = 1;
}

/*
 * Applies a sub-surface's cached commit now, as its own commit would, and
 * then its children's; the state pending for its next commit is kept.
 */
static void
flush_cached(
	struct kwl_object *surface)
{
	struct kwl_object *pending;
	struct kwl_object *callbacks;
	unsigned attached;
	int error;

	/* Nothing cached. */
	if (!surface->sub_cached)
		return;

	/* The state pending for the next commit steps aside for the cached one. */
	pending = surface->pending;
	attached = surface->attached;
	callbacks = surface->callbacks;
	surface->pending = surface->sub_cached_buffer;
	surface->attached = surface->sub_cached_attached;
	surface->callbacks = surface->sub_cached_callbacks;
	surface->sub_cached_buffer = NULL;
	surface->sub_cached_attached = 0;
	surface->sub_cached_callbacks = NULL;
	surface->sub_cached = 0;

	/* The cached commit is applied. */
	error = kwl_surface_queue(surface);
	if (error != 0)
		printf("KWL SUBSURFACE flush surface=%u errno=%d\n", surface->id, error);

	/* The pending state comes back, and the children go with the applied state. */
	surface->pending = pending;
	surface->attached = attached;
	surface->callbacks = callbacks;
	kwl_subsurface_applied(surface);
}

/* Drops a cached commit whose sub-surface goes: its buffer, and its callbacks, which are told done. */
static void
drop_cached(
	struct kwl_object *surface)
{
	/* The cached buffer's hold. */
	kwl_buffer_put(surface->sub_cached_buffer);
	surface->sub_cached_buffer = NULL;
	surface->sub_cached_attached = 0;

	/* The callbacks, which will never be shown. */
	kwl_callbacks_done(&surface->sub_cached_callbacks);
	surface->sub_cached = 0;
}

/* Takes a sub-surface out of its parent's list of children. */
static void
unlink_child(
	struct kwl_object *surface)
{
	struct kwl_object **link;

	/* Only a child of a parent is in a list. */
	if (surface->sub_parent == NULL)
		return;

	/* Its link in the list. */
	link = &surface->sub_parent->sub_children;
	while (*link != NULL && *link != surface)
		link = &(*link)->sub_next;

	/* Out of it. */
	if (*link == surface)
		*link = surface->sub_next;
	surface->sub_next = NULL;
}

/* Puts a sub-surface into its parent's list right before another child (at the end for none), below or above the parent. */
static void
insert_child(
	struct kwl_object *parent,
	struct kwl_object *surface,
	struct kwl_object *before,
	unsigned above)
{
	struct kwl_object **link;

	/* The link that names the child it goes before. */
	link = &parent->sub_children;
	while (*link != NULL && *link != before)
		link = &(*link)->sub_next;

	/* In, on its side of the parent. */
	surface->sub_next = *link;
	*link = surface;
	surface->sub_above = above;
}

/* Draws a sub-surface at (x, y): its children below, its image, its children above. */
static void
draw_tree(
	struct kwl_server *server,
	struct kwl_list *command,
	struct kwl_object *surface,
	float x,
	float y,
	float scale_x,
	float scale_y,
	unsigned depth)
{
	const struct kwl_import *image;

	/* Its place, kept for the pointer. */
	surface->x = (int32_t)x;
	surface->y = (int32_t)y;

	/* The children below it. */
	draw_group(server, command, surface, x, y, scale_x, scale_y, 0U, depth + 1U);

	/* Its image, when it has one. */
	image = NULL;
	if (surface->current != NULL)
		image = kwl_compose_surface_image(surface);
	if (image != NULL)
		draw_image(server, command, surface, image, x, y, scale_x, scale_y);

	/* The children above it. */
	draw_group(server, command, surface, x, y, scale_x, scale_y, 1U, depth + 1U);
}

/* Draws the children of one side of a parent, bottom to top. */
static void
draw_group(
	struct kwl_server *server,
	struct kwl_list *command,
	struct kwl_object *parent,
	float x,
	float y,
	float scale_x,
	float scale_y,
	unsigned above,
	unsigned depth)
{
	struct kwl_object *child;
	float child_x;
	float child_y;

	/* Deeper than allowed is not drawn. */
	if (depth >= SUBSURFACE_DEPTH)
		return;

	/* Each child on this side, from the bottom. */
	for (child = parent->sub_children; child != NULL; child = child->sub_next) {
		/* Only the children of this side. */
		if (child->sub_above != above || child->dead)
			continue;

		/* At its position from the parent's image, scaled with it. */
		child_x = x + (float)child->sub_x * scale_x;
		child_y = y + (float)child->sub_y * scale_y;
		draw_tree(server, command, child, child_x, child_y, scale_x, scale_y, depth);
	}
}

/*
 * Draws one sub-surface's image, its viewport's source at its size
 * (viewport.c): in the glass look as a shape scaled with its window,
 * otherwise as a plain quad.
 */
static void
draw_image(
	struct kwl_server *server,
	struct kwl_list *command,
	struct kwl_object *surface,
	const struct kwl_import *image,
	float x,
	float y,
	float scale_x,
	float scale_y)
{
	struct glass_shape shape;
	uint32_t width;
	uint32_t height;

	/* The plain look draws it unscaled. */
	if (!server->glass) {
		kwl_compose_surface_quad(server, command, surface, image, (int32_t)x, (int32_t)y);
		return;
	}

	/* The glass look scales it with its window, square-cornered, as opaque as the window. */
	kwl_surface_size(surface, &width, &height);
	glass_shape_init(&shape, x, y, (float)width * scale_x, (float)height * scale_y);
	kwl_viewport_source(surface, shape.uv);
	shape.opacity = server->window_opacity;
	shape.mode = MODE_IMAGE;
	shape.radius = 0.0f;
	shape.set = image->set;
	if (image->draw == KWL_DRAW_OPAQUE)
		shape.opaque = 1.0f;
	glass_shape_draw(server, command, &shape);
}

/* Finds the topmost surface of a tree at a point: children above, the surface itself, children below. */
static struct kwl_object *
tree_at(
	struct kwl_object *surface,
	int32_t x,
	int32_t y,
	unsigned depth)
{
	struct kwl_object *found;
	uint32_t width;
	uint32_t height;

	/* The children above it first. */
	found = group_at(surface, x, y, 1U, depth + 1U);
	if (found != NULL)
		return found;

	/* Then its own image, at its size (a viewport's, viewport.c, as it is drawn). */
	if (surface->current != NULL) {
		kwl_surface_size(surface, &width, &height);

		/* The point inside that size from its place. */
		if (x >= surface->x &&
		    y >= surface->y &&
		    x < surface->x + (int32_t)width &&
		    y < surface->y + (int32_t)height)
			return surface;
	}

	/* Then the children below it. */
	found = group_at(surface, x, y, 0U, depth + 1U);

	/* Succeeded: the surface found, or none. */
	return found;
}

/* Finds the topmost child of one side of a parent (with its own children) at a point. */
static struct kwl_object *
group_at(
	struct kwl_object *parent,
	int32_t x,
	int32_t y,
	unsigned above,
	unsigned depth)
{
	struct kwl_object *child;
	struct kwl_object *found;
	struct kwl_object *best;

	/* Deeper than allowed is not hit. */
	if (depth >= SUBSURFACE_DEPTH)
		return NULL;

	/* Every child on this side; the last one hit is the topmost. */
	best = NULL;
	for (child = parent->sub_children; child != NULL; child = child->sub_next) {
		/* Only the children of this side. */
		if (child->sub_above != above || child->dead)
			continue;

		/* Its place on the output from its parent's (the parent's own is already known). */
		child->x = parent->x + child->sub_x;
		child->y = parent->y + child->sub_y;
		found = tree_at(child, x, y, depth);
		if (found != NULL)
			best = found;
	}

	/* Succeeded: the topmost found, or none. */
	return best;
}

/* Finds the top surface of a sub-surface's tree; NULL for a chain deeper than allowed or broken. */
static struct kwl_object *
tree_root(
	struct kwl_object *surface)
{
	unsigned depth;

	/* Up the parents. */
	for (depth = 0; surface->sub_parent != NULL && depth < SUBSURFACE_DEPTH; depth++)
		surface = surface->sub_parent;

	/* A chain that did not end. */
	if (surface->sub_parent != NULL)
		return NULL;

	/* A surface that is still a sub-surface without a parent is shown nowhere. */
	if (surface->sub_role != NULL)
		return NULL;

	/* Succeeded: the top surface. */
	return surface;
}

/* Reads one native-endian protocol word. */
static uint32_t
subsurface_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The payload need not be aligned. */
	memcpy(&word, bytes + offset, sizeof(word));

	/* Succeeded: the word. */
	return word;
}
