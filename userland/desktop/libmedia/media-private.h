/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What libmedia's parts share inside the library (ws177-p031): the table
 * of a decoding back end's operations (decoder.c calls them; avcodec.c is
 * one), the conversion of packets into what a decoder reads without
 * private data (bitstream.c), and the library's log.
 */

#ifndef LIBMEDIA_MEDIA_PRIVATE_H
#define LIBMEDIA_MEDIA_PRIVATE_H

#include "media-decoder.h"

#include <stddef.h>
#include <stdint.h>

/* The largest packet the bitstream conversion builds (mediafile's 64 MiB and the prefix). */
#define MEDIA_BITSTREAM_MAX	(65U * 1024U * 1024U)

/*
 * A decoding back end: whether it can work (load, and why not), a
 * decoder for a track (open answers MEDIA_PROBLEM_FORMAT for a codec it
 * does not take), the decoder's operations on its own state, and the
 * pictures it gives (its own objects) and their scaler.  decoder.c tries
 * the back ends in its table's order; the first that opens a track
 * decodes it.  The operations follow media-decoder.h's.
 */
struct media_decoder_ops {
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
};

/*
 * The conversion of a track's packets into what a decoder reads without
 * private data (bitstream.c): the codec, the size of the NAL units'
 * lengths (0: not length-prefixed), the bytes put before each key frame
 * (parameter sets or headers), the ADTS header's fields, and the output
 * being built.
 */
struct media_bitstream {
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
extern const struct media_decoder_ops media_avcodec_ops;

/* Original LC reconstruction, with optional GPU video kept in a separate native backend. */
extern const struct media_decoder_ops media_aac_ops;

/* The bitstream conversion (bitstream.c). */
int media_bitstream_open(struct media_bitstream *stream, unsigned codec, const unsigned char *private_data, size_t private_size);
int media_bitstream_convert(struct media_bitstream *stream, const unsigned char *data, size_t size, int keyframe, const unsigned char **result, size_t *result_size);
void media_bitstream_close(struct media_bitstream *stream);

/* A line of the library's log, to the caller's function (media_set_log). */
void media_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif
