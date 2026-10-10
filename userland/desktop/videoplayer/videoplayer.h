/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Video Player (WS122): opens a file, plays it, pauses, stops and seeks.
 * The container is read and decoded by libmedia (ws177-p031): its reader
 * (mediafile, WS122 p003) and its decoders (the add-in that opens FFmpeg's
 * libavcodec, libavutil and libswscale, when the system has them, with
 * dlopen and without FFmpeg's headers, p004).  Without them the player says
 * that playing needs libavcodec.  The sound goes to libkeiland's sound
 * stream (audio.c, WS191); the window, its menu and the controls are
 * libkeiland's.
 *
 * Two threads: the window's (main.c), which draws the picture whose time
 * has come and takes the input, and the media thread (media.c), which reads
 * and decodes ahead, keeps a few pictures and writes the sound.  The clock
 * is the sound's (the position the stream has played) when there is sound,
 * otherwise the monotonic clock.
 */

#ifndef VIDEOPLAYER_VIDEOPLAYER_H
#define VIDEOPLAYER_VIDEOPLAYER_H

#include "userland/desktop/libmedia/media.h"
#include "userland/desktop/media-app/decoder.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/* How many decoded pictures wait at most. */
#define VP_PICTURES		8U

/* The media's state, as the window shows it. */
#define VP_EMPTY		0U	/* nothing open */
#define VP_PLAYING		1U
#define VP_PAUSED		2U
#define VP_FAILED        4U	/* decoding stopped with an error */
#define VP_ENDED		3U	/* played to its end */

struct kl_audio_stream;

/*
 * The player's playback stream (audio.c): libkeiland's sound stream
 * (kl_audio_stream_*, WS191), its format, and whether it was made and
 * runs.  created and rate are read by the players' clocks; running is the
 * state the last start or stop left.
 */
struct vp_audio {
	struct kl_audio_stream *stream;
	uint32_t rate;
	uint32_t channels;
	uint32_t capacity;
	int created;
	int running;
};

/*
 * The media (media.c): the file, what is known of it, the pictures decoded
 * ahead, the clock's anchors and the requests to the media thread.  The
 * lock covers every field after it; the condition wakes the thread when a
 * picture is taken or a request comes.
 */
struct vp_media {
	pthread_t thread;
	int thread_started;
	struct vp_audio *audio;

	pthread_mutex_t lock;
	pthread_cond_t wake;

	/* What is open: the path, the picture's size, the length (seconds), and whether there is sound. */
	char path[1024];
	int width;
	int height;
	double duration;
	int has_audio;
	int error;

	/* The state, and the end of the file reached by the reader. */
	unsigned state;
	int eof;

	/* Why the last open failed (MEDIA_PROBLEM_*, 0 for another reason or none). */
	int codec_problem;
	int failure;

	/* The pictures decoded ahead (a ring of references), with their times (seconds). */
	struct app_frame *pictures[VP_PICTURES];
	double picture_times[VP_PICTURES];
	unsigned picture_first;
	unsigned picture_count;
	unsigned late;	/* Due pictures superseded before presentation; the window owns this count. */

	/* The clock: the time at an anchor, and the anchor (the stream's position heard, or the monotonic time). */
	double clock_time;
	uint64_t clock_frames;
	uint64_t clock_us;

	/* Requests: a seek (to a time), and the end of the thread. */
	int seek_wanted;
	double seek_to;
	int quit;
};

/* The sound (audio.c). */
int vp_audio_open(struct vp_audio *audio);
void vp_audio_close(struct vp_audio *audio);
int vp_audio_start(struct vp_audio *audio);
int vp_audio_stop(struct vp_audio *audio);
int vp_audio_flush(struct vp_audio *audio);
uint64_t vp_audio_read_position(const struct vp_audio *audio);
uint64_t vp_audio_write_position(const struct vp_audio *audio);
uint64_t vp_audio_clock_position(const struct vp_audio *audio);
void vp_audio_renew(struct vp_audio *audio);
int vp_audio_lost(struct vp_audio *audio);
size_t vp_audio_write(struct vp_audio *audio, const int16_t *samples, size_t frames);

/* The media (media.c). */
void vp_media_init(struct vp_media *media, struct vp_audio *audio);
int vp_media_open(struct vp_media *media, const char *path);
void vp_media_close(struct vp_media *media);
void vp_media_play(struct vp_media *media);
void vp_media_pause(struct vp_media *media);
void vp_media_seek(struct vp_media *media, double seconds);
double vp_media_clock(struct vp_media *media);
struct app_frame *vp_media_take(struct vp_media *media, double clock, double *time, double *next);

/* The log the tests read (main.c). */
void vp_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif
