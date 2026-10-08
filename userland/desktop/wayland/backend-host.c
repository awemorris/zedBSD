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
 * state only and never call back into the backend (the seat tells its
 * service after they return, and the scan closes a device not kept).  Inputs are known by their device
 * path, which stays the same while their descriptors change.
 */

#include "userland/desktop/wayland/kwl.h"

#include <stdio.h>
#include <string.h>

static struct kwl_input_device *backend_input(struct kwl_server *server, const char *path);
static const char *power_source_text(unsigned source);

/*
 * The seat is paused: drawing stops and the output closes now (Vulkan's
 * duplicate of the primary node goes before the seat returns it).
 */
void
kwl_backend_session_paused(
	void *data)
{
	struct kwl_server *server;

	/* No frame and no new input device from now on. */
	server = data;
	server->os_paused = 1;

	/* The frame in flight finishes and the output closes. */
	if (server->compose != NULL) {
		kwl_compose_quiesce(server);
		kwl_compose_output_close(server);
	}

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
	printf("KWL SEAT resumed\n");
}

/*
 * One input is paused: it is not read until it resumes (its descriptor
 * stays the seat's).
 */
void
kwl_backend_input_paused(
	void *data,
	const char *path)
{
	struct kwl_input_device *input;

	/* A node the compositor did not keep has nothing to stop. */
	input = backend_input(data, path);
	if (input == NULL)
		return;
	input->fd = -1;
}

/*
 * One input resumes on a new descriptor; a partial report of the old one
 * does not enter it.
 */
void
kwl_backend_input_resumed(
	void *data,
	const char *path,
	int descriptor)
{
	struct kwl_input_device *input;

	/* A node the compositor did not keep has nothing to resume. */
	input = backend_input(data, path);
	if (input == NULL)
		return;
	input->fd = descriptor;
	input->frame_count = 0;
	input->discarding = 0;
}

/*
 * One input is gone: it is forgotten (its descriptor was the seat's and is
 * closed).
 */
void
kwl_backend_input_gone(
	void *data,
	const char *path)
{
	struct kwl_input_device *input;

	/* A node the compositor did not keep has nothing to forget. */
	input = backend_input(data, path);
	if (input == NULL)
		return;
	kwl_input_forget(data, input);
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
 * An input device came or went (ws132-p003): the devices are scanned again
 * in the event loop's next pass instead of at the next KWL_INPUT_SCAN_MS,
 * and then every KWL_INPUT_SETTLE_SCAN_MS for a while, because the new node
 * may not be the seat's user's yet (BUG-264).
 */
void
kwl_backend_input_changed(
	void *data)
{
	struct kwl_server *server;
	uint64_t now;

	/* The next pass scans (main.c compares the time of the last scan). */
	server = data;
	server->input_scan_time = 0;

	/* The quick scans that follow; a failed clock leaves the ordinary ones. */
	now = kwl_milliseconds();
	if (now != UINT64_MAX)
		server->input_settle_until = now + KWL_INPUT_SETTLE_MS;
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

	/* The bar's state, then the extension's. */
	server = data;
	kwl_power_read(server);
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
	unsigned actions;
	int session;
	int locked;
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
			error = kwl_output_use_external(server);
			if (error == 0) {
				server->lid.closed = 1U;
				server->lid.closed_ms = kwl_milliseconds();
				server->lid.lock_is_lid = 0U;
				server->output_lid_moved = 1U;
				printf("KWL LID external: the desktop moved to the external display\n");
				return;
			}

			/* Refused (the limit of outputs shown at once): the closing locks and sleeps as usual. */
			printf("KWL LID external refused errno=%d\n", error);
		}
	}

	/* Opened after the closing moved the desktop to an external display: it comes back to the panel (N8). */
	if (open != 0U && server->output_lid_moved) {
		server->output_lid_moved = 0U;
		error = kwl_output_use_internal(server);
		printf("KWL LID internal: the desktop comes back to the panel errno=%d\n", error);
	}

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

	/* The backlight back, when the lid put it out. */
	if (server->backlight_out) {
		error = kl_backend_backlight_set(server->backlight, server->backlight_saved);
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
 * Reads the power's state for the bar: unknown (no battery shown) when the
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
	if (error != 0) {
		state.source = KL_BACKEND_POWER_SOURCE_UNKNOWN;
		state.percent = -1;
		state.charging = 0U;
	}

	/* Kept for the bar, and written in the log. */
	server->power = state;
	printf("KWL POWER source=%s percent=%d charging=%u\n", power_source_text(state.source), state.percent,
	       state.charging);

	/* A low battery is warned of (notify-system.c, ws156-p003). */
	kwl_notify_battery(server);
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

	/* The backlight, opened the first time it is needed. */
	if (server->backlight == NULL) {
		error = kl_backend_backlight_open(&server->backlight);
		if (error != 0) {
			server->backlight = NULL;
			printf("KWL LID backlight none error=%d\n", error);
			return;
		}
	}

	/* Already out. */
	if (server->backlight_out)
		return;

	/* Its brightness kept (full when it cannot say, or was already dark), then off. */
	percent = 100U;
	error = kl_backend_backlight_get(server->backlight, &percent);
	if (error != 0 || percent == 0U)
		percent = 100U;
	error = kl_backend_backlight_set(server->backlight, 0U);
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
		if (server->inputs[index].live == 0)
			continue;
		same = strcmp(server->inputs[index].path, path);
		if (same == 0)
			return &server->inputs[index];
	}

	/* No input of that path. */
	return NULL;
}
