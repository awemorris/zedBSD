/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pump of the sound's playback streams on Linux and FreeBSD (WS191
 * p004, plan/ws191/design.md section 6.4): pump.c makes a stream's ring in
 * a sealed memfd and runs a thread that carries the frames from the ring
 * to a sound device; each system gives its device through these calls
 * (alsa-lib on Linux, OSS on FreeBSD).  Every call of a device is made on
 * the pump's thread only, and none waits.
 */

#ifndef KL_BACKEND_AUDIO_PUMP_H
#define KL_BACKEND_AUDIO_PUMP_H

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <poll.h>

struct pump_device;

/* What a device can do; the pump calls them on its thread. */
struct pump_device_ops {
	/*
	 * Opens the device for a format: 0 with the device, the format it
	 * takes (KL_BACKEND_AUDIO_FORMAT_*, the asked one or S16_LE, which
	 * the pump converts to) and its period in frames; or a
	 * KL_BACKEND_AUDIO_ERROR_* value.
	 */
	unsigned (*open)(const struct kl_backend_audio_stream_format *format, struct pump_device **device, unsigned *device_format, unsigned *period_frames);
	/* Fills the descriptors to wait on until frames may be written; gives how many (at most capacity). */
	int (*descriptors)(struct pump_device *device, struct pollfd *descriptors, int capacity);
	/* Gives how many frames may be written now (0 when none or after a recovered failure), or a negative errno value when the device is gone. */
	long (*avail)(struct pump_device *device);
	/* Writes frames without waiting; gives how many, or a negative errno value when the device is gone. */
	long (*write)(struct pump_device *device, const void *frames, unsigned long count);
	/* Gives the frames written and not heard yet (0 when it cannot tell). */
	long (*delay)(struct pump_device *device);
	/* Pauses (on 1) or resumes (on 0) the device; a device that cannot pause drops what it holds. */
	void (*pause)(struct pump_device *device, int on);
	/* Drops what the device holds and makes it ready again. */
	void (*drop)(struct pump_device *device);
	/* Starts the device playing what it holds (a drain of less than its start threshold). */
	void (*start)(struct pump_device *device);
	/* Tells whether the device has played out what it held (1) or not (0). */
	int (*idle)(struct pump_device *device);
	/* Closes the device. */
	void (*close)(struct pump_device *device);
};

/*
 * Makes a stream on a device: the ring, and the pump's thread, which opens
 * the device and reports READY or FAILED.  Returns NULL with errno when
 * the ring or the thread cannot be made.
 */
struct kl_backend_audio_stream *kl_backend_audio_pump_open(const struct pump_device_ops *ops, const struct kl_backend_audio_stream_format *format);

#endif
