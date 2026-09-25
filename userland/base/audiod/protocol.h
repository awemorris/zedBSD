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

#include <stdint.h>

#define AUDIOD_SOCKET_PATH	"/run/audiod.sock"
#define AUDIOD_VERSION		1U

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

#define AUDIOD_MESSAGE_MAX	64U

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

	word = (const uint32_t *)(const void *)position;
	do {
		before = __atomic_load_n(sequence, __ATOMIC_ACQUIRE);
		low = __atomic_load_n(&word[AUDIOD_LOW_WORD], __ATOMIC_RELAXED);
		high = __atomic_load_n(&word[1 - AUDIOD_LOW_WORD], __ATOMIC_RELAXED);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		after = __atomic_load_n(sequence, __ATOMIC_RELAXED);
	} while ((before & 1U) != 0 || before != after);
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

#endif
