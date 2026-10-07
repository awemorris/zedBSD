/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sound volume in the glass look's system bar (ws100-p004,
 * plan/ws100/design.md section 3.4): an icon left of the network's that
 * shows the volume (a speaker with no to three waves, a cross when muted,
 * struck through and pale with no sound), and a popup under it with a
 * slider (0 to 100) and a mute switch.
 *
 * A click on the icon opens the popup; a press or drag on the slider sets
 * the volume, a press on the mute row switches it; a click elsewhere, or
 * Esc, closes it.  The wheel over the icon (or the open popup) moves the
 * volume by VOLUME_WHEEL_STEP a notch.  A wheel notch, the end of a drag
 * and switching mute off each play audiod's short feedback sound once; the
 * steps of a drag play none (BUG-170, 2026-10-04 user: once, when the slider
 * is let go).
 *
 * During a session audiod alone holds the volume: the system bar and
 * Settings' Sound page both set it there and follow what it reports, and
 * nothing is written to a file while the user changes it (BUG-161,
 * ws100-p012; 2026-10-03 user: no I/O for each change, keep it at the end
 * of the session).  The user's preferences (sound.volume, sound.muted) are
 * read once, when audiod is first reached, and applied; the volume is
 * written back once, when the session ends (Log Out, or the compositor told to
 * stop), and only when it differs from what the file holds.  A volume
 * changed after a power cut or a crash is lost, which the user accepted.
 *
 * All of it goes through libkeiland-backend (kl_backend_audio_*, ws131-p004):
 * The compositor never speaks audiod's protocol, and nothing here waits for it.
 * The kept volume is the compositor's settings store's (settings.c, WS135):
 * the store gives the volume the file held, takes audiod's volume as the
 * session's, and writes it at the session's end.  A client's set of the
 * volume (kl_system_settings_v1) comes here as kwl_volume_request.
 */

#include "glass.h"
#include "titlebar.h"
#include "settings-store.h"
#include <keiland/keiland.h>

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A parameter a function does not use. */
#define UNUSED_PARAMETER(name)	((void)(name))

/* The evdev code of Esc. */
#define VOLUME_KEY_ESC		1U

/* The popup's size, padding, rows and corner radius. */
#define VOLUME_POPUP_WIDTH	260
#define VOLUME_PADDING		14
#define VOLUME_TITLE_HEIGHT	34
#define VOLUME_SLIDER_HEIGHT	34
#define VOLUME_ROW_HEIGHT	34
#define VOLUME_NOTE_HEIGHT	26
#define VOLUME_RADIUS		12.0f

/* The slider's track and knob. */
#define VOLUME_TRACK_HEIGHT	6
#define VOLUME_KNOB		18

/* The wheel: percent a notch (kwl_seat_axis counts notches, as evdev's wheel does). */
#define VOLUME_WHEEL_STEP	5

/* How often a drag sends the volume, in milliseconds. */
#define VOLUME_SEND_MS		50U

/* The preferences' keys. */
#define VOLUME_KEY_VOLUME	"sound.volume"
#define VOLUME_KEY_MUTED	"sound.muted"

/*
 * The volume's side of the system bar: audiod's link in libkeiland (made
 * on the desktop's first tick), what it last reported, the volume shown
 * (while a drag or a wheel leads, ahead of audiod's report), whether the
 * popup is open, where and on which output's bar it opened, where the icon
 * was drawn on each output's bar (the system bar's and each head's,
 * ws113-p015) and its size, whether the slider is dragged, the time of the
 * last send, and whether a volume is waiting to be sent.
 *
 * restored is set once the preferences' volume has been applied (on the
 * first connection to audiod); an audiod reached again later gets the
 * session's volume instead.  kept, kept_value and kept_muted are what the
 * file holds as far as the compositor knows (read at the start, written at the
 * end), so that the end writes only a volume that changed.
 */
struct volume_view {
	struct kl_backend_audio *audio;
	unsigned opened;
	struct kl_backend_audio_state state;
	unsigned value;
	unsigned muted;
	unsigned open;
	int32_t popup_x;
	int32_t popup_y;
	int32_t popup_height;
	unsigned output;
	struct kwl_plane_places icons;
	int32_t icon_width;
	int32_t icon_height;
	unsigned icon_logged;
	unsigned dragging;
	uint64_t sent_ms;
	unsigned send_waiting;
	unsigned applied;
	unsigned restored;
	unsigned kept;
	unsigned kept_value;
	unsigned kept_muted;
};

/*
 * The one view of the volume.  Only the event loop's thread touches it (the
 * ticks, the drawing, the input).
 */
static struct volume_view volume_view;

static void volume_open_popup(struct kwl_server *server, unsigned slot);
static void volume_close_popup(struct kwl_server *server, const char *via);
static void volume_set(struct kwl_server *server, unsigned value, unsigned muted, const char *via, unsigned final);
static void volume_send(struct kwl_server *server);
static void volume_restore(struct kwl_server *server);
static int volume_sound(void);
static int volume_in_icon(unsigned slot, int32_t x, int32_t y);
static int volume_in_popup(int32_t x, int32_t y);
static unsigned volume_slider_value(int32_t x);
static int32_t volume_slider_top(void);
static int32_t volume_mute_top(void);
static void volume_draw_switch(struct kwl_server *server, VkCommandBuffer command, int32_t right, int32_t middle, unsigned on, float fade);

/*
 * Reads what audiod has reported since the last tick, and sends a volume a
 * drag held back.  The link is made on the desktop's first tick.
 */
void
kwl_volume_tick(
	struct kwl_server *server)
{
	unsigned changed;
	uint64_t now;

	/* The link, once (the backend connects to audiod when it can). */
	if (!volume_view.opened) {
		volume_view.opened = 1U;
		volume_view.audio = kl_backend_audio_open();
		volume_view.value = 100U;
	}

	/* No link could be made (no memory): the icon says there is no sound. */
	if (volume_view.audio == NULL)
		return;

	/* What arrived. */
	(void)kl_backend_audio_update(volume_view.audio, &changed);
	if (changed != 0U) {
		kl_backend_audio_get_state(volume_view.audio, &volume_view.state);
		server->dirty = 1;

		/* audiod reached or lost: the volume is given once per connection. */
		if ((changed & KL_BACKEND_AUDIO_CHANGED_REACHABLE) != 0U) {
			printf("KWL VOLUME reachable=%u device=%u\n", volume_view.state.reachable, volume_view.state.device);
			if (!volume_view.state.reachable)
				volume_view.applied = 0U;
		}

		/* A report shows the volume, unless a drag or a wheel leads. */
		if ((changed & KL_BACKEND_AUDIO_CHANGED_VOLUME) != 0U && !volume_view.dragging && !volume_view.send_waiting) {
			volume_view.value = volume_view.state.left;
			volume_view.muted = volume_view.state.muted;
		}

		/* A connection with the volume known takes the preferences' volume, or the session's when reached again. */
		if (volume_view.state.reachable && !volume_view.applied && (changed & KL_BACKEND_AUDIO_CHANGED_VOLUME) != 0U) {
			volume_view.applied = 1U;
			volume_restore(server);
		}
	}

	/* A volume a drag held back. */
	now = kwl_milliseconds();
	if (volume_view.send_waiting && now - volume_view.sent_ms >= VOLUME_SEND_MS)
		volume_send(server);
}

/*
 * Writes the session's volume to the preferences, once, when the session
 * ends (Log Out, or the compositor told to stop; BUG-161).  Nothing is written
 * when the file already holds it, or without preferences (the login
 * screen) or before the volume was known.
 */
void
kwl_volume_keep(
	struct kwl_server *server,
	const char *why)
{
	unsigned value;
	unsigned muted;
	char text[16];

	/* Without the settings (the login screen), or before audiod ever reported, there is nothing to keep. */
	if (server->settings == NULL || !volume_view.restored)
		return;

	/* audiod's volume when it is reached, else the last one shown. */
	value = volume_view.value;
	muted = volume_view.muted;
	if (volume_view.state.reachable) {
		value = volume_view.state.left;
		muted = volume_view.state.muted;
	}

	/* What the file holds already is not written again. */
	if (volume_view.kept && value == volume_view.kept_value && muted == volume_view.kept_muted) {
		printf("KWL VOLUME kept value=%u muted=%u why=%s write=0\n", value, muted, why);
		return;
	}

	/* Hands the volume to the store, which writes it as the session ends (settings.c). */
	(void)snprintf(text, sizeof(text), "%u", value);
	kwl_settings_store_report(server->settings, VOLUME_KEY_VOLUME, text);

	/* Hands the mute to the store the same way. */
	(void)snprintf(text, sizeof(text), "%u", muted);
	kwl_settings_store_report(server->settings, VOLUME_KEY_MUTED, text);

	/*
	 * Remembers what the store now holds, so that a later keep with the
	 * same volume writes nothing again.
	 */
	volume_view.kept = 1U;
	volume_view.kept_value = value;
	volume_view.kept_muted = muted;

	/* Logs the keep for the tests; handing it to the store cannot fail, so the error is always 0. */
	printf("KWL VOLUME kept value=%u muted=%u why=%s write=1 error=%d\n", value, muted, why, 0);
}

/*
 * Reports the volume for the settings (settings.c, WS135): whether the
 * session's volume is known (audiod was reached and given the kept
 * volume), whether there is sound, and audiod's volume and mute.
 */
void
kwl_volume_report(
	unsigned *restored,
	unsigned *available,
	unsigned *value,
	unsigned *muted)
{
	int sound;

	/* Known once audiod took the kept volume. */
	*restored = volume_view.restored;

	/* Sound: audiod reached, with a device. */
	sound = volume_sound();
	*available = 0U;
	if (sound)
		*available = 1U;

	/* audiod's volume when it is reached, else the last one shown. */
	*value = volume_view.value;
	*muted = volume_view.muted;
	if (volume_view.state.reachable) {
		*value = volume_view.state.left;
		*muted = volume_view.state.muted;
	}
}

/*
 * Sets the volume a client asked for (kl_system_settings_v1, WS135): shown
 * and sent to audiod, without the feedback sound (the client plays its
 * own).  Returns 0, EBUSY before audiod took the kept volume (the start's
 * volume must not be overtaken), or ENODEV without sound.
 */
int
kwl_volume_request(
	struct kwl_server *server,
	unsigned value,
	unsigned muted)
{
	/* No link, or audiod not reached. */
	if (volume_view.audio == NULL || !volume_view.state.reachable)
		return ENODEV;

	/* The kept volume goes to audiod first. */
	if (!volume_view.restored)
		return EBUSY;

	/* Shows the asked volume and sends it to audiod. */
	volume_view.value = value;
	volume_view.muted = muted;
	volume_send(server);
	server->dirty = 1;

	/* Logs the set for the tests. */
	printf("KWL VOLUME set value=%u muted=%u via=settings final=1 at_ms=%llu\n", value, muted, (unsigned long long)kwl_milliseconds());

	/* Succeeded: audiod has the volume (its report comes back to the settings). */
	return 0;
}

/*
 * Sets both channels' volume a client asked for (kl_system_audio_v1, WS131
 * p010): the left shown in the system bar, both sent to the sound service,
 * without the feedback sound.  Returns 0, EBUSY before the service took
 * the kept volume, ENODEV without sound, or the error of sending.
 */
int
kwl_volume_request_channels(
	struct kwl_server *server,
	unsigned left,
	unsigned right,
	unsigned muted)
{
	int error;

	/* No link, or the service not reached. */
	if (volume_view.audio == NULL || !volume_view.state.reachable)
		return ENODEV;

	/* The kept volume goes to the service first. */
	if (!volume_view.restored)
		return EBUSY;

	/* Shown (the bar shows the left channel) and sent now; a drag's send held back is overtaken. */
	volume_view.value = left;
	volume_view.muted = muted;
	volume_view.send_waiting = 0U;
	volume_view.sent_ms = kwl_milliseconds();
	server->dirty = 1;
	error = kl_backend_audio_set_volume(volume_view.audio, left, right, muted);
	printf("KWL VOLUME set left=%u right=%u muted=%u via=system error=%d\n", left, right, muted, error);
	if (error != 0)
		return error;

	/* Succeeded: the service has the volume (its report comes back as the state). */
	return 0;
}

/*
 * Plays the short feedback sound a client asked for (kl_system_audio_v1).
 * Returns 0, ENODEV without sound, or the error of sending.
 */
int
kwl_volume_feedback(
	void)
{
	int error;

	/* No link, or the service not reached. */
	if (volume_view.audio == NULL || !volume_view.state.reachable)
		return ENODEV;

	/* The sound, at the device volume. */
	error = kl_backend_audio_feedback(volume_view.audio);
	if (error != 0)
		return error;

	/* Succeeded: the service plays it. */
	return 0;
}

/*
 * Copies the sound service's state as last reported (all zero before the
 * link: not reached).
 */
void
kwl_volume_audio_state(
	struct kl_backend_audio_state *state)
{
	/* The state last read. */
	*state = volume_view.state;
}

/*
 * Draws the volume's icon at x (its left edge) in a bar whose top is at
 * top, in the bar's ink: the system bar's, or a head's on the output the
 * pass draws (ws113-p015).
 */
void
kwl_volume_draw_icon(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t x,
	int32_t top,
	const float *ink)
{
	float blue[4];
	float faint[4];
	unsigned icon;
	int sound;

	/* Where a click on this output's bar opens the popup (a little larger than the drawing). */
	kwl_plane_place(&volume_view.icons, server->view_output, x - 5, top);
	volume_view.icon_width = 30;
	volume_view.icon_height = KWL_GLASS_BAR - 6;
	if (!volume_view.icon_logged && server->view_output == KWL_PLANE_ANCHOR) {
		volume_view.icon_logged = 1U;
		printf("KWL VOLUME icon x=%d y=%d width=%d height=%d\n", x - 5, top + 3, volume_view.icon_width, volume_view.icon_height);
	}

	/* While the popup is open its icon on the bar it opened from has a pale back of the accent the user chose (the bar keeps its colours). */
	if (volume_view.open && volume_view.output == server->view_output) {
		kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 0.28f, blue);
		glass_draw_solid(server, command, (float)(x - 5), (float)(top + KWL_GLASS_BAR_MIDDLE - 14), (float)volume_view.icon_width, 28.0f, 7.0f, blue);
	}

	/* No sound: a pale speaker, struck through. */
	sound = volume_sound();
	if (!sound) {
		memcpy(faint, ink, sizeof(faint));
		faint[3] *= 0.35f;
		glass_draw_icon(server, command, GLASS_ICON_VOLUME_0, x, top + KWL_GLASS_BAR_MIDDLE - 10, 20U, faint);
		glass_draw_solid(server, command, (float)(x + 1), (float)(top + KWL_GLASS_BAR_MIDDLE - 1), 18.0f, 2.0f, 1.0f, ink);
		return;
	}

	/* Muted, or as many waves as the volume is loud. */
	icon = GLASS_ICON_VOLUME_MUTED;
	if (!volume_view.muted) {
		icon = GLASS_ICON_VOLUME_3;
		if (volume_view.value <= 66U)
			icon = GLASS_ICON_VOLUME_2;
		if (volume_view.value <= 33U)
			icon = GLASS_ICON_VOLUME_1;
		if (volume_view.value == 0U)
			icon = GLASS_ICON_VOLUME_0;
	}

	/* The icon. */
	glass_draw_icon(server, command, icon, x, top + KWL_GLASS_BAR_MIDDLE - 10, 20U, ink);
}

/*
 * Draws the open popup under the icon: its shadow, its glass, the title
 * with the volume, the slider and the mute switch, or the reason there is
 * no sound.  It is drawn by the pass of the output whose bar opened it.
 */
void
kwl_volume_draw_popup(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float soft[4] = { 0.34f, 0.38f, 0.46f, 1.0f };
	static const float track[4] = { 0.62f, 0.66f, 0.72f, 0.55f };
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	static const float edge[4] = { 0.12f, 0.16f, 0.24f, 0.25f };
	struct glass_shape shape;
	char text[32];
	float fade;
	float fill[4];
	unsigned kept;
	int32_t left;
	int32_t width;
	int32_t top;
	int32_t knob;
	int sound;

	/* Only an open popup, on its output. */
	if (!volume_view.open)
		return;
	if (volume_view.output != server->view_output)
		return;

	/* The shadow. */
	glass_shape_init(&shape, (float)volume_view.popup_x, (float)volume_view.popup_y + 6.0f, (float)VOLUME_POPUP_WIDTH, (float)volume_view.popup_height);
	shape.quad[0] -= 40.0f;
	shape.quad[1] -= 40.0f;
	shape.quad[2] += 80.0f;
	shape.quad[3] += 80.0f;
	shape.mode = MODE_SHADOW;
	shape.radius = VOLUME_RADIUS;
	shape.soft = 18.0f;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.24f;
	glass_shape_draw(server, command, &shape);

	/* The glass, as white as the network's menu. */
	glass_shape_init(&shape, (float)volume_view.popup_x, (float)volume_view.popup_y, (float)VOLUME_POPUP_WIDTH, (float)volume_view.popup_height);
	shape.mode = MODE_GLASS;
	shape.radius = VOLUME_RADIUS;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.86f;
	shape.edge = 0.85f;
	glass_shape_draw(server, command, &shape);

	/* The title, and the volume at the right (Muted, or the percent). */
	left = volume_view.popup_x + VOLUME_PADDING;
	width = VOLUME_POPUP_WIDTH - 2 * VOLUME_PADDING;
	top = volume_view.popup_y + VOLUME_PADDING / 2;
	glass_draw_text(server, command, SIZE_TITLE, left, top + 22, kl_tr("Sound"), width, dark);
	sound = volume_sound();
	if (sound) {
		(void)snprintf(text, sizeof(text), "%u%%", volume_view.value);
		if (volume_view.muted)
			(void)snprintf(text, sizeof(text), "%s", kl_tr("Muted"));
		glass_draw_text(server, command, SIZE_BAR, left + width - glass_text_width(server, SIZE_BAR, text), top + 22, text, width, soft);
	}

	/* The controls are pale and do nothing without sound. */
	fade = 1.0f;
	if (!sound)
		fade = 0.35f;

	/* The slider: the track, its filled part and the knob at the volume. */
	top = volume_slider_top();
	kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, fade, fill);
	glass_draw_solid(server, command, (float)left, (float)(top + (VOLUME_SLIDER_HEIGHT - VOLUME_TRACK_HEIGHT) / 2), (float)width, (float)VOLUME_TRACK_HEIGHT, 3.0f, track);
	knob = left + (int32_t)((unsigned)(width - VOLUME_KNOB) * volume_view.value / 100U);
	kept = kwl_accent_as_is(server);
	glass_draw_solid(server, command, (float)left, (float)(top + (VOLUME_SLIDER_HEIGHT - VOLUME_TRACK_HEIGHT) / 2), (float)(knob - left + VOLUME_KNOB / 2), (float)VOLUME_TRACK_HEIGHT, 3.0f, fill);
	kwl_accent_done(server, kept);
	glass_draw_solid(server, command, (float)knob - 1.0f, (float)(top + (VOLUME_SLIDER_HEIGHT - VOLUME_KNOB) / 2) - 1.0f, (float)VOLUME_KNOB + 2.0f, (float)VOLUME_KNOB + 2.0f, (float)VOLUME_KNOB / 2.0f + 1.0f, edge);
	glass_draw_solid(server, command, (float)knob, (float)(top + (VOLUME_SLIDER_HEIGHT - VOLUME_KNOB) / 2), (float)VOLUME_KNOB, (float)VOLUME_KNOB, (float)VOLUME_KNOB / 2.0f, white);

	/* The mute row: its name and switch. */
	top = volume_mute_top();
	glass_draw_text(server, command, SIZE_BAR, left, top + 22, kl_tr("Mute"), width, dark);
	volume_draw_switch(server, command, left + width, top + VOLUME_ROW_HEIGHT / 2, volume_view.muted, fade);

	/* Without sound, the reason under the controls. */
	if (!sound) {
		top += VOLUME_ROW_HEIGHT;
		if (!volume_view.state.reachable) {
			glass_draw_text(server, command, SIZE_BAR, left, top + 18, kl_tr("Sound service is not running"), width, soft);
		} else {
			glass_draw_text(server, command, SIZE_BAR, left, top + 18, kl_tr("No sound output"), width, soft);
		}
	}
}

/*
 * Handles a pointer button for the volume: a press on the icon opens or
 * closes the popup; while it is open, a press on the slider sets the volume
 * and follows a drag, a press on the mute row switches it, and a press
 * elsewhere closes the popup.  Returns 1 when the button was the volume's.
 */
int
kwl_volume_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	int32_t y;
	int32_t slider;
	int32_t mute;
	int inside;
	int sound;

	/* With the popup closed, only a left press on the icon. */
	if (!volume_view.open) {
		/* A release, or another button, goes on. */
		if (state == 0U || button != KWL_BUTTON_LEFT)
			return 0;

		/* A press off the icon on the bar the pointer is on goes on. */
		inside = volume_in_icon(server->pointer_output, server->pointer_x, server->pointer_y);
		if (!inside)
			return 0;

		/* The popup opens under that icon. */
		volume_open_popup(server, server->pointer_output);
		return 1;
	}

	/* A release ends a drag: the last volume goes, with its sound. */
	if (state == 0U) {
		if (volume_view.dragging) {
			volume_view.dragging = 0U;
			volume_set(server, volume_view.value, volume_view.muted, "slider", 1U);
		}

		/* Every release is the popup's. */
		return 1;
	}

	/* A press on the icon, on any output's bar, closes it. */
	inside = volume_in_icon(server->pointer_output, server->pointer_x, server->pointer_y);
	if (inside) {
		volume_close_popup(server, "icon");
		return 1;
	}

	/* A press outside the popup closes it and goes no further. */
	inside = volume_in_popup(server->pointer_x, server->pointer_y);
	if (!inside) {
		volume_close_popup(server, "outside");
		return 1;
	}

	/* Without sound, or with another button, the controls do nothing. */
	sound = volume_sound();
	if (!sound || button != KWL_BUTTON_LEFT)
		return 1;

	/* The rows' tops, and where the press is. */
	y = server->pointer_y;
	slider = volume_slider_top();
	mute = volume_mute_top();

	/* The slider: the volume under the pointer, then a drag. */
	if (y >= slider && y < slider + VOLUME_SLIDER_HEIGHT) {
		volume_view.dragging = 1U;
		volume_set(server, volume_slider_value(server->pointer_x), volume_view.muted, "slider", 0U);
		return 1;
	}

	/* The mute row switches mute. */
	if (y >= mute && y < mute + VOLUME_ROW_HEIGHT) {
		volume_set(server, volume_view.value, !volume_view.muted, "mute", 1U);
		return 1;
	}

	/* Succeeded: the press was the popup's. */
	return 1;
}

/*
 * Follows the pointer while the popup is open: a drag moves the volume.
 * Returns 1 when the motion was the volume's.
 */
int
kwl_volume_motion(
	struct kwl_server *server)
{
	/* A closed popup does not follow the pointer. */
	if (!volume_view.open)
		return 0;

	/* A drag sets the volume under the pointer. */
	if (volume_view.dragging)
		volume_set(server, volume_slider_value(server->pointer_x), volume_view.muted, "slider", 0U);

	/* Succeeded: the motion was the popup's. */
	return 1;
}

/*
 * Handles a key while the popup is open: Esc closes it, and the others are
 * the popup's too.  Returns 1 when the key was the volume's.
 */
int
kwl_volume_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state)
{
	/* A closed popup takes no key. */
	if (!volume_view.open)
		return 0;

	/* Esc, pressed, closes it. */
	if (key == VOLUME_KEY_ESC && state != 0U)
		volume_close_popup(server, "key");

	/* Succeeded: the key was the popup's. */
	return 1;
}

/*
 * Moves the volume with the wheel over the icon or the open popup,
 * VOLUME_WHEEL_STEP a notch (up louder).  Returns 1 when the wheel was the
 * volume's.
 */
int
kwl_volume_axis(
	struct kwl_server *server,
	int32_t vertical,
	int32_t horizontal)
{
	int32_t notches;
	int32_t value;
	int over;
	int sound;

	UNUSED_PARAMETER(horizontal);

	/* Only over the icon, or the open popup. */
	over = volume_in_icon(server->pointer_output, server->pointer_x, server->pointer_y);
	if (!over && volume_view.open)
		over = volume_in_popup(server->pointer_x, server->pointer_y);
	if (!over)
		return 0;

	/* Without sound the wheel does nothing, and goes no further. */
	sound = volume_sound();
	if (!sound || vertical == 0)
		return 1;

	/* The notches; down (positive, in Wayland's direction) is quieter. */
	notches = vertical;

	/* The new volume within 0..100. */
	value = (int32_t)volume_view.value - notches * VOLUME_WHEEL_STEP;
	if (value < 0)
		value = 0;
	if (value > 100)
		value = 100;
	volume_set(server, (unsigned)value, volume_view.muted, "wheel", 1U);

	/* Succeeded: the wheel was the volume's. */
	return 1;
}

/*
 * Tells whether the popup is open (the look is not still while it is).
 */
int
kwl_volume_is_open(
	void)
{
	/* Open or not. */
	return (int)volume_view.open;
}

/* Opens the popup under the icon of an output's bar, kept on that output. */
static void
volume_open_popup(
	struct kwl_server *server,
	unsigned slot)
{
	struct kwl_plane_rect output;
	int32_t icon_x;
	int32_t top;
	int placed;
	int sound;

	/* The output and the icon's place on its bar. */
	(void)kwl_output_rect(server, slot, &output);
	placed = kwl_plane_placed(&volume_view.icons, slot, &icon_x, &top);
	if (!placed)
		return;
	volume_view.output = slot;

	/* Its place: under the icon, its right edge not past the output's. */
	volume_view.popup_x = icon_x + volume_view.icon_width / 2 - VOLUME_POPUP_WIDTH / 2;
	if (volume_view.popup_x + VOLUME_POPUP_WIDTH > output.x + (int32_t)output.width - 8)
		volume_view.popup_x = output.x + (int32_t)output.width - 8 - VOLUME_POPUP_WIDTH;
	if (volume_view.popup_x < output.x + 8)
		volume_view.popup_x = output.x + 8;
	volume_view.popup_y = top + KWL_GLASS_BAR + 6;

	/* Its height: the title, the slider, the mute row, and a note without sound. */
	volume_view.popup_height = VOLUME_PADDING + VOLUME_TITLE_HEIGHT + VOLUME_SLIDER_HEIGHT + VOLUME_ROW_HEIGHT;
	sound = volume_sound();
	if (!sound)
		volume_view.popup_height += VOLUME_NOTE_HEIGHT;

	/* Open. */
	volume_view.open = 1U;
	volume_view.dragging = 0U;
	server->dirty = 1;
	printf("KWL VOLUME popup open x=%d y=%d width=%d height=%d slider=%d mute=%d sound=%d\n", volume_view.popup_x, volume_view.popup_y, VOLUME_POPUP_WIDTH, volume_view.popup_height, volume_slider_top(), volume_mute_top(), sound);
}

/* Closes the popup. */
static void
volume_close_popup(
	struct kwl_server *server,
	const char *via)
{
	/* A drag in progress ends with its volume. */
	if (volume_view.dragging) {
		volume_view.dragging = 0U;
		volume_set(server, volume_view.value, volume_view.muted, "slider", 1U);
	}

	/* Closed. */
	volume_view.open = 0U;
	server->dirty = 1;
	printf("KWL VOLUME popup close via=%s\n", via);
}

/*
 * Shows and sends a volume: a drag's steps are sent at most every
 * VOLUME_SEND_MS and play no sound; a final one (a wheel notch, the end of a
 * drag, mute) is sent now and plays the feedback sound once, except that
 * switching mute on plays nothing (BUG-170).
 */
static void
volume_set(
	struct kwl_server *server,
	unsigned value,
	unsigned muted,
	const char *via,
	unsigned final)
{
	uint64_t now;

	/* Nothing to do when nothing changes, except at a drag's end. */
	if (value == volume_view.value && muted == volume_view.muted && !final)
		return;

	/* Shown at once. */
	volume_view.value = value;
	volume_view.muted = muted;
	volume_view.send_waiting = 1U;
	server->dirty = 1;
	printf("KWL VOLUME set value=%u muted=%u via=%s final=%u at_ms=%llu\n", value, muted, via, final, (unsigned long long)kwl_milliseconds());

	/* Sent now when final, or when a drag's wait is over (audiod holds it; nothing is written). */
	now = kwl_milliseconds();
	if (final || now - volume_view.sent_ms >= VOLUME_SEND_MS)
		volume_send(server);

	/* A drag's steps and switching mute on play no sound. */
	if (!final || muted)
		return;

	/* The sound, once for the final volume, at that volume. */
	(void)kl_backend_audio_feedback(volume_view.audio);
	printf("KWL VOLUME feedback at_ms=%llu via=%s\n", (unsigned long long)now, via);
}

/* Sends the volume shown to audiod. */
static void
volume_send(
	struct kwl_server *server)
{
	int error;

	UNUSED_PARAMETER(server);

	/* The request; one that cannot go is logged and dropped (the next report shows audiod's). */
	volume_view.send_waiting = 0U;
	volume_view.sent_ms = kwl_milliseconds();
	error = kl_backend_audio_set_volume(volume_view.audio, volume_view.value, volume_view.value, volume_view.muted);
	if (error != 0)
		printf("KWL VOLUME send errno=%d\n", error);
}

/*
 * Gives a newly reached audiod its volume: the preferences' on the first
 * connection of the session (the volume kept at the end of the last one),
 * the session's on a later one (an audiod that came back).
 */
static void
volume_restore(
	struct kwl_server *server)
{
	int value;
	int muted;
	int error;

	/* audiod came back: it gets the volume the session had. */
	if (volume_view.restored) {
		if (volume_view.value != volume_view.state.left || volume_view.muted != volume_view.state.muted) {
			volume_send(server);
			printf("KWL VOLUME restored value=%u muted=%u from=session\n", volume_view.value, volume_view.muted);
		}
		return;
	}
	volume_view.restored = 1U;

	/* Without the settings (the login screen), audiod's volume stays. */
	if (server->settings == NULL)
		return;

	/* Takes the volume the file kept; without a kept volume, audiod's stays. */
	error = kwl_settings_kept(server, VOLUME_KEY_VOLUME, &value);
	if (error != 0)
		return;

	/* Takes the kept mute, which stays off when the file holds none. */
	muted = 0;
	(void)kwl_settings_kept(server, VOLUME_KEY_MUTED, &muted);

	/* What the file holds, so that the end writes only a change. */
	volume_view.kept = 1U;
	volume_view.kept_value = (unsigned)value;
	volume_view.kept_muted = (unsigned)muted;

	/* The same as audiod's: nothing to send. */
	if ((unsigned)value == volume_view.state.left && (unsigned)muted == volume_view.state.muted)
		return;

	/* Shown and sent, without a sound. */
	volume_view.value = (unsigned)value;
	volume_view.muted = (unsigned)muted;
	volume_send(server);
	server->dirty = 1;
	printf("KWL VOLUME preferences value=%d muted=%d\n", value, muted);
}

/* Tells whether there is sound: audiod reached, with a device. */
static int
volume_sound(
	void)
{
	/* audiod, and its device. */
	if (!volume_view.state.reachable)
		return 0;
	if (!volume_view.state.device)
		return 0;

	/* Succeeded: there is sound. */
	return 1;
}

/* Tells whether a point is on the icon's area in an output's bar. */
static int
volume_in_icon(
	unsigned slot,
	int32_t x,
	int32_t y)
{
	int32_t icon_x;
	int32_t top;
	int placed;

	/* Not drawn on that output's bar yet. */
	placed = kwl_plane_placed(&volume_view.icons, slot, &icon_x, &top);
	if (!placed)
		return 0;

	/* Within its rectangle, a little in from the bar's top and bottom. */
	if (x < icon_x || x >= icon_x + volume_view.icon_width)
		return 0;
	if (y < top + 3 || y >= top + 3 + volume_view.icon_height)
		return 0;

	/* Succeeded: on the icon. */
	return 1;
}

/* Tells whether a point is on the open popup. */
static int
volume_in_popup(
	int32_t x,
	int32_t y)
{
	/* Within its rectangle. */
	if (x < volume_view.popup_x || x >= volume_view.popup_x + VOLUME_POPUP_WIDTH)
		return 0;
	if (y < volume_view.popup_y || y >= volume_view.popup_y + volume_view.popup_height)
		return 0;

	/* Succeeded: on the popup. */
	return 1;
}

/* Gives the volume at a point of the slider, 0 at the track's left end and 100 at its right. */
static unsigned
volume_slider_value(
	int32_t x)
{
	int32_t left;
	int32_t width;
	int32_t offset;

	/* The knob's centre runs from the track's left end to its right end. */
	left = volume_view.popup_x + VOLUME_PADDING + VOLUME_KNOB / 2;
	width = VOLUME_POPUP_WIDTH - 2 * VOLUME_PADDING - VOLUME_KNOB;
	offset = x - left;
	if (offset < 0)
		offset = 0;
	if (offset > width)
		offset = width;

	/* Succeeded: the percent, rounded. */
	return (unsigned)((offset * 100 + width / 2) / width);
}

/* Gives the slider's row top. */
static int32_t
volume_slider_top(
	void)
{
	/* Under the title. */
	return volume_view.popup_y + VOLUME_PADDING / 2 + VOLUME_TITLE_HEIGHT;
}

/* Gives the mute row's top. */
static int32_t
volume_mute_top(
	void)
{
	/* Under the slider. */
	return volume_slider_top() + VOLUME_SLIDER_HEIGHT;
}

/* Draws a switch, its right edge at right, on or off, faded when it cannot be used. */
static void
volume_draw_switch(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t right,
	int32_t middle,
	unsigned on,
	float fade)
{
	float pill[4] = { 0.62f, 0.66f, 0.72f, 1.0f };
	float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	unsigned kept;
	int32_t x;

	/* The pill: the accent the user chose when on, drawn as it is with its knob (ws179-p001). */
	kept = server->keep_colours;
	if (on) {
		kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 1.0f, pill);
		kept = kwl_accent_as_is(server);
	}

	/* Faded when it cannot be used, and drawn. */
	pill[3] *= fade;
	white[3] *= fade;
	x = right - 36;
	glass_draw_solid(server, command, (float)x, (float)(middle - 10), 36.0f, 20.0f, 10.0f, pill);

	/* The knob, right when on. */
	if (on) {
		glass_draw_solid(server, command, (float)(x + 18), (float)(middle - 8), 16.0f, 16.0f, 8.0f, white);
	} else {
		glass_draw_solid(server, command, (float)(x + 2), (float)(middle - 8), 16.0f, 16.0f, 8.0f, white);
	}
	kwl_accent_done(server, kept);
}
