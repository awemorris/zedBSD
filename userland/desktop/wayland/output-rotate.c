/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * An output's angle changed while it is shown (WS217, ws217-p002b; the
 * design is plan/ws217/phase002b/phase.md, section 3).
 *
 * Two steps are kept apart (review-4 S12): the user's choice of an angle
 * (kwl_output_select_rotation: the display's mask, the choice and
 * displays.conf), and the change of the angle shown
 * (kwl_output_apply_rotation: how it is shown and what follows), which a
 * game's pause of the turn (ws217-p012) also takes without touching the
 * choice.  An opening of the output shows the choice by itself
 * (compose_rotation_choose, kwl_output_rotated).
 *
 * The turn between angles the compositor turns exchanges the images of
 * the logical size: the new turn image and the look's wallpaper are made
 * first, so that a failure changes nothing; then, with the render thread
 * parked and the device idle, the new size is set, the windows fitted and
 * the clients told (their events sent at once, so that they draw the new
 * size meanwhile), and the old images go and the optional caches (the
 * scene's copy, the layers' pictures, the backdrop) are made again at the
 * new size.  A change of the scanout's angle (the display's hardware, 180)
 * opens the output again (ws217-p010).
 */

#include "kwl.h"
#include "compose.h"
#include "desktop.h"
#include "extras.h"
#include "scanout-rules.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * When each step of a change of the angle shown ended (kwl_milliseconds),
 * for the log's times (BUG-306: a turn that took 7 to 12 s); a step not
 * taken ends when the one before it did.
 */
struct output_rotate_clock {
	/* The change began. */
	uint64_t started;

	/* The new turn image made, and the look's wallpaper at the new size. */
	uint64_t turn_made;
	uint64_t glass_made;

	/* The fingers and the pens ended (everything made before the render thread parks). */
	uint64_t prepared;

	/* The render thread parked, and the device idle. */
	uint64_t parked;
	uint64_t idle;

	/* The pointer and the windows fitted, and the clients told (their events sent). */
	uint64_t fitted;
	uint64_t told;

	/* The turn image and the wallpaper exchanged, then the caches made again and the thread going on. */
	uint64_t exchanged;
	uint64_t swapped;

	/* The choice given to displays.conf's writer. */
	uint64_t saved;
};

/*
 * What a turn to another angle made before the clients are told, kept
 * until its images are exchanged (output_rotate_turn_begin, then
 * output_rotate_turn_end, with the render thread parked between them):
 * the new turn image, whether the logical size changes, and whether the
 * turn image of the size serves the new angle.  The look's wallpaper at a
 * new size is made after, off the event loop (kwl_glass_resize: the
 * picture read and decoded by a thread of its own and drawn and blurred
 * by the blur thread; the old one is drawn stretched meanwhile).
 */
struct output_rotate_work {
	struct kwl_turn fresh;
	unsigned resized;
	unsigned reuse;
};

/*
 * The timer of the old frame's hold after a turn (server->rotate_hold_ms):
 * armed while the hold waits for windows, for KWL_ROTATE_HOLD_MS after it
 * began, when the scheduler's pass shows the new size anyway
 * (kwl_output_rotate_held looks at the time).  A hold ended meanwhile
 * leaves it armed; it then fires once for nothing.  Readied by the first
 * hold; only the event loop touches it.
 */
static struct kwl_timer output_rotate_hold_timer;

static int output_rotate_turn_begin(struct kwl_server *server, const struct kwl_rotate_state *after, struct output_rotate_clock *clock, struct output_rotate_work *work);
static void output_rotate_turn_end(struct kwl_server *server, struct output_rotate_work *work, struct output_rotate_clock *clock);
static void output_rotate_flush(struct kwl_server *server);
static const char *output_rotate_heads(const struct kwl_compose *compose);
static void output_rotate_caches(struct kwl_server *server, unsigned scene_kept, unsigned resized);
static const char *output_rotate_mode_name(unsigned mode);
static void output_rotate_times(const struct output_rotate_clock *clock);
static void output_rotate_clock_fill(struct output_rotate_clock *clock, uint64_t now);
static int output_rotate_waits(struct kwl_server *server, const struct kwl_object *surface);
static void output_rotate_hold_fired(void *context, struct kwl_timer *timer);

/*
 * Takes the user's choice of an output's angle: refused outside the
 * display's mask; saved, and shown now when the output is open (or at its
 * next opening).  *applied says whether the angle is shown now, *saved
 * whether displays.conf's writing was given (0: its thread writes it,
 * heads.c) or its errno.  Returns 0, EINVAL
 * for a number that is not an angle, ENOTSUP for an angle the display may
 * not take (or a head's, ws217-p005), or the error of showing it (the
 * choice then goes back to what it was).
 */
int
kwl_output_select_rotation(
	struct kwl_server *server,
	unsigned slot,
	unsigned rotation,
	unsigned *applied,
	int *saved)
{
	struct kwl_compose *compose;
	unsigned previous;
	unsigned decision;
	unsigned open;
	unsigned scanning_out;
	unsigned stored;
	const char *heads;
	int valid;
	int error;

	/* Nothing applied or written yet. */
	*applied = 0U;
	*saved = -1;
	compose = server->compose;

	/* An angle, of the anchor (a head's is ws217-p005), outside the login screen (D9). */
	valid = kwl_rotate_valid(rotation);
	if (!valid)
		return EINVAL;
	if (compose == NULL || slot != KWL_PLANE_ANCHOR || server->greeter)
		return ENOTSUP;

	/* What the choice does, against the mask, the choice and the angle shown now. */
	open = 0U;
	if (compose->output_open && !compose->output_lost)
		open = 1U;
	scanning_out = 0U;
	if (server->scanout != NULL)
		scanning_out = 1U;
	decision = kwl_rotate_select(rotation, compose->rotations, compose->rotation_selected, compose->rotation, open, scanning_out);
	if (decision == KWL_ROTATE_SELECT_REFUSE) {
		printf("KWL OUTPUT rotate refused key=%s rotation=%u reason=mask mask=%u why=%u\n", compose->display_name, rotation,
		    compose->rotations, compose->rotate_why);
		return ENOTSUP;
	}

	/*
	 * The same choice shown changes nothing on the display; displays.conf
	 * is written when it says another angle (a test's policy showed 0 and
	 * kept the line, T1-583), so the choice is the one kept.
	 */
	if (decision == KWL_ROTATE_SELECT_NOTHING) {
		*applied = 1U;
		stored = kwl_displays_rotation(&compose->config, compose->display_name);
		if (stored == rotation)
			return 0;
		error = kwl_displays_set_rotation(&compose->config, compose->display_name, rotation);
		if (error != 0)
			return error;
		*saved = kwl_heads_config_save(server);
		printf("KWL OUTPUT rotate key=%s selected=%u saved=%d applied=1 reason=kept\n", compose->display_name, rotation, *saved);
		kwl_displays_tell(server);
		return 0;
	}

	/*
	 * Not beside an extended display shown (review M4): the heads' places
	 * are checked only when they open, and the anchor's new size would
	 * leave a gap or an overlap until ws217-p005 places them again.  Nor
	 * while a mirror shows the picture (ws217-p003 review M6): a device on
	 * the mirror's display would map by the anchor's angle until
	 * ws217-p006 maps it by the display it is on.
	 */
	heads = output_rotate_heads(compose);
	if (heads != NULL) {
		printf("KWL OUTPUT rotate refused key=%s rotation=%u reason=%s\n", compose->display_name, rotation, heads);
		return ENOTSUP;
	}

	/* The choice, kept for displays.conf (a closed output's next opening shows it). */
	previous = compose->rotation_selected;
	compose->rotation_selected = rotation;
	error = kwl_displays_set_rotation(&compose->config, compose->display_name, rotation);
	if (error != 0) {
		compose->rotation_selected = previous;
		return error;
	}

	/* A closed output: saved, shown at its next opening. */
	if (decision == KWL_ROTATE_SELECT_SAVE) {
		*saved = kwl_heads_config_save(server);
		printf("KWL OUTPUT rotate key=%s selected=%u saved=%d applied=0 reason=closed\n", compose->display_name, rotation, *saved);
		kwl_displays_tell(server);
		return 0;
	}

	/* A client's buffer on the display: saved, the display given back to the desktop, whose opening shows it. */
	if (decision == KWL_ROTATE_SELECT_LEAVE) {
		*saved = kwl_heads_config_save(server);
		kwl_scanout_leave(server, KWL_SCANOUT_ROTATED);
		printf("KWL OUTPUT rotate key=%s selected=%u saved=%d applied=0 reason=scanout\n", compose->display_name, rotation, *saved);
		kwl_displays_tell(server);
		return 0;
	}

	/* Shown now; a failure puts the choice back as it was. */
	error = kwl_output_apply_rotation(server, slot, rotation, KWL_ROTATE_MODE_USER);
	if (error != 0) {
		compose->rotation_selected = previous;
		(void)kwl_displays_set_rotation(&compose->config, compose->display_name, previous);
		return error;
	}

	/* Succeeded: shown now, saved with the turn (SAVE), and Settings told the new angle and size. */
	*applied = 1U;
	*saved = compose->rotate_saved;
	kwl_displays_tell(server);
	return 0;
}

