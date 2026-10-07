/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sound's volume as the Sound page shows and sets it (ws100-p005): the
 * same volume the system bar's popup sets (userland/desktop/wayland/
 * volume.c), through the same ways.
 *
 *   - audiod holds the volume during the session.  The compositor is asked for it
 *     and follows audiod's reports for every client (libkeiland's
 *     kl_system_audio_*, WS131 p011, which also plays the feedback sound);
 *     the page shows what the compositor tells.  Nothing is written to a file
 *     while the user changes it (BUG-161): the compositor keeps the volume at the
 *     session's end and gives it to audiod at the next login.  On a desktop
 *     without Keiland's system extension the page says the sound is not
 *     available.
 *   - The short feedback sound follows the system bar's rules: once when a
 *     change is final (a drag let go, mute turned off), none for the steps
 *     of a drag, and never when mute is turned on (BUG-170, 2026-10-04
 *     user: once, when the slider is let go).
 */

#include "settings.h"

#include <stdio.h>

/* How long a drag holds back its volumes, in milliseconds (the system bar's). */
#define SOUND_SEND_MS		50U

/* How often the page follows audiod while it shows, in milliseconds. */
#define SOUND_POLL_MS		250

static void sound_set(struct se_app *app, int value, int muted, int final);
static void sound_send(struct se_app *app);
static void sound_feedback(struct se_app *app);

/*
 * Starts following the sound, when the desktop offers it: what the compositor
 * told of audiod when the system opened.
 */
void
se_sound_open(
	struct se_app *app)
{
	struct se_sound *sound;
	unsigned capabilities;

	/* All of the volume until it is known. */
	sound = &app->sound;
	sound->value = 100;
	sound->muted = 0;

	/* Without the desktop's sound the page says so. */
	capabilities = 0U;
	if (app->system != NULL)
		capabilities = kl_system_capabilities(app->system);
	if ((capabilities & KL_SYSTEM_HAS_AUDIO) == 0U) {
		se_log("SOUND open live=0");
		return;
	}

	/* The sound is followed from now on. */
	sound->live = 1;

	/* What the compositor told; the volume shown is audiod's once it is reached. */
	kl_system_audio_get_state(app->system, &sound->state);
	if (sound->state.reachable) {
		sound->value = (int)sound->state.left;
		sound->muted = (int)sound->state.muted;
	}

	/* The log lines the tests read: the open, and the first report as a change is told (WS131 p011, T2-022). */
	se_log("SOUND open live=1 reachable=%u value=%d muted=%d", sound->state.reachable, sound->value, sound->muted);
	se_log("SOUND report reachable=%u device=%u value=%u muted=%u", sound->state.reachable, sound->state.device, sound->state.left, sound->state.muted);
}

/*
 * Follows what the compositor told of audiod, and sends a volume a drag held
 * back.
 */
void
se_sound_poll(
	struct se_app *app,
	uint64_t now)
{
	struct se_sound *sound;

	/* Nothing to follow. */
	sound = &app->sound;
	if (!sound->live)
		return;

	/* A new report. */
	if ((app->system_changed & KL_SYSTEM_CHANGED_AUDIO) != 0U) {
		kl_system_audio_get_state(app->system, &sound->state);
		app->dirty = 1;
		se_log("SOUND report reachable=%u device=%u value=%u muted=%u", sound->state.reachable, sound->state.device, sound->state.left, sound->state.muted);

		/* A report shows the volume (set here or in the system bar), unless a drag or a held volume leads. */
		if (sound->state.reachable && !sound->dragging && !sound->send_waiting) {
			sound->value = (int)sound->state.left;
			sound->muted = (int)sound->state.muted;
		}
	}

	/* A volume held back. */
	if (sound->send_waiting && now - sound->sent_at >= SOUND_SEND_MS)
		sound_send(app);
}

/*
 * Reports how long the main loop may sleep for the sound (-1: as long as it
 * likes): soon while something is held back, a little while the page shows.
 */
int
se_sound_wait(
	const struct se_app *app)
{
	/* A volume held back. */
	if (app->sound.send_waiting)
		return (int)SOUND_SEND_MS;

	/* The page shows: it follows the system bar's changes. */
	if (app->page == SE_PAGE_SOUND && app->sound.live)
		return SOUND_POLL_MS;

	/* Nothing due. */
	return -1;
}

/*
 * Stops following the sound (a volume held back is sent first).
 */
void
se_sound_close(
	struct se_app *app)
{
	/* The last volume. */
	if (!app->sound.live)
		return;
	if (app->sound.send_waiting)
		sound_send(app);
	app->sound.live = 0;
}

/*
 * Tells whether the volume can be set: audiod runs, with a device.
 */
int
se_sound_available(
	const struct se_app *app)
{
	/* audiod, and its device. */
	if (!app->sound.live || !app->sound.state.reachable)
		return 0;
	if (!app->sound.state.device)
		return 0;

	/* Succeeded: there is sound. */
	return 1;
}

/*
 * Tells whether the sound service runs (the compositor reaches audiod), with a
 * device or without.
 */
int
se_sound_running(
	const struct se_app *app)
{
	/* The desktop's sound, and audiod reached. */
	if (!app->sound.live || !app->sound.state.reachable)
		return 0;

	/* Succeeded: the service runs. */
	return 1;
}

/*
 * Carries out a click on the Sound page: the mute switch.
 */
void
se_sound_press(
	struct se_app *app,
	int index)
{
	int available;

	/* Only the switch, and only while there is sound. */
	available = se_sound_available(app);
	if (index != SE_SOUND_MUTE || !available)
		return;

	/* Mute turns (the sound only when it is turned off). */
	sound_set(app, app->sound.value, !app->sound.muted, 1);
}

/*
 * Follows a drag on the volume's slider: the volume moves with the
 * pointer, is sent now and then while held, and is final when let go.
 */
void
se_sound_drag(
	struct se_app *app,
	int index,
	int x,
	unsigned phase)
{
	float fraction;
	int available;
	int value;

	/* Only the slider, and only while there is sound. */
	available = se_sound_available(app);
	if (index != SE_SOUND_VOLUME || !available)
		return;

	/* The volume under the pointer. */
	fraction = se_slider_fraction(&app->sound.slider, x);
	value = (int)(fraction * 100.0f + 0.5f);
	if (value < 0)
		value = 0;
	if (value > 100)
		value = 100;

	/* Held: it moves; let go: final. */
	app->sound.dragging = 1;
	if (phase == SE_DRAG_END) {
		app->sound.dragging = 0;
		sound_set(app, value, app->sound.muted, 1);
		return;
	}

	/* Still held: shown, and sent now and then. */
	sound_set(app, value, app->sound.muted, 0);
}

/* Sets the volume shown: sent (now when final, else at most every SOUND_SEND_MS), and a sound once when final (not for mute). */
static void
sound_set(
	struct se_app *app,
	int value,
	int muted,
	int final)
{
	struct se_sound *sound;

	/* Nothing changes, except at a drag's end. */
	sound = &app->sound;
	if (value == sound->value && muted == sound->muted && !final)
		return;

	/* Shown at once. */
	sound->value = value;
	sound->muted = muted;
	sound->send_waiting = 1;
	app->dirty = 1;
	se_log("SOUND set value=%d muted=%d final=%d", value, muted, final);

	/* Sent now when final, or when a drag's wait is over. */
	if (final || app->now - sound->sent_at >= SOUND_SEND_MS)
		sound_send(app);

	/* A drag's steps and turning mute on play no sound. */
	if (!final || muted)
		return;

	/* The sound, once for the final volume. */
	sound_feedback(app);
}

/* Sends the volume shown to the compositor, which asks audiod for it. */
static void
sound_send(
	struct se_app *app)
{
	struct se_sound *sound;
	int error;

	/* Records that nothing waits to be sent, and when this was sent. */
	sound = &app->sound;
	sound->send_waiting = 0;
	sound->sent_at = app->now;

	/* The request (both channels at the volume shown); its answer is logged by system.c. */
	error = kl_system_audio_set_volume(app->system, (unsigned)sound->value, (unsigned)sound->value, (unsigned)sound->muted, NULL);

	/* A request that cannot go is logged (the next report shows audiod's). */
	if (error != 0)
		se_log("SOUND send errno=%d", error);
}

/* Asks audiod for the feedback sound at the volume now. */
static void
sound_feedback(
	struct se_app *app)
{
	int error;

	/* Asks the compositor to have audiod play it. */
	error = kl_system_audio_feedback(app->system, NULL);
	se_log("SOUND feedback error=%d", error);
}
