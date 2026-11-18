/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tracks ALSA mixer controls directly, without alsa-lib or PCM playback.
 * One subscription owns a nonblocking control fd and reconnects after loss.
 */
#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include <errno.h>
#include <fcntl.h>
#include <sound/asound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define AUDIO_RETRY_MS 1000U
#define AUDIO_ELEMENTS_MAX 1024U

/* One caller owns this control fd, element descriptions and cached state. */
struct kl_backend_audio {
	int fd;
	struct snd_ctl_elem_info volume;
	struct snd_ctl_elem_info toggle;
	unsigned has_toggle;
	unsigned initial;
	unsigned dirty;
	uint64_t retry;
	uint64_t sampled;
	struct kl_backend_audio_state state;
};

static uint64_t audio_milliseconds(void);
static int audio_connect(struct kl_backend_audio *audio);
static int audio_elements(struct kl_backend_audio *audio);
static int audio_element(struct kl_backend_audio *audio, const struct snd_ctl_elem_id *ids, size_t count, const char *name, snd_ctl_elem_type_t type, struct snd_ctl_elem_info *info);
static int audio_state(struct kl_backend_audio *audio, struct kl_backend_audio_state *state);
static void audio_drop(struct kl_backend_audio *audio);
static unsigned audio_percentage(long sample, long minimum, long maximum);

/*
 * Allocates a mixer subscription even when no accessible device exists.
 */
struct kl_backend_audio *
kl_backend_audio_open(
	void)
{
	struct kl_backend_audio *audio;

	/* Device absence is recoverable, while allocation failure is not. */
	audio = calloc(1, sizeof(*audio));
	if (audio == NULL)
		return NULL;
	audio->fd = -1;
	audio->initial = 1;
	(void)audio_connect(audio);

	/* Succeeded: updates can reconnect this caller-owned subscription. */
	return audio;
}

/*
 * Closes the control fd before releasing the subscription.
 */
void
kl_backend_audio_close(
	struct kl_backend_audio *audio)
{
	/* A missing subscription owns no control device. */
	if (audio == NULL)
		return;
	if (audio->fd >= 0)
		(void)close(audio->fd);
	free(audio);

	/* Succeeded: no device ownership survives the allocation. */
	return;
}

/*
 * Returns the pollable mixer event fd or -1 while disconnected.
 */
int
kl_backend_audio_fd(
	const struct kl_backend_audio *audio)
{
	/* A missing subscription cannot supply an event source. */
	if (audio == NULL)
		return -1;

	/* Succeeded: the descriptor belongs to the subscription, not the caller. */
	return audio->fd;
}

/*
 * Drains mixer events and reads the latest volume without waiting.
 */
int
kl_backend_audio_update(
	struct kl_backend_audio *audio,
	unsigned *changed)
{
	struct snd_ctl_event event;
	struct kl_backend_audio_state previous;
	struct kl_backend_audio_state state;
	uint64_t now;
	ssize_t received;
	unsigned index;
	int error;
	int differs;

	/* Every update reports changes through an explicit output mask. */
	if (audio == NULL || changed == NULL)
		return EINVAL;
	*changed = 0;
	previous = audio->state;
	now = audio_milliseconds();

	/* Device access can appear after login or hardware arrival. */
	if (audio->fd < 0 && now >= audio->retry)
		(void)audio_connect(audio);

	/* A busy mixer cannot monopolize the application's main loop. */
	if (audio->fd >= 0) {
		for (index = 0; index < 64; index++) {
			received = read(audio->fd, &event, sizeof(event));
			if (received < 0) {
				if (errno == EAGAIN ||
				    errno == EWOULDBLOCK ||
				    errno == EINTR)
					break;
				audio_drop(audio);
				break;
			}

			/* A control disconnect or partial event invalidates this subscription. */
			if (received != sizeof(event)) {
				audio_drop(audio);
				break;
			}

			/* Value and topology events both require rereading element metadata. */
			audio->dirty = 1;
			if (event.type == SNDRV_CTL_EVENT_ELEM && event.data.elem.mask != SNDRV_CTL_EVENT_MASK_VALUE) {
				error = audio_elements(audio);
				if (error != 0) {
					audio_drop(audio);
					break;
				}
			}
		}
	}

	/* Periodic reading also handles drivers that coalesce or omit value events. */
	if (audio->fd >= 0 &&
	    (audio->dirty != 0 ||
	     now >= audio->sampled)) {
		error = audio_state(audio, &state);
		if (error != 0) {
			audio_drop(audio);
		} else {
			audio->state = state;
			audio->dirty = 0;
			audio->sampled = now + AUDIO_RETRY_MS;
		}
	}

	/* Reachability changes are separate from volume and device changes. */
	if (previous.reachable != audio->state.reachable || audio->initial != 0)
		*changed |= KL_BACKEND_AUDIO_CHANGED_REACHABLE;
	differs = memcmp(&previous, &audio->state, sizeof(previous));
	if (differs != 0 || audio->initial != 0)
		*changed |= KL_BACKEND_AUDIO_CHANGED_VOLUME;
	audio->initial = 0;

	/* Succeeded: the state cache is ready for the caller's next snapshot. */
	return 0;
}

/*
 * Copies the latest mixer snapshot without touching the device.
 */
void
kl_backend_audio_get_state(
	const struct kl_backend_audio *audio,
	struct kl_backend_audio_state *state)
{
	/* A null subscription is represented by an absent audio service. */
	if (state == NULL)
		return;
	memset(state, 0, sizeof(*state));
	if (audio == NULL)
		return;
	*state = audio->state;

	/* Succeeded: no borrowed control storage escapes the subscription. */
	return;
}

/*
 * Writes channel volume and mute through the selected mixer controls.
 */
int
kl_backend_audio_set_volume(
	struct kl_backend_audio *audio,
	unsigned left,
	unsigned right,
	unsigned muted)
{
	struct snd_ctl_elem_value control;
	long range;
	unsigned index;
	int error;

	/* The public channel range is always validated before device access. */
	if (audio == NULL ||
	    left > 100 ||
	    right > 100 ||
	    muted > 1)
		return EINVAL;
	if (audio->fd < 0 || audio->state.device == 0)
		return ENOTCONN;
	if (muted != 0 && audio->has_toggle == 0)
		return ENOTSUP;

	/* Preserve channels outside the two channels represented by the public API. */
	memset(&control, 0, sizeof(control));
	control.id = audio->volume.id;
	error = ioctl(audio->fd, SNDRV_CTL_IOCTL_ELEM_READ, &control);
	if (error != 0)
		return errno;
	range = audio->volume.value.integer.max - audio->volume.value.integer.min;
	control.value.integer.value[0] = audio->volume.value.integer.min + ((long)left * range + 50) / 100;
	if (audio->volume.count > 1)
		control.value.integer.value[1] = audio->volume.value.integer.min + ((long)right * range + 50) / 100;
	error = ioctl(audio->fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &control);
	if (error != 0)
		return errno;
	audio->dirty = 1;

	/* A switch uses one for audible output and zero for mute. */
	if (audio->has_toggle != 0) {
		memset(&control, 0, sizeof(control));
		control.id = audio->toggle.id;
		for (index = 0; index < audio->toggle.count; index++) {
			if (muted != 0) {
				control.value.integer.value[index] = 0;
			} else {
				control.value.integer.value[index] = 1;
			}
		}

		/* Failure is visible even if the preceding volume write succeeded. */
		error = ioctl(audio->fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &control);
		if (error != 0)
			return errno;
	}

	/* Succeeded: update reads back the actual hardware quantization. */
	return 0;
}

/*
 * Accepts feedback silently because this backend does not play PCM.
 */
int
kl_backend_audio_feedback(
	struct kl_backend_audio *audio)
{
	/* Feedback cannot be submitted without an active mixer subscription. */
	if (audio == NULL)
		return EINVAL;
	if (audio->fd < 0)
		return ENOTCONN;

	/* Succeeded: no PCM playback was requested. */
	return 0;
}

/*
 * Reports whether an accessible control device has a usable volume element.
 */
int
kl_backend_audio_available(
	void)
{
	struct kl_backend_audio audio;
	int error;

	/* Probe local control metadata without allocating a lasting subscription. */
	memset(&audio, 0, sizeof(audio));
	audio.fd = -1;
	error = audio_connect(&audio);
	if (error != 0)
		return 0;
	(void)close(audio.fd);
	if (audio.state.device == 0)
		return 0;

	/* Succeeded: a volume control can be reached with the caller's permissions. */
	return 1;
}

/* Reads monotonic time for device retries independently of wall clock changes. */
static uint64_t
audio_milliseconds(
	void)
{
	struct timespec now;
	int error;

	/* A failed clock read cannot leave uninitialized retry data. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0;

	/* Succeeded: retries are measured in elapsed milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Finds an accessible ALSA card and subscribes to its mixer changes. */
static int
audio_connect(
	struct kl_backend_audio *audio)
{
	struct snd_ctl_card_info card;
	struct kl_backend_audio_state state;
	char path[64];
	unsigned index;
	int subscribe;
	int error;

	/* An unsuccessful scan is retried only after the ordinary reconnect delay. */
	audio->retry = audio_milliseconds() + AUDIO_RETRY_MS;
	for (index = 0; index < 32; index++) {
		(void)snprintf(path, sizeof(path), "/dev/snd/controlC%u", index);
		audio->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (audio->fd < 0)
			continue;
		memset(&card, 0, sizeof(card));
		error = ioctl(audio->fd, SNDRV_CTL_IOCTL_CARD_INFO, &card);
		if (error != 0) {
			(void)close(audio->fd);
			audio->fd = -1;
			continue;
		}

		/* Every usable card must first expose valid element metadata. */
		error = audio_elements(audio);
		if (error != 0) {
			(void)close(audio->fd);
			audio->fd = -1;
			continue;
		}

		/* An event subscription is required for other-process volume changes. */
		subscribe = 1;
		error = ioctl(audio->fd, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribe);
		if (error != 0) {
			(void)close(audio->fd);
			audio->fd = -1;
			continue;
		}

		/* Initial readback establishes actual volume and mute before publication. */
		error = audio_state(audio, &state);
		if (error != 0) {
			(void)close(audio->fd);
			audio->fd = -1;
			continue;
		}

		/* The selected device owns this subscription until loss or close. */
		audio->state = state;
		audio->sampled = 0;
		audio->dirty = 1;
		return 0;
	}

	/* A caller without usable mixer access remains disconnected. */
	return ENODEV;
}

/* Resolves the preferred volume and its matching mute switch from kernel IDs. */
static int
audio_elements(
	struct kl_backend_audio *audio)
{
	struct snd_ctl_elem_list list;
	struct snd_ctl_elem_id *ids;
	const char *volume_names[3] = {"Master Playback Volume", "PCM Playback Volume", "Speaker Playback Volume"};
	const char *toggle_names[3] = {"Master Playback Switch", "PCM Playback Switch", "Speaker Playback Switch"};
	unsigned index;
	int error;

	/* Query a bounded element count before allocating the kernel's ID list. */
	memset(&list, 0, sizeof(list));
	error = ioctl(audio->fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list);
	if (error != 0)
		return errno;
	if (list.count == 0 || list.count > AUDIO_ELEMENTS_MAX)
		return EOVERFLOW;
	ids = calloc(list.count, sizeof(*ids));
	if (ids == NULL)
		return ENOMEM;

	/* One allocation owns all element IDs only until metadata is copied. */
	list.space = list.count;
	list.pids = ids;
	error = ioctl(audio->fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list);
	if (error != 0) {
		error = errno;
		free(ids);
		return error;
	}

	/* Refuse a hotplug-expanded list rather than indexing outside its allocation. */
	if (list.used > list.space) {
		free(ids);
		return EOVERFLOW;
	}

	/* Prefer Master, then PCM, then Speaker volume, retaining only safe metadata. */
	memset(&audio->volume, 0, sizeof(audio->volume));
	memset(&audio->toggle, 0, sizeof(audio->toggle));
	audio->has_toggle = 0;
	for (index = 0; index < 3; index++) {
		error = audio_element(audio, ids, list.used, volume_names[index], SNDRV_CTL_ELEM_TYPE_INTEGER, &audio->volume);
		if (error != 0)
			continue;
		error = audio_element(audio, ids, list.used, toggle_names[index], SNDRV_CTL_ELEM_TYPE_BOOLEAN, &audio->toggle);
		if (error == 0)
			audio->has_toggle = 1;
		break;
	}

	/* The device remains reachable even if it has no supported volume element. */
	free(ids);
	if (index == 3)
		memset(&audio->volume, 0, sizeof(audio->volume));

	/* Succeeded: no borrowed list storage remains in either element description. */
	return 0;
}

/* Copies a named element's validated integer or Boolean metadata. */
static int
audio_element(
	struct kl_backend_audio *audio,
	const struct snd_ctl_elem_id *ids,
	size_t count,
	const char *name,
	snd_ctl_elem_type_t type,
	struct snd_ctl_elem_info *info)
{
	size_t index;
	int same;
	int error;

	/* Name and interface must match the playback mixer control. */
	for (index = 0; index < count; index++) {
		same = strncmp((const char *)ids[index].name, name, sizeof(ids[index].name));
		if (same != 0 || ids[index].iface != SNDRV_CTL_ELEM_IFACE_MIXER)
			continue;
		memset(info, 0, sizeof(*info));
		info->id = ids[index];
		error = ioctl(audio->fd, SNDRV_CTL_IOCTL_ELEM_INFO, info);
		if (error != 0)
			return errno;
		if (info->type != type ||
		    info->count == 0 ||
		    info->count > 128)
			return EPROTO;

		/* Integer mapping is bounded to avoid signed overflow on malformed metadata. */
		if (type == SNDRV_CTL_ELEM_TYPE_INTEGER) {
			if (info->value.integer.min < -1000000 ||
			    info->value.integer.max > 1000000 ||
			    info->value.integer.min >= info->value.integer.max)
				return EPROTO;
		}

		/* Succeeded: the copied ID and range remain valid after freeing the list. */
		return 0;
	}

	/* A card may expose another supported fallback control instead. */
	return ENOENT;
}

/* Reads hardware quantization and the audible state into a clean snapshot. */
static int
audio_state(
	struct kl_backend_audio *audio,
	struct kl_backend_audio_state *state)
{
	struct snd_ctl_elem_value control;
	long minimum;
	long maximum;
	int error;

	/* A card without a supported element is reachable but has no public device. */
	memset(state, 0, sizeof(*state));
	state->reachable = 1;
	if (audio->volume.count == 0)
		return 0;
	memset(&control, 0, sizeof(control));
	control.id = audio->volume.id;
	error = ioctl(audio->fd, SNDRV_CTL_IOCTL_ELEM_READ, &control);
	if (error != 0)
		return errno;

	/* Public channels use the kernel's linear raw range, with a mono mirror. */
	state->device = 1;
	state->channels = 2;
	minimum = audio->volume.value.integer.min;
	maximum = audio->volume.value.integer.max;
	state->left = audio_percentage(control.value.integer.value[0], minimum, maximum);
	state->right = state->left;
	if (audio->volume.count > 1)
		state->right = audio_percentage(control.value.integer.value[1], minimum, maximum);

	/* Either muted output channel makes the public mixer state muted. */
	if (audio->has_toggle != 0) {
		memset(&control, 0, sizeof(control));
		control.id = audio->toggle.id;
		error = ioctl(audio->fd, SNDRV_CTL_IOCTL_ELEM_READ, &control);
		if (error != 0)
			return errno;
		if (control.value.integer.value[0] == 0)
			state->muted = 1;
		if (audio->toggle.count > 1 && control.value.integer.value[1] == 0)
			state->muted = 1;
	}

	/* Succeeded: the snapshot reflects readback, not the last requested volume. */
	return 0;
}

/* Clears descriptor ownership after device or event failure. */
static void
audio_drop(
	struct kl_backend_audio *audio)
{
	/* Closing releases the event subscription as well as control access. */
	if (audio->fd >= 0)
		(void)close(audio->fd);
	audio->fd = -1;
	memset(&audio->state, 0, sizeof(audio->state));
	audio->retry = audio_milliseconds() + AUDIO_RETRY_MS;

	/* Succeeded: only reconnect may publish reachable state again. */
	return;
}

/* Maps the validated raw range to a clamped integer public percentage. */
static unsigned
audio_percentage(
	long sample,
	long minimum,
	long maximum)
{
	/* Drivers may report transient values outside their advertised range. */
	if (sample <= minimum)
		return 0;
	if (sample >= maximum)
		return 100;

	/* Succeeded: the integer percentage lies strictly inside zero to one hundred. */
	return (unsigned)((sample - minimum) * 100 / (maximum - minimum));
}