/*
 * Changes the angle an open output shows (the anchor's; a head's is
 * ws217-p005), by mode (KWL_ROTATE_MODE_*): the turn's images exchanged,
 * the pointer kept on the same physical point, the desktop fitted to a new
 * logical size, the clients told, and the user's change of choice saved.
 * Returns 0, ENOTSUP for an angle the display may not take or one that
 * needs the output opened again (the scanout's 180, ws217-p010), ENODEV
 * without an open output, or the error of making the new images (nothing
 * changed then).
 */
int
kwl_output_apply_rotation(
	struct kwl_server *server,
	unsigned slot,
	unsigned rotation,
	unsigned mode)
{
	struct kwl_compose *compose;
	struct kwl_rotate_state after;
	struct output_rotate_clock clock;
	struct output_rotate_work work;
	char names[64];
	int32_t pointer[4];
	unsigned steps;
	unsigned before_rotation;
	int error;

	/* An open output's anchor. */
	compose = server->compose;
	if (compose == NULL || slot != KWL_PLANE_ANCHOR)
		return ENOTSUP;
	if (!compose->output_open || compose->output_lost)
		return ENODEV;

	/* How the angle is shown on this display. */
	memset(&after, 0, sizeof(after));
	error = kwl_rotate_plan(rotation, compose->rotations, compose->hw180, &after.scanout, &after.turn);
	if (error != 0)
		return error;

	/* What the output shows after, against what it last opened or turned to (review-4 C7). */
	kwl_rotate_size(rotation, compose->mode_width, compose->mode_height, &after.width, &after.height);
	after.selected = compose->rotation_selected;
	after.rotation = rotation;
	steps = kwl_rotate_steps(&compose->opened, &after, mode);
	if (steps == 0U)
		return 0;

	/* Another angle of the display's hardware opens the output again: not in this build yet (ws217-p010). */
	if ((steps & KWL_ROTATE_STEP_REOPEN) != 0U) {
		printf("KWL OUTPUT rotate refused key=%s rotation=%u reason=reopen\n", compose->display_name, rotation);
		return ENOTSUP;
	}

	/*
	 * Another turn: its new images made (nothing changes when they cannot
	 * be), the render thread parked and the new angle and size set; its
	 * images are exchanged after the clients are told.
	 */
	memset(&clock, 0, sizeof(clock));
	memset(&work, 0, sizeof(work));
	clock.started = kwl_milliseconds();
	before_rotation = compose->rotation;
	if ((steps & KWL_ROTATE_STEP_TURN) != 0U) {
		error = output_rotate_turn_begin(server, &after, &clock, &work);
		if (error != 0)
			return error;
	}

	/* The same angle (a choice saved alone): the size is set as it is, and no image was made. */
	if ((steps & KWL_ROTATE_STEP_TURN) == 0U) {
		kwl_output_size_set(server, compose->mode_width, compose->mode_height, rotation);
		output_rotate_clock_fill(&clock, clock.started);
	}

	/*
	 * The pointer first, from its old logical place to the same point of
	 * the panel at the new angle (review S1: a clamp to the new size first
	 * would move it); then the desktop fitted to the new size (the windows,
	 * an arrangement whose windows no longer fit ending as at an output's
	 * move, review-4 M21; the look's images are exchanged with the turn).
	 */
	pointer[0] = -1;
	pointer[1] = -1;
	pointer[2] = -1;
	pointer[3] = -1;
	if ((steps & KWL_ROTATE_STEP_ROTATED) != 0U)
		kwl_output_pointer_turn(server, before_rotation, rotation, pointer);
	if ((steps & KWL_ROTATE_STEP_RESIZED) != 0U) {
		kwl_output_rotate_hold(server);
		kwl_output_fit(server, 0U);
	}
	clock.fitted = kwl_milliseconds();

	/*
	 * The clients told the new size (wl_output's mode, and the floating
	 * windows' bounds now rather than at the next pass), and the input
	 * thread computes the motions over the new angle (the pointer's warp
	 * gave it already when it ran).
	 */
	if ((steps & KWL_ROTATE_STEP_RESIZED) != 0U) {
		kwl_outputs_changed(server);
		kwl_window_bounds_refresh(server);
	}
	kwl_input_geometry(server);

	/*
	 * What the clients were told leaves now, before the turn's images are
	 * exchanged (BUG-306: a second on a virtual GPU): the windows make
	 * their images of the new size meanwhile, and are ready to be shown
	 * when the turn ends instead of a second or more after it.
	 */
	if ((steps & KWL_ROTATE_STEP_TURN) != 0U)
		output_rotate_flush(server);
	clock.told = kwl_milliseconds();

	/* The turn's images exchanged, and the render thread going on with them. */
	if ((steps & KWL_ROTATE_STEP_TURN) != 0U)
		output_rotate_turn_end(server, &work, &clock);

	/* Nothing to exchange: the exchange ended when the clients were told. */
	if (clock.exchanged == 0U)
		clock.exchanged = clock.told;
	if (clock.swapped == 0U)
		clock.swapped = clock.told;

	/*
	 * The old frame stays for the windows waited for from now, when frames
	 * can be drawn again, not from when they were told (rotate.h's
	 * KWL_ROTATE_HOLD_MS).
	 */
	if ((steps & KWL_ROTATE_STEP_RESIZED) != 0U && server->rotate_hold_ms != 0U)
		server->rotate_hold_ms = kwl_milliseconds();

	/*
	 * Everything is drawn again; the user's change of choice is given to
	 * displays.conf's writer, whose thread waits for the file's flush
	 * (heads.c, BUG-306: the flush in the event loop took seconds).
	 */
	server->dirty = 1;
	compose->rotate_saved = -1;
	if ((steps & KWL_ROTATE_STEP_SAVE) != 0U) {
		compose->rotate_saved = kwl_heads_config_save(server);
		if (compose->rotate_saved != 0)
			printf("KWL OUTPUT rotate save errno=%d\n", compose->rotate_saved);
	}
	clock.saved = kwl_milliseconds();

	/* Succeeded: the output shows the angle from now on, and the tests read it with how long each step took. */
	compose->opened = after;
	compose->opened_display = compose->display;
	kwl_output_steps_name(steps, names, sizeof(names));
	output_rotate_times(&clock);
	printf("KWL OUTPUT rotate key=%s rotation=%u selected=%u scanout=%u turn=%u width=%u height=%u mode=%ux%u steps=%s by=%s "
	    "pointer=%d,%d>%d,%d ms=%llu\n",
	    compose->display_name, after.rotation, after.selected, after.scanout, after.turn, after.width, after.height, compose->mode_width,
	    compose->mode_height, names, output_rotate_mode_name(mode), pointer[0], pointer[1], pointer[2], pointer[3],
	    (unsigned long long)(kwl_milliseconds() - clock.started));
	return 0;
}

