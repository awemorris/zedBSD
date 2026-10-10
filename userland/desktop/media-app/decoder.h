/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Video Player and Music's shared decoder adapter: public native media
 * is tried first. Unsupported codecs, profiles and devices may use the
 * application's optional libavcodec adapter, loaded with dlopen.
 * Pictures retain their backend ownership independently of decoder close.
 *
 * This application module is not part of libmedia or a public SDK.
 */

#ifndef MEDIA_APP_DECODER_H
#define MEDIA_APP_DECODER_H

#include "userland/desktop/libmedia/media-decoder.h"

#include <stddef.h>
#include <stdint.h>

struct app_decoder;
struct app_frame;
struct app_scaler;

/* Native startup needs no optional library; reasons describe fallback admission failures only. */
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
