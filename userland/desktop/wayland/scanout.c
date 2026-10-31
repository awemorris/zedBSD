/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The game mode: a fullscreen window that says it shows a video or a game
 * (wp_content_type_v1, content-type.c), alone on the output, draws straight
 * into the images the display shows -- nothing is composed or copied.
 * Every other window, a fullscreen one that does not ask too, is composed
 * with libvulkan as before (display.c).
 *
 * The images are lent (game.c): the compositor makes a display swapchain
 * of its own (VK_KHR_display, at the display's mode, its images allowed to
 * go out), sends the client's Vulkan WSI each image's fd and the lend, and
 * the WSI's chain draws into them.  Only libvulkan is used: the images go
 * out with vkGetMemoryFdKHR, and they are shown with vkQueuePresentKHR.
 *
 * Each pass the rules (scanout-rules.c) decide from the facts gathered
 * here; the window also needs a game object whose chain the display can
 * lend images to (its format's channel order, the display's mode, uses an
 * image the display shows has).  To enter, window mode's swapchain is
 * closed in a change of the display (display-thread.c: the frame in flight
 * completes first), the display thread makes the game chain, and the lend
 * is sent; window mode's passes go on meanwhile.
 *
 * The present thread, one resident thread of a lane named "present",
 * acquires the chain's images and waits until the display has let go of
 * each (the acquire's fence): such an image is the client's, and its
 * buffer goes back to it (wl_buffer.release) when nothing else holds it.
 * A new commit of a lent buffer (its rendering done: the commit's acquire
 * fence) is presented by the present thread, which then waits for the
 * display to give back the image it showed before -- the same image
 * acquired again is how the compositor knows it left the screen.  One
 * present is in flight at a time; the newest image goes next.  Until the
 * client draws into a lent image its own images are not shown (their frame
 * callbacks are done, so it goes on); a client that does not take the
 * lend within SCANOUT_ADOPT_MS, or declines it, is composed.
 *
 * As soon as the facts change -- something to draw over it (an edge's
 * gesture, App Home, a menu, the keyboard), the pointer moving over a
 * video, a screenshot asked, another size -- the game mode is left: the
 * lend is withdrawn (its buffers are the client's ordinary buffers from
 * then, composed as any other), the display thread destroys the game
 * chain once the job in flight has returned, and window mode opens the
 * swapchain again after it (display.c).  The buffers the game chain held
 * are let go when it is destroyed, or when their client goes.  The input
 * goes to the compositor all along.
 *
 * A game that fills the output shows no cursor on any platform, whether
 * it is shown straight or composed (kwl_scanout_cursor_hidden), so its
 * pointer moving does not leave the game mode.
 *
 * Log: "KWL SCANOUT direct=1 surface=N client=C switch_ms=M images=I
 * format=F", "KWL SCANOUT direct=0 reason=R surface=N frames=F" (also
 * once for a window that asks but is not shown straight, for each new
 * reason), "KWL SCANOUT lend refused why=W surface=N" (once for each new
 * reason of a window).
 */

#include "desktop.h"
#include "compose.h"
#include "extras.h"
#include "menu.h"
#include "scanout-rules.h"

#include "lane.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How long the present thread waits for the display to give an image back after a present, in nanoseconds. */
#define SCANOUT_ACQUIRE_NS	100000000ULL

/* How long an acquired image's fence is waited for (the display stops reading it), in nanoseconds. */
#define SCANOUT_FENCE_NS	1000000000ULL

/* How long the client has after the lend to draw into a lent image, in milliseconds. */
#define SCANOUT_ADOPT_MS	2000U

/* The uses a client's chain may have to draw into lent images. */
#define SCANOUT_USAGE		(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | \
				 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)

/*
 * One job of the present thread: whether it presents an image (index) of
 * the game chain first, and whether it then waits for the image shown
 * before to come back (none is shown before the first present), the
 * compositor and the chain, the number of the claim (server->scanout_claim
 * then), and its results -- the present's, and the images the display gave
 * back (one bit each).
 */
struct scanout_present {
	struct kwl_lane_job lane;
	unsigned present;
	unsigned wait;
	struct kwl_compose *compose;
	struct kwl_game_chain *chain;
	uint64_t claim;
	uint32_t index;
	VkResult result;
	uint32_t acquired;
};

static void scanout_present_run(struct kwl_lane_job *lane);
static void scanout_present_done(void *context, struct kwl_lane_job *lane);
static void scanout_present_drop(struct kwl_lane_job *lane);
static VkResult scanout_acquire(struct kwl_compose *compose, struct kwl_game_chain *chain, uint64_t timeout, uint32_t *index);
static void scanout_submit(struct kwl_server *server, unsigned present, uint32_t index);
static void scanout_release(struct kwl_server *server, struct kwl_game_chain *chain);
static void scanout_released(struct kwl_server *server, int error, void *context);
static void scanout_claimed(struct kwl_server *server, int error, void *context);
static void scanout_facts(struct kwl_server *server, struct kwl_object *top, struct kwl_scanout_facts *facts);
static VkFormat scanout_lendable(struct kwl_server *server, struct kwl_object *top);
static int scanout_overlay(struct kwl_server *server, struct kwl_object *top);
static void scanout_enter(struct kwl_server *server, struct kwl_object *top);
static void scanout_parked(struct kwl_server *server, int error, void *context);
static void scanout_closed(struct kwl_server *server, int error, void *context);
static void scanout_lend(struct kwl_game_chain *chain, struct kwl_object *top);
static void scanout_frame(struct kwl_server *server, struct kwl_object *top);
static void scanout_refuse(struct kwl_server *server, struct kwl_object *top, int error);
static void scanout_let_go(struct kwl_game_chain *chain, struct kwl_client *client);
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

/* The game chains whose release the display thread has not done yet (linked by next_closing), the event loop's thread's own. */
static struct kwl_game_chain *scanout_closings;

/* The serial of the last lend; each lend takes the next one (never zero), on the event loop's thread. */
static uint32_t scanout_serial;

/*
 * The window whose lend refusal was logged last and its reason, so that
 * each is logged once; compared, never followed (cleared as the surface
 * goes), on the event loop's thread.
 */
static const struct kwl_object *scanout_lend_noted;
static const char *scanout_lend_why;

/*
 * Runs the game mode's part of a pass, after the commits were taken.
 * Returns 1 when the pass is the game mode's (the client draws into the
 * display's images, nothing is composed), 0 when window mode draws it (the
 * game mode was left, or not entered).
 */
int
kwl_scanout_pass(
	struct kwl_server *server,
	struct kwl_object *top)
{
	struct kwl_scanout_facts facts;
	unsigned reason;
	VkFormat format;
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

	/* Only a window whose chain can draw into the display's images. */
	format = scanout_lendable(server, top);
	if (format == VK_FORMAT_UNDEFINED)
		return 0;

	/*
	 * Enters, while window mode draws on: the swapchain is closed and the
	 * game chain made (scanout_closed), the lend sent (scanout_claimed); a
	 * later pass shows the client's first lent image.  A failure stays
	 * window mode for this window.
	 */
	scanout_enter(server, top);
	return 0;
}

/*
 * Leaves the game mode for a reason (KWL_SCANOUT_*): the lend is
 * withdrawn (the client's lent buffers are ordinary buffers from now,
 * composed as any other), the display thread destroys the game chain once
 * the job in flight has returned, the buffers it held are let go after,
 * and window mode opens the swapchain again from the next pass
 * (server->windowed is 0; its open runs on the display thread after the
 * release).
 */
void
kwl_scanout_leave(
	struct kwl_server *server,
	unsigned reason)
{
	struct kwl_game_chain *chain;
	struct kwl_object *game;
	uint32_t surface_id;

	/* Not in it. */
	chain = server->scanout;
	if (chain == NULL)
		return;

	/* The client hears that the lend is over. */
	game = NULL;
	if (server->scanout_surface != NULL)
		game = server->scanout_surface->game_object;
	if (game != NULL)
		(void)kwl_game_send_withdraw(game, chain->serial);

	/* The chain goes on the display thread (a job's result from now is not this one's). */
	server->scanout = NULL;
	scanout_release(server, chain);

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

	/* The thread, which waits for the first job. */
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
 * thread's job returns and the thread ends.
 */
void
kwl_scanout_stop(
	struct kwl_server *server)
{
	/* The game mode left; its release waits for the job running. */
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

	/* Each job that has returned. */
	(void)kwl_lane_take(&scanout_lane, server);
}

/*
 * Waits until the present thread has no job given or running: the display
 * thread, before it destroys the game chain (whose images the present
 * thread's job uses until it returns).
 */
void
kwl_scanout_present_wait(void)
{
	/* Every job given has returned. */
	kwl_lane_wait(&scanout_lane);
}

/*
 * Lets go of a client's buffers still held by the game chain or by one
 * whose release is not done yet, as the client goes (its buffers are freed
 * after).
 */
void
kwl_scanout_client_gone(
	struct kwl_server *server,
	struct kwl_client *client)
{
	struct kwl_game_chain *chain;

	/* The chain lent now, and each one being released. */
	if (server->scanout != NULL)
		scanout_let_go(server->scanout, client);
	for (chain = scanout_closings; chain != NULL; chain = chain->next_closing)
		scanout_let_go(chain, client);
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

	/* A window being entered, a window the display refused, and the last ones logged. */
	if (server->scanout_entering == surface)
		server->scanout_entering = NULL;
	if (server->scanout_refused == surface)
		server->scanout_refused = NULL;
	if (server->scanout_noted == surface)
		server->scanout_noted = NULL;
	if (scanout_lend_noted == surface)
		scanout_lend_noted = NULL;
}

/*
 * Takes a client's tag: a buffer it made holds lent image index of the
 * lend serial (game.c checked that it is a live GPU buffer of the client).
 * The buffer is the image's from now: when the present thread has the
 * image (the client's to draw into) the buffer goes back to the client at
 * once; otherwise the game mode holds it until the display gives the image
 * back.  A tag of another lend, of an image tagged already, or of a buffer
 * of another size is passed over.
 */
void
kwl_scanout_tag(
	struct kwl_server *server,
	struct kwl_object *game,
	struct kwl_object *buffer,
	uint32_t serial,
	uint32_t index)
{
	struct kwl_game_chain *chain;
	uint32_t width;
	uint32_t height;

	/* The lend now, of the window shown, and an image of it not tagged before. */
	chain = server->scanout;
	if (chain == NULL || serial != chain->serial || game->surface != server->scanout_surface)
		return;
	if (index >= chain->output.image_count || chain->buffers[index] != NULL || buffer->lend_chain != NULL)
		return;

	/* A buffer of the image's size. */
	kwl_buffer_size(buffer, &width, &height);
	if (width != chain->output.width || height != chain->output.height) {
		printf("KWL SCANOUT tag refused buffer=%ux%u image=%ux%u\n", width, height, chain->output.width, chain->output.height);
		return;
	}

	/* The buffer holds the image; a release of it reaches the client (it counts as committed). */
	chain->buffers[index] = buffer;
	buffer->lend_chain = chain;
	buffer->lend_index = index;
	buffer->busy = 1;
	kwl_buffer_get(buffer);

	/* The client's image now goes back to it; one the swapchain has stays held until it comes back. */
	if (chain->state[index] == KWL_GAME_FREE) {
		kwl_buffer_put(buffer);
		return;
	}

	/* Succeeded: held for the swapchain. */
	chain->held[index] = 1U;
}

/*
 * Takes a client's refusal of the lend serial: the window is composed
 * until it goes or asks again.
 */
void
kwl_scanout_decline(
	struct kwl_server *server,
	struct kwl_object *game,
	uint32_t serial)
{
	struct kwl_game_chain *chain;
	struct kwl_object *top;

	/* The lend now, of the window shown. */
	chain = server->scanout;
	if (chain == NULL || serial != chain->serial || game->surface != server->scanout_surface)
		return;

	/* Logged, then window mode. */
	top = server->scanout_surface;
	printf("KWL SCANOUT lend refused why=declined surface=%u\n", top->id);
	server->scanout_refused = top;
	kwl_scanout_leave(server, KWL_SCANOUT_REFUSED);
}

/*
 * Forgets a buffer that is freed: the game chain that lent its image no
 * longer names it (the game mode held it no longer, or it would not be
 * freed).
 */
void
kwl_scanout_buffer_gone(
	struct kwl_object *buffer)
{
	struct kwl_game_chain *chain;
	uint32_t index;

	/* A buffer of no lent image. */
	chain = buffer->lend_chain;
	if (chain == NULL)
		return;

	/* The chain's image no longer has a buffer. */
	index = buffer->lend_index;
	if (index < KWL_GAME_IMAGES && chain->buffers[index] == buffer) {
		chain->buffers[index] = NULL;
		chain->held[index] = 0U;
	}

	/* Succeeded: forgotten. */
	buffer->lend_chain = NULL;
}

/*
 * Tells whether the cursor is left out because a game fills the output:
 * the top window is a fullscreen one that says it shows a game, and
 * nothing is drawn over it.  Neither the cursor plane nor the composed
 * frame shows the cursor then, whether the client draws into the display's
 * images or the compositor composes it, on every backend.  Returns 1 or 0.
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
 * Tells the format the game chain is made in for a window whose chain can
 * draw into the display's images, or VK_FORMAT_UNDEFINED: the window has a
 * game object, its chain (as the client last told) has the display's mode,
 * uses an image the display shows has, an image count the game chain can
 * add the display's two to, and a four-byte format whose channel order the
 * display shows in UNORM (the client's format reads the same bytes); the
 * display's chain has not shown that it cannot lend.  A refusal is logged
 * once for each new reason of the window.
 */
static VkFormat
scanout_lendable(
	struct kwl_server *server,
	struct kwl_object *top)
{
	struct kwl_object *game;
	const char *why;
	VkFormat format;

	/* The window's game object, and the display's chain able to lend at all. */
	game = top->game_object;
	format = VK_FORMAT_UNDEFINED;
	why = NULL;
	if (game == NULL) {
		why = "no-game-object";
	} else if (!server->compose->lendable || server->scanout_unlendable) {
		why = "display";
	} else if (game->game_count == 0U || game->game_count + 2U > KWL_GAME_IMAGES) {
		why = "count";
	} else if (game->game_width != server->compose->mode_width || game->game_height != server->compose->mode_height) {
		why = "size";
	} else if ((game->game_usage & ~(uint32_t)SCANOUT_USAGE) != 0U) {
		why = "usage";
	} else if (game->game_format == (uint32_t)VK_FORMAT_B8G8R8A8_UNORM ||
		   game->game_format == (uint32_t)VK_FORMAT_B8G8R8A8_SRGB) {
		/* The display shows BGRA bytes as they are. */
		format = VK_FORMAT_B8G8R8A8_UNORM;
	} else if (game->game_format == (uint32_t)VK_FORMAT_R8G8B8A8_UNORM ||
		   game->game_format == (uint32_t)VK_FORMAT_R8G8B8A8_SRGB) {
		/* The display shows RGBA bytes as they are. */
		format = VK_FORMAT_R8G8B8A8_UNORM;
	} else {
		why = "format";
	}

	/* A refusal, logged once for each new reason of the window. */
	if (format == VK_FORMAT_UNDEFINED) {
		if (scanout_lend_noted != top || scanout_lend_why != why) {
			scanout_lend_noted = top;
			scanout_lend_why = why;
			printf("KWL SCANOUT lend refused why=%s surface=%u\n", why, top->id);
		}
		return VK_FORMAT_UNDEFINED;
	}

	/* Succeeded: the game chain's format. */
	return format;
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
 * Enters the game mode for a window: window mode's swapchain closes in a
 * change of the display (scanout_parked), then the game chain is made.
 */
static void
scanout_enter(
	struct kwl_server *server,
	struct kwl_object *top)
{
	int error;

	/* The change; one running already is waited for by a later pass. */
	server->scanout_entering = top;
	server->scanout_entering_ms = kwl_milliseconds();
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
 * Entering the game mode, the swapchain closed: the display thread makes
 * the game chain for the window's chain (scanout_claimed).  A window gone
 * meanwhile, or one whose chain no longer fits, leaves window mode to open
 * the swapchain again at the next pass.
 */
static void
scanout_closed(
	struct kwl_server *server,
	int error,
	void *context)
{
	struct kwl_game_chain *chain;
	struct kwl_object *top;
	VkFormat format;
	uint32_t index;

	/* Window mode opens again from the next pass unless the game chain holds. */
	(void)error;
	(void)context;
	server->windowed = 0;
	top = server->scanout_entering;
	format = VK_FORMAT_UNDEFINED;
	if (top != NULL)
		format = scanout_lendable(server, top);
	if (format == VK_FORMAT_UNDEFINED) {
		server->scanout_entering = NULL;
		kwl_display_end(server);
		return;
	}

	/* The record, which the display thread fills. */
	chain = calloc(1, sizeof(*chain));
	if (chain == NULL) {
		server->scanout_entering = NULL;
		kwl_display_end(server);
		return;
	}
	chain->output.format = format;
	chain->wanted = top->game_object->game_count;
	chain->flight = -1;
	for (index = 0U; index < KWL_GAME_IMAGES; index++)
		chain->fds[index] = -1;

	/* The game chain, taken in by scanout_claimed. */
	kwl_display_scanout_open(server, chain, scanout_claimed, chain);
}

/*
 * Entering the game mode, the game chain made (or not): the lend is sent
 * and the present thread acquires the images the client may draw into
 * first; the change ends.  A failure leaves the window composed (a display
 * whose chain cannot lend its images is not tried again); a window gone
 * meanwhile gives the chain back.
 */
static void
scanout_claimed(
	struct kwl_server *server,
	int error,
	void *context)
{
	struct kwl_game_chain *chain;
	struct kwl_object *top;

	/* The window being entered, if it is still there with its game object. */
	chain = context;
	top = server->scanout_entering;
	server->scanout_entering = NULL;
	if (top != NULL && top->game_object == NULL)
		top = NULL;

	/* Gone meanwhile: the chain goes back on the display thread, before window mode's open. */
	if (top == NULL) {
		if (error == 0)
			scanout_release(server, chain);
		else
			free(chain);
		kwl_display_end(server);
		return;
	}

	/* Refused: the window stays composed until it goes or asks again. */
	if (error != 0) {
		free(chain);
		if (error == ENOTSUP)
			server->scanout_unlendable = 1U;
		server->scanout_refused = top;
		server->dirty = 1;
		printf("KWL SCANOUT direct=0 reason=backend errno=%d surface=%u client=%llu\n", error, top->id, (unsigned long long)top->client->number);
		kwl_display_end(server);
		return;
	}

	/* The client draws into the chain's images from the next pass. */
	server->scanout = chain;
	server->scanout_claim++;
	server->scanout_surface = top;
	server->scanout_frames = 0U;
	server->scanout_noted = NULL;
	printf(
	    "KWL SCANOUT direct=1 surface=%u client=%llu switch_ms=%llu images=%u format=%d\n",
	    top->id,
	    (unsigned long long)top->client->number,
	    (unsigned long long)(kwl_milliseconds() - server->scanout_entering_ms),
	    chain->output.image_count,
	    (int)chain->output.format);

	/* The lend, and the images the client may draw into first. */
	scanout_lend(chain, top);
	scanout_submit(server, 0U, 0U);

	/* Succeeded: the change ends. */
	kwl_display_end(server);
}

/* Sends the window's game object the chain's images and the lend. */
static void
scanout_lend(
	struct kwl_game_chain *chain,
	struct kwl_object *top)
{
	struct kwl_object *game;
	uint32_t index;
	int fd;
	int error;

	/* The lend's serial, never zero. */
	scanout_serial++;
	if (scanout_serial == 0U)
		scanout_serial = 1U;
	chain->serial = scanout_serial;
	chain->lent_ms = kwl_milliseconds();

	/* Each image's fd goes to the client (a send that fails retires the client, whose window then goes). */
	game = top->game_object;
	for (index = 0U; index < chain->output.image_count; index++) {
		fd = chain->fds[index];
		chain->fds[index] = -1;
		error = kwl_game_send_image(game, chain->serial, index, fd);
		if (error != 0)
			return;
	}

	/* The lend: the count, the display's mode, the format, the uses; the layout is in each image's own description. */
	(void)kwl_game_send_lend(
		game,
		chain->serial,
		chain->output.image_count,
		chain->output.width,
		chain->output.height,
		(uint32_t)chain->output.format,
		(uint32_t)chain->usage,
		0U);
}

/*
 * Shows a window's new image: a lent buffer's image is presented, unless
 * one is in flight (its end presents the newest image next).  The client's
 * own image before it draws into the lent ones is not shown; its frame
 * callbacks are done so that it draws on.  A client that has not drawn into
 * a lent image within SCANOUT_ADOPT_MS is composed.
 */
static void
scanout_frame(
	struct kwl_server *server,
	struct kwl_object *top)
{
	struct kwl_game_chain *chain;
	struct kwl_object *buffer;
	uint32_t index;
	uint64_t now;

	/* Input goes to the window shown. */
	server->front_surface = kwl_desktop_front(server, top);
	kwl_seat_focus(server);

	/* A client that has not taken the lend in time is composed. */
	chain = server->scanout;
	now = kwl_milliseconds();
	if (chain->shown == 0U && chain->flight < 0 && now - chain->lent_ms > SCANOUT_ADOPT_MS) {
		printf("KWL SCANOUT lend refused why=not-taken surface=%u ms=%llu\n", top->id, (unsigned long long)(now - chain->lent_ms));
		server->scanout_refused = top;
		kwl_scanout_leave(server, KWL_SCANOUT_REFUSED);
		return;
	}

	/* Only a new image. */
	buffer = top->current;
	if (buffer == NULL || !top->fresh)
		return;

	/* The client's own image: not shown, and its frame callbacks done so that it goes on. */
	if (buffer->lend_chain != chain) {
		top->fresh = 0;
		server->dirty = 0;
		server->damaged = 0;
		scanout_callbacks(server, top);
		return;
	}

	/* One present at a time: the newest image goes when the present in flight has returned. */
	if (chain->flight >= 0)
		return;

	/* A lent image the client was given; any other commit of it shows nothing new. */
	index = buffer->lend_index;
	if (chain->state[index] != KWL_GAME_FREE) {
		top->fresh = 0;
		return;
	}

	/* The present holds the image (for the swapchain) until the display gives it back. */
	chain->state[index] = KWL_GAME_PRESENTING;
	kwl_buffer_get(buffer);
	chain->held[index] = 1U;
	chain->flight = (int)index;
	scanout_submit(server, 1U, index);
	top->fresh = 0;
	server->dirty = 0;
	server->damaged = 0;
}

/*
 * Gives the present thread a job of the game chain shown: an image to
 * present first (present, index), then the images the display gives back.
 * Without memory for the job the game mode is left.
 */
static void
scanout_submit(
	struct kwl_server *server,
	unsigned present,
	uint32_t index)
{
	struct scanout_present *job;

	/* The job. */
	job = calloc(1, sizeof(*job));
	if (job == NULL) {
		scanout_refuse(server, server->scanout_surface, ENOMEM);
		return;
	}

	/* The job carries the chain and the claim it began on, and is handed to the thread. */
	job->present = present;
	if (present && server->scanout->shown != 0U)
		job->wait = 1U;
	job->compose = server->compose;
	job->chain = server->scanout;
	job->claim = server->scanout_claim;
	job->index = index;
	job->result = VK_SUCCESS;
	kwl_lane_submit(&scanout_lane, &job->lane, &scanout_present_ops, 0U);
}

/*
 * Runs a job on the present thread: the present of an image (under the
 * queue's lock), then the images the display gives back -- after a
 * present, the first is waited for (the image shown before leaves the
 * screen), then any others that are back already.
 */
static void
scanout_present_run(
	struct kwl_lane_job *lane)
{
	struct scanout_present *job;
	struct kwl_game_chain *chain;
	VkPresentInfoKHR info;
	VkResult result;
	uint64_t timeout;
	uint32_t taken;
	uint32_t index;

	/* The present, when the job has one. */
	job = (struct scanout_present *)lane;
	chain = job->chain;
	timeout = 0U;
	if (job->present) {
		memset(&info, 0, sizeof(info));
		info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		info.swapchainCount = 1U;
		info.pSwapchains = &chain->output.swapchain;
		info.pImageIndices = &job->index;
		kwl_queue_lock(job->compose);

		result = vkQueuePresentKHR(job->compose->queue, &info);

		kwl_queue_unlock(job->compose);

		/* A failed present acquires nothing more. */
		job->result = result;
		if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
			return;
	}

	/* The image shown before comes back once the display shows this one. */
	if (job->wait)
		timeout = SCANOUT_ACQUIRE_NS;

	/* The images the display gives back: the first waited for after a present, then the ones back already. */
	result = VK_SUCCESS;
	for (taken = 0U; taken < chain->output.image_count; taken++) {
		result = scanout_acquire(job->compose, chain, timeout, &index);
		if (result != VK_SUCCESS)
			break;
		job->acquired |= 1U << index;
		timeout = 0U;
	}

	/* None back is no failure; an error is. */
	if (result < VK_SUCCESS)
		job->result = result;
}

/*
 * Acquires an image of the game chain within a timeout and waits until the
 * display has let go of it (the acquire's fence).  Returns VK_SUCCESS with
 * its index, VK_NOT_READY or VK_TIMEOUT when none came back, or an error
 * (an image acquired whose fence did not come is VK_ERROR_DEVICE_LOST).
 */
static VkResult
scanout_acquire(
	struct kwl_compose *compose,
	struct kwl_game_chain *chain,
	uint64_t timeout,
	uint32_t *index)
{
	VkResult result;

	/* The fence, unsignaled, for this acquire. */
	result = vkResetFences(compose->device, 1U, &chain->acquired);
	if (result != VK_SUCCESS)
		return result;

	/* An image the swapchain gives back, if one comes in time. */
	result = vkAcquireNextImageKHR(compose->device, chain->output.swapchain, timeout, VK_NULL_HANDLE, chain->acquired, index);
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
		return result;

	/* The display has let go of it once the fence is signaled. */
	result = vkWaitForFences(compose->device, 1U, &chain->acquired, VK_TRUE, SCANOUT_FENCE_NS);
	if (result != VK_SUCCESS)
		return VK_ERROR_DEVICE_LOST;

	/* Succeeded: the image is the client's to draw into. */
	return VK_SUCCESS;
}

/*
 * Takes a job that has returned, on the event loop's thread: the image
 * presented is the swapchain's (held until it comes back), each image the
 * display gave back is the client's (its buffer released when nothing else
 * holds it), and a present's frame callbacks are done (the next pass shows
 * a newer image).  A failure leaves the game mode; a job of a game mode
 * left meanwhile is only freed (its buffers went with the release).
 */
static void
scanout_present_done(
	void *context,
	struct kwl_lane_job *lane)
{
	struct scanout_present *job;
	struct kwl_game_chain *chain;
	struct kwl_object *buffer;
	struct kwl_server *server;
	uint32_t index;

	/* A job of the game chain shown now, else only freed. */
	server = context;
	job = (struct scanout_present *)lane;
	chain = server->scanout;
	if (chain == NULL || job->chain != chain || job->claim != server->scanout_claim) {
		free(job);
		return;
	}

	/* The image presented is the swapchain's again (still held). */
	if (job->present) {
		chain->state[job->index] = KWL_GAME_CHAIN;
		chain->flight = -1;
	}

	/* A failure is the display's refusal from now. */
	if (job->result != VK_SUCCESS && job->result != VK_SUBOPTIMAL_KHR) {
		printf("KWL SCANOUT game chain step=%s result=%d\n", job->present ? "present" : "acquire", (int)job->result);
		free(job);
		scanout_refuse(server, server->scanout_surface, EIO);
		return;
	}

	/* Each image given back is the client's; its buffer goes back unless the surface still holds it. */
	for (index = 0U; index < chain->output.image_count; index++) {
		if ((job->acquired & (1U << index)) == 0U)
			continue;
		chain->state[index] = KWL_GAME_FREE;
		buffer = chain->buffers[index];
		if (chain->held[index] && buffer != NULL) {
			chain->held[index] = 0U;
			kwl_buffer_put(buffer);
		}
	}

	/* A present's frame: counted, and the windows draw their next one (the hidden ones are not kept waiting). */
	if (job->present) {
		chain->shown++;
		server->scanout_frames++;
		scanout_callbacks(server, server->scanout_surface);
	}

	/* Succeeded: the job is done. */
	free(job);
}

/* Frees a job not taken (the thread stopped, at the end). */
static void
scanout_present_drop(
	struct kwl_lane_job *lane)
{
	/* Freed. */
	free(lane);
}

/*
 * Gives the display thread the release of a game chain; the buffers it
 * holds are let go when it is done.
 */
static void
scanout_release(
	struct kwl_server *server,
	struct kwl_game_chain *chain)
{
	/* Among the releases waiting. */
	chain->next_closing = scanout_closings;
	scanout_closings = chain;

	/* The release, on the display thread after the job running. */
	kwl_display_scanout_close(server, chain, scanout_released, chain);
}

/*
 * Lets go of the buffers a game chain held, once the display thread has
 * destroyed it: each goes back to its client unless something else holds
 * it, and no buffer names the chain any more.
 */
static void
scanout_released(
	struct kwl_server *server,
	int error,
	void *context)
{
	struct kwl_game_chain *chain;
	struct kwl_game_chain **link;

	/* Out of the releases waiting. */
	(void)server;
	(void)error;
	chain = context;
	link = &scanout_closings;
	while (*link != NULL && *link != chain)
		link = &(*link)->next_closing;
	if (*link == chain)
		*link = chain->next_closing;

	/* Succeeded: the buffers go back to their clients, and the record goes. */
	scanout_let_go(chain, NULL);
	free(chain);
}

/*
 * Lets go of a game chain's buffers of one client (every client's for
 * NULL): none names the chain any more, and those it held go back.
 */
static void
scanout_let_go(
	struct kwl_game_chain *chain,
	struct kwl_client *client)
{
	struct kwl_object *buffer;
	unsigned held;
	uint32_t index;

	/* Each image's buffer, if it is the client's. */
	for (index = 0U; index < KWL_GAME_IMAGES; index++) {
		buffer = chain->buffers[index];
		if (buffer == NULL)
			continue;
		if (client != NULL && buffer->client != client)
			continue;

		/* Untied before its hold goes (which may free it). */
		held = chain->held[index];
		chain->buffers[index] = NULL;
		chain->held[index] = 0U;
		buffer->lend_chain = NULL;
		if (held)
			kwl_buffer_put(buffer);
	}
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
