/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBMEDIA_SOUND_H
#define LIBMEDIA_SOUND_H
#include <stddef.h>
#include <stdint.h>

#define MEDIA_PCM_ROOM 16384U

/* Decoder-owned stereo source queue and rational resampling clock; close releases its kernel. */
struct media_pcm {
	float samples[MEDIA_PCM_ROOM * 2U];
	float *kernel;
	uint64_t base;
	uint64_t total;
	uint64_t cursor;
	uint64_t phase;
	uint64_t active_end;
	uint64_t history_start;
	uint32_t input_rate;
	uint32_t output_rate;
	unsigned radius;
	int64_t origin_us;
	int64_t before_us;
	int64_t end_us;
	int started;
	int active;
	int drained;
	int error;	/* Conversion failures are consumed by the owning decoder on its next receive. */
};

int media_pcm_init(struct media_pcm *sound, uint32_t rate, int64_t end_us);
int media_pcm_push(struct media_pcm *sound, const float *stereo, size_t frames, int64_t time_us);
int media_pcm_receive(struct media_pcm *sound, int64_t *time_us);
size_t media_pcm_read(struct media_pcm *sound, int16_t *samples, size_t capacity, uint32_t rate);
void media_pcm_trim(struct media_pcm *sound, int64_t before_us);
void media_pcm_reset(struct media_pcm *sound);
void media_pcm_close(struct media_pcm *sound);
#endif
