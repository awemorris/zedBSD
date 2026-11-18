/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sound device side of audiod.
 *
 * With the OSS mmap interface audiod mixes straight into the mapped ring a
 * little ahead of the device's frontier; without it, it writes a period at
 * a time and keeps a few periods queued.  Without any device the streams
 * still move, one period per period of time, into nothing.
 */

#include "userland/base/audiod/audiod.h"

#include <uapi/audio.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static void fill_mapped(struct audiod_device *device);
static void fill_written(struct audiod_device *device);
static void read_capture(struct audiod_device *device);
static void finish_drains(struct audiod_device *device);
static int allocate_buffers(struct audiod_device *device);
static void feedback_track(struct audiod_device *device, uint64_t period_byte);
static void feedback_played(struct audiod_device *device);

/*
 * Opens /dev/dsp0 and /dev/mixer0.  A missing device is not an error:
 * audiod then runs on its own clock at 48 kHz stereo.
 */
int
audiod_device_open(
	struct audiod_device *device)
{
	struct audio_format format;
	struct audio_buffer_info info;
	struct audio_caps caps;
	struct audio_space space;
	uint32_t bits;
	void *map;
	int error;

	/* The defaults without a device: 48 kHz stereo S16, 8 fragments of 4096 bytes. */
	memset(device, 0, sizeof(*device));
	device->dsp = -1;
	device->mixer = -1;
	device->format = AUDIOD_FORMAT_S16_LE;
	device->channels = 2;
	device->rate = 48000;
	device->fragment_bytes = 4096;
	device->fragment_count = 8;

	/*
	 * A device opened write-only cannot be mapped, so playback is opened
	 * read-write; the device gives it whichever directions it has.
	 */
	device->dsp = open("/dev/dsp0", O_RDWR | O_NONBLOCK);
	if (device->dsp >= 0) {
		/* Its format; one that is not stereo is asked for S16 stereo at 48 kHz. */
		memset(&format, 0, sizeof(format));
		error = ioctl(device->dsp, KERN_AUDIO_GET_FORMAT, &format);
		if (error == 0 && format.channels != 2U) {
			format.format = KERN_AUDIO_FORMAT_S16_LE;
			format.channels = 2;
			format.rate = 48000;
			format.reserved = 0;
			(void)ioctl(device->dsp, KERN_AUDIO_SET_FORMAT, &format);
			(void)ioctl(device->dsp, KERN_AUDIO_GET_FORMAT, &format);
		}

		/* Its buffer, asked for only when it is stereo. */
		error = -1;
		if (format.channels == 2U)
			error = ioctl(device->dsp, KERN_AUDIO_GET_BUFFER, &info);

		/* A device that is not stereo, or has no buffer, is not used. */
		if (error != 0) {
			close(device->dsp);
			device->dsp = -1;
		} else {
			device->format = format.format;
			device->rate = format.rate;
			device->fragment_bytes = info.fragment_bytes;
			device->fragment_count = info.fragment_count;
			error = ioctl(device->dsp, KERN_AUDIO_GET_ISPACE, &space);
			device->capture = 0;
			if (error == 0)
				device->capture = 1;
		}
	}

	/* The frame and period sizes, and the buffers for them. */
	device->frame_bytes = audiod_frame_bytes(device->format, device->channels);
	device->period_frames = device->fragment_bytes / device->frame_bytes;
	error = allocate_buffers(device);
	if (error != 0)
		return -1;

	/* Maps the playback ring when the device can, and starts it. */
	error = -1;
	if (device->dsp >= 0)
		error = ioctl(device->dsp, KERN_AUDIO_GET_CAPS, &caps);
	if (error == 0 && (caps.caps & KERN_AUDIO_CAP_MMAP) != 0) {
		map = mmap(NULL, caps.mmap_bytes, PROT_READ | PROT_WRITE,
		    MAP_SHARED, device->dsp, 0);
		if (map != MAP_FAILED) {
			device->map = map;
			device->map_bytes = caps.mmap_bytes;
			device->lead_bytes = caps.mmap_lead_bytes;
			fill_mapped(device);

			/* Started; a device that cannot start is written to instead. */
			bits = KERN_AUDIO_TRIGGER_OUTPUT;
			error = ioctl(device->dsp, KERN_AUDIO_SET_TRIGGER, &bits);
			if (error != 0) {
				munmap(device->map, device->map_bytes);
				device->map = NULL;
			}
		}
	}

	/* The volume node, and the clock of a device-less run. */
	device->mixer = open("/dev/mixer0", O_RDWR);
	device->timer_next_ns = audiod_now_ns();

	/* The volume audiod applies itself until the device takes one (ws100-p002): full and unmuted. */
	device->soft_left = 100U;
	device->soft_right = 100U;
	device->soft_muted = 0U;

	/* The feedback sound for the device's rate; without it AUDIOD_FEEDBACK plays nothing. */
	(void)audiod_feedback_make(device);

	/* Succeeded: the device (or the clock without one) is ready. */
	return 0;
}

/* Reports the descriptor to poll and what to wait for, or -1. */
int
audiod_device_fd(
	const struct audiod_device *device,
	short *events)
{
	uint64_t ahead;

	if (device->dsp < 0)
		return -1;

	/*
	 * The mapped ring is ready once its frontier moves.  A written ring
	 * has room nearly always, so it is waited on only while audiod keeps
	 * fewer periods queued than it wants; otherwise the timeout wakes it.
	 */
	*events = 0;
	ahead = (uint64_t)audiod_route_fill_fragments() * device->fragment_bytes;
	if (device->map != NULL || device->written - device->consumed < ahead)
		*events = POLLOUT;
	if (device->capture_started)
		*events |= POLLIN;
	return device->dsp;
}

/* Reports how long poll may wait: forever with a device, a period without. */
int
audiod_device_timeout_ms(
	const struct audiod_device *device)
{
	int64_t left;

	if (device->dsp >= 0 && device->map != NULL)
		return 1000;
	if (device->dsp >= 0)
		return (int)((uint64_t)device->period_frames * 1000U / device->rate / 2U) + 1;
	left = device->timer_next_ns - audiod_now_ns();
	if (left <= 0)
		return 0;
	return (int)(left / 1000000) + 1;
}

/* Moves the device along: mixes what is due and hands on what was recorded. */
void
audiod_device_service(
	struct audiod_device *device,
	short revents)
{
	int64_t period_ns;

	if (device->dsp < 0) {
		/* Without a device each period of time consumes one period. */
		period_ns = (int64_t)device->period_frames * 1000000000 / device->rate;
		while (audiod_now_ns() >= device->timer_next_ns) {
			audiod_mix_period(device, device->scratch);
			device->written += device->fragment_bytes;
			device->consumed = device->written;
			device->timer_next_ns += period_ns;

			/* Capture streams get silence at the same rate. */
			memset(device->capture_raw, 0, device->fragment_bytes);
			audiod_capture_period(device, device->capture_raw);
		}
	} else if (device->map != NULL) {
		fill_mapped(device);
	} else {
		fill_written(device);
	}
	if (device->capture_started && (revents & POLLIN) != 0)
		read_capture(device);
	finish_drains(device);
}

/*
 * Starts the device recording, once: the first read starts it, and poll
 * then reports each recorded period.
 */
void
audiod_device_start_capture(
	struct audiod_device *device)
{
	if (device->dsp < 0 || !device->capture || device->capture_started)
		return;
	device->capture_started = 1;
	read_capture(device);
}

/*
 * Sets the device volume, in percent: the device's own volume when it has
 * one, otherwise audiod applies it to what it mixes (ws100-p002; before, a
 * device without a volume ignored the request).
 */
void
audiod_device_set_volume(
	struct audiod_device *device,
	uint32_t left,
	uint32_t right,
	uint32_t muted)
{
	struct audio_volume volume;
	int error;

	/* Kept for audiod's own volume and for the reports. */
	device->soft_left = left;
	device->soft_right = right;
	device->soft_muted = muted;

	/* The device's own volume, when it takes one. */
	error = -1;
	if (device->mixer >= 0) {
		volume.left = left;
		volume.right = right;
		volume.muted = muted;
		volume.reserved = 0;
		error = ioctl(device->mixer, KERN_AUDIO_SET_VOLUME, &volume);
	}

	/* Taken: audiod leaves what it mixes as it is. */
	if (error == 0) {
		device->soft = 0;
		return;
	}

	/* Otherwise audiod applies it. */
	device->soft = 1;
}

/* Reads the device volume, in percent; full and unmuted without a mixer. */
void
audiod_device_get_volume(
	struct audiod_device *device,
	uint32_t *left,
	uint32_t *right,
	uint32_t *muted)
{
	struct audio_volume volume;
	int error;

	/* Full and unmuted, unless something says otherwise. */
	*left = 100;
	*right = 100;
	*muted = 0;

	/* The volume audiod applies itself is the one in force (ws100-p002). */
	if (device->soft) {
		*left = device->soft_left;
		*right = device->soft_right;
		*muted = device->soft_muted;
		return;
	}

	/* Otherwise the device's own, when it reports one. */
	if (device->mixer < 0)
		return;
	error = ioctl(device->mixer, KERN_AUDIO_GET_VOLUME, &volume);
	if (error == 0) {
		*left = volume.left;
		*right = volume.right;
		*muted = volume.muted;
	}
}

/*
 * Plays the feedback sound from its start (AUDIOD_FEEDBACK, ws100-p002):
 * a sound still playing starts again, so quick changes never overlap.
 * Without a device, or without the sound, nothing plays.
 */
void
audiod_device_feedback(
	struct audiod_device *device)
{
	/* Nothing to play, or nowhere to play it. */
	if (device->feedback == NULL || device->dsp < 0)
		return;

	/* From its first frame, mixed from the next period on (mix.c). */
	device->feedback_next = 0;

	/* Its way to the device, when timed (ws100-p008). */
	device->feedback_asked_ns = audiod_now_ns();
	device->feedback_step = 1U;
	audiod_timing("feedback asked at_ms=%lld consumed=%llu written=%llu", (long long)(device->feedback_asked_ns / 1000000),
	    (unsigned long long)device->consumed, (unsigned long long)device->written);
}

/*
 * Writes one line of the feedback sound's timing to the file
 * AUDIOD_TIMING_LOG names (ws100-p008); nothing without it.
 */
void
audiod_timing(
	const char *format,
	...)
{
	static FILE *file;
	static int opened;
	const char *path;
	va_list arguments;

	/* The file, opened once. */
	if (!opened) {
		opened = 1;
		path = getenv("AUDIOD_TIMING_LOG");
		if (path != NULL)
			file = fopen(path, "a");
	}

	/* Nothing asked for. */
	if (file == NULL)
		return;

	/* The line. */
	fprintf(file, "AUDIOD timing ");
	va_start(arguments, format);
	vfprintf(file, format, arguments);
	va_end(arguments);
	fprintf(file, "\n");
	fflush(file);
}

/*
 * Notes the device byte of the feedback sound's first frame once the period
 * that starts it is placed at a device byte, and logs when the device takes
 * that byte (ws100-p008).
 */
static void
feedback_track(
	struct audiod_device *device,
	uint64_t period_byte)
{
	int64_t now_ns;

	/* The period just placed started the sound. */
	now_ns = audiod_now_ns();
	if (device->feedback_step == 2U) {
		device->feedback_byte = period_byte;
		device->feedback_step = 3U;
		audiod_timing("feedback mixed at_ms=%lld byte=%llu consumed=%llu ahead_ms=%llu", (long long)(now_ns / 1000000),
		    (unsigned long long)period_byte, (unsigned long long)device->consumed,
		    (unsigned long long)((period_byte - device->consumed) * 1000U / ((uint64_t)device->rate * device->frame_bytes)));
	}
}

/* Logs when the device has taken the feedback sound's first byte, estimated back from the byte it is at (ws100-p008). */
static void
feedback_played(
	struct audiod_device *device)
{
	uint64_t past_ms;
	int64_t now_ns;

	/* Only a sound mixed and not yet reached. */
	if (device->feedback_step != 3U || device->consumed < device->feedback_byte)
		return;

	/* The time the device reached the byte: now less the bytes it has taken since. */
	now_ns = audiod_now_ns();
	past_ms = (device->consumed - device->feedback_byte) * 1000U / ((uint64_t)device->rate * device->frame_bytes);
	device->feedback_step = 0U;
	audiod_timing("feedback played at_ms=%lld seen_ms=%lld asked_ms=%lld byte=%llu consumed=%llu",
	    (long long)(now_ns / 1000000 - (int64_t)past_ms), (long long)(now_ns / 1000000),
	    (long long)(device->feedback_asked_ns / 1000000), (unsigned long long)device->feedback_byte,
	    (unsigned long long)device->consumed);
}

/*
 * Mixes into the mapped ring up to a few fragments ahead of the frontier,
 * the position up to which the device has taken the ring.  If audiod fell
 * behind the frontier, it starts again at the frontier.
 */
static void
fill_mapped(
	struct audiod_device *device)
{
	struct audio_mmap_position position;
	uint64_t target;

	position.bytes = 0;
	(void)ioctl(device->dsp, KERN_AUDIO_GET_OPTR, &position);
	device->consumed = position.bytes;
	if (device->written < position.bytes)
		device->written = position.bytes;
	feedback_played(device);
	target = position.bytes + audiod_route_fill_fragments() * device->fragment_bytes;
	while (device->written < target) {
		audiod_mix_period(device,
		    device->map + device->written % device->map_bytes);
		feedback_track(device, device->written);
		device->written += device->fragment_bytes;
	}
}

/* Writes periods while the device holds fewer than a few. */
static void
fill_written(
	struct audiod_device *device)
{
	struct audio_space space;
	ssize_t count;
	uint64_t ahead;

	for (;;) {
		if (ioctl(device->dsp, KERN_AUDIO_GET_OSPACE, &space) != 0)
			return;
		device->consumed = space.transferred;
		feedback_played(device);
		ahead = (uint64_t)audiod_route_fill_fragments() * device->fragment_bytes;
		if (space.bytes < device->fragment_bytes || device->written - space.transferred >= ahead)
			return;
		audiod_mix_period(device, device->scratch);
		feedback_track(device, device->written);
		count = write(device->dsp, device->scratch, device->fragment_bytes);
		if (count != (ssize_t)device->fragment_bytes)
			return;
		device->written += device->fragment_bytes;
	}
}

/*
 * Reads what has been recorded and hands each whole period to the capture
 * streams.  A read returns what has arrived, which may be less than a
 * period, so the period is gathered across reads.
 */
static void
read_capture(
	struct audiod_device *device)
{
	ssize_t count;

	for (;;) {
		count = read(device->dsp, device->capture_raw + device->capture_fill,
		    device->fragment_bytes - device->capture_fill);
		if (count <= 0)
			return;
		device->capture_fill += (uint32_t)count;
		if (device->capture_fill == device->fragment_bytes) {
			audiod_capture_period(device, device->capture_raw);
			device->capture_fill = 0;
		}
	}
}

/* Reports each drain whose sound the device has played. */
static void
finish_drains(
	struct audiod_device *device)
{
	struct audiod_client *client;
	struct audiod_stream *stream;

	for (client = audiod_clients; client != NULL; client = client->next) {
		for (stream = client->streams; stream != NULL; stream = stream->next) {
			if (!stream->draining || stream->drain_target == 0 ||
			    device->consumed < stream->drain_target)
				continue;
			stream->draining = 0;
			stream->drain_target = 0;
			stream->running = 0;
			stream->shm->state = AUDIOD_STATE_STOPPED;
			audiod_send_event(stream, AUDIOD_DRAINED, stream->drain_serial);
		}
	}
}

/* Allocates the period buffers. */
static int
allocate_buffers(
	struct audiod_device *device)
{
	device->scratch = malloc(device->fragment_bytes);
	device->capture_raw = malloc(device->fragment_bytes);
	device->mix = malloc((size_t)device->period_frames * 2U * sizeof(*device->mix));
	device->capture_frames = malloc((size_t)device->period_frames * 2U *
	    sizeof(*device->capture_frames));
	if (device->scratch == NULL || device->capture_raw == NULL ||
	    device->mix == NULL || device->capture_frames == NULL)
		return -1;
	memset(device->scratch, 0, device->fragment_bytes);
	return 0;
}
