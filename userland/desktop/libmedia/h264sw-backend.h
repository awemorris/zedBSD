/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libmedia's back end for the software H.264 decoder: the operations of
 * struct media_decoder_ops (media-private.h) over h264sw-decoder.h.
 *
 * Its place in decoder.c's table of back ends is right after the Vulkan
 * Video one, and before the libavcodec one: a track whose Vulkan Video
 * decoder cannot open (no device, an unsupported profile, all contexts
 * busy) is tried here, and a track this decoder does not take goes on to
 * libavcodec.
 *
 * The operations:
 *
 * open: takes MEDIA_CODEC_H264 only (else MEDIA_PROBLEM_FORMAT).  Opens
 *   the bitstream conversion (media_bitstream_open), reads the parameter
 *   sets of the codec private data (the converted prefix, split with
 *   h264sw_nal_find and given to h264sw_decoder_nal) and answers
 *   MEDIA_PROBLEM_PROFILE when one is refused as unsupported (ENOTSUP):
 *   interlace, 4:2:2, 4:4:4, 4:0:0, more than 8 bits, slice groups and
 *   the other profiles go to the next back end.  With no parameter sets
 *   in the private data, open succeeds and the first packet decides.
 *   Remembers the track's decode_order_times, aspect ratio, range and
 *   colour.
 * send: converts the packet (media_bitstream_convert), gives each NAL
 *   unit to h264sw_decoder_nal with the packet's time, then
 *   h264sw_decoder_end_access_unit (a packet is one access unit), and
 *   moves the output frames to its own queue of pictures.  A NULL packet
 *   starts the drain (h264sw_decoder_flush).  Returns EAGAIN while
 *   pictures wait to be received (the caller receives first, as with
 *   the Vulkan Video back end).  Returns ENOTSUP when the stream turns
 *   out to be outside the decoder's range (the decoder layer then
 *   switches back ends, below).  A stream error (EINVAL) is not
 *   returned: the decoder has concealed it and goes on.
 * receive: hands out the next picture in output order: > 0 with its
 *   time, 0 when the back end needs a packet, < 0 for a lasting error.
 *   The picture is a media_picture (picture.h) from a pool of the
 *   sequence's cropped size, into which the frame's samples are copied
 *   (luma, and Cb and Cr interleaved into the chroma plane of the
 *   pool's layout), with the frame's aspect ratio (the SPS's when it
 *   has one, else the track's) and colour (matrix_coefficients 1 is 709,
 *   9 and 10 are 2020, others 601; with no VUI the track's, else 601
 *   for sizes up to 576 lines and 709 above, as vkvideo.c chooses).
 *   With decode_order_times, the times of the packets are in decoding
 *   order: each output picture takes the smallest time among the
 *   pictures decoded and not yet output (the frames keep the times of
 *   their packets, so the back end keeps that multiset).
 * flush: h264sw_decoder_reset and the queue of pictures emptied.
 * close: everything freed; pictures already handed out stay valid
 *   (their pool lives until its last picture is released).
 * The pictures' operations are picture.h's (media_picture_unref,
 *   media_picture_size, media_picture_scale, media_picture_scaler_free,
 *   media_picture_aspect); picture_gpu is NULL (the pictures are in the
 *   CPU's memory).
 *
 * The decoder layer (decoder.c) around the back ends keeps the track
 * (with a copy of its private data), the index of the back end in use
 * and the number of packets it accepted, and switches back ends when
 * one refuses the stream after opening:
 *
 * - A refusal at open (MEDIA_PROBLEM_DEVICE, _PROFILE or _BUSY) moves on
 *   to the next back end of the table; the first problem is returned
 *   when none opens.
 * - A refusal at send (ENOTSUP, ENODEV or EBUSY) before any packet was
 *   accepted: the back end is closed, the next one opened with the kept
 *   track, and the same packet sent to it.  Nothing was output and no
 *   reference kept, so the times are those of the packets as sent.
 * - A refusal at send after packets were accepted: the old back end is
 *   drained (a NULL packet, then its pictures received, each with its own
 *   time) and closed; the next back end is opened, and packets are
 *   dropped until one marked as a key frame (the container's sync sample:
 *   an IDR picture or a recovery point), which is sent first.  No
 *   reference crosses the switch.  The number of dropped packets is
 *   counted and written to the log (media_log); playback goes on with a
 *   gap.  Only when the stream ends before a key frame arrives does the
 *   decoder end with the error.
 */

#ifndef LIBMEDIA_H264SW_BACKEND_H
#define LIBMEDIA_H264SW_BACKEND_H

#include "media-private.h"

/* The operations of the software H.264 back end. */
extern const struct media_decoder_ops media_h264sw_ops;

#endif
