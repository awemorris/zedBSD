/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sound's playback streams on Linux (keiland-backend.h, WS191 p004):
 * the pump (libkeiland-backend/audio/pump.c) writes to alsa-lib's PCM
 * "default", which on a desktop with PipeWire goes through pipewire-alsa.
 * alsa-lib is opened with dlopen (the user's decision, plan/ws191/
 * design.md H2): without it, or without one of the calls used, there are
 * no streams, and the compositor does not offer them.  Its declarations
 * are written here (the types are opaque, the values alsa-lib's ABI), so
 * that the build needs no alsa-lib headers.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include "userland/desktop/libkeiland-backend/audio/pump.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The library opened (a test builds with another one). */
#ifndef KL_BACKEND_ALSA_LIBRARY
#define KL_BACKEND_ALSA_LIBRARY "libasound.so.2"
#endif

/* alsa-lib's values: the playback stream, the open's non-blocking mode, the interleaved access, the formats, the states. */
#define ALSA_STREAM_PLAYBACK		0
#define ALSA_NONBLOCK			1
#define ALSA_ACCESS_RW_INTERLEAVED	3
#define ALSA_FORMAT_S16_LE		2
#define ALSA_FORMAT_S32_LE		10
#define ALSA_FORMAT_FLOAT_LE		14
#define ALSA_STATE_RUNNING		3
#define ALSA_STATE_XRUN			4
#define ALSA_STATE_PAUSED		6

/* The latency asked of the PCM (microseconds). */
#define ALSA_LATENCY_US			40000U

/*
 * How many times a PCM is tried, and how long apart (milliseconds): the
 * sound service of a session that has just started (PipeWire) can refuse
 * the first open or its format for a moment (T1-451).  The open runs on the
 * stream's pump, so the waiting holds nothing else up.
 */
#define ALSA_OPEN_TRIES			5U
#define ALSA_RETRY_MS			200U

/*
 * alsa-lib's calls, found once (alsa_load): the table is written before
 * alsa_loaded is set under alsa_lock, and read only after.
 */
static struct {
	int (*pcm_open)(void **pcm, const char *name, int stream, int mode);
	int (*pcm_set_params)(void *pcm, int format, int access, unsigned channels, unsigned rate, int soft_resample, unsigned latency);
	int (*pcm_get_params)(void *pcm, unsigned long *buffer_size, unsigned long *period_size);
	long (*pcm_writei)(void *pcm, const void *buffer, unsigned long size);
	long (*pcm_avail_update)(void *pcm);
	int (*pcm_delay)(void *pcm, long *delay);
	int (*pcm_pause)(void *pcm, int enable);
	int (*pcm_drop)(void *pcm);
	int (*pcm_prepare)(void *pcm);
	int (*pcm_start)(void *pcm);
	int (*pcm_state)(void *pcm);
	int (*pcm_recover)(void *pcm, int error, int silent);
	int (*pcm_poll_descriptors_count)(void *pcm);
	int (*pcm_poll_descriptors)(void *pcm, struct pollfd *descriptors, unsigned space);
	int (*pcm_close)(void *pcm);
	int (*sw_params_malloc)(void **params);
	int (*sw_params_current)(void *pcm, void *params);
	int (*sw_params_set_start_threshold)(void *pcm, void *params, unsigned long value);
	int (*sw_params)(void *pcm, void *params);
	void (*sw_params_free)(void *params);
} alsa;

/*
 * Whether alsa-lib was looked for, and found with every call; and the lock
 * that orders the looking and the opening of PCMs (an old alsa-lib reads
 * its configuration unsafely from several threads).
 */
static pthread_mutex_t alsa_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned alsa_looked;
static unsigned alsa_loaded;

/* One PCM, a pump's device. */
struct pump_device {
	void *pcm;
	unsigned long buffer_size;
};

static int alsa_load(void);
static void *alsa_symbol(void *library, const char *name, int *missing);
static unsigned alsa_open(const struct kl_backend_audio_stream_format *format, struct pump_device **device, unsigned *device_format, unsigned *period_frames);
static int alsa_descriptors(struct pump_device *device, struct pollfd *descriptors, int capacity);
static long alsa_avail(struct pump_device *device);
static long alsa_write(struct pump_device *device, const void *frames, unsigned long count);
static long alsa_delay(struct pump_device *device);
static void alsa_pause(struct pump_device *device, int on);
static void alsa_drop(struct pump_device *device);
static void alsa_start(struct pump_device *device);
static int alsa_idle(struct pump_device *device);
static void alsa_close(struct pump_device *device);
static unsigned alsa_error(int error);
static unsigned alsa_open_once(int alsa_format, unsigned channels, unsigned rate, void **pcm, int *retry);
static void alsa_sleep_ms(unsigned ms);

/* The device calls of an alsa-lib PCM. */
static const struct pump_device_ops alsa_ops = {
	alsa_open,
	alsa_descriptors,
	alsa_avail,
	alsa_write,
	alsa_delay,
	alsa_pause,
	alsa_drop,
	alsa_start,
	alsa_idle,
	alsa_close
};

/*
 * Tells whether streams can be made: alsa-lib is there with every call
 * the pump uses.
 */
int
kl_backend_audio_stream_supported(
	void)
{
	int loaded;

	/* Looked for once. */
	loaded = alsa_load();

	/* Succeeded: whether it was found. */
	return loaded;
}

/*
 * Starts making a stream on alsa-lib's default PCM.
 */
struct kl_backend_audio_stream *
kl_backend_audio_stream_open(
	const struct kl_backend_audio_stream_format *format)
{
	struct kl_backend_audio_stream *stream;
	int loaded;

	/* No alsa-lib: no streams. */
	loaded = alsa_load();
	if (!loaded) {
		errno = ENOTSUP;
		return NULL;
	}

	/* The ring and the pump, which opens the PCM. */
	stream = kl_backend_audio_pump_open(&alsa_ops, format);

	/* Succeeded: the stream, or NULL with errno. */
	return stream;
}

/* Opens alsa-lib and finds its calls, once; returns 1 when every call was found. */
static int
alsa_load(
	void)
{
	void *library;
	int missing;
	unsigned loaded;

	/* Looked for under the lock, once. */
	pthread_mutex_lock(&alsa_lock);

	if (!alsa_looked) {
		alsa_looked = 1U;
		missing = 0;
		library = dlopen(KL_BACKEND_ALSA_LIBRARY, RTLD_NOW | RTLD_LOCAL);
		if (library != NULL) {
			alsa.pcm_open = alsa_symbol(library, "snd_pcm_open", &missing);
			alsa.pcm_set_params = alsa_symbol(library, "snd_pcm_set_params", &missing);
			alsa.pcm_get_params = alsa_symbol(library, "snd_pcm_get_params", &missing);
			alsa.pcm_writei = alsa_symbol(library, "snd_pcm_writei", &missing);
			alsa.pcm_avail_update = alsa_symbol(library, "snd_pcm_avail_update", &missing);
			alsa.pcm_delay = alsa_symbol(library, "snd_pcm_delay", &missing);
			alsa.pcm_pause = alsa_symbol(library, "snd_pcm_pause", &missing);
			alsa.pcm_drop = alsa_symbol(library, "snd_pcm_drop", &missing);
			alsa.pcm_prepare = alsa_symbol(library, "snd_pcm_prepare", &missing);
			alsa.pcm_start = alsa_symbol(library, "snd_pcm_start", &missing);
			alsa.pcm_state = alsa_symbol(library, "snd_pcm_state", &missing);
			alsa.pcm_recover = alsa_symbol(library, "snd_pcm_recover", &missing);
			alsa.pcm_poll_descriptors_count = alsa_symbol(library, "snd_pcm_poll_descriptors_count", &missing);
			alsa.pcm_poll_descriptors = alsa_symbol(library, "snd_pcm_poll_descriptors", &missing);
			alsa.pcm_close = alsa_symbol(library, "snd_pcm_close", &missing);
			alsa.sw_params_malloc = alsa_symbol(library, "snd_pcm_sw_params_malloc", &missing);
			alsa.sw_params_current = alsa_symbol(library, "snd_pcm_sw_params_current", &missing);
			alsa.sw_params_set_start_threshold = alsa_symbol(library, "snd_pcm_sw_params_set_start_threshold", &missing);
			alsa.sw_params = alsa_symbol(library, "snd_pcm_sw_params", &missing);
			alsa.sw_params_free = alsa_symbol(library, "snd_pcm_sw_params_free", &missing);
		}

		/* Found whole (the library stays open: its calls are kept). */
		if (library != NULL && !missing)
			alsa_loaded = 1U;
	}

	/* What was found, then or before. */
	loaded = alsa_loaded;

	pthread_mutex_unlock(&alsa_lock);

	/* Succeeded: whether alsa-lib is there. */
	return (int)loaded;
}

/* Finds one call of alsa-lib; a missing one is counted. */
static void *
alsa_symbol(
	void *library,
	const char *name,
	int *missing)
{
	void *address;

	/* The call. */
	address = dlsym(library, name);
	if (address == NULL)
		(*missing)++;

	/* Succeeded: the call, or NULL. */
	return address;
}

/* Opens the default PCM for a format: the PCM's own conversion takes every format the streams carry. */
static unsigned
alsa_open(
	const struct kl_backend_audio_stream_format *format,
	struct pump_device **result,
	unsigned *device_format,
	unsigned *period_frames)
{
	struct pump_device *device;
	unsigned long period_size;
	unsigned long threshold;
	void *params;
	unsigned tries;
	unsigned error;
	int alsa_format;
	int status;
	int retry;

	/* The record. */
	device = calloc(1, sizeof(*device));
	if (device == NULL)
		return KL_BACKEND_AUDIO_ERROR_NO_MEMORY;

	/* alsa-lib's format. */
	alsa_format = ALSA_FORMAT_S16_LE;
	if (format->format == KL_BACKEND_AUDIO_FORMAT_S32_LE)
		alsa_format = ALSA_FORMAT_S32_LE;
	if (format->format == KL_BACKEND_AUDIO_FORMAT_F32_LE)
		alsa_format = ALSA_FORMAT_FLOAT_LE;

	/* The PCM with the format, tried again a few times while the service is not ready. */
	error = KL_BACKEND_AUDIO_ERROR_NONE;
	for (tries = 1U; tries <= ALSA_OPEN_TRIES; tries++) {
		/* One try. */
		retry = 0;
		error = alsa_open_once(alsa_format, format->channels, format->rate, &device->pcm, &retry);
		if (error == KL_BACKEND_AUDIO_ERROR_NONE)
			break;

		/* A failure that another try would not change. */
		if (!retry)
			break;

		/* A while before the next try. */
		if (tries < ALSA_OPEN_TRIES)
			alsa_sleep_ms(ALSA_RETRY_MS);
	}

	/* Not opened: why. */
	if (error != KL_BACKEND_AUDIO_ERROR_NONE) {
		free(device);
		return error;
	}

	/* The buffer and the period it chose. */
	device->buffer_size = 0U;
	period_size = 0U;
	(void)alsa.pcm_get_params(device->pcm, &device->buffer_size, &period_size);

	/* The PCM starts once a period is written (a short sound and a drain start it too). */
	threshold = period_size;
	if (threshold == 0U)
		threshold = 1U;
	params = NULL;
	status = alsa.sw_params_malloc(&params);
	if (status == 0) {
		(void)alsa.sw_params_current(device->pcm, params);
		(void)alsa.sw_params_set_start_threshold(device->pcm, params, threshold);
		(void)alsa.sw_params(device->pcm, params);
		alsa.sw_params_free(params);
	}

	/* Succeeded: the PCM takes the stream's format. */
	*result = device;
	*device_format = format->format;
	*period_frames = (unsigned)period_size;
	return KL_BACKEND_AUDIO_ERROR_NONE;
}

/* Fills the PCM's poll descriptors. */
static int
alsa_descriptors(
	struct pump_device *device,
	struct pollfd *descriptors,
	int capacity)
{
	int count;

	/* As many as there is room for. */
	count = alsa.pcm_poll_descriptors(device->pcm, descriptors, (unsigned)capacity);
	if (count < 0)
		return 0;

	/* Succeeded: the descriptors. */
	return count;
}

/* Gives the frames the PCM takes now; an underrun or a suspend is recovered (0 then). */
static long
alsa_avail(
	struct pump_device *device)
{
	long avail;
	int status;

	/* The room. */
	avail = alsa.pcm_avail_update(device->pcm);
	if (avail >= 0)
		return avail;

	/* A failure recovered, or the device gone. */
	status = alsa.pcm_recover(device->pcm, (int)avail, 1);
	if (status < 0)
		return -ENODEV;

	/* Succeeded: recovered, with nothing to take yet. */
	return 0;
}

/* Writes frames without waiting. */
static long
alsa_write(
	struct pump_device *device,
	const void *frames,
	unsigned long count)
{
	long written;
	int status;

	/* The frames. */
	written = alsa.pcm_writei(device->pcm, frames, count);
	if (written >= 0)
		return written;

	/* No room now. */
	if (written == -EAGAIN)
		return 0;

	/* A failure recovered (an underrun), or the device gone. */
	status = alsa.pcm_recover(device->pcm, (int)written, 1);
	if (status < 0)
		return -ENODEV;

	/* Succeeded: recovered, nothing written. */
	return 0;
}

/* Gives the frames the PCM holds and has not played. */
static long
alsa_delay(
	struct pump_device *device)
{
	long delay;
	int status;

	/* The delay; an error says nothing is known. */
	delay = 0;
	status = alsa.pcm_delay(device->pcm, &delay);
	if (status < 0 || delay < 0)
		return 0;

	/* Succeeded: the frames held. */
	return delay;
}

/* Pauses or resumes the PCM; one that cannot pause drops what it holds. */
static void
alsa_pause(
	struct pump_device *device,
	int on)
{
	int state;
	int status;

	/* Only a running PCM pauses, and only a paused one resumes. */
	state = alsa.pcm_state(device->pcm);
	if (on && state == ALSA_STATE_RUNNING) {
		status = alsa.pcm_pause(device->pcm, 1);
		if (status < 0)
			alsa_drop(device);
		return;
	}

	/* Resumed. */
	if (!on && state == ALSA_STATE_PAUSED)
		(void)alsa.pcm_pause(device->pcm, 0);
}

/* Drops what the PCM holds and makes it ready again. */
static void
alsa_drop(
	struct pump_device *device)
{
	/* Dropped, then prepared. */
	(void)alsa.pcm_drop(device->pcm);
	(void)alsa.pcm_prepare(device->pcm);
}

/* Starts the PCM playing what it holds. */
static void
alsa_start(
	struct pump_device *device)
{
	/* Started (a running one answers an error, which says nothing). */
	(void)alsa.pcm_start(device->pcm);
}

/* Tells whether the PCM played out what it held. */
static int
alsa_idle(
	struct pump_device *device)
{
	long avail;
	int state;

	/* Not running any more (it ran dry: an underrun at the end). */
	state = alsa.pcm_state(device->pcm);
	if (state != ALSA_STATE_RUNNING)
		return 1;

	/* Running with the whole buffer free. */
	avail = alsa.pcm_avail_update(device->pcm);
	if (avail >= 0 && device->buffer_size != 0U && (unsigned long)avail >= device->buffer_size)
		return 1;

	/* Succeeded: still playing. */
	return 0;
}

/* Closes the PCM. */
static void
alsa_close(
	struct pump_device *device)
{
	/* The PCM, then the record. */
	(void)alsa.pcm_close(device->pcm);
	free(device);
}

/*
 * Opens the default PCM once and sets its format (resampled by alsa-lib
 * when the device's rate differs, at about 40 ms of latency).  Reports the
 * backend's error, and whether another try may succeed (a service not
 * ready yet), not for a device that is not there.
 */
static unsigned
alsa_open_once(
	int alsa_format,
	unsigned channels,
	unsigned rate,
	void **pcm,
	int *retry)
{
	unsigned error;
	int status;

	/* The PCM, opened one at a time, without blocking. */
	*retry = 0;
	pthread_mutex_lock(&alsa_lock);

	status = alsa.pcm_open(pcm, "default", ALSA_STREAM_PLAYBACK, ALSA_NONBLOCK);

	pthread_mutex_unlock(&alsa_lock);

	/* Not opened: no device, or the service not there (yet). */
	if (status < 0) {
		printf("KWL AUDIO alsa open error=%d\n", -status);
		error = alsa_error(status);
		if (error == KL_BACKEND_AUDIO_ERROR_UNAVAILABLE)
			*retry = 1;
		return error;
	}

	/* The format; a service that has just started may refuse it for a moment. */
	status = alsa.pcm_set_params(*pcm, alsa_format, ALSA_ACCESS_RW_INTERLEAVED, channels, rate, 1, ALSA_LATENCY_US);
	if (status < 0) {
		printf("KWL AUDIO alsa format error=%d\n", -status);
		(void)alsa.pcm_close(*pcm);
		*pcm = NULL;
		*retry = 1;
		if (status == -EINVAL)
			return KL_BACKEND_AUDIO_ERROR_INVALID;
		error = alsa_error(status);
		return error;
	}

	/* Succeeded: the PCM takes the format. */
	return KL_BACKEND_AUDIO_ERROR_NONE;
}

/* Waits a number of milliseconds (a signal cuts it short; the next try comes sooner). */
static void
alsa_sleep_ms(
	unsigned ms)
{
	struct timespec wait;

	/* The time, in seconds and nanoseconds. */
	wait.tv_sec = (time_t)(ms / 1000U);
	wait.tv_nsec = (long)(ms % 1000U) * 1000000L;
	(void)nanosleep(&wait, NULL);
}

/* Gives the backend's error for an open that failed. */
static unsigned
alsa_error(
	int error)
{
	/* No such device. */
	if (error == -ENOENT || error == -ENODEV || error == -ENXIO)
		return KL_BACKEND_AUDIO_ERROR_NO_DEVICE;

	/* Out of memory. */
	if (error == -ENOMEM)
		return KL_BACKEND_AUDIO_ERROR_NO_MEMORY;

	/* Succeeded: the service is not there now (busy, refused). */
	return KL_BACKEND_AUDIO_ERROR_UNAVAILABLE;
}
