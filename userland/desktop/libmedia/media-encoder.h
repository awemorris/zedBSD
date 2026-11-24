/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libmedia's video encoding: pictures made into H.264 or H.265 access
 * units by the first of libmedia's encoding back ends that takes the whole
 * configuration (standard Vulkan Video hardware first, then original
 * software).  An encoder never changes back end after it opened.
 *
 * An encoder is configured once (media_encoder_create), is sent pictures
 * in display order (media_encoder_send for a picture in CPU memory,
 * media_encoder_send_slot for one of the encoder's GPU images) and gives
 * packets, one access unit each in decoding order (media_encoder_receive).
 * A NULL picture drains it.
 *
 * Every call answers 0 for success, an errno value, or one of the
 * MEDIA_PROBLEM_* values (media-decoder.h's and the two below, all above
 * every errno value).  The end of a drain is MEDIA_ENCODER_DRAINED, which
 * is not a problem.  No call answers a positive value below them that is
 * not an errno value.
 *
 * One encoder is used by one thread at a time; encoders and decoders are
 * independent of each other.  media_encoder_query and media_encoder_create
 * ask the devices (tens of milliseconds).  media_encoder_send and
 * media_encoder_send_slot may wait for one picture's encode on devices
 * whose submission runs the work (some tens of milliseconds); a program
 * whose frame thread must not wait sends and receives from a thread of
 * its own.  media_encoder_receive without MEDIA_RECEIVE_NOWAIT and
 * media_encoder_close wait for the device, at most five seconds.
 *
 * The library's internal API, for the desktop's programs; not a part of
 * any SDK.
 */

#ifndef LIBMEDIA_MEDIA_ENCODER_H
#define LIBMEDIA_MEDIA_ENCODER_H

#include "userland/desktop/libmedia/media.h"
#include "userland/desktop/libmedia/media-decoder.h"
#include "userland/desktop/mediafile/mediafile.h"

#include <stddef.h>
#include <stdint.h>

/* The profiles of config.profile and of the bits of info.profiles (the software encoders' numbers). */
#define MEDIA_ENCODER_PROFILE_DEFAULT			0U
#define MEDIA_ENCODER_PROFILE_H264_CONSTRAINED_BASELINE	1U
#define MEDIA_ENCODER_PROFILE_H264_MAIN			2U
#define MEDIA_ENCODER_PROFILE_H264_HIGH			3U
#define MEDIA_ENCODER_PROFILE_H265_MAIN			1U

/*
 * The rate controls of config.rate_control (info.rate_controls has the bit
 * 1U << each one a back end offers).  DEFAULT is the back end's own
 * choice, which reads neither qp nor bitrate.  CQP keeps the quantizer qp,
 * CBR keeps bitrate, VBR keeps bitrate on average and max_bitrate at most.
 */
#define MEDIA_ENCODER_RATE_DEFAULT	0U
#define MEDIA_ENCODER_RATE_CQP		1U
#define MEDIA_ENCODER_RATE_CBR		2U
#define MEDIA_ENCODER_RATE_VBR		3U

/*
 * The forms a picture comes in (info.inputs has the bits a back end takes;
 * config.input names one).  NV12 and I420 are pictures in CPU memory: NV12
 * is Y then the interleaved CbCr plane, I420 is Y, Cb and Cr (software
 * back ends only).  A picture in CPU memory is for the tests and for
 * pictures that start in CPU memory (a camera's, a file's raw pictures);
 * a desktop program's pictures of every frame are written by its GPU into
 * the encoder's images (GPU_NV12, struct media_encoder_slot).
 */
#define MEDIA_ENCODER_INPUT_NV12	0x1U
#define MEDIA_ENCODER_INPUT_I420	0x2U
#define MEDIA_ENCODER_INPUT_GPU_NV12	0x4U

/* The kinds of back end (config.allow takes either or both, info.kind is one). */
#define MEDIA_ENCODER_HARDWARE	0x1U
#define MEDIA_ENCODER_SOFTWARE	0x2U

/*
 * What a stream is for (config.purpose).  RECORDING is a file: quality
 * comes first and pictures may wait in the encoder.  LOW_LATENCY is a
 * remote display or a call: one picture in flight and no B-frames.
 */
#define MEDIA_ENCODER_RECORDING		0U
#define MEDIA_ENCODER_LOW_LATENCY	1U

/*
 * How a packet is framed (config.output).  ANNEX_B puts a start code before
 * each NAL unit and the parameter sets before each key frame.
 * LENGTH_PREFIXED puts the 4-byte length before each NAL unit and keeps
 * the parameter sets in the codec data only (media_encoder_codec_data).
 */
#define MEDIA_ENCODER_ANNEX_B		0U
#define MEDIA_ENCODER_LENGTH_PREFIXED	1U

/* The bits of info.flags: RATE_CHANGE says media_encoder_set_rate works on an open encoder of the back end. */
#define MEDIA_ENCODER_RATE_CHANGE	0x1U

/*
 * The encoder's answers beyond media-decoder.h's, in the same series.
 * CONFIG: no allowed back end takes the configuration.  LOST: the device
 * stopped or did not answer, and the encoder answers LOST to every call
 * from then on (it can only be closed).  DRAINED: media_encoder_receive
 * has given the last packet of a drain.
 */
#define MEDIA_PROBLEM_CONFIG	1007
#define MEDIA_PROBLEM_LOST	1008
#define MEDIA_ENCODER_DRAINED	1009

/*
 * One back end on one device, as media_encoder_query lists it.
 *
 * backend names it and kind is MEDIA_ENCODER_HARDWARE or _SOFTWARE; codec
 * is the codec asked about, profiles has the bit 1U << profile of each
 * profile it makes.  The coded size it takes is min_width by min_height
 * to max_width by max_height, in steps of step.  rate_controls has the
 * bit 1U << rate control of each it keeps; max_bitrate is its highest bit
 * rate in bits per second (saturated to UINT32_MAX; 0 when it states
 * none); min_qp to max_qp is the quantizer range CQP takes.  max_b_frames
 * is the B-frames in a row and max_references the P references it makes
 * at most; quality_levels is at least 1.  inputs has the input forms it
 * takes (MEDIA_ENCODER_INPUT_*), flags MEDIA_ENCODER_RATE_CHANGE.
 * device_uuid and driver_uuid are the device's VkPhysicalDeviceIDProperties
 * values (all zero for software), which config.device_uuid and
 * config.driver_uuid name to ask for this device.
 */
struct media_encoder_info {
	char backend[32];
	unsigned kind;
	unsigned codec;
	unsigned profiles;
	uint32_t min_width;
	uint32_t min_height;
	uint32_t max_width;
	uint32_t max_height;
	uint32_t step;
	unsigned rate_controls;
	uint32_t max_bitrate;
	unsigned min_qp;
	unsigned max_qp;
	unsigned max_b_frames;
	unsigned max_references;
	unsigned quality_levels;
	unsigned inputs;
	unsigned flags;
	uint8_t device_uuid[16];
	uint8_t driver_uuid[16];
};

/*
 * What an encoder is asked for.
 *
 * codec is MEDIA_CODEC_H264 or MEDIA_CODEC_HEVC; profile a
 * MEDIA_ENCODER_PROFILE_* (DEFAULT: the back end's); level the codec's
 * level_idc (H.265: thirty times the level; 0: the lowest that fits).
 * width and height are the visible size (even, at least 16), rate_num /
 * rate_den the nominal frame rate (both at least 1, at most 300 frames a
 * second).
 *
 * rate_control is a MEDIA_ENCODER_RATE_*: CBR and VBR take bitrate (bits
 * per second, above 0), VBR max_bitrate (0: 3 / 2 of bitrate; else at
 * least bitrate), CQP qp (0..51: the P pictures' quantizer, the key
 * pictures' qp - 3).  key_interval is the frames from one key frame to the
 * next (0: two seconds' worth; UINT32_MAX: only the first and the forced
 * ones).  b_frames is the B-frames in a row (0 for LOW_LATENCY).  speed is
 * 0 for the back end's default, 1 fastest to 3 best quality.  purpose,
 * input and output are a MEDIA_ENCODER_RECORDING or _LOW_LATENCY, a
 * MEDIA_ENCODER_INPUT_* and a MEDIA_ENCODER_ANNEX_B or _LENGTH_PREFIXED.
 *
 * The colour the stream signals: colour_description 0 signals none (a
 * configuration cleared to zero signals none); 1 signals full_range (0 or
 * 1) and the ITU-T H.273 code points primaries, transfer and matrix
 * (reserved code points are refused).  A decoder's MEDIA_COLOUR_601, 709
 * and 2020 are the matrices 6, 1 and 9.
 *
 * allow is MEDIA_ENCODER_HARDWARE, _SOFTWARE or both (0: both).
 * device_uuid and driver_uuid name the device wanted (a row of
 * media_encoder_query; all zero for any, a non-zero pair leaves the
 * software back ends out).  threads is how a software back end makes its
 * threads (NULL: the system's threads, named media-encode) and
 * thread_count how many (0: its own choice).
 */
struct media_encoder_config {
	unsigned codec;
	unsigned profile;
	unsigned level;
	uint32_t width;
	uint32_t height;
	uint32_t rate_num;
	uint32_t rate_den;
	unsigned rate_control;
	uint32_t bitrate;
	uint32_t max_bitrate;
	unsigned qp;
	uint32_t key_interval;
	unsigned b_frames;
	unsigned speed;
	unsigned purpose;
	unsigned input;
	unsigned output;
	int colour_description;
	int full_range;
	unsigned primaries;
	unsigned transfer;
	unsigned matrix;
	unsigned allow;
	uint8_t device_uuid[16];
	uint8_t driver_uuid[16];
	const struct media_threads *threads;
	unsigned thread_count;
};

/*
 * A picture in CPU memory, in the configured input form: its planes, each
 * stride bytes a row (plane[2] is unused for NV12).  The encoder copies
 * the samples before media_encoder_send returns.
 */
struct media_encoder_picture {
	const uint8_t *plane[3];
	size_t stride[3];
};

/*
 * One of the encoder's GPU input images (MEDIA_ENCODER_INPUT_GPU_NV12), as
 * media_encoder_slots describes it.
 *
 * The image is a LINEAR NV12 image of image_width by image_height (the
 * coded size) on the device of device_uuid and driver_uuid.  Its memory
 * is the VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT handle fd, of
 * memory_bytes and memory_type, owned by the encoder until it closes.  A
 * program imports a dup of fd once for each slot (without a dedicated
 * allocation) into an NV12 image of the same size, LINEAR, made with
 * VkExternalMemoryImageCreateInfo (OPAQUE_FD),
 * VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, VK_IMAGE_CREATE_EXTENDED_USAGE_BIT
 * and the usage of usage (the VkImageUsageFlags the device allows for
 * writing it: transfer destination, and storage where the device offers
 * it).  Plane i starts at plane_offset[i] and has plane_pitch[i] bytes a
 * row.  The program writes every row of the coded size: the rows and
 * columns past the visible size repeat the last visible one.
 *
 * Ownership: a slot media_encoder_acquire gives is owned by the external
 * queue family in VK_IMAGE_LAYOUT_GENERAL.  The program acquires it from
 * VK_QUEUE_FAMILY_EXTERNAL, writes it, releases it to
 * VK_QUEUE_FAMILY_EXTERNAL in VK_IMAGE_LAYOUT_GENERAL, waits for that
 * work's fence, and only then sends it.  It destroys its image and memory
 * before the encoder closes.
 */
struct media_encoder_slot {
	unsigned index;
	int fd;
	uint64_t memory_bytes;
	uint32_t memory_type;
	uint32_t image_width;
	uint32_t image_height;
	uint32_t usage;
	uint64_t plane_offset[2];
	uint64_t plane_pitch[2];
	uint8_t device_uuid[16];
	uint8_t driver_uuid[16];
};

/*
 * A packet: one access unit in decoding order, framed as config.output
 * says, with the presentation and decoding times of its picture and
 * MEDIA_ENCODED_KEY for a key frame.  The bytes stay the encoder's until
 * the next media_encoder_receive or media_encoder_close.
 */
struct media_encoded {
	const uint8_t *data;
	size_t size;
	int64_t time_us;
	int64_t decode_time_us;
	unsigned flags;
};

/* media_encoded.flags: the packet is a key frame (an IDR picture). */
#define MEDIA_ENCODED_KEY	0x1U

/* The flags of media_encoder_send and _send_slot: make the picture a key frame; give the slot back without encoding it. */
#define MEDIA_ENCODE_FORCE_KEY		0x1U
#define MEDIA_ENCODE_DISCARD		0x2U
#define MEDIA_ENCODE_HIGH_QUALITY	0x4U

/* The flags of media_encoder_receive: do not wait for the device. */
#define MEDIA_RECEIVE_NOWAIT	0x1U

/*
 * The forms of the codec data for a container (media_encoder_codec_data).
 * PARAMETER_SETS is the VPS, SPS and PPS (H.264: SPS and PPS), each after a
 * start code.  HVCC is an HEVCDecoderConfigurationRecord and AVCC an
 * AVCDecoderConfigurationRecord (ISO/IEC 14496-15), for packets with
 * 4-byte lengths.
 */
#define MEDIA_CODEC_DATA_PARAMETER_SETS	1U
#define MEDIA_CODEC_DATA_HVCC		2U
#define MEDIA_CODEC_DATA_AVCC		3U

/*
 * What an encoder did since it opened: the pictures sent (not those
 * discarded), the packets taken, the pictures the device failed, the
 * pictures dropped behind a failure (in flight and predicted from a lost
 * picture: no packet), and the key frames made to recover.
 */
struct media_encoder_stats {
	uint64_t sent;
	uint64_t packets;
	uint64_t failed;
	uint64_t dropped;
	uint64_t recovered;
};

struct media_encoder;

/*
 * Lists the encoders of a codec: every back end on every device, into
 * infos (capacity rows at most; *count is the number of all rows).
 * Returns 0 (no device is no row); EINVAL for a codec that is not video;
 * ENOMEM.
 */
int media_encoder_query(unsigned codec, struct media_encoder_info *infos, unsigned capacity, unsigned *count);

/*
 * Opens an encoder: the first allowed back end that takes the whole
 * configuration.  Returns 0 and *encoder; EINVAL for a configuration out
 * of its ranges; MEDIA_PROBLEM_BUSY, _CONFIG or _DEVICE (in that order of
 * precedence) when no back end opened it; ENOMEM.
 */
int media_encoder_create(const struct media_encoder_config *config, struct media_encoder **encoder);

/* The name of the back end an encoder opened with. */
const char *media_encoder_backend(const struct media_encoder *encoder);

/*
 * Sends a picture in CPU memory to encode at time_us (after the last
 * one's), with MEDIA_ENCODE_FORCE_KEY to make it a key frame; a NULL
 * picture drains the encoder, after which nothing more is sent.  Returns
 * 0; EAGAIN when the pictures in flight are at their limit (nothing was
 * taken; receive first); EINVAL; MEDIA_PROBLEM_LOST; ENOMEM.
 */
int media_encoder_send(struct media_encoder *encoder, const struct media_encoder_picture *picture, int64_t time_us, unsigned flags);

/*
 * Describes the encoder's GPU input images (capacity at most; *count is
 * the number of all), made when it opened.  Returns 0; EINVAL for an
 * encoder without GPU input; MEDIA_PROBLEM_LOST.
 */
int media_encoder_slots(struct media_encoder *encoder, struct media_encoder_slot *slots, unsigned capacity, unsigned *count);

/* Takes a free GPU input image for the next picture.  Returns 0 and *index; EAGAIN when none is free; EINVAL; MEDIA_PROBLEM_LOST. */
int media_encoder_acquire(struct media_encoder *encoder, unsigned *index);

/*
 * Sends the acquired GPU input image index to encode at time_us (after the
 * last one's), with MEDIA_ENCODE_FORCE_KEY to make it a key frame, or
 * gives it back unencoded with MEDIA_ENCODE_DISCARD.  Returns 0; EAGAIN as
 * media_encoder_send; EINVAL for an index not acquired; MEDIA_PROBLEM_LOST.
 */
int media_encoder_send_slot(struct media_encoder *encoder, unsigned index, int64_t time_us, unsigned flags);

/*
 * Takes the next packet.  Without MEDIA_RECEIVE_NOWAIT it waits for the
 * oldest picture in flight.  Returns 0 and *packet; EAGAIN when no packet
 * is ready (more pictures are needed, or with NOWAIT the device is still
 * working); MEDIA_ENCODER_DRAINED after the last packet of a drain;
 * MEDIA_PROBLEM_LOST.  A picture the device failed gives no packet, and
 * the encoder makes the next picture a key frame.
 */
int media_encoder_receive(struct media_encoder *encoder, unsigned flags, struct media_encoded *packet);

/*
 * Changes the bit rate of an encoder opened with CBR or VBR, from the next
 * picture sent (VBR's max_bitrate 0 is 3 / 2 of bitrate; CBR ignores it).
 * Returns 0; ENOTSUP for a back end without MEDIA_ENCODER_RATE_CHANGE;
 * EINVAL for CQP or DEFAULT, a bitrate of 0 or above the back end's;
 * MEDIA_PROBLEM_LOST.
 */
int media_encoder_set_rate(struct media_encoder *encoder, uint32_t bitrate, uint32_t max_bitrate);

/*
 * Writes the codec data of a form (MEDIA_CODEC_DATA_*) into buffer and its
 * size into *length.  Returns 0; ENOBUFS when size is too small (*length
 * is the size needed); ENOTSUP for a form the codec has no such data in
 * (or AVCC, which is not made yet); EINVAL; MEDIA_PROBLEM_LOST.
 */
int media_encoder_codec_data(const struct media_encoder *encoder, unsigned form, uint8_t *buffer, size_t size, size_t *length);

/* Reads what an encoder did since it opened. */
void media_encoder_stats(const struct media_encoder *encoder, struct media_encoder_stats *stats);

/* Closes an encoder (NULL is ignored), waiting for its pictures in flight. */
void media_encoder_close(struct media_encoder *encoder);

#endif