/*
 * Changes which angles the displays may take (a test's policy, WS217: the
 * test image's capture request): the anchor's mask and reasons are worked
 * out again, and a choice the new mask does not allow is shown as 0, as an
 * opening does (displays.conf's line kept).
 */
void
kwl_output_policy_set(
	struct kwl_server *server,
	unsigned policy)
{
	struct kwl_compose *compose;
	const char *name;
	unsigned why;
	unsigned bit;
	int error;

	/* The policy, for the next opening too. */
	server->rotate_policy = policy;
	compose = server->compose;
	if (compose == NULL)
		return;

	/* The anchor's angles under it. */
	compose->rotations = kwl_rotate_mask(compose->hw180, compose->turn_default, KWL_TURN_BUILD, policy, &why);
	compose->rotate_why = why;
	name = "default";
	if (policy == KWL_ROTATE_POLICY_ANY)
		name = "any";
	printf("KWL OUTPUT rotate policy=%s mask=%u why=%u turn_default=%u\n", name, compose->rotations, why, compose->turn_default);

	/* A choice still allowed stays; Settings is told the new angles. */
	bit = kwl_rotate_bit(compose->rotation_selected);
	if ((compose->rotations & bit) != 0U) {
		kwl_displays_tell(server);
		return;
	}

	/* Otherwise 0, as an opening chooses for an angle the display cannot take (nothing saved). */
	compose->rotation_selected = KWL_ROTATE_0;
	if (!compose->output_open || compose->output_lost) {
		kwl_displays_tell(server);
		return;
	}
	error = kwl_output_apply_rotation(server, KWL_PLANE_ANCHOR, KWL_ROTATE_0, KWL_ROTATE_MODE_POLICY);
	if (error != 0)
		printf("KWL OUTPUT rotate policy errno=%d\n", error);

	/* Settings is told the new angles and size. */
	kwl_displays_tell(server);
}

/*
 * Gives what Settings is told of a display's angle (ws217-p004): for the
 * desktop's anchor (anchor 1), the angle the user chose, the angles it may
 * take and why not, the others but the chosen one held while another
 * display is shown beside it; for any other display (a head, or one shown
 * nowhere), 0 alone until a head's angle (ws217-p005), with that reason.
 */
void
kwl_output_rotation_report(
	struct kwl_server *server,
	unsigned anchor,
	unsigned *rotation,
	unsigned *mask,
	unsigned *why)
{
	struct kwl_compose *compose;
	const char *heads;
	unsigned alone;

	/* Another display than the anchor: unturned, its angle comes with the heads'. */
	compose = server->compose;
	if (compose == NULL || !anchor) {
		*rotation = KWL_ROTATE_0;
		*mask = KWL_ROTATION_0;
		*why = KWL_ROTATE_WHY_ALONE;
		return;
	}

	/* The anchor, alone or beside the heads shown. */
	heads = output_rotate_heads(compose);
	alone = 1U;
	if (heads != NULL)
		alone = 0U;

	/* Succeeded: the choice, and the angles it may take now. */
	*rotation = compose->rotation_selected;
	kwl_rotate_report(compose->rotation_selected, compose->rotations, compose->rotate_why, alone, mask, why);
}

/*
 * Begins showing the old frame after an output turned to another size
 * (BUG-306): the first frame at the new size would show each window's
 * image of the old size, cut and out of place, until its client draws the
 * new one.  Every window on the anchor's desktop shown that is told a new
 * size is waited for (a fullscreen or docked one, told by the fitting that
 * follows; a floating one of xdg-shell 4, by its new bounds), from the last
 * configure it was sent before the turn, at most KWL_ROTATE_HOLD_MS; one
 * that must change its size, until its image is of another size.  Called
 * after the new logical size is set and before the windows are fitted.
 */
void
kwl_output_rotate_hold(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *surface;
	int32_t bounds_width;
	int32_t bounds_height;
	uint32_t width;
	uint32_t height;
	unsigned windows;
	unsigned fixed;
	int waits;

	/* The space a floating window's body has at the new size. */
	kwl_window_bounds(server, &bounds_width, &bounds_height);

	/* Each window told a new size, from the last configure it heard before the turn. */
	windows = 0U;
	for (client = server->clients; client != NULL; client = client->next) {
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only a surface. */
			if (surface->kind != KWL_SURFACE)
				continue;

			/* A window not waited for now is not waited for since an earlier turn either. */
			surface->rotate_waits = 0U;
			waits = output_rotate_waits(server, surface);
			if (!waits)
				continue;

			/* The size of its image now, and whether it must draw another (a fullscreen or docked one is given one). */
			width = 0U;
			height = 0U;
			kwl_surface_size(surface, &width, &height);
			fixed = 0U;
			if (surface->fullscreen || surface->maximized)
				fixed = 1U;

			/*
			 * An image of the new size drawn after a newer configure was
			 * acknowledged ends its wait (kwl_output_rotate_committed).
			 */
			surface->rotate_waits = 1U;
			surface->rotate_serial = surface->configure_serial;
			surface->rotate_needed = (unsigned)kwl_rotate_size_needed(fixed, width, height, bounds_width, bounds_height);
			surface->rotate_width = width;
			surface->rotate_height = height;
			windows++;
		}
	}

	/* The wait begins now, when there is a window to wait for (the turn's end starts it again, kwl_output_apply_rotation). */
	server->rotate_hold_windows = windows;
	server->rotate_hold_ms = 0U;
	if (windows != 0U)
		server->rotate_hold_ms = kwl_milliseconds();
	printf("KWL OUTPUT rotate hold windows=%u bounds=%dx%d\n", windows, (int)bounds_width, (int)bounds_height);
}

/*
 * Ends a window's wait after an output's turn (BUG-306) when the image it
 * just committed was drawn after it acknowledged a configure sent after
 * the turn, and is of another size when it had to be (display.c's
 * adoption of a commit).
 */
void
kwl_output_rotate_committed(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	uint32_t width;
	uint32_t height;
	int drawn;

	/* Only a window waited for, with an image. */
	if (!surface->rotate_waits || surface->current == NULL)
		return;

	/*
	 * An image drawn before the client read the new size, or one of the
	 * old size committed after the acknowledgement (a frame drawn before
	 * it): the wait goes on.
	 */
	width = 0U;
	height = 0U;
	kwl_surface_size(surface, &width, &height);
	drawn = kwl_rotate_drawn(surface->acked_serial, surface->rotate_serial, surface->rotate_needed, surface->rotate_width, surface->rotate_height,
	    width, height);
	if (!drawn)
		return;

	/* Succeeded: the window has drawn the new size, and the log says when. */
	surface->rotate_waits = 0U;
	printf("KWL OUTPUT rotate drawn surface=%u client=%llu width=%u height=%u was=%ux%u after_ms=%llu\n", surface->id,
	    (unsigned long long)surface->client->number, width, height, surface->rotate_width, surface->rotate_height,
	    (unsigned long long)(kwl_milliseconds() - server->rotate_hold_ms));
}

/*
 * Tells whether the event loop keeps the old frame shown after an output's
 * turn to another size (BUG-306): while a window waited for has not drawn
 * the new size and the wait is not over, no frame is drawn, and every
 * frame callback is told at once so that the clients draw on (their
 * presentation waits for it).  At the wait's end everything is drawn, and
 * the log says how long the old frame stayed and how many windows were
 * late.  Returns 1 while it stays.
 */
