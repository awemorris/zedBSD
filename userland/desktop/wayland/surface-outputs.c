/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The outputs a surface is shown on, as wl_surface.enter and leave tell
 * its client (ws177-p001).
 *
 * A window belongs to one output in the extended mode (ws113-p007's
 * D-ATOMIC: it is drawn by that output alone, also where it reaches past
 * its edge), so its surfaces, its popups and their sub-surfaces are on
 * that output; in the mirror mode every display shows the anchor, so they
 * are on all of them.  Each surface keeps the outputs its client was told
 * of (outputs_entered, a bit for each slot of plane.h).  Every pass of the
 * event loop brings that set in line with the one the windows ask for
 * now: an output gained is told first, so the client is never left with
 * no output while a window moves, then an output lost.  A wl_output bound
 * later learns of the surfaces already on its display, and a display that
 * closes tells its surfaces that they left it while its bindings can
 * still be named.
 *
 * Being on an output follows the window's place, not whether it is seen
 * this moment: a minimized window and one of another virtual desktop stay
 * on their output, as the scale and the refresh the client chose for it
 * stay right (the design is plan/ws177/phase001/phase.md).
 */

#include "kwl.h"
#include "compose.h"
#include "displays.h"

#include <stdio.h>

/* The wl_surface events: an output entered and an output left. */
#define SURFACE_EVENT_ENTER	0U
#define SURFACE_EVENT_LEAVE	1U

/* The outputs a set can name: one bit for each slot of the plane. */
#define SURFACE_OUTPUTS_SLOTS	KWL_PLANE_SLOTS

static void surface_outputs_bring(struct kwl_server *server, unsigned excluded);
static unsigned surface_outputs_mirrored(struct kwl_server *server, unsigned excluded);
static unsigned surface_outputs_wanted(struct kwl_object *surface, const struct kwl_plane_rect *outputs, unsigned count, unsigned mirrored);
static int surface_outputs_shown(struct kwl_object *surface);
static void surface_outputs_tell(struct kwl_object *surface, unsigned slot, uint32_t opcode);

/*
 * Tells each client's surfaces the outputs they came onto and left since
 * the last pass: a window moved to another display, mapped or unmapped, a
 * mode chosen, a display shown or gone.
 */
void
kwl_surface_outputs_sync(
	struct kwl_server *server)
{
	/* Every output shown now counts. */
	surface_outputs_bring(server, SURFACE_OUTPUTS_SLOTS);
}

/*
 * Tells a new wl_output binding of the surfaces of its client that are
 * already on its display, as the binding would have heard had it existed
 * when they came there.
 */
void
kwl_surface_outputs_bound(
	struct kwl_object *output)
{
	struct kwl_object *surface;
	unsigned slot;
	uint32_t binding;
	int error;

	/* A binding of a display that closed, or of none the sets can name, is told nothing. */
	slot = output->output_head;
	if (slot >= SURFACE_OUTPUTS_SLOTS)
		return;

	/* Each live surface of the client that is on the binding's display. */
	binding = output->id;
	for (surface = output->client->objects; surface != NULL; surface = surface->next) {
		/* Only a live surface. */
		if (surface->kind != KWL_SURFACE || surface->dead)
			continue;

		/* Only one on this display. */
		if ((surface->outputs_entered & (1U << slot)) == 0U)
			continue;

		/* The enter names the new binding alone; the others heard it before. */
		error = kwl_emit(output->client, surface->id, SURFACE_EVENT_ENTER, &binding, sizeof(binding));
		if (error != 0) {
			printf("KWL SURFACE enter surface=%u output=%u errno=%d\n", surface->id, slot, error);
			return;
		}
	}
}

/*
 * Tells the surfaces on a display that closes (the head `head`, plane.h's
 * slot) that they left it, while its bindings are still its own, and
 * gives them the outputs they are on now.  Called before the display's
 * bindings are left with no display (protocol.c).
 */
void
kwl_surface_outputs_gone(
	struct kwl_server *server,
	uint32_t head)
{
	/* The anchor never closes; a head past the sets is not in any of them. */
	if (head == KWL_PLANE_ANCHOR || head >= SURFACE_OUTPUTS_SLOTS)
		return;

	/* Every output but the closing one counts. */
	surface_outputs_bring(server, head);
}

/*
 * Brings every live surface's told outputs in line with the outputs its
 * window is on now, leaving out the output `excluded` (a display that is
 * closing; SURFACE_OUTPUTS_SLOTS for none).
 */
static void
surface_outputs_bring(
	struct kwl_server *server,
	unsigned excluded)
{
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];
	struct kwl_client *client;
	struct kwl_object *surface;
	unsigned mirrored;
	unsigned count;
	unsigned wanted;
	unsigned gained;
	unsigned lost;
	unsigned slot;

	/* The outputs shown in the extended mode. */
	count = kwl_outputs(server, outputs);

	/* A display that is closing shows nothing more. */
	if (excluded < count)
		outputs[excluded].width = 0U;

	/* Every display the mirror shows, or none in the extended mode. */
	mirrored = surface_outputs_mirrored(server, excluded);

	/* Each live surface of every live client. */
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only a live surface. */
			if (surface->kind != KWL_SURFACE || surface->dead)
				continue;

			/* What it is on now against what its client was told; nothing more when they agree. */
			wanted = surface_outputs_wanted(surface, outputs, count, mirrored);
			if (wanted == surface->outputs_entered)
				continue;

			/* The outputs it came onto and the ones it left. */
			gained = wanted & ~surface->outputs_entered;
			lost = surface->outputs_entered & ~wanted;

			/* The outputs gained first, so the client always knows one while the window moves. */
			for (slot = 0U; slot < SURFACE_OUTPUTS_SLOTS; slot++) {
				if ((gained & (1U << slot)) != 0U)
					surface_outputs_tell(surface, slot, SURFACE_EVENT_ENTER);
			}

			/* Then the outputs it left. */
			for (slot = 0U; slot < SURFACE_OUTPUTS_SLOTS; slot++) {
				if ((lost & (1U << slot)) != 0U)
					surface_outputs_tell(surface, slot, SURFACE_EVENT_LEAVE);
			}

			/* The client now knows the set; a binding made later is told by kwl_surface_outputs_bound. */
			surface->outputs_entered = wanted;
		}
	}
}

/*
 * Gives the set of every display the mirror shows (the anchor and each head
 * open and not lost, but `excluded`), or 0 in the extended mode, where a
 * window is on its own output alone.
 */
static unsigned
surface_outputs_mirrored(
	struct kwl_server *server,
	unsigned excluded)
{
	struct kwl_compose *compose;
	const struct kwl_head *head;
	unsigned mirrored;
	unsigned index;
	unsigned slot;

	/* Without the GPU's output nothing is mirrored: the anchor alone. */
	compose = server->compose;
	if (compose == NULL)
		return 0U;

	/* The extended mode places each window on one output. */
	if (compose->display_mode != KWL_DISPLAYS_MIRROR)
		return 0U;

	/* The anchor, and each head that shows the desktop with it. */
	mirrored = 1U << KWL_PLANE_ANCHOR;
	for (index = 0U; index < KWL_HEADS; index++) {
		/* A head's slot is one past its index, and the sets name the plane's slots alone. */
		slot = index + 1U;
		if (slot >= SURFACE_OUTPUTS_SLOTS)
			break;

		/* Only a head that shows a picture now. */
		head = &compose->heads[index];
		if (!head->open || head->lost)
			continue;

		/* Not the display that is closing. */
		if (slot == excluded)
			continue;

		/* The head shows the anchor's windows too. */
		mirrored |= 1U << slot;
	}

	/* Succeeded: the displays the mirror shows. */
	return mirrored;
}

/*
 * Gives the set of outputs a surface is on now: none for a surface not
 * shown, every display of the mirror there, else its window's output (the
 * anchor for one not shown).
 */
static unsigned
surface_outputs_wanted(
	struct kwl_object *surface,
	const struct kwl_plane_rect *outputs,
	unsigned count,
	unsigned mirrored)
{
	unsigned slot;
	int shown;

	/* A surface with no picture of a window is on no output. */
	shown = surface_outputs_shown(surface);
	if (!shown)
		return 0U;

	/* The mirror shows it on every display. */
	if (mirrored != 0U)
		return mirrored;

	/*
	 * Its window's output: a sub-surface's and a popup's is their window's.
	 * One not shown now (a head gone, before the window's retreat) is the
	 * anchor's, as compose.c draws it.
	 */
	slot = kwl_window_output(surface);
	if (slot >= count || slot >= SURFACE_OUTPUTS_SLOTS)
		slot = KWL_PLANE_ANCHOR;
	if (outputs[slot].width == 0U)
		slot = KWL_PLANE_ANCHOR;

	/* Succeeded: the one output it is on. */
	return 1U << slot;
}

/*
 * Tells whether a surface is a picture of a window shown: it has an image,
 * it is not a cursor, and the surface its sub-surfaces hang from is a
 * mapped window or a popup with an image.  Returns 1 or 0.
 */
static int
surface_outputs_shown(
	struct kwl_object *surface)
{
	struct kwl_object *root;

	/* A surface with no image shows nothing. */
	if (surface->current == NULL)
		return 0;

	/* A cursor is drawn over the outputs, not on one as a window. */
	if (surface->cursor_role)
		return 0;

	/* Up from a sub-surface to the surface it hangs from (subsurface.c refuses a loop). */
	root = surface;
	while (root->sub_parent != NULL)
		root = root->sub_parent;

	/* A surface without a shell role is not a window. */
	if (root->role == NULL || root->role->top == NULL)
		return 0;

	/* A popup is shown while it has an image. */
	if (root->role->top->kind == KWL_POPUP) {
		if (root->current == NULL)
			return 0;
		return 1;
	}

	/* Anything else is a window, shown while it is mapped. */
	if (!root->mapped)
		return 0;

	/* Succeeded: it is a picture of a window shown. */
	return 1;
}

/*
 * Tells a surface's client that the surface came onto or left an output:
 * the event once for each live wl_output binding of the client that names
 * the output, and one line in the log.
 */
static void
surface_outputs_tell(
	struct kwl_object *surface,
	unsigned slot,
	uint32_t opcode)
{
	struct kwl_object *output;
	const char *what;
	uint32_t binding;
	int error;

	/* The line that says what changed, once for the surface. */
	what = "enter";
	if (opcode == SURFACE_EVENT_LEAVE)
		what = "leave";
	printf("KWL SURFACE %s surface=%u output=%u client=%llu\n", what, surface->id, slot, (unsigned long long)surface->client->number);

	/* Each live binding of the output that the client holds. */
	for (output = surface->client->objects; output != NULL; output = output->next) {
		/* Only a live wl_output of that display. */
		if (output->kind != KWL_OUTPUT || output->dead)
			continue;
		if (output->output_head != slot)
			continue;

		/* The event names the binding; a failure is the client's own and ends its telling. */
		binding = output->id;
		error = kwl_emit(surface->client, surface->id, opcode, &binding, sizeof(binding));
		if (error != 0) {
			printf("KWL SURFACE %s surface=%u output=%u errno=%d\n", what, surface->id, slot, error);
			return;
		}
	}
}
