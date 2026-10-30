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
 * here.  To enter, the display thread asks the backend whether the
 * display can show the window, then the swapchain is closed in a change of
 * the display (display-thread.c: the frame in flight completes first,
 * libvulkan gives the display back) and the display thread has the backend
 * claim the display, while window mode's passes go on.
 *
 * Each new image of the window is then readied by the backend here (its
 * import) and presented by the present thread, one resident thread of a
 * lane named "present" that waits for the display's refresh while the
 * compositor takes the clients' commits (so a mailbox client's replaced
 * images go back at once).  When its present has returned, the result is
 * taken after the threads' wake pipe: the frame callbacks are done and the
 * image it replaces let go.  One present is in flight at a time; the
 * newest image goes next.
 *
 * As soon as the facts change -- something to draw over it (an edge's
 * gesture, App Home, a menu, the keyboard), the pointer moving over a
 * video, a screenshot asked, another size -- the game mode is left: the
 * display thread releases the display once the present in flight has
 * returned, and window mode opens the swapchain again after it
 * (display.c), so the gesture's feedback and the pointer are drawn.  The
 * images the display held are let go when it has been released, or when
 * their client goes.  The input goes to the compositor all along.
 *
 * A game that fills the output shows no cursor on any platform, whether
 * it is shown straight or composed (kwl_scanout_cursor_hidden), so its
 * pointer moving does not leave the game mode.
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

#include "lane.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * One present for the present thread: the scanout it shows on (its buffer
 * readied by the backend), the number of the claim (server->scanout_claim
 * then), and the present's result.
 */
struct scanout_present {
	struct kwl_lane_job lane;
	struct kl_backend_scanout *scanout;
	uint64_t claim;
	int result;
};

/*
 * A release of the display given to the display thread after the game mode
 * was left: the images the display held (the one shown and the one in
 * flight, each with a hold, NULL once let go) until the release is done.
 */
struct scanout_closing {
	struct scanout_closing *next;
	struct kwl_object *front;
	struct kwl_object *flight;
};

static void scanout_present_run(struct kwl_lane_job *lane);
static void scanout_present_done(void *context, struct kwl_lane_job *lane);
static void scanout_present_drop(struct kwl_lane_job *lane);
static void scanout_release(struct kwl_server *server, struct kl_backend_scanout *scanout, struct kwl_object *front, struct kwl_object *flight);
static void scanout_released(struct kwl_server *server, int error, void *context);
static void scanout_claimed(struct kwl_server *server, int error, void *context);
static void scanout_facts(struct kwl_server *server, struct kwl_object *top, struct kwl_scanout_facts *facts);
static int scanout_overlay(struct kwl_server *server, struct kwl_object *top);
static void scanout_enter(struct kwl_server *server, struct kwl_object *top);
static void scanout_answered(struct kwl_server *server, int error, void *context);
static void scanout_parked(struct kwl_server *server, int error, void *context);
static void scanout_closed(struct kwl_server *server, int error, void *context);
static void scanout_frame(struct kwl_server *server, struct kwl_object *top);
static void scanout_refuse(struct kwl_server *server, struct kwl_object *top, int error);
static void scanout_callbacks(struct kwl_server *server, struct kwl_object *shown);
static void scanout_note(struct kwl_server *server, struct kwl_object *top, unsigned reason);

/* The present thread's jobs. */
static const struct kwl_lane_ops scanout_present_ops = {
	scanout_present_run,
	scanout_present_done,
	scanout_present_drop
};

/*
 * The present thread's lane, and whether it runs (the game mode is not
 * entered without it); only the event loop's thread starts and stops it,
 * and the display thread waits on it for a release.
 */
static struct kwl_lane scanout_lane;
static unsigned scanout_started;

/* The releases given to the display thread and not done yet, the event loop's thread's own. */
static struct scanout_closing *scanout_closings;

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
	int busy;

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

	/* Only with the present thread; one entry at a time, not while the display changes (a later pass tries again). */
	if (!scanout_started || server->compose == NULL || server->scanout_entering != NULL)
		return 0;
	busy = kwl_display_busy(server);
	if (busy)
		return 0;

	/*
	 * Enters, while window mode draws on: the display thread asks whether
	 * the display can show it, then the swapchain is closed and the display
	 * claimed (scanout_answered); a later pass shows the first image
	 * straight.  A refusal stays window mode for this window.
	 */
	scanout_enter(server, top);
	return 0;
}

/*
 * Leaves the game mode for a reason (KWL_SCANOUT_*): the display thread
 * releases the display once the present in flight has returned, the
 * images shown are let go after, and window mode opens the swapchain again
 * from the next pass (server->windowed is 0; its open runs on the display
 * thread after the release).
 */
void
kwl_scanout_leave(
	struct kwl_server *server,
	unsigned reason)
{
	struct kl_backend_scanout *scanout;
	struct kwl_object *front;
	struct kwl_object *flight;
	uint32_t surface_id;

	/* Not in it. */
	if (server->scanout == NULL)
		return;

	/* The display goes back on the display thread, the images it showed with it (a present's result from now is not this one's). */
	scanout = server->scanout;
	front = server->scanout_front;
	flight = server->scanout_flight;
	server->scanout = NULL;
	server->scanout_front = NULL;
	server->scanout_flight = NULL;
	scanout_release(server, scanout, front, flight);

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
 * Starts the present thread once the threads' wake pipe is there (with the
 * display thread).  Without it the game mode is not entered (logged).
 */
void
kwl_scanout_start(
	struct kwl_server *server)
{
	int error;

	/* Once, after the wake pipe. */
	if (scanout_started || !server->threads_prepared)
		return;

	/* The thread, which waits for the first present. */
	error = kwl_lane_start(&scanout_lane, "present", 1U, server->wake[1]);
	if (error != 0) {
		printf("KWL SCANOUT thread=present errno=%d the game mode is not entered\n", error);
		return;
	}

	/* Succeeded: the game mode may be entered. */
	scanout_started = 1U;
}

/*
 * Stops the present thread at the end, after the display thread: a game
 * mode still shown is left first (its release runs here), then the
 * thread's present returns and the thread ends.
 */
void
kwl_scanout_stop(
	struct kwl_server *server)
{
	/* The game mode left; its release waits for the present running. */
	if (server->scanout != NULL)
		kwl_scanout_leave(server, KWL_SCANOUT_NO_WINDOW);

	/* Nothing runs. */
	if (!scanout_started)
		return;

	/* The thread ends (a result not taken is dropped). */
	kwl_lane_stop(&scanout_lane);
	scanout_started = 0U;
}

/*
 * Takes the present thread's results, on the event loop's thread after the
 * threads' wake pipe.
 */
void
kwl_scanout_take(
	struct kwl_server *server)
{
	/* Nothing runs. */
	if (!scanout_started)
		return;

	/* Each present that has returned. */
	(void)kwl_lane_take(&scanout_lane, server);
}

/*
 * Waits until the present thread has no present given or running: the
 * display thread, before it releases the display (whose buffer the
 * present shows until it returns).
 */
void
kwl_scanout_present_wait(void)
{
	/* Every present given has returned. */
	kwl_lane_wait(&scanout_lane);
}

/*
 * Lets go of a client's images still held by a release of the display
 * that is not done yet, as the client goes (its buffers are freed after).
 */
void
kwl_scanout_client_gone(
	struct kwl_server *server,
	struct kwl_client *client)
{
	struct scanout_closing *closing;

	/* Each release waiting, the client's images in it. */
	(void)server;
	for (closing = scanout_closings; closing != NULL; closing = closing->next) {
		if (closing->front != NULL && closing->front->client == client) {
			kwl_buffer_put(closing->front);
			closing->front = NULL;
		}

		/* Lets go of the flight image of the client likewise. */
		if (closing->flight != NULL && closing->flight->client == client) {
			kwl_buffer_put(closing->flight);
			closing->flight = NULL;
		}
	}
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

	/* A window being entered, a window the display refused, and the last one logged. */
	if (server->scanout_entering == surface)
		server->scanout_entering = NULL;
	if (server->scanout_refused == surface)
		server->scanout_refused = NULL;
	if (server->scanout_noted == surface)
		server->scanout_noted = NULL;
}

/*
 * Tells whether the cursor is left out because a game fills the output:
 * the top window is a fullscreen one that says it shows a game, and
 * nothing is drawn over it.  Neither the cursor plane nor the composed
 * frame shows the cursor then, whether the display shows the game
 * straight or the compositor composes it, on every backend.  Returns 1
 * or 0.
 */
int
kwl_scanout_cursor_hidden(
	struct kwl_server *server)
{
	struct kwl_object *top;
	int overlay;

	/* A fullscreen game on top. */
	top = kwl_top_window(server);
	if (top == NULL || !top->fullscreen)
		return 0;
	if (top->content_type != KWL_SCANOUT_CONTENT_GAME)
		return 0;

	/* Something drawn over it (a menu, App Home, the keyboard) is pointed at with the cursor. */
	overlay = scanout_overlay(server, top);
	if (overlay)
		return 0;

	/* Succeeded: the game alone, without the cursor. */
	return 1;
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

	/* The output's rotation: the display hardware's 180, the compositor's turn (WS217). */
	if (server->compose != NULL) {
		facts->scanout_rotation = server->compose->scanout_rotation;
		facts->turn_rotation = server->compose->turn_rotation;
	}

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
 * Enters the game mode for a window: the display thread asks
 * the backend first, without claiming, whether the display can show it
 * straight; scanout_answered goes on with the answer.
 */
static void
scanout_enter(
	struct kwl_server *server,
	struct kwl_object *top)
{
	/* The question, for the display's mode (the physical size). */
	server->scanout_entering = top;
	server->scanout_entering_ms = kwl_milliseconds();
	(void)kwl_display_scanout_query(server, server->compose->mode_width, server->compose->mode_height, scanout_answered, NULL);
}

/*
 * Goes on entering the game mode with the backend's answer: a display that
 * cannot show the window straight (the Raspberry Pi 4's, Linux, FreeBSD)
 * keeps the output open, as closing and opening it again for nothing
 * stopped the Pi 4's firmware video decoder (ws236-p011).  One that can
 * closes the swapchain in a change of the display (libvulkan gives the
 * display back), if the window is still to be shown straight.
 */
static void
scanout_answered(
	struct kwl_server *server,
	int error,
	void *context)
{
	struct kwl_scanout_facts facts;
	struct kwl_object *current;
	struct kwl_object *top;
	unsigned reason;

	/* The window went meanwhile. */
	(void)context;
	top = server->scanout_entering;
	if (top == NULL)
		return;

	/* Refused: the window stays composed until it goes or asks again. */
	if (error != 0) {
		server->scanout_entering = NULL;
		server->scanout_refused = top;
		server->dirty = 1;
		printf("KWL SCANOUT direct=0 reason=backend errno=%d surface=%u client=%llu output=kept\n", error, top->id, (unsigned long long)top->client->number);
		return;
	}

	/* The facts as they are now: a window no longer to be shown straight stays composed. */
	scanout_facts(server, top, &facts);
	reason = kwl_scanout_decide(&facts);
	if (reason != KWL_SCANOUT_DIRECT) {
		server->scanout_entering = NULL;
		return;
	}

	/* A window that is no longer the top one stays composed too. */
	current = kwl_top_window(server);
	if (top != current) {
		server->scanout_entering = NULL;
		return;
	}

	/* The swapchain goes in a change of the display; one running already is waited for by a later pass. */
	error = kwl_display_begin(server, scanout_parked, NULL);
	if (error != 0)
		server->scanout_entering = NULL;
}

/* Entering the game mode, the render thread parked: the swapchain closes. */
static void
scanout_parked(
	struct kwl_server *server,
	int error,
	void *context)
{
	/* The close. */
	(void)error;
	kwl_display_close(server, scanout_closed, context);
}

/*
 * Entering the game mode, the swapchain closed: the display thread has the
 * backend claim the display for its mode (the physical size).  A
 * window gone meanwhile leaves window mode to open the swapchain again at
 * the next pass.
 */
static void
scanout_closed(
	struct kwl_server *server,
	int error,
	void *context)
{
	/* Window mode opens again from the next pass unless the claim holds. */
	(void)error;
	(void)context;
	server->windowed = 0;
	if (server->scanout_entering == NULL) {
		kwl_display_end(server);
		return;
	}

	/* The claim, taken in by scanout_claimed. */
	kwl_display_scanout_open(server, server->compose->mode_width, server->compose->mode_height, &server->scanout, scanout_claimed, NULL);
}

/*
 * Entering the game mode, the display claimed (or not): the window is
 * shown straight from the next pass, and the change ends.  A claim's
 * failure leaves the window composed; a window gone meanwhile gives the
 * claim back.
 */
static void
scanout_claimed(
	struct kwl_server *server,
	int error,
	void *context)
{
	struct kl_backend_scanout *scanout;
	struct kwl_object *top;

	/* The window being entered, if it is still there. */
	(void)context;
	top = server->scanout_entering;
	server->scanout_entering = NULL;

	/* Gone meanwhile: the claim goes back on the display thread, before window mode's open. */
	if (top == NULL) {
		scanout = server->scanout;
		server->scanout = NULL;
		if (scanout != NULL)
			scanout_release(server, scanout, NULL, NULL);
		kwl_display_end(server);
		return;
	}

	/* Refused. */
	if (error != 0) {
		server->scanout = NULL;
		server->scanout_refused = top;
		server->dirty = 1;
		printf("KWL SCANOUT direct=0 reason=backend errno=%d surface=%u client=%llu\n", error, top->id, (unsigned long long)top->client->number);
		kwl_display_end(server);
		return;
	}

	/* Succeeded: the window is shown straight from the next pass. */
	server->scanout_claim++;
	server->scanout_surface = top;
	server->scanout_frames = 0U;
	server->scanout_noted = NULL;
	printf("KWL SCANOUT direct=1 surface=%u client=%llu switch_ms=%llu\n", top->id, (unsigned long long)top->client->number, (unsigned long long)(kwl_milliseconds() - server->scanout_entering_ms));
	kwl_display_end(server);
}

/*
 * Shows a window's new image straight: its present begun, unless one is in
 * flight (its end shows the newest image next).  A present that cannot
 * begin leaves the game mode.
 */
static void
scanout_frame(
	struct kwl_server *server,
	struct kwl_object *top)
{
	struct scanout_present *present;
	struct kwl_object *buffer;
	int error;