int
kwl_output_rotate_held(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *surface;
	uint64_t now;
	unsigned waiting;
	int holding;

	/* No wait. */
	if (server->rotate_hold_ms == 0U)
		return 0;

	/* The windows still waited for, in every live client. */
	waiting = 0U;
	for (client = server->clients; client != NULL; client = client->next) {
		/* A failed client hears nothing. */
		if (client->fatal)
			continue;

		/* Each of its surfaces. */
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only a live surface. */
			if (surface->kind != KWL_SURFACE || surface->dead)
				continue;

			/* Its frame callbacks told now: no frame shows its image. */
			if (surface->committed_callbacks != NULL)
				kwl_callbacks_done(&surface->committed_callbacks);

			/* A window gone from the screen is not waited for. */
			if (surface->rotate_waits && surface->mapped)
				waiting++;
		}
	}

	/* The old frame stays while a window is waited for and the wait is not over. */
	now = kwl_milliseconds();
	holding = kwl_rotate_holding(waiting, server->rotate_hold_ms, now);
	if (holding) {
		if (output_rotate_hold_timer.fire == NULL)
			kwl_timer_init(&output_rotate_hold_timer, "rotate-hold", output_rotate_hold_fired);
		kwl_timer_set(server, &output_rotate_hold_timer, server->rotate_hold_ms + KWL_ROTATE_HOLD_MS);
		return 1;
	}

	/* The wait ends: no window is waited for any more. */
	for (client = server->clients; client != NULL; client = client->next) {
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* A late window is shown as it draws. */
			if (surface->kind == KWL_SURFACE)
				surface->rotate_waits = 0U;
		}
	}

	/* Succeeded: the new size is drawn from now on. */
	printf("KWL OUTPUT rotate shown held_ms=%llu windows=%u late=%u\n", (unsigned long long)(now - server->rotate_hold_ms), server->rotate_hold_windows,
	    waiting);
	server->rotate_hold_ms = 0U;
	server->dirty = 1;
	return 0;
}

/*
 * Begins the turn to another angle the compositor turns (or none): another
 * logical size makes a new turn image first (a failure changes nothing),
 * then the render and cursor threads are parked (the render thread's park
 * drains what it submitted) and the new angle and logical size set.  The images are
 * exchanged by output_rotate_turn_end, after the clients are told the new
 * size.  The clock gets when each step ended.  Returns 0 (the render
 * thread parked) or the errno of making the new images (nothing changed).
 */
static int
output_rotate_turn_begin(
	struct kwl_server *server,
	const struct kwl_rotate_state *after,
	struct output_rotate_clock *clock,
	struct output_rotate_work *work)
{
	struct kwl_compose *compose;
	VkResult result;

	/* Whether the logical size changes, and whether the turn image of the size can serve the new angle. */
	compose = server->compose;
	memset(work, 0, sizeof(*work));
	if (after->width != server->width || after->height != server->height)
		work->resized = 1U;
	if (!work->resized && after->turn != KWL_ROTATE_0 && compose->turn.image != VK_NULL_HANDLE)
		work->reuse = 1U;

	/*
	 * The cursor's plane presents nothing from now on: its thread submits
	 * on the queue the new images' layout moves are submitted on (review
	 * M2); it goes on at the next kwl_cursor_plane_update.
	 */
	kwl_cursor_plane_hold(server);

	/* A new turn image, when the new angle is turned by the compositor and none of the size serves. */
	if (after->turn != KWL_ROTATE_0 && !work->reuse) {
		result = kwl_turn_open(compose, after->width, after->height, after->turn, &work->fresh);
		if (result != VK_SUCCESS) {
			printf("KWL OUTPUT rotate refused key=%s rotation=%u reason=turn result=%d\n", compose->display_name, after->rotation, (int)result);
			return ENOMEM;
		}
	}
	clock->turn_made = kwl_milliseconds();
	clock->glass_made = clock->turn_made;

	/*
	 * The fingers and the pens end what they were doing while their places
	 * still mean the old points (ws217-p003): the angle changes now.
	 */
	kwl_input_rotated(server);
	clock->prepared = kwl_milliseconds();

	/*
	 * No frame may use the old images from now until they are exchanged
	 * (the cursor's plane is held already): the render thread parks after
	 * the frame in flight and the tasks given before (drained).
	 */
	kwl_compose_park(server);
	clock->parked = kwl_milliseconds();
	clock->idle = clock->parked;

	/* The new angle and logical size, which the windows are fitted to and the clients told. */
	kwl_output_size_set(server, compose->mode_width, compose->mode_height, after->rotation);
	compose->scanout_rotation = after->scanout;
	compose->turn_rotation = after->turn;
	compose->rotate_why &= ~KWL_ROTATE_WHY_RESOURCE;

	/* Succeeded: the render thread is parked until output_rotate_turn_end. */
	return 0;
}

/*
 * Ends the turn output_rotate_turn_begin began: with the render thread
 * still parked and drained, the old turn image goes and the size-bound
 * caches are made again, and the render thread goes on; the look's
 * wallpaper at a new size is asked for (made off the event loop, the old
 * one stretched until it comes).
 * The same size (90 and 270, 0 and a turned 180) keeps every image of the
 * size, the turn image too (its pixels are logical), and only the
 * swapchain's images are drawn whole (review M5).  The clock gets when
 * each step ended.
 */
static void
output_rotate_turn_end(
	struct kwl_server *server,
	struct output_rotate_work *work,
	struct output_rotate_clock *clock)
{
	struct kwl_compose *compose;
	unsigned scene_kept;
	int error;

	/* The same turn image at the new angle: its pixels stay, only the turn pass turns them otherwise. */
	compose = server->compose;
	if (work->reuse) {
		compose->turn.rotation = compose->turn_rotation;
		memset(compose->image_frames, 0, sizeof(compose->image_frames));
		kwl_compose_unpark(server);
		clock->exchanged = kwl_milliseconds();
		clock->swapped = clock->exchanged;
		return;
	}

	/* The turn image exchanged (or none); the frames are drawn into it, or into the swapchain's images again. */
	kwl_turn_close(compose, &compose->turn);
	compose->turn = work->fresh;
	compose->target_width = compose->output.width;
	compose->target_height = compose->output.height;
	if (compose->turn.image != VK_NULL_HANDLE) {
		compose->target_width = compose->turn.width;
		compose->target_height = compose->turn.height;
	}

	clock->exchanged = kwl_milliseconds();

	/* The caches of another size made again; every image is drawn whole next. */
	scene_kept = 0U;
	if (compose->scene.buffer != VK_NULL_HANDLE)
		scene_kept = 1U;
	output_rotate_caches(server, scene_kept, work->resized);

	/* The render thread goes on with the new images. */
	kwl_compose_unpark(server);
	clock->swapped = kwl_milliseconds();

	/* Succeeded: the look's wallpaper at the new size is made off the event loop. */
	if (work->resized) {
		error = kwl_glass_resize(server);
		if (error != 0)
			printf("KWL OUTPUT rotate glass errno=%d\n", error);
	}
	memset(work, 0, sizeof(*work));
}

/*
 * Sends every live client the events queued for it so far (the new size,
 * BUG-306), so that the windows draw it while the turn's images are
 * exchanged rather than after the turn's request is answered.
 */
static void
output_rotate_flush(
	struct kwl_server *server)
{
	struct kwl_client *client;

	/* Each live client's queued events. */
	for (client = server->clients; client != NULL; client = client->next) {
		/* A connection being retired is not written to. */
		if (client->fatal)
			continue;

		/*
		 * What the socket takes now; a connection that cannot take its
		 * events fails again at the event loop's own flush, which retires
		 * it.
		 */
		(void)kwl_flush(client);
	}
}

/*
 * Logs how long each step of a change of the angle shown took (BUG-306):
 * the new images made (the turn image, the wallpaper), the render thread
 * parked, the device idle, the windows fitted and the clients told (and
 * when, told_at_ms, to set beside a client's lines), the images exchanged
 * and the caches made again, and the choice given to displays.conf's
 * writer.
 */
static void
output_rotate_times(
	const struct output_rotate_clock *clock)
{
	/* Each step's length, from the one before it; prepare and swap with their parts. */
	printf("KWL OUTPUT rotate times prepare_ms=%llu turn_ms=%llu glass_ms=%llu park_ms=%llu idle_ms=%llu fit_ms=%llu tell_ms=%llu swap_ms=%llu "
	    "caches_ms=%llu save_ms=%llu told_at_ms=%llu\n",
	    (unsigned long long)(clock->prepared - clock->started), (unsigned long long)(clock->turn_made - clock->started),
	    (unsigned long long)(clock->glass_made - clock->turn_made), (unsigned long long)(clock->parked - clock->prepared),
	    (unsigned long long)(clock->idle - clock->parked), (unsigned long long)(clock->fitted - clock->idle),
	    (unsigned long long)(clock->told - clock->fitted), (unsigned long long)(clock->swapped - clock->told),
	    (unsigned long long)(clock->swapped - clock->exchanged), (unsigned long long)(clock->saved - clock->swapped),
	    (unsigned long long)clock->told);
}

/* Marks every step before the clients are told, from the first not taken, as ended when the one before it did (now). */
static void
output_rotate_clock_fill(
	struct output_rotate_clock *clock,
	uint64_t now)
{
	/* Each image step not taken yet ends now. */
	if (clock->turn_made == 0U)
		clock->turn_made = now;
	if (clock->glass_made == 0U)
		clock->glass_made = now;
	if (clock->prepared == 0U)
		clock->prepared = now;
	if (clock->parked == 0U)
		clock->parked = now;
	if (clock->idle == 0U)
		clock->idle = now;
}

/*
 * Makes the caches of the drawn-into size again after a turn to another
 * size (the render thread parked and drained): the scene's copy (when the output had one), the
 * layers' pictures and the backdrop (made at its next use, with its own
 * pass); a failure leaves each unused, as at the output's opening.  The
 * same size keeps them.  Every image is drawn whole next.
 */
static void
output_rotate_caches(
	struct kwl_server *server,
	unsigned scene_kept,
	unsigned resized)
{
	struct kwl_compose *compose;
	VkResult result;

	/* The scene's copy, the layers' pictures and the backdrop at the new size. */
	compose = server->compose;
	if (resized) {
		kwl_scene_close(&compose->scene, compose->device);
		if (scene_kept) {
			result = kwl_scene_open(&compose->scene, compose->physical, compose->device, compose->target_width, compose->target_height);
			if (result != VK_SUCCESS)
				printf("KWL SCENE unavailable result=%d\n", (int)result);
		}

		/* The layers' pictures, and the backdrop made at its next use. */
		kwl_layers_close(compose);
		kwl_layers_open(server);
		kwl_backdrop_destroy(compose);
		kwl_glass_maps_destroy(compose);
	}

	/* No image holds the new picture yet. */
	memset(compose->image_frames, 0, sizeof(compose->image_frames));
	compose->turn.frame = 0U;
	compose->scene.valid = 0U;
}

/*
 * Names why the anchor may not turn beside the other displays shown: a
 * head open in the extended mode ("heads") or in the mirror ("mirror");
 * NULL when the output is alone.
 */
static const char *
output_rotate_heads(
	const struct kwl_compose *compose)
{
	unsigned index;
	int open;

	/* Any head open. */
	open = 0;
	for (index = 0U; index < KWL_HEADS; index++) {
		if (compose->heads[index].open)
			open = 1;
	}

	/* None: the output is alone. */
	if (!open)
		return NULL;

	/* The extended mode's heads, or the mirror's. */
	if (compose->display_mode == KWL_DISPLAYS_EXTENDED)
		return "heads";

	/* Succeeded: a mirror shows the picture. */
	return "mirror";
}

/* Names who changed the angle, for the log. */
static const char *
output_rotate_mode_name(
	unsigned mode)
{
	/* Each mode by its word. */
	switch (mode) {
	case KWL_ROTATE_MODE_USER:
		return "user";
	case KWL_ROTATE_MODE_SESSION_PAUSE:
		return "pause";
	case KWL_ROTATE_MODE_SESSION_RESUME:
		return "resume";
	case KWL_ROTATE_MODE_POLICY:
		return "policy";
	default:
		break;
	}

	/* An opening's change. */
	return "open";
}

/*
 * Tells whether a window is waited for after its output turned to another
 * size: mapped on the anchor, on the desktop shown and not minimized, and
 * told its new size (fullscreen or docked, or floating with xdg-shell 4's
 * bounds).
 */
static int
output_rotate_waits(
	struct kwl_server *server,
	const struct kwl_object *surface)
{
	const struct kwl_object *top;
	int desktop;

	/* A live, mapped window's surface, not the desktop's icons. */
	if (surface->dead || !surface->mapped || surface->client->fatal)
		return 0;
	if (surface->role == NULL || surface->role->top == NULL)
		return 0;
	desktop = kwl_desktop_is(surface);
	if (desktop)
		return 0;

	/* Shown on the anchor now. */
	if (surface->output != KWL_PLANE_ANCHOR || surface->minimized)
		return 0;
	if (surface->desktop != server->desktop)
		return 0;

	/* A fullscreen or docked window is sent the new size. */
	if (surface->fullscreen || surface->maximized)
		return 1;

	/* A floating window hears new bounds only from xdg-shell 4 on (kwl_window_bounds_refresh). */
	top = surface->role->top;
	if (top->kind != KWL_TOPLEVEL || top->version < 4U || !surface->configured)
		return 0;

	/* Succeeded: the window is told the new bounds and draws them. */
	return 1;
}

/*
 * Ends the old frame's hold after a turn: the scheduler's pass the timer
 * woke shows the new size (kwl_output_rotate_held looks at the time and at
 * the windows still waited for).
 */
static void
output_rotate_hold_fired(
	void *context,
	struct kwl_timer *timer)
{
	(void)context;
	(void)timer;
}
