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
 * decoder.c: standard Vulkan Video H.264 followed by original AAC-LC).  A picture is the back
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

/*
 * Why decoding could not start (media_codec_load, media_decoder_open).  The
 * values are above every errno value of the systems the desktop runs on,
 * because the same calls also answer an allocation's errno: with 1 to 6, a
 * zedBSD ENOMEM (4) read as DEVICE and its ENOENT (6) as BUSY (T1-529).
 * The values from 1007 on are media-encoder.h's.
 */
#define MEDIA_PROBLEM_MISSING	1001	/* libavcodec is not installed */
#define MEDIA_PROBLEM_VERSION	1002	/* a version of libavcodec the add-in does not know */
#define MEDIA_PROBLEM_FORMAT	1003	/* the file's codec has no decoder */
#define MEDIA_PROBLEM_DEVICE	1004	/* no GPU with the required standard video capabilities */
#define MEDIA_PROBLEM_PROFILE	1005	/* a native codec's signalled profile or tools are unsupported */
#define MEDIA_PROBLEM_BUSY	1006	/* all hardware video contexts are occupied */

/*
 * The deblocking filter of a decoder that decodes on the CPU
 * (media_decoder_deblock).  EXACT is the filter of the standard, whose
 * pictures are the stream's own (the default).  FAST filters the
 * pictures that later pictures predict from exactly and leaves the others
 * unfiltered; OFF filters nothing at all.  Both are for a player that
 * falls behind: FAST changes only pictures nothing predicts from, so its
 * differences do not spread (and it saves nothing on a stream whose every
 * picture is a reference), while OFF saves more but its differences
 * spread to the pictures predicted from them until the next key frame.
 */
#define MEDIA_DEBLOCK_EXACT	0U
#define MEDIA_DEBLOCK_FAST	1U
#define MEDIA_DEBLOCK_OFF	2U

/* A picture's colour matrix (struct media_gpu_picture's colour). */
#define MEDIA_COLOUR_601 0U
#define MEDIA_COLOUR_709 1U
#define MEDIA_COLOUR_2020 2U

struct media_decoder;
struct media_frame;
struct media_scaler;

/*
 * A decoded picture the GPU holds, for a program that draws it with its own
 * Vulkan device (ws236-p011): an NV12 image of the decoding context, whose
 * memory the program imports and whose planes it samples as R8 and RG8.
 *
 * identity names the decoding context and changes when a new one is made
 * (a new stream or sequence); slot names the image in it.  fd is the
 * image memory's VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT handle, owned
 * by the picture: a program imports a dup of it once for each (identity,
 * slot), as an NV12 image of image_width by image_height created with
 * VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT and VK_IMAGE_CREATE_EXTENDED_USAGE_BIT
 * and sampled usage, into memory of memory_bytes and memory_type.  The
 * picture is the window x, y, width, height of it; colour is MEDIA_COLOUR_*
 * of picture.h and full_range its range.  The samples hold the decoder's
 * values as they are.  The image keeps the picture until the frame is freed;
 * the program frees it only after its GPU work that samples it is done.
 */
struct media_gpu_picture {
	uint64_t identity;
	unsigned slot;
	int fd;
	uint64_t memory_bytes;
	uint32_t memory_type;
	uint32_t image_width;
	uint32_t image_height;
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	unsigned colour;
	int full_range;
};

/* Native availability is unconditional; per-track open reports device and profile admission. */
int media_codec_load(void);
const char *media_codec_reason(void);

/*
 * Asks for pictures the GPU holds (struct media_gpu_picture) from the
 * decoders opened from now on, where the device can give them; hold is the
 * number of pictures the program keeps at once at most (taken and not yet
 * freed), zero for pictures in the CPU's memory as before.  A picture the
 * GPU holds cannot be scaled by media_frame_scale.
 */
void media_decoder_gpu_pictures(unsigned hold);

/* A track's decoder. */
int media_decoder_open(const struct media_track *track, struct media_decoder **decoder);
const char *media_decoder_name(const struct media_decoder *decoder);
int media_decoder_send(struct media_decoder *decoder, const struct media_packet *packet);
int media_decoder_receive(struct media_decoder *decoder, int64_t *time_us);
struct media_frame *media_decoder_picture(struct media_decoder *decoder);
size_t media_decoder_sound(struct media_decoder *decoder, int16_t *samples, size_t capacity, uint32_t rate);
const char *media_decoder_backend(const struct media_decoder *decoder);
int media_decoder_trim(struct media_decoder *decoder, int64_t before_us);
int64_t media_decoder_frame_us(const struct media_decoder *decoder);
void media_decoder_flush(struct media_decoder *decoder);

/*
 * Chooses a decoder's deblocking filter (MEDIA_DEBLOCK_*), from the next
 * picture it decodes on.  The choice is kept for a back end that takes
 * over the track.  Returns 0; EINVAL for an unknown mode (nothing
 * changes); ENOTSUP when the back end decoding now has no such choice (a
 * GPU's decoder, which filters exactly, or a sound decoder).
 */
int media_decoder_deblock(struct media_decoder *decoder, unsigned mode);
void media_decoder_close(struct media_decoder *decoder);

/* A picture taken from a decoder, and the scaler that draws it. */
void media_frame_free(struct media_frame **frame);
void media_frame_aspect(const struct media_frame *frame, int *num, int *den);
void media_frame_size(const struct media_frame *frame, int *width, int *height);
int media_frame_scale(const struct media_frame *frame, struct media_scaler **scaler, uint32_t *pixels, size_t stride, int width, int height);
void media_scaler_free(struct media_scaler *scaler);

/* Describes a picture the GPU holds: 0, or ENOTSUP for a picture in the CPU's memory. */
int media_frame_gpu(const struct media_frame *frame, struct media_gpu_picture *gpu);

#endif
