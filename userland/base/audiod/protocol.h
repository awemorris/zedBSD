/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The audiod protocol (plan/ws035/audiod-design.md).
 *
 * A client speaks to /run/audiod.sock with length-prefixed messages.  Sound
 * never crosses the socket: each stream has a ring in shared memory that
 * audiod creates, unlinks at once and hands over with SCM_RIGHTS together
 * with AUDIOD_STREAM_CREATED.  Playback: the client writes and advances
 * write_position, audiod reads and advances read_position.  Capture: the
 * other way round.  Positions count frames and only grow.
 */

#ifndef AUDIOD_PROTOCOL_H
#define AUDIOD_PROTOCOL_H

#include <sched.h>
#include <stdint.h>

#define AUDIOD_SOCKET_PATH	"/run/audiod.sock"

/*
 * The protocol's version and the shared memory ring's, apart (ws209-p002,
 * design 1.1 P1').  A client says its protocol version in HELLO; audiod
 * answers the lower of its own and the client's in WELCOME, and gives a
 * version 1 client none of version 2's events and refuses it version 2's
 * requests.  The ring's layout has not changed and keeps version 1.
 */
#define AUDIOD_PROTOCOL_VERSION_1	1U
#define AUDIOD_PROTOCOL_VERSION_2	2U
#define AUDIOD_PROTOCOL_VERSION		AUDIOD_PROTOCOL_VERSION_2
#define AUDIOD_SHM_VERSION		1U

/* Sample encodings; 1 and 2 are the kernel's numbers. */
#define AUDIOD_FORMAT_S16_LE	1U
#define AUDIOD_FORMAT_S32_LE	2U
#define AUDIOD_FORMAT_F32_LE	3U

#define AUDIOD_PLAYBACK		0U
#define AUDIOD_CAPTURE		1U

/* Stream volume: 65536 is unity. */
#define AUDIOD_VOLUME_UNITY	65536U

/* Requests. */
#define AUDIOD_HELLO		1U
#define AUDIOD_STREAM_CREATE	2U
#define AUDIOD_STREAM_START	3U
#define AUDIOD_STREAM_STOP	4U
#define AUDIOD_STREAM_DRAIN	5U
#define AUDIOD_STREAM_FLUSH	6U
#define AUDIOD_STREAM_VOLUME	7U
#define AUDIOD_STREAM_DESTROY	8U
#define AUDIOD_DEVICE_VOLUME	9U
#define AUDIOD_SUBSCRIBE	10U
/*
 * Plays audiod's short feedback sound (about 100 ms) once at the device
 * volume, as a volume control does when the volume changes (ws100-p002).
 * The header only; a request while the sound plays starts it again, so
 * quick changes never overlap.  An audiod without it answers ERROR EINVAL.
 */
#define AUDIOD_FEEDBACK		11U

/*
 * Version 2 (ws209-p002): the sound's endpoints and routes
 * (plan/ws209/phase001/phase.md).  A daemon (bluetoothd, a modem's)
 * registers its endpoints, each with a ring in shared memory as a stream
 * has; the seat's compositor chooses the routes; the daemon that holds a
 * call's far side sets the call.
 */
#define AUDIOD_ENDPOINT_ADD		12U
#define AUDIOD_ENDPOINT_REMOVE		13U
#define AUDIOD_ENDPOINT_STATE		14U
#define AUDIOD_ENDPOINT_LIST		15U
#define AUDIOD_ROUTE_SET		16U
#define AUDIOD_ROUTE_LIST		17U
#define AUDIOD_CALL_SET			18U
#define AUDIOD_CALL_CLEAR		19U
#define AUDIOD_ENDPOINT_FAILED		20U
#define AUDIOD_CALL_STATE		21U
#define AUDIOD_ENDPOINT_VOLUME_REPORT	22U
#define AUDIOD_MEDIA_RELEASE		23U

/* Replies and events. */
#define AUDIOD_WELCOME		64U
#define AUDIOD_STREAM_CREATED	65U
#define AUDIOD_DONE		66U
#define AUDIOD_ERROR		67U
#define AUDIOD_REQUEST		68U
#define AUDIOD_DRAINED		69U
#define AUDIOD_UNDERRUN		70U
#define AUDIOD_OVERRUN		71U
#define AUDIOD_VOLUME_CHANGED	72U
#define AUDIOD_ENDPOINT_ADDED	73U
#define AUDIOD_ENDPOINT_INFO	74U
#define AUDIOD_ROUTE_INFO	75U
#define AUDIOD_ENDPOINT_WANTED	76U
#define AUDIOD_MEDIA_HELD	77U
#define AUDIOD_ENDPOINT_REMOVED	78U
#define AUDIOD_ENDPOINT_VOLUME	79U

/* SUBSCRIBE's bits: the device volume (every version), the endpoints and the routes (version 2). */
#define AUDIOD_SUBSCRIBE_VOLUME		0x1U
#define AUDIOD_SUBSCRIBE_ENDPOINTS	0x2U
#define AUDIOD_SUBSCRIBE_ROUTES		0x4U

/* An endpoint's kind: audiod writes a sink and reads a source. */
#define AUDIOD_KIND_SINK		0U
#define AUDIOD_KIND_SOURCE		1U

/* An endpoint's class. */
#define AUDIOD_CLASS_SPEAKER		1U
#define AUDIOD_CLASS_MIC		2U
#define AUDIOD_CLASS_HEADSET_MEDIA	3U
#define AUDIOD_CLASS_HEADSET_VOICE	4U
#define AUDIOD_CLASS_PHONE_CALL		5U
#define AUDIOD_CLASS_MODEM_CALL		6U

/* An endpoint's state: it streams, it paused for a while, or it opens when wanted. */
#define AUDIOD_ENDPOINT_ACTIVE		1U
#define AUDIOD_ENDPOINT_SUSPENDED	2U
#define AUDIOD_ENDPOINT_DORMANT		3U

/* ENDPOINT_ADD's flags: the daemon takes the endpoint's volume (ENDPOINT_VOLUME). */
#define AUDIOD_ENDPOINT_FLAG_VOLUME	0x1U

/* The routes, and an endpoint id of 0 for a route's default. */
#define AUDIOD_ROUTE_MEDIA		1U
#define AUDIOD_ROUTE_CALL		2U
#define AUDIOD_ENDPOINT_DEFAULT		0U

/* The internal endpoints' ids (kernel's /dev/dsp0); a daemon's start at 16 and are never given twice. */
#define AUDIOD_ENDPOINT_DEVICE_OUT	1U
#define AUDIOD_ENDPOINT_DEVICE_IN	2U
#define AUDIOD_ENDPOINT_FIRST		16U

/* Why a route or an endpoint changed. */
#define AUDIOD_REASON_NONE		0U
#define AUDIOD_REASON_TIMEOUT		1U
#define AUDIOD_REASON_FAILED		2U
#define AUDIOD_REASON_OUTPUT_LOST	3U
#define AUDIOD_REASON_REMOVED		4U
#define AUDIOD_REASON_BROKEN		5U
#define AUDIOD_REASON_CALL		6U

/* A stream's role (version 2's STREAM_CREATE): media, or an alert (notifications, ringing) that a call does not stop. */
#define AUDIOD_ROLE_MEDIA		0U
#define AUDIOD_ROLE_ALERT		1U

/* An endpoint's key and name, NUL-ended. */
#define AUDIOD_KEY_MAX			32U
#define AUDIOD_NAME_MAX			32U

/* Every message starts with this; length counts the whole message. */
struct audiod_header {
	uint32_t type;
	uint32_t length;
	uint32_t serial;
	uint32_t stream;
};

struct audiod_hello {
	struct audiod_header header;
	uint32_t version;
	uint32_t reserved;
};

struct audiod_welcome {
	struct audiod_header header;
	uint32_t version;
	uint32_t device;		/* 0 when there is no sound device */
	uint32_t format;
	uint32_t channels;
	uint32_t rate;
	uint32_t period_frames;
};

struct audiod_stream_create {
	struct audiod_header header;
	uint32_t direction;
	uint32_t format;
	uint32_t channels;
	uint32_t rate;
	uint32_t buffer_frames;
	uint32_t period_frames;
};

/* Version 2's STREAM_CREATE: the same with the stream's role (version 1's length means media). */
struct audiod_stream_create2 {
	struct audiod_header header;
	uint32_t direction;
	uint32_t format;
	uint32_t channels;
	uint32_t rate;
	uint32_t buffer_frames;
	uint32_t period_frames;
	uint32_t role;
	uint32_t reserved;
};

/*
 * ENDPOINT_ADD: what the endpoint is, its ring's size and the fill the
 * daemon wants it kept at (frames), its state and flags, its latency
 * beyond the ring (us), a key that stays across reconnections
 * ("bt:001122334455:a2dp") and a name to show.  Answered with
 * ENDPOINT_ADDED and the ring's memory (SCM_RIGHTS).
 */
struct audiod_endpoint_add {
	struct audiod_header header;
	uint32_t kind;
	uint32_t endpoint_class;
	uint32_t group;
	uint32_t format;
	uint32_t channels;
	uint32_t rate;
	uint32_t buffer_frames;
	uint32_t target_frames;
	uint32_t state;
	uint32_t flags;
	uint32_t latency_us;
	uint32_t reserved;
	char key[AUDIOD_KEY_MAX];
	char name[AUDIOD_NAME_MAX];
};

/* ENDPOINT_ADDED: the endpoint's id and its ring's memory. */
struct audiod_endpoint_added {
	struct audiod_header header;
	uint32_t id;
	uint32_t shm_bytes;
	uint32_t capacity_frames;
	uint32_t reserved;
};

/*
 * ENDPOINT_REMOVE, ENDPOINT_STATE (with the WANTED ticket it answers and
 * its latency now), ENDPOINT_FAILED (the error the daemon met opening it,
 * for that ticket): a daemon's word on its endpoint.
 */
struct audiod_endpoint_state {
	struct audiod_header header;
	uint32_t id;
	uint32_t state;
	uint32_t ticket;
	uint32_t latency_us;
};

/* ENDPOINT_INFO (events and ENDPOINT_LIST's answers): an endpoint as audiod knows it. */
struct audiod_endpoint_info {
	struct audiod_header header;
	uint32_t id;
	uint32_t kind;
	uint32_t endpoint_class;
	uint32_t group;
	uint32_t state;
	uint32_t rate;
	uint32_t channels;
	uint32_t flags;
	uint32_t latency_us;
	uint32_t volume_left;
	uint32_t volume_right;
	uint32_t muted;
	char key[AUDIOD_KEY_MAX];
	char name[AUDIOD_NAME_MAX];
};

/* ROUTE_SET: a route's output and input endpoints (0: the default; for CALL the user's side). */
struct audiod_route_set {
	struct audiod_header header;
	uint32_t route;
	uint32_t output;
	uint32_t input;
	uint32_t reserved;
};

/*
 * ROUTE_INFO (events and ROUTE_LIST's answers): a route, whether it runs,
 * why it last changed, whether its media is held (MEDIA), its call's far
 * group and whether the call's sound is here (CALL).
 */
struct audiod_route_info {
	struct audiod_header header;
	uint32_t route;
	uint32_t output;
	uint32_t input;
	uint32_t active;
	uint32_t reason;
	uint32_t held;
	uint32_t far_group;
	uint32_t audio_here;
};

/* CALL_SET (far_group), CALL_STATE (active, audio_here): a call's far side and its state. */
struct audiod_call {
	struct audiod_header header;
	uint32_t far_group;
	uint32_t active;
	uint32_t audio_here;
	uint32_t reserved;
};

/* ENDPOINT_WANTED: open (1) or close (0) an endpoint, the ticket its ACTIVE answers with. */
struct audiod_endpoint_wanted {
	struct audiod_header header;
	uint32_t id;
	uint32_t wanted;
	uint32_t ticket;
	uint32_t reserved;
};

/* MEDIA_HELD: the media streams are held (not read) or not, and why. */
struct audiod_media_held {
	struct audiod_header header;
	uint32_t held;
	uint32_t reason;
};

/* ENDPOINT_REMOVED (the endpoint audiod took away, and why). */
struct audiod_endpoint_removed {
	struct audiod_header header;
	uint32_t id;
	uint32_t reason;
};

/* ENDPOINT_VOLUME (audiod to a daemon) and ENDPOINT_VOLUME_REPORT (a daemon to audiod): 0..100 a channel. */
struct audiod_endpoint_volume {
	struct audiod_header header;
	uint32_t id;
	uint32_t left;
	uint32_t right;
	uint32_t muted;
};

/* Carries the shared memory descriptor with SCM_RIGHTS. */
struct audiod_stream_created {
	struct audiod_header header;
	uint32_t shm_bytes;
	uint32_t capacity_frames;
};

/* Stream volume (0..65536 per channel), or device volume (0..100). */
struct audiod_volume {
	struct audiod_header header;
	uint32_t left;
	uint32_t right;
	uint32_t muted;
	uint32_t reserved;
};

struct audiod_subscribe {
	struct audiod_header header;
	uint32_t mask;
	uint32_t reserved;
};

/* DONE and ERROR name the request by its serial. */
struct audiod_result {
	struct audiod_header header;
	uint32_t error;
	uint32_t reserved;
};

/* REQUEST, DRAINED, UNDERRUN and OVERRUN. */
struct audiod_event {
	struct audiod_header header;
	uint32_t count;
	uint32_t reserved;
};

/* The longest message (ENDPOINT_ADD and ENDPOINT_INFO are 128 bytes; room to grow, N18). */
#define AUDIOD_MESSAGE_MAX	256U

/* The page at the start of every stream's shared memory. */
#define AUDIOD_SHM_MAGIC	0x44445541U	/* "AUDD" */
#define AUDIOD_SHM_HEADER	4096U

#define AUDIOD_STATE_STOPPED	0U
#define AUDIOD_STATE_RUNNING	1U
#define AUDIOD_STATE_DRAINING	2U

struct audiod_shm_header {
	uint32_t magic;
	uint32_t version;
	uint32_t format;
	uint32_t channels;
	uint32_t rate;
	uint32_t frame_bytes;
	uint32_t capacity_frames;
	uint32_t period_frames;

	/*
	 * Each writer's value on its own cache line.  A position is read and
	 * written only through audiod_position_load() and _store(): where an
	 * 8-byte atomic is not lock-free (i386), its sequence word makes the
	 * two halves read as one value.
	 */
	_Alignas(64) uint64_t write_position;
	uint32_t write_sequence;
	_Alignas(64) uint64_t read_position;
	uint32_t read_sequence;
	_Alignas(64) uint64_t played_position;
	int64_t played_time_ns;
	uint32_t played_sequence;
	uint32_t underruns;
	uint32_t overruns;
	uint32_t state;
};

#if defined(__GCC_ATOMIC_LLONG_LOCK_FREE) && __GCC_ATOMIC_LLONG_LOCK_FREE == 2
#define AUDIOD_ATOMIC64 1
#else
#define AUDIOD_ATOMIC64 0
#endif

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define AUDIOD_LOW_WORD 0
#else
#define AUDIOD_LOW_WORD 1
#endif

/*
 * How many times a reader looks again at a position its writer, in another
 * process, is in the middle of writing.  A writer that died there leaves the
 * sequence odd, and the reader must not wait for it forever.
 */
#define AUDIOD_POSITION_RETRIES 1000U

/* Reads the other side's position (acquire). */
static inline uint64_t
audiod_position_load(
	const uint64_t *position,
	const uint32_t *sequence)
{
#if AUDIOD_ATOMIC64
	(void)sequence;
	return __atomic_load_n(position, __ATOMIC_ACQUIRE);
#else
	const uint32_t *word;
	uint32_t before;
	uint32_t after;
	uint32_t low;
	uint32_t high;
	unsigned tries;

	/*
	 * Reads the halves until the writer was not in the middle of writing
	 * them.  A writer caught there needs the processor to finish, so the
	 * reader yields; a writer that died there leaves the sequence odd, and
	 * the last halves read stand.
	 */
	word = (const uint32_t *)(const void *)position;
	tries = 0U;
	for (;;) {
		before = __atomic_load_n(sequence, __ATOMIC_ACQUIRE);
		low = __atomic_load_n(&word[AUDIOD_LOW_WORD], __ATOMIC_RELAXED);
		high = __atomic_load_n(&word[1 - AUDIOD_LOW_WORD], __ATOMIC_RELAXED);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		after = __atomic_load_n(sequence, __ATOMIC_RELAXED);
		tries++;
		if ((before & 1U) == 0U && before == after)
			break;
		if (tries >= AUDIOD_POSITION_RETRIES)
			break;
		(void)sched_yield();
	}

	/* Succeeded: the position. */
	return ((uint64_t)high << 32) | low;
#endif
}

/* Publishes this side's position (release); only its one writer calls it. */
static inline void
audiod_position_store(
	uint64_t *position,
	uint32_t *sequence,
	uint64_t value)
{
#if AUDIOD_ATOMIC64
	(void)sequence;
	__atomic_store_n(position, value, __ATOMIC_RELEASE);
#else
	uint32_t *word;
	uint32_t current;

	word = (uint32_t *)(void *)position;
	current = *sequence;
	__atomic_store_n(sequence, current + 1U, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&word[AUDIOD_LOW_WORD], (uint32_t)value, __ATOMIC_RELAXED);
	__atomic_store_n(&word[1 - AUDIOD_LOW_WORD], (uint32_t)(value >> 32), __ATOMIC_RELAXED);
	__atomic_store_n(sequence, current + 2U, __ATOMIC_RELEASE);
#endif
}

_Static_assert(sizeof(struct audiod_endpoint_add) == 128, "ENDPOINT_ADD is 128 bytes");
_Static_assert(sizeof(struct audiod_endpoint_info) == 128, "ENDPOINT_INFO is 128 bytes");
_Static_assert(sizeof(struct audiod_stream_create) == 40, "version 1's STREAM_CREATE is 40 bytes");
_Static_assert(sizeof(struct audiod_stream_create2) == 48, "version 2's STREAM_CREATE is 48 bytes");

#endif
