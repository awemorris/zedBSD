/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What libmedia's parts share inside the library (ws177-p031): the table
 * of a native decoding backend's operations (decoder.c calls them), the conversion of packets into what a decoder reads without
 * private data (bitstream.c), the table of an encoding back end's operations and the encoder's shared parts (encoder.c),
 * and the library's log.
 */

#ifndef LIBMEDIA_MEDIA_PRIVATE_H
#define LIBMEDIA_MEDIA_PRIVATE_H

#include "media-decoder.h"
#include "media-encoder.h"

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
	void (*picture_aspect)(const void *picture, int *num, int *den);
	int (*picture_gpu)(const void *picture, struct media_gpu_picture *gpu);
	int (*deblock)(void *state, unsigned mode);
};

/* The pictures held at once a program asked the GPU's decoders for (media_decoder_gpu_pictures), zero for none. */
unsigned media_decoder_gpu_hold(void);

/* Opens a decoder for a track as media_decoder_open does, its back end's threads made through threads (NULL: the system's); the engine's path. */
int media_decoder_open_threads(const struct media_track *track, const struct media_threads *threads, struct media_decoder **decoder);

/* A retained native picture owns its backend operation table and CPU representation until media_frame_free. */
struct media_frame {
	const struct media_decoder_ops *ops;
	void *picture;
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

/* Original LC reconstruction, with optional GPU video kept in a separate native backend. */
extern const struct media_decoder_ops media_aac_ops;
extern const struct media_decoder_ops media_vkvideo_ops;
extern const struct media_decoder_ops media_vkvideo_h265_ops;

/* The bitstream conversion (bitstream.c). */
int media_bitstream_open(struct media_bitstream *stream, unsigned codec, const unsigned char *private_data, size_t private_size);
int media_bitstream_convert(struct media_bitstream *stream, const unsigned char *data, size_t size, int keyframe, const unsigned char **result, size_t *result_size);
void media_bitstream_close(struct media_bitstream *stream);

/*
 * An encoding back end (encoder.c calls it): its rows of
 * media_encoder_query, an encoder of its own for a configuration, and the
 * encoder's operations on its own state.  The operations follow
 * media-encoder.h's, with these differences.
 *
 * open gets the configuration encoder.c checked and resolved
 * (media_encoding_resolve): it takes the whole of it, or refuses with
 * MEDIA_PROBLEM_CONFIG (outside what it makes), _DEVICE (no such device,
 * or not the device of a non-zero config.device_uuid and driver_uuid) or
 * _BUSY (the device has no room now), leaving nothing behind; ENOMEM.
 *
 * receive gives one access unit in Annex B without the parameter sets,
 * with MEDIA_ENCODED_KEY for a key frame; encoder.c frames it.
 * parameter_sets gives the parameter sets in Annex B (the device's own
 * bytes), the back end's until close.  stats fills failed, dropped and
 * recovered (encoder.c counts sent and packets).  slots, acquire and
 * send_slot are NULL for a back end without GPU input, set_rate for one
 * without MEDIA_ENCODER_RATE_CHANGE.  Any answer may also be a negative
 * errno value, which encoder.c makes positive.
 */
struct media_encoder_ops {
	const char *name;
	unsigned kind;
	int (*query)(unsigned codec, struct media_encoder_info *infos, unsigned capacity, unsigned *count);
	int (*open)(const struct media_encoder_config *config, void **state);
	int (*send)(void *state, const struct media_encoder_picture *picture, int64_t time_us, unsigned flags);
	int (*slots)(void *state, struct media_encoder_slot *slots, unsigned capacity, unsigned *count);
	int (*acquire)(void *state, unsigned *index);
	int (*send_slot)(void *state, unsigned index, int64_t time_us, unsigned flags);
	int (*receive)(void *state, unsigned flags, struct media_encoded *packet);
	int (*set_rate)(void *state, uint32_t bitrate, uint32_t max_bitrate);
	int (*parameter_sets)(const void *state, const uint8_t **data, size_t *size);
	void (*stats)(const void *state, struct media_encoder_stats *stats);
	void (*close)(void *state);
};

/*
 * A back end's account of the failures of its pictures, which every
 * hardware back end keeps the same way (media_encoding_recovery_*).
 *
 * A picture the device failed gives no packet, and the next picture sent
 * is made a key frame to recover (key_pending).  The pictures in flight
 * behind a failure that predict from earlier pictures have lost their
 * references and give no packet either (broken, until a key frame comes
 * out).  key_failures counts the recovering key frames that failed in a
 * row: the third makes the encoder LOST.  The pictures dropped behind a
 * failure are not counted towards it, since one failure drops as many as
 * are in flight.
 */
struct media_encoding_recovery {
	int key_pending;
	int broken;
	unsigned key_failures;
	uint64_t failed;
	uint64_t dropped;
	uint64_t recovered;
};

/* The marks of a picture sent (media_encoding_recovery_sent): a key frame, and one made to recover. */
#define MEDIA_ENCODING_PICTURE_KEY	0x1U
#define MEDIA_ENCODING_PICTURE_RECOVERY	0x2U

/* What becomes of a picture done (media_encoding_recovery_done): its packet, no packet, or the encoder is lost. */
#define MEDIA_ENCODING_PACKET		0U
#define MEDIA_ENCODING_NO_PACKET	1U
#define MEDIA_ENCODING_LOST		2U

/* The recovering key frames that fail in a row before the encoder is lost. */
#define MEDIA_ENCODING_KEY_FAILURES_MAX	3U

/*
 * A software encoder's configuration as libmedia's software encoders take
 * it: the fields of their configurations of the same names and numbers,
 * format their picture format (MEDIA_ENCODING_FORMAT_*), and the threads.
 */
struct media_encoding_software {
	unsigned width;
	unsigned height;
	uint32_t rate_num;
	uint32_t rate_den;
	unsigned profile;
	unsigned level_idc;
	unsigned rate_control;
	uint32_t bitrate;
	uint32_t max_bitrate;
	unsigned qp;
	uint32_t key_interval;
	unsigned b_frames;
	unsigned speed;
	int full_range;
	unsigned primaries;
	unsigned transfer;
	unsigned matrix;
	unsigned format;
	const struct media_threads *threads;
	unsigned thread_count;
};

/* The software encoders' picture formats, their speed FAST, the colour code point that is not sent, and their longest key frame interval. */
#define MEDIA_ENCODING_FORMAT_NV12		1U
#define MEDIA_ENCODING_FORMAT_I420		2U
#define MEDIA_ENCODING_SPEED_FAST		1U
#define MEDIA_ENCODING_COLOUR_UNSPECIFIED	2U
#define MEDIA_ENCODING_MAX_KEY_INTERVAL		1073741824U

/*
 * The encoder's parts that the back ends and the tests share (encoder.c).
 * media_encoding_query and media_encoding_create are media_encoder_query
 * and media_encoder_create over a table of back ends that ends at NULL.
 */
int media_encoding_check(const struct media_encoder_config *config);
void media_encoding_resolve(const struct media_encoder_config *config, struct media_encoder_config *resolved);
int media_encoding_query(const struct media_encoder_ops *const *backends, unsigned codec, struct media_encoder_info *infos, unsigned capacity, unsigned *count);
int media_encoding_create(const struct media_encoder_ops *const *backends, const struct media_encoder_config *config, struct media_encoder **encoder);
unsigned media_encoding_recovery_sent(struct media_encoding_recovery *recovery, unsigned marks);
unsigned media_encoding_recovery_done(struct media_encoding_recovery *recovery, unsigned marks, int failed);
void media_encoding_software(const struct media_encoder_config *config, struct media_encoding_software *software);
int media_encoding_software_answer(int answer, int drained);

/* A line of the library's log, to the caller's function (media_set_log). */
void media_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif
