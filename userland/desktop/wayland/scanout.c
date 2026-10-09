/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The game mode (ws122-p005b, BUG-223, the 2026-10-06 user decision B): a
 * fullscreen window that says it shows a video or a game
 * (wp_content_type_v1, content-type.c), alone on the output, is shown
 * without composing -- its image goes straight to the display through the
 * backend's direct scanout (kl_backend_scanout_*, zedBSD's alone).  Every
 * other window, a fullscreen one that does not ask too, is composed with
 * libvulkan as before (display.c).
 *
 * Each pass the rules (scanout-rules.c) decide from the facts gathered
 * here.  To enter, the swapchain is closed once no frame is in flight
 * (libvulkan gives the display back) and the backend claims the display;
 * each new image of the window is then presented, its frame callbacks
 * done and the image it replaces let go.  As soon as the facts change --
 * something to draw over it (an edge's gesture, App Home, a menu, the
 * keyboard), the pointer moving, a screenshot asked, another size -- the
 * backend releases the display and window mode opens the swapchain again
 * (display.c), so the gesture's feedback and the pointer are drawn.  The
 * input goes to the compositor all along.
 *
 * Log: "KWL SCANOUT direct=1 surface=N client=C switch_ms=M" and
 * "KWL SCANOUT direct=0 reason=R surface=N frames=F" (also once for a
 * window that asks but is not shown straight, for each new reason).
 */

#include "desktop.h"
#include "compose.h"
#include "extras.h"
#include "menu.h"
#include "scanout-rules.h"

#include "userland/desktop/libkeiland-backend/keiland-backend-display.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static void scanout_facts(struct kwl_server *server, struct kwl_object *top, struct kwl_scanout_facts *facts);
static int scanout_overlay(struct kwl_server *server, struct kwl_object *top);
static int scanout_enter(struct kwl_server *server, struct kwl_object *top);
static void scanout_frame(struct kwl_server *server, struct kwl_object *top);
static void scanout_callbacks(struct kwl_server *server, struct kwl_object *shown);
static void scanout_note(struct kwl_server *server, struct kwl_object *top, unsigned reason);

/*
 * Runs the game mode's part of a pass, after the commits were taken.
 * Returns 1 when the pass is the game mode's (the display shows the window
 * straight, nothing is composed), 0 when window mode draws it (the game
 * mode was left, or not entered).
 */
int
kwl_scanout_pass(
	struct kwl_server *server,
	struct kwl_object *top)
{
	struct kwl_scanout_facts facts;
	unsigned reason;
	int error;

	/* The pass's facts and the rules' answer. */
	scanout_facts(server, top, &facts);
	reason = kwl_scanout_decide(&facts);

	/* In the game mode: the window's new image, or back to window mode. */
	if (server->scanout != NULL) {
		if (reason == KWL_SCANOUT_DIRECT && top == server->scanout_surface) {
			scanout_frame(server, top);
			return 1;
		}

		/* The facts changed: window mode draws from this pass. */
		kwl_scanout_leave(server, reason);
		return 0;
	}

	/* Not to be shown straight: window mode (the reason logged for a window that asked). */
	if (reason != KWL_SCANOUT_DIRECT) {
		scanout_note(server, top, reason);
		return 0;
	}

	/* The swapchain goes only once no frame is in flight; a later pass tries again. */
	if (server->compose == NULL || server->compose->in_flight)
		return 0;

	/* Enters: the swapchain closed, the display claimed; a refusal stays window mode for this window. */
	error = scanout_enter(server, top);
	if (error != 0)
		return 0;

	/* Succeeded: the first image goes straight to the display. */
	scanout_frame(server, top);
	return 1;
}

/*
 * Leaves the game mode for a reason (KWL_SCANOUT_*): the backend releases
 * the display, the image shown is let go, and window mode opens the
 * swapchain again on the next pass (server->windowed is 0).
 */
void
kwl_scanout_leave(
	struct kwl_server *server,
	unsigned reason)
{
	struct kwl_object *front;
	uint32_t surface_id;

	/* Not in it. */
	if (server->scanout == NULL)
		return;

	/* The display goes back, then the image it showed. */
	kl_backend_scanout_close(server->scanout);
	server->scanout = NULL;
	front = server->scanout_front;
	server->scanout_front = NULL;
	kwl_buffer_put(front);

	/* The log, and window mode's frame from the next pass. */
	surface_id = 0U;
	if (server->scanout_surface != NULL)
		surface_id = server->scanout_surface->id;
	printf("KWL SCANOUT direct=0 reason=%s surface=%u frames=%llu\n", kwl_scanout_reason_name(reason), surface_id, (unsigned long long)server->scanout_frames);
	server->scanout_surface = NULL;
	server->scanout_reason = reason;
	server->dirty = 1;
}

/*
 * Forgets a surface that goes: the game mode it was shown in ends, and a
 * refusal of it is forgotten.
 */
void
kwl_scanout_surface_gone(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	/* The window shown straight. */
	if (server->scanout != NULL && server->scanout_surface == surface)
		kwl_scanout_leave(server, KWL_SCANOUT_NO_WINDOW);

	/* A window the display refused, and the last one logged. */
	if (server->scanout_refused == surface)
		server->scanout_refused = NULL;
	if (server->scanout_noted == surface)
		server->scanout_noted = NULL;
}

/* Gathers the facts of a pass for the rules. */
static void
scanout_facts(
	struct kwl_server *server,
	struct kwl_object *top,
	struct kwl_scanout_facts *facts)
{
	uint32_t width;
	uint32_t height;
	uint64_t now;

	/* The pointer's stillness: a move restarts it. */
	now = kwl_milliseconds();
	if (server->pointer_x != server->scanout_pointer[0] || server->pointer_y != server->scanout_pointer[1]) {
		server->scanout_pointer[0] = server->pointer_x;
		server->scanout_pointer[1] = server->pointer_y;
		server->scanout_motion_ms = now;
	}

	/* Nothing known yet; the window, if any. */
	memset(facts, 0, sizeof(*facts));
	facts->pointer_still_ms = now - server->scanout_motion_ms;
	if (top == NULL)
		return;
	facts->window = 1U;
	facts->fullscreen = top->fullscreen;
	facts->content_type = top->content_type;

	/* Its image: a client's GPU buffer (not shared memory) of the output's size, shown whole. */
	if (top->current != NULL && top->current->shm == NULL)
		facts->gpu_buffer = 1U;
	kwl_surface_size(top, &width, &height);
	if (width == server->width && height == server->height && top->source[2] <= 0 && top->destination[0] <= 0)
		facts->size_matches = 1U;

	/* What may be drawn over it, a screenshot waiting, a refusal of it. */
	facts->overlay = (unsigned)scanout_overlay(server, top);
	facts->shot = (unsigned)kwl_shot_waiting();
	if (server->scanout_refused == top)
		facts->refused = 1U;
}

/*
 * Tells whether anything is drawn over a fullscreen window: the edges'
 * gestures and what they show (App Home, Wiseview, the keyboard, the
 * corner's hint), the switcher, a menu, the network's and the volume's
 * popups, the power dialog, the lock, the system bar, an animation or a
 * transition, and any popup or sub-surface shown.
 */
static int
scanout_overlay(
	struct kwl_server *server,
	struct kwl_object *top)
{
	struct kwl_client *client;
	struct kwl_object *surface;
	int showing;

	/* The edges' gestures and what they show (shell.c). */
	showing = kwl_glass_overlay(server);
	if (showing)
		return 1;

	/* The compositor's own popups and dialogs. */
	showing = kwl_menu_is_open();
	if (showing)
		return 1;
	showing = kwl_network_is_open();
	if (showing)
		return 1;
	showing = kwl_bluetooth_is_open();
	if (showing)
		return 1;
	showing = kwl_bluetooth_ask_showing();
	if (showing)
		return 1;
	showing = kwl_volume_is_open();
	if (showing)
		return 1;
	showing = kwl_status_panel_is_open();
	if (showing)
		return 1;
	showing = kwl_power_dialog_showing(server);
	if (showing)
		return 1;

	/* The switcher, the lock, the system bar shown, an animation or a transition. */
	if (server->switcher.on || server->locked || server->anim != NULL || server->transition != NULL)
		return 1;
	if (server->glass && !server->bar_hidden)
		return 1;

	/* Any client's popup or sub-surface with an image (a menu of its own, an input method's candidates). */
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* A shown surface that is not the window itself. */
			if (surface->kind != KWL_SURFACE || surface->dead || surface == top || surface->current == NULL)
				continue;
			if (surface->sub_role != NULL)
				return 1;
			if (surface->role != NULL && surface->role->top != NULL && surface->role->top->kind == KWL_POPUP)
				return 1;
		}
	}

	/* Nothing over it. */
	return 0;
}

/*
 * Enters the game mode for a window: the swapchain closed (libvulkan gives
 * the display back), the backend's claim.  Returns 0, or the claim's
 * failure (the window is then refused until it goes or asks again, and
 * window mode opens the swapchain again).
 */
static int
scanout_enter(
	struct kwl_server *server,
	struct kwl_object *top)
{
	uint64_t started;
	int error;

	/* The swapchain goes. */
	started = kwl_milliseconds();
	kwl_compose_output_close(server);
	server->windowed = 0;

	/* The backend claims the display for the output's size. */
	error = kl_backend_scanout_open(server->backend, server->width, server->height, &server->scanout);
	if (error != 0) {
		server->scanout = NULL;
		server->scanout_refused = top;
		server->dirty = 1;
		printf("KWL SCANOUT direct=0 reason=backend errno=%d surface=%u client=%llu\n", error, top->id, (unsigned long long)top->client->number);
		return error;
	}

	/* Succeeded: the window is shown straight from now. */
	server->scanout_surface = top;
	server->scanout_frames = 0U;
	server->scanout_noted = NULL;
	printf("KWL SCANOUT direct=1 surface=%u client=%llu switch_ms=%llu\n", top->id, (unsigned long long)top->client->number, (unsigned long long)(kwl_milliseconds() - started));
	return 0;
}

/*
 * Shows a window's new image straight: presented, its frame callbacks
 * (and the hidden windows') done, the image it replaces let go.  A present
 * that fails leaves the game mode.
 */
static void
scanout_frame(
	struct kwl_server *server,
	struct kwl_object *top)
{
	struct kwl_object *buffer;
	struct kwl_object *previous;
	int error;

	/* Input goes to the window shown. */
	server->front_surface = kwl_desktop_front(server, top);
	kwl_seat_focus(server);

	/* Only a new image is presented. */
	buffer = top->current;
	if (buffer == NULL || (buffer == server->scanout_front && !top->fresh))
		return;

	/* The image to the display; a failure is the backend's refusal from now. */
	error = kl_backend_scanout_present(server->scanout, kwl_gpu_host(), kwl_gpu_resource(buffer));
	if (error != 0) {
		printf("KWL SCANOUT present errno=%d surface=%u\n", error, top->id);
		server->scanout_refused = top;
		kwl_scanout_leave(server, KWL_SCANOUT_REFUSED);
		return;
	}

	/* The display holds the new image; the one it replaces is let go. */
	previous = server->scanout_front;
	kwl_buffer_get(buffer);
	server->scanout_front = buffer;
	kwl_buffer_put(previous);
	top->fresh = 0;
	server->scanout_frames++;
	server->dirty = 0;
	server->damaged = 0;

	/* The window draws its next frame; the hidden windows are not kept waiting. */
	scanout_callbacks(server, top);
}

/* Does the frame callbacks of every surface: the one shown, and the others the game mode hides. */
static void
scanout_callbacks(
	struct kwl_server *server,
	struct kwl_object *shown)
{
	struct kwl_client *client;
	struct kwl_object *surface;

	/* Every surface of every live client. */
	(void)shown;
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			if (surface->kind == KWL_SURFACE)
				kwl_callbacks_done(&surface->committed_callbacks);
		}
	}
}

/* Logs, once for each new reason, why a fullscreen window that asks for the game mode is composed. */
static void
scanout_note(
	struct kwl_server *server,
	struct kwl_object *top,
	unsigned reason)
{
	/* Only a fullscreen window that asks. */
	if (top == NULL || !top->fullscreen)
		return;
	if (top->content_type != KWL_SCANOUT_CONTENT_VIDEO && top->content_type != KWL_SCANOUT_CONTENT_GAME)
		return;

	/* The same window and reason as last time. */
	if (server->scanout_noted == top && server->scanout_reason == reason)
		return;

	/* Logged. */
	server->scanout_noted = top;
	server->scanout_reason = reason;
	printf("KWL SCANOUT direct=0 reason=%s surface=%u frames=0\n", kwl_scanout_reason_name(reason), top->id);
}
