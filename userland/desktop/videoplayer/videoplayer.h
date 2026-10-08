/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Video Player (WS122): opens a file, plays it, pauses, stops and seeks.
 * The container is read by the player's own reader (mediafile, WS122 p003);
 * the decoding is an add-in (codec.c, p004): FFmpeg's libavcodec, libavutil
 * and libswscale, when the system has them, opened with dlopen and called
 * through dlsym without FFmpeg's headers.  Without them the player says
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

#include "userland/desktop/mediafile/mediafile.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/* How many decoded pictures wait at most. */
#define VP_PICTURES		8U

/* The media's state, as the window shows it. */
#define VP_EMPTY		0U	/* nothing open */
#define VP_PLAYING		1U
#define VP_PAUSED		2U
#define VP_ENDED		3U	/* played to its end */

/* A decoded picture of the add-in (an AVFrame reference inside codec.c), and a track's decoder. */
struct vp_frame;
struct vp_decoder;

/* The largest packet the bitstream conversion builds (mediafile's 64 MiB and the prefix). */
#define VP_BITSTREAM_MAX	(65U * 1024U * 1024U)

/*
 * The conversion of a track's packets into what a decoder reads without
 * private data (bitstream.c): the codec, the size of the NAL units'
 * lengths (0: not length-prefixed), the bytes put before each key frame
 * (parameter sets or headers), the ADTS header's fields, and the output
 * being built.
 */
struct vp_bitstream {
	unsigned codec;
	unsigned length_size;
	unsigned char *prefix;
	size_t prefix_size;
	int adts;
	unsigned adts_profile;
	unsigned adts_rate_index;
	unsigned adts_channels;
	unsigned char *output;
	size_t output_size;
	size_t output_room;
};

/* Why the add-in or a decoder could not be used (vp_codec_load, vp_decoder_open). */
#define VP_CODEC_MISSING	1	/* libavcodec is not installed */
#define VP_CODEC_VERSION	2	/* a version of libavcodec the add-in does not know */
#define VP_CODEC_FORMAT		3	/* the file's codec has no decoder */

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

	/* Why the last open failed (VP_CODEC_*, 0 for another reason or none). */
	int codec_problem;

	/* The pictures decoded ahead (a ring of references), with their times (seconds). */
	struct vp_frame *pictures[VP_PICTURES];
	double picture_times[VP_PICTURES];
	unsigned picture_first;
	unsigned picture_count;

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
struct vp_frame *vp_media_take(struct vp_media *media, double clock, double *time, double *next);

/* The bitstream conversion (bitstream.c). */
int vp_bitstream_open(struct vp_bitstream *stream, unsigned codec, const unsigned char *private_data, size_t private_size);
int vp_bitstream_convert(struct vp_bitstream *stream, const unsigned char *data, size_t size, int keyframe, const unsigned char **result, size_t *result_size);
void vp_bitstream_close(struct vp_bitstream *stream);

/* The decoding add-in (codec.c). */
int vp_codec_load(void);
const char *vp_codec_reason(void);
int vp_decoder_open(const struct mf_track *track, struct vp_decoder **decoder);
const char *vp_decoder_name(const struct vp_decoder *decoder);
int vp_decoder_send(struct vp_decoder *decoder, const struct mf_packet *packet);
int vp_decoder_receive(struct vp_decoder *decoder, int64_t *time_us);
struct vp_frame *vp_decoder_picture(struct vp_decoder *decoder);
size_t vp_decoder_sound(struct vp_decoder *decoder, int16_t *samples, size_t capacity, uint32_t rate);
void vp_decoder_flush(struct vp_decoder *decoder);
void vp_decoder_close(struct vp_decoder *decoder);
void vp_frame_free(struct vp_frame **frame);
void vp_frame_size(const struct vp_frame *frame, int *width, int *height);
int vp_frame_scale(const struct vp_frame *frame, void **scaler, uint32_t *pixels, size_t stride, int width, int height);
void vp_scaler_free(void *scaler);

/* The log the tests read (main.c). */
void vp_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif
