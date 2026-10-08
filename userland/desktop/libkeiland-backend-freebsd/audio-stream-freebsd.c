/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sound's playback streams on FreeBSD (keiland-backend.h, WS191
 * p004): the pump (libkeiland-backend/audio/pump.c) writes to OSS's
 * /dev/dsp, which sound(4) mixes and converts to the device's rate.  The
 * device is opened without blocking with a few short fragments (about
 * 40 ms of latency).  A format the device does not take is written as
 * 16-bit (the pump converts).  A stop leaves what the device holds to play
 * out (OSS cannot pause a stream it plays); a flush drops it.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include "userland/desktop/libkeiland-backend/audio/pump.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <unistd.h>

/* The device, and the fragments asked for: four, of about 10 ms each. */
#define OSS_DEVICE		"/dev/dsp"
#define OSS_FRAGMENTS		4U

/* One OSS device, a pump's: its descriptor and the bytes of a frame it takes. */
struct pump_device {
	int fd;
	unsigned frame_bytes;
};

static unsigned oss_open(const struct kl_backend_audio_stream_format *format, struct pump_device **device, unsigned *device_format, unsigned *period_frames);
static int oss_descriptors(struct pump_device *device, struct pollfd *descriptors, int capacity);
static long oss_avail(struct pump_device *device);
static long oss_write(struct pump_device *device, const void *frames, unsigned long count);
static long oss_delay(struct pump_device *device);
static void oss_pause(struct pump_device *device, int on);
static void oss_drop(struct pump_device *device);
static void oss_start(struct pump_device *device);
static int oss_idle(struct pump_device *device);
static void oss_close(struct pump_device *device);
static int oss_format(unsigned format);

/* The device calls of an OSS device. */
static const struct pump_device_ops oss_ops = {
	oss_open,
	oss_descriptors,
	oss_avail,
	oss_write,
	oss_delay,
	oss_pause,
	oss_drop,
	oss_start,
	oss_idle,
	oss_close
};

/*
 * Tells whether streams can be made: OSS is FreeBSD's own (whether a
 * device is there is a stream's own FAILED).
 */
int
kl_backend_audio_stream_supported(
	void)
{
	/* Succeeded: FreeBSD's backend makes streams. */
	return 1;
}

/*
 * Starts making a stream on OSS's /dev/dsp.
 */
struct kl_backend_audio_stream *
kl_backend_audio_stream_open(
	const struct kl_backend_audio_stream_format *format)
{
	struct kl_backend_audio_stream *stream;

	/* The ring and the pump, which opens the device. */
	stream = kl_backend_audio_pump_open(&oss_ops, format);

	/* Succeeded: the stream, or NULL with errno. */
	return stream;
}

/* Opens /dev/dsp for a format; a sample format it does not take is 16-bit (converted by the pump). */
static unsigned
oss_open(
	const struct kl_backend_audio_stream_format *format,
	struct pump_device **result,
	unsigned *device_format,
	unsigned *period_frames)
{
	struct pump_device *device;
	unsigned fragment_bytes;
	unsigned shift;
	int wanted;
	int value;
	int status;

	/* The record. */
	device = calloc(1, sizeof(*device));
	if (device == NULL)
		return KL_BACKEND_AUDIO_ERROR_NO_MEMORY;

	/* The device, without blocking; none is no device, a busy one the service not there now. */
	device->fd = open(OSS_DEVICE, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (device->fd < 0) {
		status = errno;
		free(device);
		if (status == ENOENT || status == ENXIO || status == ENODEV)
			return KL_BACKEND_AUDIO_ERROR_NO_DEVICE;
		return KL_BACKEND_AUDIO_ERROR_UNAVAILABLE;
	}

	/* The sample format: the stream's, else 16-bit. */
	wanted = oss_format(format->format);
	*device_format = format->format;
	if (wanted == AFMT_S16_LE)
		*device_format = KL_BACKEND_AUDIO_FORMAT_S16_LE;
	value = wanted;
	status = ioctl(device->fd, SNDCTL_DSP_SETFMT, &value);
	if (status != 0 || value != wanted) {
		*device_format = KL_BACKEND_AUDIO_FORMAT_S16_LE;
		value = AFMT_S16_LE;
		status = ioctl(device->fd, SNDCTL_DSP_SETFMT, &value);
		if (status != 0 || value != AFMT_S16_LE) {
			oss_close(device);
			return KL_BACKEND_AUDIO_ERROR_INVALID;
		}
	}

	/* The bytes of a frame on the device. */
	device->frame_bytes = 4U * format->channels;
	if (*device_format == KL_BACKEND_AUDIO_FORMAT_S16_LE)
		device->frame_bytes = 2U * format->channels;

	/* The channels and the rate, as asked (sound(4) converts to the device's). */
	value = (int)format->channels;
	status = ioctl(device->fd, SNDCTL_DSP_CHANNELS, &value);
	if (status != 0 || value != (int)format->channels) {
		oss_close(device);
		return KL_BACKEND_AUDIO_ERROR_INVALID;
	}

	/* The rate. */
	value = (int)format->rate;
	status = ioctl(device->fd, SNDCTL_DSP_SPEED, &value);
	if (status != 0 || value != (int)format->rate) {
		oss_close(device);
		return KL_BACKEND_AUDIO_ERROR_INVALID;
	}

	/* A few short fragments: the smallest power of two of bytes holding 10 ms. */
	fragment_bytes = format->rate / 100U * device->frame_bytes;
	shift = 4U;
	while ((1U << shift) < fragment_bytes && shift < 16U)
		shift++;
	value = (int)((OSS_FRAGMENTS << 16) | shift);
	(void)ioctl(device->fd, SNDCTL_DSP_SETFRAGMENT, &value);

	/* The period: a fragment's frames. */
	value = 0;
	status = ioctl(device->fd, SNDCTL_DSP_GETBLKSIZE, &value);
	*period_frames = 0U;
	if (status == 0 && value > 0)
		*period_frames = (unsigned)value / device->frame_bytes;

	/* Succeeded: the device is the pump's. */
	*result = device;
	return KL_BACKEND_AUDIO_ERROR_NONE;
}

/* Fills the device's descriptor: writable is room for frames. */
static int
oss_descriptors(
	struct pump_device *device,
	struct pollfd *descriptors,
	int capacity)
{
	/* No room for it. */
	if (capacity < 1)
		return 0;

	/* The descriptor. */
	descriptors[0].fd = device->fd;
	descriptors[0].events = POLLOUT;
	descriptors[0].revents = 0;

	/* Succeeded: one. */
	return 1;
}

/* Gives the frames the device takes now. */
static long
oss_avail(
	struct pump_device *device)
{
	audio_buf_info info;
	int status;

	/* The room, in bytes. */
	status = ioctl(device->fd, SNDCTL_DSP_GETOSPACE, &info);
	if (status != 0)
		return -errno;

	/* Succeeded: whole frames. */
	return (long)((unsigned)info.bytes / device->frame_bytes);
}

/* Writes frames without waiting. */
static long
oss_write(
	struct pump_device *device,
	const void *frames,
	unsigned long count)
{
	ssize_t written;

	/* The bytes. */
	written = write(device->fd, frames, count * device->frame_bytes);
	if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return 0;

	/* The device went. */
	if (written < 0)
		return -errno;

	/* Succeeded: whole frames written. */
	return (long)((size_t)written / device->frame_bytes);
}

/* Gives the frames the device holds and has not played. */
static long
oss_delay(
	struct pump_device *device)
{
	int bytes;
	int status;

	/* The delay, in bytes; an error says nothing is known. */
	bytes = 0;
	status = ioctl(device->fd, SNDCTL_DSP_GETODELAY, &bytes);
	if (status != 0 || bytes < 0)
		return 0;

	/* Succeeded: whole frames. */
	return (long)((unsigned)bytes / device->frame_bytes);
}

/* Pauses or resumes: OSS plays out what it holds (the pump just writes no more). */
static void
oss_pause(
	struct pump_device *device,
	int on)
{
	/* Nothing to do on the device. */
	(void)device;
	(void)on;
}

/* Drops what the device holds. */
static void
oss_drop(
	struct pump_device *device)
{
	/* Reset: the device is empty and ready for the next write. */
	(void)ioctl(device->fd, SNDCTL_DSP_RESET, NULL);
}

/* Starts the device: OSS starts with its first write. */
static void
oss_start(
	struct pump_device *device)
{
	/* Nothing to do on the device. */
	(void)device;
}

/* Tells whether the device played out what it held. */
static int
oss_idle(
	struct pump_device *device)
{
	long delay;

	/* Nothing held. */
	delay = oss_delay(device);
	if (delay == 0)
		return 1;

	/* Succeeded: still playing. */
	return 0;
}

/* Closes the device. */
static void
oss_close(
	struct pump_device *device)
{
	/* The descriptor, then the record. */
	(void)close(device->fd);
	free(device);
}

/* Gives OSS's sample format of a stream's. */
static int
oss_format(
	unsigned format)
{
	/* A system without the 32-bit and float formats does not look at it. */
	(void)format;

#ifdef AFMT_S32_LE
	/* 32-bit. */
	if (format == KL_BACKEND_AUDIO_FORMAT_S32_LE)
		return AFMT_S32_LE;
#endif

#ifdef AFMT_F32_LE
	/* Float, where sound(4) has it. */
	if (format == KL_BACKEND_AUDIO_FORMAT_F32_LE)
		return AFMT_F32_LE;
#endif

	/* Succeeded: 16-bit (a float the system has no format for is converted to it). */
	return AFMT_S16_LE;
}
