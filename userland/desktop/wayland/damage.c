/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The damage of window mode (ws035-p055, compositing design D4): the part
 * of the output a change needs drawn again.
 *
 * Every change the compositor does not know the extent of marks the whole
 * output (server->dirty, as before).  Two frequent changes are measured
 * instead: the pointer moving over the windows' own areas (the cursor's
 * old and new places) and a window's new image when nothing near it is
 * glass (its body).  They gather in server->damage until a frame is drawn;
 * compose.c widens it by the frames each swapchain image missed (its
 * buffer age) and draws the frame inside it alone.
 */

#include "kwl.h"
#include "popup.h"

/* How far around the pointer a cursor may draw (the compositor's shapes and arrow are smaller). */
#define DAMAGE_CURSOR		64

static void damage_add(struct kwl_server *server, int32_t left, int32_t top, int32_t right, int32_t bottom);
static int cursor_unseen(struct kwl_server *server, int32_t x, int32_t y);

/*
 * Marks the damage of the pointer's move from an old place: the cursor's
 * old and new places, when nothing the compositor draws follows the pointer
 * there; otherwise the whole output.
 */
void
kwl_damage_pointer(
	struct kwl_server *server,
	int32_t old_x,
	int32_t old_y)
{
	int unseen;
	int calm;

	/*
	 * A cursor the window's client hid, over that client's window at both
	 * places, draws nothing (ws099-p015: the pen over fullscreen Notes
	 * would otherwise redraw the output for each of its places).
	 */
	unseen = cursor_unseen(server, old_x, old_y);
	if (unseen)
		unseen = cursor_unseen(server, server->pointer_x, server->pointer_y);
	if (unseen)
		return;

	/* The next frame shows the move without waiting for the windows (frame pacing, display.c). */
	server->pointer_moved = 1U;

	/* A client's own cursor surface may be of any size, and so may a drag's icon. */
	if (server->cursor_surface != NULL || server->dnd_active) {
		server->dirty = 1;
		return;
	}

	/* In the glass look both places must be over windows' own areas, in a still look. */
	if (server->glass) {
		calm = kwl_glass_pointer_calm(server, old_x, old_y);
		if (calm)
			calm = kwl_glass_pointer_calm(server, server->pointer_x, server->pointer_y);
		if (!calm) {
			server->dirty = 1;
			return;
		}
	}

	/* The cursor where it was and where it is. */
	damage_add(server, old_x - DAMAGE_CURSOR, old_y - DAMAGE_CURSOR, old_x + DAMAGE_CURSOR, old_y + DAMAGE_CURSOR);
	damage_add(server, server->pointer_x - DAMAGE_CURSOR, server->pointer_y - DAMAGE_CURSOR, server->pointer_x + DAMAGE_CURSOR, server->pointer_y + DAMAGE_CURSOR);
}

/*
 * Marks the damage of a surface's new image (its commit, adopted with the
 * image it replaced): the window's body when a toplevel's image of the
 * same size replaced another and nothing near is glass; otherwise the
 * whole output.
 */
void
kwl_damage_commit(
	struct kwl_server *server,
	struct kwl_object *surface,
	struct kwl_object *previous)
{
	int32_t rect[4];
	uint32_t old_width;
	uint32_t old_height;
	uint32_t width;
	uint32_t height;
	int alone;

	/* A first image, an image taken away, or a sub-surface's, a popup's or a cursor's: everything. */
	if (previous == NULL || surface->current == NULL || surface->sub_role != NULL || surface->cursor_role) {
		server->dirty = 1;
		return;
	}

	/* Only a mapped toplevel's window, not covering the output, is drawn alone. */
	if (surface->role == NULL || surface->role->top == NULL || !surface->mapped || surface->fullscreen) {
		server->dirty = 1;
		return;
	}

	/* An image of another size changes the window's place on the output. */
	kwl_buffer_size(previous, &old_width, &old_height);
	kwl_buffer_size(surface->current, &width, &height);
	if (old_width != width || old_height != height) {
		server->dirty = 1;
		return;
	}

	/* The glass look: the body, when no window near it is glass (shell.c). */
	if (server->glass) {
		alone = kwl_glass_body_damage(server, surface, rect);
		if (!alone) {
			server->dirty = 1;
			return;
		}

		/* The body is all. */
		damage_add(server, rect[0], rect[1], rect[2], rect[3]);
		return;
	}

	/* The plain look: the window at its place, as large as its image. */
	damage_add(server, surface->x, surface->y, surface->x + (int32_t)width, surface->y + (int32_t)height);
}

/* Adds a rectangle (left, top, right, bottom) to the damage the next frame draws. */
static void
damage_add(
	struct kwl_server *server,
	int32_t left,
	int32_t top,
	int32_t right,
	int32_t bottom)
{
	/* The first rectangle is the damage. */
	if (!server->damaged) {
		server->damage[0] = left;
		server->damage[1] = top;
		server->damage[2] = right;
		server->damage[3] = bottom;
		server->damaged = 1;
		return;
	}

	/* Later ones widen it to the box around both. */
	if (left < server->damage[0])
		server->damage[0] = left;
	if (top < server->damage[1])
		server->damage[1] = top;
	if (right > server->damage[2])
		server->damage[2] = right;
	if (bottom > server->damage[3])
		server->damage[3] = bottom;
}

/*
 * Tells whether the cursor draws nothing at a point: the glass look's
 * still windows, the client that hid its cursor (with no resize arrow, no
 * drag, no popup's grab) owning the window body there (compose.c's
 * cursor, BUG-118).
 */
static int
cursor_unseen(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	struct kwl_object *window;
	int calm;

	/* A hidden client's cursor, and nothing else that follows the pointer. */
	if (!server->glass ||
	    server->cursor_client == NULL ||
	    !server->cursor_hidden ||
	    server->cursor_surface != NULL ||
	    server->frame_edges != 0U ||
	    server->dnd_active ||
	    server->pointer_grabbed)
		return 0;

	/* Over a window's body in a still look. */
	calm = kwl_glass_pointer_calm(server, x, y);
	if (!calm)
		return 0;

	/* The client's own window. */
	window = kwl_glass_body_at(server, x, y);
	if (window == NULL || window->client != server->cursor_client)
		return 0;

	/* Succeeded: nothing is drawn there. */
	return 1;
}
