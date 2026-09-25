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
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* How far ahead of the device audiod keeps sound, in fragments. */
#define AUDIOD_FILL_FRAGMENTS	2U

static void fill_mapped(struct audiod_device *device);
static void fill_written(struct audiod_device *device);
static void read_capture(struct audiod_device *device);
static void finish_drains(struct audiod_device *device);
static int allocate_buffers(struct audiod_device *device);

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
		if (ioctl(device->dsp, KERN_AUDIO_GET_FORMAT, &format) == 0 &&
		    format.channels != 2U) {
			format.format = KERN_AUDIO_FORMAT_S16_LE;
			format.channels = 2;
			format.rate = 48000;
			format.reserved = 0;
			(void)ioctl(device->dsp, KERN_AUDIO_SET_FORMAT, &format);
			(void)ioctl(device->dsp, KERN_AUDIO_GET_FORMAT, &format);
		}
		if (format.channels != 2U ||
		    ioctl(device->dsp, KERN_AUDIO_GET_BUFFER, &info) != 0) {
			close(device->dsp);
			device->dsp = -1;
		} else {
			device->format = format.format;
			device->rate = format.rate;
			device->fragment_bytes = info.fragment_bytes;
			device->fragment_count = info.fragment_count;
			device->capture = ioctl(device->dsp, KERN_AUDIO_GET_ISPACE, &space) == 0;
		}
	}
	device->frame_bytes = audiod_frame_bytes(device->format, device->channels);
	device->period_frames = device->fragment_bytes / device->frame_bytes;
	if (allocate_buffers(device) != 0)
		return -1;

	/* Maps the playback ring when the device can, and starts it. */
	if (device->dsp >= 0 &&
	    ioctl(device->dsp, KERN_AUDIO_GET_CAPS, &caps) == 0 &&
	    (caps.caps & KERN_AUDIO_CAP_MMAP) != 0) {
		map = mmap(NULL, caps.mmap_bytes, PROT_READ | PROT_WRITE,
		    MAP_SHARED, device->dsp, 0);
		if (map != MAP_FAILED) {
			device->map = map;
			device->map_bytes = caps.mmap_bytes;
			device->lead_bytes = caps.mmap_lead_bytes;
			fill_mapped(device);
			bits = KERN_AUDIO_TRIGGER_OUTPUT;
			if (ioctl(device->dsp, KERN_AUDIO_SET_TRIGGER, &bits) != 0) {
				munmap(device->map, device->map_bytes);
				device->map = NULL;
			}
		}
	}

	device->mixer = open("/dev/mixer0", O_RDWR);
	device->timer_next_ns = audiod_now_ns();
	return 0;
}

/* Reports the descriptor to poll and what to wait for, or -1. */
int
audiod_device_fd(
	const struct audiod_device *device,
	short *events)
{
	if (device->dsp < 0)
		return -1;

	/*
	 * The mapped ring is ready once its frontier moves.  A written ring
	 * has room nearly always, so it is waited on only while audiod keeps
	 * fewer periods queued than it wants; otherwise the timeout wakes it.
	 */
	*events = 0;
	if (device->map != NULL ||
	    device->written - device->consumed <
	    AUDIOD_FILL_FRAGMENTS * device->fragment_bytes)
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

/* Sets the device volume, in percent. */
void
audiod_device_set_volume(
	struct audiod_device *device,
	uint32_t left,
	uint32_t right,
	uint32_t muted)
{
	struct audio_volume volume;

	if (device->mixer < 0)
		return;
	volume.left = left;
	volume.right = right;
	volume.muted = muted;
	volume.reserved = 0;
	(void)ioctl(device->mixer, KERN_AUDIO_SET_VOLUME, &volume);
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

	*left = 100;
	*right = 100;
	*muted = 0;
	if (device->mixer >= 0 &&
	    ioctl(device->mixer, KERN_AUDIO_GET_VOLUME, &volume) == 0) {
		*left = volume.left;
		*right = volume.right;
		*muted = volume.muted;
	}
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
	target = position.bytes + AUDIOD_FILL_FRAGMENTS * device->fragment_bytes;
	while (device->written < target) {
		audiod_mix_period(device,
		    device->map + device->written % device->map_bytes);
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

	for (;;) {
		if (ioctl(device->dsp, KERN_AUDIO_GET_OSPACE, &space) != 0)
			return;
		device->consumed = space.transferred;
		if (space.bytes < device->fragment_bytes ||
		    device->written - space.transferred >=
		    AUDIOD_FILL_FRAGMENTS * device->fragment_bytes)
			return;
		audiod_mix_period(device, device->scratch);
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
