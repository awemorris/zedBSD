/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sound's playback streams, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_AUDIO_H
#define KEILAND_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The sound's playback streams (KL_VERSION 73).
 *
 * A stream plays frames a program writes: it is opened with a format, and
 * the frames go into a ring of shared memory the desktop gives it, so the
 * sound never passes the compositor.  Each stream has its own connection
 * to the compositor (from WAYLAND_DISPLAY), so it needs no window and may
 * be used from any thread: one thread writes (kl_audio_stream_write), any
 * thread may read the positions, and the controls and
 * kl_audio_stream_dispatch take the stream's lock.  A stream whose sound
 * service went (its lost event, or its connection's end) keeps working as
 * a silent sink: the frames written are taken at the stream's rate while
 * it runs, so a player's clock and its writing go on; its controls then
 * answer EPIPE (and still take effect on the sink).
 *
 * Adding a field to struct kl_audio_format changes the interface (a new
 * KL_VERSION and a call that takes the larger struct).
 */
#define KL_AUDIO_FORMAT_S16_LE	1U
#define KL_AUDIO_FORMAT_S32_LE	2U
#define KL_AUDIO_FORMAT_F32_LE	3U

/* What kl_audio_stream_dispatch found. */
#define KL_AUDIO_EVENT_DRAINED	0x1U	/* a drain asked for is complete; the stream is stopped */
#define KL_AUDIO_EVENT_UNDERRUN	0x2U	/* the device found the ring empty while running */
#define KL_AUDIO_EVENT_LOST	0x4U	/* the sound service went: the stream is a silent sink from now */

/* What a stream plays: interleaved little-endian frames. */
struct kl_audio_format {
	unsigned format;	/* KL_AUDIO_FORMAT_* */
	unsigned channels;	/* 1 or 2 */
	unsigned rate;		/* frames a second, 8000 to 192000 */
	unsigned buffer_frames;	/* the ring, 0 for the desktop's choice */
	unsigned period_frames;	/* 0 for the desktop's choice */
};

struct kl_audio_stream;

/*
 * Opens a stream, stopped, and waits (at most two seconds) until the
 * desktop made it.  Returns 0, or ENOTSUP (no Keiland, no sound streams,
 * WAYLAND_SOCKET set or WAYLAND_DISPLAY unset), ENODEV (no sound device),
 * EAGAIN (the sound service is not there now), EINVAL, EMFILE (too many
 * streams), ENOMEM, EPROTO (a ring not as asked for), EPIPE (the
 * compositor went) or ETIMEDOUT.
 */
int kl_audio_stream_open(const struct kl_audio_format *format, struct kl_audio_stream **stream);

/*
 * Closes a stream.  No other thread may use it any more.
 */
void kl_audio_stream_close(struct kl_audio_stream *stream);

/*
 * The controls, each waiting (at most two seconds) for the desktop's
 * answer: start (the device reads the ring from now), stop (a pause; what
 * is written stays), flush (drops what is written and not read; a running
 * stream keeps running, a drain ends) and drain (plays what is written,
 * then stops; KL_AUDIO_EVENT_DRAINED tells the end).  Each returns 0,
 * EPIPE (the stream is lost: the sink took the control), EBUSY, EAGAIN,
 * EIO or ETIMEDOUT.
 */
int kl_audio_stream_start(struct kl_audio_stream *stream);
int kl_audio_stream_stop(struct kl_audio_stream *stream);
int kl_audio_stream_flush(struct kl_audio_stream *stream);
int kl_audio_stream_drain(struct kl_audio_stream *stream);

/*
 * Writes up to count frames into the ring, as many as there is room for,
 * and returns how many were written (one writing thread only).
 */
size_t kl_audio_stream_write(struct kl_audio_stream *stream, const void *frames, size_t count);

/*
 * The positions, in frames since the stream was opened: written by the
 * program, consumed (taken from the ring by the device side; what is
 * written and not consumed is what the ring holds), and the position heard
 * (the clock of a player; time_ns, when not NULL, has the CLOCK_MONOTONIC
 * time it was reckoned at).  Any thread may read them.
 */
uint64_t kl_audio_stream_written(const struct kl_audio_stream *stream);
uint64_t kl_audio_stream_consumed(const struct kl_audio_stream *stream);
uint64_t kl_audio_stream_position(const struct kl_audio_stream *stream, int64_t *time_ns);

/*
 * The ring's frames.
 */
unsigned kl_audio_stream_capacity(const struct kl_audio_stream *stream);

/*
 * Takes what the desktop told without waiting, and gives the
 * KL_AUDIO_EVENT_* bits that came since the last call.  Returns 0 (also
 * when another thread holds the stream: nothing is taken then), or EPIPE
 * once the stream is lost.
 */
int kl_audio_stream_dispatch(struct kl_audio_stream *stream, unsigned *events);

#ifdef __cplusplus
}
#endif

#endif