	/* Input goes to the window shown. */
	server->front_surface = kwl_desktop_front(server, top);
	kwl_seat_focus(server);

	/* One present at a time. */
	if (server->scanout_flight != NULL)
		return;

	/* Only a new image is presented. */
	buffer = top->current;
	if (buffer == NULL || (buffer == server->scanout_front && !top->fresh))
		return;

	/* The image readied by the backend; a failure is the backend's refusal from now. */
	error = kl_backend_scanout_begin(server->scanout, kwl_gpu_host(), kwl_gpu_resource(buffer));
	if (error != 0) {
		scanout_refuse(server, top, error);
		return;
	}

	/* The present, for the present thread. */
	present = calloc(1, sizeof(*present));
	if (present == NULL) {
		scanout_refuse(server, top, ENOMEM);
		return;
	}

	/* The present carries the scanout and the claim it began on, and is handed to the thread. */
	present->scanout = server->scanout;
	present->claim = server->scanout_claim;
	kwl_lane_submit(&scanout_lane, &present->lane, &scanout_present_ops, 0U);

	/* The present holds the image until it has ended. */
	kwl_buffer_get(buffer);
	server->scanout_flight = buffer;
	top->fresh = 0;
	server->dirty = 0;
	server->damaged = 0;
}

/* Makes a present on the present thread: it returns once the display reads the image. */
static void
scanout_present_run(
	struct kwl_lane_job *lane)
{
	struct scanout_present *present;

