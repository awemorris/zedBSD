/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of audiod: clients and their streams (main.c), mixing and
 * conversion (mix.c), and the sound device (device.c).
 */

#ifndef AUDIOD_AUDIOD_H
#define AUDIOD_AUDIOD_H

#include "userland/base/audiod/protocol.h"

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIOD_CLIENT_INPUT	1024U
#define AUDIOD_BUFFER_MAX	(1U << 20)	/* frames in one ring */

struct audiod_client;

/* One stream: a ring in shared memory and how audiod reads or fills it. */
struct audiod_stream {
	struct audiod_stream *next;
	struct audiod_client *client;
	uint32_t id;
	uint32_t direction;
	struct audiod_shm_header *shm;
	uint8_t *ring;
	size_t shm_bytes;
	unsigned running;
	unsigned draining;
	unsigned broken;
	uint32_t drain_serial;
	uint64_t drain_target;		/* device progress, in device bytes */
	uint32_t volume_left;
	uint32_t volume_right;
	uint32_t muted;

	/* Resampling: a 32.32 position in source frames, and a step per output frame. */
	uint64_t phase;
	uint64_t step;

	/* Capture: the last device frame of the previous period. */
	int32_t carry[2];

	uint64_t last_request;		/* read (playback) or write (capture) position at the last REQUEST */
	uint32_t underruns_reported;
	uint32_t overruns_reported;
};

struct audiod_client {
	struct audiod_client *next;
	int fd;
	unsigned dead;
	unsigned subscribed;
	uint8_t input[AUDIOD_CLIENT_INPUT];
	size_t input_used;
	struct audiod_stream *streams;
};

/* The device side: format, period and progress. */
struct audiod_device {
	int dsp;			/* -1 without a device */
	int mixer;
	uint32_t format;
	uint32_t channels;
	uint32_t rate;
	uint32_t frame_bytes;
	uint32_t period_frames;
	uint32_t fragment_bytes;
	uint32_t fragment_count;
	uint8_t *map;			/* the mmap'd playback ring, or NULL to write */
	uint32_t map_bytes;
	uint32_t lead_bytes;
	uint64_t written;		/* device bytes mixed so far */
	uint64_t consumed;		/* device bytes the device has taken */
	int64_t timer_next_ns;		/* without a device: when the next period is due */
	int capture;			/* the device records */
	int capture_started;		/* recording has been started */
	uint8_t *scratch;		/* one period in device format */
	uint8_t *capture_raw;		/* one recorded period being gathered */
	uint32_t capture_fill;		/* bytes of it gathered so far */
	int64_t *mix;			/* one period, two channels */
	int32_t *capture_frames;	/* one captured period as stereo int32 */
};

extern struct audiod_client *audiod_clients;
extern struct audiod_device audiod_device;
extern sigjmp_buf audiod_bus_jump;
extern volatile int audiod_bus_armed;

/* main.c */
void audiod_send_event(struct audiod_stream *stream, uint32_t type, uint32_t count);
int64_t audiod_now_ns(void);

/* mix.c */
uint32_t audiod_frame_bytes(uint32_t format, uint32_t channels);
void audiod_mix_period(struct audiod_device *device, uint8_t *out);
void audiod_capture_period(struct audiod_device *device, const uint8_t *in);
void audiod_stream_rates(struct audiod_stream *stream, const struct audiod_device *device);

/* device.c */
int audiod_device_open(struct audiod_device *device);
int audiod_device_fd(const struct audiod_device *device, short *events);
int audiod_device_timeout_ms(const struct audiod_device *device);
void audiod_device_service(struct audiod_device *device, short revents);
void audiod_device_start_capture(struct audiod_device *device);
void audiod_device_set_volume(struct audiod_device *device, uint32_t left, uint32_t right, uint32_t muted);
void audiod_device_get_volume(struct audiod_device *device, uint32_t *left, uint32_t *right, uint32_t *muted);

#endif
