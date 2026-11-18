/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Sheets (ws090-p014): a window that has a parent (xdg_toplevel.set_parent)
 * and asks for the titlebar's sheet mode (kl_titlebar_v1 version 3,
 * titlebar.c) has no title bar of its own and hangs under its parent's,
 * in front of it (shell.c places, draws and raises it, and keeps the
 * parent's body from input while it is open).  This keeps who is whose
 * parent: a parent that goes leaves its windows without one, and they are
 * windows of their own again.  Other clients' set_parent dialogs are not
 * sheets: they keep no titlebar in sheet mode.
 */

#include "titlebar.h"

#include <stddef.h>

/*
 * Returns the parent a window hangs under as a sheet: its parent while it
 * asks for the sheet mode and the parent is a window shown; NULL otherwise.
 */
struct kwl_object *
kwl_sheet_parent(
	const struct kwl_object *surface)
{
	struct kwl_titlebar_model *model;
	struct kwl_object *titlebar;
	struct kwl_object *parent;

	/* A window with a parent that is a window shown. */
	if (surface == NULL || surface->dead || !surface->mapped)
		return NULL;
	parent = surface->parent_window;
	if (parent == NULL || parent->dead || !parent->mapped)
		return NULL;

	/* The sheet mode, as its titlebar shows it. */
	model = kwl_titlebar_of_surface((struct kwl_object *)surface, &titlebar);
	if (model == NULL || model->shown.mode != KWL_TITLEBAR_SHEET)
		return NULL;

	/* Succeeded: it is a sheet of the parent. */
	return parent;
}

/* Returns the sheet hung under a window (the latest mapped when there are more), or NULL. */
struct kwl_object *
kwl_sheet_of(
	const struct kwl_object *parent)
{
	struct kwl_object *object;
	struct kwl_object *found;
	struct kwl_object *sheet;

	/* Only a window shown has sheets. */
	if (parent == NULL || parent->dead || !parent->mapped)
		return NULL;

	/* Its client's windows whose parent it is (a parent is always one of the client's). */
	found = NULL;
	for (object = parent->client->objects; object != NULL; object = object->next) {
		if (object->kind != KWL_SURFACE || object->parent_window != parent)
			continue;
		sheet = kwl_sheet_parent(object);
		if (sheet == NULL)
			continue;
		if (found == NULL || object->map_order > found->map_order)
			found = object;
	}

	/* The sheet, or none. */
	return found;
}

/* Keeps a window's parent (NULL for none). */
void
kwl_sheet_set_parent(
	struct kwl_object *surface,
	struct kwl_object *parent)
{
	/* A toplevel without its surface has nothing to keep. */
	if (surface == NULL)
		return;

	/* The parent, and a sheet shows again from its start under a new one. */
	if (surface->parent_window != parent)
		surface->sheet_ms = 0;
	surface->parent_window = parent;

	/* App Home's running marks count a window with a parent differently (layer-cache.c). */
	if (surface->client != NULL)
		surface->client->server->home_picture_stale = 1U;
}

/* Forgets a surface that goes as any window's parent. */
void
kwl_sheet_surface_gone(
	struct kwl_object *surface)
{
	struct kwl_object *object;

	/* Its client's windows that named it. */
	for (object = surface->client->objects; object != NULL; object = object->next) {
		if (object->kind == KWL_SURFACE && object->parent_window == surface) {
			object->parent_window = NULL;
			object->sheet_ms = 0;
		}
	}
}
