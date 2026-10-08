/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Mixing and conversion.
 *
 * Samples are carried as 32-bit integers (S16 is shifted up 16 bits), so
 * one S16 stream at the device's rate and unity volume comes out exactly
 * as it went in.  Rates are converted by linear interpolation with a
 * 32.32 fixed-point position; equal rates never interpolate.
 */

#include "userland/base/audiod/audiod.h"

#include <stdlib.h>
#include <string.h>

/* The feedback sound (ws100-p002): its pitch, its length and rise in milliseconds, and its peak (-12 dBFS of the mix's scale). */
#define FEEDBACK_HZ		880.0
#define FEEDBACK_MS		100U
#define FEEDBACK_RISE_MS	5U
#define FEEDBACK_PEAK		536870912.0

/* The factor of full volume (1/65536), and the percents the device volume has (0..100). */
#define SOFT_UNITY		65536U
#define SOFT_PERCENTS		101U

/*
 * The device volume audiod applies itself, per percent, in 1/65536 (ws100-p009):
 * the curve dB(p) = 50 * log10(p / 100), so 25, 50 and 75 % are -30, -15
 * and -6 dB and the steps sound even; 0 is silent.  Made by
 * `python3 plan/ws100/tests/volume-curve.py table` (its `verify` checks
 * this copy); read only, for the process.
 */
static const uint32_t volume_factors[SOFT_PERCENTS] = {
	0U, 1U, 4U, 10U, 21U, 37U, 58U, 85U,
	119U, 159U, 207U, 263U, 327U, 399U, 481U, 571U,
	671U, 781U, 901U, 1031U, 1172U, 1324U, 1488U, 1663U,
	1849U, 2048U, 2259U, 2483U, 2719U, 2968U, 3231U, 3507U,
	3796U, 4100U, 4418U, 4750U, 5096U, 5457U, 5834U, 6225U,
	6632U, 7054U, 7492U, 7946U, 8416U, 8902U, 9405U, 9925U,
	10461U, 11015U, 11585U, 12173U, 12779U, 13402U, 14043U, 14702U,
	15380U, 16076U, 16790U, 17523U, 18275U, 19046U, 19836U, 20646U,
	21475U, 22324U, 23192U, 24081U, 24989U, 25918U, 26867U, 27837U,
	28828U, 29839U, 30872U, 31925U, 33000U, 34096U, 35214U, 36354U,
	37515U, 38698U, 39904U, 41132U, 42382U, 43654U, 44950U, 46268U,
	47609U, 48973U, 50360U, 51771U, 53205U, 54662U, 56144U, 57649U,
	59178U, 60731U, 62308U, 63910U, 65536U,
};

static void fetch(const struct audiod_stream *stream, uint64_t position, int64_t sample[2]);
static void store(struct audiod_stream *stream, uint64_t position, const int64_t sample[2]);
static int32_t clamp32(int64_t value);
static void audiod_played_store(struct audiod_shm_header *shm, uint64_t position, int64_t time_ns);
static void device_to_stereo(const struct audiod_device *device, const uint8_t *in, uint32_t frames, int32_t *out);
static void feedback_mix(struct audiod_device *device, int64_t *mix, uint32_t frames);
static void soft_volume(const struct audiod_device *device, int64_t *mix, uint32_t frames);
static uint32_t soft_gain(uint32_t percent);
static double series_cos(double x);
static double series_sin(double x);

/* Reports the bytes of one frame. */
uint32_t
audiod_frame_bytes(
	uint32_t format,
	uint32_t channels)
{
	if (format == AUDIOD_FORMAT_S16_LE)
		return 2U * channels;
	return 4U * channels;
}

/* Sets how far a stream moves through its source per frame it produces. */
void
audiod_stream_rates(
	struct audiod_stream *stream,
	const struct audiod_device *device)
{
	uint64_t from;
	uint64_t to;

	/* Playback reads the stream at its rate for the device; capture the reverse. */
	from = stream->shm->rate;
	to = device->rate;
	if (stream->direction == AUDIOD_CAPTURE) {
		from = device->rate;
		to = stream->shm->rate;
	}
	stream->step = (from << 32) / to;

	/* Capture starts at the first real device frame; index 0 is the carry. */
	stream->phase = stream->direction == AUDIOD_CAPTURE ? (uint64_t)1 << 32 : 0;
	stream->carry[0] = 0;
	stream->carry[1] = 0;
}

/*
 * Makes the feedback sound for the device's rate (ws100-p002): FEEDBACK_MS
 * of a FEEDBACK_HZ sine that rises over FEEDBACK_RISE_MS and falls away to
 * nothing, the same on both channels, at FEEDBACK_PEAK.  The sine comes
 * from a two-term recurrence (no libm).  Returns 0, or -1 without memory
 * (then no feedback sound plays).
 */
int
audiod_feedback_make(
	struct audiod_device *device)
{
	int32_t *frames;
	double coefficient;
	double previous;
	double current;
	double next;
	double envelope;
	double left;
	uint32_t length;
	uint32_t rise;
	uint32_t index;

	/* Its length and rise in frames. */
	length = device->rate * FEEDBACK_MS / 1000U;
	rise = device->rate * FEEDBACK_RISE_MS / 1000U;
	if (length == 0U || rise == 0U || rise >= length)
		return -1;

	/* Room for it as stereo frames. */
	frames = malloc((size_t)length * 2U * sizeof(*frames));
	if (frames == NULL)
		return -1;

	/* The recurrence sin((n + 1) w) = 2 cos(w) sin(n w) - sin((n - 1) w), from sin(0) and sin(w). */
	coefficient = 2.0 * series_cos(2.0 * 3.14159265358979323846 * FEEDBACK_HZ / (double)device->rate);
	previous = 0.0;
	current = series_sin(2.0 * 3.14159265358979323846 * FEEDBACK_HZ / (double)device->rate);

	/* Each frame: the sine under its envelope, a straight rise and then a square fall to nothing. */
	for (index = 0U; index < length; index++) {
		if (index < rise) {
			envelope = (double)index / (double)rise;
		} else {
			left = 1.0 - (double)(index - rise) / (double)(length - rise);
			envelope = left * left;
		}

		/* The sample on both channels, and the next sine. */
		frames[index * 2U] = (int32_t)(previous * envelope * FEEDBACK_PEAK);
		frames[index * 2U + 1U] = frames[index * 2U];
		next = coefficient * current - previous;
		previous = current;
		current = next;
	}

	/* Succeeded: the sound is kept, not playing until asked for. */
	device->feedback = frames;
	device->feedback_length = length;
	device->feedback_next = length;
	return 0;
}

/*
 * Mixes one device period of every running playback stream into out, in
 * the device's format.  A stream that has less than the period plays what
 * it has and silence after it; a stream whose shared memory faults is
 * marked broken and left out.
 */
void
audiod_mix_period(
	struct audiod_device *device,
	uint8_t *out)
{
	struct audiod_client *client;
	struct audiod_stream *stream;
	struct audiod_shm_header *shm;
	int64_t *mix;
	int64_t a[2];
	int64_t b[2];
	int64_t value;
	uint64_t write_position;
	uint64_t read_position;
	uint64_t available;
	uint64_t index;
	uint64_t fraction;
	uint64_t consumed;
	uint32_t frames;
	uint32_t i;
	uint32_t channel;
	uint32_t volume[2];
	int16_t sample16;
	int32_t sample32;
	int missing;

	/* The period, started from silence. */
	frames = device->period_frames;
	mix = device->mix;
	memset(mix, 0, (size_t)frames * 2U * sizeof(*mix));

	/* Every running playback stream of every client. */
	for (client = audiod_clients; client != NULL; client = client->next) {
		for (stream = client->streams; stream != NULL; stream = stream->next) {
			if (stream->direction != AUDIOD_PLAYBACK ||
			    !stream->running || stream->broken)
				continue;
			shm = stream->shm;

			/*
			 * A fault in memory the client shrank ends this stream only.
			 * sigsetjmp stays in the condition: the C standard allows it
			 * only there or as a statement of its own, not in an
			 * assignment (an exception to the call-in-condition rule).
			 */
			if (sigsetjmp(audiod_bus_jump, 1) != 0) {
				audiod_bus_armed = 0;
				stream->broken = 1;
				continue;
			}

			/* From here a fault jumps back above. */
			audiod_bus_armed = 1;

			/* The client's position is checked, not trusted. */
			write_position = audiod_position_load(&shm->write_position,
			    &shm->write_sequence);
			read_position = shm->read_position;
			available = write_position - read_position;
			if (write_position < read_position ||
			    available > shm->capacity_frames) {
				audiod_bus_armed = 0;
				stream->broken = 1;
				continue;
			}

			/* The stream's volume; muted is silence. */
			volume[0] = stream->volume_left;
			volume[1] = stream->volume_right;
			if (stream->muted) {
				volume[0] = 0U;
				volume[1] = 0U;
			}

			/* Each frame of the period, while the stream has one (interpolated between two when the rates differ). */
			missing = 0;
			for (i = 0; i < frames; i++) {
				index = stream->phase >> 32;
				fraction = stream->phase & 0xffffffffULL;
				if (index + (fraction != 0) >= available) {
					missing = 1;
					break;
				}

				/* The frame, and the next one's share of it. */
				fetch(stream, read_position + index, a);
				if (fraction != 0) {
					fetch(stream, read_position + index + 1, b);
					for (channel = 0; channel < 2; channel++) {
						a[channel] += ((b[channel] - a[channel]) *
						    (int64_t)(fraction >> 16)) >> 16;
					}
				}

				/* Added to the mix at the stream's volume. */
				for (channel = 0; channel < 2; channel++) {
					value = (a[channel] * (int64_t)volume[channel]) >> 16;
					mix[i * 2 + channel] += value;
				}

				/* On through the stream. */
				stream->phase += stream->step;
			}

			/* The stream's memory is no longer touched. */
			audiod_bus_armed = 0;

			/* Gives the consumed frames back to the client. */
			consumed = stream->phase >> 32;
			if (consumed > available)
				consumed = available;

			/*
			 * Draining, a last frame that waits for a next one to
			 * interpolate towards never gets it; it is dropped.
			 */
			if (missing && stream->draining)
				consumed = available;
			stream->phase -= consumed << 32;
			read_position += consumed;
			audiod_position_store(&shm->read_position, &shm->read_sequence,
			    read_position);
			audiod_played_store(shm, read_position, audiod_now_ns());

			/* A stream that ran dry while playing has underrun; a drain expects to. */
			if (missing && !stream->draining && read_position == write_position) {
				shm->underruns++;
				if (shm->underruns != stream->underruns_reported) {
					stream->underruns_reported = shm->underruns;
					audiod_send_event(stream, AUDIOD_UNDERRUN, shm->underruns);
				}
			}

			/* A drain completes once the device has played what is mixed now. */
			if (stream->draining && stream->drain_target == 0 &&
			    read_position == write_position)
				stream->drain_target = device->written +
				    (uint64_t)frames * device->frame_bytes +
				    device->lead_bytes + 2U * device->fragment_bytes;

			/* Asks for more once a period's room has opened since the last ask. */
			if (!stream->draining &&
			    shm->capacity_frames - (write_position - read_position) >=
			    shm->period_frames &&
			    read_position - stream->last_request >= shm->period_frames) {
				stream->last_request = read_position;
				audiod_send_event(stream, AUDIOD_REQUEST, 0);
			}
		}
	}

	/* The feedback sound over the streams, then the volume audiod applies itself (ws100-p002). */
	feedback_mix(device, mix, frames);
	soft_volume(device, mix, frames);

	/* Writes the mix in the device's format, saturating. */
	for (i = 0; i < frames * 2U; i++) {
		value = mix[i];
		if (device->format == AUDIOD_FORMAT_S16_LE) {
			/* S16: the top 16 bits, saturated. */
			value >>= 16;
			if (value > 32767)
				value = 32767;
			if (value < -32768)
				value = -32768;
			sample16 = (int16_t)value;
			memcpy(out + i * 2U, &sample16, 2);
		} else {
			/* S32: saturated. */
			sample32 = clamp32(value);
			memcpy(out + i * 4U, &sample32, 4);
		}
	}
}

/*
 * Hands one captured device period to every running capture stream, in
 * its format and at its rate.  A stream whose reader has fallen a whole
 * ring behind loses the new frames and counts an overrun.
 */
void
audiod_capture_period(
	struct audiod_device *device,
	const uint8_t *in)
{
	struct audiod_client *client;
	struct audiod_stream *stream;
	struct audiod_shm_header *shm;
	const int32_t *frames;
	int64_t a[2];
	int64_t b[2];
	uint64_t write_position;
	uint64_t read_position;
	uint64_t index;
	uint64_t fraction;
	uint32_t count;
	uint32_t channel;
	int overran;

	count = device->period_frames;
	device_to_stereo(device, in, count, device->capture_frames);
	frames = device->capture_frames;

	for (client = audiod_clients; client != NULL; client = client->next) {
		for (stream = client->streams; stream != NULL; stream = stream->next) {
			if (stream->direction != AUDIOD_CAPTURE ||
			    !stream->running || stream->broken)
				continue;
			shm = stream->shm;
			if (sigsetjmp(audiod_bus_jump, 1) != 0) {
				audiod_bus_armed = 0;
				stream->broken = 1;
				continue;
			}
			audiod_bus_armed = 1;

			write_position = shm->write_position;
			read_position = audiod_position_load(&shm->read_position,
			    &shm->read_sequence);
			if (read_position > write_position ||
			    write_position - read_position > shm->capacity_frames) {
				audiod_bus_armed = 0;
				stream->broken = 1;
				continue;
			}

			/* Source frame 0 is the previous period's last frame. */
			overran = 0;
			for (;;) {
				index = stream->phase >> 32;
				fraction = stream->phase & 0xffffffffULL;
				if (index + (fraction != 0) > count)
					break;
				for (channel = 0; channel < 2; channel++) {
					a[channel] = index == 0 ? stream->carry[channel] :
					    frames[(index - 1) * 2 + channel];
					if (fraction != 0) {
						b[channel] = frames[index * 2 + channel];
						a[channel] += ((b[channel] - a[channel]) *
						    (int64_t)(fraction >> 16)) >> 16;
					}
				}
				if (write_position - read_position < shm->capacity_frames) {
					store(stream, write_position, a);
					write_position++;
				} else {
					overran = 1;
				}
				stream->phase += stream->step;
			}
			stream->phase -= (uint64_t)count << 32;
			stream->carry[0] = frames[(count - 1) * 2];
			stream->carry[1] = frames[(count - 1) * 2 + 1];
			audiod_position_store(&shm->write_position, &shm->write_sequence,
			    write_position);
			audiod_bus_armed = 0;

			if (overran) {
				shm->overruns++;
				if (shm->overruns != stream->overruns_reported) {
					stream->overruns_reported = shm->overruns;
					audiod_send_event(stream, AUDIOD_OVERRUN, shm->overruns);
				}
			}

			/* Tells the reader once a period has arrived since the last word. */
			if (write_position - stream->last_request >= shm->period_frames) {
				stream->last_request = write_position;
				audiod_send_event(stream, AUDIOD_REQUEST, 0);
			}
		}
	}
}

/*
 * Publishes where playback has got to and when, as one pair: a reader
 * retries while the sequence is odd or changed.
 */
static void
audiod_played_store(
	struct audiod_shm_header *shm,
	uint64_t position,
	int64_t time_ns)
{
	uint32_t current;

	current = shm->played_sequence;
	__atomic_store_n(&shm->played_sequence, current + 1U, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	shm->played_position = position;
	shm->played_time_ns = time_ns;
	__atomic_store_n(&shm->played_sequence, current + 2U, __ATOMIC_RELEASE);
}

/* Reads one frame of a stream as two 32-bit samples. */
static void
fetch(
	const struct audiod_stream *stream,
	uint64_t position,
	int64_t sample[2])
{
	const struct audiod_shm_header *shm;
	const uint8_t *frame;
	uint32_t channel;
	int16_t s16;
	int32_t s32;
	float f32;

	shm = stream->shm;
	frame = stream->ring + (size_t)(position % shm->capacity_frames) * shm->frame_bytes;
	for (channel = 0; channel < shm->channels && channel < 2U; channel++) {
		if (shm->format == AUDIOD_FORMAT_S16_LE) {
			memcpy(&s16, frame + channel * 2U, 2);
			sample[channel] = (int64_t)s16 * 65536;
		} else if (shm->format == AUDIOD_FORMAT_S32_LE) {
			memcpy(&s32, frame + channel * 4U, 4);
			sample[channel] = s32;
		} else {
			memcpy(&f32, frame + channel * 4U, 4);
			sample[channel] = clamp32((int64_t)(f32 * 2147483648.0f));
		}
	}

	/* A mono stream plays on both channels. */
	if (shm->channels == 1U)
		sample[1] = sample[0];
}

/* Writes one frame of two 32-bit samples in the stream's format. */
static void
store(
	struct audiod_stream *stream,
	uint64_t position,
	const int64_t sample[2])
{
	struct audiod_shm_header *shm;
	uint8_t *frame;
	int64_t value[2];
	uint32_t channel;
	int16_t s16;
	int32_t s32;
	float f32;

	shm = stream->shm;
	frame = stream->ring + (size_t)(position % shm->capacity_frames) * shm->frame_bytes;
	value[0] = sample[0];
	value[1] = sample[1];

	/* A mono stream records the average of the two channels. */
	if (shm->channels == 1U)
		value[0] = (sample[0] + sample[1]) / 2;
	for (channel = 0; channel < shm->channels && channel < 2U; channel++) {
		if (shm->format == AUDIOD_FORMAT_S16_LE) {
			s16 = (int16_t)(clamp32(value[channel]) >> 16);
			memcpy(frame + channel * 2U, &s16, 2);
		} else if (shm->format == AUDIOD_FORMAT_S32_LE) {
			s32 = clamp32(value[channel]);
			memcpy(frame + channel * 4U, &s32, 4);
		} else {
			f32 = (float)clamp32(value[channel]) / 2147483648.0f;
			memcpy(frame + channel * 4U, &f32, 4);
		}
	}
}

/* Saturates to 32 bits. */
static int32_t
clamp32(
	int64_t value)
{
	if (value > INT32_MAX)
		return INT32_MAX;
	if (value < INT32_MIN)
		return INT32_MIN;
	return (int32_t)value;
}

/* Converts device frames to stereo 32-bit samples. */
static void
device_to_stereo(
	const struct audiod_device *device,
	const uint8_t *in,
	uint32_t frames,
	int32_t *out)
{
	uint32_t i;
	uint32_t channel;
	int16_t s16;
	int32_t s32;

	for (i = 0; i < frames; i++) {
		for (channel = 0; channel < 2U; channel++) {
			uint32_t source = channel < device->channels ? channel : 0U;

			if (device->format == AUDIOD_FORMAT_S16_LE) {
				memcpy(&s16, in + (i * device->channels + source) * 2U, 2);
				out[i * 2 + channel] = (int32_t)s16 * 65536;
			} else {
				memcpy(&s32, in + (i * device->channels + source) * 4U, 4);
				out[i * 2 + channel] = s32;
			}
		}
	}
}

/* Adds the playing part of the feedback sound to one period of the mix, and moves on. */
static void
feedback_mix(
	struct audiod_device *device,
	int64_t *mix,
	uint32_t frames)
{
	uint32_t index;

	/* Only while it plays. */
	if (device->feedback == NULL)
		return;

	/* The period that starts it (its device byte is noted once the period is placed, device.c). */
	if (device->feedback_step == 1U && device->feedback_next == 0U)
		device->feedback_step = 2U;

	/* Each frame of the period, while the sound lasts. */
	for (index = 0U; index < frames && device->feedback_next < device->feedback_length; index++) {
		mix[index * 2U] += device->feedback[device->feedback_next * 2U];
		mix[index * 2U + 1U] += device->feedback[device->feedback_next * 2U + 1U];
		device->feedback_next++;
	}
}

/* Applies the device volume audiod keeps itself (when the device has none) to one period of the mix. */
static void
soft_volume(
	const struct audiod_device *device,
	int64_t *mix,
	uint32_t frames)
{
	uint32_t gain[2];
	uint32_t index;

	/* Only when the device does not take the volume. */
	if (!device->soft)
		return;

	/* Each channel's factor; muted is silence. */
	gain[0] = soft_gain(device->soft_left);
	gain[1] = soft_gain(device->soft_right);
	if (device->soft_muted) {
		gain[0] = 0U;
		gain[1] = 0U;
	}

	/* Every sample of the period. */
	for (index = 0U; index < frames; index++) {
		mix[index * 2U] = (mix[index * 2U] * (int64_t)gain[0]) >> 16;
		mix[index * 2U + 1U] = (mix[index * 2U + 1U] * (int64_t)gain[1]) >> 16;
	}
}

/* Gives the factor (in 1/65536) of a volume in percent from the curve's table, full from 100 up. */
static uint32_t
soft_gain(
	uint32_t percent)
{
	/* Full at 100 and above. */
	if (percent >= 100U)
		return SOFT_UNITY;

	/* Succeeded: the curve's factor, 0 at 0. */
	return volume_factors[percent];
}

/* Gives cos(x) for a small x (under 1) by its series, to x^14. */
static double
series_cos(
	double x)
{
	double term;
	double sum;
	unsigned n;

	/* Each term from the one before: -x^2 / ((2n - 1) 2n). */
	term = 1.0;
	sum = 1.0;
	for (n = 1U; n <= 7U; n++) {
		term = -term * x * x / (double)((2U * n - 1U) * 2U * n);
		sum += term;
	}

	/* Succeeded: the sum. */
	return sum;
}

/* Gives sin(x) for a small x (under 1) by its series, to x^15. */
static double
series_sin(
	double x)
{
	double term;
	double sum;
	unsigned n;

	/* Each term from the one before: -x^2 / (2n (2n + 1)). */
	term = x;
	sum = x;
	for (n = 1U; n <= 7U; n++) {
		term = -term * x * x / (double)(2U * n * (2U * n + 1U));
		sum += term;
	}

	/* Succeeded: the sum. */
	return sum;
}
