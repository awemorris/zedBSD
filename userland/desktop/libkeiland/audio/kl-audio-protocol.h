/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The wire of Keiland's audio streams (WS191, plan/ws191/design.md): the
 * opcodes of kl_audio_v1 and kl_audio_stream_v1, the values their events
 * carry, and the layout of the ring a stream's sound is written into.  The
 * compositor serves them and libkeiland speaks them; both include this
 * header and neither the other's code.
 *
 * kl_audio_v1 (a global, version 1; shown to the clients of the
 * compositor's own user, as kl_system_manager_v1 is)
 *   request 0 destroy                       the streams made from it stay
 *   request 1 create_stream(new_id kl_audio_stream_v1, uint format, uint channels, uint rate,
 *                           uint buffer_frames, uint period_frames)
 *
 * kl_audio_stream_v1
 *   request 0 destroy
 *   request 1 start(uint request)           the device reads the ring from now
 *   request 2 stop(uint request)            a pause: what is written and not read stays in the ring
 *   request 3 flush(uint request)           drops what is written and not read; a running stream
 *                                           stays running, a stopped one stopped, a drain is ended
 *   request 4 drain(uint request)           plays what is written, then stops
 *   event   0 ready(fd ring, uint bytes, uint capacity_frames, uint period_frames)
 *                                           once, after the create
 *   event   1 failed(uint error)            once, instead of ready
 *   event   2 result(uint request, uint error)
 *                                           exactly one for each start, stop, flush and drain
 *   event   3 drained(uint request)         that drain is complete; the stream is stopped
 *   event   4 underrun(uint count)          the device found the ring empty while running (at
 *                                           most once after each start or other control; count is
 *                                           the total so far, in the system's own unit)
 *   event   5 lost(uint error)              the stream is gone; no event follows
 *
 * The sound itself never passes the compositor: the client writes frames
 * into the ring (the memory ready hands over) and advances the write
 * position; the server side reads them and advances the read position.
 */

#ifndef KEILAND_KL_AUDIO_PROTOCOL_H
#define KEILAND_KL_AUDIO_PROTOCOL_H

/* The interfaces' names and versions. */
#define KL_AUDIO_NAME				"kl_audio_v1"
#define KL_AUDIO_VERSION			1U
#define KL_AUDIO_STREAM_NAME			"kl_audio_stream_v1"

/* kl_audio_v1's requests. */
#define KL_AUDIO_DESTROY			0U
#define KL_AUDIO_CREATE_STREAM			1U

/* kl_audio_stream_v1's requests and events. */
#define KL_AUDIO_STREAM_DESTROY			0U
#define KL_AUDIO_STREAM_START			1U
#define KL_AUDIO_STREAM_STOP			2U
#define KL_AUDIO_STREAM_FLUSH			3U
#define KL_AUDIO_STREAM_DRAIN			4U
#define KL_AUDIO_STREAM_EVENT_READY		0U
#define KL_AUDIO_STREAM_EVENT_FAILED		1U
#define KL_AUDIO_STREAM_EVENT_RESULT		2U
#define KL_AUDIO_STREAM_EVENT_DRAINED		3U
#define KL_AUDIO_STREAM_EVENT_UNDERRUN		4U
#define KL_AUDIO_STREAM_EVENT_LOST		5U

/* The errors failed, result and lost carry (the same on every system; libkeiland turns them into errno values). */
#define KL_AUDIO_ERROR_NONE			0U
#define KL_AUDIO_ERROR_INVALID			1U
#define KL_AUDIO_ERROR_NO_DEVICE		2U
#define KL_AUDIO_ERROR_UNAVAILABLE		3U
#define KL_AUDIO_ERROR_UNSUPPORTED		4U
#define KL_AUDIO_ERROR_NO_MEMORY		5U
#define KL_AUDIO_ERROR_TOO_MANY			6U
#define KL_AUDIO_ERROR_STATE			7U
#define KL_AUDIO_ERROR_GONE			8U
#define KL_AUDIO_ERROR_BROKEN			9U
#define KL_AUDIO_ERROR_FAILED			10U

/* The sample formats (a frame is one sample of each channel, little-endian). */
#define KL_AUDIO_WIRE_FORMAT_S16_LE		1U
#define KL_AUDIO_WIRE_FORMAT_S32_LE		2U
#define KL_AUDIO_WIRE_FORMAT_F32_LE		3U

/* What a stream may ask for. */
#define KL_AUDIO_CHANNELS_MAX			2U
#define KL_AUDIO_RATE_MIN			8000U
#define KL_AUDIO_RATE_MAX			192000U
#define KL_AUDIO_RING_BYTES_MAX			(1U << 20)

/* How many streams one process, and the compositor as a whole, may hold (failed and lost ones not counted). */
#define KL_AUDIO_STREAMS_PER_PROCESS		8U
#define KL_AUDIO_STREAMS_MAX			32U

/*
 * The ring: a page of positions, then the frames.  Each offset is in
 * bytes from the start of the memory.  A position is a count of frames
 * that only grows; it is read with an 8-byte acquire load and written
 * with an 8-byte release store by its one writer.  The played position and
 * its time are one pair under the played sequence, a sequence lock: odd
 * while the server writes them.
 */
#define KL_AUDIO_RING_HEADER			4096U
#define KL_AUDIO_RING_VERSION_VALUE		1U
#define KL_AUDIO_RING_TAG			0U	/* the server's own, not looked at */
#define KL_AUDIO_RING_VERSION			4U
#define KL_AUDIO_RING_FORMAT			8U
#define KL_AUDIO_RING_CHANNELS			12U
#define KL_AUDIO_RING_RATE			16U
#define KL_AUDIO_RING_FRAME_BYTES		20U
#define KL_AUDIO_RING_CAPACITY			24U
#define KL_AUDIO_RING_PERIOD			28U
#define KL_AUDIO_RING_WRITE_POSITION		64U	/* the client's */
#define KL_AUDIO_RING_WRITE_SEQUENCE		72U	/* not used (audiod's, without an 8-byte atomic) */
#define KL_AUDIO_RING_READ_POSITION		128U	/* the server's: taken from the ring */
#define KL_AUDIO_RING_READ_SEQUENCE		136U	/* not used */
#define KL_AUDIO_RING_PLAYED_POSITION		192U	/* the server's: heard, as it reckons */
#define KL_AUDIO_RING_PLAYED_TIME		200U	/* the server's: CLOCK_MONOTONIC (ns) of that reckoning */
#define KL_AUDIO_RING_PLAYED_SEQUENCE		208U	/* the server's: odd while the played pair is written */
#define KL_AUDIO_RING_UNDERRUNS			212U
#define KL_AUDIO_RING_OVERRUNS			216U
#define KL_AUDIO_RING_STATE			220U

#endif