	/* The backend's present of the image readied. */
	present = (struct scanout_present *)lane;
	present->result = kl_backend_scanout_present(present->scanout);
}

/*
 * Takes a present that has returned, on the event loop's thread: the
 * display holds its image, the one it replaces is let go and the frame
 * callbacks are done (the next pass shows a newer image).  A present that
 * failed leaves the game mode; one of a game mode left meanwhile is only
 * freed (its images went with the release).
 */
static void
scanout_present_done(
	void *context,
	struct kwl_lane_job *lane)
{
	struct scanout_present *present;
	struct kwl_server *server;
	struct kwl_object *previous;
	int result;

	/* Its result, and the job freed. */
	server = context;
	present = (struct scanout_present *)lane;
	result = present->result;
	if (server->scanout == NULL || present->claim != server->scanout_claim || server->scanout_flight == NULL) {
		free(present);
		return;
	}

	/* The result is read, so the job is freed. */
	free(present);

	/* A failure is the backend's refusal from now. */
	if (result != 0) {
		scanout_refuse(server, server->scanout_surface, result);
		return;
	}

	/* The display holds the image; the one it replaces is let go. */
	previous = server->scanout_front;
	server->scanout_front = server->scanout_flight;
	server->scanout_flight = NULL;
	kwl_buffer_put(previous);
	server->scanout_frames++;

	/* The window draws its next frame; the hidden windows are not kept waiting. */
	scanout_callbacks(server, server->scanout_surface);
}

/* Frees a present not taken (the thread stopped, at the end). */
static void
scanout_present_drop(
	struct kwl_lane_job *lane)
{
	/* Freed. */
	free(lane);
}

/*
 * Gives the display thread the release of a scanout, with the images the
 * display held (their holds are kept until it is done; without memory for
 * that they are let go at once).
 */
static void
scanout_release(
	struct kwl_server *server,
	struct kl_backend_scanout *scanout,
	struct kwl_object *front,
	struct kwl_object *flight)
{
	struct scanout_closing *closing;

	/* The images' record, among the releases waiting. */
	closing = calloc(1, sizeof(*closing));
	if (closing == NULL) {
		kwl_buffer_put(front);
		kwl_buffer_put(flight);
	} else {
		closing->front = front;
		closing->flight = flight;
		closing->next = scanout_closings;
		scanout_closings = closing;
	}

	/* The release, on the display thread after the present running. */
	kwl_display_scanout_close(server, scanout, scanout_released, closing);
}

/* Lets go of the images a release held, once the display is released. */
static void
scanout_released(
	struct kwl_server *server,
	int error,
	void *context)
{
	struct scanout_closing *closing;
	struct scanout_closing **link;

	/* Nothing held (no memory for the record). */
	(void)server;
	(void)error;
	closing = context;
	if (closing == NULL)
		return;

	/* Out of the releases waiting. */
	link = &scanout_closings;
	while (*link != NULL && *link != closing)
		link = &(*link)->next;
	if (*link == closing)
		*link = closing->next;

	/* Succeeded: the images go back to their clients. */
	kwl_buffer_put(closing->front);
	kwl_buffer_put(closing->flight);
	free(closing);
}

/* Leaves the game mode because the display refused a window's image: the window stays composed until it goes or asks again. */
static void
scanout_refuse(
	struct kwl_server *server,
	struct kwl_object *top,
	int error)
{
	/* Logged, then window mode. */
	printf("KWL SCANOUT present errno=%d surface=%u\n", error, top != NULL ? top->id : 0U);
	server->scanout_refused = top;
	kwl_scanout_leave(server, KWL_SCANOUT_REFUSED);
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
