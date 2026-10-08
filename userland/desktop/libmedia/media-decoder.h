/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libmedia's decoding (ws177-p031): a track's packets (mediafile.h) made
 * into pictures and sound.  A decoder is opened by the first of libmedia's
 * decoding back ends that takes the track's codec (the table in
 * decoder.c: today the add-in that opens FFmpeg's libavcodec with dlopen,
 * avcodec.c; a GPU decoder goes before it later).  A picture is the back
 * end's own, scaled into the caller's pixels through a scaler kept
 * between calls.
 *
 * The library's internal API, for the desktop's programs; not a part of
 * any SDK.
 */

#ifndef LIBMEDIA_MEDIA_DECODER_H
#define LIBMEDIA_MEDIA_DECODER_H

#include "userland/desktop/mediafile/mediafile.h"

#include <stddef.h>
#include <stdint.h>

/* Why decoding could not start (media_codec_load, media_decoder_open). */
#define MEDIA_PROBLEM_MISSING	1	/* libavcodec is not installed */
#define MEDIA_PROBLEM_VERSION	2	/* a version of libavcodec the add-in does not know */
#define MEDIA_PROBLEM_FORMAT	3	/* the file's codec has no decoder */

struct media_decoder;
struct media_frame;
struct media_scaler;

/* The software decoding add-in, loaded once (0, or MEDIA_PROBLEM_MISSING or _VERSION, with a reason). */
int media_codec_load(void);
const char *media_codec_reason(void);

/* A track's decoder. */
int media_decoder_open(const struct media_track *track, struct media_decoder **decoder);
const char *media_decoder_name(const struct media_decoder *decoder);
int media_decoder_send(struct media_decoder *decoder, const struct media_packet *packet);
int media_decoder_receive(struct media_decoder *decoder, int64_t *time_us);
struct media_frame *media_decoder_picture(struct media_decoder *decoder);
size_t media_decoder_sound(struct media_decoder *decoder, int16_t *samples, size_t capacity, uint32_t rate);
void media_decoder_flush(struct media_decoder *decoder);
void media_decoder_close(struct media_decoder *decoder);

/* A picture taken from a decoder, and the scaler that draws it. */
void media_frame_free(struct media_frame **frame);
void media_frame_size(const struct media_frame *frame, int *width, int *height);
int media_frame_scale(const struct media_frame *frame, struct media_scaler **scaler, uint32_t *pixels, size_t stride, int width, int height);
void media_scaler_free(struct media_scaler *scaler);

#endif
