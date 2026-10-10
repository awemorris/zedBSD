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

#ifndef MEDIA_APP_DECODER_H
#define MEDIA_APP_DECODER_H

#include "userland/desktop/libmedia/media-decoder.h"

#include <stddef.h>
#include <stdint.h>

/* Why decoding could not start (app_codec_load, app_decoder_open). */

struct app_decoder;
struct app_frame;
struct app_scaler;

/* The software decoding add-in, loaded once (0, or MEDIA_PROBLEM_MISSING or _VERSION, with a reason). */
int app_codec_load(void);
const char *app_codec_reason(void);

/* A track's decoder. */
int app_decoder_open(const struct media_track *track, struct app_decoder **decoder);
const char *app_decoder_name(const struct app_decoder *decoder);
int app_decoder_send(struct app_decoder *decoder, const struct media_packet *packet);
int app_decoder_receive(struct app_decoder *decoder, int64_t *time_us);
struct app_frame *app_decoder_picture(struct app_decoder *decoder);
size_t app_decoder_sound(struct app_decoder *decoder, int16_t *samples, size_t capacity, uint32_t rate);
const char *app_decoder_backend(const struct app_decoder *decoder);
int app_decoder_trim(struct app_decoder *decoder, int64_t before_us);
int64_t app_decoder_frame_us(const struct app_decoder *decoder);
void app_decoder_flush(struct app_decoder *decoder);
void app_decoder_close(struct app_decoder *decoder);

/* A picture taken from a decoder, and the scaler that draws it. */
void app_frame_free(struct app_frame **frame);
void app_frame_aspect(const struct app_frame *frame, int *num, int *den);
void app_frame_size(const struct app_frame *frame, int *width, int *height);
int app_frame_scale(const struct app_frame *frame, struct app_scaler **scaler, uint32_t *pixels, size_t stride, int width, int height);
void app_scaler_free(struct app_scaler *scaler);

#endif
