/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Follows native OSS mixer controls without PCM playback or a fictitious event fd.
 * Each subscription owns one mixer descriptor and refreshes through ordinary ticks.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <time.h>
#include <unistd.h>

#define AUDIO_RETRY_MS 1000U
#define AUDIO_MIXERS_MAX 32

/* One caller owns a native mixer, its selected control and the latest public snapshot. */
struct kl_backend_audio {
	int fd;
	int control;
	unsigned has_mute;
	unsigned initial;
	unsigned dirty;
	uint64_t retry;
	uint64_t sampled;
	struct kl_backend_audio_state state;
};

static uint64_t audio_milliseconds(void);
static int audio_connect(struct kl_backend_audio *audio);
static int audio_state(struct kl_backend_audio *audio, struct kl_backend_audio_state *state);
static void audio_drop(struct kl_backend_audio *audio);

/*
 * Allocates a native mixer subscription that can recover from device absence.
 */
struct kl_backend_audio *
kl_backend_audio_open(
	void)
{
	struct kl_backend_audio *audio;
	int error;

	/* Keeps allocation failure distinct from a temporarily unavailable mixer. */
	audio = calloc(1, sizeof(*audio));
	if (audio == NULL)
		return NULL;

	/* Establishes an empty owner before probing native device access. */
	audio->fd = -1;
	audio->initial = 1;
	error = audio_connect(audio);
	if (error != 0)
		audio_drop(audio);

	/* Succeeded: the caller owns a subscription even if hardware is absent. */
	return audio;
}

/*
 * Releases the native descriptor and its subscription allocation.
 */
void
kl_backend_audio_close(
	struct kl_backend_audio *audio)
{
	/* A missing subscription has no device ownership to retire. */
	if (audio == NULL)
		return;

	/* Drops device ownership before freeing its cached state. */
	audio_drop(audio);
	free(audio);

	/* Succeeded: no native mixer reference survives the subscription. */
	return;
}

/*
 * Reports that OSS mixer changes require periodic updates rather than event reads.
 */
int
kl_backend_audio_fd(
	const struct kl_backend_audio *audio)
{
	(void)audio;

	/* Succeeded: periodic updates provide the native mixer change source. */
	return -1;
}

/*
 * Refreshes native volume and topology while reconnecting absent devices at bounded intervals.
 */
int
kl_backend_audio_update(
	struct kl_backend_audio *audio,
	unsigned *changed)
{
	struct kl_backend_audio_state previous;
	struct kl_backend_audio_state state;
	uint64_t now;
	int error;
	int differs;

	/* Requires caller-owned output storage before changing subscription state. */
	if (audio == NULL || changed == NULL)
		return EINVAL;

	/* Saves the previous public snapshot for explicit change reporting. */
	*changed = 0;
	previous = audio->state;
	now = audio_milliseconds();

	/* Device permissions and hotplug can make an earlier failed subscription usable. */
	if (audio->fd < 0 && now >= audio->retry) {
		error = audio_connect(audio);
		if (error != 0)
			audio_drop(audio);
	}

	/* Reads external mixer changes periodically and local writes on the next tick. */
	if (audio->fd >= 0 &&
	    (audio->dirty != 0 ||
	     now >= audio->sampled)) {
		error = audio_state(audio, &state);
		if (error != 0) {
			audio_drop(audio);
		} else {
			/* Publishes only a complete native snapshot. */
			audio->state = state;
			audio->dirty = 0;
			audio->sampled = now + AUDIO_RETRY_MS;
		}
	}

	/* A caller can distinguish device arrival or loss from an ordinary volume update. */
	if (previous.reachable != audio->state.reachable || audio->initial != 0)
		*changed |= KL_BACKEND_AUDIO_CHANGED_REACHABLE;

	/* Reports any public state change, including control topology and channel count. */
	differs = memcmp(&previous, &audio->state, sizeof(previous));
	if (differs != 0 || audio->initial != 0)
		*changed |= KL_BACKEND_AUDIO_CHANGED_VOLUME;

	/* The first update has now published the initial subscription state. */
	audio->initial = 0;

	/* Succeeded: the caller can inspect a complete cached mixer snapshot. */
	return 0;
}

/*
 * Copies the latest mixer snapshot without borrowing native control storage.
 */
void
kl_backend_audio_get_state(
	const struct kl_backend_audio *audio,
	struct kl_backend_audio_state *state)
{
	/* A caller without output storage cannot receive a snapshot. */
	if (state == NULL)
		return;

	/* Represents a missing subscription as an absent service. */
	memset(state, 0, sizeof(*state));
	if (audio == NULL)
		return;

	/* Copies only subscription-owned public state. */
	*state = audio->state;

	/* Succeeded: the caller's snapshot is independent of the device lifetime. */
	return;
}

/*
 * Writes native volume and mute while preserving unrelated mixer controls.
 */
int
kl_backend_audio_set_volume(
	struct kl_backend_audio *audio,
	unsigned left,
	unsigned right,
	unsigned muted)
{
	int volume;
	int mute_mask;
	int error;

	/* Rejects invalid public values before any native device write. */
	if (audio == NULL ||
	    left > 100 ||
	    right > 100 ||
	    muted > 1)
		return EINVAL;

	/* Requires a connected control that supplied a valid public snapshot. */
	if (audio->fd < 0 || audio->state.device == 0)
		return ENOTCONN;

	/* A driver without a native mute control cannot claim to apply mute. */
	if (muted != 0 && audio->has_mute == 0)
		return ENOTSUP;

	/* Packs the native OSS left/right percentage fields without touching another control. */
	volume = (int)left | ((int)right << 8);
	if (audio->state.channels == 1)
		volume = (int)left | ((int)left << 8);

	/* Applies volume through the native control's ioctl convention. */
	error = ioctl(audio->fd, MIXER_WRITE(audio->control), &volume);
	if (error != 0)
		return errno;

	/* The next tick must observe even a partially successful volume/mute update. */
	audio->dirty = 1;

	/* Read-modify-write retains every mute bit owned by another mixer control. */
	if (audio->has_mute != 0) {
		error = ioctl(audio->fd, SOUND_MIXER_READ_MUTE, &mute_mask);
		if (error != 0)
			return errno;

		/* Applies only the selected control's mute bit. */
		if (muted != 0) {
			mute_mask |= 1U << audio->control;
		} else {
			mute_mask &= ~(1U << audio->control);
		}

		/* Reports native mute failures even after volume was successfully written. */
		error = ioctl(audio->fd, SOUND_MIXER_WRITE_MUTE, &mute_mask);
		if (error != 0)
			return errno;
	}

	/* Succeeded: update reads back the driver's quantized channel values and mute state. */
	return 0;
}

/*
 * Accepts silent feedback for a connected mixer without adding PCM playback.
 */
int
kl_backend_audio_feedback(
	struct kl_backend_audio *audio)
{
	/* A missing caller-owned subscription cannot submit feedback. */
	if (audio == NULL)
		return EINVAL;

	/* Hardware absence remains observable even for silent feedback. */
	if (audio->fd < 0)
		return ENOTCONN;

	/* Succeeded: this mixer-only backend deliberately requests no PCM playback. */
	return 0;
}

/*
 * Reports whether native mixer permissions expose a usable volume control.
 */
int
kl_backend_audio_available(
	void)
{
	struct kl_backend_audio audio;
	int error;

	/* Probes the real default or enumerated mixer without allocating a lasting subscription. */
	memset(&audio, 0, sizeof(audio));
	audio.fd = -1;
	error = audio_connect(&audio);
	if (error != 0)
		return 0;

	/* Releases the probe's own native descriptor before reporting availability. */
	audio_drop(&audio);

	/* Succeeded: a native volume control was reachable with this caller's permissions. */
	return 1;
}

/* Reads monotonic milliseconds for retries independently of wall clock changes. */
static uint64_t
audio_milliseconds(
	void)
{
	struct timespec now;
	int error;

	/* Refuses to use uninitialized timestamp fields after a failed native clock read. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0;

	/* Succeeded: reports elapsed milliseconds for native polling and reconnects. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Prefers the native default mixer alias, then scans bounded device units. */
static int
audio_connect(
	struct kl_backend_audio *audio)
{
	struct kl_backend_audio_state state;
	char path[64];
	uint64_t now;
	int index;
	int count;
	int error;

	/* Prevents absent hardware from causing unbounded device-open loops on each UI tick. */
	now = audio_milliseconds();
	audio->retry = now + AUDIO_RETRY_MS;

	/* The alias follows hw.snd.default_unit; later units are fallback candidates. */
	for (index = -1; index < AUDIO_MIXERS_MAX; index++) {
		/* Selects the system default before numbered native mixer paths. */
		if (index < 0) {
			count = snprintf(path, sizeof(path), "/dev/mixer");
		} else {
			count = snprintf(path, sizeof(path), "/dev/mixer%d", index);
		}

		/* A malformed device path cannot be probed as an unrelated endpoint. */
		if (count < 0 || (size_t)count >= sizeof(path))
			continue;

		/* Keeps the selected descriptor private to this caller across application execs. */
		audio->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (audio->fd < 0)
			continue;

		/* Requires native control metadata and initial volume before claiming availability. */
		error = audio_state(audio, &state);
		if (error != 0) {
			(void)close(audio->fd);
			audio->fd = -1;
			continue;
		}

		/* Publishes this device only after its complete initial native readback. */
		audio->state = state;
		audio->sampled = 0;
		audio->dirty = 1;

		/* Succeeded: this caller owns a native mixer and its validated volume control. */
		return 0;
	}

	/* No native mixer with a usable output control was accessible to this caller. */
	return ENODEV;
}

/* Reads native control topology, channel percentages and the real mute mask. */
static int
audio_state(
	struct kl_backend_audio *audio,
	struct kl_backend_audio_state *state)
{
	int device_mask;
	int stereo_mask;
	int mute_mask;
	int volume;
	int error;

	/* Requires current native metadata rather than retaining a removed control's identity. */
	error = ioctl(audio->fd, SOUND_MIXER_READ_DEVMASK, &device_mask);
	if (error != 0)
		return errno;

	/* Prefers the master output volume and falls back to the native PCM volume control. */
	if ((device_mask & SOUND_MASK_VOLUME) != 0) {
		audio->control = SOUND_MIXER_VOLUME;
	} else if ((device_mask & SOUND_MASK_PCM) != 0) {
		audio->control = SOUND_MIXER_PCM;
	} else {
		/* This mixer has no output control represented by the existing public API. */
		return ENODEV;
	}

	/* Determines whether the selected native control exposes independent channel values. */
	error = ioctl(audio->fd, SOUND_MIXER_READ_STEREODEVS, &stereo_mask);
	if (error != 0)
		return errno;

	/* Reads the actual driver's quantized volume before constructing a public snapshot. */
	error = ioctl(audio->fd, MIXER_READ(audio->control), &volume);
	if (error != 0)
		return errno;

	/* Rejects a driver value outside the public channel range rather than silently inventing one. */
	if ((volume & 0xff) > 100 || ((volume >> 8) & 0xff) > 100)
		return EPROTO;

	/* The mute ioctl is optional on OSS drivers, unlike the required output volume control. */
	audio->has_mute = 0;
	mute_mask = 0;
	error = ioctl(audio->fd, SOUND_MIXER_READ_MUTE, &mute_mask);
	if (error != 0) {
		/* Only a driver-declared unsupported control can omit mute capability. */
		if (errno != ENOTTY && errno != EINVAL)
			return errno;
	} else {
		audio->has_mute = 1;
	}

	/* Publishes known volume metadata without claiming an unknown hardware sample rate. */
	memset(state, 0, sizeof(*state));
	state->reachable = 1;
	state->device = 1;
	state->channels = 1;
	state->left = (unsigned)(volume & 0xff);
	state->right = state->left;

	/* A stereo native control supplies its own independently quantized right channel. */
	if ((stereo_mask & (1U << audio->control)) != 0) {
		state->channels = 2;
		state->right = (unsigned)((volume >> 8) & 0xff);
	}

	/* A native mute bit describes the selected output independently of its volume. */
	if ((mute_mask & (1U << audio->control)) != 0)
		state->muted = 1;

	/* Succeeded: every reported field came from validated native mixer controls. */
	return 0;
}

/* Retires the current device while preserving the next permitted reconnect deadline. */
static void
audio_drop(
	struct kl_backend_audio *audio)
{
	/* A connected subscription owns exactly one native descriptor. */
	if (audio->fd >= 0)
		(void)close(audio->fd);

	/* Invalidates device metadata before another caller observes the cached snapshot. */
	audio->fd = -1;
	audio->has_mute = 0;
	audio->dirty = 0;
	memset(&audio->state, 0, sizeof(audio->state));

	/* Succeeded: this subscription retains no device or public hardware state. */
	return;
}
