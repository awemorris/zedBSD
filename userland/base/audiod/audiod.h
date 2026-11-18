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
#include <sys/types.h>

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

	/*
	 * Version 2 (ws209-p002): the stream's role (AUDIOD_ROLE_*), and
	 * whether it is held because its output went away (design 1.2 G5:
	 * not read until the client stops and starts it again, or the
	 * compositor releases it).
	 */
	uint32_t role;
	unsigned held;

	/* The played position last published (ws209-p003, N4: what is heard now, never going back). */
	uint64_t played_last;
};

struct audiod_client {
	struct audiod_client *next;
	int fd;
	unsigned dead;
	unsigned subscribed;

	/* The protocol version agreed in HELLO (1 until then), and who the peer is (ws209-p002). */
	uint32_t version;
	uid_t uid;
	int uid_known;
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

	/*
	 * The device volume audiod applies itself when the device has no
	 * volume of its own (no /dev/mixer0, or one that refuses a set): 0 to
	 * 100 per channel and muted, with soft set while audiod applies it
	 * (ws100-p002).
	 */
	int soft;
	uint32_t soft_left;
	uint32_t soft_right;
	uint32_t soft_muted;

	/*
	 * The feedback sound (AUDIOD_FEEDBACK): its frames as stereo samples of
	 * the mix's scale, made once for the device's rate, how many there are,
	 * and the next one to play (feedback_length when it is not playing).
	 */
	int32_t *feedback;
	uint32_t feedback_length;
	uint32_t feedback_next;

	/*
	 * The feedback sound's way to the device, logged when AUDIOD_TIMING_LOG
	 * names a file (ws100-p008): when it was asked for (ns), the device
	 * byte its first frame was mixed at, and where it is: 0 idle, 1 asked
	 * for, 2 being mixed into the period in hand, 3 mixed and not played
	 * yet.
	 */
	int64_t feedback_asked_ns;
	uint64_t feedback_byte;
	unsigned feedback_step;
};

extern struct audiod_client *audiod_clients;
extern struct audiod_device audiod_device;
extern sigjmp_buf audiod_bus_jump;
extern volatile int audiod_bus_armed;

/* main.c */
void audiod_send_event(struct audiod_stream *stream, uint32_t type, uint32_t count);
int64_t audiod_now_ns(void);
void audiod_send(struct audiod_client *client, void *message, uint32_t length, int fd);
void audiod_send_result(struct audiod_client *client, const struct audiod_header *request, int error);
int audiod_shm_make(uint32_t format, uint32_t channels, uint32_t rate, uint32_t capacity, uint32_t period, int *fd,
    struct audiod_shm_header **shm, size_t *bytes);

/* route.c (ws209-p002): the endpoints and routes. */
void audiod_route_init(void);
int audiod_route_request(struct audiod_client *client, const uint8_t *bytes, uint32_t length);
void audiod_route_client_gone(struct audiod_client *client);
void audiod_route_tick(int64_t now_ns);
int audiod_route_timeout_ms(int64_t now_ns);
int audiod_route_media_to_device(void);
int audiod_route_output_exists(void);
int audiod_route_capture_allowed(const struct audiod_client *client);
int audiod_route_stream_plays(const struct audiod_stream *stream);
uint32_t audiod_route_stream_gain(const struct audiod_stream *stream);
unsigned audiod_route_fill_fragments(void);
double audiod_route_output_delay(void);
void audiod_flow_playback(struct audiod_device *device, int64_t *mix, uint32_t frames);
void audiod_flow_capture(struct audiod_device *device, const int32_t *frames_stereo, uint32_t frames);
void audiod_flow_input(struct audiod_device *device, int32_t *frames_stereo, uint32_t frames);
uint32_t audiod_soft_factor(uint32_t percent);
void audiod_route_stream_started(struct audiod_stream *stream);
int audiod_route_volume_get(uint32_t *left, uint32_t *right, uint32_t *muted);
int audiod_route_volume_set(uint32_t left, uint32_t right, uint32_t muted);
void audiod_route_send_volume(struct audiod_client *client);

/* mix.c */
uint32_t audiod_frame_bytes(uint32_t format, uint32_t channels);
void audiod_mix_period(struct audiod_device *device, uint8_t *out);
void audiod_capture_period(struct audiod_device *device, const uint8_t *in);
void audiod_stream_rates(struct audiod_stream *stream, const struct audiod_device *device);
int audiod_feedback_make(struct audiod_device *device);

/* device.c */
int audiod_device_open(struct audiod_device *device);
int audiod_device_fd(const struct audiod_device *device, short *events);
int audiod_device_timeout_ms(const struct audiod_device *device);
void audiod_device_service(struct audiod_device *device, short revents);
void audiod_timing(const char *format, ...);
void audiod_device_start_capture(struct audiod_device *device);
void audiod_device_set_volume(struct audiod_device *device, uint32_t left, uint32_t right, uint32_t muted);
void audiod_device_get_volume(struct audiod_device *device, uint32_t *left, uint32_t *right, uint32_t *muted);
void audiod_device_feedback(struct audiod_device *device);

#endif
