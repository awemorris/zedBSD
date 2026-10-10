/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What Video Player and Music share inside their optional codec adapter:
 * software decoder operations, packet conversion for libavcodec, and
 * the application's diagnostics. Native reconstruction stays in libmedia.
 */

#ifndef MEDIA_APP_PRIVATE_H
#define MEDIA_APP_PRIVATE_H

#include "decoder.h"

#include <stddef.h>
#include <stdint.h>

/* The largest packet the bitstream conversion builds (mediafile's 64 MiB and the prefix). */
#define MEDIA_BITSTREAM_MAX (65U * 1024U * 1024U)

/*
 * A decoding back end: whether it can work (load, and why not), a
 * decoder for a track (open answers MEDIA_PROBLEM_FORMAT for a codec it
 * does not take), the decoder's operations on its own state, and the
 * pictures it gives (its own objects) and their scaler. Application
 * decoder.c first tries public native decoding, then these operations
 * when native admission reports an unsupported codec, profile or device.
 */
struct app_decoder_ops {
	const char *name;
	int (*load)(void);
	const char *(*reason)(void);
	int (*open)(const struct media_track *track, void **state);
	const char *(*decoder_name)(const void *state);
	int (*send)(void *state, const struct media_packet *packet);
	int (*receive)(void *state, int64_t *time_us);
	void *(*picture)(void *state);
	size_t (*sound)(void *state, int16_t *samples, size_t capacity, uint32_t rate);
	void (*flush)(void *state);
	void (*close)(void *state);
	void (*picture_free)(void *picture);
	void (*picture_size)(const void *picture, int *width, int *height);
	int (*picture_scale)(const void *picture, void **scaler, uint32_t *pixels, size_t stride, int width, int height);
	void (*scaler_free)(void *scaler);
	int (*trim)(void *state, int64_t before_us);
	int64_t (*frame_us)(const void *state);
	void (*picture_aspect)(const void *picture, int *num, int *den);
};

/*
 * The conversion of a track's packets into what a decoder reads without
 * private data (bitstream.c): the codec, the size of the NAL units'
 * lengths (0: not length-prefixed), the bytes put before each key frame
 * (parameter sets or headers), the ADTS header's fields, and the output
 * being built.
 */
struct app_bitstream {
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

/* The add-in that opens FFmpeg's libavcodec (avcodec.c). */
extern const struct app_decoder_ops app_avcodec_ops;

/* The bitstream conversion (bitstream.c). */
int app_bitstream_open(struct app_bitstream *stream, unsigned codec, const unsigned char *private_data, size_t private_size);
int app_bitstream_convert(struct app_bitstream *stream, const unsigned char *data, size_t size, int keyframe, const unsigned char **result, size_t *result_size);
void app_bitstream_close(struct app_bitstream *stream);

/* An application diagnostic about native selection or optional software loading. */
void app_codec_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif
