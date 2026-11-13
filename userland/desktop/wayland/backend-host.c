/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The seat's callbacks of libkeiland-backend (ws131-p006): what the
 * compositor does when the seat takes the display and the input devices
 * away (another session has the display) and gives them back; and the
 * system's events (ws132-p003): input devices that came or went, the
 * power's changes, the buttons and the lid.
 *
 * The backend calls these from kl_backend_poll_done, and the input
 * devices' two from kl_backend_input_scan; they change the compositor's
 * state only, wait for nothing and never call back into the backend (the
 * scan closes a device not kept).  What the seat waits for -- the input
 * thread letting go of a descriptor, the output closed -- is answered from
 * the event loop (kwl_backend_seat_tick), and the seat tells its service
 * then.  Inputs are known by their device path, which stays the same
 * while their descriptors change.
 */

#include "userland/desktop/wayland/kwl.h"
#include "userland/desktop/wayland/compose.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct kwl_input_device *backend_input(struct kwl_server *server, const char *path);
static void seat_pause_close(struct kwl_server *server);
static void seat_pause_closed(struct kwl_server *server, int error, void *context);
static const char *power_source_text(unsigned source);
static void lid_external_moved(struct kwl_server *server, int error, void *context);
static void lid_internal_moved(struct kwl_server *server, int error, void *context);
static void lid_follow_rest(struct kwl_server *server, unsigned open);

/*
 * The seat is paused: drawing stops, and the output's close and every
 * input's detach are asked without waiting; kwl_backend_seat_tick tells
 * the seat once both are done (Vulkan's duplicate of the primary node goes
 * before the seat returns it).
 */
void
kwl_backend_session_paused(
	void *data)
{
	struct kwl_server *server;
	uint64_t sequence;

	/* No frame and no new input device from now on. */
	server = data;
	server->os_paused = 1;

	/* The input thread lets go of every device before the seat takes them (ws213-p004); a pause still running waits for these too. */
	sequence = kwl_input_detach_all(server);
	if (sequence > server->seat_pause_sequence)
		server->seat_pause_sequence = sequence;
	server->seat_pause = 1U;

	/* The output closes from the next tick, unless its close runs already. */
	if (server->seat_pause_output != KWL_SEAT_OUTPUT_CLOSING)
		server->seat_pause_output = KWL_SEAT_OUTPUT_ASK;

	/* The next activation makes a new output rather than reusing this one. */
	server->windowed = 0;
	printf("KWL SEAT paused\n");
}

/*
 * The seat is active again: the next frame opens the output, and the next
 * scan finds the input devices.
 */
void
kwl_backend_session_resumed(
	void *data)
{
	struct kwl_server *server;

	/* The ordinary scheduler opens the output and scans the inputs. */
	server = data;
	server->os_paused = 0;
	server->input_scan_time = 0;
	server->windowed = 0;
	server->dirty = 1;

	/* A pause whose close has not begun closes nothing now: the output is the scheduler's again. */
	if (server->seat_pause && server->seat_pause_output == KWL_SEAT_OUTPUT_ASK)
		server->seat_pause_output = KWL_SEAT_OUTPUT_CLOSED;

	/* The devices the pause let go of whose descriptors the seat left open are read again (ws213-p004). */
	kwl_input_attach_all(server);
	printf("KWL SEAT resumed\n");
}

/*
 * One input is paused: it is not read until it resumes (its descriptor
 * stays the seat's).  Returns 1 when the seat waits for the input thread
 * to let go (kwl_input_answer answers).
 */
int
kwl_backend_input_paused(
	void *data,
	const char *path)
{
	struct kwl_input_device *input;
	int later;

	/* A node the compositor did not keep has nothing to stop. */
	input = backend_input(data, path);
	if (input == NULL)
		return 0;

	/* The input thread lets go of it (ws213-p004). */
	later = kwl_input_pause(data, input);
	return later;
}

/*
 * One input resumes on a new descriptor; a partial report of the old one
 * does not enter it.  Returns 1 when the seat waits for the input thread
 * to let go of the old one.
 */
int
kwl_backend_input_resumed(
	void *data,
	const char *path,
	int descriptor)
{
	struct kwl_input_device *input;
	int later;

	/* A node the compositor did not keep has nothing to resume. */
	input = backend_input(data, path);
	if (input == NULL)
		return 0;

	/* The input thread reads the new descriptor from a fresh report (ws213-p004). */
	later = kwl_input_resume(data, input, descriptor);
	return later;
}

/*
 * One input is gone: it is forgotten (its descriptor is the seat's to
 * close).  Returns 1 when the seat waits for the input thread to let go.
 */
int
kwl_backend_input_gone(
	void *data,
	const char *path)
{
	struct kwl_input_device *input;
	int later;

	/* A node the compositor did not keep has nothing to forget. */
	input = backend_input(data, path);
	if (input == NULL)
		return 0;

	/* The input thread lets go, then the record goes. */
	later = kwl_input_forget(data, input);
	return later;
}

/*
 * Answers the seat once a pass, after the input thread's events: the
 * descriptors the input thread has let go of, then the seat's pause once
 * the output is closed (its close begun when no other change of the
 * display runs) and every input let go.
 */
void
kwl_backend_seat_tick(
	struct kwl_server *server)
{
	int detached;

	/* The descriptors first, so the seat has them all before the pause's end. */
	kwl_input_answer(server);
	if (!server->seat_pause)
		return;

	/* The output's close, begun once no change of the display runs. */
	if (server->seat_pause_output == KWL_SEAT_OUTPUT_ASK)
		seat_pause_close(server);

	/* Both done: the seat goes on. */
	detached = kwl_input_detached_through(server, server->seat_pause_sequence);
	if (!detached || server->seat_pause_output != KWL_SEAT_OUTPUT_CLOSED)
		return;
	server->seat_pause = 0U;
	server->seat_pause_sequence = 0U;
	kl_backend_seat_paused_done(server->backend);
	printf("KWL SEAT paused_done\n");
}

/*
 * Tells whether the compositor already reads the device at path (the
 * backend's scan leaves it alone).
 */
int
kwl_backend_input_known(
	void *data,
	const char *path)
{
	struct kwl_input_device *input;

	/* A live input of that path. */
	input = backend_input(data, path);
	return input != NULL;
}

/*
 * Classifies a device the backend's scan opened: 1 when the seat keeps it
 * (and owns its descriptor), 0 when the backend is to close it.
 */
int
kwl_backend_input_found(
	void *data,
	int descriptor,
	const char *path,
	const struct kl_backend_input_caps *caps)
{
	int kept;

	/* The seat's classification (input.c). */
	kept = kwl_input_probe(data, descriptor, path, caps);
	return kept;
}

/*
 * An input device came or went, or records of the system's events were
 * lost (ws132-p003): the devices are scanned again in the event loop's
 * next pass, once.  A node the scan opens through sessiond's seat socket
 * needs no owner of the seat's user's; one it opens itself is root's and
 * wheel's from its start (no session manager changes it).
 */
void
kwl_backend_input_changed(
	void *data)
{
	struct kwl_server *server;

	/* The next pass scans (input_scan_time 0 asks for it, main.c). */
	server = data;
	server->input_scan_time = 0;
	printf("KWL EVENT input changed\n");
}

/*
 * The AC adapter or a battery changed (ws132-p003): the power is read
 * again for the bar, and the system extension reads it again for its
 * clients.
 */
void
kwl_backend_power_changed(
	void *data)
{
	struct kwl_server *server;

	/* The bar's state (read on the device I/O thread), then the extension's. */
	server = data;
	kwl_power_read_later(server);
	kwl_system_power_changed(server);
	kwl_schedule(server);
}

/*
 * A power or sleep button was pressed (ws132-p003).  The sleep button
 * sleeps the machine (ws052-p012, N5).  The power button opens the power
 * dialog (Power Off, Restart, Log Out, Cancel; WS182, the 2026-10-07 user
 * decision) over the desktop; it counts as input on every screen, and is
 * passed over around a sleep, on the login and lock screens, and while the
 * dialog shows.
 */
void
kwl_backend_power_button(
	void *data,
	unsigned button)
{
	struct kwl_server *server;
	uint64_t now;
	int ignored;

	/* The sleep button: the sleep's rules decide (sleep.c). */
	server = data;
	if (button == KL_BACKEND_BUTTON_SLEEP) {
		printf("KWL EVENT sleep button\n");
		kwl_sleep_button(server);
		return;
	}

	/* The power button's press, logged with its time. */
	now = kwl_milliseconds();
	printf("KWL EVENT power button ms=%llu\n", (unsigned long long)now);

	/* A press around a sleep is not the user's (the release of the press that woke the machine comes then). */
	ignored = kwl_sleep_button_ignored(&server->sleep, now);
	if (ignored) {
		printf("KWL POWER button skip reason=sleep\n");
		return;
	}

	/* The press is the user's input: a screen out for the time without input lights, and that time counts again (sleep.c). */
	server->lock_input_ms = now;

	/* The login screen shows its own Restart and Shut Down. */
	if (server->greeter) {
		printf("KWL POWER button skip reason=greeter\n");
		return;
	}

	/* The lock screen offers no power choices to one who has not unlocked it (ws035-p102). */
	if (server->locked) {
		printf("KWL POWER button skip reason=locked\n");
		return;
	}

	/* The dialog shown already stays as it is (one that is closing opens again). */
	if (server->power_dialog.open && !server->power_dialog.closing) {
		printf("KWL POWER button skip reason=showing\n");
		return;
	}

	/* The power dialog over the desktop. */
	kwl_power_dialog_open(server, "button");
}

/*
 * The lid opened or closed (ws132-p003; what it does is ws132-p008, the
 * decision D2, and ws052-p012): closing it puts the screen out and locks
 * the session (the sleep follows at the next tick, sleep.c); opening it
 * within 15 minutes unlocks the lock the closing made (lid.c).
 */
void
kwl_backend_lid_changed(
	void *data,
	unsigned open)
{
	struct kwl_server *server;

	/* The change, logged with its time (BUG-255: an opening the firmware reports soon after a closing). */
	server = data;
	if (open != 0U) {
		printf("KWL EVENT lid open ms=%llu\n", (unsigned long long)kwl_milliseconds());
	} else {
		printf("KWL EVENT lid closed ms=%llu\n", (unsigned long long)kwl_milliseconds());
	}

	/* What it does. */
	kwl_lid_follow(server, open);
}

/*
 * Carries out a change of the lid, from its event or from its level read
 * after a sleep: the lock, the screen out, the unlock, the light.
 */
void
kwl_lid_follow(
	struct kwl_server *server,
	unsigned open)
{
	int matters;
	int deferred;
	int available;
	int error;

	/*
	 * Opened while a sleep's request waits: the sleep is cancelled if it
	 * can still be, and the opening is carried out after its answer, so
	 * that no unlock comes before the machine sleeps (sleep.c).
	 */
	if (open != 0U) {
		deferred = kwl_sleep_lid_opened(server);
		if (deferred)
			return;
	}

	/* An external display shown goes on with the lid closed (N8, R5): the closing changes nothing. */
	if (open == 0U) {
		matters = kwl_output_lid_matters(server);
		if (!matters) {
			printf("KWL LID ignored: the output shown is an external display\n");
			return;
		}
	}

	/*
	 * Closed with an external display connected: the desktop moves to it and
	 * goes on, unlocked and lit (N8, R4).  The lid counts as closed, so that
	 * the machine sleeps if the external display goes and the output comes
	 * back to the panel.
	 */
	if (open == 0U) {
		available = kwl_output_external_available(server);
		if (available) {
			/* The move runs while the passes go on (a change running ends first); lid_external_moved goes on. */
			error = kwl_output_use_external(server, lid_external_moved, NULL);
			if (error == EBUSY) {
				kwl_display_settle(server);
				error = kwl_output_use_external(server, lid_external_moved, NULL);
			}

			/* Leaves the rest to the move when it began. */
			if (error == 0)
				return;

			/* A move that could not begin ends at once with its error. */
			lid_external_moved(server, error, NULL);
			return;
		}
	}

	/* Opened after the closing moved the desktop to an external display: it comes back to the panel (N8), then the rest. */
	if (open != 0U && server->output_lid_moved) {
		server->output_lid_moved = 0U;
		error = kwl_output_use_internal(server, lid_internal_moved, NULL);
		if (error == EBUSY) {
			kwl_display_settle(server);
			error = kwl_output_use_internal(server, lid_internal_moved, NULL);
		}

		/* Leaves the rest to the move when it began. */
		if (error == 0)
			return;

		/* A move that could not begin ends at once with its error. */
		lid_internal_moved(server, error, NULL);
		return;
	}

	/* What it asks for. */
	lid_follow_rest(server, open);
}

/*
 * The closing's move to an external display ended: moved, the desktop
 * goes on, unlocked and lit (N8, R4), the lid counting as closed so that
 * the machine sleeps if the external display goes and the output comes
 * back to the panel; refused (the limit of outputs shown at once), the
 * closing locks and sleeps as usual.
 */
static void
lid_external_moved(
	struct kwl_server *server,
	int error,
	void *context)
{
	/* Moved. */
	(void)context;
	if (error == 0) {
		server->lid.closed = 1U;
		server->lid.closed_ms = kwl_milliseconds();
		server->lid.lock_is_lid = 0U;
		server->output_lid_moved = 1U;
		printf("KWL LID external: the desktop moved to the external display\n");
		return;
	}

	/* Refused: as usual. */
	printf("KWL LID external refused errno=%d\n", error);
	lid_follow_rest(server, 0U);
}

/* The opening's move back to the panel ended: the opening goes on. */
static void
lid_internal_moved(
	struct kwl_server *server,
	int error,
	void *context)
{
	/* The log, then the rest of the opening. */
	(void)context;
	printf("KWL LID internal: the desktop comes back to the panel errno=%d\n", error);
	lid_follow_rest(server, 1U);
}

/* Carries out what the lid asks for, after its moves of the output: the lock, the screen out, the unlock, the light. */
static void
lid_follow_rest(
	struct kwl_server *server,
	unsigned open)
{
	unsigned actions;
	int session;
	int locked;

	/* What it asks for: a session is any but the login screen's. */
	session = 1;
	if (server->greeter)
		session = 0;
	locked = 0;
	if (server->locked)
		locked = 1;
	if (open != 0U) {
		actions = kwl_lid_open(&server->lid, kwl_milliseconds(), locked);
	} else {
		actions = kwl_lid_close(&server->lid, kwl_milliseconds(), session, locked);
	}

	/* The lock first, so that the screen never lights on the desktop. */
	if ((actions & KWL_LID_LOCK) != 0U) {
		locked = kwl_lock(server, "lid");
		if (!locked)
			kwl_lid_lock_failed(&server->lid);
	}

	/* The screen out. */
	if ((actions & KWL_LID_SCREEN_OFF) != 0U)
		kwl_screen_off(server, "lid");

	/* The lid's own lock goes without the password. */
	if ((actions & KWL_LID_UNLOCK) != 0U)
		kwl_lock_release(server, "lid");

	/* The screen lit again. */
	if ((actions & KWL_LID_SCREEN_ON) != 0U)
		kwl_lid_screen_restore(server);
}

/*
 * Lights the screen again: the panel's backlight at the brightness it had
 * when the lid put it out, and the desktop drawn instead of black.  Also
 * called as the compositor ends, so a closed lid leaves no dark panel to
 * the next session.
 */
void
kwl_lid_screen_restore(
	struct kwl_server *server)
{
	int error;

	/* The backlight back, when the lid put it out (by the display thread, or here at the end). */
	if (server->backlight_out) {
		error = kwl_backlight_set(server, server->backlight_saved);
		printf("KWL LID backlight on percent=%u error=%d\n", server->backlight_saved, error);
		server->backlight_out = 0;
	}

	/* The desktop drawn again (the time without input no longer holds it out). */
	server->screen_idle_off = 0U;
	if (server->screen_off) {
		server->screen_off = 0;
		server->dirty = 1;
		printf("KWL LID screen on\n");
	}
}

/*
 * A read of the power's state for the bar on the device I/O thread: the
 * backend (open while the thread runs), and what it read.
 */
struct power_read_job {
	struct kwl_lane_job lane;
	struct kl_backend *backend;
	int error;
	struct kl_backend_power_state state;
};

static void power_apply(struct kwl_server *server, struct kl_backend_power_state *state, int error);
static void power_read_run(struct kwl_lane_job *job);
static void power_read_done(void *context, struct kwl_lane_job *job);
static void power_read_drop(struct kwl_lane_job *job);

/* The operations of a read of the power on the device I/O thread. */
static const struct kwl_lane_ops power_read_ops = {
	power_read_run,
	power_read_done,
	power_read_drop
};

/*
 * Reads the power's state for the bar here (the start, and a wake whose
 * rules need the lid as it is now): unknown (no battery shown) when the
 * backend did not open or cannot say.
 */
void
kwl_power_read(
	struct kwl_server *server)
{
	struct kl_backend_power_state state;
	int error;

	/* Unknown unless the backend says. */
	memset(&state, 0, sizeof(state));
	state.source = KL_BACKEND_POWER_SOURCE_UNKNOWN;
	state.percent = -1;
	error = kl_backend_power_get_state(server->backend, &state);

	/* Succeeded: the bar's from now. */
	power_apply(server, &state, error);
}

/*
 * Reads the power's state for the bar on the device I/O thread (here
 * without it), after the power changed; a change during a read reads
 * again once it is done.
 */
void
kwl_power_read_later(
	struct kwl_server *server)
{
	struct power_read_job *job;
	int error;

	/* One read at a time: the change is read when it is done. */
	if (server->power_reading) {
		server->power_again = 1U;
		return;
	}

	/* The job's record; without it the power is read here. */
	job = calloc(1U, sizeof(*job));
	if (job == NULL) {
		kwl_power_read(server);
		return;
	}

	/* Hands the job the backend the power is read from. */
	job->backend = server->backend;

	/* The thread reads; without it, it is read here. */
	server->power_reading = 1U;
	error = kwl_io_device_submit(server, &job->lane, &power_read_ops, 0U);
	if (error != 0) {
		power_read_run(&job->lane);
		power_read_done(server, &job->lane);
	}
}

/* Makes a state read the bar's: unknown after a failure, logged, and a low battery warned of. */
static void
power_apply(
	struct kwl_server *server,
	struct kl_backend_power_state *state,
	int error)
{
	const char *source_text;

	/* A state that could not be read is unknown, with no action. */
	if (error != 0) {
		state->source = KL_BACKEND_POWER_SOURCE_UNKNOWN;
		state->percent = -1;
		state->charging = 0U;
		state->actions = 0U;
	}

	/* Kept for the bar, and written in the log. */
	server->power = *state;
	source_text = power_source_text(state->source);
	printf(
	    "KWL POWER source=%s percent=%d charging=%u\n",
	    source_text,
	    state->percent,
	    state->charging);

	/* A low battery is warned of (notify-system.c, ws156-p003). */
	kwl_notify_battery(server);
}

/* Reads the power's state on the device I/O thread. */
static void
power_read_run(
	struct kwl_lane_job *lane_job)
{
	struct power_read_job *job;
	int error;

	/* The backend's state, unknown unless it says. */
	job = (struct power_read_job *)lane_job;
	memset(&job->state, 0, sizeof(job->state));
	job->state.source = KL_BACKEND_POWER_SOURCE_UNKNOWN;
	job->state.percent = -1;
	error = kl_backend_power_get_state(job->backend, &job->state);
	job->error = error;
}

/* Takes a read of the power, on the event loop's thread: the bar's state, and again for a change meanwhile. */
static void
power_read_done(
	void *context,
	struct kwl_lane_job *lane_job)
{
	struct power_read_job *job;
	struct kwl_server *server;

	/* The bar's state. */
	server = context;
	job = (struct power_read_job *)lane_job;
	server->power_reading = 0U;
	power_apply(server, &job->state, job->error);
	free(job);

	/* A change during the read is read now. */
	if (server->power_again) {
		server->power_again = 0U;
		kwl_power_read_later(server);
	}

	/* Succeeded: the bar draws it at the next frame. */
	kwl_schedule(server);
}

/* Frees a read that did not run (the thread stopped at the end); nothing waits for it. */
static void
power_read_drop(
	struct kwl_lane_job *lane_job)
{
	/* The record. */
	free(lane_job);
}

/*
 * Puts the screen out (the lid closed, or half the time without input
 * before a sleep, why): the panel's backlight off when the machine has one
 * the compositor may set (its brightness kept for the light), and black
 * drawn in any case (WS113 p013's backlight, or none).
 */
void
kwl_screen_off(
	struct kwl_server *server,
	const char *why)
{
	unsigned percent;
	int error;

	/* Black from the next frame. */
	if (!server->screen_off) {
		server->screen_off = 1;
		server->dirty = 1;
		printf("KWL LID screen off why=%s\n", why);
	}

	/* The backlight, opened by the display thread; a machine without one has nothing to put out. */
	percent = 100U;
	error = kwl_backlight_get(server, &percent);
	if (error != 0 && error != EAGAIN) {
		printf("KWL LID backlight none error=%d\n", error);
		return;
	}

	/* Already out. */
	if (server->backlight_out)
		return;

	/* Its brightness kept (full when it cannot say yet, or was already dark), then off by the display thread. */
	if (error != 0 || percent == 0U)
		percent = 100U;
	error = kwl_backlight_set(server, 0U);
	if (error != 0) {
		printf("KWL LID backlight off error=%d\n", error);
		return;
	}

	/* Succeeded: out, to come back at the opening. */
	server->backlight_saved = percent;
	server->backlight_out = 1;
	printf("KWL LID backlight off saved=%u\n", percent);
}

/* Names a power source for the log. */
static const char *
power_source_text(
	unsigned source)
{
	/* The two the kernel tells, and the rest. */
	if (source == KL_BACKEND_POWER_SOURCE_AC)
		return "ac";
	if (source == KL_BACKEND_POWER_SOURCE_BATTERY)
		return "battery";

	/* Succeeded: not known. */
	return "unknown";
}

/* Finds the input the compositor keeps for a device path, or NULL. */
static struct kwl_input_device *
backend_input(
	struct kwl_server *server,
	const char *path)
{
	unsigned index;
	int same;

	/* The path stays while the descriptors change. */
	for (index = 0; index < KWL_INPUT_MAX; index++) {
		if (server->inputs[index].live == 0 || server->inputs[index].closing)
			continue;
		same = strcmp(server->inputs[index].path, path);
		if (same == 0)
			return &server->inputs[index];
	}

	/* No input of that path. */
	return NULL;
}

/* Begins the seat's pause's close of the output once no other change of the display runs. */
static void
seat_pause_close(
	struct kwl_server *server)
{
	int busy;
	int error;

	/* A change running ends first; the next tick looks again. */
	busy = kwl_display_busy(server);
	if (busy)
		return;

	/* The close as a change of its own (at once when nothing is open). */
	server->seat_pause_output = KWL_SEAT_OUTPUT_CLOSING;
	error = kwl_display_output_close(server, seat_pause_closed, NULL);
	if (error == 0)
		return;

	/* Without memory for the change, the close runs here, waiting. */
	kwl_compose_output_close(server);
	server->seat_pause_output = KWL_SEAT_OUTPUT_CLOSED;
}

/* The seat's pause's close of the output has ended (a failure leaves nothing of the output either). */
static void
seat_pause_closed(
	struct kwl_server *server,
	int error,
	void *context)
{
	/* The pause may end at the next tick. */
	(void)context;
	if (error != 0)
		printf("KWL SEAT paused close errno=%d\n", error);
	server->seat_pause_output = KWL_SEAT_OUTPUT_CLOSED;
}
